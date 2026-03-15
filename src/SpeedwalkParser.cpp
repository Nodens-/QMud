/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: SpeedwalkParser.cpp
 * Role: Pure speedwalk evaluator implementation shared by command processing and tests.
 */

#include "SpeedwalkParser.h"

namespace
{
	const auto kEndLine = QStringLiteral("\r\n");

	bool isAsciiSpace(const QChar ch)
	{
		const ushort u = ch.unicode();
		return u == ' ' || u == '\t' || u == '\n' || u == '\r' || u == '\v' || u == '\f';
	}

	bool isAsciiDigit(const QChar ch)
	{
		const ushort u = ch.unicode();
		return u >= '0' && u <= '9';
	}
} // namespace

QString QMudSpeedwalk::makeSpeedWalkErrorString(const QString &message)
{
	return QStringLiteral("*") + message;
}

QString QMudSpeedwalk::evaluateSpeedwalk(const QString &speedWalkString, const QString &speedWalkFiller,
                                         const DirectionResolver &resolveDirection)
{
	QString strResult;
	QString str;
	int             count = 0;
	qsizetype       p     = 0;
	const qsizetype size  = speedWalkString.size();

	while (p < size)
	{
		while (p < size && isAsciiSpace(speedWalkString.at(p)))
			++p;

		if (p >= size)
			break;

		if (speedWalkString.at(p) == QLatin1Char('{'))
		{
			while (p < size && speedWalkString.at(p) != QLatin1Char('}'))
				++p;

			if (p >= size || speedWalkString.at(p) != QLatin1Char('}'))
				return makeSpeedWalkErrorString(
				    QStringLiteral("Comment code of '{' not terminated by a '}'"));
			++p;
			continue;
		}

		count = 0;
		while (p < size && isAsciiDigit(speedWalkString.at(p)))
		{
			count = count * 10 + (speedWalkString.at(p).unicode() - '0');
			++p;
			if (count > 99)
				return makeSpeedWalkErrorString(QStringLiteral("Speed walk counter exceeds 99"));
		}

		if (count == 0)
			count = 1;

		while (p < size && isAsciiSpace(speedWalkString.at(p)))
			++p;

		if (count > 1 && p >= size)
			return makeSpeedWalkErrorString(QStringLiteral("Speed walk counter not followed by an action"));

		if (count > 1 && speedWalkString.at(p) == QLatin1Char('{'))
			return makeSpeedWalkErrorString(
			    QStringLiteral("Speed walk counter may not be followed by a comment"));

		if (p >= size)
			break;

		QChar action = speedWalkString.at(p).toUpper();
		if (action == QLatin1Char('C') || action == QLatin1Char('O') || action == QLatin1Char('L') ||
		    action == QLatin1Char('K'))
		{
			if (count > 1)
				return makeSpeedWalkErrorString(QStringLiteral("Action code of C, O, L or K must not follow "
				                                               "a speed walk count (1-99)"));

			switch (action.unicode())
			{
			case 'C':
				strResult += QStringLiteral("close ");
				break;
			case 'O':
				strResult += QStringLiteral("open ");
				break;
			case 'L':
				strResult += QStringLiteral("lock ");
				break;
			case 'K':
				strResult += QStringLiteral("unlock ");
				break;
			default:
				break;
			}
			++p;

			while (p < size && isAsciiSpace(speedWalkString.at(p)))
				++p;

			if (p >= size || speedWalkString.at(p).toUpper() == QLatin1Char('F') ||
			    speedWalkString.at(p) == QLatin1Char('{'))
				return makeSpeedWalkErrorString(QStringLiteral("Action code of C, O, L or K must be followed "
				                                               "by a direction"));
		}

		if (p >= size)
			break;

		action = speedWalkString.at(p).toUpper();
		switch (action.unicode())
		{
		case 'N':
		case 'S':
		case 'E':
		case 'W':
		case 'U':
		case 'D':
			str = resolveDirection ? resolveDirection(QString(action.toLower())) : QString();
			break;
		case 'F':
			str = speedWalkFiller;
			break;
		case '(':
			str.clear();
			++p;
			while (p < size && speedWalkString.at(p) != QLatin1Char(')'))
				str += speedWalkString.at(p++);
			if (p >= size || speedWalkString.at(p) != QLatin1Char(')'))
				return makeSpeedWalkErrorString(QStringLiteral("Action code of '(' not terminated by a ')'"));
			if (const qsizetype slashPos = str.indexOf(QStringLiteral("/")); slashPos != -1)
				str = str.left(slashPos);
			break;
		default:
			return QStringLiteral("*Invalid direction '%1' in speed walk, must be "
			                      "N, S, E, W, U, D, F, or (something)")
			    .arg(speedWalkString.at(p));
		}

		++p;

		for (int j = 0; j < count; ++j)
			strResult += str + kEndLine;
	}

	return strResult;
}
