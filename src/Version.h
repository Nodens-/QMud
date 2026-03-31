/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: Version.h
 * Role: Build/version constants used for client identification, about dialogs, and script-visible version reporting.
 */

#ifndef QMUD_VERSION_H
#define QMUD_VERSION_H

// Used for detecting version changes.
inline constexpr int  kThisVersion = 1022;

// Used to display the version number.
inline constexpr char kVersionString[] = "10.22";

// CI builds append "-ci" to the runtime display version (AppController::m_version).
// kVersionString itself remains the canonical numeric version.

#endif // QMUD_VERSION_H
