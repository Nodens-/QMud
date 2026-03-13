/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: CommandMappingTypes.h
 * Role: Data types that describe command-mapping records consumed by command dispatch and scripting integration.
 */

#ifndef QMUD_COMMANDMAPPINGTYPES_H
#define QMUD_COMMANDMAPPINGTYPES_H

#include <QtGlobal>

// Lightweight command/key mapping metadata shared by accelerator dispatch and scripting.
// Table instances are defined in Accelerators.cpp; this header only declares the types/tables.
struct CommandIdMapping
{
		int         commandId;   // stable numeric id (e.g. CommandId::EditSelectAll or ResourceId::GameLook)
		const char *commandName; // e.g. "SelectAll"
};

struct VirtualKeyMapping
{
		quint16     virtualKey; // legacy Windows-compatible virtual-key code (e.g. 0x1B for Escape)
		const char *keyName;    // e.g. "Esc"
};

extern const CommandIdMapping  kCommandIdMappingTable[];
extern const VirtualKeyMapping kVirtualKeyMappingTable[];

#endif // QMUD_COMMANDMAPPINGTYPES_H
