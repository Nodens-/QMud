/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: AccessibleTextUtils.cpp
 * Role: Accessible text offset mapping helpers for native world output.
 */

#include "AccessibleTextUtils.h"

#include <QTextBoundaryFinder>

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

void QMudAccessibleTextUtils::LineOffsetIndex::clear()
{
	m_blocks.clear();
	m_lineCount = 0;
	m_lengthSum = 0;
}

void QMudAccessibleTextUtils::LineOffsetIndex::reset(const QVector<int> &lineLengths)
{
	clear();
	for (const int length : lineLengths)
		appendLength(length);
}

bool QMudAccessibleTextUtils::LineOffsetIndex::replace(const int firstLine, const int removeCount,
                                                       const QVector<int> &insertedLengths)
{
	if (firstLine < 0 || removeCount < 0 || firstLine > m_lineCount || removeCount > m_lineCount - firstLine)
	{
		return false;
	}

	if (firstLine == 0 && insertedLengths.isEmpty())
	{
		removeFront(removeCount);
		return true;
	}
	if (firstLine + removeCount == m_lineCount && insertedLengths.isEmpty())
	{
		removeBack(removeCount);
		return true;
	}
	if (firstLine == m_lineCount && removeCount == 0)
	{
		for (const int length : insertedLengths)
			appendLength(length);
		return true;
	}
	if (removeCount == 1 && insertedLengths.size() == 1)
		return setLength(firstLine, insertedLengths.constFirst());

	const QVector<int> existing = lengths();
	QVector<int>       rebuilt;
	rebuilt.reserve(m_lineCount - removeCount + insertedLengths.size());
	for (int index = 0; index < firstLine; ++index)
		rebuilt.push_back(existing.at(index));
	rebuilt.append(insertedLengths);
	for (int index = firstLine + removeCount; index < existing.size(); ++index)
		rebuilt.push_back(existing.at(index));
	reset(rebuilt);
	return true;
}

int QMudAccessibleTextUtils::LineOffsetIndex::lineCount() const
{
	return m_lineCount;
}

int QMudAccessibleTextUtils::LineOffsetIndex::characterCount() const
{
	const qint64 separators = qMax(0, m_lineCount - 1);
	return static_cast<int>(qMin<qint64>(std::numeric_limits<int>::max(), m_lengthSum + separators));
}

bool QMudAccessibleTextUtils::LineOffsetIndex::isEmpty() const
{
	return characterCount() == 0;
}

int QMudAccessibleTextUtils::LineOffsetIndex::lineLength(const int line) const
{
	if (line < 0 || line >= m_lineCount)
		return 0;
	int remaining = line;
	for (const Block &block : m_blocks)
	{
		const int blockLines = safeQSizeToInt(block.lengths.size());
		if (remaining < blockLines)
			return block.lengths.at(remaining);
		remaining -= blockLines;
	}
	return 0;
}

int QMudAccessibleTextUtils::LineOffsetIndex::offsetForPosition(const TextPosition position) const
{
	if (m_lineCount <= 0)
		return 0;
	if (position.line <= 0)
		return qBound(0, position.column, lineLength(0));
	if (position.line >= m_lineCount)
		return characterCount();

	qint64 offset         = 0;
	int    remainingLines = position.line;
	for (const Block &block : m_blocks)
	{
		const int blockLines = safeQSizeToInt(block.lengths.size());
		if (remainingLines >= blockLines)
		{
			offset += block.lengthSum + blockLines;
			remainingLines -= blockLines;
			continue;
		}
		for (int index = 0; index < remainingLines; ++index)
			offset += block.lengths.at(index) + 1LL;
		break;
	}
	offset += qBound(0, position.column, lineLength(position.line));
	return static_cast<int>(qMin<qint64>(std::numeric_limits<int>::max(), offset));
}

QMudAccessibleTextUtils::TextPosition
QMudAccessibleTextUtils::LineOffsetIndex::positionForOffset(const int offset) const
{
	if (m_lineCount <= 0)
		return {};

	qint64 target = qBound(0, offset, characterCount());
	int    line   = 0;
	for (const Block &block : m_blocks)
	{
		const int    blockLines      = safeQSizeToInt(block.lengths.size());
		const qint64 trailingBreaks  = line + blockLines < m_lineCount ? blockLines : blockLines - 1;
		const qint64 blockCharacters = block.lengthSum + qMax<qint64>(0, trailingBreaks);
		if (target > blockCharacters && line + blockLines < m_lineCount)
		{
			target -= blockCharacters;
			line += blockLines;
			continue;
		}

		for (const int length : block.lengths)
		{
			if (target <= length || line == m_lineCount - 1)
				return {line, qBound(0, static_cast<int>(qMin<qint64>(target, length)), length)};
			target -= static_cast<qint64>(length) + 1;
			++line;
		}
	}
	return {m_lineCount - 1, lineLength(m_lineCount - 1)};
}

QString QMudAccessibleTextUtils::LineOffsetIndex::text(const int startOffset, const int endOffset,
                                                       const LineTextAccessor &lineText) const
{
	if (m_lineCount <= 0)
		return {};

	const int start = qBound(0, qMin(startOffset, endOffset), characterCount());
	const int end   = qBound(start, qMax(startOffset, endOffset), characterCount());
	if (end <= start)
		return {};

	const TextPosition first = positionForOffset(start);
	LineCursor         cursor;
	if (!initializeCursor(first.line, start - first.column, cursor))
		return {};

	QString result;
	result.reserve(end - start);
	do
	{
		const int lineStart = cursor.startOffset;
		if (lineStart >= end)
			break;
		const QString &currentLineText = lineText(cursor.line);
		const int      lineEnd         = saturatedAdd(lineStart, cursorLength(cursor));
		if (start < lineEnd && end > lineStart)
		{
			const int sliceStart = qMax(start, lineStart) - lineStart;
			const int sliceEnd   = qMin(end, lineEnd) - lineStart;
			result += currentLineText.mid(sliceStart, sliceEnd - sliceStart);
		}
		if (cursor.line + 1 < m_lineCount && start <= lineEnd && end > lineEnd)
			result += QLatin1Char('\n');
	} while (advanceCursor(cursor));
	return result;
}

QMudAccessibleTextUtils::TextBoundaryResult
QMudAccessibleTextUtils::LineOffsetIndex::boundaryText(const int offset, const TextBoundaryKind boundary,
                                                       const TextBoundaryQuery query,
                                                       const LineTextAccessor &lineText) const
{
	const int count = characterCount();
	if (m_lineCount <= 0 || count <= 0)
		return {{}, 0, 0};
	const int          boundedOffset = clampedOffset(offset, count);
	const TextPosition position      = positionForOffset(boundedOffset);

	if (boundary == TextBoundaryKind::Line || boundary == TextBoundaryKind::Paragraph)
	{
		LineCursor cursor;
		if (!initializeCursor(position.line, boundedOffset - position.column, cursor))
			return {{}, 0, 0};
		if (query == TextBoundaryQuery::Before && !retreatCursor(cursor))
			return {{}, 0, 0};
		if (query == TextBoundaryQuery::After && !advanceCursor(cursor))
			return {{}, count, count};
		return {lineText(cursor.line), cursor.startOffset,
		        saturatedAdd(cursor.startOffset, cursorLength(cursor))};
	}

	if (boundary == TextBoundaryKind::Character)
	{
		int start = boundedOffset;
		if (query == TextBoundaryQuery::Before)
			--start;
		else if (query == TextBoundaryQuery::After)
			++start;
		if (start < 0)
			return {{}, 0, 0};
		if (start >= count)
			return {{}, count, count};
		const TextPosition characterPosition = positionForOffset(start);
		LineCursor         characterCursor;
		if (!initializeCursor(characterPosition.line, start - characterPosition.column, characterCursor))
			return {{}, 0, 0};
		if (characterPosition.column == cursorLength(characterCursor) &&
		    characterCursor.line + 1 < m_lineCount)
		{
			return {QString(QLatin1Char('\n')), start, saturatedAdd(start, 1)};
		}
		return {lineText(characterCursor.line).mid(characterPosition.column, 1), start,
		        saturatedAdd(start, 1)};
	}

	if (boundary == TextBoundaryKind::WholeText)
	{
		if (query != TextBoundaryQuery::At)
			return {{}, 0, 0};
		return {text(0, count, lineText), 0, count};
	}

	LineCursor cursor;
	if (!initializeCursor(position.line, boundedOffset - position.column, cursor))
		return {{}, 0, 0};
	auto lookupInLine = [&lineText, boundary](const LineCursor &lineCursor, const int localOffset,
	                                          const TextBoundaryQuery lineQuery)
	{
		return boundaryTextInLine(lineText(lineCursor.line), lineCursor.startOffset, localOffset, boundary,
		                          lineQuery);
	};
	switch (query)
	{
	case TextBoundaryQuery::Before:
		do
		{
			const int localOffset = cursor.line == position.line ? position.column : cursorLength(cursor);
			if (const BoundaryLookupResult result = lookupInLine(cursor, localOffset, query); result.found)
				return result.result;
		} while (retreatCursor(cursor));
		break;
	case TextBoundaryQuery::At:
		if (const BoundaryLookupResult result = lookupInLine(cursor, position.column, query); result.found)
		{
			return result.result;
		}
		while (retreatCursor(cursor))
		{
			if (const BoundaryLookupResult result =
			        lookupInLine(cursor, cursorLength(cursor), TextBoundaryQuery::Before);
			    result.found)
			{
				return result.result;
			}
		}
		break;
	case TextBoundaryQuery::After:
		do
		{
			const int localOffset = cursor.line == position.line ? position.column : -1;
			if (const BoundaryLookupResult result = lookupInLine(cursor, localOffset, query); result.found)
				return result.result;
		} while (advanceCursor(cursor));
		break;
	}

	return query == TextBoundaryQuery::After ? TextBoundaryResult{{}, count, count}
	                                         : TextBoundaryResult{{}, 0, 0};
}

void QMudAccessibleTextUtils::LineOffsetIndex::appendLength(const int length)
{
	const int boundedLength = qMax(0, length);
	if (m_blocks.empty() || m_blocks.back().lengths.size() >= kBlockCapacity)
		m_blocks.emplace_back();
	Block &block = m_blocks.back();
	block.lengths.push_back(boundedLength);
	block.lengthSum += boundedLength;
	m_lengthSum += boundedLength;
	++m_lineCount;
}

void QMudAccessibleTextUtils::LineOffsetIndex::removeFront(int count)
{
	count = qBound(0, count, m_lineCount);
	while (count > 0 && !m_blocks.empty())
	{
		Block    &block     = m_blocks.front();
		const int blockSize = safeQSizeToInt(block.lengths.size());
		if (count >= blockSize)
		{
			m_lengthSum -= block.lengthSum;
			m_lineCount -= blockSize;
			count -= blockSize;
			m_blocks.pop_front();
			continue;
		}
		qint64 removedSum = 0;
		for (int index = 0; index < count; ++index)
			removedSum += block.lengths.at(index);
		block.lengths.remove(0, count);
		block.lengthSum -= removedSum;
		m_lengthSum -= removedSum;
		m_lineCount -= count;
		count = 0;
	}
}

void QMudAccessibleTextUtils::LineOffsetIndex::removeBack(int count)
{
	count = qBound(0, count, m_lineCount);
	while (count > 0 && !m_blocks.empty())
	{
		Block    &block     = m_blocks.back();
		const int blockSize = safeQSizeToInt(block.lengths.size());
		if (count >= blockSize)
		{
			m_lengthSum -= block.lengthSum;
			m_lineCount -= blockSize;
			count -= blockSize;
			m_blocks.pop_back();
			continue;
		}
		qint64 removedSum = 0;
		for (int index = blockSize - count; index < blockSize; ++index)
			removedSum += block.lengths.at(index);
		block.lengths.resize(blockSize - count);
		block.lengthSum -= removedSum;
		m_lengthSum -= removedSum;
		m_lineCount -= count;
		count = 0;
	}
}

bool QMudAccessibleTextUtils::LineOffsetIndex::setLength(int line, const int length)
{
	if (line < 0 || line >= m_lineCount)
		return false;
	for (Block &block : m_blocks)
	{
		if (line >= block.lengths.size())
		{
			line -= safeQSizeToInt(block.lengths.size());
			continue;
		}
		const int boundedLength = qMax(0, length);
		const int difference    = boundedLength - block.lengths.at(line);
		block.lengths[line]     = boundedLength;
		block.lengthSum += difference;
		m_lengthSum += difference;
		return true;
	}
	return false;
}

bool QMudAccessibleTextUtils::LineOffsetIndex::initializeCursor(const int line, const int startOffset,
                                                                LineCursor &cursor) const
{
	if (line < 0 || line >= m_lineCount)
		return false;
	int remaining = line;
	for (int blockIndex = 0; blockIndex < static_cast<int>(m_blocks.size()); ++blockIndex)
	{
		const int blockLines = safeQSizeToInt(m_blocks.at(blockIndex).lengths.size());
		if (remaining < blockLines)
		{
			cursor = {blockIndex, remaining, line, qMax(0, startOffset)};
			return true;
		}
		remaining -= blockLines;
	}
	return false;
}

int QMudAccessibleTextUtils::LineOffsetIndex::cursorLength(const LineCursor &cursor) const
{
	if (cursor.blockIndex < 0 || cursor.blockIndex >= static_cast<int>(m_blocks.size()))
		return 0;
	const Block &block = m_blocks.at(cursor.blockIndex);
	if (cursor.indexInBlock < 0 || cursor.indexInBlock >= block.lengths.size())
		return 0;
	return block.lengths.at(cursor.indexInBlock);
}

bool QMudAccessibleTextUtils::LineOffsetIndex::advanceCursor(LineCursor &cursor) const
{
	if (cursor.line < 0 || cursor.line + 1 >= m_lineCount)
		return false;
	cursor.startOffset = saturatedAdd(saturatedAdd(cursor.startOffset, cursorLength(cursor)), 1);
	++cursor.line;
	++cursor.indexInBlock;
	if (cursor.blockIndex < static_cast<int>(m_blocks.size()) &&
	    cursor.indexInBlock >= m_blocks.at(cursor.blockIndex).lengths.size())
	{
		++cursor.blockIndex;
		cursor.indexInBlock = 0;
	}
	return cursor.blockIndex >= 0 && cursor.blockIndex < static_cast<int>(m_blocks.size());
}

bool QMudAccessibleTextUtils::LineOffsetIndex::retreatCursor(LineCursor &cursor) const
{
	if (cursor.line <= 0 || cursor.line >= m_lineCount)
		return false;
	--cursor.line;
	if (cursor.indexInBlock > 0)
	{
		--cursor.indexInBlock;
	}
	else
	{
		--cursor.blockIndex;
		if (cursor.blockIndex < 0 || cursor.blockIndex >= static_cast<int>(m_blocks.size()))
			return false;
		cursor.indexInBlock = safeQSizeToInt(m_blocks.at(cursor.blockIndex).lengths.size()) - 1;
	}
	cursor.startOffset = qMax(0, cursor.startOffset - cursorLength(cursor) - 1);
	return true;
}

QVector<int> QMudAccessibleTextUtils::LineOffsetIndex::lengths() const
{
	QVector<int> result;
	result.reserve(m_lineCount);
	for (const Block &block : m_blocks)
		result.append(block.lengths);
	return result;
}

QMudAccessibleTextUtils::LineOffsetMap::LineOffsetMap(QVector<QString> lines)
{
	reset(std::move(lines));
}

void QMudAccessibleTextUtils::LineOffsetMap::reset(QVector<QString> lines)
{
	m_lines = std::move(lines);
	QVector<int> lengths;
	lengths.reserve(m_lines.size());
	for (const QString &line : std::as_const(m_lines))
		lengths.push_back(safeQSizeToInt(line.size()));
	m_index.reset(lengths);
}

int QMudAccessibleTextUtils::LineOffsetMap::lineCount() const
{
	return m_index.lineCount();
}

int QMudAccessibleTextUtils::LineOffsetMap::characterCount() const
{
	return m_index.characterCount();
}

bool QMudAccessibleTextUtils::LineOffsetMap::isEmpty() const
{
	return m_index.isEmpty();
}

int QMudAccessibleTextUtils::LineOffsetMap::offsetForPosition(const TextPosition position) const
{
	return m_index.offsetForPosition(position);
}

QMudAccessibleTextUtils::TextPosition
QMudAccessibleTextUtils::LineOffsetMap::positionForOffset(const int offset) const
{
	return m_index.positionForOffset(offset);
}

QString QMudAccessibleTextUtils::LineOffsetMap::text(const int startOffset, const int endOffset) const
{
	return m_index.text(startOffset, endOffset,
	                    [this](const int line) -> const QString & { return m_lines.at(line); });
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
	return m_index.boundaryText(offset, boundary, query,
	                            [this](const int line) -> const QString & { return m_lines.at(line); });
}
