/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldEditUtils.cpp
 * Role: Common dialog helper implementations reused by world object editors to enforce consistent validation rules.
 */

#include "helpers/WorldEditUtils.h"
#include "AppController.h"
#include "FontUtils.h"
#include "SpeedwalkParser.h"
#include "WorldCommandProcessorUtils.h"
#include "WorldOptions.h"

#include <QComboBox>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDialogButtonBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <memory>

namespace
{
	bool isAsciiSpace(const QChar ch)
	{
		switch (ch.unicode())
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

	bool isAsciiDigit(const QChar ch)
	{
		return ch >= QLatin1Char('0') && ch <= QLatin1Char('9');
	}

	QChar asciiToUpper(const QChar ch)
	{
		if (ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))
			return QChar(ch.unicode() - ('a' - 'A'));
		return ch;
	}

	QChar asciiToLower(const QChar ch)
	{
		if (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z'))
			return QChar(ch.unicode() + ('a' - 'A'));
		return ch;
	}

	bool isActionCode(const QChar ch)
	{
		const QChar upper = asciiToUpper(ch);
		return upper == QLatin1Char('C') || upper == QLatin1Char('O') || upper == QLatin1Char('L') ||
		       upper == QLatin1Char('K');
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
	return QMudCommandPattern::convertToRegularExpression(matchString, wholeLine, makeAsterisksWildcards);
}

QString WorldEditUtils::evaluateSpeedwalk(const QString &speedWalkString, const QString &filler)
{
	const auto *app      = AppController::instance();
	const auto  resolver = [app](const QString &direction) -> QString
	{ return app ? app->mapDirectionToSend(direction) : QString(); };
	return QMudSpeedwalk::evaluateSpeedwalk(speedWalkString, filler, resolver);
}

QString WorldEditUtils::reverseSpeedwalk(const QString &speedWalkString)
{
	QString         strResult;
	QString         str;
	QString         strAction;
	int             count = 0;
	const auto     *app   = AppController::instance();
	qsizetype       offset{0};
	const qsizetype size = speedWalkString.size();

	while (offset < size)
	{
		// preserve spaces
		while (offset < size && isAsciiSpace(speedWalkString.at(offset)))
		{
			const QChar ch = speedWalkString.at(offset);
			switch (ch.unicode())
			{
			case '\r':
				break; // discard carriage returns
			case '\n':
				strResult = QStringLiteral("\r\n") + strResult; // newline
				break;
			default:
				strResult = ch + strResult;
				break;
			} // end of switch

			++offset;
		} // end of preserving spaces

		if (offset >= size)
			break;

		// preserve comments
		if (speedWalkString.at(offset) == QLatin1Char('{'))
		{
			str.clear();
			while (offset < size && speedWalkString.at(offset) != QLatin1Char('}'))
				str += speedWalkString.at(offset++);

			if (offset >= size)
				return QMudSpeedwalk::makeSpeedWalkErrorString(
				    QStringLiteral("Comment code of '{' not terminated by a '}'"));

			++offset;
			str += QLatin1Char('}');
			strResult = str + strResult;
			continue;
		} // end of comment

		// get counter, if any
		count = 0;
		while (offset < size && isAsciiDigit(speedWalkString.at(offset)))
		{
			count = count * 10 + speedWalkString.at(offset).unicode() - '0';
			++offset;
			if (count > 99)
				return QMudSpeedwalk::makeSpeedWalkErrorString(
				    QStringLiteral("Speed walk counter exceeds 99"));
		} // end of having digit(s)

		// no counter, assume do once
		if (count == 0)
			count = 1;

		// bypass spaces after counter
		while (offset < size && isAsciiSpace(speedWalkString.at(offset)))
			++offset;

		if (count > 1 && offset >= size)
			return QMudSpeedwalk::makeSpeedWalkErrorString(
			    QStringLiteral("Speed walk counter not followed by an action"));

		if (count > 1 && speedWalkString.at(offset) == QLatin1Char('{'))
			return QMudSpeedwalk::makeSpeedWalkErrorString(
			    QStringLiteral("Speed walk counter may not be followed by a comment"));

		// might have had trailing space
		if (offset >= size)
			break;

		if (isActionCode(speedWalkString.at(offset)))
		{
			if (count > 1)
				return QMudSpeedwalk::makeSpeedWalkErrorString(
				    QStringLiteral("Action code of C, O, L or K must not follow a speed walk count (1-99)"));

			strAction = speedWalkString.at(offset++);

			// bypass spaces after open/close/lock/unlock
			while (offset < size && isAsciiSpace(speedWalkString.at(offset)))
				++offset;

			if (offset >= size || asciiToUpper(speedWalkString.at(offset)) == QLatin1Char('F') ||
			    speedWalkString.at(offset) == QLatin1Char('{'))
				return QMudSpeedwalk::makeSpeedWalkErrorString(
				    QStringLiteral("Action code of C, O, L or K must be followed by a direction"));

		} // end of C, O, L, K
		else
			strAction.clear(); // no action

		// work out which direction we are going
		switch (asciiToUpper(speedWalkString.at(offset)).unicode())
		{
		case 'N':
		case 'S':
		case 'E':
		case 'W':
		case 'U':
		case 'D':
		case 'F':
		{
			str =
			    app ? app->mapDirectionReverse(QString(asciiToLower(speedWalkString.at(offset)))) : QString();
		}
		break;

		case '(': // special string (eg. (ne/sw) )
		{
			str.clear();
			++offset;
			while (offset < size && speedWalkString.at(offset) != QLatin1Char(')'))
				str += asciiToLower(speedWalkString.at(offset++));

			if (offset >= size)
				return QMudSpeedwalk::makeSpeedWalkErrorString(
				    QStringLiteral("Action code of '(' not terminated by a ')'"));
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
			    .arg(speedWalkString.at(offset));
		} // end of switch on character

		++offset; // bypass direction or trailing bracket

		// output it
		if (count > 1)
			strResult = QStringLiteral("%1%2%3").arg(count).arg(strAction).arg(str) + strResult;
		else
			strResult = strAction + str + strResult;
	}

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
