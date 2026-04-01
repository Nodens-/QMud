/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: LuaSupport.cpp
 * Role: Qt/Lua conversion utilities, marshalling helpers, and stack operations shared by Lua callback/API code.
 */

#include "LuaSupport.h"

#include "Environment.h"
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
	const QString value = qmudEnvironmentVariable(QStringLiteral("QMUD_LOG_LUA_COMPAT_STATE")).trimmed();
	if (value.isEmpty())
		return false;
	const QString normalized = value.toLower();
	return !(normalized == QStringLiteral("0") || normalized == QStringLiteral("false") ||
	         normalized == QStringLiteral("no") || normalized == QStringLiteral("off"));
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

-- Legacy Mushclient scripts often expect require("json") to expose global json.
-- Preserve compatibility by auto-exporting simple module names to _G when missing.
if require and not rawget(_G, "__qmud_require_compat_wrapped") then
  local _require = require
  local function patch_socket_http_https(mod)
    if type(mod) ~= "table" then
      return mod
    end
    if rawget(mod, "__qmud_https_request_compat_wrapped") then
      return mod
    end
    local raw_request = mod.request
    if type(raw_request) ~= "function" then
      rawset(mod, "__qmud_https_request_compat_wrapped", true)
      return mod
    end

    local function is_https_request(reqt)
      if type(reqt) == "string" then
        return reqt:match("^https://") ~= nil
      end
      if type(reqt) ~= "table" then
        return false
      end
      local url = reqt.url or reqt[1]
      return type(url) == "string" and url:match("^https://") ~= nil
    end

    local function request_https_string(https_mod, url, body)
      local ok_ltn12, ltn12 = pcall(_require, "ltn12")
      if not ok_ltn12 or type(ltn12) ~= "table" then
        return https_mod.request(url, body)
      end
      if type(ltn12.sink) ~= "table" or type(ltn12.sink.table) ~= "function" then
        return https_mod.request(url, body)
      end

      local response_chunks = {}
      local request = {
        url = url,
        target = response_chunks,
        sink = ltn12.sink.table(response_chunks),
      }
      if body ~= nil then
        if type(ltn12.source) ~= "table" or type(ltn12.source.string) ~= "function" then
          return https_mod.request(url, body)
        end
        local body_string = tostring(body)
        request.source = ltn12.source.string(body_string)
        request.headers = {
          ["content-length"] = string.len(body_string),
          ["content-type"] = "application/x-www-form-urlencoded",
        }
        request.method = "POST"
      end

      local first, code, headers, status = https_mod.request(request)
      if first == nil then
        return nil, code, headers, status
      end

      local page = table.concat(response_chunks)
      if page == "" and type(first) == "string" then
        page = first
      end
      return page, code, headers, status
    end

    local function should_fallback_to_raw(first, code)
      if first ~= nil then
        return false
      end
      if type(code) ~= "string" then
        return false
      end
      return code:find("create function not permitted", 1, true) ~= nil
    end

    mod.request = function(reqt, body)
      if is_https_request(reqt) then
        local ok_https, https_mod = pcall(_require, "ssl.https")
        if ok_https and type(https_mod) == "table" and type(https_mod.request) == "function" then
          if type(reqt) == "table" then
            local first, code, headers, status = https_mod.request(reqt)
            if should_fallback_to_raw(first, code) then
              return raw_request(reqt, body)
            end
            return first, code, headers, status
          end
          local first, code, headers, status = request_https_string(https_mod, reqt, body)
          if should_fallback_to_raw(first, code) then
            return raw_request(reqt, body)
          end
          return first, code, headers, status
        end
      end
      return raw_request(reqt, body)
    end

    rawset(mod, "__qmud_https_request_compat_wrapped", true)
    return mod
  end

  function require(name)
    local mod = _require(name)
    if name == "socket.http" then
      mod = patch_socket_http_https(mod)
    end
    if type(name) == "string"
      and name:match("^[%a_][%w_]*$")
      and rawget(_G, name) == nil then
      local t = type(mod)
      if t == "table" or t == "function" or t == "userdata" then
        rawset(_G, name, mod)
      end
    end
    return mod
  end
  rawset(_G, "__qmud_require_compat_wrapped", true)
end

-- Lua 5.1 accepted non-integer numbers with integer format specifiers
-- ("%i", "%d", "%x", ...). Lua 5.4 raises "number has no integer
-- representation". Preserve legacy script behavior by coercing numeric
-- arguments toward zero for those specifiers before formatting.
if string and type(string.format) == "function" and
   not rawget(_G, "__qmud_string_format_compat_wrapped") then
  local _format = string.format

  local function is_integer_specifier(spec)
    return spec == "d" or spec == "i" or spec == "o" or
           spec == "u" or spec == "x" or spec == "X"
  end

  local function coerce_to_integer(v)
    if type(v) ~= "number" then
      return v
    end
    local int_v = math.tointeger and math.tointeger(v)
    if int_v ~= nil then
      return int_v
    end
    local truncated = math.modf(v)
    return truncated
  end

  local function coerce_integer_format_args(fmt, ...)
    if type(fmt) ~= "string" then
      return _format(fmt, ...)
    end
    local args = { ... }
    local arg_index = 1
    local i = 1
    local len = #fmt

    while i <= len do
      local ch = fmt:sub(i, i)
      if ch ~= "%" then
        i = i + 1
      else
        if i < len and fmt:sub(i + 1, i + 1) == "%" then
          i = i + 2
        else
          local j = i + 1
          while j <= len do
            local token = fmt:sub(j, j)
            if token == "*" then
              arg_index = arg_index + 1
              j = j + 1
            elseif token:match("[%aA-Z]") then
              if is_integer_specifier(token) then
                args[arg_index] = coerce_to_integer(args[arg_index])
              end
              arg_index = arg_index + 1
              i = j + 1
              break
            else
              j = j + 1
            end
          end
          if j > len then
            i = len + 1
          end
        end
      end
    end

    return _format(fmt, table.unpack(args))
  end

  function string.format(fmt, ...)
    local ok, result = pcall(_format, fmt, ...)
    if ok then
      return result
    end
    if type(result) == "string" and result:find("integer representation", 1, true) then
      return coerce_integer_format_args(fmt, ...)
    end
    error(result, 2)
  end

  rawset(_G, "__qmud_string_format_compat_wrapped", true)
end

-- Lua 5.1 tolerated replacement strings in string.gsub that used "%"
-- before non-capture characters (eg "%[" ... "%]"), while Lua 5.4 raises:
-- "invalid use of '%' in replacement string".
-- Preserve Mushclient plugin compatibility by retrying with a lenient
-- replacement normalization when that specific error occurs.
if string and type(string.gsub) == "function" and
   not rawget(_G, "__qmud_string_gsub_compat_wrapped") then
  local _gsub = string.gsub

  local function normalize_gsub_replacement(rep)
    if type(rep) ~= "string" then
      return rep
    end
    local out = {}
    local i = 1
    local len = #rep
    while i <= len do
      local ch = rep:sub(i, i)
      if ch ~= "%" then
        out[#out + 1] = ch
        i = i + 1
      else
        local next_ch = rep:sub(i + 1, i + 1)
        if next_ch == "" then
          out[#out + 1] = "%"
          i = i + 1
        elseif next_ch == "%" or next_ch:match("%d") then
          out[#out + 1] = "%"
          out[#out + 1] = next_ch
          i = i + 2
        else
          out[#out + 1] = next_ch
          i = i + 2
        end
      end
    end
    return table.concat(out)
  end

  function string.gsub(s, pattern, repl, n)
    local ok, a, b = pcall(_gsub, s, pattern, repl, n)
    if ok then
      return a, b
    end
    if type(repl) == "string" and type(a) == "string" and
       a:find("invalid use of '%' in replacement string", 1, true) then
      return _gsub(s, pattern, normalize_gsub_replacement(repl), n)
    end
    error(a, 2)
  end

  rawset(_G, "__qmud_string_gsub_compat_wrapped", true)
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
