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
	 * @brief Builds `QTime` from timer hour/minute/second fields with range normalization.
	 * @param hour Hour component.
	 * @param minute Minute component.
	 * @param second Second component (fractional allowed).
	 * @return Normalized time value.
	 */
	QTime  timeFromParts(int hour, int minute, double second);

	/**
	 * @brief Converts timer hour/minute/second fields to interval milliseconds.
	 * @param hour Hour component.
	 * @param minute Minute component.
	 * @param second Second component (fractional allowed).
	 * @return Interval duration in milliseconds.
	 */
	qint64 intervalMsFromParts(int hour, int minute, double second);

	/**
	 * @brief Resets timer runtime state fields to initial scheduling baseline.
	 * @param timer Mutable timer record.
	 * @param now Current timestamp.
	 */
	void   resetTimerFields(WorldRuntime::Timer &timer, const QDateTime &now);

	/**
	 * @brief Evaluates whether timer should fire at the provided timestamp.
	 * @param timer Mutable timer record.
	 * @param now Current timestamp.
	 * @param connected Current world connection state.
	 * @return `true` when timer is due.
	 */
	bool   isTimerDue(WorldRuntime::Timer &timer, const QDateTime &now, bool connected);

	/**
	 * @brief Applies post-fire scheduling/accounting updates to timer state.
	 * @param timer Mutable timer record.
	 * @param now Current timestamp.
	 */
	void   applyTimerFiredState(WorldRuntime::Timer &timer, const QDateTime &now);
} // namespace QMudTimerScheduling

#endif // QMUD_TIMERSCHEDULINGUTILS_H
