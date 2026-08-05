/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MiniWindowUtils.cpp
 * Role: Shared miniwindow rendering helpers (brush/color plus output layout/action predicates).
 */

#include "MiniWindowUtils.h"

#include "Blending.h"
#include "FontUtils.h"
#include "HyperlinkActionUtils.h"
#include "MemoryImageDecodeCacheUtils.h"
#include "MiniWindow.h"
#include "scripting/ScriptingErrors.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QLineF>
#include <QLinearGradient>
// ReSharper disable once CppUnusedIncludeDirective
#include <QFile>
// ReSharper disable once CppUnusedIncludeDirective
#include <QMutex>
// ReSharper disable once CppUnusedIncludeDirective
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>
#include <QRandomGenerator>
#include <QScreen>
#include <QTextBoundaryFinder>
#include <QTransform>
#include <QtGlobal>
#include <QtMath>

#include <array>
#include <cmath>
#include <cstring>
#include <list>

namespace
{
	using PatternRows = std::array<quint8, 8>;

	bool hasPngSignature(const QByteArray &data)
	{
		static constexpr unsigned char kPngSignature[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
		return data.size() >= static_cast<int>(sizeof(kPngSignature)) &&
		       std::memcmp(data.constData(), kPngSignature, sizeof(kPngSignature)) == 0;
	}

	QVector<QMudMemoryImageDecodeCacheEntry> &memoryImageDecodeCache()
	{
		static QVector<QMudMemoryImageDecodeCacheEntry> cache;
		return cache;
	}

	qint64 &memoryImageDecodeCacheBytes()
	{
		static qint64 cacheBytes = 0;
		return cacheBytes;
	}

	QMutex &memoryImageDecodeCacheMutex()
	{
		static QMutex mutex;
		return mutex;
	}

	const PatternRows *patternRowsForStyle(const long brushStyle)
	{
		switch (brushStyle)
		{
		case 2:
		{
			static constexpr PatternRows rows = {0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00};
			return &rows;
		}
		case 3:
		{
			static constexpr PatternRows rows = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
			return &rows;
		}
		case 4:
		{
			static constexpr PatternRows rows = {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01};
			return &rows;
		}
		case 5:
		{
			static constexpr PatternRows rows = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
			return &rows;
		}
		case 6:
		{
			static constexpr PatternRows rows = {0xFF, 0xAA, 0xFF, 0xAA, 0xFF, 0xAA, 0xFF, 0xAA};
			return &rows;
		}
		case 7:
		{
			static constexpr PatternRows rows = {0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81};
			return &rows;
		}
		case 8:
		{
			static constexpr PatternRows rows = {0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55};
			return &rows;
		}
		case 9:
		{
			static constexpr PatternRows rows = {0x33, 0x33, 0xCC, 0xCC, 0x33, 0x33, 0xCC, 0xCC};
			return &rows;
		}
		case 10:
		{
			static constexpr PatternRows rows = {0x0F, 0x0F, 0x0F, 0x0F, 0xF0, 0xF0, 0xF0, 0xF0};
			return &rows;
		}
		case 11:
		{
			static constexpr PatternRows rows = {0xCC, 0x33, 0x00, 0x00, 0xCC, 0x33, 0x00, 0x00};
			return &rows;
		}
		case 12:
		{
			static constexpr PatternRows rows = {0x11, 0x11, 0x22, 0x22, 0x11, 0x11, 0x22, 0x22};
			return &rows;
		}
		default:
			return nullptr;
		}
	}

	QImage patternImage(const PatternRows &rows, const QColor &foreground, const QColor &background)
	{
		QImage pattern(8, 8, QImage::Format_ARGB32);
		pattern.fill(background);
		for (int y = 0; y < 8; ++y)
		{
			const quint8 row = rows[static_cast<size_t>(y)];
			for (int x = 0; x < 8; ++x)
			{
				if ((row >> (7 - x)) & 0x01)
					pattern.setPixelColor(x, y, foreground);
			}
		}
		return pattern;
	}

	struct PatternBrushKey
	{
			int           style{0};
			QRgb          foreground{0};
			QRgb          background{0};

			bool          operator==(const PatternBrushKey &) const = default;
			friend size_t qHash(const PatternBrushKey &key, size_t seed = 0) noexcept
			{
				seed = ::qHash(key.style, seed);
				seed = ::qHash(key.foreground, seed);
				return ::qHash(key.background, seed);
			}
	};

	struct PatternBrushCacheEntry
	{
			QBrush                               brush;
			std::list<PatternBrushKey>::iterator lruIt;
	};

	void touchPatternBrushLru(std::list<PatternBrushKey> &lru, const PatternBrushCacheEntry &entry)
	{
		if (entry.lruIt == std::prev(lru.end()))
			return;
		lru.splice(lru.end(), lru, entry.lruIt);
	}

	QBrush cachedPatternBrush(const long brushStyle, const QColor &foreground, const QColor &background)
	{
		const PatternRows *rows = patternRowsForStyle(brushStyle);
		if (!rows)
			return Qt::NoBrush;

		static QMutex                                         cacheMutex;
		static QHash<PatternBrushKey, PatternBrushCacheEntry> cache;
		static std::list<PatternBrushKey>                     lru;
		const PatternBrushKey key{static_cast<int>(brushStyle), foreground.rgba(), background.rgba()};

		{
			QMutexLocker lock(&cacheMutex);
			if (auto it = cache.find(key); it != cache.end())
			{
				touchPatternBrushLru(lru, it.value());
				return it.value().brush;
			}
		}

		// Build the brush outside the lock; this path is the heavy miss case.
		const QBrush brush(patternImage(*rows, foreground, background));

		QMutexLocker lock(&cacheMutex);
		if (auto it = cache.find(key); it != cache.end())
		{
			touchPatternBrushLru(lru, it.value());
			return it.value().brush;
		}

		lru.push_back(key);
		cache.insert(key, {brush, std::prev(lru.end())});

		constexpr qsizetype kCacheMaxEntries = 2048;
		if (cache.size() > kCacheMaxEntries)
		{
			constexpr qsizetype kCacheTrimTo = 1792;
			while (cache.size() > kCacheTrimTo && !lru.empty())
			{
				const PatternBrushKey evicted = lru.front();
				lru.pop_front();
				cache.remove(evicted);
			}
		}

		return brush;
	}

	constexpr int kBfTopLeft     = 0x0003;
	constexpr int kBfTopRight    = 0x0006;
	constexpr int kBfBottomLeft  = 0x0009;
	constexpr int kBfBottomRight = 0x000C;
	constexpr int kBfRect        = 0x000F;
	constexpr int kBfMiddle      = 0x0800;
	constexpr int kEdgeRaised    = 5;
	constexpr int kEdgeEtched    = 6;
	constexpr int kEdgeBump      = 9;
	constexpr int kEdgeSunken    = 10;

	constexpr int kPsStyleMask  = 0x0000000F;
	constexpr int kPsEndCapMask = 0x00000F00;
	constexpr int kPsJoinMask   = 0x0000F000;

	unsigned char clampByte(const long value)
	{
		if (value < 0)
			return 0;
		if (value > 255)
			return 255;
		return static_cast<unsigned char>(value);
	}

	long roundedToLong(const double value)
	{
		return std::lround(value);
	}

	int lowByteToInt(const long value)
	{
		return static_cast<int>(value & 0xFF);
	}

	double longToDouble(const long value)
	{
		return static_cast<double>(value);
	}

	double surfaceDevicePixelRatio(const MiniWindow &window)
	{
		if (!window.backingSurfaceIsNull())
			return window.backingSurfaceDevicePixelRatio();
		return MiniWindow::normalizedDevicePixelRatio(window.devicePixelRatio);
	}

	[[nodiscard]] QRect logicalSurfaceRect(const MiniWindow &window)
	{
		return window.logicalRect();
	}

	[[nodiscard]] QRect physicalRectForLogicalRect(const MiniWindow &window, const QRect &logicalRect)
	{
		const QRect bounded = logicalRect.intersected(logicalSurfaceRect(window));
		if (bounded.isEmpty() || window.backingSurfaceIsNull())
			return {};

		const double ratio  = surfaceDevicePixelRatio(window);
		const int    left   = static_cast<int>(std::floor(static_cast<double>(bounded.left()) * ratio));
		const int    top    = static_cast<int>(std::floor(static_cast<double>(bounded.top()) * ratio));
		const int    right  = static_cast<int>(std::ceil(static_cast<double>(bounded.right() + 1) * ratio));
		const int    bottom = static_cast<int>(std::ceil(static_cast<double>(bounded.bottom() + 1) * ratio));
		return QRect(left, top, right - left, bottom - top).intersected(window.backingSurface().rect());
	}

	[[nodiscard]] QPoint physicalPointForLogicalPixel(const MiniWindow &window, const int x, const int y)
	{
		const double ratio = surfaceDevicePixelRatio(window);
		return {static_cast<int>(std::floor((static_cast<double>(x) + 0.5) * ratio)),
		        static_cast<int>(std::floor((static_cast<double>(y) + 0.5) * ratio))};
	}

	[[nodiscard]] QImage physicalLayerForImage(const QImage &image, const QRect &source,
	                                           const QSize &logicalSize, const double ratio)
	{
		const QSize physicalSize =
		    MiniWindow::backingStoreSize(logicalSize.width(), logicalSize.height(), ratio);
		QImage layer(physicalSize, QImage::Format_ARGB32);
		layer.setDevicePixelRatio(MiniWindow::normalizedDevicePixelRatio(ratio));
		layer.fill(Qt::transparent);
		QPainter painter(&layer);
		painter.drawImage(QRect(QPoint(0, 0), logicalSize), image, source);
		return layer;
	}

	[[nodiscard]] QImage surfaceLogicalImage(const MiniWindow &window)
	{
		QImage image(qMax(1, window.width), qMax(1, window.height), QImage::Format_ARGB32);
		image.fill(Qt::transparent);
		if (window.backingSurfaceIsNull())
			return image;
		QPainter painter(&image);
		painter.drawImage(QPoint(0, 0), window.backingSurface());
		return image;
	}

	[[nodiscard]] QImage copySurfacePhysicalLayer(const MiniWindow &window, const QRect &logicalRect)
	{
		const QRect physicalRect = physicalRectForLogicalRect(window, logicalRect);
		if (physicalRect.isEmpty())
			return {};
		QImage copy = window.backingSurface().copy(physicalRect).convertToFormat(QImage::Format_ARGB32);
		copy.setDevicePixelRatio(surfaceDevicePixelRatio(window));
		return copy;
	}

	void clearTransparentSurfaceCache(MiniWindow &window)
	{
		window.transparentSurfaceCache     = QImage();
		window.transparentSurfaceSourceKey = 0;
		window.transparentSurfaceKeyRgb    = 0;
	}

	double defaultRandomUnit()
	{
		return QRandomGenerator::global()->generateDouble();
	}

	double randomUnitOrDefault(const MiniWindowUtils::RandomUnit &randomUnit)
	{
		return randomUnit ? randomUnit() : defaultRandomUnit();
	}

	int bytesPerLine24(const int width)
	{
		return MiniWindowUtils::bytesPerLine(width, 24);
	}

	int bitmapWidthBytes(const int width, const int bitsPerPixel)
	{
		if (bitsPerPixel <= 0)
			return 0;
		return ((width * bitsPerPixel + 31) / 32) * 4;
	}

	int monochromeBitmapWidthBytes(const int width)
	{
		return ((width + 15) / 16) * 2;
	}

	int bitmapBitsPerPixelFromFormat(const QImage &image)
	{
		if (image.format() == QImage::Format_Mono || image.format() == QImage::Format_MonoLSB)
			return 1;
		return image.hasAlphaChannel() ? 32 : 24;
	}

	int bitmapBitsPerPixelFromBmpHeader(const QString &filename, const QImage &fallback)
	{
		QFile file(filename);
		if (!file.open(QIODevice::ReadOnly))
			return bitmapBitsPerPixelFromFormat(fallback);

		const QByteArray header = file.read(30);
		if (header.size() < 30 || header.at(0) != 'B' || header.at(1) != 'M')
			return bitmapBitsPerPixelFromFormat(fallback);

		const auto readLe16 = [](const char *data) -> int
		{ return static_cast<unsigned char>(data[0]) | (static_cast<unsigned char>(data[1]) << 8); };
		const auto readLe32 = [](const char *data) -> quint32
		{
			return static_cast<quint32>(static_cast<unsigned char>(data[0])) |
			       (static_cast<quint32>(static_cast<unsigned char>(data[1])) << 8) |
			       (static_cast<quint32>(static_cast<unsigned char>(data[2])) << 16) |
			       (static_cast<quint32>(static_cast<unsigned char>(data[3])) << 24);
		};

		const quint32 dibHeaderSize = readLe32(header.constData() + 14);
		if (dibHeaderSize == 12 && header.size() >= 26)
			return readLe16(header.constData() + 24);
		if (dibHeaderSize >= 16 && header.size() >= 30)
			return readLe16(header.constData() + 28);
		return bitmapBitsPerPixelFromFormat(fallback);
	}

	void setImageBitmapMetadata(MiniWindowImage &entry, const int bitsPerPixel, const int widthBytes)
	{
		entry.bitmapType       = 0;
		entry.bitmapPlanes     = 1;
		entry.bitmapBitsPixel  = bitsPerPixel;
		entry.bitmapWidthBytes = widthBytes;
		entry.hasAlpha         = bitsPerPixel == 32;
		entry.monochrome       = bitsPerPixel == 1;
	}

	void imageToBgrBuffer(const QImage &image, QByteArray &buffer, const int width, const int height,
	                      const int bpl)
	{
		buffer.resize(bpl * height);
		buffer.fill(0);
		for (int y = 0; y < height; ++y)
		{
			auto *line = reinterpret_cast<unsigned char *>(buffer.data() + y * bpl);
			for (int x = 0; x < width; ++x)
			{
				const QRgb pixel  = image.pixel(x, y);
				const int  offset = x * 3;
				line[offset]      = static_cast<unsigned char>(qBlue(pixel));
				line[offset + 1]  = static_cast<unsigned char>(qGreen(pixel));
				line[offset + 2]  = static_cast<unsigned char>(qRed(pixel));
			}
		}
	}

	void bgrBufferToImage(const QByteArray &buffer, QImage &image, const int width, const int height,
	                      const int bpl)
	{
		for (int y = 0; y < height; ++y)
		{
			const auto *line = reinterpret_cast<const unsigned char *>(buffer.constData() + y * bpl);
			for (int x = 0; x < width; ++x)
			{
				const int offset = x * 3;
				image.setPixel(x, y, qRgba(line[offset + 2], line[offset + 1], line[offset], 0xFF));
			}
		}
	}

	void noise(unsigned char *buffer, const long width, const long height, const long bpl,
	           const double options, const MiniWindowUtils::RandomUnit &randomUnit)
	{
		Q_UNUSED(width);
		const long   count     = bpl * height;
		const double threshold = options / 100.0;
		for (long i = 0; i < count; ++i)
		{
			const long c =
			    buffer[i] + roundedToLong((128.0 - randomUnitOrDefault(randomUnit) * 256.0) * threshold);
			buffer[i] = clampByte(c);
		}
	}

	void monoNoise(unsigned char *buffer, const long width, const long height, const long bpl,
	               const double options, const MiniWindowUtils::RandomUnit &randomUnit)
	{
		const double threshold = options / 100.0;
		for (long row = 0; row < height; ++row)
		{
			unsigned char *line = buffer + row * bpl;
			for (long i = 0; i < width; ++i)
			{
				const long j = roundedToLong((128.0 - randomUnitOrDefault(randomUnit) * 256.0) * threshold);
				for (int channel = 0; channel < 3; ++channel)
					line[channel] = clampByte(line[channel] + j);
				line += 3;
			}
		}
	}

	void brightness(unsigned char *buffer, const long width, const long height, const long bpl,
	                const double options)
	{
		Q_UNUSED(width);
		const long count = bpl * height;
		for (long i = 0; i < count; ++i)
			buffer[i] = clampByte(buffer[i] + roundedToLong(options));
	}

	void contrast(unsigned char *buffer, const long width, const long height, const long bpl,
	              const double options)
	{
		Q_UNUSED(width);
		const long count = bpl * height;
		for (long i = 0; i < count; ++i)
			buffer[i] = clampByte(static_cast<long>((buffer[i] - 128) * options + 128));
	}

	void gammaAdjust(unsigned char *buffer, const long width, const long height, const long bpl,
	                 double options)
	{
		Q_UNUSED(width);
		if (options < 0.0)
			options = 0.0;
		const long count = bpl * height;
		for (long i = 0; i < count; ++i)
			buffer[i] =
			    clampByte(static_cast<long>(qPow(static_cast<double>(buffer[i]) / 255.0, options) * 255.0));
	}

	void colourBrightness(unsigned char *buffer, const long width, const long height, const long bpl,
	                      const double options, const long channel)
	{
		for (long row = 0; row < height; ++row)
		{
			unsigned char *line = buffer + row * bpl + channel;
			for (long i = 0; i < width; ++i)
			{
				*line = clampByte(*line + roundedToLong(options));
				line += 3;
			}
		}
	}

	void colourBrightnessMultiply(unsigned char *buffer, const long width, const long height, const long bpl,
	                              const double options, const long channel)
	{
		for (long row = 0; row < height; ++row)
		{
			unsigned char *line = buffer + row * bpl + channel;
			for (long i = 0; i < width; ++i)
			{
				*line = clampByte(roundedToLong(static_cast<double>(*line) * options));
				line += 3;
			}
		}
	}

	void colourContrast(unsigned char *buffer, const long width, const long height, const long bpl,
	                    const double options, const long channel)
	{
		unsigned char lookup[256];
		for (int i = 0; i < 256; ++i)
			lookup[i] = clampByte(static_cast<long>((i - 128) * options + 128));
		for (long row = 0; row < height; ++row)
		{
			unsigned char *line = buffer + row * bpl + channel;
			for (long i = 0; i < width; ++i)
			{
				*line = lookup[*line];
				line += 3;
			}
		}
	}

	void colourGamma(unsigned char *buffer, const long width, const long height, const long bpl,
	                 double options, const long channel)
	{
		if (options < 0.0)
			options = 0.0;
		unsigned char lookup[256];
		for (int i = 0; i < 256; ++i)
			lookup[i] = clampByte(static_cast<long>(qPow(static_cast<double>(i) / 255.0, options) * 255.0));
		for (long row = 0; row < height; ++row)
		{
			unsigned char *line = buffer + row * bpl + channel;
			for (long i = 0; i < width; ++i)
			{
				*line = lookup[*line];
				line += 3;
			}
		}
	}

	void makeGreyscale(unsigned char *buffer, const long width, const long height, const long bpl,
	                   const bool linear)
	{
		for (long row = 0; row < height; ++row)
		{
			unsigned char *line = buffer + row * bpl;
			for (long i = 0; i < width; ++i)
			{
				const double        c     = linear ? (line[0] + line[1] + line[2]) / 3.0
				                                   : line[0] * 0.11 + line[1] * 0.59 + line[2] * 0.30;
				const unsigned char value = clampByte(static_cast<long>(c));
				line[0]                   = value;
				line[1]                   = value;
				line[2]                   = value;
				line += 3;
			}
		}
	}

	void averageBuffer(unsigned char *buffer, const long width, const long height)
	{
		qint64    r         = 0;
		qint64    g         = 0;
		qint64    b         = 0;
		qint64    count     = 0;
		const int increment = bytesPerLine24(static_cast<int>(width));
		for (long col = 0; col < width; ++col)
		{
			const unsigned char *p = buffer + col * 3;
			for (long row = 0; row < height; ++row)
			{
				b += p[0];
				g += p[1];
				r += p[2];
				++count;
				p += increment;
			}
		}
		if (count == 0)
			return;
		b /= count;
		g /= count;
		r /= count;
		for (long col = 0; col < width; ++col)
		{
			unsigned char *p = buffer + col * 3;
			for (long row = 0; row < height; ++row)
			{
				p[0] = clampByte(b);
				p[1] = clampByte(g);
				p[2] = clampByte(r);
				p += increment;
			}
		}
	}

	void brightnessMultiply(unsigned char *buffer, const long width, const long height, const long bpl,
	                        const double options)
	{
		Q_UNUSED(width);
		const long count = bpl * height;
		for (long i = 0; i < count; ++i)
			buffer[i] = clampByte(roundedToLong(static_cast<double>(buffer[i]) * options));
	}

	void generalFilter(unsigned char *buffer, const long width, const long height, const long bpl,
	                   const double options, const double *matrix, const double divisor)
	{
		if (options != 2)
		{
			for (long row = 0; row < height; ++row)
			{
				unsigned char *line     = buffer + bpl * row;
				const int      lastByte = static_cast<int>(width * 3);
				for (long rgb = 0; rgb < 3; ++rgb)
				{
					long windowBytes[5];
					for (long col = 0; col < 4; ++col)
						windowBytes[col + 1] =
						    line[qBound(0, static_cast<int>(rgb + (col * 3) - 6), lastByte - 1)];
					for (long col = 0; col < lastByte - 4; col += 3)
					{
						windowBytes[0] = windowBytes[1];
						windowBytes[1] = windowBytes[2];
						windowBytes[2] = windowBytes[3];
						windowBytes[3] = windowBytes[4];
						windowBytes[4] = line[qBound(0, static_cast<int>(col + rgb + 6), lastByte - 1)];
						long total     = 0;
						for (int i = 0; i < 5; ++i)
							total += roundedToLong(static_cast<double>(windowBytes[i]) * matrix[i]);
						line[col + rgb] = clampByte(roundedToLong(static_cast<double>(total) / divisor));
					}
				}
			}
		}

		if (options != 1)
		{
			const int lastByte = static_cast<int>(width * 3);
			for (long col = 0; col < lastByte; ++col)
			{
				long windowBytes[5];
				for (long row = 0; row < 4; ++row)
				{
					const long           from = qBound(0L, row - 2, height - 1);
					const unsigned char *line = buffer + col + (from * bpl);
					windowBytes[row + 1]      = *line;
				}
				for (long row = 0; row < height; ++row)
				{
					const long     from = qBound(0L, row + 3, height - 1);
					unsigned char *line = buffer + col + (from * bpl);
					windowBytes[0]      = windowBytes[1];
					windowBytes[1]      = windowBytes[2];
					windowBytes[2]      = windowBytes[3];
					windowBytes[3]      = windowBytes[4];
					windowBytes[4]      = *line;
					long total          = 0;
					for (int i = 0; i < 5; ++i)
						total += roundedToLong(static_cast<double>(windowBytes[i]) * matrix[i]);
					line  = buffer + col + (row * bpl);
					*line = clampByte(roundedToLong(static_cast<double>(total) / divisor));
				}
			}
		}
	}

	bool blendPixelInternal(const long blend, const long base, const short mode, const double opacity,
	                        long &out, const MiniWindowUtils::RandomUnit &randomUnit)
	{
		const long rA = blend & 0xFF;
		const long gA = (blend >> 8) & 0xFF;
		const long bA = (blend >> 16) & 0xFF;
		const long rB = base & 0xFF;
		const long gB = (base >> 8) & 0xFF;
		const long bB = (base >> 16) & 0xFF;

		long       r = 0;
		long       g = 0;
		long       b = 0;

		if (opacity < 0.0 || opacity > 1.0)
			return false;

		static const std::array<quint8, 256> cosTable = []
		{
			std::array<quint8, 256> table{};
			constexpr double        kPiDiv255 = 3.1415926535898 / 255.0;
			for (int i = 0; i < 256; ++i)
			{
				const double value            = 64.0 - qCos(static_cast<double>(i) * kPiDiv255) * 64.0;
				table[static_cast<size_t>(i)] = static_cast<quint8>(std::lround(value));
			}
			return table;
		}();

		auto applyBlendOp = [&](auto blendOp)
		{
			if (opacity < 1.0)
			{
				r = QMudBlend::withOpacity(rA, rB, blendOp, opacity);
				g = QMudBlend::withOpacity(gA, gB, blendOp, opacity);
				b = QMudBlend::withOpacity(bA, bB, blendOp, opacity);
			}
			else
			{
				r = blendOp(rA, rB);
				g = blendOp(gA, gB);
				b = blendOp(bA, bB);
			}
		};

		auto applyRgb = [&](const long outR, const long outG, const long outB)
		{
			if (opacity < 1.0)
			{
				r = QMudBlend::simpleOpacity(static_cast<double>(rB), static_cast<double>(outR), opacity);
				g = QMudBlend::simpleOpacity(static_cast<double>(gB), static_cast<double>(outG), opacity);
				b = QMudBlend::simpleOpacity(static_cast<double>(bB), static_cast<double>(outB), opacity);
			}
			else
			{
				r = outR;
				g = outG;
				b = outB;
			}
		};

		switch (mode)
		{
		case 1:
			applyBlendOp(QMudBlend::normal);
			break;
		case 2:
			applyBlendOp(QMudBlend::average);
			break;
		case 3:
			applyBlendOp([](const long blendChannel, const long baseChannel)
			             { return QMudBlend::interpolate(blendChannel, baseChannel, cosTable.data()); });
			break;
		case 4:
		{
			const double rnd = randomUnitOrDefault(randomUnit);
			r                = (rnd < opacity) ? rA : rB;
			g                = (rnd < opacity) ? gA : gB;
			b                = (rnd < opacity) ? bA : bB;
			break;
		}
		case 5:
			applyBlendOp(QMudBlend::darken);
			break;
		case 6:
			applyBlendOp(QMudBlend::multiply);
			break;
		case 7:
			applyBlendOp(QMudBlend::colorBurn);
			break;
		case 8:
			applyBlendOp(QMudBlend::linearBurn);
			break;
		case 9:
			applyBlendOp(QMudBlend::inverseColorBurn);
			break;
		case 10:
			applyBlendOp(QMudBlend::subtract);
			break;
		case 11:
			applyBlendOp(QMudBlend::lighten);
			break;
		case 12:
			applyBlendOp(QMudBlend::screen);
			break;
		case 13:
			applyBlendOp(QMudBlend::colorDodge);
			break;
		case 14:
			applyBlendOp(QMudBlend::linearDodge);
			break;
		case 15:
			applyBlendOp(QMudBlend::inverseColorDodge);
			break;
		case 16:
			applyBlendOp(QMudBlend::add);
			break;
		case 17:
			applyBlendOp(QMudBlend::overlay);
			break;
		case 18:
			applyBlendOp(QMudBlend::softLight);
			break;
		case 19:
			applyBlendOp(QMudBlend::hardLight);
			break;
		case 20:
			applyBlendOp(QMudBlend::vividLight);
			break;
		case 21:
			applyBlendOp(QMudBlend::linearLight);
			break;
		case 22:
			applyBlendOp(QMudBlend::pinLight);
			break;
		case 23:
			applyBlendOp(QMudBlend::hardMix);
			break;
		case 24:
			applyBlendOp(QMudBlend::difference);
			break;
		case 25:
			applyBlendOp(QMudBlend::exclusion);
			break;
		case 26:
			applyBlendOp(QMudBlend::reflect);
			break;
		case 27:
			applyBlendOp(QMudBlend::glow);
			break;
		case 28:
			applyBlendOp(QMudBlend::freeze);
			break;
		case 29:
			applyBlendOp(QMudBlend::heat);
			break;
		case 30:
			applyBlendOp(QMudBlend::negation);
			break;
		case 31:
			applyBlendOp(QMudBlend::phoenix);
			break;
		case 32:
			applyBlendOp(QMudBlend::stamp);
			break;
		case 33:
			applyBlendOp(QMudBlend::bitXor);
			break;
		case 34:
			applyBlendOp(QMudBlend::bitAnd);
			break;
		case 35:
			applyBlendOp(QMudBlend::bitOr);
			break;
		case 36:
			applyRgb(rA, gB, bB);
			break;
		case 37:
			applyRgb(rB, gA, bB);
			break;
		case 38:
			applyRgb(rB, gB, bA);
			break;
		case 39:
			applyRgb(rA, gA, bB);
			break;
		case 40:
			applyRgb(rB, gA, bA);
			break;
		case 41:
			applyRgb(rA, gB, bA);
			break;
		case 42:
			applyRgb(rA, (gA > rA) ? rA : gA, bA);
			break;
		case 43:
			applyRgb(rA, (gA > bA) ? bA : gA, bA);
			break;
		case 44:
			applyRgb(rA, (gA > ((rA + bA) / 2)) ? ((rA + bA) / 2) : gA, bA);
			break;
		case 45:
			applyRgb(rA, gA, (bA > rA) ? rA : bA);
			break;
		case 46:
			applyRgb(rA, gA, (bA > gA) ? gA : bA);
			break;
		case 47:
			applyRgb(rA, gA, (bA > ((rA + gA) / 2)) ? ((rA + gA) / 2) : bA);
			break;
		case 48:
			applyRgb((rA > gA) ? gA : rA, gA, bA);
			break;
		case 49:
			applyRgb((rA > bA) ? bA : rA, gA, bA);
			break;
		case 50:
			applyRgb((rA > ((gA + bA) / 2)) ? ((gA + bA) / 2) : rA, gA, bA);
			break;
		case 51:
			applyRgb(rA, 0, 0);
			break;
		case 52:
			applyRgb(0, gA, 0);
			break;
		case 53:
			applyRgb(0, 0, bA);
			break;
		case 54:
			applyRgb(0, gA, bA);
			break;
		case 55:
			applyRgb(rA, 0, bA);
			break;
		case 56:
			applyRgb(rA, gA, 0);
			break;
		case 57:
			applyRgb(rA, rA, rA);
			break;
		case 58:
			applyRgb(gA, gA, gA);
			break;
		case 59:
			applyRgb(bA, bA, bA);
			break;
		case 60:
		{
			const QColor cA(lowByteToInt(rA), lowByteToInt(gA), lowByteToInt(bA));
			const QColor cB(lowByteToInt(rB), lowByteToInt(gB), lowByteToInt(bB));
			const int    hue      = (cA.hslHue() < 0) ? 0 : cA.hslHue();
			const QColor outColor = QColor::fromHsl(hue, cB.hslSaturation(), cB.lightness());
			r                     = QMudBlend::simpleOpacity(longToDouble(rB), outColor.red(), opacity);
			g                     = QMudBlend::simpleOpacity(longToDouble(gB), outColor.green(), opacity);
			b                     = QMudBlend::simpleOpacity(longToDouble(bB), outColor.blue(), opacity);
			break;
		}
		case 61:
		{
			const QColor cA(lowByteToInt(rA), lowByteToInt(gA), lowByteToInt(bA));
			const QColor cB(lowByteToInt(rB), lowByteToInt(gB), lowByteToInt(bB));
			const int    sat      = cA.hslSaturation();
			const QColor outColor = QColor::fromHsl(cB.hslHue() < 0 ? 0 : cB.hslHue(), sat, cB.lightness());
			r                     = QMudBlend::simpleOpacity(longToDouble(rB), outColor.red(), opacity);
			g                     = QMudBlend::simpleOpacity(longToDouble(gB), outColor.green(), opacity);
			b                     = QMudBlend::simpleOpacity(longToDouble(bB), outColor.blue(), opacity);
			break;
		}
		case 62:
		{
			const QColor cA(lowByteToInt(rA), lowByteToInt(gA), lowByteToInt(bA));
			const QColor cB(lowByteToInt(rB), lowByteToInt(gB), lowByteToInt(bB));
			const int    hue      = (cA.hslHue() < 0) ? 0 : cA.hslHue();
			const int    sat      = cA.hslSaturation();
			const QColor outColor = QColor::fromHsl(hue, sat, cB.lightness());
			r                     = QMudBlend::simpleOpacity(longToDouble(rB), outColor.red(), opacity);
			g                     = QMudBlend::simpleOpacity(longToDouble(gB), outColor.green(), opacity);
			b                     = QMudBlend::simpleOpacity(longToDouble(bB), outColor.blue(), opacity);
			break;
		}
		case 63:
		{
			const QColor cA(lowByteToInt(rA), lowByteToInt(gA), lowByteToInt(bA));
			const QColor cB(lowByteToInt(rB), lowByteToInt(gB), lowByteToInt(bB));
			const QColor outColor =
			    QColor::fromHsl(cB.hslHue() < 0 ? 0 : cB.hslHue(), cB.hslSaturation(), cA.lightness());
			r = QMudBlend::simpleOpacity(longToDouble(rB), outColor.red(), opacity);
			g = QMudBlend::simpleOpacity(longToDouble(gB), outColor.green(), opacity);
			b = QMudBlend::simpleOpacity(longToDouble(bB), outColor.blue(), opacity);
			break;
		}
		case 64:
		{
			const QColor cA(lowByteToInt(rA), lowByteToInt(gA), lowByteToInt(bA));
			const qreal  hue       = cA.hslHueF();
			const qreal  sat       = cA.hslSaturationF();
			const qreal  lum       = cA.lightnessF();
			const double hueScaled = (hue < 0.0) ? 0.0 : hue * 255.0;
			r                      = QMudBlend::simpleOpacity(longToDouble(rB), hueScaled, opacity);
			g                      = QMudBlend::simpleOpacity(longToDouble(gB), sat * 255.0, opacity);
			b                      = QMudBlend::simpleOpacity(longToDouble(bB), lum * 255.0, opacity);
			break;
		}
		default:
			return false;
		}

		out = static_cast<long>(clampByte(r)) | (static_cast<long>(clampByte(g)) << 8) |
		      (static_cast<long>(clampByte(b)) << 16);
		return true;
	}

	Qt::PenStyle mapPenStyle(const long style)
	{
		switch (static_cast<int>(style & 0xFF))
		{
		case 1:
			return Qt::DashLine;
		case 2:
			return Qt::DotLine;
		case 3:
			return Qt::DashDotLine;
		case 4:
			return Qt::DashDotDotLine;
		case 5:
			return Qt::NoPen;
		default:
			return Qt::SolidLine;
		}
	}

	Qt::PenCapStyle mapPenCap(const long style)
	{
		if (style & 0x00000100)
			return Qt::SquareCap;
		if (style & 0x00000200)
			return Qt::FlatCap;
		return Qt::RoundCap;
	}

	Qt::PenJoinStyle mapPenJoin(const long style)
	{
		if (style & 0x00001000)
			return Qt::BevelJoin;
		if (style & 0x00002000)
			return Qt::MiterJoin;
		return Qt::RoundJoin;
	}

	template <typename Visitor> void forEachTextCluster(const QString &text, Visitor &&visitor)
	{
		QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
		finder.toStart();
		int start = 0;
		while (true)
		{
			const int end = finder.toNextBoundary();
			if (end < 0)
				break;
			if (end > start)
				visitor(text.mid(start, end - start));
			start = end;
		}
		if (start < text.size())
			visitor(text.mid(start));
	}

	int measuredTextWidthForMiniWindow(const MiniWindow &window, const MiniWindowFont &fontEntry,
	                                   const QString &text)
	{
		if (text.isEmpty())
			return 0;
		const QImage       &surface = window.backingSurface();
		const QPaintDevice *device  = surface.isNull() ? nullptr : &surface;
		const QFontMetrics  metrics(fontEntry.font, device);
		int                 width = 0;
		forEachTextCluster(text, [&metrics, &width](const QString &cluster)
		                   { width += qMax(0, metrics.horizontalAdvance(cluster)); });
		return width;
	}

} // namespace

namespace MiniWindowUtils::Internal
{
	QImage &mutableBackingSurface(MiniWindow &window)
	{
		return window.mutableBackingSurface();
	}
} // namespace MiniWindowUtils::Internal

namespace MiniWindowUtils
{
	QColor colorFromRef(const long value)
	{
		const int r = static_cast<int>(value & 0xFF);
		const int g = static_cast<int>((value >> 8) & 0xFF);
		const int b = static_cast<int>((value >> 16) & 0xFF);
		return {r, g, b};
	}

	long colorToRef(const QColor &color)
	{
		return static_cast<long>(color.red()) | (static_cast<long>(color.green()) << 8) |
		       (static_cast<long>(color.blue()) << 16);
	}

	QColor colorFromRefOrTransparent(const long value)
	{
		if (value == -1)
			return {0, 0, 0, 0};
		return colorFromRef(value);
	}

	QBrush makeBrush(const long brushStyle, const long penColour, const long brushColour, bool *ok)
	{
		if (ok)
			*ok = true;

		if (brushStyle == 1 || brushColour == -1)
			return Qt::NoBrush;

		const QColor penColor   = colorFromRefOrTransparent(penColour);
		const QColor brushColor = colorFromRefOrTransparent(brushColour);

		switch (brushStyle)
		{
		case 0:
			return {brushColor};
		case 2:
		case 3:
		case 4:
		case 5:
		case 6:
		case 7:
			return cachedPatternBrush(brushStyle, penColor, brushColor);
		case 8:
		case 9:
		case 10:
		case 11:
		case 12:
			return cachedPatternBrush(brushStyle, brushColor, penColor);
		default:
			if (ok)
				*ok = false;
			return Qt::NoBrush;
		}
	}

	QRect rectFromCoords(const MiniWindow &window, const long left, const long top, const long right,
	                     const long bottom)
	{
		const int fixedRight  = window.fixRight(static_cast<int>(right));
		const int fixedBottom = window.fixBottom(static_cast<int>(bottom));
		return {static_cast<int>(left), static_cast<int>(top), fixedRight - static_cast<int>(left),
		        fixedBottom - static_cast<int>(top)};
	}

	QPen makePen(const long colour, const long style, const int width)
	{
		QPen pen(colorFromRef(colour));
		pen.setWidth(qMax(1, width));
		pen.setStyle(mapPenStyle(style));
		pen.setCapStyle(mapPenCap(style));
		pen.setJoinStyle(mapPenJoin(style));
		return pen;
	}

	int validatePenStyle(const long penStyle, const int penWidth)
	{
		switch (penStyle & kPsStyleMask)
		{
		case 0:
		case 5:
		case 6:
			break;
		case 1:
		case 2:
		case 3:
		case 4:
			if (penWidth > 1)
				return ePenStyleNotValid;
			break;
		default:
			return ePenStyleNotValid;
		}
		switch (penStyle & kPsEndCapMask)
		{
		case 0x00000000:
		case 0x00000100:
		case 0x00000200:
			break;
		default:
			return ePenStyleNotValid;
		}
		switch (penStyle & kPsJoinMask)
		{
		case 0x00000000:
		case 0x00001000:
		case 0x00002000:
			break;
		default:
			return ePenStyleNotValid;
		}
		return eOK;
	}

	int validateBrushStyle(const long brushStyle)
	{
		switch (brushStyle)
		{
		case 0:
		case 1:
		case 2:
		case 3:
		case 4:
		case 5:
		case 6:
		case 7:
		case 8:
		case 9:
		case 10:
		case 11:
		case 12:
			return eOK;
		default:
			return eBrushStyleNotValid;
		}
	}

	int bytesPerLine(const int width, const int bitsPerPixel)
	{
		return ((width * bitsPerPixel + 31) / 32) * 4;
	}

	int textPreviewWidth(const MiniWindow &window, const QString &fontId, const QString &text, const int left,
	                     const int top, const int right, const int bottom)
	{
		const auto it = window.fonts.constFind(fontId);
		if (it == window.fonts.constEnd())
			return -2;
		if (window.backingSurfaceIsNull())
			return eOK;
		if (text.isEmpty())
			return 0;
		const QRect rect = rectFromCoords(window, left, top, right, bottom);
		if (rect.width() <= 0 || rect.height() <= 0)
			return eOK;
		return qMin(textWidth(window, fontId, text), rect.width());
	}

	int textWidth(const MiniWindow &window, const QString &fontId, const QString &text)
	{
		const auto it = window.fonts.constFind(fontId);
		if (it == window.fonts.constEnd())
			return -2;
		return measuredTextWidthForMiniWindow(window, it.value(), text);
	}

	QVariant fontInfo(const MiniWindow &window, const QString &fontId, const int infoType)
	{
		const auto it = window.fonts.constFind(fontId);
		if (it == window.fonts.constEnd())
			return {};

		const MiniWindowFont &fontEntry = it.value();
		const QFontMetrics   &metrics   = fontEntry.metrics;
		const QFont          &font      = fontEntry.font;
		switch (infoType)
		{
		case 1:
			return metrics.height();
		case 2:
			return metrics.ascent();
		case 3:
			return metrics.descent();
		case 4:
			return 0;
		case 5:
			return metrics.leading();
		case 6:
			return metrics.averageCharWidth();
		case 7:
			return metrics.maxWidth();
		case 8:
			return font.weight();
		case 9:
			return 0;
		case 10:
			if (const QScreen *screen = QGuiApplication::primaryScreen())
				return static_cast<int>(screen->logicalDotsPerInchX());
			return 0;
		case 11:
			if (const QScreen *screen = QGuiApplication::primaryScreen())
				return static_cast<int>(screen->logicalDotsPerInchY());
			return 0;
		case 12:
			return 32;
		case 13:
			return 255;
		case 14:
			return 63;
		case 15:
			return 32;
		case 16:
			return font.italic() ? 1 : 0;
		case 17:
			return font.underline() ? 1 : 0;
		case 18:
			return font.strikeOut() ? 1 : 0;
		case 19:
			return fontEntry.pitchAndFamily;
		case 20:
			return fontEntry.charset;
		case 21:
			return font.family();
		default:
			return {};
		}
	}

	QVariant imageInfo(const MiniWindow &window, const QString &imageId, const int infoType)
	{
		const auto it = window.images.constFind(imageId);
		if (it == window.images.constEnd())
			return {};
		const QImage &image = it.value().image;
		switch (infoType)
		{
		case 1:
			return 0;
		case 2:
			return image.width();
		case 3:
			return image.height();
		case 4:
			return it.value().bitmapWidthBytes;
		case 5:
			return it.value().bitmapPlanes;
		case 6:
			return it.value().bitmapBitsPixel;
		default:
			return {};
		}
	}

	QVariant hotspotInfo(const MiniWindow &window, const QString &hotspotId, const int infoType)
	{
		const auto it = window.hotspots.constFind(hotspotId);
		if (it == window.hotspots.constEnd())
			return {};
		const MiniWindowHotspot &hotspot = it.value();
		switch (infoType)
		{
		case 1:
			return hotspot.rect.left();
		case 2:
			return hotspot.rect.top();
		case 3:
			return hotspot.rect.right();
		case 4:
			return hotspot.rect.bottom();
		case 5:
			return hotspot.mouseOver;
		case 6:
			return hotspot.cancelMouseOver;
		case 7:
			return hotspot.mouseDown;
		case 8:
			return hotspot.cancelMouseDown;
		case 9:
			return hotspot.mouseUp;
		case 10:
			return hotspot.tooltip;
		case 11:
			return hotspot.cursor;
		case 12:
			return hotspot.flags;
		case 13:
			return hotspot.moveCallback;
		case 14:
			return hotspot.releaseCallback;
		case 15:
			return hotspot.dragFlags;
		default:
			return {};
		}
	}

	long pixelValue(const MiniWindow &window, const int x, const int y)
	{
		if (!logicalSurfaceRect(window).contains(x, y))
			return -1;
		const QPoint physicalPoint = physicalPointForLogicalPixel(window, x, y);
		if (!window.backingSurface().rect().contains(physicalPoint))
			return -1;
		return colorToRef(QColor(window.backingSurface().pixel(physicalPoint)));
	}

	bool saveWindowImage24Bit(const MiniWindow &window, const QString &filename)
	{
		if (window.backingSurfaceIsNull())
			return false;
		return surfaceLogicalImage(window).convertToFormat(QImage::Format_RGB888).save(filename);
	}

	void setPixel(MiniWindow &window, const int x, const int y, const long colour)
	{
		if (!logicalSurfaceRect(window).contains(x, y))
			return;
		const QColor c = colorFromRef(colour);
		QPainter     painter(&Internal::mutableBackingSurface(window));
		painter.fillRect(QRect(x, y, 1, 1), c);
	}

	void create(MiniWindow &window, const QString &name, const int left, const int top, const int width,
	            const int height, const int position, const int flags, const QColor &background,
	            const QString &pluginId, const double devicePixelRatio)
	{
		window.name = name;
		window.create(left, top, width, height, position, flags, background, devicePixelRatio);
		window.creatingPlugin = pluginId;
		if ((flags & kMiniWindowKeepHotspots) == 0)
		{
			window.hotspots.clear();
			window.callbackPlugin.clear();
			window.mouseOverHotspot.clear();
			window.mouseDownHotspot.clear();
			window.flagsOnMouseDown = 0;
			window.outputGeneratedHotspots.clear();
			window.outputHotspotSerial = 0;
		}
	}

	void setDevicePixelRatio(MiniWindow &window, const double devicePixelRatio)
	{
		const double normalizedRatio = MiniWindow::normalizedDevicePixelRatio(devicePixelRatio);
		const QSize physicalSize = MiniWindow::backingStoreSize(window.width, window.height, normalizedRatio);
		if (qFuzzyCompare(window.devicePixelRatio, normalizedRatio) &&
		    qFuzzyCompare(window.backingSurfaceDevicePixelRatio(), normalizedRatio) &&
		    window.backingSurfaceSize() == physicalSize)
			return;

		QImage newSurface(physicalSize, QImage::Format_ARGB32);
		newSurface.setDevicePixelRatio(normalizedRatio);
		newSurface.fill(window.background.isValid() ? window.background : QColor(0, 0, 0));
		if (!window.backingSurfaceIsNull())
		{
			QPainter painter(&newSurface);
			painter.drawImage(QPoint(0, 0), window.backingSurface());
		}
		window.setBackingSurface(newSurface);
		clearTransparentSurfaceCache(window);
	}

	int font(MiniWindow &window, const QString &fontId, const QString &fontName, const double size,
	         const bool bold, const bool italic, const bool underline, const bool strikeout,
	         const int charset, const int pitchAndFamily)
	{
		if (fontName.isEmpty() && qFuzzyIsNull(size))
		{
			window.fonts.remove(fontId);
			return eOK;
		}

		QFont font;
		if (!fontName.isEmpty())
			qmudApplyMonospaceFallback(font, fontName);
		const QString preferredFamily = fontName.isEmpty() ? font.family() : fontName;
		const QString charsetFamily   = qmudFamilyForCharset(preferredFamily, charset);
		if (!charsetFamily.isEmpty())
			qmudApplyMonospaceFallback(font, charsetFamily);
		font.setPointSizeF(size > 0.0 ? size : 10.0);
		font.setBold(bold);
		font.setItalic(italic);
		font.setUnderline(underline);
		font.setStrikeOut(strikeout);

		if ((pitchAndFamily & 0x03) == 0x01)
			font.setFixedPitch(true);

		switch (pitchAndFamily & 0xF0)
		{
		case 0x10:
			font.setStyleHint(QFont::Serif);
			break;
		case 0x20:
			font.setStyleHint(QFont::SansSerif);
			break;
		case 0x30:
			font.setStyleHint(QFont::TypeWriter);
			break;
		case 0x40:
		case 0x50:
			font.setStyleHint(QFont::Decorative);
			break;
		default:
			font.setStyleHint(QFont::AnyStyle);
			break;
		}

		MiniWindowFont entry;
		entry.font           = font;
		entry.metrics        = QFontMetrics(font);
		entry.charset        = charset;
		entry.pitchAndFamily = pitchAndFamily;
		window.fonts.insert(fontId, entry);
		return eOK;
	}

	int rectOp(MiniWindow &window, const int action, const int left, const int top, const int right,
	           const int bottom, const long colour1, const long colour2)
	{
		QImage     &surface = Internal::mutableBackingSurface(window);
		const QRect rect    = rectFromCoords(window, left, top, right, bottom);
		switch (action)
		{
		case 1:
		{
			if (surface.isNull())
				return eOK;
			QPainter painter(&surface);
			painter.setPen(QPen(colorFromRef(colour1)));
			painter.setBrush(Qt::NoBrush);
			painter.drawRect(rect.adjusted(0, 0, -1, -1));
			return eOK;
		}
		case 2:
		{
			if (surface.isNull())
				return eOK;
			QPainter painter(&surface);
			painter.fillRect(rect, colorFromRef(colour1));
			return eOK;
		}
		case 3:
		{
			if (surface.isNull())
				return eOK;
			const QRect clipped = physicalRectForLogicalRect(window, rect);
			if (clipped.isEmpty())
				return eOK;
			for (int y = clipped.top(); y <= clipped.bottom(); ++y)
			{
				auto *line = reinterpret_cast<QRgb *>(surface.scanLine(y));
				for (int x = clipped.left(); x <= clipped.right(); ++x)
				{
					const QColor current(line[x]);
					line[x] = qRgba(255 - current.red(), 255 - current.green(), 255 - current.blue(),
					                current.alpha());
				}
			}
			return eOK;
		}
		case 4:
		{
			if (surface.isNull())
				return eOK;
			QPainter painter(&surface);
			painter.setPen(colorFromRef(colour1));
			painter.drawLine(rect.topLeft(), rect.topRight());
			painter.drawLine(rect.topLeft(), rect.bottomLeft());
			painter.setPen(colorFromRef(colour2));
			painter.drawLine(rect.bottomLeft(), rect.bottomRight());
			painter.drawLine(rect.topRight(), rect.bottomRight());
			return eOK;
		}
		case 5:
		{
			if (colour1 != kEdgeRaised && colour1 != kEdgeEtched && colour1 != kEdgeBump &&
			    colour1 != kEdgeSunken)
				return eBadParameter;
			if ((colour2 & 0xFF) > 0x1F)
				return eBadParameter;
			if (surface.isNull())
				return eOK;
			const QColor base  = window.background.isValid() ? window.background : QColor(64, 64, 64);
			QColor       light = base.lighter(150);
			QColor       dark  = base.darker(150);
			if (colour1 == kEdgeSunken || colour1 == kEdgeEtched)
				qSwap(light, dark);
			const bool drawTop    = (colour2 & (kBfTopLeft | kBfTopRight | kBfRect)) != 0;
			const bool drawBottom = (colour2 & (kBfBottomLeft | kBfBottomRight | kBfRect)) != 0;
			const bool drawLeft   = (colour2 & (kBfTopLeft | kBfBottomLeft | kBfRect)) != 0;
			const bool drawRight  = (colour2 & (kBfTopRight | kBfBottomRight | kBfRect)) != 0;
			QPainter   painter(&surface);
			if (colour2 & kBfMiddle)
				painter.fillRect(rect.adjusted(1, 1, -1, -1), base);
			if (drawTop)
			{
				painter.setPen(light);
				painter.drawLine(rect.topLeft(), rect.topRight());
			}
			if (drawLeft)
			{
				painter.setPen(light);
				painter.drawLine(rect.topLeft(), rect.bottomLeft());
			}
			if (drawBottom)
			{
				painter.setPen(dark);
				painter.drawLine(rect.bottomLeft(), rect.bottomRight());
			}
			if (drawRight)
			{
				painter.setPen(dark);
				painter.drawLine(rect.topRight(), rect.bottomRight());
			}
			if (colour1 == kEdgeEtched || colour1 == kEdgeBump)
			{
				const QRect inner = rect.adjusted(1, 1, -1, -1);
				if (inner.width() > 1 && inner.height() > 1)
				{
					painter.setPen(dark);
					painter.drawLine(inner.topLeft(), inner.topRight());
					painter.drawLine(inner.topLeft(), inner.bottomLeft());
					painter.setPen(light);
					painter.drawLine(inner.bottomLeft(), inner.bottomRight());
					painter.drawLine(inner.topRight(), inner.bottomRight());
				}
			}
			return eOK;
		}
		case 6:
		case 7:
		{
			if (surface.isNull())
				return eOK;
			const QColor borderColour = colorFromRef(colour1);
			const QColor fillColour   = colorFromRef(colour2);
			if (!logicalSurfaceRect(window).contains(left, top))
				return eOK;
			const QPoint start = physicalPointForLogicalPixel(window, left, top);
			if (!surface.rect().contains(start))
				return eOK;
			const QRgb borderRgb = borderColour.rgb();
			const QRgb fillRgb   = fillColour.rgb();
			const QRgb startRgb  = surface.pixel(start);
			if (action == 7 && (startRgb & 0x00FFFFFF) != (borderRgb & 0x00FFFFFF))
				return eOK;
			if (action == 6 && (startRgb & 0x00FFFFFF) == (borderRgb & 0x00FFFFFF))
				return eOK;
			auto isFillCandidate = [action, borderRgb, fillRgb](const QRgb current)
			{
				const QRgb currentRgb  = current & 0x00FFFFFF;
				const QRgb borderRgb24 = borderRgb & 0x00FFFFFF;
				const QRgb fillRgb24   = fillRgb & 0x00FFFFFF;
				if (action == 6)
					return currentRgb != borderRgb24 && currentRgb != fillRgb24;
				return currentRgb == borderRgb24 && currentRgb != fillRgb24;
			};
			auto enqueueRowRuns = [&surface, &isFillCandidate](QVector<QPoint> &seeds, const int y,
			                                                   const int left, const int right)
			{
				if (y < 0 || y >= surface.height())
					return;
				const auto *line = reinterpret_cast<const QRgb *>(surface.constScanLine(y));
				bool        inRun{false};
				for (int x = qMax(0, left); x <= qMin(right, surface.width() - 1); ++x)
				{
					if (isFillCandidate(line[x]))
					{
						if (!inRun)
							seeds.push_back(QPoint(x, y));
						inRun = true;
					}
					else
					{
						inRun = false;
					}
				}
			};

			QVector<QPoint> seeds;
			seeds.reserve(qMin(surface.width(), 256));
			seeds.push_back(start);
			const QRgb replacementRgb = qRgba(fillColour.red(), fillColour.green(), fillColour.blue(), 0xFF);
			while (!seeds.isEmpty())
			{
				const QPoint p = seeds.back();
				seeds.pop_back();
				if (!surface.rect().contains(p))
					continue;
				auto *line = reinterpret_cast<QRgb *>(surface.scanLine(p.y()));
				if (!isFillCandidate(line[p.x()]))
					continue;

				int leftEdge = p.x();
				while (leftEdge > 0 && isFillCandidate(line[leftEdge - 1]))
					--leftEdge;
				int rightEdge = p.x();
				while (rightEdge + 1 < surface.width() && isFillCandidate(line[rightEdge + 1]))
					++rightEdge;

				for (int x = leftEdge; x <= rightEdge; ++x)
					line[x] = replacementRgb;

				enqueueRowRuns(seeds, p.y() - 1, leftEdge, rightEdge);
				enqueueRowRuns(seeds, p.y() + 1, leftEdge, rightEdge);
			}
			return eOK;
		}
		default:
			return eUnknownOption;
		}
	}

	int circleOp(MiniWindow &window, const int action, const int left, const int top, const int right,
	             const int bottom, const long penColour, const long penStyle, const int penWidth,
	             const long brushColour, const long brushStyle, const int extra1, const int extra2,
	             const int extra3, const int extra4)
	{
		if (validatePenStyle(penStyle, penWidth) != eOK)
			return ePenStyleNotValid;
		if (validateBrushStyle(brushStyle) != eOK)
			return eBrushStyleNotValid;
		if (window.backingSurfaceIsNull())
			return eOK;

		const QRect rect = rectFromCoords(window, left, top, right, bottom);
		QPainter    painter(&Internal::mutableBackingSurface(window));
		painter.setPen(penStyle == 5 ? Qt::NoPen : makePen(penColour, penStyle, penWidth));
		bool         brushOk = true;
		const QBrush brush   = makeBrush(brushStyle, penColour, brushColour, &brushOk);
		if (!brushOk)
			return eBrushStyleNotValid;
		painter.setBrush(brush);
		switch (action)
		{
		case 1:
			painter.drawEllipse(rect);
			return eOK;
		case 2:
			painter.drawRect(rect.adjusted(0, 0, -1, -1));
			return eOK;
		case 3:
			painter.drawRoundedRect(rect, extra1, extra2, Qt::AbsoluteSize);
			return eOK;
		case 4:
		case 5:
		{
			const QPointF center     = rect.center();
			const double  startAngle = QLineF(center, QPointF(extra1, extra2)).angle();
			const double  endAngle   = QLineF(center, QPointF(extra3, extra4)).angle();
			double        span       = endAngle - startAngle;
			if (span <= 0)
				span += 360.0;
			const int start  = static_cast<int>(startAngle * 16.0);
			const int span16 = static_cast<int>(span * 16.0);
			if (action == 4)
				painter.drawChord(rect, start, span16);
			else
				painter.drawPie(rect, start, span16);
			return eOK;
		}
		default:
			return eUnknownOption;
		}
	}

	int line(MiniWindow &window, const int x1, const int y1, const int x2, const int y2, const long penColour,
	         const long penStyle, const int penWidth)
	{
		if (validatePenStyle(penStyle, penWidth) != eOK)
			return ePenStyleNotValid;
		if (window.backingSurfaceIsNull())
			return eOK;
		QPainter painter(&Internal::mutableBackingSurface(window));
		painter.setPen(penStyle == 5 ? Qt::NoPen : makePen(penColour, penStyle, penWidth));
		painter.drawLine(x1, y1, x2, y2);
		return eOK;
	}

	int arc(MiniWindow &window, const int left, const int top, const int right, const int bottom,
	        const int x1, const int y1, const int x2, const int y2, const long penColour, const long penStyle,
	        const int penWidth)
	{
		if (validatePenStyle(penStyle, penWidth) != eOK)
			return ePenStyleNotValid;
		if (window.backingSurfaceIsNull())
			return eOK;
		const QRect rect = rectFromCoords(window, left, top, right, bottom);
		QPainter    painter(&Internal::mutableBackingSurface(window));
		painter.setPen(penStyle == 5 ? Qt::NoPen : makePen(penColour, penStyle, penWidth));
		const QPointF center     = rect.center();
		const int     endX       = window.fixRight(x2);
		const int     endY       = window.fixBottom(y2);
		const double  startAngle = QLineF(center, QPointF(x1, y1)).angle();
		const double  endAngle   = QLineF(center, QPointF(endX, endY)).angle();
		double        span       = endAngle - startAngle;
		if (span <= 0)
			span += 360.0;
		painter.drawArc(rect, static_cast<int>(startAngle * 16.0), static_cast<int>(span * 16.0));
		return eOK;
	}

	QStringList splitLegacyPointList(QString points)
	{
		points = points.trimmed();
		QStringList parts;
		if (points.isEmpty())
			return parts;

		while (!points.isEmpty())
		{
			const qsizetype delimiter = points.indexOf(QLatin1Char(','));
			if (delimiter < 0)
				break;
			QString part = points.left(delimiter);
			points       = points.mid(delimiter + 1);
			parts.push_back(part.trimmed());
			points = points.trimmed();
		}
		if (!points.isEmpty())
			parts.push_back(points);
		return parts;
	}

	bool parseLegacySignedIntegerPoint(const QString &text, qreal &value)
	{
		if (text.isEmpty())
			return false;

		qsizetype pos = 0;
		if (text.at(0) == QLatin1Char('+') || text.at(0) == QLatin1Char('-'))
		{
			++pos;
			if (pos == text.size())
				return false;
		}
		for (; pos < text.size(); ++pos)
		{
			const QChar ch = text.at(pos);
			if (ch < QLatin1Char('0') || ch > QLatin1Char('9'))
				return false;
		}

		bool            ok     = false;
		const qlonglong parsed = text.toLongLong(&ok);
		if (!ok || parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max())
			return false;
		value = static_cast<qreal>(parsed);
		return true;
	}

	int parseLegacyPointPairs(const QString &points, const int minimumParts, const int moduloBase,
	                          const int moduloRemainder, QVector<QPointF> &out)
	{
		const QStringList parts = splitLegacyPointList(points);
		if (parts.size() < minimumParts || (parts.size() % moduloBase) != moduloRemainder)
			return eInvalidNumberOfPoints;

		out.clear();
		out.reserve(parts.size() / 2);
		for (int i = 0; i < parts.size(); i += 2)
		{
			qreal x = 0;
			qreal y = 0;
			if (!parseLegacySignedIntegerPoint(parts.at(i), x) ||
			    !parseLegacySignedIntegerPoint(parts.at(i + 1), y))
			{
				return eInvalidPoint;
			}
			out.push_back(QPointF(x, y));
		}
		return eOK;
	}

	int bezier(MiniWindow &window, const QString &points, const long penColour, const long penStyle,
	           const int penWidth)
	{
		if (validatePenStyle(penStyle, penWidth) != eOK)
			return ePenStyleNotValid;
		QVector<QPointF> pts;
		if (const int result = parseLegacyPointPairs(points, 8, 6, 2, pts); result != eOK)
			return result;
		QPainter painter(&Internal::mutableBackingSurface(window));
		painter.setPen(penStyle == 5 ? Qt::NoPen : makePen(penColour, penStyle, penWidth));
		painter.setBrush(Qt::NoBrush);
		QPainterPath path;
		path.moveTo(pts[0]);
		for (int i = 1; i + 2 < pts.size(); i += 3)
			path.cubicTo(pts[i], pts[i + 1], pts[i + 2]);
		painter.drawPath(path);
		return eOK;
	}

	int polygon(MiniWindow &window, const QString &points, const long penColour, const long penStyle,
	            const int penWidth, const long brushColour, const long brushStyle, const bool closePolygon,
	            const bool winding)
	{
		if (validatePenStyle(penStyle, penWidth) != eOK)
			return ePenStyleNotValid;
		if (validateBrushStyle(brushStyle) != eOK)
			return eBrushStyleNotValid;
		QVector<QPointF> pts;
		if (const int result = parseLegacyPointPairs(points, 4, 2, 0, pts); result != eOK)
			return result;
		bool         brushOk = true;
		const QBrush brush   = makeBrush(brushStyle, penColour, brushColour, &brushOk);
		if (!brushOk)
			return eBrushStyleNotValid;
		QPainter painter(&Internal::mutableBackingSurface(window));
		painter.setPen(penStyle == 5 ? Qt::NoPen : makePen(penColour, penStyle, penWidth));
		painter.setBrush(brush);
		if (closePolygon)
			painter.drawPolygon(pts.constData(), static_cast<int>(pts.size()),
			                    winding ? Qt::WindingFill : Qt::OddEvenFill);
		else
			painter.drawPolyline(pts.constData(), static_cast<int>(pts.size()));
		return eOK;
	}

	int gradient(MiniWindow &window, const int left, const int top, const int right, const int bottom,
	             const long startColour, const long endColour, const int mode)
	{
		if (window.backingSurfaceIsNull())
			return eOK;
		const QRect rect = rectFromCoords(window, left, top, right, bottom);
		if (rect.width() <= 0 || rect.height() <= 0)
			return eOK;
		if (mode < 1 || mode > 3)
			return eUnknownOption;
		QPainter painter(&Internal::mutableBackingSurface(window));
		if (mode == 3)
		{
			QImage       texture(rect.size(), QImage::Format_ARGB32);
			const QColor mult = colorFromRef(startColour);
			for (int y = 0; y < texture.height(); ++y)
			{
				auto *line = reinterpret_cast<QRgb *>(texture.scanLine(y));
				for (int x = 0; x < texture.width(); ++x)
				{
					const int c = (x ^ y) & 0xFF;
					line[x] =
					    qRgb((c * mult.red()) & 0xFF, (c * mult.green()) & 0xFF, (c * mult.blue()) & 0xFF);
				}
			}
			painter.drawImage(rect.topLeft(), texture);
			return eOK;
		}
		QLinearGradient gradient;
		if (mode == 2)
			gradient = QLinearGradient(rect.left(), rect.top(), rect.left(), rect.bottom());
		else
			gradient = QLinearGradient(rect.left(), rect.top(), rect.right(), rect.top());
		gradient.setColorAt(0.0, colorFromRef(startColour));
		gradient.setColorAt(1.0, colorFromRef(endColour));
		painter.fillRect(rect, gradient);
		return eOK;
	}

	int text(MiniWindow &window, const QString &fontId, const QString &text, const int left, const int top,
	         const int right, const int bottom, const long colour)
	{
		const auto it = window.fonts.constFind(fontId);
		if (it == window.fonts.constEnd())
			return -2;
		if (window.backingSurfaceIsNull())
			return eOK;
		if (text.isEmpty())
			return 0;
		const QRect rect = rectFromCoords(window, left, top, right, bottom);
		if (rect.width() <= 0 || rect.height() <= 0)
			return eOK;
		QImage  &surface = Internal::mutableBackingSurface(window);
		QPainter painter(&surface);
		painter.setFont(it.value().font);
		painter.setPen(colorFromRef(colour));
		painter.setClipRect(rect);
		const QFontMetrics metrics(it.value().font, &surface);
		const int          baseline = rect.top() + metrics.ascent();
		int                x        = rect.left();
		forEachTextCluster(text,
		                   [&metrics, &painter, &rect, &x, baseline](const QString &cluster)
		                   {
			                   if (x < rect.right() + 1)
				                   painter.drawText(QPoint(x, baseline), cluster);
			                   x += qMax(0, metrics.horizontalAdvance(cluster));
		                   });
		return qMin(qMax(0, x - rect.left()), rect.width());
	}

	void createImage(MiniWindow &window, const QString &imageId, const long row1, const long row2,
	                 const long row3, const long row4, const long row5, const long row6, const long row7,
	                 const long row8)
	{
		const long      rows[8] = {row1, row2, row3, row4, row5, row6, row7, row8};
		MiniWindowImage image;
		image.image = QImage(8, 8, QImage::Format_ARGB32);
		for (int y = 0; y < 8; ++y)
		{
			for (int x = 0; x < 8; ++x)
			{
				const bool bit = (rows[y] >> (7 - x)) & 0x01;
				image.image.setPixel(x, y, bit ? qRgba(255, 255, 255, 255) : qRgba(0, 0, 0, 255));
			}
		}
		setImageBitmapMetadata(image, 1, monochromeBitmapWidthBytes(8));
		window.images.insert(imageId, image);
	}

	bool loadImage(MiniWindow &window, const QString &imageId, const QString &filename)
	{
		window.images.remove(imageId);
		const QString trimmed = filename.trimmed();
		if (trimmed.isEmpty())
			return true;
		QImage image;
		if (!image.load(trimmed))
			return false;
		MiniWindowImage entry;
		entry.image                = image.convertToFormat(QImage::Format_ARGB32);
		entry.source               = trimmed;
		const QString lower        = trimmed.toLower();
		const int     bitsPerPixel = lower.endsWith(QStringLiteral(".png"))
		                                 ? bitmapBitsPerPixelFromFormat(image)
		                                 : bitmapBitsPerPixelFromBmpHeader(trimmed, image);
		setImageBitmapMetadata(entry, bitsPerPixel, bitmapWidthBytes(image.width(), bitsPerPixel));
		window.images.insert(imageId, entry);
		return true;
	}

	bool loadImageMemory(MiniWindow &window, const QString &imageId, const QByteArray &data,
	                     const bool swapAlpha)
	{
		window.images.remove(imageId);
		if (!hasPngSignature(data))
			return false;
		QImage decoded;
		bool   sourceHasAlpha = false;
		bool   monochrome     = false;
		{
			QMutexLocker locker(&memoryImageDecodeCacheMutex());
			qmudLookupMemoryImageDecodeCache(memoryImageDecodeCache(), data, decoded, sourceHasAlpha,
			                                 monochrome);
		}
		if (decoded.isNull())
		{
			decoded = QImage::fromData(data, "PNG");
			if (decoded.isNull())
				return false;
			sourceHasAlpha = decoded.hasAlphaChannel();
			monochrome     = false;
			QMutexLocker locker(&memoryImageDecodeCacheMutex());
			qmudInsertMemoryImageDecodeCache(memoryImageDecodeCache(), memoryImageDecodeCacheBytes(), data,
			                                 decoded, sourceHasAlpha, monochrome, 64, 64 * 1024 * 1024);
		}
		Q_UNUSED(monochrome);
		QImage image = decoded.convertToFormat(QImage::Format_ARGB32);
		if (swapAlpha && sourceHasAlpha)
		{
			QImage swapped(image.size(), QImage::Format_ARGB32);
			for (int y = 0; y < image.height(); ++y)
			{
				const auto *srcLine = reinterpret_cast<const QRgb *>(image.constScanLine(y));
				auto       *dstLine = reinterpret_cast<QRgb *>(swapped.scanLine(y));
				for (int x = 0; x < image.width(); ++x)
				{
					const QRgb pixel = srcLine[x];
					dstLine[x]       = qRgba(qGreen(pixel), qBlue(pixel), qAlpha(pixel), qRed(pixel));
				}
			}
			image = swapped;
		}
		MiniWindowImage entry;
		entry.image            = image;
		entry.source           = QStringLiteral("<memory>");
		const int bitsPerPixel = sourceHasAlpha ? 32 : 24;
		setImageBitmapMetadata(entry, bitsPerPixel, bitmapWidthBytes(image.width(), bitsPerPixel));
		window.images.insert(imageId, entry);
		return true;
	}

	void imageFromWindow(MiniWindow &window, const QString &imageId, const MiniWindow &sourceWindow)
	{
		MiniWindowImage entry;
		entry.image  = surfaceLogicalImage(sourceWindow);
		entry.source = sourceWindow.name;
		setImageBitmapMetadata(entry, 24, bitmapWidthBytes(entry.image.width(), 24));
		window.images.insert(imageId, entry);
	}

	int drawImage(MiniWindow &window, const QString &imageId, const int left, const int top, const int right,
	              const int bottom, const int mode, const int srcLeft, const int srcTop, const int srcRight,
	              const int srcBottom)
	{
		const auto it = window.images.constFind(imageId);
		if (it == window.images.constEnd())
			return eImageNotInstalled;
		if (window.backingSurfaceIsNull() || it.value().image.isNull())
			return eOK;
		const QImage &image = it.value().image;
		QRect         source(srcLeft, srcTop, srcRight - srcLeft, srcBottom - srcTop);
		if (srcRight <= 0)
			source.setRight(image.width() + srcRight - 1);
		if (srcBottom <= 0)
			source.setBottom(image.height() + srcBottom - 1);
		source = source.intersected(image.rect());
		if (source.width() <= 0 || source.height() <= 0)
			return eOK;
		QPainter painter(&Internal::mutableBackingSurface(window));
		if (mode == 2)
		{
			const QRect target = rectFromCoords(window, left, top, right, bottom);
			if (target.width() <= 0 || target.height() <= 0)
				return eOK;
			painter.drawImage(target, image, source);
		}
		else if (mode == 3)
		{
			QImage     copy = image.copy(source).convertToFormat(QImage::Format_ARGB32);
			const auto key  = QColor(image.pixel(0, 0));
			for (int y = 0; y < copy.height(); ++y)
			{
				auto *line = reinterpret_cast<QRgb *>(copy.scanLine(y));
				for (int x = 0; x < copy.width(); ++x)
				{
					if ((line[x] & 0x00FFFFFF) == (key.rgb() & 0x00FFFFFF))
						line[x] &= 0x00FFFFFF;
				}
			}
			painter.drawImage(QPoint(left, top), copy);
		}
		else if (mode == 1)
			painter.drawImage(QPoint(left, top), image.copy(source));
		else
			return eBadParameter;
		return eOK;
	}

	int drawImageAlpha(MiniWindow &window, const QString &imageId, const int left, const int top,
	                   const int right, const int bottom, const double opacity, const int srcLeft,
	                   const int srcTop)
	{
		if (opacity < 0.0 || opacity > 1.0)
			return eBadParameter;
		const auto it = window.images.constFind(imageId);
		if (it == window.images.constEnd())
			return eImageNotInstalled;
		if (!it.value().hasAlpha)
			return eImageNotInstalled;
		if (window.backingSurfaceIsNull() || it.value().image.isNull())
			return eOK;
		const QImage &image      = it.value().image;
		int           drawLeft   = left;
		int           drawTop    = top;
		int           drawRight  = right;
		int           drawBottom = bottom;
		if (drawLeft < 0)
			drawLeft = 0;
		if (drawTop < 0)
			drawTop = 0;
		if (drawRight > window.width)
			drawRight = window.width;
		if (drawBottom > window.height)
			drawBottom = window.height;
		const int sourceLeft   = qMax(0, srcLeft);
		const int sourceTop    = qMax(0, srcTop);
		int       targetWidth  = window.fixRight(drawRight) - drawLeft;
		int       targetHeight = window.fixBottom(drawBottom) - drawTop;
		if (drawLeft >= window.width || drawTop >= window.height)
			return eOK;
		targetWidth  = qMin(targetWidth, window.width - drawLeft);
		targetHeight = qMin(targetHeight, window.height - drawTop);
		if (targetWidth <= 0 || targetHeight <= 0)
			return eOK;
		QRect source(sourceLeft, sourceTop, targetWidth, targetHeight);
		source = source.intersected(image.rect());
		if (source.width() <= 0 || source.height() <= 0)
			return eOK;
		const QSize logicalSize(source.width(), source.height());
		QImage      base =
		    copySurfacePhysicalLayer(window, QRect(drawLeft, drawTop, source.width(), source.height()));
		const QImage overlay =
		    physicalLayerForImage(image, source, logicalSize, surfaceDevicePixelRatio(window));
		const int w = qMin(base.width(), overlay.width());
		const int h = qMin(base.height(), overlay.height());
		for (int y = 0; y < h; ++y)
		{
			auto       *baseLine = reinterpret_cast<QRgb *>(base.scanLine(y));
			const auto *overLine = reinterpret_cast<const QRgb *>(overlay.constScanLine(y));
			for (int x = 0; x < w; ++x)
			{
				const QColor a        = QColor::fromRgba(overLine[x]);
				const QColor b        = QColor::fromRgba(baseLine[x]);
				const int    mask     = a.alpha();
				const int    blendedR = (a.red() * mask + b.red() * (255 - mask)) / 255;
				const int    blendedG = (a.green() * mask + b.green() * (255 - mask)) / 255;
				const int    blendedB = (a.blue() * mask + b.blue() * (255 - mask)) / 255;
				const int    outR =
                    (opacity < 1.0) ? QMudBlend::simpleOpacity(b.red(), blendedR, opacity) : blendedR;
				const int outG =
				    (opacity < 1.0) ? QMudBlend::simpleOpacity(b.green(), blendedG, opacity) : blendedG;
				const int outB =
				    (opacity < 1.0) ? QMudBlend::simpleOpacity(b.blue(), blendedB, opacity) : blendedB;
				baseLine[x] = qRgba(clampByte(outR), clampByte(outG), clampByte(outB), 0xFF);
			}
		}
		QPainter painter(&Internal::mutableBackingSurface(window));
		painter.drawImage(QPoint(drawLeft, drawTop), base);
		return eOK;
	}

	int imageOp(MiniWindow &window, const int action, const int left, const int top, const int right,
	            const int bottom, const long penColour, const long penStyle, const int penWidth,
	            const long brushColour, const QString &imageId, const int ellipseWidth,
	            const int ellipseHeight)
	{
		const auto it = window.images.constFind(imageId);
		if (it == window.images.constEnd())
			return eImageNotInstalled;
		if (validatePenStyle(penStyle, penWidth) != eOK)
			return ePenStyleNotValid;
		if (window.backingSurfaceIsNull())
			return eOK;
		QImage pattern = it.value().image;
		if (pattern.isNull())
			return eOK;
		if (it.value().monochrome)
		{
			const QColor fore = colorFromRefOrTransparent(brushColour);
			const QColor back = colorFromRefOrTransparent(penColour);
			QImage       recoloured(pattern.size(), QImage::Format_ARGB32);
			for (int y = 0; y < pattern.height(); ++y)
			{
				for (int x = 0; x < pattern.width(); ++x)
				{
					const QColor sample(pattern.pixel(x, y));
					const bool   bit = qGray(sample.rgb()) > 127;
					recoloured.setPixel(x, y, (bit ? fore : back).rgba());
				}
			}
			pattern = recoloured;
		}
		QPainter painter(&Internal::mutableBackingSurface(window));
		painter.setPen(penStyle == 5 ? Qt::NoPen : makePen(penColour, penStyle, penWidth));
		QBrush brush;
		brush.setTextureImage(pattern);
		painter.setBrush(brush);
		const QRect rect = rectFromCoords(window, left, top, right, bottom);
		switch (action)
		{
		case 1:
			painter.drawEllipse(rect);
			return eOK;
		case 2:
			painter.drawRect(rect.adjusted(0, 0, -1, -1));
			return eOK;
		case 3:
			painter.drawRoundedRect(rect, ellipseWidth, ellipseHeight, Qt::AbsoluteSize);
			return eOK;
		default:
			return eUnknownOption;
		}
	}

	int mergeImageAlpha(MiniWindow &window, const QString &imageId, const QString &maskId, const int left,
	                    const int top, const int right, const int bottom, const int mode,
	                    const double opacity, const int srcLeft, const int srcTop, const int srcRight,
	                    const int srcBottom)
	{
		if (opacity < 0.0 || opacity > 1.0)
			return eBadParameter;
		const auto imageIt = window.images.constFind(imageId);
		const auto maskIt  = window.images.constFind(maskId);
		if (imageIt == window.images.constEnd() || maskIt == window.images.constEnd())
			return eImageNotInstalled;
		if (window.backingSurfaceIsNull())
			return eOK;
		const QImage &image        = imageIt.value().image;
		const QImage &mask         = maskIt.value().image;
		const int     fixedRight   = window.fixRight(right);
		const int     fixedBottom  = window.fixBottom(bottom);
		const int     targetLeft   = qMax(0, left);
		const int     targetTop    = qMax(0, top);
		const int     targetRight  = qMin(fixedRight, window.width);
		const int     targetBottom = qMin(fixedBottom, window.height);
		const int     targetWidth  = targetRight - targetLeft;
		const int     targetHeight = targetBottom - targetTop;
		if (targetWidth <= 0 || targetHeight <= 0)
			return eOK;
		const int sourceLeft   = qMax(0, srcLeft);
		const int sourceTop    = qMax(0, srcTop);
		int       sourceRight  = srcRight <= 0 ? image.width() + srcRight : srcRight;
		int       sourceBottom = srcBottom <= 0 ? image.height() + srcBottom : srcBottom;
		sourceRight            = qMin(sourceRight, image.width());
		sourceBottom           = qMin(sourceBottom, image.height());
		const int width        = qMin(targetWidth, sourceRight - sourceLeft);
		const int height       = qMin(targetHeight, sourceBottom - sourceTop);
		if (width <= 0 || height <= 0)
			return eOK;
		if (mask.width() < sourceLeft + width || mask.height() < sourceTop + height)
			return eBadParameter;
		if (mode != 0 && mode != 1)
			return eUnknownOption;
		const QSize  logicalSize(width, height);
		QImage       base = copySurfacePhysicalLayer(window, QRect(targetLeft, targetTop, width, height));
		const QRect  sourceRect(sourceLeft, sourceTop, width, height);
		const QImage src =
		    physicalLayerForImage(image, sourceRect, logicalSize, surfaceDevicePixelRatio(window));
		const QImage maskCopy =
		    physicalLayerForImage(mask, sourceRect, logicalSize, surfaceDevicePixelRatio(window));
		const QColor opaqueColor(image.pixel(0, 0));
		const int    physicalWidth  = qMin(base.width(), qMin(src.width(), maskCopy.width()));
		const int    physicalHeight = qMin(base.height(), qMin(src.height(), maskCopy.height()));
		for (int y = 0; y < physicalHeight; ++y)
		{
			auto       *baseLine = reinterpret_cast<QRgb *>(base.scanLine(y));
			const auto *srcLine  = reinterpret_cast<const QRgb *>(src.constScanLine(y));
			const auto *maskLine = reinterpret_cast<const QRgb *>(maskCopy.constScanLine(y));
			for (int x = 0; x < physicalWidth; ++x)
			{
				const QColor a(srcLine[x]);
				const QColor b(baseLine[x]);
				const QColor m(maskLine[x]);
				int          rA = a.red();
				int          gA = a.green();
				int          bA = a.blue();
				if (mode == 1 && a.red() == opaqueColor.red() && a.green() == opaqueColor.green() &&
				    a.blue() == opaqueColor.blue())
				{
					rA = b.red();
					gA = b.green();
					bA = b.blue();
				}
				const auto blendMask = [](const int A, const int B, const int M)
				{ return (A * M + B * (255 - M)) / 255; };
				const int rawR = blendMask(rA, b.red(), m.red());
				const int rawG = blendMask(gA, b.green(), m.green());
				const int rawB = blendMask(bA, b.blue(), m.blue());
				const int outR = opacity < 1.0 ? QMudBlend::simpleOpacity(b.red(), rawR, opacity) : rawR;
				const int outG = opacity < 1.0 ? QMudBlend::simpleOpacity(b.green(), rawG, opacity) : rawG;
				const int outB = opacity < 1.0 ? QMudBlend::simpleOpacity(b.blue(), rawB, opacity) : rawB;
				baseLine[x]    = qRgba(clampByte(outR), clampByte(outG), clampByte(outB), 0xFF);
			}
		}
		QPainter painter(&Internal::mutableBackingSurface(window));
		painter.drawImage(QPoint(targetLeft, targetTop), base);
		return eOK;
	}

	int getImageAlpha(MiniWindow &window, const QString &imageId, const int left, const int top,
	                  const int right, const int bottom, const int srcLeft, const int srcTop)
	{
		const auto it = window.images.constFind(imageId);
		if (it == window.images.constEnd())
			return eImageNotInstalled;
		if (!it.value().hasAlpha)
			return eImageNotInstalled;
		if (window.backingSurfaceIsNull() || it.value().image.isNull())
			return eOK;
		const int fixedRight   = window.fixRight(right);
		const int fixedBottom  = window.fixBottom(bottom);
		const int targetLeft   = qMax(0, left);
		const int targetTop    = qMax(0, top);
		const int targetRight  = qMin(fixedRight, window.width);
		const int targetBottom = qMin(fixedBottom, window.height);
		const int targetWidth  = targetRight - targetLeft;
		const int targetHeight = targetBottom - targetTop;
		if (targetWidth <= 0 || targetHeight <= 0)
			return eOK;
		QRect source(qMax(0, srcLeft), qMax(0, srcTop), targetWidth, targetHeight);
		if (srcLeft < 0)
			source.moveLeft(0);
		if (srcTop < 0)
			source.moveTop(0);
		source = source.intersected(it.value().image.rect());
		if (source.width() <= 0 || source.height() <= 0)
			return eOK;
		QImage alphaImage(source.size(), QImage::Format_ARGB32);
		for (int y = 0; y < source.height(); ++y)
		{
			for (int x = 0; x < source.width(); ++x)
			{
				const int alpha = qAlpha(it.value().image.pixel(source.x() + x, source.y() + y));
				alphaImage.setPixel(x, y, qRgba(alpha, alpha, alpha, 0xFF));
			}
		}
		QPainter painter(&Internal::mutableBackingSurface(window));
		painter.drawImage(QPoint(targetLeft, targetTop), alphaImage);
		return eOK;
	}

	int blendImage(MiniWindow &window, const QString &imageId, const int left, const int top, const int right,
	               const int bottom, const int mode, const double opacity, const int srcLeft,
	               const int srcTop, const int srcRight, const int srcBottom, const RandomUnit &randomUnit)
	{
		const auto it = window.images.constFind(imageId);
		if (it == window.images.constEnd())
			return eImageNotInstalled;
		if (opacity < 0.0 || opacity > 1.0)
			return eBadParameter;

		QImage       &surface = Internal::mutableBackingSurface(window);
		const QImage &image   = it.value().image;
		if (surface.isNull() || image.isNull())
			return eOK;

		const int fixedRight   = window.fixRight(right);
		const int fixedBottom  = window.fixBottom(bottom);
		const int targetLeft   = qMax(0, left);
		const int targetTop    = qMax(0, top);
		const int targetRight  = qMin(fixedRight, window.width);
		const int targetBottom = qMin(fixedBottom, window.height);
		const int targetWidth  = targetRight - targetLeft;
		const int targetHeight = targetBottom - targetTop;
		if (targetWidth <= 0 || targetHeight <= 0)
			return eOK;

		const int sourceLeft = qMax(0, srcLeft);
		const int sourceTop  = qMax(0, srcTop);
		int       fixedSrcRight{srcRight};
		int       fixedSrcBottom{srcBottom};
		if (fixedSrcRight <= 0)
			fixedSrcRight = image.width() + fixedSrcRight;
		if (fixedSrcBottom <= 0)
			fixedSrcBottom = image.height() + fixedSrcBottom;
		fixedSrcRight  = qMin(fixedSrcRight, image.width());
		fixedSrcBottom = qMin(fixedSrcBottom, image.height());

		const int width  = qMin(targetWidth, fixedSrcRight - sourceLeft);
		const int height = qMin(targetHeight, fixedSrcBottom - sourceTop);
		if (width <= 0 || height <= 0)
			return eOK;
		if (mode < 1 || mode > 64)
			return eUnknownOption;

		const QSize  logicalSize(width, height);
		QImage       base  = copySurfacePhysicalLayer(window, QRect(targetLeft, targetTop, width, height));
		const QImage blend = physicalLayerForImage(image, QRect(sourceLeft, sourceTop, width, height),
		                                           logicalSize, surfaceDevicePixelRatio(window));

		const int    physicalWidth  = qMin(base.width(), blend.width());
		const int    physicalHeight = qMin(base.height(), blend.height());
		for (int y = 0; y < physicalHeight; ++y)
		{
			auto       *baseLine  = reinterpret_cast<QRgb *>(base.scanLine(y));
			const auto *blendLine = reinterpret_cast<const QRgb *>(blend.constScanLine(y));
			for (int x = 0; x < physicalWidth; ++x)
			{
				const long blendPixel = static_cast<long>(qRed(blendLine[x])) |
				                        (static_cast<long>(qGreen(blendLine[x])) << 8) |
				                        (static_cast<long>(qBlue(blendLine[x])) << 16);
				const long basePixel = static_cast<long>(qRed(baseLine[x])) |
				                       (static_cast<long>(qGreen(baseLine[x])) << 8) |
				                       (static_cast<long>(qBlue(baseLine[x])) << 16);
				long result{0};
				if (!blendPixelInternal(blendPixel, basePixel, static_cast<short>(mode), opacity, result,
				                        randomUnit))
					return eUnknownOption;
				baseLine[x] =
				    qRgba(lowByteToInt(result), lowByteToInt(result >> 8), lowByteToInt(result >> 16), 0xFF);
			}
		}

		QPainter painter(&surface);
		painter.drawImage(QPoint(targetLeft, targetTop), base);
		return eOK;
	}

	int filter(MiniWindow &window, const int left, const int top, const int right, const int bottom,
	           const int operation, const double options, const RandomUnit &randomUnit)
	{
		QImage &surface = Internal::mutableBackingSurface(window);
		if (surface.isNull())
			return eOK;

		const int fixedRight   = window.fixRight(right);
		const int fixedBottom  = window.fixBottom(bottom);
		const int targetLeft   = qMax(0, left);
		const int targetTop    = qMax(0, top);
		const int targetRight  = qMin(fixedRight, window.width);
		const int targetBottom = qMin(fixedBottom, window.height);
		const int width        = targetRight - targetLeft;
		const int height       = targetBottom - targetTop;
		if (width <= 0 || height <= 0)
			return eOK;

		QImage     copy = copySurfacePhysicalLayer(window, QRect(targetLeft, targetTop, width, height));
		const int  physicalWidth  = copy.width();
		const int  physicalHeight = copy.height();
		const int  bpl            = bytesPerLine24(physicalWidth);
		QByteArray buffer;
		imageToBgrBuffer(copy, buffer, physicalWidth, physicalHeight, bpl);

		switch (operation)
		{
		case 1:
			noise(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth, physicalHeight, bpl,
			      options, randomUnit);
			break;
		case 2:
			monoNoise(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth, physicalHeight, bpl,
			          options, randomUnit);
			break;
		case 3:
		{
			const double matrix[5] = {1, 1, 1, 1, 1};
			generalFilter(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth, physicalHeight,
			              bpl, options, matrix, 5);
			break;
		}
		case 4:
		{
			constexpr double matrix[5] = {-1, -1, 7, -1, -1};
			generalFilter(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth, physicalHeight,
			              bpl, options, matrix, 3);
			break;
		}
		case 5:
		{
			constexpr double matrix[5] = {0, 2.5, -6, 2.5, 0};
			generalFilter(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth, physicalHeight,
			              bpl, options, matrix, 1);
			break;
		}
		case 6:
		{
			constexpr double matrix[5] = {1, 2, 1, -1, -2};
			generalFilter(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth, physicalHeight,
			              bpl, options, matrix, 1);
			break;
		}
		case 7:
			brightness(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth, physicalHeight, bpl,
			           options);
			break;
		case 8:
			contrast(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth, physicalHeight, bpl,
			         options);
			break;
		case 9:
			gammaAdjust(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth, physicalHeight, bpl,
			            options);
			break;
		case 10:
			colourBrightness(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth, physicalHeight,
			                 bpl, options, 2);
			break;
		case 11:
			colourContrast(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth, physicalHeight,
			               bpl, options, 2);
			break;
		case 12:
			colourGamma(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth, physicalHeight, bpl,
			            options, 2);
			break;
		case 13:
			colourBrightness(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth, physicalHeight,
			                 bpl, options, 1);
			break;
		case 14:
			colourContrast(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth, physicalHeight,
			               bpl, options, 1);
			break;
		case 15:
			colourGamma(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth, physicalHeight, bpl,
			            options, 1);
			break;
		case 16:
			colourBrightness(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth, physicalHeight,
			                 bpl, options, 0);
			break;
		case 17:
			colourContrast(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth, physicalHeight,
			               bpl, options, 0);
			break;
		case 18:
			colourGamma(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth, physicalHeight, bpl,
			            options, 0);
			break;
		case 19:
			makeGreyscale(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth, physicalHeight,
			              bpl, true);
			break;
		case 20:
			makeGreyscale(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth, physicalHeight,
			              bpl, false);
			break;
		case 21:
			brightnessMultiply(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth,
			                   physicalHeight, bpl, options);
			break;
		case 22:
			colourBrightnessMultiply(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth,
			                         physicalHeight, bpl, options, 2);
			break;
		case 23:
			colourBrightnessMultiply(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth,
			                         physicalHeight, bpl, options, 1);
			break;
		case 24:
			colourBrightnessMultiply(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth,
			                         physicalHeight, bpl, options, 0);
			break;
		case 25:
		{
			const double matrix[5] = {0, 1, 1, 1, 0};
			generalFilter(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth, physicalHeight,
			              bpl, options, matrix, 3);
			break;
		}
		case 26:
		{
			constexpr double matrix[5] = {0, 0.5, 1, 0.5, 0};
			generalFilter(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth, physicalHeight,
			              bpl, options, matrix, 2);
			break;
		}
		case 27:
			averageBuffer(reinterpret_cast<unsigned char *>(buffer.data()), physicalWidth, physicalHeight);
			break;
		default:
			return eUnknownOption;
		}

		bgrBufferToImage(buffer, copy, physicalWidth, physicalHeight, bpl);
		QPainter painter(&surface);
		painter.drawImage(QPoint(targetLeft, targetTop), copy);
		return eOK;
	}

	int transformImage(MiniWindow &window, const QString &imageId, const float left, const float top,
	                   const int mode, const float mxx, const float mxy, const float myx, const float myy)
	{
		const auto it = window.images.constFind(imageId);
		if (it == window.images.constEnd())
			return eImageNotInstalled;
		if (window.backingSurfaceIsNull() || it.value().image.isNull())
			return eOK;
		if (mode != 1 && mode != 3)
			return eBadParameter;
		QImage image = it.value().image;
		if (mode == 3)
		{
			image          = image.convertToFormat(QImage::Format_ARGB32);
			const auto key = QColor(image.pixel(0, 0));
			for (int y = 0; y < image.height(); ++y)
			{
				auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
				for (int x = 0; x < image.width(); ++x)
				{
					if ((line[x] & 0x00FFFFFF) == (key.rgb() & 0x00FFFFFF))
						line[x] &= 0x00FFFFFF;
				}
			}
		}
		QPainter painter(&Internal::mutableBackingSurface(window));
		painter.setTransform(QTransform(mxx, myx, mxy, myy, left, top), true);
		painter.drawImage(QPointF(0, 0), image);
		return eOK;
	}

	void resize(MiniWindow &window, const int width, const int height, const long colour)
	{
		if (window.width == width && window.height == height)
			return;
		const double ratio = surfaceDevicePixelRatio(window);
		QImage       newImage(MiniWindow::backingStoreSize(width, height, ratio), QImage::Format_ARGB32);
		newImage.setDevicePixelRatio(ratio);
		const QColor fill = colorFromRef(colour);
		newImage.fill(fill);
		QPainter painter(&newImage);
		painter.drawImage(0, 0, window.backingSurface());
		window.setBackingSurface(newImage);
		window.width  = width;
		window.height = height;
		clearTransparentSurfaceCache(window);
	}

	void position(MiniWindow &window, const int left, const int top, const int position, const int flags)
	{
		window.location = QPoint(left, top);
		window.position = position;
		window.flags    = flags;
		if ((flags & kMiniWindowTransparent) == 0 && !window.transparentSurfaceCache.isNull())
			clearTransparentSurfaceCache(window);
	}

	void setZOrder(MiniWindow &window, const int zOrder)
	{
		window.zOrder = zOrder;
	}

	int addHotspot(MiniWindow &window, const QString &hotspotId, const int left, const int top,
	               const int right, const int bottom, const QString &mouseOver,
	               const QString &cancelMouseOver, const QString &mouseDown, const QString &cancelMouseDown,
	               const QString &mouseUp, const QString &tooltip, const int cursor, const int flags,
	               const QString &pluginId)
	{
		if (!window.callbackPlugin.isEmpty() && window.callbackPlugin != pluginId)
			return eHotspotPluginChanged;
		if (window.callbackPlugin.isEmpty())
			window.callbackPlugin = pluginId;

		if (window.mouseOverHotspot == hotspotId)
			window.mouseOverHotspot.clear();
		if (window.mouseDownHotspot == hotspotId)
			window.mouseDownHotspot.clear();

		MiniWindowHotspot hotspot;
		hotspot.rect            = rectFromCoords(window, left, top, right, bottom);
		hotspot.mouseOver       = mouseOver;
		hotspot.cancelMouseOver = cancelMouseOver;
		hotspot.mouseDown       = mouseDown;
		hotspot.cancelMouseDown = cancelMouseDown;
		hotspot.mouseUp         = mouseUp;
		hotspot.tooltip         = tooltip;
		hotspot.cursor          = cursor;
		hotspot.flags           = flags;
		window.hotspots.insert(hotspotId, hotspot);
		return eOK;
	}

	int deleteHotspot(MiniWindow &window, const QString &hotspotId)
	{
		const auto it = window.hotspots.find(hotspotId);
		if (it == window.hotspots.end())
			return eHotspotNotInstalled;
		window.hotspots.erase(it);
		window.outputGeneratedHotspots.remove(hotspotId);
		if (window.mouseOverHotspot == hotspotId)
			window.mouseOverHotspot.clear();
		if (window.mouseDownHotspot == hotspotId)
			window.mouseDownHotspot.clear();
		if (window.hotspots.isEmpty())
			window.callbackPlugin.clear();
		return eOK;
	}

	void deleteAllHotspots(MiniWindow &window)
	{
		window.hotspots.clear();
		window.outputGeneratedHotspots.clear();
		window.outputHotspotSerial = 0;
		window.mouseOverHotspot.clear();
		window.mouseDownHotspot.clear();
		window.callbackPlugin.clear();
	}

	int setHotspotTooltip(MiniWindow &window, const QString &hotspotId, const QString &tooltip)
	{
		const auto it = window.hotspots.find(hotspotId);
		if (it == window.hotspots.end())
			return eHotspotNotInstalled;
		it.value().tooltip = tooltip;
		return eOK;
	}

	int moveHotspot(MiniWindow &window, const QString &hotspotId, const int left, const int top,
	                const int right, const int bottom)
	{
		const auto it = window.hotspots.find(hotspotId);
		if (it == window.hotspots.end())
			return eHotspotNotInstalled;
		it.value().rect = rectFromCoords(window, left, top, right, bottom);
		return eOK;
	}

	int setHotspotDragHandler(MiniWindow &window, const QString &hotspotId, const QString &moveCallback,
	                          const QString &releaseCallback, const int flags, const QString &pluginId)
	{
		if (!window.callbackPlugin.isEmpty() && window.callbackPlugin != pluginId)
			return eHotspotPluginChanged;
		const auto it = window.hotspots.find(hotspotId);
		if (it == window.hotspots.end())
			return eHotspotNotInstalled;
		it.value().moveCallback    = moveCallback;
		it.value().releaseCallback = releaseCallback;
		it.value().dragFlags       = flags;
		return eOK;
	}

	int setHotspotScrollwheelHandler(MiniWindow &window, const QString &hotspotId,
	                                 const QString &moveCallback, const QString &pluginId)
	{
		if (!window.callbackPlugin.isEmpty() && window.callbackPlugin != pluginId)
			return eHotspotPluginChanged;
		const auto it = window.hotspots.find(hotspotId);
		if (it == window.hotspots.end())
			return eHotspotNotInstalled;
		it.value().scrollwheelCallback = moveCallback;
		return eOK;
	}

	void clearGeneratedOutputHotspots(MiniWindow &window, const QString &hotspotPrefix)
	{
		const QString normalizedPrefix =
		    hotspotPrefix.trimmed().isEmpty() ? QStringLiteral("output_link") : hotspotPrefix.trimmed();
		const QString    scopedPrefix = normalizedPrefix + QLatin1Char('_');
		QVector<QString> scopedGeneratedIds;
		scopedGeneratedIds.reserve(window.outputGeneratedHotspots.size());
		for (const QString &generatedHotspotId : std::as_const(window.outputGeneratedHotspots))
		{
			if (!generatedHotspotId.startsWith(scopedPrefix))
				continue;
			window.hotspots.remove(generatedHotspotId);
			if (window.mouseOverHotspot == generatedHotspotId)
				window.mouseOverHotspot.clear();
			if (window.mouseDownHotspot == generatedHotspotId)
				window.mouseDownHotspot.clear();
			scopedGeneratedIds.push_back(generatedHotspotId);
		}
		for (const QString &generatedHotspotId : std::as_const(scopedGeneratedIds))
			window.outputGeneratedHotspots.remove(generatedHotspotId);
		if (window.hotspots.isEmpty())
		{
			window.callbackPlugin.clear();
			window.outputHotspotSerial = 0;
		}
	}

	int addGeneratedOutputHotspot(MiniWindow &window, const QString &hotspotPrefix, const int left,
	                              const int top, const int right, const int bottom, const QString &mouseUp,
	                              const QString &pluginId, const int actionType, const QString &action)
	{
		const QString normalizedPrefix =
		    hotspotPrefix.trimmed().isEmpty() ? QStringLiteral("output_link") : hotspotPrefix.trimmed();
		const QString hotspotId =
		    QStringLiteral("%1_%2").arg(normalizedPrefix).arg(++window.outputHotspotSerial);
		const int result = addHotspot(window, hotspotId, left, top, right, bottom, QString(), QString(),
		                              QString(), QString(), mouseUp, QString(), 1, 0, pluginId);
		if (result != eOK)
			return result;
		if (auto it = window.hotspots.find(hotspotId); it != window.hotspots.end())
		{
			it->outputActionType = actionType;
			it->outputAction     = action;
			window.outputGeneratedHotspots.insert(hotspotId);
		}
		return eOK;
	}

	bool lineFitsVertically(const int y, const int lineHeight, const int rectBottom)
	{
		if (lineHeight <= 0)
			return false;
		const qint64 lineBottom = static_cast<qint64>(y) + static_cast<qint64>(lineHeight) - 1;
		return lineBottom <= static_cast<qint64>(rectBottom);
	}

	bool runNeedsWrap(const int x, const int candidateWidth, const int currentLineWidth, const int rectLeft,
	                  const int rightLimitExclusive)
	{
		const qint64 candidateRightExclusive = static_cast<qint64>(x) + static_cast<qint64>(candidateWidth);
		const qint64 currentRightExclusive   = static_cast<qint64>(x) + static_cast<qint64>(currentLineWidth);
		return candidateRightExclusive > static_cast<qint64>(rightLimitExclusive) &&
		       currentRightExclusive > static_cast<qint64>(rectLeft);
	}

	bool hasActivatableAction(const int actionType, const QString &action, const int actionNoneType)
	{
		return actionType != actionNoneType && !action.trimmed().isEmpty() &&
		       !hasUnresolvedMxpTextEntityReference(action);
	}
} // namespace MiniWindowUtils
