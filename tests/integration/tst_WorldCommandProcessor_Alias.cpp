/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldCommandProcessor_Alias.cpp
 * Role: QTest coverage for WorldCommandProcessor Alias behavior.
 */

#include "AliasMatchUtils.h"
#include "WorldCommandProcessorUtils.h"

#include <QRegularExpression>
#include <QtTest/QTest>

/**
 * @brief QTest fixture covering WorldCommandProcessor Alias scenarios.
 */
class tst_WorldCommandProcessor_Alias : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void wildcardAliasCapturesWildcards()
		{
			const QString pattern =
			    QMudCommandPattern::convertToRegularExpression(QStringLiteral("buy * from *"), true, true);
			const QRegularExpression regex(pattern);
			QVERIFY(regex.isValid());

			const QMudAliasMatch::MatchResult result =
			    QMudAliasMatch::matchWithCaptures(regex, QStringLiteral("buy sword from merchant"), true);
			QVERIFY(result.matched);
			QCOMPARE(result.wildcards.size(), 3);
			QCOMPARE(result.wildcards.at(1), QStringLiteral("sword"));
			QCOMPARE(result.wildcards.at(2), QStringLiteral("merchant"));
		}

		void regexpAliasCapturesNamedWildcard()
		{
			const QRegularExpression regex(QStringLiteral("^give (?<what>\\w+) to (?<who>\\w+)$"));
			QVERIFY(regex.isValid());

			const QMudAliasMatch::MatchResult result =
			    QMudAliasMatch::matchWithCaptures(regex, QStringLiteral("give coin to guard"), true);
			QVERIFY(result.matched);
			QCOMPARE(result.namedWildcards.value(QStringLiteral("what")), QStringLiteral("coin"));
			QCOMPARE(result.namedWildcards.value(QStringLiteral("who")), QStringLiteral("guard"));
		}

		void ignoreCaseOptionControlsAliasMatch()
		{
			const QString pattern =
			    QMudCommandPattern::convertToRegularExpression(QStringLiteral("Cast *"), true, true);

			const QRegularExpression sensitiveRegex(pattern);
			QVERIFY(!QMudAliasMatch::matchWithCaptures(sensitiveRegex, QStringLiteral("cast fire"), true)
			             .matched);

			const QRegularExpression insensitiveRegex(pattern, QRegularExpression::CaseInsensitiveOption);
			const QMudAliasMatch::MatchResult insensitiveResult =
			    QMudAliasMatch::matchWithCaptures(insensitiveRegex, QStringLiteral("cast fire"), true);
			QVERIFY(insensitiveResult.matched);
			QCOMPARE(insensitiveResult.wildcards.at(1), QStringLiteral("fire"));
		}

		void emptyRegexpMatchCanBeRejected()
		{
			const QRegularExpression regex(QStringLiteral("^.*$"));
			QVERIFY(regex.isValid());

			QVERIFY(!QMudAliasMatch::matchWithCaptures(regex, QString(), false).matched);
			QVERIFY(QMudAliasMatch::matchWithCaptures(regex, QString(), true).matched);
		}

		void recursionGuardRespectsMaxDepth()
		{
			QVERIFY(!QMudAliasMatch::exceedsExecutionDepth(0, 20));
			QVERIFY(!QMudAliasMatch::exceedsExecutionDepth(19, 20));
			QVERIFY(QMudAliasMatch::exceedsExecutionDepth(20, 20));
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_WorldCommandProcessor_Alias)

#if __has_include("tst_WorldCommandProcessor_Alias.moc")
#include "tst_WorldCommandProcessor_Alias.moc"
#endif
