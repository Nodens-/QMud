/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_AccessibleTextUtils.cpp
 * Role: QTest coverage for accessible text offset mapping helpers.
 */

#include "AccessibleTextUtils.h"

#include <QtTest/QTest>

using QMudAccessibleTextUtils::LineOffsetMap;
using QMudAccessibleTextUtils::TextBoundaryKind;
using QMudAccessibleTextUtils::TextBoundaryQuery;
using QMudAccessibleTextUtils::TextPosition;

/**
 * @brief QTest fixture covering native output accessible text mapping.
 */
class tst_AccessibleTextUtils : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void emptyOutputHasNoAccessibleText()
		{
			const LineOffsetMap map;

			QVERIFY(map.isEmpty());
			QCOMPARE(map.lineCount(), 0);
			QCOMPARE(map.characterCount(), 0);
			QCOMPARE(map.text(0, 10), QString());
			QCOMPARE(map.offsetForPosition({3, 7}), 0);

			const TextPosition position = map.positionForOffset(5);
			QCOMPARE(position.line, 0);
			QCOMPARE(position.column, 0);
		}

		void oneLineMapsOffsetsWithoutTrailingNewline()
		{
			const LineOffsetMap map({QStringLiteral("prompt> look")});

			QVERIFY(!map.isEmpty());
			QCOMPARE(map.lineCount(), 1);
			QCOMPARE(map.characterCount(), 12);
			QCOMPARE(map.text(0, map.characterCount()), QStringLiteral("prompt> look"));
			QCOMPARE(map.offsetForPosition({0, 8}), 8);

			const TextPosition position = map.positionForOffset(8);
			QCOMPARE(position.line, 0);
			QCOMPARE(position.column, 8);
		}

		void multipleLinesUseNewlineSeparators()
		{
			const LineOffsetMap map(
			    {QStringLiteral("alpha"), QStringLiteral("beta"), QStringLiteral("gamma")});

			QCOMPARE(map.characterCount(), 16);
			QCOMPARE(map.text(0, map.characterCount()), QStringLiteral("alpha\nbeta\ngamma"));
			QCOMPARE(map.offsetForPosition({1, 0}), 6);
			QCOMPARE(map.offsetForPosition({2, 2}), 13);

			const TextPosition newlinePosition = map.positionForOffset(5);
			QCOMPARE(newlinePosition.line, 0);
			QCOMPARE(newlinePosition.column, 5);
		}

		void slicesCanStartAndEndInsideDifferentLines()
		{
			const LineOffsetMap map(
			    {QStringLiteral("alpha"), QStringLiteral("beta"), QStringLiteral("gamma")});

			QCOMPARE(map.text(2, 12), QStringLiteral("pha\nbeta\ng"));
			QCOMPARE(map.text(5, 6), QStringLiteral("\n"));
			QCOMPARE(map.text(6, 10), QStringLiteral("beta"));
		}

		void positionsAndOffsetsClampToCurrentBuffer()
		{
			const LineOffsetMap map({QStringLiteral("one"), QStringLiteral("two")});

			QCOMPARE(map.offsetForPosition({-2, -5}), 0);
			QCOMPARE(map.offsetForPosition({0, 99}), 3);
			QCOMPARE(map.offsetForPosition({7, 1}), map.characterCount());

			TextPosition position = map.positionForOffset(-10);
			QCOMPARE(position.line, 0);
			QCOMPARE(position.column, 0);

			position = map.positionForOffset(999);
			QCOMPARE(position.line, 1);
			QCOMPARE(position.column, 3);
		}

		void resetModelsCappedBufferTrim()
		{
			LineOffsetMap map({QStringLiteral("old1"), QStringLiteral("old2"), QStringLiteral("new")});
			QCOMPARE(map.text(0, map.characterCount()), QStringLiteral("old1\nold2\nnew"));

			map.reset({QStringLiteral("old2"), QStringLiteral("new"), QStringLiteral("tail")});

			QCOMPARE(map.characterCount(), 13);
			QCOMPARE(map.text(0, map.characterCount()), QStringLiteral("old2\nnew\ntail"));
			QCOMPARE(map.offsetForPosition({1, 0}), 5);
		}

		void selectionTextUsesEitherEndpointOrder()
		{
			const LineOffsetMap map(
			    {QStringLiteral("north"), QStringLiteral("east"), QStringLiteral("south")});

			constexpr TextPosition anchor{0, 2};
			constexpr TextPosition cursor{2, 3};

			QCOMPARE(map.selectedText(anchor, cursor), QStringLiteral("rth\neast\nsou"));
			QCOMPARE(map.selectedText(cursor, anchor), QStringLiteral("rth\neast\nsou"));
			QCOMPARE(map.selectedText({1, 1}, {1, 1}), QString());
		}

		void lineBoundaryQueriesUseLogicalOutputLines()
		{
			const LineOffsetMap map(
			    {QStringLiteral("north"), QStringLiteral("east"), QStringLiteral("south")});

			const QMudAccessibleTextUtils::TextBoundaryResult at =
			    map.boundaryText(7, TextBoundaryKind::Line, TextBoundaryQuery::At);
			QCOMPARE(at.text, QStringLiteral("east"));
			QCOMPARE(at.startOffset, 6);
			QCOMPARE(at.endOffset, 10);

			const QMudAccessibleTextUtils::TextBoundaryResult before =
			    map.boundaryText(7, TextBoundaryKind::Line, TextBoundaryQuery::Before);
			QCOMPARE(before.text, QStringLiteral("north"));
			QCOMPARE(before.startOffset, 0);
			QCOMPARE(before.endOffset, 5);

			const QMudAccessibleTextUtils::TextBoundaryResult after =
			    map.boundaryText(7, TextBoundaryKind::Line, TextBoundaryQuery::After);
			QCOMPARE(after.text, QStringLiteral("south"));
			QCOMPARE(after.startOffset, 11);
			QCOMPARE(after.endOffset, 16);
		}

		void lineBoundaryQueriesClampAtBufferEdges()
		{
			const LineOffsetMap map({QStringLiteral("north"), QStringLiteral("east")});

			const QMudAccessibleTextUtils::TextBoundaryResult beforeStart =
			    map.boundaryText(0, TextBoundaryKind::Line, TextBoundaryQuery::Before);
			QCOMPARE(beforeStart.text, QString());
			QCOMPARE(beforeStart.startOffset, 0);
			QCOMPARE(beforeStart.endOffset, 0);

			const QMudAccessibleTextUtils::TextBoundaryResult afterEnd =
			    map.boundaryText(map.characterCount(), TextBoundaryKind::Line, TextBoundaryQuery::After);
			QCOMPARE(afterEnd.text, QString());
			QCOMPARE(afterEnd.startOffset, map.characterCount());
			QCOMPARE(afterEnd.endOffset, map.characterCount());
		}

		void characterBoundaryQueriesExposeLineSeparators()
		{
			const LineOffsetMap                               map({QStringLiteral("a"), QStringLiteral("b")});

			const QMudAccessibleTextUtils::TextBoundaryResult atFirst =
			    map.boundaryText(0, TextBoundaryKind::Character, TextBoundaryQuery::At);
			QCOMPARE(atFirst.text, QStringLiteral("a"));
			QCOMPARE(atFirst.startOffset, 0);
			QCOMPARE(atFirst.endOffset, 1);

			const QMudAccessibleTextUtils::TextBoundaryResult afterFirst =
			    map.boundaryText(0, TextBoundaryKind::Character, TextBoundaryQuery::After);
			QCOMPARE(afterFirst.text, QStringLiteral("\n"));
			QCOMPARE(afterFirst.startOffset, 1);
			QCOMPARE(afterFirst.endOffset, 2);

			const QMudAccessibleTextUtils::TextBoundaryResult afterSeparator =
			    map.boundaryText(1, TextBoundaryKind::Character, TextBoundaryQuery::After);
			QCOMPARE(afterSeparator.text, QStringLiteral("b"));
			QCOMPARE(afterSeparator.startOffset, 2);
			QCOMPARE(afterSeparator.endOffset, 3);
		}

		void wordBoundaryQueriesSkipBlankLines()
		{
			const LineOffsetMap map(
			    {QStringLiteral("alpha beta"), QStringLiteral("   "), QStringLiteral("gamma delta")});

			const int                                         betaStart = map.offsetForPosition({0, 6});
			const QMudAccessibleTextUtils::TextBoundaryResult at =
			    map.boundaryText(betaStart, TextBoundaryKind::Word, TextBoundaryQuery::At);
			QCOMPARE(at.text, QStringLiteral("beta"));
			QCOMPARE(at.startOffset, betaStart);
			QCOMPARE(at.endOffset, map.offsetForPosition({0, 10}));

			const QMudAccessibleTextUtils::TextBoundaryResult before =
			    map.boundaryText(betaStart, TextBoundaryKind::Word, TextBoundaryQuery::Before);
			QCOMPARE(before.text, QStringLiteral("alpha"));
			QCOMPARE(before.startOffset, 0);
			QCOMPARE(before.endOffset, 5);

			const QMudAccessibleTextUtils::TextBoundaryResult after = map.boundaryText(
			    map.offsetForPosition({0, 10}), TextBoundaryKind::Word, TextBoundaryQuery::After);
			QCOMPARE(after.text, QStringLiteral("gamma"));
			QCOMPARE(after.startOffset, map.offsetForPosition({2, 0}));
			QCOMPARE(after.endOffset, map.offsetForPosition({2, 5}));
		}

		void sentenceBoundaryQueriesUseTargetedText()
		{
			const LineOffsetMap map({QStringLiteral("One."), QStringLiteral("   "), QStringLiteral("Two.")});

			const QMudAccessibleTextUtils::TextBoundaryResult at =
			    map.boundaryText(0, TextBoundaryKind::Sentence, TextBoundaryQuery::At);
			QCOMPARE(at.text.trimmed(), QStringLiteral("One."));
			QCOMPARE(at.startOffset, 0);
			QVERIFY(at.endOffset > at.startOffset);

			const QMudAccessibleTextUtils::TextBoundaryResult after =
			    map.boundaryText(0, TextBoundaryKind::Sentence, TextBoundaryQuery::After);
			QCOMPARE(after.text.trimmed(), QStringLiteral("Two."));
			QVERIFY(after.startOffset > at.startOffset);
			QVERIFY(after.endOffset > after.startOffset);

			const QMudAccessibleTextUtils::TextBoundaryResult final = map.boundaryText(
			    map.offsetForPosition({2, 0}), TextBoundaryKind::Sentence, TextBoundaryQuery::At);
			QCOMPARE(final.text.trimmed(), QStringLiteral("Two."));
			QCOMPARE(final.startOffset, map.offsetForPosition({2, 0}));
			QCOMPARE(final.endOffset, map.offsetForPosition({2, 4}));
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_AccessibleTextUtils)

#if __has_include("tst_AccessibleTextUtils.moc")
#include "tst_AccessibleTextUtils.moc"
#endif
