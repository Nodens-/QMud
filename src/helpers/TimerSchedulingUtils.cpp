/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: TimerSchedulingUtils.cpp
 * Role: Deterministic helpers for timer due checks and post-fire accounting.
 */

#include "TimerSchedulingUtils.h"

#include <QTimeZone>
#include <QtMath>

namespace
{
	bool isEnabledValue(const QString &value)
	{
		return value == QStringLiteral("1") || value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
		       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
	}
} // namespace

namespace QMudTimerScheduling
{
	QTime timeFromParts(const int hour, const int minute, const double second)
	{
		if (hour < 0 || minute < 0 || second < 0.0)
			return {};
		const int secInt = qFloor(second);
		int       msec   = qRound((second - secInt) * 1000.0);
		int       adjSec = secInt;
		if (msec >= 1000)
		{
			msec -= 1000;
			adjSec += 1;
		}
		return {hour, minute, adjSec, msec};
	}

	qint64 intervalMsFromParts(const int hour, const int minute, const double second)
	{
		const int    secInt = qFloor(second);
		const qint64 baseMs = static_cast<qint64>(hour) * 3600 * 1000 +
		                      static_cast<qint64>(minute) * 60 * 1000 + static_cast<qint64>(secInt) * 1000;
		const auto   fracMs = static_cast<qint64>(qRound((second - secInt) * 1000.0));
		return baseMs + fracMs;
	}

	bool resetTimerFields(WorldRuntime::Timer &timer, const QDateTime &now)
	{
		const QDateTime previousLastFired    = timer.lastFired;
		const QDateTime previousNextFireTime = timer.nextFireTime;
		const auto      changed              = [&timer, &previousLastFired, &previousNextFireTime]
		{ return timer.lastFired != previousLastFired || timer.nextFireTime != previousNextFireTime; };

		if (!isEnabledValue(timer.attributes.value(QStringLiteral("enabled"))))
			return false;

		const bool   atTime       = isEnabledValue(timer.attributes.value(QStringLiteral("at_time")));
		const int    hour         = timer.attributes.value(QStringLiteral("hour")).toInt();
		const int    minute       = timer.attributes.value(QStringLiteral("minute")).toInt();
		const double second       = timer.attributes.value(QStringLiteral("second")).toDouble();
		const int    offsetHour   = timer.attributes.value(QStringLiteral("offset_hour")).toInt();
		const int    offsetMinute = timer.attributes.value(QStringLiteral("offset_minute")).toInt();
		const double offsetSecond = timer.attributes.value(QStringLiteral("offset_second")).toDouble();

		timer.lastFired = now;

		if (atTime)
		{
			const QTime at = timeFromParts(hour, minute, second);
			if (!at.isValid())
				return changed();

			QDateTime fire(now.date(), at, now.timeZone());
			if (fire < now)
				fire = fire.addDays(1);
			timer.nextFireTime = fire;
			return changed();
		}

		const qint64 intervalMs = intervalMsFromParts(hour, minute, second);
		const qint64 offsetMs   = intervalMsFromParts(offsetHour, offsetMinute, offsetSecond);
		timer.nextFireTime      = now.addMSecs(intervalMs - offsetMs);
		return changed();
	}

	TimerDueEvaluation evaluateTimerDue(WorldRuntime::Timer &timer, const QDateTime &now,
	                                    const bool connected)
	{
		if (!isEnabledValue(timer.attributes.value(QStringLiteral("enabled"))))
			return {};
		if (!isEnabledValue(timer.attributes.value(QStringLiteral("active_closed"))) && !connected)
			return {};

		bool runtimeStateChanged = false;
		if (!timer.nextFireTime.isValid())
			runtimeStateChanged = resetTimerFields(timer, now);
		return {.due                 = timer.nextFireTime.isValid() && timer.nextFireTime <= now,
		        .runtimeStateChanged = runtimeStateChanged};
	}

	bool applyTimerFiredState(WorldRuntime::Timer &timer, const QDateTime &now)
	{
		timer.firedCount++;
		timer.lastFired = now;

		if (const bool atTime = isEnabledValue(timer.attributes.value(QStringLiteral("at_time"))); atTime)
		{
			if (timer.nextFireTime.isValid())
				timer.nextFireTime = timer.nextFireTime.addDays(1);
		}
		else
		{
			const int    hour   = timer.attributes.value(QStringLiteral("hour")).toInt();
			const int    minute = timer.attributes.value(QStringLiteral("minute")).toInt();
			const double second = timer.attributes.value(QStringLiteral("second")).toDouble();
			timer.nextFireTime  = timer.nextFireTime.addMSecs(intervalMsFromParts(hour, minute, second));
		}

		if (!timer.nextFireTime.isValid() || timer.nextFireTime <= now)
			static_cast<void>(resetTimerFields(timer, now));

		return isEnabledValue(timer.attributes.value(QStringLiteral("one_shot")));
	}
} // namespace QMudTimerScheduling
