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
		/**
		 * @brief Returns the runtime-owned Lua executor storage for executor-seam tests.
		 * @param runtime Runtime whose executor is inspected or decorated.
		 * @return Mutable executor ownership slot.
		 */
		static std::unique_ptr<ILuaExecutor> &luaExecutorStorage(WorldRuntime &runtime)
		{
			return runtime.m_luaExecutor;
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
		/**
		 * @brief Opens an inbound transmission boundary for deterministic staging tests.
		 * @param runtime Runtime whose boundary is opened.
		 */
		static void beginInboundTransmissionDeferralScope(WorldRuntime &runtime)
		{
			runtime.beginInboundTransmissionDeferralScope();
		}
		/**
		 * @brief Closes an inbound transmission boundary and flushes staged bytes.
		 * @param runtime Runtime whose boundary is closed.
		 */
		static void endInboundTransmissionDeferralScope(WorldRuntime &runtime)
		{
			runtime.endInboundTransmissionDeferralScope();
		}
		/**
		 * @brief Returns the active inbound transmission-boundary nesting depth.
		 * @param runtime Runtime whose boundary state is inspected.
		 * @return Current nesting depth.
		 */
		[[nodiscard]] static int inboundTransmissionDeferralDepth(const WorldRuntime &runtime)
		{
			return runtime.m_inboundTransmissionDeferralDepth;
		}
		/**
		 * @brief Sets connection phase for deterministic command-dispatch tests without a network socket.
		 * @param runtime Runtime whose phase is changed.
		 * @param phase WorldRuntime connection phase.
		 */
		static void setConnectPhase(WorldRuntime &runtime, const WorldRuntime::ConnectPhase phase)
		{
			runtime.m_connectPhase = phase;
		}
		/**
		 * @brief Commits pending partial input to runtime storage without exercising presentation behavior.
		 * @param runtime Runtime whose pending partial state is committed.
		 * @param committedText Optional receiver for the unwrapped committed text.
		 * @return `true` when pending partial input was committed.
		 */
		static bool commitPendingIncomingPartialLineStorage(WorldRuntime &runtime,
		                                                    QString      *committedText = nullptr)
		{
			return runtime.commitPendingIncomingPartialLineStorage(committedText);
		}
		static void layoutMiniWindows(WorldRuntime &runtime, const QSize &clientSize, const QSize &ownerSize,
		                              const bool                   underneath,
		                              const QVector<MiniWindow *> *orderedWindows = nullptr)
		{
			runtime.layoutMiniWindows(clientSize, ownerSize, underneath, orderedWindows);
		}
		/**
		 * @brief Dispatches a world-Lua hotspot callback through the production callback lane.
		 * @param runtime Runtime that owns the world Lua engine.
		 * @param functionName Lua callback function name.
		 * @param hotspotId Hotspot identifier passed to Lua.
		 * @param miniWindowName Miniwindow execution context passed to Lua.
		 */
		static void dispatchWorldHotspotCallback(WorldRuntime &runtime, const QString &functionName,
		                                         const QString &hotspotId      = {},
		                                         const QString &miniWindowName = {})
		{
			static_cast<void>(
			    runtime.callWorldHotspotFunction(functionName, 0, hotspotId, miniWindowName, false));
		}
		/**
		 * @brief Commits callback-local miniwindow shadows through the production reconciliation path.
		 * @param runtime Runtime receiving the shadows.
		 * @param windows Final callback-local window states.
		 * @param removedWindowNames Window ids removed by the callback.
		 * @param replacedWindowNames Window ids recreated by the callback.
		 * @param addedHotspots Exact hotspot instances added by the callback.
		 */
		static void commitLuaCallbackMiniWindowStates(WorldRuntime                &runtime,
		                                              QHash<QString, MiniWindow>   windows,
		                                              const QSet<QString>         &removedWindowNames,
		                                              const QSet<QString>         &replacedWindowNames,
		                                              MiniWindowHotspotInstanceMap addedHotspots = {})
		{
			QHash<QString, QSharedPointer<const MiniWindow>> baseWindowsByName;
			QSet<QString>                                    presentationChangedWindowNames;
			for (auto window = windows.constBegin(); window != windows.constEnd(); ++window)
			{
				if (const auto current = runtime.m_miniWindows.constFind(window.key());
				    current != runtime.m_miniWindows.constEnd())
				{
					baseWindowsByName.insert(window.key(),
					                         QSharedPointer<const MiniWindow>::create(current.value()));
				}
				presentationChangedWindowNames.insert(window.key());
			}
			for (const QString &windowName : removedWindowNames)
			{
				if (const auto current = runtime.m_miniWindows.constFind(windowName);
				    current != runtime.m_miniWindows.constEnd())
				{
					baseWindowsByName.insert(windowName,
					                         QSharedPointer<const MiniWindow>::create(current.value()));
				}
			}
			runtime.commitLuaCallbackMiniWindowStates(std::move(windows), removedWindowNames,
			                                          replacedWindowNames, std::move(addedHotspots), {},
			                                          baseWindowsByName, presentationChangedWindowNames);
		}
};
