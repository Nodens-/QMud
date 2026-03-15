/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldCommandProcessor.cpp
 * Role: Command pipeline implementation that interprets input lines and routes resolved actions to world runtime/script
 * handlers.
 */

#include "WorldCommandProcessor.h"
#include "AliasMatchUtils.h"
#include "AppController.h"
#include "CommandPatternUtils.h"
#include "CommandQueueUtils.h"
#include "CommandTextUtils.h"
#include "FileExtensions.h"
#include "MainWindowHost.h"
#include "MainWindowHostResolver.h"
#include "SpeedwalkParser.h"
#include "TimerSchedulingUtils.h"
#include "WorldOptions.h"
#include "WorldRuntime.h"
#include "WorldView.h"
#include "scripting/ScriptingErrors.h"

#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QInputDialog>
#include <QMessageBox>
#include <QRegularExpression>
#include <QUrl>
#include <algorithm>
#include <limits>
#ifdef HILITE
constexpr int kStyleHilite = HILITE;
#else
constexpr int kStyleHilite = 0x0001;
#endif
#ifdef UNDERLINE
constexpr int kStyleUnderline = UNDERLINE;
#else
constexpr int kStyleUnderline = 0x0002;
#endif
#ifdef BLINK
constexpr int kStyleBlink = BLINK;
#else
constexpr int kStyleBlink = 0x0004;
#endif
#ifdef INVERSE
constexpr int kStyleInverse = INVERSE;
#else
constexpr int kStyleInverse = 0x0008;
#endif

namespace
{
	const auto    kEndLine           = QStringLiteral("\r\n");
	constexpr int kMaxExecutionDepth = 20; // Legacy MAX_EXECUTION_DEPTH

	struct AnsiPalette
	{
			QVector<QColor> normal;
			QVector<QColor> bold;
			QVector<QColor> customText;
			QVector<QColor> customBack;
	};

	int safeQSizeToInt(const qsizetype size)
	{
		if (size <= 0)
			return 0;
		constexpr qsizetype kMaxInt = std::numeric_limits<int>::max();
		return size > kMaxInt ? std::numeric_limits<int>::max() : static_cast<int>(size);
	}

	QColor parseColorValue(const QString &value)
	{
		if (value.isEmpty())
			return {};
		if (const QColor color(value); color.isValid())
			return color;
		bool      ok      = false;
		const int numeric = value.toInt(&ok);
		if (!ok)
			return {};
		const int r = numeric & 0xFF;
		const int g = numeric >> 8 & 0xFF;
		const int b = numeric >> 16 & 0xFF;
		return {r, g, b};
	}

	bool isEnabledValue(const QString &value)
	{
		return value == QStringLiteral("1") || value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
		       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
	}

	QString fixUpGerman(const QString &message)
	{
		QString result = message;
		result.replace(QChar(0x00FC), QStringLiteral("ue")); // ü
		result.replace(QChar(0x00DC), QStringLiteral("Ue")); // Ü
		result.replace(QChar(0x00E4), QStringLiteral("ae")); // ä
		result.replace(QChar(0x00C4), QStringLiteral("Ae")); // Ä
		result.replace(QChar(0x00F6), QStringLiteral("oe")); // ö
		result.replace(QChar(0x00D6), QStringLiteral("Oe")); // Ö
		result.replace(QChar(0x00DF), QStringLiteral("ss")); // ß
		return result;
	}

	QString englishWeekdayLower(const QDate &date)
	{
		switch (date.dayOfWeek())
		{
		case 1:
			return QStringLiteral("monday");
		case 2:
			return QStringLiteral("tuesday");
		case 3:
			return QStringLiteral("wednesday");
		case 4:
			return QStringLiteral("thursday");
		case 5:
			return QStringLiteral("friday");
		case 6:
			return QStringLiteral("saturday");
		case 7:
			return QStringLiteral("sunday");
		default:
			return QStringLiteral("unknown");
		}
	}

	QString ensureDisconnectSaveWorldPath(const WorldRuntime &runtime, const AppController *app)
	{
		if (QString filePath = runtime.worldFilePath().trimmed(); !filePath.isEmpty())
			return filePath;

		QString baseName =
		    runtime.worldAttributes().value(QStringLiteral("name"), QStringLiteral("World")).trimmed();
		if (baseName.isEmpty())
			baseName = QStringLiteral("World");
		for (const QChar ch : QStringLiteral("<>\"|?:#%;/\\"))
			baseName.replace(ch, QLatin1Char('_'));
		if (!baseName.endsWith(QStringLiteral(".qdl"), Qt::CaseInsensitive))
			baseName += QStringLiteral(".qdl");
		baseName = QMudFileExtensions::replaceOrAppendExtension(baseName, QStringLiteral("qdl"));

		QString baseDir;
		if (app)
		{
			baseDir = app->makeAbsolutePath(app->defaultWorldDirectory());
			if (baseDir.isEmpty())
				baseDir = app->makeAbsolutePath(QStringLiteral("."));
		}
		if (baseDir.isEmpty())
			baseDir = runtime.startupDirectory();
		if (baseDir.isEmpty())
			baseDir = QCoreApplication::applicationDirPath();

		const QDir dir(baseDir);
		dir.mkpath(QStringLiteral("."));
		return dir.filePath(baseName);
	}

	bool createDisconnectWeekdayBackup(const QString &savedPath, const WorldRuntime &runtime,
	                                   const AppController *app, QString &error)
	{
		const QFileInfo srcInfo(savedPath);
		if (!srcInfo.exists() || !srcInfo.isFile())
		{
			error = QStringLiteral("Saved world file not found: %1").arg(savedPath);
			return false;
		}

		QString worldsDir = runtime.defaultWorldDirectory();
		if (!worldsDir.isEmpty() && app)
			worldsDir = app->makeAbsolutePath(worldsDir);
		if (app)
		{
			if (worldsDir.isEmpty())
				worldsDir = app->makeAbsolutePath(app->defaultWorldDirectory());
			if (worldsDir.isEmpty())
				worldsDir = app->makeAbsolutePath(QStringLiteral("worlds"));
		}
		if (worldsDir.isEmpty() && !runtime.startupDirectory().trimmed().isEmpty())
			worldsDir = QDir(runtime.startupDirectory()).filePath(QStringLiteral("worlds"));
		if (worldsDir.isEmpty())
			worldsDir = srcInfo.absolutePath();

		const QString backupDirPath = QDir(worldsDir).filePath(QStringLiteral("backup"));
		if (!QDir().mkpath(backupDirPath))
		{
			error = QStringLiteral("Unable to create backup directory: %1").arg(backupDirPath);
			return false;
		}

		const QString weekday    = englishWeekdayLower(QDate::currentDate());
		const QString ext        = srcInfo.suffix();
		QString       backupName = srcInfo.completeBaseName() + QStringLiteral("-backup-") + weekday;
		if (!ext.isEmpty())
			backupName += QStringLiteral(".") + ext;
		const QString backupPath = QDir(backupDirPath).filePath(backupName);

		// Keep one backup per weekday file name, but always refresh it with latest saved world content.
		if (QFileInfo::exists(backupPath) && !QFile::remove(backupPath))
		{
			error = QStringLiteral("Unable to overwrite existing backup file: %1").arg(backupPath);
			return false;
		}

		if (!QFile::copy(srcInfo.filePath(), backupPath))
		{
			error = QStringLiteral("Unable to create backup file: %1").arg(backupPath);
			return false;
		}
		return true;
	}

	AnsiPalette buildPalette(WorldRuntime *runtime)
	{
		AnsiPalette palette;
		palette.normal     = QVector<QColor>(8);
		palette.bold       = QVector<QColor>(8);
		palette.customText = QVector<QColor>(16);
		palette.customBack = QVector<QColor>(16);

		palette.normal[0] = QColor(0, 0, 0);
		palette.normal[1] = QColor(128, 0, 0);
		palette.normal[2] = QColor(0, 128, 0);
		palette.normal[3] = QColor(128, 128, 0);
		palette.normal[4] = QColor(0, 0, 128);
		palette.normal[5] = QColor(128, 0, 128);
		palette.normal[6] = QColor(0, 128, 128);
		palette.normal[7] = QColor(192, 192, 192);
		palette.bold[0]   = QColor(128, 128, 128);
		palette.bold[1]   = QColor(255, 0, 0);
		palette.bold[2]   = QColor(0, 255, 0);
		palette.bold[3]   = QColor(255, 255, 0);
		palette.bold[4]   = QColor(0, 0, 255);
		palette.bold[5]   = QColor(255, 0, 255);
		palette.bold[6]   = QColor(0, 255, 255);
		palette.bold[7]   = QColor(255, 255, 255);

		for (int i = 0; i < palette.customText.size(); ++i)
		{
			palette.customText[i] = QColor(255, 255, 255);
			palette.customBack[i] = QColor(0, 0, 0);
		}
		palette.customText[0]  = QColor(255, 128, 128);
		palette.customText[1]  = QColor(255, 255, 128);
		palette.customText[2]  = QColor(128, 255, 128);
		palette.customText[3]  = QColor(128, 255, 255);
		palette.customText[4]  = QColor(0, 128, 255);
		palette.customText[5]  = QColor(255, 128, 192);
		palette.customText[6]  = QColor(255, 0, 0);
		palette.customText[7]  = QColor(0, 128, 192);
		palette.customText[8]  = QColor(255, 0, 255);
		palette.customText[9]  = QColor(128, 64, 64);
		palette.customText[10] = QColor(255, 128, 64);
		palette.customText[11] = QColor(0, 128, 128);
		palette.customText[12] = QColor(0, 64, 128);
		palette.customText[13] = QColor(255, 0, 128);
		palette.customText[14] = QColor(0, 128, 0);
		palette.customText[15] = QColor(0, 0, 255);

		if (!runtime)
			return palette;

		for (const auto &[groupValue, attributes] : runtime->colours())
		{
			const QString group = groupValue.trimmed().toLower();
			bool          ok    = false;
			const int     seq   = attributes.value(QStringLiteral("seq")).toInt(&ok);
			const int     index = ok ? seq - 1 : -1;
			if (index < 0)
				continue;
			if (group == QStringLiteral("ansi/normal") && index < palette.normal.size())
			{
				if (const QColor rgb = parseColorValue(attributes.value(QStringLiteral("rgb")));
				    rgb.isValid())
					palette.normal[index] = rgb;
			}
			else if (group == QStringLiteral("ansi/bold") && index < palette.bold.size())
			{
				if (const QColor rgb = parseColorValue(attributes.value(QStringLiteral("rgb")));
				    rgb.isValid())
					palette.bold[index] = rgb;
			}
			else if ((group == QStringLiteral("custom/custom") || group == QStringLiteral("custom")) &&
			         index < palette.customText.size())
			{
				const QColor text = parseColorValue(attributes.value(QStringLiteral("text")));
				const QColor back = parseColorValue(attributes.value(QStringLiteral("back")));
				if (text.isValid())
					palette.customText[index] = text;
				if (back.isValid())
					palette.customBack[index] = back;
			}
		}

		return palette;
	}

	int toRgbNumber(const QColor &color)
	{
		if (!color.isValid())
			return 0;
		return color.blue() << 16 | color.green() << 8 | color.red();
	}

	QVector<WorldRuntime::StyleSpan> ensureSpansForLine(const QString                          &line,
	                                                    const QVector<WorldRuntime::StyleSpan> &spans,
	                                                    const QColor &fore, const QColor &back)
	{
		if (!spans.isEmpty())
			return spans;
		if (line.isEmpty())
			return spans;
		WorldRuntime::StyleSpan span;
		span.length = safeQSizeToInt(line.size());
		span.fore   = fore;
		span.back   = back;
		return {span};
	}

	bool sameStyleForWrap(const WorldRuntime::StyleSpan &a, const WorldRuntime::StyleSpan &b)
	{
		return a.fore == b.fore && a.back == b.back && a.bold == b.bold && a.underline == b.underline &&
		       a.italic == b.italic && a.blink == b.blink && a.strike == b.strike && a.inverse == b.inverse &&
		       a.changed == b.changed && a.actionType == b.actionType && a.action == b.action &&
		       a.hint == b.hint && a.variable == b.variable && a.startTag == b.startTag;
	}

	void wrapStyledLineForColumn(QString &text, QVector<WorldRuntime::StyleSpan> &spans, const int wrapColumn,
	                             const bool indentParas)
	{
		if (wrapColumn <= 0 || text.isEmpty())
			return;

		QVector<WorldRuntime::StyleSpan> expandedStyles;
		expandedStyles.reserve(text.size());
		WorldRuntime::StyleSpan lastStyle;
		int                     covered = 0;
		for (const WorldRuntime::StyleSpan &span : spans)
		{
			lastStyle     = span;
			const int len = qMax(0, span.length);
			for (int i = 0; i < len && covered < text.size(); ++i, ++covered)
				expandedStyles.push_back(span);
			if (covered >= text.size())
				break;
		}
		while (expandedStyles.size() < text.size())
			expandedStyles.push_back(lastStyle);

		QString wrappedText;
		wrappedText.reserve(safeQSizeToInt(text.size() + text.size() / qMax(1, wrapColumn)));
		QVector<WorldRuntime::StyleSpan> wrappedStyles;
		wrappedStyles.reserve(
		    safeQSizeToInt(expandedStyles.size() + expandedStyles.size() / qMax(1, wrapColumn)));

		int  column             = 0;
		int  lineStart          = 0;
		int  lastSpace          = -1;
		bool lineHasVisibleChar = false;

		auto recomputeLineState = [&]
		{
			column             = 0;
			lineStart          = 0;
			lastSpace          = -1;
			lineHasVisibleChar = false;
			for (int i = safeQSizeToInt(wrappedText.size()) - 1; i >= 0; --i)
			{
				if (wrappedText.at(i) == QLatin1Char('\n'))
				{
					lineStart = i + 1;
					break;
				}
			}
			for (int i = lineStart, size = safeQSizeToInt(wrappedText.size()); i < size; ++i)
			{
				const QChar ch = wrappedText.at(i);
				if (ch == QLatin1Char('\n'))
				{
					lineStart          = i + 1;
					column             = 0;
					lastSpace          = -1;
					lineHasVisibleChar = false;
					continue;
				}
				if (ch == QLatin1Char(' '))
					lastSpace = i;
				if (!ch.isSpace())
					lineHasVisibleChar = true;
				++column;
			}
		};

		for (int i = 0; i < text.size(); ++i)
		{
			const QChar                   ch    = text.at(i);
			const WorldRuntime::StyleSpan style = expandedStyles.value(i);
			wrappedText.append(ch);
			wrappedStyles.push_back(style);

			if (ch == QLatin1Char('\n'))
			{
				column             = 0;
				lineStart          = safeQSizeToInt(wrappedText.size());
				lastSpace          = -1;
				lineHasVisibleChar = false;
				continue;
			}

			if (ch == QLatin1Char(' '))
				lastSpace = safeQSizeToInt(wrappedText.size()) - 1;
			if (!ch.isSpace())
				lineHasVisibleChar = true;

			++column;
			if (column < wrapColumn)
				continue;

			if (!lineHasVisibleChar)
				continue;

			int insertPos = safeQSizeToInt(wrappedText.size());
			if (lastSpace >= lineStart)
			{
				insertPos = indentParas ? lastSpace : lastSpace + 1;

				// If this wrap point would emit only leading padding, avoid creating
				// blank-looking lines from server-side wide-table indentation.
				bool onlyWhitespace = true;
				for (int j = lineStart, size = safeQSizeToInt(wrappedText.size()); j < insertPos && j < size;
				     ++j)
				{
					if (!wrappedText.at(j).isSpace())
					{
						onlyWhitespace = false;
						break;
					}
				}
				if (onlyWhitespace)
					insertPos = safeQSizeToInt(wrappedText.size());
			}

			const WorldRuntime::StyleSpan newlineStyle = insertPos > 0 && insertPos - 1 < wrappedStyles.size()
			                                                 ? wrappedStyles.at(insertPos - 1)
			                                                 : style;
			wrappedText.insert(insertPos, QLatin1Char('\n'));
			wrappedStyles.insert(insertPos, newlineStyle);
			recomputeLineState();
		}

		QVector<WorldRuntime::StyleSpan> rebuilt;
		rebuilt.reserve(wrappedStyles.size());
		for (const WorldRuntime::StyleSpan &oneStyle : wrappedStyles)
		{
			WorldRuntime::StyleSpan span = oneStyle;
			span.length                  = 1;
			if (!rebuilt.isEmpty() && sameStyleForWrap(rebuilt.last(), span))
				rebuilt.last().length++;
			else
				rebuilt.push_back(span);
		}

		text  = wrappedText;
		spans = rebuilt;
	}

	void applyStyleToSpans(QVector<WorldRuntime::StyleSpan> &spans, const int start, const int end,
	                       const QColor &newFore, const QColor &newBack, const bool changeFore,
	                       const bool changeBack, const bool makeBold, const bool makeItalic,
	                       const bool makeUnderline)
	{
		if (start < 0 || end <= start || spans.isEmpty())
			return;
		QVector<WorldRuntime::StyleSpan> updated;
		int                              pos = 0;
		for (const auto &span : spans)
		{
			const int spanStart = pos;
			const int spanEnd   = pos + span.length;
			if (spanEnd <= start || spanStart >= end)
			{
				updated.push_back(span);
				pos = spanEnd;
				continue;
			}
			if (spanStart < start)
			{
				WorldRuntime::StyleSpan left = span;
				left.length                  = start - spanStart;
				updated.push_back(left);
			}
			const int               overlapStart = qMax(spanStart, start);
			const int               overlapEnd   = qMin(spanEnd, end);
			WorldRuntime::StyleSpan middle       = span;
			middle.length                        = overlapEnd - overlapStart;
			bool didChange                       = false;
			if (changeFore && newFore.isValid() && middle.fore != newFore)
			{
				middle.fore = newFore;
				didChange   = true;
			}
			if (changeBack && newBack.isValid() && middle.back != newBack)
			{
				middle.back = newBack;
				didChange   = true;
			}
			if (makeBold && !middle.bold)
			{
				middle.bold = true;
				didChange   = true;
			}
			if (makeItalic && !middle.italic)
			{
				middle.italic = true;
				didChange     = true;
			}
			if (makeUnderline && !middle.underline)
			{
				middle.underline = true;
				didChange        = true;
			}
			if (didChange)
				middle.changed = true;
			updated.push_back(middle);
			if (spanEnd > end)
			{
				WorldRuntime::StyleSpan right = span;
				right.length                  = spanEnd - end;
				updated.push_back(right);
			}
			pos = spanEnd;
		}
		spans = updated;
	}

	QVector<LuaStyleRun> buildStyleRuns(const QString &line, const QVector<WorldRuntime::StyleSpan> &spans,
	                                    const QColor &defaultFore, const QColor &defaultBack)
	{
		QVector<LuaStyleRun> runs;
		const auto           effective = ensureSpansForLine(line, spans, defaultFore, defaultBack);
		int                  pos       = 0;
		for (const auto &span : effective)
		{
			if (span.length <= 0)
				continue;
			LuaStyleRun run;
			run.text       = line.mid(pos, span.length);
			run.textColour = toRgbNumber(span.fore);
			run.backColour = toRgbNumber(span.back);
			int style      = 0;
			if (span.bold)
				style |= kStyleHilite;
			if (span.underline)
				style |= kStyleUnderline;
			if (span.blink)
				style |= kStyleBlink;
			if (span.inverse)
				style |= kStyleInverse;
			run.style = style;
			runs.push_back(run);
			pos += span.length;
		}
		return runs;
	}

	void mixSignature(quint64 &signature, const quint64 value)
	{
		signature ^= value + 0x9e3779b97f4a7c15ULL + (signature << 6) + (signature >> 2);
	}

	quint64 aliasOrderSignature(const QList<WorldRuntime::Alias> &aliases)
	{
		quint64 signature = 0x9f4d03f4d03f4d03ULL;
		mixSignature(signature, static_cast<quint64>(aliases.size()));
		for (const WorldRuntime::Alias &alias : aliases)
		{
			bool      ok       = false;
			const int sequence = alias.attributes.value(QStringLiteral("sequence")).toInt(&ok);
			mixSignature(signature, static_cast<quint64>(ok ? sequence : 0));
			mixSignature(signature, qHash(alias.attributes.value(QStringLiteral("name"))));
		}
		return signature;
	}

	quint64 triggerOrderSignature(const QList<WorldRuntime::Trigger> &triggers)
	{
		quint64 signature = 0x42f0b4a1d2c3e5f7ULL;
		mixSignature(signature, static_cast<quint64>(triggers.size()));
		for (const WorldRuntime::Trigger &trigger : triggers)
		{
			bool      ok       = false;
			const int sequence = trigger.attributes.value(QStringLiteral("sequence")).toInt(&ok);
			mixSignature(signature, static_cast<quint64>(ok ? sequence : 0));
			mixSignature(signature, qHash(trigger.attributes.value(QStringLiteral("match"))));
		}
		return signature;
	}

	quint64 pluginOrderSignature(const QList<WorldRuntime::Plugin> &plugins)
	{
		quint64 signature = 0xc3d2e1f0a5b49786ULL;
		mixSignature(signature, static_cast<quint64>(plugins.size()));
		for (const WorldRuntime::Plugin &plugin : plugins)
		{
			mixSignature(signature, static_cast<quint64>(plugin.sequence));
			mixSignature(signature, qHash(plugin.attributes.value(QStringLiteral("id"))));
		}
		return signature;
	}
} // namespace

QString WorldCommandProcessor::fixHtmlString(const QString &source)
{
	QString   strOldString = source;
	QString   strNewString;

	qsizetype i = -1;
	while ((i = strOldString.indexOf(QRegularExpression(QStringLiteral("[<>&\\\"]")))) != -1)
	{
		strNewString += strOldString.left(i);

		switch (strOldString.at(i).unicode())
		{
		case '<':
			strNewString += QStringLiteral("&lt;");
			break;
		case '>':
			strNewString += QStringLiteral("&gt;");
			break;
		case '&':
			strNewString += QStringLiteral("&amp;");
			break;
		case '\"':
			strNewString += QStringLiteral("&quot;");
			break;
		default:
			break;
		}

		strOldString = strOldString.mid(i + 1);
	}

	strNewString += strOldString;

	return strNewString;

} // end of FixHTMLString

WorldCommandProcessor::WorldCommandProcessor(QObject *parent) : QObject(parent)
{
}

bool WorldCommandProcessor::canExecuteWorldScript(const QString &functionType, const QString &functionName,
                                                  LuaCallbackEngine *lua, const bool requireFunction) const
{
	if (!m_runtime)
		return false;

	const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
	const bool warn = isEnabledValue(attrs.value(QStringLiteral("warn_if_scripting_inactive")));
	const bool scriptingEnabled =
	    isEnabledValue(attrs.value(QStringLiteral("enable_scripts"))) &&
	    attrs.value(QStringLiteral("script_language")).compare(QStringLiteral("Lua"), Qt::CaseInsensitive) ==
	        0 &&
	    lua != nullptr;
	if (!scriptingEnabled)
	{
		if (warn)
		{
			m_runtime->outputText(
			    QStringLiteral("%1 function \"%2\" cannot execute - scripting disabled/parse error.")
			        .arg(functionType, functionName),
			    true, true);
		}
		return false;
	}

	if (requireFunction && !lua->hasFunction(functionName))
	{
		if (warn)
		{
			m_runtime->outputText(QStringLiteral("%1 function \"%2\" not found or had a previous error.")
			                          .arg(functionType, functionName),
			                      true, true);
		}
		return false;
	}

	return true;
}

void WorldCommandProcessor::setRuntime(WorldRuntime *runtime)
{
	m_runtime = runtime;
	m_triggerOrderCache.clear();
	m_aliasOrderCache.clear();
	m_pluginOrderCache = PluginOrderCacheEntry();
	m_regexCache.clear();
	m_wildcardRegexCache.clear();
	m_invalidRegexWarnings.clear();
	if (!m_runtime)
	{
		if (m_timerCheck)
			m_timerCheck->stop();
		return;
	}
	if (!m_timerCheck)
	{
		m_timerCheck = new QTimer(this);
		connect(m_timerCheck, &QTimer::timeout, this, &WorldCommandProcessor::checkTimers);
		m_timerCheck->start(200);
	}
	const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
	m_speedWalkDelay                    = attrs.value(QStringLiteral("speed_walk_delay")).toInt();
	m_speedWalkFiller                   = attrs.value(QStringLiteral("speed_walk_filler"));
	const QString translateGerman       = attrs.value(QStringLiteral("translate_german"));
	m_translateGerman                   = isEnabledValue(translateGerman);
	const QString translateBackslash    = attrs.value(QStringLiteral("translate_backslash_sequences"));
	m_translateBackslashSequences       = isEnabledValue(translateBackslash);
	const QString enableSpam            = attrs.value(QStringLiteral("enable_spam_prevention"));
	m_enableSpamPrevention              = isEnabledValue(enableSpam);
	m_spamLineCount                     = attrs.value(QStringLiteral("spam_line_count")).toInt();
	m_spamMessage                       = attrs.value(QStringLiteral("spam_message"));
	const QString noTranslateIac        = attrs.value(QStringLiteral("do_not_translate_iac_to_iac_iac"));
	m_doNotTranslateIac                 = isEnabledValue(noTranslateIac);
	const QString matchEmpty            = attrs.value(QStringLiteral("regexp_match_empty"));
	m_regexpMatchEmpty                  = !(matchEmpty == QStringLiteral("0") ||
                           matchEmpty.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0);
	const QString utf8                  = attrs.value(QStringLiteral("utf_8"));
	m_utf8                              = isEnabledValue(utf8);

	if (m_speedWalkDelay <= 0 && !m_queuedCommands.isEmpty())
		processQueuedCommands(true);
}

QString WorldCommandProcessor::wildcardToRegexCached(const QString &matchText)
{
	if (const auto cached = m_wildcardRegexCache.constFind(matchText);
	    cached != m_wildcardRegexCache.constEnd())
		return cached.value();

	const QString converted = convertToRegularExpression(matchText);
	if (constexpr int kWildcardRegexCacheLimit = 4096;
	    m_wildcardRegexCache.size() >= kWildcardRegexCacheLimit)
		m_wildcardRegexCache.clear();
	m_wildcardRegexCache.insert(matchText, converted);
	return converted;
}

const QVector<int> &WorldCommandProcessor::sortedPluginIndices()
{
	static const QVector<int> kEmpty;
	if (!m_runtime)
		return kEmpty;

	const QList<WorldRuntime::Plugin> &plugins   = m_runtime->pluginsMutable();
	const quint64                      signature = pluginOrderSignature(plugins);
	const int                          count     = safeQSizeToInt(plugins.size());
	if (m_pluginOrderCache.count == count && m_pluginOrderCache.signature == signature)
	{
		return m_pluginOrderCache.indices;
	}

	PluginOrderCacheEntry rebuilt;
	rebuilt.count     = count;
	rebuilt.signature = signature;
	rebuilt.indices.reserve(count);
	for (int i = 0; i < count; ++i)
		rebuilt.indices.push_back(i);

	// Legacy behavior: stable sequence order; equal-sequence plugins retain load order.
	std::ranges::stable_sort(rebuilt.indices, [&](const int left, const int right)
	                         { return plugins.at(left).sequence < plugins.at(right).sequence; });

	m_pluginOrderCache = rebuilt;
	return m_pluginOrderCache.indices;
}

void WorldCommandProcessor::setView(WorldView *view)
{
	m_view = view;
}

const QStringList &WorldCommandProcessor::queuedCommands() const
{
	return m_queuedCommands;
}

int WorldCommandProcessor::discardQueuedCommands()
{
	const int count = QMudCommandQueue::discardAll(m_queuedCommands);
	if (m_runtime)
		m_runtime->setQueuedCommandCount(0);
	updateQueuedCommandsStatusLine();
	return count;
}

QString WorldCommandProcessor::evaluateSpeedwalk(const QString &speedWalkString) const
{
	return doEvaluateSpeedwalk(speedWalkString);
}

int WorldCommandProcessor::executeCommand(const QString &text)
{
	if (QMudAliasMatch::exceedsExecutionDepth(m_executionDepth, kMaxExecutionDepth))
	{
		return eCommandsNestedTooDeeply;
	}
	++m_executionDepth;
	evaluateCommand(text);
	--m_executionDepth;
	return eOK;
}

void WorldCommandProcessor::sendToFromAccelerator(int sendTo, const QString &text, const QString &description,
                                                  const WorldRuntime::Plugin *plugin)
{
	this->sendTo(sendTo, text, true, true, QString(), description, plugin);
}

void WorldCommandProcessor::onCommandEntered(const QString &text)
{
	if (m_runtime && m_runtime->currentActionSource() == WorldRuntime::eUnknownActionSource)
		m_runtime->setCurrentActionSource(WorldRuntime::eUserTyping);

	m_processingEnteredCommand         = true;
	m_omitFromHistoryForEnteredCommand = false;
	m_enteredCommandSendFailed         = false;

	QString commandText = text;
	if (m_runtime)
		m_runtime->firePluginCommandEntered(commandText);
	const QString historyText = commandText;

	// Sentinel host return codes from OnPluginCommandEntered:
	// "\r" => ignore command, "\t" => discard command.
	if (commandText == QStringLiteral("\r") || commandText == QStringLiteral("\t"))
	{
		m_processingEnteredCommand = false;
		if (m_runtime)
			m_runtime->setCurrentActionSource(WorldRuntime::eUnknownActionSource);
		return;
	}

	if (m_translateBackslashSequences)
		commandText = fixupEscapeSequences(commandText);

	const int execResult       = executeCommand(commandText);
	m_processingEnteredCommand = false;
	if (const bool isKeypadCommand =
	        m_runtime && m_runtime->currentActionSource() == WorldRuntime::eUserKeypad;
	    execResult == eOK && !isKeypadCommand && !m_omitFromHistoryForEnteredCommand &&
	    !m_enteredCommandSendFailed && m_view)
		m_view->addToHistory(historyText);
	if (m_runtime)
		m_runtime->setCurrentActionSource(WorldRuntime::eUnknownActionSource);
}

void WorldCommandProcessor::onIncomingLineReceived(const QString &line)
{
	onIncomingStyledLineReceived(line, QVector<WorldRuntime::StyleSpan>());
}

void WorldCommandProcessor::onIncomingStyledLineReceived(const QString                          &line,
                                                         const QVector<WorldRuntime::StyleSpan> &spans)
{
	const auto isEnabled = [](const QString &value)
	{
		return value == QStringLiteral("1") || value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
		       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
	};

	const QMap<QString, QString> *attrs       = nullptr;
	bool                          runtimeWrap = false;
	int                           wrapColumn  = 0;
	bool                          indentParas = true;
	QColor                        defaultFore;
	QColor                        defaultBack;
	bool                          defaultColorsLoaded = false;
	auto                          loadDefaultColors   = [&]
	{
		if (defaultColorsLoaded || !m_runtime || !attrs)
			return;
		const AnsiPalette palette  = buildPalette(m_runtime);
		const bool custom16Default = isEnabled(attrs->value(QStringLiteral("custom_16_is_default_colour")));
		defaultFore                = parseColorValue(attrs->value(QStringLiteral("output_text_colour")));
		defaultBack = parseColorValue(attrs->value(QStringLiteral("output_background_colour")));
		if (!defaultFore.isValid())
			defaultFore = custom16Default ? palette.customText.value(15) : palette.normal.value(7);
		if (!defaultBack.isValid())
			defaultBack = custom16Default ? palette.customBack.value(15) : palette.normal.value(0);
		defaultColorsLoaded = true;
	};
	if (m_runtime)
	{
		attrs                     = &m_runtime->worldAttributes();
		const bool wrapEnabled    = isEnabled(attrs->value(QStringLiteral("wrap")));
		const bool autoWrapWindow = isEnabled(attrs->value(QStringLiteral("auto_wrap_window_width")));
		const bool nawsEnabled    = isEnabled(attrs->value(QStringLiteral("naws")));
		wrapColumn                = attrs->value(QStringLiteral("wrap_column")).toInt();
		runtimeWrap               = wrapEnabled && !autoWrapWindow && wrapColumn > 0 && !nawsEnabled;
		indentParas               = !(
            attrs->value(QStringLiteral("indent_paras")) == QStringLiteral("0") ||
            attrs->value(QStringLiteral("indent_paras")).compare(QStringLiteral("n"), Qt::CaseInsensitive) ==
                0 ||
            attrs->value(QStringLiteral("indent_paras"))
                    .compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0);
	}

	QVector<WorldRuntime::StyleSpan> normalizedSpans = spans;
	if (normalizedSpans.isEmpty())
	{
		loadDefaultColors();
		normalizedSpans = ensureSpansForLine(line, normalizedSpans, defaultFore, defaultBack);
	}

	bool logOutput = false;
	if (m_runtime)
		logOutput = isEnabled(m_runtime->worldAttributes().value(QStringLiteral("log_output")));

	if (m_runtime)
		m_runtime->beginIncomingLineLuaContext(line, WorldRuntime::LineOutput, normalizedSpans, true);

	if (m_runtime)
		m_runtime->setCurrentActionSource(WorldRuntime::eInputFromServer);
	bool omitFromPluginCallback = false;
	if (m_runtime && !m_runtime->firePluginLineReceived(line))
		omitFromPluginCallback = true;
	if (m_runtime)
		m_runtime->setCurrentActionSource(WorldRuntime::eUnknownActionSource);

	TriggerEvaluationResult triggerResult = processTriggersForLine(line, normalizedSpans);
	if (omitFromPluginCallback)
	{
		triggerResult.omitFromOutput = true;
		if (m_runtime)
			m_runtime->setLineOmittedFromOutput(true);
	}

	if (m_view)
	{
		m_view->clearPartialOutput();
		if (!triggerResult.omitFromOutput)
		{
			QString                          displayLine  = line;
			QVector<WorldRuntime::StyleSpan> displaySpans = triggerResult.spans;
			if (runtimeWrap && !displayLine.isEmpty())
			{
				loadDefaultColors();
				displaySpans = ensureSpansForLine(displayLine, displaySpans, defaultFore, defaultBack);
				wrapStyledLineForColumn(displayLine, displaySpans, wrapColumn, indentParas);
			}
			if (displaySpans.isEmpty())
				m_view->appendOutputText(displayLine);
			else
				m_view->appendOutputTextStyled(displayLine, displaySpans);
			if (m_runtime)
			{
				m_runtime->markIncomingLineLuaContextBuffered();
				m_runtime->firePluginScreendraw(0, logOutput && !triggerResult.omitFromLog ? 1 : 0, line);
			}
		}
	}

	if (m_view && !triggerResult.extraOutput.isEmpty())
	{
		QString extra = triggerResult.extraOutput;
		if (extra.endsWith(kEndLine))
			extra.chop(kEndLine.size());
		for (const QString &outputLine : extra.split(kEndLine))
		{
			if (outputLine.isEmpty())
				continue;
			if (m_runtime)
				m_runtime->firePluginScreendraw(0, logOutput && !triggerResult.omitFromLog ? 1 : 0,
				                                outputLine);
			// Legacy behavior: trigger "send to output" text is comment/note output,
			// not normal world-output text.
			m_view->appendNoteText(outputLine, true);
		}
	}

	QVector<LuaStyleRun> styleRuns;
	if (m_runtime && (!triggerResult.deferredScripts.isEmpty() || !triggerResult.triggerScripts.isEmpty()))
	{
		loadDefaultColors();
		styleRuns = buildStyleRuns(line, triggerResult.spans, defaultFore, defaultBack);
	}

	for (const auto &[plugin, scriptText, description] : triggerResult.deferredScripts)
	{
		LuaCallbackEngine *lua =
		    plugin ? plugin->lua.data() : (m_runtime ? m_runtime->luaCallbacks() : nullptr);
		if (!plugin && !canExecuteWorldScript(QStringLiteral("Script"), description, lua, false))
			continue;
		if (!lua)
			continue;
		if (m_runtime)
			m_runtime->setCurrentActionSource(WorldRuntime::eTriggerFired);
		lua->executeScript(scriptText, description, &styleRuns);
		if (m_runtime)
			m_runtime->setCurrentActionSource(WorldRuntime::eUnknownActionSource);
	}

	for (const auto &[plugin, index, label, scriptLine, wildcards, namedWildcards] :
	     triggerResult.triggerScripts)
	{
		WorldRuntime::Trigger *trigger    = nullptr;
		auto                   matchLabel = [&](const WorldRuntime::Trigger &candidate) -> bool
		{
			const QString triggerName = candidate.attributes.value(QStringLiteral("name")).trimmed();
			const QString match       = candidate.attributes.value(QStringLiteral("match")).trimmed();
			return triggerName == label || match == label;
		};
		if (plugin)
		{
			if (index >= 0 && index < plugin->triggers.size())
			{
				WorldRuntime::Trigger &candidate = plugin->triggers[index];
				if (matchLabel(candidate))
					trigger = &candidate;
			}
			if (!trigger)
			{
				for (auto &candidate : plugin->triggers)
				{
					if (matchLabel(candidate))
					{
						trigger = &candidate;
						break;
					}
				}
			}
		}
		else if (m_runtime)
		{
			QList<WorldRuntime::Trigger> &triggers = m_runtime->triggersMutable();
			if (index >= 0 && index < triggers.size())
			{
				WorldRuntime::Trigger &candidate = triggers[index];
				if (matchLabel(candidate))
					trigger = &candidate;
			}
			if (!trigger)
			{
				for (auto &candidate : triggers)
				{
					if (matchLabel(candidate))
					{
						trigger = &candidate;
						break;
					}
				}
			}
		}
		if (!trigger)
			continue;
		const QString scriptName = trigger->attributes.value(QStringLiteral("script"));
		if (scriptName.isEmpty())
			continue;
		LuaCallbackEngine *lua =
		    plugin ? plugin->lua.data() : (m_runtime ? m_runtime->luaCallbacks() : nullptr);
		if (!plugin && !canExecuteWorldScript(QStringLiteral("Trigger"), scriptName, lua, true))
			continue;
		if (!lua)
			continue;
		trigger->executingScript = true;
		if (m_runtime)
			m_runtime->setCurrentActionSource(WorldRuntime::eTriggerFired);
		lua->callFunctionWithStringsAndWildcards(scriptName, {label, scriptLine}, wildcards, namedWildcards,
		                                         &styleRuns);
		if (m_runtime)
			m_runtime->setCurrentActionSource(WorldRuntime::eUnknownActionSource);
		trigger->executingScript = false;
		trigger->invocationCount++;
	}

	if (!triggerResult.oneShotTriggers.isEmpty())
	{
		auto removeTriggerByName = [](QList<WorldRuntime::Trigger> &triggers, const QString &name)
		{
			const QString normalized = name.trimmed().toLower();
			for (int i = safeQSizeToInt(triggers.size()) - 1; i >= 0; --i)
			{
				const QString label =
				    triggers.at(i).attributes.value(QStringLiteral("name")).trimmed().toLower();
				const QString match =
				    triggers.at(i).attributes.value(QStringLiteral("match")).trimmed().toLower();
				if (label == normalized || match == normalized)
					triggers.removeAt(i);
			}
		};
		for (const auto &[plugin, name] : triggerResult.oneShotTriggers)
		{
			if (plugin)
				removeTriggerByName(plugin->triggers, name);
			else if (m_runtime)
			{
				QList<WorldRuntime::Trigger> triggers = m_runtime->triggersMutable();
				removeTriggerByName(triggers, name);
				m_runtime->setTriggers(triggers);
			}
		}
	}

	if (m_runtime && !triggerResult.omitFromOutput)
	{
		// Keep the incoming line as the active Lua context until all
		// trigger/deferred scripts have run, then commit.
		m_runtime->markIncomingLineLuaContextCommitted();
	}

	if (m_runtime)
	{
		logOutput = isEnabled(m_runtime->worldAttributes().value(QStringLiteral("log_output")));
		if (logOutput && !triggerResult.omitFromLog)
			logLine(line, QStringLiteral("log_line_preamble_output"),
			        QStringLiteral("log_line_postamble_output"));
		m_runtime->endIncomingLineLuaContext();
	}
}

void WorldCommandProcessor::onIncomingStyledLinePartialReceived(
    const QString &line, const QVector<WorldRuntime::StyleSpan> &spans) const
{
	if (!m_view)
		return;
	m_view->updatePartialOutputText(line, spans);
	if (m_runtime)
		m_runtime->firePluginPartialLine(line);
}

void WorldCommandProcessor::onHyperlinkActivated(const QString &href)
{
	if (href.isEmpty())
		return;

	if (const QString lower = href.toLower(); lower.startsWith(QStringLiteral("http://")) ||
	                                          lower.startsWith(QStringLiteral("https://")) ||
	                                          lower.startsWith(QStringLiteral("mailto:")))
	{
		QDesktopServices::openUrl(QUrl(href));
		return;
	}

	if (m_runtime && m_view)
	{
		const QVector<WorldRuntime::LineEntry> &lines = m_runtime->lines();
		for (int i = safeQSizeToInt(lines.size()) - 1; i >= 0; --i)
		{
			for (const WorldRuntime::StyleSpan &span : lines.at(i).spans)
			{
				if (span.action == href && span.actionType == WorldRuntime::ActionPrompt)
				{
					m_view->setInputText(href, true);
					return;
				}
			}
		}
	}

	bool echo         = true;
	bool addToHistory = false;
	if (m_runtime)
	{
		const QMap<QString, QString> &attrs     = m_runtime->worldAttributes();
		auto                          isEnabled = [](const QString &value)
		{
			return value == QStringLiteral("1") ||
			       value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
			       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
		};
		echo         = isEnabled(attrs.value(QStringLiteral("echo_hyperlink_in_output_window")));
		addToHistory = isEnabled(attrs.value(QStringLiteral("hyperlink_adds_to_command_history")));
	}

	sendMsg(href, echo, false, true);
	if (addToHistory && m_view)
		m_view->addHyperlinkToHistory(href);
}

void WorldCommandProcessor::note(const QString &text, const bool newLine) const
{
	if (m_runtime)
	{
		const QString value    = m_runtime->worldAttributes().value(QStringLiteral("log_notes"));
		const bool    logNotes = value == QStringLiteral("1") ||
		                      value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
		                      value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
		m_runtime->firePluginScreendraw(1, logNotes ? 1 : 0, text);
	}
	if (m_view)
		m_view->appendNoteText(text, newLine);
	logLine(text, QStringLiteral("log_line_preamble_notes"), QStringLiteral("log_line_postamble_notes"));
}

void WorldCommandProcessor::sendRawText(const QString &text, const bool echo, const bool queue,
                                        const bool log, const bool history)
{
	sendMsg(text, echo, queue, log);
	if (history && m_view)
		m_view->addToHistoryForced(text);
}

void WorldCommandProcessor::sendImmediateText(const QString &text, const bool echo, const bool log,
                                              const bool history)
{
	doSendMsg(text, echo, log);
	if (history && m_view)
		m_view->addToHistoryForced(text);
}

void WorldCommandProcessor::logInputCommand(const QString &text) const
{
	QString logText = text;
	if (logText.endsWith(kEndLine))
		logText.chop(kEndLine.size());
	logText.replace(kEndLine, QStringLiteral("\n"));
	logLine(logText, QStringLiteral("log_line_preamble_input"), QStringLiteral("log_line_postamble_input"));
}

void WorldCommandProcessor::setNoEcho(const bool enabled)
{
	m_noEcho = enabled;
}

bool WorldCommandProcessor::noEcho() const
{
	return m_noEcho;
}

bool WorldCommandProcessor::pluginProcessingSent() const
{
	return m_pluginProcessingSent;
}

void WorldCommandProcessor::handleWorldConnected()
{
	if (!m_runtime)
		return;

	const QMap<QString, QString> &attrs     = m_runtime->worldAttributes();
	const QMap<QString, QString> &multi     = m_runtime->worldMultilineAttributes();
	auto                          isEnabled = [](const QString &value)
	{
		return value == QStringLiteral("1") || value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
		       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
	};
	auto usesPlayerDirective = [](const QString &pattern)
	{
		for (int i = 0; i < pattern.size(); ++i)
		{
			if (pattern.at(i) != QLatin1Char('%'))
				continue;
			if (i + 1 >= pattern.size())
				break;
			QChar next = pattern.at(++i);
			if (next == QLatin1Char('%'))
				continue;
			if (next == QLatin1Char('#') && i + 1 < pattern.size())
				next = pattern.at(++i);
			if (next == QLatin1Char('P'))
				return true;
		}
		return false;
	};

	const bool    logHtml         = isEnabled(attrs.value(QStringLiteral("log_html")));
	const QString autoLogFileName = attrs.value(QStringLiteral("auto_log_file_name"));
	const QString playerName      = attrs.value(QStringLiteral("player")).trimmed();
	const bool    canAutoLog =
	    !autoLogFileName.isEmpty() && !(usesPlayerDirective(autoLogFileName) && playerName.isEmpty());
	if (canAutoLog)
	{
		const QDateTime now = QDateTime::currentDateTime();
		if (const QString strName = m_runtime->formatTime(now, autoLogFileName, logHtml);
		    m_runtime->openLog(strName, true) == eCouldNotOpenFile)
			QMessageBox::information(m_view, QStringLiteral("QMud"),
			                         QStringLiteral("Could not open log file \"%1\"").arg(strName));
		else
		{
			QString preamble = multi.value(QStringLiteral("log_file_preamble"));
			if (preamble.isEmpty())
				preamble = attrs.value(QStringLiteral("log_file_preamble"));

			if (!preamble.isEmpty())
			{
				preamble.replace(QStringLiteral("%n"), QStringLiteral("\n"));
				preamble = m_runtime->formatTime(now, preamble, logHtml);
				preamble.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
				m_runtime->writeLog(preamble);
				m_runtime->writeLog(QStringLiteral("\n"));
			}

			if (const bool writeWorldName = isEnabled(attrs.value(QStringLiteral("write_world_name_to_log")));
			    writeWorldName)
			{
				const QString strTime =
				    m_runtime->formatTime(now, QStringLiteral("%A, %B %d, %Y, %#I:%M %p"), false);
				QString strPreamble = attrs.value(QStringLiteral("name"));
				strPreamble += QStringLiteral(" - ");
				strPreamble += strTime;

				if (logHtml)
				{
					m_runtime->writeLog(QStringLiteral("<br>\n"));
					m_runtime->writeLog(fixHtmlString(strPreamble));
					m_runtime->writeLog(QStringLiteral("<br>\n"));
				}
				else
				{
					m_runtime->writeLog(QStringLiteral("\n"));
					m_runtime->writeLog(strPreamble);
					m_runtime->writeLog(QStringLiteral("\n"));
				}

				const QString strHyphens(strPreamble.length(), QLatin1Char('-'));
				m_runtime->writeLog(strHyphens);
				if (logHtml)
					m_runtime->writeLog(QStringLiteral("<br><br>"));
				else
					m_runtime->writeLog(QStringLiteral("\n\n"));
			}
		}
	}

	const int     connectMethod = attrs.value(QStringLiteral("connect_method")).toInt();
	const QString player        = attrs.value(QStringLiteral("player"));
	const QString worldName     = attrs.value(QStringLiteral("name"));
	QString       password      = attrs.value(QStringLiteral("password"));
	QString       connectText   = multi.value(QStringLiteral("connect_text"));
	if (connectText.isEmpty())
		connectText = attrs.value(QStringLiteral("connect_text"));

	const QString displayInput = attrs.value(QStringLiteral("display_my_input"));
	const bool    echoInput    = isEnabledValue(displayInput);

	const bool    needsPassword =
	    password.isEmpty() && ((connectMethod != 0 && !player.isEmpty()) ||
	                           connectText.contains(QStringLiteral("%password%"), Qt::CaseInsensitive));
	if (needsPassword)
	{
		bool          ok     = false;
		const QString prompt = QStringLiteral("Enter password for %1")
		                           .arg(worldName.isEmpty() ? QStringLiteral("world") : worldName);
		password = QInputDialog::getText(m_view, QStringLiteral("Password"), prompt, QLineEdit::Password,
		                                 QString(), &ok);
		if (!ok)
			password.clear();
	}

	switch (connectMethod)
	{
	case 1: // eConnectMUSH
		if (!player.isEmpty() && !password.isEmpty())
		{
			const QString cmd = QStringLiteral("connect %1 %2").arg(player, password);
			sendMsg(cmd, false, false, false);
		}
		break;
	case 2: // eConnectDiku
		if (!player.isEmpty())
			sendMsg(player, echoInput, false, false);
		if (!password.isEmpty())
			sendMsg(password, false, false, false);
		break;
	default:
		break;
	}

	if (!connectText.isEmpty())
	{
		QString text = connectText;
		text.replace(QStringLiteral("%name%"), player);
		text.replace(QStringLiteral("%password%"), password);
		sendMsg(text, false, false, false);
	}

	m_runtime->fireWorldConnectHandlers();
}

void WorldCommandProcessor::handleWorldDisconnected() const
{
	if (!m_runtime)
		return;
	m_runtime->fireWorldDisconnectHandlers();
	const QMap<QString, QString> &attrs           = m_runtime->worldAttributes();
	const QString                 autoLogFileName = attrs.value(QStringLiteral("auto_log_file_name"));
	const AppController          *app             = AppController::instance();
	if (!autoLogFileName.isEmpty())
		m_runtime->closeLog();

	const bool saveWorldAutomatically =
	    isEnabledValue(attrs.value(QStringLiteral("save_world_automatically")));
	if (const bool needsSave = m_runtime->worldFileModified() || m_runtime->variablesChanged();
	    !saveWorldAutomatically || !needsSave)
	{
		return;
	}

	const QString savePath = ensureDisconnectSaveWorldPath(*m_runtime, app);
	if (savePath.trimmed().isEmpty())
		return;

	QString saveError;
	if (!m_runtime->saveWorldFile(savePath, &saveError))
	{
		if (!saveError.isEmpty())
			note(QStringLiteral("Unable to save world on disconnect: %1").arg(saveError), true);
		return;
	}

	m_runtime->setWorldFilePath(savePath);
	m_runtime->setWorldFileModified(false);
	m_runtime->setVariablesChanged(false);

	QString backupError;
	if (!createDisconnectWeekdayBackup(savePath, *m_runtime, app, backupError) && !backupError.isEmpty())
		note(QStringLiteral("Unable to create disconnect backup: %1").arg(backupError), true);
}

bool WorldCommandProcessor::evaluateCommand(const QString &input)
{
	if (!m_runtime)
		return false;

	m_runtime->setLastUserInput(QDateTime::currentDateTime());

	QString line = input;
	line.replace(QStringLiteral("\r"), QString());

	const QMap<QString, QString> &attrs        = m_runtime->worldAttributes();
	const QString                 displayInput = attrs.value(QStringLiteral("display_my_input"));
	const bool                    echoInput    = isEnabledValue(displayInput);

	const auto                    isEnabled = [](const QString &value) -> bool
	{
		return value == QStringLiteral("1") || value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
		       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
	};

	// ----------------------------- AUTO SAY ------------------------------
	bool          autoSay = !m_processingAutoSay && isEnabled(attrs.value(QStringLiteral("enable_auto_say")));
	const QString autoSayString     = attrs.value(QStringLiteral("auto_say_string"));
	const QString overridePrefix    = attrs.value(QStringLiteral("auto_say_override_prefix"));
	const bool    excludeNonAlpha   = isEnabled(attrs.value(QStringLiteral("autosay_exclude_non_alpha")));
	const bool    excludeMacros     = isEnabled(attrs.value(QStringLiteral("autosay_exclude_macros")));
	const bool    reEvaluateAutoSay = isEnabled(attrs.value(QStringLiteral("re_evaluate_auto_say")));

	if (autoSay && !overridePrefix.isEmpty() && line.startsWith(overridePrefix))
	{
		autoSay = false;
		line    = line.mid(overridePrefix.size());
	}

	if (autoSay && line.isEmpty())
		autoSay = false;

	if (autoSay && excludeNonAlpha)
	{
		if (const QChar first = line.at(0); !first.isLetterOrNumber())
			autoSay = false;
	}

	if (autoSay && excludeMacros)
	{
		for (const auto &[attributes, children] : m_runtime->macros())
		{
			if (const QString type = attributes.value(QStringLiteral("type")).trimmed().toLower();
			    type != QStringLiteral("replace") && type != QStringLiteral("send_now"))
			{
				continue;
			}
			const QString macroText = children.value(QStringLiteral("send"));
			if (macroText.isEmpty())
				continue;
			if (line.startsWith(macroText))
			{
				autoSay = false;
				break;
			}
		}
	}

	if (autoSay && !autoSayString.isEmpty() && line.startsWith(autoSayString))
		autoSay = false;

	if (autoSay && !autoSayString.isEmpty())
	{
		if (!reEvaluateAutoSay && m_runtime->connectPhase() != WorldRuntime::eConnectConnectedToMud)
		{
			if (m_processingEnteredCommand)
				m_enteredCommandSendFailed = true;
			return true;
		}

		const QStringList commands      = line.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
		const QString     savedStacking = attrs.value(QStringLiteral("enable_command_stack"));
		m_runtime->setWorldAttribute(QStringLiteral("enable_command_stack"), QStringLiteral("0"));
		m_processingAutoSay = true;
		for (const QString &command : commands)
		{
			const QString prefixed = autoSayString + command;
			if (reEvaluateAutoSay)
				evaluateCommand(prefixed);
			else
				sendMsg(prefixed, echoInput, false, true);
		}
		m_processingAutoSay = false;
		m_runtime->setWorldAttribute(QStringLiteral("enable_command_stack"), savedStacking);
		return false;
	}

	// -------------------------- SCRIPT PREFIX ---------------------------
	const bool scriptingEnabled =
	    isEnabled(attrs.value(QStringLiteral("enable_scripts"))) &&
	    attrs.value(QStringLiteral("script_language")).compare(QStringLiteral("Lua"), Qt::CaseInsensitive) ==
	        0;
	if (const QString scriptPrefix = attrs.value(QStringLiteral("script_prefix"));
	    !m_processingAutoSay && scriptingEnabled && !scriptPrefix.isEmpty() && line.startsWith(scriptPrefix))
	{
		const QString code = line.mid(scriptPrefix.size());
		sendTo(eSendToScript, code, false, false, QString(), QStringLiteral("script prefix"), nullptr);
		return false;
	}

	// ---------------------- COMMAND STACKING ------------------------------
	const QString commandStackEnabled = attrs.value(QStringLiteral("enable_command_stack"));
	const bool    enableCommandStack  = isEnabledValue(commandStackEnabled);
	if (const QString commandStackCharacter = attrs.value(QStringLiteral("command_stack_character"));
	    enableCommandStack && !commandStackCharacter.isEmpty())
	{
		if (const auto stackChar = commandStackCharacter.at(0); !line.isEmpty() && line.at(0) == stackChar)
		{
			// Leading stack char disables command stacking for this line.
			line.remove(0, 1);
		}
		else
		{
			QString escaped;
			escaped += stackChar;
			escaped += stackChar;
			constexpr QChar placeholder = QChar(0x01);
			line.replace(escaped, QString(placeholder));
			line.replace(stackChar, QLatin1Char('\n'));
			line.replace(QString(placeholder), QString(stackChar));

			if (const QStringList stacked = line.split(QLatin1Char('\n')); stacked.size() > 1)
			{
				for (const QString &part : stacked)
					evaluateCommand(part);
				return false;
			}
		}
	}

	// ------------------------- PLUGIN COMMAND CALLBACK -------------------
	// Legacy Execute() calls OnPluginCommand per stacked command before normal
	// command evaluation; false return omits this command.
	if (m_runtime && !m_runtime->firePluginCommand(line))
		return false;

	if (line.isEmpty())
	{
		sendMsg(QString(), echoInput, false, true);
		return true;
	}

	// ------------------------- SPEED WALKING ------------------------------
	// see if they are doing speed walking

	const QString speedWalkEnabled = attrs.value(QStringLiteral("enable_speed_walk"));
	if (const QString speedWalkPrefix = attrs.value(QStringLiteral("speed_walk_prefix"));
	    isEnabledValue(speedWalkEnabled) && !speedWalkPrefix.isEmpty() && line.startsWith(speedWalkPrefix))
	{
		if (const QString evaluated = doEvaluateSpeedwalk(line.mid(speedWalkPrefix.length()));
		    !evaluated.isEmpty())
		{
			if (evaluated.at(0) == QLatin1Char('*'))
			{
				QMessageBox::warning(m_view, QStringLiteral("QMud"), evaluated.mid(1));
				return true;
			}
			// sendMsg/doSendMsg enforce connected-state checks (CheckConnected behavior).
			sendMsg(evaluated, echoInput, true, true);
		}
		return false;
	}

	// --------------------------- ALIASES ------------------------------
	bool              bEchoAlias       = echoInput;
	bool              bOmitFromLog     = false;
	bool              bOmitFromHistory = false;
	QVector<AliasRef> matchedAliases;
	QVector<AliasRef> oneShotAliases;

	if (const QString enableAliases = attrs.value(QStringLiteral("enable_aliases"));
	    isEnabledValue(enableAliases))
	{
		bool                         stopAliasEvaluation = false;
		QList<WorldRuntime::Plugin> &pluginList          = m_runtime->pluginsMutable();
		const QVector<int>          &pluginIndices       = sortedPluginIndices();
		for (int pluginIndex : pluginIndices)
		{
			if (pluginIndex < 0 || pluginIndex >= pluginList.size())
				continue;
			WorldRuntime::Plugin *plugin = &pluginList[pluginIndex];
			if (!plugin || !plugin->enabled || plugin->installPending || plugin->sequence >= 0)
				continue;
			if (processOneAliasSequence(line, true, bOmitFromLog, bEchoAlias, bOmitFromHistory,
			                            matchedAliases, oneShotAliases, plugin))
			{
				stopAliasEvaluation = true;
				break;
			}
		}

		if (!stopAliasEvaluation &&
		    processOneAliasSequence(line, true, bOmitFromLog, bEchoAlias, bOmitFromHistory, matchedAliases,
		                            oneShotAliases, nullptr))
		{
			stopAliasEvaluation = true;
		}

		for (int pluginIndex : pluginIndices)
		{
			if (stopAliasEvaluation)
				break;
			if (pluginIndex < 0 || pluginIndex >= pluginList.size())
				continue;
			WorldRuntime::Plugin *plugin = &pluginList[pluginIndex];
			if (!plugin || !plugin->enabled || plugin->installPending || plugin->sequence < 0)
				continue;
			if (processOneAliasSequence(line, true, bOmitFromLog, bEchoAlias, bOmitFromHistory,
			                            matchedAliases, oneShotAliases, plugin))
			{
				break;
			}
		}
	}

	if (bOmitFromHistory)
		m_omitFromHistoryForEnteredCommand = true;

	if (matchedAliases.isEmpty())
	{
		auto quitMacroText = QStringLiteral("quit");
		for (const auto &[attributes, children] : m_runtime->macros())
		{
			if (const QString name = attributes.value(QStringLiteral("name")).trimmed();
			    name.compare(QStringLiteral("quit"), Qt::CaseInsensitive) != 0)
			{
				continue;
			}
			if (const QString configured = children.value(QStringLiteral("send")).trimmed();
			    !configured.isEmpty())
			{
				quitMacroText = configured;
			}
			break;
		}
		if (!quitMacroText.isEmpty() && line.compare(quitMacroText, Qt::CaseInsensitive) == 0)
			m_runtime->setDisconnectOk(true);

		sendMsg(line, echoInput, false, !bOmitFromLog);
		return false;
	}

	for (const auto &[plugin, index, aliasName, wildcards, namedWildcards] : matchedAliases)
	{
		WorldRuntime::Alias *alias = nullptr;
		if (plugin)
		{
			if (index >= 0 && index < plugin->aliases.size())
				alias = &plugin->aliases[index];
		}
		else if (m_runtime)
		{
			QList<WorldRuntime::Alias> &aliases = m_runtime->aliasesMutable();
			if (index >= 0 && index < aliases.size())
				alias = &aliases[index];
		}
		if (!alias)
			continue;
		const QString scriptName = alias->attributes.value(QStringLiteral("script"));
		if (scriptName.isEmpty())
			continue;
		const QString label =
		    aliasName.isEmpty() ? alias->attributes.value(QStringLiteral("match")) : aliasName;
		LuaCallbackEngine *lua = plugin      ? plugin->lua.data()
		                         : m_runtime ? m_runtime->luaCallbacks()
		                                     : nullptr;
		if (!plugin && !canExecuteWorldScript(QStringLiteral("Alias"), scriptName, lua, true))
			continue;
		if (lua)
		{
			alias->executingScript = true;
			lua->callFunctionWithStringsAndWildcards(scriptName, {label, line}, wildcards, namedWildcards,
			                                         nullptr);
			alias->executingScript = false;
			alias->invocationCount++;
		}
	}

	if (!oneShotAliases.isEmpty())
	{
		auto removeAliasByName = [](QList<WorldRuntime::Alias> &aliases, const QString &name)
		{
			const QString normalized = name.trimmed().toLower();
			for (int i = safeQSizeToInt(aliases.size()) - 1; i >= 0; --i)
			{
				const QString label =
				    aliases.at(i).attributes.value(QStringLiteral("name")).trimmed().toLower();
				if (label == normalized)
					aliases.removeAt(i);
			}
		};
		for (const AliasRef &ref : oneShotAliases)
		{
			if (ref.plugin)
				removeAliasByName(ref.plugin->aliases, ref.name);
			else if (m_runtime)
			{
				QList<WorldRuntime::Alias> aliases = m_runtime->aliasesMutable();
				removeAliasByName(aliases, ref.name);
				m_runtime->setAliases(aliases);
			}
		}
	}

	return true;
}

QString WorldCommandProcessor::makeSpeedWalkErrorString(const QString &message)
{
	return QMudSpeedwalk::makeSpeedWalkErrorString(message);
}

QString WorldCommandProcessor::doEvaluateSpeedwalk(const QString &speedWalkString) const
{
	const AppController *app      = AppController::instance();
	auto                 resolver = [app](const QString &direction) -> QString
	{ return app ? app->mapDirectionToSend(direction) : QString(); };
	return QMudSpeedwalk::evaluateSpeedwalk(speedWalkString, m_speedWalkFiller, resolver);
} // end of WorldCommandProcessor::doEvaluateSpeedwalk

QString WorldCommandProcessor::convertToRegularExpression(const QString &matchString, const bool wholeLine,
                                                          const bool makeAsterisksWildcards)
{
	return QMudCommandPattern::convertToRegularExpression(matchString, wholeLine, makeAsterisksWildcards);
} // end of convertToRegularExpression

QString WorldCommandProcessor::fixupEscapeSequences(const QString &source)
{
	return QMudCommandText::fixupEscapeSequences(source);

} // end of FixupEscapeSequences

QString WorldCommandProcessor::fixWildcard(const QString &wildcard, const bool makeLowerCase,
                                           const int sendTo, const QString &language)
{
	return QMudCommandText::fixWildcard(wildcard, makeLowerCase, sendTo, language);
} // end of FixWildcard

bool WorldCommandProcessor::findVariable(const QString &name, QString &value,
                                         const WorldRuntime::Plugin *plugin) const
{
	if (plugin)
	{
		for (auto it = plugin->variables.constBegin(); it != plugin->variables.constEnd(); ++it)
		{
			if (it.key().compare(name, Qt::CaseInsensitive) == 0)
			{
				value = it.value();
				return true;
			}
		}
		return false;
	}

	if (!m_runtime)
		return false;
	return m_runtime->findVariable(name, value);
}

QString WorldCommandProcessor::fixSendText(const QString &source, int sendTo, const QStringList &wildcards,
                                           const QMap<QString, QString> &namedWildcards,
                                           const QString &language, bool makeWildcardsLower,
                                           bool expandVariables, bool expandWildcards, bool fixRegexps,
                                           bool isRegexp, bool throwExceptions, const QString &name,
                                           const WorldRuntime::Plugin *plugin, bool *ok) const
{
	QString      strOutput; // result of expansion

	const QChar *pText;
	const QChar *pStartOfGroup;

	pText = pStartOfGroup = source.constData();

	while (!pText->isNull())
	{
		switch (pText->unicode())
		{

			/* -------------------------------------------------------------------- *
			 *  Variable expansion - @foo becomes <contents of foo>                 *
			 * -------------------------------------------------------------------- */

		case '@':
		{
			if (!expandVariables)
			{
				pText++; // just copy the @
				break;
			}

			// copy up to the @ sign

			strOutput += QString(pStartOfGroup, static_cast<int>(pText - pStartOfGroup));

			pText++; // skip the @

			// @@ becomes @
			if (pText->unicode() == '@')
			{
				pStartOfGroup = ++pText;
				strOutput += QLatin1Char('@');
				continue;
			}

			const QChar *pName;
			bool         bEscape = fixRegexps;

			// syntax @!variable defeats the escaping

			if (pText->unicode() == '!')
			{
				pText++;
				bEscape = false;
			}

			pName = pText;

			// find end of variable name
			while (!pText->isNull())
			{
				if (pText->unicode() == '_' || pText->isLetterOrNumber())
					pText++;
				else
					break;
			}

			/* -------------------------------------------------------------------- *
			 *  We have a variable - look it up and do internal replacements        *
			 * -------------------------------------------------------------------- */

			if (QString strVariableName(pName, static_cast<int>(pText - pName)); strVariableName.isEmpty())
			{
				if (throwExceptions)
				{
					if (ok)
						*ok = false;
					QMessageBox::warning(m_view, QStringLiteral("QMud"),
					                     QStringLiteral("@ must be followed by a variable name"));
					return {};
				}
			} // end of no variable name
			else
			{
				strVariableName = strVariableName.toLower();
				QString strVariableContents;
				if (findVariable(strVariableName, strVariableContents, plugin))
				{
					// fix up so regexps don't get confused with [ etc. inside variable
					if (bEscape)
					{
						QString escaped;
						escaped.reserve(strVariableContents.size() * 2);
						for (int i = 0; i < strVariableContents.size(); ++i)
						{
							const QChar ch = strVariableContents.at(i);
							if (ch.unicode() < ' ')
								continue; // drop non-printables
							if (isRegexp)
							{
								if (!ch.isLetterOrNumber() && ch != QLatin1Char(' '))
								{
									escaped += QLatin1Char('\\'); // escape it
									escaped += ch;
								}
								else
								{
									escaped += ch; // just copy it
								}
							}
							else if (ch != QLatin1Char('*')) // not regexp
							{
								escaped += ch; // copy all except asterisks
							}
						}
						strVariableContents = escaped;
					} // end of escaping wanted

					// in the "send" box may need to convert script expansions etc.
					if (!fixRegexps)
						strVariableContents = fixWildcard(strVariableContents,
						                                  false, // not force variables to lowercase
						                                  sendTo, language);

					// fix up HTML sequences if we are sending it to a log file
					const QString logHtml = m_runtime->worldAttributes().value(QStringLiteral("log_html"));
					if (isEnabledValue(logHtml) && sendTo == eSendToLogFile)
						strVariableContents = fixHtmlString(strVariableContents);

					strOutput += strVariableContents;
				} // end of name existing in variable map
				else
				{
					if (throwExceptions)
					{
						if (ok)
							*ok = false;
						QMessageBox::warning(
						    m_view, QStringLiteral("QMud"),
						    QStringLiteral("Variable '%1' is not defined.").arg(strVariableName));
						return {};
					}
				} // end of variable does not exist

			} // end of not empty name

			// get ready for next batch from beyond the variable
			pStartOfGroup = pText;
		}
		break; // end of '@'

			/* -------------------------------------------------------------------- *
			 *  Wildcard substitution - %1 becomes <contents of wildcard 1>         *
			 *                        - %<foo> becomes <contents of wildcard "foo"> *
			 *                        - %% becomes %                                *
			 * -------------------------------------------------------------------- */

		case '%':

			if (!expandWildcards)
			{
				pText++; // just copy the %
				break;
			}

			// see what comes after the % symbol
			switch (pText[1].unicode())
			{

				/* -------------------------------------------------------------------- *
				 *  %%                                                                  *
				 * -------------------------------------------------------------------- */

			case '%':

				// copy up to - and including - the percent sign

				strOutput += QString(pStartOfGroup, static_cast<int>(pText - pStartOfGroup + 1));

				// get ready for next batch from beyond the %%

				pText += 2; // don't reprocess the %%
				pStartOfGroup = pText;
				break; // end of %%

				/* -------------------------------------------------------------------- *
				 *  %0 to %9                                                            *
				 * -------------------------------------------------------------------- */

			case '0': // a digit?
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
			case '9':
			{

				// copy up to the percent sign

				strOutput += QString(pStartOfGroup, static_cast<int>(pText - pStartOfGroup));

				// output the appropriate replacement text

				if (const int index = pText[1].unicode() - '0'; index >= 0 && index < wildcards.size())
				{
					QString strWildcard =
					    fixWildcard(wildcards.at(index), makeWildcardsLower, sendTo, language);

					// fix up HTML sequences if we are sending it to a log file
					const QString logHtml = m_runtime->worldAttributes().value(QStringLiteral("log_html"));
					if (isEnabledValue(logHtml) && sendTo == eSendToLogFile)
						strWildcard = fixHtmlString(strWildcard);

					strOutput += strWildcard;
				}

				// get ready for next batch from beyond the digit

				pText += 2;
				pStartOfGroup = pText;
			}
			break; // end of %(digit) (eg. %2)

				/* -------------------------------------------------------------------- *
				 *  %<name>                                                             *
				 * -------------------------------------------------------------------- */

			case '<':
			{
				// copy up to the % sign

				strOutput += QString(pStartOfGroup, static_cast<int>(pText - pStartOfGroup));

				pText += 2; // skip the %<

				const QChar *pName = pText;

				// find end of wildcard name
				while (!pText->isNull())
				{
					if (pText->unicode() != '>')
						pText++;
					else
						break;
				}

				if (QString wildcardName(pName, static_cast<int>(pText - pName)); !wildcardName.isEmpty())
				{
					QString strWildcard;
					if (namedWildcards.contains(wildcardName))
						strWildcard = namedWildcards.value(wildcardName);
					else
					{
						bool okNumber = false;
						if (const int numeric = wildcardName.toInt(&okNumber);
						    okNumber && numeric >= 0 && numeric < wildcards.size())
						{
							strWildcard = wildcards.at(numeric);
						}
					}

					if (!strWildcard.isEmpty())
					{
						strWildcard = fixWildcard(strWildcard, makeWildcardsLower, sendTo, language);

						// fix up HTML sequences if we are sending it to a log file
						const QString logHtml =
						    m_runtime->worldAttributes().value(QStringLiteral("log_html"));
						if (isEnabledValue(logHtml) && sendTo == eSendToLogFile)
							strWildcard = fixHtmlString(strWildcard);

						strOutput += strWildcard;
					}
				}
				// get ready for next batch from beyond the name

				if (pText->unicode() == '>')
					pText++;
				pStartOfGroup = pText;
			}
			break; // end of %<foo>

				/* -------------------------------------------------------------------- *
				 *  %C - clipboard contents                                             *
				 * -------------------------------------------------------------------- */

			case 'C':
			case 'c':
			{
				// copy up to the percent sign
				strOutput += QString(pStartOfGroup, static_cast<int>(pText - pStartOfGroup));

				if (QString strClipboard = QGuiApplication::clipboard()->text(); !strClipboard.isEmpty())
				{
					strOutput += strClipboard;
				}
				else if (throwExceptions)
				{
					if (ok)
						*ok = false;
					QMessageBox::warning(m_view, QStringLiteral("QMud"),
					                     QStringLiteral("No text on the Clipboard"));
					return {};
				}

				// get ready for next batch from beyond the 'c'

				pText += 2;
				pStartOfGroup = pText;
			}
			break; // end of %c

				/* -------------------------------------------------------------------- *
				 *  %N - name of the thing                                              *
				 * -------------------------------------------------------------------- */

			case 'N':
			case 'n':
			{
				// copy up to the percent sign
				strOutput += QString(pStartOfGroup, static_cast<int>(pText - pStartOfGroup));

				if (!name.isEmpty())
					strOutput += name;

				// get ready for next batch from beyond the 'n'

				pText += 2;
				pStartOfGroup = pText;
			}
			break; // end of %n

				/* -------------------------------------------------------------------- *
				 *  %(something else)                                                   *
				 * -------------------------------------------------------------------- */

			default:
				pText++;
				break;

			} // end of switch on character after '%'

			break; // end of '%(something)'

			/* -------------------------------------------------------------------- *
			 *  All other characters - just increment pointer so we can copy later  *
			 * -------------------------------------------------------------------- */

		default:
			pText++;
			break;

		} // end of switch on *pText

	} // end of not end of string yet

	// copy last group

	strOutput += QString(pStartOfGroup);

	if (ok)
		*ok = true;
	return strOutput;
} // end of  WorldCommandProcessor::fixSendText

bool WorldCommandProcessor::processOneAliasSequence(const QString &currentLine, bool countThem,
                                                    bool &omitFromLog, bool &echoAlias, bool &omitFromHistory,
                                                    QVector<AliasRef>    &matchedAliases,
                                                    QVector<AliasRef>    &oneShotAliases,
                                                    WorldRuntime::Plugin *plugin)
{
	if (!m_runtime)
		return false;

	QElapsedTimer timer;
	timer.start();
	auto recordTime = [this, &timer]
	{
		if (m_runtime)
			m_runtime->addAliasTimeNs(timer.nsecsElapsed());
	};

	auto attrTrue = [](const QString &value)
	{
		return value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 || value == QStringLiteral("1") ||
		       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
	};

	QList<WorldRuntime::Alias> &aliases    = plugin ? plugin->aliases : m_runtime->aliasesMutable();
	const auto                  cacheKey   = reinterpret_cast<quintptr>(&aliases);
	const quint64               signature  = aliasOrderSignature(aliases);
	const AliasOrderCacheEntry *cacheEntry = nullptr;
	const int                   count      = safeQSizeToInt(aliases.size());
	if (auto cacheIt = m_aliasOrderCache.constFind(cacheKey);
	    cacheIt != m_aliasOrderCache.constEnd() && cacheIt->count == count && cacheIt->signature == signature)
	{
		cacheEntry = &cacheIt.value();
	}
	else
	{
		AliasOrderCacheEntry rebuilt;
		rebuilt.count     = count;
		rebuilt.signature = signature;
		rebuilt.indices.reserve(count);
		for (int i = 0; i < count; ++i)
		{
			rebuilt.indices.push_back(i);
		}
		std::ranges::sort(rebuilt.indices,
		                  [&](const int left, const int right)
		                  {
			                  const WorldRuntime::Alias &a = aliases.at(left);
			                  const WorldRuntime::Alias &b = aliases.at(right);
			                  const int seqA = a.attributes.value(QStringLiteral("sequence")).toInt();
			                  if (const int seqB = b.attributes.value(QStringLiteral("sequence")).toInt();
			                      seqA != seqB)
				                  return seqA < seqB;
			                  // Legacy behavior: alias tie-break is name, not match text.
			                  return a.attributes.value(QStringLiteral("name")) <
			                         b.attributes.value(QStringLiteral("name"));
		                  });
		if (m_aliasOrderCache.size() > 2048)
			m_aliasOrderCache.clear();
		m_aliasOrderCache.insert(cacheKey, rebuilt);
		cacheEntry = &m_aliasOrderCache[cacheKey];
	}

	if (!cacheEntry)
	{
		recordTime();
		return false;
	}

	bool matchedAny = false;
	for (int index : cacheEntry->indices)
	{
		WorldRuntime::Alias &alias = aliases[index];
		// ignore non-enabled aliases
		if (const QString enabled = alias.attributes.value(QStringLiteral("enabled")); !attrTrue(enabled))
		{
			continue;
		}

		if (m_runtime)
			m_runtime->incrementAliasesEvaluated();
		alias.matchAttempts++;
		alias.lastMatchTarget = currentLine;

		const QString matchText = alias.attributes.value(QStringLiteral("match"));
		if (matchText.isEmpty())
			continue;

		const bool             isRegexp   = attrTrue(alias.attributes.value(QStringLiteral("regexp")));
		const bool             ignoreCase = attrTrue(alias.attributes.value(QStringLiteral("ignore_case")));

		const QString          pattern = isRegexp ? matchText : wildcardToRegexCached(matchText);
		QStringList            wildcards;
		QMap<QString, QString> namedWildcards;
		int                    startCol = 0;
		int                    endCol   = 0;
		if (!regexMatch(pattern, currentLine, ignoreCase, wildcards, namedWildcards, &startCol, &endCol))
			continue;

		matchedAny = true;
		alias.matched++;
		alias.lastMatched = QDateTime::currentDateTime();

		// if alias wants it, omit entire typed line from command history
		if (attrTrue(alias.attributes.value(QStringLiteral("omit_from_command_history"))))
		{
			omitFromHistory = true;
		}

		// Legacy behavior: if current line is not a note line, force a line change
		// on the note channel so script output does not terminate/style-bleed
		// the previous non-note line.
		if (m_runtime)
		{
			if (const QVector<WorldRuntime::LineEntry> &lines = m_runtime->lines();
			    !lines.isEmpty() && (lines.last().flags & WorldRuntime::LineNote) == 0)
			{
				m_runtime->outputText(QString(), true, true);
			}
		}

		// echo the alias they typed, unless command echo off, or previously displayed
		// (if wanted - v3.38)

		if (echoAlias && attrTrue(alias.attributes.value(QStringLiteral("echo_alias"))))
		{
			if (m_view)
				m_view->echoInputText(currentLine);
			echoAlias = false; // don't echo the same line twice
			// Log the typed alias command when input logging is enabled (LogCommand behavior).
			if (m_runtime &&
			    isEnabledValue(m_runtime->worldAttributes().value(QStringLiteral("log_input"))) &&
			    !attrTrue(alias.attributes.value(QStringLiteral("omit_from_log"))))
			{
				logLine(currentLine, QStringLiteral("log_line_preamble_input"),
				        QStringLiteral("log_line_postamble_input"));
			}
		}

		if (countThem)
		{
			if (m_runtime)
				m_runtime->incrementAliasesMatched();
		}

		omitFromLog = attrTrue(alias.attributes.value(QStringLiteral("omit_from_log")));

		// get unlabelled alias's internal name
		const QString  label       = alias.attributes.value(QStringLiteral("name"));
		const QString  scriptLabel = label.isEmpty() ? matchText : label;

		const QString  language       = m_runtime->worldAttributes().value(QStringLiteral("script_language"));
		constexpr bool lowerWildcards = false;
		QStringList    fixedWildcards = wildcards;
		for (QString &fixed : fixedWildcards)
			fixed = fixWildcard(fixed, lowerWildcards,
			                    alias.attributes.value(QStringLiteral("send_to")).toInt(), language);
		QMap<QString, QString> fixedNamed = namedWildcards;
		for (auto it = fixedNamed.begin(); it != fixedNamed.end(); ++it)
			it.value() = fixWildcard(it.value(), lowerWildcards,
			                         alias.attributes.value(QStringLiteral("send_to")).toInt(), language);

		if (m_runtime && !scriptLabel.isEmpty())
		{
			if (plugin)
				m_runtime->setPluginAliasWildcards(plugin->attributes.value(QStringLiteral("id")),
				                                   scriptLabel, fixedWildcards, fixedNamed);
			else
				m_runtime->setAliasWildcards(scriptLabel, fixedWildcards, fixedNamed);
		}

		// if we have to do parameter substitution on the alias, do it now

		QString sendText = alias.children.value(QStringLiteral("send"));

		// copy contents to strSendText area, replacing %1, %2 etc. with appropriate contents

		bool    ok = false;
		sendText   = fixSendText(
            fixupEscapeSequences(sendText), alias.attributes.value(QStringLiteral("send_to")).toInt(),
            wildcards, namedWildcards, m_runtime->worldAttributes().value(QStringLiteral("script_language")),
            false, // lower-case wildcards
            attrTrue(alias.attributes.value(QStringLiteral("expand_variables"))),
            true,  // expand wildcards
            false, // convert regexps
            false, // is it regexp or normal?
            true,  // throw exceptions
            scriptLabel, plugin, &ok);
		if (!ok)
		{
			recordTime();
			return true;
		}

		// sendTo -> sendMsg/doSendMsg enforce connected-state checks for world sends.
		Q_UNUSED(sendText);

		const int     sendToValue      = alias.attributes.value(QStringLiteral("send_to")).toInt();
		const bool    omitFromOutput   = attrTrue(alias.attributes.value(QStringLiteral("omit_from_output")));
		const bool    omitFromLogValue = attrTrue(alias.attributes.value(QStringLiteral("omit_from_log")));
		const QString variableName     = alias.attributes.value(QStringLiteral("variable"));

		alias.executingScript = true;
		sendTo(sendToValue, sendText, omitFromOutput, omitFromLogValue, variableName, scriptLabel, plugin);
		alias.executingScript = false;

		AliasRef ref;
		ref.plugin         = plugin;
		ref.index          = index;
		ref.name           = scriptLabel;
		ref.wildcards      = wildcards;
		ref.namedWildcards = namedWildcards;
		matchedAliases.push_back(ref);

		if (attrTrue(alias.attributes.value(QStringLiteral("one_shot"))))
			oneShotAliases.push_back(ref);

		// display/output behavior is handled inline in sendTo/sendMsg runtime paths.

		// only re-match if they want multiple matches

		if (!attrTrue(alias.attributes.value(QStringLiteral("keep_evaluating"))))
			break;
	} // end of looping, checking each alias

	recordTime();
	return matchedAny;
} // end of WorldCommandProcessor::processOneAliasSequence

bool WorldCommandProcessor::regexMatch(const QString &pattern, const QString &subject, const bool ignoreCase,
                                       QStringList &wildcards, QMap<QString, QString> &namedWildcards,
                                       int *startCol, int *endCol, const int startOffset,
                                       const bool multiLine) const
{
	QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;
	if (ignoreCase)
		options |= QRegularExpression::CaseInsensitiveOption;
	if (multiLine)
		options |= QRegularExpression::MultilineOption;

	QString cacheKey = pattern;
	cacheKey += QChar(0x1F);
	cacheKey += ignoreCase ? QLatin1Char('1') : QLatin1Char('0');
	cacheKey += multiLine ? QLatin1Char('1') : QLatin1Char('0');

	QRegularExpression regex;
	if (const auto cached = m_regexCache.constFind(cacheKey); cached != m_regexCache.constEnd())
	{
		regex = cached.value();
	}
	else
	{
		regex = QRegularExpression(pattern, options);
		// Keep cache bounded; clear all on overflow to avoid unbounded growth
		// from variable-expanded dynamic patterns.
		if (constexpr int kRegexCacheLimit = 4096; m_regexCache.size() >= kRegexCacheLimit)
		{
			m_regexCache.clear();
			m_invalidRegexWarnings.clear();
		}
		m_regexCache.insert(cacheKey, regex);
	}

	if (!regex.isValid())
	{
		if (!m_invalidRegexWarnings.contains(cacheKey))
		{
			m_invalidRegexWarnings.insert(cacheKey);
			QMessageBox::warning(m_view, QStringLiteral("QMud"),
			                     QStringLiteral("Failed: %1 at offset %2")
			                         .arg(regex.errorString())
			                         .arg(regex.patternErrorOffset()));
		}
		return false;
	}

	const QMudAliasMatch::MatchResult result =
	    QMudAliasMatch::matchWithCaptures(regex, subject, m_regexpMatchEmpty, startOffset);
	if (!result.matched)
		return false;

	if (startCol)
		*startCol = result.startCol;
	if (endCol)
		*endCol = result.endCol;

	wildcards      = result.wildcards;
	namedWildcards = result.namedWildcards;

	return true;
}

WorldCommandProcessor::TriggerEvaluationResult
WorldCommandProcessor::processTriggersForLine(const QString                          &line,
                                              const QVector<WorldRuntime::StyleSpan> &spans)
{
	TriggerEvaluationResult result;
	if (!m_runtime)
		return result;

	auto attrTrue = [](const QString &value)
	{
		return value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 || value == QStringLiteral("1") ||
		       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
	};

	m_runtime->addRecentLine(line);
	m_runtime->setLineOmittedFromOutput(false);

	const QMap<QString, QString> &worldAttrs = m_runtime->worldAttributes();
	if (!attrTrue(worldAttrs.value(QStringLiteral("enable_triggers"))))
		return result;

	QElapsedTimer timer;
	timer.start();

	const QString language = worldAttrs.value(QStringLiteral("script_language"));

	bool          paletteReady = false;
	AnsiPalette   palette;
	QColor        defaultFore;
	QColor        defaultBack;
	auto          ensurePaletteReady = [&]
	{
		if (paletteReady)
			return;
		palette = buildPalette(m_runtime);
		const bool custom16Default =
		    attrTrue(worldAttrs.value(QStringLiteral("custom_16_is_default_colour")));
		defaultFore = parseColorValue(worldAttrs.value(QStringLiteral("output_text_colour")));
		defaultBack = parseColorValue(worldAttrs.value(QStringLiteral("output_background_colour")));
		if (!defaultFore.isValid())
			defaultFore = custom16Default ? palette.customText.value(15) : palette.normal.value(7);
		if (!defaultBack.isValid())
			defaultBack = custom16Default ? palette.customBack.value(15) : palette.normal.value(0);
		paletteReady = true;
	};

	QVector<WorldRuntime::StyleSpan> workingSpans = spans;
	bool                             spansChanged = false;

	auto                             buildTarget = [&](const int linesToMatch) -> QString
	{
		int count = linesToMatch;
		if (count <= 0)
			count = 1;
		const QStringList recentLines = m_runtime->recentLines(count);
		QString           target;
		for (const QString &recentLine : recentLines)
		{
			target += recentLine;
			target += QLatin1Char('\n');
		}
		return target;
	};

	auto colourIndexFor = [&](const QColor &colour) -> int
	{
		ensurePaletteReady();
		for (int i = 0; i < palette.normal.size(); ++i)
		{
			if (colour == palette.normal.at(i) || colour == palette.bold.at(i))
				return i;
		}
		return -1;
	};

	auto styleAtColumn = [&](const int column, QColor &fore, QColor &back, bool &bold, bool &italic,
	                         bool &underline, bool &inverse) -> bool
	{
		ensurePaletteReady();
		const QVector<WorldRuntime::StyleSpan> &sourceSpans = workingSpans.isEmpty() ? spans : workingSpans;
		const auto effective = ensureSpansForLine(line, sourceSpans, defaultFore, defaultBack);
		int        pos       = 0;
		for (const auto &span : effective)
		{
			if (column < pos + span.length)
			{
				fore      = span.fore;
				back      = span.back;
				bold      = span.bold;
				italic    = span.italic;
				underline = span.underline;
				inverse   = span.inverse;
				return true;
			}
			pos += span.length;
		}
		if (!effective.isEmpty())
		{
			const auto &span = effective.last();
			fore             = span.fore;
			back             = span.back;
			bold             = span.bold;
			italic           = span.italic;
			underline        = span.underline;
			inverse          = span.inverse;
			return true;
		}
		return false;
	};

	auto triggerColourFromCustom = [](const int customColour) -> int
	{
		if (customColour <= 0)
			return -1;
		if (customColour == OTHER_CUSTOM + 1)
			return OTHER_CUSTOM;
		if (customColour > OTHER_CUSTOM + 1)
			return -1;
		return customColour - 1;
	};

	auto processSequence = [&](QList<WorldRuntime::Trigger> &triggers, WorldRuntime::Plugin *plugin)
	{
		const auto                    cacheKey   = reinterpret_cast<quintptr>(&triggers);
		const quint64                 signature  = triggerOrderSignature(triggers);
		const TriggerOrderCacheEntry *cacheEntry = nullptr;
		const int                     count      = safeQSizeToInt(triggers.size());
		if (auto cacheIt = m_triggerOrderCache.constFind(cacheKey);
		    cacheIt != m_triggerOrderCache.constEnd() && cacheIt->count == count &&
		    cacheIt->signature == signature)
		{
			cacheEntry = &cacheIt.value();
		}
		else
		{
			TriggerOrderCacheEntry rebuilt;
			rebuilt.count     = count;
			rebuilt.signature = signature;
			rebuilt.indices.reserve(count);
			for (int i = 0; i < count; ++i)
			{
				rebuilt.indices.push_back(i);
			}

			// Legacy behavior: trigger ordering is sequence, then match text.
			std::ranges::stable_sort(
			    rebuilt.indices,
			    [&](const int left, const int right)
			    {
				    const WorldRuntime::Trigger &a = triggers.at(left);
				    const WorldRuntime::Trigger &b = triggers.at(right);
				    const int seqA                 = a.attributes.value(QStringLiteral("sequence")).toInt();
				    if (const int seqB = b.attributes.value(QStringLiteral("sequence")).toInt(); seqA != seqB)
					    return seqA < seqB;
				    return a.attributes.value(QStringLiteral("match")) <
				           b.attributes.value(QStringLiteral("match"));
			    });

			if (m_triggerOrderCache.size() > 2048)
				m_triggerOrderCache.clear();
			m_triggerOrderCache.insert(cacheKey, rebuilt);
			cacheEntry = &m_triggerOrderCache[cacheKey];
		}

		if (!cacheEntry)
			return;

		for (int index : cacheEntry->indices)
		{
			if (m_runtime->stopTriggerEvaluation() != WorldRuntime::KeepEvaluating)
				break;

			WorldRuntime::Trigger &trigger = triggers[index];
			if (!attrTrue(trigger.attributes.value(QStringLiteral("enabled"))))
				continue;

			m_runtime->incrementTriggersEvaluated();
			trigger.matchAttempts++;

			QString matchText = trigger.attributes.value(QStringLiteral("match"));
			if (matchText.isEmpty())
				continue;

			const bool isRegexp   = attrTrue(trigger.attributes.value(QStringLiteral("regexp")));
			const bool ignoreCase = attrTrue(trigger.attributes.value(QStringLiteral("ignore_case")));
			const bool multiLine  = attrTrue(trigger.attributes.value(QStringLiteral("multi_line")));

			QString    target =
                multiLine ? buildTarget(trigger.attributes.value(QStringLiteral("lines_to_match")).toInt())
			                 : line;
			trigger.lastMatchTarget = target;

			if (attrTrue(trigger.attributes.value(QStringLiteral("expand_variables"))) &&
			    matchText.contains(QLatin1Char('@')))
			{
				matchText =
				    fixSendText(matchText, trigger.attributes.value(QStringLiteral("send_to")).toInt(),
				                QStringList(), QMap<QString, QString>(), language, false, true, false, true,
				                isRegexp, false, QString(), plugin, nullptr);
			}

			const QString          pattern = isRegexp ? matchText : wildcardToRegexCached(matchText);
			QStringList            wildcards;
			QMap<QString, QString> namedWildcards;
			int                    startCol = 0;
			int                    endCol   = 0;
			if (!regexMatch(pattern, target, ignoreCase, wildcards, namedWildcards, &startCol, &endCol, 0,
			                multiLine))
			{
				continue;
			}

			if (!multiLine)
			{
				QColor fore;
				QColor back;
				bool   bold      = false;
				bool   italic    = false;
				bool   underline = false;
				bool   inverse   = false;
				if (!styleAtColumn(startCol, fore, back, bold, italic, underline, inverse))
					continue;

				if (inverse)
					qSwap(fore, back);

				const bool matchTextColour =
				    attrTrue(trigger.attributes.value(QStringLiteral("match_text_colour")));
				const bool matchBack =
				    attrTrue(trigger.attributes.value(QStringLiteral("match_back_colour")));
				const bool matchBold   = attrTrue(trigger.attributes.value(QStringLiteral("match_bold")));
				const bool matchItalic = attrTrue(trigger.attributes.value(QStringLiteral("match_italic")));
				const bool matchUnderline =
				    attrTrue(trigger.attributes.value(QStringLiteral("match_underline")));
				const bool matchInverse = attrTrue(trigger.attributes.value(QStringLiteral("match_inverse")));

				if (matchTextColour)
				{
					if (const int expected = trigger.attributes.value(QStringLiteral("text_colour")).toInt();
					    colourIndexFor(fore) != expected)
					{
						continue;
					}
				}
				if (matchBack)
				{
					if (const int expected = trigger.attributes.value(QStringLiteral("back_colour")).toInt();
					    colourIndexFor(back) != expected)
					{
						continue;
					}
				}
				if (matchBold)
				{
					if (const bool desired = attrTrue(trigger.attributes.value(QStringLiteral("bold")));
					    bold != desired)
					{
						continue;
					}
				}
				if (matchItalic)
				{
					if (const bool desired = attrTrue(trigger.attributes.value(QStringLiteral("italic")));
					    italic != desired)
					{
						continue;
					}
				}
				if (matchUnderline)
				{
					if (const bool desired = attrTrue(trigger.attributes.value(QStringLiteral("underline")));
					    underline != desired)
					{
						continue;
					}
				}
				if (matchInverse)
				{
					if (const bool desired = attrTrue(trigger.attributes.value(QStringLiteral("inverse")));
					    inverse != desired)
					{
						continue;
					}
				}
			}

			trigger.matched++;
			trigger.lastMatched = QDateTime::currentDateTime();
			m_runtime->incrementTriggersMatched();
			if (const QString triggerSound = trigger.attributes.value(QStringLiteral("sound")).trimmed();
			    !triggerSound.isEmpty() &&
			    triggerSound.compare(QStringLiteral("(No sound)"), Qt::CaseInsensitive) != 0 &&
			    attrTrue(m_runtime->worldAttributes().value(QStringLiteral("enable_trigger_sounds"))))
			{
				const bool soundIfInactive =
				    attrTrue(trigger.attributes.value(QStringLiteral("sound_if_inactive")));
				if (!soundIfInactive || !m_runtime->isActive())
					m_runtime->playSound(0, triggerSound, false, 0.0, 0.0);
			}

			if (attrTrue(trigger.attributes.value(QStringLiteral("one_shot"))))
			{
				const QString label = trigger.attributes.value(QStringLiteral("name")).trimmed();
				const QString key =
				    label.isEmpty() ? trigger.attributes.value(QStringLiteral("match")).trimmed() : label;
				if (!key.isEmpty())
					result.oneShotTriggers.push_back(qMakePair(plugin, key));
			}

			const QString label = trigger.attributes.value(QStringLiteral("name")).trimmed();
			const QString scriptLabel =
			    label.isEmpty() ? trigger.attributes.value(QStringLiteral("match")).trimmed() : label;

			const int  sendToValue = trigger.attributes.value(QStringLiteral("send_to")).toInt();
			const bool lowerWildcards =
			    attrTrue(trigger.attributes.value(QStringLiteral("lowercase_wildcard")));
			QStringList fixedWildcards = wildcards;
			for (QString &fixed : fixedWildcards)
				fixed = fixWildcard(fixed, lowerWildcards, sendToValue, language);
			QMap<QString, QString> fixedNamed = namedWildcards;
			for (auto it = fixedNamed.begin(); it != fixedNamed.end(); ++it)
				it.value() = fixWildcard(it.value(), lowerWildcards, sendToValue, language);

			if (m_runtime && !scriptLabel.isEmpty())
			{
				if (plugin)
					m_runtime->setPluginTriggerWildcards(plugin->attributes.value(QStringLiteral("id")),
					                                     scriptLabel, fixedWildcards, fixedNamed);
				else
					m_runtime->setTriggerWildcards(scriptLabel, fixedWildcards, fixedNamed);
			}

			if (const int clipboardArg = trigger.attributes.value(QStringLiteral("clipboard_arg")).toInt();
			    clipboardArg > 0 && clipboardArg < fixedWildcards.size())
			{
				if (QClipboard *clipboard = QGuiApplication::clipboard())
					clipboard->setText(fixedWildcards.at(clipboardArg));
			}

			QString sendText = trigger.children.value(QStringLiteral("send"));
			sendText = fixSendText(fixupEscapeSequences(sendText), sendToValue, wildcards, namedWildcards,
			                       language, lowerWildcards,
			                       attrTrue(trigger.attributes.value(QStringLiteral("expand_variables"))),
			                       true, false, false, false, scriptLabel, plugin, nullptr);

			const bool omitFromLog = attrTrue(trigger.attributes.value(QStringLiteral("omit_from_log")));
			const bool omitFromOutput =
			    attrTrue(trigger.attributes.value(QStringLiteral("omit_from_output")));

			if (!multiLine)
			{
				if (omitFromLog)
					result.omitFromLog = true;
				if (omitFromOutput)
					result.omitFromOutput = true;
			}

			if (sendToValue == eSendToScriptAfterOmit)
			{
				DeferredScript deferred;
				deferred.plugin      = plugin;
				deferred.scriptText  = sendText;
				deferred.description = QStringLiteral("Trigger: %1").arg(scriptLabel);
				result.deferredScripts.push_back(deferred);
			}
			else if (sendToValue == eSendToOutput)
			{
				if (!sendText.isEmpty())
				{
					result.extraOutput += sendText;
					if (!sendText.endsWith(kEndLine))
						result.extraOutput += kEndLine;
				}
			}
			else
			{
				trigger.executingScript             = true;
				unsigned short previousActionSource = WorldRuntime::eUnknownActionSource;
				if (m_runtime)
				{
					previousActionSource = m_runtime->currentActionSource();
					m_runtime->setCurrentActionSource(WorldRuntime::eTriggerFired);
				}
				sendTo(sendToValue, sendText, omitFromOutput, omitFromLog,
				       trigger.attributes.value(QStringLiteral("variable")),
				       QStringLiteral("Trigger: %1").arg(scriptLabel), plugin);
				if (m_runtime)
					m_runtime->setCurrentActionSource(previousActionSource);
				trigger.executingScript = false;
			}

			if (const QString scriptName = trigger.attributes.value(QStringLiteral("script"));
			    !scriptName.isEmpty())
			{
				TriggerScript script;
				script.plugin         = plugin;
				script.index          = index;
				script.label          = scriptLabel;
				script.line           = line;
				script.wildcards      = wildcards;
				script.namedWildcards = namedWildcards;
				result.triggerScripts.push_back(script);
			}

			const int colourChange =
			    triggerColourFromCustom(trigger.attributes.value(QStringLiteral("custom_colour")).toInt());
			const bool makeBold      = attrTrue(trigger.attributes.value(QStringLiteral("make_bold")));
			const bool makeItalic    = attrTrue(trigger.attributes.value(QStringLiteral("make_italic")));
			const bool makeUnderline = attrTrue(trigger.attributes.value(QStringLiteral("make_underline")));
			const int  changeType    = trigger.attributes.value(QStringLiteral("colour_change_type")).toInt();

			if (!multiLine && (colourChange >= 0 || makeBold || makeItalic || makeUnderline))
			{
				if (workingSpans.isEmpty())
				{
					ensurePaletteReady();
					workingSpans = ensureSpansForLine(line, spans, defaultFore, defaultBack);
				}
				spansChanged = true;

				auto applyColour = [&](const int sCol, const int eCol)
				{
					QColor newFore;
					QColor newBack;
					bool   changeFore = false;
					bool   changeBack = false;
					if (colourChange >= 0)
					{
						ensurePaletteReady();
						if (colourChange == OTHER_CUSTOM)
						{
							newFore = parseColorValue(
							    trigger.attributes.value(QStringLiteral("other_text_colour")));
							newBack = parseColorValue(
							    trigger.attributes.value(QStringLiteral("other_back_colour")));
						}
						else if (colourChange < palette.customText.size())
						{
							newFore = palette.customText.at(colourChange);
							newBack = palette.customBack.at(colourChange);
						}
						if (changeType == TRIGGER_COLOUR_CHANGE_BOTH ||
						    changeType == TRIGGER_COLOUR_CHANGE_FOREGROUND)
							changeFore = true;
						if (changeType == TRIGGER_COLOUR_CHANGE_BOTH ||
						    changeType == TRIGGER_COLOUR_CHANGE_BACKGROUND)
							changeBack = true;
					}
					applyStyleToSpans(workingSpans, sCol, eCol, newFore, newBack, changeFore, changeBack,
					                  makeBold, makeItalic, makeUnderline);
				};

				applyColour(startCol, endCol);

				if (isRegexp && attrTrue(trigger.attributes.value(QStringLiteral("repeat"))))
				{
					int offset = endCol;
					while (regexMatch(pattern, target, ignoreCase, wildcards, namedWildcards, &startCol,
					                  &endCol, offset, multiLine))
					{
						if (endCol <= offset)
							break;
						applyColour(startCol, endCol);
						offset = endCol;
					}
				}
			}

			if (!attrTrue(trigger.attributes.value(QStringLiteral("keep_evaluating"))))
				break;
		}
	};

	QList<WorldRuntime::Plugin> &pluginList    = m_runtime->pluginsMutable();
	const QVector<int>          &pluginIndices = sortedPluginIndices();
	for (int pluginIndex : pluginIndices)
	{
		if (pluginIndex < 0 || pluginIndex >= pluginList.size())
			continue;
		WorldRuntime::Plugin *plugin = &pluginList[pluginIndex];
		if (!plugin || !plugin->enabled || plugin->installPending || plugin->sequence >= 0)
			continue;
		m_runtime->setStopTriggerEvaluation(WorldRuntime::KeepEvaluating);
		processSequence(plugin->triggers, plugin);
		if (m_runtime->stopTriggerEvaluation() == WorldRuntime::StopAllSequences)
			break;
	}

	if (m_runtime->stopTriggerEvaluation() != WorldRuntime::StopAllSequences)
	{
		m_runtime->setStopTriggerEvaluation(WorldRuntime::KeepEvaluating);
		processSequence(m_runtime->triggersMutable(), nullptr);
	}

	if (m_runtime->stopTriggerEvaluation() != WorldRuntime::StopAllSequences)
	{
		for (int pluginIndex : pluginIndices)
		{
			if (pluginIndex < 0 || pluginIndex >= pluginList.size())
				continue;
			WorldRuntime::Plugin *plugin = &pluginList[pluginIndex];
			if (!plugin || !plugin->enabled || plugin->installPending || plugin->sequence < 0)
				continue;
			m_runtime->setStopTriggerEvaluation(WorldRuntime::KeepEvaluating);
			processSequence(plugin->triggers, plugin);
			if (m_runtime->stopTriggerEvaluation() == WorldRuntime::StopAllSequences)
				break;
		}
	}

	if (spansChanged)
		result.spans = workingSpans;
	else
		result.spans = spans;

	m_runtime->setLineOmittedFromOutput(result.omitFromOutput);
	m_runtime->addTriggerTimeNs(timer.nsecsElapsed());
	return result;
}

void WorldCommandProcessor::checkTimers()
{
	processQueuedCommands(false);

	if (!m_runtime)
		return;
	if (!isEnabledValue(m_runtime->worldAttributes().value(QStringLiteral("enable_timers"))))
		return;

	const QDateTime now       = QDateTime::currentDateTime();
	const bool      connected = m_runtime->connectPhase() == WorldRuntime::eConnectConnectedToMud;

	auto processTimers = [&](QList<WorldRuntime::Timer> &timers, const WorldRuntime::Plugin *plugin)
	{
		struct TimerKey
		{
				QString name;
		};
		QVector<TimerKey> fired;

		for (int i = 0, size = safeQSizeToInt(timers.size()); i < size; ++i)
		{
			WorldRuntime::Timer &timer = timers[i];
			if (!QMudTimerScheduling::isTimerDue(timer, now, connected))
				continue;
			fired.push_back({timer.attributes.value(QStringLiteral("name")).trimmed()});
		}

		auto findTimerByName = [&](const QString &name) -> int
		{
			const QString normalized = name.trimmed().toLower();
			for (int i = 0, size = safeQSizeToInt(timers.size()); i < size; ++i)
			{
				if (timers.at(i).attributes.value(QStringLiteral("name")).trimmed().toLower() == normalized)
					return i;
			}
			return -1;
		};

		for (const auto &[timerName] : fired)
		{
			const int timerIndex = findTimerByName(timerName);
			if (timerIndex < 0)
				continue;
			WorldRuntime::Timer &timer = timers[timerIndex];
			if (!isEnabledValue(timer.attributes.value(QStringLiteral("enabled"))))
				continue;

			QMudTimerScheduling::applyTimerFiredState(timer, now);
			m_runtime->incrementTimersFired();

			const int  sendToValue = timer.attributes.value(QStringLiteral("send_to")).toInt();
			const bool omitFromOutput =
			    isEnabledValue(timer.attributes.value(QStringLiteral("omit_from_output")));
			const bool omitFromLog = isEnabledValue(timer.attributes.value(QStringLiteral("omit_from_log")));
			const QString variableName = timer.attributes.value(QStringLiteral("variable"));
			const QString label        = timer.attributes.value(QStringLiteral("name")).trimmed();
			const QString sendText     = timer.children.value(QStringLiteral("send"));

			timer.executingScript = true;
			if (m_runtime)
				m_runtime->setCurrentActionSource(WorldRuntime::eTimerFired);
			sendTo(sendToValue, sendText, omitFromOutput, omitFromLog, variableName,
			       QStringLiteral("Timer: %1").arg(label), plugin);
			if (m_runtime)
				m_runtime->setCurrentActionSource(WorldRuntime::eUnknownActionSource);
			timer.executingScript = false;

			if (const QString scriptName = timer.attributes.value(QStringLiteral("script"));
			    !scriptName.isEmpty())
			{
				LuaCallbackEngine *lua = plugin ? plugin->lua.data() : m_runtime->luaCallbacks();
				if (!plugin && !canExecuteWorldScript(QStringLiteral("Timer"), scriptName, lua, true))
					continue;
				if (lua)
				{
					timer.executingScript = true;
					if (m_runtime)
						m_runtime->setCurrentActionSource(WorldRuntime::eTimerFired);
					lua->callFunctionWithStringsAndWildcards(scriptName, {label}, QStringList(),
					                                         QMap<QString, QString>(), nullptr);
					if (m_runtime)
						m_runtime->setCurrentActionSource(WorldRuntime::eUnknownActionSource);
					timer.executingScript = false;
					timer.invocationCount++;
				}
			}
		}
	};

	processTimers(m_runtime->timersMutable(), nullptr);
	for (auto &plugin : m_runtime->pluginsMutable())
	{
		if (!plugin.enabled || plugin.installPending)
			continue;
		processTimers(plugin.timers, &plugin);
	}
}

void WorldCommandProcessor::sendTo(const int sendTo, const QString &text, const bool omitFromOutput,
                                   const bool omitFromLog, const QString &variableName,
                                   const QString &description, const WorldRuntime::Plugin *plugin)
{
	const bool canSendEmpty = sendTo == eSendToNotepad || sendTo == eAppendToNotepad ||
	                          sendTo == eReplaceNotepad || sendTo == eSendToOutput ||
	                          sendTo == eSendToLogFile || sendTo == eSendToVariable;

	if (text.isEmpty() && !canSendEmpty)
		return;

	const QMap<QString, QString> &attrs = m_runtime ? m_runtime->worldAttributes() : QMap<QString, QString>();
	const auto                    isEnabled = [](const QString &value)
	{
		return value == QStringLiteral("1") || value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
		       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
	};
	const bool echoInput = isEnabled(attrs.value(QStringLiteral("display_my_input")));
	const bool logInput  = isEnabled(attrs.value(QStringLiteral("log_input")));
	const bool logIt     = logInput && !omitFromLog;
	const bool fromTimer = m_runtime && m_runtime->currentActionSource() == WorldRuntime::eTimerFired;
	const bool echoSend  = omitFromOutput ? false : (fromTimer ? true : echoInput);

	switch (sendTo)
	{
	case eSendToWorld:
		sendMsg(text, echoSend, false, logIt);
		break;
	case eSendToCommandQueue:
		sendMsg(text, echoSend, true, logIt);
		break;
	case eSendToSpeedwalk:
	{
		if (const QString evaluated = doEvaluateSpeedwalk(text);
		    !evaluated.isEmpty() && evaluated.at(0) != QLatin1Char('*'))
		{
			sendMsg(evaluated, echoSend, true, logIt);
		}
	}
	break;
	case eSendImmediate:
		if (m_runtime)
			m_runtime->setLastImmediateExpression(text);
		doSendMsg(text, echoSend, logIt);
		break;
	case eSendToCommand:
		if (m_view && m_view->inputText().isEmpty())
			m_view->setInputText(text);
		break;
	case eSendToOutput:
		if (m_view)
			m_view->appendNoteText(text, true);
		break;
	case eSendToStatus:
		if (m_view)
		{
			QString status = text;
			if (const qsizetype newline = status.indexOf(kEndLine); newline >= 0)
				status = status.left(newline);
			if (MainWindowHost *main = resolveMainWindowHost(m_view->window()))
				main->setStatusMessageNow(status);
		}
		break;
	case eSendToNotepad:
		if (m_view)
		{
			if (MainWindowHost *main = resolveMainWindowHost(m_view->window()))
				main->sendToNotepad(description, text + kEndLine, nullptr);
		}
		break;
	case eAppendToNotepad:
		if (m_view)
		{
			if (MainWindowHost *main = resolveMainWindowHost(m_view->window()))
				main->appendToNotepad(description, text + kEndLine, false, nullptr);
		}
		break;
	case eReplaceNotepad:
		if (m_view)
		{
			if (MainWindowHost *main = resolveMainWindowHost(m_view->window()))
				main->appendToNotepad(description, text + kEndLine, true, nullptr);
		}
		break;
	case eSendToExecute:
	{
		const bool saved = m_suppressInputLog;
		if (omitFromLog)
			m_suppressInputLog = true;
		executeCommand(text);
		m_suppressInputLog = saved;
	}
	break;
	case eSendToVariable:
		if (!variableName.isEmpty() && m_runtime)
			m_runtime->setVariable(variableName, text);
		break;
	case eSendToLogFile:
		if (m_runtime && m_runtime->isLogOpen() && !isEnabled(attrs.value(QStringLiteral("log_raw"))))
		{
			m_runtime->writeLog(text);
			m_runtime->writeLog(QStringLiteral("\n"));
		}
		break;
	case eSendToScript:
	case eSendToScriptAfterOmit:
	{
		LuaCallbackEngine *lua = nullptr;
		if (plugin)
		{
			lua = plugin->lua.data();
		}
		else if (m_runtime)
		{
			const QString enableScripts = attrs.value(QStringLiteral("enable_scripts"));
			const QString language      = attrs.value(QStringLiteral("script_language"));
			const bool    scriptingEnabled =
			    isEnabled(enableScripts) && language.compare(QStringLiteral("Lua"), Qt::CaseInsensitive) == 0;
			if (!scriptingEnabled)
			{
				if (const QString warn = attrs.value(QStringLiteral("warn_if_scripting_inactive"));
				    isEnabled(warn))
				{
					const QString name = description.isEmpty() ? QStringLiteral("(unnamed)") : description;
					m_runtime->outputText(
					    QStringLiteral(
					        "Script function \"%1\" cannot execute - scripting disabled/parse error.")
					        .arg(name),
					    true, true);
				}
				break;
			}
			lua = m_runtime->luaCallbacks();
			if (!lua)
			{
				if (const QString warn = attrs.value(QStringLiteral("warn_if_scripting_inactive"));
				    isEnabled(warn))
				{
					const QString name = description.isEmpty() ? QStringLiteral("(unnamed)") : description;
					m_runtime->outputText(
					    QStringLiteral(
					        "Script function \"%1\" cannot execute - scripting disabled/parse error.")
					        .arg(name),
					    true, true);
				}
				break;
			}
		}
		if (lua)
			lua->executeScript(text, description, nullptr);
	}
	break;
	default:
		break;
	}
}

void WorldCommandProcessor::sendMsg(const QString &text, const bool echo, const bool queueIt, bool logIt)
{
	if (!ensureConnectedForSend())
	{
		if (m_processingEnteredCommand)
			m_enteredCommandSendFailed = true;
		return;
	}

	QString strText = text;

	// cannot change what we are sending in OnPluginSent
	if (m_pluginProcessingSent)
		return;

	bool bEcho = echo;
	if (m_suppressInputLog)
		logIt = false;

	if (m_view && m_runtime)
	{
		const QMap<QString, QString> &attrs     = m_runtime->worldAttributes();
		const auto                    isEnabled = [](const QString &value)
		{
			return value == QStringLiteral("1") ||
			       value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
			       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
		};
		if (isEnabled(attrs.value(QStringLiteral("unpause_on_send"))) && m_view->isFrozen())
			m_view->setFrozen(false);
	}

	// test to see if world has suppressed echoing

	if (m_noEcho)
		bEcho = false;

	// strip trailing endline - that would trigger an extra blank line
	if (strText.endsWith(kEndLine))
		strText.chop(kEndLine.size());

	// fix up German umlauts
	if (m_translateGerman)
		strText = fixUpGerman(strText);

	// to make sure each individual line ends up on the output window marked as user
	// input (and in the right color) break up the string into individual lines
	QStringList strList = strText.split(kEndLine);

	// if list is empty, make sure we send at least one empty line
	if (strList.isEmpty())
		strList.append(QString());

	for (const QString &strLine : strList)
	{
		// it needs to be queued if queuing is requested
		// it also needs to be queued regardless if there is already something in the queue

		if (QMudCommandQueue::shouldQueueCommand(m_speedWalkDelay, queueIt, !m_queuedCommands.isEmpty()))
		{
			m_queuedCommands.append(QMudCommandQueue::encodeQueueEntry(strLine, queueIt, bEcho, logIt));

		} // end of having a speedwalk delay
		else
			doSendMsg(strLine, bEcho, logIt); // just send it
	} // end of breaking it into lines

	if (!m_queuedCommands.isEmpty())
		updateQueuedCommandsStatusLine();
	if (m_runtime)
		m_runtime->setQueuedCommandCount(safeQSizeToInt(m_queuedCommands.size()));
}

void WorldCommandProcessor::doSendMsg(const QString &text, const bool echo, const bool logIt)
{
	if (!ensureConnectedForSend())
	{
		if (m_processingEnteredCommand)
			m_enteredCommandSendFailed = true;
		return;
	}

	QString str = text;

	// cannot change what we are sending in OnPluginSent
	if (m_pluginProcessingSent)
		return;

	// append an end-of-line if there isn't one already

	if (!str.endsWith(kEndLine))
		str += kEndLine;

	// "OnPluginSend" - script can cancel send

	if (!m_pluginProcessingSend)
	{
		m_pluginProcessingSend = true; // so we don't go into a loop
		QString pluginText     = str;
		if (pluginText.endsWith(kEndLine))
			pluginText.chop(kEndLine.size());
		if (m_runtime && !m_runtime->firePluginSend(pluginText))
		{
			m_pluginProcessingSend = false;
			return;
		}
		m_pluginProcessingSend = false;
	}

	// count number of times we sent this

	if (str == m_lastCommandSent)
		m_lastCommandCount++; // same one - count them
	else
	{
		m_lastCommandSent  = str; // new one - remember it
		m_lastCommandCount = 1;
	}

	if (m_enableSpamPrevention && // provided they want it
	    m_spamLineCount > 2 &&    // otherwise we might loop
	    !m_spamMessage.isEmpty()) // not much point without something to send
	{
		if (m_lastCommandCount > m_spamLineCount)
		{
			m_lastCommandCount = 0;                // so we don't recurse again
			doSendMsg(m_spamMessage, echo, logIt); // recursive call
			m_lastCommandSent  = str;              // remember it for next time
			m_lastCommandCount = 1;
		} // end of time to do it

	} // end of spam prevention active

	// "OnPluginSent" - we are definitely sending this
	// See: http://www.gammon.com.au/forum/bbshowpost.php?bbsubject_id=7244

	if (!m_pluginProcessingSent)
	{
		m_pluginProcessingSent = true; // so we don't go into a loop
		QString pluginText     = str;
		if (pluginText.endsWith(kEndLine))
			pluginText.chop(kEndLine.size());
		if (m_runtime)
			m_runtime->firePluginSent(pluginText);
		m_pluginProcessingSent = false;
	}

	// echo sent text if required

	if (echo && m_view)
	{
		m_view->echoInputText(str);
		if (m_runtime)
		{
			QString screenText = str;
			if (screenText.endsWith(kEndLine))
				screenText.chop(kEndLine.size());
			m_runtime->firePluginScreendraw(2, logIt ? 1 : 0, screenText);
		}
	}

	// log sent text if required

	if (logIt)
	{
		QString logText = str;
		if (logText.endsWith(kEndLine))
			logText.chop(kEndLine.size());
		logLine(logText, QStringLiteral("log_line_preamble_input"),
		        QStringLiteral("log_line_postamble_input"));
	}

	// add to mapper if required
	if (m_runtime && m_runtime->isMapping())
	{
		QString direction = str.trimmed().toLower();
		if (direction.endsWith(kEndLine))
			direction.chop(kEndLine.size());
		direction = direction.trimmed();

		const AppController *app = AppController::instance();
		if (const QString mapped = app ? app->mapDirectionToLog(direction) : QString(); !mapped.isEmpty())
		{
			const QString reverse = app ? app->mapDirectionReverse(direction) : QString();

			auto          appendMapped = [&] { m_runtime->addToMapper(mapped, reverse); };

			if (m_runtime->removeMapReverses() && m_runtime->mappingCount() > 0)
			{
				QString heldComment;
				QString last = m_runtime->mappingItem(m_runtime->mappingCount() - 1);
				if (last.size() >= 2 && last.startsWith(QLatin1Char('{')) &&
				    last.endsWith(QLatin1Char('}')) && m_runtime->mappingCount() > 1)
				{
					heldComment = last.mid(1, last.size() - 2);
					m_runtime->deleteLastMapItem();
					last = m_runtime->mappingItem(m_runtime->mappingCount() - 1);
				}

				bool collapsed = false;
				if (const qsizetype slashPos = last.indexOf(QLatin1Char('/')); slashPos != -1)
				{
					const QString lastReverse = last.mid(slashPos + 1).trimmed().toLower();
					if (const QString lastReverseMapped =
					        app ? app->mapDirectionToLog(lastReverse) : QString();
					    !lastReverseMapped.isEmpty())
					{
						if (lastReverseMapped == mapped)
						{
							m_runtime->deleteLastMapItem();
							collapsed = true;
						}
					}
				}
				else if (last.trimmed().toLower() == reverse)
				{
					m_runtime->deleteLastMapItem();
					collapsed = true;
				}

				if (!collapsed)
				{
					if (!heldComment.isEmpty())
						m_runtime->addMapperComment(heldComment);
					appendMapped();
				}
			}
			else
			{
				appendMapped();
			}
		}
	}

	// for MXP debugging
#ifdef SHOW_ALL_COMMS
	// Optional Debug_MUD hook is currently disabled in the Qt build.
#endif

	// line accounting is maintained through incrementLinesSent().

	// send it

	if (m_runtime)
	{
		QByteArray payload = m_utf8 ? str.toUtf8() : str.toLocal8Bit();
		if (!m_doNotTranslateIac)
			payload.replace(static_cast<char>(0xFF), QByteArray("\xFF\xFF", 2));
		m_runtime->sendToWorld(payload);
		if (m_processingEnteredCommand)
			m_enteredCommandSendFailed = false;
		m_runtime->incrementLinesSent();
		QString commandText = str;
		if (commandText.endsWith(kEndLine))
			commandText.chop(kEndLine.size());
		m_runtime->setLastCommandSent(commandText);
	}
}

bool WorldCommandProcessor::ensureConnectedForSend() const
{
	if (!m_runtime)
		return false;

	// Connected is always sendable.
	if (m_runtime->isConnected())
		return true;

	const int phase = m_runtime->connectPhase();
	if (phase == WorldRuntime::eConnectDisconnecting)
		return false;

	const unsigned short source = m_runtime->currentActionSource();
	const bool           interactiveSource =
	    source == WorldRuntime::eUserTyping || source == WorldRuntime::eUserMacro ||
	    source == WorldRuntime::eUserKeypad || source == WorldRuntime::eUserAccelerator ||
	    source == WorldRuntime::eUserMenuAction;

	const QMap<QString, QString> &attrs     = m_runtime->worldAttributes();
	const QString                 host      = attrs.value(QStringLiteral("site")).trimmed();
	bool                          portOk    = false;
	const int                     portValue = attrs.value(QStringLiteral("port")).toInt(&portOk);
	const quint16 port       = portOk && portValue > 0 && portValue <= 65535 ? static_cast<quint16>(portValue)
	                                                                         : static_cast<quint16>(0);
	const QString worldLabel = attrs.value(QStringLiteral("name")).trimmed().isEmpty()
	                               ? (host.isEmpty() ? QStringLiteral("the world") : host)
	                               : attrs.value(QStringLiteral("name")).trimmed();

	// Legacy behavior: sentinel host means "do nothing silently".
	if (host == QStringLiteral("0.0.0.0"))
		return false;

	// Non-interactive sends (Lua API, trigger/timer/plugin paths) should fail silently.
	if (!interactiveSource)
		return false;

	if (phase != WorldRuntime::eConnectNotConnected)
	{
		QMessageBox::information(
		    m_view, QStringLiteral("QMud"),
		    QStringLiteral("The connection to %1 is currently being established.").arg(worldLabel));
		return false;
	}

	const QMessageBox::StandardButton answer = QMessageBox::question(
	    m_view, QStringLiteral("QMud"),
	    QStringLiteral("The connection to %1 is not open. Attempt to reconnect?").arg(worldLabel),
	    QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

	if (answer != QMessageBox::Yes)
		return false;

	if (host.isEmpty() || port == 0)
	{
		QMessageBox::warning(m_view, QStringLiteral("QMud"),
		                     QStringLiteral("Cannot connect: host/port not specified."));
		return false;
	}

	m_runtime->connectToWorld(host, port);
	return false;
}

void WorldCommandProcessor::processQueuedCommands(bool flushAll)
{
	if (m_queuedCommands.isEmpty())
	{
		updateQueuedCommandsStatusLine();
		return;
	}

	if (!flushAll)
	{
		if (m_speedWalkDelay <= 0)
			flushAll = true;
		else
		{
			if (!m_queueDispatchTimer.isValid())
				m_queueDispatchTimer.start();
			else if (m_queueDispatchTimer.elapsed() < m_speedWalkDelay)
				return;
		}
	}

	bool              sentAny = false;
	const QStringList batch   = QMudCommandQueue::takeDispatchBatch(m_queuedCommands, flushAll);
	for (const QString &queued : batch)
	{
		const QMudCommandQueue::QueueEntry decoded = QMudCommandQueue::decodeQueueEntry(queued);
		doSendMsg(decoded.payload, decoded.withEcho, decoded.logIt);
		sentAny = true;
	}

	if (sentAny && !flushAll)
		m_queueDispatchTimer.restart();
	if (flushAll && m_queueDispatchTimer.isValid())
		m_queueDispatchTimer.invalidate();

	if (m_runtime)
		m_runtime->setQueuedCommandCount(safeQSizeToInt(m_queuedCommands.size()));
	updateQueuedCommandsStatusLine();
}

void WorldCommandProcessor::updateQueuedCommandsStatusLine()
{
	if (!m_view)
		return;
	MainWindowHost *main = resolveMainWindowHost(m_view->window());
	if (!main)
		return;

	if (m_queuedCommands.isEmpty())
	{
		main->setStatusNormal();
		return;
	}

	auto    queued = QStringLiteral("Queued: ");
	QString last;
	int     count    = 0;
	bool    overflow = false;

	auto    appendLast = [&]
	{
		if (last.isEmpty())
			return;
		QString out = last;
		if (out.size() > 1)
			out = QStringLiteral("(%1)").arg(out);
		if (count == 1)
			queued += out + QLatin1Char(' ');
		else if (count > 1)
			queued += QString::number(count) + out + QLatin1Char(' ');
	};

	for (const QString &item : m_queuedCommands)
	{
		if (constexpr int kMaxShown = 50; queued.size() >= kMaxShown)
		{
			overflow = true;
			break;
		}

		const QString direction = item.mid(1).trimmed();
		if (direction.isEmpty())
			continue;

		if (direction == last)
		{
			++count;
			continue;
		}

		appendLast();
		last  = direction;
		count = 1;
	}

	appendLast();
	if (overflow)
		queued += QStringLiteral("...");

	main->setStatusMessageNow(queued);
}

void WorldCommandProcessor::logLine(const QString &text, const QString &preambleKey,
                                    const QString &postambleKey) const
{
	if (!m_runtime)
		return;

	const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
	const QMap<QString, QString> &multi = m_runtime->worldMultilineAttributes();

	const auto                    isEnabled = [](const QString &value)
	{
		return value == QStringLiteral("1") || value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
		       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
	};

	if (!m_runtime->isLogOpen())
		return;

	if (const bool logRaw = isEnabled(attrs.value(QStringLiteral("log_raw"))); logRaw)
		return;

	if (preambleKey == QStringLiteral("log_line_preamble_input") &&
	    !isEnabled(attrs.value(QStringLiteral("log_input"))))
		return;
	if (preambleKey == QStringLiteral("log_line_preamble_notes") &&
	    !isEnabled(attrs.value(QStringLiteral("log_notes"))))
		return;

	const bool logHtml  = isEnabled(attrs.value(QStringLiteral("log_html")));
	QString    preamble = multi.value(preambleKey);
	if (preamble.isEmpty())
		preamble = attrs.value(preambleKey);
	QString postamble = multi.value(postambleKey);
	if (postamble.isEmpty())
		postamble = attrs.value(postambleKey);

	const QDateTime now = QDateTime::currentDateTime();
	if (!preamble.isEmpty())
	{
		preamble.replace(QStringLiteral("%n"), QStringLiteral("\n"));
		preamble = m_runtime->formatTime(now, preamble, logHtml);
		m_runtime->writeLog(preamble);
	}

	if (logHtml)
	{
		const bool inputLine   = preambleKey == QStringLiteral("log_line_preamble_input");
		const bool logInColour = isEnabled(attrs.value(QStringLiteral("log_in_colour")));
		bool       wrapped     = false;
		if (inputLine && logInColour)
		{
			bool ok = false;
			if (const int echoColour = attrs.value(QStringLiteral("echo_colour")).toInt(&ok);
			    ok && echoColour >= 1 && echoColour <= MAX_CUSTOM)
			{
				const long colour = m_runtime->customColourText(echoColour);
				const int  r      = static_cast<int>(colour & 0xFF);
				const int  g      = static_cast<int>(colour >> 8 & 0xFF);
				const int  b      = static_cast<int>(colour >> 16 & 0xFF);
				m_runtime->writeLog(QStringLiteral("<font color=\"#%1%2%3\">")
				                        .arg(r, 2, 16, QLatin1Char('0'))
				                        .arg(g, 2, 16, QLatin1Char('0'))
				                        .arg(b, 2, 16, QLatin1Char('0'))
				                        .toUpper());
				wrapped = true;
			}
		}
		m_runtime->writeLog(fixHtmlString(text));
		if (wrapped)
			m_runtime->writeLog(QStringLiteral("</font>"));
	}
	else
		m_runtime->writeLog(text);

	if (!postamble.isEmpty())
	{
		postamble.replace(QStringLiteral("%n"), QStringLiteral("\n"));
		postamble = m_runtime->formatTime(now, postamble, logHtml);
		m_runtime->writeLog(postamble);
	}

	m_runtime->writeLog(QStringLiteral("\n"));
}
