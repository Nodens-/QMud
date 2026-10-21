/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: SoundFileRoutingUtils.cpp
 * Role: Sound path validation and playback-capability helpers.
 */

#include "SoundFileRoutingUtils.h"
#include "PluginPathUtils.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <utility>

#if QMUD_ENABLE_SOUND
#include <QMediaFormat>
#include <QMimeType>
#endif

namespace
{
	/**
	 * @brief Resolves existing sound-directory components case-insensitively beneath the sounds root.
	 * @param soundsRoot Existing canonical sounds root.
	 * @param relativePath Validated relative directory path.
	 * @param resolvedPath Receives the actual spelling of existing components and the requested missing suffix.
	 * @param error Optional destination receiving a diagnostic.
	 * @return `true` when the directory path remains contained and contains no symbolic links.
	 */
	bool resolveSoundDirectoryCaseInsensitively(const QString &soundsRoot, const QString &relativePath,
	                                            QString *resolvedPath, QString *error)
	{
		const QString canonicalRoot =
		    QMudPluginPathUtils::normalizeSeparators(QFileInfo(soundsRoot).canonicalFilePath());
		if (canonicalRoot.isEmpty() || !QFileInfo(canonicalRoot).isDir())
		{
			if (error)
				*error = QStringLiteral("The sounds directory is unavailable.");
			return false;
		}

		const QStringList parts   = QMudPluginPathUtils::normalizeSeparators(relativePath)
		                                .split(QLatin1Char('/'), Qt::SkipEmptyParts);
		QString           current = canonicalRoot;
		for (qsizetype index = 0; index < parts.size(); ++index)
		{
			const QDir      directory(current);
			const QFileInfo exact(directory.filePath(parts.at(index)));
			QFileInfo       matched;
			if (exact.exists() || exact.isSymLink())
				matched = exact;
			else
			{
				const QFileInfoList entries = directory.entryInfoList(
				    QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
				const auto entry = std::ranges::find_if(
				    entries, [&parts, index](const QFileInfo &candidate)
				    { return candidate.fileName().compare(parts.at(index), Qt::CaseInsensitive) == 0; });
				if (entry != entries.cend())
					matched = *entry;
			}

			if (!matched.exists() && !matched.isSymLink())
			{
				current = QMudPluginPathUtils::normalizeSeparators(
				    QDir(current).filePath(parts.mid(index).join(QLatin1Char('/'))));
				break;
			}
			if (matched.isSymLink())
			{
				if (error)
					*error = QStringLiteral("The configured sound path cannot contain symbolic links.");
				return false;
			}
			if (!matched.isDir())
			{
				if (error)
					*error = QStringLiteral("The configured sound path is not a directory.");
				return false;
			}
			current = QMudPluginPathUtils::normalizeSeparators(matched.canonicalFilePath());
			if (current.isEmpty() ||
			    !QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(current, canonicalRoot))
			{
				if (error)
					*error = QStringLiteral("The configured sound path resolves outside ./sounds/.");
				return false;
			}
		}

		if (resolvedPath)
			*resolvedPath = current;
		return true;
	}
} // namespace

bool soundPathContainsSymlink(const QString &rootDirectory, const QString &absolutePath)
{
	const QString normalizedRoot = QMudPluginPathUtils::normalizeSeparators(QDir::cleanPath(rootDirectory));
	const QString normalizedPath = QMudPluginPathUtils::normalizeSeparators(QDir::cleanPath(absolutePath));
	if (normalizedRoot.isEmpty())
		return false;
	if (QFileInfo(normalizedRoot).isSymLink())
		return true;
	if (!QMudPluginPathUtils::pathIsWithinOrEqualTo(normalizedPath, normalizedRoot))
		return false;

	const QString relative =
	    QMudPluginPathUtils::normalizeSeparators(QDir(normalizedRoot).relativeFilePath(normalizedPath));
	const QStringList parts   = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
	QString           current = normalizedRoot;
	for (const QString &part : parts)
	{
		const QFileInfo component(QDir(current).filePath(part));
		if (component.isSymLink())
			return true;
		current = component.absoluteFilePath();
	}
	return false;
}

bool resolveSoundFileCaseInsensitively(const QString &rootDirectory, const QString &relativePath,
                                       QString *resolvedPath)
{
	QString containedPath;
	QString error;
	if (!soundPlaybackSupportsFile(relativePath) ||
	    !QMudPluginPathUtils::resolveInsideDirectory(rootDirectory, relativePath, &containedPath, &error))
	{
		return false;
	}

	const QString canonicalRoot =
	    QMudPluginPathUtils::normalizeSeparators(QFileInfo(rootDirectory).canonicalFilePath());
	const QString relative =
	    QMudPluginPathUtils::normalizeSeparators(QDir(canonicalRoot).relativeFilePath(containedPath));
	const QStringList parts = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
	if (canonicalRoot.isEmpty() || parts.isEmpty() ||
	    std::ranges::any_of(parts, [](const QString &part) { return part == QLatin1String(".."); }))
	{
		return false;
	}

	QString current = canonicalRoot;
	for (qsizetype index = 0; index < parts.size(); ++index)
	{
		const QDir      directory(current);
		const QFileInfo exact(directory.filePath(parts.at(index)));
		QFileInfo       matched;
		if (exact.exists() || exact.isSymLink())
			matched = exact;
		else
		{
			const QFileInfoList entries =
			    directory.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
			                            QDir::Name | QDir::IgnoreCase);
			const auto entry = std::ranges::find_if(
			    entries, [&parts, index](const QFileInfo &candidate)
			    { return candidate.fileName().compare(parts.at(index), Qt::CaseInsensitive) == 0; });
			if (entry != entries.cend())
				matched = *entry;
		}

		const bool finalComponent = index + 1 == parts.size();
		if ((!matched.exists() && !matched.isSymLink()) || matched.isSymLink() ||
		    (!finalComponent && !matched.isDir()) ||
		    (finalComponent && (!matched.isFile() || !matched.isReadable())))
		{
			return false;
		}
		current = QMudPluginPathUtils::normalizeSeparators(matched.canonicalFilePath());
		if (current.isEmpty() || !QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(current, canonicalRoot))
		{
			return false;
		}
	}

	if (resolvedPath)
		*resolvedPath = current;
	return true;
}

bool resolveWorldSoundDirectory(const QString &qmudHome, const QString &configuredPath,
                                WorldSoundDirectoryPaths *paths, QString *error)
{
	QString configured = configuredPath;
	if (configured.isEmpty())
		configured = QString::fromLatin1(kDefaultWorldSoundDirectory);
	QString homeResolved;
	QString resolveError;
	if (!QMudPluginPathUtils::resolveInsideQmudHome(qmudHome, configured, &homeResolved, &resolveError))
	{
		if (error)
			*error = resolveError;
		return false;
	}
	const QString     portable = QMudPluginPathUtils::qmudHomeRelativePath(qmudHome, homeResolved, false);
	const QStringList parts    = portable.split(QLatin1Char('/'), Qt::SkipEmptyParts);
	if (parts.isEmpty() || parts.constFirst().compare(QStringLiteral("sounds"), Qt::CaseInsensitive) != 0)
	{
		if (error)
			*error = QStringLiteral("The directory must be beneath ./sounds/.");
		return false;
	}

	QString soundsPath;
	if (!QMudPluginPathUtils::resolveInsideQmudHome(qmudHome, QStringLiteral("sounds"), &soundsPath,
	                                                &resolveError))
	{
		if (error)
			*error = resolveError;
		return false;
	}

	const QString requestedPrefix = parts.mid(1).join(QLatin1Char('/'));
	QString       absolutePath;
	if (!QMudPluginPathUtils::resolveInsideDirectory(
	        soundsPath, requestedPrefix.isEmpty() ? QStringLiteral(".") : requestedPrefix, &absolutePath,
	        &resolveError))
	{
		if (error)
			*error = resolveError;
		return false;
	}
	if (!resolveSoundDirectoryCaseInsensitively(
	        soundsPath, requestedPrefix.isEmpty() ? QStringLiteral(".") : requestedPrefix, &absolutePath,
	        &resolveError))
	{
		if (error)
			*error = resolveError;
		return false;
	}
	const QFileInfo directoryInfo(absolutePath);
	if (soundPathContainsSymlink(soundsPath, absolutePath))
	{
		if (error)
			*error = QStringLiteral("The configured sound path cannot contain symbolic links.");
		return false;
	}
	if (directoryInfo.exists() && !directoryInfo.isDir())
	{
		if (error)
			*error = QStringLiteral("The configured sound path is not a directory.");
		return false;
	}

	if (paths)
	{
		const QString catalogPrefix =
		    QMudPluginPathUtils::normalizeSeparators(QDir(soundsPath).relativeFilePath(absolutePath));
		paths->storedPath = QMudPluginPathUtils::qmudHomeRelativePath(qmudHome, absolutePath, true);
		if (!paths->storedPath.startsWith(QStringLiteral("./")))
			paths->storedPath.prepend(QStringLiteral("./"));
		paths->absolutePath  = absolutePath;
		paths->catalogPrefix = catalogPrefix == QLatin1String(".") ? QString() : catalogPrefix;
	}
	return true;
}

QString soundTreeRelativePath(QString path, bool *explicitlyRooted)
{
	if (explicitlyRooted)
		*explicitlyRooted = false;
	path = QMudPluginPathUtils::legacyPathRelativeToQmudHome(std::move(path));
	if (path.isEmpty())
		return {};

	QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
	if (parts.isEmpty())
		return {};
	if (parts.constFirst().compare(QStringLiteral("sounds"), Qt::CaseInsensitive) == 0)
	{
		if (explicitlyRooted)
			*explicitlyRooted = true;
		parts.removeFirst();
	}
	return parts.join(QLatin1Char('/'));
}

const QSet<QString> &soundPlaybackSupportedFileSuffixes()
{
#if QMUD_ENABLE_SOUND
	static const QSet<QString> kSuffixes = []
	{
		QSet<QString> suffixes;
		QMediaFormat  capabilities;
		for (const QMediaFormat::FileFormat format : capabilities.supportedFileFormats(QMediaFormat::Decode))
		{
			const QMimeType mimeType = QMediaFormat(format).mimeType();
			for (const QString &suffix : mimeType.suffixes())
			{
				const QString normalized = suffix.trimmed().toLower();
				if (!normalized.isEmpty())
					suffixes.insert(normalized);
			}
		}
		return suffixes;
	}();
#else
	static const QSet<QString> kSuffixes;
#endif
	return kSuffixes;
}

bool soundPlaybackSupportsFile(const QString &fileName)
{
	const QString suffix = QFileInfo(fileName).suffix().toLower();
	return !suffix.isEmpty() && soundPlaybackSupportedFileSuffixes().contains(suffix);
}
