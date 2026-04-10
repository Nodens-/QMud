/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: LuaUtf8Utils.h
 * Role: Helpers for UTF-8 argument packing used by Lua callback dispatch.
 */

#ifndef QMUD_LUAUTF8UTILS_H
#define QMUD_LUAUTF8UTILS_H

#include <QByteArray>
#include <QString>

#include <array>

/**
 * @brief Encodes three QString callback arguments to UTF-8.
 * @param arg2 First argument.
 * @param arg3 Second argument.
 * @param arg4 Third argument.
 * @return UTF-8 encoded byte-array triplet.
 */
inline std::array<QByteArray, 3> qmudEncodeUtf8Triplet(const QString &arg2, const QString &arg3,
                                                       const QString &arg4)
{
	return {arg2.toUtf8(), arg3.toUtf8(), arg4.toUtf8()};
}

#endif // QMUD_LUAUTF8UTILS_H
