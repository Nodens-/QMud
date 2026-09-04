/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldOptions.cpp
 * Role: World-option metadata tables and conversion logic used for validation, persistence, and script accessors.
 */

#include "WorldOptions.h"
#include "ColorPacking.h"
#include "StringUtils.h"
#include <QColor>
#include <climits>
#include <cmath>

namespace
{
	constexpr long long RGB(const int red, const int green, const int blue)
	{
		return static_cast<long long>(qmudRgb(red, green, blue));
	}
} // namespace

using enum WorldNumericOptionBinding;

static const WorldNumericOption kWorldNumericOptions[] = {
#define QMUD_INCLUDE_WORLD_OPTIONS_DATA_NUMERIC_ROWS
#include "WorldOptionsDataNumeric.inc"
#undef QMUD_INCLUDE_WORLD_OPTIONS_DATA_NUMERIC_ROWS
    {nullptr, 0, 0, 0, 0, WorldNumericOptionBinding::None}};

const WorldNumericOption *worldNumericOptions()
{
	return kWorldNumericOptions;
}

int worldNumericOptionCount()
{
	int count = 0;
	while (kWorldNumericOptions[count].name)
		count++;
	return count;
}

const WorldNumericOption *QMudWorldOptions::findWorldNumericOption(const QString &name)
{
	const QString key = name.trimmed().toLower();
	for (int i = 0; kWorldNumericOptions[i].name; i++)
	{
		if (QString::fromLatin1(kWorldNumericOptions[i].name) == key)
			return &kWorldNumericOptions[i];
	}
	return nullptr;
}

bool QMudWorldOptions::numericOptionValueInRange(const WorldNumericOption &option, const long long value)
{
	const long long maximum = option.minValue == 0 && option.maxValue == 0 ? 1 : option.maxValue;
	return value >= option.minValue && value <= maximum;
}

bool QMudWorldOptions::numericOptionValueInRange(const WorldNumericOption &option, const double value)
{
	const long long maximum = option.minValue == 0 && option.maxValue == 0 ? 1 : option.maxValue;
	if (!std::isfinite(value))
		return false;

	const long double widened = value;
	return widened >= static_cast<long double>(option.minValue) &&
	       widened <= static_cast<long double>(maximum);
}

std::optional<long long> QMudWorldOptions::publicNumericOptionValue(const WorldNumericOption &option,
                                                                    const QString            &text)
{
	const QString trimmed = text.trimmed();
	if (trimmed.isEmpty())
		return std::nullopt;

	if (option.flags & OPT_RGB_COLOUR)
	{
		bool            ok      = false;
		const long long numeric = trimmed.toLongLong(&ok);
		if (ok)
			return numeric >= 0 && numeric <= 0xFFFFFF ? std::optional<long long>{numeric} : std::nullopt;

		const QColor colour(trimmed);
		if (!colour.isValid())
			return std::nullopt;
		return static_cast<long long>(qmudRgb(colour.red(), colour.green(), colour.blue()));
	}

	bool booleanValue = false;
	if (qmudParseBooleanKeyword(trimmed, booleanValue))
		return booleanValue ? 1 : 0;

	if (option.minValue == 0 && option.maxValue == 0)
	{
		bool         ok     = false;
		const double number = trimmed.toDouble(&ok);
		if (!ok || !std::isfinite(number))
			return std::nullopt;
		return number != 0.0 ? 1 : 0;
	}

	bool            ok     = false;
	const long long number = trimmed.toLongLong(&ok);
	return ok ? std::optional<long long>{number} : std::nullopt;
}

QString QMudWorldOptions::storedNumericOptionText(const WorldNumericOption &option, const long long value)
{
	if (option.flags & OPT_RGB_COLOUR)
	{
		const auto colour = static_cast<QMudColorRef>(value);
		return QString::asprintf("#%02X%02X%02X", qmudRed(colour), qmudGreen(colour), qmudBlue(colour));
	}

	return QString::number(value);
}
