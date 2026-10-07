/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: LuaExecutor.h
 * Role: Lua callback execution abstraction used by runtime dispatch code so execution backends can be swapped
 * without changing callback call sites.
 */

#ifndef QMUD_LUAEXECUTOR_H
#define QMUD_LUAEXECUTOR_H

// ReSharper disable once CppUnusedIncludeDirective
#include "AnsiSgrParseUtils.h"
#include "Flags.h"
#include "MiniWindow.h"
#include "TelnetProcessor.h"

#include <QByteArray>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDateTime>
#include <QFlags>
// ReSharper disable once CppUnusedIncludeDirective
#include <QHash>
// ReSharper disable once CppUnusedIncludeDirective
#include <QList>
// ReSharper disable once CppUnusedIncludeDirective
#include <QMap>
#include <QMutex>
// ReSharper disable once CppUnusedIncludeDirective
#include <QPointer>
// ReSharper disable once CppUnusedIncludeDirective
#include <QRect>
// ReSharper disable once CppUnusedIncludeDirective
#include <QSet>
// ReSharper disable once CppUnusedIncludeDirective
#include <QSharedPointer>
#include <QString>
// ReSharper disable once CppUnusedIncludeDirective
#include <QStringList>
// ReSharper disable once CppUnusedIncludeDirective
#include <QVariant>
// ReSharper disable once CppUnusedIncludeDirective
#include <QVector>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

class LuaCallbackEngine;
class WorldRuntime;
class QObject;
struct LuaEngineObservedInitializationRequest;
struct MiniWindow;
#ifdef QMUD_ENABLE_LUA_SCRIPTING
struct lua_State;
#endif

/**
 * @brief Shared cancellation generation for callbacks derived from one world connection.
 *
 * Network and reload ingress retain this object through queued callback commands and mutation journals. Replacing
 * the runtime's current generation marks all ordinary older work cancelled in constant time. Modal suspensions
 * register exact cleanup actions, so invalidation performs work only for suspended callbacks and never scans callback
 * queues or suspended-command containers.
 */
class LuaCallbackConnectionCancellationState final
{
	public:
		using RegistrationId = quint64;

		/** @brief Creates an active connection generation. */
		explicit LuaCallbackConnectionCancellationState(const quint64 connectionEpoch)
		    : m_connectionEpoch(connectionEpoch)
		{
		}

		/** @return Connection epoch represented by this state. */
		[[nodiscard]] quint64 connectionEpoch() const noexcept
		{
			return m_connectionEpoch;
		}

		/** @return `true` after the owning connection generation was invalidated. */
		[[nodiscard]] bool isCancelled() const noexcept
		{
			return m_cancelled.load(std::memory_order_acquire);
		}

		/**
		 * @brief Registers exact cleanup for a suspended callback.
		 * @param cancellation Cleanup action to run when this generation is invalidated.
		 * @return Registration identity, or zero when the generation is already cancelled.
		 */
		[[nodiscard]] RegistrationId registerSuspensionCancellation(std::function<void()> cancellation)
		{
			if (!cancellation)
				return 0;
			QMutexLocker lock(&m_mutex);
			if (m_cancelled.load(std::memory_order_relaxed))
				return 0;
			RegistrationId registrationId = 0;
			do
			{
				registrationId = m_nextRegistrationId++;
			} while (registrationId == 0 || m_suspensionCancellations.contains(registrationId));
			m_suspensionCancellations.insert(registrationId, std::move(cancellation));
			return registrationId;
		}

		/** @brief Removes one completed suspension cleanup registration. */
		void unregisterSuspensionCancellation(const RegistrationId registrationId)
		{
			if (registrationId == 0)
				return;
			QMutexLocker lock(&m_mutex);
			m_suspensionCancellations.remove(registrationId);
		}

		/** @brief Invalidates the generation and runs every registered suspension cleanup exactly once. */
		void cancel()
		{
			QHash<RegistrationId, std::function<void()>> cancellations;
			{
				QMutexLocker lock(&m_mutex);
				if (m_cancelled.exchange(true, std::memory_order_acq_rel))
					return;
				cancellations.swap(m_suspensionCancellations);
			}
			for (const auto &cancellation : std::as_const(cancellations))
			{
				if (cancellation)
					cancellation();
			}
		}

	private:
		quint64                                      m_connectionEpoch{0};
		std::atomic_bool                             m_cancelled{false};
		QMutex                                       m_mutex;
		QHash<RegistrationId, std::function<void()>> m_suspensionCancellations;
		RegistrationId                               m_nextRegistrationId{1};
};

/**
 * @brief Shared cancellation generation for work accepted during one runtime lifetime phase.
 *
 * Destruction replaces the runtime's current generation before dispatching teardown callbacks. Queued callbacks,
 * worker requests, and deferred mutation journals retain their accepting generation, allowing all pre-destruction
 * work to be rejected in constant time while teardown work uses the replacement generation.
 */
class LuaCallbackRuntimeLifetimeState final
{
	public:
		/** @return `true` after the accepting runtime lifetime phase ended. */
		[[nodiscard]] bool isCancelled() const noexcept
		{
			return m_cancelled.load(std::memory_order_acquire);
		}

		/** @brief Ends this runtime lifetime phase. */
		void cancel() noexcept
		{
			m_cancelled.store(true, std::memory_order_release);
		}

	private:
		std::atomic_bool m_cancelled{false};
};

/**
 * @brief Shared terminal cancellation state for interactive world-script reloads.
 *
 * The primary world window ends this state only after its close is accepted. Queued reload commands and worker
 * requests retain the state, allowing reloads that have not started to be rejected in constant time without
 * changing connection or runtime-destruction semantics.
 */
class LuaScriptReloadCancellationState final
{
	public:
		/** @return `true` after the owning world session has closed. */
		[[nodiscard]] bool isCancelled() const noexcept
		{
			return m_cancelled.load(std::memory_order_acquire);
		}

		/** @brief Prevents queued, worker-pending, and future reloads from starting. */
		void cancel() noexcept
		{
			m_cancelled.store(true, std::memory_order_release);
		}

	private:
		std::atomic_bool m_cancelled{false};
};

/**
 * @brief Runtime-maintained placement state for callback output delayed past its capture point.
 *
 * Queued and suspended callbacks promote an output anchor into this state. A callback that already owns an output
 * stream aliases the state stored in that stream, avoiding another allocation when a nested callback or suspension
 * needs the same positional updates. Middle edits update the state through the cursor registry, while head eviction
 * advances one shared counter from which every surviving state derives its current index. Callback dispatch therefore
 * never has to search the output buffer, and ordinary capped-buffer appends never walk the cursor registry.
 */
struct LuaCallbackOutputAnchorState
{
		/**
		 * @brief Controls whether delayed output follows an active line's synchronous tail or its captured boundary.
		 */
		enum class PlacementMode : quint8
		{
			FollowActiveIncomingTail,
			FixedEventBoundary
		};

		struct Presentation
		{
				int  bufferIndex{0};
				bool activeIncoming{false};
				bool hidden{false};
		};

		explicit LuaCallbackOutputAnchorState(
		    const int initialBufferIndex = 0, const bool initiallyActiveIncoming = false,
		    const bool initiallyHidden = false, const quint64 promotedOutputStreamId = 0,
		    std::shared_ptr<const std::atomic<quint64>> headRemovalCount = {},
		    const PlacementMode placementMode = PlacementMode::FollowActiveIncomingTail) noexcept
		    : outputStreamId(promotedOutputStreamId), m_headRemovalCount(std::move(headRemovalCount)),
		      m_presentation(packPresentation(shiftedBufferIndex(initialBufferIndex), initialBufferIndex > 0,
		                                      initiallyActiveIncoming, initiallyHidden)),
		      m_placementMode(placementMode)
		{
		}

		[[nodiscard]] PlacementMode placementMode() const noexcept
		{
			return m_placementMode.load(std::memory_order_relaxed);
		}

		void setPlacementMode(const PlacementMode placementMode) noexcept
		{
			m_placementMode.store(placementMode, std::memory_order_relaxed);
		}

		[[nodiscard]] Presentation
		presentation(const std::memory_order order = std::memory_order_relaxed) const noexcept
		{
			return unpackPresentation(m_presentation.load(order), currentHeadRemovalCount());
		}

		void storePresentation(const int bufferIndex, const bool activeIncoming, const bool hidden,
		                       const std::memory_order order = std::memory_order_relaxed) noexcept
		{
			m_presentation.store(
			    packPresentation(shiftedBufferIndex(bufferIndex), bufferIndex > 0, activeIncoming, hidden),
			    order);
		}

		void storeBufferIndex(const int               bufferIndex,
		                      const std::memory_order order = std::memory_order_relaxed) noexcept
		{
			quint64 current = m_presentation.load(std::memory_order_relaxed);
			quint64 desired = 0;
			do
			{
				desired = (current & kPresentationStyleFlagsMask) | shiftedBufferIndex(bufferIndex) |
				          (bufferIndex > 0 ? kBufferIndexValidMask : quint64{0});
			} while (
			    !m_presentation.compare_exchange_weak(current, desired, order, std::memory_order_relaxed));
		}

		void clearPresentation(const std::memory_order order = std::memory_order_relaxed) noexcept
		{
			m_presentation.store(0, order);
		}

		/**
		 * @brief Attaches the runtime's cumulative head-removal counter before the state becomes observable again.
		 * @param headRemovalCount Counter shared by every promoted anchor owned by one runtime.
		 */
		void bindHeadRemovalCount(std::shared_ptr<const std::atomic<quint64>> headRemovalCount) noexcept
		{
			if (!headRemovalCount || m_headRemovalCount == headRemovalCount)
				return;
			Q_ASSERT(!m_headRemovalCount);
			if (m_headRemovalCount)
				return;
			const Presentation current = presentation();
			m_headRemovalCount         = std::move(headRemovalCount);
			storePresentation(current.bufferIndex, current.activeIncoming, current.hidden);
		}

		std::atomic<quint64> outputStreamId;

	private:
		static constexpr quint64 kBufferIndexMask            = (quint64{1} << 61) - 1;
		static constexpr quint64 kActiveIncomingMask         = quint64{1} << 61;
		static constexpr quint64 kHiddenMask                 = quint64{1} << 62;
		static constexpr quint64 kBufferIndexValidMask       = quint64{1} << 63;
		static constexpr quint64 kPresentationStyleFlagsMask = kActiveIncomingMask | kHiddenMask;

		[[nodiscard]] quint64    currentHeadRemovalCount() const noexcept
		{
			return m_headRemovalCount ? m_headRemovalCount->load(std::memory_order_relaxed) : 0;
		}

		[[nodiscard]] quint64 shiftedBufferIndex(const int bufferIndex) const noexcept
		{
			if (bufferIndex <= 0)
				return 0;
			return (currentHeadRemovalCount() + static_cast<quint64>(bufferIndex)) & kBufferIndexMask;
		}

		[[nodiscard]] static constexpr quint64 packPresentation(const quint64 shiftedIndex, const bool valid,
		                                                        const bool activeIncoming,
		                                                        const bool hidden) noexcept
		{
			return shiftedIndex | (valid ? kBufferIndexValidMask : quint64{0}) |
			       (activeIncoming ? kActiveIncomingMask : quint64{0}) | (hidden ? kHiddenMask : quint64{0});
		}

		[[nodiscard]] static constexpr Presentation
		unpackPresentation(const quint64 value, const quint64 headRemovalCount) noexcept
		{
			const quint64 shiftedIndex = value & kBufferIndexMask;
			const quint64 bufferIndex =
			    (shiftedIndex - (headRemovalCount & kBufferIndexMask)) & kBufferIndexMask;
			const int resolvedBufferIndex =
			    (value & kBufferIndexValidMask) != 0 && bufferIndex <= std::numeric_limits<int>::max()
			        ? static_cast<int>(bufferIndex)
			        : 0;
			return {resolvedBufferIndex, (value & kActiveIncomingMask) != 0, (value & kHiddenMask) != 0};
		}

		std::shared_ptr<const std::atomic<quint64>> m_headRemovalCount;
		std::atomic<quint64>                        m_presentation;
		std::atomic<PlacementMode>                  m_placementMode;
};

/**
 * @brief Immutable identity of one plugin-owned asynchronous API request.
 *
 * The plugin id locates the recipient while `engineInstanceId` proves that the recipient is still the exact Lua
 * engine that initiated the work. Keeping these values in one object prevents an id-only completion path from
 * delivering a pre-reload result to a same-id replacement.
 */
struct LuaPluginAsyncResultRequest
{
		QString            pluginId;
		quint64            engineInstanceId{0};
		quint64            requestId{0};

		[[nodiscard]] bool isValid() const noexcept
		{
			return !pluginId.isEmpty() && engineInstanceId != 0 && requestId != 0;
		}
};

/**
 * @brief Attribute/children payload used by callback-scope runtime snapshots.
 */
struct LuaCallbackAttributeChildrenSnapshot
{
		QMap<QString, QString> attributes;
		QMap<QString, QString> children;
		bool                   operator==(const LuaCallbackAttributeChildrenSnapshot &) const = default;
};

/**
 * @brief Attribute/content payload used by callback-scope runtime snapshots.
 */
struct LuaCallbackAttributeContentSnapshot
{
		QMap<QString, QString> attributes;
		QString                content;
		bool                   operator==(const LuaCallbackAttributeContentSnapshot &) const = default;
};

/**
 * @brief Trigger payload used by callback-scope rule-list snapshots.
 */
struct LuaCallbackTriggerSnapshot
{
		QMap<QString, QString> attributes;
		QMap<QString, QString> children;
		bool                   included{false};
		int                    matched{0};
		int                    invocationCount{0};
		int                    matchCount{0};
		int                    matchAttempts{0};
		qint64                 executionTimeNs{0};
		QString                lastMatchTarget;
		QStringList            lastMatchWildcards;
		QDateTime              lastMatched;
		quint64                runtimeId{0};
		quint64                runtimeLocationHandle{0};
		int                    executingScriptDepth{0};
		bool                   executingScript{false};
		bool                   operator==(const LuaCallbackTriggerSnapshot &) const = default;
};

/**
 * @brief Alias payload used by callback-scope rule-list snapshots.
 */
struct LuaCallbackAliasSnapshot
{
		QMap<QString, QString> attributes;
		QMap<QString, QString> children;
		bool                   included{false};
		int                    matched{0};
		int                    invocationCount{0};
		int                    matchCount{0};
		int                    matchAttempts{0};
		qint64                 executionTimeNs{0};
		QString                lastMatchTarget;
		QStringList            lastMatchWildcards;
		QDateTime              lastMatched;
		quint64                runtimeId{0};
		quint64                runtimeLocationHandle{0};
		int                    executingScriptDepth{0};
		bool                   executingScript{false};
		bool                   operator==(const LuaCallbackAliasSnapshot &) const = default;
};

/**
 * @brief Timer payload used by callback-scope rule-list snapshots.
 */
struct LuaCallbackTimerSnapshot
{
		QMap<QString, QString> attributes;
		QMap<QString, QString> children;
		bool                   included{false};
		QDateTime              lastFired;
		QDateTime              nextFireTime;
		int                    firedCount{0};
		int                    invocationCount{0};
		bool                   executingScript{false};
		quint64                runtimeId{0};
		quint64                runtimeLocationHandle{0};
		int                    executingScriptDepth{0};
		bool                   operator==(const LuaCallbackTimerSnapshot &) const = default;
};

/**
 * @brief Compact scalar runtime state shared by trigger, alias, and timer snapshot stores.
 */
struct LuaCallbackRuleRuntimeStateSnapshot
{
		quint64 runtimeId{0};
		quint64 runtimeLocationHandle{0};
		qint64  executionTimeNs{0};
		int     matched{0};
		int     invocationCount{0};
		int     matchCount{0};
		int     matchAttempts{0};
		int     firedCount{0};
		int     executingScriptDepth{0};
		bool    executingScript{false};
		bool    operator==(const LuaCallbackRuleRuntimeStateSnapshot &) const = default;
};

/**
 * @brief Infrequently changing non-definition state shared by callback rule snapshots.
 */
struct LuaCallbackRuleDetailStateSnapshot
{
		quint64     runtimeId{0};
		quint64     runtimeLocationHandle{0};
		QString     lastMatchTarget;
		QStringList lastMatchWildcards;
		QDateTime   lastMatched;
		QDateTime   lastFired;
		QDateTime   nextFireTime;
		bool        operator==(const LuaCallbackRuleDetailStateSnapshot &) const = default;
};

/**
 * @brief Extracts compact scalar state from a trigger row.
 * @param row Trigger snapshot row.
 * @return Compact scalar state.
 */
[[nodiscard]] inline LuaCallbackRuleRuntimeStateSnapshot
luaCallbackRuleRuntimeState(const LuaCallbackTriggerSnapshot &row)
{
	return {row.runtimeId,
	        row.runtimeLocationHandle,
	        row.executionTimeNs,
	        row.matched,
	        row.invocationCount,
	        row.matchCount,
	        row.matchAttempts,
	        0,
	        row.executingScriptDepth,
	        row.executingScript};
}

/**
 * @brief Extracts compact scalar state from an alias row.
 * @param row Alias snapshot row.
 * @return Compact scalar state.
 */
[[nodiscard]] inline LuaCallbackRuleRuntimeStateSnapshot
luaCallbackRuleRuntimeState(const LuaCallbackAliasSnapshot &row)
{
	return {row.runtimeId,
	        row.runtimeLocationHandle,
	        row.executionTimeNs,
	        row.matched,
	        row.invocationCount,
	        row.matchCount,
	        row.matchAttempts,
	        0,
	        row.executingScriptDepth,
	        row.executingScript};
}

/**
 * @brief Extracts compact scalar state from a timer row.
 * @param row Timer snapshot row.
 * @return Compact scalar state.
 */
[[nodiscard]] inline LuaCallbackRuleRuntimeStateSnapshot
luaCallbackRuleRuntimeState(const LuaCallbackTimerSnapshot &row)
{
	return {row.runtimeId,
	        row.runtimeLocationHandle,
	        0,
	        0,
	        row.invocationCount,
	        0,
	        0,
	        row.firedCount,
	        row.executingScriptDepth,
	        row.executingScript};
}

/**
 * @brief Extracts detail state from a trigger row.
 * @param row Trigger snapshot row.
 * @return Trigger detail state.
 */
[[nodiscard]] inline LuaCallbackRuleDetailStateSnapshot
luaCallbackRuleDetailState(const LuaCallbackTriggerSnapshot &row)
{
	return {row.runtimeId,
	        row.runtimeLocationHandle,
	        row.lastMatchTarget,
	        row.lastMatchWildcards,
	        row.lastMatched,
	        {},
	        {}};
}

/**
 * @brief Extracts detail state from an alias row.
 * @param row Alias snapshot row.
 * @return Alias detail state.
 */
[[nodiscard]] inline LuaCallbackRuleDetailStateSnapshot
luaCallbackRuleDetailState(const LuaCallbackAliasSnapshot &row)
{
	return {row.runtimeId,
	        row.runtimeLocationHandle,
	        row.lastMatchTarget,
	        row.lastMatchWildcards,
	        row.lastMatched,
	        {},
	        {}};
}

/**
 * @brief Extracts detail state from a timer row.
 * @param row Timer snapshot row.
 * @return Timer detail state.
 */
[[nodiscard]] inline LuaCallbackRuleDetailStateSnapshot
luaCallbackRuleDetailState(const LuaCallbackTimerSnapshot &row)
{
	return {row.runtimeId, row.runtimeLocationHandle, {}, {}, {}, row.lastFired, row.nextFireTime};
}

/**
 * @brief Applies compact scalar state to a trigger row.
 * @param row Trigger row to update.
 * @param state Scalar state to apply.
 */
inline void luaCallbackApplyRuleRuntimeState(LuaCallbackTriggerSnapshot                &row,
                                             const LuaCallbackRuleRuntimeStateSnapshot &state)
{
	row.matched              = state.matched;
	row.invocationCount      = state.invocationCount;
	row.matchCount           = state.matchCount;
	row.matchAttempts        = state.matchAttempts;
	row.executionTimeNs      = state.executionTimeNs;
	row.executingScriptDepth = state.executingScriptDepth;
	row.executingScript      = state.executingScript;
}

/**
 * @brief Applies compact scalar state to an alias row.
 * @param row Alias row to update.
 * @param state Scalar state to apply.
 */
inline void luaCallbackApplyRuleRuntimeState(LuaCallbackAliasSnapshot                  &row,
                                             const LuaCallbackRuleRuntimeStateSnapshot &state)
{
	row.matched              = state.matched;
	row.invocationCount      = state.invocationCount;
	row.matchCount           = state.matchCount;
	row.matchAttempts        = state.matchAttempts;
	row.executionTimeNs      = state.executionTimeNs;
	row.executingScriptDepth = state.executingScriptDepth;
	row.executingScript      = state.executingScript;
}

/**
 * @brief Applies compact scalar state to a timer row.
 * @param row Timer row to update.
 * @param state Scalar state to apply.
 */
inline void luaCallbackApplyRuleRuntimeState(LuaCallbackTimerSnapshot                  &row,
                                             const LuaCallbackRuleRuntimeStateSnapshot &state)
{
	row.firedCount           = state.firedCount;
	row.invocationCount      = state.invocationCount;
	row.executingScriptDepth = state.executingScriptDepth;
	row.executingScript      = state.executingScript;
}

/**
 * @brief Applies detail state to a trigger row.
 * @param row Trigger row to update.
 * @param state Detail state to apply.
 */
inline void luaCallbackApplyRuleDetailState(LuaCallbackTriggerSnapshot               &row,
                                            const LuaCallbackRuleDetailStateSnapshot &state)
{
	row.lastMatchTarget    = state.lastMatchTarget;
	row.lastMatchWildcards = state.lastMatchWildcards;
	row.lastMatched        = state.lastMatched;
}

/**
 * @brief Applies detail state to an alias row.
 * @param row Alias row to update.
 * @param state Detail state to apply.
 */
inline void luaCallbackApplyRuleDetailState(LuaCallbackAliasSnapshot                 &row,
                                            const LuaCallbackRuleDetailStateSnapshot &state)
{
	row.lastMatchTarget    = state.lastMatchTarget;
	row.lastMatchWildcards = state.lastMatchWildcards;
	row.lastMatched        = state.lastMatched;
}

/**
 * @brief Applies detail state to a timer row.
 * @param row Timer row to update.
 * @param state Detail state to apply.
 */
inline void luaCallbackApplyRuleDetailState(LuaCallbackTimerSnapshot                 &row,
                                            const LuaCallbackRuleDetailStateSnapshot &state)
{
	row.lastFired    = state.lastFired;
	row.nextFireTime = state.nextFireTime;
}

/**
 * @brief Returns the dense slot key encoded in a runtime-location handle.
 *
 * The low 32 bits identify the recycled allocator slot. The complete handle remains the identity and must be used
 * to distinguish generations that coexist in an immutable snapshot.
 *
 * @param runtimeLocationHandle Generation-checked runtime-location handle, or zero for the world owner.
 * @return Dense allocator slot key.
 */
[[nodiscard]] constexpr quint32
luaCallbackRuntimeLocationSlotKey(const quint64 runtimeLocationHandle) noexcept
{
	return static_cast<quint32>(runtimeLocationHandle & 0xffffffffULL);
}

/**
 * @brief Immutable fixed-depth slot map used by callback snapshots.
 *
 * Capturing a map copies one shared root. A write copies the eight nodes selected by the 32-bit slot key, independent
 * of the number of plugins or rules. Batch writes reuse every node already copied by that batch. Readers never touch
 * mutable runtime storage and therefore require no locking.
 *
 * @tparam Value Immutable value stored at a runtime-location slot.
 */
template <typename Value> class LuaCallbackPersistentSlotMap
{
	private:
		static constexpr int kLevels = 8;
		static constexpr int kFanout = 16;

		struct Node
		{
				std::array<QSharedPointer<const Node>, kFanout>  children;
				std::array<QSharedPointer<const Value>, kFanout> values;
				quint64                                          builderToken{0};
		};

		QSharedPointer<const Node>   m_root;

		[[nodiscard]] static quint64 nextBuilderToken() noexcept
		{
			static std::atomic<quint64> token{1};
			quint64                     result = token.fetch_add(1, std::memory_order_relaxed);
			if (result == 0)
				result = token.fetch_add(1, std::memory_order_relaxed);
			return result;
		}

		[[nodiscard]] static QSharedPointer<Node> cloneNode(const QSharedPointer<const Node> &source,
		                                                    const quint64                     builderToken)
		{
			auto clone = source ? QSharedPointer<Node>::create(*source) : QSharedPointer<Node>::create();
			clone->builderToken = builderToken;
			return clone;
		}

	public:
		/**
		 * @brief One replacement applied by withUpdates().
		 */
		struct Update
		{
				quint32                     key{0};
				QSharedPointer<const Value> value;
		};

		/**
		 * @brief Finds one immutable value.
		 * @param key Stable slot key.
		 * @return Stored value, or an empty pointer when absent.
		 */
		[[nodiscard]] QSharedPointer<const Value> value(const quint32 key) const
		{
			QSharedPointer<const Node> node = m_root;
			for (int level = kLevels - 1; level > 0; --level)
			{
				if (!node)
					return {};
				const int branch = static_cast<int>((key >> (level * 4)) & 0x0fU);
				node             = node->children[branch];
			}
			return node ? node->values[static_cast<int>(key & 0x0fU)] : QSharedPointer<const Value>{};
		}

		/**
		 * @brief Applies replacements and removals in one immutable transaction.
		 * @param updates Slot replacements; an empty value removes the slot.
		 * @return Map sharing every untouched page with this map.
		 */
		[[nodiscard]] LuaCallbackPersistentSlotMap withUpdates(const QVector<Update> &updates) const
		{
			if (updates.isEmpty())
				return *this;

			const quint64        builderToken = nextBuilderToken();
			QSharedPointer<Node> root         = cloneNode(m_root, builderToken);
			for (const Update &update : updates)
			{
				QSharedPointer<Node> node = root;
				for (int level = kLevels - 1; level > 0; --level)
				{
					const int            branch = static_cast<int>((update.key >> (level * 4)) & 0x0fU);
					auto                 child  = node->children[branch];
					QSharedPointer<Node> mutableChild;
					if (child && child->builderToken == builderToken)
						mutableChild = qSharedPointerConstCast<Node>(child);
					else
						mutableChild = cloneNode(child, builderToken);
					node->children[branch] = mutableChild;
					node                   = std::move(mutableChild);
				}
				node->values[static_cast<int>(update.key & 0x0fU)] = update.value;
			}

			LuaCallbackPersistentSlotMap result;
			result.m_root = std::move(root);
			return result;
		}

		bool operator==(const LuaCallbackPersistentSlotMap &other) const noexcept
		{
			return m_root == other.m_root;
		}
};

/**
 * @brief Immutable paged map for generation-checked callback rule runtime locations.
 *
 * Runtime-location slots are allocated densely and recycled. Each page slot retains the immutable values for every
 * generation that still belongs to that snapshot, distinguished by the complete runtime-location handle. Capturing
 * a map copies one shared directory root. A batch write copies one fixed-depth directory path per touched page and
 * each touched fixed-size page once. The normal one-generation case performs one directory lookup, one page lookup,
 * and one handle comparison.
 *
 * @tparam Value Immutable value stored at a runtime-location slot.
 */
template <typename Value> class LuaCallbackPersistentGenerationalDenseSlotMap
{
	private:
		static constexpr quint32 kPageSize = 64;

		struct CollisionEntry
		{
				QSharedPointer<const Value>          value;
				QSharedPointer<const CollisionEntry> next;
		};

		struct Page
		{
				std::array<QSharedPointer<const Value>, kPageSize>   values;
				QHash<quint32, QSharedPointer<const CollisionEntry>> collisionChains;
				quint32                                              occupiedCount{0};
		};

		LuaCallbackPersistentSlotMap<Page>        m_pages;

		/**
		 * @brief Clones one immutable page for a transaction.
		 * @param source Existing page, or an empty pointer for a new page.
		 * @return Mutable page clone.
		 */
		[[nodiscard]] static QSharedPointer<Page> clonePage(const QSharedPointer<const Page> &source)
		{
			return source ? QSharedPointer<Page>::create(*source) : QSharedPointer<Page>::create();
		}

		/**
		 * @brief Applies one exact-generation replacement or removal to a collision chain.
		 * @param head Existing immutable generation chain.
		 * @param runtimeLocationHandle Complete generation-checked identity to change.
		 * @param value Replacement value, or an empty pointer to remove that generation.
		 * @param found Receives whether the requested generation existed.
		 * @return Updated chain sharing every unaffected suffix.
		 */
		[[nodiscard]] static QSharedPointer<const CollisionEntry>
		updatedCollisionChain(const QSharedPointer<const CollisionEntry> &head,
		                      const quint64 runtimeLocationHandle, const QSharedPointer<const Value> &value,
		                      bool &found)
		{
			found = false;
			if (!head)
				return {};
			if (head->value->runtimeLocationHandle == runtimeLocationHandle)
			{
				found = true;
				return value ? QSharedPointer<const CollisionEntry>::create(CollisionEntry{value, head->next})
				             : head->next;
			}

			QVector<QSharedPointer<const CollisionEntry>> prefix;
			auto                                          current = head;
			while (current && current->value->runtimeLocationHandle != runtimeLocationHandle)
			{
				prefix.push_back(current);
				current = current->next;
			}
			if (!current)
				return head;

			found = true;
			QSharedPointer<const CollisionEntry> updated =
			    value ? QSharedPointer<const CollisionEntry>::create(CollisionEntry{value, current->next})
			          : current->next;
			for (auto it = prefix.crbegin(); it != prefix.crend(); ++it)
			{
				updated = QSharedPointer<const CollisionEntry>::create(
				    CollisionEntry{(*it)->value, std::move(updated)});
			}
			return updated;
		}

	public:
		/**
		 * @brief One replacement applied by withUpdates().
		 */
		struct Update
		{
				quint64                     runtimeLocationHandle{0};
				QSharedPointer<const Value> value;
		};

		/**
		 * @brief Finds one immutable value.
		 * @param runtimeLocationHandle Complete generation-checked runtime-location identity.
		 * @return Stored value, or an empty pointer when absent.
		 */
		[[nodiscard]] QSharedPointer<const Value> value(const quint64 runtimeLocationHandle) const
		{
			const quint32 key       = luaCallbackRuntimeLocationSlotKey(runtimeLocationHandle);
			const quint32 pageIndex = key / kPageSize;
			const auto    page      = m_pages.value(pageIndex);
			if (!page)
				return {};
			const quint32 entryIndex = key % kPageSize;
			const auto   &primary    = page->values.at(entryIndex);
			if (!primary || primary->runtimeLocationHandle == runtimeLocationHandle)
				return primary;
			auto collision = page->collisionChains.value(entryIndex);
			while (collision && collision->value->runtimeLocationHandle != runtimeLocationHandle)
				collision = collision->next;
			return collision ? collision->value : QSharedPointer<const Value>{};
		}

		/**
		 * @brief Applies replacements and removals in one immutable transaction.
		 * @param updates Exact-generation replacements; an empty value removes that generation.
		 * @return Map sharing every untouched page with this map.
		 */
		[[nodiscard]] LuaCallbackPersistentGenerationalDenseSlotMap
		withUpdates(const QVector<Update> &updates) const
		{
			if (updates.isEmpty())
				return *this;

			QHash<quint32, QSharedPointer<Page>> mutablePages;
			mutablePages.reserve(updates.size());
			for (const Update &update : updates)
			{
				Q_ASSERT(!update.value ||
				         update.value->runtimeLocationHandle == update.runtimeLocationHandle);
				const quint32 key       = luaCallbackRuntimeLocationSlotKey(update.runtimeLocationHandle);
				const quint32 pageIndex = key / kPageSize;
				auto          mutableIt = mutablePages.find(pageIndex);
				if (mutableIt == mutablePages.end())
				{
					const auto page = m_pages.value(pageIndex);
					if (!page && !update.value)
						continue;
					mutableIt = mutablePages.insert(pageIndex, clonePage(page));
				}
				QSharedPointer<Page> &mutablePage = mutableIt.value();

				const quint32         entryIndex = key % kPageSize;
				auto                 &primary    = mutablePage->values.at(entryIndex);
				const bool            wasPresent = static_cast<bool>(primary);
				if (!primary)
				{
					primary = update.value;
				}
				else if (primary->runtimeLocationHandle == update.runtimeLocationHandle)
				{
					if (update.value)
					{
						primary = update.value;
					}
					else if (const auto collisions = mutablePage->collisionChains.value(entryIndex))
					{
						primary = collisions->value;
						if (collisions->next)
							mutablePage->collisionChains.insert(entryIndex, collisions->next);
						else
							mutablePage->collisionChains.remove(entryIndex);
					}
					else
					{
						primary.reset();
					}
				}
				else
				{
					const auto collisions        = mutablePage->collisionChains.value(entryIndex);
					bool       collisionFound    = false;
					const auto updatedCollisions = updatedCollisionChain(
					    collisions, update.runtimeLocationHandle, update.value, collisionFound);
					if (collisionFound)
					{
						if (updatedCollisions)
							mutablePage->collisionChains.insert(entryIndex, updatedCollisions);
						else
							mutablePage->collisionChains.remove(entryIndex);
					}
					else if (update.value)
					{
						mutablePage->collisionChains.insert(entryIndex,
						                                    QSharedPointer<const CollisionEntry>::create(
						                                        CollisionEntry{primary, collisions}));
						primary = update.value;
					}
				}
				const bool isPresent = static_cast<bool>(primary);
				if (wasPresent != isPresent)
				{
					if (isPresent)
						++mutablePage->occupiedCount;
					else
						--mutablePage->occupiedCount;
				}
			}
			QVector<typename LuaCallbackPersistentSlotMap<Page>::Update> pageUpdates;
			pageUpdates.reserve(mutablePages.size());
			for (auto it = mutablePages.begin(); it != mutablePages.end(); ++it)
			{
				pageUpdates.push_back({it.key(), it.value()->occupiedCount == 0
				                                     ? QSharedPointer<const Page>{}
				                                     : QSharedPointer<const Page>(std::move(it.value()))});
			}

			LuaCallbackPersistentGenerationalDenseSlotMap result;
			result.m_pages = m_pages.withUpdates(pageUpdates);
			return result;
		}

		/**
		 * @brief Compares immutable directory identity.
		 * @param other Map to compare.
		 * @return Whether both maps share the same directory revision.
		 */
		bool operator==(const LuaCallbackPersistentGenerationalDenseSlotMap &other) const noexcept
		{
			return m_pages == other.m_pages;
		}
};

/**
 * @brief Immutable generation-checked map storing small values inline in fixed-size pages.
 *
 * The fixed-depth directory and exact-generation collision handling match
 * LuaCallbackPersistentGenerationalDenseSlotMap. Inline page cells avoid one allocation and one shared-pointer
 * reference count per scalar rule-state update.
 *
 * @tparam Value Small immutable value carrying a runtimeLocationHandle member.
 */
template <typename Value> class LuaCallbackPersistentGenerationalInlineSlotMap
{
	private:
		static constexpr quint32 kPageSize = 64;

		struct CollisionEntry
		{
				Value                                value;
				QSharedPointer<const CollisionEntry> next;
		};

		struct Page
		{
				std::array<std::optional<Value>, kPageSize>          values;
				QHash<quint32, QSharedPointer<const CollisionEntry>> collisionChains;
				quint32                                              occupiedCount{0};
		};

		LuaCallbackPersistentSlotMap<Page> m_pages;

		/**
		 * @brief Replaces or removes one exact generation in an immutable collision chain.
		 * @param head Existing collision chain.
		 * @param runtimeLocationHandle Complete generation-checked identity.
		 * @param value Replacement value, or no value for removal.
		 * @param found Receives whether the generation existed.
		 * @return Updated collision chain.
		 */
		[[nodiscard]] static QSharedPointer<const CollisionEntry>
		updatedCollisionChain(const QSharedPointer<const CollisionEntry> &head,
		                      const quint64 runtimeLocationHandle, const std::optional<Value> &value,
		                      bool &found)
		{
			found = false;
			if (!head)
				return {};
			if (head->value.runtimeLocationHandle == runtimeLocationHandle)
			{
				found = true;
				return value
				           ? QSharedPointer<const CollisionEntry>::create(CollisionEntry{*value, head->next})
				           : head->next;
			}

			QVector<QSharedPointer<const CollisionEntry>> prefix;
			auto                                          current = head;
			while (current && current->value.runtimeLocationHandle != runtimeLocationHandle)
			{
				prefix.push_back(current);
				current = current->next;
			}
			if (!current)
				return head;

			found = true;
			QSharedPointer<const CollisionEntry> updated =
			    value ? QSharedPointer<const CollisionEntry>::create(CollisionEntry{*value, current->next})
			          : current->next;
			for (auto it = prefix.crbegin(); it != prefix.crend(); ++it)
			{
				updated = QSharedPointer<const CollisionEntry>::create(
				    CollisionEntry{(*it)->value, std::move(updated)});
			}
			return updated;
		}

	public:
		/** @brief One exact-generation replacement or removal. */
		struct Update
		{
				quint64              runtimeLocationHandle{0};
				std::optional<Value> value;
		};

		/**
		 * @brief Finds one exact-generation value.
		 * @param runtimeLocationHandle Complete generation-checked runtime-location identity.
		 * @return Stored value, or no value when absent.
		 */
		[[nodiscard]] std::optional<Value> value(const quint64 runtimeLocationHandle) const
		{
			const quint32 key       = luaCallbackRuntimeLocationSlotKey(runtimeLocationHandle);
			const quint32 pageIndex = key / kPageSize;
			const auto    page      = m_pages.value(pageIndex);
			if (!page)
				return std::nullopt;
			const quint32 entryIndex = key % kPageSize;
			const auto   &primary    = page->values.at(entryIndex);
			if (!primary || primary->runtimeLocationHandle == runtimeLocationHandle)
				return primary;
			auto collision = page->collisionChains.value(entryIndex);
			while (collision && collision->value.runtimeLocationHandle != runtimeLocationHandle)
				collision = collision->next;
			return collision ? std::optional<Value>{collision->value} : std::nullopt;
		}

		/**
		 * @brief Applies exact-generation replacements and removals in one immutable transaction.
		 * @param updates Replacements and removals to apply.
		 * @return Map sharing all untouched directory paths and pages.
		 */
		[[nodiscard]] LuaCallbackPersistentGenerationalInlineSlotMap
		withUpdates(const QVector<Update> &updates) const
		{
			if (updates.isEmpty())
				return *this;

			QHash<quint32, QSharedPointer<Page>> mutablePages;
			mutablePages.reserve(updates.size());
			for (const Update &update : updates)
			{
				Q_ASSERT(!update.value ||
				         update.value->runtimeLocationHandle == update.runtimeLocationHandle);
				const quint32 key       = luaCallbackRuntimeLocationSlotKey(update.runtimeLocationHandle);
				const quint32 pageIndex = key / kPageSize;
				auto          mutableIt = mutablePages.find(pageIndex);
				if (mutableIt == mutablePages.end())
				{
					const auto page = m_pages.value(pageIndex);
					if (!page && !update.value)
						continue;
					mutableIt = mutablePages.insert(pageIndex, page ? QSharedPointer<Page>::create(*page)
					                                                : QSharedPointer<Page>::create());
				}
				QSharedPointer<Page> &page       = mutableIt.value();
				const quint32         entryIndex = key % kPageSize;
				auto                 &primary    = page->values.at(entryIndex);
				const bool            wasPresent = primary.has_value();
				if (!primary)
				{
					primary = update.value;
				}
				else if (primary->runtimeLocationHandle == update.runtimeLocationHandle)
				{
					if (update.value)
						primary = update.value;
					else if (const auto collisions = page->collisionChains.value(entryIndex))
					{
						primary = collisions->value;
						if (collisions->next)
							page->collisionChains.insert(entryIndex, collisions->next);
						else
							page->collisionChains.remove(entryIndex);
					}
					else
						primary.reset();
				}
				else
				{
					const auto collisions = page->collisionChains.value(entryIndex);
					bool       found      = false;
					const auto updated =
					    updatedCollisionChain(collisions, update.runtimeLocationHandle, update.value, found);
					if (found)
					{
						if (updated)
							page->collisionChains.insert(entryIndex, updated);
						else
							page->collisionChains.remove(entryIndex);
					}
					else if (update.value)
					{
						page->collisionChains.insert(entryIndex, QSharedPointer<const CollisionEntry>::create(
						                                             CollisionEntry{*primary, collisions}));
						primary = update.value;
					}
				}
				const bool isPresent = primary.has_value();
				if (wasPresent != isPresent)
				{
					if (isPresent)
						++page->occupiedCount;
					else
						--page->occupiedCount;
				}
			}

			QVector<typename LuaCallbackPersistentSlotMap<Page>::Update> pageUpdates;
			pageUpdates.reserve(mutablePages.size());
			for (auto it = mutablePages.begin(); it != mutablePages.end(); ++it)
			{
				pageUpdates.push_back({it.key(), it.value()->occupiedCount == 0
				                                     ? QSharedPointer<const Page>{}
				                                     : QSharedPointer<const Page>(std::move(it.value()))});
			}
			LuaCallbackPersistentGenerationalInlineSlotMap result;
			result.m_pages = m_pages.withUpdates(pageUpdates);
			return result;
		}

		/**
		 * @brief Compares immutable directory identity.
		 * @param other Map to compare.
		 * @return Whether both maps share one directory revision.
		 */
		bool operator==(const LuaCallbackPersistentGenerationalInlineSlotMap &other) const noexcept
		{
			return m_pages == other.m_pages;
		}
};

/**
 * @brief Immutable string-keyed map with structural sharing between callback snapshots.
 *
 * A write copies one fixed-depth hash path and the final collision bucket. Unchanged values and branches remain
 * shared, so publishing one named mutation never detaches a container proportional to the total entry count.
 * Hash collisions are resolved by the complete string key in the leaf bucket.
 *
 * @tparam Value Immutable value stored under a string key.
 */
template <typename Value> class LuaCallbackPersistentStringMap
{
	public:
		using ValuePointer = QSharedPointer<const Value>;

		/** @brief Result of one exact-key mutation. */
		struct MutationResult
		{
				ValuePointer       previousValue;
				bool               changed{false};

				/** @return Whether the key existed before the mutation. */
				[[nodiscard]] bool existed() const noexcept
				{
					return !previousValue.isNull();
				}
		};

	private:
		static constexpr int     kBitsPerLevel = 5;
		static constexpr int     kLevels       = 13;
		static constexpr quint64 kBranchMask   = 0x1fU;

		struct Bucket
		{
				quint64                      builderToken{0};
				QHash<QString, ValuePointer> values;
		};

		struct Node
		{
				quint64                               builderToken{0};
				quint32                               occupiedBranches{0};
				QVector<QSharedPointer<const Node>>   children;
				QVector<QSharedPointer<const Bucket>> buckets;
		};

		QSharedPointer<const Node>   m_root;
		qsizetype                    m_size{0};

		[[nodiscard]] static quint64 keyHash(const QString &key) noexcept
		{
			if constexpr (sizeof(size_t) >= sizeof(quint64))
				return static_cast<quint64>(qHash(key, size_t{0}));
			const auto high = static_cast<quint64>(qHash(key, size_t{0x4d595df4U}));
			const auto low  = static_cast<quint64>(qHash(key, size_t{0x1b873593U}));
			return (high << 32U) | low;
		}

		[[nodiscard]] static quint64 nextBuilderToken() noexcept
		{
			static std::atomic<quint64> nextToken{1};
			quint64                     result = nextToken.fetch_add(1, std::memory_order_relaxed);
			if (result == 0)
				result = nextToken.fetch_add(1, std::memory_order_relaxed);
			return result;
		}

		[[nodiscard]] static int branchForHash(const quint64 hash, const int level) noexcept
		{
			return static_cast<int>((hash >> (level * kBitsPerLevel)) & kBranchMask);
		}

		[[nodiscard]] static quint32 branchBit(const int branch) noexcept
		{
			return quint32{1} << static_cast<quint32>(branch);
		}

		[[nodiscard]] static qsizetype branchIndex(const quint32 bitmap, const quint32 bit) noexcept
		{
			return static_cast<qsizetype>(std::popcount(bitmap & (bit - 1U)));
		}

		[[nodiscard]] static QSharedPointer<Node> editableNode(const QSharedPointer<const Node> &source,
		                                                       const quint64                     builderToken)
		{
			if (source && source->builderToken == builderToken)
				return qSharedPointerConstCast<Node>(source);
			auto result = source ? QSharedPointer<Node>::create(*source) : QSharedPointer<Node>::create();
			result->builderToken = builderToken;
			return result;
		}

		[[nodiscard]] static QSharedPointer<Bucket> editableBucket(const QSharedPointer<const Bucket> &source,
		                                                           const quint64 builderToken)
		{
			if (source && source->builderToken == builderToken)
				return qSharedPointerConstCast<Bucket>(source);
			auto result = source ? QSharedPointer<Bucket>::create(*source) : QSharedPointer<Bucket>::create();
			result->builderToken = builderToken;
			return result;
		}

		[[nodiscard]] static QSharedPointer<const Node>
		updateNode(const QSharedPointer<const Node> &source, const quint64 hash, const int level,
		           const QString &key, const ValuePointer &value, const quint64 builderToken,
		           MutationResult &mutationResult)
		{
			const int       branch  = branchForHash(hash, level);
			const quint32   bit     = branchBit(branch);
			const quint32   bitmap  = source ? source->occupiedBranches : 0U;
			const bool      present = (bitmap & bit) != 0U;
			const qsizetype index   = branchIndex(bitmap, bit);
			if (level > 0)
			{
				const QSharedPointer<const Node> child = present ? source->children.at(index) : nullptr;
				const QSharedPointer<const Node> updated =
				    updateNode(child, hash, level - 1, key, value, builderToken, mutationResult);
				if (updated == child)
					return source;
				auto result = editableNode(source, builderToken);
				if (updated)
				{
					if (present)
						result->children[index] = updated;
					else
					{
						result->occupiedBranches |= bit;
						result->children.insert(index, updated);
					}
				}
				else if (present)
				{
					result->occupiedBranches &= ~bit;
					result->children.removeAt(index);
				}
				return result->occupiedBranches == 0U ? QSharedPointer<const Node>{} : result;
			}
			else
			{
				const QSharedPointer<const Bucket> existing = present ? source->buckets.at(index) : nullptr;
				mutationResult.previousValue = existing ? existing->values.value(key) : ValuePointer{};
				if (!value && !mutationResult.previousValue)
					return source;
				if (value == mutationResult.previousValue)
					return source;
				mutationResult.changed = true;
				auto result            = editableNode(source, builderToken);
				auto bucket            = editableBucket(existing, builderToken);
				if (value)
					bucket->values.insert(key, value);
				else
					bucket->values.remove(key);
				if (!bucket->values.isEmpty())
				{
					if (present)
						result->buckets[index] = bucket;
					else
					{
						result->occupiedBranches |= bit;
						result->buckets.insert(index, bucket);
					}
				}
				else if (present)
				{
					result->occupiedBranches &= ~bit;
					result->buckets.removeAt(index);
				}
				return result->occupiedBranches == 0U ? QSharedPointer<const Node>{} : result;
			}
		}

		template <typename Visitor>
		static void visitNode(const QSharedPointer<const Node> &node, const int level, Visitor &visitor)
		{
			if (!node)
				return;
			if (level > 0)
			{
				for (const auto &child : node->children)
					visitNode(child, level - 1, visitor);
				return;
			}
			for (const auto &bucket : node->buckets)
			{
				if (!bucket)
					continue;
				for (auto it = bucket->values.cbegin(); it != bucket->values.cend(); ++it)
					visitor(it.key(), it.value());
			}
		}

		[[nodiscard]] static ValuePointer valueInRoot(const QSharedPointer<const Node> &root,
		                                              const QString                    &key)
		{
			const quint64              hash = keyHash(key);
			QSharedPointer<const Node> node = root;
			for (int level = kLevels - 1; level > 0; --level)
			{
				if (!node)
					return {};
				const quint32 bit = branchBit(branchForHash(hash, level));
				if ((node->occupiedBranches & bit) == 0U)
					return {};
				node = node->children.at(branchIndex(node->occupiedBranches, bit));
			}
			if (!node)
				return {};
			const quint32 bit = branchBit(branchForHash(hash, 0));
			if ((node->occupiedBranches & bit) == 0U)
				return {};
			const auto &bucket = node->buckets.at(branchIndex(node->occupiedBranches, bit));
			return bucket->values.value(key);
		}

	public:
		/** @brief One replacement or removal applied by withUpdates(). */
		struct Update
		{
				QString      key;
				ValuePointer value;
		};

		/**
		 * @brief Mutable transaction that reuses copied hash paths until it is frozen.
		 *
		 * A transaction is private to its caller. finish() publishes a new immutable map; starting another
		 * transaction uses a new edit token and therefore cannot modify an already published version.
		 */
		class Transaction
		{
			public:
				explicit Transaction(const LuaCallbackPersistentStringMap &source)
				    : m_root(source.m_root), m_size(source.m_size), m_builderToken(nextBuilderToken())
				{
				}
				Transaction(const Transaction &)                       = delete;
				Transaction &operator=(const Transaction &)            = delete;
				Transaction(Transaction &&)                            = delete;
				Transaction                 &operator=(Transaction &&) = delete;

				/** Applies one replacement or removal and returns its prior state. */
				[[nodiscard]] MutationResult apply(const Update &update)
				{
					Q_ASSERT(m_builderToken != 0);
					MutationResult result;
					m_root = updateNode(m_root, keyHash(update.key), kLevels - 1, update.key, update.value,
					                    m_builderToken, result);
					if (result.changed && result.existed() != !update.value.isNull())
						m_size += update.value ? 1 : -1;
					return result;
				}

				/** @return Current transaction value for @p key, or an empty pointer when absent. */
				[[nodiscard]] ValuePointer value(const QString &key) const
				{
					return valueInRoot(m_root, key);
				}

				/** @return Whether @p key is present in the current transaction. */
				[[nodiscard]] bool contains(const QString &key) const
				{
					return !value(key).isNull();
				}

				/** @return Number of current transaction entries. */
				[[nodiscard]] qsizetype size() const noexcept
				{
					return m_size;
				}

				/** Invokes @p visitor for every current transaction entry. */
				template <typename Visitor> void forEach(Visitor visitor) const
				{
					visitNode(m_root, kLevels - 1, visitor);
				}

				/** @return Current transaction keys in unspecified order. */
				[[nodiscard]] QStringList keys() const
				{
					QStringList result;
					result.reserve(m_size);
					forEach([&result](const QString &key, const ValuePointer &) { result.append(key); });
					return result;
				}

				/** @return Immutable map containing every transaction mutation. */
				[[nodiscard]] LuaCallbackPersistentStringMap finish() &&
				{
					LuaCallbackPersistentStringMap result;
					result.m_root  = std::move(m_root);
					result.m_size  = m_size;
					m_builderToken = 0;
					return result;
				}

			private:
				QSharedPointer<const Node> m_root;
				qsizetype                  m_size{0};
				quint64                    m_builderToken{0};
		};

		/** @return Immutable value stored for @p key, or an empty pointer when absent. */
		[[nodiscard]] ValuePointer value(const QString &key) const
		{
			return valueInRoot(m_root, key);
		}

		/** @return Whether @p key is present. */
		[[nodiscard]] bool contains(const QString &key) const
		{
			return !value(key).isNull();
		}

		/** @return Number of entries. */
		[[nodiscard]] qsizetype size() const noexcept
		{
			return m_size;
		}

		/** @return Whether the map has no entries. */
		[[nodiscard]] bool isEmpty() const noexcept
		{
			return m_size == 0;
		}

		/**
		 * @brief Applies exact replacements and removals.
		 * @param updates Updates whose empty values remove their keys.
		 * @return New map sharing all unaffected hash paths.
		 */
		[[nodiscard]] LuaCallbackPersistentStringMap withUpdates(const QVector<Update> &updates) const
		{
			Transaction transaction(*this);
			for (const Update &update : updates)
				static_cast<void>(transaction.apply(update));
			return std::move(transaction).finish();
		}

		/** Invokes @p visitor for every stored key and immutable value. */
		template <typename Visitor> void forEach(Visitor visitor) const
		{
			visitNode(m_root, kLevels - 1, visitor);
		}

		/** @return Every stored key in unspecified order. */
		[[nodiscard]] QStringList keys() const
		{
			QStringList result;
			result.reserve(m_size);
			forEach([&result](const QString &key, const ValuePointer &) { result.append(key); });
			return result;
		}

		bool operator==(const LuaCallbackPersistentStringMap &other) const
		{
			if (m_root == other.m_root)
				return true;
			if (m_size != other.m_size)
				return false;
			bool equal = true;
			forEach(
			    [&other, &equal](const QString &key, const ValuePointer &value)
			    {
				    if (equal)
				    {
					    const ValuePointer otherValue = other.value(key);
					    equal                         = otherValue && *value == *otherValue;
				    }
			    });
			return equal;
		}
};

/**
 * @brief Ordered owner membership and immutable rule rows for one callback rule domain.
 * @tparam Row Serialized trigger, alias, or timer row.
 */
template <typename Row> struct LuaCallbackRuleSnapshotStore
{
		/**
		 * @brief One world or plugin rule collection.
		 */
		struct Owner
		{
				quint64          runtimeLocationHandle{0};
				QVector<quint64> orderedRuleLocationHandles;
				bool             operator==(const Owner &) const = default;
		};

		using OwnerMap = LuaCallbackPersistentGenerationalDenseSlotMap<Owner>;
		using RowMap   = LuaCallbackPersistentGenerationalDenseSlotMap<Row>;
		using RuntimeStateMap =
		    LuaCallbackPersistentGenerationalInlineSlotMap<LuaCallbackRuleRuntimeStateSnapshot>;
		using DetailStateMap =
		    LuaCallbackPersistentGenerationalDenseSlotMap<LuaCallbackRuleDetailStateSnapshot>;

		OwnerMap        owners;
		RowMap          rows;
		RuntimeStateMap runtimeStates;
		DetailStateMap  detailStates;

		bool            operator==(const LuaCallbackRuleSnapshotStore &) const = default;
};

/**
 * @brief Materializes one exact rule row from its independently shared components.
 * @param store Rule snapshot store.
 * @param runtimeLocationHandle Generation-checked rule location.
 * @return Combined row, or no value when any component is absent or has a different identity.
 */
template <typename Row>
[[nodiscard]] std::optional<Row> luaCallbackRuleSnapshotValue(const LuaCallbackRuleSnapshotStore<Row> &store,
                                                              const quint64 runtimeLocationHandle)
{
	const auto definition = store.rows.value(runtimeLocationHandle);
	const auto runtime    = store.runtimeStates.value(runtimeLocationHandle);
	const auto detail     = store.detailStates.value(runtimeLocationHandle);
	if (!definition || !runtime || !detail || definition->runtimeId != runtime->runtimeId ||
	    definition->runtimeId != detail->runtimeId)
	{
		return std::nullopt;
	}
	Row row = *definition;
	luaCallbackApplyRuleRuntimeState(row, *runtime);
	luaCallbackApplyRuleDetailState(row, *detail);
	return row;
}

/**
 * @brief Materializes one ordered rule collection from an immutable snapshot store.
 * @param store Rule snapshot store.
 * @param ownerRuntimeLocationHandle World owner zero, or a generation-checked plugin location.
 * @param found Receives whether the exact owner generation exists.
 * @return Ordered snapshot rows when found.
 */
template <typename Row>
[[nodiscard]] QList<Row> luaCallbackRuleSnapshotList(const LuaCallbackRuleSnapshotStore<Row> &store,
                                                     const quint64 ownerRuntimeLocationHandle, bool &found)
{
	found            = false;
	const auto owner = store.owners.value(ownerRuntimeLocationHandle);
	if (!owner)
		return {};

	QList<Row> result;
	result.reserve(owner->orderedRuleLocationHandles.size());
	for (const quint64 ruleHandle : owner->orderedRuleLocationHandles)
	{
		const auto row = luaCallbackRuleSnapshotValue(store, ruleHandle);
		if (!row)
			return {};
		result.push_back(std::move(*row));
	}
	found = true;
	return result;
}

/**
 * @brief Accelerator payload used by callback-scope runtime snapshots.
 */
struct LuaCallbackAcceleratorSnapshot
{
		qint64  key{0};
		int     commandId{-1};
		QString text;
		int     sendTo{0};
		bool    operator==(const LuaCallbackAcceleratorSnapshot &) const = default;
};

/**
 * @brief World entry payload used by callback-scope world-list snapshots.
 */
struct LuaCallbackWorldRuntimeSnapshot
{
		QPointer<WorldRuntime> runtime;
		QString                id;
		QString                name;
};

/**
 * @brief World child-window geometry payload captured before callback dispatch.
 */
struct LuaCallbackWorldWindowPositionSnapshot
{
		WorldRuntime *runtime{nullptr};
		int           ordinal{0};
		QRect         normalGeometry;
		QRect         frameGeometry;
		QRect         screenNormalGeometry;
		QRect         screenFrameGeometry;
};

/**
 * @brief Notepad window payload captured before callback dispatch.
 */
struct LuaCallbackNotepadSnapshot
{
		WorldRuntime *runtime{nullptr};
		QString       worldId;
		QString       title;
		QRect         geometry;
		QString       text;
		bool          hasEditor{false};
		bool          hasGeometry{false};
		bool          hasText{false};
};

/**
 * @brief Style span payload used by callback-scope output-buffer snapshots.
 */
struct LuaCallbackLineStyleSnapshot
{
		int     length{0};
		quint32 foreRgba{0};
		quint32 backRgba{0};
		bool    foreValid{false};
		bool    backValid{false};
		bool    bold{false};
		bool    underline{false};
		bool    italic{false};
		bool    blink{false};
		bool    strike{false};
		bool    inverse{false};
		bool    changed{false};
		int     actionType{0};
		QString action;
		QString hint;
		QString variable;
		bool    startTag{false};

		bool    operator==(const LuaCallbackLineStyleSnapshot &) const = default;
};

/**
 * @brief Styled text run exposed to Lua together with its lossless source-span segmentation.
 *
 * The first four fields are the MUSHclient-compatible callback projection. `sourceSpans` is internal
 * transport used when the same callback line must also back GetStyleInfo without losing action or style
 * metadata while adjacent visually equivalent callback runs are merged.
 */
struct LuaStyleRun
{
		QString                               text;
		int                                   textColour{0};
		int                                   backColour{0};
		int                                   style{0};
		QVector<LuaCallbackLineStyleSnapshot> sourceSpans;
};

/**
 * @brief Output-buffer line payload used by callback-scope read snapshots.
 */
struct LuaCallbackLineEntrySnapshot
{
		QString                               text;
		int                                   flags{0};
		bool                                  hardReturn{true};
		QVector<LuaCallbackLineStyleSnapshot> spans;
		QDateTime                             time;
		qint64                                lineNumber{0};
		double                                ticks{0.0};
		double                                elapsed{0.0};
};

/**
 * @brief Immutable output-buffer snapshot shared by callback dispatches that need line APIs.
 */
struct LuaCallbackLineBufferSnapshot
{
		quint64                                  lineBufferGeneration{0};
		int                                      lineBufferCount{0};
		int                                      firstCapturedLineBufferIndex{0};
		int                                      lastCapturedLineBufferIndex{0};
		QHash<int, LuaCallbackLineEntrySnapshot> lineEntriesByBufferIndex;
		QStringList                              recentLinesSnapshot;
};

/**
 * @brief SQLite handle/query state used by callback-scope database read snapshots.
 */
struct LuaCallbackDatabaseSnapshot
{
		QString           diskName;
		bool              isOpen{false};
		bool              stmtPrepared{false};
		bool              validRow{false};
		int               columns{0};
		int               columnsStatus{0};
		int               lastError{0};
		QString           errorText;
		int               totalChanges{0};
		int               changes{0};
		QString           lastInsertRowid;
		QStringList       columnNames;
		QVector<QVariant> columnValues;
		bool              operator==(const LuaCallbackDatabaseSnapshot &) const = default;
};

/**
 * @brief Callback-local indexed database API values grouped by database name.
 *
 * Keeping the indexed values in one per-database object makes invalidating a stepped or reset database an O(1)
 * outer-map erase. It also avoids composite string keys and scans through cached values belonging to unrelated
 * databases.
 */
struct LuaCallbackDatabaseIndexedCache
{
		QHash<int, QString>  columnNamesByIndex;
		QSet<int>            missingColumnNameIndexes;
		QHash<int, QString>  columnTextByIndex;
		QSet<int>            missingColumnTextIndexes;
		QHash<int, QVariant> columnValuesByIndex;
		QSet<int>            missingColumnValueIndexes;
		QHash<int, int>      columnTypesByIndex;
		QHash<int, QVariant> infoByType;
		QSet<int>            missingInfoTypes;

		bool                 operator==(const LuaCallbackDatabaseIndexedCache &) const = default;
};

/**
 * @brief Aggregate result shape shared by initial, resumed, and fallback dispatch handling.
 */
enum class LuaBatchAggregateShape
{
	None,
	BooleanAndFunction,
	StopOnFalse,
	FunctionHandled,
	StopOnTrue,
	FunctionCount,
	FunctionAny,
	BytesInOut,
	StringInOut,
	LastResult
};

/**
 * @brief How a suspended dispatch is completed after its current recipient resumes.
 *
 * This is dispatch semantics, not WorldRuntime policy. `ResultOnly` returns the resumed operation as-is,
 * `AggregateSingleRecipient` merges it into the pre-yield result and finishes, and
 * `AggregateRemainingRecipients` additionally continues through the request's later recipients.
 */
enum class LuaBatchResumeContinuationShape
{
	ResultOnly,
	AggregateSingleRecipient,
	AggregateRemainingRecipients
};

/**
 * @brief Extra neutral-result contract not derivable from recipient aggregation.
 */
enum class LuaBatchFallbackShape
{
	AggregateNeutral,
	HasFunctionFalse,
	BoolFalse,
	BoolAndFunctionFalse
};

/**
 * @brief Whether a queued callback dispatch requires a Lua function name.
 */
enum class LuaBatchFunctionNamePolicy
{
	Required,
	NotRequired
};

/**
 * @brief Authoritative registry for all batch-kind dispatch policies.
 *
 * Adding a dispatch kind must classify aggregation, continuation, fallback, and function-name handling here.
 * The enum and every policy query are generated from this registry so no dispatch site can acquire a second
 * hand-maintained kind list.
 */
#define QMUD_LUA_BATCH_DISPATCH_KIND_REGISTRY(X)                                                             \
	X(NoArgs, BooleanAndFunction, AggregateRemainingRecipients, AggregateNeutral, Required)                  \
	X(HasFunction, None, ResultOnly, HasFunctionFalse, Required)                                             \
	X(String, LastResult, AggregateRemainingRecipients, AggregateNeutral, Required)                          \
	X(StringStopOnFalse, StopOnFalse, AggregateRemainingRecipients, AggregateNeutral, Required)              \
	X(StringHandled, FunctionHandled, AggregateRemainingRecipients, AggregateNeutral, Required)              \
	X(Bytes, LastResult, AggregateRemainingRecipients, AggregateNeutral, Required)                           \
	X(BytesInOut, BytesInOut, AggregateRemainingRecipients, AggregateNeutral, Required)                      \
	X(StringInOut, StringInOut, AggregateRemainingRecipients, AggregateNeutral, Required)                    \
	X(NumberAndStringStopOnTrue, StopOnTrue, AggregateRemainingRecipients, AggregateNeutral, Required)       \
	X(NumberAndStringStopOnFalse, StopOnFalse, AggregateRemainingRecipients, AggregateNeutral, Required)     \
	X(NumberAndString, LastResult, AggregateRemainingRecipients, AggregateNeutral, Required)                 \
	X(TwoNumbersAndStringStopOnFalse, StopOnFalse, AggregateRemainingRecipients, AggregateNeutral, Required) \
	X(TwoNumbersAndString, LastResult, AggregateRemainingRecipients, AggregateNeutral, Required)             \
	X(NumberAndBytesStopOnTrue, StopOnTrue, AggregateRemainingRecipients, AggregateNeutral, Required)        \
	X(NumberAndBytes, LastResult, AggregateRemainingRecipients, AggregateNeutral, Required)                  \
	X(NumberAndUtf8StringsCount, FunctionCount, AggregateRemainingRecipients, AggregateNeutral, Required)    \
	X(StringsAndWildcards, FunctionAny, AggregateRemainingRecipients, AggregateNeutral, Required)            \
	X(ExecuteScript, LastResult, AggregateSingleRecipient, BoolFalse, NotRequired)                           \
	X(ResetAndLoadScript, None, ResultOnly, BoolFalse, NotRequired)                                          \
	X(InitializeEnginesWithObservedCallbacksMany, None, ResultOnly, AggregateNeutral, Required)              \
	X(UpdateObservedCallbacksMany, None, ResultOnly, AggregateNeutral, Required)                             \
	X(CallPluginLuaMarshalling, None, ResultOnly, BoolFalse, Required)                                       \
	X(TeardownEnginesMany, None, ResultOnly, AggregateNeutral, Required)                                     \
	X(ApplyPackageRestrictionsMany, None, ResultOnly, AggregateNeutral, NotRequired)                         \
	X(ProcedureWithString, LastResult, AggregateSingleRecipient, BoolAndFunctionFalse, Required)             \
	X(MxpError, LastResult, AggregateSingleRecipient, BoolFalse, Required)                                   \
	X(MxpStartUp, None, AggregateRemainingRecipients, AggregateNeutral, Required)                            \
	X(MxpShutDown, None, AggregateRemainingRecipients, AggregateNeutral, Required)                           \
	X(MxpStartTag, LastResult, AggregateSingleRecipient, BoolFalse, Required)                                \
	X(MxpEndTag, None, AggregateRemainingRecipients, AggregateNeutral, Required)                             \
	X(MxpSetVariable, None, AggregateRemainingRecipients, AggregateNeutral, Required)                        \
	X(CancelSuspendedModalString, None, ResultOnly, AggregateNeutral, NotRequired)                           \
	X(ResumeSuspendedModalString, None, ResultOnly, AggregateNeutral, NotRequired)

/**
 * @brief Batch callback command kinds routed through the Lua executor.
 */
enum class LuaBatchDispatchKind
{
#define QMUD_DECLARE_LUA_BATCH_DISPATCH_KIND(name, aggregateShape, continuationShape, fallbackShape,         \
                                             functionNamePolicy)                                             \
	name,
	QMUD_LUA_BATCH_DISPATCH_KIND_REGISTRY(QMUD_DECLARE_LUA_BATCH_DISPATCH_KIND)
#undef QMUD_DECLARE_LUA_BATCH_DISPATCH_KIND
};

/**
 * @brief Atomic configuration for resetting and reloading one Lua script engine.
 */
struct LuaScriptReloadRequest
{
		QString                                           scriptText;
		std::shared_ptr<LuaScriptReloadCancellationState> cancellationState;
		bool                                              allowPackage{true};
		bool                                              loadScript{true};
};

/**
 * @brief Worker lane selection for Lua batch dispatch commands.
 */
enum class LuaBatchDispatchLane
{
	Control,
	Callback
};

/**
 * @brief Ingress-owned runtime values captured when one complete line is emitted.
 *
 * Parsing may continue while that line's Lua work is suspended. These values keep callbacks caused by the line
 * from observing counters or protocol state belonging to later lines or packets.
 */
#define QMUD_LUA_CALLBACK_INCOMING_LINE_OBSERVATION_FIELDS(X)                                                \
	X(NewLines, newLines, int, m_newLines)                                                                   \
	X(TotalLinesReceived, totalLinesReceived, int, m_linesReceived)                                          \
	X(InputPacketCount, inputPacketCount, int, m_inputPacketCount)                                           \
	X(TotalUncompressedBytes, totalUncompressedBytes, qint64, m_telnet.totalUncompressedBytes())             \
	X(TotalCompressedBytes, totalCompressedBytes, qint64, m_telnet.totalCompressedBytes())                   \
	X(MccpType, mccpType, int, m_telnet.mccpType())                                                          \
	X(MxpErrorCount, mxpErrorCount, int, m_mxpErrors)                                                        \
	X(MxpTagCount, mxpTagCount, qint64, m_telnet.mxpTagCount())                                              \
	X(MxpEntityCount, mxpEntityCount, qint64, m_telnet.mxpEntityCount())                                     \
	X(BytesIn, bytesIn, qint64, m_bytesIn)                                                                   \
	X(Utf8ErrorCount, utf8ErrorCount, int, m_utf8ErrorCount)                                                 \
	X(LastLineWithIacGa, lastLineWithIacGa, int, m_lastLineWithIacGa)                                        \
	X(LastTelnetSubnegotiation, lastTelnetSubnegotiation, QString, m_lastTelnetSubnegotiation)

struct LuaCallbackIncomingLineObservation
{
#define QMUD_DECLARE_INCOMING_LINE_OBSERVATION_FIELD(enumName, member, type, source) type member{};
		QMUD_LUA_CALLBACK_INCOMING_LINE_OBSERVATION_FIELDS(QMUD_DECLARE_INCOMING_LINE_OBSERVATION_FIELD)
#undef QMUD_DECLARE_INCOMING_LINE_OBSERVATION_FIELD
		bool valid{false};
};

/**
 * @brief Outbound counters retained across one asynchronous callback notification.
 *
 * The event producer selects the observation boundary. Recipient continuations carry mutations to these fields
 * without exposing later unrelated runtime values or freezing unrelated callback-visible state.
 */
#define QMUD_LUA_CALLBACK_SENT_OBSERVATION_FIELDS(X)                                                         \
	X(TotalLinesSent, totalLinesSent, int)                                                                   \
	X(OutputPacketCount, outputPacketCount, int)                                                             \
	X(BytesOut, bytesOut, qint64)

struct LuaCallbackSentObservation
{
#define QMUD_DECLARE_SENT_OBSERVATION_FIELD(enumName, member, type) type member{};
		QMUD_LUA_CALLBACK_SENT_OBSERVATION_FIELDS(QMUD_DECLARE_SENT_OBSERVATION_FIELD)
#undef QMUD_DECLARE_SENT_OBSERVATION_FIELD
		bool valid{false};
};

/**
 * @brief Queue-ordering domain for a callback event.
 */
enum class LuaCallbackOrderingDomain : quint8
{
	Deferrable,
	TelnetNotification,
	InputCritical
};

/**
 * @brief Authoritative callback-event identity and queue-ordering registry.
 *
 * A routine name does not identify an event: aliases, timers, and triggers may call a function whose name also
 * happens to be a MUSHclient plugin callback name. Only the event-producing runtime path assigns one of these values.
 * The registry is the single source for both the standard plugin function name and queue-ordering domain. Caller
 * completion requirements are deliberately represented separately by the runtime dispatch API.
 */
#define QMUD_LUA_CALLBACK_EVENT_REGISTRY(X)                                                                  \
	X(None, "", Deferrable)                                                                                  \
	X(TriggerRoutine, "", InputCritical)                                                                     \
	X(TriggerScript, "", InputCritical)                                                                      \
	X(AliasRoutine, "", InputCritical)                                                                       \
	X(AliasScript, "", InputCritical)                                                                        \
	X(TimerRoutine, "", Deferrable)                                                                          \
	X(TimerScript, "", Deferrable)                                                                           \
	X(Hotspot, "", InputCritical)                                                                            \
	X(AsyncResult, "OnPluginAsyncResult", Deferrable)                                                        \
	X(Broadcast, "OnPluginBroadcast", Deferrable)                                                            \
	X(ChatAccept, "OnPluginChatAccept", Deferrable)                                                          \
	X(ChatDisplay, "OnPluginChatDisplay", Deferrable)                                                        \
	X(ChatMessage, "OnPluginChatMessage", Deferrable)                                                        \
	X(ChatMessageOut, "OnPluginChatMessageOut", Deferrable)                                                  \
	X(ChatNewUser, "OnPluginChatNewUser", Deferrable)                                                        \
	X(ChatUserDisconnect, "OnPluginChatUserDisconnect", Deferrable)                                          \
	X(Close, "OnPluginClose", InputCritical)                                                                 \
	X(Command, "OnPluginCommand", InputCritical)                                                             \
	X(CommandChanged, "OnPluginCommandChanged", Deferrable)                                                  \
	X(CommandEntered, "OnPluginCommandEntered", InputCritical)                                               \
	X(Connect, "OnPluginConnect", Deferrable)                                                                \
	X(Disable, "OnPluginDisable", Deferrable)                                                                \
	X(Disconnect, "OnPluginDisconnect", Deferrable)                                                          \
	X(DrawOutputWindow, "OnPluginDrawOutputWindow", Deferrable)                                              \
	X(Enable, "OnPluginEnable", Deferrable)                                                                  \
	X(GetFocus, "OnPluginGetFocus", Deferrable)                                                              \
	X(IacGa, "OnPluginIacGa", TelnetNotification)                                                            \
	X(Install, "OnPluginInstall", Deferrable)                                                                \
	X(LineReceived, "OnPluginLineReceived", InputCritical)                                                   \
	X(ListChanged, "OnPluginListChanged", Deferrable)                                                        \
	X(LoseFocus, "OnPluginLoseFocus", Deferrable)                                                            \
	X(MouseMoved, "OnPluginMouseMoved", Deferrable)                                                          \
	X(MxpCloseTag, "OnPluginMXPcloseTag", InputCritical)                                                     \
	X(MxpError, "OnPluginMXPerror", Deferrable)                                                              \
	X(MxpOpenTag, "OnPluginMXPopenTag", InputCritical)                                                       \
	X(MxpSetEntity, "OnPluginMXPsetEntity", InputCritical)                                                   \
	X(MxpSetVariable, "OnPluginMXPsetVariable", InputCritical)                                               \
	X(MxpStart, "OnPluginMXPstart", Deferrable)                                                              \
	X(MxpStop, "OnPluginMXPstop", Deferrable)                                                                \
	X(PacketReceived, "OnPluginPacketReceived", InputCritical)                                               \
	X(PartialLine, "OnPluginPartialLine", Deferrable)                                                        \
	X(PlaySound, "OnPluginPlaySound", Deferrable)                                                            \
	X(SaveState, "OnPluginSaveState", InputCritical)                                                         \
	X(ScreenDraw, "OnPluginScreendraw", Deferrable)                                                          \
	X(SelectionChanged, "OnPluginSelectionChanged", Deferrable)                                              \
	X(Send, "OnPluginSend", InputCritical)                                                                   \
	X(Sent, "OnPluginSent", InputCritical)                                                                   \
	X(TabComplete, "OnPluginTabComplete", InputCritical)                                                     \
	X(TelnetOption, "OnPluginTelnetOption", TelnetNotification)                                              \
	X(TelnetRequest, "OnPluginTelnetRequest", InputCritical)                                                 \
	X(TelnetSubnegotiation, "OnPluginTelnetSubnegotiation", TelnetNotification)                              \
	X(Tick, "OnPluginTick", Deferrable)                                                                      \
	X(Trace, "OnPluginTrace", Deferrable)                                                                    \
	X(PacketDebug, "OnPluginPacketDebug", Deferrable)                                                        \
	X(WorldOutputResized, "OnPluginWorldOutputResized", Deferrable)                                          \
	X(WorldSave, "OnPluginWorldSave", Deferrable)

enum class LuaCallbackEvent : quint8
{
#define QMUD_DECLARE_LUA_CALLBACK_EVENT(name, functionName, orderingDomain) name,
	QMUD_LUA_CALLBACK_EVENT_REGISTRY(QMUD_DECLARE_LUA_CALLBACK_EVENT)
#undef QMUD_DECLARE_LUA_CALLBACK_EVENT
	    Count
};

inline constexpr std::size_t kLuaCallbackEventCount = static_cast<std::size_t>(LuaCallbackEvent::Count);

inline constexpr std::array<LuaCallbackOrderingDomain, kLuaCallbackEventCount>
    kLuaCallbackEventOrderingDomains = {
#define QMUD_APPEND_LUA_CALLBACK_EVENT_ORDERING_DOMAIN(name, functionName, orderingDomain)                   \
	LuaCallbackOrderingDomain::orderingDomain,
        QMUD_LUA_CALLBACK_EVENT_REGISTRY(QMUD_APPEND_LUA_CALLBACK_EVENT_ORDERING_DOMAIN)
#undef QMUD_APPEND_LUA_CALLBACK_EVENT_ORDERING_DOMAIN
};

inline const std::array<QString, kLuaCallbackEventCount> kLuaCallbackEventFunctionNames = {
#define QMUD_APPEND_LUA_CALLBACK_EVENT_FUNCTION_NAME(name, functionName, orderingDomain)                     \
	QStringLiteral(functionName),
    QMUD_LUA_CALLBACK_EVENT_REGISTRY(QMUD_APPEND_LUA_CALLBACK_EVENT_FUNCTION_NAME)
#undef QMUD_APPEND_LUA_CALLBACK_EVENT_FUNCTION_NAME
};

[[nodiscard]] constexpr LuaCallbackOrderingDomain
luaCallbackEventOrderingDomain(const LuaCallbackEvent event) noexcept
{
	const auto index = static_cast<std::size_t>(event);
	return index < kLuaCallbackEventOrderingDomains.size() ? kLuaCallbackEventOrderingDomains[index]
	                                                       : LuaCallbackOrderingDomain::Deferrable;
}

[[nodiscard]] inline QString luaCallbackEventFunctionName(const LuaCallbackEvent event)
{
	const auto index = static_cast<std::size_t>(event);
	return index < kLuaCallbackEventFunctionNames.size() ? kLuaCallbackEventFunctionNames[index] : QString();
}

[[nodiscard]] inline const QStringList &luaPluginCallbackFunctionNames()
{
	static const QStringList names = []
	{
		QStringList result;
		result.reserve(static_cast<qsizetype>(kLuaCallbackEventFunctionNames.size()));
		for (const QString &name : kLuaCallbackEventFunctionNames)
		{
			if (!name.isEmpty())
				result.push_back(name);
		}
		return result;
	}();
	return names;
}

#undef QMUD_LUA_CALLBACK_EVENT_REGISTRY

/**
 * @brief Output-line snapshot depth requested by a callback dispatch.
 */
enum class LuaCallbackLineSnapshotPolicy
{
	None,
	CountAndLast,
	CountAndRecentText,
	CountAndRecent,
	Full
};

/**
 * @brief Exact miniwindow resource identity used by callback snapshots and caches.
 */
struct LuaCallbackMiniWindowResourceKey
{
		QString       windowName;
		QString       resourceId;

		bool          operator==(const LuaCallbackMiniWindowResourceKey &) const = default;
		friend size_t qHash(const LuaCallbackMiniWindowResourceKey &key, size_t seed = 0) noexcept
		{
			seed = ::qHash(key.windowName, seed);
			return ::qHash(key.resourceId, seed);
		}
};

/**
 * @brief Exact miniwindow identity paired with a WindowInfo selector.
 */
struct LuaCallbackMiniWindowInfoKey
{
		QString       windowName;
		int           infoType{0};

		bool          operator==(const LuaCallbackMiniWindowInfoKey &) const = default;
		friend size_t qHash(const LuaCallbackMiniWindowInfoKey &key, size_t seed = 0) noexcept
		{
			seed = ::qHash(key.windowName, seed);
			return ::qHash(key.infoType, seed);
		}
};

/**
 * @brief Exact miniwindow/font identity paired with a WindowFontInfo selector.
 */
struct LuaCallbackMiniWindowFontInfoKey
{
		QString       windowName;
		QString       fontId;
		int           infoType{0};

		bool          operator==(const LuaCallbackMiniWindowFontInfoKey &) const = default;
		friend size_t qHash(const LuaCallbackMiniWindowFontInfoKey &key, size_t seed = 0) noexcept
		{
			seed = ::qHash(key.windowName, seed);
			seed = ::qHash(key.fontId, seed);
			return ::qHash(key.infoType, seed);
		}
};

/**
 * @brief Exact miniwindow/font/text identity used by callback text-width caches.
 */
struct LuaCallbackMiniWindowTextWidthKey
{
		QString       windowName;
		QString       fontId;
		QString       text;

		bool          operator==(const LuaCallbackMiniWindowTextWidthKey &) const = default;
		friend size_t qHash(const LuaCallbackMiniWindowTextWidthKey &key, size_t seed = 0) noexcept
		{
			seed = ::qHash(key.windowName, seed);
			seed = ::qHash(key.fontId, seed);
			return ::qHash(key.text, seed);
		}
};

/**
 * @brief Immutable callback request view assembled from one runtime-owned stable base.
 *
 * WorldRuntime owns and patches a private base on its object thread. Every dispatch receives a
 * top-level copy; Qt containers stay shallow until either copy is written, so later base patches
 * detach and cannot alter an in-flight worker view. Stable fields are patched at their mutation
 * source. Dispatch-local fields are cleared from the cached base and populated after each clone.
 * Callback-local read-your-writes live in LuaCallbackEngine overlays rather than this shared
 * snapshot.
 *
 * Extension contract for a new callback-visible field:
 * 1. Classify it as stable, dispatch-local/asynchronously sampled, or callback-local.
 * 2. Put stable state in the domain-state type selected by LuaCallbackStableSnapshotDomain, or add a new
 *    entry to its single registry. The aggregate inherits every registered state, and causal replay verifies
 *    the complete selected state against the captured revision. Mark the entry plugin-indexed when plugin load/unload changes the
 *    field; a Plugins patch then expands mechanically, with no second dependent-domain list. Put cold
 *    construction and patch refresh in the same WorldRuntime population helper and handle the domain in
 *    populateLuaCallbackStableSnapshotDomains(), the sole domain-to-population registry used by both
 *    cold construction and patching; masks are derived mechanically from the enum. Call
 *    patchLuaCallbackStableSnapshot() from every authoritative mutation source. Plugin-scoped rule
 *    collections use the scoped overload of that same patch function, never a parallel registry.
 * 3. If Lua can mutate it, also journal the mutation and provide a LuaCallbackEngine overlay when
 *    the writing callback must read its own uncommitted value (which is 99% of the time). A field
 *    indexed by plugin id must also be handled by markCallbackPluginTopologyUnavailable() and
 *    reconcileCallbackPluginTopology(), the two ends of the single callback-local lifecycle overlay;
 *    do not add an API-specific unload/reload fanout. Plugin ids are canonicalized only by XML validation and
 *    plugin-facing Lua API argument decoders that actually accept a plugin id. Runtime service methods, metadata,
 *    journals, snapshots, overlays, and cache keys must copy the canonical id unchanged; they must not normalize
 *    or independently validate its spelling.
 * 4. For dispatch-local/sampled state, populate it only after cloning and clear it from the cached
 *    base in clearLuaCallbackDispatchVolatileSnapshot(); do not add an incidental stable-base patch.
 * 5. Cover cold construction, cache-hit visibility, immutability of an issued snapshot, and later
 *    recipient/nested-callback visibility with tests. A new field is incomplete until these paths
 *    agree.
 */
#define QMUD_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN_REGISTRY(X)                                                 \
	X(WorldVariables, false, false, LuaCallbackWorldVariablesSnapshotState)                                  \
	X(PluginVariables, true, true, LuaCallbackPluginVariablesSnapshotState)                                  \
	X(Triggers, true, true, LuaCallbackTriggersSnapshotState)                                                \
	X(Aliases, true, true, LuaCallbackAliasesSnapshotState)                                                  \
	X(Timers, true, true, LuaCallbackTimersSnapshotState)                                                    \
	X(WorldAttributes, false, false, LuaCallbackWorldAttributesSnapshotState)                                \
	X(Arrays, false, false, LuaCallbackArraysSnapshotState)                                                  \
	X(Colours, false, false, LuaCallbackColoursSnapshotState)                                                \
	X(CustomElements, false, false, LuaCallbackCustomElementsSnapshotState)                                  \
	X(Mapper, false, false, LuaCallbackMapperSnapshotState)                                                  \
	X(Databases, false, false, LuaCallbackDatabasesSnapshotState)                                            \
	X(Macros, false, false, LuaCallbackMacrosSnapshotState)                                                  \
	X(Keypad, false, false, LuaCallbackKeypadSnapshotState)                                                  \
	X(Accelerators, false, false, LuaCallbackAcceleratorsSnapshotState)                                      \
	X(Plugins, false, false, LuaCallbackPluginsSnapshotState)                                                \
	X(MiniWindows, false, false, LuaCallbackMiniWindowsSnapshotState)                                        \
	X(TriggerWildcards, true, false, LuaCallbackTriggerWildcardsSnapshotState)                               \
	X(AliasWildcards, true, false, LuaCallbackAliasWildcardsSnapshotState)

enum class LuaCallbackStableSnapshotDomain : quint8
{
#define QMUD_DECLARE_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN(name, pluginIndexed, pluginInfoCoupled, stateType)  \
	name,
	QMUD_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN_REGISTRY(QMUD_DECLARE_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN)
#undef QMUD_DECLARE_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN
	    Count
};

/** @brief Explicit extent of one scoped stable-snapshot patch. */
enum class LuaCallbackStableSnapshotPatchKind : quint8
{
	Owner,
	Item,
	Member
};

/**
 * @brief Exact stable snapshot mutation carried by a causal callback revision.
 */
struct LuaCallbackStableCausalPatch
{
		QString                            pluginId;
		QString                            itemName;
		QString                            memberName;
		bool                               topologyMayChange{false};
		LuaCallbackStableSnapshotPatchKind kind{LuaCallbackStableSnapshotPatchKind::Owner};
		bool                               operator==(const LuaCallbackStableCausalPatch &other) const
		{
			return pluginId == other.pluginId && itemName == other.itemName &&
			       memberName == other.memberName && kind == other.kind;
		}

		friend size_t qHash(const LuaCallbackStableCausalPatch &patch, const size_t seed = 0) noexcept
		{
			return qHashMulti(seed, patch.pluginId, patch.itemName, patch.memberName, patch.kind);
		}
};

/** @brief Independently stored portions of a callback rule snapshot row. */
enum class LuaCallbackRuleSnapshotPart : quint8
{
	Definition   = 0x01,
	RuntimeState = 0x02,
	DetailState  = 0x04
};
Q_DECLARE_FLAGS(LuaCallbackRuleSnapshotParts, LuaCallbackRuleSnapshotPart)
Q_DECLARE_OPERATORS_FOR_FLAGS(LuaCallbackRuleSnapshotParts)

/** @return Every independently stored callback rule snapshot portion. */
[[nodiscard]] constexpr LuaCallbackRuleSnapshotParts luaCallbackAllRuleSnapshotParts() noexcept
{
	return LuaCallbackRuleSnapshotPart::Definition | LuaCallbackRuleSnapshotPart::RuntimeState |
	       LuaCallbackRuleSnapshotPart::DetailState;
}

/**
 * @brief Component changes for one generation-checked callback rule.
 */
struct LuaCallbackRuleSnapshotChange
{
		quint64                      runtimeLocationHandle{0};
		quint64                      runtimeId{0};
		int                          indexHint{-1};
		LuaCallbackRuleSnapshotParts parts{};
};

/**
 * @brief Exact component patch for one trigger, alias, or timer snapshot row.
 *
 * The complete runtime-location handle is both the coalescing key and generation check. The runtime id independently
 * verifies the resolved rule, while the index hint accelerates lookup without defining identity.
 */
struct LuaCallbackTargetedRulePatch
{
		QString                      pluginId;
		quint64                      runtimeLocationHandle{0};
		quint64                      runtimeId{0};
		int                          indexHint{-1};
		LuaCallbackRuleSnapshotParts parts{};
};

using LuaCallbackTargetedRulePatches = QHash<quint64, LuaCallbackTargetedRulePatch>;

[[nodiscard]] constexpr quint32
luaCallbackStableSnapshotDomainBit(const LuaCallbackStableSnapshotDomain domain)
{
	return quint32{1} << static_cast<quint32>(domain);
}

[[nodiscard]] constexpr quint32 luaCallbackAllStableSnapshotDomainsMask()
{
	static_assert(static_cast<quint32>(LuaCallbackStableSnapshotDomain::Count) < 32U);
	return luaCallbackStableSnapshotDomainBit(LuaCallbackStableSnapshotDomain::Count) - 1U;
}

[[nodiscard]] constexpr quint32 luaCallbackPluginIndexedStableSnapshotDomainsMask()
{
	return 0U
#define QMUD_PLUGIN_INDEXED_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN(name, pluginIndexed, pluginInfoCoupled,      \
                                                                stateType)                                   \
	| (pluginIndexed ? luaCallbackStableSnapshotDomainBit(LuaCallbackStableSnapshotDomain::name) : 0U)
	    QMUD_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN_REGISTRY(
	        QMUD_PLUGIN_INDEXED_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN)
#undef QMUD_PLUGIN_INDEXED_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN
	        ;
}

[[nodiscard]] constexpr quint32 luaCallbackCausalStableSnapshotDomainMask(const quint32 domainMask)
{
	quint32 expandedMask = domainMask;
	if ((domainMask & luaCallbackStableSnapshotDomainBit(LuaCallbackStableSnapshotDomain::Plugins)) != 0U)
		expandedMask |= luaCallbackPluginIndexedStableSnapshotDomainsMask();
#define QMUD_PLUGIN_INFO_COUPLED_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN(name, pluginIndexed, pluginInfoCoupled, \
                                                                     stateType)                              \
	if (pluginInfoCoupled &&                                                                                 \
	    (domainMask & luaCallbackStableSnapshotDomainBit(LuaCallbackStableSnapshotDomain::name)) != 0U)      \
		expandedMask |= luaCallbackStableSnapshotDomainBit(LuaCallbackStableSnapshotDomain::Plugins);
	QMUD_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN_REGISTRY(
	    QMUD_PLUGIN_INFO_COUPLED_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN)
#undef QMUD_PLUGIN_INFO_COUPLED_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN
	return expandedMask;
}

struct LuaCallbackWorldVariablesSnapshotState
{
		bool                                       hasWorldVariablesSnapshot{false};
		QMap<QString, QString>                     worldVariablesSnapshot;
		bool                                       hasVariableEntriesSnapshot{false};
		QList<LuaCallbackAttributeContentSnapshot> variableEntriesSnapshot;
		bool operator==(const LuaCallbackWorldVariablesSnapshotState &) const = default;
};

struct LuaCallbackPluginVariablesSnapshotState
{
		QHash<QString, QMap<QString, QString>> pluginVariablesSnapshotById;
		QSet<QString>                          unavailablePluginVariableSnapshotIds;
		bool operator==(const LuaCallbackPluginVariablesSnapshotState &) const = default;
};

struct LuaCallbackTriggersSnapshotState
{
		LuaCallbackRuleSnapshotStore<LuaCallbackTriggerSnapshot> triggerSnapshotStore;
		QHash<QString, QList<LuaCallbackTriggerSnapshot>>        triggerListOverridesByPluginId;
		QSet<QString>                                            missingTriggerListPluginIds;
		bool operator==(const LuaCallbackTriggersSnapshotState &) const = default;
};

struct LuaCallbackAliasesSnapshotState
{
		LuaCallbackRuleSnapshotStore<LuaCallbackAliasSnapshot> aliasSnapshotStore;
		QHash<QString, QList<LuaCallbackAliasSnapshot>>        aliasListOverridesByPluginId;
		QSet<QString>                                          missingAliasListPluginIds;
		bool operator==(const LuaCallbackAliasesSnapshotState &) const = default;
};

struct LuaCallbackTimersSnapshotState
{
		LuaCallbackRuleSnapshotStore<LuaCallbackTimerSnapshot> timerSnapshotStore;
		QHash<QString, QList<LuaCallbackTimerSnapshot>>        timerListOverridesByPluginId;
		QSet<QString>                                          missingTimerListPluginIds;
		bool operator==(const LuaCallbackTimersSnapshotState &) const = default;
};

struct LuaCallbackWorldAttributesSnapshotState
{
		bool                   hasWorldAttributeSnapshot{false};
		QMap<QString, QString> worldAttributesSnapshot;
		QMap<QString, QString> worldMultilineAttributesSnapshot;
		bool                   operator==(const LuaCallbackWorldAttributesSnapshotState &) const = default;
};

/** @return Process-unique nonzero runtime identity for a scripting array instance. */
[[nodiscard]] inline quint64 nextLuaCallbackArrayRuntimeId() noexcept
{
	static std::atomic<quint64> nextId{1};
	quint64                     result = nextId.fetch_add(1, std::memory_order_relaxed);
	if (result == 0)
		result = nextId.fetch_add(1, std::memory_order_relaxed);
	return result;
}

/**
 * @brief Immutable callback view of one scripting array.
 *
 * Values use a persistent hash tree, so replacing or removing one key copies a fixed hash path while every
 * unaffected key remains shared with previously issued callback snapshots.
 */
struct LuaCallbackArraySnapshot
{
		quint64                                     runtimeId{0};
		LuaCallbackPersistentStringMap<QString>     values;

		/** @return Value for @p key, or an empty pointer when absent. */
		[[nodiscard]] QSharedPointer<const QString> value(const QString &key) const
		{
			return values.value(key);
		}
		/** @return Whether @p key exists. */
		[[nodiscard]] bool contains(const QString &key) const
		{
			return values.contains(key);
		}
		/** @return Number of values. */
		[[nodiscard]] qsizetype size() const noexcept
		{
			return values.size();
		}
		/** @return Whether the array is empty. */
		[[nodiscard]] bool isEmpty() const noexcept
		{
			return values.isEmpty();
		}
		/** @return Keys in unspecified order without materializing the value map. */
		[[nodiscard]] QStringList keys() const
		{
			return values.keys();
		}
		/** Replaces one value and returns its prior state. */
		[[nodiscard]] LuaCallbackPersistentStringMap<QString>::MutationResult setValue(const QString &key,
		                                                                               const QString &value)
		{
			auto transaction = LuaCallbackPersistentStringMap<QString>::Transaction(values);
			auto result      = transaction.apply({key, QSharedPointer<const QString>::create(value)});
			values           = std::move(transaction).finish();
			return result;
		}
		/** Removes one value and returns its prior state. */
		[[nodiscard]] LuaCallbackPersistentStringMap<QString>::MutationResult removeValue(const QString &key)
		{
			auto transaction = LuaCallbackPersistentStringMap<QString>::Transaction(values);
			auto result      = transaction.apply({key, {}});
			values           = std::move(transaction).finish();
			return result;
		}
		/** Removes every value from this mutable snapshot view. */
		void clear()
		{
			values = {};
		}
		/** @return Values materialized in the scripting API's sorted key order. */
		[[nodiscard]] QMap<QString, QString> materializedValues() const
		{
			QMap<QString, QString> result;
			values.forEach([&result](const QString &key, const QSharedPointer<const QString> &entry)
			               { result.insert(key, *entry); });
			return result;
		}

		bool operator==(const LuaCallbackArraySnapshot &) const = default;
};

/** @brief One ordered key replacement in an array batch mutation. */
struct LuaCallbackArrayValueMutation
{
		QString key;
		QString value;
};

/**
 * @brief Exact array operation carried by one causal callback revision.
 */
struct LuaCallbackArrayMutation
{
		enum class Kind : quint8
		{
			Create,
			Delete,
			Clear,
			Set,
			DeleteKey,
			SetMany
		};

		LuaCallbackArrayMutation() = default;
		LuaCallbackArrayMutation(Kind mutationKind, QString name, quint64 arrayRuntimeId, QString memberKey,
		                         QString memberValue, QStringList removedKeys,
		                         QVector<LuaCallbackArrayValueMutation> batchValues = {})
		    : kind(mutationKind), arrayName(std::move(name)), runtimeId(arrayRuntimeId),
		      key(std::move(memberKey)), value(std::move(memberValue)), clearedKeys(std::move(removedKeys)),
		      values(std::move(batchValues))
		{
		}

		Kind                                   kind{Kind::Set};
		QString                                arrayName;
		quint64                                runtimeId{0};
		QString                                key;
		QString                                value;
		QStringList                            clearedKeys;
		QVector<LuaCallbackArrayValueMutation> values;
};

struct LuaCallbackArraysSnapshotState
{
		bool                                                     hasArraySnapshot{false};
		QStringList                                              arrayNamesSnapshot;
		LuaCallbackPersistentStringMap<LuaCallbackArraySnapshot> arraysByName;
		bool operator==(const LuaCallbackArraysSnapshotState &) const = default;
};

struct LuaCallbackColoursSnapshotState
{
		QHash<int, long>    boldAnsiColoursByIndex;
		QHash<int, long>    normalAnsiColoursByIndex;
		QHash<int, long>    customTextColoursByIndex;
		QHash<int, long>    customBackgroundColoursByIndex;
		QHash<int, QString> customColourNamesByIndex;
		bool                operator==(const LuaCallbackColoursSnapshotState &) const = default;
};

struct LuaCallbackCustomElementSnapshotRow
{
		TelnetProcessor::CustomElementInfo info;
		quint64                            revision{0};
		bool operator==(const LuaCallbackCustomElementSnapshotRow &) const = default;
};

struct LuaCallbackCustomElementsSnapshotState
{
		QMap<QByteArray, LuaCallbackCustomElementSnapshotRow> customElementsSnapshot;
		bool operator==(const LuaCallbackCustomElementsSnapshotState &) const = default;
};

struct LuaCallbackMapperSnapshotState
{
		bool             hasMapColourSnapshot{false};
		QMap<long, long> mapColourSnapshot;
		bool             hasMappingEntriesSnapshot{false};
		QStringList      mappingEntriesSnapshot;
		bool             operator==(const LuaCallbackMapperSnapshotState &) const = default;
};

struct LuaCallbackDatabasesSnapshotState
{
		bool                                            hasDatabaseListSnapshot{false};
		bool                                            databaseListSnapshotDirty{false};
		QStringList                                     databaseNamesSnapshot;
		bool                                            hasDatabaseSnapshot{false};
		QHash<QString, LuaCallbackDatabaseSnapshot>     databaseSnapshotsByName;
		QHash<QString, int>                             databaseColumnsByName;
		QHash<QString, QString>                         databaseErrorsByName;
		QHash<QString, LuaCallbackDatabaseIndexedCache> databaseIndexedCachesByName;
		QHash<QString, QStringList>                     databaseColumnNamesByName;
		QSet<QString>                                   missingDatabaseColumnNamesByName;
		QHash<QString, QVector<QVariant>>               databaseColumnValuesByName;
		QSet<QString>                                   missingDatabaseColumnValuesByName;
		QHash<QString, int>                             databaseTotalChangesByName;
		QHash<QString, int>                             databaseChangesByName;
		QHash<QString, QString>                         databaseLastInsertRowidByName;
		bool operator==(const LuaCallbackDatabasesSnapshotState &) const = default;
};

struct LuaCallbackMacrosSnapshotState
{
		bool                                        hasMacroEntriesSnapshot{false};
		QList<LuaCallbackAttributeChildrenSnapshot> macroEntriesSnapshot;
		bool operator==(const LuaCallbackMacrosSnapshotState &) const = default;
};

struct LuaCallbackKeypadSnapshotState
{
		bool                                       hasKeypadEntriesSnapshot{false};
		QList<LuaCallbackAttributeContentSnapshot> keypadEntriesSnapshot;
		bool operator==(const LuaCallbackKeypadSnapshotState &) const = default;
};

struct LuaCallbackAcceleratorsSnapshotState
{
		bool                                    hasAcceleratorSnapshot{false};
		QVector<LuaCallbackAcceleratorSnapshot> acceleratorSnapshot;
		bool operator==(const LuaCallbackAcceleratorsSnapshotState &) const = default;
};

struct LuaCallbackPluginsSnapshotState
{
		struct RuntimeIdentity
		{
				quint64 runtimeLocationHandle{0};
				quint64 runtimeInstanceId{0};
				quint64 engineInstanceId{0};
				bool    operator==(const RuntimeIdentity &) const = default;
		};

		QHash<QString, QString>                           pluginNamesById;
		QHash<QString, QString>                           pluginDirectoriesById;
		QHash<QString, bool>                              pluginEnabledById;
		QHash<QString, bool>                              pluginInstallPendingById;
		QHash<QString, bool>                              pluginGlobalById;
		QHash<QString, bool>                              nativePluginSpeechEnabledById;
		QHash<QString, QSharedPointer<LuaCallbackEngine>> pluginEnginesById;
		QHash<QString, RuntimeIdentity>                   pluginRuntimeIdentitiesById;
		QHash<QString, QSet<QString>>                     pluginLuaFunctionsById;
		QStringList                                       broadcastPluginIdsSnapshot;
		bool                                              hasBroadcastPluginSnapshot{false};
		QStringList                                       pluginIdsSnapshot;
		QHash<QString, QString>                           pluginIdsByLookupKey;
		QHash<QString, QHash<int, QVariant>>              pluginInfoValuesById;
		QHash<QString, bool>                              pluginCallbackPresenceByName;
		bool operator==(const LuaCallbackPluginsSnapshotState &) const = default;
};

/**
 * @brief One accepted callback-local mapper operation replayed into causally later snapshots.
 */
struct LuaCallbackMapperSnapshotOperation
{
		enum class Kind
		{
			Append,
			RemoveLast,
			Clear,
			SetEnabled
		};

		Kind    kind{Kind::Append};
		QString entry;
		bool    enabled{false};
};

/**
 * @brief Causal handling policy for one callback-visible runtime value.
 */
enum class LuaCallbackRuntimeCounterPolicy : quint8
{
	Causal,
	Derived,
	DispatchContext,
	External
};

/**
 * @brief Authoritative registry of callback-visible runtime values.
 *
 * Each entry supplies the enum name, snapshot member, value type, Lua snapshot key, and causal policy. Values
 * marked `Causal` are diffed once per completed causal mutation batch. Dispatch-context values are supplied by
 * the callback request, while external values are sampled independently of callback mutation playback.
 */
#define QMUD_LUA_CALLBACK_RUNTIME_COUNTER_FIELDS(X)                                                          \
	X(NewLines, newLines, int, "newLines", Causal)                                                           \
	X(TotalLinesSent, totalLinesSent, int, "totalLinesSent", Causal)                                         \
	X(InputPacketCount, inputPacketCount, int, "inputPacketCount", External)                                 \
	X(OutputPacketCount, outputPacketCount, int, "outputPacketCount", Causal)                                \
	X(TotalUncompressedBytes, totalUncompressedBytes, qint64, "totalUncompressedBytes", External)            \
	X(TotalCompressedBytes, totalCompressedBytes, qint64, "totalCompressedBytes", External)                  \
	X(MccpType, mccpType, int, "mccpType", External)                                                         \
	X(MxpErrorCount, mxpErrorCount, int, "mxpErrorCount", External)                                          \
	X(MxpTagCount, mxpTagCount, qint64, "mxpTagCount", External)                                             \
	X(MxpEntityCount, mxpEntityCount, qint64, "mxpEntityCount", External)                                    \
	X(BytesIn, bytesIn, qint64, "bytesIn", External)                                                         \
	X(BytesOut, bytesOut, qint64, "bytesOut", Causal)                                                        \
	X(TotalLinesReceived, totalLinesReceived, int, "totalLinesReceived", Causal)                             \
	X(OutputFontHeight, outputFontHeight, int, "outputFontHeight", Causal)                                   \
	X(OutputFontWidth, outputFontWidth, int, "outputFontWidth", Causal)                                      \
	X(InputFontHeight, inputFontHeight, int, "inputFontHeight", Causal)                                      \
	X(InputFontWidth, inputFontWidth, int, "inputFontWidth", Causal)                                         \
	X(VariableCount, variableCount, int, "variableCount", Derived)                                           \
	X(TriggerCount, triggerCount, int, "triggerCount", Derived)                                              \
	X(TimerCount, timerCount, int, "timerCount", Derived)                                                    \
	X(AliasCount, aliasCount, int, "aliasCount", Derived)                                                    \
	X(QueuedCommandCount, queuedCommandCount, int, "queuedCommandCount", Causal)                             \
	X(MappingCount, mappingCount, int, "mappingCount", Derived)                                              \
	X(OutputLineCount, outputLineCount, int, "outputLineCount", Causal)                                      \
	X(CustomElementCount, customElementCount, int, "customElementCount", Derived)                            \
	X(CustomEntityCount, customEntityCount, int, "customEntityCount", Derived)                               \
	X(ConnectPhase, connectPhase, int, "connectPhase", Causal)                                               \
	X(PeerAddressV4, peerAddressV4, quint32, "peerAddressV4", Causal)                                        \
	X(ProxyAddressV4, proxyAddressV4, quint32, "proxyAddressV4", Causal)                                     \
	X(LogFilePosition, logFilePosition, qint64, "logFilePosition", Causal)                                   \
	X(TriggerTimeSeconds, triggerTimeSeconds, double, "triggerTimeSeconds", Causal)                          \
	X(AliasTimeSeconds, aliasTimeSeconds, double, "aliasTimeSeconds", Causal)                                \
	X(ScriptTimeSeconds, scriptTimeSeconds, double, "scriptTimeSeconds", External)                           \
	X(NoCommandEcho, noCommandEcho, bool, "noCommandEcho", Causal)                                           \
	X(DebugIncomingPackets, debugIncomingPackets, bool, "debugIncomingPackets", Causal)                      \
	X(IsCompressing, isCompressing, bool, "isCompressing", Causal)                                           \
	X(IsMxpActive, isMxpActive, bool, "isMxpActive", Causal)                                                 \
	X(IsPuebloActive, isPuebloActive, bool, "isPuebloActive", Causal)                                        \
	X(RemoveMapReverses, removeMapReverses, bool, "removeMapReverses", Causal)                               \
	X(NotesInRgb, notesInRgb, bool, "notesInRgb", Causal)                                                    \
	X(DisconnectOk, disconnectOk, bool, "disconnectOk", Causal)                                              \
	X(TraceEnabled, traceEnabled, bool, "traceEnabled", Causal)                                              \
	X(IsLogOpen, isLogOpen, bool, "isLogOpen", Causal)                                                       \
	X(ScriptFileChanged, scriptFileChanged, bool, "scriptFileChanged", Causal)                               \
	X(WorldFileModified, worldFileModified, bool, "worldFileModified", Causal)                               \
	X(IsMapping, isMapping, bool, "isMapping", Causal)                                                       \
	X(IsActive, isActive, bool, "isActive", External)                                                        \
	X(OutputFrozen, outputFrozen, bool, "outputFrozen", Causal)                                              \
	X(VariablesChanged, variablesChanged, bool, "variablesChanged", Causal)                                  \
	X(DoingSimulate, doingSimulate, bool, "doingSimulate", DispatchContext)                                  \
	X(LineOmittedFromOutput, lineOmittedFromOutput, bool, "lineOmittedFromOutput", DispatchContext)          \
	X(HasLuaCallbacks, hasLuaCallbacks, bool, "hasLuaCallbacks", Causal)                                     \
	X(PluginProcessingSent, pluginProcessingSent, bool, "pluginProcessingSent", DispatchContext)             \
	X(IsChatAcceptingCalls, isChatAcceptingCalls, bool, "isChatAcceptingCalls", Causal)                      \
	X(NoteStyle, noteStyle, unsigned short, "noteStyle", Causal)                                             \
	X(NoteTextColour, noteTextColour, int, "noteTextColour", Causal)                                         \
	X(NoteColourBack, noteColourBack, long, "noteColourBack", Causal)                                        \
	X(NoteColourFore, noteColourFore, long, "noteColourFore", Causal)                                        \
	X(BackgroundColour, backgroundColour, long, "backgroundColour", Causal)                                  \
	X(Utf8ErrorCount, utf8ErrorCount, int, "utf8ErrorCount", External)                                       \
	X(TriggersEvaluatedCount, triggersEvaluatedCount, int, "triggersEvaluatedCount", Causal)                 \
	X(TriggersMatchedThisSession, triggersMatchedThisSession, int, "triggersMatchedThisSession", Causal)     \
	X(AliasesEvaluatedCount, aliasesEvaluatedCount, int, "aliasesEvaluatedCount", Causal)                    \
	X(AliasesMatchedThisSession, aliasesMatchedThisSession, int, "aliasesMatchedThisSession", Causal)        \
	X(TimersFiredThisSession, timersFiredThisSession, int, "timersFiredThisSession", Causal)                 \
	X(LastLineWithIacGa, lastLineWithIacGa, int, "lastLineWithIacGa", External)                              \
	X(OutputWindowRedrawCount, outputWindowRedrawCount, int, "outputWindowRedrawCount", Causal)              \
	X(CurrentActionSource, currentActionSource, int, "currentActionSource", DispatchContext)                 \
	X(NewlinesReceived, newlinesReceived, qint64, "newlinesReceived", Causal)                                \
	X(ConnectTime, connectTime, QDateTime, "connectTime", Causal)                                            \
	X(StatusTime, statusTime, QDateTime, "statusTime", Causal)                                               \
	X(LastFlushTime, lastFlushTime, QDateTime, "lastFlushTime", Causal)                                      \
	X(ClientStartTime, clientStartTime, QDateTime, "clientStartTime", External)                              \
	X(WorldStartTime, worldStartTime, QDateTime, "worldStartTime", External)                                 \
	X(ScriptFileModTime, scriptFileModTime, QDateTime, "scriptFileModTime", External)                        \
	X(LogFileName, logFileName, QString, "logFileName", Causal)                                              \
	X(LastImmediateExpression, lastImmediateExpression, QString, "lastImmediateExpression", Causal)          \
	X(StatusMessage, statusMessage, QString, "statusMessage", Causal)                                        \
	X(WorldFilePath, worldFilePath, QString, "worldFilePath", Causal)                                        \
	X(WindowTitleOverride, windowTitleOverride, QString, "windowTitleOverride", Causal)                      \
	X(MainTitleOverride, mainTitleOverride, QString, "mainTitleOverride", Causal)                            \
	X(DefaultWorldDirectory, defaultWorldDirectory, QString, "defaultWorldDirectory", Causal)                \
	X(DefaultLogDirectory, defaultLogDirectory, QString, "defaultLogDirectory", Causal)                      \
	X(PluginsDirectory, pluginsDirectory, QString, "pluginsDirectory", Causal)                               \
	X(PeerAddressString, peerAddressString, QString, "peerAddressString", Causal)                            \
	X(ProxyAddressString, proxyAddressString, QString, "proxyAddressString", Causal)                         \
	X(StartupDirectory, startupDirectory, QString, "startupDirectory", Causal)                               \
	X(TranslatorFile, translatorFile, QString, "translatorFile", Causal)                                     \
	X(Locale, locale, QString, "locale", Causal)                                                             \
	X(FixedPitchFont, fixedPitchFont, QString, "fixedPitchFont", Causal)                                     \
	X(LastTelnetSubnegotiation, lastTelnetSubnegotiation, QString, "lastTelnetSubnegotiation", External)     \
	X(FirstSpecialFontPath, firstSpecialFontPath, QString, "firstSpecialFontPath", Causal)                   \
	X(PreferencesDatabaseName, preferencesDatabaseName, QString, "preferencesDatabaseName", Causal)          \
	X(FileBrowsingDirectory, fileBrowsingDirectory, QString, "fileBrowsingDirectory", Causal)                \
	X(StateFilesDirectory, stateFilesDirectory, QString, "stateFilesDirectory", Causal)                      \
	X(LastCommandSent, lastCommandSent, QString, "lastCommandSent", Causal)

/**
 * @brief Coherent command-window and output-presentation state sampled for one callback dispatch.
 */
struct LuaCallbackCommandUiSnapshotState
{
		QStringList queuedCommands;
		QString     commandInputText;
		QStringList commandHistory;
		int         triggerPriorityQueuedCommandCount{0};
		int         inputSelectionStartColumn{0};
		int         inputSelectionEndColumn{0};
		int         outputSelectionEndColumn{0};
		int         outputSelectionEndLine{0};
		int         outputSelectionStartColumn{0};
		int         outputSelectionStartLine{0};
		int         textRectangleLeft{0};
		int         textRectangleTop{0};
		int         textRectangleRight{0};
		int         textRectangleBottom{0};
		int         textRectangleBorderOffset{0};
		int         textRectangleBorderWidth{0};
		int         textRectangleOutsideFillColour{0};
		int         textRectangleOutsideFillStyle{0};
		int         textRectangleBorderColour{0};
		bool        hasView{false};
		bool        hasFrameData{false};
		bool        outputScrollBarWanted{true};
		int         outputScrollPosition{0};
		int         outputClientHeight{0};
		int         outputClientWidth{0};
		int         viewHeight{0};
		int         viewWidth{0};
		int         outputTextRectLeft{0};
		int         outputTextRectTop{0};
		int         outputTextRectRight{0};
		int         outputTextRectBottom{0};
		bool        hasLastMousePosition{false};
		int         lastMouseX{-1};
		int         lastMouseY{-1};
		QString     selectedWord;
		bool        selectedWordResolved{false};
		bool        fullScreenMode{false};
		int         worldWindowCount{0};
		int         worldWindowShowCommand{kWindowShowNormal};
		int         mainClientHeight{0};
		int         mainClientWidth{0};
		int         mainToolbarHeight{0};
		int         mainToolbarWidth{0};
		int         worldToolbarHeight{0};
		int         worldToolbarWidth{0};
		int         activityToolbarHeight{0};
		int         activityToolbarWidth{0};
		int         infoBarHeight{0};
		int         infoBarWidth{0};
		int         statusBarHeight{0};
		int         statusBarWidth{0};
		int         worldChildWindowHeight{0};
		int         worldChildWindowWidth{0};
};

/**
 * @brief Coherent callback-visible runtime values sampled for one callback dispatch.
 */
struct LuaCallbackRuntimeCountersSnapshotState
{
#define QMUD_DECLARE_RUNTIME_COUNTER_MEMBER(enumName, member, type, key, policy) type member{};
		QMUD_LUA_CALLBACK_RUNTIME_COUNTER_FIELDS(QMUD_DECLARE_RUNTIME_COUNTER_MEMBER)
#undef QMUD_DECLARE_RUNTIME_COUNTER_MEMBER
};

/**
 * @brief Callback-visible runtime value identified by the authoritative runtime-value registry.
 */
enum class LuaCallbackRuntimeCounterField : quint8
{
#define QMUD_DECLARE_RUNTIME_COUNTER_ENUM(enumName, member, type, key, policy) enumName,
	QMUD_LUA_CALLBACK_RUNTIME_COUNTER_FIELDS(QMUD_DECLARE_RUNTIME_COUNTER_ENUM)
#undef QMUD_DECLARE_RUNTIME_COUNTER_ENUM
	    Count
};

/**
 * @brief Hashes a callback runtime-counter field.
 * @param field Field to hash.
 * @param seed Hash seed.
 * @return Hash value.
 */
[[nodiscard]] inline size_t qHash(const LuaCallbackRuntimeCounterField field, const size_t seed = 0) noexcept
{
	return ::qHash(static_cast<quint8>(field), seed);
}

/**
 * @brief Final callback-visible runtime assignments made by one accepted causal mutation batch.
 */
using LuaCallbackRuntimeCounterUpdates = QHash<LuaCallbackRuntimeCounterField, QVariant>;

/** @brief Final state of one custom MXP entity changed by an accepted causal mutation batch. */
struct LuaCallbackEntityMutation
{
		quint32    expectedSlot{0};
		quint32    resultSlot{0};
		QByteArray value;
		bool       present{false};
};

/**
 * @brief Final custom MXP entity assignments made by one accepted causal mutation batch.
 */
using LuaCallbackEntityMutations = QHash<QByteArray, LuaCallbackEntityMutation>;

/**
 * @brief Entity identities invalidated by reset boundaries in one causal mutation batch.
 */
using LuaCallbackEntityResetSlots = QHash<QByteArray, QSet<quint32>>;

/**
 * @brief Immutable value stored for one callback-visible custom MXP entity slot.
 */
struct LuaCallbackEntitySnapshotRow
{
		QByteArray value;
		bool       present{false};
		bool       operator==(const LuaCallbackEntitySnapshotRow &) const = default;
};

/**
 * @brief Identity and value override applied over an immutable entity snapshot.
 */
struct LuaCallbackEntitySnapshotOverride
{
		quint32    slot{0};
		QByteArray value;
		bool       present{false};
		bool       operator==(const LuaCallbackEntitySnapshotOverride &) const = default;
};

/**
 * @brief Derived miniwindow snapshot indexes affected by one window mutation.
 *
 * Every mutation still replaces the immutable MiniWindow snapshot. These flags restrict rebuilding the
 * independent lookup containers used by WindowInfo and the font, image, and hotspot list/existence APIs.
 */
enum class LuaCallbackMiniWindowSnapshotChange : quint8
{
	WindowInfo        = 0x01,
	Fonts             = 0x02,
	Images            = 0x04,
	HotspotMembership = 0x08,
	Membership        = 0x10
};
Q_DECLARE_FLAGS(LuaCallbackMiniWindowSnapshotChanges, LuaCallbackMiniWindowSnapshotChange)
Q_DECLARE_OPERATORS_FOR_FLAGS(LuaCallbackMiniWindowSnapshotChanges)

[[nodiscard]] constexpr LuaCallbackMiniWindowSnapshotChanges
luaCallbackAllMiniWindowSnapshotChanges() noexcept
{
	using Change = LuaCallbackMiniWindowSnapshotChange;
	return Change::WindowInfo | Change::Fonts | Change::Images | Change::HotspotMembership |
	       Change::Membership;
}

struct LuaCallbackMiniWindowsSnapshotState
{
		/** @brief Layout and mouse state that changes independently of miniwindow semantic content. */
		struct PresentationSnapshot
		{
				quint64 runtimeId{0};
				QRect   rect;
				bool    temporarilyHide{false};
				QPoint  lastMousePosition;
				int     lastMouseUpdate{0};
				QPoint  clientMousePosition;
				QString mouseOverHotspot;
				QString mouseDownHotspot;
				bool    operator==(const PresentationSnapshot &) const = default;
		};

		struct WindowInfoSnapshot
		{
				int       locationX{0};
				int       locationY{0};
				int       width{0};
				int       height{0};
				bool      show{false};
				bool      temporarilyHide{false};
				int       position{0};
				int       flags{0};
				qlonglong backgroundRef{0};
				int       rectLeft{0};
				int       rectTop{0};
				int       rectRight{0};
				int       rectBottom{0};
				int       lastMouseX{0};
				int       lastMouseY{0};
				int       lastMouseUpdate{0};
				int       clientMouseX{0};
				int       clientMouseY{0};
				QString   mouseOverHotspot;
				QString   mouseDownHotspot;
				double    installedAt{0.0};
				int       zOrder{0};
				QString   creatingPlugin;
				bool      operator==(const WindowInfoSnapshot &) const = default;
		};

		QStringList                                        windowNames;
		QHash<QString, QStringList>                        fontIdsByWindow;
		QHash<QString, QStringList>                        imageIdsByWindow;
		QHash<QString, QStringList>                        hotspotIdsByWindow;
		QHash<LuaCallbackMiniWindowResourceKey, bool>      imageHasAlphaByKey;
		QHash<QString, WindowInfoSnapshot>                 windowInfoByWindow;
		QHash<QString, QSharedPointer<const MiniWindow>>   miniWindowsByWindow;
		LuaCallbackPersistentSlotMap<PresentationSnapshot> miniWindowPresentationByRuntimeId;
		/**
		 * @brief Runtime owner and revision of the complete stable miniwindow domain.
		 *
		 * Zero in either field marks a snapshot assembled from callback-local or merged state. Such a snapshot
		 * must use field-level causal merging because it is not an exact copy of a runtime-owned domain revision.
		 */
		quint64                                            miniWindowStableRuntimeId{0};
		quint64                                            miniWindowStableRevision{0};
		bool                                               miniWindowLookupCacheValid{false};
		QSet<QString>                                      miniWindowIds;
		QSet<LuaCallbackMiniWindowResourceKey>             miniWindowFontKeys;
		QSet<LuaCallbackMiniWindowResourceKey>             miniWindowImageKeys;
		QSet<LuaCallbackMiniWindowResourceKey>             miniWindowHotspotKeys;

		void                                               rebuildMiniWindowLookupCaches()
		{
			auto addWindowKey = [this](const QString &windowName)
			{
				if (!windowName.isEmpty())
					miniWindowIds.insert(windowName);
				return windowName;
			};
			auto addItemKeys = [&addWindowKey](const QHash<QString, QStringList>      &itemsByWindow,
			                                   QSet<LuaCallbackMiniWindowResourceKey> &destination)
			{
				for (auto it = itemsByWindow.constBegin(); it != itemsByWindow.constEnd(); ++it)
				{
					const QString windowKey = addWindowKey(it.key());
					if (windowKey.isEmpty())
						continue;
					for (const QString &item : it.value())
					{
						if (!item.isEmpty())
							destination.insert({windowKey, item});
					}
				}
			};

			miniWindowIds.clear();
			miniWindowFontKeys.clear();
			miniWindowImageKeys.clear();
			miniWindowHotspotKeys.clear();
			for (const QString &windowName : windowNames)
				addWindowKey(windowName);
			for (auto it = miniWindowsByWindow.constBegin(); it != miniWindowsByWindow.constEnd(); ++it)
				addWindowKey(it.key());
			for (auto it = windowInfoByWindow.constBegin(); it != windowInfoByWindow.constEnd(); ++it)
				addWindowKey(it.key());
			addItemKeys(fontIdsByWindow, miniWindowFontKeys);
			addItemKeys(imageIdsByWindow, miniWindowImageKeys);
			addItemKeys(hotspotIdsByWindow, miniWindowHotspotKeys);
			miniWindowLookupCacheValid = true;
		}
		bool operator==(const LuaCallbackMiniWindowsSnapshotState &) const = default;
};

/** @brief Exact miniwindow presentation replacement carried by one causal mutation batch. */
struct LuaCallbackMiniWindowPresentationMutation
{
		quint64                                                                  runtimeId{0};
		std::optional<LuaCallbackMiniWindowsSnapshotState::PresentationSnapshot> presentation;
};

using LuaCallbackMiniWindowPresentationMutations = QHash<QString, LuaCallbackMiniWindowPresentationMutation>;

struct LuaCallbackTriggerWildcardsSnapshotState
{
		QMap<QString, QStringList>                            triggerWildcardsSnapshot;
		QMap<QString, QMap<QString, QString>>                 triggerNamedWildcardsSnapshot;
		QHash<QString, QMap<QString, QStringList>>            pluginTriggerWildcardsSnapshotById;
		QHash<QString, QMap<QString, QMap<QString, QString>>> pluginTriggerNamedWildcardsSnapshotById;
		bool operator==(const LuaCallbackTriggerWildcardsSnapshotState &) const = default;
};

struct LuaCallbackAliasWildcardsSnapshotState
{
		QMap<QString, QStringList>                            aliasWildcardsSnapshot;
		QMap<QString, QMap<QString, QString>>                 aliasNamedWildcardsSnapshot;
		QHash<QString, QMap<QString, QStringList>>            pluginAliasWildcardsSnapshotById;
		QHash<QString, QMap<QString, QMap<QString, QString>>> pluginAliasNamedWildcardsSnapshotById;
		bool operator==(const LuaCallbackAliasWildcardsSnapshotState &) const = default;
};

struct LuaCallbackStableSnapshotRoot
{
};

struct LuaCallbackStableSnapshotState
    : LuaCallbackStableSnapshotRoot
#define QMUD_INHERIT_LUA_CALLBACK_STABLE_SNAPSHOT_STATE(name, pluginIndexed, pluginInfoCoupled, stateType)   \
	, public stateType
      QMUD_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN_REGISTRY(QMUD_INHERIT_LUA_CALLBACK_STABLE_SNAPSHOT_STATE)
#undef QMUD_INHERIT_LUA_CALLBACK_STABLE_SNAPSHOT_STATE
{
};

[[nodiscard]] inline bool luaCallbackStableSnapshotDomainsEqual(const LuaCallbackStableSnapshotState &lhs,
                                                                const LuaCallbackStableSnapshotState &rhs,
                                                                const quint32 domainMask)
{
	auto ruleStoresEqual = [&]<typename Row>(const LuaCallbackRuleSnapshotStore<Row> &lhsStore,
	                                         const LuaCallbackRuleSnapshotStore<Row> &rhsStore)
	{
		auto ownerRowsEqual = [&](const LuaCallbackStableSnapshotState    &snapshot,
		                          const LuaCallbackRuleSnapshotStore<Row> &store, const QString &pluginId,
		                          const LuaCallbackStableSnapshotState    &otherSnapshot,
		                          const LuaCallbackRuleSnapshotStore<Row> &otherStore)
		{
			const auto ownerHandle =
			    [&](const LuaCallbackStableSnapshotState &state) -> std::optional<quint64>
			{
				if (pluginId.isEmpty())
					return 0;
				const auto identity = state.pluginRuntimeIdentitiesById.constFind(pluginId);
				return identity == state.pluginRuntimeIdentitiesById.constEnd() ||
				               identity->runtimeLocationHandle == 0
				           ? std::optional<quint64>{}
				           : std::optional<quint64>{identity->runtimeLocationHandle};
			};
			const auto lhsOwner = ownerHandle(snapshot);
			const auto rhsOwner = ownerHandle(otherSnapshot);
			if (lhsOwner.has_value() != rhsOwner.has_value())
				return false;
			if (!lhsOwner)
				return true;
			bool             lhsFound = false;
			bool             rhsFound = false;
			const QList<Row> lhsRows  = luaCallbackRuleSnapshotList(store, *lhsOwner, lhsFound);
			const QList<Row> rhsRows  = luaCallbackRuleSnapshotList(otherStore, *rhsOwner, rhsFound);
			return lhsFound == rhsFound && (!lhsFound || lhsRows == rhsRows);
		};

		if (!ownerRowsEqual(lhs, lhsStore, {}, rhs, rhsStore))
			return false;
		QSet<QString> pluginIds;
		for (auto it = lhs.pluginRuntimeIdentitiesById.constBegin();
		     it != lhs.pluginRuntimeIdentitiesById.constEnd(); ++it)
			pluginIds.insert(it.key());
		for (auto it = rhs.pluginRuntimeIdentitiesById.constBegin();
		     it != rhs.pluginRuntimeIdentitiesById.constEnd(); ++it)
			pluginIds.insert(it.key());
		return std::ranges::all_of(pluginIds, [&](const QString &pluginId)
		                           { return ownerRowsEqual(lhs, lhsStore, pluginId, rhs, rhsStore); });
	};

	bool       equal             = true;
	quint32    remainingMask     = domainMask;
	const auto compareRuleDomain = [&](const LuaCallbackStableSnapshotDomain domain, const auto &lhsStore,
	                                   const auto &rhsStore, const auto &lhsOverrides,
	                                   const auto &rhsOverrides, const auto &lhsMissing,
	                                   const auto &rhsMissing)
	{
		const quint32 bit = luaCallbackStableSnapshotDomainBit(domain);
		if ((remainingMask & bit) == 0U)
			return;
		equal = equal && ruleStoresEqual(lhsStore, rhsStore) && lhsOverrides == rhsOverrides &&
		        lhsMissing == rhsMissing;
		remainingMask &= ~bit;
	};
	compareRuleDomain(LuaCallbackStableSnapshotDomain::Triggers, lhs.triggerSnapshotStore,
	                  rhs.triggerSnapshotStore, lhs.triggerListOverridesByPluginId,
	                  rhs.triggerListOverridesByPluginId, lhs.missingTriggerListPluginIds,
	                  rhs.missingTriggerListPluginIds);
	compareRuleDomain(LuaCallbackStableSnapshotDomain::Aliases, lhs.aliasSnapshotStore,
	                  rhs.aliasSnapshotStore, lhs.aliasListOverridesByPluginId,
	                  rhs.aliasListOverridesByPluginId, lhs.missingAliasListPluginIds,
	                  rhs.missingAliasListPluginIds);
	compareRuleDomain(LuaCallbackStableSnapshotDomain::Timers, lhs.timerSnapshotStore, rhs.timerSnapshotStore,
	                  lhs.timerListOverridesByPluginId, rhs.timerListOverridesByPluginId,
	                  lhs.missingTimerListPluginIds, rhs.missingTimerListPluginIds);
#define QMUD_COMPARE_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN(name, pluginIndexed, pluginInfoCoupled, stateType)  \
	if ((remainingMask & luaCallbackStableSnapshotDomainBit(LuaCallbackStableSnapshotDomain::name)) != 0U)   \
		equal = equal && static_cast<const stateType &>(lhs) == static_cast<const stateType &>(rhs);
	QMUD_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN_REGISTRY(QMUD_COMPARE_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN)
#undef QMUD_COMPARE_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN
	return equal;
}

#undef QMUD_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN_REGISTRY
struct LuaCallbackSnapshot : LuaCallbackStableSnapshotState
{
		// Dispatch-local presentation and runtime counters.
		void                                               *framePointer{nullptr};
		bool                                                hasFramePointer{false};
		bool                                                hasCommandUiSnapshot{false};
		LuaCallbackCommandUiSnapshotState                   commandUiSnapshotState;
		bool                                                hasCommandHistorySnapshot{false};
		QStringList                                         commandHistorySnapshot;
		QString                                             activeMiniWindowExecutionName;
		/** MUSHclient WindowAddHotspot recursion guard inherited by this callback dispatch. */
		bool                                                windowAddHotspotReentryBlocked{false};
		QString                                             geometryConstrainedMiniWindowName;
		int                                                 geometryConstraintDisplayClientHeight{0};
		int                                                 geometryConstraintDisplayClientWidth{0};
		double                                              absoluteMiniWindowScaleXOver{1.0};
		double                                              absoluteMiniWindowScaleYOver{1.0};
		double                                              absoluteMiniWindowScaleXUnder{1.0};
		double                                              absoluteMiniWindowScaleYUnder{1.0};
		bool                                                hasRuntimeCountersSnapshot{false};
		LuaCallbackRuntimeCountersSnapshotState             runtimeCountersSnapshotState;
		// Dispatch-sampled external/asynchronous domains.
		bool                                                hasChatSnapshot{false};
		QList<long>                                         chatConnectionIdsSnapshot;
		QHash<long, QHash<int, QVariant>>                   chatInfoValuesById;
		QHash<long, QHash<QString, QVariant>>               chatOptionValuesById;
		QHash<QString, long>                                chatIdsByLookupKey;
		// Dispatch-sampled cross-runtime and backend state.
		bool                                                hasUdpPortSnapshot{false};
		QList<int>                                          udpPortsSnapshot;
		QHash<int, QString>                                 udpListenerPluginIdsByPort;
		bool                                                hasUsedUdpPortsSnapshot{false};
		QSet<int>                                           usedUdpPortsSnapshot;
		QHash<int, int>                                     usedUdpPortReferenceCountsSnapshot;
		QHash<int, int>                                     soundStatusByBuffer;
		QHash<int, bool>                                    soundBufferReusableByBuffer;
		// Request-local callback line context and overlays.
		bool                                                hasLineBufferSnapshot{false};
		int                                                 lineBufferCount{0};
		QSharedPointer<const LuaCallbackLineBufferSnapshot> lineBufferSnapshot;
		QHash<int, LuaCallbackLineEntrySnapshot>            lineEntriesByBufferIndex;
		bool                                                hasCallbackOutputAnchor{false};
		int                                                 callbackOutputAnchorBufferIndex{0};
		qint64                                              callbackOutputAnchorAbsoluteNumber{0};
		bool                                                callbackOutputAnchorIsActiveIncoming{false};
		bool                                                callbackOutputAnchorIsHidden{false};
		// Deferred output playback and promoted runtime cursors share this state across immutable
		// snapshots. Queued and suspended callbacks therefore retain position and presentation without a scan.
		std::shared_ptr<LuaCallbackOutputAnchorState>       callbackOutputAnchorState;
		bool                                                triggerMatchedLineSnapshotResolved{false};
		bool                                                hasTriggerMatchedLineSnapshot{false};
		int                                                 triggerMatchedLineBufferIndex{0};
		qint64                                              triggerMatchedLineAbsoluteNumber{0};
		LuaCallbackLineEntrySnapshot                        triggerMatchedLineSnapshot;
		bool                                                hasLineBufferDeltaSnapshot{false};
		bool                                                hasLineBufferCountDelta{false};
		int                                                 lineBufferDeltaCount{0};
		QHash<int, LuaCallbackLineEntrySnapshot>            lineEntryDeltasByBufferIndex;
		QSet<int>                                           missingLineEntryDeltasByBufferIndex;
		bool                                                linePresentationRequiresRefresh{false};
		bool                                                outputScrollPositionRequiresRefresh{false};
		bool                                             miniWindowGeometryConstraintRequiresRefresh{false};
		bool                                             recentLinesSnapshotInvalidated{false};
		bool                                             hasRecentLinesSnapshot{false};
		QStringList                                      recentLinesSnapshot;
		// Dispatch-local entity, main-window, clipboard, and notepad presentation.
		bool                                             hasEntitySnapshot{false};
		QSharedPointer<const QHash<QByteArray, quint32>> entitySlotsByName;
		LuaCallbackPersistentSlotMap<LuaCallbackEntitySnapshotRow> entityValuesBySlot;
		QHash<QByteArray, LuaCallbackEntitySnapshotOverride>       entityValueOverridesByName;
		bool                                                       hasUiSnapshot{false};
		QVariantMap                                                guiSystemValues;
		bool                                                       hasClipboardText{false};
		QString                                                    clipboardText;
		QHash<int, QRect>                                          mainWindowPositionsByMode;
		bool                                                       mainWindowPositionsDirty{false};
		QHash<QString, QRect>                                      worldWindowPositionsByKey;
		QSet<QString>                                              missingWorldWindowPositionKeys;
		QSet<int>                                                  dirtyWorldWindowPositionOrdinals;
		QHash<QString, QRect>                                      notepadWindowPositionsByKey;
		QSet<QString>                                              missingNotepadWindowPositionKeys;
		QSet<QString>                                              dirtyNotepadWindowPositionKeys;
		QSet<QString>                                              dirtyNotepadDocumentKeys;
		QHash<QString, int>                                        notepadLengthByKey;
		QSet<QString>                                              missingNotepadLengthKeys;
		QHash<QString, QString>                                    notepadTextByKey;
		QSet<QString>                                              missingNotepadTextKeys;
		QSet<QString>                                              dirtyNotepadListKeys;
		QHash<QString, QStringList>                                notepadListByKey;
		QSet<QString>                                              missingNotepadListKeys;
		QVector<LuaCallbackWorldRuntimeSnapshot>                   worldRuntimeSnapshot;
		QVector<LuaCallbackWorldWindowPositionSnapshot>            worldWindowPositionSnapshot;
		bool                                                       hasNotepadPresentationSnapshot{false};
		QVector<LuaCallbackNotepadSnapshot>                        notepadSnapshot;
		int                                                        actionSourceOverride{0};
		bool                                                       hasActionSourceOverride{false};
};

/**
 * @brief Batch callback dispatch request payload.
 */
struct LuaBatchDispatchRequest
{
		LuaBatchDispatchKind          kind{LuaBatchDispatchKind::NoArgs};
		LuaBatchDispatchLane          lane{LuaBatchDispatchLane::Callback};
		LuaCallbackEvent              callbackEvent{LuaCallbackEvent::None};
		LuaCallbackLineSnapshotPolicy lineSnapshotPolicy{LuaCallbackLineSnapshotPolicy::CountAndLast};
		QVector<QSharedPointer<LuaCallbackEngine>>                            engines;
		/** First engine in the shared recipient vector that this request dispatches. */
		int                                                                   firstEngineIndex{0};
		QString                                                               functionName;
		QString                                                               stringArg;
		QString                                                               stringArg2;
		QString                                                               miniWindowExecutionName;
		QSharedPointer<const QVector<LuaEngineObservedInitializationRequest>> initRequestsArg;
		QSharedPointer<const LuaScriptReloadRequest>                          scriptReloadArg;
		QSet<QString>                                                         observedCallbackNamesArg;
		QStringList                                                           stringListArg;
		QStringList                                                           stringListArg2;
		QByteArray                                                            bytesArg;
		QByteArray                                                            bytesArg2;
		QByteArray                                                            bytesArg3;
		QMap<QString, QString>                                                mapArg;
		QSharedPointer<const QVector<LuaStyleRun>>                            styleRunsArg;
		QSharedPointer<const LuaCallbackSnapshot>                             callbackSnapshotArg;
		std::shared_ptr<LuaCallbackConnectionCancellationState>               connectionCancellationState;
		std::shared_ptr<LuaCallbackRuntimeLifetimeState>                      runtimeLifetimeState;
		LuaCallbackIncomingLineObservation                                    incomingLineObservationArg;
		LuaCallbackSentObservation                                            sentObservationArg;
		long                                                                  numberArg1{0};
		long                                                                  numberArg2{0};
		int                                                                   intArg1{0};
		int                                                                   intArg2{0};
		int     triggerMatchedLineBufferIndex{0};
		qint64  triggerMatchedLineAbsoluteNumber{0};
		int     callbackOutputAnchorBufferIndex{0};
		qint64  callbackOutputAnchorAbsoluteNumber{0};
		int     actionSourceOverride{0};
		bool    doingSimulateOverride{false};
		bool    optionFlag{false};
		bool    defaultResult{false};
		bool    revalidateObservedRecipients{false};
		bool    hasActionSourceOverride{false};
		bool    hasDoingSimulateOverride{false};
		bool    hasCallbackOutputAnchor{false};
		bool    executeScriptHasTriggerContext{false};
		bool    triggerOutputReplacesMatchedLine{false};
		bool    applyCallingPluginContext{false};
		bool    screendrawExecutionGuard{false};
		bool    drawOutputWindowExecutionGuard{false};
		/** The dispatch continues a call chain that originated in a runtime callback. */
		bool    inheritedCallbackOrigin{false};
		/** Keep WindowAddHotspot guarded while processing mouse callbacks induced by an add. */
		bool    windowAddHotspotReentryBlocked{false};
		QString callingPluginId;
		quint64 modalResumeId{0};
		quint64 runtimeModalResumeId{0};
#ifdef QMUD_ENABLE_LUA_SCRIPTING
		lua_State *luaStateArg{nullptr};
#endif
		bool refreshCallbackCatalogAfter{false};
};

/**
 * @brief Runtime identity dependencies shared by one nested callback chain for one world.
 *
 * Deferred rule, miniwindow, and entity mutations can consume identities produced by an earlier
 * journal before the runtime thread has applied that journal. This state carries those translations
 * across nested callback contexts without runtime-container scans.
 */
struct LuaCallbackMutationDependencyState
{
		/** @brief Runtime slots resolved for captured identities of one normalized entity name. */
		struct EntityBindings
		{
				QHash<quint32, quint32> runtimeSlotsByCapturedSlot;
		};

		QHash<quint64, quint64>                                     ruleLocationsByRuntimeId;
		QHash<QString, std::shared_ptr<MiniWindowCommitOutcomeMap>> miniWindowOutcomesByName;
		QHash<QByteArray, EntityBindings>                           entityBindingsByName;
		quint32                                                     invalidatedEntitySlotUpperBound{0};
};

/**
 * @brief Runtime-owned mutation journal produced by a callback worker.
 *
 * The callback thread records ordered mutations while preserving callback-local read-your-writes
 * semantics. The owning runtime thread applies this journal after callback execution returns.
 */
struct LuaDeferredRuntimeMutationBatch
{
		QPointer<WorldRuntime>                                  runtime;
		QVector<std::function<void()>>                          mutations;
		/** Connection generation that must remain live while this journal is applied. */
		std::shared_ptr<LuaCallbackConnectionCancellationState> connectionCancellationState;
		/** Runtime lifetime phase that accepted the callback producing this journal. */
		std::shared_ptr<LuaCallbackRuntimeLifetimeState>        runtimeLifetimeState;
		/** Identity dependencies shared by the callback chain that produced this batch. */
		std::shared_ptr<LuaCallbackMutationDependencyState>     mutationDependencies;
		/** Whether callbacks raised while applying this batch must retain its callback-lane position. */
		bool                                                    requiresCausalOrdering{true};
		/** Whether callbacks raised while applying this batch inherit WindowAddHotspot reentry blocking. */
		bool                                                    windowAddHotspotReentryBlocked{false};
		/** Whether the originating callback event is available for mutation-playback behavior. */
		bool                                                    hasPlaybackEvent{false};
		/** Originating callback event when @ref hasPlaybackEvent is true. */
		LuaCallbackEvent                                        playbackEvent{LuaCallbackEvent::None};
		/** Effective queue domain inherited by callbacks raised during playback. */
		LuaCallbackOrderingDomain orderingDomain{LuaCallbackOrderingDomain::InputCritical};
};

using LuaDeferredRuntimeMutationConsumer = std::function<void(QVector<LuaDeferredRuntimeMutationBatch>)>;

/**
 * @brief Recovery ownership for deferred mutations carried by an asynchronous dispatch result.
 *
 * Normal result consumption and worker shutdown atomically compete for the same backup. This
 * guarantees that an undelivered completion cannot lose mutations or apply them twice.
 */
class LuaDeferredRuntimeMutationDelivery final
{
	public:
		using DeliveryAction   = std::function<void(QVector<LuaDeferredRuntimeMutationBatch>)>;
		using DeliveryConsumer = std::function<bool(const DeliveryAction &)>;

		LuaDeferredRuntimeMutationDelivery(QVector<LuaDeferredRuntimeMutationBatch> backup,
		                                   DeliveryConsumer                         deliveryConsumer)
		    : m_backup(std::move(backup)), m_deliveryConsumer(std::move(deliveryConsumer))
		{
		}

		[[nodiscard]] bool consumeForDelivery(const DeliveryAction &consumer) const
		{
			return m_deliveryConsumer && m_deliveryConsumer(consumer);
		}

		[[nodiscard]] bool recoverUndelivered(const LuaDeferredRuntimeMutationConsumer &consumer)
		{
			return consumeForDelivery(
			    [this, consumer](QVector<LuaDeferredRuntimeMutationBatch> earlierBatches)
			    {
				    QVector<LuaDeferredRuntimeMutationBatch> backup = takeBackupForRecovery();
				    earlierBatches += std::move(backup);
				    if (!earlierBatches.isEmpty() && consumer)
					    consumer(std::move(earlierBatches));
			    });
		}

		[[nodiscard]] QVector<LuaDeferredRuntimeMutationBatch> takeBackupForRecovery()
		{
			QVector<LuaDeferredRuntimeMutationBatch> backup;
			backup.swap(m_backup);
			return backup;
		}

	private:
		QVector<LuaDeferredRuntimeMutationBatch> m_backup;
		DeliveryConsumer                         m_deliveryConsumer;
};

/**
 * @brief Shared presentation produced by an internal callback line-page request.
 */
struct LuaCallbackLinePageResult
{
		QPointer<WorldRuntime>                              runtime;
		QSharedPointer<const LuaCallbackLineBufferSnapshot> presentation;
		bool                                                hasRecentLinesSnapshot{false};
};

/**
 * @brief Pending string-result modal request produced by a yielded Lua callback.
 */
struct LuaPendingModalStringRequest
{
		enum class InternalRuntimeOperation : quint8
		{
			None,
			ResetMxp
		};

		std::function<QString()>                                       guiCallable;
		std::function<void(WorldRuntime &, std::function<void(bool)>)> runtimeOperation;
		std::function<void(WorldRuntime &, const QString &)>           beforeRuntimeResumeCallback;
		std::function<void(const QString &)>                           beforeResumeCallback;
		std::function<void()>                                          beforeCancelCallback;
		std::function<void(quint64, QString)>                          resultCallback;
		QSharedPointer<LuaCallbackLinePageResult>                      linePageResult;
		InternalRuntimeOperation internalRuntimeOperation{InternalRuntimeOperation::None};
		bool                     refreshSnapshotBeforeResume{false};
		bool                     internalImmediateResume{false};
};

/**
 * @brief Batch callback dispatch result payload.
 */
struct LuaBatchDispatchResult
{
		bool                                                    boolResult{false};
		bool                                                    boolResultValid{false};
		bool                                                    hasFunction{false};
		bool                                                    hasFunctionValid{false};
		int                                                     countResult{0};
		bool                                                    countResultValid{false};
		QString                                                 stringResult;
		QByteArray                                              bytesResult;
		int                                                     marshallingError{0};
		bool                                                    marshallingErrorValid{false};
		int                                                     marshallingIndex{0};
		QByteArray                                              marshallingTypeName;
		QString                                                 marshallingRuntimeError;
		int                                                     marshallingReturnCount{0};
		bool                                                    marshallingSameState{false};
		bool                                                    suspended{false};
		bool                                                    recipientMutationBoundary{false};
		int                                                     nextEngineIndex{-1};
		QSharedPointer<const LuaCallbackSnapshot>               callbackSnapshotAfterMutations;
		bool                                                    callbackSnapshotIsPresentationOnly{false};
		bool                                                    linePresentationRequiresRefresh{false};
		bool                                                    outputScrollPositionRequiresRefresh{false};
		bool                                                    outputScrollPositionChanged{false};
		bool                                                    commandUiPresentationRequiresRefresh{false};
		bool                                                    globalPresentationRequiresRefresh{false};
		bool                                                    commandHistoryChanged{false};
		bool                                                    notepadPresentationChanged{false};
		bool                                                    hasNotepadPresentationSnapshot{false};
		QVector<LuaCallbackNotepadSnapshot>                     notepadPresentationSnapshot;
		quint64                                                 modalResumeId{0};
		int                                                     suspendedEngineIndex{-1};
		bool                                                    hasPendingModalStringRequest{false};
		LuaPendingModalStringRequest                            pendingModalStringRequest;
		std::shared_ptr<LuaCallbackConnectionCancellationState> connectionCancellationState;
		QVector<LuaDeferredRuntimeMutationBatch>                deferredRuntimeMutationBatches;
		QSharedPointer<LuaDeferredRuntimeMutationDelivery>      deferredRuntimeMutationDelivery;
};

enum class LuaBatchRecipientStopCondition
{
	Never,
	FalseResult,
	TrueResult,
	FunctionPresent
};

/**
 * @brief Complete behavior classification for one batch dispatch kind.
 *
 * The policy array is generated in enum order from the same registry that declares LuaBatchDispatchKind. This
 * retains one authoritative classification without generating switches containing repeated return branches.
 */
struct LuaBatchDispatchPolicy
{
		LuaBatchAggregateShape          aggregateShape;
		LuaBatchResumeContinuationShape continuationShape;
		LuaBatchFallbackShape           fallbackShape;
		LuaBatchFunctionNamePolicy      functionNamePolicy;
};

inline constexpr auto kLuaBatchDispatchPolicies = std::to_array<LuaBatchDispatchPolicy>({
#define QMUD_DECLARE_LUA_BATCH_DISPATCH_POLICY(name, aggregateShape, continuationShape, fallbackShape,       \
                                               functionNamePolicy)                                           \
	LuaBatchDispatchPolicy{                                                                                  \
	    LuaBatchAggregateShape::aggregateShape, LuaBatchResumeContinuationShape::continuationShape,          \
	    LuaBatchFallbackShape::fallbackShape, LuaBatchFunctionNamePolicy::functionNamePolicy},
    QMUD_LUA_BATCH_DISPATCH_KIND_REGISTRY(QMUD_DECLARE_LUA_BATCH_DISPATCH_POLICY)
#undef QMUD_DECLARE_LUA_BATCH_DISPATCH_POLICY
});

inline constexpr LuaBatchDispatchPolicy kInvalidLuaBatchDispatchPolicy{
    LuaBatchAggregateShape::None,
    LuaBatchResumeContinuationShape::ResultOnly,
    LuaBatchFallbackShape::AggregateNeutral,
    LuaBatchFunctionNamePolicy::Required,
};

[[nodiscard]] constexpr const LuaBatchDispatchPolicy &
luaBatchDispatchPolicy(const LuaBatchDispatchKind kind) noexcept
{
	const auto index = static_cast<std::size_t>(kind);
	return index < kLuaBatchDispatchPolicies.size() ? kLuaBatchDispatchPolicies[index]
	                                                : kInvalidLuaBatchDispatchPolicy;
}

/**
 * @brief Returns the aggregate shape for a batch dispatch kind.
 * @param kind Dispatch kind to classify.
 * @return Shared aggregate result shape.
 */
[[nodiscard]] constexpr LuaBatchAggregateShape luaBatchDispatchAggregateShape(const LuaBatchDispatchKind kind)
{
	return luaBatchDispatchPolicy(kind).aggregateShape;
}

/**
 * @brief Returns the authoritative post-resume continuation shape for a dispatch kind.
 */
[[nodiscard]] constexpr LuaBatchResumeContinuationShape
luaBatchDispatchResumeContinuationShape(const LuaBatchDispatchKind kind)
{
	return luaBatchDispatchPolicy(kind).continuationShape;
}

/**
 * @brief Returns the authoritative neutral-result contract for a dispatch kind.
 */
[[nodiscard]] constexpr LuaBatchFallbackShape luaBatchDispatchFallbackShape(const LuaBatchDispatchKind kind)
{
	return luaBatchDispatchPolicy(kind).fallbackShape;
}

/**
 * @brief Returns whether a dispatch kind requires a non-empty Lua function name.
 */
[[nodiscard]] constexpr bool luaBatchDispatchRequiresFunctionName(const LuaBatchDispatchKind kind)
{
	return luaBatchDispatchPolicy(kind).functionNamePolicy == LuaBatchFunctionNamePolicy::Required;
}

#undef QMUD_LUA_BATCH_DISPATCH_KIND_REGISTRY

/**
 * @brief Builds the neutral result returned when a batch dispatch cannot run or continue.
 * @param request Dispatch whose public result contract must be preserved.
 * @return Neutral, fully initialized result for the dispatch kind.
 */
[[nodiscard]] inline LuaBatchDispatchResult
makeLuaBatchDispatchFallback(const LuaBatchDispatchRequest &request)
{
	LuaBatchDispatchResult result{};
	switch (luaBatchDispatchAggregateShape(request.kind))
	{
	case LuaBatchAggregateShape::BooleanAndFunction:
	case LuaBatchAggregateShape::StopOnFalse:
		result.boolResult       = true;
		result.boolResultValid  = true;
		result.hasFunction      = false;
		result.hasFunctionValid = true;
		break;
	case LuaBatchAggregateShape::FunctionHandled:
	case LuaBatchAggregateShape::StopOnTrue:
		result.boolResult       = false;
		result.boolResultValid  = true;
		result.hasFunction      = false;
		result.hasFunctionValid = true;
		break;
	case LuaBatchAggregateShape::FunctionCount:
		result.countResult      = 0;
		result.countResultValid = true;
		break;
	case LuaBatchAggregateShape::FunctionAny:
		result.hasFunction      = false;
		result.hasFunctionValid = true;
		break;
	case LuaBatchAggregateShape::BytesInOut:
		result.bytesResult = request.bytesArg;
		break;
	case LuaBatchAggregateShape::StringInOut:
		result.stringResult = request.stringArg;
		break;
	case LuaBatchAggregateShape::LastResult:
	case LuaBatchAggregateShape::None:
		break;
	}

	switch (luaBatchDispatchFallbackShape(request.kind))
	{
	case LuaBatchFallbackShape::HasFunctionFalse:
		result.hasFunction      = false;
		result.hasFunctionValid = true;
		break;
	case LuaBatchFallbackShape::BoolFalse:
		result.boolResult      = false;
		result.boolResultValid = true;
		break;
	case LuaBatchFallbackShape::BoolAndFunctionFalse:
		result.boolResult       = false;
		result.boolResultValid  = true;
		result.hasFunction      = false;
		result.hasFunctionValid = true;
		break;
	case LuaBatchFallbackShape::AggregateNeutral:
		break;
	}
	return result;
}

/**
 * @brief Returns the single stop policy shared by initial and continued callback dispatches.
 */
[[nodiscard]] constexpr LuaBatchRecipientStopCondition
luaBatchDispatchRecipientStopCondition(const LuaBatchDispatchKind kind)
{
	switch (luaBatchDispatchAggregateShape(kind))
	{
	case LuaBatchAggregateShape::StopOnFalse:
		return LuaBatchRecipientStopCondition::FalseResult;
	case LuaBatchAggregateShape::FunctionHandled:
		return LuaBatchRecipientStopCondition::FunctionPresent;
	case LuaBatchAggregateShape::StopOnTrue:
		return LuaBatchRecipientStopCondition::TrueResult;
	default:
		return LuaBatchRecipientStopCondition::Never;
	}
}

[[nodiscard]] inline bool luaBatchDispatchStopsAfterRecipient(const LuaBatchDispatchKind kind,
                                                              const bool hasFunction, const bool boolResult)
{
	switch (luaBatchDispatchRecipientStopCondition(kind))
	{
	case LuaBatchRecipientStopCondition::FalseResult:
		return hasFunction && !boolResult;
	case LuaBatchRecipientStopCondition::TrueResult:
		return hasFunction && boolResult;
	case LuaBatchRecipientStopCondition::FunctionPresent:
		return hasFunction;
	case LuaBatchRecipientStopCondition::Never:
		return false;
	}
	return false;
}

[[nodiscard]] inline bool luaBatchDispatchStopsAfterRecipient(const LuaBatchDispatchKind    kind,
                                                              const LuaBatchDispatchResult &result)
{
	switch (luaBatchDispatchRecipientStopCondition(kind))
	{
	case LuaBatchRecipientStopCondition::FalseResult:
		return result.boolResultValid && !result.boolResult;
	case LuaBatchRecipientStopCondition::TrueResult:
		return result.boolResultValid && result.boolResult;
	case LuaBatchRecipientStopCondition::FunctionPresent:
		return result.hasFunctionValid && result.hasFunction;
	case LuaBatchRecipientStopCondition::Never:
		return false;
	}
	return false;
}

inline void mergeLuaBatchPresentationRefreshFlags(LuaBatchDispatchResult       &aggregate,
                                                  const LuaBatchDispatchResult &result)
{
	aggregate.linePresentationRequiresRefresh |= result.linePresentationRequiresRefresh;
	aggregate.outputScrollPositionRequiresRefresh |= result.outputScrollPositionRequiresRefresh;
	aggregate.outputScrollPositionChanged |= result.outputScrollPositionChanged;
	aggregate.commandUiPresentationRequiresRefresh |= result.commandUiPresentationRequiresRefresh;
	aggregate.globalPresentationRequiresRefresh |= result.globalPresentationRequiresRefresh;
	aggregate.commandHistoryChanged |= result.commandHistoryChanged;
	if (result.notepadPresentationChanged)
	{
		aggregate.notepadPresentationChanged     = true;
		aggregate.hasNotepadPresentationSnapshot = result.hasNotepadPresentationSnapshot;
		aggregate.notepadPresentationSnapshot    = result.hasNotepadPresentationSnapshot
		                                               ? result.notepadPresentationSnapshot
		                                               : QVector<LuaCallbackNotepadSnapshot>{};
	}
}

/**
 * @brief Carries a cumulative in/out aggregate into the request for the next recipient.
 * @param kind Original batch dispatch kind.
 * @param request Next-recipient request to update.
 * @param aggregate Cumulative result produced by the preceding recipients.
 *
 * In/out continuation is an aggregation policy, not a runtime dispatch-kind policy. Keeping this switch beside the
 * authoritative aggregate implementation prevents continuation sites from maintaining their own kind lists.
 */
inline void carryLuaBatchAggregateIntoNextRecipientRequest(const LuaBatchDispatchKind    kind,
                                                           LuaBatchDispatchRequest      &request,
                                                           const LuaBatchDispatchResult &aggregate)
{
	switch (luaBatchDispatchAggregateShape(kind))
	{
	case LuaBatchAggregateShape::BytesInOut:
		request.bytesArg = aggregate.bytesResult;
		break;
	case LuaBatchAggregateShape::StringInOut:
		request.stringArg = aggregate.stringResult;
		break;
	case LuaBatchAggregateShape::BooleanAndFunction:
	case LuaBatchAggregateShape::StopOnFalse:
	case LuaBatchAggregateShape::FunctionHandled:
	case LuaBatchAggregateShape::StopOnTrue:
	case LuaBatchAggregateShape::FunctionCount:
	case LuaBatchAggregateShape::FunctionAny:
	case LuaBatchAggregateShape::LastResult:
	case LuaBatchAggregateShape::None:
		break;
	}
}

/**
 * @brief Merges one completed recipient into a batch aggregate.
 * @param kind Original batch dispatch kind.
 * @param aggregate Aggregate to update.
 * @param recipient Completed recipient result.
 */
inline void mergeLuaBatchRecipientResult(const LuaBatchDispatchKind kind, LuaBatchDispatchResult &aggregate,
                                         const LuaBatchDispatchResult &recipient)
{
	mergeLuaBatchPresentationRefreshFlags(aggregate, recipient);
	switch (luaBatchDispatchAggregateShape(kind))
	{
	case LuaBatchAggregateShape::BooleanAndFunction:
		aggregate.boolResult      = aggregate.boolResult && recipient.boolResult;
		aggregate.boolResultValid = true;
		aggregate.hasFunction =
		    aggregate.hasFunction || (recipient.hasFunctionValid && recipient.hasFunction);
		aggregate.hasFunctionValid = true;
		break;
	case LuaBatchAggregateShape::StopOnFalse:
		if (recipient.hasFunctionValid && recipient.hasFunction && recipient.boolResultValid &&
		    !recipient.boolResult)
			aggregate.boolResult = false;
		aggregate.boolResultValid = true;
		aggregate.hasFunction =
		    aggregate.hasFunction || (recipient.hasFunctionValid && recipient.hasFunction);
		aggregate.hasFunctionValid = true;
		break;
	case LuaBatchAggregateShape::FunctionHandled:
		if (recipient.hasFunctionValid && recipient.hasFunction)
			aggregate.boolResult = true;
		aggregate.boolResultValid = true;
		aggregate.hasFunction =
		    aggregate.hasFunction || (recipient.hasFunctionValid && recipient.hasFunction);
		aggregate.hasFunctionValid = true;
		break;
	case LuaBatchAggregateShape::StopOnTrue:
		if (recipient.hasFunctionValid && recipient.hasFunction && recipient.boolResultValid &&
		    recipient.boolResult)
			aggregate.boolResult = true;
		aggregate.boolResultValid = true;
		aggregate.hasFunction =
		    aggregate.hasFunction || (recipient.hasFunctionValid && recipient.hasFunction);
		aggregate.hasFunctionValid = true;
		break;
	case LuaBatchAggregateShape::FunctionCount:
		if (recipient.countResultValid)
			aggregate.countResult += recipient.countResult;
		else if (recipient.hasFunctionValid && recipient.hasFunction)
			++aggregate.countResult;
		aggregate.countResultValid = true;
		break;
	case LuaBatchAggregateShape::FunctionAny:
		aggregate.hasFunction =
		    aggregate.hasFunction || (recipient.hasFunctionValid && recipient.hasFunction);
		aggregate.hasFunctionValid = true;
		break;
	case LuaBatchAggregateShape::BytesInOut:
		aggregate.bytesResult = recipient.bytesResult;
		break;
	case LuaBatchAggregateShape::StringInOut:
		aggregate.stringResult = recipient.stringResult;
		break;
	case LuaBatchAggregateShape::LastResult:
		if (recipient.boolResultValid)
		{
			aggregate.boolResult      = recipient.boolResult;
			aggregate.boolResultValid = true;
		}
		if (recipient.hasFunctionValid)
		{
			aggregate.hasFunction      = recipient.hasFunction;
			aggregate.hasFunctionValid = true;
		}
		break;
	case LuaBatchAggregateShape::None:
		break;
	}
}

/**
 * @brief Merges an executor-produced recipient-range aggregate into an earlier aggregate.
 * @param kind Original batch dispatch kind.
 * @param aggregate Aggregate produced by recipients before the dispatched range.
 * @param rangeResult Aggregate produced by the dispatched recipient range.
 *
 * Unlike mergeLuaBatchRecipientResult(), this preserves raw partial values when the range ends in a
 * suspended recipient and therefore marks its public result fields invalid. Those raw values are required
 * when the suspended recipient resumes and completes the aggregate.
 */
inline void mergeLuaBatchRecipientRangeResult(const LuaBatchDispatchKind    kind,
                                              LuaBatchDispatchResult       &aggregate,
                                              const LuaBatchDispatchResult &rangeResult)
{
	mergeLuaBatchPresentationRefreshFlags(aggregate, rangeResult);
	switch (luaBatchDispatchAggregateShape(kind))
	{
	case LuaBatchAggregateShape::BooleanAndFunction:
	case LuaBatchAggregateShape::StopOnFalse:
		aggregate.boolResult       = aggregate.boolResult && rangeResult.boolResult;
		aggregate.boolResultValid  = rangeResult.boolResultValid;
		aggregate.hasFunction      = aggregate.hasFunction || rangeResult.hasFunction;
		aggregate.hasFunctionValid = rangeResult.hasFunctionValid;
		break;
	case LuaBatchAggregateShape::FunctionHandled:
	case LuaBatchAggregateShape::StopOnTrue:
		aggregate.boolResult       = aggregate.boolResult || rangeResult.boolResult;
		aggregate.boolResultValid  = rangeResult.boolResultValid;
		aggregate.hasFunction      = aggregate.hasFunction || rangeResult.hasFunction;
		aggregate.hasFunctionValid = rangeResult.hasFunctionValid;
		break;
	case LuaBatchAggregateShape::FunctionCount:
		aggregate.countResult += rangeResult.countResult;
		aggregate.countResultValid = rangeResult.countResultValid;
		break;
	case LuaBatchAggregateShape::FunctionAny:
		aggregate.hasFunction      = aggregate.hasFunction || rangeResult.hasFunction;
		aggregate.hasFunctionValid = rangeResult.hasFunctionValid;
		break;
	case LuaBatchAggregateShape::BytesInOut:
		aggregate.bytesResult = rangeResult.bytesResult;
		break;
	case LuaBatchAggregateShape::StringInOut:
		aggregate.stringResult = rangeResult.stringResult;
		break;
	case LuaBatchAggregateShape::LastResult:
		if (rangeResult.boolResultValid)
		{
			aggregate.boolResult      = rangeResult.boolResult;
			aggregate.boolResultValid = true;
		}
		if (rangeResult.hasFunctionValid)
		{
			aggregate.hasFunction      = rangeResult.hasFunction;
			aggregate.hasFunctionValid = true;
		}
		break;
	case LuaBatchAggregateShape::None:
		break;
	}
}

/**
 * @brief Preserves results produced before a suspended recipient when dispatch cannot resume.
 * @param kind Original multi-recipient dispatch shape.
 * @param fallback Request-level fallback to update.
 * @param partial Aggregate produced before the suspended recipient completed.
 */
inline void preserveLuaBatchPartialResultOnFallback(const LuaBatchDispatchKind    kind,
                                                    LuaBatchDispatchResult       &fallback,
                                                    const LuaBatchDispatchResult &partial)
{
	mergeLuaBatchPresentationRefreshFlags(fallback, partial);
	switch (luaBatchDispatchAggregateShape(kind))
	{
	case LuaBatchAggregateShape::BooleanAndFunction:
	case LuaBatchAggregateShape::StopOnFalse:
	case LuaBatchAggregateShape::StopOnTrue:
	case LuaBatchAggregateShape::FunctionHandled:
		fallback.boolResult       = partial.boolResult;
		fallback.boolResultValid  = true;
		fallback.hasFunction      = partial.hasFunction;
		fallback.hasFunctionValid = true;
		break;
	case LuaBatchAggregateShape::FunctionCount:
		fallback.countResult      = partial.countResult;
		fallback.countResultValid = true;
		break;
	case LuaBatchAggregateShape::FunctionAny:
		fallback.hasFunction      = partial.hasFunction;
		fallback.hasFunctionValid = true;
		break;
	case LuaBatchAggregateShape::BytesInOut:
		fallback.bytesResult = partial.bytesResult;
		break;
	case LuaBatchAggregateShape::StringInOut:
		fallback.stringResult = partial.stringResult;
		break;
	case LuaBatchAggregateShape::LastResult:
	case LuaBatchAggregateShape::None:
		break;
	}
}

[[nodiscard]] inline bool luaBatchPresentationRequiresRefresh(const LuaBatchDispatchResult &result)
{
	return result.linePresentationRequiresRefresh || result.outputScrollPositionRequiresRefresh ||
	       result.outputScrollPositionChanged || result.commandUiPresentationRequiresRefresh ||
	       result.globalPresentationRequiresRefresh || result.commandHistoryChanged ||
	       result.notepadPresentationChanged;
}

/**
 * @brief Returns whether a callback result published committed or deferred runtime mutations.
 * @param result Dispatch result to inspect before its deferred mutation batches are consumed.
 * @return `true` when the runtime must advance the request snapshot before another recipient runs.
 *
 * Owner-thread callbacks normally apply mutations synchronously and publish the resulting immutable snapshot.
 * Worker callbacks additionally carry deferred mutation batches. Both are representations of the same mutation
 * boundary; continuation code must use this predicate rather than testing either representation independently.
 */
[[nodiscard]] inline bool luaBatchPublishedMutationBoundary(const LuaBatchDispatchResult &result)
{
	if (result.callbackSnapshotAfterMutations && !result.callbackSnapshotIsPresentationOnly)
		return true;
	return std::ranges::any_of(result.deferredRuntimeMutationBatches,
	                           [](const LuaDeferredRuntimeMutationBatch &batch)
	                           { return batch.requiresCausalOrdering; });
}

namespace QMudLuaDeferredRuntimeMutation
{
	/**
	 * @brief Applies ordered callback cleanup batches on their owning runtime threads.
	 * @param batches Batches to consume.
	 */
	void apply(QVector<LuaDeferredRuntimeMutationBatch> batches);
	/**
	 * @brief Applies ordered callback batches and reports completion after suspended descendants finish.
	 * @param batches Batches to consume.
	 * @param completion Completion invoked after the complete journal.
	 */
	void apply(QVector<LuaDeferredRuntimeMutationBatch> batches, std::function<void(bool)> completion);
	/**
	 * @brief Applies local batches immediately and queues foreign-thread batches without waiting.
	 * @param batches Batches to consume.
	 */
	void applyLocallyOrQueue(QVector<LuaDeferredRuntimeMutationBatch> batches);
	/**
	 * @brief Takes and applies every deferred cleanup batch carried by a dispatch result.
	 * @param result Dispatch result whose cleanup batches are consumed.
	 */
	void apply(LuaBatchDispatchResult &result);
	/**
	 * @brief Applies a result journal and invokes completion after every suspended mutation resumes.
	 * @param result Dispatch result whose cleanup batches are consumed.
	 * @param completion Completion invoked with `true` after the full ordered journal finishes, or `false`
	 * when playback is cancelled.
	 */
	void apply(LuaBatchDispatchResult &result, std::function<void(bool)> completion);
} // namespace QMudLuaDeferredRuntimeMutation

/**
 * @brief Engine bootstrap payload used for batched plugin Lua initialization.
 */
struct LuaEngineObservedInitializationRequest
{
		LuaCallbackEngine                *engine{nullptr};
		QSharedPointer<LuaCallbackEngine> workerLifetimeOwner;
		WorldRuntime                     *runtime{nullptr};
		QString                           scriptText;
		QString                           pluginId;
		QString                           pluginName;
		QString                           pluginDirectory;
		QSet<QString>                     callbackNames;
		std::function<void(const QString &, const QSet<QString> &, const QSet<QString> &)> observer;
};

/**
 * @brief Execution seam for invoking Lua callback engine operations.
 *
 * Every runtime uses the worker-thread executor. Its worker lane invokes the
 * same-thread direct primitive through this interface.
 */
class ILuaExecutor
{
	public:
		virtual ~ILuaExecutor() = default;

		/**
		 * @brief Dispatches one structured batch callback command.
		 * @param request Command payload and arguments.
		 * @return Dispatch result payload.
		 */
		[[nodiscard]] virtual LuaBatchDispatchResult
		dispatchBatch(const LuaBatchDispatchRequest &request) const;
		/**
		 * @brief Dispatches one structured batch callback command asynchronously with completion callback.
		 * @param request Command payload and arguments.
		 * @param completionTarget QObject owning thread for completion delivery; may be null.
		 * @param completion Completion callback receiving dispatch result payload.
		 */
		virtual void
		dispatchBatchAsync(const LuaBatchDispatchRequest &request, QObject *completionTarget,
		                   const std::function<void(const LuaBatchDispatchResult &)> &completion) const;
};

/**
 * @brief Same-thread execution primitive used exclusively on a Lua worker lane.
 */
class LuaExecutorDirect final : public ILuaExecutor
{
};

/**
 * @brief Creates the runtime Lua executor backend.
 * @return Worker-thread Lua executor used by every runtime.
 */
std::unique_ptr<ILuaExecutor> makeLuaExecutor(LuaDeferredRuntimeMutationConsumer shutdownMutationConsumer);

#endif // QMUD_LUAEXECUTOR_H
