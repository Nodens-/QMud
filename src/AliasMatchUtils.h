/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: AliasMatchUtils.h
 * Role: Shared helpers for alias/trigger regex capture extraction and execution-depth guards.
 */

#ifndef QMUD_ALIASMATCHUTILS_H
#define QMUD_ALIASMATCHUTILS_H

#include <QMap>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace QMudAliasMatch
{
	/**
	 * @brief Captured result details for one alias/trigger regex match operation.
	 */
	struct MatchResult
	{
			bool                   matched{false};
			int                    startCol{0};
			int                    endCol{0};
			QStringList            wildcards;
			QMap<QString, QString> namedWildcards;
	};

	/**
	 * @brief Executes regex match and extracts positional/named captures.
	 * @param regex Compiled regex pattern.
	 * @param subject Input command text.
	 * @param allowEmptyMatch Accept zero-length match when `true`.
	 * @param startOffset Start offset for regex matching.
	 * @return Match result with capture data and span columns.
	 */
	MatchResult matchWithCaptures(const QRegularExpression &regex, const QString &subject,
	                              bool allowEmptyMatch, int startOffset = 0);

	/**
	 * @brief Checks whether alias/trigger nested execution depth exceeds configured limit.
	 * @param currentDepth Current nested execution depth.
	 * @param maxDepth Configured maximum allowed depth.
	 * @return `true` when execution must stop due to depth overflow.
	 */
	bool        exceedsExecutionDepth(int currentDepth, int maxDepth);
} // namespace QMudAliasMatch

#endif // QMUD_ALIASMATCHUTILS_H
