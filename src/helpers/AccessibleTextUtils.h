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

#include <deque>
#include <functional>

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
	 * @brief Incremental line-length index for newline-joined accessible text.
	 *
	 * The index stores numeric lengths only. Text remains owned by the caller's render model.
	 */
	class LineOffsetIndex final
	{
		public:
			using LineTextAccessor = std::function<const QString &(int)>;

			/**
			 * @brief Removes all indexed lines.
			 */
			void               clear();
			/**
			 * @brief Rebuilds the index from line lengths.
			 * @param lineLengths UTF-16 lengths without implicit separators.
			 */
			void               reset(const QVector<int> &lineLengths);
			/**
			 * @brief Replaces one logical line range.
			 * @param firstLine First line to replace.
			 * @param removeCount Number of existing lines to remove.
			 * @param insertedLengths Lengths of replacement lines.
			 * @return `true` when the requested range was valid and applied.
			 */
			[[nodiscard]] bool replace(int firstLine, int removeCount, const QVector<int> &insertedLengths);
			/**
			 * @brief Returns the indexed line count.
			 * @return Number of logical lines.
			 */
			[[nodiscard]] int  lineCount() const;
			/**
			 * @brief Returns the accessible character count.
			 * @return UTF-16 count including separators, clamped to the Qt accessibility range.
			 */
			[[nodiscard]] int  characterCount() const;
			/**
			 * @brief Returns whether the indexed text is empty.
			 * @return `true` when there are no accessible characters.
			 */
			[[nodiscard]] bool isEmpty() const;
			/**
			 * @brief Returns one indexed line length.
			 * @param line Zero-based line index.
			 * @return UTF-16 line length, or zero outside the index.
			 */
			[[nodiscard]] int  lineLength(int line) const;
			/**
			 * @brief Converts a line position to an accessible offset.
			 * @param position Line and column to convert.
			 * @return Clamped accessible offset.
			 */
			[[nodiscard]] int  offsetForPosition(TextPosition position) const;
			/**
			 * @brief Converts an accessible offset to a line position.
			 * @param offset Accessible offset to convert.
			 * @return Clamped line and column.
			 */
			[[nodiscard]] TextPosition       positionForOffset(int offset) const;
			/**
			 * @brief Extracts accessible text from caller-owned logical lines.
			 * @param startOffset Inclusive accessible offset.
			 * @param endOffset Exclusive accessible offset.
			 * @param lineText Returns the authoritative text for a valid indexed line.
			 * @return Requested newline-joined text range.
			 */
			[[nodiscard]] QString            text(int startOffset, int endOffset,
			                                      const LineTextAccessor &lineText) const;
			/**
			 * @brief Resolves a text boundary against caller-owned logical lines.
			 * @param offset Accessible offset to query.
			 * @param boundary Boundary granularity.
			 * @param query Boundary relation to @p offset.
			 * @param lineText Returns the authoritative text for a valid indexed line.
			 * @return Boundary text and offsets.
			 */
			[[nodiscard]] TextBoundaryResult boundaryText(int offset, TextBoundaryKind boundary,
			                                              TextBoundaryQuery       query,
			                                              const LineTextAccessor &lineText) const;

		private:
			static constexpr int kBlockCapacity = 512;
			struct Block
			{
					QVector<int> lengths;
					qint64       lengthSum{0};
			};
			/**
			 * @brief Transient position used while walking indexed lines without restarting block lookup.
			 */
			struct LineCursor
			{
					int blockIndex{-1};   ///< Index of the current length block.
					int indexInBlock{-1}; ///< Index of the current line within its block.
					int line{-1};         ///< Absolute zero-based line index.
					int startOffset{0};   ///< Accessible start offset of the current line.
			};

			std::deque<Block>          m_blocks;
			int                        m_lineCount{0};
			qint64                     m_lengthSum{0};

			/**
			 * @brief Appends one bounded line length.
			 * @param length UTF-16 line length.
			 */
			void                       appendLength(int length);
			/**
			 * @brief Removes line lengths from the front.
			 * @param count Number of lengths to remove.
			 */
			void                       removeFront(int count);
			/**
			 * @brief Removes line lengths from the back.
			 * @param count Number of lengths to remove.
			 */
			void                       removeBack(int count);
			/**
			 * @brief Replaces one existing line length in place.
			 * @param line Zero-based line index.
			 * @param length Replacement UTF-16 length.
			 * @return `true` when @p line was valid.
			 */
			[[nodiscard]] bool         setLength(int line, int length);
			/**
			 * @brief Locates one indexed line for sequential traversal.
			 * @param line Zero-based line index.
			 * @param startOffset Accessible start offset of @p line.
			 * @param cursor Receives the located line cursor.
			 * @return `true` when @p line was valid and located.
			 */
			[[nodiscard]] bool         initializeCursor(int line, int startOffset, LineCursor &cursor) const;
			/**
			 * @brief Returns the indexed length at a valid cursor.
			 * @param cursor Cursor whose line length is requested.
			 * @return Indexed UTF-16 line length, or zero for an invalid cursor.
			 */
			[[nodiscard]] int          cursorLength(const LineCursor &cursor) const;
			/**
			 * @brief Advances a cursor to the following indexed line.
			 * @param cursor Cursor to advance.
			 * @return `true` when a following line exists.
			 */
			[[nodiscard]] bool         advanceCursor(LineCursor &cursor) const;
			/**
			 * @brief Moves a cursor to the preceding indexed line.
			 * @param cursor Cursor to move backward.
			 * @return `true` when a preceding line exists.
			 */
			[[nodiscard]] bool         retreatCursor(LineCursor &cursor) const;
			/**
			 * @brief Flattens the numeric index for an uncommon general replacement.
			 * @return All indexed line lengths.
			 */
			[[nodiscard]] QVector<int> lengths() const;
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
			QVector<QString> m_lines;
			LineOffsetIndex  m_index;
	};
} // namespace QMudAccessibleTextUtils

#endif // QMUD_ACCESSIBLETEXTUTILS_H
