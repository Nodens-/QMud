/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: FileExtensions.h
 * Role: Defines QMud file-association metadata and centralizes extension handling across registration,
 * migration, and file-dialog workflows.
 */

#ifndef QMUD_FILEEXTENSIONS_H
#define QMUD_FILEEXTENSIONS_H

#include <QFileInfo>
#include <QList>
#include <QString>
// ReSharper disable once CppUnusedIncludeDirective
#include <QStringList>

#include <string_view>

namespace QMudFileExtensions
{
	/**
	 * @brief Describes one semantic QMud document type and its platform identifiers.
	 */
	struct Entry
	{
			const char *programIdSuffix;
			const char *description;
			const char *mimeType;
			const char *modernExtension;
			const char *legacyExtension;
			const char *uniformTypeIdentifier;
	};

	/**
	 * @brief Returns the authoritative QMud file-association table.
	 *
	 * CMake parses these initializer rows to generate the macOS bundle declarations. Keep every entry as one
	 * aggregate initializer containing exactly the six fields declared by Entry.
	 *
	 * @return File-association entries shared by runtime registration and bundle generation.
	 */
	inline const QList<Entry> &entries()
	{
		static const QList<Entry> kEntries = {
		    {"World",     "QMud World File",    "application/x-qmud-world",     "qdl", "mcl",
		     "com.abnormalfrequency.qmud.world"    },
		    {"Triggers",  "QMud Trigger File",  "application/x-qmud-triggers",  "qdt", "mct",
		     "com.abnormalfrequency.qmud.triggers" },
		    {"Aliases",   "QMud Alias File",    "application/x-qmud-aliases",   "qda", "mca",
		     "com.abnormalfrequency.qmud.aliases"  },
		    {"Timers",    "QMud Timer File",    "application/x-qmud-timers",    "qdi", "mci",
		     "com.abnormalfrequency.qmud.timers"   },
		    {"Colours",   "QMud Colour File",   "application/x-qmud-colours",   "qdc", "mcc",
		     "com.abnormalfrequency.qmud.colours"  },
		    {"Macros",    "QMud Macro File",    "application/x-qmud-macros",    "qdm", "mcm",
		     "com.abnormalfrequency.qmud.macros"   },
		    {"Variables", "QMud Variable File", "application/x-qmud-variables", "qdv", "mcv",
		     "com.abnormalfrequency.qmud.variables"},
		};
		return kEntries;
	}

	/**
	 * @brief Finds an association by its platform program identifier suffix.
	 * @param programIdSuffix Program identifier suffix to find.
	 * @return Matching entry, or `nullptr` when the suffix is unknown.
	 */
	inline const Entry *findByProgramIdSuffix(const std::string_view programIdSuffix)
	{
		for (const Entry &entry : entries())
		{
			if (entry.programIdSuffix == programIdSuffix)
				return &entry;
		}
		return nullptr;
	}

	/**
	 * @brief Finds an association by a filename extension without its leading dot.
	 * @param extension Filename extension to find.
	 * @return Matching entry, or `nullptr` when the extension is not registered.
	 */
	inline const Entry *findByExtension(const QString &extension)
	{
		for (const Entry &entry : entries())
		{
			if (extension.compare(QString::fromLatin1(entry.modernExtension), Qt::CaseInsensitive) == 0 ||
			    extension.compare(QString::fromLatin1(entry.legacyExtension), Qt::CaseInsensitive) == 0)
			{
				return &entry;
			}
		}
		return nullptr;
	}

	/**
	 * @brief Extracts the sole registered document path from a process argument list.
	 * @param arguments Process arguments including the executable at index zero.
	 * @return The exact path argument, or an empty string for any command line containing other operands.
	 */
	inline QString fileAssociationPathArgument(const QStringList &arguments)
	{
		QString path;
		for (qsizetype index = 1; index < arguments.size(); ++index)
		{
			const QString &argument = arguments.at(index);
			if (argument.compare(QStringLiteral("--noauto"), Qt::CaseInsensitive) == 0 ||
			    argument.compare(QStringLiteral("--multi-instance"), Qt::CaseInsensitive) == 0 ||
			    argument.compare(QStringLiteral("--allow-multi-instance"), Qt::CaseInsensitive) == 0)
			{
				continue;
			}
			if (!path.isEmpty() || !findByExtension(QFileInfo(argument).suffix()))
				return {};
			path = argument;
		}
		return path;
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
		for (const Entry &entry : entries())
		{
			if (suffixLower == QLatin1String(entry.legacyExtension) ||
			    suffixLower == QLatin1String(entry.modernExtension))
			{
				return QString::fromUtf8(entry.modernExtension);
			}
		}
		return {};
	}

	/**
	 * @brief Returns true when suffix is world-file extension.
	 */
	inline bool isWorldSuffix(const QString &suffixLower)
	{
		const Entry *const world = findByProgramIdSuffix("World");
		return world && (suffixLower == QLatin1String(world->legacyExtension) ||
		                 suffixLower == QLatin1String(world->modernExtension));
	}

	/**
	 * @brief Returns true when suffix is legacy world extension.
	 */
	inline bool isLegacyWorldSuffix(const QString &suffixLower)
	{
		const Entry *const world = findByProgramIdSuffix("World");
		return world && suffixLower == QLatin1String(world->legacyExtension);
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
