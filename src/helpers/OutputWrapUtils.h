/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: OutputWrapUtils.h
 * Role: Fixed-column output wrapping helpers shared by runtime, command processing, and tests.
 */

#ifndef QMUD_OUTPUTWRAPUTILS_H
#define QMUD_OUTPUTWRAPUTILS_H

#include "WorldRuntime.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QMap>
#include <QString>
// ReSharper disable once CppUnusedIncludeDirective
#include <QVector>

#include <functional>
#include <utility>

/**
 * @brief Shared helpers for QMud's own fixed-column output wrapping.
 */
namespace QMudOutputWrapUtils
{
	/**
	 * @brief Resolved fixed-column wrapping settings.
	 */
	struct FixedColumnWrapConfig
	{
			bool enabled{false};    ///< Whether fixed-column wrapping should run.
			int  wrapColumn{0};     ///< Column where wrapping should occur.
			bool indentParas{true}; ///< Whether wrapped paragraphs keep indentation.
	};

	/**
	 * @brief One hard-return-delimited output segment.
	 */
	struct OutputLineSegment
	{
			QString                          text;              ///< Segment text without CR/LF separators.
			QVector<WorldRuntime::StyleSpan> spans;             ///< Style spans for `text`.
			bool                             hardReturn{false}; ///< Whether this segment ends a hard line.
	};

	/**
	 * @brief Resolves local output wrapping from world settings.
	 * @param attrs World attribute map.
	 * @param nawsNegotiated Whether NAWS/server-side wrap is active.
	 * @param windowColumns Current output window column count.
	 * @return Resolved fixed-column wrapping configuration.
	 */
	[[nodiscard]] FixedColumnWrapConfig localOutputWrapConfig(const QMap<QString, QString> &attrs,
	                                                          bool nawsNegotiated, int windowColumns);

	/**
	 * @brief Returns the visible column width of the final hard-break segment.
	 * @param text Text to inspect.
	 * @return Column width after the last explicit newline.
	 */
	[[nodiscard]] int                   trailingLineColumnWidthForWrap(const QString &text);

	/**
	 * @brief Reports whether spans already form an unchanged exact presentation for text.
	 * @param text Presented text.
	 * @param spans Candidate spans.
	 * @return True only when every span is positive and their combined length is exactly text.size().
	 */
	[[nodiscard]] bool styleSpansCoverTextExactly(const QString                          &text,
	                                              const QVector<WorldRuntime::StyleSpan> &spans);

	/**
	 * @brief Clips style spans to text and fills any uncovered suffix with one fallback style.
	 * @param text Text the returned spans must cover exactly.
	 * @param spans Candidate spans.
	 * @param fallback Style used for an uncovered suffix.
	 * @return Positive-length spans whose combined length equals `text.size()`.
	 */
	[[nodiscard]] QVector<WorldRuntime::StyleSpan>
	normalizeStyleSpansForText(const QString &text, const QVector<WorldRuntime::StyleSpan> &spans,
	                           const WorldRuntime::StyleSpan &fallback);

	/**
	 * @brief Preserves exact spans without evaluating a potentially expensive fallback provider.
	 * @param text Text the returned spans must cover exactly.
	 * @param spans Candidate spans.
	 * @param fallbackProvider Callable producing the fallback style only when normalization is required.
	 * @return Unchanged exact spans, or normalized spans using the lazily produced fallback.
	 */
	template <typename FallbackProvider>
	[[nodiscard]] QVector<WorldRuntime::StyleSpan>
	normalizeStyleSpansForTextLazily(const QString &text, const QVector<WorldRuntime::StyleSpan> &spans,
	                                 FallbackProvider &&fallbackProvider)
	{
		if (styleSpansCoverTextExactly(text, spans))
			return spans;
		return normalizeStyleSpansForText(text, spans,
		                                  std::invoke(std::forward<FallbackProvider>(fallbackProvider)));
	}

	/**
	 * @brief Applies fixed-column wrapping to plain text.
	 * @param text Text to mutate in place.
	 * @param wrapColumn Target wrap column.
	 * @param indentParas Preserve indentation when wrapping at whitespace.
	 * @param firstLinePrefixColumns Existing columns before `text` on the first row.
	 * @param firstLinePrefixEndsWithSpace Whether `text` starts at a valid wrap boundary in the prefix.
	 */
	void wrapPlainLineForColumn(QString &text, int wrapColumn, bool indentParas,
	                            int firstLinePrefixColumns = 0, bool firstLinePrefixEndsWithSpace = false);

	/**
	 * @brief Applies fixed-column wrapping to styled text.
	 * @param text Text to mutate in place.
	 * @param spans Style spans to mutate with inserted hard breaks.
	 * @param wrapColumn Target wrap column.
	 * @param indentParas Preserve indentation when wrapping at whitespace.
	 * @param firstLinePrefixColumns Existing columns before `text` on the first row.
	 * @param firstLinePrefixEndsWithSpace Whether `text` starts at a valid wrap boundary in the prefix.
	 */
	void wrapStyledLineForColumn(QString &text, QVector<WorldRuntime::StyleSpan> &spans, int wrapColumn,
	                             bool indentParas, int firstLinePrefixColumns = 0,
	                             bool firstLinePrefixEndsWithSpace = false);

	/**
	 * @brief Splits output text into hard-return-delimited runtime line segments.
	 * @param text Text to split.
	 * @param spans Style spans covering `text`.
	 * @param finalHardReturn Whether the final segment should end a hard line.
	 * @return Text segments with CR, LF, and CRLF represented as hard-return boundaries.
	 */
	[[nodiscard]] QVector<OutputLineSegment>
	splitOutputTextAtLineBreaks(const QString &text, const QVector<WorldRuntime::StyleSpan> &spans,
	                            bool finalHardReturn);
} // namespace QMudOutputWrapUtils

#endif // QMUD_OUTPUTWRAPUTILS_H
