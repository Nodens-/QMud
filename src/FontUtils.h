/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: FontUtils.h
 * Role: Font-selection helper interfaces that enforce cross-platform monospace fallbacks for QMud editors and views.
 */

#ifndef QMUD_FONTUTILS_H
#define QMUD_FONTUTILS_H

#include <QFontDatabase>
#include <QString>

class QFont;

/**
 * @brief Applies fallback monospace family to font when needed.
 * @param font Font instance to update.
 * @param preferredFamily Preferred family name.
 */
void    qmudApplyMonospaceFallback(QFont &font, const QString &preferredFamily = QString());
/**
 * @brief Returns preferred monospace font with optional family and size.
 * @param preferredFamily Preferred family name.
 * @param pointSize Requested point size.
 * @return Selected monospace font.
 */
QFont   qmudPreferredMonospaceFont(const QString &preferredFamily = QString(), int pointSize = 0);
/**
 * @brief Maps Windows charset id to Qt writing system.
 * @param charset Windows charset id.
 * @param outWritingSystem Output Qt writing system.
 * @return `true` when mapping is available.
 */
bool    qmudMapWindowsCharsetToWritingSystem(int charset, QFontDatabase::WritingSystem *outWritingSystem);
/**
 * @brief Chooses suitable font family for preferred family and charset.
 * @param preferredFamily Preferred family name.
 * @param charset Windows charset id.
 * @return Selected family name.
 */
QString qmudFamilyForCharset(const QString &preferredFamily, int charset);

#endif // QMUD_FONTUTILS_H
