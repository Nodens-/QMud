/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: AsciiArt.h
 * Role: ASCII-art helper interfaces used by scripts/runtime features that generate or transform text-art output.
 */

#ifndef QMUD_ASCIIART_H
#define QMUD_ASCIIART_H

#include <QStringList>

namespace QMudAsciiArt
{
	bool render(const QString &text, const QString &fontFilePath, int layoutMode, QStringList *outLines,
	            QString *errorMessage);
}

#endif // QMUD_ASCIIART_H
