/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_ReloadPlannerUtils.cpp
 * Role: QTest coverage for reload world policy planning and MCCP fallback decisions.
 */

#include "ReloadPlannerUtils.h"

#include <QtTest/QTest>

/**
 * @brief QTest fixture covering reload planner policy selection helpers.
 */
class tst_ReloadPlannerUtils : public QObject
{
		Q_OBJECT

	// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void shouldAttemptMccpDisableRequiresConnectedDescriptorAndActiveMccp()
		{
			QVERIFY(shouldAttemptReloadMccpDisable(true, 10, true));
			QVERIFY(!shouldAttemptReloadMccpDisable(false, 10, true));
			QVERIFY(!shouldAttemptReloadMccpDisable(true, -1, true));
			QVERIFY(!shouldAttemptReloadMccpDisable(true, 10, false));
		}

		void disconnectedWorldIsNotRecovered()
		{
			const ReloadWorldPolicyDecision decision =
			    computeReloadWorldPolicy({false, false, -1, false, false});
			QVERIFY(!decision.connectedAtReload);
			QVERIFY(!decision.shouldAttemptDescriptorInheritance);
			QCOMPARE(decision.policy, ReloadSocketPolicy::Reattach);
		}

		void connectedWorldWithoutMccpUsesReattach()
		{
			const ReloadWorldPolicyDecision decision =
			    computeReloadWorldPolicy({true, false, 42, false, false});
			QVERIFY(decision.connectedAtReload);
			QVERIFY(decision.shouldAttemptDescriptorInheritance);
			QCOMPARE(decision.policy, ReloadSocketPolicy::Reattach);
		}

		void connectedWorldWithMccpTimeoutFallsBackToReconnect()
		{
			const ReloadWorldPolicyDecision decision =
			    computeReloadWorldPolicy({true, false, 42, true, false});
			QVERIFY(decision.connectedAtReload);
			QVERIFY(decision.shouldAttemptDescriptorInheritance);
			QCOMPARE(decision.policy, ReloadSocketPolicy::ParkReconnect);
		}

		void connectedWorldWithMccpDisableSuccessReattaches()
		{
			const ReloadWorldPolicyDecision decision =
			    computeReloadWorldPolicy({true, false, 42, true, true});
			QVERIFY(decision.connectedAtReload);
			QVERIFY(decision.shouldAttemptDescriptorInheritance);
			QCOMPARE(decision.policy, ReloadSocketPolicy::Reattach);
		}

		void connectedWorldWithoutDescriptorFallsBackToReconnect()
		{
			const ReloadWorldPolicyDecision decision =
			    computeReloadWorldPolicy({true, false, -1, false, false});
			QVERIFY(decision.connectedAtReload);
			QVERIFY(!decision.shouldAttemptDescriptorInheritance);
			QCOMPARE(decision.policy, ReloadSocketPolicy::ParkReconnect);
		}

		void connectingWorldUsesReconnectPolicy()
		{
			const ReloadWorldPolicyDecision decision =
			    computeReloadWorldPolicy({false, true, 7, false, false});
			QVERIFY(decision.connectedAtReload);
			QVERIFY(decision.shouldAttemptDescriptorInheritance);
			QCOMPARE(decision.policy, ReloadSocketPolicy::ParkReconnect);
		}
	// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_ReloadPlannerUtils)


#if __has_include("tst_ReloadPlannerUtils.moc")
#include "tst_ReloadPlannerUtils.moc"
#endif
