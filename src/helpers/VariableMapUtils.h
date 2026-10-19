/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: VariableMapUtils.h
 * Role: Shared case-insensitive operations for world and plugin variable maps.
 */

#ifndef QMUD_VARIABLEMAPUTILS_H
#define QMUD_VARIABLEMAPUTILS_H

#include <QMap>
#include <QString>
#include <optional>

namespace QMudVariableMapUtils
{
	using VariableMap = QMap<QString, QString>;

	/**
	 * @brief Finds a variable case-insensitively.
	 * @param variables Variable map to search.
	 * @param name Variable name to resolve.
	 * @return Iterator to the variable, or `variables.cend()` when absent.
	 */
	[[nodiscard]] inline VariableMap::const_iterator find(const VariableMap &variables, const QString &name)
	{
		for (auto it = variables.cbegin(); it != variables.cend(); ++it)
		{
			if (it.key().compare(name, Qt::CaseInsensitive) == 0)
				return it;
		}
		return variables.cend();
	}

	/**
	 * @brief Finds a mutable variable case-insensitively.
	 * @param variables Variable map to search.
	 * @param name Variable name to resolve.
	 * @return Iterator to the variable, or `variables.end()` when absent.
	 */
	[[nodiscard]] inline VariableMap::iterator find(VariableMap &variables, const QString &name)
	{
		for (auto it = variables.begin(); it != variables.end(); ++it)
		{
			if (it.key().compare(name, Qt::CaseInsensitive) == 0)
				return it;
		}
		return variables.end();
	}

	/**
	 * @brief Resolves the stored spelling of a variable name case-insensitively.
	 * @param variables Variable map to search.
	 * @param name Variable name to resolve.
	 * @return Stored key when found, otherwise no value.
	 */
	[[nodiscard]] inline std::optional<QString> matchingKey(const VariableMap &variables, const QString &name)
	{
		const auto it = find(variables, name);
		return it == variables.cend() ? std::nullopt : std::optional<QString>(it.key());
	}

	/**
	 * @brief Reads a variable value case-insensitively.
	 * @param variables Variable map to search.
	 * @param name Variable name to resolve.
	 * @param value Output value when found.
	 * @return `true` when the variable exists.
	 */
	[[nodiscard]] inline bool findValue(const VariableMap &variables, const QString &name, QString &value)
	{
		const auto it = find(variables, name);
		if (it == variables.cend())
			return false;
		value = it.value();
		return true;
	}

	/**
	 * @brief Inserts or updates a variable without changing an existing key's spelling.
	 * @param variables Variable map to modify.
	 * @param name Variable name to insert or resolve case-insensitively.
	 * @param value Replacement value.
	 */
	inline void insertOrAssign(VariableMap &variables, const QString &name, const QString &value)
	{
		if (const auto it = find(variables, name); it != variables.end())
			it.value() = value;
		else
			variables.insert(name, value);
	}

	/**
	 * @brief Removes a variable case-insensitively.
	 * @param variables Variable map to modify.
	 * @param name Variable name to remove.
	 * @return `true` when a variable was removed.
	 */
	[[nodiscard]] inline bool remove(VariableMap &variables, const QString &name)
	{
		const auto it = find(variables, name);
		if (it == variables.end())
			return false;
		variables.erase(it);
		return true;
	}
} // namespace QMudVariableMapUtils

#endif // QMUD_VARIABLEMAPUTILS_H
