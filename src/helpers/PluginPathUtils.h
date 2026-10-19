/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: PluginPathUtils.h
 * Role: Path normalization for plugin file APIs and shared QMud-home containment policy.
 */

#ifndef QMUD_PLUGINPATHUTILS_H
#define QMUD_PLUGINPATHUTILS_H

#include <QString>

/**
 * @brief Helpers for normalizing paths and enforcing the resolved QMud home directory boundary.
 */
namespace QMudPluginPathUtils
{
	/**
	 * @brief Normalizes path separators to POSIX style and collapses repeated slashes.
	 * @param path Path text supplied by scripts or runtime state.
	 * @return Separator-normalized path.
	 */
	[[nodiscard]] QString normalizeSeparators(QString path);
	/**
	 * @brief Interprets a legacy absolute or mixed-separator path as relative to the resolved QMud home directory.
	 * @param path Script-supplied path.
	 * @return QMud-home-relative POSIX path, or an empty string when parent traversal is present.
	 */
	[[nodiscard]] QString legacyPathRelativeToQmudHome(QString path);
	/**
	 * @brief Resolves a script file path under the resolved QMud home directory and rejects symlink/canonical escapes.
	 * @param qmudHome Resolved QMud home directory path.
	 * @param rawPath Script-supplied path.
	 * @param resolvedPath Receives the absolute normalized path on success.
	 * @param error Receives a diagnostic on failure.
	 * @return True when the path is safely contained inside the resolved QMud home directory.
	 */
	[[nodiscard]] bool    resolveInsideQmudHome(const QString &qmudHome, const QString &rawPath,
	                                            QString *resolvedPath, QString *error = nullptr);
	/**
	 * @brief Resolves a relative path inside an existing directory and rejects canonical or symlink escapes.
	 * @param rootDirectory Existing directory that owns the relative path.
	 * @param relativePath Relative path using native or portable separators.
	 * @param resolvedPath Receives the absolute normalized path on success.
	 * @param error Receives a diagnostic on failure.
	 * @return True when the relative path is safely contained inside the directory.
	 */
	[[nodiscard]] bool    resolveInsideDirectory(const QString &rootDirectory, const QString &relativePath,
	                                             QString *resolvedPath, QString *error = nullptr);
	/**
	 * @brief Tests whether a plugins directory would overlap QMud home or its reserved sounds tree.
	 * @param qmudHome Resolved QMud home directory path.
	 * @param pluginsDirectory Configured plugins directory in native, portable, or legacy form.
	 * @return True when the directory resolves to QMud home or anywhere beneath `QMUD_HOME/sounds`.
	 */
	[[nodiscard]] bool    pluginDirectoryConflictsWithSoundStorage(const QString &qmudHome,
	                                                               const QString &pluginsDirectory);
	/**
	 * @brief Converts a path to a QMud-home-relative POSIX path for script metadata APIs.
	 * @param qmudHome Resolved QMud home directory path.
	 * @param path Path to convert.
	 * @param trailingSlash True when directory-like values should end in `/`.
	 * @return Relative POSIX path, or `./` for the resolved QMud home directory itself when requested.
	 */
	[[nodiscard]] QString qmudHomeRelativePath(const QString &qmudHome, const QString &path,
	                                           bool trailingSlash);
	/**
	 * @brief Tests whether a path is inside a root directory after separator cleanup.
	 * @param path Path to test.
	 * @param root Root directory.
	 * @return True when path equals root or is below it.
	 */
	[[nodiscard]] bool    pathIsWithinOrEqualTo(const QString &path, const QString &root);
	/**
	 * @brief Tests whether one canonical native path equals or resides below another canonical native path.
	 * @param path Canonical native path to test.
	 * @param root Canonical native root directory.
	 * @return True when path equals root or is below it according to platform path case rules.
	 */
	[[nodiscard]] bool    canonicalPathIsWithinOrEqualTo(const QString &path, const QString &root);
} // namespace QMudPluginPathUtils

#endif // QMUD_PLUGINPATHUTILS_H
