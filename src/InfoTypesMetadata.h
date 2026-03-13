/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: InfoTypesMetadata.h
 * Role: Metadata interfaces describing supported GetInfo/Info types and their value contracts for script consumers.
 */

#ifndef QMUD_INFOTYPESMETADATA_H
#define QMUD_INFOTYPESMETADATA_H

struct InfoTypeMapping
{
		int         infoType;
		const char *description;
};

extern const InfoTypeMapping kInfoTypeMappingTable[];

#endif // QMUD_INFOTYPESMETADATA_H
