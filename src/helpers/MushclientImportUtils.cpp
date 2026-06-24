/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MushclientImportUtils.cpp
 * Role: Shared MUSHclient import and legacy path/config migration helper implementations.
 */

#include "MushclientImportUtils.h"

#include "FileExtensions.h"
#include "NativePluginRegistry.h"
#include "PluginPathUtils.h"
#include "WorldDocument.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QStringConverter>
// ReSharper disable once CppUnusedIncludeDirective
#include <QStringDecoder>
#include <QVariant>
// ReSharper disable once CppUnusedIncludeDirective
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <QtGlobal>
// ReSharper disable once CppUnusedIncludeDirective
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>
#include <QtSql/QSqlRecord>

namespace
{
	QString relativePathUnderBase(const QString &baseDir, const QString &absolutePath);

	QString normalizePathString(const QString &input)
	{
		QString output = input;
		output.replace(QLatin1Char('\\'), QLatin1Char('/'));
		return output;
	}

	QString stripOptionalQuotes(const QString &value)
	{
		if (value.size() < 2)
			return value;
		if (const QChar first = value.front(), last = value.back();
		    (first == QLatin1Char('"') && last == QLatin1Char('"')) ||
		    (first == QLatin1Char('\'') && last == QLatin1Char('\'')))
			return value.mid(1, value.size() - 2);
		return value;
	}

	QString absolutePathFromBase(const QString &baseDir, const QString &path)
	{
		auto looksLikeWindowsDrivePath = [](const QString &value, const int offset = 0) -> bool
		{
			return value.size() > offset + 1 && value.at(offset).isLetter() &&
			       value.at(offset + 1) == QLatin1Char(':');
		};

		QString normalized = normalizePathString(path.trimmed());
		while (normalized.startsWith(QStringLiteral("./")))
			normalized = normalized.mid(2);
		if (normalized.startsWith(QLatin1Char('/')) && looksLikeWindowsDrivePath(normalized, 1))
			normalized = normalized.mid(1);
		if (QFileInfo(normalized).isAbsolute() || looksLikeWindowsDrivePath(normalized))
			return QDir::cleanPath(normalized);
		return QDir::cleanPath(QDir(baseDir).filePath(normalized));
	}

	bool hasWindowsDrivePath(const QString &path)
	{
		return path.size() > 1 && path.at(0).isLetter() && path.at(1) == QLatin1Char(':');
	}

	bool hasPathSyntax(const QString &path)
	{
		const QString normalized = normalizePathString(path).trimmed();
		return QFileInfo(normalized).isAbsolute() || hasWindowsDrivePath(normalized) ||
		       normalized.contains(QLatin1Char('/')) || normalized.startsWith(QLatin1Char('.'));
	}

	QString resolveExistingPathCaseInsensitive(const QString &path)
	{
		QString cleaned = QDir::cleanPath(path);
		if (cleaned.isEmpty())
			return {};
		if (QFileInfo::exists(cleaned))
			return cleaned;
#ifdef Q_OS_WIN
		return {};
#else
		if (!QDir::isAbsolutePath(cleaned))
			return {};
		QStringList segments = cleaned.split(QLatin1Char('/'), Qt::SkipEmptyParts);
		QString     current  = QStringLiteral("/");
		for (const QString &segment : segments)
		{
			const QDir      dir(current);
			const QFileInfo matched = [&]() -> QFileInfo
			{
				const QFileInfoList entries =
				    dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
				for (const QFileInfo &entry : entries)
				{
					if (entry.fileName().compare(segment, Qt::CaseInsensitive) == 0)
						return entry;
				}
				return {};
			}();
			if (!matched.exists())
				return {};
			current = matched.absoluteFilePath();
		}
		return QDir::cleanPath(current);
#endif
	}

	QString dotRelativeStoragePath(const QString &qmudHome, const QString &value)
	{
		QString normalized = QMudFileExtensions::canonicalizePathExtension(normalizePathString(value));
		if (normalized.trimmed().isEmpty())
			return {};
		QString relative = QMudPluginPathUtils::qmudHomeRelativePath(qmudHome, normalized, false);
		if (relative.isEmpty())
			return {};
		if (relative == QLatin1String("."))
			relative = QStringLiteral("./");
		if (!relative.startsWith(QStringLiteral("./")) && !relative.startsWith(QStringLiteral("../")))
			relative.prepend(QStringLiteral("./"));
		return relative;
	}

	QString storagePathFromAbsolute(const QString &baseDir, const QString &absolutePath)
	{
		return dotRelativeStoragePath(baseDir, absolutePath);
	}

	QString containedLegacyStoragePath(const QString &destinationBaseDir, const QString &path)
	{
		QString normalized = stripOptionalQuotes(normalizePathString(path).trimmed());
		if (normalized.isEmpty())
			return normalized;

		if (QFileInfo(normalized).isAbsolute() || hasWindowsDrivePath(normalized))
		{
			const QString fileName = QFileInfo(normalized).fileName();
			if (fileName.isEmpty())
				return QStringLiteral("./");
			return dotRelativeStoragePath(destinationBaseDir, QDir(destinationBaseDir).filePath(fileName));
		}

		const QString portableRelative = QMudPluginPathUtils::legacyPathRelativeToQmudHome(normalized);
		if (portableRelative.isEmpty() || portableRelative.startsWith(QStringLiteral("../")) ||
		    QDir::isAbsolutePath(portableRelative))
		{
			const QString fileName = QFileInfo(normalized).fileName();
			if (fileName.isEmpty())
				return QStringLiteral("./");
			return dotRelativeStoragePath(destinationBaseDir, QDir(destinationBaseDir).filePath(fileName));
		}

		return dotRelativeStoragePath(destinationBaseDir,
		                              QDir(destinationBaseDir).filePath(portableRelative));
	}

	QString containedPluginStoragePath(const QString &destinationBaseDir, const QString &path)
	{
		QString normalized = stripOptionalQuotes(normalizePathString(path).trimmed());
		if (normalized.isEmpty())
			return normalized;

		const QString portableRelative = QMudPluginPathUtils::legacyPathRelativeToQmudHome(normalized);
		if (portableRelative.startsWith(QStringLiteral("worlds/plugins/"), Qt::CaseInsensitive) ||
		    portableRelative.startsWith(QStringLiteral("plugins/"), Qt::CaseInsensitive))
		{
			const QString relative =
			    portableRelative.startsWith(QStringLiteral("plugins/"), Qt::CaseInsensitive)
			        ? QStringLiteral("worlds/") + portableRelative
			        : portableRelative;
			return dotRelativeStoragePath(destinationBaseDir, QDir(destinationBaseDir).filePath(relative));
		}

		const QString fileName = QFileInfo(normalized).fileName();
		if (fileName.isEmpty())
			return containedLegacyStoragePath(destinationBaseDir, normalized);
		return dotRelativeStoragePath(
		    destinationBaseDir,
		    QDir(destinationBaseDir).filePath(QStringLiteral("worlds/plugins/") + fileName));
	}

	QString containedWorldStoragePath(const QString &destinationBaseDir, const QString &canonicalPath)
	{
		QString normalized = stripOptionalQuotes(normalizePathString(canonicalPath).trimmed());
		if (normalized.isEmpty())
			return normalized;

		QString normalizedRelative = normalized;
		while (normalizedRelative.startsWith(QStringLiteral("./")))
			normalizedRelative = normalizedRelative.mid(2);
		const bool isAbsolute =
		    QFileInfo(normalizedRelative).isAbsolute() || hasWindowsDrivePath(normalizedRelative);
		QString portableRelative = QMudPluginPathUtils::legacyPathRelativeToQmudHome(normalizedRelative);
		if (portableRelative.startsWith(QStringLiteral("worlds/"), Qt::CaseInsensitive))
		{
			const QString relativeWorld =
			    QMudFileExtensions::canonicalizePathExtension(portableRelative.mid(7));
			return dotRelativeStoragePath(
			    destinationBaseDir,
			    QDir(destinationBaseDir).filePath(QStringLiteral("worlds/") + relativeWorld));
		}

		if (!isAbsolute && !portableRelative.isEmpty() &&
		    !portableRelative.startsWith(QStringLiteral("../")) && !QDir::isAbsolutePath(portableRelative))
		{
			return dotRelativeStoragePath(
			    destinationBaseDir,
			    QDir(destinationBaseDir)
			        .filePath(QStringLiteral("worlds/") +
			                  QMudFileExtensions::canonicalizePathExtension(portableRelative)));
		}

		const QString fileName = QFileInfo(normalized).fileName();
		if (fileName.isEmpty())
			return QStringLiteral("./worlds/");
		return dotRelativeStoragePath(
		    destinationBaseDir, QDir(destinationBaseDir).filePath(QStringLiteral("worlds/") + fileName));
	}

	QString containedWorldAbsolutePath(const QString &sourceBaseDir, const QString &destinationBaseDir,
	                                   const QString &canonicalPath, const QString &resolvedLegacyPath)
	{
		if (!resolvedLegacyPath.isEmpty())
		{
			const QString relativeWorld = relativePathUnderBase(
			    QDir(sourceBaseDir).filePath(QStringLiteral("worlds")), resolvedLegacyPath);
			if (!relativeWorld.isEmpty())
			{
				return QDir(destinationBaseDir)
				    .filePath(QStringLiteral("worlds/") +
				              QMudFileExtensions::canonicalizePathExtension(relativeWorld));
			}
		}

		const QString storagePath = containedWorldStoragePath(destinationBaseDir, canonicalPath);
		return absolutePathFromBase(destinationBaseDir, storagePath);
	}

	QString remapLegacyWindowsWorldPathToBase(const QString &baseDir, const QString &path)
	{
		const QString normalized = normalizePathString(path.trimmed());
		const bool    hasDrive =
		    normalized.size() > 1 && normalized.at(0).isLetter() && normalized.at(1) == QLatin1Char(':');
		if (!hasDrive)
			return {};
		const qsizetype worldsPos = normalized.indexOf(QStringLiteral("/worlds/"), 0, Qt::CaseInsensitive);
		if (worldsPos < 0)
			return {};
		const QString relativeWorldPath = normalized.mid(worldsPos + 1);
		return absolutePathFromBase(baseDir, relativeWorldPath);
	}

	QString archiveRelativePathFor(const QString &baseDir, const QString &absolutePath)
	{
		QString relative = QDir(baseDir).relativeFilePath(absolutePath);
		if (relative == QStringLiteral("..") || relative.startsWith(QStringLiteral("../")) ||
		    QDir::isAbsolutePath(relative))
		{
			relative = QDir::cleanPath(absolutePath);
#ifdef Q_OS_WIN
			if (relative.size() > 1 && relative.at(1) == QLatin1Char(':'))
				relative.replace(1, 1, QStringLiteral("_"));
#endif
			while (relative.startsWith(QLatin1Char('/')))
				relative.remove(0, 1);
		}
		return normalizePathString(relative);
	}

	bool moveFileToArchive(const QString &sourcePath, const QString &targetPath)
	{
		const QFileInfo targetInfo(targetPath);
		if (const QString targetDir = targetInfo.absolutePath(); !QDir().mkpath(targetDir))
			return false;
		if (QFileInfo::exists(targetPath) && !QFile::remove(targetPath))
			return false;
		if (QFile::rename(sourcePath, targetPath))
			return true;
		if (!QFile::copy(sourcePath, targetPath))
			return false;
		return QFile::remove(sourcePath);
	}

	QStringList splitSerializedPathList(const QString &valueList)
	{
		if (valueList.trimmed().isEmpty())
			return {};
		QString normalized = valueList;
		normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
		normalized.replace(QLatin1Char('*'), QLatin1Char('\n'));

		QStringList items;
		for (QString entry : normalized.split(QLatin1Char('\n'), Qt::KeepEmptyParts))
		{
			entry = stripOptionalQuotes(entry.trimmed());
			if (!entry.isEmpty())
				items.push_back(entry);
		}
		return items;
	}

	QStringList splitSerializedWorldList(const QString &worldList)
	{
		return splitSerializedPathList(worldList);
	}

	QString keyLeafName(const QString &key)
	{
		const qsizetype slash = key.lastIndexOf(QLatin1Char('/'));
		if (slash < 0)
			return key;
		return key.mid(slash + 1);
	}

	bool isPathListKey(const QString &key)
	{
		const QString leaf = keyLeafName(key);
		return leaf.compare(QStringLiteral("WorldList"), Qt::CaseInsensitive) == 0 ||
		       leaf.compare(QStringLiteral("PluginList"), Qt::CaseInsensitive) == 0;
	}

	bool shouldNormalizePathKey(const QString &key)
	{
		const QString leaf      = keyLeafName(key);
		const QString lowerLeaf = leaf.toLower();
		return leaf.contains(QStringLiteral("Directory")) || leaf.contains(QStringLiteral("File")) ||
		       leaf.contains(QStringLiteral("Path")) || lowerLeaf.contains(QStringLiteral("directory")) ||
		       lowerLeaf.contains(QStringLiteral("path")) || lowerLeaf == QStringLiteral("file") ||
		       lowerLeaf.contains(QStringLiteral("_file")) || lowerLeaf.contains(QStringLiteral("file_")) ||
		       lowerLeaf.contains(QStringLiteral("filename"));
	}

	using PathTransformFn = QString (*)(const QString &);

	QString transformPathList(const QString &input, const PathTransformFn transform)
	{
		if (input.isEmpty())
			return input;
		const QStringList items = input.split(QLatin1Char('*'), Qt::KeepEmptyParts);
		QStringList       transformed;
		transformed.reserve(items.size());
		for (const QString &item : items)
			transformed.push_back(transform(item));
		return transformed.join(QLatin1Char('*'));
	}

	QString normalizePathList(const QString &input)
	{
		return transformPathList(input, normalizePathString);
	}

	QString quotedSqlIdentifier(const QString &identifier)
	{
		QString quoted = identifier;
		quoted.replace(QLatin1Char('"'), QStringLiteral("\"\""));
		return QLatin1Char('"') + quoted + QLatin1Char('"');
	}

	QString questionPlaceholders(const int count)
	{
		QStringList placeholders;
		placeholders.reserve(count);
		for (int i = 0; i < count; ++i)
			placeholders.push_back(QStringLiteral("?"));
		return placeholders.join(QLatin1Char(','));
	}

	QString findTopLevelFileWithCaseFallback(const QString &rootDir, const QString &fileName)
	{
		QString exactPath = QDir(rootDir).filePath(fileName);
		if (QFileInfo::exists(exactPath))
			return exactPath;

		const QFileInfoList entries =
		    QDir(rootDir).entryInfoList(QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
		for (const QFileInfo &entry : entries)
		{
			if (entry.fileName().compare(fileName, Qt::CaseInsensitive) == 0)
				return entry.absoluteFilePath();
		}
		return {};
	}

	QString decodeBytesWithEncoding(const QByteArray &input, const QStringConverter::Encoding encoding,
	                                bool &success)
	{
		QStringDecoder decoder(encoding);
		QString        decoded = decoder.decode(input);
		success                = !decoder.hasError();
		return decoded;
	}

	QString decodeLegacyIniText(const QByteArray &input)
	{
		if (const auto detectedEncoding = QStringConverter::encodingForData(input);
		    detectedEncoding.has_value())
		{
			bool          success = false;
			const QString decoded = decodeBytesWithEncoding(input, *detectedEncoding, success);
			if (success)
				return decoded;
		}

		bool    utf8Success = false;
		QString utf8        = decodeBytesWithEncoding(input, QStringConverter::Utf8, utf8Success);
		if (utf8Success)
			return utf8;

		QStringDecoder windowsDecoder(QStringLiteral("windows-1252"));
		if (windowsDecoder.isValid())
		{
			QString decoded = windowsDecoder.decode(input);
			if (!windowsDecoder.hasError())
				return decoded;
		}

		return QString::fromLatin1(input);
	}

	QHash<QString, QString> parseLegacyIniRawValues(const QString &iniPath)
	{
		QHash<QString, QString> values;
		QFile                   file(iniPath);
		if (!file.open(QIODevice::ReadOnly))
			return values;

		QString           currentSection;
		const QString     content = decodeLegacyIniText(file.readAll());
		const QStringList lines =
		    content.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::KeepEmptyParts);
		for (QString line : lines)
		{
			line = line.trimmed();
			if (line.isEmpty())
				continue;
			if (line.startsWith(QLatin1Char(';')) || line.startsWith(QLatin1Char('#')))
				continue;
			if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']')) && line.size() >= 2)
			{
				currentSection = line.mid(1, line.size() - 2).trimmed();
				continue;
			}

			const qsizetype equalsPos = line.indexOf(QLatin1Char('='));
			if (equalsPos < 0)
				continue;
			const QString key   = line.left(equalsPos).trimmed();
			const QString value = line.mid(equalsPos + 1);
			if (key.isEmpty())
				continue;
			const QString fullKey = currentSection.isEmpty() ? key : currentSection + QLatin1Char('/') + key;
			values.insert(fullKey, value);
		}
		return values;
	}

	QString migrateLegacyIniPathValueDetailed(const QString &sourceBaseDir, const QString &destinationBaseDir,
	                                          const QString &key, const QString &value,
	                                          bool archiveLegacySource, QStringList *warnings,
	                                          int *convertedWorlds);

	QVariant migrateLegacyIniValueWithRawSource(const QString &sourceBaseDir,
	                                            const QString &destinationBaseDir, const QString &key,
	                                            const QVariant                &value,
	                                            const QHash<QString, QString> &rawValues,
	                                            const bool archiveLegacySource, QStringList *warnings,
	                                            int *convertedWorlds)
	{
		if (const QString leaf = keyLeafName(key);
		    (isPathListKey(leaf) || shouldNormalizePathKey(leaf)) && rawValues.contains(key))
		{
			return migrateLegacyIniPathValueDetailed(sourceBaseDir, destinationBaseDir, key,
			                                         rawValues.value(key), archiveLegacySource, warnings,
			                                         convertedWorlds);
		}
		if (value.canConvert<QStringList>())
		{
			const QStringList list = value.toStringList();
			QStringList       migrated;
			migrated.reserve(list.size());
			for (const QString &item : list)
				migrated.push_back(migrateLegacyIniPathValueDetailed(sourceBaseDir, destinationBaseDir, key,
				                                                     item, archiveLegacySource, warnings,
				                                                     convertedWorlds));
			return {migrated};
		}
		if (value.canConvert<QString>())
		{
			return migrateLegacyIniPathValueDetailed(sourceBaseDir, destinationBaseDir, key, value.toString(),
			                                         archiveLegacySource, warnings, convertedWorlds);
		}
		return value;
	}

	void pruneLegacyCtrlBarsGroups(QSettings &modern)
	{
		for (const QStringList keys = modern.allKeys(); const QString &key : keys)
		{
			if (key.startsWith(QStringLiteral("CtrlBars-Bar"), Qt::CaseInsensitive) ||
			    key.startsWith(QStringLiteral("CtrlBars-Summary"), Qt::CaseInsensitive))
			{
				modern.remove(key);
			}
		}
	}

	void recordImportError(QMudMushclientImportUtils::ImportStats &stats, const QString &message)
	{
		++stats.errors;
		if (stats.errorDetails.size() < 12)
			stats.errorDetails.push_back(message);
		qWarning() << "MUSHclient import:" << message;
	}

	void appendMigrationWarning(QStringList *warnings, const QString &message)
	{
		if (message.trimmed().isEmpty())
			return;
		if (warnings)
			warnings->push_back(message);
		qWarning() << "MUSHclient migration:" << message;
	}

	void recordImportWarning(QMudMushclientImportUtils::ImportStats &stats, const QString &message)
	{
		++stats.warnings;
		if (stats.warningDetails.size() < 12)
			stats.warningDetails.push_back(message);
		qWarning() << "MUSHclient import:" << message;
	}

	QString importDestinationError(const QString &qmudHome, const QString &destinationPath)
	{
		QString resolvedPath;
		QString error;
		if (QMudPluginPathUtils::resolveInsideQmudHome(qmudHome, destinationPath, &resolvedPath, &error))
			return {};
		return QStringLiteral("Destination is outside QMud home or unsafe: %1 (%2)")
		    .arg(destinationPath, error);
	}

	bool validateImportDestination(const QString &qmudHome, const QString &destinationPath,
	                               QMudMushclientImportUtils::ImportStats &stats)
	{
		const QString error = importDestinationError(qmudHome, destinationPath);
		if (error.isEmpty())
			return true;
		recordImportError(stats, error);
		return false;
	}

	bool copyFileNoOverwrite(const QString &qmudHome, const QString &sourcePath,
	                         const QString &destinationPath, QMudMushclientImportUtils::ImportStats &stats)
	{
		if (!validateImportDestination(qmudHome, destinationPath, stats))
			return false;

		if (QFileInfo::exists(destinationPath))
		{
			++stats.filesSkippedExisting;
			return true;
		}

		const QString destinationDir = QFileInfo(destinationPath).absolutePath();
		if (!QDir().mkpath(destinationDir))
		{
			recordImportError(stats, QStringLiteral("Unable to create directory: %1").arg(destinationDir));
			return false;
		}

		if (!QFile::copy(sourcePath, destinationPath))
		{
			QString error = QStringLiteral("Unable to copy %1 to %2").arg(sourcePath, destinationPath);
			if (QFileInfo::exists(destinationPath) && !QFile::remove(destinationPath))
				error += QStringLiteral("; unable to remove incomplete destination");
			recordImportError(stats, error);
			return false;
		}
		++stats.filesCopied;
		return true;
	}

	QString relativePathUnderBase(const QString &baseDir, const QString &absolutePath)
	{
		const QString relative =
		    QDir(QDir::cleanPath(baseDir)).relativeFilePath(QDir::cleanPath(absolutePath));
		if (relative == QStringLiteral("..") || relative.startsWith(QStringLiteral("../")) ||
		    QDir::isAbsolutePath(relative))
			return {};
		return normalizePathString(relative);
	}

	bool isUnderRelativeDirectory(const QString &relativePath, const QString &directoryName)
	{
		const QString normalized = normalizePathString(relativePath).trimmed();
		return normalized.compare(directoryName, Qt::CaseInsensitive) == 0 ||
		       normalized.startsWith(directoryName + QLatin1Char('/'), Qt::CaseInsensitive);
	}

	bool isLegacyNonLuaScriptSuffix(const QString &suffix)
	{
		static const QStringList legacySuffixes = {
		    QStringLiteral("vbs"), QStringLiteral("vbe"), QStringLiteral("js"), QStringLiteral("jse"),
		    QStringLiteral("pl"),  QStringLiteral("py"),  QStringLiteral("pys")};
		return legacySuffixes.contains(suffix.toLower());
	}

	bool isImportableLuaPluginXml(const QString &path)
	{
		WorldDocument document;
		if (!document.loadFromPluginFile(path))
			return false;
		if (document.plugins().isEmpty())
			return false;

		const WorldDocument::Plugin &plugin = document.plugins().front();
		if (plugin.attributes.value(QStringLiteral("language"))
		        .trimmed()
		        .compare(QStringLiteral("lua"), Qt::CaseInsensitive) != 0)
			return false;

		const QString pluginId = plugin.attributes.value(QStringLiteral("id")).trimmed().toLower();
		return !QMudNativePluginRegistry::isBlacklistedId(pluginId);
	}

	bool shouldImportWorldsFile(const QString &sourcePath, const QString &relativePath,
	                            QMudMushclientImportUtils::ImportStats &stats)
	{
		if (!isUnderRelativeDirectory(relativePath, QStringLiteral("plugins")))
			return true;

		const QString suffix = QFileInfo(relativePath).suffix().toLower();
		if (isLegacyNonLuaScriptSuffix(suffix))
		{
			++stats.filesSkippedFiltered;
			return false;
		}
		if (suffix != QStringLiteral("xml"))
			return true;
		if (isImportableLuaPluginXml(sourcePath))
			return true;

		++stats.filesSkippedFiltered;
		return false;
	}

	QString mappedSourceTreeStoragePath(const QString &sourceBaseDir, const QString &destinationBaseDir,
	                                    const QString &relativeBaseDir, const QString &path)
	{
		QString normalized = stripOptionalQuotes(normalizePathString(path).trimmed());
		if (normalized.isEmpty())
			return normalized;

		QStringList bases;
		if (!relativeBaseDir.trimmed().isEmpty())
			bases.push_back(relativeBaseDir);
		bases.push_back(sourceBaseDir);

		for (const QString &base : bases)
		{
			const QString absolute = absolutePathFromBase(base, normalized);
			const QString resolved = QFileInfo::exists(absolute)
			                             ? QDir::cleanPath(absolute)
			                             : resolveExistingPathCaseInsensitive(absolute);
			if (resolved.isEmpty())
				continue;

			const QString relative = relativePathUnderBase(sourceBaseDir, resolved);
			if (!relative.isEmpty())
			{
				return dotRelativeStoragePath(destinationBaseDir,
				                              QDir(destinationBaseDir).filePath(relative));
			}
			if (!relativeBaseDir.trimmed().isEmpty())
			{
				const QString localRelative = relativePathUnderBase(relativeBaseDir, resolved);
				if (!localRelative.isEmpty())
				{
					return dotRelativeStoragePath(destinationBaseDir,
					                              QDir(destinationBaseDir).filePath(localRelative));
				}
			}
		}

		return {};
	}

	QString mappedSourcePluginStoragePath(const QString &sourceBaseDir, const QString &destinationBaseDir,
	                                      const QString &path)
	{
		QString normalized = stripOptionalQuotes(normalizePathString(path).trimmed());
		if (normalized.isEmpty())
			return normalized;

		QStringList   bases;
		const QString sourcePluginsDir = QDir(sourceBaseDir).filePath(QStringLiteral("worlds/plugins"));
		if (!sourcePluginsDir.trimmed().isEmpty())
			bases.push_back(sourcePluginsDir);

		for (const QString &base : bases)
		{
			const QString absolute = absolutePathFromBase(base, normalized);
			const QString resolved = QFileInfo::exists(absolute)
			                             ? QDir::cleanPath(absolute)
			                             : resolveExistingPathCaseInsensitive(absolute);
			if (resolved.isEmpty())
				continue;

			const QString relativePlugin = relativePathUnderBase(
			    QDir(sourceBaseDir).filePath(QStringLiteral("worlds/plugins")), resolved);
			if (!relativePlugin.isEmpty())
			{
				return dotRelativeStoragePath(
				    destinationBaseDir,
				    QDir(destinationBaseDir).filePath(QStringLiteral("worlds/plugins/") + relativePlugin));
			}

			return containedPluginStoragePath(destinationBaseDir, resolved);
		}

		return containedPluginStoragePath(destinationBaseDir, normalized);
	}

	QString migratePluginListPathsDetailed(const QString &sourceBaseDir, const QString &destinationBaseDir,
	                                       const QString &pluginList, bool *changed = nullptr)
	{
		const QStringList items = splitSerializedPathList(pluginList);
		QStringList       migrated;
		migrated.reserve(items.size());
		bool anyChanged = false;
		for (const QString &item : items)
		{
			const QString value = mappedSourcePluginStoragePath(sourceBaseDir, destinationBaseDir, item);
			if (value != item)
				anyChanged = true;
			migrated.push_back(value);
		}
		const QString joined = migrated.join(QLatin1Char('*'));
		if (changed)
			*changed = anyChanged || (joined != pluginList);
		return joined;
	}

	QString legacyWorldPathInputForXmlValue(const QString &sourceBaseDir, const QString &relativeBaseDir,
	                                        const QString &path)
	{
		QString normalized = stripOptionalQuotes(normalizePathString(path).trimmed());
		if (normalized.isEmpty())
			return normalized;
		if (relativeBaseDir.trimmed().isEmpty())
			return normalized;

		QString normalizedRelative = normalized;
		while (normalizedRelative.startsWith(QStringLiteral("./")))
			normalizedRelative = normalizedRelative.mid(2);
		const bool hasLeadingSlashWindowsDrive =
		    normalizedRelative.size() > 2 && normalizedRelative.at(0) == QLatin1Char('/') &&
		    normalizedRelative.at(1).isLetter() && normalizedRelative.at(2) == QLatin1Char(':');
		const bool isWindowsAbsolute = hasWindowsDrivePath(normalizedRelative) || hasLeadingSlashWindowsDrive;
		const bool isNativeAbsolute  = QFileInfo(normalizedRelative).isAbsolute() && !isWindowsAbsolute;
		const bool isAbsolute        = isNativeAbsolute || isWindowsAbsolute;
		const QString sourceWorlds   = QDir(sourceBaseDir).filePath(QStringLiteral("worlds"));
		if (isNativeAbsolute)
		{
			const QString absolutePath = absolutePathFromBase(relativeBaseDir, normalized);
			if (const QString relativeWorld = relativePathUnderBase(sourceWorlds, absolutePath);
			    !relativeWorld.isEmpty())
			{
				return QStringLiteral("worlds/") + relativeWorld;
			}
		}

		QString portableRelative = QMudPluginPathUtils::legacyPathRelativeToQmudHome(normalizedRelative);
		if (portableRelative.startsWith(QStringLiteral("worlds/"), Qt::CaseInsensitive))
			return portableRelative;
		if (!isAbsolute && !portableRelative.isEmpty() &&
		    !portableRelative.startsWith(QStringLiteral("../")) && !QDir::isAbsolutePath(portableRelative))
		{
			const QString baseRelative = relativePathUnderBase(sourceWorlds, relativeBaseDir);
			if (!baseRelative.isEmpty() || QDir::cleanPath(relativeBaseDir) == QDir::cleanPath(sourceWorlds))
			{
				const QString combined =
				    QDir::cleanPath(QDir(baseRelative.isEmpty() ? QStringLiteral(".") : baseRelative)
				                        .filePath(portableRelative));
				if (combined != QStringLiteral("..") && !combined.startsWith(QStringLiteral("../")) &&
				    !QDir::isAbsolutePath(combined))
				{
					return QStringLiteral("worlds/") + combined;
				}
			}
		}

		if (!isAbsolute)
		{
			const QString relativeBaseAbsolute = absolutePathFromBase(relativeBaseDir, normalized);
			if (const QString relativeWorld = relativePathUnderBase(sourceWorlds, relativeBaseAbsolute);
			    !relativeWorld.isEmpty())
			{
				return QStringLiteral("worlds/") + relativeWorld;
			}
		}

		return normalized;
	}

	QString migrateLegacyXmlPluginPathValue(const QString &sourceBaseDir, const QString &destinationBaseDir,
	                                        const QString &value)
	{
		return mappedSourcePluginStoragePath(sourceBaseDir, destinationBaseDir, value);
	}

	struct LegacyWorldMigrationResult
	{
			QString path;
			bool    converted{false};
			bool    skippedExisting{false};
			QString warning;
			QString error;
	};

	LegacyWorldMigrationResult
	migrateLegacyWorldFilePathDetailed(const QString &sourceBaseDir, const QString &destinationBaseDir,
	                                   const QString &path, bool archiveLegacySource,
	                                   QSet<QString> *activeConversions, QStringList *warnings,
	                                   int *convertedWorlds);

	QString migrateLegacyXmlPathValue(const QString &sourceBaseDir, const QString &destinationBaseDir,
	                                  const QString &relativeBaseDir, const QString &key,
	                                  const QString &value, const bool archiveLegacySource,
	                                  QSet<QString> *activeConversions, QStringList *warnings,
	                                  int *convertedWorlds)
	{
		const QString leaf = keyLeafName(key);
		if (leaf.compare(QStringLiteral("WorldList"), Qt::CaseInsensitive) == 0)
		{
			const QStringList items = splitSerializedWorldList(value);
			QStringList       migrated;
			migrated.reserve(items.size());
			for (const QString &item : items)
			{
				const LegacyWorldMigrationResult result = migrateLegacyWorldFilePathDetailed(
				    sourceBaseDir, destinationBaseDir,
				    legacyWorldPathInputForXmlValue(sourceBaseDir, relativeBaseDir, item),
				    archiveLegacySource, activeConversions, warnings, convertedWorlds);
				appendMigrationWarning(warnings, result.warning);
				if (!result.error.isEmpty())
					appendMigrationWarning(warnings, result.error);
				migrated.push_back(result.path);
			}
			return migrated.join(QLatin1Char('*'));
		}
		if (leaf.compare(QStringLiteral("PluginList"), Qt::CaseInsensitive) == 0)
		{
			const QStringList items = splitSerializedPathList(value);
			QStringList       migrated;
			migrated.reserve(items.size());
			for (const QString &item : items)
				migrated.push_back(migrateLegacyXmlPluginPathValue(sourceBaseDir, destinationBaseDir, item));
			return migrated.join(QLatin1Char('*'));
		}

		if (const QString suffix = QFileInfo(normalizePathString(value)).suffix().toLower();
		    QMudFileExtensions::isWorldSuffix(suffix))
		{
			const LegacyWorldMigrationResult result = migrateLegacyWorldFilePathDetailed(
			    sourceBaseDir, destinationBaseDir,
			    legacyWorldPathInputForXmlValue(sourceBaseDir, relativeBaseDir, value), archiveLegacySource,
			    activeConversions, warnings, convertedWorlds);
			appendMigrationWarning(warnings, result.warning);
			if (!result.error.isEmpty())
				appendMigrationWarning(warnings, result.error);
			return result.path;
		}

		if (const QString mapped =
		        mappedSourceTreeStoragePath(sourceBaseDir, destinationBaseDir, relativeBaseDir, value);
		    !mapped.isEmpty())
		{
			return mapped;
		}

		if (hasPathSyntax(value))
			return containedLegacyStoragePath(destinationBaseDir, value);

		return QMudMushclientImportUtils::migrateLegacyIniPathValue(sourceBaseDir, destinationBaseDir, key,
		                                                            value, archiveLegacySource);
	}

	bool xmlBooleanAttributeIsTrue(const QString &value)
	{
		const QString normalized = value.trimmed();
		return normalized.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
		       normalized.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0 ||
		       normalized.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0 ||
		       normalized == QStringLiteral("1");
	}

	bool shouldMigrateXmlText(const QString &elementName, const QXmlStreamAttributes &attributes,
	                          QString &migrationKey)
	{
		if (isPathListKey(elementName) || shouldNormalizePathKey(elementName))
		{
			migrationKey = elementName;
			return true;
		}

		if (elementName.compare(QStringLiteral("variable"), Qt::CaseInsensitive) == 0)
		{
			const QString variableName = attributes.value(QLatin1String("name")).toString();
			if (shouldNormalizePathKey(variableName) || isPathListKey(variableName))
			{
				migrationKey = variableName;
				return true;
			}
		}

		return false;
	}

	QString detectXmlEncodingName(const QByteArray &input)
	{
		if (input.startsWith("\xEF\xBB\xBF"))
			return QStringLiteral("UTF-8");
		if (input.startsWith("\xFF\xFE"))
			return QStringLiteral("UTF-16LE");
		if (input.startsWith("\xFE\xFF"))
			return QStringLiteral("UTF-16BE");

		const QByteArray              prefix      = input.left(512);
		const QString                 declaration = QString::fromLatin1(prefix);
		const QRegularExpression      encodingPattern(QStringLiteral(R"(encoding\s*=\s*(['"])([^'"]+)\1)"),
		                                              QRegularExpression::CaseInsensitiveOption);
		const QRegularExpressionMatch match = encodingPattern.match(declaration);
		if (!match.hasMatch())
			return QStringLiteral("UTF-8");
		return match.captured(2).trimmed();
	}

	QString decodeLegacyWorldXml(const QByteArray &input)
	{
		const QByteArray encodingName = detectXmlEncodingName(input).toLatin1();
		if (const auto encoding = QStringConverter::encodingForName(encodingName); encoding.has_value())
		{
			QStringDecoder decoder(*encoding);
			const QString  decoded = decoder.decode(input);
			if (!decoder.hasError())
				return decoded;
		}
		return QString::fromUtf8(input);
	}

	bool migrateLegacyWorldXmlData(const QString &sourceBaseDir, const QString &destinationBaseDir,
	                               const QString &relativeBaseDir, const QByteArray &input,
	                               QByteArray &output, QString &error, const bool archiveLegacySource,
	                               QSet<QString> *activeConversions, QStringList *warnings,
	                               int *convertedWorlds)
	{
		QXmlStreamReader reader(decodeLegacyWorldXml(input));
		QByteArray       migrated;
		QXmlStreamWriter writer(&migrated);
		writer.setAutoFormatting(true);

		QVector<QString> textMigrationKeys;
		while (!reader.atEnd())
		{
			switch (reader.readNext())
			{
			case QXmlStreamReader::StartDocument:
				writer.writeStartDocument(QStringLiteral("1.0"), reader.isStandaloneDocument());
				break;
			case QXmlStreamReader::EndDocument:
				writer.writeEndDocument();
				break;
			case QXmlStreamReader::StartElement:
			{
				const QString              elementName = reader.name().toString();
				const QXmlStreamAttributes attributes  = reader.attributes();
				const bool                 isPluginInclude =
				    elementName.compare(QStringLiteral("include"), Qt::CaseInsensitive) == 0 &&
				    xmlBooleanAttributeIsTrue(attributes.value(QLatin1String("plugin")).toString());

				writer.writeStartElement(reader.qualifiedName().toString());
				for (const QXmlStreamNamespaceDeclaration &declaration : reader.namespaceDeclarations())
				{
					writer.writeNamespace(declaration.namespaceUri().toString(),
					                      declaration.prefix().toString());
				}

				for (const QXmlStreamAttribute &attribute : attributes)
				{
					const QString attributeName = attribute.name().toString();
					QString       value         = attribute.value().toString();
					if (elementName.compare(QStringLiteral("include"), Qt::CaseInsensitive) == 0 &&
					    attributeName.compare(QStringLiteral("name"), Qt::CaseInsensitive) == 0)
					{
						value =
						    isPluginInclude
						        ? migrateLegacyXmlPluginPathValue(sourceBaseDir, destinationBaseDir, value)
						        : migrateLegacyXmlPathValue(sourceBaseDir, destinationBaseDir,
						                                    relativeBaseDir, QStringLiteral("File"), value,
						                                    archiveLegacySource, activeConversions, warnings,
						                                    convertedWorlds);
					}
					else if (elementName.compare(QStringLiteral("plugin"), Qt::CaseInsensitive) == 0 &&
					         attributeName.compare(QStringLiteral("source"), Qt::CaseInsensitive) == 0)
					{
						value = migrateLegacyXmlPluginPathValue(sourceBaseDir, destinationBaseDir, value);
					}
					else if (isPathListKey(attributeName) || shouldNormalizePathKey(attributeName))
					{
						value = migrateLegacyXmlPathValue(sourceBaseDir, destinationBaseDir, relativeBaseDir,
						                                  attributeName, value, archiveLegacySource,
						                                  activeConversions, warnings, convertedWorlds);
					}

					if (attribute.namespaceUri().isEmpty())
						writer.writeAttribute(attribute.qualifiedName().toString(), value);
					else
						writer.writeAttribute(attribute.namespaceUri().toString(),
						                      attribute.name().toString(), value);
				}

				QString textMigrationKey;
				(void)shouldMigrateXmlText(elementName, attributes, textMigrationKey);
				textMigrationKeys.push_back(textMigrationKey);
				break;
			}
			case QXmlStreamReader::EndElement:
				writer.writeEndElement();
				if (!textMigrationKeys.isEmpty())
					textMigrationKeys.pop_back();
				break;
			case QXmlStreamReader::Characters:
			{
				QString text = reader.text().toString();
				if (!textMigrationKeys.isEmpty() && !textMigrationKeys.back().isEmpty() &&
				    !text.trimmed().isEmpty())
				{
					text = migrateLegacyXmlPathValue(sourceBaseDir, destinationBaseDir, relativeBaseDir,
					                                 textMigrationKeys.back(), text, archiveLegacySource,
					                                 activeConversions, warnings, convertedWorlds);
				}
				if (reader.isCDATA())
					writer.writeCDATA(text);
				else
					writer.writeCharacters(text);
				break;
			}
			case QXmlStreamReader::Comment:
				writer.writeComment(reader.text().toString());
				break;
			case QXmlStreamReader::DTD:
				writer.writeDTD(reader.text().toString());
				break;
			case QXmlStreamReader::EntityReference:
				writer.writeEntityReference(reader.name().toString());
				break;
			case QXmlStreamReader::ProcessingInstruction:
				writer.writeProcessingInstruction(reader.processingInstructionTarget().toString(),
				                                  reader.processingInstructionData().toString());
				break;
			case QXmlStreamReader::NoToken:
			case QXmlStreamReader::Invalid:
				break;
			}
		}

		if (reader.hasError())
		{
			error = QStringLiteral("Failed to parse legacy world XML: %1").arg(reader.errorString());
			return false;
		}

		output = migrated;
		return true;
	}

	LegacyWorldMigrationResult legacyWorldMigrationPath(const QString &path)
	{
		LegacyWorldMigrationResult result;
		result.path = path;
		return result;
	}

	LegacyWorldMigrationResult legacyWorldMigrationExisting(const QString &path)
	{
		LegacyWorldMigrationResult result = legacyWorldMigrationPath(path);
		result.skippedExisting            = true;
		return result;
	}

	LegacyWorldMigrationResult legacyWorldMigrationError(const QString &path, const QString &error)
	{
		LegacyWorldMigrationResult result = legacyWorldMigrationPath(path);
		result.error                      = error;
		return result;
	}

	LegacyWorldMigrationResult migrateLegacyWorldFilePathDetailed(const QString &sourceBaseDir,
	                                                              const QString &destinationBaseDir,
	                                                              const QString &path,
	                                                              const bool     archiveLegacySource,
	                                                              QSet<QString> *activeConversions,
	                                                              QStringList *warnings, int *convertedWorlds)
	{
		QString normalizedPath = stripOptionalQuotes(normalizePathString(path).trimmed());
		if (normalizedPath.startsWith(QStringLiteral("./")) && normalizedPath.size() > 3 &&
		    normalizedPath.at(2).isLetter() && normalizedPath.at(3) == QLatin1Char(':'))
		{
			normalizedPath = normalizedPath.mid(2);
		}
		if (normalizedPath.startsWith(QLatin1Char('/')) && normalizedPath.size() > 3 &&
		    normalizedPath.at(1).isLetter() && normalizedPath.at(2) == QLatin1Char(':'))
		{
			normalizedPath = normalizedPath.mid(1);
		}
		if (normalizedPath.trimmed().isEmpty())
			return legacyWorldMigrationPath(normalizedPath);

		const QString suffix = QFileInfo(normalizedPath).suffix().toLower();
		if (!QMudFileExtensions::isWorldSuffix(suffix))
			return legacyWorldMigrationPath(QMudFileExtensions::canonicalizePathExtension(normalizedPath));

		QString canonicalPath = QMudFileExtensions::canonicalizePathExtension(normalizedPath);
		if (!QMudFileExtensions::isLegacyWorldSuffix(suffix))
		{
			const QString modernAbsolutePath =
			    containedWorldAbsolutePath(sourceBaseDir, destinationBaseDir, canonicalPath, {});
			if (QFileInfo::exists(modernAbsolutePath))
			{
				return legacyWorldMigrationExisting(
				    storagePathFromAbsolute(destinationBaseDir, modernAbsolutePath));
			}
			if (const QString resolvedModernPath = resolveExistingPathCaseInsensitive(modernAbsolutePath);
			    !resolvedModernPath.isEmpty())
			{
				return legacyWorldMigrationExisting(
				    storagePathFromAbsolute(destinationBaseDir, resolvedModernPath));
			}
			if (const QString legacyFallbackPath =
			        QMudFileExtensions::replaceOrAppendExtension(normalizedPath, QStringLiteral("mcl"));
			    QFileInfo(absolutePathFromBase(sourceBaseDir, legacyFallbackPath)).exists() ||
			    !resolveExistingPathCaseInsensitive(absolutePathFromBase(sourceBaseDir, legacyFallbackPath))
			         .isEmpty())
			{
				return migrateLegacyWorldFilePathDetailed(sourceBaseDir, destinationBaseDir,
				                                          legacyFallbackPath, archiveLegacySource,
				                                          activeConversions, warnings, convertedWorlds);
			}
			if (const QString destinationRelative =
			        containedWorldStoragePath(destinationBaseDir, canonicalPath);
			    !destinationRelative.isEmpty())
			{
				return legacyWorldMigrationPath(destinationRelative);
			}
			return legacyWorldMigrationPath(containedWorldStoragePath(destinationBaseDir, canonicalPath));
		}

		const QString legacyAbsolutePath = absolutePathFromBase(sourceBaseDir, normalizedPath);
		QString       resolvedLegacyPath = QFileInfo::exists(legacyAbsolutePath)
		                                       ? QDir::cleanPath(legacyAbsolutePath)
		                                       : resolveExistingPathCaseInsensitive(legacyAbsolutePath);
		if (resolvedLegacyPath.isEmpty())
		{
			if (const QString remappedLegacyPath = resolveExistingPathCaseInsensitive(
			        remapLegacyWindowsWorldPathToBase(sourceBaseDir, normalizedPath));
			    !remappedLegacyPath.isEmpty())
			{
				resolvedLegacyPath = remappedLegacyPath;
			}
		}

		const QString effectiveModernAbsolutePath =
		    containedWorldAbsolutePath(sourceBaseDir, destinationBaseDir, canonicalPath, resolvedLegacyPath);
		if (const QString error = importDestinationError(destinationBaseDir, effectiveModernAbsolutePath);
		    !error.isEmpty())
		{
			return legacyWorldMigrationError(containedWorldStoragePath(destinationBaseDir, canonicalPath),
			                                 error);
		}

		QString resolvedModernPath = QFileInfo::exists(effectiveModernAbsolutePath)
		                                 ? QDir::cleanPath(effectiveModernAbsolutePath)
		                                 : resolveExistingPathCaseInsensitive(effectiveModernAbsolutePath);
		if (!resolvedModernPath.isEmpty())
			return legacyWorldMigrationExisting(
			    storagePathFromAbsolute(destinationBaseDir, resolvedModernPath));

		LegacyWorldMigrationResult result = legacyWorldMigrationPath(
		    storagePathFromAbsolute(destinationBaseDir, effectiveModernAbsolutePath));
		if (resolvedLegacyPath.isEmpty())
		{
			result.warning =
			    QStringLiteral("Referenced legacy world file was not found: %1; reference migrated to %2")
			        .arg(normalizedPath, result.path);
			return result;
		}

		if (QFileInfo(resolvedLegacyPath).isSymLink())
		{
			result.warning =
			    QStringLiteral("Referenced legacy world file is a symlink and was not imported: %1; "
			                   "reference migrated to %2")
			        .arg(normalizedPath, result.path);
			return result;
		}

		if (activeConversions && activeConversions->contains(resolvedLegacyPath))
			return result;

		if (const QString modernDir = QFileInfo(effectiveModernAbsolutePath).absolutePath();
		    !QDir().mkpath(modernDir))
		{
			result.error = QStringLiteral("Failed to create migrated world directory: %1").arg(modernDir);
			return result;
		}

		QFile legacyFile(resolvedLegacyPath);
		if (!legacyFile.open(QIODevice::ReadOnly))
		{
			result.error = QStringLiteral("Failed to read legacy world file: %1").arg(resolvedLegacyPath);
			return result;
		}
		const QByteArray data = legacyFile.readAll();
		legacyFile.close();

		if (activeConversions)
			activeConversions->insert(resolvedLegacyPath);

		QByteArray migratedData;
		QString    xmlError;
		if (!migrateLegacyWorldXmlData(
		        sourceBaseDir, destinationBaseDir, QFileInfo(resolvedLegacyPath).absolutePath(), data,
		        migratedData, xmlError, archiveLegacySource, activeConversions, warnings, convertedWorlds))
		{
			if (activeConversions)
				activeConversions->remove(resolvedLegacyPath);
			result.error = QStringLiteral("%1: %2").arg(xmlError, resolvedLegacyPath);
			return result;
		}

		if (activeConversions)
			activeConversions->remove(resolvedLegacyPath);

		QSaveFile modernFile(effectiveModernAbsolutePath);
		if (!modernFile.open(QIODevice::WriteOnly))
		{
			result.error =
			    QStringLiteral("Failed to create migrated world file: %1").arg(effectiveModernAbsolutePath);
			return result;
		}
		if (modernFile.write(migratedData) != migratedData.size() || !modernFile.commit())
		{
			result.error =
			    QStringLiteral("Failed to write migrated world file: %1").arg(effectiveModernAbsolutePath);
			return result;
		}
		result.converted = true;
		if (convertedWorlds)
			++*convertedWorlds;

		if (archiveLegacySource)
		{
			const QString relativeLegacy = archiveRelativePathFor(destinationBaseDir, resolvedLegacyPath);
			const QString archivePath =
			    QDir(destinationBaseDir).filePath(QStringLiteral("migrated/") + relativeLegacy);
			if (!moveFileToArchive(resolvedLegacyPath, archivePath))
				qWarning() << "Failed to archive legacy world file:" << resolvedLegacyPath << "->"
				           << archivePath;
		}

		return result;
	}

	QString migrateLegacyIniPathValueDetailed(const QString &sourceBaseDir, const QString &destinationBaseDir,
	                                          const QString &key, const QString &value,
	                                          const bool archiveLegacySource, QStringList *warnings,
	                                          int *convertedWorlds)
	{
		const QString leaf = keyLeafName(key);
		if (isPathListKey(leaf))
		{
			if (leaf.compare(QStringLiteral("WorldList"), Qt::CaseInsensitive) == 0)
			{
				const QStringList items = splitSerializedWorldList(value);
				QStringList       migrated;
				migrated.reserve(items.size());
				for (const QString &item : items)
				{
					QSet<QString>                    activeConversions;
					const LegacyWorldMigrationResult result = migrateLegacyWorldFilePathDetailed(
					    sourceBaseDir, destinationBaseDir, item, archiveLegacySource, &activeConversions,
					    warnings, convertedWorlds);
					appendMigrationWarning(warnings, result.warning);
					if (!result.error.isEmpty())
						appendMigrationWarning(warnings, result.error);
					migrated.push_back(result.path);
				}
				return migrated.join(QLatin1Char('*'));
			}
			if (leaf.compare(QStringLiteral("PluginList"), Qt::CaseInsensitive) == 0)
				return migratePluginListPathsDetailed(sourceBaseDir, destinationBaseDir, value);
			return normalizePathList(value);
		}

		if (!shouldNormalizePathKey(leaf))
			return value;

		const QString normalized = normalizePathString(value);
		if (const QString suffix = QFileInfo(normalized).suffix().toLower();
		    QMudFileExtensions::isWorldSuffix(suffix))
		{
			QSet<QString>                    activeConversions;
			const LegacyWorldMigrationResult result = migrateLegacyWorldFilePathDetailed(
			    sourceBaseDir, destinationBaseDir, normalized, archiveLegacySource, &activeConversions,
			    warnings, convertedWorlds);
			appendMigrationWarning(warnings, result.warning);
			if (!result.error.isEmpty())
				appendMigrationWarning(warnings, result.error);
			return result.path;
		}
		if (const QString mapped =
		        mappedSourceTreeStoragePath(sourceBaseDir, destinationBaseDir, {}, normalized);
		    !mapped.isEmpty())
		{
			return mapped;
		}
		if (hasPathSyntax(normalized))
			return containedLegacyStoragePath(destinationBaseDir, normalized);
		return QMudFileExtensions::canonicalizePathExtension(normalized);
	}

	void importWorldsTree(const QString &mushclientRoot, const QString &qmudHome,
	                      QMudMushclientImportUtils::ImportStats &stats)
	{
		const QDir    sourceBase(mushclientRoot);
		const QDir    destinationBase(qmudHome);
		const QString sourceWorlds = sourceBase.filePath(QStringLiteral("worlds"));
		if (!QFileInfo(sourceWorlds).isDir())
			return;

		QDirIterator iterator(sourceWorlds,
		                      QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
		                      QDirIterator::Subdirectories);
		while (iterator.hasNext())
		{
			const QString   path = iterator.next();
			const QFileInfo info = iterator.fileInfo();
			if (info.isSymLink())
			{
				++stats.filesSkippedFiltered;
				continue;
			}

			const QString relativePath = normalizePathString(QDir(sourceWorlds).relativeFilePath(path));
			if (relativePath.isEmpty() || relativePath == QStringLiteral(".") ||
			    relativePath.startsWith(QStringLiteral("../")))
			{
				++stats.filesSkippedFiltered;
				continue;
			}

			const QString destinationPath =
			    QDir(destinationBase.filePath(QStringLiteral("worlds"))).filePath(relativePath);
			if (info.isDir())
			{
				if (!validateImportDestination(qmudHome, destinationPath, stats))
					continue;
				if (!QDir().mkpath(destinationPath))
					recordImportError(stats,
					                  QStringLiteral("Unable to create directory: %1").arg(destinationPath));
				continue;
			}
			if (!info.isFile())
			{
				++stats.filesSkippedFiltered;
				continue;
			}
			if (!shouldImportWorldsFile(path, relativePath, stats))
				continue;

			if (QMudFileExtensions::isLegacyWorldSuffix(info.suffix().toLower()))
			{
				QSet<QString>                    activeConversions;
				QStringList                      warnings;
				const LegacyWorldMigrationResult result = migrateLegacyWorldFilePathDetailed(
				    mushclientRoot, qmudHome, QStringLiteral("worlds/") + relativePath, false,
				    &activeConversions, &warnings, &stats.worldsConverted);
				for (const QString &warning : warnings)
					recordImportWarning(stats, warning);
				if (!result.warning.isEmpty())
					recordImportWarning(stats, result.warning);
				if (!result.error.isEmpty())
				{
					recordImportError(stats, result.error);
				}
				else if (result.skippedExisting)
				{
					++stats.filesSkippedExisting;
				}
				continue;
			}

			(void)copyFileNoOverwrite(qmudHome, path, destinationPath, stats);
		}
	}

	using ImportFileFilter = bool (*)(const QString &sourcePath, const QString &relativePath,
	                                  QMudMushclientImportUtils::ImportStats &stats);

	bool copyAllImportFileFilter(const QString &, const QString &, QMudMushclientImportUtils::ImportStats &)
	{
		return true;
	}

	bool copyLuaImportFileFilter(const QString &, const QString &relativePath,
	                             QMudMushclientImportUtils::ImportStats &stats)
	{
		if (QFileInfo(relativePath).suffix().compare(QStringLiteral("dll"), Qt::CaseInsensitive) != 0)
			return true;
		++stats.filesSkippedFiltered;
		return false;
	}

	void copyImportDirectoryTree(const QString &qmudHome, const QString &sourceRoot,
	                             const QString                          &destinationRoot,
	                             QMudMushclientImportUtils::ImportStats &stats, const ImportFileFilter filter)
	{
		if (!QFileInfo(sourceRoot).isDir())
			return;

		QDirIterator iterator(sourceRoot,
		                      QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
		                      QDirIterator::Subdirectories);
		while (iterator.hasNext())
		{
			const QString   path = iterator.next();
			const QFileInfo info = iterator.fileInfo();
			if (info.isSymLink())
			{
				++stats.filesSkippedFiltered;
				continue;
			}

			const QString relativePath = normalizePathString(QDir(sourceRoot).relativeFilePath(path));
			if (relativePath.isEmpty() || relativePath == QStringLiteral(".") ||
			    relativePath.startsWith(QStringLiteral("../")))
			{
				++stats.filesSkippedFiltered;
				continue;
			}

			const QString destinationPath = QDir(destinationRoot).filePath(relativePath);
			if (info.isDir())
			{
				if (!validateImportDestination(qmudHome, destinationPath, stats))
					continue;
				if (!QDir().mkpath(destinationPath))
					recordImportError(stats,
					                  QStringLiteral("Unable to create directory: %1").arg(destinationPath));
				continue;
			}
			if (!info.isFile())
			{
				++stats.filesSkippedFiltered;
				continue;
			}
			if (filter && !filter(path, relativePath, stats))
				continue;
			(void)copyFileNoOverwrite(qmudHome, path, destinationPath, stats);
		}
	}

	void copyTopLevelDatabases(const QString &mushclientRoot, const QString &qmudHome,
	                           QMudMushclientImportUtils::ImportStats &stats)
	{
		const QDir          sourceDir(mushclientRoot);
		const QFileInfoList entries =
		    sourceDir.entryInfoList(QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
		for (const QFileInfo &entry : entries)
		{
			if (entry.suffix().compare(QStringLiteral("db"), Qt::CaseInsensitive) != 0)
				continue;
			if (entry.isSymLink())
			{
				++stats.filesSkippedFiltered;
				continue;
			}
			(void)copyFileNoOverwrite(qmudHome, entry.absoluteFilePath(),
			                          QDir(qmudHome).filePath(entry.fileName()), stats);
		}
	}

	QString settingsStatusText(const QSettings::Status status)
	{
		switch (status)
		{
		case QSettings::NoError:
			return QStringLiteral("no error");
		case QSettings::AccessError:
			return QStringLiteral("access error");
		case QSettings::FormatError:
			return QStringLiteral("format error");
		}
		return QStringLiteral("unknown error");
	}

	bool migrateLegacyIniToQmudConfDetailed(const QString &sourceDir, const QString &destinationDir,
	                                        const bool archiveSource, QString &error, QStringList *warnings,
	                                        int *convertedWorlds)
	{
		const QString sourceBaseDir      = QDir::cleanPath(sourceDir);
		const QString destinationBaseDir = QDir::cleanPath(destinationDir);
		const QString legacyPath =
		    findTopLevelFileWithCaseFallback(sourceBaseDir, QStringLiteral("MUSHclient.ini"));
		if (legacyPath.isEmpty() || !QFileInfo::exists(legacyPath))
			return true;
		if (QFileInfo(legacyPath).isSymLink())
		{
			error =
			    QStringLiteral("Legacy config file is a symlink and was not imported: %1").arg(legacyPath);
			return false;
		}

		QFile legacyFile(legacyPath);
		if (!legacyFile.open(QIODevice::ReadOnly))
		{
			error = QStringLiteral("Unable to read legacy config file: %1").arg(legacyPath);
			return false;
		}
		legacyFile.close();

		const QString newPath = QDir(destinationBaseDir).filePath(QStringLiteral("QMud.conf"));
		if (const QString destinationError = importDestinationError(destinationBaseDir, newPath);
		    !destinationError.isEmpty())
		{
			error = destinationError;
			return false;
		}
		const QSettings               legacy(legacyPath, QSettings::IniFormat);
		QSettings                     modern(newPath, QSettings::IniFormat);
		const QHash<QString, QString> rawValues = parseLegacyIniRawValues(legacyPath);
		const QStringList             keys      = legacy.allKeys();
		if (legacy.status() != QSettings::NoError)
		{
			error = QStringLiteral("Unable to parse legacy config file %1: %2")
			            .arg(legacyPath, settingsStatusText(legacy.status()));
			return false;
		}
		for (const QString &key : keys)
		{
			modern.setValue(key, migrateLegacyIniValueWithRawSource(
			                         sourceBaseDir, destinationBaseDir, key, legacy.value(key), rawValues,
			                         archiveSource, warnings, convertedWorlds));
		}
		pruneLegacyCtrlBarsGroups(modern);
		modern.sync();
		if (modern.status() != QSettings::NoError)
		{
			error = QStringLiteral("Unable to write migrated config file %1: %2")
			            .arg(newPath, settingsStatusText(modern.status()));
			return false;
		}

		if (!archiveSource)
			return true;

		const QString migratedDir = QDir(destinationBaseDir).filePath(QStringLiteral("migrated"));
		if (!QDir().mkpath(migratedDir))
		{
			error = QStringLiteral("Failed to create migrated directory: %1").arg(migratedDir);
			return false;
		}

		const QString migratedPath = QDir(migratedDir).filePath(QStringLiteral("MUSHclient.ini"));
		if (QFileInfo::exists(migratedPath) && !QFile::remove(migratedPath))
		{
			error = QStringLiteral("Failed to replace archived legacy config file: %1").arg(migratedPath);
			return false;
		}
		if (!QFile::rename(legacyPath, migratedPath))
		{
			error = QStringLiteral("Failed to archive legacy config file: %1 -> %2")
			            .arg(legacyPath, migratedPath);
			return false;
		}
		return true;
	}

	struct SourcePreferenceTable
	{
			QString name;
			QString createSql;
	};

	bool copyOpenPreferencesDatabase(const QSqlDatabase &sourceDb, QSqlDatabase &destinationDb,
	                                 QMudMushclientImportUtils::ImportStats &stats)
	{
		QSqlQuery tableQuery(sourceDb);
		if (!tableQuery.exec(QStringLiteral("SELECT name, sql FROM sqlite_master WHERE type = 'table' "
		                                    "AND name NOT LIKE 'sqlite_%' ORDER BY name")))
		{
			recordImportError(stats, QStringLiteral("Unable to inspect MUSHclient preferences database: %1")
			                             .arg(tableQuery.lastError().text()));
			return false;
		}

		QVector<SourcePreferenceTable> sourceTables;
		while (tableQuery.next())
			sourceTables.push_back({tableQuery.value(0).toString(), tableQuery.value(1).toString()});

		if (sourceTables.isEmpty())
		{
			recordImportError(stats,
			                  QStringLiteral("MUSHclient preferences database has no importable tables."));
			return false;
		}

		if (!destinationDb.transaction())
		{
			recordImportError(stats, QStringLiteral("Unable to begin QMud preferences import transaction: %1")
			                             .arg(destinationDb.lastError().text()));
			return false;
		}

		auto failTransaction = [&](const QString &message)
		{
			(void)destinationDb.rollback();
			recordImportError(stats, message);
			return false;
		};

		QSqlQuery destinationQuery(destinationDb);
		auto      dropDestinationObjects = [&](const QString &type, const QString &dropKeyword) -> bool
		{
			QSqlQuery objectQuery(destinationDb);
			if (!objectQuery.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type = '%1' "
			                                     "AND name NOT LIKE 'sqlite_%' ORDER BY name")
			                          .arg(type)))
			{
				return failTransaction(QStringLiteral("Unable to inspect QMud preferences %1 objects: %2")
				                           .arg(type, objectQuery.lastError().text()));
			}

			QStringList names;
			while (objectQuery.next())
				names.push_back(objectQuery.value(0).toString());

			for (const QString &name : names)
			{
				if (!destinationQuery.exec(
				        QStringLiteral("DROP %1 IF EXISTS %2").arg(dropKeyword, quotedSqlIdentifier(name))))
				{
					return failTransaction(QStringLiteral("Unable to clear QMud preferences %1 %2: %3")
					                           .arg(type, name, destinationQuery.lastError().text()));
				}
			}
			return true;
		};

		if (!dropDestinationObjects(QStringLiteral("trigger"), QStringLiteral("TRIGGER")) ||
		    !dropDestinationObjects(QStringLiteral("view"), QStringLiteral("VIEW")) ||
		    !dropDestinationObjects(QStringLiteral("index"), QStringLiteral("INDEX")) ||
		    !dropDestinationObjects(QStringLiteral("table"), QStringLiteral("TABLE")))
		{
			return false;
		}

		for (const SourcePreferenceTable &table : sourceTables)
		{
			if (table.createSql.trimmed().isEmpty() || !destinationQuery.exec(table.createSql))
			{
				return failTransaction(QStringLiteral("Unable to create QMud preferences table %1: %2")
				                           .arg(table.name, destinationQuery.lastError().text()));
			}

			QSqlQuery sourceRows(sourceDb);
			if (!sourceRows.exec(QStringLiteral("SELECT * FROM %1").arg(quotedSqlIdentifier(table.name))))
			{
				return failTransaction(QStringLiteral("Unable to read MUSHclient preferences table %1: %2")
				                           .arg(table.name, sourceRows.lastError().text()));
			}

			const QSqlRecord record = sourceRows.record();
			if (record.count() == 0)
				continue;

			QStringList columnNames;
			columnNames.reserve(record.count());
			for (int column = 0; column < record.count(); ++column)
				columnNames.push_back(quotedSqlIdentifier(record.fieldName(column)));

			QSqlQuery insertQuery(destinationDb);
			if (!insertQuery.prepare(QStringLiteral("INSERT INTO %1 (%2) VALUES (%3)")
			                             .arg(quotedSqlIdentifier(table.name),
			                                  columnNames.join(QLatin1Char(',')),
			                                  questionPlaceholders(record.count()))))
			{
				return failTransaction(
				    QStringLiteral("Unable to prepare QMud preferences import for table %1: %2")
				        .arg(table.name, insertQuery.lastError().text()));
			}

			while (sourceRows.next())
			{
				for (int column = 0; column < record.count(); ++column)
					insertQuery.bindValue(column, sourceRows.value(column));
				if (!insertQuery.exec())
				{
					return failTransaction(QStringLiteral("Unable to import QMud preferences table %1: %2")
					                           .arg(table.name, insertQuery.lastError().text()));
				}
			}
		}

		QSqlQuery schemaQuery(sourceDb);
		if (!schemaQuery.exec(QStringLiteral("SELECT sql FROM sqlite_master WHERE type IN "
		                                     "('index', 'trigger', 'view') AND sql IS NOT NULL "
		                                     "ORDER BY type, name")))
		{
			return failTransaction(QStringLiteral("Unable to inspect MUSHclient preferences schema: %1")
			                           .arg(schemaQuery.lastError().text()));
		}
		while (schemaQuery.next())
		{
			if (!destinationQuery.exec(schemaQuery.value(0).toString()))
			{
				return failTransaction(QStringLiteral("Unable to import MUSHclient preferences schema: %1")
				                           .arg(destinationQuery.lastError().text()));
			}
		}

		if (!destinationDb.commit())
		{
			recordImportError(stats, QStringLiteral("Unable to commit QMud preferences import: %1")
			                             .arg(destinationDb.lastError().text()));
			(void)destinationDb.rollback();
			return false;
		}

		return true;
	}
} // namespace

namespace QMudMushclientImportUtils
{
	QString migrateLegacyWorldFilePath(const QString &sourceBaseDir, const QString &destinationBaseDir,
	                                   const QString &path, const bool archiveLegacySource)
	{
		QSet<QString>                    activeConversions;
		const LegacyWorldMigrationResult result =
		    migrateLegacyWorldFilePathDetailed(sourceBaseDir, destinationBaseDir, path, archiveLegacySource,
		                                       &activeConversions, nullptr, nullptr);
		appendMigrationWarning(nullptr, result.warning);
		if (!result.error.isEmpty())
			qWarning() << result.error;
		return result.path;
	}

	QString migrateWorldListPaths(const QString &baseDir, const QString &worldList, bool *changed)
	{
		const QStringList items = splitSerializedWorldList(worldList);
		QStringList       migrated;
		migrated.reserve(items.size());
		bool anyChanged = false;
		for (const QString &item : items)
		{
			const QString value = migrateLegacyWorldFilePath(baseDir, baseDir, item, true);
			if (value != item)
				anyChanged = true;
			migrated.push_back(value);
		}
		const QString joined = migrated.join(QLatin1Char('*'));
		if (changed)
			*changed = anyChanged || (joined != worldList);
		return joined;
	}

	QString canonicalizeWorldListForRuntime(const QString &worldList)
	{
		return splitSerializedWorldList(worldList).join(QLatin1Char('*'));
	}

	QString migrateLegacyPluginFilePath(const QString &baseDir, const QString &path)
	{
		QString normalizedPath = stripOptionalQuotes(normalizePathString(path).trimmed());
		if (normalizedPath.startsWith(QStringLiteral("./")) && normalizedPath.size() > 3 &&
		    normalizedPath.at(2).isLetter() && normalizedPath.at(3) == QLatin1Char(':'))
		{
			normalizedPath = normalizedPath.mid(2);
		}
		if (normalizedPath.startsWith(QLatin1Char('/')) && normalizedPath.size() > 3 &&
		    normalizedPath.at(1).isLetter() && normalizedPath.at(2) == QLatin1Char(':'))
		{
			normalizedPath = normalizedPath.mid(1);
		}
		if (normalizedPath.trimmed().isEmpty())
			return normalizedPath;

		const QString defaultAbsolutePath = absolutePathFromBase(baseDir, normalizedPath);
		if (const QString resolvedPath = QFileInfo::exists(defaultAbsolutePath)
		                                     ? QDir::cleanPath(defaultAbsolutePath)
		                                     : resolveExistingPathCaseInsensitive(defaultAbsolutePath);
		    !resolvedPath.isEmpty())
		{
			return containedPluginStoragePath(baseDir, resolvedPath);
		}

		if (const QString remappedPath = resolveExistingPathCaseInsensitive(
		        remapLegacyWindowsWorldPathToBase(baseDir, normalizedPath));
		    !remappedPath.isEmpty())
		{
			return containedPluginStoragePath(baseDir, remappedPath);
		}

		return containedPluginStoragePath(baseDir, normalizedPath);
	}

	QString migratePluginListPaths(const QString &baseDir, const QString &pluginList, bool *changed)
	{
		return migratePluginListPathsDetailed(baseDir, baseDir, pluginList, changed);
	}

	QString canonicalizePluginListForRuntime(const QString &pluginList)
	{
		return splitSerializedPathList(pluginList).join(QLatin1Char('*'));
	}

	void migrateLegacyWorldTree(const QString &baseDir, const QString &worldDirectory)
	{
		const QString normalizedWorldDir = normalizePathString(worldDirectory.trimmed());
		if (normalizedWorldDir.isEmpty())
			return;

		const QString absoluteWorldDir = absolutePathFromBase(baseDir, normalizedWorldDir);
		const QDir    rootDir(absoluteWorldDir);
		if (!rootDir.exists())
			return;

		QStringList  legacyWorldFiles;
		QDirIterator iterator(rootDir.absolutePath(),
		                      QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
		                      QDirIterator::Subdirectories);
		while (iterator.hasNext())
		{
			const QString filePath = iterator.next();
			const QString suffix   = iterator.fileInfo().suffix().toLower();
			if (!QMudFileExtensions::isLegacyWorldSuffix(suffix))
				continue;
			legacyWorldFiles.push_back(QDir::cleanPath(filePath));
		}

		for (const QString &legacyAbsolutePath : legacyWorldFiles)
		{
			const QString migrationInput = archiveRelativePathFor(baseDir, legacyAbsolutePath);
			(void)migrateLegacyWorldFilePath(baseDir, baseDir, migrationInput, true);
		}
	}

	QString migrateLegacyIniPathValue(const QString &baseDir, const QString &key, const QString &value)
	{
		return migrateLegacyIniPathValue(baseDir, baseDir, key, value, true);
	}

	QString migrateLegacyIniPathValue(const QString &sourceBaseDir, const QString &destinationBaseDir,
	                                  const QString &key, const QString &value,
	                                  const bool archiveLegacySource)
	{
		return migrateLegacyIniPathValueDetailed(sourceBaseDir, destinationBaseDir, key, value,
		                                         archiveLegacySource, nullptr, nullptr);
	}

	void migrateLegacyIniToQmudConf(const QString &sourceDir, const QString &destinationDir,
	                                const bool archiveSource)
	{
		QString error;
		if (!migrateLegacyIniToQmudConfDetailed(sourceDir, destinationDir, archiveSource, error, nullptr,
		                                        nullptr))
			qWarning() << error;
	}

	bool isLikelyCorruptedRelativeWorldPath(const QString &path)
	{
		const QString normalized = normalizePathString(path.trimmed());
		if (normalized.isEmpty())
			return false;
		if (const QString suffix = QFileInfo(normalized).suffix().toLower();
		    !QMudFileExtensions::isWorldSuffix(suffix))
			return false;
		if (!normalized.startsWith(QLatin1Char('.')))
			return false;
		return !normalized.contains(QLatin1Char('/'));
	}

	ImportStats importDirectory(const QString &mushclientRoot, const QString &qmudHome)
	{
		ImportStats stats;

		importWorldsTree(mushclientRoot, qmudHome, stats);
		copyImportDirectoryTree(qmudHome, QDir(mushclientRoot).filePath(QStringLiteral("lua")),
		                        QDir(qmudHome).filePath(QStringLiteral("lua")), stats,
		                        copyLuaImportFileFilter);
		copyImportDirectoryTree(qmudHome, QDir(mushclientRoot).filePath(QStringLiteral("scripts")),
		                        QDir(qmudHome).filePath(QStringLiteral("scripts")), stats,
		                        copyAllImportFileFilter);
		copyImportDirectoryTree(qmudHome, QDir(mushclientRoot).filePath(QStringLiteral("logs")),
		                        QDir(qmudHome).filePath(QStringLiteral("logs")), stats,
		                        copyAllImportFileFilter);

		if (!findTopLevelFileWithCaseFallback(mushclientRoot, QStringLiteral("MUSHclient.ini")).isEmpty())
		{
			QString     error;
			QStringList warnings;
			if (migrateLegacyIniToQmudConfDetailed(mushclientRoot, qmudHome, false, error, &warnings,
			                                       &stats.worldsConverted))
			{
				for (const QString &warning : warnings)
					recordImportWarning(stats, warning);
				++stats.configsImported;
			}
			else
			{
				for (const QString &warning : warnings)
					recordImportWarning(stats, warning);
				recordImportError(stats, error);
			}
		}
		copyTopLevelDatabases(mushclientRoot, qmudHome, stats);

		return stats;
	}

	void importPreferencesDatabase(const QString &mushclientRoot, QSqlDatabase &destinationDb,
	                               ImportStats &stats)
	{
		const QString sourcePath =
		    findTopLevelFileWithCaseFallback(mushclientRoot, QStringLiteral("mushclient_prefs.sqlite"));
		if (sourcePath.isEmpty() || !QFileInfo(sourcePath).isFile())
			return;
		if (QFileInfo(sourcePath).isSymLink())
		{
			++stats.filesSkippedFiltered;
			return;
		}
		if (!destinationDb.isOpen())
		{
			recordImportError(stats, QStringLiteral("QMud preferences database is not open."));
			return;
		}

		const QString sourceConnectionName =
		    QStringLiteral("qmud_mushclient_prefs_import_%1").arg(reinterpret_cast<quintptr>(&stats));
		QSqlDatabase sourceDb = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), sourceConnectionName);
		sourceDb.setDatabaseName(sourcePath);
		sourceDb.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
		if (!sourceDb.open())
		{
			recordImportError(stats, QStringLiteral("Unable to open MUSHclient preferences database: %1")
			                             .arg(sourceDb.lastError().text()));
			sourceDb.close();
			sourceDb = QSqlDatabase();
			QSqlDatabase::removeDatabase(sourceConnectionName);
			return;
		}

		if (copyOpenPreferencesDatabase(sourceDb, destinationDb, stats))
			++stats.preferenceDatabasesImported;

		sourceDb.close();
		sourceDb = QSqlDatabase();
		QSqlDatabase::removeDatabase(sourceConnectionName);
	}
} // namespace QMudMushclientImportUtils
