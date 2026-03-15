/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: CommandPatternUtils.cpp
 * Role: Pure wildcard-to-regex conversion utilities extracted from command processor logic.
 */

#include "CommandPatternUtils.h"

QString QMudCommandPattern::convertToRegularExpression(const QString &matchString, const bool wholeLine,
                                                       const bool makeAsterisksWildcards)
{
	QString strRegexp;

	int     iSize = 0;
	for (const QChar ch : matchString)
	{
		if (const ushort u = ch.unicode(); u < ' ')
			iSize += 3;
		else if (!(ch.isLetterOrNumber() || ch == QLatin1Char(' ') || u >= 0x80))
		{
			if (ch == QLatin1Char('*'))
				iSize += 4;
			else
				iSize++;
		}
	}

	strRegexp.reserve(matchString.length() + iSize + 2 + 10);

	if (wholeLine)
		strRegexp += QLatin1Char('^');

	for (const QChar ch : matchString)
	{
		if (ch == QLatin1Char('\n'))
		{
			strRegexp += QLatin1Char('\\');
			strRegexp += QLatin1Char('n');
		}
		else if (const ushort u = ch.unicode(); u < ' ')
		{
			strRegexp += QLatin1Char('\\');
			strRegexp += QLatin1Char('x');
			constexpr char hex[] = "0123456789abcdef";
			strRegexp += QLatin1Char(hex[u >> 4 & 0x0F]);
			strRegexp += QLatin1Char(hex[u & 0x0F]);
		}
		else if (ch.isLetterOrNumber() || ch == QLatin1Char(' ') || ch.unicode() >= 0x80)
		{
			strRegexp += ch;
		}
		else if (ch == QLatin1Char('*') && makeAsterisksWildcards)
		{
			strRegexp += QStringLiteral("(.*?)");
		}
		else
		{
			strRegexp += QLatin1Char('\\');
			strRegexp += ch;
		}
	}

	if (wholeLine)
		strRegexp += QLatin1Char('$');

	return strRegexp;
}
