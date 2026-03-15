/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: ImportMergeUtils.h
 * Role: Shared XML-import merge helpers covering duplicate handling policies across named lists and variables.
 */

#ifndef QMUD_IMPORTMERGEUTILS_H
#define QMUD_IMPORTMERGEUTILS_H

#include <QMap>
#include <QString>

namespace QMudImportMerge
{
	/**
	 * @brief Merges named import items into destination list with duplicate policy controls.
	 * @tparam Item Item type exposing `attributes` map with `name`.
	 * @param dest Destination list to update.
	 * @param src Source list to merge from.
	 * @param kind User-visible item kind label for error text.
	 * @param allowOverwrite Replace existing entries on duplicate name when `true`.
	 * @param allowPasteDuplicate Keep both entries on duplicate name when `true`.
	 * @param duplicates Optional duplicate counter output.
	 * @param errorMessage Optional error output when merge stops on duplicate.
	 * @return `true` when merge succeeds.
	 */
	template <typename Item>
	bool mergeNamedList(QList<Item> &dest, const QList<Item> &src, const QString &kind,
	                    const bool allowOverwrite, const bool allowPasteDuplicate, int *duplicates,
	                    QString *errorMessage)
	{
		QMap<QString, int> indexByName;
		for (int i = 0; i < dest.size(); ++i)
		{
			const QString name = dest[i].attributes.value(QStringLiteral("name")).trimmed().toLower();
			if (!name.isEmpty())
				indexByName.insert(name, i);
		}

		for (const auto &item : src)
		{
			const QString rawName = item.attributes.value(QStringLiteral("name")).trimmed();
			const QString key     = rawName.toLower();
			if (key.isEmpty() || !indexByName.contains(key))
			{
				dest.push_back(item);
				if (!key.isEmpty())
					indexByName.insert(key, dest.size() - 1);
				continue;
			}

			if (allowOverwrite)
			{
				dest[indexByName.value(key)] = item;
				if (duplicates)
					++(*duplicates);
				continue;
			}

			if (allowPasteDuplicate)
			{
				dest.push_back(item);
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
	 * @brief Merges variable list using overwrite/paste-duplicate policies.
	 * @tparam VariableItem Variable item type exposing `attributes` map with `name` and `content`.
	 * @param dest Destination list to update.
	 * @param src Source list to merge from.
	 * @param allowOverwrite Replace existing entries on duplicate name when `true`.
	 * @param allowPasteDuplicate Keep both entries on duplicate name when `true`.
	 * @param duplicates Optional duplicate/change counter output.
	 */
	template <typename VariableItem>
	void mergeVariables(QList<VariableItem> &dest, const QList<VariableItem> &src, const bool allowOverwrite,
	                    const bool allowPasteDuplicate, int *duplicates)
	{
		QMap<QString, int> indexByName;
		for (int i = 0; i < dest.size(); ++i)
		{
			const QString name = dest[i].attributes.value(QStringLiteral("name")).trimmed().toLower();
			if (!name.isEmpty())
				indexByName.insert(name, i);
		}

		for (const auto &item : src)
		{
			const QString rawName = item.attributes.value(QStringLiteral("name")).trimmed();
			const QString key     = rawName.toLower();
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

			if (allowPasteDuplicate)
			{
				dest.push_back(item);
				if (duplicates)
					++(*duplicates);
				continue;
			}

			if (dest[existingIndex].content != item.content && duplicates)
				++(*duplicates);
			dest[existingIndex] = item;
		}
	}
} // namespace QMudImportMerge

#endif // QMUD_IMPORTMERGEUTILS_H
