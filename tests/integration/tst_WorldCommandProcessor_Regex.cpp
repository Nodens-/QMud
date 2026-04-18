/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldCommandProcessor_Regex.cpp
 * Role: QTest coverage for WorldCommandProcessor Regex behavior.
 */

#include "WorldCommandProcessorUtils.h"

#include <QtTest/QTest>

/**
 * @brief QTest fixture covering WorldCommandProcessor Regex scenarios.
 */
class tst_WorldCommandProcessor_Regex : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void wildcardConvertsToNonGreedyCapture()
		{
			const QString pattern =
			    QMudCommandPattern::convertToRegularExpression(QStringLiteral("look * at *"), true, true);
			const QRegularExpression regex(pattern);
			QVERIFY(regex.isValid());

			const QRegularExpressionMatch match = regex.match(QStringLiteral("look red dragon at horizon"));
			QVERIFY(match.hasMatch());
			QCOMPARE(match.captured(1), QStringLiteral("red dragon"));
			QCOMPARE(match.captured(2), QStringLiteral("horizon"));
		}

		void wholeLineAnchoringCanBeDisabled()
		{
			const QString anchored =
			    QMudCommandPattern::convertToRegularExpression(QStringLiteral("abc"), true, true);
			const QString partial =
			    QMudCommandPattern::convertToRegularExpression(QStringLiteral("abc"), false, true);

			QVERIFY(QRegularExpression(anchored).match(QStringLiteral("xabcx")).hasMatch() == false);
			QVERIFY(QRegularExpression(partial).match(QStringLiteral("xabcx")).hasMatch());
		}

		void regexMetaCharactersAreEscaped()
		{
			const QString pattern =
			    QMudCommandPattern::convertToRegularExpression(QStringLiteral("a+b(c).?"), true, true);
			const QRegularExpression regex(pattern);
			QVERIFY(regex.isValid());
			QVERIFY(regex.match(QStringLiteral("a+b(c).?")).hasMatch());
			QVERIFY(!regex.match(QStringLiteral("aaabcc")).hasMatch());
		}

		void controlCharactersAreHexEscaped()
		{
			const QString source  = QStringLiteral("left") + QChar(0x01) + QStringLiteral("right");
			const QString pattern = QMudCommandPattern::convertToRegularExpression(source, true, true);
			QVERIFY(pattern.contains(QStringLiteral("\\x01")));

			const QRegularExpression regex(pattern);
			QVERIFY(regex.isValid());
			QVERIFY(regex.match(source).hasMatch());
		}

		void asteriskCanRemainLiteralWhenDisabled()
		{
			const QString pattern =
			    QMudCommandPattern::convertToRegularExpression(QStringLiteral("value *"), true, false);
			const QRegularExpression regex(pattern);
			QVERIFY(regex.isValid());
			QVERIFY(regex.match(QStringLiteral("value *")).hasMatch());
			QVERIFY(!regex.match(QStringLiteral("value anything")).hasMatch());
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_WorldCommandProcessor_Regex)

#if __has_include("tst_WorldCommandProcessor_Regex.moc")
#include "tst_WorldCommandProcessor_Regex.moc"
#endif
