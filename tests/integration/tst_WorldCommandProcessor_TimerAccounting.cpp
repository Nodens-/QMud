/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldCommandProcessor_TimerAccounting.cpp
 * Role: QTest coverage for WorldCommandProcessor TimerAccounting behavior.
 */

#include "TimerSchedulingUtils.h"

#include <QtTest/QTest>

namespace
{
	WorldRuntime::Timer makeTimer(const QString &name)
	{
		WorldRuntime::Timer timer;
		timer.attributes.insert(QStringLiteral("name"), name);
		timer.attributes.insert(QStringLiteral("enabled"), QStringLiteral("1"));
		timer.attributes.insert(QStringLiteral("active_closed"), QStringLiteral("1"));
		timer.attributes.insert(QStringLiteral("at_time"), QStringLiteral("0"));
		timer.attributes.insert(QStringLiteral("hour"), QStringLiteral("0"));
		timer.attributes.insert(QStringLiteral("minute"), QStringLiteral("0"));
		timer.attributes.insert(QStringLiteral("second"), QStringLiteral("1"));
		timer.attributes.insert(QStringLiteral("offset_hour"), QStringLiteral("0"));
		timer.attributes.insert(QStringLiteral("offset_minute"), QStringLiteral("0"));
		timer.attributes.insert(QStringLiteral("offset_second"), QStringLiteral("0"));
		timer.attributes.insert(QStringLiteral("one_shot"), QStringLiteral("0"));
		return timer;
	}
} // namespace

/**
 * @brief QTest fixture covering WorldCommandProcessor TimerAccounting scenarios.
 */
class tst_WorldCommandProcessor_TimerAccounting : public QObject
{
		Q_OBJECT

	// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void dueTimerAdvancesAndIncrementsFiredCount()
		{
			WorldRuntime::Timer timer = makeTimer(QStringLiteral("heartbeat"));
			const QDateTime     now(QDate(2026, 3, 15), QTime(12, 0, 0), QTimeZone::UTC);
			timer.nextFireTime = now.addMSecs(-10);

			QVERIFY(QMudTimerScheduling::isTimerDue(timer, now, true));
			QMudTimerScheduling::applyTimerFiredState(timer, now);

			QCOMPARE(timer.firedCount, 1);
			QCOMPARE(timer.lastFired, now);
			QVERIFY(timer.nextFireTime > now);
		}

		void oneShotTimerIsDisabledAfterFire()
		{
			WorldRuntime::Timer timer = makeTimer(QStringLiteral("once"));
			timer.attributes.insert(QStringLiteral("one_shot"), QStringLiteral("1"));
			const QDateTime now(QDate(2026, 3, 15), QTime(12, 0, 0), QTimeZone::UTC);
			timer.nextFireTime = now;

			QVERIFY(QMudTimerScheduling::isTimerDue(timer, now, true));
			QMudTimerScheduling::applyTimerFiredState(timer, now);

			QCOMPARE(timer.attributes.value(QStringLiteral("enabled")), QStringLiteral("0"));
		}

		void resetTimerFieldsUsesInjectedClockForAtTime()
		{
			WorldRuntime::Timer timer = makeTimer(QStringLiteral("daily"));
			timer.attributes.insert(QStringLiteral("at_time"), QStringLiteral("1"));
			timer.attributes.insert(QStringLiteral("hour"), QStringLiteral("10"));
			timer.attributes.insert(QStringLiteral("minute"), QStringLiteral("0"));
			timer.attributes.insert(QStringLiteral("second"), QStringLiteral("0"));

			const QDateTime now(QDate(2026, 3, 15), QTime(12, 0, 0), QTimeZone::UTC);
			QMudTimerScheduling::resetTimerFields(timer, now);

			QCOMPARE(timer.lastFired, now);
			QCOMPARE(timer.nextFireTime, QDateTime(QDate(2026, 3, 16), QTime(10, 0, 0), QTimeZone::UTC));
		}

		void disconnectedTimerNeedsActiveClosedToBeDue()
		{
			WorldRuntime::Timer timer = makeTimer(QStringLiteral("closed"));
			const QDateTime     now(QDate(2026, 3, 15), QTime(12, 0, 0), QTimeZone::UTC);
			timer.nextFireTime = now.addSecs(-1);
			timer.attributes.insert(QStringLiteral("active_closed"), QStringLiteral("0"));

			QVERIFY(!QMudTimerScheduling::isTimerDue(timer, now, false));
			QVERIFY(QMudTimerScheduling::isTimerDue(timer, now, true));
		}
	// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_WorldCommandProcessor_TimerAccounting)

#include "tst_WorldCommandProcessor_TimerAccounting.moc"
