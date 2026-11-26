/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_StringUtils.cpp
 * Role: QTest coverage for StringUtils behavior.
 */

#include "StringUtils.h"

#include <QtTest/QTest>

namespace
{
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

			void splitLegacyMenuItemsTrimsWhitespaceAndPreservesSeparators()
			{
				const QStringList parts = qmudSplitLegacyMenuItems(
				    QStringLiteral(">Channels | +tell | shout | < | - | New Tab | ^Delete Tab | "));

				QCOMPARE(parts.size(), 8);
				QCOMPARE(parts.at(0), QStringLiteral(">Channels"));
				QCOMPARE(parts.at(1), QStringLiteral("+tell"));
				QCOMPARE(parts.at(2), QStringLiteral("shout"));
				QCOMPARE(parts.at(3), QStringLiteral("<"));
				QCOMPARE(parts.at(4), QStringLiteral("-"));
				QCOMPARE(parts.at(5), QStringLiteral("New Tab"));
				QCOMPARE(parts.at(6), QStringLiteral("^Delete Tab"));
				QCOMPARE(parts.at(7), QString());
			}

			void splitLegacyMenuItemsKeepsTrailingStateMarkersLiteral()
			{
				const QStringList parts = qmudSplitLegacyMenuItems(
				    QStringLiteral("Rename | New Tab^ | Delete Tab | Notify+ | Configure"));

				QCOMPARE(parts.size(), 5);
				QCOMPARE(parts.at(0), QStringLiteral("Rename"));
				QCOMPARE(parts.at(1), QStringLiteral("New Tab^"));
				QCOMPARE(parts.at(2), QStringLiteral("Delete Tab"));
				QCOMPARE(parts.at(3), QStringLiteral("Notify+"));
				QCOMPARE(parts.at(4), QStringLiteral("Configure"));
			}

			void splitLegacyMenuItemsKeepsPlusPlusLabels()
			{
				const QStringList parts = qmudSplitLegacyMenuItems(QStringLiteral("C++ | Other"));

				QCOMPARE(parts.size(), 2);
				QCOMPARE(parts.at(0), QStringLiteral("C++"));
				QCOMPARE(parts.at(1), QStringLiteral("Other"));
			}

			void editDistance()
			{
				QCOMPARE(qmudEditDistance(QStringView{QStringLiteral("kitten")},
				                          QStringView{QStringLiteral("sitting")}),
				         3);
				QCOMPARE(qmudEditDistance(QStringView{QStringLiteral("same")},
				                          QStringView{QStringLiteral("same")}),
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
				const QString input =
				    QStringLiteral("HP:\x1b[0m\x1b[38;5;10m255\x1b[0m SP:\x1b[31m120\x1b[0m");
				QCOMPARE(qmudStripAnsiEscapeCodes(input), QStringLiteral("HP:255 SP:120"));
				QCOMPARE(qmudStripAnsiEscapeCodes(QStringLiteral("plain text")),
				         QStringLiteral("plain text"));
			}

			/**
			 * @brief Supplies terminal formatting, metadata, and ordinary text for plain-text copying.
			 */
			static void stripTerminalControlCodes_data()
			{
				QTest::addColumn<QString>("input");
				QTest::addColumn<QString>("expected");
				QTest::newRow("plain")
				    << QStringLiteral(" αβ\tline\r\nnext ") << QStringLiteral(" αβ\tline\r\nnext ");
				QTest::newRow("sgr") << QStringLiteral("\x1b[1;31mred\x1b[0m\x1b[38;5;42mgreen\x1b[0m")
				                     << QStringLiteral("redgreen");
				QTest::newRow("rgb") << QStringLiteral("\x1b[38;2;1;2;3mone\x1b[48:2::4:5:6mtwo\x1b[m")
				                     << QStringLiteral("onetwo");
				QTest::newRow("csi") << QStringLiteral("\x1b[2J\x1b[?25ltext\x1b[?25h")
				                     << QStringLiteral("text");
				QTest::newRow("osc")
				    << QStringLiteral(
				           "\x1b]0;title\x07\x1b]8;;https://example.invalid\x1b\\link\x1b]8;;\x1b\\")
				    << QStringLiteral("link");
				QTest::newRow("c1")
				    << QStringLiteral(
				           "\u009b31mred\u009b0m\u009d8;;https://example.invalid\u009clink\u009d8;;\u009c")
				    << QStringLiteral("redlink");
				QTest::newRow("control-strings")
				    << QStringLiteral("\x1bPmetadata\x1b\\\x1b_hidden\x1b\\text") << QStringLiteral("text");
				QTest::newRow("other-controls") << QStringLiteral("\x1b(Btext\x07\x1b"
				                                                  "7\n\tnext")
				                                << QStringLiteral("text\n\tnext");
				QTest::newRow("unfinished-csi") << QStringLiteral("text\x1b[38;2;") << QStringLiteral("text");
				QTest::newRow("unfinished-osc")
				    << QStringLiteral("text\x1b]0;title") << QStringLiteral("text");
				QTest::newRow("formatting-only") << QStringLiteral("\x1b[0m") << QString{};
			}

			/**
			 * @brief Removes terminal controls without altering the remaining text or whitespace.
			 */
			static void stripTerminalControlCodes()
			{
				QFETCH(QString, input);
				QFETCH(QString, expected);
				QCOMPARE(qmudStripTerminalControlCodes(input), expected);
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

			void booleanKeyword_data()
			{
				QTest::addColumn<QString>("value");
				QTest::addColumn<bool>("expectedResult");

				QTest::newRow("y") << QStringLiteral("y") << true;
				QTest::newRow("yes") << QStringLiteral("YES") << true;
				QTest::newRow("true") << QStringLiteral("True") << true;
				QTest::newRow("n") << QStringLiteral("n") << false;
				QTest::newRow("no") << QStringLiteral("NO") << false;
				QTest::newRow("false") << QStringLiteral("False") << false;
			}

			void booleanKeyword()
			{
				QFETCH(QString, value);
				QFETCH(bool, expectedResult);
				bool result = !expectedResult;
				QVERIFY(qmudParseBooleanKeyword(value, result));
				QCOMPARE(result, expectedResult);
			}

			void unrecognizedBooleanKeywordLeavesResultUnchanged()
			{
				bool result = true;
				QVERIFY(!qmudParseBooleanKeyword(QStringLiteral("invalid"), result));
				QVERIFY(result);
			}

			void boolToYn()
			{
				QCOMPARE(qmudBoolToYn(true), QStringLiteral("y"));
				QCOMPARE(qmudBoolToYn(false), QStringLiteral("n"));
			}
			// NOLINTEND(readability-convert-member-functions-to-static)
	};
} // namespace

QTEST_APPLESS_MAIN(tst_StringUtils)

#include "tst_StringUtils.moc"
