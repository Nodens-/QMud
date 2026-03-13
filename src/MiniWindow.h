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
#include <QImage>
#include <QMap>
#include <QPoint>
#include <QRect>
#include <QString>

/**
 * @brief Mouse-interaction callbacks and geometry for one miniwindow hotspot.
 */
struct MiniWindowHotspot
{
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
};

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
};

/**
 * @brief Complete miniwindow state container used by scripting/rendering.
 */
struct MiniWindow
{
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

		QImage                           surface;
		QMap<QString, MiniWindowFont>    fonts;
		QMap<QString, MiniWindowImage>   images;
		QMap<QString, MiniWindowHotspot> hotspots;

		/**
		 * @brief Initializes miniwindow geometry, flags, and backing surface.
		 */
		void create(int left, int top, int newWidth, int newHeight, int newPosition, int newFlags,
		            const QColor &newBackground)
		{
			location        = QPoint(left, top);
			width           = newWidth;
			height          = newHeight;
			position        = newPosition;
			flags           = newFlags;
			background      = newBackground;
			rect            = QRect(0, 0, 0, 0);
			temporarilyHide = false;
			show            = false;
			surface         = QImage(qMax(1, width), qMax(1, height), QImage::Format_ARGB32);
			surface.fill(background.isValid() ? background : QColor(0, 0, 0));
			installedAt = QDateTime::currentDateTime();
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
};

#endif // QMUD_MINIWINDOW_H
