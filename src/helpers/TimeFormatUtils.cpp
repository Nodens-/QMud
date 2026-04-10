/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: TimeFormatUtils.cpp
 * Role: Shared world-time formatting helpers used by runtime and async restore paths.
 */

#include "TimeFormatUtils.h"

#include <QCoreApplication>
#include <QDir>
#include <QLocale>

namespace
{
	QString escapePercent(const QString &value)
	{
		auto result = value;
		result.replace(QStringLiteral("%"), QStringLiteral("%%"));
		return result;
	}

	QString toQtDateFormat(const QString &format)
	{
		QString result;
		result.reserve(format.size() + 16);

		QString literal;
		literal.reserve(format.size());
		const auto flushLiteral = [&]
		{
			if (literal.isEmpty())
				return;
			auto escaped = literal;
			escaped.replace(QStringLiteral("'"), QStringLiteral("''"));
			result += QLatin1Char('\'');
			result += escaped;
			result += QLatin1Char('\'');
			literal.clear();
		};

		for (int i = 0; i < format.size(); ++i)
		{
			const auto ch = format.at(i);
			if (ch != QLatin1Char('%'))
			{
				literal += ch;
				continue;
			}

			if (i + 1 >= format.size())
			{
				literal += QLatin1Char('%');
				break;
			}

			auto next  = format.at(++i);
			bool noPad = false;
			if (next == QLatin1Char('#') && i + 1 < format.size())
			{
				noPad = true;
				next  = format.at(++i);
			}

			QString token;
			switch (next.unicode())
			{
			case '%':
				literal += QLatin1Char('%');
				break;
			case 'A':
				token = QStringLiteral("dddd");
				break;
			case 'B':
				token = QStringLiteral("MMMM");
				break;
			case 'b':
				token = QStringLiteral("MMM");
				break;
			case 'd':
				token = noPad ? QStringLiteral("d") : QStringLiteral("dd");
				break;
			case 'm':
				token = noPad ? QStringLiteral("M") : QStringLiteral("MM");
				break;
			case 'Y':
				token = QStringLiteral("yyyy");
				break;
			case 'H':
				token = noPad ? QStringLiteral("H") : QStringLiteral("HH");
				break;
			case 'I':
				token = noPad ? QStringLiteral("h") : QStringLiteral("hh");
				break;
			case 'M':
				token = noPad ? QStringLiteral("m") : QStringLiteral("mm");
				break;
			case 'S':
				token = noPad ? QStringLiteral("s") : QStringLiteral("ss");
				break;
			case 'p':
				token = QStringLiteral("AP");
				break;
			default:
				literal += QLatin1Char('%');
				if (noPad)
					literal += QLatin1Char('#');
				literal += next;
				break;
			}

			if (!token.isEmpty())
			{
				flushLiteral();
				result += token;
			}
		}

		flushLiteral();
		return result;
	}
} // namespace

QString TimeFormatUtils::resolveWorkingDir(const QString &startupDir)
{
	if (!startupDir.isEmpty())
		return startupDir;
	auto dir = QCoreApplication::applicationDirPath();
	if (!dir.endsWith(QLatin1Char('/')))
		dir += QLatin1Char('/');
	return dir;
}

QString TimeFormatUtils::makeAbsolutePath(const QString &fileName, const QString &workingDir)
{
	if (fileName.isEmpty())
		return fileName;

#ifdef Q_OS_WIN
	auto normalized = QDir::fromNativeSeparators(fileName);
#else
	auto normalized = fileName;
	normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
#endif
	const auto hadTrailingSeparator =
	    normalized.endsWith(QLatin1Char('/')) || normalized.endsWith(QLatin1Char('\\'));
	const auto first      = normalized.at(0);
	const auto isDrive    = normalized.size() > 1 && normalized.at(1) == QChar(':') && first.isLetter();
	const auto isAbsolute = isDrive || first == QChar('\\') || first == QChar('/');

	if (!isAbsolute)
	{
		auto relative = normalized;
		if (relative.startsWith(QStringLiteral("./")) || relative.startsWith(QStringLiteral(".\\")))
			relative = relative.mid(2);
		auto resolved = QDir(workingDir).filePath(relative);
#ifndef Q_OS_WIN
		resolved = QDir::cleanPath(resolved);
#endif
		if (hadTrailingSeparator && !resolved.endsWith(QLatin1Char('/')))
			resolved += QLatin1Char('/');
		return resolved;
	}

#ifndef Q_OS_WIN
	normalized = QDir::cleanPath(normalized);
#endif
	if (hadTrailingSeparator && !normalized.endsWith(QLatin1Char('/')))
		normalized += QLatin1Char('/');
	return normalized;
}

QString TimeFormatUtils::formatWorldTime(const QDateTime &time, const QString &format,
                                         const WorldTimeFormatContext &context, const bool fixHtml,
                                         HtmlFixupFn fixupHtml)
{
	auto       strFormat = format;

	const auto fixupOrIdentity = [&](const QString &value)
	{
		if (!fixHtml || !fixupHtml)
			return value;
		return fixupHtml(value);
	};

	strFormat.replace(QStringLiteral("%E"), escapePercent(fixupOrIdentity(context.workingDir)));
	strFormat.replace(QStringLiteral("%N"), escapePercent(fixupOrIdentity(context.worldName)));
	strFormat.replace(QStringLiteral("%P"), escapePercent(fixupOrIdentity(context.playerName)));
	strFormat.replace(QStringLiteral("%F"), escapePercent(fixupOrIdentity(context.worldDir)));
	strFormat.replace(QStringLiteral("%L"), escapePercent(fixupOrIdentity(context.logDir)));

	const auto qtFormat = toQtDateFormat(strFormat);
	return QLocale::system().toString(time, qtFormat);
}
