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
#include "LuaCallbackEngine.h"
#include "MainWindowHost.h"
#include "MainWindowHostResolver.h"
#include "NativePluginRegistry.h"
#include "SpeedwalkParser.h"
#include "TimerSchedulingUtils.h"
#include "WorldCommandProcessorUtils.h"
#include "WorldOptions.h"
#include "WorldRuleEnableUtils.h"
#include "WorldRuntime.h"
#include "WorldView.h"
#include "helpers/ColorUtils.h"
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
#include <QScopedValueRollback>
#include <QUrl>
#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
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

/**
 * @brief Narrow internal access used by automation execution and one-shot removal.
 *
 * Collection references remain private in WorldRuntime; every mutation performed through this accessor is paired
 * with the processor's definition/runtime-state publication guards.
 */
class WorldCommandProcessorMutationAccess final
{
	public:
		static QList<WorldRuntime::Trigger> &triggers(WorldRuntime &runtime)
		{
			return runtime.triggersMutable();
		}
		static QList<WorldRuntime::Alias> &aliases(WorldRuntime &runtime)
		{
			return runtime.aliasesMutable();
		}
		static QList<WorldRuntime::Timer> &timers(WorldRuntime &runtime)
		{
			return runtime.timersMutable();
		}
		static QList<WorldRuntime::Plugin> &plugins(WorldRuntime &runtime)
		{
			return runtime.pluginsMutable();
		}
		static WorldRuntime::Plugin *plugin(WorldRuntime &runtime, const QString &pluginId)
		{
			return runtime.pluginForIdMutable(pluginId);
		}
		static WorldRuntime::Plugin *pluginForLocation(WorldRuntime &runtime, const quint64 locationHandle,
		                                               const quint64 instanceId, const int indexHint = -1,
		                                               int *resolvedIndex = nullptr)
		{
			return runtime.pluginForRuntimeLocation(locationHandle, instanceId, indexHint, resolvedIndex);
		}
		static void synchronizeAliases(WorldRuntime &runtime, WorldRuntime::Plugin *plugin,
		                               const int firstChangedIndex)
		{
			runtime.synchronizeAliasRuntimeLocations(plugin, firstChangedIndex);
		}
		static void synchronizeTriggers(WorldRuntime &runtime, WorldRuntime::Plugin *plugin,
		                                const int firstChangedIndex)
		{
			runtime.synchronizeTriggerRuntimeLocations(plugin, firstChangedIndex);
		}
		static void synchronizeTimers(WorldRuntime &runtime, WorldRuntime::Plugin *plugin,
		                              const int firstChangedIndex)
		{
			runtime.synchronizeTimerRuntimeLocations(plugin, firstChangedIndex);
		}
		static void invalidateLocation(WorldRuntime &runtime, const quint64 locationHandle)
		{
			runtime.invalidateRuntimeLocation(locationHandle);
		}
};

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
			else if (groupValue == QMudColourGroup::kCustom && index < palette.customText.size())
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
		WorldRuntime::StyleSpan fallback;
		fallback.fore = fore;
		fallback.back = back;
		return QMudOutputWrapUtils::normalizeStyleSpansForText(line, spans, fallback);
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
			run.style                             = style;
			WorldRuntime::StyleSpan presentedSpan = span;
			presentedSpan.fore                    = fore;
			presentedSpan.back                    = back;
			run.sourceSpans.push_back(QMudLuaCallbackLineSnapshot::fromStyleSpan(presentedSpan));
			if (!runs.isEmpty())
			{
				LuaStyleRun &previous = runs.last();
				if (previous.textColour == run.textColour && previous.backColour == run.backColour &&
				    previous.style == run.style)
				{
					previous.text += run.text;
					previous.sourceSpans += run.sourceSpans;
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
		return !plugin.attributes.value(QStringLiteral("id")).isEmpty();
	}

	QString pluginIdOf(const WorldRuntime::Plugin *plugin)
	{
		if (!plugin)
			return {};
		const QString pluginId = plugin->attributes.value(QStringLiteral("id"));
		return pluginId;
	}

	WorldRuntime::Plugin *resolveCapturedPlugin(WorldRuntime *runtime, const QString &pluginId)
	{
		if (!runtime || pluginId.isEmpty())
			return nullptr;
		return WorldCommandProcessorMutationAccess::plugin(*runtime, pluginId);
	}

	WorldRuntime::Alias *resolveAliasByRuntimeId(WorldRuntime *runtime, const quint64 runtimeLocationHandle,
	                                             const quint64 runtimeId, const int indexHint)
	{
		return runtime ? runtime->aliasForRuntimeLocation(runtimeLocationHandle, runtimeId, indexHint)
		               : nullptr;
	}

	WorldRuntime::Trigger *resolveTriggerByRuntimeId(WorldRuntime *runtime,
	                                                 const quint64 runtimeLocationHandle,
	                                                 const quint64 runtimeId, const int indexHint = -1)
	{
		return runtime ? runtime->triggerForRuntimeLocation(runtimeLocationHandle, runtimeId, indexHint)
		               : nullptr;
	}

	WorldRuntime::Timer *resolveTimerByRuntimeId(WorldRuntime *runtime, const quint64 runtimeLocationHandle,
	                                             const quint64 runtimeId, const int indexHint = -1)
	{
		return runtime ? runtime->timerForRuntimeLocation(runtimeLocationHandle, runtimeId, indexHint)
		               : nullptr;
	}

	bool removeAliasByRuntimeId(WorldRuntime *runtime, const quint64 runtimeLocationHandle,
	                            const quint64 runtimeId, const QString &pluginId,
	                            const quint64 pluginRuntimeLocationHandle,
	                            const quint64 pluginRuntimeInstanceId, const int pluginIndexHint,
	                            const int indexHint)
	{
		if (!runtime || runtimeId == 0)
			return false;
		QList<WorldRuntime::Alias> *aliases = &WorldCommandProcessorMutationAccess::aliases(*runtime);
		WorldRuntime::Plugin       *owner   = nullptr;
		if (!pluginId.isEmpty())
		{
			owner = WorldCommandProcessorMutationAccess::pluginForLocation(
			    *runtime, pluginRuntimeLocationHandle, pluginRuntimeInstanceId, pluginIndexHint);
			if (!owner)
				return false;
			aliases = &owner->aliases;
		}
		int resolvedIndex = -1;
		if (!runtime->aliasForRuntimeLocation(runtimeLocationHandle, runtimeId, indexHint, &resolvedIndex))
			return false;
		WorldCommandProcessorMutationAccess::invalidateLocation(*runtime, runtimeLocationHandle);
		aliases->removeAt(resolvedIndex);
		WorldCommandProcessorMutationAccess::synchronizeAliases(*runtime, owner, resolvedIndex);
		return true;
	}

	bool removeTriggerByRuntimeId(WorldRuntime *runtime, const quint64 runtimeLocationHandle,
	                              const quint64 runtimeId, const QString &pluginId,
	                              const quint64 pluginRuntimeLocationHandle,
	                              const quint64 pluginRuntimeInstanceId, const int pluginIndexHint,
	                              const int indexHint)
	{
		if (!runtime || runtimeId == 0)
			return false;
		QList<WorldRuntime::Trigger> *triggers = &WorldCommandProcessorMutationAccess::triggers(*runtime);
		WorldRuntime::Plugin         *owner    = nullptr;
		if (!pluginId.isEmpty())
		{
			owner = WorldCommandProcessorMutationAccess::pluginForLocation(
			    *runtime, pluginRuntimeLocationHandle, pluginRuntimeInstanceId, pluginIndexHint);
			if (!owner)
				return false;
			triggers = &owner->triggers;
		}
		int resolvedIndex = -1;
		if (!runtime->triggerForRuntimeLocation(runtimeLocationHandle, runtimeId, indexHint, &resolvedIndex))
			return false;
		WorldCommandProcessorMutationAccess::invalidateLocation(*runtime, runtimeLocationHandle);
		triggers->removeAt(resolvedIndex);
		WorldCommandProcessorMutationAccess::synchronizeTriggers(*runtime, owner, resolvedIndex);
		return true;
	}

	bool removeTimerByRuntimeId(WorldRuntime *runtime, const quint64 runtimeLocationHandle,
	                            const quint64 runtimeId, const QString &pluginId,
	                            const quint64 pluginRuntimeLocationHandle,
	                            const quint64 pluginRuntimeInstanceId, const int pluginIndexHint,
	                            const int indexHint)
	{
		if (!runtime || runtimeId == 0)
			return false;
		QList<WorldRuntime::Timer> *timers = &WorldCommandProcessorMutationAccess::timers(*runtime);
		WorldRuntime::Plugin       *owner  = nullptr;
		if (!pluginId.isEmpty())
		{
			owner = WorldCommandProcessorMutationAccess::pluginForLocation(
			    *runtime, pluginRuntimeLocationHandle, pluginRuntimeInstanceId, pluginIndexHint);
			if (!owner)
				return false;
			timers = &owner->timers;
		}
		int resolvedIndex = -1;
		if (!runtime->timerForRuntimeLocation(runtimeLocationHandle, runtimeId, indexHint, &resolvedIndex))
			return false;
		WorldCommandProcessorMutationAccess::invalidateLocation(*runtime, runtimeLocationHandle);
		timers->removeAt(resolvedIndex);
		WorldCommandProcessorMutationAccess::synchronizeTimers(*runtime, owner, resolvedIndex);
		return true;
	}

	class AliasExecutionScope final
	{
		public:
			AliasExecutionScope(WorldRuntime *runtime, const quint64 runtimeLocationHandle,
			                    const quint64 runtimeId, QString pluginId, const bool countInvocation,
			                    const int indexHint = -1)
			    : m_runtime(runtime), m_pluginId(std::move(pluginId)),
			      m_runtimeLocationHandle(runtimeLocationHandle), m_ruleRuntimeId(runtimeId),
			      m_indexHint(indexHint), m_countInvocation(countInvocation)
			{
				if (!runtime)
					return;
				WorldRuntime::Alias *alias =
				    resolveAliasByRuntimeId(runtime, m_runtimeLocationHandle, m_ruleRuntimeId, m_indexHint);
				if (!alias)
					return;
				alias->executingScriptDepth++;
				alias->executingScript = true;
				m_active               = true;
				runtime->markAliasRuntimeStateChanged(m_pluginId, m_runtimeLocationHandle, m_ruleRuntimeId,
				                                      m_indexHint, LuaCallbackRuleSnapshotPart::RuntimeState);
			}

			~AliasExecutionScope()
			{
				if (!m_active || !m_runtime)
					return;
				if (WorldRuntime::Alias *resolved = resolveAliasByRuntimeId(
				        m_runtime.data(), m_runtimeLocationHandle, m_ruleRuntimeId, m_indexHint))
				{
					if (resolved->executingScriptDepth > 0)
						resolved->executingScriptDepth--;
					resolved->executingScript = resolved->executingScriptDepth > 0;
					if (m_countInvocation)
						resolved->invocationCount++;
					m_runtime->markAliasRuntimeStateChanged(m_pluginId, m_runtimeLocationHandle,
					                                        m_ruleRuntimeId, m_indexHint,
					                                        LuaCallbackRuleSnapshotPart::RuntimeState);
				}
			}

			AliasExecutionScope(const AliasExecutionScope &)            = delete;
			AliasExecutionScope &operator=(const AliasExecutionScope &) = delete;

		private:
			QPointer<WorldRuntime> m_runtime;
			QString                m_pluginId;
			quint64                m_runtimeLocationHandle{0};
			quint64                m_ruleRuntimeId{0};
			int                    m_indexHint{-1};
			bool                   m_countInvocation{false};
			bool                   m_active{false};
	};

	class TriggerExecutionScope final
	{
		public:
			TriggerExecutionScope(WorldRuntime *runtime, const quint64 runtimeLocationHandle,
			                      const quint64 runtimeId, QString pluginId, const bool countInvocation,
			                      const int indexHint = -1)
			    : m_runtime(runtime), m_pluginId(std::move(pluginId)), m_ruleRuntimeId(runtimeId),
			      m_runtimeLocationHandle(runtimeLocationHandle), m_indexHint(indexHint),
			      m_countInvocation(countInvocation)
			{
				if (!runtime)
					return;
				WorldRuntime::Trigger *trigger =
				    resolveTriggerByRuntimeId(runtime, m_runtimeLocationHandle, m_ruleRuntimeId, m_indexHint);
				if (!trigger)
					return;
				trigger->executingScriptDepth++;
				trigger->executingScript = true;
				m_active                 = true;
				runtime->markTriggerRuntimeStateChanged(m_pluginId, m_runtimeLocationHandle, m_ruleRuntimeId,
				                                        m_indexHint,
				                                        LuaCallbackRuleSnapshotPart::RuntimeState);
			}

			~TriggerExecutionScope()
			{
				if (!m_active || !m_runtime)
					return;
				if (WorldRuntime::Trigger *resolved = resolveTriggerByRuntimeId(
				        m_runtime.data(), m_runtimeLocationHandle, m_ruleRuntimeId, m_indexHint))
				{
					if (resolved->executingScriptDepth > 0)
						resolved->executingScriptDepth--;
					resolved->executingScript = resolved->executingScriptDepth > 0;
					if (m_countInvocation)
						resolved->invocationCount++;
					m_runtime->markTriggerRuntimeStateChanged(m_pluginId, m_runtimeLocationHandle,
					                                          m_ruleRuntimeId, m_indexHint,
					                                          LuaCallbackRuleSnapshotPart::RuntimeState);
				}
			}

			TriggerExecutionScope(const TriggerExecutionScope &)            = delete;
			TriggerExecutionScope &operator=(const TriggerExecutionScope &) = delete;

		private:
			QPointer<WorldRuntime> m_runtime;
			QString                m_pluginId;
			quint64                m_ruleRuntimeId{0};
			quint64                m_runtimeLocationHandle{0};
			int                    m_indexHint{-1};
			bool                   m_countInvocation{false};
			bool                   m_active{false};
	};

	class TimerExecutionScope final
	{
		public:
			TimerExecutionScope(WorldRuntime *runtime, const quint64 runtimeLocationHandle,
			                    const quint64 runtimeId, QString pluginId, const bool countInvocation,
			                    const int indexHint = -1)
			    : m_runtime(runtime), m_pluginId(std::move(pluginId)),
			      m_runtimeLocationHandle(runtimeLocationHandle), m_ruleRuntimeId(runtimeId),
			      m_indexHint(indexHint), m_countInvocation(countInvocation)
			{
				if (!runtime)
					return;
				WorldRuntime::Timer *timer =
				    resolveTimerByRuntimeId(runtime, m_runtimeLocationHandle, m_ruleRuntimeId, m_indexHint);
				if (!timer)
					return;
				timer->executingScriptDepth++;
				timer->executingScript = true;
				m_active               = true;
				runtime->markTimerRuntimeStateChanged(m_pluginId, m_runtimeLocationHandle, m_ruleRuntimeId,
				                                      m_indexHint, LuaCallbackRuleSnapshotPart::RuntimeState);
			}

			~TimerExecutionScope()
			{
				if (!m_active || !m_runtime)
					return;
				if (WorldRuntime::Timer *resolved = resolveTimerByRuntimeId(
				        m_runtime.data(), m_runtimeLocationHandle, m_ruleRuntimeId, m_indexHint))
				{
					if (resolved->executingScriptDepth > 0)
						resolved->executingScriptDepth--;
					resolved->executingScript = resolved->executingScriptDepth > 0;
					if (m_countInvocation)
						resolved->invocationCount++;
					m_runtime->markTimerRuntimeStateChanged(m_pluginId, m_runtimeLocationHandle,
					                                        m_ruleRuntimeId, m_indexHint,
					                                        LuaCallbackRuleSnapshotPart::RuntimeState);
				}
			}

			TimerExecutionScope(const TimerExecutionScope &)            = delete;
			TimerExecutionScope &operator=(const TimerExecutionScope &) = delete;

		private:
			QPointer<WorldRuntime> m_runtime;
			QString                m_pluginId;
			quint64                m_runtimeLocationHandle{0};
			quint64                m_ruleRuntimeId{0};
			int                    m_indexHint{-1};
			bool                   m_countInvocation{false};
			bool                   m_active{false};
	};
} // namespace

struct WorldCommandProcessor::LogicalOperationCompletion
{
		QPointer<WorldCommandProcessor> processor;
		LogicalOperationCompletionPtr   parent;
		quint64                         inputCriticalOperationId{0};
		int                             pending{1};
		bool                            succeeded{true};
		bool                            completed{false};
		bool                            inputCriticalCompletionRequested{false};
		bool                            inputCriticalFinalized{false};
		CommandCompletion               completion;

		void                            retain()
		{
			Q_ASSERT(!completed);
			if (!completed)
				++pending;
		}

		void release(const bool operationSucceeded)
		{
			Q_ASSERT(!completed);
			Q_ASSERT(pending > 0);
			if (completed || pending <= 0)
				return;
			succeeded = succeeded && operationSucceeded;
			--pending;
			if (pending != 0)
				return;
			completed                                      = true;
			LogicalOperationCompletionPtr parentCompletion = std::move(parent);
			if (processor && processor->m_activeOperationCompletion.data() == this)
				processor->m_activeOperationCompletion = parentCompletion;
			if (auto finalCompletion = std::move(completion); finalCompletion)
			{
				finalCompletion(succeeded);
				if (parentCompletion)
					parentCompletion->release(succeeded);
			}
			else if (parentCompletion)
				parentCompletion->release(succeeded);
		}
};

WorldCommandProcessor::LogicalOperationCompletionPtr
WorldCommandProcessor::createLogicalOperationCompletion(CommandCompletion completion,
                                                        const bool        joinActiveOperation)
{
	auto group        = QSharedPointer<LogicalOperationCompletion>::create();
	group->processor  = this;
	group->completion = std::move(completion);
	if (!joinActiveOperation || !m_activeOperationCompletion)
		return group;

	group->parent = m_activeOperationCompletion;
	group->parent->retain();
	return group;
}

struct WorldCommandProcessor::PendingPluginCommandEvaluation final : LuaCallbackResultContinuation
{
		QPointer<WorldCommandProcessor> processor;
		QString                         line;
		bool                            echoInput{false};
		CommandCompletion               completion;

		void                            resume(LuaBatchDispatchResult &&result) override
		{
			if (!processor)
			{
				if (completion)
					completion(false);
				return;
			}
			if (const bool accepted = result.boolResultValid ? result.boolResult : true; accepted)
				static_cast<void>(processor->continueEvaluateCommandAfterPluginCommand(line, echoInput));
			if (completion)
				completion(true);
		}

		void cancel(LuaBatchDispatchResult && /*fallback*/) override
		{
			if (completion)
				completion(false);
		}
};

struct WorldCommandProcessor::PendingPluginCommandEntered final : LuaCallbackResultContinuation
{
		QPointer<WorldCommandProcessor> processor;
		QString                         originalText;

		void                            resume(LuaBatchDispatchResult &&result) override
		{
			if (!processor)
				return;
			processor->continueCommandEnteredAfterPluginTransform(
			    result.stringResult.isNull() ? originalText : result.stringResult);
		}

		void cancel(LuaBatchDispatchResult && /*fallback*/) override
		{
			if (processor)
				processor->cancelActiveEnteredCommand();
		}
};

struct WorldCommandProcessor::PendingPluginLineDecision final : LuaCallbackResultContinuation
{
		QSharedPointer<PendingIncomingLineOperation> operation;

		void                                         resume(LuaBatchDispatchResult &&result) override
		{
			if (!operation || !operation->processor)
				return;
			const bool accepted = result.boolResultValid ? result.boolResult : true;
			const QPointer<WorldCommandProcessor> processor  = operation->processor;
			const auto                            resumeLine = [operation = operation, accepted]
			{
				if (operation->processor)
					operation->processor->continueIncomingLineAfterPluginDecision(operation, accepted);
			};
			processor->runWithInputCriticalOperationScope(operation->inputCriticalOperationScope, resumeLine);
		}

		void cancel(LuaBatchDispatchResult && /*fallback*/) override
		{
			if (!operation || !operation->processor)
				return;
			const QPointer<WorldCommandProcessor> processor  = operation->processor;
			const auto                            cancelLine = [operation = operation]
			{
				if (operation->processor)
					operation->processor->cancelIncomingLine(operation);
			};
			processor->runWithInputCriticalOperationScope(operation->inputCriticalOperationScope, cancelLine);
		}
};

struct WorldCommandProcessor::PendingPluginSendDecision final : LuaCallbackResultContinuation
{
		QPointer<WorldCommandProcessor> processor;
		QPointer<WorldRuntime>          runtime;
		QString                         text;
		bool                            echo{false};
		bool                            logIt{false};
		OutboundTransport               transport{OutboundTransport::Immediate};
		std::function<void(bool)>       mutationCompletion;
		LogicalOperationCompletionPtr   operationCompletion;
		bool                            bypassSpamPrevention{false};

		void                            resume(LuaBatchDispatchResult &&result) override
		{
			if (!processor || processor->m_runtime != runtime)
			{
				if (mutationCompletion)
					mutationCompletion(true);
				if (operationCompletion)
					operationCompletion->release(false);
				return;
			}
			const bool accepted = result.boolResultValid ? result.boolResult : true;
			processor->completePluginSendDecision(text, echo, logIt, transport, accepted,
			                                      std::move(mutationCompletion),
			                                      std::move(operationCompletion), bypassSpamPrevention);
		}

		void cancel(LuaBatchDispatchResult && /*fallback*/) override
		{
			if (!processor || processor->m_runtime != runtime)
			{
				if (mutationCompletion)
					mutationCompletion(true);
				if (operationCompletion)
					operationCompletion->release(false);
				return;
			}
			processor->cancelPluginSendDecision(mutationCompletion, operationCompletion);
		}
};

struct WorldCommandProcessor::PendingOutboundCommand
{
		QPointer<WorldCommandProcessor>     processor;
		QPointer<WorldRuntime>              runtime;
		QString                             text;
		bool                                echo{false};
		bool                                logIt{false};
		LuaBatchDispatchRequest             sentRequest;
		std::function<void(bool)>           mutationCompletion;
		LogicalOperationCompletionPtr       operationCompletion;
		QSharedPointer<CommandHistoryState> historyState;
		SpamCommitState                     spamCommitState;
		bool                                settled{false};

		/**
		 * @brief Releases retained command work when its processor no longer exists.
		 * @param transaction Physical-write transaction whose notification must be discarded.
		 */
		void settleWithoutProcessor(const QSharedPointer<OutboundCommandTransaction> &transaction)
		{
			if (settled)
				return;
			settled = true;
			if (transaction && transaction->outcome == OutboundCommandTransaction::Outcome::Rejected &&
			    historyState)
			{
				historyState->sendFailed = true;
			}
			if (runtime)
				runtime->discardOutboundCommandNotification(transaction);
			else if (transaction)
			{
				transaction->notificationFinished = true;
				transaction->discardPacketDebug   = true;
				if (auto causalCompletion = std::move(transaction->causalCompletion); causalCompletion)
					causalCompletion();
			}
			if (auto completion = std::move(mutationCompletion); completion)
				completion(true);
			if (auto completion = std::move(operationCompletion); completion)
				completion->release(false);
		}
};

struct WorldCommandProcessor::PendingPluginSentNotification final : LuaCallbackResultContinuation
{
		std::shared_ptr<PendingOutboundCommand>    command;
		QSharedPointer<OutboundCommandTransaction> transaction;

		void finish(const AcceptedOutboundCompletionMode completionMode =
		                AcceptedOutboundCompletionMode::ContinueOriginatingWork) const
		{
			if (!command || !command->processor)
			{
				if (command)
					command->settleWithoutProcessor(transaction);
				return;
			}
			command->processor->completePluginSentNotification(command, transaction, completionMode);
		}

		void resume(LuaBatchDispatchResult && /*result*/) override
		{
			finish();
		}

		void cancel(LuaBatchDispatchResult && /*fallback*/) override
		{
			if (command && command->processor && command->runtime &&
			    command->runtime->m_runtimeDestructionStarted)
			{
				command->processor->rejectOutboundCommand(command, transaction);
				return;
			}
			const AcceptedOutboundCompletionMode completionMode =
			    command && command->processor && command->processor->m_outboundBindingCancellationActive
			        ? AcceptedOutboundCompletionMode::CancelOriginatingWork
			        : AcceptedOutboundCompletionMode::ContinueOriginatingWork;
			finish(completionMode);
		}
};

struct WorldCommandProcessor::CommandEvaluationSequence final
    : std::enable_shared_from_this<WorldCommandProcessor::CommandEvaluationSequence>
{
		QPointer<WorldCommandProcessor> processor;
		QStringList                     commands;
		qsizetype                       nextIndex{0};
		CommandCompletion               completion;
		bool                            autoSayEvaluation{false};
		CallerContextRunner             callerContext;
		bool                            advancing{false};
		bool                            advanceRequested{false};
		bool                            finished{false};

		void                            commandCompleted(const bool completed)
		{
			if (finished)
				return;
			if (!completed)
			{
				finished = true;
				if (auto finalCompletion = std::move(completion); finalCompletion)
					finalCompletion(false);
				return;
			}
			++nextIndex;
			if (advancing)
			{
				advanceRequested = true;
				return;
			}
			advance();
		}

		void advance()
		{
			if (finished)
				return;
			if (advancing)
			{
				advanceRequested = true;
				return;
			}
			advancing                  = true;
			const auto finishAdvancing = qScopeGuard([this] { advancing = false; });
			Q_UNUSED(finishAdvancing);
			for (;;)
			{
				advanceRequested = false;
				if (!processor || nextIndex >= commands.size())
				{
					finished = true;
					if (auto finalCompletion = std::move(completion); finalCompletion)
						finalCompletion(true);
					return;
				}

				const qsizetype startingIndex = nextIndex;
				const auto      self          = shared_from_this();
				const auto      evaluate      = [self, this]
				{
					if (processor)
						static_cast<void>(processor->evaluateCommand(commands.at(nextIndex), false,
						                                             [self](const bool completed)
						                                             { self->commandCompleted(completed); }));
				};
				if (callerContext)
					callerContext.run(evaluate);
				else
					evaluate();
				if (nextIndex == startingIndex)
					return;
				if (!advanceRequested)
					return;
			}
		}
};

struct WorldCommandProcessor::AliasEvaluationSequence final
    : std::enable_shared_from_this<WorldCommandProcessor::AliasEvaluationSequence>
{
		struct Scope
		{
				QString pluginId;
				quint64 engineInstanceId{0};
				quint64 pluginRuntimeInstanceId{0};
				quint64 pluginRuntimeLocationHandle{0};
				int     pluginIndexHint{-1};
		};

		enum class Phase
		{
			Aliases,
			Routines,
			Finished
		};

		enum class PendingAction
		{
			None,
			AliasBody,
			AliasRoutine
		};

		QPointer<WorldCommandProcessor>        processor;
		QPointer<WorldRuntime>                 runtime;
		QString                                line;
		bool                                   echoInput{false};
		bool                                   echoAlias{false};
		bool                                   omitFromLog{false};
		bool                                   omitFromHistory{false};
		QVector<Scope>                         scopes;
		qsizetype                              scopeIndex{0};
		QVector<AliasEvaluationPlanEntry>      plan;
		qsizetype                              planIndex{0};
		bool                                   scopeMatched{false};
		bool                                   anyAliasMatched{false};
		QVector<AliasRef>                      matchedAliases;
		QVector<AliasRef>                      oneShotAliases;
		QVector<quint64>                       reservedOneShotRuntimeIds;
		qsizetype                              routineIndex{0};
		QHash<QString, bool>                   worldFunctionPresence;
		Phase                                  phase{Phase::Aliases};
		PendingAction                          pendingAction{PendingAction::None};
		quint64                                pendingAliasRuntimeId{0};
		quint64                                pendingAliasRuntimeLocationHandle{0};
		QString                                pendingPluginId;
		int                                    pendingIndexHint{-1};
		bool                                   pendingKeepEvaluating{false};
		std::unique_ptr<AliasExecutionScope>   executionScope;
		QVector<LuaCallbackRuleSnapshotChange> changedAliasRuntimeLocations;
		bool                                   fullAliasRuntimeStatePublicationRequired{false};
		quint64                                generation{0};
		CallerContextRunner                    callerContext;
		bool                                   advancing{false};
		bool                                   advanceRequested{false};

		~AliasEvaluationSequence()
		{
			if (!processor)
				return;
			for (const quint64 runtimeId : std::as_const(reservedOneShotRuntimeIds))
				processor->m_reservedOneShotAliasRuntimeIds.remove(runtimeId);
		}

		[[nodiscard]] bool scopeStillCurrent(const Scope &scope) const
		{
			if (!runtime)
				return false;
			if (scope.pluginId.isEmpty())
				return true;
			const WorldRuntime::Plugin *plugin = WorldCommandProcessorMutationAccess::pluginForLocation(
			    *runtime, scope.pluginRuntimeLocationHandle, scope.pluginRuntimeInstanceId,
			    scope.pluginIndexHint);
			if (!plugin || !plugin->enabled || plugin->installPending)
				return false;
			const quint64 currentInstanceId = plugin->lua ? plugin->lua->instanceId() : 0;
			return currentInstanceId == scope.engineInstanceId;
		}

		[[nodiscard]] static DecodedAlias decodeAlias(const WorldRuntime::Alias &alias, const int index)
		{
			const QMap<QString, QString> &attrs = alias.attributes;
			DecodedAlias                  decoded;
			decoded.index        = index;
			decoded.sequence     = attrs.value(QStringLiteral("sequence")).toInt();
			decoded.matchText    = attrs.value(QStringLiteral("match"));
			decoded.sendText     = alias.children.value(QStringLiteral("send"));
			decoded.label        = attrs.value(QStringLiteral("name"));
			decoded.scriptLabel  = decoded.label.isEmpty() ? decoded.matchText : decoded.label;
			decoded.variableName = attrs.value(QStringLiteral("variable"));
			decoded.scriptName   = attrs.value(QStringLiteral("script"));
			decoded.enabled      = isEnabledValue(attrs.value(QStringLiteral("enabled")));
			decoded.isRegexp     = isEnabledValue(attrs.value(QStringLiteral("regexp")));
			decoded.ignoreCase   = isEnabledValue(attrs.value(QStringLiteral("ignore_case")));
			decoded.omitCommandHistory =
			    isEnabledValue(attrs.value(QStringLiteral("omit_from_command_history")));
			decoded.echoMatchedAlias = isEnabledValue(attrs.value(QStringLiteral("echo_alias")));
			decoded.expandVariables  = isEnabledValue(attrs.value(QStringLiteral("expand_variables")));
			decoded.omitFromOutput   = isEnabledValue(attrs.value(QStringLiteral("omit_from_output")));
			decoded.omitFromLog      = isEnabledValue(attrs.value(QStringLiteral("omit_from_log")));
			decoded.oneShot          = isEnabledValue(attrs.value(QStringLiteral("one_shot")));
			decoded.keepEvaluating   = isEnabledValue(attrs.value(QStringLiteral("keep_evaluating")));
			decoded.sendToValue      = attrs.value(QStringLiteral("send_to")).toInt();
			return decoded;
		}

		void recordRuntimeStateChange(const quint64 runtimeLocationHandle, const quint64 runtimeId,
		                              const int indexHint, const LuaCallbackRuleSnapshotParts parts)
		{
			if (runtimeLocationHandle == 0 || runtimeId == 0)
				fullAliasRuntimeStatePublicationRequired = true;
			else if (!changedAliasRuntimeLocations.isEmpty() &&
			         changedAliasRuntimeLocations.constLast().runtimeLocationHandle ==
			             runtimeLocationHandle &&
			         changedAliasRuntimeLocations.constLast().runtimeId == runtimeId)
			{
				changedAliasRuntimeLocations.last().indexHint = indexHint;
				changedAliasRuntimeLocations.last().parts |= parts;
			}
			else
			{
				changedAliasRuntimeLocations.push_back({runtimeLocationHandle, runtimeId, indexHint, parts});
			}
		}

		void publishRuntimeStateChanges()
		{
			if ((!fullAliasRuntimeStatePublicationRequired && changedAliasRuntimeLocations.isEmpty()) ||
			    !runtime || scopeIndex >= scopes.size())
				return;
			const QString &pluginId = scopes.at(scopeIndex).pluginId;
			if (fullAliasRuntimeStatePublicationRequired)
				runtime->markAliasRuntimeStateChanged(pluginId);
			else
				runtime->markAliasRuntimeStatesChanged(pluginId, changedAliasRuntimeLocations);
			changedAliasRuntimeLocations.clear();
			fullAliasRuntimeStatePublicationRequired = false;
		}

		void start()
		{
			if (!processor || !runtime)
				return;
			generation                           = processor->m_ruleEvaluationGeneration;
			echoAlias                            = echoInput;
			QList<WorldRuntime::Plugin> &plugins = WorldCommandProcessorMutationAccess::plugins(*runtime);
			QVector<Scope>               negative;
			QVector<Scope>               nonNegative;
			for (const int pluginIndex : processor->sortedPluginIndices())
			{
				if (pluginIndex < 0 || pluginIndex >= plugins.size())
					continue;
				const WorldRuntime::Plugin &plugin = plugins.at(pluginIndex);
				if (!pluginHasValidId(plugin))
					continue;
				Scope scope{pluginIdOf(&plugin), plugin.lua ? plugin.lua->instanceId() : 0,
				            plugin.runtimeInstanceId, plugin.runtimeLocationHandle, pluginIndex};
				if (plugin.sequence < 0)
					negative.push_back(std::move(scope));
				else
					nonNegative.push_back(std::move(scope));
			}
			scopes = std::move(negative);
			if (shouldEvaluateRuleCollection(runtime->worldAttributes(), WorldRuleKind::Alias, false))
				scopes.push_back({});
			scopes += nonNegative;
			callerContext = processor->captureCallerContextRunner();
			advance();
		}

		bool prepareScope()
		{
			plan.clear();
			planIndex    = 0;
			scopeMatched = false;
			while (scopeIndex < scopes.size())
			{
				const Scope &scope = scopes.at(scopeIndex);
				if (!scopeStillCurrent(scope))
				{
					++scopeIndex;
					continue;
				}
				WorldRuntime::Plugin *plugin =
				    scope.pluginId.isEmpty() ? nullptr
				                             : WorldCommandProcessorMutationAccess::pluginForLocation(
				                                   *runtime, scope.pluginRuntimeLocationHandle,
				                                   scope.pluginRuntimeInstanceId, scope.pluginIndexHint);
				QList<WorldRuntime::Alias> &aliases =
				    plugin ? plugin->aliases : WorldCommandProcessorMutationAccess::aliases(*runtime);
				const AliasEvaluationCacheEntry &cache = processor->decodedAliasEvaluationCache(aliases);
				plan.reserve(cache.enabledCount);
				for (const DecodedAlias &decoded : cache.aliases)
				{
					if (!decoded.enabled || decoded.index < 0 || decoded.index >= aliases.size())
						continue;
					const WorldRuntime::Alias &alias = aliases.at(decoded.index);
					plan.push_back({decoded, alias.runtimeId, alias.runtimeLocationHandle,
					                alias.executionTimeNs, alias.matchCount, alias.matchAttempts});
				}
				return true;
			}
			return false;
		}

		void orderedActionFinished(const bool completed)
		{
			executionScope.reset();
			if (!completed || !processor || !runtime || generation != processor->m_ruleEvaluationGeneration)
			{
				phase = Phase::Finished;
				return;
			}
			if (pendingAction == PendingAction::AliasBody)
			{
				bool keepEvaluating = pendingKeepEvaluating;
				if (const WorldRuntime::Alias *alias = runtime->aliasForRuntimeLocation(
				        pendingAliasRuntimeLocationHandle, pendingAliasRuntimeId, pendingIndexHint))
				{
					keepEvaluating =
					    isEnabledValue(alias->attributes.value(QStringLiteral("keep_evaluating")));
				}
				if (!keepEvaluating)
					phase = Phase::Routines;
			}
			else if (pendingAction == PendingAction::AliasRoutine)
			{
				++routineIndex;
			}
			pendingAction = PendingAction::None;
			advance();
		}

		void resumeOrderedAction(const bool completed)
		{
			const auto self   = shared_from_this();
			const auto resume = [self, completed] { self->orderedActionFinished(completed); };
			if (callerContext)
				callerContext.run(resume);
			else
				resume();
		}

		void startBody(const DecodedAlias &decoded, const quint64 runtimeLocationHandle,
		               const quint64 runtimeId, const QString &pluginId, const int index,
		               const QString &sendText)
		{
			pendingAction                     = PendingAction::AliasBody;
			pendingAliasRuntimeId             = runtimeId;
			pendingAliasRuntimeLocationHandle = runtimeLocationHandle;
			pendingPluginId                   = pluginId;
			pendingIndexHint                  = index;
			pendingKeepEvaluating             = decoded.keepEvaluating;
			executionScope  = std::make_unique<AliasExecutionScope>(runtime, runtimeLocationHandle, runtimeId,
			                                                        pluginId, false, index);
			const auto self = shared_from_this();
			processor->runOrderedRuleAction(
			    [self, decoded, pluginId, sendText]
			    {
				    if (!self->processor)
					    return;
				    self->processor->sendTo(decoded.sendToValue, sendText, decoded.omitFromOutput,
				                            decoded.omitFromLog, decoded.variableName, decoded.scriptLabel,
				                            pluginId, nullptr, false, false, 0, 0,
				                            LuaCallbackEvent::AliasScript);
			    },
			    [self](const bool completed) { self->resumeOrderedAction(completed); });
		}

		bool processNextAlias()
		{
			if (scopeIndex >= scopes.size())
				return false;
			const Scope &scope = scopes.at(scopeIndex);
			while (planIndex < plan.size())
			{
				const AliasEvaluationPlanEntry planned = plan.at(planIndex++);
				if (!processor->m_reservedOneShotAliasRuntimeIds.isEmpty() &&
				    processor->m_reservedOneShotAliasRuntimeIds.contains(planned.runtimeId))
				{
					continue;
				}
				WorldRuntime::Alias *alias = runtime->aliasForRuntimeLocation(
				    planned.runtimeLocationHandle, planned.runtimeId, planned.decoded.index);
				if (!alias || !scopeStillCurrent(scope))
					continue;
				const DecodedAlias decoded = decodeAlias(*alias, planned.decoded.index);
				if (!decoded.enabled || decoded.matchText.isEmpty())
					continue;
				runtime->incrementAliasesEvaluated();
				if (!alias->lastMatchWildcards.isEmpty())
				{
					alias->lastMatchWildcards.clear();
					recordRuntimeStateChange(planned.runtimeLocationHandle, planned.runtimeId,
					                         planned.decoded.index, LuaCallbackRuleSnapshotPart::DetailState);
				}
				const QString          pattern = decoded.isRegexp
				                                     ? decoded.matchText
				                                     : processor->wildcardToRegexCached(decoded.matchText);
				QStringList            wildcards;
				QMap<QString, QString> namedWildcards;
				qint64                 executionTimeNs  = alias->executionTimeNs;
				int                    matchCount       = alias->matchCount;
				int                    matchAttempts    = alias->matchAttempts;
				const int              previousAttempts = matchAttempts;
				if (!processor->regexMatch(pattern, line, decoded.ignoreCase, wildcards, namedWildcards,
				                           nullptr, nullptr, 0, false, &executionTimeNs, &matchCount,
				                           &matchAttempts))
				{
					if (matchAttempts != previousAttempts)
					{
						alias->executionTimeNs = executionTimeNs;
						alias->matchCount      = matchCount;
						alias->matchAttempts   = matchAttempts;
						recordRuntimeStateChange(planned.runtimeLocationHandle, planned.runtimeId,
						                         planned.decoded.index,
						                         LuaCallbackRuleSnapshotPart::RuntimeState);
					}
					continue;
				}

				const QString language = runtime->worldAttributes().value(QStringLiteral("script_language"));
				QStringList   fixedWildcards = wildcards;
				for (QString &fixed : fixedWildcards)
					fixed = WorldCommandProcessor::fixWildcard(fixed, false, decoded.sendToValue, language);
				QMap<QString, QString> fixedNamed = namedWildcards;
				for (auto it = fixedNamed.begin(); it != fixedNamed.end(); ++it)
					it.value() =
					    WorldCommandProcessor::fixWildcard(it.value(), false, decoded.sendToValue, language);

				scopeMatched           = true;
				anyAliasMatched        = true;
				alias->executionTimeNs = executionTimeNs;
				alias->matchCount      = matchCount;
				alias->matchAttempts   = matchAttempts;
				alias->matched++;
				alias->lastMatchTarget    = line;
				alias->lastMatchWildcards = fixedWildcards;
				alias->lastMatched        = QDateTime::currentDateTime();
				recordRuntimeStateChange(
				    planned.runtimeLocationHandle, planned.runtimeId, planned.decoded.index,
				    LuaCallbackRuleSnapshotPart::RuntimeState | LuaCallbackRuleSnapshotPart::DetailState);
				if (!scope.pluginId.isEmpty())
					runtime->setPluginAliasWildcards(scope.pluginId, decoded.scriptLabel, fixedWildcards,
					                                 fixedNamed);
				else
					runtime->setAliasWildcards(decoded.scriptLabel, fixedWildcards, fixedNamed);
				publishRuntimeStateChanges();
				if (decoded.omitCommandHistory)
					omitFromHistory = true;
				if (const auto &lines = runtime->lines();
				    !lines.isEmpty() && (lines.last().flags & WorldRuntime::LineNote) == 0)
				{
					runtime->outputText(QString(), true, true);
				}
				if (echoAlias && decoded.echoMatchedAlias)
				{
					if (processor->m_view)
						processor->m_view->echoInputText(line);
					echoAlias = false;
					if (isEnabledValue(runtime->worldAttributes().value(QStringLiteral("log_input"))) &&
					    !decoded.omitFromLog)
					{
						processor->logLine(line, QStringLiteral("log_line_preamble_input"),
						                   QStringLiteral("log_line_postamble_input"));
					}
				}
				runtime->incrementAliasesMatched();
				omitFromLog = decoded.omitFromLog;
				processor->emitTrace(decoded.label.isEmpty()
				                         ? QStringLiteral("Matched alias \"%1\"").arg(decoded.matchText)
				                         : QStringLiteral("Matched alias %1").arg(decoded.label));
				bool          ok       = false;
				const QString sendText = processor->fixSendText(
				    WorldCommandProcessor::fixupEscapeSequences(decoded.sendText), decoded.sendToValue,
				    wildcards, namedWildcards, language, false, decoded.expandVariables, true, false, false,
				    true, decoded.scriptLabel, scope.pluginId, &ok);

				if (!ok)
				{
					phase = Phase::Routines;
					return false;
				}
				AliasRef ref;
				ref.runtimeId                   = planned.runtimeId;
				ref.runtimeLocationHandle       = planned.runtimeLocationHandle;
				ref.pluginEngineInstanceId      = scope.engineInstanceId;
				ref.pluginRuntimeInstanceId     = scope.pluginRuntimeInstanceId;
				ref.pluginRuntimeLocationHandle = scope.pluginRuntimeLocationHandle;
				ref.pluginIndexHint             = scope.pluginIndexHint;
				ref.pluginId                    = scope.pluginId;
				ref.label                       = decoded.scriptLabel;
				ref.scriptName                  = decoded.scriptName;
				ref.line                        = line;
				ref.wildcards                   = wildcards;
				ref.namedWildcards              = namedWildcards;
				ref.indexHint                   = planned.decoded.index;
				matchedAliases.push_back(ref);
				if (decoded.oneShot)
				{
					processor->m_reservedOneShotAliasRuntimeIds.insert(planned.runtimeId);
					reservedOneShotRuntimeIds.push_back(planned.runtimeId);
					oneShotAliases.push_back(ref);
				}
				startBody(decoded, planned.runtimeLocationHandle, planned.runtimeId, scope.pluginId,
				          planned.decoded.index, sendText);
				return true;
			}
			if (scopeMatched)
			{
				publishRuntimeStateChanges();
				phase = Phase::Routines;
				return false;
			}
			publishRuntimeStateChanges();
			++scopeIndex;
			return false;
		}

		void startRoutine(const AliasRef &ref, const QSharedPointer<LuaCallbackEngine> &engine)
		{
			pendingAction                     = PendingAction::AliasRoutine;
			pendingAliasRuntimeId             = ref.runtimeId;
			pendingAliasRuntimeLocationHandle = ref.runtimeLocationHandle;
			pendingPluginId                   = ref.pluginId;
			pendingIndexHint                  = ref.indexHint;
			executionScope                    = std::make_unique<AliasExecutionScope>(
			    runtime, ref.runtimeLocationHandle, ref.runtimeId, ref.pluginId, true, ref.indexHint);
			const auto self = shared_from_this();
			processor->runOrderedRuleAction(
			    [self, ref, engine]
			    {
				    if (!self->processor || !self->runtime)
					    return;
				    auto operation = self->processor->m_activeOperationCompletion;
				    if (operation)
					    operation->retain();
				    const CallerContextRunner context = self->processor->captureCallerContextRunner();
				    self->runtime->dispatchLuaStringsAndWildcardsAsync(
				        engine, ref.scriptName, LuaCallbackEvent::AliasRoutine, {ref.label, ref.line},
				        ref.wildcards, ref.namedWildcards, nullptr, -1, false, 0, 0,
				        [self, ref, operation, context](const LuaBatchDispatchResult &result)
				        {
					        const auto complete = [self, ref, operation, result]
					        {
						        if (ref.pluginId.isEmpty() && result.hasFunctionValid)
						        {
							        self->worldFunctionPresence.insert(ref.scriptName, result.hasFunction);
							        if (!result.hasFunction && self->processor)
								        self->processor->warnMissingWorldScriptFunction(
								            QStringLiteral("Alias"), ref.scriptName);
						        }
						        if (operation)
							        operation->release(true);
					        };
					        if (context)
						        context.run(complete);
					        else
						        complete();
				        });
			    },
			    [self](const bool completed) { self->resumeOrderedAction(completed); });
		}

		bool processNextRoutine()
		{
			while (routineIndex < matchedAliases.size())
			{
				AliasRef             ref = matchedAliases.at(routineIndex);
				WorldRuntime::Alias *alias =
				    runtime->aliasForRuntimeLocation(ref.runtimeLocationHandle, ref.runtimeId, ref.indexHint);
				if (!alias)
				{
					++routineIndex;
					continue;
				}
				ref.scriptName             = alias->attributes.value(QStringLiteral("script"));
				const QString currentLabel = alias->attributes.value(QStringLiteral("name"));
				ref.label =
				    currentLabel.isEmpty() ? alias->attributes.value(QStringLiteral("match")) : currentLabel;
				if (ref.scriptName.isEmpty())
				{
					++routineIndex;
					continue;
				}
				QSharedPointer<LuaCallbackEngine> engine;
				if (!ref.pluginId.isEmpty())
				{
					const WorldRuntime::Plugin *plugin =
					    WorldCommandProcessorMutationAccess::pluginForLocation(
					        *runtime, ref.pluginRuntimeLocationHandle, ref.pluginRuntimeInstanceId,
					        ref.pluginIndexHint);
					if (!plugin || !plugin->enabled || plugin->installPending || !plugin->lua ||
					    plugin->lua->instanceId() != ref.pluginEngineInstanceId)
					{
						++routineIndex;
						continue;
					}
					engine = plugin->lua;
				}
				else
				{
					if (const auto presence = worldFunctionPresence.constFind(ref.scriptName);
					    presence != worldFunctionPresence.constEnd() && !presence.value())
					{
						processor->warnMissingWorldScriptFunction(QStringLiteral("Alias"), ref.scriptName);
						++routineIndex;
						continue;
					}
					LuaCallbackEngine *worldLua = runtime->luaCallbacks();
					if (!processor->canExecuteWorldScript(QStringLiteral("Alias"), ref.scriptName, worldLua))
					{
						++routineIndex;
						continue;
					}
					engine =
					    QSharedPointer<LuaCallbackEngine>(worldLua, [](LuaCallbackEngine * /*unused*/) {});
				}
				if (!engine)
				{
					++routineIndex;
					continue;
				}
				startRoutine(ref, engine);
				return true;
			}
			return false;
		}

		void finish()
		{
			phase = Phase::Finished;
			if (!processor || !runtime)
				return;
			if (omitFromHistory && processor->m_activeCommandHistoryState)
				processor->m_activeCommandHistoryState->omitFromHistory = true;
			if (!anyAliasMatched)
			{
				if (QMudNativePluginRegistry::handleMushReaderCommand(runtime, line) ||
				    QMudNativePluginRegistry::handleLuaAudioCommand(runtime, line))
				{
					return;
				}
				QString quitMacroText = QStringLiteral("quit");
				for (const auto &[attributes, children] : runtime->macros())
				{
					if (attributes.value(QStringLiteral("name"))
					        .trimmed()
					        .compare(QStringLiteral("quit"), Qt::CaseInsensitive) != 0)
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
					runtime->setDisconnectOk(true);
				processor->sendMsg(line, echoInput, false, !omitFromLog);
				return;
			}
			if (!oneShotAliases.isEmpty())
			{
				runtime->beginLuaCallbackSnapshotMutationBatch();
				for (const AliasRef &ref : std::as_const(oneShotAliases))
				{
					if (!removeAliasByRuntimeId(runtime, ref.runtimeLocationHandle, ref.runtimeId,
					                            ref.pluginId, ref.pluginRuntimeLocationHandle,
					                            ref.pluginRuntimeInstanceId, ref.pluginIndexHint,
					                            ref.indexHint))
						continue;
					runtime->noteAliasStructureMutation();
					if (ref.pluginId.isEmpty())
						runtime->markAliasesChanged();
					else
						runtime->markPluginAliasesChanged(ref.pluginId);
				}
				runtime->endLuaCallbackSnapshotMutationBatch();
			}
			for (const quint64 runtimeId : std::as_const(reservedOneShotRuntimeIds))
				processor->m_reservedOneShotAliasRuntimeIds.remove(runtimeId);
			reservedOneShotRuntimeIds.clear();
		}

		void advance()
		{
			if (phase == Phase::Finished || !processor || !runtime ||
			    generation != processor->m_ruleEvaluationGeneration)
			{
				phase = Phase::Finished;
				return;
			}
			if (advancing)
			{
				advanceRequested = true;
				return;
			}
			advancing = true;
			QElapsedTimer elapsed;
			elapsed.start();
			const auto stopAdvancing = qScopeGuard([this] { advancing = false; });
			const auto recordElapsed = qScopeGuard(
			    [this, &elapsed]
			    {
				    if (runtime)
					    runtime->finishAliasEvaluationSlice(elapsed.nsecsElapsed());
			    });
			Q_UNUSED(stopAdvancing);
			Q_UNUSED(recordElapsed);
			for (;;)
			{
				advanceRequested = false;
				if (phase == Phase::Aliases)
				{
					if (plan.isEmpty() && planIndex == 0 && !prepareScope())
						phase = Phase::Routines;
					if (phase == Phase::Aliases && processNextAlias())
					{
						if (!advanceRequested)
							return;
						continue;
					}
					if (phase == Phase::Aliases)
					{
						plan.clear();
						planIndex = 0;
						continue;
					}
				}
				if (phase == Phase::Routines)
				{
					if (processNextRoutine())
					{
						if (!advanceRequested)
							return;
						continue;
					}
					finish();
					return;
				}
				return;
			}
		}
};

struct WorldCommandProcessor::TimerEvaluationSequence final
    : std::enable_shared_from_this<WorldCommandProcessor::TimerEvaluationSequence>
{
		struct Entry
		{
				quint64 runtimeId{0};
				quint64 runtimeLocationHandle{0};
				QString pluginId;
				quint64 engineInstanceId{0};
				quint64 pluginRuntimeInstanceId{0};
				quint64 pluginRuntimeLocationHandle{0};
				int     pluginIndexHint{-1};
				int     indexHint{-1};
		};

		enum class PendingAction
		{
			None,
			Body,
			Routine
		};
		enum class AdvanceDisposition
		{
			Ready,
			WaitingForRoutine
		};

		QPointer<WorldCommandProcessor>      processor;
		QPointer<WorldRuntime>               runtime;
		QVector<Entry>                       scopes;
		qsizetype                            scopeIndex{0};
		QVector<Entry>                       entries;
		qsizetype                            nextIndex{0};
		Entry                                current;
		PendingAction                        pendingAction{PendingAction::None};
		std::unique_ptr<TimerExecutionScope> executionScope;
		QDateTime                            evaluationTime;
		bool                                 connected{false};
		quint64                              generation{0};
		bool                                 advancing{false};
		bool                                 advanceRequested{false};
		int                                  completionDepth{0};

		[[nodiscard]] bool                   ownerStillCurrent(const Entry &entry) const
		{
			if (!runtime)
				return false;
			if (entry.pluginId.isEmpty())
				return true;
			const WorldRuntime::Plugin *plugin = WorldCommandProcessorMutationAccess::pluginForLocation(
			    *runtime, entry.pluginRuntimeLocationHandle, entry.pluginRuntimeInstanceId,
			    entry.pluginIndexHint);
			return plugin && plugin->enabled && !plugin->installPending &&
			       (plugin->lua ? plugin->lua->instanceId() : 0) == entry.engineInstanceId;
		}

		bool prepareScope(const Entry &scope)
		{
			WorldRuntime::Plugin *plugin = nullptr;
			if (!scope.pluginId.isEmpty())
			{
				plugin = WorldCommandProcessorMutationAccess::pluginForLocation(
				    *runtime, scope.pluginRuntimeLocationHandle, scope.pluginRuntimeInstanceId,
				    scope.pluginIndexHint);
				if (!plugin || !plugin->enabled || plugin->installPending)
					return false;
			}
			entries.clear();
			nextIndex = 0;
			QList<WorldRuntime::Timer> &timers =
			    plugin ? plugin->timers : WorldCommandProcessorMutationAccess::timers(*runtime);
			QVector<LuaCallbackRuleSnapshotChange> changedRuntimeLocations;
			for (int index = 0; index < timers.size(); ++index)
			{
				WorldRuntime::Timer &timer = timers[index];
				const auto           evaluation =
				    QMudTimerScheduling::evaluateTimerDue(timer, evaluationTime, connected);
				if (evaluation.runtimeStateChanged)
				{
					changedRuntimeLocations.push_back({timer.runtimeLocationHandle, timer.runtimeId, index,
					                                   LuaCallbackRuleSnapshotPart::DetailState});
				}
				if (!evaluation.due || timer.runtimeId == 0)
					continue;
				entries.push_back({timer.runtimeId, timer.runtimeLocationHandle, scope.pluginId,
				                   scope.engineInstanceId, scope.pluginRuntimeInstanceId,
				                   scope.pluginRuntimeLocationHandle, scope.pluginIndexHint, index});
			}
			runtime->markTimerRuntimeStatesChanged(scope.pluginId, changedRuntimeLocations);
			return true;
		}

		bool prepareNextScope()
		{
			while (scopeIndex < scopes.size())
			{
				const Entry scope = scopes.at(scopeIndex++);
				if (!prepareScope(scope))
					continue;
				if (!entries.isEmpty())
					return true;
			}
			return false;
		}

		void start()
		{
			if (!processor || !runtime)
				return;
			generation     = processor->m_ruleEvaluationGeneration;
			evaluationTime = QDateTime::currentDateTime();
			connected      = runtime->connectPhase() == WorldRuntime::eConnectConnectedToMud;
			if (shouldEvaluateRuleCollection(runtime->worldAttributes(), WorldRuleKind::Timer, false))
				scopes.push_back({});
			const QList<WorldRuntime::Plugin> &plugins = runtime->plugins();
			scopes.reserve(scopes.size() + plugins.size());
			for (int pluginIndex = 0; pluginIndex < plugins.size(); ++pluginIndex)
			{
				const WorldRuntime::Plugin &plugin = plugins.at(pluginIndex);
				if (pluginHasValidId(plugin) && plugin.enabled && !plugin.installPending)
					scopes.push_back({0, 0, pluginIdOf(&plugin), plugin.lua ? plugin.lua->instanceId() : 0,
					                  plugin.runtimeInstanceId, plugin.runtimeLocationHandle, pluginIndex,
					                  -1});
			}
			advance();
		}

		void finishCurrentTimer()
		{
			if (!runtime)
				return;
			WorldRuntime::Timer *timer = runtime->timerForRuntimeLocation(
			    current.runtimeLocationHandle, current.runtimeId, current.indexHint);
			if (timer && isEnabledValue(timer->attributes.value(QStringLiteral("one_shot"))))
			{
				if (removeTimerByRuntimeId(runtime, current.runtimeLocationHandle, current.runtimeId,
				                           current.pluginId, current.pluginRuntimeLocationHandle,
				                           current.pluginRuntimeInstanceId, current.pluginIndexHint,
				                           current.indexHint))
				{
					runtime->noteTimerStructureMutation();
					if (current.pluginId.isEmpty())
						runtime->markTimersChanged();
					else
						runtime->markPluginTimersChanged(current.pluginId);
				}
			}
			++nextIndex;
			pendingAction = PendingAction::None;
		}

		void orderedActionFinished(const bool completed)
		{
			++completionDepth;
			const auto leaveCompletion = qScopeGuard([this] { --completionDepth; });
			Q_UNUSED(leaveCompletion);
			executionScope.reset();
			if (!completed || !processor || !runtime || generation != processor->m_ruleEvaluationGeneration)
			{
				finish();
				return;
			}
			AdvanceDisposition disposition = AdvanceDisposition::Ready;
			if (pendingAction == PendingAction::Body)
				disposition = startRoutineOrFinish();
			else if (pendingAction == PendingAction::Routine)
				finishCurrentTimer();
			else
			{
				Q_ASSERT_X(false, "TimerEvaluationSequence::orderedActionFinished",
				           "timer action completed without a pending action");
				return;
			}
			if (disposition == AdvanceDisposition::WaitingForRoutine)
				return;
			if (completionDepth > 1)
			{
				advanceRequested = true;
				return;
			}
			advance();
		}

		void startBody(WorldRuntime::Timer &timer)
		{
			const QMap<QString, QString> attrs        = timer.attributes;
			const QString                sendText     = timer.children.value(QStringLiteral("send"));
			const int                    sendToValue  = attrs.value(QStringLiteral("send_to")).toInt();
			const QString                variableName = attrs.value(QStringLiteral("variable"));
			const QString                label        = attrs.value(QStringLiteral("name")).trimmed();
			const bool omitFromOutput = isEnabledValue(attrs.value(QStringLiteral("omit_from_output")));
			const bool omitFromLog    = isEnabledValue(attrs.value(QStringLiteral("omit_from_log")));
			const bool oneShot        = isEnabledValue(attrs.value(QStringLiteral("one_shot")));
			if (oneShot)
				timer.attributes.insert(QStringLiteral("enabled"), QStringLiteral("0"));
			QMudTimerScheduling::applyTimerFiredState(timer, evaluationTime);
			runtime->markTimerRuntimeStateChanged(current.pluginId, current.runtimeLocationHandle,
			                                      current.runtimeId, current.indexHint,
			                                      oneShot ? luaCallbackAllRuleSnapshotParts()
			                                              : LuaCallbackRuleSnapshotPart::RuntimeState |
			                                                    LuaCallbackRuleSnapshotPart::DetailState);
			runtime->incrementTimersFired();
			processor->emitTrace(label.isEmpty() ? QStringLiteral("Fired unlabelled timer ")
			                                     : QStringLiteral("Fired timer %1").arg(label));
			pendingAction  = PendingAction::Body;
			executionScope = std::make_unique<TimerExecutionScope>(runtime, current.runtimeLocationHandle,
			                                                       current.runtimeId, current.pluginId, false,
			                                                       current.indexHint);
			const auto           self           = shared_from_this();
			const unsigned short previousSource = runtime->currentActionSource();
			runtime->setCurrentActionSource(WorldRuntime::eTimerFired);
			processor->runOrderedRuleAction(
			    [self, sendToValue, sendText, omitFromOutput, omitFromLog, variableName, label]
			    {
				    if (self->processor)
					    self->processor->sendTo(sendToValue, sendText, omitFromOutput, omitFromLog,
					                            variableName, QStringLiteral("Timer: %1").arg(label),
					                            self->current.pluginId, nullptr, false, false, 0, 0,
					                            LuaCallbackEvent::TimerScript);
			    },
			    [self](const bool completed) { self->orderedActionFinished(completed); });
			if (runtime)
				runtime->setCurrentActionSource(previousSource);
		}

		[[nodiscard]] AdvanceDisposition startRoutineOrFinish()
		{
			pendingAction = PendingAction::None;
			if (!runtime || !processor || !ownerStillCurrent(current))
			{
				finishCurrentTimer();
				return AdvanceDisposition::Ready;
			}
			WorldRuntime::Timer *timer = runtime->timerForRuntimeLocation(
			    current.runtimeLocationHandle, current.runtimeId, current.indexHint);
			if (!timer)
			{
				++nextIndex;
				return AdvanceDisposition::Ready;
			}
			const QString scriptName = timer->attributes.value(QStringLiteral("script"));
			const QString label      = timer->attributes.value(QStringLiteral("name")).trimmed();
			if (scriptName.isEmpty())
			{
				finishCurrentTimer();
				return AdvanceDisposition::Ready;
			}
			QSharedPointer<LuaCallbackEngine> engine;
			if (current.pluginId.isEmpty())
			{
				LuaCallbackEngine *worldLua = runtime->luaCallbacks();
				if (!processor->canExecuteWorldScript(QStringLiteral("Timer"), scriptName, worldLua))
				{
					finishCurrentTimer();
					return AdvanceDisposition::Ready;
				}
				engine = QSharedPointer<LuaCallbackEngine>(worldLua, [](LuaCallbackEngine * /*unused*/) {});
			}
			else
			{
				const WorldRuntime::Plugin *plugin = WorldCommandProcessorMutationAccess::pluginForLocation(
				    *runtime, current.pluginRuntimeLocationHandle, current.pluginRuntimeInstanceId,
				    current.pluginIndexHint);
				if (plugin)
					engine = plugin->lua;
			}
			if (!engine)
			{
				finishCurrentTimer();
				return AdvanceDisposition::Ready;
			}
			pendingAction  = PendingAction::Routine;
			executionScope = std::make_unique<TimerExecutionScope>(runtime, current.runtimeLocationHandle,
			                                                       current.runtimeId, current.pluginId, true,
			                                                       current.indexHint);
			const auto           self           = shared_from_this();
			const unsigned short previousSource = runtime->currentActionSource();
			runtime->setCurrentActionSource(WorldRuntime::eTimerFired);
			processor->runOrderedRuleAction(
			    [self, engine, scriptName, label]
			    {
				    if (!self->processor || !self->runtime)
					    return;
				    auto operation = self->processor->m_activeOperationCompletion;
				    if (operation)
					    operation->retain();
				    const CallerContextRunner context     = self->processor->captureCallerContextRunner();
				    const bool                worldScript = self->current.pluginId.isEmpty();
				    self->runtime->dispatchLuaStringsAndWildcardsAsync(
				        engine, scriptName, LuaCallbackEvent::TimerRoutine, {label}, {}, {}, nullptr,
				        WorldRuntime::eTimerFired, false, 0, 0,
				        [self, operation, context, worldScript,
				         scriptName](const LuaBatchDispatchResult &result)
				        {
					        const auto complete = [self, operation, worldScript, scriptName, result]
					        {
						        if (worldScript && result.hasFunctionValid && !result.hasFunction &&
						            self->processor)
						        {
							        self->processor->warnMissingWorldScriptFunction(QStringLiteral("Timer"),
							                                                        scriptName);
						        }
						        if (operation)
							        operation->release(true);
					        };
					        if (context)
						        context.run(complete);
					        else
						        complete();
				        });
			    },
			    [self](const bool completed) { self->orderedActionFinished(completed); });
			if (runtime)
				runtime->setCurrentActionSource(previousSource);
			return pendingAction == PendingAction::Routine ? AdvanceDisposition::WaitingForRoutine
			                                               : AdvanceDisposition::Ready;
		}

		void finish() const
		{
			if (!processor)
				return;
			if (processor->m_timerEvaluationSequence.get() != this)
				return;
			processor->m_timerEvaluationSequence.reset();
			if (!processor->m_timerEvaluationRerunPending)
				return;
			processor->m_timerEvaluationRerunPending       = false;
			QPointer<WorldCommandProcessor> processorGuard = processor;
			QMetaObject::invokeMethod(
			    processor,
			    [processorGuard]
			    {
				    if (processorGuard)
					    processorGuard->checkTimers();
			    },
			    Qt::QueuedConnection);
		}

		void advance()
		{
			if (!processor || !runtime || generation != processor->m_ruleEvaluationGeneration)
			{
				finish();
				return;
			}
			if (advancing)
			{
				advanceRequested = true;
				return;
			}
			advancing                = true;
			const auto stopAdvancing = qScopeGuard([this] { advancing = false; });
			Q_UNUSED(stopAdvancing);
			for (;;)
			{
				advanceRequested = false;
				while (nextIndex < entries.size())
				{
					current = entries.at(nextIndex);
					if (!ownerStillCurrent(current))
					{
						++nextIndex;
						continue;
					}
					WorldRuntime::Timer *timer = runtime->timerForRuntimeLocation(
					    current.runtimeLocationHandle, current.runtimeId, current.indexHint);
					if (!timer)
					{
						++nextIndex;
						continue;
					}
					startBody(*timer);
					if (pendingAction != PendingAction::None)
						return;
					if (!advanceRequested)
						return;
					break;
				}
				if (nextIndex >= entries.size())
				{
					if (prepareNextScope())
						continue;
					finish();
					return;
				}
			}
		}
};

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
                                                  const LuaCallbackEngine *lua) const
{
	if (!m_runtime)
		return false;

	const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
	const bool warn             = isEnabledValue(attrs.value(QStringLiteral("warn_if_scripting_inactive")));
	const bool scriptingEnabled = m_runtime->luaScriptingAvailable() && lua;
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
	if (m_runtime == runtime)
	{
		refreshRuntimeOptions();
		return;
	}
	{
		QScopedValueRollback cancellingOutboundBinding(m_outboundBindingCancellationActive, true);
		if (m_activeIncomingLineOperation)
			cancelIncomingLine(m_activeIncomingLineOperation);
		else
			cancelInputCriticalOperations();
		if (m_processingEnteredCommand)
			cancelActiveEnteredCommand();
		cancelOutboundWorkForRuntimeDetach(m_runtime);
	}
	++m_ruleEvaluationGeneration;
	m_timerEvaluationSequence.reset();
	m_timerEvaluationRerunPending = false;
	m_runtime                     = runtime;
	invalidateTriggerEvaluationCache();
	m_aliasEvaluationCache.clear();
	m_aliasEvaluationCacheGeneration = 0;
	m_pluginOrderCache               = PluginOrderCacheEntry();
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
	refreshRuntimeOptions();
}

void WorldCommandProcessor::refreshRuntimeOptions()
{
	if (!m_runtime)
		return;
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

void WorldCommandProcessor::applyNumericWorldOption(const WorldNumericOptionBinding binding,
                                                    const long long                 value)
{
	const bool enabled = value != 0;
	switch (binding)
	{
	case WorldNumericOptionBinding::CommandDoNotTranslateIac:
		m_doNotTranslateIac = enabled;
		break;
	case WorldNumericOptionBinding::CommandEnableSpamPrevention:
		m_enableSpamPrevention = enabled;
		break;
	case WorldNumericOptionBinding::CommandRegexpMatchEmpty:
		m_regexpMatchEmpty = enabled;
		break;
	case WorldNumericOptionBinding::CommandSpamLineCount:
		m_spamLineCount = static_cast<int>(value);
		break;
	case WorldNumericOptionBinding::CommandSpeedWalkDelay:
		m_speedWalkDelay = static_cast<int>(value);
		if (m_speedWalkDelay <= 0 && !m_queuedCommands.isEmpty())
			processQueuedCommands(true);
		break;
	case WorldNumericOptionBinding::CommandTranslateBackslashSequences:
		m_translateBackslashSequences = enabled;
		break;
	case WorldNumericOptionBinding::CommandTranslateGerman:
		m_translateGerman = enabled;
		break;
	case WorldNumericOptionBinding::CommandUtf8:
		m_utf8 = enabled;
		break;
	default:
		break;
	}
}

void WorldCommandProcessor::setRuntimeAutomationOwner(const bool owner)
{
	m_runtimeAutomationOwner = owner;
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

	const QList<WorldRuntime::Plugin> &plugins   = WorldCommandProcessorMutationAccess::plugins(*m_runtime);
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

QSharedPointer<const WorldCommandProcessor::TriggerEvaluationCacheEntry>
WorldCommandProcessor::decodedTriggerEvaluationCache(QList<WorldRuntime::Trigger> &triggers,
                                                     WorldRuntime::Plugin         *plugin)
{
	if (!m_runtime)
		return {};

	const quint64 generation = m_runtime->triggerRuleGeneration();
	if (m_triggerEvaluationCacheGeneration != generation)
	{
		m_triggerEvaluationCache.clear();
		m_triggerEvaluationCacheGeneration = generation;
	}

	const auto cacheKey = reinterpret_cast<quintptr>(&triggers);
	const int  count    = safeQSizeToInt(triggers.size());
	if (auto cacheIt = m_triggerEvaluationCache.constFind(cacheKey);
	    cacheIt != m_triggerEvaluationCache.constEnd() && (*cacheIt)->generation == generation &&
	    (*cacheIt)->count == count)
	{
		return *cacheIt;
	}
	if (std::ranges::any_of(triggers,
	                        [](const WorldRuntime::Trigger &trigger) { return trigger.runtimeId == 0; }))
	{
		m_runtime->ensureAllTriggerRuntimeIds();
	}
	if (std::ranges::any_of(triggers, [](const WorldRuntime::Trigger &trigger)
	                        { return trigger.runtimeLocationHandle == 0; }))
	{
		WorldCommandProcessorMutationAccess::synchronizeTriggers(*m_runtime, plugin, 0);
	}

	auto rebuilt        = QSharedPointer<TriggerEvaluationCacheEntry>::create();
	rebuilt->generation = generation;
	rebuilt->count      = count;
	rebuilt->triggers.reserve(count);
	QHash<QString, int> matcherIndexByKey;
	matcherIndexByKey.reserve(count);
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
		decoded.keepEvaluating        = isEnabledValue(attrs.value(QStringLiteral("keep_evaluating")));
		decoded.soundIfInactive       = isEnabledValue(attrs.value(QStringLiteral("sound_if_inactive")));
		decoded.otherTextColour       = attrs.value(QStringLiteral("other_text_colour"));
		decoded.otherBackColour       = attrs.value(QStringLiteral("other_back_colour"));
		decoded.clipboardArg          = attrs.value(QStringLiteral("clipboard_arg")).toInt();
		decoded.runtimeId             = trigger.runtimeId;
		decoded.runtimeLocationHandle = trigger.runtimeLocationHandle;
		decoded.executionTimeNs       = trigger.executionTimeNs;
		decoded.matchCount            = trigger.matchCount;
		decoded.matchAttempts         = trigger.matchAttempts;
		decoded.dynamicPattern = decoded.expandVariables && decoded.matchText.contains(QLatin1Char('@'));
		if (!decoded.dynamicPattern && !decoded.matchText.isEmpty())
		{
			const QString pattern =
			    decoded.isRegexp ? decoded.matchText : wildcardToRegexCached(decoded.matchText);
			QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;
			if (decoded.ignoreCase)
				options |= QRegularExpression::CaseInsensitiveOption;
			if (decoded.multiLine)
				options |= QRegularExpression::MultilineOption;
			QString warningKey = pattern;
			warningKey += QChar(0x1F);
			warningKey += decoded.ignoreCase ? QLatin1Char('1') : QLatin1Char('0');
			warningKey += decoded.multiLine ? QLatin1Char('1') : QLatin1Char('0');
			const auto existingMatcher = matcherIndexByKey.constFind(warningKey);
			if (existingMatcher != matcherIndexByKey.cend())
				decoded.matcherIndex = *existingMatcher;
			else
			{
				decoded.matcherIndex = safeQSizeToInt(rebuilt->matchers.size());
				matcherIndexByKey.insert(warningKey, decoded.matcherIndex);
				rebuilt->matchers.push_back({std::move(warningKey), QRegularExpression(pattern, options)});
			}
		}
		rebuilt->triggers.push_back(std::move(decoded));
	}

	std::ranges::stable_sort(rebuilt->triggers,
	                         [](const DecodedTrigger &left, const DecodedTrigger &right)
	                         {
		                         if (left.sequence != right.sequence)
			                         return left.sequence < right.sequence;
		                         return left.matchText < right.matchText;
	                         });

	if (m_triggerEvaluationCache.size() > 2048)
		m_triggerEvaluationCache.clear();
	const QSharedPointer<const TriggerEvaluationCacheEntry> immutable = rebuilt;
	m_triggerEvaluationCache.insert(cacheKey, immutable);
	return immutable;
}

const WorldCommandProcessor::AliasEvaluationCacheEntry &
WorldCommandProcessor::decodedAliasEvaluationCache(const QList<WorldRuntime::Alias> &aliases)
{
	static const AliasEvaluationCacheEntry kEmpty;
	if (!m_runtime)
		return kEmpty;

	const quint64 generation = m_runtime->aliasRuleGeneration();
	if (m_aliasEvaluationCacheGeneration != generation)
	{
		m_aliasEvaluationCache.clear();
		m_aliasEvaluationCacheGeneration = generation;
	}

	const auto cacheKey = reinterpret_cast<quintptr>(&aliases);
	const int  count    = safeQSizeToInt(aliases.size());
	if (auto cacheIt = m_aliasEvaluationCache.constFind(cacheKey);
	    cacheIt != m_aliasEvaluationCache.constEnd() && cacheIt->generation == generation &&
	    cacheIt->count == count)
	{
		return cacheIt.value();
	}

	AliasEvaluationCacheEntry rebuilt;
	rebuilt.generation = generation;
	rebuilt.count      = count;
	rebuilt.aliases.reserve(count);
	for (int i = 0; i < count; ++i)
	{
		const WorldRuntime::Alias    &alias = aliases.at(i);
		const QMap<QString, QString> &attrs = alias.attributes;

		DecodedAlias                  decoded;
		decoded.index              = i;
		decoded.sequence           = attrs.value(QStringLiteral("sequence")).toInt();
		decoded.matchText          = attrs.value(QStringLiteral("match"));
		decoded.sendText           = alias.children.value(QStringLiteral("send"));
		decoded.label              = attrs.value(QStringLiteral("name"));
		decoded.scriptLabel        = decoded.label.isEmpty() ? decoded.matchText : decoded.label;
		decoded.variableName       = attrs.value(QStringLiteral("variable"));
		decoded.scriptName         = attrs.value(QStringLiteral("script"));
		decoded.enabled            = isEnabledValue(attrs.value(QStringLiteral("enabled")));
		decoded.isRegexp           = isEnabledValue(attrs.value(QStringLiteral("regexp")));
		decoded.ignoreCase         = isEnabledValue(attrs.value(QStringLiteral("ignore_case")));
		decoded.omitCommandHistory = isEnabledValue(attrs.value(QStringLiteral("omit_from_command_history")));
		decoded.echoMatchedAlias   = isEnabledValue(attrs.value(QStringLiteral("echo_alias")));
		decoded.expandVariables    = isEnabledValue(attrs.value(QStringLiteral("expand_variables")));
		decoded.omitFromOutput     = isEnabledValue(attrs.value(QStringLiteral("omit_from_output")));
		decoded.omitFromLog        = isEnabledValue(attrs.value(QStringLiteral("omit_from_log")));
		decoded.oneShot            = isEnabledValue(attrs.value(QStringLiteral("one_shot")));
		decoded.keepEvaluating     = isEnabledValue(attrs.value(QStringLiteral("keep_evaluating")));
		decoded.sendToValue        = attrs.value(QStringLiteral("send_to")).toInt();
		if (decoded.enabled)
			++rebuilt.enabledCount;
		rebuilt.aliases.push_back(std::move(decoded));
	}

	std::ranges::sort(rebuilt.aliases,
	                  [](const DecodedAlias &left, const DecodedAlias &right)
	                  {
		                  if (left.sequence != right.sequence)
			                  return left.sequence < right.sequence;
		                  return left.label < right.label;
	                  });

	if (m_aliasEvaluationCache.size() > 2048)
		m_aliasEvaluationCache.clear();
	m_aliasEvaluationCache.insert(cacheKey, rebuilt);
	return m_aliasEvaluationCache[cacheKey];
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
	if (count > 0)
		emit queuedCommandsChanged();
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
	if (!hasCurrentInputCriticalOperation())
	{
		enqueueOrRunInputCriticalAction(
		    [processor = QPointer<WorldCommandProcessor>(this), text](CommandCompletion completion)
		    {
			    if (!processor)
			    {
				    if (completion)
					    completion(false);
				    return;
			    }
			    CommandCompletion failureCompletion = completion;
			    const int result = processor->executeCommandWithCompletion(text, std::move(completion));
			    if (result != eOK && failureCompletion)
				    failureCompletion(false);
		    });
		return eOK;
	}
	return executeCommandWithCompletion(text, {});
}

WorldCommandProcessor::CommandExecutionContext WorldCommandProcessor::captureCommandExecutionContext() const
{
	CommandExecutionContext context;
	context.actionSource      = m_runtime ? m_runtime->currentActionSource()
	                                      : static_cast<unsigned short>(WorldRuntime::eUnknownActionSource);
	context.executionDepth    = m_executionDepth;
	context.suppressInputLog  = m_suppressInputLog;
	context.processingAutoSay = m_processingAutoSay;
	context.triggerCommandPriorityDepth         = m_triggerCommandPriorityDepth;
	context.triggerPriorityActionExecutionDepth = m_triggerPriorityActionExecutionDepth;
	context.historyState                        = m_activeCommandHistoryState;
	context.operationCompletion                 = m_activeOperationCompletion;
	context.inputCriticalOperationScope         = m_currentInputCriticalOperationScope;
	return context;
}

WorldCommandProcessor::CallerContextRunner WorldCommandProcessor::captureCallerContextRunner() const
{
	const CommandExecutionContext         context = captureCommandExecutionContext();
	const QPointer<WorldCommandProcessor> processor(const_cast<WorldCommandProcessor *>(this));
	return CallerContextRunner(
	    [processor, context](const std::function<void()> &work)
	    {
		    if (!work)
			    return;
		    if (!processor)
		    {
			    work();
			    return;
		    }
		    processor->runWithCommandExecutionContext(context, work);
	    });
}

WorldCommandProcessor::CallerContextRunner WorldCommandProcessor::captureRuntimeCallerContextRunner() const
{
	if (m_runtime)
		return m_runtime->captureRuntimeCallerContext();
	return captureCallerContextRunner();
}

WorldCommandProcessor::CallerContextRunner WorldCommandProcessor::captureAmbientCallerContextRunner() const
{
	CommandExecutionContext context = captureCommandExecutionContext();
	context.operationCompletion.reset();
	context.inputCriticalOperationScope.clear();
	const QPointer<WorldCommandProcessor> processor(const_cast<WorldCommandProcessor *>(this));
	return CallerContextRunner(
	    [processor, context](const std::function<void()> &work)
	    {
		    if (!work)
			    return;
		    if (!processor)
		    {
			    work();
			    return;
		    }
		    processor->runWithCommandExecutionContext(context, work);
	    });
}

void WorldCommandProcessor::runWithCommandExecutionContext(const CommandExecutionContext &context,
                                                           const std::function<void()>   &work)
{
	if (!work)
		return;
	const CommandExecutionContext previous = captureCommandExecutionContext();
	if (m_runtime)
		m_runtime->setCurrentActionSource(context.actionSource);
	m_executionDepth                      = context.executionDepth;
	m_suppressInputLog                    = context.suppressInputLog;
	m_processingAutoSay                   = context.processingAutoSay;
	m_triggerCommandPriorityDepth         = context.triggerCommandPriorityDepth;
	m_triggerPriorityActionExecutionDepth = context.triggerPriorityActionExecutionDepth;
	m_activeCommandHistoryState           = context.historyState;
	m_activeOperationCompletion           = context.operationCompletion;
	m_currentInputCriticalOperationScope  = context.inputCriticalOperationScope;
	const auto restore                    = qScopeGuard(
	    [this, previous]
	    {
		    if (m_runtime)
			    m_runtime->setCurrentActionSource(previous.actionSource);
		    m_executionDepth                      = previous.executionDepth;
		    m_suppressInputLog                    = previous.suppressInputLog;
		    m_processingAutoSay                   = previous.processingAutoSay;
		    m_triggerCommandPriorityDepth         = previous.triggerCommandPriorityDepth;
		    m_triggerPriorityActionExecutionDepth = previous.triggerPriorityActionExecutionDepth;
		    m_activeCommandHistoryState           = previous.historyState;
		    m_activeOperationCompletion           = previous.operationCompletion;
		    m_currentInputCriticalOperationScope  = previous.inputCriticalOperationScope;
	    });
	Q_UNUSED(restore);
	work();
}

void WorldCommandProcessor::runWithInputCriticalOperationScope(const InputCriticalOperationScopePtr &scope,
                                                               const std::function<void()>          &work)
{
	if (!work)
		return;
	const LogicalOperationCompletionPtr  previousOperation = m_activeOperationCompletion;
	const InputCriticalOperationScopePtr previousScope     = m_currentInputCriticalOperationScope;
	m_activeOperationCompletion                            = scope;
	m_currentInputCriticalOperationScope                   = scope;
	const auto restore                                     = qScopeGuard(
	    [this, previousOperation, previousScope]
	    {
		    m_activeOperationCompletion          = previousOperation;
		    m_currentInputCriticalOperationScope = previousScope;
	    });
	Q_UNUSED(restore);
	work();
}

int WorldCommandProcessor::executeCommandWithCompletion(const QString &text, CommandCompletion completion)
{
	if (QMudAliasMatch::exceedsExecutionDepth(m_executionDepth, kMaxExecutionDepth))
		return eCommandsNestedTooDeeply;
	++m_executionDepth;
	const auto restoreDepth = qScopeGuard([this] { --m_executionDepth; });
	Q_UNUSED(restoreDepth);
	static_cast<void>(evaluateCommand(text, true, std::move(completion)));
	return eOK;
}

int WorldCommandProcessor::executeUserMacroSendNow(const QString &text, const bool history)
{
	if (!hasCurrentInputCriticalOperation())
	{
		enqueueOrRunInputCriticalAction(
		    [processor = QPointer<WorldCommandProcessor>(this), text, history](CommandCompletion completion)
		    {
			    if (!processor)
			    {
				    if (completion)
					    completion(false);
				    return;
			    }
			    processor->runOrderedRuleAction(
			        [processor, text, history]
			        { static_cast<void>(processor->executeUserMacroSendNow(text, history)); },
			        std::move(completion));
		    });
		return eOK;
	}

	QString commandText = text;
	if (m_translateBackslashSequences)
		commandText = fixupEscapeSequences(commandText);

	const auto              historyState = QSharedPointer<CommandHistoryState>::create();
	CommandExecutionContext context      = captureCommandExecutionContext();
	context.historyState                 = historyState;
	const QPointer<WorldCommandProcessor> processor(this);
	int                                   result = eWorldClosed;
	runWithCommandExecutionContext(context,
	                               [this, &result, commandText, processor, text, history, historyState]
	                               {
		                               result = executeCommandWithCompletion(
		                                   commandText,
		                                   [processor, text, history, historyState](const bool completed)
		                                   {
			                                   if (!completed || !processor)
				                                   return;
			                                   if (history && !historyState->omitFromHistory &&
			                                       !historyState->sendFailed && processor->m_view)
			                                   {
				                                   processor->m_view->addToHistoryForced(text);
			                                   }
		                                   });
	                               });
	return result;
}

int WorldCommandProcessor::executeCommandWithTriggerPriority(const QString &text)
{
	if (!hasCurrentInputCriticalOperation())
	{
		enqueueOrRunInputCriticalAction(
		    [processor = QPointer<WorldCommandProcessor>(this), text](CommandCompletion completion)
		    {
			    if (!processor)
			    {
				    if (completion)
					    completion(false);
				    return;
			    }
			    processor->runOrderedRuleAction(
			        [processor, text]
			        { static_cast<void>(processor->executeCommandWithTriggerPriority(text)); },
			        std::move(completion));
		    });
		return eOK;
	}

	if (m_pendingTriggerPriorityCommands > 0 && m_triggerPriorityActionExecutionDepth == 0)
	{
		PendingTriggerPriorityAction action;
		action.kind = PendingTriggerPriorityAction::Kind::Execute;
		action.text = text;
		enqueuePendingTriggerPriorityAction(std::move(action));
		return eOK;
	}

	ensureTriggerPriorityOutboundGroup();
	++m_triggerCommandPriorityDepth;
	++m_triggerPriorityActionExecutionDepth;
	const auto restoreTriggerCommandPriority = qScopeGuard(
	    [this]
	    {
		    --m_triggerPriorityActionExecutionDepth;
		    --m_triggerCommandPriorityDepth;
	    });
	Q_UNUSED(restoreTriggerCommandPriority);
	++m_pendingTriggerPriorityCommands;
	const QPointer<WorldCommandProcessor> processor(this);
	int                                   result = eWorldClosed;
	runWithTriggerPriorityOutboundGroup(
	    [this, &result, text, processor]
	    {
		    result = executeCommandWithCompletion(text,
		                                          [processor](const bool completed)
		                                          {
			                                          if (processor)
				                                          processor->finishTriggerPriorityCommand(completed);
		                                          });
	    });
	if (result != eOK)
		finishTriggerPriorityCommand(false);
	return result;
}

void WorldCommandProcessor::ensureTriggerPriorityOutboundGroup()
{
	if (!m_runtime || m_triggerPriorityOutboundGroup)
		return;
	m_triggerPriorityOutboundGroup = m_runtime->acquireCausalOutboundGroup();
}

void WorldCommandProcessor::releaseTriggerPriorityOutboundGroup()
{
	if (!m_runtime || !m_triggerPriorityOutboundGroup)
		return;
	const QSharedPointer<CausalOutboundGroup> group = std::exchange(m_triggerPriorityOutboundGroup, {});
	m_runtime->releaseCausalOutboundGroup(group);
}

void WorldCommandProcessor::runWithTriggerPriorityOutboundGroup(const std::function<void()> &work) const
{
	if (!work)
		return;
	if (!m_runtime || !m_triggerPriorityOutboundGroup)
	{
		work();
		return;
	}
	m_runtime->runWithCausalOutboundGroup(m_triggerPriorityOutboundGroup, work);
}

void WorldCommandProcessor::enqueuePendingTriggerPriorityAction(PendingTriggerPriorityAction action)
{
	action.callerContext       = captureCallerContextRunner();
	action.operationCompletion = m_activeOperationCompletion;
	if (action.operationCompletion)
		action.operationCompletion->retain();
	m_pendingTriggerPriorityActions.enqueue(std::move(action));
}

void WorldCommandProcessor::cancelPendingTriggerPriorityActions()
{
	QQueue<PendingTriggerPriorityAction> pending;
	pending.swap(m_pendingTriggerPriorityActions);
	while (!pending.isEmpty())
	{
		PendingTriggerPriorityAction action = pending.dequeue();
		if (!action.operationCompletion)
			continue;
		const auto releaseOperation = [&action]
		{
			action.operationCompletion->release(false);
			action.operationCompletion.clear();
		};
		if (action.callerContext)
			action.callerContext.run(releaseOperation);
		else
			releaseOperation();
	}
}

void WorldCommandProcessor::finishTriggerPriorityCommand(const bool completed)
{
	Q_ASSERT(m_pendingTriggerPriorityCommands > 0);
	if (m_pendingTriggerPriorityCommands > 0)
		--m_pendingTriggerPriorityCommands;
	if (m_pendingTriggerPriorityCommands > 0)
		return;
	if (!completed)
	{
		cancelPendingTriggerPriorityActions();
		releaseTriggerPriorityOutboundGroup();
		return;
	}
	drainPendingTriggerPriorityActions();
	if (m_pendingTriggerPriorityCommands == 0 && m_pendingTriggerPriorityActions.isEmpty())
		releaseTriggerPriorityOutboundGroup();
	if (m_pendingTriggerPriorityCommands == 0 && !m_queuedCommands.isEmpty() && m_speedWalkDelay > 0)
		m_queueDispatchTimer.restart();
}

void WorldCommandProcessor::drainPendingTriggerPriorityActions()
{
	if (m_drainingPendingTriggerPriorityActions || m_pendingTriggerPriorityCommands > 0)
		return;
	m_drainingPendingTriggerPriorityActions = true;
	const auto finishDraining = qScopeGuard([this] { m_drainingPendingTriggerPriorityActions = false; });
	Q_UNUSED(finishDraining);
	while (m_pendingTriggerPriorityCommands == 0 && !m_pendingTriggerPriorityActions.isEmpty())
	{
		PendingTriggerPriorityAction action        = m_pendingTriggerPriorityActions.dequeue();
		const auto                   executeAction = [this, &action]
		{
			bool started = true;
			if (action.kind == PendingTriggerPriorityAction::Kind::Execute)
			{
				started = executeCommandWithTriggerPriority(action.text) == eOK;
			}
			else
			{
				sendRawTextWithTriggerPriority(action.text, action.echo, action.queueIt, action.logIt, false);
			}
			if (action.operationCompletion)
			{
				action.operationCompletion->release(started);
				action.operationCompletion.clear();
			}
		};
		runWithTriggerPriorityOutboundGroup(
		    [&action, &executeAction]
		    {
			    if (action.callerContext)
				    action.callerContext.run(executeAction);
			    else
				    executeAction();
		    });
	}
}

void WorldCommandProcessor::sendToFromAccelerator(int sendTo, const QString &text, const QString &description,
                                                  const WorldRuntime::Plugin *plugin)
{
	if (!hasCurrentInputCriticalOperation())
	{
		const QString pluginId                    = pluginIdOf(plugin);
		const quint64 pluginRuntimeLocationHandle = plugin ? plugin->runtimeLocationHandle : 0;
		const quint64 pluginRuntimeInstanceId     = plugin ? plugin->runtimeInstanceId : 0;
		enqueueOrRunInputCriticalAction(
		    [processor = QPointer<WorldCommandProcessor>(this), sendTo, text, description, pluginId,
		     pluginRuntimeLocationHandle, pluginRuntimeInstanceId](CommandCompletion completion)
		    {
			    if (!processor)
			    {
				    if (completion)
					    completion(false);
				    return;
			    }
			    processor->runOrderedRuleAction(
			        [processor, sendTo, text, description, pluginId, pluginRuntimeLocationHandle,
			         pluginRuntimeInstanceId]
			        {
				        const WorldRuntime::Plugin *resolvedPlugin =
				            processor->m_runtime && !pluginId.isEmpty()
				                ? WorldCommandProcessorMutationAccess::pluginForLocation(
				                      *processor->m_runtime, pluginRuntimeLocationHandle,
				                      pluginRuntimeInstanceId, -1)
				                : nullptr;
				        if (!pluginId.isEmpty() &&
				            (!resolvedPlugin || !resolvedPlugin->enabled || resolvedPlugin->installPending))
					        return;
				        processor->sendToFromAccelerator(sendTo, text, description, resolvedPlugin);
			        },
			        std::move(completion));
		    });
		return;
	}
	this->sendTo(sendTo, text, true, true, QString(), description, pluginIdOf(plugin));
}

void WorldCommandProcessor::emitTrace(const QString &message) const
{
	if (!m_runtime)
		return;
	m_runtime->emitTrace(message);
}

void WorldCommandProcessor::onCommandEntered(const QString &text)
{
	PendingEnteredCommand command;
	command.text         = text;
	command.historyState = QSharedPointer<CommandHistoryState>::create();
	if (m_runtime)
	{
		command.actionSource = m_runtime->currentActionSource();
		if (command.actionSource == WorldRuntime::eUnknownActionSource)
			command.actionSource = WorldRuntime::eUserTyping;
		m_runtime->setCurrentActionSource(WorldRuntime::eUnknownActionSource);
	}

	enqueueOrRunInputCriticalAction(
	    [processor = QPointer<WorldCommandProcessor>(this),
	     command   = std::move(command)](CommandCompletion completion) mutable
	    {
		    if (!processor)
		    {
			    if (completion)
				    completion(false);
			    return;
		    }
		    command.completion = std::move(completion);
		    processor->startEnteredCommand(std::move(command));
	    });
}

void WorldCommandProcessor::startEnteredCommand(PendingEnteredCommand command)
{
	Q_ASSERT(!m_processingEnteredCommand);
	m_processingEnteredCommand       = true;
	m_activeEnteredCommandCompletion = std::move(command.completion);
	CommandExecutionContext context  = captureCommandExecutionContext();
	context.actionSource             = command.actionSource;
	context.historyState             = command.historyState;
	runWithCommandExecutionContext(
	    context,
	    [this, command = std::move(command)]() mutable
	    {
		    QString commandText = std::move(command.text);
		    if (m_runtime)
		    {
			    auto continuationFactory = makeLuaCallbackResultContinuationFactory(
			        [processor = QPointer<WorldCommandProcessor>(this), commandText]
			        {
				        PendingPluginCommandEntered state;
				        state.processor    = processor;
				        state.originalText = commandText;
				        return state;
			        });
			    LuaCallbackContinuationDispatchResult callback =
			        m_runtime->firePluginCommandEnteredContinuable(commandText, continuationFactory);
			    if (callback.suspended())
				    return;
			    commandText = callback.result.stringResult;
		    }
		    continueCommandEnteredAfterPluginTransform(std::move(commandText));
	    });
}

void WorldCommandProcessor::continueCommandEnteredAfterPluginTransform(QString commandText)
{
	const QString historyText = commandText;

	// Sentinel host return codes from OnPluginCommandEntered:
	// "\r" => ignore command, "\t" => discard command.
	if (commandText == QStringLiteral("\r") || commandText == QStringLiteral("\t"))
	{
		finishCommandEntered(historyText, m_activeCommandHistoryState, false, true);
		return;
	}

	if (m_translateBackslashSequences)
		commandText = fixupEscapeSequences(commandText);

	const QPointer<WorldCommandProcessor>     processor(this);
	const QSharedPointer<CommandHistoryState> historyState = m_activeCommandHistoryState;
	const int                                 result       = executeCommandWithCompletion(
	    commandText,
	    [processor, historyText, historyState](const bool completed)
	    {
		    if (!processor)
			    return;
		    processor->finishCommandEntered(historyText, historyState, true, completed);
	    });
	if (result != eOK)
		finishCommandEntered(historyText, historyState, false, true);
}

void WorldCommandProcessor::finishCommandEntered(const QString                             &historyText,
                                                 const QSharedPointer<CommandHistoryState> &historyState,
                                                 const bool executionAccepted, const bool completed)
{
	m_processingEnteredCommand = false;
	if (const bool isKeypadCommand =
	        m_runtime && m_runtime->currentActionSource() == WorldRuntime::eUserKeypad;
	    completed && executionAccepted && !isKeypadCommand && historyState &&
	    !historyState->omitFromHistory && !historyState->sendFailed && m_view)
		m_view->addToHistory(historyText);
	if (auto operationCompletion = std::move(m_activeEnteredCommandCompletion); operationCompletion)
		operationCompletion(completed);
}

void WorldCommandProcessor::cancelActiveEnteredCommand()
{
	m_processingEnteredCommand = false;
	if (auto completion = std::move(m_activeEnteredCommandCompletion); completion)
		completion(false);
}

void WorldCommandProcessor::onIncomingLineReceived(const QString &line)
{
	onIncomingStyledLineReceived(line, QVector<WorldRuntime::StyleSpan>(),
	                             m_runtime ? m_runtime->m_incomingPartialLineRevision : 0,
	                             m_runtime && m_runtime->isConnected() && m_runtime->isNawsNegotiated());
}

void WorldCommandProcessor::onIncomingStyledLineReceived(const QString                          &line,
                                                         const QVector<WorldRuntime::StyleSpan> &spans,
                                                         const quint64 partialLineRevision,
                                                         const bool    serverSideWrapActive)
{
	PendingIncomingStreamItem request;
	request.line                 = line;
	request.spans                = spans;
	request.partialLineRevision  = partialLineRevision;
	request.serverSideWrapActive = serverSideWrapActive;
	request.presentationBatch    = m_runtime ? m_runtime->acquireIncomingStreamPresentationBatch() : nullptr;
	adoptIncomingStreamBacklogPresentationBatch(request);
	request.observation =
	    m_runtime ? m_runtime->captureIncomingLineLuaObservation() : LuaCallbackIncomingLineObservation{};
	if (m_processingInputCriticalOperation || m_processingIncomingLine || m_processingIncomingStreamAction)
	{
		const std::shared_ptr<LuaDeferredRuntimeMutationPlayback> playback =
		    m_runtime ? m_runtime->activeLuaDeferredRuntimeMutationPlayback() : nullptr;
		request.nestingPlayback = playback;
		if (m_processingIncomingLine && playback && m_activeIncomingLineOperation &&
		    m_activeIncomingLineOperation->nestingPlayback != playback)
		{
			request.inputCriticalOperationScope = m_activeIncomingLineOperation->inputCriticalOperationScope;
			startIncomingLine(std::move(request), m_activeIncomingLineOperation, playback);
			return;
		}
		m_pendingIncomingStreamItems.enqueue(std::move(request));
		return;
	}
	static_cast<void>(processPendingIncomingStreamItem(std::move(request), {}, {}));
}

void WorldCommandProcessor::startIncomingLine(
    PendingIncomingStreamItem request, const QSharedPointer<PendingIncomingLineOperation> &parent,
    const std::shared_ptr<LuaDeferredRuntimeMutationPlayback> &nestingPlayback)
{
	if (m_runtime)
		m_runtime->activateIncomingStreamPresentationBatch(request.presentationBatch);
	WorldRuntime::IncomingLineLuaObservationScope observationScope(m_runtime, request.observation);
	m_processingIncomingLine = true;
	const auto isEnabled     = [](const QString &value)
	{
		return value == QStringLiteral("1") || value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
		       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
	};

	const QString                          line        = std::move(request.line);
	const QVector<WorldRuntime::StyleSpan> spans       = std::move(request.spans);
	const QMap<QString, QString>          *attrs       = nullptr;
	bool                                   runtimeWrap = false;
	int                                    wrapColumn  = 0;
	bool                                   indentParas = true;
	QColor                                 defaultFore;
	QColor                                 defaultBack;
	bool                                   defaultColorsLoaded = false;
	auto                                   loadDefaultColors   = [&]
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
		runtimeWrap = wrapEnabled && !request.serverSideWrapActive && wrapColumn > 0;
		indentParas = !(
		    attrs->value(QStringLiteral("indent_paras")) == QStringLiteral("0") ||
		    attrs->value(QStringLiteral("indent_paras")).compare(QStringLiteral("n"), Qt::CaseInsensitive) ==
		        0 ||
		    attrs->value(QStringLiteral("indent_paras"))
		            .compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0);
	}

	QVector<WorldRuntime::StyleSpan> normalizedSpans =
	    QMudOutputWrapUtils::normalizeStyleSpansForTextLazily(line, spans,
	                                                          [&]
	                                                          {
		                                                          loadDefaultColors();
		                                                          WorldRuntime::StyleSpan fallback;
		                                                          fallback.fore = defaultFore;
		                                                          fallback.back = defaultBack;
		                                                          return fallback;
	                                                          });

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

	std::optional<WorldRuntime::IncomingLineLuaContextState> parentLineContext;
	if (m_runtime)
	{
		if (parent)
			parentLineContext = m_runtime->captureIncomingLineLuaContextState();
		m_runtime->beginIncomingLineLuaContext(line, WorldRuntime::LineOutput, normalizedSpans, true);
	}
	if (m_runtime)
		m_runtime->reserveIncomingLineLuaContextInBuffer(request.partialLineRevision);

	auto operation               = QSharedPointer<PendingIncomingLineOperation>::create();
	operation->processor         = this;
	operation->parent            = parent;
	operation->nestingPlayback   = nestingPlayback;
	operation->presentationBatch = std::move(request.presentationBatch);
	operation->inputCriticalOperationScope =
	    parent ? parent->inputCriticalOperationScope : request.inputCriticalOperationScope;
	operation->observation = request.observation;
	if (parentLineContext)
	{
		operation->restoreParentLineContext =
		    [runtime = QPointer<WorldRuntime>(m_runtime), state = std::move(*parentLineContext)]
		{
			if (runtime)
				runtime->restoreIncomingLineLuaContextState(state);
		};
	}
	operation->line                = line;
	operation->spans               = std::move(normalizedSpans);
	operation->partialLineRevision = request.partialLineRevision;
	operation->runtimeWrap         = runtimeWrap;
	operation->wrapColumn          = wrapColumn;
	operation->indentParas         = indentParas;
	operation->logOutput           = logOutput;
	m_activeIncomingLineOperation  = operation;
	if (!m_runtime)
	{
		continueIncomingLineAfterPluginDecision(operation, true);
		return;
	}
	auto continuationFactory = makeLuaCallbackResultContinuationFactory(
	    [operation]
	    {
		    PendingPluginLineDecision state;
		    state.operation = operation;
		    return state;
	    });
	LuaCallbackContinuationDispatchResult callback =
	    m_runtime->firePluginLineReceivedContinuable(line, continuationFactory);
	if (callback.suspended())
		return;
	continueIncomingLineAfterPluginDecision(
	    operation, callback.result.boolResultValid ? callback.result.boolResult : true);
}

void WorldCommandProcessor::continueIncomingLineAfterPluginDecision(
    const QSharedPointer<PendingIncomingLineOperation> &operation, const bool accepted)
{
	if (!operation || operation->processor != this)
		return;
	WorldRuntime::IncomingLineLuaObservationScope observationScope(m_runtime, operation->observation);
	const QString                                &line            = operation->line;
	const QVector<WorldRuntime::StyleSpan>       &normalizedSpans = operation->spans;
	const bool                                    runtimeWrap     = operation->runtimeWrap;
	const int                                     wrapColumn      = operation->wrapColumn;
	const bool                                    indentParas     = operation->indentParas;
	bool                                          logOutput       = operation->logOutput;
	QColor                                        defaultFore;
	QColor                                        defaultBack;
	bool                                          defaultColorsLoaded = false;
	auto                                          loadDefaultColors   = [&]
	{
		if (defaultColorsLoaded || !m_runtime)
			return;
		ensurePaletteCache(m_runtime->worldAttributes());
		defaultFore         = m_paletteCacheDefaultFore;
		defaultBack         = m_paletteCacheDefaultBack;
		defaultColorsLoaded = true;
	};
	if (m_runtime)
		m_runtime->beginOutputViewMutationBatch();
	[[maybe_unused]] const auto flushOutputViewMutations = qScopeGuard(
	    [this]
	    {
		    if (m_runtime)
			    m_runtime->endOutputViewMutationBatch();
	    });

	TriggerEvaluationResult triggerResult = processTriggersForLine(line, normalizedSpans);
	if (!accepted)
	{
		triggerResult.omitFromOutput = true;
		if (m_runtime)
			m_runtime->setLineOmittedFromOutput(true);
	}

	if (m_view)
	{
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
			if (updatedRuntimeLine)
			{
				m_runtime->completePartialOutputPresentation(
				    operation->partialLineRevision, m_runtime->incomingLineLuaContextAbsoluteNumber());
			}
			else
			{
				if (m_runtime)
					static_cast<void>(
					    m_runtime->clearPartialOutputPresentation(operation->partialLineRevision));
				else
					m_view->clearPartialOutput();
				if (displaySpans.isEmpty())
					m_view->appendOutputText(displayLine);
				else
					m_view->appendOutputTextStyled(displayLine, displaySpans);
			}
			if (m_runtime)
			{
				m_runtime->markIncomingLineLuaContextBuffered();
				m_runtime->firePluginScreendraw(0, logOutput && !triggerResult.omitFromLog ? 1 : 0, line);
			}
		}
		else
		{
			if (m_runtime)
				static_cast<void>(m_runtime->clearPartialOutputPresentation(operation->partialLineRevision));
			else
				m_view->clearPartialOutput();
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

	if (m_runtime && (!triggerResult.deferredScripts.isEmpty() || !triggerResult.triggerScripts.isEmpty()))
	{
		loadDefaultColors();
		operation->styleRuns = QSharedPointer<QVector<LuaStyleRun>>::create(
		    buildStyleRuns(line, triggerResult.spans, defaultFore, defaultBack));
	}
	operation->triggerMatchedLineBufferIndex = m_runtime ? m_runtime->incomingLineLuaContextBufferIndex() : 0;
	operation->triggerMatchedLineAbsoluteNumber =
	    m_runtime ? m_runtime->incomingLineLuaContextAbsoluteNumber() : 0;
	operation->triggerResult = std::move(triggerResult);
	advanceIncomingLineScripts(operation);
}

void WorldCommandProcessor::advanceIncomingLineScripts(
    const QSharedPointer<PendingIncomingLineOperation> &operation)

{
	if (!operation || operation->processor != this)
		return;
	WorldRuntime::IncomingLineLuaObservationScope observationScope(m_runtime, operation->observation);
	if (!m_runtime)
	{
		finishIncomingLine(operation);
		return;
	}

	while (operation->deferredScriptIndex < operation->triggerResult.deferredScripts.size())
	{
		const DeferredScript script =
		    operation->triggerResult.deferredScripts.at(operation->deferredScriptIndex++);
		if (!resolveTriggerByRuntimeId(m_runtime, script.triggerRuntimeLocationHandle,
		                               script.triggerRuntimeId, script.triggerIndexHint))
		{
			continue;
		}
		const unsigned short previousActionSource = m_runtime->currentActionSource();
		m_runtime->setCurrentActionSource(WorldRuntime::eTriggerFired);
		const auto restoreActionSource = qScopeGuard(
		    [runtime = QPointer<WorldRuntime>(m_runtime), previousActionSource]
		    {
			    if (runtime)
				    runtime->setCurrentActionSource(previousActionSource);
		    });
		emit sendToScriptRequested(
		    script.pluginId, script.scriptText, script.description, LuaCallbackEvent::TriggerScript,
		    script.styleRuns ? script.styleRuns.data() : operation->styleRuns.data(), true,
		    script.replaceMatchedLineOutput, operation->triggerMatchedLineBufferIndex,
		    operation->triggerMatchedLineAbsoluteNumber);
		Q_UNUSED(restoreActionSource);
		if (!resolveTriggerByRuntimeId(m_runtime, script.triggerRuntimeLocationHandle,
		                               script.triggerRuntimeId, script.triggerIndexHint))
		{
			continue;
		}
		QSharedPointer<LuaCallbackEngine> engine;
		if (script.pluginId.isEmpty())
		{
			if (!m_runtime->luaScriptingAvailable())
			{
				if (isEnabledValue(
				        m_runtime->worldAttributes().value(QStringLiteral("warn_if_scripting_inactive"))))
				{
					const QString name =
					    script.description.isEmpty() ? QStringLiteral("(unnamed)") : script.description;
					m_runtime->outputText(
					    QStringLiteral(
					        "Script function \"%1\" cannot execute - scripting disabled/parse error.")
					        .arg(name),
					    true, true);
				}
				continue;
			}
			LuaCallbackEngine *worldEngine = m_runtime->luaCallbacks();
			if (!worldEngine)
				continue;
			engine = QSharedPointer<LuaCallbackEngine>(worldEngine, [](LuaCallbackEngine * /*unused*/) {});
		}
		else
		{
			WorldRuntime::Plugin *plugin = WorldCommandProcessorMutationAccess::pluginForLocation(
			    *m_runtime, script.pluginRuntimeLocationHandle, script.pluginRuntimeInstanceId, -1);
			if (!plugin || !plugin->enabled || plugin->installPending || !plugin->lua ||
			    plugin->lua->instanceId() != script.pluginEngineInstanceId)
			{
				continue;
			}
			engine = plugin->lua;
		}

		std::shared_ptr<TriggerExecutionScope> executionScope;
		if (script.trackTriggerExecution)
		{
			executionScope = std::make_shared<TriggerExecutionScope>(
			    m_runtime, script.triggerRuntimeLocationHandle, script.triggerRuntimeId, script.pluginId,
			    false, script.triggerIndexHint);
		}
		const QPointer<WorldCommandProcessor> processor(this);
		const QVector<LuaStyleRun>           *styleRuns =
		    script.styleRuns ? script.styleRuns.data() : operation->styleRuns.data();
		m_runtime->dispatchLuaExecuteScriptAsync(
		    engine, script.scriptText, script.description, LuaCallbackEvent::TriggerScript, styleRuns, true,
		    script.replaceMatchedLineOutput, operation->triggerMatchedLineBufferIndex,
		    operation->triggerMatchedLineAbsoluteNumber,
		    [processor, operation, executionScope](const bool /*succeeded*/) mutable
		    {
			    std::function<void()> resumeLine =
			        [processor, operation, executionScope = std::move(executionScope)]() mutable
			    {
				    executionScope.reset();
				    if (processor)
					    processor->advanceIncomingLineScripts(operation);
			    };
			    if (processor)
				    processor->runWithInputCriticalOperationScope(operation->inputCriticalOperationScope,
				                                                  resumeLine);
		    });
		return;
	}

	while (operation->triggerScriptIndex < operation->triggerResult.triggerScripts.size())
	{
		const TriggerScript script =
		    operation->triggerResult.triggerScripts.at(operation->triggerScriptIndex++);
		if (script.scriptName.isEmpty())
			continue;
		WorldRuntime::Trigger *trigger = resolveTriggerByRuntimeId(m_runtime, script.runtimeLocationHandle,
		                                                           script.runtimeId, script.indexHint);
		if (!trigger)
			continue;
		WorldRuntime::Plugin *plugin = nullptr;
		if (!script.pluginId.isEmpty())
		{
			plugin = WorldCommandProcessorMutationAccess::pluginForLocation(
			    *m_runtime, script.pluginRuntimeLocationHandle, script.pluginRuntimeInstanceId,
			    script.pluginIndexHint);
		}
		if (!script.pluginId.isEmpty() &&
		    (!plugin || !plugin->enabled || plugin->installPending || !plugin->lua ||
		     plugin->lua->instanceId() != script.pluginEngineInstanceId))
		{
			continue;
		}
		QSharedPointer<LuaCallbackEngine> engine = plugin ? plugin->lua : QSharedPointer<LuaCallbackEngine>{};
		LuaCallbackEngine                *lua    = engine ? engine.data() : m_runtime->luaCallbacks();
		if (!plugin)
		{
			if (const auto presence = operation->worldFunctionPresence.constFind(script.scriptName);
			    presence != operation->worldFunctionPresence.constEnd() && !presence.value())
			{
				warnMissingWorldScriptFunction(QStringLiteral("Trigger"), script.scriptName);
				continue;
			}
			if (!canExecuteWorldScript(QStringLiteral("Trigger"), script.scriptName, lua))
				continue;
		}
		if (!lua)
			continue;
		if (!engine)
			engine = QSharedPointer<LuaCallbackEngine>(lua, [](LuaCallbackEngine * /*unused*/) {});

		auto executionScope =
		    std::make_shared<TriggerExecutionScope>(m_runtime, script.runtimeLocationHandle, script.runtimeId,
		                                            script.pluginId, true, script.indexHint);
		const QPointer<WorldCommandProcessor> processor(this);
		m_runtime->dispatchLuaStringsAndWildcardsAsync(
		    engine, script.scriptName, LuaCallbackEvent::TriggerRoutine, {script.label, script.line},
		    script.wildcards, script.namedWildcards, operation->styleRuns.data(), WorldRuntime::eTriggerFired,
		    script.replaceMatchedLineOutput, operation->triggerMatchedLineBufferIndex,
		    operation->triggerMatchedLineAbsoluteNumber,
		    [processor, operation, executionScope, functionName = script.scriptName,
		     worldFunction = !plugin](const LuaBatchDispatchResult &result) mutable
		    {
			    const bool            hasFunctionValid = result.hasFunctionValid;
			    const bool            hasFunction      = result.hasFunction;
			    std::function<void()> resumeLine = [processor, operation,
			                                        executionScope = std::move(executionScope), functionName,
			                                        worldFunction, hasFunctionValid, hasFunction]() mutable
			    {
				    executionScope.reset();
				    if (!processor)
					    return;
				    if (worldFunction && hasFunctionValid)
				    {
					    operation->worldFunctionPresence.insert(functionName, hasFunction);
					    if (!hasFunction)
						    processor->warnMissingWorldScriptFunction(QStringLiteral("Trigger"),
						                                              functionName);
				    }
				    processor->advanceIncomingLineScripts(operation);
			    };
			    if (processor)
				    processor->runWithInputCriticalOperationScope(operation->inputCriticalOperationScope,
				                                                  resumeLine);
		    });
		return;
	}

	finishIncomingLine(operation);
}

void WorldCommandProcessor::finishIncomingLine(const QSharedPointer<PendingIncomingLineOperation> &operation)
{
	if (!operation || operation->processor != this)
		return;
	WorldRuntime::IncomingLineLuaObservationScope observationScope(m_runtime, operation->observation);
	TriggerEvaluationResult                      &triggerResult = operation->triggerResult;
	const QString                                &line          = operation->line;

	if (m_runtime && !triggerResult.oneShotTriggers.isEmpty())
	{
		m_runtime->beginLuaCallbackSnapshotMutationBatch();
		const auto finishSnapshotBatch = qScopeGuard(
		    [runtime = QPointer<WorldRuntime>(m_runtime)]
		    {
			    if (runtime)
				    runtime->endLuaCallbackSnapshotMutationBatch();
		    });
		for (const RuleRef &ref : triggerResult.oneShotTriggers)
		{
			if (!removeTriggerByRuntimeId(m_runtime, ref.runtimeLocationHandle, ref.runtimeId, ref.pluginId,
			                              ref.pluginRuntimeLocationHandle, ref.pluginRuntimeInstanceId,
			                              ref.pluginIndexHint, ref.indexHint))
				continue;
			if (ref.pluginId.isEmpty())
				m_runtime->markTriggersChanged();
			else
				m_runtime->markPluginTriggersChanged(ref.pluginId);
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
		const bool logOutput =
		    isEnabledValue(m_runtime->worldAttributes().value(QStringLiteral("log_output")));
		if (logOutput && !triggerResult.omitFromLog)
			logLine(line, QStringLiteral("log_line_preamble_output"),
			        QStringLiteral("log_line_postamble_output"));
		m_runtime->endIncomingLineLuaContext();
	}
	const QSharedPointer<PendingIncomingLineOperation>        parent          = operation->parent;
	const std::shared_ptr<LuaDeferredRuntimeMutationPlayback> nestingPlayback = operation->nestingPlayback;
	if (operation->restoreParentLineContext)
		operation->restoreParentLineContext();
	if (m_activeIncomingLineOperation == operation)
		m_activeIncomingLineOperation = parent;
	operation->processor.clear();
	operation->presentationBatch.clear();
	if (parent)
	{
		drainNestedIncomingLines(parent, nestingPlayback);
		finishIncomingStreamBacklogPresentationBatchIfIdle();
		return;
	}
	m_processingIncomingLine = false;
	finishInputCriticalOperation(operation->inputCriticalOperationScope, true);
}

void WorldCommandProcessor::cancelIncomingLine(const QSharedPointer<PendingIncomingLineOperation> &operation)
{
	if (!operation || operation->processor != this)
		return;
	WorldRuntime::IncomingLineLuaObservationScope observationScope(m_runtime, operation->observation);
	if (m_runtime)
		m_runtime->endIncomingLineLuaContext();
	m_processingIncomingLine = false;
	for (QSharedPointer<PendingIncomingLineOperation> current = operation; current; current = current->parent)
	{
		current->processor.clear();
		current->presentationBatch.clear();
	}
	m_activeIncomingLineOperation.clear();
	m_incomingStreamBacklogPresentationBatch.clear();
	cancelInputCriticalOperations();
}

void WorldCommandProcessor::adoptIncomingStreamBacklogPresentationBatch(PendingIncomingStreamItem &item)
{
	if (m_incomingStreamBacklogPresentationBatch)
	{
		item.presentationBatch = m_incomingStreamBacklogPresentationBatch;
		return;
	}
	if (item.presentationBatch)
		m_incomingStreamBacklogPresentationBatch = item.presentationBatch;
}

void WorldCommandProcessor::finishIncomingStreamBacklogPresentationBatchIfIdle()
{
	if (m_processingInputCriticalOperation || m_processingIncomingLine || m_processingIncomingStreamAction ||
	    !m_pendingIncomingStreamItems.isEmpty())
	{
		return;
	}
	m_incomingStreamBacklogPresentationBatch.clear();
}

WorldCommandProcessor::InputCriticalOperationScopePtr
WorldCommandProcessor::createInputCriticalOperationScope(const bool joinActiveOperation)
{
	auto scope       = InputCriticalOperationScopePtr::create();
	scope->processor = this;
	do
	{
		scope->inputCriticalOperationId = m_nextInputCriticalOperationId++;
	} while (scope->inputCriticalOperationId == 0);
	if (joinActiveOperation && m_activeOperationCompletion)
	{
		scope->parent = m_activeOperationCompletion;
		scope->parent->retain();
	}
	const QWeakPointer<LogicalOperationCompletion> weakScope(scope);
	scope->completion =
	    [processor = QPointer<WorldCommandProcessor>(this), weakScope](const bool /*completed*/)
	{
		if (processor)
			processor->completeInputCriticalOperation(weakScope.toStrongRef());
	};
	return scope;
}

bool WorldCommandProcessor::hasCurrentInputCriticalOperation() const
{
	return m_currentInputCriticalOperationScope &&
	       !m_currentInputCriticalOperationScope->inputCriticalFinalized;
}

void WorldCommandProcessor::finishInputCriticalOperation(const InputCriticalOperationScopePtr &scope,
                                                         const bool                            completed)
{
	if (!scope || scope->inputCriticalFinalized || scope->inputCriticalCompletionRequested)
		return;
	scope->inputCriticalCompletionRequested = true;
	scope->release(completed);
}

void WorldCommandProcessor::completeInputCriticalOperation(const InputCriticalOperationScopePtr &scope)
{
	if (!scope || scope->inputCriticalFinalized)
		return;
	scope->inputCriticalFinalized = true;
	if (m_activeInputCriticalOperationScope != scope)
		return;
	m_activeInputCriticalOperationScope.clear();
	m_processingInputCriticalOperation = false;
	drainPendingIncomingLines();
	finishIncomingStreamBacklogPresentationBatchIfIdle();
}

void WorldCommandProcessor::cancelInputCriticalOperations()
{
	QQueue<PendingIncomingStreamItem> pending;
	pending.swap(m_pendingIncomingStreamItems);
	const InputCriticalOperationScopePtr active = std::exchange(m_activeInputCriticalOperationScope, {});
	m_processingInputCriticalOperation          = false;
	m_currentInputCriticalOperationScope.clear();

	auto cancelScope = [](const InputCriticalOperationScopePtr &scope)
	{
		if (!scope || scope->inputCriticalFinalized)
			return;
		scope->inputCriticalFinalized = true;
		if (!scope->inputCriticalCompletionRequested && !scope->completed)
		{
			scope->inputCriticalCompletionRequested = true;
			scope->release(false);
		}
	};
	cancelScope(active);
	while (!pending.isEmpty())
		cancelScope(pending.dequeue().inputCriticalOperationScope);
}

void WorldCommandProcessor::enqueueOrRunInputCriticalAction(
    PendingIncomingStreamItem::InputCriticalAction action)
{
	if (!action)
		return;
	PendingIncomingStreamItem item;
	item.kind                        = PendingIncomingStreamItem::Kind::InputCriticalAction;
	item.inputCriticalAction         = std::move(action);
	item.inputCriticalOperationScope = createInputCriticalOperationScope(true);
	item.inputCriticalActionContext  = captureCommandExecutionContext();
	item.inputCriticalActionContext.inputCriticalOperationScope = item.inputCriticalOperationScope;
	item.inputCriticalActionContext.operationCompletion         = item.inputCriticalOperationScope;
	// External commands divide presentation produced by earlier and later network deliveries.
	m_incomingStreamBacklogPresentationBatch.clear();
	if (m_processingInputCriticalOperation || m_processingIncomingLine || m_processingIncomingStreamAction)
	{
		item.inputCriticalCallerContext = captureRuntimeCallerContextRunner();
		m_pendingIncomingStreamItems.enqueue(std::move(item));
		return;
	}
	static_cast<void>(processPendingIncomingStreamItem(std::move(item), {}, {}));
	drainPendingIncomingLines();
	finishIncomingStreamBacklogPresentationBatchIfIdle();
}

void WorldCommandProcessor::enqueueOrRunIncomingStreamAction(PendingIncomingStreamItem::OrderedAction action)
{
	if (!action)
		return;
	PendingIncomingStreamItem item;
	item.kind              = PendingIncomingStreamItem::Kind::OrderedAction;
	item.action            = std::move(action);
	item.nestingPlayback   = m_runtime ? m_runtime->activeLuaDeferredRuntimeMutationPlayback() : nullptr;
	item.presentationBatch = m_runtime ? m_runtime->acquireIncomingStreamPresentationBatch() : nullptr;
	adoptIncomingStreamBacklogPresentationBatch(item);
	item.observation =
	    m_runtime ? m_runtime->captureIncomingLineLuaObservation() : LuaCallbackIncomingLineObservation{};
	item.actionSource  = m_runtime ? m_runtime->currentActionSource()
	                               : static_cast<unsigned short>(WorldRuntime::eUnknownActionSource);
	item.doingSimulate = m_runtime && m_runtime->doingSimulate();
	if (m_processingInputCriticalOperation || m_processingIncomingLine || m_processingIncomingStreamAction)
	{
		m_pendingIncomingStreamItems.enqueue(std::move(item));
		return;
	}
	static_cast<void>(processPendingIncomingStreamItem(std::move(item), {}, {}));
	drainPendingIncomingLines();
	finishIncomingStreamBacklogPresentationBatchIfIdle();
}

bool WorldCommandProcessor::processPendingIncomingStreamItem(
    PendingIncomingStreamItem item, const QSharedPointer<PendingIncomingLineOperation> &parent,
    const std::shared_ptr<LuaDeferredRuntimeMutationPlayback> &nestingPlayback)
{
	const bool topLevel = !parent;
	if (topLevel)
	{
		Q_ASSERT(!m_processingInputCriticalOperation);
		if (!item.inputCriticalOperationScope)
			item.inputCriticalOperationScope = createInputCriticalOperationScope();
		m_activeInputCriticalOperationScope = item.inputCriticalOperationScope;
		m_processingInputCriticalOperation  = true;
	}
	const InputCriticalOperationScopePtr processedScope = item.inputCriticalOperationScope;

	bool                                 processingBecameAsynchronous = false;
	auto                                 processItem                  = [&]
	{
		switch (item.kind)
		{
		case PendingIncomingStreamItem::Kind::CompleteLine:
			startIncomingLine(std::move(item), parent, nestingPlayback);
			processingBecameAsynchronous =
			    parent ? m_activeIncomingLineOperation != parent : m_processingIncomingLine;
			return;
		case PendingIncomingStreamItem::Kind::PartialLine:
			if (m_runtime)
				m_runtime->activateIncomingStreamPresentationBatch(item.presentationBatch);
			if (!m_runtime || item.partialLineRevision == m_runtime->m_incomingPartialLineRevision)
				presentIncomingPartialLine(item.line, item.spans, item.partialLineRevision,
				                           item.serverSideWrapActive);
			return;
		case PendingIncomingStreamItem::Kind::OrderedAction:
		{
			if (m_runtime)
				m_runtime->activateIncomingStreamPresentationBatch(item.presentationBatch);
			WorldRuntime::IncomingLineLuaObservationScope observationScope(m_runtime, item.observation);
			const unsigned short                          previousActionSource =
			    m_runtime ? m_runtime->currentActionSource()
			              : static_cast<unsigned short>(WorldRuntime::eUnknownActionSource);
			const bool previousDoingSimulate = m_runtime && m_runtime->doingSimulate();
			const auto restoreRuntimeContext = qScopeGuard(
			    [runtime = QPointer<WorldRuntime>(m_runtime), previousActionSource, previousDoingSimulate]
			    {
				    if (!runtime)
					    return;
				    runtime->setCurrentActionSource(previousActionSource);
				    runtime->m_doingSimulate = previousDoingSimulate;
			    });
			if (m_runtime)
			{
				m_runtime->setCurrentActionSource(item.actionSource);
				m_runtime->m_doingSimulate = item.doingSimulate;
			}
			m_processingIncomingStreamAction = true;
			const auto finishAction = qScopeGuard([this] { m_processingIncomingStreamAction = false; });
			Q_UNUSED(finishAction);
			if (item.action)
				item.action();
			return;
		}
		case PendingIncomingStreamItem::Kind::InputCriticalAction:
		{
			const InputCriticalOperationScopePtr scope     = item.inputCriticalOperationScope;
			const auto                           runAction = [&]
			{
				if (!item.inputCriticalAction)
					return;
				item.inputCriticalAction(
				    [processor = QPointer<WorldCommandProcessor>(this), scope](const bool completed)
				    {
					    if (processor)
						    WorldCommandProcessor::finishInputCriticalOperation(scope, completed);
				    });
			};
			const auto runWithInputCriticalContext = [&]
			{ runWithCommandExecutionContext(item.inputCriticalActionContext, runAction); };
			if (item.inputCriticalCallerContext)
				item.inputCriticalCallerContext.run(runWithInputCriticalContext);
			else
				runWithInputCriticalContext();
			processingBecameAsynchronous =
			    m_processingInputCriticalOperation && m_activeInputCriticalOperationScope == scope;
			return;
		}
		}
		Q_UNREACHABLE();
	};

	if (topLevel)
	{
		CommandExecutionContext operationContext     = captureCommandExecutionContext();
		operationContext.operationCompletion         = processedScope;
		operationContext.inputCriticalOperationScope = processedScope;
		runWithCommandExecutionContext(operationContext, processItem);
	}
	else
		processItem();

	if (topLevel && !processingBecameAsynchronous && m_activeInputCriticalOperationScope == processedScope)
		finishInputCriticalOperation(processedScope);
	return processingBecameAsynchronous;
}

void WorldCommandProcessor::drainNestedIncomingLines(
    const QSharedPointer<PendingIncomingLineOperation>        &parent,
    const std::shared_ptr<LuaDeferredRuntimeMutationPlayback> &nestingPlayback)
{
	drainPendingIncomingStreamItems(parent, nestingPlayback, true);
}

void WorldCommandProcessor::drainPendingIncomingLines()
{
	drainPendingIncomingStreamItems({}, {}, false);
}

void WorldCommandProcessor::drainPendingIncomingStreamItems(
    const QSharedPointer<PendingIncomingLineOperation>        &parent,
    const std::shared_ptr<LuaDeferredRuntimeMutationPlayback> &nestingPlayback, const bool nestedOnly)
{
	const PendingIncomingStreamDrainFrame frame{parent.data(), nestingPlayback.get(), nestedOnly};
	if (!m_pendingIncomingStreamDrainStack.isEmpty())
	{
		const PendingIncomingStreamDrainFrame &active = m_pendingIncomingStreamDrainStack.constLast();
		if (active.parent == frame.parent && active.nestingPlayback == frame.nestingPlayback &&
		    active.nestedOnly == frame.nestedOnly)
		{
			return;
		}
	}
	m_pendingIncomingStreamDrainStack.push_back(frame);
	const auto finishDrain = qScopeGuard(
	    [this]
	    {
		    m_pendingIncomingStreamDrainStack.removeLast();
		    finishIncomingStreamBacklogPresentationBatchIfIdle();
	    });
	Q_UNUSED(finishDrain);
	while (!m_pendingIncomingStreamItems.isEmpty())
	{
		if (nestedOnly)
		{
			if (!parent || m_activeIncomingLineOperation != parent ||
			    m_pendingIncomingStreamItems.constFirst().nestingPlayback != nestingPlayback)
			{
				return;
			}
		}
		else if (m_processingInputCriticalOperation || m_processingIncomingLine ||
		         m_processingIncomingStreamAction)
		{
			return;
		}
		PendingIncomingStreamItem item = m_pendingIncomingStreamItems.dequeue();
		if (processPendingIncomingStreamItem(std::move(item), parent, nestingPlayback))
			return;
	}
}

void WorldCommandProcessor::onIncomingStyledLinePartialReceived(const QString                          &line,
                                                                const QVector<WorldRuntime::StyleSpan> &spans,
                                                                const quint64 partialLineRevision,
                                                                const bool    serverSideWrapActive)
{
	PendingIncomingStreamItem request;
	request.kind                 = PendingIncomingStreamItem::Kind::PartialLine;
	request.line                 = line;
	request.spans                = spans;
	request.partialLineRevision  = partialLineRevision;
	request.serverSideWrapActive = serverSideWrapActive;
	request.nestingPlayback   = m_runtime ? m_runtime->activeLuaDeferredRuntimeMutationPlayback() : nullptr;
	request.presentationBatch = m_runtime ? m_runtime->acquireIncomingStreamPresentationBatch() : nullptr;
	adoptIncomingStreamBacklogPresentationBatch(request);
	if (m_processingInputCriticalOperation || m_processingIncomingLine || m_processingIncomingStreamAction)
	{
		if (!m_pendingIncomingStreamItems.isEmpty() &&
		    m_pendingIncomingStreamItems.constLast().kind == PendingIncomingStreamItem::Kind::PartialLine &&
		    m_pendingIncomingStreamItems.constLast().partialLineRevision == request.partialLineRevision &&
		    m_pendingIncomingStreamItems.constLast().nestingPlayback == request.nestingPlayback &&
		    m_pendingIncomingStreamItems.constLast().presentationBatch == request.presentationBatch)
			m_pendingIncomingStreamItems.last() = std::move(request);
		else
			m_pendingIncomingStreamItems.enqueue(std::move(request));
		return;
	}
	if (m_runtime)
		m_runtime->activateIncomingStreamPresentationBatch(request.presentationBatch);
	if (!m_runtime || request.partialLineRevision == m_runtime->m_incomingPartialLineRevision)
		presentIncomingPartialLine(request.line, request.spans, request.partialLineRevision,
		                           request.serverSideWrapActive);
	finishIncomingStreamBacklogPresentationBatchIfIdle();
}

void WorldCommandProcessor::presentIncomingPartialLine(const QString                          &line,
                                                       const QVector<WorldRuntime::StyleSpan> &spans,
                                                       const quint64 partialLineRevision,
                                                       const bool    serverSideWrapActive) const
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

		if (wrapEnabled && !serverSideWrapActive && wrapColumn > 0)
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

	if (m_runtime)
	{
		if (m_runtime->publishPartialOutputPresentation(displayLine, displaySpans, partialLineRevision))
			m_runtime->firePluginPartialLine(line);
	}
	else
	{
		m_view->updatePartialOutputText(displayLine, displaySpans);
	}
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

	const QPointer<WorldRuntime> runtimeGuard(m_runtime);
	const unsigned short         previousActionSource =
	    runtimeGuard ? runtimeGuard->currentActionSource()
	                 : static_cast<unsigned short>(WorldRuntime::eUnknownActionSource);
	if (runtimeGuard)
		runtimeGuard->setCurrentActionSource(WorldRuntime::eUserTyping);
	[[maybe_unused]] const auto restoreActionSource = qScopeGuard(
	    [runtimeGuard, previousActionSource]
	    {
		    if (runtimeGuard)
			    runtimeGuard->setCurrentActionSource(previousActionSource);
	    });
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
		sendRawText(sendText, echo, false, true, false);
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
	if (rejectInvalidOutboundRequest())
		return;
	if (!hasCurrentInputCriticalOperation())
	{
		enqueueOrRunInputCriticalAction(
		    [processor = QPointer<WorldCommandProcessor>(this), text, echo, queue, log,
		     history](CommandCompletion completion)
		    {
			    if (!processor)
			    {
				    if (completion)
					    completion(false);
				    return;
			    }
			    processor->runOrderedRuleAction([processor, text, echo, queue, log, history]
			                                    { processor->sendRawText(text, echo, queue, log, history); },
			                                    std::move(completion));
		    });
		return;
	}
	sendMsg(text, echo, queue, log);
	if (history && m_view)
		m_view->addToHistoryForced(text);
}

void WorldCommandProcessor::sendRawTextWithTriggerPriority(const QString &text, const bool echo,
                                                           const bool queue, const bool log,
                                                           const bool history)
{
	if (rejectInvalidOutboundRequest())
		return;
	if (!hasCurrentInputCriticalOperation())
	{
		enqueueOrRunInputCriticalAction(
		    [processor = QPointer<WorldCommandProcessor>(this), text, echo, queue, log,
		     history](CommandCompletion completion)
		    {
			    if (!processor)
			    {
				    if (completion)
					    completion(false);
				    return;
			    }
			    processor->runOrderedRuleAction(
			        [processor, text, echo, queue, log, history]
			        { processor->sendRawTextWithTriggerPriority(text, echo, queue, log, history); },
			        std::move(completion));
		    });
		return;
	}
	if (m_pendingTriggerPriorityCommands > 0 && m_triggerPriorityActionExecutionDepth == 0)
	{
		runWithTriggerPriorityOutboundGroup(
		    [this, &text, echo, queue, log, history]
		    {
			    ++m_triggerCommandPriorityDepth;
			    const auto restoreTriggerCommandPriority =
			        qScopeGuard([this] { --m_triggerCommandPriorityDepth; });
			    Q_UNUSED(restoreTriggerCommandPriority);
			    sendRawText(text, echo, queue, log, history);
		    });
		return;
	}

	ensureTriggerPriorityOutboundGroup();
	++m_pendingTriggerPriorityCommands;
	const QPointer<WorldCommandProcessor> processor(this);
	auto                                  actionCompletion = createLogicalOperationCompletion(
	    [processor](const bool completed)
	    {
		    if (processor)
			    processor->finishTriggerPriorityCommand(completed);
	    });
	runWithTriggerPriorityOutboundGroup(
	    [&]
	    {
		    ++m_triggerCommandPriorityDepth;
		    ++m_triggerPriorityActionExecutionDepth;
		    const LogicalOperationCompletionPtr previousOperationCompletion = m_activeOperationCompletion;
		    m_activeOperationCompletion                                     = actionCompletion;
		    const auto restoreTriggerContext                                = qScopeGuard(
		        [this, previousOperationCompletion]
		        {
			        m_activeOperationCompletion = previousOperationCompletion;
			        --m_triggerPriorityActionExecutionDepth;
			        --m_triggerCommandPriorityDepth;
		        });
		    Q_UNUSED(restoreTriggerContext);
		    sendRawText(text, echo, queue, log, history);
	    });
	actionCompletion->release(true);
}

void WorldCommandProcessor::sendImmediateText(const QString &text, const bool echo, const bool log,
                                              const bool history)
{
	if (rejectInvalidOutboundRequest())
		return;
	if (!hasCurrentInputCriticalOperation())
	{
		enqueueOrRunInputCriticalAction(
		    [processor = QPointer<WorldCommandProcessor>(this), text, echo, log,
		     history](CommandCompletion completion)
		    {
			    if (!processor)
			    {
				    if (completion)
					    completion(false);
				    return;
			    }
			    processor->runOrderedRuleAction([processor, text, echo, log, history]
			                                    { processor->sendImmediateText(text, echo, log, history); },
			                                    std::move(completion));
		    });
		return;
	}
	doSendMsg(text, echo, log, OutboundTransport::Immediate);
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
	if (!hasCurrentInputCriticalOperation())
	{
		enqueueOrRunInputCriticalAction(
		    [processor = QPointer<WorldCommandProcessor>(this), connectMethod, player, password, connectText,
		     echoInput](CommandCompletion completion)
		    {
			    if (!processor)
			    {
				    if (completion)
					    completion(false);
				    return;
			    }
			    processor->runOrderedRuleAction(
			        [processor, connectMethod, player, password, connectText, echoInput]
			        {
				        processor->runWorldConnectActions(connectMethod, player, password, connectText,
				                                          echoInput);
			        },
			        std::move(completion));
		    });
		return;
	}
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
	const QPointer<WorldCommandProcessor> self(this);
	m_runtime->fireWorldDisconnectHandlers(
	    [self](const bool /*cancelled*/)
	    {
		    if (self)
			    self->finishWorldDisconnected();
	    });
}

void WorldCommandProcessor::finishWorldDisconnected()
{
	if (!m_runtime)
		return;
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

	const QPointer<WorldCommandProcessor> self(this);
	const QPointer<WorldRuntime>          runtime(m_runtime);
	m_runtime->saveWorldFileAsync(
	    savePath,
	    [self, runtime, savePath](const bool saved, const QString &saveError)
	    {
		    if (!self || !runtime || self->m_runtime != runtime.data())
			    return;
		    if (!saved)
		    {
			    if (!saveError.isEmpty())
				    self->note(QStringLiteral("Unable to save world on disconnect: %1").arg(saveError), true);
			    return;
		    }

		    QString backupError;
		    if (!createDisconnectWeekdayBackup(savePath, *runtime, AppController::instance(), backupError) &&
		        !backupError.isEmpty())
		    {
			    self->note(QStringLiteral("Unable to create disconnect backup: %1").arg(backupError), true);
		    }
	    });
}

bool WorldCommandProcessor::evaluateCommand(const QString &input, const bool allowCommandStacking,
                                            CommandCompletion completion)
{
	if (!m_runtime)
	{
		if (completion)
			completion(false);
		return false;
	}

	LogicalOperationCompletionPtr evaluationCompletion;
	if (completion)
	{
		evaluationCompletion = createLogicalOperationCompletion(std::move(completion));
		completion           = [evaluationCompletion](const bool completed)
		{ evaluationCompletion->release(completed); };
	}

	bool       completionDeferred = false;
	const auto finishEvaluation   = qScopeGuard(
	    [&completionDeferred, &completion]
	    {
		    if (!completionDeferred && completion)
			    completion(true);
	    });
	Q_UNUSED(finishEvaluation);
	const LogicalOperationCompletionPtr previousOperationCompletion = m_activeOperationCompletion;
	if (evaluationCompletion)
		m_activeOperationCompletion = evaluationCompletion;
	const auto restoreOperationCompletion = qScopeGuard(
	    [this, previousOperationCompletion] { m_activeOperationCompletion = previousOperationCompletion; });
	Q_UNUSED(restoreOperationCompletion);

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
			if (m_activeCommandHistoryState)
				m_activeCommandHistoryState->sendFailed = true;
			return true;
		}

		const QStringList commands = line.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
		if (reEvaluateAutoSay)
		{
			QStringList prefixedCommands;
			prefixedCommands.reserve(commands.size());
			for (const QString &command : commands)
				prefixedCommands.push_back(autoSayString + command);
			completionDeferred = true;
			evaluateCommandSequence(std::move(prefixedCommands), std::move(completion), true);
			return false;
		}
		for (const QString &command : commands)
			sendMsg(autoSayString + command, echoInput, false, true);
		return false;
	}

	// -------------------------- SCRIPT PREFIX ---------------------------
	const bool scriptingEnabled = m_runtime->luaScriptingAvailable();
	if (const QString scriptPrefix = attrs.value(QStringLiteral("script_prefix"));
	    !m_processingAutoSay && scriptingEnabled && !scriptPrefix.isEmpty() && line.startsWith(scriptPrefix))
	{
		const QString code = line.mid(scriptPrefix.size());
		sendTo(eSendToScript, code, false, false, QString(), QStringLiteral("script prefix"), QString());
		return false;
	}

	// ---------------------- COMMAND STACKING ------------------------------
	const QString commandStackEnabled = attrs.value(QStringLiteral("enable_command_stack"));
	const bool    enableCommandStack  = isEnabledValue(commandStackEnabled);
	if (const QString commandStackSeparator = attrs.value(QStringLiteral("command_stack_character"));
	    allowCommandStacking && enableCommandStack &&
	    QMudCommandStack::isValidSeparator(commandStackSeparator))
	{
		const QStringList commands = QMudCommandStack::expand(line, commandStackSeparator);
		if (commands.size() > 1)
		{
			completionDeferred = true;
			evaluateCommandSequence(commands, std::move(completion));
			return false;
		}
		line = commands.constFirst();
	}

	// ------------------------- PLUGIN COMMAND CALLBACK -------------------
	// Legacy Execute() calls OnPluginCommand per stacked command before normal
	// command evaluation; false return omits this command.
	if (m_runtime)
	{
		auto continuationFactory = makeLuaCallbackResultContinuationFactory(
		    [processor = QPointer<WorldCommandProcessor>(this), line, echoInput, &completion]() mutable
		    {
			    PendingPluginCommandEvaluation state;
			    state.processor  = processor;
			    state.line       = line;
			    state.echoInput  = echoInput;
			    state.completion = std::move(completion);
			    return state;
		    });
		LuaCallbackContinuationDispatchResult callback =
		    m_runtime->firePluginCommandContinuable(line, continuationFactory);
		if (callback.suspended())
		{
			completionDeferred = true;
			return false;
		}
		if (callback.result.boolResultValid && !callback.result.boolResult)
			return false;
	}

	return continueEvaluateCommandAfterPluginCommand(line, echoInput);
}

void WorldCommandProcessor::evaluateCommandSequence(QStringList commands, CommandCompletion completion,
                                                    const bool autoSayEvaluation)
{
	auto sequence                   = std::make_shared<CommandEvaluationSequence>();
	sequence->processor             = this;
	sequence->commands              = std::move(commands);
	sequence->completion            = std::move(completion);
	sequence->autoSayEvaluation     = autoSayEvaluation;
	CommandExecutionContext context = captureCommandExecutionContext();
	context.processingAutoSay       = autoSayEvaluation;
	const QPointer<WorldCommandProcessor> processor(this);
	sequence->callerContext = CallerContextRunner(
	    [processor, context](const std::function<void()> &work)
	    {
		    if (processor)
			    processor->runWithCommandExecutionContext(context, work);
	    });
	sequence->advance();
}

void WorldCommandProcessor::runOrderedRuleAction(const std::function<void()> &action,
                                                 CommandCompletion            completion)
{
	auto actionCompletion = createLogicalOperationCompletion(std::move(completion));
	const LogicalOperationCompletionPtr previousOperationCompletion = m_activeOperationCompletion;
	m_activeOperationCompletion                                     = actionCompletion;
	const auto restoreOperation = qScopeGuard([this, previousOperationCompletion]
	                                          { m_activeOperationCompletion = previousOperationCompletion; });
	Q_UNUSED(restoreOperation);
	if (action)
		action();
	actionCompletion->release(true);
}

bool WorldCommandProcessor::continueEvaluateCommandAfterPluginCommand(const QString &line,
                                                                      const bool     echoInput)
{
	if (!m_runtime)
		return false;
	const QMap<QString, QString> &attrs = m_runtime->worldAttributes();

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
	auto sequence       = std::make_shared<AliasEvaluationSequence>();
	sequence->processor = this;
	sequence->runtime   = m_runtime;
	sequence->line      = line;
	sequence->echoInput = echoInput;
	sequence->start();
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

bool WorldCommandProcessor::findVariable(const QString &name, QString &value, const QString &pluginId) const
{
	if (!pluginId.isEmpty())
	{
		const WorldRuntime::Plugin *plugin = resolveCapturedPlugin(m_runtime, pluginId);
		if (!plugin)
			return false;
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
	return m_runtime && m_runtime->findVariable(name, value);
}

QString WorldCommandProcessor::fixSendText(const QString &source, int sendTo, const QStringList &wildcards,
                                           const QMap<QString, QString> &namedWildcards,
                                           const QString &language, bool makeWildcardsLower,
                                           bool expandVariables, bool expandWildcards, bool fixRegexps,
                                           bool isRegexp, bool throwExceptions, const QString &name,
                                           const QString &pluginId, bool *ok) const
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
				if (findVariable(strVariableName, strVariableContents, pluginId))
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

bool WorldCommandProcessor::regexMatch(const QString &pattern, const QString &subject, const bool ignoreCase,
                                       QStringList &wildcards, QMap<QString, QString> &namedWildcards,
                                       int *startCol, int *endCol, const int startOffset,
                                       const bool multiLine, qint64 *executionTimeNs, int *matchCount,
                                       int                                   *matchAttempts,
                                       const QRegularExpression::MatchOptions matchOptions) const
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

	auto cached = m_regexCache.constFind(cacheKey);
	if (cached == m_regexCache.constEnd())
	{
		QRegularExpression regex(pattern, options);
		// Keep cache bounded; clear all on overflow to avoid unbounded growth
		// from variable-expanded dynamic patterns.
		if (constexpr int kRegexCacheLimit = 4096; m_regexCache.size() >= kRegexCacheLimit)
		{
			m_regexCache.clear();
			m_invalidRegexWarnings.clear();
		}
		m_regexCache.insert(cacheKey, regex);
		cached = m_regexCache.constFind(cacheKey);
	}
	return compiledRegexMatch(cached.value(), cacheKey, subject, wildcards, namedWildcards, startCol, endCol,
	                          startOffset, executionTimeNs, matchCount, matchAttempts, matchOptions);
}

bool WorldCommandProcessor::compiledRegexMatch(const QRegularExpression &regex, const QString &warningKey,
                                               const QString &subject, QStringList &wildcards,
                                               QMap<QString, QString> &namedWildcards, int *startCol,
                                               int *endCol, const int startOffset, qint64 *executionTimeNs,
                                               int *matchCount, int *matchAttempts,
                                               const QRegularExpression::MatchOptions matchOptions) const
{
	if (!regex.isValid())
	{
		if (!m_invalidRegexWarnings.contains(warningKey))
		{
			m_invalidRegexWarnings.insert(warningKey);
			QMessageBox::warning(m_view, QStringLiteral("QMud"),
			                     QStringLiteral("Failed: %1 at offset %2")
			                         .arg(regex.errorString())
			                         .arg(regex.patternErrorOffset()));
		}
		return false;
	}

	const QMudAliasMatch::MatchResult result = QMudAliasMatch::matchWithCaptures(
	    regex, subject, m_regexpMatchEmpty, startOffset, executionTimeNs, matchAttempts, matchOptions);
	if (!result.matched)
		return false;

	if (startCol)
		*startCol = result.startCol;
	if (endCol)
		*endCol = result.endCol;

	wildcards      = result.wildcards;
	namedWildcards = result.namedWildcards;
	if (matchCount)
		*matchCount = result.matchCount;

	return true;
}

WorldCommandProcessor::TriggerEvaluationResult
WorldCommandProcessor::processTriggersForLine(const QString                          &line,
                                              const QVector<WorldRuntime::StyleSpan> &spans)
{
	TriggerEvaluationResult result;
	if (!m_runtime)
		return result;

	m_runtime->beginLuaCallbackSnapshotMutationBatch();
	const auto finishSnapshotBatch = qScopeGuard(
	    [runtime = QPointer<WorldRuntime>(m_runtime)]
	    {
		    if (runtime)
			    runtime->endLuaCallbackSnapshotMutationBatch();
	    });

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
		const QString scopePluginId                    = pluginIdOf(plugin);
		const quint64 scopePluginRuntimeLocationHandle = plugin ? plugin->runtimeLocationHandle : 0;
		const quint64 scopePluginRuntimeInstanceId     = plugin ? plugin->runtimeInstanceId : 0;
		const quint64 scopePluginEngineInstanceId = plugin && plugin->lua ? plugin->lua->instanceId() : 0;
		QVector<LuaCallbackRuleSnapshotChange> changedRuntimeLocations;
		auto                                   recordRuntimeStateChange =
		    [&changedRuntimeLocations](const quint64 locationHandle, const quint64 runtimeId,
		                               const int indexHint, const LuaCallbackRuleSnapshotParts parts)
		{
			if (!changedRuntimeLocations.isEmpty() &&
			    changedRuntimeLocations.constLast().runtimeLocationHandle == locationHandle &&
			    changedRuntimeLocations.constLast().runtimeId == runtimeId)
			{
				changedRuntimeLocations.last().indexHint = indexHint;
				changedRuntimeLocations.last().parts |= parts;
			}
			else
			{
				changedRuntimeLocations.push_back({locationHandle, runtimeId, indexHint, parts});
			}
		};
		auto publishChangedRuntimeState =
		    [runtime = QPointer<WorldRuntime>(m_runtime), &changedRuntimeLocations, scopePluginId]
		{
			if (runtime)
			{
				runtime->markTriggerRuntimeStatesChanged(scopePluginId, changedRuntimeLocations);
				changedRuntimeLocations.clear();
			}
		};
		const auto publishRuntimeState =
		    qScopeGuard([&publishChangedRuntimeState] { publishChangedRuntimeState(); });
		const QSharedPointer<const TriggerEvaluationCacheEntry> cacheEntry =
		    decodedTriggerEvaluationCache(triggers, plugin);
		if (!cacheEntry)
			return;
		const quint64 evaluationGeneration = m_runtime->triggerRuleGeneration();
		for (const DecodedTrigger &decoded : cacheEntry->triggers)
		{
			if (!decoded.enabled)
				continue;
			if (m_runtime->stopTriggerEvaluation() != WorldRuntime::KeepEvaluating)
				break;

			const quint64          triggerRuntimeId             = decoded.runtimeId;
			const quint64          triggerRuntimeLocationHandle = decoded.runtimeLocationHandle;
			WorldRuntime::Trigger *trigger                      = nullptr;
			if (m_runtime->triggerRuleGeneration() == evaluationGeneration && decoded.index >= 0 &&
			    decoded.index < triggers.size() && triggers.at(decoded.index).runtimeId == triggerRuntimeId &&
			    triggers.at(decoded.index).runtimeLocationHandle == triggerRuntimeLocationHandle)
			{
				trigger = &triggers[decoded.index];
			}
			else
			{
				trigger = resolveTriggerByRuntimeId(m_runtime, triggerRuntimeLocationHandle, triggerRuntimeId,
				                                    decoded.index);
			}

			m_runtime->incrementTriggersEvaluated();

			if (decoded.matchText.isEmpty())
				continue;

			const bool     preserveTrailingWhitespace = decoded.isRegexp;
			QString        multiLineTarget;
			const QString *target = preserveTrailingWhitespace ? &matchInputLine : &trimmedTriggerLine;
			if (decoded.multiLine)
			{
				multiLineTarget = buildTarget(decoded.linesToMatch, preserveTrailingWhitespace);
				target          = &multiLineTarget;
			}

			QString        expandedMatchText;
			const QString *effectiveMatchText = &decoded.matchText;
			if (decoded.dynamicPattern)
			{
				expandedMatchText  = fixSendText(decoded.matchText, decoded.sendToValue, QStringList(),
				                                 QMap<QString, QString>(), language, false, true, false, true,
				                                 decoded.isRegexp, false, QString(), scopePluginId, nullptr);
				effectiveMatchText = &expandedMatchText;
			}

			QString dynamicPattern;
			if (decoded.dynamicPattern)
				dynamicPattern =
				    decoded.isRegexp ? *effectiveMatchText : wildcardToRegexCached(*effectiveMatchText);
			QStringList            wildcards;
			QMap<QString, QString> namedWildcards;
			int                    startCol = 0;
			int                    endCol   = 0;
			qint64         executionTimeNs  = trigger ? trigger->executionTimeNs : decoded.executionTimeNs;
			int            matchCount       = trigger ? trigger->matchCount : decoded.matchCount;
			int            matchAttempts    = trigger ? trigger->matchAttempts : decoded.matchAttempts;
			const int      previousMatchAttempts = matchAttempts;
			constexpr auto trustedTriggerSubject = QRegularExpression::DontCheckSubjectStringMatchOption;
			auto           matchPattern          = [&](const int startOffset)
			{
				if (decoded.dynamicPattern)
				{
					return regexMatch(dynamicPattern, *target, decoded.ignoreCase, wildcards, namedWildcards,
					                  &startCol, &endCol, startOffset, decoded.multiLine, &executionTimeNs,
					                  &matchCount, &matchAttempts, trustedTriggerSubject);
				}
				if (decoded.matcherIndex < 0 || decoded.matcherIndex >= cacheEntry->matchers.size())
					return false;
				const CompiledTriggerMatcher &matcher = cacheEntry->matchers.at(decoded.matcherIndex);
				return compiledRegexMatch(matcher.regex, matcher.warningKey, *target, wildcards,
				                          namedWildcards, &startCol, &endCol, startOffset, &executionTimeNs,
				                          &matchCount, &matchAttempts, trustedTriggerSubject);
			};
			if (!matchPattern(0))
			{
				if (trigger && matchAttempts != previousMatchAttempts)
				{
					trigger->executionTimeNs = executionTimeNs;
					trigger->matchCount      = matchCount;
					trigger->matchAttempts   = matchAttempts;
					recordRuntimeStateChange(triggerRuntimeLocationHandle, triggerRuntimeId, decoded.index,
					                         LuaCallbackRuleSnapshotPart::RuntimeState);
				}
				continue;
			}
			if (trigger)
			{
				trigger->executionTimeNs = executionTimeNs;
				trigger->matchCount      = matchCount;
				trigger->matchAttempts   = matchAttempts;
				trigger->lastMatchTarget = *target;
				recordRuntimeStateChange(triggerRuntimeLocationHandle, triggerRuntimeId, decoded.index,
				                         LuaCallbackRuleSnapshotPart::RuntimeState |
				                             LuaCallbackRuleSnapshotPart::DetailState);
			}
			QStringList fixedWildcards = wildcards;
			for (QString &fixed : fixedWildcards)
				fixed = fixWildcard(fixed, decoded.lowerWildcards, decoded.sendToValue, language);
			auto publishCurrentRegexCaptures = [&]
			{
				if (decoded.scriptLabel.isEmpty())
					return;
				if (!scopePluginId.isEmpty())
					m_runtime->setPluginTriggerWildcards(scopePluginId, decoded.scriptLabel, wildcards,
					                                     namedWildcards);
				else
					m_runtime->setTriggerWildcards(decoded.scriptLabel, wildcards, namedWildcards);
			};
			publishCurrentRegexCaptures();
			if (trigger)
			{
				trigger->lastMatchWildcards = fixedWildcards;
				recordRuntimeStateChange(triggerRuntimeLocationHandle, triggerRuntimeId, decoded.index,
				                         LuaCallbackRuleSnapshotPart::DetailState);
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

			if (trigger)
			{
				trigger->matched++;
				trigger->lastMatched = QDateTime::currentDateTime();
			}
			m_runtime->incrementTriggersMatched();
			const bool triggerSoundEnabled =
			    QMudTriggerSound::shouldPlayTriggerSound(!scopePluginId.isEmpty(), worldTriggerSoundsEnabled);
			if (!decoded.sound.isEmpty() &&
			    decoded.sound.compare(QStringLiteral("(No sound)"), Qt::CaseInsensitive) != 0 &&
			    triggerSoundEnabled)
			{
				if (!decoded.soundIfInactive || !m_runtime->isActive())
					m_runtime->playSound(0, decoded.sound, false, 0.0, 0.0);
			}

			if (decoded.oneShot)
			{
				result.oneShotTriggers.push_back(
				    {triggerRuntimeId, triggerRuntimeLocationHandle, scopePluginRuntimeInstanceId,
				     scopePluginRuntimeLocationHandle, -1, decoded.index, scopePluginId});
			}

			if (m_runtime->traceEnabled())
				publishChangedRuntimeState();
			if (decoded.label.isEmpty())
				emitTrace(QStringLiteral("Matched trigger \"%1\"").arg(decoded.matchText));
			else
				emitTrace(QStringLiteral("Matched trigger %1").arg(decoded.label));

			if (decoded.clipboardArg > 0 && decoded.clipboardArg < fixedWildcards.size())
			{
				if (QClipboard *clipboard = QGuiApplication::clipboard())
					clipboard->setText(fixedWildcards.at(decoded.clipboardArg));
			}

			QString sendText = decoded.sendText;
			sendText = fixSendText(fixupEscapeSequences(sendText), decoded.sendToValue, wildcards,
			                       namedWildcards, language, decoded.lowerWildcards, decoded.expandVariables,
			                       true, false, false, false, decoded.scriptLabel, scopePluginId, nullptr);

			if (!decoded.multiLine)
			{
				if (decoded.omitFromLog)
					result.omitFromLog = true;
				if (decoded.omitFromOutput)
					result.omitFromOutput = true;
			}

			if (decoded.sendToValue == eSendToScript || decoded.sendToValue == eSendToScriptAfterOmit)
			{
				DeferredScript deferred;
				deferred.triggerRuntimeId             = triggerRuntimeId;
				deferred.triggerRuntimeLocationHandle = triggerRuntimeLocationHandle;
				deferred.triggerIndexHint             = decoded.index;
				deferred.pluginEngineInstanceId       = scopePluginEngineInstanceId;
				deferred.pluginRuntimeInstanceId      = scopePluginRuntimeInstanceId;
				deferred.pluginRuntimeLocationHandle  = scopePluginRuntimeLocationHandle;
				deferred.pluginId                     = scopePluginId;
				deferred.scriptText                   = sendText;
				deferred.description = QStringLiteral("Trigger: %1").arg(decoded.scriptLabel);
				if (decoded.sendToValue == eSendToScript)
				{
					deferred.trackTriggerExecution = true;
					ensurePaletteReady();
					const QVector<WorldRuntime::StyleSpan> &sourceSpans =
					    workingSpans.isEmpty() ? matchInputSpans : workingSpans;
					deferred.styleRuns = QSharedPointer<QVector<LuaStyleRun>>::create(
					    buildStyleRuns(matchInputLine, sourceSpans, defaultFore, defaultBack));
				}
				deferred.replaceMatchedLineOutput =
				    decoded.sendToValue == eSendToScriptAfterOmit && decoded.omitFromOutput;
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
				publishChangedRuntimeState();
				TriggerExecutionScope executionScope(m_runtime, triggerRuntimeLocationHandle,
				                                     triggerRuntimeId, scopePluginId, false, decoded.index);
				unsigned short        previousActionSource = WorldRuntime::eUnknownActionSource;
				if (m_runtime)
				{
					previousActionSource = m_runtime->currentActionSource();
					m_runtime->setCurrentActionSource(WorldRuntime::eTriggerFired);
				}
				sendTo(decoded.sendToValue, sendText, decoded.omitFromOutput, decoded.omitFromLog,
				       decoded.variableName, QStringLiteral("Trigger: %1").arg(decoded.scriptLabel),
				       scopePluginId);
				if (m_runtime)
					m_runtime->setCurrentActionSource(previousActionSource);
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
					const qint64 executionTimeBeforeRepeats = executionTimeNs;
					const int    attemptsBeforeRepeats      = matchAttempts;
					bool         repeatedMatchSucceeded     = false;
					int          offset                     = endCol;
					while (true)
					{
						if (!matchPattern(offset))
							break;
						repeatedMatchSucceeded = true;
						if (endCol <= offset)
							break;
						applyColour(startCol, endCol);
						offset = endCol;
					}
					const qint64 repeatExecutionTimeNs = executionTimeNs - executionTimeBeforeRepeats;
					const int    repeatMatchAttempts   = matchAttempts - attemptsBeforeRepeats;
					if (repeatExecutionTimeNs != 0 || repeatMatchAttempts != 0 || repeatedMatchSucceeded)
					{
						if (WorldRuntime::Trigger *resolved = resolveTriggerByRuntimeId(
						        m_runtime, triggerRuntimeLocationHandle, triggerRuntimeId, decoded.index))
						{
							resolved->executionTimeNs += repeatExecutionTimeNs;
							resolved->matchAttempts += repeatMatchAttempts;
							if (repeatedMatchSucceeded)
							{
								resolved->matchCount = matchCount;
							}
							recordRuntimeStateChange(triggerRuntimeLocationHandle, triggerRuntimeId,
							                         decoded.index,
							                         LuaCallbackRuleSnapshotPart::RuntimeState);
						}
					}
					if (repeatedMatchSucceeded)
						publishCurrentRegexCaptures();
				}
			}

			if (!decoded.scriptName.isEmpty())
			{
				TriggerScript script;
				script.runtimeId                   = triggerRuntimeId;
				script.runtimeLocationHandle       = triggerRuntimeLocationHandle;
				script.pluginEngineInstanceId      = scopePluginEngineInstanceId;
				script.pluginRuntimeInstanceId     = scopePluginRuntimeInstanceId;
				script.pluginRuntimeLocationHandle = scopePluginRuntimeLocationHandle;
				script.indexHint                   = decoded.index;
				script.pluginId                    = scopePluginId;
				script.label                       = decoded.scriptLabel;
				script.scriptName                  = decoded.scriptName;
				script.line                        = matchInputLine;
				script.wildcards                   = wildcards;
				script.namedWildcards              = namedWildcards;
				script.replaceMatchedLineOutput    = decoded.omitFromOutput;
				result.triggerScripts.push_back(script);
			}

			if (!decoded.keepEvaluating)
				break;
		}
	};

	QList<WorldRuntime::Plugin> &pluginList = WorldCommandProcessorMutationAccess::plugins(*m_runtime);
	struct TriggerPluginPlanEntry
	{
			QString pluginId;
			int     sequence{0};
			quint64 runtimeLocationHandle{0};
			quint64 runtimeInstanceId{0};
			int     indexHint{-1};
	};
	QVector<TriggerPluginPlanEntry> pluginPlan;
	for (const int pluginIndex : sortedPluginIndices())
	{
		if (pluginIndex < 0 || pluginIndex >= pluginList.size())
			continue;
		const WorldRuntime::Plugin &plugin = pluginList.at(pluginIndex);
		if (!pluginHasValidId(plugin))
			continue;
		pluginPlan.push_back({pluginIdOf(&plugin), plugin.sequence, plugin.runtimeLocationHandle,
		                      plugin.runtimeInstanceId, pluginIndex});
	}
	for (const TriggerPluginPlanEntry &plannedPlugin : std::as_const(pluginPlan))
	{
		if (plannedPlugin.sequence >= 0)
			continue;
		WorldRuntime::Plugin *plugin = WorldCommandProcessorMutationAccess::pluginForLocation(
		    *m_runtime, plannedPlugin.runtimeLocationHandle, plannedPlugin.runtimeInstanceId,
		    plannedPlugin.indexHint);
		if (!plugin || !plugin->enabled || plugin->installPending)
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
			processSequence(WorldCommandProcessorMutationAccess::triggers(*m_runtime), nullptr);
		}
	}

	if (m_runtime->stopTriggerEvaluation() != WorldRuntime::StopAllSequences)
	{
		for (const TriggerPluginPlanEntry &plannedPlugin : std::as_const(pluginPlan))
		{
			if (plannedPlugin.sequence < 0)
				continue;
			WorldRuntime::Plugin *plugin = WorldCommandProcessorMutationAccess::pluginForLocation(
			    *m_runtime, plannedPlugin.runtimeLocationHandle, plannedPlugin.runtimeInstanceId,
			    plannedPlugin.indexHint);
			if (!plugin || !plugin->enabled || plugin->installPending)
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
	m_runtime->finishTriggerEvaluation(timer.nsecsElapsed());
	return result;
}

void WorldCommandProcessor::checkTimers()
{
	processQueuedCommands(false);
	if (!m_runtime || !m_runtimeAutomationOwner)
		return;
	if (m_timerEvaluationSequence)
	{
		m_timerEvaluationRerunPending = true;
		return;
	}
	auto sequence             = std::make_shared<TimerEvaluationSequence>();
	sequence->processor       = this;
	sequence->runtime         = m_runtime;
	m_timerEvaluationSequence = sequence;
	sequence->start();
}

void WorldCommandProcessor::sendTo(const int sendTo, const QString &text, const bool omitFromOutput,
                                   const bool omitFromLog, const QString &variableName,
                                   const QString &description, const QString &pluginId,
                                   const QVector<LuaStyleRun> *styleRuns, const bool hasTriggerContext,
                                   const bool             replaceMatchedLineOutput,
                                   const int              triggerMatchedLineBufferIndex,
                                   const qint64           triggerMatchedLineAbsoluteNumber,
                                   const LuaCallbackEvent callbackEvent)
{
	const bool outboundTarget = sendTo == eSendToWorld || sendTo == eSendToCommandQueue ||
	                            sendTo == eSendToSpeedwalk || sendTo == eSendImmediate;
	if (outboundTarget && rejectInvalidOutboundRequest())
		return;
	const bool startsCommandOperation =
	    sendTo == eSendToWorld || sendTo == eSendImmediate || sendTo == eSendToExecute;
	if (startsCommandOperation && !hasCurrentInputCriticalOperation())
	{
		const QSharedPointer<QVector<LuaStyleRun>> retainedStyleRuns =
		    styleRuns ? QSharedPointer<QVector<LuaStyleRun>>::create(*styleRuns) : nullptr;
		enqueueOrRunInputCriticalAction(
		    [processor = QPointer<WorldCommandProcessor>(this), sendTo, text, omitFromOutput, omitFromLog,
		     variableName, description, pluginId, retainedStyleRuns, hasTriggerContext,
		     replaceMatchedLineOutput, triggerMatchedLineBufferIndex, triggerMatchedLineAbsoluteNumber,
		     callbackEvent](CommandCompletion completion)
		    {
			    if (!processor)
			    {
				    if (completion)
					    completion(false);
				    return;
			    }
			    processor->runOrderedRuleAction(
			        [processor, sendTo, text, omitFromOutput, omitFromLog, variableName, description,
			         pluginId, retainedStyleRuns, hasTriggerContext, replaceMatchedLineOutput,
			         triggerMatchedLineBufferIndex, triggerMatchedLineAbsoluteNumber, callbackEvent]
			        {
				        processor->sendTo(sendTo, text, omitFromOutput, omitFromLog, variableName,
				                          description, pluginId, retainedStyleRuns.data(), hasTriggerContext,
				                          replaceMatchedLineOutput, triggerMatchedLineBufferIndex,
				                          triggerMatchedLineAbsoluteNumber, callbackEvent);
			        },
			        std::move(completion));
		    });
		return;
	}

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

	switch (sendTo)
	{
	case eSendToWorld:
		if (triggerCommandPriority)
		{
			sendRawTextWithTriggerPriority(QMudCommandText::normalizeActionCommandTextNewlines(text),
			                               echoSend, false, logIt, false);
		}
		else
		{
			sendMsg(QMudCommandText::normalizeActionCommandTextNewlines(text), echoSend, false, logIt);
		}
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
		doSendMsg(text, echoSend, logIt, OutboundTransport::Immediate);
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
			if (triggerCommandPriority)
			{
				for (const QString &command : commands)
					static_cast<void>(executeCommandWithTriggerPriority(command));
			}
			else
				evaluateCommandSequence(commands, {});
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
		dispatchScriptSend(pluginId, text, description,
		                   hasTriggerContext ? LuaCallbackEvent::TriggerScript : callbackEvent, styleRuns,
		                   hasTriggerContext, replaceMatchedLineOutput, triggerMatchedLineBufferIndex,
		                   triggerMatchedLineAbsoluteNumber);
		break;
	default:
		break;
	}
}

void WorldCommandProcessor::dispatchScriptSend(
    const QString &pluginId, const QString &text, const QString &description,
    const LuaCallbackEvent callbackEvent, const QVector<LuaStyleRun> *styleRuns, const bool hasTriggerContext,
    const bool replaceMatchedLineOutput, const int triggerMatchedLineBufferIndex,
    const qint64 triggerMatchedLineAbsoluteNumber)
{
	if (!m_runtime)
		return;
	emit sendToScriptRequested(pluginId, text, description, callbackEvent, styleRuns, hasTriggerContext,
	                           replaceMatchedLineOutput, triggerMatchedLineBufferIndex,
	                           triggerMatchedLineAbsoluteNumber);

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
		if (!m_runtime->luaScriptingAvailable())
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
		lua    = m_runtime->luaCallbacks();
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
		    luaRef, text, description, callbackEvent, styleRuns, hasTriggerContext, replaceMatchedLineOutput,
		    triggerMatchedLineBufferIndex, triggerMatchedLineAbsoluteNumber));
		return;
	}

	LogicalOperationCompletionPtr operationCompletion = m_activeOperationCompletion;
	std::function<void(bool)>     scriptCompletion;
	if (operationCompletion)
	{
		operationCompletion->retain();
		CallerContextRunner completionContext = captureCallerContextRunner();
		scriptCompletion = [operationCompletion = std::move(operationCompletion),
		                    completionContext = std::move(completionContext)](const bool /*scriptSucceeded*/)
		{
			const auto release = [operationCompletion] { operationCompletion->release(true); };
			if (completionContext)
				completionContext.run(release);
			else
				release();
		};
	}
	executionRuntime->dispatchLuaExecuteScriptAsync(
	    luaRef, text, description, callbackEvent, styleRuns, hasTriggerContext, replaceMatchedLineOutput,
	    triggerMatchedLineBufferIndex, triggerMatchedLineAbsoluteNumber, std::move(scriptCompletion));
}

void WorldCommandProcessor::sendMsg(const QString &text, const bool echo, const bool queueIt, bool logIt)
{
	if (rejectInvalidOutboundRequest())
		return;
	if (m_pendingTriggerPriorityCommands > 0 && m_triggerCommandPriorityDepth > 0 &&
	    m_triggerPriorityActionExecutionDepth == 0)
	{
		PendingTriggerPriorityAction action;
		action.kind    = PendingTriggerPriorityAction::Kind::Send;
		action.text    = text;
		action.echo    = echo;
		action.queueIt = queueIt;
		action.logIt   = logIt;
		enqueuePendingTriggerPriorityAction(std::move(action));
		return;
	}
	if (!ensureConnectedForSend())
	{
		if (m_activeCommandHistoryState)
			m_activeCommandHistoryState->sendFailed = true;
		return;
	}

	QString strText = text;

	// cannot change what we are sending in OnPluginSent
	if (m_pluginProcessingSent)
		return;
	const qsizetype queuedCommandCountBefore = m_queuedCommands.size();

	bool            bEcho = echo;
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
			const unsigned short actionSource =
			    m_runtime ? m_runtime->currentActionSource()
			              : static_cast<unsigned short>(WorldRuntime::eUnknownActionSource);
			const QString encoded =
			    QMudCommandQueue::encodeQueueEntry(strLine, queueIt, bEcho, logIt, actionSource);
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
			const bool              triggerPriority = !queueIt && m_triggerCommandPriorityDepth > 0;
			const OutboundTransport transport = triggerPriority
			                                        ? OutboundTransport::CausalDeferredWithTriggerPriority
			                                        : OutboundTransport::CausalDeferred;
			doSendMsg(strLine, bEcho, logIt, transport);
		}
	} // end of breaking it into lines

	if (!m_queuedCommands.isEmpty())
		updateQueuedCommandsStatusLine();
	if (m_runtime)
		m_runtime->setQueuedCommandCount(safeQSizeToInt(m_queuedCommands.size()));
	if (m_queuedCommands.size() != queuedCommandCountBefore)
		emit queuedCommandsChanged();
}

void WorldCommandProcessor::doSendMsg(const QString &text, const bool echo, const bool logIt,
                                      const OutboundTransport       transport,
                                      std::function<void(bool)>     mutationCompletion,
                                      LogicalOperationCompletionPtr operationCompletion,
                                      const bool                    bypassSpamPrevention)
{
	if (!operationCompletion && m_activeOperationCompletion)
	{
		operationCompletion = m_activeOperationCompletion;
		operationCompletion->retain();
	}
	if (!mutationCompletion && m_runtime)
		mutationCompletion = m_runtime->retainLuaDeferredRuntimeMutationPlayback();
	if (rejectInvalidOutboundContext(mutationCompletion, operationCompletion, true))
		return;
	if (m_pluginSentNotificationPending)
	{
		PendingOutboundSend pending;
		pending.text                 = text;
		pending.echo                 = echo;
		pending.logIt                = logIt;
		pending.transport            = transport;
		pending.callerContext        = captureRuntimeCallerContextRunner();
		pending.mutationCompletion   = std::move(mutationCompletion);
		pending.operationCompletion  = std::move(operationCompletion);
		pending.bypassSpamPrevention = bypassSpamPrevention;
		m_pendingPluginSends.enqueue(std::move(pending));
		return;
	}
	if (m_pluginProcessingSend)
	{
		const bool replayingPluginSendMutations =
		    m_runtime && !m_runtime->m_pluginCallbackMutationPlaybackContexts.isEmpty() &&
		    m_runtime->m_pluginCallbackMutationPlaybackContexts.constLast().event == LuaCallbackEvent::Send;
		if (!replayingPluginSendMutations)
		{
			PendingOutboundSend pending;
			pending.text                 = text;
			pending.echo                 = echo;
			pending.logIt                = logIt;
			pending.transport            = transport;
			pending.callerContext        = captureRuntimeCallerContextRunner();
			pending.mutationCompletion   = std::move(mutationCompletion);
			pending.operationCompletion  = std::move(operationCompletion);
			pending.bypassSpamPrevention = bypassSpamPrevention;
			m_pendingPluginSends.enqueue(std::move(pending));
			return;
		}
	}

	if (!ensureConnectedForSend())
	{
		if (m_activeCommandHistoryState)
			m_activeCommandHistoryState->sendFailed = true;
		if (mutationCompletion)
			mutationCompletion(false);
		if (operationCompletion)
			operationCompletion->release(true);
		return;
	}

	QString str = text;

	// cannot change what we are sending in OnPluginSent
	if (m_pluginProcessingSent)
	{
		if (mutationCompletion)
			mutationCompletion(false);
		if (operationCompletion)
			operationCompletion->release(true);
		return;
	}

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
		if (m_runtime)
		{
			auto continuationFactory = makeLuaCallbackResultContinuationFactory(
			    [processor = QPointer<WorldCommandProcessor>(this), str, echo, logIt, transport,
			     &mutationCompletion, &operationCompletion, bypassSpamPrevention]() mutable
			    {
				    PendingPluginSendDecision state;
				    state.processor            = processor;
				    state.runtime              = processor ? processor->m_runtime : nullptr;
				    state.text                 = str;
				    state.echo                 = echo;
				    state.logIt                = logIt;
				    state.transport            = transport;
				    state.mutationCompletion   = std::move(mutationCompletion);
				    state.operationCompletion  = std::move(operationCompletion);
				    state.bypassSpamPrevention = bypassSpamPrevention;
				    return state;
			    });
			LuaCallbackContinuationDispatchResult callback =
			    m_runtime->firePluginSendContinuable(pluginText, continuationFactory);
			if (callback.suspended())
			{
				m_pluginSendDecisionPending = true;
				m_pluginSendDecisionTicket  = callback.ticket;
				return;
			}
			if (callback.result.boolResultValid && !callback.result.boolResult)
			{
				m_pluginProcessingSend = false;
				if (mutationCompletion)
					mutationCompletion(false);
				if (operationCompletion)
					operationCompletion->release(true);
				return;
			}
		}
		m_pluginProcessingSend = false;
	}

	continueDoSendMsgAfterPluginSend(str, echo, logIt, transport, std::move(mutationCompletion),
	                                 std::move(operationCompletion), bypassSpamPrevention);
}

void WorldCommandProcessor::completePluginSendDecision(const QString &text, const bool echo, const bool logIt,
                                                       const OutboundTransport transport, const bool accepted,
                                                       std::function<void(bool)>     mutationCompletion,
                                                       LogicalOperationCompletionPtr operationCompletion,
                                                       const bool                    bypassSpamPrevention)
{
	Q_ASSERT(m_pluginSendDecisionPending);
	m_pluginSendDecisionTicket  = 0;
	m_pluginSendDecisionPending = false;
	m_pluginProcessingSend      = false;
	if (accepted)
	{
		continueDoSendMsgAfterPluginSend(text, echo, logIt, transport, std::move(mutationCompletion),
		                                 std::move(operationCompletion), bypassSpamPrevention);
		return;
	}
	if (mutationCompletion)
		mutationCompletion(false);
	drainPendingPluginSends();
	if (operationCompletion)
		operationCompletion->release(true);
}

void WorldCommandProcessor::cancelPluginSendDecision(const std::function<void(bool)>     &mutationCompletion,
                                                     const LogicalOperationCompletionPtr &operationCompletion)
{
	Q_ASSERT(m_pluginSendDecisionPending);
	Q_ASSERT(m_pluginProcessingSend);
	if (mutationCompletion)
		mutationCompletion(true);
	if (operationCompletion)
		operationCompletion->release(false);
	m_pluginSendDecisionTicket  = 0;
	m_pluginSendDecisionPending = false;
	m_pluginProcessingSend      = false;
	if (!m_runtime || !m_runtime->m_runtimeDestructionStarted)
		drainPendingPluginSends();
}

void WorldCommandProcessor::cancelPendingPluginSendsForShutdown()
{
	Q_ASSERT(!m_runtime || m_runtime->m_runtimeDestructionStarted);
	cancelPendingPluginSends();
}

void WorldCommandProcessor::cancelOutboundWorkForRuntimeDetach(WorldRuntime *const runtime)
{
	if (!runtime)
	{
		cancelPendingPluginSends();
		return;
	}

	if (const quint64 ticket = std::exchange(m_pluginSendDecisionTicket, quint64{0}); ticket != 0)
		static_cast<void>(runtime->cancelPluginCallbackResultContinuation(ticket));
	if (const quint64 ticket = std::exchange(m_pluginSentNotificationTicket, quint64{0}); ticket != 0)
		static_cast<void>(runtime->cancelPluginCallbackResultContinuation(ticket));

	if (m_activeOutboundTransaction &&
	    m_activeOutboundTransaction->outcome == OutboundCommandTransaction::Outcome::PendingWrite)
	{
		m_activeOutboundTransaction->resolve(OutboundCommandTransaction::Outcome::Rejected);
	}
	cancelPendingPluginSends();
}

void WorldCommandProcessor::cancelPendingPluginSends()
{
	const bool wasDraining       = m_drainingPendingPluginSends;
	m_drainingPendingPluginSends = true;
	while (!m_pendingPluginSends.isEmpty())
	{
		PendingOutboundSend pending       = m_pendingPluginSends.dequeue();
		auto                callerContext = pending.callerContext;
		auto                cancel        = [pending = std::move(pending)]() mutable
		{
			if (pending.mutationCompletion)
				pending.mutationCompletion(true);
			if (pending.operationCompletion)
				pending.operationCompletion->release(false);
		};
		if (callerContext)
			callerContext.run(cancel);
		else
			cancel();
	}
	m_drainingPendingPluginSends = wasDraining;
}

void WorldCommandProcessor::drainPendingPluginSends()
{
	if (m_runtime && m_runtime->m_runtimeDestructionStarted)
		return;
	if (m_outboundBindingCancellationActive || m_drainingPendingPluginSends || m_pluginProcessingSend ||
	    m_pluginSentNotificationPending)
		return;
	m_drainingPendingPluginSends = true;
	const auto finishDraining    = qScopeGuard([this] { m_drainingPendingPluginSends = false; });
	Q_UNUSED(finishDraining);
	while (!m_pluginProcessingSend && !m_pluginSentNotificationPending && !m_pendingPluginSends.isEmpty())
	{
		PendingOutboundSend pending = m_pendingPluginSends.dequeue();
		auto                send    = [this, &pending]() mutable
		{
			if (pending.phase == PendingOutboundSend::Phase::AfterPluginSend)
			{
				continueDoSendMsgAfterPluginSend(pending.text, pending.echo, pending.logIt, pending.transport,
				                                 std::move(pending.mutationCompletion),
				                                 std::move(pending.operationCompletion),
				                                 pending.bypassSpamPrevention);
				return;
			}
			doSendMsg(pending.text, pending.echo, pending.logIt, pending.transport,
			          std::move(pending.mutationCompletion), std::move(pending.operationCompletion),
			          pending.bypassSpamPrevention);
		};
		if (pending.callerContext)
			pending.callerContext.run(send);
		else
			send();
	}
}

void WorldCommandProcessor::continueDoSendMsgAfterPluginSend(
    const QString &str, const bool echo, const bool logIt, const OutboundTransport transport,
    std::function<void(bool)> mutationCompletion, LogicalOperationCompletionPtr operationCompletion,
    const bool bypassSpamPrevention)
{
	if (rejectInvalidOutboundContext(mutationCompletion, operationCompletion, true))
		return;
	if (m_pluginSentNotificationPending)
	{
		PendingOutboundSend pending;
		pending.phase                = PendingOutboundSend::Phase::AfterPluginSend;
		pending.text                 = str;
		pending.echo                 = echo;
		pending.logIt                = logIt;
		pending.transport            = transport;
		pending.callerContext        = captureRuntimeCallerContextRunner();
		pending.mutationCompletion   = std::move(mutationCompletion);
		pending.operationCompletion  = std::move(operationCompletion);
		pending.bypassSpamPrevention = bypassSpamPrevention;
		m_pendingPluginSends.enqueue(std::move(pending));
		return;
	}

	SpamCommitState acceptedSpamState{str, str == m_lastCommandSent ? m_lastCommandCount + 1 : 1};

	if (m_enableSpamPrevention && // provided they want it
	    m_spamLineCount > 2 &&    // otherwise we might loop
	    !m_spamMessage.isEmpty()) // not much point without something to send
	{
		if (!bypassSpamPrevention && acceptedSpamState.count > m_spamLineCount)
		{
			PendingOutboundSend resumeOriginal;
			resumeOriginal.phase                = PendingOutboundSend::Phase::AfterPluginSend;
			resumeOriginal.text                 = str;
			resumeOriginal.echo                 = echo;
			resumeOriginal.logIt                = logIt;
			resumeOriginal.transport            = transport;
			resumeOriginal.callerContext        = captureRuntimeCallerContextRunner();
			resumeOriginal.mutationCompletion   = std::move(mutationCompletion);
			resumeOriginal.operationCompletion  = std::move(operationCompletion);
			resumeOriginal.bypassSpamPrevention = true;
			m_pendingPluginSends.prepend(std::move(resumeOriginal));

			doSendMsg(m_spamMessage, echo, logIt, transport, {}, {}, true);
			if (!m_pluginProcessingSend && !m_pluginSentNotificationPending)
				drainPendingPluginSends();
			return;
		} // end of time to do it

	} // end of spam prevention active

	// "OnPluginSent" - we are definitely sending this
	// See: http://www.gammon.com.au/forum/bbshowpost.php?bbsubject_id=7244

	startPluginSentNotification(str, echo, logIt, transport, std::move(mutationCompletion),
	                            std::move(operationCompletion), std::move(acceptedSpamState));
}

void WorldCommandProcessor::startPluginSentNotification(const QString &str, const bool echo, const bool logIt,
                                                        const OutboundTransport       transport,
                                                        std::function<void(bool)>     mutationCompletion,
                                                        LogicalOperationCompletionPtr operationCompletion,
                                                        SpamCommitState               spamCommitState)
{
	if (rejectInvalidOutboundContext(mutationCompletion, operationCompletion, true))
		return;
	Q_ASSERT(!m_pluginSentNotificationPending);
	QString pluginText = str;
	if (pluginText.endsWith(kEndLine))
		pluginText.chop(kEndLine.size());
	if (!m_runtime)
	{
		finishRejectedOutboundSend(mutationCompletion, operationCompletion, true);
		return;
	}
	auto command                 = std::make_shared<PendingOutboundCommand>();
	command->processor           = this;
	command->runtime             = m_runtime;
	command->text                = str;
	command->echo                = echo;
	command->logIt               = logIt;
	command->sentRequest         = m_runtime->preparePluginSentDispatch(pluginText);
	command->mutationCompletion  = std::move(mutationCompletion);
	command->operationCompletion = std::move(operationCompletion);
	command->historyState        = m_activeCommandHistoryState;
	command->spamCommitState     = std::move(spamCommitState);

	const QSharedPointer<OutboundCommandTransaction> transaction =
	    QSharedPointer<OutboundCommandTransaction>::create();
	transaction->callerContext = m_runtime->captureOutboundCallerContext();
	const QWeakPointer<OutboundCommandTransaction> weakTransaction = transaction.toWeakRef();
	transaction->accepted = [processor = QPointer<WorldCommandProcessor>(this), command, weakTransaction]
	{
		if (const QSharedPointer<OutboundCommandTransaction> resolved = weakTransaction.toStrongRef())
		{
			if (processor)
				processor->beginAcceptedPluginSentNotification(command, resolved);
			else
				command->settleWithoutProcessor(resolved);
		}
	};
	transaction->rejected = [processor = QPointer<WorldCommandProcessor>(this), command, weakTransaction]
	{
		if (processor)
		{
			if (const QSharedPointer<OutboundCommandTransaction> resolved = weakTransaction.toStrongRef())
				processor->rejectOutboundCommand(command, resolved);
		}
		else if (const QSharedPointer<OutboundCommandTransaction> resolved = weakTransaction.toStrongRef())
			command->settleWithoutProcessor(resolved);
	};

	QByteArray payload = m_utf8 ? str.toUtf8() : qmudEncodeWorldText(str, m_legacyEncodingName, nullptr);
	if (!m_doNotTranslateIac)
		payload.replace(static_cast<char>(0xFF), QByteArray("\xFF\xFF", 2));
	m_pluginSentNotificationPending = true;
	m_activeOutboundCommand         = command;
	m_activeOutboundTransaction     = transaction;
	const bool causalDeferred       = transport != OutboundTransport::Immediate;
	const bool triggerPriority      = transport == OutboundTransport::CausalDeferredWithTriggerPriority;
	const WorldRuntime::CausalOutboundStageResult stageResult =
	    causalDeferred ? m_runtime->stageCausalOutboundTransmission(payload, triggerPriority, transaction)
	                   : WorldRuntime::CausalOutboundStageResult::Immediate;
	if (stageResult == WorldRuntime::CausalOutboundStageResult::Cancelled)
	{
		transaction->resolve(OutboundCommandTransaction::Outcome::Rejected);
		return;
	}
	if (stageResult == WorldRuntime::CausalOutboundStageResult::Immediate)
		m_runtime->sendToWorld(payload, transaction);
}

void WorldCommandProcessor::beginAcceptedPluginSentNotification(
    const std::shared_ptr<PendingOutboundCommand>    &command,
    const QSharedPointer<OutboundCommandTransaction> &transaction)
{
	if (!command || !command->runtime || m_runtime != command->runtime ||
	    transaction->outcome != OutboundCommandTransaction::Outcome::Accepted)
	{
		rejectOutboundCommand(command, transaction);
		return;
	}
	transaction->accepted                   = {};
	command->sentRequest.sentObservationArg = transaction->preWriteObservation;
	m_lastCommandSent                       = command->spamCommitState.command;
	m_lastCommandCount                      = command->spamCommitState.count;
	m_pluginProcessingSent                  = true;
	auto continuationFactory                = makeLuaCallbackResultContinuationFactory(
	    [command, transaction]
	    {
		    PendingPluginSentNotification state;
		    state.command     = command;
		    state.transaction = transaction;
		    return state;
	    });
	LuaCallbackContinuationDispatchResult callback =
	    m_runtime->firePluginSentContinuable(command->sentRequest, continuationFactory);
	m_pluginProcessingSent = false;
	if (callback.suspended())
	{
		m_pluginSentNotificationTicket = callback.ticket;
		return;
	}
	completePluginSentNotification(command, transaction);
}

void WorldCommandProcessor::completePluginSentNotification(
    const std::shared_ptr<PendingOutboundCommand>    &command,
    const QSharedPointer<OutboundCommandTransaction> &transaction,
    const AcceptedOutboundCompletionMode              completionMode)
{
	if (!command || !command->runtime || m_runtime != command->runtime ||
	    transaction->outcome != OutboundCommandTransaction::Outcome::Accepted)
	{
		rejectOutboundCommand(command, transaction);
		return;
	}
	if (command->settled)
		return;
	command->settled                = true;
	m_pluginSentNotificationTicket  = 0;
	m_pluginSentNotificationPending = false;
	m_pluginProcessingSent          = false;
	clearActiveOutboundCommand(command, transaction);
	const QPointer<WorldRuntime> runtime(command->runtime);
	const QString               &str   = command->text;
	const bool                   echo  = command->echo;
	const bool                   logIt = command->logIt;
	const bool                   cancelOriginatingWork =
	    completionMode == AcceptedOutboundCompletionMode::CancelOriginatingWork;

	// echo sent text if required

	if (echo && m_view)
	{
		m_view->echoInputText(str);
		if (runtime)
		{
			QString screenText = str;
			if (screenText.endsWith(kEndLine))
				screenText.chop(kEndLine.size());
			runtime->firePluginScreendraw(2, logIt ? 1 : 0, screenText);
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
	if (runtime && runtime->isMapping())
	{
		QString direction = str.trimmed().toLower();
		if (direction.endsWith(kEndLine))
			direction.chop(kEndLine.size());
		direction = direction.trimmed();

		const AppController *app = AppController::instance();
		if (const QString mapped = app ? app->mapDirectionToLog(direction) : QString(); !mapped.isEmpty())
		{
			const QString reverse = app ? app->mapDirectionReverse(direction) : QString();

			auto          appendMapped = [&] { runtime->addToMapper(mapped, reverse); };

			if (runtime->removeMapReverses() && runtime->mappingCount() > 0)
			{
				QString heldComment;
				QString last = runtime->mappingItem(runtime->mappingCount() - 1);
				if (last.size() >= 2 && last.startsWith(QLatin1Char('{')) &&
				    last.endsWith(QLatin1Char('}')) && runtime->mappingCount() > 1)
				{
					heldComment = last.mid(1, last.size() - 2);
					runtime->deleteLastMapItem();
					last = runtime->mappingItem(runtime->mappingCount() - 1);
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
							runtime->deleteLastMapItem();
							collapsed = true;
						}
					}
				}
				else if (last.trimmed().toLower() == reverse)
				{
					runtime->deleteLastMapItem();
					collapsed = true;
				}

				if (!collapsed)
				{
					if (!heldComment.isEmpty())
						runtime->addMapperComment(heldComment);
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

	// The transport was committed before OnPluginSent. Complete the observable host accounting and the
	// command's causal mutation batch here. Packet-debug publication continues independently afterward.
	if (runtime)
	{
		runtime->incrementLinesSent();
		QString commandText = str;
		if (commandText.endsWith(kEndLine))
			commandText.chop(kEndLine.size());
		runtime->setLastCommandSent(commandText);
		const QPointer<WorldCommandProcessor> processor(this);
		if (auto mutationCompletion = std::move(command->mutationCompletion); mutationCompletion)
			mutationCompletion(cancelOriginatingWork);
		if (auto operationCompletion = std::move(command->operationCompletion); operationCompletion)
			operationCompletion->release(!cancelOriginatingWork);
		if (!runtime)
			return;
		if (processor && processor->m_runtime == runtime.data())
			processor->drainPendingPluginSends();
		runtime->finishOutboundCommandNotification(transaction);
		return;
	}
	if (auto completion = std::move(command->mutationCompletion); completion)
		completion(cancelOriginatingWork);
	drainPendingPluginSends();
	if (auto completion = std::move(command->operationCompletion); completion)
		completion->release(!cancelOriginatingWork);
}

void WorldCommandProcessor::rejectOutboundCommand(
    const std::shared_ptr<PendingOutboundCommand>    &command,
    const QSharedPointer<OutboundCommandTransaction> &transaction)
{
	if (!command || command->settled)
		return;
	command->settled             = true;
	const bool ownsProcessorSlot = !m_runtime || m_runtime == command->runtime;
	if (ownsProcessorSlot)
	{
		m_pluginSentNotificationPending = false;
		m_pluginProcessingSent          = false;
	}
	m_pluginSentNotificationTicket = 0;
	clearActiveOutboundCommand(command, transaction);
	if (transaction)
	{
		transaction->accepted = {};
		transaction->rejected = {};
	}
	if (command->historyState)
		command->historyState->sendFailed = true;
	WorldRuntime *const runtime = command->runtime;
	if (runtime)
		runtime->discardOutboundCommandNotification(transaction);
	if (auto completion = std::move(command->mutationCompletion); completion)
		completion(true);
	if (ownsProcessorSlot && m_runtime == runtime && (!runtime || !runtime->m_runtimeDestructionStarted))
		drainPendingPluginSends();
	if (auto completion = std::move(command->operationCompletion); completion)
		completion->release(false);
}

void WorldCommandProcessor::clearActiveOutboundCommand(
    const std::shared_ptr<PendingOutboundCommand>    &command,
    const QSharedPointer<OutboundCommandTransaction> &transaction)
{
	if (m_activeOutboundCommand != command || m_activeOutboundTransaction != transaction)
		return;
	m_activeOutboundCommand.reset();
	m_activeOutboundTransaction.clear();
}

void WorldCommandProcessor::finishRejectedOutboundSend(std::function<void(bool)>     &mutationCompletion,
                                                       LogicalOperationCompletionPtr &operationCompletion,
                                                       const bool                     drainPendingSends)
{
	if (m_activeCommandHistoryState)
		m_activeCommandHistoryState->sendFailed = true;
	auto rejectedMutation  = std::move(mutationCompletion);
	auto rejectedOperation = std::move(operationCompletion);
	if (rejectedMutation)
		rejectedMutation(false);
	if (drainPendingSends)
		drainPendingPluginSends();
	if (rejectedOperation)
		rejectedOperation->release(false);
}

bool WorldCommandProcessor::rejectInvalidOutboundContext(std::function<void(bool)>     &mutationCompletion,
                                                         LogicalOperationCompletionPtr &operationCompletion,
                                                         const bool                     drainPendingSends)
{
	if (!m_outboundBindingCancellationActive && (!m_runtime || !m_runtime->currentOutboundContextRejected()))
		return false;
	finishRejectedOutboundSend(mutationCompletion, operationCompletion, drainPendingSends);
	return true;
}

bool WorldCommandProcessor::rejectInvalidOutboundRequest() const
{
	if (!m_outboundBindingCancellationActive && (!m_runtime || !m_runtime->currentOutboundContextRejected()))
		return false;
	if (m_activeCommandHistoryState)
		m_activeCommandHistoryState->sendFailed = true;
	if (m_activeOperationCompletion)
		m_activeOperationCompletion->succeeded = false;
	return true;
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

	const bool interactiveSource = WorldRuntime::isInteractiveActionSource(m_runtime->currentActionSource());

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
	if (m_pendingTriggerPriorityCommands > 0)
		return;

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
	if (removedFromHead > 0)
		emit queuedCommandsChanged();
	m_triggerPriorityQueuedCommands = qMax(0, m_triggerPriorityQueuedCommands - removedFromHead);
	for (const QString &queued : batch)
	{
		const QMudCommandQueue::QueueEntry decoded = QMudCommandQueue::decodeQueueEntry(queued);
		const QPointer<WorldRuntime>       runtimeGuard(m_runtime);
		const unsigned short               previousActionSource =
		    runtimeGuard ? runtimeGuard->currentActionSource()
		                 : static_cast<unsigned short>(WorldRuntime::eUnknownActionSource);
		if (runtimeGuard)
			runtimeGuard->setCurrentActionSource(decoded.actionSource);
		[[maybe_unused]] const auto restoreActionSource = qScopeGuard(
		    [runtimeGuard, previousActionSource]
		    {
			    if (runtimeGuard)
				    runtimeGuard->setCurrentActionSource(previousActionSource);
		    });
		sendImmediateText(decoded.payload, decoded.withEcho, decoded.logIt, false);
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

		const QString direction = QMudCommandQueue::decodeQueueEntry(item).payload.trimmed();
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
