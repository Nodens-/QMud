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
#include "FileExtensions.h"
#include "HyperlinkActionUtils.h"
#include "MainWindowHost.h"
#include "MainWindowHostResolver.h"
#include "NativePluginRegistry.h"
#include "SpeedwalkParser.h"
#include "TimerSchedulingUtils.h"
#include "TraceDispatchUtils.h"
#include "WorldCommandProcessorUtils.h"
#include "WorldOptions.h"
#include "WorldRuleEnableUtils.h"
#include "WorldRuntime.h"
#include "WorldView.h"
#include "helpers/EncodingUtils.h"
#include "helpers/OutputWrapUtils.h"
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
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QUrl>
#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
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
			run.text    = line.mid(pos, span.length);
			QColor fore = span.fore.isValid() ? span.fore : defaultFore;
			QColor back = span.back.isValid() ? span.back : defaultBack;
			if (span.inverse)
				qSwap(fore, back);
			run.textColour = toRgbNumber(fore);
			run.backColour = toRgbNumber(back);
			int style      = 0;
			if (span.bold)
				style |= kStyleHilite;
			if (span.underline)
				style |= kStyleUnderline;
			if (span.blink || span.italic)
				style |= kStyleBlink;
			run.style = style;
			if (!runs.isEmpty())
			{
				LuaStyleRun &previous = runs.last();
				if (previous.textColour == run.textColour && previous.backColour == run.backColour &&
				    previous.style == run.style)
				{
					previous.text += run.text;
					pos += span.length;
					continue;
				}
			}
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

	quint64 paletteSignature(const QMap<QString, QString> &attrs, const QList<WorldRuntime::Colour> &colours)
	{
		quint64    signature = 0x6b8f1d2c4a709e35ULL;
		const auto mixAttr   = [&](const QString &key) { mixSignature(signature, qHash(attrs.value(key))); };
		mixAttr(QStringLiteral("custom_16_is_default_colour"));
		mixAttr(QStringLiteral("output_text_colour"));
		mixAttr(QStringLiteral("output_background_colour"));
		mixSignature(signature, static_cast<quint64>(colours.size()));
		for (const WorldRuntime::Colour &colour : colours)
		{
			mixSignature(signature, qHash(colour.group));
			mixSignature(signature, qHash(colour.attributes.value(QStringLiteral("seq"))));
			mixSignature(signature, qHash(colour.attributes.value(QStringLiteral("rgb"))));
			mixSignature(signature, qHash(colour.attributes.value(QStringLiteral("text"))));
			mixSignature(signature, qHash(colour.attributes.value(QStringLiteral("back"))));
		}
		return signature;
	}

	int triggerColourFromCustom(const int customColour)
	{
		if (customColour <= 0)
			return -1;
		if (customColour == OTHER_CUSTOM + 1)
			return OTHER_CUSTOM;
		if (customColour > OTHER_CUSTOM + 1)
			return -1;
		return customColour - 1;
	}

	bool pluginHasValidId(const WorldRuntime::Plugin &plugin)
	{
		return !plugin.attributes.value(QStringLiteral("id")).trimmed().isEmpty();
	}

	quint64 nextRuleRuntimeId()
	{
		static std::atomic<quint64> idCounter{0};
		quint64                     id = idCounter.fetch_add(1, std::memory_order_relaxed) + 1;
		if (id == 0)
			id = idCounter.fetch_add(1, std::memory_order_relaxed) + 1;
		return id;
	}

	QString pluginIdOf(const WorldRuntime::Plugin *plugin)
	{
		return plugin ? plugin->attributes.value(QStringLiteral("id")) : QString();
	}

	WorldRuntime::Plugin *resolveCapturedPlugin(WorldRuntime *runtime, const QString &pluginId)
	{
		if (!runtime || pluginId.isEmpty())
			return nullptr;
		return runtime->pluginForId(pluginId);
	}

	bool aliasRuntimeIdUsedElsewhere(WorldRuntime *runtime, const quint64 runtimeId,
	                                 const WorldRuntime::Alias *current)
	{
		if (!runtime || runtimeId == 0)
			return false;
		for (WorldRuntime::Alias &alias : runtime->aliasesMutable())
		{
			if (&alias != current && alias.runtimeId == runtimeId)
				return true;
		}
		for (WorldRuntime::Plugin &plugin : runtime->pluginsMutable())
		{
			for (WorldRuntime::Alias &alias : plugin.aliases)
			{
				if (&alias != current && alias.runtimeId == runtimeId)
					return true;
			}
		}
		return false;
	}

	bool triggerRuntimeIdUsedElsewhere(WorldRuntime *runtime, const quint64 runtimeId,
	                                   const WorldRuntime::Trigger *current)
	{
		if (!runtime || runtimeId == 0)
			return false;
		for (WorldRuntime::Trigger &trigger : runtime->triggersMutable())
		{
			if (&trigger != current && trigger.runtimeId == runtimeId)
				return true;
		}
		for (WorldRuntime::Plugin &plugin : runtime->pluginsMutable())
		{
			for (WorldRuntime::Trigger &trigger : plugin.triggers)
			{
				if (&trigger != current && trigger.runtimeId == runtimeId)
					return true;
			}
		}
		return false;
	}

	bool timerRuntimeIdUsedElsewhere(WorldRuntime *runtime, const quint64 runtimeId,
	                                 const WorldRuntime::Timer *current)
	{
		if (!runtime || runtimeId == 0)
			return false;
		for (WorldRuntime::Timer &timer : runtime->timersMutable())
		{
			if (&timer != current && timer.runtimeId == runtimeId)
				return true;
		}
		for (WorldRuntime::Plugin &plugin : runtime->pluginsMutable())
		{
			for (WorldRuntime::Timer &timer : plugin.timers)
			{
				if (&timer != current && timer.runtimeId == runtimeId)
					return true;
			}
		}
		return false;
	}

	quint64 ensureAliasRuntimeId(WorldRuntime *runtime, WorldRuntime::Alias *alias)
	{
		if (!alias)
			return 0;
		if (alias->runtimeId == 0 || aliasRuntimeIdUsedElsewhere(runtime, alias->runtimeId, alias))
			alias->runtimeId = nextRuleRuntimeId();
		return alias->runtimeId;
	}

	quint64 ensureTriggerRuntimeId(WorldRuntime *runtime, WorldRuntime::Trigger *trigger)
	{
		if (!trigger)
			return 0;
		if (trigger->runtimeId == 0 || triggerRuntimeIdUsedElsewhere(runtime, trigger->runtimeId, trigger))
			trigger->runtimeId = nextRuleRuntimeId();
		return trigger->runtimeId;
	}

	quint64 ensureTimerRuntimeId(WorldRuntime *runtime, WorldRuntime::Timer *timer)
	{
		if (!timer)
			return 0;
		if (timer->runtimeId == 0 || timerRuntimeIdUsedElsewhere(runtime, timer->runtimeId, timer))
			timer->runtimeId = nextRuleRuntimeId();
		return timer->runtimeId;
	}

	WorldRuntime::Alias *resolveAliasByRuntimeId(WorldRuntime *runtime, const quint64 runtimeId)
	{
		if (!runtime || runtimeId == 0)
			return nullptr;
		for (WorldRuntime::Alias &alias : runtime->aliasesMutable())
		{
			if (alias.runtimeId == runtimeId)
				return &alias;
		}
		for (WorldRuntime::Plugin &plugin : runtime->pluginsMutable())
		{
			for (WorldRuntime::Alias &alias : plugin.aliases)
			{
				if (alias.runtimeId == runtimeId)
					return &alias;
			}
		}
		return nullptr;
	}

	WorldRuntime::Trigger *resolveTriggerByRuntimeId(WorldRuntime *runtime, const quint64 runtimeId)
	{
		if (!runtime || runtimeId == 0)
			return nullptr;
		for (WorldRuntime::Trigger &trigger : runtime->triggersMutable())
		{
			if (trigger.runtimeId == runtimeId)
				return &trigger;
		}
		for (WorldRuntime::Plugin &plugin : runtime->pluginsMutable())
		{
			for (WorldRuntime::Trigger &trigger : plugin.triggers)
			{
				if (trigger.runtimeId == runtimeId)
					return &trigger;
			}
		}
		return nullptr;
	}

	WorldRuntime::Timer *resolveTimerByRuntimeId(WorldRuntime *runtime, const quint64 runtimeId)
	{
		if (!runtime || runtimeId == 0)
			return nullptr;
		for (WorldRuntime::Timer &timer : runtime->timersMutable())
		{
			if (timer.runtimeId == runtimeId)
				return &timer;
		}
		for (WorldRuntime::Plugin &plugin : runtime->pluginsMutable())
		{
			for (WorldRuntime::Timer &timer : plugin.timers)
			{
				if (timer.runtimeId == runtimeId)
					return &timer;
			}
		}
		return nullptr;
	}

	bool removeAliasByRuntimeId(WorldRuntime *runtime, const quint64 runtimeId)
	{
		if (!runtime || runtimeId == 0)
			return false;
		QList<WorldRuntime::Alias> &worldAliases = runtime->aliasesMutable();
		for (int i = safeQSizeToInt(worldAliases.size()) - 1; i >= 0; --i)
		{
			if (worldAliases.at(i).runtimeId == runtimeId)
			{
				worldAliases.removeAt(i);
				return true;
			}
		}
		for (WorldRuntime::Plugin &plugin : runtime->pluginsMutable())
		{
			for (int i = safeQSizeToInt(plugin.aliases.size()) - 1; i >= 0; --i)
			{
				if (plugin.aliases.at(i).runtimeId == runtimeId)
				{
					plugin.aliases.removeAt(i);
					return true;
				}
			}
		}
		return false;
	}

	bool removeTriggerByRuntimeId(WorldRuntime *runtime, const quint64 runtimeId)
	{
		if (!runtime || runtimeId == 0)
			return false;
		QList<WorldRuntime::Trigger> &worldTriggers = runtime->triggersMutable();
		for (int i = safeQSizeToInt(worldTriggers.size()) - 1; i >= 0; --i)
		{
			if (worldTriggers.at(i).runtimeId == runtimeId)
			{
				worldTriggers.removeAt(i);
				return true;
			}
		}
		for (WorldRuntime::Plugin &plugin : runtime->pluginsMutable())
		{
			for (int i = safeQSizeToInt(plugin.triggers.size()) - 1; i >= 0; --i)
			{
				if (plugin.triggers.at(i).runtimeId == runtimeId)
				{
					plugin.triggers.removeAt(i);
					return true;
				}
			}
		}
		return false;
	}

	class AliasExecutionScope final
	{
		public:
			AliasExecutionScope(WorldRuntime *runtime, WorldRuntime::Alias *alias, const bool countInvocation)
			    : m_runtime(runtime), m_countInvocation(countInvocation)
			{
				if (!alias || !runtime)
					return;
				m_ruleRuntimeId = ensureAliasRuntimeId(runtime, alias);
				alias->executingScriptDepth++;
				alias->executingScript = true;
				m_active               = true;
			}

			~AliasExecutionScope()
			{
				if (!m_active || !m_runtime)
					return;
				if (WorldRuntime::Alias *resolved =
				        resolveAliasByRuntimeId(m_runtime.data(), m_ruleRuntimeId))
				{
					if (resolved->executingScriptDepth > 0)
						resolved->executingScriptDepth--;
					resolved->executingScript = resolved->executingScriptDepth > 0;
					if (m_countInvocation)
						resolved->invocationCount++;
				}
			}

			AliasExecutionScope(const AliasExecutionScope &)            = delete;
			AliasExecutionScope &operator=(const AliasExecutionScope &) = delete;

		private:
			QPointer<WorldRuntime> m_runtime;
			quint64                m_ruleRuntimeId{0};
			bool                   m_countInvocation{false};
			bool                   m_active{false};
	};

	class TriggerExecutionScope final
	{
		public:
			TriggerExecutionScope(WorldRuntime *runtime, WorldRuntime::Trigger *trigger,
			                      const bool countInvocation)
			    : m_runtime(runtime), m_countInvocation(countInvocation)
			{
				if (!trigger || !runtime)
					return;
				m_ruleRuntimeId = ensureTriggerRuntimeId(runtime, trigger);
				trigger->executingScriptDepth++;
				trigger->executingScript = true;
				m_active                 = true;
			}

			~TriggerExecutionScope()
			{
				if (!m_active || !m_runtime)
					return;
				if (WorldRuntime::Trigger *resolved =
				        resolveTriggerByRuntimeId(m_runtime.data(), m_ruleRuntimeId))
				{
					if (resolved->executingScriptDepth > 0)
						resolved->executingScriptDepth--;
					resolved->executingScript = resolved->executingScriptDepth > 0;
					if (m_countInvocation)
						resolved->invocationCount++;
				}
			}

			TriggerExecutionScope(const TriggerExecutionScope &)            = delete;
			TriggerExecutionScope &operator=(const TriggerExecutionScope &) = delete;

		private:
			QPointer<WorldRuntime> m_runtime;
			quint64                m_ruleRuntimeId{0};
			bool                   m_countInvocation{false};
			bool                   m_active{false};
	};

	class TimerExecutionScope final
	{
		public:
			TimerExecutionScope(WorldRuntime *runtime, WorldRuntime::Timer *timer, const bool countInvocation)
			    : m_runtime(runtime), m_countInvocation(countInvocation)
			{
				if (!timer || !runtime)
					return;
				m_ruleRuntimeId = ensureTimerRuntimeId(runtime, timer);
				timer->executingScriptDepth++;
				timer->executingScript = true;
				m_active               = true;
			}

			~TimerExecutionScope()
			{
				if (!m_active || !m_runtime)
					return;
				if (WorldRuntime::Timer *resolved =
				        resolveTimerByRuntimeId(m_runtime.data(), m_ruleRuntimeId))
				{
					if (resolved->executingScriptDepth > 0)
						resolved->executingScriptDepth--;
					resolved->executingScript = resolved->executingScriptDepth > 0;
					if (m_countInvocation)
						resolved->invocationCount++;
				}
			}

			TimerExecutionScope(const TimerExecutionScope &)            = delete;
			TimerExecutionScope &operator=(const TimerExecutionScope &) = delete;

		private:
			QPointer<WorldRuntime> m_runtime;
			quint64                m_ruleRuntimeId{0};
			bool                   m_countInvocation{false};
			bool                   m_active{false};
	};
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
	connect(this, &WorldCommandProcessor::sendToScriptRequested, this,
	        &WorldCommandProcessor::dispatchScriptSend, Qt::DirectConnection);
}

bool WorldCommandProcessor::canExecuteWorldScript(const QString &functionType, const QString &functionName,
                                                  const LuaCallbackEngine *lua) const
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

	return true;
}

void WorldCommandProcessor::warnMissingWorldScriptFunction(const QString &functionType,
                                                           const QString &functionName) const
{
	if (!m_runtime)
		return;
	const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
	if (!isEnabledValue(attrs.value(QStringLiteral("warn_if_scripting_inactive"))))
		return;
	m_runtime->outputText(QStringLiteral("%1 function \"%2\" not found or had a previous error.")
	                          .arg(functionType, functionName),
	                      true, true);
}

void WorldCommandProcessor::setRuntime(WorldRuntime *runtime)
{
	m_runtime = runtime;
	invalidateTriggerEvaluationCache();
	m_aliasOrderCache.clear();
	m_pluginOrderCache = PluginOrderCacheEntry();
	m_regexCache.clear();
	m_wildcardRegexCache.clear();
	m_invalidRegexWarnings.clear();
	m_paletteCacheValid = false;
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
	m_regexpMatchEmpty   = !(matchEmpty == QStringLiteral("0") ||
	                         matchEmpty.compare(QStringLiteral("n"), Qt::CaseInsensitive) == 0 ||
	                         matchEmpty.compare(QStringLiteral("no"), Qt::CaseInsensitive) == 0 ||
	                         matchEmpty.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0);
	const QString utf8   = attrs.value(QStringLiteral("utf_8"));
	m_utf8               = isEnabledValue(utf8);
	m_legacyEncodingName = qmudNormalizeWorldTextEncodingName(attrs.value(QStringLiteral("legacy_encoding")));

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

const WorldCommandProcessor::TriggerEvaluationCacheEntry &
WorldCommandProcessor::decodedTriggerEvaluationCache(const QList<WorldRuntime::Trigger> &triggers)
{
	static const TriggerEvaluationCacheEntry kEmpty;
	if (!m_runtime)
		return kEmpty;

	const quint64 generation = m_runtime->triggerRuleGeneration();
	if (m_triggerEvaluationCacheGeneration != generation)
	{
		m_triggerEvaluationCache.clear();
		m_triggerEvaluationCacheGeneration = generation;
	}

	const auto cacheKey = reinterpret_cast<quintptr>(&triggers);
	const int  count    = safeQSizeToInt(triggers.size());
	if (auto cacheIt = m_triggerEvaluationCache.constFind(cacheKey);
	    cacheIt != m_triggerEvaluationCache.constEnd() && cacheIt->generation == generation &&
	    cacheIt->count == count)
	{
		return cacheIt.value();
	}

	TriggerEvaluationCacheEntry rebuilt;
	rebuilt.generation = generation;
	rebuilt.count      = count;
	rebuilt.triggers.reserve(count);
	for (int i = 0; i < count; ++i)
	{
		const WorldRuntime::Trigger  &trigger = triggers.at(i);
		const QMap<QString, QString> &attrs   = trigger.attributes;

		DecodedTrigger                decoded;
		decoded.index            = i;
		decoded.matchText        = attrs.value(QStringLiteral("match"));
		decoded.sendText         = trigger.children.value(QStringLiteral("send"));
		decoded.sequence         = attrs.value(QStringLiteral("sequence")).toInt();
		decoded.enabled          = isEnabledValue(attrs.value(QStringLiteral("enabled")));
		decoded.isRegexp         = isEnabledValue(attrs.value(QStringLiteral("regexp")));
		decoded.ignoreCase       = isEnabledValue(attrs.value(QStringLiteral("ignore_case")));
		decoded.multiLine        = isEnabledValue(attrs.value(QStringLiteral("multi_line")));
		decoded.linesToMatch     = attrs.value(QStringLiteral("lines_to_match")).toInt();
		decoded.expandVariables  = isEnabledValue(attrs.value(QStringLiteral("expand_variables")));
		decoded.sendToValue      = attrs.value(QStringLiteral("send_to")).toInt();
		decoded.matchTextColour  = isEnabledValue(attrs.value(QStringLiteral("match_text_colour")));
		decoded.matchBack        = isEnabledValue(attrs.value(QStringLiteral("match_back_colour")));
		decoded.matchBold        = isEnabledValue(attrs.value(QStringLiteral("match_bold")));
		decoded.matchItalic      = isEnabledValue(attrs.value(QStringLiteral("match_italic")));
		decoded.matchUnderline   = isEnabledValue(attrs.value(QStringLiteral("match_underline")));
		decoded.matchInverse     = isEnabledValue(attrs.value(QStringLiteral("match_inverse")));
		decoded.textColour       = attrs.value(QStringLiteral("text_colour")).toInt();
		decoded.backColour       = attrs.value(QStringLiteral("back_colour")).toInt();
		decoded.desiredBold      = isEnabledValue(attrs.value(QStringLiteral("bold")));
		decoded.desiredItalic    = isEnabledValue(attrs.value(QStringLiteral("italic")));
		decoded.desiredUnderline = isEnabledValue(attrs.value(QStringLiteral("underline")));
		decoded.desiredInverse   = isEnabledValue(attrs.value(QStringLiteral("inverse")));
		decoded.sound            = attrs.value(QStringLiteral("sound")).trimmed();
		decoded.oneShot          = isEnabledValue(attrs.value(QStringLiteral("one_shot")));
		decoded.label            = attrs.value(QStringLiteral("name")).trimmed();
		decoded.scriptLabel      = decoded.label.isEmpty() ? decoded.matchText.trimmed() : decoded.label;
		decoded.lowerWildcards   = isEnabledValue(attrs.value(QStringLiteral("lowercase_wildcard")));
		decoded.omitFromLog      = isEnabledValue(attrs.value(QStringLiteral("omit_from_log")));
		decoded.omitFromOutput   = isEnabledValue(attrs.value(QStringLiteral("omit_from_output")));
		decoded.variableName     = attrs.value(QStringLiteral("variable"));
		decoded.scriptName       = attrs.value(QStringLiteral("script"));
		decoded.colourChange  = triggerColourFromCustom(attrs.value(QStringLiteral("custom_colour")).toInt());
		decoded.makeBold      = isEnabledValue(attrs.value(QStringLiteral("make_bold")));
		decoded.makeItalic    = isEnabledValue(attrs.value(QStringLiteral("make_italic")));
		decoded.makeUnderline = isEnabledValue(attrs.value(QStringLiteral("make_underline")));
		decoded.changeType    = attrs.value(QStringLiteral("colour_change_type")).toInt();
		decoded.repeatMatches = decoded.isRegexp && isEnabledValue(attrs.value(QStringLiteral("repeat")));
		decoded.keepEvaluating  = isEnabledValue(attrs.value(QStringLiteral("keep_evaluating")));
		decoded.otherTextColour = attrs.value(QStringLiteral("other_text_colour"));
		decoded.otherBackColour = attrs.value(QStringLiteral("other_back_colour"));
		decoded.clipboardArg    = attrs.value(QStringLiteral("clipboard_arg")).toInt();
		rebuilt.triggers.push_back(decoded);
	}

	std::ranges::stable_sort(rebuilt.triggers,
	                         [](const DecodedTrigger &left, const DecodedTrigger &right)
	                         {
		                         if (left.sequence != right.sequence)
			                         return left.sequence < right.sequence;
		                         return left.matchText < right.matchText;
	                         });

	if (m_triggerEvaluationCache.size() > 2048)
		m_triggerEvaluationCache.clear();
	m_triggerEvaluationCache.insert(cacheKey, rebuilt);
	return m_triggerEvaluationCache[cacheKey];
}

void WorldCommandProcessor::invalidateTriggerEvaluationCache()
{
	m_triggerEvaluationCache.clear();
	m_triggerEvaluationCacheGeneration = 0;
}

void WorldCommandProcessor::ensurePaletteCache(const QMap<QString, QString> &attrs) const
{
	if (!m_runtime)
		return;

	const quint64 signature = paletteSignature(attrs, m_runtime->colours());
	if (m_paletteCacheValid && m_paletteCacheSignature == signature)
		return;

	const AnsiPalette palette = buildPalette(m_runtime);
	m_paletteCacheNormal      = palette.normal;
	m_paletteCacheBold        = palette.bold;
	m_paletteCacheCustomText  = palette.customText;
	m_paletteCacheCustomBack  = palette.customBack;

	const bool custom16Default = isEnabledValue(attrs.value(QStringLiteral("custom_16_is_default_colour")));
	m_paletteCacheDefaultFore  = parseColorValue(attrs.value(QStringLiteral("output_text_colour")));
	m_paletteCacheDefaultBack  = parseColorValue(attrs.value(QStringLiteral("output_background_colour")));
	if (!m_paletteCacheDefaultFore.isValid())
		m_paletteCacheDefaultFore =
		    custom16Default ? m_paletteCacheCustomText.value(15) : m_paletteCacheNormal.value(7);
	if (!m_paletteCacheDefaultBack.isValid())
		m_paletteCacheDefaultBack =
		    custom16Default ? m_paletteCacheCustomBack.value(15) : m_paletteCacheNormal.value(0);

	m_paletteCacheSignature = signature;
	m_paletteCacheValid     = true;
}

void WorldCommandProcessor::setView(WorldView *view)
{
	m_view = view;
}

const QStringList &WorldCommandProcessor::queuedCommands() const
{
	return m_queuedCommands;
}

int WorldCommandProcessor::triggerPriorityQueuedCommandCount() const
{
	return m_triggerPriorityQueuedCommands;
}

int WorldCommandProcessor::discardQueuedCommands()
{
	const int count                 = QMudCommandQueue::discardAll(m_queuedCommands);
	m_triggerPriorityQueuedCommands = 0;
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

int WorldCommandProcessor::executeUserMacroSendNow(const QString &text, const bool history)
{
	m_omitFromHistoryForEnteredCommand = false;
	m_enteredCommandSendFailed         = false;

	QString commandText = text;
	if (m_translateBackslashSequences)
		commandText = fixupEscapeSequences(commandText);

	const int result = executeCommand(commandText);
	if (result == eOK && history && !m_omitFromHistoryForEnteredCommand && !m_enteredCommandSendFailed &&
	    m_view)
	{
		m_view->addToHistoryForced(text);
	}

	m_omitFromHistoryForEnteredCommand = false;
	m_enteredCommandSendFailed         = false;
	return result;
}

int WorldCommandProcessor::executeCommandWithTriggerPriority(const QString &text)
{
	++m_triggerCommandPriorityDepth;
	const auto restoreTriggerCommandPriority = qScopeGuard([this] { --m_triggerCommandPriorityDepth; });
	Q_UNUSED(restoreTriggerCommandPriority);
	return executeCommand(text);
}

void WorldCommandProcessor::sendToFromAccelerator(int sendTo, const QString &text, const QString &description,
                                                  const WorldRuntime::Plugin *plugin)
{
	this->sendTo(sendTo, text, true, true, QString(), description, plugin);
}

void WorldCommandProcessor::emitTrace(const QString &message) const
{
	if (!m_runtime)
		return;
	QMudTraceDispatch::emitTrace(
	    message, QMudTraceDispatch::Callbacks{[this]() { return m_runtime && m_runtime->traceEnabled(); },
	                                          [this](const bool enabled)
	                                          {
		                                          if (m_runtime)
			                                          m_runtime->setTraceEnabled(enabled);
	                                          },
	                                          [this](const QString &text)
	                                          { return m_runtime && m_runtime->firePluginTrace(text); },
	                                          [this](const QString &line)
	                                          {
		                                          if (m_runtime)
			                                          m_runtime->outputText(line, true, true);
	                                          }});
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
		ensurePaletteCache(*attrs);
		defaultFore         = m_paletteCacheDefaultFore;
		defaultBack         = m_paletteCacheDefaultBack;
		defaultColorsLoaded = true;
	};
	if (m_runtime)
	{
		attrs                          = &m_runtime->worldAttributes();
		const bool wrapEnabled         = isEnabled(attrs->value(QStringLiteral("wrap")));
		const bool autoWrapWindow      = isEnabled(attrs->value(QStringLiteral("auto_wrap_window_width")));
		const bool nawsNegotiated      = m_runtime->isConnected() && m_runtime->isNawsNegotiated();
		const int  worldWrapColumn     = attrs->value(QStringLiteral("wrap_column")).toInt();
		int        effectiveWrapColumn = worldWrapColumn;
		if (autoWrapWindow)
		{
			const int calculatedColumns = m_runtime->outputWrapColumns();
			if (worldWrapColumn > 0)
				effectiveWrapColumn =
				    calculatedColumns > 0 ? qMax(calculatedColumns, worldWrapColumn) : worldWrapColumn;
			else
				effectiveWrapColumn = calculatedColumns;
		}
		wrapColumn  = effectiveWrapColumn;
		runtimeWrap = wrapEnabled && !nawsNegotiated && wrapColumn > 0;
		indentParas = !(
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
		m_runtime->beginOutputViewMutationBatch();
	[[maybe_unused]] const auto flushOutputViewMutations = qScopeGuard(
	    [this]
	    {
		    if (m_runtime)
			    m_runtime->endOutputViewMutationBatch();
	    });

	if (m_runtime)
		m_runtime->beginIncomingLineLuaContext(line, WorldRuntime::LineOutput, normalizedSpans, true);
	if (m_runtime)
		m_runtime->reserveIncomingLineLuaContextInBuffer();

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
				QMudOutputWrapUtils::wrapStyledLineForColumn(displayLine, displaySpans, wrapColumn,
				                                             indentParas);
			}
			const bool updatedRuntimeLine =
			    m_runtime && m_runtime->updateBufferedIncomingLineLuaContext(
			                     displayLine, WorldRuntime::LineOutput, displaySpans, true);
			if (!updatedRuntimeLine && displaySpans.isEmpty())
				m_view->appendOutputText(displayLine);
			else if (!updatedRuntimeLine)
				m_view->appendOutputTextStyled(displayLine, displaySpans);
			if (m_runtime)
			{
				m_runtime->markIncomingLineLuaContextBuffered();
				m_runtime->firePluginScreendraw(0, logOutput && !triggerResult.omitFromLog ? 1 : 0, line);
			}
		}
		else
		{
			const bool hasReplacementOutput =
			    std::ranges::any_of(triggerResult.deferredScripts, [](const DeferredScript &script)
			                        { return script.replaceMatchedLineOutput; }) ||
			    std::ranges::any_of(triggerResult.triggerScripts, [](const TriggerScript &script)
			                        { return script.replaceMatchedLineOutput; });
			if (hasReplacementOutput)
			{
				if (m_runtime)
					static_cast<void>(m_runtime->hideBufferedIncomingLineLuaContextForReplacement());
			}
			else if (m_runtime)
				static_cast<void>(m_runtime->removeBufferedIncomingLineLuaContext());
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

	QVector<LuaStyleRun>                       styleRuns;
	QSharedPointer<const QVector<LuaStyleRun>> styleRunsShared;
	if (m_runtime && (!triggerResult.deferredScripts.isEmpty() || !triggerResult.triggerScripts.isEmpty()))
	{
		loadDefaultColors();
		styleRuns       = buildStyleRuns(line, triggerResult.spans, defaultFore, defaultBack);
		styleRunsShared = QSharedPointer<QVector<LuaStyleRun>>::create(styleRuns);
	}
	const int    triggerMatchedLineBufferIndex = m_runtime ? m_runtime->luaContextLinesInBufferCount() : 0;
	const qint64 triggerMatchedLineAbsoluteNumber =
	    m_runtime ? m_runtime->incomingLineLuaContextAbsoluteNumber() : 0;

	for (const auto &[pluginId, scriptText, description, replaceMatchedLineOutput] :
	     triggerResult.deferredScripts)
	{
		QPointer<WorldRuntime> executionRuntime = m_runtime;
		if (executionRuntime)
			executionRuntime->setCurrentActionSource(WorldRuntime::eTriggerFired);
		emit sendToScriptRequested(pluginId, scriptText, description, &styleRuns, true,
		                           replaceMatchedLineOutput, triggerMatchedLineBufferIndex,
		                           triggerMatchedLineAbsoluteNumber);
		if (executionRuntime)
			executionRuntime->setCurrentActionSource(WorldRuntime::eUnknownActionSource);
	}

	QHash<QString, bool> worldTriggerFunctionPresenceCache;
	for (const auto &[runtimeId, pluginId, label, scriptName, scriptLine, wildcards, namedWildcards,
	                  replaceMatchedLineOutput] : triggerResult.triggerScripts)
	{
		if (scriptName.isEmpty())
			continue;
		WorldRuntime::Trigger *trigger = resolveTriggerByRuntimeId(m_runtime, runtimeId);
		if (!trigger)
			continue;
		WorldRuntime::Plugin             *plugin = resolveCapturedPlugin(m_runtime, pluginId);
		QSharedPointer<LuaCallbackEngine> luaRef = plugin ? plugin->lua : QSharedPointer<LuaCallbackEngine>{};
		LuaCallbackEngine *lua = luaRef ? luaRef.data() : (m_runtime ? m_runtime->luaCallbacks() : nullptr);
		if (!plugin)
		{
			if (const auto it = worldTriggerFunctionPresenceCache.constFind(scriptName);
			    it != worldTriggerFunctionPresenceCache.constEnd() && !it.value())
			{
				warnMissingWorldScriptFunction(QStringLiteral("Trigger"), scriptName);
				continue;
			}
		}
		if (!plugin && !canExecuteWorldScript(QStringLiteral("Trigger"), scriptName, lua))
			continue;
		if (!lua)
			continue;
		if (!m_runtime)
			continue;
		const QSharedPointer<LuaCallbackEngine> dispatchLua =
		    luaRef ? luaRef : QSharedPointer<LuaCallbackEngine>(lua, [](LuaCallbackEngine * /*unused*/) {});
		TriggerExecutionScope executionScope(m_runtime, trigger, true);
		m_runtime->setCurrentActionSource(WorldRuntime::eTriggerFired);
		const auto result = m_runtime->dispatchLuaStringsAndWildcards(
		    dispatchLua, scriptName, {label, scriptLine}, wildcards, namedWildcards, styleRunsShared.data(),
		    replaceMatchedLineOutput, triggerMatchedLineBufferIndex, triggerMatchedLineAbsoluteNumber);
		if (!plugin && result.hasFunctionValid)
		{
			worldTriggerFunctionPresenceCache.insert(scriptName, result.hasFunction);
			if (!result.hasFunction)
			{
				warnMissingWorldScriptFunction(QStringLiteral("Trigger"), scriptName);
				if (m_runtime)
					m_runtime->setCurrentActionSource(WorldRuntime::eUnknownActionSource);
				continue;
			}
		}
		if (m_runtime)
			m_runtime->setCurrentActionSource(WorldRuntime::eUnknownActionSource);
	}

	if (m_runtime && !triggerResult.oneShotTriggers.isEmpty())
	{
		bool removedTrigger = false;
		for (const quint64 runtimeId : triggerResult.oneShotTriggers)
		{
			removedTrigger = removeTriggerByRuntimeId(m_runtime, runtimeId) || removedTrigger;
		}
		if (removedTrigger)
			m_runtime->markTriggerRulesChanged();
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

	QString                          displayLine  = line;
	QVector<WorldRuntime::StyleSpan> displaySpans = spans;
	if (m_runtime && !displayLine.isEmpty())
	{
		const QMap<QString, QString> &attrs       = m_runtime->worldAttributes();
		const bool                    wrapEnabled = isEnabledValue(attrs.value(QStringLiteral("wrap")));
		const bool autoWrapWindow  = isEnabledValue(attrs.value(QStringLiteral("auto_wrap_window_width")));
		const bool nawsNegotiated  = m_runtime->isConnected() && m_runtime->isNawsNegotiated();
		const int  worldWrapColumn = attrs.value(QStringLiteral("wrap_column")).toInt();
		int        wrapColumn      = worldWrapColumn;
		if (autoWrapWindow)
		{
			const int calculatedColumns = m_runtime->outputWrapColumns();
			if (worldWrapColumn > 0)
				wrapColumn =
				    calculatedColumns > 0 ? qMax(calculatedColumns, worldWrapColumn) : worldWrapColumn;
			else
				wrapColumn = calculatedColumns;
		}

		if (wrapEnabled && !nawsNegotiated && wrapColumn > 0)
		{
			const bool indentParas = !(attrs.value(QStringLiteral("indent_paras")) == QStringLiteral("0") ||
			                           attrs.value(QStringLiteral("indent_paras"))
			                                   .compare(QStringLiteral("n"), Qt::CaseInsensitive) == 0 ||
			                           attrs.value(QStringLiteral("indent_paras"))
			                                   .compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0);
			ensurePaletteCache(attrs);
			displaySpans = ensureSpansForLine(displayLine, displaySpans, m_paletteCacheDefaultFore,
			                                  m_paletteCacheDefaultBack);
			QMudOutputWrapUtils::wrapStyledLineForColumn(displayLine, displaySpans, wrapColumn, indentParas);
		}
	}

	m_view->updatePartialOutputText(displayLine, displaySpans);
	if (m_runtime)
		m_runtime->firePluginPartialLine(line);
}

void WorldCommandProcessor::onHyperlinkActivated(const QString &href)
{
	if (href.isEmpty())
		return;

	const QString normalizedHref = normalizeMxpActionText(href);
	if (normalizedHref.isEmpty())
		return;
	if (hasUnresolvedMxpTextEntityReference(normalizedHref))
		return;

	if (const QString lower = normalizedHref.toLower(); lower.startsWith(QStringLiteral("http://")) ||
	                                                    lower.startsWith(QStringLiteral("https://")) ||
	                                                    lower.startsWith(QStringLiteral("mailto:")))
	{
		QDesktopServices::openUrl(QUrl(normalizedHref));
		return;
	}

	MxpHyperlinkDispatchPolicy dispatchPolicy = MxpHyperlinkDispatchPolicy::DirectSend;
	if (m_runtime && m_view)
		dispatchPolicy = resolveMxpHyperlinkDispatchPolicy(m_runtime->lines(), normalizedHref);
	if (dispatchPolicy == MxpHyperlinkDispatchPolicy::PromptInput)
	{
		m_view->setInputText(normalizedHref, true);
		return;
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

	const QString sendText = firstMxpSendAction(normalizedHref);
	if (sendText.isEmpty())
		return;
	if (dispatchPolicy == MxpHyperlinkDispatchPolicy::CommandProcessing)
	{
		PluginHyperlinkCall pluginCall;
		if (m_runtime && parsePluginHyperlinkCall(sendText, pluginCall))
		{
			const int result = m_runtime->callPlugin(pluginCall.pluginId, pluginCall.routine,
			                                         pluginCall.argument, QString());
			if (result != eOK)
			{
				QString pluginName = pluginCall.pluginId;
				if (const WorldRuntime::Plugin *plugin = m_runtime->pluginForId(pluginCall.pluginId))
				{
					if (const QString name = plugin->attributes.value(QStringLiteral("name")).trimmed();
					    !name.isEmpty())
						pluginName = name;
				}

				switch (result)
				{
				case eNoSuchPlugin:
					note(QStringLiteral("Plugin \"%1\" is not installed").arg(pluginName));
					break;
				case eNoSuchRoutine:
					note(QStringLiteral("Script routine \"%1\" is not in plugin %2")
					         .arg(pluginCall.routine, pluginName));
					break;
				case eErrorCallingPluginRoutine:
					note(QStringLiteral("An error occurred calling plugin %1").arg(pluginName));
					break;
				default:
				{
					const QString description = WorldRuntime::errorDesc(result);
					if (!description.isEmpty())
						note(QStringLiteral("%1 (error %2)").arg(description).arg(result));
					else
						note(QStringLiteral("Plugin callback failed (error %1)").arg(result));
					break;
				}
				}
			}
		}
		else
		{
			executeCommand(sendText);
		}
	}
	else
		sendMsg(sendText, echo, false, true);
	if (addToHistory && m_view)
		m_view->addHyperlinkToHistory(sendText);
}

void WorldCommandProcessor::onMiniWindowOutputActionActivated(const int actionType, const QString &action)
{
	if (actionType == WorldRuntime::ActionPrompt)
	{
		const QString normalizedAction = normalizeMxpActionText(action);
		if (normalizedAction.isEmpty())
			return;
		if (hasUnresolvedMxpTextEntityReference(normalizedAction))
			return;
		if (m_view)
			m_view->setInputText(normalizedAction, true);
		return;
	}

	onHyperlinkActivated(action);
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

void WorldCommandProcessor::sendRawTextWithTriggerPriority(const QString &text, const bool echo,
                                                           const bool queue, const bool log,
                                                           const bool history)
{
	++m_triggerCommandPriorityDepth;
	const auto restoreTriggerCommandPriority = qScopeGuard([this] { --m_triggerCommandPriorityDepth; });
	Q_UNUSED(restoreTriggerCommandPriority);
	sendRawText(text, echo, queue, log, history);
}

void WorldCommandProcessor::sendImmediateText(const QString &text, const bool echo, const bool log,
                                              const bool history)
{
	doSendMsg(text, echo, log);
	if (history && m_view)
		m_view->addToHistoryForced(text);
}

void WorldCommandProcessor::sendDeferredInboundText(const QString &text, const bool echo, const bool log)
{
	doSendMsg(text, echo, log);
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

	++m_autoConnectDelayGeneration;

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

	if (m_runtime->consumeReloadReattachConnectActionsSuppressed())
	{
		m_runtime->fireWorldConnectHandlers();
		return;
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
	const int     connectDelay = qBound(0, attrs.value(QStringLiteral("connect_delay")).toInt(), 10000);

	const bool    needsPassword = password.isEmpty() && ((connectMethod != 0 && !player.isEmpty()) ||
	                                                     connectText.contains(QStringLiteral("%password%")));
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

	if (connectDelay > 0 && (connectMethod == eConnectMUSH || connectMethod == eConnectDiku))
	{
		const QPointer<WorldCommandProcessor> self(this);
		const QPointer<WorldRuntime>          runtime(m_runtime);
		const quint64                         generation = m_autoConnectDelayGeneration;
		QTimer::singleShot(
		    connectDelay, this,
		    [self, runtime, generation, connectMethod, player, password, connectText, echoInput]
		    {
			    if (!self || !runtime || self->m_runtime != runtime.data() ||
			        self->m_autoConnectDelayGeneration != generation || !runtime->isConnected())
				    return;
			    self->runWorldConnectActions(connectMethod, player, password, connectText, echoInput);
		    });
		return;
	}

	runWorldConnectActions(connectMethod, player, password, connectText, echoInput);
}

void WorldCommandProcessor::runWorldConnectActions(const int connectMethod, const QString &player,
                                                   const QString &password, const QString &connectText,
                                                   const bool echoInput)
{
	if (!m_runtime)
		return;

	switch (connectMethod)
	{
	case eConnectMUSH:
		if (!player.isEmpty() && !password.isEmpty())
		{
			const QString cmd = QStringLiteral("connect %1 %2").arg(player, password);
			sendMsg(cmd, false, false, false);
		}
		break;
	case eConnectDiku:
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

void WorldCommandProcessor::handleWorldDisconnected()
{
	++m_autoConnectDelayGeneration;

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
	const bool userMacroSource = m_runtime && m_runtime->currentActionSource() == WorldRuntime::eUserMacro;
	bool       autoSay =
	    !userMacroSource && !m_processingAutoSay && isEnabled(attrs.value(QStringLiteral("enable_auto_say")));
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
	bool                         bEchoAlias       = echoInput;
	bool                         bOmitFromLog     = false;
	bool                         bOmitFromHistory = false;
	QVector<AliasRef>            matchedAliases;
	QVector<AliasRef>            oneShotAliases;
	bool                         stopAliasEvaluation = false;
	QList<WorldRuntime::Plugin> &pluginList          = m_runtime->pluginsMutable();
	const QVector<int>          &pluginIndices       = sortedPluginIndices();
	for (int pluginIndex : pluginIndices)
	{
		if (pluginIndex < 0 || pluginIndex >= pluginList.size())
			continue;
		WorldRuntime::Plugin *plugin = &pluginList[pluginIndex];
		if (!plugin || !pluginHasValidId(*plugin) || !plugin->enabled || plugin->installPending ||
		    plugin->sequence >= 0)
			continue;
		if (processOneAliasSequence(line, true, bOmitFromLog, bEchoAlias, bOmitFromHistory, matchedAliases,
		                            oneShotAliases, plugin))
		{
			stopAliasEvaluation = true;
			break;
		}
	}

	if (!stopAliasEvaluation && shouldEvaluateRuleCollection(attrs, WorldRuleKind::Alias, false) &&
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
		if (!plugin || !pluginHasValidId(*plugin) || !plugin->enabled || plugin->installPending ||
		    plugin->sequence < 0)
			continue;
		if (processOneAliasSequence(line, true, bOmitFromLog, bEchoAlias, bOmitFromHistory, matchedAliases,
		                            oneShotAliases, plugin))
		{
			break;
		}
	}

	if (bOmitFromHistory)
		m_omitFromHistoryForEnteredCommand = true;

	if (matchedAliases.isEmpty())
	{
		if (QMudNativePluginRegistry::handleMushReaderCommand(m_runtime, line))
			return false;
		if (QMudNativePluginRegistry::handleLuaAudioCommand(m_runtime, line))
			return false;

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

	QHash<QString, bool> worldAliasFunctionPresenceCache;
	for (const auto &[runtimeId, pluginId, label, scriptName, scriptLine, wildcards, namedWildcards] :
	     matchedAliases)
	{
		if (scriptName.isEmpty())
			continue;
		WorldRuntime::Alias *alias = resolveAliasByRuntimeId(m_runtime, runtimeId);
		if (!alias)
			continue;
		WorldRuntime::Plugin             *plugin = resolveCapturedPlugin(m_runtime, pluginId);
		QSharedPointer<LuaCallbackEngine> luaRef = plugin ? plugin->lua : QSharedPointer<LuaCallbackEngine>{};
		LuaCallbackEngine *lua = luaRef ? luaRef.data() : (m_runtime ? m_runtime->luaCallbacks() : nullptr);
		if (!plugin)
		{
			if (const auto it = worldAliasFunctionPresenceCache.constFind(scriptName);
			    it != worldAliasFunctionPresenceCache.constEnd() && !it.value())
			{
				warnMissingWorldScriptFunction(QStringLiteral("Alias"), scriptName);
				continue;
			}
		}
		if (!plugin && !canExecuteWorldScript(QStringLiteral("Alias"), scriptName, lua))
			continue;
		if (lua)
		{
			AliasExecutionScope executionScope(m_runtime, alias, true);
			if (m_runtime)
			{
				const QSharedPointer<LuaCallbackEngine> dispatchLua =
				    luaRef ? luaRef
				           : QSharedPointer<LuaCallbackEngine>(lua, [](LuaCallbackEngine * /*unused*/) {});
				const auto result = m_runtime->dispatchLuaStringsAndWildcards(
				    dispatchLua, scriptName, {label, scriptLine}, wildcards, namedWildcards);
				if (!plugin && result.hasFunctionValid)
				{
					worldAliasFunctionPresenceCache.insert(scriptName, result.hasFunction);
					if (!result.hasFunction)
					{
						warnMissingWorldScriptFunction(QStringLiteral("Alias"), scriptName);
						continue;
					}
				}
			}
		}
	}

	if (m_runtime && !oneShotAliases.isEmpty())
	{
		for (const AliasRef &ref : oneShotAliases)
		{
			removeAliasByRuntimeId(m_runtime, ref.runtimeId);
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
			if (const auto &lines = m_runtime->lines();
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
		const QString label       = alias.attributes.value(QStringLiteral("name"));
		const QString scriptLabel = label.isEmpty() ? matchText : label;
		if (label.isEmpty())
			emitTrace(QStringLiteral("Matched alias \"%1\"").arg(matchText));
		else
			emitTrace(QStringLiteral("Matched alias %1").arg(label));

		const QString  language       = m_runtime->worldAttributes().value(QStringLiteral("script_language"));
		constexpr bool lowerWildcards = false;
		const int      sendToValue    = alias.attributes.value(QStringLiteral("send_to")).toInt();
		const bool     expandVariables = attrTrue(alias.attributes.value(QStringLiteral("expand_variables")));
		const bool     omitFromOutput  = attrTrue(alias.attributes.value(QStringLiteral("omit_from_output")));
		const bool     omitFromLogValue = attrTrue(alias.attributes.value(QStringLiteral("omit_from_log")));
		const QString  variableName     = alias.attributes.value(QStringLiteral("variable"));
		const QString  scriptName       = alias.attributes.value(QStringLiteral("script"));
		const bool     oneShot          = attrTrue(alias.attributes.value(QStringLiteral("one_shot")));
		const bool     keepEvaluating   = attrTrue(alias.attributes.value(QStringLiteral("keep_evaluating")));
		QStringList    fixedWildcards   = wildcards;
		for (QString &fixed : fixedWildcards)
			fixed = fixWildcard(fixed, lowerWildcards, sendToValue, language);
		QMap<QString, QString> fixedNamed = namedWildcards;
		for (auto it = fixedNamed.begin(); it != fixedNamed.end(); ++it)
			it.value() = fixWildcard(it.value(), lowerWildcards, sendToValue, language);

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
		sendText   = fixSendText(fixupEscapeSequences(sendText), sendToValue, wildcards, namedWildcards,
		                         m_runtime->worldAttributes().value(QStringLiteral("script_language")),
		                         false, // lower-case wildcards
		                         expandVariables,
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

		const quint64       aliasRuntimeId = ensureAliasRuntimeId(m_runtime, &alias);
		AliasExecutionScope executionScope(m_runtime, &alias, false);
		sendTo(sendToValue, sendText, omitFromOutput, omitFromLogValue, variableName, scriptLabel, plugin);

		AliasRef ref;
		ref.runtimeId      = aliasRuntimeId;
		ref.pluginId       = pluginIdOf(plugin);
		ref.label          = scriptLabel;
		ref.scriptName     = scriptName;
		ref.line           = currentLine;
		ref.wildcards      = wildcards;
		ref.namedWildcards = namedWildcards;
		matchedAliases.push_back(ref);

		if (oneShot)
			oneShotAliases.push_back(ref);

		// display/output behavior is handled inline in sendTo/sendMsg runtime paths.

		// only re-match if they want multiple matches

		if (!keepEvaluating)
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

	const QString                          &matchInputLine  = line;
	const QVector<WorldRuntime::StyleSpan> &matchInputSpans = spans;
	const QString trimmedTriggerLine = QMudCommandText::normalizeTriggerMatchLine(matchInputLine, false);

	// Keep original incoming text for multiline trigger history.
	m_runtime->addRecentLine(matchInputLine);
	m_runtime->setLineOmittedFromOutput(false);

	const QMap<QString, QString> &worldAttrs = m_runtime->worldAttributes();
	const bool worldTriggersEnabled = shouldEvaluateRuleCollection(worldAttrs, WorldRuleKind::Trigger, false);
	const bool worldTriggerSoundsEnabled =
	    isEnabledValue(worldAttrs.value(QStringLiteral("enable_trigger_sounds")));

	QElapsedTimer timer;
	timer.start();

	const QString language = worldAttrs.value(QStringLiteral("script_language"));

	bool          paletteReady = false;
	QColor        defaultFore;
	QColor        defaultBack;
	auto          ensurePaletteReady = [&]
	{
		if (paletteReady)
			return;
		ensurePaletteCache(worldAttrs);
		defaultFore  = m_paletteCacheDefaultFore;
		defaultBack  = m_paletteCacheDefaultBack;
		paletteReady = true;
	};

	QVector<WorldRuntime::StyleSpan> workingSpans = spans;
	bool                             spansChanged = false;

	auto buildTarget = [&](const int linesToMatch, const bool preserveTrailingWhitespace) -> QString
	{
		int count = linesToMatch;
		if (count <= 0)
			count = 1;
		const QStringList recentLines = m_runtime->recentLines(count);
		return QMudCommandText::buildTriggerMultilineTarget(recentLines, preserveTrailingWhitespace);
	};

	auto colourIndexFor = [&](const QColor &colour) -> int
	{
		ensurePaletteReady();
		for (int i = 0; i < m_paletteCacheNormal.size(); ++i)
		{
			if (colour == m_paletteCacheNormal.at(i) || colour == m_paletteCacheBold.at(i))
				return i;
		}
		return -1;
	};

	auto styleAtColumn = [&](const int column, QColor &fore, QColor &back, bool &bold, bool &italic,
	                         bool &underline, bool &inverse) -> bool
	{
		ensurePaletteReady();
		// Trigger matching always uses immutable input line/spans.
		// Trigger style rewrites are output transformations only.
		const auto effective = ensureSpansForLine(matchInputLine, matchInputSpans, defaultFore, defaultBack);
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

	auto processSequence = [&](QList<WorldRuntime::Trigger> &triggers, WorldRuntime::Plugin *plugin)
	{
		const TriggerEvaluationCacheEntry &cacheEntry = decodedTriggerEvaluationCache(triggers);
		for (const DecodedTrigger &decoded : cacheEntry.triggers)
		{
			if (m_runtime->stopTriggerEvaluation() != WorldRuntime::KeepEvaluating)
				break;

			if (decoded.index < 0 || decoded.index >= safeQSizeToInt(triggers.size()))
				continue;
			WorldRuntime::Trigger &trigger = triggers[decoded.index];
			if (!decoded.enabled)
				continue;

			m_runtime->incrementTriggersEvaluated();
			trigger.matchAttempts++;

			QString matchText = decoded.matchText;
			if (matchText.isEmpty())
				continue;

			const bool preserveTrailingWhitespace = decoded.isRegexp;
			QString    target = decoded.multiLine
			                        ? buildTarget(decoded.linesToMatch, preserveTrailingWhitespace)
			                        : (preserveTrailingWhitespace ? matchInputLine : trimmedTriggerLine);
			trigger.lastMatchTarget = target;

			if (decoded.expandVariables && matchText.contains(QLatin1Char('@')))
			{
				matchText = fixSendText(matchText, decoded.sendToValue, QStringList(),
				                        QMap<QString, QString>(), language, false, true, false, true,
				                        decoded.isRegexp, false, QString(), plugin, nullptr);
			}

			const QString          pattern = decoded.isRegexp ? matchText : wildcardToRegexCached(matchText);
			QStringList            wildcards;
			QMap<QString, QString> namedWildcards;
			int                    startCol = 0;
			int                    endCol   = 0;
			if (!regexMatch(pattern, target, decoded.ignoreCase, wildcards, namedWildcards, &startCol,
			                &endCol, 0, decoded.multiLine))
			{
				continue;
			}

			if (!decoded.multiLine)
			{
				const bool requiresStyleState = decoded.matchTextColour || decoded.matchBack ||
				                                decoded.matchBold || decoded.matchItalic ||
				                                decoded.matchUnderline || decoded.matchInverse;

				if (requiresStyleState)
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

					if (decoded.matchTextColour)
					{
						if (colourIndexFor(fore) != decoded.textColour)
						{
							continue;
						}
					}
					if (decoded.matchBack)
					{
						if (colourIndexFor(back) != decoded.backColour)
						{
							continue;
						}
					}
					if (decoded.matchBold)
					{
						if (bold != decoded.desiredBold)
						{
							continue;
						}
					}
					if (decoded.matchItalic)
					{
						if (italic != decoded.desiredItalic)
						{
							continue;
						}
					}
					if (decoded.matchUnderline)
					{
						if (underline != decoded.desiredUnderline)
						{
							continue;
						}
					}
					if (decoded.matchInverse)
					{
						if (inverse != decoded.desiredInverse)
						{
							continue;
						}
					}
				}
			}

			trigger.matched++;
			trigger.lastMatched = QDateTime::currentDateTime();
			m_runtime->incrementTriggersMatched();
			const quint64 triggerRuntimeId = ensureTriggerRuntimeId(m_runtime, &trigger);
			const bool    triggerSoundEnabled =
			    QMudTriggerSound::shouldPlayTriggerSound(plugin != nullptr, worldTriggerSoundsEnabled);
			if (!decoded.sound.isEmpty() &&
			    decoded.sound.compare(QStringLiteral("(No sound)"), Qt::CaseInsensitive) != 0 &&
			    triggerSoundEnabled)
			{
				if (!isEnabledValue(trigger.attributes.value(QStringLiteral("sound_if_inactive"))) ||
				    !m_runtime->isActive())
					m_runtime->playSound(0, decoded.sound, false, 0.0, 0.0);
			}

			if (decoded.oneShot)
			{
				result.oneShotTriggers.push_back(triggerRuntimeId);
			}

			if (decoded.label.isEmpty())
				emitTrace(QStringLiteral("Matched trigger \"%1\"").arg(decoded.matchText));
			else
				emitTrace(QStringLiteral("Matched trigger %1").arg(decoded.label));

			QStringList fixedWildcards = wildcards;
			for (QString &fixed : fixedWildcards)
				fixed = fixWildcard(fixed, decoded.lowerWildcards, decoded.sendToValue, language);
			QMap<QString, QString> fixedNamed = namedWildcards;
			for (auto it = fixedNamed.begin(); it != fixedNamed.end(); ++it)
				it.value() = fixWildcard(it.value(), decoded.lowerWildcards, decoded.sendToValue, language);

			if (m_runtime && !decoded.scriptLabel.isEmpty())
			{
				if (plugin)
					m_runtime->setPluginTriggerWildcards(plugin->attributes.value(QStringLiteral("id")),
					                                     decoded.scriptLabel, fixedWildcards, fixedNamed);
				else
					m_runtime->setTriggerWildcards(decoded.scriptLabel, fixedWildcards, fixedNamed);
			}

			if (decoded.clipboardArg > 0 && decoded.clipboardArg < fixedWildcards.size())
			{
				if (QClipboard *clipboard = QGuiApplication::clipboard())
					clipboard->setText(fixedWildcards.at(decoded.clipboardArg));
			}

			QString sendText = decoded.sendText;
			sendText = fixSendText(fixupEscapeSequences(sendText), decoded.sendToValue, wildcards,
			                       namedWildcards, language, decoded.lowerWildcards, decoded.expandVariables,
			                       true, false, false, false, decoded.scriptLabel, plugin, nullptr);

			if (!decoded.multiLine)
			{
				if (decoded.omitFromLog)
					result.omitFromLog = true;
				if (decoded.omitFromOutput)
					result.omitFromOutput = true;
			}

			if (decoded.sendToValue == eSendToScriptAfterOmit)
			{
				DeferredScript deferred;
				deferred.pluginId                 = pluginIdOf(plugin);
				deferred.scriptText               = sendText;
				deferred.description              = QStringLiteral("Trigger: %1").arg(decoded.scriptLabel);
				deferred.replaceMatchedLineOutput = decoded.omitFromOutput;
				result.deferredScripts.push_back(deferred);
			}
			else if (decoded.sendToValue == eSendToOutput)
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
				TriggerExecutionScope executionScope(m_runtime, &trigger, false);
				unsigned short        previousActionSource = WorldRuntime::eUnknownActionSource;
				if (m_runtime)
				{
					previousActionSource = m_runtime->currentActionSource();
					m_runtime->setCurrentActionSource(WorldRuntime::eTriggerFired);
				}
				if (decoded.sendToValue == eSendToScript)
				{
					ensurePaletteReady();
					const QVector<WorldRuntime::StyleSpan> &sourceSpans =
					    workingSpans.isEmpty() ? matchInputSpans : workingSpans;
					const QVector<LuaStyleRun> triggerStyleRuns =
					    buildStyleRuns(matchInputLine, sourceSpans, defaultFore, defaultBack);
					sendTo(decoded.sendToValue, sendText, decoded.omitFromOutput, decoded.omitFromLog,
					       decoded.variableName, QStringLiteral("Trigger: %1").arg(decoded.scriptLabel),
					       plugin, &triggerStyleRuns, true, false, m_runtime->luaContextLinesInBufferCount(),
					       m_runtime->incomingLineLuaContextAbsoluteNumber());
				}
				else
				{
					sendTo(decoded.sendToValue, sendText, decoded.omitFromOutput, decoded.omitFromLog,
					       decoded.variableName, QStringLiteral("Trigger: %1").arg(decoded.scriptLabel),
					       plugin);
				}
				if (m_runtime)
					m_runtime->setCurrentActionSource(previousActionSource);
			}

			if (!decoded.scriptName.isEmpty())
			{
				TriggerScript script;
				script.runtimeId                = triggerRuntimeId;
				script.pluginId                 = pluginIdOf(plugin);
				script.label                    = decoded.scriptLabel;
				script.scriptName               = decoded.scriptName;
				script.line                     = matchInputLine;
				script.wildcards                = wildcards;
				script.namedWildcards           = namedWildcards;
				script.replaceMatchedLineOutput = decoded.omitFromOutput;
				result.triggerScripts.push_back(script);
			}

			if (!decoded.multiLine && (decoded.colourChange >= 0 || decoded.makeBold || decoded.makeItalic ||
			                           decoded.makeUnderline))
			{
				if (workingSpans.isEmpty())
				{
					ensurePaletteReady();
					workingSpans =
					    ensureSpansForLine(matchInputLine, matchInputSpans, defaultFore, defaultBack);
				}
				spansChanged = true;

				auto applyColour = [&](const int sCol, const int eCol)
				{
					QColor newFore;
					QColor newBack;
					bool   changeFore = false;
					bool   changeBack = false;
					if (decoded.colourChange >= 0)
					{
						ensurePaletteReady();
						if (decoded.colourChange == OTHER_CUSTOM)
						{
							newFore = parseColorValue(decoded.otherTextColour);
							newBack = parseColorValue(decoded.otherBackColour);
						}
						else if (decoded.colourChange < m_paletteCacheCustomText.size())
						{
							newFore = m_paletteCacheCustomText.at(decoded.colourChange);
							newBack = m_paletteCacheCustomBack.at(decoded.colourChange);
						}
						if (decoded.changeType == TRIGGER_COLOUR_CHANGE_BOTH ||
						    decoded.changeType == TRIGGER_COLOUR_CHANGE_FOREGROUND)
							changeFore = true;
						if (decoded.changeType == TRIGGER_COLOUR_CHANGE_BOTH ||
						    decoded.changeType == TRIGGER_COLOUR_CHANGE_BACKGROUND)
							changeBack = true;
					}
					applyStyleToSpans(workingSpans, sCol, eCol, newFore, newBack, changeFore, changeBack,
					                  decoded.makeBold, decoded.makeItalic, decoded.makeUnderline);
				};

				applyColour(startCol, endCol);

				if (decoded.repeatMatches)
				{
					int offset = endCol;
					while (regexMatch(pattern, target, decoded.ignoreCase, wildcards, namedWildcards,
					                  &startCol, &endCol, offset, decoded.multiLine))
					{
						if (endCol <= offset)
							break;
						applyColour(startCol, endCol);
						offset = endCol;
					}
				}
			}

			if (!decoded.keepEvaluating)
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
		if (!plugin || !pluginHasValidId(*plugin) || !plugin->enabled || plugin->installPending ||
		    plugin->sequence >= 0)
			continue;
		m_runtime->setStopTriggerEvaluation(WorldRuntime::KeepEvaluating);
		processSequence(plugin->triggers, plugin);
		if (m_runtime->stopTriggerEvaluation() == WorldRuntime::StopAllSequences)
			break;
	}

	if (m_runtime->stopTriggerEvaluation() != WorldRuntime::StopAllSequences)
	{
		if (worldTriggersEnabled)
		{
			m_runtime->setStopTriggerEvaluation(WorldRuntime::KeepEvaluating);
			processSequence(m_runtime->triggersMutable(), nullptr);
		}
	}

	if (m_runtime->stopTriggerEvaluation() != WorldRuntime::StopAllSequences)
	{
		for (int pluginIndex : pluginIndices)
		{
			if (pluginIndex < 0 || pluginIndex >= pluginList.size())
				continue;
			WorldRuntime::Plugin *plugin = &pluginList[pluginIndex];
			if (!plugin || !pluginHasValidId(*plugin) || !plugin->enabled || plugin->installPending ||
			    plugin->sequence < 0)
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
		result.spans = matchInputSpans;

	m_runtime->setLineOmittedFromOutput(result.omitFromOutput);
	m_runtime->addTriggerTimeNs(timer.nsecsElapsed());
	return result;
}

void WorldCommandProcessor::checkTimers()
{
	processQueuedCommands(false);

	if (!m_runtime)
		return;
	const bool worldTimersEnabled =
	    shouldEvaluateRuleCollection(m_runtime->worldAttributes(), WorldRuleKind::Timer, false);

	const QDateTime now       = QDateTime::currentDateTime();
	const bool      connected = m_runtime->connectPhase() == WorldRuntime::eConnectConnectedToMud;

	auto            processTimers = [&](const QString &contextPluginId) -> bool
	{
		constexpr int kMaxRescansPerTick = 2;
		int           rescanCount        = 0;
		const bool    pluginScoped       = !contextPluginId.isEmpty();

		while (true)
		{
			QList<WorldRuntime::Timer> *timers = nullptr;
			const WorldRuntime::Plugin *plugin = nullptr;
			if (!pluginScoped)
			{
				timers = &m_runtime->timersMutable();
			}
			else
			{
				WorldRuntime::Plugin *activePlugin =
				    m_runtime ? m_runtime->pluginForId(contextPluginId) : nullptr;
				if (!activePlugin || !pluginHasValidId(*activePlugin) || !activePlugin->enabled ||
				    activePlugin->installPending)
					return false;
				plugin = activePlugin;
				timers = &activePlugin->timers;
			}

			QVector<quint64> firedRuntimeIds;
			for (int i = 0, size = safeQSizeToInt(timers->size()); i < size; ++i)
			{
				WorldRuntime::Timer &timer = (*timers)[i];
				if (!QMudTimerScheduling::isTimerDue(timer, now, connected))
					continue;
				const quint64 timerRuntimeId = ensureTimerRuntimeId(m_runtime, &timer);
				if (timerRuntimeId == 0)
					continue;
				firedRuntimeIds.push_back(timerRuntimeId);
			}

			bool rescanNeeded = false;
			for (const quint64 timerRuntimeId : std::as_const(firedRuntimeIds))
			{
				WorldRuntime::Timer *timer = resolveTimerByRuntimeId(m_runtime, timerRuntimeId);
				if (!timer)
				{
					if (pluginScoped)
						return false;
					rescanNeeded = true;
					break;
				}

				if (!isEnabledValue(timer->attributes.value(QStringLiteral("enabled"))))
					continue;

				QMudTimerScheduling::applyTimerFiredState(*timer, now);
				m_runtime->incrementTimersFired();

				const int  sendToValue = timer->attributes.value(QStringLiteral("send_to")).toInt();
				const bool omitFromOutput =
				    isEnabledValue(timer->attributes.value(QStringLiteral("omit_from_output")));
				const bool omitFromLog =
				    isEnabledValue(timer->attributes.value(QStringLiteral("omit_from_log")));
				const QString variableName = timer->attributes.value(QStringLiteral("variable"));
				const QString label        = timer->attributes.value(QStringLiteral("name")).trimmed();
				const QString sendText     = timer->children.value(QStringLiteral("send"));
				const QString scriptName   = timer->attributes.value(QStringLiteral("script"));
				if (label.isEmpty())
					emitTrace(QStringLiteral("Fired unlabelled timer "));
				else
					emitTrace(QStringLiteral("Fired timer %1").arg(label));

				const quint64 serialBeforeSend = m_runtime->timerStructureMutationSerial();
				{
					TimerExecutionScope executionScope(m_runtime, timer, false);
					m_runtime->setCurrentActionSource(WorldRuntime::eTimerFired);
					sendTo(sendToValue, sendText, omitFromOutput, omitFromLog, variableName,
					       QStringLiteral("Timer: %1").arg(label), plugin);
					m_runtime->setCurrentActionSource(WorldRuntime::eUnknownActionSource);
				}
				const quint64 serialAfterSend = m_runtime->timerStructureMutationSerial();
				if (serialAfterSend != serialBeforeSend)
				{
					if (pluginScoped)
						return false;
					rescanNeeded = true;
					break;
				}

				if (scriptName.isEmpty())
					continue;

				const QSharedPointer<LuaCallbackEngine> luaRef =
				    plugin ? plugin->lua : QSharedPointer<LuaCallbackEngine>{};
				LuaCallbackEngine *lua = luaRef ? luaRef.data() : m_runtime->luaCallbacks();
				if (!plugin && !canExecuteWorldScript(QStringLiteral("Timer"), scriptName, lua))
					continue;
				if (!lua)
					continue;
				if (!m_runtime)
					continue;

				const QSharedPointer<LuaCallbackEngine> dispatchLua =
				    luaRef ? luaRef
				           : QSharedPointer<LuaCallbackEngine>(lua, [](LuaCallbackEngine * /*unused*/) {});
				const bool worldScript = plugin == nullptr;
				{
					TimerExecutionScope executionScope(m_runtime, timer, true);
					m_runtime->setCurrentActionSource(WorldRuntime::eTimerFired);
					const LuaBatchDispatchResult result = m_runtime->dispatchLuaStringsAndWildcards(
					    dispatchLua, scriptName, {label}, {}, {}, nullptr);
					if (worldScript && result.hasFunctionValid && !result.hasFunction)
						warnMissingWorldScriptFunction(QStringLiteral("Timer"), scriptName);
					m_runtime->setCurrentActionSource(WorldRuntime::eUnknownActionSource);
				}
			}

			if (!rescanNeeded || rescanCount >= kMaxRescansPerTick)
				break;
			++rescanCount;
		}
		return false;
	};

	if (worldTimersEnabled)
	{
		if (processTimers(QString()))
			return;
	}

	QStringList pluginIds;
	for (const auto &plugin : m_runtime->plugins())
	{
		if (!pluginHasValidId(plugin) || !plugin.enabled || plugin.installPending)
			continue;
		const QString pluginId = plugin.attributes.value(QStringLiteral("id")).trimmed().toLower();
		pluginIds.push_back(pluginId);
	}
	for (const QString &pluginId : std::as_const(pluginIds))
	{
		WorldRuntime::Plugin *plugin = m_runtime->pluginForId(pluginId);
		if (!plugin || !pluginHasValidId(*plugin) || !plugin->enabled || plugin->installPending)
			continue;
		if (processTimers(pluginId))
			return;
	}
}

void WorldCommandProcessor::sendTo(const int sendTo, const QString &text, const bool omitFromOutput,
                                   const bool omitFromLog, const QString &variableName,
                                   const QString &description, const WorldRuntime::Plugin *plugin,
                                   const QVector<LuaStyleRun> *styleRuns, const bool hasTriggerContext,
                                   const bool   replaceMatchedLineOutput,
                                   const int    triggerMatchedLineBufferIndex,
                                   const qint64 triggerMatchedLineAbsoluteNumber)
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
	const bool triggerCommandPriority = m_runtime &&
	                                    m_runtime->currentActionSource() == WorldRuntime::eTriggerFired &&
	                                    (sendTo == eSendToWorld || sendTo == eSendToExecute);
	if (triggerCommandPriority)
		++m_triggerCommandPriorityDepth;
	[[maybe_unused]] const auto restoreTriggerCommandPriority = qScopeGuard(
	    [this, triggerCommandPriority]
	    {
		    if (triggerCommandPriority)
			    --m_triggerCommandPriorityDepth;
	    });

	switch (sendTo)
	{
	case eSendToWorld:
		sendMsg(QMudCommandText::normalizeActionCommandTextNewlines(text), echoSend, false, logIt);
		break;
	case eSendToCommandQueue:
		sendMsg(QMudCommandText::normalizeActionCommandTextNewlines(text), echoSend, true, logIt);
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
				main->sendToNotepad(description, text + kEndLine, m_runtime);
		}
		break;
	case eAppendToNotepad:
		if (m_view)
		{
			if (MainWindowHost *main = resolveMainWindowHost(m_view->window()))
				main->appendToNotepad(description, text + kEndLine, false, m_runtime);
		}
		break;
	case eReplaceNotepad:
		if (m_view)
		{
			if (MainWindowHost *main = resolveMainWindowHost(m_view->window()))
				main->appendToNotepad(description, text + kEndLine, true, m_runtime);
		}
		break;
	case eSendToExecute:
	{
		const bool saved = m_suppressInputLog;
		if (omitFromLog)
			m_suppressInputLog = true;
		const QString     normalized = QMudCommandText::normalizeActionCommandTextNewlines(text);
		const QStringList commands   = normalized.split(kEndLine, Qt::SkipEmptyParts);
		if (!commands.isEmpty())
		{
			for (const QString &command : commands)
				executeCommand(command);
		}
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
		emit sendToScriptRequested(pluginIdOf(plugin), text, description, styleRuns, hasTriggerContext,
		                           replaceMatchedLineOutput, triggerMatchedLineBufferIndex,
		                           triggerMatchedLineAbsoluteNumber);
		break;
	default:
		break;
	}
}

void WorldCommandProcessor::dispatchScriptSend(
    const QString &pluginId, const QString &text, const QString &description,
    const QVector<LuaStyleRun> *styleRuns, const bool hasTriggerContext, const bool replaceMatchedLineOutput,
    const int triggerMatchedLineBufferIndex, const qint64 triggerMatchedLineAbsoluteNumber) const
{
	if (!m_runtime)
		return;

	QSharedPointer<LuaCallbackEngine> luaRef;
	LuaCallbackEngine                *lua = nullptr;
	if (!pluginId.isEmpty())
	{
		if (const WorldRuntime::Plugin *plugin = m_runtime->pluginForId(pluginId);
		    plugin && pluginHasValidId(*plugin) && plugin->enabled && !plugin->installPending)
		{
			luaRef = plugin->lua;
			lua    = luaRef.data();
		}
	}
	else
	{
		const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
		const bool scriptingEnabled = isEnabledValue(attrs.value(QStringLiteral("enable_scripts"))) &&
		                              attrs.value(QStringLiteral("script_language"))
		                                      .compare(QStringLiteral("Lua"), Qt::CaseInsensitive) == 0;
		if (!scriptingEnabled)
		{
			if (isEnabledValue(attrs.value(QStringLiteral("warn_if_scripting_inactive"))))
			{
				const QString name = description.isEmpty() ? QStringLiteral("(unnamed)") : description;
				m_runtime->outputText(
				    QStringLiteral("Script function \"%1\" cannot execute - scripting disabled/parse error.")
				        .arg(name),
				    true, true);
			}
			return;
		}
		lua = m_runtime->luaCallbacks();
		if (!lua)
		{
			if (isEnabledValue(attrs.value(QStringLiteral("warn_if_scripting_inactive"))))
			{
				const QString name = description.isEmpty() ? QStringLiteral("(unnamed)") : description;
				m_runtime->outputText(
				    QStringLiteral("Script function \"%1\" cannot execute - scripting disabled/parse error.")
				        .arg(name),
				    true, true);
			}
			return;
		}
		luaRef = QSharedPointer<LuaCallbackEngine>(lua, [](LuaCallbackEngine * /*unused*/) {});
	}

	if (!lua)
		return;

	QPointer<WorldRuntime> executionRuntime = m_runtime;
	if (!executionRuntime)
		return;
	if (hasTriggerContext)
	{
		static_cast<void>(executionRuntime->dispatchLuaExecuteScript(
		    luaRef, text, description, styleRuns, hasTriggerContext, replaceMatchedLineOutput,
		    triggerMatchedLineBufferIndex, triggerMatchedLineAbsoluteNumber));
		return;
	}

	executionRuntime->dispatchLuaExecuteScriptAsync(luaRef, text, description, styleRuns, hasTriggerContext,
	                                                replaceMatchedLineOutput, triggerMatchedLineBufferIndex,
	                                                triggerMatchedLineAbsoluteNumber);
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
			const QString encoded = QMudCommandQueue::encodeQueueEntry(strLine, queueIt, bEcho, logIt);
			if (const bool triggerPriority = !queueIt && m_triggerCommandPriorityDepth > 0; triggerPriority)
			{
				const int insertAt =
				    qBound(0, m_triggerPriorityQueuedCommands, safeQSizeToInt(m_queuedCommands.size()));
				m_queuedCommands.insert(insertAt, encoded);
				m_triggerPriorityQueuedCommands = insertAt + 1;
			}
			else
			{
				m_queuedCommands.append(encoded);
			}

		} // end of having a speedwalk delay
		else
		{
			const bool triggerPriority = !queueIt && m_triggerCommandPriorityDepth > 0;
			if (m_runtime && m_runtime->deferInboundImmediateCommand(strLine, bEcho, logIt, triggerPriority))
				continue;
			doSendMsg(strLine, bEcho, logIt); // just send it
		}
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
		QByteArray payload = m_utf8 ? str.toUtf8() : qmudEncodeWorldText(str, m_legacyEncodingName, nullptr);
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

	bool              sentAny         = false;
	const int         queuedBefore    = safeQSizeToInt(m_queuedCommands.size());
	const QStringList batch           = QMudCommandQueue::takeDispatchBatch(m_queuedCommands, flushAll);
	const int         removedFromHead = queuedBefore - safeQSizeToInt(m_queuedCommands.size());
	m_triggerPriorityQueuedCommands   = qMax(0, m_triggerPriorityQueuedCommands - removedFromHead);
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
	if (m_queuedCommands.isEmpty())
		m_triggerPriorityQueuedCommands = 0;

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
		if (!m_queueStatusOwnsMessage)
			return;
		m_queueStatusOwnsMessage = false;
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
	m_queueStatusOwnsMessage = true;
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
