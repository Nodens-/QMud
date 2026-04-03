/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_MemoryImageDecodeCacheUtils.cpp
 * Role: Unit coverage for bounded decoded-image cache helpers used by WindowLoadImageMemory.
 */

#include "MemoryImageDecodeCacheUtils.h"

#include <QtTest/QTest>

/**
 * @brief QTest fixture for memory-image decode cache helper behavior.
 */
class tst_MemoryImageDecodeCacheUtils : public QObject
{
		Q_OBJECT

	private slots:
		/**
		 * @brief Verifies empty payload fingerprint is zero.
		 */
		static void fingerprintZeroForEmptyPayload()
		{
			QCOMPARE(qmudMemoryImageDecodeFingerprint(QByteArray()), static_cast<quint64>(0));
		}

		/**
		 * @brief Verifies fingerprint stays stable for identical payloads.
		 */
		static void fingerprintStableForSamePayload()
		{
			const QByteArray data("png-bytes-123");
			const quint64    a = qmudMemoryImageDecodeFingerprint(data);
			const quint64    b = qmudMemoryImageDecodeFingerprint(data);
			QCOMPARE(a, b);
			QVERIFY(a != 0);
		}

		/**
		 * @brief Verifies cache hit returns decoded payload and promotes entry to front.
		 */
		static void lookupReturnsCachedImageAndPromotesMru()
		{
			QVector<QMudMemoryImageDecodeCacheEntry> cache;
			qint64                                   cacheBytes = 0;
			const QImage                             imageA(4, 4, QImage::Format_ARGB32);
			const QImage                             imageB(8, 8, QImage::Format_ARGB32);

			qmudInsertMemoryImageDecodeCache(cache, cacheBytes, QByteArray("A"), imageA, true, false, 16,
			                                 16 * 1024 * 1024);
			qmudInsertMemoryImageDecodeCache(cache, cacheBytes, QByteArray("B"), imageB, false, true, 16,
			                                 16 * 1024 * 1024);
			QCOMPARE(cache.size(), 2);
			QCOMPARE(cache.constFirst().encodedData, QByteArray("B"));

			QImage out;
			bool   outAlpha      = false;
			bool   outMonochrome = false;
			QVERIFY(qmudLookupMemoryImageDecodeCache(cache, QByteArray("A"), out, outAlpha, outMonochrome));
			QCOMPARE(out.size(), imageA.size());
			QVERIFY(outAlpha);
			QVERIFY(!outMonochrome);
			QCOMPARE(cache.constFirst().encodedData, QByteArray("A"));
		}

		/**
		 * @brief Verifies lookup miss for unmatched payload.
		 */
		static void lookupMissForMismatchedPayload()
		{
			QVector<QMudMemoryImageDecodeCacheEntry> cache;
			qint64                                   cacheBytes = 0;
			const QImage                             image(8, 8, QImage::Format_ARGB32);
			qmudInsertMemoryImageDecodeCache(cache, cacheBytes, QByteArray("cached"), image, false, false, 16,
			                                 16 * 1024 * 1024);

			QImage out;
			bool   outAlpha      = false;
			bool   outMonochrome = false;
			QVERIFY(
			    !qmudLookupMemoryImageDecodeCache(cache, QByteArray("other"), out, outAlpha, outMonochrome));
		}

		/**
		 * @brief Verifies entry-count and byte-size trimming limits.
		 */
		static void insertRespectsEntryAndByteLimits()
		{
			QVector<QMudMemoryImageDecodeCacheEntry> cache;
			qint64                                   cacheBytes = 0;
			const QImage                             image(32, 32, QImage::Format_ARGB32);

			qmudInsertMemoryImageDecodeCache(cache, cacheBytes, QByteArray("one"), image, false, false, 2,
			                                 12 * 1024);
			qmudInsertMemoryImageDecodeCache(cache, cacheBytes, QByteArray("two"), image, false, false, 2,
			                                 12 * 1024);
			qmudInsertMemoryImageDecodeCache(cache, cacheBytes, QByteArray("three"), image, false, false, 2,
			                                 12 * 1024);

			QVERIFY(cache.size() <= 2);
			QVERIFY(cacheBytes <= 12 * 1024);

			QImage out;
			bool   outAlpha      = false;
			bool   outMonochrome = false;
			QVERIFY(
			    !qmudLookupMemoryImageDecodeCache(cache, QByteArray("one"), out, outAlpha, outMonochrome));
			QVERIFY(
			    qmudLookupMemoryImageDecodeCache(cache, QByteArray("three"), out, outAlpha, outMonochrome));
		}

		/**
		 * @brief Verifies oversize insert clears existing cache and is rejected.
		 */
		static void oversizedInsertClearsCache()
		{
			QVector<QMudMemoryImageDecodeCacheEntry> cache;
			qint64                                   cacheBytes = 0;
			const QImage                             small(8, 8, QImage::Format_ARGB32);
			qmudInsertMemoryImageDecodeCache(cache, cacheBytes, QByteArray("small"), small, false, false, 16,
			                                 256 * 1024);
			QVERIFY(!cache.isEmpty());

			const QImage large(512, 512, QImage::Format_ARGB32);
			qmudInsertMemoryImageDecodeCache(cache, cacheBytes, QByteArray("too-large"), large, false, false,
			                                 16, 256 * 1024);
			QVERIFY(cache.isEmpty());
			QCOMPARE(cacheBytes, static_cast<qint64>(0));
		}
};

QTEST_APPLESS_MAIN(tst_MemoryImageDecodeCacheUtils)

#if __has_include("tst_MemoryImageDecodeCacheUtils.moc")
#include "tst_MemoryImageDecodeCacheUtils.moc"
#endif
