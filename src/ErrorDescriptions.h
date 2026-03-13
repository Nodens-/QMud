/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: ErrorDescriptions.h
 * Role: Shared declarations for the canonical runtime/Lua error-description lookup table.
 */

#ifndef QMUD_ERRORDESCRIPTIONS_H
#define QMUD_ERRORDESCRIPTIONS_H

struct IntFlagsPair
{
		int         key;
		const char *value;
};

extern const IntFlagsPair kErrorDescriptionMappingTable[];

#endif // QMUD_ERRORDESCRIPTIONS_H
