/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: FileExtensions.h
 * Role: Centralizes legacy-to-modern QMud file-extension mappings and path normalization helpers used by
 * migration and file-dialog workflows.
 */

#ifndef QMUD_FILEEXTENSIONS_H
#define QMUD_FILEEXTENSIONS_H

#include <QFileInfo>
#include <QList>
#include <QString>

namespace QMudFileExtensions
{
	struct ExtensionMapping
	{
			const char *legacy;
			const char *modern;
	};

	/**
	 * @brief Returns legacy/modern extension mapping table.
	 */
	inline const QList<ExtensionMapping> &mappings()
	{
		static const QList<ExtensionMapping> kMappings = {
		    {"mcl", "qdl"},
            {"mct", "qdt"},
            {"mca", "qda"},
            {"mci", "qdi"},
		    {"mcc", "qdc"},
            {"mcm", "qdm"},
            {"mcv", "qdv"},
		};
		return kMappings;
	}

	/**
	 * @brief Replaces existing suffix or appends one when absent.
	 */
	inline QString replaceOrAppendExtension(const QString &path, const QString &extensionLower)
	{
		if (path.isEmpty())
			return path;
		const qsizetype slash = qMax(path.lastIndexOf(QLatin1Char('/')), path.lastIndexOf(QLatin1Char('\\')));
		const qsizetype dot   = path.lastIndexOf(QLatin1Char('.'));
		if (dot > slash)
			return path.left(dot + 1) + extensionLower;
		return path + QLatin1Char('.') + extensionLower;
	}

	/**
	 * @brief Resolves modern extension for given suffix.
	 */
	inline QString modernForSuffix(const QString &suffixLower)
	{
		for (const ExtensionMapping &mapping : mappings())
		{
			if (suffixLower == QLatin1String(mapping.legacy) || suffixLower == QLatin1String(mapping.modern))
			{
				return QString::fromUtf8(mapping.modern);
			}
		}
		return {};
	}

	/**
	 * @brief Returns true when suffix is world-file extension.
	 */
	inline bool isWorldSuffix(const QString &suffixLower)
	{
		return suffixLower == QLatin1String("mcl") || suffixLower == QLatin1String("qdl");
	}

	/**
	 * @brief Returns true when suffix is legacy world extension.
	 */
	inline bool isLegacyWorldSuffix(const QString &suffixLower)
	{
		return suffixLower == QLatin1String("mcl");
	}

	/**
	 * @brief Canonicalizes file path extension to modern suffix.
	 */
	inline QString canonicalizePathExtension(const QString &path, bool *changed = nullptr)
	{
		const QString suffixLower = QFileInfo(path).suffix().toLower();
		const QString modern      = modernForSuffix(suffixLower);
		if (modern.isEmpty())
		{
			if (changed)
				*changed = false;
			return path;
		}
		const QString output = replaceOrAppendExtension(path, modern);
		if (changed)
			*changed = (output != path);
		return output;
	}

} // namespace QMudFileExtensions

#endif // QMUD_FILEEXTENSIONS_H
