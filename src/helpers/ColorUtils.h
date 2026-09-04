/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: ColorUtils.h
 * Role: Colour-name conversion helper interfaces shared by XML serialization and Lua-facing colour utility methods.
 */

#ifndef QMUD_COLORUTILS_H
#define QMUD_COLORUTILS_H

// ReSharper disable once CppUnusedIncludeDirective
#include <QLatin1StringView>
#include <QString>

namespace QMudColourGroup
{
	/**
	 * @brief Canonical internal and XML group name for MUSHclient custom colours.
	 */
	inline constexpr QLatin1StringView kCustom{"custom"};
} // namespace QMudColourGroup

/**
 * @brief Converts RGB color value to closest/known name string.
 * @param colour RGB colour value.
 * @return Closest/known color name.
 */
QString qmudColourToName(long colour);

#endif // QMUD_COLORUTILS_H
