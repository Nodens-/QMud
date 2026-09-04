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

	struct TimerScheduleDefinition
	{
			bool               enabled{false};
			bool               atTime{false};
			int                hour{0};
			int                minute{0};
			double             second{0.0};
			int                offsetHour{0};
			int                offsetMinute{0};
			double             offsetSecond{0.0};

			[[nodiscard]] bool hasSameDeadlineDefinition(const TimerScheduleDefinition &other) const
			{
				if (atTime != other.atTime || hour != other.hour || minute != other.minute ||
				    second != other.second)
				{
					return false;
				}
				return atTime || (offsetHour == other.offsetHour && offsetMinute == other.offsetMinute &&
				                  offsetSecond == other.offsetSecond);
			}
	};

	TimerScheduleDefinition timerScheduleDefinition(const WorldRuntime::Timer &timer)
	{
		const QMap<QString, QString> &attributes = timer.attributes;
		return {
		    .enabled      = isEnabledValue(attributes.value(QStringLiteral("enabled"))),
		    .atTime       = isEnabledValue(attributes.value(QStringLiteral("at_time"))),
		    .hour         = attributes.value(QStringLiteral("hour")).toInt(),
		    .minute       = attributes.value(QStringLiteral("minute")).toInt(),
		    .second       = attributes.value(QStringLiteral("second")).toDouble(),
		    .offsetHour   = attributes.value(QStringLiteral("offset_hour")).toInt(),
		    .offsetMinute = attributes.value(QStringLiteral("offset_minute")).toInt(),
		    .offsetSecond = attributes.value(QStringLiteral("offset_second")).toDouble(),
		};
	}

	bool resetTimerFields(WorldRuntime::Timer &timer, const QDateTime &now,
	                      const TimerScheduleDefinition &schedule)
	{
		const QDateTime previousLastFired    = timer.lastFired;
		const QDateTime previousNextFireTime = timer.nextFireTime;
		const auto      changed              = [&timer, &previousLastFired, &previousNextFireTime]
		{ return timer.lastFired != previousLastFired || timer.nextFireTime != previousNextFireTime; };

		if (!schedule.enabled)
			return false;

		timer.lastFired = now;

		if (schedule.atTime)
		{
			const QTime at =
			    QMudTimerScheduling::timeFromParts(schedule.hour, schedule.minute, schedule.second);
			if (!at.isValid())
				return changed();

			QDateTime fire(now.date(), at, now.timeZone());
			if (fire < now)
				fire = fire.addDays(1);
			timer.nextFireTime = fire;
			return changed();
		}

		const qint64 intervalMs =
		    QMudTimerScheduling::intervalMsFromParts(schedule.hour, schedule.minute, schedule.second);
		const qint64 offsetMs = QMudTimerScheduling::intervalMsFromParts(
		    schedule.offsetHour, schedule.offsetMinute, schedule.offsetSecond);
		timer.nextFireTime = now.addMSecs(intervalMs - offsetMs);
		return changed();
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
		return ::resetTimerFields(timer, now, timerScheduleDefinition(timer));
	}

	bool timerDeadlineDefinitionChanged(const WorldRuntime::Timer &before, const WorldRuntime::Timer &after)
	{
		return !timerScheduleDefinition(before).hasSameDeadlineDefinition(timerScheduleDefinition(after));
	}

	bool resetTimerDeadlineIfDefinitionChanged(const WorldRuntime::Timer &before, WorldRuntime::Timer &after,
	                                           const TimerResetMutation &mutation)
	{
		if (!timerDeadlineDefinitionChanged(before, after))
			return false;

		after.lastFired    = {};
		after.nextFireTime = {};
		static_cast<void>(mutation.apply(after));
		return true;
	}

	TimerDueEvaluation evaluateTimerDue(WorldRuntime::Timer &timer, const QDateTime &now,
	                                    const bool connected)
	{
		const TimerScheduleDefinition schedule = timerScheduleDefinition(timer);
		if (!schedule.enabled)
			return {};
		if (!isEnabledValue(timer.attributes.value(QStringLiteral("active_closed"))) && !connected)
			return {};

		bool runtimeStateChanged = false;
		if (!timer.nextFireTime.isValid())
			runtimeStateChanged = ::resetTimerFields(timer, now, schedule);
		return {.due                 = timer.nextFireTime.isValid() && timer.nextFireTime <= now,
		        .runtimeStateChanged = runtimeStateChanged};
	}

	bool applyTimerFiredState(WorldRuntime::Timer &timer, const QDateTime &now)
	{
		timer.firedCount++;
		timer.lastFired = now;

		const TimerScheduleDefinition schedule = timerScheduleDefinition(timer);
		if (schedule.atTime)
		{
			if (timer.nextFireTime.isValid())
				timer.nextFireTime = timer.nextFireTime.addDays(1);
		}
		else
		{
			timer.nextFireTime = timer.nextFireTime.addMSecs(
			    intervalMsFromParts(schedule.hour, schedule.minute, schedule.second));
		}

		if (!timer.nextFireTime.isValid() || timer.nextFireTime <= now)
			static_cast<void>(resetTimerFields(timer, now));

		return isEnabledValue(timer.attributes.value(QStringLiteral("one_shot")));
	}
} // namespace QMudTimerScheduling
