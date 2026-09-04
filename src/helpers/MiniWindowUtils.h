/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MiniWindowUtils.h
 * Role: Shared miniwindow rendering helpers (brush/color plus output layout/action predicates).
 */

#pragma once

// ReSharper disable once CppUnusedIncludeDirective
#include <QBrush>
// ReSharper disable once CppUnusedIncludeDirective
#include <QByteArray>
#include <QColor>
#include <QPen>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QVariant>
// ReSharper disable once CppUnusedIncludeDirective
#include <QString>

#include <functional>

struct MiniWindow;

namespace MiniWindowUtils
{
	[[nodiscard]] QColor colorFromRef(long value);
	[[nodiscard]] long   colorToRef(const QColor &color);
	[[nodiscard]] QColor colorFromRefOrTransparent(long value);
	[[nodiscard]] QBrush makeBrush(long brushStyle, long penColour, long brushColour, bool *ok = nullptr);
	[[nodiscard]] QRect  rectFromCoords(const MiniWindow &window, long left, long top, long right,
	                                    long bottom);
	[[nodiscard]] QPen   makePen(long colour, long style, int width);
	/**
	 * @brief Clamps a captured absolute-miniwindow move while preserving its size.
	 * @param requestedLocation Requested top-left client coordinate.
	 * @param windowSize Current logical miniwindow size.
	 * @param clientSize World-output client size.
	 * @return Top-left coordinate that keeps the complete miniwindow inside the client.
	 */
	[[nodiscard]] QPoint constrainCapturedAbsolutePosition(const QPoint &requestedLocation,
	                                                       const QSize &windowSize, const QSize &clientSize);
	/**
	 * @brief Constrains a captured absolute-miniwindow resize independently on both axes.
	 * @param location Current top-left client coordinate.
	 * @param currentSize Current logical miniwindow size.
	 * @param requestedSize Requested logical miniwindow size.
	 * @param clientSize World-output client size.
	 * @return Size that reaches an available edge but rejects further outward growth.
	 */
	[[nodiscard]] QSize  constrainCapturedAbsoluteResize(const QPoint &location, const QSize &currentSize,
	                                                     const QSize &requestedSize, const QSize &clientSize);
	/**
	 * @brief Constrains an atomic captured absolute-miniwindow move and resize on all four edges.
	 * @param currentLocation Current top-left client coordinate.
	 * @param currentSize Current logical miniwindow size.
	 * @param requestedLocation Requested top-left client coordinate.
	 * @param requestedSize Requested logical miniwindow size.
	 * @param clientSize World-output client size.
	 * @return Geometry that preserves pure-move size and rejects outward edge growth independently.
	 */
	[[nodiscard]] QRect  constrainCapturedAbsoluteGeometry(const QPoint &currentLocation,
	                                                       const QSize  &currentSize,
	                                                       const QPoint &requestedLocation,
	                                                       const QSize  &requestedSize,
	                                                       const QSize  &clientSize);
	/**
	 * @brief Clamps a captured drag coordinate to world-output geometry edges.
	 * @param requestedPosition Requested callback coordinate.
	 * @param clientSize World-output client size.
	 * @return Coordinate limited independently to the client area's valid pixel coordinates.
	 */
	[[nodiscard]] QPoint constrainCapturedDragPosition(const QPoint &requestedPosition,
	                                                   const QSize  &clientSize);
	/**
	 * @brief Converts a display-space drag coordinate into the active layer's canonical coordinate space.
	 * @param displayPosition Display-space output-client coordinate.
	 * @param scaleX Active horizontal absolute-miniwindow scale.
	 * @param scaleY Active vertical absolute-miniwindow scale.
	 * @return Coordinate expressed in canonical miniwindow client units.
	 */
	[[nodiscard]] QPoint capturedDragPositionToCanonical(const QPoint &displayPosition, double scaleX,
	                                                     double scaleY);
	[[nodiscard]] int    validatePenStyle(long penStyle, int penWidth);
	[[nodiscard]] int    validateBrushStyle(long brushStyle);
	[[nodiscard]] int    bytesPerLine(int width, int bitsPerPixel);
	[[nodiscard]] int    textWidth(const MiniWindow &window, const QString &fontId, const QString &text);
	[[nodiscard]] int textPreviewWidth(const MiniWindow &window, const QString &fontId, const QString &text,
	                                   int left, int top, int right, int bottom);
	[[nodiscard]] QVariant fontInfo(const MiniWindow &window, const QString &fontId, int infoType);
	[[nodiscard]] QVariant imageInfo(const MiniWindow &window, const QString &imageId, int infoType);
	[[nodiscard]] QVariant hotspotInfo(const MiniWindow &window, const QString &hotspotId, int infoType);
	[[nodiscard]] long     pixelValue(const MiniWindow &window, int x, int y);
	[[nodiscard]] bool     saveWindowImage24Bit(const MiniWindow &window, const QString &filename);
	void                   setPixel(MiniWindow &window, int x, int y, long colour);
	void create(MiniWindow &window, const QString &name, int left, int top, int width, int height,
	            int position, int flags, const QColor &background, const QString &pluginId,
	            double devicePixelRatio = 1.0);
	void setDevicePixelRatio(MiniWindow &window, double devicePixelRatio);
	int  font(MiniWindow &window, const QString &fontId, const QString &fontName, double size, bool bold,
	          bool italic, bool underline, bool strikeout, int charset, int pitchAndFamily);
	int  rectOp(MiniWindow &window, int action, int left, int top, int right, int bottom, long colour1,
	            long colour2);
	int  circleOp(MiniWindow &window, int action, int left, int top, int right, int bottom, long penColour,
	              long penStyle, int penWidth, long brushColour, long brushStyle, int extra1, int extra2,
	              int extra3, int extra4);
	int line(MiniWindow &window, int x1, int y1, int x2, int y2, long penColour, long penStyle, int penWidth);
	int arc(MiniWindow &window, int left, int top, int right, int bottom, int x1, int y1, int x2, int y2,
	        long penColour, long penStyle, int penWidth);
	int bezier(MiniWindow &window, const QString &points, long penColour, long penStyle, int penWidth);
	int polygon(MiniWindow &window, const QString &points, long penColour, long penStyle, int penWidth,
	            long brushColour, long brushStyle, bool closePolygon, bool winding);
	int gradient(MiniWindow &window, int left, int top, int right, int bottom, long startColour,
	             long endColour, int mode);
	int text(MiniWindow &window, const QString &fontId, const QString &text, int left, int top, int right,
	         int bottom, long colour);
	void createImage(MiniWindow &window, const QString &imageId, long row1, long row2, long row3, long row4,
	                 long row5, long row6, long row7, long row8);
	bool loadImage(MiniWindow &window, const QString &imageId, const QString &filename);
	bool loadImageMemory(MiniWindow &window, const QString &imageId, const QByteArray &data, bool swapAlpha);
	void imageFromWindow(MiniWindow &window, const QString &imageId, const MiniWindow &sourceWindow);
	int  drawImage(MiniWindow &window, const QString &imageId, int left, int top, int right, int bottom,
	               int mode, int srcLeft, int srcTop, int srcRight, int srcBottom);
	int  drawImageAlpha(MiniWindow &window, const QString &imageId, int left, int top, int right, int bottom,
	                    double opacity, int srcLeft, int srcTop);
	int  imageOp(MiniWindow &window, int action, int left, int top, int right, int bottom, long penColour,
	             long penStyle, int penWidth, long brushColour, const QString &imageId, int ellipseWidth,
	             int ellipseHeight);
	int  mergeImageAlpha(MiniWindow &window, const QString &imageId, const QString &maskId, int left, int top,
	                     int right, int bottom, int mode, double opacity, int srcLeft, int srcTop,
	                     int srcRight, int srcBottom);
	int  getImageAlpha(MiniWindow &window, const QString &imageId, int left, int top, int right, int bottom,
	                   int srcLeft, int srcTop);
	using RandomUnit = std::function<double()>;
	int blendImage(MiniWindow &window, const QString &imageId, int left, int top, int right, int bottom,
	               int mode, double opacity, int srcLeft, int srcTop, int srcRight, int srcBottom,
	               const RandomUnit &randomUnit);
	int filter(MiniWindow &window, int left, int top, int right, int bottom, int operation, double options,
	           const RandomUnit &randomUnit);
	int transformImage(MiniWindow &window, const QString &imageId, float left, float top, int mode, float mxx,
	                   float mxy, float myx, float myy);
	void resize(MiniWindow &window, int width, int height, long colour);
	void position(MiniWindow &window, int left, int top, int position, int flags);
	void setZOrder(MiniWindow &window, int zOrder);
	int addHotspot(MiniWindow &window, const QString &hotspotId, int left, int top, int right, int bottom,
	               const QString &mouseOver, const QString &cancelMouseOver, const QString &mouseDown,
	               const QString &cancelMouseDown, const QString &mouseUp, const QString &tooltip, int cursor,
	               int flags, const QString &pluginId);
	int deleteHotspot(MiniWindow &window, const QString &hotspotId);
	void deleteAllHotspots(MiniWindow &window);
	int  setHotspotTooltip(MiniWindow &window, const QString &hotspotId, const QString &tooltip);
	int  moveHotspot(MiniWindow &window, const QString &hotspotId, int left, int top, int right, int bottom);
	int  setHotspotDragHandler(MiniWindow &window, const QString &hotspotId, const QString &moveCallback,
	                           const QString &releaseCallback, int flags, const QString &pluginId);
	int  setHotspotScrollwheelHandler(MiniWindow &window, const QString &hotspotId,
	                                  const QString &moveCallback, const QString &pluginId);
	void clearGeneratedOutputHotspots(MiniWindow &window, const QString &hotspotPrefix);
	int  addGeneratedOutputHotspot(MiniWindow &window, const QString &hotspotPrefix, int left, int top,
	                               int right, int bottom, const QString &mouseUp, const QString &pluginId,
	                               int actionType, const QString &action);
	[[nodiscard]] bool lineFitsVertically(int y, int lineHeight, int rectBottom);
	[[nodiscard]] bool runNeedsWrap(int x, int candidateWidth, int currentLineWidth, int rectLeft,
	                                int rightLimitExclusive);
	[[nodiscard]] bool hasActivatableAction(int actionType, const QString &action, int actionNoneType);
} // namespace MiniWindowUtils
