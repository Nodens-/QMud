/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: StringUtils.h
 * Role: Reusable QString/string manipulation helpers used by runtime parsing, command processing, and UI formatting.
 */

#ifndef QMUD_STRINGUTILS_H
#define QMUD_STRINGUTILS_H

#include <QString>

/**
 * @brief Converts command id to canonical command string.
 * @param id Command identifier.
 * @return Canonical command string.
 */
QString qmudCommandIdToString(int id);
/**
 * @brief Converts command string to command id.
 * @param command Command string.
 * @return Command identifier, or 0 when unknown.
 */
int     qmudStringToCommandId(const QString &command);
/**
 * @brief Expands escape sequences in user-facing text.
 * @param source Source text.
 * @return Text with escape sequences expanded.
 */
QString qmudFixupEscapeSequences(const QString &source);
/**
 * @brief Applies German character fixups used by legacy behavior.
 * @param message Source text.
 * @return Text with legacy German fixups applied.
 */
QString qmudFixUpGerman(const QString &message);
/**
 * @brief Parses enabled flag strings (y/n/true/false).
 * @param value Flag text.
 * @return Parsed boolean flag value.
 */
bool    qmudIsEnabledFlag(const QString &value);
/**
 * @brief Converts boolean to legacy y/n string.
 * @param value Boolean value.
 * @return Legacy `y`/`n` string.
 */
QString qmudBoolToYn(bool value);
/**
 * @brief Computes edit distance between two strings.
 * @param source Source text.
 * @param target Target text.
 * @return Edit distance.
 */
int     qmudEditDistance(QStringView source, QStringView target);
/**
 * @brief Replaces target substring in source with replacement text.
 * @param source Source text.
 * @param target Target substring.
 * @param replacement Replacement substring.
 * @param replaceAll Replace all matches when `true`, only first otherwise.
 * @return Resulting string.
 */
QString qmudReplaceString(const QString &source, const QString &target, const QString &replacement,
                          bool replaceAll = true);

#endif // QMUD_STRINGUTILS_H
