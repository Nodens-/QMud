/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MiniWindow.h
 * Role: Miniwindow data structures and constants used by scripting APIs to create and render custom overlay windows.
 */

#ifndef QMUD_MINIWINDOW_H
#define QMUD_MINIWINDOW_H

#include <QDateTime>
#include <QFontMetrics>
#include <QHash>
#include <QImage>
#include <QMap>
#include <QPoint>
#include <QRect>
#include <QSet>
#include <QSize>
#include <QString>

#include <cmath>
#include <optional>
#include <utility>

struct MiniWindow;

namespace MiniWindowUtils::Internal
{
	QImage &mutableBackingSurface(MiniWindow &window);
}

/**
 * @brief Mouse-interaction callbacks and geometry for one miniwindow hotspot.
 */
struct MiniWindowHotspot
{
		/** Process-local identity retained by snapshots and renewed when an id is replaced. */
		quint64 instanceId{0};
		QRect   rect;
		QString mouseOver;
		QString cancelMouseOver;
		QString mouseDown;
		QString cancelMouseDown;
		QString mouseUp;
		QString tooltip;
		int     cursor{0};
		int     flags{0};
		QString moveCallback;
		QString releaseCallback;
		int     dragFlags{0};
		QString scrollwheelCallback;
		int     outputActionType{0};
		QString outputAction;

		bool    operator==(const MiniWindowHotspot &) const = default;
};

/** Hotspot instance identities grouped by miniwindow and hotspot id. */
using MiniWindowHotspotInstanceMap = QHash<QString, QHash<QString, quint64>>;

/**
 * @brief Final callback-local mutation of one hotspot, guarded by its captured instance identity.
 */
struct MiniWindowHotspotMutation
{
		quint64                          expectedInstanceId{0};
		std::optional<MiniWindowHotspot> beforeHotspot;
		std::optional<MiniWindowHotspot> hotspot;
		bool                             outputGenerated{false};
};

/** Hotspot mutations grouped by miniwindow and hotspot id. */
using MiniWindowHotspotMutationMap = QHash<QString, QHash<QString, MiniWindowHotspotMutation>>;

/**
 * @brief Result of applying one callback-local miniwindow state to the authoritative runtime.
 */
struct MiniWindowCommitOutcome
{
		bool    applied{false};
		bool    exists{false};
		quint64 runtimeId{0};
};

/** Miniwindow commit results keyed by window id. */
using MiniWindowCommitOutcomeMap = QHash<QString, MiniWindowCommitOutcome>;

constexpr int kMiniWindowDrawUnderneath   = 0x01;
constexpr int kMiniWindowAbsoluteLocation = 0x02;
constexpr int kMiniWindowTransparent      = 0x04;
constexpr int kMiniWindowIgnoreMouse      = 0x08;
constexpr int kMiniWindowKeepHotspots     = 0x10;

/**
 * @brief Cached font object and metrics for miniwindow text rendering.
 */
struct MiniWindowFont
{
		QFont        font;
		QFontMetrics metrics{QFont()};
		int          charset{1};
		int          pitchAndFamily{0};

		bool         operator==(const MiniWindowFont &) const = default;
};

/**
 * @brief Image resource metadata stored in a miniwindow.
 */
struct MiniWindowImage
{
		QImage  image;
		QString source;
		bool    hasAlpha{false};
		bool    monochrome{false};
		int     bitmapType{0};
		int     bitmapWidthBytes{0};
		int     bitmapPlanes{1};
		int     bitmapBitsPixel{0};

		bool    operator==(const MiniWindowImage &) const = default;
};

/**
 * @brief Complete miniwindow state container used by scripting/rendering.
 */
struct MiniWindow
{
		/** Process-local identity of this window instance. Renewed after deletion and recreation. */
		quint64                          runtimeId{0};
		/** Authoritative runtime mutation revision used to detect stale callback shadows. */
		quint64                          mutationRevision{0};
		/** Backing-surface revision retained across detached callback snapshots. */
		quint64                          surfaceRevision{0};
		QString                          name;
		int                              width{0};
		int                              height{0};
		int                              position{0};
		int                              flags{0};
		QColor                           background;
		bool                             show{false};
		QPoint                           location;
		QRect                            rect;
		bool                             temporarilyHide{false};
		int                              zOrder{0};
		int                              executingScriptDepth{0};
		bool                             executingScript{false};
		QString                          creatingPlugin;
		QString                          callbackPlugin;
		QString                          mouseOverHotspot;
		QString                          mouseDownHotspot;
		int                              flagsOnMouseDown{0};
		QPoint                           lastMousePosition;
		int                              lastMouseUpdate{0};
		QPoint                           clientMousePosition;
		QDateTime                        installedAt;

		double                           devicePixelRatio{1.0};
		QImage                           transparentSurfaceCache;
		qint64                           transparentSurfaceSourceKey{0};
		QRgb                             transparentSurfaceKeyRgb{0};
		QMap<QString, MiniWindowFont>    fonts;
		QMap<QString, MiniWindowImage>   images;
		QMap<QString, MiniWindowHotspot> hotspots;
		QSet<QString>                    outputGeneratedHotspots;
		quint64                          outputHotspotSerial{0};

		bool                             operator==(const MiniWindow &) const = default;

		/**
		 * @brief Returns a copy with independent image backing stores.
		 *
		 * Miniwindow callback snapshots can be painted on the Lua executor thread, so image data must not
		 * share implicit Qt backing storage with the runtime-side miniwindow.
		 *
		 * @return Miniwindow copy with detached surface/cache/image resources.
		 */
		[[nodiscard]] MiniWindow         detachedImageCopy() const
		{
			MiniWindow copy              = *this;
			copy.m_surface               = m_surface.copy();
			copy.transparentSurfaceCache = transparentSurfaceCache.copy();
			for (MiniWindowImage &image : copy.images)
				image.image = image.image.copy();
			return copy;
		}

		/**
		 * @brief Normalizes a backing-store device pixel ratio.
		 */
		[[nodiscard]] static double normalizedDevicePixelRatio(const double ratio)
		{
			if (!std::isfinite(ratio) || ratio <= 1.0)
				return 1.0;
			return ratio;
		}

		/**
		 * @brief Returns the physical backing-store size for a logical miniwindow size.
		 */
		[[nodiscard]] static QSize backingStoreSize(const int logicalWidth, const int logicalHeight,
		                                            const double ratio)
		{
			const double normalizedRatio = normalizedDevicePixelRatio(ratio);
			const int    physicalWidth   = qMax(
			    1, static_cast<int>(std::ceil(static_cast<double>(qMax(1, logicalWidth)) * normalizedRatio)));
			const int physicalHeight = qMax(
			    1,
			    static_cast<int>(std::ceil(static_cast<double>(qMax(1, logicalHeight)) * normalizedRatio)));
			return {physicalWidth, physicalHeight};
		}

		/**
		 * @brief Returns the logical miniwindow API size.
		 */
		[[nodiscard]] QSize logicalSize() const
		{
			return {qMax(0, width), qMax(0, height)};
		}

		/**
		 * @brief Returns the logical miniwindow API rectangle.
		 */
		[[nodiscard]] QRect logicalRect() const
		{
			return {QPoint(0, 0), logicalSize()};
		}

		/**
		 * @brief Returns the script-visible miniwindow rectangle for WindowInfo APIs.
		 *
		 * Absolute miniwindows keep their plugin coordinate space even when QMud scales their
		 * painted rectangle to fit the current view.
		 */
		[[nodiscard]] QRect apiRect() const
		{
			if ((flags & kMiniWindowAbsoluteLocation) != 0)
				return {location, logicalSize()};
			return rect;
		}

		/**
		 * @brief Returns the high-DPI backing image for painting/compositing.
		 */
		[[nodiscard]] const QImage &backingSurface() const
		{
			return m_surface;
		}

		/**
		 * @brief Returns the revision of the backing pixels.
		 */
		[[nodiscard]] quint64 backingSurfaceRevision() const
		{
			return surfaceRevision;
		}

		/**
		 * @brief Replaces the high-DPI backing image and records its DPR.
		 */
		void setBackingSurface(QImage surface)
		{
			const double normalizedRatio = normalizedDevicePixelRatio(surface.devicePixelRatio());
			surface.setDevicePixelRatio(normalizedRatio);
			devicePixelRatio = normalizedRatio;
			m_surface        = std::move(surface);
			markBackingSurfaceChanged();
		}

		/**
		 * @brief Returns whether the high-DPI backing image is empty.
		 */
		[[nodiscard]] bool backingSurfaceIsNull() const
		{
			return m_surface.isNull();
		}

		/**
		 * @brief Returns the physical high-DPI backing image size.
		 */
		[[nodiscard]] QSize backingSurfaceSize() const
		{
			return m_surface.size();
		}

		/**
		 * @brief Returns the DPR stored on the high-DPI backing image.
		 */
		[[nodiscard]] double backingSurfaceDevicePixelRatio() const
		{
			return normalizedDevicePixelRatio(m_surface.devicePixelRatio());
		}

		/**
		 * @brief Initializes miniwindow geometry, flags, and backing surface.
		 */
		void create(int left, int top, int newWidth, int newHeight, int newPosition, int newFlags,
		            const QColor &newBackground, double newDevicePixelRatio = 1.0)
		{
			location         = QPoint(left, top);
			width            = newWidth;
			height           = newHeight;
			position         = newPosition;
			flags            = newFlags;
			background       = newBackground;
			rect             = QRect(0, 0, 0, 0);
			temporarilyHide  = false;
			show             = false;
			devicePixelRatio = normalizedDevicePixelRatio(newDevicePixelRatio);
			m_surface = QImage(backingStoreSize(width, height, devicePixelRatio), QImage::Format_ARGB32);
			m_surface.setDevicePixelRatio(devicePixelRatio);
			m_surface.fill(background.isValid() ? background : QColor(0, 0, 0));
			markBackingSurfaceChanged();
			transparentSurfaceCache     = QImage();
			transparentSurfaceSourceKey = 0;
			transparentSurfaceKeyRgb    = 0;
			installedAt                 = QDateTime::currentDateTime();
		}

		/**
		 * @brief Normalizes right coordinate where non-positive means relative.
		 */
		[[nodiscard]] int fixRight(int right) const
		{
			if (right <= 0)
				return width + right;
			return right;
		}

		/**
		 * @brief Normalizes bottom coordinate where non-positive means relative.
		 */
		[[nodiscard]] int fixBottom(int bottom) const
		{
			if (bottom <= 0)
				return height + bottom;
			return bottom;
		}

	private:
		friend class WorldRuntime;
		friend QImage        &MiniWindowUtils::Internal::mutableBackingSurface(MiniWindow &window);

		/**
		 * @brief Returns the high-DPI backing image for trusted drawing operations.
		 */
		[[nodiscard]] QImage &mutableBackingSurface()
		{
			markBackingSurfaceChanged();
			return m_surface;
		}

		void markBackingSurfaceChanged()
		{
			if (++surfaceRevision == 0)
				++surfaceRevision;
		}

		QImage m_surface;
};

#endif // QMUD_MINIWINDOW_H
