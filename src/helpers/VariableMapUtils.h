/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: VariableMapUtils.h
 * Role: Variable-name boundary normalization and canonical plugin variable storage.
 */

#ifndef QMUD_VARIABLEMAPUTILS_H
#define QMUD_VARIABLEMAPUTILS_H

#include <QMap>
#include <QString>

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
} // namespace QMudVariableMapUtils

#endif // QMUD_VARIABLEMAPUTILS_H
