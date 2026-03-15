/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: CommandTextUtils.cpp
 * Role: Send-target wildcard and escape processing extracted from command processor logic.
 */

#include "CommandTextUtils.h"

#include "WorldOptions.h"

namespace
{
	bool isAsciiHexDigit(const QChar ch)
	{
		const ushort u = ch.unicode();
		return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F');
	}

	int asciiHexValue(const QChar ch)
	{
		const ushort u = ch.unicode();
		if (u >= '0' && u <= '9')
			return u - '0';
		if (u >= 'a' && u <= 'f')
			return 10 + (u - 'a');
		if (u >= 'A' && u <= 'F')
			return 10 + (u - 'A');
		return 0;
	}
} // namespace

QString QMudCommandText::fixupEscapeSequences(const QString &source)
{
	QString out;
	out.reserve(source.size());

	for (int i = 0; i < source.size(); ++i)
	{
		QChar c = source.at(i);

		if (c == QLatin1Char('\\'))
		{
			++i;
			if (i >= source.size())
				break;

			c = source.at(i);
			switch (c.unicode())
			{
			case 'a':
				c = QChar(QLatin1Char('\a'));
				break;
			case 'b':
				c = QChar(QLatin1Char('\b'));
				break;
			case 'f':
				c = QChar(QLatin1Char('\f'));
				break;
			case 'n':
				c = QChar(QLatin1Char('\n'));
				break;
			case 'r':
				c = QChar(QLatin1Char('\r'));
				break;
			case 't':
				c = QChar(QLatin1Char('\t'));
				break;
			case 'v':
				c = QChar(QLatin1Char('\v'));
				break;
			case '\'':
				c = QLatin1Char('\'');
				break;
			case '\"':
				c = QLatin1Char('\"');
				break;
			case '\\':
				c = QLatin1Char('\\');
				break;
			case '?':
				c = QLatin1Char('\?');
				break;
			case 'x':
			{
				int value  = 0;
				int digits = 0;
				while (i + 1 < source.size() && digits < 2 && isAsciiHexDigit(source.at(i + 1)))
				{
					++i;
					value = (value << 4) + asciiHexValue(source.at(i));
					++digits;
				}
				c = QChar(static_cast<ushort>(value));
			}
			break;
			default:
				break;
			}
		}

		out.append(c);
	}

	return out;
}

QString QMudCommandText::fixWildcard(const QString &wildcard, const bool makeLowerCase, const int sendTo,
                                     const QString &language)
{
	QString result = wildcard;

	if (makeLowerCase)
		result = result.toLower();

	if (sendTo == eSendToScript || sendTo == eSendToScriptAfterOmit)
	{
		if (language.compare(QStringLiteral("vbscript"), Qt::CaseInsensitive) == 0)
		{
			result.replace(QStringLiteral("\""), QStringLiteral("\"\""));
		}
		else
		{
			result.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
			result.replace(QStringLiteral("\""), QStringLiteral("\\\""));
			if (language.compare(QStringLiteral("perlscript"), Qt::CaseInsensitive) == 0)
				result.replace(QStringLiteral("$"), QStringLiteral("\\$"));
		}
	}

	return result;
}
