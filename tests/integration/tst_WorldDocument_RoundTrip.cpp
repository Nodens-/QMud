/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldDocument_RoundTrip.cpp
 * Role: QTest coverage for WorldDocument RoundTrip behavior.
 */

#include "TestEnvironment.h"
#include "WorldDocument.h"
#include "WorldRuntime.h"

#include <QColor>
// ReSharper disable once CppUnusedIncludeDirective
#include <QCoreApplication>
#include <QDir>
// ReSharper disable once CppUnusedIncludeDirective
#include <QUuid>
#include <QtTest/QTest>

namespace
{
	QString fixturePath(const QString &relativePath)
	{
		return QDir(QStringLiteral(QMUD_TEST_SOURCE_DIR)).filePath(relativePath);
	}

	template <typename Entry> void compareNamedEntries(const QList<Entry> &left, const QList<Entry> &right)
	{
		QCOMPARE(left.size(), right.size());
		for (qsizetype i = 0; i < left.size(); ++i)
		{
			QCOMPARE(left[i].attributes, right[i].attributes);
			QCOMPARE(left[i].children, right[i].children);
		}
	}

	template <typename Entry> void compareContentEntries(const QList<Entry> &left, const QList<Entry> &right)
	{
		QCOMPARE(left.size(), right.size());
		for (qsizetype i = 0; i < left.size(); ++i)
		{
			QCOMPARE(left[i].attributes, right[i].attributes);
			QCOMPARE(left[i].content, right[i].content);
		}
	}

	template <typename Entry> void compareGroupedEntries(const QList<Entry> &left, const QList<Entry> &right)
	{
		QCOMPARE(left.size(), right.size());
		for (qsizetype i = 0; i < left.size(); ++i)
		{
			QCOMPARE(left[i].group, right[i].group);
			QCOMPARE(left[i].attributes, right[i].attributes);
		}
	}

	template <typename Entry>
	const Entry *entryByAttribute(const QList<Entry> &entries, const QString &name, const QString &value)
	{
		for (const Entry &entry : entries)
		{
			if (entry.attributes.value(name) == value)
				return &entry;
		}
		return nullptr;
	}

	template <typename Entry>
	const Entry *groupedEntryBySequence(const QList<Entry> &entries, const QString &group,
	                                    const QString &sequence)
	{
		for (const Entry &entry : entries)
		{
			if (entry.group == group && entry.attributes.value(QStringLiteral("seq")) == sequence)
				return &entry;
		}
		return nullptr;
	}

	/**
	 * @brief QTest fixture covering WorldDocument RoundTrip scenarios.
	 */
	class tst_WorldDocument_RoundTrip : public QObject
	{
			Q_OBJECT

		private slots:
			// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
			void initTestCase()
			{
				QMudTest::applyDeterministicTestEnvironment();
			}

			// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
			void representativeSectionsCanRoundTrip()
			{
				const QString sourcePath =
				    fixturePath(QStringLiteral("tests/data/worlds/world_sections.xml"));

				WorldDocument source;
				QVERIFY2(source.loadFromFile(sourcePath), qPrintable(source.errorString()));
				QVERIFY(source.warnings().isEmpty());

				const QString artifactDirectory =
				    QDir(QCoreApplication::applicationDirPath())
				        .filePath(QStringLiteral("test-artifacts/tst_WorldDocument_RoundTrip/%1")
				                      .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
				QVERIFY(QDir().mkpath(artifactDirectory));
				const QString roundTripPath =
				    QDir(artifactDirectory).filePath(QStringLiteral("roundtrip.xml"));

				WorldRuntime runtime;
				runtime.setStartupDirectory(QCoreApplication::applicationDirPath());
				runtime.setPluginInstallDeferred(true);
				runtime.applyFromDocument(source);
				QString saveError;
				QVERIFY2(runtime.saveWorldFile(roundTripPath, &saveError), qPrintable(saveError));

				WorldDocument loaded;
				QVERIFY2(loaded.loadFromFile(roundTripPath), qPrintable(loaded.errorString()));
				QVERIFY(loaded.warnings().isEmpty());

				QCOMPARE(loaded.worldAttributes().value(QStringLiteral("id")),
				         QStringLiteral("222222222222222222222222"));
				QCOMPARE(loaded.worldAttributes().value(QStringLiteral("name")),
				         QStringLiteral("Section World"));
				QCOMPARE(loaded.worldAttributes().value(QStringLiteral("site")),
				         QStringLiteral("example.org"));
				QCOMPARE(loaded.worldAttributes().value(QStringLiteral("port")), QStringLiteral("4001"));
				QCOMPARE(loaded.worldMultilineAttributes(), source.worldMultilineAttributes());

				const auto *trigger = entryByAttribute(loaded.triggers(), QStringLiteral("name"),
				                                       QStringLiteral("main_trigger"));
				QVERIFY(trigger);
				QCOMPARE(trigger->children.value(QStringLiteral("send")), QStringLiteral("main trigger"));
				const auto *alias =
				    entryByAttribute(loaded.aliases(), QStringLiteral("name"), QStringLiteral("main_alias"));
				QVERIFY(alias);
				QCOMPARE(alias->children.value(QStringLiteral("send")), QStringLiteral("main alias"));
				const auto *timer =
				    entryByAttribute(loaded.timers(), QStringLiteral("name"), QStringLiteral("main_timer"));
				QVERIFY(timer);
				QCOMPARE(timer->children.value(QStringLiteral("send")), QStringLiteral("main timer"));
				const auto *macro =
				    entryByAttribute(loaded.macros(), QStringLiteral("name"), QStringLiteral("up"));
				QVERIFY(macro);
				QCOMPARE(macro->children.value(QStringLiteral("send")), QStringLiteral("up"));
				const auto *variable =
				    entryByAttribute(loaded.variables(), QStringLiteral("name"), QStringLiteral("main_var"));
				QVERIFY(variable);
				QCOMPARE(variable->content, QStringLiteral("value"));
				const auto *keypad =
				    entryByAttribute(loaded.keypadEntries(), QStringLiteral("name"), QStringLiteral("1"));
				QVERIFY(keypad);
				QCOMPARE(keypad->content, QStringLiteral("north"));
				const auto *colour = groupedEntryBySequence(loaded.colours(), QStringLiteral("ansi/normal"),
				                                            QStringLiteral("1"));
				QVERIFY(colour);
				QCOMPARE(QColor(colour->attributes.value(QStringLiteral("rgb"))), QColor(Qt::black));
				const auto *printing = groupedEntryBySequence(
				    loaded.printingStyles(), QStringLiteral("ansi/normal"), QStringLiteral("1"));
				QVERIFY(printing);
				QCOMPARE(printing->attributes.value(QStringLiteral("bold")), QStringLiteral("y"));
				QCOMPARE(loaded.includes().size(), 1);
				QCOMPARE(
				    QDir::cleanPath(loaded.includes().constFirst().attributes.value(QStringLiteral("name"))),
				    QStringLiteral("include_child.xml"));
				QVERIFY(!loaded.scripts().isEmpty());
				QCOMPARE(loaded.scripts().constFirst().content, QStringLiteral("print(\"sections script\")"));

				WorldRuntime restoredRuntime;
				restoredRuntime.setStartupDirectory(QCoreApplication::applicationDirPath());
				restoredRuntime.setPluginInstallDeferred(true);
				restoredRuntime.applyFromDocument(loaded);
				const QString stabilizedPath =
				    QDir(artifactDirectory).filePath(QStringLiteral("stabilized.xml"));
				QVERIFY2(restoredRuntime.saveWorldFile(stabilizedPath, &saveError), qPrintable(saveError));
				WorldDocument stabilized;
				QVERIFY2(stabilized.loadFromFile(stabilizedPath), qPrintable(stabilized.errorString()));
				QVERIFY(stabilized.warnings().isEmpty());

				QMap<QString, QString> loadedWorldAttributes     = loaded.worldAttributes();
				QMap<QString, QString> stabilizedWorldAttributes = stabilized.worldAttributes();
				loadedWorldAttributes.remove(QStringLiteral("date_saved"));
				stabilizedWorldAttributes.remove(QStringLiteral("date_saved"));
				QCOMPARE(stabilizedWorldAttributes, loadedWorldAttributes);
				QCOMPARE(stabilized.worldMultilineAttributes(), loaded.worldMultilineAttributes());
				compareNamedEntries(stabilized.triggers(), loaded.triggers());
				compareNamedEntries(stabilized.aliases(), loaded.aliases());
				compareNamedEntries(stabilized.timers(), loaded.timers());
				compareNamedEntries(stabilized.macros(), loaded.macros());
				compareContentEntries(stabilized.variables(), loaded.variables());
				compareGroupedEntries(stabilized.colours(), loaded.colours());
				compareContentEntries(stabilized.keypadEntries(), loaded.keypadEntries());
				compareGroupedEntries(stabilized.printingStyles(), loaded.printingStyles());
				QCOMPARE(stabilized.includes().size(), loaded.includes().size());
				for (qsizetype i = 0; i < stabilized.includes().size(); ++i)
					QCOMPARE(stabilized.includes()[i].attributes, loaded.includes()[i].attributes);
				QCOMPARE(stabilized.scripts().size(), loaded.scripts().size());
				for (qsizetype i = 0; i < stabilized.scripts().size(); ++i)
					QCOMPARE(stabilized.scripts()[i].content, loaded.scripts()[i].content);
			}
	};
} // namespace

QTEST_GUILESS_MAIN(tst_WorldDocument_RoundTrip)

#if __has_include("tst_WorldDocument_RoundTrip.moc")
#include "tst_WorldDocument_RoundTrip.moc"
#endif
