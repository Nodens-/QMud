/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldOptionDefaults.cpp
 * Role: Authoritative default world-option value definitions consumed by document initialization and migration logic.
 */

#include "WorldOptionDefaults.h"
#include "ColorPacking.h"
#include "StringUtils.h"
#include "WorldOptions.h"

#include <QCryptographicHash>
#include <QUuid>
#include <climits>

namespace
{
	constexpr int  OPT_MULTLINE      = 0x000001;
	constexpr int  OPT_KEEP_SPACES   = 0x000002;
	constexpr int  OPT_PASSWORD      = 0x000004;
	constexpr int  OPT_COMMAND_STACK = 0x000008;
	constexpr int  OPT_WORLD_ID      = 0x000010;

	constexpr int  PLUGIN_UNIQUE_ID_LENGTH = 24;
	constexpr char kNoSoundLiteral[]       = "(No sound)";

	struct WorldNumericDefault
	{
			constexpr WorldNumericDefault(const char *name = nullptr, long long value = 0, long long min = 0,
			                              long long max = 0, int flags = 0)
			    : name(name), value(value), min(min), max(max), flags(flags)
			{
			}

			const char *name;
			long long   value;
			long long   min;
			long long   max;
			int         flags;
	};

	struct WorldAlphaDefault
	{
			constexpr WorldAlphaDefault(const char *name = nullptr, const char *value = nullptr,
			                            int flags = 0)
			    : name(name), value(value), flags(flags)
			{
			}

			const char *name;
			const char *value;
			int         flags;
	};

	constexpr long long RGB(const int red, const int green, const int blue)
	{
		return static_cast<long long>(qmudRgb(red, green, blue));
	}

	const WorldNumericDefault kNumericDefaults[] = {
#define QMUD_INCLUDE_WORLD_OPTION_DEFAULTS_NUMERIC_ROWS
#include "WorldOptionDefaultsNumeric.inc"
#undef QMUD_INCLUDE_WORLD_OPTION_DEFAULTS_NUMERIC_ROWS
	};

	const WorldAlphaDefault kAlphaDefaults[] = {
#define QMUD_INCLUDE_WORLD_OPTION_DEFAULTS_ALPHA_ROWS
#include "WorldOptionDefaultsAlpha.inc"
#undef QMUD_INCLUDE_WORLD_OPTION_DEFAULTS_ALPHA_ROWS
	};

	const WorldAlphaOption kAlphaOptions[] = {
#define QMUD_INCLUDE_WORLD_OPTION_DEFAULTS_ALPHA_ROWS
#include "WorldOptionDefaultsAlpha.inc"
#undef QMUD_INCLUDE_WORLD_OPTION_DEFAULTS_ALPHA_ROWS
	};

	QString colorRefToName(int colorRef)
	{
		const auto         ref = static_cast<QMudColorRef>(colorRef);
		const int          r   = qmudRed(ref);
		const int          g   = qmudGreen(ref);
		const int          b   = qmudBlue(ref);
		struct NamedColor
		{
				int         r;
				int         g;
				int         b;
				const char *name;
		};
		static const NamedColor kNamedColors[] = {
		    {0,   0,   0,   "black" },
            {255, 255, 255, "white" },
            {255, 0,   0,   "red"   },
		    {0,   0,   255, "blue"  },
            {128, 0,   0,   "maroon"},
		};
		for (const auto &entry : kNamedColors)
		{
			if (entry.r == r && entry.g == g && entry.b == b)
				return QString::fromLatin1(entry.name);
		}
		return QString::asprintf("#%02X%02X%02X", r, g, b);
	}

	QString numberToString(const WorldNumericDefault &opt)
	{
		if (opt.flags & OPT_RGB_COLOUR)
			return colorRefToName(static_cast<int>(opt.value));

		if (const bool isBool = (opt.min == 0 && opt.max == 0); isBool)
			return qmudBoolToYn(opt.value != 0);

		if (opt.flags & OPT_DOUBLE)
			return QString::asprintf("%.2f", static_cast<double>(opt.value));

		return QString::number(opt.value);
	}

	QString alphaDefaultValue(const WorldAlphaDefault &opt)
	{
		if (opt.flags & OPT_WORLD_ID)
			return QMudWorldOptionDefaults::generateWorldUniqueId();

		if (opt.value && opt.value[0])
			return QString::fromLatin1(opt.value);

		return {};
	}
} // namespace

QString QMudWorldOptionDefaults::generateWorldUniqueId()
{
	const QString    uuid = QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-'));
	const QByteArray hash = QCryptographicHash::hash(uuid.toLatin1(), QCryptographicHash::Sha1);
	const QByteArray id   = hash.left(PLUGIN_UNIQUE_ID_LENGTH / 2);
	return QString::fromLatin1(id.toHex());
}

void QMudWorldOptionDefaults::applyWorldOptionDefaults(QMap<QString, QString> &attrs,
                                                       QMap<QString, QString> &multilineAttrs)
{
	for (const auto &opt : kNumericDefaults)
	{
		if (!opt.name)
			break;
		const QString key = QString::fromLatin1(opt.name);
		if (attrs.contains(key))
			continue;
		attrs.insert(key, numberToString(opt));
	}

	for (const auto &opt : kAlphaDefaults)
	{
		if (!opt.name)
			break;
		const QString key = QString::fromLatin1(opt.name);
		if (const bool isMultiline = (opt.flags & OPT_MULTLINE) != 0; isMultiline)
		{
			if (multilineAttrs.contains(key))
				continue;
			multilineAttrs.insert(key, alphaDefaultValue(opt));
			continue;
		}

		if (attrs.contains(key))
			continue;
		attrs.insert(key, alphaDefaultValue(opt));
	}
}

const WorldAlphaOption *worldAlphaOptions()
{
	return kAlphaOptions;
}

int worldAlphaOptionCount()
{
	int count = 0;
	while (kAlphaOptions[count].name)
		count++;
	return count;
}

const WorldAlphaOption *QMudWorldOptionDefaults::findWorldAlphaOption(const QString &name)
{
	const QString key = name.trimmed().toLower();
	for (int i = 0; kAlphaOptions[i].name; ++i)
	{
		if (QString::fromLatin1(kAlphaOptions[i].name) == key)
			return &kAlphaOptions[i];
	}
	return nullptr;
}
