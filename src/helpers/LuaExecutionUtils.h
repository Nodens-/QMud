/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: LuaExecutionUtils.h
 * Role: Shared Lua execution helpers for backend mode selection, thread-affinity assertions,
 * and CallPlugin Lua marshalling.
 */

#ifndef QMUD_LUAEXECUTIONUTILS_H
#define QMUD_LUAEXECUTIONUTILS_H

#include <QScopeGuard>
#include <QString>
#include <functional>
#include <memory>

class QObject;
class QThread;
struct lua_State;

/**
 * @brief Lifetime-safe endpoint for posting work to a captured Lua bridge thread.
 */
class LuaBridgeAsyncTarget final
{
	public:
		/**
		 * @brief Creates an invalid endpoint.
		 */
		LuaBridgeAsyncTarget() = default;

		/**
		 * @brief Reports whether the captured bridge context is still available.
		 * @return `true` when work can be posted through this endpoint.
		 */
		[[nodiscard]] bool isValid() const
		{
			return static_cast<bool>(m_context);
		}

	private:
		explicit LuaBridgeAsyncTarget(std::shared_ptr<void> context) : m_context(std::move(context))
		{
		}

		std::shared_ptr<void>       m_context;

		friend LuaBridgeAsyncTarget qmudLuaBridgeCaptureAsyncTarget(QThread *thread);
		friend bool qmudLuaBridgePost(const LuaBridgeAsyncTarget &target, std::function<void()> fn,
		                              std::function<void()> canceled, quintptr asynchronousPumpKey);
};

/**
 * @brief Last status classification for Lua bridge invocation on the current thread.
 */
enum class LuaBridgeInvokeStatus
{
	Success,
	InvalidTargetOrCallback,
	NoOwningThread,
	MissingTargetBridgeContext,
	NoQCoreApplication,
	TargetThreadNotRunning,
	TargetThreadNoEventDispatcher,
	BridgeInvokerUnavailable,
	BridgeEndpointNotReadyTimeout,
	MissingCallerBridgeContext,
	EnqueueFailed,
	RequestCanceledBeforeDispatch,
	RequestCanceledTargetThreadStopped,
	TimedOutInFlight,
	UnknownFailure
};

/**
 * @brief Error classification for Lua CallPlugin marshaling/invocation.
 */
enum class CallPluginLuaMarshallingError
{
	None,
	NoSuchRoutine,
	UnsupportedArgumentType,
	RuntimeError,
	UnsupportedReturnType
};

/**
 * @brief Result of invoking/marshaling a Lua callback between caller/target states.
 */
struct CallPluginLuaMarshallingResult
{
		CallPluginLuaMarshallingError error{CallPluginLuaMarshallingError::None};
		int                           index{0};
		QByteArray                    typeName;
		QString                       runtimeError;
		int                           returnCount{0};
};

/**
 * @brief Calls a dotted Lua function name and marshals values between Lua states.
 * @param callerState Caller-side Lua state where arguments and final return values live.
 * @param targetState Target Lua state where routine is executed.
 * @param routine Dotted routine name (`foo.bar.baz`) to invoke.
 * @param firstArg 1-based caller stack index of first argument for routine call.
 * @return Classification + details of the invocation/marshaling result.
 */
CallPluginLuaMarshallingResult qmudCallPluginLuaWithMarshalling(lua_State     *callerState,
                                                                lua_State     *targetState,
                                                                const QString &routine, int firstArg);

/**
 * @brief Returns whether two Lua thread handles share the same main Lua state.
 * @param left First Lua state/thread handle.
 * @param right Second Lua state/thread handle.
 * @return `true` when both handles belong to the same Lua VM.
 */
[[nodiscard]] bool             qmudLuaStatesShareMainThread(lua_State *left, lua_State *right);

/**
 * @brief Debug-only assertion that verifies an object is used from its owning Qt thread.
 * @param object Object whose affinity should match current thread.
 * @param context Assertion context label.
 */
void                           qmudAssertObjectThreadAffinity(const QObject *object, const char *context);
/**
 * @brief Completes one Lua worker callback dispatch on the runtime thread.
 *
 * The worker-in-flight marker is cleared before the result is consumed so deferred
 * runtime mutations may perform synchronous callback work without blocking behind
 * the worker dispatch that already finished. Pending queue draining runs after the
 * result consumer.
 *
 * @param workerInFlight Worker-in-flight state to clear before consuming the result.
 * @param consumeResult Callback that applies the completed worker result.
 * @param drainQueuedWork Callback that schedules any pending queued callback work.
 */
template <typename ConsumeResult, typename DrainQueuedWork>
void qmudCompleteLuaWorkerCallbackDispatch(bool &workerInFlight, ConsumeResult &&consumeResult,
                                           DrainQueuedWork &&drainQueuedWork)
{
	workerInFlight         = false;
	const auto drainQueued = qScopeGuard([&drainQueuedWork] { drainQueuedWork(); });
	consumeResult();
}
/**
 * @brief Ensures a target object's bridge endpoint is in Running state.
 * @param target Target QObject.
 * @return `true` when target thread bridge endpoint is ready to accept bridge calls.
 */
bool                               qmudLuaBridgeEnsureObjectThreadReady(const QObject *target);
/**
 * @brief Captures the stable Lua bridge endpoint for a thread.
 * @param thread Thread whose endpoint will receive later asynchronous work.
 * @return Opaque endpoint handle, or an invalid handle for a null thread.
 */
[[nodiscard]] LuaBridgeAsyncTarget qmudLuaBridgeCaptureAsyncTarget(QThread *thread);
/**
 * @brief Posts work through a captured thread bridge endpoint.
 * @param target Previously captured endpoint.
 * @param fn Work to execute on the endpoint thread.
 * @param canceled Called exactly once instead when the endpoint cannot execute the work.
 * @param asynchronousPumpKey Optional stable target identity used for targeted cooperative pumping.
 * @return `true` when the request was queued; otherwise `false` after calling `canceled`.
 */
bool    qmudLuaBridgePost(const LuaBridgeAsyncTarget &target, std::function<void()> fn,
                          std::function<void()> canceled, quintptr asynchronousPumpKey = 0);
/**
 * @brief Pumps one asynchronous bridge post associated with a specific logical target.
 * @param asynchronousPumpKey Stable target identity supplied when the post was queued.
 * @return `true` when one matching request was executed.
 */
bool    qmudLuaBridgePumpCurrentThreadAsyncOnce(quintptr asynchronousPumpKey);
/**
 * @brief Pumps one pending bridge request on the current thread, if any.
 * @return `true` when one request was executed; otherwise `false`.
 */
bool    qmudLuaBridgePumpCurrentThreadOnce();
/**
 * @brief Registers cooperative wait-work pump for current thread's bridge wait loop.
 * @param pump Callback that may execute one unit of local work and return `true` when work ran.
 */
void    qmudLuaBridgeSetCurrentThreadWaitWorkPump(std::function<bool()> pump);
/**
 * @brief Clears cooperative wait-work pump for current thread.
 */
void    qmudLuaBridgeClearCurrentThreadWaitWorkPump();
/**
 * @brief Waits for the current thread's Lua-bridge wake channel.
 * @param timeoutMs Maximum wait in milliseconds; values <= 0 are treated as 1ms.
 * @return `true` when the wake channel signaled during the wait.
 */
bool    qmudLuaBridgeWaitForCurrentThreadWake(int timeoutMs);
/**
 * @brief Signals the Lua-bridge wake channel for a target thread.
 * @param thread Target thread whose bridge wake channel should be signaled.
 */
void    qmudLuaBridgeNotifyThreadWake(QThread *thread);
/**
 * @brief Returns `true` while current thread is executing a Lua bridge request callback.
 * @return `true` when inside bridge request execution on current thread.
 */
bool    qmudLuaBridgeIsExecutingRequestOnCurrentThread();
/**
 * @brief Synchronously executes a callback on `target` object's thread via dedicated Lua bridge channel.
 *
 * Invoke timeout bounds queued pre-dispatch wait only. If dispatch has already
 * started executing on the target thread, this call keeps cooperative waiting
 * until completion to preserve ordering/correctness semantics.
 *
 * @param target Target QObject whose owning thread should execute the callback.
 * @param fn Callback to execute on target thread.
 * @return `true` when callback was executed, `false` when target/context is invalid.
 */
bool    qmudLuaBridgeInvokeOnObjectThread(const QObject *target, const std::function<void()> &fn);
/**
 * @brief Synchronously executes modal dialog work on `target` object's thread.
 *
 * This is intentionally narrower than qmudLuaBridgeInvokeOnObjectThread(): the caller blocks only on this
 * modal request's completion condition and does not pump caller-side bridge, callback-lane, or runtime work
 * while the modal UI is open. The invoke timeout only cancels work that is still queued before dispatch; once
 * the GUI thread has started the modal callable, the caller waits for the dialog result.
 *
 * @param target Target QObject whose owning thread should execute the modal callback.
 * @param fn Modal callback to execute on target thread.
 * @return `true` when callback dispatch completed successfully, `false` when target/context is invalid or queued
 * dispatch was canceled before execution.
 */
bool    qmudLuaBridgeInvokeModalOnObjectThread(const QObject *target, const std::function<void()> &fn);
/**
 * @brief Returns current Lua bridge invoke timeout in milliseconds.
 * @return Active invoke timeout in milliseconds.
 */
int     qmudLuaBridgeInvokeTimeoutMs();
/**
 * @brief Overrides Lua bridge invoke timeout for the current process.
 *
 * The timeout is used for queued pre-dispatch wait/cancellation. In-flight
 * requests continue to completion.
 *
 * @param timeoutMs New timeout in milliseconds. Values <= 0 reset to default.
 */
void    qmudSetLuaBridgeInvokeTimeoutMs(int timeoutMs);
/**
 * @brief Returns last bridge failure reason for current thread.
 * @return Human-readable failure reason, or empty string when no error was recorded.
 */
QString qmudLuaBridgeLastError();
/**
 * @brief Returns last bridge status classification for current thread.
 * @return Last bridge status.
 */
LuaBridgeInvokeStatus qmudLuaBridgeLastStatus();

#endif // QMUD_LUAEXECUTIONUTILS_H
