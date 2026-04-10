/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: CommandPatternUtils.h
 * Role: Pure helpers for wildcard/regex conversion used by command processing and test seams.
 */

#ifndef QMUD_COMMANDPATTERNUTILS_H
#define QMUD_COMMANDPATTERNUTILS_H

#include <QString>

namespace QMudCommandPattern
{
	/**
	 * @brief Converts wildcard/regexp command pattern into a normalized Qt regex string.
	 * @param matchString Raw user pattern text.
	 * @param wholeLine Anchor pattern to full line when `true`.
	 * @param makeAsterisksWildcards Interpret `*` as wildcard groups when `true`.
	 * @return Regex pattern string suitable for `QRegularExpression`.
	 */
	QString convertToRegularExpression(const QString &matchString, bool wholeLine = true,
	                                   bool makeAsterisksWildcards = true);
} // namespace QMudCommandPattern

#endif // QMUD_COMMANDPATTERNUTILS_H
