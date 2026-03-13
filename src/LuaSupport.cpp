/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: LuaSupport.cpp
 * Role: Qt/Lua conversion utilities, marshalling helpers, and stack operations shared by Lua callback/API code.
 */

#include "LuaSupport.h"
#include "scripting/ScriptingErrors.h"

#include <QByteArray>
#include <QDebug>

#include <cstdlib>

// Lua allocator callback signature is fixed by Lua's C API (lua_Alloc).
// ReSharper disable once CppParameterMayBeConstPtrOrRef
static void *luaAlloc(void *const ud, void *ptr, const size_t osize, const size_t nsize)
{
	(void)ud;
	(void)osize;
	if (nsize == 0)
	{
		free(ptr);
		return nullptr;
	}
	return realloc(ptr, nsize);
}

static int luaPanic(lua_State *L)
{
	if (const char *msg = lua_tostring(L, -1); msg)
		qWarning() << "Lua panic:" << msg;
	else
		qWarning() << "Lua panic: <unknown>";
	return 0;
}

lua_State *QMudLuaSupport::makeLuaState()
{
	lua_State *L = lua_newstate(luaAlloc, nullptr);
	if (L)
		lua_atpanic(L, &luaPanic);
	return L;
}

LuaStateOwner::LuaStateOwner(lua_State *state) : m_state(state)
{
}

LuaStateOwner::~LuaStateOwner()
{
	reset();
}

LuaStateOwner::LuaStateOwner(LuaStateOwner &&other) noexcept : m_state(other.release())
{
}

LuaStateOwner &LuaStateOwner::operator=(LuaStateOwner &&other) noexcept
{
	if (this == &other)
		return *this;
	reset(other.release());
	return *this;
}

lua_State *LuaStateOwner::get() const
{
	return m_state;
}

lua_State *LuaStateOwner::release()
{
	lua_State *released = m_state;
	m_state             = nullptr;
	return released;
}

void LuaStateOwner::reset(lua_State *state)
{
	if (m_state)
		lua_close(m_state);
	m_state = state;
}

LuaStateOwner::operator bool() const
{
	return m_state != nullptr;
}

static void pushTracebackFunction(lua_State *L)
{
	lua_getglobal(L, LUA_DBLIBNAME);
	if (lua_istable(L, -1))
	{
		lua_getfield(L, -1, "traceback");
		lua_remove(L, -2);
		if (lua_isfunction(L, -1))
			return;
	}
	lua_pop(L, 1);
	lua_pushnil(L);
}

int QMudLuaSupport::callLuaWithTraceback(lua_State *L, const int arguments, const int returnsCount)
{
	const int base = lua_gettop(L) - arguments;
	pushTracebackFunction(L);
	int error = 0;
	if (lua_isnil(L, -1))
	{
		lua_pop(L, 1);
		error = lua_pcall(L, arguments, returnsCount, 0);
	}
	else
	{
		lua_insert(L, base);
		error = lua_pcall(L, arguments, returnsCount, base);
		lua_remove(L, base);
	}
	return error;
}

void QMudLuaSupport::callLuaCFunction(lua_State *L, const lua_CFunction fn)
{
	lua_pushcfunction(L, fn);
	if (lua_pcall(L, 0, 0, 0) != 0)
	{
		const char *err = lua_tostring(L, -1);
		qWarning() << "Lua C function failed:" << (err ? err : "<unknown>");
		lua_pop(L, 1);
	}
}

static bool luaCompatLoggingEnabled()
{
	const QByteArray env = qgetenv("QMUD_LOG_LUA_COMPAT_STATE").trimmed().toLower();
	if (env.isEmpty())
		return false;
	return !(env == "0" || env == "false" || env == "no" || env == "off");
}

static bool luaGlobalIsFunction(lua_State *L, const char *name)
{
	lua_getglobal(L, name);
	const bool result = lua_isfunction(L, -1) != 0;
	lua_pop(L, 1);
	return result;
}

void qmudLogLua51CompatState(lua_State *L, const char *context)
{
	if (!L || !luaCompatLoggingEnabled())
		return;
	const bool hasGetfenv = luaGlobalIsFunction(L, "getfenv");
	const bool hasSetfenv = luaGlobalIsFunction(L, "setfenv");
	const bool hasModule  = luaGlobalIsFunction(L, "module");
	const bool hasUnpack  = luaGlobalIsFunction(L, "unpack");
	qInfo() << "Lua 5.1 compatibility surface" << (context ? context : "<unknown>")
	        << "getfenv=" << hasGetfenv << "setfenv=" << hasSetfenv << "module=" << hasModule
	        << "unpack=" << hasUnpack;
}

void QMudLuaSupport::applyLua51Compat(lua_State *L)
{
	if (!L)
		return;
	static constexpr auto kCompatScript = R"lua(
local function findenv(f)
  local level = 1
  while true do
    local name, value = debug.getupvalue(f, level)
    if name == "_ENV" then return level, value end
    if not name then return nil end
    level = level + 1
  end
end

function getfenv(f)
  if f == nil then f = 2 end
  if type(f) == "number" then
    local info = debug.getinfo(f + 1, "f")
    f = info and info.func or nil
  end
  if type(f) ~= "function" then return _G end
  local level, env = findenv(f)
  return env or _G
end

function setfenv(f, env)
  if type(f) == "number" then
    local info = debug.getinfo(f + 1, "f")
    f = info and info.func or nil
  end
  if type(f) ~= "function" then return f end
  local level = findenv(f)
  if level then debug.setupvalue(f, level, env) end
  return f
end

if not unpack then unpack = table.unpack end
if not loadstring then loadstring = load end

if not module then
  function module(name, ...)
    local M = package.loaded[name] or _G[name] or {}
    package.loaded[name] = M
    _G[name] = M
    local env = setmetatable(M, { __index = _G })
    setfenv(2, env)
    for _, f in ipairs({...}) do
      f(env)
    end
    return env
  end
end
)lua";

	if (luaL_dostring(L, kCompatScript) != 0)
	{
		const char *err = lua_tostring(L, -1);
		qWarning() << "Lua 5.1 compat init failed:" << (err ? err : "unknown");
		lua_pop(L, 1);
	}
}

void QMudLuaSupport::applyLuaPackageRestrictions(lua_State *L, const bool enablePackage)
{
	if (!L)
		return;

	if (!enablePackage)
	{
		lua_getglobal(L, LUA_LOADLIBNAME);
		if (!lua_istable(L, -1))
		{
			qWarning() << "Lua package table missing or invalid; skipping DLL restrictions.";
			lua_settop(L, 0);
			return;
		}

		lua_pushnil(L);
		lua_setfield(L, -2, "loadlib");

		lua_getfield(L, -1, "loaders");
		if (!lua_istable(L, -1))
		{
			qWarning() << "Lua package.loaders missing or invalid; skipping DLL restrictions.";
			lua_settop(L, 0);
			return;
		}

		lua_pushnil(L);
		lua_rawseti(L, -2, 4);
		lua_pushnil(L);
		lua_rawseti(L, -2, 3);

		lua_settop(L, 0);
	}
}

static void applyLuaRuntimeSafetyOverrides(lua_State *L)
{
	if (!L)
		return;
	lua_getglobal(L, LUA_DBLIBNAME);
	if (lua_istable(L, -1))
	{
		lua_pushcfunction(L, [](lua_State *state) -> int
		                  { return luaL_error(state, "'debug.debug' not implemented in QMud"); });
		lua_setfield(L, -2, "debug");
	}
	lua_pop(L, 1);

	lua_getglobal(L, LUA_OSLIBNAME);
	if (lua_istable(L, -1))
	{
		lua_pushcfunction(L, [](lua_State *state) -> int
		                  { return luaL_error(state, "'os.exit' not implemented in QMud"); });
		lua_setfield(L, -2, "exit");
	}
	lua_pop(L, 1);

	lua_getglobal(L, LUA_IOLIBNAME);
	if (lua_istable(L, -1))
	{
		lua_pushcfunction(L, [](lua_State *state) -> int
		                  { return luaL_error(state, "'io.popen' not implemented in QMud"); });
		lua_setfield(L, -2, "popen");
	}
	lua_pop(L, 1);

	lua_settop(L, 0);
}

void QMudLuaSupport::applyLuaSecurityRestrictions(lua_State *L, const bool enablePackage)
{
	applyLuaPackageRestrictions(L, enablePackage);
	applyLuaRuntimeSafetyOverrides(L);
}

bool QMudLuaSupport::optBoolean(lua_State *L, const int argIndex, const bool defaultValue)
{
	if (lua_gettop(L) < argIndex)
		return defaultValue;
	if (lua_isnil(L, argIndex))
		return defaultValue;
	if (lua_isboolean(L, argIndex))
		return lua_toboolean(L, argIndex) != 0;
	return luaL_checknumber(L, argIndex) != 0;
}

void QMudLuaSupport::luaError(lua_State *L, const char *strEvent, const char *strProcedure,
                              const char *strType, const char *strReason)
{
	const char   *err    = lua_tostring(L, -1);
	const QString event  = strEvent ? QString::fromUtf8(strEvent) : QStringLiteral("Lua error");
	const QString proc   = strProcedure ? QString::fromUtf8(strProcedure) : QString();
	const QString type   = strType ? QString::fromUtf8(strType) : QString();
	const QString reason = strReason ? QString::fromUtf8(strReason) : QString();
	const QString desc   = err ? QString::fromUtf8(err) : QStringLiteral("<no error message>");
	const QString details =
	    proc.isEmpty() ? desc : QStringLiteral("%1 (%2: %3) - %4").arg(desc, type, proc, reason);
	qWarning() << event << details;
	lua_settop(L, 0);
}
long qmudValidateBrushStyle(const long brushStyle, const long penColour, const long brushColour)
{
	Q_UNUSED(penColour);
	Q_UNUSED(brushColour);
	if (brushStyle < 0 || brushStyle > 12)
		return eBrushStyleNotValid;
	return eOK;
}
