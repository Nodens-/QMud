/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: LuaExecutor.cpp
 * Role: Direct Lua callback execution backend implementation for ILuaExecutor seam.
 */

#include "LuaExecutor.h"

#include "LuaCallbackEngine.h"
#include "LuaExecutorWorker.h"
#include "helpers/LuaExecutionUtils.h"

#include <QDebug>
#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include <algorithm>
#include <utility>

namespace
{
#ifndef NDEBUG
	bool qmudMmStartupDiagIsWatchedPluginId(const QString &pluginId)
	{
		const QString normalized = pluginId.trimmed().toLower();
		return normalized == QStringLiteral("c97329b91f12ca48d14c3db2") ||
		       normalized == QStringLiteral("adc3a873d4e47348da7cb426") ||
		       normalized == QStringLiteral("f973af093e715dece34dc25f") ||
		       normalized == QStringLiteral("f67c4339ed0591a5b010d05b");
	}

	bool qmudMmStartupDiagIsLifecycleCallback(const QString &functionName)
	{
		return functionName == QStringLiteral("OnPluginInstall") ||
		       functionName == QStringLiteral("OnPluginEnable") ||
		       functionName == QStringLiteral("OnPluginConnect") ||
		       functionName == QStringLiteral("OnPluginDisable") ||
		       functionName == QStringLiteral("OnPluginBroadcast") ||
		       functionName == QStringLiteral("OnPluginTelnetRequest") ||
		       functionName == QStringLiteral("OnPluginTelnetSubnegotiation");
	}

	bool qmudMmStartupDiagShouldLogEngine(const LuaCallbackEngine *engine, const QString &functionName)
	{
		return engine && qmudMmStartupDiagIsLifecycleCallback(functionName) &&
		       qmudMmStartupDiagIsWatchedPluginId(engine->pluginId());
	}

	QString qmudMmStartupDiagEngineLabel(const LuaCallbackEngine *engine)
	{
		if (!engine)
			return QStringLiteral("<null>");
		const QString id   = engine->pluginId().trimmed();
		const QString name = engine->pluginName().trimmed();
		if (name.isEmpty())
			return id;
		return QStringLiteral("%1/%2").arg(id, name);
	}

	QString qmudMmStartupDiagDeferredLabel(const QVector<LuaDeferredRuntimeMutationBatch> &batches)
	{
		qsizetype mutationCount = 0;
		for (const LuaDeferredRuntimeMutationBatch &batch : batches)
			mutationCount += batch.mutations.size();
		return QStringLiteral("deferredBatches=%1 deferredMutations=%2")
		    .arg(batches.size())
		    .arg(mutationCount);
	}
#endif
} // namespace

LuaBatchDispatchResult ILuaExecutor::dispatchBatch(const LuaBatchDispatchRequest &request) const
{
	LuaBatchDispatchResult result;
	const auto             invokeForEngine = [&](LuaCallbackEngine *engine, auto &&fn)
	{
		if (!engine)
			return;
		if (request.miniWindowSnapshotArg)
		{
			engine->pushDispatchMiniWindowSnapshot(request.miniWindowSnapshotArg);
			const auto popSnapshot = qScopeGuard([engine] { engine->popDispatchMiniWindowSnapshot(); });
			fn(engine);
			QVector<LuaDeferredRuntimeMutationBatch> batches = engine->takeDeferredRuntimeMutationBatches();
#ifndef NDEBUG
			if (qmudMmStartupDiagShouldLogEngine(engine, request.functionName))
			{
				qInfo().noquote() << QStringLiteral(
				                         "[QMud][MMStartupDiag] executor-deferred callback=%1 engine=%2 %3")
				                         .arg(request.functionName, qmudMmStartupDiagEngineLabel(engine),
				                              qmudMmStartupDiagDeferredLabel(batches));
			}
#endif
			result.deferredRuntimeMutationBatches += batches;
			return;
		}
		fn(engine);
		QVector<LuaDeferredRuntimeMutationBatch> batches = engine->takeDeferredRuntimeMutationBatches();
#ifndef NDEBUG
		if (qmudMmStartupDiagShouldLogEngine(engine, request.functionName))
		{
			qInfo().noquote() << QStringLiteral(
			                         "[QMud][MMStartupDiag] executor-deferred callback=%1 engine=%2 %3")
			                         .arg(request.functionName, qmudMmStartupDiagEngineLabel(engine),
			                              qmudMmStartupDiagDeferredLabel(batches));
		}
#endif
		result.deferredRuntimeMutationBatches += batches;
	};
	const auto forEachEngine = [&](auto &&fn)
	{
		for (const auto &engine : request.engines)
		{
			invokeForEngine(engine.data(), fn);
		}
	};
	const auto withFirstEngine = [&](auto &&fn) -> bool
	{
		return std::ranges::any_of(request.engines,
		                           [&](const auto &engine)
		                           {
			                           if (!engine)
				                           return false;
			                           invokeForEngine(engine.data(), fn);
			                           return true;
		                           });
	};
	const auto withFirstEngineIndexed = [&](auto &&fn) -> bool
	{
		for (int engineIndex = 0; engineIndex < request.engines.size(); ++engineIndex)
		{
			const auto &engine = request.engines.at(engineIndex);
			if (!engine)
				continue;
			invokeForEngine(engine.data(),
			                [&](LuaCallbackEngine *targetEngine) { fn(engineIndex, targetEngine); });
			return true;
		}
		return false;
	};
	const auto forEachEngineIndexed = [&](auto &&fn) -> bool
	{
		for (int engineIndex = 0; engineIndex < request.engines.size(); ++engineIndex)
		{
			const auto &engine = request.engines.at(engineIndex);
			if (!engine)
				continue;
			bool keepGoing = true;
			invokeForEngine(engine.data(), [&](LuaCallbackEngine *targetEngine)
			                { keepGoing = fn(engineIndex, targetEngine); });
			if (!keepGoing)
				return false;
		}
		return true;
	};
	const auto collectDeferredBatches = [&](LuaCallbackEngine *engine)
	{
		QVector<LuaDeferredRuntimeMutationBatch> batches = engine->takeDeferredRuntimeMutationBatches();
#ifndef NDEBUG
		if (qmudMmStartupDiagShouldLogEngine(engine, request.functionName))
		{
			qInfo().noquote() << QStringLiteral(
			                         "[QMud][MMStartupDiag] executor-deferred callback=%1 engine=%2 %3")
			                         .arg(request.functionName, qmudMmStartupDiagEngineLabel(engine),
			                              qmudMmStartupDiagDeferredLabel(batches));
		}
#endif
		result.deferredRuntimeMutationBatches += batches;
	};
	const auto storeSuspension =
	    [&](const int engineIndex, const quint64 resumeId, LuaPendingModalStringRequest &&modalRequest)
	{
		result.suspended                    = true;
		result.modalResumeId                = resumeId;
		result.suspendedEngineIndex         = engineIndex;
		result.hasPendingModalStringRequest = true;
		result.pendingModalStringRequest    = std::move(modalRequest);
	};
	switch (request.kind)
	{
	case LuaBatchDispatchKind::NoArgs:
		result.boolResult  = true;
		result.hasFunction = false;
		for (int engineIndex = 0; engineIndex < request.engines.size(); ++engineIndex)
		{
			LuaCallbackEngine *engine = request.engines.at(engineIndex).data();
			if (!engine)
				continue;
			if (request.miniWindowSnapshotArg)
				engine->pushDispatchMiniWindowSnapshot(request.miniWindowSnapshotArg);
			const auto popSnapshot = qScopeGuard(
			    [engine, hasSnapshot = static_cast<bool>(request.miniWindowSnapshotArg)]
			    {
				    if (hasSnapshot)
					    engine->popDispatchMiniWindowSnapshot();
			    });
			Q_UNUSED(popSnapshot);
			bool                         hasFunction = false;
			bool                         suspended   = false;
			quint64                      resumeId    = 0;
			LuaPendingModalStringRequest modalRequest;
			const bool                   ok = engine->callFunctionNoArgs(
			    request.functionName, &hasFunction, request.defaultResult,
			    request.hasActionSourceOverride ? request.actionSourceOverride : -1, &suspended, &resumeId,
			    &modalRequest);
			collectDeferredBatches(engine);
#ifndef NDEBUG
			if (qmudMmStartupDiagShouldLogEngine(engine, request.functionName))
			{
				qInfo().noquote() << QStringLiteral(
				                         "[QMud][MMStartupDiag] executor-noargs callback=%1 engine=%2 "
				                         "hasFunction=%3 ok=%4 "
				                         "defaultResult=%5")
				                         .arg(request.functionName, qmudMmStartupDiagEngineLabel(engine),
				                              hasFunction ? QStringLiteral("1") : QStringLiteral("0"),
				                              ok ? QStringLiteral("1") : QStringLiteral("0"),
				                              request.defaultResult ? QStringLiteral("1")
				                                                    : QStringLiteral("0"));
			}
#endif
			if (suspended)
			{
				storeSuspension(engineIndex, resumeId, std::move(modalRequest));
				result.boolResultValid  = false;
				result.hasFunctionValid = false;
				return result;
			}
			result.boolResult  = result.boolResult && ok;
			result.hasFunction = result.hasFunction || hasFunction;
		}
		result.boolResultValid  = true;
		result.hasFunctionValid = true;
		return result;
	case LuaBatchDispatchKind::HasFunction:
	{
		result.hasFunction = false;
		static_cast<void>(
		    withFirstEngine([&](LuaCallbackEngine *engine)
		                    { result.hasFunction = engine->hasFunction(request.functionName); }));
		result.hasFunctionValid = true;
		return result;
	}
	case LuaBatchDispatchKind::String:
		for (int engineIndex = 0; engineIndex < request.engines.size(); ++engineIndex)
		{
			LuaCallbackEngine *engine = request.engines.at(engineIndex).data();
			if (!engine)
				continue;
			if (request.miniWindowSnapshotArg)
				engine->pushDispatchMiniWindowSnapshot(request.miniWindowSnapshotArg);
			const auto popSnapshot = qScopeGuard(
			    [engine, hasSnapshot = static_cast<bool>(request.miniWindowSnapshotArg)]
			    {
				    if (hasSnapshot)
					    engine->popDispatchMiniWindowSnapshot();
			    });
			Q_UNUSED(popSnapshot);
			bool                         suspended = false;
			quint64                      resumeId  = 0;
			LuaPendingModalStringRequest modalRequest;
			static_cast<void>(engine->callFunctionWithString(request.functionName, request.stringArg, nullptr,
			                                                 request.defaultResult, &suspended, &resumeId,
			                                                 &modalRequest));
			collectDeferredBatches(engine);
			if (suspended)
			{
				storeSuspension(engineIndex, resumeId, std::move(modalRequest));
				return result;
			}
		}
		return result;
	case LuaBatchDispatchKind::StringStopOnFalse:
		result.boolResult = true;
		for (int engineIndex = 0; engineIndex < request.engines.size(); ++engineIndex)
		{
			LuaCallbackEngine *engine = request.engines.at(engineIndex).data();
			if (!engine)
				continue;
			if (request.miniWindowSnapshotArg)
				engine->pushDispatchMiniWindowSnapshot(request.miniWindowSnapshotArg);
			const auto popSnapshot = qScopeGuard(
			    [engine, hasSnapshot = static_cast<bool>(request.miniWindowSnapshotArg)]
			    {
				    if (hasSnapshot)
					    engine->popDispatchMiniWindowSnapshot();
			    });
			Q_UNUSED(popSnapshot);
			bool                         hasFunction = false;
			bool                         suspended   = false;
			quint64                      resumeId    = 0;
			LuaPendingModalStringRequest modalRequest;
			const bool                   ok =
			    engine->callFunctionWithString(request.functionName, request.stringArg, &hasFunction,
			                                   request.defaultResult, &suspended, &resumeId, &modalRequest);
			collectDeferredBatches(engine);
			if (suspended)
			{
				storeSuspension(engineIndex, resumeId, std::move(modalRequest));
				result.boolResultValid = false;
				return result;
			}
			if (hasFunction && !ok)
				result.boolResult = false;
		}
		result.boolResultValid = true;
		return result;
	case LuaBatchDispatchKind::StringHandled:
		result.boolResult = false;
		for (int engineIndex = 0; engineIndex < request.engines.size(); ++engineIndex)
		{
			if (result.boolResult)
				break;
			LuaCallbackEngine *engine = request.engines.at(engineIndex).data();
			if (!engine)
				continue;
			if (request.miniWindowSnapshotArg)
				engine->pushDispatchMiniWindowSnapshot(request.miniWindowSnapshotArg);
			const auto popSnapshot = qScopeGuard(
			    [engine, hasSnapshot = static_cast<bool>(request.miniWindowSnapshotArg)]
			    {
				    if (hasSnapshot)
					    engine->popDispatchMiniWindowSnapshot();
			    });
			Q_UNUSED(popSnapshot);
			bool                         hasFunction = false;
			bool                         suspended   = false;
			quint64                      resumeId    = 0;
			LuaPendingModalStringRequest modalRequest;
			static_cast<void>(engine->callFunctionWithString(request.functionName, request.stringArg,
			                                                 &hasFunction, false, &suspended, &resumeId,
			                                                 &modalRequest));
			collectDeferredBatches(engine);
			if (suspended)
			{
				storeSuspension(engineIndex, resumeId, std::move(modalRequest));
				result.boolResultValid = false;
				return result;
			}
			if (hasFunction)
				result.boolResult = true;
		}
		result.boolResultValid = true;
		return result;
	case LuaBatchDispatchKind::Bytes:
		for (int engineIndex = 0; engineIndex < request.engines.size(); ++engineIndex)
		{
			LuaCallbackEngine *engine = request.engines.at(engineIndex).data();
			if (!engine)
				continue;
			if (request.miniWindowSnapshotArg)
				engine->pushDispatchMiniWindowSnapshot(request.miniWindowSnapshotArg);
			const auto popSnapshot = qScopeGuard(
			    [engine, hasSnapshot = static_cast<bool>(request.miniWindowSnapshotArg)]
			    {
				    if (hasSnapshot)
					    engine->popDispatchMiniWindowSnapshot();
			    });
			Q_UNUSED(popSnapshot);
			bool                         suspended = false;
			quint64                      resumeId  = 0;
			LuaPendingModalStringRequest modalRequest;
			static_cast<void>(engine->callFunctionWithBytes(request.functionName, request.bytesArg, nullptr,
			                                                request.defaultResult, &suspended, &resumeId,
			                                                &modalRequest));
			collectDeferredBatches(engine);
			if (suspended)
			{
				storeSuspension(engineIndex, resumeId, std::move(modalRequest));
				return result;
			}
		}
		return result;
	case LuaBatchDispatchKind::BytesInOut:
		result.bytesResult = request.bytesArg;
		for (int engineIndex = 0; engineIndex < request.engines.size(); ++engineIndex)
		{
			LuaCallbackEngine *engine = request.engines.at(engineIndex).data();
			if (!engine)
				continue;
			if (request.miniWindowSnapshotArg)
				engine->pushDispatchMiniWindowSnapshot(request.miniWindowSnapshotArg);
			const auto popSnapshot = qScopeGuard(
			    [engine, hasSnapshot = static_cast<bool>(request.miniWindowSnapshotArg)]
			    {
				    if (hasSnapshot)
					    engine->popDispatchMiniWindowSnapshot();
			    });
			Q_UNUSED(popSnapshot);
			bool                         suspended = false;
			quint64                      resumeId  = 0;
			LuaPendingModalStringRequest modalRequest;
			static_cast<void>(engine->callFunctionWithBytesInOut(
			    request.functionName, result.bytesResult, nullptr, &suspended, &resumeId, &modalRequest));
			collectDeferredBatches(engine);
			if (suspended)
			{
				storeSuspension(engineIndex, resumeId, std::move(modalRequest));
				return result;
			}
		}
		return result;
	case LuaBatchDispatchKind::StringInOut:
		result.stringResult = request.stringArg;
		for (int engineIndex = 0; engineIndex < request.engines.size(); ++engineIndex)
		{
			LuaCallbackEngine *engine = request.engines.at(engineIndex).data();
			if (!engine)
				continue;
			if (request.miniWindowSnapshotArg)
				engine->pushDispatchMiniWindowSnapshot(request.miniWindowSnapshotArg);
			const auto popSnapshot = qScopeGuard(
			    [engine, hasSnapshot = static_cast<bool>(request.miniWindowSnapshotArg)]
			    {
				    if (hasSnapshot)
					    engine->popDispatchMiniWindowSnapshot();
			    });
			Q_UNUSED(popSnapshot);
			bool                         suspended = false;
			quint64                      resumeId  = 0;
			LuaPendingModalStringRequest modalRequest;
			static_cast<void>(engine->callFunctionWithStringInOut(
			    request.functionName, result.stringResult, nullptr, &suspended, &resumeId, &modalRequest));
			collectDeferredBatches(engine);
			if (suspended)
			{
				storeSuspension(engineIndex, resumeId, std::move(modalRequest));
				return result;
			}
		}
		return result;
	case LuaBatchDispatchKind::NumberAndStringStopOnTrue:
		result.boolResult = false;
		for (int engineIndex = 0; engineIndex < request.engines.size(); ++engineIndex)
		{
			if (result.boolResult)
				break;
			LuaCallbackEngine *engine = request.engines.at(engineIndex).data();
			if (!engine)
				continue;
			if (request.miniWindowSnapshotArg)
				engine->pushDispatchMiniWindowSnapshot(request.miniWindowSnapshotArg);
			const auto popSnapshot = qScopeGuard(
			    [engine, hasSnapshot = static_cast<bool>(request.miniWindowSnapshotArg)]
			    {
				    if (hasSnapshot)
					    engine->popDispatchMiniWindowSnapshot();
			    });
			Q_UNUSED(popSnapshot);
			bool                         hasFunction = false;
			bool                         suspended   = false;
			quint64                      resumeId    = 0;
			LuaPendingModalStringRequest modalRequest;
			const bool                   ok = engine->callFunctionWithNumberAndString(
			    request.functionName, request.numberArg1, request.stringArg2, &hasFunction,
			    request.defaultResult, request.hasActionSourceOverride ? request.actionSourceOverride : -1,
			    &suspended, &resumeId, &modalRequest);
			QVector<LuaDeferredRuntimeMutationBatch> batches = engine->takeDeferredRuntimeMutationBatches();
#ifndef NDEBUG
			if (qmudMmStartupDiagShouldLogEngine(engine, request.functionName))
			{
				qInfo().noquote() << QStringLiteral(
				                         "[QMud][MMStartupDiag] executor-deferred callback=%1 engine=%2 %3")
				                         .arg(request.functionName, qmudMmStartupDiagEngineLabel(engine),
				                              qmudMmStartupDiagDeferredLabel(batches));
			}
#endif
			result.deferredRuntimeMutationBatches += batches;
			if (suspended)
			{
				result.suspended                    = true;
				result.modalResumeId                = resumeId;
				result.suspendedEngineIndex         = engineIndex;
				result.boolResultValid              = false;
				result.hasPendingModalStringRequest = true;
				result.pendingModalStringRequest    = std::move(modalRequest);
				return result;
			}
			if (hasFunction && ok)
				result.boolResult = true;
		}
		result.boolResultValid = true;
		return result;
	case LuaBatchDispatchKind::NumberAndStringStopOnFalse:
		result.boolResult = true;
		for (int engineIndex = 0; engineIndex < request.engines.size(); ++engineIndex)
		{
			LuaCallbackEngine *engine = request.engines.at(engineIndex).data();
			if (!engine)
				continue;
			if (request.miniWindowSnapshotArg)
				engine->pushDispatchMiniWindowSnapshot(request.miniWindowSnapshotArg);
			const auto popSnapshot = qScopeGuard(
			    [engine, hasSnapshot = static_cast<bool>(request.miniWindowSnapshotArg)]
			    {
				    if (hasSnapshot)
					    engine->popDispatchMiniWindowSnapshot();
			    });
			Q_UNUSED(popSnapshot);
			bool                         hasFunction = false;
			bool                         suspended   = false;
			quint64                      resumeId    = 0;
			LuaPendingModalStringRequest modalRequest;
			const bool                   ok = engine->callFunctionWithNumberAndString(
			    request.functionName, request.numberArg1, request.stringArg2, &hasFunction,
			    request.defaultResult, request.hasActionSourceOverride ? request.actionSourceOverride : -1,
			    &suspended, &resumeId, &modalRequest);
			QVector<LuaDeferredRuntimeMutationBatch> batches = engine->takeDeferredRuntimeMutationBatches();
#ifndef NDEBUG
			if (qmudMmStartupDiagShouldLogEngine(engine, request.functionName))
			{
				qInfo().noquote() << QStringLiteral(
				                         "[QMud][MMStartupDiag] executor-deferred callback=%1 engine=%2 %3")
				                         .arg(request.functionName, qmudMmStartupDiagEngineLabel(engine),
				                              qmudMmStartupDiagDeferredLabel(batches));
			}
#endif
			result.deferredRuntimeMutationBatches += batches;
			if (suspended)
			{
				result.suspended                    = true;
				result.modalResumeId                = resumeId;
				result.suspendedEngineIndex         = engineIndex;
				result.boolResultValid              = false;
				result.hasPendingModalStringRequest = true;
				result.pendingModalStringRequest    = std::move(modalRequest);
				return result;
			}
			if (hasFunction && !ok)
				result.boolResult = false;
		}
		result.boolResultValid = true;
		return result;
	case LuaBatchDispatchKind::NumberAndString:
		for (int engineIndex = 0; engineIndex < request.engines.size(); ++engineIndex)
		{
			LuaCallbackEngine *engine = request.engines.at(engineIndex).data();
			if (!engine)
				continue;
			if (request.miniWindowSnapshotArg)
				engine->pushDispatchMiniWindowSnapshot(request.miniWindowSnapshotArg);
			const auto popSnapshot = qScopeGuard(
			    [engine, hasSnapshot = static_cast<bool>(request.miniWindowSnapshotArg)]
			    {
				    if (hasSnapshot)
					    engine->popDispatchMiniWindowSnapshot();
			    });
			Q_UNUSED(popSnapshot);
			bool                         suspended = false;
			quint64                      resumeId  = 0;
			LuaPendingModalStringRequest modalRequest;
			static_cast<void>(engine->callFunctionWithNumberAndString(
			    request.functionName, request.numberArg1, request.stringArg2, nullptr, request.defaultResult,
			    request.hasActionSourceOverride ? request.actionSourceOverride : -1, &suspended, &resumeId,
			    &modalRequest));
			QVector<LuaDeferredRuntimeMutationBatch> batches = engine->takeDeferredRuntimeMutationBatches();
#ifndef NDEBUG
			if (qmudMmStartupDiagShouldLogEngine(engine, request.functionName))
			{
				qInfo().noquote() << QStringLiteral(
				                         "[QMud][MMStartupDiag] executor-deferred callback=%1 engine=%2 %3")
				                         .arg(request.functionName, qmudMmStartupDiagEngineLabel(engine),
				                              qmudMmStartupDiagDeferredLabel(batches));
			}
#endif
			result.deferredRuntimeMutationBatches += batches;
			if (suspended)
			{
				result.suspended                    = true;
				result.modalResumeId                = resumeId;
				result.suspendedEngineIndex         = engineIndex;
				result.hasPendingModalStringRequest = true;
				result.pendingModalStringRequest    = std::move(modalRequest);
				return result;
			}
		}
		return result;
	case LuaBatchDispatchKind::TwoNumbersAndStringStopOnFalse:
		result.boolResult = true;
		for (int engineIndex = 0; engineIndex < request.engines.size(); ++engineIndex)
		{
			LuaCallbackEngine *engine = request.engines.at(engineIndex).data();
			if (!engine)
				continue;
			if (request.miniWindowSnapshotArg)
				engine->pushDispatchMiniWindowSnapshot(request.miniWindowSnapshotArg);
			const auto popSnapshot = qScopeGuard(
			    [engine, hasSnapshot = static_cast<bool>(request.miniWindowSnapshotArg)]
			    {
				    if (hasSnapshot)
					    engine->popDispatchMiniWindowSnapshot();
			    });
			Q_UNUSED(popSnapshot);
			bool                         hasFunction = false;
			bool                         suspended   = false;
			quint64                      resumeId    = 0;
			LuaPendingModalStringRequest modalRequest;
			const bool                   ok = engine->callFunctionWithTwoNumbersAndString(
			    request.functionName, request.numberArg1, request.numberArg2, request.stringArg2,
			    &hasFunction, request.defaultResult, &suspended, &resumeId, &modalRequest);
			collectDeferredBatches(engine);
			if (suspended)
			{
				storeSuspension(engineIndex, resumeId, std::move(modalRequest));
				result.boolResultValid = false;
				return result;
			}
			if (hasFunction && !ok)
				result.boolResult = false;
		}
		result.boolResultValid = true;
		return result;
	case LuaBatchDispatchKind::TwoNumbersAndString:
		for (int engineIndex = 0; engineIndex < request.engines.size(); ++engineIndex)
		{
			LuaCallbackEngine *engine = request.engines.at(engineIndex).data();
			if (!engine)
				continue;
			if (request.miniWindowSnapshotArg)
				engine->pushDispatchMiniWindowSnapshot(request.miniWindowSnapshotArg);
			const auto popSnapshot = qScopeGuard(
			    [engine, hasSnapshot = static_cast<bool>(request.miniWindowSnapshotArg)]
			    {
				    if (hasSnapshot)
					    engine->popDispatchMiniWindowSnapshot();
			    });
			Q_UNUSED(popSnapshot);
			bool                         suspended = false;
			quint64                      resumeId  = 0;
			LuaPendingModalStringRequest modalRequest;
			static_cast<void>(engine->callFunctionWithTwoNumbersAndString(
			    request.functionName, request.numberArg1, request.numberArg2, request.stringArg2, nullptr,
			    request.defaultResult, &suspended, &resumeId, &modalRequest));
			collectDeferredBatches(engine);
			if (suspended)
			{
				storeSuspension(engineIndex, resumeId, std::move(modalRequest));
				return result;
			}
		}
		return result;
	case LuaBatchDispatchKind::NumberAndBytesStopOnTrue:
		result.boolResult = false;
		for (int engineIndex = 0; engineIndex < request.engines.size(); ++engineIndex)
		{
			if (result.boolResult)
				break;
			LuaCallbackEngine *engine = request.engines.at(engineIndex).data();
			if (!engine)
				continue;
			if (request.miniWindowSnapshotArg)
				engine->pushDispatchMiniWindowSnapshot(request.miniWindowSnapshotArg);
			const auto popSnapshot = qScopeGuard(
			    [engine, hasSnapshot = static_cast<bool>(request.miniWindowSnapshotArg)]
			    {
				    if (hasSnapshot)
					    engine->popDispatchMiniWindowSnapshot();
			    });
			Q_UNUSED(popSnapshot);
			bool                         hasFunction = false;
			bool                         suspended   = false;
			quint64                      resumeId    = 0;
			LuaPendingModalStringRequest modalRequest;
			const bool                   ok = engine->callFunctionWithNumberAndBytes(
			    request.functionName, request.numberArg1, request.bytesArg, &hasFunction,
			    request.defaultResult, &suspended, &resumeId, &modalRequest);
			collectDeferredBatches(engine);
#ifndef NDEBUG
			if (qmudMmStartupDiagShouldLogEngine(engine, request.functionName))
			{
				qInfo().noquote() << QStringLiteral(
				                         "[QMud][MMStartupDiag] executor-number-bytes-stop-true callback=%1 "
				                         "engine=%2 number=%3 bytes=%4 sample=%5 hasFunction=%6 ok=%7")
				                         .arg(request.functionName, qmudMmStartupDiagEngineLabel(engine))
				                         .arg(request.numberArg1)
				                         .arg(request.bytesArg.size())
				                         .arg(QString::fromLatin1(request.bytesArg.left(120)),
				                              hasFunction ? QStringLiteral("1") : QStringLiteral("0"),
				                              ok ? QStringLiteral("1") : QStringLiteral("0"));
			}
#endif
			if (suspended)
			{
				storeSuspension(engineIndex, resumeId, std::move(modalRequest));
				result.boolResultValid = false;
				return result;
			}
			if (hasFunction && ok)
				result.boolResult = true;
		}
		result.boolResultValid = true;
		return result;
	case LuaBatchDispatchKind::NumberAndBytes:
		for (int engineIndex = 0; engineIndex < request.engines.size(); ++engineIndex)
		{
			LuaCallbackEngine *engine = request.engines.at(engineIndex).data();
			if (!engine)
				continue;
			if (request.miniWindowSnapshotArg)
				engine->pushDispatchMiniWindowSnapshot(request.miniWindowSnapshotArg);
			const auto popSnapshot = qScopeGuard(
			    [engine, hasSnapshot = static_cast<bool>(request.miniWindowSnapshotArg)]
			    {
				    if (hasSnapshot)
					    engine->popDispatchMiniWindowSnapshot();
			    });
			Q_UNUSED(popSnapshot);
			bool                         hasFunction = false;
			bool                         suspended   = false;
			quint64                      resumeId    = 0;
			LuaPendingModalStringRequest modalRequest;
#ifndef NDEBUG
			const bool ok = engine->callFunctionWithNumberAndBytes(
			    request.functionName, request.numberArg1, request.bytesArg, &hasFunction,
			    request.defaultResult, &suspended, &resumeId, &modalRequest);
#else
			static_cast<void>(engine->callFunctionWithNumberAndBytes(
			    request.functionName, request.numberArg1, request.bytesArg, &hasFunction,
			    request.defaultResult, &suspended, &resumeId, &modalRequest));
#endif
			collectDeferredBatches(engine);
#ifndef NDEBUG
			if (qmudMmStartupDiagShouldLogEngine(engine, request.functionName))
			{
				qInfo().noquote() << QStringLiteral(
				                         "[QMud][MMStartupDiag] executor-number-bytes callback=%1 engine=%2 "
				                         "number=%3 bytes=%4 sample=%5 hasFunction=%6 ok=%7")
				                         .arg(request.functionName, qmudMmStartupDiagEngineLabel(engine))
				                         .arg(request.numberArg1)
				                         .arg(request.bytesArg.size())
				                         .arg(QString::fromLatin1(request.bytesArg.left(120)),
				                              hasFunction ? QStringLiteral("1") : QStringLiteral("0"),
				                              ok ? QStringLiteral("1") : QStringLiteral("0"));
			}
#endif
			if (suspended)
			{
				storeSuspension(engineIndex, resumeId, std::move(modalRequest));
				return result;
			}
		}
		return result;
	case LuaBatchDispatchKind::NumberAndUtf8StringsCount:
		result.countResult = 0;
		for (int engineIndex = 0; engineIndex < request.engines.size(); ++engineIndex)
		{
			LuaCallbackEngine *engine = request.engines.at(engineIndex).data();
			if (!engine)
				continue;
			if (request.miniWindowSnapshotArg)
				engine->pushDispatchMiniWindowSnapshot(request.miniWindowSnapshotArg);
			const auto popSnapshot = qScopeGuard(
			    [engine, hasSnapshot = static_cast<bool>(request.miniWindowSnapshotArg)]
			    {
				    if (hasSnapshot)
					    engine->popDispatchMiniWindowSnapshot();
			    });
			Q_UNUSED(popSnapshot);
			bool                         hasFunction = false;
			bool                         suspended   = false;
			quint64                      resumeId    = 0;
			LuaPendingModalStringRequest modalRequest;
			static_cast<void>(engine->callFunctionWithNumberAndUtf8Strings(
			    request.functionName, request.numberArg1, request.bytesArg, request.bytesArg2,
			    request.bytesArg3, &hasFunction, request.defaultResult, &suspended, &resumeId,
			    &modalRequest));
			collectDeferredBatches(engine);
#ifndef NDEBUG
			if (qmudMmStartupDiagShouldLogEngine(engine, request.functionName))
			{
				qInfo().noquote()
				    << QStringLiteral(
				           "[QMud][MMStartupDiag] executor-broadcast callback=%1 engine=%2 message=%3 "
				           "sender=%4 text=%5 hasFunction=%6 defaultResult=%7")
				           .arg(request.functionName, qmudMmStartupDiagEngineLabel(engine))
				           .arg(request.numberArg1)
				           .arg(QString::fromUtf8(request.bytesArg),
				                QString::fromUtf8(request.bytesArg3).left(120),
				                hasFunction ? QStringLiteral("1") : QStringLiteral("0"),
				                request.defaultResult ? QStringLiteral("1") : QStringLiteral("0"));
			}
#endif
			if (suspended)
			{
				storeSuspension(engineIndex, resumeId, std::move(modalRequest));
				result.countResultValid = false;
				return result;
			}
			if (hasFunction)
				++result.countResult;
		}
		result.countResultValid = true;
		return result;
	case LuaBatchDispatchKind::StringsAndWildcards:
		result.hasFunction      = false;
		result.hasFunctionValid = true;
		static_cast<void>(forEachEngineIndexed(
		    [&](const int engineIndex, LuaCallbackEngine *engine) -> bool
		    {
			    bool                         hasFunction = false;
			    bool                         suspended   = false;
			    quint64                      resumeId    = 0;
			    LuaPendingModalStringRequest modalRequest;
			    static_cast<void>(engine->callFunctionWithStringsAndWildcards(
			        request.functionName, request.stringListArg, request.stringListArg2, request.mapArg,
			        request.styleRunsArg ? request.styleRunsArg.data() : nullptr,
			        request.miniWindowSnapshotArg ? request.miniWindowSnapshotArg.data() : nullptr,
			        &hasFunction, request.hasActionSourceOverride ? request.actionSourceOverride : -1,
			        request.triggerOutputReplacesMatchedLine, request.triggerMatchedLineBufferIndex,
			        request.triggerMatchedLineAbsoluteNumber, &suspended, &resumeId, &modalRequest));
			    if (suspended)
			    {
				    storeSuspension(engineIndex, resumeId, std::move(modalRequest));
				    result.hasFunctionValid = false;
				    return false;
			    }
			    result.hasFunction = result.hasFunction || hasFunction;
			    return true;
		    }));
		return result;
	case LuaBatchDispatchKind::ExecuteScript:
	{
		result.boolResult = false;
		static_cast<void>(withFirstEngineIndexed(
		    [&](const int engineIndex, LuaCallbackEngine *engine)
		    {
			    bool                         suspended = false;
			    quint64                      resumeId  = 0;
			    LuaPendingModalStringRequest modalRequest;
			    result.boolResult = engine->executeScript(
			        request.stringArg, request.stringArg2,
			        request.styleRunsArg ? request.styleRunsArg.data() : nullptr,
			        request.executeScriptHasTriggerContext,
			        request.hasActionSourceOverride ? request.actionSourceOverride : -1,
			        request.triggerOutputReplacesMatchedLine, request.triggerMatchedLineBufferIndex,
			        request.triggerMatchedLineAbsoluteNumber, &suspended, &resumeId, &modalRequest);
			    if (suspended)
			    {
				    storeSuspension(engineIndex, resumeId, std::move(modalRequest));
				    result.boolResultValid = false;
			    }
		    }));
		if (result.suspended)
			return result;
		result.boolResultValid = true;
		return result;
	}
	case LuaBatchDispatchKind::ResetAndLoadScript:
	{
		result.boolResult = false;
		static_cast<void>(withFirstEngine(
		    [&](LuaCallbackEngine *engine)
		    {
			    engine->resetState();
			    result.boolResult = engine->loadScript();
		    }));
		result.boolResultValid = true;
		return result;
	}
	case LuaBatchDispatchKind::InitializeEnginesWithObservedCallbacksMany:
		if (request.initRequestsArg)
		{
			for (const LuaEngineObservedInitializationRequest &initRequest : *request.initRequestsArg)
			{
				if (!initRequest.engine)
					continue;
#ifndef NDEBUG
				if (qmudMmStartupDiagIsWatchedPluginId(initRequest.pluginId))
				{
					qInfo().noquote() << QStringLiteral("[QMud][MMStartupDiag] executor-init-observed "
					                                    "plugin=%1 name=%2 callbackCount=%3")
					                         .arg(initRequest.pluginId, initRequest.pluginName)
					                         .arg(initRequest.callbackNames.size());
				}
#endif
				initRequest.engine->setWorldRuntime(initRequest.runtime);
				initRequest.engine->setScriptText(initRequest.scriptText);
				if (!initRequest.pluginId.isEmpty() || !initRequest.pluginName.isEmpty() ||
				    !initRequest.pluginDirectory.isEmpty())
				{
					initRequest.engine->setPluginInfo(initRequest.pluginId, initRequest.pluginName,
					                                  initRequest.pluginDirectory);
				}
				initRequest.engine->setCallbackCatalogObserver(initRequest.observer);
				initRequest.engine->setObservedPluginCallbacks(initRequest.callbackNames);
			}
		}
		return result;
	case LuaBatchDispatchKind::UpdateObservedCallbacksMany:
		forEachEngine(
		    [&](LuaCallbackEngine *engine)
		    {
#ifndef NDEBUG
			    if (qmudMmStartupDiagShouldLogEngine(engine, QStringLiteral("OnPluginBroadcast")))
			    {
				    qInfo().noquote()
				        << QStringLiteral(
				               "[QMud][MMStartupDiag] executor-update-observed engine=%1 callbackCount=%2")
				               .arg(qmudMmStartupDiagEngineLabel(engine))
				               .arg(request.observedCallbackNamesArg.size());
			    }
#endif
			    engine->setObservedPluginCallbacks(request.observedCallbackNamesArg);
		    });
		return result;
	case LuaBatchDispatchKind::TeardownEnginesMany:
		for (const auto &engine : request.engines)
		{
			if (!engine)
				continue;
			engine->setCallbackCatalogObserver({});
			engine->setWorldRuntime(nullptr);
			engine->resetState();
			result.deferredRuntimeMutationBatches += engine->takeDeferredRuntimeMutationBatches();
			engine->clearExecutionThreadAffinity();
		}
		return result;
	case LuaBatchDispatchKind::ApplyPackageRestrictionsMany:
		forEachEngine([&](LuaCallbackEngine *engine)
		              { engine->applyPackageRestrictions(request.optionFlag); });
		return result;
	case LuaBatchDispatchKind::CallPluginLuaMarshalling:
	{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
		result.boolResultValid       = true;
		result.marshallingErrorValid = true;
		if (!request.luaStateArg)
		{
			result.boolResult       = false;
			result.marshallingError = static_cast<int>(CallPluginLuaMarshallingError::NoSuchRoutine);
			return result;
		}
		const bool dispatched = withFirstEngine(
		    [&](LuaCallbackEngine *engine)
		    {
			    if (!engine->loadScript())
			    {
				    result.boolResult       = false;
				    result.marshallingError = static_cast<int>(CallPluginLuaMarshallingError::NoSuchRoutine);
				    return;
			    }

			    if (request.applyCallingPluginContext)
				    engine->pushCallingPluginId(request.callingPluginId);
			    const auto restoreCallingContext = qScopeGuard(
			        [&]
			        {
				        if (request.applyCallingPluginContext)
					        engine->popCallingPluginId();
			        });
			    const CallPluginLuaMarshallingResult marshalling =
			        engine->callPluginLuaWithMarshalling(request.luaStateArg, request.functionName,
			                                             request.intArg1, {}, request.miniWindowSnapshotArg);
			    result.boolResult            = true;
			    lua_State *const targetState = engine->luaState();
			    result.marshallingSameState  = qmudLuaStatesShareMainThread(targetState, request.luaStateArg);
			    result.marshallingError      = static_cast<int>(marshalling.error);
			    result.marshallingIndex      = marshalling.index;
			    result.marshallingTypeName   = marshalling.typeName;
			    result.marshallingRuntimeError = marshalling.runtimeError;
			    result.marshallingReturnCount  = marshalling.returnCount;
			    if (request.refreshCallbackCatalogAfter)
				    engine->refreshLuaCallbackCatalogNow();
		    });
		if (!dispatched)
		{
			result.boolResult       = false;
			result.marshallingError = static_cast<int>(CallPluginLuaMarshallingError::NoSuchRoutine);
		}
		return result;
#else
		result.boolResult            = false;
		result.boolResultValid       = true;
		result.marshallingError      = static_cast<int>(CallPluginLuaMarshallingError::NoSuchRoutine);
		result.marshallingErrorValid = true;
		return result;
#endif
	}
	case LuaBatchDispatchKind::ProcedureWithString:
	{
		bool hasFunction  = false;
		result.boolResult = false;
		static_cast<void>(withFirstEngineIndexed(
		    [&](const int engineIndex, LuaCallbackEngine *engine)
		    {
			    if (request.applyCallingPluginContext)
				    engine->pushCallingPluginId(request.callingPluginId);
			    const auto restoreCallingContext = qScopeGuard(
			        [&]
			        {
				        if (request.applyCallingPluginContext)
					        engine->popCallingPluginId();
			        });
			    bool                         suspended = false;
			    quint64                      resumeId  = 0;
			    LuaPendingModalStringRequest modalRequest;
			    result.boolResult =
			        engine->callProcedureWithString(request.functionName, request.stringArg, &hasFunction,
			                                        &suspended, &resumeId, &modalRequest);
			    if (suspended)
			    {
				    storeSuspension(engineIndex, resumeId, std::move(modalRequest));
				    result.boolResultValid  = false;
				    result.hasFunctionValid = false;
			    }
		    }));
		if (result.suspended)
			return result;
		result.boolResultValid  = true;
		result.hasFunction      = hasFunction;
		result.hasFunctionValid = true;
		return result;
	}
	case LuaBatchDispatchKind::MxpError:
	{
		result.boolResult = false;
		static_cast<void>(withFirstEngineIndexed(
		    [&](const int engineIndex, LuaCallbackEngine *engine)
		    {
			    bool                         suspended = false;
			    quint64                      resumeId  = 0;
			    LuaPendingModalStringRequest modalRequest;
			    result.boolResult = engine->callMxpError(
			        request.functionName, request.intArg1, request.numberArg1, request.intArg2,
			        request.stringArg, &suspended, &resumeId, &modalRequest);
			    if (suspended)
			    {
				    storeSuspension(engineIndex, resumeId, std::move(modalRequest));
				    result.boolResultValid = false;
			    }
		    }));
		if (result.suspended)
			return result;
		result.boolResultValid = true;
		return result;
	}
	case LuaBatchDispatchKind::MxpStartUp:
		for (int engineIndex = 0; engineIndex < request.engines.size(); ++engineIndex)
		{
			LuaCallbackEngine *engine = request.engines.at(engineIndex).data();
			if (!engine)
				continue;
			if (request.miniWindowSnapshotArg)
				engine->pushDispatchMiniWindowSnapshot(request.miniWindowSnapshotArg);
			const auto popSnapshot = qScopeGuard(
			    [engine, hasSnapshot = static_cast<bool>(request.miniWindowSnapshotArg)]
			    {
				    if (hasSnapshot)
					    engine->popDispatchMiniWindowSnapshot();
			    });
			Q_UNUSED(popSnapshot);
			bool                         suspended = false;
			quint64                      resumeId  = 0;
			LuaPendingModalStringRequest modalRequest;
			engine->callMxpStartUp(request.functionName, &suspended, &resumeId, &modalRequest);
			collectDeferredBatches(engine);
			if (suspended)
			{
				storeSuspension(engineIndex, resumeId, std::move(modalRequest));
				return result;
			}
		}
		return result;
	case LuaBatchDispatchKind::MxpShutDown:
		for (int engineIndex = 0; engineIndex < request.engines.size(); ++engineIndex)
		{
			LuaCallbackEngine *engine = request.engines.at(engineIndex).data();
			if (!engine)
				continue;
			if (request.miniWindowSnapshotArg)
				engine->pushDispatchMiniWindowSnapshot(request.miniWindowSnapshotArg);
			const auto popSnapshot = qScopeGuard(
			    [engine, hasSnapshot = static_cast<bool>(request.miniWindowSnapshotArg)]
			    {
				    if (hasSnapshot)
					    engine->popDispatchMiniWindowSnapshot();
			    });
			Q_UNUSED(popSnapshot);
			bool                         suspended = false;
			quint64                      resumeId  = 0;
			LuaPendingModalStringRequest modalRequest;
			engine->callMxpShutDown(request.functionName, &suspended, &resumeId, &modalRequest);
			collectDeferredBatches(engine);
			if (suspended)
			{
				storeSuspension(engineIndex, resumeId, std::move(modalRequest));
				return result;
			}
		}
		return result;
	case LuaBatchDispatchKind::MxpStartTag:
	{
		result.boolResult = false;
		static_cast<void>(withFirstEngineIndexed(
		    [&](const int engineIndex, LuaCallbackEngine *engine)
		    {
			    bool                         suspended = false;
			    quint64                      resumeId  = 0;
			    LuaPendingModalStringRequest modalRequest;
			    result.boolResult =
			        engine->callMxpStartTag(request.functionName, request.stringArg, request.stringArg2,
			                                request.mapArg, &suspended, &resumeId, &modalRequest);
			    if (suspended)
			    {
				    storeSuspension(engineIndex, resumeId, std::move(modalRequest));
				    result.boolResultValid = false;
			    }
		    }));
		if (result.suspended)
			return result;
		result.boolResultValid = true;
		return result;
	}
	case LuaBatchDispatchKind::MxpEndTag:
		for (int engineIndex = 0; engineIndex < request.engines.size(); ++engineIndex)
		{
			LuaCallbackEngine *engine = request.engines.at(engineIndex).data();
			if (!engine)
				continue;
			if (request.miniWindowSnapshotArg)
				engine->pushDispatchMiniWindowSnapshot(request.miniWindowSnapshotArg);
			const auto popSnapshot = qScopeGuard(
			    [engine, hasSnapshot = static_cast<bool>(request.miniWindowSnapshotArg)]
			    {
				    if (hasSnapshot)
					    engine->popDispatchMiniWindowSnapshot();
			    });
			Q_UNUSED(popSnapshot);
			bool                         suspended = false;
			quint64                      resumeId  = 0;
			LuaPendingModalStringRequest modalRequest;
			engine->callMxpEndTag(request.functionName, request.stringArg, request.stringArg2, &suspended,
			                      &resumeId, &modalRequest);
			collectDeferredBatches(engine);
			if (suspended)
			{
				storeSuspension(engineIndex, resumeId, std::move(modalRequest));
				return result;
			}
		}
		return result;
	case LuaBatchDispatchKind::MxpSetVariable:
		for (int engineIndex = 0; engineIndex < request.engines.size(); ++engineIndex)
		{
			LuaCallbackEngine *engine = request.engines.at(engineIndex).data();
			if (!engine)
				continue;
			if (request.miniWindowSnapshotArg)
				engine->pushDispatchMiniWindowSnapshot(request.miniWindowSnapshotArg);
			const auto popSnapshot = qScopeGuard(
			    [engine, hasSnapshot = static_cast<bool>(request.miniWindowSnapshotArg)]
			    {
				    if (hasSnapshot)
					    engine->popDispatchMiniWindowSnapshot();
			    });
			Q_UNUSED(popSnapshot);
			bool                         suspended = false;
			quint64                      resumeId  = 0;
			LuaPendingModalStringRequest modalRequest;
			engine->callMxpSetVariable(request.functionName, request.stringArg, request.stringArg2,
			                           &suspended, &resumeId, &modalRequest);
			collectDeferredBatches(engine);
			if (suspended)
			{
				storeSuspension(engineIndex, resumeId, std::move(modalRequest));
				return result;
			}
		}
		return result;
	case LuaBatchDispatchKind::CancelSuspendedModalString:
		static_cast<void>(withFirstEngine([&](LuaCallbackEngine *engine)
		                                  { engine->cancelSuspendedModalString(request.modalResumeId); }));
		return result;
	case LuaBatchDispatchKind::ResumeSuspendedModalString:
		static_cast<void>(withFirstEngine(
		    [&](LuaCallbackEngine *engine)
		    { result = engine->resumeSuspendedModalString(request.modalResumeId, request.stringArg); }));
		return result;
	}
	return result;
}

void ILuaExecutor::dispatchBatchAsync(const LuaBatchDispatchRequest &request) const
{
	static_cast<void>(dispatchBatch(request));
}

void ILuaExecutor::dispatchBatchAsync(
    const LuaBatchDispatchRequest &request, QObject *completionTarget,
    const std::function<void(const LuaBatchDispatchResult &)> &completion) const
{
	const LuaBatchDispatchResult result = dispatchBatch(request);
	if (!completion)
		return;
	if (!completionTarget || completionTarget->thread() == QThread::currentThread())
	{
		completion(result);
		return;
	}
	const QPointer<QObject> targetGuard(completionTarget);
	auto                    queuedCompletion = completion;
	const bool              queued           = QMetaObject::invokeMethod(
	    completionTarget,
	    [targetGuard, completion = std::move(queuedCompletion), result]() mutable
	    {
		    if (!targetGuard)
			    return;
		    completion(result);
	    },
	    Qt::QueuedConnection);
	if (!queued)
	{
		qWarning().noquote() << QStringLiteral(
		    "[QMud][LuaExecutor] failed to queue completion delivery to target thread");
	}
}

std::unique_ptr<ILuaExecutor> makeLuaExecutor()
{
#if QMUD_ENABLE_EXPERIMENTAL_THREADED_LUA_EXECUTOR
	return std::make_unique<LuaExecutorWorker>();
#else
	return std::make_unique<LuaExecutorDirect>();
#endif
}
