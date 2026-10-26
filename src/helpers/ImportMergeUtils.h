/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: ImportMergeUtils.h
 * Role: Shared XML-import merge helpers covering duplicate handling policies across named lists and variables.
 */

#ifndef QMUD_IMPORTMERGEUTILS_H
#define QMUD_IMPORTMERGEUTILS_H

#include "VariableMapUtils.h"

#include <QHash>
#include <QMap>
#include <QString>
#include <utility>

namespace QMudImportMerge
{
	/**
	 * @brief Merges named import items into destination list with duplicate policy controls.
	 * @tparam Item Item type exposing `attributes` map with `name`.
	 * @param dest Destination list to update.
	 * @param src Source list to merge from.
	 * @param kind User-visible item kind label for error text.
	 * @param allowOverwrite Replace existing entries on duplicate name when `true`.
	 * @param duplicates Optional duplicate counter output.
	 * @param errorMessage Optional error output when merge stops on duplicate.
	 * @return `true` when merge succeeds.
	 */
	template <typename Item>
	bool mergeNamedList(QList<Item> &dest, const QList<Item> &src, const QString &kind,
	                    const bool allowOverwrite, int *duplicates, QString *errorMessage)
	{
		QHash<QString, int> indexByName;
		indexByName.reserve(dest.size() + src.size());
		for (int i = 0; i < dest.size(); ++i)
		{
			const QString name = dest[i].attributes.value(QStringLiteral("name")).trimmed().toCaseFolded();
			if (!name.isEmpty())
				indexByName.insert(name, i);
		}

		for (const auto &item : src)
		{
			const QString rawName = item.attributes.value(QStringLiteral("name")).trimmed();
			const QString key     = rawName.toCaseFolded();
			if (key.isEmpty() || !indexByName.contains(key))
			{
				dest.push_back(item);
				if (!key.isEmpty())
					indexByName.insert(key, dest.size() - 1);
				continue;
			}

			if (allowOverwrite)
			{
				Item replacement = item;
				if constexpr (requires(Item value) {
					              value.runtimeId;
					              value.runtimeLocationHandle;
				              })
				{
					const Item &existing              = dest.at(indexByName.value(key));
					replacement.runtimeId             = existing.runtimeId;
					replacement.runtimeLocationHandle = existing.runtimeLocationHandle;
				}
				dest[indexByName.value(key)] = std::move(replacement);
				if (duplicates)
					++(*duplicates);
				continue;
			}

			if (errorMessage)
				*errorMessage = QStringLiteral("Duplicate %1 label \"%2\"").arg(kind, rawName);
			return false;
		}
		return true;
	}

	/**
	 * @brief Merges a variable list using overwrite and duplicate-counting policies.
	 * @tparam VariableItem Variable item type exposing `attributes` map with `name` and `content`.
	 * @param dest Destination list to update.
	 * @param src Source list to merge from.
	 * @param allowOverwrite Replace existing entries on duplicate name when `true`.
	 * @param duplicates Optional duplicate/change counter output.
	 */
	template <typename VariableItem>
	void mergeVariables(QList<VariableItem> &dest, const QList<VariableItem> &src, const bool allowOverwrite,
	                    int *duplicates)
	{
		QHash<QString, int> indexByName;
		indexByName.reserve(dest.size() + src.size());
		for (int i = 0; i < dest.size(); ++i)
		{
			const QString name =
			    QMudVariableMapUtils::Name(dest[i].attributes.value(QStringLiteral("name"))).key();
			if (!name.isEmpty())
				indexByName.insert(name, i);
		}

		for (const auto &item : src)
		{
			const QString key =
			    QMudVariableMapUtils::Name(item.attributes.value(QStringLiteral("name"))).key();
			if (key.isEmpty() || !indexByName.contains(key))
			{
				dest.push_back(item);
				if (!key.isEmpty())
					indexByName.insert(key, static_cast<int>(dest.size() - 1));
				continue;
			}

			const int existingIndex = indexByName.value(key);
			if (allowOverwrite)
			{
				dest[existingIndex] = item;
				if (duplicates)
					++(*duplicates);
				continue;
			}

			if (dest[existingIndex].content != item.content && duplicates)
				++(*duplicates);
			dest[existingIndex] = item;
		}
	}

	/**
	 * @brief Merges imported variables into a plugin variable map using case-insensitive names.
	 * @param dest Destination plugin variables.
	 * @param src Imported name/value pairs.
	 * @param allowOverwrite Replace existing values when `true`.
	 * @param duplicates Optional duplicate/change counter output.
	 */
	inline void mergePluginVariables(QMudVariableMapUtils::Store          &dest,
	                                 const QList<QPair<QString, QString>> &src, const bool allowOverwrite,
	                                 int *duplicates)
	{
		for (const auto &[rawName, value] : src)
		{
			const QMudVariableMapUtils::Name name(rawName);
			if (name.key().isEmpty())
				continue;
			const auto existing = dest.values().constFind(name.key());
			if (existing == dest.values().cend())
			{
				dest.insert(name, value);
				continue;
			}
			if (allowOverwrite)
			{
				dest.insert(name, value);
				if (duplicates)
					++(*duplicates);
			}
			else if (*existing != value && duplicates)
				++(*duplicates);
		}
	}
} // namespace QMudImportMerge

#endif // QMUD_IMPORTMERGEUTILS_H
