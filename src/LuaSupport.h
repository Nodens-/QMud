/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: LuaSupport.h
 * Role: Qt-to-Lua conversion and stack helper interfaces used throughout scripting API implementations.
 */

#ifndef QMUD_LUASUPPORT_H
#define QMUD_LUASUPPORT_H

#include "LuaHeaders.h"

#include <QString>

/**
 * @brief RAII wrapper for a raw Lua state pointer.
 *
 * Owns a `lua_State*` and ensures proper close/reset semantics across moves.
 */
class LuaStateOwner
{
	public:
		/**
		 * @brief Constructs an empty owner with no Lua state.
		 */
		LuaStateOwner() = default;
		/**
		 * @brief Takes ownership of an existing Lua state pointer.
		 * @param state Lua state pointer to own.
		 */
		explicit LuaStateOwner(lua_State *state);
		/**
		 * @brief Closes and releases the owned Lua state if present.
		 */
		~LuaStateOwner();
		LuaStateOwner(const LuaStateOwner &)            = delete;
		LuaStateOwner &operator=(const LuaStateOwner &) = delete;
		/**
		 * @brief Moves Lua state ownership from another owner.
		 * @param other Source owner.
		 */
		LuaStateOwner(LuaStateOwner &&other) noexcept;
		/**
		 * @brief Replaces this owner with another owner's state.
		 * @param other Source owner.
		 * @return Reference to this owner.
		 */
		LuaStateOwner           &operator=(LuaStateOwner &&other) noexcept;
		/**
		 * @brief Returns the currently owned raw Lua state pointer.
		 * @return Owned Lua state pointer, or `nullptr`.
		 */
		[[nodiscard]] lua_State *get() const;
		/**
		 * @brief Releases ownership without closing and returns the raw pointer.
		 * @return Released Lua state pointer.
		 */
		lua_State               *release();
		/**
		 * @brief Closes current state and optionally adopts a new one.
		 * @param state New Lua state pointer to adopt.
		 */
		void                     reset(lua_State *state = nullptr);
		/**
		 * @brief Returns true when a Lua state is owned.
		 * @return `true` when an owned state exists.
		 */
		explicit                 operator bool() const;

	private:
		lua_State *m_state{nullptr};
};

namespace QMudLuaSupport
{
	/**
	 * @brief Creates a configured Lua state for QMud scripting.
	 * @return Initialized Lua state pointer, or `nullptr` on failure.
	 */
	lua_State *makeLuaState();
	/**
	 * @brief Calls a C function with Lua error protection.
	 * @param L Lua state pointer.
	 * @param fn Lua C function pointer to invoke.
	 */
	void       callLuaCFunction(lua_State *L, lua_CFunction fn);
	/**
	 * @brief Executes a protected Lua call with traceback support.
	 * @param L Lua state pointer.
	 * @param arguments Number of call arguments.
	 * @param returnsCount Expected number of returns.
	 * @return `lua_pcall` status code.
	 */
	int        callLuaWithTraceback(lua_State *L, int arguments, int returnsCount);
	/**
	 * @brief Enables or disables Lua package-loading facilities.
	 * @param L Lua state pointer.
	 * @param enablePackage Enable package access when `true`.
	 */
	void       applyLuaPackageRestrictions(lua_State *L, bool enablePackage);
	/**
	 * @brief Applies script-security restrictions to a Lua state.
	 * @param L Lua state pointer.
	 * @param enablePackage Enable package access when `true`.
	 */
	void       applyLuaSecurityRestrictions(lua_State *L, bool enablePackage);
	/**
	 * @brief Enables Lua 5.1 compatibility helpers.
	 * @param L Lua state pointer.
	 */
	void       applyLua51Compat(lua_State *L);
	/**
	 * @brief Pushes a named Lua function (supports dotted names) onto the stack.
	 *
	 * Supports direct globals (`foo`) and table-qualified names (`table.fn`).
	 *
	 * @param L Lua state pointer.
	 * @param functionName Lua function name.
	 * @return `true` when a function is resolved and pushed, otherwise `false`.
	 */
	bool       pushLuaFunctionByName(lua_State *L, const QString &functionName);
	/**
	 * @brief Calls a named Lua function (supports dotted names) as a procedure with one string argument.
	 *
	 * Procedure semantics: callback return value is ignored; success means the function exists and executes
	 * without Lua runtime error.
	 *
	 * @param L Lua state pointer.
	 * @param functionName Lua function name (e.g. `foo` or `table.fn`).
	 * @param arg String argument.
	 * @param hasFunction Optional output flag indicating function existence.
	 * @param luaError Optional output for Lua runtime error text on failure.
	 * @return `true` when function exists and executes successfully.
	 */
	bool       callLuaNamedProcedureWithString(lua_State *L, const QString &functionName, const QString &arg,
	                                           bool *hasFunction = nullptr, QString *luaError = nullptr);
	/**
	 * @brief Reads optional boolean argument with default fallback.
	 * @param L Lua state pointer.
	 * @param argIndex Lua argument index.
	 * @param defaultValue Fallback value when argument is absent/nil.
	 * @return Parsed boolean value.
	 */
	bool       optBoolean(lua_State *L, int argIndex, bool defaultValue);
	/**
	 * @brief Raises a standardized Lua runtime error.
	 * @param L Lua state pointer.
	 * @param strEvent Event/category text.
	 * @param strProcedure Procedure name text.
	 * @param strType Type/category text.
	 * @param strReason Detailed reason text.
	 */
	void       luaError(lua_State *L, const char *strEvent = "Run-time error", const char *strProcedure = "",
	                    const char *strType = "", const char *strReason = "");
} // namespace QMudLuaSupport

/**
 * @brief Logs whether Lua 5.1 compatibility mode is active.
 * @param L Lua state pointer.
 * @param context Logging context label.
 */
void qmudLogLua51CompatState(lua_State *L, const char *context);
/**
 * @brief Validates and normalizes brush-style values for drawing APIs.
 * @param brushStyle Requested brush style value.
 * @param penColour Pen color value.
 * @param brushColour Brush color value.
 * @return Normalized brush style value.
 */
long qmudValidateBrushStyle(long brushStyle, long penColour, long brushColour);

#endif // QMUD_LUASUPPORT_H
