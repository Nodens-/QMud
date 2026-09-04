/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: LuaCompletionDeliveryUtils.h
 * Role: Centralized, lifetime-safe delivery of Lua executor completion callbacks.
 */

#ifndef QMUD_LUACOMPLETIONDELIVERYUTILS_H
#define QMUD_LUACOMPLETIONDELIVERYUTILS_H

#include "helpers/LuaExecutionUtils.h"

#include <QPointer>
#include <functional>

class QObject;
class QThread;
struct LuaBatchDispatchResult;

/**
 * @brief Lifetime-safe execution and finalization endpoints for one Lua completion.
 */
struct LuaCompletionTarget
{
		QPointer<QObject>    object;                 ///< Optional object whose lifetime gates delivery.
		QPointer<QThread>    thread;                 ///< Thread captured for completion execution.
		LuaBridgeAsyncTarget executionTarget;        ///< Stable completion execution endpoint.
		LuaBridgeAsyncTarget finalizationTarget;     ///< Endpoint used to finish canceled delivery.
		QPointer<QThread>    finalizationThread;     ///< Thread owning canceled-delivery finalization.
		quintptr             asynchronousPumpKey{0}; ///< Logical target identity for cooperative pumping.
		bool                 supplied{false};        ///< Whether the caller supplied an object target.
};

/**
 * @brief Thread-handoff policy for a Lua completion callback.
 */
enum class LuaCompletionDeliveryMode
{
	DirectWhenOnTargetThread, ///< Execute immediately when already on the captured target thread.
	AlwaysQueued,             ///< Always enqueue through the captured bridge endpoint.
};

/**
 * @brief Captures stable delivery endpoints and lifetime state for a completion target.
 * @param target Optional object whose thread and lifetime govern delivery.
 * @return Captured completion target safe to retain across worker execution.
 */
[[nodiscard]] LuaCompletionTarget qmudCaptureLuaCompletionTarget(QObject *target);

/**
 * @brief Delivers one Lua result exactly once or finalizes it as canceled.
 * @param target Previously captured delivery target.
 * @param completion Callback receiving the dispatch result when delivery remains valid.
 * @param result Dispatch result moved into the delivery request.
 * @param mode Direct-or-queued delivery policy.
 * @param deliveryFinished Callback run after execution or cancellation finalization.
 */
void                              qmudDeliverLuaCompletion(
    const LuaCompletionTarget &target, const std::function<void(const LuaBatchDispatchResult &)> &completion,
    LuaBatchDispatchResult    result,
    LuaCompletionDeliveryMode mode             = LuaCompletionDeliveryMode::DirectWhenOnTargetThread,
    std::function<void()>     deliveryFinished = {});

#endif // QMUD_LUACOMPLETIONDELIVERYUTILS_H
