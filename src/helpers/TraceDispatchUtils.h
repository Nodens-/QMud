/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: TraceDispatchUtils.h
 * Role: Shared helpers for routing trace lines through plugin callbacks with world-output fallback.
 */

#ifndef QMUD_TRACEDISPATCHUTILS_H
#define QMUD_TRACEDISPATCHUTILS_H

#include <QString>
#include <functional>

namespace QMudTraceDispatch
{
	/**
	 * @brief Runtime callback hooks required to dispatch one trace line.
	 */
	struct Callbacks
	{
			std::function<bool()>                isTraceEnabled;
			std::function<void(bool)>            setTraceEnabled;
			std::function<bool(const QString &)> firePluginTrace;
			std::function<void(const QString &)> outputTraceLine;
	};

	/**
	 * @brief Emits one trace line via plugin `OnPluginTrace` callbacks or world-output fallback.
	 *
	 * Dispatch behavior matches Mushclient semantics: if trace is disabled, nothing is emitted.
	 * When enabled, trace is temporarily disabled while invoking plugin trace callbacks to prevent
	 * recursion. If no plugin handles the trace message, the fallback line is emitted.
	 *
	 * @param message Trace message text without `TRACE:` prefix.
	 * @param callbacks Runtime callback hooks used for dispatch.
	 */
	void emitTrace(const QString &message, const Callbacks &callbacks);
} // namespace QMudTraceDispatch

#endif // QMUD_TRACEDISPATCHUTILS_H
