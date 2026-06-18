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
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_IndexedRingBuffer)

#if __has_include("tst_IndexedRingBuffer.moc")
#include "tst_IndexedRingBuffer.moc"
#endif
