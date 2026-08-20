/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: LuaCompletionDeliveryUtils.cpp
 * Role: Centralized, lifetime-safe delivery of Lua executor completion callbacks.
 */

#include "LuaCompletionDeliveryUtils.h"

#include "LuaExecutor.h"

#include <QCoreApplication>
// ReSharper disable once CppUnusedIncludeDirective
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QScopeGuard>
#include <QThread>

#include <memory>
#include <utility>

namespace
{
	/**
	 * @brief Coordinates exactly-once execution finalization across delivery and cancellation paths.
	 */
	class CompletionDeliveryAttempt final
	{
		public:
			/**
			 * @brief Creates an attempt with its terminal finalization callback.
			 * @param finished Callback invoked once when delivery terminates.
			 */
			explicit CompletionDeliveryAttempt(std::function<void()> finished)
			    : m_finishedCallback(std::move(finished))
			{
			}

			/**
			 * @brief Claims the attempt for completion execution.
			 * @return `true` unless the attempt was already finalized.
			 */
			[[nodiscard]] bool beginExecution()
			{
				QMutexLocker locker(&m_mutex);
				if (m_finished)
					return false;
				m_executing = true;
				return true;
			}

			/**
			 * @brief Finalizes the attempt and invokes its terminal callback once.
			 */
			void finish()
			{
				std::function<void()> finishedCallback;
				{
					QMutexLocker locker(&m_mutex);
					if (m_finished)
						return;
					m_finished       = true;
					m_executing      = false;
					finishedCallback = std::move(m_finishedCallback);
				}
				if (finishedCallback)
					finishedCallback();
			}

		private:
			QMutex                m_mutex;
			std::function<void()> m_finishedCallback;
			bool                  m_executing{false};
			bool                  m_finished{false};
	};

	/**
	 * @brief Executes a completion when its target remains alive and finalizes the attempt.
	 */
	void executeCompletion(const std::shared_ptr<CompletionDeliveryAttempt> &attempt,
	                       const QPointer<QObject> &target, const bool targetSupplied,
	                       const std::function<void(const LuaBatchDispatchResult &)> &completion,
	                       const LuaBatchDispatchResult                              &result)
	{
		if (!attempt || !attempt->beginExecution())
			return;
		const auto finish = qScopeGuard([attempt] { attempt->finish(); });
		if (targetSupplied && !target)
			return;
		if (completion)
			completion(result);
	}

	/**
	 * @brief Finalizes canceled delivery on its captured finalization endpoint.
	 */
	void finishCanceledDelivery(const LuaCompletionTarget                        &target,
	                            const std::shared_ptr<CompletionDeliveryAttempt> &attempt)
	{
		if (!attempt)
			return;
		if (!target.finalizationTarget.isValid() || target.finalizationThread == QThread::currentThread())
		{
			attempt->finish();
			return;
		}
		static_cast<void>(qmudLuaBridgePost(
		    target.finalizationTarget, [attempt] { attempt->finish(); }, [attempt] { attempt->finish(); }));
	}
} // namespace

LuaCompletionTarget qmudCaptureLuaCompletionTarget(QObject *target)
{
	LuaCompletionTarget captured;
	QThread *const      finalizationThread =
        QCoreApplication::instance() ? QCoreApplication::instance()->thread() : QThread::currentThread();
	captured.finalizationThread = finalizationThread;
	captured.finalizationTarget = qmudLuaBridgeCaptureAsyncTarget(finalizationThread);
	captured.supplied           = target != nullptr;
	if (!target)
	{
		captured.thread          = QThread::currentThread();
		captured.executionTarget = qmudLuaBridgeCaptureAsyncTarget(captured.thread.data());
		return captured;
	}
	captured.object              = target;
	captured.thread              = target->thread();
	captured.executionTarget     = qmudLuaBridgeCaptureAsyncTarget(captured.thread.data());
	captured.asynchronousPumpKey = reinterpret_cast<quintptr>(target);
	return captured;
}

void qmudDeliverLuaCompletion(const LuaCompletionTarget                                 &target,
                              const std::function<void(const LuaBatchDispatchResult &)> &completion,
                              LuaBatchDispatchResult result, const LuaCompletionDeliveryMode mode,
                              std::function<void()> deliveryFinished)
{
	const auto attempt = std::make_shared<CompletionDeliveryAttempt>(std::move(deliveryFinished));
	if (!completion || (target.supplied && !target.object))
	{
		attempt->finish();
		return;
	}

	if (mode == LuaCompletionDeliveryMode::DirectWhenOnTargetThread &&
	    target.thread == QThread::currentThread())
	{
		executeCompletion(attempt, target.object, target.supplied, completion, result);
		return;
	}

	const QPointer<QObject> targetGuard = target.object;
	static_cast<void>(qmudLuaBridgePost(
	    target.executionTarget,
	    [attempt, targetGuard, targetSupplied = target.supplied, completion,
	     result = std::move(result)]() mutable
	    { executeCompletion(attempt, targetGuard, targetSupplied, completion, result); },
	    [target, attempt] { finishCanceledDelivery(target, attempt); }, target.asynchronousPumpKey));
}
