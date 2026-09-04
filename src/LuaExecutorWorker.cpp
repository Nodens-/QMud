/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: LuaExecutorWorker.cpp
 * Role: Worker-thread Lua executor backend implementation.
 */

#include "LuaExecutorWorker.h"

#include "LuaCallbackEngine.h"
#include "helpers/LuaExecutionUtils.h"

#include <QDeadlineTimer>
#include <QDebug>
#include <QMetaObject>
#include <QMutexLocker>
#include <QPointer>
#include <QQueue>
#include <QScopeGuard>
#include <QThread>
#include <QWaitCondition>

#include <exception>
#include <memory>
#include <new>
#include <utility>

namespace
{
	const QString kWorkerDispatchFailurePrefix = QStringLiteral("[QMud][LuaExecutor] worker dispatch failed");

	LuaBatchDispatchResult fallbackBatchDispatchResult(const LuaBatchDispatchRequest &request)
	{
		LuaBatchDispatchResult result = makeLuaBatchDispatchFallback(request);
		if (request.kind == LuaBatchDispatchKind::CallPluginLuaMarshalling)
		{
			result.boolResult            = false;
			result.boolResultValid       = true;
			result.marshallingError      = static_cast<int>(CallPluginLuaMarshallingError::NoSuchRoutine);
			result.marshallingErrorValid = true;
			result.marshallingIndex      = 0;
			result.marshallingTypeName   = {};
			result.marshallingRuntimeError.clear();
			result.marshallingReturnCount = 0;
			result.marshallingSameState   = false;
		}
		return result;
	}

	QString dispatchRequestDebugLabel(const LuaBatchDispatchRequest &request)
	{
		const QString callbackName = request.functionName.trimmed().isEmpty()
		                                 ? QStringLiteral("<none>")
		                                 : request.functionName.trimmed();
		return QStringLiteral("kind=%1 callback=%2 engines=%3")
		    .arg(static_cast<int>(request.kind))
		    .arg(callbackName)
		    .arg(request.engines.size());
	}

	bool ensureWorkerReady(const QObject *invoker, QThread *workerThread, std::atomic_bool *workerBridgeReady)
	{
		if (!invoker || !workerThread)
			return false;
		if (workerBridgeReady && workerBridgeReady->load(std::memory_order_acquire) &&
		    workerThread->isRunning())
		{
			return true;
		}
		if (!workerThread->isRunning())
			workerThread->start();
		if (!workerThread->isRunning())
		{
			if (workerBridgeReady)
				workerBridgeReady->store(false, std::memory_order_release);
			return false;
		}
		if (qmudLuaBridgeEnsureObjectThreadReady(invoker))
		{
			if (workerBridgeReady)
				workerBridgeReady->store(true, std::memory_order_release);
			return true;
		}
		if (workerBridgeReady)
			workerBridgeReady->store(false, std::memory_order_release);
		qWarning().noquote() << QStringLiteral("[QMud][LuaExecutor] worker bridge readiness failed: %1")
		                            .arg(qmudLuaBridgeLastError());
		return false;
	}

	template <typename Operation>
	bool runWorkerOperationSafely(const QString &operation, Operation &&execute) noexcept
	{
		try
		{
			std::forward<Operation>(execute)();
			return true;
		}
		catch (const std::bad_alloc &)
		{
			qWarning().noquote()
			    << QStringLiteral("%1: std::bad_alloc while %2").arg(kWorkerDispatchFailurePrefix, operation);
		}
		catch (const std::exception &ex)
		{
			qWarning().noquote() << QStringLiteral("%1: exception while %2: %3")
			                            .arg(kWorkerDispatchFailurePrefix, operation,
			                                 QString::fromLocal8Bit(ex.what()));
		}
		catch (...)
		{
			qWarning().noquote() << QStringLiteral("%1: unknown exception while %2")
			                            .arg(kWorkerDispatchFailurePrefix, operation);
		}
		return false;
	}

	void recoverUndeliveredMutationSafely(
	    const QSharedPointer<LuaDeferredRuntimeMutationDelivery> &mutationDelivery,
	    const LuaDeferredRuntimeMutationConsumer                 &recoveryConsumer) noexcept
	{
		if (!mutationDelivery)
			return;
		static_cast<void>(runWorkerOperationSafely(
		    QStringLiteral("recovering an undelivered asynchronous mutation journal"),
		    [&] { static_cast<void>(mutationDelivery->recoverUndelivered(recoveryConsumer)); }));
	}

} // namespace

struct LuaExecutorWorker::DeferredMutationDeliveryRegistry
{
		quint64 reserveId()
		{
			QMutexLocker locker(&mutex);
			return nextId++;
		}

		void insert(const quint64 id, const QSharedPointer<LuaDeferredRuntimeMutationDelivery> &delivery)
		{
			QMutexLocker locker(&mutex);
			pending.insert(id, delivery);
		}

		bool consumeDelivery(const quint64                                             id,
		                     const LuaDeferredRuntimeMutationDelivery::DeliveryAction &consumer)
		{
			bool                  runDelivery    = false;
			QThread *const        deliveryThread = QThread::currentThread();
			std::function<void()> action;
			{
				QMutexLocker locker(&mutex);
				if (recoveryStarted || !pending.contains(id))
					return false;
				QVector<LuaDeferredRuntimeMutationBatch> earlierBatches;
				auto                                     earlier = pending.begin();
				while (earlier != pending.end() && earlier.key() < id)
				{
					if (earlier.value())
						earlierBatches += earlier.value()->takeBackupForRecovery();
					earlier = pending.erase(earlier);
				}
				QSharedPointer<LuaDeferredRuntimeMutationDelivery> delivery = pending.take(id);
				if (!delivery)
					return false;
				action = [delivery, consumer, earlierBatches = std::move(earlierBatches)]() mutable
				{
					Q_UNUSED(delivery);
					if (consumer)
						consumer(std::move(earlierBatches));
				};
				if (activeDeliveries == 0)
				{
					activeDeliveries = 1;
					activeDeliveryThreads.insert(deliveryThread, 1);
					runDelivery = true;
				}
				else
				{
					queuedDeliveryActions.insert(id, action);
				}
			}
			if (runDelivery)
				runDeliveryActions(std::move(action), deliveryThread);
			return true;
		}

		void runDeliveryActions(std::function<void()> action, QThread *deliveryThread)
		{
			std::exception_ptr firstException;
			for (;;)
			{
				try
				{
					if (action)
						action();
				}
				catch (...)
				{
					if (!firstException)
						firstException = std::current_exception();
				}

				std::function<void()> deferredRecovery;
				{
					QMutexLocker locker(&mutex);
					if (!queuedDeliveryActions.isEmpty())
					{
						auto next = queuedDeliveryActions.begin();
						action    = std::move(next.value());
						queuedDeliveryActions.erase(next);
						continue;
					}
					activeDeliveries = 0;
					activeDeliveryThreads.remove(deliveryThread);
					activeDeliveriesFinished.wakeAll();
					deferredRecovery = std::move(recoveryAfterActiveDeliveries);
				}
				if (deferredRecovery)
					deferredRecovery();
				break;
			}
			if (firstException)
				std::rethrow_exception(firstException);
		}

		void consumeAllForRecovery(QVector<LuaDeferredRuntimeMutationBatch>  newerBatches,
		                           const LuaDeferredRuntimeMutationConsumer &consumer)
		{
			QMap<quint64, QSharedPointer<LuaDeferredRuntimeMutationDelivery>> deliveries;
			bool                                                              deferRecovery = false;
			{
				QMutexLocker locker(&mutex);
				recoveryStarted = true;
				deferRecovery   = activeDeliveryThreads.value(QThread::currentThread()) > 0;
				while (!deferRecovery && activeDeliveries > 0)
				{
					locker.unlock();
					if (!qmudLuaBridgePumpCurrentThreadOnce())
					{
						locker.relock();
						if (activeDeliveries > 0)
							activeDeliveriesFinished.wait(&mutex, 10);
						locker.unlock();
					}
					locker.relock();
				}
				deliveries.swap(pending);
			}
			QVector<LuaDeferredRuntimeMutationBatch> batches;
			for (const QSharedPointer<LuaDeferredRuntimeMutationDelivery> &delivery : deliveries)
			{
				if (delivery)
					batches += delivery->takeBackupForRecovery();
			}
			batches += std::move(newerBatches);
			if (batches.isEmpty())
				return;
			if (!deferRecovery)
			{
				consumer(std::move(batches));
				return;
			}
			std::function<void()> recovery = [consumer, batches = std::move(batches)]() mutable
			{ consumer(std::move(batches)); };
			{
				QMutexLocker locker(&mutex);
				if (activeDeliveries > 0)
				{
					recoveryAfterActiveDeliveries = std::move(recovery);
					return;
				}
			}
			recovery();
		}

		QMutex                                                            mutex;
		QWaitCondition                                                    activeDeliveriesFinished;
		quint64                                                           nextId{1};
		qsizetype                                                         activeDeliveries{0};
		bool                                                              recoveryStarted{false};
		QHash<QThread *, qsizetype>                                       activeDeliveryThreads;
		QMap<quint64, std::function<void()>>                              queuedDeliveryActions;
		std::function<void()>                                             recoveryAfterActiveDeliveries;
		QMap<quint64, QSharedPointer<LuaDeferredRuntimeMutationDelivery>> pending;
};

struct LuaExecutorWorker::CompletionDelivery
{
		LuaCompletionTarget                                 target;
		std::function<void(const LuaBatchDispatchResult &)> completion;
		LuaBatchDispatchResult                              result;
		QSharedPointer<LuaDeferredRuntimeMutationDelivery>  mutationDelivery;
		LuaDeferredRuntimeMutationConsumer                  recoveryConsumer;
};

struct LuaExecutorWorker::CompletionDeliveryTicket
{
		std::unique_ptr<CompletionDelivery> delivery;
		bool                                resolved{false};
};

struct LuaExecutorWorker::CompletionDeliverySequencer final
    : std::enable_shared_from_this<LuaExecutorWorker::CompletionDeliverySequencer>
{
		[[nodiscard]] std::shared_ptr<CompletionDeliveryTicket> reserve()
		{
			const auto   ticket = std::make_shared<CompletionDeliveryTicket>();
			QMutexLocker locker(&mutex);
			pending.enqueue(ticket);
			return ticket;
		}

		void resolve(const std::shared_ptr<CompletionDeliveryTicket> &ticket, CompletionDelivery delivery)
		{
			bool startDelivery = false;
			{
				QMutexLocker locker(&mutex);
				if (!ticket || ticket->resolved)
					return;
				ticket->delivery = std::make_unique<CompletionDelivery>(std::move(delivery));
				ticket->resolved = true;
				if (!deliveryActive && !pending.isEmpty() && pending.head()->resolved)
				{
					deliveryActive = true;
					startDelivery  = true;
				}
			}
			if (startDelivery)
				deliverNext();
		}

		void skip(const std::shared_ptr<CompletionDeliveryTicket> &ticket) noexcept
		{
			static_cast<void>(runWorkerOperationSafely(
			    QStringLiteral("resolving an abandoned asynchronous completion"),
			    [this, &ticket]
			    {
				    bool startDelivery = false;
				    {
					    QMutexLocker locker(&mutex);
					    if (!ticket || ticket->resolved)
						    return;
					    ticket->delivery.reset();
					    ticket->resolved = true;
					    if (!deliveryActive && !pending.isEmpty() && pending.head()->resolved)
					    {
						    deliveryActive = true;
						    startDelivery  = true;
					    }
				    }
				    if (startDelivery)
					    deliverNext();
			    }));
		}

	private:
		struct DeliveryContinuation
		{
				QMutex mutex;
				bool   deliveryCallReturned{false};
				bool   deliveryFinished{false};
		};

		void deliverNext()
		{
			for (;;)
			{
				CompletionDelivery delivery;
				{
					QMutexLocker locker(&mutex);
					if (pending.isEmpty() || !pending.head()->resolved)
					{
						deliveryActive = false;
						return;
					}
					const std::shared_ptr<CompletionDeliveryTicket> ticket = pending.dequeue();
					if (!ticket->delivery)
						continue;
					delivery = std::move(*ticket->delivery);
					ticket->delivery.reset();
				}
				const auto continuation = std::make_shared<DeliveryContinuation>();
				const QSharedPointer<LuaDeferredRuntimeMutationDelivery> mutationDelivery =
				    delivery.mutationDelivery;
				const LuaDeferredRuntimeMutationConsumer recoveryConsumer = delivery.recoveryConsumer;
				const auto completionExecuted = std::make_shared<std::atomic_bool>(false);
				const auto orderedCompletion  = [completion = std::move(delivery.completion),
				                                 completionExecuted](const LuaBatchDispatchResult &result)
				{
					if (completion)
						completion(result);
					completionExecuted->store(true, std::memory_order_release);
				};
				const std::shared_ptr<CompletionDeliverySequencer> self = shared_from_this();
				qmudDeliverLuaCompletion(
				    delivery.target, orderedCompletion, std::move(delivery.result),
				    LuaCompletionDeliveryMode::AlwaysQueued,
				    [self, continuation, mutationDelivery, recoveryConsumer, completionExecuted]
				    {
					    if (!completionExecuted->load(std::memory_order_acquire))
						    recoverUndeliveredMutationSafely(mutationDelivery, recoveryConsumer);
					    bool continueAsynchronously = false;
					    {
						    QMutexLocker locker(&continuation->mutex);
						    continuation->deliveryFinished = true;
						    continueAsynchronously         = continuation->deliveryCallReturned;
					    }
					    if (continueAsynchronously)
						    self->deliverNext();
				    });
				bool continueSynchronously = false;
				{
					QMutexLocker locker(&continuation->mutex);
					continuation->deliveryCallReturned = true;
					continueSynchronously              = continuation->deliveryFinished;
				}
				if (!continueSynchronously)
					return;
			}
		}

		QMutex                                            mutex;
		QQueue<std::shared_ptr<CompletionDeliveryTicket>> pending;
		bool                                              deliveryActive{false};
};

struct LuaExecutorWorker::QueuedDispatchRequest
{
		LuaBatchDispatchRequest                             request;
		LuaBatchDispatchResult                              fallback;
		bool                                                needsResult{false};
		LuaCompletionTarget                                 completionTarget;
		std::function<void(const LuaBatchDispatchResult &)> completion;
		std::shared_ptr<CompletionDeliveryTicket>           completionTicket;
		QPointer<QThread>                                   waiterThread;
		bool                                                started{false};
		bool                                                canceled{false};
		bool                                                completed{false};
		LuaBatchDispatchResult                              result;
		QMutex                                              mutex;
		QWaitCondition                                      wake;
};

LuaExecutorWorker::LuaExecutorWorker(LuaDeferredRuntimeMutationConsumer shutdownMutationConsumer)
    : m_deferredMutationDeliveryRegistry(QSharedPointer<DeferredMutationDeliveryRegistry>::create()),
      m_completionDeliverySequencer(std::make_shared<CompletionDeliverySequencer>()),
      m_shutdownMutationConsumer(std::move(shutdownMutationConsumer))
{
	if (!m_shutdownMutationConsumer)
		qFatal("LuaExecutorWorker requires a shutdown mutation consumer");
	m_controlLane.name  = "control";
	m_callbackLane.name = "callback";
	initializeWorkerLane(m_controlLane, QStringLiteral("QMudLuaExecutorControlWorker"));
	initializeWorkerLane(m_callbackLane, QStringLiteral("QMudLuaExecutorCallbackWorker"));
}

LuaExecutorWorker::~LuaExecutorWorker()
{
	shutdownWorker();
}

const char *LuaExecutorWorker::laneName(const WorkerLaneState &lane)
{
	return lane.name ? lane.name : "unknown";
}

LuaExecutorWorker::WorkerLaneState &
LuaExecutorWorker::laneStateForRequest(const LuaBatchDispatchRequest &request) const
{
	if (request.lane == LuaBatchDispatchLane::Control)
	{
		static std::atomic_bool warnedUnsupportedControlLane{false};
		if (!warnedUnsupportedControlLane.exchange(true, std::memory_order_acq_rel))
		{
			qWarning().noquote() << QStringLiteral(
			                            "[QMud][LuaExecutor] Control lane requested for Lua-state dispatch "
			                            "kind=%1; forcing callback lane to preserve serialization")
			                            .arg(static_cast<int>(request.kind));
		}
	}
	return m_callbackLane;
}

qsizetype LuaExecutorWorker::retainedEngineIndex(WorkerLaneState &lane, LuaCallbackEngine *engine)
{
	if (!engine)
		return -1;
	const auto cached = lane.retainedEngineIndexes.constFind(engine);
	if (cached == lane.retainedEngineIndexes.constEnd())
		return -1;
	if (cached.value() >= 0 && cached.value() < lane.retainedEngines.size() &&
	    lane.retainedEngines.at(cached.value()).data() == engine)
	{
		return cached.value();
	}
	for (qsizetype index = 0; index < lane.retainedEngines.size(); ++index)
	{
		if (lane.retainedEngines.at(index).data() != engine)
			continue;
		lane.retainedEngineIndexes.insert(engine, index);
		return index;
	}
	lane.retainedEngineIndexes.remove(engine);
	return -1;
}

void LuaExecutorWorker::retainEngine(WorkerLaneState &lane, const QSharedPointer<LuaCallbackEngine> &engine)
{
	if (!engine || retainedEngineIndex(lane, engine.data()) >= 0)
		return;
	lane.retainedEngineIndexes.insert(engine.data(), lane.retainedEngines.size());
	lane.retainedEngines.push_back(engine);
}

void LuaExecutorWorker::releaseRetainedEngine(WorkerLaneState &lane, LuaCallbackEngine *engine)
{
	const qsizetype index = retainedEngineIndex(lane, engine);
	if (index < 0)
		return;
	lane.retainedEngines.removeAt(index);
	lane.retainedEngineIndexes.remove(engine);
	for (qsizetype shifted = index; shifted < lane.retainedEngines.size(); ++shifted)
	{
		const QSharedPointer<LuaCallbackEngine> &shiftedEngine = lane.retainedEngines.at(shifted);
		if (shiftedEngine)
			lane.retainedEngineIndexes.insert(shiftedEngine.data(), shifted);
	}
}

void LuaExecutorWorker::initializeWorkerLane(WorkerLaneState &lane, const QString &threadName)
{
	lane.thread = std::make_unique<QThread>();
	lane.thread->setObjectName(threadName);
	lane.invoker = std::make_unique<QObject>();
	lane.invoker->moveToThread(lane.thread.get());
	lane.thread->start();
	if (!ensureWorkerReady(lane.invoker.get(), lane.thread.get(), &lane.bridgeReady))
	{
		qWarning().noquote() << QStringLiteral("[QMud][LuaExecutor] %1 worker thread failed to start")
		                            .arg(QString::fromUtf8(laneName(lane)));
	}
}

LuaBatchDispatchResult LuaExecutorWorker::dispatchBatch(const LuaBatchDispatchRequest &request) const
{
	WorkerLaneState       &lane     = laneStateForRequest(request);
	LuaBatchDispatchResult fallback = fallbackBatchDispatchResult(request);
	const auto             cancelQueuedBeforeStart =
	    [&lane](const QSharedPointer<QueuedDispatchRequest> &queuedRequest) -> bool
	{
		if (!queuedRequest)
			return false;
		{
			QMutexLocker queueLocker(&lane.dispatchQueueMutex);
			static_cast<void>(lane.dispatchQueue.removeOne(queuedRequest));
		}
		bool canceled = false;
		{
			QMutexLocker locker(&queuedRequest->mutex);
			if (queuedRequest->completed || queuedRequest->started)
				return false;
			queuedRequest->canceled  = true;
			queuedRequest->completed = true;
			queuedRequest->result    = queuedRequest->fallback;
			queuedRequest->wake.wakeAll();
			canceled = true;
		}
		return canceled;
	};

	if (!lane.thread)
		return fallback;
	if (QThread::currentThread() == lane.thread.get())
		return dispatchOnWorkerLane(lane, request);
	if (!lane.invoker || !ensureWorkerReady(lane.invoker.get(), lane.thread.get(), &lane.bridgeReady))
		return fallback;
	if (m_workerShuttingDown.load(std::memory_order_acquire))
		return fallback;
	const bool     bridgeReentrantCaller = qmudLuaBridgeIsExecutingRequestOnCurrentThread();
	const bool     useCallbackLane       = &lane == &m_callbackLane;
	QObject *const laneInvoker = useCallbackLane ? m_callbackLane.invoker.get() : m_controlLane.invoker.get();
	bool           bridgeDriveFailed             = false;
	const auto     driveLaneOnceForBridgeReentry = [&]() -> bool
	{
		if (!bridgeReentrantCaller || bridgeDriveFailed || !laneInvoker)
			return false;
		bool       ranWork = false;
		const bool bridged =
		    qmudLuaBridgeInvokeOnObjectThread(laneInvoker,
		                                      [this, useCallbackLane, &ranWork]
		                                      {
			                                      WorkerLaneState &targetLane =
			                                          useCallbackLane ? m_callbackLane : m_controlLane;
			                                      ranWork = processOneQueuedDispatch(targetLane);
		                                      });
		if (bridged)
			return ranWork;
		bridgeDriveFailed = true;
		qWarning().noquote() << QStringLiteral("%1: %2-lane bridge re-entry queue drive failed: %3")
		                            .arg(kWorkerDispatchFailurePrefix, QString::fromUtf8(laneName(lane)),
		                                 qmudLuaBridgeLastError());
		return false;
	};

	const auto queuedRequest    = QSharedPointer<QueuedDispatchRequest>::create();
	queuedRequest->request      = request;
	queuedRequest->fallback     = fallback;
	queuedRequest->needsResult  = true;
	queuedRequest->waiterThread = QThread::currentThread();
	if (!enqueueQueuedDispatch(lane, queuedRequest))
		return queuedRequest->fallback;

	const qint64   invokeTimeoutMs = qMax<qint64>(1, qmudLuaBridgeInvokeTimeoutMs());
	QDeadlineTimer deadline(invokeTimeoutMs);
	for (;;)
	{
		{
			QMutexLocker locker(&queuedRequest->mutex);
			if (queuedRequest->completed)
				return queuedRequest->result;
		}
		if (!lane.thread || !lane.thread->isRunning())
		{
			if (cancelQueuedBeforeStart(queuedRequest))
			{
				qWarning().noquote() << QStringLiteral(
				                            "%1: canceled queued %2-lane worker dispatch because lane thread "
				                            "stopped before execution")
				                            .arg(kWorkerDispatchFailurePrefix,
				                                 QString::fromUtf8(laneName(lane)));
				return queuedRequest->fallback;
			}
			break;
		}
		if (deadline.hasExpired())
			break;
		if (qmudLuaBridgePumpCurrentThreadOnce())
			continue;
		if (driveLaneOnceForBridgeReentry())
			continue;
		{
			QMutexLocker locker(&queuedRequest->mutex);
			if (queuedRequest->completed)
				return queuedRequest->result;
		}

		const qint64 remainingMs  = qMax<qint64>(1, deadline.remainingTime());
		const int    bridgeWaitMs = static_cast<int>(qMin<qint64>(remainingMs, 100));
		static_cast<void>(qmudLuaBridgeWaitForCurrentThreadWake(bridgeWaitMs));
	}
	{
		QMutexLocker locker(&queuedRequest->mutex);
		if (queuedRequest->completed)
			return queuedRequest->result;
	}
	qWarning().noquote() << QStringLiteral("%1: queued %2-lane worker dispatch exceeded %3 ms; waiting "
	                                       "for completion (%4)")
	                            .arg(kWorkerDispatchFailurePrefix, QString::fromUtf8(laneName(lane)))
	                            .arg(invokeTimeoutMs)
	                            .arg(dispatchRequestDebugLabel(request));
	for (;;)
	{
		{
			QMutexLocker locker(&queuedRequest->mutex);
			if (queuedRequest->completed)
				return queuedRequest->result;
		}
		if (m_workerShuttingDown.load(std::memory_order_acquire) || !lane.thread || !lane.thread->isRunning())
		{
			const bool canceled = cancelQueuedBeforeStart(queuedRequest);
			qWarning().noquote()
			    << QStringLiteral("%1: %2-lane worker stopped while waiting for queued dispatch completion")
			           .arg(kWorkerDispatchFailurePrefix, QString::fromUtf8(laneName(lane)));
			Q_UNUSED(canceled);
			return queuedRequest->fallback;
		}
		if (qmudLuaBridgePumpCurrentThreadOnce())
			continue;
		if (driveLaneOnceForBridgeReentry())
			continue;
		static_cast<void>(qmudLuaBridgeWaitForCurrentThreadWake(100));
	}
	Q_UNREACHABLE_RETURN(queuedRequest->fallback);
}

void LuaExecutorWorker::dispatchBatchAsync(
    const LuaBatchDispatchRequest &request, QObject *completionTarget,
    const std::function<void(const LuaBatchDispatchResult &)> &completion) const
{
	if (!completion)
		return;
	const LuaCompletionTarget capturedCompletionTarget = qmudCaptureLuaCompletionTarget(completionTarget);
	WorkerLaneState          &lane                     = laneStateForRequest(request);
	LuaBatchDispatchResult    fallback                 = fallbackBatchDispatchResult(request);
	const std::shared_ptr<CompletionDeliveryTicket> completionTicket =
	    m_completionDeliverySequencer ? m_completionDeliverySequencer->reserve() : nullptr;

	if (!lane.thread)
	{
		deliverCompletion(completionTicket, capturedCompletionTarget, completion, std::move(fallback));
		return;
	}
	if (QThread::currentThread() == lane.thread.get())
	{
		LuaBatchDispatchResult result = dispatchOnWorkerLaneSafely(lane, request, std::move(fallback));
		retainAsyncMutationBackup(result);
		deliverCompletion(completionTicket, capturedCompletionTarget, completion, std::move(result));
		return;
	}
	if (!lane.invoker || !ensureWorkerReady(lane.invoker.get(), lane.thread.get(), &lane.bridgeReady))
	{
		deliverCompletion(completionTicket, capturedCompletionTarget, completion, std::move(fallback));
		return;
	}
	if (m_workerShuttingDown.load(std::memory_order_acquire))
	{
		deliverCompletion(completionTicket, capturedCompletionTarget, completion, std::move(fallback));
		return;
	}

	const auto queuedRequest        = QSharedPointer<QueuedDispatchRequest>::create();
	queuedRequest->request          = request;
	queuedRequest->fallback         = fallback;
	queuedRequest->needsResult      = false;
	queuedRequest->completionTarget = capturedCompletionTarget;
	queuedRequest->completion       = completion;
	queuedRequest->completionTicket = completionTicket;
	if (!enqueueQueuedDispatch(lane, queuedRequest))
		deliverCompletion(queuedRequest->completionTicket, queuedRequest->completionTarget,
		                  queuedRequest->completion, std::move(queuedRequest->fallback));
}

void LuaExecutorWorker::deliverCompletion(
    const std::shared_ptr<CompletionDeliveryTicket> &ticket, const LuaCompletionTarget &target,
    const std::function<void(const LuaBatchDispatchResult &)> &completion, LuaBatchDispatchResult result,
    const LuaCompletionDeliveryMode mode) const noexcept
{
	auto skipUnresolvedTicket = qScopeGuard(
	    [sequencer = m_completionDeliverySequencer, ticket]
	    {
		    if (sequencer && ticket)
			    sequencer->skip(ticket);
	    });
	if (!ticket || !m_completionDeliverySequencer)
	{
		static_cast<void>(runWorkerOperationSafely(
		    QStringLiteral("delivering an asynchronous completion"),
		    [&] { qmudDeliverLuaCompletion(target, completion, std::move(result), mode); }));
		return;
	}
	if (!target.supplied && mode == LuaCompletionDeliveryMode::DirectWhenOnTargetThread)
	{
		const QSharedPointer<LuaDeferredRuntimeMutationDelivery> mutationDelivery =
		    result.deferredRuntimeMutationDelivery;
		const bool completed =
		    runWorkerOperationSafely(QStringLiteral("delivering a targetless asynchronous completion"),
		                             [&]
		                             {
			                             qmudDeliverLuaCompletion(qmudCaptureLuaCompletionTarget(nullptr),
			                                                      completion, std::move(result));
		                             });
		if (!completed)
			recoverUndeliveredMutationSafely(mutationDelivery, m_shutdownMutationConsumer);
		return;
	}
	const bool resolved =
	    runWorkerOperationSafely(QStringLiteral("sequencing an asynchronous completion"),
	                             [&]
	                             {
		                             CompletionDelivery delivery;
		                             delivery.target =
		                                 target.supplied ? target : qmudCaptureLuaCompletionTarget(nullptr);
		                             delivery.completion       = completion;
		                             delivery.mutationDelivery = result.deferredRuntimeMutationDelivery;
		                             delivery.recoveryConsumer = m_shutdownMutationConsumer;
		                             delivery.result           = std::move(result);
		                             m_completionDeliverySequencer->resolve(ticket, std::move(delivery));
	                             });
	if (resolved)
		skipUnresolvedTicket.dismiss();
}

bool LuaExecutorWorker::enqueueQueuedDispatch(WorkerLaneState                             &lane,
                                              const QSharedPointer<QueuedDispatchRequest> &request) const
{
	if (!request || !lane.invoker)
		return false;
	bool schedulePump = false;
	{
		QMutexLocker locker(&lane.dispatchQueueMutex);
		if (m_workerShuttingDown.load(std::memory_order_acquire))
			return false;
		lane.dispatchQueue.enqueue(request);
		if (!lane.dispatchQueuePumpQueued)
		{
			lane.dispatchQueuePumpQueued = true;
			schedulePump                 = true;
		}
	}
	if (lane.thread)
		qmudLuaBridgeNotifyThreadWake(lane.thread.get());
	if (!schedulePump)
		return true;
	const bool     useCallbackLane = &lane == &m_callbackLane;
	QObject *const laneInvoker = useCallbackLane ? m_callbackLane.invoker.get() : m_controlLane.invoker.get();
	if (!laneInvoker)
		return false;
	const bool queued = QMetaObject::invokeMethod(
	    laneInvoker,
	    [this, useCallbackLane]
	    {
		    WorkerLaneState &targetLane = useCallbackLane ? m_callbackLane : m_controlLane;
		    processQueuedDispatches(targetLane);
	    },
	    Qt::QueuedConnection);
	if (queued)
		return true;

	lane.bridgeReady.store(false, std::memory_order_release);
	qWarning().noquote() << QStringLiteral("%1: failed to queue %2-lane worker dispatch pump")
	                            .arg(kWorkerDispatchFailurePrefix, QString::fromUtf8(laneName(lane)));

	QMutexLocker locker(&lane.dispatchQueueMutex);
	lane.dispatchQueuePumpQueued = false;
	const bool removed           = lane.dispatchQueue.removeOne(request);
	if (!removed)
		return false;
	if (!request->needsResult)
		return false;
	QMutexLocker requestLocker(&request->mutex);
	request->completed = true;
	request->result    = request->fallback;
	request->wake.wakeAll();
	qmudLuaBridgeNotifyThreadWake(request->waiterThread.data());
	return false;
}

LuaBatchDispatchResult LuaExecutorWorker::dispatchOnWorkerLane(WorkerLaneState               &lane,
                                                               const LuaBatchDispatchRequest &request) const
{
	if (request.kind == LuaBatchDispatchKind::InitializeEnginesWithObservedCallbacksMany &&
	    request.initRequestsArg)
	{
		for (const LuaEngineObservedInitializationRequest &initRequest : *request.initRequestsArg)
		{
			if (initRequest.engine && initRequest.workerLifetimeOwner &&
			    initRequest.workerLifetimeOwner.data() == initRequest.engine)
			{
				retainEngine(lane, initRequest.workerLifetimeOwner);
			}
		}
	}

	LuaBatchDispatchResult result = m_direct.dispatchBatch(request);
	if (request.kind == LuaBatchDispatchKind::TeardownEnginesMany)
	{
		for (const QSharedPointer<LuaCallbackEngine> &engine : request.engines)
		{
			if (engine)
				releaseRetainedEngine(lane, engine.data());
		}
	}
	return result;
}

LuaBatchDispatchResult
LuaExecutorWorker::dispatchOnWorkerLaneSafely(WorkerLaneState &lane, const LuaBatchDispatchRequest &request,
                                              LuaBatchDispatchResult fallback) const noexcept
{
	static_cast<void>(runWorkerOperationSafely(QStringLiteral("executing a worker dispatch"),
	                                           [&] { fallback = dispatchOnWorkerLane(lane, request); }));
	return fallback;
}

void LuaExecutorWorker::retainAsyncMutationBackup(LuaBatchDispatchResult &result) const noexcept
{
	if (result.deferredRuntimeMutationBatches.isEmpty() || result.deferredRuntimeMutationDelivery ||
	    !m_deferredMutationDeliveryRegistry)
	{
		return;
	}
	static_cast<void>(runWorkerOperationSafely(
	    QStringLiteral("retaining an asynchronous mutation journal"),
	    [&]
	    {
		    const QSharedPointer<DeferredMutationDeliveryRegistry> registry =
		        m_deferredMutationDeliveryRegistry;
		    const quint64                                        id = registry->reserveId();
		    const QWeakPointer<DeferredMutationDeliveryRegistry> weakRegistry(registry);
		    auto delivery = QSharedPointer<LuaDeferredRuntimeMutationDelivery>::create(
		        result.deferredRuntimeMutationBatches,
		        [weakRegistry, id](const LuaDeferredRuntimeMutationDelivery::DeliveryAction &consumer)
		        {
			        if (const QSharedPointer<DeferredMutationDeliveryRegistry> locked =
			                weakRegistry.toStrongRef())
			        {
				        return locked->consumeDelivery(id, consumer);
			        }
			        return false;
		        });
		    registry->insert(id, delivery);
		    result.deferredRuntimeMutationDelivery = std::move(delivery);
	    }));
}

bool LuaExecutorWorker::processOneQueuedDispatch(WorkerLaneState &lane) const
{
	if (!lane.thread || QThread::currentThread() != lane.thread.get())
		return false;

	QSharedPointer<QueuedDispatchRequest> request;
	{
		QMutexLocker locker(&lane.dispatchQueueMutex);
		if (lane.dispatchQueue.isEmpty())
			return false;
		request = lane.dispatchQueue.dequeue();
	}

	if (!request)
		return true;
	{
		QMutexLocker requestLocker(&request->mutex);
		if (request->completed || request->canceled)
		{
			if (request->needsResult && !request->completed)
			{
				request->completed = true;
				request->result    = request->fallback;
				request->wake.wakeAll();
			}
			return true;
		}
		request->started = true;
	}

	bool       terminallyCompleted = false;
	const auto completeOnExit      = qScopeGuard(
	    [&]
	    {
		    if (terminallyCompleted)
			    return;
		    if (!request->needsResult)
		    {
			    deliverCompletion(request->completionTicket, request->completionTarget, request->completion,
			                      request->fallback);
			    return;
		    }
		    {
			    QMutexLocker requestLocker(&request->mutex);
			    if (!request->completed)
			    {
				    request->result    = request->fallback;
				    request->completed = true;
				    request->wake.wakeAll();
			    }
		    }
		    qmudLuaBridgeNotifyThreadWake(request->waiterThread.data());
	    });

	LuaBatchDispatchResult result = dispatchOnWorkerLaneSafely(lane, request->request, request->fallback);
	if (!request->needsResult)
		retainAsyncMutationBackup(result);
	if (!request->needsResult)
	{
		deliverCompletion(request->completionTicket, request->completionTarget, request->completion,
		                  std::move(result));
		terminallyCompleted = true;
		return true;
	}
	{
		QMutexLocker requestLocker(&request->mutex);
		if (!request->completed)
		{
			request->result    = result;
			request->completed = true;
			request->wake.wakeAll();
		}
	}
	qmudLuaBridgeNotifyThreadWake(request->waiterThread.data());
	terminallyCompleted = true;
	return true;
}

void LuaExecutorWorker::processQueuedDispatches(WorkerLaneState &lane) const
{
	for (;;)
	{
		bool processed = false;
		static_cast<void>(runWorkerOperationSafely(QStringLiteral("draining the worker dispatch queue"),
		                                           [&] { processed = processOneQueuedDispatch(lane); }));
		if (processed)
			continue;

		QMutexLocker locker(&lane.dispatchQueueMutex);
		if (!lane.dispatchQueue.isEmpty())
			continue;
		lane.dispatchQueuePumpQueued = false;
		return;
	}
}

void LuaExecutorWorker::shutdownWorker() const
{
	m_workerShuttingDown.store(true, std::memory_order_release);
	QVector<QSharedPointer<QueuedDispatchRequest>> pendingRequests;
	QVector<LuaDeferredRuntimeMutationBatch>       teardownBatches =
	    shutdownWorkerLane(m_controlLane, pendingRequests);
	teardownBatches += shutdownWorkerLane(m_callbackLane, pendingRequests);
	if (m_deferredMutationDeliveryRegistry)
	{
		m_deferredMutationDeliveryRegistry->consumeAllForRecovery(std::move(teardownBatches),
		                                                          m_shutdownMutationConsumer);
	}
	else if (!teardownBatches.isEmpty() && m_shutdownMutationConsumer)
	{
		m_shutdownMutationConsumer(std::move(teardownBatches));
	}
	deliverShutdownFallbacks(pendingRequests);
}

QVector<LuaDeferredRuntimeMutationBatch> LuaExecutorWorker::shutdownWorkerLane(
    WorkerLaneState &lane, QVector<QSharedPointer<QueuedDispatchRequest>> &shutdownPendingRequests) const
{
	QVector<QSharedPointer<QueuedDispatchRequest>> pendingRequests;
	{
		QMutexLocker locker(&lane.dispatchQueueMutex);
		pendingRequests.reserve(lane.dispatchQueue.size());
		while (!lane.dispatchQueue.isEmpty())
		{
			pendingRequests.push_back(lane.dispatchQueue.dequeue());
		}
		lane.dispatchQueuePumpQueued = false;
	}
	QVector<LuaDeferredRuntimeMutationBatch> teardownBatches;
	bool                                     cleaned = false;
	if (lane.invoker && lane.thread && lane.thread->isRunning())
	{
		if (QThread::currentThread() == lane.thread.get())
			qFatal("LuaExecutorWorker cannot be destroyed from its own %s lane", laneName(lane));

		struct ShutdownCleanupState
		{
				QMutex                                   mutex;
				QWaitCondition                           wake;
				QVector<LuaDeferredRuntimeMutationBatch> batches;
				bool                                     done{false};
				bool                                     succeeded{false};
		};
		const auto state  = std::make_shared<ShutdownCleanupState>();
		const bool queued = QMetaObject::invokeMethod(
		    lane.invoker.get(),
		    [this, &lane, &pendingRequests, state]
		    {
			    QVector<LuaDeferredRuntimeMutationBatch> batches;
			    bool                                     succeeded = false;
			    try
			    {
				    for (const QSharedPointer<QueuedDispatchRequest> &request : pendingRequests)
				    {
					    if (!request)
						    continue;
					    request->request.engines.clear();
					    request->request.initRequestsArg.clear();
				    }
				    LuaBatchDispatchRequest teardownRequest;
				    teardownRequest.kind = LuaBatchDispatchKind::TeardownEnginesMany;
				    teardownRequest.engines.reserve(lane.retainedEngines.size());
				    for (const QSharedPointer<LuaCallbackEngine> &engine : lane.retainedEngines)
					    teardownRequest.engines.push_back(engine);
				    LuaBatchDispatchResult result = dispatchOnWorkerLane(lane, teardownRequest);
				    batches += std::move(result.deferredRuntimeMutationBatches);
				    succeeded = true;
			    }
			    catch (...)
			    {
			    }
			    QMutexLocker locker(&state->mutex);
			    state->batches   = std::move(batches);
			    state->succeeded = succeeded;
			    state->done      = true;
			    state->wake.wakeAll();
		    },
		    Qt::QueuedConnection);
		while (queued)
		{
			{
				QMutexLocker locker(&state->mutex);
				if (state->done)
				{
					teardownBatches = std::move(state->batches);
					cleaned         = state->succeeded;
					break;
				}
			}
			if (!lane.thread->isRunning())
				break;
			if (qmudLuaBridgePumpCurrentThreadOnce())
				continue;
			QMutexLocker locker(&state->mutex);
			if (!state->done)
				state->wake.wait(&state->mutex, 10);
		}
	}
	else
	{
		bool cleanupRequired = !lane.retainedEngines.isEmpty();
		for (const QSharedPointer<QueuedDispatchRequest> &request : pendingRequests)
		{
			if (!request)
				continue;
			cleanupRequired = cleanupRequired || !request->request.engines.isEmpty();
			if (!request->request.initRequestsArg)
				continue;
			for (const LuaEngineObservedInitializationRequest &initRequest :
			     *request->request.initRequestsArg)
			{
				cleanupRequired = cleanupRequired || !initRequest.workerLifetimeOwner.isNull();
			}
		}
		cleaned = !cleanupRequired;
	}
	if (!cleaned)
	{
		qFatal("LuaExecutorWorker failed to release %s-lane engine ownership on its worker thread",
		       laneName(lane));
	}
	if (lane.invoker)
	{
		QObject *workerObject = lane.invoker.release();
		QThread *ownerThread  = workerObject ? workerObject->thread() : nullptr;
		if (workerObject &&
		    (!ownerThread || ownerThread == QThread::currentThread() || !ownerThread->isRunning()))
		{
			delete workerObject;
		}
		else if (workerObject)
		{
			workerObject->deleteLater();
		}
	}

	if (lane.thread && lane.thread->isRunning())
	{
		lane.thread->quit();
		if (QThread::currentThread() != lane.thread.get())
			lane.thread->wait();
	}
	lane.bridgeReady.store(false, std::memory_order_release);
	shutdownPendingRequests += std::move(pendingRequests);
	return teardownBatches;
}

void LuaExecutorWorker::deliverShutdownFallbacks(
    const QVector<QSharedPointer<QueuedDispatchRequest>> &pendingRequests) const
{
	for (const QSharedPointer<QueuedDispatchRequest> &request : pendingRequests)
	{
		if (!request)
			continue;
		if (request->completion)
		{
			auto completion = std::move(request->completion);
			deliverCompletion(request->completionTicket, request->completionTarget, completion,
			                  request->fallback, LuaCompletionDeliveryMode::AlwaysQueued);
		}
		if (!request->needsResult)
			continue;
		QMutexLocker requestLocker(&request->mutex);
		request->completed = true;
		request->result    = request->fallback;
		request->wake.wakeAll();
		qmudLuaBridgeNotifyThreadWake(request->waiterThread.data());
	}
}
