/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: VariableMapUtils.h
 * Role: Variable-name boundary normalization, canonical storage, and callback-local variable views.
 */

#ifndef QMUD_VARIABLEMAPUTILS_H
#define QMUD_VARIABLEMAPUTILS_H

#include <QHash>
#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>

namespace QMudVariableMapUtils
{
	/** @brief Variable name normalized once when external input enters the variable APIs. */
	class Name
	{
		public:
			/**
			 * @brief Normalizes an external lookup key by trimming and lowercasing it.
			 * @param input External variable name; its original spelling is retained for serialization.
			 */
			Name(const QString &input) : m_key(input.trimmed().toLower()), m_label(input)
			{
			}
			/**
			 * @brief Returns the canonical lookup key without further normalization.
			 * @return Canonical variable key.
			 */
			[[nodiscard]] const QString &key() const
			{
				return m_key;
			}
			/**
			 * @brief Returns the original external spelling.
			 * @return Variable label for serialization.
			 */
			[[nodiscard]] const QString &label() const
			{
				return m_label;
			}

		private:
			QString m_key;
			QString m_label;
	};

	using VariableMap = QMap<QString, QString>;

	/** @brief Canonical plugin values with original labels owned and mutated together. */
	class Store
	{
		public:
			/**
			 * @brief Returns the implicitly shared canonical value map for immutable snapshots.
			 * @return Variable values keyed by canonical names.
			 */
			[[nodiscard]] const VariableMap &values() const
			{
				return m_values;
			}
			/**
			 * @brief Returns a variable's original label.
			 * @param key Canonical variable key.
			 * @return Stored label, or an empty string when absent.
			 */
			[[nodiscard]] QString label(const QString &key) const
			{
				return m_labels.value(key);
			}
			/**
			 * @brief Reads a variable using an already canonical key.
			 * @param key Canonical variable key.
			 * @return Stored value, or an empty string when absent.
			 */
			[[nodiscard]] QString value(const QString &key) const
			{
				return m_values.value(key);
			}
			/**
			 * @brief Reports the number of variables.
			 * @return Variable count.
			 */
			[[nodiscard]] qsizetype size() const
			{
				return m_values.size();
			}
			/**
			 * @brief Visits original labels and values in canonical key order without per-entry lookups.
			 * @param visitor Callable receiving each label and value by const reference; must not mutate this store.
			 */
			template <typename Visitor> void forEachVariable(Visitor &&visitor) const
			{
				auto label = m_labels.cbegin();
				for (auto value = m_values.cbegin(); value != m_values.cend(); ++value, ++label)
				{
					Q_ASSERT(label != m_labels.cend() && label.key() == value.key());
					visitor(label.value(), value.value());
				}
				Q_ASSERT(label == m_labels.cend());
			}
			/**
			 * @brief Inserts or assigns a variable without detaching an unchanged value map.
			 * @param name Boundary-normalized name and original label.
			 * @param value New variable value.
			 * @return Whether the snapshot-visible value map changed.
			 */
			bool insert(const Name &name, const QString &value)
			{
				if (name.key().isEmpty())
					return false;
				const auto current = m_values.constFind(name.key());
				const bool changed = current == m_values.cend() || *current != value;
				if (changed)
					m_values.insert(name.key(), value);
				const auto currentLabel = m_labels.constFind(name.key());
				if (currentLabel == m_labels.cend() || *currentLabel != name.label())
					m_labels.insert(name.key(), name.label());
				return changed;
			}
			/**
			 * @brief Removes a variable and its serialization label.
			 * @param key Canonical variable key.
			 * @return Whether a variable was removed.
			 */
			bool remove(const QString &key)
			{
				if (!m_values.contains(key))
					return false;
				m_values.remove(key);
				m_labels.remove(key);
				return true;
			}
			/** @brief Clears all variables and labels. */
			void clear()
			{
				m_values.clear();
				m_labels.clear();
			}

		private:
			VariableMap m_values;
			VariableMap m_labels;
	};

	/**
	 * @brief Reads a variable using an already canonical name.
	 * @param variables Canonical variable values.
	 * @param name Canonical variable key.
	 * @param value Output value when found.
	 * @return Whether the variable exists.
	 */
	[[nodiscard]] inline bool findValue(const VariableMap &variables, const QString &name, QString &value)
	{
		const auto it = variables.constFind(name);
		if (it == variables.cend())
			return false;
		value = *it;
		return true;
	}

	/**
	 * @brief Assigns a canonical variable only when its value changes.
	 * @param variables Canonical variable values.
	 * @param name Canonical variable key.
	 * @param value New value.
	 */
	inline void insertOrAssign(VariableMap &variables, const QString &name, const QString &value)
	{
		const auto it = variables.constFind(name);
		if (it == variables.cend() || *it != value)
			variables.insert(name, value);
	}

	/**
	 * @brief Lane-local plugin variable view over an immutable captured map and explicit local writes.
	 * Cached observations never become writes. Full maps are materialized on demand and then maintained
	 * with Qt copy-on-write, so repeated list queries and snapshot publication reuse the same storage.
	 */
	class WorkingView
	{
		public:
			/** @brief Source knowledge, independent of individual cached variable values. */
			enum class Source
			{
				Uncaptured,
				Available,
				Unavailable
			};
			/** @brief Work performed while constructing complete or sparse snapshot projections. */
			struct Statistics
			{
					qsizetype cachedEntries{0};    ///< Resolved or written variable identities.
					qsizetype writtenEntries{0};   ///< Distinct explicitly written identities.
					quint64   materializations{0}; ///< Complete maps constructed from a captured base.
					quint64   projectedWrites{0};  ///< Entry applications to derived or outgoing maps.
			};
			/**
			 * @brief Reports whether the owner map was captured, omitted, or unavailable.
			 * @return Current source knowledge.
			 */
			[[nodiscard]] Source source() const
			{
				return m_source;
			}
			/**
			 * @brief Installs a captured map while preserving explicit writes made before backfill.
			 * @param values Complete canonical map captured for this owner.
			 */
			void capture(const VariableMap &values)
			{
				m_source = Source::Available;
				m_base   = values;
				m_materialized.reset();
				m_count = m_base.size();
				for (auto it = m_entries.begin(); it != m_entries.end();)
					it = it->written ? ++it : m_entries.erase(it);
				for (const QString &name : m_writtenKeys)
					m_count += static_cast<qsizetype>(m_entries.constFind(name)->value.has_value()) -
					           static_cast<qsizetype>(m_base.contains(name));
			}
			/** @brief Invalidates all observations and writes when this plugin instance becomes unavailable. */
			void makeUnavailable()
			{
				*this    = WorkingView{};
				m_source = Source::Unavailable;
			}
			/**
			 * @brief Resolves a canonical variable from the local cache or captured base without constructing a map.
			 * @param name Canonical variable name.
			 * @param value Receives the value when present, including an empty string.
			 * @param resolved Receives whether presence is known; an omitted base is not a known miss.
			 * @return Whether a value is present.
			 */
			bool find(const QString &name, QString &value, bool &resolved)
			{
				resolved = m_source != Source::Uncaptured;
				if (m_source == Source::Unavailable)
					return false;
				if (const auto it = m_entries.constFind(name); it != m_entries.cend())
				{
					resolved = true;
					if (it->value)
						value = *it->value;
					return it->value.has_value();
				}
				if (!resolved)
					return false;
				const auto found   = m_base.constFind(name);
				const bool present = found != m_base.cend();
				if (present)
					value = *found;
				remember(name, present, value);
				return present;
			}
			/**
			 * @brief Caches a resolved observation without replacing an explicit write.
			 * @param name Canonical variable name.
			 * @param present Whether the variable exists.
			 * @param value Observed value when present.
			 */
			void remember(const QString &name, const bool present, const QString &value)
			{
				auto &entry = m_entries[name];
				if (!entry.written)
					entry.value = present ? std::optional<QString>(value) : std::nullopt;
			}
			/**
			 * @brief Records local write visibility without changing executable journal ownership or order.
			 * @param name Canonical variable name.
			 * @param value Assigned value, ignored for deletion.
			 * @param remove Whether to delete the variable.
			 */
			void write(const QString &name, const QString &value, const bool remove)
			{
				auto       it      = m_entries.find(name);
				const bool present = it != m_entries.end() ? it->value.has_value() : m_base.contains(name);
				if (it == m_entries.end())
					it = m_entries.insert(name, Entry{});
				if (!it->written)
				{
					it->written = true;
					m_writtenKeys.push_back(name);
				}
				it->value = remove ? std::nullopt : std::optional<QString>(value);
				if (m_source == Source::Available)
					m_count += static_cast<qsizetype>(!remove) - static_cast<qsizetype>(present);
				if (m_materialized)
					applyEntry(*m_materialized, name, *it);
			}
			/**
			 * @brief Returns the local variable count only when a complete base is known.
			 * @return Count, or no value for an omitted or unavailable owner map.
			 */
			[[nodiscard]] std::optional<qsizetype> count() const
			{
				return m_source == Source::Available ? std::optional<qsizetype>(m_count) : std::nullopt;
			}
			/**
			 * @brief Reports whether this view contains explicit writes.
			 * @return Whether a mutation projection is required.
			 */
			[[nodiscard]] bool hasWrites() const
			{
				return !m_writtenKeys.isEmpty();
			}
			/**
			 * @brief Obtains a reusable complete map, materializing only explicitly written keys.
			 * @return Borrowed map valid until this view changes, or null when no complete base exists.
			 */
			[[nodiscard]] const VariableMap *values() const
			{
				if (m_source != Source::Available)
					return nullptr;
				if (m_writtenKeys.isEmpty())
					return &m_base;
				if (!m_materialized)
				{
					m_materialized = m_base;
					++m_materializations;
					for (const QString &name : m_writtenKeys)
						applyEntry(*m_materialized, name, *m_entries.constFind(name));
				}
				return &*m_materialized;
			}
			/**
			 * @brief Projects writes into an owning snapshot while keeping cached reads private.
			 * @param target Existing target map; unrelated entries survive sparse projection.
			 */
			void project(VariableMap &target) const
			{
				if (!hasWrites())
					return;
				if (const auto complete = values())
					target = *complete;
				else if (m_source == Source::Uncaptured)
					for (const QString &name : m_writtenKeys)
						applyEntry(target, name, *m_entries.constFind(name));
			}
			/**
			 * @brief Returns bounded-work diagnostics without changing the view.
			 * @return Current cache sizes and map-construction work counts.
			 */
			[[nodiscard]] Statistics statistics() const
			{
				return {m_entries.size(), m_writtenKeys.size(), m_materializations, m_projectedWrites};
			}

		private:
			/** @brief One known value or miss, distinguished from an explicit assignment or deletion. */
			struct Entry
			{
					std::optional<QString> value;
					bool                   written{false};
			};
			/**
			 * @brief Applies one explicit entry without detaching an unchanged target map.
			 * @param target Map receiving the entry.
			 * @param name Canonical variable name.
			 * @param entry Written value or deletion.
			 */
			void applyEntry(VariableMap &target, const QString &name, const Entry &entry) const
			{
				++m_projectedWrites;
				if (entry.value)
					insertOrAssign(target, name, *entry.value);
				else if (target.contains(name))
					target.remove(name);
			}
			Source                             m_source{Source::Uncaptured};
			VariableMap                        m_base;
			QHash<QString, Entry>              m_entries;
			QStringList                        m_writtenKeys;
			qsizetype                          m_count{0};
			mutable std::optional<VariableMap> m_materialized;
			mutable quint64                    m_materializations{0};
			mutable quint64                    m_projectedWrites{0};
	};
} // namespace QMudVariableMapUtils

#endif // QMUD_VARIABLEMAPUTILS_H
