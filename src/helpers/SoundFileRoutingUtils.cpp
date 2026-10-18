/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: SoundFileRoutingUtils.cpp
 * Role: Sound backend routing helpers for file-based playback.
 */

#include "SoundFileRoutingUtils.h"
#include "PluginPathUtils.h"

#include <QByteArray>
#include <QDir>
// ReSharper disable once CppUnusedIncludeDirective
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QStringList>
#include <algorithm>
#include <array>
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

namespace
{
	/**
	 * @brief Reads an unsigned 16-bit little-endian integer from byte data.
	 * @param data Source byte data.
	 * @return Decoded integer.
	 */
	quint16 littleEndianUInt16(const QByteArray &data)
	{
		return static_cast<quint16>(static_cast<quint8>(data.at(0))) |
		       static_cast<quint16>(static_cast<quint8>(data.at(1)) << 8U);
	}

	/**
	 * @brief Reads an unsigned 32-bit little-endian integer from WAVE chunk-header size bytes.
	 * @param data Source byte data.
	 * @return Decoded integer.
	 */
	quint32 littleEndianChunkSize(const QByteArray &data)
	{
		return static_cast<quint32>(static_cast<quint8>(data.at(4))) |
		       (static_cast<quint32>(static_cast<quint8>(data.at(5))) << 8U) |
		       (static_cast<quint32>(static_cast<quint8>(data.at(6))) << 16U) |
		       (static_cast<quint32>(static_cast<quint8>(data.at(7))) << 24U);
	}

	/**
	 * @brief Checks if a WAVE fmt chunk represents a simple QSoundEffect-friendly format.
	 * @param formatChunk WAVE fmt chunk payload.
	 * @return `true` when the format should remain on QSoundEffect.
	 */
	bool waveFormatChunkUsesSoundEffect(const QByteArray &formatChunk)
	{
		constexpr quint16              kWaveFormatPcm                 = 0x0001;
		constexpr quint16              kWaveFormatExtensible          = 0xFFFE;
		constexpr qsizetype            kWaveExtensibleSubFormatOffset = 24;
		constexpr std::array<char, 16> kPcmSubFormatGuid{'\x01', '\x00', '\x00',
		                                                 '\x00', '\x00', '\x00',
		                                                 '\x10', '\x00', static_cast<char>(0x80),
		                                                 '\x00', '\x00', static_cast<char>(0xAA),
		                                                 '\x00', '\x38', static_cast<char>(0x9B),
		                                                 '\x71'};

		if (formatChunk.size() < 2)
			return false;

		const quint16 formatTag = littleEndianUInt16(formatChunk);
		if (formatTag == kWaveFormatPcm)
			return true;
		if (formatTag != kWaveFormatExtensible)
			return false;
		if (formatChunk.size() <
		    kWaveExtensibleSubFormatOffset + static_cast<qsizetype>(kPcmSubFormatGuid.size()))
			return false;

		return std::equal(kPcmSubFormatGuid.cbegin(), kPcmSubFormatGuid.cend(),
		                  formatChunk.constData() + kWaveExtensibleSubFormatOffset);
	}

	/**
	 * @brief Checks if a WAVE file should use QSoundEffect.
	 * @param fileName Absolute file path to inspect.
	 * @return `true` when QSoundEffect is appropriate.
	 */
	bool waveFileUsesSoundEffect(const QString &fileName)
	{
		QFile file(fileName);
		if (!file.open(QIODevice::ReadOnly))
			return false;

		const QByteArray riffHeader = file.read(12);
		if (riffHeader.size() != 12 || riffHeader.first(4) != QByteArrayLiteral("RIFF") ||
		    riffHeader.sliced(8, 4) != QByteArrayLiteral("WAVE"))
			return false;

		const qint64 fileSize = file.size();
		while (file.pos() + 8 <= fileSize)
		{
			const QByteArray chunkHeader = file.read(8);
			if (chunkHeader.size() != 8)
				return false;

			const QByteArray chunkId   = chunkHeader.first(4);
			const quint32    chunkSize = littleEndianChunkSize(chunkHeader);
			if (chunkId == QByteArrayLiteral("fmt "))
			{
				const qint64     bytesToRead = qMin<qint64>(chunkSize, 40);
				const QByteArray formatChunk = file.read(bytesToRead);
				return waveFormatChunkUsesSoundEffect(formatChunk);
			}

			const qint64 nextChunkPosition = file.pos() + static_cast<qint64>(chunkSize) + (chunkSize % 2U);
			if (nextChunkPosition < file.pos() || nextChunkPosition > fileSize ||
			    !file.seek(nextChunkPosition))
				return false;
		}

		return false;
	}
} // namespace

SoundFilePlaybackBackend soundFilePlaybackBackendForFile(const QString &fileName)
{
	const QString suffix = QFileInfo(fileName).suffix().toLower();
	if (suffix != QStringLiteral("wav") && suffix != QStringLiteral("wave"))
		return SoundFilePlaybackBackend::QMediaPlayer;

	return waveFileUsesSoundEffect(fileName) ? SoundFilePlaybackBackend::QSoundEffect
	                                         : SoundFilePlaybackBackend::QMediaPlayer;
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
