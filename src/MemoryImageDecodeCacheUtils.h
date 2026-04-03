/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MemoryImageDecodeCacheUtils.h
 * Role: Helpers for bounded in-memory decoded-image cache used by miniwindow memory image loading.
 */

#ifndef QMUD_MEMORYIMAGEDECODECACHEUTILS_H
#define QMUD_MEMORYIMAGEDECODECACHEUTILS_H

#include <QByteArray>
#include <QImage>
// ReSharper disable once CppUnusedIncludeDirective
#include <QVector>

/**
 * @brief One cached decoded image entry keyed by original encoded bytes.
 */
struct QMudMemoryImageDecodeCacheEntry
{
		QByteArray encodedData;
		QImage     decodedImage;
		bool       decodedHasAlpha{false};
		bool       monochrome{false};
		quint64    fingerprint{0};
		qint64     totalBytes{0};
};

/**
 * @brief Computes lightweight fingerprint for encoded image payload.
 * @param data Encoded image bytes.
 * @return Fingerprint value.
 */
inline quint64 qmudMemoryImageDecodeFingerprint(const QByteArray &data)
{
	const qsizetype size = data.size();
	if (size <= 0)
		return 0;

	constexpr quint64 kOffset = 1469598103934665603ULL;
	constexpr quint64 kPrime  = 1099511628211ULL;
	quint64           hash    = kOffset;
	auto              mix     = [&hash](const unsigned char byte)
	{
		hash ^= static_cast<quint64>(byte);
		hash *= kPrime;
	};

	const qsizetype sampleCount = qMin<qsizetype>(size, 192);
	const qsizetype stride      = qMax<qsizetype>(1, size / sampleCount);
	for (qsizetype i = 0, offset = 0; i < sampleCount && offset < size; ++i, offset += stride)
		mix(static_cast<unsigned char>(data.at(offset)));
	mix(static_cast<unsigned char>(data.at(0)));
	mix(static_cast<unsigned char>(data.at(size - 1)));
	hash ^= static_cast<quint64>(size);
	hash *= kPrime;
	return hash;
}

/**
 * @brief Looks up decoded image in cache by exact encoded payload match.
 * @param cache Mutable cache entries (MRU order).
 * @param data Encoded image bytes.
 * @param image Output decoded image.
 * @param decodedHasAlpha Output decoded alpha-channel flag.
 * @param monochrome Output decoded monochrome flag.
 * @return `true` when cache hit is found.
 */
inline bool qmudLookupMemoryImageDecodeCache(QVector<QMudMemoryImageDecodeCacheEntry> &cache,
                                             const QByteArray &data, QImage &image, bool &decodedHasAlpha,
                                             bool &monochrome)
{
	if (data.isEmpty() || cache.isEmpty())
		return false;

	const quint64   fingerprint = qmudMemoryImageDecodeFingerprint(data);
	const qsizetype dataSize    = data.size();
	for (int i = 0; i < cache.size(); ++i)
	{
		const QMudMemoryImageDecodeCacheEntry &entry = cache.at(i);
		if (entry.fingerprint != fingerprint)
			continue;
		if (entry.encodedData.size() != dataSize)
			continue;
		if (entry.encodedData != data)
			continue;

		image           = entry.decodedImage;
		decodedHasAlpha = entry.decodedHasAlpha;
		monochrome      = entry.monochrome;
		if (i > 0)
			cache.move(i, 0);
		return true;
	}

	return false;
}

/**
 * @brief Trims cache to entry-count and total-byte limits.
 * @param cache Mutable cache entries (MRU order).
 * @param cacheBytes Mutable cached total byte size.
 * @param maxEntries Maximum entry count.
 * @param maxBytes Maximum total bytes.
 */
inline void qmudTrimMemoryImageDecodeCache(QVector<QMudMemoryImageDecodeCacheEntry> &cache,
                                           qint64 &cacheBytes, const int maxEntries, const qint64 maxBytes)
{
	while (!cache.isEmpty() && (cache.size() > maxEntries || cacheBytes > maxBytes))
	{
		cacheBytes -= cache.constLast().totalBytes;
		cache.removeLast();
	}
	if (cacheBytes < 0)
		cacheBytes = 0;
}

/**
 * @brief Inserts decoded image into bounded MRU cache.
 * @param cache Mutable cache entries (MRU order).
 * @param cacheBytes Mutable cached total byte size.
 * @param data Encoded image bytes key.
 * @param image Decoded image value.
 * @param decodedHasAlpha Decoded alpha-channel flag.
 * @param monochrome Decoded monochrome flag.
 * @param maxEntries Maximum cache entry count.
 * @param maxBytes Maximum cache bytes.
 */
inline void qmudInsertMemoryImageDecodeCache(QVector<QMudMemoryImageDecodeCacheEntry> &cache,
                                             qint64 &cacheBytes, const QByteArray &data, const QImage &image,
                                             const bool decodedHasAlpha, const bool monochrome,
                                             const int maxEntries, const qint64 maxBytes)
{
	if (data.isEmpty() || image.isNull())
		return;

	const quint64   fingerprint = qmudMemoryImageDecodeFingerprint(data);
	const qsizetype dataSize    = data.size();
	for (int i = 0; i < cache.size(); ++i)
	{
		const QMudMemoryImageDecodeCacheEntry &existing = cache.at(i);
		if (existing.fingerprint != fingerprint)
			continue;
		if (existing.encodedData.size() != dataSize)
			continue;
		if (existing.encodedData != data)
			continue;

		cacheBytes -= existing.totalBytes;
		cache.remove(i);
		break;
	}

	QMudMemoryImageDecodeCacheEntry entry;
	entry.encodedData         = data;
	entry.decodedImage        = image;
	entry.decodedHasAlpha     = decodedHasAlpha;
	entry.monochrome          = monochrome;
	entry.fingerprint         = fingerprint;
	const qint64 encodedBytes = qMax<qint64>(0, static_cast<qint64>(entry.encodedData.size()));
	const qint64 imageBytes   = qMax<qint64>(0, static_cast<qint64>(entry.decodedImage.sizeInBytes()));
	entry.totalBytes          = encodedBytes + imageBytes;
	if (entry.totalBytes <= 0)
		return;

	if (entry.totalBytes > maxBytes)
	{
		cache.clear();
		cacheBytes = 0;
		return;
	}

	cache.prepend(std::move(entry));
	cacheBytes += cache.constFirst().totalBytes;
	qmudTrimMemoryImageDecodeCache(cache, cacheBytes, maxEntries, maxBytes);
}

#endif // QMUD_MEMORYIMAGEDECODECACHEUTILS_H
