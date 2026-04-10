/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: TraceDispatchUtils.cpp
 * Role: Shared helpers for routing trace lines through plugin callbacks with world-output fallback.
 */

#include "TraceDispatchUtils.h"

namespace QMudTraceDispatch
{
	void emitTrace(const QString &message, const Callbacks &callbacks)
	{
		if (message.isEmpty() || !callbacks.isTraceEnabled || !callbacks.setTraceEnabled ||
		    !callbacks.firePluginTrace || !callbacks.outputTraceLine)
		{
			return;
		}
		if (!callbacks.isTraceEnabled())
			return;

		const bool traceWasEnabled = callbacks.isTraceEnabled();
		callbacks.setTraceEnabled(false);
		const bool handledByPlugin = callbacks.firePluginTrace(message);
		callbacks.setTraceEnabled(traceWasEnabled);
		if (!handledByPlugin)
			callbacks.outputTraceLine(QStringLiteral("TRACE: %1").arg(message));
	}
} // namespace QMudTraceDispatch
