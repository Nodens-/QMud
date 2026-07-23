/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: OutputWrapUtils.cpp
 * Role: Fixed-column output wrapping helpers shared by runtime, command processing, and tests.
 */

#include "OutputWrapUtils.h"

#include <QTextBoundaryFinder>

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

	int boundedQSizeToInt(const qsizetype size)
	{
		return safeQSizeToInt(size);
	}

	bool isEnabledFlag(const QString &value)
	{
		return value == QStringLiteral("1") || value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
		       value.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0 ||
		       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
	}

	int wrapColumnFromWorldSettings(const QMap<QString, QString> &attrs)
	{
		return attrs.value(QStringLiteral("wrap_column")).toInt();
	}

	int wrapColumnWithWorldMinimum(const int calculatedColumns, const int worldWrapColumn)
	{
		if (worldWrapColumn <= 0)
			return calculatedColumns;
		if (calculatedColumns <= 0)
			return worldWrapColumn;
		return qMax(calculatedColumns, worldWrapColumn);
	}

	int localWrapColumnForOutput(const QMap<QString, QString> &attrs, const bool nawsNegotiated,
	                             const int windowColumns)
	{
		if (!isEnabledFlag(attrs.value(QStringLiteral("wrap"))))
			return 0;

		const int  worldWrapColumn = wrapColumnFromWorldSettings(attrs);
		const bool autoWrapWindow  = isEnabledFlag(attrs.value(QStringLiteral("auto_wrap_window_width")));

		if (nawsNegotiated)
			return wrapColumnWithWorldMinimum(windowColumns, worldWrapColumn);

		if (autoWrapWindow)
			return wrapColumnWithWorldMinimum(windowColumns, worldWrapColumn);

		return worldWrapColumn;
	}

	bool indentParasEnabled(const QMap<QString, QString> &attrs)
	{
		const QString indentValue = attrs.value(QStringLiteral("indent_paras"));
		return !(indentValue == QStringLiteral("0") ||
		         indentValue.compare(QStringLiteral("n"), Qt::CaseInsensitive) == 0 ||
		         indentValue.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0);
	}

	bool sameStyleForWrap(const WorldRuntime::StyleSpan &a, const WorldRuntime::StyleSpan &b)
	{
		return a.hasSameStyleAttributes(b);
	}

	void appendMergedStyleSpan(QVector<WorldRuntime::StyleSpan> &spans, const WorldRuntime::StyleSpan &span)
	{
		if (span.length <= 0)
			return;
		if (!spans.isEmpty() && sameStyleForWrap(spans.last(), span))
		{
			spans.last().length += span.length;
			return;
		}
		spans.push_back(span);
	}

	QVector<WorldRuntime::StyleSpan> slicedStyleSpans(const QVector<WorldRuntime::StyleSpan> &spans,
	                                                  const int start, const int length)
	{
		QVector<WorldRuntime::StyleSpan> result;
		if (spans.isEmpty() || length <= 0)
			return result;

		const int end      = start + length;
		int       spanBase = 0;
		for (const WorldRuntime::StyleSpan &span : spans)
		{
			const int spanLength = qMax(0, span.length);
			const int spanEnd    = spanBase + spanLength;
			if (spanEnd <= start)
			{
				spanBase = spanEnd;
				continue;
			}
			if (spanBase >= end)
				break;

			const int overlapStart = qMax(start, spanBase);
			const int overlapEnd   = qMin(end, spanEnd);
			if (overlapStart < overlapEnd)
			{
				WorldRuntime::StyleSpan sliced = span;
				sliced.length                  = overlapEnd - overlapStart;
				appendMergedStyleSpan(result, sliced);
			}
			spanBase = spanEnd;
		}
		return result;
	}

	bool isSingleCharGrapheme(const QStringView grapheme, const QChar ch)
	{
		return grapheme.size() == 1 && grapheme.at(0) == ch;
	}

	bool isWhitespaceGrapheme(const QStringView grapheme)
	{
		for (const QChar ch : grapheme)
		{
			if (!ch.isSpace())
				return false;
		}
		return !grapheme.isEmpty();
	}

	bool isZeroWidthWrapCodeUnit(const QChar ch)
	{
		switch (ch.category())
		{
		case QChar::Mark_NonSpacing:
		case QChar::Mark_SpacingCombining:
		case QChar::Mark_Enclosing:
		case QChar::Other_Format:
			return true;
		default:
			break;
		}
		return false;
	}

	int graphemeColumnWidthForWrap(const QStringView grapheme)
	{
		if (grapheme.isEmpty())
			return 0;
		bool hasVisibleCodeUnit = false;
		for (const QChar ch : grapheme)
		{
			if (isZeroWidthWrapCodeUnit(ch))
				continue;
			hasVisibleCodeUnit = true;
			break;
		}
		return hasVisibleCodeUnit ? 1 : 0;
	}
} // namespace

QMudOutputWrapUtils::FixedColumnWrapConfig
QMudOutputWrapUtils::localOutputWrapConfig(const QMap<QString, QString> &attrs, const bool nawsNegotiated,
                                           const int windowColumns)
{
	FixedColumnWrapConfig config;
	config.indentParas = indentParasEnabled(attrs);
	config.wrapColumn  = localWrapColumnForOutput(attrs, nawsNegotiated, windowColumns);
	config.enabled     = config.wrapColumn > 0;
	return config;
}

int QMudOutputWrapUtils::trailingLineColumnWidthForWrap(const QString &text)
{
	if (text.isEmpty())
		return 0;

	constexpr auto      kNoBoundary = qsizetype{-1};
	QTextBoundaryFinder boundary(QTextBoundaryFinder::Grapheme, text);
	const QStringView   textView{text};
	int                 column = 0;
	for (qsizetype graphemeStart = 0, graphemeEnd = boundary.toNextBoundary(); graphemeEnd != kNoBoundary;
	     graphemeStart = graphemeEnd, graphemeEnd = boundary.toNextBoundary())
	{
		const QStringView grapheme = textView.sliced(graphemeStart, graphemeEnd - graphemeStart);
		if (isSingleCharGrapheme(grapheme, QLatin1Char('\n')))
		{
			column = 0;
			continue;
		}
		column += graphemeColumnWidthForWrap(grapheme);
	}
	return column;
}

void QMudOutputWrapUtils::wrapPlainLineForColumn(QString &text, const int wrapColumn, const bool indentParas,
                                                 const int firstLinePrefixColumns)
{
	if (wrapColumn <= 0 || text.isEmpty())
	{
		return;
	}
	constexpr auto kNoBoundary = qsizetype{-1};

	QString        wrappedText;
	wrappedText.reserve(safeQSizeToInt(text.size() + text.size() / qMax(1, wrapColumn)));

	int  column             = qMax(0, firstLinePrefixColumns);
	int  lineStart          = 0;
	int  lastSpace          = -1;
	bool lineHasVisibleChar = column > 0;

	auto recomputeLineState = [&]
	{
		column             = 0;
		lineStart          = 0;
		lastSpace          = -1;
		lineHasVisibleChar = false;
		for (int i = safeQSizeToInt(wrappedText.size()) - 1; i >= 0; --i)
		{
			if (wrappedText.at(i) == QLatin1Char('\n'))
			{
				lineStart = i + 1;
				break;
			}
		}
		QTextBoundaryFinder boundary(QTextBoundaryFinder::Grapheme, wrappedText);
		boundary.setPosition(lineStart);
		const QStringView wrappedView{wrappedText};
		for (qsizetype graphemeStart                   = static_cast<qsizetype>(lineStart),
		               graphemeEnd                     = boundary.toNextBoundary();
		     graphemeEnd != kNoBoundary; graphemeStart = graphemeEnd, graphemeEnd = boundary.toNextBoundary())
		{
			const QStringView grapheme = wrappedView.sliced(graphemeStart, graphemeEnd - graphemeStart);
			if (isSingleCharGrapheme(grapheme, QLatin1Char('\n')))
			{
				lineStart          = boundedQSizeToInt(graphemeEnd);
				column             = 0;
				lastSpace          = -1;
				lineHasVisibleChar = false;
				continue;
			}
			if (isSingleCharGrapheme(grapheme, QLatin1Char(' ')))
				lastSpace = boundedQSizeToInt(graphemeEnd - 1);
			if (!isWhitespaceGrapheme(grapheme))
				lineHasVisibleChar = true;
			column += graphemeColumnWidthForWrap(grapheme);
		}
	};

	QTextBoundaryFinder boundary(QTextBoundaryFinder::Grapheme, text);
	const QStringView   textView{text};
	for (qsizetype graphemeStart = 0, graphemeEnd = boundary.toNextBoundary(); graphemeEnd != kNoBoundary;
	     graphemeStart = graphemeEnd, graphemeEnd = boundary.toNextBoundary())
	{
		const QStringView grapheme = textView.sliced(graphemeStart, graphemeEnd - graphemeStart);
		if (isSingleCharGrapheme(grapheme, QLatin1Char('\n')))
		{
			wrappedText.append(grapheme);
			column             = 0;
			lineStart          = safeQSizeToInt(wrappedText.size());
			lastSpace          = -1;
			lineHasVisibleChar = false;
			continue;
		}

		const int graphemeWidth = graphemeColumnWidthForWrap(grapheme);
		if (lineHasVisibleChar && graphemeWidth > 0 && column + graphemeWidth > wrapColumn)
		{
			int insertPos = safeQSizeToInt(wrappedText.size());
			if (lastSpace >= lineStart)
			{
				insertPos = indentParas ? lastSpace : lastSpace + 1;

				bool onlyWhitespace = true;
				for (int j = lineStart, size = safeQSizeToInt(wrappedText.size()); j < insertPos && j < size;
				     ++j)
				{
					if (!wrappedText.at(j).isSpace())
					{
						onlyWhitespace = false;
						break;
					}
				}
				if (onlyWhitespace)
					insertPos = safeQSizeToInt(wrappedText.size());
			}

			wrappedText.insert(insertPos, QLatin1Char('\n'));
			recomputeLineState();
		}

		wrappedText.append(grapheme);
		if (isSingleCharGrapheme(grapheme, QLatin1Char(' ')))
			lastSpace = safeQSizeToInt(wrappedText.size()) - 1;
		if (!isWhitespaceGrapheme(grapheme))
			lineHasVisibleChar = true;
		column += graphemeWidth;
	}

	text = wrappedText;
}

void QMudOutputWrapUtils::wrapStyledLineForColumn(QString &text, QVector<WorldRuntime::StyleSpan> &spans,
                                                  const int wrapColumn, const bool indentParas,
                                                  const int firstLinePrefixColumns)
{
	if (wrapColumn <= 0 || text.isEmpty())
		return;

	constexpr auto                   kNoBoundary = qsizetype{-1};

	QVector<WorldRuntime::StyleSpan> expandedStyles;
	expandedStyles.reserve(text.size());
	WorldRuntime::StyleSpan lastStyle;
	int                     covered = 0;
	for (const WorldRuntime::StyleSpan &span : spans)
	{
		lastStyle     = span;
		const int len = qMax(0, span.length);
		for (int i = 0; i < len && covered < text.size(); ++i, ++covered)
			expandedStyles.push_back(span);
		if (covered >= text.size())
			break;
	}
	while (expandedStyles.size() < text.size())
		expandedStyles.push_back(lastStyle);

	QString wrappedText;
	wrappedText.reserve(safeQSizeToInt(text.size() + text.size() / qMax(1, wrapColumn)));
	QVector<WorldRuntime::StyleSpan> wrappedStyles;
	wrappedStyles.reserve(
	    safeQSizeToInt(expandedStyles.size() + expandedStyles.size() / qMax(1, wrapColumn)));

	int  column             = qMax(0, firstLinePrefixColumns);
	int  lineStart          = 0;
	int  lastSpace          = -1;
	bool lineHasVisibleChar = column > 0;

	auto recomputeLineState = [&]
	{
		column             = 0;
		lineStart          = 0;
		lastSpace          = -1;
		lineHasVisibleChar = false;
		for (int i = safeQSizeToInt(wrappedText.size()) - 1; i >= 0; --i)
		{
			if (wrappedText.at(i) == QLatin1Char('\n'))
			{
				lineStart = i + 1;
				break;
			}
		}
		QTextBoundaryFinder boundary(QTextBoundaryFinder::Grapheme, wrappedText);
		boundary.setPosition(lineStart);
		const QStringView wrappedView{wrappedText};
		for (qsizetype graphemeStart                   = static_cast<qsizetype>(lineStart),
		               graphemeEnd                     = boundary.toNextBoundary();
		     graphemeEnd != kNoBoundary; graphemeStart = graphemeEnd, graphemeEnd = boundary.toNextBoundary())
		{
			const QStringView grapheme = wrappedView.sliced(graphemeStart, graphemeEnd - graphemeStart);
			if (isSingleCharGrapheme(grapheme, QLatin1Char('\n')))
			{
				lineStart          = boundedQSizeToInt(graphemeEnd);
				column             = 0;
				lastSpace          = -1;
				lineHasVisibleChar = false;
				continue;
			}
			if (isSingleCharGrapheme(grapheme, QLatin1Char(' ')))
				lastSpace = boundedQSizeToInt(graphemeEnd - 1);
			if (!isWhitespaceGrapheme(grapheme))
				lineHasVisibleChar = true;
			column += graphemeColumnWidthForWrap(grapheme);
		}
	};

	QTextBoundaryFinder boundary(QTextBoundaryFinder::Grapheme, text);
	const QStringView   textView{text};
	for (qsizetype graphemeStart = 0, graphemeEnd = boundary.toNextBoundary(); graphemeEnd != kNoBoundary;
	     graphemeStart = graphemeEnd, graphemeEnd = boundary.toNextBoundary())
	{
		const QStringView             grapheme = textView.sliced(graphemeStart, graphemeEnd - graphemeStart);
		const WorldRuntime::StyleSpan style    = expandedStyles.value(boundedQSizeToInt(graphemeEnd - 1));

		if (isSingleCharGrapheme(grapheme, QLatin1Char('\n')))
		{
			wrappedText.append(grapheme);
			for (qsizetype i = graphemeStart; i < graphemeEnd; ++i)
				wrappedStyles.push_back(expandedStyles.value(boundedQSizeToInt(i)));
			column             = 0;
			lineStart          = safeQSizeToInt(wrappedText.size());
			lastSpace          = -1;
			lineHasVisibleChar = false;
			continue;
		}

		const int graphemeWidth = graphemeColumnWidthForWrap(grapheme);
		if (lineHasVisibleChar && graphemeWidth > 0 && column + graphemeWidth > wrapColumn)
		{
			int insertPos = safeQSizeToInt(wrappedText.size());
			if (lastSpace >= lineStart)
			{
				insertPos = indentParas ? lastSpace : lastSpace + 1;

				bool onlyWhitespace = true;
				for (int j = lineStart, size = safeQSizeToInt(wrappedText.size()); j < insertPos && j < size;
				     ++j)
				{
					if (!wrappedText.at(j).isSpace())
					{
						onlyWhitespace = false;
						break;
					}
				}
				if (onlyWhitespace)
					insertPos = safeQSizeToInt(wrappedText.size());
			}

			const WorldRuntime::StyleSpan newlineStyle = insertPos > 0 && insertPos - 1 < wrappedStyles.size()
			                                                 ? wrappedStyles.at(insertPos - 1)
			                                                 : style;
			wrappedText.insert(insertPos, QLatin1Char('\n'));
			wrappedStyles.insert(insertPos, newlineStyle);
			recomputeLineState();
		}

		wrappedText.append(grapheme);
		for (qsizetype i = graphemeStart; i < graphemeEnd; ++i)
			wrappedStyles.push_back(expandedStyles.value(boundedQSizeToInt(i)));
		if (isSingleCharGrapheme(grapheme, QLatin1Char(' ')))
			lastSpace = safeQSizeToInt(wrappedText.size()) - 1;
		if (!isWhitespaceGrapheme(grapheme))
			lineHasVisibleChar = true;
		column += graphemeWidth;
	}

	QVector<WorldRuntime::StyleSpan> rebuilt;
	rebuilt.reserve(wrappedStyles.size());
	for (const WorldRuntime::StyleSpan &oneStyle : wrappedStyles)
	{
		WorldRuntime::StyleSpan span = oneStyle;
		span.length                  = 1;
		if (!rebuilt.isEmpty() && sameStyleForWrap(rebuilt.last(), span))
			rebuilt.last().length++;
		else
			rebuilt.push_back(span);
	}

	text  = wrappedText;
	spans = rebuilt;
}

QVector<QMudOutputWrapUtils::OutputLineSegment> QMudOutputWrapUtils::splitOutputTextAtLineBreaks(
    const QString &text, const QVector<WorldRuntime::StyleSpan> &spans, const bool finalHardReturn)
{
	QVector<OutputLineSegment> segments;
	if (text.isEmpty())
	{
		if (finalHardReturn)
			segments.push_back(OutputLineSegment{QString(), {}, true});
		return segments;
	}
	const int textSize = safeQSizeToInt(text.size());

	auto      appendSegment = [&](const int start, const int end, const bool hardReturn)
	{
		OutputLineSegment segment;
		const int         length = qMax(0, end - start);
		segment.text             = text.mid(start, length);
		segment.spans            = slicedStyleSpans(spans, start, length);
		segment.hardReturn       = hardReturn;
		segments.push_back(std::move(segment));
	};

	int segmentStart = 0;
	int index        = 0;
	while (index < textSize)
	{
		const QChar ch = text.at(index);
		if (ch != QLatin1Char('\r') && ch != QLatin1Char('\n'))
		{
			++index;
			continue;
		}

		appendSegment(segmentStart, index, true);
		if (ch == QLatin1Char('\r') && index + 1 < textSize && text.at(index + 1) == QLatin1Char('\n'))
			index += 2;
		else
			++index;
		segmentStart = index;
	}

	if (segmentStart < textSize)
		appendSegment(segmentStart, textSize, finalHardReturn);

	return segments;
}
