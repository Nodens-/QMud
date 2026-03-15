/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldDocument_Masks.cpp
 * Role: QTest coverage for WorldDocument Masks behavior.
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
} // namespace

/**
 * @brief QTest fixture covering WorldDocument Masks scenarios.
 */
class tst_WorldDocument_Masks : public QObject
{
		Q_OBJECT

	private slots:
		// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
		void initTestCase()
		{
			QMudTest::applyDeterministicTestEnvironment();
		}

		// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
		void sectionMaskFiltersCollections()
		{
			WorldDocument doc;
			doc.setLoadMask(WorldDocument::XML_TRIGGERS | WorldDocument::XML_VARIABLES |
			                WorldDocument::XML_INCLUDES);

			QVERIFY(doc.loadFromFile(fixturePath(QStringLiteral("tests/data/worlds/world_sections.xml"))));

			QVERIFY(doc.worldAttributes().isEmpty());
			QVERIFY(doc.worldMultilineAttributes().isEmpty());
			QCOMPARE(doc.triggers().size(), 1);
			QCOMPARE(doc.variables().size(), 1);
			QCOMPARE(doc.includes().size(), 1);
			QVERIFY(doc.aliases().isEmpty());
			QVERIFY(doc.timers().isEmpty());
			QVERIFY(doc.macros().isEmpty());
			QVERIFY(doc.colours().isEmpty());
			QVERIFY(doc.keypadEntries().isEmpty());
			QVERIFY(doc.printingStyles().isEmpty());
			QVERIFY(doc.plugins().isEmpty());
		}

		// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
		void importMainFileOnlySkipsIncludeExpansion()
		{
			const QString worldPath   = fixturePath(QStringLiteral("tests/data/worlds/world_with_plugin_include.xml"));
			const QString pluginsDir  = fixturePath(QStringLiteral("tests/data/plugins"));
			const QString programDir  = fixturePath(QStringLiteral("tests/data"));
			const QString stateDir    = fixturePath(QStringLiteral("tests/data/plugins/state"));

			WorldDocument  doc;
			doc.setLoadMask(WorldDocument::XML_TRIGGERS | WorldDocument::XML_INCLUDES |
			                WorldDocument::XML_PLUGINS | WorldDocument::XML_IMPORT_MAIN_FILE_ONLY);

			QVERIFY(doc.loadFromFile(worldPath));
			QCOMPARE(doc.triggers().size(), 1);
			QCOMPARE(doc.includes().size(), 2);
			QVERIFY(doc.plugins().isEmpty());

			QVERIFY(doc.expandIncludes(worldPath, pluginsDir, programDir, stateDir));
			QCOMPARE(doc.triggers().size(), 1);
			QVERIFY(doc.plugins().isEmpty());
			QVERIFY(doc.includeFileList().isEmpty());
		}

		// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
		void generalOnlyMaskLoadsOnlyWorldAttributes()
		{
			WorldDocument doc;
			doc.setLoadMask(WorldDocument::XML_GENERAL);

			QVERIFY(doc.loadFromFile(fixturePath(QStringLiteral("tests/data/worlds/world_sections.xml"))));

			QVERIFY(!doc.worldAttributes().isEmpty());
			QVERIFY(!doc.worldMultilineAttributes().isEmpty());
			QVERIFY(doc.triggers().isEmpty());
			QVERIFY(doc.aliases().isEmpty());
			QVERIFY(doc.timers().isEmpty());
			QVERIFY(doc.macros().isEmpty());
			QVERIFY(doc.variables().isEmpty());
			QVERIFY(doc.colours().isEmpty());
			QVERIFY(doc.keypadEntries().isEmpty());
			QVERIFY(doc.printingStyles().isEmpty());
			QVERIFY(doc.includes().isEmpty());
			QVERIFY(doc.plugins().isEmpty());
		}
};

QTEST_APPLESS_MAIN(tst_WorldDocument_Masks)

#include "tst_WorldDocument_Masks.moc"
