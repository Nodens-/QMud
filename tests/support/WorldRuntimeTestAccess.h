/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldRuntimeTestAccess.h
 * Role: Purpose-built test access to unpublished WorldRuntime collections.
 */

#pragma once

#include "WorldRuntime.h"

/**
 * @brief Keeps raw authoritative collection access out of WorldRuntime's production API.
 *
 * Tests that must arrange execution-only state can borrow collections here. Production rule/plugin collection code
 * is restricted to committed mutation APIs or the two explicitly friended mutation dispatchers. Mutable miniwindow
 * pointers are likewise limited to WorldView, WorldRuntime itself, and this test seam.
 */
class WorldRuntimeTestAccess final
{
	public:
		static QList<WorldRuntime::Trigger> &triggers(WorldRuntime &runtime)
		{
			return runtime.triggersMutable();
		}
		static QList<WorldRuntime::Alias> &aliases(WorldRuntime &runtime)
		{
			return runtime.aliasesMutable();
		}
		static QList<WorldRuntime::Timer> &timers(WorldRuntime &runtime)
		{
			return runtime.timersMutable();
		}
		static QList<WorldRuntime::Plugin> &plugins(WorldRuntime &runtime)
		{
			return runtime.pluginsMutable();
		}
		static WorldRuntime::Plugin *plugin(WorldRuntime &runtime, const QString &pluginId)
		{
			return runtime.pluginForIdMutable(pluginId);
		}
		static MiniWindow *miniWindow(WorldRuntime &runtime, const QString &name)
		{
			return runtime.miniWindowMutable(name);
		}
		static QVector<MiniWindow *> sortedMiniWindows(WorldRuntime &runtime)
		{
			return runtime.sortedMiniWindowsMutable();
		}
		static void processRawDataPayload(WorldRuntime &runtime, const QByteArray &data,
		                                  const bool simulatedInput = false)
		{
			runtime.processRawDataPayload(data, simulatedInput);
		}
		static void layoutMiniWindows(WorldRuntime &runtime, const QSize &clientSize, const QSize &ownerSize,
		                              const bool                   underneath,
		                              const QVector<MiniWindow *> *orderedWindows = nullptr)
		{
			runtime.layoutMiniWindows(clientSize, ownerSize, underneath, orderedWindows);
		}
};
