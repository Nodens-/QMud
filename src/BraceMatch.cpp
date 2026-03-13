/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: BraceMatch.cpp
 * Role: Brace-matching logic used by command/script editing paths to improve navigation and validation feedback.
 */

#include "BraceMatch.h"

#include <QApplication>
#include <QString>
#include <algorithm>

namespace
{
	constexpr QChar kDelims[]      = {QLatin1Char('{'), QLatin1Char('['), QLatin1Char('('),
	                                  QLatin1Char('}'), QLatin1Char(']'), QLatin1Char(')')};
	constexpr QChar kStartDelims[] = {QLatin1Char('{'), QLatin1Char('['), QLatin1Char('(')};
	constexpr QChar kEndDelims[]   = {QLatin1Char('}'), QLatin1Char(']'), QLatin1Char(')')};

	bool            isDelimiter(const QChar ch)
	{
		return std::ranges::any_of(kDelims, [ch](const QChar d) { return d == ch; });
	}

	bool isStartDelimiter(const QChar ch)
	{
		return std::ranges::any_of(kStartDelims, [ch](const QChar d) { return d == ch; });
	}

	bool isEndDelimiter(const QChar ch)
	{
		return std::ranges::any_of(kEndDelims, [ch](const QChar d) { return d == ch; });
	}

	QChar matchingDelimiter(QChar ch)
	{
		switch (ch.unicode())
		{
		case '[':
			return QLatin1Char(']');
		case '(':
			return QLatin1Char(')');
		case '{':
			return QLatin1Char('}');
		case ']':
			return QLatin1Char('[');
		case ')':
			return QLatin1Char('(');
		case '}':
			return QLatin1Char('{');
		default:
			return {};
		}
	}

	void warnNoMatch()
	{
		QApplication::beep();
	}

	int countEscapesBackward(const QString &text, const int pos, const QChar escapeChar)
	{
		int count = 0;
		int p     = pos;
		while (p > 0 && text.at(p - 1) == escapeChar)
		{
			--p;
			++count;
		}
		return count;
	}
} // namespace

namespace QMudBraceMatch
{
	bool findMatchingBrace(QPlainTextEdit *edit, const bool selectRange, const int flags)
	{
		if (!edit)
			return false;

		const QString text = edit->toPlainText();
		if (text.isEmpty())
		{
			warnNoMatch();
			return false;
		}

		const QTextCursor cursor = edit->textCursor();
		int               end    = 0;
		if (cursor.hasSelection())
		{
			end = cursor.selectionEnd();
		}
		else
		{
			end = cursor.position();
		}

		if (end < 0 || end > text.size())
		{
			warnNoMatch();
			return false;
		}

		int pos = end;
		if (pos == text.size())
			pos = end - 1;
		else if (pos > 0 && !isDelimiter(text.at(pos)))
			pos--;

		if (pos < 0 || pos >= text.size())
		{
			warnNoMatch();
			return false;
		}

		const QChar startDelim         = text.at(pos);
		const bool  nestedPairs        = (flags & 0x0001) != 0;
		const bool  allowSingleQuotes  = (flags & 0x0002) != 0;
		const bool  allowDoubleQuotes  = (flags & 0x0004) != 0;
		const bool  escapeSingleQuotes = (flags & 0x0008) != 0;
		const bool  escapeDoubleQuotes = (flags & 0x0010) != 0;
		const bool  backslashEscape    = (flags & 0x0020) != 0;
		const bool  percentEscape      = (flags & 0x0040) != 0;
		int         escapeCount        = 0;
		if (backslashEscape)
			escapeCount = countEscapesBackward(text, pos, QLatin1Char('\\'));
		if (escapeCount == 0 && percentEscape)
			escapeCount = countEscapesBackward(text, pos, QLatin1Char('%'));

		if (escapeCount & 1 || !isDelimiter(startDelim))
		{
			warnNoMatch();
			return false;
		}

		int                  level   = -1;
		static constexpr int maxNest = 1000;
		QChar                nestArray[maxNest];
		bool                 backwards = false;

		int                  matchStart = pos;
		int                  matchEnd   = pos;

		if (isStartDelimiter(startDelim))
		{
			nestArray[++level] = matchingDelimiter(startDelim);
			for (int i = pos + 1; i < text.size(); ++i)
			{
				if (const QChar c = text.at(i); c == nestArray[level])
				{
					if (level <= 0)
					{
						matchEnd = i;
						break;
					}
					--level;
				}
				else if ((nestedPairs && isStartDelimiter(c)) || (!nestedPairs && c == startDelim))
				{
					++level;
					if (level >= maxNest)
					{
						warnNoMatch();
						return false;
					}
					nestArray[level] = matchingDelimiter(c);
				}
				else if ((c == QLatin1Char('\\') && backslashEscape) ||
				         (c == QLatin1Char('%') && percentEscape))
				{
					if (i + 1 < text.size())
						++i;
				}
				else if ((c == QLatin1Char('\"') && allowDoubleQuotes) ||
				         (c == QLatin1Char('\'') && allowSingleQuotes))
				{
					const bool escapeSingle = escapeSingleQuotes && c == QLatin1Char('\'');
					const bool escapeDouble = escapeDoubleQuotes && c == QLatin1Char('\"');
					const bool escapedQuote = escapeSingle || escapeDouble;
					for (int j = i + 1; j < text.size(); ++j)
					{
						const QChar qc = text.at(j);
						if (qc == c)
						{
							i = j;
							break;
						}
						else if (((qc == QLatin1Char('\\') && backslashEscape) ||
						          (qc == QLatin1Char('%') && percentEscape)) &&
						         escapedQuote)
						{
							if (j + 1 < text.size())
								++j;
						}
					}
				}
			}

			if (matchEnd == matchStart)
			{
				warnNoMatch();
				return false;
			}
		}
		else
		{
			backwards          = true;
			nestArray[++level] = matchingDelimiter(startDelim);
			for (int i = pos - 1; i >= 0; --i)
			{
				const QChar c                = text.at(i);
				int         escapeCountLocal = 0;
				if (backslashEscape)
					escapeCountLocal = countEscapesBackward(text, i, QLatin1Char('\\'));
				if (escapeCountLocal == 0 && percentEscape)
					escapeCountLocal = countEscapesBackward(text, i, QLatin1Char('%'));
				if (escapeCountLocal & 1)
					continue;

				if (c == nestArray[level])
				{
					if (level <= 0)
					{
						matchStart = i;
						break;
					}
					--level;
				}
				else if (nestedPairs && isStartDelimiter(c))
				{
					if (level <= 0)
					{
						warnNoMatch();
						return false;
					}
					--level;
					++i;
				}
				else if ((nestedPairs && isEndDelimiter(c)) || (!nestedPairs && c == startDelim))
				{
					++level;
					if (level >= maxNest)
					{
						warnNoMatch();
						return false;
					}
					nestArray[level] = matchingDelimiter(c);
				}
				else if ((c == QLatin1Char('\"') && allowDoubleQuotes) ||
				         (c == QLatin1Char('\'') && allowSingleQuotes))
				{
					const bool escapeSingle = escapeSingleQuotes && c == QLatin1Char('\'');
					const bool escapeDouble = escapeDoubleQuotes && c == QLatin1Char('\"');
					const bool escapedQuote = escapeSingle || escapeDouble;
					for (int j = i - 1; j >= 0; --j)
					{
						const QChar qc = text.at(j);
						if ((qc == QLatin1Char('\\') && backslashEscape && escapedQuote) ||
						    (qc == QLatin1Char('%') && percentEscape && escapedQuote))
						{
							--j;
						}
						else if (qc == c)
						{
							i = j;
							break;
						}
					}
				}
			}

			if (matchStart == matchEnd)
			{
				warnNoMatch();
				return false;
			}
		}

		QTextCursor out = edit->textCursor();
		if (selectRange)
		{
			out.setPosition(matchStart);
			out.setPosition(matchEnd + 1, QTextCursor::KeepAnchor);
		}
		else
		{
			if (backwards)
			{
				out.setPosition(matchStart);
				out.setPosition(matchStart);
			}
			else
			{
				out.setPosition(matchEnd);
				out.setPosition(matchEnd);
			}
		}
		edit->setTextCursor(out);
		return true;
	}
} // namespace QMudBraceMatch
