/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_IndexedRingBuffer.cpp
 * Role: QTest coverage for IndexedRingBuffer logical indexing and head-trim behavior.
 */

#include "IndexedRingBuffer.h"

#include <QtTest/QTest>

#include <algorithm>
#include <ranges>

static_assert(std::random_access_iterator<IndexedRingBuffer<int>::iterator>);
static_assert(std::random_access_iterator<IndexedRingBuffer<int>::const_iterator>);
static_assert(std::ranges::random_access_range<IndexedRingBuffer<int>>);
static_assert(std::ranges::random_access_range<const IndexedRingBuffer<int>>);

namespace
{
	/**
	 * @brief QTest fixture covering IndexedRingBuffer container behavior.
	 */
	class tst_IndexedRingBuffer : public QObject
	{
			Q_OBJECT

			// NOLINTBEGIN(readability-convert-member-functions-to-static)
		private slots:
			void headRemovalPreservesLogicalOrder()
			{
				IndexedRingBuffer<int> buffer;
				for (int value = 1; value <= 6; ++value)
					buffer.push_back(value);

				buffer.remove(0, 3);
				buffer.push_back(7);
				buffer.push_back(8);

				QCOMPARE(buffer.size(), 5);
				QCOMPARE(buffer.at(0), 4);
				QCOMPARE(buffer.at(1), 5);
				QCOMPARE(buffer.at(2), 6);
				QCOMPARE(buffer.at(3), 7);
				QCOMPARE(buffer.at(4), 8);
				QCOMPARE(buffer.toVector(), QVector<int>({4, 5, 6, 7, 8}));
			}

			void resizeAfterHeadRemovalKeepsIndexingContiguous()
			{
				IndexedRingBuffer<int> buffer(QVector<int>({10, 20, 30, 40, 50}));

				buffer.removeFirst();
				buffer.removeFirst();
				buffer.resize(5);
				buffer[3] = 60;
				buffer[4] = 70;

				QCOMPARE(buffer.toVector(), QVector<int>({30, 40, 50, 60, 70}));
			}

			void middleInsertAndEraseMaintainLogicalSequence()
			{
				IndexedRingBuffer<int> buffer(QVector<int>({1, 2, 4, 5}));
				buffer.removeFirst();
				buffer.insert(1, 3);
				buffer.erase(buffer.begin() + 2);

				QCOMPARE(buffer.toVector(), QVector<int>({2, 3, 5}));
			}

			void wrappedInsertNearFrontShiftsPrefixOnly()
			{
				IndexedRingBuffer<int> buffer(QVector<int>({1, 2, 4, 5, 6, 7}));
				buffer.remove(0, 2);
				buffer.push_back(8);
				buffer.push_back(9);

				buffer.insert(1, 3);

				QCOMPARE(buffer.toVector(), QVector<int>({4, 3, 5, 6, 7, 8, 9}));
			}

			void wrappedInsertNearBackShiftsSuffixOnly()
			{
				IndexedRingBuffer<int> buffer(QVector<int>({1, 2, 3, 4, 6, 7}));
				buffer.remove(0, 2);
				buffer.push_back(8);
				buffer.push_back(9);

				buffer.insert(3, 5);

				QCOMPARE(buffer.toVector(), QVector<int>({3, 4, 6, 5, 7, 8, 9}));
			}

			void wrappedRemoveNearFrontShiftsPrefixOnly()
			{
				IndexedRingBuffer<int> buffer(QVector<int>({1, 2, 3, 4, 5, 6, 7}));
				buffer.remove(0, 2);
				buffer.push_back(8);
				buffer.push_back(9);

				buffer.remove(2, 2);

				QCOMPARE(buffer.toVector(), QVector<int>({3, 4, 7, 8, 9}));
			}

			void wrappedRemoveNearBackShiftsSuffixOnly()
			{
				IndexedRingBuffer<int> buffer(QVector<int>({1, 2, 3, 4, 5, 6, 7}));
				buffer.remove(0, 2);
				buffer.push_back(8);
				buffer.push_back(9);

				buffer.remove(3, 2);

				QCOMPARE(buffer.toVector(), QVector<int>({3, 4, 5, 8, 9}));
			}

			void wrappedReplaceGrowthNearFrontShiftsPrefixOnce()
			{
				IndexedRingBuffer<int> buffer(QVector<int>({1, 2, 3, 4, 8, 9}));
				buffer.remove(0, 2);
				buffer.push_back(10);
				buffer.push_back(11);

				buffer.replace({1, 1, 3}, 7);

				QCOMPARE(buffer.toVector(), QVector<int>({3, 7, 7, 7, 8, 9, 10, 11}));
			}

			void wrappedReplaceGrowthNearBackShiftsSuffixOnce()
			{
				IndexedRingBuffer<int> buffer(QVector<int>({1, 2, 3, 4, 5, 9}));
				buffer.remove(0, 2);
				buffer.push_back(10);
				buffer.push_back(11);

				buffer.replace({3, 1, 3}, 8);

				QCOMPARE(buffer.toVector(), QVector<int>({3, 4, 5, 8, 8, 8, 10, 11}));
			}

			void wrappedReplaceShrinkNearFrontShiftsPrefixOnce()
			{
				IndexedRingBuffer<int> buffer(QVector<int>({1, 2, 3, 4, 5, 6, 7, 8}));
				buffer.remove(0, 2);
				buffer.push_back(9);
				buffer.push_back(10);

				buffer.replace({1, 3, 1}, 11);

				QCOMPARE(buffer.toVector(), QVector<int>({3, 11, 7, 8, 9, 10}));
			}

			void wrappedReplaceShrinkNearBackShiftsSuffixOnce()
			{
				IndexedRingBuffer<int> buffer(QVector<int>({1, 2, 3, 4, 5, 6, 7, 8}));
				buffer.remove(0, 2);
				buffer.push_back(9);
				buffer.push_back(10);

				buffer.replace({4, 3, 1}, 11);

				QCOMPARE(buffer.toVector(), QVector<int>({3, 4, 5, 6, 11, 10}));
			}

			void replaceInsertOnlyAtFront()
			{
				IndexedRingBuffer<int> buffer(QVector<int>({1, 2, 3, 4}));
				buffer.remove(0, 2);
				buffer.push_back(5);
				buffer.push_back(6);

				buffer.replace({0, 0, 2}, 9);

				QCOMPARE(buffer.toVector(), QVector<int>({9, 9, 3, 4, 5, 6}));
			}

			void replaceAppendOnlyAtBack()
			{
				IndexedRingBuffer<int> buffer(QVector<int>({1, 2, 3, 4}));
				buffer.remove(0, 2);
				buffer.push_back(5);
				buffer.push_back(6);

				buffer.replace({buffer.size(), 0, 2}, 9);

				QCOMPARE(buffer.toVector(), QVector<int>({3, 4, 5, 6, 9, 9}));
			}

			void replaceEqualSizeKeepsUnchangedSides()
			{
				IndexedRingBuffer<int> buffer(QVector<int>({1, 2, 3, 4, 5, 6}));
				buffer.remove(0, 2);
				buffer.push_back(7);
				buffer.push_back(8);

				buffer.replace({2, 2, 2}, 9);

				QCOMPARE(buffer.toVector(), QVector<int>({3, 4, 9, 9, 7, 8}));
			}

			void replaceRemoveOnlyInMiddle()
			{
				IndexedRingBuffer<int> buffer(QVector<int>({1, 2, 3, 4, 5, 6, 7}));
				buffer.remove(0, 2);
				buffer.push_back(8);
				buffer.push_back(9);

				buffer.replace({2, 3, 0}, 0);

				QCOMPARE(buffer.toVector(), QVector<int>({3, 4, 8, 9}));
			}

			void iteratorsSupportAlgorithmsInLogicalOrder()
			{
				IndexedRingBuffer<int> buffer(QVector<int>({1, 3, 5, 7, 9}));
				buffer.remove(0, 2);
				buffer.push_back(11);
				buffer.push_back(13);

				const IndexedRingBuffer<int> &constBuffer = buffer;
				const auto                    found       = std::ranges::upper_bound(constBuffer, 10);
				QVERIFY(found != constBuffer.cend());
				QCOMPARE(*found, 11);

				std::ranges::fill(buffer, 4);
				QCOMPARE(buffer.toVector(), QVector<int>({4, 4, 4, 4, 4}));
			}

			void indexedFindTraversesWrappedStorageInLogicalOrder()
			{
				IndexedRingBuffer<int> buffer(QVector<int>({1, 2, 3, 4, 5, 6}));
				buffer.remove(0, 4);
				buffer.push_back(7);
				buffer.push_back(8);
				buffer.push_back(9);
				buffer.push_back(10);

				QVector<QPair<int, qsizetype>> visited;
				const qsizetype                matchedIndex = buffer.findIndexIf(
				    [&visited](const int value, const qsizetype logicalIndex)
				    {
					    visited.push_back({value, logicalIndex});
					    return value == 9;
				    });

				const QVector<QPair<int, qsizetype>> expectedVisited{
				    {5, 0},
                    {6, 1},
                    {7, 2},
                    {8, 3},
                    {9, 4}
                };
				QCOMPARE(matchedIndex, qsizetype{4});
				QCOMPARE(visited, expectedVisited);
				QCOMPARE(buffer.findIndexIf([](const int value, const qsizetype) { return value == 99; }),
				         qsizetype{-1});
			}
			// NOLINTEND(readability-convert-member-functions-to-static)
	};
} // namespace

QTEST_APPLESS_MAIN(tst_IndexedRingBuffer)

#if __has_include("tst_IndexedRingBuffer.moc")
#include "tst_IndexedRingBuffer.moc"
#endif
