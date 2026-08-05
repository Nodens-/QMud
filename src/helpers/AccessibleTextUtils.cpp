/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: AccessibleTextUtils.cpp
 * Role: Accessible text offset mapping helpers for native world output.
 */

#include "AccessibleTextUtils.h"

#include <QTextBoundaryFinder>

#include <algorithm>
#include <limits>
#include <utility>

namespace
{
	struct BoundaryLookupResult
	{
			bool                                        found{false};
			QMudAccessibleTextUtils::TextBoundaryResult result;
	};

	int safeQSizeToInt(const qsizetype size)
	{
		if (size <= 0)
			return 0;
		constexpr qsizetype kMaxInt = std::numeric_limits<int>::max();
		return size > kMaxInt ? std::numeric_limits<int>::max() : static_cast<int>(size);
	}

	int saturatedAdd(const int value, const int amount)
	{
		if (amount <= 0)
			return value;
		constexpr int kMaxInt = std::numeric_limits<int>::max();
		if (value >= kMaxInt - amount)
			return kMaxInt;
		return value + amount;
	}

	int clampedOffset(const int offset, const int characterCount)
	{
		return qBound(0, offset, characterCount);
	}

	bool hasNonSpaceText(const QString &text, const int start, const int end)
	{
		for (int i = start; i < end; ++i)
		{
			if (!text.at(i).isSpace())
				return true;
		}
		return false;
	}

	BoundaryLookupResult boundaryTextInLine(const QString &lineText, const int lineStartOffset,
	                                        const int                                        localOffset,
	                                        const QMudAccessibleTextUtils::TextBoundaryKind  boundary,
	                                        const QMudAccessibleTextUtils::TextBoundaryQuery query)
	{
		if (lineText.isEmpty())
			return {};

		const QTextBoundaryFinder::BoundaryType finderType =
		    boundary == QMudAccessibleTextUtils::TextBoundaryKind::Sentence ? QTextBoundaryFinder::Sentence
		                                                                    : QTextBoundaryFinder::Word;
		QTextBoundaryFinder finder(finderType, lineText);
		constexpr qsizetype kNoBoundary   = -1;
		qsizetype           start         = 0;
		int                 selectedStart = -1;
		int                 selectedEnd   = -1;
		for (qsizetype end = finder.toNextBoundary(); end != kNoBoundary;
		     start = end, end = finder.toNextBoundary())
		{
			const int rangeStart = safeQSizeToInt(start);
			const int rangeEnd   = safeQSizeToInt(end);
			if (rangeEnd <= rangeStart || !hasNonSpaceText(lineText, rangeStart, rangeEnd))
				continue;

			switch (query)
			{
			case QMudAccessibleTextUtils::TextBoundaryQuery::Before:
				if (rangeEnd <= localOffset)
				{
					selectedStart = rangeStart;
					selectedEnd   = rangeEnd;
					continue;
				}
				break;
			case QMudAccessibleTextUtils::TextBoundaryQuery::At:
				if (localOffset >= rangeStart && localOffset < rangeEnd)
				{
					selectedStart = rangeStart;
					selectedEnd   = rangeEnd;
				}
				else if (rangeEnd <= localOffset)
				{
					selectedStart = rangeStart;
					selectedEnd   = rangeEnd;
					continue;
				}
				break;
			case QMudAccessibleTextUtils::TextBoundaryQuery::After:
				if (rangeStart > localOffset)
				{
					selectedStart = rangeStart;
					selectedEnd   = rangeEnd;
				}
				break;
			}
			break;
		}

		if (selectedStart < 0)
			return {};
		const int globalStart = saturatedAdd(lineStartOffset, selectedStart);
		const int globalEnd   = saturatedAdd(lineStartOffset, selectedEnd);
		const QMudAccessibleTextUtils::TextBoundaryResult result{
		    lineText.mid(selectedStart, selectedEnd - selectedStart), globalStart, globalEnd};
		return {true, result};
	}

} // namespace

QMudAccessibleTextUtils::LineOffsetMap::LineOffsetMap(QVector<QString> lines)
{
	reset(std::move(lines));
}

void QMudAccessibleTextUtils::LineOffsetMap::reset(QVector<QString> lines)
{
	m_lines = std::move(lines);
	rebuildPrefixes();
}

int QMudAccessibleTextUtils::LineOffsetMap::lineCount() const
{
	return safeQSizeToInt(m_lines.size());
}

int QMudAccessibleTextUtils::LineOffsetMap::characterCount() const
{
	return m_characterCount;
}

bool QMudAccessibleTextUtils::LineOffsetMap::isEmpty() const
{
	return m_characterCount == 0;
}

int QMudAccessibleTextUtils::LineOffsetMap::offsetForPosition(const TextPosition position) const
{
	if (m_lines.isEmpty())
		return 0;
	if (position.line <= 0)
		return qBound(0, position.column, clampedLineLength(0));
	if (position.line >= m_lines.size())
		return m_characterCount;

	const int line   = position.line;
	const int column = qBound(0, position.column, clampedLineLength(line));
	return saturatedAdd(m_lineStarts.at(line), column);
}

QMudAccessibleTextUtils::TextPosition
QMudAccessibleTextUtils::LineOffsetMap::positionForOffset(const int offset) const
{
	if (m_lines.isEmpty())
		return {};

	const int  target = clampedOffset(offset, m_characterCount);
	const auto begin  = m_lineStarts.cbegin();
	const auto end    = m_lineStarts.cend();
	const auto upper  = std::upper_bound(begin, end, target);
	const int  line   = upper == begin ? 0 : static_cast<int>(std::distance(begin, upper) - 1);

	const int  column = qBound(0, target - m_lineStarts.at(line), clampedLineLength(line));
	return TextPosition{line, column};
}

QString QMudAccessibleTextUtils::LineOffsetMap::text(const int startOffset, const int endOffset) const
{
	if (m_lines.isEmpty())
		return {};

	const int start = clampedOffset(qMin(startOffset, endOffset), m_characterCount);
	const int end   = clampedOffset(qMax(startOffset, endOffset), m_characterCount);
	if (end <= start)
		return {};

	QString result;
	result.reserve(end - start);
	for (int line = 0; line < m_lines.size(); ++line)
	{
		const int lineStart = m_lineStarts.at(line);
		if (lineStart >= end)
			break;

		const QString &lineText = m_lines.at(line);
		const int      lineEnd  = saturatedAdd(lineStart, clampedLineLength(line));
		if (start < lineEnd && end > lineStart)
		{
			const int sliceStart = qMax(start, lineStart) - lineStart;
			const int sliceEnd   = qMin(end, lineEnd) - lineStart;
			result += lineText.mid(sliceStart, sliceEnd - sliceStart);
		}

		const bool hasSeparator = line + 1 < m_lines.size();
		if (hasSeparator && start <= lineEnd && end > lineEnd)
			result += QLatin1Char('\n');
	}
	return result;
}

QString QMudAccessibleTextUtils::LineOffsetMap::selectedText(const TextPosition anchor,
                                                             const TextPosition cursor) const
{
	const int anchorOffset = offsetForPosition(anchor);
	const int cursorOffset = offsetForPosition(cursor);
	return text(qMin(anchorOffset, cursorOffset), qMax(anchorOffset, cursorOffset));
}

QMudAccessibleTextUtils::TextBoundaryResult
QMudAccessibleTextUtils::LineOffsetMap::boundaryText(const int offset, const TextBoundaryKind boundary,
                                                     const TextBoundaryQuery query) const
{
	switch (boundary)
	{
	case TextBoundaryKind::Character:
		return characterBoundaryText(offset, query);
	case TextBoundaryKind::Paragraph:
	case TextBoundaryKind::Line:
		return lineBoundaryText(offset, query);
	case TextBoundaryKind::WholeText:
		return wholeTextBoundaryText(query);
	case TextBoundaryKind::Word:
	case TextBoundaryKind::Sentence:
		return qtBoundaryText(offset, boundary, query);
	}
	return {};
}

void QMudAccessibleTextUtils::LineOffsetMap::rebuildPrefixes()
{
	m_lineStarts.clear();
	m_lineStarts.reserve(m_lines.size());

	int offset = 0;
	for (int line = 0; line < m_lines.size(); ++line)
	{
		m_lineStarts.push_back(offset);
		offset = saturatedAdd(offset, clampedLineLength(line));
		if (line + 1 < m_lines.size())
			offset = saturatedAdd(offset, 1);
	}
	m_characterCount = offset;
}

int QMudAccessibleTextUtils::LineOffsetMap::clampedLineLength(const int line) const
{
	if (line < 0 || line >= m_lines.size())
		return 0;
	return safeQSizeToInt(m_lines.at(line).size());
}

QMudAccessibleTextUtils::TextBoundaryResult
QMudAccessibleTextUtils::LineOffsetMap::lineBoundaryText(const int               offset,
                                                         const TextBoundaryQuery query) const
{
	if (m_lines.isEmpty())
		return {{}, 0, 0};

	const int          boundedOffset = clampedOffset(offset, m_characterCount);
	const TextPosition position      = positionForOffset(boundedOffset);
	int                line          = position.line;
	switch (query)
	{
	case TextBoundaryQuery::Before:
		--line;
		break;
	case TextBoundaryQuery::After:
		++line;
		break;
	case TextBoundaryQuery::At:
		break;
	}

	if (line < 0)
		return {{}, 0, 0};
	if (line >= lineCount())
		return {{}, m_characterCount, m_characterCount};

	const int start = m_lineStarts.at(line);
	const int end   = saturatedAdd(start, clampedLineLength(line));
	return {m_lines.at(line), start, end};
}

QMudAccessibleTextUtils::TextBoundaryResult
QMudAccessibleTextUtils::LineOffsetMap::characterBoundaryText(const int               offset,
                                                              const TextBoundaryQuery query) const
{
	if (m_characterCount <= 0)
		return {{}, 0, 0};

	const int boundedOffset = clampedOffset(offset, m_characterCount);
	int       start         = boundedOffset;
	switch (query)
	{
	case TextBoundaryQuery::Before:
		start = boundedOffset - 1;
		break;
	case TextBoundaryQuery::After:
		start = boundedOffset + 1;
		break;
	case TextBoundaryQuery::At:
		break;
	}

	if (start < 0)
		return {{}, 0, 0};
	if (start >= m_characterCount)
		return {{}, m_characterCount, m_characterCount};

	const TextPosition position   = positionForOffset(start);
	const int          lineLength = clampedLineLength(position.line);
	if (position.column == lineLength && position.line + 1 < lineCount())
		return {QString(QLatin1Char('\n')), start, saturatedAdd(start, 1)};
	return {m_lines.at(position.line).mid(position.column, 1), start, saturatedAdd(start, 1)};
}

QMudAccessibleTextUtils::TextBoundaryResult
QMudAccessibleTextUtils::LineOffsetMap::wholeTextBoundaryText(const TextBoundaryQuery query) const
{
	if (query != TextBoundaryQuery::At || m_characterCount <= 0)
		return {{}, 0, 0};
	return {text(0, m_characterCount), 0, m_characterCount};
}

QMudAccessibleTextUtils::TextBoundaryResult
QMudAccessibleTextUtils::LineOffsetMap::qtBoundaryText(const int offset, const TextBoundaryKind boundary,
                                                       const TextBoundaryQuery query) const
{
	if (m_characterCount <= 0)
		return {{}, 0, 0};

	const int          boundedOffset = clampedOffset(offset, m_characterCount);
	const TextPosition position      = positionForOffset(boundedOffset);
	const int          totalLines    = lineCount();
	switch (query)
	{
	case TextBoundaryQuery::Before:
		for (int line = qMin(position.line, totalLines - 1); line >= 0; --line)
		{
			const int localOffset = line == position.line ? position.column : clampedLineLength(line);
			if (const BoundaryLookupResult result =
			        boundaryTextInLine(m_lines.at(line), m_lineStarts.at(line), localOffset, boundary, query);
			    result.found)
			{
				return result.result;
			}
		}
		break;
	case TextBoundaryQuery::At:
		if (position.line >= 0 && position.line < totalLines)
		{
			if (const BoundaryLookupResult result =
			        boundaryTextInLine(m_lines.at(position.line), m_lineStarts.at(position.line),
			                           position.column, boundary, query);
			    result.found)
			{
				return result.result;
			}
		}
		for (int line = qMin(position.line - 1, totalLines - 1); line >= 0; --line)
		{
			if (const BoundaryLookupResult result =
			        boundaryTextInLine(m_lines.at(line), m_lineStarts.at(line), clampedLineLength(line),
			                           boundary, TextBoundaryQuery::Before);
			    result.found)
			{
				return result.result;
			}
		}
		break;
	case TextBoundaryQuery::After:
		for (int line = qMax(0, position.line); line < totalLines; ++line)
		{
			const int localOffset = line == position.line ? position.column : -1;
			if (const BoundaryLookupResult result =
			        boundaryTextInLine(m_lines.at(line), m_lineStarts.at(line), localOffset, boundary, query);
			    result.found)
			{
				return result.result;
			}
		}
		break;
	}

	return query == TextBoundaryQuery::After ? TextBoundaryResult{{}, m_characterCount, m_characterCount}
	                                         : TextBoundaryResult{{}, 0, 0};
}
