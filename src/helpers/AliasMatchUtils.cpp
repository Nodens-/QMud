/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: AliasMatchUtils.cpp
 * Role: Shared helpers for alias/trigger regex capture extraction and execution-depth guards.
 */

#include "AliasMatchUtils.h"

#include <limits>

namespace
{
	int safeQSizeToInt(const qsizetype size)
	{
		if (size <= 0)
			return 0;
		constexpr qsizetype kMaxInt = std::numeric_limits<int>::max();
		return size > kMaxInt ? std::numeric_limits<int>::max() : static_cast<int>(size);
	}
} // namespace

namespace QMudAliasMatch
{
	MatchResult matchWithCaptures(const QRegularExpression &regex, const QString &subject,
	                              const bool allowEmptyMatch, const int startOffset)
	{
		MatchResult                   out;
		const QRegularExpressionMatch match = regex.match(subject, startOffset);
		if (!match.hasMatch())
			return out;

		if (!allowEmptyMatch && match.capturedLength(0) == 0)
			return out;

		out.matched  = true;
		out.startCol = safeQSizeToInt(match.capturedStart(0));
		out.endCol   = safeQSizeToInt(match.capturedEnd(0));

		const int lastIndex = match.lastCapturedIndex();
		out.wildcards.reserve(lastIndex + 1);
		for (int i = 0; i <= lastIndex; ++i)
			out.wildcards.append(match.captured(i));

		const QStringList names = regex.namedCaptureGroups();
		for (int i = 1, size = safeQSizeToInt(names.size()); i < size; ++i)
		{
			const QString &name = names.at(i);
			if (name.isEmpty())
				continue;
			if (i <= lastIndex)
				out.namedWildcards.insert(name, match.captured(i));
		}

		return out;
	}

	bool exceedsExecutionDepth(const int currentDepth, const int maxDepth)
	{
		return currentDepth + 1 > maxDepth;
	}
} // namespace QMudAliasMatch
