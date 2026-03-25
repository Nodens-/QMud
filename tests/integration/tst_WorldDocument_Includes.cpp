/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldDocument_Includes.cpp
 * Role: QTest coverage for WorldDocument Includes behavior.
 */

#include "TestEnvironment.h"
#include "WorldDocument.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtTest/QTest>
#include <algorithm>

namespace
{
	constexpr unsigned int kIncludeMergeOverwrite = 0x02;
	constexpr unsigned int kIncludeMergeKeep      = 0x04;
	constexpr unsigned int kIncludeMergeWarn      = 0x08;

	QString                fixturePath(const QString &relativePath)
	{
		return QDir(QStringLiteral(QMUD_TEST_SOURCE_DIR)).filePath(relativePath);
	}

	bool writeTextFile(const QString &filePath, const QString &content)
	{
		QFile file(filePath);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
			return false;
		return file.write(content.toUtf8()) == content.toUtf8().size();
	}

	QString triggerSendByName(const WorldDocument &doc, const QString &name)
	{
		for (const WorldDocument::Trigger &trigger : doc.triggers())
		{
			if (trigger.attributes.value(QStringLiteral("name")).compare(name, Qt::CaseInsensitive) == 0)
				return trigger.children.value(QStringLiteral("send"));
		}
		return {};
	}

	bool warningContains(const WorldDocument &doc, const QString &needle)
	{
		return std::ranges::any_of(doc.warnings(),
		                           [&needle](const QString &warning) { return warning.contains(needle); });
	}

	QString pluginVariableByName(const WorldDocument::Plugin &plugin, const QString &name)
	{
		for (const WorldDocument::Variable &variable : plugin.variables)
		{
			if (variable.attributes.value(QStringLiteral("name")).compare(name, Qt::CaseInsensitive) == 0)
				return variable.content;
		}
		return {};
	}
} // namespace

/**
 * @brief QTest fixture covering WorldDocument Includes scenarios.
 */
class tst_WorldDocument_Includes : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void initTestCase()
		{
			QMudTest::applyDeterministicTestEnvironment();
		}

		void expandIncludesMergesWorldAndPluginFixtures()
		{
			const QString worldPath =
			    fixturePath(QStringLiteral("tests/data/worlds/world_with_plugin_include.xml"));
			const QString pluginsDir = fixturePath(QStringLiteral("tests/data/plugins"));
			const QString programDir = fixturePath(QStringLiteral("tests/data"));
			const QString stateDir   = fixturePath(QStringLiteral("tests/data/plugins/state"));

			WorldDocument doc;
			QVERIFY(doc.loadFromFile(worldPath));
			QVERIFY(doc.expandIncludes(worldPath, pluginsDir, programDir, stateDir));

			QCOMPARE(doc.includeFileList(), QStringList({QStringLiteral("include_child.xml")}));
			QCOMPARE(doc.triggers().size(), 2);
			QCOMPARE(triggerSendByName(doc, QStringLiteral("main_trigger")), QStringLiteral("main"));
			QCOMPARE(triggerSendByName(doc, QStringLiteral("child_trigger")), QStringLiteral("child"));

			bool childTriggerIncluded = false;
			for (const WorldDocument::Trigger &trigger : doc.triggers())
			{
				if (trigger.attributes.value(QStringLiteral("name")) == QStringLiteral("child_trigger"))
					childTriggerIncluded = trigger.included;
			}
			QVERIFY(childTriggerIncluded);

			QCOMPARE(doc.plugins().size(), 1);
			const WorldDocument::Plugin plugin = doc.plugins().front();
			QCOMPARE(plugin.attributes.value(QStringLiteral("name")), QStringLiteral("StatePlugin"));
			QCOMPARE(plugin.attributes.value(QStringLiteral("source")),
			         QFileInfo(fixturePath(QStringLiteral("tests/data/plugins/plugin_state_test.xml")))
			             .absoluteFilePath());
			QCOMPARE(plugin.attributes.value(QStringLiteral("directory")),
			         QFileInfo(pluginsDir).absoluteFilePath());
			QCOMPARE(pluginVariableByName(plugin, QStringLiteral("score")), QStringLiteral("99"));
		}

		void duplicatePluginIdIsRejected()
		{
			QMudTest::ScopedTempDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString pluginsPath = QDir(tempDir.path()).filePath(QStringLiteral("plugins"));
			QVERIFY(QDir().mkpath(pluginsPath));
			const QString mainPath    = QDir(tempDir.path()).filePath(QStringLiteral("main.xml"));
			const QString pluginAPath = QDir(pluginsPath).filePath(QStringLiteral("a.xml"));
			const QString pluginBPath = QDir(pluginsPath).filePath(QStringLiteral("b.xml"));

			QVERIFY(writeTextFile(mainPath, QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<qmud>
  <world id="aaaaaaaaaaaaaaaaaaaaaaaa" name="Main"/>
  <include name="a.xml" plugin="y"/>
  <include name="b.xml" plugin="y"/>
</qmud>)")));
			QVERIFY(writeTextFile(pluginAPath, QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin name="PluginA" id="bbbbbbbbbbbbbbbbbbbbbbbb" language="lua"/>
</muclient>)")));
			QVERIFY(writeTextFile(pluginBPath, QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin name="PluginB" id="bbbbbbbbbbbbbbbbbbbbbbbb" language="lua"/>
</muclient>)")));

			WorldDocument doc;
			QVERIFY(doc.loadFromFile(mainPath));
			QVERIFY(!doc.expandIncludes(mainPath, pluginsPath, tempDir.path(), QString()));
			QVERIFY(doc.errorString().contains(QStringLiteral("already loaded")));
		}

		void includeMergeFlagsControlDuplicateTriggerHandling()
		{
			QMudTest::ScopedTempDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString mainPath  = QDir(tempDir.path()).filePath(QStringLiteral("main.xml"));
			const QString childPath = QDir(tempDir.path()).filePath(QStringLiteral("child.xml"));
			QVERIFY(writeTextFile(mainPath, QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<qmud>
  <world id="aaaaaaaaaaaaaaaaaaaaaaaa" name="Main"/>
  <triggers>
    <trigger name="dup">
      <send>main</send>
    </trigger>
  </triggers>
  <include name="child.xml"/>
</qmud>)")));
			QVERIFY(writeTextFile(childPath, QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<qmud>
  <triggers>
    <trigger name="dup">
      <send>child</send>
    </trigger>
  </triggers>
</qmud>)")));

			{
				WorldDocument doc;
				QVERIFY(doc.loadFromFile(mainPath));
				QVERIFY(!doc.expandIncludes(mainPath, tempDir.path(), tempDir.path(), QString()));
				QVERIFY(doc.errorString().contains(QStringLiteral("Duplicate trigger label")));
			}

			{
				WorldDocument doc;
				doc.setIncludeMergeFlags(kIncludeMergeOverwrite | kIncludeMergeWarn);
				QVERIFY(doc.loadFromFile(mainPath));
				QVERIFY(doc.expandIncludes(mainPath, tempDir.path(), tempDir.path(), QString()));
				QCOMPARE(triggerSendByName(doc, QStringLiteral("dup")), QStringLiteral("child"));
				QVERIFY(warningContains(doc, QStringLiteral("overwritten")));
			}

			{
				WorldDocument doc;
				doc.setIncludeMergeFlags(kIncludeMergeKeep | kIncludeMergeWarn);
				QVERIFY(doc.loadFromFile(mainPath));
				QVERIFY(doc.expandIncludes(mainPath, tempDir.path(), tempDir.path(), QString()));
				QCOMPARE(triggerSendByName(doc, QStringLiteral("dup")), QStringLiteral("main"));
				QVERIFY(warningContains(doc, QStringLiteral("ignored")));
			}
		}

		void pluginLocalIncludesAreNotPromotedToWorldIncludes()
		{
			QMudTest::ScopedTempDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString pluginsPath       = QDir(tempDir.path()).filePath(QStringLiteral("plugins"));
			const QString worldPath         = QDir(tempDir.path()).filePath(QStringLiteral("main.xml"));
			const QString pluginPath        = QDir(pluginsPath).filePath(QStringLiteral("plugin.xml"));
			const QString pluginIncludePath = QDir(pluginsPath).filePath(QStringLiteral("constants.xml"));

			QVERIFY(QDir().mkpath(pluginsPath));
			QVERIFY(writeTextFile(worldPath, QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<qmud>
  <world id="aaaaaaaaaaaaaaaaaaaaaaaa" name="Main"/>
  <include name="plugin.xml" plugin="y"/>
</qmud>)")));
			QVERIFY(writeTextFile(pluginPath, QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin name="PluginA" id="bbbbbbbbbbbbbbbbbbbbbbbb" language="lua"/>
  <include name="constants.xml"/>
</muclient>)")));
			QVERIFY(writeTextFile(pluginIncludePath, QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<qmud>
  <variables>
    <variable name="from_constants">ok</variable>
  </variables>
</qmud>)")));

			WorldDocument doc;
			QVERIFY(doc.loadFromFile(worldPath));
			QVERIFY(doc.expandIncludes(worldPath, pluginsPath, tempDir.path(), QString()));

			QCOMPARE(doc.includes().size(), 1);
			QCOMPARE(doc.includes().front().attributes.value(QStringLiteral("name")),
			         QStringLiteral("plugin.xml"));
			QCOMPARE(doc.plugins().size(), 1);
			QCOMPARE(pluginVariableByName(doc.plugins().front(), QStringLiteral("from_constants")),
			         QStringLiteral("ok"));
		}

		void includeWithoutNameReturnsError()
		{
			QMudTest::ScopedTempDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString mainPath = QDir(tempDir.path()).filePath(QStringLiteral("main.xml"));
			QVERIFY(writeTextFile(mainPath, QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<qmud>
  <world id="aaaaaaaaaaaaaaaaaaaaaaaa" name="Main"/>
  <include plugin="y"/>
</qmud>)")));

			WorldDocument doc;
			QVERIFY(doc.loadFromFile(mainPath));
			QVERIFY(!doc.expandIncludes(mainPath, tempDir.path(), tempDir.path(), QString()));
			QCOMPARE(doc.errorString(), QStringLiteral("Name of include file not specified"));
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_WorldDocument_Includes)


#if __has_include("tst_WorldDocument_Includes.moc")
#include "tst_WorldDocument_Includes.moc"
#endif
