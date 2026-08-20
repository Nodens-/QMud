/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: LuaExecutionUtils.cpp
 * Role: Shared Lua execution helpers for backend mode selection, thread-affinity assertions,
 * and CallPlugin Lua marshalling.
 */

#include "helpers/LuaExecutionUtils.h"

#ifdef QMUD_ENABLE_LUA_SCRIPTING
#include "LuaHeaders.h"
#include "LuaSupport.h"
#endif

// ReSharper disable once CppUnusedIncludeDirective
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDebug>
#include <QEvent>
#include <QHash>
#include <QList>
// ReSharper disable once CppUnusedIncludeDirective
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
// ReSharper disable once CppUnusedIncludeDirective
#include <QStringList>
#include <QThread>
#include <QWaitCondition>
#include <QtGlobal>

#include <algorithm>
#include <atomic>
#include <exception>
#include <memory>
#include <new>

namespace
{
	constexpr qint64    kLuaBridgeReadyTimeoutMs         = 5000;
	constexpr qint64    kLuaBridgeInvokeTimeoutDefaultMs = 30000;
	constexpr qsizetype kLuaBridgePumpBatchSize          = 32;
	std::atomic<qint64> g_luaBridgeInvokeTimeoutMs{kLuaBridgeInvokeTimeoutDefaultMs};

	enum class LuaBridgeContextState
	{
		Running,
		Stopping,
		Stopped
	};

	struct LuaBridgeThreadContext;
	bool                 scheduleLuaBridgePump(const std::shared_ptr<LuaBridgeThreadContext> &context);

	thread_local QString t_luaBridgeLastError;
	thread_local LuaBridgeInvokeStatus t_luaBridgeLastStatus{LuaBridgeInvokeStatus::Success};
	thread_local int                   t_luaBridgeRequestExecutionDepth{0};
	thread_local std::function<bool()> t_luaBridgeWaitWorkPump;

	QString                            threadLabel(const QThread *thread)
	{
		if (!thread)
			return QStringLiteral("<null>");
		const QString name = thread->objectName().trimmed();
		if (name.isEmpty())
			return QStringLiteral("0x%1").arg(
			    QString::number(reinterpret_cast<quintptr>(thread), 16).toUpper());
		return QStringLiteral("%1 [0x%2]")
		    .arg(name, QString::number(reinterpret_cast<quintptr>(thread), 16).toUpper());
	}

	bool setLuaBridgeError(const LuaBridgeInvokeStatus status, const QString &message)
	{
		t_luaBridgeLastStatus = status;
		t_luaBridgeLastError  = message;
		qWarning().noquote() << message;
		return false;
	}

	void clearLuaBridgeError()
	{
		t_luaBridgeLastStatus = LuaBridgeInvokeStatus::Success;
		t_luaBridgeLastError.clear();
	}

	QEvent::Type luaBridgePumpEventType()
	{
		static const auto type = static_cast<QEvent::Type>(QEvent::registerEventType());
		return type;
	}

	class LuaBridgePumpEvent final : public QEvent
	{
		public:
			LuaBridgePumpEvent() : QEvent(luaBridgePumpEventType())
			{
			}
	};

	struct LuaBridgeRequest
	{
			std::function<void()>                   fn;
			std::function<void(bool)>               finished;
			std::shared_ptr<LuaBridgeThreadContext> waiterContext;
			QMutex                                  mutex;
			QWaitCondition                          wake;
			bool                                    done{false};
			bool                                    canceled{false};
			LuaBridgeInvokeStatus                   cancelStatus{LuaBridgeInvokeStatus::UnknownFailure};
			QString                                 cancelReason;
			bool                                    cooperativePumpEligible{true};
			quintptr                                asynchronousPumpKey{0};
	};

	struct LuaBridgeThreadContext
	{
			QMutex                                   mutex;
			QWaitCondition                           wake;
			QList<std::shared_ptr<LuaBridgeRequest>> queue;
			QPointer<QObject>                        pumpInvoker;
			bool                                     pumpPosted{false};
			QPointer<QThread>                        thread;
			LuaBridgeContextState                    state{LuaBridgeContextState::Stopped};
			quint64                                  wakeSerial{0};
	};

	void completeBridgeRequest(const std::shared_ptr<LuaBridgeRequest> &request, const bool canceled,
	                           const LuaBridgeInvokeStatus status, const QString &reason)
	{
		if (!request)
			return;

		std::shared_ptr<LuaBridgeThreadContext> waiterContext;
		std::function<void(bool)>               finished;
		{
			QMutexLocker requestLocker(&request->mutex);
			if (request->done)
				return;
			request->done = true;
			if (canceled)
			{
				request->canceled     = true;
				request->cancelStatus = status;
				request->cancelReason = reason;
			}
			request->wake.wakeAll();
			waiterContext = request->waiterContext;
			finished      = std::move(request->finished);
		}
		if (waiterContext)
		{
			QMutexLocker waiterLocker(&waiterContext->mutex);
			++waiterContext->wakeSerial;
			waiterContext->wake.wakeAll();
		}
		if (finished)
		{
			try
			{
				finished(canceled);
			}
			catch (const std::exception &ex)
			{
				qWarning().noquote() << QStringLiteral(
				                            "[QMud][LuaBridge] asynchronous completion threw exception: %1")
				                            .arg(QString::fromLocal8Bit(ex.what()));
			}
			catch (...)
			{
				qWarning().noquote() << QStringLiteral(
				    "[QMud][LuaBridge] asynchronous completion threw unknown exception");
			}
		}
	}

	void executeBridgeRequest(const std::shared_ptr<LuaBridgeRequest> &request)
	{
		if (!request)
			return;

		try
		{
			struct BridgeRequestExecutionScope
			{
					BridgeRequestExecutionScope()
					{
						++t_luaBridgeRequestExecutionDepth;
					}

					~BridgeRequestExecutionScope()
					{
						--t_luaBridgeRequestExecutionDepth;
					}
			};
			const BridgeRequestExecutionScope executionScope;
			Q_UNUSED(executionScope);
			if (request->fn)
				request->fn();
			completeBridgeRequest(request, false, LuaBridgeInvokeStatus::Success, QString());
		}
		catch (const std::bad_alloc &)
		{
			completeBridgeRequest(request, true, LuaBridgeInvokeStatus::UnknownFailure,
			                      QStringLiteral("[QMud][LuaBridge] Invoke callback threw std::bad_alloc"));
		}
		catch (const std::exception &ex)
		{
			completeBridgeRequest(request, true, LuaBridgeInvokeStatus::UnknownFailure,
			                      QStringLiteral("[QMud][LuaBridge] Invoke callback threw exception: %1")
			                          .arg(QString::fromLocal8Bit(ex.what())));
		}
		catch (...)
		{
			completeBridgeRequest(
			    request, true, LuaBridgeInvokeStatus::UnknownFailure,
			    QStringLiteral("[QMud][LuaBridge] Invoke callback threw unknown exception"));
		}
	}

	class LuaBridgePumpInvoker final : public QObject
	{
		public:
			explicit LuaBridgePumpInvoker(const std::weak_ptr<LuaBridgeThreadContext> &context)
			    : m_context(context)
			{
			}

		public:
			bool event(QEvent *event) override
			{
				if (event->type() == luaBridgePumpEventType())
				{
					if (const auto context = m_context.lock())
					{
						qsizetype processed = 0;
						while (processed < kLuaBridgePumpBatchSize)
						{
							std::shared_ptr<LuaBridgeRequest> request;
							{
								QMutexLocker locker(&context->mutex);
								if (context->queue.isEmpty())
									break;
								request = context->queue.takeFirst();
							}
							++processed;
							if (!request)
								continue;
							executeBridgeRequest(request);
						}

						bool repost = false;
						{
							QMutexLocker locker(&context->mutex);
							if (context->queue.isEmpty())
								context->pumpPosted = false;
							else
								repost = true;
						}
						if (repost)
							QCoreApplication::postEvent(this, new LuaBridgePumpEvent());
					}
					return true;
				}
				return QObject::event(event);
			}

		private:
			std::weak_ptr<LuaBridgeThreadContext> m_context;
	};

	QMutex                                                               g_luaBridgeContextsMutex;
	QHash<QThread *, std::shared_ptr<LuaBridgeThreadContext>>            g_luaBridgeContexts;
	thread_local QHash<QThread *, std::weak_ptr<LuaBridgeThreadContext>> t_luaBridgeContextCache;

	void                                                                 pruneLuaBridgeContextCache()
	{
		auto it = t_luaBridgeContextCache.begin();
		while (it != t_luaBridgeContextCache.end())
		{
			const std::shared_ptr<LuaBridgeThreadContext> context = it.value().lock();
			if (!context || context->thread.isNull())
				it = t_luaBridgeContextCache.erase(it);
			else
				++it;
		}
	}

	void purgeDeadLuaBridgeContextsLocked()
	{
		auto it = g_luaBridgeContexts.begin();
		while (it != g_luaBridgeContexts.end())
		{
			const std::shared_ptr<LuaBridgeThreadContext> &ctx           = it.value();
			QThread *const                                 contextThread = ctx ? ctx->thread.data() : nullptr;
			if (!ctx || !contextThread || (contextThread->isFinished() && !contextThread->isRunning()))
				it = g_luaBridgeContexts.erase(it);
			else
				++it;
		}
	}

	bool targetThreadRunnable(const QThread *thread, QString &reason, LuaBridgeInvokeStatus &failureStatus)
	{
		if (!thread)
		{
			reason        = QStringLiteral("target thread pointer is null");
			failureStatus = LuaBridgeInvokeStatus::MissingTargetBridgeContext;
			return false;
		}
		if (!thread->isRunning())
		{
			reason        = QStringLiteral("target thread is not running");
			failureStatus = LuaBridgeInvokeStatus::TargetThreadNotRunning;
			return false;
		}
		if (!thread->eventDispatcher())
		{
			reason        = QStringLiteral("target thread has no event dispatcher");
			failureStatus = LuaBridgeInvokeStatus::TargetThreadNoEventDispatcher;
			return false;
		}
		failureStatus = LuaBridgeInvokeStatus::Success;
		return true;
	}

	std::shared_ptr<LuaBridgeThreadContext> contextForThreadLocked(QThread *thread)
	{
		Q_ASSERT(thread);
		purgeDeadLuaBridgeContextsLocked();

		auto &ctx = g_luaBridgeContexts[thread];
		if (!ctx)
		{
			ctx         = std::make_shared<LuaBridgeThreadContext>();
			ctx->thread = thread;
		}
		return ctx;
	}

	std::shared_ptr<LuaBridgeThreadContext> contextForThread(QThread *thread)
	{
		if (!thread)
			return nullptr;
		if (t_luaBridgeContextCache.size() > 8)
			pruneLuaBridgeContextCache();
		if (const auto cachedIt = t_luaBridgeContextCache.find(thread);
		    cachedIt != t_luaBridgeContextCache.end())
		{
			if (const std::shared_ptr<LuaBridgeThreadContext> cached = cachedIt.value().lock();
			    cached && cached->thread.data() == thread)
			{
				return cached;
			}
			t_luaBridgeContextCache.erase(cachedIt);
		}
		QMutexLocker                                  locker(&g_luaBridgeContextsMutex);
		const std::shared_ptr<LuaBridgeThreadContext> context = contextForThreadLocked(thread);
		t_luaBridgeContextCache.insert(thread, context);
		return context;
	}

	QObject *ensureLuaBridgePumpInvoker(const std::shared_ptr<LuaBridgeThreadContext> &context)
	{
		if (!context || context->thread.isNull())
			return nullptr;
		QMutexLocker locker(&context->mutex);
		if (context->pumpInvoker)
			return context->pumpInvoker.data();
		auto *const invoker = new LuaBridgePumpInvoker(context);
		invoker->moveToThread(context->thread.data());
		QObject::connect(
		    context->thread.data(), &QThread::finished, invoker,
		    [weakContext = std::weak_ptr<LuaBridgeThreadContext>(context)]
		    {
			    QList<std::shared_ptr<LuaBridgeRequest>> pendingRequests;
			    QObject                                 *invokerToDelete = nullptr;
			    const QString                            stopReason      = QStringLiteral(
			        "[QMud][LuaBridge] Invoke failed: target bridge thread stopped before request dispatch");
			    if (const auto locked = weakContext.lock())
			    {
				    QMutexLocker finishedLocker(&locked->mutex);
				    if (locked->state == LuaBridgeContextState::Running)
					    locked->state = LuaBridgeContextState::Stopping;
				    pendingRequests.swap(locked->queue);
				    invokerToDelete     = locked->pumpInvoker.data();
				    locked->state       = LuaBridgeContextState::Stopped;
				    locked->pumpInvoker = nullptr;
				    locked->pumpPosted  = false;
				    ++locked->wakeSerial;
				    locked->wake.wakeAll();
			    }
			    for (const std::shared_ptr<LuaBridgeRequest> &request : pendingRequests)
				    completeBridgeRequest(
				        request, true, LuaBridgeInvokeStatus::RequestCanceledTargetThreadStopped, stopReason);
			    if (invokerToDelete)
			    {
				    if (QThread::currentThread() == invokerToDelete->thread())
					    delete invokerToDelete;
				    else
					    invokerToDelete->deleteLater();
			    }
		    },
		    Qt::DirectConnection);
		context->pumpInvoker = invoker;
		return invoker;
	}

	bool ensureBridgeContextReady(const std::shared_ptr<LuaBridgeThreadContext> &context,
	                              const QString                                 &operationLabel)
	{
		if (!context || context->thread.isNull())
		{
			return setLuaBridgeError(
			    LuaBridgeInvokeStatus::MissingTargetBridgeContext,
			    QStringLiteral("[QMud][LuaBridge] %1 failed: missing target bridge context")
			        .arg(operationLabel));
		}
		if (!QCoreApplication::instance())
		{
			return setLuaBridgeError(
			    LuaBridgeInvokeStatus::NoQCoreApplication,
			    QStringLiteral("[QMud][LuaBridge] %1 failed: no QCoreApplication instance")
			        .arg(operationLabel));
		}
		QThread *const targetThread = context->thread.data();
		if (!targetThread || !targetThread->isRunning())
		{
			return setLuaBridgeError(
			    LuaBridgeInvokeStatus::TargetThreadNotRunning,
			    QStringLiteral("[QMud][LuaBridge] %1 failed for thread %2: target thread is not running")
			        .arg(operationLabel, threadLabel(targetThread)));
		}
		{
			QMutexLocker          locker(&context->mutex);
			QString               runnableReason;
			LuaBridgeInvokeStatus runnableStatus = LuaBridgeInvokeStatus::UnknownFailure;
			if (context->state == LuaBridgeContextState::Running && context->pumpInvoker &&
			    targetThreadRunnable(targetThread, runnableReason, runnableStatus))
			{
				return true;
			}
		}

		QObject *const invoker = ensureLuaBridgePumpInvoker(context);
		if (!invoker)
		{
			return setLuaBridgeError(
			    LuaBridgeInvokeStatus::BridgeInvokerUnavailable,
			    QStringLiteral("[QMud][LuaBridge] %1 failed for thread %2: unable to create bridge invoker")
			        .arg(operationLabel, threadLabel(targetThread)));
		}
		Q_UNUSED(invoker);

		QDeadlineTimer deadline(kLuaBridgeReadyTimeoutMs);
		for (;;)
		{
			{
				QMutexLocker          locker(&context->mutex);
				QString               runnableReason;
				LuaBridgeInvokeStatus runnableStatus = LuaBridgeInvokeStatus::UnknownFailure;
				if (targetThreadRunnable(targetThread, runnableReason, runnableStatus))
				{
					const bool wasRunning = context->state == LuaBridgeContextState::Running;
					context->state        = LuaBridgeContextState::Running;
					if (!wasRunning)
					{
						++context->wakeSerial;
						context->wake.wakeAll();
					}
					return true;
				}
				if (context->state != LuaBridgeContextState::Stopped)
				{
					context->state      = LuaBridgeContextState::Stopped;
					context->pumpPosted = false;
				}
				if (!targetThread->isRunning())
				{
					return setLuaBridgeError(
					    LuaBridgeInvokeStatus::TargetThreadNotRunning,
					    QStringLiteral(
					        "[QMud][LuaBridge] %1 failed for thread %2: target thread is not running")
					        .arg(operationLabel, threadLabel(targetThread)));
				}
				if (deadline.hasExpired())
				{
					const LuaBridgeInvokeStatus timeoutStatus =
					    runnableStatus == LuaBridgeInvokeStatus::TargetThreadNoEventDispatcher
					        ? LuaBridgeInvokeStatus::TargetThreadNoEventDispatcher
					        : LuaBridgeInvokeStatus::BridgeEndpointNotReadyTimeout;
					return setLuaBridgeError(
					    timeoutStatus,
					    QStringLiteral("[QMud][LuaBridge] %1 timed out waiting for bridge endpoint ready on "
					                   "thread %2 (%3)")
					        .arg(operationLabel, threadLabel(targetThread), runnableReason));
				}
				const qint64  remainingMs = qMax<qint64>(1, qMin<qint64>(deadline.remainingTime(), 100));
				const quint64 wakeSerial  = context->wakeSerial;
				if (remainingMs > 0 && context->state != LuaBridgeContextState::Running &&
				    context->wakeSerial == wakeSerial)
				{
					context->wake.wait(&context->mutex, static_cast<unsigned long>(remainingMs));
				}
			}
		}
	}

	bool enqueueBridgeRequest(const std::shared_ptr<LuaBridgeThreadContext> &context,
	                          const std::shared_ptr<LuaBridgeRequest>       &request)
	{
		if (!context || !request)
			return false;
		{
			QMutexLocker locker(&context->mutex);
			if (context->state != LuaBridgeContextState::Running)
				return false;
			context->queue.append(request);
			++context->wakeSerial;
			context->wake.wakeAll();
		}
		return scheduleLuaBridgePump(context);
	}

	bool pumpLuaBridgeRequestForContext(const std::shared_ptr<LuaBridgeThreadContext> &context,
	                                    const bool asynchronous, const quintptr asynchronousPumpKey = 0)
	{
		if (!context)
			return false;

		std::shared_ptr<LuaBridgeRequest> request;
		{
			QMutexLocker locker(&context->mutex);
			auto         requestIt = std::ranges::find_if(
			    context->queue,
			    [asynchronous, asynchronousPumpKey](const std::shared_ptr<LuaBridgeRequest> &candidate)
			    {
				    if (!candidate || candidate->cooperativePumpEligible == asynchronous)
					    return false;
				    return !asynchronous || (asynchronousPumpKey != 0 &&
				                             candidate->asynchronousPumpKey == asynchronousPumpKey);
			    });
			if (requestIt == context->queue.end())
				return false;
			request = *requestIt;
			context->queue.erase(requestIt);
		}
		if (!request)
			return false;

		executeBridgeRequest(request);
		return true;
	}

	bool scheduleLuaBridgePump(const std::shared_ptr<LuaBridgeThreadContext> &context)
	{
		if (!context || !QCoreApplication::instance())
			return false;
		QObject *const invoker = ensureLuaBridgePumpInvoker(context);
		if (!invoker)
			return false;
		{
			QMutexLocker locker(&context->mutex);
			if (context->state != LuaBridgeContextState::Running)
				return false;
			if (context->pumpPosted)
				return true;
			context->pumpPosted = true;
		}
		QCoreApplication::postEvent(invoker, new LuaBridgePumpEvent());
		return true;
	}

#ifdef QMUD_ENABLE_LUA_SCRIPTING
	bool pushNestedFunction(lua_State *state, const QString &dottedName)
	{
		const QStringList parts = dottedName.split(QLatin1Char('.'), Qt::SkipEmptyParts);
		if (parts.isEmpty())
			return false;
		const QByteArray first = parts.first().toLocal8Bit();
		lua_getglobal(state, first.constData());
		if (parts.size() == 1)
			return lua_isfunction(state, -1);
		if (!lua_istable(state, -1))
		{
			lua_pop(state, 1);
			return false;
		}
		for (int i = 1; i < parts.size(); ++i)
		{
			const QByteArray field = parts.at(i).toLocal8Bit();
			lua_getfield(state, -1, field.constData());
			lua_remove(state, -2);
			if (i < parts.size() - 1)
			{
				if (!lua_istable(state, -1))
				{
					lua_pop(state, 1);
					return false;
				}
			}
		}
		if (!lua_isfunction(state, -1))
		{
			lua_pop(state, 1);
			return false;
		}
		return true;
	}
#endif
} // namespace

void qmudAssertObjectThreadAffinity(const QObject *object, const char *context)
{
#if !defined(NDEBUG)
	Q_ASSERT_X(!object || object->thread() == QThread::currentThread(), context,
	           "QObject accessed from non-owning thread");
#else
	Q_UNUSED(object);
	Q_UNUSED(context);
#endif
}

bool qmudLuaBridgeEnsureObjectThreadReady(const QObject *target)
{
	clearLuaBridgeError();
	if (!target)
		return setLuaBridgeError(
		    LuaBridgeInvokeStatus::InvalidTargetOrCallback,
		    QStringLiteral("[QMud][LuaBridge] EnsureReady failed: target object is null"));
	QThread *const targetThread = target->thread();
	if (!targetThread)
	{
		return setLuaBridgeError(
		    LuaBridgeInvokeStatus::NoOwningThread,
		    QStringLiteral("[QMud][LuaBridge] EnsureReady failed for target %1: no owning thread")
		        .arg(QString::number(reinterpret_cast<quintptr>(target), 16).toUpper()));
	}
	const std::shared_ptr<LuaBridgeThreadContext> context = contextForThread(targetThread);
	return ensureBridgeContextReady(context, QStringLiteral("EnsureReady"));
}

LuaBridgeAsyncTarget qmudLuaBridgeCaptureAsyncTarget(QThread *thread)
{
	if (!thread)
		return {};
	return LuaBridgeAsyncTarget(contextForThread(thread));
}

bool qmudLuaBridgePost(const LuaBridgeAsyncTarget &target, std::function<void()> fn,
                       std::function<void()> canceled, const quintptr asynchronousPumpKey)
{
	auto cancel = [&canceled]
	{
		if (canceled)
			canceled();
	};
	if (!target.m_context || !fn)
	{
		cancel();
		return false;
	}

	const auto context = std::static_pointer_cast<LuaBridgeThreadContext>(target.m_context);
	if (!ensureBridgeContextReady(context, QStringLiteral("Asynchronous post")))
	{
		cancel();
		return false;
	}

	const auto request               = std::make_shared<LuaBridgeRequest>();
	request->fn                      = std::move(fn);
	request->cooperativePumpEligible = false;
	request->asynchronousPumpKey     = asynchronousPumpKey;
	request->finished                = [canceled = std::move(canceled)](const bool wasCanceled)
	{
		if (wasCanceled && canceled)
			canceled();
	};
	if (enqueueBridgeRequest(context, request))
		return true;

	bool removed = false;
	{
		QMutexLocker locker(&context->mutex);
		removed = context->queue.removeOne(request);
	}
	if (!removed)
		return true;
	completeBridgeRequest(request, true, LuaBridgeInvokeStatus::EnqueueFailed,
	                      QStringLiteral("[QMud][LuaBridge] asynchronous post could not be scheduled"));
	return false;
}

bool qmudLuaBridgePumpCurrentThreadOnce()
{
	QThread *const currentThread = QThread::currentThread();
	if (!currentThread)
		return false;
	const std::shared_ptr<LuaBridgeThreadContext> context = contextForThread(currentThread);
	if (!context)
		return false;
	return pumpLuaBridgeRequestForContext(context, false);
}

bool qmudLuaBridgePumpCurrentThreadAsyncOnce(const quintptr asynchronousPumpKey)
{
	if (asynchronousPumpKey == 0)
		return false;
	QThread *const currentThread = QThread::currentThread();
	if (!currentThread)
		return false;
	const std::shared_ptr<LuaBridgeThreadContext> context = contextForThread(currentThread);
	if (!context)
		return false;
	return pumpLuaBridgeRequestForContext(context, true, asynchronousPumpKey);
}

void qmudLuaBridgeSetCurrentThreadWaitWorkPump(std::function<bool()> pump)
{
	t_luaBridgeWaitWorkPump = std::move(pump);
}

void qmudLuaBridgeClearCurrentThreadWaitWorkPump()
{
	t_luaBridgeWaitWorkPump = {};
}

bool qmudLuaBridgeWaitForCurrentThreadWake(const int timeoutMs)
{
	QThread *const currentThread = QThread::currentThread();
	if (!currentThread)
		return false;
	const std::shared_ptr<LuaBridgeThreadContext> context = contextForThread(currentThread);
	if (!context)
		return false;

	const unsigned long waitMs = static_cast<unsigned long>(qMax(1, timeoutMs));
	QMutexLocker        locker(&context->mutex);
	const quint64       wakeSerial = context->wakeSerial;
	context->wake.wait(&context->mutex, waitMs);
	return context->wakeSerial != wakeSerial;
}

void qmudLuaBridgeNotifyThreadWake(QThread *thread)
{
	if (!thread)
		return;
	const std::shared_ptr<LuaBridgeThreadContext> context = contextForThread(thread);
	if (!context)
		return;

	QMutexLocker locker(&context->mutex);
	++context->wakeSerial;
	context->wake.wakeAll();
}

bool qmudLuaBridgeIsExecutingRequestOnCurrentThread()
{
	return t_luaBridgeRequestExecutionDepth > 0;
}

bool qmudLuaBridgeInvokeOnObjectThread(const QObject *target, const std::function<void()> &fn)
{
	clearLuaBridgeError();
	if (!target || !fn)
		return setLuaBridgeError(
		    LuaBridgeInvokeStatus::InvalidTargetOrCallback,
		    QStringLiteral("[QMud][LuaBridge] Invoke failed: invalid target or callback"));

	QThread *const targetThread = target->thread();
	if (!targetThread)
	{
		return setLuaBridgeError(
		    LuaBridgeInvokeStatus::NoOwningThread,
		    QStringLiteral("[QMud][LuaBridge] Invoke failed for target %1: no owning thread")
		        .arg(QString::number(reinterpret_cast<quintptr>(target), 16).toUpper()));
	}

	QThread *const currentThread = QThread::currentThread();
	if (currentThread == targetThread)
	{
		try
		{
			fn();
			return true;
		}
		catch (const std::bad_alloc &)
		{
			return setLuaBridgeError(
			    LuaBridgeInvokeStatus::UnknownFailure,
			    QStringLiteral("[QMud][LuaBridge] Invoke callback threw std::bad_alloc"));
		}
		catch (const std::exception &ex)
		{
			return setLuaBridgeError(LuaBridgeInvokeStatus::UnknownFailure,
			                         QStringLiteral("[QMud][LuaBridge] Invoke callback threw exception: %1")
			                             .arg(QString::fromLocal8Bit(ex.what())));
		}
		catch (...)
		{
			return setLuaBridgeError(
			    LuaBridgeInvokeStatus::UnknownFailure,
			    QStringLiteral("[QMud][LuaBridge] Invoke callback threw unknown exception"));
		}
	}

	const std::shared_ptr<LuaBridgeThreadContext> targetContext = contextForThread(targetThread);
	if (!ensureBridgeContextReady(targetContext, QStringLiteral("Invoke")))
		return false;
	const std::shared_ptr<LuaBridgeThreadContext> currentContext = contextForThread(currentThread);
	if (!currentContext)
	{
		return setLuaBridgeError(
		    LuaBridgeInvokeStatus::MissingCallerBridgeContext,
		    QStringLiteral("[QMud][LuaBridge] Invoke failed for edge %1 -> %2: missing caller bridge context")
		        .arg(threadLabel(currentThread), threadLabel(targetThread)));
	}

	const QPointer<const QObject>           targetGuard(target);
	const std::shared_ptr<LuaBridgeRequest> request = std::make_shared<LuaBridgeRequest>();
	request->fn                                     = [targetGuard, fn]
	{
		if (targetGuard)
			fn();
	};
	request->waiterContext = currentContext;

	if (!enqueueBridgeRequest(targetContext, request))
	{
		{
			QMutexLocker locker(&targetContext->mutex);
			targetContext->queue.removeOne(request);
		}
		return setLuaBridgeError(
		    LuaBridgeInvokeStatus::EnqueueFailed,
		    QStringLiteral(
		        "[QMud][LuaBridge] Invoke failed for edge %1 -> %2: unable to enqueue bridge request")
		        .arg(threadLabel(currentThread), threadLabel(targetThread)));
	}

	const qint64 invokeTimeoutMs =
	    qMax<qint64>(1, g_luaBridgeInvokeTimeoutMs.load(std::memory_order_relaxed));
	QDeadlineTimer deadline(invokeTimeoutMs);
	auto isRequestCompleted = [&](bool &canceled, LuaBridgeInvokeStatus &status, QString &reason) -> bool
	{
		QMutexLocker requestLocker(&request->mutex);
		if (!request->done)
			return false;
		canceled = request->canceled;
		status   = request->cancelStatus;
		reason   = request->cancelReason;
		return true;
	};
	auto completeStatusOrSuccess = [&](const bool canceled, const LuaBridgeInvokeStatus status,
	                                   const QString &reason) -> bool
	{
		if (!canceled)
			return true;
		const QString fallbackReason =
		    QStringLiteral("[QMud][LuaBridge] Invoke failed for edge %1 -> %2: request canceled")
		        .arg(threadLabel(currentThread), threadLabel(targetThread));
		return setLuaBridgeError(status, reason.isEmpty() ? fallbackReason : reason);
	};
	const QString targetThreadStoppedReason =
	    QStringLiteral("[QMud][LuaBridge] Invoke failed for edge %1 -> %2: target thread stopped while "
	                   "request was executing")
	        .arg(threadLabel(currentThread), threadLabel(targetThread));
	bool                  requestCanceled = false;
	LuaBridgeInvokeStatus cancelStatus    = LuaBridgeInvokeStatus::Success;
	QString               cancelReason;
	for (;;)
	{
		if (isRequestCompleted(requestCanceled, cancelStatus, cancelReason))
			return completeStatusOrSuccess(requestCanceled, cancelStatus, cancelReason);
		if (!targetThread->isRunning())
			return setLuaBridgeError(LuaBridgeInvokeStatus::RequestCanceledTargetThreadStopped,
			                         targetThreadStoppedReason);
		if (pumpLuaBridgeRequestForContext(currentContext, false))
			continue;
		if (t_luaBridgeWaitWorkPump)
		{
			bool pumpedWaitWork = false;
			try
			{
				pumpedWaitWork = t_luaBridgeWaitWorkPump();
			}
			catch (const std::bad_alloc &)
			{
				qWarning().noquote() << QStringLiteral(
				    "[QMud][LuaBridge] wait-work pump threw std::bad_alloc");
				t_luaBridgeWaitWorkPump = {};
			}
			catch (const std::exception &ex)
			{
				qWarning().noquote() << QStringLiteral("[QMud][LuaBridge] wait-work pump threw exception: %1")
				                            .arg(QString::fromLocal8Bit(ex.what()));
				t_luaBridgeWaitWorkPump = {};
			}
			catch (...)
			{
				qWarning().noquote() << QStringLiteral(
				    "[QMud][LuaBridge] wait-work pump threw unknown exception");
				t_luaBridgeWaitWorkPump = {};
			}
			if (pumpedWaitWork)
				continue;
		}

		if (isRequestCompleted(requestCanceled, cancelStatus, cancelReason))
			return completeStatusOrSuccess(requestCanceled, cancelStatus, cancelReason);
		if (deadline.hasExpired())
			break;

		quint64 wakeSerial = 0;
		{
			QMutexLocker waiterLocker(&currentContext->mutex);
			wakeSerial = currentContext->wakeSerial;
		}
		if (isRequestCompleted(requestCanceled, cancelStatus, cancelReason))
			return completeStatusOrSuccess(requestCanceled, cancelStatus, cancelReason);
		{
			QMutexLocker waiterLocker(&currentContext->mutex);
			const qint64 waitMs = qMax<qint64>(1, qMin<qint64>(deadline.remainingTime(), 10));
			if (currentContext->wakeSerial == wakeSerial)
				currentContext->wake.wait(&currentContext->mutex, static_cast<unsigned long>(waitMs));
		}
	}

	bool canceledBeforeDispatch = false;
	{
		QMutexLocker contextLocker(&targetContext->mutex);
		canceledBeforeDispatch = targetContext->queue.removeOne(request);
	}
	if (canceledBeforeDispatch)
	{
		QMutexLocker requestLocker(&request->mutex);
		if (!request->done)
		{
			request->canceled     = true;
			request->cancelStatus = LuaBridgeInvokeStatus::RequestCanceledBeforeDispatch;
			request->cancelReason =
			    QStringLiteral(
			        "[QMud][LuaBridge] Invoke timed out and canceled queued request for edge %1 -> %2")
			        .arg(threadLabel(currentThread), threadLabel(targetThread));
			request->done = true;
			request->wake.wakeAll();
		}
		const QString reason =
		    request->cancelReason.isEmpty()
		        ? QStringLiteral(
		              "[QMud][LuaBridge] Invoke timed out and canceled queued request for edge %1 -> %2")
		              .arg(threadLabel(currentThread), threadLabel(targetThread))
		        : request->cancelReason;
		return setLuaBridgeError(request->cancelStatus, reason);
	}

	if (isRequestCompleted(requestCanceled, cancelStatus, cancelReason))
		return completeStatusOrSuccess(requestCanceled, cancelStatus, cancelReason);

	// Preserve legacy blocking correctness: once dispatch has started, keep waiting
	// cooperatively instead of failing with an in-flight timeout.
	qWarning().noquote() << QStringLiteral(
	                            "[QMud][LuaBridge] Invoke exceeded %1 ms while request was already executing "
	                            "for edge %2 -> %3; waiting for completion")
	                            .arg(invokeTimeoutMs)
	                            .arg(threadLabel(currentThread), threadLabel(targetThread));

	for (;;)
	{
		if (isRequestCompleted(requestCanceled, cancelStatus, cancelReason))
			return completeStatusOrSuccess(requestCanceled, cancelStatus, cancelReason);
		if (!targetThread->isRunning())
			return setLuaBridgeError(LuaBridgeInvokeStatus::RequestCanceledTargetThreadStopped,
			                         targetThreadStoppedReason);
		if (pumpLuaBridgeRequestForContext(currentContext, false))
			continue;
		if (t_luaBridgeWaitWorkPump)
		{
			bool pumpedWaitWork = false;
			try
			{
				pumpedWaitWork = t_luaBridgeWaitWorkPump();
			}
			catch (const std::bad_alloc &)
			{
				qWarning().noquote() << QStringLiteral(
				    "[QMud][LuaBridge] wait-work pump threw std::bad_alloc");
				t_luaBridgeWaitWorkPump = {};
			}
			catch (const std::exception &ex)
			{
				qWarning().noquote() << QStringLiteral("[QMud][LuaBridge] wait-work pump threw exception: %1")
				                            .arg(QString::fromLocal8Bit(ex.what()));
				t_luaBridgeWaitWorkPump = {};
			}
			catch (...)
			{
				qWarning().noquote() << QStringLiteral(
				    "[QMud][LuaBridge] wait-work pump threw unknown exception");
				t_luaBridgeWaitWorkPump = {};
			}
			if (pumpedWaitWork)
				continue;
		}

		if (isRequestCompleted(requestCanceled, cancelStatus, cancelReason))
			return completeStatusOrSuccess(requestCanceled, cancelStatus, cancelReason);

		quint64 wakeSerial = 0;
		{
			QMutexLocker waiterLocker(&currentContext->mutex);
			wakeSerial = currentContext->wakeSerial;
		}
		if (isRequestCompleted(requestCanceled, cancelStatus, cancelReason))
			return completeStatusOrSuccess(requestCanceled, cancelStatus, cancelReason);

		{
			QMutexLocker waiterLocker(&currentContext->mutex);
			if (currentContext->wakeSerial == wakeSerial)
				currentContext->wake.wait(&currentContext->mutex, 100UL);
		}
	}
	Q_UNREACHABLE_RETURN(false);
}

bool qmudLuaBridgeInvokeModalOnObjectThread(const QObject *target, const std::function<void()> &fn)
{
	clearLuaBridgeError();
	if (!target || !fn)
		return setLuaBridgeError(
		    LuaBridgeInvokeStatus::InvalidTargetOrCallback,
		    QStringLiteral("[QMud][LuaBridge] Modal invoke failed: invalid target or callback"));

	QThread *const targetThread = target->thread();
	if (!targetThread)
	{
		return setLuaBridgeError(
		    LuaBridgeInvokeStatus::NoOwningThread,
		    QStringLiteral("[QMud][LuaBridge] Modal invoke failed for target %1: no owning thread")
		        .arg(QString::number(reinterpret_cast<quintptr>(target), 16).toUpper()));
	}

	QThread *const currentThread = QThread::currentThread();
	if (currentThread == targetThread)
	{
		try
		{
			fn();
			return true;
		}
		catch (const std::bad_alloc &)
		{
			return setLuaBridgeError(
			    LuaBridgeInvokeStatus::UnknownFailure,
			    QStringLiteral("[QMud][LuaBridge] Modal invoke callback threw std::bad_alloc"));
		}
		catch (const std::exception &ex)
		{
			return setLuaBridgeError(
			    LuaBridgeInvokeStatus::UnknownFailure,
			    QStringLiteral("[QMud][LuaBridge] Modal invoke callback threw exception: %1")
			        .arg(QString::fromLocal8Bit(ex.what())));
		}
		catch (...)
		{
			return setLuaBridgeError(
			    LuaBridgeInvokeStatus::UnknownFailure,
			    QStringLiteral("[QMud][LuaBridge] Modal invoke callback threw unknown exception"));
		}
	}

	const std::shared_ptr<LuaBridgeThreadContext> targetContext = contextForThread(targetThread);
	if (!ensureBridgeContextReady(targetContext, QStringLiteral("Modal invoke")))
		return false;

	const QPointer<const QObject>           targetGuard(target);
	const std::shared_ptr<LuaBridgeRequest> request = std::make_shared<LuaBridgeRequest>();
	request->fn                                     = [targetGuard, fn]
	{
		if (targetGuard)
			fn();
	};

	if (!enqueueBridgeRequest(targetContext, request))
	{
		{
			QMutexLocker locker(&targetContext->mutex);
			targetContext->queue.removeOne(request);
		}
		return setLuaBridgeError(
		    LuaBridgeInvokeStatus::EnqueueFailed,
		    QStringLiteral(
		        "[QMud][LuaBridge] Modal invoke failed for edge %1 -> %2: unable to enqueue bridge request")
		        .arg(threadLabel(currentThread), threadLabel(targetThread)));
	}

	const qint64 invokeTimeoutMs =
	    qMax<qint64>(1, g_luaBridgeInvokeTimeoutMs.load(std::memory_order_relaxed));
	QDeadlineTimer deadline(invokeTimeoutMs);
	auto isRequestCompleted = [&](bool &canceled, LuaBridgeInvokeStatus &status, QString &reason) -> bool
	{
		QMutexLocker requestLocker(&request->mutex);
		if (!request->done)
			return false;
		canceled = request->canceled;
		status   = request->cancelStatus;
		reason   = request->cancelReason;
		return true;
	};
	auto completeStatusOrSuccess = [&](const bool canceled, const LuaBridgeInvokeStatus status,
	                                   const QString &reason) -> bool
	{
		if (!canceled)
			return true;
		const QString fallbackReason =
		    QStringLiteral("[QMud][LuaBridge] Modal invoke failed for edge %1 -> %2: request canceled")
		        .arg(threadLabel(currentThread), threadLabel(targetThread));
		return setLuaBridgeError(status, reason.isEmpty() ? fallbackReason : reason);
	};
	const QString targetThreadStoppedReason =
	    QStringLiteral("[QMud][LuaBridge] Modal invoke failed for edge %1 -> %2: target thread stopped while "
	                   "request was executing")
	        .arg(threadLabel(currentThread), threadLabel(targetThread));
	bool                  requestCanceled = false;
	LuaBridgeInvokeStatus cancelStatus    = LuaBridgeInvokeStatus::Success;
	QString               cancelReason;
	for (;;)
	{
		if (isRequestCompleted(requestCanceled, cancelStatus, cancelReason))
			return completeStatusOrSuccess(requestCanceled, cancelStatus, cancelReason);
		if (!targetThread->isRunning())
			return setLuaBridgeError(LuaBridgeInvokeStatus::RequestCanceledTargetThreadStopped,
			                         targetThreadStoppedReason);
		if (deadline.hasExpired())
			break;

		QMutexLocker requestLocker(&request->mutex);
		if (request->done)
			continue;
		const qint64 waitMs = qMax<qint64>(1, qMin<qint64>(deadline.remainingTime(), 100));
		request->wake.wait(&request->mutex, static_cast<unsigned long>(waitMs));
	}

	bool canceledBeforeDispatch = false;
	{
		QMutexLocker contextLocker(&targetContext->mutex);
		canceledBeforeDispatch = targetContext->queue.removeOne(request);
	}
	if (canceledBeforeDispatch)
	{
		QMutexLocker requestLocker(&request->mutex);
		if (!request->done)
		{
			request->canceled     = true;
			request->cancelStatus = LuaBridgeInvokeStatus::RequestCanceledBeforeDispatch;
			request->cancelReason =
			    QStringLiteral(
			        "[QMud][LuaBridge] Modal invoke timed out and canceled queued request for edge %1 -> %2")
			        .arg(threadLabel(currentThread), threadLabel(targetThread));
			request->done = true;
			request->wake.wakeAll();
		}
		const QString reason = request->cancelReason.isEmpty()
		                           ? QStringLiteral("[QMud][LuaBridge] Modal invoke timed out and canceled "
		                                            "queued request for edge %1 -> %2")
		                                 .arg(threadLabel(currentThread), threadLabel(targetThread))
		                           : request->cancelReason;
		return setLuaBridgeError(request->cancelStatus, reason);
	}

	for (;;)
	{
		if (isRequestCompleted(requestCanceled, cancelStatus, cancelReason))
			return completeStatusOrSuccess(requestCanceled, cancelStatus, cancelReason);
		if (!targetThread->isRunning())
			return setLuaBridgeError(LuaBridgeInvokeStatus::RequestCanceledTargetThreadStopped,
			                         targetThreadStoppedReason);

		QMutexLocker requestLocker(&request->mutex);
		if (!request->done)
			request->wake.wait(&request->mutex, 100UL);
	}
	Q_UNREACHABLE_RETURN(false);
}

int qmudLuaBridgeInvokeTimeoutMs()
{
	return static_cast<int>(qMax<qint64>(1, g_luaBridgeInvokeTimeoutMs.load(std::memory_order_relaxed)));
}

void qmudSetLuaBridgeInvokeTimeoutMs(const int timeoutMs)
{
	const qint64 clamped = timeoutMs > 0 ? static_cast<qint64>(timeoutMs) : kLuaBridgeInvokeTimeoutDefaultMs;
	g_luaBridgeInvokeTimeoutMs.store(qMax<qint64>(1, clamped), std::memory_order_relaxed);
}

QString qmudLuaBridgeLastError()
{
	return t_luaBridgeLastError;
}

LuaBridgeInvokeStatus qmudLuaBridgeLastStatus()
{
	return t_luaBridgeLastStatus;
}

bool qmudLuaStatesShareMainThread(lua_State *left, lua_State *right)
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	if (!left || !right)
		return false;
	if (left == right)
		return true;
	const auto mainThreadForState = [](lua_State *state) -> lua_State *
	{
		if (!lua_checkstack(state, 1))
			return state;
		lua_rawgeti(state, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
		lua_State *mainThread = lua_tothread(state, -1);
		lua_pop(state, 1);
		return mainThread ? mainThread : state;
	};
	return mainThreadForState(left) == mainThreadForState(right);
#else
	Q_UNUSED(left);
	Q_UNUSED(right);
	return false;
#endif
}

CallPluginLuaMarshallingResult qmudCallPluginLuaWithMarshalling(lua_State     *callerState,
                                                                lua_State     *targetState,
                                                                const QString &routine, const int firstArg)
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	CallPluginLuaMarshallingResult result;
	if (!callerState || !targetState)
	{
		result.error = CallPluginLuaMarshallingError::NoSuchRoutine;
		return result;
	}

	const int top      = lua_gettop(callerState);
	const int argCount = qMax(0, top - firstArg + 1);
	if (qmudLuaStatesShareMainThread(callerState, targetState))
	{
		if (!pushNestedFunction(callerState, routine))
		{
			result.error = CallPluginLuaMarshallingError::NoSuchRoutine;
			return result;
		}

		lua_insert(callerState, firstArg);
		if (QMudLuaSupport::callLuaProtected(callerState, argCount, LUA_MULTRET, 0) != 0)
		{
			const char *err     = lua_tostring(callerState, -1);
			result.error        = CallPluginLuaMarshallingError::RuntimeError;
			result.runtimeError = QString::fromUtf8(err ? err : "unknown");
			lua_settop(callerState, firstArg - 1);
			return result;
		}

		result.returnCount = lua_gettop(callerState) - firstArg + 1;
		return result;
	}

	lua_settop(targetState, 0);
	if (!pushNestedFunction(targetState, routine))
	{
		result.error = CallPluginLuaMarshallingError::NoSuchRoutine;
		return result;
	}

	lua_checkstack(targetState, argCount + 2);
	for (int i = firstArg; i <= top; ++i)
	{
		switch (const int type = lua_type(callerState, i); type)
		{
		case LUA_TNIL:
			lua_pushnil(targetState);
			break;
		case LUA_TBOOLEAN:
			lua_pushboolean(targetState, lua_toboolean(callerState, i));
			break;
		case LUA_TNUMBER:
			lua_pushnumber(targetState, lua_tonumber(callerState, i));
			break;
		case LUA_TSTRING:
		{
			size_t      len = 0;
			const char *s   = lua_tolstring(callerState, i, &len);
			lua_pushlstring(targetState, s, len);
			break;
		}
		default:
		{
			result.error    = CallPluginLuaMarshallingError::UnsupportedArgumentType;
			result.index    = i;
			result.typeName = lua_typename(callerState, type);
			lua_settop(targetState, 0);
			return result;
		}
		}
	}

	if (QMudLuaSupport::callLuaProtected(targetState, argCount, LUA_MULTRET, 0) != 0)
	{
		const char *err     = lua_tostring(targetState, -1);
		result.error        = CallPluginLuaMarshallingError::RuntimeError;
		result.runtimeError = QString::fromUtf8(err ? err : "unknown");
		lua_settop(targetState, 0);
		return result;
	}

	const int retCount = lua_gettop(targetState);
	for (int i = 1; i <= retCount; ++i)
	{
		switch (const int type = lua_type(targetState, i); type)
		{
		case LUA_TNIL:
			lua_pushnil(callerState);
			break;
		case LUA_TBOOLEAN:
			lua_pushboolean(callerState, lua_toboolean(targetState, i));
			break;
		case LUA_TNUMBER:
			lua_pushnumber(callerState, lua_tonumber(targetState, i));
			break;
		case LUA_TSTRING:
		{
			size_t      len = 0;
			const char *s   = lua_tolstring(targetState, i, &len);
			lua_pushlstring(callerState, s, len);
			break;
		}
		default:
		{
			result.error    = CallPluginLuaMarshallingError::UnsupportedReturnType;
			result.index    = i;
			result.typeName = lua_typename(targetState, type);
			lua_settop(targetState, 0);
			return result;
		}
		}
	}

	lua_settop(targetState, 0);
	result.returnCount = retCount;
	return result;
#else
	Q_UNUSED(callerState);
	Q_UNUSED(targetState);
	Q_UNUSED(routine);
	Q_UNUSED(firstArg);
	CallPluginLuaMarshallingResult result;
	result.error = CallPluginLuaMarshallingError::NoSuchRoutine;
	return result;
#endif
}
