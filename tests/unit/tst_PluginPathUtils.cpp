/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_PluginPathUtils.cpp
 * Role: Unit coverage for plugin path normalization and QMUD_HOME containment helpers.
 */

#include "../../src/helpers/PluginPathUtils.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFile>
#include <QObject>
#include <QtTest/QtTest>

namespace
{
	class tst_PluginPathUtils final : public QObject
	{
			Q_OBJECT

		private slots:
			static void resolvesLegacyAbsolutePathsUnderQmudHome();
			static void resolvesNativeAbsolutePathsInsideQmudHome();
			static void rejectsParentTraversalAndSymlinkEscapes();
			static void returnsRelativePosixPathsForGetInfo();
			static void keepsWindowsStyleRelativePathsPosix();
	};

	bool prepareSandbox(QString *rootPath)
	{
		*rootPath = QDir(QDir::currentPath()).filePath(QStringLiteral("qmud_plugin_path_utils_case"));
		QDir root(*rootPath);
		if (root.exists())
			static_cast<void>(root.removeRecursively());
		return QDir().mkpath(QDir(*rootPath).filePath(QStringLiteral("home/worlds/foo"))) &&
		       QDir().mkpath(QDir(*rootPath).filePath(QStringLiteral("home/sounds"))) &&
		       QDir().mkpath(QDir(*rootPath).filePath(QStringLiteral("home/logs")));
	}

	void writeFile(const QString &path)
	{
		QFile file(path);
		QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
		QVERIFY(file.write("x") == 1);
	}

	void tst_PluginPathUtils::resolvesLegacyAbsolutePathsUnderQmudHome()
	{
		QString root;
		QVERIFY(prepareSandbox(&root));
		const QString home = QDir(root).filePath(QStringLiteral("home"));
		writeFile(QDir(home).filePath(QStringLiteral("worlds/foo/bar.txt")));
		writeFile(QDir(home).filePath(QStringLiteral("sounds/alert.txt")));
		writeFile(QDir(home).filePath(QStringLiteral("logs/test.txt")));

		QString resolved;
		QVERIFY(QMudPluginPathUtils::resolveInsideQmudHome(
		    home, QStringLiteral(R"(C:\MUSHclient\worlds\\foo\bar.txt)"), &resolved));
		QCOMPARE(QMudPluginPathUtils::normalizeSeparators(QDir(home).relativeFilePath(resolved)),
		         QStringLiteral("worlds/foo/bar.txt"));

		QVERIFY(QMudPluginPathUtils::resolveInsideQmudHome(
		    home, QStringLiteral(R"(\\legacy\share\sounds\\alert.txt)"), &resolved));
		QCOMPARE(QMudPluginPathUtils::normalizeSeparators(QDir(home).relativeFilePath(resolved)),
		         QStringLiteral("sounds/alert.txt"));

		QVERIFY(QMudPluginPathUtils::resolveInsideQmudHome(
		    home, QStringLiteral("//legacy/share/logs///test.txt"), &resolved));
		QCOMPARE(QMudPluginPathUtils::normalizeSeparators(QDir(home).relativeFilePath(resolved)),
		         QStringLiteral("logs/test.txt"));
	}

	void tst_PluginPathUtils::resolvesNativeAbsolutePathsInsideQmudHome()
	{
		QString root;
		QVERIFY(prepareSandbox(&root));
		const QString home = QDir(root).filePath(QStringLiteral("home"));
		QVERIFY(QDir().mkpath(QDir(home).filePath(QStringLiteral("custom"))));
		writeFile(QDir(home).filePath(QStringLiteral("custom/local.txt")));

		QString resolved;
		QVERIFY(QMudPluginPathUtils::resolveInsideQmudHome(
		    home, QDir(home).filePath(QStringLiteral("custom/local.txt")), &resolved));
		QCOMPARE(QMudPluginPathUtils::normalizeSeparators(QDir(home).relativeFilePath(resolved)),
		         QStringLiteral("custom/local.txt"));
		QCOMPARE(QMudPluginPathUtils::qmudHomeRelativePath(
		             home, QDir(home).filePath(QStringLiteral("custom/local.txt")), false),
		         QStringLiteral("custom/local.txt"));
	}

	void tst_PluginPathUtils::rejectsParentTraversalAndSymlinkEscapes()
	{
		QString root;
		QVERIFY(prepareSandbox(&root));
		const QString home = QDir(root).filePath(QStringLiteral("home"));
		writeFile(QDir(root).filePath(QStringLiteral("outside.txt")));

		QString resolved;
		QString error;
		QVERIFY(!QMudPluginPathUtils::resolveInsideQmudHome(
		    home, QStringLiteral(R"(C:\MUSHclient\worlds\..\outside.txt)"), &resolved, &error));
		QVERIFY(!error.isEmpty());
		QVERIFY(QMudPluginPathUtils::qmudHomeRelativePath(
		            home, QStringLiteral(R"(C:\MUSHclient\worlds\..\outside.txt)"), false)
		            .isEmpty());
		QVERIFY(!QMudPluginPathUtils::pathIsWithinOrEqualTo(QStringLiteral("/tmp/file.txt"), QString()));
		QVERIFY(QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(
		    QStringLiteral("/tmp/qmud/worlds/world.qdl"), QStringLiteral("/tmp/qmud")));
		QVERIFY(!QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(
		    QStringLiteral("/tmp/qmud-sibling/world.qdl"), QStringLiteral("/tmp/qmud")));

		const QString linkPath = QDir(home).filePath(QStringLiteral("worlds/foo/link.txt"));
		if (QFile::link(QDir(root).filePath(QStringLiteral("outside.txt")), linkPath))
		{
			error.clear();
			QVERIFY(!QMudPluginPathUtils::resolveInsideQmudHome(home, QStringLiteral("worlds/foo/link.txt"),
			                                                    &resolved, &error));
			QVERIFY(!error.isEmpty());
		}
	}

	void tst_PluginPathUtils::returnsRelativePosixPathsForGetInfo()
	{
		QString root;
		QVERIFY(prepareSandbox(&root));
		const QString home      = QDir(root).filePath(QStringLiteral("home"));
		const QString worldPath = QDir(home).filePath(QStringLiteral("worlds/foo/test.mcl"));

		QCOMPARE(QMudPluginPathUtils::qmudHomeRelativePath(home, worldPath, false),
		         QStringLiteral("worlds/foo/test.mcl"));
		QCOMPARE(QMudPluginPathUtils::qmudHomeRelativePath(
		             home, QStringLiteral(R"(C:\MUSHclient\worlds\foo\test.mcl)"), false),
		         QStringLiteral("worlds/foo/test.mcl"));
		QCOMPARE(QMudPluginPathUtils::qmudHomeRelativePath(home, home, true), QStringLiteral("./"));
	}

	void tst_PluginPathUtils::keepsWindowsStyleRelativePathsPosix()
	{
		const QString home = QStringLiteral("C:/QMud");

		QCOMPARE(
		    QMudPluginPathUtils::qmudHomeRelativePath(home, QStringLiteral(R"(C:\QMud\worlds\foo\)"), true),
		    QStringLiteral("worlds/foo/"));
		QCOMPARE(QMudPluginPathUtils::qmudHomeRelativePath(
		             home, QStringLiteral(R"(C:\MUSHclient\worlds\\foo\bar.mcl)"), false),
		         QStringLiteral("worlds/foo/bar.mcl"));
		QCOMPARE(QMudPluginPathUtils::qmudHomeRelativePath(
		             home, QStringLiteral(R"(C:\MUSHclient\logs\\session.log)"), false),
		         QStringLiteral("logs/session.log"));
		QVERIFY(QMudPluginPathUtils::qmudHomeRelativePath(
		            home, QStringLiteral(R"(C:\QMud\worlds\..\outside.mcl)"), false)
		            .isEmpty());
	}

} // namespace

QTEST_MAIN(tst_PluginPathUtils)
#include "tst_PluginPathUtils.moc"
