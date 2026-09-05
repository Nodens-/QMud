/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldRuntimeTestAccess.h
 * Role: Purpose-built test access to unpublished WorldRuntime state and dispatch paths.
 */

#pragma once

#include "WorldRuntime.h"

/**
 * @brief Keeps test-only access to runtime internals out of WorldRuntime's production API.
 *
 * Tests that must arrange execution-only state can borrow collections here. Production rule/plugin collection code
 * is restricted to committed mutation APIs or the two explicitly friended mutation dispatchers. Mutable miniwindow
 * pointers and internal dispatch entry points are likewise limited to their production owners and this test seam.
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
		static void dispatchInitializeLuaEnginesWithObservedCallbacks(
		    const WorldRuntime &runtime, const QVector<LuaEngineObservedInitializationRequest> &requests,
		    const bool completionBarrier)
		{
			runtime.dispatchInitializeLuaEnginesWithObservedCallbacks(requests, completionBarrier);
		}
		[[nodiscard]] static bool
		dispatchLuaResetAndLoadScript(const WorldRuntime                      &runtime,
		                              const QSharedPointer<LuaCallbackEngine> &engine)
		{
			return runtime.dispatchLuaResetAndLoadScript(engine);
		}
		[[nodiscard]] static LuaBatchDispatchResult
		queuePluginCallbackDispatch(WorldRuntime &runtime, const LuaBatchDispatchRequest &request,
		                            const bool completionBarrier)
		{
			return runtime.queuePluginCallbackDispatch(request, completionBarrier);
		}
		static void dispatchTeardownLuaEngines(const WorldRuntime                               &runtime,
		                                       const QVector<QSharedPointer<LuaCallbackEngine>> &engines,
		                                       const bool completionBarrier)
		{
			runtime.dispatchTeardownLuaEngines(engines, completionBarrier);
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
