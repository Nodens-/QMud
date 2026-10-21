/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: SoundCacheUtils.h
 * Role: Source-keyed ownership, retained and idle reuse, accounting, and eviction ordering for the sound cache.
 */

#ifndef QMUD_SOUNDCACHEUTILS_H
#define QMUD_SOUNDCACHEUTILS_H

#include <QtGlobal>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

/**
 * @brief Owns cached backends by source and maintains retained reuse, idle reuse, and source-level LRU state.
 * @tparam Key Hashable source-identity type.
 * @tparam Backend Owned backend type.
 */
template <typename Key, typename Backend> class QMudSoundCache final
{
	public:
		using BackendId = quint64;

		/** @brief Result of adding one active backend. */
		struct InsertResult
		{
				Backend *backend{nullptr};
				bool     addedSource{false};
		};

		/** @brief One retained completed backend removed for immediate source-matched reuse. */
		struct RetainedResult
		{
				Backend *backend{nullptr};
				qint64   completedAtMsecs{0};
		};

		/** @brief Result of one bounded retained-to-idle maintenance step. */
		struct RetainedReleaseResult
		{
				qsizetype released{0};
				bool      expiredRemain{false};
		};

		/**
		 * @brief Adds one active backend and starts accounting for a new source when necessary.
		 * @param key Source identity.
		 * @param sourceBytes Loaded source-file or payload size.
		 * @param backendId Process-unique backend identity.
		 * @param backend Backend object to own.
		 * @param usedAtMsecs Positive monotonic use timestamp.
		 * @return Added backend and whether its source was new.
		 */
		[[nodiscard]] InsertResult insertActive(const Key &key, const qint64 sourceBytes,
		                                        const BackendId backendId, std::unique_ptr<Backend> backend,
		                                        const qint64 usedAtMsecs)
		{
			auto [it, inserted] = m_sources.try_emplace(key);
			if (inserted)
			{
				it->second.bytes = qMax<qint64>(0, sourceBytes);
				m_totalBytes += it->second.bytes;
			}
			Source &source = it->second;
			Q_ASSERT(source.bytes == qMax<qint64>(0, sourceBytes));
			removeIdleOrder(source);
			Backend *result                         = backend.get();
			const auto [backendIt, backendInserted] = source.backends.emplace(backendId, std::move(backend));
			Q_UNUSED(backendIt);
			Q_ASSERT(backendInserted);
			if (!backendInserted)
				return {};
			source.lastUsedMsecs = qMax(source.lastUsedMsecs, usedAtMsecs);
			++m_backendCount;
			return {result, inserted};
		}

		/**
		 * @brief Takes one idle backend for a source without examining unrelated sources.
		 * @param key Source identity.
		 * @param usedAtMsecs Positive monotonic use timestamp.
		 * @return Reusable backend, or `nullptr` when none is idle.
		 */
		[[nodiscard]] Backend *takeIdle(const Key &key, const qint64 usedAtMsecs)
		{
			auto it = m_sources.find(key);
			if (it == m_sources.end() || it->second.idleBackendIds.empty())
				return nullptr;
			Source &source = it->second;
			removeIdleOrder(source);
			const auto idleIt    = source.idleBackendIds.begin();
			const auto backendId = *idleIt;
			source.idleBackendIds.erase(idleIt);
			source.lastUsedMsecs = qMax(source.lastUsedMsecs, usedAtMsecs);
			const auto backendIt = source.backends.find(backendId);
			Q_ASSERT(backendIt != source.backends.end());
			return backendIt == source.backends.end() ? nullptr : backendIt->second.get();
		}

		/**
		 * @brief Takes one retained completed backend before consulting idle cache entries.
		 * @param key Source identity.
		 * @param usedAtMsecs Positive monotonic use timestamp.
		 * @return Reusable backend and its original completion timestamp, or an empty result.
		 */
		[[nodiscard]] RetainedResult takeRetained(const Key &key, const qint64 usedAtMsecs)
		{
			auto it = m_sources.find(key);
			if (it == m_sources.end() || it->second.retainedOrder.empty())
				return {};
			Source &source = it->second;
			removeIdleOrder(source);
			const auto retainedIt  = source.retainedOrder.begin();
			const auto backendId   = retainedIt->first;
			const auto orderIt     = retainedIt->second;
			const auto completedAt = orderIt->first;
			m_retainedBackends.erase(orderIt);
			source.retainedOrder.erase(retainedIt);
			source.lastUsedMsecs = qMax(source.lastUsedMsecs, usedAtMsecs);
			const auto backendIt = source.backends.find(backendId);
			Q_ASSERT(backendIt != source.backends.end());
			return backendIt == source.backends.end() ? RetainedResult{}
			                                          : RetainedResult{backendIt->second.get(), completedAt};
		}

		/**
		 * @brief Records use of an already assigned backend.
		 * @param key Source identity.
		 * @param usedAtMsecs Positive monotonic use timestamp.
		 */
		void noteUse(const Key &key, const qint64 usedAtMsecs)
		{
			auto it = m_sources.find(key);
			if (it == m_sources.end())
				return;
			it->second.lastUsedMsecs = qMax(it->second.lastUsedMsecs, usedAtMsecs);
		}

		/**
		 * @brief Marks one backend idle and makes a fully idle source eviction-eligible.
		 * @param key Source identity.
		 * @param backendId Backend becoming idle.
		 * @param usedAtMsecs Positive monotonic last-use timestamp.
		 */
		void markIdle(const Key &key, const BackendId backendId, const qint64 usedAtMsecs)
		{
			auto it = m_sources.find(key);
			if (it == m_sources.end())
				return;
			Source &source = it->second;
			if (!source.backends.contains(backendId))
				return;
			removeRetainedOrder(source, backendId);
			const bool inserted = source.idleBackendIds.insert(backendId).second;
			if (!inserted)
				return;
			source.lastUsedMsecs = qMax(source.lastUsedMsecs, usedAtMsecs);
			addIdleOrderIfEligible(key, source);
		}

		/**
		 * @brief Retains one naturally completed backend outside idle eviction during its grace period.
		 * @param key Source identity.
		 * @param backendId Completed backend identity.
		 * @param completedAtMsecs Positive monotonic completion timestamp.
		 */
		void markRetained(const Key &key, const BackendId backendId, const qint64 completedAtMsecs)
		{
			auto it = m_sources.find(key);
			if (it == m_sources.end() || !it->second.backends.contains(backendId))
				return;
			Source &source = it->second;
			removeIdleOrder(source);
			source.idleBackendIds.erase(backendId);
			removeRetainedOrder(source, backendId);
			source.retainedOrder.emplace(
			    backendId,
			    m_retainedBackends.emplace(completedAtMsecs, std::pair<Key, BackendId>{key, backendId}));
			source.lastUsedMsecs = qMax(source.lastUsedMsecs, completedAtMsecs);
		}

		/**
		 * @brief Releases a bounded number of expired retained backends into the idle cache.
		 * @param nowMsecs Current monotonic time.
		 * @param graceMsecs Required retention interval.
		 * @param maximumTransitions Maximum number of backends to release during this step.
		 * @return Released count and whether another expired backend remains.
		 */
		[[nodiscard]] RetainedReleaseResult releaseExpiredRetained(const qint64    nowMsecs,
		                                                           const qint64    graceMsecs,
		                                                           const qsizetype maximumTransitions)
		{
			RetainedReleaseResult result;
			const qsizetype       limit = qMax<qsizetype>(0, maximumTransitions);
			qsizetype             processed{0};
			while (processed < limit && !m_retainedBackends.empty())
			{
				++processed;
				const auto orderIt = m_retainedBackends.begin();
				if (!elapsedAtLeast(nowMsecs, orderIt->first, graceMsecs))
					break;
				const Key       key       = orderIt->second.first;
				const BackendId backendId = orderIt->second.second;
				auto            sourceIt  = m_sources.find(key);
				if (sourceIt != m_sources.end())
				{
					Source &source = sourceIt->second;
					source.retainedOrder.erase(backendId);
					source.idleBackendIds.insert(backendId);
					addIdleOrderIfEligible(key, source);
					++result.released;
				}
				m_retainedBackends.erase(orderIt);
			}
			result.expiredRemain = !m_retainedBackends.empty() &&
			                       elapsedAtLeast(nowMsecs, m_retainedBackends.begin()->first, graceMsecs);
			return result;
		}

		/**
		 * @brief Removes one backend from idle reuse while deferred destruction is pending.
		 * @param key Source identity.
		 * @param backendId Backend becoming unavailable.
		 */
		void markUnavailable(const Key &key, const BackendId backendId)
		{
			auto it = m_sources.find(key);
			if (it == m_sources.end() || !it->second.backends.contains(backendId))
				return;
			Source &source = it->second;
			removeIdleOrder(source);
			source.idleBackendIds.erase(backendId);
			removeRetainedOrder(source, backendId);
		}

		/**
		 * @brief Removes one backend directly by source and unique identity.
		 * @param key Source identity.
		 * @param backendId Backend identity.
		 * @param beforeDestroy Callback invoked before the backend is destroyed.
		 * @return `true` when a backend was removed.
		 */
		template <typename BeforeDestroy>
		bool eraseBackend(const Key &key, const BackendId backendId, BeforeDestroy beforeDestroy)
		{
			auto sourceIt = m_sources.find(key);
			if (sourceIt == m_sources.end())
				return false;
			Source &source    = sourceIt->second;
			auto    backendIt = source.backends.find(backendId);
			if (backendIt == source.backends.end())
				return false;
			removeIdleOrder(source);
			source.idleBackendIds.erase(backendId);
			removeRetainedOrder(source, backendId);
			beforeDestroy(*backendIt->second);
			source.backends.erase(backendIt);
			--m_backendCount;
			if (source.backends.empty())
			{
				m_totalBytes -= source.bytes;
				m_sources.erase(sourceIt);
			}
			else
			{
				addIdleOrderIfEligible(key, source);
			}
			return true;
		}

		/**
		 * @brief Evicts the least recently used fully idle source.
		 * @param beforeDestroy Callback invoked for every backend before destruction.
		 * @return Removed source identity, or an empty value when no source is fully idle.
		 */
		template <typename BeforeDestroy>
		[[nodiscard]] std::optional<Key> evictOldestIdle(BeforeDestroy beforeDestroy)
		{
			if (m_idleSources.empty())
				return std::nullopt;
			const auto orderIt  = m_idleSources.begin();
			const Key  key      = orderIt->second;
			auto       sourceIt = m_sources.find(key);
			Q_ASSERT(sourceIt != m_sources.end());
			if (sourceIt == m_sources.end())
			{
				m_idleSources.erase(orderIt);
				return std::nullopt;
			}
			Source &source = sourceIt->second;
			Q_ASSERT(source.idleOrder && *source.idleOrder == orderIt);
			Q_ASSERT(source.idleBackendIds.size() == source.backends.size());
			Q_ASSERT(source.retainedOrder.empty());
			for (auto &[id, backend] : source.backends)
			{
				Q_UNUSED(id);
				beforeDestroy(*backend);
			}
			m_backendCount -= source.backends.size();
			m_totalBytes -= source.bytes;
			m_idleSources.erase(orderIt);
			m_sources.erase(sourceIt);
			return key;
		}

		/**
		 * @brief Destroys every cached backend and resets all accounting.
		 * @param beforeDestroy Callback invoked for every backend before destruction.
		 */
		template <typename BeforeDestroy> void clear(BeforeDestroy beforeDestroy)
		{
			for (auto &[key, source] : m_sources)
			{
				Q_UNUSED(key);
				for (auto &[id, backend] : source.backends)
				{
					Q_UNUSED(id);
					beforeDestroy(*backend);
				}
			}
			m_sources.clear();
			m_idleSources.clear();
			m_retainedBackends.clear();
			m_totalBytes   = 0;
			m_backendCount = 0;
		}

		/**
		 * @brief Returns the oldest fully idle source timestamp.
		 * @return Positive monotonic timestamp, or zero when no source is fully idle.
		 */
		[[nodiscard]] qint64 oldestIdleLastUsedMsecs() const noexcept
		{
			return m_idleSources.empty() ? 0 : m_idleSources.begin()->first;
		}

		/**
		 * @brief Returns whether at least one source is fully idle and evictable.
		 * @return `true` when one source may be evicted.
		 */
		[[nodiscard]] bool hasIdleSource() const noexcept
		{
			return !m_idleSources.empty();
		}

		/**
		 * @brief Returns uniquely accounted source bytes.
		 * @return Total bytes across distinct cached sources.
		 */
		[[nodiscard]] qint64 totalBytes() const noexcept
		{
			return m_totalBytes;
		}

		/**
		 * @brief Returns the number of cached source identities.
		 * @return Number of distinct sources.
		 */
		[[nodiscard]] qsizetype sourceCount() const noexcept
		{
			return static_cast<qsizetype>(m_sources.size());
		}

		/**
		 * @brief Returns the number of owned backend instances.
		 * @return Number of backend objects.
		 */
		[[nodiscard]] qsizetype backendCount() const noexcept
		{
			return static_cast<qsizetype>(m_backendCount);
		}

		/**
		 * @brief Returns the number of backend instances for one source.
		 * @param key Source identity.
		 * @return Backend count, or zero when absent.
		 */
		[[nodiscard]] qsizetype backendCount(const Key &key) const
		{
			const auto it = m_sources.find(key);
			return it == m_sources.end() ? 0 : static_cast<qsizetype>(it->second.backends.size());
		}

		/**
		 * @brief Returns the number of idle backend instances for one source.
		 * @param key Source identity.
		 * @return Idle backend count, or zero when absent.
		 */
		[[nodiscard]] qsizetype idleBackendCount(const Key &key) const
		{
			const auto it = m_sources.find(key);
			return it == m_sources.end() ? 0 : static_cast<qsizetype>(it->second.idleBackendIds.size());
		}

		/**
		 * @brief Returns the number of completed backends still inside their retention grace.
		 * @param key Source identity.
		 * @return Retained backend count, or zero when absent.
		 */
		[[nodiscard]] qsizetype retainedBackendCount(const Key &key) const
		{
			const auto it = m_sources.find(key);
			return it == m_sources.end() ? 0 : static_cast<qsizetype>(it->second.retainedOrder.size());
		}

		/**
		 * @brief Checks whether a positive timestamp has reached a required age.
		 * @param nowMsecs Current monotonic time.
		 * @param sinceMsecs Start timestamp.
		 * @param durationMsecs Required duration.
		 * @return `true` when the duration has elapsed without a clock reversal.
		 */
		[[nodiscard]] static bool elapsedAtLeast(const qint64 nowMsecs, const qint64 sinceMsecs,
		                                         const qint64 durationMsecs) noexcept
		{
			return sinceMsecs > 0 && nowMsecs >= sinceMsecs &&
			       nowMsecs - sinceMsecs >= qMax<qint64>(0, durationMsecs);
		}

	private:
		struct KeyHasher
		{
				/**
				 * @brief Hashes a source key through its Qt hash overload.
				 * @param key Source key to hash.
				 * @return Hash value for the key.
				 */
				[[nodiscard]] std::size_t operator()(const Key &key) const noexcept
				{
					return qHash(key);
				}
		};

		using IdleOrder     = std::multimap<qint64, Key>;
		using RetainedOrder = std::multimap<qint64, std::pair<Key, BackendId>>;

		struct Source
		{
				qint64                                                          bytes{0};
				qint64                                                          lastUsedMsecs{0};
				std::unordered_map<BackendId, std::unique_ptr<Backend>>         backends;
				std::unordered_set<BackendId>                                   idleBackendIds;
				std::unordered_map<BackendId, typename RetainedOrder::iterator> retainedOrder;
				std::optional<typename IdleOrder::iterator>                     idleOrder;
		};

		/**
		 * @brief Removes a source from fully idle ordering when present.
		 * @param source Source state whose order entry is removed.
		 */
		void removeIdleOrder(Source &source)
		{
			if (!source.idleOrder)
				return;
			m_idleSources.erase(*source.idleOrder);
			source.idleOrder.reset();
		}

		/**
		 * @brief Removes one backend from retained-completion ordering when present.
		 * @param source Source containing the backend.
		 * @param backendId Backend identity to remove.
		 */
		void removeRetainedOrder(Source &source, const BackendId backendId)
		{
			const auto retainedIt = source.retainedOrder.find(backendId);
			if (retainedIt == source.retainedOrder.end())
				return;
			m_retainedBackends.erase(retainedIt->second);
			source.retainedOrder.erase(retainedIt);
		}

		/**
		 * @brief Adds a source to fully idle ordering when every backend is idle.
		 * @param key Source identity.
		 * @param source Source state to inspect and order.
		 */
		void addIdleOrderIfEligible(const Key &key, Source &source)
		{
			if (source.idleOrder || !source.retainedOrder.empty() || source.backends.empty() ||
			    source.idleBackendIds.size() != source.backends.size())
				return;
			source.idleOrder = m_idleSources.emplace(source.lastUsedMsecs, key);
		}

		std::unordered_map<Key, Source, KeyHasher> m_sources;
		IdleOrder                                  m_idleSources;
		RetainedOrder                              m_retainedBackends;
		qint64                                     m_totalBytes{0};
		std::size_t                                m_backendCount{0};
};

#endif // QMUD_SOUNDCACHEUTILS_H
