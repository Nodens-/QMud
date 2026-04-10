/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MiniWindowBrushUtils.h
 * Role: Shared colour/brush helpers for miniwindow and text-rectangle compatibility drawing.
 */

#pragma once

#include <QBrush>
#include <QColor>

namespace MiniWindowBrushUtils
{
	[[nodiscard]] QColor colorFromRef(long value);
	[[nodiscard]] QColor colorFromRefOrTransparent(long value);
	[[nodiscard]] QBrush makeBrush(long brushStyle, long penColour, long brushColour, bool *ok = nullptr);
} // namespace MiniWindowBrushUtils
