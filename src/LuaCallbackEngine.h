/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: LuaCallbackEngine.h
 * Role: Core Lua bridge interfaces for callback dispatch, API exposure, and script-driven interaction with world
 * runtime state.
 */

#ifndef QMUD_LUACALLBACKENGINE_H
#define QMUD_LUACALLBACKENGINE_H

#include "LuaExecutor.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QByteArray>
// ReSharper disable once CppUnusedIncludeDirective
#include <QHash>
#include <QSet>
#include <QSharedPointer>
#include <QString>
#include <QThread>
// ReSharper disable once CppUnusedIncludeDirective
#include <QVector>
#include <functional>
#include <memory>

#ifdef QMUD_ENABLE_LUA_SCRIPTING
struct lua_State;
struct CallPluginLuaMarshallingResult;
struct LuaSuspendedCallback;

struct LuaStateDeleter
{
		/**
		 * @brief Closes Lua state for unique_ptr ownership.
		 * @param state Lua state pointer to close.
		 */
		void operator()(lua_State *state) const;
};

enum class LuaPreparedCallbackResultMode
{
	Bool,
	NoResult,
	Bytes,
	String
};
#endif

/**
 * @brief Styled text run produced/consumed by Lua-facing text APIs.
 */
struct LuaStyleRun
{
		QString text;
		int     textColour{0};
		int     backColour{0};
		int     style{0};
};

struct LuaCallbackMiniWindowSnapshot;

/**
 * @brief Lua VM integration layer for world/plugin callback execution.
 *
 * Owns Lua state lifetime, binds exported APIs, and marshals calls between C++
 * runtime objects and script-side functions.
 */
class LuaCallbackEngine
{
	public:
		using CallbackCatalogObserver =
		    std::function<void(const QString &, const QSet<QString> &, const QSet<QString> &)>;

		/**
		 * @brief Constructs callback engine with no loaded script.
		 */
		LuaCallbackEngine();
		/**
		 * @brief Destroys Lua state and callback resources.
		 */
		~LuaCallbackEngine();

		/**
		 * @brief Binds runtime context visible to script callbacks.
		 * @param runtime Runtime context pointer.
		 */
		void                              setWorldRuntime(class WorldRuntime *runtime);
		/**
		 * @brief Swaps runtime context and returns previous runtime pointer.
		 * @param runtime New runtime context pointer.
		 * @return Previous runtime context pointer.
		 */
		class WorldRuntime               *swapWorldRuntime(class WorldRuntime *runtime);
		/**
		 * @brief Returns currently bound runtime.
		 * @return Bound runtime pointer, or `nullptr`.
		 */
		[[nodiscard]] class WorldRuntime *worldRuntime() const;
		/**
		 * @brief Returns bound runtime pointer for APIs that perform their own runtime-thread bridging.
		 * @return Bound runtime pointer, or `nullptr`.
		 */
		[[nodiscard]] class WorldRuntime *worldRuntimeForBridgedCall() const;
		/**
		 * @brief Sets plugin identity metadata used by callback context.
		 * @param id Plugin id.
		 * @param name Plugin display name.
		 * @param directory Plugin directory (legacy type-20 GetPluginInfo value).
		 */
		void setPluginInfo(const QString &id, const QString &name, const QString &directory = QString());
		/**
		 * @brief Returns bound plugin id.
		 * @return Plugin id.
		 */
		[[nodiscard]] QString pluginId() const;
		/**
		 * @brief Returns plugin id metadata without binding Lua execution-thread affinity.
		 * @return Plugin id metadata.
		 */
		[[nodiscard]] QString pluginIdMetadata() const;
		/**
		 * @brief Returns bound plugin display name.
		 * @return Plugin display name.
		 */
		[[nodiscard]] QString pluginName() const;
		/**
		 * @brief Returns plugin display-name metadata without binding Lua execution-thread affinity.
		 * @return Plugin display-name metadata.
		 */
		[[nodiscard]] QString pluginNameMetadata() const;
		/**
		 * @brief Returns bound plugin directory (legacy type-20 GetPluginInfo value).
		 * @return Plugin directory with trailing separator when known.
		 */
		[[nodiscard]] QString pluginDirectory() const;
		/**
		 * @brief Pushes per-call caller plugin context for `GetPluginInfo(..., 23)`.
		 * @param pluginId Calling plugin id for the current call scope.
		 */
		void                  pushCallingPluginId(const QString &pluginId);
		/**
		 * @brief Pops per-call caller plugin context.
		 */
		void                  popCallingPluginId();
		/**
		 * @brief Returns current per-call caller plugin context.
		 * @return Calling plugin id for the active call scope, or empty when not in such scope.
		 */
		[[nodiscard]] QString currentCallingPluginId() const;
		/**
		 * @brief Replaces current Lua script source text.
		 * @param script Lua script source text.
		 */
		void                  setScriptText(const QString &script);
		/**
		 * @brief Loads/compiles current script text into Lua state.
		 * @return `true` on successful load/compile.
		 */
		bool                  loadScript();
		/**
		 * @brief Resets VM and callback state while keeping metadata.
		 */
		void                  resetState();
		/**
		 * @brief Checks whether Lua function exists in current state.
		 * @param functionName Lua function name.
		 * @return `true` when function exists.
		 */
		bool                  hasFunction(const QString &functionName);
		/**
		 * @brief Calls MXP error callback with protocol diagnostics.
		 * @param functionName Callback function name.
		 * @param level Error level.
		 * @param messageNumber Protocol message number.
		 * @param lineNumber Related line number.
		 * @param message Error message text.
		 * @param suspended Optional output flag set when callback yielded at a modal API.
		 * @param modalResumeId Optional output id for the suspended modal callback.
		 * @param pendingModalStringRequest Optional output request for the yielded modal API.
		 * @return `true` when callback returns truthy/continue semantics.
		 */
		bool callMxpError(const QString &functionName, int level, long messageNumber, int lineNumber,
		                  const QString &message, bool *suspended = nullptr, quint64 *modalResumeId = nullptr,
		                  LuaPendingModalStringRequest *pendingModalStringRequest = nullptr);
		/**
		 * @brief Calls MXP startup callback.
		 * @param functionName Callback function name.
		 * @param suspended Optional output flag set when callback yielded at a modal API.
		 * @param modalResumeId Optional output id for the suspended modal callback.
		 * @param pendingModalStringRequest Optional output request for the yielded modal API.
		 */
		void callMxpStartUp(const QString &functionName, bool *suspended = nullptr,
		                    quint64                      *modalResumeId             = nullptr,
		                    LuaPendingModalStringRequest *pendingModalStringRequest = nullptr);
		/**
		 * @brief Calls MXP shutdown callback.
		 * @param functionName Callback function name.
		 * @param suspended Optional output flag set when callback yielded at a modal API.
		 * @param modalResumeId Optional output id for the suspended modal callback.
		 * @param pendingModalStringRequest Optional output request for the yielded modal API.
		 */
		void callMxpShutDown(const QString &functionName, bool *suspended = nullptr,
		                     quint64                      *modalResumeId             = nullptr,
		                     LuaPendingModalStringRequest *pendingModalStringRequest = nullptr);
		/**
		 * @brief Calls MXP start-tag callback.
		 * @param functionName Callback function name.
		 * @param name Tag name.
		 * @param args Raw argument string.
		 * @param table Parsed attribute table.
		 * @param suspended Optional output flag set when callback yielded at a modal API.
		 * @param modalResumeId Optional output id for the suspended modal callback.
		 * @param pendingModalStringRequest Optional output request for the yielded modal API.
		 * @return `true` when callback returns truthy/continue semantics.
		 */
		bool callMxpStartTag(const QString &functionName, const QString &name, const QString &args,
		                     const QMap<QString, QString> &table, bool *suspended = nullptr,
		                     quint64                      *modalResumeId             = nullptr,
		                     LuaPendingModalStringRequest *pendingModalStringRequest = nullptr);
		/**
		 * @brief Calls MXP end-tag callback.
		 * @param functionName Callback function name.
		 * @param name Tag name.
		 * @param text Tag text content.
		 * @param suspended Optional output flag set when callback yielded at a modal API.
		 * @param modalResumeId Optional output id for the suspended modal callback.
		 * @param pendingModalStringRequest Optional output request for the yielded modal API.
		 */
		void callMxpEndTag(const QString &functionName, const QString &name, const QString &text,
		                   bool *suspended = nullptr, quint64 *modalResumeId = nullptr,
		                   LuaPendingModalStringRequest *pendingModalStringRequest = nullptr);
		/**
		 * @brief Calls MXP variable callback.
		 * @param functionName Callback function name.
		 * @param name Variable name.
		 * @param contents Variable contents.
		 * @param suspended Optional output flag set when callback yielded at a modal API.
		 * @param modalResumeId Optional output id for the suspended modal callback.
		 * @param pendingModalStringRequest Optional output request for the yielded modal API.
		 */
		void callMxpSetVariable(const QString &functionName, const QString &name, const QString &contents,
		                        bool *suspended = nullptr, quint64 *modalResumeId = nullptr,
		                        LuaPendingModalStringRequest *pendingModalStringRequest = nullptr);
		/**
		 * @brief Calls function with no arguments and boolean result.
		 * @param functionName Callback function name.
		 * @param hasFunction Optional output flag indicating function existence.
		 * @param defaultResult Result when function is missing/error.
		 * @param actionSourceOverride Optional callback-local action source, or `-1` to use runtime state.
		 * @param suspended Optional output flag set when callback yielded at a modal API.
		 * @param modalResumeId Optional output id for the suspended modal callback.
		 * @param pendingModalStringRequest Optional output request for the yielded modal API.
		 * @return Callback result.
		 */
		bool callFunctionNoArgs(const QString &functionName, bool *hasFunction = nullptr,
		                        bool defaultResult = true, int actionSourceOverride = -1,
		                        bool *suspended = nullptr, quint64 *modalResumeId = nullptr,
		                        LuaPendingModalStringRequest *pendingModalStringRequest = nullptr);
		/**
		 * @brief Calls function with one string argument and ignores return value semantics.
		 * @param functionName Callback function name.
		 * @param arg String argument.
		 * @param hasFunction Optional output flag indicating function existence.
		 * @param suspended Optional output flag set when callback yielded at a modal API.
		 * @param modalResumeId Optional output id for the suspended modal callback.
		 * @param pendingModalStringRequest Optional output request for the yielded modal API.
		 * @return `true` when function exists and executes without Lua error.
		 */
		bool callProcedureWithString(const QString &functionName, const QString &arg,
		                             bool *hasFunction = nullptr, bool *suspended = nullptr,
		                             quint64                      *modalResumeId             = nullptr,
		                             LuaPendingModalStringRequest *pendingModalStringRequest = nullptr);
		/**
		 * @brief Calls a dotted Lua routine and marshals arguments/returns to another Lua state.
		 *
		 * Executes under callback script scope so callback-path deferred runtime mutation semantics
		 * are preserved for cross-plugin `CallPlugin` flows.
		 *
		 * @param callerState Caller Lua state receiving marshaled return values.
		 * @param routine Dotted routine name.
		 * @param firstArg 1-based stack index for first routine argument in caller state.
		 * @param miniWindowNamesSnapshot Optional miniwindow-name snapshot used to seed callback
		 * existence cache for bridge-forbidden callback contexts when full snapshot is unavailable.
		 * @param miniWindowSnapshot Optional full callback dispatch snapshot used to seed
		 * callback-scope API read caches.
		 * @return Marshaling/invocation result classification.
		 */
		CallPluginLuaMarshallingResult callPluginLuaWithMarshalling(
		    lua_State *callerState, const QString &routine, int firstArg,
		    const QStringList                                         &miniWindowNamesSnapshot = {},
		    const QSharedPointer<const LuaCallbackMiniWindowSnapshot> &miniWindowSnapshot      = {});
		/**
		 * @brief Calls function with one string argument.
		 * @param functionName Callback function name.
		 * @param arg String argument.
		 * @param hasFunction Optional output flag indicating function existence.
		 * @param defaultResult Result when function is missing/error.
		 * @param suspended Optional output flag set when callback yielded at a modal API.
		 * @param modalResumeId Optional output id for the suspended modal callback.
		 * @param pendingModalStringRequest Optional output request for the yielded modal API.
		 * @return Callback result.
		 */
		bool callFunctionWithString(const QString &functionName, const QString &arg,
		                            bool *hasFunction = nullptr, bool defaultResult = true,
		                            bool *suspended = nullptr, quint64 *modalResumeId = nullptr,
		                            LuaPendingModalStringRequest *pendingModalStringRequest = nullptr);
		/**
		 * @brief Calls function with one byte-array argument.
		 * @param functionName Callback function name.
		 * @param arg Byte-array argument.
		 * @param hasFunction Optional output flag indicating function existence.
		 * @param defaultResult Result when function is missing/error.
		 * @param suspended Optional output flag set when callback yielded at a modal API.
		 * @param modalResumeId Optional output id for the suspended modal callback.
		 * @param pendingModalStringRequest Optional output request for the yielded modal API.
		 * @return Callback result.
		 */
		bool callFunctionWithBytes(const QString &functionName, const QByteArray &arg,
		                           bool *hasFunction = nullptr, bool defaultResult = true,
		                           bool *suspended = nullptr, quint64 *modalResumeId = nullptr,
		                           LuaPendingModalStringRequest *pendingModalStringRequest = nullptr);
		/**
		 * @brief Calls function with mutable byte-array in/out argument.
		 * @param functionName Callback function name.
		 * @param arg Byte-array argument modified in place.
		 * @param hasFunction Optional output flag indicating function existence.
		 * @param suspended Optional output flag set when callback yielded at a modal API.
		 * @param modalResumeId Optional output id for the suspended modal callback.
		 * @param pendingModalStringRequest Optional output request for the yielded modal API.
		 * @return Callback result.
		 */
		bool callFunctionWithBytesInOut(const QString &functionName, QByteArray &arg,
		                                bool *hasFunction = nullptr, bool *suspended = nullptr,
		                                quint64                      *modalResumeId             = nullptr,
		                                LuaPendingModalStringRequest *pendingModalStringRequest = nullptr);
		/**
		 * @brief Calls function with mutable string in/out argument.
		 * @param functionName Callback function name.
		 * @param arg String argument modified in place.
		 * @param hasFunction Optional output flag indicating function existence.
		 * @param suspended Optional output flag set when callback yielded at a modal API.
		 * @param modalResumeId Optional output id for the suspended modal callback.
		 * @param pendingModalStringRequest Optional output request for the yielded modal API.
		 * @return Callback result.
		 */
		bool callFunctionWithStringInOut(const QString &functionName, QString &arg,
		                                 bool *hasFunction = nullptr, bool *suspended = nullptr,
		                                 quint64                      *modalResumeId             = nullptr,
		                                 LuaPendingModalStringRequest *pendingModalStringRequest = nullptr);
		/**
		 * @brief Calls function with one number and one string argument.
		 * @param functionName Callback function name.
		 * @param arg1 Numeric argument.
		 * @param arg String argument.
		 * @param hasFunction Optional output flag indicating function existence.
		 * @param defaultResult Result when function is missing/error.
		 * @param actionSourceOverride Optional callback-local action source, or `-1` to use runtime state.
		 * @param suspended Optional output flag set when callback yielded at a modal API.
		 * @param modalResumeId Optional output id for the suspended modal callback.
		 * @param pendingModalStringRequest Optional output request for the yielded modal API.
		 * @return Callback result.
		 */
		bool
		callFunctionWithNumberAndString(const QString &functionName, long arg1, const QString &arg,
		                                bool *hasFunction = nullptr, bool defaultResult = true,
		                                int actionSourceOverride = -1, bool *suspended = nullptr,
		                                quint64                      *modalResumeId             = nullptr,
		                                LuaPendingModalStringRequest *pendingModalStringRequest = nullptr);
		/**
		 * @brief Calls function with one number and three string arguments.
		 * @param functionName Callback function name.
		 * @param arg1 Numeric argument.
		 * @param arg2 First string argument.
		 * @param arg3 Second string argument.
		 * @param arg4 Third string argument.
		 * @param hasFunction Optional output flag indicating function existence.
		 * @param defaultResult Result when function is missing/error.
		 * @return Callback result.
		 */
		bool callFunctionWithNumberAndStrings(const QString &functionName, long arg1, const QString &arg2,
		                                      const QString &arg3, const QString &arg4,
		                                      bool *hasFunction = nullptr, bool defaultResult = true);
		/**
		 * @brief Calls function with one number and three pre-encoded UTF-8 string arguments.
		 * @param functionName Callback function name.
		 * @param arg1 Numeric argument.
		 * @param arg2Utf8 First string argument encoded as UTF-8 bytes.
		 * @param arg3Utf8 Second string argument encoded as UTF-8 bytes.
		 * @param arg4Utf8 Third string argument encoded as UTF-8 bytes.
		 * @param hasFunction Optional output flag indicating function existence.
		 * @param defaultResult Result when function is missing/error.
		 * @param suspended Optional output flag set when callback yielded at a modal API.
		 * @param modalResumeId Optional output id for the suspended modal callback.
		 * @param pendingModalStringRequest Optional output request for the yielded modal API.
		 * @return Callback result.
		 */
		bool callFunctionWithNumberAndUtf8Strings(
		    const QString &functionName, long arg1, const QByteArray &arg2Utf8, const QByteArray &arg3Utf8,
		    const QByteArray &arg4Utf8, bool *hasFunction = nullptr, bool defaultResult = true,
		    bool *suspended = nullptr, quint64 *modalResumeId = nullptr,
		    LuaPendingModalStringRequest *pendingModalStringRequest = nullptr);
		/**
		 * @brief Calls function with two numbers and one string argument.
		 * @param functionName Callback function name.
		 * @param arg1 First numeric argument.
		 * @param arg2 Second numeric argument.
		 * @param arg String argument.
		 * @param hasFunction Optional output flag indicating function existence.
		 * @param defaultResult Result when function is missing/error.
		 * @param suspended Optional output flag set when callback yielded at a modal API.
		 * @param modalResumeId Optional output id for the suspended modal callback.
		 * @param pendingModalStringRequest Optional output request for the yielded modal API.
		 * @return Callback result.
		 */
		bool callFunctionWithTwoNumbersAndString(
		    const QString &functionName, long arg1, long arg2, const QString &arg,
		    bool *hasFunction = nullptr, bool defaultResult = true, bool *suspended = nullptr,
		    quint64                      *modalResumeId             = nullptr,
		    LuaPendingModalStringRequest *pendingModalStringRequest = nullptr);
		/**
		 * @brief Calls function with one number and byte-array argument.
		 * @param functionName Callback function name.
		 * @param arg1 Numeric argument.
		 * @param arg Byte-array argument.
		 * @param hasFunction Optional output flag indicating function existence.
		 * @param defaultResult Result when function is missing/error.
		 * @param suspended Optional output flag set when callback yielded at a modal API.
		 * @param modalResumeId Optional output id for the suspended modal callback.
		 * @param pendingModalStringRequest Optional output request for the yielded modal API.
		 * @return Callback result.
		 */
		bool
		callFunctionWithNumberAndBytes(const QString &functionName, long arg1, const QByteArray &arg,
		                               bool *hasFunction = nullptr, bool defaultResult = true,
		                               bool *suspended = nullptr, quint64 *modalResumeId = nullptr,
		                               LuaPendingModalStringRequest *pendingModalStringRequest = nullptr);
		/**
		 * @brief Calls function with argument list, wildcard sets, and optional style runs.
		 * @param functionName Callback function name.
		 * @param args Positional string arguments.
		 * @param wildcards Positional wildcard values.
		 * @param namedWildcards Named wildcard values.
		 * @param styleRuns Optional style-run list.
		 * @param miniWindowSnapshot Optional runtime-thread miniwindow snapshot for callback cache seeding.
		 * @param hasFunction Optional output flag indicating function existence.
		 * @param actionSourceOverride Optional callback-local action source, or `-1` to use runtime state.
		 * @param triggerOutputReplacesMatchedLine Whether trigger output should replace the matched line.
		 * @param triggerMatchedLineBufferIndex Buffer index of the trigger-matched line.
		 * @param triggerMatchedLineAbsoluteNumber Absolute line number of the trigger-matched line.
		 * @param suspended Optional output flag set when callback yielded at a modal API.
		 * @param modalResumeId Optional output id for the suspended modal callback.
		 * @param pendingModalStringRequest Optional output request for the yielded modal API.
		 * @return Callback result.
		 */
		bool callFunctionWithStringsAndWildcards(
		    const QString &functionName, const QStringList &args, const QStringList &wildcards,
		    const QMap<QString, QString> &namedWildcards, const QVector<LuaStyleRun> *styleRuns,
		    const LuaCallbackMiniWindowSnapshot *miniWindowSnapshot, bool *hasFunction = nullptr,
		    int actionSourceOverride = -1, bool triggerOutputReplacesMatchedLine = false,
		    int triggerMatchedLineBufferIndex = 0, qint64 triggerMatchedLineAbsoluteNumber = 0,
		    bool *suspended = nullptr, quint64 *modalResumeId = nullptr,
		    LuaPendingModalStringRequest *pendingModalStringRequest = nullptr);
		/**
		 * @brief Executes arbitrary Lua code chunk.
		 * @param code Lua code text.
		 * @param description Description used for diagnostics.
		 * @param styleRuns Optional style-run context.
		 * @param hasTriggerContext Whether the script executes with trigger-line context.
		 * @param actionSourceOverride Optional callback-local action source, or `-1` to use runtime state.
		 * @param triggerOutputReplacesMatchedLine Whether trigger output should replace the matched line.
		 * @param triggerMatchedLineBufferIndex Buffer index of the trigger-matched line.
		 * @param triggerMatchedLineAbsoluteNumber Absolute line number of the trigger-matched line.
		 * @param suspended Optional output flag set when callback yielded at a modal API.
		 * @param modalResumeId Optional output id for the suspended modal callback.
		 * @param pendingModalStringRequest Optional output request for the yielded modal API.
		 * @return `true` on successful execution.
		 */
		bool executeScript(const QString &code, const QString &description,
		                   const QVector<LuaStyleRun> *styleRuns = nullptr, bool hasTriggerContext = false,
		                   int actionSourceOverride = -1, bool triggerOutputReplacesMatchedLine = false,
		                   int triggerMatchedLineBufferIndex = 0, qint64 triggerMatchedLineAbsoluteNumber = 0,
		                   bool *suspended = nullptr, quint64 *modalResumeId = nullptr,
		                   LuaPendingModalStringRequest *pendingModalStringRequest = nullptr);
		/**
		 * @brief Enables/disables package loading restrictions.
		 * @param enablePackage Enable package access when `true`.
		 */
		void applyPackageRestrictions(bool enablePackage);
		/**
		 * @brief Returns current nested script execution depth.
		 * @return Current script-execution depth.
		 */
		[[nodiscard]] int scriptExecutionDepth() const;
		/**
		 * @brief Increments script execution depth guard.
		 */
		void              pushScriptExecutionDepth();
		/**
		 * @brief Decrements script execution depth guard.
		 */
		void              popScriptExecutionDepth();
#ifdef QMUD_ENABLE_LUA_SCRIPTING
		/**
		 * @brief Returns underlying Lua state pointer.
		 * @return Underlying Lua state pointer, or `nullptr`.
		 */
		[[nodiscard]] lua_State *luaState() const;
#endif
		/**
		 * @brief Returns set of discovered Lua function names.
		 * @return Immutable set of discovered function names.
		 */
		[[nodiscard]] const QSet<QString> &luaFunctionsSet() const;
		/**
		 * @brief Assigns observer invoked when Lua callback catalog changes.
		 * @param observer Observer callback.
		 */
		void                               setCallbackCatalogObserver(CallbackCatalogObserver observer);
		/**
		 * @brief Sets tracked plugin-callback names whose presence should be monitored.
		 * @param callbackNames Callback names to monitor.
		 */
		void                               setObservedPluginCallbacks(const QSet<QString> &callbackNames);
		/**
		 * @brief Returns whether monitored callback is currently available.
		 * @param functionName Callback function name.
		 * @return `true` when callback exists in current Lua state.
		 */
		[[nodiscard]] bool                 hasObservedPluginCallback(const QString &functionName) const;
		/**
		 * @brief Refreshes monitored callback-presence map and notifies observer on changes.
		 */
		void                               refreshLuaCallbackCatalogNow();
		/**
		 * @brief Binds execution to current thread on first call and asserts affinity afterwards.
		 * @param context Diagnostic context label used in affinity violations.
		 */
		void                               bindOrAssertExecutionThread(const char *context) const;
		/**
		 * @brief Clears execution-thread affinity marker after coordinated teardown.
		 */
		void                               clearExecutionThreadAffinity() const;
		/**
		 * @brief Pushes per-dispatch miniwindow snapshot used by callback-scope read caches.
		 * @param snapshot Snapshot captured on runtime thread for the active dispatch.
		 */
		void
		pushDispatchMiniWindowSnapshot(const QSharedPointer<const LuaCallbackMiniWindowSnapshot> &snapshot);
		/**
		 * @brief Pops per-dispatch miniwindow snapshot.
		 */
		void                                               popDispatchMiniWindowSnapshot();
		/**
		 * @brief Returns active per-dispatch miniwindow snapshot.
		 * @return Snapshot pointer, or `nullptr` when unset.
		 */
		[[nodiscard]] const LuaCallbackMiniWindowSnapshot *currentDispatchMiniWindowSnapshot() const;
		/**
		 * @brief Returns shared ownership of the active per-dispatch miniwindow snapshot.
		 * @return Snapshot shared pointer, or null when unset.
		 */
		[[nodiscard]] QSharedPointer<const LuaCallbackMiniWindowSnapshot>
		     currentDispatchMiniWindowSnapshotShared() const;
		/**
		 * @brief Appends a deferred runtime mutation journal produced by callback scope teardown.
		 * @param runtime Runtime that owns the mutations.
		 * @param mutations Ordered mutation callables to execute on the runtime thread.
		 */
		void appendDeferredRuntimeMutationBatch(class WorldRuntime              *runtime,
		                                        QVector<std::function<void()>> &&mutations);
		/**
		 * @brief Moves a nested callback mutation journal into the active outer callback result.
		 * @param batch Batch produced by a nested callback dispatch. Consumed on success.
		 * @return `true` when an active callback accepted the batch.
		 */
		static bool
		appendDeferredRuntimeMutationBatchToActiveCallback(LuaDeferredRuntimeMutationBatch &batch);
		/**
		 * @brief Takes and clears deferred runtime mutation journals produced by the last dispatch.
		 * @return Ordered mutation batches for runtime-thread application.
		 */
		[[nodiscard]] QVector<LuaDeferredRuntimeMutationBatch> takeDeferredRuntimeMutationBatches();
		/**
		 * @brief Resumes a callback previously suspended by a modal string-result API.
		 * @param resumeId Suspended callback id.
		 * @param result Modal result string to return to Lua.
		 * @return Dispatch result carrying deferred mutations produced by resumed execution.
		 */
		[[nodiscard]] LuaBatchDispatchResult resumeSuspendedModalString(quint64        resumeId,
		                                                                const QString &result);
		/**
		 * @brief Cancels a callback coroutine previously suspended by a modal API.
		 * @param resumeId Suspended callback id.
		 */
		void                                 cancelSuspendedModalString(quint64 resumeId);

	private:
		/**
		 * @brief Ensures Lua state exists and is initialized.
		 * @return `true` when Lua state is ready.
		 */
		bool                                                         ensureState();
		/**
		 * @brief Registers world/runtime bindings into Lua globals.
		 */
		void                                                         registerWorldBindings();

		QString                                                      m_script;
		bool                                                         m_scriptLoaded{false};
		bool                                                         m_worldBindingsReady{false};
		bool                                                         m_allowPackage{true};
		class WorldRuntime                                          *m_worldRuntime{nullptr};
		QString                                                      m_pluginId;
		QString                                                      m_pluginName;
		QString                                                      m_pluginDirectory;
		QVector<QString>                                             m_callingPluginIdStack;
		QVector<QSharedPointer<const LuaCallbackMiniWindowSnapshot>> m_dispatchMiniWindowSnapshotStack;
		int                                                          m_scriptExecutionDepth{0};
		mutable QThread                                             *m_executionThread{nullptr};
		mutable bool                                                 m_reportedRuntimeThreadMismatch{false};

#ifdef QMUD_ENABLE_LUA_SCRIPTING
		/**
		 * @brief Executes a prepared callback function on a yieldable coroutine.
		 * @param functionName Callback function name used for diagnostics.
		 * @param argCount Number of arguments already pushed after the function.
		 * @param expectedResults Number of return values expected by legacy caller semantics.
		 * @param defaultResult Result when the callback errors or suspends.
		 * @param suspended Optional output flag set when callback yielded at a modal API.
		 * @param modalResumeId Optional output id for the suspended modal callback.
		 * @param pendingModalStringRequest Optional output request for the yielded modal API.
		 * @param resultMode Selects the expected return-value conversion mode.
		 * @param bytesResult Optional output byte-array result when `resultMode` is `Bytes`.
		 * @param stringResult Optional output string result when `resultMode` is `String`.
		 * @return Callback boolean result for non-suspended execution.
		 */
		bool callPreparedYieldableCallback(
		    const QString &functionName, int argCount, int expectedResults, bool defaultResult,
		    bool *suspended, quint64 *modalResumeId, LuaPendingModalStringRequest *pendingModalStringRequest,
		    LuaPreparedCallbackResultMode resultMode = LuaPreparedCallbackResultMode::Bool,
		    QByteArray *bytesResult = nullptr, QString *stringResult = nullptr);
		std::unique_ptr<lua_State, LuaStateDeleter>           m_ownedState;
		lua_State                                            *m_state{nullptr};
		bool                                                  m_packageRestrictionsApplied{false};
		bool                                                  m_packageRestrictionsAppliedValue{true};
		QHash<quint64, std::shared_ptr<LuaSuspendedCallback>> m_suspendedCallbacks;
		quint64                                               m_nextSuspendedCallbackId{1};
#endif
		QSet<QString>                            m_luaFunctionsSet;
		QSet<QString>                            m_observedPluginCallbacks;
		QHash<QString, bool>                     m_observedPluginCallbackPresence;
		CallbackCatalogObserver                  m_callbackCatalogObserver;
		QVector<LuaDeferredRuntimeMutationBatch> m_deferredRuntimeMutationBatches;
};

#endif // QMUD_LUACALLBACKENGINE_H
