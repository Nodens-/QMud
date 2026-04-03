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

// ReSharper disable once CppUnusedIncludeDirective
#include <QByteArray>
// ReSharper disable once CppUnusedIncludeDirective
#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>
#include <functional>
#include <memory>

#ifdef QMUD_ENABLE_LUA_SCRIPTING
struct lua_State;

struct LuaStateDeleter
{
		/**
		 * @brief Closes Lua state for unique_ptr ownership.
		 * @param state Lua state pointer to close.
		 */
		void operator()(lua_State *state) const;
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

/**
 * @brief Lua VM integration layer for world/plugin callback execution.
 *
 * Owns Lua state lifetime, binds exported APIs, and marshals calls between C++
 * runtime objects and script-side functions.
 */
class LuaCallbackEngine
{
	public:
		using CallbackCatalogObserver = std::function<void()>;

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
		 * @brief Sets plugin identity metadata used by callback context.
		 * @param id Plugin id.
		 * @param name Plugin display name.
		 */
		void                              setPluginInfo(const QString &id, const QString &name);
		/**
		 * @brief Returns bound plugin id.
		 * @return Plugin id.
		 */
		[[nodiscard]] QString             pluginId() const;
		/**
		 * @brief Returns bound plugin display name.
		 * @return Plugin display name.
		 */
		[[nodiscard]] QString             pluginName() const;
		/**
		 * @brief Replaces current Lua script source text.
		 * @param script Lua script source text.
		 */
		void                              setScriptText(const QString &script);
		/**
		 * @brief Loads/compiles current script text into Lua state.
		 * @return `true` on successful load/compile.
		 */
		bool                              loadScript();
		/**
		 * @brief Resets VM and callback state while keeping metadata.
		 */
		void                              resetState();
		/**
		 * @brief Checks whether Lua function exists in current state.
		 * @param functionName Lua function name.
		 * @return `true` when function exists.
		 */
		bool                              hasFunction(const QString &functionName);
		/**
		 * @brief Calls MXP error callback with protocol diagnostics.
		 * @param functionName Callback function name.
		 * @param level Error level.
		 * @param messageNumber Protocol message number.
		 * @param lineNumber Related line number.
		 * @param message Error message text.
		 * @return `true` when callback returns truthy/continue semantics.
		 */
		bool callMxpError(const QString &functionName, int level, long messageNumber, int lineNumber,
		                  const QString &message);
		/**
		 * @brief Calls MXP startup callback.
		 * @param functionName Callback function name.
		 */
		void callMxpStartUp(const QString &functionName);
		/**
		 * @brief Calls MXP shutdown callback.
		 * @param functionName Callback function name.
		 */
		void callMxpShutDown(const QString &functionName);
		/**
		 * @brief Calls MXP start-tag callback.
		 * @param functionName Callback function name.
		 * @param name Tag name.
		 * @param args Raw argument string.
		 * @param table Parsed attribute table.
		 * @return `true` when callback returns truthy/continue semantics.
		 */
		bool callMxpStartTag(const QString &functionName, const QString &name, const QString &args,
		                     const QMap<QString, QString> &table);
		/**
		 * @brief Calls MXP end-tag callback.
		 * @param functionName Callback function name.
		 * @param name Tag name.
		 * @param text Tag text content.
		 */
		void callMxpEndTag(const QString &functionName, const QString &name, const QString &text);
		/**
		 * @brief Calls MXP variable callback.
		 * @param functionName Callback function name.
		 * @param name Variable name.
		 * @param contents Variable contents.
		 */
		void callMxpSetVariable(const QString &functionName, const QString &name, const QString &contents);
		/**
		 * @brief Calls function with no arguments and boolean result.
		 * @param functionName Callback function name.
		 * @param hasFunction Optional output flag indicating function existence.
		 * @param defaultResult Result when function is missing/error.
		 * @return Callback result.
		 */
		bool callFunctionNoArgs(const QString &functionName, bool *hasFunction = nullptr,
		                        bool defaultResult = true);
		/**
		 * @brief Calls function with one string argument.
		 * @param functionName Callback function name.
		 * @param arg String argument.
		 * @param hasFunction Optional output flag indicating function existence.
		 * @param defaultResult Result when function is missing/error.
		 * @return Callback result.
		 */
		bool callFunctionWithString(const QString &functionName, const QString &arg,
		                            bool *hasFunction = nullptr, bool defaultResult = true);
		/**
		 * @brief Calls function with one byte-array argument.
		 * @param functionName Callback function name.
		 * @param arg Byte-array argument.
		 * @param hasFunction Optional output flag indicating function existence.
		 * @param defaultResult Result when function is missing/error.
		 * @return Callback result.
		 */
		bool callFunctionWithBytes(const QString &functionName, const QByteArray &arg,
		                           bool *hasFunction = nullptr, bool defaultResult = true);
		/**
		 * @brief Calls function with mutable byte-array in/out argument.
		 * @param functionName Callback function name.
		 * @param arg Byte-array argument modified in place.
		 * @param hasFunction Optional output flag indicating function existence.
		 * @return Callback result.
		 */
		bool callFunctionWithBytesInOut(const QString &functionName, QByteArray &arg,
		                                bool *hasFunction = nullptr);
		/**
		 * @brief Calls function with mutable string in/out argument.
		 * @param functionName Callback function name.
		 * @param arg String argument modified in place.
		 * @param hasFunction Optional output flag indicating function existence.
		 * @return Callback result.
		 */
		bool callFunctionWithStringInOut(const QString &functionName, QString &arg,
		                                 bool *hasFunction = nullptr);
		/**
		 * @brief Calls function with one number and one string argument.
		 * @param functionName Callback function name.
		 * @param arg1 Numeric argument.
		 * @param arg String argument.
		 * @param hasFunction Optional output flag indicating function existence.
		 * @param defaultResult Result when function is missing/error.
		 * @return Callback result.
		 */
		bool callFunctionWithNumberAndString(const QString &functionName, long arg1, const QString &arg,
		                                     bool *hasFunction = nullptr, bool defaultResult = true);
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
		 * @return Callback result.
		 */
		bool callFunctionWithNumberAndUtf8Strings(const QString &functionName, long arg1,
		                                          const QByteArray &arg2Utf8, const QByteArray &arg3Utf8,
		                                          const QByteArray &arg4Utf8, bool *hasFunction = nullptr,
		                                          bool defaultResult = true);
		/**
		 * @brief Calls function with two numbers and one string argument.
		 * @param functionName Callback function name.
		 * @param arg1 First numeric argument.
		 * @param arg2 Second numeric argument.
		 * @param arg String argument.
		 * @param hasFunction Optional output flag indicating function existence.
		 * @param defaultResult Result when function is missing/error.
		 * @return Callback result.
		 */
		bool callFunctionWithTwoNumbersAndString(const QString &functionName, long arg1, long arg2,
		                                         const QString &arg, bool *hasFunction = nullptr,
		                                         bool defaultResult = true);
		/**
		 * @brief Calls function with one number and byte-array argument.
		 * @param functionName Callback function name.
		 * @param arg1 Numeric argument.
		 * @param arg Byte-array argument.
		 * @param hasFunction Optional output flag indicating function existence.
		 * @param defaultResult Result when function is missing/error.
		 * @return Callback result.
		 */
		bool callFunctionWithNumberAndBytes(const QString &functionName, long arg1, const QByteArray &arg,
		                                    bool *hasFunction = nullptr, bool defaultResult = true);
		/**
		 * @brief Calls function with argument list, wildcard sets, and optional style runs.
		 * @param functionName Callback function name.
		 * @param args Positional string arguments.
		 * @param wildcards Positional wildcard values.
		 * @param namedWildcards Named wildcard values.
		 * @param styleRuns Optional style-run list.
		 * @param hasFunction Optional output flag indicating function existence.
		 * @return Callback result.
		 */
		bool callFunctionWithStringsAndWildcards(const QString &functionName, const QStringList &args,
		                                         const QStringList            &wildcards,
		                                         const QMap<QString, QString> &namedWildcards,
		                                         const QVector<LuaStyleRun>   *styleRuns,
		                                         bool                         *hasFunction = nullptr);
		/**
		 * @brief Executes arbitrary Lua code chunk.
		 * @param code Lua code text.
		 * @param description Description used for diagnostics.
		 * @param styleRuns Optional style-run context.
		 * @return `true` on successful execution.
		 */
		bool executeScript(const QString &code, const QString &description,
		                   const QVector<LuaStyleRun> *styleRuns = nullptr);
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

	private:
		/**
		 * @brief Ensures Lua state exists and is initialized.
		 * @return `true` when Lua state is ready.
		 */
		bool                ensureState();
		/**
		 * @brief Registers world/runtime bindings into Lua globals.
		 */
		void                registerWorldBindings();

		QString             m_script;
		bool                m_scriptLoaded{false};
		bool                m_worldBindingsReady{false};
		bool                m_allowPackage{true};
		class WorldRuntime *m_worldRuntime{nullptr};
		QString             m_pluginId;
		QString             m_pluginName;
		int                 m_scriptExecutionDepth{0};

#ifdef QMUD_ENABLE_LUA_SCRIPTING
		std::unique_ptr<lua_State, LuaStateDeleter> m_ownedState;
		lua_State                                  *m_state{nullptr};
		bool                                        m_packageRestrictionsApplied{false};
		bool                                        m_packageRestrictionsAppliedValue{true};
#endif
		QSet<QString>           m_luaFunctionsSet;
		QSet<QString>           m_observedPluginCallbacks;
		QHash<QString, bool>    m_observedPluginCallbackPresence;
		CallbackCatalogObserver m_callbackCatalogObserver;
};

#endif // QMUD_LUACALLBACKENGINE_H
