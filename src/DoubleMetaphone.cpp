/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: DoubleMetaphone.cpp
 * Role: Double Metaphone phonetic algorithm implementation used for approximate text matching within QMud.
 */

#include "DoubleMetaphone.h"

#include <QString>
#include <QStringView>
#include <QtGlobal>

namespace
{
	class DoubleMetaphoneState
	{
			qsizetype       length{0};
			qsizetype       last{0};
			const qsizetype maxlength;
			bool            alternate{false};
			QString         primary;
			QString         secondary;

		public:
			DoubleMetaphoneState(QStringView text, qsizetype max);

			[[nodiscard]] QChar         GetAt(qsizetype at) const;
			void                        Insert(qsizetype index, QStringView text);
			void                        MakeUpper();
			[[nodiscard]] bool          SlavoGermanic() const;
			[[nodiscard]] bool          IsVowel(qsizetype at) const;
			void                        MetaphAdd(QLatin1StringView main);
			void                        MetaphAdd(QLatin1StringView main, QLatin1StringView alt);
			template <qsizetype N> void MetaphAdd(const char (&main)[N])
			{
				MetaphAdd(toLatin1(main));
			}
			template <qsizetype N1, qsizetype N2>
			void MetaphAdd(const char (&main)[N1], const char (&alt)[N2])
			{
				MetaphAdd(toLatin1(main), toLatin1(alt));
			}
			template <typename... Patterns>
			[[nodiscard]] bool StringAt(const qsizetype start, const qsizetype spanLength,
			                            const Patterns &...patterns) const
			{
				static_assert(sizeof...(Patterns) > 0, "StringAt requires at least one pattern");
				if (start < 0)
					return false;
				const QString target = Mid(start, spanLength);
				return (patternMatches(target, toLatin1(patterns)) || ...);
			}
			void                                           DoubleMetaphone(QString &metaph, QString &metaph2);

			[[nodiscard]] qsizetype                        Find(QChar ch) const;
			[[nodiscard]] qsizetype                        Find(QLatin1StringView text) const;
			template <qsizetype N> [[nodiscard]] qsizetype Find(const char (&text)[N]) const
			{
				return Find(toLatin1(text));
			}
			[[nodiscard]] QString   Mid(qsizetype start, qsizetype len) const;
			[[nodiscard]] qsizetype GetLength() const;

		private:
			template <qsizetype N> static constexpr QLatin1StringView toLatin1(const char (&text)[N])
			{
				return {text, N - 1};
			}
			static constexpr QLatin1StringView toLatin1(const QLatin1StringView text)
			{
				return text;
			}
			static bool patternMatches(QStringView target, QLatin1StringView pattern);
			QString     s;
	};
} // namespace

DoubleMetaphoneState::DoubleMetaphoneState(const QStringView text, const qsizetype max)
    : maxlength(max), s(text.toString())
{
}

qsizetype DoubleMetaphoneState::Find(const QChar ch) const
{
	return s.indexOf(ch);
}

qsizetype DoubleMetaphoneState::Find(const QLatin1StringView text) const
{
	return s.indexOf(QString::fromLatin1(text.data(), text.size()));
}

QString DoubleMetaphoneState::Mid(const qsizetype start, const qsizetype len) const
{
	if (start >= s.size())
		return {};
	return s.mid(start, qMax<qsizetype>(0, len));
}

qsizetype DoubleMetaphoneState::GetLength() const
{
	return s.size();
}

QChar DoubleMetaphoneState::GetAt(const qsizetype at) const
{
	if (at < 0 || at >= s.size())
		return {};
	return s.at(at);
}

void DoubleMetaphoneState::Insert(const qsizetype index, const QStringView text)
{
	if (text.isEmpty())
		return;
	const qsizetype clamped = qBound<qsizetype>(0, index, s.size());
	s.insert(clamped, text.toString());
}

void DoubleMetaphoneState::MakeUpper()
{
	s = s.toUpper();
}

bool DoubleMetaphoneState::SlavoGermanic() const
{
	return Find('W') > -1 || Find('K') > -1 || Find("CZ") > -1 || Find("WITZ") > -1;
}

void DoubleMetaphoneState::MetaphAdd(const QLatin1StringView main)
{
	if (!main.isEmpty())
	{
		const QString chunk = QString::fromLatin1(main.data(), main.size());
		primary += chunk;
		secondary += chunk;
	}
}

void DoubleMetaphoneState::MetaphAdd(const QLatin1StringView main, const QLatin1StringView alt)
{
	if (!main.isEmpty())
		primary += QString::fromLatin1(main.data(), main.size());
	if (!alt.isEmpty())
	{
		alternate = true;
		if (alt.front() != ' ')
			secondary += QString::fromLatin1(alt.data(), alt.size());
	}
	else if (!main.isEmpty() && (main.front() != ' '))
		secondary += QString::fromLatin1(main.data(), main.size());
}

bool DoubleMetaphoneState::IsVowel(const qsizetype at) const
{

	if (at >= length)
		return false;

	const QChar it = GetAt(at);
	return it == 'A' || it == 'E' || it == 'I' || it == 'O' || it == 'U' || it == 'Y';
}

bool DoubleMetaphoneState::patternMatches(const QStringView target, const QLatin1StringView pattern)
{
	return !pattern.isEmpty() && target == QString::fromLatin1(pattern.data(), pattern.size());
}

// Main algorithm loop.
void DoubleMetaphoneState::DoubleMetaphone(QString &metaph, QString &metaph2)
{

	qsizetype current = 0;

	length = GetLength();
	if (length < 1)
		return;
	last = length - 1; // zero based index

	alternate = false;

	MakeUpper();

	// pad the original string so that we can index beyond the edge of the world
	Insert(GetLength(), QStringLiteral("     "));

	// skip these when at start of word
	if (StringAt(0, 2, "GN", "KN", "PN", "WR", "PS", ""))
		current += 1;

	// Initial 'X' is pronounced 'Z' e.g. 'Xavier'
	if (GetAt(0) == 'X')
	{
		MetaphAdd("S"); //'Z' maps to 'S'
		current += 1;
	}

	// Main loop.
	while ((primary.size() < maxlength) || (secondary.size() < maxlength))
	{
		if (current >= length)
			break;

		switch (GetAt(current).unicode())
		{
		case 'A':
		case 'E':
		case 'I':
		case 'O':
		case 'U':
		case 'Y':
			if (current == 0)
				// all init vowels now map to 'A'
				MetaphAdd("A");
			current += 1;
			break;

		case 'B':

			//"-mb", e.g", "dumb", already skipped over...
			MetaphAdd("P");

			if (GetAt(current + 1) == 'B')
				current += 2;
			else
				current += 1;
			break;

		case 0x00C7:
			MetaphAdd("S");
			current += 1;
			break;

		case 'C':
			// various germanic
			if ((current > 1) && !IsVowel(current - 2) && StringAt((current - 1), 3, "ACH", "") &&
			    ((GetAt(current + 2) != 'I') &&
			     ((GetAt(current + 2) != 'E') || StringAt((current - 2), 6, "BACHER", "MACHER", ""))))
			{
				MetaphAdd("K");
				current += 2;
				break;
			}

			// special case 'caesar'
			if ((current == 0) && StringAt(current, 6, "CAESAR", ""))
			{
				MetaphAdd("S");
				current += 2;
				break;
			}

			// italian 'chianti'
			if (StringAt(current, 4, "CHIA", ""))
			{
				MetaphAdd("K");
				current += 2;
				break;
			}

			if (StringAt(current, 2, "CH", ""))
			{
				// find 'michael'
				if ((current > 0) && StringAt(current, 4, "CHAE", ""))
				{
					MetaphAdd("K", "X");
					current += 2;
					break;
				}

				// greek roots e.g. 'chemistry', 'chorus'
				if ((current == 0) &&
				    (StringAt((current + 1), 5, "HARAC", "HARIS", "") ||
				     StringAt((current + 1), 3, "HOR", "HYM", "HIA", "HEM", "")) &&
				    !StringAt(0, 5, "CHORE", ""))
				{
					MetaphAdd("K");
					current += 2;
					break;
				}

				// germanic, greek, or otherwise 'ch' for 'kh' sound
				if ((StringAt(0, 4, "VAN ", "VON ", "") || StringAt(0, 3, "SCH", ""))
				    // 'architect but not 'arch', 'orchestra', 'orchid'
				    || StringAt((current - 2), 6, "ORCHES", "ARCHIT", "ORCHID", "") ||
				    StringAt((current + 2), 1, "T", "S", "") ||
				    ((StringAt((current - 1), 1, "A", "O", "U", "E", "") || (current == 0))
				     // e.g., 'wachtler', 'wechsler', but not 'tichner'
				     && StringAt((current + 2), 1, "L", "R", "N", "M", "B", "H", "F", "V", "W", " ", "")))
				{
					MetaphAdd("K");
				}
				else
				{
					if (current > 0)
					{
						if (StringAt(0, 2, "MC", ""))
							// e.g., "McHugh"
							MetaphAdd("K");
						else
							MetaphAdd("X", "K");
					}
					else
						MetaphAdd("X");
				}
				current += 2;
				break;
			}
			// e.g, 'czerny'
			if (StringAt(current, 2, "CZ", "") && !StringAt((current - 2), 4, "WICZ", ""))
			{
				MetaphAdd("S", "X");
				current += 2;
				break;
			}

			// e.g., 'focaccia'
			if (StringAt((current + 1), 3, "CIA", ""))
			{
				MetaphAdd("X");
				current += 3;
				break;
			}

			// double 'C', but not if e.g. 'McClellan'
			if (StringAt(current, 2, "CC", "") && !(current == 1 && GetAt(0) == 'M'))
			{
				//'bellocchio' but not 'bacchus'
				if (StringAt((current + 2), 1, "I", "E", "H", "") && !StringAt((current + 2), 2, "HU", ""))
				{
					//'accident', 'accede' 'succeed'
					if (((current == 1) && (GetAt(current - 1) == 'A')) ||
					    StringAt((current - 1), 5, "UCCEE", "UCCES", ""))
						MetaphAdd("KS");
					//'bacci', 'bertucci', other italian
					else
						MetaphAdd("X");
					current += 3;
					break;
				}
				// Pierce's rule
				MetaphAdd("K");
				current += 2;
				break;
			}

			if (StringAt(current, 2, "CK", "CG", "CQ", ""))
			{
				MetaphAdd("K");
				current += 2;
				break;
			}

			if (StringAt(current, 2, "CI", "CE", "CY", ""))
			{
				// italian vs. english
				if (StringAt(current, 3, "CIO", "CIE", "CIA", ""))
					MetaphAdd("S", "X");
				else
					MetaphAdd("S");
				current += 2;
				break;
			}

			MetaphAdd("K");

			// name sent in 'mac caffrey', 'mac gregor
			if (StringAt((current + 1), 2, " C", " Q", " G", ""))
				current += 3;
			else if (StringAt((current + 1), 1, "C", "K", "Q", "") &&
			         !StringAt((current + 1), 2, "CE", "CI", ""))
				current += 2;
			else
				current += 1;
			break;

		case 'D':
			if (StringAt(current, 2, "DG", ""))
			{
				if (StringAt((current + 2), 1, "I", "E", "Y", ""))
				{
					// e.g. 'edge'
					MetaphAdd("J");
					current += 3;
					break;
				}
				// e.g. 'edgar'
				MetaphAdd("TK");
				current += 2;
				break;
			}

			if (StringAt(current, 2, "DT", "DD", ""))
			{
				MetaphAdd("T");
				current += 2;
				break;
			}

			MetaphAdd("T");
			current += 1;
			break;

		case 'F':
			if (GetAt(current + 1) == 'F')
				current += 2;
			else
				current += 1;
			MetaphAdd("F");
			break;

		case 'G':
			if (GetAt(current + 1) == 'H')
			{
				if ((current > 0) && !IsVowel(current - 1))
				{
					MetaphAdd("K");
					current += 2;
					break;
				}

				if (current < 3)
				{
					//'ghislane', ghiradelli
					if (current == 0)
					{
						if (GetAt(current + 2) == 'I')
							MetaphAdd("J");
						else
							MetaphAdd("K");
						current += 2;
						break;
					}
				}
				// Parker's rule (with some further refinements) - e.g., 'hugh'
				if (((current > 1) && StringAt((current - 2), 1, "B", "H", "D", ""))
				    // e.g., 'bough'
				    || ((current > 2) && StringAt((current - 3), 1, "B", "H", "D", ""))
				    // e.g., 'broughton'
				    || ((current > 3) && StringAt((current - 4), 1, "B", "H", "")))
				{
					current += 2;
					break;
				}
				// e.g., 'laugh', 'McLaughlin', 'cough', 'gough', 'rough', 'tough'
				if ((current > 2) && (GetAt(current - 1) == 'U') &&
				    StringAt((current - 3), 1, "C", "G", "L", "R", "T", ""))
				{
					MetaphAdd("F");
				}
				else if ((current > 0) && GetAt(current - 1) != 'I')
					MetaphAdd("K");

				current += 2;
				break;
			}

			if (GetAt(current + 1) == 'N')
			{
				if ((current == 1) && IsVowel(0) && !SlavoGermanic())
				{
					MetaphAdd("KN", "N");
				}
				else
					// not e.g. 'cagney'
					if (!StringAt((current + 2), 2, "EY", "") && (GetAt(current + 1) != 'Y') &&
					    !SlavoGermanic())
					{
						MetaphAdd("N", "KN");
					}
					else
						MetaphAdd("KN");
				current += 2;
				break;
			}

			//'tagliaro'
			if (StringAt((current + 1), 2, "LI", "") && !SlavoGermanic())
			{
				MetaphAdd("KL", "L");
				current += 2;
				break;
			}

			//-ges-,-gep-,-gel-, -gie- at beginning
			if ((current == 0) &&
			    ((GetAt(current + 1) == 'Y') || StringAt((current + 1), 2, "ES", "EP", "EB", "EL", "EY", "IB",
			                                             "IL", "IN", "IE", "EI", "ER", "")))
			{
				MetaphAdd("K", "J");
				current += 2;
				break;
			}

			// -ger-,  -gy-
			if ((StringAt((current + 1), 2, "ER", "") || (GetAt(current + 1) == 'Y')) &&
			    !StringAt(0, 6, "DANGER", "RANGER", "MANGER", "") &&
			    !StringAt((current - 1), 1, "E", "I", "") && !StringAt((current - 1), 3, "RGY", "OGY", ""))
			{
				MetaphAdd("K", "J");
				current += 2;
				break;
			}

			// italian e.g, 'biaggi'
			if (StringAt((current + 1), 1, "E", "I", "Y", "") ||
			    StringAt((current - 1), 4, "AGGI", "OGGI", ""))
			{
				// obvious germanic
				if ((StringAt(0, 4, "VAN ", "VON ", "") || StringAt(0, 3, "SCH", "")) ||
				    StringAt((current + 1), 2, "ET", ""))
					MetaphAdd("K");
				else
					// always soft if french ending
					if (StringAt((current + 1), 4, "IER ", ""))
						MetaphAdd("J");
					else
						MetaphAdd("J", "K");
				current += 2;
				break;
			}

			if (GetAt(current + 1) == 'G')
				current += 2;
			else
				current += 1;
			MetaphAdd("K");
			break;

		case 'H':
			// only keep if first & before vowel or btw. 2 vowels
			if (((current == 0) || IsVowel(current - 1)) && IsVowel(current + 1))
			{
				MetaphAdd("H");
				current += 2;
			}
			else // also takes care of 'HH'
				current += 1;
			break;

		case 'J':
			// obvious spanish, 'jose', 'san jacinto'
			if (StringAt(current, 4, "JOSE", "") || StringAt(0, 4, "SAN ", ""))
			{
				if (((current == 0) && (GetAt(current + 4) == ' ')) || StringAt(0, 4, "SAN ", ""))
					MetaphAdd("H");
				else
				{
					MetaphAdd("J", "H");
				}
				current += 1;
				break;
			}

			if ((current == 0) && !StringAt(current, 4, "JOSE", ""))
				MetaphAdd("J", "A"); // Yankelovich/Jankelowicz
			else
				// spanish pron. of e.g. 'bajador'
				if (IsVowel(current - 1) && !SlavoGermanic() &&
				    ((GetAt(current + 1) == 'A') || (GetAt(current + 1) == 'O')))
					MetaphAdd("J", "H");
				else if (current == last)
					MetaphAdd("J", " ");
				else if (!StringAt((current + 1), 1, "L", "T", "K", "S", "N", "M", "B", "Z", "") &&
				         !StringAt((current - 1), 1, "S", "K", "L", ""))
					MetaphAdd("J");

			if (GetAt(current + 1) == 'J') // it could happen!
				current += 2;
			else
				current += 1;
			break;

		case 'K':
			if (GetAt(current + 1) == 'K')
				current += 2;
			else
				current += 1;
			MetaphAdd("K");
			break;

		case 'L':
			if (GetAt(current + 1) == 'L')
			{
				// spanish e.g. 'cabrillo', 'gallegos'
				if (((current == (length - 3)) && StringAt((current - 1), 4, "ILLO", "ILLA", "ALLE", "")) ||
				    ((StringAt((last - 1), 2, "AS", "OS", "") || StringAt(last, 1, "A", "O", "")) &&
				     StringAt((current - 1), 4, "ALLE", "")))
				{
					MetaphAdd("L", " ");
					current += 2;
					break;
				}
				current += 2;
			}
			else
				current += 1;
			MetaphAdd("L");
			break;

		case 'M':
			if ((StringAt((current - 1), 3, "UMB", "") &&
			     (((current + 1) == last) || StringAt((current + 2), 2, "ER", "")))
			    //'dumb','thumb'
			    || (GetAt(current + 1) == 'M'))
				current += 2;
			else
				current += 1;
			MetaphAdd("M");
			break;

		case 'N':
			if (GetAt(current + 1) == 'N')
				current += 2;
			else
				current += 1;
			MetaphAdd("N");
			break;

		case 0x00D1:
			current += 1;
			MetaphAdd("N");
			break;

		case 'P':
			if (GetAt(current + 1) == 'H')
			{
				MetaphAdd("F");
				current += 2;
				break;
			}

			// also account for "campbell", "raspberry"
			if (StringAt((current + 1), 1, "P", "B", ""))
				current += 2;
			else
				current += 1;
			MetaphAdd("P");
			break;

		case 'Q':
			if (GetAt(current + 1) == 'Q')
				current += 2;
			else
				current += 1;
			MetaphAdd("K");
			break;

		case 'R':
			// french e.g. 'rogier', but exclude 'hochmeier'
			if ((current == last) && !SlavoGermanic() && StringAt((current - 2), 2, "IE", "") &&
			    !StringAt((current - 4), 2, "ME", "MA", ""))
				MetaphAdd("", "R");
			else
				MetaphAdd("R");

			if (GetAt(current + 1) == 'R')
				current += 2;
			else
				current += 1;
			break;

		case 'S':
			// special cases 'island', 'isle', 'carlisle', 'carlysle'
			if (StringAt((current - 1), 3, "ISL", "YSL", ""))
			{
				current += 1;
				break;
			}

			// special case 'sugar-'
			if ((current == 0) && StringAt(current, 5, "SUGAR", ""))
			{
				MetaphAdd("X", "S");
				current += 1;
				break;
			}

			if (StringAt(current, 2, "SH", ""))
			{
				// germanic
				if (StringAt((current + 1), 4, "HEIM", "HOEK", "HOLM", "HOLZ", ""))
					MetaphAdd("S");
				else
					MetaphAdd("X");
				current += 2;
				break;
			}

			// italian & armenian
			if (StringAt(current, 3, "SIO", "SIA", "") || StringAt(current, 4, "SIAN", ""))
			{
				if (!SlavoGermanic())
					MetaphAdd("S", "X");
				else
					MetaphAdd("S");
				current += 3;
				break;
			}

			// german & anglicizations, e.g. 'smith' match 'schmidt', 'snider' match 'schneider'
			// also, -sz- in slavic language altho in hungarian it is pronounced 's'
			if (((current == 0) && StringAt((current + 1), 1, "M", "N", "L", "W", "")) ||
			    StringAt((current + 1), 1, "Z", ""))
			{
				MetaphAdd("S", "X");
				if (StringAt((current + 1), 1, "Z", ""))
					current += 2;
				else
					current += 1;
				break;
			}

			if (StringAt(current, 2, "SC", ""))
			{
				// Schlesinger's rule
				if (GetAt(current + 2) == 'H')
				{
					// dutch origin, e.g. 'school', 'schooner'
					if (StringAt((current + 3), 2, "OO", "ER", "EN", "UY", "ED", "EM", ""))
					{
						//'schermerhorn', 'schenker'
						if (StringAt((current + 3), 2, "ER", "EN", ""))
						{
							MetaphAdd("X", "SK");
						}
						else
							MetaphAdd("SK");
						current += 3;
						break;
					}
					if ((current == 0) && !IsVowel(3) && (GetAt(3) != 'W'))
						MetaphAdd("X", "S");
					else
						MetaphAdd("X");
					current += 3;
					break;
				}

				if (StringAt((current + 2), 1, "I", "E", "Y", ""))
				{
					MetaphAdd("S");
					current += 3;
					break;
				}
				MetaphAdd("SK");
				current += 3;
				break;
			}

			// french e.g. 'resnais', 'artois'
			if ((current == last) && StringAt((current - 2), 2, "AI", "OI", ""))
				MetaphAdd("", "S");
			else
				MetaphAdd("S");

			if (StringAt((current + 1), 1, "S", "Z", ""))
				current += 2;
			else
				current += 1;
			break;

		case 'T':
			if (StringAt(current, 4, "TION", ""))
			{
				MetaphAdd("X");
				current += 3;
				break;
			}

			if (StringAt(current, 3, "TIA", "TCH", ""))
			{
				MetaphAdd("X");
				current += 3;
				break;
			}

			if (StringAt(current, 2, "TH", "") || StringAt(current, 3, "TTH", ""))
			{
				// special case 'thomas', 'thames' or germanic
				if (StringAt((current + 2), 2, "OM", "AM", "") || StringAt(0, 4, "VAN ", "VON ", "") ||
				    StringAt(0, 3, "SCH", ""))
				{
					MetaphAdd("T");
				}
				else
				{
					MetaphAdd("0", "T");
				}
				current += 2;
				break;
			}

			if (StringAt((current + 1), 1, "T", "D", ""))
				current += 2;
			else
				current += 1;
			MetaphAdd("T");
			break;

		case 'V':
			if (GetAt(current + 1) == 'V')
				current += 2;
			else
				current += 1;
			MetaphAdd("F");
			break;

		case 'W':
			// can also be in middle of word
			if (StringAt(current, 2, "WR", ""))
			{
				MetaphAdd("R");
				current += 2;
				break;
			}

			if ((current == 0) && (IsVowel(current + 1) || StringAt(current, 2, "WH", "")))
			{
				// Wasserman should match Vasserman
				if (IsVowel(current + 1))
					MetaphAdd("A", "F");
				else
					// need Uomo to match Womo
					MetaphAdd("A");
			}

			// Arnow should match Arnoff
			if (((current == last) && IsVowel(current - 1)) ||
			    StringAt((current - 1), 5, "EWSKI", "EWSKY", "OWSKI", "OWSKY", "") ||
			    StringAt(0, 3, "SCH", ""))
			{
				MetaphAdd("", "F");
				current += 1;
				break;
			}

			// polish e.g. 'filipowicz'
			if (StringAt(current, 4, "WICZ", "WITZ", ""))
			{
				MetaphAdd("TS", "FX");
				current += 4;
				break;
			}

			current += 1;
			break;

		case 'X':
			// french e.g. breaux
			if (!((current == last) && (StringAt((current - 3), 3, "IAU", "EAU", "") ||
			                            StringAt((current - 2), 2, "AU", "OU", ""))))
				MetaphAdd("KS");

			if (StringAt((current + 1), 1, "C", "X", ""))
				current += 2;
			else
				current += 1;
			break;

		case 'Z':
			// chinese pinyin e.g. 'zhao'
			if (GetAt(current + 1) == 'H')
			{
				MetaphAdd("J");
				current += 2;
				break;
			}
			if (StringAt((current + 1), 2, "ZO", "ZI", "ZA", "") ||
			    (SlavoGermanic() && ((current > 0) && GetAt(current - 1) != 'T')))
			{
				MetaphAdd("S", "TS");
			}
			else
				MetaphAdd("S");

			if (GetAt(current + 1) == 'Z')
				current += 2;
			else
				current += 1;
			break;

		default:
			current += 1;
		}
	}

	metaph = primary;
	// only give back maxlength char metaph
	if (metaph.size() > maxlength)
		metaph.resize(maxlength);
	if (alternate || secondary != primary)
	{
		metaph2 = secondary;
		if (metaph2.size() > maxlength)
			metaph2.resize(maxlength);
	}
}

QPair<QString, QString> qmudDoubleMetaphone(const QString &input, const int length)
{
	DoubleMetaphoneState m(input, length);
	QString              first;
	QString              second;
	m.DoubleMetaphone(first, second);
	return {first, second};
}
