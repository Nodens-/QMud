/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldDocument_Load.cpp
 * Role: QTest coverage for WorldDocument Load behavior.
 */

#include "TestEnvironment.h"
#include "WorldDocument.h"

#include <QDir>
#include <QtTest/QTest>

namespace
{
	QString fixturePath(const QString &relativePath)
	{
		return QDir(QStringLiteral(QMUD_TEST_SOURCE_DIR)).filePath(relativePath);
	}

	bool containsMessage(const QStringList &messages, const QString &needle)
	{
		for (const QString &message : messages)
		{
			if (message.contains(needle))
				return true;
		}
		return false;
	}
} // namespace

/**
 * @brief QTest fixture covering WorldDocument Load scenarios.
 */
class tst_WorldDocument_Load : public QObject
{
		Q_OBJECT

	// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void initTestCase()
		{
			QMudTest::applyDeterministicTestEnvironment();
		}

		void loadWorldFixtureParsesCoreFields()
		{
			WorldDocument doc;
			QVERIFY(doc.loadFromFile(fixturePath(QStringLiteral("tests/data/worlds/minimal_world.xml"))));

			QCOMPARE(doc.worldFileVersion(), 1000);
			QCOMPARE(doc.qmudVersion(), QStringLiteral("10.01"));
			QCOMPARE(doc.dateSaved().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
			         QStringLiteral("2026-03-15 12:00:00"));
			QCOMPARE(doc.worldAttributes().value(QStringLiteral("name")), QStringLiteral("Minimal World"));
			QCOMPARE(doc.worldMultilineAttributes().value(QStringLiteral("notes")),
			         QStringLiteral("hello world notes"));
			QCOMPARE(doc.triggers().size(), 1);
			QCOMPARE(doc.aliases().size(), 1);
			QCOMPARE(doc.timers().size(), 1);
			QCOMPARE(doc.variables().size(), 1);
			QCOMPARE(doc.includes().size(), 1);
			QCOMPARE(doc.scripts().size(), 1);
			QCOMPARE(doc.scripts().front().content, QStringLiteral("print(\"root script\")"));
			QVERIFY(doc.errorString().isEmpty());
			QVERIFY(doc.warnings().isEmpty());
		}

		void loadPluginFixtureFinalizesPluginSections()
		{
			WorldDocument doc;
			QVERIFY(doc.loadFromPluginFile(fixturePath(QStringLiteral("tests/data/plugins/minimal_plugin.xml"))));

			QCOMPARE(doc.plugins().size(), 1);
			QVERIFY(doc.triggers().isEmpty());
			QVERIFY(doc.variables().isEmpty());

			const WorldDocument::Plugin plugin = doc.plugins().front();
			QCOMPARE(plugin.attributes.value(QStringLiteral("name")), QStringLiteral("FixturePlugin"));
			QCOMPARE(plugin.attributes.value(QStringLiteral("id")), QStringLiteral("0123456789abcdef01234567"));
			QCOMPARE(plugin.script.trimmed(), QStringLiteral("return \"ok\""));
			QCOMPARE(plugin.triggers.size(), 1);
			QCOMPARE(plugin.triggers.front().attributes.value(QStringLiteral("name")),
			         QStringLiteral("plugin_trigger"));
			QCOMPARE(plugin.variables.size(), 1);
			QCOMPARE(plugin.variables.front().attributes.value(QStringLiteral("name")),
			         QStringLiteral("plugin_var"));
			QCOMPARE(plugin.variables.front().content, QStringLiteral("plugin value"));
		}

		void pluginPolicyThroughPublicLoadApis()
		{
			WorldDocument allowDoc;
			QVERIFY(allowDoc.loadFromFile(fixturePath(QStringLiteral("tests/data/worlds/world_with_plugin.xml"))));
			QCOMPARE(allowDoc.plugins().size(), 1);

			WorldDocument noPluginsDoc;
			noPluginsDoc.setLoadMask(WorldDocument::kDefaultLoadMask | WorldDocument::XML_NO_PLUGINS);
			QVERIFY(!noPluginsDoc.loadFromFile(
			    fixturePath(QStringLiteral("tests/data/worlds/world_with_plugin.xml"))));
			QCOMPARE(noPluginsDoc.errorString(),
			         QStringLiteral("Plugin not expected here. Use File -> Plugins to load plugins"));

			WorldDocument requirePluginDoc;
			QVERIFY(!requirePluginDoc.loadFromPluginFile(
			    fixturePath(QStringLiteral("tests/data/worlds/minimal_world.xml"))));
			QCOMPARE(requirePluginDoc.errorString(), QStringLiteral("No plugin found"));
		}

		void errorAndWarningPropagation()
		{
			WorldDocument malformedDoc;
			QVERIFY(!malformedDoc.loadFromFile(
			    fixturePath(QStringLiteral("tests/data/worlds/malformed_world.xml"))));
			QVERIFY(!malformedDoc.errorString().isEmpty());

			WorldDocument warningDoc;
			QVERIFY(warningDoc.loadFromPluginFile(
			    fixturePath(QStringLiteral("tests/data/plugins/plugin_with_warning.xml"))));
			QVERIFY(containsMessage(warningDoc.warnings(),
			                        QStringLiteral("<macros> tag cannot be used inside a plugin")));
		}
	// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_WorldDocument_Load)


#if __has_include("tst_WorldDocument_Load.moc")
#include "tst_WorldDocument_Load.moc"
#endif
