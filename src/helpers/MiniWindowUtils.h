/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MiniWindowUtils.h
 * Role: Shared miniwindow rendering helpers (brush/color plus output layout/action predicates).
 */

#pragma once

#include <QBrush>
#include <QColor>
// ReSharper disable once CppUnusedIncludeDirective
#include <QString>

namespace MiniWindowUtils
{
	[[nodiscard]] QColor colorFromRef(long value);
	[[nodiscard]] QColor colorFromRefOrTransparent(long value);
	[[nodiscard]] QBrush makeBrush(long brushStyle, long penColour, long brushColour, bool *ok = nullptr);
	[[nodiscard]] bool   lineFitsVertically(int y, int lineHeight, int rectBottom);
	[[nodiscard]] bool   runNeedsWrap(int x, int candidateWidth, int currentLineWidth, int rectLeft,
	                                  int rightLimitExclusive);
	[[nodiscard]] bool   hasActivatableAction(int actionType, const QString &action, int actionNoneType);
} // namespace MiniWindowUtils
