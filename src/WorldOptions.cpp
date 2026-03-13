/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldOptions.cpp
 * Role: World-option metadata tables and conversion logic used for validation, persistence, and script accessors.
 */

#include "WorldOptions.h"
#include "ColorPacking.h"
#include <climits>

namespace
{
	constexpr long long RGB(const int red, const int green, const int blue)
	{
		return static_cast<long long>(qmudRgb(red, green, blue));
	}
} // namespace

static const WorldNumericOption kWorldNumericOptions[] = {
#include "WorldOptionsDataNumeric.inc"
    {nullptr, 0, 0, 0, 0}
};

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
