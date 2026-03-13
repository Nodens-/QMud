/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: LuaFunctionTypes.h
 * Role: Type definitions for Lua function signatures, metadata entries, and registration structures used by the
 * scripting bridge.
 */

#ifndef QMUD_LUAFUNCTIONTYPES_H
#define QMUD_LUAFUNCTIONTYPES_H

// Canonical Lua function metadata table row.
struct InternalFunctionMetadata
{
		const char *functionName;       // function name (eg. "WindowShow")
		const char *argumentsSignature; // argument description (eg. "(Window, Showflag)")
};

extern const InternalFunctionMetadata kInternalFunctionMetadataTable[];

#endif // QMUD_LUAFUNCTIONTYPES_H
