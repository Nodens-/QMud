/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: CommandIds.h
 * Role: Shared numeric command identifier definitions used to preserve stable action routing across UI and scripting
 * layers.
 */

#ifndef QMUD_COMMANDIDS_H
#define QMUD_COMMANDIDS_H

// Canonical command-id values used by accelerator metadata.
// Numeric values are intentionally stable for UI/Lua compatibility.
enum class CommandId : int
{
	AppAbout         = 0xE140,
	AppExit          = 0xE141,
	ContextHelp      = 0xE130,
	EditCopy         = 0xE122,
	EditCut          = 0xE123,
	EditPaste        = 0xE125,
	EditSelectAll    = 0xE12A,
	EditUndo         = 0xE120,
	FileClose        = 0xE106,
	FileNew          = 0xE100,
	FileOpen         = 0xE101,
	FilePrint        = 0xE107,
	FilePrintPreview = 0xE109,
	FilePrintSetup   = 0xE10A,
	FileSave         = 0xE103,
	FileSaveAs       = 0xE104,
	Help             = 0xE146,
	HelpIndex        = 0xE147,
	HelpUsing        = 0xE148,
	NextPane         = 0xE135,
	ViewStatusBar    = 0xE801,
	ViewToolbar      = 0xE800,
	WindowArrange    = 0xE133,
	WindowCascade    = 0xE130,
	WindowNew        = 0xE134,
	WindowTileHorz   = 0xE131,
	WindowTileVert   = 0xE132
};

/**
 * @brief Converts strongly-typed command id to underlying integer value.
 * @param id Strongly-typed command id.
 * @return Integer command id.
 */
constexpr int commandIdValue(const CommandId id) noexcept
{
	return static_cast<int>(id);
}

#endif // QMUD_COMMANDIDS_H
