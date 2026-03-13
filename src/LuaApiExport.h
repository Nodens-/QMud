/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: LuaApiExport.h
 * Role: Lua API inventory exporter interface used by tooling and CMake targets to regenerate inventory snapshots.
 */

#ifndef QMUD_LUAAPIEXPORT_H
#define QMUD_LUAAPIEXPORT_H

#include <QString>

/**
 * @brief Exports Lua API inventory files to output directory.
 * @param outputDirectory Destination directory path.
 * @param errorMessage Optional output error message.
 * @return `true` when export succeeds.
 */
bool exportLuaApiInventory(const QString &outputDirectory, QString *errorMessage = nullptr);

#endif // QMUD_LUAAPIEXPORT_H
