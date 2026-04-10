/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldRuleEnableUtils.cpp
 * Role: Unit coverage for world-vs-plugin rule enable gating helpers.
 */

#include "WorldRuleEnableUtils.h"

#include <QtTest/QTest>

/**
 * @brief QTest fixture covering world and plugin rule gating semantics.
 */
class tst_WorldRuleEnableUtils : public QObject
{
		Q_OBJECT

	private slots:
		/**
		 * @brief Verifies world enable keys map to expected world attribute names.
		 */
		static void worldRuleEnableKeyMatchesExpectedAttribute()
		{
			QCOMPARE(worldRuleEnableKey(WorldRuleKind::Alias), QStringLiteral("enable_aliases"));
			QCOMPARE(worldRuleEnableKey(WorldRuleKind::Trigger), QStringLiteral("enable_triggers"));
			QCOMPARE(worldRuleEnableKey(WorldRuleKind::Timer), QStringLiteral("enable_timers"));
		}

		/**
		 * @brief Verifies world rule collections follow corresponding world enable flags.
		 */
		static void worldRuleCollectionFollowsWorldEnableFlags()
		{
			QMap<QString, QString> attrs;
			attrs.insert(QStringLiteral("enable_aliases"), QStringLiteral("0"));
			attrs.insert(QStringLiteral("enable_triggers"), QStringLiteral("n"));
			attrs.insert(QStringLiteral("enable_timers"), QStringLiteral("false"));

			QVERIFY(!shouldEvaluateRuleCollection(attrs, WorldRuleKind::Alias, false));
			QVERIFY(!shouldEvaluateRuleCollection(attrs, WorldRuleKind::Trigger, false));
			QVERIFY(!shouldEvaluateRuleCollection(attrs, WorldRuleKind::Timer, false));

			attrs.insert(QStringLiteral("enable_aliases"), QStringLiteral("1"));
			attrs.insert(QStringLiteral("enable_triggers"), QStringLiteral("Y"));
			attrs.insert(QStringLiteral("enable_timers"), QStringLiteral("TRUE"));

			QVERIFY(shouldEvaluateRuleCollection(attrs, WorldRuleKind::Alias, false));
			QVERIFY(shouldEvaluateRuleCollection(attrs, WorldRuleKind::Trigger, false));
			QVERIFY(shouldEvaluateRuleCollection(attrs, WorldRuleKind::Timer, false));
		}

		/**
		 * @brief Verifies plugin rule collections ignore world enable flags.
		 */
		static void pluginRuleCollectionAlwaysEnabledRegardlessOfWorldFlags()
		{
			QMap<QString, QString> attrs;
			attrs.insert(QStringLiteral("enable_aliases"), QStringLiteral("0"));
			attrs.insert(QStringLiteral("enable_triggers"), QStringLiteral("0"));
			attrs.insert(QStringLiteral("enable_timers"), QStringLiteral("0"));

			QVERIFY(shouldEvaluateRuleCollection(attrs, WorldRuleKind::Alias, true));
			QVERIFY(shouldEvaluateRuleCollection(attrs, WorldRuleKind::Trigger, true));
			QVERIFY(shouldEvaluateRuleCollection(attrs, WorldRuleKind::Timer, true));
		}
};

QTEST_APPLESS_MAIN(tst_WorldRuleEnableUtils)

#if __has_include("tst_WorldRuleEnableUtils.moc")
#include "tst_WorldRuleEnableUtils.moc"
#endif
