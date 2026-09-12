/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: Blending.h
 * Role: Color-blending primitives used by text styling and miniwindow rendering paths for consistent visual
 * composition.
 */

#ifndef QMUD_BLENDING_H
#define QMUD_BLENDING_H

#include <QtGlobal>
#include <array>
#include <cmath>

namespace QMudBlend
{
	/**
	 * @brief Clamps integer channel value to [0,255].
	 */
	constexpr int clampToByteRange(const int value) noexcept
	{
		return (value < 0) ? 0 : (value > 255) ? 255 : value;
	}

	using Channel = quint8;

	/**
	 * @brief Floating-point HSL components used by MUSHclient-compatible blend modes.
	 */
	struct HslComponents
	{
			double hueDegrees{0.0};
			double saturation{0.0};
			double luminance{0.0};
	};

	/**
	 * @brief Converts byte RGB channels to the HSL representation used by MUSHclient.
	 */
	inline HslComponents rgbToHsl(const long redChannel, const long greenChannel,
	                              const long blueChannel) noexcept
	{
		const double red        = static_cast<double>(redChannel) / 255.0;
		const double green      = static_cast<double>(greenChannel) / 255.0;
		const double blue       = static_cast<double>(blueChannel) / 255.0;
		const double minimum    = qMin(red, qMin(green, blue));
		const double maximum    = qMax(red, qMax(green, blue));
		const double difference = maximum - minimum;

		if (difference == 0.0)
			return {0.0, 0.0, minimum};

		HslComponents result;
		result.luminance  = (maximum + minimum) / 2.0;
		result.saturation = result.luminance < 0.5 ? difference / (maximum + minimum)
		                                           : difference / (2.0 - maximum - minimum);

		if (red == maximum)
			result.hueDegrees = 60.0 * (green - blue) / difference;
		else if (green == maximum)
			result.hueDegrees = 60.0 * (blue - red) / difference + 120.0;
		else
			result.hueDegrees = 60.0 * (red - green) / difference + 240.0;

		if (result.hueDegrees < 0.0)
			result.hueDegrees += 360.0;
		else if (result.hueDegrees > 360.0)
			result.hueDegrees -= 360.0;
		result.saturation = qBound(0.0, result.saturation, 1.0);
		return result;
	}

	/**
	 * @brief Converts MUSHclient HSL components to rounded byte RGB channels.
	 */
	inline std::array<Channel, 3> hslToRgb(const HslComponents &hsl) noexcept
	{
		const auto roundedChannel = [](const double value)
		{ return static_cast<Channel>(clampToByteRange(static_cast<int>(std::lround(value * 255.0)))); };
		if (hsl.saturation <= 0.0)
		{
			const Channel grey = roundedChannel(hsl.luminance);
			return {grey, grey, grey};
		}

		const double q                = hsl.luminance < 0.5
		                                    ? hsl.luminance * (1.0 + hsl.saturation)
		                                    : hsl.luminance + hsl.saturation - hsl.luminance * hsl.saturation;
		const double p                = 2.0 * hsl.luminance - q;
		const double hue              = hsl.hueDegrees / 360.0;
		const auto   channelComponent = [p, q](double component)
		{
			if (component < 0.0)
				component += 1.0;
			if (component > 1.0)
				component -= 1.0;
			if (component < 1.0 / 6.0)
				return p + (q - p) * 6.0 * component;
			if (component < 0.5)
				return q;
			if (component < 2.0 / 3.0)
				return p + (q - p) * 6.0 * (2.0 / 3.0 - component);
			return p;
		};
		return {roundedChannel(channelComponent(hue + 1.0 / 3.0)), roundedChannel(channelComponent(hue)),
		        roundedChannel(channelComponent(hue - 1.0 / 3.0))};
	}

	/**
	 * @brief Returns square of long value.
	 */
	constexpr long squaredLong(const long value) noexcept
	{
		return value * value;
	}

	// A = blend, B = base
	inline Channel normal(const long blend,
	                      const long /* base */) noexcept ///< Normal blend (source channel passthrough).
	{
		return static_cast<Channel>(blend);
	}

	inline Channel lighten(const long blend, const long base) noexcept ///< Lighten blend (component max).
	{
		return static_cast<Channel>((base > blend) ? base : blend);
	}

	inline Channel darken(const long blend, const long base) noexcept ///< Darken blend (component min).
	{
		return static_cast<Channel>((base > blend) ? blend : base);
	}

	inline Channel multiply(const long blend, const long base) noexcept ///< Multiply blend.
	{
		return static_cast<Channel>((blend * base) / 255);
	}

	inline Channel average(const long blend, const long base) noexcept ///< Average blend.
	{
		return static_cast<Channel>((blend + base) / 2);
	}

	inline Channel add(const long blend, const long base) noexcept ///< Additive blend with saturation.
	{
		return static_cast<Channel>((blend + base > 255) ? 255 : (blend + base));
	}

	inline Channel subtract(const long blend, const long base) noexcept ///< Subtractive blend with floor.
	{
		return static_cast<Channel>((blend + base < 255) ? 0 : (blend + base - 255));
	}

	inline Channel difference(const long blend, const long base) noexcept ///< Absolute channel difference.
	{
		return static_cast<Channel>(qAbs(blend - base));
	}

	inline Channel negation(const long blend, const long base) noexcept ///< Negation blend.
	{
		return static_cast<Channel>(255 - qAbs(255 - blend - base));
	}

	inline Channel screen(const long blend, const long base) noexcept ///< Screen blend.
	{
		return static_cast<Channel>(255 - (((255 - blend) * (255 - base)) >> 8));
	}

	inline Channel exclusion(const long blend, const long base) noexcept ///< Exclusion blend.
	{
		return static_cast<Channel>(blend + base - 2 * blend * base / 255);
	}

	inline Channel overlay(const long blend, const long base) noexcept ///< Overlay blend.
	{
		return static_cast<Channel>((base < 128) ? (2 * blend * base / 255)
		                                         : (255 - 2 * (255 - blend) * (255 - base) / 255));
	}

	inline Channel softLight(const long blend, const long base) noexcept ///< Soft-light blend.
	{
		const long component = (blend * base) >> 8;
		return static_cast<Channel>(
		    component + ((base * (255 - (((255 - base) * (255 - blend)) >> 8) - component)) >> 8));
	}

	inline Channel hardLight(const long blend, const long base) noexcept ///< Hard-light blend.
	{
		return overlay(base, blend);
	}

	inline Channel colorDodge(const long blend, const long base) noexcept ///< Color-dodge blend.
	{
		if (blend == 255)
			return static_cast<Channel>(blend);
		const unsigned long value =
		    (static_cast<unsigned long>(base) << 8) / static_cast<unsigned long>(255 - blend);
		return static_cast<Channel>(value > 255UL ? 255UL : value);
	}

	inline Channel colorBurn(const long blend, const long base) noexcept ///< Color-burn blend.
	{
		if (blend == 0)
			return 0;
		return static_cast<Channel>(
		    255UL - ((static_cast<unsigned long>(255 - base) << 8) / static_cast<unsigned long>(blend)));
	}

	inline Channel linearDodge(const long blend, const long base) noexcept ///< Linear-dodge alias (add).
	{
		return add(blend, base);
	}

	inline Channel linearBurn(const long blend, const long base) noexcept ///< Linear-burn alias (subtract).
	{
		return subtract(blend, base);
	}

	inline Channel linearLight(const long blend, const long base) noexcept ///< Linear-light blend.
	{
		return static_cast<Channel>((blend < 128) ? linearBurn(2 * blend, base)
		                                          : linearDodge(2 * (blend - 128), base));
	}

	inline Channel vividLight(const long blend, const long base) noexcept ///< Vivid-light blend.
	{
		return static_cast<Channel>((blend < 128) ? colorBurn(2 * blend, base)
		                                          : colorDodge(2 * (blend - 128), base));
	}

	inline Channel pinLight(const long blend, const long base) noexcept ///< Pin-light blend.
	{
		return static_cast<Channel>((blend < 128) ? darken(2 * blend, base)
		                                          : lighten(2 * (blend - 128), base));
	}

	inline Channel hardMix(const long blend, const long base) noexcept ///< Hard-mix threshold blend.
	{
		return static_cast<Channel>((blend < 255 - base) ? 0 : 255);
	}

	inline Channel reflect(const long blend, const long base) noexcept ///< Reflect blend.
	{
		if (base == 255)
			return static_cast<Channel>(base);
		const long value = (blend * blend) / (255 - base);
		return static_cast<Channel>((value > 255) ? 255 : value);
	}

	inline Channel glow(const long blend, const long base) noexcept ///< Glow blend (swapped reflect).
	{
		return reflect(base, blend);
	}

	inline Channel phoenix(const long blend, const long base) noexcept ///< Phoenix blend.
	{
		return static_cast<Channel>(qMin(blend, base) - qMax(blend, base) + 255);
	}

	/**
	 * @brief Applies blend operation and linear opacity mixing.
	 */
	template <typename BlendOp>
	inline Channel withOpacity(const long blend, const long base, BlendOp blendOp,
	                           const double opacity) noexcept
	{
		const double blended =
		    opacity * static_cast<double>(blendOp(blend, base)) + (1.0 - opacity) * static_cast<double>(base);
		return static_cast<Channel>(clampToByteRange(static_cast<int>(std::lround(blended))));
	}

	inline Channel inverseColorDodge(const long blend,
	                                 const long base) noexcept ///< Inverse color-dodge (swapped inputs).
	{
		return colorDodge(base, blend);
	}

	inline Channel inverseColorBurn(const long blend,
	                                const long base) noexcept ///< Inverse color-burn (swapped inputs).
	{
		return colorBurn(base, blend);
	}

	inline Channel freeze(const long blend, const long base) noexcept ///< Freeze blend.
	{
		if (blend == 0)
			return static_cast<Channel>(blend);
		const long value = 255 - squaredLong(255 - base) / blend;
		return static_cast<Channel>((value < 0) ? 0 : value);
	}

	inline Channel heat(const long blend, const long base) noexcept ///< Heat blend (swapped freeze).
	{
		return freeze(base, blend);
	}

	inline Channel stamp(const long blend, const long base) noexcept ///< Stamp blend.
	{
		const long value = base + 2 * blend - 256;
		if (value < 0)
			return 0;
		if (value > 255)
			return 255;
		return static_cast<Channel>(value);
	}

	inline Channel interpolate(const long blend, const long base,
	                           const quint8 *cosTable) noexcept ///< Cos-table interpolation blend.
	{
		const int value = cosTable[blend] + cosTable[base];
		return static_cast<Channel>((value > 255) ? 255 : value);
	}

	inline Channel bitXor(const long blend, const long base) noexcept ///< Bitwise XOR blend.
	{
		return static_cast<Channel>(blend ^ base);
	}

	inline Channel bitAnd(const long blend, const long base) noexcept ///< Bitwise AND blend.
	{
		return static_cast<Channel>(blend & base);
	}

	inline Channel bitOr(const long blend, const long base) noexcept ///< Bitwise OR blend.
	{
		return static_cast<Channel>(blend | base);
	}

	/**
	 * @brief Applies simple opacity interpolation between base and value.
	 */
	inline quint8 simpleOpacity(const double base, const double value, const double opacity)
	{
		const double blended = opacity * value + (1.0 - opacity) * base;
		return static_cast<quint8>(clampToByteRange(static_cast<int>(std::lround(blended))));
	}
} // namespace QMudBlend

#endif // QMUD_BLENDING_H
