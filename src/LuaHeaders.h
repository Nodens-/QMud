/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: LuaHeaders.h
 * Role: Centralized Lua header inclusion and compatibility macros so QMud builds cleanly across supported Lua variants.
 */

#ifndef QMUD_LUAHEADERS_H
#define QMUD_LUAHEADERS_H

#ifdef __cplusplus
extern "C"
{
#endif
// ReSharper disable once CppUnusedIncludeDirective
#include <lauxlib.h>
// ReSharper disable once CppUnusedIncludeDirective
#include <lua.h>
// ReSharper disable once CppUnusedIncludeDirective
#include <lualib.h>
#ifdef __cplusplus
}
#endif

#endif // QMUD_LUAHEADERS_H
