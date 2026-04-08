/**
 * @file LuaLuacomShim.h
 * @brief Registration entrypoint for the Lua `luacom` compatibility shim.
 */

#ifndef QMUD_LUALUACOMSHIM_H
#define QMUD_LUALUACOMSHIM_H

#include "LuaHeaders.h"

#include <functional>
#include <memory>

namespace QMudTtsEngine
{
	class TtsEngine;
}

namespace QMudLuacomShim
{
	/**
	 * @brief Registers `luacom` in `package.preload` for the active Lua state.
	 * @param L Lua state.
	 */
	void registerPreload(lua_State *L);

	/**
	 * @brief Runtime-factory callback used for luacom `SAPI.SpVoice` creation.
	 *
	 * This is primarily intended for deterministic unit tests.
	 */
	using RuntimeFactory = std::function<std::shared_ptr<QMudTtsEngine::TtsEngine>()>;

	/**
	 * @brief Overrides runtime factory used by `luacom.CreateObject("SAPI.SpVoice")`.
	 * @param factory Factory callback. Empty callback restores default behavior.
	 */
	void setRuntimeFactoryForTesting(RuntimeFactory factory);

	/**
	 * @brief Restores default runtime factory used for `SAPI.SpVoice`.
	 */
	void resetRuntimeFactoryForTesting();
} // namespace QMudLuacomShim

#endif // QMUD_LUALUACOMSHIM_H
