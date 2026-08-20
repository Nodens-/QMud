/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_LuaExecution.cpp
 * Role: Consolidated unit coverage for Lua execution helpers and compatibility shims.
 */

#include "LuaExecutorWorker.h"
#include "LuaSupport.h"
#include "helpers/LuaCompletionDeliveryUtils.h"
#include "helpers/LuaExecutionUtils.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QScopeGuard>
// ReSharper disable once CppUnusedIncludeDirective
#include <QPointer>
#include <QSemaphore>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryDir>
#include <QThread>
#include <QtTest/QTest>

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

extern "C" int         luaopen_lsqlite3(lua_State *L);

LuaBatchDispatchResult ILuaExecutor::dispatchBatch(const LuaBatchDispatchRequest &request) const
{
	LuaBatchDispatchResult result{};
	if (request.kind == LuaBatchDispatchKind::HasFunction)
	{
		result.hasFunction      = false;
		result.hasFunctionValid = true;
	}
	return result;
}

void ILuaExecutor::dispatchBatchAsync(
    const LuaBatchDispatchRequest &request, QObject *completionTarget,
    const std::function<void(const LuaBatchDispatchResult &)> &completion) const
{
	if (!completion)
		return;
	const LuaCompletionTarget target = qmudCaptureLuaCompletionTarget(completionTarget);
	qmudDeliverLuaCompletion(target, completion, dispatchBatch(request));
}

namespace
{
	LuaDeferredRuntimeMutationConsumer recoveredMutationConsumerForTest()
	{
		return [](const QVector<LuaDeferredRuntimeMutationBatch> &batches)
		{
			for (const LuaDeferredRuntimeMutationBatch &batch : batches)
			{
				for (const std::function<void()> &mutation : batch.mutations)
				{
					if (mutation)
						mutation();
				}
			}
		};
	}

	class RawLuaStateOwner final
	{
		public:
			explicit RawLuaStateOwner(lua_State *state) : m_state(state)
			{
			}
			~RawLuaStateOwner()
			{
				if (m_state)
					lua_close(m_state);
			}

			RawLuaStateOwner(const RawLuaStateOwner &)                   = delete;
			RawLuaStateOwner        &operator=(const RawLuaStateOwner &) = delete;

			[[nodiscard]] lua_State *get() const
			{
				return m_state;
			}

		private:
			lua_State *m_state{nullptr};
	};

	bool runLuaChunk(lua_State *state, const QByteArray &chunk)
	{
		if (!state)
			return false;
		if (luaL_loadbuffer(state, chunk.constData(), static_cast<size_t>(chunk.size()), "test") != LUA_OK)
		{
			lua_pop(state, 1);
			return false;
		}
		if (lua_pcall(state, 0, 0, 0) != LUA_OK)
		{
			lua_pop(state, 1);
			return false;
		}
		return true;
	}

	/**
	 * @brief Executes a Lua chunk and returns the first result as UTF-8 string.
	 */
	struct LuaStringEvalResult
	{
			bool    ok{false};
			QString value;
			QString error;
	};

	/**
	 * @brief Creates a Lua state with standard libraries and Lua 5.1 compatibility shims.
	 */
	LuaStateOwner makeCompatLuaState()
	{
		LuaStateOwner state(QMudLuaSupport::makeLuaState());
		if (!state)
			return {};
		luaL_openlibs(state.get());
		QMudLuaSupport::applyLua51Compat(state.get());
		return state;
	}

	LuaStringEvalResult evaluateLuaToString(lua_State *L, const QByteArray &chunk)
	{
		LuaStringEvalResult result;
		if (!L)
		{
			result.error = QStringLiteral("Lua state is null.");
			return result;
		}
		if (luaL_loadbuffer(L, chunk.constData(), static_cast<size_t>(chunk.size()),
		                    "tst_LuaExecution_eval") != 0)
		{
			const char *err = lua_tostring(L, -1);
			result.error    = QString::fromUtf8(err ? err : "unknown load error");
			lua_pop(L, 1);
			return result;
		}
		if (lua_pcall(L, 0, 1, 0) != 0)
		{
			const char *err = lua_tostring(L, -1);
			result.error    = QString::fromUtf8(err ? err : "unknown runtime error");
			lua_pop(L, 1);
			return result;
		}
		if (!lua_isstring(L, -1))
		{
			result.error = QStringLiteral("Lua chunk did not return a string result.");
			lua_pop(L, 1);
			return result;
		}

		size_t      len   = 0;
		const char *bytes = lua_tolstring(L, -1, &len);
		result.ok         = true;
		result.value      = QString::fromUtf8(bytes ? bytes : "", static_cast<int>(len));
		lua_pop(L, 1);
		return result;
	}

	bool executeLuaChunk(lua_State *L, const QByteArray &chunk, QString &error)
	{
		error.clear();
		if (!L)
		{
			error = QStringLiteral("Lua state is null.");
			return false;
		}
		if (luaL_loadbuffer(L, chunk.constData(), static_cast<size_t>(chunk.size()),
		                    "tst_LuaExecution_exec") != 0)
		{
			const char *err = lua_tostring(L, -1);
			error           = QString::fromUtf8(err ? err : "unknown load error");
			lua_pop(L, 1);
			return false;
		}
		if (lua_pcall(L, 0, 0, 0) != 0)
		{
			const char *err = lua_tostring(L, -1);
			error           = QString::fromUtf8(err ? err : "unknown runtime error");
			lua_pop(L, 1);
			return false;
		}
		return true;
	}

	bool openSqliteModule(lua_State *L, QString &error)
	{
		if (!L)
		{
			error = QStringLiteral("Lua state is null.");
			return false;
		}
		if (luaopen_lsqlite3(L) != 1)
		{
			error = QStringLiteral("luaopen_lsqlite3 did not return a module table.");
			return false;
		}
		lua_pop(L, 1);
		return true;
	}

	/**
	 * @brief QTest fixture covering Lua execution helpers and Lua 5.1 compatibility shims.
	 */
	class tst_LuaExecution : public QObject
	{
			Q_OBJECT

			// NOLINTBEGIN(readability-convert-member-functions-to-static)
		private slots:
			void luaBridgeTimeoutOverrideRoundTrip()
			{
				const int  originalTimeoutMs = qmudLuaBridgeInvokeTimeoutMs();
				const auto restoreTimeout =
				    qScopeGuard([originalTimeoutMs] { qmudSetLuaBridgeInvokeTimeoutMs(originalTimeoutMs); });

				QVERIFY(originalTimeoutMs > 0);
				qmudSetLuaBridgeInvokeTimeoutMs(1234);
				QCOMPARE(qmudLuaBridgeInvokeTimeoutMs(), 1234);

				qmudSetLuaBridgeInvokeTimeoutMs(0);
				QVERIFY(qmudLuaBridgeInvokeTimeoutMs() > 0);
				QVERIFY(qmudLuaBridgeInvokeTimeoutMs() != 1234);
			}

			void luaBridgeRejectsInvalidInputs()
			{
				QVERIFY(!qmudLuaBridgeEnsureObjectThreadReady(nullptr));
				QVERIFY(qmudLuaBridgeLastError().contains(QStringLiteral("target object is null")));

				QVERIFY(!qmudLuaBridgeInvokeOnObjectThread(nullptr, [] {}));
				QVERIFY(qmudLuaBridgeLastError().contains(QStringLiteral("invalid target or callback")));

				QObject                     target;
				const std::function<void()> emptyFn;
				QVERIFY(!qmudLuaBridgeInvokeOnObjectThread(&target, emptyFn));
				QVERIFY(qmudLuaBridgeLastError().contains(QStringLiteral("invalid target or callback")));
			}

			void luaBridgeClearsErrorAfterSuccessfulInvoke()
			{
				QVERIFY(!qmudLuaBridgeInvokeOnObjectThread(nullptr, [] {}));
				QVERIFY(!qmudLuaBridgeLastError().isEmpty());

				QObject target;
				bool    invoked = false;
				QVERIFY(qmudLuaBridgeInvokeOnObjectThread(&target, [&invoked] { invoked = true; }));
				QVERIFY(invoked);
				QVERIFY(qmudLuaBridgeLastError().isEmpty());
			}

			void luaWorkerCallbackDispatchClearsInFlightBeforeResultConsumption()
			{
				bool             workerInFlight        = true;
				bool             nestedDispatchAllowed = false;
				std::vector<int> order;

				qmudCompleteLuaWorkerCallbackDispatch(
				    workerInFlight,
				    [&]
				    {
					    order.push_back(1);
					    nestedDispatchAllowed = !workerInFlight;
				    },
				    [&] { order.push_back(2); });

				QVERIFY(!workerInFlight);
				QVERIFY(nestedDispatchAllowed);
				QCOMPARE(order.size(), static_cast<size_t>(2));
				QCOMPARE(order.at(0), 1);
				QCOMPARE(order.at(1), 2);
			}

			void luaBridgeInvokesOnWorkerThread()
			{
				QThread workerThread;
				workerThread.setObjectName(QStringLiteral("tst_LuaBridgeWorker"));
				QObject target;
				target.moveToThread(&workerThread);
				workerThread.start();
				const auto stopWorker = qScopeGuard(
				    [&]()
				    {
					    if (target.thread() == &workerThread)
					    {
						    QThread *const mainThread = QThread::currentThread();
						    static_cast<void>(qmudLuaBridgeInvokeOnObjectThread(
						        &target, [&target, mainThread] { target.moveToThread(mainThread); }));
					    }
					    workerThread.quit();
					    static_cast<void>(workerThread.wait());
				    });

				QVERIFY(qmudLuaBridgeEnsureObjectThreadReady(&target));

				std::atomic_bool invokedOnWorker{false};
				std::atomic_int  invokeCount{0};
				QVERIFY(qmudLuaBridgeInvokeOnObjectThread(&target,
				                                          [&workerThread, &invokedOnWorker, &invokeCount]
				                                          {
					                                          invokeCount.fetch_add(1);
					                                          invokedOnWorker.store(
					                                              QThread::currentThread() == &workerThread);
				                                          }));

				QCOMPARE(invokeCount.load(), 1);
				QVERIFY(invokedOnWorker.load());
			}

			void luaBridgeSupportsNestedWorkerToMainInvoke()
			{
				QObject  mainTarget;
				QThread *mainThread = QThread::currentThread();

				QThread  workerThread;
				workerThread.setObjectName(QStringLiteral("tst_LuaBridgeNestedWorker"));
				QObject workerTarget;
				workerTarget.moveToThread(&workerThread);
				workerThread.start();
				const auto stopWorker = qScopeGuard(
				    [&]()
				    {
					    if (workerTarget.thread() == &workerThread)
					    {
						    static_cast<void>(qmudLuaBridgeInvokeOnObjectThread(
						        &workerTarget,
						        [&workerTarget, mainThread] { workerTarget.moveToThread(mainThread); }));
					    }
					    workerThread.quit();
					    static_cast<void>(workerThread.wait());
				    });

				QVERIFY(qmudLuaBridgeEnsureObjectThreadReady(&workerTarget));

				std::atomic_bool outerInvoked{false};
				std::atomic_bool nestedInvokeOk{false};
				std::atomic_bool outerOnWorker{false};
				std::atomic_bool innerInvoked{false};
				std::atomic_bool innerOnMain{false};

				const bool       invokeOk = qmudLuaBridgeInvokeOnObjectThread(
				    &workerTarget,
				    [&]()
				    {
					    outerInvoked.store(true);
					    outerOnWorker.store(QThread::currentThread() == &workerThread);
					    const bool innerOk = qmudLuaBridgeInvokeOnObjectThread(
					        &mainTarget,
					        [&]()
					        {
						        innerInvoked.store(true);
						        innerOnMain.store(QThread::currentThread() == mainThread);
					        });
					    nestedInvokeOk.store(innerOk);
				    });

				QVERIFY(invokeOk);
				QVERIFY(outerInvoked.load());
				QVERIFY(outerOnWorker.load());
				QVERIFY(innerInvoked.load());
				QVERIFY(innerOnMain.load());
				QVERIFY(nestedInvokeOk.load());
			}

			void luaBridgeWaitWorkPumpBreaksWorkerMainWaitCycle()
			{
				QObject  mainTarget;
				QThread *mainThread = QThread::currentThread();

				QThread  workerThread;
				workerThread.setObjectName(QStringLiteral("tst_LuaBridgeWaitWorkPumpWorker"));
				QObject workerTarget;
				workerTarget.moveToThread(&workerThread);
				workerThread.start();
				const auto stopWorker = qScopeGuard(
				    [&]()
				    {
					    if (workerTarget.thread() == &workerThread)
					    {
						    static_cast<void>(qmudLuaBridgeInvokeOnObjectThread(
						        &workerTarget,
						        [&workerTarget, mainThread] { workerTarget.moveToThread(mainThread); }));
					    }
					    workerThread.quit();
					    static_cast<void>(workerThread.wait());
				    });

				QVERIFY(qmudLuaBridgeEnsureObjectThreadReady(&workerTarget));
				QVERIFY(qmudLuaBridgeEnsureObjectThreadReady(&mainTarget));

				std::mutex                        workMutex;
				std::deque<std::function<void()>> workQueue;
				std::mutex                        doneMutex;
				std::condition_variable           doneCv;
				bool                              workDone = false;

				QVERIFY(qmudLuaBridgeInvokeOnObjectThread(&workerTarget,
				                                          [&]()
				                                          {
					                                          qmudLuaBridgeSetCurrentThreadWaitWorkPump(
					                                              [&]() -> bool
					                                              {
						                                              std::function<void()> work;
						                                              {
							                                              std::lock_guard lock(workMutex);
							                                              if (workQueue.empty())
								                                              return false;
							                                              work = std::move(workQueue.front());
							                                              workQueue.pop_front();
						                                              }
						                                              if (work)
							                                              work();
						                                              return true;
					                                              });
				                                          }));
				const auto clearPump = qScopeGuard(
				    [&]()
				    {
					    static_cast<void>(qmudLuaBridgeInvokeOnObjectThread(
					        &workerTarget, [] { qmudLuaBridgeClearCurrentThreadWaitWorkPump(); }));
				    });

				bool outerInvokeOk = false;
				bool innerInvokeOk = false;
				bool waitSucceeded = false;
				outerInvokeOk      = qmudLuaBridgeInvokeOnObjectThread(
				    &workerTarget,
				    [&]()
				    {
					    innerInvokeOk = qmudLuaBridgeInvokeOnObjectThread(
					        &mainTarget,
					        [&]()
					        {
						        {
							        std::lock_guard lock(workMutex);
							        workQueue.emplace_back(
							            [&]()
							            {
								            std::lock_guard doneLock(doneMutex);
								            workDone = true;
								            doneCv.notify_all();
							            });
						        }
						        qmudLuaBridgeNotifyThreadWake(&workerThread);
						        std::unique_lock waitLock(doneMutex);
						        waitSucceeded = doneCv.wait_for(waitLock, std::chrono::milliseconds(1000),
						                                        [&]() { return workDone; });
					        });
				    });

				QVERIFY(outerInvokeOk);
				QVERIFY(innerInvokeOk);
				QVERIFY(waitSucceeded);
				QVERIFY(workDone);
			}

			void luaBridgeCooperativePumpSkipsAsynchronousPosts()
			{
				const LuaBridgeAsyncTarget target = qmudLuaBridgeCaptureAsyncTarget(QThread::currentThread());
				std::atomic_bool           executed{false};
				std::atomic_bool           canceled{false};
				constexpr quintptr         kPumpKey = 0x1234;
				QVERIFY(qmudLuaBridgePost(
				    target, [&executed] { executed.store(true); }, [&canceled] { canceled.store(true); },
				    kPumpKey));

				static_cast<void>(qmudLuaBridgePumpCurrentThreadOnce());
				QVERIFY(!executed.load());
				QVERIFY(!canceled.load());
				QVERIFY(!qmudLuaBridgePumpCurrentThreadAsyncOnce(kPumpKey + 1));
				QVERIFY(!executed.load());
				QVERIFY(qmudLuaBridgePumpCurrentThreadAsyncOnce(kPumpKey));
				QVERIFY(executed.load());
				QVERIFY(!canceled.load());
			}

			void luaBridgePumpYieldsToQueuedEventsDuringBacklog()
			{
				constexpr int              requestCount = 1024;
				const LuaBridgeAsyncTarget target = qmudLuaBridgeCaptureAsyncTarget(QThread::currentThread());
				int                        executed         = 0;
				int                        canceled         = 0;
				int                        executedAtMarker = -1;

				for (int requestIndex = 0; requestIndex < requestCount; ++requestIndex)
				{
					QVERIFY(
					    qmudLuaBridgePost(target, [&executed] { ++executed; }, [&canceled] { ++canceled; }));
				}
				QVERIFY(QMetaObject::invokeMethod(
				    this, [&executed, &executedAtMarker] { executedAtMarker = executed; },
				    Qt::QueuedConnection));

				QTRY_VERIFY_WITH_TIMEOUT(executedAtMarker >= 0, 3000);
				QVERIFY(executedAtMarker > 0);
				QVERIFY(executedAtMarker < requestCount);
				QTRY_COMPARE_WITH_TIMEOUT(executed, requestCount, 3000);
				QCOMPARE(canceled, 0);
			}

			void luaExecutorWorkerReentrantDispatchPreservesQueueOrder()
			{
				LuaExecutorWorker executor(recoveredMutationConsumerForTest());
				QObject           mainTarget;
				QThread          *mainThread = QThread::currentThread();

				QThread           workerThread;
				workerThread.setObjectName(QStringLiteral("tst_LuaExecutorWorkerReentrantWorker"));
				QObject workerTarget;
				workerTarget.moveToThread(&workerThread);
				workerThread.start();
				const auto stopWorker = qScopeGuard(
				    [&]()
				    {
					    if (workerTarget.thread() == &workerThread)
					    {
						    static_cast<void>(qmudLuaBridgeInvokeOnObjectThread(
						        &workerTarget,
						        [&workerTarget, mainThread] { workerTarget.moveToThread(mainThread); }));
					    }
					    workerThread.quit();
					    static_cast<void>(workerThread.wait());
				    });

				QVERIFY(qmudLuaBridgeEnsureObjectThreadReady(&workerTarget));
				QVERIFY(qmudLuaBridgeEnsureObjectThreadReady(&mainTarget));

				LuaBatchDispatchRequest asyncRequest;
				asyncRequest.kind = LuaBatchDispatchKind::HasFunction;
				LuaBatchDispatchRequest syncRequest;
				syncRequest.kind = LuaBatchDispatchKind::HasFunction;

				std::mutex      orderMutex;
				std::deque<int> order;
				const bool      invokeOk = qmudLuaBridgeInvokeOnObjectThread(
				    &workerTarget,
				    [&]()
				    {
					    const bool innerOk = qmudLuaBridgeInvokeOnObjectThread(
					        &mainTarget,
					        [&]()
					        {
						        executor.dispatchBatchAsync(asyncRequest, nullptr,
						                                    [&](const LuaBatchDispatchResult &)
						                                    {
							                                    std::scoped_lock lock(orderMutex);
							                                    order.push_back(1);
						                                    });
						        static_cast<void>(executor.dispatchBatch(syncRequest));
						        std::scoped_lock lock(orderMutex);
						        order.push_back(2);
					        });
					    QVERIFY(innerOk);
				    });
				QVERIFY(invokeOk);

				QTRY_VERIFY_WITH_TIMEOUT(
				    [&]()
				    {
					    std::scoped_lock lock(orderMutex);
					    return order.size() == 2;
				    }(),
				    2000);

				std::scoped_lock lock(orderMutex);
				QCOMPARE(order.size(), static_cast<size_t>(2));
				QCOMPARE(order.at(0), 1);
				QCOMPARE(order.at(1), 2);
			}

			void luaExecutorCompletionTargetDestroyedDuringDispatchIsNotInvoked()
			{
				static_assert(!std::is_default_constructible_v<LuaExecutorWorker>);
				class TargetDestroyingExecutor final : public ILuaExecutor
				{
					public:
						explicit TargetDestroyingExecutor(std::unique_ptr<QObject> &target) : m_target(target)
						{
						}

						[[nodiscard]] LuaBatchDispatchResult
						dispatchBatch(const LuaBatchDispatchRequest & /*request*/) const override
						{
							m_target.reset();
							return {};
						}

					private:
						std::unique_ptr<QObject> &m_target;
				};

				auto                     target        = std::make_unique<QObject>();
				QObject *const           targetPointer = target.get();
				TargetDestroyingExecutor executor(target);
				bool                     completionCalled = false;
				LuaBatchDispatchRequest  request;
				executor.dispatchBatchAsync(request, targetPointer,
				                            [&](const LuaBatchDispatchResult &) { completionCalled = true; });

				QVERIFY(!target);
				QVERIFY(!completionCalled);
			}

			void luaCompletionTargetDestructionFinishesFromBridgeQueue()
			{
				auto target                    = std::make_unique<QObject>();
				bool destructionSignalActive   = false;
				bool completionCalled          = false;
				bool deliveryFinished          = false;
				bool finishedDuringDestruction = false;
				QObject::connect(target.get(), &QObject::destroyed,
				                 [&destructionSignalActive] { destructionSignalActive = true; });
				const LuaCompletionTarget captured = qmudCaptureLuaCompletionTarget(target.get());
				qmudDeliverLuaCompletion(
				    captured, [&](const LuaBatchDispatchResult &) { completionCalled = true; }, {},
				    LuaCompletionDeliveryMode::AlwaysQueued,
				    [&]
				    {
					    deliveryFinished          = true;
					    finishedDuringDestruction = destructionSignalActive;
				    });

				target.reset();
				QVERIFY(destructionSignalActive);
				QVERIFY(!deliveryFinished);
				destructionSignalActive = false;
				QTRY_VERIFY_WITH_TIMEOUT(deliveryFinished, 3000);
				QVERIFY(!completionCalled);
				QVERIFY(!finishedDuringDestruction);
			}

			void luaWorkerStoppedCompletionThreadAdvancesSequence()
			{
				LuaExecutorWorker executor(recoveredMutationConsumerForTest());
				QThread           stoppedThread;
				auto             *stoppedTarget = new QObject();
				QPointer<QObject> stoppedGuard(stoppedTarget);
				stoppedTarget->moveToThread(&stoppedThread);
				QObject::connect(&stoppedThread, &QThread::finished, stoppedTarget, &QObject::deleteLater);
				stoppedThread.start();

				QSemaphore blocked;
				QSemaphore release;
				QVERIFY(QMetaObject::invokeMethod(
				    stoppedTarget,
				    [&]
				    {
					    blocked.release();
					    release.acquire();
				    },
				    Qt::QueuedConnection));
				QVERIFY(blocked.tryAcquire(1, 3000));

				LuaBatchDispatchRequest request;
				request.kind = LuaBatchDispatchKind::HasFunction;
				QObject          followingTarget;
				std::atomic_bool stoppedCompletion{false};
				std::atomic_bool followingCompletion{false};
				executor.dispatchBatchAsync(request, stoppedTarget, [&](const LuaBatchDispatchResult &)
				                            { stoppedCompletion.store(true); });
				executor.dispatchBatchAsync(request, &followingTarget, [&](const LuaBatchDispatchResult &)
				                            { followingCompletion.store(true); });
				static_cast<void>(executor.dispatchBatch(request));
				QVERIFY(!followingCompletion.load());

				stoppedThread.quit();
				release.release();
				QVERIFY(stoppedThread.wait(3000));
				QTRY_VERIFY_WITH_TIMEOUT(followingCompletion.load(), 3000);
				QVERIFY(!stoppedCompletion.load());
				QTRY_VERIFY_WITH_TIMEOUT(stoppedGuard.isNull(), 3000);
			}

			void luaWorkerThrowingTargetlessCompletionDoesNotStopQueuePump()
			{
				LuaExecutorWorker       executor(recoveredMutationConsumerForTest());
				LuaBatchDispatchRequest request;
				request.kind = LuaBatchDispatchKind::HasFunction;
				QObject          followingTarget;
				std::atomic_bool followingCompletion{false};

				QTest::ignoreMessage(
				    QtWarningMsg,
				    "[QMud][LuaExecutor] worker dispatch failed: exception while delivering a targetless "
				    "asynchronous completion: queued completion failure");
				executor.dispatchBatchAsync(request, nullptr, [](const LuaBatchDispatchResult &)
				                            { throw std::runtime_error("queued completion failure"); });
				executor.dispatchBatchAsync(request, &followingTarget, [&](const LuaBatchDispatchResult &)
				                            { followingCompletion.store(true, std::memory_order_release); });
				static_cast<void>(executor.dispatchBatch(request));

				QTRY_VERIFY_WITH_TIMEOUT(followingCompletion.load(std::memory_order_acquire), 3000);
			}

			void luaWorkerReentrantThrowingCompletionResolvesTicket()
			{
				LuaExecutorWorker       executor(recoveredMutationConsumerForTest());
				LuaBatchDispatchRequest request;
				request.kind = LuaBatchDispatchKind::HasFunction;
				QObject          followingTarget;
				std::atomic_bool reentrantDispatchReturned{false};
				std::atomic_bool followingCompletion{false};

				QTest::ignoreMessage(
				    QtWarningMsg,
				    "[QMud][LuaExecutor] worker dispatch failed: exception while delivering a targetless "
				    "asynchronous completion: reentrant completion failure");
				executor.dispatchBatchAsync(
				    request, nullptr,
				    [&](const LuaBatchDispatchResult &)
				    {
					    executor.dispatchBatchAsync(
					        request, nullptr, [](const LuaBatchDispatchResult &)
					        { throw std::runtime_error("reentrant completion failure"); });
					    executor.dispatchBatchAsync(
					        request, &followingTarget, [&](const LuaBatchDispatchResult &)
					        { followingCompletion.store(true, std::memory_order_release); });
					    reentrantDispatchReturned.store(true, std::memory_order_release);
				    });
				static_cast<void>(executor.dispatchBatch(request));

				QTRY_VERIFY_WITH_TIMEOUT(reentrantDispatchReturned.load(std::memory_order_acquire), 3000);
				QTRY_VERIFY_WITH_TIMEOUT(followingCompletion.load(std::memory_order_acquire), 3000);
			}

			void luaExecutorWorkerShutdownDrainsAsyncCompletions()
			{
				constexpr int   requestCount = 128;
				std::atomic_int completionCount{0};
				QObject         completionTarget;

				{
					auto executor = std::make_unique<LuaExecutorWorker>(recoveredMutationConsumerForTest());
					LuaBatchDispatchRequest request;
					request.kind = LuaBatchDispatchKind::HasFunction;
					for (int i = 0; i < requestCount; ++i)
					{
						executor->dispatchBatchAsync(request, &completionTarget,
						                             [&](const LuaBatchDispatchResult &)
						                             { completionCount.fetch_add(1); });
					}
					executor.reset();
				}

				QTRY_COMPARE_WITH_TIMEOUT(completionCount.load(), requestCount, 3000);
			}

			void luaExecutorQueuedMiniWindowSnapshotOutlivesProducerScope()
			{
				QObject          completionTarget;

				std::atomic_bool completed{false};
				QString          observedWindowName;
				int              observedWidth  = 0;
				bool             observedLookup = false;

				{
					auto snapshot         = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
					snapshot->windowNames = {QStringLiteral("Map"), QStringLiteral("map"),
					                         QStringLiteral("Map "), QStringLiteral("map|left")};
					LuaCallbackMiniWindowSnapshot::WindowInfoSnapshot info;
					info.width  = 123;
					info.height = 45;
					info.show   = true;
					snapshot->windowInfoByWindow.insert(QStringLiteral("Map"), info);
					snapshot->fontIdsByWindow.insert(QStringLiteral("Map"),
					                                 {QStringLiteral("Font"), QStringLiteral("Font ")});
					snapshot->fontIdsByWindow.insert(QStringLiteral("map"), {QStringLiteral("left|font")});
					snapshot->imageIdsByWindow.insert(QStringLiteral("Map"),
					                                  {QStringLiteral("Image"), QStringLiteral("Image ")});
					snapshot->imageIdsByWindow.insert(QStringLiteral("map"), {QStringLiteral("left|image")});
					snapshot->hotspotIdsByWindow.insert(QStringLiteral("Map"),
					                                    {QStringLiteral("Move"), QStringLiteral("Move ")});
					snapshot->hotspotIdsByWindow.insert(QStringLiteral("map"), {QStringLiteral("left|drag")});
					snapshot->rebuildMiniWindowLookupCaches();

					LuaBatchDispatchRequest request;
					request.kind                  = LuaBatchDispatchKind::HasFunction;
					request.miniWindowSnapshotArg = snapshot;
					LuaExecutorWorker executor(recoveredMutationConsumerForTest());
					executor.dispatchBatchAsync(
					    request, &completionTarget,
					    [snapshot = request.miniWindowSnapshotArg, &completed, &observedWindowName,
					     &observedWidth, &observedLookup](const LuaBatchDispatchResult &)
					    {
						    observedWindowName = snapshot->windowNames.value(0);
						    observedWidth  = snapshot->windowInfoByWindow.value(QStringLiteral("Map")).width;
						    observedLookup = snapshot->miniWindowIds.contains(QStringLiteral("Map")) &&
						                     snapshot->miniWindowIds.contains(QStringLiteral("map")) &&
						                     snapshot->miniWindowIds.contains(QStringLiteral("Map ")) &&
						                     snapshot->miniWindowIds.contains(QStringLiteral("map|left")) &&
						                     snapshot->miniWindowFontKeys.contains(
						                         {QStringLiteral("Map"), QStringLiteral("Font")}) &&
						                     snapshot->miniWindowFontKeys.contains(
						                         {QStringLiteral("Map"), QStringLiteral("Font ")}) &&
						                     snapshot->miniWindowFontKeys.contains(
						                         {QStringLiteral("map"), QStringLiteral("left|font")}) &&
						                     !snapshot->miniWindowFontKeys.contains(
						                         {QStringLiteral("map|left"), QStringLiteral("font")}) &&
						                     snapshot->miniWindowImageKeys.contains(
						                         {QStringLiteral("Map"), QStringLiteral("Image")}) &&
						                     snapshot->miniWindowImageKeys.contains(
						                         {QStringLiteral("Map"), QStringLiteral("Image ")}) &&
						                     snapshot->miniWindowImageKeys.contains(
						                         {QStringLiteral("map"), QStringLiteral("left|image")}) &&
						                     !snapshot->miniWindowImageKeys.contains(
						                         {QStringLiteral("map|left"), QStringLiteral("image")}) &&
						                     snapshot->miniWindowHotspotKeys.contains(
						                         {QStringLiteral("Map"), QStringLiteral("Move")}) &&
						                     snapshot->miniWindowHotspotKeys.contains(
						                         {QStringLiteral("Map"), QStringLiteral("Move ")}) &&
						                     snapshot->miniWindowHotspotKeys.contains(
						                         {QStringLiteral("map"), QStringLiteral("left|drag")}) &&
						                     !snapshot->miniWindowHotspotKeys.contains(
						                         {QStringLiteral("map|left"), QStringLiteral("drag")});
						    completed.store(true);
					    });
				}

				QTRY_VERIFY_WITH_TIMEOUT(completed.load(), 3000);
				QCOMPARE(observedWindowName, QStringLiteral("Map"));
				QCOMPARE(observedWidth, 123);
				QVERIFY(observedLookup);
			}

			void luaBridgeRejectsExceptionFromSameThreadCallback()
			{
				QObject target;
				QVERIFY(
				    !qmudLuaBridgeInvokeOnObjectThread(&target, [] { throw std::runtime_error("boom"); }));
				QCOMPARE(qmudLuaBridgeLastStatus(), LuaBridgeInvokeStatus::UnknownFailure);
				QVERIFY(qmudLuaBridgeLastError().contains(QStringLiteral("threw exception")));
			}

			void luaBridgeRejectsExceptionFromWorkerThreadCallback()
			{
				QThread workerThread;
				workerThread.setObjectName(QStringLiteral("tst_LuaBridgeThrowWorker"));
				QObject  target;
				QThread *mainThread = QThread::currentThread();
				target.moveToThread(&workerThread);
				workerThread.start();
				const auto stopWorker = qScopeGuard(
				    [&]()
				    {
					    if (target.thread() == &workerThread)
					    {
						    static_cast<void>(qmudLuaBridgeInvokeOnObjectThread(
						        &target, [&target, mainThread] { target.moveToThread(mainThread); }));
					    }
					    workerThread.quit();
					    static_cast<void>(workerThread.wait());
				    });

				QVERIFY(qmudLuaBridgeEnsureObjectThreadReady(&target));
				QVERIFY(
				    !qmudLuaBridgeInvokeOnObjectThread(&target, [] { throw std::runtime_error("boom"); }));
				QCOMPARE(qmudLuaBridgeLastStatus(), LuaBridgeInvokeStatus::UnknownFailure);
				QVERIFY(qmudLuaBridgeLastError().contains(QStringLiteral("threw exception")));
			}

			void luaBridgeEnsureReadyFailsForNonRunningThread()
			{
				QThread  workerThread;
				auto    *target     = new QObject();
				QThread *mainThread = QThread::currentThread();
				target->moveToThread(&workerThread);

				QVERIFY(!qmudLuaBridgeEnsureObjectThreadReady(target));
				QVERIFY(qmudLuaBridgeLastError().contains(QStringLiteral("not running")));

				workerThread.start();
				const auto stopWorker = qScopeGuard(
				    [&]()
				    {
					    workerThread.quit();
					    static_cast<void>(workerThread.wait());
				    });
				QVERIFY(qmudLuaBridgeInvokeOnObjectThread(target, [target, mainThread]
				                                          { target->moveToThread(mainThread); }));
				delete target;
			}

			void luaBridgeCompletesWhenTargetDestroyedBeforeDispatch()
			{
				QThread workerThread;
				workerThread.setObjectName(QStringLiteral("tst_LuaBridgeDestroyedTarget"));
				workerThread.start();
				const auto stopWorker = qScopeGuard(
				    [&]()
				    {
					    workerThread.quit();
					    static_cast<void>(workerThread.wait());
				    });

				QPointer<QObject> target = new QObject();
				target->moveToThread(&workerThread);

				std::mutex              gateMutex;
				std::condition_variable gateCv;
				bool                    firstEntered = false;

				std::atomic_bool        firstInvokeOk{false};
				std::thread             firstCaller(
				    [&]()
				    {
					    firstInvokeOk.store(qmudLuaBridgeInvokeOnObjectThread(target.data(),
					                                                          [&]()
					                                                          {
						                                                          {
							                                                          std::lock_guard lock(
							                                                              gateMutex);
							                                                          firstEntered = true;
						                                                          }
						                                                          gateCv.notify_all();
						                                                          QThread::msleep(100);
						                                                          if (target)
						                                                          {
							                                                          delete target.data();
							                                                          target.clear();
						                                                          }
					                                                          }));
				    });

				{
					std::unique_lock lock(gateMutex);
					gateCv.wait(lock, [&] { return firstEntered; });
				}

				std::atomic_bool secondCallbackInvoked{false};
				const bool       secondInvokeOk = qmudLuaBridgeInvokeOnObjectThread(
				    target.data(), [&]() { secondCallbackInvoked.store(true); });

				firstCaller.join();

				QVERIFY(firstInvokeOk.load());
				QVERIFY(secondInvokeOk);
				QVERIFY(!secondCallbackInvoked.load());
			}

			void luaBridgeHandlesConcurrentCallersWithoutLoss()
			{
				constexpr int callerCount      = 4;
				constexpr int invokesPerCaller = 25;

				QThread       workerThread;
				workerThread.setObjectName(QStringLiteral("tst_LuaBridgeConcurrentWorker"));
				QObject  target;
				QThread *mainThread = QThread::currentThread();
				target.moveToThread(&workerThread);
				workerThread.start();
				const auto stopWorker = qScopeGuard(
				    [&]()
				    {
					    if (target.thread() == &workerThread)
					    {
						    static_cast<void>(qmudLuaBridgeInvokeOnObjectThread(
						        &target, [&target, mainThread] { target.moveToThread(mainThread); }));
					    }
					    workerThread.quit();
					    static_cast<void>(workerThread.wait());
				    });

				QVERIFY(qmudLuaBridgeEnsureObjectThreadReady(&target));

				std::atomic_bool             invokeFailure{false};
				std::atomic_bool             wrongThread{false};
				std::atomic_int              totalInvokes{0};
				std::mutex                   orderMutex;
				std::array<int, callerCount> lastSeenByCaller{};
				lastSeenByCaller.fill(-1);
				bool                     orderOk = true;

				std::vector<std::thread> callers;
				callers.reserve(callerCount);
				for (int callerIndex = 0; callerIndex < callerCount; ++callerIndex)
				{
					callers.emplace_back(
					    [&, callerIndex]
					    {
						    for (int callIndex = 0; callIndex < invokesPerCaller; ++callIndex)
						    {
							    const bool ok = qmudLuaBridgeInvokeOnObjectThread(
							        &target,
							        [&, callerIndex, callIndex]
							        {
								        if (QThread::currentThread() != &workerThread)
									        wrongThread.store(true);
								        {
									        std::lock_guard lock(orderMutex);
									        if (lastSeenByCaller[callerIndex] + 1 != callIndex)
										        orderOk = false;
									        lastSeenByCaller[callerIndex] = callIndex;
								        }
								        totalInvokes.fetch_add(1);
							        });
							    if (!ok)
							    {
								    invokeFailure.store(true);
								    break;
							    }
						    }
					    });
				}
				for (auto &caller : callers)
					caller.join();

				QVERIFY(!invokeFailure.load());
				QVERIFY(!wrongThread.load());
				QVERIFY(orderOk);
				QCOMPARE(totalInvokes.load(), callerCount * invokesPerCaller);
				for (int i = 0; i < callerCount; ++i)
					QCOMPARE(lastSeenByCaller[i], invokesPerCaller - 1);
			}

			void luaBridgeLastErrorIsThreadLocal()
			{
				QThread workerThread;
				workerThread.setObjectName(QStringLiteral("tst_LuaBridgeThreadLocalWorker"));
				QObject  target;
				QThread *mainThread = QThread::currentThread();
				target.moveToThread(&workerThread);
				workerThread.start();
				const auto stopWorker = qScopeGuard(
				    [&]()
				    {
					    if (target.thread() == &workerThread)
					    {
						    static_cast<void>(qmudLuaBridgeInvokeOnObjectThread(
						        &target, [&target, mainThread] { target.moveToThread(mainThread); }));
					    }
					    workerThread.quit();
					    static_cast<void>(workerThread.wait());
				    });

				QVERIFY(qmudLuaBridgeEnsureObjectThreadReady(&target));

				std::mutex              gateMutex;
				std::condition_variable gateCv;
				bool                    aFailed = false;
				bool                    bDone   = false;
				QString                 errorA1;
				QString                 errorA2;
				QString                 errorB;
				std::atomic_bool        invokeBSuccess{false};

				std::thread             callerA(
				    [&]()
				    {
					    static_cast<void>(qmudLuaBridgeInvokeOnObjectThread(nullptr, [] {}));
					    errorA1 = qmudLuaBridgeLastError();
					    {
						    std::lock_guard lock(gateMutex);
						    aFailed = true;
					    }
					    gateCv.notify_all();
					    std::unique_lock lock(gateMutex);
					    gateCv.wait(lock, [&] { return bDone; });
					    errorA2 = qmudLuaBridgeLastError();
				    });

				std::thread callerB(
				    [&]()
				    {
					    std::unique_lock lock(gateMutex);
					    gateCv.wait(lock, [&] { return aFailed; });
					    lock.unlock();
					    invokeBSuccess.store(qmudLuaBridgeInvokeOnObjectThread(&target, [] {}));
					    errorB = qmudLuaBridgeLastError();
					    {
						    std::lock_guard doneLock(gateMutex);
						    bDone = true;
					    }
					    gateCv.notify_all();
				    });

				callerA.join();
				callerB.join();

				QVERIFY(invokeBSuccess.load());
				QVERIFY(!errorA1.isEmpty());
				QCOMPARE(errorA2, errorA1);
				QVERIFY(errorB.isEmpty());
			}

			void crossStateMarshalsSupportedTypes()
			{
				RawLuaStateOwner caller(luaL_newstate());
				RawLuaStateOwner target(luaL_newstate());
				QVERIFY(caller.get());
				QVERIFY(target.get());
				QVERIFY(runLuaChunk(target.get(), "function cb(a,b,c,d) return a,b,c,d end"));

				lua_pushnil(caller.get());
				lua_pushboolean(caller.get(), 1);
				lua_pushnumber(caller.get(), 42.5);
				lua_pushstring(caller.get(), "abc");

				const auto result = qmudCallPluginLuaWithMarshalling(caller.get(), target.get(), "cb", 1);
				QCOMPARE(result.error, CallPluginLuaMarshallingError::None);
				QCOMPARE(result.returnCount, 4);
				QCOMPARE(lua_gettop(caller.get()), 8);
				QVERIFY(lua_isnil(caller.get(), 5));
				QVERIFY(lua_toboolean(caller.get(), 6));
				QCOMPARE(lua_tonumber(caller.get(), 7), 42.5);
				QCOMPARE(QString::fromUtf8(lua_tostring(caller.get(), 8)), QStringLiteral("abc"));
			}

			void crossStateRejectsUnsupportedArgumentType()
			{
				RawLuaStateOwner caller(luaL_newstate());
				RawLuaStateOwner target(luaL_newstate());
				QVERIFY(caller.get());
				QVERIFY(target.get());
				QVERIFY(runLuaChunk(target.get(), "function cb(x) return x end"));

				lua_newtable(caller.get());
				const auto result = qmudCallPluginLuaWithMarshalling(caller.get(), target.get(), "cb", 1);
				QCOMPARE(result.error, CallPluginLuaMarshallingError::UnsupportedArgumentType);
				QCOMPARE(result.index, 1);
				QCOMPARE(QString::fromLatin1(result.typeName), QStringLiteral("table"));
				QCOMPARE(lua_gettop(target.get()), 0);
			}

			void crossStateRejectsUnsupportedReturnType()
			{
				RawLuaStateOwner caller(luaL_newstate());
				RawLuaStateOwner target(luaL_newstate());
				QVERIFY(caller.get());
				QVERIFY(target.get());
				QVERIFY(runLuaChunk(target.get(), "function cb() return {} end"));

				const auto result = qmudCallPluginLuaWithMarshalling(caller.get(), target.get(), "cb", 1);
				QCOMPARE(result.error, CallPluginLuaMarshallingError::UnsupportedReturnType);
				QCOMPARE(result.index, 1);
				QCOMPARE(QString::fromLatin1(result.typeName), QStringLiteral("table"));
				QCOMPARE(lua_gettop(target.get()), 0);
			}

			void crossStateReportsRuntimeError()
			{
				RawLuaStateOwner caller(luaL_newstate());
				RawLuaStateOwner target(luaL_newstate());
				QVERIFY(caller.get());
				QVERIFY(target.get());
				QVERIFY(runLuaChunk(target.get(), "function cb() error('boom') end"));

				const auto result = qmudCallPluginLuaWithMarshalling(caller.get(), target.get(), "cb", 1);
				QCOMPARE(result.error, CallPluginLuaMarshallingError::RuntimeError);
				QVERIFY(!result.runtimeError.trimmed().isEmpty());
				QCOMPARE(lua_gettop(target.get()), 0);
			}

			void reportsNoSuchRoutine()
			{
				RawLuaStateOwner caller(luaL_newstate());
				RawLuaStateOwner target(luaL_newstate());
				QVERIFY(caller.get());
				QVERIFY(target.get());

				const auto result =
				    qmudCallPluginLuaWithMarshalling(caller.get(), target.get(), "pkg.inner.missing", 1);
				QCOMPARE(result.error, CallPluginLuaMarshallingError::NoSuchRoutine);
			}

			void sameStateInvocationReturnsFunctionResults()
			{
				RawLuaStateOwner state(luaL_newstate());
				QVERIFY(state.get());
				QVERIFY(runLuaChunk(state.get(), "function add_pair(a,b) return a+b,'ok' end"));

				lua_pushnumber(state.get(), 2);
				lua_pushnumber(state.get(), 3);
				const auto result = qmudCallPluginLuaWithMarshalling(state.get(), state.get(), "add_pair", 1);
				QCOMPARE(result.error, CallPluginLuaMarshallingError::None);
				QCOMPARE(result.returnCount, 2);
				QCOMPARE(lua_gettop(state.get()), 2);
				QCOMPARE(lua_tonumber(state.get(), 1), 5.0);
				QCOMPARE(QString::fromUtf8(lua_tostring(state.get(), 2)), QStringLiteral("ok"));
			}

			void sameMainThreadCoroutineInvocationReturnsOnCallerStack()
			{
				RawLuaStateOwner state(luaL_newstate());
				QVERIFY(state.get());
				QVERIFY(runLuaChunk(state.get(), "function add_pair(a,b) return a+b,'ok' end"));

				lua_State *const coroutine = lua_newthread(state.get());
				QVERIFY(coroutine);
				const int  coroutineRef   = luaL_ref(state.get(), LUA_REGISTRYINDEX);
				const auto unrefCoroutine = qScopeGuard(
				    [&state, coroutineRef] { luaL_unref(state.get(), LUA_REGISTRYINDEX, coroutineRef); });
				Q_UNUSED(unrefCoroutine);
				QVERIFY(qmudLuaStatesShareMainThread(coroutine, state.get()));
				QVERIFY(lua_checkstack(coroutine, 2));

				lua_pushnumber(coroutine, 2);
				lua_pushnumber(coroutine, 3);
				const auto result =
				    qmudCallPluginLuaWithMarshalling(coroutine, state.get(), QStringLiteral("add_pair"), 1);

				QCOMPARE(result.error, CallPluginLuaMarshallingError::None);
				QCOMPARE(result.returnCount, 2);
				QCOMPARE(lua_gettop(state.get()), 0);
				QCOMPARE(lua_gettop(coroutine), 2);
				QCOMPARE(lua_tonumber(coroutine, 1), 5.0);
				QCOMPARE(QString::fromUtf8(lua_tostring(coroutine, 2)), QStringLiteral("ok"));
			}

			void getfenvAndSetfenvWorkForFunctionClosures()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				const auto result = evaluateLuaToString(
				    state.get(),
				    QByteArrayLiteral("local f = assert(load(\"return test_value\"))\n"
				                      "setfenv(f, { test_value = \"compat\" })\n"
				                      "return tostring(getfenv(f).test_value) .. \":\" .. tostring(f())"));
				QVERIFY2(result.ok, qPrintable(result.error));
				QCOMPARE(result.value, QStringLiteral("compat:compat"));
			}

			void moduleShimCreatesAndUsesModuleEnvironment()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				const auto result = evaluateLuaToString(
				    state.get(), QByteArrayLiteral("local tostring = tostring\n"
				                                   "local package = package\n"
				                                   "local globals = _G\n"
				                                   "module(\"compat_mod\")\n"
				                                   "value = 42\n"
				                                   "return tostring(globals.compat_mod.value) .. \":\" .. "
				                                   "tostring(package.loaded.compat_mod.value) .. \":\" .. "
				                                   "tostring(value)"));
				QVERIFY2(result.ok, qPrintable(result.error));
				QCOMPARE(result.value, QStringLiteral("42:42:42"));
			}

			void moduleShimSetsLegacyModuleFields()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				const auto result = evaluateLuaToString(
				    state.get(),
				    QByteArrayLiteral("local tostring = tostring\n"
				                      "local package = package\n"
				                      "module(\"compat.parent\")\n"
				                      "return tostring(_NAME) .. \":\" .. "
				                      "tostring(_M == package.loaded[\"compat.parent\"]) .. \":\" .. "
				                      "tostring(_PACKAGE)"));
				QVERIFY2(result.ok, qPrintable(result.error));
				QCOMPARE(result.value, QStringLiteral("compat.parent:true:compat."));
			}

			void moduleShimCreatesDottedGlobalPath()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				const auto result = evaluateLuaToString(
				    state.get(), QByteArrayLiteral("local tostring = tostring\n"
				                                   "local type = type\n"
				                                   "local package = package\n"
				                                   "local globals = _G\n"
				                                   "module(\"compat.path.deep\")\n"
				                                   "value = 7\n"
				                                   "return tostring(type(globals.compat)) .. \":\" .. "
				                                   "tostring(type(globals.compat.path)) .. \":\" .. "
				                                   "tostring(globals.compat.path.deep.value) .. \":\" .. "
				                                   "tostring(package.loaded[\"compat.path.deep\"] == "
				                                   "globals.compat.path.deep)"));
				QVERIFY2(result.ok, qPrintable(result.error));
				QCOMPARE(result.value, QStringLiteral("table:table:7:true"));
			}

			void moduleShimSupportsPackageSeeallOption()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				const auto result = evaluateLuaToString(
				    state.get(), QByteArrayLiteral("local tostring = tostring\n"
				                                   "module(\"compat.seeall\", package.seeall)\n"
				                                   "return tostring(type(print)) .. \":\" .. "
				                                   "tostring(getmetatable(_M).__index == _G)"));
				QVERIFY2(result.ok, qPrintable(result.error));
				QCOMPARE(result.value, QStringLiteral("function:true"));
			}

			void unpackAliasUsesTableUnpackBehaviour()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				const auto result = evaluateLuaToString(
				    state.get(), QByteArrayLiteral("local a, b, c = unpack({1, 2, 3})\n"
				                                   "return string.format(\"%d,%d,%d\", a, b, c)"));
				QVERIFY2(result.ok, qPrintable(result.error));
				QCOMPARE(result.value, QStringLiteral("1,2,3"));
			}

			void loadstringAliasCompilesAndRunsChunks()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				const auto result = evaluateLuaToString(
				    state.get(), QByteArrayLiteral("local fn = loadstring(\"return 123\")\n"
				                                   "return tostring(type(fn)) .. \":\" .. tostring(fn())"));
				QVERIFY2(result.ok, qPrintable(result.error));
				QCOMPARE(result.value, QStringLiteral("function:123"));
			}

			void namedProcedureCallTreatsNilReturnAsSuccess()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				QString execError;
				QVERIFY2(executeLuaChunk(state.get(),
				                         QByteArrayLiteral("mapper = {}\n"
				                                           "function mapper.do_hyperlink(hash)\n"
				                                           "  _G.__qmud_procedure_arg = hash\n"
				                                           "  return nil\n"
				                                           "end"),
				                         execError),
				         qPrintable(execError));

				bool    hasFunction = false;
				QString callError;
				QVERIFY(QMudLuaSupport::callLuaNamedProcedureWithString(
				    state.get(), QStringLiteral("mapper.do_hyperlink"), QStringLiteral("7B69"), &hasFunction,
				    &callError));
				QVERIFY(hasFunction);
				QVERIFY(callError.isEmpty());

				const auto result = evaluateLuaToString(
				    state.get(), QByteArrayLiteral("return tostring(__qmud_procedure_arg)"));
				QVERIFY2(result.ok, qPrintable(result.error));
				QCOMPARE(result.value, QStringLiteral("7B69"));
			}

			void namedProcedureCallIgnoresBooleanFalseReturn()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				QString execError;
				QVERIFY2(executeLuaChunk(state.get(),
				                         QByteArrayLiteral("mapper = {}\n"
				                                           "function mapper.do_hyperlink(hash)\n"
				                                           "  _G.__qmud_false_return_seen = hash\n"
				                                           "  return false\n"
				                                           "end"),
				                         execError),
				         qPrintable(execError));

				bool    hasFunction = false;
				QString callError;
				QVERIFY(QMudLuaSupport::callLuaNamedProcedureWithString(
				    state.get(), QStringLiteral("mapper.do_hyperlink"), QStringLiteral("A1"), &hasFunction,
				    &callError));
				QVERIFY(hasFunction);
				QVERIFY(callError.isEmpty());

				const auto result = evaluateLuaToString(
				    state.get(), QByteArrayLiteral("return tostring(__qmud_false_return_seen)"));
				QVERIFY2(result.ok, qPrintable(result.error));
				QCOMPARE(result.value, QStringLiteral("A1"));
			}

			void namedProcedureCallReportsRuntimeError()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				QString execError;
				QVERIFY2(executeLuaChunk(state.get(),
				                         QByteArrayLiteral("mapper = {}\n"
				                                           "function mapper.do_hyperlink(hash)\n"
				                                           "  error(\"boom:\" .. tostring(hash))\n"
				                                           "end"),
				                         execError),
				         qPrintable(execError));

				bool    hasFunction = false;
				QString callError;
				QVERIFY(!QMudLuaSupport::callLuaNamedProcedureWithString(
				    state.get(), QStringLiteral("mapper.do_hyperlink"), QStringLiteral("ERR"), &hasFunction,
				    &callError));
				QVERIFY(hasFunction);
				QVERIFY(callError.contains(QStringLiteral("boom:ERR")));
			}

			void namedProcedureCallReportsMissingFunction()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				bool    hasFunction = true;
				QString callError;
				QVERIFY(!QMudLuaSupport::callLuaNamedProcedureWithString(
				    state.get(), QStringLiteral("mapper.missing_callback"), QStringLiteral("ARG"),
				    &hasFunction, &callError));
				QVERIFY(!hasFunction);
				QVERIFY(callError.isEmpty());
			}

			void pushLuaFunctionByNameResolvesGlobalAndNestedFunctions()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				QString execError;
				QVERIFY2(executeLuaChunk(state.get(),
				                         QByteArrayLiteral("function global_cb(v) return v end\n"
				                                           "nested = { hooks = {} }\n"
				                                           "function nested.hooks.cb(v) return v end"),
				                         execError),
				         qPrintable(execError));

				const int stackBeforeGlobal = lua_gettop(state.get());
				QVERIFY(QMudLuaSupport::pushLuaFunctionByName(state.get(), QStringLiteral("global_cb")));
				QVERIFY(lua_isfunction(state.get(), -1));
				lua_pop(state.get(), 1);
				QCOMPARE(lua_gettop(state.get()), stackBeforeGlobal);

				const int stackBeforeNested = lua_gettop(state.get());
				QVERIFY(
				    QMudLuaSupport::pushLuaFunctionByName(state.get(), QStringLiteral("nested.hooks.cb")));
				QVERIFY(lua_isfunction(state.get(), -1));
				lua_pop(state.get(), 1);
				QCOMPARE(lua_gettop(state.get()), stackBeforeNested);
			}

			void pushLuaFunctionByNameRejectsInvalidLookupAndKeepsStackBalanced()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				QString execError;
				QVERIFY2(executeLuaChunk(state.get(),
				                         QByteArrayLiteral("root = { child = {} }\n"
				                                           "root.child.not_fn = 7\n"
				                                           "root.leaf = 3"),
				                         execError),
				         qPrintable(execError));

				const int stackBeforeMissing = lua_gettop(state.get());
				QVERIFY(!QMudLuaSupport::pushLuaFunctionByName(state.get(), QStringLiteral("root.missing")));
				QCOMPARE(lua_gettop(state.get()), stackBeforeMissing);

				const int stackBeforeNotFunction = lua_gettop(state.get());
				QVERIFY(
				    !QMudLuaSupport::pushLuaFunctionByName(state.get(), QStringLiteral("root.child.not_fn")));
				QCOMPARE(lua_gettop(state.get()), stackBeforeNotFunction);

				const int stackBeforeNonTableIntermediate = lua_gettop(state.get());
				QVERIFY(
				    !QMudLuaSupport::pushLuaFunctionByName(state.get(), QStringLiteral("root.leaf.deeper")));
				QCOMPARE(lua_gettop(state.get()), stackBeforeNonTableIntermediate);
			}

			void namedProcedureCallSupportsGlobalFunctionName()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				QString execError;
				QVERIFY2(executeLuaChunk(state.get(),
				                         QByteArrayLiteral("function global_hyperlink(hash)\n"
				                                           "  _G.__qmud_global_proc_arg = hash\n"
				                                           "end"),
				                         execError),
				         qPrintable(execError));

				bool    hasFunction = false;
				QString callError;
				QVERIFY(QMudLuaSupport::callLuaNamedProcedureWithString(
				    state.get(), QStringLiteral("global_hyperlink"), QStringLiteral("G1"), &hasFunction,
				    &callError));
				QVERIFY(hasFunction);
				QVERIFY(callError.isEmpty());

				const auto result = evaluateLuaToString(
				    state.get(), QByteArrayLiteral("return tostring(__qmud_global_proc_arg)"));
				QVERIFY2(result.ok, qPrintable(result.error));
				QCOMPARE(result.value, QStringLiteral("G1"));
			}

			void namedProcedureCallMissingIntermediateTableReportsNoFunction()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				QString execError;
				QVERIFY2(executeLuaChunk(state.get(), QByteArrayLiteral("mapper = { wrong = 5 }"), execError),
				         qPrintable(execError));

				bool    hasFunction = true;
				QString callError;
				QVERIFY(!QMudLuaSupport::callLuaNamedProcedureWithString(
				    state.get(), QStringLiteral("mapper.wrong.callback"), QStringLiteral("ARG"), &hasFunction,
				    &callError));
				QVERIFY(!hasFunction);
				QVERIFY(callError.isEmpty());
			}

			void requireShimExportsSimpleModuleNamesToGlobals()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				const auto result = evaluateLuaToString(
				    state.get(), QByteArrayLiteral("package.preload[\"json\"] = function()\n"
				                                   "  return { decode = function() return \"ok\" end }\n"
				                                   "end\n"
				                                   "local m = require(\"json\")\n"
				                                   "return tostring(_G.json == m) .. \":\" .. "
				                                   "tostring(type(json.decode))"));
				QVERIFY2(result.ok, qPrintable(result.error));
				QCOMPARE(result.value, QStringLiteral("true:function"));
			}

			void requireShimDoesNotExportDottedModuleNames()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				const auto result = evaluateLuaToString(
				    state.get(), QByteArrayLiteral("package.preload[\"foo.bar\"] = function()\n"
				                                   "  return { value = 7 }\n"
				                                   "end\n"
				                                   "local m = require(\"foo.bar\")\n"
				                                   "return tostring(m.value) .. \":\" .. "
				                                   "tostring(rawget(_G, \"foo.bar\") == nil)"));
				QVERIFY2(result.ok, qPrintable(result.error));
				QCOMPARE(result.value, QStringLiteral("7:true"));
			}

			void requireShimLeavesCaughtRequireFailuresSilent()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				const auto result = evaluateLuaToString(
				    state.get(),
				    QByteArrayLiteral(
				        "local ok, err = pcall(require, \"__qmud_missing_module_for_test__\")\n"
				        "local package_cached = package.loaded[\"__qmud_missing_module_for_test__\"] "
				        "~= nil\n"
				        "return tostring(ok) .. \"|\" .. tostring(package_cached) .. \"|\" .. "
				        "tostring(type(err))"));
				QVERIFY2(result.ok, qPrintable(result.error));
				const QStringList parts = result.value.split('|');
				QCOMPARE(parts.size(), 3);
				QCOMPARE(parts.at(0), QStringLiteral("false"));
				QCOMPARE(parts.at(1), QStringLiteral("false"));
				QCOMPARE(parts.at(2), QStringLiteral("string"));
			}

			void stringFormatIntegerSpecifiersCoerceLua54NumbersLikeLua51()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				const auto result = evaluateLuaToString(
				    state.get(), QByteArrayLiteral("return string.format(\"%d|%i|%x\", 12.9, -3.2, 15.9)"));
				QVERIFY2(result.ok, qPrintable(result.error));
				QCOMPARE(result.value, QStringLiteral("12|-3|f"));
			}

			void stringFormatStillErrorsForNonNumericIntegerSpecifiers()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				const auto result = evaluateLuaToString(
				    state.get(), QByteArrayLiteral("return string.format(\"%d\", \"abc\")"));
				QVERIFY(!result.ok);
				QVERIFY(result.error.contains(QStringLiteral("number expected"), Qt::CaseInsensitive) ||
				        result.error.contains(QStringLiteral("bad argument"), Qt::CaseInsensitive));
			}

			void stringSubCoercesFractionalIndicesLikeLua51()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				const auto result =
				    evaluateLuaToString(state.get(), QByteArrayLiteral("return (\"abcdef\"):sub(2.9, 4.2)"));
				QVERIFY2(result.ok, qPrintable(result.error));
				QCOMPARE(result.value, QStringLiteral("bcd"));
			}

			void stringSubStillErrorsForNonNumericIndices()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				const auto result =
				    evaluateLuaToString(state.get(), QByteArrayLiteral("return string.sub(\"abcdef\", {})"));
				QVERIFY(!result.ok);
				QVERIFY(result.error.contains(QStringLiteral("number expected"), Qt::CaseInsensitive) ||
				        result.error.contains(QStringLiteral("bad argument"), Qt::CaseInsensitive));
			}

			void applyLua51CompatIsIdempotent()
			{
				LuaStateOwner state(QMudLuaSupport::makeLuaState());
				QVERIFY(state);
				luaL_openlibs(state.get());
				QMudLuaSupport::applyLua51Compat(state.get());
				QMudLuaSupport::applyLua51Compat(state.get());

				const auto result = evaluateLuaToString(
				    state.get(),
				    QByteArrayLiteral(
				        "local a = tostring(rawget(_G, \"__qmud_require_compat_wrapped\") == true)\n"
				        "local b = tostring(rawget(_G, \"__qmud_string_format_compat_wrapped\") == true)\n"
				        "local c = tostring(rawget(_G, \"__qmud_string_sub_compat_wrapped\") == true)\n"
				        "local d = tostring(rawget(_G, \"__qmud_string_gsub_compat_wrapped\") == true)\n"
				        "local e = tostring(debug.getinfo(string.sub, \"S\").what == \"C\")\n"
				        "local s = string.gsub(\"[x]\", \"%[x%]\", \"%[ok%]\")\n"
				        "return a .. \":\" .. b .. \":\" .. c .. \":\" .. d .. \":\" .. e .. \":\" .. s"));
				QVERIFY2(result.ok, qPrintable(result.error));
				QCOMPARE(result.value, QStringLiteral("true:true:true:true:true:[ok]"));
			}

			void stringGsubLenientReplacementMatchesMushclientBehaviour()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				const auto result = evaluateLuaToString(
				    state.get(),
				    QByteArrayLiteral("return (string.gsub(\"[CLAN Novice Adventurers] Friction: 'hax'\", "
				                      "\"%[CLAN Novice Adventurers%]\", \"%[Novice%]\"))"));
				QVERIFY2(result.ok, qPrintable(result.error));
				QCOMPARE(result.value, QStringLiteral("[Novice] Friction: 'hax'"));
			}

			void stringGsubCaptureReplacementStillWorks()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				const auto result = evaluateLuaToString(
				    state.get(),
				    QByteArrayLiteral("return (string.gsub(\"abc123\", \"(%a+)(%d+)\", \"%2-%1\"))"));
				QVERIFY2(result.ok, qPrintable(result.error));
				QCOMPARE(result.value, QStringLiteral("123-abc"));
			}

			void stringGsubPercentLiteralStillWorks()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				const auto result = evaluateLuaToString(
				    state.get(), QByteArrayLiteral("return (string.gsub(\"foo\", \"foo\", \"%%foo\"))"));
				QVERIFY2(result.ok, qPrintable(result.error));
				QCOMPARE(result.value, QStringLiteral("%foo"));
			}

			void socketHttpRequireShimRoutesHttpsThroughSslModuleWhenAvailable()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				const auto result = evaluateLuaToString(
				    state.get(),
				    QByteArrayLiteral("package.preload[\"socket.http\"] = function()\n"
				                      "  return {\n"
				                      "    request = function(reqt, body)\n"
				                      "      return \"plain:\" .. tostring(reqt), 480, { src = "
				                      "\"plain\" }\n"
				                      "    end\n"
				                      "  }\n"
				                      "end\n"
				                      "package.preload[\"ssl.https\"] = function()\n"
				                      "  return {\n"
				                      "    request = function(reqt, body)\n"
				                      "      if type(reqt) == \"table\" then\n"
				                      "        if reqt.sink then reqt.sink(\"secure:\" .. "
				                      "tostring(reqt.url)) end\n"
				                      "        return 1, 200, { src = \"ssl\" }\n"
				                      "      end\n"
				                      "      return \"secure:\" .. tostring(reqt), 200, { src = "
				                      "\"ssl\" }\n"
				                      "    end\n"
				                      "  }\n"
				                      "end\n"
				                      "local http = require(\"socket.http\")\n"
				                      "local page, code, headers = "
				                      "http.request(\"https://example.invalid/path\")\n"
				                      "return tostring(page) .. \"|\" .. tostring(code) .. \"|\" "
				                      ".. tostring(headers and headers.src)"));
				QVERIFY2(result.ok, qPrintable(result.error));
				QCOMPARE(result.value, QStringLiteral("secure:https://example.invalid/path|200|ssl"));
			}

			void socketHttpRequireShimPreservesHttpsPostBodySemantics()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				const auto result = evaluateLuaToString(
				    state.get(), QByteArrayLiteral("package.preload[\"socket.http\"] = function()\n"
				                                   "  return {\n"
				                                   "    request = function(reqt, body)\n"
				                                   "      return \"plain-fallback\", 599, { src = "
				                                   "\"plain\" }\n"
				                                   "    end\n"
				                                   "  }\n"
				                                   "end\n"
				                                   "package.preload[\"ltn12\"] = function()\n"
				                                   "  return {\n"
				                                   "    source = {\n"
				                                   "      string = function(value)\n"
				                                   "        local emitted = false\n"
				                                   "        return function()\n"
				                                   "          if emitted then return nil end\n"
				                                   "          emitted = true\n"
				                                   "          return value\n"
				                                   "        end\n"
				                                   "      end\n"
				                                   "    },\n"
				                                   "    sink = {\n"
				                                   "      table = function(target)\n"
				                                   "        return function(chunk)\n"
				                                   "          if chunk then target[#target + 1] = chunk "
				                                   "end\n"
				                                   "          return 1\n"
				                                   "        end\n"
				                                   "      end\n"
				                                   "    }\n"
				                                   "  }\n"
				                                   "end\n"
				                                   "package.preload[\"ssl.https\"] = function()\n"
				                                   "  return {\n"
				                                   "    request = function(req)\n"
				                                   "      local body = \"\"\n"
				                                   "      while true do\n"
				                                   "        local chunk = req.source and req.source()\n"
				                                   "        if chunk == nil then break end\n"
				                                   "        body = body .. chunk\n"
				                                   "      end\n"
				                                   "      if req.sink then req.sink(\"atlas-ok\") end\n"
				                                   "      return 1, 200, {\n"
				                                   "        method = req.method,\n"
				                                   "        ct = req.headers and "
				                                   "req.headers[\"content-type\"],\n"
				                                   "        cl = req.headers and "
				                                   "req.headers[\"content-length\"],\n"
				                                   "        body = body,\n"
				                                   "      }, \"HTTP/1.1 200 OK\"\n"
				                                   "    end\n"
				                                   "  }\n"
				                                   "end\n"
				                                   "local http = require(\"socket.http\")\n"
				                                   "local page, code, headers = "
				                                   "http.request(\"https://example.invalid/path\", "
				                                   "\"map=x\")\n"
				                                   "return tostring(page) .. \"|\" .. tostring(code) .. "
				                                   "\"|\" .. tostring(headers and headers.method) .. "
				                                   "\"|\" .. tostring(headers and headers.ct) .. \"|\" .. "
				                                   "tostring(headers and headers.cl) .. \"|\" .. "
				                                   "tostring(headers and headers.body)"));
				QVERIFY2(result.ok, qPrintable(result.error));
				QCOMPARE(result.value,
				         QStringLiteral("atlas-ok|200|POST|application/x-www-form-urlencoded|5|map=x"));
			}

			void socketHttpRequireShimFallsBackWhenSslModuleUnavailable()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				const auto result = evaluateLuaToString(
				    state.get(),
				    QByteArrayLiteral("package.preload[\"socket.http\"] = function()\n"
				                      "  return {\n"
				                      "    request = function(reqt, body)\n"
				                      "      return \"plain:\" .. tostring(reqt), 201, { src = "
				                      "\"plain\" }\n"
				                      "    end\n"
				                      "  }\n"
				                      "end\n"
				                      "package.loaded[\"ssl.https\"] = nil\n"
				                      "package.preload[\"ssl.https\"] = function()\n"
				                      "  error(\"ssl disabled for test\")\n"
				                      "end\n"
				                      "local http = require(\"socket.http\")\n"
				                      "local page, code, headers = "
				                      "http.request(\"https://example.invalid/path\", \"map=x\")\n"
				                      "return tostring(code) .. \"|\" .. "
				                      "tostring(headers and headers.src)"));
				QVERIFY2(result.ok, qPrintable(result.error));
				QCOMPARE(result.value, QStringLiteral("201|plain"));
			}

			void socketHttpRequireShimFallsBackToRawWhenSslHttpsRejectsCreateFunction()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				const auto result = evaluateLuaToString(
				    state.get(),
				    QByteArrayLiteral("package.preload[\"socket.http\"] = function()\n"
				                      "  return {\n"
				                      "    request = function(reqt, body)\n"
				                      "      return \"raw:\" .. tostring(reqt), 206, { src = "
				                      "\"raw\" }\n"
				                      "    end\n"
				                      "  }\n"
				                      "end\n"
				                      "package.preload[\"ssl.https\"] = function()\n"
				                      "  return {\n"
				                      "    request = function(reqt, body)\n"
				                      "      return nil, \"create function not permitted\"\n"
				                      "    end\n"
				                      "  }\n"
				                      "end\n"
				                      "local http = require(\"socket.http\")\n"
				                      "local page, code, headers = "
				                      "http.request(\"https://example.invalid/path\", \"map=x\")\n"
				                      "return tostring(page) .. \"|\" .. tostring(code) .. "
				                      "\"|\" .. tostring(headers and headers.src)"));
				QVERIFY2(result.ok, qPrintable(result.error));
				QCOMPARE(result.value, QStringLiteral("raw:https://example.invalid/path|206|raw"));
			}

			void socketHttpRequireShimPatchesRealModulesForHttps()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				const auto result = evaluateLuaToString(
				    state.get(),
				    QByteArrayLiteral("local ok_http, http = pcall(require, \"socket.http\")\n"
				                      "if not ok_http then\n"
				                      "  return \"no-http\"\n"
				                      "end\n"
				                      "local ok_https, https = pcall(require, \"ssl.https\")\n"
				                      "if not ok_https then\n"
				                      "  return \"no-https\"\n"
				                      "end\n"
				                      "if type(http) ~= \"table\" or type(http.request) ~= "
				                      "\"function\" then\n"
				                      "  return \"bad-http\"\n"
				                      "end\n"
				                      "if type(https) ~= \"table\" or type(https.request) ~= "
				                      "\"function\" then\n"
				                      "  return \"bad-https\"\n"
				                      "end\n"
				                      "local called = false\n"
				                      "local original = https.request\n"
				                      "https.request = function(reqt, body)\n"
				                      "  called = true\n"
				                      "  if type(reqt) == \"table\" then\n"
				                      "    if reqt.sink then reqt.sink(\"shim-ok\") end\n"
				                      "    return 1, 299, { src = \"ssl-real\" }\n"
				                      "  end\n"
				                      "  return \"shim-ok\", 299, { src = \"ssl-real\" }\n"
				                      "end\n"
				                      "local page, code, headers = "
				                      "http.request(\"https://example.invalid/path\")\n"
				                      "https.request = original\n"
				                      "return tostring(called) .. \"|\" .. tostring(page) .. \"|\" "
				                      ".. tostring(code) .. \"|\" .. tostring(headers and "
				                      "headers.src)"));

				QVERIFY2(result.ok, qPrintable(result.error));
				if (result.value == QStringLiteral("no-http") || result.value == QStringLiteral("no-https"))
					QSKIP("System Lua modules socket.http/ssl.https not available in this environment");
				QCOMPARE(result.value, QStringLiteral("true|shim-ok|299|ssl-real"));
			}

			void callLuaProtectedReturnsRuntimeErrorWithoutStackCorruption()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				QString execError;
				QVERIFY2(executeLuaChunk(state.get(),
				                         QByteArrayLiteral("function __qmud_test_explode()\n"
				                                           "  error('boom-callLuaProtected')\n"
				                                           "end"),
				                         execError),
				         qPrintable(execError));

				lua_getglobal(state.get(), "__qmud_test_explode");
				QVERIFY(lua_isfunction(state.get(), -1));
				const int status = QMudLuaSupport::callLuaProtected(state.get(), 0, 0, 0);
				QVERIFY(status != 0);
				QVERIFY(lua_isstring(state.get(), -1));
				const QString errorText = QString::fromUtf8(lua_tostring(state.get(), -1));
				QVERIFY(errorText.contains(QStringLiteral("boom-callLuaProtected")));
				lua_pop(state.get(), 1);
				QCOMPARE(lua_gettop(state.get()), 0);
			}

			void sqliteExecuteCallbackRuntimeErrorDoesNotAbortOrCorruptLuaState()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				QString sqliteError;
				QVERIFY2(openSqliteModule(state.get(), sqliteError), qPrintable(sqliteError));

				const auto result = evaluateLuaToString(
				    state.get(),
				    QByteArrayLiteral(
				        "local db = assert(sqlite3.open_memory())\n"
				        "assert(db:execute([["
				        "CREATE TABLE t(v INTEGER);"
				        "INSERT INTO t(v) VALUES (1);"
				        "INSERT INTO t(v) VALUES (2);"
				        "]]))\n"
				        "local calls = 0\n"
				        "local rc = db:execute('SELECT v FROM t ORDER BY v', function(_, ncols, row, cols)\n"
				        "  calls = calls + 1\n"
				        "  if calls == 1 then\n"
				        "    error('boom-sqlite-callback')\n"
				        "  end\n"
				        "  return 0\n"
				        "end)\n"
				        "db:close()\n"
				        "return tostring(rc) .. ':' .. tostring(calls)"));
				QVERIFY2(result.ok, qPrintable(result.error));
				const QStringList parts = result.value.split(':');
				QCOMPARE(parts.size(), 2);
				QString rcText = parts.at(0);
				rcText.replace(',', '.');
				bool         okRc = false;
				const double rc   = rcText.toDouble(&okRc);
				QVERIFY(okRc);
				QVERIFY(qAbs(rc) < 0.000001);
				QCOMPARE(parts.at(1), QStringLiteral("2"));
			}

			void sqliteNamedRowsAllowsNestedIteratorsOnSameConnection()
			{
				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);

				QString sqliteError;
				QVERIFY2(openSqliteModule(state.get(), sqliteError), qPrintable(sqliteError));

				const auto result = evaluateLuaToString(
				    state.get(),
				    QByteArrayLiteral(
				        "local db = assert(sqlite3.open_memory())\n"
				        "assert(db:execute([["
				        "CREATE TABLE rooms(uid TEXT, area TEXT);"
				        "CREATE TABLE bookmarks(uid TEXT, notes TEXT);"
				        "INSERT INTO rooms(uid, area) VALUES ('A0', 'A');"
				        "INSERT INTO rooms(uid, area) VALUES ('A1', 'A');"
				        "INSERT INTO rooms(uid, area) VALUES ('A2', 'A');"
				        "INSERT INTO bookmarks(uid, notes) VALUES ('A1', 'Keep');"
				        "]]))\n"
				        "local collected = {}\n"
				        "for row in db:nrows(\"SELECT uid FROM rooms WHERE area = 'A' ORDER BY uid\") do\n"
				        "  local note = nil\n"
				        "  for note_row in db:nrows(string.format(\"SELECT notes FROM bookmarks WHERE uid = "
				        "%s\", string.format(\"'%s'\", row.uid))) do\n"
				        "    note = note_row.notes\n"
				        "  end\n"
				        "  if note and note ~= '' then\n"
				        "    collected[row.uid] = note\n"
				        "  end\n"
				        "end\n"
				        "db:close()\n"
				        "return tostring(collected['A1'] ~= nil) .. ':' .. tostring(next(collected) ~= "
				        "nil)"));
				QVERIFY2(result.ok, qPrintable(result.error));
				QCOMPARE(result.value, QStringLiteral("true:true"));
			}

			void sqliteFileWritePersistsAfterLuaStateShutdown()
			{
				QTemporaryDir tempDir(
				    QDir::current().filePath(QStringLiteral("tst_LuaExecution_sqlite_XXXXXX")));
				QVERIFY(tempDir.isValid());
				const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("plugin_state.sqlite"));

				{
					LuaStateOwner state = makeCompatLuaState();
					QVERIFY(state);
					QString sqliteError;
					QVERIFY2(openSqliteModule(state.get(), sqliteError), qPrintable(sqliteError));

					const QByteArray script =
					    QStringLiteral("local db = assert(sqlite3.open([[%1]]))\n"
					                   "assert(db:execute([["
					                   "CREATE TABLE plugin_state(k TEXT PRIMARY KEY, v TEXT);"
					                   "INSERT INTO plugin_state(k, v) VALUES ('mini_rect', '44,55,64,40');"
					                   "]]))\n"
					                   "assert(db:close())\n")
					        .arg(dbPath)
					        .toUtf8();
					QString execError;
					QVERIFY2(executeLuaChunk(state.get(), script, execError), qPrintable(execError));
				}

				LuaStateOwner state = makeCompatLuaState();
				QVERIFY(state);
				QString sqliteError;
				QVERIFY2(openSqliteModule(state.get(), sqliteError), qPrintable(sqliteError));

				const QByteArray verifyScript =
				    QStringLiteral(
				        "local db = assert(sqlite3.open([[%1]]))\n"
				        "local value = nil\n"
				        "for row in db:nrows(\"SELECT v FROM plugin_state WHERE k = 'mini_rect'\") do\n"
				        "  value = row.v\n"
				        "end\n"
				        "assert(db:close())\n"
				        "return value or ''\n")
				        .arg(dbPath)
				        .toUtf8();
				const auto result = evaluateLuaToString(state.get(), verifyScript);
				QVERIFY2(result.ok, qPrintable(result.error));
				QCOMPARE(result.value, QStringLiteral("44,55,64,40"));
			}
			// NOLINTEND(readability-convert-member-functions-to-static)
	};
} // namespace

QTEST_MAIN(tst_LuaExecution)

#if __has_include("tst_LuaExecution.moc")
#include "tst_LuaExecution.moc"
#endif
