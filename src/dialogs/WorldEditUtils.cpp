/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldEditUtils.cpp
 * Role: Common dialog helper implementations reused by world object editors to enforce consistent validation rules.
 */

#include "dialogs/WorldEditUtils.h"
#include "AppController.h"
#include "FontUtils.h"
#include "WorldOptions.h"

#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <memory>

namespace
{
	bool isAsciiSpace(const char ch)
	{
		switch (ch)
		{
		case ' ':
		case '\t':
		case '\n':
		case '\r':
		case '\f':
		case '\v':
			return true;
		default:
			return false;
		}
	}

	bool isAsciiDigit(const char ch)
	{
		return ch >= '0' && ch <= '9';
	}

	bool isAsciiAlpha(const char ch)
	{
		return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
	}

	bool isAsciiAlnum(const char ch)
	{
		return isAsciiAlpha(ch) || isAsciiDigit(ch);
	}

	char asciiToUpper(const char ch)
	{
		if (ch >= 'a' && ch <= 'z')
			return static_cast<char>(ch - ('a' - 'A'));
		return ch;
	}

	char asciiToLower(const char ch)
	{
		if (ch >= 'A' && ch <= 'Z')
			return static_cast<char>(ch + ('a' - 'A'));
		return ch;
	}

	bool isActionCode(const char ch)
	{
		const char upper = asciiToUpper(ch);
		return upper == 'C' || upper == 'O' || upper == 'L' || upper == 'K';
	}

	bool fixedFontEnabled()
	{
		const auto *app = AppController::instance();
		if (!app)
			return false;
		return app->getGlobalOption(QStringLiteral("FixedFontForEditing")).toInt() != 0;
	}

	bool tabInsertsTab()
	{
		const auto *app = AppController::instance();
		if (!app)
			return false;
		return app->getGlobalOption(QStringLiteral("TabInsertsTabInMultiLineDialogs")).toInt() != 0;
	}

	QFont fixedPitchFont()
	{
		QFont       font = qmudPreferredMonospaceFont();
		const auto *app  = AppController::instance();
		if (!app)
			return font;

		const QString family = app->getGlobalOption(QStringLiteral("FixedPitchFont")).toString();
		const int     size   = app->getGlobalOption(QStringLiteral("FixedPitchFontSize")).toInt();
		qmudApplyMonospaceFallback(font, family);
		if (size > 0)
			font.setPointSize(size);
		return font;
	}

	QString makeSpeedwalkErrorString(const QString &message)
	{
		return QStringLiteral("*") + message;
	}

	struct SendToItem
	{
			int         value;
			const char *label;
			const char *token;
	};
} // namespace

void WorldEditUtils::populateSendToCombo(QComboBox *combo)
{
	if (!combo)
		return;

	combo->clear();
	const SendToItem items[] = {
	    {eSendToWorld,           "World",               "world"            },
	    {eSendToCommand,         "Command window",      "command"          },
	    {eSendToOutput,          "Output window",       "output"           },
	    {eSendToStatus,          "Status line",         "status"           },
	    {eSendToNotepad,         "Notepad",             "notepad"          },
	    {eAppendToNotepad,       "Append to notepad",   "notepad_append"   },
	    {eSendToLogFile,         "Log file",            "log"              },
	    {eReplaceNotepad,        "Replace notepad",     "notepad_replace"  },
	    {eSendToCommandQueue,    "Command queue",       "queue"            },
	    {eSendToVariable,        "Variable",            "variable"         },
	    {eSendToExecute,         "Execute",             "execute"          },
	    {eSendToSpeedwalk,       "Speedwalk",           "speedwalk"        },
	    {eSendToScript,          "Script",              "script"           },
	    {eSendImmediate,         "Send immediate",      "immediate"        },
	    {eSendToScriptAfterOmit, "Script (after omit)", "script_after_omit"}
    };

	for (const auto &item : items)
		combo->addItem(QString::fromLatin1(item.label), item.value);
}

QString WorldEditUtils::sendToLabel(const int sendTo)
{
	const SendToItem items[] = {
	    {eSendToWorld,           "World",               "world"            },
	    {eSendToCommand,         "Command window",      "command"          },
	    {eSendToOutput,          "Output window",       "output"           },
	    {eSendToStatus,          "Status line",         "status"           },
	    {eSendToNotepad,         "Notepad",             "notepad"          },
	    {eAppendToNotepad,       "Append to notepad",   "notepad_append"   },
	    {eSendToLogFile,         "Log file",            "log"              },
	    {eReplaceNotepad,        "Replace notepad",     "notepad_replace"  },
	    {eSendToCommandQueue,    "Command queue",       "queue"            },
	    {eSendToVariable,        "Variable",            "variable"         },
	    {eSendToExecute,         "Execute",             "execute"          },
	    {eSendToSpeedwalk,       "Speedwalk",           "speedwalk"        },
	    {eSendToScript,          "Script",              "script"           },
	    {eSendImmediate,         "Send immediate",      "immediate"        },
	    {eSendToScriptAfterOmit, "Script (after omit)", "script_after_omit"}
    };

	for (const auto &item : items)
	{
		if (item.value == sendTo)
			return QString::fromLatin1(item.token);
	}

	return QString::number(sendTo);
}

QString WorldEditUtils::convertToRegularExpression(const QString &matchString, const bool wholeLine,
                                                   const bool makeAsterisksWildcards)
{
	QString          strRegexp;
	int              iSize      = 0;
	const QByteArray inputBytes = matchString.toLocal8Bit();
	const char      *p          = inputBytes.constData();

	// count places where the size will get larger
	for (; *p; ++p)
	{
		const auto uch = static_cast<unsigned char>(*p);
		if (uch < ' ')
			iSize += 3; // non-printable 01 to 1F become \xhh
		else if (*p == '*' && makeAsterisksWildcards)
			iSize += 4; // * becomes .*?  (non-greedy wildcard)
		else if (!(isAsciiAlnum(*p) || *p == ' ' || uch >= 0x80))
			iSize++; // others are escaped, eg. ( becomes \(
	}

	// work out new buffer size
	strRegexp.reserve(matchString.length() + iSize + // escaped sequences
	                  2 +                            // ^ at start, and $ at end
	                  10);                           // 1 for null, plus 9 just in case

	// now copy across non-regexp, turning it into a regexp
	if (wholeLine)
		strRegexp += QLatin1Char('^'); // start of buffer marker

	for (p = inputBytes.constData(); *p; p++)
	{
		if (*p == '\n') // newlines become \n
		{
			strRegexp += QLatin1Char('\\');
			strRegexp += QLatin1Char('n');
		}
		else if (const auto uch = static_cast<unsigned char>(*p); uch < ' ')
		{
			strRegexp += QLatin1Char('\\');
			strRegexp += QLatin1Char('x');
			const auto     ch    = static_cast<unsigned char>(*p);
			constexpr char hex[] = "0123456789abcdef";
			strRegexp += QLatin1Char(hex[(ch >> 4) & 0x0F]);
			strRegexp += QLatin1Char(hex[ch & 0x0F]);
		}
		else if (isAsciiAlnum(*p) || *p == ' ' || static_cast<unsigned char>(*p) >= 0x80)
			strRegexp += QLatin1Char(*p);             // copy alphanumeric, spaces, across
		else if (*p == '*' && makeAsterisksWildcards) // wildcard
		{
			strRegexp += QLatin1Char('(');
			strRegexp += QLatin1Char('.');
			strRegexp += QLatin1Char('*');
			strRegexp += QLatin1Char('?');
			strRegexp += QLatin1Char(')');
		}
		else
		{ // non-alphanumeric are escaped out
			strRegexp += QLatin1Char('\\');
			strRegexp += QLatin1Char(*p);
		}
	} // end of scanning input buffer

	if (wholeLine)
		strRegexp += QLatin1Char('$'); // end of buffer marker

	return strRegexp;
}

QString WorldEditUtils::evaluateSpeedwalk(const QString &speedWalkString, const QString &filler)
{
	QString          strResult;
	QString          str;
	int              count      = 0;
	const auto      *app        = AppController::instance();
	const QByteArray inputBytes = speedWalkString.toLocal8Bit();
	const char      *p          = inputBytes.constData();

	while (*p) // until string runs out
	{
		// bypass spaces
		while (isAsciiSpace(*p))
			p++;

		// bypass comments
		if (*p == '{')
		{
			while (*p && *p != '}')
				p++;

			if (*p != '}')
				return makeSpeedwalkErrorString(
				    QStringLiteral("Comment code of '{' not terminated by a '}'"));
			p++;      // skip } symbol
			continue; // back to start of loop
		} // end of comment

		// get counter, if any
		count = 0;
		while (isAsciiDigit(*p))
		{
			count = count * 10 + (*p++ - '0');
			if (count > 99)
				return makeSpeedwalkErrorString(QStringLiteral("Speed walk counter exceeds 99"));
		} // end of having digit(s)

		// no counter, assume do once
		if (count == 0)
			count = 1;

		// bypass spaces after counter
		while (isAsciiSpace(*p))
			p++;

		if (count > 1 && *p == 0)
			return makeSpeedwalkErrorString(QStringLiteral("Speed walk counter not followed by an action"));

		if (count > 1 && *p == '{')
			return makeSpeedwalkErrorString(
			    QStringLiteral("Speed walk counter may not be followed by a comment"));

		// might have had trailing space
		if (*p == 0)
			break;

		if (isActionCode(*p))
		{
			if (count > 1)
				return makeSpeedwalkErrorString(QStringLiteral("Action code of C, O, L or K must not follow "
				                                               "a speed walk count (1-99)"));

			switch (asciiToUpper(*p++))
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
			} // end of switch

			// bypass spaces after open/close/lock/unlock
			while (isAsciiSpace(*p))
				p++;

			if (*p == 0 || asciiToUpper(*p) == 'F' || *p == '{')
				return makeSpeedwalkErrorString(QStringLiteral("Action code of C, O, L or K must be followed "
				                                               "by a direction"));

		} // end of C, O, L, K

		// work out which direction we are going
		switch (asciiToUpper(*p))
		{
		case 'N':
		case 'S':
		case 'E':
		case 'W':
		case 'U':
		case 'D':
		{
			// we know it will be in the list - look up the direction to send
			str = app ? app->mapDirectionToSend(QString(QChar(asciiToLower(*p)))) : QString();
		}
		break;

		case 'F':
			str = filler;
			break;
		case '(': // special string (eg. (ne/sw) )
		{
			str.clear();
			++p;
			while (*p && *p != ')')
				str += *p++; // add to string

			if (*p != ')')
				return makeSpeedwalkErrorString(QStringLiteral("Action code of '(' not terminated by a ')'"));
			if (const qsizetype iSlash = str.indexOf(QStringLiteral("/")); iSlash != -1)
				str = str.left(iSlash);
		}
		break; // end of (blahblah/blah blah)
		default:
			return QStringLiteral("*Invalid direction '%1' in speed walk, must be "
			                      "N, S, E, W, U, D, F, or (something)")
			    .arg(QChar(*p));
		} // end of switch on character

		p++; // bypass whatever that character was (or the trailing bracket)

		// output required number of times
		for (int j = 0; j < count; j++)
			strResult += str + QStringLiteral("\r\n");

	} // end of processing each character

	return strResult;
}

QString WorldEditUtils::reverseSpeedwalk(const QString &speedWalkString)
{
	QString          strResult;
	QString          str;
	QString          strAction;
	int              count      = 0;
	const auto      *app        = AppController::instance();
	const QByteArray inputBytes = speedWalkString.toLocal8Bit();
	const char      *p          = inputBytes.constData();

	while (*p) // until string runs out
	{
		// preserve spaces
		while (isAsciiSpace(*p))
		{
			switch (*p)
			{
			case '\r':
				break; // discard carriage returns
			case '\n':
				strResult = QStringLiteral("\r\n") + strResult; // newline
				break;
			default:
				strResult = QChar(*p) + strResult;
				break;
			} // end of switch

			p++;
		} // end of preserving spaces

		// preserve comments
		if (*p == '{')
		{
			str.clear();
			while (*p && *p != '}')
				str += *p++; // add to string

			if (*p != '}')
				return makeSpeedwalkErrorString(
				    QStringLiteral("Comment code of '{' not terminated by a '}'"));

			p++; // skip } symbol

			str += QLatin1Char('}');

			strResult = str + strResult;
			continue; // back to start of loop
		} // end of comment

		// get counter, if any
		count = 0;
		while (isAsciiDigit(*p))
		{
			count = count * 10 + (*p++ - '0');
			if (count > 99)
				return makeSpeedwalkErrorString(QStringLiteral("Speed walk counter exceeds 99"));
		} // end of having digit(s)

		// no counter, assume do once
		if (count == 0)
			count = 1;

		// bypass spaces after counter
		while (isAsciiSpace(*p))
			p++;

		if (count > 1 && *p == 0)
			return makeSpeedwalkErrorString(QStringLiteral("Speed walk counter not followed by an action"));

		if (count > 1 && *p == '{')
			return makeSpeedwalkErrorString(
			    QStringLiteral("Speed walk counter may not be followed by a comment"));

		// might have had trailing space
		if (*p == 0)
			break;

		if (isActionCode(*p))
		{
			if (count > 1)
				return makeSpeedwalkErrorString(QStringLiteral("Action code of C, O, L or K must not follow "
				                                               "a speed walk count (1-99)"));

			strAction = QChar(*p++); // remember action

			// bypass spaces after open/close/lock/unlock
			while (isAsciiSpace(*p))
				p++;

			if (*p == 0 || asciiToUpper(*p) == 'F' || *p == '{')
				return makeSpeedwalkErrorString(QStringLiteral("Action code of C, O, L or K must be followed "
				                                               "by a direction"));

		} // end of C, O, L, K
		else
			strAction.clear(); // no action

		// work out which direction we are going
		switch (asciiToUpper(*p))
		{
		case 'N':
		case 'S':
		case 'E':
		case 'W':
		case 'U':
		case 'D':
		case 'F':
		{
			str = app ? app->mapDirectionReverse(QString(QChar(asciiToLower(*p)))) : QString();
		}
		break;

		case '(': // special string (eg. (ne/sw) )
		{
			str.clear();
			++p;
			while (*p && *p != ')')
				str += QChar(asciiToLower(*p++)); // add to string

			if (*p != ')')
				return makeSpeedwalkErrorString(QStringLiteral("Action code of '(' not terminated by a ')'"));
			// if no slash try to convert whole thing (e.g. ne becomes sw)
			if (const qsizetype iSlash = str.indexOf(QStringLiteral("/")); iSlash == -1)
			{
				if (const auto reverse = app ? app->mapDirectionReverse(str) : QString(); !reverse.isEmpty())
					str = reverse;
			}
			else
			{
				const QString strLeftPart  = str.left(iSlash);
				const QString strRightPart = str.mid(iSlash + 1);
				str                        = strRightPart + QStringLiteral("/") + strLeftPart; // swap parts
			}

			str = QStringLiteral("(") + str + QStringLiteral(")");
		}
		break; // end of (blahblah/blah blah)
		default:
			return QStringLiteral("*Invalid direction '%1' in speed walk, must be "
			                      "N, S, E, W, U, D, F, or (something)")
			    .arg(QChar(*p));
		} // end of switch on character

		p++; // bypass whatever that character was (or the trailing bracket)

		// output it
		if (count > 1)
			strResult = QStringLiteral("%1%2%3").arg(count).arg(strAction).arg(str) + strResult;
		else
			strResult = strAction + str + strResult;

	} // end of processing each character

	return strResult;
}

bool WorldEditUtils::checkLabelInvalid(const QString &label, const bool script)
{
	if (label.isEmpty())
		return true;

	// first character must be letter
	if (const QChar first = label.at(0); !first.isLetter())
		return true;

	for (int i = 1; i < label.size(); i++)
	{
		const QChar c = label.at(i);
		if (c.isLetterOrNumber())
			continue;
		if (c == QLatin1Char('_'))
			continue;
		if (c == QLatin1Char('.') && script)
			continue;
		return true;
	}

	return false;
}

int WorldEditUtils::findInvalidChar(const QString &text, const QList<ushort> &invalid)
{
	for (int i = 0; i < text.size(); ++i)
	{
		if (const ushort code = text.at(i).unicode(); invalid.contains(code))
			return i;
	}
	return -1;
}

bool WorldEditUtils::checkRegularExpression(QWidget *parent, const QString &pattern,
                                            const QRegularExpression::PatternOptions options)
{
	const QRegularExpression regex(pattern, options);
	if (regex.isValid())
		return true;

	QDialog dlg(parent);
	dlg.setWindowTitle(QStringLiteral("Regular expression problem"));

	auto  layout    = std::make_unique<QVBoxLayout>();
	auto *layoutPtr = layout.get();
	dlg.setLayout(layout.release());
	auto errorLabel = std::make_unique<QLabel>(&dlg);
	errorLabel->setText(regex.errorString() + QStringLiteral("."));
	layoutPtr->addWidget(errorLabel.release());

	const auto column = regex.patternErrorOffset() + 1;
	auto       columnLabel =
	    std::make_unique<QLabel>(QStringLiteral("Error occurred at column %1.").arg(column), &dlg);
	layoutPtr->addWidget(columnLabel.release());

	auto text = std::make_unique<QPlainTextEdit>(&dlg);
	text->setReadOnly(true);
	text->setPlainText(pattern + QStringLiteral("\n") +
	                   QString(column > 1 ? column - 1 : 0, QLatin1Char('-')) + QStringLiteral("^"));
	text->setFont(qmudPreferredMonospaceFont());
	layoutPtr->addWidget(text.release());

	auto buttons = std::make_unique<QDialogButtonBox>(QDialogButtonBox::Ok, &dlg);
	QObject::connect(buttons.get(), &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	layoutPtr->addWidget(buttons.release());

	(void)dlg.exec();
	return false;
}

bool WorldEditUtils::showMultipleAsterisksWarning(QWidget *parent)
{
	auto file    = QFile(QStringLiteral(":/qmud/text/multiple_asterisks.txt"));
	auto message = QStringLiteral("Your \"match\" text contains multiple consecutive asterisks.");
	if (file.open(QIODevice::ReadOnly | QIODevice::Text))
		message = QString::fromUtf8(file.readAll()).trimmed();

	QMessageBox::information(parent, QStringLiteral("Warning"), message);
	return true;
}

void WorldEditUtils::applyEditorPreferences(QLineEdit *edit)
{
	if (!edit || !fixedFontEnabled())
		return;
	edit->setFont(fixedPitchFont());
}

void WorldEditUtils::applyEditorPreferences(QPlainTextEdit *edit)
{
	if (!edit)
		return;
	if (fixedFontEnabled())
		edit->setFont(fixedPitchFont());
	edit->setTabChangesFocus(!tabInsertsTab());
}
