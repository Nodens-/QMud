/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_MushclientImportUtils.cpp
 * Role: Integration coverage for explicit MUSHclient directory import behavior.
 */

#include "../../src/FileExtensions.h"
#include "../../src/helpers/MushclientImportUtils.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFile>
#include <QFileInfo>
// ReSharper disable once CppUnusedIncludeDirective
#include <QSettings>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryDir>
// ReSharper disable once CppUnusedIncludeDirective
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>
#include <QtTest/QtTest>
#include <filesystem>

class tst_MushclientImportUtils final : public QObject
{
		Q_OBJECT

	private slots:
		static void importsDirectoryReadOnlyWithFiltersAndNoOverwrite();
#ifndef Q_OS_WIN
		static void rejectsDestinationSymlinkEscape();
		static void rejectsReferencedLegacyWorldSymlinkButMigratesReference();
		static void rejectsTopLevelConfigAndPrefsSymlinks();
#endif
};

namespace
{
	void writeFile(const QString &path, const QByteArray &data)
	{
		QVERIFY2(QDir().mkpath(QFileInfo(path).absolutePath()),
		         qPrintable(QStringLiteral("Unable to create parent for %1").arg(path)));
		QFile file(path);
		QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
		         qPrintable(QStringLiteral("Unable to write %1").arg(path)));
		QCOMPARE(file.write(data), data.size());
	}

	QByteArray pluginXml(const QString &name, const QString &id, const QString &language)
	{
		return QStringLiteral(
		           R"(<?xml version="1.0" encoding="utf-8"?>
<muclient>
<plugin name="%1" author="QMudTest" id="%2" language="%3" purpose="Import test">
<script>print("ok")</script>
</plugin>
</muclient>
)")
		    .arg(name, id, language)
		    .toUtf8();
	}

	void openTestDatabase(const QString &path, const QString &connectionName, QSqlDatabase &db)
	{
		db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
		db.setDatabaseName(path);
		QVERIFY2(db.open(),
		         qPrintable(
		             QStringLiteral("Unable to open test database %1: %2").arg(path, db.lastError().text())));
	}

	void execSql(const QSqlDatabase &db, const QString &sql)
	{
		QSqlQuery query(db);
		QVERIFY2(query.exec(sql),
		         qPrintable(QStringLiteral("SQL failed: %1\n%2").arg(sql, query.lastError().text())));
	}
} // namespace

void tst_MushclientImportUtils::importsDirectoryReadOnlyWithFiltersAndNoOverwrite()
{
	QTemporaryDir temp;
	QVERIFY(temp.isValid());

	const QDir    root(temp.path());
	const QString mush = root.filePath(QStringLiteral("mush"));
	const QString home = root.filePath(QStringLiteral("home"));
	QVERIFY(QDir().mkpath(mush));
	QVERIFY(QDir().mkpath(home));
	const QString externalWorld       = root.filePath(QStringLiteral("outside.mcl"));
	const QString iniOnlyWorld        = root.filePath(QStringLiteral("ini-only.mcl"));
	const QString missingWorld        = root.filePath(QStringLiteral("missing.mcl"));
	const QString iniMissingWorld     = root.filePath(QStringLiteral("ini-missing.mcl"));
	const QString ansiIniWorld        = QDir(mush).filePath(QStringLiteral("caf\u00e9.mcl"));
	const QString win1252IniWorld     = QDir(mush).filePath(QStringLiteral("price\u20ac.mcl"));
	const QString fallbackWorld       = root.filePath(QStringLiteral("fallback.qdl"));
	const QString externalLog         = root.filePath(QStringLiteral("external.log"));
	const QString externalPlugin      = root.filePath(QStringLiteral("ExternalPlugin.xml"));
	const QString rootSharedWorld     = QDir(mush).filePath(QStringLiteral("shared.mcl"));
	const QString relativeSharedWorld = QDir(mush).filePath(QStringLiteral("shared/relative.mcl"));
	writeFile(externalWorld, QByteArrayLiteral(R"(<?xml version="1.0" encoding="ISO-8859-1"?>
<muclient>
<world name="caf)") + QByteArray(1, static_cast<char>(0xE9)) +
	                             QByteArrayLiteral(R"(" log_file_name=")") + externalLog.toLatin1() +
	                             QByteArrayLiteral(R"(" />
</muclient>
)"));
	writeFile(iniOnlyWorld, QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?><muclient><world "
	                                          "name=\"ini-only\" /></muclient>"));
	writeFile(ansiIniWorld, QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?><muclient><world "
	                                          "name=\"caf\xc3\xa9\" /></muclient>"));
	writeFile(win1252IniWorld, QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?><muclient><world "
	                                             "name=\"price\xe2\x82\xac\" /></muclient>"));
	writeFile(
	    QMudFileExtensions::replaceOrAppendExtension(fallbackWorld, QStringLiteral("mcl")),
	    QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?><muclient><world name=\"fallback\" "
	                      "/></muclient>"));
	writeFile(rootSharedWorld, QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?><muclient><world "
	                                             "name=\"root-shared\" /></muclient>"));
	writeFile(relativeSharedWorld,
	          QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?><muclient><world "
	                            "name=\"relative-shared\" /></muclient>"));
	writeFile(QDir(mush).filePath(QStringLiteral("worlds/nested/child.mcl")),
	          QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?><muclient><world "
	                            "name=\"nested-child\" /></muclient>"));
	writeFile(QDir(mush).filePath(QStringLiteral("worlds/TestSubdir/existing.mcl")),
	          QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?><muclient><world "
	                            "name=\"mm-existing\" /></muclient>"));

	writeFile(QDir(mush).filePath(QStringLiteral("worlds/main.mcl")),
	          QStringLiteral(
	              R"(<?xml version="1.0" encoding="utf-8"?>
<muclient>
<world script_file="scripts/run.lua" module_file="lua/module.lua" log_file_name="%1" world_file="%2" missing_file="%3" fallback_file="%4" root_file="%5" relative_root_file="../shared/relative.mcl" nested_file="%6" fixture_existing_file="TestSubdir\existing.mcl" fixture_missing_file="TestSubdir\missing.mcl" fixture_absolute_missing_file="C:\MUSHclient\worlds\TestSubdir\missing_abs.mcl" fixture_portable_missing_file=".\worlds\TestSubdir\missing_portable.mcl" flat_missing_file="flat_missing.mcl" />
<include name="plugins/Good.xml" plugin="y" />
<include name="TestSubdir\fixture-plugin.xml" plugin="y" />
<include name="C:\MUSHclient\worlds\plugins\TestSubdir\Absolute.xml" plugin="y" />
<include name=".\worlds\plugins\TestSubdir\Portable.xml" plugin="y" />
<plugin source="%7" />
<plugin source="lua/module.lua" />
<variables>
<variable name="db_path">%8</variable>
</variables>
</muclient>
)")
	              .arg(QDir(mush).filePath(QStringLiteral("logs/main.log")), externalWorld, missingWorld,
	                   fallbackWorld, rootSharedWorld,
	                   QDir(mush).filePath(QStringLiteral("worlds/nested/child.mcl")), externalPlugin,
	                   QDir(mush).filePath(QStringLiteral("map.DB")))
	              .toUtf8());
	writeFile(QDir(mush).filePath(QStringLiteral("worlds/already.mcl")), QByteArrayLiteral("new legacy"));
	writeFile(QDir(home).filePath(QStringLiteral("worlds/already.qdl")), QByteArrayLiteral("existing qdl"));
	writeFile(QDir(mush).filePath(QStringLiteral("worlds/support.txt")), QByteArrayLiteral("support"));
	writeFile(QDir(mush).filePath(QStringLiteral("worlds/existing.txt")), QByteArrayLiteral("source"));
	writeFile(QDir(home).filePath(QStringLiteral("worlds/existing.txt")), QByteArrayLiteral("destination"));

	writeFile(QDir(mush).filePath(QStringLiteral("worlds/plugins/Good.xml")),
	          pluginXml(QStringLiteral("GoodPlugin"), QStringLiteral("0123456789abcdef01234567"),
	                    QStringLiteral("Lua")));
	writeFile(QDir(mush).filePath(QStringLiteral("worlds/plugins/TestSubdir/fixture-plugin.xml")),
	          pluginXml(QStringLiteral("AcceleratorsPlugin"), QStringLiteral("2123456789abcdef01234567"),
	                    QStringLiteral("Lua")));
	writeFile(QDir(mush).filePath(QStringLiteral("worlds/plugins/TestSubdir/Absolute.xml")),
	          pluginXml(QStringLiteral("AbsolutePlugin"), QStringLiteral("3123456789abcdef01234567"),
	                    QStringLiteral("Lua")));
	writeFile(QDir(mush).filePath(QStringLiteral("worlds/plugins/TestSubdir/Portable.xml")),
	          pluginXml(QStringLiteral("PortablePlugin"), QStringLiteral("4123456789abcdef01234567"),
	                    QStringLiteral("Lua")));
	writeFile(QDir(mush).filePath(QStringLiteral("worlds/plugins/NotAPlugin.xml")),
	          QByteArrayLiteral("<muclient><world name=\"x\"/></muclient>"));
	writeFile(QDir(mush).filePath(QStringLiteral("worlds/plugins/VbPlugin.xml")),
	          pluginXml(QStringLiteral("VbPlugin"), QStringLiteral("1123456789abcdef01234567"),
	                    QStringLiteral("VBscript")));
	writeFile(QDir(mush).filePath(QStringLiteral("worlds/plugins/AutoSave.xml")),
	          pluginXml(QStringLiteral("AutoSave"), QStringLiteral("8238deec7c06bade8ebc3819"),
	                    QStringLiteral("Lua")));
	writeFile(QDir(mush).filePath(QStringLiteral("worlds/plugins/helper.vbs")),
	          QByteArrayLiteral("' legacy script"));
	writeFile(QDir(mush).filePath(QStringLiteral("worlds/plugins/data.dat")), QByteArrayLiteral("data"));

	writeFile(QDir(mush).filePath(QStringLiteral("lua/module.lua")), QByteArrayLiteral("return {}"));
	writeFile(QDir(mush).filePath(QStringLiteral("lua/socket.dll")), QByteArrayLiteral("dll"));
	writeFile(QDir(mush).filePath(QStringLiteral("scripts/run.lua")), QByteArrayLiteral("print('run')"));
	writeFile(QDir(mush).filePath(QStringLiteral("logs/main.log")), QByteArrayLiteral("imported log"));
	writeFile(QDir(mush).filePath(QStringLiteral("logs/existing.log")), QByteArrayLiteral("source log"));
	writeFile(QDir(home).filePath(QStringLiteral("logs/existing.log")), QByteArrayLiteral("destination log"));
	writeFile(QDir(mush).filePath(QStringLiteral("map.DB")), QByteArrayLiteral("db"));
	{
		const QString sourcePrefsPath      = QDir(mush).filePath(QStringLiteral("MUSHCLIENT_PREFS.SQLITE"));
		const QString sourceConnectionName = QStringLiteral("tst_mushclient_import_source_prefs");
		QSqlDatabase  sourceDb;
		openTestDatabase(sourcePrefsPath, sourceConnectionName, sourceDb);
		execSql(sourceDb, QStringLiteral("CREATE TABLE prefs (name TEXT PRIMARY KEY, value TEXT NOT NULL)"));
		execSql(sourceDb, QStringLiteral("INSERT INTO prefs (name, value) VALUES ('ImportedPref', 'yes')"));
		execSql(sourceDb, QStringLiteral("CREATE TABLE worlds (name TEXT PRIMARY KEY, value TEXT NOT NULL)"));
		execSql(sourceDb, QStringLiteral("INSERT INTO worlds (name, value) VALUES ('ImportedWorld', '42')"));
		execSql(sourceDb, QStringLiteral("CREATE INDEX prefs_value_idx ON prefs(value)"));
		execSql(sourceDb, QStringLiteral("CREATE VIEW prefs_view AS SELECT name, value FROM prefs"));
		execSql(sourceDb, QStringLiteral("CREATE TRIGGER prefs_insert_trigger AFTER INSERT ON prefs "
		                                 "BEGIN UPDATE worlds SET value = value WHERE name = "
		                                 "'ImportedWorld'; END"));
		sourceDb.close();
		sourceDb = QSqlDatabase();
		QSqlDatabase::removeDatabase(sourceConnectionName);
	}
	const QString destinationPrefsPath      = QDir(home).filePath(QStringLiteral("QMud.sqlite"));
	const QString destinationConnectionName = QStringLiteral("tst_mushclient_import_destination_prefs");
	QSqlDatabase  destinationPreferencesDb;
	openTestDatabase(destinationPrefsPath, destinationConnectionName, destinationPreferencesDb);
	execSql(destinationPreferencesDb,
	        QStringLiteral("CREATE TABLE prefs (name TEXT PRIMARY KEY, value TEXT NOT NULL)"));
	execSql(destinationPreferencesDb,
	        QStringLiteral("INSERT INTO prefs (name, value) VALUES ('ExistingPref', 'old')"));
	execSql(destinationPreferencesDb, QStringLiteral("CREATE VIEW prefs_view AS SELECT name FROM prefs"));
	execSql(destinationPreferencesDb, QStringLiteral("CREATE VIEW stale_view AS SELECT name FROM prefs"));
	writeFile(QDir(mush).filePath(QStringLiteral("mUsHcLiEnT.InI")),
	          QStringLiteral("CtrlBars-Bar0=x\nCtrlBars-Summary=x\n[Global prefs]\nWorldList=%1*%2*")
	                  .arg(QDir(mush).filePath(QStringLiteral("worlds/main.mcl")), iniOnlyWorld)
	                  .toUtf8() +
	              QByteArrayLiteral("caf") + QByteArray(1, static_cast<char>(0xE9)) +
	              QByteArrayLiteral(".mcl*price") + QByteArray(1, static_cast<char>(0x80)) +
	              QStringLiteral(".mcl*%1\nPluginList=%2*%3\n")
	                  .arg(iniMissingWorld, QDir(mush).filePath(QStringLiteral("worlds/plugins/Good.xml")),
	                       externalPlugin)
	                  .toUtf8());

	QMudMushclientImportUtils::ImportStats stats = QMudMushclientImportUtils::importDirectory(mush, home);
	QMudMushclientImportUtils::importPreferencesDatabase(mush, destinationPreferencesDb, stats);

	QCOMPARE(stats.errors, 0);
	QCOMPARE(stats.warnings, 7);
	const QString warningText = stats.warningDetails.join(QLatin1Char('\n'));
	QVERIFY(warningText.contains(QStringLiteral("missing.mcl")));
	QVERIFY(warningText.contains(QStringLiteral("../shared/relative.mcl")));
	QVERIFY(warningText.contains(QStringLiteral("ini-missing.mcl")));
	QVERIFY(warningText.contains(QStringLiteral("TestSubdir/missing.mcl")));
	QVERIFY(warningText.contains(QStringLiteral("TestSubdir/missing_abs.mcl")));
	QVERIFY(warningText.contains(QStringLiteral("TestSubdir/missing_portable.mcl")));
	QVERIFY(warningText.contains(QStringLiteral("flat_missing.mcl")));
	QCOMPARE(stats.worldsConverted, 9);
	QCOMPARE(stats.configsImported, 1);
	QCOMPARE(stats.preferenceDatabasesImported, 1);
	QVERIFY(stats.filesSkippedExisting >= 2);
	QVERIFY(stats.filesSkippedFiltered >= 5);

	QCOMPARE(QFileInfo(QDir(mush).filePath(QStringLiteral("worlds/main.mcl"))).exists(), true);
	QCOMPARE(QFileInfo(QDir(mush).filePath(QStringLiteral("mUsHcLiEnT.InI"))).exists(), true);
	QVERIFY(!QFileInfo(QDir(mush).filePath(QStringLiteral("migrated/MUSHclient.ini"))).exists());

	QFile converted(QDir(home).filePath(QStringLiteral("worlds/main.qdl")));
	QVERIFY(converted.open(QIODevice::ReadOnly));
	const QByteArray convertedData = converted.readAll();
	QVERIFY(convertedData.contains(R"(script_file="./scripts/run.lua")"));
	QVERIFY(convertedData.contains(R"(module_file="./lua/module.lua")"));
	QVERIFY(convertedData.contains(R"(log_file_name="./logs/main.log")"));
	QVERIFY(convertedData.contains(R"(world_file="./worlds/outside.qdl")"));
	QVERIFY(convertedData.contains(R"(missing_file="./worlds/missing.qdl")"));
	QVERIFY(convertedData.contains(R"(fallback_file="./worlds/fallback.qdl")"));
	QVERIFY(convertedData.contains(R"(root_file="./worlds/shared.qdl")"));
	QVERIFY(convertedData.contains(R"(relative_root_file="./worlds/relative.qdl")"));
	QVERIFY(convertedData.contains(R"(nested_file="./worlds/nested/child.qdl")"));
	QVERIFY(convertedData.contains(R"(fixture_existing_file="./worlds/TestSubdir/existing.qdl")"));
	QVERIFY(convertedData.contains(R"(fixture_missing_file="./worlds/TestSubdir/missing.qdl")"));
	QVERIFY(convertedData.contains(R"(fixture_absolute_missing_file="./worlds/TestSubdir/missing_abs.qdl")"));
	QVERIFY(convertedData.contains(
	    R"(fixture_portable_missing_file="./worlds/TestSubdir/missing_portable.qdl")"));
	QVERIFY(convertedData.contains(R"(flat_missing_file="./worlds/flat_missing.qdl")"));
	QVERIFY(convertedData.contains(R"(name="./worlds/plugins/Good.xml")"));
	QVERIFY(convertedData.contains(R"(name="./worlds/plugins/TestSubdir/fixture-plugin.xml")"));
	QVERIFY(convertedData.contains(R"(name="./worlds/plugins/TestSubdir/Absolute.xml")"));
	QVERIFY(convertedData.contains(R"(name="./worlds/plugins/TestSubdir/Portable.xml")"));
	QVERIFY(convertedData.contains(R"(source="./worlds/plugins/ExternalPlugin.xml")"));
	QVERIFY(convertedData.contains(R"(source="./worlds/plugins/module.lua")"));
	QVERIFY(convertedData.contains(QByteArrayLiteral(">./map.DB<")));
	QVERIFY(!convertedData.contains(externalWorld.toUtf8()));
	QVERIFY(!convertedData.contains(missingWorld.toUtf8()));
	QVERIFY(!convertedData.contains(fallbackWorld.toUtf8()));
	QVERIFY(!convertedData.contains(rootSharedWorld.toUtf8()));
	QVERIFY(!convertedData.contains(relativeSharedWorld.toUtf8()));
	QVERIFY(!convertedData.contains(externalPlugin.toUtf8()));
	QVERIFY(!QFileInfo(QDir(mush).filePath(QStringLiteral("worlds/main.qdl"))).exists());
	QVERIFY(QFileInfo(QDir(home).filePath(QStringLiteral("worlds/outside.qdl"))).exists());
	QVERIFY(QFileInfo(QDir(home).filePath(QStringLiteral("worlds/shared.qdl"))).exists());
	QVERIFY(!QFileInfo(QDir(home).filePath(QStringLiteral("shared.qdl"))).exists());
	QVERIFY(!QFileInfo(QDir(home).filePath(QStringLiteral("worlds/relative.qdl"))).exists());
	QVERIFY(!QFileInfo(QDir(home).filePath(QStringLiteral("shared/relative.qdl"))).exists());
	QVERIFY(QFileInfo(QDir(home).filePath(QStringLiteral("worlds/nested/child.qdl"))).exists());
	QVERIFY(QFileInfo(QDir(home).filePath(QStringLiteral("worlds/TestSubdir/existing.qdl"))).exists());
	QVERIFY(!QFileInfo(QDir(home).filePath(QStringLiteral("worlds/TestSubdir/missing.qdl"))).exists());
	QVERIFY(!QFileInfo(QDir(home).filePath(QStringLiteral("worlds/TestSubdir/missing_abs.qdl"))).exists());
	QVERIFY(
	    !QFileInfo(QDir(home).filePath(QStringLiteral("worlds/TestSubdir/missing_portable.qdl"))).exists());
	QVERIFY(!QFileInfo(QDir(home).filePath(QStringLiteral("worlds/flat_missing.qdl"))).exists());
	QVERIFY(QFileInfo(QDir(home).filePath(QStringLiteral("worlds/ini-only.qdl"))).exists());
	QVERIFY(QFileInfo(QDir(home).filePath(QStringLiteral("worlds/caf\u00e9.qdl"))).exists());
	QVERIFY(QFileInfo(QDir(home).filePath(QStringLiteral("worlds/price\u20ac.qdl"))).exists());
	QVERIFY(!QFileInfo(QDir(home).filePath(QStringLiteral("worlds/ini-missing.qdl"))).exists());
	QVERIFY(!QFileInfo(QDir(home).filePath(QStringLiteral("worlds/missing.qdl"))).exists());
	QVERIFY(QFileInfo(QDir(home).filePath(QStringLiteral("worlds/fallback.qdl"))).exists());
	QFile recursivelyConverted(QDir(home).filePath(QStringLiteral("worlds/outside.qdl")));
	QVERIFY(recursivelyConverted.open(QIODevice::ReadOnly));
	const QByteArray recursivelyConvertedData = recursivelyConverted.readAll();
	QVERIFY(recursivelyConvertedData.contains(R"(encoding="UTF-8")"));
	QVERIFY(recursivelyConvertedData.contains(QByteArrayLiteral("caf\xc3\xa9")));
	QVERIFY(recursivelyConvertedData.contains(R"(log_file_name="./external.log")"));
	QVERIFY(!recursivelyConvertedData.contains(externalLog.toUtf8()));

	QFile fallbackConverted(QDir(home).filePath(QStringLiteral("worlds/fallback.qdl")));
	QVERIFY(fallbackConverted.open(QIODevice::ReadOnly));
	QVERIFY(fallbackConverted.readAll().contains(R"(name="fallback")"));

	QFile alreadyConverted(QDir(home).filePath(QStringLiteral("worlds/already.qdl")));
	QVERIFY(alreadyConverted.open(QIODevice::ReadOnly));
	QCOMPARE(alreadyConverted.readAll(), QByteArrayLiteral("existing qdl"));

	QFile existing(QDir(home).filePath(QStringLiteral("worlds/existing.txt")));
	QVERIFY(existing.open(QIODevice::ReadOnly));
	QCOMPARE(existing.readAll(), QByteArrayLiteral("destination"));

	QVERIFY(QFileInfo(QDir(home).filePath(QStringLiteral("worlds/plugins/Good.xml"))).exists());
	QVERIFY(QFileInfo(QDir(home).filePath(QStringLiteral("worlds/plugins/TestSubdir/fixture-plugin.xml")))
	            .exists());
	QVERIFY(
	    QFileInfo(QDir(home).filePath(QStringLiteral("worlds/plugins/TestSubdir/Absolute.xml"))).exists());
	QVERIFY(
	    QFileInfo(QDir(home).filePath(QStringLiteral("worlds/plugins/TestSubdir/Portable.xml"))).exists());
	QVERIFY(QFileInfo(QDir(home).filePath(QStringLiteral("worlds/plugins/data.dat"))).exists());
	QVERIFY(!QFileInfo(QDir(home).filePath(QStringLiteral("worlds/plugins/NotAPlugin.xml"))).exists());
	QVERIFY(!QFileInfo(QDir(home).filePath(QStringLiteral("worlds/plugins/VbPlugin.xml"))).exists());
	QVERIFY(!QFileInfo(QDir(home).filePath(QStringLiteral("worlds/plugins/AutoSave.xml"))).exists());
	QVERIFY(!QFileInfo(QDir(home).filePath(QStringLiteral("worlds/plugins/helper.vbs"))).exists());

	QVERIFY(QFileInfo(QDir(home).filePath(QStringLiteral("lua/module.lua"))).exists());
	QVERIFY(!QFileInfo(QDir(home).filePath(QStringLiteral("lua/socket.dll"))).exists());
	QVERIFY(QFileInfo(QDir(home).filePath(QStringLiteral("scripts/run.lua"))).exists());
	QFile importedLog(QDir(home).filePath(QStringLiteral("logs/main.log")));
	QVERIFY(importedLog.open(QIODevice::ReadOnly));
	QCOMPARE(importedLog.readAll(), QByteArrayLiteral("imported log"));
	QFile existingLog(QDir(home).filePath(QStringLiteral("logs/existing.log")));
	QVERIFY(existingLog.open(QIODevice::ReadOnly));
	QCOMPARE(existingLog.readAll(), QByteArrayLiteral("destination log"));
	QVERIFY(QFileInfo(QDir(home).filePath(QStringLiteral("map.DB"))).exists());
	QVERIFY(QFileInfo(QDir(home).filePath(QStringLiteral("QMud.sqlite"))).exists());
	{
		QSqlQuery importedPrefs(destinationPreferencesDb);
		QVERIFY(importedPrefs.exec(QStringLiteral("SELECT value FROM prefs WHERE name = 'ExistingPref'")));
		QVERIFY(!importedPrefs.next());
		QVERIFY(importedPrefs.exec(QStringLiteral("SELECT value FROM prefs WHERE name = 'ImportedPref'")));
		QVERIFY(importedPrefs.next());
		QCOMPARE(importedPrefs.value(0).toString(), QStringLiteral("yes"));
		QVERIFY(importedPrefs.exec(QStringLiteral("SELECT value FROM worlds WHERE name = 'ImportedWorld'")));
		QVERIFY(importedPrefs.next());
		QCOMPARE(importedPrefs.value(0).toString(), QStringLiteral("42"));
		QVERIFY(importedPrefs.exec(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE name = "
		                                          "'stale_view'")));
		QVERIFY(importedPrefs.next());
		QCOMPARE(importedPrefs.value(0).toInt(), 0);
		QVERIFY(importedPrefs.exec(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE name IN "
		                                          "('prefs_value_idx', 'prefs_view', "
		                                          "'prefs_insert_trigger')")));
		QVERIFY(importedPrefs.next());
		QCOMPARE(importedPrefs.value(0).toInt(), 3);
	}
	destinationPreferencesDb.close();
	destinationPreferencesDb = QSqlDatabase();
	QSqlDatabase::removeDatabase(destinationConnectionName);

	QSettings imported(QDir(home).filePath(QStringLiteral("QMud.conf")), QSettings::IniFormat);
	QCOMPARE(imported.value(QStringLiteral("Global prefs/WorldList")).toString(),
	         QStringLiteral("./worlds/main.qdl*./worlds/ini-only.qdl*./worlds/caf\u00e9.qdl*./worlds/"
	                        "price\u20ac.qdl*./worlds/ini-missing.qdl"));
	QCOMPARE(imported.value(QStringLiteral("Global prefs/PluginList")).toString(),
	         QStringLiteral("./worlds/plugins/Good.xml*./worlds/plugins/ExternalPlugin.xml"));
	QVERIFY(imported.value(QStringLiteral("CtrlBars-Bar0")).toString().isEmpty());
	QVERIFY(imported.value(QStringLiteral("CtrlBars-Summary")).toString().isEmpty());
}

#ifndef Q_OS_WIN
void tst_MushclientImportUtils::rejectsDestinationSymlinkEscape()
{
	QTemporaryDir temp;
	QVERIFY(temp.isValid());

	const QDir    root(temp.path());
	const QString mush    = root.filePath(QStringLiteral("mush"));
	const QString home    = root.filePath(QStringLiteral("home"));
	const QString outside = root.filePath(QStringLiteral("outside"));
	QVERIFY(QDir().mkpath(QDir(mush).filePath(QStringLiteral("worlds"))));
	QVERIFY(QDir().mkpath(home));
	QVERIFY(QDir().mkpath(outside));

	writeFile(QDir(mush).filePath(QStringLiteral("worlds/support.txt")), QByteArrayLiteral("support"));
	writeFile(QDir(mush).filePath(QStringLiteral("MUSHclient.ini")),
	          QByteArrayLiteral("[Global prefs]\nWorldList=worlds/support.txt\n"));

	std::error_code error;
	std::filesystem::create_directory_symlink(
	    outside.toStdString(), QDir(home).filePath(QStringLiteral("worlds")).toStdString(), error);
	if (error)
		QSKIP(qPrintable(QStringLiteral("Directory symlinks unavailable: %1")
		                     .arg(QString::fromStdString(error.message()))));

	std::filesystem::create_symlink(QDir(outside).filePath(QStringLiteral("QMud.conf")).toStdString(),
	                                QDir(home).filePath(QStringLiteral("QMud.conf")).toStdString(), error);
	if (error)
		QSKIP(qPrintable(
		    QStringLiteral("File symlinks unavailable: %1").arg(QString::fromStdString(error.message()))));

	const QMudMushclientImportUtils::ImportStats stats =
	    QMudMushclientImportUtils::importDirectory(mush, home);

	QVERIFY(stats.errors >= 2);
	QVERIFY(!QFileInfo(QDir(outside).filePath(QStringLiteral("support.txt"))).exists());
	QVERIFY(!QFileInfo(QDir(outside).filePath(QStringLiteral("QMud.conf"))).exists());
}

void tst_MushclientImportUtils::rejectsReferencedLegacyWorldSymlinkButMigratesReference()
{
	QTemporaryDir temp;
	QVERIFY(temp.isValid());

	const QDir    root(temp.path());
	const QString mush    = root.filePath(QStringLiteral("mush"));
	const QString home    = root.filePath(QStringLiteral("home"));
	const QString outside = root.filePath(QStringLiteral("outside"));
	QVERIFY(QDir().mkpath(QDir(mush).filePath(QStringLiteral("worlds"))));
	QVERIFY(QDir().mkpath(home));
	QVERIFY(QDir().mkpath(outside));

	const QString targetPath = QDir(outside).filePath(QStringLiteral("linked-target.mcl"));
	writeFile(targetPath, QByteArrayLiteral("not xml"));

	std::error_code error;
	std::filesystem::create_symlink(targetPath.toStdString(),
	                                QDir(mush).filePath(QStringLiteral("worlds/link.mcl")).toStdString(),
	                                error);
	if (error)
		QSKIP(qPrintable(
		    QStringLiteral("File symlinks unavailable: %1").arg(QString::fromStdString(error.message()))));

	writeFile(QDir(mush).filePath(QStringLiteral("worlds/main.mcl")),
	          QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?><muclient><world "
	                            "world_file=\"link.mcl\" /></muclient>"));

	const QMudMushclientImportUtils::ImportStats stats =
	    QMudMushclientImportUtils::importDirectory(mush, home);

	QCOMPARE(stats.errors, 0);
	QCOMPARE(stats.warnings, 1);
	QVERIFY(stats.warningDetails.join(QLatin1Char('\n')).contains(QStringLiteral("link.mcl")));

	QFile converted(QDir(home).filePath(QStringLiteral("worlds/main.qdl")));
	QVERIFY(converted.open(QIODevice::ReadOnly));
	QVERIFY(converted.readAll().contains(R"(world_file="./worlds/link.qdl")"));
	QVERIFY(!QFileInfo(QDir(home).filePath(QStringLiteral("worlds/link.qdl"))).exists());
}

void tst_MushclientImportUtils::rejectsTopLevelConfigAndPrefsSymlinks()
{
	QTemporaryDir temp;
	QVERIFY(temp.isValid());

	const QDir    root(temp.path());
	const QString mush    = root.filePath(QStringLiteral("mush"));
	const QString home    = root.filePath(QStringLiteral("home"));
	const QString outside = root.filePath(QStringLiteral("outside"));
	QVERIFY(QDir().mkpath(mush));
	QVERIFY(QDir().mkpath(home));
	QVERIFY(QDir().mkpath(outside));

	const QString externalIni = QDir(outside).filePath(QStringLiteral("MUSHclient.ini"));
	const QString externalDb  = QDir(outside).filePath(QStringLiteral("mushclient_prefs.sqlite"));
	writeFile(externalIni, QByteArrayLiteral("[Global prefs]\nWorldList=outside.mcl\n"));
	writeFile(externalDb, QByteArrayLiteral("external prefs"));

	const QString destinationConnectionName =
	    QStringLiteral("tst_mushclient_import_symlink_destination_prefs");
	QSqlDatabase destinationPreferencesDb;
	openTestDatabase(QDir(home).filePath(QStringLiteral("QMud.sqlite")), destinationConnectionName,
	                 destinationPreferencesDb);

	std::error_code error;
	std::filesystem::create_symlink(externalIni.toStdString(),
	                                QDir(mush).filePath(QStringLiteral("MUSHclient.ini")).toStdString(),
	                                error);
	if (error)
		QSKIP(qPrintable(
		    QStringLiteral("File symlinks unavailable: %1").arg(QString::fromStdString(error.message()))));

	std::filesystem::create_symlink(
	    externalDb.toStdString(),
	    QDir(mush).filePath(QStringLiteral("mushclient_prefs.sqlite")).toStdString(), error);
	if (error)
		QSKIP(qPrintable(
		    QStringLiteral("File symlinks unavailable: %1").arg(QString::fromStdString(error.message()))));

	QMudMushclientImportUtils::ImportStats stats = QMudMushclientImportUtils::importDirectory(mush, home);
	QMudMushclientImportUtils::importPreferencesDatabase(mush, destinationPreferencesDb, stats);
	destinationPreferencesDb.close();
	destinationPreferencesDb = QSqlDatabase();
	QSqlDatabase::removeDatabase(destinationConnectionName);

	QCOMPARE(stats.configsImported, 0);
	QCOMPARE(stats.preferenceDatabasesImported, 0);
	QVERIFY(stats.errors >= 1);
	QVERIFY(stats.filesSkippedFiltered >= 1);
	QVERIFY(!QFileInfo(QDir(home).filePath(QStringLiteral("QMud.conf"))).exists());
}
#endif

QTEST_MAIN(tst_MushclientImportUtils)
#include "tst_MushclientImportUtils.moc"
