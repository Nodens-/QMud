/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: StringUtils.h
 * Role: Reusable QString/string manipulation helpers used by runtime parsing, command processing, and UI formatting.
 */

#ifndef QMUD_STRINGUTILS_H
#define QMUD_STRINGUTILS_H

// ReSharper disable once CppUnusedIncludeDirective
#include <QString>
#include <QStringList>

/**
 * @brief Converts command id to canonical command string.
 * @param id Command identifier.
 * @return Canonical command string.
 */
QString                   qmudCommandIdToString(int id);
/**
 * @brief Converts command string to command id.
 * @param command Command string.
 * @return Command identifier, or 0 when unknown.
 */
int                       qmudStringToCommandId(const QString &command);
/**
 * @brief Expands escape sequences in user-facing text.
 * @param source Source text.
 * @return Text with escape sequences expanded.
 */
QString                   qmudFixupEscapeSequences(const QString &source);
/**
 * @brief Applies German character fixups used by legacy behavior.
 * @param message Source text.
 * @return Text with legacy German fixups applied.
 */
QString                   qmudFixUpGerman(const QString &message);
/**
 * @brief Removes ANSI CSI escape sequences from text.
 * @param input Source text that may include terminal escape codes.
 * @return Text with ANSI control sequences removed.
 */
QString                   qmudStripAnsiEscapeCodes(const QString &input);
/**
 * @brief Parses enabled flag strings (y/n/true/false).
 * @param value Flag text.
 * @return Parsed boolean flag value.
 */
bool                      qmudIsEnabledFlag(const QString &value);
/**
 * @brief Parses a textual boolean keyword without treating arbitrary text as false.
 * @param value Flag text.
 * @param result Receives the parsed boolean value when recognized.
 * @return `true` for y/yes/true or n/no/false, case-insensitively; otherwise `false`.
 */
[[nodiscard]] inline bool qmudParseBooleanKeyword(const QString &value, bool &result)
{
	if (value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
	    value.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0 ||
	    value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0)
	{
		result = true;
		return true;
	}
	if (value.compare(QStringLiteral("n"), Qt::CaseInsensitive) == 0 ||
	    value.compare(QStringLiteral("no"), Qt::CaseInsensitive) == 0 ||
	    value.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0)
	{
		result = false;
		return true;
	}
	return false;
}
/**
 * @brief Converts boolean to legacy y/n string.
 * @param value Boolean value.
 * @return Legacy `y`/`n` string.
 */
QString     qmudBoolToYn(bool value);
/**
 * @brief Computes edit distance between two strings.
 * @param source Source text.
 * @param target Target text.
 * @return Edit distance.
 */
int         qmudEditDistance(QStringView source, QStringView target);
/**
 * @brief Replaces target substring in source with replacement text.
 * @param source Source text.
 * @param target Target substring.
 * @param replacement Replacement substring.
 * @param replaceAll Replace all matches when `true`, only first otherwise.
 * @return Resulting string.
 */
QString     qmudReplaceString(const QString &source, const QString &target, const QString &replacement,
                              bool replaceAll = true);
/**
 * @brief Splits a legacy pipe-delimited menu string and trims spaces around each token.
 * @param items Source menu text.
 * @return Menu tokens preserving empty fields after trimming.
 */
QStringList qmudSplitLegacyMenuItems(const QString &items);

#endif // QMUD_STRINGUTILS_H
