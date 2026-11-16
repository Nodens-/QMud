/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: PersistentStringMap.h
 * Role: Structurally shared immutable string maps for published state.
 */
#ifndef QMUD_PERSISTENTSTRINGMAP_H
#define QMUD_PERSISTENTSTRINGMAP_H

#include <QHash>
#include <QSharedPointer>
#include <QStringList>
// ReSharper disable once CppUnusedIncludeDirective
#include <QVector>
#include <atomic>
#include <bit>
#include <utility>
/**
 * @brief Immutable string-keyed map with structural sharing between published snapshots.
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

		/**
		 * @brief Result of one exact-key mutation.
		 */
		struct MutationResult
		{
				ValuePointer       previousValue;
				bool               changed{false};

				/**
				 * @brief Checks whether the mutation replaced an existing key.
				 * @return Whether the key existed before the mutation.
				 */
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

		/**
		 * @brief Hashes an exact string key.
		 * @param key Key to hash.
		 * @return 64-bit hash used for directory traversal.
		 */
		[[nodiscard]] static quint64 keyHash(const QString &key) noexcept
		{
			return static_cast<quint64>(qHash(key, size_t{0}));
		}

		/**
		 * @brief Allocates a unique mutable transaction identity.
		 * @return Nonzero builder token.
		 */
		[[nodiscard]] static quint64 nextBuilderToken() noexcept
		{
			static std::atomic<quint64> nextToken{1};
			quint64                     result = nextToken.fetch_add(1, std::memory_order_relaxed);
			if (result == 0)
				result = nextToken.fetch_add(1, std::memory_order_relaxed);
			return result;
		}

		/**
		 * @brief Selects a hash branch.
		 * @param hash Complete key hash.
		 * @param level Directory level.
		 * @return Branch number.
		 */
		[[nodiscard]] static int branchForHash(const quint64 hash, const int level) noexcept
		{
			return static_cast<int>((hash >> (level * kBitsPerLevel)) & kBranchMask);
		}

		/**
		 * @brief Encodes a branch in an occupancy bitmap.
		 * @param branch Branch number.
		 * @return Occupancy bit.
		 */
		[[nodiscard]] static quint32 branchBit(const int branch) noexcept
		{
			return quint32{1} << static_cast<quint32>(branch);
		}

		/**
		 * @brief Resolves a bitmap branch to packed storage.
		 * @param bitmap Occupied branches.
		 * @param bit Selected branch bit.
		 * @return Packed branch index.
		 */
		[[nodiscard]] static qsizetype branchIndex(const quint32 bitmap, const quint32 bit) noexcept
		{
			return static_cast<qsizetype>(std::popcount(bitmap & (bit - 1U)));
		}

		/**
		 * @brief Obtains a transaction-owned directory node.
		 * @param source Existing immutable node.
		 * @param builderToken Current transaction identity.
		 * @return Editable node owned by this transaction.
		 */
		[[nodiscard]] static QSharedPointer<Node> editableNode(const QSharedPointer<const Node> &source,
		                                                       const quint64                     builderToken)
		{
			if (source && source->builderToken == builderToken)
				return qSharedPointerConstCast<Node>(source);
			auto result = source ? QSharedPointer<Node>::create(*source) : QSharedPointer<Node>::create();
			result->builderToken = builderToken;
			return result;
		}

		/**
		 * @brief Obtains a transaction-owned collision bucket.
		 * @param source Existing immutable bucket.
		 * @param builderToken Current transaction identity.
		 * @return Editable bucket owned by this transaction.
		 */
		[[nodiscard]] static QSharedPointer<Bucket> editableBucket(const QSharedPointer<const Bucket> &source,
		                                                           const quint64 builderToken)
		{
			if (source && source->builderToken == builderToken)
				return qSharedPointerConstCast<Bucket>(source);
			auto result = source ? QSharedPointer<Bucket>::create(*source) : QSharedPointer<Bucket>::create();
			result->builderToken = builderToken;
			return result;
		}

		/**
		 * @brief Applies one key update along its directory path.
		 * @param source Existing directory node.
		 * @param hash Complete key hash.
		 * @param level Directory level.
		 * @param key Exact key.
		 * @param value Replacement value, or empty to remove.
		 * @param builderToken Current transaction identity.
		 * @param mutationResult Output previous value and change status.
		 * @return Updated immutable directory node.
		 */
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

		/**
		 * @brief Visits all entries beneath a node.
		 * @param node Directory node.
		 * @param level Directory level.
		 * @param visitor Callback receiving keys and values.
		 */
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

		/**
		 * @brief Looks up an exact key beneath a directory root.
		 * @param root Directory root.
		 * @param key Exact key.
		 * @return Value or an empty pointer.
		 */
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
		/**
		 * @brief One replacement or removal applied by withUpdates().
		 */
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
				/**
				 * @brief Begins a transaction sharing an immutable base.
				 * @param source Published base map.
				 */
				explicit Transaction(const LuaCallbackPersistentStringMap &source)
				    : m_root(source.m_root), m_size(source.m_size), m_builderToken(nextBuilderToken())
				{
				}
				/**
				 * @brief Disables copying or moving an active transaction.
				 */
				Transaction(const Transaction &) = delete;
				/**
				 * @brief Disables copying or moving an active transaction.
				 * @return Assignment is unavailable.
				 */
				Transaction &operator=(const Transaction &) = delete;
				/**
				 * @brief Disables copying or moving an active transaction.
				 */
				Transaction(Transaction &&) = delete;
				/**
				 * @brief Disables copying or moving an active transaction.
				 * @return Assignment is unavailable.
				 */
				Transaction                 &operator=(Transaction &&) = delete;

				/**
				 * @brief Applies one replacement or removal.
				 * @param update Exact key update.
				 * @return Previous value and change status.
				 */
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

				/**
				 * @brief Looks up a transaction value.
				 * @param key Exact key.
				 * @return Current value or an empty pointer.
				 */
				[[nodiscard]] ValuePointer value(const QString &key) const
				{
					return valueInRoot(m_root, key);
				}

				/**
				 * @brief Checks transaction membership.
				 * @param key Exact key.
				 * @return Whether the key exists.
				 */
				[[nodiscard]] bool contains(const QString &key) const
				{
					return !value(key).isNull();
				}

				/**
				 * @brief Counts transaction entries.
				 * @return Current entry count.
				 */
				[[nodiscard]] qsizetype size() const noexcept
				{
					return m_size;
				}

				/**
				 * @brief Visits transaction entries.
				 * @param visitor Callback receiving each key and value.
				 */
				template <typename Visitor> void forEach(Visitor visitor) const
				{
					visitNode(m_root, kLevels - 1, visitor);
				}

				/**
				 * @brief Lists transaction keys.
				 * @return Keys in unspecified order.
				 */
				[[nodiscard]] QStringList keys() const
				{
					QStringList result;
					result.reserve(m_size);
					forEach([&result](const QString &key, const ValuePointer &) { result.append(key); });
					return result;
				}

				/**
				 * @brief Publishes this transaction and consumes its builder.
				 * @return Immutable map containing its mutations.
				 */
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

		/**
		 * @brief Looks up a published value.
		 * @param key Exact key.
		 * @return Stored value or an empty pointer.
		 */
		[[nodiscard]] ValuePointer value(const QString &key) const
		{
			return valueInRoot(m_root, key);
		}

		/**
		 * @brief Checks published membership.
		 * @param key Exact key.
		 * @return Whether the key exists.
		 */
		[[nodiscard]] bool contains(const QString &key) const
		{
			return !value(key).isNull();
		}

		/**
		 * @brief Counts published entries.
		 * @return Entry count.
		 */
		[[nodiscard]] qsizetype size() const noexcept
		{
			return m_size;
		}

		/**
		 * @brief Checks whether this map is empty.
		 * @return Whether there are no entries.
		 */
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

		/**
		 * @brief Visits published entries.
		 * @param visitor Callback receiving each key and value.
		 */
		template <typename Visitor> void forEach(Visitor visitor) const
		{
			visitNode(m_root, kLevels - 1, visitor);
		}

		/**
		 * @brief Lists published keys.
		 * @return Keys in unspecified order.
		 */
		[[nodiscard]] QStringList keys() const
		{
			QStringList result;
			result.reserve(m_size);
			forEach([&result](const QString &key, const ValuePointer &) { result.append(key); });
			return result;
		}

		/**
		 * @brief Checks immutable storage identity without comparing entries.
		 * @param other Map whose storage is compared.
		 * @return Whether both maps share the same root and entry count.
		 */
		[[nodiscard]] bool sharesStorageWith(const LuaCallbackPersistentStringMap &other) const noexcept
		{
			return m_root == other.m_root && m_size == other.m_size;
		}

		/**
		 * @brief Compares published maps, checking storage identity first.
		 * @param other Map to compare.
		 * @return Whether every key and value agrees.
		 */
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

#endif
