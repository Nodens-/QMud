/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: PluginPathUtils.cpp
 * Role: Path normalization for plugin file APIs and shared QMud-home containment policy.
 */

#include "PluginPathUtils.h"

#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <algorithm>
#include <array>
#include <utility>

namespace
{
	bool isPortableRootSegment(const QString &segment)
	{
		static constexpr std::array<const char *, 8> kPortableRoots = {"worlds", "lua",   "logs",   "plugins",
		                                                               "sounds", "state", "backup", "docs"};
		return std::ranges::any_of(
		    kPortableRoots, [&segment](const char *root)
		    { return segment.compare(QLatin1String(root), Qt::CaseInsensitive) == 0; });
	}

	bool containsParentTraversal(QString path)
	{
		path                       = QMudPluginPathUtils::normalizeSeparators(std::move(path));
		const QStringList segments = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
		return std::ranges::any_of(segments,
		                           [](const QString &segment) { return segment == QLatin1String(".."); });
	}

	bool isAbsolutePathLike(const QString &path)
	{
		if (path.isEmpty())
			return false;
		const QChar first   = path.at(0);
		const bool  isDrive = path.size() > 1 && path.at(1) == QLatin1Char(':') && first.isLetter();
		return isDrive || first == QLatin1Char('/');
	}

	QString directRelativePathUnderRoot(QString path, QString root, bool &matched)
	{
		path = QDir::cleanPath(QMudPluginPathUtils::normalizeSeparators(std::move(path)));
		root = QDir::cleanPath(QMudPluginPathUtils::normalizeSeparators(std::move(root)));
		if (root.endsWith(QLatin1Char('/')))
			root.chop(1);
		matched = false;
		if (root.isEmpty())
			return {};
#ifdef Q_OS_WIN
		const bool equals = path.compare(root, Qt::CaseInsensitive) == 0;
		const bool below  = path.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive);
#else
		const bool equals = path == root;
		const bool below  = path.startsWith(root + QLatin1Char('/'));
#endif
		if (!equals && !below)
			return {};
		matched = true;
		if (equals)
			return {};
		return path.mid(root.size() + 1);
	}

	bool resolveExistingCanonicalParentInsideHome(const QString &absolutePath, const QString &canonicalHome,
	                                              QString *error)
	{
		QFileInfo parentInfo(QFileInfo(absolutePath).absolutePath());
		while (!parentInfo.exists())
		{
			const QString parentPath = parentInfo.absolutePath();
			if (parentPath == parentInfo.filePath())
				break;
			parentInfo.setFile(parentPath);
		}

		const QString canonicalParent =
		    QMudPluginPathUtils::normalizeSeparators(parentInfo.canonicalFilePath());
		if (canonicalParent.isEmpty())
		{
			if (error)
				*error = QStringLiteral("No existing parent directory inside QMud home directory");
			return false;
		}
		if (!QMudPluginPathUtils::pathIsWithinOrEqualTo(canonicalParent, canonicalHome))
		{
			if (error)
				*error = QStringLiteral("Path parent resolves outside QMud home directory");
			return false;
		}
		return true;
	}
} // namespace

QString QMudPluginPathUtils::normalizeSeparators(QString path)
{
	path = path.trimmed();
	if (path.startsWith(QStringLiteral("file://"), Qt::CaseInsensitive))
	{
		const QUrl url(path);
		if (url.isLocalFile())
			path = url.toLocalFile();
		else
			path.remove(0, 7);
	}
	path.replace(QLatin1Char('\\'), QLatin1Char('/'));
	while (path.contains(QStringLiteral("//")))
		path.replace(QStringLiteral("//"), QStringLiteral("/"));
	return path;
}

QString QMudPluginPathUtils::legacyPathRelativeToQmudHome(QString path)
{
	path = normalizeSeparators(std::move(path));
	if (path.size() >= 2 && path.at(1) == QLatin1Char(':') && path.at(0).isLetter())
		path.remove(0, 2);

	const QStringList rawSegments = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
	QStringList       segments;
	segments.reserve(rawSegments.size());
	for (const QString &segment : rawSegments)
	{
		if (segment == QLatin1String("."))
			continue;
		if (segment == QLatin1String(".."))
			return {};
		segments.push_back(segment);
	}

	for (qsizetype i = 0; i < segments.size(); ++i)
	{
		if (isPortableRootSegment(segments.at(i)))
			return segments.mid(i).join(QLatin1Char('/'));
	}

	return segments.join(QLatin1Char('/'));
}

bool QMudPluginPathUtils::pathIsWithinOrEqualTo(const QString &path, const QString &root)
{
	const QString normalizedRootText = normalizeSeparators(root);
	if (normalizedRootText.isEmpty())
		return false;
	const QString normalizedPath = QDir::cleanPath(normalizeSeparators(path));
	const QString normalizedRoot = QDir::cleanPath(normalizedRootText);
	return canonicalPathIsWithinOrEqualTo(normalizedPath, normalizedRoot);
}

bool QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(const QString &path, const QString &root)
{
	if (path.isEmpty() || root.isEmpty())
		return false;

	const QString nativePath = QDir::fromNativeSeparators(path);
	QString       nativeRoot = QDir::fromNativeSeparators(root);
	while (nativeRoot.size() > 1 && nativeRoot.endsWith(QLatin1Char('/')) &&
	       !(nativeRoot.size() == 3 && nativeRoot.at(1) == QLatin1Char(':')))
	{
		nativeRoot.chop(1);
	}
	QString childPrefix = nativeRoot;
	if (!childPrefix.endsWith(QLatin1Char('/')))
		childPrefix.append(QLatin1Char('/'));
#ifdef Q_OS_WIN
	return nativePath.compare(nativeRoot, Qt::CaseInsensitive) == 0 ||
	       nativePath.startsWith(childPrefix, Qt::CaseInsensitive);
#else
	return nativePath == nativeRoot || nativePath.startsWith(childPrefix);
#endif
}

bool QMudPluginPathUtils::resolveInsideQmudHome(const QString &qmudHome, const QString &rawPath,
                                                QString *resolvedPath, QString *error)
{
	const QString home = normalizeSeparators(qmudHome);
	if (home.isEmpty())
	{
		if (error)
			*error = QStringLiteral("QMud home directory is not available");
		return false;
	}

	const QString canonicalHome = normalizeSeparators(QFileInfo(home).canonicalFilePath());
	if (canonicalHome.isEmpty())
	{
		if (error)
			*error = QStringLiteral("QMud home directory does not resolve to an existing directory");
		return false;
	}

	const QString normalizedRaw = normalizeSeparators(rawPath);
	QString       relative;
	if (isAbsolutePathLike(normalizedRaw))
	{
		const QString cleanHome  = QDir::cleanPath(home);
		const QString cleanedRaw = QDir::cleanPath(normalizedRaw);
		if (pathIsWithinOrEqualTo(cleanedRaw, cleanHome))
			relative = normalizeSeparators(QDir(cleanHome).relativeFilePath(cleanedRaw));
		else if (pathIsWithinOrEqualTo(cleanedRaw, canonicalHome))
			relative = normalizeSeparators(QDir(canonicalHome).relativeFilePath(cleanedRaw));
	}
	if (relative.isEmpty())
		relative = legacyPathRelativeToQmudHome(normalizedRaw);
	if (relative.isEmpty() && !normalizeSeparators(rawPath).isEmpty() &&
	    normalizeSeparators(rawPath) != QLatin1String("."))
	{
		if (error)
			*error = QStringLiteral("Path contains an escaping parent segment");
		return false;
	}
	if (relative.isEmpty())
		relative = QStringLiteral(".");

	const QString absolutePath = normalizeSeparators(QDir::cleanPath(QDir(canonicalHome).filePath(relative)));
	if (!pathIsWithinOrEqualTo(absolutePath, canonicalHome))
	{
		if (error)
			*error = QStringLiteral("Path resolves outside QMud home directory");
		return false;
	}

	const QFileInfo targetInfo(absolutePath);
	if (targetInfo.exists() || targetInfo.isSymLink())
	{
		const QString canonicalTarget = normalizeSeparators(targetInfo.canonicalFilePath());
		if (canonicalTarget.isEmpty() || !pathIsWithinOrEqualTo(canonicalTarget, canonicalHome))
		{
			if (error)
				*error = QStringLiteral("Path target resolves outside QMud home directory");
			return false;
		}
	}
	else if (!resolveExistingCanonicalParentInsideHome(absolutePath, canonicalHome, error))
	{
		return false;
	}

	if (resolvedPath)
		*resolvedPath = absolutePath;
	return true;
}

QString QMudPluginPathUtils::qmudHomeRelativePath(const QString &qmudHome, const QString &path,
                                                  const bool trailingSlash)
{
	const QString normalizedPath = normalizeSeparators(path);
	if (normalizedPath.isEmpty())
		return {};
	if (containsParentTraversal(normalizedPath))
		return {};

	QString relative;
	bool    resolvedRelative = false;
	if (!qmudHome.trimmed().isEmpty())
	{
		const QString cleanHome = QDir::cleanPath(normalizeSeparators(qmudHome));
		const QString cleanPath = QDir::cleanPath(normalizedPath);
		if (cleanPath == cleanHome)
		{
			relative.clear();
			resolvedRelative = true;
		}
		else if (pathIsWithinOrEqualTo(cleanPath, cleanHome))
		{
			bool matched = false;
			relative     = directRelativePathUnderRoot(cleanPath, cleanHome, matched);
			if (!matched)
				relative = normalizeSeparators(QDir(cleanHome).relativeFilePath(cleanPath));
			if (relative == QLatin1String("."))
				relative.clear();
			resolvedRelative = true;
		}
	}

	if (!resolvedRelative)
		relative = legacyPathRelativeToQmudHome(normalizedPath);

	relative = normalizeSeparators(QDir::cleanPath(relative.isEmpty() ? QStringLiteral(".") : relative));
	if (relative == QLatin1String("."))
		relative = trailingSlash ? QStringLiteral("./") : QStringLiteral(".");
	else if (trailingSlash && !relative.endsWith(QLatin1Char('/')))
		relative += QLatin1Char('/');
	return relative;
}
