/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: LuaExecutorWorker.h
 * Role: Worker-thread Lua executor backend.
 */

#ifndef QMUD_LUAEXECUTORWORKER_H
#define QMUD_LUAEXECUTORWORKER_H

#include "LuaExecutor.h"
#include "helpers/LuaCompletionDeliveryUtils.h"

#include <QHash>
#include <QMutex>
// ReSharper disable once CppUnusedIncludeDirective
#include <QObject>
#include <QQueue>
#include <QSharedPointer>
#include <QThread>
#include <atomic>

/**
 * @brief Worker-thread executor backend for Lua callback dispatch.
 *
 * All callback commands are enqueued to the worker. Barrier commands wait for queued
 * completion results, while non-barrier commands return immediately after enqueue.
 */
class LuaExecutorWorker final : public ILuaExecutor
{
	public:
		explicit LuaExecutorWorker(LuaDeferredRuntimeMutationConsumer shutdownMutationConsumer);
		~LuaExecutorWorker() override;

		LuaBatchDispatchResult dispatchBatch(const LuaBatchDispatchRequest &request) const override;
		void                   dispatchBatchAsync(
		    const LuaBatchDispatchRequest &request, QObject *completionTarget,
		    const std::function<void(const LuaBatchDispatchResult &)> &completion) const override;

	private:
		struct DeferredMutationDeliveryRegistry;
		struct CompletionDelivery;
		struct CompletionDeliveryTicket;
		struct CompletionDeliverySequencer;
		struct QueuedDispatchRequest;
		struct WorkerLaneState;
		[[nodiscard]] WorkerLaneState   &laneStateForRequest(const LuaBatchDispatchRequest &request) const;
		[[nodiscard]] static const char *laneName(const WorkerLaneState &lane);
		bool enqueueQueuedDispatch(WorkerLaneState                             &lane,
		                           const QSharedPointer<QueuedDispatchRequest> &request) const;
		[[nodiscard]] LuaBatchDispatchResult
		dispatchOnWorkerLane(WorkerLaneState &lane, const LuaBatchDispatchRequest &request) const;
		[[nodiscard]] LuaBatchDispatchResult
		dispatchOnWorkerLaneSafely(WorkerLaneState &lane, const LuaBatchDispatchRequest &request,
		                           LuaBatchDispatchResult fallback) const noexcept;
		[[nodiscard]] static qsizetype retainedEngineIndex(WorkerLaneState &lane, LuaCallbackEngine *engine);
		static void retainEngine(WorkerLaneState &lane, const QSharedPointer<LuaCallbackEngine> &engine);
		static void releaseRetainedEngine(WorkerLaneState &lane, LuaCallbackEngine *engine);
		void        retainAsyncMutationBackup(LuaBatchDispatchResult &result) const noexcept;
		void        deliverCompletion(const std::shared_ptr<CompletionDeliveryTicket>           &ticket,
		                              const LuaCompletionTarget                                 &target,
		                              const std::function<void(const LuaBatchDispatchResult &)> &completion,
		                              LuaBatchDispatchResult                                     result,
		                              LuaCompletionDeliveryMode                                  mode =
		                                  LuaCompletionDeliveryMode::DirectWhenOnTargetThread) const noexcept;
		bool        processOneQueuedDispatch(WorkerLaneState &lane) const;
		void        processQueuedDispatches(WorkerLaneState &lane) const;
		static void initializeWorkerLane(WorkerLaneState &lane, const QString &threadName);
		[[nodiscard]] QVector<LuaDeferredRuntimeMutationBatch>
		shutdownWorkerLane(WorkerLaneState                                &lane,
		                   QVector<QSharedPointer<QueuedDispatchRequest>> &pendingRequests) const;
		void
		deliverShutdownFallbacks(const QVector<QSharedPointer<QueuedDispatchRequest>> &pendingRequests) const;
		void shutdownWorker() const;

		struct WorkerLaneState
		{
				const char                                   *name{nullptr};
				std::unique_ptr<QThread>                      thread;
				std::unique_ptr<QObject>                      invoker;
				std::atomic_bool                              bridgeReady{false};
				QMutex                                        dispatchQueueMutex;
				QQueue<QSharedPointer<QueuedDispatchRequest>> dispatchQueue;
				bool                                          dispatchQueuePumpQueued{false};
				QVector<QSharedPointer<LuaCallbackEngine>>    retainedEngines;
				QHash<LuaCallbackEngine *, qsizetype>         retainedEngineIndexes;
		};

		mutable WorkerLaneState                          m_controlLane;
		mutable WorkerLaneState                          m_callbackLane;
		mutable std::atomic_bool                         m_workerShuttingDown{false};
		QSharedPointer<DeferredMutationDeliveryRegistry> m_deferredMutationDeliveryRegistry;
		std::shared_ptr<CompletionDeliverySequencer>     m_completionDeliverySequencer;
		LuaDeferredRuntimeMutationConsumer               m_shutdownMutationConsumer;
		LuaExecutorDirect                                m_direct;
};

#endif // QMUD_LUAEXECUTORWORKER_H
