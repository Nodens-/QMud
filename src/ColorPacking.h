/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: ColorPacking.h
 * Role: Bit-packing helpers for converting between channel components and packed integer color representations.
 */

#ifndef QMUD_COLORPACKING_H
#define QMUD_COLORPACKING_H

#include <QtGlobal>

// MUSHclient-compatible packed RGB representation:
// low byte = red, then green, then blue (Windows COLORREF layout).
using QMudColorRef = quint32;

/**
 * @brief Packs RGB channel bytes into MUSHclient-style COLORREF.
 * @param red Red channel [0,255].
 * @param green Green channel [0,255].
 * @param blue Blue channel [0,255].
 * @return Packed COLORREF value.
 */
inline constexpr QMudColorRef qmudRgb(int red, int green, int blue)
{
	return static_cast<QMudColorRef>((red & 0xFF) | ((green & 0xFF) << 8) | ((blue & 0xFF) << 16));
}

/**
 * @brief Extracts red channel from packed COLORREF.
 * @param color Packed COLORREF value.
 * @return Red channel value.
 */
inline constexpr quint8 qmudRed(QMudColorRef color)
{
	return static_cast<quint8>(color & 0xFF);
}

/**
 * @brief Extracts green channel from packed COLORREF.
 * @param color Packed COLORREF value.
 * @return Green channel value.
 */
inline constexpr quint8 qmudGreen(QMudColorRef color)
{
	return static_cast<quint8>((color >> 8) & 0xFF);
}

/**
 * @brief Extracts blue channel from packed COLORREF.
 * @param color Packed COLORREF value.
 * @return Blue channel value.
 */
inline constexpr quint8 qmudBlue(QMudColorRef color)
{
	return static_cast<quint8>((color >> 16) & 0xFF);
}

#endif // QMUD_COLORPACKING_H
