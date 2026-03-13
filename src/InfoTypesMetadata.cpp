/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: InfoTypesMetadata.cpp
 * Role: Metadata tables and lookup logic for Info/GetInfo selectors exposed through scripting and diagnostics.
 */

#include "InfoTypesMetadata.h"

// Canonical info type metadata table used by utils.infotypes.
// Extracted from scripting/methods/methods_info.cpp without runtime engine dependencies.
const InfoTypeMapping kInfoTypeMappingTable[] = {
#define QMUD_INCLUDE_INFO_TYPE_DATA_ROWS
#include "InfoTypeData.inc"
#undef QMUD_INCLUDE_INFO_TYPE_DATA_ROWS
};
