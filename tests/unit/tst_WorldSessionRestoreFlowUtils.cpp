/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldSessionRestoreFlowUtils.cpp
 * Role: QTest coverage for post-session-restore startup sequencing helpers.
 */

#include "WorldSessionRestoreFlowUtils.h"

#include <QtTest/QTest>

/**
 * @brief QTest fixture covering post-restore sequencing behavior.
 */
class tst_WorldSessionRestoreFlowUtils : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void loadPlanDefaultsOnWithoutStateFileSkipsRead()
		{
			const auto plan = QMudWorldSessionRestoreFlow::computeSessionStateLoadPlan(true, true, false);
			QCOMPARE(plan, QMudWorldSessionRestoreFlow::SessionStateLoadPlan::SkipApplyAndSucceed);
		}

		void loadPlanDefaultsOnWithStateFileReads()
		{
			const auto plan = QMudWorldSessionRestoreFlow::computeSessionStateLoadPlan(true, true, true);
			QCOMPARE(plan, QMudWorldSessionRestoreFlow::SessionStateLoadPlan::ReadFileAndApply);
		}

		void loadPlanBothDisabledRemovesFile()
		{
			const auto plan = QMudWorldSessionRestoreFlow::computeSessionStateLoadPlan(false, false, true);
			QCOMPARE(plan, QMudWorldSessionRestoreFlow::SessionStateLoadPlan::RemoveFileAndSucceed);
		}

		void scrollbackStatusTrackingRequiresOutputBufferPersistence()
		{
			const bool track = QMudWorldSessionRestoreFlow::shouldTrackScrollbackRestoreStatus(
			    true, QMudWorldSessionRestoreFlow::SessionStateLoadPlan::ReadFileAndApply);
			QVERIFY(track);
		}

		void scrollbackStatusTrackingSkipsHistoryOnlyReadPlan()
		{
			const bool track = QMudWorldSessionRestoreFlow::shouldTrackScrollbackRestoreStatus(
			    false, QMudWorldSessionRestoreFlow::SessionStateLoadPlan::ReadFileAndApply);
			QVERIFY(!track);
		}

		void scrollbackStatusTrackingIncludesSkipApplyPlanWhenOutputBufferIsEnabled()
		{
			const bool skipPlan = QMudWorldSessionRestoreFlow::shouldTrackScrollbackRestoreStatus(
			    true, QMudWorldSessionRestoreFlow::SessionStateLoadPlan::SkipApplyAndSucceed);
			QVERIFY(skipPlan);
		}

		void scrollbackStatusTrackingSkipsRemovePlan()
		{
			const bool removePlan = QMudWorldSessionRestoreFlow::shouldTrackScrollbackRestoreStatus(
			    false, QMudWorldSessionRestoreFlow::SessionStateLoadPlan::RemoveFileAndSucceed);
			QVERIFY(!removePlan);
		}

		void deferredUpgradeWelcomeRequiresDeferFlag()
		{
			const bool show = QMudWorldSessionRestoreFlow::shouldShowDeferredUpgradeWelcome(false, true, 0);
			QVERIFY(!show);
		}

		void deferredUpgradeWelcomeRequiresDispatchComplete()
		{
			const bool show = QMudWorldSessionRestoreFlow::shouldShowDeferredUpgradeWelcome(true, false, 0);
			QVERIFY(!show);
		}

		void deferredUpgradeWelcomeRequiresNoInFlightRestores()
		{
			const bool show = QMudWorldSessionRestoreFlow::shouldShowDeferredUpgradeWelcome(true, true, 2);
			QVERIFY(!show);
		}

		void deferredUpgradeWelcomeAllowsWhenReady()
		{
			const bool show = QMudWorldSessionRestoreFlow::shouldShowDeferredUpgradeWelcome(true, true, 0);
			QVERIFY(show);
		}

		void restoreStatusMessageAlwaysShowsRemainingCount()
		{
			const QString one     = QMudWorldSessionRestoreFlow::restoreScrollbackStatusMessage(1);
			const QString many    = QMudWorldSessionRestoreFlow::restoreScrollbackStatusMessage(3);
			const QString clamped = QMudWorldSessionRestoreFlow::restoreScrollbackStatusMessage(-4);
			QCOMPARE(one, QStringLiteral("Restoring scrollback buffers (1 remaining)"));
			QCOMPARE(many, QStringLiteral("Restoring scrollback buffers (3 remaining)"));
			QCOMPARE(clamped, QStringLiteral("Restoring scrollback buffers (0 remaining)"));
		}

		void runsStartupThenAutoConnectOnSuccess()
		{
			QStringList sequence;
			QMudWorldSessionRestoreFlow::runPostRestoreFlow(
			    true, QStringLiteral("ignored"),
			    {
			        [&sequence] { sequence.push_back(QStringLiteral("startup")); },
			        [&sequence] { sequence.push_back(QStringLiteral("autoconnect")); },
			        [&sequence](const QString &error)
			        { sequence.push_back(QStringLiteral("error:%1").arg(error)); },
			    });

			const QStringList expected{
			    QStringLiteral("startup"),
			    QStringLiteral("autoconnect"),
			};
			QCOMPARE(sequence, expected);
		}

		void reportsErrorAndStillRunsStartupAndAutoConnect()
		{
			QStringList sequence;
			QMudWorldSessionRestoreFlow::runPostRestoreFlow(
			    false, QStringLiteral("restore failed"),
			    {
			        [&sequence] { sequence.push_back(QStringLiteral("startup")); },
			        [&sequence] { sequence.push_back(QStringLiteral("autoconnect")); },
			        [&sequence](const QString &error)
			        { sequence.push_back(QStringLiteral("error:%1").arg(error)); },
			    });

			const QStringList expected{
			    QStringLiteral("error:restore failed"),
			    QStringLiteral("startup"),
			    QStringLiteral("autoconnect"),
			};
			QCOMPARE(sequence, expected);
		}

		void doesNotReportErrorWhenEmpty()
		{
			QStringList sequence;
			QMudWorldSessionRestoreFlow::runPostRestoreFlow(
			    false, QString(),
			    {
			        [&sequence] { sequence.push_back(QStringLiteral("startup")); },
			        [&sequence] { sequence.push_back(QStringLiteral("autoconnect")); },
			        [&sequence](const QString &error)
			        { sequence.push_back(QStringLiteral("error:%1").arg(error)); },
			    });

			const QStringList expected{
			    QStringLiteral("startup"),
			    QStringLiteral("autoconnect"),
			};
			QCOMPARE(sequence, expected);
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_WorldSessionRestoreFlowUtils)

#if __has_include("tst_WorldSessionRestoreFlowUtils.moc")
#include "tst_WorldSessionRestoreFlowUtils.moc"
#endif
