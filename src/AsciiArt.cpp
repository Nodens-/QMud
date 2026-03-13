/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: AsciiArt.cpp
 * Role: ASCII-art generation and transformation routines used by scripting and text-output utility features.
 */

#include "AsciiArt.h"

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QTextStream>
#include <QVector>
#include <limits>

namespace
{
	constexpr int kDefaultColumns = 500;

	constexpr int SM_SMUSH     = 128;
	constexpr int SM_KERN      = 64;
	constexpr int SM_EQUAL     = 1;
	constexpr int SM_LOWLINE   = 2;
	constexpr int SM_HIERARCHY = 4;
	constexpr int SM_PAIR      = 8;
	constexpr int SM_BIGX      = 16;
	constexpr int SM_HARDBLANK = 32;

	constexpr int SMO_NO    = 0;
	constexpr int SMO_YES   = 1;
	constexpr int SMO_FORCE = 2;

	struct FigletState
	{
			QHash<int, QVector<QByteArray>> glyphs;
			char                            hardblank{'$'};
			int                             charheight{0};
			int                             right2left{0};
			int                             smushmode{0};
			int                             smushoverride{SMO_NO};
			int                             outlinelenlimit{kDefaultColumns - 1};
			QVector<QByteArray>             outputline;
			int                             outlinelen{0};
			QVector<QByteArray>             currchar;
			int                             currcharwidth{0};
			int                             previouscharwidth{0};
	};

	bool parseInt(const QString &text, int *value)
	{
		if (!value)
			return false;
		bool      ok     = false;
		const int parsed = text.toInt(&ok);
		if (!ok)
			return false;
		*value = parsed;
		return true;
	}

	int toIntSaturated(const qsizetype value)
	{
		if (value > static_cast<qsizetype>(std::numeric_limits<int>::max()))
			return std::numeric_limits<int>::max();
		return static_cast<int>(value);
	}

	QByteArray stripGlyphEndMarker(const QString &line)
	{
		QByteArray bytes = line.toLatin1();
		int        k     = toIntSaturated(bytes.size()) - 1;
		while (k >= 0 &&
		       (bytes.at(k) == ' ' || bytes.at(k) == '\t' || bytes.at(k) == '\r' || bytes.at(k) == '\n'))
			--k;
		if (k < 0)
			return {};

		const char endChar = bytes.at(k);
		while (k >= 0 && bytes.at(k) == endChar)
			--k;
		bytes.truncate(k + 1);
		return bytes;
	}

	QVector<QByteArray> missingGlyph(const int charheight)
	{
		QVector<QByteArray> rows;
		rows.resize(qMax(1, charheight));
		return rows;
	}

	QVector<QByteArray> lookupGlyph(const FigletState &s, const int codepoint)
	{
		auto it = s.glyphs.constFind(codepoint);
		if (it != s.glyphs.constEnd())
			return it.value();
		it = s.glyphs.constFind(0);
		if (it != s.glyphs.constEnd())
			return it.value();
		return missingGlyph(s.charheight);
	}

	char smushem(const FigletState &s, const char lch, const char rch)
	{
		if (lch == ' ')
			return rch;
		if (rch == ' ')
			return lch;

		if (s.previouscharwidth < 2 || s.currcharwidth < 2)
			return '\0';
		if ((s.smushmode & SM_SMUSH) == 0)
			return '\0';

		if ((s.smushmode & 63) == 0)
		{
			if (lch == s.hardblank)
				return rch;
			if (rch == s.hardblank)
				return lch;
			if (s.right2left == 1)
				return lch;
			return rch;
		}

		if (s.smushmode & SM_HARDBLANK && lch == s.hardblank && rch == s.hardblank)
			return lch;

		if (lch == s.hardblank || rch == s.hardblank)
			return '\0';

		if (s.smushmode & SM_EQUAL && lch == rch)
			return lch;

		auto inSet = [](const char ch, const char *set) -> bool { return QByteArray(set).contains(ch); };

		if (s.smushmode & SM_LOWLINE)
		{
			if (lch == '_' && inSet(rch, "|/\\[]{}()<>"))
				return rch;
			if (rch == '_' && inSet(lch, "|/\\[]{}()<>"))
				return lch;
		}

		if (s.smushmode & SM_HIERARCHY)
		{
			if (lch == '|' && inSet(rch, "/\\[]{}()<>"))
				return rch;
			if (rch == '|' && inSet(lch, "/\\[]{}()<>"))
				return lch;
			if (inSet(lch, "/\\") && inSet(rch, "[]{}()<>"))
				return rch;
			if (inSet(rch, "/\\") && inSet(lch, "[]{}()<>"))
				return lch;
			if (inSet(lch, "[]") && inSet(rch, "{}()<>"))
				return rch;
			if (inSet(rch, "[]") && inSet(lch, "{}()<>"))
				return lch;
			if (inSet(lch, "{}") && inSet(rch, "()<>"))
				return rch;
			if (inSet(rch, "{}") && inSet(lch, "()<>"))
				return lch;
			if (inSet(lch, "()") && inSet(rch, "<>"))
				return rch;
			if (inSet(rch, "()") && inSet(lch, "<>"))
				return lch;
		}

		if (s.smushmode & SM_PAIR)
		{
			if ((lch == '[' && rch == ']') || (rch == '[' && lch == ']'))
				return '|';
			if ((lch == '{' && rch == '}') || (rch == '{' && lch == '}'))
				return '|';
			if ((lch == '(' && rch == ')') || (rch == '(' && lch == ')'))
				return '|';
		}

		if (s.smushmode & SM_BIGX)
		{
			if (lch == '/' && rch == '\\')
				return '|';
			if (rch == '/' && lch == '\\')
				return 'Y';
			if (lch == '>' && rch == '<')
				return 'X';
		}

		return '\0';
	}

	int smushamt(FigletState &s)
	{
		if ((s.smushmode & (SM_SMUSH | SM_KERN)) == 0)
			return 0;

		int maxsmush = s.currcharwidth;
		for (int row = 0; row < s.charheight; ++row)
		{
			int  amt    = 0;
			int  linebd = 0;
			int  charbd = 0;
			char ch1    = '\0';
			char ch2    = '\0';

			if (s.right2left)
			{
				const QByteArray &curr = s.currchar[row];
				const QByteArray &out  = s.outputline[row];
				charbd                 = toIntSaturated(curr.size()) - 1;
				const int outSize      = toIntSaturated(out.size());
				while (charbd > 0)
				{
					ch1 = curr.at(charbd);
					if (ch1 != '\0' && ch1 != ' ')
						break;
					--charbd;
				}
				linebd = 0;
				while (linebd < outSize)
				{
					ch2 = out.at(linebd);
					if (ch2 != ' ')
						break;
					++linebd;
				}
				amt = linebd + s.currcharwidth - 1 - charbd;
			}
			else
			{
				QByteArray       &out      = s.outputline[row];
				const QByteArray &curr     = s.currchar[row];
				const int         outSize  = toIntSaturated(out.size());
				const int         currSize = toIntSaturated(curr.size());
				linebd                     = outSize - 1;
				while (linebd > 0)
				{
					ch1 = out.at(linebd);
					if (ch1 != '\0' && ch1 != ' ')
						break;
					--linebd;
				}
				charbd = 0;
				while (charbd < currSize)
				{
					ch2 = curr.at(charbd);
					if (ch2 != ' ')
						break;
					++charbd;
				}
				amt = charbd + s.outlinelen - 1 - linebd;
			}

			if (const bool canSmush =
			        ch1 == '\0' || ch1 == ' ' || (ch2 != '\0' && smushem(s, ch1, ch2) != '\0');
			    canSmush)
				++amt;

			if (amt < maxsmush)
				maxsmush = amt;
		}

		return maxsmush;
	}

	bool addchar(FigletState &s, const int codepoint)
	{
		s.currchar          = lookupGlyph(s, codepoint);
		s.previouscharwidth = s.currcharwidth;
		s.currcharwidth     = s.currchar.isEmpty() ? 0 : toIntSaturated(s.currchar.first().size());
		int smushamount     = smushamt(s);
		if (smushamount < 0)
			smushamount = 0;
		if (smushamount > s.currcharwidth)
			smushamount = s.currcharwidth;
		// Nothing can be smushed before we have existing output columns.
		if (smushamount > s.outlinelen)
			smushamount = s.outlinelen;

		if (s.outlinelen + s.currcharwidth - smushamount > s.outlinelenlimit)
			return false;

		for (int row = 0; row < s.charheight; ++row)
		{
			if (s.right2left)
			{
				QByteArray temp = s.currchar[row];
				for (int k = 0; k < smushamount; ++k)
					temp[s.currcharwidth - smushamount + k] =
					    smushem(s, temp[s.currcharwidth - smushamount + k], s.outputline[row][k]);
				temp += s.outputline[row].mid(smushamount);
				s.outputline[row] = temp;
			}
			else
			{
				for (int k = 0; k < smushamount; ++k)
					s.outputline[row][s.outlinelen - smushamount + k] =
					    smushem(s, s.outputline[row][s.outlinelen - smushamount + k], s.currchar[row][k]);
				s.outputline[row] += s.currchar[row].mid(smushamount);
			}
		}

		s.outlinelen = s.outputline.isEmpty() ? 0 : toIntSaturated(s.outputline.first().size());
		return true;
	}

	bool readGlyph(QTextStream &stream, const int charheight, QVector<QByteArray> &rows)
	{
		rows.clear();
		rows.reserve(charheight);
		for (int row = 0; row < charheight; ++row)
		{
			if (stream.atEnd())
				return false;
			rows.push_back(stripGlyphEndMarker(stream.readLine()));
		}
		return true;
	}

	bool loadFont(FigletState &s, const QString &path, QString *errorMessage)
	{
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Could not open FIGlet font: %1").arg(path);
			return false;
		}

		QTextStream stream(&file);
		if (stream.atEnd())
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Empty FIGlet font file.");
			return false;
		}

		const QString header = stream.readLine();
		if (!header.startsWith(QStringLiteral("flf2")) || header.size() < 6)
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Not a FIGlet 2 font file.");
			return false;
		}

		s.hardblank             = header.at(5).toLatin1();
		const QStringList parts = header.mid(6).simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
		if (parts.size() < 5)
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Invalid FIGlet font header.");
			return false;
		}

		int maxlen        = 1;
		int oldLayout     = 0;
		int commentLines  = 0;
		int ffRightToLeft = 0;
		int smush2        = 0;
		if (!parseInt(parts.value(0), &s.charheight) || !parseInt(parts.value(2), &maxlen) ||
		    !parseInt(parts.value(3), &oldLayout) || !parseInt(parts.value(4), &commentLines))
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Invalid FIGlet numeric header fields.");
			return false;
		}
		if (parts.size() >= 6)
			parseInt(parts.value(5), &ffRightToLeft);
		if (parts.size() >= 7)
			parseInt(parts.value(6), &smush2);
		else
		{
			if (oldLayout == 0)
				smush2 = SM_KERN;
			else if (oldLayout < 0)
				smush2 = 0;
			else
			{
				const int smushMask = oldLayout & 31;
				smush2              = smushMask | SM_SMUSH;
			}
		}

		if (s.charheight < 1)
			s.charheight = 1;
		if (maxlen < 1)
			maxlen = 1;
		Q_UNUSED(maxlen);

		if (s.smushoverride == SMO_NO)
			s.smushmode = smush2;
		else if (s.smushoverride == SMO_FORCE)
			s.smushmode |= smush2;

		s.right2left = ffRightToLeft;

		for (int i = 0; i < commentLines && !stream.atEnd(); ++i)
			stream.readLine();

		s.glyphs.insert(0, missingGlyph(s.charheight));

		QVector<QByteArray> rows;
		for (int code = 32; code <= 126; ++code)
		{
			if (!readGlyph(stream, s.charheight, rows))
			{
				if (errorMessage)
					*errorMessage = QStringLiteral("Unexpected end of FIGlet font.");
				return false;
			}
			s.glyphs.insert(code, rows);
		}

		for (const int germanCodes[7] = {196, 214, 220, 228, 246, 252, 223}; const int code : germanCodes)
		{
			if (!readGlyph(stream, s.charheight, rows))
			{
				if (errorMessage)
					*errorMessage = QStringLiteral("Unexpected end of FIGlet font.");
				return false;
			}
			s.glyphs.insert(code, rows);
		}

		while (!stream.atEnd())
		{
			const QString line = stream.readLine().trimmed();
			if (line.isEmpty())
				continue;
			bool      ok   = false;
			const int code = line.toInt(&ok);
			if (!ok)
				continue;
			if (!readGlyph(stream, s.charheight, rows))
				break;
			s.glyphs.insert(code, rows);
		}

		return true;
	}
} // namespace

namespace QMudAsciiArt
{
	bool render(const QString &text, const QString &fontFilePath, const int layoutMode, QStringList *outLines,
	            QString *errorMessage)
	{
		if (!outLines)
			return false;

		FigletState s;
		switch (layoutMode)
		{
		case 1: // full smush
			s.smushmode     = SM_SMUSH;
			s.smushoverride = SMO_FORCE;
			break;
		case 2: // kern
			s.smushmode     = SM_KERN;
			s.smushoverride = SMO_YES;
			break;
		case 3: // full width
			s.smushmode     = 0;
			s.smushoverride = SMO_YES;
			break;
		case 4: // overlap
			s.smushmode     = SM_SMUSH;
			s.smushoverride = SMO_YES;
			break;
		default: // default smush mode from font
			s.smushoverride = SMO_NO;
			break;
		}

		if (!loadFont(s, fontFilePath, errorMessage))
			return false;

		s.outputline.resize(s.charheight);
		s.outlinelen        = 0;
		s.currcharwidth     = 0;
		s.previouscharwidth = 0;

		const QString &normalized = text;
		for (int i = 0; i < normalized.size(); ++i)
		{
			const QChar qc = normalized.at(i);
			int         c  = qc.unicode();
			if (qc.isSpace())
				c = qc.unicode() == 0x0009u || qc.unicode() == 0x0020u ? 0x0020 : 0x000Au;

			if ((c > 0 && c < ' ' && c != '\n') || c == 127)
				continue;

			if (!addchar(s, c))
				break;
		}

		outLines->clear();
		outLines->reserve(s.outputline.size());
		for (const auto &outputLines = s.outputline; const QByteArray &row : outputLines)
		{
			QByteArray copy = row;
			copy.replace(s.hardblank, ' ');
			outLines->push_back(QString::fromLatin1(copy));
		}

		return true;
	}
} // namespace QMudAsciiArt
