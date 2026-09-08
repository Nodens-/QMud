/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldEditUtils.cpp
 * Role: Unit coverage for shared alias, trigger, and timer editor behavior.
 */

#include "AppController.h"
#include "WorldOptions.h"
#include "helpers/WorldEditUtils.h"

#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QtTest/QTest>

namespace
{
	/**
	 * @brief QTest fixture covering world editor validation and transformation helpers.
	 */
	class tst_WorldEditUtils final : public QObject
	{
			Q_OBJECT

		private slots:
			static void populatesCompleteSendTargetList()
			{
				WorldEditUtils::populateSendToCombo(nullptr);

				QComboBox combo;
				combo.addItem(QStringLiteral("stale"), -1);
				WorldEditUtils::populateSendToCombo(&combo);

				QCOMPARE(combo.count(), 15);
				QCOMPARE(combo.itemText(0), QStringLiteral("World"));
				QCOMPARE(combo.itemData(0).toInt(), eSendToWorld);
				QCOMPARE(combo.itemText(combo.count() - 1), QStringLiteral("Script (after omit)"));
				QCOMPARE(combo.itemData(combo.count() - 1).toInt(), eSendToScriptAfterOmit);
				QCOMPARE(WorldEditUtils::sendToLabel(eSendToOutput), QStringLiteral("output"));
				QCOMPARE(WorldEditUtils::sendToLabel(eSendToScriptAfterOmit),
				         QStringLiteral("script_after_omit"));
				QCOMPARE(WorldEditUtils::sendToLabel(12345), QStringLiteral("12345"));
			}

			static void convertsWildcardTextToRegularExpression()
			{
				QCOMPARE(WorldEditUtils::convertToRegularExpression(QStringLiteral("say * (now)\n\x01")),
				         QStringLiteral("^say (.*?) \\(now\\)\\n\\x01$"));
				QCOMPARE(WorldEditUtils::convertToRegularExpression(QStringLiteral("café * 世界")),
				         QStringLiteral("^café (.*?) 世界$"));
				QString embeddedNull = QStringLiteral("before");
				embeddedNull += QChar::Null;
				embeddedNull += QStringLiteral("after");
				QCOMPARE(WorldEditUtils::convertToRegularExpression(embeddedNull),
				         QStringLiteral("^before\\x00after$"));
				QCOMPARE(WorldEditUtils::convertToRegularExpression(QStringLiteral("a*b"), false, false),
				         QStringLiteral("a\\*b"));
				QCOMPARE(WorldEditUtils::convertToRegularExpression(QStringLiteral("plain text")),
				         QStringLiteral("^plain text$"));
			}

			static void evaluatesSupportedSpeedwalkForms_data()
			{
				QTest::addColumn<QString>("input");
				QTest::addColumn<QString>("filler");
				QTest::addColumn<QString>("expected");

				QTest::newRow("directions-and-counts")
				    << QStringLiteral("2n 3e") << QString()
				    << QStringLiteral("north\r\nnorth\r\neast\r\neast\r\neast\r\n");
				QTest::newRow("action-codes")
				    << QStringLiteral("c n o e l w k s") << QString()
				    << QStringLiteral("close north\r\nopen east\r\nlock west\r\nunlock south\r\n");
				QTest::newRow("comments-parentheses-and-filler")
				    << QStringLiteral("{ignored} (go north/go south) 2f") << QStringLiteral("wait")
				    << QStringLiteral("go north\r\nwait\r\nwait\r\n");
				QTest::newRow("unicode-parenthesized-command")
				    << QStringLiteral("(gå 北/sør)") << QString() << QStringLiteral("gå 北\r\n");
			}

			static void evaluatesSupportedSpeedwalkForms()
			{
				QFETCH(QString, input);
				QFETCH(QString, filler);
				QFETCH(QString, expected);

				AppController app;
				QCOMPARE(WorldEditUtils::evaluateSpeedwalk(input, filler), expected);
			}

			static void reportsSpeedwalkErrors_data()
			{
				QTest::addColumn<QString>("input");
				QTest::addColumn<QString>("expected");

				QTest::newRow("unterminated-comment")
				    << QStringLiteral("{oops")
				    << QStringLiteral("*Comment code of '{' not terminated by a '}'");
				QTest::newRow("excessive-count")
				    << QStringLiteral("100n") << QStringLiteral("*Speed walk counter exceeds 99");
				QTest::newRow("missing-counted-action")
				    << QStringLiteral("2") << QStringLiteral("*Speed walk counter not followed by an action");
				QTest::newRow("counted-comment")
				    << QStringLiteral("2{x}")
				    << QStringLiteral("*Speed walk counter may not be followed by a comment");
				QTest::newRow("counted-action-code")
				    << QStringLiteral("2c n")
				    << QStringLiteral(
				           "*Action code of C, O, L or K must not follow a speed walk count (1-99)");
				QTest::newRow("action-without-direction")
				    << QStringLiteral("o")
				    << QStringLiteral("*Action code of C, O, L or K must be followed by a direction");
				QTest::newRow("action-followed-by-filler")
				    << QStringLiteral("lf")
				    << QStringLiteral("*Action code of C, O, L or K must be followed by a direction");
				QTest::newRow("action-followed-by-comment")
				    << QStringLiteral("k{x}")
				    << QStringLiteral("*Action code of C, O, L or K must be followed by a direction");
				QTest::newRow("unterminated-parentheses")
				    << QStringLiteral("(north")
				    << QStringLiteral("*Action code of '(' not terminated by a ')'");
				QTest::newRow("invalid-direction")
				    << QStringLiteral("q")
				    << QStringLiteral("*Invalid direction 'q' in speed walk, must be N, S, E, W, U, D, F, or "
				                      "(something)");
			}

			static void reportsSpeedwalkErrors()
			{
				QFETCH(QString, input);
				QFETCH(QString, expected);

				AppController app;
				QCOMPARE(WorldEditUtils::evaluateSpeedwalk(input, QStringLiteral("wait")), expected);
			}

			static void reversesSupportedSpeedwalkForms_data()
			{
				QTest::addColumn<QString>("input");
				QTest::addColumn<QString>("expected");

				QTest::newRow("directions-and-counts") << QStringLiteral("2n 3e") << QStringLiteral("3w 2s");
				QTest::newRow("action-codes")
				    << QStringLiteral("c n o e l w k s") << QStringLiteral("kn le ow cs");
				QTest::newRow("parenthesized-pair")
				    << QStringLiteral("(north/south) u") << QStringLiteral("d (south/north)");
				QTest::newRow("mapped-parenthesized-direction")
				    << QStringLiteral("(ne)") << QStringLiteral("(sw)");
				QTest::newRow("comments-and-newline")
				    << QStringLiteral("n\n{back}e") << QStringLiteral("w{back}\r\ns");
				QTest::newRow("unicode-parentheses-and-comment")
				    << QStringLiteral("n{Å北}(Gå/Sør)") << QStringLiteral("(sør/gå){Å北}s");
			}

			static void reversesSupportedSpeedwalkForms()
			{
				QFETCH(QString, input);
				QFETCH(QString, expected);

				AppController app;
				QCOMPARE(WorldEditUtils::reverseSpeedwalk(input), expected);
			}

			static void reportsReverseSpeedwalkErrors_data()
			{
				QTest::addColumn<QString>("input");
				QTest::addColumn<QString>("expected");

				QTest::newRow("unterminated-comment")
				    << QStringLiteral("{oops")
				    << QStringLiteral("*Comment code of '{' not terminated by a '}'");
				QTest::newRow("excessive-count")
				    << QStringLiteral("100n") << QStringLiteral("*Speed walk counter exceeds 99");
				QTest::newRow("missing-counted-action")
				    << QStringLiteral("2") << QStringLiteral("*Speed walk counter not followed by an action");
				QTest::newRow("counted-comment")
				    << QStringLiteral("2{x}")
				    << QStringLiteral("*Speed walk counter may not be followed by a comment");
				QTest::newRow("counted-action-code")
				    << QStringLiteral("2c n")
				    << QStringLiteral(
				           "*Action code of C, O, L or K must not follow a speed walk count (1-99)");
				QTest::newRow("action-without-direction")
				    << QStringLiteral("c")
				    << QStringLiteral("*Action code of C, O, L or K must be followed by a direction");
				QTest::newRow("unterminated-parentheses")
				    << QStringLiteral("(north")
				    << QStringLiteral("*Action code of '(' not terminated by a ')'");
				QTest::newRow("invalid-direction")
				    << QStringLiteral("q")
				    << QStringLiteral("*Invalid direction 'q' in speed walk, must be N, S, E, W, U, D, F, or "
				                      "(something)");
			}

			static void reportsReverseSpeedwalkErrors()
			{
				QFETCH(QString, input);
				QFETCH(QString, expected);

				AppController app;
				QCOMPARE(WorldEditUtils::reverseSpeedwalk(input), expected);
			}

			static void validatesLabelsAndCharacters()
			{
				QVERIFY(WorldEditUtils::checkLabelInvalid(QString(), false));
				QVERIFY(WorldEditUtils::checkLabelInvalid(QStringLiteral("1name"), false));
				QVERIFY(WorldEditUtils::checkLabelInvalid(QStringLiteral("a.b"), false));
				QVERIFY(WorldEditUtils::checkLabelInvalid(QStringLiteral("a-b"), true));
				QVERIFY(!WorldEditUtils::checkLabelInvalid(QStringLiteral("Alias_12"), false));
				QVERIFY(!WorldEditUtils::checkLabelInvalid(QStringLiteral("plugin.callback_2"), true));

				const QList<ushort> invalid = {static_cast<ushort>('\n'), static_cast<ushort>(';')};
				QCOMPARE(WorldEditUtils::findInvalidChar(QStringLiteral("valid"), invalid), -1);
				QCOMPARE(WorldEditUtils::findInvalidChar(QStringLiteral("ab;cd"), invalid), 2);
				QVERIFY(WorldEditUtils::checkRegularExpression(nullptr, QStringLiteral("^(north|south)$"),
				                                               QRegularExpression::CaseInsensitiveOption));
			}

			static void appliesEditorPreferences()
			{
				AppController  app;
				QLineEdit      lineEdit;
				QPlainTextEdit plainTextEdit;

				WorldEditUtils::applyEditorPreferences(static_cast<QLineEdit *>(nullptr));
				WorldEditUtils::applyEditorPreferences(static_cast<QPlainTextEdit *>(nullptr));

				app.setGlobalOptionInt(QStringLiteral("FixedFontForEditing"), 0);
				const QFont originalLineFont = lineEdit.font();
				WorldEditUtils::applyEditorPreferences(&lineEdit);
				QCOMPARE(lineEdit.font(), originalLineFont);

				app.setGlobalOptionString(QStringLiteral("FixedPitchFont"),
				                          QStringLiteral("DejaVu Sans Mono"));
				app.setGlobalOptionInt(QStringLiteral("FixedPitchFontSize"), 17);
				app.setGlobalOptionInt(QStringLiteral("FixedFontForEditing"), 1);
				WorldEditUtils::applyEditorPreferences(&lineEdit);
				QCOMPARE(lineEdit.font().pointSize(), 17);

				app.setGlobalOptionInt(QStringLiteral("TabInsertsTabInMultiLineDialogs"), 0);
				WorldEditUtils::applyEditorPreferences(&plainTextEdit);
				QVERIFY(plainTextEdit.tabChangesFocus());
				QCOMPARE(plainTextEdit.font().pointSize(), 17);

				app.setGlobalOptionInt(QStringLiteral("TabInsertsTabInMultiLineDialogs"), 1);
				WorldEditUtils::applyEditorPreferences(&plainTextEdit);
				QVERIFY(!plainTextEdit.tabChangesFocus());
			}
	};
} // namespace

QTEST_MAIN(tst_WorldEditUtils)

#if __has_include("tst_WorldEditUtils.moc")
#include "tst_WorldEditUtils.moc"
#endif
