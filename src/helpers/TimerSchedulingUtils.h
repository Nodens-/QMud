/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: TimerSchedulingUtils.h
 * Role: Deterministic helpers for timer due checks and post-fire accounting.
 */

#ifndef QMUD_TIMERSCHEDULINGUTILS_H
#define QMUD_TIMERSCHEDULINGUTILS_H

#include "WorldRuntime.h"

#include <QDateTime>

namespace QMudTimerScheduling
{
	/**
	 * @brief Result of evaluating a timer at one scheduler timestamp.
	 *
	 * Schedule initialization is a runtime-state mutation even when the timer is not yet due. The caller must
	 * publish that mutation to the callback snapshot whenever runtimeStateChanged is true.
	 */
	struct TimerDueEvaluation
	{
			bool due{false};
			bool runtimeStateChanged{false};
	};

	/**
	 * @brief Builds `QTime` from timer hour/minute/second fields with range normalization.
	 * @param hour Hour component.
	 * @param minute Minute component.
	 * @param second Second component (fractional allowed).
	 * @return Normalized time value.
	 */
	QTime              timeFromParts(int hour, int minute, double second);

	/**
	 * @brief Converts timer hour/minute/second fields to interval milliseconds.
	 * @param hour Hour component.
	 * @param minute Minute component.
	 * @param second Second component (fractional allowed).
	 * @return Interval duration in milliseconds.
	 */
	qint64             intervalMsFromParts(int hour, int minute, double second);

	/**
	 * @brief Resets timer runtime state fields to initial scheduling baseline.
	 * @param timer Mutable timer record.
	 * @param now Current timestamp.
	 * @return `true` when snapshot-visible runtime fields changed.
	 */
	[[nodiscard]] bool resetTimerFields(WorldRuntime::Timer &timer, const QDateTime &now);

	/**
	 * @brief Checks whether an edit changed fields that determine the timer deadline.
	 * @param before Timer definition before the edit.
	 * @param after Timer definition after the edit.
	 * @return `true` when the timer mode, duration, or offset changed and its deadline must be recalculated.
	 */
	[[nodiscard]] bool timerDeadlineDefinitionChanged(const WorldRuntime::Timer &before,
	                                                  const WorldRuntime::Timer &after);

	/**
	 * @brief Value journal for one schedule reset evaluated in more than one state copy.
	 *
	 * Callback APIs apply the same value to their request-local overlay and deferred authoritative mutation. This
	 * prevents replay from sampling a second clock value and publishing state different from what the callback saw.
	 */
	struct TimerResetMutation
	{
			QDateTime          timestamp;

			[[nodiscard]] bool apply(WorldRuntime::Timer &timer) const
			{
				return resetTimerFields(timer, timestamp);
			}
	};

	/**
	 * @brief Invalidates and rebuilds timer runtime state when its deadline definition changed.
	 * @param before Timer definition before the edit.
	 * @param after Timer definition after the edit and target for refreshed runtime state.
	 * @param mutation Reset operation carrying the authoritative scheduling timestamp.
	 * @return `true` when the deadline definition changed.
	 */
	[[nodiscard]] bool resetTimerDeadlineIfDefinitionChanged(const WorldRuntime::Timer &before,
	                                                         WorldRuntime::Timer       &after,
	                                                         const TimerResetMutation  &mutation);

	/**
	 * @brief Evaluates whether timer should fire at the provided timestamp.
	 * @param timer Mutable timer record.
	 * @param now Current timestamp.
	 * @param connected Current world connection state.
	 * @return Due state and whether schedule initialization mutated snapshot-visible runtime fields.
	 */
	[[nodiscard]] TimerDueEvaluation evaluateTimerDue(WorldRuntime::Timer &timer, const QDateTime &now,
	                                                  bool connected);

	/**
	 * @brief Applies post-fire scheduling/accounting updates to timer state.
	 * @param timer Mutable timer record.
	 * @param now Current timestamp.
	 * @return `true` when the caller must delete the one-shot timer after its action finishes.
	 */
	bool                             applyTimerFiredState(WorldRuntime::Timer &timer, const QDateTime &now);
} // namespace QMudTimerScheduling

#endif // QMUD_TIMERSCHEDULINGUTILS_H
