/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_RegexCompat.cpp
 * Role: QTest coverage for RegexCompat behavior.
 */

#include <QRegularExpression>
#include <QtTest/QTest>

/**
 * @brief QTest fixture covering RegexCompat scenarios.
 */
class tst_RegexCompat : public QObject
{
		Q_OBJECT

	// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void compatibilityVectors_data()
		{
			QTest::addColumn<QString>("id");
			QTest::addColumn<QString>("pattern");
			QTest::addColumn<QString>("subject");
			QTest::addColumn<bool>("ignoreCase");
			QTest::addColumn<bool>("matchEmpty");
			QTest::addColumn<bool>("expectValid");
			QTest::addColumn<bool>("expectEffectiveMatch");
			QTest::addColumn<QString>("expectCaptured0");
			QTest::addColumn<int>("expectErrorOffset");
			QTest::addColumn<QString>("expectNamedWord");

			QTest::newRow("1-basic-capture")
			    << QStringLiteral("1") << QStringLiteral("(foo)(bar)") << QStringLiteral("foobar") << false << true
			    << true << true << QStringLiteral("foobar") << -1 << QString();
			QTest::newRow("2-empty-match-allowed")
			    << QStringLiteral("2") << QStringLiteral("^") << QStringLiteral("abc") << false << true << true
			    << true << QString() << -1 << QString();
			QTest::newRow("3-empty-match-disallowed")
			    << QStringLiteral("3") << QStringLiteral("^") << QStringLiteral("abc") << false << false << true
			    << false << QString() << -1 << QString();
			QTest::newRow("4-ignore-case")
			    << QStringLiteral("4") << QStringLiteral("hello") << QStringLiteral("HeLLo") << true << true << true
			    << true << QStringLiteral("HeLLo") << -1 << QString();
			QTest::newRow("5-named-group")
			    << QStringLiteral("5") << QStringLiteral("(?<word>foo)bar") << QStringLiteral("foobar") << false
			    << true << true << true << QStringLiteral("foobar") << -1 << QStringLiteral("foo");
			QTest::newRow("6-duplicate-named-group-invalid")
			    << QStringLiteral("6") << QStringLiteral("(?<x>a)|(?<x>b)") << QStringLiteral("b") << false << true
			    << false << false << QString() << 13 << QString();
			QTest::newRow("9-recursion")
			    << QStringLiteral("9") << QStringLiteral("(a(?R)?b)") << QStringLiteral("aab") << false << true
			    << true << true << QStringLiteral("ab") << -1 << QString();
			QTest::newRow("10-callout")
			    << QStringLiteral("10") << QStringLiteral("a(?C)b") << QStringLiteral("ab") << false << true << true
			    << true << QStringLiteral("ab") << -1 << QString();
			QTest::newRow("11-backslash-C")
			    << QStringLiteral("11") << QStringLiteral("\\C\\C") << QStringLiteral("A") << false << true << true
			    << false << QString() << -1 << QString();
			QTest::newRow("12-backslash-R")
			    << QStringLiteral("12") << QStringLiteral("a\\Rb") << QStringLiteral("a\r\nb") << false << true
			    << true << true << QStringLiteral("a\r\nb") << -1 << QString();
			QTest::newRow("13-atomic-group")
			    << QStringLiteral("13") << QStringLiteral("(?>ab|a)b") << QStringLiteral("ab") << false << true
			    << true << false << QString() << -1 << QString();
			QTest::newRow("14-skip-verb")
			    << QStringLiteral("14") << QStringLiteral("a(*SKIP)b") << QStringLiteral("ab") << false << true
			    << true << true << QStringLiteral("ab") << -1 << QString();
			QTest::newRow("15-invalid-pattern")
			    << QStringLiteral("15") << QStringLiteral("(abc") << QStringLiteral("abc") << false << true << false
			    << false << QString() << 4 << QString();
		}

		void compatibilityVectors()
		{
			QFETCH(QString, id);
			QFETCH(QString, pattern);
			QFETCH(QString, subject);
			QFETCH(bool, ignoreCase);
			QFETCH(bool, matchEmpty);
			QFETCH(bool, expectValid);
			QFETCH(bool, expectEffectiveMatch);
			QFETCH(QString, expectCaptured0);
			QFETCH(int, expectErrorOffset);
			QFETCH(QString, expectNamedWord);

			QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;
			if (ignoreCase)
				options |= QRegularExpression::CaseInsensitiveOption;

			const QRegularExpression regex(pattern, options);
			QCOMPARE(regex.isValid(), expectValid);

			if (!expectValid)
			{
				if (expectErrorOffset >= 0)
					QCOMPARE(regex.patternErrorOffset(), expectErrorOffset);
				return;
			}

			const QRegularExpressionMatch match          = regex.match(subject);
			const bool                    effectiveMatch = match.hasMatch() && (matchEmpty || match.capturedLength(0) != 0);
			QVERIFY2(effectiveMatch == expectEffectiveMatch, qPrintable(id));

			if (effectiveMatch)
			{
				QCOMPARE(match.captured(0), expectCaptured0);
				if (!expectNamedWord.isEmpty())
					QCOMPARE(match.captured(QStringLiteral("word")), expectNamedWord);
			}
		}
	// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_RegexCompat)



#if __has_include("tst_RegexCompat.moc")
#include "tst_RegexCompat.moc"
#endif
