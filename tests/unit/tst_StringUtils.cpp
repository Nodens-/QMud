/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_StringUtils.cpp
 * Role: QTest coverage for StringUtils behavior.
 */

#include "StringUtils.h"

#include <QtTest/QTest>

/**
 * @brief QTest fixture covering StringUtils scenarios.
 */
class tst_StringUtils : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void commandRoundTrip_data()
		{
			QTest::addColumn<QString>("command");
			QTest::newRow("copy") << QStringLiteral("Copy");
			QTest::newRow("paste") << QStringLiteral("Paste");
			QTest::newRow("about") << QStringLiteral("About");
			QTest::newRow("quick-connect") << QStringLiteral("QuickConnect");
		}

		void commandRoundTrip()
		{
			QFETCH(QString, command);
			const int id = qmudStringToCommandId(command);
			QVERIFY(id != 0);
			QCOMPARE(qmudCommandIdToString(id).toLower(), command.toLower());
		}

		void commandLookupIsCaseInsensitive()
		{
			QCOMPARE(qmudStringToCommandId(QStringLiteral("copy")),
			         qmudStringToCommandId(QStringLiteral("CoPy")));
			QCOMPARE(qmudStringToCommandId(QStringLiteral("no_such_command")), 0);
			QCOMPARE(qmudStringToCommandId(QString()), 0);
			QCOMPARE(qmudCommandIdToString(0), QString());
		}

		void escapeFixups()
		{
			const QString source   = QStringLiteral("line\\nnext\\t\\x41\\\\z\\?");
			const QString expected = QStringLiteral("line\nnext\tA\\z?");
			QCOMPARE(qmudFixupEscapeSequences(source), expected);
		}

		void replaceString()
		{
			const QString input = QStringLiteral("foo foo");
			QCOMPARE(qmudReplaceString(input, QStringLiteral("foo"), QStringLiteral("bar"), true),
			         QStringLiteral("bar bar"));
			QCOMPARE(qmudReplaceString(input, QStringLiteral("foo"), QStringLiteral("bar"), false),
			         QStringLiteral("bar foo"));
			QCOMPARE(qmudReplaceString(input, QString(), QStringLiteral("bar"), true), input);
		}

		void editDistance()
		{
			QCOMPARE(qmudEditDistance(QStringView{QStringLiteral("kitten")},
			                          QStringView{QStringLiteral("sitting")}),
			         3);
			QCOMPARE(
			    qmudEditDistance(QStringView{QStringLiteral("same")}, QStringView{QStringLiteral("same")}),
			    0);

			const QString longA(25, QLatin1Char('a'));
			const QString longB(25, QLatin1Char('b'));
			QCOMPARE(qmudEditDistance(QStringView{longA}, QStringView{longB}), 20);
		}

		void germanFixups()
		{
			QCOMPARE(qmudFixUpGerman(QStringLiteral("ÄÖÜäöüß")), QStringLiteral("AeOeUeaeoeuess"));
		}

		void stripAnsiEscapeCodes()
		{
			const QString input = QStringLiteral("HP:\x1b[0m\x1b[38;5;10m255\x1b[0m SP:\x1b[31m120\x1b[0m");
			QCOMPARE(qmudStripAnsiEscapeCodes(input), QStringLiteral("HP:255 SP:120"));
			QCOMPARE(qmudStripAnsiEscapeCodes(QStringLiteral("plain text")), QStringLiteral("plain text"));
		}

		void enabledFlag_data()
		{
			QTest::addColumn<QString>("value");
			QTest::addColumn<bool>("expected");

			QTest::newRow("1") << QStringLiteral("1") << true;
			QTest::newRow("yes") << QStringLiteral("yes") << true;
			QTest::newRow("Y") << QStringLiteral("Y") << true;
			QTest::newRow("true") << QStringLiteral("true") << true;
			QTest::newRow("no") << QStringLiteral("no") << false;
			QTest::newRow("0") << QStringLiteral("0") << false;
		}

		void enabledFlag()
		{
			QFETCH(QString, value);
			QFETCH(bool, expected);
			QCOMPARE(qmudIsEnabledFlag(value), expected);
		}

		void boolToYn()
		{
			QCOMPARE(qmudBoolToYn(true), QStringLiteral("y"));
			QCOMPARE(qmudBoolToYn(false), QStringLiteral("n"));
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_StringUtils)

#if __has_include("tst_StringUtils.moc")
#include "tst_StringUtils.moc"
#endif
