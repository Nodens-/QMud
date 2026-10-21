/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: SoundFileRoutingUtils.h
 * Role: Sound path validation and playback-capability helpers.
 */

#ifndef QMUD_SOUNDFILEROUTINGUTILS_H
#define QMUD_SOUNDFILEROUTINGUTILS_H

#include <QSet>
#include <QString>

inline constexpr char kDefaultWorldSoundDirectory[] = "./sounds/";

/** @brief Validated per-world sound-directory paths used by persistence and worker routing. */
struct WorldSoundDirectoryPaths
{
		/** @brief Canonical dot-relative value written to the world file. */
		QString storedPath;
		/** @brief Absolute directory used for ordinary per-world file lookup. */
		QString absolutePath;
		/** @brief Directory below the shared sounds root used by the worker catalog. */
		QString catalogPrefix;
};

/**
 * @brief Resolves a persisted per-world sound directory beneath the QMud sounds tree.
 * @param qmudHome Resolved QMud home directory.
 * @param configuredPath Candidate relative directory setting; an empty value selects the default.
 * @param paths Destination receiving normalized storage, absolute, and catalog-relative paths.
 * @param error Optional destination receiving a validation diagnostic.
 * @return `true` when the directory is safely contained beneath `./sounds/`.
 */
[[nodiscard]] bool    resolveWorldSoundDirectory(const QString &qmudHome, const QString &configuredPath,
                                                 WorldSoundDirectoryPaths *paths, QString *error = nullptr);

/**
 * @brief Checks whether a containment root or any component below it is a symbolic link.
 * @param rootDirectory Existing containment root.
 * @param absolutePath Absolute candidate at or below the root.
 * @return `true` when the root or an existing path component below it is a symbolic link.
 */
[[nodiscard]] bool    soundPathContainsSymlink(const QString &rootDirectory, const QString &absolutePath);

/**
 * @brief Resolves an existing supported audio file case-insensitively beneath one containment root.
 * @param rootDirectory Existing containment root.
 * @param relativePath Relative file path using native or portable separators.
 * @param resolvedPath Destination receiving the canonical file path.
 * @return `true` when the request names a readable regular audio file without traversing a symbolic link.
 */
[[nodiscard]] bool    resolveSoundFileCaseInsensitively(const QString &rootDirectory,
                                                        const QString &relativePath, QString *resolvedPath);

/**
 * @brief Converts a native, portable, or legacy sound path to a path relative to the sounds tree.
 * @param path Sound path supplied by a compatibility API or protocol.
 * @param explicitlyRooted Optional destination reporting whether the input named the `sounds` root.
 * @return Forward-slash relative path, or an empty string for an invalid or root-only path.
 */
[[nodiscard]] QString soundTreeRelativePath(QString path, bool *explicitlyRooted = nullptr);

/**
 * @brief Returns lower-case file suffixes supported for playback by the active Qt Multimedia backend.
 * @return Process-wide immutable set of supported suffixes without leading dots.
 */
[[nodiscard]] const QSet<QString> &soundPlaybackSupportedFileSuffixes();

/**
 * @brief Checks whether a file name has a suffix supported by the active Qt Multimedia backend.
 * @param fileName File name or path to inspect.
 * @return `true` when the suffix is supported for decoding.
 */
[[nodiscard]] bool                 soundPlaybackSupportsFile(const QString &fileName);

#endif // QMUD_SOUNDFILEROUTINGUTILS_H
