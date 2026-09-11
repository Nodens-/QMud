/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldCommandProcessorUtils.cpp
 * Role: Consolidated pure helper utilities used by world command processing and related tests.
 */

#include "WorldCommandProcessorUtils.h"

#include "WorldOptions.h"

#include <QStringView>
#include <algorithm>
#include <limits>

namespace
{
	constexpr QChar kQueueActionSourceMarker{0xFDD0};

	bool            isAsciiHexDigit(const QChar ch)
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

	QString rightTrimmedTriggerLine(const QString &line)
	{
		qsizetype end = line.size();
		while (end > 0)
		{
			const QChar ch = line.at(end - 1);
			if (ch != QLatin1Char(' ') && ch != QLatin1Char('\t'))
				break;
			--end;
		}
		return end == line.size() ? line : line.left(end);
	}
} // namespace

namespace QMudCommandStack
{
	bool isValidSeparator(const QString &separator)
	{
		if (separator.isEmpty() || separator.size() > kMaximumSeparatorLength)
			return false;

		return std::ranges::all_of(separator, [](const QChar ch) { return ch.isPrint() && !ch.isSpace(); });
	}

	QStringList expand(const QString &command, const QString &separator)
	{
		if (!isValidSeparator(separator))
			return {command};

		if (command.startsWith(separator))
			return {command.sliced(separator.size())};

		const QString doubledSeparator = separator + separator;
		QStringList   commands;
		QString       current;
		current.reserve(command.size());
		for (qsizetype offset = 0; offset < command.size();)
		{
			const QStringView remaining(command.constData() + offset, command.size() - offset);
			if (remaining.startsWith(doubledSeparator))
			{
				current += separator;
				offset += doubledSeparator.size();
				continue;
			}
			if (remaining.startsWith(separator))
			{
				commands.push_back(std::move(current));
				current.clear();
				offset += separator.size();
				continue;
			}
			if (command.at(offset) == QLatin1Char('\n'))
			{
				commands.push_back(std::move(current));
				current.clear();
				++offset;
				continue;
			}

			current += command.at(offset);
			++offset;
		}
		commands.push_back(std::move(current));
		return commands;
	}
} // namespace QMudCommandStack

namespace QMudCommandPattern
{
	QString convertToRegularExpression(const QString &matchString, const bool wholeLine,
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
} // namespace QMudCommandPattern

namespace QMudCommandQueue
{
	bool shouldQueueCommand(const int speedWalkDelayMs, const bool queueRequested, const bool queueNotEmpty)
	{
		return speedWalkDelayMs > 0 && (queueRequested || queueNotEmpty);
	}

	QString encodeQueueEntry(const QString &payload, const bool queueRequested, const bool echo,
	                         const bool logIt, const unsigned short actionSource)
	{
		QString flag;
		if (queueRequested)
			flag = echo ? QStringLiteral("E") : QStringLiteral("e");
		else
			flag = echo ? QStringLiteral("I") : QStringLiteral("i");

		if (!logIt)
			flag = flag.toLower();

		QString encoded;
		encoded.reserve(payload.size() + 3);
		encoded += flag;
		encoded += kQueueActionSourceMarker;
		encoded += QChar(actionSource);
		encoded += payload;
		return encoded;
	}

	QueueEntry decodeQueueEntry(const QString &entry)
	{
		if (entry.isEmpty())
			return {};

		const QChar kind  = entry.at(0);
		const QChar upper = kind.toUpper();

		QueueEntry  out;
		out.withEcho   = (upper == QLatin1Char('E') || upper == QLatin1Char('I'));
		out.logIt      = kind.isUpper();
		out.queuedType = (upper == QLatin1Char('E'));
		if (entry.size() >= 3 && entry.at(1) == kQueueActionSourceMarker)
		{
			out.actionSource = entry.at(2).unicode();
			out.payload      = entry.mid(3);
		}
		else
		{
			out.payload = entry.mid(1);
		}
		return out;
	}

	QStringList takeDispatchBatch(QStringList &queue, const bool flushAll)
	{
		QStringList batch;
		while (!queue.isEmpty())
		{
			const QString queued = queue.takeFirst();
			if (queued.isEmpty())
				continue;

			batch.append(queued);
			if (!flushAll && decodeQueueEntry(queued).queuedType)
				break;
		}
		return batch;
	}

	int discardAll(QStringList &queue)
	{
		const qsizetype size = queue.size();
		queue.clear();
		if (size <= 0)
			return 0;
		constexpr qsizetype kMaxInt = std::numeric_limits<int>::max();
		return size > kMaxInt ? std::numeric_limits<int>::max() : static_cast<int>(size);
	}
} // namespace QMudCommandQueue

namespace QMudCommandText
{
	QString normalizeActionCommandTextNewlines(const QString &source)
	{
		QString normalized;
		normalized.reserve(source.size());
		for (qsizetype i = 0; i < source.size(); ++i)
		{
			const QChar ch = source.at(i);
			if (ch == QLatin1Char('\r'))
			{
				if (i + 1 < source.size() && source.at(i + 1) == QLatin1Char('\n'))
					++i;
				normalized += QStringLiteral("\r\n");
				continue;
			}
			if (ch == QLatin1Char('\n'))
			{
				normalized += QStringLiteral("\r\n");
				continue;
			}
			normalized += ch;
		}
		return normalized;
	}

	QString fixupEscapeSequences(const QString &source)
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

	QString fixWildcard(const QString &wildcard, const bool makeLowerCase, const int sendTo,
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

	QString normalizeTriggerMatchLine(const QString &line, const bool preserveTrailingWhitespace)
	{
		return preserveTrailingWhitespace ? line : rightTrimmedTriggerLine(line);
	}

	QString buildTriggerMultilineTarget(const QStringList &recentLines, const bool preserveTrailingWhitespace)
	{
		QString target;
		for (const QString &recentLine : recentLines)
		{
			target += normalizeTriggerMatchLine(recentLine, preserveTrailingWhitespace);
			target += QLatin1Char('\n');
		}
		return target;
	}
} // namespace QMudCommandText

namespace QMudTriggerSound
{
	bool shouldPlayTriggerSound(const bool pluginScoped, const bool worldTriggerSoundsEnabled)
	{
		if (pluginScoped)
			return true;
		return worldTriggerSoundsEnabled;
	}
} // namespace QMudTriggerSound
