/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: AccessibleTextUtils.h
 * Role: Accessible text offset mapping helpers for native world output.
 */

#ifndef QMUD_ACCESSIBLETEXTUTILS_H
#define QMUD_ACCESSIBLETEXTUTILS_H

// ReSharper disable once CppUnusedIncludeDirective
#include <QString>
#include <QVector>

/**
 * @brief Helpers for exposing native output text through Qt accessibility APIs.
 */
namespace QMudAccessibleTextUtils
{
	/**
	 * @brief Accessible text boundary granularity.
	 */
	enum class TextBoundaryKind
	{
		Character, ///< Single UTF-16 code unit.
		Word,      ///< Word boundary.
		Sentence,  ///< Sentence boundary.
		Paragraph, ///< Logical output paragraph.
		Line,      ///< Logical output line.
		WholeText  ///< Entire accessible text.
	};

	/**
	 * @brief Accessible text boundary query direction.
	 */
	enum class TextBoundaryQuery
	{
		Before, ///< Boundary before the supplied offset.
		At,     ///< Boundary containing the supplied offset.
		After   ///< Boundary after the supplied offset.
	};

	/**
	 * @brief Text and offsets returned for an accessible boundary query.
	 */
	struct TextBoundaryResult
	{
			QString text;           ///< Text covered by the boundary.
			int     startOffset{0}; ///< Inclusive start offset.
			int     endOffset{0};   ///< Exclusive end offset.
	};

	/**
	 * @brief Line and column position in the accessible text model.
	 */
	struct TextPosition
	{
			int line{0};   ///< Zero-based line index.
			int column{0}; ///< Zero-based UTF-16 column within the line.
	};

	/**
	 * @brief Maps newline-joined logical output lines to linear accessible text offsets.
	 */
	class LineOffsetMap final
	{
		public:
			/**
			 * @brief Creates an empty offset map.
			 */
			LineOffsetMap() = default;
			/**
			 * @brief Creates an offset map for the supplied logical lines.
			 * @param lines Output lines without implicit trailing newline.
			 */
			explicit LineOffsetMap(QVector<QString> lines);

			/**
			 * @brief Replaces the mapped output lines.
			 * @param lines Output lines without implicit trailing newline.
			 */
			void                             reset(QVector<QString> lines);

			/**
			 * @brief Returns the number of mapped logical lines.
			 * @return Line count clamped to the accessibility API integer range.
			 */
			[[nodiscard]] int                lineCount() const;
			/**
			 * @brief Returns the total accessible character count.
			 * @return UTF-16 code-unit count including inserted newline separators.
			 */
			[[nodiscard]] int                characterCount() const;
			/**
			 * @brief Returns whether the accessible text is empty.
			 * @return True when there are no accessible characters.
			 */
			[[nodiscard]] bool               isEmpty() const;

			/**
			 * @brief Converts a line position to a linear accessible offset.
			 * @param position Line and column position to convert.
			 * @return Clamped linear offset.
			 */
			[[nodiscard]] int                offsetForPosition(TextPosition position) const;
			/**
			 * @brief Converts a linear accessible offset to a line position.
			 * @param offset Linear offset to convert.
			 * @return Clamped line and column position.
			 */
			[[nodiscard]] TextPosition       positionForOffset(int offset) const;
			/**
			 * @brief Extracts accessible text in a linear offset range.
			 * @param startOffset Inclusive start offset.
			 * @param endOffset Exclusive end offset.
			 * @return Text slice with logical line separators inserted as newlines.
			 */
			[[nodiscard]] QString            text(int startOffset, int endOffset) const;
			/**
			 * @brief Extracts text between two line positions.
			 * @param anchor First selection endpoint.
			 * @param cursor Second selection endpoint.
			 * @return Selected text with logical line separators inserted as newlines.
			 */
			[[nodiscard]] QString            selectedText(TextPosition anchor, TextPosition cursor) const;
			/**
			 * @brief Returns text and offsets for an accessible text boundary query.
			 * @param offset Linear text offset.
			 * @param boundary Boundary granularity to query.
			 * @param query Boundary relation to the supplied offset.
			 * @return Boundary text and inclusive/exclusive offsets.
			 */
			[[nodiscard]] TextBoundaryResult boundaryText(int offset, TextBoundaryKind boundary,
			                                              TextBoundaryQuery query) const;

		private:
			QVector<QString>                 m_lines;
			QVector<int>                     m_lineStarts;
			int                              m_characterCount{0};

			void                             rebuildPrefixes();
			[[nodiscard]] int                clampedLineLength(int line) const;
			[[nodiscard]] TextBoundaryResult lineBoundaryText(int offset, TextBoundaryQuery query) const;
			[[nodiscard]] TextBoundaryResult characterBoundaryText(int offset, TextBoundaryQuery query) const;
			[[nodiscard]] TextBoundaryResult wholeTextBoundaryText(TextBoundaryQuery query) const;
			[[nodiscard]] TextBoundaryResult qtBoundaryText(int offset, TextBoundaryKind boundary,
			                                                TextBoundaryQuery query) const;
	};
} // namespace QMudAccessibleTextUtils

#endif // QMUD_ACCESSIBLETEXTUTILS_H
