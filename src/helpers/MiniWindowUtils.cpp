/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MiniWindowUtils.cpp
 * Role: Shared miniwindow rendering helpers (brush/color plus output layout/action predicates).
 */

#include "MiniWindowUtils.h"

#include <QHash>
#include <QImage>
// ReSharper disable once CppUnusedIncludeDirective
#include <QMutex>
#include <QMutexLocker>
#include <QtGlobal>

#include <array>
#include <list>

namespace
{
	using PatternRows = std::array<quint8, 8>;

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
} // namespace

namespace MiniWindowUtils
{
	QColor colorFromRef(const long value)
	{
		const int r = static_cast<int>(value & 0xFF);
		const int g = static_cast<int>((value >> 8) & 0xFF);
		const int b = static_cast<int>((value >> 16) & 0xFF);
		return {r, g, b};
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
		return actionType != actionNoneType && !action.trimmed().isEmpty();
	}
} // namespace MiniWindowUtils
