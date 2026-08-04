/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldRuntime.cpp
 * Role: Primary world runtime engine implementation handling socket traffic, telnet/MXP processing, rendering,
 * automation, and Lua callbacks.
 */

#include "WorldRuntime.h"

#include "AnsiSgrParseUtils.h"
#include "AppController.h"
#include "Blending.h"
#include "ColorUtils.h"
#include "CommandMappingTypes.h"
#include "DoubleMetaphone.h"
#include "EncodingUtils.h"
#include "ErrorDescriptions.h"
#include "GuiSystemUtils.h"
#include "HyperlinkActionUtils.h"
#include "LogCompressionUtils.h"
#include "LuaCallbackEngine.h"
#include "LuaExecutor.h"
#include "LuaHeaders.h"
#include "MainFrame.h"
#include "MainFrameActionUtils.h"
#include "MainFrameMdiUtils.h"
#include "MainWindowHostResolver.h"
#include "MiniWindowUtils.h"
#include "MxpDiagnostics.h"
#include "NameGeneration.h"
#include "NativePluginRegistry.h"
#include "PluginBroadcastSelectionUtils.h"
#include "PluginCallbackCatalogUtils.h"
#include "PluginCallbackDispatchUtils.h"
#include "ReloadUtils.h"
#include "SqliteCompat.h"
#include "StartTlsFallbackUtils.h"
#include "StringUtils.h"
#include "TimeFormatUtils.h"
#include "Version.h"
#include "WorldChildWindow.h"
#include "WorldCommandProcessor.h"
#include "WorldDocument.h"
#include "WorldOptionDefaults.h"
#include "WorldOptions.h"
#include "WorldRuntimeAttributeUtils.h"
#include "WorldSocket.h"
#include "WorldView.h"
#include "helpers/LuaExecutionUtils.h"
#include "helpers/LuaModalDialogUtils.h"
#include "helpers/NoteColourUtils.h"
#include "helpers/OutputWrapUtils.h"
#include "helpers/PluginPathUtils.h"
#include "helpers/SoundFileRoutingUtils.h"
#include "scripting/ScriptingErrors.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHash>
#include <QHostInfo>
#include <QImageReader>
#include <QLibrary>
#include <QLocale>
#include <QMessageBox>
#include <QMetaType>
#include <QNetworkInterface>
#include <QPainter>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStringConverter>
// ReSharper disable once CppUnusedIncludeDirective
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextStream>
#include <QThread>
#include <QThreadPool>
#include <QTimeZone>
#include <QTimer>
#include <QUdpSocket>
#include <QtSql/QSqlDriver>
#include <QtSql/QSqlError>
#include <QtSql/QSqlRecord>
#if QMUD_ENABLE_SOUND
#include <QAudioOutput>
#include <QMediaPlayer>
#include <QSoundEffect>
#include <QTemporaryFile>
#endif
#include <QPointer>
#include <QUrl>
#include <QtMath>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <zlib.h>

Q_DECLARE_OPAQUE_POINTER(sqlite3 *)

struct ResolvedWorldColourTables
{
		QVector<QColor> normalAnsi;
		QVector<QColor> boldAnsi;
		QVector<QColor> customText;
		QVector<QColor> customBack;
};

struct ResolvedNoteColours
{
		long fore{0};
		long back{0};
};

static long    colorToLong(const QColor &color);
static int     colourSeqFromAttributes(const QMap<QString, QString> &attributes);
static QString convertToRegularExpression(const QString &text);
static void    buildCustomColours(const QList<WorldRuntime::Colour> &colours, QVector<QColor> &normalAnsi,
                                  QVector<QColor> &customText, QVector<QColor> &customBack);
static void    buildResolvedColourTables(const QList<WorldRuntime::Colour> &colours,
                                         ResolvedWorldColourTables         &tables);
static ResolvedWorldColourTables buildResolvedWorldColourTables(const QList<WorldRuntime::Colour> &colours);
static bool                      worldAttributeAffectsResolvedOutputColours(const QString &key);
static ResolvedNoteColours       resolveNoteColours(bool notesInRgb, int noteTextColour, long noteColourFore,
                                                    long                             noteColourBack,
                                                    const ResolvedWorldColourTables &colourTables,
                                                    const QMap<QString, QString>    &worldAttributes);
static ResolvedNoteColours resolveNoteColours(bool notesInRgb, int noteTextColour, long noteColourFore,
                                              long noteColourBack, const QList<WorldRuntime::Colour> &colours,
                                              const QMap<QString, QString> &worldAttributes);
static int                 publicNoteColourIndexFromWorldAttribute(const QString                     &value,
                                                                   const QList<WorldRuntime::Colour> &colours,
                                                                   int                                fallbackPublicIndex);
static QStringList         macroDescriptionList();
static QStringList         keypadNameList();

constexpr int              kChatLoopDiscardSeconds           = 5;
constexpr int              ADJUST_COLOUR_INVERT              = 1;
constexpr int              ADJUST_COLOUR_LIGHTER             = 2;
constexpr int              ADJUST_COLOUR_DARKER              = 3;
constexpr int              ADJUST_COLOUR_LESS_COLOUR         = 4;
constexpr int              ADJUST_COLOUR_MORE_COLOUR         = 5;
constexpr int              kPacketDebugChars                 = 16;
constexpr int              kMaxMxpTextBufferBytes            = 256 * 1024;
constexpr int              kMaxMxpStackDepth                 = 512;
constexpr int              kMemoryImageDecodeCacheMaxEntries = 48;
constexpr qint64           kMemoryImageDecodeCacheMaxBytes   = 64LL * 1024LL * 1024LL;
constexpr int              kAnsiBlack                        = 0;
constexpr int              kAnsiWhite                        = 7;

namespace
{
	using QMudOutputWrapUtils::FixedColumnWrapConfig;
	using QMudOutputWrapUtils::localOutputWrapConfig;
	using QMudOutputWrapUtils::OutputLineSegment;
	using QMudOutputWrapUtils::splitOutputTextAtLineBreaks;
	using QMudOutputWrapUtils::trailingLineColumnWidthForWrap;
	using QMudOutputWrapUtils::wrapPlainLineForColumn;
	using QMudOutputWrapUtils::wrapStyledLineForColumn;

	bool isAsciiPrintableByte(const unsigned char c)
	{
		return c >= 0x20 && c < 0x7F;
	}

	double miniWindowDevicePixelRatioForView(const WorldView *view)
	{
		if (!view)
			return 1.0;
		return MiniWindow::normalizedDevicePixelRatio(view->devicePixelRatioF());
	}

	bool isAsciiAlnumByte(const unsigned char c)
	{
		return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
	}

	int safeQSizeToInt(const qsizetype size)
	{
		if (size <= 0)
			return 0;
		constexpr qsizetype kMaxInt = std::numeric_limits<int>::max();
		return size > kMaxInt ? std::numeric_limits<int>::max() : static_cast<int>(size);
	}

	bool isMxpModeCode(const int mode, const TelnetProcessor::MxpMode expected)
	{
		return mode == TelnetProcessor::mxpModeCode(expected);
	}

	bool isMxpLockedModeCode(const int mode)
	{
		return isMxpModeCode(mode, TelnetProcessor::MxpMode::Locked) ||
		       isMxpModeCode(mode, TelnetProcessor::MxpMode::PermanentLocked);
	}

	bool isMxpOpenModeCode(const int mode)
	{
		return isMxpModeCode(mode, TelnetProcessor::MxpMode::Open) ||
		       isMxpModeCode(mode, TelnetProcessor::MxpMode::PermanentOpen);
	}

	bool isMxpResetModeCode(const int mode)
	{
		return isMxpModeCode(mode, TelnetProcessor::MxpMode::Reset);
	}

	QString replaceMxpTextEntityReferences(const QString &text, const QString &replacement)
	{
		static constexpr QLatin1StringView kEscapedTextEntity{"&amp;text;"};
		static constexpr QLatin1StringView kTextEntity{"&text;"};

		QString                            result;
		result.reserve(text.size());
		const QStringView textView{text};
		for (qsizetype i = 0; i < text.size();)
		{
			if (textView.mid(i, kEscapedTextEntity.size()) == kEscapedTextEntity)
			{
				result.append(replacement);
				i += kEscapedTextEntity.size();
				continue;
			}
			if (textView.mid(i, kTextEntity.size()) == kTextEntity)
			{
				result.append(replacement);
				i += kTextEntity.size();
				continue;
			}
			result.append(text.at(i));
			++i;
		}
		return result;
	}

	bool resolveMxpTextActionReferences(QVector<WorldRuntime::StyleSpan> &spans, const int startColumn,
	                                    const int endColumn, const WorldRuntime::MxpStyleState &actionState,
	                                    const QString &replacement)
	{
		if (startColumn < 0 || endColumn <= startColumn)
			return false;

		const QString resolvedAction = actionState.action.isEmpty()
		                                   ? replacement
		                                   : replaceMxpTextEntityReferences(actionState.action, replacement);
		const QString resolvedHint   = replaceMxpTextEntityReferences(actionState.hint, replacement);

		int           spanStart = 0;
		bool          changed   = false;
		for (WorldRuntime::StyleSpan &span : spans)
		{
			const int spanLength = qMax(0, span.length);
			const int spanEnd    = spanStart + spanLength;
			if (spanLength > 0 && spanEnd > startColumn && spanStart < endColumn &&
			    span.actionType == actionState.actionType && span.action == actionState.action &&
			    span.hint == actionState.hint)
			{
				span.action = resolvedAction;
				span.hint   = resolvedHint;
				changed     = true;
			}
			spanStart = spanEnd;
		}
		return changed;
	}

	WorldRuntime::MxpStyleFrame makeMxpStyleFrame(const QByteArray                  &tag,
	                                              const WorldRuntime::MxpStyleState &state)
	{
		WorldRuntime::MxpStyleFrame frame;
		frame.tag   = tag;
		frame.state = state;
		return frame;
	}

	template <typename Func>
	bool qmudInvokeMethod(QObject *target, Func &&fn,
	                      Qt::ConnectionType connectionType = Qt::BlockingQueuedConnection)
	{
		if (!target)
			return false;
		if (connectionType != Qt::BlockingQueuedConnection)
			return QMetaObject::invokeMethod(target, std::forward<Func>(fn), connectionType);
		if (target->thread() == QThread::currentThread())
		{
			std::forward<Func>(fn)();
			return true;
		}
		return qmudLuaBridgeInvokeOnObjectThread(target, std::forward<Func>(fn));
	}

	template <typename Fallback, typename Func>
	auto qmudInvokeMethodOr(QObject *target, const Fallback &fallback, Func &&fn) -> std::decay_t<Fallback>
	{
		using Result       = std::decay_t<Fallback>;
		Result     result  = fallback;
		const bool invoked = qmudInvokeMethod(target, [&result, fn = std::forward<Func>(fn)]() mutable
		                                      { result = static_cast<Result>(std::move(fn)()); });
		return invoked ? result : fallback;
	}

	QRect validWindowGeometry(const QRect &preferred, const QRect &fallback)
	{
		return preferred.isValid() && !preferred.isNull() ? preferred : fallback;
	}

	QRect screenWindowGeometry(const QWidget *window, const QRect &geometry)
	{
		if (!window)
			return geometry;
		QRect screenGeometry = geometry;
		screenGeometry.moveTopLeft(window->mapToGlobal(screenGeometry.topLeft()));
		return screenGeometry;
	}

	bool closeNotepadWindowOnMainThread(const QString &title, const bool querySave,
	                                    const qulonglong ownerToken, const QString &worldId)
	{
		QCoreApplication *app = QCoreApplication::instance();
		if (!app)
			return false;
		const QString trimmedTitle = title.trimmed();
		if (trimmedTitle.isEmpty())
			return false;

		return qmudInvokeMethodOr(
		    app, false,
		    [&trimmedTitle, querySave, ownerToken, worldId]() -> bool
		    {
			    AppController const *controller = AppController::instance();
			    MainWindow          *main       = controller ? controller->mainWindow() : nullptr;
			    if (!main)
				    return false;

			    TextChildWindow               *target   = nullptr;
			    const QList<TextChildWindow *> notepads = main->findChildren<TextChildWindow *>();
			    for (TextChildWindow *text : notepads)
			    {
				    if (!text || text->windowTitle().compare(trimmedTitle, Qt::CaseInsensitive) != 0)
					    continue;

				    if (QMudMainFrameMdiUtils::windowMatchesRuntimeIdentity(text, ownerToken, worldId, false))
				    {
					    target = text;
					    break;
				    }
			    }

			    if (!target)
				    return false;

			    const QPointer<TextChildWindow> guard(target);
			    target->setQuerySaveOnClose(querySave);
			    target->close();
			    return guard.isNull() || !guard->isVisible();
		    });
	}

	void captureMainWindowSnapshotForLuaDispatch(LuaCallbackMiniWindowSnapshot &snapshot,
	                                             const WorldRuntime            *ownerRuntime)
	{
		auto *app = QCoreApplication::instance();
		if (!app)
			return;

		const bool collected = qmudInvokeMethod(
		    app,
		    [&snapshot, ownerRuntime]
		    {
			    snapshot.hasUiSnapshot = true;
			    MainWindowHost *host =
			        ownerRuntime ? resolveMainWindowHostForRuntime(const_cast<WorldRuntime *>(ownerRuntime))
			                     : resolveMainWindowHost(nullptr);
			    auto *frame = dynamic_cast<MainWindow *>(host);
			    if (!frame)
				    return;
			    snapshot.framePointer    = frame;
			    snapshot.hasFramePointer = true;

			    snapshot.mainWindowPositionsByMode.insert(
			        0, validWindowGeometry(frame->normalGeometry(), frame->geometry()));
			    snapshot.mainWindowPositionsByMode.insert(
			        1, validWindowGeometry(frame->frameGeometry(), frame->geometry()));

			    QHash<WorldRuntime *, int> worldOrdinals;
			    for (const WorldWindowDescriptor &entry : frame->worldWindowDescriptors())
			    {
				    if (!entry.runtime)
					    continue;

				    LuaCallbackWorldRuntimeSnapshot world;
				    world.runtime = entry.runtime;
				    snapshot.worldRuntimeSnapshot.push_back(world);

				    if (!entry.window)
					    continue;
				    const int                              ordinal = ++worldOrdinals[entry.runtime];
				    LuaCallbackWorldWindowPositionSnapshot position;
				    position.runtime = entry.runtime;
				    position.ordinal = ordinal;
				    position.normalGeometry =
				        validWindowGeometry(entry.window->normalGeometry(), entry.window->geometry());
				    position.frameGeometry =
				        validWindowGeometry(entry.window->frameGeometry(), entry.window->geometry());
				    position.screenNormalGeometry =
				        screenWindowGeometry(entry.window, position.normalGeometry);
				    position.screenFrameGeometry = screenWindowGeometry(entry.window, position.frameGeometry);
				    snapshot.worldWindowPositionSnapshot.push_back(position);
			    }

			    for (TextChildWindow *text : frame->notepadWindows())
			    {
				    if (!text)
					    continue;
				    LuaCallbackNotepadSnapshot notepad;
				    const qulonglong runtimeToken = text->property("worldRuntimeToken").toULongLong();
				    notepad.runtime =
				        runtimeToken == 0
				            ? nullptr
				            : reinterpret_cast<WorldRuntime *>(static_cast<quintptr>(runtimeToken));
				    notepad.worldId  = text->property("worldId").toString().trimmed();
				    notepad.title    = text->windowTitle();
				    notepad.geometry = validWindowGeometry(text->normalGeometry(), text->geometry());
				    if (QPlainTextEdit *editor = text->editor())
				    {
					    notepad.text      = editor->toPlainText();
					    notepad.hasEditor = true;
				    }
				    snapshot.notepadSnapshot.push_back(notepad);
			    }
		    });
		if (!collected)
			return;

		for (LuaCallbackWorldRuntimeSnapshot &world : snapshot.worldRuntimeSnapshot)
		{
			if (!world.runtime)
				continue;
			world.id =
			    qmudInvokeMethodOr(world.runtime, QString(), [&world]
			                       { return world.runtime->worldAttributeValue(QStringLiteral("id")); });
			world.name =
			    qmudInvokeMethodOr(world.runtime, QString(), [&world]
			                       { return world.runtime->worldAttributeValue(QStringLiteral("name")); });
		}
	}

	template <typename Func> bool qmudInvokeMethodTrueOnSuccess(QObject *target, Func &&fn)
	{
		bool       result  = false;
		const bool invoked = qmudInvokeMethod(target, [&result, fn = std::forward<Func>(fn)]() mutable
		                                      { result = std::move(fn)(); });
		return invoked && result;
	}

	template <typename Func> void qmudInvokeMethodChecked(QObject *target, Func &&fn)
	{
		(void)qmudInvokeMethod(target, std::forward<Func>(fn));
	}

	template <typename Func> bool qmudInvokeMethodQueued(QObject *target, Func &&fn)
	{
		return qmudInvokeMethod(target, std::forward<Func>(fn), Qt::QueuedConnection);
	}

	void flashTaskbarForView(QWidget *view)
	{
		if (const AppController *app = AppController::instance())
		{
			if (MainWindow *mainWindow = app->mainWindow())
			{
				(void)mainWindow->requestBackgroundTaskbarFlash(view);
				return;
			}
		}
		if (!view)
			return;
		QWidget *alertTarget = view->window();
		if (!alertTarget)
			alertTarget = view;
		QApplication::alert(alertTarget, 0);
	}

	int boundedQSizeToInt(const qsizetype size)
	{
		constexpr qsizetype kMaxInt = std::numeric_limits<int>::max();
		constexpr qsizetype kMinInt = std::numeric_limits<int>::min();
		if (size > kMaxInt)
			return std::numeric_limits<int>::max();
		if (size < kMinInt)
			return std::numeric_limits<int>::min();
		return static_cast<int>(size);
	}

	QSharedPointer<LuaCallbackMiniWindowSnapshot>
	captureMiniWindowSnapshotForLuaDispatch(const QMap<QString, MiniWindow> &miniWindows)
	{
		const auto toRef = [](const QColor &color) -> qlonglong
		{
			return static_cast<qlonglong>(static_cast<long>(color.red()) |
			                              (static_cast<long>(color.green()) << 8) |
			                              (static_cast<long>(color.blue()) << 16));
		};
		const auto toLuaSystemTimeSnapshot = [](const QDateTime &time) -> double
		{
			if (!time.isValid())
				return 0.0;
			return static_cast<double>(time.toSecsSinceEpoch());
		};

		auto snapshot         = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
		snapshot->windowNames = miniWindows.keys();
		for (auto windowIt = miniWindows.constBegin(); windowIt != miniWindows.constEnd(); ++windowIt)
		{
			const QString windowKey = windowIt.key().trimmed().toLower();
			if (windowKey.isEmpty())
				continue;
			const MiniWindow &window = windowIt.value();
			snapshot->miniWindowsByWindow.insert(windowKey, QSharedPointer<MiniWindow>::create(window));
			snapshot->fontIdsByWindow.insert(windowKey, window.fonts.keys());
			snapshot->imageIdsByWindow.insert(windowKey, window.images.keys());
			snapshot->hotspotIdsByWindow.insert(windowKey, window.hotspots.keys());
			for (auto imageIt = window.images.constBegin(); imageIt != window.images.constEnd(); ++imageIt)
			{
				const QString imageKey = imageIt.key().trimmed().toLower();
				if (imageKey.isEmpty())
					continue;
				snapshot->imageHasAlphaByKey.insert(QStringLiteral("%1|%2").arg(windowKey, imageKey),
				                                    imageIt.value().hasAlpha);
			}

			LuaCallbackMiniWindowSnapshot::WindowInfoSnapshot infoSnapshot;
			infoSnapshot.locationX       = window.location.x();
			infoSnapshot.locationY       = window.location.y();
			infoSnapshot.width           = window.width;
			infoSnapshot.height          = window.height;
			infoSnapshot.show            = window.show;
			infoSnapshot.temporarilyHide = window.temporarilyHide;
			infoSnapshot.position        = window.position;
			infoSnapshot.flags           = window.flags;
			infoSnapshot.backgroundRef =
			    window.background.isValid() ? toRef(window.background) : static_cast<qlonglong>(0);
			const QRect apiRect           = window.apiRect();
			infoSnapshot.rectLeft         = apiRect.left();
			infoSnapshot.rectTop          = apiRect.top();
			infoSnapshot.rectRight        = apiRect.right();
			infoSnapshot.rectBottom       = apiRect.bottom();
			infoSnapshot.lastMouseX       = window.lastMousePosition.x();
			infoSnapshot.lastMouseY       = window.lastMousePosition.y();
			infoSnapshot.lastMouseUpdate  = window.lastMouseUpdate;
			infoSnapshot.clientMouseX     = window.clientMousePosition.x();
			infoSnapshot.clientMouseY     = window.clientMousePosition.y();
			infoSnapshot.mouseOverHotspot = window.mouseOverHotspot;
			infoSnapshot.mouseDownHotspot = window.mouseDownHotspot;
			infoSnapshot.installedAt      = toLuaSystemTimeSnapshot(window.installedAt);
			infoSnapshot.zOrder           = window.zOrder;
			infoSnapshot.creatingPlugin   = window.creatingPlugin;
			snapshot->windowInfoByWindow.insert(windowKey, infoSnapshot);
		}
		snapshot->rebuildMiniWindowLookupCaches();
		return snapshot;
	}

	void refreshMiniWindowMouseStateForLuaDispatch(LuaCallbackMiniWindowSnapshot   &snapshot,
	                                               const QMap<QString, MiniWindow> &miniWindows)
	{
		for (auto windowIt = miniWindows.constBegin(); windowIt != miniWindows.constEnd(); ++windowIt)
		{
			const QString windowKey = windowIt.key().trimmed().toLower();
			if (windowKey.isEmpty())
				continue;

			const MiniWindow &window = windowIt.value();
			if (auto snapshotWindowIt = snapshot.miniWindowsByWindow.find(windowKey);
			    snapshotWindowIt != snapshot.miniWindowsByWindow.end() && snapshotWindowIt.value())
			{
				MiniWindow &snapshotWindow         = *snapshotWindowIt.value();
				snapshotWindow.lastMousePosition   = window.lastMousePosition;
				snapshotWindow.lastMouseUpdate     = window.lastMouseUpdate;
				snapshotWindow.clientMousePosition = window.clientMousePosition;
				snapshotWindow.mouseOverHotspot    = window.mouseOverHotspot;
				snapshotWindow.mouseDownHotspot    = window.mouseDownHotspot;
			}

			if (auto infoIt = snapshot.windowInfoByWindow.find(windowKey);
			    infoIt != snapshot.windowInfoByWindow.end())
			{
				infoIt->lastMouseX       = window.lastMousePosition.x();
				infoIt->lastMouseY       = window.lastMousePosition.y();
				infoIt->lastMouseUpdate  = window.lastMouseUpdate;
				infoIt->clientMouseX     = window.clientMousePosition.x();
				infoIt->clientMouseY     = window.clientMousePosition.y();
				infoIt->mouseOverHotspot = window.mouseOverHotspot;
				infoIt->mouseDownHotspot = window.mouseDownHotspot;
			}
		}
	}

	bool worldAttributeAffectsCommandProcessor(const QString &key)
	{
		return key.compare(QStringLiteral("speed_walk_delay"), Qt::CaseInsensitive) == 0 ||
		       key.compare(QStringLiteral("speed_walk_filler"), Qt::CaseInsensitive) == 0 ||
		       key.compare(QStringLiteral("translate_german"), Qt::CaseInsensitive) == 0 ||
		       key.compare(QStringLiteral("translate_backslash_sequences"), Qt::CaseInsensitive) == 0 ||
		       key.compare(QStringLiteral("enable_spam_prevention"), Qt::CaseInsensitive) == 0 ||
		       key.compare(QStringLiteral("spam_line_count"), Qt::CaseInsensitive) == 0 ||
		       key.compare(QStringLiteral("spam_message"), Qt::CaseInsensitive) == 0 ||
		       key.compare(QStringLiteral("do_not_translate_iac_to_iac_iac"), Qt::CaseInsensitive) == 0 ||
		       key.compare(QStringLiteral("regexp_match_empty"), Qt::CaseInsensitive) == 0 ||
		       key.compare(QStringLiteral("legacy_encoding"), Qt::CaseInsensitive) == 0 ||
		       key.compare(QStringLiteral("utf_8"), Qt::CaseInsensitive) == 0;
	}

	int safeQInt64ToInt(const qint64 value)
	{
		if (value <= 0)
			return 0;
		constexpr qint64 kMaxInt = std::numeric_limits<int>::max();
		return static_cast<int>(value > kMaxInt ? kMaxInt : value);
	}

#ifndef NDEBUG
	qint64 pluginCallbackDispatchNowNs()
	{
		static QElapsedTimer timer;
		static bool          started = []()
		{
			timer.start();
			return true;
		}();
		Q_UNUSED(started);
		return timer.nsecsElapsed();
	}

	qint64 pluginCallbackDispatchElapsedMs(const qint64 startNs)
	{
		if (startNs <= 0)
			return 0;
		const qint64 elapsedNs = pluginCallbackDispatchNowNs() - startNs;
		if (elapsedNs <= 0)
			return 0;
		return elapsedNs / 1000000;
	}

	bool qmudMmStartupDiagIsWatchedPluginId(const QString &pluginId)
	{
		const QString normalized = pluginId.trimmed().toLower();
		return normalized == QStringLiteral("c97329b91f12ca48d14c3db2") ||
		       normalized == QStringLiteral("adc3a873d4e47348da7cb426") ||
		       normalized == QStringLiteral("f973af093e715dece34dc25f") ||
		       normalized == QStringLiteral("f67c4339ed0591a5b010d05b");
	}

	bool qmudMmStartupDiagIsLifecycleCallback(const QString &functionName)
	{
		return functionName == QStringLiteral("OnPluginInstall") ||
		       functionName == QStringLiteral("OnPluginEnable") ||
		       functionName == QStringLiteral("OnPluginConnect") ||
		       functionName == QStringLiteral("OnPluginDisable") ||
		       functionName == QStringLiteral("OnPluginBroadcast") ||
		       functionName == QStringLiteral("OnPluginTelnetRequest") ||
		       functionName == QStringLiteral("OnPluginTelnetSubnegotiation");
	}

	QString qmudMmStartupDiagEngineLabel(const QSharedPointer<LuaCallbackEngine> &engine)
	{
		if (!engine)
			return QStringLiteral("<null>");
		const QString id   = engine->pluginIdMetadata().trimmed();
		const QString name = engine->pluginNameMetadata().trimmed();
		if (name.isEmpty())
			return id;
		return QStringLiteral("%1/%2").arg(id, name);
	}

	QString qmudMmStartupDiagEngineLabels(const QVector<QSharedPointer<LuaCallbackEngine>> &engines)
	{
		QStringList labels;
		labels.reserve(engines.size());
		for (const auto &engine : engines)
			labels.push_back(qmudMmStartupDiagEngineLabel(engine));
		return labels.join(QLatin1Char(','));
	}

	bool qmudMmStartupDiagHasWatchedEngine(const QVector<QSharedPointer<LuaCallbackEngine>> &engines)
	{
		return std::ranges::any_of(
		    engines, [](const QSharedPointer<LuaCallbackEngine> &engine)
		    { return engine && qmudMmStartupDiagIsWatchedPluginId(engine->pluginIdMetadata()); });
	}

	bool qmudMmStartupDiagRequestHasWatchedEngine(const LuaBatchDispatchRequest &request)
	{
		return qmudMmStartupDiagHasWatchedEngine(request.engines);
	}

	bool qmudMmStartupDiagShouldLogRequest(const LuaBatchDispatchRequest &request)
	{
		return qmudMmStartupDiagIsLifecycleCallback(request.functionName) &&
		       qmudMmStartupDiagRequestHasWatchedEngine(request);
	}

	bool qmudMmStartupDiagShouldLogBroadcast(const QString &callingPluginId,
	                                         const QVector<QSharedPointer<LuaCallbackEngine>> &recipients)
	{
		if (qmudMmStartupDiagIsWatchedPluginId(callingPluginId))
			return true;
		return std::ranges::any_of(
		    recipients, [](const QSharedPointer<LuaCallbackEngine> &engine)
		    { return engine && qmudMmStartupDiagIsWatchedPluginId(engine->pluginIdMetadata()); });
	}

	QString qmudMmStartupDiagResultLabel(const LuaBatchDispatchResult &result)
	{
		qsizetype mutationCount = 0;
		for (const LuaDeferredRuntimeMutationBatch &batch : result.deferredRuntimeMutationBatches)
			mutationCount += batch.mutations.size();
		return QStringLiteral(
		           "boolValid=%1 bool=%2 countValid=%3 count=%4 deferredBatches=%5 deferredMutations=%6")
		    .arg(result.boolResultValid ? QStringLiteral("1") : QStringLiteral("0"),
		         result.boolResult ? QStringLiteral("1") : QStringLiteral("0"),
		         result.countResultValid ? QStringLiteral("1") : QStringLiteral("0"))
		    .arg(result.countResult)
		    .arg(result.deferredRuntimeMutationBatches.size())
		    .arg(mutationCount);
	}
#endif

	LuaBatchDispatchRequest makePluginBroadcastDispatchRequest(
	    const long message, const QString &text, const QString &callingPluginId,
	    const QString &callingPluginName, const QVector<QSharedPointer<LuaCallbackEngine>> &recipients,
	    const QSharedPointer<const LuaCallbackMiniWindowSnapshot> &miniWindowSnapshot)
	{
		LuaBatchDispatchRequest request;
		request.kind                         = LuaBatchDispatchKind::NumberAndUtf8StringsCount;
		request.engines                      = recipients;
		request.functionName                 = QStringLiteral("OnPluginBroadcast");
		request.numberArg1                   = message;
		request.bytesArg                     = callingPluginId.toUtf8();
		request.bytesArg2                    = callingPluginName.toUtf8();
		request.bytesArg3                    = text.toUtf8();
		request.defaultResult                = true;
		request.miniWindowSnapshotArg        = miniWindowSnapshot;
		request.revalidateObservedRecipients = true;
		return request;
	}

#ifndef NDEBUG
	QString qmudMmStartupDiagBroadcastSnapshotLabel(
	    const QSharedPointer<const LuaCallbackMiniWindowSnapshot> &snapshot)
	{
		if (!snapshot)
			return QStringLiteral("snapshot=0");
		return QStringLiteral("snapshot=1 hasBroadcastSnapshot=%1 broadcastIds=%2 broadcastEngines=%3 ids=%4")
		    .arg(snapshot->hasBroadcastPluginSnapshot ? QStringLiteral("1") : QStringLiteral("0"))
		    .arg(snapshot->broadcastPluginIdsSnapshot.size())
		    .arg(snapshot->broadcastPluginEnginesSnapshot.size())
		    .arg(snapshot->broadcastPluginIdsSnapshot.join(QLatin1Char(',')));
	}

	QString pluginCallbackDispatchKindLabel(const LuaBatchDispatchKind kind)
	{
		switch (kind)
		{
		case LuaBatchDispatchKind::NoArgs:
			return QStringLiteral("NoArgs");
		case LuaBatchDispatchKind::String:
			return QStringLiteral("String");
		case LuaBatchDispatchKind::StringStopOnFalse:
			return QStringLiteral("StringStopOnFalse");
		case LuaBatchDispatchKind::StringHandled:
			return QStringLiteral("StringHandled");
		case LuaBatchDispatchKind::StringInOut:
			return QStringLiteral("StringInOut");
		case LuaBatchDispatchKind::BytesInOut:
			return QStringLiteral("BytesInOut");
		case LuaBatchDispatchKind::NumberAndBytes:
			return QStringLiteral("NumberAndBytes");
		case LuaBatchDispatchKind::NumberAndBytesStopOnTrue:
			return QStringLiteral("NumberAndBytesStopOnTrue");
		case LuaBatchDispatchKind::ProcedureWithString:
			return QStringLiteral("ProcedureWithString");
		case LuaBatchDispatchKind::CancelSuspendedModalString:
			return QStringLiteral("CancelSuspendedModalString");
		case LuaBatchDispatchKind::ResumeSuspendedModalString:
			return QStringLiteral("ResumeSuspendedModalString");
		default:
			return QStringLiteral("Kind(%1)").arg(static_cast<int>(kind));
		}
	}

	bool pluginCallbackDispatchIsCommandPath(const QString &functionName)
	{
		return functionName == QStringLiteral("OnPluginSent") ||
		       functionName == QStringLiteral("OnPluginSend") ||
		       functionName == QStringLiteral("OnPluginCommandEntered");
	}
#endif

	bool pluginCallbackDispatchIsInputCritical(const LuaBatchDispatchRequest &request)
	{
		if (request.inputCritical)
			return true;
		if (request.kind == LuaBatchDispatchKind::ExecuteScript && request.executeScriptHasTriggerContext)
			return true;
		if (request.kind == LuaBatchDispatchKind::StringsAndWildcards &&
		    request.triggerMatchedLineAbsoluteNumber > 0)
			return true;

		static const QSet<QString> kInputCriticalCallbacks{
		    QStringLiteral("OnPluginTelnetRequest"), QStringLiteral("OnPluginTelnetSubnegotiation"),
		    QStringLiteral("OnPluginTelnetOption"),  QStringLiteral("OnPluginPacketReceived"),
		    QStringLiteral("OnPluginMXPopenTag"),    QStringLiteral("OnPluginMXPcloseTag"),
		    QStringLiteral("OnPluginMXPsetEntity"),  QStringLiteral("OnPluginMXPsetVariable"),
		    QStringLiteral("OnPluginClose"),         QStringLiteral("OnPluginSaveState"),
		};
		return kInputCriticalCallbacks.contains(request.functionName);
	}

#ifndef NDEBUG
	bool pluginCallbackDispatchShouldLog(const bool diagnosticsEnabled, const QString &functionName,
	                                     const qint64 elapsedMs)
	{
		return diagnosticsEnabled && (pluginCallbackDispatchIsCommandPath(functionName) || elapsedMs >= 25);
	}
#endif

	LuaBatchDispatchResult pluginCallbackDispatchFallback(const LuaBatchDispatchRequest &request)
	{
		LuaBatchDispatchResult fallback{};
		if (request.kind == LuaBatchDispatchKind::StringStopOnFalse ||
		    request.kind == LuaBatchDispatchKind::NumberAndStringStopOnFalse ||
		    request.kind == LuaBatchDispatchKind::TwoNumbersAndStringStopOnFalse)
		{
			fallback.boolResult      = true;
			fallback.boolResultValid = true;
		}
		else if (request.kind == LuaBatchDispatchKind::StringHandled ||
		         request.kind == LuaBatchDispatchKind::NumberAndStringStopOnTrue ||
		         request.kind == LuaBatchDispatchKind::NumberAndBytesStopOnTrue ||
		         request.kind == LuaBatchDispatchKind::ProcedureWithString)
		{
			fallback.boolResult      = false;
			fallback.boolResultValid = true;
		}
		if (request.kind == LuaBatchDispatchKind::StringsAndWildcards)
		{
			fallback.hasFunction      = false;
			fallback.hasFunctionValid = true;
		}
		if (request.kind == LuaBatchDispatchKind::NumberAndUtf8StringsCount)
		{
			fallback.countResult      = 0;
			fallback.countResultValid = true;
		}
		if (request.kind == LuaBatchDispatchKind::ExecuteScript)
		{
			fallback.boolResult      = false;
			fallback.boolResultValid = true;
		}
		if (request.kind == LuaBatchDispatchKind::BytesInOut)
			fallback.bytesResult = request.bytesArg;
		if (request.kind == LuaBatchDispatchKind::StringInOut)
			fallback.stringResult = request.stringArg;
		return fallback;
	}

	void applyLuaDeferredRuntimeMutationBatch(LuaDeferredRuntimeMutationBatch &&batch)
	{
		if (!batch.runtime || batch.mutations.isEmpty())
			return;

		WorldRuntime *runtime = batch.runtime;
		if (QThread::currentThread() != runtime->thread() &&
		    LuaCallbackEngine::appendDeferredRuntimeMutationBatchToActiveCallback(batch))
		{
			return;
		}

		QPointer<WorldRuntime>         runtimeGuard(runtime);
		QVector<std::function<void()>> mutations  = std::move(batch.mutations);
		auto                           applyBatch = [runtimeGuard, mutations = std::move(mutations)]() mutable
		{
			if (!runtimeGuard)
				return;
			runtimeGuard->beginMiniWindowMutationBatch();
			const auto closeMiniWindowBatch = qScopeGuard(
			    [runtimeGuard]
			    {
				    if (runtimeGuard)
					    runtimeGuard->endMiniWindowMutationBatch();
			    });
			runtimeGuard->beginOutputViewMutationBatch();
			const auto closeOutputViewBatch = qScopeGuard(
			    [runtimeGuard]
			    {
				    if (runtimeGuard)
					    runtimeGuard->endOutputViewMutationBatch();
			    });
			for (auto &mutation : mutations)
			{
				if (mutation)
					mutation();
			}
		};

		if (QThread::currentThread() == runtime->thread())
		{
			applyBatch();
			return;
		}

		if (!qmudLuaBridgeInvokeOnObjectThread(runtime, applyBatch))
		{
			qWarning().noquote()
			    << QStringLiteral("[QMud][LuaBridge] deferred runtime mutation journal apply failed: %1")
			           .arg(qmudLuaBridgeLastError());
		}
	}

	void applyLuaDeferredRuntimeMutationBatches(LuaBatchDispatchResult &result)
	{
		QVector<LuaDeferredRuntimeMutationBatch> batches;
		batches.swap(result.deferredRuntimeMutationBatches);
		for (LuaDeferredRuntimeMutationBatch &batch : batches)
			applyLuaDeferredRuntimeMutationBatch(std::move(batch));
	}

	[[nodiscard]] bool textRectangleSettingsEqual(const WorldRuntime::TextRectangleSettings &lhs,
	                                              const WorldRuntime::TextRectangleSettings &rhs)
	{
		return lhs.left == rhs.left && lhs.top == rhs.top && lhs.right == rhs.right &&
		       lhs.bottom == rhs.bottom && lhs.borderOffset == rhs.borderOffset &&
		       lhs.borderColour == rhs.borderColour && lhs.borderWidth == rhs.borderWidth &&
		       lhs.outsideFillColour == rhs.outsideFillColour && lhs.outsideFillStyle == rhs.outsideFillStyle;
	}

	bool hasValidPluginId(const WorldRuntime::Plugin &plugin)
	{
		return !plugin.attributes.value(QStringLiteral("id")).trimmed().isEmpty();
	}

	QString normalizedPluginId(const QString &pluginId)
	{
		return pluginId.trimmed().toLower();
	}

	QString normalizedAsyncResultApiName(const QString &apiName)
	{
		return apiName.trimmed().toLower();
	}

	bool canExecutePlugin(const WorldRuntime::Plugin &plugin)
	{
		return hasValidPluginId(plugin) && plugin.enabled && plugin.lua && !plugin.installPending;
	}

	QVector<int> collectExecutablePluginRecipientIndicesForCallback(
	    const QString &functionName, const QList<WorldRuntime::Plugin> &plugins,
	    const QHash<QString, QVector<int>> &recipientIndexCache)
	{
		QVector<int> recipientIndices;
		if (functionName.isEmpty())
			return recipientIndices;

		const auto recipientsIt = recipientIndexCache.constFind(functionName);
		if (recipientsIt == recipientIndexCache.constEnd())
			return recipientIndices;

		const int          pluginCount = safeQSizeToInt(plugins.size());
		const QVector<int> indices = qmudFilterValidPluginRecipientIndices(recipientsIt.value(), pluginCount);
		if (indices.isEmpty())
			return recipientIndices;

		recipientIndices.reserve(indices.size());
		for (const int pluginIndex : indices)
		{
			if (pluginIndex < 0 || pluginIndex >= plugins.size())
				continue;
			const auto &plugin = plugins.at(pluginIndex);
			if (!canExecutePlugin(plugin))
				continue;
			recipientIndices.push_back(pluginIndex);
		}
		return recipientIndices;
	}

	long roundedToLong(const double value)
	{
		return std::lround(value);
	}

	int lowByteToInt(const long value)
	{
		return static_cast<int>(value & 0xFFL);
	}

	double longToDouble(const long value)
	{
		return static_cast<double>(value);
	}

	int asciiHexValue(const unsigned char c)
	{
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if (c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return -1;
	}

	void appendHexByte(QByteArray &out, const unsigned char c)
	{
		constexpr char kHex[] = "0123456789abcdef";
		out.append(kHex[(c >> 4) & 0x0F]);
		out.append(kHex[c & 0x0F]);
	}

} // namespace

static double runtimeRandomUnit()
{
	if (AppController *app = AppController::instance())
		return app->nextRandomUnit();
	return QRandomGenerator::global()->generateDouble();
}

static MiniWindowUtils::RandomUnit seededMiniWindowRandomUnit(const quint32 seed)
{
	auto generator = std::make_shared<QRandomGenerator>(seed);
	return [generator]() { return generator->generateDouble(); };
}

constexpr int kChatNameChange         = 1;
constexpr int kChatRequestConnections = 2;
constexpr int kChatConnectionList     = 3;
constexpr int kChatTextEverybody      = 4;
constexpr int kChatTextPersonal       = 5;
constexpr int kChatTextGroup          = 6;
constexpr int kChatMessage            = 7;
constexpr int kChatDoNotDisturb       = 8;
constexpr int kChatSendAction         = 9;
constexpr int kChatSendAlias          = 10;
constexpr int kChatSendMacro          = 11;
constexpr int kChatSendVariable       = 12;
constexpr int kChatSendEvent          = 13;
constexpr int kChatSendGag            = 14;
constexpr int kChatSendHighlight      = 15;
constexpr int kChatSendList           = 16;
constexpr int kChatSendArray          = 17;
constexpr int kChatSendBaritem        = 18;
constexpr int kChatVersion            = 19;
constexpr int kChatFileStart          = 20;
constexpr int kChatFileDeny           = 21;
constexpr int kChatFileBlockRequest   = 22;
constexpr int kChatFileBlock          = 23;
constexpr int kChatFileEnd            = 24;
constexpr int kChatFileCancel         = 25;
constexpr int kChatPingRequest        = 26;
constexpr int kChatPingResponse       = 27;
constexpr int kChatPeekConnections    = 28;

struct MxpSupportTag
{
		const char *name;
		const char *args;
};

static const MxpSupportTag *mxpSupportedTags()
{
	static const MxpSupportTag kSupportedTags[] = {
	    {"send",             "href,hint,prompt,xch_cmd,xch_hint"},
	    {"a",	            "href,xch_cmd,xch_hint"            },
	    {"bold",             ""	                             },
	    {"b",	            ""	                             },
	    {"strong",           ""	                             },
	    {"underline",        ""	                             },
	    {"u",	            ""	                             },
	    {"italic",           ""	                             },
	    {"i",	            ""	                             },
	    {"em",	           ""	                             },
	    {"color",            "fore,back"                        },
	    {"c",	            "fore,back"                        },
	    {"font",             "color,back,fgcolor,bgcolor"       },
	    {"high",             ""	                             },
	    {"h",	            ""	                             },
	    {"br",	           ""	                             },
	    {"p",	            ""	                             },
	    {"hr",	           ""	                             },
	    {"pre",              ""	                             },
	    {"ul",	           ""	                             },
	    {"ol",	           ""	                             },
	    {"li",	           ""	                             },
	    {"samp",             ""	                             },
	    {"reset",            ""	                             },
	    {"mxp",              "off"	                          },
	    {"version",          ""	                             },
	    {"support",          ""	                             },
	    {"option",           ""	                             },
	    {"recommend_option", ""	                             },
	    {"afk",              "challenge"                        },
	    {"user",             ""	                             },
	    {"username",         ""	                             },
	    {"password",         ""	                             },
	    {"pass",             ""	                             },
	    {"var",              ""	                             },
	    {"v",	            ""	                             },
	    {"body",             ""	                             },
	    {"head",             ""	                             },
	    {"html",             ""	                             },
	    {"title",            ""	                             },
	    {"img",              "src,xch_mode"                     },
	    {"xch_page",         ""	                             },
	    {nullptr,            nullptr	                        }
    };
	return kSupportedTags;
}

static int mxpBuiltinTagCount()
{
	int         count     = 0;
	const auto *supported = mxpSupportedTags();
	for (int i = 0; supported[i].name; ++i)
		count++;
	return count;
}

constexpr int     kChatPeekList  = 29;
constexpr int     kChatSnoop     = 30;
constexpr int     kChatSnoopData = 31;

constexpr int     kChatIcon          = 100;
constexpr int     kChatStatus        = 101;
constexpr int     kChatEmailAddress  = 102;
constexpr int     kChatRequestPgpKey = 103;
constexpr int     kChatPgpKey        = 104;
constexpr int     kChatSendCommand   = 105;
constexpr int     kChatStamp         = 106;

constexpr char    kChatEndOfCommand = static_cast<char>(0xFF);

constexpr int     kChatTypeMudMaster = 0;
constexpr int     kChatTypeZMud      = 1;

constexpr int     kChatStatusClosed                    = 0;
constexpr int     kChatStatusConnecting                = 1;
constexpr int     kChatStatusAwaitingConnectConfirm    = 2;
constexpr int     kChatStatusAwaitingConnectionRequest = 3;
constexpr int     kChatStatusConnected                 = 4;

constexpr int     kDefaultChatPort       = 4050;
constexpr int     kDefaultChatBlockSize  = 500;
constexpr int     kDefaultZChatBlockSize = 1024;

static QByteArray makeChatStamp(long stamp)
{
	QByteArray payload(4, '\0');
	payload[0] = static_cast<char>(stamp & 0xFF);
	payload[1] = static_cast<char>((stamp >> 8) & 0xFF);
	payload[2] = static_cast<char>((stamp >> 16) & 0xFF);
	payload[3] = static_cast<char>((stamp >> 24) & 0xFF);
	return payload;
}

static long extractChatStamp(QByteArray &payload)
{
	if (payload.size() < 4)
		return 0;
	const auto *data  = reinterpret_cast<const unsigned char *>(payload.constData());
	const long  stamp = static_cast<long>(data[0]) | (static_cast<long>(data[1]) << 8) |
	                    (static_cast<long>(data[2]) << 16) | (static_cast<long>(data[3]) << 24);
	payload           = payload.mid(4);
	return stamp;
}

static quint32 sha1Word(const QByteArray &digest, int wordIndex)
{
	const int offset = wordIndex * 4;
	if (digest.size() < offset + 4)
		return 0;
	const auto *data = reinterpret_cast<const unsigned char *>(digest.constData() + offset);
	return (static_cast<quint32>(data[0]) << 24) | (static_cast<quint32>(data[1]) << 16) |
	       (static_cast<quint32>(data[2]) << 8) | static_cast<quint32>(data[3]);
}

static QString formatSha1Words(const QByteArray &digest)
{
	return QStringLiteral("%1 %2 %3 %4 %5")
	    .arg(sha1Word(digest, 0), 8, 16, QLatin1Char('0'))
	    .arg(sha1Word(digest, 1), 8, 16, QLatin1Char('0'))
	    .arg(sha1Word(digest, 2), 8, 16, QLatin1Char('0'))
	    .arg(sha1Word(digest, 3), 8, 16, QLatin1Char('0'))
	    .arg(sha1Word(digest, 4), 8, 16, QLatin1Char('0'));
}

static QByteArray ansiCode(int code)
{
	QByteArray payload("\x1b[");
	payload.append(QByteArray::number(code));
	payload.append('m');
	return payload;
}

namespace
{
	constexpr int kDbErrorIdNotFound            = -1;
	constexpr int kDbErrorNotOpen               = -2;
	constexpr int kDbErrorHavePreparedStatement = -3;
	constexpr int kDbErrorNoPreparedStatement   = -4;
	constexpr int kDbErrorNoValidRow            = -5;
	constexpr int kDbErrorDatabaseAlreadyExists = -6;
	constexpr int kDbErrorColumnOutOfRange      = -7;

	constexpr int kAnsiBold     = 1;
	constexpr int kAnsiTextRed  = 31;
	constexpr int kAnsiTextCyan = 36;

} // namespace

class WorldRuntime::ChatConnection : public QObject
{
	public:
		friend class WorldRuntime;

		explicit ChatConnection(WorldRuntime *runtime, QTcpSocket *socket)
		    : QObject(runtime), m_runtime(runtime), m_socket(socket)
		{
			if (m_socket)
			{
				m_socket->setParent(this);
				connect(m_socket, &QTcpSocket::readyRead, this, &ChatConnection::onReadyRead);
				connect(m_socket, &QTcpSocket::connected, this, &ChatConnection::onConnected);
				connect(m_socket, &QTcpSocket::disconnected, this, &ChatConnection::onDisconnected);
				connect(m_socket, &QTcpSocket::bytesWritten, this, &ChatConnection::onBytesWritten);
				connect(m_socket, &QTcpSocket::errorOccurred, this, &ChatConnection::onError);
			}

			if (m_runtime)
			{
				if (++m_runtime->m_nextChatId > std::numeric_limits<long>::max())
					m_runtime->m_nextChatId = 1;
				m_chatId = m_runtime->m_nextChatId;
			}

			m_zChatStamp         = makeRandomStamp();
			m_zChatStatus        = 1;
			m_whenStarted        = QDateTime::currentDateTime();
			m_lastIncoming       = QDateTime();
			m_lastOutgoing       = QDateTime();
			m_chatStatus         = kChatStatusClosed;
			m_chatConnectionType = kChatTypeMudMaster;
			m_fileBlockSize      = kDefaultChatBlockSize;
		}

		~ChatConnection() override
		{
			stopFileTransfer(true);
			if (m_socket)
			{
				static_cast<void>(disconnect(m_socket, nullptr, this, nullptr));
				if (m_socket->state() != QAbstractSocket::UnconnectedState)
					m_socket->disconnectFromHost();
			}
			if (m_runtime && m_wasConnected)
			{
				m_runtime->callPluginCallbacksWithNumberAndString(
				    QStringLiteral("OnPluginChatUserDisconnect"), m_chatId, m_remoteUserName, false);
			}
		}

		[[nodiscard]] long id() const
		{
			return m_chatId;
		}
		[[nodiscard]] bool isConnected() const
		{
			return m_chatStatus == kChatStatusConnected;
		}
		[[nodiscard]] bool isIncoming() const
		{
			return m_incoming;
		}
		[[nodiscard]] const QString &allegedAddress() const
		{
			return m_allegedAddress;
		}
		[[nodiscard]] int allegedPort() const
		{
			return m_allegedPort;
		}

		void startOutgoing(const QString &server, int port, bool zChat)
		{
			if (!m_socket || !m_runtime)
				return;
			m_serverName         = server;
			m_chatConnectionType = zChat ? kChatTypeZMud : kChatTypeMudMaster;
			m_fileBlockSize      = zChat ? kDefaultZChatBlockSize : kDefaultChatBlockSize;
			m_chatStatus         = kChatStatusConnecting;

			if (port == 0)
				port = kDefaultChatPort;

			m_socket->connectToHost(server, static_cast<quint16>(port));
		}

		void startIncoming()
		{
			if (!m_socket)
				return;
			m_incoming   = true;
			m_chatStatus = kChatStatusAwaitingConnectionRequest;
			if (m_socket->peerAddress().protocol() == QAbstractSocket::IPv4Protocol)
				m_serverName = m_socket->peerAddress().toString();
			else
				m_serverName = m_socket->peerAddress().toString();
		}

		void sendChatMessage(int message, const QString &text, long stamp = 0)
		{
			if (!m_socket || m_deleteMe || m_chatStatus != kChatStatusConnected)
				return;

			if (!m_runtime->callPluginCallbacksStopOnFalseWithTwoNumbersAndString(
			        QStringLiteral("OnPluginChatMessageOut"), m_chatId, message, text))
				return;

			if (message == kChatSnoop && m_youAreSnooping)
				m_youAreSnooping = false;

			QByteArray payload;
			if (m_chatConnectionType == kChatTypeMudMaster)
			{
				payload.append(static_cast<char>(message & 0xFF));
				payload.append(text.toLatin1());
				if (message != kChatFileBlock)
					payload.replace(kChatEndOfCommand, 'y');
				payload.append(kChatEndOfCommand);
			}
			else
			{
				QByteArray messageBytes;
				if (message == kChatTextEverybody || message == kChatTextPersonal ||
				    message == kChatTextGroup)
					messageBytes = makeChatStamp(stamp ? stamp : m_zChatStamp) + text.toLatin1();
				else
					messageBytes = text.toLatin1();

				const int length = safeQSizeToInt(messageBytes.size());
				payload.append(static_cast<char>(message & 0xFF));
				payload.append(static_cast<char>((message >> 8) & 0xFF));
				payload.append(static_cast<char>(length & 0xFF));
				payload.append(static_cast<char>((length >> 8) & 0xFF));
				payload.append(messageBytes);
			}

			sendData(payload);
		}

		void stopFileTransfer(bool abort)
		{
			if (!m_doingFileTransfer)
				return;

			if (m_file)
			{
				m_file->close();
				m_file.reset();
			}
			m_fileBuffer.clear();

			if (abort && m_chatStatus == kChatStatusConnected)
				sendChatMessage(kChatFileCancel, QString());

			if (abort)
			{
				if (m_sendFile)
					m_runtime->chatNote(14, QStringLiteral("Aborted sending file %1").arg(m_ourFileName));
				else
				{
					m_runtime->chatNote(14, QStringLiteral("Aborted receiving file %1").arg(m_ourFileName));
					m_runtime->chatNote(14, QStringLiteral("File %1 deleted.").arg(m_ourFileName));
					QFile::remove(m_ourFileName);
				}
			}

			m_doingFileTransfer   = false;
			m_sendFile            = false;
			m_fileSize            = 0;
			m_fileBlocks          = 0;
			m_blocksTransferred   = 0;
			m_startedFileTransfer = QDateTime();
		}

		void sendData(const QByteArray &data)
		{
			if (m_deleteMe || !m_socket)
				return;
			m_lastOutgoing = QDateTime::currentDateTime();
			m_outstandingOutput.append(data);
			flushOutput();
		}

		void close()
		{
			if (m_chatStatus == kChatStatusConnected)
				m_runtime->chatNote(1, QStringLiteral("Chat session to %1 closed.").arg(m_remoteUserName));
			m_chatStatus = kChatStatusClosed;
			m_deleteMe   = true;
			if (m_socket)
				m_socket->disconnectFromHost();
		}

	private slots:
		void onReadyRead()
		{
			if (!m_socket)
				return;
			m_lastIncoming            = QDateTime::currentDateTime();
			const QByteArray incoming = m_socket->readAll();
			if (incoming.isEmpty())
				return;
			m_outstandingInput.append(incoming);
			processInput();
		}

		void onConnected()
		{
			if (!m_runtime)
				return;
			if (m_chatStatus != kChatStatusConnecting)
				return;

			m_serverAddress  = m_socket->peerAddress();
			m_serverPort     = m_socket->peerPort();
			m_allegedAddress = m_serverAddress.toString();
			m_allegedPort    = m_serverPort;

			if (m_runtime->isChatAlreadyConnected(m_allegedAddress, m_allegedPort, this))
			{
				m_runtime->chatNote(0, QStringLiteral("You are already connected to %1 port %2")
				                           .arg(m_allegedAddress)
				                           .arg(m_allegedPort));
				close();
				return;
			}

			m_runtime->chatNote(
			    0,
			    QStringLiteral("Calling chat server at %1 port %2").arg(m_allegedAddress).arg(m_allegedPort));

			m_runtime->chatNote(1, QStringLiteral("Session established to %1.").arg(m_serverName));

			const QString hostAddress  = chatLocalAddress();
			const int     incomingPort = m_runtime->chatIncomingPort();
			if (m_chatConnectionType == kChatTypeZMud)
				sendData(QStringLiteral("ZCHAT:%1\t\n%2%3")
				             .arg(m_runtime->chatOurName(), hostAddress,
				                  QStringLiteral("%1").arg(incomingPort, 5, 10, QLatin1Char('0')))
				             .toLatin1());
			else
				sendData(QStringLiteral("CHAT:%1\n%2%3")
				             .arg(m_runtime->chatOurName(), hostAddress,
				                  QStringLiteral("%1").arg(incomingPort, -5, 10, QLatin1Char(' ')))
				             .toLatin1());

			m_chatStatus = kChatStatusAwaitingConnectConfirm;
		}

		void onDisconnected()
		{
			close();
			if (m_runtime)
				m_runtime->removeChatConnection(this);
		}

		void onBytesWritten(qint64)
		{
			flushOutput();
		}

		void onError(QAbstractSocket::SocketError)
		{
			if (!m_socket || !m_runtime)
				return;
			if (m_chatStatus == kChatStatusConnecting || m_chatStatus == kChatStatusAwaitingConnectConfirm)
			{
				const QString error = m_socket->errorString();
				m_runtime->chatNote(0, QStringLiteral("Unable to connect to \"%1\", code = %2 (%3)")
				                           .arg(m_serverName)
				                           .arg(static_cast<int>(m_socket->error()))
				                           .arg(error));
				close();
			}
		}

	private:
		void flushOutput()
		{
			if (!m_socket || m_outstandingOutput.isEmpty())
				return;
			const qint64 sent = m_socket->write(m_outstandingOutput);
			if (sent > 0)
			{
				m_outstandingOutput = m_outstandingOutput.mid(static_cast<int>(sent));
			}
		}

		void processInput()
		{
			if (m_deleteMe || !m_runtime)
				return;

			if (m_chatStatus == kChatStatusAwaitingConnectConfirm)
			{
				if (m_outstandingInput.size() < 4)
					return;
				if (!m_outstandingInput.startsWith("YES:"))
				{
					m_runtime->chatNote(0, QStringLiteral("Server rejected chat session attempt."));
					close();
					return;
				}

				m_outstandingInput.remove(0, 4);
				const qsizetype newline = m_outstandingInput.indexOf('\n');
				if (newline == -1)
				{
					m_remoteUserName = QString::fromLatin1(m_outstandingInput);
					m_outstandingInput.clear();
				}
				else
				{
					m_remoteUserName = QString::fromLatin1(m_outstandingInput.left(newline));
					m_outstandingInput.remove(0, newline + 1);
				}

				m_runtime->chatNote(
				    1, QStringLiteral("Chat session accepted, remote server: \"%1\"").arg(m_remoteUserName));

				m_chatStatus   = kChatStatusConnected;
				m_wasConnected = true;
				sendChatMessage(kChatVersion, QStringLiteral("QMud v") + QString::fromLatin1(kVersionString));
				if (m_chatConnectionType == kChatTypeZMud)
				{
					sendChatMessage(kChatStatus, QStringLiteral("\x01"));
					sendChatMessage(kChatStamp, QString::fromLatin1(makeChatStamp(m_zChatStamp)));
				}
				m_runtime->callPluginCallbacksWithNumberAndString(QStringLiteral("OnPluginChatNewUser"),
				                                                  m_chatId, m_remoteUserName, false);
			}

			if (m_chatStatus == kChatStatusAwaitingConnectionRequest)
			{
				if (m_outstandingInput.size() < 7)
					return;
				const bool isChat  = m_outstandingInput.startsWith("CHAT:");
				const bool isZChat = m_outstandingInput.startsWith("ZCHAT:");
				if (!isChat && !isZChat)
				{
					m_runtime->chatNote(0, QStringLiteral("Unexpected chat negotiation."));
					sendData("NO");
					close();
					return;
				}

				if (isChat)
				{
					m_outstandingInput.remove(0, 5);
					m_chatConnectionType = kChatTypeMudMaster;
				}
				else
				{
					m_outstandingInput.remove(0, 6);
					m_chatConnectionType = kChatTypeZMud;
					m_fileBlockSize      = kDefaultZChatBlockSize;
				}

				const qsizetype newline = m_outstandingInput.indexOf('\n');
				if (newline == -1)
				{
					m_remoteUserName = QString::fromLatin1(m_outstandingInput);
					m_outstandingInput.clear();
				}
				else
				{
					m_remoteUserName = QString::fromLatin1(m_outstandingInput.left(newline));
					m_outstandingInput.remove(0, newline + 1);
				}

				const qsizetype tabIndex = m_remoteUserName.indexOf('\t');
				if (tabIndex != -1)
					m_remoteUserName = m_remoteUserName.left(tabIndex);

				QString         rest        = QString::fromLatin1(m_outstandingInput);
				const qsizetype restNewline = rest.indexOf('\n');
				if (restNewline != -1)
					rest = rest.left(restNewline);

				if (rest.size() > 5)
				{
					m_allegedPort    = rest.right(5).toInt();
					m_allegedAddress = rest.left(rest.size() - 5);
				}

				m_outstandingInput.clear();

				const QString acceptPayload =
				    QStringLiteral("%1,%2").arg(m_serverAddress.toString(), m_remoteUserName);
				if (!m_runtime->callPluginCallbacksStopOnFalse(QStringLiteral("OnPluginChatAccept"),
				                                               acceptPayload))
				{
					sendData("NO");
					close();
					return;
				}

				if (m_runtime->validateIncomingChatCalls())
				{
					const QString question =
					    QStringLiteral(
					        "Incoming chat call to world %1 from %2, IP address: %3.\n\nAccept it?")
					        .arg(m_runtime->worldName(), m_remoteUserName, m_serverAddress.toString());
					if (QMessageBox::question(nullptr, QStringLiteral("Chat"), question) != QMessageBox::Yes)
					{
						sendData("NO");
						close();
						return;
					}
				}

				m_runtime->chatNote(
				    1, QStringLiteral("Chat session accepted, remote user: \"%1\"").arg(m_remoteUserName));
				sendData(QStringLiteral("YES:%1\n").arg(m_runtime->chatOurName()).toLatin1());
				m_chatStatus   = kChatStatusConnected;
				m_wasConnected = true;
				sendChatMessage(kChatVersion,
				                QStringLiteral("MUSHclient v") + QString::fromLatin1(kVersionString));
				if (m_chatConnectionType == kChatTypeZMud)
				{
					sendChatMessage(kChatStatus, QStringLiteral("\x01"));
					sendChatMessage(kChatStamp, QString::fromLatin1(makeChatStamp(m_zChatStamp)));
				}

				m_runtime->callPluginCallbacksWithNumberAndString(QStringLiteral("OnPluginChatNewUser"),
				                                                  m_chatId, m_remoteUserName, false);
			}

			while (!m_deleteMe && !m_outstandingInput.isEmpty())
			{
				if (m_chatConnectionType == kChatTypeMudMaster)
				{
					if (static_cast<unsigned char>(m_outstandingInput.at(0)) ==
					    static_cast<unsigned char>(kChatFileBlock))
					{
						const int required = m_fileBlockSize + 2;
						if (m_outstandingInput.size() < required)
							return;
						const QByteArray block = m_outstandingInput.mid(1, m_fileBlockSize);
						m_outstandingInput.remove(0, required);
						processChatMessage(kChatFileBlock, QString::fromLatin1(block));
					}
					else
					{
						const qsizetype terminator = m_outstandingInput.indexOf(kChatEndOfCommand);
						if (terminator == -1)
							return;
						const QByteArray message = m_outstandingInput.left(terminator);
						m_outstandingInput.remove(0, terminator + 1);
						if (message.isEmpty())
							continue;
						const int code = static_cast<unsigned char>(message.at(0));
						processChatMessage(code, QString::fromLatin1(message.mid(1)));
					}
				}
				else
				{
					if (m_outstandingInput.size() < 4)
						return;
					const auto *data =
					    reinterpret_cast<const unsigned char *>(m_outstandingInput.constData());
					const int code   = data[0] | (data[1] << 8);
					const int length = data[2] | (data[3] << 8);
					if (m_outstandingInput.size() < length + 4)
						return;
					const QByteArray message = m_outstandingInput.mid(4, length);
					m_outstandingInput.remove(0, length + 4);
					processChatMessage(code, QString::fromLatin1(message));
				}
			}
		}

		void processChatMessage(int message, const QString &text)
		{
			if (m_deleteMe || m_chatStatus != kChatStatusConnected)
				return;

			if (!m_runtime->callPluginCallbacksStopOnFalseWithTwoNumbersAndString(
			        QStringLiteral("OnPluginChatMessage"), m_chatId, message, text))
				return;

			switch (message)
			{
			case kChatTextEverybody:
				processTextEverybody(text);
				break;
			case kChatTextPersonal:
				processTextPersonal(text);
				break;
			case kChatMessage:
				processMessage(text);
				break;
			case kChatTextGroup:
				processTextGroup(text);
				break;
			case kChatPingRequest:
				processPingRequest(text);
				break;
			case kChatPingResponse:
				processPingResponse(text);
				break;
			case kChatVersion:
				m_remoteVersion = text;
				break;
			case kChatRequestConnections:
				processRequestConnections();
				break;
			case kChatConnectionList:
				processConnectionList(text);
				break;
			case kChatPeekConnections:
				processPeekConnections();
				break;
			case kChatPeekList:
				processPeekList(text);
				break;
			case kChatSnoop:
				processSnoop(text);
				break;
			case kChatSnoopData:
				processSnoopData(text);
				break;
			case kChatNameChange:
				processNameChange(text);
				break;
			case kChatSendCommand:
				processSendCommand(text);
				break;
			case kChatFileStart:
				processFileStart(text);
				break;
			case kChatFileDeny:
				processFileDeny(text);
				break;
			case kChatFileBlockRequest:
				processFileBlockRequest();
				break;
			case kChatFileBlock:
				processFileBlock(text);
				break;
			case kChatFileEnd:
				processFileEnd();
				break;
			case kChatFileCancel:
				processFileCancel();
				break;
			case kChatIcon:
				break;
			case kChatStatus:
				if (!text.isEmpty())
					m_zChatStatus = static_cast<unsigned char>(text.at(0).toLatin1());
				break;
			case kChatEmailAddress:
				m_emailAddress = text;
				break;
			case kChatStamp:
				processStamp(text);
				break;
			case kChatRequestPgpKey:
				sendChatMessage(kChatMessage, QStringLiteral("Cannot send PGP key."));
				break;
			case kChatPgpKey:
				m_pgpKey = text;
				break;
			default:
				sendChatMessage(kChatMessage, QStringLiteral("\n%1 does not support the chat command %2.\n")
				                                  .arg(m_runtime->chatOurName())
				                                  .arg(message));
				m_runtime->chatNote(13, QStringLiteral("Received unsupported chat command %1 from %2")
				                            .arg(message)
				                            .arg(m_remoteUserName));
				break;
			}
		}

		void processNameChange(const QString &message)
		{
			const QString oldName = m_remoteUserName;
			m_remoteUserName      = message;
			m_runtime->chatNote(
			    2, QStringLiteral("%1 has changed his/her name to %2.").arg(oldName, m_remoteUserName));
		}

		void processRequestConnections()
		{
			QStringList parts;
			for (ChatConnection const *connection : m_runtime->chatConnections())
			{
				if (!connection->isConnected() || connection->m_private)
					continue;
				if (connection == this)
					continue;
				parts << connection->m_allegedAddress;
				parts << QString::number(connection->m_allegedPort);
			}

			m_runtime->chatNote(
			    13, QStringLiteral("%1 has requested your public connections").arg(m_remoteUserName));
			sendChatMessage(kChatConnectionList, parts.join(QLatin1Char(',')));
		}

		void processConnectionList(const QString &message) const
		{
			const QStringList parts = message.split(QLatin1Char(','), Qt::KeepEmptyParts);
			const int         count = safeQSizeToInt(parts.size() / 2);
			m_runtime->chatNote(11, QStringLiteral("Found %1 connection%2 to %3")
			                            .arg(count)
			                            .arg(count == 1 ? QString() : QStringLiteral("s"))
			                            .arg(m_remoteUserName));
			for (int i = 0; i < count; ++i)
			{
				const QString &address = parts.at(i * 2);
				const int      port    = parts.at(i * 2 + 1).toInt();
				m_runtime->chatCall(address, port, false);
			}
		}

		void processTextEverybody(const QString &message)
		{
			if (m_ignore)
				return;

			QByteArray raw   = message.toLatin1();
			long       stamp = 0;
			if (m_chatConnectionType == kChatTypeZMud)
				stamp = extractChatStamp(raw);
			const QString fixedMessage = QString::fromLatin1(raw);

			if (fixedMessage == m_runtime->m_lastChatMessageSent &&
			    m_runtime->m_lastChatMessageTime.isValid() &&
			    m_runtime->m_lastChatMessageTime.secsTo(QDateTime::currentDateTime()) <
			        kChatLoopDiscardSeconds)
				return;

			if (m_chatConnectionType == kChatTypeZMud && m_zChatStamp == stamp)
				return;

			m_countIncomingAll++;
			m_runtime->chatNote(5, fixedMessage);
			if (m_incoming)
				m_runtime->sendChatMessageToAll(kChatTextEverybody, fixedMessage, true, false, false,
				                                m_chatId, QString(), stamp);
			else
				m_runtime->sendChatMessageToAll(kChatTextEverybody, fixedMessage, true, true, false, m_chatId,
				                                QString(), stamp);
		}

		void processTextPersonal(const QString &message)
		{
			if (m_ignore)
				return;
			QByteArray raw   = message.toLatin1();
			long       stamp = 0;
			if (m_chatConnectionType == kChatTypeZMud)
				stamp = extractChatStamp(raw);
			const QString fixedMessage = QString::fromLatin1(raw);
			if (m_chatConnectionType == kChatTypeZMud && m_zChatStamp == stamp)
				return;
			m_countIncomingPersonal++;
			m_runtime->chatNote(4, fixedMessage);
		}

		void processTextGroup(const QString &message)
		{
			if (m_ignore)
				return;

			if (message == m_runtime->m_lastChatGroupMessageSent &&
			    m_runtime->m_lastChatGroupMessageTime.isValid() &&
			    m_runtime->m_lastChatGroupMessageTime.secsTo(QDateTime::currentDateTime()) <
			        kChatLoopDiscardSeconds)
				return;

			QByteArray raw   = message.toLatin1();
			long       stamp = 0;
			if (m_chatConnectionType == kChatTypeZMud)
				stamp = extractChatStamp(raw);
			if (m_chatConnectionType == kChatTypeZMud && m_zChatStamp == stamp)
				return;

			const QString fixedMessage = QString::fromLatin1(raw);
			if (fixedMessage.size() <= 15)
				return;

			m_countIncomingGroup++;
			const QString group = fixedMessage.left(15).trimmed();
			const QString body  = fixedMessage.mid(15);
			m_runtime->chatNote(6, body);
			if (!m_incoming)
				m_runtime->sendChatMessageToAll(kChatTextGroup, fixedMessage, true, true, false, m_chatId,
				                                group, stamp);
		}

		void processMessage(const QString &message)
		{
			if (m_ignore)
				return;
			m_countMessages++;
			m_runtime->chatNote(3, message);
		}

		void processPingRequest(const QString &message)
		{
			sendChatMessage(kChatPingResponse, message);
		}

		void processPingResponse(const QString &message)
		{
			if (m_pingTimer.isValid())
			{
				m_lastPingTime = static_cast<double>(m_pingTimer.elapsed()) / 1000.0;
				m_pingTimer.invalidate();
				m_runtime->chatNote(12, QStringLiteral("Ping time to %1: %2 seconds")
				                            .arg(m_remoteUserName)
				                            .arg(QString::number(m_lastPingTime, 'f', 3)));
			}
			else
			{
				m_runtime->chatNote(12, QStringLiteral("Ping response: %1").arg(message));
			}
		}

		void processPeekConnections()
		{
			QString result;
			for (ChatConnection const *connection : m_runtime->chatConnections())
			{
				if (!connection->isConnected() || connection->m_private)
					continue;
				if (connection == this)
					continue;
				if (m_chatConnectionType == kChatTypeZMud)
				{
					result += connection->m_allegedAddress;
					result += ",";
					result += QString::number(connection->m_allegedPort);
					result += ",";
				}
				else
				{
					result += connection->m_allegedAddress;
					result += "~";
					result += QString::number(connection->m_allegedPort);
					result += "~";
					result += connection->m_remoteUserName;
					result += "~";
				}
			}
			sendChatMessage(kChatPeekList, result);
			m_runtime->chatNote(13,
			                    QStringLiteral("%1 is peeking at your connections").arg(m_remoteUserName));
		}

		void processPeekList(const QString &message) const
		{
			if (m_chatConnectionType == kChatTypeZMud)
			{
				const QStringList parts = message.split(QLatin1Char(','), Qt::KeepEmptyParts);
				const int         count = safeQSizeToInt(parts.size() / 2);
				m_runtime->chatNote(10, QStringLiteral("Peek found %1 connection%2 to %3")
				                            .arg(count)
				                            .arg(count == 1 ? QString() : QStringLiteral("s"))
				                            .arg(m_remoteUserName));
				if (count > 0)
				{
					m_runtime->chatNote(10, QStringLiteral("Address                    Port"));
					m_runtime->chatNote(10, QStringLiteral("=========================  ====="));
				}
				for (int i = 0; i < count; ++i)
				{
					const QString &ip   = parts.at(i * 2);
					const QString &port = parts.at(i * 2 + 1);
					m_runtime->chatNote(10, QStringLiteral("%1 %2")
					                            .arg(ip.leftJustified(26, QLatin1Char(' ')))
					                            .arg(port.leftJustified(5, QLatin1Char(' '))));
				}
			}
			else
			{
				const QStringList parts = message.split(QLatin1Char('~'), Qt::KeepEmptyParts);
				const int         count = safeQSizeToInt(parts.size() / 3);
				m_runtime->chatNote(10, QStringLiteral("Peek found %1 connection%2 to %3")
				                            .arg(count)
				                            .arg(count == 1 ? QString() : QStringLiteral("s"))
				                            .arg(m_remoteUserName));
				if (count > 0)
				{
					m_runtime->chatNote(
					    10, QStringLiteral("Name                    Address                    Port"));
					m_runtime->chatNote(
					    10, QStringLiteral("======================  =========================  ====="));
				}
				for (int i = 0; i < count; ++i)
				{
					const QString &ip   = parts.at(i * 3);
					const QString &port = parts.at(i * 3 + 1);
					const QString &name = parts.at(i * 3 + 2);
					m_runtime->chatNote(10, QStringLiteral("%1 %2 %3")
					                            .arg(name.leftJustified(23, QLatin1Char(' ')))
					                            .arg(ip.leftJustified(26, QLatin1Char(' ')))
					                            .arg(port.leftJustified(5, QLatin1Char(' '))));
				}
			}
		}

		void processSnoop(const QString &)
		{
			if (m_heIsSnooping)
			{
				sendChatMessage(
				    kChatMessage,
				    QStringLiteral("\nYou are no longer snooping %1.\n").arg(m_runtime->chatOurName()));
				m_runtime->chatNote(13, QStringLiteral("%1 has stopped snooping you.").arg(m_remoteUserName));
				m_heIsSnooping = false;
				return;
			}

			if (m_canSnoop)
			{
				if (!m_runtime->autoAllowSnooping())
				{
					const QString question = QStringLiteral("%1 wishes to start snooping you.\n\nPermit it?")
					                             .arg(m_remoteUserName);
					if (QMessageBox::question(nullptr, QStringLiteral("Chat"), question) != QMessageBox::Yes)
					{
						sendChatMessage(kChatMessage,
						                QStringLiteral("\n%1 does not want you to snoop just now.\n")
						                    .arg(m_runtime->chatOurName()));
						return;
					}
				}
				sendChatMessage(kChatMessage,
				                QStringLiteral("\nYou are now snooping %1.\n").arg(m_runtime->chatOurName()));
				m_runtime->chatNote(13, QStringLiteral("%1 is now snooping you.").arg(m_remoteUserName));
				m_heIsSnooping = true;
			}
			else
			{
				sendChatMessage(kChatMessage, QStringLiteral("\n%1 has not given you permission to snoop.\n")
				                                  .arg(m_runtime->chatOurName()));
				m_runtime->chatNote(13, QStringLiteral("%1 attempted to snoop you.").arg(m_remoteUserName));
			}
		}

		void processSnoopData(const QString &message)
		{
			const bool oldNotesRgb = m_runtime->notesInRgb();
			const long oldFore     = m_runtime->noteColourFore();
			const long oldBack     = m_runtime->noteColourBack();

			m_runtime->setNoteColourFore(colorToLong(QColor(QStringLiteral("springgreen"))));
			m_runtime->setNoteColourBack(colorToLong(QColor(Qt::black)));

			const QString &body = message;
			if (body.size() >= 4 && body.left(4).trimmed().size() == 4 && body.left(4).toInt() >= 0)
			{
				const int fore = body.left(2).toInt();
				const int back = body.mid(2, 2).toInt();
				if (fore >= 0 && fore <= 15)
				{
					const QColor foreColour = m_runtime->ansiColour(fore > 7, (fore % 8) + 1);
					m_runtime->setNoteColourFore(colorToLong(foreColour));
					if (back >= 0 && back <= 7)
					{
						const QColor backColour = m_runtime->ansiColour(false, back + 1);
						m_runtime->setNoteColourBack(colorToLong(backColour));
					}
				}
				m_runtime->outputAnsiText(body.mid(4), true);
			}
			else
			{
				m_runtime->outputAnsiText(body, true);
			}

			if (oldNotesRgb)
			{
				m_runtime->setNoteColourFore(oldFore);
				m_runtime->setNoteColourBack(oldBack);
			}
			else
			{
				m_runtime->m_notesInRgb = false;
			}

			m_youAreSnooping = true;
		}

		void processSendCommand(const QString &command)
		{
			if (m_canSendCommands)
			{
				sendChatMessage(
				    kChatMessage,
				    QStringLiteral("\nYou command %1 to '%2'.\n").arg(m_runtime->chatOurName(), command));
				m_runtime->chatNote(
				    16, QStringLiteral("%1 commands you to '%2'.").arg(m_remoteUserName, command));
				static_cast<void>(m_runtime->executeCommand(command));
			}
			else
			{
				sendChatMessage(kChatMessage,
				                QStringLiteral("\n%1 has not given you permission to send commands.\n")
				                    .arg(m_runtime->chatOurName()));
				m_runtime->chatNote(
				    16, QStringLiteral("%1 attempted to send you a command.").arg(m_remoteUserName));
			}
		}

		void processFileStart(const QString &message)
		{
			if (!m_canSendFiles)
			{
				sendChatMessage(kChatFileDeny, QStringLiteral("%1 is not allowing file transfers from you.")
				                                   .arg(m_runtime->chatOurName()));
				return;
			}

			const QStringList parts = message.split(QLatin1Char(','), Qt::KeepEmptyParts);
			if (parts.size() != 2)
			{
				sendChatMessage(kChatFileDeny,
				                QStringLiteral("Expected \"filename,filesize\" but did not get that."));
				return;
			}

			m_senderFileName = parts.at(0);
			if (m_senderFileName.contains('/') || m_senderFileName.contains('\\'))
			{
				sendChatMessage(kChatFileDeny,
				                QStringLiteral("Supplied file name of \"%1\" may not contain slashes.")
				                    .arg(m_senderFileName));
				return;
			}

			bool ok    = false;
			m_fileSize = parts.at(1).toLong(&ok);
			if (!ok)
			{
				sendChatMessage(kChatFileDeny, QStringLiteral("File size was not numeric."));
				return;
			}

			bool         wanted = true;
			const double kb     = static_cast<double>(m_fileSize) / 1024.0;

			if (!m_runtime->autoAllowFiles())
			{
				const QString question =
				    QStringLiteral(
				        "%1 wishes to send you the file \"%2\", size %3 bytes (%4 Kb).\n\nAccept it?")
				        .arg(m_remoteUserName, m_senderFileName)
				        .arg(m_fileSize)
				        .arg(kb, 0, 'f', 1);
				if (QMessageBox::question(nullptr, QStringLiteral("Chat"), question) != QMessageBox::Yes)
					wanted = false;
			}

			if (wanted)
			{
				const QString saveDir = m_runtime->chatSaveDirectory();
				if (saveDir.isEmpty() || !m_runtime->autoAllowFiles())
				{
					const QString title =
					    QStringLiteral("Chat: Save file from %1 as ...").arg(m_remoteUserName);
					const QString path = QFileDialog::getSaveFileName(
					    nullptr, title,
					    saveDir.isEmpty() ? m_senderFileName : saveDir + "/" + m_senderFileName,
					    QStringLiteral("All files (*.*)"));
					if (path.isEmpty())
						wanted = false;
					else
						m_ourFileName = path;
				}
				else
				{
					QString dir = saveDir;
					dir.replace("\\", "/");
					if (!dir.endsWith('/'))
						dir += "/";
					m_ourFileName = dir + m_senderFileName;
				}
			}

			if (!wanted)
			{
				sendChatMessage(
				    kChatFileDeny,
				    QStringLiteral("%1 does not want that particular file.").arg(m_runtime->chatOurName()));
				return;
			}

			m_file = std::make_unique<QFile>(m_ourFileName);
			if (!m_file->open(QIODevice::WriteOnly))
			{
				sendChatMessage(kChatFileDeny,
				                QStringLiteral("%1 can not open that file.").arg(m_runtime->chatOurName()));
				m_runtime->chatNote(14, QStringLiteral("File %1 cannot be opened.").arg(m_ourFileName));
				m_file.reset();
				m_ourFileName.clear();
				m_fileSize = 0;
				return;
			}

			sendChatMessage(kChatFileBlockRequest, QString());
			m_runtime->chatNote(
			    14, QStringLiteral("Receiving a file from %1 -- Filename: %2, Length: %3 bytes (%4 Kb).")
			            .arg(m_remoteUserName, m_ourFileName)
			            .arg(m_fileSize)
			            .arg(kb, 0, 'f', 1));

			m_startedFileTransfer = QDateTime::currentDateTime();
			m_sendFile            = false;
			m_doingFileTransfer   = true;
			m_blocksTransferred   = 0;
			m_fileBlocks          = (m_fileSize + m_fileBlockSize - 1) / m_fileBlockSize;
			m_fileSha1.reset();
		}

		void processFileDeny(const QString &message)
		{
			m_runtime->chatNote(14, message);
			stopFileTransfer(true);
		}

		void processFileBlockRequest()
		{
			if (!m_doingFileTransfer)
				return;
			if (!m_sendFile)
			{
				sendChatMessage(kChatMessage, QStringLiteral("We are supposed to be receiving a file."));
				return;
			}

			if (!m_file)
				return;

			if (m_fileBuffer.size() != m_fileBlockSize)
				m_fileBuffer.resize(m_fileBlockSize);
			m_fileBuffer.fill('\0');

			const qint64 bytesRead = m_file->read(m_fileBuffer.data(), m_fileBlockSize);
			m_blocksTransferred++;

			qint64 expected = m_fileBlockSize;
			if (m_blocksTransferred == m_fileBlocks)
				expected = m_fileSize % m_fileBlockSize;
			if (expected == 0)
				expected = m_fileBlockSize;

			if (bytesRead != expected)
			{
				sendChatMessage(kChatMessage,
				                QStringLiteral("File transfer aborted due to read error by sender."));
				m_runtime->chatNote(
				    14, QStringLiteral("Send of file \"%1\" aborted due to read error.").arg(m_ourFileName));
				stopFileTransfer(true);
				return;
			}

			m_fileSha1.addData(QByteArrayView(m_fileBuffer.constData(), safeQInt64ToInt(bytesRead)));

			sendChatMessage(kChatFileBlock, QString::fromLatin1(m_fileBuffer));
			m_countFileBytesOut += static_cast<long>(bytesRead);

			if (m_blocksTransferred >= m_fileBlocks)
			{
				sendChatMessage(kChatFileEnd, QString());
				m_runtime->chatNote(14, QStringLiteral("Send of file \"%1\" complete.").arg(m_ourFileName));
				const QString sumText = formatSha1Words(m_fileSha1.result());
				sendChatMessage(kChatMessage, QStringLiteral("Sumcheck from sender was: %1").arg(sumText));
				m_runtime->chatNote(14, QStringLiteral("Sumcheck we calculated:   %1").arg(sumText));
				stopFileTransfer(false);
			}
		}

		void processFileBlock(const QString &message)
		{
			if (!m_doingFileTransfer)
				return;
			if (m_sendFile)
			{
				sendChatMessage(kChatMessage, QStringLiteral("We are supposed to be sending a file."));
				return;
			}

			if (!m_file)
				return;

			m_blocksTransferred++;
			qint64 expected = m_fileBlockSize;
			if (m_blocksTransferred == m_fileBlocks)
				expected = m_fileSize % m_fileBlockSize;
			if (expected == 0)
				expected = m_fileBlockSize;

			const QByteArray buffer = message.toLatin1();
			if (m_file->write(buffer.constData(), expected) != expected)
			{
				sendChatMessage(kChatMessage,
				                QStringLiteral("File transfer aborted due to write error by receiver."));
				m_runtime->chatNote(
				    14,
				    QStringLiteral("Receive of file \"%1\" aborted due to write error.").arg(m_ourFileName));
				stopFileTransfer(true);
				return;
			}

			m_fileSha1.addData(QByteArrayView(buffer.constData(), safeQInt64ToInt(expected)));
			m_countFileBytesIn += static_cast<long>(expected);

			sendChatMessage(kChatFileBlockRequest, QString());
			if (m_blocksTransferred >= m_fileBlocks)
			{
				m_runtime->chatNote(14,
				                    QStringLiteral("Receive of file \"%1\" complete.").arg(m_ourFileName));
				const QString sumText = formatSha1Words(m_fileSha1.result());
				m_runtime->chatNote(14, QStringLiteral("Sumcheck as written was:  %1").arg(sumText));
				sendChatMessage(kChatMessage, QStringLiteral("Sumcheck as received was: %1").arg(sumText));
				stopFileTransfer(false);
			}
		}

		void processFileEnd()
		{
			if (!m_doingFileTransfer)
				return;
			if (m_fileBlocks != m_blocksTransferred)
			{
				m_runtime->chatNote(
				    14, QStringLiteral("Transfer of file \"%1\" stopped prematurely.").arg(m_ourFileName));
				stopFileTransfer(true);
			}
			else
				stopFileTransfer(false);
		}

		void processFileCancel()
		{
			stopFileTransfer(true);
		}

		void processStamp(const QString &message)
		{
			if (m_chatConnectionType != kChatTypeZMud)
				return;
			if (message.size() != 4)
				return;
			QByteArray payload = message.toLatin1();
			long const stamp   = extractChatStamp(payload);
			if (stamp == m_zChatStamp)
			{
				m_zChatStamp = makeRandomStamp();
				for (ChatConnection *connection : m_runtime->chatConnections())
				{
					if (!connection->isConnected() || connection->m_chatConnectionType != kChatTypeZMud)
						continue;
					connection->sendChatMessage(kChatStamp, QString::fromLatin1(makeChatStamp(m_zChatStamp)));
				}
			}
		}

		long makeRandomStamp()
		{
			const QString    seed      = QStringLiteral("%1 %2 %3 %4 %5")
			                                 .arg(m_runtime ? m_runtime->worldName() : QString())
			                                 .arg(QRandomGenerator::global()->generate())
			                                 .arg(QCoreApplication::applicationPid())
			                                 .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate))
			                                 .arg(reinterpret_cast<quintptr>(this));
			const QByteArray seedBytes = seed.toLatin1();
			const QByteArray digest    = QCryptographicHash::hash(seedBytes, QCryptographicHash::Sha1);
			return sha1Word(digest, 0);
		}

		WorldRuntime          *m_runtime{nullptr};
		QTcpSocket            *m_socket{nullptr};
		QByteArray             m_outstandingOutput;
		QByteArray             m_outstandingInput;
		QString                m_serverName;
		QHostAddress           m_serverAddress;
		int                    m_serverPort{0};
		QString                m_remoteUserName;
		QString                m_group;
		QString                m_remoteVersion;
		QString                m_allegedAddress{QStringLiteral("<Unknown>")};
		int                    m_allegedPort{kDefaultChatPort};
		int                    m_chatStatus{kChatStatusClosed};
		int                    m_chatConnectionType{kChatTypeMudMaster};
		bool                   m_deleteMe{false};
		long                   m_chatId{0};
		QDateTime              m_whenStarted;
		QDateTime              m_lastIncoming;
		QDateTime              m_lastOutgoing;
		bool                   m_incoming{false};
		bool                   m_ignore{false};
		bool                   m_canSnoop{false};
		bool                   m_youAreSnooping{false};
		bool                   m_heIsSnooping{false};
		bool                   m_canSendCommands{false};
		bool                   m_private{false};
		bool                   m_canSendFiles{false};
		bool                   m_wasConnected{false};
		QElapsedTimer          m_pingTimer;
		double                 m_lastPingTime{0.0};
		long                   m_zChatStamp{0};
		QString                m_emailAddress;
		QString                m_pgpKey;
		short                  m_zChatStatus{1};
		long                   m_userOption{0};

		bool                   m_doingFileTransfer{false};
		bool                   m_sendFile{false};
		QString                m_senderFileName;
		QString                m_ourFileName;
		long                   m_fileSize{0};
		long                   m_fileBlocks{0};
		long                   m_blocksTransferred{0};
		std::unique_ptr<QFile> m_file;
		QDateTime              m_startedFileTransfer;
		int                    m_fileBlockSize{kDefaultChatBlockSize};
		QByteArray             m_fileBuffer;
		QCryptographicHash     m_fileSha1{QCryptographicHash::Sha1};

		long                   m_countIncomingPersonal{0};
		long                   m_countIncomingAll{0};
		long                   m_countIncomingGroup{0};
		long                   m_countOutgoingPersonal{0};
		long                   m_countOutgoingAll{0};
		long                   m_countOutgoingGroup{0};
		long                   m_countMessages{0};
		long                   m_countFileBytesIn{0};
		long                   m_countFileBytesOut{0};
};

namespace
{
	QColor colorFromRef(long value)
	{
		return MiniWindowUtils::colorFromRef(value);
	}

	long colorToRef(const QColor &color)
	{
		return static_cast<long>(color.red()) | (static_cast<long>(color.green()) << 8) |
		       (static_cast<long>(color.blue()) << 16);
	}

	unsigned char clampByte(long value)
	{
		if (value < 0)
			return 0;
		if (value > 255)
			return 255;
		return static_cast<unsigned char>(value);
	}

	bool blendPixelInternal(long blend, long base, short mode, double opacity, long &out)
	{
		long rA = blend & 0xFF;
		long gA = (blend >> 8) & 0xFF;
		long bA = (blend >> 16) & 0xFF;
		long rB = base & 0xFF;
		long gB = (base >> 8) & 0xFF;
		long bB = (base >> 16) & 0xFF;

		long r = 0;
		long g = 0;
		long b = 0;

		if (opacity < 0.0 || opacity > 1.0)
			return false;

		static const std::array<quint8, 256> cosTable = []
		{
			std::array<quint8, 256> table{};
			constexpr double        pi_div255 = 3.1415926535898 / 255.0;
			for (int i = 0; i < 256; ++i)
			{
				const double a                = 64.0 - qCos(static_cast<double>(i) * pi_div255) * 64.0;
				table[static_cast<size_t>(i)] = static_cast<quint8>(std::lround(a));
			}
			return table;
		}();

		auto applyBlendOp = [&](auto blendOp)
		{
			if (opacity < 1.0)
			{
				r = QMudBlend::withOpacity(rA, rB, blendOp, opacity);
				g = QMudBlend::withOpacity(gA, gB, blendOp, opacity);
				b = QMudBlend::withOpacity(bA, bB, blendOp, opacity);
			}
			else
			{
				r = blendOp(rA, rB);
				g = blendOp(gA, gB);
				b = blendOp(bA, bB);
			}
		};

		auto applyRgb = [&](const long outR, const long outG, const long outB)
		{
			if (opacity < 1.0)
			{
				r = QMudBlend::simpleOpacity(static_cast<double>(rB), static_cast<double>(outR), opacity);
				g = QMudBlend::simpleOpacity(static_cast<double>(gB), static_cast<double>(outG), opacity);
				b = QMudBlend::simpleOpacity(static_cast<double>(bB), static_cast<double>(outB), opacity);
			}
			else
			{
				r = outR;
				g = outG;
				b = outB;
			}
		};

		switch (mode)
		{
		case 1:
			applyBlendOp(QMudBlend::normal);
			break;
		case 2:
			applyBlendOp(QMudBlend::average);
			break;
		case 3:
			applyBlendOp([](const long blendChannel, const long baseChannel)
			             { return QMudBlend::interpolate(blendChannel, baseChannel, cosTable.data()); });
			break;
		case 4:
		{
			const double rnd = runtimeRandomUnit();
			r                = (rnd < opacity) ? rA : rB;
			g                = (rnd < opacity) ? gA : gB;
			b                = (rnd < opacity) ? bA : bB;
		}
		break;
		case 5:
			applyBlendOp(QMudBlend::darken);
			break;
		case 6:
			applyBlendOp(QMudBlend::multiply);
			break;
		case 7:
			applyBlendOp(QMudBlend::colorBurn);
			break;
		case 8:
			applyBlendOp(QMudBlend::linearBurn);
			break;
		case 9:
			applyBlendOp(QMudBlend::inverseColorBurn);
			break;
		case 10:
			applyBlendOp(QMudBlend::subtract);
			break;
		case 11:
			applyBlendOp(QMudBlend::lighten);
			break;
		case 12:
			applyBlendOp(QMudBlend::screen);
			break;
		case 13:
			applyBlendOp(QMudBlend::colorDodge);
			break;
		case 14:
			applyBlendOp(QMudBlend::linearDodge);
			break;
		case 15:
			applyBlendOp(QMudBlend::inverseColorDodge);
			break;
		case 16:
			applyBlendOp(QMudBlend::add);
			break;
		case 17:
			applyBlendOp(QMudBlend::overlay);
			break;
		case 18:
			applyBlendOp(QMudBlend::softLight);
			break;
		case 19:
			applyBlendOp(QMudBlend::hardLight);
			break;
		case 20:
			applyBlendOp(QMudBlend::vividLight);
			break;
		case 21:
			applyBlendOp(QMudBlend::linearLight);
			break;
		case 22:
			applyBlendOp(QMudBlend::pinLight);
			break;
		case 23:
			applyBlendOp(QMudBlend::hardMix);
			break;
		case 24:
			applyBlendOp(QMudBlend::difference);
			break;
		case 25:
			applyBlendOp(QMudBlend::exclusion);
			break;
		case 26:
			applyBlendOp(QMudBlend::reflect);
			break;
		case 27:
			applyBlendOp(QMudBlend::glow);
			break;
		case 28:
			applyBlendOp(QMudBlend::freeze);
			break;
		case 29:
			applyBlendOp(QMudBlend::heat);
			break;
		case 30:
			applyBlendOp(QMudBlend::negation);
			break;
		case 31:
			applyBlendOp(QMudBlend::phoenix);
			break;
		case 32:
			applyBlendOp(QMudBlend::stamp);
			break;
		case 33:
			applyBlendOp(QMudBlend::bitXor);
			break;
		case 34:
			applyBlendOp(QMudBlend::bitAnd);
			break;
		case 35:
			applyBlendOp(QMudBlend::bitOr);
			break;
		case 36:
			applyRgb(rA, gB, bB);
			break;
		case 37:
			applyRgb(rB, gA, bB);
			break;
		case 38:
			applyRgb(rB, gB, bA);
			break;
		case 39:
			applyRgb(rA, gA, bB);
			break;
		case 40:
			applyRgb(rB, gA, bA);
			break;
		case 41:
			applyRgb(rA, gB, bA);
			break;
		case 42:
			applyRgb(rA, (gA > rA) ? rA : gA, bA);
			break;
		case 43:
			applyRgb(rA, (gA > bA) ? bA : gA, bA);
			break;
		case 44:
			applyRgb(rA, (gA > ((rA + bA) / 2)) ? ((rA + bA) / 2) : gA, bA);
			break;
		case 45:
			applyRgb(rA, gA, (bA > rA) ? rA : bA);
			break;
		case 46:
			applyRgb(rA, gA, (bA > gA) ? gA : bA);
			break;
		case 47:
			applyRgb(rA, gA, (bA > ((rA + gA) / 2)) ? ((rA + gA) / 2) : bA);
			break;
		case 48:
			applyRgb((rA > gA) ? gA : rA, gA, bA);
			break;
		case 49:
			applyRgb((rA > bA) ? bA : rA, gA, bA);
			break;
		case 50:
			applyRgb((rA > ((gA + bA) / 2)) ? ((gA + bA) / 2) : rA, gA, bA);
			break;
		case 51:
			applyRgb(rA, 0, 0);
			break;
		case 52:
			applyRgb(0, gA, 0);
			break;
		case 53:
			applyRgb(0, 0, bA);
			break;
		case 54:
			applyRgb(0, gA, bA);
			break;
		case 55:
			applyRgb(rA, 0, bA);
			break;
		case 56:
			applyRgb(rA, gA, 0);
			break;
		case 57:
			applyRgb(rA, rA, rA);
			break;
		case 58:
			applyRgb(gA, gA, gA);
			break;
		case 59:
			applyRgb(bA, bA, bA);
			break;
		case 60:
		{
			const QColor cA(lowByteToInt(rA), lowByteToInt(gA), lowByteToInt(bA));
			const QColor cB(lowByteToInt(rB), lowByteToInt(gB), lowByteToInt(bB));
			const int    hue      = (cA.hslHue() < 0) ? 0 : cA.hslHue();
			QColor const outColor = QColor::fromHsl(hue, cB.hslSaturation(), cB.lightness());
			r                     = QMudBlend::simpleOpacity(longToDouble(rB), outColor.red(), opacity);
			g                     = QMudBlend::simpleOpacity(longToDouble(gB), outColor.green(), opacity);
			b                     = QMudBlend::simpleOpacity(longToDouble(bB), outColor.blue(), opacity);
		}
		break;
		case 61:
		{
			const QColor cA(lowByteToInt(rA), lowByteToInt(gA), lowByteToInt(bA));
			const QColor cB(lowByteToInt(rB), lowByteToInt(gB), lowByteToInt(bB));
			const int    sat      = cA.hslSaturation();
			QColor const outColor = QColor::fromHsl(cB.hslHue() < 0 ? 0 : cB.hslHue(), sat, cB.lightness());
			r                     = QMudBlend::simpleOpacity(longToDouble(rB), outColor.red(), opacity);
			g                     = QMudBlend::simpleOpacity(longToDouble(gB), outColor.green(), opacity);
			b                     = QMudBlend::simpleOpacity(longToDouble(bB), outColor.blue(), opacity);
		}
		break;
		case 62:
		{
			const QColor cA(lowByteToInt(rA), lowByteToInt(gA), lowByteToInt(bA));
			const QColor cB(lowByteToInt(rB), lowByteToInt(gB), lowByteToInt(bB));
			const int    hue      = (cA.hslHue() < 0) ? 0 : cA.hslHue();
			const int    sat      = cA.hslSaturation();
			QColor const outColor = QColor::fromHsl(hue, sat, cB.lightness());
			r                     = QMudBlend::simpleOpacity(longToDouble(rB), outColor.red(), opacity);
			g                     = QMudBlend::simpleOpacity(longToDouble(gB), outColor.green(), opacity);
			b                     = QMudBlend::simpleOpacity(longToDouble(bB), outColor.blue(), opacity);
		}
		break;
		case 63:
		{
			const QColor cA(lowByteToInt(rA), lowByteToInt(gA), lowByteToInt(bA));
			const QColor cB(lowByteToInt(rB), lowByteToInt(gB), lowByteToInt(bB));
			QColor const outColor =
			    QColor::fromHsl(cB.hslHue() < 0 ? 0 : cB.hslHue(), cB.hslSaturation(), cA.lightness());
			r = QMudBlend::simpleOpacity(longToDouble(rB), outColor.red(), opacity);
			g = QMudBlend::simpleOpacity(longToDouble(gB), outColor.green(), opacity);
			b = QMudBlend::simpleOpacity(longToDouble(bB), outColor.blue(), opacity);
		}
		break;
		case 64:
			r = QMudBlend::simpleOpacity(longToDouble(rB), 0.0, opacity);
			g = QMudBlend::simpleOpacity(longToDouble(gB), 0.0, opacity);
			b = QMudBlend::simpleOpacity(longToDouble(bB), 0.0, opacity);
			break;
		default:
			return false;
		}

		out = static_cast<long>(clampByte(r)) | (static_cast<long>(clampByte(g)) << 8) |
		      (static_cast<long>(clampByte(b)) << 16);
		return true;
	}

	QRect rectFromCoords(const MiniWindow &window, long left, long top, long right, long bottom)
	{
		const int fixedRight  = window.fixRight(static_cast<int>(right));
		const int fixedBottom = window.fixBottom(static_cast<int>(bottom));
		const int width       = fixedRight - static_cast<int>(left);
		const int height      = fixedBottom - static_cast<int>(top);
		return {static_cast<int>(left), static_cast<int>(top), width, height};
	}

	bool isValidScriptLabel(const QString &label)
	{
		const QString trimmed = label.trimmed();
		if (trimmed.isEmpty())
			return false;
		return std::ranges::all_of(
		    trimmed, [](const QChar ch)
		    { return ch == QLatin1Char('_') || ch == QLatin1Char('.') || ch.isLetterOrNumber(); });
	}

	struct AtomicTagInfo
	{
			bool open{false};
			bool command{false};
			bool noReset{false};
			bool puebloOnly{false};
	};

	const QHash<QByteArray, AtomicTagInfo> &atomicTagInfoMap()
	{
		static const QHash<QByteArray, AtomicTagInfo> kMap = []
		{
			QHash<QByteArray, AtomicTagInfo> map;
			auto                             add =
			    [&map](const QByteArray &name, bool open, bool command, bool noReset, bool puebloOnly = false)
			{
				AtomicTagInfo info;
				info.open       = open;
				info.command    = command;
				info.noReset    = noReset;
				info.puebloOnly = puebloOnly;
				map.insert(name, info);
			};

			// open tags
			add("bold", true, false, false);
			add("b", true, false, false);
			add("high", true, false, false);
			add("h", true, false, false);
			add("underline", true, false, false);
			add("u", true, false, false);
			add("italic", true, false, false);
			add("i", true, false, false);
			add("em", true, false, false);
			add("color", true, false, false);
			add("c", true, false, false);
			add("s", true, false, false);
			add("strike", true, false, false);
			add("strong", true, false, false);
			add("small", true, false, false);
			add("tt", true, false, false);
			add("font", true, false, false);

			// secure tags
			add("frame", false, false, false);
			add("dest", false, false, false);
			add("image", false, true, false);
			add("filter", false, false, false);
			add("a", false, false, false);
			add("h1", false, false, false);
			add("h2", false, false, false);
			add("h3", false, false, false);
			add("h4", false, false, false);
			add("h5", false, false, false);
			add("h6", false, false, false);
			add("hr", false, true, false);
			add("nobr", false, false, false);
			add("p", false, false, false);
			add("script", false, false, false);
			add("send", false, false, false);
			add("ul", false, false, false);
			add("ol", false, false, false);
			add("samp", false, false, false);
			add("center", false, false, false);
			add("var", false, false, false);
			add("v", false, false, false);
			add("gauge", false, false, false);
			add("stat", false, false, false);
			add("expire", false, false, false);
			add("li", false, true, false);

			// secure, command tags
			add("sound", false, true, false);
			add("music", false, true, false);
			add("br", false, true, false);
			add("username", false, true, false);
			add("user", false, true, false);
			add("password", false, true, false);
			add("pass", false, true, false);
			add("relocate", false, true, false);
			add("version", false, true, false);

			// extension tags
			add("reset", false, true, false);
			add("mxp", false, true, false);
			add("support", false, true, false);
			add("option", false, true, false);
			add("afk", false, true, false);
			add("recommend_option", false, true, false);

			// Pueblo tags
			add("pre", false, false, false, true);
			add("body", false, false, true, true);
			add("head", false, false, true, true);
			add("html", false, false, true, true);
			add("title", false, false, false, true);
			add("img", false, true, false, true);
			add("xch_page", false, true, false, true);
			add("xch_pane", false, true, false, true);

			return map;
		}();
		return kMap;
	}

	bool lookupAtomicTagInfo(const QByteArray &tag, AtomicTagInfo &info)
	{
		const auto &map = atomicTagInfoMap();
		const auto  it  = map.find(tag);
		if (it == map.end())
			return false;
		info = it.value();
		return true;
	}

	[[nodiscard]] bool mxpQuotedTokenEndsHere(const char *afterQuote)
	{
		const char *p = afterQuote;
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			++p;
		if (*p == '\0' || *p == '>' || *p == '/')
			return true;
		if (*p == '|' || *p == ',' || *p == ';')
			return false;

		const auto isAttrChar = [](const unsigned char ch)
		{ return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == ':'; };

		if (!isAttrChar(static_cast<unsigned char>(*p)))
			return false;
		while (isAttrChar(static_cast<unsigned char>(*p)))
			++p;
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			++p;
		return *p == '=' || *p == '\0' || *p == '>' || *p == '/';
	}

	bool mxpGetWord(QByteArray &result, QByteArray &input)
	{
		const char *p = input.constData();
		const char *pStart;
		int         len = 0;

		// skip leading spaces
		while (*p == ' ')
			p++;

		// is it quoted string?
		if (*p == '\'' || *p == '\"')
		{
			const char quote = *p;
			pStart           = ++p; // bypass opening quote
			for (; *p; ++p)
			{
				if (*p == quote)
				{
					// Be forgiving with malformed MXP values that embed quote chars
					// without entity escaping (e.g. href='... 'harm' | ...'). Treat a
					// quote as closing only at real token boundaries.
					if (mxpQuotedTokenEndsHere(p + 1))
						break;
				}
				++len;
			}
			result = QByteArray(pStart, len);
			if (*p == quote)
				++p;
			input = QByteArray(p); // return rest of line
			return false;          // not end, even if empty
		}

		// where word starts
		pStart = p;

		// is it a word or number?
		if (isAsciiAlnumByte(static_cast<unsigned char>(*p)) || *p == '+' || *p == '-')
		{
			// skip initial character, and then look for terminator
			for (p++, len++; *p; p++, len++)
			{
				if (isAsciiAlnumByte(static_cast<unsigned char>(*p)))
					continue;
				if (*p == '_' || // underscore
				    *p == '-' || // hyphen
				    *p == '.' || // period
				    *p == ','    // comma
				)
					continue;

				break; // stop if not alpha, digit, or above special characters
			}
		}
		else
			// is it a colour?  ie. #xxxxxx
			if (*p == '#')
			{
				for (p++, len++; *p; p++, len++)
				{
					if (asciiHexValue(static_cast<unsigned char>(*p)) >= 0)
						continue;
					break; // stop if not hex digit
				}
			}
			else
				// is it an argument?  i.e. &xxx;
				if (*p == '&')
				{
					for (p++, len++; *p && *p != ';'; p++, len++)
						; // keep going until ;
					      // include the ; in the word
					if (*p == ';')
					{
						len++;
						p++;
					}
				}
				else if (*p) // provided not end of line
				// assume single character, e.g. '=' or ',' or something
				{
					len = 1;
					p++;
				}

		result = QByteArray(pStart, len);
		input  = QByteArray(p); // return rest of line
		return len == 0;        // true if nothing found
	}

	struct MxpArgument
	{
			QString    name;
			QString    value;
			QByteArray rawName;
			QByteArray rawValue;
			int        position{0};
	};

	struct ParsedMxpArguments
	{
			QString            rawArguments;
			QList<MxpArgument> args;
	};

	using MxpArgumentDecoder = std::function<QString(const QByteArray &)>;

	QString decodeMxpInternalArgumentBytes(const QByteArray &bytes)
	{
		return QString::fromUtf8(bytes);
	}

	ParsedMxpArguments parseMxpArguments(const QByteArray         &rawTag,
	                                     const MxpArgumentDecoder &decodeArgumentBytes)
	{
		ParsedMxpArguments parsed;
		QByteArray         input = rawTag;
		QByteArray         tagName;
		mxpGetWord(tagName, input);
		parsed.rawArguments = decodeArgumentBytes(input);

		int        position = 0;
		QByteArray pending;
		bool       hasPending = false;

		while (true)
		{
			QByteArray argName;
			if (hasPending)
			{
				argName    = pending;
				hasPending = false;
			}
			else
			{
				if (mxpGetWord(argName, input))
					break;
			}

			if (argName.isEmpty())
				break;

			if (argName == "/")
				break;

			QByteArray equals;
			const bool atEnd = mxpGetWord(equals, input);

			if (equals == "=")
			{
				QByteArray value;
				if (mxpGetWord(value, input))
					break;
				MxpArgument arg;
				arg.name     = QString::fromLatin1(argName).toLower();
				arg.value    = decodeArgumentBytes(value);
				arg.rawName  = argName;
				arg.rawValue = value;
				arg.position = 0;
				parsed.args.push_back(arg);
				if (atEnd)
					break;
			}
			else
			{
				MxpArgument arg;
				arg.name.clear();
				arg.value = decodeArgumentBytes(argName);
				arg.rawName.clear();
				arg.rawValue = argName;
				arg.position = ++position;
				parsed.args.push_back(arg);
				if (atEnd)
					break;
				pending    = equals;
				hasPending = true;
			}
		}

		return parsed;
	}

	ParsedMxpArguments parseMxpArguments(const QByteArray &rawTag)
	{
		return parseMxpArguments(rawTag, decodeMxpInternalArgumentBytes);
	}

	QMap<QString, QString>       buildArgumentTable(const QList<MxpArgument> &args);
	QMap<QByteArray, QByteArray> buildArgumentTableBytes(const QList<MxpArgument> &args);

	QList<MxpArgument> internalizeMxpArgumentRawValues(const QList<MxpArgument> &args,
	                                                   const MxpArgumentDecoder &decodeArgumentBytes)
	{
		QList<MxpArgument> internalized;
		internalized.reserve(args.size());
		for (MxpArgument arg : args)
		{
			arg.rawValue = decodeArgumentBytes(arg.rawValue).toUtf8();
			internalized.push_back(arg);
		}
		return internalized;
	}

	QMap<QByteArray, QByteArray> mergeAttributeDefaultsBytes(const QByteArray         &defaults,
	                                                         const QList<MxpArgument> &provided)
	{
		QMap<QByteArray, QByteArray> merged = buildArgumentTableBytes(provided);
		if (defaults.isEmpty())
			return merged;

		const QByteArray         tag            = QByteArray("tag ") + defaults;
		ParsedMxpArguments const parsedDefaults = parseMxpArguments(tag);
		int                      sequence       = 0;

		for (const MxpArgument &attribute : parsedDefaults.args)
		{
			++sequence;
			QByteArray nameKey;
			QByteArray defaultValue;
			if (attribute.name.isEmpty())
			{
				nameKey = attribute.rawValue.trimmed().toLower();
			}
			else
			{
				nameKey      = attribute.rawName.trimmed().toLower();
				defaultValue = attribute.rawValue;
			}
			if (nameKey.isEmpty())
				continue;

			QByteArray positionalValue;
			QByteArray namedValue;
			bool       hasPositionalValue = false;
			bool       hasNamedValue      = false;
			for (const MxpArgument &providedArg : provided)
			{
				if (!providedArg.rawName.isEmpty() && providedArg.rawName.trimmed().toLower() == nameKey)
				{
					namedValue    = providedArg.rawValue;
					hasNamedValue = true;
					break;
				}
				if (!hasPositionalValue && providedArg.position == sequence)
				{
					positionalValue    = providedArg.rawValue;
					hasPositionalValue = true;
				}
			}

			QByteArray replacement = hasNamedValue        ? namedValue
			                         : hasPositionalValue ? positionalValue
			                                              : defaultValue;

			merged.insert(nameKey, replacement);
			merged.insert(QByteArray::number(sequence), replacement);
		}

		return merged;
	}

	using MxpEntityResolver    = std::function<bool(const QByteArray &, QByteArray &)>;
	using MxpEntityTextEncoder = std::function<QByteArray(const QString &)>;

	QByteArray encodeMxpInternalEntityText(const QString &text)
	{
		return text.toUtf8();
	}

	bool decodeNumericEntityText(const QByteArray &name, QString &decoded)
	{
		if (!name.startsWith('#'))
			return false;
		bool     ok   = false;
		uint32_t code = 0;
		if (name.size() > 2 && (name.at(1) == 'x' || name.at(1) == 'X'))
			code = name.mid(2).toUInt(&ok, 16);
		else
			code = name.mid(1).toUInt(&ok, 10);
		if (!ok || code > 0x10FFFFu)
			return false;
		decoded.clear();
		decoded.reserve(2);
		decoded.append(QChar::fromUcs4(code));
		return !decoded.isEmpty();
	}

	QByteArray resolveDefinitionEntities(const QByteArray &source, const QMap<QByteArray, QByteArray> &values,
	                                     const MxpEntityResolver    &entityResolver,
	                                     const MxpEntityTextEncoder &textEncoder)
	{
		QByteArray output;
		output.reserve(source.size());
		for (qsizetype i = 0; i < source.size(); ++i)
		{
			const char ch = source.at(i);
			if (ch != '&')
			{
				output.append(ch);
				continue;
			}
			const qsizetype semi = source.indexOf(';', i + 1);
			if (semi < 0)
			{
				output.append(ch);
				continue;
			}
			const QByteArray name = source.mid(i + 1, semi - i - 1);
			if (name == "text")
			{
				output.append('&');
				output.append(name);
				output.append(';');
				i = semi;
				continue;
			}
			const QByteArray key = name.toLower();
			if (values.contains(key))
				output.append(values.value(key));
			else
			{
				QString decodedText;
				if (decodeNumericEntityText(name, decodedText))
					output.append(textEncoder ? textEncoder(decodedText)
					                          : encodeMxpInternalEntityText(decodedText));
				else if (QByteArray resolved; entityResolver && entityResolver(name, resolved))
					output.append(resolved);
				else
				{
					output.append('&');
					output.append(name);
					output.append(';');
				}
			}
			i = semi;
		}
		return output;
	}

	QByteArray resolveDefinitionEntities(const QByteArray &source, const QMap<QByteArray, QByteArray> &values,
	                                     const TelnetProcessor      &telnet,
	                                     const MxpEntityTextEncoder &textEncoder)
	{
		const MxpEntityResolver resolver = [&telnet](const QByteArray &name, QByteArray &resolved) -> bool
		{ return telnet.resolveEntityValue(name, resolved); };
		return resolveDefinitionEntities(source, values, resolver, textEncoder);
	}

	QByteArray resolveDefinitionEntities(const QByteArray &source, const QMap<QByteArray, QByteArray> &values,
	                                     const MxpEntityResolver &entityResolver)
	{
		return resolveDefinitionEntities(source, values, entityResolver, encodeMxpInternalEntityText);
	}

	bool parseDefinitionAlias(const QByteArray &definition, QByteArray &aliasTag,
	                          QMap<QByteArray, QByteArray> &aliasAttributes,
	                          const MxpArgumentDecoder     &decodeArgumentBytes)
	{
		if (definition.isEmpty())
			return false;
		QByteArray trimmed = definition.trimmed();
		if (!trimmed.startsWith('<') || !trimmed.endsWith('>'))
			return false;
		trimmed                         = trimmed.mid(1, trimmed.size() - 2);
		ParsedMxpArguments const parsed = parseMxpArguments(trimmed, decodeArgumentBytes);
		QByteArray               name;
		QByteArray               temp = trimmed;
		mxpGetWord(name, temp);
		if (name.isEmpty())
			return false;
		aliasTag        = name.toLower();
		aliasAttributes = buildArgumentTableBytes(parsed.args);
		return true;
	}

	bool parseDefinitionAlias(const QByteArray &definition, QByteArray &aliasTag,
	                          QMap<QByteArray, QByteArray> &aliasAttributes)
	{
		return parseDefinitionAlias(definition, aliasTag, aliasAttributes, decodeMxpInternalArgumentBytes);
	}

	QVector<QByteArray> extractDefinitionTags(const QByteArray &definition)
	{
		QVector<QByteArray> tags;
		qsizetype           i = 0;
		while (i < definition.size())
		{
			const qsizetype start = definition.indexOf('<', i);
			if (start < 0)
				break;
			bool      inQuote = false;
			char      quote   = 0;
			qsizetype end     = -1;
			for (qsizetype j = start + 1; j < definition.size(); ++j)
			{
				const char ch = definition.at(j);
				if ((ch == '\'' || ch == '\"') && (!inQuote || ch == quote))
				{
					if (!inQuote)
					{
						inQuote = true;
						quote   = ch;
					}
					else
					{
						inQuote = false;
						quote   = 0;
					}
					continue;
				}
				if (!inQuote && ch == '>')
				{
					end = j;
					break;
				}
			}
			if (end < 0)
				break;
			const QByteArray body = definition.mid(start + 1, end - start - 1);
			tags.push_back(body);
			i = end + 1;
		}
		return tags;
	}

	QMap<QString, QString> buildArgumentTable(const QList<MxpArgument> &args)
	{
		QMap<QString, QString> table;
		for (const auto &arg : args)
		{
			if (arg.name.isEmpty())
				table.insert(QString::number(arg.position), arg.value);
			else
				table.insert(arg.name, arg.value);
		}
		return table;
	}

	QMap<QByteArray, QByteArray> buildArgumentTableBytes(const QList<MxpArgument> &args)
	{
		QMap<QByteArray, QByteArray> table;
		for (const auto &arg : args)
		{
			QByteArray key = arg.rawName.isEmpty() ? QByteArray::number(arg.position) : arg.rawName;
			if (!arg.name.isEmpty())
				key = key.trimmed().toLower();
			table.insert(key, arg.rawValue);
		}
		return table;
	}

	QString buildArgumentString(const QList<MxpArgument> &args)
	{
		QStringList parts;
		for (const auto &arg : args)
		{
			if (arg.name.isEmpty())
				parts << QStringLiteral("'%1'").arg(arg.value);
			else
				parts << QStringLiteral("%1='%2'").arg(arg.name, arg.value);
		}
		return parts.join(' ');
	}

	bool isEnabledFlag(const QString &value)
	{
		return value == QStringLiteral("1") || value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
		       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
	}

	int currentMxpDebugLevel(const QMap<QString, QString> &worldAttributes)
	{
		int           debugLevel = DBG_NONE;
		bool          ok         = false;
		const QString rawDebug   = worldAttributes.value(QStringLiteral("mxp_debug_level"));
		debugLevel               = rawDebug.toInt(&ok);
		if (!ok)
		{
			const WorldNumericOption *opt =
			    QMudWorldOptions::findWorldNumericOption(QStringLiteral("mxp_debug_level"));
			if (opt)
				debugLevel = static_cast<int>(opt->defaultValue);
		}
		return debugLevel;
	}

	void applyMappingFailureIfMatched(WorldRuntime &runtime, const QString &line)
	{
		if (!runtime.isMapping())
			return;

		const QMap<QString, QString> &attrs          = runtime.worldAttributes();
		const QString                 mappingFailure = attrs.value(QStringLiteral("mapping_failure"));
		if (mappingFailure.isEmpty())
			return;

		const bool useRegexp = isEnabledFlag(attrs.value(QStringLiteral("map_failure_regexp")));
		bool       matched   = false;
		if (useRegexp)
		{
			const QRegularExpression re(mappingFailure);
			if (re.isValid())
				matched = re.match(line).hasMatch();
		}
		else
		{
			matched = (line == mappingFailure);
		}

		if (matched)
			runtime.deleteLastMapItem();
	}

	QTime timeFromParts(int hour, int minute, double second)
	{
		if (hour < 0 || minute < 0 || second < 0.0)
			return {};
		const int secInt = qFloor(second);
		int       msec   = qRound((second - secInt) * 1000.0);
		int       adjSec = secInt;
		if (msec >= 1000)
		{
			msec -= 1000;
			adjSec += 1;
		}
		return {hour, minute, adjSec, msec};
	}

	qint64 intervalMsFromParts(int hour, int minute, double second)
	{
		const int    secInt = qFloor(second);
		const qint64 baseMs = static_cast<qint64>(hour) * 3600 * 1000 +
		                      static_cast<qint64>(minute) * 60 * 1000 + static_cast<qint64>(secInt) * 1000;
		const qint64 fracMs = qRound((second - secInt) * 1000.0);
		return baseMs + fracMs;
	}

	void resetTimerFields(WorldRuntime::Timer &timer)
	{
		if (!isEnabledFlag(timer.attributes.value(QStringLiteral("enabled"))))
			return;

		const bool      atTime       = isEnabledFlag(timer.attributes.value(QStringLiteral("at_time")));
		const int       hour         = timer.attributes.value(QStringLiteral("hour")).toInt();
		const int       minute       = timer.attributes.value(QStringLiteral("minute")).toInt();
		const double    second       = timer.attributes.value(QStringLiteral("second")).toDouble();
		const int       offsetHour   = timer.attributes.value(QStringLiteral("offset_hour")).toInt();
		const int       offsetMinute = timer.attributes.value(QStringLiteral("offset_minute")).toInt();
		const double    offsetSecond = timer.attributes.value(QStringLiteral("offset_second")).toDouble();

		const QDateTime now = QDateTime::currentDateTime();
		timer.lastFired     = now;

		if (atTime)
		{
			QTime const at = timeFromParts(hour, minute, second);
			if (!at.isValid())
				return;
			QDateTime fire(QDate::currentDate(), at);
			if (fire < now)
				fire = fire.addDays(1);
			timer.nextFireTime = fire;
			return;
		}

		const qint64 intervalMs = intervalMsFromParts(hour, minute, second);
		const qint64 offsetMs   = intervalMsFromParts(offsetHour, offsetMinute, offsetSecond);
		timer.nextFireTime      = now.addMSecs(intervalMs - offsetMs);
	}

	QDateTime parsePluginDate(const QString &value)
	{
		if (value.trimmed().isEmpty())
			return {};
		QDateTime parsed = QDateTime::fromString(value, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
		if (!parsed.isValid())
			parsed = QDateTime::fromString(value, QStringLiteral("yyyy-MM-dd HH:mm"));
		if (!parsed.isValid())
			parsed = QDateTime::fromString(value, QStringLiteral("yyyy-MM-dd"));
		if (!parsed.isValid())
			parsed = QDateTime::fromString(value, Qt::ISODate);
		return parsed;
	}

	constexpr int kDefaultPluginSequence = 5000;

	int           pluginSequenceFromAttributes(const QMap<QString, QString> &attributes)
	{
		bool      ok  = false;
		const int seq = attributes.value(QStringLiteral("sequence")).toInt(&ok);
		return ok ? seq : kDefaultPluginSequence;
	}

	qlonglong toLuaSystemTime(const QDateTime &time)
	{
		if (!time.isValid())
			return 0;
		return time.toSecsSinceEpoch();
	}

	int findPluginIndex(const QList<WorldRuntime::Plugin> &plugins, const QString &pluginId)
	{
		const QString key = pluginId.trimmed().toLower();
		if (key.isEmpty())
			return -1;
		for (int i = 0; i < plugins.size(); ++i)
		{
			const QString id = plugins.at(i).attributes.value(QStringLiteral("id")).trimmed().toLower();
			if (id == key)
				return i;
		}
		return -1;
	}

	WorldRuntime::Plugin
	makeNativeShimShadowPlugin(const QMudNativePluginRegistry::NativePluginMetadata &metadata,
	                           const bool                                            markGlobal)
	{
		WorldRuntime::Plugin plugin;
		plugin.attributes.insert(QStringLiteral("id"), metadata.id);
		plugin.attributes.insert(QStringLiteral("name"), metadata.name);
		plugin.attributes.insert(QStringLiteral("author"), metadata.author);
		plugin.attributes.insert(QStringLiteral("purpose"), metadata.purpose);
		plugin.attributes.insert(QStringLiteral("language"), metadata.language);
		plugin.attributes.insert(QStringLiteral("enabled"), QStringLiteral("1"));
		plugin.description      = metadata.description;
		plugin.source           = metadata.source;
		plugin.directory        = metadata.directory;
		plugin.enabled          = true;
		plugin.global           = markGlobal;
		plugin.saveState        = false;
		plugin.installPending   = false;
		plugin.nativeShim       = true;
		plugin.nativeShimMarker = QMudNativePluginRegistry::nativeShimMarkerText();
		plugin.sequence         = metadata.sequence;
		plugin.version          = metadata.version;
		plugin.requiredVersion  = metadata.requiredVersion;
		plugin.dateWritten      = metadata.dateWritten;
		plugin.dateModified     = metadata.dateModified;
		plugin.dateInstalled    = QDateTime::currentDateTime();
		return plugin;
	}

	int nativeShimShadowPluginIndex(const QList<WorldRuntime::Plugin> &plugins, const QString &shimId)
	{
		const int index = findPluginIndex(plugins, shimId);
		return index >= 0 && plugins.at(index).nativeShim ? index : -1;
	}

	bool nativeShimAvailableWithoutShadowPlugin(const QString &shimId)
	{
		return shimId.compare(QMudNativePluginRegistry::mushReaderPluginId(), Qt::CaseInsensitive) != 0;
	}

	bool nativeShimVisibleForPluginApi(const QList<WorldRuntime::Plugin> &plugins, const QString &shimId)
	{
		return nativeShimShadowPluginIndex(plugins, shimId) >= 0 ||
		       nativeShimAvailableWithoutShadowPlugin(shimId);
	}

	bool nativeShimPluginInfoEnabled(const QList<WorldRuntime::Plugin> &plugins, const QString &shimId)
	{
		if (const int shadowIndex = nativeShimShadowPluginIndex(plugins, shimId); shadowIndex >= 0)
			return plugins.at(shadowIndex).enabled;
		return nativeShimAvailableWithoutShadowPlugin(shimId);
	}

	QMudNativePluginRegistry::NativeCallContext
	nativeShimCallContext(const WorldRuntime *runtime, const QList<WorldRuntime::Plugin> &plugins,
	                      const QString &shimId)
	{
		QMudNativePluginRegistry::NativeCallContext context;
		context.installed     = nativeShimVisibleForPluginApi(plugins, shimId);
		context.pluginEnabled = nativeShimPluginInfoEnabled(plugins, shimId);
		context.mushReaderSpeechEnabled =
		    shimId.compare(QMudNativePluginRegistry::mushReaderPluginId(), Qt::CaseInsensitive) == 0
		        ? QMudNativePluginRegistry::isMushReaderSpeechEnabled(runtime)
		        : true;
		const int shadowIndex = nativeShimShadowPluginIndex(plugins, shimId);
		if (shadowIndex >= 0)
			context.pluginName = plugins.at(shadowIndex).attributes.value(QStringLiteral("name"));
		if (context.pluginName.trimmed().isEmpty())
		{
			QMudNativePluginRegistry::NativePluginMetadata metadata;
			if (QMudNativePluginRegistry::metadataForShim(shimId, metadata))
				context.pluginName = metadata.name;
		}
		return context;
	}

	QMudNativePluginRegistry::NativeCallResult nativeShimCallGateSynchronizationError()
	{
		QMudNativePluginRegistry::NativeCallResult result;
		result.errorCode = eErrorCallingPluginRoutine;
		result.errorText =
		    QStringLiteral("Failed to synchronize native CallPlugin state with runtime thread");
		return result;
	}

	QString fixHtmlString(const QString &source)
	{
		QString cleaned;
		cleaned.reserve(source.size());
		for (const QChar ch : source)
		{
			const ushort code = ch.unicode();
			if (code < 0x20 && code != 0x09 && code != 0x0A && code != 0x0D)
				continue;
			cleaned += ch;
		}

		QString   strOldString = cleaned;
		QString   strNewString;

		qsizetype i;

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
	}

	QString fixHtmlMultilineString(const QString &source)
	{
		QString cleaned;
		cleaned.reserve(source.size());
		for (const QChar ch : source)
		{
			const ushort code = ch.unicode();
			if (code < 0x20 && code != 0x09 && code != 0x0A && code != 0x0D)
				continue;
			cleaned += ch;
		}

		QString   strOldString = cleaned;
		QString   strNewString;

		qsizetype i;

		while ((i = strOldString.indexOf(QRegularExpression(QStringLiteral("[<>&\\t]")))) != -1)
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
			case '\t':
				strNewString += QStringLiteral("&#9;");
				break;
			default:
				break;
			}

			strOldString = strOldString.mid(i + 1);
		}

		strNewString += strOldString;

		return strNewString;
	}

	QString replaceNewlines(const QString &value)
	{
		QString result = value;
		result.replace(QStringLiteral("\r\n"), QStringLiteral(" "));
		result.replace(QLatin1Char('\n'), QLatin1Char(' '));
		result.replace(QLatin1Char('\r'), QLatin1Char(' '));
		return result;
	}

	long parseColorRef(const QString &value)
	{
		if (value.isEmpty())
			return 0;
		QColor const color(value);
		if (color.isValid())
			return colorToLong(color);
		bool       ok      = false;
		const long numeric = value.toLong(&ok);
		return ok ? numeric : 0;
	}

	bool isLikelyPathAttributeName(const QString &name)
	{
		const QString key = name.trimmed().toLower();
		if (key.isEmpty())
			return false;
		return key.contains(QStringLiteral("file")) || key.contains(QStringLiteral("directory")) ||
		       key.contains(QStringLiteral("path")) || key == QStringLiteral("sound") ||
		       key.endsWith(QStringLiteral("_sound"));
	}

	bool isCanonicalizablePathAttributeName(const QString &name)
	{
		const QString key = name.trimmed().toLower();
		if (key.isEmpty())
			return false;
		if (key == QStringLiteral("script_filename") || key == QStringLiteral("auto_log_file_name") ||
		    key == QStringLiteral("chat_file_save_directory") || key == QStringLiteral("sound") ||
		    key == QStringLiteral("beep_sound") || key == QStringLiteral("new_activity_sound"))
		{
			return true;
		}
		return key.endsWith(QStringLiteral("_directory")) || key.endsWith(QStringLiteral("_file")) ||
		       key.endsWith(QStringLiteral("_filename")) || key.endsWith(QStringLiteral("_file_name")) ||
		       key.endsWith(QStringLiteral("_path")) || key.endsWith(QStringLiteral("_sound"));
	}

	QString normalizePathForStorage(const QString &value)
	{
		QString normalized = value;
		normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
		return normalized;
	}

	QString normalizePathForRuntime(const QString &value)
	{
		return normalizePathForStorage(value);
	}

	bool isAbsolutePathLike(const QString &value)
	{
		if (value.isEmpty())
			return false;
		const QChar first      = value.at(0);
		const bool  isDrive    = value.size() > 1 && value.at(1) == QLatin1Char(':') && first.isLetter();
		const bool  isUncShare = value.startsWith(QStringLiteral("//"));
		return isDrive || isUncShare || first == QLatin1Char('/') || first == QLatin1Char('\\');
	}

	QString normalizeAutoLogFileNameValue(const QString &value)
	{
		QString normalized = normalizePathForStorage(value.trimmed());
		if (normalized.isEmpty() || isAbsolutePathLike(normalized))
			return normalizePathForRuntime(normalized);

		while (normalized.startsWith(QStringLiteral("./")))
			normalized.remove(0, 2);

		return normalizePathForRuntime(normalized);
	}

	void saveXmlNumber(QTextStream &out, const QString &nl, const char *name, long long number,
	                   bool sameLine = false)
	{
		if (number)
			out << (sameLine ? "" : "   ") << name << "=\"" << number << "\"" << (sameLine ? " " : nl);
	}

	void saveXmlBoolean(QTextStream &out, const QString &nl, const char *name, bool value,
	                    bool sameLine = false)
	{
		if (value)
			out << (sameLine ? "" : "   ") << name << "=\"y\"" << (sameLine ? " " : nl);
	}

	void saveXmlString(QTextStream &out, const QString &nl, const char *name, const QString &value,
	                   bool sameLine = false)
	{
		QString output = value;
		if (isLikelyPathAttributeName(QString::fromLatin1(name)))
			output = normalizePathForStorage(output);
		if (!output.isEmpty())
			out << (sameLine ? "" : "   ") << name << "=\"" << fixHtmlString(replaceNewlines(output)) << "\""
			    << (sameLine ? " " : nl);
	}

	void saveXmlDate(QTextStream &out, const QString &nl, const char *name, const QDateTime &value)
	{
		if (value.isValid())
			saveXmlString(out, nl, name, value.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
	}

	void saveXmlDouble(QTextStream &out, const QString &nl, const char *name, double value)
	{
		QString number = QString::asprintf("%.2f", value);
		number.replace(QLatin1Char(','), QLatin1Char('.'));
		saveXmlString(out, nl, name, number, true);
	}

	void saveXmlColour(QTextStream &out, const QString &nl, const char *name, long colour,
	                   bool sameLine = false)
	{
		saveXmlString(out, nl, name, qmudColourToName(colour), sameLine);
	}

	void saveXmlMulti(QTextStream &out, const QString &nl, const char *name, const QString &value)
	{
		if (!value.isEmpty())
			out << "  <" << name << ">" << fixHtmlMultilineString(value) << "</" << name << ">" << nl;
	}

	constexpr int OPT_MULTLINE      = 0x000001;
	constexpr int OPT_PASSWORD      = 0x000004;
	constexpr int OPT_KEEP_SPACES   = 0x000002;
	constexpr int OPT_COMMAND_STACK = 0x000008;
	constexpr int OPT_WORLD_ID      = 0x000010;

	struct WorldNumericSaveOption
	{
			constexpr WorldNumericSaveOption(const char *name, const long long defaultValue)
			    : name(name), defaultValue(defaultValue), min(0), max(0), flags(0)
			{
			}

			constexpr WorldNumericSaveOption(const char *name, const long long defaultValue,
			                                 const long long min, const long long max)
			    : name(name), defaultValue(defaultValue), min(min), max(max), flags(0)
			{
			}

			constexpr WorldNumericSaveOption(const char *name, const long long defaultValue,
			                                 const long long min, const long long max, const int flags)
			    : name(name), defaultValue(defaultValue), min(min), max(max), flags(flags)
			{
			}

			const char *name;
			long long   defaultValue;
			long long   min;
			long long   max;
			int         flags;
	};

	struct WorldAlphaSaveOption
	{
			constexpr WorldAlphaSaveOption(const char *name, const char *) : name(name), flags(0)
			{
			}

			constexpr WorldAlphaSaveOption(const char *name, const char *, int flags)
			    : name(name), flags(flags)
			{
			}

			const char *name;
			int         flags;
	};
	constexpr char kNoSoundLiteral[] = "(No sound)";

#ifdef RGB
#undef RGB
#endif
	constexpr long long RGB(const int red, const int green, const int blue)
	{
		return qmudRgb(red, green, blue);
	}
	const WorldNumericSaveOption kNumericSaveOptions[] = {
#define QMUD_INCLUDE_WORLD_OPTION_DEFAULTS_NUMERIC_ROWS
#include "WorldOptionDefaultsNumeric.inc"

#undef QMUD_INCLUDE_WORLD_OPTION_DEFAULTS_NUMERIC_ROWS

	};

	const WorldAlphaSaveOption kAlphaSaveOptions[] = {
#define QMUD_INCLUDE_WORLD_OPTION_DEFAULTS_ALPHA_ROWS
#include "WorldOptionDefaultsAlpha.inc"

#undef QMUD_INCLUDE_WORLD_OPTION_DEFAULTS_ALPHA_ROWS

	};

	QString resolveWorkingDir(const QString &startupDir)
	{
		if (!startupDir.isEmpty())
			return startupDir;
		QString dir = QCoreApplication::applicationDirPath();
		if (!dir.endsWith('/'))
			dir += '/';
		return dir;
	}

#ifndef Q_OS_WIN
	QString normalizeSeparators(const QString &input)
	{
		QString output = input;
		output.replace(QLatin1Char('\\'), QLatin1Char('/'));
		return output;
	}
#endif

	QString appendNumberBeforeExtension(const QString &filePath, int number)
	{
		const QFileInfo info(filePath);
		const QString   dir    = info.path();
		const QString   suffix = info.completeSuffix();
		QString         stem   = info.completeBaseName();
		if (stem.isEmpty())
			stem = info.fileName();
		QString numbered = QStringLiteral("%1-%2").arg(stem).arg(number);
		if (!suffix.isEmpty())
			numbered += QStringLiteral(".%1").arg(suffix);
		if (dir.isEmpty() || dir == QStringLiteral("."))
			return numbered;
		return QDir(dir).filePath(numbered);
	}

	QString makeAbsolutePath(const QString &fileName, const QString &workingDir)
	{
		if (fileName.isEmpty())
			return fileName;

		QString normalized;
#ifdef Q_OS_WIN
		normalized = QDir::fromNativeSeparators(fileName);
#else
		normalized = normalizeSeparators(fileName);
#endif
		const bool hadTrailingSeparator =
		    normalized.endsWith(QLatin1Char('/')) || normalized.endsWith(QLatin1Char('\\'));
		const QChar first      = normalized.at(0);
		const bool  isDrive    = normalized.size() > 1 && normalized.at(1) == QChar(':') && first.isLetter();
		const bool  isAbsolute = isDrive || first == QChar('\\') || first == QChar('/');

		if (!isAbsolute)
		{
			QString relative = normalized;
			if (relative.startsWith(QStringLiteral("./")) || relative.startsWith(QStringLiteral(".\\")))
				relative = relative.mid(2);
			QString resolved = QDir(workingDir).filePath(relative);
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

	QString canonicalPortableRootName(const QString &segment)
	{
		static constexpr std::array<const char *, 8> kPortableRoots = {"worlds", "lua",   "logs",   "plugins",
		                                                               "sounds", "state", "backup", "docs"};
		for (const char *root : kPortableRoots)
		{
			if (segment.compare(QLatin1String(root), Qt::CaseInsensitive) == 0)
				return QString::fromLatin1(root);
		}
		return {};
	}

	QString collapseLeadingDuplicatePortableRoot(const QString &relativePath)
	{
		QString     cleaned  = QDir::cleanPath(relativePath);
		QStringList segments = cleaned.split(QLatin1Char('/'), Qt::SkipEmptyParts);
		if (segments.size() < 2)
			return cleaned;

		const QString root = canonicalPortableRootName(segments.at(0));
		if (root.isEmpty())
			return cleaned;

		// Guard against accidentally duplicated portable-root prefixes such as
		// "worlds/worlds/..." or "worlds/plugins/worlds/plugins/..."
		// introduced by legacy path rewrites.
		auto prefixesMatch = [&segments](const int startA, const int startB, const int count) -> bool
		{
			for (int i = 0; i < count; ++i)
			{
				if (segments.at(startA + i).compare(segments.at(startB + i), Qt::CaseInsensitive) != 0)
					return false;
			}
			return true;
		};

		bool collapsed = true;
		while (collapsed)
		{
			collapsed              = false;
			const int maxPrefixLen = safeQSizeToInt(segments.size() / 2);
			for (int prefixLen = maxPrefixLen; prefixLen >= 1; --prefixLen)
			{
				if (!prefixesMatch(0, prefixLen, prefixLen))
					continue;
				for (int removeIndex = 0; removeIndex < prefixLen; ++removeIndex)
					segments.removeAt(prefixLen);
				collapsed = true;
				break;
			}
		}
		return QDir::cleanPath(segments.join(QLatin1Char('/')));
	}

	QString extractPortableRootRelativePath(const QString &path)
	{
		QString normalized = normalizePathForStorage(path.trimmed());
		if (normalized.isEmpty())
			return {};
		if (normalized.startsWith(QLatin1Char('/')) && normalized.size() > 3 && normalized.at(1).isLetter() &&
		    normalized.at(2) == QLatin1Char(':'))
		{
			normalized.remove(0, 1);
		}

		const QStringList segments = normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts);
		if (segments.isEmpty())
			return {};

		for (qsizetype i = 0; i < segments.size(); ++i)
		{
			const QString root = canonicalPortableRootName(segments.at(i));
			if (root.isEmpty())
				continue;
			QStringList relativeSegments;
			relativeSegments.reserve(segments.size() - i);
			relativeSegments.push_back(root);
			for (qsizetype j = i + 1; j < segments.size(); ++j)
				relativeSegments.push_back(segments.at(j));
			return collapseLeadingDuplicatePortableRoot(relativeSegments.join(QLatin1Char('/')));
		}
		return {};
	}

	bool hasUrlScheme(const QString &value)
	{
		const QUrl url(value);
		return url.isValid() && !url.scheme().isEmpty() && value.contains(QStringLiteral("://"));
	}

	QString toDotRelativePath(const QString &relativePath)
	{
		QString cleaned = normalizePathForStorage(relativePath.trimmed());
		if (cleaned.isEmpty())
			return {};
		cleaned = QDir::cleanPath(cleaned);
		while (cleaned.startsWith(QStringLiteral("./")))
			cleaned.remove(0, 2);
		if (cleaned.isEmpty() || cleaned == QStringLiteral("."))
			return QStringLiteral("./");
		if (cleaned.startsWith(QStringLiteral("../")))
			return cleaned;
		return QStringLiteral("./") + cleaned;
	}

	QString canonicalizePathForStorage(const QString &path, const QString &workingDir)
	{
		QString normalized = normalizePathForStorage(path.trimmed());
		if (normalized.isEmpty() || hasUrlScheme(normalized))
			return normalized;

		const bool trailingSlash = normalized.endsWith(QLatin1Char('/'));
		QString relative = QMudPluginPathUtils::qmudHomeRelativePath(workingDir, normalized, trailingSlash);
		if (relative.isEmpty())
			return {};
		relative = toDotRelativePath(relative);
		if (trailingSlash && !relative.endsWith(QLatin1Char('/')))
			relative += QLatin1Char('/');
		return relative;
	}

	QString canonicalizePathForRuntime(const QString &path, const QString &workingDir)
	{
		return normalizePathForRuntime(canonicalizePathForStorage(path, workingDir));
	}

	void normalizePathAttributeMapForStorage(QMap<QString, QString> &attributes, const QString &workingDir)
	{
		for (auto it = attributes.begin(); it != attributes.end(); ++it)
		{
			if (!isCanonicalizablePathAttributeName(it.key()))
				continue;
			if (it.value().trimmed() == QString::fromLatin1(kNoSoundLiteral))
				continue;
			it.value() = canonicalizePathForStorage(it.value(), workingDir);
			if (it.key() == QStringLiteral("auto_log_file_name"))
				it.value() = normalizeAutoLogFileNameValue(it.value());
		}
	}

	bool patternUsesPlayerDirective(const QString &pattern)
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
	}

	bool isLuaScriptingEnabled(const QMap<QString, QString> &attrs)
	{
		if (!isEnabledFlag(attrs.value(QStringLiteral("enable_scripts"))))
			return false;
		const QString language = attrs.value(QStringLiteral("script_language"));
		return language.compare(QStringLiteral("Lua"), Qt::CaseInsensitive) == 0;
	}

	QSharedPointer<LuaCallbackEngine> makeNonOwningLuaEngineRef(LuaCallbackEngine *engine)
	{
		return {engine, [](LuaCallbackEngine * /*unused*/) {}};
	}
} // namespace

WorldRuntime::WorldRuntime(QObject *parent) : QObject(parent)
{
	m_luaExecutor  = makeLuaExecutor();
	m_luaCallbacks = new LuaCallbackEngine();
	{
		LuaEngineObservedInitializationRequest request;
		request.engine     = m_luaCallbacks;
		request.runtime    = this;
		request.scriptText = QString();
		dispatchInitializeLuaEnginesWithObservedCallbacks({request}, true);
	}
	m_socket         = new WorldSocket(this);
	m_statusTime     = QDateTime::currentDateTime();
	m_lastFlushTime  = QDateTime::currentDateTime();
	m_worldStartTime = QDateTime::currentDateTime();
	m_lineTimer.start();
	m_nextLineNumber = 1;
	m_soundBuffers.resize(kMaxSoundBuffers);
	m_scriptWatcher = new QFileSystemWatcher(this);
	connect(m_scriptWatcher, &QFileSystemWatcher::fileChanged, this,
	        [this](const QString &)
	        {
		        m_scriptFileChanged = true;
		        emit scriptFileChangedDetected();
		        if (m_scriptWatcher)
		        {
			        const QString scriptFile = m_worldAttributes.value(QStringLiteral("script_filename"));
			        if (!scriptFile.isEmpty())
			        {
				        const QString workingDir = resolveWorkingDir(m_startupDirectory);
				        const QString path       = makeAbsolutePath(scriptFile, workingDir);
				        if (!path.isEmpty() && !m_scriptWatcher->files().contains(path))
					        m_scriptWatcher->addPath(path);
			        }
		        }
	        });
	TelnetProcessor::Callbacks callbacks;
	callbacks.onTelnetRequest = [this](int number, const QString &type)
	{ return callPluginCallbacksStopOnTrue(QStringLiteral("OnPluginTelnetRequest"), number, type); };
	callbacks.onTelnetSubnegotiation = [](int number, const QByteArray &data)
	{
		Q_UNUSED(number);
		Q_UNUSED(data);
	};
	callbacks.onTelnetOption = [](const QByteArray &data) { Q_UNUSED(data); };
	callbacks.onIacGa        = [] {};
	callbacks.onMxpStart     = [this](bool pueblo, bool manual)
	{
		Q_UNUSED(manual);
		mxpStartUp();
		if (pueblo)
			mxpError(DBG_INFO, infoMXP_on, QStringLiteral("Pueblo turned on."));
		else
			mxpError(DBG_INFO, infoMXP_on, QStringLiteral("MXP turned on."));
	};
	callbacks.onMxpReset = [this]
	{
		if (m_mxpActive)
		{
			m_mxpTagStack.clear();
			m_mxpOpenTags.clear();
			m_mxpTextBuffer.clear();
			resetMxpRenderState();
			clearAnsiActionContext();
		}
		mxpError(DBG_INFO, infoMXP_ResetReceived, QStringLiteral("MXP reset."));
	};
	callbacks.onMxpStop = [this](bool completely, bool puebloActive)
	{
		if (!completely)
			return;
		mxpShutDown();
		if (puebloActive)
			mxpError(DBG_INFO, infoMXP_off, QStringLiteral("Pueblo turned off."));
		else
			mxpError(DBG_INFO, infoMXP_off, QStringLiteral("MXP turned off."));
	};
	callbacks.onMxpModeChange = [this](int oldMode, int newMode, bool shouldLog)
	{
		// Defensive state barrier: when MXP transitions to a locked/reset mode,
		// drop open render/tag state so secure link/style spans cannot leak into
		// subsequent plain text.
		if (isMxpResetModeCode(newMode) || isMxpLockedModeCode(newMode))
		{
			m_mxpTagStack.clear();
			m_mxpOpenTags.clear();
			m_mxpTextBuffer.clear();
			resetMxpRenderState();
			clearAnsiActionContext();
		}

		if (!shouldLog)
			return;
		auto modeLabel = [](int mode) -> QString
		{
			switch (mode)
			{
			case 0:
				return QStringLiteral("open");
			case 1:
				return QStringLiteral("secure");
			case 2:
				return QStringLiteral("locked");
			case 3:
				return QStringLiteral("reset");
			case 4:
				return QStringLiteral("secure next tag only");
			case 5:
				return QStringLiteral("permanently open");
			case 6:
				return QStringLiteral("permanently secure");
			case 7:
				return QStringLiteral("permanently locked");
			default:
				break;
			}
			return QStringLiteral("unknown mode %1").arg(mode);
		};
		const QString from = modeLabel(oldMode);
		const QString to   = modeLabel(newMode);
		mxpError(DBG_INFO, infoMXP_ModeChange,
		         QStringLiteral("MXP mode change from '%1' to '%2'").arg(from, to));
	};
	callbacks.onMxpDiagnosticNeeded = [this](int level)
	{
		if (isLuaScriptingEnabled(m_worldAttributes) && m_luaCallbacks &&
		    !m_worldAttributes.value(QStringLiteral("on_mxp_error")).trimmed().isEmpty())
		{
			return true;
		}
		return level <= currentMxpDebugLevel(m_worldAttributes);
	};
	callbacks.onMxpDiagnostic = [this](int level, long messageNumber, const QString &message)
	{ mxpError(level, messageNumber, message); };
	callbacks.onNoEchoChanged      = [this](bool enabled) { setNoCommandEcho(enabled); };
	callbacks.onFatalProtocolError = [this](const QString &message)
	{
#ifndef NDEBUG
		const QString worldName = m_worldAttributes.value(QStringLiteral("name")).trimmed();
		qWarning().noquote() << QStringLiteral("[QMud][MCCP] fatal protocol error in world \"%1\": %2")
		                            .arg(worldName.isEmpty() ? QStringLiteral("<unnamed>") : worldName,
		                                 message.isEmpty()
		                                     ? QStringLiteral(
		                                           "Cannot process compressed output. World closed.")
		                                     : message);
#endif
		emit socketError(message.isEmpty() ? QStringLiteral("Cannot process compressed output. World closed.")
		                                   : message);
		disconnectFromWorld();
	};
	m_telnet.setCallbacks(callbacks);
	connect(m_socket, &WorldSocketService::rawDataReceived, this, &WorldRuntime::receiveRawData);
	connect(m_socket, &WorldSocketService::socketError, this,
	        [this](const QString &message)
	        {
		        // Preserve behavior: intentional disconnects should not
		        // surface connection-failure messages through UI/Lua paths.
		        if (m_disconnectOk || m_connectPhase == eConnectDisconnecting)
			        return;
		        emit socketError(message);
	        });
	connect(m_socket, &WorldSocketService::socketStateChanged, this,
	        [this](QAbstractSocket::SocketState state)
	        {
		        switch (state)
		        {
		        case QAbstractSocket::HostLookupState:
			        m_connectPhase = m_connectViaProxy ? eConnectProxyNameLookup : eConnectMudNameLookup;
			        break;
		        case QAbstractSocket::ConnectingState:
			        m_connectPhase = m_connectViaProxy ? eConnectConnectingToProxy : eConnectConnectingToMud;
			        break;
		        case QAbstractSocket::UnconnectedState:
			        if (m_connectPhase != eConnectConnectedToMud && m_connectPhase != eConnectDisconnecting)
			        {
				        m_connectViaProxy = false;
				        m_connectPhase    = eConnectNotConnected;
			        }
			        break;
		        default:
			        break;
		        }
	        });
	connect(m_socket, &WorldSocketService::connected, this,
	        [this]
	        {
		        m_mxpActive = false;
		        m_mxpTagStack.clear();
		        m_mxpOpenTags.clear();
		        m_mxpTextBuffer.clear();
		        resetAnsiRenderState();
		        m_connectViaProxy = false;
		        m_hasCachedIp     = true;
		        m_connectTime     = QDateTime::currentDateTime();
		        if (!m_tlsEncryptionEnabled)
		        {
			        finalizeSocketConnectedState();
			        return;
		        }
		        if (m_tlsMethod == eTlsDirect)
		        {
			        finalizeSocketConnectedState();
			        return;
		        }
		        if (m_tlsMethod == eTlsStartTls)
		        {
			        m_telnet.queueStartTlsNegotiation();
			        if (const QByteArray outbound = m_telnet.takeOutboundData(); !outbound.isEmpty())
				        sendToWorld(outbound);
			        startStartTlsFallbackTimer();
			        return;
		        }
	        });
	connect(m_socket, &WorldSocketService::tlsEncrypted, this,
	        [this]
	        {
		        cancelStartTlsFallbackTimer();
		        if (m_tlsEncryptionEnabled && m_tlsMethod == eTlsStartTls)
			        m_telnet.setStartTlsActive(true);
		        finalizeSocketConnectedState();
	        });
	connect(m_socket, &WorldSocketService::disconnected, this,
	        [this]
	        {
		        cancelStartTlsFallbackTimer();
		        cancelReloadMccpProbeTimeout();
		        m_reloadReattachMccpProbePending        = false;
		        m_reloadReattachMccpResumePending       = false;
		        m_reloadReattachLookProbeSent           = false;
		        m_reloadReattachMccpProbeDecisionOffset = 0;
		        m_reloadMccpProbeTimeoutPass            = 0;
		        m_reloadReattachMccpProbeBuffer.clear();
		        m_reloadReattachUseDeferredMxpReplay = false;
		        m_reloadReattachMxpProbeEvents.clear();
		        m_reloadReattachMxpProbeModeChanges.clear();
		        const bool wasConnected = m_socketReadyForWorld || m_connectPhase == eConnectDisconnecting;
		        mxpShutDown();
		        m_connectViaProxy     = false;
		        m_connectPhase        = eConnectNotConnected;
		        m_socketReadyForWorld = false;
		        m_telnet.resetConnectionState();
		        resetAnsiRenderState();
		        if (wasConnected)
			        emit disconnected();
	        });
	QMudNativePluginRegistry::ensureMushReaderRuntimeSetup(this);
}

WorldRuntime::~WorldRuntime()
{
	QVector<QSharedPointer<LuaCallbackEngine>> enginesToTeardown;
	enginesToTeardown.reserve((m_luaCallbacks ? 1 : 0) + m_plugins.size());
	for (auto &plugin : m_plugins)
	{
		if (!plugin.lua)
			continue;
		enginesToTeardown.push_back(plugin.lua);
	}
	if (m_luaCallbacks)
		enginesToTeardown.push_back(makeNonOwningLuaEngineRef(m_luaCallbacks));

	cancelSuspendedPluginCallbackDispatchesForEngines(enginesToTeardown);

	for (auto &plugin : m_plugins)
	{
		if (plugin.lua && hasValidPluginId(plugin))
			dispatchSingleEngineNoArgCallback(plugin.lua, QStringLiteral("OnPluginClose"), true);
		savePluginStateForPlugin(plugin, false, nullptr);
	}
	m_pluginCallbackDispatchShuttingDown = true;

	if (m_viewDestroyedConnection)
	{
		QObject::disconnect(m_viewDestroyedConnection);
		m_viewDestroyedConnection = QMetaObject::Connection{};
	}
	m_view = nullptr;

	for (auto &plugin : m_plugins)
		plugin.lua.clear();
	dispatchTeardownLuaEngines(enginesToTeardown, true);
	if (m_luaCallbacks)
	{
		delete m_luaCallbacks;
		m_luaCallbacks = nullptr;
	}
	QMudNativePluginRegistry::discardRuntimeState(this);

	const QStringList dbNames = m_databases.keys();
	for (const QString &name : dbNames)
		databaseClose(name);

#if QMUD_ENABLE_SOUND
	for (SoundBuffer &buffer : m_soundBuffers)
		clearSoundBuffer(buffer);
#endif

	for (int const fontId : std::as_const(m_specialFontIds))
		QFontDatabase::removeApplicationFont(fontId);
	m_specialFontIds.clear();
	m_specialFontPaths.clear();
	m_specialFontPathOrder.clear();

	for (UdpListener &listener : m_udpListeners)
	{
		if (listener.socket)
		{
			listener.socket->close();
			delete listener.socket;
			listener.socket = nullptr;
		}
	}
	m_udpListeners.clear();
}

void WorldRuntime::addScriptTime(qint64 nanos)
{
	if (nanos <= 0)
		return;
	m_scriptTimeNanos.fetch_add(nanos, std::memory_order_relaxed);
}

double WorldRuntime::scriptTimeSeconds() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), 0.0,
		                          [this] { return scriptTimeSeconds(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::scriptTimeSeconds");
	return static_cast<double>(m_scriptTimeNanos.load(std::memory_order_relaxed)) / 1000000000.0;
}

void WorldRuntime::resetAnsiRenderState()
{
	m_ansiStreamState = QMudAnsiStreamState{};
	m_partialLineText.clear();
	m_partialLineSpans.clear();
	m_pendingCarriageReturnOverwrite = false;
	++m_incomingPartialLineRevision;
	m_ansiRenderState = AnsiRenderState{};
	m_streamUtf8Carry.clear();
	m_streamUtf8DecoderEnabled = false;
	m_streamLegacyEncodingName.clear();
	m_streamLegacyDecoder = QStringDecoder();
	resetMxpRenderState();
}

void WorldRuntime::resetMxpRenderState()
{
	m_mxpRenderStyle = MxpStyleState{};
	m_mxpRenderStack.clear();
	m_mxpRenderBlockStack.clear();
	m_mxpRenderLinkOpen = false;
	m_mxpRenderPreDepth = 0;
}

void WorldRuntime::clearAnsiActionContext()
{
	m_ansiRenderState.actionType = ActionNone;
	m_ansiRenderState.action.clear();
	m_ansiRenderState.hint.clear();
	m_ansiRenderState.variable.clear();
	m_ansiRenderState.startTag = false;
}

void WorldRuntime::receiveRawData(const QByteArray &data)
{
	if (m_incomingSocketDataPaused)
		return;

	if (data.isEmpty())
		return;

	m_pendingRawIngressPayloads.enqueue({data, m_doingSimulate});
	if (m_rawIngressProcessing)
		return;

	beginOutputViewMutationBatch();
	const auto closeOutputViewBatch      = qScopeGuard([this] { endOutputViewMutationBatch(); });
	m_rawIngressProcessing               = true;
	const auto closeRawIngressProcessing = qScopeGuard([this] { m_rawIngressProcessing = false; });
	while (!m_pendingRawIngressPayloads.isEmpty())
	{
		const RawIngressPayload payload                = m_pendingRawIngressPayloads.dequeue();
		const bool              previousSimulatedInput = m_doingSimulate;
		m_doingSimulate                                = payload.simulatedInput;
		processRawDataPayload(payload.data, payload.simulatedInput);
		m_doingSimulate = previousSimulatedInput;
	}
}

void WorldRuntime::beginInboundCommandDeferralScope() const
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::beginInboundCommandDeferralScope");
	++m_inboundCommandDeferralDepth;
}

void WorldRuntime::endInboundCommandDeferralScope() const
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::endInboundCommandDeferralScope");
	if (m_inboundCommandDeferralDepth <= 0)
		return;
	--m_inboundCommandDeferralDepth;
	if (m_inboundCommandDeferralDepth == 0)
		flushDeferredInboundCommands();
}

void WorldRuntime::flushDeferredInboundCommands() const
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::flushDeferredInboundCommands");
	if (m_flushingDeferredInboundCommands || m_deferredInboundCommands.isEmpty())
		return;

	const QVector<DeferredInboundCommand> commands = m_deferredInboundCommands;
	m_deferredInboundCommands.clear();
	m_deferredInboundTriggerPriorityCommands = 0;

	m_flushingDeferredInboundCommands = true;
	const auto restoreFlushing        = qScopeGuard([this] { m_flushingDeferredInboundCommands = false; });
	Q_UNUSED(restoreFlushing);
	for (const DeferredInboundCommand &command : commands)
	{
		if (!m_commandProcessor)
			break;
		const unsigned short previousActionSource               = m_currentActionSource;
		const_cast<WorldRuntime *>(this)->m_currentActionSource = command.actionSource;
		const auto restoreActionSource =
		    qScopeGuard([this, previousActionSource]
		                { const_cast<WorldRuntime *>(this)->m_currentActionSource = previousActionSource; });
		Q_UNUSED(restoreActionSource);
		m_commandProcessor->sendDeferredInboundText(command.text, command.echo, command.log);
	}
}

void WorldRuntime::processRawDataPayload(const QByteArray &data, const bool simulatedInput)
{
	if (!simulatedInput)
	{
		if (AppController const *app = AppController::instance())
		{
			if (MainWindow *main = app->mainWindow())
				main->checkTimerFallback();
		}
	}

	const unsigned short previousActionSource = m_currentActionSource;
	m_currentActionSource                     = eInputFromServer;
	beginInboundCommandDeferralScope();
	const auto    closeInboundCommandDeferral = qScopeGuard([this] { endInboundCommandDeferralScope(); });

	const QString utf8Flag = m_worldAttributes.value(QStringLiteral("utf_8"));
	const bool    useUtf8  = isEnabledFlag(utf8Flag);
	const QString legacyEncodingName =
	    qmudNormalizeWorldTextEncodingName(m_worldAttributes.value(QStringLiteral("legacy_encoding")));
	const QString gaFlag    = m_worldAttributes.value(QStringLiteral("convert_ga_to_newline"));
	const bool    convertGA = isEnabledFlag(gaFlag);
	const bool    carriageReturnClears =
	    isEnabledFlag(m_worldAttributes.value(QStringLiteral("carriage_return_clears_line")));
	const QString noEchoOffFlag          = m_worldAttributes.value(QStringLiteral("no_echo_off"));
	const bool    noEchoOff              = isEnabledFlag(noEchoOffFlag);
	const QString disableCompressionFlag = m_worldAttributes.value(QStringLiteral("disable_compression"));
	const bool    disableCompression     = isEnabledFlag(disableCompressionFlag);
	const bool    negotiateOptionsOnce =
	    isEnabledFlag(m_worldAttributes.value(QStringLiteral("only_negotiate_telnet_options_once")));
	const int     useMxp     = m_worldAttributes.value(QStringLiteral("use_mxp")).toInt();
	const QString terminalId = m_worldAttributes.value(QStringLiteral("terminal_identification"));

	m_telnet.setUseUtf8(useUtf8);
	m_telnet.setLegacyEncodingName(legacyEncodingName);
	m_telnet.setPreferredCharsetNames(qmudWorldTextTelnetCharsetNames(useUtf8, legacyEncodingName));
	m_telnet.setConvertGAtoNewline(convertGA);
	m_telnet.setNoEchoOff(noEchoOff);
	m_telnet.setDisableCompression(disableCompression);
	m_telnet.setNegotiateOptionsOnce(negotiateOptionsOnce);
	if (useMxp >= 0)
		m_telnet.setUseMxp(useMxp);
	updateTelnetWindowSizeForNaws();
	if (!terminalId.isEmpty())
		m_telnet.setTerminalIdentification(terminalId);

	if (!simulatedInput && m_reloadReattachMccpProbePending)
	{
		// Probe quarantine mode: keep telnet state/protocol negotiation current,
		// but defer all output/plugin processing until keep/reconnect decision.
		QByteArray processed = m_telnet.processBytes(data);
		static_cast<void>(m_telnet.takeTelnetPluginEvents());
		QList<TelnetProcessor::MxpEvent>      mxpEventsDuringProbe      = m_telnet.takeMxpEvents();
		QList<TelnetProcessor::MxpModeChange> mxpModeChangesDuringProbe = m_telnet.takeMxpModeChanges();
		QByteArray const                      outbound                  = m_telnet.takeOutboundData();
		if (!outbound.isEmpty())
			sendToWorld(outbound);
		if (m_tlsEncryptionEnabled && m_tlsMethod == eTlsStartTls &&
		    m_telnet.takeStartTlsNegotiationRejected() && !m_socketReadyForWorld)
		{
			cancelStartTlsFallbackTimer();
			outputText(QStringLiteral("TLS Encryption NOT enabled: Server denied."), true, true);
			m_telnet.setStartTlsEnabled(false);
			finalizeSocketConnectedState();
		}
		if (m_telnet.takeStartTlsUpgradeRequest())
		{
			cancelStartTlsFallbackTimer();
			if (!beginStartTlsUpgrade())
			{
				m_currentActionSource = previousActionSource;
				return;
			}
		}
		if (!mxpEventsDuringProbe.isEmpty() || !mxpModeChangesDuringProbe.isEmpty())
		{
			const qsizetype baseOffset = m_reloadReattachMccpProbeBuffer.size();
			for (TelnetProcessor::MxpEvent &ev : mxpEventsDuringProbe)
				ev.offset = safeQSizeToInt(baseOffset + static_cast<qsizetype>(qMax(ev.offset, 0)));
			for (TelnetProcessor::MxpModeChange &change : mxpModeChangesDuringProbe)
				change.offset = safeQSizeToInt(baseOffset + static_cast<qsizetype>(qMax(change.offset, 0)));
			m_reloadReattachMxpProbeEvents.append(mxpEventsDuringProbe);
			m_reloadReattachMxpProbeModeChanges.append(mxpModeChangesDuringProbe);
			m_reloadReattachUseDeferredMxpReplay = true;
		}
		const ReloadMccpProbeIngressInput ingressInput{
		    simulatedInput,
		    m_reloadReattachMccpProbePending,
		    data.size(),
		    processed,
		};
		(void)ingestReloadMccpProbeChunk(ingressInput, &m_bytesIn, &m_inputPacketCount,
		                                 &m_reloadReattachMccpProbeBuffer);
		m_currentActionSource = previousActionSource;
		return;
	}

	if (!simulatedInput)
	{
		m_bytesIn += data.size();
		m_inputPacketCount++;
	}
	if (m_debugIncomingPackets)
	{
		auto appendPacketDebug = [this](const QString &caption, const QByteArray &packet, qint64 number)
		{
			const QString   worldName = m_worldAttributes.value(QStringLiteral("name"));
			const QString   title     = QStringLiteral("Packet debug - %1")
			                                .arg(worldName.isEmpty() ? QStringLiteral("World") : worldName);
			const QDateTime now       = QDateTime::currentDateTime();
			const QString   timestamp =
			    QLocale::system().toString(now, QStringLiteral("dddd, MMMM dd, yyyy, h:mm:ss AP"));
			QString const header = QStringLiteral("\r\n%1 packet: %2 (%3 bytes) at %4\r\n\r\n")
			                           .arg(caption)
			                           .arg(number)
			                           .arg(packet.size())
			                           .arg(timestamp);
			if (!callPluginCallbacksStopOnTrueWithString(QStringLiteral("OnPluginPacketDebug"), header))
			{
				if (AppController const *app = AppController::instance())
					if (MainWindow *main = app->mainWindow())
						main->appendToNotepad(title, header, false, this);
			}

			int remaining = safeQSizeToInt(packet.size());
			int offset    = 0;
			while (remaining > 0)
			{
				const int count = qMin(remaining, kPacketDebugChars);
				QString   ascii;
				QString   hex;
				ascii.reserve(kPacketDebugChars);
				hex.reserve(kPacketDebugChars * 3);
				for (int i = 0; i < count; ++i)
				{
					const auto byte = static_cast<unsigned char>(packet.at(offset + i));
					ascii += (isAsciiPrintableByte(byte) ? QChar(byte) : QChar('.'));
					hex += QString::asprintf(" %02x", byte);
				}
				const QString line = QStringLiteral("%1%2%3\r\n")
				                         .arg(ascii.leftJustified(kPacketDebugChars, QLatin1Char(' ')))
				                         .arg(QLatin1Char(' '))
				                         .arg(hex);
				if (!callPluginCallbacksStopOnTrueWithString(QStringLiteral("OnPluginPacketDebug"), line))
				{
					if (AppController const *app = AppController::instance())
						if (MainWindow *main = app->mainWindow())
							main->appendToNotepad(title, line, false, this);
				}
				remaining -= count;
				offset += count;
			}
		};
		appendPacketDebug(QStringLiteral("Incoming"), data, m_inputPacketCount);
	}

	QByteArray                                processed;
	QList<TelnetProcessor::TelnetPluginEvent> telnetPluginEvents;
	auto rebaseTelnetEventOffsetsForRemoval = [&](const qsizetype startOffset, const qsizetype removedSize)
	{
		if (removedSize <= 0)
			return;
		const int start = safeQSizeToInt(startOffset);
		const int count = safeQSizeToInt(removedSize);
		for (TelnetProcessor::TelnetPluginEvent &event : telnetPluginEvents)
		{
			if (event.offset <= start)
				continue;
			const int removedBeforeEvent = qMin(count, event.offset - start);
			event.offset -= removedBeforeEvent;
		}
	};
	auto rebaseTelnetEventOffsetsForFilter = [&](const QByteArray &bytes, const auto shouldRemove)
	{
		if (telnetPluginEvents.isEmpty())
			return;
		QVector<int> removedBefore;
		removedBefore.reserve(safeQSizeToInt(bytes.size()) + 1);
		int removed = 0;
		removedBefore.push_back(0);
		for (const char ch : bytes)
		{
			if (shouldRemove(ch))
				++removed;
			removedBefore.push_back(removed);
		}
		const int originalSize = safeQSizeToInt(bytes.size());
		for (TelnetProcessor::TelnetPluginEvent &event : telnetPluginEvents)
		{
			const int originalOffset = qBound(0, event.offset, originalSize);
			event.offset             = originalOffset - removedBefore.at(originalOffset);
		}
	};
	if (simulatedInput)
	{
		processed = data;
		callPluginCallbacksTransformBytes(QStringLiteral("OnPluginPacketReceived"), processed);
	}
	else
	{
		// Packet callbacks transform each decompressed incoming payload before telnet/MXP
		// parsing; parser event offsets are then based on the same bytes that render.
		processed = m_telnet.processBytes(
		    data, [this](QByteArray &packetPayload)
		    { callPluginCallbacksTransformBytes(QStringLiteral("OnPluginPacketReceived"), packetPayload); });
		telnetPluginEvents = m_telnet.takeTelnetPluginEvents();
	}
	if (processed.contains('\0'))
	{
		const QByteArray beforeFilter = processed;
		QByteArray       sanitized;
		sanitized.reserve(processed.size());
		for (const char ch : processed)
		{
			if (ch != '\0')
				sanitized.append(ch);
		}
		rebaseTelnetEventOffsetsForFilter(beforeFilter, [](const char ch) { return ch == '\0'; });
		processed.swap(sanitized);
	}

	// Port of the ReceiveMsg/DisplayMsg line handling:
	// - pass callback-transformed data through telnet/MCCP phase processing
	// - accumulate raw bytes
	// - split on LF
	// - trim CR
	if (const bool detectPueblo = isEnabledFlag(m_worldAttributes.value(QStringLiteral("detect_pueblo")));
	    detectPueblo && !m_mxpActive && !m_telnet.isPuebloActive())
	{
		static const auto kPuebloStart = QByteArrayLiteral("</xch_mudtext>");
		if (const qsizetype marker = processed.indexOf(kPuebloStart); marker >= 0)
		{
			m_telnet.activatePuebloMode();
			rebaseTelnetEventOffsetsForRemoval(marker, kPuebloStart.size());
			processed.remove(marker, kPuebloStart.size());
			if (marker < processed.size())
			{
				if (marker + 1 < processed.size() && processed.at(marker) == '\r' &&
				    processed.at(marker + 1) == '\n')
				{
					rebaseTelnetEventOffsetsForRemoval(marker, 2);
					processed.remove(marker, 2);
				}
				else if (processed.at(marker) == '\n')
				{
					rebaseTelnetEventOffsetsForRemoval(marker, 1);
					processed.remove(marker, 1);
				}
			}
		}
	}
	int bellCount = 0;
	for (char const ch : processed)
	{
		if (static_cast<unsigned char>(ch) == 0x07)
			++bellCount;
	}
	if (bellCount > 0)
	{
		const QByteArray beforeFilter = processed;
		QByteArray       filtered;
		filtered.reserve(processed.size());
		for (char const ch : processed)
		{
			if (static_cast<unsigned char>(ch) != 0x07)
				filtered.append(ch);
		}
		rebaseTelnetEventOffsetsForFilter(beforeFilter, [](const char ch)
		                                  { return static_cast<unsigned char>(ch) == 0x07; });
		processed = filtered;

		if (const bool enableBeeps = isEnabledFlag(m_worldAttributes.value(QStringLiteral("enable_beeps")));
		    enableBeeps)
		{
			const QString beepSound = m_worldAttributes.value(QStringLiteral("beep_sound")).trimmed();
			if (!beepSound.isEmpty())
			{
				for (int i = 0; i < bellCount; ++i)
					playSound(0, beepSound, false, 0.0, 0.0);
			}
			else
			{
				for (int i = 0; i < bellCount; ++i)
					QApplication::beep();
			}
		}
	}
	QByteArray const outbound = m_telnet.takeOutboundData();
	if (!outbound.isEmpty())
		sendToWorld(outbound);
	if (m_tlsEncryptionEnabled && m_tlsMethod == eTlsStartTls && m_telnet.takeStartTlsNegotiationRejected() &&
	    !m_socketReadyForWorld)
	{
		cancelStartTlsFallbackTimer();
		outputText(QStringLiteral("TLS Encryption NOT enabled: Server denied."), true, true);
		m_telnet.setStartTlsEnabled(false);
		finalizeSocketConnectedState();
	}
	if (m_telnet.takeStartTlsUpgradeRequest())
	{
		cancelStartTlsFallbackTimer();
		if (!beginStartTlsUpgrade())
		{
			m_currentActionSource = previousActionSource;
			return;
		}
	}

	QList<TelnetProcessor::MxpEvent>      events;
	QList<TelnetProcessor::MxpModeChange> modeChanges;
	if (simulatedInput && m_reloadReattachUseDeferredMxpReplay)
	{
		events.swap(m_reloadReattachMxpProbeEvents);
		modeChanges.swap(m_reloadReattachMxpProbeModeChanges);
		m_reloadReattachUseDeferredMxpReplay = false;
	}
	else
	{
		events      = m_telnet.takeMxpEvents();
		modeChanges = m_telnet.takeMxpModeChanges();
	}

	std::ranges::sort(
	    telnetPluginEvents,
	    [](const TelnetProcessor::TelnetPluginEvent &a, const TelnetProcessor::TelnetPluginEvent &b)
	    {
		    if (a.offset != b.offset)
			    return a.offset < b.offset;
		    return a.sequence < b.sequence;
	    });
	auto dispatchTelnetPluginEvent = [this](const TelnetProcessor::TelnetPluginEvent &event)
	{
		switch (event.type)
		{
		case TelnetProcessor::TelnetPluginEvent::IacGa:
			m_lastLineWithIacGa = m_linesReceived;
			callPluginCallbacksNoArgs(QStringLiteral("OnPluginIacGa"), true);
			break;
		case TelnetProcessor::TelnetPluginEvent::Option:
			callPluginCallbacksWithBytes(QStringLiteral("OnPluginTelnetOption"), event.data, true);
			break;
		case TelnetProcessor::TelnetPluginEvent::Subnegotiation:
			m_lastTelnetSubnegotiation = QString::fromLatin1(event.data);
			callPluginCallbacksWithNumberAndBytes(QStringLiteral("OnPluginTelnetSubnegotiation"),
			                                      event.option, event.data, true);
			break;
		}
	};
	if (processed.isEmpty())
	{
		if (events.isEmpty() && modeChanges.isEmpty())
		{
			for (const TelnetProcessor::TelnetPluginEvent &event : std::as_const(telnetPluginEvents))
				dispatchTelnetPluginEvent(event);
			m_currentActionSource = previousActionSource;
			return;
		}
	}

	const bool utf8ModeChanged       = useUtf8 != m_streamUtf8DecoderEnabled;
	const bool legacyEncodingChanged = legacyEncodingName != m_streamLegacyEncodingName;
	if (utf8ModeChanged)
	{
		m_streamUtf8DecoderEnabled = useUtf8;
		m_streamUtf8Carry.clear();
	}
	if (legacyEncodingChanged || (utf8ModeChanged && !useUtf8))
	{
		m_streamLegacyEncodingName = legacyEncodingName;
		m_streamLegacyDecoder      = qmudCreateWorldTextDecoder(legacyEncodingName);
	}
	auto decodeIncomingDisplayBytes = [&](const QByteArrayView bytes) -> QString
	{
		if (bytes.isEmpty())
			return {};

		if (useUtf8)
		{
			bool          hadInvalidBytes = false;
			const QString decoded =
			    qmudDecodeUtf8WithWindows1252Fallback(bytes, m_streamUtf8Carry, &hadInvalidBytes);
			if (hadInvalidBytes)
				++m_utf8ErrorCount;
			return decoded;
		}

		bool          hadInvalidBytes = false;
		const QString decoded         = qmudDecodeWorldText(bytes, m_streamLegacyDecoder, &hadInvalidBytes);
		if (hadInvalidBytes)
			++m_utf8ErrorCount;
		return decoded;
	};

	auto decodeIncomingIsolatedBytes = [&](const QByteArray &bytes) -> QString
	{
		if (bytes.isEmpty())
			return {};

		if (useUtf8)
		{
			QStringDecoder decoder(QStringConverter::Utf8);
			const QString  decoded = decoder.decode(bytes);
			if (decoder.hasError())
				++m_utf8ErrorCount;
			return decoded;
		}

		bool          hadInvalidBytes = false;
		const QString decoded = qmudDecodeWorldTextIsolated(bytes, legacyEncodingName, &hadInvalidBytes);
		if (hadInvalidBytes)
			++m_utf8ErrorCount;
		return decoded;
	};

	auto resetMxpTrackingState = [this]
	{
		m_mxpTagStack.clear();
		m_mxpOpenTags.clear();
		m_mxpTextBuffer.clear();
		resetMxpRenderState();
		clearAnsiActionContext();
	};
	const bool hasActiveMxpParserContext = !m_mxpTagStack.isEmpty() || !m_mxpRenderStack.isEmpty() ||
	                                       !m_mxpRenderBlockStack.isEmpty() || m_mxpRenderLinkOpen;
	const bool keepMxpTextBuffer = !events.isEmpty() || !modeChanges.isEmpty() || hasActiveMxpParserContext;
	int        mxpBaseOffset     = 0;
	if (keepMxpTextBuffer)
	{
		mxpBaseOffset = safeQSizeToInt(m_mxpTextBuffer.size());
		m_mxpTextBuffer.append(processed);
		if (m_mxpTextBuffer.size() > kMaxMxpTextBufferBytes)
		{
			mxpError(DBG_WARNING, wrnMXP_TextBufferLimitExceeded,
			         QStringLiteral("MXP text buffer exceeded %1 bytes; resetting MXP context.")
			             .arg(kMaxMxpTextBufferBytes));
			resetMxpTrackingState();
			events.clear();
			mxpBaseOffset = 0;
		}
	}
	else
	{
		// No active MXP frames and no MXP events in this packet: avoid unbounded
		// growth from plain-text traffic.
		m_mxpTextBuffer.clear();
	}
	if (!events.isEmpty() || !modeChanges.isEmpty())
		mxpStartUp();
	const bool luaEnabled = isLuaScriptingEnabled(m_worldAttributes);

	const bool ignoreMxpColourChanges =
	    isEnabledFlag(m_worldAttributes.value(QStringLiteral("ignore_mxp_colour_changes")));
	const ResolvedOutputColourCache &outputColours               = resolvedOutputColourCache();
	const QVector<QColor>           &normalAnsi                  = outputColours.normalAnsi;
	const QVector<QColor>           &boldAnsi                    = outputColours.boldAnsi;
	const QString                   &defaultFore                 = outputColours.defaultFore;
	const QString                   &defaultBack                 = outputColours.defaultBack;
	quint64                          incomingPartialLineRevision = m_incomingPartialLineRevision;

	auto                             emitCompletedLine = [&](QString &lineText, QVector<StyleSpan> &lineSpans)
	{
		applyMappingFailureIfMatched(*this, lineText);
		if (!m_active)
			incrementNewLines();
		emit incomingStyledLineReceived(lineText, lineSpans);
		emit incomingLineReceived(lineText);
		m_linesReceived++;
		m_newlinesReceived++;
		lineText.clear();
		lineSpans.clear();
		++incomingPartialLineRevision;
	};

	const bool hasActiveMxpRenderContext =
	    !m_mxpRenderStack.isEmpty() || !m_mxpRenderBlockStack.isEmpty() || m_mxpRenderLinkOpen;

	if (events.isEmpty() && modeChanges.isEmpty() && !hasActiveMxpRenderContext)
	{
		AnsiRenderState current = m_ansiRenderState;
		if (current.fore.isEmpty())
			current.fore = defaultFore;
		if (current.back.isEmpty())
			current.back = defaultBack;

		QString            lineText              = m_partialLineText;
		QVector<StyleSpan> lineSpans             = m_partialLineSpans;
		bool               carriageReturnPending = m_pendingCarriageReturnOverwrite;

		auto               colorFromIndex = [](int idx) -> QString
		{
			if (idx < 0 || idx >= 256)
				return {};
			const AppController *app = AppController::instance();
			const QMudColorRef   ref = app ? app->xtermColorAt(idx) : qmudRgb(0, 0, 0);
			return QColor(qmudRed(ref), qmudGreen(ref), qmudBlue(ref)).name();
		};

		auto appendDecodedSegment = [&](const QString &decoded, const QMudStyledTextState &segmentState)
		{
			QString rawSegment = decoded;
			// Avoid treating Unicode separator controls as visual line breaks.
			rawSegment.replace(QChar(0x0085), QLatin1Char(' ')); // NEL
			rawSegment.replace(QChar(0x2028), QLatin1Char(' ')); // LINE SEPARATOR
			rawSegment.replace(QChar(0x2029), QLatin1Char(' ')); // PARAGRAPH SEPARATOR
			if (rawSegment.isEmpty())
			{
				return;
			}

			const int segmentLength = safeQSizeToInt(rawSegment.size());
			bool      startTag      = segmentState.startTag;
			auto      backspaceOne  = [&]
			{
				if (lineText.isEmpty())
					return;
				lineText.chop(1);
				while (!lineSpans.isEmpty())
				{
					StyleSpan &lastSpan = lineSpans.last();
					if (lastSpan.length > 1)
					{
						lastSpan.length -= 1;
						return;
					}
					lineSpans.pop_back();
				}
			};
			auto appendRun = [&](int start, int length)
			{
				if (length <= 0)
					return;
				lineText += rawSegment.mid(start, length);
				StyleSpan span;
				span.length     = length;
				span.fore       = segmentState.fore.isEmpty() ? QColor() : QColor(segmentState.fore);
				span.back       = segmentState.back.isEmpty() ? QColor() : QColor(segmentState.back);
				span.bold       = segmentState.bold;
				span.italic     = segmentState.italic;
				span.blink      = segmentState.blink;
				span.underline  = segmentState.underline;
				span.inverse    = segmentState.inverse;
				span.strike     = segmentState.strike;
				span.actionType = segmentState.actionType;
				span.action     = segmentState.action;
				span.hint       = segmentState.hint;
				span.variable   = segmentState.variable;
				span.startTag   = startTag;
				startTag        = false;
				if (!lineSpans.isEmpty())
				{
					StyleSpan &lastSpan = lineSpans.last();
					if (lastSpan.hasSameStyleAttributes(span))
						lastSpan.length += span.length;
					else
						lineSpans.push_back(span);
				}
				else
					lineSpans.push_back(span);
			};

			int runStart = -1;
			for (int i = 0; i < segmentLength; ++i)
			{
				const QChar ch = rawSegment.at(i);
				if (carriageReturnPending && ch != QLatin1Char('\n') && ch != QLatin1Char('\r'))
				{
					if (carriageReturnClears)
					{
						lineText.clear();
						lineSpans.clear();
					}
					carriageReturnPending = false;
				}
				if (ch == QLatin1Char('\n') || ch == QLatin1Char('\r'))
				{
					if (runStart >= 0)
					{
						appendRun(runStart, i - runStart);
						runStart = -1;
					}

					if (ch == QLatin1Char('\r'))
					{
						const bool nextIsNewline =
						    (i + 1 < segmentLength && rawSegment.at(i + 1) == QLatin1Char('\n'));
						if (!nextIsNewline)
							carriageReturnPending = carriageReturnClears;
						continue;
					}

					carriageReturnPending = false;
					emitCompletedLine(lineText, lineSpans);
					continue;
				}
				if (ch == QLatin1Char('\b'))
				{
					if (runStart >= 0)
					{
						appendRun(runStart, i - runStart);
						runStart = -1;
					}
					backspaceOne();
					continue;
				}

				if (runStart < 0)
					runStart = i;
			}

			if (runStart >= 0)
				appendRun(runStart, segmentLength - runStart);
		};

		auto normalAnsiColorFromIndex = [&](const int idx) -> QString
		{
			if (idx < 0 || idx >= normalAnsi.size())
				return {};
			return normalAnsi.at(idx).name();
		};
		auto boldAnsiColorFromIndex = [&](const int idx) -> QString
		{
			if (idx < 0 || idx >= boldAnsi.size())
				return {};
			return boldAnsi.at(idx).name();
		};

		QMudStyledTextState ansiState;
		ansiState.bold       = current.bold;
		ansiState.underline  = current.underline;
		ansiState.italic     = current.italic;
		ansiState.blink      = current.blink;
		ansiState.strike     = current.strike;
		ansiState.inverse    = current.inverse;
		ansiState.fore       = current.fore;
		ansiState.back       = current.back;
		ansiState.actionType = current.actionType;
		ansiState.action     = current.action;
		ansiState.hint       = current.hint;
		ansiState.variable   = current.variable;
		ansiState.startTag   = current.startTag;
		ansiState.monospace  = current.monospace;

		auto commitAnsiRenderState = [&]
		{
			current.bold       = ansiState.bold;
			current.underline  = ansiState.underline;
			current.italic     = ansiState.italic;
			current.blink      = ansiState.blink;
			current.strike     = ansiState.strike;
			current.inverse    = ansiState.inverse;
			current.fore       = ansiState.fore;
			current.back       = ansiState.back;
			current.actionType = ansiState.actionType;
			current.action     = ansiState.action;
			current.hint       = ansiState.hint;
			current.variable   = ansiState.variable;
			current.startTag   = ansiState.startTag;
			current.monospace  = ansiState.monospace;

			m_ansiRenderState                = current;
			m_partialLineText                = lineText;
			m_partialLineSpans               = lineSpans;
			m_pendingCarriageReturnOverwrite = carriageReturnPending;
			m_incomingPartialLineRevision    = incomingPartialLineRevision;
			publishIncomingStyledLinePartial(lineText, lineSpans);
		};

		constexpr QMudOscActionIds oscActionIds{ActionNone, ActionSend, ActionPrompt, ActionHyperlink};
		bool                       ansiRenderStateDirty = false;
		auto                       appendAnsiBytes      = [&](const QByteArrayView bytes)
		{
			if (bytes.isEmpty())
				return;
			const QVector<QMudStyledChunk> chunks = qmudParseAnsiSgrChunks(
			    bytes, m_ansiStreamState, defaultFore, defaultBack, normalAnsiColorFromIndex,
			    boldAnsiColorFromIndex, colorFromIndex, decodeIncomingDisplayBytes, ansiState, oscActionIds);
			for (const QMudStyledChunk &chunk : chunks)
				appendDecodedSegment(chunk.text, chunk.state);
			ansiRenderStateDirty = true;
		};

		const int            processedSize = safeQSizeToInt(processed.size());
		const QByteArrayView processedView(processed);
		int                  telnetEventIndex = 0;
		int                  lastTelnetOffset = 0;
		for (; telnetEventIndex < telnetPluginEvents.size(); ++telnetEventIndex)
		{
			const TelnetProcessor::TelnetPluginEvent &event       = telnetPluginEvents.at(telnetEventIndex);
			const int                                 eventOffset = qBound(0, event.offset, processedSize);
			if (eventOffset > lastTelnetOffset)
				appendAnsiBytes(processedView.sliced(lastTelnetOffset, eventOffset - lastTelnetOffset));
			lastTelnetOffset = eventOffset;
			if (ansiRenderStateDirty)
			{
				commitAnsiRenderState();
				ansiRenderStateDirty = false;
			}
			dispatchTelnetPluginEvent(event);
		}
		if (lastTelnetOffset < processedSize)
			appendAnsiBytes(processedView.sliced(lastTelnetOffset));

		commitAnsiRenderState();
		if (m_mxpTagStack.isEmpty() && !hasActiveMxpRenderContext)
			m_mxpTextBuffer.clear();
		m_currentActionSource = previousActionSource;
		return;
	}
	const QString openTagCallback =
	    luaEnabled ? m_worldAttributes.value(QStringLiteral("on_mxp_open_tag")).trimmed() : QString();
	const QString closeTagCallback =
	    luaEnabled ? m_worldAttributes.value(QStringLiteral("on_mxp_close_tag")).trimmed() : QString();
	const QString setVarCallback =
	    luaEnabled ? m_worldAttributes.value(QStringLiteral("on_mxp_set_variable")).trimmed() : QString();
	{
		QList<TelnetProcessor::MxpEvent> sorted = events;
		std::ranges::sort(sorted,
		                  [](const TelnetProcessor::MxpEvent &a, const TelnetProcessor::MxpEvent &b)
		                  {
			                  if (a.offset != b.offset)
				                  return a.offset < b.offset;
			                  return a.sequence < b.sequence;
		                  });
		QList<TelnetProcessor::MxpModeChange> sortedModeChanges = modeChanges;
		std::ranges::sort(sortedModeChanges,
		                  [](const TelnetProcessor::MxpModeChange &a, const TelnetProcessor::MxpModeChange &b)
		                  {
			                  if (a.offset != b.offset)
				                  return a.offset < b.offset;
			                  return a.sequence < b.sequence;
		                  });
		QList<TelnetProcessor::TelnetPluginEvent> sortedTelnetPluginEvents = telnetPluginEvents;

		MxpStyleState                             current               = m_mxpRenderStyle;
		QVector<MxpStyleFrame>                    stack                 = m_mxpRenderStack;
		QVector<QByteArray>                       blockStack            = m_mxpRenderBlockStack;
		bool                                      linkOpen              = m_mxpRenderLinkOpen;
		int                                       preDepth              = m_mxpRenderPreDepth;
		QString                                   lineText              = m_partialLineText;
		QVector<StyleSpan>                        lineSpans             = m_partialLineSpans;
		QString                                   notifiedLineText      = m_partialLineText;
		QVector<StyleSpan>                        notifiedLineSpans     = m_partialLineSpans;
		bool                                      carriageReturnPending = m_pendingCarriageReturnOverwrite;
		bool                                      mxpTrackingReset{false};
		auto                                      restoreCurrentFromAnsiState = [&]
		{
			current.bold       = m_ansiRenderState.bold;
			current.underline  = m_ansiRenderState.underline;
			current.italic     = m_ansiRenderState.italic;
			current.blink      = m_ansiRenderState.blink;
			current.strike     = m_ansiRenderState.strike;
			current.monospace  = m_ansiRenderState.monospace;
			current.inverse    = m_ansiRenderState.inverse;
			current.fore       = m_ansiRenderState.fore;
			current.back       = m_ansiRenderState.back;
			current.actionType = m_ansiRenderState.actionType;
			current.action     = m_ansiRenderState.action;
			current.hint       = m_ansiRenderState.hint;
			current.variable   = m_ansiRenderState.variable;
			current.startTag   = m_ansiRenderState.startTag;
			if (current.fore.isEmpty())
				current.fore = defaultFore;
			if (current.back.isEmpty())
				current.back = defaultBack;
		};
		auto resetMxpTrackingForOverflow = [&](const QString &reason)
		{
			if (mxpTrackingReset)
				return;
			mxpError(DBG_WARNING, wrnMXP_TagStackLimitExceeded, reason);
			m_mxpOpenTags.clear();
			m_mxpTagStack.clear();
			m_mxpTextBuffer.clear();
			clearAnsiActionContext();
			restoreCurrentFromAnsiState();
			stack.clear();
			blockStack.clear();
			linkOpen         = false;
			preDepth         = 0;
			mxpTrackingReset = true;
		};
		auto ensureOpenTagCapacity = [&]()
		{
			if (m_mxpOpenTags.size() >= kMaxMxpStackDepth)
			{
				resetMxpTrackingForOverflow(
				    QStringLiteral("MXP open-tag stack exceeded %1 entries; resetting MXP context.")
				        .arg(kMaxMxpStackDepth));
				return false;
			}
			return true;
		};
		auto ensureTagFrameCapacity = [&]()
		{
			if (m_mxpTagStack.size() >= kMaxMxpStackDepth)
			{
				resetMxpTrackingForOverflow(
				    QStringLiteral("MXP tag frame stack exceeded %1 entries; resetting MXP context.")
				        .arg(kMaxMxpStackDepth));
				return false;
			}
			return true;
		};
		auto ensureRenderStackCapacity = [&](const QByteArray &tag)
		{
			if (stack.size() >= kMaxMxpStackDepth)
			{
				resetMxpTrackingForOverflow(
				    QStringLiteral("MXP render stack exceeded %1 entries near <%2>; resetting MXP context.")
				        .arg(kMaxMxpStackDepth)
				        .arg(QString::fromLatin1(tag)));
				return false;
			}
			return true;
		};
		auto ensureRenderBlockCapacity = [&](const QByteArray &tag)
		{
			if (blockStack.size() >= kMaxMxpStackDepth)
			{
				resetMxpTrackingForOverflow(
				    QStringLiteral(
				        "MXP block-tag stack exceeded %1 entries near <%2>; resetting MXP context.")
				        .arg(kMaxMxpStackDepth)
				        .arg(QString::fromLatin1(tag)));
				return false;
			}
			return true;
		};
		const bool hasPersistedMxpRenderContext =
		    !stack.isEmpty() || !blockStack.isEmpty() || linkOpen || preDepth > 0;
		if (!hasPersistedMxpRenderContext)
			restoreCurrentFromAnsiState();

		auto decodeMxpAttributeBytes = [&](const QByteArray &value, const bool internalUtf8) -> QString
		{ return internalUtf8 ? QString::fromUtf8(value) : decodeIncomingIsolatedBytes(value); };

		static const QRegularExpression kHexColor6(QStringLiteral("^[0-9A-Fa-f]{6}$"));
		auto normalizeColorBytes = [&](const QByteArray &value, const bool internalUtf8) -> QString
		{
			QString name = decodeMxpAttributeBytes(value, internalUtf8).trimmed();
			if (name.isEmpty())
				return {};
			if (kHexColor6.match(name).hasMatch())
				name.prepend(QLatin1Char('#'));
			QColor const color(name);
			if (!color.isValid())
				return {};
			return color.name();
		};

		auto normalizeColorText = [](const QString &value) -> QString
		{
			QString name = value.trimmed();
			if (name.isEmpty())
				return {};
			if (kHexColor6.match(name).hasMatch())
				name.prepend(QLatin1Char('#'));
			QColor const color(name);
			if (!color.isValid())
				return {};
			return color.name();
		};
		auto mxpTagsEquivalent = [](const QByteArray &lhs, const QByteArray &rhs)
		{
			if (lhs == rhs)
				return true;
			return (lhs == "send" && rhs == "a") || (lhs == "a" && rhs == "send");
		};
		auto updateLinkOpenFromStack = [&]
		{
			linkOpen =
			    std::ranges::any_of(stack, [&](const MxpStyleFrame &frame)
			                        { return mxpTagsEquivalent(frame.tag, QByteArrayLiteral("send")); });
		};

		const int processedSize = safeQSizeToInt(processed.size());
		int       last          = 0;

		auto      applyStartTag = [&](const QByteArray                   &activeTag,
		                              const QMap<QByteArray, QByteArray> &activeAttributes,
		                              const bool                          attributesAreInternalUtf8)
		{
			const QByteArray unnamedForeLocal = activeAttributes.value("1");
			const QByteArray unnamedBackLocal = activeAttributes.value("2");

			if (activeTag == "send" || activeTag == "a")
			{
				QString href =
				    decodeMxpAttributeBytes(activeAttributes.value("href"), attributesAreInternalUtf8);
				if (href.isEmpty())
					href =
					    decodeMxpAttributeBytes(activeAttributes.value("xch_cmd"), attributesAreInternalUtf8);
				QString xchPrompt =
				    decodeMxpAttributeBytes(activeAttributes.value("xch_prompt"), attributesAreInternalUtf8);
				if (xchPrompt.isEmpty())
					xchPrompt =
					    decodeMxpAttributeBytes(activeAttributes.value("prompt"), attributesAreInternalUtf8);
				QString hint =
				    decodeMxpAttributeBytes(activeAttributes.value("hint"), attributesAreInternalUtf8);
				if (hint.isEmpty())
					hint = decodeMxpAttributeBytes(activeAttributes.value("xch_hint"),
					                               attributesAreInternalUtf8);
				if (!ensureRenderStackCapacity(activeTag))
					return;
				MxpStyleState actionState = current;
				if (activeTag == "a")
					actionState.actionType = ActionHyperlink;
				else if (!xchPrompt.isEmpty())
					actionState.actionType = ActionPrompt;
				else
					actionState.actionType = ActionSend;
				actionState.action = href;
				actionState.hint   = hint.isEmpty() ? xchPrompt : hint;
				QString variable =
				    decodeMxpAttributeBytes(activeAttributes.value("xch_set"), attributesAreInternalUtf8);
				if (variable.isEmpty())
					variable = decodeMxpAttributeBytes(activeAttributes.value("variable"),
					                                   attributesAreInternalUtf8);
				if (variable.isEmpty())
					variable =
					    decodeMxpAttributeBytes(activeAttributes.value("set"), attributesAreInternalUtf8);
				actionState.variable = variable;
				actionState.startTag = true;
				MxpStyleFrame frame;
				frame.tag                           = activeTag;
				frame.state                         = current;
				frame.actionState                   = actionState;
				frame.actionTextLineNumber          = m_linesReceived;
				frame.actionTextStartColumn         = safeQSizeToInt(lineText.size());
				frame.actionTextRuntimeLineNumber   = m_nextLineNumber;
				frame.actionTextPartialLineRevision = incomingPartialLineRevision;
				stack.push_back(frame);
				linkOpen = true;
				current  = actionState;
			}
			else if (activeTag == "bold" || activeTag == "b" || activeTag == "strong" || activeTag == "h1" ||
			         activeTag == "h2" || activeTag == "h3" || activeTag == "h4" || activeTag == "h5" ||
			         activeTag == "h6")
			{
				if (!ensureRenderStackCapacity(activeTag))
					return;
				stack.push_back(makeMxpStyleFrame(activeTag, current));
				current.bold = true;
			}
			else if (activeTag == "underline" || activeTag == "u")
			{
				if (!ensureRenderStackCapacity(activeTag))
					return;
				stack.push_back(makeMxpStyleFrame(activeTag, current));
				current.underline = true;
			}
			else if (activeTag == "italic" || activeTag == "i" || activeTag == "em")
			{
				if (!ensureRenderStackCapacity(activeTag))
					return;
				stack.push_back(makeMxpStyleFrame(activeTag, current));
				current.italic = true;
			}
			else if (activeTag == "strike" || activeTag == "s")
			{
				if (!ensureRenderStackCapacity(activeTag))
					return;
				stack.push_back(makeMxpStyleFrame(activeTag, current));
				current.strike = true;
			}
			else if (activeTag == "color")
			{
				if (!ensureRenderStackCapacity(activeTag))
					return;
				stack.push_back(makeMxpStyleFrame(activeTag, current));
				if (!ignoreMxpColourChanges)
				{
					QString fore =
					    normalizeColorBytes(activeAttributes.value("fore"), attributesAreInternalUtf8);
					if (fore.isEmpty())
						fore = normalizeColorBytes(unnamedForeLocal, attributesAreInternalUtf8);
					QString back =
					    normalizeColorBytes(activeAttributes.value("back"), attributesAreInternalUtf8);
					if (back.isEmpty())
						back = normalizeColorBytes(unnamedBackLocal, attributesAreInternalUtf8);
					if (!fore.isEmpty())
						current.fore = fore;
					if (!back.isEmpty())
						current.back = back;
				}
			}
			else if (activeTag == "c")
			{
				if (!ensureRenderStackCapacity(activeTag))
					return;
				stack.push_back(makeMxpStyleFrame(activeTag, current));
				if (!ignoreMxpColourChanges)
				{
					QString const fore = normalizeColorBytes(unnamedForeLocal, attributesAreInternalUtf8);
					QString const back = normalizeColorBytes(unnamedBackLocal, attributesAreInternalUtf8);
					if (!fore.isEmpty())
						current.fore = fore;
					if (!back.isEmpty())
						current.back = back;
				}
			}
			else if (activeTag == "font")
			{
				if (!ensureRenderStackCapacity(activeTag))
					return;
				stack.push_back(makeMxpStyleFrame(activeTag, current));
				QString colorSpec =
				    decodeMxpAttributeBytes(activeAttributes.value("color"), attributesAreInternalUtf8);
				if (colorSpec.isEmpty())
					colorSpec =
					    decodeMxpAttributeBytes(activeAttributes.value("fgcolor"), attributesAreInternalUtf8);
				QStringList const parts = colorSpec.split(',', Qt::SkipEmptyParts);
				for (QString part : parts)
				{
					part = part.trimmed();
					if (part.compare(QStringLiteral("bold"), Qt::CaseInsensitive) == 0)
						current.bold = true;
					else if (part.compare(QStringLiteral("italic"), Qt::CaseInsensitive) == 0)
						current.italic = true;
					else if (part.compare(QStringLiteral("underline"), Qt::CaseInsensitive) == 0)
						current.underline = true;
					else if (part.compare(QStringLiteral("blink"), Qt::CaseInsensitive) == 0)
						current.blink = true;
					else if (part.compare(QStringLiteral("inverse"), Qt::CaseInsensitive) == 0)
					{
						current.inverse = true;
						if (!current.fore.isEmpty() || !current.back.isEmpty())
							qSwap(current.fore, current.back);
					}
					else
					{
						if (!ignoreMxpColourChanges)
						{
							QString const color = normalizeColorText(part);
							if (!color.isEmpty())
								current.fore = color;
						}
					}
				}
				QString back =
				    decodeMxpAttributeBytes(activeAttributes.value("back"), attributesAreInternalUtf8);
				if (back.isEmpty())
					back =
					    decodeMxpAttributeBytes(activeAttributes.value("bgcolor"), attributesAreInternalUtf8);
				if (!ignoreMxpColourChanges)
				{
					QString const backColor = normalizeColorText(back);
					if (!backColor.isEmpty())
						current.back = backColor;
				}
			}
			else if (activeTag == "high" || activeTag == "h")
			{
				if (!ensureRenderStackCapacity(activeTag))
					return;
				stack.push_back(makeMxpStyleFrame(activeTag, current));
				if (!current.fore.isEmpty())
				{
					QColor const color(current.fore);
					if (color.isValid())
						current.fore = color.lighter(115).name();
				}
			}
			else if (activeTag == "tt" || activeTag == "samp")
			{
				if (!ensureRenderStackCapacity(activeTag))
					return;
				stack.push_back(makeMxpStyleFrame(activeTag, current));
				current.monospace = true;
			}
			else if (activeTag == "pre")
			{
				if (!ensureRenderBlockCapacity(activeTag))
					return;
				blockStack.push_back(activeTag);
				preDepth++;
			}
			else if (activeTag == "center" || activeTag == "ul" || activeTag == "ol" || activeTag == "li")
			{
				if (!ensureRenderBlockCapacity(activeTag))
					return;
				blockStack.push_back(activeTag);
			}
			else if (activeTag == "reset")
			{
				if (linkOpen)
				{
					linkOpen = false;
				}
				while (!blockStack.isEmpty())
				{
					const QByteArray closeTag = blockStack.takeLast();
					if (closeTag == "pre")
					{
						if (preDepth > 0)
							preDepth--;
					}
				}
				current = MxpStyleState();
				stack.clear();
			}
		};

		auto findMxpTextActionBufferedStartIndex = [&](const qint64 runtimeLineNumber)
		{
			if (runtimeLineNumber <= 0)
				return -1;

			for (int index = safeQSizeToInt(m_lines.size()) - 1; index >= 0; --index)
			{
				if (m_lines.at(index).lineNumber == runtimeLineNumber)
					return index;
			}

			for (int index = 0; index < m_lines.size(); ++index)
			{
				if (m_lines.at(index).lineNumber > runtimeLineNumber)
					return index;
			}
			return -1;
		};

		auto resolveBufferedMxpTextActionReferences =
		    [&](const MxpStyleFrame &frame, const QString &replacement)
		{
			const int startIndex = findMxpTextActionBufferedStartIndex(frame.actionTextRuntimeLineNumber);
			if (startIndex < 0)
				return;

			int firstChangedIndex = -1;
			for (int i = startIndex; i < m_lines.size(); ++i)
			{
				LineEntry &entry = m_lines[i];
				if ((entry.flags & LineOutput) == 0)
					continue;
				const int startColumn =
				    entry.lineNumber == frame.actionTextRuntimeLineNumber
				        ? qBound(0, frame.actionTextStartColumn, safeQSizeToInt(entry.text.size()))
				        : 0;
				if (resolveMxpTextActionReferences(entry.spans, startColumn,
				                                   safeQSizeToInt(entry.text.size()), frame.actionState,
				                                   replacement) &&
				    firstChangedIndex < 0)
				{
					firstChangedIndex = i;
				}
			}
			if (firstChangedIndex < 0)
				return;
			invalidateLuaCallbackLineBufferSnapshot();
			notifyOutputViewRangeChanged(firstChangedIndex);
		};

		auto appendMxpBodyLine = [](QString &body, bool &hasBodyLine, const QString &line)
		{
			if (hasBodyLine)
				body.append(QLatin1Char('\n'));
			body.append(line);
			hasBodyLine = true;
		};

		auto mxpActionBodyStillOnlyInCurrentPartialLine = [&](const MxpStyleFrame &frame)
		{
			return frame.actionTextLineNumber == m_linesReceived &&
			       frame.actionTextPartialLineRevision == incomingPartialLineRevision;
		};
		auto mxpCurrentPartialActionStartColumn = [&](const MxpStyleFrame &frame)
		{
			return mxpActionBodyStillOnlyInCurrentPartialLine(frame)
			           ? qBound(0, frame.actionTextStartColumn, safeQSizeToInt(lineText.size()))
			           : 0;
		};

		auto collectMxpTextActionBody = [&](const MxpStyleFrame &frame)
		{
			QString body;
			bool    hasBodyLine = false;
			if (mxpActionBodyStillOnlyInCurrentPartialLine(frame))
			{
				const int startColumn = mxpCurrentPartialActionStartColumn(frame);
				appendMxpBodyLine(body, hasBodyLine, lineText.mid(startColumn));
				return body;
			}
			if (frame.actionTextRuntimeLineNumber >= 0)
			{
				const int startIndex = findMxpTextActionBufferedStartIndex(frame.actionTextRuntimeLineNumber);
				if (startIndex >= 0)
				{
					for (int i = startIndex; i < m_lines.size(); ++i)
					{
						const LineEntry &entry = m_lines.at(i);
						if ((entry.flags & LineOutput) == 0)
							continue;
						const int startColumn =
						    entry.lineNumber == frame.actionTextRuntimeLineNumber
						        ? qBound(0, frame.actionTextStartColumn, safeQSizeToInt(entry.text.size()))
						        : 0;
						appendMxpBodyLine(body, hasBodyLine, entry.text.mid(startColumn));
					}
				}
			}

			const int startColumn = mxpCurrentPartialActionStartColumn(frame);
			appendMxpBodyLine(body, hasBodyLine, lineText.mid(startColumn));
			return body;
		};

		auto applyEndTag = [&](const QByteArray &closeTag, const QString *resolvedBodyText)
		{
			if (closeTag == "send" || closeTag == "a")
			{
				bool closedActionTag = false;
				for (int i = safeQSizeToInt(stack.size()) - 1; i >= 0; --i)
				{
					if (mxpTagsEquivalent(stack.at(i).tag, closeTag))
					{
						const MxpStyleFrame frame = stack.at(i);
						const QString       replacement =
						    resolvedBodyText ? *resolvedBodyText : collectMxpTextActionBody(frame);
						const int startColumn = mxpCurrentPartialActionStartColumn(frame);
						resolveMxpTextActionReferences(lineSpans, startColumn,
						                               safeQSizeToInt(lineText.size()), frame.actionState,
						                               replacement);
						if (!mxpActionBodyStillOnlyInCurrentPartialLine(frame))
							resolveBufferedMxpTextActionReferences(frame, replacement);
						current = frame.state;
						stack.removeAt(i);
						closedActionTag = true;
						break;
					}
				}
				if (closedActionTag || linkOpen)
					updateLinkOpenFromStack();
			}
			else if (closeTag == "pre" || closeTag == "center" || closeTag == "ul" || closeTag == "ol" ||
			         closeTag == "li")
			{
				for (int i = safeQSizeToInt(blockStack.size()) - 1; i >= 0; --i)
				{
					if (blockStack.at(i) == closeTag)
					{
						for (int j = safeQSizeToInt(blockStack.size()) - 1; j >= i; --j)
						{
							const QByteArray &innerTag = blockStack.at(j);
							if (innerTag == "pre")
							{
								if (preDepth > 0)
									preDepth--;
							}
						}
						blockStack.resize(i);
						break;
					}
				}
			}
			else
			{
				for (int i = safeQSizeToInt(stack.size()) - 1; i >= 0; --i)
				{
					if (stack.at(i).tag == closeTag)
					{
						current = stack.at(i).state;
						stack.removeAt(i);
						break;
					}
				}
			}
		};

		auto applyMxpModeBarrier = [&](const TelnetProcessor::MxpModeChange &modeChange)
		{
			const bool closeOpenTagsForModeTransition =
			    isMxpOpenModeCode(modeChange.oldMode) && !isMxpOpenModeCode(modeChange.newMode);
			const bool resetOrLockedTransition =
			    isMxpResetModeCode(modeChange.newMode) || isMxpLockedModeCode(modeChange.newMode);

			if (!closeOpenTagsForModeTransition && !resetOrLockedTransition)
				return;

			m_mxpOpenTags.clear();
			m_mxpTagStack.clear();

			if (!stack.isEmpty())
				current = stack.front().state;
			stack.clear();
			blockStack.clear();
			linkOpen = false;
			preDepth = 0;

			current.actionType = ActionNone;
			current.action.clear();
			current.hint.clear();
			current.variable.clear();
			current.startTag = false;
		};

		const QByteArrayView processedView(processed);

		auto                 appendStyled = [&](const QByteArrayView bytes)
		{
			if (bytes.isEmpty())
				return;
			auto appendDecodedSegment = [&](const QString &decoded, const QMudStyledTextState &segmentState)
			{
				QString rawSegment = decoded;
				if (rawSegment.isEmpty())
				{
					return;
				}

				const int segmentLength = safeQSizeToInt(rawSegment.size());
				bool      startTag      = segmentState.startTag;
				auto      backspaceOne  = [&]
				{
					if (lineText.isEmpty())
						return;
					lineText.chop(1);
					while (!lineSpans.isEmpty())
					{
						StyleSpan &lastSpan = lineSpans.last();
						if (lastSpan.length > 1)
						{
							lastSpan.length -= 1;
							return;
						}
						lineSpans.pop_back();
					}
				};
				auto appendRun = [&](int start, int length)
				{
					if (length <= 0)
						return;
					lineText += rawSegment.mid(start, length);
					StyleSpan span;
					span.length     = length;
					span.fore       = segmentState.fore.isEmpty() ? QColor() : QColor(segmentState.fore);
					span.back       = segmentState.back.isEmpty() ? QColor() : QColor(segmentState.back);
					span.bold       = segmentState.bold;
					span.italic     = segmentState.italic;
					span.blink      = segmentState.blink;
					span.underline  = segmentState.underline;
					span.inverse    = segmentState.inverse;
					span.strike     = segmentState.strike;
					span.actionType = segmentState.actionType;
					span.action     = segmentState.action;
					span.hint       = segmentState.hint;
					span.variable   = segmentState.variable;
					span.startTag   = startTag;
					startTag        = false;
					if (!lineSpans.isEmpty())
					{
						StyleSpan &lastSpan = lineSpans.last();
						if (lastSpan.hasSameStyleAttributes(span))
						{
							lastSpan.length += span.length;
						}
						else
							lineSpans.push_back(span);
					}
					else
						lineSpans.push_back(span);
				};
				int runStart = -1;
				for (int i = 0; i < segmentLength; ++i)
				{
					const QChar ch = rawSegment.at(i);
					if (carriageReturnPending && ch != QLatin1Char('\n') && ch != QLatin1Char('\r'))
					{
						if (carriageReturnClears)
						{
							lineText.clear();
							lineSpans.clear();
						}
						carriageReturnPending = false;
					}
					if (ch == QLatin1Char('\n') || ch == QLatin1Char('\r'))
					{
						if (runStart >= 0)
						{
							appendRun(runStart, i - runStart);
							runStart = -1;
						}

						if (ch == QLatin1Char('\r'))
						{
							const bool nextIsNewline =
							    (i + 1 < segmentLength && rawSegment.at(i + 1) == QLatin1Char('\n'));
							if (!nextIsNewline)
								carriageReturnPending = carriageReturnClears;
							continue;
						}

						carriageReturnPending = false;
						emitCompletedLine(lineText, lineSpans);
						continue;
					}
					if (ch == QLatin1Char('\b'))
					{
						if (runStart >= 0)
						{
							appendRun(runStart, i - runStart);
							runStart = -1;
						}
						backspaceOne();
						continue;
					}
					if (runStart < 0)
						runStart = i;
				}
				if (runStart >= 0)
					appendRun(runStart, segmentLength - runStart);
			};

			auto colorFromIndex = [](int idx) -> QString
			{
				if (idx < 0 || idx >= 256)
					return {};
				const AppController *app = AppController::instance();
				const QMudColorRef   ref = app ? app->xtermColorAt(idx) : qmudRgb(0, 0, 0);
				return QColor(qmudRed(ref), qmudGreen(ref), qmudBlue(ref)).name();
			};
			auto normalAnsiColorFromIndex = [&](const int idx) -> QString
			{
				if (idx < 0 || idx >= normalAnsi.size())
					return {};
				return normalAnsi.at(idx).name();
			};
			auto boldAnsiColorFromIndex = [&](const int idx) -> QString
			{
				if (idx < 0 || idx >= boldAnsi.size())
					return {};
				return boldAnsi.at(idx).name();
			};

			QMudStyledTextState ansiState;
			ansiState.bold       = current.bold;
			ansiState.underline  = current.underline;
			ansiState.italic     = current.italic;
			ansiState.blink      = current.blink;
			ansiState.strike     = current.strike;
			ansiState.inverse    = current.inverse;
			ansiState.fore       = current.fore;
			ansiState.back       = current.back;
			ansiState.actionType = current.actionType;
			ansiState.action     = current.action;
			ansiState.hint       = current.hint;
			ansiState.variable   = current.variable;
			ansiState.startTag   = current.startTag;
			ansiState.monospace  = current.monospace;

			constexpr QMudOscActionIds oscActionIds{ActionNone, ActionSend, ActionPrompt, ActionHyperlink};
			const QVector<QMudStyledChunk> chunks = qmudParseAnsiSgrChunks(
			    bytes, m_ansiStreamState, defaultFore, defaultBack, normalAnsiColorFromIndex,
			    boldAnsiColorFromIndex, colorFromIndex, decodeIncomingDisplayBytes, ansiState, oscActionIds);
			for (const QMudStyledChunk &chunk : chunks)
				appendDecodedSegment(chunk.text, chunk.state);

			current.bold       = ansiState.bold;
			current.underline  = ansiState.underline;
			current.italic     = ansiState.italic;
			current.blink      = ansiState.blink;
			current.strike     = ansiState.strike;
			current.inverse    = ansiState.inverse;
			current.fore       = ansiState.fore;
			current.back       = ansiState.back;
			current.actionType = ansiState.actionType;
			current.action     = ansiState.action;
			current.hint       = ansiState.hint;
			current.variable   = ansiState.variable;
			current.startTag   = ansiState.startTag;
			current.monospace  = ansiState.monospace;
		};

		auto commitMxpRenderState = [&]
		{
			m_mxpRenderStyle             = current;
			m_mxpRenderStack             = stack;
			m_mxpRenderBlockStack        = blockStack;
			m_mxpRenderLinkOpen          = linkOpen;
			m_mxpRenderPreDepth          = preDepth;
			m_ansiRenderState.bold       = current.bold;
			m_ansiRenderState.underline  = current.underline;
			m_ansiRenderState.italic     = current.italic;
			m_ansiRenderState.blink      = current.blink;
			m_ansiRenderState.strike     = current.strike;
			m_ansiRenderState.monospace  = current.monospace;
			m_ansiRenderState.inverse    = current.inverse;
			m_ansiRenderState.fore       = current.fore;
			m_ansiRenderState.back       = current.back;
			m_ansiRenderState.actionType = current.actionType;
			m_ansiRenderState.action     = current.action;
			m_ansiRenderState.hint       = current.hint;
			m_ansiRenderState.variable   = current.variable;
			m_ansiRenderState.startTag   = current.startTag;

			m_partialLineText                = lineText;
			m_partialLineSpans               = lineSpans;
			m_pendingCarriageReturnOverwrite = carriageReturnPending;
			m_incomingPartialLineRevision    = incomingPartialLineRevision;
			if (lineText != notifiedLineText || lineSpans != notifiedLineSpans)
			{
				notifiedLineText  = lineText;
				notifiedLineSpans = lineSpans;
				publishIncomingStyledLinePartial(lineText, lineSpans);
			}
		};

		auto isAtomicTag = [](const QByteArray &tag)
		{
			AtomicTagInfo info;
			return lookupAtomicTagInfo(tag, info);
		};

		int  modeChangeIndex        = 0;
		int  telnetPluginEventIndex = 0;
		auto boundaryComesBefore    = [](const int boundaryOffset, const int boundarySequence,
		                                 const int eventOffset, const int eventSequence)
		{
			return (boundaryOffset < eventOffset) ||
			       (boundaryOffset == eventOffset && boundarySequence < eventSequence);
		};
		auto drainTelnetAndModeBoundariesBefore = [&](const int eventOffset, const int eventSequence)
		{
			while (true)
			{
				const bool haveModeChange =
				    modeChangeIndex < sortedModeChanges.size() &&
				    boundaryComesBefore(sortedModeChanges.at(modeChangeIndex).offset,
				                        sortedModeChanges.at(modeChangeIndex).sequence, eventOffset,
				                        eventSequence);
				const bool haveTelnetEvent =
				    telnetPluginEventIndex < sortedTelnetPluginEvents.size() &&
				    boundaryComesBefore(sortedTelnetPluginEvents.at(telnetPluginEventIndex).offset,
				                        sortedTelnetPluginEvents.at(telnetPluginEventIndex).sequence,
				                        eventOffset, eventSequence);
				if (!haveModeChange && !haveTelnetEvent)
					break;

				const bool takeModeChange =
				    haveModeChange &&
				    (!haveTelnetEvent ||
				     boundaryComesBefore(sortedModeChanges.at(modeChangeIndex).offset,
				                         sortedModeChanges.at(modeChangeIndex).sequence,
				                         sortedTelnetPluginEvents.at(telnetPluginEventIndex).offset,
				                         sortedTelnetPluginEvents.at(telnetPluginEventIndex).sequence));
				if (takeModeChange)
				{
					const TelnetProcessor::MxpModeChange &modeChange =
					    sortedModeChanges.at(modeChangeIndex++);
					if (modeChange.offset > last)
					{
						appendStyled(processedView.sliced(last, modeChange.offset - last));
						last = modeChange.offset;
					}
					applyMxpModeBarrier(modeChange);
					continue;
				}

				const TelnetProcessor::TelnetPluginEvent &telnetEvent =
				    sortedTelnetPluginEvents.at(telnetPluginEventIndex++);
				const int telnetOffset = qBound(0, telnetEvent.offset, processedSize);
				if (telnetOffset > last)
				{
					appendStyled(processedView.sliced(last, telnetOffset - last));
					last = telnetOffset;
				}
				commitMxpRenderState();
				dispatchTelnetPluginEvent(telnetEvent);
			}
		};

		for (const TelnetProcessor::MxpEvent &ev : sorted)
		{
			if (mxpTrackingReset)
				break;

			drainTelnetAndModeBoundariesBefore(ev.offset, ev.sequence);

			if (ev.offset > last)
			{
				appendStyled(processedView.sliced(last, ev.offset - last));
				last = ev.offset;
			}

			const QByteArray                   tag                       = ev.name.toLower();
			const QString                      tagText                   = QString::fromLatin1(tag);
			QMap<QByteArray, QByteArray>       attributes                = ev.attributes;
			bool                               attributesAreInternalUtf8 = false;
			QByteArray                         effectiveTag              = tag;
			TelnetProcessor::CustomElementInfo customInfo;
			AtomicTagInfo                      atomicInfo;
			bool                               hasCustomElement = false;
			bool                               hasAtomicTag     = false;
			bool                               tagIsCommand     = false;
			bool                               mxpSecure        = false;
			if (ev.type == TelnetProcessor::MxpEvent::StartTag)
			{
				hasCustomElement = m_telnet.getCustomElementInfo(tag, customInfo);
				hasAtomicTag     = lookupAtomicTagInfo(tag, atomicInfo);
				if (hasCustomElement)
				{
					const ParsedMxpArguments providedArgs =
					    parseMxpArguments(ev.raw, decodeIncomingIsolatedBytes);
					const QList<MxpArgument> internalProvidedArgs =
					    internalizeMxpArgumentRawValues(providedArgs.args, decodeIncomingIsolatedBytes);
					const QMap<QByteArray, QByteArray> mergedDefaults =
					    mergeAttributeDefaultsBytes(customInfo.attributes, internalProvidedArgs);
					QByteArray const resolvedDefinition = resolveDefinitionEntities(
					    customInfo.definition, mergedDefaults, m_telnet, encodeMxpInternalEntityText);
					QMap<QByteArray, QByteArray> aliasAttributes;
					QByteArray                   aliasTag;
					if (parseDefinitionAlias(resolvedDefinition, aliasTag, aliasAttributes))
					{
						effectiveTag = aliasTag;
						if (!aliasAttributes.isEmpty())
						{
							attributes                = aliasAttributes;
							attributesAreInternalUtf8 = true;
						}
					}
				}
				if (!hasCustomElement && !hasAtomicTag)
				{
					mxpError(DBG_ERROR, errMXP_UnknownElement,
					         QStringLiteral("Unknown MXP element: <%1> (custom elements loaded: %2)")
					             .arg(tagText)
					             .arg(m_telnet.customElementCount()));
					continue;
				}
				if (hasAtomicTag && atomicInfo.puebloOnly && !m_telnet.isPuebloActive())
				{
					mxpError(DBG_ERROR, errMXP_PuebloOnly,
					         QStringLiteral("Using Pueblo-only element in MXP mode: <%1>").arg(tagText));
					continue;
				}
				const bool tagIsOpen = hasCustomElement ? customInfo.open : atomicInfo.open;
				tagIsCommand         = hasCustomElement ? customInfo.command : atomicInfo.command;
				mxpSecure            = ev.secure;
				if (!tagIsOpen && !mxpSecure)
				{
					mxpError(
					    DBG_ERROR, errMXP_ElementWhenNotSecure,
					    QStringLiteral("Secure MXP tag ignored when not in secure mode: <%1>").arg(tagText));
					continue;
				}
			}
			if (ev.type == TelnetProcessor::MxpEvent::Definition)
			{
				QByteArray definition = ev.raw;
				if (definition.startsWith('!'))
					definition = definition.mid(1);
				QByteArray defType;
				mxpGetWord(defType, definition);
				defType = defType.toLower();
				if (defType == "entity")
				{
					QByteArray name;
					mxpGetWord(name, definition);
					if (!name.isEmpty())
					{
						QByteArray value;
						if (m_telnet.getCustomEntityValue(name, value))
						{
							// Custom MXP entity storage is internal UTF-8; keep this plugin payload in that form.
							QByteArray payload = name.toLower();
							payload.append('=');
							payload.append(value);
							callPluginCallbacksWithBytes(QStringLiteral("OnPluginMXPsetEntity"), payload,
							                             true);
						}
					}
				}
				continue;
			}

			if (ev.type == TelnetProcessor::MxpEvent::StartTag)
			{
				const ParsedMxpArguments parsed = parseMxpArguments(ev.raw, decodeIncomingIsolatedBytes);
				QString                  argsForScript = parsed.rawArguments;
				if (isAtomicTag(tag))
					argsForScript = buildArgumentString(parsed.args);

				// MUSHclient parity: MXP open-tag callbacks are only invoked for
				// user-defined tags, not built-in atomic command/style tags.
				if (hasCustomElement)
				{
					const QString pluginOpenPayload = QStringLiteral("%1,%2").arg(tagText, argsForScript);
					if (!callPluginCallbacksStopOnFalse(QStringLiteral("OnPluginMXPopenTag"),
					                                    pluginOpenPayload))
						continue;
					if (!openTagCallback.isEmpty() && m_luaCallbacks && tag != "afk")
					{
						const QMap<QString, QString> argumentTable = buildArgumentTable(parsed.args);
						const QSharedPointer<LuaCallbackEngine> worldLua(
						    m_luaCallbacks, [](LuaCallbackEngine * /*unused*/) {});
						LuaBatchDispatchRequest request;
						request.kind                        = LuaBatchDispatchKind::MxpStartTag;
						request.engines                     = {worldLua};
						request.functionName                = openTagCallback;
						request.stringArg                   = tagText;
						request.stringArg2                  = argsForScript;
						request.mapArg                      = argumentTable;
						const LuaBatchDispatchResult result = queuePluginCallbackDispatch(request, true);
						if (const bool suppress = result.boolResultValid ? result.boolResult : false;
						    suppress)
							continue;
					}
				}

				if (!tagIsCommand)
				{
					if (!ensureOpenTagCapacity())
						continue;
					MxpOpenTag openTag;
					openTag.tag          = tag;
					openTag.openedSecure = mxpSecure;
					openTag.noReset      = hasAtomicTag ? atomicInfo.noReset : false;
					m_mxpOpenTags.push_back(openTag);
				}

				QVector<QByteArray>                   customTagSequence;
				QVector<QMap<QByteArray, QByteArray>> customTagAttributes;
				if (hasCustomElement)
				{
					const QList<MxpArgument> internalProvidedArgs =
					    internalizeMxpArgumentRawValues(parsed.args, decodeIncomingIsolatedBytes);
					const QMap<QByteArray, QByteArray> mergedDefaults =
					    mergeAttributeDefaultsBytes(customInfo.attributes, internalProvidedArgs);
					const QVector<QByteArray> rawTags = extractDefinitionTags(customInfo.definition);
					for (const QByteArray &rawTag : rawTags)
					{
						const QByteArray resolved = resolveDefinitionEntities(
						    rawTag, mergedDefaults, m_telnet, encodeMxpInternalEntityText);
						ParsedMxpArguments const defParsed = parseMxpArguments(resolved);
						QByteArray               tagName;
						QByteArray               temp = resolved;
						mxpGetWord(tagName, temp);
						tagName = tagName.toLower();
						if (tagName.isEmpty())
							continue;
						if (!isAtomicTag(tagName))
							continue;
						QMap<QByteArray, QByteArray> const attrMap = buildArgumentTableBytes(defParsed.args);
						customTagSequence.push_back(tagName);
						customTagAttributes.push_back(attrMap);
					}
				}

				bool const trackable = !tagIsCommand;
				if (tagIsCommand)
				{
					customTagSequence.clear();
					customTagAttributes.clear();
				}

				if (!customTagSequence.isEmpty())
				{
					QVector<QByteArray> appliedCloseTags;
					for (int i = 0; i < customTagSequence.size(); ++i)
					{
						const QByteArray                  &childTag   = customTagSequence.at(i);
						const QMap<QByteArray, QByteArray> childAttrs = customTagAttributes.value(i);
						applyStartTag(childTag, childAttrs, true);
						if (mxpTrackingReset)
							break;
						appliedCloseTags.push_back(childTag);
					}
					if (mxpTrackingReset)
						continue;

					if (trackable)
					{
						if (!ensureTagFrameCapacity())
							continue;
						MxpTagFrame frame;
						frame.tag          = tag;
						frame.contentStart = mxpBaseOffset + ev.offset;
						if (frame.variableName.isEmpty() && !customInfo.flag.isEmpty())
							frame.variableName = QString::fromUtf8(customInfo.flag).toLower();
						frame.closeTags = appliedCloseTags;
						m_mxpTagStack.push_back(frame);
					}
					continue;
				}

				if (trackable)
				{
					if (!ensureTagFrameCapacity())
						continue;
					MxpTagFrame frame;
					frame.tag          = tag;
					frame.contentStart = mxpBaseOffset + ev.offset;
					if (tag == "var" || tag == "v")
					{
						for (const auto &arg : parsed.args)
						{
							if (arg.name.isEmpty() && arg.position == 1)
							{
								frame.variableName = arg.value.toLower();
								break;
							}
						}
					}
					if (frame.variableName.isEmpty() && hasCustomElement && !customInfo.flag.isEmpty())
						frame.variableName = QString::fromUtf8(customInfo.flag).toLower();
					m_mxpTagStack.push_back(frame);
				}

				applyStartTag(effectiveTag, attributes, attributesAreInternalUtf8);
				if (mxpTrackingReset)
					continue;

				if (effectiveTag == "version" || effectiveTag == "afk" || effectiveTag == "support" ||
				    effectiveTag == "option" || effectiveTag == "recommend_option" ||
				    effectiveTag == "user" || effectiveTag == "username" || effectiveTag == "password" ||
				    effectiveTag == "pass")
				{
					// Port of MXP_OpenAtomicTag actions that generate responses.
					const QByteArray endLine("\r\n");
					auto             sendMxpPacket = [&](const QString &payload)
					{
						if (payload.isEmpty())
							return;
						if (useUtf8)
							sendToWorld(payload.toUtf8());
						else
							sendToWorld(qmudEncodeWorldText(payload, legacyEncodingName, nullptr));
					};

					auto extractArgumentTokens =
					    [&](const QMap<QByteArray, QByteArray> &attrs, const bool attrsAreInternalUtf8)
					{
						QList<int>  numericKeys;
						QStringList tokens;
						for (auto it = attrs.constBegin(); it != attrs.constEnd(); ++it)
						{
							if (it.key() == "_tag")
								continue;
							bool      ok    = false;
							const int index = QString::fromLatin1(it.key()).toInt(&ok);
							if (ok)
							{
								numericKeys.append(index);
								continue;
							}
							const QString key   = QString::fromLatin1(it.key());
							const QString value = decodeMxpAttributeBytes(it.value(), attrsAreInternalUtf8);
							if (value.isEmpty())
								tokens.append(key);
							else
								tokens.append(QStringLiteral("%1=%2").arg(key, value));
						}
						std::ranges::sort(numericKeys);
						for (int const idx : numericKeys)
						{
							const QByteArray key = QByteArray::number(idx);
							const QString    value =
							    decodeMxpAttributeBytes(attrs.value(key), attrsAreInternalUtf8);
							if (!value.isEmpty())
								tokens.append(value);
						}
						return tokens;
					};

					auto getWorldAttribute = [&](const QString &key) { return m_worldAttributes.value(key); };

					auto parseOptionValue = [&](const QString &raw, bool *okOut) -> long long
					{
						const QString trimmed = raw.trimmed();
						if (trimmed.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
						    trimmed.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0)
						{
							if (okOut)
								*okOut = true;
							return 1;
						}
						if (trimmed.compare(QStringLiteral("n"), Qt::CaseInsensitive) == 0 ||
						    trimmed.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0)
						{
							if (okOut)
								*okOut = true;
							return 0;
						}
						bool            ok    = false;
						const long long value = trimmed.toLongLong(&ok);
						if (okOut)
							*okOut = ok;
						return ok ? value : 0;
					};

					auto currentOptionValue = [&](const QString            &key,
					                              const WorldNumericOption *option) -> long long
					{
						const QString   raw   = getWorldAttribute(key);
						bool            ok    = false;
						const long long value = parseOptionValue(raw, &ok);
						if (ok)
							return value;
						if (option)
							return option->defaultValue;
						return 0;
					};

					auto getWorldPassword = [&]
					{
						QString password = getWorldAttribute(QStringLiteral("password"));
						if (!password.isEmpty())
							return password;
						const QString base64 = getWorldAttribute(QStringLiteral("password_base64"));
						if (!base64.isEmpty())
						{
							const QByteArray decoded = QByteArray::fromBase64(base64.toLocal8Bit());
							return QString::fromLocal8Bit(decoded);
						}
						return QString();
					};

					auto isConnectMxp = [&]
					{
						const int connectMethod = getWorldAttribute(QStringLiteral("connect_method")).toInt();
						return connectMethod == 3; // eConnectMXP
					};

					auto mxpSupports = [&] { return mxpSupportedTags(); };

					auto isValidMxpName = [](const QString &name)
					{
						if (name.isEmpty())
							return false;
						const QChar first = name.at(0);
						if (!first.isLetter())
							return false;
						for (int i = 1; i < name.size(); ++i)
						{
							const QChar ch = name.at(i);
							if (ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('-') ||
							    ch == QLatin1Char('.'))
								continue;
							return false;
						}
						return true;
					};

					if (effectiveTag == "version")
					{
						// From mxpOpenAtomic.cpp: <VERSION MXP="0.5" CLIENT=MUSHclient VERSION="..." REGISTERED=YES>
						const QString version = QString::fromLatin1(kVersionString);
						const QString packet =
						    QStringLiteral("\x1B[1z<VERSION MXP=\"0.5\" CLIENT=QMud VERSION=\"%1\" "
						                   "REGISTERED=YES>%2")
						        .arg(version, QString::fromLatin1(endLine));
						sendMxpPacket(packet);
						mxpError(DBG_INFO, infoMXP_VersionSent,
						         QStringLiteral("Sent version response: %1").arg(packet.mid(4)));
					}
					else if (effectiveTag == "afk")
					{
						const QString sendFlag = getWorldAttribute(QStringLiteral("send_mxp_afk_response"));
						const bool    allowResponse =
						    !(sendFlag == QStringLiteral("0") ||
						      sendFlag.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0);
						if (allowResponse)
						{
							const QStringList args =
							    extractArgumentTokens(attributes, attributesAreInternalUtf8);
							const QString   challenge = args.isEmpty() ? QString() : args.first();
							const QDateTime now       = QDateTime::currentDateTime();
							const qint64    seconds =
							    (m_lastUserInput.isValid() ? m_lastUserInput.secsTo(now) : 0);
							const QString packet = QStringLiteral("\x1B[1z<AFK %1 %2>%3")
							                           .arg(seconds)
							                           .arg(challenge, QString::fromLatin1(endLine));
							sendMxpPacket(packet);
							mxpError(DBG_INFO, infoMXP_AFKSent,
							         QStringLiteral("Sent AFK response: %1").arg(packet.mid(4)));
						}
					}
					else if (effectiveTag == "support")
					{
						// From mxpOpenAtomic.cpp: reply with supported tags and arguments.
						const QStringList args = extractArgumentTokens(attributes, attributesAreInternalUtf8);
						QString           supports;
						const auto       *supported              = mxpSupports();
						bool              invalidSupportArgument = false;

						if (args.isEmpty())
						{
							for (int i = 0; supported[i].name; i++)
							{
								supports +=
								    QStringLiteral("+%1 ").arg(QString::fromLatin1(supported[i].name));
								const QString argList = QString::fromLatin1(supported[i].args);
								if (!argList.isEmpty())
								{
									const QStringList items = argList.split(',', Qt::SkipEmptyParts);
									for (const QString &item : items)
										supports += QStringLiteral("+%1.%2 ").arg(
										    QString::fromLatin1(supported[i].name), item);
								}
							}
						}
						else
						{
							for (const QString &raw : args)
							{
								const QStringList parts = raw.split('.', Qt::SkipEmptyParts);
								if (parts.size() > 2)
								{
									mxpError(DBG_ERROR, errMXP_InvalidSupportArgument,
									         QStringLiteral("Invalid <support> argument: %1").arg(raw));
									invalidSupportArgument = true;
									break;
								}
								const QString tagName = parts.value(0).toLower();
								const QString subTag  = parts.value(1).toLower();
								if (!isValidMxpName(tagName) ||
								    (!subTag.isEmpty() && subTag != "*" && !isValidMxpName(subTag)))
								{
									mxpError(DBG_ERROR, errMXP_InvalidSupportArgument,
									         QStringLiteral("Invalid <support> argument: %1").arg(raw));
									invalidSupportArgument = true;
									break;
								}
								bool found = false;
								for (int i = 0; supported[i].name; i++)
								{
									if (tagName == QString::fromLatin1(supported[i].name))
									{
										found = true;
										if (subTag.isEmpty())
										{
											supports += QStringLiteral("+%1 ").arg(tagName);
										}
										else if (subTag == "*")
										{
											const QString argList = QString::fromLatin1(supported[i].args);
											if (!argList.isEmpty())
											{
												const QStringList items =
												    argList.split(',', Qt::SkipEmptyParts);
												for (const QString &item : items)
													supports += QStringLiteral("+%1.%2 ").arg(tagName, item);
											}
										}
										else
										{
											const QString argList   = QString::fromLatin1(supported[i].args);
											const QStringList items = argList.split(',', Qt::SkipEmptyParts);
											if (items.contains(subTag))
												supports += QStringLiteral("+%1.%2 ").arg(tagName, subTag);
											else
												supports += QStringLiteral("-%1.%2 ").arg(tagName, subTag);
										}
										break;
									}
								}
								if (!found)
									supports += QStringLiteral("-%1 ").arg(tagName);
							}
						}

						if (invalidSupportArgument)
							continue;

						const QString packet = QStringLiteral("\x1B[1z<SUPPORTS %1>%2")
						                           .arg(supports.trimmed(), QString::fromLatin1(endLine));
						sendMxpPacket(packet);
						mxpError(DBG_INFO, infoMXP_SupportsSent,
						         QStringLiteral("Sent supports response: %1").arg(packet.mid(4)));
					}
					else if (effectiveTag == "option")
					{
						// From mxpOpenAtomic.cpp: reply with options requested (if any).
						const QStringList args = extractArgumentTokens(attributes, attributesAreInternalUtf8);
						QString           options;
						if (args.isEmpty())
						{
							const WorldNumericOption *list = worldNumericOptions();
							for (int i = 0; list[i].name; i++)
							{
								const QString   key   = QString::fromLatin1(list[i].name);
								const long long value = currentOptionValue(key, &list[i]);
								options += QStringLiteral("%1=%2 ").arg(key).arg(QString::number(value));
							}
						}
						else
						{
							for (const QString &raw : args)
							{
								const QString key = raw.section('=', 0, 0).trimmed();
								if (key.isEmpty())
									continue;
								const WorldNumericOption *opt = QMudWorldOptions::findWorldNumericOption(key);
								const long long           value = opt ? currentOptionValue(key, opt) : 0;
								options += QStringLiteral("%1=%2 ").arg(key).arg(QString::number(value));
							}
						}
						const QString packet = QStringLiteral("\x1B[1z<OPTIONS %1>%2")
						                           .arg(options.trimmed(), QString::fromLatin1(endLine));
						sendMxpPacket(packet);
						mxpError(DBG_INFO, infoMXP_OptionsSent,
						         QStringLiteral("Sent options response: %1").arg(packet.mid(4)));
					}
					else if (effectiveTag == "recommend_option")
					{
						// Server-suggested options (only applies to server-writable options).
						if (!isEnabledFlag(m_worldAttributes.value(QStringLiteral("mud_can_change_options"))))
							continue;

						const QStringList args = extractArgumentTokens(ev.attributes, false);
						for (const QString &raw : args)
						{
							const QString key       = raw.section('=', 0, 0).trimmed();
							const QString valueText = raw.section('=', 1).trimmed();
							if (key.isEmpty() || valueText.isEmpty())
								continue;
							if (!isValidMxpName(key))
							{
								mxpError(DBG_ERROR, errMXP_InvalidOptionArgument,
								         QStringLiteral("Option named '%1' not known.").arg(key));
								continue;
							}
							const WorldNumericOption *opt = QMudWorldOptions::findWorldNumericOption(key);
							if (!opt)
							{
								mxpError(DBG_ERROR, errMXP_InvalidOptionArgument,
								         QStringLiteral("Option named '%1' not known.").arg(key));
								continue;
							}
							if ((opt->flags & OPT_SERVER_CAN_WRITE) == 0)
							{
								mxpError(DBG_ERROR, errMXP_CannotChangeOption,
								         QStringLiteral("Option named '%1' cannot be changed.").arg(key));
								continue;
							}
							bool            ok    = false;
							const long long value = parseOptionValue(valueText, &ok);
							if (!ok)
								continue;
							if (key.compare(QStringLiteral("hyperlink_colour"), Qt::CaseInsensitive) == 0 &&
							    !isEnabledFlag(
							        m_worldAttributes.value(QStringLiteral("mud_can_change_link_colour"))))
							{
								mxpError(DBG_ERROR, errMXP_CannotChangeOption,
								         QStringLiteral("Option named '%1' cannot be changed.").arg(key));
								continue;
							}
							if (key.compare(QStringLiteral("underline_hyperlinks"), Qt::CaseInsensitive) ==
							        0 &&
							    value == 0 &&
							    !isEnabledFlag(
							        m_worldAttributes.value(QStringLiteral("mud_can_remove_underline"))))
							{
								mxpError(DBG_ERROR, errMXP_CannotChangeOption,
								         QStringLiteral("Option named '%1' cannot be changed.").arg(key));
								continue;
							}
							long long const minValue = opt->minValue;
							long long       maxValue = opt->maxValue;
							if (minValue == 0 && maxValue == 0)
								maxValue = 1;
							if (value < minValue || value > maxValue)
							{
								mxpError(DBG_ERROR, errMXP_OptionOutOfRange,
								         QStringLiteral(
								             "Option named '%1' could not be changed to '%2' (out of range).")
								             .arg(key, valueText));
								continue;
							}
							m_worldAttributes.insert(key, QString::number(value));
							if (worldAttributeAffectsResolvedOutputColours(key))
								invalidateResolvedOutputColourCache();
							mxpError(
							    DBG_INFO, infoMXP_OptionChanged,
							    QStringLiteral("Option named '%1' changed to '%2'.").arg(key, valueText));
						}
					}
					else if (effectiveTag == "mxp")
					{
						const QStringList args = extractArgumentTokens(attributes, attributesAreInternalUtf8);
						auto              hasKeyword = [&](const QString &keyword)
						{
							return std::ranges::any_of(
							    args,
							    [&](const QString &arg)
							    {
								    const QString token = arg.section('=', 0, 0).trimmed();
								    return token.compare(keyword, Qt::CaseInsensitive) == 0;
							    });
						};

						if (hasKeyword(QStringLiteral("off")))
						{
							m_telnet.disableMxp();
							continue;
						}
						if (hasKeyword(QStringLiteral("default_open")))
							m_telnet.setMxpDefaultMode(TelnetProcessor::MxpDefaultMode::Open);
						if (hasKeyword(QStringLiteral("default_secure")))
							m_telnet.setMxpDefaultMode(TelnetProcessor::MxpDefaultMode::Secure);
						if (hasKeyword(QStringLiteral("default_locked")))
							m_telnet.setMxpDefaultMode(TelnetProcessor::MxpDefaultMode::Locked);
					}
					else if (effectiveTag == "user" || effectiveTag == "username")
					{
						const QString name = getWorldAttribute(QStringLiteral("player"));
						if (!name.isEmpty() && isConnectMxp())
						{
							const QString packet =
							    QStringLiteral("%1%2").arg(name, QString::fromLatin1(endLine));
							sendMxpPacket(packet);
							mxpError(DBG_INFO, infoMXP_CharacterNameSent,
							         QStringLiteral("Sent character name: %1").arg(name));
						}
						else if (!isConnectMxp())
							mxpError(
							    DBG_WARNING, wrnMXP_CharacterNameRequestedButNotDefined,
							    QStringLiteral("Character name requested but auto-connect not set to MXP."));
						else
							mxpError(DBG_WARNING, wrnMXP_CharacterNameRequestedButNotDefined,
							         QStringLiteral("Character name requested but none defined."));
					}
					else if (effectiveTag == "password" || effectiveTag == "pass")
					{
						if (m_totalLinesSent > 10)
						{
							mxpError(DBG_WARNING, wrnMXP_PasswordNotSent,
							         QStringLiteral("Too many lines sent to MUD - password not sent."));
						}
						else if (isConnectMxp())
						{
							const QString password = getWorldPassword();
							if (!password.isEmpty())
							{
								const QString packet =
								    QStringLiteral("%1%2").arg(password, QString::fromLatin1(endLine));
								sendMxpPacket(packet);
								mxpError(DBG_INFO, infoMXP_PasswordSent,
								         QStringLiteral("Sent password to world."));
							}
							else
								mxpError(DBG_WARNING, wrnMXP_PasswordRequestedButNotDefined,
								         QStringLiteral("Password requested but none defined."));
						}
						else
							mxpError(DBG_WARNING, wrnMXP_PasswordRequestedButNotDefined,
							         QStringLiteral("Password requested but auto-connect not set to MXP."));
					}
				}
			}
			else if (ev.type == TelnetProcessor::MxpEvent::EndTag)
			{
				const QByteArray &closeTag     = tag;
				const bool        endMxpSecure = ev.secure;

				int               openMatchIndex       = -1;
				bool              closeBlockedBySecure = false;
				for (int i = safeQSizeToInt(m_mxpOpenTags.size()) - 1; i >= 0; --i)
				{
					const MxpOpenTag &openTag = m_mxpOpenTags.at(i);
					if (mxpTagsEquivalent(openTag.tag, closeTag))
					{
						openMatchIndex = i;
						break;
					}
					if (!endMxpSecure && openTag.openedSecure)
					{
						mxpError(DBG_WARNING, wrnMXP_OpenTagBlockedBySecureTag,
						         QStringLiteral("Cannot close open MXP tag <%1> - blocked by secure tag <%2>")
						             .arg(tagText, QString::fromLatin1(openTag.tag)));
						closeBlockedBySecure = true;
						break;
					}
				}
				if (closeBlockedBySecure)
					continue;

				if (openMatchIndex < 0)
				{
					mxpError(DBG_WARNING, wrnMXP_OpenTagNotThere,
					         QStringLiteral("Closing MXP tag </%1> does not have corresponding opening tag")
					             .arg(tagText));
					continue;
				}

				if (!endMxpSecure && m_mxpOpenTags.at(openMatchIndex).openedSecure)
				{
					mxpError(DBG_WARNING, wrnMXP_TagOpenedInSecureMode,
					         QStringLiteral("Cannot close open MXP tag <%1> - it was opened in secure mode.")
					             .arg(tagText));
					continue;
				}

				while (m_mxpOpenTags.size() - 1 >= openMatchIndex)
				{
					const MxpOpenTag openTag = m_mxpOpenTags.takeLast();
					if (!mxpTagsEquivalent(openTag.tag, closeTag))
					{
						mxpError(DBG_WARNING, wrnMXP_ClosingOutOfSequenceTag,
						         QStringLiteral("Closing out-of-sequence MXP tag: <%1>")
						             .arg(QString::fromLatin1(openTag.tag)));
					}
				}

				int                 matchIndex = -1;
				QVector<QByteArray> closeTagsToApply;
				QString             closeTagBodyText;
				bool                hasCloseTagBodyText = false;
				for (int i = safeQSizeToInt(m_mxpTagStack.size()) - 1; i >= 0; --i)
				{
					if (mxpTagsEquivalent(m_mxpTagStack.at(i).tag, closeTag))
					{
						matchIndex = i;
						break;
					}
				}

				if (matchIndex >= 0)
				{
					const int eventOffset = mxpBaseOffset + ev.offset;
					while (m_mxpTagStack.size() - 1 >= matchIndex)
					{
						const MxpTagFrame frame = m_mxpTagStack.takeLast();
						QByteArray        fullTextBytes;
						if (eventOffset > frame.contentStart && frame.contentStart >= 0)
							fullTextBytes =
							    m_mxpTextBuffer.mid(frame.contentStart, eventOffset - frame.contentStart);
						QByteArray textBytes = fullTextBytes;
						if (textBytes.size() > 1000)
							textBytes = textBytes.left(1000);
						QString const text          = decodeIncomingIsolatedBytes(textBytes);
						QString const sanitizedText = qmudStripAnsiEscapeCodes(text);

						if (!closeTagCallback.isEmpty() && m_luaCallbacks)
						{
							const QSharedPointer<LuaCallbackEngine> worldLua(
							    m_luaCallbacks, [](LuaCallbackEngine * /*unused*/) {});
							LuaBatchDispatchRequest request;
							request.kind         = LuaBatchDispatchKind::MxpEndTag;
							request.engines      = {worldLua};
							request.functionName = closeTagCallback;
							request.stringArg    = QString::fromLatin1(frame.tag);
							request.stringArg2   = sanitizedText;
							static_cast<void>(queuePluginCallbackDispatch(request, true));
						}

						const QString pluginClosePayload =
						    QStringLiteral("%1,%2").arg(QString::fromLatin1(frame.tag), sanitizedText);
						callPluginCallbacks(QStringLiteral("OnPluginMXPcloseTag"), pluginClosePayload, true);

						if (!frame.variableName.isEmpty())
						{
							const QString variableName = QStringLiteral("mxp_%1").arg(frame.variableName);
							setVariable(variableName, sanitizedText);
							if (!setVarCallback.isEmpty() && m_luaCallbacks)
							{
								const QSharedPointer<LuaCallbackEngine> worldLua(
								    m_luaCallbacks, [](LuaCallbackEngine * /*unused*/) {});
								LuaBatchDispatchRequest request;
								request.kind         = LuaBatchDispatchKind::MxpSetVariable;
								request.engines      = {worldLua};
								request.functionName = setVarCallback;
								request.stringArg    = variableName;
								request.stringArg2   = sanitizedText;
								static_cast<void>(queuePluginCallbackDispatch(request, true));
							}

							const QString pluginVarPayload =
							    QStringLiteral("%1=%2").arg(variableName, sanitizedText);
							callPluginCallbacks(QStringLiteral("OnPluginMXPsetVariable"), pluginVarPayload,
							                    true);
							if (frame.tag == "var" || frame.tag == "v")
								callPluginCallbacks(QStringLiteral("OnPluginMXPsetEntity"), pluginVarPayload,
								                    true);
						}

						if (mxpTagsEquivalent(frame.tag, closeTag))
						{
							closeTagBodyText = sanitizedText;
							if (textBytes.size() != fullTextBytes.size())
								closeTagBodyText =
								    qmudStripAnsiEscapeCodes(decodeIncomingIsolatedBytes(fullTextBytes));
							hasCloseTagBodyText = true;
							if (closeTagsToApply.isEmpty() && !frame.closeTags.isEmpty())
								closeTagsToApply = frame.closeTags;
						}
					}
				}
				const QString *closeTagBodyTextPtr = hasCloseTagBodyText ? &closeTagBodyText : nullptr;
				if (!closeTagsToApply.isEmpty())
				{
					for (int i = safeQSizeToInt(closeTagsToApply.size()) - 1; i >= 0; --i)
						applyEndTag(closeTagsToApply.at(i), closeTagBodyTextPtr);
				}
				else
				{
					applyEndTag(closeTag, closeTagBodyTextPtr);
				}
			}
		}

		drainTelnetAndModeBoundariesBefore(std::numeric_limits<int>::max(), std::numeric_limits<int>::max());

		if (last < processedSize)
			appendStyled(processedView.sliced(last));

		const bool hasActiveMxpRenderContextAfterProcessing =
		    !stack.isEmpty() || !blockStack.isEmpty() || linkOpen || preDepth > 0;
		if (m_mxpTagStack.isEmpty() && !hasActiveMxpRenderContextAfterProcessing)
			m_mxpTextBuffer.clear();

		commitMxpRenderState();
		m_currentActionSource = previousActionSource;
	}
}

void WorldRuntime::finalizeSocketConnectedState()
{
	if (m_socketReadyForWorld)
		return;
	m_connectPhase       = eConnectConnectedToMud;
	const bool convertGA = isEnabledFlag(m_worldAttributes.value(QStringLiteral("convert_ga_to_newline")));
	const int  useMxp    = m_worldAttributes.value(QStringLiteral("use_mxp")).toInt();
	m_telnet.setConvertGAtoNewline(convertGA);
	if (useMxp >= 0)
		m_telnet.setUseMxp(useMxp);
	m_telnet.queueInitialNegotiation(true, convertGA);
	if (const QByteArray outbound = m_telnet.takeOutboundData(); !outbound.isEmpty())
		sendToWorld(outbound);
	m_socketReadyForWorld = true;
	emit connected();
}

bool WorldRuntime::beginStartTlsUpgrade()
{
	if (!m_socket)
		return false;
	QString error;
	if (!m_socket->startClientEncryption(&error))
	{
		if (error.isEmpty())
			error = QStringLiteral("START-TLS upgrade failed.");
		emit socketError(error);
		disconnectFromWorld();
		return false;
	}
	return true;
}

void WorldRuntime::startStartTlsFallbackTimer()
{
	if (!(m_tlsEncryptionEnabled && m_tlsMethod == eTlsStartTls))
		return;
	const quint64 generation = ++m_startTlsFallbackGeneration;
	QTimer::singleShot(500, this,
	                   [this, generation]
	                   {
		                   const StartTlsFallbackContext context = {generation,
		                                                            m_startTlsFallbackGeneration,
		                                                            m_socket != nullptr,
		                                                            m_socket && m_socket->isConnected(),
		                                                            m_socketReadyForWorld,
		                                                            m_tlsEncryptionEnabled,
		                                                            m_tlsMethod};
		                   if (!shouldFallbackToPlainOnStartTlsTimeout(context))
			                   return;
		                   outputText(QStringLiteral("TLS Encryption NOT enabled: Server did not respond to "
		                                             "START-TLS"),
		                              true, true);
		                   m_telnet.setStartTlsEnabled(false);
		                   finalizeSocketConnectedState();
	                   });
}

void WorldRuntime::cancelStartTlsFallbackTimer()
{
	++m_startTlsFallbackGeneration;
}

bool WorldRuntime::connectToWorld(const QString &host, quint16 port)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodTrueOnSuccess(this, [this, host, port] { return connectToWorld(host, port); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::connectToWorld");
	if (!m_socket)
		return false;

	cancelStartTlsFallbackTimer();
	cancelReloadMccpProbeTimeout();
	m_reloadReattachMccpProbePending        = false;
	m_reloadReattachMccpResumePending       = false;
	m_reloadReattachLookProbeSent           = false;
	m_reloadReattachMccpProbeDecisionOffset = 0;
	m_reloadMccpProbeTimeoutPass            = 0;
	m_reloadReattachMccpProbeBuffer.clear();
	m_reloadReattachUseDeferredMxpReplay = false;
	m_reloadReattachMxpProbeEvents.clear();
	m_reloadReattachMxpProbeModeChanges.clear();
	m_proxyAddressString.clear();
	m_proxyAddressV4      = 0;
	m_disconnectOk        = false;
	m_socketReadyForWorld = false;
	m_telnet.resetConnectionState();
	resetAnsiRenderState();
	m_tlsEncryptionEnabled = isEnabledFlag(m_worldAttributes.value(QStringLiteral("tls_encryption")));
	if (m_tlsEncryptionEnabled)
	{
		const int configuredTlsMethod = m_worldAttributes.value(QStringLiteral("tls_method")).toInt();
		if (configuredTlsMethod == eTlsStartTls)
			m_tlsMethod = eTlsStartTls;
		else
			m_tlsMethod = eTlsDirect;
		m_tlsDisableCertificateValidation =
		    isEnabledFlag(m_worldAttributes.value(QStringLiteral("tls_disable_certificate_validation")));
	}
	else
	{
		m_tlsMethod                       = eTlsDirect;
		m_tlsDisableCertificateValidation = false;
	}
	m_telnet.setStartTlsEnabled(m_tlsEncryptionEnabled && m_tlsMethod == eTlsStartTls);
#if !QT_CONFIG(ssl)
	if (m_tlsEncryptionEnabled)
	{
		const QString message =
		    QStringLiteral("World has TLS option enabled but TLS is not enabled in this build.");
		outputText(message, true, true);
		m_connectViaProxy     = false;
		m_connectPhase        = eConnectNotConnected;
		m_socketReadyForWorld = false;
		return false;
	}
#endif
	const bool    useUtf8   = isEnabledFlag(m_worldAttributes.value(QStringLiteral("utf_8")));
	const bool    keepAlive = isEnabledFlag(m_worldAttributes.value(QStringLiteral("send_keep_alives")));
	const int     configuredProxyType = m_worldAttributes.value(QStringLiteral("proxy_type")).toInt();
	const QString proxyServer         = m_worldAttributes.value(QStringLiteral("proxy_server")).trimmed();
	const quint16 proxyPort =
	    static_cast<quint16>(m_worldAttributes.value(QStringLiteral("proxy_port")).toUInt());
	const QString proxyUsername = m_worldAttributes.value(QStringLiteral("proxy_username"));
	const QString proxyPassword = m_worldAttributes.value(QStringLiteral("proxy_password"));
	m_connectViaProxy =
	    (configuredProxyType == eProxyServerSocks4 || configuredProxyType == eProxyServerSocks5) &&
	    !proxyServer.isEmpty() && proxyPort != 0;
	WorldSocketConnectionSettings connectionSettings;
	connectionSettings.useUtf8                         = useUtf8;
	connectionSettings.keepAlive                       = keepAlive;
	connectionSettings.tlsEncryption                   = m_tlsEncryptionEnabled;
	connectionSettings.tlsMethod                       = m_tlsMethod;
	connectionSettings.disableTlsCertificateValidation = m_tlsDisableCertificateValidation;
	connectionSettings.tlsServerName                   = host.trimmed();
	connectionSettings.proxyType     = m_connectViaProxy ? configuredProxyType : eProxyServerNone;
	connectionSettings.proxyServer   = m_connectViaProxy ? proxyServer : QString();
	connectionSettings.proxyPort     = m_connectViaProxy ? proxyPort : 0;
	connectionSettings.proxyUsername = m_connectViaProxy ? proxyUsername : QString();
	connectionSettings.proxyPassword = m_connectViaProxy ? proxyPassword : QString();
	m_socket->applyConnectionSettings(connectionSettings);
	if (m_connectViaProxy)
	{
		QHostAddress proxyAddr;
		if (proxyAddr.setAddress(proxyServer) && proxyAddr.protocol() == QAbstractSocket::IPv4Protocol)
		{
			m_proxyAddressString = proxyAddr.toString();
			m_proxyAddressV4     = proxyAddr.toIPv4Address();
		}
		else
		{
			const QHostInfo info = QHostInfo::fromName(proxyServer);
			if (info.error() == QHostInfo::NoError)
			{
				for (const QHostAddress &candidate : info.addresses())
				{
					if (candidate.protocol() == QAbstractSocket::IPv4Protocol)
					{
						m_proxyAddressString = candidate.toString();
						m_proxyAddressV4     = candidate.toIPv4Address();
						break;
					}
				}
			}
		}
		m_connectPhase = eConnectConnectingToProxy;
	}
	else
	{
		m_connectPhase = eConnectConnectingToMud;
	}
	QString targetHost = host;
	if (m_connectViaProxy && connectionSettings.proxyType == eProxyServerSocks4)
	{
		// SOCKS4 does not support hostnames; resolve to IPv4 first.
		QHostAddress hostAddr;
		if (hostAddr.setAddress(host) && hostAddr.protocol() == QAbstractSocket::IPv4Protocol)
			targetHost = hostAddr.toString();
		else
		{
			const QHostInfo info = QHostInfo::fromName(host);
			if (info.error() == QHostInfo::NoError)
			{
				for (const QHostAddress &candidate : info.addresses())
				{
					if (candidate.protocol() == QAbstractSocket::IPv4Protocol)
					{
						targetHost = candidate.toString();
						break;
					}
				}
			}
		}
		QHostAddress resolvedTarget;
		if (!resolvedTarget.setAddress(targetHost) ||
		    resolvedTarget.protocol() != QAbstractSocket::IPv4Protocol)
		{
			m_connectViaProxy = false;
			m_connectPhase    = eConnectNotConnected;
			emit socketError(
			    QStringLiteral("Unable to resolve an IPv4 address for SOCKS4 connection target."));
			return false;
		}
	}

	const bool ok = m_socket->connectToHost(targetHost, port);
	if (!ok)
	{
		m_connectViaProxy = false;
		m_connectPhase    = eConnectNotConnected;
	}
	return ok;
}

void WorldRuntime::disconnectFromWorld()
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this] { disconnectFromWorld(); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::disconnectFromWorld");
	m_deferredConnectAfterPluginInstallPending = false;
	m_deferredWorldConnectHandlersPending      = false;
	m_deferredConnectHost.clear();
	m_deferredConnectPort = 0;
	if (!m_socket)
		return;
	if (m_connectPhase == eConnectNotConnected || m_connectPhase == eConnectDisconnecting)
		return;
	cancelStartTlsFallbackTimer();
	cancelReloadMccpProbeTimeout();
	m_reloadReattachMccpProbePending        = false;
	m_reloadReattachMccpResumePending       = false;
	m_reloadReattachLookProbeSent           = false;
	m_reloadReattachMccpProbeDecisionOffset = 0;
	m_reloadMccpProbeTimeoutPass            = 0;
	m_reloadReattachMccpProbeBuffer.clear();
	m_reloadReattachUseDeferredMxpReplay = false;
	m_reloadReattachMxpProbeEvents.clear();
	m_reloadReattachMxpProbeModeChanges.clear();
	m_disconnectOk = true;
	m_connectPhase = eConnectDisconnecting;
	m_socket->disconnectFromHost();
}

int WorldRuntime::nativeSocketDescriptor() const
{
	if (!m_socket)
		return -1;
	const qintptr descriptor = m_socket->nativeSocketDescriptor();
	return descriptor < 0 ? -1 : static_cast<int>(descriptor);
}

bool WorldRuntime::adoptConnectedSocketDescriptor(const int descriptor, QString *errorMessage)
{
	if (errorMessage)
		errorMessage->clear();
	if (!m_socket)
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Socket service is unavailable.");
		return false;
	}
	if (descriptor < 0)
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Socket descriptor is invalid.");
		return false;
	}

	const QList<TelnetProcessor::CustomElementInfo> preservedCustomMxpElements =
	    m_telnet.customElementInfos();
	const TelnetProcessor::MxpSessionState preservedMxpSessionState = m_telnet.mxpSessionState();
	const bool                             preservedMxpActive       = m_mxpActive;

	cancelStartTlsFallbackTimer();
	cancelReloadMccpProbeTimeout();
	m_reloadReattachMccpProbePending        = false;
	m_reloadReattachMccpResumePending       = false;
	m_reloadReattachLookProbeSent           = false;
	m_reloadReattachMccpProbeDecisionOffset = 0;
	m_reloadMccpProbeTimeoutPass            = 0;
	m_reloadReattachMccpProbeBuffer.clear();
	m_reloadReattachUseDeferredMxpReplay = false;
	m_reloadReattachMxpProbeEvents.clear();
	m_reloadReattachMxpProbeModeChanges.clear();
	m_disconnectOk        = false;
	m_socketReadyForWorld = false;
	m_telnet.resetConnectionState();
	m_telnet.setStartTlsEnabled(false);
	m_telnet.setCustomElementInfos(preservedCustomMxpElements);
	m_telnet.setMxpSessionState(preservedMxpSessionState);
	m_mxpActive = preservedMxpActive && preservedMxpSessionState.enabled;
	resetAnsiRenderState();
	m_mxpTagStack.clear();
	m_mxpOpenTags.clear();
	m_mxpTextBuffer.clear();
	m_incomingSocketDataPaused = false;

	QString adoptionError;
	if (!m_socket->adoptConnectedSocketDescriptor(static_cast<qintptr>(descriptor), &adoptionError))
	{
		if (errorMessage)
			*errorMessage = adoptionError;
		m_connectViaProxy = false;
		m_connectPhase    = eConnectNotConnected;
		return false;
	}

	m_connectViaProxy                 = false;
	m_connectPhase                    = eConnectConnectedToMud;
	m_tlsEncryptionEnabled            = false;
	m_tlsMethod                       = eTlsDirect;
	m_tlsDisableCertificateValidation = false;
	m_hasCachedIp                     = true;
	m_connectTime                     = QDateTime::currentDateTime();
	const bool convertGA = isEnabledFlag(m_worldAttributes.value(QStringLiteral("convert_ga_to_newline")));
	m_telnet.setConvertGAtoNewline(convertGA);
	m_socketReadyForWorld = true;
	emit connected();
	return true;
}

void WorldRuntime::closeSocketForReloadReconnect()
{
	if (!m_socket)
		return;
	cancelStartTlsFallbackTimer();
	cancelReloadMccpProbeTimeout();
	m_reloadReattachMccpProbePending        = false;
	m_reloadReattachMccpResumePending       = false;
	m_reloadReattachLookProbeSent           = false;
	m_reloadReattachMccpProbeDecisionOffset = 0;
	m_reloadMccpProbeTimeoutPass            = 0;
	m_reloadReattachMccpProbeBuffer.clear();
	m_reloadReattachUseDeferredMxpReplay = false;
	m_reloadReattachMxpProbeEvents.clear();
	m_reloadReattachMxpProbeModeChanges.clear();
	m_incomingSocketDataPaused            = false;
	m_disconnectOk                        = true;
	m_deferredWorldConnectHandlersPending = false;
	m_connectViaProxy                     = false;
	m_connectPhase                        = eConnectNotConnected;
	m_tlsDisableCertificateValidation     = false;
	m_socketReadyForWorld                 = false;
	m_socket->abortSocket();
	m_telnet.resetConnectionState();
	m_telnet.setStartTlsEnabled(false);
	resetAnsiRenderState();
}

void WorldRuntime::setIncomingSocketDataPaused(const bool paused)
{
	m_incomingSocketDataPaused = paused;
}

bool WorldRuntime::incomingSocketDataPaused() const
{
	return m_incomingSocketDataPaused;
}

void WorldRuntime::markReloadReattachConnectActionsSuppressed()
{
	m_reloadReattachSuppressConnectActions = true;
}

bool WorldRuntime::reloadReattachConnectActionsSuppressed() const
{
	return m_reloadReattachSuppressConnectActions;
}

bool WorldRuntime::consumeReloadReattachConnectActionsSuppressed()
{
	const bool suppressed                  = m_reloadReattachSuppressConnectActions;
	m_reloadReattachSuppressConnectActions = false;
	return suppressed;
}

void WorldRuntime::requestMccpResumeAfterReloadReattach()
{
	if (!m_socket || !m_socket->isConnected())
		return;
	if (isCompressing() || mccpType() != 0)
		return;
	if (const bool disableCompression =
	        isEnabledFlag(m_worldAttributes.value(QStringLiteral("disable_compression")));
	    disableCompression)
	{
		return;
	}
	m_telnet.queueEnableCompression2Negotiation();
	if (const QByteArray outbound = m_telnet.takeOutboundData(); !outbound.isEmpty())
		sendToWorld(outbound);
}

void WorldRuntime::configureReloadMccpReattachProbe(const bool enabled)
{
	cancelReloadMccpProbeTimeout();
	m_reloadReattachMccpProbePending        = enabled;
	m_reloadReattachMccpResumePending       = enabled;
	m_reloadReattachLookProbeSent           = false;
	m_reloadReattachMccpProbeDecisionOffset = 0;
	m_reloadMccpProbeTimeoutPass            = 0;
	m_reloadReattachMccpProbeBuffer.clear();
	m_reloadReattachUseDeferredMxpReplay = false;
	m_reloadReattachMxpProbeEvents.clear();
	m_reloadReattachMxpProbeModeChanges.clear();
	if (enabled)
		armReloadMccpProbeTimeout();
}

void WorldRuntime::sendReloadReattachLookProbe()
{
	if (!m_socket || !m_socket->isConnected())
		return;
	if (m_reloadReattachMccpProbePending)
	{
		m_reloadReattachLookProbeSent           = true;
		m_reloadReattachMccpProbeDecisionOffset = m_reloadReattachMccpProbeBuffer.size();
	}
	sendToWorld(QByteArrayLiteral("look\r\n"));
}

void WorldRuntime::armReloadMccpProbeTimeout()
{
	const quint64 generation     = ++m_reloadMccpProbeGeneration;
	m_reloadMccpProbeTimeoutPass = 0;
	armReloadMccpProbeTimeoutPass(generation, 0);
}

void WorldRuntime::armReloadMccpProbeTimeoutPass(const quint64 generation, const int pass)
{
	QTimer::singleShot(
	    500, this,
	    [this, generation, pass]
	    {
		    if (generation != m_reloadMccpProbeGeneration)
			    return;
		    if (!m_reloadReattachMccpProbePending)
			    return;
		    const ReloadMccpProbePayloads payloads =
		        makeReloadMccpProbePayloads(m_reloadReattachMccpProbeBuffer, m_reloadReattachLookProbeSent,
		                                    m_reloadReattachMccpProbeDecisionOffset);
		    const ReloadMccpProbeTimeoutAction timeoutAction =
		        resolveReloadMccpProbeTimeoutAction(payloads.decisionPayload, pass);
		    if (timeoutAction == ReloadMccpProbeTimeoutAction::WaitSecondPass)
		    {
			    m_reloadMccpProbeTimeoutPass = 1;
			    armReloadMccpProbeTimeoutPass(generation, 1);
			    return;
		    }

		    m_reloadReattachMccpProbePending        = false;
		    m_reloadMccpProbeTimeoutPass            = 0;
		    const QByteArray decisionPayload        = payloads.decisionPayload;
		    m_reloadReattachLookProbeSent           = false;
		    m_reloadReattachMccpProbeDecisionOffset = 0;
		    if (timeoutAction == ReloadMccpProbeTimeoutAction::Reconnect)
		    {
			    m_reloadReattachMccpResumePending = false;
			    m_reloadReattachMccpProbeBuffer.clear();
			    m_reloadReattachUseDeferredMxpReplay = false;
			    m_reloadReattachMxpProbeEvents.clear();
			    m_reloadReattachMxpProbeModeChanges.clear();
			    if (decisionPayload.isEmpty())
			    {
				    outputText(QStringLiteral("Reload reattach probe timed out without response. "
				                              "Reconnecting."),
				               true, true);
			    }
			    else
			    {
				    outputText(QStringLiteral("Reload reattach detected active MCCP stream. "
				                              "Reconnecting."),
				               true, true);
			    }
			    const QString host = m_worldAttributes.value(QStringLiteral("site")).trimmed();
			    const quint16 port = m_worldAttributes.value(QStringLiteral("port")).toUShort();
			    closeSocketForReloadReconnect();
			    if (!host.isEmpty() && port != 0)
			    {
				    if (!connectToWorld(host, port))
					    outputText(QStringLiteral("Reload reconnect failed to start after MCCP "
					                              "probe fallback."),
					               true, true);
			    }
			    else
			    {
				    outputText(QStringLiteral("Reload reconnect skipped after MCCP probe fallback "
				                              "due to missing host/port."),
				               true, true);
			    }
			    return;
		    }

		    const bool resumeAfterReplay      = m_reloadReattachMccpResumePending;
		    m_reloadReattachMccpResumePending = false;
		    const QByteArray buffered = takeDeferredReloadMccpProbePayload(&m_reloadReattachMccpProbeBuffer);
		    if (!buffered.isEmpty())
		    {
			    const bool wasSimulated = m_doingSimulate;
			    m_doingSimulate         = true;
			    receiveRawData(buffered);
			    m_doingSimulate = wasSimulated;
		    }
		    if (m_reloadReattachUseDeferredMxpReplay)
		    {
			    m_reloadReattachUseDeferredMxpReplay = false;
			    m_reloadReattachMxpProbeEvents.clear();
			    m_reloadReattachMxpProbeModeChanges.clear();
		    }
		    if (resumeAfterReplay)
			    requestMccpResumeAfterReloadReattach();
	    });
}

void WorldRuntime::cancelReloadMccpProbeTimeout()
{
	++m_reloadMccpProbeGeneration;
	m_reloadMccpProbeTimeoutPass = 0;
}

void WorldRuntime::queueMccpDisableForReload()
{
	if (isMccpDisableCompleteForReload())
		return;

	m_telnet.queueDisableCompressionNegotiation();
	const QByteArray outbound = m_telnet.takeOutboundData();
	if (!outbound.isEmpty())
		sendToWorld(outbound);
}

bool WorldRuntime::isMccpDisableCompleteForReload() const
{
	return !isCompressing() && mccpType() == 0;
}

bool WorldRuntime::requestMccpDisableForReload(const int timeoutMs)
{
	queueMccpDisableForReload();

	QElapsedTimer timer;
	timer.start();
	const int boundedTimeoutMs = qMax(0, timeoutMs);
	while (timer.elapsed() < boundedTimeoutMs)
	{
		QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
		if (isMccpDisableCompleteForReload())
			return true;
	}
	return isMccpDisableCompleteForReload();
}

void WorldRuntime::sendToWorld(const QByteArray &payload)
{
	if (!m_socket)
		return;

	const qint64 written = m_socket->sendPacket(payload);
	if (written <= 0)
		return;

	m_bytesOut += written;
	m_outputPacketCount++;
	if (m_debugIncomingPackets)
	{
		auto appendPacketDebug = [this](const QString &caption, const QByteArray &packet, qint64 number)
		{
			const QString   worldName = m_worldAttributes.value(QStringLiteral("name"));
			const QString   title     = QStringLiteral("Packet debug - %1")
			                                .arg(worldName.isEmpty() ? QStringLiteral("World") : worldName);
			const QDateTime now       = QDateTime::currentDateTime();
			const QString   timestamp =
			    QLocale::system().toString(now, QStringLiteral("dddd, MMMM dd, yyyy, h:mm:ss AP"));
			QString const header = QStringLiteral("\r\n%1 packet: %2 (%3 bytes) at %4\r\n\r\n")
			                           .arg(caption)
			                           .arg(number)
			                           .arg(packet.size())
			                           .arg(timestamp);
			if (!callPluginCallbacksStopOnTrueWithString(QStringLiteral("OnPluginPacketDebug"), header))
			{
				if (AppController const *app = AppController::instance())
					if (MainWindow *main = app->mainWindow())
						main->appendToNotepad(title, header, false, this);
			}

			int remaining = safeQSizeToInt(packet.size());
			int offset    = 0;
			while (remaining > 0)
			{
				const int count = qMin(remaining, kPacketDebugChars);
				QString   ascii;
				QString   hex;
				ascii.reserve(kPacketDebugChars);
				hex.reserve(kPacketDebugChars * 3);
				for (int i = 0; i < count; ++i)
				{
					const auto byte = static_cast<unsigned char>(packet.at(offset + i));
					ascii += (isAsciiPrintableByte(byte) ? QChar(byte) : QChar('.'));
					hex += QString::asprintf(" %02x", byte);
				}
				const QString line = QStringLiteral("%1%2%3\r\n")
				                         .arg(ascii.leftJustified(kPacketDebugChars, QLatin1Char(' ')))
				                         .arg(QLatin1Char(' '))
				                         .arg(hex);
				if (!callPluginCallbacksStopOnTrueWithString(QStringLiteral("OnPluginPacketDebug"), line))
				{
					if (AppController const *app = AppController::instance())
						if (MainWindow *main = app->mainWindow())
							main->appendToNotepad(title, line, false, this);
				}
				remaining -= count;
				offset += count;
			}
		};
		appendPacketDebug(QStringLiteral("Sent "), payload, m_outputPacketCount);
	}
}

void WorldRuntime::incrementLinesSent()
{
	m_totalLinesSent++;
}

int WorldRuntime::totalLinesSent() const
{
	return m_totalLinesSent;
}

int WorldRuntime::totalLinesReceived() const
{
	if (QThread::currentThread() != thread())
		return runtimeCountersSnapshot(false).totalLinesReceived;

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::totalLinesReceived");
	return m_linesReceived;
}

int WorldRuntime::newLines() const
{
	return m_newLines;
}

void WorldRuntime::setNewLines(int value)
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this, value] { setNewLines(value); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::setNewLines");
	m_newLines = value;
}

void WorldRuntime::incrementNewLines()
{
	++m_newLines;
	const bool qtAppFocused  = QGuiApplication::applicationState() == Qt::ApplicationActive;
	bool       windowFocused = qtAppFocused;
	if (const AppController *app = AppController::instance())
	{
		if (const MainWindow *mainWindow = app->mainWindow())
			windowFocused = mainWindow->isApplicationFocused();
	}
	const bool appFocusedForFlash =
	    QMudMainFrameActionUtils::resolveIncomingLineFocusForFlash(qtAppFocused, windowFocused);
	const bool appFocusedForSound =
	    QMudMainFrameActionUtils::resolveIncomingLineFocusForActivitySound(qtAppFocused, windowFocused);
	const bool worldFlashEnabled =
	    isEnabledFlag(m_worldAttributes.value(QStringLiteral("flash_taskbar_icon")));
	if (QMudMainFrameActionUtils::shouldAttemptIncomingLineTaskbarFlash(worldFlashEnabled,
	                                                                    appFocusedForFlash))
	{
		flashTaskbarForView(m_view);
	}

	const bool worldInactive          = !m_active;
	const bool backgroundWindowActive = !appFocusedForSound;
	if (worldInactive || backgroundWindowActive)
	{
		const QString sound = m_worldAttributes.value(QStringLiteral("new_activity_sound")).trimmed();
		if (!sound.isEmpty() && sound.compare(QStringLiteral("(No sound)"), Qt::CaseInsensitive) != 0)
		{
			const bool allowBackground =
			    isEnabledFlag(m_worldAttributes.value(QStringLiteral("play_sounds_in_background")));
			if (appFocusedForSound || allowBackground)
				playSound(0, sound, false, 0.0, 0.0);
		}
	}
}

void WorldRuntime::clearNewLines()
{
	m_newLines = 0;
}

void WorldRuntime::requestActiveState(const bool active)
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodQueued(this, [this, active] { requestActiveState(active); });
		return;
	}
	enqueueActiveStateTransition(active);
}

bool WorldRuntime::isActive() const
{
	return m_active;
}

bool WorldRuntime::doNotShowOutstandingLines() const
{
	const QString flag = m_worldAttributes.value(QStringLiteral("do_not_show_outstanding_lines"));
	return flag == QStringLiteral("1") || flag.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
	       flag.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
}

void WorldRuntime::setCurrentActionSource(unsigned short source)
{
	m_currentActionSource = source;
}

unsigned short WorldRuntime::currentActionSource() const
{
	return m_currentActionSource;
}

void WorldRuntime::incrementUtf8ErrorCount()
{
	++m_utf8ErrorCount;
}

int WorldRuntime::utf8ErrorCount() const
{
	return m_utf8ErrorCount;
}

void WorldRuntime::setLastLineWithIacGa(int lineNumber)
{
	m_lastLineWithIacGa = lineNumber;
}

int WorldRuntime::lastLineWithIacGa() const
{
	return m_lastLineWithIacGa;
}

void WorldRuntime::incrementTriggersMatched()
{
	m_triggersMatchedThisSession++;
}

void WorldRuntime::incrementTriggersEvaluated()
{
	m_triggersEvaluatedCount++;
}

void WorldRuntime::incrementAliasesMatched()
{
	m_aliasesMatchedThisSession++;
}

void WorldRuntime::incrementAliasesEvaluated()
{
	m_aliasesEvaluatedCount++;
}

void WorldRuntime::incrementTimersFired()
{
	m_timersFiredThisSession++;
}

int WorldRuntime::triggersEvaluatedCount() const
{
	return m_triggersEvaluatedCount;
}

int WorldRuntime::triggersMatchedThisSession() const
{
	return m_triggersMatchedThisSession;
}

int WorldRuntime::aliasesEvaluatedCount() const
{
	return m_aliasesEvaluatedCount;
}

int WorldRuntime::aliasesMatchedThisSession() const
{
	return m_aliasesMatchedThisSession;
}

int WorldRuntime::timersFiredThisSession() const
{
	return m_timersFiredThisSession;
}

void WorldRuntime::setLastUserInput(const QDateTime &when)
{
	m_lastUserInput = when;
}

QDateTime WorldRuntime::lastUserInput() const
{
	return m_lastUserInput;
}

int WorldRuntime::addFontFromFile(const QString &path)
{
	const QString trimmed = path.trimmed();
	if (trimmed.isEmpty())
		return eBadParameter;

	QString resolved;
	QString error;
	if (!QMudPluginPathUtils::resolveInsideQmudHome(m_startupDirectory, trimmed, &resolved, &error))
		return eFileNotFound;

	QFileInfo const info(resolved);
	if (!info.exists())
		return eFileNotFound;

	const QString absoluteFontPath = info.absoluteFilePath();
	const QString key =
	    QMudPluginPathUtils::qmudHomeRelativePath(m_startupDirectory, absoluteFontPath, false);
	if (m_specialFontPaths.contains(key))
		return eOK;

	const int fontId = QFontDatabase::addApplicationFont(absoluteFontPath);
	if (fontId < 0)
		return eFileNotFound;

	m_specialFontPaths.insert(key);
	m_specialFontPathOrder.append(key);
	m_specialFontIds.append(fontId);
	return eOK;
}

QString WorldRuntime::firstSpecialFontPath() const
{
	if (m_specialFontPathOrder.isEmpty())
		return {};
	return m_specialFontPathOrder.first();
}

bool WorldRuntime::isConnected() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodTrueOnSuccess(const_cast<WorldRuntime *>(this),
		                                     [this] { return isConnected(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::isConnected");
	return m_connectPhase == eConnectConnectedToMud;
}

bool WorldRuntime::isNawsNegotiated() const
{
	return m_telnet.isNawsNegotiated();
}

int WorldRuntime::outputWrapColumns() const
{
	return m_telnet.windowColumns();
}

int WorldRuntime::connectPhase() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), eConnectNotConnected,
		                          [this] { return connectPhase(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::connectPhase");
	return m_connectPhase;
}

int WorldRuntime::lastPreferencesPage() const
{
	return m_lastPreferencesPage;
}

void WorldRuntime::setLastPreferencesPage(int page)
{
	m_lastPreferencesPage = page;
}

QString WorldRuntime::lastTriggerTreeExpandedGroup() const
{
	return m_lastTriggerTreeExpandedGroup;
}

void WorldRuntime::setLastTriggerTreeExpandedGroup(const QString &group)
{
	m_lastTriggerTreeExpandedGroup = group;
}

QString WorldRuntime::lastAliasTreeExpandedGroup() const
{
	return m_lastAliasTreeExpandedGroup;
}

void WorldRuntime::setLastAliasTreeExpandedGroup(const QString &group)
{
	m_lastAliasTreeExpandedGroup = group;
}

QString WorldRuntime::lastTimerTreeExpandedGroup() const
{
	return m_lastTimerTreeExpandedGroup;
}

void WorldRuntime::setLastTimerTreeExpandedGroup(const QString &group)
{
	m_lastTimerTreeExpandedGroup = group;
}

QDateTime WorldRuntime::connectTime() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QDateTime(),
		                          [this] { return connectTime(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::connectTime");
	return m_connectTime;
}

void WorldRuntime::setDisconnectOk(bool ok)
{
	m_disconnectOk = ok;
}

bool WorldRuntime::disconnectOk() const
{
	return m_disconnectOk;
}

void WorldRuntime::setReconnectOnLinkFailure(bool enabled)
{
	m_reconnectOnLinkFailure = enabled;
}

bool WorldRuntime::reconnectOnLinkFailure() const
{
	return m_reconnectOnLinkFailure;
}

QDateTime WorldRuntime::statusTime() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QDateTime(),
		                          [this] { return statusTime(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::statusTime");
	return m_statusTime;
}

void WorldRuntime::resetStatusTime()
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this] { resetStatusTime(); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::resetStatusTime");
	m_statusTime = QDateTime::currentDateTime();
}

QDateTime WorldRuntime::lastFlushTime() const
{
	return m_lastFlushTime;
}

QDateTime WorldRuntime::scriptFileModTime() const
{
	const QString scriptFile = m_worldAttributes.value(QStringLiteral("script_filename"));
	if (scriptFile.isEmpty())
		return {};
	const QString   workingDir = resolveWorkingDir(m_startupDirectory);
	const QString   path       = makeAbsolutePath(scriptFile, workingDir);
	QFileInfo const info(path);
	if (!info.exists())
		return {};
	return info.lastModified();
}

QDateTime WorldRuntime::clientStartTime() const
{
	return m_clientStartTime;
}

QDateTime WorldRuntime::worldStartTime() const
{
	return m_worldStartTime;
}

void WorldRuntime::setClientStartTime(const QDateTime &when)
{
	m_clientStartTime = when;
}

void WorldRuntime::setWorldFilePath(const QString &path)
{
	m_worldFilePath = QMudPluginPathUtils::qmudHomeRelativePath(m_startupDirectory, path, false);
}

QString WorldRuntime::worldFilePath() const
{
	return m_worldFilePath;
}

WorldRuntime::SaveSnapshot WorldRuntime::buildSaveSnapshot(const QString &fileName) const
{
	SaveSnapshot snapshot;
	snapshot.targetFilePath           = fileName.trimmed();
	snapshot.startupDirectory         = m_startupDirectory;
	snapshot.worldFilePath            = m_worldFilePath;
	snapshot.pluginsDirectory         = m_pluginsDirectory;
	snapshot.worldAttributes          = m_worldAttributes;
	snapshot.worldMultilineAttributes = m_worldMultilineAttributes;
	snapshot.includes                 = m_includes;
	snapshot.triggers                 = m_triggers;
	snapshot.aliases                  = m_aliases;
	snapshot.timers                   = m_timers;
	snapshot.macros                   = m_macros;
	snapshot.variables                = m_variables;
	snapshot.colours                  = m_colours;
	snapshot.keypadEntries            = m_keypadEntries;
	snapshot.printingStyles           = m_printingStyles;
	snapshot.plugins.reserve(m_plugins.size());
	for (const Plugin &plugin : m_plugins)
	{
		SavePluginSnapshot pluginSnapshot;
		pluginSnapshot.attributes = plugin.attributes;
		pluginSnapshot.source     = plugin.source;
		pluginSnapshot.global     = plugin.global;
		snapshot.plugins.push_back(pluginSnapshot);
	}
	return snapshot;
}

bool WorldRuntime::saveStateMatchesSnapshot(const SaveSnapshot &snapshot) const
{
	if (m_worldAttributes != snapshot.worldAttributes ||
	    m_worldMultilineAttributes != snapshot.worldMultilineAttributes ||
	    m_plugins.size() != snapshot.plugins.size())
	{
		return false;
	}

	auto includesEqual = [&](const QList<Include> &current, const QList<Include> &saved) -> bool
	{
		if (current.size() != saved.size())
			return false;
		for (int i = 0; i < current.size(); ++i)
		{
			if (current.at(i).attributes != saved.at(i).attributes)
				return false;
		}
		return true;
	};
	if (!includesEqual(m_includes, snapshot.includes))
		return false;

	auto triggersEqual = [&](const QList<Trigger> &current, const QList<Trigger> &saved) -> bool
	{
		if (current.size() != saved.size())
			return false;
		for (int i = 0; i < current.size(); ++i)
		{
			const Trigger &a = current.at(i);
			const Trigger &b = saved.at(i);
			if (a.attributes != b.attributes || a.children != b.children || a.included != b.included)
				return false;
		}
		return true;
	};
	if (!triggersEqual(m_triggers, snapshot.triggers))
		return false;

	auto aliasesEqual = [&](const QList<Alias> &current, const QList<Alias> &saved) -> bool
	{
		if (current.size() != saved.size())
			return false;
		for (int i = 0; i < current.size(); ++i)
		{
			const Alias &a = current.at(i);
			const Alias &b = saved.at(i);
			if (a.attributes != b.attributes || a.children != b.children || a.included != b.included)
				return false;
		}
		return true;
	};
	if (!aliasesEqual(m_aliases, snapshot.aliases))
		return false;

	auto timersEqual = [&](const QList<Timer> &current, const QList<Timer> &saved) -> bool
	{
		if (current.size() != saved.size())
			return false;
		for (int i = 0; i < current.size(); ++i)
		{
			const Timer &a = current.at(i);
			const Timer &b = saved.at(i);
			if (a.attributes != b.attributes || a.children != b.children || a.included != b.included)
				return false;
		}
		return true;
	};
	if (!timersEqual(m_timers, snapshot.timers))
		return false;

	auto macrosEqual = [&](const QList<Macro> &current, const QList<Macro> &saved) -> bool
	{
		if (current.size() != saved.size())
			return false;
		for (int i = 0; i < current.size(); ++i)
		{
			if (current.at(i).attributes != saved.at(i).attributes ||
			    current.at(i).children != saved.at(i).children)
				return false;
		}
		return true;
	};
	if (!macrosEqual(m_macros, snapshot.macros))
		return false;

	auto variablesEqual = [&](const QList<Variable> &current, const QList<Variable> &saved) -> bool
	{
		if (current.size() != saved.size())
			return false;
		for (int i = 0; i < current.size(); ++i)
		{
			if (current.at(i).attributes != saved.at(i).attributes ||
			    current.at(i).content != saved.at(i).content)
				return false;
		}
		return true;
	};
	if (!variablesEqual(m_variables, snapshot.variables))
		return false;

	auto coloursEqual = [&](const QList<Colour> &current, const QList<Colour> &saved) -> bool
	{
		if (current.size() != saved.size())
			return false;
		for (int i = 0; i < current.size(); ++i)
		{
			if (current.at(i).group != saved.at(i).group ||
			    current.at(i).attributes != saved.at(i).attributes)
				return false;
		}
		return true;
	};
	if (!coloursEqual(m_colours, snapshot.colours))
		return false;

	auto keypadEqual = [&](const QList<Keypad> &current, const QList<Keypad> &saved) -> bool
	{
		if (current.size() != saved.size())
			return false;
		for (int i = 0; i < current.size(); ++i)
		{
			if (current.at(i).attributes != saved.at(i).attributes ||
			    current.at(i).content != saved.at(i).content)
				return false;
		}
		return true;
	};
	if (!keypadEqual(m_keypadEntries, snapshot.keypadEntries))
		return false;

	auto printingEqual = [&](const QList<PrintingStyle> &current, const QList<PrintingStyle> &saved) -> bool
	{
		if (current.size() != saved.size())
			return false;
		for (int i = 0; i < current.size(); ++i)
		{
			if (current.at(i).group != saved.at(i).group ||
			    current.at(i).attributes != saved.at(i).attributes)
				return false;
		}
		return true;
	};
	if (!printingEqual(m_printingStyles, snapshot.printingStyles))
		return false;

	for (int i = 0; i < m_plugins.size(); ++i)
	{
		const Plugin             &plugin = m_plugins.at(i);
		const SavePluginSnapshot &saved  = snapshot.plugins.at(i);
		if (plugin.attributes != saved.attributes || plugin.source != saved.source ||
		    plugin.global != saved.global)
		{
			return false;
		}
	}

	return true;
}

bool WorldRuntime::saveWorldFile(const QString &fileName, QString *error)
{
	const QString trimmed = fileName.trimmed();
	if (trimmed.isEmpty())
	{
		if (error)
			*error = QStringLiteral("No filename specified");
		return false;
	}

	if (m_worldAttributes.value(QStringLiteral("id")).trimmed().isEmpty())
		m_worldAttributes.insert(QStringLiteral("id"), QMudWorldOptionDefaults::generateWorldUniqueId());

	// Preserve ordering: run world/plugin save callbacks before persisting.
	fireWorldSaveHandlers();

	QString resolvedFileName;
	QString pathError;
	if (!QMudPluginPathUtils::resolveInsideQmudHome(m_startupDirectory, trimmed, &resolvedFileName,
	                                                &pathError))
	{
		if (error)
			*error = pathError;
		return false;
	}

	SaveSnapshot const snapshot = buildSaveSnapshot(resolvedFileName);
	if (!writeSaveSnapshot(snapshot, error))
		return false;

	m_worldFilePath =
	    QMudPluginPathUtils::qmudHomeRelativePath(m_startupDirectory, snapshot.targetFilePath, false);
	if (saveStateMatchesSnapshot(snapshot))
	{
		setWorldFileModified(false);
		setVariablesChanged(false);
	}

	return true;
}

void WorldRuntime::saveWorldFileAsync(const QString                             &fileName,
                                      std::function<void(bool, const QString &)> completion)
{
	const QString trimmed = fileName.trimmed();
	if (trimmed.isEmpty())
	{
		if (completion)
			completion(false, QStringLiteral("No filename specified"));
		return;
	}

	if (m_worldAttributes.value(QStringLiteral("id")).trimmed().isEmpty())
		m_worldAttributes.insert(QStringLiteral("id"), QMudWorldOptionDefaults::generateWorldUniqueId());

	// Preserve ordering: run world/plugin save callbacks before persisting.
	fireWorldSaveHandlers();

	QString resolvedFileName;
	QString pathError;
	if (!QMudPluginPathUtils::resolveInsideQmudHome(m_startupDirectory, trimmed, &resolvedFileName,
	                                                &pathError))
	{
		if (completion)
			completion(false, pathError);
		return;
	}

	auto           snapshot = QSharedPointer<SaveSnapshot>::create(buildSaveSnapshot(resolvedFileName));
	QPointer const guard(this);
	auto           completionFn =
	    QSharedPointer<std::function<void(bool, const QString &)>>::create(std::move(completion));

	QThreadPool::globalInstance()->start(
	    [guard, snapshot, completionFn]
	    {
		    QString    saveError;
		    const bool ok = writeSaveSnapshot(*snapshot, &saveError);
		    qmudInvokeMethodQueued(qApp,
		                           [guard, snapshot, completionFn, ok, saveError]
		                           {
			                           if (!guard)
				                           return;

			                           if (ok)
			                           {
				                           guard->m_worldFilePath = QMudPluginPathUtils::qmudHomeRelativePath(
				                               guard->m_startupDirectory, snapshot->targetFilePath, false);
				                           if (guard->saveStateMatchesSnapshot(*snapshot))
				                           {
					                           guard->setWorldFileModified(false);
					                           guard->setVariablesChanged(false);
				                           }
			                           }

			                           if (completionFn && *completionFn)
				                           (*completionFn)(ok, saveError);
		                           });
	    });
}

bool WorldRuntime::writeSaveSnapshot(const SaveSnapshot &snapshot, QString *error)
{
	SaveSnapshot  normalizedSnapshot = snapshot;
	const QString workingDir         = resolveWorkingDir(normalizedSnapshot.startupDirectory);
	const QString worldFilePath      = normalizedSnapshot.worldFilePath.trimmed();
	const QString absoluteWorldFilePath =
	    worldFilePath.isEmpty() ? QString() : makeAbsolutePath(worldFilePath, workingDir);
	const QString worldDir =
	    absoluteWorldFilePath.isEmpty() ? QString() : QFileInfo(absoluteWorldFilePath).absolutePath();
	const QString pluginsDir = normalizedSnapshot.pluginsDirectory.trimmed();

	const auto    normalizePathAttributes = [&](QMap<QString, QString> &attributes)
	{ normalizePathAttributeMapForStorage(attributes, workingDir); };
	const auto normalizeEntryPathAttributes = [&](auto &entries)
	{
		for (auto &entry : entries)
			normalizePathAttributes(entry.attributes);
	};
	const auto normalizeNativePluginSourceForStorage = [](const QString &source) -> QString
	{
		QMudNativePluginRegistry::NativePluginMetadata metadata;
		if (!QMudNativePluginRegistry::metadataForNativeSource(source, metadata))
			return {};
		return metadata.source;
	};

	normalizePathAttributes(normalizedSnapshot.worldAttributes);
	normalizeEntryPathAttributes(normalizedSnapshot.triggers);
	normalizeEntryPathAttributes(normalizedSnapshot.aliases);
	normalizeEntryPathAttributes(normalizedSnapshot.timers);
	normalizeEntryPathAttributes(normalizedSnapshot.macros);
	normalizeEntryPathAttributes(normalizedSnapshot.variables);
	normalizeEntryPathAttributes(normalizedSnapshot.colours);
	normalizeEntryPathAttributes(normalizedSnapshot.keypadEntries);
	normalizeEntryPathAttributes(normalizedSnapshot.printingStyles);

	for (auto &includeEntry : normalizedSnapshot.includes)
	{
		QString includeName = includeEntry.attributes.value(QStringLiteral("name")).trimmed();
		if (includeName.isEmpty())
			continue;
		if (const QString nativeSource = normalizeNativePluginSourceForStorage(includeName);
		    !nativeSource.isEmpty())
		{
			includeEntry.attributes.insert(QStringLiteral("name"), nativeSource);
			continue;
		}
		const QString normalizedInclude = normalizePathForStorage(includeName);
		const bool    hasPortableRoot   = !extractPortableRootRelativePath(normalizedInclude).isEmpty();
		if (!isAbsolutePathLike(normalizedInclude) && !hasPortableRoot && !worldDir.isEmpty())
			includeName = QDir(worldDir).filePath(normalizedInclude);
		includeEntry.attributes.insert(QStringLiteral("name"),
		                               canonicalizePathForStorage(includeName, workingDir));
	}

	for (auto &plugin : normalizedSnapshot.plugins)
	{
		normalizePathAttributes(plugin.attributes);
		QString source = plugin.source.trimmed();
		if (!source.isEmpty())
		{
			if (const QString nativeSource = normalizeNativePluginSourceForStorage(source);
			    !nativeSource.isEmpty())
			{
				plugin.source = nativeSource;
			}
			else
			{
				const QString normalizedSource = normalizePathForStorage(source);
				const bool    hasPortableRoot  = !extractPortableRootRelativePath(normalizedSource).isEmpty();
				if (!isAbsolutePathLike(normalizedSource) && !hasPortableRoot && !pluginsDir.isEmpty())
					source = QDir(pluginsDir).filePath(normalizedSource);
				plugin.source = canonicalizePathForStorage(source, workingDir);
			}
		}

		if (const QString sourceAttr = plugin.attributes.value(QStringLiteral("source")).trimmed();
		    !sourceAttr.isEmpty())
		{
			QString       rewrittenSource = sourceAttr;
			const QString nativeSource    = normalizeNativePluginSourceForStorage(sourceAttr);
			if (!nativeSource.isEmpty())
			{
				rewrittenSource = nativeSource;
			}
			else if (const QString normalizedSource = normalizePathForStorage(sourceAttr);
			         !isAbsolutePathLike(normalizedSource) &&
			         extractPortableRootRelativePath(normalizedSource).isEmpty() && !pluginsDir.isEmpty())
			{
				rewrittenSource = QDir(pluginsDir).filePath(normalizedSource);
			}
			plugin.attributes.insert(QStringLiteral("source"),
			                         !nativeSource.isEmpty()
			                             ? rewrittenSource
			                             : canonicalizePathForStorage(rewrittenSource, workingDir));
		}
	}

	const QString trimmed = normalizedSnapshot.targetFilePath.trimmed();
	if (trimmed.isEmpty())
	{
		if (error)
			*error = QStringLiteral("No filename specified");
		return false;
	}

	const auto &m_worldAttributes          = normalizedSnapshot.worldAttributes;
	const auto &m_worldMultilineAttributes = normalizedSnapshot.worldMultilineAttributes;
	const auto &m_triggers                 = normalizedSnapshot.triggers;
	const auto &m_aliases                  = normalizedSnapshot.aliases;
	const auto &m_timers                   = normalizedSnapshot.timers;
	const auto &m_macros                   = normalizedSnapshot.macros;
	const auto &m_variables                = normalizedSnapshot.variables;
	const auto &m_colours                  = normalizedSnapshot.colours;
	const auto &m_keypadEntries            = normalizedSnapshot.keypadEntries;
	const auto &m_printingStyles           = normalizedSnapshot.printingStyles;
	const auto &m_plugins                  = normalizedSnapshot.plugins;

	QSaveFile   file(trimmed);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		if (error)
			*error = QStringLiteral("Unable to create the requested file.");
		return false;
	}

	QTextStream out(&file);
	out.setEncoding(QStringConverter::Utf8);
	const auto      nl      = QStringLiteral("\r\n");
	const QDateTime now     = QDateTime::currentDateTime();
	const QString   savedOn = QLocale::system().toString(now, QStringLiteral("dddd, MMMM dd, yyyy, h:mm AP"));
	const bool omitDate = isEnabledFlag(m_worldAttributes.value(QStringLiteral("omit_date_from_save_files")));

	out << R"(<?xml version="1.0" encoding="utf-8"?>)" << nl;
	out << "<!DOCTYPE qmud>" << nl;
	if (!omitDate)
		out << "<!-- Saved on " << fixHtmlString(savedOn) << " -->" << nl;
	out << "<!-- QMud version " << kVersionString << " -->" << nl;
	out << "<!-- Written by Panagiotis Kalogiratos -->" << nl;
	out << "<!-- Home Page: https://github.com/Nodens-/QMud -->" << nl;
	out << "<qmud>" << nl;

	out << "<world" << nl;
	saveXmlString(out, nl, "qmud_version", QString::fromLatin1(kVersionString));
	saveXmlNumber(out, nl, "world_file_version", kThisVersion);
	if (!omitDate)
		saveXmlDate(out, nl, "date_saved", now);
	out << nl;

	for (const auto &opt : kAlphaSaveOptions)
	{
		if (!opt.name)
			break;
		if (opt.flags & OPT_MULTLINE)
			continue;
		QString value = m_worldAttributes.value(QString::fromLatin1(opt.name));
		if (value.isEmpty())
			continue;
		if ((opt.flags & OPT_PASSWORD) != 0)
		{
			const QByteArray utf8       = value.toUtf8();
			const QString    base64Name = QString::fromLatin1(opt.name) + QStringLiteral("_base64");
			saveXmlBoolean(out, nl, base64Name.toLatin1().constData(), true);
			value = qmudEncodeBase64Text(utf8, false);
		}
		saveXmlString(out, nl, opt.name, value);
	}

	out << nl;

	for (const auto &opt : kNumericSaveOptions)
	{
		if (!opt.name)
			break;
		const QString key   = QString::fromLatin1(opt.name);
		const QString value = m_worldAttributes.value(key);
		if (const bool isBool = (opt.min == 0 && opt.max == 0); isBool)
		{
			const bool currentValue = isEnabledFlag(value);
			const bool defaultValue = opt.defaultValue != 0;
			if (currentValue != defaultValue)
				saveXmlString(out, nl, opt.name, qmudBoolToYn(currentValue));
			continue;
		}
		if (opt.flags & OPT_RGB_COLOUR)
		{
			const long colour = parseColorRef(value);
			saveXmlColour(out, nl, opt.name, colour);
			continue;
		}
		bool            ok     = false;
		const long long number = value.toLongLong(&ok);
		if (ok)
			saveXmlNumber(out, nl, opt.name, number);
	}

	out << "   > <!-- end of general world attributes -->" << nl;

	for (const auto &opt : kAlphaSaveOptions)
	{
		if (!opt.name)
			break;
		if (!(opt.flags & OPT_MULTLINE))
			continue;
		const QString value = m_worldMultilineAttributes.value(QString::fromLatin1(opt.name));
		if (value.isEmpty())
			continue;
		saveXmlMulti(out, nl, opt.name, value);
	}

	out << nl << "</world>" << nl;

	auto saveHeader = [&](const char *name)
	{
		out << nl << "<!-- " << name << " -->" << nl;
		out << nl << "<" << name << nl;
		saveXmlString(out, nl, "qmud_version", QString::fromLatin1(kVersionString));
		saveXmlNumber(out, nl, "world_file_version", kThisVersion);
		if (!omitDate)
			saveXmlDate(out, nl, "date_saved", now);
		out << "  >" << nl;
	};

	auto                   saveFooter = [&](const char *name) { out << "</" << name << ">" << nl; };

	QList<const Trigger *> triggers;
	for (const auto &tr : m_triggers)
	{
		if (tr.included || isEnabledFlag(tr.attributes.value(QStringLiteral("temporary"))))
			continue;
		triggers.push_back(&tr);
	}
	saveHeader("triggers");
	std::ranges::stable_sort(triggers,
	                         [](const Trigger *a, const Trigger *b)
	                         {
		                         bool      okA  = false;
		                         bool      okB  = false;
		                         const int seqA = a->attributes.value(QStringLiteral("sequence")).toInt(&okA);
		                         const int seqB = b->attributes.value(QStringLiteral("sequence")).toInt(&okB);
		                         const int seqValA = okA ? seqA : 0;
		                         const int seqValB = okB ? seqB : 0;
		                         if (seqValA != seqValB)
			                         return seqValA < seqValB;
		                         return a->attributes.value(QStringLiteral("match")) <
		                                b->attributes.value(QStringLiteral("match"));
	                         });
	for (const auto *tr : triggers)
	{
		out << "  <trigger" << nl;
		auto num = [&](const char *key)
		{
			bool            ok    = false;
			const long long value = tr->attributes.value(QString::fromLatin1(key)).toLongLong(&ok);
			if (ok)
				saveXmlNumber(out, nl, key, value);
		};
		auto boolean = [&](const char *key)
		{ saveXmlBoolean(out, nl, key, isEnabledFlag(tr->attributes.value(QString::fromLatin1(key)))); };
		auto text = [&](const char *key)
		{ saveXmlString(out, nl, key, tr->attributes.value(QString::fromLatin1(key))); };
		num("back_colour");
		boolean("bold");
		num("clipboard_arg");
		{
			bool            ok    = false;
			const long long value = tr->attributes.value(QStringLiteral("custom_colour")).toLongLong(&ok);
			if (ok && value != 0)
				saveXmlNumber(out, nl, "custom_colour", value);
		}
		num("colour_change_type");
		if (const bool triggerEnabled = isEnabledFlag(tr->attributes.value(QStringLiteral("enabled")));
		    triggerEnabled)
			saveXmlBoolean(out, nl, "enabled", true);
		else
			out << "   enabled=\"n\"" << nl;
		boolean("expand_variables");
		text("group");
		boolean("ignore_case");
		boolean("inverse");
		boolean("italic");
		num("lines_to_match");
		boolean("keep_evaluating");
		boolean("make_bold");
		boolean("make_italic");
		boolean("make_underline");
		text("match");
		boolean("match_back_colour");
		boolean("match_bold");
		boolean("match_inverse");
		boolean("match_italic");
		boolean("match_text_colour");
		boolean("match_underline");
		boolean("multi_line");
		text("name");
		boolean("one_shot");
		boolean("omit_from_log");
		boolean("omit_from_output");
		boolean("regexp");
		boolean("repeat");
		text("script");
		num("send_to");
		num("sequence");
		text("sound");
		boolean("sound_if_inactive");
		boolean("lowercase_wildcard");
		boolean("temporary");
		num("text_colour");
		num("user");
		text("variable");
		{
			if (const long otherText =
			        parseColorRef(tr->attributes.value(QStringLiteral("other_text_colour")));
			    otherText)
				saveXmlColour(out, nl, "other_text_colour", otherText);
			if (const long otherBack =
			        parseColorRef(tr->attributes.value(QStringLiteral("other_back_colour")));
			    otherBack)
				saveXmlColour(out, nl, "other_back_colour", otherBack);
		}
		out << "  >" << nl;
		saveXmlMulti(out, nl, "send", tr->children.value(QStringLiteral("send")));
		out << "  </trigger>" << nl;
	}
	saveFooter("triggers");

	QList<const Alias *> aliases;
	for (const auto &al : m_aliases)
	{
		if (al.included || isEnabledFlag(al.attributes.value(QStringLiteral("temporary"))))
			continue;
		aliases.push_back(&al);
	}
	saveHeader("aliases");
	std::ranges::stable_sort(aliases,
	                         [](const Alias *a, const Alias *b)
	                         {
		                         return a->attributes.value(QStringLiteral("match")) <
		                                b->attributes.value(QStringLiteral("match"));
	                         });
	for (const auto *al : aliases)
	{
		out << "  <alias" << nl;
		saveXmlString(out, nl, "name", al->attributes.value(QStringLiteral("name")));
		saveXmlString(out, nl, "script", al->attributes.value(QStringLiteral("script")));
		saveXmlString(out, nl, "match", al->attributes.value(QStringLiteral("match")));
		if (const bool aliasEnabled = isEnabledFlag(al->attributes.value(QStringLiteral("enabled")));
		    aliasEnabled)
			saveXmlBoolean(out, nl, "enabled", true);
		else
			out << "   enabled=\"n\"" << nl;
		saveXmlBoolean(out, nl, "echo_alias",
		               isEnabledFlag(al->attributes.value(QStringLiteral("echo_alias"))));
		saveXmlBoolean(out, nl, "expand_variables",
		               isEnabledFlag(al->attributes.value(QStringLiteral("expand_variables"))));
		saveXmlString(out, nl, "group", al->attributes.value(QStringLiteral("group")));
		saveXmlString(out, nl, "variable", al->attributes.value(QStringLiteral("variable")));
		saveXmlBoolean(out, nl, "omit_from_command_history",
		               isEnabledFlag(al->attributes.value(QStringLiteral("omit_from_command_history"))));
		saveXmlBoolean(out, nl, "omit_from_log",
		               isEnabledFlag(al->attributes.value(QStringLiteral("omit_from_log"))));
		saveXmlBoolean(out, nl, "regexp", isEnabledFlag(al->attributes.value(QStringLiteral("regexp"))));
		{
			bool            ok    = false;
			const long long value = al->attributes.value(QStringLiteral("send_to")).toLongLong(&ok);
			if (ok)
				saveXmlNumber(out, nl, "send_to", value);
		}
		saveXmlBoolean(out, nl, "omit_from_output",
		               isEnabledFlag(al->attributes.value(QStringLiteral("omit_from_output"))));
		saveXmlBoolean(out, nl, "one_shot", isEnabledFlag(al->attributes.value(QStringLiteral("one_shot"))));
		saveXmlBoolean(out, nl, "menu", isEnabledFlag(al->attributes.value(QStringLiteral("menu"))));
		saveXmlBoolean(out, nl, "ignore_case",
		               isEnabledFlag(al->attributes.value(QStringLiteral("ignore_case"))));
		saveXmlBoolean(out, nl, "keep_evaluating",
		               isEnabledFlag(al->attributes.value(QStringLiteral("keep_evaluating"))));
		{
			bool            ok    = false;
			const long long value = al->attributes.value(QStringLiteral("sequence")).toLongLong(&ok);
			if (ok)
				saveXmlNumber(out, nl, "sequence", value);
		}
		saveXmlBoolean(out, nl, "temporary",
		               isEnabledFlag(al->attributes.value(QStringLiteral("temporary"))));
		{
			bool            ok    = false;
			const long long value = al->attributes.value(QStringLiteral("user")).toLongLong(&ok);
			if (ok)
				saveXmlNumber(out, nl, "user", value);
		}
		out << "  >" << nl;
		saveXmlMulti(out, nl, "send", al->children.value(QStringLiteral("send")));
		out << "  </alias>" << nl;
	}
	saveFooter("aliases");

	QList<const Timer *> timers;
	for (const auto &tm : m_timers)
	{
		if (tm.included || isEnabledFlag(tm.attributes.value(QStringLiteral("temporary"))))
			continue;
		timers.push_back(&tm);
	}
	saveHeader("timers");
	for (const auto *tm : timers)
	{
		out << "  <timer ";
		saveXmlString(out, nl, "name", tm->attributes.value(QStringLiteral("name")), true);
		saveXmlString(out, nl, "script", tm->attributes.value(QStringLiteral("script")), true);
		if (const bool timerEnabled = isEnabledFlag(tm->attributes.value(QStringLiteral("enabled")));
		    timerEnabled)
			saveXmlBoolean(out, nl, "enabled", true, true);
		else
			out << "enabled=\"n\" ";

		bool            ok   = false;
		const long long hour = tm->attributes.value(QStringLiteral("hour")).toLongLong(&ok);
		if (ok)
			saveXmlNumber(out, nl, "hour", hour, true);
		const long long minute = tm->attributes.value(QStringLiteral("minute")).toLongLong(&ok);
		if (ok)
			saveXmlNumber(out, nl, "minute", minute, true);
		const double second = tm->attributes.value(QStringLiteral("second")).toDouble(&ok);
		if (ok)
			saveXmlDouble(out, nl, "second", second);

		const long long offsetHour = tm->attributes.value(QStringLiteral("offset_hour")).toLongLong(&ok);
		if (ok)
			saveXmlNumber(out, nl, "offset_hour", offsetHour, true);
		const long long offsetMinute = tm->attributes.value(QStringLiteral("offset_minute")).toLongLong(&ok);
		if (ok)
			saveXmlNumber(out, nl, "offset_minute", offsetMinute, true);
		const double offsetSecond = tm->attributes.value(QStringLiteral("offset_second")).toDouble(&ok);
		if (ok)
			saveXmlDouble(out, nl, "offset_second", offsetSecond);
		const long long sendTo = tm->attributes.value(QStringLiteral("send_to")).toLongLong(&ok);
		if (ok)
			saveXmlNumber(out, nl, "send_to", sendTo);
		saveXmlBoolean(out, nl, "temporary",
		               isEnabledFlag(tm->attributes.value(QStringLiteral("temporary"))));
		const long long user = tm->attributes.value(QStringLiteral("user")).toLongLong(&ok);
		if (ok)
			saveXmlNumber(out, nl, "user", user);

		saveXmlBoolean(out, nl, "at_time", isEnabledFlag(tm->attributes.value(QStringLiteral("at_time"))),
		               true);
		saveXmlString(out, nl, "group", tm->attributes.value(QStringLiteral("group")), true);
		saveXmlString(out, nl, "variable", tm->attributes.value(QStringLiteral("variable")));
		saveXmlBoolean(out, nl, "one_shot", isEnabledFlag(tm->attributes.value(QStringLiteral("one_shot"))),
		               true);
		saveXmlBoolean(out, nl, "omit_from_output",
		               isEnabledFlag(tm->attributes.value(QStringLiteral("omit_from_output"))), true);
		saveXmlBoolean(out, nl, "omit_from_log",
		               isEnabledFlag(tm->attributes.value(QStringLiteral("omit_from_log"))), true);
		saveXmlBoolean(out, nl, "active_closed",
		               isEnabledFlag(tm->attributes.value(QStringLiteral("active_closed"))), true);

		out << ">" << nl;
		saveXmlMulti(out, nl, "send", tm->children.value(QStringLiteral("send")));
		out << nl << "  </timer>" << nl;
	}
	saveFooter("timers");

	saveHeader("macros");
	const QStringList            macroNames = macroDescriptionList();
	QMap<QString, const Macro *> macroMap;
	for (const auto &macro : m_macros)
	{
		const QString name = macro.attributes.value(QStringLiteral("name")).trimmed();
		if (!name.isEmpty())
			macroMap.insert(name, &macro);
	}
	for (const QString &name : macroNames)
	{
		const Macro *macro = macroMap.value(name, nullptr);
		if (!macro)
			continue;
		const QString send = macro->children.value(QStringLiteral("send"));
		if (send.isEmpty())
			continue;
		QString type = macro->attributes.value(QStringLiteral("type")).trimmed();
		if (type.isEmpty())
			type = QStringLiteral("replace");
		if (type != QStringLiteral("replace") && type != QStringLiteral("send_now") &&
		    type != QStringLiteral("insert"))
			type = QStringLiteral("unknown");
		out << nl << "  <macro ";
		out << "name=\"" << fixHtmlString(name) << "\" ";
		out << "type=\"" << fixHtmlString(type) << "\" ";
		out << ">" << nl;
		out << "  <send>" << fixHtmlMultilineString(send) << "</send>";
		out << nl << "  </macro>" << nl;
	}
	saveFooter("macros");

	saveHeader("variables");
	QList<Variable> variables = m_variables;
	std::ranges::stable_sort(
	    variables, [](const Variable &a, const Variable &b)
	    { return a.attributes.value(QStringLiteral("name")) < b.attributes.value(QStringLiteral("name")); });
	for (const auto &var : variables)
	{
		const QString name = var.attributes.value(QStringLiteral("name"));
		out << "  <variable name=\"" << fixHtmlString(name) << "\">" << fixHtmlMultilineString(var.content)
		    << "</variable>" << nl;
	}
	out << "</variables>" << nl;

	saveHeader("colours");
	auto parseColourValue = [](const QString &value) -> QColor
	{
		if (value.isEmpty())
			return {};
		QColor color(value);
		if (color.isValid())
			return color;
		bool      ok      = false;
		const int numeric = value.toInt(&ok);
		if (!ok)
			return {};
		const int r = numeric & 0xFF;
		const int g = (numeric >> 8) & 0xFF;
		const int b = (numeric >> 16) & 0xFF;
		return {r, g, b};
	};

	QVector<QColor> normalAnsi(8);
	QVector<QColor> boldAnsi(8);
	QVector<QColor> customText(16);
	QVector<QColor> customBack(16);
	QStringList     customNames;
	customNames.reserve(16);
	for (int i = 0; i < 16; ++i)
		customNames.push_back(QStringLiteral("Custom%1").arg(i + 1));

	normalAnsi[0] = QColor(0, 0, 0);
	normalAnsi[1] = QColor(128, 0, 0);
	normalAnsi[2] = QColor(0, 128, 0);
	normalAnsi[3] = QColor(128, 128, 0);
	normalAnsi[4] = QColor(0, 0, 128);
	normalAnsi[5] = QColor(128, 0, 128);
	normalAnsi[6] = QColor(0, 128, 128);
	normalAnsi[7] = QColor(192, 192, 192);
	boldAnsi[0]   = QColor(128, 128, 128);
	boldAnsi[1]   = QColor(255, 0, 0);
	boldAnsi[2]   = QColor(0, 255, 0);
	boldAnsi[3]   = QColor(255, 255, 0);
	boldAnsi[4]   = QColor(0, 0, 255);
	boldAnsi[5]   = QColor(255, 0, 255);
	boldAnsi[6]   = QColor(0, 255, 255);
	boldAnsi[7]   = QColor(255, 255, 255);
	for (int i = 0; i < customText.size(); ++i)
	{
		customText[i] = QColor(255, 255, 255);
		customBack[i] = QColor(0, 0, 0);
	}
	customText[0]  = QColor(255, 128, 128);
	customText[1]  = QColor(255, 255, 128);
	customText[2]  = QColor(128, 255, 128);
	customText[3]  = QColor(128, 255, 255);
	customText[4]  = QColor(0, 128, 255);
	customText[5]  = QColor(255, 128, 192);
	customText[6]  = QColor(255, 0, 0);
	customText[7]  = QColor(0, 128, 192);
	customText[8]  = QColor(255, 0, 255);
	customText[9]  = QColor(128, 64, 64);
	customText[10] = QColor(255, 128, 64);
	customText[11] = QColor(0, 128, 128);
	customText[12] = QColor(0, 64, 128);
	customText[13] = QColor(255, 0, 128);
	customText[14] = QColor(0, 128, 0);
	customText[15] = QColor(0, 0, 255);

	for (const auto &colour : m_colours)
	{
		const QString group = colour.group.trimmed().toLower();
		bool          ok    = false;
		const int     seq   = colour.attributes.value(QStringLiteral("seq")).toInt(&ok);
		const int     index = ok ? seq - 1 : -1;
		if (index < 0)
			continue;
		if (group == QStringLiteral("ansi/normal") && index < normalAnsi.size())
		{
			const QColor rgb = parseColourValue(colour.attributes.value(QStringLiteral("rgb")));
			if (rgb.isValid())
				normalAnsi[index] = rgb;
		}
		else if (group == QStringLiteral("ansi/bold") && index < boldAnsi.size())
		{
			const QColor rgb = parseColourValue(colour.attributes.value(QStringLiteral("rgb")));
			if (rgb.isValid())
				boldAnsi[index] = rgb;
		}
		else if ((group == QStringLiteral("custom/custom") || group == QStringLiteral("custom")) &&
		         index < customText.size())
		{
			const QColor text = parseColourValue(colour.attributes.value(QStringLiteral("text")));
			const QColor back = parseColourValue(colour.attributes.value(QStringLiteral("back")));
			if (text.isValid())
				customText[index] = text;
			if (back.isValid())
				customBack[index] = back;
			const QString name = colour.attributes.value(QStringLiteral("name")).trimmed();
			if (!name.isEmpty())
				customNames[index] = name;
		}
	}

	out << nl << "<ansi>" << nl;
	out << nl << " <normal>" << nl;
	for (int i = 0; i < normalAnsi.size(); ++i)
	{
		out << "   <colour ";
		saveXmlNumber(out, nl, "seq", i + 1, true);
		saveXmlColour(out, nl, "rgb", colorToLong(normalAnsi[i]), true);
		out << "/>" << nl;
	}
	out << nl << " </normal>" << nl;

	out << nl << " <bold>" << nl;
	for (int i = 0; i < boldAnsi.size(); ++i)
	{
		out << "   <colour ";
		saveXmlNumber(out, nl, "seq", i + 1, true);
		saveXmlColour(out, nl, "rgb", colorToLong(boldAnsi[i]), true);
		out << "/>" << nl;
	}
	out << nl << " </bold>" << nl;
	out << nl << "</ansi>" << nl;

	out << nl << "<custom>" << nl;
	for (int i = 0; i < customText.size(); ++i)
	{
		out << "  <colour ";
		saveXmlNumber(out, nl, "seq", i + 1, true);
		saveXmlString(out, nl, "name", customNames.value(i), true);
		saveXmlColour(out, nl, "text", colorToLong(customText[i]), true);
		saveXmlColour(out, nl, "back", colorToLong(customBack[i]), true);
		out << "/>" << nl;
	}
	out << nl << "</custom>" << nl;
	saveFooter("colours");

	const QStringList      keypadNames = keypadNameList();
	QMap<QString, QString> keypadMap;
	for (const auto &key : m_keypadEntries)
		keypadMap.insert(key.attributes.value(QStringLiteral("name")), key.content);
	saveHeader("keypad");
	for (const QString &keyName : keypadNames)
	{
		out << nl << "  <key ";
		saveXmlString(out, nl, "name", keyName, true);
		out << ">" << nl;
		saveXmlMulti(out, nl, "send", keypadMap.value(keyName));
		out << "  </key>" << nl;
	}
	saveFooter("keypad");

	saveHeader("printing");
	struct StyleState
	{
			bool bold{false};
			bool italic{false};
			bool underline{false};
	};
	QVector<StyleState> normalStyles(8);
	QVector<StyleState> boldStyles(8);

	for (const auto &style : m_printingStyles)
	{
		const QString group = style.group.trimmed().toLower();
		bool          ok    = false;
		const int     seq   = style.attributes.value(QStringLiteral("seq")).toInt(&ok);
		const int     index = ok ? seq - 1 : -1;
		if (index < 0 || index >= normalStyles.size())
			continue;
		StyleState state;
		state.bold      = isEnabledFlag(style.attributes.value(QStringLiteral("bold")));
		state.italic    = isEnabledFlag(style.attributes.value(QStringLiteral("italic")));
		state.underline = isEnabledFlag(style.attributes.value(QStringLiteral("underline")));
		if (group == QStringLiteral("ansi/bold"))
			boldStyles[index] = state;
		else if (group == QStringLiteral("ansi/normal"))
			normalStyles[index] = state;
	}

	out << nl << "<ansi>" << nl;
	out << nl << " <normal>" << nl;
	for (int i = 0; i < normalStyles.size(); ++i)
	{
		const StyleState &state = normalStyles[i];
		if (!state.bold && !state.italic && !state.underline)
			continue;
		out << "   <style ";
		saveXmlNumber(out, nl, "seq", i + 1, true);
		saveXmlBoolean(out, nl, "bold", state.bold, true);
		saveXmlBoolean(out, nl, "italic", state.italic, true);
		saveXmlBoolean(out, nl, "underline", state.underline, true);
		out << "/>" << nl;
	}
	out << nl << " </normal>" << nl;

	out << nl << " <bold>" << nl;
	for (int i = 0; i < boldStyles.size(); ++i)
	{
		const StyleState &state = boldStyles[i];
		if (!state.bold && !state.italic && !state.underline)
			continue;
		out << "   <style ";
		saveXmlNumber(out, nl, "seq", i + 1, true);
		saveXmlBoolean(out, nl, "bold", state.bold, true);
		saveXmlBoolean(out, nl, "italic", state.italic, true);
		saveXmlBoolean(out, nl, "underline", state.underline, true);
		out << "/>" << nl;
	}
	out << nl << " </bold>" << nl;
	out << nl << "</ansi>" << nl;
	saveFooter("printing");

	bool wrotePlugins = false;
	for (const auto &plugin : m_plugins)
	{
		if (plugin.global)
			continue;
		QString source = plugin.attributes.value(QStringLiteral("source")).trimmed();
		if (source.isEmpty())
			source = plugin.source.trimmed();
		if (source.isEmpty())
			continue;
		if (!wrotePlugins)
		{
			out << nl << "<!-- plugins -->" << nl;
			wrotePlugins = true;
		}
		source = normalizePathForStorage(source);
		out << "<include ";
		saveXmlString(out, nl, "name", source, true);
		saveXmlBoolean(out, nl, "plugin", true, true);
		if (!isEnabledFlag(plugin.attributes.value(QStringLiteral("enabled"))) &&
		    plugin.attributes.contains(QStringLiteral("enabled")))
			out << "enabled=\"n\" ";
		out << "/>" << nl;
	}

	out << "</qmud>" << nl;

	if (!file.commit())
	{
		if (error)
			*error = QStringLiteral("Unable to create the requested file.");
		return false;
	}

	return true;
}

void WorldRuntime::setPluginsDirectory(const QString &path)
{
	m_pluginsDirectory = QMudPluginPathUtils::qmudHomeRelativePath(m_startupDirectory, path, true);
}

QString WorldRuntime::pluginsDirectory() const
{
	return m_pluginsDirectory;
}

void WorldRuntime::setStateFilesDirectory(const QString &path)
{
	m_stateFilesDirectory = QMudPluginPathUtils::qmudHomeRelativePath(m_startupDirectory, path, true);
}

QString WorldRuntime::stateFilesDirectory() const
{
	return m_stateFilesDirectory;
}

void WorldRuntime::setFileBrowsingDirectory(const QString &path)
{
	m_fileBrowsingDirectory = QMudPluginPathUtils::qmudHomeRelativePath(m_startupDirectory, path, true);
}

QString WorldRuntime::fileBrowsingDirectory() const
{
	return m_fileBrowsingDirectory;
}

void WorldRuntime::setPreferencesDatabaseName(const QString &path)
{
	m_preferencesDatabaseName = QMudPluginPathUtils::qmudHomeRelativePath(m_startupDirectory, path, false);
}

QString WorldRuntime::preferencesDatabaseName() const
{
	return m_preferencesDatabaseName;
}

void WorldRuntime::setTranslatorFile(const QString &path)
{
	m_translatorFile = QMudPluginPathUtils::qmudHomeRelativePath(m_startupDirectory, path, false);
}

QString WorldRuntime::translatorFile() const
{
	return m_translatorFile;
}

void WorldRuntime::setLocale(const QString &value)
{
	m_locale = value;
}

QString WorldRuntime::locale() const
{
	return m_locale;
}

void WorldRuntime::setFixedPitchFont(const QString &value)
{
	m_fixedPitchFont = value;
}

void WorldRuntime::applyDefaultWorldOptions()
{
	QMudWorldOptionDefaults::applyWorldOptionDefaults(m_worldAttributes, m_worldMultilineAttributes);
}

QString WorldRuntime::fixedPitchFont() const
{
	return m_fixedPitchFont;
}

void WorldRuntime::setStatusMessage(const QString &value)
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this, value] { setStatusMessage(value); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::setStatusMessage");
	m_statusMessage = value;
}

QString WorldRuntime::statusMessage() const
{
	return m_statusMessage;
}

void WorldRuntime::setWordUnderMenu(const QString &value, const bool resolved)
{
	m_wordUnderMenu         = value;
	m_wordUnderMenuResolved = resolved;
}

QString WorldRuntime::wordUnderMenu() const
{
	return m_wordUnderMenu;
}

bool WorldRuntime::wordUnderMenuResolved() const
{
	return m_wordUnderMenuResolved;
}

void WorldRuntime::setDebugIncomingPackets(bool enabled)
{
	m_debugIncomingPackets = enabled;
}

bool WorldRuntime::debugIncomingPackets() const
{
	return m_debugIncomingPackets;
}

void WorldRuntime::setLastImmediateExpression(const QString &value)
{
	m_lastImmediateExpression = value;
}

QString WorldRuntime::lastImmediateExpression() const
{
	return m_lastImmediateExpression;
}

void WorldRuntime::setVariablesChanged(bool changed)
{
	m_variablesChanged = changed;
}

bool WorldRuntime::variablesChanged() const
{
	return m_variablesChanged;
}

void WorldRuntime::setLineOmittedFromOutput(bool omitted)
{
	m_lineOmittedFromOutput = omitted;
}

bool WorldRuntime::lineOmittedFromOutput() const
{
	return m_lineOmittedFromOutput;
}

void WorldRuntime::addRecentLine(const QString &line)
{
	constexpr int kMaxRecentLines = 200;
	m_recentLines.push_back(line);
	if (m_recentLines.size() > kMaxRecentLines)
		m_recentLines.pop_front();
	invalidateLuaCallbackLineBufferSnapshot();
}

QStringList WorldRuntime::recentLines(int maxCount) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QStringList(),
		                          [this, maxCount] { return recentLines(maxCount); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::recentLines");
	if (maxCount <= 0 || maxCount >= m_recentLines.size())
		return m_recentLines;
	return m_recentLines.mid(m_recentLines.size() - maxCount);
}

void WorldRuntime::clearRecentLines()
{
	m_recentLines.clear();
	invalidateLuaCallbackLineBufferSnapshot();
}

void WorldRuntime::bookmarkLine(int lineNumber, bool set)
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this, lineNumber, set] { bookmarkLine(lineNumber, set); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::bookmarkLine");
	if (m_sessionStateOutputBufferSealed)
		return;
	if (lineNumber <= 0 || lineNumber > m_lines.size())
		return;

	LineEntry &entry = m_lines[lineNumber - 1];
	if (set)
		entry.flags |= LineBookmark;
	else
		entry.flags &= ~LineBookmark;
	invalidateLuaCallbackLineBufferSnapshot();
}

void WorldRuntime::setStopTriggerEvaluation(StopTriggerEvaluation mode)
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this, mode] { setStopTriggerEvaluation(mode); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::setStopTriggerEvaluation");
	m_stopTriggerEvaluation = mode;
}

WorldRuntime::StopTriggerEvaluation WorldRuntime::stopTriggerEvaluation() const
{
	return m_stopTriggerEvaluation;
}

LuaCallbackEngine *WorldRuntime::luaCallbacks() const
{
	return m_luaCallbacks;
}

const ILuaExecutor *WorldRuntime::luaExecutor() const
{
	return m_luaExecutor.get();
}

void WorldRuntime::invalidateLuaCallbackLineBufferSnapshot() const
{
	++m_luaCallbackLineBufferSnapshotGeneration;
	m_luaCallbackLineBufferSnapshotCache.clear();
	m_luaCallbackLineBufferSnapshotCacheGeneration = 0;
	m_luaCallbackRecentLineBufferSnapshotCache.clear();
	m_luaCallbackRecentLineBufferSnapshotCacheGeneration = 0;
	m_luaCallbackRecentTextLineBufferSnapshotCache.clear();
	m_luaCallbackRecentTextLineBufferSnapshotCacheGeneration = 0;
}

void WorldRuntime::invalidateLuaCallbackDispatchSnapshot() const
{
	++m_luaCallbackDispatchSnapshotGeneration;
	m_luaCallbackDispatchSnapshotBaseCache.clear();
	m_luaCallbackDispatchSnapshotCacheGeneration = 0;
}

QSharedPointer<LuaCallbackMiniWindowSnapshot> WorldRuntime::cloneLuaCallbackDispatchSnapshotBase() const
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::cloneLuaCallbackDispatchSnapshotBase");
	if (!m_luaCallbackDispatchSnapshotBaseCache ||
	    m_luaCallbackDispatchSnapshotCacheGeneration != m_luaCallbackDispatchSnapshotGeneration)
	{
		return {};
	}

	return QSharedPointer<LuaCallbackMiniWindowSnapshot>::create(*m_luaCallbackDispatchSnapshotBaseCache);
}

void WorldRuntime::clearLuaCallbackDispatchVolatileSnapshot(LuaCallbackMiniWindowSnapshot &snapshot)
{
	snapshot.framePointer    = nullptr;
	snapshot.hasFramePointer = false;

	snapshot.hasCallbackOutputAnchor            = false;
	snapshot.callbackOutputAnchorBufferIndex    = 0;
	snapshot.callbackOutputAnchorAbsoluteNumber = 0;
	snapshot.hasLineBufferSnapshot              = false;
	snapshot.lineBufferCount                    = 0;
	snapshot.lineBufferSnapshot.clear();
	snapshot.lineEntriesByBufferIndex.clear();
	snapshot.hasLineBufferDeltaSnapshot = false;
	snapshot.hasLineBufferCountDelta    = false;
	snapshot.lineBufferDeltaCount       = 0;
	snapshot.lineEntryDeltasByBufferIndex.clear();
	snapshot.missingLineEntryDeltasByBufferIndex.clear();
	snapshot.hasRecentLinesSnapshot = false;
	snapshot.recentLinesSnapshot.clear();

	snapshot.hasCommandUiSnapshot        = false;
	snapshot.commandUiHasView            = false;
	snapshot.commandUiHasFrameData       = false;
	snapshot.commandUiOutputClientHeight = 0;
	snapshot.commandUiOutputClientWidth  = 0;
	snapshot.commandUiViewHeight         = 0;
	snapshot.commandUiViewWidth          = 0;
	snapshot.commandUiValues.clear();

	snapshot.hasRuntimeCountersSnapshot = false;
	snapshot.runtimeOutputFontHeight    = 0;
	snapshot.runtimeOutputFontWidth     = 0;
	snapshot.runtimeCounterValues.clear();

	snapshot.hasEntitySnapshot = false;
	snapshot.entityValuesByName.clear();

	snapshot.hasWindowOutputTextRenderSnapshot = false;
	snapshot.windowOutputTextAnsiStreamState   = QMudAnsiStreamState{};
	snapshot.windowOutputTextAnsiRenderState   = LuaCallbackAnsiRenderStateSnapshot{};
	snapshot.windowOutputTextMxpStyleState     = LuaCallbackMxpStyleStateSnapshot{};
	snapshot.windowOutputTextMxpStyleStack.clear();
	snapshot.windowOutputTextMxpBlockStack.clear();
	snapshot.windowOutputTextMxpLinkOpen = false;
	snapshot.windowOutputTextMxpPreDepth = 0;
	snapshot.windowOutputTextCustomElements.clear();

	snapshot.hasUiSnapshot = false;
	snapshot.guiSystemValues.clear();
	snapshot.hasClipboardText = false;
	snapshot.clipboardText.clear();
	snapshot.mainWindowPositionsByMode.clear();
	snapshot.mainWindowPositionsDirty = false;
	snapshot.worldWindowPositionsByKey.clear();
	snapshot.missingWorldWindowPositionKeys.clear();
	snapshot.dirtyWorldWindowPositionOrdinals.clear();
	snapshot.notepadWindowPositionsByKey.clear();
	snapshot.missingNotepadWindowPositionKeys.clear();
	snapshot.dirtyNotepadWindowPositionKeys.clear();
	snapshot.dirtyNotepadDocumentKeys.clear();
	snapshot.notepadLengthByKey.clear();
	snapshot.missingNotepadLengthKeys.clear();
	snapshot.notepadTextByKey.clear();
	snapshot.missingNotepadTextKeys.clear();
	snapshot.dirtyNotepadListKeys.clear();
	snapshot.notepadListByKey.clear();
	snapshot.missingNotepadListKeys.clear();
	snapshot.worldRuntimeSnapshot.clear();
	snapshot.worldWindowPositionSnapshot.clear();
	snapshot.notepadSnapshot.clear();
}

void WorldRuntime::populateLuaCallbackDispatchVolatileSnapshot(
    LuaCallbackMiniWindowSnapshot &snapshot, const LuaCallbackLineSnapshotPolicy lineSnapshotPolicy) const
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::populateLuaCallbackDispatchVolatileSnapshot");
	clearLuaCallbackDispatchVolatileSnapshot(snapshot);
	refreshMiniWindowMouseStateForLuaDispatch(snapshot, m_miniWindows);

	const int callbackOutputAnchorBufferIndex = luaContextLinesInBufferCount();
	if (callbackOutputAnchorBufferIndex > 0)
	{
		LineEntry callbackOutputAnchorEntry;
		if (luaContextLineEntry(callbackOutputAnchorBufferIndex, callbackOutputAnchorEntry) &&
		    callbackOutputAnchorEntry.lineNumber > 0)
		{
			snapshot.hasCallbackOutputAnchor            = true;
			snapshot.callbackOutputAnchorBufferIndex    = callbackOutputAnchorBufferIndex;
			snapshot.callbackOutputAnchorAbsoluteNumber = callbackOutputAnchorEntry.lineNumber;
		}
	}

	const CommandUiSnapshot commandUi    = commandUiSnapshot(false, true, false);
	snapshot.hasCommandUiSnapshot        = true;
	snapshot.commandUiHasView            = commandUi.hasView;
	snapshot.commandUiHasFrameData       = commandUi.hasFrameData;
	snapshot.commandUiOutputClientHeight = commandUi.outputClientHeight;
	snapshot.commandUiOutputClientWidth  = commandUi.outputClientWidth;
	snapshot.commandUiViewHeight         = commandUi.viewHeight;
	snapshot.commandUiViewWidth          = commandUi.viewWidth;
	snapshot.commandUiValues.reserve(53);
	snapshot.commandUiValues.insert(QStringLiteral("queuedCommands"), commandUi.queuedCommands);
	snapshot.commandUiValues.insert(QStringLiteral("triggerPriorityQueuedCommandCount"),
	                                commandUi.triggerPriorityQueuedCommandCount);
	snapshot.commandUiValues.insert(QStringLiteral("commandInputText"), commandUi.commandInputText);
	snapshot.commandUiValues.insert(QStringLiteral("inputSelectionStartColumn"),
	                                commandUi.inputSelectionStartColumn);
	snapshot.commandUiValues.insert(QStringLiteral("inputSelectionEndColumn"),
	                                commandUi.inputSelectionEndColumn);
	snapshot.commandUiValues.insert(QStringLiteral("outputSelectionEndColumn"),
	                                commandUi.outputSelectionEndColumn);
	snapshot.commandUiValues.insert(QStringLiteral("outputSelectionEndLine"),
	                                commandUi.outputSelectionEndLine);
	snapshot.commandUiValues.insert(QStringLiteral("outputSelectionStartColumn"),
	                                commandUi.outputSelectionStartColumn);
	snapshot.commandUiValues.insert(QStringLiteral("outputSelectionStartLine"),
	                                commandUi.outputSelectionStartLine);
	snapshot.commandUiValues.insert(QStringLiteral("textRectangleLeft"), commandUi.textRectangleLeft);
	snapshot.commandUiValues.insert(QStringLiteral("textRectangleTop"), commandUi.textRectangleTop);
	snapshot.commandUiValues.insert(QStringLiteral("textRectangleRight"), commandUi.textRectangleRight);
	snapshot.commandUiValues.insert(QStringLiteral("textRectangleBottom"), commandUi.textRectangleBottom);
	snapshot.commandUiValues.insert(QStringLiteral("textRectangleBorderOffset"),
	                                commandUi.textRectangleBorderOffset);
	snapshot.commandUiValues.insert(QStringLiteral("textRectangleBorderWidth"),
	                                commandUi.textRectangleBorderWidth);
	snapshot.commandUiValues.insert(QStringLiteral("textRectangleOutsideFillColour"),
	                                commandUi.textRectangleOutsideFillColour);
	snapshot.commandUiValues.insert(QStringLiteral("textRectangleOutsideFillStyle"),
	                                commandUi.textRectangleOutsideFillStyle);
	snapshot.commandUiValues.insert(QStringLiteral("textRectangleBorderColour"),
	                                commandUi.textRectangleBorderColour);
	snapshot.commandUiValues.insert(QStringLiteral("hasView"), commandUi.hasView);
	snapshot.commandUiValues.insert(QStringLiteral("hasFrameData"), commandUi.hasFrameData);
	snapshot.commandUiValues.insert(QStringLiteral("outputScrollBarWanted"), commandUi.outputScrollBarWanted);
	snapshot.commandUiValues.insert(QStringLiteral("outputScrollPosition"), commandUi.outputScrollPosition);
	snapshot.commandUiValues.insert(QStringLiteral("outputClientHeight"), commandUi.outputClientHeight);
	snapshot.commandUiValues.insert(QStringLiteral("outputClientWidth"), commandUi.outputClientWidth);
	snapshot.commandUiValues.insert(QStringLiteral("viewHeight"), commandUi.viewHeight);
	snapshot.commandUiValues.insert(QStringLiteral("viewWidth"), commandUi.viewWidth);
	snapshot.commandUiValues.insert(QStringLiteral("outputTextRectLeft"), commandUi.outputTextRectLeft);
	snapshot.commandUiValues.insert(QStringLiteral("outputTextRectTop"), commandUi.outputTextRectTop);
	snapshot.commandUiValues.insert(QStringLiteral("outputTextRectRight"), commandUi.outputTextRectRight);
	snapshot.commandUiValues.insert(QStringLiteral("outputTextRectBottom"), commandUi.outputTextRectBottom);
	snapshot.commandUiValues.insert(QStringLiteral("hasLastMousePosition"), commandUi.hasLastMousePosition);
	snapshot.commandUiValues.insert(QStringLiteral("lastMouseX"), commandUi.lastMouseX);
	snapshot.commandUiValues.insert(QStringLiteral("lastMouseY"), commandUi.lastMouseY);
	snapshot.commandUiValues.insert(QStringLiteral("selectedWord"), commandUi.selectedWord);
	snapshot.commandUiValues.insert(QStringLiteral("selectedWordResolved"), commandUi.selectedWordResolved);
	snapshot.commandUiValues.insert(QStringLiteral("fullScreenMode"), commandUi.fullScreenMode);
	snapshot.commandUiValues.insert(QStringLiteral("worldWindowCount"), commandUi.worldWindowCount);
	snapshot.commandUiValues.insert(QStringLiteral("worldWindowShowCommand"),
	                                commandUi.worldWindowShowCommand);
	snapshot.commandUiValues.insert(QStringLiteral("mainClientHeight"), commandUi.mainClientHeight);
	snapshot.commandUiValues.insert(QStringLiteral("mainClientWidth"), commandUi.mainClientWidth);
	snapshot.commandUiValues.insert(QStringLiteral("mainToolbarHeight"), commandUi.mainToolbarHeight);
	snapshot.commandUiValues.insert(QStringLiteral("mainToolbarWidth"), commandUi.mainToolbarWidth);
	snapshot.commandUiValues.insert(QStringLiteral("worldToolbarHeight"), commandUi.worldToolbarHeight);
	snapshot.commandUiValues.insert(QStringLiteral("worldToolbarWidth"), commandUi.worldToolbarWidth);
	snapshot.commandUiValues.insert(QStringLiteral("activityToolbarHeight"), commandUi.activityToolbarHeight);
	snapshot.commandUiValues.insert(QStringLiteral("activityToolbarWidth"), commandUi.activityToolbarWidth);
	snapshot.commandUiValues.insert(QStringLiteral("infoBarHeight"), commandUi.infoBarHeight);
	snapshot.commandUiValues.insert(QStringLiteral("infoBarWidth"), commandUi.infoBarWidth);
	snapshot.commandUiValues.insert(QStringLiteral("statusBarHeight"), commandUi.statusBarHeight);
	snapshot.commandUiValues.insert(QStringLiteral("statusBarWidth"), commandUi.statusBarWidth);
	snapshot.commandUiValues.insert(QStringLiteral("worldChildWindowHeight"),
	                                commandUi.worldChildWindowHeight);
	snapshot.commandUiValues.insert(QStringLiteral("worldChildWindowWidth"), commandUi.worldChildWindowWidth);

	const ResolvedWorldColourTables colourTables = buildResolvedWorldColourTables(m_colours);
	const ResolvedNoteColours       noteColours  = resolveNoteColours(
	    m_notesInRgb, m_noteTextColour, m_noteColourFore, m_noteColourBack, colourTables, m_worldAttributes);
	const RuntimeCountersSnapshot counters =
	    runtimeCountersSnapshotWithResolvedNoteColours(true, noteColours.fore, noteColours.back);
	snapshot.hasRuntimeCountersSnapshot = true;
	snapshot.runtimeOutputFontHeight    = counters.outputFontHeight;
	snapshot.runtimeOutputFontWidth     = counters.outputFontWidth;
	snapshot.runtimeCounterValues.reserve(88);
	snapshot.runtimeCounterValues.insert(QStringLiteral("newLines"), counters.newLines);
	snapshot.runtimeCounterValues.insert(QStringLiteral("totalLinesSent"), counters.totalLinesSent);
	snapshot.runtimeCounterValues.insert(QStringLiteral("inputPacketCount"), counters.inputPacketCount);
	snapshot.runtimeCounterValues.insert(QStringLiteral("outputPacketCount"), counters.outputPacketCount);
	snapshot.runtimeCounterValues.insert(QStringLiteral("totalUncompressedBytes"),
	                                     QVariant::fromValue(counters.totalUncompressedBytes));
	snapshot.runtimeCounterValues.insert(QStringLiteral("totalCompressedBytes"),
	                                     QVariant::fromValue(counters.totalCompressedBytes));
	snapshot.runtimeCounterValues.insert(QStringLiteral("mccpType"), counters.mccpType);
	snapshot.runtimeCounterValues.insert(QStringLiteral("mxpErrorCount"), counters.mxpErrorCount);
	snapshot.runtimeCounterValues.insert(QStringLiteral("mxpTagCount"),
	                                     QVariant::fromValue(counters.mxpTagCount));
	snapshot.runtimeCounterValues.insert(QStringLiteral("mxpEntityCount"),
	                                     QVariant::fromValue(counters.mxpEntityCount));
	snapshot.runtimeCounterValues.insert(QStringLiteral("bytesIn"), QVariant::fromValue(counters.bytesIn));
	snapshot.runtimeCounterValues.insert(QStringLiteral("bytesOut"), QVariant::fromValue(counters.bytesOut));
	snapshot.runtimeCounterValues.insert(QStringLiteral("totalLinesReceived"), counters.totalLinesReceived);
	snapshot.runtimeCounterValues.insert(QStringLiteral("outputFontHeight"), counters.outputFontHeight);
	snapshot.runtimeCounterValues.insert(QStringLiteral("outputFontWidth"), counters.outputFontWidth);
	snapshot.runtimeCounterValues.insert(QStringLiteral("inputFontHeight"), counters.inputFontHeight);
	snapshot.runtimeCounterValues.insert(QStringLiteral("inputFontWidth"), counters.inputFontWidth);
	snapshot.runtimeCounterValues.insert(QStringLiteral("variableCount"), counters.variableCount);
	snapshot.runtimeCounterValues.insert(QStringLiteral("triggerCount"), counters.triggerCount);
	snapshot.runtimeCounterValues.insert(QStringLiteral("timerCount"), counters.timerCount);
	snapshot.runtimeCounterValues.insert(QStringLiteral("aliasCount"), counters.aliasCount);
	snapshot.runtimeCounterValues.insert(QStringLiteral("queuedCommandCount"), counters.queuedCommandCount);
	snapshot.runtimeCounterValues.insert(QStringLiteral("mappingCount"), counters.mappingCount);
	snapshot.runtimeCounterValues.insert(QStringLiteral("outputLineCount"), counters.outputLineCount);
	snapshot.runtimeCounterValues.insert(QStringLiteral("customElementCount"), counters.customElementCount);
	snapshot.runtimeCounterValues.insert(QStringLiteral("customEntityCount"), counters.customEntityCount);
	snapshot.runtimeCounterValues.insert(QStringLiteral("connectPhase"), counters.connectPhase);
	snapshot.runtimeCounterValues.insert(QStringLiteral("peerAddressV4"), counters.peerAddressV4);
	snapshot.runtimeCounterValues.insert(QStringLiteral("proxyAddressV4"), counters.proxyAddressV4);
	snapshot.runtimeCounterValues.insert(QStringLiteral("logFilePosition"),
	                                     QVariant::fromValue(counters.logFilePosition));
	snapshot.runtimeCounterValues.insert(QStringLiteral("triggerTimeSeconds"), counters.triggerTimeSeconds);
	snapshot.runtimeCounterValues.insert(QStringLiteral("aliasTimeSeconds"), counters.aliasTimeSeconds);
	snapshot.runtimeCounterValues.insert(QStringLiteral("scriptTimeSeconds"), counters.scriptTimeSeconds);
	snapshot.runtimeCounterValues.insert(QStringLiteral("noCommandEcho"), counters.noCommandEcho);
	snapshot.runtimeCounterValues.insert(QStringLiteral("debugIncomingPackets"),
	                                     counters.debugIncomingPackets);
	snapshot.runtimeCounterValues.insert(QStringLiteral("isCompressing"), counters.isCompressing);
	snapshot.runtimeCounterValues.insert(QStringLiteral("isMxpActive"), counters.isMxpActive);
	snapshot.runtimeCounterValues.insert(QStringLiteral("isPuebloActive"), counters.isPuebloActive);
	snapshot.runtimeCounterValues.insert(QStringLiteral("removeMapReverses"), counters.removeMapReverses);
	snapshot.runtimeCounterValues.insert(QStringLiteral("notesInRgb"), counters.notesInRgb);
	snapshot.runtimeCounterValues.insert(QStringLiteral("disconnectOk"), counters.disconnectOk);
	snapshot.runtimeCounterValues.insert(QStringLiteral("traceEnabled"), counters.traceEnabled);
	snapshot.runtimeCounterValues.insert(QStringLiteral("isLogOpen"), counters.isLogOpen);
	snapshot.runtimeCounterValues.insert(QStringLiteral("scriptFileChanged"), counters.scriptFileChanged);
	snapshot.runtimeCounterValues.insert(QStringLiteral("worldFileModified"), counters.worldFileModified);
	snapshot.runtimeCounterValues.insert(QStringLiteral("isMapping"), counters.isMapping);
	snapshot.runtimeCounterValues.insert(QStringLiteral("isActive"), counters.isActive);
	snapshot.runtimeCounterValues.insert(QStringLiteral("outputFrozen"), counters.outputFrozen);
	snapshot.runtimeCounterValues.insert(QStringLiteral("variablesChanged"), counters.variablesChanged);
	snapshot.runtimeCounterValues.insert(QStringLiteral("doingSimulate"), counters.doingSimulate);
	snapshot.runtimeCounterValues.insert(QStringLiteral("lineOmittedFromOutput"),
	                                     counters.lineOmittedFromOutput);
	snapshot.runtimeCounterValues.insert(QStringLiteral("hasLuaCallbacks"), counters.hasLuaCallbacks);
	snapshot.runtimeCounterValues.insert(QStringLiteral("pluginProcessingSent"),
	                                     counters.pluginProcessingSent);
	snapshot.runtimeCounterValues.insert(QStringLiteral("isChatAcceptingCalls"),
	                                     counters.isChatAcceptingCalls);
	snapshot.runtimeCounterValues.insert(QStringLiteral("noteStyle"), counters.noteStyle);
	snapshot.runtimeCounterValues.insert(QStringLiteral("noteTextColour"), counters.noteTextColour);
	snapshot.runtimeCounterValues.insert(QStringLiteral("noteColourBack"),
	                                     QVariant::fromValue(counters.noteColourBack));
	snapshot.runtimeCounterValues.insert(QStringLiteral("noteColourFore"),
	                                     QVariant::fromValue(counters.noteColourFore));
	snapshot.runtimeCounterValues.insert(QStringLiteral("backgroundColour"),
	                                     QVariant::fromValue(counters.backgroundColour));
	snapshot.runtimeCounterValues.insert(QStringLiteral("utf8ErrorCount"), counters.utf8ErrorCount);
	snapshot.runtimeCounterValues.insert(QStringLiteral("triggersEvaluatedCount"),
	                                     counters.triggersEvaluatedCount);
	snapshot.runtimeCounterValues.insert(QStringLiteral("triggersMatchedThisSession"),
	                                     counters.triggersMatchedThisSession);
	snapshot.runtimeCounterValues.insert(QStringLiteral("aliasesEvaluatedCount"),
	                                     counters.aliasesEvaluatedCount);
	snapshot.runtimeCounterValues.insert(QStringLiteral("aliasesMatchedThisSession"),
	                                     counters.aliasesMatchedThisSession);
	snapshot.runtimeCounterValues.insert(QStringLiteral("timersFiredThisSession"),
	                                     counters.timersFiredThisSession);
	snapshot.runtimeCounterValues.insert(QStringLiteral("lastLineWithIacGa"), counters.lastLineWithIacGa);
	snapshot.runtimeCounterValues.insert(QStringLiteral("outputWindowRedrawCount"),
	                                     counters.outputWindowRedrawCount);
	snapshot.runtimeCounterValues.insert(QStringLiteral("currentActionSource"), counters.currentActionSource);
	snapshot.runtimeCounterValues.insert(QStringLiteral("newlinesReceived"),
	                                     QVariant::fromValue(counters.newlinesReceived));
	snapshot.runtimeCounterValues.insert(QStringLiteral("connectTime"), counters.connectTime);
	snapshot.runtimeCounterValues.insert(QStringLiteral("statusTime"), counters.statusTime);
	snapshot.runtimeCounterValues.insert(QStringLiteral("lastFlushTime"), counters.lastFlushTime);
	snapshot.runtimeCounterValues.insert(QStringLiteral("clientStartTime"), counters.clientStartTime);
	snapshot.runtimeCounterValues.insert(QStringLiteral("worldStartTime"), counters.worldStartTime);
	snapshot.runtimeCounterValues.insert(QStringLiteral("scriptFileModTime"), counters.scriptFileModTime);
	snapshot.runtimeCounterValues.insert(QStringLiteral("logFileName"), counters.logFileName);
	snapshot.runtimeCounterValues.insert(QStringLiteral("lastImmediateExpression"),
	                                     counters.lastImmediateExpression);
	snapshot.runtimeCounterValues.insert(QStringLiteral("statusMessage"), counters.statusMessage);
	snapshot.runtimeCounterValues.insert(QStringLiteral("worldFilePath"), counters.worldFilePath);
	snapshot.runtimeCounterValues.insert(QStringLiteral("windowTitleOverride"), counters.windowTitleOverride);
	snapshot.runtimeCounterValues.insert(QStringLiteral("mainTitleOverride"), counters.mainTitleOverride);
	snapshot.runtimeCounterValues.insert(QStringLiteral("defaultWorldDirectory"),
	                                     counters.defaultWorldDirectory);
	snapshot.runtimeCounterValues.insert(QStringLiteral("defaultLogDirectory"), counters.defaultLogDirectory);
	snapshot.runtimeCounterValues.insert(QStringLiteral("pluginsDirectory"), counters.pluginsDirectory);
	snapshot.runtimeCounterValues.insert(QStringLiteral("peerAddressString"), counters.peerAddressString);
	snapshot.runtimeCounterValues.insert(QStringLiteral("proxyAddressString"), counters.proxyAddressString);
	snapshot.runtimeCounterValues.insert(QStringLiteral("startupDirectory"), counters.startupDirectory);
	snapshot.runtimeCounterValues.insert(QStringLiteral("translatorFile"), counters.translatorFile);
	snapshot.runtimeCounterValues.insert(QStringLiteral("locale"), counters.locale);
	snapshot.runtimeCounterValues.insert(QStringLiteral("fixedPitchFont"), counters.fixedPitchFont);
	snapshot.runtimeCounterValues.insert(QStringLiteral("lastTelnetSubnegotiation"),
	                                     counters.lastTelnetSubnegotiation);
	snapshot.runtimeCounterValues.insert(QStringLiteral("firstSpecialFontPath"),
	                                     counters.firstSpecialFontPath);
	snapshot.runtimeCounterValues.insert(QStringLiteral("preferencesDatabaseName"),
	                                     counters.preferencesDatabaseName);
	snapshot.runtimeCounterValues.insert(QStringLiteral("fileBrowsingDirectory"),
	                                     counters.fileBrowsingDirectory);
	snapshot.runtimeCounterValues.insert(QStringLiteral("stateFilesDirectory"), counters.stateFilesDirectory);
	snapshot.runtimeCounterValues.insert(QStringLiteral("lastCommandSent"), counters.lastCommandSent);

	const QMap<QString, QString> entitySnapshot = customEntitySnapshot();
	for (auto it = entitySnapshot.constBegin(); it != entitySnapshot.constEnd(); ++it)
		snapshot.entityValuesByName.insert(it.key(), it.value());
	snapshot.hasEntitySnapshot = true;

	const auto makeAnsiRenderSnapshot = [](const AnsiRenderState &state)
	{
		LuaCallbackAnsiRenderStateSnapshot row;
		row.bold       = state.bold;
		row.underline  = state.underline;
		row.italic     = state.italic;
		row.blink      = state.blink;
		row.inverse    = state.inverse;
		row.strike     = state.strike;
		row.monospace  = state.monospace;
		row.fore       = state.fore;
		row.back       = state.back;
		row.actionType = state.actionType;
		row.action     = state.action;
		row.hint       = state.hint;
		row.variable   = state.variable;
		row.startTag   = state.startTag;
		return row;
	};
	const auto makeMxpStyleSnapshot = [](const MxpStyleState &state)
	{
		LuaCallbackMxpStyleStateSnapshot row;
		row.bold       = state.bold;
		row.underline  = state.underline;
		row.italic     = state.italic;
		row.blink      = state.blink;
		row.strike     = state.strike;
		row.monospace  = state.monospace;
		row.inverse    = state.inverse;
		row.fore       = state.fore;
		row.back       = state.back;
		row.actionType = state.actionType;
		row.action     = state.action;
		row.hint       = state.hint;
		row.variable   = state.variable;
		row.startTag   = state.startTag;
		return row;
	};
	const auto makeMxpStyleFrameSnapshot = [&makeMxpStyleSnapshot](const MxpStyleFrame &frame)
	{
		LuaCallbackMxpStyleFrameSnapshot row;
		row.tag                           = frame.tag;
		row.state                         = makeMxpStyleSnapshot(frame.state);
		row.actionState                   = makeMxpStyleSnapshot(frame.actionState);
		row.actionTextLineNumber          = frame.actionTextLineNumber;
		row.actionTextStartColumn         = frame.actionTextStartColumn;
		row.actionTextRuntimeLineNumber   = frame.actionTextRuntimeLineNumber;
		row.actionTextPartialLineRevision = frame.actionTextPartialLineRevision;
		return row;
	};

	snapshot.hasWindowOutputTextRenderSnapshot = true;
	snapshot.windowOutputTextAnsiStreamState   = m_ansiStreamState;
	snapshot.windowOutputTextAnsiRenderState   = makeAnsiRenderSnapshot(m_ansiRenderState);
	snapshot.windowOutputTextMxpStyleState     = makeMxpStyleSnapshot(m_mxpRenderStyle);
	snapshot.windowOutputTextMxpBlockStack     = m_mxpRenderBlockStack;
	snapshot.windowOutputTextMxpLinkOpen       = m_mxpRenderLinkOpen;
	snapshot.windowOutputTextMxpPreDepth       = m_mxpRenderPreDepth;
	snapshot.windowOutputTextMxpStyleStack.reserve(m_mxpRenderStack.size());
	for (const MxpStyleFrame &frame : m_mxpRenderStack)
		snapshot.windowOutputTextMxpStyleStack.push_back(makeMxpStyleFrameSnapshot(frame));
	const QList<TelnetProcessor::CustomElementInfo> customElements = m_telnet.customElementInfos();
	snapshot.windowOutputTextCustomElements.reserve(customElements.size());
	for (const TelnetProcessor::CustomElementInfo &element : customElements)
	{
		LuaCallbackMxpCustomElementSnapshot row;
		row.name       = element.name;
		row.open       = element.open;
		row.command    = element.command;
		row.tag        = element.tag;
		row.flag       = element.flag;
		row.definition = element.definition;
		row.attributes = element.attributes;
		snapshot.windowOutputTextCustomElements.push_back(row);
	}

	if (lineSnapshotPolicy == LuaCallbackLineSnapshotPolicy::CountAndRecentText ||
	    lineSnapshotPolicy == LuaCallbackLineSnapshotPolicy::Full ||
	    lineSnapshotPolicy == LuaCallbackLineSnapshotPolicy::CountAndRecent)
	{
		snapshot.lineBufferSnapshot    = captureLuaCallbackLineBufferSnapshotForDispatch(lineSnapshotPolicy);
		snapshot.hasLineBufferSnapshot = static_cast<bool>(snapshot.lineBufferSnapshot);
		if (snapshot.lineBufferSnapshot)
		{
			snapshot.lineBufferCount        = snapshot.lineBufferSnapshot->lineBufferCount;
			snapshot.recentLinesSnapshot    = snapshot.lineBufferSnapshot->recentLinesSnapshot;
			snapshot.hasRecentLinesSnapshot = true;
		}
	}

	captureMainWindowSnapshotForLuaDispatch(snapshot, this);
}

QSharedPointer<const LuaCallbackLineBufferSnapshot>
WorldRuntime::captureLuaCallbackLineBufferSnapshotForDispatch(
    const LuaCallbackLineSnapshotPolicy lineSnapshotPolicy) const
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::captureLuaCallbackLineBufferSnapshotForDispatch");
	if (lineSnapshotPolicy == LuaCallbackLineSnapshotPolicy::Full && m_luaCallbackLineBufferSnapshotCache &&
	    m_luaCallbackLineBufferSnapshotCacheGeneration == m_luaCallbackLineBufferSnapshotGeneration)
	{
		return m_luaCallbackLineBufferSnapshotCache;
	}
	if (lineSnapshotPolicy == LuaCallbackLineSnapshotPolicy::CountAndRecent &&
	    m_luaCallbackRecentLineBufferSnapshotCache &&
	    m_luaCallbackRecentLineBufferSnapshotCacheGeneration == m_luaCallbackLineBufferSnapshotGeneration)
	{
		return m_luaCallbackRecentLineBufferSnapshotCache;
	}
	if (lineSnapshotPolicy == LuaCallbackLineSnapshotPolicy::CountAndRecentText &&
	    m_luaCallbackRecentTextLineBufferSnapshotCache &&
	    m_luaCallbackRecentTextLineBufferSnapshotCacheGeneration == m_luaCallbackLineBufferSnapshotGeneration)
	{
		return m_luaCallbackRecentTextLineBufferSnapshotCache;
	}

	const auto makeLineSnapshot = [](const LineEntry &entry) -> LuaCallbackLineEntrySnapshot
	{
		LuaCallbackLineEntrySnapshot row;
		row.text       = entry.text;
		row.flags      = entry.flags;
		row.hardReturn = entry.hardReturn;
		row.time       = entry.time;
		row.lineNumber = entry.lineNumber;
		row.ticks      = entry.ticks;
		row.elapsed    = entry.elapsed;
		row.spans.reserve(entry.spans.size());
		for (const StyleSpan &span : entry.spans)
		{
			LuaCallbackLineStyleSnapshot spanRow;
			spanRow.length     = span.length;
			spanRow.fore       = colorToLong(span.fore);
			spanRow.back       = colorToLong(span.back);
			spanRow.bold       = span.bold;
			spanRow.underline  = span.underline;
			spanRow.italic     = span.italic;
			spanRow.blink      = span.blink;
			spanRow.strike     = span.strike;
			spanRow.inverse    = span.inverse;
			spanRow.changed    = span.changed;
			spanRow.actionType = span.actionType;
			spanRow.action     = span.action;
			spanRow.hint       = span.hint;
			spanRow.variable   = span.variable;
			spanRow.startTag   = span.startTag;
			row.spans.push_back(std::move(spanRow));
		}
		return row;
	};

	const QSharedPointer<LuaCallbackLineBufferSnapshot> snapshot =
	    QSharedPointer<LuaCallbackLineBufferSnapshot>::create();
	snapshot->lineBufferCount = luaContextLinesInBufferCount();
	if (lineSnapshotPolicy != LuaCallbackLineSnapshotPolicy::CountAndRecentText)
	{
		constexpr int kRecentLineSnapshotLimit = 200;
		const int firstLineNumber = lineSnapshotPolicy == LuaCallbackLineSnapshotPolicy::Full
		                                ? 1
		                                : qMax(1, snapshot->lineBufferCount - kRecentLineSnapshotLimit + 1);
		for (int lineNumber = firstLineNumber; lineNumber <= snapshot->lineBufferCount; ++lineNumber)
		{
			LineEntry entry;
			if (luaContextLineEntry(lineNumber, entry))
				snapshot->lineEntriesByBufferIndex.insert(lineNumber, makeLineSnapshot(entry));
		}
	}
	snapshot->recentLinesSnapshot = recentLines();

	if (lineSnapshotPolicy == LuaCallbackLineSnapshotPolicy::Full)
	{
		m_luaCallbackLineBufferSnapshotCache           = snapshot;
		m_luaCallbackLineBufferSnapshotCacheGeneration = m_luaCallbackLineBufferSnapshotGeneration;
	}
	else if (lineSnapshotPolicy == LuaCallbackLineSnapshotPolicy::CountAndRecent)
	{
		m_luaCallbackRecentLineBufferSnapshotCache           = snapshot;
		m_luaCallbackRecentLineBufferSnapshotCacheGeneration = m_luaCallbackLineBufferSnapshotGeneration;
	}
	else
	{
		m_luaCallbackRecentTextLineBufferSnapshotCache           = snapshot;
		m_luaCallbackRecentTextLineBufferSnapshotCacheGeneration = m_luaCallbackLineBufferSnapshotGeneration;
	}
	return snapshot;
}

QSharedPointer<const LuaCallbackMiniWindowSnapshot> WorldRuntime::captureLuaCallbackSnapshotForDispatch(
    const QVector<QSharedPointer<LuaCallbackEngine>> &recipients,
    const LuaCallbackLineSnapshotPolicy               lineSnapshotPolicy) const
{
	return captureLuaCallbackSnapshotForDispatchMutable(recipients, lineSnapshotPolicy);
}

QSharedPointer<LuaCallbackMiniWindowSnapshot> WorldRuntime::captureLuaCallbackSnapshotForDispatchMutable(
    const QVector<QSharedPointer<LuaCallbackEngine>> &recipients,
    const LuaCallbackLineSnapshotPolicy               lineSnapshotPolicy) const
{
	if (QThread::currentThread() != thread())
	{
		return qmudInvokeMethodOr(
		    const_cast<WorldRuntime *>(this), QSharedPointer<LuaCallbackMiniWindowSnapshot>{},
		    [this, recipients, lineSnapshotPolicy]
		    { return captureLuaCallbackSnapshotForDispatchMutable(recipients, lineSnapshotPolicy); });
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::captureLuaCallbackSnapshotForDispatchMutable");
	if (auto *mutableRuntime = const_cast<WorldRuntime *>(this);
	    mutableRuntime->applyPendingObservedPluginCallbackPresenceSnapshots())
	{
		mutableRuntime->invalidateLuaCallbackDispatchSnapshot();
		mutableRuntime->m_pluginCallbackPresenceDirty = true;
	}

	if (QSharedPointer<LuaCallbackMiniWindowSnapshot> cachedSnapshot = cloneLuaCallbackDispatchSnapshotBase())
	{
		populateLuaCallbackDispatchVolatileSnapshot(*cachedSnapshot, lineSnapshotPolicy);
#ifndef NDEBUG
		if (qmudMmStartupDiagHasWatchedEngine(recipients))
		{
			qInfo().noquote() << QStringLiteral(
			                         "[QMud][MMStartupDiag] snapshot-capture cached=1 targets=%1 %2")
			                         .arg(qmudMmStartupDiagEngineLabels(recipients),
			                              qmudMmStartupDiagBroadcastSnapshotLabel(cachedSnapshot));
		}
#endif
		return cachedSnapshot;
	}

	QSharedPointer<LuaCallbackMiniWindowSnapshot> snapshot =
	    captureMiniWindowSnapshotForLuaDispatch(m_miniWindows);
	if (!snapshot)
		return {};

	const int callbackOutputAnchorBufferIndex = luaContextLinesInBufferCount();
	if (callbackOutputAnchorBufferIndex > 0)
	{
		LineEntry callbackOutputAnchorEntry;
		if (luaContextLineEntry(callbackOutputAnchorBufferIndex, callbackOutputAnchorEntry) &&
		    callbackOutputAnchorEntry.lineNumber > 0)
		{
			snapshot->hasCallbackOutputAnchor            = true;
			snapshot->callbackOutputAnchorBufferIndex    = callbackOutputAnchorBufferIndex;
			snapshot->callbackOutputAnchorAbsoluteNumber = callbackOutputAnchorEntry.lineNumber;
		}
	}

	const auto makeTriggerSnapshot = [](const Trigger &trigger) -> LuaCallbackTriggerSnapshot
	{
		LuaCallbackTriggerSnapshot row;
		row.attributes           = trigger.attributes;
		row.children             = trigger.children;
		row.included             = trigger.included;
		row.matched              = trigger.matched;
		row.invocationCount      = trigger.invocationCount;
		row.matchAttempts        = trigger.matchAttempts;
		row.lastMatchTarget      = trigger.lastMatchTarget;
		row.lastMatched          = trigger.lastMatched;
		row.runtimeId            = trigger.runtimeId;
		row.executingScriptDepth = trigger.executingScriptDepth;
		row.executingScript      = trigger.executingScript;
		return row;
	};
	const auto makeTriggerSnapshots =
	    [&makeTriggerSnapshot](const QList<Trigger> &triggers) -> QList<LuaCallbackTriggerSnapshot>
	{
		QList<LuaCallbackTriggerSnapshot> rows;
		rows.reserve(triggers.size());
		for (const Trigger &trigger : triggers)
			rows.push_back(makeTriggerSnapshot(trigger));
		return rows;
	};
	const auto makeAliasSnapshot = [](const Alias &alias) -> LuaCallbackAliasSnapshot
	{
		LuaCallbackAliasSnapshot row;
		row.attributes           = alias.attributes;
		row.children             = alias.children;
		row.included             = alias.included;
		row.matched              = alias.matched;
		row.invocationCount      = alias.invocationCount;
		row.matchAttempts        = alias.matchAttempts;
		row.lastMatchTarget      = alias.lastMatchTarget;
		row.lastMatched          = alias.lastMatched;
		row.runtimeId            = alias.runtimeId;
		row.executingScriptDepth = alias.executingScriptDepth;
		row.executingScript      = alias.executingScript;
		return row;
	};
	const auto makeAliasSnapshots =
	    [&makeAliasSnapshot](const QList<Alias> &aliases) -> QList<LuaCallbackAliasSnapshot>
	{
		QList<LuaCallbackAliasSnapshot> rows;
		rows.reserve(aliases.size());
		for (const Alias &alias : aliases)
			rows.push_back(makeAliasSnapshot(alias));
		return rows;
	};
	const auto makeTimerSnapshot = [](const Timer &timer) -> LuaCallbackTimerSnapshot
	{
		LuaCallbackTimerSnapshot row;
		row.attributes           = timer.attributes;
		row.children             = timer.children;
		row.included             = timer.included;
		row.lastFired            = timer.lastFired;
		row.nextFireTime         = timer.nextFireTime;
		row.firedCount           = timer.firedCount;
		row.invocationCount      = timer.invocationCount;
		row.executingScript      = timer.executingScript;
		row.runtimeId            = timer.runtimeId;
		row.executingScriptDepth = timer.executingScriptDepth;
		return row;
	};
	const auto makeTimerSnapshots =
	    [&makeTimerSnapshot](const QList<Timer> &timers) -> QList<LuaCallbackTimerSnapshot>
	{
		QList<LuaCallbackTimerSnapshot> rows;
		rows.reserve(timers.size());
		for (const Timer &timer : timers)
			rows.push_back(makeTimerSnapshot(timer));
		return rows;
	};
	const auto makeAnsiRenderSnapshot = [](const AnsiRenderState &state)
	{
		LuaCallbackAnsiRenderStateSnapshot row;
		row.bold       = state.bold;
		row.underline  = state.underline;
		row.italic     = state.italic;
		row.blink      = state.blink;
		row.inverse    = state.inverse;
		row.strike     = state.strike;
		row.monospace  = state.monospace;
		row.fore       = state.fore;
		row.back       = state.back;
		row.actionType = state.actionType;
		row.action     = state.action;
		row.hint       = state.hint;
		row.variable   = state.variable;
		row.startTag   = state.startTag;
		return row;
	};
	const auto makeMxpStyleSnapshot = [](const MxpStyleState &state)
	{
		LuaCallbackMxpStyleStateSnapshot row;
		row.bold       = state.bold;
		row.underline  = state.underline;
		row.italic     = state.italic;
		row.blink      = state.blink;
		row.strike     = state.strike;
		row.monospace  = state.monospace;
		row.inverse    = state.inverse;
		row.fore       = state.fore;
		row.back       = state.back;
		row.actionType = state.actionType;
		row.action     = state.action;
		row.hint       = state.hint;
		row.variable   = state.variable;
		row.startTag   = state.startTag;
		return row;
	};
	const auto makeMxpStyleFrameSnapshot = [&makeMxpStyleSnapshot](const MxpStyleFrame &frame)
	{
		LuaCallbackMxpStyleFrameSnapshot row;
		row.tag                           = frame.tag;
		row.state                         = makeMxpStyleSnapshot(frame.state);
		row.actionState                   = makeMxpStyleSnapshot(frame.actionState);
		row.actionTextLineNumber          = frame.actionTextLineNumber;
		row.actionTextStartColumn         = frame.actionTextStartColumn;
		row.actionTextRuntimeLineNumber   = frame.actionTextRuntimeLineNumber;
		row.actionTextPartialLineRevision = frame.actionTextPartialLineRevision;
		return row;
	};

	const CommandUiSnapshot commandUi     = commandUiSnapshot(false, true, false);
	snapshot->hasCommandUiSnapshot        = true;
	snapshot->commandUiHasView            = commandUi.hasView;
	snapshot->commandUiHasFrameData       = commandUi.hasFrameData;
	snapshot->commandUiOutputClientHeight = commandUi.outputClientHeight;
	snapshot->commandUiOutputClientWidth  = commandUi.outputClientWidth;
	snapshot->commandUiViewHeight         = commandUi.viewHeight;
	snapshot->commandUiViewWidth          = commandUi.viewWidth;
	snapshot->commandUiValues.reserve(53);
	snapshot->commandUiValues.insert(QStringLiteral("queuedCommands"), commandUi.queuedCommands);
	snapshot->commandUiValues.insert(QStringLiteral("triggerPriorityQueuedCommandCount"),
	                                 commandUi.triggerPriorityQueuedCommandCount);
	snapshot->commandUiValues.insert(QStringLiteral("commandInputText"), commandUi.commandInputText);
	snapshot->commandUiValues.insert(QStringLiteral("inputSelectionStartColumn"),
	                                 commandUi.inputSelectionStartColumn);
	snapshot->commandUiValues.insert(QStringLiteral("inputSelectionEndColumn"),
	                                 commandUi.inputSelectionEndColumn);
	snapshot->commandUiValues.insert(QStringLiteral("outputSelectionEndColumn"),
	                                 commandUi.outputSelectionEndColumn);
	snapshot->commandUiValues.insert(QStringLiteral("outputSelectionEndLine"),
	                                 commandUi.outputSelectionEndLine);
	snapshot->commandUiValues.insert(QStringLiteral("outputSelectionStartColumn"),
	                                 commandUi.outputSelectionStartColumn);
	snapshot->commandUiValues.insert(QStringLiteral("outputSelectionStartLine"),
	                                 commandUi.outputSelectionStartLine);
	snapshot->commandUiValues.insert(QStringLiteral("textRectangleLeft"), commandUi.textRectangleLeft);
	snapshot->commandUiValues.insert(QStringLiteral("textRectangleTop"), commandUi.textRectangleTop);
	snapshot->commandUiValues.insert(QStringLiteral("textRectangleRight"), commandUi.textRectangleRight);
	snapshot->commandUiValues.insert(QStringLiteral("textRectangleBottom"), commandUi.textRectangleBottom);
	snapshot->commandUiValues.insert(QStringLiteral("textRectangleBorderOffset"),
	                                 commandUi.textRectangleBorderOffset);
	snapshot->commandUiValues.insert(QStringLiteral("textRectangleBorderWidth"),
	                                 commandUi.textRectangleBorderWidth);
	snapshot->commandUiValues.insert(QStringLiteral("textRectangleOutsideFillColour"),
	                                 commandUi.textRectangleOutsideFillColour);
	snapshot->commandUiValues.insert(QStringLiteral("textRectangleOutsideFillStyle"),
	                                 commandUi.textRectangleOutsideFillStyle);
	snapshot->commandUiValues.insert(QStringLiteral("textRectangleBorderColour"),
	                                 commandUi.textRectangleBorderColour);
	snapshot->commandUiValues.insert(QStringLiteral("hasView"), commandUi.hasView);
	snapshot->commandUiValues.insert(QStringLiteral("hasFrameData"), commandUi.hasFrameData);
	snapshot->commandUiValues.insert(QStringLiteral("outputScrollBarWanted"),
	                                 commandUi.outputScrollBarWanted);
	snapshot->commandUiValues.insert(QStringLiteral("outputScrollPosition"), commandUi.outputScrollPosition);
	snapshot->commandUiValues.insert(QStringLiteral("outputClientHeight"), commandUi.outputClientHeight);
	snapshot->commandUiValues.insert(QStringLiteral("outputClientWidth"), commandUi.outputClientWidth);
	snapshot->commandUiValues.insert(QStringLiteral("viewHeight"), commandUi.viewHeight);
	snapshot->commandUiValues.insert(QStringLiteral("viewWidth"), commandUi.viewWidth);
	snapshot->commandUiValues.insert(QStringLiteral("outputTextRectLeft"), commandUi.outputTextRectLeft);
	snapshot->commandUiValues.insert(QStringLiteral("outputTextRectTop"), commandUi.outputTextRectTop);
	snapshot->commandUiValues.insert(QStringLiteral("outputTextRectRight"), commandUi.outputTextRectRight);
	snapshot->commandUiValues.insert(QStringLiteral("outputTextRectBottom"), commandUi.outputTextRectBottom);
	snapshot->commandUiValues.insert(QStringLiteral("hasLastMousePosition"), commandUi.hasLastMousePosition);
	snapshot->commandUiValues.insert(QStringLiteral("lastMouseX"), commandUi.lastMouseX);
	snapshot->commandUiValues.insert(QStringLiteral("lastMouseY"), commandUi.lastMouseY);
	snapshot->commandUiValues.insert(QStringLiteral("selectedWord"), commandUi.selectedWord);
	snapshot->commandUiValues.insert(QStringLiteral("selectedWordResolved"), commandUi.selectedWordResolved);
	snapshot->commandUiValues.insert(QStringLiteral("fullScreenMode"), commandUi.fullScreenMode);
	snapshot->commandUiValues.insert(QStringLiteral("worldWindowCount"), commandUi.worldWindowCount);
	snapshot->commandUiValues.insert(QStringLiteral("worldWindowShowCommand"),
	                                 commandUi.worldWindowShowCommand);
	snapshot->commandUiValues.insert(QStringLiteral("mainClientHeight"), commandUi.mainClientHeight);
	snapshot->commandUiValues.insert(QStringLiteral("mainClientWidth"), commandUi.mainClientWidth);
	snapshot->commandUiValues.insert(QStringLiteral("mainToolbarHeight"), commandUi.mainToolbarHeight);
	snapshot->commandUiValues.insert(QStringLiteral("mainToolbarWidth"), commandUi.mainToolbarWidth);
	snapshot->commandUiValues.insert(QStringLiteral("worldToolbarHeight"), commandUi.worldToolbarHeight);
	snapshot->commandUiValues.insert(QStringLiteral("worldToolbarWidth"), commandUi.worldToolbarWidth);
	snapshot->commandUiValues.insert(QStringLiteral("activityToolbarHeight"),
	                                 commandUi.activityToolbarHeight);
	snapshot->commandUiValues.insert(QStringLiteral("activityToolbarWidth"), commandUi.activityToolbarWidth);
	snapshot->commandUiValues.insert(QStringLiteral("infoBarHeight"), commandUi.infoBarHeight);
	snapshot->commandUiValues.insert(QStringLiteral("infoBarWidth"), commandUi.infoBarWidth);
	snapshot->commandUiValues.insert(QStringLiteral("statusBarHeight"), commandUi.statusBarHeight);
	snapshot->commandUiValues.insert(QStringLiteral("statusBarWidth"), commandUi.statusBarWidth);
	snapshot->commandUiValues.insert(QStringLiteral("worldChildWindowHeight"),
	                                 commandUi.worldChildWindowHeight);
	snapshot->commandUiValues.insert(QStringLiteral("worldChildWindowWidth"),
	                                 commandUi.worldChildWindowWidth);

	const ResolvedWorldColourTables colourTables = buildResolvedWorldColourTables(m_colours);
	const ResolvedNoteColours       noteColours  = resolveNoteColours(
	    m_notesInRgb, m_noteTextColour, m_noteColourFore, m_noteColourBack, colourTables, m_worldAttributes);
	const RuntimeCountersSnapshot counters =
	    runtimeCountersSnapshotWithResolvedNoteColours(true, noteColours.fore, noteColours.back);
	snapshot->hasRuntimeCountersSnapshot = true;
	snapshot->runtimeOutputFontHeight    = counters.outputFontHeight;
	snapshot->runtimeOutputFontWidth     = counters.outputFontWidth;
	snapshot->runtimeCounterValues.reserve(88);
	snapshot->runtimeCounterValues.insert(QStringLiteral("newLines"), counters.newLines);
	snapshot->runtimeCounterValues.insert(QStringLiteral("totalLinesSent"), counters.totalLinesSent);
	snapshot->runtimeCounterValues.insert(QStringLiteral("inputPacketCount"), counters.inputPacketCount);
	snapshot->runtimeCounterValues.insert(QStringLiteral("outputPacketCount"), counters.outputPacketCount);
	snapshot->runtimeCounterValues.insert(QStringLiteral("totalUncompressedBytes"),
	                                      QVariant::fromValue(counters.totalUncompressedBytes));
	snapshot->runtimeCounterValues.insert(QStringLiteral("totalCompressedBytes"),
	                                      QVariant::fromValue(counters.totalCompressedBytes));
	snapshot->runtimeCounterValues.insert(QStringLiteral("mccpType"), counters.mccpType);
	snapshot->runtimeCounterValues.insert(QStringLiteral("mxpErrorCount"), counters.mxpErrorCount);
	snapshot->runtimeCounterValues.insert(QStringLiteral("mxpTagCount"),
	                                      QVariant::fromValue(counters.mxpTagCount));
	snapshot->runtimeCounterValues.insert(QStringLiteral("mxpEntityCount"),
	                                      QVariant::fromValue(counters.mxpEntityCount));
	snapshot->runtimeCounterValues.insert(QStringLiteral("bytesIn"), QVariant::fromValue(counters.bytesIn));
	snapshot->runtimeCounterValues.insert(QStringLiteral("bytesOut"), QVariant::fromValue(counters.bytesOut));
	snapshot->runtimeCounterValues.insert(QStringLiteral("totalLinesReceived"), counters.totalLinesReceived);
	snapshot->runtimeCounterValues.insert(QStringLiteral("outputFontHeight"), counters.outputFontHeight);
	snapshot->runtimeCounterValues.insert(QStringLiteral("outputFontWidth"), counters.outputFontWidth);
	snapshot->runtimeCounterValues.insert(QStringLiteral("inputFontHeight"), counters.inputFontHeight);
	snapshot->runtimeCounterValues.insert(QStringLiteral("inputFontWidth"), counters.inputFontWidth);
	snapshot->runtimeCounterValues.insert(QStringLiteral("variableCount"), counters.variableCount);
	snapshot->runtimeCounterValues.insert(QStringLiteral("triggerCount"), counters.triggerCount);
	snapshot->runtimeCounterValues.insert(QStringLiteral("timerCount"), counters.timerCount);
	snapshot->runtimeCounterValues.insert(QStringLiteral("aliasCount"), counters.aliasCount);
	snapshot->runtimeCounterValues.insert(QStringLiteral("queuedCommandCount"), counters.queuedCommandCount);
	snapshot->runtimeCounterValues.insert(QStringLiteral("mappingCount"), counters.mappingCount);
	snapshot->runtimeCounterValues.insert(QStringLiteral("outputLineCount"), counters.outputLineCount);
	snapshot->runtimeCounterValues.insert(QStringLiteral("customElementCount"), counters.customElementCount);
	snapshot->runtimeCounterValues.insert(QStringLiteral("customEntityCount"), counters.customEntityCount);
	snapshot->runtimeCounterValues.insert(QStringLiteral("connectPhase"), counters.connectPhase);
	snapshot->runtimeCounterValues.insert(QStringLiteral("peerAddressV4"), counters.peerAddressV4);
	snapshot->runtimeCounterValues.insert(QStringLiteral("proxyAddressV4"), counters.proxyAddressV4);
	snapshot->runtimeCounterValues.insert(QStringLiteral("logFilePosition"),
	                                      QVariant::fromValue(counters.logFilePosition));
	snapshot->runtimeCounterValues.insert(QStringLiteral("triggerTimeSeconds"), counters.triggerTimeSeconds);
	snapshot->runtimeCounterValues.insert(QStringLiteral("aliasTimeSeconds"), counters.aliasTimeSeconds);
	snapshot->runtimeCounterValues.insert(QStringLiteral("scriptTimeSeconds"), counters.scriptTimeSeconds);
	snapshot->runtimeCounterValues.insert(QStringLiteral("noCommandEcho"), counters.noCommandEcho);
	snapshot->runtimeCounterValues.insert(QStringLiteral("debugIncomingPackets"),
	                                      counters.debugIncomingPackets);
	snapshot->runtimeCounterValues.insert(QStringLiteral("isCompressing"), counters.isCompressing);
	snapshot->runtimeCounterValues.insert(QStringLiteral("isMxpActive"), counters.isMxpActive);
	snapshot->runtimeCounterValues.insert(QStringLiteral("isPuebloActive"), counters.isPuebloActive);
	snapshot->runtimeCounterValues.insert(QStringLiteral("removeMapReverses"), counters.removeMapReverses);
	snapshot->runtimeCounterValues.insert(QStringLiteral("notesInRgb"), counters.notesInRgb);
	snapshot->runtimeCounterValues.insert(QStringLiteral("disconnectOk"), counters.disconnectOk);
	snapshot->runtimeCounterValues.insert(QStringLiteral("traceEnabled"), counters.traceEnabled);
	snapshot->runtimeCounterValues.insert(QStringLiteral("isLogOpen"), counters.isLogOpen);
	snapshot->runtimeCounterValues.insert(QStringLiteral("scriptFileChanged"), counters.scriptFileChanged);
	snapshot->runtimeCounterValues.insert(QStringLiteral("worldFileModified"), counters.worldFileModified);
	snapshot->runtimeCounterValues.insert(QStringLiteral("isMapping"), counters.isMapping);
	snapshot->runtimeCounterValues.insert(QStringLiteral("isActive"), counters.isActive);
	snapshot->runtimeCounterValues.insert(QStringLiteral("outputFrozen"), counters.outputFrozen);
	snapshot->runtimeCounterValues.insert(QStringLiteral("variablesChanged"), counters.variablesChanged);
	snapshot->runtimeCounterValues.insert(QStringLiteral("doingSimulate"), counters.doingSimulate);
	snapshot->runtimeCounterValues.insert(QStringLiteral("lineOmittedFromOutput"),
	                                      counters.lineOmittedFromOutput);
	snapshot->runtimeCounterValues.insert(QStringLiteral("hasLuaCallbacks"), counters.hasLuaCallbacks);
	snapshot->runtimeCounterValues.insert(QStringLiteral("pluginProcessingSent"),
	                                      counters.pluginProcessingSent);
	snapshot->runtimeCounterValues.insert(QStringLiteral("isChatAcceptingCalls"),
	                                      counters.isChatAcceptingCalls);
	snapshot->runtimeCounterValues.insert(QStringLiteral("noteStyle"), counters.noteStyle);
	snapshot->runtimeCounterValues.insert(QStringLiteral("noteTextColour"), counters.noteTextColour);
	snapshot->runtimeCounterValues.insert(QStringLiteral("noteColourBack"),
	                                      QVariant::fromValue(counters.noteColourBack));
	snapshot->runtimeCounterValues.insert(QStringLiteral("noteColourFore"),
	                                      QVariant::fromValue(counters.noteColourFore));
	snapshot->runtimeCounterValues.insert(QStringLiteral("backgroundColour"),
	                                      QVariant::fromValue(counters.backgroundColour));
	snapshot->runtimeCounterValues.insert(QStringLiteral("utf8ErrorCount"), counters.utf8ErrorCount);
	snapshot->runtimeCounterValues.insert(QStringLiteral("triggersEvaluatedCount"),
	                                      counters.triggersEvaluatedCount);
	snapshot->runtimeCounterValues.insert(QStringLiteral("triggersMatchedThisSession"),
	                                      counters.triggersMatchedThisSession);
	snapshot->runtimeCounterValues.insert(QStringLiteral("aliasesEvaluatedCount"),
	                                      counters.aliasesEvaluatedCount);
	snapshot->runtimeCounterValues.insert(QStringLiteral("aliasesMatchedThisSession"),
	                                      counters.aliasesMatchedThisSession);
	snapshot->runtimeCounterValues.insert(QStringLiteral("timersFiredThisSession"),
	                                      counters.timersFiredThisSession);
	snapshot->runtimeCounterValues.insert(QStringLiteral("lastLineWithIacGa"), counters.lastLineWithIacGa);
	snapshot->runtimeCounterValues.insert(QStringLiteral("outputWindowRedrawCount"),
	                                      counters.outputWindowRedrawCount);
	snapshot->runtimeCounterValues.insert(QStringLiteral("currentActionSource"),
	                                      counters.currentActionSource);
	snapshot->runtimeCounterValues.insert(QStringLiteral("newlinesReceived"),
	                                      QVariant::fromValue(counters.newlinesReceived));
	snapshot->runtimeCounterValues.insert(QStringLiteral("connectTime"), counters.connectTime);
	snapshot->runtimeCounterValues.insert(QStringLiteral("statusTime"), counters.statusTime);
	snapshot->runtimeCounterValues.insert(QStringLiteral("lastFlushTime"), counters.lastFlushTime);
	snapshot->runtimeCounterValues.insert(QStringLiteral("clientStartTime"), counters.clientStartTime);
	snapshot->runtimeCounterValues.insert(QStringLiteral("worldStartTime"), counters.worldStartTime);
	snapshot->runtimeCounterValues.insert(QStringLiteral("scriptFileModTime"), counters.scriptFileModTime);
	snapshot->runtimeCounterValues.insert(QStringLiteral("logFileName"), counters.logFileName);
	snapshot->runtimeCounterValues.insert(QStringLiteral("lastImmediateExpression"),
	                                      counters.lastImmediateExpression);
	snapshot->runtimeCounterValues.insert(QStringLiteral("statusMessage"), counters.statusMessage);
	snapshot->runtimeCounterValues.insert(QStringLiteral("worldFilePath"), counters.worldFilePath);
	snapshot->runtimeCounterValues.insert(QStringLiteral("windowTitleOverride"),
	                                      counters.windowTitleOverride);
	snapshot->runtimeCounterValues.insert(QStringLiteral("mainTitleOverride"), counters.mainTitleOverride);
	snapshot->runtimeCounterValues.insert(QStringLiteral("defaultWorldDirectory"),
	                                      counters.defaultWorldDirectory);
	snapshot->runtimeCounterValues.insert(QStringLiteral("defaultLogDirectory"),
	                                      counters.defaultLogDirectory);
	snapshot->runtimeCounterValues.insert(QStringLiteral("pluginsDirectory"), counters.pluginsDirectory);
	snapshot->runtimeCounterValues.insert(QStringLiteral("peerAddressString"), counters.peerAddressString);
	snapshot->runtimeCounterValues.insert(QStringLiteral("proxyAddressString"), counters.proxyAddressString);
	snapshot->runtimeCounterValues.insert(QStringLiteral("startupDirectory"), counters.startupDirectory);
	snapshot->runtimeCounterValues.insert(QStringLiteral("translatorFile"), counters.translatorFile);
	snapshot->runtimeCounterValues.insert(QStringLiteral("locale"), counters.locale);
	snapshot->runtimeCounterValues.insert(QStringLiteral("fixedPitchFont"), counters.fixedPitchFont);
	snapshot->runtimeCounterValues.insert(QStringLiteral("lastTelnetSubnegotiation"),
	                                      counters.lastTelnetSubnegotiation);
	snapshot->runtimeCounterValues.insert(QStringLiteral("firstSpecialFontPath"),
	                                      counters.firstSpecialFontPath);
	snapshot->runtimeCounterValues.insert(QStringLiteral("preferencesDatabaseName"),
	                                      counters.preferencesDatabaseName);
	snapshot->runtimeCounterValues.insert(QStringLiteral("fileBrowsingDirectory"),
	                                      counters.fileBrowsingDirectory);
	snapshot->runtimeCounterValues.insert(QStringLiteral("stateFilesDirectory"),
	                                      counters.stateFilesDirectory);
	snapshot->runtimeCounterValues.insert(QStringLiteral("lastCommandSent"), counters.lastCommandSent);

	snapshot->worldVariablesSnapshot    = variableSnapshot();
	snapshot->hasWorldVariablesSnapshot = true;
	snapshot->triggerListsByPluginId.insert(QString(), makeTriggerSnapshots(m_triggers));
	snapshot->aliasListsByPluginId.insert(QString(), makeAliasSnapshots(m_aliases));
	snapshot->timerListsByPluginId.insert(QString(), makeTimerSnapshots(m_timers));
	snapshot->triggerWildcardsSnapshot      = m_triggerWildcards;
	snapshot->triggerNamedWildcardsSnapshot = m_triggerNamedWildcards;
	snapshot->aliasWildcardsSnapshot        = m_aliasWildcards;
	snapshot->aliasNamedWildcardsSnapshot   = m_aliasNamedWildcards;
	for (const Plugin &plugin : m_plugins)
	{
		const QString pluginKey = normalizedPluginId(plugin.attributes.value(QStringLiteral("id")));
		if (pluginKey.isEmpty())
			continue;
		snapshot->pluginVariablesSnapshotById.insert(pluginKey, plugin.variables);
		snapshot->triggerListsByPluginId.insert(pluginKey, makeTriggerSnapshots(plugin.triggers));
		snapshot->aliasListsByPluginId.insert(pluginKey, makeAliasSnapshots(plugin.aliases));
		snapshot->timerListsByPluginId.insert(pluginKey, makeTimerSnapshots(plugin.timers));
		snapshot->pluginTriggerWildcardsSnapshotById.insert(pluginKey, plugin.triggerWildcards);
		snapshot->pluginTriggerNamedWildcardsSnapshotById.insert(pluginKey, plugin.triggerNamedWildcards);
		snapshot->pluginAliasWildcardsSnapshotById.insert(pluginKey, plugin.aliasWildcards);
		snapshot->pluginAliasNamedWildcardsSnapshotById.insert(pluginKey, plugin.aliasNamedWildcards);
	}
	snapshot->worldAttributesSnapshot           = m_worldAttributes;
	snapshot->worldMultilineAttributesSnapshot  = m_worldMultilineAttributes;
	snapshot->hasWorldAttributeSnapshot         = true;
	const QMap<QString, QString> entitySnapshot = customEntitySnapshot();
	for (auto it = entitySnapshot.constBegin(); it != entitySnapshot.constEnd(); ++it)
		snapshot->entityValuesByName.insert(it.key(), it.value());
	snapshot->hasEntitySnapshot                 = true;
	snapshot->hasWindowOutputTextRenderSnapshot = true;
	snapshot->windowOutputTextAnsiStreamState   = m_ansiStreamState;
	snapshot->windowOutputTextAnsiRenderState   = makeAnsiRenderSnapshot(m_ansiRenderState);
	snapshot->windowOutputTextMxpStyleState     = makeMxpStyleSnapshot(m_mxpRenderStyle);
	snapshot->windowOutputTextMxpBlockStack     = m_mxpRenderBlockStack;
	snapshot->windowOutputTextMxpLinkOpen       = m_mxpRenderLinkOpen;
	snapshot->windowOutputTextMxpPreDepth       = m_mxpRenderPreDepth;
	snapshot->windowOutputTextMxpStyleStack.reserve(m_mxpRenderStack.size());
	for (const MxpStyleFrame &frame : m_mxpRenderStack)
		snapshot->windowOutputTextMxpStyleStack.push_back(makeMxpStyleFrameSnapshot(frame));
	const QList<TelnetProcessor::CustomElementInfo> customElements = m_telnet.customElementInfos();
	snapshot->windowOutputTextCustomElements.reserve(customElements.size());
	for (const TelnetProcessor::CustomElementInfo &element : customElements)
	{
		LuaCallbackMxpCustomElementSnapshot row;
		row.name       = element.name;
		row.open       = element.open;
		row.command    = element.command;
		row.tag        = element.tag;
		row.flag       = element.flag;
		row.definition = element.definition;
		row.attributes = element.attributes;
		snapshot->windowOutputTextCustomElements.push_back(row);
	}
	snapshot->arrayNamesSnapshot = m_arrays.keys();
	for (auto arrayIt = m_arrays.constBegin(); arrayIt != m_arrays.constEnd(); ++arrayIt)
		snapshot->arraysByName.insert(arrayIt.key(), arrayIt.value().values);
	snapshot->hasArraySnapshot          = true;
	snapshot->chatConnectionIdsSnapshot = chatList();
	for (const long chatId : snapshot->chatConnectionIdsSnapshot)
	{
		for (int infoType = 1; infoType <= 46; ++infoType)
			snapshot->chatInfoValuesById[chatId].insert(infoType, chatInfo(chatId, infoType));
		for (const QString &optionName :
		     {QStringLiteral("can_send_commands"), QStringLiteral("can_send_files"),
		      QStringLiteral("can_snoop"), QStringLiteral("ignore"), QStringLiteral("served"),
		      QStringLiteral("private"), QStringLiteral("user"), QStringLiteral("server"),
		      QStringLiteral("username"), QStringLiteral("group"), QStringLiteral("version"),
		      QStringLiteral("address")})
		{
			snapshot->chatOptionValuesById[chatId].insert(optionName, chatOption(chatId, optionName));
		}
		const QString userName = chatInfo(chatId, 2).toString().trimmed().toLower();
		if (!userName.isEmpty())
			snapshot->chatIdsByLookupKey.insert(userName, chatId);
		snapshot->chatIdsByLookupKey.insert(QString::number(chatId), chatId);
	}
	snapshot->hasChatSnapshot = true;
	for (int index = 1; index <= 8; ++index)
	{
		snapshot->boldAnsiColoursByIndex.insert(index, colorToLong(colourTables.boldAnsi.value(index - 1)));
		snapshot->normalAnsiColoursByIndex.insert(index, normalColour(index));
	}
	QVector<QString> customNames(MAX_CUSTOM);
	QVector<uchar>   customNameResolved(MAX_CUSTOM, 0);
	for (const auto &colour : m_colours)
	{
		const QString groupKey = colour.group.trimmed().toLower();
		if (groupKey != QStringLiteral("custom/custom") && groupKey != QStringLiteral("custom"))
			continue;
		const int index = colourSeqFromAttributes(colour.attributes) - 1;
		if (index < 0 || index >= MAX_CUSTOM || customNameResolved.at(index) != 0)
			continue;
		customNames[index]        = colour.attributes.value(QStringLiteral("name")).trimmed();
		customNameResolved[index] = 1;
	}
	for (int index = 1; index <= MAX_CUSTOM; ++index)
	{
		snapshot->customTextColoursByIndex.insert(index,
		                                          colorToLong(colourTables.customText.value(index - 1)));
		snapshot->customBackgroundColoursByIndex.insert(
		    index, colorToLong(colourTables.customBack.value(index - 1)));
		const QString customName = customNames.value(index - 1);
		snapshot->customColourNamesByIndex.insert(
		    index, customName.isEmpty() ? QStringLiteral("Custom%1").arg(index) : customName);
	}
	snapshot->mapColourSnapshot          = mapColourList();
	snapshot->hasMapColourSnapshot       = true;
	snapshot->mappingEntriesSnapshot     = m_mappingList;
	snapshot->hasMappingEntriesSnapshot  = true;
	snapshot->udpListenerPluginIdsByPort = udpListenerPluginIdsByPort();
	snapshot->udpPortsSnapshot           = snapshot->udpListenerPluginIdsByPort.keys();
	std::ranges::sort(snapshot->udpPortsSnapshot);
	snapshot->hasUdpPortSnapshot = true;
	if (MainWindowHost *host = resolveMainWindowHostForRuntime(this))
	{
		for (const WorldWindowDescriptor &entry : host->worldWindowDescriptors())
		{
			if (!entry.runtime)
				continue;
			const QList<int> ports = entry.runtime == this ? udpPortList() : entry.runtime->udpPortList();
			for (const int port : ports)
			{
				snapshot->usedUdpPortsSnapshot.insert(port);
				snapshot->usedUdpPortReferenceCountsSnapshot[port] += 1;
			}
		}
	}
	if (snapshot->usedUdpPortsSnapshot.isEmpty())
	{
		for (const int port : snapshot->udpPortsSnapshot)
		{
			snapshot->usedUdpPortsSnapshot.insert(port);
			snapshot->usedUdpPortReferenceCountsSnapshot[port] += 1;
		}
	}
	snapshot->hasUsedUdpPortsSnapshot = true;

	if (lineSnapshotPolicy == LuaCallbackLineSnapshotPolicy::CountAndRecentText ||
	    lineSnapshotPolicy == LuaCallbackLineSnapshotPolicy::Full ||
	    lineSnapshotPolicy == LuaCallbackLineSnapshotPolicy::CountAndRecent)
	{
		snapshot->lineBufferSnapshot    = captureLuaCallbackLineBufferSnapshotForDispatch(lineSnapshotPolicy);
		snapshot->hasLineBufferSnapshot = static_cast<bool>(snapshot->lineBufferSnapshot);
		if (snapshot->lineBufferSnapshot)
		{
			snapshot->lineBufferCount        = snapshot->lineBufferSnapshot->lineBufferCount;
			snapshot->recentLinesSnapshot    = snapshot->lineBufferSnapshot->recentLinesSnapshot;
			snapshot->hasRecentLinesSnapshot = true;
		}
	}

	snapshot->databaseNamesSnapshot   = databaseList();
	snapshot->hasDatabaseListSnapshot = true;
	snapshot->hasDatabaseSnapshot     = true;
	for (const QString &databaseName : snapshot->databaseNamesSnapshot)
	{
		LuaCallbackDatabaseSnapshot database;
		if (const auto dbIt = m_databases.constFind(databaseName); dbIt != m_databases.constEnd())
		{
			database.diskName        = dbIt->diskName;
			database.isOpen          = dbIt->db.isValid() && dbIt->db.isOpen();
			database.stmtPrepared    = dbIt->stmtPrepared;
			database.validRow        = dbIt->validRow;
			database.columns         = dbIt->columns;
			database.columnsStatus   = databaseColumns(databaseName);
			database.lastError       = dbIt->lastError;
			database.errorText       = databaseError(databaseName);
			database.totalChanges    = databaseTotalChanges(databaseName);
			database.changes         = databaseChanges(databaseName);
			database.lastInsertRowid = databaseLastInsertRowid(databaseName);
			database.columnNames     = databaseColumnNames(databaseName);
			static_cast<void>(databaseColumnValues(databaseName, database.columnValues));
		}
		snapshot->databaseSnapshotsByName.insert(databaseName, database);
	}
	for (int buffer = 1; buffer <= kMaxSoundBuffers; ++buffer)
	{
		snapshot->soundStatusByBuffer.insert(buffer, soundStatus(buffer));
		snapshot->soundBufferReusableByBuffer.insert(buffer, soundBufferReusableForNativeAudio(buffer));
	}
	for (const Macro &macro : m_macros)
	{
		LuaCallbackAttributeChildrenSnapshot row;
		row.attributes = macro.attributes;
		row.children   = macro.children;
		snapshot->macroEntriesSnapshot.push_back(row);
	}
	snapshot->hasMacroEntriesSnapshot = true;
	for (const Variable &variable : m_variables)
	{
		LuaCallbackAttributeContentSnapshot row;
		row.attributes = variable.attributes;
		row.content    = variable.content;
		snapshot->variableEntriesSnapshot.push_back(row);
	}
	snapshot->hasVariableEntriesSnapshot = true;
	for (const Keypad &keypad : m_keypadEntries)
	{
		LuaCallbackAttributeContentSnapshot row;
		row.attributes = keypad.attributes;
		row.content    = keypad.content;
		snapshot->keypadEntriesSnapshot.push_back(row);
	}
	snapshot->hasKeypadEntriesSnapshot = true;
	for (const AcceleratorSnapshot &accelerator : acceleratorSnapshot())
	{
		LuaCallbackAcceleratorSnapshot row;
		row.key       = accelerator.key;
		row.commandId = accelerator.commandId;
		row.text      = accelerator.text;
		row.sendTo    = accelerator.sendTo;
		snapshot->acceleratorSnapshot.push_back(row);
	}
	snapshot->hasAcceleratorSnapshot = true;

	for (const Plugin &plugin : m_plugins)
	{
		const QString pluginId = normalizedPluginId(plugin.attributes.value(QStringLiteral("id")));
		if (pluginId.isEmpty() || QMudNativePluginRegistry::isBlacklistedId(pluginId))
			continue;
		snapshot->pluginIdsSnapshot.push_back(plugin.attributes.value(QStringLiteral("id")));
		snapshot->pluginIdsByLookupKey.insert(pluginId, pluginId);
		snapshot->pluginNamesById.insert(pluginId, plugin.attributes.value(QStringLiteral("name")));
		QString dir = plugin.directory;
		if (!dir.isEmpty() && !dir.endsWith('/') && !dir.endsWith('\\'))
			dir += '/';
		dir.replace(QLatin1Char('\\'), QLatin1Char('/'));
		snapshot->pluginDirectoriesById.insert(pluginId, dir);
		snapshot->pluginEnabledById.insert(pluginId, plugin.enabled);
		snapshot->pluginEnginesById.insert(pluginId, plugin.lua);
		if (const auto functionsIt = m_pluginLuaFunctionCatalogById.constFind(pluginId);
		    functionsIt != m_pluginLuaFunctionCatalogById.constEnd())
		{
			snapshot->pluginLuaFunctionsById.insert(pluginId, functionsIt.value());
		}
		auto &pluginInfo = snapshot->pluginInfoValuesById[pluginId];
		pluginInfo.insert(1, plugin.attributes.value(QStringLiteral("name")));
		pluginInfo.insert(2, plugin.attributes.value(QStringLiteral("author")));
		pluginInfo.insert(3, plugin.description);
		pluginInfo.insert(4, plugin.script);
		pluginInfo.insert(5, plugin.attributes.value(QStringLiteral("language")));
		pluginInfo.insert(6, plugin.source);
		pluginInfo.insert(7, plugin.attributes.value(QStringLiteral("id")));
		pluginInfo.insert(8, plugin.attributes.value(QStringLiteral("purpose")));
		pluginInfo.insert(9, plugin.triggers.size());
		pluginInfo.insert(10, plugin.aliases.size());
		pluginInfo.insert(11, plugin.timers.size());
		pluginInfo.insert(12, plugin.variables.size());
		pluginInfo.insert(13, plugin.dateWritten.isValid() ? QVariant(plugin.dateWritten) : QVariant());
		pluginInfo.insert(14, plugin.dateModified.isValid() ? QVariant(plugin.dateModified) : QVariant());
		pluginInfo.insert(15, plugin.saveState);
		pluginInfo.insert(16, plugin.lua != nullptr);
		pluginInfo.insert(17, plugin.enabled);
		pluginInfo.insert(18, plugin.requiredVersion);
		pluginInfo.insert(19, plugin.version);
		pluginInfo.insert(20, snapshot->pluginDirectoriesById.value(pluginId));
		pluginInfo.insert(21, snapshot->pluginIdsSnapshot.size());
		pluginInfo.insert(22, plugin.dateInstalled.isValid() ? QVariant(plugin.dateInstalled) : QVariant());
		pluginInfo.insert(23, plugin.callingPluginId);
		pluginInfo.insert(24, 0.0);
		pluginInfo.insert(25, plugin.sequence);
	}
	for (const QString &shimId :
	     {QMudNativePluginRegistry::mushReaderPluginId(), QMudNativePluginRegistry::luaAudioPluginId()})
	{
		if (!nativeShimVisibleForPluginApi(m_plugins, shimId))
			continue;
		QMudNativePluginRegistry::NativePluginMetadata metadata;
		if (!QMudNativePluginRegistry::metadataForShim(shimId, metadata))
			continue;
		const bool enabled = nativeShimPluginInfoEnabled(m_plugins, shimId);
		if (!snapshot->pluginIdsSnapshot.contains(shimId, Qt::CaseInsensitive))
			snapshot->pluginIdsSnapshot.push_back(shimId);
		int visibleIndex = 0;
		for (int i = 0; i < snapshot->pluginIdsSnapshot.size(); ++i)
		{
			if (snapshot->pluginIdsSnapshot.at(i).compare(shimId, Qt::CaseInsensitive) == 0)
			{
				visibleIndex = i + 1;
				break;
			}
		}
		snapshot->pluginIdsByLookupKey.insert(shimId, shimId);
		snapshot->pluginIdsByLookupKey.insert(metadata.name.trimmed().toLower(), shimId);
		snapshot->pluginNamesById.insert(shimId, metadata.name);
		snapshot->pluginDirectoriesById.insert(shimId, metadata.directory);
		snapshot->pluginEnabledById.insert(shimId, enabled);
		if (shimId.compare(QMudNativePluginRegistry::mushReaderPluginId(), Qt::CaseInsensitive) == 0)
		{
			snapshot->nativePluginSpeechEnabledById.insert(
			    shimId, QMudNativePluginRegistry::isMushReaderSpeechEnabled(this));
		}
		QSet<QString> functions;
		for (const QString &routine : QMudNativePluginRegistry::supportedRoutines(shimId))
			functions.insert(routine);
		snapshot->pluginLuaFunctionsById.insert(shimId, functions);
		auto &pluginInfo = snapshot->pluginInfoValuesById[shimId];
		for (int infoType = 1; infoType <= 25; ++infoType)
			pluginInfo.insert(infoType,
			                  QMudNativePluginRegistry::pluginInfo(shimId, infoType, visibleIndex, enabled));
	}
	for (const Plugin &plugin : m_plugins)
	{
		const QString pluginId = normalizedPluginId(plugin.attributes.value(QStringLiteral("id")));
		if (pluginId.isEmpty() || QMudNativePluginRegistry::isBlacklistedId(pluginId))
			continue;
		const QString pluginNameKey = plugin.attributes.value(QStringLiteral("name")).trimmed().toLower();
		if (!pluginNameKey.isEmpty() && !snapshot->pluginIdsByLookupKey.contains(pluginNameKey))
			snapshot->pluginIdsByLookupKey.insert(pluginNameKey, pluginId);
	}
	for (const QString &callbackName :
	     {QStringLiteral("OnPluginBroadcast"), QStringLiteral("OnPluginChatDisplay"),
	      QStringLiteral("OnPluginTrace")})
	{
		snapshot->pluginCallbackPresenceByName.insert(
		    callbackName, const_cast<WorldRuntime *>(this)->hasPluginCallbackRecipient(callbackName));
	}
	const QVector<int> broadcastRecipientIndices =
	    collectExecutablePluginRecipientIndicesWithWarmupFallback(QStringLiteral("OnPluginBroadcast"));
	snapshot->broadcastPluginIdsSnapshot.reserve(broadcastRecipientIndices.size());
	snapshot->broadcastPluginEnginesSnapshot.reserve(broadcastRecipientIndices.size());
	for (const int pluginIndex : broadcastRecipientIndices)
	{
		if (pluginIndex < 0 || pluginIndex >= m_plugins.size())
			continue;
		const Plugin &plugin = m_plugins.at(pluginIndex);
		snapshot->broadcastPluginIdsSnapshot.push_back(plugin.attributes.value(QStringLiteral("id")));
		snapshot->broadcastPluginEnginesSnapshot.push_back(plugin.lua);
	}
	snapshot->hasBroadcastPluginSnapshot = true;
#ifndef NDEBUG
	if (qmudMmStartupDiagHasWatchedEngine(recipients))
	{
		qInfo().noquote() << QStringLiteral("[QMud][MMStartupDiag] snapshot-capture cached=0 targets=%1 %2")
		                         .arg(qmudMmStartupDiagEngineLabels(recipients),
		                              qmudMmStartupDiagBroadcastSnapshotLabel(snapshot));
	}
#endif
	captureMainWindowSnapshotForLuaDispatch(*snapshot, this);

	const QSharedPointer<LuaCallbackMiniWindowSnapshot> cachedBase =
	    QSharedPointer<LuaCallbackMiniWindowSnapshot>::create(*snapshot);
	clearLuaCallbackDispatchVolatileSnapshot(*cachedBase);
	m_luaCallbackDispatchSnapshotBaseCache       = cachedBase;
	m_luaCallbackDispatchSnapshotCacheGeneration = m_luaCallbackDispatchSnapshotGeneration;

	return snapshot;
}

void WorldRuntime::stampLuaCallbackSnapshotActionSource(LuaCallbackMiniWindowSnapshot &snapshot,
                                                        const int                      actionSourceOverride)
{
	if (actionSourceOverride < 0)
		return;
	snapshot.hasActionSourceOverride = true;
	snapshot.actionSourceOverride    = actionSourceOverride;
	if (snapshot.hasRuntimeCountersSnapshot)
		snapshot.runtimeCounterValues.insert(QStringLiteral("currentActionSource"), actionSourceOverride);
}

QSharedPointer<const LuaCallbackMiniWindowSnapshot>
WorldRuntime::captureLuaCallbackSnapshotForDispatchWithActionSource(
    const QVector<QSharedPointer<LuaCallbackEngine>> &recipients,
    const LuaCallbackLineSnapshotPolicy lineSnapshotPolicy, const int actionSourceOverride) const
{
	QSharedPointer<LuaCallbackMiniWindowSnapshot> snapshot =
	    captureLuaCallbackSnapshotForDispatchMutable(recipients, lineSnapshotPolicy);
	if (snapshot)
		stampLuaCallbackSnapshotActionSource(*snapshot, actionSourceOverride);
	return snapshot;
}

LuaBatchDispatchResult WorldRuntime::dispatchLuaStringsAndWildcards(
    const QSharedPointer<LuaCallbackEngine> &engine, const QString &functionName, const QStringList &args,
    const QStringList &wildcards, const QMap<QString, QString> &namedWildcards,
    const QVector<LuaStyleRun> *styleRuns, const bool triggerOutputReplacesMatchedLine,
    const int triggerMatchedLineBufferIndex, const qint64 triggerMatchedLineAbsoluteNumber) const
{
	if (!engine || functionName.isEmpty())
		return {};
	LuaBatchDispatchRequest request;
	request.kind           = LuaBatchDispatchKind::StringsAndWildcards;
	request.engines        = {engine};
	request.functionName   = functionName;
	request.stringListArg  = args;
	request.stringListArg2 = wildcards;
	request.mapArg         = namedWildcards;
	request.lane           = LuaBatchDispatchLane::Callback;
	request.styleRunsArg   = styleRuns ? QSharedPointer<QVector<LuaStyleRun>>::create(*styleRuns)
	                                   : QSharedPointer<const QVector<LuaStyleRun>>{};
	request.triggerOutputReplacesMatchedLine = triggerOutputReplacesMatchedLine;
	request.triggerMatchedLineBufferIndex    = triggerMatchedLineBufferIndex;
	request.triggerMatchedLineAbsoluteNumber = triggerMatchedLineAbsoluteNumber;
	request.inputCritical                    = triggerMatchedLineAbsoluteNumber > 0;
	request.lineSnapshotPolicy               = LuaCallbackLineSnapshotPolicy::CountAndRecentText;
	if (const unsigned short actionSource = currentActionSource(); actionSource != eUnknownActionSource)
	{
		request.actionSourceOverride    = actionSource;
		request.hasActionSourceOverride = true;
	}
	request.miniWindowSnapshotArg = captureLuaCallbackSnapshotForDispatchWithActionSource(
	    request.engines, request.lineSnapshotPolicy,
	    request.hasActionSourceOverride ? request.actionSourceOverride : -1);
	return const_cast<WorldRuntime *>(this)->queuePluginCallbackDispatch(request, true);
}

#ifdef QMUD_ENABLE_LUA_SCRIPTING
LuaBatchDispatchResult WorldRuntime::dispatchLuaCallPluginMarshalling(
    const QSharedPointer<LuaCallbackEngine> &engine, const QString &routine, lua_State *callerState,
    const int firstArg, const QString &callingPluginId,
    const QSharedPointer<const LuaCallbackMiniWindowSnapshot> &snapshot) const
{
	if (!engine || routine.trimmed().isEmpty() || !callerState)
		return {};
	LuaBatchDispatchRequest request;
	request.kind                        = LuaBatchDispatchKind::CallPluginLuaMarshalling;
	request.engines                     = {engine};
	request.functionName                = routine;
	request.intArg1                     = firstArg;
	request.luaStateArg                 = callerState;
	request.refreshCallbackCatalogAfter = true;
	request.applyCallingPluginContext   = true;
	request.callingPluginId             = callingPluginId;
	request.lane                        = LuaBatchDispatchLane::Callback;
	request.miniWindowSnapshotArg       = snapshot;
	return dispatchLuaBatch(request);
}
#endif

void WorldRuntime::dispatchLuaStringsAndWildcardsAsync(
    const QSharedPointer<LuaCallbackEngine> &engine, const QString &functionName, const QStringList &args,
    const QStringList &wildcards, const QMap<QString, QString> &namedWildcards,
    const QVector<LuaStyleRun> *styleRuns, const int actionSourceOverride,
    const bool triggerOutputReplacesMatchedLine, const int triggerMatchedLineBufferIndex,
    const qint64                                               triggerMatchedLineAbsoluteNumber,
    const std::function<void(const LuaBatchDispatchResult &)> &completion) const
{
	if (!engine || functionName.isEmpty())
	{
		if (completion)
			completion({});
		return;
	}
	LuaBatchDispatchRequest request;
	request.kind           = LuaBatchDispatchKind::StringsAndWildcards;
	request.engines        = {engine};
	request.functionName   = functionName;
	request.stringListArg  = args;
	request.stringListArg2 = wildcards;
	request.mapArg         = namedWildcards;
	request.lane           = LuaBatchDispatchLane::Callback;
	request.styleRunsArg   = styleRuns ? QSharedPointer<QVector<LuaStyleRun>>::create(*styleRuns)
	                                   : QSharedPointer<const QVector<LuaStyleRun>>{};
	request.triggerOutputReplacesMatchedLine = triggerOutputReplacesMatchedLine;
	request.triggerMatchedLineBufferIndex    = triggerMatchedLineBufferIndex;
	request.triggerMatchedLineAbsoluteNumber = triggerMatchedLineAbsoluteNumber;
	request.inputCritical                    = triggerMatchedLineAbsoluteNumber > 0;
	request.lineSnapshotPolicy               = LuaCallbackLineSnapshotPolicy::CountAndRecentText;
	if (actionSourceOverride >= 0)
	{
		request.actionSourceOverride    = actionSourceOverride;
		request.hasActionSourceOverride = true;
	}
	else if (const unsigned short actionSource = currentActionSource(); actionSource != eUnknownActionSource)
	{
		request.actionSourceOverride    = actionSource;
		request.hasActionSourceOverride = true;
	}
	request.miniWindowSnapshotArg = captureLuaCallbackSnapshotForDispatchWithActionSource(
	    request.engines, request.lineSnapshotPolicy,
	    request.hasActionSourceOverride ? request.actionSourceOverride : -1);
	const_cast<WorldRuntime *>(this)->queuePluginCallbackDispatchAsync(request, completion);
}

LuaBatchDispatchResult WorldRuntime::dispatchLuaStringsAndWildcards(
    LuaCallbackEngine *engine, const QString &functionName, const QStringList &args,
    const QStringList &wildcards, const QMap<QString, QString> &namedWildcards,
    const QVector<LuaStyleRun> *styleRuns, const bool triggerOutputReplacesMatchedLine,
    const int triggerMatchedLineBufferIndex, const qint64 triggerMatchedLineAbsoluteNumber) const
{
	return dispatchLuaStringsAndWildcards(makeNonOwningLuaEngineRef(engine), functionName, args, wildcards,
	                                      namedWildcards, styleRuns, triggerOutputReplacesMatchedLine,
	                                      triggerMatchedLineBufferIndex, triggerMatchedLineAbsoluteNumber);
}

bool WorldRuntime::dispatchLuaExecuteScript(const QSharedPointer<LuaCallbackEngine> &engine,
                                            const QString &code, const QString &description,
                                            const QVector<LuaStyleRun> *styleRuns,
                                            const bool                  hasTriggerContext,
                                            const bool                  triggerOutputReplacesMatchedLine,
                                            const int                   triggerMatchedLineBufferIndex,
                                            const qint64 triggerMatchedLineAbsoluteNumber) const
{
	if (!engine)
		return false;
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::dispatchLuaExecuteScript");

	const unsigned short previousActionSource = currentActionSource();
	const int effectiveActionSource = hasTriggerContext ? eTriggerFired
	                                                    : (previousActionSource == eUnknownActionSource
	                                                           ? eLuaSandbox
	                                                           : static_cast<int>(previousActionSource));

	LuaBatchDispatchRequest request;
	request.kind         = LuaBatchDispatchKind::ExecuteScript;
	request.lane         = LuaBatchDispatchLane::Callback;
	request.engines      = {engine};
	request.stringArg    = code;
	request.stringArg2   = description;
	request.styleRunsArg = styleRuns ? QSharedPointer<QVector<LuaStyleRun>>::create(*styleRuns)
	                                 : QSharedPointer<const QVector<LuaStyleRun>>{};
	request.executeScriptHasTriggerContext   = hasTriggerContext;
	request.triggerOutputReplacesMatchedLine = triggerOutputReplacesMatchedLine;
	request.triggerMatchedLineBufferIndex    = triggerMatchedLineBufferIndex;
	request.triggerMatchedLineAbsoluteNumber = triggerMatchedLineAbsoluteNumber;
	request.inputCritical                    = hasTriggerContext;
	request.lineSnapshotPolicy      = hasTriggerContext ? LuaCallbackLineSnapshotPolicy::CountAndRecentText
	                                                    : LuaCallbackLineSnapshotPolicy::None;
	request.hasActionSourceOverride = true;
	request.actionSourceOverride    = effectiveActionSource;
	request.miniWindowSnapshotArg   = captureLuaCallbackSnapshotForDispatchWithActionSource(
	    request.engines, request.lineSnapshotPolicy, effectiveActionSource);

	const LuaBatchDispatchResult result =
	    hasTriggerContext ? const_cast<WorldRuntime *>(this)->queuePluginCallbackDispatch(request, true)
	                      : dispatchLuaBatch(request);
	return result.boolResultValid ? result.boolResult : false;
}

void WorldRuntime::dispatchLuaExecuteScriptAsync(
    const QSharedPointer<LuaCallbackEngine> &engine, const QString &code, const QString &description,
    const QVector<LuaStyleRun> *styleRuns, const bool hasTriggerContext,
    const bool triggerOutputReplacesMatchedLine, const int triggerMatchedLineBufferIndex,
    const qint64 triggerMatchedLineAbsoluteNumber, std::function<void(bool)> completion) const
{
	if (!engine)
	{
		if (completion)
			completion(false);
		return;
	}
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::dispatchLuaExecuteScriptAsync");

	const unsigned short previousActionSource = currentActionSource();
	const int effectiveActionSource = hasTriggerContext ? eTriggerFired
	                                                    : (previousActionSource == eUnknownActionSource
	                                                           ? eLuaSandbox
	                                                           : static_cast<int>(previousActionSource));

	LuaBatchDispatchRequest request;
	request.kind         = LuaBatchDispatchKind::ExecuteScript;
	request.lane         = LuaBatchDispatchLane::Callback;
	request.engines      = {engine};
	request.stringArg    = code;
	request.stringArg2   = description;
	request.styleRunsArg = styleRuns ? QSharedPointer<QVector<LuaStyleRun>>::create(*styleRuns)
	                                 : QSharedPointer<const QVector<LuaStyleRun>>{};
	request.executeScriptHasTriggerContext   = hasTriggerContext;
	request.triggerOutputReplacesMatchedLine = triggerOutputReplacesMatchedLine;
	request.triggerMatchedLineBufferIndex    = triggerMatchedLineBufferIndex;
	request.triggerMatchedLineAbsoluteNumber = triggerMatchedLineAbsoluteNumber;
	request.inputCritical                    = hasTriggerContext;
	request.lineSnapshotPolicy      = hasTriggerContext ? LuaCallbackLineSnapshotPolicy::CountAndRecentText
	                                                    : LuaCallbackLineSnapshotPolicy::None;
	request.hasActionSourceOverride = true;
	request.actionSourceOverride    = effectiveActionSource;
	request.miniWindowSnapshotArg   = captureLuaCallbackSnapshotForDispatchWithActionSource(
	    request.engines, request.lineSnapshotPolicy, effectiveActionSource);

	const_cast<WorldRuntime *>(this)->queuePluginCallbackDispatchAsync(
	    request,
	    [completion = std::move(completion)](const LuaBatchDispatchResult &result) mutable
	    {
		    if (completion)
			    completion(result.boolResultValid ? result.boolResult : false);
	    });
}

bool WorldRuntime::dispatchLuaExecuteScript(LuaCallbackEngine *engine, const QString &code,
                                            const QString &description, const QVector<LuaStyleRun> *styleRuns,
                                            const bool   hasTriggerContext,
                                            const bool   triggerOutputReplacesMatchedLine,
                                            const int    triggerMatchedLineBufferIndex,
                                            const qint64 triggerMatchedLineAbsoluteNumber) const
{
	return dispatchLuaExecuteScript(makeNonOwningLuaEngineRef(engine), code, description, styleRuns,
	                                hasTriggerContext, triggerOutputReplacesMatchedLine,
	                                triggerMatchedLineBufferIndex, triggerMatchedLineAbsoluteNumber);
}

bool WorldRuntime::dispatchLuaResetAndLoadScript(const QSharedPointer<LuaCallbackEngine> &engine) const
{
	if (!engine)
		return false;
	LuaBatchDispatchRequest request;
	request.kind                        = LuaBatchDispatchKind::ResetAndLoadScript;
	request.lane                        = LuaBatchDispatchLane::Callback;
	request.engines                     = {engine};
	const LuaBatchDispatchResult result = dispatchLuaBatch(request);
	return result.boolResultValid ? result.boolResult : false;
}

bool WorldRuntime::dispatchLuaResetAndLoadScript(LuaCallbackEngine *engine) const
{
	return dispatchLuaResetAndLoadScript(makeNonOwningLuaEngineRef(engine));
}

void WorldRuntime::applyPackageRestrictions(bool enablePackage)
{
	QVector<QSharedPointer<LuaCallbackEngine>> engines;
	engines.reserve((m_luaCallbacks ? 1 : 0) + m_plugins.size());
	if (m_luaCallbacks)
		engines.push_back(makeNonOwningLuaEngineRef(m_luaCallbacks));
	for (auto &plugin : m_plugins)
	{
		if (plugin.lua)
			engines.push_back(plugin.lua);
	}
	dispatchApplyPackageRestrictions(engines, enablePackage, false);
}

void WorldRuntime::setTraceEnabled(bool enabled)
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this, enabled] { setTraceEnabled(enabled); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::setTraceEnabled");
	m_traceEnabled = enabled;
}

bool WorldRuntime::traceEnabled() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodTrueOnSuccess(const_cast<WorldRuntime *>(this),
		                                     [this] { return traceEnabled(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::traceEnabled");
	return m_traceEnabled;
}

void WorldRuntime::setWorldFileModified(bool modified)
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this, modified] { setWorldFileModified(modified); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::setWorldFileModified");
	m_worldFileModified = modified;
}

bool WorldRuntime::worldFileModified() const
{
	return m_worldFileModified;
}

qint64 WorldRuntime::newlinesReceived() const
{
	return m_newlinesReceived;
}

void WorldRuntime::setScriptFileChanged(bool changed)
{
	m_scriptFileChanged = changed;
}

bool WorldRuntime::scriptFileChanged() const
{
	return m_scriptFileChanged;
}

void WorldRuntime::setLastCommandSent(const QString &value)
{
	m_lastCommandSent = value;
}

QString WorldRuntime::lastCommandSent() const
{
	return m_lastCommandSent;
}

bool WorldRuntime::isMxpActive() const
{
	return m_mxpActive;
}

int WorldRuntime::mxpErrorCount() const
{
	return m_mxpErrors;
}

void WorldRuntime::setOutputFrozen(bool frozen)
{
	m_outputFrozen = frozen;
}

bool WorldRuntime::outputFrozen() const
{
	return m_outputFrozen;
}

const WorldRuntime::TextRectangleSettings &WorldRuntime::textRectangle() const
{
	return m_textRectangle;
}

void WorldRuntime::setTextRectangle(const TextRectangleSettings &settings)
{
	if (textRectangleSettingsEqual(m_textRectangle, settings))
		return;

	m_textRectangle = settings;
	invalidateLuaCallbackDispatchSnapshot();
	++m_suppressWorldOutputResizedCallbacks;
	if (WorldView *view = this->view())
	{
		view->updateWrapMargin();
		view->update();
	}
	updateTelnetWindowSizeForNaws();
	if (m_suppressWorldOutputResizedCallbacks > 0)
		--m_suppressWorldOutputResizedCallbacks;
}

QList<int> WorldRuntime::udpPortList() const
{
	QList<int> ports;
	ports.reserve(m_udpListeners.size());
	for (auto it = m_udpListeners.cbegin(); it != m_udpListeners.cend(); ++it)
		ports.append(it.key());
	return ports;
}

QHash<int, QString> WorldRuntime::udpListenerPluginIdsByPort() const
{
	QHash<int, QString> owners;
	owners.reserve(m_udpListeners.size());
	for (auto it = m_udpListeners.cbegin(); it != m_udpListeners.cend(); ++it)
		owners.insert(it.key(), it->pluginId);
	return owners;
}

int WorldRuntime::udpListen(const QString &pluginId, const QString &ip, int port, const QString &script)
{
	if (port < 1 || port > 65535)
		return eBadParameter;

	if (auto existing = m_udpListeners.find(port); existing != m_udpListeners.end())
	{
		if (existing->pluginId.compare(pluginId, Qt::CaseInsensitive) != 0)
			return eBadParameter;
		if (existing->socket)
		{
			existing->socket->close();
			delete existing->socket;
		}
		m_udpListeners.erase(existing);
	}

	if (script.isEmpty())
		return eNoNameSpecified;

	QHostAddress bindAddress;
	if (ip.isEmpty())
		bindAddress = QHostAddress::Any;
	else
		bindAddress = QHostAddress(ip);
	if (bindAddress.isNull())
		return eBadParameter;

	auto       socket = std::make_unique<QUdpSocket>(this);
	const bool bound  = socket->bind(bindAddress, static_cast<quint16>(port),
	                                 QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
	if (!bound)
		return eBadParameter;

	UdpListener listener;
	listener.socket      = socket.get();
	listener.pluginId    = pluginId;
	listener.script      = script;
	listener.bindAddress = ip;
	m_udpListeners.insert(port, listener);
	QUdpSocket const *udpSocket = socket.release();

	connect(udpSocket, &QUdpSocket::readyRead, this,
	        [this, port]
	        {
		        const auto it = m_udpListeners.find(port);
		        if (it == m_udpListeners.end() || !it->socket)
			        return;
		        QUdpSocket *udp = it->socket;
		        while (udp->hasPendingDatagrams())
		        {
			        QByteArray datagram;
			        datagram.resize(static_cast<int>(udp->pendingDatagramSize()));
			        QHostAddress sender;
			        quint16      senderPort = 0;
			        const qint64 bytes =
			            udp->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
			        if (bytes <= 0)
				        continue;
			        datagram.resize(static_cast<int>(bytes));
			        const QString payload = QString::fromLatin1(datagram.constData(), datagram.size());
			        callPlugin(it->pluginId, it->script, payload, it->pluginId);
		        }
	        });

	return eOK;
}

int WorldRuntime::udpSend(const QString &ip, int port, const QByteArray &payload)
{
	if (port < 1 || port > 65535)
		return eBadParameter;
	QHostAddress const address(ip);
	if (address.isNull())
		return -1;

	QUdpSocket socket;
	if (const qint64 bytes = socket.writeDatagram(payload, address, static_cast<quint16>(port)); bytes < 0)
		return socket.error();
	return 0;
}

QString WorldRuntime::lastTelnetSubnegotiation() const
{
	return m_lastTelnetSubnegotiation;
}

#if QMUD_ENABLE_SOUND
static double normalizeSoundVolume(double volume)
{
	if (volume > 0.0 || volume < -100.0)
		volume = 0.0;
	const double scaled = 1.0 + (volume / 100.0);
	return qBound(0.0, scaled, 1.0);
}

static QString mediaPlayerErrorName(const QMediaPlayer::Error error)
{
	switch (error)
	{
	case QMediaPlayer::NoError:
		return QStringLiteral("NoError");
	case QMediaPlayer::ResourceError:
		return QStringLiteral("ResourceError");
	case QMediaPlayer::FormatError:
		return QStringLiteral("FormatError");
	case QMediaPlayer::NetworkError:
		return QStringLiteral("NetworkError");
	case QMediaPlayer::AccessDeniedError:
		return QStringLiteral("AccessDeniedError");
	}
	return QStringLiteral("UnknownError");
}

bool WorldRuntime::soundBufferHasBackend(const SoundBuffer &entry)
{
	return entry.effect || entry.player;
}

bool WorldRuntime::soundBufferIsPlaying(const SoundBuffer &entry)
{
	if (entry.effect)
		return entry.effect->isPlaying();
	if (entry.player)
		return entry.player->playbackState() == QMediaPlayer::PlayingState;
	return false;
}

bool WorldRuntime::soundBufferIsReusable(const SoundBuffer &entry)
{
	if (!soundBufferHasBackend(entry))
		return true;
	if (!entry.playbackStarted)
		return false;
	if (entry.looping)
		return false;
	return !soundBufferIsPlaying(entry);
}

bool WorldRuntime::soundBufferIsStealable(const SoundBuffer &entry)
{
	return soundBufferHasBackend(entry) && entry.playbackStarted;
}

void WorldRuntime::clearSoundBuffer(SoundBuffer &entry)
{
	QSoundEffect   *effect   = std::exchange(entry.effect, nullptr);
	QMediaPlayer   *player   = std::exchange(entry.player, nullptr);
	QTemporaryFile *tempFile = std::exchange(entry.tempFile, nullptr);
	entry.audioOutput        = nullptr;
	entry.playbackStarted    = false;
	entry.looping            = false;
	entry.volume             = 1.0;
	entry.pan                = 0.0;

	if (effect)
	{
		QObject::disconnect(effect, nullptr, nullptr, nullptr);
		effect->stop();
		effect->setSource(QUrl());
		effect->deleteLater();
	}
	if (player)
	{
		QObject::disconnect(player, nullptr, nullptr, nullptr);
		player->stop();
		player->setSource(QUrl());
		player->deleteLater();
	}
	delete tempFile;
}

WorldRuntime::SoundBuffer *WorldRuntime::soundBufferForEffectSignal(const QSoundEffect *effect,
                                                                    const int           targetBuffer)
{
	if (!effect || targetBuffer < 1 || targetBuffer > kMaxSoundBuffers)
		return nullptr;
	SoundBuffer &current = m_soundBuffers[targetBuffer - 1];
	return current.effect == effect ? &current : nullptr;
}

WorldRuntime::SoundBuffer *WorldRuntime::soundBufferForPlayerSignal(const QMediaPlayer *player,
                                                                    const int           targetBuffer)
{
	if (!player || targetBuffer < 1 || targetBuffer > kMaxSoundBuffers)
		return nullptr;
	SoundBuffer &current = m_soundBuffers[targetBuffer - 1];
	return current.player == player ? &current : nullptr;
}

bool WorldRuntime::shouldUseMediaPlayerForSoundFile(const QString &fileName)
{
	return soundFilePlaybackBackendForFile(fileName) == SoundFilePlaybackBackend::QMediaPlayer;
}

int WorldRuntime::playSound(int buffer, const QString &fileName, bool loop, double volume, double pan)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eCannotPlaySound, [this, buffer, fileName, loop, volume, pan]
		                          { return playSound(buffer, fileName, loop, volume, pan); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::playSound");
	if (fileName.isEmpty() && buffer == 0)
		return eBadParameter;
	if (!fileName.isEmpty() && !m_inPlaySoundPluginCallback)
	{
		m_inPlaySoundPluginCallback = true;
		if (firePluginPlaySound(fileName))
		{
			m_inPlaySoundPluginCallback = false;
			return eOK;
		}
		m_inPlaySoundPluginCallback = false;
	}
	return playSoundBypassingPluginCallbacks(buffer, fileName, loop, volume, pan);
}

int WorldRuntime::playSoundBypassingPluginCallbacks(int buffer, const QString &fileName, bool loop,
                                                    double volume, double pan)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(
		    this, eCannotPlaySound, [this, buffer, fileName, loop, volume, pan]
		    { return playSoundBypassingPluginCallbacks(buffer, fileName, loop, volume, pan); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::playSoundBypassingPluginCallbacks");
	if (fileName.isEmpty() && buffer == 0)
		return eBadParameter;
	if (buffer >= 1 && buffer <= kMaxSoundBuffers && fileName.isEmpty())
	{
		SoundBuffer &entry = m_soundBuffers[buffer - 1];
		if (!soundBufferHasBackend(entry))
			return eBadParameter;
		if (!soundBufferIsPlaying(entry))
			return eCannotPlaySound;
		entry.looping = loop;
		entry.volume  = normalizeSoundVolume(volume);
		entry.pan     = pan;
		if (entry.effect)
		{
			entry.effect->setLoopCount(loop ? QSoundEffect::Infinite : 1);
			entry.effect->setVolume(static_cast<float>(entry.volume));
		}
		if (entry.player)
			entry.player->setLoops(loop ? QMediaPlayer::Infinite : QMediaPlayer::Once);
		if (entry.audioOutput)
			entry.audioOutput->setVolume(static_cast<float>(entry.volume));
		return eOK;
	}

	if (buffer < 0 || buffer > kMaxSoundBuffers)
		return eBadParameter;

	int targetBuffer = buffer;
	if (targetBuffer == 0)
	{
		for (int i = 0; i < (kMaxSoundBuffers / 2); ++i)
		{
			if (!soundBufferHasBackend(m_soundBuffers[i]))
			{
				targetBuffer = i + 1;
				break;
			}
		}
	}
	if (targetBuffer == 0)
	{
		for (int i = 0; i < kMaxSoundBuffers; ++i)
		{
			if (soundBufferIsReusable(m_soundBuffers[i]))
			{
				targetBuffer = i + 1;
				break;
			}
		}
	}
	if (targetBuffer == 0)
	{
		for (int i = 0; i < kMaxSoundBuffers; ++i)
		{
			if (soundBufferIsStealable(m_soundBuffers[i]))
			{
				targetBuffer = i + 1;
				break;
			}
		}
	}
	if (targetBuffer == 0)
		return eCannotPlaySound;

	if (targetBuffer < 1 || targetBuffer > kMaxSoundBuffers)
		return eBadParameter;

	SoundBuffer &entry = m_soundBuffers[targetBuffer - 1];
	clearSoundBuffer(entry);

	const QString relative = QMudPluginPathUtils::legacyPathRelativeToQmudHome(fileName);
	QString       resolved;
	QStringList   candidates;
	if (!relative.isEmpty() && !relative.startsWith(QStringLiteral("sounds/"), Qt::CaseInsensitive))
		candidates.push_back(QStringLiteral("sounds/%1").arg(relative));
	candidates.push_back(fileName);

	for (const QString &candidate : candidates)
	{
		QString normalizedCandidate;
		QString error;
		if (!QMudPluginPathUtils::resolveInsideQmudHome(m_startupDirectory, candidate, &normalizedCandidate,
		                                                &error))
			continue;
		if (QFileInfo::exists(normalizedCandidate))
		{
			resolved = normalizedCandidate;
			break;
		}
	}

	if (resolved.isEmpty())
		return eFileNotFound;

	QMudNativePluginRegistry::LuaAudioRuntimeBufferState luaAudioState;
	const quint64                                        luaAudioGeneration =
	    QMudNativePluginRegistry::luaAudioRuntimeBufferState(this, targetBuffer, luaAudioState)
	        ? luaAudioState.generation
	        : 0;
	entry.looping = loop;
	entry.volume  = normalizeSoundVolume(volume);
	entry.pan     = pan;
	if (shouldUseMediaPlayerForSoundFile(resolved))
	{
		auto                  *player      = new QMediaPlayer(this);
		auto                  *audioOutput = new QAudioOutput(player);
		QPointer<QMediaPlayer> playerGuard(player);
		player->setAudioOutput(audioOutput);
		player->setLoops(loop ? QMediaPlayer::Infinite : QMediaPlayer::Once);
		audioOutput->setVolume(static_cast<float>(entry.volume));
		const QString mediaBackend = QString::fromUtf8(qgetenv("QT_MEDIA_BACKEND")).trimmed();
		const QString backendText  = mediaBackend.isEmpty() ? QStringLiteral("default") : mediaBackend;
		const auto    reportMediaPlayerError =
		    [this, resolved, backendText](const QMediaPlayer::Error error, const QString &errorText)
		{
			const QString trimmedError = errorText.trimmed();
			const QString detail =
			    trimmedError.isEmpty() ? QStringLiteral("unknown media-player error") : trimmedError;
			const QString message =
			    QStringLiteral("QMud sound playback failed: %1 (file: %2, Qt media backend: %3, error: "
			                   "%4)")
			        .arg(detail, resolved, backendText, mediaPlayerErrorName(error));
			qWarning().noquote() << message;
			outputText(message, true, true);
		};
		connect(player, &QMediaPlayer::errorOccurred, this,
		        [this, playerGuard, targetBuffer, luaAudioGeneration,
		         reportMediaPlayerError](const QMediaPlayer::Error error, const QString &errorText)
		        {
			        QMediaPlayer *player  = playerGuard.data();
			        SoundBuffer  *current = soundBufferForPlayerSignal(player, targetBuffer);
			        if (!current)
				        return;

			        if (luaAudioGeneration != 0)
				        QMudNativePluginRegistry::luaAudioReleaseRuntimeBufferIfGeneration(
				            this, targetBuffer, luaAudioGeneration);
			        reportMediaPlayerError(error, errorText);
			        clearSoundBuffer(*current);
		        });
		connect(player, &QMediaPlayer::playbackStateChanged, this,
		        [this, playerGuard, loop, targetBuffer,
		         luaAudioGeneration](const QMediaPlayer::PlaybackState state)
		        {
			        SoundBuffer *current = soundBufferForPlayerSignal(playerGuard.data(), targetBuffer);
			        if (!current)
				        return;
			        if (state == QMediaPlayer::PlayingState)
			        {
				        current->playbackStarted = true;
				        return;
			        }
			        if (loop || state != QMediaPlayer::StoppedState || !current->playbackStarted ||
			            luaAudioGeneration == 0)
				        return;
			        QMudNativePluginRegistry::luaAudioReleaseRuntimeBufferIfGeneration(this, targetBuffer,
			                                                                           luaAudioGeneration);
		        });
		player->setSource(QUrl::fromLocalFile(resolved));
		if (player->error() != QMediaPlayer::NoError)
		{
			reportMediaPlayerError(player->error(), player->errorString());
			delete player;
			entry.playbackStarted = false;
			entry.looping         = false;
			entry.volume          = 1.0;
			entry.pan             = 0.0;
			return eCannotPlaySound;
		}
		entry.player      = player;
		entry.audioOutput = audioOutput;
		player->play();
		return eOK;
	}

	auto                  *effect = new QSoundEffect(this);
	QPointer<QSoundEffect> effectGuard(effect);
	connect(effect, &QSoundEffect::statusChanged, this,
	        [this, effectGuard, targetBuffer, luaAudioGeneration]
	        {
		        QSoundEffect *effect  = effectGuard.data();
		        SoundBuffer  *current = soundBufferForEffectSignal(effect, targetBuffer);
		        if (!current)
			        return;
		        if (effect->status() != QSoundEffect::Error)
			        return;
		        if (luaAudioGeneration != 0)
			        QMudNativePluginRegistry::luaAudioReleaseRuntimeBufferIfGeneration(this, targetBuffer,
			                                                                           luaAudioGeneration);
		        clearSoundBuffer(*current);
	        });
	effect->setSource(QUrl::fromLocalFile(resolved));
	if (effect->status() == QSoundEffect::Error)
	{
		delete effect;
		entry.looping = false;
		entry.volume  = 1.0;
		entry.pan     = 0.0;
		return eCannotPlaySound;
	}
	effect->setLoopCount(loop ? QSoundEffect::Infinite : 1);
	effect->setVolume(static_cast<float>(entry.volume));
	connect(effect, &QSoundEffect::playingChanged, this,
	        [this, effectGuard, loop, targetBuffer, luaAudioGeneration]
	        {
		        QSoundEffect *effect  = effectGuard.data();
		        SoundBuffer  *current = soundBufferForEffectSignal(effect, targetBuffer);
		        if (!current)
			        return;
		        if (effect->isPlaying())
		        {
			        current->playbackStarted = true;
			        return;
		        }
		        if (loop || !current->playbackStarted || luaAudioGeneration == 0)
			        return;
		        QMudNativePluginRegistry::luaAudioReleaseRuntimeBufferIfGeneration(this, targetBuffer,
		                                                                           luaAudioGeneration);
	        });
	entry.effect = effect;
	effect->play();
	return eOK;
}

int WorldRuntime::playSoundMemory(int buffer, const QByteArray &data, bool loop, double volume, double pan)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eCannotPlaySound, [this, buffer, data, loop, volume, pan]
		                          { return playSoundMemory(buffer, data, loop, volume, pan); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::playSoundMemory");
	if (data.isEmpty())
		return eBadParameter;
	if (buffer < 0 || buffer > kMaxSoundBuffers)
		return eBadParameter;
	int targetBuffer = buffer;
	if (targetBuffer == 0)
	{
		for (int i = 0; i < (kMaxSoundBuffers / 2); ++i)
		{
			if (!soundBufferHasBackend(m_soundBuffers[i]))
			{
				targetBuffer = i + 1;
				break;
			}
		}
	}
	if (targetBuffer == 0)
	{
		for (int i = 0; i < kMaxSoundBuffers; ++i)
		{
			if (soundBufferIsReusable(m_soundBuffers[i]))
			{
				targetBuffer = i + 1;
				break;
			}
		}
	}
	if (targetBuffer == 0)
	{
		for (int i = 0; i < kMaxSoundBuffers; ++i)
		{
			if (soundBufferIsStealable(m_soundBuffers[i]))
			{
				targetBuffer = i + 1;
				break;
			}
		}
	}
	if (targetBuffer == 0)
		return eCannotPlaySound;

	if (targetBuffer < 1 || targetBuffer > kMaxSoundBuffers)
		return eBadParameter;

	SoundBuffer &entry = m_soundBuffers[targetBuffer - 1];
	clearSoundBuffer(entry);

	auto *temp = new QTemporaryFile(this);
	temp->setAutoRemove(true);
	if (!temp->open())
	{
		delete temp;
		return eCannotPlaySound;
	}
	if (temp->write(data) != data.size())
	{
		delete temp;
		return eCannotPlaySound;
	}
	temp->flush();
	entry.tempFile = temp;

	auto                  *effect = new QSoundEffect(this);
	QPointer<QSoundEffect> effectGuard(effect);
	connect(effect, &QSoundEffect::statusChanged, this,
	        [this, effectGuard, targetBuffer]
	        {
		        QSoundEffect *effect  = effectGuard.data();
		        SoundBuffer  *current = soundBufferForEffectSignal(effect, targetBuffer);
		        if (!current)
			        return;
		        if (effect->status() != QSoundEffect::Error)
			        return;
		        clearSoundBuffer(*current);
	        });
	effect->setSource(QUrl::fromLocalFile(temp->fileName()));
	if (effect->status() == QSoundEffect::Error)
	{
		delete effect;
		clearSoundBuffer(entry);
		return eCannotPlaySound;
	}
	entry.looping = loop;
	entry.volume  = normalizeSoundVolume(volume);
	entry.pan     = pan;
	effect->setLoopCount(loop ? QSoundEffect::Infinite : 1);
	effect->setVolume(static_cast<float>(entry.volume));
	connect(effect, &QSoundEffect::playingChanged, this,
	        [this, effectGuard, targetBuffer]
	        {
		        QSoundEffect *effect  = effectGuard.data();
		        SoundBuffer  *current = soundBufferForEffectSignal(effect, targetBuffer);
		        if (!current || !effect->isPlaying())
			        return;
		        current->playbackStarted = true;
	        });
	entry.effect = effect;
	effect->play();
	return eOK;
}

int WorldRuntime::stopSound(int buffer)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eCannotPlaySound, [this, buffer] { return stopSound(buffer); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::stopSound");
	if (buffer == 0 && !m_inCancelSoundPluginCallback)
	{
		m_inCancelSoundPluginCallback = true;
		if (firePluginPlaySound(QString()))
		{
			m_inCancelSoundPluginCallback = false;
			return eOK;
		}
		m_inCancelSoundPluginCallback = false;
	}
	return stopSoundBypassingPluginCallbacks(buffer);
}

int WorldRuntime::stopSoundBypassingPluginCallbacks(int buffer)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eCannotPlaySound,
		                          [this, buffer] { return stopSoundBypassingPluginCallbacks(buffer); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::stopSoundBypassingPluginCallbacks");
	if (buffer == 0)
	{
		for (SoundBuffer &entry : m_soundBuffers)
			clearSoundBuffer(entry);
		return eOK;
	}

	if (buffer < 1 || buffer > kMaxSoundBuffers)
		return eBadParameter;

	SoundBuffer &entry = m_soundBuffers[buffer - 1];
	clearSoundBuffer(entry);
	return eOK;
}

int WorldRuntime::soundStatus(int buffer) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), -3,
		                          [this, buffer] { return soundStatus(buffer); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::soundStatus");
	if (buffer < 1 || buffer > kMaxSoundBuffers)
		return -1;
	const SoundBuffer &entry = m_soundBuffers[buffer - 1];
	if (!soundBufferHasBackend(entry))
		return -2;
	if (!soundBufferIsPlaying(entry))
		return 0;
	return entry.looping ? 2 : 1;
}

bool WorldRuntime::soundBufferReusableForNativeAudio(int buffer) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), false,
		                          [this, buffer] { return soundBufferReusableForNativeAudio(buffer); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::soundBufferReusableForNativeAudio");
	if (buffer < 1 || buffer > kMaxSoundBuffers)
		return false;
	return soundBufferIsReusable(m_soundBuffers[buffer - 1]);
}
#else
int WorldRuntime::playSound(int, const QString &, bool, double, double)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eCannotPlaySound,
		                          [this] { return playSound(0, QString(), false, 0.0, 0.0); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::playSound");
	return eCannotPlaySound;
}

int WorldRuntime::playSoundBypassingPluginCallbacks(int, const QString &, bool, double, double)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(
		    this, eCannotPlaySound,
		    [this] { return playSoundBypassingPluginCallbacks(0, QString(), false, 0.0, 0.0); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::playSoundBypassingPluginCallbacks");
	return eCannotPlaySound;
}

int WorldRuntime::playSoundMemory(int, const QByteArray &, bool, double, double)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eCannotPlaySound,
		                          [this] { return playSoundMemory(0, QByteArray(), false, 0.0, 0.0); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::playSoundMemory");
	return eCannotPlaySound;
}

int WorldRuntime::stopSound(int)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eCannotPlaySound, [this] { return stopSound(0); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::stopSound");
	return eCannotPlaySound;
}

int WorldRuntime::stopSoundBypassingPluginCallbacks(int)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eCannotPlaySound,
		                          [this] { return stopSoundBypassingPluginCallbacks(0); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::stopSoundBypassingPluginCallbacks");
	return eCannotPlaySound;
}

int WorldRuntime::soundStatus(int) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), -3, [this] { return soundStatus(0); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::soundStatus");
	return -3;
}

bool WorldRuntime::soundBufferReusableForNativeAudio(int) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), false,
		                          [this] { return soundBufferReusableForNativeAudio(0); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::soundBufferReusableForNativeAudio");
	return false;
}
#endif

long WorldRuntime::adjustColour(long colour, short method)
{
	const auto   base = static_cast<QMudColorRef>(colour & 0x00FFFFFF);
	const QColor baseColor(qmudRed(base), qmudGreen(base), qmudBlue(base));

	auto         adjustedHsl = [&](double saturationDelta, double luminanceDelta) -> long
	{
		QColor hsl        = baseColor.toHsl();
		float  hue        = 0.0f;
		float  saturation = 0.0f;
		float  luminance  = 0.0f;
		float  alpha      = 1.0f;
		hsl.getHslF(&hue, &saturation, &luminance, &alpha);

		// For achromatic colors Qt reports hue as -1; behavior treated this as 0.
		if (hue < 0.0f)
			hue = 0.0f;

		saturation = qBound(0.0f, saturation + static_cast<float>(saturationDelta), 1.0f);
		luminance  = qBound(0.0f, luminance + static_cast<float>(luminanceDelta), 1.0f);
		hsl.setHslF(hue, saturation, luminance, alpha);

		const QColor rgb = hsl.toRgb();
		return qmudRgb(rgb.red(), rgb.green(), rgb.blue());
	};

	switch (method)
	{
	case ADJUST_COLOUR_INVERT:
		return qmudRgb(255 - qmudRed(base), 255 - qmudGreen(base), 255 - qmudBlue(base));
	case ADJUST_COLOUR_LIGHTER:
		return adjustedHsl(0.0, 0.02);
	case ADJUST_COLOUR_DARKER:
		return adjustedHsl(0.0, -0.02);
	case ADJUST_COLOUR_LESS_COLOUR:
		return adjustedHsl(-0.05, 0.0);
	case ADJUST_COLOUR_MORE_COLOUR:
		return adjustedHsl(0.05, 0.0);
	default:
		return base;
	}
}

long WorldRuntime::blendPixel(long blend, long base, short mode, double opacity)
{
	long out = 0;
	if (!blendPixelInternal(blend, base, mode, opacity, out))
		return -2;
	return out;
}

long WorldRuntime::filterPixel(long pixel, short operation, double options)
{
	long r = qmudRed(static_cast<QMudColorRef>(pixel));
	long g = qmudGreen(static_cast<QMudColorRef>(pixel));
	long b = qmudBlue(static_cast<QMudColorRef>(pixel));

	switch (operation)
	{
	case 1:
	{
		const double threshold = options / 100.0;
		r += roundedToLong((128.0 - runtimeRandomUnit() * 256.0) * threshold);
		g += roundedToLong((128.0 - runtimeRandomUnit() * 256.0) * threshold);
		b += roundedToLong((128.0 - runtimeRandomUnit() * 256.0) * threshold);
		break;
	}
	case 2:
	{
		const double threshold = options / 100.0;
		const long   j         = roundedToLong((128.0 - runtimeRandomUnit() * 256.0) * threshold);
		r += j;
		g += j;
		b += j;
		break;
	}
	case 7:
		r += roundedToLong(options);
		g += roundedToLong(options);
		b += roundedToLong(options);
		break;
	case 8:
	{
		double c = (static_cast<double>(r) - 128.0) * options;
		r        = roundedToLong(c + 128.0);
		c        = (static_cast<double>(g) - 128.0) * options;
		g        = roundedToLong(c + 128.0);
		c        = (static_cast<double>(b) - 128.0) * options;
		b        = roundedToLong(c + 128.0);
		break;
	}
	case 9:
	{
		if (options < 0.0)
			options = 0.0;
		double c = static_cast<double>(r) / 255.0;
		c        = pow(c, options);
		r        = roundedToLong(c * 255.0);
		c        = static_cast<double>(g) / 255.0;
		c        = pow(c, options);
		g        = roundedToLong(c * 255.0);
		c        = static_cast<double>(b) / 255.0;
		c        = pow(c, options);
		b        = roundedToLong(c * 255.0);
		break;
	}
	case 10:
		r += roundedToLong(options);
		break;
	case 11:
	{
		const double c = (static_cast<double>(r) - 128.0) * options;
		r              = roundedToLong(c + 128.0);
		break;
	}
	case 12:
	{
		if (options < 0.0)
			options = 0.0;
		double c = static_cast<double>(r) / 255.0;
		c        = pow(c, options);
		r        = roundedToLong(c * 255.0);
		break;
	}
	case 13:
		g += roundedToLong(options);
		break;
	case 14:
	{
		const double c = (static_cast<double>(g) - 128.0) * options;
		g              = roundedToLong(c + 128.0);
		break;
	}
	case 15:
	{
		if (options < 0.0)
			options = 0.0;
		double c = static_cast<double>(g) / 255.0;
		c        = pow(c, options);
		g        = roundedToLong(c * 255.0);
		break;
	}
	case 16:
		b += roundedToLong(options);
		break;
	case 17:
	{
		const double c = (static_cast<double>(b) - 128.0) * options;
		b              = roundedToLong(c + 128.0);
		break;
	}
	case 18:
	{
		if (options < 0.0)
			options = 0.0;
		double c = static_cast<double>(b) / 255.0;
		c        = pow(c, options);
		b        = roundedToLong(c * 255.0);
		break;
	}
	case 19:
	{
		const double c = (static_cast<double>(r) + static_cast<double>(g) + static_cast<double>(b)) / 3.0;
		r              = roundedToLong(c);
		g              = roundedToLong(c);
		b              = roundedToLong(c);
		break;
	}
	case 20:
	{
		double c =
		    static_cast<double>(b) * 0.11 + static_cast<double>(g) * 0.59 + static_cast<double>(r) * 0.30;
		c /= 3.0;
		r = roundedToLong(c);
		g = roundedToLong(c);
		b = roundedToLong(c);
		break;
	}
	case 21:
		r = roundedToLong(static_cast<double>(r) * options);
		g = roundedToLong(static_cast<double>(g) * options);
		b = roundedToLong(static_cast<double>(b) * options);
		break;
	case 22:
		r = roundedToLong(static_cast<double>(r) * options);
		break;
	case 23:
		g = roundedToLong(static_cast<double>(g) * options);
		break;
	case 24:
		b = roundedToLong(static_cast<double>(b) * options);
		break;
	case 27:
		break;
	default:
		return -1;
	}

	return qmudRgb(QMudBlend::clampToByteRange(static_cast<int>(r)),
	               QMudBlend::clampToByteRange(static_cast<int>(g)),
	               QMudBlend::clampToByteRange(static_cast<int>(b)));
}

QString WorldRuntime::base64Encode(const QString &text, bool multiLine)
{
	const QByteArray utf8 = text.toUtf8();
	return qmudEncodeBase64Text(utf8, multiLine);
}

QString WorldRuntime::base64Decode(const QString &text)
{
	QByteArray input = text.toUtf8();
	input.replace('\r', "");
	input.replace('\n', "");
	const QByteArray decoded = QByteArray::fromBase64(input, QByteArray::Base64Encoding);
	return QString::fromUtf8(decoded);
}

QString WorldRuntime::rgbColourToName(long colour)
{
	return qmudColourToName(colour);
}

QString WorldRuntime::clientVersionString()
{
	return QString::fromLatin1(kVersionString);
}

QString WorldRuntime::errorDesc(int code)
{
	for (const IntFlagsPair *p = kErrorDescriptionMappingTable; p->value != nullptr; ++p)
	{
		if (p->key == code)
			return QString::fromLatin1(p->value);
	}
	return {};
}

QString WorldRuntime::generateName()
{
	return qmudGenerateName();
}

QStringList WorldRuntime::internalCommandsList()
{
	QStringList commands;
	for (int i = 0; kCommandIdMappingTable[i].commandId; ++i)
		commands.append(QString::fromLatin1(kCommandIdMappingTable[i].commandName));
	return commands;
}

void WorldRuntime::setOutputFontMetrics(int height, int width)
{
	m_outputFontHeight = height;
	m_outputFontWidth  = width;
}

void WorldRuntime::setInputFontMetrics(int height, int width)
{
	m_inputFontHeight = height;
	m_inputFontWidth  = width;
}

int WorldRuntime::outputFontHeight() const
{
	return m_outputFontHeight;
}

int WorldRuntime::outputFontWidth() const
{
	return m_outputFontWidth;
}

int WorldRuntime::inputFontHeight() const
{
	return m_inputFontHeight;
}

int WorldRuntime::inputFontWidth() const
{
	return m_inputFontWidth;
}

long WorldRuntime::colourNameToRGB(const QString &name)
{
	QString trimmed = name.trimmed();
	if (trimmed.size() >= 2)
	{
		const QChar first = trimmed.front();
		const QChar last  = trimmed.back();
		if ((first == QLatin1Char('"') && last == QLatin1Char('"')) ||
		    (first == QLatin1Char('\'') && last == QLatin1Char('\'')))
			trimmed = trimmed.mid(1, trimmed.size() - 2).trimmed();
	}
	if (trimmed.isEmpty())
		return -1;
	bool       ok      = false;
	const long numeric = trimmed.toLong(&ok);
	if (ok)
		return (numeric < 0 || numeric > 0xFFFFFF) ? -1 : numeric;
	const QString lowered = trimmed.toLower();
	QColor const  color(lowered);
	if (color.isValid())
		return colorToLong(color);

	static const QHash<QString, long> kMxpColors = []
	{
		auto toRef = [](unsigned int rgb) -> long
		{
			const int r = static_cast<int>((rgb >> 16) & 0xFF);
			const int g = static_cast<int>((rgb >> 8) & 0xFF);
			const int b = static_cast<int>(rgb & 0xFF);
			return r | (g << 8) | (b << 16);
		};
		QHash<QString, long> map;
		map.insert(QStringLiteral("aliceblue"), toRef(0xF0F8FF));
		map.insert(QStringLiteral("antiquewhite"), toRef(0xFAEBD7));
		map.insert(QStringLiteral("aqua"), toRef(0x00FFFF));
		map.insert(QStringLiteral("aquamarine"), toRef(0x7FFFD4));
		map.insert(QStringLiteral("azure"), toRef(0xF0FFFF));
		map.insert(QStringLiteral("beige"), toRef(0xF5F5DC));
		map.insert(QStringLiteral("bisque"), toRef(0xFFE4C4));
		map.insert(QStringLiteral("black"), toRef(0x000000));
		map.insert(QStringLiteral("blanchedalmond"), toRef(0xFFEBCD));
		map.insert(QStringLiteral("blue"), toRef(0x0000FF));
		map.insert(QStringLiteral("blueviolet"), toRef(0x8A2BE2));
		map.insert(QStringLiteral("brown"), toRef(0xA52A2A));
		map.insert(QStringLiteral("burlywood"), toRef(0xDEB887));
		map.insert(QStringLiteral("cadetblue"), toRef(0x5F9EA0));
		map.insert(QStringLiteral("chartreuse"), toRef(0x7FFF00));
		map.insert(QStringLiteral("chocolate"), toRef(0xD2691E));
		map.insert(QStringLiteral("coral"), toRef(0xFF7F50));
		map.insert(QStringLiteral("cornflowerblue"), toRef(0x6495ED));
		map.insert(QStringLiteral("cornsilk"), toRef(0xFFF8DC));
		map.insert(QStringLiteral("crimson"), toRef(0xDC143C));
		map.insert(QStringLiteral("cyan"), toRef(0x00FFFF));
		map.insert(QStringLiteral("darkblue"), toRef(0x00008B));
		map.insert(QStringLiteral("darkcyan"), toRef(0x008B8B));
		map.insert(QStringLiteral("darkgoldenrod"), toRef(0xB8860B));
		map.insert(QStringLiteral("darkgray"), toRef(0xA9A9A9));
		map.insert(QStringLiteral("darkgrey"), toRef(0xA9A9A9));
		map.insert(QStringLiteral("darkgreen"), toRef(0x006400));
		map.insert(QStringLiteral("darkkhaki"), toRef(0xBDB76B));
		map.insert(QStringLiteral("darkmagenta"), toRef(0x8B008B));
		map.insert(QStringLiteral("darkolivegreen"), toRef(0x556B2F));
		map.insert(QStringLiteral("darkorange"), toRef(0xFF8C00));
		map.insert(QStringLiteral("darkorchid"), toRef(0x9932CC));
		map.insert(QStringLiteral("darkred"), toRef(0x8B0000));
		map.insert(QStringLiteral("darksalmon"), toRef(0xE9967A));
		map.insert(QStringLiteral("darkseagreen"), toRef(0x8FBC8F));
		map.insert(QStringLiteral("darkslateblue"), toRef(0x483D8B));
		map.insert(QStringLiteral("darkslategray"), toRef(0x2F4F4F));
		map.insert(QStringLiteral("darkslategrey"), toRef(0x2F4F4F));
		map.insert(QStringLiteral("darkturquoise"), toRef(0x00CED1));
		map.insert(QStringLiteral("darkviolet"), toRef(0x9400D3));
		map.insert(QStringLiteral("deeppink"), toRef(0xFF1493));
		map.insert(QStringLiteral("deepskyblue"), toRef(0x00BFFF));
		map.insert(QStringLiteral("dimgray"), toRef(0x696969));
		map.insert(QStringLiteral("dimgrey"), toRef(0x696969));
		map.insert(QStringLiteral("dodgerblue"), toRef(0x1E90FF));
		map.insert(QStringLiteral("firebrick"), toRef(0xB22222));
		map.insert(QStringLiteral("floralwhite"), toRef(0xFFFAF0));
		map.insert(QStringLiteral("forestgreen"), toRef(0x228B22));
		map.insert(QStringLiteral("fuchsia"), toRef(0xFF00FF));
		map.insert(QStringLiteral("gainsboro"), toRef(0xDCDCDC));
		map.insert(QStringLiteral("ghostwhite"), toRef(0xF8F8FF));
		map.insert(QStringLiteral("gold"), toRef(0xFFD700));
		map.insert(QStringLiteral("goldenrod"), toRef(0xDAA520));
		map.insert(QStringLiteral("gray"), toRef(0x808080));
		map.insert(QStringLiteral("grey"), toRef(0x808080));
		map.insert(QStringLiteral("green"), toRef(0x008000));
		map.insert(QStringLiteral("greenyellow"), toRef(0xADFF2F));
		map.insert(QStringLiteral("honeydew"), toRef(0xF0FFF0));
		map.insert(QStringLiteral("hotpink"), toRef(0xFF69B4));
		map.insert(QStringLiteral("indianred"), toRef(0xCD5C5C));
		map.insert(QStringLiteral("indigo"), toRef(0x4B0082));
		map.insert(QStringLiteral("ivory"), toRef(0xFFFFF0));
		map.insert(QStringLiteral("khaki"), toRef(0xF0E68C));
		map.insert(QStringLiteral("lavender"), toRef(0xE6E6FA));
		map.insert(QStringLiteral("lavenderblush"), toRef(0xFFF0F5));
		map.insert(QStringLiteral("lawngreen"), toRef(0x7CFC00));
		map.insert(QStringLiteral("lemonchiffon"), toRef(0xFFFACD));
		map.insert(QStringLiteral("lightblue"), toRef(0xADD8E6));
		map.insert(QStringLiteral("lightcoral"), toRef(0xF08080));
		map.insert(QStringLiteral("lightcyan"), toRef(0xE0FFFF));
		map.insert(QStringLiteral("lightgoldenrodyellow"), toRef(0xFAFAD2));
		map.insert(QStringLiteral("lightgreen"), toRef(0x90EE90));
		map.insert(QStringLiteral("lightgrey"), toRef(0xD3D3D3));
		map.insert(QStringLiteral("lightgray"), toRef(0xD3D3D3));
		map.insert(QStringLiteral("lightpink"), toRef(0xFFB6C1));
		map.insert(QStringLiteral("lightsalmon"), toRef(0xFFA07A));
		map.insert(QStringLiteral("lightseagreen"), toRef(0x20B2AA));
		map.insert(QStringLiteral("lightskyblue"), toRef(0x87CEFA));
		map.insert(QStringLiteral("lightslategray"), toRef(0x778899));
		map.insert(QStringLiteral("lightslategrey"), toRef(0x778899));
		map.insert(QStringLiteral("lightsteelblue"), toRef(0xB0C4DE));
		map.insert(QStringLiteral("lightyellow"), toRef(0xFFFFE0));
		map.insert(QStringLiteral("lime"), toRef(0x00FF00));
		map.insert(QStringLiteral("limegreen"), toRef(0x32CD32));
		map.insert(QStringLiteral("linen"), toRef(0xFAF0E6));
		map.insert(QStringLiteral("magenta"), toRef(0xFF00FF));
		map.insert(QStringLiteral("maroon"), toRef(0x800000));
		map.insert(QStringLiteral("mediumaquamarine"), toRef(0x66CDAA));
		map.insert(QStringLiteral("mediumblue"), toRef(0x0000CD));
		map.insert(QStringLiteral("mediumorchid"), toRef(0xBA55D3));
		map.insert(QStringLiteral("mediumpurple"), toRef(0x9370DB));
		map.insert(QStringLiteral("mediumseagreen"), toRef(0x3CB371));
		map.insert(QStringLiteral("mediumslateblue"), toRef(0x7B68EE));
		map.insert(QStringLiteral("mediumspringgreen"), toRef(0x00FA9A));
		map.insert(QStringLiteral("mediumturquoise"), toRef(0x48D1CC));
		map.insert(QStringLiteral("mediumvioletred"), toRef(0xC71585));
		map.insert(QStringLiteral("midnightblue"), toRef(0x191970));
		map.insert(QStringLiteral("mintcream"), toRef(0xF5FFFA));
		map.insert(QStringLiteral("mistyrose"), toRef(0xFFE4E1));
		map.insert(QStringLiteral("moccasin"), toRef(0xFFE4B5));
		map.insert(QStringLiteral("navajowhite"), toRef(0xFFDEAD));
		map.insert(QStringLiteral("navy"), toRef(0x000080));
		map.insert(QStringLiteral("oldlace"), toRef(0xFDF5E6));
		map.insert(QStringLiteral("olive"), toRef(0x808000));
		map.insert(QStringLiteral("olivedrab"), toRef(0x6B8E23));
		map.insert(QStringLiteral("orange"), toRef(0xFFA500));
		map.insert(QStringLiteral("orangered"), toRef(0xFF4500));
		map.insert(QStringLiteral("orchid"), toRef(0xDA70D6));
		map.insert(QStringLiteral("palegoldenrod"), toRef(0xEEE8AA));
		map.insert(QStringLiteral("palegreen"), toRef(0x98FB98));
		map.insert(QStringLiteral("paleturquoise"), toRef(0xAFEEEE));
		map.insert(QStringLiteral("palevioletred"), toRef(0xDB7093));
		map.insert(QStringLiteral("papayawhip"), toRef(0xFFEFD5));
		map.insert(QStringLiteral("peachpuff"), toRef(0xFFDAB9));
		map.insert(QStringLiteral("peru"), toRef(0xCD853F));
		map.insert(QStringLiteral("pink"), toRef(0xFFC0CB));
		map.insert(QStringLiteral("plum"), toRef(0xDDA0DD));
		map.insert(QStringLiteral("powderblue"), toRef(0xB0E0E6));
		map.insert(QStringLiteral("purple"), toRef(0x800080));
		map.insert(QStringLiteral("rebeccapurple"), toRef(0x663399));
		map.insert(QStringLiteral("red"), toRef(0xFF0000));
		map.insert(QStringLiteral("rosybrown"), toRef(0xBC8F8F));
		map.insert(QStringLiteral("royalblue"), toRef(0x4169E1));
		map.insert(QStringLiteral("saddlebrown"), toRef(0x8B4513));
		map.insert(QStringLiteral("salmon"), toRef(0xFA8072));
		map.insert(QStringLiteral("sandybrown"), toRef(0xF4A460));
		map.insert(QStringLiteral("seagreen"), toRef(0x2E8B57));
		map.insert(QStringLiteral("seashell"), toRef(0xFFF5EE));
		map.insert(QStringLiteral("sienna"), toRef(0xA0522D));
		map.insert(QStringLiteral("silver"), toRef(0xC0C0C0));
		map.insert(QStringLiteral("skyblue"), toRef(0x87CEEB));
		map.insert(QStringLiteral("slateblue"), toRef(0x6A5ACD));
		map.insert(QStringLiteral("slategray"), toRef(0x708090));
		map.insert(QStringLiteral("slategrey"), toRef(0x708090));
		map.insert(QStringLiteral("snow"), toRef(0xFFFAFA));
		map.insert(QStringLiteral("springgreen"), toRef(0x00FF7F));
		map.insert(QStringLiteral("steelblue"), toRef(0x4682B4));
		map.insert(QStringLiteral("tan"), toRef(0xD2B48C));
		map.insert(QStringLiteral("teal"), toRef(0x008080));
		map.insert(QStringLiteral("thistle"), toRef(0xD8BFD8));
		map.insert(QStringLiteral("tomato"), toRef(0xFF6347));
		map.insert(QStringLiteral("turquoise"), toRef(0x40E0D0));
		map.insert(QStringLiteral("violet"), toRef(0xEE82EE));
		map.insert(QStringLiteral("wheat"), toRef(0xF5DEB3));
		map.insert(QStringLiteral("white"), toRef(0xFFFFFF));
		map.insert(QStringLiteral("whitesmoke"), toRef(0xF5F5F5));
		map.insert(QStringLiteral("yellow"), toRef(0xFFFF00));
		map.insert(QStringLiteral("yellowgreen"), toRef(0x9ACD32));
		return map;
	}();

	const auto it = kMxpColors.constFind(lowered);
	if (it != kMxpColors.constEnd())
		return it.value();
	return -1;
}

static int colourSeqFromAttributes(const QMap<QString, QString> &attributes)
{
	bool ok  = false;
	int  seq = attributes.value(QStringLiteral("seq")).toInt(&ok);
	if (ok)
		return seq;

	seq = attributes.value(QStringLiteral("seq_index")).toInt(&ok);
	if (ok)
		return seq + 1;

	return -1;
}

int WorldRuntime::setCustomColourText(int index, const QColor &color)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eBadParameter,
		                          [this, index, color] { return setCustomColourText(index, color); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::setCustomColourText");
	if (index < 1 || index > MAX_CUSTOM || !color.isValid())
		return eBadParameter;

	const auto    group    = QStringLiteral("custom/custom");
	const QString seq      = QString::number(index);
	const QString seqIndex = QString::number(index - 1);
	const QString value    = QString::number(colorToLong(color));

	for (auto &colour : m_colours)
	{
		const QString groupKey = colour.group.trimmed().toLower();
		if (groupKey != QStringLiteral("custom/custom") && groupKey != QStringLiteral("custom"))
			continue;
		const int itemSeq = colourSeqFromAttributes(colour.attributes);
		if (itemSeq != index)
			continue;
		colour.attributes.insert(QStringLiteral("seq"), seq);
		colour.attributes.insert(QStringLiteral("seq_index"), seqIndex);
		colour.attributes.insert(QStringLiteral("text"), value);
		m_worldFileModified = true;
		invalidateResolvedOutputColourCache();
		if (m_view)
		{
			m_view->applyRuntimeSettings();
			m_view->update();
		}
		return eOK;
	}

	Colour entry;
	entry.group = group;
	entry.attributes.insert(QStringLiteral("seq"), seq);
	entry.attributes.insert(QStringLiteral("seq_index"), seqIndex);
	entry.attributes.insert(QStringLiteral("text"), value);
	m_colours.push_back(entry);
	m_colourCount       = safeQSizeToInt(m_colours.size());
	m_worldFileModified = true;
	invalidateResolvedOutputColourCache();
	if (m_view)
	{
		m_view->applyRuntimeSettings();
		m_view->update();
	}
	return eOK;
}

int WorldRuntime::setCustomColourBackground(int index, const QColor &color)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eBadParameter,
		                          [this, index, color] { return setCustomColourBackground(index, color); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::setCustomColourBackground");
	if (index < 1 || index > MAX_CUSTOM || !color.isValid())
		return eBadParameter;

	const auto    group    = QStringLiteral("custom/custom");
	const QString seq      = QString::number(index);
	const QString seqIndex = QString::number(index - 1);
	const QString value    = QString::number(colorToLong(color));

	for (auto &colour : m_colours)
	{
		const QString groupKey = colour.group.trimmed().toLower();
		if (groupKey != QStringLiteral("custom/custom") && groupKey != QStringLiteral("custom"))
			continue;
		const int itemSeq = colourSeqFromAttributes(colour.attributes);
		if (itemSeq != index)
			continue;
		colour.attributes.insert(QStringLiteral("seq"), seq);
		colour.attributes.insert(QStringLiteral("seq_index"), seqIndex);
		colour.attributes.insert(QStringLiteral("back"), value);
		m_worldFileModified = true;
		invalidateResolvedOutputColourCache();
		if (m_view)
		{
			m_view->applyRuntimeSettings();
			m_view->update();
		}
		return eOK;
	}

	Colour entry;
	entry.group = group;
	entry.attributes.insert(QStringLiteral("seq"), seq);
	entry.attributes.insert(QStringLiteral("seq_index"), seqIndex);
	entry.attributes.insert(QStringLiteral("back"), value);
	m_colours.push_back(entry);
	m_colourCount       = safeQSizeToInt(m_colours.size());
	m_worldFileModified = true;
	invalidateResolvedOutputColourCache();
	if (m_view)
	{
		m_view->applyRuntimeSettings();
		m_view->update();
	}
	return eOK;
}

int WorldRuntime::setCustomColourName(int index, const QString &name)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eOptionOutOfRange,
		                          [this, index, name] { return setCustomColourName(index, name); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::setCustomColourName");
	if (index < 1 || index > MAX_CUSTOM)
		return eOptionOutOfRange;
	const QString trimmed = name.trimmed();
	if (trimmed.isEmpty())
		return eNoNameSpecified;
	if (trimmed.size() > 30)
		return eInvalidObjectLabel;

	const auto    group    = QStringLiteral("custom/custom");
	const QString seq      = QString::number(index);
	const QString seqIndex = QString::number(index - 1);

	for (auto &colour : m_colours)
	{
		const QString groupKey = colour.group.trimmed().toLower();
		if (groupKey != QStringLiteral("custom/custom") && groupKey != QStringLiteral("custom"))
			continue;
		const int itemSeq = colourSeqFromAttributes(colour.attributes);
		if (itemSeq != index)
			continue;
		colour.attributes.insert(QStringLiteral("seq"), seq);
		colour.attributes.insert(QStringLiteral("seq_index"), seqIndex);
		colour.attributes.insert(QStringLiteral("name"), trimmed);
		m_worldFileModified = true;
		return eOK;
	}

	Colour entry;
	entry.group = group;
	entry.attributes.insert(QStringLiteral("seq"), seq);
	entry.attributes.insert(QStringLiteral("seq_index"), seqIndex);
	entry.attributes.insert(QStringLiteral("name"), trimmed);
	m_colours.push_back(entry);
	m_colourCount       = safeQSizeToInt(m_colours.size());
	m_worldFileModified = true;
	return eOK;
}

QString WorldRuntime::customColourName(int index) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QString(),
		                          [this, index] { return customColourName(index); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::customColourName");
	if (index < 1 || index > MAX_CUSTOM)
		return {};

	for (const auto &colour : m_colours)
	{
		const QString groupKey = colour.group.trimmed().toLower();
		if (groupKey != QStringLiteral("custom/custom") && groupKey != QStringLiteral("custom"))
			continue;
		if (colourSeqFromAttributes(colour.attributes) != index)
			continue;
		const QString name = colour.attributes.value(QStringLiteral("name")).trimmed();
		if (!name.isEmpty())
			return name;
		break;
	}
	return QStringLiteral("Custom%1").arg(index);
}

long WorldRuntime::customColourText(int index) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), 0L,
		                          [this, index] { return customColourText(index); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::customColourText");
	if (index < 1 || index > MAX_CUSTOM)
		return 0;
	QVector<QColor> normalAnsi;
	QVector<QColor> customText;
	QVector<QColor> customBack;
	buildCustomColours(m_colours, normalAnsi, customText, customBack);
	return colorToLong(customText.value(index - 1));
}

long WorldRuntime::customColourBackground(int index) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), 0L,
		                          [this, index] { return customColourBackground(index); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::customColourBackground");
	if (index < 1 || index > MAX_CUSTOM)
		return 0;
	QVector<QColor> normalAnsi;
	QVector<QColor> customText;
	QVector<QColor> customBack;
	buildCustomColours(m_colours, normalAnsi, customText, customBack);
	return colorToLong(customBack.value(index - 1));
}

namespace
{
	bool isAsciiLetter(const ushort code)
	{
		return (code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z');
	}

	bool isAsciiDigit(const ushort code)
	{
		return code >= '0' && code <= '9';
	}
} // namespace

int WorldRuntime::loadWorldImageFile(const QString &rawName, QImage *target, QString *storedName)
{
	const QString fileName = rawName.trimmed();
	if (fileName.isEmpty())
	{
		if (target)
			*target = QImage();
		if (storedName)
			storedName->clear();
		return eOK;
	}

	if (fileName.size() < 5)
		return eBadParameter;

	const QString lower = fileName.toLower();
	if (!lower.endsWith(QStringLiteral(".png")) && !lower.endsWith(QStringLiteral(".bmp")))
		return eBadParameter;

	if (!QFileInfo::exists(fileName))
		return eFileNotFound;

	QImageReader reader(fileName);
	reader.setAutoTransform(true);
	const QImage image = reader.read();
	if (image.isNull())
		return eCouldNotOpenFile;

	if (target)
		*target = image;
	if (storedName)
		*storedName = fileName;
	return eOK;
}

int WorldRuntime::setBackgroundImage(const QString &fileName, int mode)
{
	if (mode < 0 || mode > 13)
		return eBadParameter;
	QString resolvedFileName;
	QString error;
	if (!fileName.trimmed().isEmpty() &&
	    !QMudPluginPathUtils::resolveInsideQmudHome(m_startupDirectory, fileName, &resolvedFileName, &error))
		return eFileNotFound;
	const QString imageFileName = fileName.trimmed().isEmpty() ? QString() : resolvedFileName;
	const int     result        = loadWorldImageFile(imageFileName, &m_backgroundImage, nullptr);
	if (result == eOK)
	{
		m_backgroundImageMode = mode;
		m_backgroundImageName =
		    imageFileName.isEmpty()
		        ? QString()
		        : QMudPluginPathUtils::qmudHomeRelativePath(m_startupDirectory, imageFileName, false);
		m_worldFileModified = true;
		if (m_view)
		{
			m_view->applyRuntimeSettings();
			m_view->update();
		}
	}
	return result;
}

int WorldRuntime::setForegroundImage(const QString &fileName, int mode)
{
	if (mode < 0 || mode > 13)
		return eBadParameter;
	QString resolvedFileName;
	QString error;
	if (!fileName.trimmed().isEmpty() &&
	    !QMudPluginPathUtils::resolveInsideQmudHome(m_startupDirectory, fileName, &resolvedFileName, &error))
		return eFileNotFound;
	const QString imageFileName = fileName.trimmed().isEmpty() ? QString() : resolvedFileName;
	const int     result        = loadWorldImageFile(imageFileName, &m_foregroundImage, nullptr);
	if (result == eOK)
	{
		m_foregroundImageMode = mode;
		m_foregroundImageName =
		    imageFileName.isEmpty()
		        ? QString()
		        : QMudPluginPathUtils::qmudHomeRelativePath(m_startupDirectory, imageFileName, false);
		m_worldFileModified = true;
		if (m_view)
		{
			m_view->applyRuntimeSettings();
			m_view->update();
		}
	}
	return result;
}

QImage WorldRuntime::backgroundImage() const
{
	return m_backgroundImage;
}

QImage WorldRuntime::foregroundImage() const
{
	return m_foregroundImage;
}

int WorldRuntime::backgroundImageMode() const
{
	return m_backgroundImageMode;
}

int WorldRuntime::foregroundImageMode() const
{
	return m_foregroundImageMode;
}

QString WorldRuntime::backgroundImageName() const
{
	return m_backgroundImageName;
}

QString WorldRuntime::foregroundImageName() const
{
	return m_foregroundImageName;
}

bool WorldRuntime::closeNotepad(const QString &title, bool querySave)
{
	const QString trimmedTitle = title.trimmed();
	if (trimmedTitle.isEmpty())
		return false;
	const auto ownerToken = reinterpret_cast<qulonglong>(this);
	QString    worldId;
	if (QThread::currentThread() == thread())
	{
		qmudAssertObjectThreadAffinity(this, "WorldRuntime::closeNotepad");
		worldId = m_worldAttributes.value(QStringLiteral("id")).trimmed();
	}
	else
	{
		worldId = qmudInvokeMethodOr(this, QString(), [this]
		                             { return m_worldAttributes.value(QStringLiteral("id")).trimmed(); });
	}
	return closeNotepadWindowOnMainThread(trimmedTitle, querySave, ownerToken, worldId);
}

QVariant WorldRuntime::debugCommand(const QString &command)
{
	auto          note = [this](const QString &line) { outputText(line, true, true); };

	const QString cmd = command.trimmed().toLower();
	if (cmd == QStringLiteral("summary"))
	{
		note(QStringLiteral("World: %1")
		         .arg(worldName().isEmpty() ? QStringLiteral("(unnamed)") : worldName()));
		note(QStringLiteral("Connected: %1")
		         .arg(isConnected() ? QStringLiteral("yes") : QStringLiteral("no")));
		note(QStringLiteral("Bytes in/out: %1 / %2").arg(m_bytesIn).arg(m_bytesOut));
		note(QStringLiteral("Packets in/out: %1 / %2").arg(m_inputPacketCount).arg(m_outputPacketCount));
		note(QStringLiteral("Lines in/out: %1 / %2").arg(m_linesReceived).arg(m_totalLinesSent));
		note(QStringLiteral("New lines: %1").arg(m_newLines));
		note(QStringLiteral("Triggers/Aliases/Timers fired: %1 / %2 / %3")
		         .arg(m_triggersMatchedThisSession)
		         .arg(m_aliasesMatchedThisSession)
		         .arg(m_timersFiredThisSession));
		note(QStringLiteral("Counts - triggers:%1 aliases:%2 timers:%3 variables:%4 arrays:%5 plugins:%6")
		         .arg(m_triggers.size())
		         .arg(m_aliases.size())
		         .arg(m_timers.size())
		         .arg(m_variables.size())
		         .arg(m_arrays.size())
		         .arg(m_plugins.size()));
		return {};
	}

	if (cmd == QStringLiteral("variables"))
	{
		int count = 0;
		for (const Variable &var : m_variables)
		{
			const QString name = var.attributes.value(QStringLiteral("name"));
			note(QStringLiteral("%1 = %2").arg(name, var.content));
			++count;
		}
		note(QStringLiteral("%1 variable%2.").arg(count).arg(count == 1 ? QString() : QStringLiteral("s")));
		return {};
	}

	if (cmd == QStringLiteral("arrays"))
	{
		int count = 0;
		for (auto it = m_arrays.cbegin(); it != m_arrays.cend(); ++it)
		{
			note(QStringLiteral("[%1] (%2 items)").arg(it.key()).arg(it->values.size()));
			for (auto kv = it->values.cbegin(); kv != it->values.cend(); ++kv)
				note(QStringLiteral("  %1 = %2").arg(kv.key(), kv.value()));
			++count;
		}
		note(QStringLiteral("%1 array%2.").arg(count).arg(count == 1 ? QString() : QStringLiteral("s")));
		return {};
	}

	if (cmd == QStringLiteral("plugins"))
	{
		int count = 0;
		for (const Plugin &plugin : m_plugins)
		{
			const QString id      = plugin.attributes.value(QStringLiteral("id")).isEmpty()
			                            ? QStringLiteral("(no id)")
			                            : plugin.attributes.value(QStringLiteral("id"));
			const QString name    = plugin.attributes.value(QStringLiteral("name")).isEmpty()
			                            ? QStringLiteral("(unnamed)")
			                            : plugin.attributes.value(QStringLiteral("name"));
			const QString enabled = plugin.enabled ? QStringLiteral("enabled") : QStringLiteral("disabled");
			note(QStringLiteral("%1 | %2 | %3").arg(id, name, enabled));
			++count;
		}
		note(QStringLiteral("%1 plugin%2.").arg(count).arg(count == 1 ? QString() : QStringLiteral("s")));
		return {};
	}

	if (cmd == QStringLiteral("triggers"))
	{
		int count = 0;
		for (const Trigger &trigger : m_triggers)
		{
			note(QStringLiteral("[%1] %2").arg(trigger.attributes.value(QStringLiteral("name")),
			                                   trigger.attributes.value(QStringLiteral("match"))));
			++count;
		}
		note(QStringLiteral("%1 trigger%2.").arg(count).arg(count == 1 ? QString() : QStringLiteral("s")));
		return {};
	}

	if (cmd == QStringLiteral("aliases"))
	{
		int count = 0;
		for (const Alias &alias : m_aliases)
		{
			note(QStringLiteral("[%1] %2").arg(alias.attributes.value(QStringLiteral("name")),
			                                   alias.attributes.value(QStringLiteral("match"))));
			++count;
		}
		note(QStringLiteral("%1 alias%2.").arg(count).arg(count == 1 ? QString() : QStringLiteral("es")));
		return {};
	}

	if (cmd == QStringLiteral("internal_commands"))
	{
		const QStringList commands = internalCommandsList();
		for (const QString &item : commands)
			note(item);
		note(QStringLiteral("%1 internal command%2.")
		         .arg(commands.size())
		         .arg(commands.size() == 1 ? QString() : QStringLiteral("s")));
		return {};
	}

	if (cmd == QStringLiteral("commands"))
	{
		const QString last = m_lastCommandSent.trimmed();
		if (!last.isEmpty())
			note(last);
		const int count = last.isEmpty() ? 0 : 1;
		note(QStringLiteral("%1 command%2.").arg(count).arg(count == 1 ? QString() : QStringLiteral("s")));
		return {};
	}

	if (cmd == QStringLiteral("options") || cmd == QStringLiteral("alpha_options"))
	{
		QStringList keys = m_worldAttributes.keys();
		std::ranges::sort(keys, [](const QString &a, const QString &b)
		                  { return a.compare(b, Qt::CaseInsensitive) < 0; });
		for (const QString &key : keys)
			note(QStringLiteral("%1 = %2").arg(key, m_worldAttributes.value(key)));
		note(QStringLiteral("%1 option%2.")
		         .arg(keys.size())
		         .arg(keys.size() == 1 ? QString() : QStringLiteral("s")));
		return {};
	}

	note(QStringLiteral("----- Debug commands available -----"));
	note(QStringLiteral("aliases"));
	note(QStringLiteral("alpha_options"));
	note(QStringLiteral("arrays"));
	note(QStringLiteral("commands"));
	note(QStringLiteral("internal_commands"));
	note(QStringLiteral("options"));
	note(QStringLiteral("plugins"));
	note(QStringLiteral("summary"));
	note(QStringLiteral("triggers"));
	note(QStringLiteral("variables"));
	return {};
}

void WorldRuntime::setQueuedCommandCount(int count)
{
	m_queuedCommandCount = count;
}

int WorldRuntime::queuedCommandCount() const
{
	return m_queuedCommandCount;
}

qint64 WorldRuntime::bytesIn() const
{
	if (QThread::currentThread() != thread())
		return runtimeCountersSnapshot(false).bytesIn;

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::bytesIn");
	return m_bytesIn;
}

qint64 WorldRuntime::bytesOut() const
{
	if (QThread::currentThread() != thread())
		return runtimeCountersSnapshot(false).bytesOut;

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::bytesOut");
	return m_bytesOut;
}

int WorldRuntime::inputPacketCount() const
{
	return m_inputPacketCount;
}

int WorldRuntime::outputPacketCount() const
{
	return m_outputPacketCount;
}

qint64 WorldRuntime::totalCompressedBytes() const
{
	return m_telnet.totalCompressedBytes();
}

qint64 WorldRuntime::totalUncompressedBytes() const
{
	return m_telnet.totalUncompressedBytes();
}

int WorldRuntime::mccpType() const
{
	return m_telnet.mccpType();
}

qint64 WorldRuntime::mxpTagCount() const
{
	return m_telnet.mxpTagCount();
}

qint64 WorldRuntime::mxpEntityCount() const
{
	return m_telnet.mxpEntityCount();
}

int WorldRuntime::customElementCount() const
{
	return m_telnet.customElementCount();
}

int WorldRuntime::customEntityCount() const
{
	return m_telnet.customEntityCount();
}

QList<TelnetProcessor::CustomElementInfo> WorldRuntime::customMxpElements() const
{
	return m_telnet.customElementInfos();
}

void WorldRuntime::setCustomMxpElements(const QList<TelnetProcessor::CustomElementInfo> &elements)
{
	m_telnet.setCustomElementInfos(elements);
}

TelnetProcessor::MxpSessionState WorldRuntime::mxpSessionState() const
{
	return m_telnet.mxpSessionState();
}

void WorldRuntime::setMxpSessionState(const TelnetProcessor::MxpSessionState &state)
{
	m_telnet.setMxpSessionState(state);
	m_mxpActive = state.enabled;
	if (!state.enabled)
	{
		m_mxpTagStack.clear();
		m_mxpOpenTags.clear();
		m_mxpTextBuffer.clear();
		resetMxpRenderState();
		clearAnsiActionContext();
	}
}

QString WorldRuntime::peerAddressString() const
{
	if (!m_socket)
		return {};
	return m_socket->peerAddressString();
}

quint32 WorldRuntime::peerAddressV4() const
{
	if (!m_socket)
		return 0;
	return m_socket->peerAddressV4();
}

QString WorldRuntime::proxyAddressString() const
{
	return m_proxyAddressString;
}

quint32 WorldRuntime::proxyAddressV4() const
{
	return m_proxyAddressV4;
}

bool WorldRuntime::isCompressing() const
{
	return m_telnet.isCompressing();
}

bool WorldRuntime::isPuebloActive() const
{
	return m_telnet.isPuebloActive();
}

bool WorldRuntime::isConnecting() const
{
	switch (m_connectPhase)
	{
	case eConnectNotConnected:
	case eConnectConnectedToMud:
	case eConnectDisconnecting:
		return false;
	default:
		return true;
	}
}

bool WorldRuntime::isMapping() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodTrueOnSuccess(const_cast<WorldRuntime *>(this),
		                                     [this] { return isMapping(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::isMapping");
	return m_isMapping;
}

int WorldRuntime::mappingCount() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), 0, [this] { return mappingCount(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::mappingCount");
	return safeQSizeToInt(m_mappingList.size());
}

QString WorldRuntime::mappingItem(int index) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QString(),
		                          [this, index] { return mappingItem(index); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::mappingItem");
	if (index < 0 || index >= m_mappingList.size())
		return {};
	return m_mappingList.at(index);
}

QString WorldRuntime::mappingString(bool omitComments) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QString(),
		                          [this, omitComments] { return mappingString(omitComments); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::mappingString");
	QString lastDir;
	QString output;
	int     count = 0;

	auto    outputPrevious = [&](const QString &current)
	{
		if (!lastDir.isEmpty())
		{
			QString formatted = lastDir;
			if (formatted.size() > 1)
				formatted = QStringLiteral("(%1)").arg(formatted);
			if (count == 1)
				output += formatted + QLatin1Char(' ');
			else if (count > 1)
				output += QString::number(count) + formatted + QLatin1Char(' ');
		}
		lastDir = current;
		count   = 1;
	};

	for (const QString &entry : m_mappingList)
	{
		if (entry.size() > 2 && entry.startsWith(QLatin1Char('{')) && entry.endsWith(QLatin1Char('}')))
		{
			outputPrevious(QString());
			if (!omitComments)
			{
				if (!output.isEmpty())
					output += QLatin1Char('\n');
				output += entry;
				output += QLatin1Char('\n');
			}
			lastDir.clear();
			count = 0;
			continue;
		}

		if (entry == lastDir && count <= 98)
		{
			count++;
			continue;
		}
		outputPrevious(entry);
	}

	outputPrevious(QString());
	return output;
}

unsigned short WorldRuntime::noteStyle() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), static_cast<unsigned short>(0),
		                          [this] { return noteStyle(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::noteStyle");
	return m_noteStyle;
}

void WorldRuntime::setNoteStyle(unsigned short style)
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this, style] { setNoteStyle(style); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::setNoteStyle");
	m_noteStyle = style;
}

static QColor parseColourValue(const QString &value)
{
	if (value.isEmpty())
		return {};
	QColor color(value);
	if (color.isValid())
		return color;
	bool      ok      = false;
	const int numeric = value.toInt(&ok);
	if (!ok)
		return {};
	const int r = numeric & 0xFF;
	const int g = (numeric >> 8) & 0xFF;
	const int b = (numeric >> 16) & 0xFF;
	return {r, g, b};
}

static void buildCustomColours(const QList<WorldRuntime::Colour> &colours, QVector<QColor> &normalAnsi,
                               QVector<QColor> &customText, QVector<QColor> &customBack)
{
	ResolvedWorldColourTables tables;
	buildResolvedColourTables(colours, tables);
	normalAnsi = tables.normalAnsi;
	customText = tables.customText;
	customBack = tables.customBack;
}

static void buildResolvedColourTables(const QList<WorldRuntime::Colour> &colours,
                                      ResolvedWorldColourTables         &tables)
{
	tables.normalAnsi = QVector<QColor>(8);
	tables.boldAnsi   = QVector<QColor>(8);
	tables.customText = QVector<QColor>(16);
	tables.customBack = QVector<QColor>(16);

	tables.normalAnsi[0] = QColor(0, 0, 0);
	tables.normalAnsi[1] = QColor(128, 0, 0);
	tables.normalAnsi[2] = QColor(0, 128, 0);
	tables.normalAnsi[3] = QColor(128, 128, 0);
	tables.normalAnsi[4] = QColor(0, 0, 128);
	tables.normalAnsi[5] = QColor(128, 0, 128);
	tables.normalAnsi[6] = QColor(0, 128, 128);
	tables.normalAnsi[7] = QColor(192, 192, 192);
	tables.boldAnsi[0]   = QColor(128, 128, 128);
	tables.boldAnsi[1]   = QColor(255, 0, 0);
	tables.boldAnsi[2]   = QColor(0, 255, 0);
	tables.boldAnsi[3]   = QColor(255, 255, 0);
	tables.boldAnsi[4]   = QColor(0, 0, 255);
	tables.boldAnsi[5]   = QColor(255, 0, 255);
	tables.boldAnsi[6]   = QColor(0, 255, 255);
	tables.boldAnsi[7]   = QColor(255, 255, 255);

	for (int i = 0; i < tables.customText.size(); ++i)
	{
		tables.customText[i] = QColor(255, 255, 255);
		tables.customBack[i] = QColor(0, 0, 0);
	}

	tables.customText[0]  = QColor(255, 128, 128);
	tables.customText[1]  = QColor(255, 255, 128);
	tables.customText[2]  = QColor(128, 255, 128);
	tables.customText[3]  = QColor(128, 255, 255);
	tables.customText[4]  = QColor(0, 128, 255);
	tables.customText[5]  = QColor(255, 128, 192);
	tables.customText[6]  = QColor(255, 0, 0);
	tables.customText[7]  = QColor(0, 128, 192);
	tables.customText[8]  = QColor(255, 0, 255);
	tables.customText[9]  = QColor(128, 64, 64);
	tables.customText[10] = QColor(255, 128, 64);
	tables.customText[11] = QColor(0, 128, 128);
	tables.customText[12] = QColor(0, 64, 128);
	tables.customText[13] = QColor(255, 0, 128);
	tables.customText[14] = QColor(0, 128, 0);
	tables.customText[15] = QColor(0, 0, 255);

	for (const auto &colour : colours)
	{
		const QString group = colour.group.trimmed().toLower();
		bool          ok    = false;
		const int     seq   = colour.attributes.value(QStringLiteral("seq")).toInt(&ok);
		const int     index = ok ? seq - 1 : -1;
		if (index < 0)
			continue;
		if (group == QStringLiteral("ansi/normal") && index < tables.normalAnsi.size())
		{
			const QColor rgb = parseColourValue(colour.attributes.value(QStringLiteral("rgb")));
			if (rgb.isValid())
				tables.normalAnsi[index] = rgb;
		}
		else if (group == QStringLiteral("ansi/bold") && index < tables.boldAnsi.size())
		{
			const QColor rgb = parseColourValue(colour.attributes.value(QStringLiteral("rgb")));
			if (rgb.isValid())
				tables.boldAnsi[index] = rgb;
		}
		else if ((group == QStringLiteral("custom/custom") || group == QStringLiteral("custom")) &&
		         index < tables.customText.size())
		{
			const QColor text = parseColourValue(colour.attributes.value(QStringLiteral("text")));
			const QColor back = parseColourValue(colour.attributes.value(QStringLiteral("back")));
			if (text.isValid())
				tables.customText[index] = text;
			if (back.isValid())
				tables.customBack[index] = back;
		}
	}
}

static ResolvedWorldColourTables buildResolvedWorldColourTables(const QList<WorldRuntime::Colour> &colours)
{
	ResolvedWorldColourTables tables;
	buildResolvedColourTables(colours, tables);
	return tables;
}

static bool worldAttributeAffectsResolvedOutputColours(const QString &key)
{
	return key == QStringLiteral("custom_16_is_default_colour") ||
	       key == QStringLiteral("output_text_colour") || key == QStringLiteral("output_background_colour");
}

void WorldRuntime::invalidateResolvedOutputColourCache()
{
	m_resolvedOutputColourCache.valid = false;
}

const WorldRuntime::ResolvedOutputColourCache &WorldRuntime::resolvedOutputColourCache()
{
	if (m_resolvedOutputColourCache.valid)
		return m_resolvedOutputColourCache;

	const ResolvedWorldColourTables tables = buildResolvedWorldColourTables(m_colours);
	m_resolvedOutputColourCache.normalAnsi = tables.normalAnsi;
	m_resolvedOutputColourCache.boldAnsi   = tables.boldAnsi;
	m_resolvedOutputColourCache.customText = tables.customText;
	m_resolvedOutputColourCache.customBack = tables.customBack;

	const bool custom16Default =
	    isEnabledFlag(m_worldAttributes.value(QStringLiteral("custom_16_is_default_colour")));
	QColor defaultForeColor = parseColourValue(m_worldAttributes.value(QStringLiteral("output_text_colour")));
	QColor defaultBackColor =
	    parseColourValue(m_worldAttributes.value(QStringLiteral("output_background_colour")));
	if (!defaultForeColor.isValid())
		defaultForeColor = custom16Default ? m_resolvedOutputColourCache.customText.value(15)
		                                   : m_resolvedOutputColourCache.normalAnsi.value(kAnsiWhite);
	if (!defaultBackColor.isValid())
		defaultBackColor = custom16Default ? m_resolvedOutputColourCache.customBack.value(15)
		                                   : m_resolvedOutputColourCache.normalAnsi.value(kAnsiBlack);

	m_resolvedOutputColourCache.defaultFore =
	    defaultForeColor.isValid() ? defaultForeColor.name() : QString();
	m_resolvedOutputColourCache.defaultBack =
	    defaultBackColor.isValid() ? defaultBackColor.name() : QString();
	m_resolvedOutputColourCache.valid = true;
	return m_resolvedOutputColourCache;
}

static int publicNoteColourIndexFromWorldAttribute(const QString                     &value,
                                                   const QList<WorldRuntime::Colour> &colours,
                                                   const int                          fallbackPublicIndex)
{
	const ResolvedWorldColourTables tables = buildResolvedWorldColourTables(colours);
	return QMudNoteColour::publicIndexFromWorldAttribute(value, tables.customText, fallbackPublicIndex);
}

static long colorToLong(const QColor &color)
{
	if (!color.isValid())
		return 0;
	return (color.red() | (color.green() << 8) | (color.blue() << 16));
}

static QString convertToRegularExpression(const QString &text)
{
	const QByteArray input = text.toLatin1();
	QByteArray       out;
	out.append('^');

	for (unsigned char const c : input)
	{
		if (c == '\n')
		{
			out.append("\\n");
		}
		else if (c < ' ')
		{
			out.append("\\x");
			appendHexByte(out, c);
		}
		else if (isAsciiAlnumByte(c) || c == ' ' || c >= 0x80)
		{
			out.append(static_cast<char>(c));
		}
		else if (c == '*')
		{
			out.append("(.*?)");
		}
		else
		{
			out.append('\\');
			out.append(static_cast<char>(c));
		}
	}

	out.append('$');

	return QString::fromLatin1(out);
}

static ResolvedNoteColours resolveNoteColours(const bool notesInRgb, const int noteTextColour,
                                              const long noteColourFore, const long noteColourBack,
                                              const ResolvedWorldColourTables &colourTables,
                                              const QMap<QString, QString>    &worldAttributes)
{
	if (notesInRgb)
		return {noteColourFore, noteColourBack};

	const bool custom16Default =
	    isEnabledFlag(worldAttributes.value(QStringLiteral("custom_16_is_default_colour")));
	if (const bool sameColour = (noteTextColour == WorldRuntime::kSameColour || noteTextColour < 0);
	    sameColour)
	{
		return {custom16Default ? colorToLong(colourTables.customText.value(15))
		                        : colorToLong(colourTables.normalAnsi.value(kAnsiWhite)),
		        custom16Default ? colorToLong(colourTables.customBack.value(15))
		                        : colorToLong(colourTables.normalAnsi.value(kAnsiBlack))};
	}
	if (noteTextColour >= 0 && noteTextColour < colourTables.customText.size() &&
	    noteTextColour < colourTables.customBack.size())
		return {colorToLong(colourTables.customText.value(noteTextColour)),
		        colorToLong(colourTables.customBack.value(noteTextColour))};
	return {};
}

static ResolvedNoteColours resolveNoteColours(const bool notesInRgb, const int noteTextColour,
                                              const long noteColourFore, const long noteColourBack,
                                              const QList<WorldRuntime::Colour> &colours,
                                              const QMap<QString, QString>      &worldAttributes)
{
	const ResolvedWorldColourTables colourTables = buildResolvedWorldColourTables(colours);
	return resolveNoteColours(notesInRgb, noteTextColour, noteColourFore, noteColourBack, colourTables,
	                          worldAttributes);
}

bool WorldRuntime::notesInRgb() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodTrueOnSuccess(const_cast<WorldRuntime *>(this),
		                                     [this] { return notesInRgb(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::notesInRgb");
	return m_notesInRgb;
}

int WorldRuntime::noteTextColour() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), -1, [this] { return noteTextColour(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::noteTextColour");
	return m_noteTextColour;
}

void WorldRuntime::setNoteTextColour(int value)
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this, value] { setNoteTextColour(value); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::setNoteTextColour");
	if (value < 0 || value > MAX_CUSTOM)
		return;
	if (m_noteTextColour == value - 1 && !m_notesInRgb)
		return;
	m_noteTextColour = value - 1;
	m_notesInRgb     = false;
	invalidateLuaCallbackDispatchSnapshot();
}

long WorldRuntime::noteColourFore() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), 0L, [this] { return noteColourFore(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::noteColourFore");
	if (m_notesInRgb)
		return m_noteColourFore;

	return resolveNoteColours(m_notesInRgb, m_noteTextColour, m_noteColourFore, m_noteColourBack, m_colours,
	                          m_worldAttributes)
	    .fore;
}

long WorldRuntime::noteColourBack() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), 0L, [this] { return noteColourBack(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::noteColourBack");
	if (m_notesInRgb)
		return m_noteColourBack;

	return resolveNoteColours(m_notesInRgb, m_noteTextColour, m_noteColourFore, m_noteColourBack, m_colours,
	                          m_worldAttributes)
	    .back;
}

void WorldRuntime::setNoteColourFore(long value)
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this, value] { setNoteColourFore(value); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::setNoteColourFore");
	if (!m_notesInRgb)
	{
		QVector<QColor> normalAnsi;
		QVector<QColor> customText;
		QVector<QColor> customBack;
		buildCustomColours(m_colours, normalAnsi, customText, customBack);
		const bool custom16Default =
		    isEnabledFlag(m_worldAttributes.value(QStringLiteral("custom_16_is_default_colour")));
		if (const bool sameColour = (m_noteTextColour == kSameColour || m_noteTextColour < 0); sameColour)
			m_noteColourBack = custom16Default ? colorToLong(customBack.value(15))
			                                   : colorToLong(normalAnsi.value(kAnsiBlack));
		else if (m_noteTextColour >= 0 && m_noteTextColour < customBack.size())
			m_noteColourBack = colorToLong(customBack.value(m_noteTextColour));
	}

	m_notesInRgb     = true;
	m_noteColourFore = value & 0x00FFFFFF;
}

void WorldRuntime::setNoteColourBack(long value)
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this, value] { setNoteColourBack(value); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::setNoteColourBack");
	if (!m_notesInRgb)
	{
		QVector<QColor> normalAnsi;
		QVector<QColor> customText;
		QVector<QColor> customBack;
		buildCustomColours(m_colours, normalAnsi, customText, customBack);
		const bool custom16Default =
		    isEnabledFlag(m_worldAttributes.value(QStringLiteral("custom_16_is_default_colour")));
		if (const bool sameColour = (m_noteTextColour == kSameColour || m_noteTextColour < 0); sameColour)
			m_noteColourFore = custom16Default ? colorToLong(customText.value(15))
			                                   : colorToLong(normalAnsi.value(kAnsiWhite));
		else if (m_noteTextColour >= 0 && m_noteTextColour < customText.size())
			m_noteColourFore = colorToLong(customText.value(m_noteTextColour));
	}

	m_notesInRgb     = true;
	m_noteColourBack = value & 0x00FFFFFF;
}

long WorldRuntime::mapColourValue(long original) const
{
	const auto it = m_colourTranslationMap.find(original);
	if (it == m_colourTranslationMap.end())
		return original;
	return it.value();
}

long WorldRuntime::getMapColour(long value) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), value,
		                          [this, value] { return getMapColour(value); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::getMapColour");
	return mapColourValue(value);
}

long WorldRuntime::getSysColor(int index)
{
	return qmudGuiSystemColor(index);
}

long WorldRuntime::getUniqueNumber()
{
	AppController *app = AppController::instance();
	if (!app)
		return 0;
	const qint64 value = app->nextUniqueNumber();
	return value & 0x7fffffff;
}

QString WorldRuntime::makeRegularExpression(const QString &text)
{
	return convertToRegularExpression(text);
}

QString WorldRuntime::metaphone(const QString &text, int length)
{
	const QPair<QString, QString> encoded = qmudDoubleMetaphone(text, length);
	if (encoded.second.isEmpty())
		return encoded.first;
	return encoded.first + QStringLiteral(",") + encoded.second;
}

double WorldRuntime::mtRand()
{
	return runtimeRandomUnit();
}

int WorldRuntime::readNamesFile(const QString &fileName)
{
	if (fileName.isEmpty())
		return eNoNameSpecified;

	try
	{
		qmudReadNames(fileName, true);
	}
	catch (const std::exception &)
	{
		return eCouldNotOpenFile;
	}

	return eOK;
}

void WorldRuntime::mapColour(long original, long replacement)
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this, original, replacement] { mapColour(original, replacement); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::mapColour");
	m_colourTranslationMap.insert(original, replacement);
}

QMap<long, long> WorldRuntime::mapColourList() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QMap<long, long>(),
		                          [this] { return mapColourList(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::mapColourList");
	return m_colourTranslationMap;
}

long WorldRuntime::normalColour(int index) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), 0L,
		                          [this, index] { return normalColour(index); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::normalColour");
	if (index < 1 || index > 8)
		return 0;
	return colorToLong(ansiColour(false, index));
}

void WorldRuntime::setNormalColour(int index, long value)
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this, index, value] { setNormalColour(index, value); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::setNormalColour");
	if (index < 1 || index > 8)
		return;
	const auto packed = static_cast<QMudColorRef>(value & 0x00FFFFFF);
	setAnsiColour(false, index, QColor(qmudRed(packed), qmudGreen(packed), qmudBlue(packed)));
}

int WorldRuntime::addToMapper(const QString &direction, const QString &reverse)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eBadMapItem,
		                          [this, direction, reverse] { return addToMapper(direction, reverse); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::addToMapper");
	if (containsReservedMapperCharacter(direction) || containsReservedMapperCharacter(reverse))
		return eBadMapItem;

	if (direction.isEmpty() && reverse.isEmpty())
		return eBadMapItem;

	const QString entry = QStringLiteral("%1/%2").arg(direction, reverse);
	m_mappingList.append(entry);
	invalidateLuaCallbackDispatchSnapshot();
	return eOK;
}

int WorldRuntime::addMapperComment(const QString &comment)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eBadMapItem, [this, comment] { return addMapperComment(comment); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::addMapperComment");
	if (containsReservedMapperCharacter(comment))
		return eBadMapItem;

	const QString entry = QStringLiteral("{%1}").arg(comment);
	m_mappingList.append(entry);
	invalidateLuaCallbackDispatchSnapshot();
	return eOK;
}

bool WorldRuntime::containsReservedMapperCharacter(const QString &text)
{
	for (const QChar ch : text)
	{
		switch (ch.unicode())
		{
		case '{':
		case '}':
		case '(':
		case ')':
		case '/':
		case '\\':
			return true;
		default:
			break;
		}
	}
	return false;
}

int WorldRuntime::shiftTabCompleteItem(const QString &item)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eBadParameter, [this, item] { return shiftTabCompleteItem(item); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::shiftTabCompleteItem");
	if (!isValidShiftTabCompleteItem(item))
		return eBadParameter;
	if (item == QStringLiteral("<clear>"))
	{
		m_shiftTabCompleteItems.clear();
		return eOK;
	}
	if (item == QStringLiteral("<functions>"))
	{
		m_tabCompleteFunctions = true;
		return eOK;
	}
	if (item == QStringLiteral("<nofunctions>"))
	{
		m_tabCompleteFunctions = false;
		return eOK;
	}

	m_shiftTabCompleteItems.insert(item);
	return eOK;
}

bool WorldRuntime::isValidShiftTabCompleteItem(const QString &item)
{
	if (item.isEmpty() || item.size() > 30)
		return false;
	if (item == QStringLiteral("<clear>") || item == QStringLiteral("<functions>") ||
	    item == QStringLiteral("<nofunctions>"))
	{
		return true;
	}

	const ushort firstCode = item.at(0).unicode();
	if (!isAsciiLetter(firstCode))
		return false;

	return std::ranges::all_of(item,
	                           [](const QChar ch)
	                           {
		                           const ushort code = ch.unicode();
		                           return isAsciiLetter(code) || isAsciiDigit(code) || code == '_' ||
		                                  code == '-' || code == '.';
	                           });
}

int WorldRuntime::deleteLastMapItem()
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eNoMapItems, [this] { return deleteLastMapItem(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::deleteLastMapItem");
	if (m_mappingList.isEmpty())
		return eNoMapItems;
	m_mappingList.removeLast();
	invalidateLuaCallbackDispatchSnapshot();
	return eOK;
}

int WorldRuntime::deleteAllMapItems()
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eNoMapItems, [this] { return deleteAllMapItems(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::deleteAllMapItems");
	if (m_mappingList.isEmpty())
		return eNoMapItems;
	m_mappingList.clear();
	invalidateLuaCallbackDispatchSnapshot();
	return eOK;
}

void WorldRuntime::deleteLines(int count)
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this, count] { deleteLines(count); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::deleteLines");
	if (m_sessionStateOutputBufferSealed)
		return;
	if (count <= 0 || m_lines.isEmpty())
		return;
	if (count >= m_lines.size())
		m_lines.clear();
	else
		m_lines.remove(m_lines.size() - count, count);
	invalidateLuaCallbackLineBufferSnapshot();
	if (m_view)
		m_view->rebuildOutputFromLines(m_lines);
}

void WorldRuntime::deleteOutput()
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this] { deleteOutput(); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::deleteOutput");
	if (m_sessionStateOutputBufferSealed)
		return;
	m_lines.clear();
	invalidateLuaCallbackLineBufferSnapshot();
	if (m_view)
		m_view->clearOutputBuffer();
}

int WorldRuntime::deleteVariable(const QString &name)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eVariableNotFound, [this, name] { return deleteVariable(name); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::deleteVariable");
	for (int i = 0; i < m_variables.size(); ++i)
	{
		const QString varName = m_variables.at(i).attributes.value(QStringLiteral("name"));
		if (varName.compare(name, Qt::CaseInsensitive) == 0)
		{
			m_variables.removeAt(i);
			m_variableCount    = safeQSizeToInt(m_variables.size());
			m_variablesChanged = true;
			invalidateLuaCallbackDispatchSnapshot();
			return eOK;
		}
	}
	return eVariableNotFound;
}

int WorldRuntime::discardQueuedCommands() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), 0,
		                          [this] { return discardQueuedCommands(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::discardQueuedCommands");
	if (!m_commandProcessor)
		return 0;
	return m_commandProcessor->discardQueuedCommands();
}

void WorldRuntime::setMappingEnabled(bool enabled)
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this, enabled] { setMappingEnabled(enabled); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::setMappingEnabled");
	m_isMapping = enabled;
}

QString WorldRuntime::evaluateSpeedwalk(const QString &speedWalkString) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QString(),
		                          [this, speedWalkString] { return evaluateSpeedwalk(speedWalkString); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::evaluateSpeedwalk");
	if (!m_commandProcessor)
		return {};
	return m_commandProcessor->evaluateSpeedwalk(speedWalkString);
}

int WorldRuntime::executeCommand(const QString &text) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), eWorldClosed,
		                          [this, text] { return executeCommand(text); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::executeCommand");
	if (!m_commandProcessor)
		return eWorldClosed;
	return m_commandProcessor->executeCommand(text);
}

int WorldRuntime::executeUserMacroSendNow(const QString &text, const bool history) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), eWorldClosed,
		                          [this, text, history] { return executeUserMacroSendNow(text, history); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::executeUserMacroSendNow");
	if (!m_commandProcessor)
		return eWorldClosed;
	return m_commandProcessor->executeUserMacroSendNow(text, history);
}

int WorldRuntime::executeCommandWithTriggerPriority(const QString &text) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), eWorldClosed,
		                          [this, text] { return executeCommandWithTriggerPriority(text); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::executeCommandWithTriggerPriority");
	if (!m_commandProcessor)
		return eWorldClosed;
	return m_commandProcessor->executeCommandWithTriggerPriority(text);
}

QString WorldRuntime::getEntityValue(const QString &name) const
{
	QByteArray value;
	if (m_telnet.getCustomEntityValue(name.toUtf8(), value))
		return QString::fromUtf8(value);
	return {};
}

QMap<QString, QString> WorldRuntime::customEntitySnapshot() const
{
	QMap<QString, QString>             snapshot;
	const QMap<QByteArray, QByteArray> entities = m_telnet.customEntitySnapshot();
	for (auto it = entities.constBegin(); it != entities.constEnd(); ++it)
		snapshot.insert(QString::fromUtf8(it.key()), QString::fromUtf8(it.value()));
	return snapshot;
}

void WorldRuntime::setEntityValue(const QString &name, const QString &value)
{
	const QByteArray key = name.toUtf8();
	m_telnet.setCustomEntity(key, value.toUtf8());
}

void WorldRuntime::addTriggerTimeNs(qint64 ns)
{
	if (ns > 0)
		m_triggerTimeNs += ns;
}

void WorldRuntime::addAliasTimeNs(qint64 ns)
{
	if (ns > 0)
		m_aliasTimeNs += ns;
}

double WorldRuntime::triggerTimeSeconds() const
{
	return static_cast<double>(m_triggerTimeNs) / 1000000000.0;
}

double WorldRuntime::aliasTimeSeconds() const
{
	return static_cast<double>(m_aliasTimeNs) / 1000000000.0;
}

bool WorldRuntime::removeMapReverses() const
{
	return m_removeMapReverses;
}

void WorldRuntime::setRemoveMapReverses(bool enabled)
{
	m_removeMapReverses = enabled;
}

int WorldRuntime::lastGoToLine() const
{
	return m_lastGoTo;
}

void WorldRuntime::setLastGoToLine(int line)
{
	m_lastGoTo = line;
}

void WorldRuntime::fireWorldConnectHandlers()
{
	if (m_pluginInstallDeferred || m_pluginInstallInProgress || hasPendingPluginInstallWork())
	{
		m_deferredWorldConnectHandlersPending = true;
		return;
	}

	const unsigned short previousActionSource = m_currentActionSource;
	m_currentActionSource                     = eWorldAction;
	dispatchWorldNoArgCallbackByAttribute(QStringLiteral("on_world_connect"), true);
	callPluginCallbacksNoArgs(QStringLiteral("OnPluginConnect"), true);
	m_currentActionSource = previousActionSource;
}

void WorldRuntime::fireWorldDisconnectHandlers()
{
	const unsigned short previousActionSource = m_currentActionSource;
	m_currentActionSource                     = eWorldAction;
	dispatchWorldNoArgCallbackByAttribute(QStringLiteral("on_world_disconnect"), true);
	callPluginCallbacksNoArgs(QStringLiteral("OnPluginDisconnect"), true);
	m_currentActionSource = previousActionSource;
}

void WorldRuntime::fireWorldOpenHandlers()
{
	const unsigned short previousActionSource = m_currentActionSource;
	m_currentActionSource                     = eWorldAction;
	dispatchWorldNoArgCallbackByAttribute(QStringLiteral("on_world_open"), false);
	m_currentActionSource = previousActionSource;
}

void WorldRuntime::fireWorldCloseHandlers()
{
	const unsigned short previousActionSource = m_currentActionSource;
	m_currentActionSource                     = eWorldAction;
	dispatchWorldNoArgCallbackByAttribute(QStringLiteral("on_world_close"), false);
	m_currentActionSource = previousActionSource;
}

void WorldRuntime::fireWorldSaveHandlers()
{
	const unsigned short previousActionSource = m_currentActionSource;
	m_currentActionSource                     = eWorldAction;
	dispatchWorldNoArgCallbackByAttribute(QStringLiteral("on_world_save"), true);
	callPluginCallbacksNoArgs(QStringLiteral("OnPluginWorldSave"), true);
	m_currentActionSource = previousActionSource;
}

void WorldRuntime::fireWorldGetFocusHandlers()
{
	requestActiveState(true);
}

void WorldRuntime::fireWorldLoseFocusHandlers()
{
	requestActiveState(false);
}

void WorldRuntime::mxpError(int level, long messageNumber, const QString &message)
{
	if (const QString callbackName = m_worldAttributes.value(QStringLiteral("on_mxp_error")).trimmed();
	    !callbackName.isEmpty() && m_luaCallbacks)
	{
		const QSharedPointer<LuaCallbackEngine> worldLua(m_luaCallbacks,
		                                                 [](LuaCallbackEngine * /*unused*/) {});
		LuaBatchDispatchRequest                 request;
		request.kind                        = LuaBatchDispatchKind::MxpError;
		request.engines                     = {worldLua};
		request.functionName                = callbackName;
		request.intArg1                     = level;
		request.numberArg1                  = messageNumber;
		request.intArg2                     = m_linesReceived;
		request.stringArg                   = message;
		const LuaBatchDispatchResult result = queuePluginCallbackDispatch(request, true);
		if (const bool suppress = result.boolResultValid ? result.boolResult : false; suppress)
			return;
	}

	constexpr const char *sLevel[] = {" ", "E", "W", "I", "A"};
	auto                 *p        = "?";
	if (level >= 0 && level < static_cast<int>(std::size(sLevel)))
		p = sLevel[level];

	if (level == DBG_ERROR)
		m_mxpErrors++;

	int           debugLevel = DBG_NONE;
	bool          ok         = false;
	const QString rawDebug   = m_worldAttributes.value(QStringLiteral("mxp_debug_level"));
	debugLevel               = rawDebug.toInt(&ok);
	if (!ok)
	{
		const WorldNumericOption *opt =
		    QMudWorldOptions::findWorldNumericOption(QStringLiteral("mxp_debug_level"));
		if (opt)
			debugLevel = static_cast<int>(opt->defaultValue);
	}

	if (level > debugLevel)
		return;

	auto title = QStringLiteral("MXP Messages");
	if (const QString worldName = m_worldAttributes.value(QStringLiteral("name")); !worldName.isEmpty())
		title += QStringLiteral(" - %1").arg(worldName);

	const QString pluginPayload = QStringLiteral("%1,%2,%3,%4")
	                                  .arg(QString::fromLatin1(p))
	                                  .arg(messageNumber)
	                                  .arg(m_linesReceived)
	                                  .arg(message);
	callPluginCallbacks(QStringLiteral("OnPluginMXPerror"), pluginPayload, true);

	const QString formatted = QStringLiteral("%1 %2: (%3) %4\r\n")
	                              .arg(QString::fromLatin1(p))
	                              .arg(messageNumber, 5)
	                              .arg(m_linesReceived, 5)
	                              .arg(message);

	emit          mxpDebugMessage(title, formatted);
}

void WorldRuntime::mxpStartUp()
{
	if (m_mxpActive)
		return;
	m_mxpActive = true;

	if (isLuaScriptingEnabled(m_worldAttributes) && m_luaCallbacks)
	{
		const QString callbackName = m_worldAttributes.value(QStringLiteral("on_mxp_start")).trimmed();
		if (!callbackName.isEmpty())
		{
			const QSharedPointer<LuaCallbackEngine> worldLua(m_luaCallbacks,
			                                                 [](LuaCallbackEngine * /*unused*/) {});
			LuaBatchDispatchRequest                 request;
			request.kind         = LuaBatchDispatchKind::MxpStartUp;
			request.engines      = {worldLua};
			request.functionName = callbackName;
			static_cast<void>(queuePluginCallbackDispatch(request, true));
		}
	}

	callPluginCallbacksNoArgs(QStringLiteral("OnPluginMXPstart"), true);
}

void WorldRuntime::mxpShutDown()
{
	if (!m_mxpActive)
		return;
	m_mxpActive = false;
	m_mxpTagStack.clear();
	m_mxpOpenTags.clear();
	m_mxpTextBuffer.clear();
	resetMxpRenderState();
	clearAnsiActionContext();

	if (isLuaScriptingEnabled(m_worldAttributes) && m_luaCallbacks)
	{
		const QString callbackName = m_worldAttributes.value(QStringLiteral("on_mxp_stop")).trimmed();
		if (!callbackName.isEmpty())
		{
			const QSharedPointer<LuaCallbackEngine> worldLua(m_luaCallbacks,
			                                                 [](LuaCallbackEngine * /*unused*/) {});
			LuaBatchDispatchRequest                 request;
			request.kind         = LuaBatchDispatchKind::MxpShutDown;
			request.engines      = {worldLua};
			request.functionName = callbackName;
			static_cast<void>(queuePluginCallbackDispatch(request, true));
		}
	}

	callPluginCallbacksNoArgs(QStringLiteral("OnPluginMXPstop"), true);
}

void WorldRuntime::resetMxp()
{
	m_telnet.resetMxp();
	mxpShutDown();
}

void WorldRuntime::resetMxpTags()
{
	m_telnet.resetMxp();
	m_mxpTagStack.clear();
	m_mxpOpenTags.clear();
	m_mxpTextBuffer.clear();
	resetMxpRenderState();
	clearAnsiActionContext();
}

void WorldRuntime::resetIpCache()
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this] { resetIpCache(); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::resetIpCache");
	m_hasCachedIp = false;
}

void WorldRuntime::resetAllTimers()
{
	for (auto &timer : m_timers)
		resetTimerFields(timer);
}

bool WorldRuntime::hasCachedIp() const
{
	return m_hasCachedIp;
}

int WorldRuntime::mxpBuiltinElementCount()
{
	return mxpBuiltinTagCount();
}

int WorldRuntime::mxpBuiltinEntityCount()
{
	return TelnetProcessor::builtinEntityCount();
}

int WorldRuntime::mxpOpenTagCount() const
{
	return safeQSizeToInt(m_mxpOpenTags.size());
}

int WorldRuntime::openLog(const QString &logFileName, bool append)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eCouldNotOpenFile,
		                          [this, logFileName, append] { return openLog(logFileName, append); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::openLog");
	if (m_logFile.isOpen())
		return eLogFileAlreadyOpen;

	m_logFileName = logFileName;

	if (m_logFileName.isEmpty())
	{
		if (const QString autoName = m_worldAttributes.value(QStringLiteral("auto_log_file_name"));
		    !autoName.isEmpty())
		{
			if (const QString playerName = m_worldAttributes.value(QStringLiteral("player")).trimmed();
			    patternUsesPlayerDirective(autoName) && playerName.isEmpty())
				return eCouldNotOpenFile;
			m_logFileName = formatTime(QDateTime::currentDateTime(), autoName, false);
		}
	}

	if (m_logFileName.isEmpty())
		return eCouldNotOpenFile;

	const QString workingDir = resolveWorkingDir(m_startupDirectory);
	QString       logBaseDir = m_defaultLogDirectory.trimmed();
	if (logBaseDir.isEmpty())
		logBaseDir = QDir::cleanPath(QDir(workingDir).filePath(QStringLiteral("logs")));
	else
		logBaseDir = makeAbsolutePath(logBaseDir, workingDir);
	const QString baseRelative =
	    QMudPluginPathUtils::qmudHomeRelativePath(m_startupDirectory, logBaseDir, true);
	QString logRelative = QMudPluginPathUtils::legacyPathRelativeToQmudHome(m_logFileName);
	if (logRelative.isEmpty() && !QMudPluginPathUtils::normalizeSeparators(m_logFileName).isEmpty())
		return eCouldNotOpenFile;
	if (!baseRelative.isEmpty() && baseRelative != QLatin1String("./") &&
	    !logRelative.startsWith(QStringLiteral("logs/"), Qt::CaseInsensitive) &&
	    !logRelative.startsWith(QStringLiteral("worlds/"), Qt::CaseInsensitive))
		logRelative = QDir::cleanPath(QDir(baseRelative).filePath(logRelative));
	QString error;
	if (!QMudPluginPathUtils::resolveInsideQmudHome(m_startupDirectory, logRelative, &m_logFileName, &error))
		return eCouldNotOpenFile;

	// Ensure missing parent folders (for patterns like logs/foo/bar.txt) exist.
	const QFileInfo logInfo(m_logFileName);
	if (const QString parentDir = logInfo.path(); !parentDir.isEmpty() && parentDir != QStringLiteral("."))
	{
		if (const QDir parent(parentDir); !parent.exists() && !QDir().mkpath(parentDir))
			return eCouldNotOpenFile;
	}

	m_logRotationBaseFileName = m_logFileName;
	m_logFile.setFileName(m_logFileName);
	QIODevice::OpenMode mode = QIODevice::WriteOnly;
	if (append)
		mode |= QIODevice::Append;
	else
		mode |= QIODevice::Truncate;

	if (!m_logFile.open(mode))
		return eCouldNotOpenFile;

	m_lastFlushTime = QDateTime::currentDateTime();
	emit logStateChanged(true);
	return eOK;
}

int WorldRuntime::closeLog()
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eLogFileNotOpen, [this] { return closeLog(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::closeLog");
	if (!m_logFile.isOpen())
		return eLogFileNotOpen;
	const QString closedLogFileName = m_logFileName;

	const bool    logRaw = isEnabledFlag(m_worldAttributes.value(QStringLiteral("log_raw")));
	if (!logRaw)
	{
		const QMap<QString, QString> &multi     = worldMultilineAttributes();
		QString                       postamble = multi.value(QStringLiteral("log_file_postamble"));
		if (postamble.isEmpty())
			postamble = m_worldAttributes.value(QStringLiteral("log_file_postamble"));
		if (!postamble.isEmpty())
		{
			const bool      logHtml = isEnabledFlag(m_worldAttributes.value(QStringLiteral("log_html")));
			QDateTime const now     = QDateTime::currentDateTime();
			postamble.replace(QStringLiteral("%n"), QStringLiteral("\n"));
			postamble = formatTime(now, postamble, logHtml);
			postamble.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
			writeLog(postamble);
			writeLog(QStringLiteral("\n"));
		}
	}

	m_logFile.close();
	if (logRotateGzipEnabled() && !closedLogFileName.isEmpty() && QFileInfo::exists(closedLogFileName))
	{
		QString gzipError;
		if (!gzipFileInPlace(closedLogFileName, &gzipError) && !gzipError.isEmpty())
			QMessageBox::warning(nullptr, QStringLiteral("QMud"), gzipError);
	}
	emit logStateChanged(false);
	return eOK;
}

qint64 WorldRuntime::logRotateLimitBytes() const
{
	bool   ok       = false;
	qint64 rotateMb = m_worldAttributes.value(QStringLiteral("log_rotate_mb")).toLongLong(&ok);
	if (!ok)
		rotateMb = 100;
	if (rotateMb <= 0)
		return 0;
	return rotateMb * 1024 * 1024;
}

bool WorldRuntime::logRotateGzipEnabled() const
{
	return isEnabledFlag(m_worldAttributes.value(QStringLiteral("log_rotate_gzip"), QStringLiteral("1")));
}

QString WorldRuntime::buildRotatedLogFileName(const QString &previousFileName) const
{
	const QString baseName  = m_logRotationBaseFileName.isEmpty() ? m_logFileName : m_logRotationBaseFileName;
	QString       candidate = baseName;
	int           index     = 2;
	while (candidate == previousFileName || QFileInfo::exists(candidate) ||
	       QFileInfo::exists(candidate + QStringLiteral(".gz")))
	{
		candidate = appendNumberBeforeExtension(baseName, index);
		++index;
	}
	return candidate;
}

bool WorldRuntime::gzipFileInPlace(const QString &sourceFileName, QString *errorMessage)
{
	return qmudGzipFileInPlace(sourceFileName, errorMessage);
}

int WorldRuntime::rotateLogFile()
{
	if (!m_logFile.isOpen())
		return eLogFileNotOpen;
	if (m_logRotateInProgress)
		return eOK;

	m_logRotateInProgress          = true;
	const QString previousFileName = m_logFileName;
	const bool    gzipRotated      = logRotateGzipEnabled();

	m_logFile.flush();
	m_logFile.close();

	if (gzipRotated && QFileInfo::exists(previousFileName))
	{
		QString gzipError;
		if (!gzipFileInPlace(previousFileName, &gzipError) && !gzipError.isEmpty())
			QMessageBox::warning(nullptr, QStringLiteral("QMud"), gzipError);
	}

	const QString nextFileName = buildRotatedLogFileName(previousFileName);
	m_logFileName              = nextFileName;
	m_logFile.setFileName(nextFileName);
	if (!m_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
	{
		const QString openError = QStringLiteral("Unable to open rotated log file \"%1\"").arg(nextFileName);
		QMessageBox::warning(nullptr, QStringLiteral("QMud"), openError);
		m_logFileName = previousFileName;
		m_logFile.setFileName(previousFileName);
		if (!m_logFile.open(QIODevice::WriteOnly | QIODevice::Append))
			QMessageBox::warning(
			    nullptr, QStringLiteral("QMud"),
			    QStringLiteral("Unable to reopen previous log file \"%1\"").arg(previousFileName));
		m_logRotateInProgress = false;
		return eCouldNotOpenFile;
	}

	m_lastFlushTime       = QDateTime::currentDateTime();
	m_logRotateInProgress = false;
	return eOK;
}

int WorldRuntime::writeLog(const QByteArray &bytes)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eLogFileNotOpen, [this, bytes] { return writeLog(bytes); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::writeLog(QByteArray)");
	if (!m_logFile.isOpen())
		return eLogFileNotOpen;
	if (bytes.isEmpty())
		return eOK;

	const qint64 written = m_logFile.write(bytes);
	if (written != bytes.size())
	{
		QString const str = QStringLiteral("An error occurred writing to log file \"%1\"").arg(m_logFileName);
		m_logFile.close();
		QMessageBox::warning(nullptr, QStringLiteral("QMud"), str);
		return eLogFileBadWrite;
	}

	m_lastFlushTime          = QDateTime::currentDateTime();
	const qint64 rotateLimit = logRotateLimitBytes();
	if (rotateLimit > 0 && m_logFile.size() >= rotateLimit)
		return rotateLogFile();
	return eOK;
}

int WorldRuntime::writeLog(const QString &text)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eLogFileNotOpen, [this, text] { return writeLog(text); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::writeLog(QString)");
	return writeLog(text.toLocal8Bit());
}

int WorldRuntime::flushLog()
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eLogFileNotOpen, [this] { return flushLog(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::flushLog");
	if (!m_logFile.isOpen())
		return eLogFileNotOpen;
	return m_logFile.flush() ? eOK : eLogFileBadWrite;
}

bool WorldRuntime::isLogOpen() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodTrueOnSuccess(const_cast<WorldRuntime *>(this),
		                                     [this] { return isLogOpen(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::isLogOpen");
	return m_logFile.isOpen();
}

QString WorldRuntime::logFileName() const
{
	return m_logFileName;
}

qint64 WorldRuntime::logFilePosition() const
{
	if (!m_logFile.isOpen())
		return 0;
	return m_logFile.pos();
}

QString WorldRuntime::formatTime(const QDateTime &time, const QString &format, bool fixHtml) const
{
	const auto workingDir = TimeFormatUtils::resolveWorkingDir(m_startupDirectory);
	const auto worldDir   = TimeFormatUtils::makeAbsolutePath(m_defaultWorldDirectory, workingDir);
	const auto logDir     = TimeFormatUtils::makeAbsolutePath(m_defaultLogDirectory, workingDir);

	TimeFormatUtils::WorldTimeFormatContext context;
	context.workingDir = workingDir;
	context.worldName  = m_worldAttributes.value(QStringLiteral("name"));
	context.playerName = m_worldAttributes.value(QStringLiteral("player"));
	context.worldDir   = worldDir;
	context.logDir     = logDir;

	return TimeFormatUtils::formatWorldTime(time, format, context, fixHtml, &fixHtmlString);
}

void WorldRuntime::setDefaultWorldDirectory(const QString &path)
{
	m_defaultWorldDirectory = QMudPluginPathUtils::qmudHomeRelativePath(m_startupDirectory, path, true);
}

void WorldRuntime::setDefaultLogDirectory(const QString &path)
{
	m_defaultLogDirectory = QMudPluginPathUtils::qmudHomeRelativePath(m_startupDirectory, path, true);
}

void WorldRuntime::setStartupDirectory(const QString &path)
{
	m_startupDirectory = path;
}

QString WorldRuntime::startupDirectory() const
{
	return m_startupDirectory;
}

QString WorldRuntime::defaultWorldDirectory() const
{
	return m_defaultWorldDirectory;
}

QString WorldRuntime::defaultLogDirectory() const
{
	return m_defaultLogDirectory;
}

void WorldRuntime::setWindowTitleOverride(const QString &title)
{
	if (m_windowTitleOverride == title)
		return;
	m_windowTitleOverride = title;
	emit windowTitleChanged();
}

QString WorldRuntime::windowTitleOverride() const
{
	return m_windowTitleOverride;
}

void WorldRuntime::setMainTitleOverride(const QString &title)
{
	if (m_mainTitleOverride == title)
		return;
	m_mainTitleOverride = title;
	emit mainTitleChanged();
}

QString WorldRuntime::mainTitleOverride() const
{
	return m_mainTitleOverride;
}

bool WorldRuntime::hasAnyPluginCallback(const QString &functionName)
{
	if (functionName.isEmpty())
		return false;

	if (applyPendingObservedPluginCallbackPresenceSnapshots())
		m_pluginCallbackPresenceDirty = true;

	noteObservedPluginCallbackQuery(m_observedPluginCallbackQueryGeneration, functionName,
	                                m_observedPluginCallbackGeneration);

	if (ensureObservedPluginCallback(m_observedPluginCallbacks, functionName))
	{
		m_observedPluginCallbacksPendingWarmup.insert(functionName);
		scheduleObservedPluginCallbackPropagation();
		m_pluginCallbackPresenceDirty = true;
	}

	if (m_plugins.isEmpty())
		return false;

	const int pluginCount = safeQSizeToInt(m_plugins.size());
	if (m_pluginCallbackPresencePluginCount != pluginCount)
	{
		m_pluginCallbackPresencePluginCount = pluginCount;
		m_pluginCallbackPresenceDirty       = true;
	}

	if (m_pluginCallbackPresenceDirty)
		rebuildPluginCallbackPresenceCache();
	if (m_pluginCallbackPresenceCounts.value(functionName) > 0)
		return true;
	if (!isObservedPluginCallbackPropagationPending(functionName))
		return false;
	return hasAnyExecutableLuaPluginRecipient();
}

bool WorldRuntime::hasPluginCallbackRecipient(const QString &functionName)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, false,
		                          [this, functionName] { return hasPluginCallbackRecipient(functionName); });
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::hasPluginCallbackRecipient");
	return hasAnyPluginCallback(functionName);
}

void WorldRuntime::invalidatePluginCallbackPresenceCache()
{
	if (QThread::currentThread() != thread())
	{
		bool expected = false;
		if (!m_pluginCallbackPresenceInvalidateQueued.compare_exchange_strong(
		        expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
		{
			return;
		}
		const bool queued = qmudInvokeMethodQueued(this,
		                                           [this]
		                                           {
			                                           m_pluginCallbackPresenceInvalidateQueued.store(
			                                               false, std::memory_order_release);
			                                           invalidatePluginCallbackPresenceCache();
		                                           });
		if (!queued)
		{
			m_pluginCallbackPresenceInvalidateQueued.store(false, std::memory_order_release);
			qWarning().noquote() << QStringLiteral(
			    "[QMud][LuaBridge] Failed to queue plugin callback presence cache invalidation");
		}
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::invalidatePluginCallbackPresenceCache");
	m_pluginCallbackPresenceInvalidateQueued.store(false, std::memory_order_release);
	m_pluginCallbackPresenceDirty = true;
}

void WorldRuntime::recordObservedPluginCallbackPresenceSnapshot(const QString       &pluginId,
                                                                const QSet<QString> &presentCallbacks,
                                                                const QSet<QString> &luaFunctions)
{
	const QString normalizedId = normalizedPluginId(pluginId);
	if (normalizedId.isEmpty())
		return;
	{
		QMutexLocker locker(&m_pluginObservedCallbackPresenceMutex);
		m_pendingPluginObservedCallbackPresenceById.insert(normalizedId, presentCallbacks);
		m_pendingPluginLuaFunctionCatalogById.insert(normalizedId, luaFunctions);
	}
	invalidatePluginCallbackPresenceCache();
}

bool WorldRuntime::applyPendingObservedPluginCallbackPresenceSnapshots()
{
	QHash<QString, QSet<QString>> pendingSnapshots;
	QHash<QString, QSet<QString>> pendingFunctionCatalogs;
	{
		QMutexLocker locker(&m_pluginObservedCallbackPresenceMutex);
		if (m_pendingPluginObservedCallbackPresenceById.isEmpty() &&
		    m_pendingPluginLuaFunctionCatalogById.isEmpty())
			return false;
		pendingSnapshots.swap(m_pendingPluginObservedCallbackPresenceById);
		pendingFunctionCatalogs.swap(m_pendingPluginLuaFunctionCatalogById);
	}

	bool changed = false;
	for (auto it = pendingSnapshots.constBegin(); it != pendingSnapshots.constEnd(); ++it)
	{
		const QString       &pluginId    = it.key();
		const QSet<QString> &presentSet  = it.value();
		const auto           existingIt  = m_pluginObservedCallbackPresenceById.constFind(pluginId);
		const bool           hasExisting = existingIt != m_pluginObservedCallbackPresenceById.constEnd();
		if (const bool existingSetIsSame = hasExisting && existingIt.value() == presentSet; existingSetIsSame)
			continue;
		if (presentSet.isEmpty())
			m_pluginObservedCallbackPresenceById.remove(pluginId);
		else
			m_pluginObservedCallbackPresenceById.insert(pluginId, presentSet);
		changed = true;
	}
	for (auto it = pendingFunctionCatalogs.constBegin(); it != pendingFunctionCatalogs.constEnd(); ++it)
	{
		const QString       &pluginId    = it.key();
		const QSet<QString> &functionSet = it.value();
		const auto           existingIt  = m_pluginLuaFunctionCatalogById.constFind(pluginId);
		const bool           hasExisting = existingIt != m_pluginLuaFunctionCatalogById.constEnd();
		if (hasExisting && existingIt.value() == functionSet)
			continue;
		if (functionSet.isEmpty())
			m_pluginLuaFunctionCatalogById.remove(pluginId);
		else
			m_pluginLuaFunctionCatalogById.insert(pluginId, functionSet);
		changed = true;
	}
	return changed;
}

bool WorldRuntime::isObservedPluginCallbackPropagationPending(const QString &functionName) const
{
	return !functionName.isEmpty() && m_observedPluginCallbacksPendingWarmup.contains(functionName);
}

bool WorldRuntime::hasAnyExecutableLuaPluginRecipient() const
{
	return std::ranges::any_of(m_plugins, [](const Plugin &plugin) { return canExecutePlugin(plugin); });
}

QVector<int>
WorldRuntime::collectExecutablePluginRecipientIndicesWithWarmupFallback(const QString &functionName) const
{
	QVector<int> recipientIndices = collectExecutablePluginRecipientIndicesForCallback(
	    functionName, m_plugins, m_pluginCallbackRecipientIndices);
	if (!recipientIndices.isEmpty() || !isObservedPluginCallbackPropagationPending(functionName))
		return recipientIndices;

	recipientIndices.reserve(m_plugins.size());
	for (int pluginIndex = 0; pluginIndex < m_plugins.size(); ++pluginIndex)
	{
		const Plugin &plugin = m_plugins.at(pluginIndex);
		if (!canExecutePlugin(plugin))
			continue;
		recipientIndices.push_back(pluginIndex);
	}
	return recipientIndices;
}

QVector<QSharedPointer<LuaCallbackEngine>>
WorldRuntime::collectExecutablePluginRecipientEnginesWithWarmupFallback(const QString &functionName) const
{
	const QVector<int> recipientIndices =
	    collectExecutablePluginRecipientIndicesWithWarmupFallback(functionName);
	if (recipientIndices.isEmpty())
		return {};

	QVector<QSharedPointer<LuaCallbackEngine>> recipients;
	recipients.reserve(recipientIndices.size());
	for (const int pluginIndex : recipientIndices)
	{
		if (pluginIndex < 0 || pluginIndex >= m_plugins.size())
			continue;
		const Plugin &plugin = m_plugins.at(pluginIndex);
		if (!plugin.lua)
			continue;
		recipients.push_back(plugin.lua);
	}
	return recipients;
}

void WorldRuntime::scheduleObservedPluginCallbackPropagation()
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::scheduleObservedPluginCallbackPropagation");

	if (m_observedPluginCallbacksPendingWarmup.isEmpty())
		return;
	if (m_observedPluginCallbackPropagationInFlight)
	{
		m_observedPluginCallbackPropagationQueued = true;
		return;
	}

	QVector<QSharedPointer<LuaCallbackEngine>> engines;
	engines.reserve(m_plugins.size());
	for (const Plugin &plugin : m_plugins)
	{
		if (plugin.lua)
			engines.push_back(plugin.lua);
	}

	if (engines.isEmpty() || !m_luaExecutor)
	{
		m_observedPluginCallbacksPendingWarmup.clear();
		m_observedPluginCallbackPropagationInFlight = false;
		m_observedPluginCallbackPropagationQueued   = false;
		return;
	}

	m_observedPluginCallbackPropagationInFlight     = true;
	m_observedPluginCallbackPropagationQueued       = false;
	const QSet<QString>     propagatedCallbackNames = m_observedPluginCallbacks;

	LuaBatchDispatchRequest request;
	request.kind                     = LuaBatchDispatchKind::UpdateObservedCallbacksMany;
	request.engines                  = std::move(engines);
	request.observedCallbackNamesArg = propagatedCallbackNames;

	m_luaExecutor->dispatchBatchAsync(
	    request, this, [this, propagatedCallbackNames](const LuaBatchDispatchResult & /*unused*/)
	    { onObservedPluginCallbackPropagationCompleted(propagatedCallbackNames); });
}

void WorldRuntime::onObservedPluginCallbackPropagationCompleted(const QSet<QString> &propagatedCallbackNames)
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::onObservedPluginCallbackPropagationCompleted");

	for (const QString &callbackName : propagatedCallbackNames)
	{
		if (callbackName.isEmpty())
			continue;
		m_observedPluginCallbacksPendingWarmup.remove(callbackName);
	}
	m_pluginCallbackPresenceDirty               = true;
	m_observedPluginCallbackPropagationInFlight = false;

	if (m_observedPluginCallbackPropagationQueued || !m_observedPluginCallbacksPendingWarmup.isEmpty())
	{
		m_observedPluginCallbackPropagationQueued = false;
		scheduleObservedPluginCallbackPropagation();
	}
}

void WorldRuntime::rebuildPluginCallbackPresenceCache()
{
	if (applyPendingObservedPluginCallbackPresenceSnapshots())
		m_pluginCallbackPresenceDirty = true;

	m_pluginCallbackPresenceCounts.clear();
	m_pluginCallbackRecipientIndices.clear();
	m_pluginCallbackRecipientEngines.clear();
	pruneStaleObservedPluginCallbacks(m_observedPluginCallbacks, m_observedPluginCallbackQueryGeneration,
	                                  m_observedPluginCallbackGeneration,
	                                  observedPluginCallbackRetentionGenerations());

	if (m_observedPluginCallbacks.isEmpty())
	{
		m_pluginCallbackPresencePluginCount = safeQSizeToInt(m_plugins.size());
		m_pluginCallbackPresenceDirty       = false;
		advanceObservedPluginCallbackGeneration(m_observedPluginCallbackGeneration);
		return;
	}

	for (int pluginIndex = 0; pluginIndex < m_plugins.size(); ++pluginIndex)
	{
		const auto &plugin = m_plugins[pluginIndex];
		if (!canExecutePlugin(plugin))
			continue;
		const QString pluginId = normalizedPluginId(plugin.attributes.value(QStringLiteral("id")));
		if (pluginId.isEmpty())
			continue;
		const QSet<QString> presentCallbacks = m_pluginObservedCallbackPresenceById.value(pluginId);
		for (const QString &name : presentCallbacks)
		{
			if (name.isEmpty() || !m_observedPluginCallbacks.contains(name))
				continue;
			++m_pluginCallbackPresenceCounts[name];
			m_pluginCallbackRecipientIndices[name].push_back(pluginIndex);
			m_pluginCallbackRecipientEngines[name].push_back(plugin.lua);
		}
	}
	m_pluginCallbackPresencePluginCount = safeQSizeToInt(m_plugins.size());
	m_pluginCallbackPresenceDirty       = false;
	advanceObservedPluginCallbackGeneration(m_observedPluginCallbackGeneration);
}

QVector<QSharedPointer<LuaCallbackEngine>>
WorldRuntime::collectPluginCallbackRecipients(const QString &functionName)
{
	if (!hasAnyPluginCallback(functionName))
		return {};
	return collectExecutablePluginRecipientEnginesWithWarmupFallback(functionName);
}

void WorldRuntime::revalidateObservedCallbackRecipients(LuaBatchDispatchRequest &request)
{
	if (!request.revalidateObservedRecipients || request.functionName.isEmpty() || request.engines.isEmpty())
		return;

	if (applyPendingObservedPluginCallbackPresenceSnapshots())
		m_pluginCallbackPresenceDirty = true;

	const int pluginCount = safeQSizeToInt(m_plugins.size());
	if (m_pluginCallbackPresencePluginCount != pluginCount)
	{
		m_pluginCallbackPresencePluginCount = pluginCount;
		m_pluginCallbackPresenceDirty       = true;
	}

	if (m_pluginCallbackPresenceDirty)
		rebuildPluginCallbackPresenceCache();

	const QVector<QSharedPointer<LuaCallbackEngine>> currentRecipients =
	    collectExecutablePluginRecipientEnginesWithWarmupFallback(request.functionName);
	if (currentRecipients.isEmpty())
	{
		request.engines.clear();
		return;
	}

	QSet<const LuaCallbackEngine *> currentSet;
	currentSet.reserve(currentRecipients.size());
	for (const QSharedPointer<LuaCallbackEngine> &engine : currentRecipients)
	{
		if (engine)
			currentSet.insert(engine.data());
	}
	if (currentSet.isEmpty())
	{
		request.engines.clear();
		return;
	}

	QVector<QSharedPointer<LuaCallbackEngine>> filteredRecipients;
	filteredRecipients.reserve(request.engines.size());
	for (const QSharedPointer<LuaCallbackEngine> &engine : request.engines)
	{
		if (engine && currentSet.contains(engine.data()))
			filteredRecipients.push_back(engine);
	}
	request.engines = std::move(filteredRecipients);
}

bool WorldRuntime::shouldDispatchPluginAsyncResult(const Plugin &plugin, const QString &apiName)
{
	if (plugin.asyncResultFilterAll)
		return true;
	const QString normalizedApiName = normalizedAsyncResultApiName(apiName);
	if (normalizedApiName.isEmpty())
		return false;
	return plugin.asyncResultFilterApis.contains(normalizedApiName);
}

void WorldRuntime::beginMiniWindowCallbackScriptExecution(const QString &windowName)
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::beginMiniWindowCallbackScriptExecution");
	if (windowName.isEmpty())
		return;
	auto it = m_miniWindows.find(windowName);
	if (it == m_miniWindows.end())
		return;
	MiniWindow &window = it.value();
	++window.executingScriptDepth;
	window.executingScript = true;
}

void WorldRuntime::endMiniWindowCallbackScriptExecution(const QString &windowName)
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::endMiniWindowCallbackScriptExecution");
	if (windowName.isEmpty())
		return;
	auto it = m_miniWindows.find(windowName);
	if (it == m_miniWindows.end())
		return;
	MiniWindow &window = it.value();
	if (window.executingScriptDepth > 0)
		--window.executingScriptDepth;
	window.executingScript = window.executingScriptDepth > 0;
}

void WorldRuntime::beginPluginCallbackDispatchCommandGuard(PluginCallbackDispatchCommand &command)
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::beginPluginCallbackDispatchCommandGuard");
	if (command.miniWindowExecutionGuardActive || command.request.miniWindowExecutionName.isEmpty())
		return;
	if (!m_miniWindows.contains(command.request.miniWindowExecutionName))
		return;
	beginMiniWindowCallbackScriptExecution(command.request.miniWindowExecutionName);
	command.miniWindowExecutionGuardActive = true;
}

void WorldRuntime::endPluginCallbackDispatchCommandGuard(PluginCallbackDispatchCommand &command)
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::endPluginCallbackDispatchCommandGuard");
	if (!command.miniWindowExecutionGuardActive)
		return;
	endMiniWindowCallbackScriptExecution(command.request.miniWindowExecutionName);
	command.miniWindowExecutionGuardActive = false;
}

void WorldRuntime::finishPluginCallbackDispatchCommand(PluginCallbackDispatchCommand &&command,
                                                       LuaBatchDispatchResult        &&result)
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::finishPluginCallbackDispatchCommand");
	endPluginCallbackDispatchCommandGuard(command);
	if (command.retainResult)
	{
		m_pluginCallbackDispatchResults.insert(command.id, std::move(result));
		return;
	}
	if (command.completion)
		command.completion(result);
}

void WorldRuntime::storeSuspendedPluginCallbackDispatch(PluginCallbackDispatchCommand &&command,
                                                        LuaBatchDispatchResult        &&result,
                                                        const int                       nextEngineIndex)
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::storeSuspendedPluginCallbackDispatch");
	const quint64 engineModalResumeId = result.modalResumeId;
	if (engineModalResumeId == 0)
	{
		finishPluginCallbackDispatchCommand(std::move(command), std::move(result));
		return;
	}
	endPluginCallbackDispatchCommandGuard(command);
	quint64 runtimeResumeId = 0;
	do
	{
		runtimeResumeId = m_nextSuspendedPluginCallbackResumeId++;
		if (runtimeResumeId == 0)
			runtimeResumeId = m_nextSuspendedPluginCallbackResumeId++;
	} while (runtimeResumeId == 0 || m_suspendedPluginCallbackDispatches.contains(runtimeResumeId));

	const bool                   hasModalRequest = result.hasPendingModalStringRequest;
	LuaPendingModalStringRequest modalRequest    = std::move(result.pendingModalStringRequest);
	result.hasPendingModalStringRequest          = false;
	result.modalResumeId                         = runtimeResumeId;
	SuspendedPluginCallbackDispatch suspended;
	const int                       suspendedEngineIndex = nextEngineIndex - 1;
	if (suspendedEngineIndex >= 0 && suspendedEngineIndex < command.request.engines.size())
	{
		if (const QSharedPointer<LuaCallbackEngine> &engine =
		        command.request.engines.at(suspendedEngineIndex);
		    engine)
			suspended.pluginId = engine->pluginIdMetadata();
	}
	suspended.command                     = std::move(command);
	suspended.partialResult               = std::move(result);
	suspended.beforeRuntimeResumeCallback = std::move(modalRequest.beforeRuntimeResumeCallback);
	suspended.engineModalResumeId         = engineModalResumeId;
	suspended.nextEngineIndex             = nextEngineIndex;
	m_suspendedPluginCallbackDispatches.insert(runtimeResumeId, std::move(suspended));
	if (hasModalRequest)
		postLuaModalStringRequest(runtimeResumeId, std::move(modalRequest));
}

void WorldRuntime::postLuaModalStringRequest(const quint64 resumeId, LuaPendingModalStringRequest &&request)
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::postLuaModalStringRequest");
	if (resumeId == 0)
		return;
	auto requestPtr = std::make_shared<LuaPendingModalStringRequest>(std::move(request));
	const QPointer<WorldRuntime> runtimeGuard(this);
	auto                         requestIsCurrent = [runtimeGuard, resumeId]() -> bool
	{
		if (!runtimeGuard)
			return false;
		if (QThread::currentThread() != runtimeGuard->thread())
			return true;
		const auto suspendedIt = runtimeGuard->m_suspendedPluginCallbackDispatches.constFind(resumeId);
		return suspendedIt != runtimeGuard->m_suspendedPluginCallbackDispatches.constEnd() &&
		       !suspendedIt->resumeQueued;
	};
	auto runRequest = [resumeId, requestPtr, requestIsCurrent]() mutable
	{
		if (!requestIsCurrent())
			return;
		QString result;
		if (requestPtr->guiCallable)
			result = requestPtr->guiCallable();
		if (!requestIsCurrent())
			return;
		if (requestPtr->resultCallback)
			requestPtr->resultCallback(resumeId, result);
	};

	QCoreApplication *app = QCoreApplication::instance();
	if (!app)
	{
		if (requestPtr->resultCallback)
			requestPtr->resultCallback(resumeId, QString());
		return;
	}
	if (QThread::currentThread() == app->thread())
	{
		if (!m_pluginCallbackDispatchActive)
		{
			runRequest();
			return;
		}

		auto runRequestWithReleasedCallbackLane = [runtimeGuard, runRequest = std::move(runRequest)]() mutable
		{
			if (!runtimeGuard)
				return;
			const bool restoreDispatchActive = runtimeGuard->m_pluginCallbackDispatchActive;
			if (restoreDispatchActive)
				runtimeGuard->m_pluginCallbackDispatchActive = false;
			const auto restoreCallbackLane = qScopeGuard(
			    [runtimeGuard, restoreDispatchActive]
			    {
				    if (runtimeGuard && restoreDispatchActive)
					    runtimeGuard->m_pluginCallbackDispatchActive = true;
			    });
			Q_UNUSED(restoreCallbackLane);
			runRequest();
		};
		const bool queued = QMetaObject::invokeMethod(this, std::move(runRequestWithReleasedCallbackLane),
		                                              Qt::QueuedConnection);
		if (!queued && requestPtr->resultCallback)
			requestPtr->resultCallback(resumeId, QString());
		return;
	}
	const bool queued = QMetaObject::invokeMethod(app, std::move(runRequest), Qt::QueuedConnection);
	if (!queued && requestPtr->resultCallback)
		requestPtr->resultCallback(resumeId, QString());
}

bool WorldRuntime::hasSuspendedPluginCallbackDispatchCommand(const quint64 commandId) const
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::hasSuspendedPluginCallbackDispatchCommand");
	return std::ranges::any_of(m_suspendedPluginCallbackDispatches,
	                           [commandId](const SuspendedPluginCallbackDispatch &suspended)
	                           { return suspended.command.id == commandId; });
}

void WorldRuntime::cancelSuspendedPluginCallbackDispatchesForEngines(
    const QVector<QSharedPointer<LuaCallbackEngine>> &engines)
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::cancelSuspendedPluginCallbackDispatchesForEngines");
	QSet<LuaCallbackEngine *> targetEngines;
	targetEngines.reserve(engines.size());
	for (const QSharedPointer<LuaCallbackEngine> &engine : engines)
	{
		if (engine)
			targetEngines.insert(engine.data());
	}
	if (targetEngines.isEmpty())
		return;

	QVector<quint64> resumeIds;
	for (auto it = m_suspendedPluginCallbackDispatches.constBegin();
	     it != m_suspendedPluginCallbackDispatches.constEnd(); ++it)
	{
		bool targetsEngine = false;
		for (const QSharedPointer<LuaCallbackEngine> &engine : it->command.request.engines)
		{
			if (engine && targetEngines.contains(engine.data()))
			{
				targetsEngine = true;
				break;
			}
		}
		if (targetsEngine)
			resumeIds.push_back(it.key());
	}

	for (const quint64 resumeId : resumeIds)
	{
		auto it = m_suspendedPluginCallbackDispatches.find(resumeId);
		if (it == m_suspendedPluginCallbackDispatches.end())
			continue;
		const SuspendedPluginCallbackDispatch &suspendedDispatch    = it.value();
		const int                              suspendedEngineIndex = suspendedDispatch.nextEngineIndex - 1;
		auto dispatchCancelToEngine = [this, engineResumeId = suspendedDispatch.engineModalResumeId](
		                                  const QSharedPointer<LuaCallbackEngine> &engine) -> bool
		{
			if (!engine)
				return false;
			LuaBatchDispatchRequest cancelRequest;
			cancelRequest.kind          = LuaBatchDispatchKind::CancelSuspendedModalString;
			cancelRequest.engines       = {engine};
			cancelRequest.modalResumeId = engineResumeId;
			static_cast<void>(dispatchLuaBatch(cancelRequest));
			return true;
		};
		bool cancelledSuspendedEngine = false;
		if (suspendedEngineIndex >= 0 &&
		    suspendedEngineIndex < suspendedDispatch.command.request.engines.size())
		{
			if (const QSharedPointer<LuaCallbackEngine> &engine =
			        suspendedDispatch.command.request.engines.at(suspendedEngineIndex);
			    engine)
				cancelledSuspendedEngine = dispatchCancelToEngine(engine);
		}
		if (!cancelledSuspendedEngine)
		{
			QSet<LuaCallbackEngine *> cancelledEngines;
			for (const QSharedPointer<LuaCallbackEngine> &engine : suspendedDispatch.command.request.engines)
			{
				if (!engine || !targetEngines.contains(engine.data()) ||
				    cancelledEngines.contains(engine.data()))
					continue;
				if (dispatchCancelToEngine(engine))
					cancelledEngines.insert(engine.data());
			}
		}
		SuspendedPluginCallbackDispatch suspended = std::move(it.value());
		m_suspendedPluginCallbackDispatches.erase(it);
		LuaBatchDispatchResult fallback = pluginCallbackDispatchFallback(suspended.command.request);
		finishPluginCallbackDispatchCommand(std::move(suspended.command), std::move(fallback));
	}
}

void WorldRuntime::finishSuspendedPluginCallbackDispatchWithFallback(const quint64 resumeId,
                                                                     const bool    cancelLuaCoroutine)
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::finishSuspendedPluginCallbackDispatchWithFallback");
	auto it = m_suspendedPluginCallbackDispatches.find(resumeId);
	if (it == m_suspendedPluginCallbackDispatches.end())
		return;

	if (cancelLuaCoroutine)
	{
		const SuspendedPluginCallbackDispatch &suspendedDispatch    = it.value();
		const int                              suspendedEngineIndex = suspendedDispatch.nextEngineIndex - 1;
		if (suspendedEngineIndex >= 0 &&
		    suspendedEngineIndex < suspendedDispatch.command.request.engines.size())
		{
			if (const QSharedPointer<LuaCallbackEngine> &engine =
			        suspendedDispatch.command.request.engines.at(suspendedEngineIndex);
			    engine)
			{
				LuaBatchDispatchRequest cancelRequest;
				cancelRequest.kind          = LuaBatchDispatchKind::CancelSuspendedModalString;
				cancelRequest.engines       = {engine};
				cancelRequest.modalResumeId = suspendedDispatch.engineModalResumeId;
				static_cast<void>(dispatchLuaBatch(cancelRequest));
			}
		}
	}

	SuspendedPluginCallbackDispatch suspended = std::move(it.value());
	m_suspendedPluginCallbackDispatches.erase(it);
	LuaBatchDispatchResult fallback = pluginCallbackDispatchFallback(suspended.command.request);
	finishPluginCallbackDispatchCommand(std::move(suspended.command), std::move(fallback));
}

void WorldRuntime::handleCompletedPluginCallbackDispatchCommand(PluginCallbackDispatchCommand &&command,
                                                                LuaBatchDispatchResult        &&result)
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::handleCompletedPluginCallbackDispatchCommand");
	applyLuaDeferredRuntimeMutationBatches(result);
	if (command.request.kind == LuaBatchDispatchKind::ResumeSuspendedModalString)
	{
		endPluginCallbackDispatchCommandGuard(command);
		handleModalResumeDispatchResult(command.request.runtimeModalResumeId, std::move(result));
		return;
	}
	if (result.suspended)
	{
		const int nextEngineIndex = result.suspendedEngineIndex + 1;
		storeSuspendedPluginCallbackDispatch(std::move(command), std::move(result), nextEngineIndex);
		return;
	}
	finishPluginCallbackDispatchCommand(std::move(command), std::move(result));
}

void WorldRuntime::handleModalResumeDispatchResult(const quint64 resumeId, LuaBatchDispatchResult &&result)
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::handleModalResumeDispatchResult");
	auto it = m_suspendedPluginCallbackDispatches.find(resumeId);
	if (it == m_suspendedPluginCallbackDispatches.end())
		return;
	SuspendedPluginCallbackDispatch suspended = std::move(it.value());
	m_suspendedPluginCallbackDispatches.erase(it);
	if (result.suspended)
	{
		LuaBatchDispatchResult stackedPartial       = std::move(suspended.partialResult);
		stackedPartial.suspended                    = true;
		stackedPartial.modalResumeId                = result.modalResumeId;
		stackedPartial.suspendedEngineIndex         = result.suspendedEngineIndex;
		stackedPartial.hasPendingModalStringRequest = result.hasPendingModalStringRequest;
		stackedPartial.pendingModalStringRequest    = std::move(result.pendingModalStringRequest);
		storeSuspendedPluginCallbackDispatch(std::move(suspended.command), std::move(stackedPartial),
		                                     suspended.nextEngineIndex);
		return;
	}
	continueSuspendedPluginCallbackDispatch(std::move(suspended), std::move(result));
}

void WorldRuntime::continueSuspendedPluginCallbackDispatch(SuspendedPluginCallbackDispatch &&suspended,
                                                           LuaBatchDispatchResult          &&resumeResult)
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::continueSuspendedPluginCallbackDispatch");
	PluginCallbackDispatchCommand command = std::move(suspended.command);
	if (!m_luaExecutor)
	{
		finishPluginCallbackDispatchCommand(std::move(command), std::move(resumeResult));
		return;
	}
	auto supportsContinuation = [](const LuaBatchDispatchKind kind) -> bool
	{
		switch (kind)
		{
		case LuaBatchDispatchKind::NoArgs:
		case LuaBatchDispatchKind::String:
		case LuaBatchDispatchKind::StringStopOnFalse:
		case LuaBatchDispatchKind::StringHandled:
		case LuaBatchDispatchKind::Bytes:
		case LuaBatchDispatchKind::BytesInOut:
		case LuaBatchDispatchKind::StringInOut:
		case LuaBatchDispatchKind::NumberAndString:
		case LuaBatchDispatchKind::NumberAndStringStopOnTrue:
		case LuaBatchDispatchKind::NumberAndStringStopOnFalse:
		case LuaBatchDispatchKind::TwoNumbersAndString:
		case LuaBatchDispatchKind::TwoNumbersAndStringStopOnFalse:
		case LuaBatchDispatchKind::NumberAndBytes:
		case LuaBatchDispatchKind::NumberAndBytesStopOnTrue:
		case LuaBatchDispatchKind::NumberAndUtf8StringsCount:
		case LuaBatchDispatchKind::StringsAndWildcards:
		case LuaBatchDispatchKind::ExecuteScript:
		case LuaBatchDispatchKind::ProcedureWithString:
		case LuaBatchDispatchKind::MxpError:
		case LuaBatchDispatchKind::MxpStartUp:
		case LuaBatchDispatchKind::MxpShutDown:
		case LuaBatchDispatchKind::MxpStartTag:
		case LuaBatchDispatchKind::MxpEndTag:
		case LuaBatchDispatchKind::MxpSetVariable:
			return true;
		default:
			return false;
		}
	};
	if (!supportsContinuation(command.request.kind))
	{
		finishPluginCallbackDispatchCommand(std::move(command), std::move(resumeResult));
		return;
	}

	auto isSingleRecipientContinuation = [](const LuaBatchDispatchKind kind) -> bool
	{
		switch (kind)
		{
		case LuaBatchDispatchKind::ExecuteScript:
		case LuaBatchDispatchKind::ProcedureWithString:
		case LuaBatchDispatchKind::MxpError:
		case LuaBatchDispatchKind::MxpStartTag:
			return true;
		default:
			return false;
		}
	};

	auto shouldStop = [](const LuaBatchDispatchKind kind, const LuaBatchDispatchResult &result) -> bool
	{
		if (kind == LuaBatchDispatchKind::StringHandled)
			return result.boolResultValid && result.boolResult;
		if (kind == LuaBatchDispatchKind::NumberAndStringStopOnTrue)
			return result.boolResultValid && result.boolResult;
		if (kind == LuaBatchDispatchKind::NumberAndStringStopOnFalse)
			return result.boolResultValid && !result.boolResult;
		if (kind == LuaBatchDispatchKind::TwoNumbersAndStringStopOnFalse)
			return result.boolResultValid && !result.boolResult;
		if (kind == LuaBatchDispatchKind::NumberAndBytesStopOnTrue)
			return result.boolResultValid && result.boolResult;
		return false;
	};

	auto mergeRecipientResult = [](const LuaBatchDispatchKind kind, LuaBatchDispatchResult &aggregate,
	                               const LuaBatchDispatchResult &recipient)
	{
		switch (kind)
		{
		case LuaBatchDispatchKind::NoArgs:
			aggregate.boolResult      = aggregate.boolResult && recipient.boolResult;
			aggregate.boolResultValid = true;
			aggregate.hasFunction =
			    aggregate.hasFunction || (recipient.hasFunctionValid && recipient.hasFunction);
			aggregate.hasFunctionValid = true;
			break;
		case LuaBatchDispatchKind::StringStopOnFalse:
		case LuaBatchDispatchKind::NumberAndStringStopOnFalse:
		case LuaBatchDispatchKind::TwoNumbersAndStringStopOnFalse:
			if (recipient.hasFunctionValid && recipient.hasFunction && recipient.boolResultValid &&
			    !recipient.boolResult)
				aggregate.boolResult = false;
			aggregate.boolResultValid = true;
			break;
		case LuaBatchDispatchKind::StringHandled:
			if (recipient.hasFunctionValid && recipient.hasFunction)
				aggregate.boolResult = true;
			aggregate.boolResultValid = true;
			break;
		case LuaBatchDispatchKind::NumberAndStringStopOnTrue:
		case LuaBatchDispatchKind::NumberAndBytesStopOnTrue:
			if (recipient.hasFunctionValid && recipient.hasFunction && recipient.boolResultValid &&
			    recipient.boolResult)
				aggregate.boolResult = true;
			aggregate.boolResultValid = true;
			break;
		case LuaBatchDispatchKind::NumberAndUtf8StringsCount:
			if (recipient.hasFunctionValid && recipient.hasFunction)
				++aggregate.countResult;
			aggregate.countResultValid = true;
			break;
		case LuaBatchDispatchKind::StringsAndWildcards:
			aggregate.hasFunction =
			    aggregate.hasFunction || (recipient.hasFunctionValid && recipient.hasFunction);
			aggregate.hasFunctionValid = true;
			break;
		case LuaBatchDispatchKind::NumberAndString:
		case LuaBatchDispatchKind::String:
		case LuaBatchDispatchKind::TwoNumbersAndString:
		case LuaBatchDispatchKind::NumberAndBytes:
		case LuaBatchDispatchKind::Bytes:
		case LuaBatchDispatchKind::ExecuteScript:
		case LuaBatchDispatchKind::MxpError:
		case LuaBatchDispatchKind::MxpStartTag:
		case LuaBatchDispatchKind::ProcedureWithString:
			if (recipient.boolResultValid)
			{
				aggregate.boolResult      = recipient.boolResult;
				aggregate.boolResultValid = true;
			}
			if (recipient.hasFunctionValid)
			{
				aggregate.hasFunction      = recipient.hasFunction;
				aggregate.hasFunctionValid = true;
			}
			break;
		case LuaBatchDispatchKind::BytesInOut:
			aggregate.bytesResult = recipient.bytesResult;
			break;
		case LuaBatchDispatchKind::StringInOut:
			aggregate.stringResult = recipient.stringResult;
			break;
		default:
			break;
		}
	};

	LuaBatchDispatchResult finalResult       = std::move(suspended.partialResult);
	finalResult.suspended                    = false;
	finalResult.modalResumeId                = 0;
	finalResult.suspendedEngineIndex         = -1;
	finalResult.hasPendingModalStringRequest = false;
	mergeRecipientResult(command.request.kind, finalResult, resumeResult);
	if (shouldStop(command.request.kind, finalResult))
	{
		finishPluginCallbackDispatchCommand(std::move(command), std::move(finalResult));
		return;
	}
	if (isSingleRecipientContinuation(command.request.kind))
	{
		finishPluginCallbackDispatchCommand(std::move(command), std::move(finalResult));
		return;
	}

	if (suspended.nextEngineIndex < command.request.engines.size())
		beginPluginCallbackDispatchCommandGuard(command);
	for (int engineIndex = suspended.nextEngineIndex; engineIndex < command.request.engines.size();
	     ++engineIndex)
	{
		const QSharedPointer<LuaCallbackEngine> engine = command.request.engines.at(engineIndex);
		if (!engine)
			continue;
		LuaBatchDispatchRequest request      = command.request;
		request.engines                      = {engine};
		request.revalidateObservedRecipients = false;
		if (command.request.kind == LuaBatchDispatchKind::BytesInOut)
			request.bytesArg = finalResult.bytesResult;
		else if (command.request.kind == LuaBatchDispatchKind::StringInOut)
			request.stringArg = finalResult.stringResult;
		LuaBatchDispatchResult result = m_luaExecutor->dispatchBatch(request);
		applyLuaDeferredRuntimeMutationBatches(result);
		if (result.suspended)
		{
			LuaBatchDispatchResult suspendedPartial       = std::move(finalResult);
			suspendedPartial.suspended                    = true;
			suspendedPartial.modalResumeId                = result.modalResumeId;
			suspendedPartial.suspendedEngineIndex         = result.suspendedEngineIndex;
			suspendedPartial.hasPendingModalStringRequest = result.hasPendingModalStringRequest;
			suspendedPartial.pendingModalStringRequest    = std::move(result.pendingModalStringRequest);
			storeSuspendedPluginCallbackDispatch(std::move(command), std::move(suspendedPartial),
			                                     engineIndex + 1);
			return;
		}
		mergeRecipientResult(command.request.kind, finalResult, result);
		if (shouldStop(command.request.kind, finalResult))
		{
			finishPluginCallbackDispatchCommand(std::move(command), std::move(finalResult));
			return;
		}
		if (command.request.kind == LuaBatchDispatchKind::NumberAndString)
			finalResult = std::move(result);
	}
	finishPluginCallbackDispatchCommand(std::move(command), std::move(finalResult));
}

LuaBatchDispatchResult WorldRuntime::queuePluginCallbackDispatch(const LuaBatchDispatchRequest &request,
                                                                 const bool completionBarrier)
{
	LuaBatchDispatchRequest callbackRequest = request;
	callbackRequest.lane                    = LuaBatchDispatchLane::Callback;
	LuaBatchDispatchResult fallback         = pluginCallbackDispatchFallback(callbackRequest);
#ifndef NDEBUG
	const qint64 dispatchStartNs = pluginCallbackDispatchNowNs();
#endif

	if (QThread::currentThread() != thread())
	{
		return qmudInvokeMethodOr(
		    this, fallback, [this, callbackRequest, completionBarrier]
		    { return queuePluginCallbackDispatch(callbackRequest, completionBarrier); });
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::queuePluginCallbackDispatch");
	if (m_pluginCallbackDispatchShuttingDown)
		return fallback;

	const bool missingFunctionName =
	    callbackRequest.kind != LuaBatchDispatchKind::ExecuteScript &&
	    callbackRequest.kind != LuaBatchDispatchKind::CancelSuspendedModalString &&
	    callbackRequest.kind != LuaBatchDispatchKind::ResumeSuspendedModalString &&
	    callbackRequest.functionName.isEmpty();
	if (missingFunctionName || callbackRequest.engines.isEmpty())
		return fallback;
	if (!m_luaExecutor)
		return fallback;
	applyCurrentActionSourceOverride(callbackRequest);
	if (!callbackRequest.miniWindowSnapshotArg)
		callbackRequest.miniWindowSnapshotArg = captureLuaCallbackSnapshotForDispatch(
		    callbackRequest.engines, callbackRequest.lineSnapshotPolicy);
	revalidateObservedCallbackRecipients(callbackRequest);
	if (callbackRequest.engines.isEmpty())
		return fallback;
#ifndef NDEBUG
	const bool mmStartupDiag              = qmudMmStartupDiagShouldLogRequest(callbackRequest);
	const bool dispatchDiagnosticsEnabled = m_traceEnabled;
#endif
	const bool inputCritical         = pluginCallbackDispatchIsInputCritical(callbackRequest);
	callbackRequest.inputCritical    = inputCritical;
	const bool mustWaitForCompletion = inputCritical;
	const bool canRetainQueuedResult =
	    completionBarrier && (mustWaitForCompletion || (!m_pluginCallbackDispatchWorkerInFlight &&
	                                                    m_pluginCallbackDispatchQueue.isEmpty()));

	if (completionBarrier && !m_pluginCallbackDispatchActive && !m_pluginCallbackDispatchWorkerInFlight &&
	    m_pluginCallbackDispatchQueue.isEmpty() && !m_pluginCallbackDispatchDrainQueued)
	{
		PluginCallbackDispatchCommand directCommand;
		directCommand.id              = m_nextPluginCallbackDispatchId++;
		directCommand.request         = callbackRequest;
		directCommand.retainResult    = true;
		const quint64 directCommandId = directCommand.id;
		beginPluginCallbackDispatchCommandGuard(directCommand);
#ifndef NDEBUG
		if (mmStartupDiag)
		{
			qInfo().noquote()
			    << QStringLiteral(
			           "[QMud][MMStartupDiag] dispatch-direct-sync-start callback=%1 targets=%2 kind=%3 "
			           "completionBarrier=%4 inputCritical=%5")
			           .arg(callbackRequest.functionName,
			                qmudMmStartupDiagEngineLabels(callbackRequest.engines),
			                pluginCallbackDispatchKindLabel(callbackRequest.kind),
			                completionBarrier ? QStringLiteral("1") : QStringLiteral("0"),
			                inputCritical ? QStringLiteral("1") : QStringLiteral("0"));
		}
#endif
#ifndef NDEBUG
		const qint64 executeStartNs = pluginCallbackDispatchNowNs();
#endif
		LuaBatchDispatchResult directResult = m_luaExecutor->dispatchBatch(directCommand.request);
		applyLuaDeferredRuntimeMutationBatches(directResult);
		if (directResult.suspended)
		{
			LuaBatchDispatchResult suspendedResult = directResult;
			storeSuspendedPluginCallbackDispatch(std::move(directCommand), std::move(suspendedResult),
			                                     directResult.suspendedEngineIndex + 1);
#ifndef NDEBUG
			bool          timeoutLogged = false;
			QElapsedTimer waitTimer;
			waitTimer.start();
			const qint64 timeoutMs = qMax<qint64>(1, qmudLuaBridgeInvokeTimeoutMs());
#endif
			while (!m_pluginCallbackDispatchResults.contains(directCommandId))
			{
				drainPluginCallbackDispatchQueue(directCommandId);
				if (m_pluginCallbackDispatchResults.contains(directCommandId))
					break;

				QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
				if (m_pluginCallbackDispatchResults.contains(directCommandId))
					break;

				if (qmudLuaBridgePumpCurrentThreadOnce())
					continue;

				if (!m_pluginCallbackDispatchWorkerInFlight && m_pluginCallbackDispatchQueue.isEmpty() &&
				    !m_pluginCallbackDispatchActive &&
				    !hasSuspendedPluginCallbackDispatchCommand(directCommandId))
					break;

#ifndef NDEBUG
				if (!timeoutLogged && waitTimer.elapsed() >= timeoutMs)
				{
					timeoutLogged = true;
					qWarning().noquote()
					    << QStringLiteral("[QMud][LuaDispatchDiag] retained barrier waiting for suspended "
					                      "callback work for %1 ms; id=%2 callback=%3 kind=%4")
					           .arg(waitTimer.elapsed())
					           .arg(directCommandId)
					           .arg(callbackRequest.functionName,
					                pluginCallbackDispatchKindLabel(callbackRequest.kind));
				}
#endif

				static_cast<void>(qmudLuaBridgeWaitForCurrentThreadWake(10));
			}
			auto it = m_pluginCallbackDispatchResults.find(directCommandId);
			if (it != m_pluginCallbackDispatchResults.end())
			{
				LuaBatchDispatchResult result = std::move(it.value());
				m_pluginCallbackDispatchResults.erase(it);
				applyLuaDeferredRuntimeMutationBatches(result);
				return result;
			}
		}
		else
			endPluginCallbackDispatchCommandGuard(directCommand);
#ifndef NDEBUG
		const qint64 executeMs = pluginCallbackDispatchElapsedMs(executeStartNs);
		if (mmStartupDiag)
		{
			qInfo().noquote()
			    << QStringLiteral(
			           "[QMud][MMStartupDiag] dispatch-direct-sync-done callback=%1 targets=%2 execMs=%3 %4")
			           .arg(callbackRequest.functionName,
			                qmudMmStartupDiagEngineLabels(callbackRequest.engines))
			           .arg(executeMs)
			           .arg(qmudMmStartupDiagResultLabel(directResult));
		}
		if (pluginCallbackDispatchShouldLog(dispatchDiagnosticsEnabled, callbackRequest.functionName,
		                                    executeMs))
		{
			qInfo().noquote()
			    << QStringLiteral(
			           "[QMud][LuaDispatchDiag] dispatch-direct-sync callback=%1 kind=%2 exec_ms=%3")
			           .arg(callbackRequest.functionName,
			                pluginCallbackDispatchKindLabel(callbackRequest.kind))
			           .arg(executeMs);
		}
#endif
		return directResult;
	}

	PluginCallbackDispatchCommand command;
	command.id           = m_nextPluginCallbackDispatchId++;
	command.request      = callbackRequest;
	command.retainResult = canRetainQueuedResult;
#ifndef NDEBUG
	command.enqueuedAtNs        = dispatchStartNs;
	command.queueDepthAtEnqueue = safeQSizeToInt(m_pluginCallbackDispatchQueue.size()) + 1;
#endif
	if (inputCritical)
	{
		auto insertIt = m_pluginCallbackDispatchQueue.begin();
		while (insertIt != m_pluginCallbackDispatchQueue.end() &&
		       pluginCallbackLaneCommandBlocksInputCriticalInsertion(*insertIt))
			++insertIt;
		PluginCallbackLaneCommand laneCommand;
		laneCommand.kind     = PluginCallbackLaneCommand::Kind::CallbackDispatch;
		laneCommand.callback = command;
		m_pluginCallbackDispatchQueue.insert(insertIt, laneCommand);
	}
	else
	{
		PluginCallbackLaneCommand laneCommand;
		laneCommand.kind     = PluginCallbackLaneCommand::Kind::CallbackDispatch;
		laneCommand.callback = command;
		m_pluginCallbackDispatchQueue.enqueue(laneCommand);
	}
#ifndef NDEBUG
	if (mmStartupDiag)
	{
		qInfo().noquote()
		    << QStringLiteral(
		           "[QMud][MMStartupDiag] dispatch-enqueue-sync id=%1 callback=%2 targets=%3 kind=%4 "
		           "retain=%5 completionBarrier=%6 inputCritical=%7 lowPriority=%8 "
		           "queueDepthAtEnqueue=%9 workerInFlight=%10 active=%11 drainQueued=%12")
		           .arg(command.id)
		           .arg(callbackRequest.functionName, qmudMmStartupDiagEngineLabels(callbackRequest.engines),
		                pluginCallbackDispatchKindLabel(callbackRequest.kind),
		                command.retainResult ? QStringLiteral("1") : QStringLiteral("0"),
		                completionBarrier ? QStringLiteral("1") : QStringLiteral("0"),
		                inputCritical ? QStringLiteral("1") : QStringLiteral("0"),
		                callbackRequest.lowPriority ? QStringLiteral("1") : QStringLiteral("0"))
		           .arg(command.queueDepthAtEnqueue)
		           .arg(m_pluginCallbackDispatchWorkerInFlight ? QStringLiteral("1") : QStringLiteral("0"),
		                m_pluginCallbackDispatchActive ? QStringLiteral("1") : QStringLiteral("0"),
		                m_pluginCallbackDispatchDrainQueued ? QStringLiteral("1") : QStringLiteral("0"));
	}
#endif

	if (command.retainResult)
	{
#ifndef NDEBUG
		bool          timeoutLogged = false;
		QElapsedTimer waitTimer;
		waitTimer.start();
		const qint64 timeoutMs = qMax<qint64>(1, qmudLuaBridgeInvokeTimeoutMs());
#endif
		while (!m_pluginCallbackDispatchResults.contains(command.id))
		{
			drainPluginCallbackDispatchQueue(command.id);
			if (m_pluginCallbackDispatchResults.contains(command.id))
				break;

			if (!mustWaitForCompletion && !hasSuspendedPluginCallbackDispatchCommand(command.id))
				break;

			QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
			if (m_pluginCallbackDispatchResults.contains(command.id))
				break;

			if (qmudLuaBridgePumpCurrentThreadOnce())
				continue;

			if (!m_pluginCallbackDispatchWorkerInFlight && m_pluginCallbackDispatchQueue.isEmpty() &&
			    !m_pluginCallbackDispatchActive && !hasSuspendedPluginCallbackDispatchCommand(command.id))
				break;

#ifndef NDEBUG
			if (!timeoutLogged && waitTimer.elapsed() >= timeoutMs)
			{
				timeoutLogged = true;
				qWarning().noquote()
				    << QStringLiteral("[QMud][LuaDispatchDiag] retained barrier queued behind callback work "
				                      "for %1 ms; waiting for completion id=%2 callback=%3 kind=%4")
				           .arg(waitTimer.elapsed())
				           .arg(command.id)
				           .arg(callbackRequest.functionName,
				                pluginCallbackDispatchKindLabel(callbackRequest.kind));
			}
#endif

			static_cast<void>(qmudLuaBridgeWaitForCurrentThreadWake(10));
		}
		auto it = m_pluginCallbackDispatchResults.find(command.id);
		if (it == m_pluginCallbackDispatchResults.end())
		{
#ifndef NDEBUG
			const qint64 totalWaitMs = pluginCallbackDispatchElapsedMs(dispatchStartNs);
			if (mmStartupDiag)
			{
				qInfo().noquote()
				    << QStringLiteral(
				           "[QMud][MMStartupDiag] dispatch-barrier-miss id=%1 callback=%2 targets=%3 "
				           "waitMs=%4 queueAtEnqueue=%5 active=%6 pendingQueue=%7")
				           .arg(command.id)
				           .arg(callbackRequest.functionName,
				                qmudMmStartupDiagEngineLabels(callbackRequest.engines))
				           .arg(totalWaitMs)
				           .arg(command.queueDepthAtEnqueue)
				           .arg(m_pluginCallbackDispatchActive ? QStringLiteral("1") : QStringLiteral("0"))
				           .arg(m_pluginCallbackDispatchQueue.size());
			}
			if (pluginCallbackDispatchShouldLog(dispatchDiagnosticsEnabled, callbackRequest.functionName,
			                                    totalWaitMs))
			{
				qInfo().noquote() << QStringLiteral(
				                         "[QMud][LuaDispatchDiag] barrier-miss id=%1 callback=%2 kind=%3 "
				                         "wait_ms=%4 queue_at_enqueue=%5 active=%6 pending_queue=%7")
				                         .arg(command.id)
				                         .arg(callbackRequest.functionName,
				                              pluginCallbackDispatchKindLabel(callbackRequest.kind))
				                         .arg(totalWaitMs)
				                         .arg(command.queueDepthAtEnqueue)
				                         .arg(m_pluginCallbackDispatchActive ? QStringLiteral("1")
				                                                             : QStringLiteral("0"))
				                         .arg(m_pluginCallbackDispatchQueue.size());
			}
#endif
			return fallback;
		}
		LuaBatchDispatchResult result = std::move(it.value());
		m_pluginCallbackDispatchResults.erase(it);
		applyLuaDeferredRuntimeMutationBatches(result);
#ifndef NDEBUG
		const qint64 totalWaitMs = pluginCallbackDispatchElapsedMs(dispatchStartNs);
		if (mmStartupDiag)
		{
			qInfo().noquote() << QStringLiteral("[QMud][MMStartupDiag] dispatch-barrier-done id=%1 "
			                                    "callback=%2 targets=%3 waitMs=%4 %5")
			                         .arg(command.id)
			                         .arg(callbackRequest.functionName,
			                              qmudMmStartupDiagEngineLabels(callbackRequest.engines))
			                         .arg(totalWaitMs)
			                         .arg(qmudMmStartupDiagResultLabel(result));
		}
		if (pluginCallbackDispatchShouldLog(dispatchDiagnosticsEnabled, callbackRequest.functionName,
		                                    totalWaitMs))
		{
			qInfo().noquote()
			    << QStringLiteral("[QMud][LuaDispatchDiag] barrier-done id=%1 callback=%2 kind=%3 wait_ms=%4 "
			                      "queue_at_enqueue=%5 pending_queue=%6")
			           .arg(command.id)
			           .arg(callbackRequest.functionName,
			                pluginCallbackDispatchKindLabel(callbackRequest.kind))
			           .arg(totalWaitMs)
			           .arg(command.queueDepthAtEnqueue)
			           .arg(m_pluginCallbackDispatchQueue.size());
		}
#endif
		return result;
	}

	queuePluginCallbackDispatchDrain();
	return fallback;
}

void WorldRuntime::queuePluginCallbackDispatchAsync(
    const LuaBatchDispatchRequest &request, std::function<void(const LuaBatchDispatchResult &)> completion)
{
	if (QThread::currentThread() != thread())
	{
		const bool queued =
		    qmudInvokeMethodQueued(this, [this, request, completion = std::move(completion)]() mutable
		                           { queuePluginCallbackDispatchAsync(request, std::move(completion)); });
		if (!queued)
		{
			qWarning().noquote() << QStringLiteral(
			    "[QMud][LuaBridge] failed to queue plugin callback dispatch to runtime "
			    "thread");
		}
		return;
	}

	static_cast<void>(tryQueuePluginCallbackDispatchAsyncOnRuntimeThread(request, std::move(completion)));
}

bool WorldRuntime::tryQueuePluginCallbackDispatchAsyncOnRuntimeThread(
    const LuaBatchDispatchRequest &request, std::function<void(const LuaBatchDispatchResult &)> completion)
{
	LuaBatchDispatchRequest callbackRequest = request;
	callbackRequest.lane                    = LuaBatchDispatchLane::Callback;
	LuaBatchDispatchResult fallback         = pluginCallbackDispatchFallback(callbackRequest);
#ifndef NDEBUG
	const qint64 dispatchStartNs = pluginCallbackDispatchNowNs();
#endif

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::tryQueuePluginCallbackDispatchAsyncOnRuntimeThread");
	if (m_pluginCallbackDispatchShuttingDown)
	{
		if (completion)
			completion(fallback);
		return false;
	}

	const bool missingFunctionName =
	    callbackRequest.kind != LuaBatchDispatchKind::ExecuteScript &&
	    callbackRequest.kind != LuaBatchDispatchKind::CancelSuspendedModalString &&
	    callbackRequest.kind != LuaBatchDispatchKind::ResumeSuspendedModalString &&
	    callbackRequest.functionName.isEmpty();
	if (missingFunctionName || callbackRequest.engines.isEmpty())
	{
		if (completion)
			completion(fallback);
		return false;
	}
	if (!m_luaExecutor)
	{
		if (completion)
			completion(fallback);
		return false;
	}
	applyCurrentActionSourceOverride(callbackRequest);
	if (!callbackRequest.miniWindowSnapshotArg)
		callbackRequest.miniWindowSnapshotArg = captureLuaCallbackSnapshotForDispatch(
		    callbackRequest.engines, callbackRequest.lineSnapshotPolicy);
	revalidateObservedCallbackRecipients(callbackRequest);
	if (callbackRequest.engines.isEmpty())
	{
		if (completion)
			completion(fallback);
		return false;
	}
	const bool inputCritical = pluginCallbackDispatchIsInputCritical(callbackRequest);
#ifndef NDEBUG
	const bool mmStartupDiag = qmudMmStartupDiagShouldLogRequest(callbackRequest);
#endif
	callbackRequest.inputCritical = inputCritical;
	callbackRequest.lowPriority   = !inputCritical;
	PluginCallbackDispatchCommand command;
	command.id           = m_nextPluginCallbackDispatchId++;
	command.request      = callbackRequest;
	command.retainResult = false;
	command.completion   = std::move(completion);
#ifndef NDEBUG
	command.enqueuedAtNs        = dispatchStartNs;
	command.queueDepthAtEnqueue = safeQSizeToInt(m_pluginCallbackDispatchQueue.size()) + 1;
#endif
	if (inputCritical)
	{
		auto insertIt = m_pluginCallbackDispatchQueue.begin();
		while (insertIt != m_pluginCallbackDispatchQueue.end() &&
		       pluginCallbackLaneCommandBlocksInputCriticalInsertion(*insertIt))
			++insertIt;
		PluginCallbackLaneCommand laneCommand;
		laneCommand.kind     = PluginCallbackLaneCommand::Kind::CallbackDispatch;
		laneCommand.callback = std::move(command);
		m_pluginCallbackDispatchQueue.insert(insertIt, std::move(laneCommand));
	}
	else
	{
		PluginCallbackLaneCommand laneCommand;
		laneCommand.kind     = PluginCallbackLaneCommand::Kind::CallbackDispatch;
		laneCommand.callback = std::move(command);
		m_pluginCallbackDispatchQueue.enqueue(std::move(laneCommand));
	}
#ifndef NDEBUG
	if (mmStartupDiag)
	{
		qInfo().noquote()
		    << QStringLiteral(
		           "[QMud][MMStartupDiag] dispatch-enqueue-async id=%1 callback=%2 targets=%3 kind=%4 "
		           "inputCritical=%5 lowPriority=%6 queueDepthAtEnqueue=%7 "
		           "workerInFlight=%8 active=%9 drainQueued=%10")
		           .arg(m_nextPluginCallbackDispatchId - 1)
		           .arg(callbackRequest.functionName, qmudMmStartupDiagEngineLabels(callbackRequest.engines),
		                pluginCallbackDispatchKindLabel(callbackRequest.kind),
		                inputCritical ? QStringLiteral("1") : QStringLiteral("0"),
		                callbackRequest.lowPriority ? QStringLiteral("1") : QStringLiteral("0"))
		           .arg(safeQSizeToInt(m_pluginCallbackDispatchQueue.size()))
		           .arg(m_pluginCallbackDispatchWorkerInFlight ? QStringLiteral("1") : QStringLiteral("0"),
		                m_pluginCallbackDispatchActive ? QStringLiteral("1") : QStringLiteral("0"),
		                m_pluginCallbackDispatchDrainQueued ? QStringLiteral("1") : QStringLiteral("0"));
	}
#endif
	queuePluginCallbackDispatchDrain();
	return true;
}

void WorldRuntime::applyCurrentActionSourceOverride(LuaBatchDispatchRequest &request) const
{
	if (request.hasActionSourceOverride)
		return;
	const unsigned short actionSource = currentActionSource();
	if (actionSource == eUnknownActionSource)
		return;
	request.hasActionSourceOverride = true;
	request.actionSourceOverride    = actionSource;
}

LuaBatchDispatchResult WorldRuntime::dispatchLuaBatch(const LuaBatchDispatchRequest &request) const
{
	if (!m_luaExecutor)
		return {};
	LuaBatchDispatchResult result = m_luaExecutor->dispatchBatch(request);
	applyLuaDeferredRuntimeMutationBatches(result);
	return result;
}

void WorldRuntime::dispatchLuaBatchAsync(const LuaBatchDispatchRequest &request) const
{
	if (!m_luaExecutor)
		return;
	m_luaExecutor->dispatchBatchAsync(request, const_cast<WorldRuntime *>(this),
	                                  [](const LuaBatchDispatchResult &result)
	                                  {
		                                  LuaBatchDispatchResult appliedResult = result;
		                                  applyLuaDeferredRuntimeMutationBatches(appliedResult);
	                                  });
}

void WorldRuntime::dispatchInitializeLuaEnginesWithObservedCallbacks(
    const QVector<LuaEngineObservedInitializationRequest> &initRequests, const bool completionBarrier) const
{
	if (initRequests.isEmpty())
		return;
	LuaBatchDispatchRequest request;
	request.kind = LuaBatchDispatchKind::InitializeEnginesWithObservedCallbacksMany;
	request.lane = LuaBatchDispatchLane::Callback;
	request.initRequestsArg =
	    QSharedPointer<QVector<LuaEngineObservedInitializationRequest>>::create(initRequests);
	if (completionBarrier)
		static_cast<void>(dispatchLuaBatch(request));
	else
		dispatchLuaBatchAsync(request);
}

void WorldRuntime::dispatchTeardownLuaEngines(const QVector<QSharedPointer<LuaCallbackEngine>> &engines,
                                              const bool completionBarrier) const
{
	if (engines.isEmpty())
		return;
	LuaBatchDispatchRequest request;
	request.kind    = LuaBatchDispatchKind::TeardownEnginesMany;
	request.lane    = LuaBatchDispatchLane::Callback;
	request.engines = engines;
	if (completionBarrier)
		static_cast<void>(dispatchLuaBatch(request));
	else
		dispatchLuaBatchAsync(request);
}

void WorldRuntime::dispatchApplyPackageRestrictions(const QVector<QSharedPointer<LuaCallbackEngine>> &engines,
                                                    const bool enablePackage,
                                                    const bool completionBarrier) const
{
	if (engines.isEmpty())
		return;
	LuaBatchDispatchRequest request;
	request.kind       = LuaBatchDispatchKind::ApplyPackageRestrictionsMany;
	request.lane       = LuaBatchDispatchLane::Callback;
	request.engines    = engines;
	request.optionFlag = enablePackage;
	if (completionBarrier)
		static_cast<void>(dispatchLuaBatch(request));
	else
		dispatchLuaBatchAsync(request);
}

void WorldRuntime::dispatchSingleEngineNoArgCallback(const QSharedPointer<LuaCallbackEngine> &engine,
                                                     const QString                           &functionName,
                                                     const bool completionBarrier, const bool defaultResult)
{
	if (!engine || functionName.isEmpty())
		return;
	LuaBatchDispatchRequest request;
	request.kind          = LuaBatchDispatchKind::NoArgs;
	request.engines       = {engine};
	request.functionName  = functionName;
	request.defaultResult = defaultResult;
	static_cast<void>(queuePluginCallbackDispatch(request, completionBarrier));
}

void WorldRuntime::dispatchWorldNoArgCallbackByAttribute(const QString &attributeName,
                                                         const bool     completionBarrier)
{
	if (attributeName.isEmpty() || !isLuaScriptingEnabled(m_worldAttributes) || !m_luaCallbacks)
		return;
	const QString callbackName = m_worldAttributes.value(attributeName).trimmed();
	if (callbackName.isEmpty())
		return;
	dispatchSingleEngineNoArgCallback(makeNonOwningLuaEngineRef(m_luaCallbacks), callbackName,
	                                  completionBarrier);
}

void WorldRuntime::enqueueActiveStateTransition(const bool active)
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::enqueueActiveStateTransition");
	if (m_pluginCallbackDispatchShuttingDown)
		return;
	if (m_pendingActiveStateTransitionCount == 0 && m_active == active)
	{
		if (active)
		{
			clearNewLines();
			invalidateLuaCallbackDispatchSnapshot();
		}
		return;
	}
	if (m_pendingActiveStateTransitionCount > 0 && m_lastPendingActiveState == active)
		return;

	m_active = active;
	if (active)
		clearNewLines();
	invalidateLuaCallbackDispatchSnapshot();

	ActiveStateTransitionCommand command;
	command.id     = m_nextPluginCallbackDispatchId++;
	command.active = active;
#ifndef NDEBUG
	command.enqueuedAtNs        = pluginCallbackDispatchNowNs();
	command.queueDepthAtEnqueue = safeQSizeToInt(m_pluginCallbackDispatchQueue.size()) + 1;
#endif

	PluginCallbackLaneCommand laneCommand;
	laneCommand.kind        = PluginCallbackLaneCommand::Kind::ActiveStateTransition;
	laneCommand.activeState = command;
	auto insertIt           = m_pluginCallbackDispatchQueue.begin();
	while (insertIt != m_pluginCallbackDispatchQueue.end() &&
	       pluginCallbackLaneCommandBlocksInputCriticalInsertion(*insertIt))
		++insertIt;
	m_pluginCallbackDispatchQueue.insert(insertIt, laneCommand);
	++m_pendingActiveStateTransitionCount;
	m_lastPendingActiveState = active;
	queuePluginCallbackDispatchDrain();
}

bool WorldRuntime::buildActiveStateNoArgCallbackCommand(
    const QVector<QSharedPointer<LuaCallbackEngine>> &engines, const QString &functionName,
    const bool revalidateObservedRecipients, PluginCallbackDispatchCommand &command)
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::buildActiveStateNoArgCallbackCommand");
	if (!m_luaExecutor || functionName.isEmpty() || engines.isEmpty())
		return false;

	LuaBatchDispatchRequest request;
	request.kind                         = LuaBatchDispatchKind::NoArgs;
	request.lane                         = LuaBatchDispatchLane::Callback;
	request.engines                      = engines;
	request.functionName                 = functionName;
	request.defaultResult                = true;
	request.revalidateObservedRecipients = revalidateObservedRecipients;
	request.hasActionSourceOverride      = true;
	request.actionSourceOverride         = eWorldAction;
	if (revalidateObservedRecipients)
		revalidateObservedCallbackRecipients(request);
	if (request.engines.isEmpty())
		return false;
	request.miniWindowSnapshotArg = captureLuaCallbackSnapshotForDispatchWithActionSource(
	    request.engines, request.lineSnapshotPolicy, request.actionSourceOverride);

	command.id           = m_nextPluginCallbackDispatchId++;
	command.request      = std::move(request);
	command.retainResult = false;
#ifndef NDEBUG
	command.enqueuedAtNs        = pluginCallbackDispatchNowNs();
	command.queueDepthAtEnqueue = safeQSizeToInt(m_pluginCallbackDispatchQueue.size()) + 1;
#endif
	return true;
}

void WorldRuntime::processActiveStateTransitionCommand(const ActiveStateTransitionCommand &command)
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::processActiveStateTransitionCommand");
	if (m_pendingActiveStateTransitionCount > 0)
		--m_pendingActiveStateTransitionCount;

	const QString worldCallbackName =
	    command.active ? m_worldAttributes.value(QStringLiteral("on_world_get_focus")).trimmed()
	                   : m_worldAttributes.value(QStringLiteral("on_world_lose_focus")).trimmed();
	const QString pluginCallbackName =
	    command.active ? QStringLiteral("OnPluginGetFocus") : QStringLiteral("OnPluginLoseFocus");
	const bool currentActive = m_active;
	m_active                 = command.active;
	invalidateLuaCallbackDispatchSnapshot();
	const auto restoreActive = qScopeGuard(
	    [this, currentActive]
	    {
		    m_active = currentActive;
		    invalidateLuaCallbackDispatchSnapshot();
	    });

	PluginCallbackDispatchCommand worldCommand;
	const bool                    hasWorldCommand =
	    isLuaScriptingEnabled(m_worldAttributes) && m_luaCallbacks &&
	    buildActiveStateNoArgCallbackCommand({makeNonOwningLuaEngineRef(m_luaCallbacks)}, worldCallbackName,
	                                         false, worldCommand);
	PluginCallbackDispatchCommand pluginCommand;
	const bool                    hasPluginCommand = buildActiveStateNoArgCallbackCommand(
	    collectPluginCallbackRecipients(pluginCallbackName), pluginCallbackName, true, pluginCommand);

	if (hasPluginCommand)
	{
		PluginCallbackLaneCommand laneCommand;
		laneCommand.kind     = PluginCallbackLaneCommand::Kind::CallbackDispatch;
		laneCommand.callback = std::move(pluginCommand);
		m_pluginCallbackDispatchQueue.insert(m_pluginCallbackDispatchQueue.begin(), std::move(laneCommand));
	}
	if (hasWorldCommand)
	{
		PluginCallbackLaneCommand laneCommand;
		laneCommand.kind     = PluginCallbackLaneCommand::Kind::CallbackDispatch;
		laneCommand.callback = std::move(worldCommand);
		m_pluginCallbackDispatchQueue.insert(m_pluginCallbackDispatchQueue.begin(), std::move(laneCommand));
	}
}

void WorldRuntime::drainPluginCallbackDispatchQueue(const quint64 completionCommandId)
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::drainPluginCallbackDispatchQueue");

	if (m_pluginCallbackDispatchActive)
	{
		if (completionCommandId == 0)
		{
			if (!m_pluginCallbackDispatchQueue.isEmpty())
				queuePluginCallbackDispatchDrain();
			return;
		}
		while (!m_pluginCallbackDispatchResults.contains(completionCommandId))
		{
			if (m_pluginCallbackDispatchQueue.isEmpty())
				break;
			processNextPluginCallbackDispatchCommand();
		}
		return;
	}
	if (m_pluginCallbackDispatchWorkerInFlight)
		return;

	m_pluginCallbackDispatchActive = true;
	const auto    resetQueueActive = qScopeGuard([this] { m_pluginCallbackDispatchActive = false; });
	constexpr int nonBarrierBurst  = 32;
	int           processed        = 0;
	while (!m_pluginCallbackDispatchQueue.isEmpty())
	{
		processNextPluginCallbackDispatchCommand();
		++processed;
		if (m_pluginCallbackDispatchWorkerInFlight)
			break;
		if (completionCommandId != 0 && m_pluginCallbackDispatchResults.contains(completionCommandId))
			break;
		if (completionCommandId == 0 && processed >= nonBarrierBurst)
			break;
	}
	if (!m_pluginCallbackDispatchQueue.isEmpty())
		queuePluginCallbackDispatchDrain();
}

void WorldRuntime::queuePluginCallbackDispatchDrain()
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::queuePluginCallbackDispatchDrain");
	if (m_pluginCallbackDispatchDrainQueued)
		return;

	m_pluginCallbackDispatchDrainQueued = true;
	const bool queued                   = QMetaObject::invokeMethod(
	    this,
	    [this]
	    {
		    qmudAssertObjectThreadAffinity(this, "WorldRuntime::queuePluginCallbackDispatchDrain::drain");
		    m_pluginCallbackDispatchDrainQueued = false;
		    if (!m_pluginCallbackDispatchQueue.isEmpty())
			    drainPluginCallbackDispatchQueue();
	    },
	    Qt::QueuedConnection);
	if (queued)
		return;

	m_pluginCallbackDispatchDrainQueued = false;
	if (!m_pluginCallbackDispatchActive)
		drainPluginCallbackDispatchQueue();
}

bool WorldRuntime::pluginCallbackLaneCommandBlocksInputCriticalInsertion(
    const PluginCallbackLaneCommand &command)
{
	if (command.kind == PluginCallbackLaneCommand::Kind::ActiveStateTransition)
		return true;
	return !command.callback.request.lowPriority;
}

void WorldRuntime::processNextPluginCallbackDispatchCommand()
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::processNextPluginCallbackDispatchCommand");
	if (m_pluginCallbackDispatchQueue.isEmpty())
		return;
	if (m_pluginCallbackDispatchWorkerInFlight)
		return;
	PluginCallbackLaneCommand command = m_pluginCallbackDispatchQueue.dequeue();
	if (command.kind == PluginCallbackLaneCommand::Kind::ActiveStateTransition)
	{
		processActiveStateTransitionCommand(command.activeState);
		return;
	}
	processPluginCallbackDispatchCommand(std::move(command.callback));
}

void WorldRuntime::processPluginCallbackDispatchCommand(PluginCallbackDispatchCommand &&command)
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::processPluginCallbackDispatchCommand");
	revalidateObservedCallbackRecipients(command.request);
#ifndef NDEBUG
	const bool mmStartupDiag = qmudMmStartupDiagShouldLogRequest(command.request);
#endif
	if (command.request.engines.isEmpty())
	{
		const LuaBatchDispatchResult fallback = pluginCallbackDispatchFallback(command.request);
#ifndef NDEBUG
		if (mmStartupDiag)
		{
			qInfo().noquote()
			    << QStringLiteral(
			           "[QMud][MMStartupDiag] dispatch-dequeue-empty id=%1 callback=%2 queueRemaining=%3 %4")
			           .arg(command.id)
			           .arg(command.request.functionName)
			           .arg(m_pluginCallbackDispatchQueue.size())
			           .arg(qmudMmStartupDiagResultLabel(fallback));
		}
#endif
		finishPluginCallbackDispatchCommand(std::move(command), LuaBatchDispatchResult(fallback));
		return;
	}
	if (!command.request.miniWindowSnapshotArg)
		command.request.miniWindowSnapshotArg = captureLuaCallbackSnapshotForDispatch(
		    command.request.engines, command.request.lineSnapshotPolicy);
#ifndef NDEBUG
	if (mmStartupDiag)
	{
		qInfo().noquote()
		    << QStringLiteral(
		           "[QMud][MMStartupDiag] dispatch-dequeue id=%1 callback=%2 targets=%3 kind=%4 retain=%5 "
		           "hasCompletion=%6 queueRemaining=%7 workerInFlight=%8 active=%9")
		           .arg(command.id)
		           .arg(command.request.functionName, qmudMmStartupDiagEngineLabels(command.request.engines),
		                pluginCallbackDispatchKindLabel(command.request.kind),
		                command.retainResult ? QStringLiteral("1") : QStringLiteral("0"),
		                command.completion ? QStringLiteral("1") : QStringLiteral("0"))
		           .arg(m_pluginCallbackDispatchQueue.size())
		           .arg(m_pluginCallbackDispatchWorkerInFlight ? QStringLiteral("1") : QStringLiteral("0"),
		                m_pluginCallbackDispatchActive ? QStringLiteral("1") : QStringLiteral("0"));
	}
	const qint64 queueWaitMs                = pluginCallbackDispatchElapsedMs(command.enqueuedAtNs);
	const bool   dispatchDiagnosticsEnabled = m_traceEnabled;
#endif
	if (command.retainResult)
	{
#ifndef NDEBUG
		const quint64 diagCommandId           = command.id;
		const QString diagCallback            = command.request.functionName;
		const QString diagTargets             = qmudMmStartupDiagEngineLabels(command.request.engines);
		const QString diagKind                = pluginCallbackDispatchKindLabel(command.request.kind);
		const int     diagQueueDepthAtEnqueue = command.queueDepthAtEnqueue;
		const qint64  executeStartNs          = pluginCallbackDispatchNowNs();
#endif
		beginPluginCallbackDispatchCommandGuard(command);
		LuaBatchDispatchResult result = m_luaExecutor->dispatchBatch(command.request);
#ifndef NDEBUG
		const qint64                 executeMs  = pluginCallbackDispatchElapsedMs(executeStartNs);
		const LuaBatchDispatchResult diagResult = result;
#endif
		handleCompletedPluginCallbackDispatchCommand(std::move(command), std::move(result));
#ifndef NDEBUG
		if (mmStartupDiag)
		{
			qInfo().noquote()
			    << QStringLiteral(
			           "[QMud][MMStartupDiag] dispatch-sync-done id=%1 callback=%2 targets=%3 queueWaitMs=%4 "
			           "execMs=%5 %6")
			           .arg(diagCommandId)
			           .arg(diagCallback, diagTargets)
			           .arg(queueWaitMs)
			           .arg(executeMs)
			           .arg(qmudMmStartupDiagResultLabel(diagResult));
		}
		if (pluginCallbackDispatchShouldLog(dispatchDiagnosticsEnabled, diagCallback,
		                                    queueWaitMs + executeMs))
		{
			qInfo().noquote() << QStringLiteral(
			                         "[QMud][LuaDispatchDiag] dispatch-sync id=%1 callback=%2 kind=%3 "
			                         "queue_wait_ms=%4 exec_ms=%5 queue_at_enqueue=%6 queue_remaining=%7")
			                         .arg(diagCommandId)
			                         .arg(diagCallback, diagKind)
			                         .arg(queueWaitMs)
			                         .arg(executeMs)
			                         .arg(diagQueueDepthAtEnqueue)
			                         .arg(m_pluginCallbackDispatchQueue.size());
		}
#endif
		return;
	}
	if (command.completion)
	{
#ifndef NDEBUG
		if (pluginCallbackDispatchShouldLog(dispatchDiagnosticsEnabled, command.request.functionName,
		                                    queueWaitMs))
		{
			qInfo().noquote()
			    << QStringLiteral(
			           "[QMud][LuaDispatchDiag] dispatch-async-completion id=%1 callback=%2 kind=%3 "
			           "queue_wait_ms=%4 queue_at_enqueue=%5 queue_remaining=%6")
			           .arg(command.id)
			           .arg(command.request.functionName,
			                pluginCallbackDispatchKindLabel(command.request.kind))
			           .arg(queueWaitMs)
			           .arg(command.queueDepthAtEnqueue)
			           .arg(m_pluginCallbackDispatchQueue.size());
		}
#endif
		m_pluginCallbackDispatchWorkerInFlight = true;
#ifndef NDEBUG
		const bool    diagShouldLog = mmStartupDiag;
		const quint64 diagCommandId = command.id;
		const QString diagCallback  = command.request.functionName;
		const QString diagTargets   = qmudMmStartupDiagEngineLabels(command.request.engines);
#endif
		const LuaBatchDispatchRequest dispatchRequest = command.request;
		beginPluginCallbackDispatchCommandGuard(command);
		m_luaExecutor->dispatchBatchAsync(
		    dispatchRequest, this,
		    [this, command = std::move(command)
#ifndef NDEBUG
		               ,
		     diagShouldLog, diagCommandId, diagCallback, diagTargets
#endif
		](const LuaBatchDispatchResult &result) mutable
		    {
			    qmudCompleteLuaWorkerCallbackDispatch(
			        m_pluginCallbackDispatchWorkerInFlight,
			        [this, &command, &result
#ifndef NDEBUG
			         ,
			         diagShouldLog, diagCommandId, &diagCallback, &diagTargets
#endif
			    ]
			        {
#ifndef NDEBUG
				        if (diagShouldLog)
				        {
					        qInfo().noquote()
					            << QStringLiteral(
					                   "[QMud][MMStartupDiag] dispatch-async-completion id=%1 callback=%2 "
					                   "targets=%3 %4")
					                   .arg(diagCommandId)
					                   .arg(diagCallback, diagTargets, qmudMmStartupDiagResultLabel(result));
				        }
#endif
				        handleCompletedPluginCallbackDispatchCommand(std::move(command),
				                                                     LuaBatchDispatchResult(result));
			        },
			        [this]
			        {
				        if (!m_pluginCallbackDispatchQueue.isEmpty())
					        queuePluginCallbackDispatchDrain();
			        });
		    });
		return;
	}
#ifndef NDEBUG
	if (pluginCallbackDispatchShouldLog(dispatchDiagnosticsEnabled, command.request.functionName,
	                                    queueWaitMs))
	{
		qInfo().noquote() << QStringLiteral(
		                         "[QMud][LuaDispatchDiag] dispatch-async id=%1 callback=%2 kind=%3 "
		                         "queue_wait_ms=%4 queue_at_enqueue=%5 queue_remaining=%6")
		                         .arg(command.id)
		                         .arg(command.request.functionName,
		                              pluginCallbackDispatchKindLabel(command.request.kind))
		                         .arg(queueWaitMs)
		                         .arg(command.queueDepthAtEnqueue)
		                         .arg(m_pluginCallbackDispatchQueue.size());
	}
#endif
	m_pluginCallbackDispatchWorkerInFlight = true;
#ifndef NDEBUG
	const bool    diagShouldLog = mmStartupDiag;
	const quint64 diagCommandId = command.id;
	const QString diagCallback  = command.request.functionName;
	const QString diagTargets   = qmudMmStartupDiagEngineLabels(command.request.engines);
#endif
	const LuaBatchDispatchRequest dispatchRequest = command.request;
	beginPluginCallbackDispatchCommandGuard(command);
	m_luaExecutor->dispatchBatchAsync(
	    dispatchRequest, this,
	    [this, command = std::move(command)
#ifndef NDEBUG
	               ,
	     diagShouldLog, diagCommandId, diagCallback, diagTargets
#endif
	](const LuaBatchDispatchResult &result) mutable
	    {
		    qmudCompleteLuaWorkerCallbackDispatch(
		        m_pluginCallbackDispatchWorkerInFlight,
		        [this, &command, &result
#ifndef NDEBUG
		         ,
		         diagShouldLog, diagCommandId, &diagCallback, &diagTargets
#endif
		    ]
		        {
#ifndef NDEBUG
			        if (diagShouldLog)
			        {
				        qInfo().noquote()
				            << QStringLiteral("[QMud][MMStartupDiag] dispatch-async-done "
				                              "id=%1 callback=%2 targets=%3 %4")
				                   .arg(diagCommandId)
				                   .arg(diagCallback, diagTargets, qmudMmStartupDiagResultLabel(result));
			        }
#endif
			        handleCompletedPluginCallbackDispatchCommand(std::move(command),
			                                                     LuaBatchDispatchResult(result));
		        },
		        [this]
		        {
			        if (!m_pluginCallbackDispatchQueue.isEmpty())
				        queuePluginCallbackDispatchDrain();
		        });
	    });
}

void WorldRuntime::propagateObservedPluginCallbacksToLuaEngines(const bool completionBarrier) const
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::propagateObservedPluginCallbacksToLuaEngines");
	QVector<QSharedPointer<LuaCallbackEngine>> engines;
	engines.reserve(m_plugins.size());
	for (const Plugin &plugin : m_plugins)
	{
		if (plugin.lua)
			engines.push_back(plugin.lua);
	}
	if (engines.isEmpty())
		return;

	LuaBatchDispatchRequest request;
	request.kind                     = LuaBatchDispatchKind::UpdateObservedCallbacksMany;
	request.engines                  = std::move(engines);
	request.observedCallbackNamesArg = m_observedPluginCallbacks;
	if (completionBarrier)
		static_cast<void>(dispatchLuaBatch(request));
	else
		dispatchLuaBatchAsync(request);
}

bool WorldRuntime::callPluginCallbacksStopOnFalse(const QString &functionName, const QString &payload)
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::callPluginCallbacksStopOnFalse");
	const QVector<QSharedPointer<LuaCallbackEngine>> recipients =
	    collectPluginCallbackRecipients(functionName);
	if (recipients.isEmpty())
		return true;

	LuaBatchDispatchRequest request;
	request.kind                         = LuaBatchDispatchKind::StringStopOnFalse;
	request.engines                      = recipients;
	request.functionName                 = functionName;
	request.stringArg                    = payload;
	request.defaultResult                = true;
	request.revalidateObservedRecipients = true;
	const LuaBatchDispatchResult result  = queuePluginCallbackDispatch(request, true);
	return result.boolResultValid ? result.boolResult : true;
}

void WorldRuntime::callPluginCallbacks(const QString &functionName, const QString &payload,
                                       const bool completionBarrier)
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::callPluginCallbacks");
	const QVector<QSharedPointer<LuaCallbackEngine>> recipients =
	    collectPluginCallbackRecipients(functionName);
	if (recipients.isEmpty())
		return;
	LuaBatchDispatchRequest request;
	request.kind                         = LuaBatchDispatchKind::String;
	request.engines                      = recipients;
	request.functionName                 = functionName;
	request.stringArg                    = payload;
	request.defaultResult                = true;
	request.revalidateObservedRecipients = true;
	static_cast<void>(queuePluginCallbackDispatch(request, completionBarrier));
}

void WorldRuntime::callPluginCallbacksNoArgs(const QString &functionName, const bool completionBarrier)
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::callPluginCallbacksNoArgs");
	const QVector<QSharedPointer<LuaCallbackEngine>> recipients =
	    collectPluginCallbackRecipients(functionName);
	if (recipients.isEmpty())
		return;
	LuaBatchDispatchRequest request;
	request.kind                         = LuaBatchDispatchKind::NoArgs;
	request.engines                      = recipients;
	request.functionName                 = functionName;
	request.defaultResult                = true;
	request.revalidateObservedRecipients = true;
	static_cast<void>(queuePluginCallbackDispatch(request, completionBarrier));
}

bool WorldRuntime::callPluginCallbacksStopOnTrue(const QString &functionName, long arg1, const QString &arg2)
{
	const QVector<QSharedPointer<LuaCallbackEngine>> recipients =
	    collectPluginCallbackRecipients(functionName);
	if (recipients.isEmpty())
		return false;
	LuaBatchDispatchRequest request;
	request.kind                         = LuaBatchDispatchKind::NumberAndStringStopOnTrue;
	request.engines                      = recipients;
	request.functionName                 = functionName;
	request.numberArg1                   = arg1;
	request.stringArg2                   = arg2;
	request.defaultResult                = false;
	request.revalidateObservedRecipients = true;
	const LuaBatchDispatchResult result  = queuePluginCallbackDispatch(request, true);
	return result.boolResultValid ? result.boolResult : false;
}

bool WorldRuntime::callPluginCallbacksStopOnTrueWithString(const QString &functionName,
                                                           const QString &payload)
{
	const QVector<QSharedPointer<LuaCallbackEngine>> recipients =
	    collectPluginCallbackRecipients(functionName);
	if (recipients.isEmpty())
		return false;
	LuaBatchDispatchRequest request;
	request.kind                         = LuaBatchDispatchKind::StringHandled;
	request.engines                      = recipients;
	request.functionName                 = functionName;
	request.stringArg                    = payload;
	request.revalidateObservedRecipients = true;
	const LuaBatchDispatchResult result  = queuePluginCallbackDispatch(request, true);
	return result.boolResultValid ? result.boolResult : false;
}

void WorldRuntime::callPluginCallbacksTransformBytes(const QString &functionName, QByteArray &payload)
{
	const QVector<QSharedPointer<LuaCallbackEngine>> recipients =
	    collectPluginCallbackRecipients(functionName);
	if (recipients.isEmpty())
		return;
	LuaBatchDispatchRequest request;
	request.kind                         = LuaBatchDispatchKind::BytesInOut;
	request.engines                      = recipients;
	request.functionName                 = functionName;
	request.bytesArg                     = payload;
	request.revalidateObservedRecipients = true;
	const LuaBatchDispatchResult result  = queuePluginCallbackDispatch(request, true);
	payload                              = result.bytesResult;
}

void WorldRuntime::callPluginCallbacksTransformString(const QString &functionName, QString &payload)
{
	const QVector<QSharedPointer<LuaCallbackEngine>> recipients =
	    collectPluginCallbackRecipients(functionName);
	if (recipients.isEmpty())
		return;
	LuaBatchDispatchRequest request;
	request.kind                         = LuaBatchDispatchKind::StringInOut;
	request.engines                      = recipients;
	request.functionName                 = functionName;
	request.stringArg                    = payload;
	request.revalidateObservedRecipients = true;
	const LuaBatchDispatchResult result  = queuePluginCallbackDispatch(request, true);
	payload                              = result.stringResult;
}

bool WorldRuntime::callPluginCallbacksStopOnFalseWithNumberAndString(const QString &functionName, long arg1,
                                                                     const QString &arg2)
{
	const QVector<QSharedPointer<LuaCallbackEngine>> recipients =
	    collectPluginCallbackRecipients(functionName);
	if (recipients.isEmpty())
		return true;
	LuaBatchDispatchRequest request;
	request.kind                         = LuaBatchDispatchKind::NumberAndStringStopOnFalse;
	request.engines                      = recipients;
	request.functionName                 = functionName;
	request.numberArg1                   = arg1;
	request.stringArg2                   = arg2;
	request.defaultResult                = true;
	request.revalidateObservedRecipients = true;
	const LuaBatchDispatchResult result  = queuePluginCallbackDispatch(request, true);
	return result.boolResultValid ? result.boolResult : true;
}

bool WorldRuntime::callPluginCallbacksStopOnFalseWithTwoNumbersAndString(const QString &functionName,
                                                                         long arg1, long arg2,
                                                                         const QString &arg3)
{
	const QVector<QSharedPointer<LuaCallbackEngine>> recipients =
	    collectPluginCallbackRecipients(functionName);
	if (recipients.isEmpty())
		return true;
	LuaBatchDispatchRequest request;
	request.kind                         = LuaBatchDispatchKind::TwoNumbersAndStringStopOnFalse;
	request.engines                      = recipients;
	request.functionName                 = functionName;
	request.numberArg1                   = arg1;
	request.numberArg2                   = arg2;
	request.stringArg2                   = arg3;
	request.defaultResult                = true;
	request.revalidateObservedRecipients = true;
	const LuaBatchDispatchResult result  = queuePluginCallbackDispatch(request, true);
	return result.boolResultValid ? result.boolResult : true;
}

void WorldRuntime::callPluginCallbacksWithNumberAndString(const QString &functionName, long arg1,
                                                          const QString &arg2, const bool completionBarrier)
{
	const QVector<QSharedPointer<LuaCallbackEngine>> recipients =
	    collectPluginCallbackRecipients(functionName);
	if (recipients.isEmpty())
		return;
	LuaBatchDispatchRequest request;
	request.kind                         = LuaBatchDispatchKind::NumberAndString;
	request.engines                      = recipients;
	request.functionName                 = functionName;
	request.numberArg1                   = arg1;
	request.stringArg2                   = arg2;
	request.defaultResult                = true;
	request.revalidateObservedRecipients = true;
	static_cast<void>(queuePluginCallbackDispatch(request, completionBarrier));
}

void WorldRuntime::callPluginCallbacksWithBytes(const QString &functionName, const QByteArray &payload,
                                                const bool completionBarrier)
{
	const QVector<QSharedPointer<LuaCallbackEngine>> recipients =
	    collectPluginCallbackRecipients(functionName);
	if (recipients.isEmpty())
		return;
	LuaBatchDispatchRequest request;
	request.kind                         = LuaBatchDispatchKind::Bytes;
	request.engines                      = recipients;
	request.functionName                 = functionName;
	request.bytesArg                     = payload;
	request.defaultResult                = true;
	request.revalidateObservedRecipients = true;
	static_cast<void>(queuePluginCallbackDispatch(request, completionBarrier));
}

bool WorldRuntime::callPluginCallbacksStopOnTrueBytes(const QString &functionName, long arg1,
                                                      const QByteArray &payload)
{
	const QVector<QSharedPointer<LuaCallbackEngine>> recipients =
	    collectPluginCallbackRecipients(functionName);
	if (recipients.isEmpty())
		return false;
	LuaBatchDispatchRequest request;
	request.kind                         = LuaBatchDispatchKind::NumberAndBytesStopOnTrue;
	request.engines                      = recipients;
	request.functionName                 = functionName;
	request.numberArg1                   = arg1;
	request.bytesArg                     = payload;
	request.defaultResult                = false;
	request.revalidateObservedRecipients = true;
	const LuaBatchDispatchResult result  = queuePluginCallbackDispatch(request, true);
	return result.boolResultValid ? result.boolResult : false;
}

void WorldRuntime::callPluginCallbacksWithNumberAndBytes(const QString &functionName, long arg1,
                                                         const QByteArray &payload,
                                                         const bool        completionBarrier)
{
	const QVector<QSharedPointer<LuaCallbackEngine>> recipients =
	    collectPluginCallbackRecipients(functionName);
	if (recipients.isEmpty())
		return;
	LuaBatchDispatchRequest request;
	request.kind                         = LuaBatchDispatchKind::NumberAndBytes;
	request.engines                      = recipients;
	request.functionName                 = functionName;
	request.numberArg1                   = arg1;
	request.bytesArg                     = payload;
	request.defaultResult                = true;
	request.revalidateObservedRecipients = true;
	static_cast<void>(queuePluginCallbackDispatch(request, completionBarrier));
}

bool WorldRuntime::callPluginHotspotFunction(const QString &pluginId, const QString &functionName, long flags,
                                             const QString &hotspotId, const QString &miniWindowName,
                                             const bool queueWhenCallbackLaneBusy)
{
	if (pluginId.isEmpty() || functionName.isEmpty())
		return false;
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return false;
	Plugin const &plugin = m_plugins[index];
	if (!canExecutePlugin(plugin))
		return false;
	const unsigned short previousActionSource = m_currentActionSource;
	m_currentActionSource                     = eHotspotCallback;
	LuaBatchDispatchRequest request;
	request.kind                    = LuaBatchDispatchKind::NumberAndStringStopOnTrue;
	request.engines                 = {plugin.lua};
	request.functionName            = functionName;
	request.numberArg1              = flags;
	request.stringArg2              = hotspotId;
	request.defaultResult           = false;
	request.inputCritical           = true;
	request.hasActionSourceOverride = true;
	request.actionSourceOverride    = eHotspotCallback;
	request.miniWindowExecutionName = miniWindowName;
	if (qmudShouldQueueContendedHotspotCallback(
	        queueWhenCallbackLaneBusy, m_pluginCallbackDispatchActive, m_pluginCallbackDispatchWorkerInFlight,
	        !m_pluginCallbackDispatchQueue.isEmpty(), m_pluginCallbackDispatchDrainQueued))
	{
		queuePluginCallbackDispatchAsync(request);
		m_currentActionSource = previousActionSource;
		return false;
	}
	const LuaBatchDispatchResult result = queuePluginCallbackDispatch(request, true);
	m_currentActionSource               = previousActionSource;
	return result.boolResultValid ? result.boolResult : false;
}

void WorldRuntime::queueLuaModalCallbackResume(const QString &pluginId, const quint64 resumeId,
                                               const QString &result)
{
	if (resumeId == 0)
		return;
	if (QThread::currentThread() != thread())
	{
		const QPointer<WorldRuntime> runtimeGuard(this);
		const bool                   queued = QMetaObject::invokeMethod(
		    this,
		    [runtimeGuard, pluginId, resumeId, result]
		    {
			    if (runtimeGuard)
				    runtimeGuard->queueLuaModalCallbackResume(pluginId, resumeId, result);
		    },
		    Qt::QueuedConnection);
		if (!queued)
		{
			qWarning().noquote() << QStringLiteral(
			    "[QMud][LuaBridge] failed to queue modal Lua callback resume");
		}
		return;
	}

	auto suspendedIt = m_suspendedPluginCallbackDispatches.find(resumeId);
	if (suspendedIt == m_suspendedPluginCallbackDispatches.end())
		return;
	if (suspendedIt->resumeQueued)
		return;

	auto reportModalResumePluginMismatch = [](const char *message)
	{
#ifndef NDEBUG
		qWarning().noquote() << QStringLiteral("[QMud][LuaBridge] invalid modal resume ownership: %1")
		                            .arg(QString::fromLatin1(message));
#else
		Q_UNUSED(message);
#endif
	};

	QSharedPointer<LuaCallbackEngine> engine;
	const int                         suspendedEngineIndex = suspendedIt->nextEngineIndex - 1;
	if (suspendedEngineIndex >= 0 && suspendedEngineIndex < suspendedIt->command.request.engines.size())
	{
		engine = suspendedIt->command.request.engines.at(suspendedEngineIndex);
	}
	if (!engine)
	{
		finishSuspendedPluginCallbackDispatchWithFallback(resumeId, false);
		return;
	}

	const QString expectedPluginId = suspendedIt->pluginId;
	if (!pluginId.isEmpty() && !expectedPluginId.isEmpty() &&
	    pluginId.compare(expectedPluginId, Qt::CaseInsensitive) != 0)
	{
		reportModalResumePluginMismatch("modal resume plugin id does not match suspended dispatch owner");
		finishSuspendedPluginCallbackDispatchWithFallback(resumeId, true);
		return;
	}
	const auto beforeRuntimeResume = suspendedIt->beforeRuntimeResumeCallback;
	suspendedIt->resumeQueued      = true;
	if (beforeRuntimeResume)
		beforeRuntimeResume(*this, result);

	const auto validatedSuspendedIt = m_suspendedPluginCallbackDispatches.constFind(resumeId);
	if (validatedSuspendedIt == m_suspendedPluginCallbackDispatches.constEnd())
		return;
	if (!pluginId.isEmpty() && !validatedSuspendedIt->pluginId.isEmpty() &&
	    pluginId.compare(validatedSuspendedIt->pluginId, Qt::CaseInsensitive) != 0)
	{
		reportModalResumePluginMismatch(
		    "modal resume plugin id changed before callback-lane resume dispatch");
		finishSuspendedPluginCallbackDispatchWithFallback(resumeId, true);
		return;
	}
	QSharedPointer<LuaCallbackEngine> validatedEngine;
	const int validatedSuspendedEngineIndex = validatedSuspendedIt->nextEngineIndex - 1;
	if (validatedSuspendedEngineIndex >= 0 &&
	    validatedSuspendedEngineIndex < validatedSuspendedIt->command.request.engines.size())
	{
		validatedEngine = validatedSuspendedIt->command.request.engines.at(validatedSuspendedEngineIndex);
	}
	if (!validatedEngine)
	{
		finishSuspendedPluginCallbackDispatchWithFallback(resumeId, false);
		return;
	}
	LuaBatchDispatchRequest request;
	request.kind                    = LuaBatchDispatchKind::ResumeSuspendedModalString;
	request.engines                 = {validatedEngine};
	request.modalResumeId           = validatedSuspendedIt->engineModalResumeId;
	request.runtimeModalResumeId    = resumeId;
	request.stringArg               = result;
	request.inputCritical           = true;
	request.miniWindowExecutionName = validatedSuspendedIt->command.request.miniWindowExecutionName;
	if (!tryQueuePluginCallbackDispatchAsyncOnRuntimeThread(request))
		finishSuspendedPluginCallbackDispatchWithFallback(resumeId, true);
}

bool WorldRuntime::callWorldHotspotFunction(const QString &functionName, long flags, const QString &hotspotId,
                                            const QString &miniWindowName,
                                            const bool     queueWhenCallbackLaneBusy)
{
	if (functionName.isEmpty() || !m_luaCallbacks)
		return false;
	const QSharedPointer<LuaCallbackEngine> worldLua(m_luaCallbacks, [](LuaCallbackEngine * /*unused*/) {});
	const unsigned short                    previousActionSource = m_currentActionSource;
	m_currentActionSource                                        = eHotspotCallback;
	LuaBatchDispatchRequest request;
	request.kind                    = LuaBatchDispatchKind::NumberAndStringStopOnTrue;
	request.engines                 = {worldLua};
	request.functionName            = functionName;
	request.numberArg1              = flags;
	request.stringArg2              = hotspotId;
	request.defaultResult           = false;
	request.inputCritical           = true;
	request.hasActionSourceOverride = true;
	request.actionSourceOverride    = eHotspotCallback;
	request.miniWindowExecutionName = miniWindowName;
	if (qmudShouldQueueContendedHotspotCallback(
	        queueWhenCallbackLaneBusy, m_pluginCallbackDispatchActive, m_pluginCallbackDispatchWorkerInFlight,
	        !m_pluginCallbackDispatchQueue.isEmpty(), m_pluginCallbackDispatchDrainQueued))
	{
		queuePluginCallbackDispatchAsync(request);
		m_currentActionSource = previousActionSource;
		return false;
	}
	const LuaBatchDispatchResult result = queuePluginCallbackDispatch(request, true);
	m_currentActionSource               = previousActionSource;
	return result.boolResultValid ? result.boolResult : false;
}

void WorldRuntime::setLuaScriptText(const QString &script)
{
	m_luaScriptText = script;
	if (m_luaCallbacks)
	{
		LuaEngineObservedInitializationRequest request;
		request.engine     = m_luaCallbacks;
		request.runtime    = this;
		request.scriptText = script;
		dispatchInitializeLuaEnginesWithObservedCallbacks({request}, true);
	}
}

void WorldRuntime::sendText(const QString &text, bool addNewline)
{
	if (!addNewline)
	{
		if (isEnabledFlag(m_worldAttributes.value(QStringLiteral("utf_8"))))
			sendToWorld(text.toUtf8());
		else
		{
			const QString legacyEncoding = qmudNormalizeWorldTextEncodingName(
			    m_worldAttributes.value(QStringLiteral("legacy_encoding")));
			sendToWorld(qmudEncodeWorldText(text, legacyEncoding, nullptr));
		}
		return;
	}

	const bool echo = isEnabledFlag(m_worldAttributes.value(QStringLiteral("display_my_input")));
	static_cast<void>(sendCommand(text, echo, false, false, false, false));
}

void WorldRuntime::outputText(const QString &text, bool note, bool newLine)
{
	if (text.isEmpty() && !newLine)
		return;
	const int type = note ? 1 : 0;
	const int log  = note ? (isEnabledFlag(m_worldAttributes.value(QStringLiteral("log_notes"))) ? 1 : 0)
	                      : (isEnabledFlag(m_worldAttributes.value(QStringLiteral("log_output"))) ? 1 : 0);
	firePluginScreendraw(type, log, text);
	QString                     displayText          = text;
	const bool                  serverSideWrapActive = isConnected() && m_telnet.isNawsNegotiated();
	const FixedColumnWrapConfig wrapConfig =
	    (!note && serverSideWrapActive)
	        ? FixedColumnWrapConfig{}
	        : localOutputWrapConfig(m_worldAttributes, serverSideWrapActive, m_telnet.windowColumns());
	if (wrapConfig.enabled && !displayText.isEmpty())
		wrapPlainLineForColumn(displayText, wrapConfig.wrapColumn, wrapConfig.indentParas);
	const QVector<OutputLineSegment> segments = splitOutputTextAtLineBreaks(displayText, {}, newLine);
	for (const OutputLineSegment &segment : segments)
		emit outputRequested(segment.text, segment.hardReturn, note);
}

void WorldRuntime::outputStyledText(const QString &text, const QVector<StyleSpan> &spans, bool note,
                                    bool newLine)
{
	if (text.isEmpty() && spans.isEmpty() && !newLine)
		return;
	const int type = note ? 1 : 0;
	const int log  = note ? (isEnabledFlag(m_worldAttributes.value(QStringLiteral("log_notes"))) ? 1 : 0)
	                      : (isEnabledFlag(m_worldAttributes.value(QStringLiteral("log_output"))) ? 1 : 0);
	firePluginScreendraw(type, log, text);
	QString                     displayText          = text;
	QVector<StyleSpan>          displaySpans         = spans;
	const bool                  serverSideWrapActive = isConnected() && m_telnet.isNawsNegotiated();
	const FixedColumnWrapConfig wrapConfig =
	    (!note && serverSideWrapActive)
	        ? FixedColumnWrapConfig{}
	        : localOutputWrapConfig(m_worldAttributes, serverSideWrapActive, m_telnet.windowColumns());
	if (wrapConfig.enabled && !displayText.isEmpty())
	{
		if (displaySpans.isEmpty())
			wrapPlainLineForColumn(displayText, wrapConfig.wrapColumn, wrapConfig.indentParas);
		else
			wrapStyledLineForColumn(displayText, displaySpans, wrapConfig.wrapColumn, wrapConfig.indentParas);
	}
	const QVector<OutputLineSegment> segments =
	    splitOutputTextAtLineBreaks(displayText, displaySpans, newLine);
	for (const OutputLineSegment &segment : segments)
		emit outputStyledRequested(segment.text, segment.spans, segment.hardReturn, note);
}

void WorldRuntime::prepareInputEchoForDisplay(QString &text, QVector<StyleSpan> &spans,
                                              const bool appendToCurrentLine) const
{
	if (text.isEmpty())
		return;

	const bool wrapEnabled = isEnabledFlag(m_worldAttributes.value(QStringLiteral("wrap")));
	if (!wrapEnabled)
		return;

	const bool                  nawsNegotiated = isConnected() && m_telnet.isNawsNegotiated();
	const FixedColumnWrapConfig wrapConfig =
	    localOutputWrapConfig(m_worldAttributes, nawsNegotiated, m_telnet.windowColumns());
	if (!wrapConfig.enabled || wrapConfig.wrapColumn <= 0)
		return;

	int firstLinePrefixColumns = 0;
	if (appendToCurrentLine && !m_lines.isEmpty())
	{
		firstLinePrefixColumns = trailingLineColumnWidthForWrap(m_lines.constLast().text);
		if (firstLinePrefixColumns >= wrapConfig.wrapColumn)
		{
			text.prepend(QLatin1Char('\n'));
			if (!spans.isEmpty())
			{
				StyleSpan prefixSpan = spans.constFirst();
				prefixSpan.length    = 1;
				spans.prepend(prefixSpan);
			}
			firstLinePrefixColumns = 0;
		}
	}
	if (spans.isEmpty())
		wrapPlainLineForColumn(text, wrapConfig.wrapColumn, wrapConfig.indentParas, firstLinePrefixColumns);
	else
		wrapStyledLineForColumn(text, spans, wrapConfig.wrapColumn, wrapConfig.indentParas,
		                        firstLinePrefixColumns);
}

void WorldRuntime::outputAnsiText(const QString &text, bool note)
{
	if (text.isEmpty())
		return;
	const bool                  serverSideWrapActive = isConnected() && m_telnet.isNawsNegotiated();
	const FixedColumnWrapConfig wrapConfig =
	    (!note && serverSideWrapActive)
	        ? FixedColumnWrapConfig{}
	        : localOutputWrapConfig(m_worldAttributes, serverSideWrapActive, m_telnet.windowColumns());

	auto parseColorValue = [](const QString &value) -> QColor
	{
		if (value.isEmpty())
			return {};
		QColor color(value);
		if (color.isValid())
			return color;
		bool      ok      = false;
		const int numeric = value.toInt(&ok);
		if (!ok)
			return {};
		const int r = numeric & 0xFF;
		const int g = (numeric >> 8) & 0xFF;
		const int b = (numeric >> 16) & 0xFF;
		return {r, g, b};
	};

	QVector<QColor> normalAnsi(8);
	QVector<QColor> boldAnsi(8);
	QVector<QColor> customText(16);
	QVector<QColor> customBack(16);
	normalAnsi[0] = QColor(0, 0, 0);
	normalAnsi[1] = QColor(128, 0, 0);
	normalAnsi[2] = QColor(0, 128, 0);
	normalAnsi[3] = QColor(128, 128, 0);
	normalAnsi[4] = QColor(0, 0, 128);
	normalAnsi[5] = QColor(128, 0, 128);
	normalAnsi[6] = QColor(0, 128, 128);
	normalAnsi[7] = QColor(192, 192, 192);
	boldAnsi[0]   = QColor(128, 128, 128);
	boldAnsi[1]   = QColor(255, 0, 0);
	boldAnsi[2]   = QColor(0, 255, 0);
	boldAnsi[3]   = QColor(255, 255, 0);
	boldAnsi[4]   = QColor(0, 0, 255);
	boldAnsi[5]   = QColor(255, 0, 255);
	boldAnsi[6]   = QColor(0, 255, 255);
	boldAnsi[7]   = QColor(255, 255, 255);
	for (int i = 0; i < customText.size(); ++i)
	{
		customText[i] = QColor(255, 255, 255);
		customBack[i] = QColor(0, 0, 0);
	}

	for (const auto &colour : m_colours)
	{
		const QString group = colour.group.trimmed().toLower();
		bool          ok    = false;
		const int     seq   = colour.attributes.value(QStringLiteral("seq")).toInt(&ok);
		const int     index = ok ? seq - 1 : -1;
		if (index < 0)
			continue;
		if (group == QStringLiteral("ansi/normal") && index < normalAnsi.size())
		{
			const QColor rgb = parseColorValue(colour.attributes.value(QStringLiteral("rgb")));
			if (rgb.isValid())
				normalAnsi[index] = rgb;
		}
		else if (group == QStringLiteral("ansi/bold") && index < boldAnsi.size())
		{
			const QColor rgb = parseColorValue(colour.attributes.value(QStringLiteral("rgb")));
			if (rgb.isValid())
				boldAnsi[index] = rgb;
		}
		else if ((group == QStringLiteral("custom/custom") || group == QStringLiteral("custom")) &&
		         index < customText.size())
		{
			const QColor textColor = parseColorValue(colour.attributes.value(QStringLiteral("text")));
			const QColor backColor = parseColorValue(colour.attributes.value(QStringLiteral("back")));
			if (textColor.isValid())
				customText[index] = textColor;
			if (backColor.isValid())
				customBack[index] = backColor;
		}
	}

	QColor defaultFore = parseColorValue(m_worldAttributes.value(QStringLiteral("output_text_colour")));
	QColor defaultBack = parseColorValue(m_worldAttributes.value(QStringLiteral("output_background_colour")));
	if (note)
	{
		const auto colourFromLong = [](const long value) -> QColor
		{
			return {static_cast<int>(value & 0xFF), static_cast<int>((value >> 8) & 0xFF),
			        static_cast<int>((value >> 16) & 0xFF)};
		};
		defaultFore = colourFromLong(noteColourFore());
		defaultBack = colourFromLong(noteColourBack());
	}
	if (!defaultFore.isValid())
		defaultFore = normalAnsi.value(7);
	if (!defaultBack.isValid())
		defaultBack = normalAnsi.value(0);

	struct StyleState
	{
			bool    bold{false};
			bool    underline{false};
			bool    italic{false};
			bool    blink{false};
			bool    inverse{false};
			bool    strike{false};
			QString fore;
			QString back;
	};

	StyleState current;
	current.fore = defaultFore.name();
	current.back = defaultBack.name();

	auto colorFromIndex = [](int idx) -> QString
	{
		if (idx < 0 || idx >= 256)
			return {};
		const AppController *app = AppController::instance();
		const QMudColorRef   ref = app ? app->xtermColorAt(idx) : qmudRgb(0, 0, 0);
		return QColor(qmudRed(ref), qmudGreen(ref), qmudBlue(ref)).name();
	};

	auto setFgIndex = [&](int idx)
	{
		if (idx < 0 || idx >= normalAnsi.size())
		{
			current.fore.clear();
			return;
		}
		const QColor chosen = current.bold ? boldAnsi.at(idx) : normalAnsi.at(idx);
		current.fore        = chosen.isValid() ? chosen.name() : QString();
	};
	auto setBgIndex = [&](int idx)
	{
		if (idx < 0 || idx >= normalAnsi.size())
		{
			current.back.clear();
			return;
		}
		const QColor chosen = normalAnsi.at(idx);
		current.back        = chosen.isValid() ? chosen.name() : QString();
	};

	QString            lineText;
	QVector<StyleSpan> lineSpans;

	auto               emitLine = [&](bool newLine)
	{
		QString            displayText  = lineText;
		QVector<StyleSpan> displaySpans = lineSpans;
		if (wrapConfig.enabled && !displayText.isEmpty())
			wrapStyledLineForColumn(displayText, displaySpans, wrapConfig.wrapColumn, wrapConfig.indentParas);
		emit outputStyledRequested(displayText, displaySpans, newLine, note);
		lineText.clear();
		lineSpans.clear();
	};

	auto appendPlain = [&](const QString &raw)
	{
		if (raw.isEmpty())
			return;
		QString cooked = raw;
		cooked.remove(QLatin1Char('\r'));
		if (cooked.isEmpty())
			return;
		const int segmentLength = safeQSizeToInt(cooked.size());
		int       start         = 0;
		while (start <= segmentLength)
		{
			const qsizetype newline = cooked.indexOf(QLatin1Char('\n'), start);
			const int pieceLen = (newline >= 0) ? safeQSizeToInt(newline - start) : (segmentLength - start);
			if (pieceLen > 0)
			{
				lineText += cooked.mid(start, pieceLen);
				StyleSpan span;
				span.length    = pieceLen;
				span.fore      = current.fore.isEmpty() ? QColor() : QColor(current.fore);
				span.back      = current.back.isEmpty() ? QColor() : QColor(current.back);
				span.bold      = current.bold;
				span.italic    = current.italic;
				span.blink     = current.blink;
				span.underline = current.underline;
				span.inverse   = current.inverse;
				span.strike    = current.strike;
				if (!lineSpans.isEmpty())
				{
					StyleSpan &lastSpan = lineSpans.last();
					if (lastSpan.hasSameStyleAttributes(span))
						lastSpan.length += span.length;
					else
						lineSpans.push_back(span);
				}
				else
					lineSpans.push_back(span);
			}
			if (newline < 0)
				break;
			emitLine(true);
			start = safeQSizeToInt(newline + 1);
		}
	};

	auto applySgr = [&](const QVector<int> &codes)
	{
		QVector<int> params = codes;
		if (params.isEmpty())
			params.push_back(0);
		for (int i = 0; i < params.size(); ++i)
		{
			const int code = params.at(i);
			if (code == 0)
			{
				current      = StyleState();
				current.fore = defaultFore.name();
				current.back = defaultBack.name();
			}
			else if (code == 1)
				current.bold = true;
			else if (code == 3)
				current.italic = true;
			else if (code == 5)
				current.blink = true;
			else if (code == 4)
				current.underline = true;
			else if (code == 7)
			{
				current.inverse = true;
				if (!current.fore.isEmpty() || !current.back.isEmpty())
					qSwap(current.fore, current.back);
			}
			else if (code == 9)
				current.strike = true;
			else if (code == 22)
				current.bold = false;
			else if (code == 23)
				current.italic = false;
			else if (code == 25)
				current.blink = false;
			else if (code == 24)
				current.underline = false;
			else if (code == 27)
				current.inverse = false;
			else if (code == 29)
				current.strike = false;
			else if (code == 39)
				current.fore = defaultFore.name();
			else if (code == 49)
				current.back = defaultBack.name();
			else if (code >= 30 && code <= 37)
				setFgIndex(code - 30);
			else if (code >= 40 && code <= 47)
				setBgIndex(code - 40);
			else if (code >= 90 && code <= 97)
			{
				const int idx = code - 90;
				if (idx >= 0 && idx < boldAnsi.size())
					current.fore = boldAnsi.at(idx).name();
			}
			else if (code >= 100 && code <= 107)
			{
				const int idx = code - 100;
				if (idx >= 0 && idx < boldAnsi.size())
					current.back = boldAnsi.at(idx).name();
			}
			else if (code == 38 || code == 48)
			{
				const bool isFore = (code == 38);
				if (i + 1 >= params.size())
					continue;
				const int mode = params.at(i + 1);
				if (mode == 5 && i + 2 < params.size())
				{
					const int idx = params.at(i + 2);
					if (isFore)
						current.fore = colorFromIndex(idx);
					else
						current.back = colorFromIndex(idx);
					i += 2;
				}
				else if (mode == 2 && i + 4 < params.size())
				{
					const int     r   = params.at(i + 2);
					const int     g   = params.at(i + 3);
					const int     b   = params.at(i + 4);
					const QString rgb = QColor(r, g, b).name();
					if (isFore)
						current.fore = rgb;
					else
						current.back = rgb;
					i += 4;
				}
			}
		}
	};

	QByteArray const bytes = text.toUtf8();
	QString          plainText;
	for (int i = 0; i < bytes.size(); ++i)
	{
		const char ch = bytes.at(i);
		if (ch == '\r')
			continue;
		if (ch == '\x1b')
		{
			if (i + 1 >= bytes.size())
				break;
			if (bytes.at(i + 1) != '[')
				continue;
			appendPlain(plainText);
			plainText.clear();
			i += 2;
			QByteArray paramBytes;
			bool       aborted = false;
			while (i < bytes.size())
			{
				const auto byte = static_cast<unsigned char>(bytes.at(i));
				if (byte >= 0x40 && byte <= 0x7E)
					break;
				if (byte >= 0x20 && byte <= 0x3F)
				{
					paramBytes.append(static_cast<char>(byte));
					++i;
					continue;
				}
				aborted = true;
				break;
			}
			if (aborted)
			{
				i -= 1;
				continue;
			}
			if (i >= bytes.size())
				break;
			const char finalByte = bytes.at(i);
			if (finalByte == 'm')
			{
				paramBytes.replace(':', ';');
				const QList<QByteArray> parts = paramBytes.split(';');
				QVector<int>            codes;
				for (const QByteArray &part : parts)
				{
					if (part.isEmpty())
					{
						codes.push_back(0);
						continue;
					}
					bool      ok    = false;
					const int value = part.toInt(&ok);
					if (ok)
						codes.push_back(value);
				}
				applySgr(codes);
			}
			continue;
		}
		plainText.append(ch);
	}

	appendPlain(plainText);
	if (!lineText.isEmpty() || !lineSpans.isEmpty())
		emitLine(false);
}

int WorldRuntime::sendCommand(const QString &text, bool echo, bool queue, bool log, bool history,
                              bool immediate) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), eWorldClosed,
		                          [this, text, echo, queue, log, history, immediate]
		                          { return sendCommand(text, echo, queue, log, history, immediate); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::sendCommand");
	if (m_connectPhase != eConnectConnectedToMud)
		return eWorldClosed;
	if (!m_commandProcessor)
		return eWorldClosed;
	if (m_commandProcessor->pluginProcessingSent())
		return eItemInUse;

	if (immediate)
		m_commandProcessor->sendImmediateText(text, echo, log, history);
	else
		m_commandProcessor->sendRawText(text, echo, queue, log, history);
	return eOK;
}

int WorldRuntime::sendCommandWithTriggerPriority(const QString &text, const bool echo, const bool queue,
                                                 const bool log, const bool history) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(
		    const_cast<WorldRuntime *>(this), eWorldClosed, [this, text, echo, queue, log, history]
		    { return sendCommandWithTriggerPriority(text, echo, queue, log, history); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::sendCommandWithTriggerPriority");
	if (m_connectPhase != eConnectConnectedToMud)
		return eWorldClosed;
	if (!m_commandProcessor)
		return eWorldClosed;
	if (m_commandProcessor->pluginProcessingSent())
		return eItemInUse;

	m_commandProcessor->sendRawTextWithTriggerPriority(text, echo, queue, log, history);
	return eOK;
}

bool WorldRuntime::deferInboundImmediateCommand(const QString &text, const bool echo, const bool log,
                                                const bool triggerPriority) const
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::deferInboundImmediateCommand");
	if (m_inboundCommandDeferralDepth <= 0 || m_flushingDeferredInboundCommands)
		return false;

	DeferredInboundCommand command;
	command.text         = text;
	command.echo         = echo;
	command.log          = log;
	command.actionSource = m_currentActionSource;
	if (triggerPriority)
	{
		const int insertAt = qBound(0, m_deferredInboundTriggerPriorityCommands,
		                            safeQSizeToInt(m_deferredInboundCommands.size()));
		m_deferredInboundCommands.insert(insertAt, command);
		m_deferredInboundTriggerPriorityCommands = insertAt + 1;
		return true;
	}

	m_deferredInboundCommands.push_back(command);
	return true;
}

void WorldRuntime::logInputCommand(const QString &text) const
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(const_cast<WorldRuntime *>(this), [this, text] { logInputCommand(text); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::logInputCommand");
	if (!m_commandProcessor)
		return;
	m_commandProcessor->logInputCommand(text);
}

bool WorldRuntime::executeAcceleratorCommand(int commandId, const QString &keyLabel)
{
	if (!m_commandProcessor)
		return false;
	const AcceleratorEntry *entry = acceleratorEntryForCommand(commandId);
	if (!entry || entry->text.isEmpty())
		return false;

	if (entry->sendTo == eSendToExecute)
		return m_commandProcessor->executeCommand(entry->text) == eOK;

	Plugin const *plugin = nullptr;
	if (!entry->pluginId.isEmpty())
	{
		const int index = findPluginIndex(m_plugins, entry->pluginId);
		if (index < 0)
			return false;
		plugin = &m_plugins[index];
		if (!hasValidPluginId(*plugin) || !plugin->enabled || plugin->installPending)
			return false;
	}

	const QString description =
	    keyLabel.isEmpty() ? QStringLiteral("Accelerator") : QStringLiteral("Accelerator: %1").arg(keyLabel);
	m_commandProcessor->sendToFromAccelerator(entry->sendTo, entry->text, description, plugin);
	return true;
}

void WorldRuntime::setNoCommandEcho(bool enabled) const
{
	if (m_commandProcessor)
		m_commandProcessor->setNoEcho(enabled);
}

bool WorldRuntime::noCommandEcho() const
{
	return m_commandProcessor ? m_commandProcessor->noEcho() : false;
}

void WorldRuntime::syncChatAcceptCallsWithPreferences()
{
	const bool shouldAccept =
	    isEnabledFlag(m_worldAttributes.value(QStringLiteral("accept_chat_connections")));
	if (!shouldAccept)
	{
		if (m_chatServer)
			chatStopAcceptingCalls();
		return;
	}

	const int desiredPort = chatIncomingPort();

	if (m_chatServer)
	{
		const int currentPort = m_chatServer->serverPort();
		if (currentPort == desiredPort)
			return;

		chatStopAcceptingCalls();
		setWorldAttribute(QStringLiteral("accept_chat_connections"), QStringLiteral("1"));
	}

	chatAcceptCalls(static_cast<short>(desiredPort));
}

void WorldRuntime::setCommandProcessor(WorldCommandProcessor *processor)
{
	m_commandProcessor = processor;
}

void WorldRuntime::refreshCommandProcessorOptions()
{
	if (!m_commandProcessor)
		return;
	m_commandProcessor->setRuntime(this);
}

WorldRuntime::CommandUiSnapshot WorldRuntime::commandUiSnapshot(const bool includeHistory,
                                                                const bool includeFrameData,
                                                                const bool allowSelectedWordHitTest) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(
		    const_cast<WorldRuntime *>(this), CommandUiSnapshot{},
		    [this, includeHistory, includeFrameData, allowSelectedWordHitTest]
		    { return commandUiSnapshot(includeHistory, includeFrameData, allowSelectedWordHitTest); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::commandUiSnapshot");
	CommandUiSnapshot snapshot;
	if (m_commandProcessor)
	{
		snapshot.queuedCommands                    = m_commandProcessor->queuedCommands();
		snapshot.triggerPriorityQueuedCommandCount = m_commandProcessor->triggerPriorityQueuedCommandCount();
	}
	snapshot.textRectangleLeft              = m_textRectangle.left;
	snapshot.textRectangleTop               = m_textRectangle.top;
	snapshot.textRectangleRight             = m_textRectangle.right;
	snapshot.textRectangleBottom            = m_textRectangle.bottom;
	snapshot.textRectangleBorderOffset      = m_textRectangle.borderOffset;
	snapshot.textRectangleBorderWidth       = m_textRectangle.borderWidth;
	snapshot.textRectangleOutsideFillColour = m_textRectangle.outsideFillColour;
	snapshot.textRectangleOutsideFillStyle  = m_textRectangle.outsideFillStyle;
	snapshot.textRectangleBorderColour      = m_textRectangle.borderColour;
	if (m_view)
	{
		snapshot.inputSelectionStartColumn = m_view->inputSelectionStartColumn();
		snapshot.inputSelectionEndColumn   = m_view->inputSelectionEndColumn();
		snapshot.commandInputText          = m_view->inputText();
		if (includeHistory)
			snapshot.commandHistory = m_view->commandHistoryList();
		snapshot.outputSelectionEndColumn   = m_view->outputSelectionEndColumn();
		snapshot.outputSelectionEndLine     = m_view->outputSelectionEndLine();
		snapshot.outputSelectionStartColumn = m_view->outputSelectionStartColumn();
		snapshot.outputSelectionStartLine   = m_view->outputSelectionStartLine();
		snapshot.hasView                    = true;
		snapshot.outputScrollBarWanted      = m_view->outputScrollBarWanted();
		snapshot.outputScrollPosition       = m_view->outputScrollPosition();
		snapshot.outputClientHeight         = m_view->outputClientHeight();
		snapshot.outputClientWidth          = m_view->outputClientWidth();
		snapshot.viewHeight                 = m_view->height();
		snapshot.viewWidth                  = m_view->width();
		const QRect outputRect              = m_view->outputTextViewportRectangle();
		snapshot.outputTextRectLeft         = outputRect.left();
		snapshot.outputTextRectTop          = outputRect.top();
		snapshot.outputTextRectRight        = outputRect.left() + outputRect.width();
		snapshot.outputTextRectBottom       = outputRect.top() + outputRect.height();
		snapshot.hasLastMousePosition       = m_view->hasLastMousePosition();
		if (snapshot.hasLastMousePosition)
		{
			snapshot.lastMouseX = m_view->lastMousePosition().x();
			snapshot.lastMouseY = m_view->lastMousePosition().y();
		}
	}
	snapshot.selectedWord         = m_wordUnderMenu;
	snapshot.selectedWordResolved = m_wordUnderMenuResolved;
	if (includeFrameData)
	{
		MainWindowHost *host = resolveMainWindowHostForRuntime(const_cast<WorldRuntime *>(this));
		if (auto *frame = dynamic_cast<MainWindow *>(host); frame)
		{
			if (snapshot.selectedWord.isEmpty() && allowSelectedWordHitTest)
			{
				if (WorldChildWindow *activeWorld = frame->activeWorldChildWindow())
				{
					if (WorldView *activeView = activeWorld->view())
						snapshot.selectedWord = activeView->wordUnderCursor();
				}
				snapshot.selectedWordResolved = true;
			}
			snapshot.fullScreenMode   = frame->isFullScreenMode();
			snapshot.worldWindowCount = frame->worldWindowCount();
			if (const QWidget *client = frame->centralWidget())
			{
				snapshot.mainClientHeight = client->height();
				snapshot.mainClientWidth  = client->width();
			}
			if (const QToolBar *toolBar = frame->mainToolbar())
			{
				snapshot.mainToolbarHeight = toolBar->height();
				snapshot.mainToolbarWidth  = toolBar->width();
			}
			if (const QToolBar *toolBar = frame->worldToolbar())
			{
				snapshot.worldToolbarHeight = toolBar->height();
				snapshot.worldToolbarWidth  = toolBar->width();
			}
			if (const QToolBar *toolBar = frame->activityToolbar())
			{
				snapshot.activityToolbarHeight = toolBar->height();
				snapshot.activityToolbarWidth  = toolBar->width();
			}
			if (const QWidget *infoBar = frame->infoBarWidget())
			{
				snapshot.infoBarHeight = infoBar->height();
				snapshot.infoBarWidth  = infoBar->width();
			}
			if (const QStatusBar *statusBar = frame->frameStatusBar())
			{
				snapshot.statusBarHeight = statusBar->height();
				snapshot.statusBarWidth  = statusBar->width();
			}
			if (WorldChildWindow *childWindow = frame->findWorldChildWindow(const_cast<WorldRuntime *>(this)))
			{
				if (!childWindow->isVisible())
					snapshot.worldWindowShowCommand = 0;
				else if (childWindow->isMinimized())
					snapshot.worldWindowShowCommand = 6;
				else if (childWindow->isMaximized())
					snapshot.worldWindowShowCommand = 3;
				snapshot.worldChildWindowHeight = childWindow->height();
				snapshot.worldChildWindowWidth  = childWindow->width();
			}
			snapshot.hasFrameData = true;
		}
	}
	return snapshot;
}

WorldRuntime::RuntimeCountersSnapshot WorldRuntime::runtimeCountersSnapshot(const bool includeStrings) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), RuntimeCountersSnapshot{},
		                          [this, includeStrings] { return runtimeCountersSnapshot(includeStrings); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::runtimeCountersSnapshot");
	const ResolvedNoteColours noteColours = resolveNoteColours(
	    m_notesInRgb, m_noteTextColour, m_noteColourFore, m_noteColourBack, m_colours, m_worldAttributes);
	return runtimeCountersSnapshotWithResolvedNoteColours(includeStrings, noteColours.fore, noteColours.back);
}

WorldRuntime::RuntimeCountersSnapshot WorldRuntime::runtimeCountersSnapshotWithResolvedNoteColours(
    const bool includeStrings, const long noteColourFore, const long noteColourBack) const
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::runtimeCountersSnapshotWithResolvedNoteColours");
	RuntimeCountersSnapshot snapshot;
	snapshot.newLines               = m_newLines;
	snapshot.totalLinesSent         = m_totalLinesSent;
	snapshot.inputPacketCount       = m_inputPacketCount;
	snapshot.outputPacketCount      = m_outputPacketCount;
	snapshot.totalUncompressedBytes = m_telnet.totalUncompressedBytes();
	snapshot.totalCompressedBytes   = m_telnet.totalCompressedBytes();
	snapshot.mccpType               = m_telnet.mccpType();
	snapshot.mxpErrorCount          = m_mxpErrors;
	snapshot.mxpTagCount            = m_telnet.mxpTagCount();
	snapshot.mxpEntityCount         = m_telnet.mxpEntityCount();
	snapshot.bytesIn                = m_bytesIn;
	snapshot.bytesOut               = m_bytesOut;
	snapshot.totalLinesReceived     = m_linesReceived;
	snapshot.outputFontHeight       = m_outputFontHeight;
	snapshot.outputFontWidth        = m_outputFontWidth;
	snapshot.inputFontHeight        = m_inputFontHeight;
	snapshot.inputFontWidth         = m_inputFontWidth;
	snapshot.variableCount          = m_variableCount;
	snapshot.triggerCount           = m_triggerCount;
	snapshot.timerCount             = m_timerCount;
	snapshot.aliasCount             = m_aliasCount;
	snapshot.queuedCommandCount     = m_queuedCommandCount;
	snapshot.mappingCount           = safeQSizeToInt(m_mappingList.size());
	snapshot.outputLineCount        = safeQSizeToInt(m_lines.size());
	snapshot.customElementCount     = m_telnet.customElementCount();
	snapshot.customEntityCount      = m_telnet.customEntityCount();
	snapshot.connectPhase           = m_connectPhase;
	snapshot.peerAddressV4          = m_socket ? m_socket->peerAddressV4() : 0;
	snapshot.proxyAddressV4         = m_proxyAddressV4;
	snapshot.logFilePosition        = m_logFile.isOpen() ? m_logFile.pos() : 0;
	snapshot.triggerTimeSeconds     = static_cast<double>(m_triggerTimeNs) / 1000000000.0;
	snapshot.aliasTimeSeconds       = static_cast<double>(m_aliasTimeNs) / 1000000000.0;
	snapshot.scriptTimeSeconds =
	    static_cast<double>(m_scriptTimeNanos.load(std::memory_order_relaxed)) / 1000000000.0;
	snapshot.noCommandEcho          = m_commandProcessor ? m_commandProcessor->noEcho() : false;
	snapshot.debugIncomingPackets   = m_debugIncomingPackets;
	snapshot.isCompressing          = m_telnet.isCompressing();
	snapshot.isMxpActive            = m_mxpActive;
	snapshot.isPuebloActive         = m_telnet.isPuebloActive();
	snapshot.removeMapReverses      = m_removeMapReverses;
	snapshot.notesInRgb             = m_notesInRgb;
	snapshot.disconnectOk           = m_disconnectOk;
	snapshot.traceEnabled           = m_traceEnabled;
	snapshot.isLogOpen              = m_logFile.isOpen();
	snapshot.scriptFileChanged      = m_scriptFileChanged;
	snapshot.worldFileModified      = m_worldFileModified;
	snapshot.isMapping              = m_isMapping;
	snapshot.isActive               = m_active;
	snapshot.outputFrozen           = m_outputFrozen;
	snapshot.variablesChanged       = m_variablesChanged;
	snapshot.doingSimulate          = m_doingSimulate;
	snapshot.lineOmittedFromOutput  = m_lineOmittedFromOutput;
	snapshot.hasLuaCallbacks        = m_luaCallbacks != nullptr;
	snapshot.pluginProcessingSent   = m_commandProcessor ? m_commandProcessor->pluginProcessingSent() : false;
	snapshot.isChatAcceptingCalls   = m_chatServer != nullptr;
	snapshot.noteStyle              = m_noteStyle;
	snapshot.noteTextColour         = m_noteTextColour;
	snapshot.noteColourBack         = noteColourBack;
	snapshot.noteColourFore         = noteColourFore;
	snapshot.backgroundColour       = m_backgroundColour;
	snapshot.utf8ErrorCount         = m_utf8ErrorCount;
	snapshot.triggersEvaluatedCount = m_triggersEvaluatedCount;
	snapshot.triggersMatchedThisSession = m_triggersMatchedThisSession;
	snapshot.aliasesEvaluatedCount      = m_aliasesEvaluatedCount;
	snapshot.aliasesMatchedThisSession  = m_aliasesMatchedThisSession;
	snapshot.timersFiredThisSession     = m_timersFiredThisSession;
	snapshot.lastLineWithIacGa          = m_lastLineWithIacGa;
	snapshot.outputWindowRedrawCount    = m_outputWindowRedrawCount;
	snapshot.currentActionSource        = static_cast<int>(m_currentActionSource);
	snapshot.newlinesReceived           = m_newlinesReceived;
	snapshot.connectTime                = m_connectTime;
	snapshot.statusTime                 = m_statusTime;
	snapshot.lastFlushTime              = m_lastFlushTime;
	snapshot.clientStartTime            = m_clientStartTime;
	snapshot.worldStartTime             = m_worldStartTime;
	if (includeStrings)
	{
		snapshot.scriptFileModTime        = scriptFileModTime();
		snapshot.logFileName              = m_logFileName;
		snapshot.lastImmediateExpression  = m_lastImmediateExpression;
		snapshot.statusMessage            = m_statusMessage;
		snapshot.worldFilePath            = m_worldFilePath;
		snapshot.windowTitleOverride      = m_windowTitleOverride;
		snapshot.mainTitleOverride        = m_mainTitleOverride;
		snapshot.defaultWorldDirectory    = m_defaultWorldDirectory;
		snapshot.defaultLogDirectory      = m_defaultLogDirectory;
		snapshot.pluginsDirectory         = m_pluginsDirectory;
		snapshot.peerAddressString        = m_socket ? m_socket->peerAddressString() : QString();
		snapshot.proxyAddressString       = m_proxyAddressString;
		snapshot.startupDirectory         = m_startupDirectory;
		snapshot.translatorFile           = m_translatorFile;
		snapshot.locale                   = m_locale;
		snapshot.fixedPitchFont           = m_fixedPitchFont;
		snapshot.lastTelnetSubnegotiation = m_lastTelnetSubnegotiation;
		snapshot.firstSpecialFontPath     = firstSpecialFontPath();
		snapshot.preferencesDatabaseName  = m_preferencesDatabaseName;
		snapshot.fileBrowsingDirectory    = m_fileBrowsingDirectory;
		snapshot.stateFilesDirectory      = m_stateFilesDirectory;
		snapshot.lastCommandSent          = m_lastCommandSent;
	}
	return snapshot;
}

QStringList WorldRuntime::queuedCommands() const
{
	if (QThread::currentThread() != thread())
		return commandUiSnapshot(false, false).queuedCommands;

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::queuedCommands");
	if (!m_commandProcessor)
		return {};
	return m_commandProcessor->queuedCommands();
}

QString WorldRuntime::commandInputText() const
{
	if (QThread::currentThread() != thread())
		return commandUiSnapshot(false, false).commandInputText;

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::commandInputText");
	return m_view ? m_view->inputText() : QString();
}

QStringList WorldRuntime::commandHistorySnapshot() const
{
	if (QThread::currentThread() != thread())
		return commandUiSnapshot(true, false).commandHistory;

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::commandHistorySnapshot");
	return m_view ? m_view->commandHistoryList() : QStringList();
}

int WorldRuntime::outputSelectionEndColumn() const
{
	if (QThread::currentThread() != thread())
		return commandUiSnapshot(false, false).outputSelectionEndColumn;

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::outputSelectionEndColumn");
	return m_view ? m_view->outputSelectionEndColumn() : 0;
}

int WorldRuntime::outputSelectionEndLine() const
{
	if (QThread::currentThread() != thread())
		return commandUiSnapshot(false, false).outputSelectionEndLine;

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::outputSelectionEndLine");
	return m_view ? m_view->outputSelectionEndLine() : 0;
}

int WorldRuntime::outputSelectionStartColumn() const
{
	if (QThread::currentThread() != thread())
		return commandUiSnapshot(false, false).outputSelectionStartColumn;

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::outputSelectionStartColumn");
	return m_view ? m_view->outputSelectionStartColumn() : 0;
}

int WorldRuntime::outputSelectionStartLine() const
{
	if (QThread::currentThread() != thread())
		return commandUiSnapshot(false, false).outputSelectionStartLine;

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::outputSelectionStartLine");
	return m_view ? m_view->outputSelectionStartLine() : 0;
}

int WorldRuntime::allocateAcceleratorCommand()
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, -1, [this] { return allocateAcceleratorCommand(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::allocateAcceleratorCommand");
	if (m_nextAcceleratorCommand >= (kAcceleratorFirstCommand + kAcceleratorCount))
		return -1;
	return m_nextAcceleratorCommand++;
}

void WorldRuntime::registerAccelerator(qint64 key, int commandId, const AcceleratorEntry &entry)
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this, key, commandId, entry]
		                        { registerAccelerator(key, commandId, entry); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::registerAccelerator");
	const auto existing = m_acceleratorKeyToCommand.constFind(key);
	if (existing != m_acceleratorKeyToCommand.constEnd())
	{
		const int previousCommandId = existing.value();
		if (previousCommandId == commandId)
		{
			const auto existingEntry = m_commandToAcceleratorEntry.constFind(commandId);
			if (existingEntry != m_commandToAcceleratorEntry.constEnd() &&
			    existingEntry->text == entry.text && existingEntry->sendTo == entry.sendTo &&
			    existingEntry->pluginId == entry.pluginId)
				return;
		}
		else
		{
			m_commandToAcceleratorEntry.remove(previousCommandId);
		}
	}
	m_acceleratorKeyToCommand[key]         = commandId;
	m_commandToAcceleratorEntry[commandId] = entry;
	emit acceleratorsChanged();
}

void WorldRuntime::removeAccelerator(qint64 key)
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this, key] { removeAccelerator(key); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::removeAccelerator");
	const auto it = m_acceleratorKeyToCommand.find(key);
	if (it == m_acceleratorKeyToCommand.end())
		return;
	m_commandToAcceleratorEntry.remove(it.value());
	m_acceleratorKeyToCommand.erase(it);
	emit acceleratorsChanged();
}

int WorldRuntime::acceleratorCommandForKey(qint64 key) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), -1,
		                          [this, key] { return acceleratorCommandForKey(key); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::acceleratorCommandForKey");
	const auto it = m_acceleratorKeyToCommand.constFind(key);
	return it == m_acceleratorKeyToCommand.constEnd() ? -1 : it.value();
}

bool WorldRuntime::hasAccelerator(int commandId) const
{
	return m_commandToAcceleratorEntry.contains(commandId);
}

const WorldRuntime::AcceleratorEntry *WorldRuntime::acceleratorEntryForCommand(int commandId) const
{
	const auto it = m_commandToAcceleratorEntry.constFind(commandId);
	if (it == m_commandToAcceleratorEntry.constEnd())
		return nullptr;
	return &it.value();
}

QVector<qint64> WorldRuntime::acceleratorKeys() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QVector<qint64>(),
		                          [this] { return acceleratorKeys(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::acceleratorKeys");
	QVector<qint64> keys;
	keys.reserve(m_acceleratorKeyToCommand.size());
	for (auto it = m_acceleratorKeyToCommand.constBegin(); it != m_acceleratorKeyToCommand.constEnd(); ++it)
		keys.append(it.key());
	return keys;
}

QVector<WorldRuntime::AcceleratorSnapshot> WorldRuntime::acceleratorSnapshot() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QVector<AcceleratorSnapshot>{},
		                          [this] { return acceleratorSnapshot(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::acceleratorSnapshot");
	QVector<AcceleratorSnapshot> snapshot;
	snapshot.reserve(m_acceleratorKeyToCommand.size());
	for (auto it = m_acceleratorKeyToCommand.constBegin(); it != m_acceleratorKeyToCommand.constEnd(); ++it)
	{
		AcceleratorSnapshot row;
		row.key       = it.key();
		row.commandId = it.value();
		if (const AcceleratorEntry *entry = acceleratorEntryForCommand(row.commandId))
		{
			row.text   = entry->text;
			row.sendTo = entry->sendTo;
		}
		snapshot.push_back(row);
	}
	return snapshot;
}

QString WorldRuntime::acceleratorCommandText(int commandId) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QString(),
		                          [this, commandId] { return acceleratorCommandText(commandId); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::acceleratorCommandText");
	if (const AcceleratorEntry *entry = acceleratorEntryForCommand(commandId))
		return entry->text;
	return {};
}

int WorldRuntime::acceleratorSendTarget(int commandId) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), 0,
		                          [this, commandId] { return acceleratorSendTarget(commandId); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::acceleratorSendTarget");
	if (const AcceleratorEntry *entry = acceleratorEntryForCommand(commandId))
		return entry->sendTo;
	return 0;
}

QString WorldRuntime::acceleratorPluginId(int commandId) const
{
	if (const AcceleratorEntry *entry = acceleratorEntryForCommand(commandId))
		return entry->pluginId;
	return {};
}

bool WorldRuntime::firePluginCommand(const QString &text)
{
	return callPluginCallbacksStopOnFalse(QStringLiteral("OnPluginCommand"), text);
}

void WorldRuntime::firePluginCommandChanged()
{
	callPluginCallbacksNoArgs(QStringLiteral("OnPluginCommandChanged"), false);
}

bool WorldRuntime::firePluginSend(const QString &text)
{
	return callPluginCallbacksStopOnFalse(QStringLiteral("OnPluginSend"), text);
}

void WorldRuntime::firePluginCommandEntered(QString &text)
{
	callPluginCallbacksTransformString(QStringLiteral("OnPluginCommandEntered"), text);
}

void WorldRuntime::firePluginTabComplete(QString &text)
{
	callPluginCallbacksTransformString(QStringLiteral("OnPluginTabComplete"), text);
	QMudNativePluginRegistry::handleMushReaderTabComplete(this, text);
}

bool WorldRuntime::firePluginLineReceived(const QString &text)
{
	return callPluginCallbacksStopOnFalse(QStringLiteral("OnPluginLineReceived"), text);
}

bool WorldRuntime::firePluginPlaySound(const QString &sound)
{
	return callPluginCallbacksStopOnTrueWithString(QStringLiteral("OnPluginPlaySound"), sound) ||
	       QMudNativePluginRegistry::handleLuaAudioPlaySound(this, sound);
}

bool WorldRuntime::firePluginTrace(const QString &message)
{
	return callPluginCallbacksStopOnTrueWithString(QStringLiteral("OnPluginTrace"), message);
}

void WorldRuntime::firePluginScreendraw(int type, int log, const QString &text)
{
	if (m_inScreendrawCallback)
		return;
	m_inScreendrawCallback = true;
	callPluginCallbacksWithTwoNumbersAndString(QStringLiteral("OnPluginScreendraw"), type, log, text, true);
	m_inScreendrawCallback = false;
	QMudNativePluginRegistry::handleMushReaderScreenDraw(this, type, log, text);
}

void WorldRuntime::firePluginTick()
{
	callPluginCallbacksNoArgs(QStringLiteral("OnPluginTick"), false);
}

void WorldRuntime::firePluginSent(const QString &text)
{
	callPluginCallbacks(QStringLiteral("OnPluginSent"), text, true);
}

void WorldRuntime::notifyNativePluginStateChanged()
{
	if (QThread::currentThread() != thread())
	{
		QMetaObject::invokeMethod(this, [this] { notifyNativePluginStateChanged(); }, Qt::QueuedConnection);
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::notifyNativePluginStateChanged");
	invalidatePluginCallbackPresenceCache();
	invalidateLuaCallbackDispatchSnapshot();
}

bool WorldRuntime::hasMushReaderLiveSpeechOwner() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), false,
		                          [this] { return hasMushReaderLiveSpeechOwner(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::hasMushReaderLiveSpeechOwner");
	const QString mushReaderId = QMudNativePluginRegistry::mushReaderPluginId();
	if (const int index = findPluginIndex(m_plugins, mushReaderId); index >= 0)
		return m_plugins.at(index).enabled;
	return QMudNativePluginRegistry::isMushReaderPluginEnabled(this);
}

bool WorldRuntime::isMushReaderSpeechEnabled() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), false,
		                          [this] { return isMushReaderSpeechEnabled(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::isMushReaderSpeechEnabled");
	return hasMushReaderLiveSpeechOwner() && QMudNativePluginRegistry::isMushReaderSpeechEnabled(this);
}

bool WorldRuntime::speakMushReaderReviewText(const QString &text) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), false,
		                          [this, text] { return speakMushReaderReviewText(text); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::speakMushReaderReviewText");
	return QMudNativePluginRegistry::speakMushReaderReviewText(this, text);
}

bool WorldRuntime::isQtAccessibilitySpeechEnabled() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), true,
		                          [this] { return isQtAccessibilitySpeechEnabled(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::isQtAccessibilitySpeechEnabled");
	return QMudNativePluginRegistry::isQtAccessibilitySpeechEnabled(this);
}

void WorldRuntime::firePluginPartialLine(const QString &text)
{
	callPluginCallbacks(QStringLiteral("OnPluginPartialLine"), text, true);
	QMudNativePluginRegistry::handleMushReaderPartialLine(this, text);
}

void WorldRuntime::notifyMiniWindowMouseMoved(int x, int y, const QString &windowName)
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodQueued(this,
		                       [this, x, y, windowName] { notifyMiniWindowMouseMoved(x, y, windowName); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::notifyMiniWindowMouseMoved");
	static const QString callbackName = QStringLiteral("OnPluginMouseMoved");
	if (!hasAnyPluginCallback(callbackName))
		return;

	m_pendingMiniWindowMouseX          = x;
	m_pendingMiniWindowMouseY          = y;
	m_pendingMiniWindowMouseWindowName = windowName;
	m_hasPendingMiniWindowMouseMoved   = true;
	if (m_pendingMiniWindowMouseMovedQueued)
		return;

	m_pendingMiniWindowMouseMovedQueued = true;
	const bool queued                   = QMetaObject::invokeMethod(
	    this,
	    [this]
	    {
		    qmudAssertObjectThreadAffinity(this, "WorldRuntime::notifyMiniWindowMouseMoved::flush");
		    flushPendingMiniWindowMouseMoved();
	    },
	    Qt::QueuedConnection);
	if (queued)
		return;

	m_pendingMiniWindowMouseMovedQueued = false;
	flushPendingMiniWindowMouseMoved();
}

void WorldRuntime::flushPendingMiniWindowMouseMoved()
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::flushPendingMiniWindowMouseMoved");
	m_pendingMiniWindowMouseMovedQueued = false;
	if (!m_hasPendingMiniWindowMouseMoved)
		return;

	const int     x                  = m_pendingMiniWindowMouseX;
	const int     y                  = m_pendingMiniWindowMouseY;
	const QString windowName         = m_pendingMiniWindowMouseWindowName;
	m_hasPendingMiniWindowMouseMoved = false;
	m_pendingMiniWindowMouseWindowName.clear();
	callPluginCallbacksWithTwoNumbersAndString(QStringLiteral("OnPluginMouseMoved"), x, y, windowName, false);
}

void WorldRuntime::notifyWorldOutputResized()
{
	updateTelnetWindowSizeForNaws();
	if (m_suppressWorldOutputResizedCallbacks > 0)
		return;
	callPluginCallbacksNoArgs(QStringLiteral("OnPluginWorldOutputResized"), false);
}

void WorldRuntime::refreshNawsWindowSize()
{
	updateTelnetWindowSizeForNaws();
}

void WorldRuntime::updateTelnetWindowSizeForNaws()
{
	const bool textRectangleCompatActive = m_textRectangle.left != 0 || m_textRectangle.top != 0 ||
	                                       m_textRectangle.right != 0 || m_textRectangle.bottom != 0;
	const bool nawsEnabled =
	    isEnabledFlag(m_worldAttributes.value(QStringLiteral("naws"))) && !textRectangleCompatActive;
	m_telnet.setNawsEnabled(nawsEnabled);
	const bool wrapEnabled     = isEnabledFlag(m_worldAttributes.value(QStringLiteral("wrap")));
	const int  worldWrapColumn = m_worldAttributes.value(QStringLiteral("wrap_column")).toInt();
	const int  minimumColumns  = wrapEnabled ? worldWrapColumn : 0;

	int        columns = 0;
	int        rows    = 0;

	if (m_view)
	{
		const QRect textRect     = textRectangleCompatActive ? m_view->outputTextRectangleUnreserved()
		                                                     : m_view->outputTextRectangle();
		int         widthPixels  = textRect.width();
		const int   heightPixels = textRect.height();
		if (widthPixels > 0 && heightPixels > 0)
		{
			const QFont        font = m_view->outputFont();
			const QFontMetrics metrics(font);
			const int          charWidth  = qMax(1, metrics.horizontalAdvance(QLatin1Char('x')));
			const int          charHeight = qMax(1, metrics.lineSpacing());
			columns                       = widthPixels / charWidth;
			rows                          = heightPixels / charHeight;
		}
	}

	if (columns <= 0 || rows <= 0)
	{
		// When geometry is not measurable yet (e.g. hidden/inactive view), keep
		// NAWS values protocol-valid using configured wrap or last known telnet size.
		columns = m_telnet.windowColumns();
		if (minimumColumns > 0)
			columns = qMax(columns, minimumColumns);
		rows = m_telnet.windowRows() > 0 ? m_telnet.windowRows() : 24;
	}
	else if (minimumColumns > 0)
		columns = qMax(columns, minimumColumns);

	if (columns > 0 && rows > 0)
		m_telnet.setWindowSize(columns, rows);

	if (m_socket && m_socket->isConnected())
	{
		const QByteArray outbound = m_telnet.takeOutboundData();
		if (!outbound.isEmpty())
			sendToWorld(outbound);
	}
}

void WorldRuntime::notifyDrawOutputWindow(int firstLine, int offset)
{
	if (m_inDrawOutputWindowCallback)
		return;
	++m_outputWindowRedrawCount;
	m_inDrawOutputWindowCallback = true;
	callPluginCallbacksWithTwoNumbersAndString(QStringLiteral("OnPluginDrawOutputWindow"), firstLine, offset,
	                                           QString(), false);
	m_inDrawOutputWindowCallback = false;
}

int WorldRuntime::outputWindowRedrawCount() const
{
	return m_outputWindowRedrawCount;
}

bool WorldRuntime::suppressScriptErrorOutputToWorld() const
{
	return m_inDrawOutputWindowCallback || m_inScreendrawCallback;
}

void WorldRuntime::notifyOutputSelectionChanged()
{
	callPluginCallbacksNoArgs(QStringLiteral("OnPluginSelectionChanged"), false);
}

static void setDefault(QMap<QString, QString> &attributes, const QString &key, const QString &value)
{
	if (!attributes.contains(key))
		attributes.insert(key, value);
}

static void applyTriggerDefaults(WorldRuntime::Trigger &trigger)
{
	QMap<QString, QString> &a = trigger.attributes;
	setDefault(a, QStringLiteral("enabled"), QStringLiteral("1"));
	setDefault(a, QStringLiteral("ignore_case"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("omit_from_log"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("omit_from_output"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("keep_evaluating"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("expand_variables"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("send_to"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("regexp"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("repeat"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("sequence"), QStringLiteral("100"));
	setDefault(a, QStringLiteral("sound_if_inactive"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("lowercase_wildcard"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("temporary"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("user"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("one_shot"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("lines_to_match"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("colour_change_type"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("match_back_colour"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("match_text_colour"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("match_bold"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("match_italic"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("match_underline"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("match_inverse"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("bold"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("italic"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("underline"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("inverse"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("make_bold"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("make_italic"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("make_underline"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("custom_colour"), QStringLiteral("0"));
}

static void applyAliasDefaults(WorldRuntime::Alias &alias)
{
	QMap<QString, QString> &a = alias.attributes;
	setDefault(a, QStringLiteral("enabled"), QStringLiteral("1"));
	setDefault(a, QStringLiteral("ignore_case"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("expand_variables"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("omit_from_log"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("regexp"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("omit_from_output"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("sequence"), QStringLiteral("100"));
	setDefault(a, QStringLiteral("keep_evaluating"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("menu"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("send_to"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("echo_alias"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("omit_from_command_history"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("user"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("one_shot"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("temporary"), QStringLiteral("0"));
}

static void applyTimerDefaults(WorldRuntime::Timer &timer)
{
	QMap<QString, QString> &a = timer.attributes;
	setDefault(a, QStringLiteral("enabled"), QStringLiteral("1"));
	setDefault(a, QStringLiteral("one_shot"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("active_closed"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("send_to"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("omit_from_output"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("omit_from_log"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("at_time"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("hour"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("minute"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("second"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("offset_hour"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("offset_minute"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("offset_second"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("user"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("temporary"), QStringLiteral("0"));
}

static void applyVariableDefaults(WorldRuntime::Variable &variable)
{
	QMap<QString, QString> &a = variable.attributes;
	setDefault(a, QStringLiteral("trim"), QStringLiteral("0"));
}

static void applyPrintingDefaults(WorldRuntime::PrintingStyle &style)
{
	QMap<QString, QString> &a = style.attributes;
	setDefault(a, QStringLiteral("bold"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("italic"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("underline"), QStringLiteral("0"));
}

static QStringList macroDescriptionList()
{
	return {QStringLiteral("up"),        QStringLiteral("down"),      QStringLiteral("north"),
	        QStringLiteral("south"),     QStringLiteral("east"),      QStringLiteral("west"),
	        QStringLiteral("examine"),   QStringLiteral("look"),      QStringLiteral("page"),
	        QStringLiteral("say"),       QStringLiteral("whisper"),   QStringLiteral("doing"),
	        QStringLiteral("who"),       QStringLiteral("drop"),      QStringLiteral("take"),
	        QStringLiteral("F2"),        QStringLiteral("F3"),        QStringLiteral("F4"),
	        QStringLiteral("F5"),        QStringLiteral("F7"),        QStringLiteral("F8"),
	        QStringLiteral("F9"),        QStringLiteral("F10"),       QStringLiteral("F11"),
	        QStringLiteral("F12"),       QStringLiteral("F2+Shift"),  QStringLiteral("F3+Shift"),
	        QStringLiteral("F4+Shift"),  QStringLiteral("F5+Shift"),  QStringLiteral("F6+Shift"),
	        QStringLiteral("F7+Shift"),  QStringLiteral("F8+Shift"),  QStringLiteral("F9+Shift"),
	        QStringLiteral("F10+Shift"), QStringLiteral("F11+Shift"), QStringLiteral("F12+Shift"),
	        QStringLiteral("F2+Ctrl"),   QStringLiteral("F3+Ctrl"),   QStringLiteral("F5+Ctrl"),
	        QStringLiteral("F7+Ctrl"),   QStringLiteral("F8+Ctrl"),   QStringLiteral("F9+Ctrl"),
	        QStringLiteral("F10+Ctrl"),  QStringLiteral("F11+Ctrl"),  QStringLiteral("F12+Ctrl"),
	        QStringLiteral("logout"),    QStringLiteral("quit"),      QStringLiteral("Alt+A"),
	        QStringLiteral("Alt+B"),     QStringLiteral("Alt+J"),     QStringLiteral("Alt+K"),
	        QStringLiteral("Alt+L"),     QStringLiteral("Alt+M"),     QStringLiteral("Alt+N"),
	        QStringLiteral("Alt+O"),     QStringLiteral("Alt+P"),     QStringLiteral("Alt+Q"),
	        QStringLiteral("Alt+R"),     QStringLiteral("Alt+S"),     QStringLiteral("Alt+T"),
	        QStringLiteral("Alt+U"),     QStringLiteral("Alt+X"),     QStringLiteral("Alt+Y"),
	        QStringLiteral("Alt+Z"),     QStringLiteral("F1"),        QStringLiteral("F1+Ctrl"),
	        QStringLiteral("F1+Shift"),  QStringLiteral("F6"),        QStringLiteral("F6+Ctrl")};
}

static QList<WorldRuntime::Macro> normalizeMacroSlots(const QList<WorldRuntime::Macro> &macros)
{
	QMap<QString, WorldRuntime::Macro> macroByName;
	for (const auto &macro : macros)
	{
		const QString name = macro.attributes.value(QStringLiteral("name")).trimmed();
		if (name.isEmpty())
			continue;
		const QString key = name.toLower();
		if (macroByName.contains(key))
			continue;
		WorldRuntime::Macro normalized = macro;
		normalized.attributes.insert(QStringLiteral("name"), name);
		macroByName.insert(key, normalized);
	}

	const QStringList          macroNames = macroDescriptionList();
	QList<WorldRuntime::Macro> normalizedMacros;
	normalizedMacros.reserve(macroNames.size());
	for (int i = 0; i < macroNames.size(); ++i)
	{
		const QString      &canonicalName = macroNames.at(i);
		const QString       key           = canonicalName.toLower();
		const auto          it            = macroByName.constFind(key);

		WorldRuntime::Macro macro;
		if (it != macroByName.cend())
			macro = it.value();
		else
			macro.attributes.insert(QStringLiteral("type"), QStringLiteral("send_now"));

		macro.attributes.insert(QStringLiteral("name"), canonicalName);
		macro.attributes.insert(QStringLiteral("index"), QString::number(i));
		normalizedMacros.push_back(macro);
	}

	return normalizedMacros;
}

static QStringList keypadNameList()
{
	return {QStringLiteral("0"),      QStringLiteral("1"),      QStringLiteral("2"),
	        QStringLiteral("3"),      QStringLiteral("4"),      QStringLiteral("5"),
	        QStringLiteral("6"),      QStringLiteral("7"),      QStringLiteral("8"),
	        QStringLiteral("9"),      QStringLiteral("."),      QStringLiteral("/"),
	        QStringLiteral("*"),      QStringLiteral("-"),      QStringLiteral("+"),
	        QStringLiteral("Ctrl+0"), QStringLiteral("Ctrl+1"), QStringLiteral("Ctrl+2"),
	        QStringLiteral("Ctrl+3"), QStringLiteral("Ctrl+4"), QStringLiteral("Ctrl+5"),
	        QStringLiteral("Ctrl+6"), QStringLiteral("Ctrl+7"), QStringLiteral("Ctrl+8"),
	        QStringLiteral("Ctrl+9"), QStringLiteral("Ctrl+."), QStringLiteral("Ctrl+/"),
	        QStringLiteral("Ctrl+*"), QStringLiteral("Ctrl+-"), QStringLiteral("Ctrl++")};
}

void WorldRuntime::applyFromDocument(const WorldDocument &doc)
{
	m_loadingDocument        = true;
	const QString workingDir = resolveWorkingDir(m_startupDirectory);
	m_worldAttributes        = doc.worldAttributes();
	for (auto it = m_worldAttributes.begin(); it != m_worldAttributes.end(); ++it)
	{
		if (isCanonicalizablePathAttributeName(it.key()) &&
		    it.value().trimmed() != QString::fromLatin1(kNoSoundLiteral))
			it.value() = canonicalizePathForRuntime(it.value(), workingDir);
		else if (isLikelyPathAttributeName(it.key()))
			it.value() = normalizePathForRuntime(it.value());
		if (it.key() == QStringLiteral("auto_log_file_name"))
			it.value() = normalizeAutoLogFileNameValue(it.value());
	}
	m_worldMultilineAttributes = doc.worldMultilineAttributes();

	// Legacy behavior: world files store password base64-encoded when password_base64 is enabled.
	// Decode on load so runtime attributes always hold clear text for connect/send paths.
	if (isEnabledFlag(m_worldAttributes.value(QStringLiteral("password_base64"))))
	{
		const QString encodedPassword = m_worldAttributes.value(QStringLiteral("password"));
		if (!encodedPassword.isEmpty())
		{
			const QByteArray decoded =
			    QByteArray::fromBase64(encodedPassword.toLatin1(), QByteArray::Base64Encoding);
			if (!decoded.isEmpty())
				m_worldAttributes.insert(QStringLiteral("password"), QString::fromUtf8(decoded));
		}
	}

	applyDefaultWorldOptions();
	m_traceEnabled      = isEnabledFlag(m_worldAttributes.value(QStringLiteral("trace")));
	m_worldFileModified = false;
	m_variablesChanged  = false;
	m_scriptFileChanged = false;
	m_worldFileVersion  = doc.worldFileVersion();
	m_qmudVersion       = doc.qmudVersion();
	m_dateSaved         = doc.dateSaved();
	m_comments          = doc.comments();

	if (m_worldMultilineAttributes.contains(QStringLiteral("script")))
		setLuaScriptText(m_worldMultilineAttributes.value(QStringLiteral("script")));

	m_triggerCount       = safeQSizeToInt(doc.triggers().size());
	m_aliasCount         = safeQSizeToInt(doc.aliases().size());
	m_timerCount         = safeQSizeToInt(doc.timers().size());
	m_variableCount      = safeQSizeToInt(doc.variables().size());
	m_colourCount        = safeQSizeToInt(doc.colours().size());
	m_keypadCount        = safeQSizeToInt(doc.keypadEntries().size());
	m_printingStyleCount = safeQSizeToInt(doc.printingStyles().size());
	m_pluginCount        = safeQSizeToInt(doc.plugins().size());
	m_includeCount       = safeQSizeToInt(doc.includes().size());
	m_scriptCount        = safeQSizeToInt(doc.scripts().size());
	m_connectPhase       = eConnectNotConnected;
	m_connectViaProxy    = false;
	m_proxyAddressString.clear();
	m_proxyAddressV4 = 0;
	m_connectTime    = QDateTime();
	m_recentLines.clear();
	invalidateLuaCallbackLineBufferSnapshot();
	m_stopTriggerEvaluation = KeepEvaluating;

	m_triggers.clear();
	for (const auto &t : doc.triggers())
	{
		Trigger rt;
		rt.attributes = t.attributes;
		rt.children   = t.children;
		rt.included   = t.included;
		applyTriggerDefaults(rt);
		m_triggers.push_back(rt);
	}
	m_aliases.clear();
	for (const auto &a : doc.aliases())
	{
		Alias ra;
		ra.attributes = a.attributes;
		ra.children   = a.children;
		ra.included   = a.included;
		applyAliasDefaults(ra);
		m_aliases.push_back(ra);
	}
	m_timers.clear();
	for (const auto &t : doc.timers())
	{
		Timer rt;
		rt.attributes = t.attributes;
		rt.children   = t.children;
		rt.included   = t.included;
		applyTimerDefaults(rt);
		resetTimerFields(rt);
		m_timers.push_back(rt);
	}
	QList<Macro> parsedMacros;
	parsedMacros.reserve(doc.macros().size());
	for (const auto &m : doc.macros())
	{
		Macro rm;
		rm.attributes = m.attributes;
		rm.children   = m.children;
		parsedMacros.push_back(rm);
	}
	m_macros     = normalizeMacroSlots(parsedMacros);
	m_macroCount = safeQSizeToInt(m_macros.size());
	m_variables.clear();
	for (const auto &v : doc.variables())
	{
		Variable rv;
		rv.attributes = v.attributes;
		rv.content    = v.content;
		applyVariableDefaults(rv);
		m_variables.push_back(rv);
	}
	m_colours.clear();
	for (const auto &c : doc.colours())
	{
		Colour rc;
		rc.group      = c.group;
		rc.attributes = c.attributes;
		bool      ok  = false;
		const int seq = rc.attributes.value(QStringLiteral("seq")).toInt(&ok);
		if (ok)
			rc.attributes.insert(QStringLiteral("seq_index"), QString::number(seq - 1));
		m_colours.push_back(rc);
	}
	setNoteTextColour(
	    publicNoteColourIndexFromWorldAttribute(m_worldAttributes.value(QStringLiteral("note_text_colour")),
	                                            m_colours, QMudNoteColour::kDefaultPublicIndex));
	invalidateResolvedOutputColourCache();
	m_keypadEntries.clear();
	const QStringList keypadNames = keypadNameList();
	for (const auto &k : doc.keypadEntries())
	{
		Keypad rk;
		rk.attributes         = k.attributes;
		rk.content            = k.content;
		const QString keyName = rk.attributes.value(QStringLiteral("name")).trimmed();
		const int     index   = boundedQSizeToInt(keypadNames.indexOf(keyName));
		if (index >= 0)
			rk.attributes.insert(QStringLiteral("index"), QString::number(index));
		m_keypadEntries.push_back(rk);
	}
	m_printingStyles.clear();
	for (const auto &p : doc.printingStyles())
	{
		PrintingStyle rp;
		rp.group      = p.group;
		rp.attributes = p.attributes;
		bool      ok  = false;
		const int seq = rp.attributes.value(QStringLiteral("seq")).toInt(&ok);
		if (ok)
			rp.attributes.insert(QStringLiteral("seq_index"), QString::number(seq - 1));
		applyPrintingDefaults(rp);
		m_printingStyles.push_back(rp);
	}
	QVector<QSharedPointer<LuaCallbackEngine>> pluginEnginesToTeardown;
	pluginEnginesToTeardown.reserve(m_plugins.size());
	for (auto &plugin : m_plugins)
	{
		if (!plugin.lua)
			continue;
		pluginEnginesToTeardown.push_back(plugin.lua);
	}
	cancelSuspendedPluginCallbackDispatchesForEngines(pluginEnginesToTeardown);
	for (auto &plugin : m_plugins)
		plugin.lua.clear();
	dispatchTeardownLuaEngines(pluginEnginesToTeardown, false);
	QMudNativePluginRegistry::luaAudioResetRuntime(this);
	m_plugins.clear();
	resetObservedPluginCallbackTracking(m_observedPluginCallbacks, m_observedPluginCallbackQueryGeneration,
	                                    m_observedPluginCallbackGeneration);
	m_observedPluginCallbacksPendingWarmup.clear();
	m_observedPluginCallbackPropagationInFlight = false;
	m_observedPluginCallbackPropagationQueued   = false;
	{
		QMutexLocker locker(&m_pluginObservedCallbackPresenceMutex);
		m_pendingPluginObservedCallbackPresenceById.clear();
		m_pendingPluginLuaFunctionCatalogById.clear();
	}
	m_pluginObservedCallbackPresenceById.clear();
	m_pluginLuaFunctionCatalogById.clear();
	m_pluginCallbackPresenceCounts.clear();
	m_pluginCallbackRecipientIndices.clear();
	m_pluginCallbackPresencePluginCount = -1;
	invalidatePluginCallbackPresenceCache();
	QMudNativePluginRegistry::setMushReaderPluginEnabled(this, false);
	QMudNativePluginRegistry::setQtAccessibilitySpeechEnabled(this, true);
	QVector<LuaEngineObservedInitializationRequest> pluginLuaInitRequests;
	pluginLuaInitRequests.reserve(safeQSizeToInt(doc.plugins().size()));
	for (const auto &p : doc.plugins())
	{
		Plugin rp;
		rp.attributes             = p.attributes;
		rp.description            = p.description;
		rp.script                 = p.script;
		rp.source                 = rp.attributes.value(QStringLiteral("source"));
		rp.directory              = rp.attributes.value(QStringLiteral("directory"));
		const QString pluginId    = rp.attributes.value(QStringLiteral("id")).trimmed().toLower();
		rp.sequence               = pluginSequenceFromAttributes(rp.attributes);
		rp.version                = rp.attributes.value(QStringLiteral("version")).toDouble();
		rp.requiredVersion        = rp.attributes.value(QStringLiteral("requires")).toDouble();
		rp.dateWritten            = parsePluginDate(rp.attributes.value(QStringLiteral("date_written")));
		rp.dateModified           = parsePluginDate(rp.attributes.value(QStringLiteral("date_modified")));
		rp.dateInstalled          = QDateTime::currentDateTime();
		rp.global                 = isEnabledFlag(rp.attributes.value(QStringLiteral("global")));
		rp.saveState              = isEnabledFlag(rp.attributes.value(QStringLiteral("save_state")));
		const QString enabledFlag = rp.attributes.value(QStringLiteral("enabled"));
		const bool    requestedEnabled = enabledFlag.isEmpty() ? true : isEnabledFlag(enabledFlag);
		rp.enabled                     = requestedEnabled;
		if (QMudNativePluginRegistry::isBlacklistedId(pluginId))
			continue;
		if (QMudNativePluginRegistry::isShimId(pluginId))
		{
			QMudNativePluginRegistry::NativePluginMetadata metadata;
			if (!QMudNativePluginRegistry::metadataForShim(pluginId, metadata))
				continue;
			Plugin shadow  = makeNativeShimShadowPlugin(metadata, rp.global);
			shadow.enabled = requestedEnabled;
			shadow.attributes.insert(QStringLiteral("enabled"),
			                         requestedEnabled ? QStringLiteral("1") : QStringLiteral("0"));
			if (findPluginIndex(m_plugins, metadata.id) < 0)
				m_plugins.push_back(shadow);
			if (pluginId == QMudNativePluginRegistry::mushReaderPluginId())
				QMudNativePluginRegistry::setMushReaderPluginEnabled(this, requestedEnabled);
			continue;
		}
		for (const auto &t : p.triggers)
		{
			Trigger rt;
			rt.attributes                 = t.attributes;
			rt.children                   = t.children;
			rt.included                   = t.included;
			const bool hasExplicitEnabled = rt.attributes.contains(QStringLiteral("enabled"));
			applyTriggerDefaults(rt);
			if (!hasExplicitEnabled)
				rt.attributes.insert(QStringLiteral("enabled"), QStringLiteral("0"));
			rp.triggers.push_back(rt);
		}
		for (const auto &a : p.aliases)
		{
			Alias ra;
			ra.attributes                 = a.attributes;
			ra.children                   = a.children;
			ra.included                   = a.included;
			const bool hasExplicitEnabled = ra.attributes.contains(QStringLiteral("enabled"));
			applyAliasDefaults(ra);
			if (!hasExplicitEnabled)
				ra.attributes.insert(QStringLiteral("enabled"), QStringLiteral("0"));
			rp.aliases.push_back(ra);
		}
		for (const auto &t : p.timers)
		{
			Timer rt;
			rt.attributes                 = t.attributes;
			rt.children                   = t.children;
			rt.included                   = t.included;
			const bool hasExplicitEnabled = rt.attributes.contains(QStringLiteral("enabled"));
			applyTimerDefaults(rt);
			if (!hasExplicitEnabled)
				rt.attributes.insert(QStringLiteral("enabled"), QStringLiteral("0"));
			resetTimerFields(rt);
			rp.timers.push_back(rt);
		}
		for (const auto &v : p.variables)
		{
			const QString name = v.attributes.value(QStringLiteral("name"));
			if (!name.isEmpty())
				rp.variables.insert(name, v.content);
		}
		// Preserve load-path behavior: world plugins pick up persisted state variables on world load.
		if (!m_stateFilesDirectory.isEmpty())
		{
			const QString worldId = m_worldAttributes.value(QStringLiteral("id")).trimmed();
			if (!worldId.isEmpty() && !pluginId.isEmpty())
			{
				QString base = m_stateFilesDirectory;
				if (!base.endsWith('/') && !base.endsWith('\\'))
					base += '/';
				const QString   stateFile = base + worldId + "-" + pluginId + "-state.xml";
				QFileInfo const stateInfo(stateFile);
				if (stateInfo.exists() && stateInfo.size() > 0)
				{
					WorldDocument stateDoc;
					stateDoc.setLoadMask(WorldDocument::XML_VARIABLES | WorldDocument::XML_NO_PLUGINS);
					if (stateDoc.loadFromFile(stateInfo.absoluteFilePath()))
					{
						for (const auto &v : stateDoc.variables())
						{
							const QString name = v.attributes.value(QStringLiteral("name"));
							if (!name.isEmpty())
								rp.variables.insert(name, v.content);
						}
					}
					else
					{
						qWarning() << "Plugin state load failed:" << stateDoc.errorString();
					}
				}
			}
		}
		const QString language = rp.attributes.value(QStringLiteral("language"));
		if (language.compare(QStringLiteral("lua"), Qt::CaseInsensitive) == 0)
		{
			rp.lua = QSharedPointer<LuaCallbackEngine>::create();
			LuaEngineObservedInitializationRequest initRequest;
			initRequest.engine          = rp.lua.data();
			initRequest.runtime         = this;
			initRequest.scriptText      = rp.script;
			initRequest.pluginId        = rp.attributes.value(QStringLiteral("id"));
			initRequest.pluginName      = rp.attributes.value(QStringLiteral("name"));
			initRequest.pluginDirectory = rp.directory;
			initRequest.callbackNames   = m_observedPluginCallbacks;
			initRequest.observer = [this](const QString &pluginId, const QSet<QString> &presentCallbacks,
			                              const QSet<QString> &luaFunctions)
			{ recordObservedPluginCallbackPresenceSnapshot(pluginId, presentCallbacks, luaFunctions); };
			pluginLuaInitRequests.push_back(std::move(initRequest));
		}
		if (!requestedEnabled && rp.lua && hasValidPluginId(rp))
		{
			rp.enabled             = true;
			rp.disableAfterInstall = true;
		}
		m_plugins.push_back(rp);
	}
	if (!pluginLuaInitRequests.isEmpty())
	{
		dispatchInitializeLuaEnginesWithObservedCallbacks(pluginLuaInitRequests, true);
		invalidatePluginCallbackPresenceCache();
	}
	sortPluginsBySequence();
	m_pluginCount = safeQSizeToInt(m_plugins.size());
	invalidatePluginCallbackPresenceCache();
	invalidateLuaCallbackDispatchSnapshot();
	markTriggerRulesChanged();
	for (auto &plugin : m_plugins)
	{
		queuePluginInstall(plugin);
	}
	m_includes.clear();
	QHash<QString, bool> uniqueNonPluginIncludes;
	for (const auto &i : doc.includes())
	{
		Include ri;
		ri.attributes = i.attributes;
		if (isEnabledFlag(ri.attributes.value(QStringLiteral("plugin"))))
		{
			QMudNativePluginRegistry::NativePluginMetadata metadata;
			if (QMudNativePluginRegistry::metadataForNativeSource(ri.attributes.value(QStringLiteral("name")),
			                                                      metadata))
			{
				ri.attributes.insert(QStringLiteral("name"), metadata.source);
			}
		}
		else
		{
			const QString includeName =
			    normalizePathForStorage(ri.attributes.value(QStringLiteral("name")).trimmed());
			if (includeName.isEmpty())
				continue;
			const QString includeKey = includeName.toLower();
			if (uniqueNonPluginIncludes.contains(includeKey))
				continue;
			uniqueNonPluginIncludes.insert(includeKey, true);
			ri.attributes.insert(QStringLiteral("name"), includeName);
		}
		m_includes.push_back(ri);
	}
	m_includeCount = safeQSizeToInt(m_includes.size());
	m_scripts.clear();
	for (const auto &s : doc.scripts())
	{
		Script rs;
		rs.content = s.content;
		m_scripts.push_back(rs);
	}
	const QString scriptFile = m_worldAttributes.value(QStringLiteral("script_filename"));
	if (m_scriptWatcher)
	{
		const QStringList watched = m_scriptWatcher->files();
		if (!watched.isEmpty())
			m_scriptWatcher->removePaths(watched);
		if (!scriptFile.isEmpty())
		{
			const QString scriptWorkingDir = resolveWorkingDir(m_startupDirectory);
			const QString path             = makeAbsolutePath(scriptFile, scriptWorkingDir);
			if (!path.isEmpty())
				m_scriptWatcher->addPath(path);
		}
	}
	m_loadingDocument = false;
	refreshCommandProcessorOptions();
	installPendingPlugins();
}

const QMap<QString, QString> &WorldRuntime::worldAttributes() const
{
	return m_worldAttributes;
}

QString WorldRuntime::worldAttributeValue(const QString &key) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QString(),
		                          [this, key] { return worldAttributeValue(key); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::worldAttributeValue");
	return m_worldAttributes.value(key);
}

void WorldRuntime::setWorldAttribute(const QString &key, const QString &value)
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this, key, value] { setWorldAttribute(key, value); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::setWorldAttribute");
	QString normalizedValue = value;
	if (isLikelyPathAttributeName(key))
		normalizedValue = normalizePathForRuntime(normalizedValue);
	if (key == QStringLiteral("auto_log_file_name"))
		normalizedValue = normalizeAutoLogFileNameValue(normalizedValue);
	if (key == QStringLiteral("note_text_colour"))
	{
		const int fallbackIndex = QMudNoteColour::publicIndexFromRuntimeIndex(m_noteTextColour);
		setNoteTextColour(publicNoteColourIndexFromWorldAttribute(normalizedValue, m_colours, fallbackIndex));
	}
	const auto existing = m_worldAttributes.constFind(key);
	if (existing != m_worldAttributes.constEnd() && existing.value() == normalizedValue)
	{
		return;
	}
	m_worldAttributes.insert(key, normalizedValue);
	if (worldAttributeAffectsResolvedOutputColours(key))
		invalidateResolvedOutputColourCache();
	invalidateLuaCallbackDispatchSnapshot();
	if (key == QStringLiteral("max_output_lines"))
		static_cast<void>(enforceOutputLineLimit());
	if (key == QStringLiteral("naws"))
	{
		updateTelnetWindowSizeForNaws();
		if (m_view)
			m_view->applyRuntimeSettingsWithoutOutputRebuild();
	}
	if (!m_loadingDocument && worldAttributeAffectsCommandProcessor(key))
		refreshCommandProcessorOptions();
	if (!m_loadingDocument)
		m_worldFileModified = true;
	emit worldAttributeChanged(key);
	if (key == QStringLiteral("script_filename") && m_scriptWatcher)
	{
		const QStringList watched = m_scriptWatcher->files();
		if (!watched.isEmpty())
			m_scriptWatcher->removePaths(watched);
		if (!normalizedValue.isEmpty())
		{
			const QString workingDir = resolveWorkingDir(m_startupDirectory);
			const QString path       = makeAbsolutePath(normalizedValue, workingDir);
			if (!path.isEmpty())
				m_scriptWatcher->addPath(path);
		}
	}
}

const QMap<QString, QString> &WorldRuntime::worldMultilineAttributes() const
{
	return m_worldMultilineAttributes;
}

QString WorldRuntime::worldMultilineAttributeValue(const QString &key) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QString(),
		                          [this, key] { return worldMultilineAttributeValue(key); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::worldMultilineAttributeValue");
	return m_worldMultilineAttributes.value(key);
}

void WorldRuntime::setWorldMultilineAttribute(const QString &key, const QString &value)
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this, key, value] { setWorldMultilineAttribute(key, value); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::setWorldMultilineAttribute");
	if (!upsertWorldMultilineAttributeIfChanged(m_worldMultilineAttributes, key, value))
		return;
	invalidateLuaCallbackDispatchSnapshot();
	if (!m_loadingDocument)
		m_worldFileModified = true;
	if (key == QStringLiteral("script"))
		setLuaScriptText(value);
}

int WorldRuntime::worldFileVersion() const
{
	return m_worldFileVersion;
}

QString WorldRuntime::qmudVersion() const
{
	return m_qmudVersion;
}

QDateTime WorldRuntime::dateSaved() const
{
	return m_dateSaved;
}

int WorldRuntime::triggerCount() const
{
	return m_triggerCount;
}

int WorldRuntime::aliasCount() const
{
	return m_aliasCount;
}

int WorldRuntime::timerCount() const
{
	return m_timerCount;
}

int WorldRuntime::macroCount() const
{
	return m_macroCount;
}

int WorldRuntime::variableCount() const
{
	return m_variableCount;
}

int WorldRuntime::colourCount() const
{
	return m_colourCount;
}

int WorldRuntime::keypadCount() const
{
	return m_keypadCount;
}

int WorldRuntime::printingStyleCount() const
{
	return m_printingStyleCount;
}

int WorldRuntime::pluginCount() const
{
	return m_pluginCount;
}

int WorldRuntime::includeCount() const
{
	return m_includeCount;
}

int WorldRuntime::scriptCount() const
{
	return m_scriptCount;
}

const QList<WorldRuntime::Trigger> &WorldRuntime::triggers() const
{
	return m_triggers;
}

quint64 WorldRuntime::triggerRuleGeneration() const
{
	return m_triggerRuleGeneration;
}

QList<WorldRuntime::Trigger> &WorldRuntime::triggersMutable()
{
	return m_triggers;
}

void WorldRuntime::setTriggers(const QList<Trigger> &triggers)
{
	m_triggers.clear();
	for (const auto &t : triggers)
	{
		Trigger rt = t;
		applyTriggerDefaults(rt);
		m_triggers.push_back(rt);
	}
	m_triggerCount      = safeQSizeToInt(m_triggers.size());
	m_worldFileModified = true;
	invalidateLuaCallbackDispatchSnapshot();
	markTriggerRulesChanged();
}

void WorldRuntime::markTriggersChanged()
{
	m_triggerCount      = safeQSizeToInt(m_triggers.size());
	m_worldFileModified = true;
	invalidateLuaCallbackDispatchSnapshot();
	markTriggerRulesChanged();
}

void WorldRuntime::markTriggerRulesChanged()
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this] { markTriggerRulesChanged(); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::markTriggerRulesChanged");
	++m_triggerRuleGeneration;
}

const QList<WorldRuntime::Alias> &WorldRuntime::aliases() const
{
	return m_aliases;
}

QList<WorldRuntime::Alias> &WorldRuntime::aliasesMutable()
{
	return m_aliases;
}

void WorldRuntime::setAliases(const QList<Alias> &aliases)
{
	m_aliases.clear();
	for (const auto &a : aliases)
	{
		Alias ra = a;
		applyAliasDefaults(ra);
		m_aliases.push_back(ra);
	}
	m_aliasCount        = safeQSizeToInt(m_aliases.size());
	m_worldFileModified = true;
	invalidateLuaCallbackDispatchSnapshot();
}

void WorldRuntime::markAliasesChanged()
{
	m_aliasCount        = safeQSizeToInt(m_aliases.size());
	m_worldFileModified = true;
	invalidateLuaCallbackDispatchSnapshot();
}

const QList<WorldRuntime::Timer> &WorldRuntime::timers() const
{
	return m_timers;
}

QList<WorldRuntime::Timer> &WorldRuntime::timersMutable()
{
	return m_timers;
}

void WorldRuntime::setTimers(const QList<Timer> &timers)
{
	m_timers.clear();
	for (const auto &t : timers)
	{
		Timer rt = t;
		applyTimerDefaults(rt);
		m_timers.push_back(rt);
	}
	m_timerCount = safeQSizeToInt(m_timers.size());
	noteTimerStructureMutation();
	m_worldFileModified = true;
	invalidateLuaCallbackDispatchSnapshot();
}

void WorldRuntime::markTimersChanged()
{
	m_timerCount        = safeQSizeToInt(m_timers.size());
	m_worldFileModified = true;
	invalidateLuaCallbackDispatchSnapshot();
}

quint64 WorldRuntime::timerStructureMutationSerial() const
{
	return m_timerStructureMutationSerial;
}

void WorldRuntime::noteTimerStructureMutation()
{
	++m_timerStructureMutationSerial;
}

const QList<WorldRuntime::Macro> &WorldRuntime::macros() const
{
	return m_macros;
}

void WorldRuntime::setMacros(const QList<Macro> &macros)
{
	m_macros            = normalizeMacroSlots(macros);
	m_macroCount        = safeQSizeToInt(m_macros.size());
	m_worldFileModified = true;
	invalidateLuaCallbackDispatchSnapshot();
}

const QList<WorldRuntime::Variable> &WorldRuntime::variables() const
{
	return m_variables;
}

bool WorldRuntime::findVariable(const QString &name, QString &value) const
{
	if (QThread::currentThread() != thread())
	{
		const auto resolved =
		    qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), std::pair<bool, QString>{false, {}},
		                       [this, name]
		                       {
			                       QString    resolvedValue;
			                       const bool found = findVariable(name, resolvedValue);
			                       return std::pair<bool, QString>{found, resolvedValue};
		                       });
		if (resolved.first)
			value = resolved.second;
		return resolved.first;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::findVariable");
	for (const auto &var : m_variables)
	{
		const QString varName = var.attributes.value(QStringLiteral("name"));
		if (varName.compare(name, Qt::CaseInsensitive) == 0)
		{
			value = var.content;
			return true;
		}
	}
	return false;
}

QStringList WorldRuntime::variableList() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QStringList(),
		                          [this] { return variableList(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::variableList");
	QStringList names;
	names.reserve(m_variables.size());
	for (const auto &var : m_variables)
	{
		const QString name = var.attributes.value(QStringLiteral("name"));
		if (!name.isEmpty())
			names.append(name);
	}
	return names;
}

QMap<QString, QString> WorldRuntime::variableSnapshot() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QMap<QString, QString>{},
		                          [this] { return variableSnapshot(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::variableSnapshot");
	QMap<QString, QString> snapshot;
	for (const auto &var : m_variables)
	{
		const QString name = var.attributes.value(QStringLiteral("name"));
		if (name.isEmpty())
			continue;
		snapshot.insert(name, var.content);
	}
	return snapshot;
}

void WorldRuntime::setVariable(const QString &name, const QString &value)
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this, name, value] { setVariable(name, value); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::setVariable");
	for (auto &var : m_variables)
	{
		const QString varName = var.attributes.value(QStringLiteral("name"));
		if (varName.compare(name, Qt::CaseInsensitive) == 0)
		{
			var.content        = value;
			m_variablesChanged = true;
			invalidateLuaCallbackDispatchSnapshot();
			return;
		}
	}

	Variable variable;
	variable.attributes.insert(QStringLiteral("name"), name);
	variable.content = value;
	m_variables.push_back(variable);
	m_variablesChanged = true;
	invalidateLuaCallbackDispatchSnapshot();
}

void WorldRuntime::setVariables(const QList<Variable> &variables)
{
	m_variables = variables;
	for (auto &var : m_variables)
		applyVariableDefaults(var);
	m_variableCount    = safeQSizeToInt(m_variables.size());
	m_variablesChanged = true;
	invalidateLuaCallbackDispatchSnapshot();
}

int WorldRuntime::arrayCreate(const QString &name)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eArrayDoesNotExist, [this, name] { return arrayCreate(name); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::arrayCreate");
	if (m_arrays.contains(name))
		return eArrayAlreadyExists;
	ArrayEntry const entry;
	m_arrays.insert(name, entry);
	invalidateLuaCallbackDispatchSnapshot();
	return eOK;
}

int WorldRuntime::arrayDelete(const QString &name)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eArrayDoesNotExist, [this, name] { return arrayDelete(name); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::arrayDelete");
	const auto it = m_arrays.find(name);
	if (it == m_arrays.end())
		return eArrayDoesNotExist;
	m_arrays.erase(it);
	invalidateLuaCallbackDispatchSnapshot();
	return eOK;
}

int WorldRuntime::arrayClear(const QString &name)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eArrayDoesNotExist, [this, name] { return arrayClear(name); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::arrayClear");
	const auto it = m_arrays.find(name);
	if (it == m_arrays.end())
		return eArrayDoesNotExist;
	it->values.clear();
	invalidateLuaCallbackDispatchSnapshot();
	return eOK;
}

bool WorldRuntime::arrayExists(const QString &name) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodTrueOnSuccess(const_cast<WorldRuntime *>(this),
		                                     [this, name] { return arrayExists(name); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::arrayExists");
	return m_arrays.contains(name);
}

int WorldRuntime::arrayCount() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), 0, [this] { return arrayCount(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::arrayCount");
	return safeQSizeToInt(m_arrays.size());
}

int WorldRuntime::arraySize(const QString &name) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), 0,
		                          [this, name] { return arraySize(name); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::arraySize");
	const auto it = m_arrays.find(name);
	if (it == m_arrays.end())
		return 0;
	return safeQSizeToInt(it->values.size());
}

bool WorldRuntime::arrayKeyExists(const QString &name, const QString &key) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodTrueOnSuccess(const_cast<WorldRuntime *>(this),
		                                     [this, name, key] { return arrayKeyExists(name, key); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::arrayKeyExists");
	const auto it = m_arrays.find(name);
	if (it == m_arrays.end())
		return false;
	return it->values.contains(key);
}

bool WorldRuntime::arrayGet(const QString &name, const QString &key, QString &value) const
{
	if (QThread::currentThread() != thread())
	{
		const auto resolved =
		    qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), std::pair<bool, QString>{false, {}},
		                       [this, name, key]
		                       {
			                       QString    localValue;
			                       const bool found = arrayGet(name, key, localValue);
			                       return std::pair<bool, QString>{found, localValue};
		                       });
		if (resolved.first)
			value = resolved.second;
		return resolved.first;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::arrayGet");
	const auto it = m_arrays.find(name);
	if (it == m_arrays.end())
		return false;
	const auto kv = it->values.find(key);
	if (kv == it->values.end())
		return false;
	value = kv.value();
	return true;
}

int WorldRuntime::arraySet(const QString &name, const QString &key, const QString &value)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eArrayDoesNotExist,
		                          [this, name, key, value] { return arraySet(name, key, value); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::arraySet");
	const auto it = m_arrays.find(name);
	if (it == m_arrays.end())
		return eArrayDoesNotExist;
	if (it->values.contains(key))
	{
		it->values.insert(key, value);
		invalidateLuaCallbackDispatchSnapshot();
		return eSetReplacingExistingValue;
	}
	it->values.insert(key, value);
	invalidateLuaCallbackDispatchSnapshot();
	return eOK;
}

int WorldRuntime::arrayDeleteKey(const QString &name, const QString &key)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eArrayDoesNotExist,
		                          [this, name, key] { return arrayDeleteKey(name, key); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::arrayDeleteKey");
	const auto it = m_arrays.find(name);
	if (it == m_arrays.end())
		return eArrayDoesNotExist;
	const auto kv = it->values.find(key);
	if (kv == it->values.end())
		return eKeyDoesNotExist;
	it->values.erase(kv);
	invalidateLuaCallbackDispatchSnapshot();
	return eOK;
}

bool WorldRuntime::arrayFirstKey(const QString &name, QString &key) const
{
	if (QThread::currentThread() != thread())
	{
		const auto resolved =
		    qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), std::pair<bool, QString>{false, {}},
		                       [this, name]
		                       {
			                       QString    localKey;
			                       const bool found = arrayFirstKey(name, localKey);
			                       return std::pair<bool, QString>{found, localKey};
		                       });
		if (resolved.first)
			key = resolved.second;
		return resolved.first;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::arrayFirstKey");
	const auto it = m_arrays.find(name);
	if (it == m_arrays.end() || it->values.isEmpty())
		return false;
	key = it->values.firstKey();
	return true;
}

bool WorldRuntime::arrayLastKey(const QString &name, QString &key) const
{
	if (QThread::currentThread() != thread())
	{
		const auto resolved =
		    qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), std::pair<bool, QString>{false, {}},
		                       [this, name]
		                       {
			                       QString    localKey;
			                       const bool found = arrayLastKey(name, localKey);
			                       return std::pair<bool, QString>{found, localKey};
		                       });
		if (resolved.first)
			key = resolved.second;
		return resolved.first;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::arrayLastKey");
	const auto it = m_arrays.find(name);
	if (it == m_arrays.end() || it->values.isEmpty())
		return false;
	key = it->values.lastKey();
	return true;
}

QStringList WorldRuntime::arrayListAll() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QStringList{},
		                          [this] { return arrayListAll(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::arrayListAll");
	return m_arrays.keys();
}

QStringList WorldRuntime::arrayListKeys(const QString &name) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QStringList{},
		                          [this, name] { return arrayListKeys(name); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::arrayListKeys");
	const auto it = m_arrays.find(name);
	if (it == m_arrays.end())
		return {};
	return it->values.keys();
}

QStringList WorldRuntime::arrayListValues(const QString &name) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QStringList{},
		                          [this, name] { return arrayListValues(name); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::arrayListValues");
	const auto it = m_arrays.find(name);
	if (it == m_arrays.end())
		return {};
	return it->values.values();
}

namespace
{
	int mapSqlErrorToSqlite(const QSqlError &err)
	{
		if (!err.isValid())
			return SQLITE_OK;
		const QString text = err.text().toLower();
		if (text.contains(QStringLiteral("locked")) || text.contains(QStringLiteral("busy")))
			return SQLITE_BUSY;
		if (text.contains(QStringLiteral("readonly")))
			return SQLITE_READONLY;
		return SQLITE_ERROR;
	}

	int applySqliteWalAndNormalSynchronous(const QSqlDatabase &db, QString &errorMessage)
	{
		if (!db.isValid() || !db.isOpen())
		{
			errorMessage = QStringLiteral("Database connection is not open.");
			return SQLITE_ERROR;
		}

		QSqlQuery query(db);
		if (!query.exec(QStringLiteral("PRAGMA journal_mode=WAL")))
		{
			errorMessage = query.lastError().text();
			return mapSqlErrorToSqlite(query.lastError());
		}

		const QString mode = query.next() ? query.value(0).toString().trimmed() : QString();
		if (mode.compare(QStringLiteral("wal"), Qt::CaseInsensitive) != 0)
		{
			errorMessage = QStringLiteral("PRAGMA journal_mode returned '%1' instead of 'wal'.").arg(mode);
			return SQLITE_ERROR;
		}

		if (!query.exec(QStringLiteral("PRAGMA synchronous=NORMAL")))
		{
			errorMessage = query.lastError().text();
			return mapSqlErrorToSqlite(query.lastError());
		}

		return SQLITE_OK;
	}

	int mapVariantTypeToSqlite(const QVariant &value)
	{
		if (value.isNull())
			return SQLITE_NULL;
		switch (value.typeId())
		{
		case QMetaType::Int:
		case QMetaType::UInt:
		case QMetaType::LongLong:
		case QMetaType::ULongLong:
		case QMetaType::Short:
		case QMetaType::UShort:
		case QMetaType::Char:
		case QMetaType::UChar:
			return SQLITE_INTEGER;
		case QMetaType::Double:
		case QMetaType::Float:
			return SQLITE_FLOAT;
		case QMetaType::QByteArray:
			return SQLITE_BLOB;
		default:
			return SQLITE_TEXT;
		}
	}

	using SqliteExecCallback = int (*)(void *, int, char **, char **);
	using SqliteExecFn       = int (*)(sqlite3 *, const char *, SqliteExecCallback, void *, char **);
	using SqliteFreeFn       = void (*)(void *);
	using SqliteErrmsgFn     = const char *(*)(sqlite3 *);

	struct NativeSqliteApi
	{
			QLibrary       library;
			SqliteExecFn   exec{nullptr};
			SqliteFreeFn   free{nullptr};
			SqliteErrmsgFn errmsg{nullptr};
			QString        errorMessage;

			NativeSqliteApi() : library(QStringLiteral("sqlite3"))
			{
				if (!library.load())
				{
					library.setFileName(QStringLiteral("libsqlite3.so.0"));
					library.load();
				}

				if (!library.isLoaded())
				{
					errorMessage = library.errorString();
					return;
				}

				exec   = reinterpret_cast<SqliteExecFn>(library.resolve("sqlite3_exec"));
				free   = reinterpret_cast<SqliteFreeFn>(library.resolve("sqlite3_free"));
				errmsg = reinterpret_cast<SqliteErrmsgFn>(library.resolve("sqlite3_errmsg"));
				if (!exec || !free || !errmsg)
					errorMessage = QStringLiteral("Required SQLite entry points are unavailable.");
			}

			[[nodiscard]] bool isAvailable() const
			{
				return exec && free && errmsg;
			}
	};

	const NativeSqliteApi &nativeSqliteApi()
	{
		static const NativeSqliteApi api;
		return api;
	}

	sqlite3 *nativeSqliteHandle(const QSqlDatabase &db)
	{
		QSqlDriver *driver = db.driver();
		if (!driver)
			return nullptr;
		const QVariant handle = driver->handle();
		if (!handle.isValid() || qstrcmp(handle.typeName(), "sqlite3*") != 0)
			return nullptr;
		return *static_cast<sqlite3 *const *>(handle.constData());
	}
} // namespace

int WorldRuntime::databaseOpen(const QString &name, const QString &filename, int flags)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, SQLITE_ERROR, [this, name, filename, flags]
		                          { return databaseOpen(name, filename, flags); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::databaseOpen");
	const bool isMemory = (flags & SQLITE_OPEN_MEMORY) != 0 || filename == QStringLiteral(":memory:");
	QString    diskName = filename;
	if (!isMemory)
	{
		QString error;
		if (!QMudPluginPathUtils::resolveInsideQmudHome(m_startupDirectory, filename, &diskName, &error))
			return SQLITE_CANTOPEN;
	}
	const auto it = m_databases.find(name);
	if (it != m_databases.end())
	{
		if (it->diskName == diskName)
			return SQLITE_OK;
		return kDbErrorDatabaseAlreadyExists;
	}

	if (!isMemory && (flags & SQLITE_OPEN_CREATE) == 0 && !QFileInfo::exists(diskName))
		return SQLITE_CANTOPEN;

	static std::atomic<quint64> connectionCounter{0};
	DatabaseEntry               entry;
	entry.diskName = diskName;
	entry.connectionName =
	    QStringLiteral("world_db_%1").arg(connectionCounter.fetch_add(1, std::memory_order_relaxed) + 1);
	entry.db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), entry.connectionName);
	entry.db.setDatabaseName(isMemory ? QStringLiteral(":memory:") : diskName);
	if (flags & SQLITE_OPEN_READONLY)
		entry.db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
	if (!entry.db.open())
	{
		entry.lastError        = mapSqlErrorToSqlite(entry.db.lastError());
		entry.lastErrorMessage = entry.db.lastError().text();
		entry.db               = QSqlDatabase();
		QSqlDatabase::removeDatabase(entry.connectionName);
		return entry.lastError;
	}

	if (!isMemory && (flags & SQLITE_OPEN_READONLY) == 0)
	{
		entry.lastError = applySqliteWalAndNormalSynchronous(entry.db, entry.lastErrorMessage);
		if (entry.lastError != SQLITE_OK)
		{
			entry.db.close();
			entry.db = QSqlDatabase();
			QSqlDatabase::removeDatabase(entry.connectionName);
			return entry.lastError;
		}
	}

	m_databases.insert(name, entry);
	invalidateLuaCallbackDispatchSnapshot();
	return SQLITE_OK;
}

int WorldRuntime::databaseClose(const QString &name)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, kDbErrorIdNotFound, [this, name] { return databaseClose(name); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::databaseClose");
	const auto it = m_databases.find(name);
	if (it == m_databases.end())
		return kDbErrorIdNotFound;
	if (!it->db.isValid() || !it->db.isOpen())
		return kDbErrorNotOpen;
	if (it->stmtPrepared)
	{
		it->stmt->finish();
		it->stmtPrepared = false;
		it->stmt.clear();
	}
	it->db.close();
	const QString connectionName = it->connectionName;
	m_databases.erase(it);
	QSqlDatabase::removeDatabase(connectionName);
	invalidateLuaCallbackDispatchSnapshot();
	return SQLITE_OK;
}

int WorldRuntime::databasePrepare(const QString &name, const QString &sql)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, kDbErrorIdNotFound,
		                          [this, name, sql] { return databasePrepare(name, sql); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::databasePrepare");
	const auto it = m_databases.find(name);
	if (it == m_databases.end())
		return kDbErrorIdNotFound;
	if (!it->db.isValid() || !it->db.isOpen())
		return kDbErrorNotOpen;
	if (it->stmtPrepared)
		return kDbErrorHavePreparedStatement;

	it->validRow = false;
	it->columns  = 0;
	it->stmt     = QSharedPointer<QSqlQuery>::create(it->db);
	if (!it->stmt->prepare(sql))
	{
		it->lastError        = mapSqlErrorToSqlite(it->stmt->lastError());
		it->lastErrorMessage = it->stmt->lastError().text();
		return it->lastError;
	}
	it->stmtPrepared = true;
	it->stmtExecuted = false;
	it->lastError    = SQLITE_OK;
	it->lastErrorMessage.clear();
	it->columns = it->stmt->record().count();
	invalidateLuaCallbackDispatchSnapshot();
	return SQLITE_OK;
}

int WorldRuntime::databaseFinalize(const QString &name)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, kDbErrorIdNotFound, [this, name] { return databaseFinalize(name); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::databaseFinalize");
	const auto it = m_databases.find(name);
	if (it == m_databases.end())
		return kDbErrorIdNotFound;
	if (!it->db.isValid() || !it->db.isOpen())
		return kDbErrorNotOpen;
	if (!it->stmtPrepared)
		return kDbErrorNoPreparedStatement;

	it->stmt->finish();
	it->stmt.clear();
	it->stmtPrepared = false;
	it->stmtExecuted = false;
	it->validRow     = false;
	it->columns      = 0;
	invalidateLuaCallbackDispatchSnapshot();
	return SQLITE_OK;
}

int WorldRuntime::databaseReset(const QString &name)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, kDbErrorIdNotFound, [this, name] { return databaseReset(name); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::databaseReset");
	const auto it = m_databases.find(name);
	if (it == m_databases.end())
		return kDbErrorIdNotFound;
	if (!it->db.isValid() || !it->db.isOpen())
		return kDbErrorNotOpen;
	if (!it->stmtPrepared)
		return kDbErrorNoPreparedStatement;
	it->stmt->finish();
	it->stmtExecuted = false;
	it->validRow     = false;
	invalidateLuaCallbackDispatchSnapshot();
	return SQLITE_OK;
}

int WorldRuntime::databaseColumns(const QString &name) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), kDbErrorIdNotFound,
		                          [this, name] { return databaseColumns(name); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::databaseColumns");
	const auto it = m_databases.find(name);
	if (it == m_databases.end())
		return kDbErrorIdNotFound;
	if (!it->db.isValid() || !it->db.isOpen())
		return kDbErrorNotOpen;
	if (!it->stmtPrepared)
		return kDbErrorNoPreparedStatement;
	return it->columns;
}

int WorldRuntime::databaseStep(const QString &name)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, kDbErrorIdNotFound, [this, name] { return databaseStep(name); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::databaseStep");
	const auto it = m_databases.find(name);
	if (it == m_databases.end())
		return kDbErrorIdNotFound;
	if (!it->db.isValid() || !it->db.isOpen())
		return kDbErrorNotOpen;
	if (!it->stmtPrepared)
		return kDbErrorNoPreparedStatement;

	if (!it->stmtExecuted)
	{
		if (!it->stmt->exec())
		{
			it->lastError        = mapSqlErrorToSqlite(it->stmt->lastError());
			it->lastErrorMessage = it->stmt->lastError().text();
			it->validRow         = false;
			return it->lastError;
		}
		it->stmtExecuted = true;
		it->columns      = it->stmt->record().count();
	}

	if (it->stmt->next())
	{
		it->validRow  = true;
		it->lastError = SQLITE_ROW;
		invalidateLuaCallbackDispatchSnapshot();
		return SQLITE_ROW;
	}

	it->validRow  = false;
	it->lastError = SQLITE_DONE;
	invalidateLuaCallbackDispatchSnapshot();
	return SQLITE_DONE;
}

QString WorldRuntime::databaseError(const QString &name) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QStringLiteral("database error"),
		                          [this, name] { return databaseError(name); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::databaseError");
	const auto it = m_databases.find(name);
	if (it == m_databases.end())
		return QStringLiteral("database id not found");
	if (!it->db.isValid() || !it->db.isOpen())
		return QStringLiteral("database not open");

	switch (it->lastError)
	{
	case SQLITE_OK:
		return QStringLiteral("not an error");
	case SQLITE_ROW:
		return QStringLiteral("row ready");
	case SQLITE_DONE:
		return QStringLiteral("finished");
	case kDbErrorIdNotFound:
		return QStringLiteral("database id not found");
	case kDbErrorNotOpen:
		return QStringLiteral("database not open");
	case kDbErrorHavePreparedStatement:
		return QStringLiteral("already have prepared statement");
	case kDbErrorNoPreparedStatement:
		return QStringLiteral("do not have prepared statement");
	case kDbErrorNoValidRow:
		return QStringLiteral("do not have a valid row");
	case kDbErrorDatabaseAlreadyExists:
		return QStringLiteral("database already exists under a different disk name");
	case kDbErrorColumnOutOfRange:
		return QStringLiteral("column count out of valid range");
	default:
		return it->lastErrorMessage.isEmpty() ? QStringLiteral("database error") : it->lastErrorMessage;
	}
}

QString WorldRuntime::databaseColumnName(const QString &name, int column) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QString(),
		                          [this, name, column] { return databaseColumnName(name, column); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::databaseColumnName");
	const auto it = m_databases.find(name);
	if (it == m_databases.end() || !it->db.isValid() || !it->db.isOpen() || !it->stmtPrepared || column < 1 ||
	    column > it->columns)
		return {};
	return it->stmt->record().fieldName(column - 1);
}

QString WorldRuntime::databaseColumnText(const QString &name, int column, bool *ok) const
{
	if (ok)
		*ok = false;
	if (QThread::currentThread() != thread())
	{
		const auto resolved =
		    qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), std::pair<QString, bool>{{}, false},
		                       [this, name, column]
		                       {
			                       bool    localOk = false;
			                       QString value   = databaseColumnText(name, column, &localOk);
			                       return std::pair<QString, bool>{value, localOk};
		                       });
		if (ok)
			*ok = resolved.second;
		return resolved.first;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::databaseColumnText");
	const auto it = m_databases.find(name);
	if (it == m_databases.end() || !it->db.isValid() || !it->db.isOpen() || !it->stmtPrepared ||
	    !it->validRow || column < 1 || column > it->columns)
		return {};
	const QVariant value = it->stmt->value(column - 1);
	if (value.isNull())
		return {};
	if (ok)
		*ok = true;
	return value.toString();
}

bool WorldRuntime::databaseColumnValue(const QString &name, int column, QVariant &value) const
{
	if (QThread::currentThread() != thread())
	{
		const auto resolved =
		    qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), std::pair<bool, QVariant>{false, QVariant()},
		                       [this, name, column]
		                       {
			                       QVariant   localValue;
			                       const bool found = databaseColumnValue(name, column, localValue);
			                       return std::pair<bool, QVariant>{found, localValue};
		                       });
		if (resolved.first)
			value = resolved.second;
		return resolved.first;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::databaseColumnValue");
	const auto it = m_databases.find(name);
	if (it == m_databases.end() || !it->db.isValid() || !it->db.isOpen() || !it->stmtPrepared ||
	    !it->validRow || column < 1 || column > it->columns)
		return false;

	value = it->stmt->value(column - 1);
	return true;
}

int WorldRuntime::databaseColumnType(const QString &name, int column) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), kDbErrorIdNotFound,
		                          [this, name, column] { return databaseColumnType(name, column); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::databaseColumnType");
	const auto it = m_databases.find(name);
	if (it == m_databases.end())
		return kDbErrorIdNotFound;
	if (!it->db.isValid() || !it->db.isOpen())
		return kDbErrorNotOpen;
	if (!it->stmtPrepared)
		return kDbErrorNoPreparedStatement;
	if (!it->validRow)
		return kDbErrorNoValidRow;
	if (column < 1 || column > it->columns)
		return kDbErrorColumnOutOfRange;
	return mapVariantTypeToSqlite(it->stmt->value(column - 1));
}

int WorldRuntime::databaseTotalChanges(const QString &name) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), kDbErrorIdNotFound,
		                          [this, name] { return databaseTotalChanges(name); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::databaseTotalChanges");
	const auto it = m_databases.find(name);
	if (it == m_databases.end())
		return kDbErrorIdNotFound;
	if (!it->db.isValid() || !it->db.isOpen())
		return kDbErrorNotOpen;
	QSqlQuery query(it->db);
	if (!query.exec(QStringLiteral("SELECT total_changes()")) || !query.next())
		return 0;
	return query.value(0).toInt();
}

int WorldRuntime::databaseChanges(const QString &name) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), kDbErrorIdNotFound,
		                          [this, name] { return databaseChanges(name); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::databaseChanges");
	const auto it = m_databases.find(name);
	if (it == m_databases.end())
		return kDbErrorIdNotFound;
	if (!it->db.isValid() || !it->db.isOpen())
		return kDbErrorNotOpen;
	QSqlQuery query(it->db);
	if (!query.exec(QStringLiteral("SELECT changes()")) || !query.next())
		return 0;
	return query.value(0).toInt();
}

QString WorldRuntime::databaseLastInsertRowid(const QString &name) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QString(),
		                          [this, name] { return databaseLastInsertRowid(name); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::databaseLastInsertRowid");
	const auto it = m_databases.find(name);
	if (it == m_databases.end() || !it->db.isValid() || !it->db.isOpen())
		return {};
	QSqlQuery query(it->db);
	if (!query.exec(QStringLiteral("SELECT last_insert_rowid()")) || !query.next())
		return {};
	return query.value(0).toString();
}

QStringList WorldRuntime::databaseList() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QStringList{},
		                          [this] { return databaseList(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::databaseList");
	return m_databases.keys();
}

QVariant WorldRuntime::databaseInfo(const QString &name, int infoType) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QVariant(),
		                          [this, name, infoType] { return databaseInfo(name, infoType); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::databaseInfo");
	const auto it = m_databases.find(name);
	if (it == m_databases.end())
		return {};

	switch (infoType)
	{
	case 1:
		return it->diskName;
	case 2:
		return it->stmtPrepared;
	case 3:
		return it->validRow;
	case 4:
		return it->columns;
	default:
		return {};
	}
}

int WorldRuntime::databaseExec(const QString &name, const QString &sql)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, kDbErrorIdNotFound,
		                          [this, name, sql] { return databaseExec(name, sql); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::databaseExec");
	const auto it = m_databases.find(name);
	if (it == m_databases.end())
		return kDbErrorIdNotFound;
	if (!it->db.isValid() || !it->db.isOpen())
		return kDbErrorNotOpen;
	if (it->stmtPrepared)
		return kDbErrorHavePreparedStatement;

	it->validRow = false;
	it->columns  = 0;

	const NativeSqliteApi &api    = nativeSqliteApi();
	sqlite3               *handle = nativeSqliteHandle(it->db);
	if (!api.isAvailable() || !handle)
	{
		QSqlQuery query(it->db);
		if (query.exec(sql))
		{
			it->lastError = SQLITE_OK;
			it->lastErrorMessage.clear();
			return SQLITE_OK;
		}
		it->lastError        = mapSqlErrorToSqlite(query.lastError());
		it->lastErrorMessage = api.errorMessage.isEmpty() ? query.lastError().text() : api.errorMessage;
		return it->lastError;
	}

	char            *errorMessage = nullptr;
	const QByteArray sqlBytes     = sql.toUtf8();
	const int        result       = api.exec(handle, sqlBytes.constData(), nullptr, nullptr, &errorMessage);
	it->lastError                 = result;
	if (result == SQLITE_OK)
	{
		it->lastErrorMessage.clear();
		return SQLITE_OK;
	}

	if (errorMessage)
	{
		it->lastErrorMessage = QString::fromUtf8(errorMessage);
		api.free(errorMessage);
	}
	else
	{
		it->lastErrorMessage = QString::fromUtf8(api.errmsg(handle));
	}
	return result;
}

QStringList WorldRuntime::databaseColumnNames(const QString &name) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QStringList{},
		                          [this, name] { return databaseColumnNames(name); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::databaseColumnNames");
	const auto it = m_databases.find(name);
	if (it == m_databases.end() || !it->db.isValid() || !it->db.isOpen() || !it->stmtPrepared)
		return {};
	QStringList      names;
	const QSqlRecord record = it->stmt->record();
	for (int i = 0; i < it->columns; ++i)
		names.append(record.fieldName(i));
	return names;
}

bool WorldRuntime::databaseColumnValues(const QString &name, QVector<QVariant> &values) const
{
	values.clear();
	if (QThread::currentThread() != thread())
	{
		const auto resolved = qmudInvokeMethodOr(
		    const_cast<WorldRuntime *>(this), std::pair<bool, QVector<QVariant>>{false, {}},
		    [this, name]
		    {
			    QVector<QVariant> localValues;
			    const bool        found = databaseColumnValues(name, localValues);
			    return std::pair<bool, QVector<QVariant>>{found, localValues};
		    });
		if (resolved.first)
			values = resolved.second;
		return resolved.first;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::databaseColumnValues");
	const auto it = m_databases.find(name);
	if (it == m_databases.end() || !it->db.isValid() || !it->db.isOpen() || !it->stmtPrepared ||
	    !it->validRow)
		return false;
	values.reserve(it->columns);
	for (int i = 0; i < it->columns; ++i)
		values.push_back(it->stmt->value(i));
	return true;
}

const QList<WorldRuntime::Colour> &WorldRuntime::colours() const
{
	return m_colours;
}

void WorldRuntime::setColours(const QList<Colour> &colours)
{
	m_colours           = colours;
	m_colourCount       = safeQSizeToInt(m_colours.size());
	m_worldFileModified = true;
	invalidateResolvedOutputColourCache();
	invalidateLuaCallbackDispatchSnapshot();
}

namespace
{
	QColor parseColorValueRuntime(const QString &value)
	{
		return parseColourValue(value);
	}

	QVector<QColor> defaultAnsiColours(bool bold)
	{
		QVector<QColor> colours(8);
		if (bold)
		{
			colours[0] = QColor(128, 128, 128);
			colours[1] = QColor(255, 0, 0);
			colours[2] = QColor(0, 255, 0);
			colours[3] = QColor(255, 255, 0);
			colours[4] = QColor(0, 0, 255);
			colours[5] = QColor(255, 0, 255);
			colours[6] = QColor(0, 255, 255);
			colours[7] = QColor(255, 255, 255);
		}
		else
		{
			colours[0] = QColor(0, 0, 0);
			colours[1] = QColor(128, 0, 0);
			colours[2] = QColor(0, 128, 0);
			colours[3] = QColor(128, 128, 0);
			colours[4] = QColor(0, 0, 128);
			colours[5] = QColor(128, 0, 128);
			colours[6] = QColor(0, 128, 128);
			colours[7] = QColor(192, 192, 192);
		}
		return colours;
	}

} // namespace

QColor WorldRuntime::ansiColour(bool bold, int index) const
{
	if (index < 1 || index > 8)
		return {};

	const QString targetGroup = bold ? QStringLiteral("ansi/bold") : QStringLiteral("ansi/normal");
	for (const auto &colour : m_colours)
	{
		if (colour.group.trimmed().compare(targetGroup, Qt::CaseInsensitive) != 0)
			continue;
		const int seq = colourSeqFromAttributes(colour.attributes);
		if (seq != index)
			continue;
		const QColor rgb = parseColorValueRuntime(colour.attributes.value(QStringLiteral("rgb")));
		if (rgb.isValid())
			return rgb;
	}

	return defaultAnsiColours(bold).value(index - 1);
}

void WorldRuntime::setAnsiColour(bool bold, int index, const QColor &color)
{
	if (index < 1 || index > 8 || !color.isValid())
		return;

	const QString targetGroup = bold ? QStringLiteral("ansi/bold") : QStringLiteral("ansi/normal");
	const QString rgb         = color.name(QColor::HexRgb);

	for (auto &colour : m_colours)
	{
		if (colour.group.trimmed().compare(targetGroup, Qt::CaseInsensitive) != 0)
			continue;
		const int seq = colourSeqFromAttributes(colour.attributes);
		if (seq != index)
			continue;
		colour.attributes.insert(QStringLiteral("seq"), QString::number(index));
		colour.attributes.insert(QStringLiteral("seq_index"), QString::number(index - 1));
		colour.attributes.insert(QStringLiteral("rgb"), rgb);
		m_worldFileModified = true;
		invalidateResolvedOutputColourCache();
		invalidateLuaCallbackDispatchSnapshot();
		return;
	}

	Colour entry;
	entry.group = targetGroup;
	entry.attributes.insert(QStringLiteral("seq"), QString::number(index));
	entry.attributes.insert(QStringLiteral("seq_index"), QString::number(index - 1));
	entry.attributes.insert(QStringLiteral("rgb"), rgb);
	m_colours.push_back(entry);
	m_colourCount       = safeQSizeToInt(m_colours.size());
	m_worldFileModified = true;
	invalidateResolvedOutputColourCache();
	invalidateLuaCallbackDispatchSnapshot();
}

const QList<WorldRuntime::Keypad> &WorldRuntime::keypadEntries() const
{
	return m_keypadEntries;
}

void WorldRuntime::setKeypadEntries(const QList<Keypad> &entries)
{
	m_keypadEntries     = entries;
	m_keypadCount       = safeQSizeToInt(m_keypadEntries.size());
	m_worldFileModified = true;
	invalidateLuaCallbackDispatchSnapshot();
}

const QList<WorldRuntime::PrintingStyle> &WorldRuntime::printingStyles() const
{
	return m_printingStyles;
}

void WorldRuntime::setPrintingStyles(const QList<PrintingStyle> &styles)
{
	m_printingStyles     = styles;
	m_printingStyleCount = safeQSizeToInt(m_printingStyles.size());
	m_worldFileModified  = true;
}

const QList<WorldRuntime::Plugin> &WorldRuntime::plugins() const
{
	return m_plugins;
}

QList<WorldRuntime::Plugin> &WorldRuntime::pluginsMutable()
{
	return m_plugins;
}

bool WorldRuntime::loadPluginFile(const QString &fileName, QString *error, bool markGlobal)
{
	if (QThread::currentThread() != thread())
	{
		const auto resolved = qmudInvokeMethodOr(this, std::pair<bool, QString>{false, {}},
		                                         [this, fileName, markGlobal, needsError = (error != nullptr)]
		                                         {
			                                         QString    localError;
			                                         QString   *errorPtr = needsError ? &localError : nullptr;
			                                         const bool loaded =
			                                             loadPluginFile(fileName, errorPtr, markGlobal);
			                                         return std::pair<bool, QString>{loaded, localError};
		                                         });
		if (error)
			*error = resolved.second;
		return resolved.first;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::loadPluginFile");
	const QString raw = fileName.trimmed();
	if (raw.isEmpty())
	{
		if (error)
			*error = QStringLiteral("No plugin file specified");
		return false;
	}

	const QString pluginsRelative =
	    QMudPluginPathUtils::qmudHomeRelativePath(m_startupDirectory, m_pluginsDirectory, true);
	const QString rawRelative = QMudPluginPathUtils::legacyPathRelativeToQmudHome(raw);
	QStringList   candidates;
	if (!rawRelative.isEmpty() && !pluginsRelative.isEmpty() && pluginsRelative != QLatin1String("./") &&
	    !rawRelative.startsWith(QStringLiteral("worlds/"), Qt::CaseInsensitive) &&
	    !rawRelative.startsWith(QStringLiteral("plugins/"), Qt::CaseInsensitive))
		candidates.push_back(QDir::cleanPath(QDir(pluginsRelative).filePath(rawRelative)));
	candidates.push_back(raw);

	QString resolved;
	for (const QString &candidate : candidates)
	{
		QString errorMessage;
		if (!QMudPluginPathUtils::resolveInsideQmudHome(m_startupDirectory, candidate, &resolved,
		                                                &errorMessage))
			continue;
		if (QFileInfo::exists(resolved))
			break;
		resolved.clear();
	}
	QFileInfo info(resolved);

	if (!info.exists())
	{
		if (error)
			*error = QStringLiteral("Plugin file not found: %1").arg(resolved);
		return false;
	}

	WorldDocument           doc;
	constexpr unsigned long mask = WorldDocument::XML_TRIGGERS | WorldDocument::XML_ALIASES |
	                               WorldDocument::XML_TIMERS | WorldDocument::XML_VARIABLES |
	                               WorldDocument::XML_PLUGINS | WorldDocument::XML_INCLUDES;
	doc.setLoadMask(mask);
	if (!doc.loadFromPluginFile(resolved))
	{
		if (error)
			*error = doc.errorString();
		return false;
	}

	QString pluginsDir;
	QString pluginsDirError;
	static_cast<void>(QMudPluginPathUtils::resolveInsideQmudHome(
	    m_startupDirectory, pluginsRelative.isEmpty() ? QStringLiteral("worlds/plugins") : pluginsRelative,
	    &pluginsDir, &pluginsDirError));
	if (!doc.expandIncludes(resolved, pluginsDir, resolveWorkingDir(m_startupDirectory),
	                        m_stateFilesDirectory))
	{
		if (error)
			*error = doc.errorString();
		return false;
	}

	if (doc.plugins().isEmpty())
	{
		if (error)
			*error = QStringLiteral("No plugin found");
		return false;
	}

	const WorldDocument::Plugin &p        = doc.plugins().front();
	const QString                pluginId = p.attributes.value(QStringLiteral("id")).trimmed().toLower();
	if (QMudNativePluginRegistry::isBlacklistedId(pluginId))
		return true;
	if (QMudNativePluginRegistry::isShimId(pluginId))
	{
		QMudNativePluginRegistry::NativePluginMetadata metadata;
		if (!QMudNativePluginRegistry::metadataForShim(pluginId, metadata))
			return true;
		const int existingShimIndex = findPluginIndex(m_plugins, pluginId);
		if (existingShimIndex >= 0)
		{
			const bool wasEnabled                = m_plugins.at(existingShimIndex).enabled;
			const bool wasGlobal                 = m_plugins.at(existingShimIndex).global || markGlobal;
			m_plugins[existingShimIndex]         = makeNativeShimShadowPlugin(metadata, wasGlobal);
			m_plugins[existingShimIndex].enabled = wasEnabled;
			m_plugins[existingShimIndex].attributes.insert(
			    QStringLiteral("enabled"), wasEnabled ? QStringLiteral("1") : QStringLiteral("0"));
			if (pluginId == QMudNativePluginRegistry::mushReaderPluginId())
				QMudNativePluginRegistry::setMushReaderPluginEnabled(this, wasEnabled);
		}
		else
		{
			m_plugins.push_back(makeNativeShimShadowPlugin(metadata, markGlobal));
			if (pluginId == QMudNativePluginRegistry::mushReaderPluginId())
				QMudNativePluginRegistry::setMushReaderPluginEnabled(this, true);
		}
		sortPluginsBySequence();
		m_pluginCount = safeQSizeToInt(m_plugins.size());
		noteTimerStructureMutation();
		invalidatePluginCallbackPresenceCache();
		invalidateLuaCallbackDispatchSnapshot();
		QMudNativePluginRegistry::ensureMushReaderRuntimeSetup(this);
		callPluginCallbacksNoArgs(QStringLiteral("OnPluginListChanged"), false);
		return true;
	}
	const int existingIndex = findPluginIndex(m_plugins, pluginId);
	if (existingIndex >= 0)
	{
		Plugin &existing = m_plugins[existingIndex];
		if (markGlobal)
		{
			existing.global = true;
			if (existing.source.isEmpty())
			{
				existing.source =
				    QMudPluginPathUtils::qmudHomeRelativePath(m_startupDirectory, resolved, false);
				existing.directory =
				    QMudPluginPathUtils::qmudHomeRelativePath(m_startupDirectory, info.absolutePath(), true);
			}
			return true;
		}
		if (error)
		{
			const QString pluginName = existing.attributes.value(QStringLiteral("name")).trimmed();
			if (!pluginName.isEmpty())
				*error = QStringLiteral("The plugin \"%1\" is already loaded.").arg(pluginName);
			else
				*error = QStringLiteral("The plugin is already loaded.");
		}
		return false;
	}

	Plugin rp;
	rp.attributes  = p.attributes;
	rp.description = p.description;
	rp.script      = p.script;
	rp.source      = QMudPluginPathUtils::qmudHomeRelativePath(m_startupDirectory, resolved, false);
	rp.directory   = QMudPluginPathUtils::qmudHomeRelativePath(m_startupDirectory, info.absolutePath(), true);
	rp.global      = markGlobal;
	rp.sequence    = pluginSequenceFromAttributes(rp.attributes);
	rp.version     = rp.attributes.value(QStringLiteral("version")).toDouble();
	rp.requiredVersion             = rp.attributes.value(QStringLiteral("requires")).toDouble();
	rp.dateWritten                 = parsePluginDate(rp.attributes.value(QStringLiteral("date_written")));
	rp.dateModified                = parsePluginDate(rp.attributes.value(QStringLiteral("date_modified")));
	rp.dateInstalled               = QDateTime::currentDateTime();
	rp.saveState                   = isEnabledFlag(rp.attributes.value(QStringLiteral("save_state")));
	const QString enabledFlag      = rp.attributes.value(QStringLiteral("enabled"));
	const bool    requestedEnabled = enabledFlag.isEmpty() ? true : isEnabledFlag(enabledFlag);
	rp.enabled                     = requestedEnabled;

	for (const auto &t : p.triggers)
	{
		Trigger rt;
		rt.attributes                 = t.attributes;
		rt.children                   = t.children;
		rt.included                   = t.included;
		const bool hasExplicitEnabled = rt.attributes.contains(QStringLiteral("enabled"));
		applyTriggerDefaults(rt);
		if (!hasExplicitEnabled)
			rt.attributes.insert(QStringLiteral("enabled"), QStringLiteral("0"));
		rp.triggers.push_back(rt);
	}
	for (const auto &a : p.aliases)
	{
		Alias ra;
		ra.attributes                 = a.attributes;
		ra.children                   = a.children;
		ra.included                   = a.included;
		const bool hasExplicitEnabled = ra.attributes.contains(QStringLiteral("enabled"));
		applyAliasDefaults(ra);
		if (!hasExplicitEnabled)
			ra.attributes.insert(QStringLiteral("enabled"), QStringLiteral("0"));
		rp.aliases.push_back(ra);
	}
	for (const auto &t : p.timers)
	{
		Timer rt;
		rt.attributes                 = t.attributes;
		rt.children                   = t.children;
		rt.included                   = t.included;
		const bool hasExplicitEnabled = rt.attributes.contains(QStringLiteral("enabled"));
		applyTimerDefaults(rt);
		if (!hasExplicitEnabled)
			rt.attributes.insert(QStringLiteral("enabled"), QStringLiteral("0"));
		resetTimerFields(rt);
		rp.timers.push_back(rt);
	}
	for (const auto &v : p.variables)
	{
		const QString name = v.attributes.value(QStringLiteral("name"));
		if (!name.isEmpty())
			rp.variables.insert(name, v.content);
	}
	if (!m_stateFilesDirectory.isEmpty())
	{
		const QString worldId = m_worldAttributes.value(QStringLiteral("id"));
		if (!worldId.isEmpty() && !pluginId.isEmpty())
		{
			QString base = m_stateFilesDirectory;
			if (!base.endsWith('/') && !base.endsWith('\\'))
				base += '/';
			const QString   stateFile = base + worldId + "-" + pluginId + "-state.xml";
			QFileInfo const stateInfo(stateFile);
			if (stateInfo.exists())
			{
				if (stateInfo.size() > 0)
				{
					WorldDocument stateDoc;
					stateDoc.setLoadMask(WorldDocument::XML_VARIABLES | WorldDocument::XML_NO_PLUGINS);
					if (!stateDoc.loadFromFile(stateInfo.absoluteFilePath()))
					{
						if (error)
							*error = stateDoc.errorString();
						return false;
					}
					for (const auto &v : stateDoc.variables())
					{
						const QString name = v.attributes.value(QStringLiteral("name"));
						if (!name.isEmpty())
							rp.variables.insert(name, v.content);
					}
				}
			}
		}
	}

	const QString language = rp.attributes.value(QStringLiteral("language"));
	if (language.compare(QStringLiteral("lua"), Qt::CaseInsensitive) == 0)
	{
		rp.lua = QSharedPointer<LuaCallbackEngine>::create();
		LuaEngineObservedInitializationRequest initRequest;
		initRequest.engine          = rp.lua.data();
		initRequest.runtime         = this;
		initRequest.scriptText      = rp.script;
		initRequest.pluginId        = rp.attributes.value(QStringLiteral("id"));
		initRequest.pluginName      = rp.attributes.value(QStringLiteral("name"));
		initRequest.pluginDirectory = rp.directory;
		initRequest.callbackNames   = m_observedPluginCallbacks;
		initRequest.observer        = [this](const QString &pluginId, const QSet<QString> &presentCallbacks,
		                                     const QSet<QString> &luaFunctions)
		{ recordObservedPluginCallbackPresenceSnapshot(pluginId, presentCallbacks, luaFunctions); };
		dispatchInitializeLuaEnginesWithObservedCallbacks({initRequest}, true);
		invalidatePluginCallbackPresenceCache();
	}
	if (!requestedEnabled && rp.lua && hasValidPluginId(rp))
	{
		rp.enabled             = true;
		rp.disableAfterInstall = true;
	}

	m_plugins.push_back(rp);
	sortPluginsBySequence();
	m_pluginCount = safeQSizeToInt(m_plugins.size());
	noteTimerStructureMutation();
	invalidatePluginCallbackPresenceCache();
	invalidateLuaCallbackDispatchSnapshot();

	// Queue install for the plugin we just loaded (not "last after sort"),
	// otherwise lower-sequence plugins can remain permanently install-pending.
	const int installedIndex = findPluginIndex(m_plugins, pluginId);
	if (installedIndex >= 0)
		queuePluginInstall(m_plugins[installedIndex]);
	callPluginCallbacksNoArgs(QStringLiteral("OnPluginListChanged"), false);
	return true;
}

bool WorldRuntime::unloadPlugin(const QString &pluginId, QString *error)
{
	if (QThread::currentThread() != thread())
	{
		const auto resolved = qmudInvokeMethodOr(this, std::pair<bool, QString>{false, {}},
		                                         [this, pluginId, needsError = (error != nullptr)]
		                                         {
			                                         QString    localError;
			                                         QString   *errorPtr = needsError ? &localError : nullptr;
			                                         const bool unloaded = unloadPlugin(pluginId, errorPtr);
			                                         return std::pair<bool, QString>{unloaded, localError};
		                                         });
		if (error)
			*error = resolved.second;
		return resolved.first;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::unloadPlugin");
	const QString resolvedPluginId = resolvePluginIdOrName(pluginId);
	const int     index            = findPluginIndex(m_plugins, resolvedPluginId);
	if (index < 0)
	{
		if (QMudNativePluginRegistry::isShimId(resolvedPluginId))
			return nativeShimAvailableWithoutShadowPlugin(resolvedPluginId);
		if (error)
			*error = QStringLiteral("Plugin not found");
		return false;
	}

	Plugin &plugin = m_plugins[index];
	if (plugin.nativeShim && resolvedPluginId == QMudNativePluginRegistry::mushReaderPluginId())
		QMudNativePluginRegistry::setMushReaderPluginEnabled(this, false);
	if (plugin.nativeShim && resolvedPluginId == QMudNativePluginRegistry::luaAudioPluginId())
		QMudNativePluginRegistry::luaAudioStopRuntime(this);
	if (plugin.lua)
		dispatchSingleEngineNoArgCallback(plugin.lua, QStringLiteral("OnPluginClose"), true);
	savePluginStateForPlugin(plugin, false, nullptr);
	if (plugin.lua)
	{
		const QString unloadedPluginId = normalizedPluginId(plugin.attributes.value(QStringLiteral("id")));
		if (!unloadedPluginId.isEmpty())
		{
			{
				QMutexLocker locker(&m_pluginObservedCallbackPresenceMutex);
				m_pendingPluginObservedCallbackPresenceById.remove(unloadedPluginId);
				m_pendingPluginLuaFunctionCatalogById.remove(unloadedPluginId);
			}
			m_pluginObservedCallbackPresenceById.remove(unloadedPluginId);
			m_pluginLuaFunctionCatalogById.remove(unloadedPluginId);
		}
		const QVector<QSharedPointer<LuaCallbackEngine>> enginesToTeardown{plugin.lua};
		cancelSuspendedPluginCallbackDispatchesForEngines(enginesToTeardown);
		dispatchTeardownLuaEngines(enginesToTeardown, false);
		plugin.lua.clear();
	}
	m_plugins.removeAt(index);
	m_pluginCount = safeQSizeToInt(m_plugins.size());
	noteTimerStructureMutation();
	invalidatePluginCallbackPresenceCache();
	invalidateLuaCallbackDispatchSnapshot();
	callPluginCallbacksNoArgs(QStringLiteral("OnPluginListChanged"), false);
	return true;
}

bool WorldRuntime::enablePlugin(const QString &pluginId, bool enable)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodTrueOnSuccess(this, [this, pluginId, enable]
		                                     { return enablePlugin(pluginId, enable); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::enablePlugin");
	const QString resolvedPluginId = resolvePluginIdOrName(pluginId);
	const int     index            = findPluginIndex(m_plugins, resolvedPluginId);
	if (index < 0)
		return QMudNativePluginRegistry::isShimId(resolvedPluginId) &&
		       nativeShimAvailableWithoutShadowPlugin(resolvedPluginId);
	Plugin &plugin = m_plugins[index];
	if (plugin.enabled == enable)
	{
		if (enable && plugin.disableAfterInstall)
		{
			plugin.disableAfterInstall = false;
			plugin.attributes.insert(QStringLiteral("enabled"), QStringLiteral("1"));
			invalidatePluginCallbackPresenceCache();
			invalidateLuaCallbackDispatchSnapshot();
			if (!m_loadingDocument)
				m_worldFileModified = true;
		}
		return true;
	}
	plugin.enabled = enable;
	if (!enable)
		plugin.disableAfterInstall = false;
	plugin.attributes.insert(QStringLiteral("enabled"), enable ? QStringLiteral("1") : QStringLiteral("0"));
	if (plugin.nativeShim && resolvedPluginId == QMudNativePluginRegistry::mushReaderPluginId())
		QMudNativePluginRegistry::setMushReaderPluginEnabled(this, enable);
	if (plugin.nativeShim && resolvedPluginId == QMudNativePluginRegistry::luaAudioPluginId() && !enable)
		QMudNativePluginRegistry::luaAudioStopRuntime(this);
	invalidateLuaCallbackDispatchSnapshot();
	if (enable)
	{
		if (plugin.lua)
			dispatchSingleEngineNoArgCallback(plugin.lua, QStringLiteral("OnPluginEnable"), true);
		// Callback may toggle plugin state (e.g. script calls EnablePlugin on itself).
		if (plugin.enabled && plugin.installPending)
			queuePluginInstall(plugin);
	}
	else
	{
		if (plugin.lua)
			dispatchSingleEngineNoArgCallback(plugin.lua, QStringLiteral("OnPluginDisable"), true);
	}
	if (!m_loadingDocument)
		m_worldFileModified = true;
	invalidatePluginCallbackPresenceCache();
	return true;
}

int WorldRuntime::reloadPlugin(const QString &pluginId, QString *error)
{
	if (QThread::currentThread() != thread())
	{
		const auto resolved = qmudInvokeMethodOr(this, std::pair<int, QString>{eNoSuchPlugin, {}},
		                                         [this, pluginId, needsError = (error != nullptr)]
		                                         {
			                                         QString   localError;
			                                         QString  *errorPtr = needsError ? &localError : nullptr;
			                                         const int result   = reloadPlugin(pluginId, errorPtr);
			                                         return std::pair<int, QString>{result, localError};
		                                         });
		if (error)
			*error = resolved.second;
		return resolved.first;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::reloadPlugin");
	const QString resolvedPluginId = resolvePluginIdOrName(pluginId);
	if (QMudNativePluginRegistry::isShimId(resolvedPluginId))
		return nativeShimVisibleForPluginApi(m_plugins, resolvedPluginId) ? eOK : eNoSuchPlugin;
	const int index = findPluginIndex(m_plugins, resolvedPluginId);
	if (index < 0)
		return eNoSuchPlugin;

	const Plugin  existing = m_plugins.at(index);
	const QString source   = existing.source;
	if (source.isEmpty())
		return ePluginFileNotFound;

	const bool wasGlobal  = existing.global;
	const bool wasEnabled = existing.enabled;

	QString    unloadError;
	if (!unloadPlugin(resolvedPluginId, &unloadError))
	{
		if (error)
			*error = unloadError;
		return eNoSuchPlugin;
	}

	QString loadError;
	if (!loadPluginFile(source, &loadError, wasGlobal))
	{
		if (error)
			*error = loadError;
		if (loadError.contains(QStringLiteral("not found"), Qt::CaseInsensitive))
			return ePluginFileNotFound;
		return eProblemsLoadingPlugin;
	}

	if (!wasEnabled)
		enablePlugin(resolvedPluginId, false);

	return eOK;
}

bool WorldRuntime::isPluginInstalled(const QString &pluginId) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodTrueOnSuccess(const_cast<WorldRuntime *>(this),
		                                     [this, pluginId] { return isPluginInstalled(pluginId); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::isPluginInstalled");
	if (QMudNativePluginRegistry::isBlacklistedId(pluginId))
		return false;
	if (const QString shimId = QMudNativePluginRegistry::resolveShimIdOrName(pluginId); !shimId.isEmpty())
		return nativeShimVisibleForPluginApi(m_plugins, shimId);
	return findPluginIndex(m_plugins, pluginId) >= 0;
}

QMudNativePluginRegistry::NativeCallContext
WorldRuntime::nativePluginCallContext(const QString &pluginId) const
{
	if (QThread::currentThread() != thread())
	{
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this),
		                          QMudNativePluginRegistry::NativeCallContext{},
		                          [this, pluginId] { return nativePluginCallContext(pluginId); });
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::nativePluginCallContext");
	const QString shimId = QMudNativePluginRegistry::resolveShimIdOrName(pluginId);
	if (shimId.isEmpty() || QMudNativePluginRegistry::isBlacklistedId(shimId))
		return {};
	return nativeShimCallContext(this, m_plugins, shimId);
}

QString WorldRuntime::resolvePluginIdOrName(const QString &pluginIdOrName) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QString{},
		                          [this, pluginIdOrName] { return resolvePluginIdOrName(pluginIdOrName); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::resolvePluginIdOrName");
	const QString key = pluginIdOrName.trimmed();
	if (key.isEmpty())
		return {};
	if (QMudNativePluginRegistry::isBlacklistedId(key))
		return {};
	if (const QString shimId = QMudNativePluginRegistry::resolveShimIdOrName(key); !shimId.isEmpty())
		return shimId;
	const int idIndex = findPluginIndex(m_plugins, key);
	if (idIndex >= 0)
	{
		const QString id = normalizedPluginId(m_plugins.at(idIndex).attributes.value(QStringLiteral("id")));
		return QMudNativePluginRegistry::isBlacklistedId(id) ? QString() : id;
	}
	for (const Plugin &plugin : m_plugins)
	{
		if (plugin.attributes.value(QStringLiteral("name")).compare(key, Qt::CaseInsensitive) == 0)
		{
			const QString id = normalizedPluginId(plugin.attributes.value(QStringLiteral("id")));
			return QMudNativePluginRegistry::isBlacklistedId(id) ? QString() : id;
		}
	}
	return {};
}

int WorldRuntime::pluginSupports(const QString &pluginId, const QString &routine) const
{
	if (QMudNativePluginRegistry::isBlacklistedId(pluginId))
		return eNoSuchPlugin;
	if (const QString shimId = QMudNativePluginRegistry::resolveShimIdOrName(pluginId); !shimId.isEmpty())
	{
		if (QThread::currentThread() != thread())
		{
			return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), eNoSuchPlugin,
			                          [this, shimId, routine] { return pluginSupports(shimId, routine); });
		}
		qmudAssertObjectThreadAffinity(this, "WorldRuntime::pluginSupports");
		if (!nativeShimVisibleForPluginApi(m_plugins, shimId))
			return eNoSuchPlugin;
		return QMudNativePluginRegistry::pluginSupports(shimId, routine);
	}

	const auto supportsRoutine = [this, &routine](const QSharedPointer<LuaCallbackEngine> &engine) -> int
	{
		if (!engine)
			return eNoSuchRoutine;

		LuaBatchDispatchRequest request;
		request.kind                        = LuaBatchDispatchKind::HasFunction;
		request.engines                     = {engine};
		request.functionName                = routine;
		request.lane                        = LuaBatchDispatchLane::Callback;
		const LuaBatchDispatchResult result = dispatchLuaBatch(request);
		if (!result.hasFunctionValid)
			return eNoSuchRoutine;
		return result.hasFunction ? eOK : eNoSuchRoutine;
	};

	if (QThread::currentThread() != thread())
	{
		struct WorkerSupportsContext
		{
				int                               result{eNoSuchRoutine};
				QSharedPointer<LuaCallbackEngine> lua;
		};

		const WorkerSupportsContext context =
		    qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), WorkerSupportsContext{},
		                       [this, &pluginId]
		                       {
			                       WorkerSupportsContext context;
			                       const int             index = findPluginIndex(m_plugins, pluginId);
			                       if (index < 0)
			                       {
				                       context.result = eNoSuchPlugin;
				                       return context;
			                       }
			                       const Plugin &plugin = m_plugins.at(index);
			                       if (!plugin.lua)
			                       {
				                       context.result = eNoSuchRoutine;
				                       return context;
			                       }
			                       context.lua    = plugin.lua;
			                       context.result = eOK;
			                       return context;
		                       });
		if (context.result != eOK)
			return context.result;
		return supportsRoutine(context.lua);
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::pluginSupports");
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return eNoSuchPlugin;
	if (routine.trimmed().isEmpty())
		return eNoSuchRoutine;
	const Plugin &plugin = m_plugins.at(index);
	return supportsRoutine(plugin.lua);
}

int WorldRuntime::callPlugin(const QString &pluginId, const QString &routine, const QString &argument,
                             const QString &callingPluginId)
{
	if (QMudNativePluginRegistry::isBlacklistedId(pluginId))
		return eNoSuchPlugin;
	if (const QString shimId = QMudNativePluginRegistry::resolveShimIdOrName(pluginId); !shimId.isEmpty())
	{
		if (QThread::currentThread() != thread())
			return qmudInvokeMethodOr(this, eErrorCallingPluginRoutine,
			                          [this, shimId, routine, argument, callingPluginId]
			                          { return callPlugin(shimId, routine, argument, callingPluginId); });
		const QMudNativePluginRegistry::NativeCallResult result = QMudNativePluginRegistry::callRoutine(
		    this, shimId, routine, {QVariant(argument)}, nativeShimCallContext(this, m_plugins, shimId));
		return result.errorCode;
	}

	if (QThread::currentThread() != thread())
	{
		struct WorkerCallContext
		{
				int                                                 result{eErrorCallingPluginRoutine};
				QSharedPointer<LuaCallbackEngine>                   lua;
				QSharedPointer<const LuaCallbackMiniWindowSnapshot> miniWindowSnapshot;
		};

		const WorkerCallContext context =
		    qmudInvokeMethodOr(this, WorkerCallContext{},
		                       [&]
		                       {
			                       WorkerCallContext workerContext;
			                       const int         index = findPluginIndex(m_plugins, pluginId);
			                       if (index < 0)
			                       {
				                       workerContext.result = eNoSuchPlugin;
				                       return workerContext;
			                       }

			                       Plugin &plugin = m_plugins[index];
			                       if (!plugin.lua)
			                       {
				                       workerContext.result = eNoSuchRoutine;
				                       return workerContext;
			                       }
			                       if (!plugin.enabled)
			                       {
				                       workerContext.result = ePluginDisabled;
				                       return workerContext;
			                       }
			                       if (routine.trimmed().isEmpty())
			                       {
				                       workerContext.result = eNoSuchRoutine;
				                       return workerContext;
			                       }

			                       workerContext.lua = plugin.lua;
			                       workerContext.miniWindowSnapshot =
			                           captureLuaCallbackSnapshotForDispatch({workerContext.lua});
			                       workerContext.result = eOK;
			                       return workerContext;
		                       });
		if (context.result != eOK)
			return context.result;
		LuaBatchDispatchRequest request;
		request.kind                             = LuaBatchDispatchKind::ProcedureWithString;
		request.engines                          = {context.lua};
		request.functionName                     = routine;
		request.stringArg                        = argument;
		request.applyCallingPluginContext        = true;
		request.callingPluginId                  = callingPluginId;
		request.lane                             = LuaBatchDispatchLane::Callback;
		request.miniWindowSnapshotArg            = context.miniWindowSnapshot;
		const LuaBatchDispatchResult result      = dispatchLuaBatch(request);
		const bool                   hasFunction = result.hasFunctionValid ? result.hasFunction : false;
		const bool                   ok          = result.boolResultValid ? result.boolResult : false;
		if (!hasFunction)
			return eNoSuchRoutine;
		return ok ? eOK : eErrorCallingPluginRoutine;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::callPlugin");
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return eNoSuchPlugin;
	Plugin &plugin = m_plugins[index];
	if (!plugin.lua)
		return eNoSuchRoutine;
	if (!plugin.enabled)
		return ePluginDisabled;
	if (routine.trimmed().isEmpty())
		return eNoSuchRoutine;

	LuaBatchDispatchRequest request;
	request.kind                             = LuaBatchDispatchKind::ProcedureWithString;
	request.engines                          = {plugin.lua};
	request.functionName                     = routine;
	request.stringArg                        = argument;
	request.applyCallingPluginContext        = true;
	request.callingPluginId                  = callingPluginId;
	const LuaBatchDispatchResult result      = queuePluginCallbackDispatch(request, true);
	const bool                   hasFunction = result.hasFunctionValid ? result.hasFunction : false;
	const bool                   ok          = result.boolResultValid ? result.boolResult : false;
	if (!hasFunction)
		return eNoSuchRoutine;
	return ok ? eOK : eErrorCallingPluginRoutine;
}

#ifdef QMUD_ENABLE_LUA_SCRIPTING
int WorldRuntime::callPluginLua(const QString &pluginId, const QString &routine, lua_State *callerState,
                                int firstArg, const QString &callingPluginId)
{
	if (!callerState)
		return 0;
	if (routine.trimmed().isEmpty())
	{
		lua_pushnumber(callerState, eNoSuchRoutine);
		lua_pushstring(callerState, "No function name supplied");
		return 2;
	}

	const auto pushUtf8String = [](lua_State *state, const QString &message)
	{
		const QByteArray bytes = message.toUtf8();
		lua_pushlstring(state, bytes.constData(), bytes.size());
	};

	const auto pushCodeAndMessage = [callerState, &pushUtf8String](const int code, const QString &message)
	{
		lua_pushnumber(callerState, code);
		pushUtf8String(callerState, message);
		return 2;
	};

	if (QMudNativePluginRegistry::isBlacklistedId(pluginId))
		return pushCodeAndMessage(eNoSuchPlugin,
		                          QStringLiteral("Plugin ID (%1) is not installed").arg(pluginId));

	if (const QString shimId = QMudNativePluginRegistry::resolveShimIdOrName(pluginId); !shimId.isEmpty())
	{
		const auto nativeContext = [this, &shimId] { return nativeShimCallContext(this, m_plugins, shimId); };
		const QMudNativePluginRegistry::NativeCallContext context =
		    QThread::currentThread() == thread()
		        ? nativeContext()
		        : qmudInvokeMethodOr(this, QMudNativePluginRegistry::NativeCallContext{}, nativeContext);
		if (!context.installed || !context.pluginEnabled)
		{
			const QMudNativePluginRegistry::NativeCallResult result =
			    QMudNativePluginRegistry::callRoutine(this, shimId, routine, {}, context);
			return pushCodeAndMessage(result.errorCode, result.errorText);
		}

		QVector<QVariant> arguments;
		const int         top = lua_gettop(callerState);
		arguments.reserve(std::max(0, top - firstArg + 1));
		for (int index = firstArg; index <= top; ++index)
		{
			switch (lua_type(callerState, index))
			{
			case LUA_TSTRING:
			{
				size_t      length = 0;
				const char *data   = lua_tolstring(callerState, index, &length);
				arguments.push_back(QString::fromUtf8(data, static_cast<qsizetype>(length)));
				break;
			}
			case LUA_TNUMBER:
				arguments.push_back(lua_tonumber(callerState, index));
				break;
			case LUA_TBOOLEAN:
				arguments.push_back(lua_toboolean(callerState, index) != 0);
				break;
			case LUA_TNIL:
				arguments.push_back(QVariant());
				break;
			default:
				return pushCodeAndMessage(
				    eBadParameter,
				    QStringLiteral("Cannot pass argument #%1 (%2 type) to CallPlugin")
				        .arg(index - firstArg + 3)
				        .arg(QString::fromLatin1(lua_typename(callerState, lua_type(callerState, index)))));
			}
		}
		const auto invokeNative = [this, shimId, routine, arguments]
		{
			return QMudNativePluginRegistry::callRoutine(this, shimId, routine, arguments,
			                                             nativeShimCallContext(this, m_plugins, shimId));
		};
		const QMudNativePluginRegistry::NativeCallResult result =
		    QThread::currentThread() == thread()
		        ? invokeNative()
		        : qmudInvokeMethodOr(this, nativeShimCallGateSynchronizationError(), invokeNative);
		if (result.errorCode != eOK)
			return pushCodeAndMessage(result.errorCode, result.errorText);
		const qsizetype returnValueCount      = result.returnValues.size();
		constexpr int   kErrorCodeReturnCount = 1;
		if (returnValueCount > std::numeric_limits<int>::max() - kErrorCodeReturnCount)
		{
			return pushCodeAndMessage(
			    eErrorCallingPluginRoutine,
			    QStringLiteral("Native plugin routine '%1' returned too many values").arg(routine));
		}
		const int pushedValueCount = static_cast<int>(returnValueCount) + kErrorCodeReturnCount;
		if (lua_checkstack(callerState, pushedValueCount) == 0)
		{
			return pushCodeAndMessage(
			    eErrorCallingPluginRoutine,
			    QStringLiteral("Unable to reserve Lua stack space for native plugin routine '%1'")
			        .arg(routine));
		}
		lua_pushnumber(callerState, result.errorCode);
		for (const QVariant &value : result.returnValues)
		{
			switch (value.typeId())
			{
			case QMetaType::Bool:
				lua_pushboolean(callerState, value.toBool());
				break;
			case QMetaType::Int:
			case QMetaType::LongLong:
				lua_pushinteger(callerState, static_cast<lua_Integer>(value.toLongLong()));
				break;
			case QMetaType::Double:
				lua_pushnumber(callerState, value.toDouble());
				break;
			default:
				pushUtf8String(callerState, value.toString());
				break;
			}
		}
		return pushedValueCount;
	}

	if (QThread::currentThread() != thread())
	{
		struct WorkerCallContext
		{
				int     errorCode{eErrorCallingPluginRoutine};
				QString errorText{QStringLiteral("Failed to synchronize CallPlugin with runtime thread")};
				QString pluginName;
				QSharedPointer<LuaCallbackEngine>                   lua;
				QSharedPointer<const LuaCallbackMiniWindowSnapshot> miniWindowSnapshot;
		};

		const WorkerCallContext context = qmudInvokeMethodOr(
		    this, WorkerCallContext{},
		    [&]
		    {
			    WorkerCallContext workerContext;
			    const int         index = findPluginIndex(m_plugins, pluginId);
			    if (index < 0)
			    {
				    workerContext.errorCode = eNoSuchPlugin;
				    workerContext.errorText = QStringLiteral("Plugin ID (%1) is not installed").arg(pluginId);
				    return workerContext;
			    }

			    Plugin &plugin           = m_plugins[index];
			    workerContext.pluginName = plugin.attributes.value(QStringLiteral("name"));
			    if (!plugin.enabled)
			    {
				    workerContext.errorCode = ePluginDisabled;
				    workerContext.errorText =
				        QStringLiteral("Plugin '%1' (%2) disabled").arg(workerContext.pluginName, pluginId);
				    return workerContext;
			    }

			    if (!plugin.lua)
			    {
				    workerContext.errorCode = eNoSuchRoutine;
				    workerContext.errorText = QStringLiteral("Scripting not enabled in plugin '%1' (%2)")
				                                  .arg(workerContext.pluginName, pluginId);
				    return workerContext;
			    }

			    workerContext.errorCode = eOK;
			    workerContext.errorText.clear();
			    workerContext.lua                = plugin.lua;
			    workerContext.miniWindowSnapshot = captureLuaCallbackSnapshotForDispatch({workerContext.lua});
			    return workerContext;
		    });
		if (context.errorCode != eOK)
			return pushCodeAndMessage(context.errorCode, context.errorText);
		const int               callerTopBefore = lua_gettop(callerState);
		LuaBatchDispatchRequest request;
		request.kind                                     = LuaBatchDispatchKind::CallPluginLuaMarshalling;
		request.engines                                  = {context.lua};
		request.functionName                             = routine;
		request.intArg1                                  = firstArg;
		request.luaStateArg                              = callerState;
		request.refreshCallbackCatalogAfter              = true;
		request.applyCallingPluginContext                = true;
		request.callingPluginId                          = callingPluginId;
		request.lane                                     = LuaBatchDispatchLane::Callback;
		request.miniWindowSnapshotArg                    = context.miniWindowSnapshot;
		const LuaBatchDispatchResult result              = dispatchLuaBatch(request);
		const bool                   marshallingExecuted = result.boolResultValid ? result.boolResult : false;
		const bool                   sameState           = result.marshallingSameState;
		CallPluginLuaMarshallingResult marshalling;
		marshalling.error        = result.marshallingErrorValid
		                               ? static_cast<CallPluginLuaMarshallingError>(result.marshallingError)
		                               : CallPluginLuaMarshallingError::NoSuchRoutine;
		marshalling.index        = result.marshallingIndex;
		marshalling.typeName     = result.marshallingTypeName;
		marshalling.runtimeError = result.marshallingRuntimeError;
		marshalling.returnCount  = result.marshallingReturnCount;
		if (!marshallingExecuted)
		{
			return pushCodeAndMessage(eNoSuchRoutine,
			                          QStringLiteral("Scripting not enabled in plugin '%1' (%2)")
			                              .arg(context.pluginName, pluginId));
		}

		switch (marshalling.error)
		{
		case CallPluginLuaMarshallingError::NoSuchRoutine:
		{
			return pushCodeAndMessage(eNoSuchRoutine, QStringLiteral("No function '%1' in plugin '%2' (%3)")
			                                              .arg(routine, context.pluginName, pluginId));
		}
		case CallPluginLuaMarshallingError::UnsupportedArgumentType:
		{
			lua_pushnumber(callerState, eBadParameter);
			const int     displayIndex = marshalling.index - firstArg + 3; // plugin ID + routine removed
			const QString error        = QStringLiteral("Cannot pass argument #%1 (%2 type) to CallPlugin")
			                                 .arg(displayIndex)
			                                 .arg(QString::fromLatin1(marshalling.typeName));
			pushUtf8String(callerState, error);
			return 2;
		}
		case CallPluginLuaMarshallingError::RuntimeError:
		{
			lua_pushnumber(callerState, eErrorCallingPluginRoutine);
			const QString error = QStringLiteral("Runtime error in function '%1', plugin '%2' (%3)")
			                          .arg(routine, context.pluginName, pluginId);
			pushUtf8String(callerState, error);
			pushUtf8String(callerState, marshalling.runtimeError);
			return 3;
		}
		case CallPluginLuaMarshallingError::UnsupportedReturnType:
		{
			lua_pushnumber(callerState, eErrorCallingPluginRoutine);
			const QString error =
			    QStringLiteral("Cannot handle return value #%1 (%2 type) from function '%3' in plugin "
			                   "'%4' (%5)")
			        .arg(marshalling.index)
			        .arg(QString::fromLatin1(marshalling.typeName))
			        .arg(routine, context.pluginName, pluginId);
			pushUtf8String(callerState, error);
			return 2;
		}
		case CallPluginLuaMarshallingError::None:
			break;
		}

		lua_pushnumber(callerState, eOK);
		if (sameState)
			lua_insert(callerState, firstArg);
		else
			lua_insert(callerState, callerTopBefore + 1);
		return marshalling.returnCount + 1;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::callPluginLua");
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
	{
		return pushCodeAndMessage(eNoSuchPlugin,
		                          QStringLiteral("Plugin ID (%1) is not installed").arg(pluginId));
	}

	Plugin &plugin = m_plugins[index];
	if (!plugin.enabled)
	{
		return pushCodeAndMessage(ePluginDisabled,
		                          QStringLiteral("Plugin '%1' (%2) disabled")
		                              .arg(plugin.attributes.value(QStringLiteral("name")), pluginId));
	}

	const int               callerTopBefore = lua_gettop(callerState);
	LuaBatchDispatchRequest request;
	request.kind                                       = LuaBatchDispatchKind::CallPluginLuaMarshalling;
	request.engines                                    = {plugin.lua};
	request.functionName                               = routine;
	request.intArg1                                    = firstArg;
	request.luaStateArg                                = callerState;
	request.refreshCallbackCatalogAfter                = true;
	request.applyCallingPluginContext                  = true;
	request.callingPluginId                            = callingPluginId;
	const LuaBatchDispatchResult   result              = queuePluginCallbackDispatch(request, true);
	const bool                     marshallingExecuted = result.boolResultValid ? result.boolResult : false;
	const bool                     sameState           = result.marshallingSameState;
	CallPluginLuaMarshallingResult marshalling;
	marshalling.error        = result.marshallingErrorValid
	                               ? static_cast<CallPluginLuaMarshallingError>(result.marshallingError)
	                               : CallPluginLuaMarshallingError::NoSuchRoutine;
	marshalling.index        = result.marshallingIndex;
	marshalling.typeName     = result.marshallingTypeName;
	marshalling.runtimeError = result.marshallingRuntimeError;
	marshalling.returnCount  = result.marshallingReturnCount;
	if (!marshallingExecuted)
	{
		return pushCodeAndMessage(eNoSuchRoutine,
		                          QStringLiteral("Scripting not enabled in plugin '%1' (%2)")
		                              .arg(plugin.attributes.value(QStringLiteral("name")), pluginId));
	}

	switch (marshalling.error)
	{
	case CallPluginLuaMarshallingError::NoSuchRoutine:
	{
		return pushCodeAndMessage(
		    eNoSuchRoutine, QStringLiteral("No function '%1' in plugin '%2' (%3)")
		                        .arg(routine, plugin.attributes.value(QStringLiteral("name")), pluginId));
	}
	case CallPluginLuaMarshallingError::UnsupportedArgumentType:
	{
		lua_pushnumber(callerState, eBadParameter);
		const int     displayIndex = marshalling.index - firstArg + 3; // plugin ID + routine removed
		const QString error        = QStringLiteral("Cannot pass argument #%1 (%2 type) to CallPlugin")
		                                 .arg(displayIndex)
		                                 .arg(QString::fromLatin1(marshalling.typeName));
		pushUtf8String(callerState, error);
		return 2;
	}
	case CallPluginLuaMarshallingError::RuntimeError:
	{
		lua_pushnumber(callerState, eErrorCallingPluginRoutine);
		const QString error = QStringLiteral("Runtime error in function '%1', plugin '%2' (%3)")
		                          .arg(routine, plugin.attributes.value(QStringLiteral("name")), pluginId);
		pushUtf8String(callerState, error);
		pushUtf8String(callerState, marshalling.runtimeError);
		return 3;
	}
	case CallPluginLuaMarshallingError::UnsupportedReturnType:
	{
		lua_pushnumber(callerState, eErrorCallingPluginRoutine);
		const QString error =
		    QStringLiteral("Cannot handle return value #%1 (%2 type) from function '%3' in plugin '%4' (%5)")
		        .arg(marshalling.index)
		        .arg(QString::fromLatin1(marshalling.typeName))
		        .arg(routine, plugin.attributes.value(QStringLiteral("name")), pluginId);
		pushUtf8String(callerState, error);
		return 2;
	}
	case CallPluginLuaMarshallingError::None:
		break;
	}

	lua_pushnumber(callerState, eOK);
	if (sameState)
		lua_insert(callerState, firstArg);
	else
		lua_insert(callerState, callerTopBefore + 1);
	return marshalling.returnCount + 1;
}
#endif

int WorldRuntime::broadcastPlugin(long message, const QString &text, const QString &callingPluginId,
                                  const QString &callingPluginName)
{
	const QString callbackName = QStringLiteral("OnPluginBroadcast");

	struct BroadcastSnapshot
	{
			QVector<QSharedPointer<LuaCallbackEngine>>          recipients;
			QSharedPointer<const LuaCallbackMiniWindowSnapshot> miniWindowSnapshot;
	};

	const auto collectSnapshot = [&](BroadcastSnapshot &snapshot)
	{
		if (!hasAnyPluginCallback(callbackName))
			return;
		const QVector<int> recipientIndices =
		    collectExecutablePluginRecipientIndicesWithWarmupFallback(callbackName);
		if (recipientIndices.isEmpty())
			return;
		const int pluginCount = safeQSizeToInt(m_plugins.size());
		if (qmudShouldSkipSelfOnlyPluginBroadcast(
		        recipientIndices, pluginCount, callingPluginId, [this](const int index)
		        { return m_plugins.at(index).attributes.value(QStringLiteral("id")); }))
		{
			return;
		}

		snapshot.recipients.reserve(recipientIndices.size());
		for (const int pluginIndex : recipientIndices)
		{
			auto         &plugin   = m_plugins[pluginIndex];
			const QString pluginId = plugin.attributes.value(QStringLiteral("id"));
			if (!callingPluginId.isEmpty() && pluginId.compare(callingPluginId, Qt::CaseInsensitive) == 0)
				continue;
			snapshot.recipients.push_back(plugin.lua);
		}

		if (snapshot.recipients.isEmpty())
			return;
		snapshot.miniWindowSnapshot = captureLuaCallbackSnapshotForDispatch(snapshot.recipients);
	};

	const auto dispatchSnapshot = [&](const BroadcastSnapshot &snapshot) -> int
	{
		if (snapshot.recipients.isEmpty())
			return 0;
#ifndef NDEBUG
		if (qmudMmStartupDiagShouldLogBroadcast(callingPluginId, snapshot.recipients))
		{
			qInfo().noquote()
			    << QStringLiteral(
			           "[QMud][MMStartupDiag] broadcast-dispatch message=%1 sender=%2 senderName=%3 text=%4 "
			           "recipients=%5")
			           .arg(message)
			           .arg(callingPluginId, callingPluginName, text.left(120),
			                qmudMmStartupDiagEngineLabels(snapshot.recipients));
		}
#endif
		return broadcastPluginToRecipients(message, text, callingPluginId, callingPluginName,
		                                   snapshot.recipients, snapshot.miniWindowSnapshot);
	};

	if (QThread::currentThread() != thread())
	{
		BroadcastSnapshot snapshot =
		    qmudInvokeMethodOr(this, BroadcastSnapshot{},
		                       [&]
		                       {
			                       BroadcastSnapshot s;
			                       // Callback-lane callers must not wait on callback-presence propagation, because this
			                       // path may already be executing on the callback worker. Capture executable recipients
			                       // directly from runtime state; dispatch still counts only plugins that implement the
			                       // callback.
			                       s.recipients = broadcastPluginRecipientSnapshot(callingPluginId);
			                       if (!s.recipients.isEmpty())
				                       s.miniWindowSnapshot =
				                           captureLuaCallbackSnapshotForDispatch(s.recipients);
			                       return s;
		                       });
		return dispatchSnapshot(snapshot);
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::broadcastPlugin");
	BroadcastSnapshot snapshot;
	collectSnapshot(snapshot);
	return dispatchSnapshot(snapshot);
}

int WorldRuntime::broadcastPluginToRecipients(
    long message, const QString &text, const QString &callingPluginId, const QString &callingPluginName,
    const QVector<QSharedPointer<LuaCallbackEngine>>          &recipients,
    const QSharedPointer<const LuaCallbackMiniWindowSnapshot> &miniWindowSnapshot)
{
	if (recipients.isEmpty())
		return 0;
	LuaBatchDispatchRequest request = makePluginBroadcastDispatchRequest(
	    message, text, callingPluginId, callingPluginName, recipients, miniWindowSnapshot);
#ifndef NDEBUG
	if (qmudMmStartupDiagShouldLogBroadcast(callingPluginId, recipients))
	{
		qInfo().noquote()
		    << QStringLiteral(
		           "[QMud][MMStartupDiag] broadcast-to-recipients-start message=%1 sender=%2 text=%3 "
		           "recipients=%4 thread=%5")
		           .arg(message)
		           .arg(callingPluginId, text.left(120), qmudMmStartupDiagEngineLabels(recipients),
		                QThread::currentThread() == thread() ? QStringLiteral("runtime")
		                                                     : QStringLiteral("foreign"));
	}
#endif
	if (QThread::currentThread() != thread())
	{
		request.lane = LuaBatchDispatchLane::Callback;
		if (!request.miniWindowSnapshotArg)
		{
			request.miniWindowSnapshotArg =
			    qmudInvokeMethodOr(this, QSharedPointer<const LuaCallbackMiniWindowSnapshot>{},
			                       [this, recipients = request.engines]
			                       { return captureLuaCallbackSnapshotForDispatch(recipients); });
		}
		const LuaBatchDispatchResult result = dispatchLuaBatch(request);
#ifndef NDEBUG
		if (qmudMmStartupDiagShouldLogBroadcast(callingPluginId, recipients))
		{
			qInfo().noquote()
			    << QStringLiteral(
			           "[QMud][MMStartupDiag] broadcast-to-recipients-direct-done sender=%1 recipients=%2 %3")
			           .arg(callingPluginId, qmudMmStartupDiagEngineLabels(recipients),
			                qmudMmStartupDiagResultLabel(result));
		}
#endif
		return result.countResultValid ? result.countResult : 0;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::broadcastPluginToRecipients");
	const LuaBatchDispatchResult result = queuePluginCallbackDispatch(request, true);
#ifndef NDEBUG
	if (qmudMmStartupDiagShouldLogBroadcast(callingPluginId, recipients))
	{
		qInfo().noquote()
		    << QStringLiteral(
		           "[QMud][MMStartupDiag] broadcast-to-recipients-queued-done sender=%1 recipients=%2 %3")
		           .arg(callingPluginId, qmudMmStartupDiagEngineLabels(recipients),
		                qmudMmStartupDiagResultLabel(result));
	}
#endif
	return result.countResultValid ? result.countResult : 0;
}

int WorldRuntime::broadcastPluginRecipientCount(const QString &callingPluginId) const
{
	const QString callbackName = QStringLiteral("OnPluginBroadcast");

	const auto    resolveCount = [&]() -> int
	{
		if (!const_cast<WorldRuntime *>(this)->hasAnyPluginCallback(callbackName))
			return 0;
		const QVector<int> recipientIndices =
		    collectExecutablePluginRecipientIndicesWithWarmupFallback(callbackName);
		if (recipientIndices.isEmpty())
			return 0;
		const int pluginCount = safeQSizeToInt(m_plugins.size());
		if (qmudShouldSkipSelfOnlyPluginBroadcast(
		        recipientIndices, pluginCount, callingPluginId, [this](const int index)
		        { return m_plugins.at(index).attributes.value(QStringLiteral("id")); }))
		{
			return 0;
		}
		int count = 0;
		for (const int pluginIndex : recipientIndices)
		{
			const QString pluginId = m_plugins.at(pluginIndex).attributes.value(QStringLiteral("id"));
			if (!callingPluginId.isEmpty() && pluginId.compare(callingPluginId, Qt::CaseInsensitive) == 0)
				continue;
			++count;
		}
		return count;
	};

	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), 0, [&] { return resolveCount(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::broadcastPluginRecipientCount");
	return resolveCount();
}

QVector<QSharedPointer<LuaCallbackEngine>>
WorldRuntime::broadcastPluginRecipientSnapshot(const QString &callingPluginId) const
{
	if (QThread::currentThread() != thread())
	{
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this),
		                          QVector<QSharedPointer<LuaCallbackEngine>>{}, [this, callingPluginId]
		                          { return broadcastPluginRecipientSnapshot(callingPluginId); });
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::broadcastPluginRecipientSnapshot");
	const QVector<int> recipientIndices = qmudCollectBroadcastRecipientIndices(
	    m_plugins, callingPluginId,
	    [this](const int pluginIndex)
	    {
		    if (pluginIndex < 0 || pluginIndex >= m_plugins.size())
			    return false;
		    return canExecutePlugin(m_plugins.at(pluginIndex));
	    },
	    [this](const int pluginIndex)
	    {
		    if (pluginIndex < 0 || pluginIndex >= m_plugins.size())
			    return QString();
		    return m_plugins.at(pluginIndex).attributes.value(QStringLiteral("id"));
	    });

	QVector<QSharedPointer<LuaCallbackEngine>> recipients;
	recipients.reserve(recipientIndices.size());
	for (const int pluginIndex : recipientIndices)
		recipients.push_back(m_plugins.at(pluginIndex).lua);
	return recipients;
}

int WorldRuntime::setPluginAsyncResultFilter(const QString &pluginId, const QSet<QString> &apiNames,
                                             const bool allowAll)
{
	if (QThread::currentThread() != thread())
	{
		return qmudInvokeMethodOr(this, eNoSuchPlugin, [this, pluginId, apiNames, allowAll]
		                          { return setPluginAsyncResultFilter(pluginId, apiNames, allowAll); });
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::setPluginAsyncResultFilter");
	const QString normalizedId = normalizedPluginId(pluginId);
	if (normalizedId.isEmpty())
		return eNotAPlugin;

	Plugin *plugin = pluginForId(normalizedId);
	if (!plugin)
		return eNoSuchPlugin;

	plugin->asyncResultFilterAll = allowAll;
	plugin->asyncResultFilterApis.clear();
	if (!allowAll)
	{
		for (const QString &apiName : apiNames)
		{
			const QString normalizedApiName = normalizedAsyncResultApiName(apiName);
			if (!normalizedApiName.isEmpty())
				plugin->asyncResultFilterApis.insert(normalizedApiName);
		}
	}

	return eOK;
}

void WorldRuntime::dispatchPluginAsyncResult(const QString &pluginId, const quint64 requestId,
                                             const QString &apiName, const bool ok, const int errorCode,
                                             const QString &payload)
{
	if (QThread::currentThread() != thread())
	{
		const bool queued = qmudInvokeMethodQueued(
		    this, [this, pluginId, requestId, apiName, ok, errorCode, payload]
		    { dispatchPluginAsyncResult(pluginId, requestId, apiName, ok, errorCode, payload); });
		if (!queued)
		{
			qWarning().noquote() << QStringLiteral(
			    "[QMud][LuaBridge] failed to queue plugin async result dispatch to runtime thread");
		}
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::dispatchPluginAsyncResult");
	const QString normalizedId = normalizedPluginId(pluginId);
	if (normalizedId.isEmpty())
		return;

	const int pluginIndex = findPluginIndex(m_plugins, normalizedId);
	if (pluginIndex < 0)
		return;
	const Plugin &plugin = m_plugins.at(pluginIndex);
	if (!canExecutePlugin(plugin) || !plugin.lua)
		return;
	if (!shouldDispatchPluginAsyncResult(plugin, apiName))
		return;

	LuaBatchDispatchRequest request;
	request.kind          = LuaBatchDispatchKind::NumberAndUtf8StringsCount;
	request.engines       = {plugin.lua};
	request.functionName  = QStringLiteral("OnPluginAsyncResult");
	request.numberArg1    = requestId > static_cast<quint64>(std::numeric_limits<long>::max())
	                            ? std::numeric_limits<long>::max()
	                            : static_cast<long>(requestId);
	request.bytesArg      = apiName.toUtf8();
	request.bytesArg2     = ok ? QByteArrayLiteral("ok") : QByteArray::number(errorCode);
	request.bytesArg3     = payload.toUtf8();
	request.defaultResult = true;
	queuePluginCallbackDispatchAsync(request);
}

QString WorldRuntime::worldName() const
{
	return m_worldAttributes.value(QStringLiteral("name"));
}

QString WorldRuntime::chatOurName()
{
	QString chatName = m_worldAttributes.value(QStringLiteral("chat_name"));
	if (chatName.trimmed().isEmpty())
	{
		const QString fallback = worldName();
		if (!fallback.trimmed().isEmpty())
		{
			chatName = fallback;
			setWorldAttribute(QStringLiteral("chat_name"), chatName);
		}
	}
	if (chatName.trimmed().isEmpty())
		chatName = QStringLiteral("Name-not-set");
	return chatName;
}

int WorldRuntime::chatIncomingPort() const
{
	bool ok   = false;
	int  port = m_worldAttributes.value(QStringLiteral("chat_port")).toInt(&ok);
	if (!ok || port == 0)
		port = kDefaultChatPort;
	return port;
}

QString WorldRuntime::chatLocalAddress()
{
	const auto addresses = QNetworkInterface::allAddresses();
	for (const QHostAddress &addr : addresses)
	{
		if (addr.protocol() == QAbstractSocket::IPv4Protocol && !addr.isLoopback())
			return addr.toString();
	}
	return QStringLiteral("127.0.0.1");
}

bool WorldRuntime::validateIncomingChatCalls() const
{
	return isEnabledFlag(m_worldAttributes.value(QStringLiteral("validate_incoming_chat_calls")));
}

bool WorldRuntime::autoAllowFiles() const
{
	return isEnabledFlag(m_worldAttributes.value(QStringLiteral("auto_allow_files")));
}

bool WorldRuntime::autoAllowSnooping() const
{
	return isEnabledFlag(m_worldAttributes.value(QStringLiteral("auto_allow_snooping")));
}

QString WorldRuntime::chatSaveDirectory() const
{
	return m_worldAttributes.value(QStringLiteral("chat_file_save_directory"));
}

bool WorldRuntime::ignoreChatColours() const
{
	return isEnabledFlag(m_worldAttributes.value(QStringLiteral("ignore_chat_colours")));
}

int WorldRuntime::chatMaxLines() const
{
	return m_worldAttributes.value(QStringLiteral("chat_max_lines_per_message")).toInt();
}

int WorldRuntime::chatMaxBytes() const
{
	return m_worldAttributes.value(QStringLiteral("chat_max_bytes_per_message")).toInt();
}

long WorldRuntime::chatForegroundColour() const
{
	bool            ok    = false;
	const qlonglong value = m_worldAttributes.value(QStringLiteral("chat_foreground_colour")).toLongLong(&ok);
	return ok ? static_cast<long>(value) : colorToLong(QColor(255, 0, 0));
}

long WorldRuntime::chatBackgroundColour() const
{
	bool            ok    = false;
	const qlonglong value = m_worldAttributes.value(QStringLiteral("chat_background_colour")).toLongLong(&ok);
	return ok ? static_cast<long>(value) : colorToLong(QColor(0, 0, 0));
}

bool WorldRuntime::isChatAlreadyConnected(const QString &address, int port,
                                          const ChatConnection *exclude) const
{
	return std::ranges::any_of(chatConnections(),
	                           [&](const ChatConnection *connection)
	                           {
		                           return connection && connection != exclude && connection->isConnected() &&
		                                  connection->allegedAddress() == address &&
		                                  connection->allegedPort() == port;
	                           });
}

QList<WorldRuntime::ChatConnection *> WorldRuntime::chatConnections() const
{
	return m_chatConnections.values();
}

WorldRuntime::ChatConnection *WorldRuntime::chatConnectionById(long id) const
{
	const auto it = m_chatConnections.constFind(id);
	return it == m_chatConnections.constEnd() ? nullptr : it.value();
}

void WorldRuntime::removeChatConnection(ChatConnection *connection)
{
	if (!connection)
		return;
	const auto it = m_chatConnections.find(connection->id());
	if (it != m_chatConnections.end() && it.value() == connection)
		m_chatConnections.erase(it);
	connection->deleteLater();
}

int WorldRuntime::sendChatMessageToAll(int message, const QString &text, bool unlessIgnoring,
                                       bool incomingOnly, bool outgoingOnly, long exceptId,
                                       const QString &group, long stamp)
{
	int count = 0;
	for (ChatConnection *connection : chatConnections())
	{
		if (!connection || !connection->isConnected())
			continue;
		if (incomingOnly && !connection->isIncoming())
			continue;
		if (outgoingOnly && connection->isIncoming())
			continue;
		if (unlessIgnoring && connection->m_ignore)
			continue;
		if (exceptId != 0 && connection->id() == exceptId)
			continue;
		if (!group.isEmpty() && connection->m_group.compare(group, Qt::CaseInsensitive) != 0)
			continue;

		if (group.isEmpty())
			connection->m_countOutgoingAll++;
		else
			connection->m_countOutgoingGroup++;

		connection->sendChatMessage(message, text, stamp);
		++count;
	}

	if (count > 0)
	{
		if (message == kChatTextEverybody)
		{
			m_lastChatMessageSent = text;
			m_lastChatMessageTime = QDateTime::currentDateTime();
		}
		else if (message == kChatTextGroup)
		{
			m_lastChatGroupMessageSent = text;
			m_lastChatGroupMessageTime = QDateTime::currentDateTime();
		}
	}

	return count;
}

int WorldRuntime::chatAcceptCalls(short port)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eCannotCreateChatSocket,
		                          [this, port] { return chatAcceptCalls(port); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::chatAcceptCalls");
	if (m_chatServer)
		return eChatAlreadyListening;

	if (port != 0)
		setWorldAttribute(QStringLiteral("chat_port"), QString::number(port));

	const int incomingPort      = chatIncomingPort();
	bool      hasConfiguredPort = false;
	const int configuredPort =
	    m_worldAttributes.value(QStringLiteral("chat_port")).trimmed().toInt(&hasConfiguredPort);
	if (!hasConfiguredPort || configuredPort == 0)
		setWorldAttribute(QStringLiteral("chat_port"), QString::number(incomingPort));

	if (!isEnabledFlag(m_worldAttributes.value(QStringLiteral("accept_chat_connections"))))
		setWorldAttribute(QStringLiteral("accept_chat_connections"), QStringLiteral("1"));

	auto chatServer = std::make_unique<QTcpServer>(this);
	if (!chatServer->listen(QHostAddress::Any, static_cast<quint16>(incomingPort)))
	{
		const QString errorText = QStringLiteral("Cannot accept calls on port %1, code = %2 (%3)")
		                              .arg(incomingPort)
		                              .arg(static_cast<int>(chatServer->serverError()))
		                              .arg(chatServer->errorString());
		chatNote(0, errorText);
		return eCannotCreateChatSocket;
	}

	m_chatServer = chatServer.release();
	connect(m_chatServer, &QTcpServer::newConnection, this,
	        [this]
	        {
		        chatNote(0, QStringLiteral("Incoming chat call"));
		        while (m_chatServer && m_chatServer->hasPendingConnections())
		        {
			        auto *socket = m_chatServer->nextPendingConnection();
			        if (!socket)
				        continue;
			        auto connection = std::make_unique<ChatConnection>(this, socket);
			        connection->startIncoming();
			        auto *accepted = connection.release();
			        m_chatConnections.insert(accepted->id(), accepted);
			        chatNote(0, QStringLiteral("Accepted call from %1 port %2")
			                        .arg(socket->peerAddress().toString())
			                        .arg(socket->peerPort()));
		        }
	        });

	chatNote(0, QStringLiteral("Listening for chat connections on port %1").arg(incomingPort));
	return eOK;
}

int WorldRuntime::chatCall(const QString &server, long port, bool zChat)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eCannotCreateChatSocket,
		                          [this, server, port, zChat] { return chatCall(server, port, zChat); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::chatCall");
	const QString trimmed = server.trimmed();
	if (trimmed.isEmpty())
		return eBadParameter;

	auto socket     = std::make_unique<QTcpSocket>(this);
	auto connection = std::make_unique<ChatConnection>(this, socket.get());
	connection->startOutgoing(trimmed, static_cast<int>(port), zChat);
	const auto *releasedSocket = socket.release();
	auto       *released       = connection.release();
	m_chatConnections.insert(released->id(), released);
	Q_UNUSED(releasedSocket);
	return eOK;
}

int WorldRuntime::chatDisconnect(long id)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eChatIDNotFound, [this, id] { return chatDisconnect(id); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::chatDisconnect");
	ChatConnection *connection = chatConnectionById(id);
	if (!connection)
		return eChatIDNotFound;

	chatNote(0, QStringLiteral("Connection to %1 dropped.").arg(connection->m_remoteUserName));
	connection->close();
	return eOK;
}

int WorldRuntime::chatDisconnectAll()
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eOK, [this] { return chatDisconnectAll(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::chatDisconnectAll");
	int count = 0;
	for (ChatConnection *connection : chatConnections())
	{
		if (!connection || connection->m_deleteMe)
			continue;
		count++;
		connection->close();
	}
	chatNote(0, QStringLiteral("%1 connection%2 closed.")
	                .arg(count)
	                .arg(count == 1 ? QString() : QStringLiteral("s")));
	return count;
}

int WorldRuntime::chatEverybody(const QString &message, bool emote)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eNoChatConnections,
		                          [this, message, emote] { return chatEverybody(message, emote); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::chatEverybody");
	const QString bold    = QString::fromLatin1(ansiCode(kAnsiBold));
	const QString cyan    = QString::fromLatin1(ansiCode(kAnsiTextCyan));
	const QString red     = QString::fromLatin1(ansiCode(kAnsiTextRed));
	const QString ourName = chatOurName();

	QString       payload;
	if (emote)
		payload = QStringLiteral("\n%1%2%3 %4%5\n").arg(bold, cyan, ourName, message, red);
	else
		payload =
		    QStringLiteral("\n%1 chats to everybody, '%2%3%4%5'\n").arg(ourName, bold, cyan, message, red);

	const int count = sendChatMessageToAll(kChatTextEverybody, payload, true, false, false, 0, QString(), 0);
	if (count > 0)
	{
		if (emote)
			chatNote(
			    8,
			    QStringLiteral("You emote to everybody: %1%2%3 %4%5").arg(bold, cyan, ourName, message, red));
		else
			chatNote(8, QStringLiteral("You chat to everybody, '%1%2%3%4'").arg(bold, cyan, message, red));
		return eOK;
	}

	chatNote(8, QStringLiteral("No (relevant) chat connections."));
	return eNoChatConnections;
}

long WorldRuntime::chatGetId(const QString &who) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), 0L,
		                          [this, who] { return chatGetId(who); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::chatGetId");
	QString const trimmed = who.trimmed();
	if (trimmed.isEmpty())
		return 0;

	bool       ok = false;
	const long id = trimmed.toLong(&ok);
	if (ok)
	{
		if (chatConnectionById(id))
			return id;
		const_cast<WorldRuntime *>(this)->chatNote(13,
		                                           QStringLiteral("Chat ID %1 is not connected.").arg(id));
		return 0;
	}

	for (ChatConnection const *connection : chatConnections())
	{
		if (!connection || !connection->isConnected())
			continue;
		if (connection->m_remoteUserName.compare(trimmed, Qt::CaseInsensitive) == 0)
			return connection->id();
	}

	const_cast<WorldRuntime *>(this)->chatNote(13,
	                                           QStringLiteral("Cannot find connection \"%1\".").arg(trimmed));
	return 0;
}

int WorldRuntime::chatGroup(const QString &group, const QString &message, bool emote)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eNoChatConnections,
		                          [this, group, message, emote] { return chatGroup(group, message, emote); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::chatGroup");
	if (group.trimmed().isEmpty())
		return eBadParameter;

	const QString bold       = QString::fromLatin1(ansiCode(kAnsiBold));
	const QString cyan       = QString::fromLatin1(ansiCode(kAnsiTextCyan));
	const QString red        = QString::fromLatin1(ansiCode(kAnsiTextRed));
	const QString ourName    = chatOurName();
	const QString groupLabel = group.leftJustified(15, QLatin1Char(' '));

	QString       payload;
	if (emote)
		payload = QStringLiteral("%1\nTo the group, %2%3%4 %5%6\n")
		              .arg(groupLabel, bold, cyan, ourName, message, red);
	else
		payload = QStringLiteral("%1\n%2 chats to the group, '%3%4%5%6'\n")
		              .arg(groupLabel, ourName, bold, cyan, message, red);

	const int count = sendChatMessageToAll(kChatTextGroup, payload, true, false, false, 0, group, 0);
	if (count > 0)
	{
		if (emote)
			chatNote(9, QStringLiteral("You emote to the group %1: %2%3%4 %5%6")
			                .arg(group, bold, cyan, ourName, message, red));
		else
			chatNote(
			    9,
			    QStringLiteral("You chat to the group %1, '%2%3%4%5'").arg(group, bold, cyan, message, red));
		return eOK;
	}

	chatNote(9, QStringLiteral("No chat connections in the group %1.").arg(group));
	return eNoChatConnections;
}

int WorldRuntime::chatId(long id, const QString &message, bool emote)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eChatIDNotFound,
		                          [this, id, message, emote] { return chatId(id, message, emote); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::chatId");
	if (id == 0)
		return eChatIDNotFound;

	ChatConnection *connection = chatConnectionById(id);
	if (!connection)
	{
		chatNote(7, QStringLiteral("Chat ID %1 is not connected.").arg(id));
		return eChatIDNotFound;
	}

	const QString bold    = QString::fromLatin1(ansiCode(kAnsiBold));
	const QString cyan    = QString::fromLatin1(ansiCode(kAnsiTextCyan));
	const QString red     = QString::fromLatin1(ansiCode(kAnsiTextRed));
	const QString ourName = chatOurName();

	QString       payload;
	if (emote)
		payload = QStringLiteral("\nTo you, %1%2%3 %4%5\n").arg(bold, cyan, ourName, message, red);
	else
		payload = QStringLiteral("\n%1 chats to you, '%2%3%4%5'\n").arg(ourName, bold, cyan, message, red);

	connection->m_countOutgoingPersonal++;
	connection->sendChatMessage(kChatTextPersonal, payload);

	if (emote)
		chatNote(7, QStringLiteral("You emote to %1: %2%3%4 %5%6")
		                .arg(connection->m_remoteUserName, bold, cyan, ourName, message, red));
	else
		chatNote(7, QStringLiteral("You chat to %1, '%2%3%4%5'")
		                .arg(connection->m_remoteUserName, bold, cyan, message, red));
	return eOK;
}

int WorldRuntime::chatMessage(long id, short message, const QString &text) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), eChatIDNotFound,
		                          [this, id, message, text] { return chatMessage(id, message, text); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::chatMessage");
	ChatConnection *connection = chatConnectionById(id);
	if (!connection)
		return eChatIDNotFound;
	connection->sendChatMessage(message, text);
	return eOK;
}

int WorldRuntime::chatNameChange(const QString &newName)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eBadParameter, [this, newName] { return chatNameChange(newName); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::chatNameChange");
	if (newName.trimmed().isEmpty())
		return eBadParameter;
	QString oldName = m_worldAttributes.value(QStringLiteral("chat_name"));
	if (oldName.trimmed().isEmpty())
		oldName = QStringLiteral("<no name>");

	if (oldName != newName)
		setWorldAttribute(QStringLiteral("chat_name"), newName);

	chatNote(2, QStringLiteral("Your chat name changed from %1 to %2").arg(oldName, newName));

	sendChatMessageToAll(kChatNameChange, newName, false, false, false, 0, QString(), 0);
	return eOK;
}

void WorldRuntime::chatNote(short noteType, const QString &message)
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this, noteType, message] { chatNote(noteType, message); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::chatNote");
	QString text = message;
	if (text.startsWith(QLatin1Char('\n')))
		text.remove(0, 1);
	if (text.endsWith(QLatin1Char('\n')))
		text.chop(1);

	const int maxBytes = chatMaxBytes();
	if (maxBytes > 0)
	{
		QByteArray bytes = text.toLatin1();
		if (bytes.size() > maxBytes)
		{
			bytes = bytes.left(maxBytes);
			text  = QString::fromLatin1(bytes);
			text += QStringLiteral("\n[Chat message truncated, exceeds %1 bytes]").arg(maxBytes);
		}
	}

	const int maxLines = chatMaxLines();
	if (maxLines > 0)
	{
		int lines = 0;
		for (int i = 0; i < text.size(); ++i)
		{
			if (text.at(i) != QLatin1Char('\n'))
				continue;
			if (++lines >= maxLines)
			{
				const bool truncated = (i + 1 < text.size());
				text                 = text.left(i);
				if (truncated)
					text += QStringLiteral("\n[Chat message truncated, exceeds %1 lines]").arg(maxLines);
				break;
			}
		}
	}

	if (!callPluginCallbacksStopOnFalseWithNumberAndString(QStringLiteral("OnPluginChatDisplay"), noteType,
	                                                       text))
		return;

	QString output = text;
	if (ignoreChatColours())
		output = qmudStripAnsiEscapeCodes(output);

	const QString prefix = m_worldAttributes.value(QStringLiteral("chat_message_prefix"));
	output               = prefix + output;

	long foreValue = chatForegroundColour();
	long backValue = chatBackgroundColour();
	if (foreValue == backValue)
	{
		foreValue = colorToLong(QColor(255, 0, 0));
		backValue = colorToLong(QColor(0, 0, 0));
	}
	const QColor  foreColor(lowByteToInt(foreValue), lowByteToInt(foreValue >> 8),
	                        lowByteToInt(foreValue >> 16));
	const QColor  backColor(lowByteToInt(backValue), lowByteToInt(backValue >> 8),
	                        lowByteToInt(backValue >> 16));
	const QString colourPrefix = QStringLiteral("\x1b[38;2;%1;%2;%3m\x1b[48;2;%4;%5;%6m")
	                                 .arg(foreColor.red())
	                                 .arg(foreColor.green())
	                                 .arg(foreColor.blue())
	                                 .arg(backColor.red())
	                                 .arg(backColor.green())
	                                 .arg(backColor.blue());
	const auto    reset        = QStringLiteral("\x1b[0m");
	outputAnsiText(colourPrefix + output + reset + QLatin1Char('\n'), true);
}

int WorldRuntime::chatPasteEverybody()
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eNoChatConnections, [this] { return chatPasteEverybody(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::chatPasteEverybody");
	const QString contents = QGuiApplication::clipboard()->text();
	if (contents.isEmpty())
		return eClipboardEmpty;

	const QString bold = QString::fromLatin1(ansiCode(kAnsiBold));
	const QString cyan = QString::fromLatin1(ansiCode(kAnsiTextCyan));
	const QString red  = QString::fromLatin1(ansiCode(kAnsiTextRed));

	const QString payload = QStringLiteral("\n%1 pastes to everybody: \n\n%2%3%4%5\n")
	                            .arg(chatOurName(), bold, cyan, contents, red);

	const int count = sendChatMessageToAll(kChatTextEverybody, payload, true, false, false, 0, QString(), 0);
	if (count > 0)
	{
		chatNote(8, QStringLiteral("You paste to everybody: \n\n%1%2%3%4").arg(bold, cyan, contents, red));
		return eOK;
	}

	chatNote(8, QStringLiteral("No (relevant) chat connections."));
	return eNoChatConnections;
}

int WorldRuntime::chatPasteText(long id)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eChatIDNotFound, [this, id] { return chatPasteText(id); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::chatPasteText");
	if (id == 0)
		return eChatIDNotFound;

	ChatConnection *connection = chatConnectionById(id);
	if (!connection)
	{
		chatNote(7, QStringLiteral("Chat ID %1 is not connected.").arg(id));
		return eChatIDNotFound;
	}

	const QString contents = QGuiApplication::clipboard()->text();
	if (contents.isEmpty())
		return eClipboardEmpty;

	const QString bold = QString::fromLatin1(ansiCode(kAnsiBold));
	const QString cyan = QString::fromLatin1(ansiCode(kAnsiTextCyan));
	const QString red  = QString::fromLatin1(ansiCode(kAnsiTextRed));

	const QString payload =
	    QStringLiteral("\n%1 pastes to you: \n\n%2%3%4%5\n").arg(chatOurName(), bold, cyan, contents, red);
	connection->sendChatMessage(kChatTextPersonal, payload);
	chatNote(7, QStringLiteral("You paste to %1: \n\n%2%3%4%5")
	                .arg(connection->m_remoteUserName, bold, cyan, contents, red));
	return eOK;
}

int WorldRuntime::chatPeekConnections(long id) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), eChatIDNotFound,
		                          [this, id] { return chatPeekConnections(id); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::chatPeekConnections");
	ChatConnection *connection = chatConnectionById(id);
	if (!connection)
		return eChatIDNotFound;
	connection->sendChatMessage(kChatPeekConnections, QString());
	return eOK;
}

int WorldRuntime::chatPersonal(const QString &who, const QString &message, bool emote)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eChatPersonNotFound,
		                          [this, who, message, emote] { return chatPersonal(who, message, emote); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::chatPersonal");
	const QString trimmed = who.trimmed();
	if (trimmed.isEmpty())
		return eBadParameter;

	int count = 0;
	for (ChatConnection const *connection : chatConnections())
	{
		if (!connection || !connection->isConnected())
			continue;
		if (connection->m_remoteUserName.compare(trimmed, Qt::CaseInsensitive) != 0)
			continue;
		if (chatId(connection->id(), message, emote) == eOK)
			count++;
	}

	if (count == 0)
	{
		chatNote(7, QStringLiteral("%1 is not connected.").arg(trimmed));
		return eChatPersonNotFound;
	}
	if (count > 1)
		chatNote(7, QStringLiteral("%1 matches.").arg(count));
	return eOK;
}

int WorldRuntime::chatPing(long id) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), eChatIDNotFound,
		                          [this, id] { return chatPing(id); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::chatPing");
	ChatConnection *connection = chatConnectionById(id);
	if (!connection)
		return eChatIDNotFound;

	connection->m_pingTimer.start();
	const QString payload =
	    QDateTime::currentDateTime().toString(QStringLiteral("dddd, MMMM dd, yyyy, h:mm AP"));
	connection->sendChatMessage(kChatPingRequest, payload);
	return eOK;
}

int WorldRuntime::chatRequestConnections(long id) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), eChatIDNotFound,
		                          [this, id] { return chatRequestConnections(id); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::chatRequestConnections");
	ChatConnection *connection = chatConnectionById(id);
	if (!connection)
		return eChatIDNotFound;
	connection->sendChatMessage(kChatRequestConnections, QString());
	return eOK;
}

int WorldRuntime::chatSendFile(long id, const QString &path)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eChatIDNotFound, [this, id, path] { return chatSendFile(id, path); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::chatSendFile");
	if (id == 0)
		return eChatIDNotFound;

	ChatConnection *connection = chatConnectionById(id);
	if (!connection)
	{
		chatNote(14, QStringLiteral("Chat ID %1 is not connected.").arg(id));
		return eChatIDNotFound;
	}

	if (connection->m_doingFileTransfer)
	{
		if (connection->m_sendFile)
			chatNote(14, QStringLiteral("Already sending file %1").arg(connection->m_ourFileName));
		else
			chatNote(14, QStringLiteral("Already receiving file %1").arg(connection->m_ourFileName));
		return eAlreadyTransferringFile;
	}

	QString fileName = path;
	if (fileName.trimmed().isEmpty())
	{
		const QString initialDir = fileBrowsingDirectory();
		fileName = QFileDialog::getOpenFileName(nullptr, QStringLiteral("Select file to send"), initialDir,
		                                        QStringLiteral("All files (*.*)"));
		if (fileName.isEmpty())
			return eFileNotFound;
	}
	else
	{
		QString error;
		if (!QMudPluginPathUtils::resolveInsideQmudHome(m_startupDirectory, fileName, &fileName, &error))
			return eFileNotFound;
	}

	connection->m_ourFileName = fileName;
	QFileInfo const info(fileName);
	connection->m_senderFileName = info.fileName();

	connection->m_file = std::make_unique<QFile>(fileName);
	if (!connection->m_file->open(QIODevice::ReadOnly))
	{
		chatNote(14, QStringLiteral("File %1 cannot be opened.").arg(fileName));
		connection->m_file.reset();
		connection->m_ourFileName.clear();
		connection->m_fileSize = 0;
		return eFileNotFound;
	}

	connection->m_fileSize = static_cast<long>(connection->m_file->size());
	connection->m_fileBuffer.resize(connection->m_fileBlockSize);

	connection->sendChatMessage(
	    kChatFileStart,
	    QStringLiteral("%1,%2").arg(connection->m_senderFileName).arg(connection->m_fileSize));

	connection->m_startedFileTransfer = QDateTime::currentDateTime();
	connection->m_sendFile            = true;
	connection->m_doingFileTransfer   = true;
	connection->m_blocksTransferred   = 0;
	connection->m_fileBlocks =
	    (connection->m_fileSize + connection->m_fileBlockSize - 1L) / connection->m_fileBlockSize;

	connection->m_fileSha1.reset();
	const double kb = static_cast<double>(connection->m_fileSize) / 1024.0;
	chatNote(14, QStringLiteral("Initiated transfer of file %1, %2 bytes (%3 Kb).")
	                 .arg(fileName)
	                 .arg(connection->m_fileSize)
	                 .arg(kb, 0, 'f', 1));
	return eOK;
}

int WorldRuntime::chatStopAcceptingCalls()
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eOK, [this] { return chatStopAcceptingCalls(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::chatStopAcceptingCalls");
	if (m_chatServer)
	{
		m_chatServer->close();
		m_chatServer->deleteLater();
		m_chatServer = nullptr;
		chatNote(0, QStringLiteral("Stopped accepting chat connections."));
		setWorldAttribute(QStringLiteral("accept_chat_connections"), QStringLiteral("0"));
	}
	return eOK;
}

int WorldRuntime::chatStopFileTransfer(long id)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eChatIDNotFound, [this, id] { return chatStopFileTransfer(id); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::chatStopFileTransfer");
	if (id == 0)
		return eChatIDNotFound;

	ChatConnection *connection = chatConnectionById(id);
	if (!connection)
	{
		chatNote(7, QStringLiteral("Chat ID %1 is not connected.").arg(id));
		return eChatIDNotFound;
	}

	if (!connection->m_doingFileTransfer)
		return eNotTransferringFile;

	connection->stopFileTransfer(true);
	return eOK;
}

QVariant WorldRuntime::chatInfo(long id, int infoType) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QVariant(),
		                          [this, id, infoType] { return chatInfo(id, infoType); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::chatInfo");
	const ChatConnection *connection = chatConnectionById(id);
	if (!connection)
		return {};

	switch (infoType)
	{
	case 1:
		return connection->m_serverName;
	case 2:
		return connection->m_remoteUserName;
	case 3:
		return connection->m_group;
	case 4:
		return connection->m_remoteVersion;
	case 5:
		return connection->m_allegedAddress;
	case 6:
		return connection->m_serverAddress.toString();
	case 7:
		return connection->m_serverPort;
	case 8:
		return connection->m_allegedPort;
	case 9:
		return connection->m_chatStatus;
	case 10:
		return connection->m_chatConnectionType;
	case 11:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_chatId));
	case 12:
		return connection->m_incoming;
	case 13:
		return connection->m_canSnoop;
	case 14:
		return connection->m_youAreSnooping;
	case 15:
		return connection->m_heIsSnooping;
	case 16:
		return connection->m_canSendCommands;
	case 17:
		return connection->m_private;
	case 18:
		return connection->m_canSendFiles;
	case 19:
		return connection->m_ignore;
	case 20:
		return connection->m_lastPingTime;
	case 21:
		return connection->m_whenStarted.isValid() ? QVariant(toLuaSystemTime(connection->m_whenStarted))
		                                           : QVariant();
	case 22:
		return connection->m_lastIncoming.isValid() ? QVariant(toLuaSystemTime(connection->m_lastIncoming))
		                                            : QVariant();
	case 23:
		return connection->m_lastOutgoing.isValid() ? QVariant(toLuaSystemTime(connection->m_lastOutgoing))
		                                            : QVariant();
	case 24:
		return connection->m_startedFileTransfer.isValid()
		           ? QVariant(toLuaSystemTime(connection->m_startedFileTransfer))
		           : QVariant();
	case 25:
		return connection->m_doingFileTransfer;
	case 26:
		return connection->m_sendFile;
	case 27:
		return connection->m_senderFileName;
	case 28:
		return connection->m_ourFileName;
	case 29:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_fileSize));
	case 30:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_fileBlocks));
	case 31:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_blocksTransferred));
	case 32:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_fileBlockSize));
	case 33:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_countIncomingPersonal));
	case 34:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_countIncomingAll));
	case 35:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_countIncomingGroup));
	case 36:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_countOutgoingPersonal));
	case 37:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_countOutgoingAll));
	case 38:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_countOutgoingGroup));
	case 39:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_countMessages));
	case 40:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_countFileBytesIn));
	case 41:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_countFileBytesOut));
	case 42:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_zChatStamp));
	case 43:
		return connection->m_emailAddress;
	case 44:
		return connection->m_pgpKey;
	case 45:
		return connection->m_zChatStatus;
	case 46:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_userOption));
	default:
		break;
	}
	return {};
}

QList<long> WorldRuntime::chatList() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QList<long>{},
		                          [this] { return chatList(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::chatList");
	QList<long> result;
	for (ChatConnection const *connection : chatConnections())
	{
		if (connection && connection->isConnected())
			result.push_back(connection->id());
	}
	return result;
}

QVariant WorldRuntime::chatOption(long id, const QString &optionName) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QVariant(),
		                          [this, id, optionName] { return chatOption(id, optionName); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::chatOption");
	const ChatConnection *connection = chatConnectionById(id);
	if (!connection)
		return {};

	const QString name = optionName.trimmed().toLower();
	if (name == QStringLiteral("can_send_commands"))
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_canSendCommands ? 1 : 0));
	if (name == QStringLiteral("can_send_files"))
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_canSendFiles ? 1 : 0));
	if (name == QStringLiteral("can_snoop"))
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_canSnoop ? 1 : 0));
	if (name == QStringLiteral("ignore"))
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_ignore ? 1 : 0));
	if (name == QStringLiteral("served"))
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_incoming ? 1 : 0));
	if (name == QStringLiteral("private"))
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_private ? 1 : 0));
	if (name == QStringLiteral("user"))
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_userOption));
	if (name == QStringLiteral("server"))
		return connection->m_serverName;
	if (name == QStringLiteral("username"))
		return connection->m_remoteUserName;
	if (name == QStringLiteral("group"))
		return connection->m_group;
	if (name == QStringLiteral("version"))
		return connection->m_remoteVersion;
	if (name == QStringLiteral("address"))
		return connection->m_allegedAddress;
	return {};
}

int WorldRuntime::chatSetOption(long id, const QString &optionName, const QString &value)
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, eChatIDNotFound, [this, id, optionName, value]
		                          { return chatSetOption(id, optionName, value); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::chatSetOption");
	ChatConnection *connection = chatConnectionById(id);
	if (!connection)
		return eChatIDNotFound;

	const QString name    = optionName.trimmed().toLower();
	const QString trimmed = value.trimmed();

	auto          parseBool = [&](bool &ok) -> long
	{
		if (trimmed.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0)
		{
			ok = true;
			return 1;
		}
		if (trimmed.compare(QStringLiteral("n"), Qt::CaseInsensitive) == 0)
		{
			ok = true;
			return 0;
		}
		bool       numericOk = false;
		const long num       = trimmed.toLong(&numericOk);
		ok                   = numericOk;
		return num;
	};

	if (name == QStringLiteral("can_send_commands") || name == QStringLiteral("can_send_files") ||
	    name == QStringLiteral("can_snoop") || name == QStringLiteral("ignore") ||
	    name == QStringLiteral("served") || name == QStringLiteral("private") ||
	    name == QStringLiteral("user"))
	{
		bool       ok      = false;
		const long numeric = name == QStringLiteral("user") ? trimmed.toLong(&ok) : parseBool(ok);
		if (!ok)
			return eOptionOutOfRange;

		bool changed = false;
		if (name == QStringLiteral("can_send_commands"))
		{
			changed                       = connection->m_canSendCommands != (numeric != 0);
			connection->m_canSendCommands = (numeric != 0);
			if (changed)
				connection->sendChatMessage(kChatMessage, QStringLiteral("You can %1send %2 commands")
				                                              .arg(connection->m_canSendCommands
				                                                       ? QStringLiteral("now ")
				                                                       : QStringLiteral("no longer "))
				                                              .arg(chatOurName()));
		}
		else if (name == QStringLiteral("can_send_files"))
		{
			changed                    = connection->m_canSendFiles != (numeric != 0);
			connection->m_canSendFiles = (numeric != 0);
			if (changed)
				connection->sendChatMessage(kChatMessage, QStringLiteral("You can %1send %2 files")
				                                              .arg(connection->m_canSendFiles
				                                                       ? QStringLiteral("now ")
				                                                       : QStringLiteral("no longer "))
				                                              .arg(chatOurName()));
		}
		else if (name == QStringLiteral("can_snoop"))
		{
			changed                = connection->m_canSnoop != (numeric != 0);
			connection->m_canSnoop = (numeric != 0);
			if (changed)
				connection->sendChatMessage(
				    kChatMessage,
				    QStringLiteral("You can %1snoop %2")
				        .arg(connection->m_canSnoop ? QStringLiteral("now ") : QStringLiteral("no longer "))
				        .arg(chatOurName()));
		}
		else if (name == QStringLiteral("ignore"))
		{
			changed              = connection->m_ignore != (numeric != 0);
			connection->m_ignore = (numeric != 0);
			if (changed)
				connection->sendChatMessage(
				    kChatMessage,
				    QStringLiteral("%1 is %2ignoring you")
				        .arg(chatOurName(), connection->m_ignore ? QString() : QStringLiteral("no longer ")));
		}
		else if (name == QStringLiteral("served"))
		{
			changed                = connection->m_incoming != (numeric != 0);
			connection->m_incoming = (numeric != 0);
		}
		else if (name == QStringLiteral("private"))
		{
			changed               = connection->m_private != (numeric != 0);
			connection->m_private = (numeric != 0);
			if (changed)
				connection->sendChatMessage(
				    kChatMessage, QStringLiteral("%1 has marked your connection as %2")
				                      .arg(chatOurName(), connection->m_private ? QStringLiteral("private")
				                                                                : QStringLiteral("public")));
		}
		else if (name == QStringLiteral("user"))
		{
			changed                  = connection->m_userOption != numeric;
			connection->m_userOption = numeric;
		}

		Q_UNUSED(changed);
		return eOK;
	}

	if (name == QStringLiteral("group"))
	{
		const bool changed  = connection->m_group != trimmed;
		connection->m_group = trimmed;
		if (changed)
		{
			if (!trimmed.isEmpty())
				connection->sendChatMessage(
				    kChatMessage,
				    QStringLiteral("%1 has added you to the group %2").arg(chatOurName(), trimmed));
			else
				connection->sendChatMessage(
				    kChatMessage,
				    QStringLiteral("%1 has removed you from the chat group").arg(chatOurName()));
		}
		return eOK;
	}

	if (name == QStringLiteral("server") || name == QStringLiteral("username") ||
	    name == QStringLiteral("version") || name == QStringLiteral("address"))
		return ePluginCannotSetOption;

	return eUnknownOption;
}

QVariant WorldRuntime::pluginInfo(const QString &pluginId, int infoType) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QVariant(),
		                          [this, pluginId, infoType] { return pluginInfo(pluginId, infoType); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::pluginInfo");
	if (QMudNativePluginRegistry::isBlacklistedId(pluginId))
		return {};
	if (const QString shimId = QMudNativePluginRegistry::resolveShimIdOrName(pluginId); !shimId.isEmpty())
	{
		if (!nativeShimVisibleForPluginApi(m_plugins, shimId))
			return {};
		int               visibleIndex = 0;
		const QStringList ids          = pluginIdList();
		const bool        enabled      = nativeShimPluginInfoEnabled(m_plugins, shimId);
		for (int i = 0; i < ids.size(); ++i)
		{
			if (ids.at(i).compare(shimId, Qt::CaseInsensitive) == 0)
			{
				visibleIndex = i + 1;
				break;
			}
		}
		return QMudNativePluginRegistry::pluginInfo(shimId, infoType, visibleIndex, enabled);
	}
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return {};

	const Plugin &plugin = m_plugins.at(index);
	switch (infoType)
	{
	case 1:
		return plugin.attributes.value(QStringLiteral("name"));
	case 2:
		return plugin.attributes.value(QStringLiteral("author"));
	case 3:
		return plugin.description;
	case 4:
		return plugin.script;
	case 5:
		return plugin.attributes.value(QStringLiteral("language"));
	case 6:
		return plugin.source;
	case 7:
		return plugin.attributes.value(QStringLiteral("id"));
	case 8:
		return plugin.attributes.value(QStringLiteral("purpose"));
	case 9:
		return plugin.triggers.size();
	case 10:
		return plugin.aliases.size();
	case 11:
		return plugin.timers.size();
	case 12:
		return plugin.variables.size();
	case 13:
		return plugin.dateWritten.isValid() ? QVariant(plugin.dateWritten) : QVariant();
	case 14:
		return plugin.dateModified.isValid() ? QVariant(plugin.dateModified) : QVariant();
	case 15:
		return plugin.saveState;
	case 16:
		return plugin.lua != nullptr;
	case 17:
		return plugin.enabled;
	case 18:
		return plugin.requiredVersion;
	case 19:
		return plugin.version;
	case 20:
	{
		QString dir = plugin.directory;
		if (!dir.isEmpty() && !dir.endsWith('/') && !dir.endsWith('\\'))
			dir += '/';
		dir.replace(QLatin1Char('\\'), QLatin1Char('/'));
		return dir;
	}
	case 21:
		return index + 1;
	case 22:
		return plugin.dateInstalled.isValid() ? QVariant(plugin.dateInstalled) : QVariant();
	case 23:
		return plugin.callingPluginId;
	case 24:
		return 0.0;
	case 25:
		return plugin.sequence;
	default:
		break;
	}
	return {};
}

QStringList WorldRuntime::pluginIdList() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QStringList{},
		                          [this] { return pluginIdList(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::pluginIdList");
	QStringList ids;
	for (const Plugin &plugin : m_plugins)
	{
		const QString id = plugin.attributes.value(QStringLiteral("id"));
		if (!id.isEmpty() && !QMudNativePluginRegistry::isBlacklistedId(id))
			ids.push_back(id);
	}
	for (const QString &shimId :
	     {QMudNativePluginRegistry::mushReaderPluginId(), QMudNativePluginRegistry::luaAudioPluginId()})
	{
		if (nativeShimVisibleForPluginApi(m_plugins, shimId) && !ids.contains(shimId, Qt::CaseInsensitive))
			ids.push_back(shimId);
	}
	return ids;
}

QString WorldRuntime::pluginVariableValue(const QString &pluginId, const QString &name) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QString(),
		                          [this, pluginId, name] { return pluginVariableValue(pluginId, name); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::pluginVariableValue");
	QString value;
	if (findPluginVariable(pluginId, name, value))
		return value;
	return {};
}

bool WorldRuntime::findPluginVariable(const QString &pluginId, const QString &name, QString &value) const
{
	if (QThread::currentThread() != thread())
	{
		const auto resolved =
		    qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), std::pair<bool, QString>{false, {}},
		                       [this, pluginId, name]
		                       {
			                       QString    localValue;
			                       const bool found = findPluginVariable(pluginId, name, localValue);
			                       return std::pair<bool, QString>{found, localValue};
		                       });
		if (resolved.first)
			value = resolved.second;
		return resolved.first;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::findPluginVariable");
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return false;
	const Plugin &plugin = m_plugins.at(index);
	for (auto it = plugin.variables.constBegin(); it != plugin.variables.constEnd(); ++it)
	{
		if (it.key().compare(name, Qt::CaseInsensitive) == 0)
		{
			value = it.value();
			return true;
		}
	}
	return false;
}

void WorldRuntime::setPluginVariableValue(const QString &pluginId, const QString &name, const QString &value)
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this, pluginId, name, value]
		                        { setPluginVariableValue(pluginId, name, value); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::setPluginVariableValue");
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0 || name.isEmpty())
		return;
	Plugin &plugin = m_plugins[index];
	for (auto it = plugin.variables.begin(); it != plugin.variables.end(); ++it)
	{
		if (it.key().compare(name, Qt::CaseInsensitive) == 0)
		{
			it.value() = value;
			invalidateLuaCallbackDispatchSnapshot();
			return;
		}
	}
	plugin.variables.insert(name, value);
	invalidateLuaCallbackDispatchSnapshot();
}

QStringList WorldRuntime::pluginVariableList(const QString &pluginId) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QStringList{},
		                          [this, pluginId] { return pluginVariableList(pluginId); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::pluginVariableList");
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return {};
	const Plugin &plugin = m_plugins.at(index);
	return plugin.variables.keys();
}

bool WorldRuntime::pluginVariableSnapshot(const QString &pluginId, QMap<QString, QString> &values) const
{
	values.clear();
	if (QThread::currentThread() != thread())
	{
		const auto resolved = qmudInvokeMethodOr(
		    const_cast<WorldRuntime *>(this), std::pair<bool, QMap<QString, QString>>{false, {}},
		    [this, pluginId]
		    {
			    QMap<QString, QString> localValues;
			    const bool             found = pluginVariableSnapshot(pluginId, localValues);
			    return std::pair<bool, QMap<QString, QString>>{found, localValues};
		    });
		if (resolved.first)
			values = resolved.second;
		return resolved.first;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::pluginVariableSnapshot");
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return false;
	values = m_plugins.at(index).variables;
	return true;
}

QStringList WorldRuntime::pluginTriggerList(const QString &pluginId) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QStringList{},
		                          [this, pluginId] { return pluginTriggerList(pluginId); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::pluginTriggerList");
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return {};
	QStringList names;
	for (const auto &trigger : m_plugins.at(index).triggers)
	{
		const QString name = trigger.attributes.value(QStringLiteral("name"));
		if (!name.isEmpty())
			names.push_back(name);
	}
	return names;
}

QStringList WorldRuntime::pluginAliasList(const QString &pluginId) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QStringList{},
		                          [this, pluginId] { return pluginAliasList(pluginId); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::pluginAliasList");
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return {};
	QStringList names;
	for (const auto &alias : m_plugins.at(index).aliases)
	{
		const QString name = alias.attributes.value(QStringLiteral("name"));
		if (!name.isEmpty())
			names.push_back(name);
	}
	return names;
}

QStringList WorldRuntime::pluginTimerList(const QString &pluginId) const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), QStringList{},
		                          [this, pluginId] { return pluginTimerList(pluginId); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::pluginTimerList");
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return {};
	QStringList names;
	for (const auto &timer : m_plugins.at(index).timers)
	{
		const QString name = timer.attributes.value(QStringLiteral("name"));
		if (!name.isEmpty())
			names.push_back(name);
	}
	return names;
}

namespace
{
	QString normalizeLabel(const QString &name)
	{
		return name.trimmed().toLower();
	}

	bool resolveWildcardValue(const QStringList &wildcards, const QMap<QString, QString> &namedWildcards,
	                          const QString &wildcardName, QString &value)
	{
		if (wildcardName.isEmpty())
			return false;
		bool      okNumber = false;
		const int index    = wildcardName.toInt(&okNumber);
		if (okNumber)
		{
			if (index >= 0 && index < wildcards.size())
			{
				value = wildcards.at(index);
				return true;
			}
			return false;
		}
		if (namedWildcards.contains(wildcardName))
		{
			value = namedWildcards.value(wildcardName);
			return true;
		}
		return false;
	}
} // namespace

void WorldRuntime::callPluginCallbacksWithTwoNumbersAndString(const QString &functionName, long arg1,
                                                              long arg2, const QString &arg3,
                                                              const bool completionBarrier)
{
	const QVector<QSharedPointer<LuaCallbackEngine>> recipients =
	    collectPluginCallbackRecipients(functionName);
	if (recipients.isEmpty())
		return;
	LuaBatchDispatchRequest request;
	request.kind                         = LuaBatchDispatchKind::TwoNumbersAndString;
	request.engines                      = recipients;
	request.functionName                 = functionName;
	request.numberArg1                   = arg1;
	request.numberArg2                   = arg2;
	request.stringArg2                   = arg3;
	request.defaultResult                = true;
	request.revalidateObservedRecipients = true;
	static_cast<void>(queuePluginCallbackDispatch(request, completionBarrier));
}

void WorldRuntime::setTriggerWildcards(const QString &triggerName, const QStringList &wildcards,
                                       const QMap<QString, QString> &namedWildcards)
{
	const QString key = normalizeLabel(triggerName);
	if (key.isEmpty())
		return;
	m_triggerWildcards.insert(key, wildcards);
	m_triggerNamedWildcards.insert(key, namedWildcards);
}

void WorldRuntime::setAliasWildcards(const QString &aliasName, const QStringList &wildcards,
                                     const QMap<QString, QString> &namedWildcards)
{
	const QString key = normalizeLabel(aliasName);
	if (key.isEmpty())
		return;
	m_aliasWildcards.insert(key, wildcards);
	m_aliasNamedWildcards.insert(key, namedWildcards);
}

bool WorldRuntime::triggerWildcard(const QString &triggerName, const QString &wildcardName,
                                   QString &value) const
{
	const QString key = normalizeLabel(triggerName);
	if (key.isEmpty() || !m_triggerWildcards.contains(key))
		return false;
	const QStringList            wildcards = m_triggerWildcards.value(key);
	const QMap<QString, QString> named     = m_triggerNamedWildcards.value(key);
	return resolveWildcardValue(wildcards, named, wildcardName, value);
}

bool WorldRuntime::aliasWildcard(const QString &aliasName, const QString &wildcardName, QString &value) const
{
	const QString key = normalizeLabel(aliasName);
	if (key.isEmpty() || !m_aliasWildcards.contains(key))
		return false;
	const QStringList            wildcards = m_aliasWildcards.value(key);
	const QMap<QString, QString> named     = m_aliasNamedWildcards.value(key);
	return resolveWildcardValue(wildcards, named, wildcardName, value);
}

void WorldRuntime::setPluginTriggerWildcards(const QString &pluginId, const QString &triggerName,
                                             const QStringList            &wildcards,
                                             const QMap<QString, QString> &namedWildcards)
{
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return;
	Plugin       &plugin = m_plugins[index];
	const QString key    = normalizeLabel(triggerName);
	if (key.isEmpty())
		return;
	plugin.triggerWildcards.insert(key, wildcards);
	plugin.triggerNamedWildcards.insert(key, namedWildcards);
}

void WorldRuntime::setPluginAliasWildcards(const QString &pluginId, const QString &aliasName,
                                           const QStringList            &wildcards,
                                           const QMap<QString, QString> &namedWildcards)
{
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return;
	Plugin       &plugin = m_plugins[index];
	const QString key    = normalizeLabel(aliasName);
	if (key.isEmpty())
		return;
	plugin.aliasWildcards.insert(key, wildcards);
	plugin.aliasNamedWildcards.insert(key, namedWildcards);
}

bool WorldRuntime::pluginTriggerWildcard(const QString &pluginId, const QString &triggerName,
                                         const QString &wildcardName, QString &value) const
{
	if (QThread::currentThread() != thread())
	{
		const auto resolved = qmudInvokeMethodOr(
		    const_cast<WorldRuntime *>(this), std::pair<bool, QString>{false, {}},
		    [this, pluginId, triggerName, wildcardName]
		    {
			    QString    resolvedValue;
			    const bool found = pluginTriggerWildcard(pluginId, triggerName, wildcardName, resolvedValue);
			    return std::pair<bool, QString>{found, resolvedValue};
		    });
		if (resolved.first)
			value = resolved.second;
		return resolved.first;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::pluginTriggerWildcard");
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return false;
	const Plugin &plugin = m_plugins.at(index);
	const QString key    = normalizeLabel(triggerName);
	if (key.isEmpty() || !plugin.triggerWildcards.contains(key))
		return false;
	const QStringList            wildcards = plugin.triggerWildcards.value(key);
	const QMap<QString, QString> named     = plugin.triggerNamedWildcards.value(key);
	return resolveWildcardValue(wildcards, named, wildcardName, value);
}

bool WorldRuntime::pluginAliasWildcard(const QString &pluginId, const QString &aliasName,
                                       const QString &wildcardName, QString &value) const
{
	if (QThread::currentThread() != thread())
	{
		const auto resolved =
		    qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), std::pair<bool, QString>{false, {}},
		                       [this, pluginId, aliasName, wildcardName]
		                       {
			                       QString    resolvedValue;
			                       const bool found =
			                           pluginAliasWildcard(pluginId, aliasName, wildcardName, resolvedValue);
			                       return std::pair<bool, QString>{found, resolvedValue};
		                       });
		if (resolved.first)
			value = resolved.second;
		return resolved.first;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::pluginAliasWildcard");
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return false;
	const Plugin &plugin = m_plugins.at(index);
	const QString key    = normalizeLabel(aliasName);
	if (key.isEmpty() || !plugin.aliasWildcards.contains(key))
		return false;
	const QStringList            wildcards = plugin.aliasWildcards.value(key);
	const QMap<QString, QString> named     = plugin.aliasNamedWildcards.value(key);
	return resolveWildcardValue(wildcards, named, wildcardName, value);
}

WorldRuntime::Plugin *WorldRuntime::pluginForId(const QString &pluginId)
{
	if (QMudNativePluginRegistry::isBlacklistedId(pluginId))
		return nullptr;
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return nullptr;
	return &m_plugins[index];
}

const WorldRuntime::Plugin *WorldRuntime::pluginForId(const QString &pluginId) const
{
	if (QMudNativePluginRegistry::isBlacklistedId(pluginId))
		return nullptr;
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return nullptr;
	return &m_plugins[index];
}

int WorldRuntime::savePluginState(const QString &pluginId, bool scripted, QString *error)
{
	if (QThread::currentThread() != thread())
	{
		const auto resolved = qmudInvokeMethodOr(this, std::pair<int, QString>{ePluginCouldNotSaveState, {}},
		                                         [this, pluginId, scripted, needsError = (error != nullptr)]
		                                         {
			                                         QString   localError;
			                                         QString  *errorPtr = needsError ? &localError : nullptr;
			                                         const int result =
			                                             savePluginState(pluginId, scripted, errorPtr);
			                                         return std::pair<int, QString>{result, localError};
		                                         });
		if (error)
			*error = resolved.second;
		return resolved.first;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::savePluginState");
	const QString trimmed = pluginId.trimmed();
	if (trimmed.isEmpty())
		return eNotAPlugin;
	const int index = findPluginIndex(m_plugins, trimmed);
	if (index < 0)
		return eNoSuchPlugin;
	return savePluginStateForPlugin(m_plugins[index], scripted, error);
}

int WorldRuntime::savePluginStateForPlugin(Plugin &plugin, bool scripted, QString *error)
{
	if (!scripted && !plugin.saveState)
		return eOK;
	if (plugin.savingStateNow)
		return ePluginCouldNotSaveState;
	if (m_stateFilesDirectory.isEmpty())
		return scripted ? ePluginCouldNotSaveState : eOK;
	const QString worldId = m_worldAttributes.value(QStringLiteral("id")).trimmed();
	if (worldId.isEmpty())
		return scripted ? ePluginCouldNotSaveState : eOK;
	const QString pluginId = plugin.attributes.value(QStringLiteral("id")).trimmed();
	if (pluginId.isEmpty())
		return scripted ? ePluginCouldNotSaveState : eOK;

	QString base = m_stateFilesDirectory;
	if (!base.endsWith('/') && !base.endsWith('\\'))
		base += '/';
	const QDir baseDir(base);
	if (!baseDir.exists())
		return scripted ? ePluginCouldNotSaveState : eOK;

	plugin.savingStateNow = true;
	if (plugin.lua)
		dispatchSingleEngineNoArgCallback(plugin.lua, QStringLiteral("OnPluginSaveState"), true);
	plugin.savingStateNow = false;

	const QString stateFile = base + worldId + "-" + pluginId + "-state.xml";
	QSaveFile     file(stateFile);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		if (error)
			*error = QStringLiteral("Unable to create plugin state file: %1").arg(stateFile);
		return scripted ? ePluginCouldNotSaveState : eOK;
	}

	QTextStream out(&file);
	out.setEncoding(QStringConverter::Utf8);
	const auto      nl = QStringLiteral("\r\n");

	const QDateTime now     = QDateTime::currentDateTime();
	const QString   savedOn = QLocale::system().toString(now, QStringLiteral("dddd, MMMM dd, yyyy, h:mm AP"));

	out << R"(<?xml version="1.0" encoding="utf-8"?>)" << nl;
	out << "<!DOCTYPE qmud>" << nl;
	out << "<!-- Saved on " << fixHtmlString(savedOn) << " -->" << nl;
	out << "<!-- QMud version " << kVersionString << " -->" << nl;
	out << "<!-- Written by Panagiotis Kalogiratos -->" << nl;
	out << "<!-- Home Page: https://github.com/Nodens-/QMud -->" << nl;
	out << "<qmud>" << nl;
	out << nl << "<!-- variables -->" << nl;
	out << "<variables>" << nl;

	for (auto it = plugin.variables.constBegin(); it != plugin.variables.constEnd(); ++it)
	{
		out << "  <variable name=\"" << fixHtmlString(it.key()) << "\">" << fixHtmlMultilineString(it.value())
		    << "</variable>" << nl;
	}

	out << "</variables>" << nl;
	out << "</qmud>" << nl;

	if (!file.commit())
	{
		if (error)
			*error = QStringLiteral("Unable to write plugin state file: %1").arg(stateFile);
		return scripted ? ePluginCouldNotSaveState : eOK;
	}

	return eOK;
}

void WorldRuntime::sortPluginsBySequence()
{
	std::ranges::stable_sort(m_plugins,
	                         [](const Plugin &a, const Plugin &b) { return a.sequence < b.sequence; });
}

const QList<WorldRuntime::Include> &WorldRuntime::includes() const
{
	return m_includes;
}

const QList<WorldRuntime::Script> &WorldRuntime::scripts() const
{
	return m_scripts;
}

QString WorldRuntime::comments() const
{
	return m_comments;
}

void WorldRuntime::addLine(const QString &text, int flags, bool hardReturn, const QDateTime &time)
{
	if (m_sessionStateOutputBufferSealed)
		return;

	LineEntry entry;
	entry.text       = text;
	entry.flags      = flags;
	entry.hardReturn = hardReturn;
	entry.spans.clear();
	entry.time       = time;
	entry.lineNumber = m_nextLineNumber++;
	if (m_lineTimer.isValid())
	{
		entry.ticks   = static_cast<double>(m_lineTimer.nsecsElapsed()) / 1000000000.0;
		entry.elapsed = entry.ticks;
	}
	else
	{
		const double fallback = (m_worldStartTime.isValid() && time.isValid())
		                            ? static_cast<double>(m_worldStartTime.msecsTo(time)) / 1000.0
		                            : 0.0;
		entry.ticks           = fallback;
		entry.elapsed         = fallback;
	}
	m_lines.push_back(entry);
	static_cast<void>(enforceOutputLineLimit());
	invalidateLuaCallbackLineBufferSnapshot();
}

void WorldRuntime::addLine(const QString &text, int flags, const QVector<StyleSpan> &spans, bool hardReturn,
                           const QDateTime &time)
{
	if (m_sessionStateOutputBufferSealed)
		return;

	LineEntry entry;
	entry.text       = text;
	entry.flags      = flags;
	entry.hardReturn = hardReturn;
	entry.spans      = spans;
	entry.time       = time;
	entry.lineNumber = m_nextLineNumber++;
	if (m_lineTimer.isValid())
	{
		entry.ticks   = static_cast<double>(m_lineTimer.nsecsElapsed()) / 1000000000.0;
		entry.elapsed = entry.ticks;
	}
	else
	{
		const double fallback = (m_worldStartTime.isValid() && time.isValid())
		                            ? static_cast<double>(m_worldStartTime.msecsTo(time)) / 1000.0
		                            : 0.0;
		entry.ticks           = fallback;
		entry.elapsed         = fallback;
	}
	m_lines.push_back(entry);
	static_cast<void>(enforceOutputLineLimit());
	invalidateLuaCallbackLineBufferSnapshot();
}

bool WorldRuntime::commitPendingIncomingPartialLine()
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(this, false, [this] { return commitPendingIncomingPartialLine(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::commitPendingIncomingPartialLine");
	if (m_sessionStateOutputBufferSealed)
		return false;
	if (m_partialLineText.isEmpty() && m_partialLineSpans.isEmpty())
		return false;

	QString            text  = m_partialLineText;
	QVector<StyleSpan> spans = m_partialLineSpans;
	m_partialLineText.clear();
	m_partialLineSpans.clear();
	m_pendingCarriageReturnOverwrite = false;
	++m_incomingPartialLineRevision;

	const bool                  serverSideWrapActive = isConnected() && m_telnet.isNawsNegotiated();
	const FixedColumnWrapConfig wrapConfig =
	    serverSideWrapActive
	        ? FixedColumnWrapConfig{}
	        : localOutputWrapConfig(m_worldAttributes, serverSideWrapActive, m_telnet.windowColumns());
	if (wrapConfig.enabled && !text.isEmpty())
	{
		if (spans.isEmpty())
			wrapPlainLineForColumn(text, wrapConfig.wrapColumn, wrapConfig.indentParas);
		else
			wrapStyledLineForColumn(text, spans, wrapConfig.wrapColumn, wrapConfig.indentParas);
	}

	if (spans.isEmpty())
		addLine(text, LineOutput, true);
	else
		addLine(text, LineOutput, spans, true);
	QMudNativePluginRegistry::clearMushReaderPartialLine(this);
	return true;
}

int WorldRuntime::maxOutputLinesLimit() const
{
	bool      ok         = false;
	const int configured = m_worldAttributes.value(QStringLiteral("max_output_lines")).toInt(&ok);
	if (!ok || configured <= 0)
		return 0;
	return qBound(0, configured, 500000);
}

int WorldRuntime::enforceOutputLineLimit()
{
	if (m_sessionStateOutputBufferSealed)
		return 0;

	const int maxLines = maxOutputLinesLimit();
	if (maxLines <= 0 || m_lines.size() <= maxLines)
		return 0;

	const int removed = safeQSizeToInt(m_lines.size() - maxLines);
	m_lines.remove(0, removed);
	invalidateLuaCallbackLineBufferSnapshot();

	if (m_lastGoTo > m_lines.size())
		m_lastGoTo = qMax(1, safeQSizeToInt(m_lines.size()));

	if (!m_luaContextLineActive)
		return removed;

	if (!m_luaContextLineBuffered)
	{
		m_luaContextLineBufferIndex = safeQSizeToInt(m_lines.size() + 1);
		return removed;
	}

	if (m_luaContextLineBufferIndex > removed)
	{
		m_luaContextLineBufferIndex -= removed;
		return removed;
	}

	if (m_lines.isEmpty())
	{
		m_luaContextLineBuffered    = false;
		m_luaContextLineBufferIndex = 0;
		return removed;
	}

	m_luaContextLineBufferIndex = 1;
	m_luaContextLineEntry       = m_lines.first();
	return removed;
}

const IndexedRingBuffer<WorldRuntime::LineEntry> &WorldRuntime::lines() const
{
	return m_lines;
}

void WorldRuntime::setSessionStateOutputBufferSealed(const bool sealed)
{
	if (QThread::currentThread() != thread())
	{
		qmudInvokeMethodChecked(this, [this, sealed] { setSessionStateOutputBufferSealed(sealed); });
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::setSessionStateOutputBufferSealed");
	m_sessionStateOutputBufferSealed = sealed;
}

bool WorldRuntime::isSessionStateOutputBufferSealed() const
{
	return m_sessionStateOutputBufferSealed;
}

void WorldRuntime::replaceOutputLines(const IndexedRingBuffer<LineEntry> &lines)
{
	if (m_sessionStateOutputBufferSealed)
		return;

	m_lines = lines;
	invalidateLuaCallbackLineBufferSnapshot();

	qint64 maxLineNumber = 0;
	for (const LineEntry &line : m_lines)
		maxLineNumber = qMax(maxLineNumber, line.lineNumber);
	m_nextLineNumber = qMax<qint64>(1, maxLineNumber + 1);

	static_cast<void>(enforceOutputLineLimit());
	invalidateLuaCallbackLineBufferSnapshot();
	if (m_view)
		m_view->restoreOutputFromPersistedLines(m_lines);
}

void WorldRuntime::replaceOutputLines(const QVector<LineEntry> &lines)
{
	replaceOutputLines(IndexedRingBuffer<LineEntry>(lines));
}

void WorldRuntime::finalizePendingInputLineHardReturn()
{
	if (m_sessionStateOutputBufferSealed)
		return;

	if (m_lines.isEmpty())
		return;

	LineEntry &last = m_lines.last();
	if (last.hardReturn)
		return;
	if ((last.flags & LineInput) == 0)
		return;

	last.hardReturn = true;
	invalidateLuaCallbackLineBufferSnapshot();
}

void WorldRuntime::clearLastLineHardReturn()
{
	if (m_sessionStateOutputBufferSealed)
		return;

	if (m_lines.isEmpty())
		return;

	LineEntry &last = m_lines.last();
	if (!last.hardReturn)
		return;

	last.hardReturn = false;
	invalidateLuaCallbackLineBufferSnapshot();
}

void WorldRuntime::beginIncomingLineLuaContext(const QString &text, int flags,
                                               const QVector<StyleSpan> &spans, bool hardReturn)
{
	m_luaContextLineEntry.text       = text;
	m_luaContextLineEntry.flags      = flags;
	m_luaContextLineEntry.hardReturn = hardReturn;
	m_luaContextLineEntry.spans      = spans;
	m_luaContextLineEntry.time       = QDateTime::currentDateTime();
	m_luaContextLineEntry.lineNumber = m_nextLineNumber;
	if (m_lineTimer.isValid())
	{
		m_luaContextLineEntry.ticks   = static_cast<double>(m_lineTimer.nsecsElapsed()) / 1000000000.0;
		m_luaContextLineEntry.elapsed = m_luaContextLineEntry.ticks;
	}
	else
	{
		const double fallback =
		    (m_worldStartTime.isValid() && m_luaContextLineEntry.time.isValid())
		        ? static_cast<double>(m_worldStartTime.msecsTo(m_luaContextLineEntry.time)) / 1000.0
		        : 0.0;
		m_luaContextLineEntry.ticks   = fallback;
		m_luaContextLineEntry.elapsed = fallback;
	}
	m_luaContextLineBuffered    = false;
	m_luaContextLineCommitted   = false;
	m_luaContextLineBufferIndex = safeQSizeToInt(m_lines.size() + 1);
	m_luaContextLineActive      = true;
	invalidateLuaCallbackLineBufferSnapshot();
}

bool WorldRuntime::reserveIncomingLineLuaContextInBuffer()
{
	if (m_sessionStateOutputBufferSealed)
		return false;

	if (!m_luaContextLineActive)
		return false;
	if (m_luaContextLineBuffered)
		return true;

	LineEntry entry = m_luaContextLineEntry;
	if (entry.lineNumber <= 0)
		entry.lineNumber = m_nextLineNumber;
	if (entry.lineNumber >= m_nextLineNumber)
		m_nextLineNumber = entry.lineNumber + 1;
	m_lines.push_back(entry);
	m_luaContextLineBuffered    = true;
	m_luaContextLineBufferIndex = safeQSizeToInt(m_lines.size());
	m_luaContextLineEntry       = entry;
	static_cast<void>(enforceOutputLineLimit());
	invalidateLuaCallbackLineBufferSnapshot();
	return true;
}

bool WorldRuntime::updateBufferedIncomingLineLuaContext(const QString &text, int flags,
                                                        const QVector<StyleSpan> &spans, bool hardReturn)
{
	if (m_sessionStateOutputBufferSealed)
		return false;

	if (!m_luaContextLineActive || !m_luaContextLineBuffered)
		return false;
	if (m_luaContextLineBufferIndex <= 0 || m_luaContextLineBufferIndex > m_lines.size())
		return false;

	LineEntry &entry      = m_lines[m_luaContextLineBufferIndex - 1];
	const int  entryIndex = m_luaContextLineBufferIndex - 1;
	entry.text            = text;
	entry.flags           = flags;
	entry.spans           = spans;
	entry.hardReturn      = hardReturn;
	m_luaContextLineEntry = entry;
	invalidateLuaCallbackLineBufferSnapshot();
	notifyOutputViewLineChanged(entryIndex);
	return true;
}

bool WorldRuntime::removeBufferedIncomingLineLuaContext()
{
	if (m_sessionStateOutputBufferSealed)
		return false;

	if (!m_luaContextLineActive || !m_luaContextLineBuffered)
		return false;
	if (m_luaContextLineBufferIndex <= 0 || m_luaContextLineBufferIndex > m_lines.size())
		return false;

	const int removedIndex = m_luaContextLineBufferIndex - 1;
	m_lines.removeAt(removedIndex);
	m_luaContextLineBuffered    = false;
	m_luaContextLineBufferIndex = safeQSizeToInt(m_lines.size() + 1);
	invalidateLuaCallbackLineBufferSnapshot();
	QMudNativePluginRegistry::clearMushReaderPartialLine(this);
	notifyOutputViewRangeChanged(qMax(0, qMin(removedIndex, safeQSizeToInt(m_lines.size()) - 1)));
	return true;
}

bool WorldRuntime::hideBufferedIncomingLineLuaContextForReplacement()
{
	if (m_sessionStateOutputBufferSealed)
		return false;

	if (!m_luaContextLineActive || !m_luaContextLineBuffered)
		return false;
	if (m_luaContextLineBufferIndex <= 0 || m_luaContextLineBufferIndex > m_lines.size())
		return false;

	LineEntry &entry = m_lines[m_luaContextLineBufferIndex - 1];
	entry.flags |= LineHidden;
	m_luaContextLineEntry = entry;
	invalidateLuaCallbackLineBufferSnapshot();
	QMudNativePluginRegistry::clearMushReaderPartialLine(this);
	notifyOutputViewRangeChanged(m_luaContextLineBufferIndex - 1);
	return true;
}

qint64 WorldRuntime::incomingLineLuaContextAbsoluteNumber() const
{
	if (!m_luaContextLineActive)
		return 0;
	if (m_luaContextLineEntry.lineNumber > 0)
		return m_luaContextLineEntry.lineNumber;
	return m_nextLineNumber;
}

bool WorldRuntime::removeHiddenLuaContextLineByAbsoluteNumber(const qint64 absoluteLineNumber)
{
	if (m_sessionStateOutputBufferSealed)
		return false;

	if (absoluteLineNumber <= 0)
		return false;
	for (int i = 0; i < m_lines.size(); ++i)
	{
		if (m_lines.at(i).lineNumber != absoluteLineNumber)
			continue;
		if ((m_lines.at(i).flags & LineHidden) == 0)
			return false;
		m_lines.removeAt(i);
		if (m_luaContextLineBufferIndex > i + 1)
			--m_luaContextLineBufferIndex;
		else if (m_luaContextLineBufferIndex == i + 1)
			m_luaContextLineBuffered = false;
		invalidateLuaCallbackLineBufferSnapshot();
		notifyOutputViewRangeChanged(qMax(0, qMin(i, safeQSizeToInt(m_lines.size()) - 1)));
		return true;
	}
	return false;
}

void WorldRuntime::notifyOutputViewLineChanged(const int runtimeLineIndex)
{
	if (!m_view)
		return;
	if (runtimeLineIndex >= 0)
	{
		if (m_outputViewMutationBatchDepth > 0)
		{
			if (m_outputViewRangeChangedPending)
			{
				if (m_outputViewFirstChangedIndex < 0)
					m_outputViewFirstChangedIndex = runtimeLineIndex;
				else
					m_outputViewFirstChangedIndex = qMin(m_outputViewFirstChangedIndex, runtimeLineIndex);
				return;
			}
			if (m_outputViewLineChangedPending && m_outputViewLineChangedIndex >= 0 &&
			    m_outputViewLineChangedIndex != runtimeLineIndex)
			{
				m_outputViewRangeChangedPending = true;
				m_outputViewFirstChangedIndex   = qMin(m_outputViewLineChangedIndex, runtimeLineIndex);
				m_outputViewLineChangedIndex    = -1;
				return;
			}
			m_outputViewLineChangedPending = true;
			m_outputViewLineChangedIndex   = runtimeLineIndex;
			return;
		}
		m_view->notifyRuntimeOutputLineChanged(runtimeLineIndex);
		return;
	}
	if (m_outputViewMutationBatchDepth > 0)
	{
		m_outputViewLineChangedPending = true;
		m_outputViewLineChangedIndex   = -1;
		return;
	}
	m_view->notifyRuntimeOutputLineChanged();
}

void WorldRuntime::notifyOutputViewRangeChanged(const int runtimeLineIndex)
{
	if (!m_view)
		return;
	if (m_outputViewMutationBatchDepth > 0)
	{
		m_outputViewRangeChangedPending = true;
		int firstChangedIndex           = runtimeLineIndex;
		if (m_outputViewLineChangedPending && m_outputViewLineChangedIndex >= 0)
			firstChangedIndex = qMin(firstChangedIndex, m_outputViewLineChangedIndex);
		if (m_outputViewFirstChangedIndex < 0)
			m_outputViewFirstChangedIndex = firstChangedIndex;
		else
			m_outputViewFirstChangedIndex = qMin(m_outputViewFirstChangedIndex, firstChangedIndex);
		m_outputViewLineChangedIndex = -1;
		return;
	}
	m_view->notifyRuntimeOutputRangeChanged(runtimeLineIndex);
}

void WorldRuntime::flushOutputViewMutationBatch()
{
	if (!m_view)
	{
		m_outputViewLineChangedPending  = false;
		m_outputViewLineChangedIndex    = -1;
		m_outputViewRangeChangedPending = false;
		m_outputViewFirstChangedIndex   = -1;
		return;
	}
	const bool rangeChanged         = m_outputViewRangeChangedPending;
	const int  firstChangedIndex    = m_outputViewFirstChangedIndex;
	const bool changed              = m_outputViewLineChangedPending;
	const int  changedIndex         = m_outputViewLineChangedIndex;
	m_outputViewLineChangedPending  = false;
	m_outputViewLineChangedIndex    = -1;
	m_outputViewRangeChangedPending = false;
	m_outputViewFirstChangedIndex   = -1;
	if (rangeChanged && firstChangedIndex >= 0)
	{
		m_view->notifyRuntimeOutputRangeChanged(firstChangedIndex);
		return;
	}
	if (changed)
	{
		if (changedIndex >= 0)
			m_view->notifyRuntimeOutputLineChanged(changedIndex);
		else
			m_view->notifyRuntimeOutputLineChanged();
	}
}

void WorldRuntime::publishIncomingStyledLinePartial(const QString &line, const QVector<StyleSpan> &spans)
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::publishIncomingStyledLinePartial");
	if (m_outputViewLineChangedPending || m_outputViewRangeChangedPending)
		flushOutputViewMutationBatch();
	emit incomingStyledLinePartialReceived(line, spans);
}

void WorldRuntime::beginOutputViewMutationBatch()
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::beginOutputViewMutationBatch");
	if (m_outputViewMutationBatchDepth == 0)
	{
		m_outputViewMutationBatchView = m_view;
		if (m_outputViewMutationBatchView)
			m_outputViewMutationBatchView->beginRuntimeOutputMutationBatch();
	}
	++m_outputViewMutationBatchDepth;
}

void WorldRuntime::endOutputViewMutationBatch()
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::endOutputViewMutationBatch");
	if (m_outputViewMutationBatchDepth <= 0)
		return;
	--m_outputViewMutationBatchDepth;
	if (m_outputViewMutationBatchDepth > 0)
		return;
	const QPointer<WorldView> batchView = m_outputViewMutationBatchView;
	m_outputViewMutationBatchView.clear();
	flushOutputViewMutationBatch();
	if (batchView)
		batchView->endRuntimeOutputMutationBatch();
}

bool WorldRuntime::writeLuaCallbackOutputAtLineAnchor(const qint64 anchorLineNumber,
                                                      const int    anchorRelativeOffset,
                                                      const bool replaceAnchor, const QString &text,
                                                      int flags, const QVector<StyleSpan> &spans,
                                                      const bool hardReturn)
{
	if (m_sessionStateOutputBufferSealed)
		return false;

	if (text.isEmpty() && spans.isEmpty() && !hardReturn)
		return false;

	if (anchorLineNumber <= 0 || anchorRelativeOffset < 0)
		return false;

	QString                     displayText          = text;
	QVector<StyleSpan>          displaySpans         = spans;
	const bool                  note                 = (flags & LineNote) != 0;
	const bool                  serverSideWrapActive = isConnected() && m_telnet.isNawsNegotiated();
	const FixedColumnWrapConfig wrapConfig =
	    (!note && serverSideWrapActive)
	        ? FixedColumnWrapConfig{}
	        : localOutputWrapConfig(m_worldAttributes, serverSideWrapActive, m_telnet.windowColumns());
	if (wrapConfig.enabled && !displayText.isEmpty())
	{
		if (displaySpans.isEmpty())
			wrapPlainLineForColumn(displayText, wrapConfig.wrapColumn, wrapConfig.indentParas);
		else
			wrapStyledLineForColumn(displayText, displaySpans, wrapConfig.wrapColumn, wrapConfig.indentParas);
	}
	const QVector<OutputLineSegment> segments =
	    splitOutputTextAtLineBreaks(displayText, displaySpans, hardReturn);
	if (segments.isEmpty())
		return false;
	const int segmentCount = safeQSizeToInt(segments.size());

	int       anchorIndex = -1;
	for (int index = safeQSizeToInt(m_lines.size()) - 1; index >= 0; --index)
	{
		if (m_lines.at(index).lineNumber == anchorLineNumber)
		{
			anchorIndex = index;
			break;
		}
	}
	if (anchorIndex < 0)
	{
		m_luaCallbackAfterAnchorInsertionOffsets.remove(anchorLineNumber);
		return false;
	}

	if (replaceAnchor && (m_lines.at(anchorIndex).flags & LineHidden) == 0)
		return false;

	const int type = (flags & LineNote) != 0 ? 1 : 0;
	const int log  = (flags & LineLog) != 0 ? 1 : 0;
	firePluginScreendraw(type, log, text);

	auto makeInsertedEntry = [&](const OutputLineSegment &segment)
	{
		LineEntry entry;
		entry.text       = segment.text;
		entry.flags      = flags & ~LineHidden;
		entry.spans      = segment.spans;
		entry.hardReturn = segment.hardReturn;
		entry.time       = QDateTime::currentDateTime();
		entry.lineNumber = m_nextLineNumber++;
		if (m_lineTimer.isValid())
		{
			entry.ticks   = static_cast<double>(m_lineTimer.nsecsElapsed()) / 1000000000.0;
			entry.elapsed = entry.ticks;
		}
		else
		{
			const double fallback = (m_worldStartTime.isValid() && entry.time.isValid())
			                            ? static_cast<double>(m_worldStartTime.msecsTo(entry.time)) / 1000.0
			                            : 0.0;
			entry.ticks           = fallback;
			entry.elapsed         = fallback;
		}
		return entry;
	};

	if (replaceAnchor)
	{
		const OutputLineSegment &firstSegment = segments.first();
		LineEntry               &entry        = m_lines[anchorIndex];
		entry.text                            = firstSegment.text;
		entry.flags                           = flags & ~LineHidden;
		entry.spans                           = firstSegment.spans;
		entry.hardReturn                      = firstSegment.hardReturn;
		for (int index = 1; index < segmentCount; ++index)
			m_lines.insert(m_lines.begin() + anchorIndex + index, makeInsertedEntry(segments.at(index)));
		const int insertedCount = segmentCount - 1;
		if (m_luaContextLineActive && m_luaContextLineBuffered &&
		    m_luaContextLineBufferIndex > anchorIndex + 1)
			m_luaContextLineBufferIndex += insertedCount;
		const int removedHeadCount = enforceOutputLineLimit();
		invalidateLuaCallbackLineBufferSnapshot();
		if (!m_lines.isEmpty())
		{
			const int notifyIndex =
			    qBound(0, anchorIndex - removedHeadCount, safeQSizeToInt(m_lines.size()) - 1);
			notifyOutputViewRangeChanged(notifyIndex);
		}
		return true;
	}

	int effectiveRelativeOffset = anchorRelativeOffset;
	if (anchorRelativeOffset > 0)
	{
		const int nextRelativeOffset =
		    m_luaCallbackAfterAnchorInsertionOffsets.value(anchorLineNumber, anchorRelativeOffset);
		effectiveRelativeOffset = qMax(anchorRelativeOffset, nextRelativeOffset);
		m_luaCallbackAfterAnchorInsertionOffsets.insert(anchorLineNumber,
		                                                effectiveRelativeOffset + segmentCount);
		if (m_luaCallbackAfterAnchorInsertionOffsets.size() > 4096)
			m_luaCallbackAfterAnchorInsertionOffsets.clear();
	}
	const int insertIndex = qBound(0, anchorIndex + effectiveRelativeOffset, safeQSizeToInt(m_lines.size()));
	for (int index = 0; index < segmentCount; ++index)
		m_lines.insert(m_lines.begin() + insertIndex + index, makeInsertedEntry(segments.at(index)));
	if (m_luaContextLineActive && m_luaContextLineBuffered && m_luaContextLineBufferIndex > insertIndex)
		m_luaContextLineBufferIndex += segmentCount;
	const int removedHeadCount = enforceOutputLineLimit();
	invalidateLuaCallbackLineBufferSnapshot();
	if (!m_lines.isEmpty())
	{
		const int notifyIndex = qBound(0, insertIndex - removedHeadCount, safeQSizeToInt(m_lines.size()) - 1);
		notifyOutputViewRangeChanged(notifyIndex);
	}
	return true;
}

void WorldRuntime::endIncomingLineLuaContext()
{
	m_luaContextLineBuffered    = false;
	m_luaContextLineCommitted   = false;
	m_luaContextLineBufferIndex = 0;
	m_luaContextLineActive      = false;
	invalidateLuaCallbackLineBufferSnapshot();
}

void WorldRuntime::markIncomingLineLuaContextBuffered()
{
	if (!m_luaContextLineActive)
		return;
	if (m_luaContextLineBuffered)
	{
		if (m_luaContextLineBufferIndex > 0 && m_luaContextLineBufferIndex <= m_lines.size())
			m_luaContextLineEntry = m_lines.at(m_luaContextLineBufferIndex - 1);
		invalidateLuaCallbackLineBufferSnapshot();
		return;
	}
	m_luaContextLineBuffered    = true;
	m_luaContextLineBufferIndex = safeQSizeToInt(m_lines.size());
	if (m_luaContextLineBufferIndex > 0 && m_luaContextLineBufferIndex <= m_lines.size())
		m_luaContextLineEntry = m_lines.at(m_luaContextLineBufferIndex - 1);
	invalidateLuaCallbackLineBufferSnapshot();
}

void WorldRuntime::markIncomingLineLuaContextCommitted()
{
	if (!m_luaContextLineActive)
		return;
	m_luaContextLineCommitted = true;
	invalidateLuaCallbackLineBufferSnapshot();
}

bool WorldRuntime::luaContextLinePresentInBuffer() const
{
	return m_luaContextLineActive && m_luaContextLineBuffered;
}

bool WorldRuntime::luaContextLineEntry(int lineNumber, LineEntry &entry) const
{
	if (lineNumber <= 0)
		return false;

	const bool pendingLineVisible  = m_luaContextLineActive && !m_luaContextLineCommitted;
	const bool pendingLineBuffered = pendingLineVisible && luaContextLinePresentInBuffer();
	if (pendingLineVisible && pendingLineBuffered && lineNumber == m_luaContextLineBufferIndex)
	{
		entry = m_luaContextLineEntry;
		return true;
	}

	if (pendingLineVisible && !pendingLineBuffered && lineNumber == m_lines.size() + 1)
	{
		entry            = m_luaContextLineEntry;
		entry.lineNumber = m_nextLineNumber;
		return true;
	}

	if (lineNumber <= m_lines.size())
	{
		entry = m_lines.at(lineNumber - 1);
		return true;
	}
	return false;
}

bool WorldRuntime::luaContextLineEntryByAbsoluteNumber(const qint64 absoluteLineNumber,
                                                       LineEntry   &entry) const
{
	if (absoluteLineNumber <= 0)
		return false;

	if (QThread::currentThread() != thread())
	{
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), false, [this, absoluteLineNumber, &entry]
		                          { return luaContextLineEntryByAbsoluteNumber(absoluteLineNumber, entry); });
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::luaContextLineEntryByAbsoluteNumber");
	if (const bool pendingLineVisible = m_luaContextLineActive && !m_luaContextLineCommitted;
	    pendingLineVisible)
	{
		qint64 pendingAbsolute = m_luaContextLineEntry.lineNumber;
		if (pendingAbsolute <= 0)
			pendingAbsolute = m_nextLineNumber;
		if (pendingAbsolute == absoluteLineNumber)
		{
			entry = m_luaContextLineEntry;
			if (entry.lineNumber <= 0)
				entry.lineNumber = pendingAbsolute;
			return true;
		}
	}

	for (const LineEntry &line : m_lines)
	{
		if (line.lineNumber != absoluteLineNumber)
			continue;
		entry = line;
		return true;
	}
	return false;
}

int WorldRuntime::luaContextLinesInBufferCount() const
{
	if (QThread::currentThread() != thread())
		return qmudInvokeMethodOr(const_cast<WorldRuntime *>(this), 0,
		                          [this] { return luaContextLinesInBufferCount(); });

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::luaContextLinesInBufferCount");
	const bool pendingLineVisible = m_luaContextLineActive && !m_luaContextLineCommitted;
	if (const bool pendingLineBuffered = pendingLineVisible && luaContextLinePresentInBuffer();
	    pendingLineVisible && pendingLineBuffered)
	{
		// During trigger/deferred Lua callbacks, keep GetLinesInBufferCount()
		// pinned to the currently-processed incoming line even if callback-side
		// output appends additional lines before scripts finish.
		return m_luaContextLineBufferIndex;
	}
	return safeQSizeToInt(m_lines.size()) + (pendingLineVisible ? 1 : 0);
}

int WorldRuntime::windowCreate(const QString &name, int left, int top, int width, int height, int position,
                               int flags, const QColor &background, const QString &pluginId)
{
	if (name.isEmpty())
		return eNoNameSpecified;
	if (width < 0 || height < 0)
		return eBadParameter;

	auto it = m_miniWindows.find(name);
	if (it == m_miniWindows.end())
	{
		MiniWindow window;
		window.name = name;
		it          = m_miniWindows.insert(name, window);
	}

	MiniWindow &window = it.value();
	MiniWindowUtils::create(window, name, left, top, width, height, position, flags, background, pluginId,
	                        miniWindowDevicePixelRatioForView(m_view));

	emitMiniWindowsChangedCoalesced();
	return eOK;
}

int WorldRuntime::windowShow(const QString &name, bool show)
{
	const auto it = m_miniWindows.find(name);
	if (it == m_miniWindows.end())
		return eNoSuchWindow;
	it.value().show = show;
	emitMiniWindowsChangedCoalesced();
	return eOK;
}

int WorldRuntime::windowDelete(const QString &name)
{
	const auto it = m_miniWindows.find(name);
	if (it == m_miniWindows.end())
		return eNoSuchWindow;
	if (it.value().executingScript)
		return eItemInUse;
	m_miniWindows.erase(it);
	emitMiniWindowsChangedCoalesced();
	return eOK;
}

void WorldRuntime::beginMiniWindowMutationBatch()
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::beginMiniWindowMutationBatch");
	++m_miniWindowMutationBatchDepth;
}

void WorldRuntime::endMiniWindowMutationBatch()
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::endMiniWindowMutationBatch");
	if (m_miniWindowMutationBatchDepth <= 0)
	{
		m_miniWindowMutationBatchDepth = 0;
		m_miniWindowsChangedPending    = false;
		return;
	}
	--m_miniWindowMutationBatchDepth;
	if (m_miniWindowMutationBatchDepth == 0 && m_miniWindowsChangedPending)
	{
		m_miniWindowsChangedPending = false;
		emit miniWindowsChanged();
	}
}

void WorldRuntime::emitMiniWindowsChangedCoalesced()
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::emitMiniWindowsChangedCoalesced");
	invalidateLuaCallbackDispatchSnapshot();
	if (m_suppressMiniWindowsChangedSignal)
		return;
	if (m_miniWindowMutationBatchDepth > 0)
	{
		m_miniWindowsChangedPending = true;
		return;
	}
	emit miniWindowsChanged();
}

QStringList WorldRuntime::windowList() const
{
	return m_miniWindows.keys();
}

QVariant WorldRuntime::windowInfo(const QString &name, int infoType) const
{
	const MiniWindow *window = miniWindow(name);
	if (!window)
		return {};

	switch (infoType)
	{
	case 1:
		return window->location.x();
	case 2:
		return window->location.y();
	case 3:
		return window->width;
	case 4:
		return window->height;
	case 5:
		return window->show;
	case 6:
		return window->temporarilyHide;
	case 7:
		return window->position;
	case 8:
		return window->flags;
	case 9:
		if (window->background.isValid())
			return QVariant::fromValue<qlonglong>(colorToRef(window->background));
		return 0;
	case 10:
		return window->apiRect().left();
	case 11:
		return window->apiRect().top();
	case 12:
		return window->apiRect().right();
	case 13:
		return window->apiRect().bottom();
	case 14:
		return window->lastMousePosition.x();
	case 15:
		return window->lastMousePosition.y();
	case 16:
		return window->lastMouseUpdate;
	case 17:
		return window->clientMousePosition.x();
	case 18:
		return window->clientMousePosition.y();
	case 19:
		return window->mouseOverHotspot;
	case 20:
		return window->mouseDownHotspot;
	case 21:
		return window->installedAt.isValid() ? QVariant(toLuaSystemTime(window->installedAt)) : QVariant();
	case 22:
		return window->zOrder;
	case 23:
		return window->creatingPlugin;
	default:
		return {};
	}
}

MiniWindow *WorldRuntime::miniWindow(const QString &name)
{
	const auto it = m_miniWindows.find(name);
	if (it == m_miniWindows.end())
		return nullptr;
	return &it.value();
}

const MiniWindow *WorldRuntime::miniWindow(const QString &name) const
{
	const auto it = m_miniWindows.find(name);
	if (it == m_miniWindows.end())
		return nullptr;
	return &it.value();
}

QVector<MiniWindow *> WorldRuntime::sortedMiniWindows()
{
	QVector<MiniWindow *> ordered;
	ordered.reserve(m_miniWindows.size());
	for (auto it = m_miniWindows.begin(); it != m_miniWindows.end(); ++it)
		ordered.push_back(&it.value());

	std::ranges::sort(ordered,
	                  [](const MiniWindow *a, const MiniWindow *b)
	                  {
		                  if (a->zOrder == b->zOrder)
			                  return a->name < b->name;
		                  return a->zOrder < b->zOrder;
	                  });

	return ordered;
}

void WorldRuntime::layoutMiniWindows(const QSize &clientSize, const QSize &ownerSize, bool underneath,
                                     const QVector<MiniWindow *> *orderedWindows)
{
	const int             clientWidth  = clientSize.width();
	const int             clientHeight = clientSize.height();
	const int             ownerWidth   = ownerSize.width();
	const int             ownerHeight  = ownerSize.height();
	QVector<MiniWindow *> fallbackWindows;
	if (!orderedWindows)
	{
		fallbackWindows = sortedMiniWindows();
		orderedWindows  = &fallbackWindows;
	}
	const QVector<MiniWindow *> &windows = *orderedWindows;

	int                          absoluteMaxRight  = 0;
	int                          absoluteMaxBottom = 0;
	bool                         sawAbsolute       = false;
	for (MiniWindow const *window : windows)
	{
		if (!window || !window->show)
			continue;
		const bool drawUnder = (window->flags & kMiniWindowDrawUnderneath) != 0;
		if (drawUnder != underneath)
			continue;
		if ((window->flags & kMiniWindowAbsoluteLocation) == 0)
			continue;
		sawAbsolute = true;

		absoluteMaxRight  = qMax(absoluteMaxRight, window->location.x() + qMax(0, window->width));
		absoluteMaxBottom = qMax(absoluteMaxBottom, window->location.y() + qMax(0, window->height));
	}

	int       &referenceRight  = underneath ? m_absoluteReferenceRightUnder : m_absoluteReferenceRightOver;
	int       &referenceBottom = underneath ? m_absoluteReferenceBottomUnder : m_absoluteReferenceBottomOver;
	const bool captureActive   = m_view && m_view->isMiniWindowCaptureActive();
	if (!sawAbsolute)
	{
		referenceRight  = 0;
		referenceBottom = 0;
	}
	else if (captureActive)
	{
		// Keep drag scaling stable while a miniwindow is being moved/resized.
		referenceRight  = qMax(referenceRight, absoluteMaxRight);
		referenceBottom = qMax(referenceBottom, absoluteMaxBottom);
	}
	else
	{
		// Outside drag, use current extents to avoid stale startup/session anchors.
		referenceRight  = absoluteMaxRight;
		referenceBottom = absoluteMaxBottom;
	}

	double absoluteScaleX        = 1.0;
	double absoluteScaleY        = 1.0;
	bool   forceUnscaledAbsolute = false;
	if (AppController const *app = AppController::instance())
	{
		if (app->getGlobalOption(QStringLiteral("DisableWindowScaler")).toInt() != 0)
			forceUnscaledAbsolute = true;
	}
	if (m_view)
	{
		if (QWidget const *rootWindow = m_view->window(); rootWindow)
			forceUnscaledAbsolute = forceUnscaledAbsolute || rootWindow->isMaximized();
	}
	if (!forceUnscaledAbsolute)
	{
		if (clientWidth > 0 && referenceRight > clientWidth)
			absoluteScaleX = static_cast<double>(clientWidth) / static_cast<double>(referenceRight);
		if (clientHeight > 0 && referenceBottom > clientHeight)
			absoluteScaleY = static_cast<double>(clientHeight) / static_cast<double>(referenceBottom);
	}

	auto scaledAbsoluteRect = [clientWidth, clientHeight, absoluteScaleX,
	                           absoluteScaleY](const MiniWindow *window) -> QRect
	{
		if (!window || clientWidth <= 0 || clientHeight <= 0)
			return {};

		const int canonicalWidth  = qMax(0, window->width);
		const int canonicalHeight = qMax(0, window->height);

		int       scaledWidth  = qRound(static_cast<double>(canonicalWidth) * absoluteScaleX);
		int       scaledHeight = qRound(static_cast<double>(canonicalHeight) * absoluteScaleY);
		if (canonicalWidth > 0 && scaledWidth <= 0)
			scaledWidth = 1;
		if (canonicalHeight > 0 && scaledHeight <= 0)
			scaledHeight = 1;
		if (scaledWidth > clientWidth)
			scaledWidth = clientWidth;
		if (scaledHeight > clientHeight)
			scaledHeight = clientHeight;

		int scaledLeft = qRound(static_cast<double>(window->location.x()) * absoluteScaleX);
		int scaledTop  = qRound(static_cast<double>(window->location.y()) * absoluteScaleY);

		if (scaledLeft < 0)
			scaledLeft = 0;
		if (scaledTop < 0)
			scaledTop = 0;

		if (scaledLeft + scaledWidth > clientWidth)
			scaledLeft = qMax(0, clientWidth - scaledWidth);
		if (scaledTop + scaledHeight > clientHeight)
			scaledTop = qMax(0, clientHeight - scaledHeight);

		return {scaledLeft, scaledTop, scaledWidth, scaledHeight};
	};

	QPoint                topLeft(0, 0);
	QPoint                topRight(clientWidth, 0);
	QPoint                bottomLeft(0, clientHeight);
	QPoint                bottomRight(clientWidth, clientHeight);

	int                   topWidths    = 0;
	int                   rightHeights = 0;
	int                   bottomWidths = 0;
	int                   leftHeights  = 0;

	QVector<MiniWindow *> topWindows;
	QVector<MiniWindow *> rightWindows;
	QVector<MiniWindow *> bottomWindows;
	QVector<MiniWindow *> leftWindows;

	for (MiniWindow *window : windows)
	{
		window->temporarilyHide = false;

		if (!window->show)
			continue;

		const bool drawUnder = (window->flags & kMiniWindowDrawUnderneath) != 0;
		if (drawUnder != underneath)
			continue;

		const int w = window->width;
		const int h = window->height;

		if (window->flags & kMiniWindowAbsoluteLocation)
		{
			// Keep canonical plugin coordinates in window->location, but fit absolute
			// windows proportionally into the current client area when downsized.
			window->rect = scaledAbsoluteRect(window);
			continue;
		}

		switch (window->position)
		{
		case 0:
			window->rect = QRect(0, 0, clientWidth, clientHeight);
			continue;
		case 1:
			if (h > 0)
			{
				const double ratio = static_cast<double>(w) / static_cast<double>(h);
				window->rect       = QRect(0, 0, static_cast<int>(clientHeight * ratio), clientHeight);
			}
			else
				window->rect = QRect(0, 0, clientWidth, clientHeight);
			continue;
		case 2:
			window->rect = QRect(0, 0, ownerWidth, ownerHeight);
			continue;
		case 3:
			if (h > 0)
			{
				const double ratio = static_cast<double>(w) / static_cast<double>(h);
				window->rect       = QRect(0, 0, static_cast<int>(ownerHeight * ratio), ownerHeight);
			}
			else
				window->rect = QRect(0, 0, ownerWidth, ownerHeight);
			continue;
		case 4:
			window->rect = QRect(0, 0, w, h);
			topLeft.setX(qMax(topLeft.x(), window->rect.x() + window->rect.width()));
			topLeft.setY(qMax(topLeft.y(), window->rect.y() + window->rect.height()));
			break;
		case 5:
			topWidths += w;
			topWindows.push_back(window);
			break;
		case 6:
			window->rect = QRect(clientWidth - w, 0, w, h);
			topRight.setX(qMin(topRight.x(), window->rect.x()));
			topRight.setY(qMax(topRight.y(), window->rect.y() + window->rect.height()));
			break;
		case 7:
			rightHeights += h;
			rightWindows.push_back(window);
			break;
		case 8:
			window->rect = QRect(clientWidth - w, clientHeight - h, w, h);
			bottomRight.setX(qMin(bottomRight.x(), window->rect.x()));
			bottomRight.setY(qMin(bottomRight.y(), window->rect.y()));
			break;
		case 9:
			bottomWidths += w;
			bottomWindows.push_back(window);
			break;
		case 10:
			window->rect = QRect(0, clientHeight - h, w, h);
			bottomLeft.setX(qMax(bottomLeft.x(), window->rect.x() + window->rect.width()));
			bottomLeft.setY(qMin(bottomLeft.y(), window->rect.y()));
			break;
		case 11:
			leftHeights += h;
			leftWindows.push_back(window);
			break;
		case 12:
			window->rect = QRect((clientWidth - w) / 2, (clientHeight - h) / 2, w, h);
			break;
		default:
			break;
		}
	}

	const int topRoom    = topRight.x() - topLeft.x();
	const int rightRoom  = bottomRight.y() - topRight.y();
	const int bottomRoom = bottomRight.x() - bottomLeft.x();
	const int leftRoom   = bottomLeft.y() - topLeft.y();

	auto discardOverflow = [](QVector<MiniWindow *> &dockedWindows, int &totalSize, int room, bool vertical)
	{
		while (!dockedWindows.isEmpty() && totalSize > room)
		{
			MiniWindow *window = dockedWindows.back();
			dockedWindows.removeLast();
			totalSize -= vertical ? window->height : window->width;
			window->temporarilyHide = true;
		}
	};

	discardOverflow(topWindows, topWidths, topRoom, false);
	discardOverflow(rightWindows, rightHeights, rightRoom, true);
	discardOverflow(bottomWindows, bottomWidths, bottomRoom, false);
	discardOverflow(leftWindows, leftHeights, leftRoom, true);

	auto distribute = [](QVector<MiniWindow *> &dockedWindows, int room, int startPos, bool vertical,
	                     int fixedX, int fixedY, bool alignRight, bool alignBottom)
	{
		if (dockedWindows.isEmpty())
			return;
		int totalSize = 0;
		for (const MiniWindow *window : dockedWindows)
			totalSize += vertical ? window->height : window->width;
		const int gap   = (room - totalSize) / (safeQSizeToInt(dockedWindows.size()) + 1);
		int       start = startPos + gap;
		for (MiniWindow *window : dockedWindows)
		{
			if (vertical)
			{
				const int x  = alignRight ? fixedX - window->width : fixedX;
				window->rect = QRect(x, start, window->width, window->height);
				start += window->height + gap;
			}
			else
			{
				const int y  = alignBottom ? fixedY - window->height : fixedY;
				window->rect = QRect(start, y, window->width, window->height);
				start += window->width + gap;
			}
		}
	};

	distribute(topWindows, topRoom, topLeft.x(), false, 0, 0, false, false);
	distribute(rightWindows, rightRoom, topRight.y(), true, clientWidth, 0, true, false);
	distribute(bottomWindows, bottomRoom, bottomLeft.x(), false, 0, clientHeight, false, true);
	distribute(leftWindows, leftRoom, topLeft.y(), true, 0, 0, false, false);
}

int WorldRuntime::windowRectOp(const QString &name, int action, int left, int top, int right, int bottom,
                               long colour1, long colour2)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const int result = MiniWindowUtils::rectOp(*window, action, left, top, right, bottom, colour1, colour2);
	if (result == eOK)
		emitMiniWindowsChangedCoalesced();
	return result;
}

int WorldRuntime::windowCircleOp(const QString &name, int action, int left, int top, int right, int bottom,
                                 long penColour, long penStyle, int penWidth, long brushColour,
                                 long brushStyle, int extra1, int extra2, int extra3, int extra4)
{
	if (name.isEmpty())
		return eNoNameSpecified;
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const int result =
	    MiniWindowUtils::circleOp(*window, action, left, top, right, bottom, penColour, penStyle, penWidth,
	                              brushColour, brushStyle, extra1, extra2, extra3, extra4);
	if (result == eOK)
		emitMiniWindowsChangedCoalesced();
	return result;
}

int WorldRuntime::windowLine(const QString &name, int x1, int y1, int x2, int y2, long penColour,
                             long penStyle, int penWidth)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const int result = MiniWindowUtils::line(*window, x1, y1, x2, y2, penColour, penStyle, penWidth);
	if (result == eOK)
		emitMiniWindowsChangedCoalesced();
	return result;
}

int WorldRuntime::windowArc(const QString &name, int left, int top, int right, int bottom, int x1, int y1,
                            int x2, int y2, long penColour, long penStyle, int penWidth)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const int result = MiniWindowUtils::arc(*window, left, top, right, bottom, x1, y1, x2, y2, penColour,
	                                        penStyle, penWidth);
	if (result == eOK)
		emitMiniWindowsChangedCoalesced();
	return result;
}

int WorldRuntime::windowBezier(const QString &name, const QString &points, long penColour, long penStyle,
                               int penWidth)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const int result = MiniWindowUtils::bezier(*window, points, penColour, penStyle, penWidth);
	if (result == eOK)
		emitMiniWindowsChangedCoalesced();
	return result;
}

int WorldRuntime::windowPolygon(const QString &name, const QString &points, long penColour, long penStyle,
                                int penWidth, long brushColour, long brushStyle, bool closePolygon,
                                bool winding)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const int result = MiniWindowUtils::polygon(*window, points, penColour, penStyle, penWidth, brushColour,
	                                            brushStyle, closePolygon, winding);
	if (result == eOK)
		emitMiniWindowsChangedCoalesced();
	return result;
}

int WorldRuntime::windowGradient(const QString &name, int left, int top, int right, int bottom,
                                 long startColour, long endColour, int mode)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const int result =
	    MiniWindowUtils::gradient(*window, left, top, right, bottom, startColour, endColour, mode);
	if (result == eOK)
		emitMiniWindowsChangedCoalesced();
	return result;
}

int WorldRuntime::windowFont(const QString &name, const QString &fontId, const QString &fontName, double size,
                             bool bold, bool italic, bool underline, bool strikeout, int charset,
                             int pitchAndFamily)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;

	const int result = MiniWindowUtils::font(*window, fontId, fontName, size, bold, italic, underline,
	                                         strikeout, charset, pitchAndFamily);
	if (result == eOK)
		invalidateLuaCallbackDispatchSnapshot();
	return result;
}

QVariant WorldRuntime::windowFontInfo(const QString &name, const QString &fontId, int infoType) const
{
	const MiniWindow *window = miniWindow(name);
	if (!window)
		return {};
	return MiniWindowUtils::fontInfo(*window, fontId, infoType);
}

QStringList WorldRuntime::windowFontList(const QString &name) const
{
	const MiniWindow *window = miniWindow(name);
	if (!window)
		return {};
	return window->fonts.keys();
}

int WorldRuntime::windowText(const QString &name, const QString &fontId, const QString &text, int left,
                             int top, int right, int bottom, long colour)
{
	if (name.isEmpty())
		return -1;
	MiniWindow *window = miniWindow(name);
	if (!window)
		return -1;
	const int result = MiniWindowUtils::text(*window, fontId, text, left, top, right, bottom, colour);
	if (result >= 0)
		emitMiniWindowsChangedCoalesced();
	return result;
}

int WorldRuntime::windowTextPreviewWidth(const QString &name, const QString &fontId, const QString &text,
                                         int left, int top, int right, int bottom) const
{
	if (name.isEmpty())
		return -1;
	const MiniWindow *window = miniWindow(name);
	if (!window)
		return -1;
	return MiniWindowUtils::textPreviewWidth(*window, fontId, text, left, top, right, bottom);
}

int WorldRuntime::renderWindowOutputText(MiniWindow &targetWindow, const QString &fontId, const QString &text,
                                         const int left, const int top, const int right, const int bottom,
                                         const long colour, const QString &mouseUp,
                                         const QString &hotspotPrefix, const QString &pluginId,
                                         WindowOutputTextRenderContext renderContext,
                                         WindowOutputMetrics          *metricsOut)
{
	if (metricsOut)
		*metricsOut = WindowOutputMetrics{};

	MiniWindow *window = &targetWindow;

	const auto  fontIt = window->fonts.find(fontId);
	if (fontIt == window->fonts.end())
		return -2;

	if (!mouseUp.isEmpty() && !isValidScriptLabel(mouseUp))
		return eInvalidObjectLabel;

	QImage &surface = window->mutableBackingSurface();
	if (surface.isNull())
		return eOK;

	const QRect rect = rectFromCoords(*window, left, top, right, bottom);
	if (rect.width() <= 0 || rect.height() <= 0)
		return eOK;

	const QString normalizedPrefix =
	    hotspotPrefix.trimmed().isEmpty() ? QStringLiteral("output_link") : hotspotPrefix.trimmed();
	MiniWindowUtils::clearGeneratedOutputHotspots(*window, normalizedPrefix);

	if (text.isEmpty())
		return 0;

	auto parseColorValue = [](const QString &value) -> QColor
	{
		if (value.isEmpty())
			return {};
		QColor color(value);
		if (color.isValid())
			return color;
		bool      ok      = false;
		const int numeric = value.toInt(&ok);
		if (!ok)
			return {};
		const int r = numeric & 0xFF;
		const int g = (numeric >> 8) & 0xFF;
		const int b = (numeric >> 16) & 0xFF;
		return {r, g, b};
	};

	QVector<QColor> normalAnsi = renderContext.normalAnsi;
	QVector<QColor> boldAnsi   = renderContext.boldAnsi;
	QVector<QColor> customText = renderContext.customText;
	QVector<QColor> customBack = renderContext.customBack;
	if (normalAnsi.size() < 8)
		normalAnsi = defaultAnsiColours(false);
	if (boldAnsi.size() < 8)
		boldAnsi = defaultAnsiColours(true);
	if (customText.size() < 16)
		customText.resize(16);
	if (customBack.size() < 16)
		customBack.resize(16);
	for (int i = 0; i < customText.size(); ++i)
	{
		if (!customText.at(i).isValid())
			customText[i] = QColor(255, 255, 255);
		if (!customBack.at(i).isValid())
			customBack[i] = QColor(0, 0, 0);
	}

	const bool custom16Default =
	    isEnabledFlag(renderContext.worldAttributes.value(QStringLiteral("custom_16_is_default_colour")));
	const bool ignoreMxpColourChanges =
	    isEnabledFlag(renderContext.worldAttributes.value(QStringLiteral("ignore_mxp_colour_changes")));
	QColor defaultForeColor =
	    parseColorValue(renderContext.worldAttributes.value(QStringLiteral("output_text_colour")));
	QColor defaultBackColor =
	    parseColorValue(renderContext.worldAttributes.value(QStringLiteral("output_background_colour")));
	if (!defaultForeColor.isValid())
		defaultForeColor = custom16Default ? customText.value(15) : normalAnsi.value(7);
	if (!defaultBackColor.isValid())
		defaultBackColor = custom16Default ? customBack.value(15) : normalAnsi.value(0);
	const QString defaultFore = defaultForeColor.name();
	const QString defaultBack = defaultBackColor.name();
	const bool    useCustomLinkColour =
	    isEnabledFlag(renderContext.worldAttributes.value(QStringLiteral("use_custom_link_colour")));
	const bool underlineHyperlinks =
	    isEnabledFlag(renderContext.worldAttributes.value(QStringLiteral("underline_hyperlinks")));
	const QColor configuredHyperlinkColour =
	    parseColorValue(renderContext.worldAttributes.value(QStringLiteral("hyperlink_colour")));

	MxpStyleState          current = renderContext.mxpStyle;
	QVector<MxpStyleFrame> mxpStyleStack(renderContext.mxpStyleStack);
	QVector<QByteArray>    mxpBlockStack(renderContext.mxpBlockStack);
	bool                   mxpLinkOpen = renderContext.mxpLinkOpen;
	int                    mxpPreDepth = renderContext.mxpPreDepth;
	const bool             hasPersistedMxpRenderContext =
	    !mxpStyleStack.isEmpty() || !mxpBlockStack.isEmpty() || mxpLinkOpen || mxpPreDepth > 0;
	if (!hasPersistedMxpRenderContext)
	{
		current.bold       = renderContext.ansiRenderState.bold;
		current.underline  = renderContext.ansiRenderState.underline;
		current.italic     = renderContext.ansiRenderState.italic;
		current.blink      = renderContext.ansiRenderState.blink;
		current.strike     = renderContext.ansiRenderState.strike;
		current.monospace  = renderContext.ansiRenderState.monospace;
		current.inverse    = renderContext.ansiRenderState.inverse;
		current.fore       = renderContext.ansiRenderState.fore;
		current.back       = renderContext.ansiRenderState.back;
		current.actionType = renderContext.ansiRenderState.actionType;
		current.action     = renderContext.ansiRenderState.action;
		current.hint       = renderContext.ansiRenderState.hint;
		current.variable   = renderContext.ansiRenderState.variable;
		current.startTag   = renderContext.ansiRenderState.startTag;
	}
	if (current.fore.isEmpty())
		current.fore = defaultFore;
	if (current.back.isEmpty())
		current.back = defaultBack;

	QMudAnsiStreamState ansiStreamState = renderContext.ansiStreamState;

	struct ParsedRun
	{
			QString                 text;
			WorldRuntime::StyleSpan style;
	};
	QVector<ParsedRun> parsedRuns;
	int                parsedTextLength = 0;
	auto               appendRun        = [&](const QString &segment, const QMudStyledTextState &state)
	{
		if (segment.isEmpty())
			return;
		WorldRuntime::StyleSpan span;
		span.length     = safeQSizeToInt(segment.size());
		span.fore       = state.fore.isEmpty() ? QColor() : QColor(state.fore);
		span.back       = state.back.isEmpty() ? QColor() : QColor(state.back);
		span.bold       = state.bold;
		span.underline  = state.underline;
		span.italic     = state.italic;
		span.blink      = state.blink;
		span.strike     = state.strike;
		span.inverse    = state.inverse;
		span.actionType = state.actionType;
		span.action     = state.action;
		span.hint       = state.hint;
		span.variable   = state.variable;
		if (!parsedRuns.isEmpty() && parsedRuns.last().style.hasSameStyleAttributes(span))
		{
			parsedRuns.last().text += segment;
			parsedRuns.last().style.length = safeQSizeToInt(parsedRuns.last().text.size());
			parsedTextLength += safeQSizeToInt(segment.size());
			return;
		}
		parsedRuns.push_back({segment, span});
		parsedTextLength += safeQSizeToInt(segment.size());
	};

	auto colorFromXtermIndex = [](const int index) -> QString
	{
		if (index < 0 || index >= 256)
			return {};
		const AppController *app = AppController::instance();
		const QMudColorRef   ref = app ? app->xtermColorAt(index) : qmudRgb(0, 0, 0);
		return QColor(qmudRed(ref), qmudGreen(ref), qmudBlue(ref)).name();
	};
	auto normalAnsiColor = [&](const int index) -> QString
	{
		if (index < 0 || index >= normalAnsi.size())
			return {};
		return normalAnsi.at(index).name();
	};
	auto boldAnsiColor = [&](const int index) -> QString
	{
		if (index < 0 || index >= boldAnsi.size())
			return {};
		return boldAnsi.at(index).name();
	};
	auto appendStyledChunk = [&](const QString &chunkText)
	{
		if (chunkText.isEmpty())
			return;

		QMudStyledTextState ansiState;
		ansiState.bold       = current.bold;
		ansiState.underline  = current.underline;
		ansiState.italic     = current.italic;
		ansiState.blink      = current.blink;
		ansiState.strike     = current.strike;
		ansiState.inverse    = current.inverse;
		ansiState.fore       = current.fore;
		ansiState.back       = current.back;
		ansiState.actionType = current.actionType;
		ansiState.action     = current.action;
		ansiState.hint       = current.hint;
		ansiState.variable   = current.variable;
		ansiState.startTag   = current.startTag;
		ansiState.monospace  = current.monospace;

		constexpr QMudOscActionIds     oscActionIds{ActionNone, ActionSend, ActionPrompt, ActionHyperlink};
		const QVector<QMudStyledChunk> chunks = qmudParseAnsiSgrChunks(
		    chunkText.toUtf8(), ansiStreamState, defaultFore, defaultBack, normalAnsiColor, boldAnsiColor,
		    colorFromXtermIndex, [](const QByteArrayView bytes)
		    { return QString::fromUtf8(bytes.data(), safeQSizeToInt(bytes.size())); }, ansiState,
		    oscActionIds);
		for (const QMudStyledChunk &chunk : chunks)
		{
			QString normalized = chunk.text;
			normalized.replace(QChar(0x0085), QLatin1Char(' '));
			normalized.replace(QChar(0x2028), QLatin1Char(' '));
			normalized.replace(QChar(0x2029), QLatin1Char(' '));
			appendRun(normalized, chunk.state);
		}

		current.bold       = ansiState.bold;
		current.underline  = ansiState.underline;
		current.italic     = ansiState.italic;
		current.blink      = ansiState.blink;
		current.strike     = ansiState.strike;
		current.inverse    = ansiState.inverse;
		current.fore       = ansiState.fore;
		current.back       = ansiState.back;
		current.actionType = ansiState.actionType;
		current.action     = ansiState.action;
		current.hint       = ansiState.hint;
		current.variable   = ansiState.variable;
		current.startTag   = ansiState.startTag;
		current.monospace  = ansiState.monospace;
	};

	static const QRegularExpression kHexColor6(QStringLiteral("^[0-9A-Fa-f]{6}$"));
	auto                            normalizeColorBytes = [](const QByteArray &value) -> QString
	{
		QString name = decodeMxpInternalArgumentBytes(value).trimmed();
		if (name.isEmpty())
			return {};
		if (kHexColor6.match(name).hasMatch())
			name.prepend(QLatin1Char('#'));
		QColor color(name);
		return color.isValid() ? color.name() : QString();
	};
	auto normalizeColorText = [](const QString &value) -> QString
	{
		QString name = value.trimmed();
		if (name.isEmpty())
			return {};
		if (kHexColor6.match(name).hasMatch())
			name.prepend(QLatin1Char('#'));
		QColor color(name);
		return color.isValid() ? color.name() : QString();
	};
	auto mxpTagsEquivalent = [](const QByteArray &lhs, const QByteArray &rhs)
	{
		if (lhs == rhs)
			return true;
		return (lhs == "send" && rhs == "a") || (lhs == "a" && rhs == "send");
	};
	auto updateLinkOpenFromStack = [&]
	{
		mxpLinkOpen =
		    std::ranges::any_of(mxpStyleStack, [&](const MxpStyleFrame &frame)
		                        { return mxpTagsEquivalent(frame.tag, QByteArrayLiteral("send")); });
	};
	auto textFromParsedRuns = [&](const int startColumn, const int endColumn)
	{
		QString extractedText;
		if (startColumn < 0 || endColumn <= startColumn)
			return extractedText;
		extractedText.reserve(endColumn - startColumn);

		int runStart = 0;
		for (const ParsedRun &run : std::as_const(parsedRuns))
		{
			const int runLength = safeQSizeToInt(run.text.size());
			const int runEnd    = runStart + runLength;
			if (runLength > 0 && runEnd > startColumn && runStart < endColumn)
			{
				const int localStart = qMax(0, startColumn - runStart);
				const int localEnd   = qMin(runLength, endColumn - runStart);
				extractedText += run.text.mid(localStart, localEnd - localStart);
			}
			runStart = runEnd;
		}
		return extractedText;
	};
	auto resolveParsedRunMxpAction = [&](const MxpStyleFrame &frame)
	{
		const int     startColumn = qBound(0, frame.actionTextStartColumn, parsedTextLength);
		const int     endColumn   = parsedTextLength;
		const QString replacement = textFromParsedRuns(startColumn, endColumn);
		if (replacement.isEmpty() && frame.actionState.action.isEmpty())
			return;
		const QString resolvedAction =
		    frame.actionState.action.isEmpty()
		        ? replacement
		        : replaceMxpTextEntityReferences(frame.actionState.action, replacement);
		const QString resolvedHint = replaceMxpTextEntityReferences(frame.actionState.hint, replacement);

		int           runStart = 0;
		for (ParsedRun &run : parsedRuns)
		{
			const int runLength = safeQSizeToInt(run.text.size());
			const int runEnd    = runStart + runLength;
			if (runLength > 0 && runEnd > startColumn && runStart < endColumn &&
			    run.style.actionType == frame.actionState.actionType &&
			    run.style.action == frame.actionState.action && run.style.hint == frame.actionState.hint)
			{
				run.style.action = resolvedAction;
				run.style.hint   = resolvedHint;
			}
			runStart = runEnd;
		}
	};
	auto resolveAttributeEntities = [&](QMap<QByteArray, QByteArray> attributes)
	{
		const QMap<QByteArray, QByteArray> noLocalValues;
		for (auto it = attributes.begin(); it != attributes.end(); ++it)
			it.value() = resolveDefinitionEntities(it.value(), noLocalValues, renderContext.entityResolver);
		return attributes;
	};
	auto applyStartTag =
	    [&](const QByteArray &activeTag, const QMap<QByteArray, QByteArray> &activeAttributes)
	{
		const QByteArray unnamedFore = activeAttributes.value("1");
		const QByteArray unnamedBack = activeAttributes.value("2");
		if (activeTag == "send" || activeTag == "a")
		{
			QString href = decodeMxpInternalArgumentBytes(activeAttributes.value("href"));
			if (href.isEmpty())
				href = decodeMxpInternalArgumentBytes(activeAttributes.value("xch_cmd"));
			QString xchPrompt = decodeMxpInternalArgumentBytes(activeAttributes.value("xch_prompt"));
			if (xchPrompt.isEmpty())
				xchPrompt = decodeMxpInternalArgumentBytes(activeAttributes.value("prompt"));
			QString hint = decodeMxpInternalArgumentBytes(activeAttributes.value("hint"));
			if (hint.isEmpty())
				hint = decodeMxpInternalArgumentBytes(activeAttributes.value("xch_hint"));
			if (mxpStyleStack.size() >= kMaxMxpStackDepth)
				return;
			MxpStyleState actionState = current;
			if (activeTag == "a")
				actionState.actionType = ActionHyperlink;
			else if (!xchPrompt.isEmpty())
				actionState.actionType = ActionPrompt;
			else
				actionState.actionType = ActionSend;
			actionState.action = href;
			actionState.hint   = hint.isEmpty() ? xchPrompt : hint;
			QString variable   = decodeMxpInternalArgumentBytes(activeAttributes.value("xch_set"));
			if (variable.isEmpty())
				variable = decodeMxpInternalArgumentBytes(activeAttributes.value("variable"));
			if (variable.isEmpty())
				variable = decodeMxpInternalArgumentBytes(activeAttributes.value("set"));
			actionState.variable = variable;
			actionState.startTag = true;
			MxpStyleFrame frame;
			frame.tag                   = activeTag;
			frame.state                 = current;
			frame.actionState           = actionState;
			frame.actionTextLineNumber  = 0;
			frame.actionTextStartColumn = parsedTextLength;
			mxpStyleStack.push_back(frame);
			mxpLinkOpen = true;
			current     = actionState;
			return;
		}
		if (activeTag == "bold" || activeTag == "b" || activeTag == "strong" || activeTag == "h1" ||
		    activeTag == "h2" || activeTag == "h3" || activeTag == "h4" || activeTag == "h5" ||
		    activeTag == "h6")
		{
			if (mxpStyleStack.size() >= kMaxMxpStackDepth)
				return;
			mxpStyleStack.push_back(makeMxpStyleFrame(activeTag, current));
			current.bold = true;
			return;
		}
		if (activeTag == "underline" || activeTag == "u")
		{
			if (mxpStyleStack.size() >= kMaxMxpStackDepth)
				return;
			mxpStyleStack.push_back(makeMxpStyleFrame(activeTag, current));
			current.underline = true;
			return;
		}
		if (activeTag == "italic" || activeTag == "i" || activeTag == "em")
		{
			if (mxpStyleStack.size() >= kMaxMxpStackDepth)
				return;
			mxpStyleStack.push_back(makeMxpStyleFrame(activeTag, current));
			current.italic = true;
			return;
		}
		if (activeTag == "strike" || activeTag == "s")
		{
			if (mxpStyleStack.size() >= kMaxMxpStackDepth)
				return;
			mxpStyleStack.push_back(makeMxpStyleFrame(activeTag, current));
			current.strike = true;
			return;
		}
		if (activeTag == "color")
		{
			if (mxpStyleStack.size() >= kMaxMxpStackDepth)
				return;
			mxpStyleStack.push_back(makeMxpStyleFrame(activeTag, current));
			if (!ignoreMxpColourChanges)
			{
				QString fore = normalizeColorBytes(activeAttributes.value("fore"));
				if (fore.isEmpty())
					fore = normalizeColorBytes(unnamedFore);
				QString back = normalizeColorBytes(activeAttributes.value("back"));
				if (back.isEmpty())
					back = normalizeColorBytes(unnamedBack);
				if (!fore.isEmpty())
					current.fore = fore;
				if (!back.isEmpty())
					current.back = back;
			}
			return;
		}
		if (activeTag == "c")
		{
			if (mxpStyleStack.size() >= kMaxMxpStackDepth)
				return;
			mxpStyleStack.push_back(makeMxpStyleFrame(activeTag, current));
			if (!ignoreMxpColourChanges)
			{
				const QString fore = normalizeColorBytes(unnamedFore);
				const QString back = normalizeColorBytes(unnamedBack);
				if (!fore.isEmpty())
					current.fore = fore;
				if (!back.isEmpty())
					current.back = back;
			}
			return;
		}
		if (activeTag == "font")
		{
			if (mxpStyleStack.size() >= kMaxMxpStackDepth)
				return;
			mxpStyleStack.push_back(makeMxpStyleFrame(activeTag, current));
			QString colorSpec = decodeMxpInternalArgumentBytes(activeAttributes.value("color"));
			if (colorSpec.isEmpty())
				colorSpec = decodeMxpInternalArgumentBytes(activeAttributes.value("fgcolor"));
			const QStringList parts = colorSpec.split(',', Qt::SkipEmptyParts);
			for (QString part : parts)
			{
				part = part.trimmed();
				if (part.compare(QStringLiteral("bold"), Qt::CaseInsensitive) == 0)
					current.bold = true;
				else if (part.compare(QStringLiteral("italic"), Qt::CaseInsensitive) == 0)
					current.italic = true;
				else if (part.compare(QStringLiteral("underline"), Qt::CaseInsensitive) == 0)
					current.underline = true;
				else if (part.compare(QStringLiteral("blink"), Qt::CaseInsensitive) == 0)
					current.blink = true;
				else if (part.compare(QStringLiteral("inverse"), Qt::CaseInsensitive) == 0)
				{
					current.inverse = true;
					if (!current.fore.isEmpty() || !current.back.isEmpty())
						qSwap(current.fore, current.back);
				}
				else if (!ignoreMxpColourChanges)
				{
					const QString resolved = normalizeColorText(part);
					if (!resolved.isEmpty())
						current.fore = resolved;
				}
			}
			QString back = decodeMxpInternalArgumentBytes(activeAttributes.value("back"));
			if (back.isEmpty())
				back = decodeMxpInternalArgumentBytes(activeAttributes.value("bgcolor"));
			if (!ignoreMxpColourChanges)
			{
				const QString resolvedBack = normalizeColorText(back);
				if (!resolvedBack.isEmpty())
					current.back = resolvedBack;
			}
			return;
		}
		if (activeTag == "high" || activeTag == "h")
		{
			if (mxpStyleStack.size() >= kMaxMxpStackDepth)
				return;
			mxpStyleStack.push_back(makeMxpStyleFrame(activeTag, current));
			if (!current.fore.isEmpty())
			{
				const QColor color(current.fore);
				if (color.isValid())
					current.fore = color.lighter(115).name();
			}
			return;
		}
		if (activeTag == "tt" || activeTag == "samp")
		{
			if (mxpStyleStack.size() >= kMaxMxpStackDepth)
				return;
			mxpStyleStack.push_back(makeMxpStyleFrame(activeTag, current));
			current.monospace = true;
			return;
		}
		if (activeTag == "pre")
		{
			if (mxpBlockStack.size() >= kMaxMxpStackDepth)
				return;
			mxpBlockStack.push_back(activeTag);
			++mxpPreDepth;
			return;
		}
		if (activeTag == "center" || activeTag == "ul" || activeTag == "ol" || activeTag == "li")
		{
			if (mxpBlockStack.size() >= kMaxMxpStackDepth)
				return;
			mxpBlockStack.push_back(activeTag);
			return;
		}
		if (activeTag == "reset")
		{
			mxpStyleStack.clear();
			mxpBlockStack.clear();
			mxpLinkOpen  = false;
			mxpPreDepth  = 0;
			current      = MxpStyleState{};
			current.fore = defaultFore;
			current.back = defaultBack;
		}
	};
	auto applyEndTag = [&](const QByteArray &closeTag)
	{
		if (closeTag == "send" || closeTag == "a")
		{
			bool closedActionTag = false;
			for (int i = safeQSizeToInt(mxpStyleStack.size()) - 1; i >= 0; --i)
			{
				if (mxpTagsEquivalent(mxpStyleStack.at(i).tag, closeTag))
				{
					const MxpStyleFrame frame = mxpStyleStack.at(i);
					resolveParsedRunMxpAction(frame);
					current = frame.state;
					mxpStyleStack.removeAt(i);
					closedActionTag = true;
					break;
				}
			}
			if (closedActionTag || mxpLinkOpen)
				updateLinkOpenFromStack();
			return;
		}
		if (closeTag == "pre" || closeTag == "center" || closeTag == "ul" || closeTag == "ol" ||
		    closeTag == "li")
		{
			for (int i = safeQSizeToInt(mxpBlockStack.size()) - 1; i >= 0; --i)
			{
				if (mxpBlockStack.at(i) == closeTag)
				{
					for (int j = safeQSizeToInt(mxpBlockStack.size()) - 1; j >= i; --j)
					{
						if (mxpBlockStack.at(j) == "pre" && mxpPreDepth > 0)
							--mxpPreDepth;
					}
					mxpBlockStack.resize(i);
					break;
				}
			}
			return;
		}
		for (int i = safeQSizeToInt(mxpStyleStack.size()) - 1; i >= 0; --i)
		{
			if (mxpStyleStack.at(i).tag == closeTag)
			{
				current = mxpStyleStack.at(i).state;
				mxpStyleStack.removeAt(i);
				break;
			}
		}
	};

	struct LogicalCustomFrame
	{
			QByteArray          tag;
			QVector<QByteArray> closeTags;
	};
	QVector<LogicalCustomFrame> logicalFrames;
	auto                        applyMarkerTag = [&](const QString &tagContent, const bool closing)
	{
		const QByteArray rawTag = tagContent.trimmed().toUtf8();
		if (rawTag.isEmpty())
			return;

		if (closing)
		{
			const QByteArray closeTag = rawTag.toLower();
			for (int i = safeQSizeToInt(logicalFrames.size()) - 1; i >= 0; --i)
			{
				if (!mxpTagsEquivalent(logicalFrames.at(i).tag, closeTag))
					continue;
				const QVector<QByteArray> closeTags = logicalFrames.at(i).closeTags;
				logicalFrames.resize(i);
				if (!closeTags.isEmpty())
				{
					for (int j = safeQSizeToInt(closeTags.size()) - 1; j >= 0; --j)
						applyEndTag(closeTags.at(j));
				}
				else
					applyEndTag(closeTag);
				return;
			}
			applyEndTag(closeTag);
			return;
		}

		QByteArray tagName;
		QByteArray temp = rawTag;
		mxpGetWord(tagName, temp);
		tagName = tagName.toLower();
		if (tagName.isEmpty())
			return;

		ParsedMxpArguments                 parsed       = parseMxpArguments(rawTag);
		QMap<QByteArray, QByteArray>       attributes   = buildArgumentTableBytes(parsed.args);
		QByteArray                         effectiveTag = tagName;
		TelnetProcessor::CustomElementInfo customInfo;
		AtomicTagInfo                      atomicInfo;
		const bool                         hasCustomElement =
		    renderContext.customElementResolver && renderContext.customElementResolver(tagName, customInfo);
		const bool hasAtomicTag = lookupAtomicTagInfo(tagName, atomicInfo);
		if (!hasCustomElement && !hasAtomicTag)
			return;

		if (hasCustomElement)
		{
			const QMap<QByteArray, QByteArray> mergedDefaults =
			    mergeAttributeDefaultsBytes(customInfo.attributes, parsed.args);
			const QByteArray resolvedDefinition = resolveDefinitionEntities(
			    customInfo.definition, mergedDefaults, renderContext.entityResolver);
			QMap<QByteArray, QByteArray> aliasAttributes;
			QByteArray                   aliasTag;
			if (parseDefinitionAlias(resolvedDefinition, aliasTag, aliasAttributes))
			{
				effectiveTag = aliasTag;
				if (!aliasAttributes.isEmpty())
					attributes = aliasAttributes;
			}
		}

		QVector<QByteArray> customTagSequence;
		if (hasCustomElement)
		{
			const QMap<QByteArray, QByteArray> mergedDefaults =
			    mergeAttributeDefaultsBytes(customInfo.attributes, parsed.args);
			const QVector<QByteArray> rawTags = extractDefinitionTags(customInfo.definition);
			for (const QByteArray &rawDefinitionTag : rawTags)
			{
				const QByteArray resolved =
				    resolveDefinitionEntities(rawDefinitionTag, mergedDefaults, renderContext.entityResolver);
				const ParsedMxpArguments defParsed = parseMxpArguments(resolved);
				QByteArray               definitionTagName;
				QByteArray               definitionTemp = resolved;
				mxpGetWord(definitionTagName, definitionTemp);
				definitionTagName = definitionTagName.toLower();
				if (definitionTagName.isEmpty())
					continue;
				if (!lookupAtomicTagInfo(definitionTagName, atomicInfo))
					continue;
				customTagSequence.push_back(definitionTagName);
				applyStartTag(definitionTagName, buildArgumentTableBytes(defParsed.args));
			}
		}

		const bool trackable = !(hasCustomElement ? customInfo.command : atomicInfo.command);
		if (customTagSequence.isEmpty())
			applyStartTag(effectiveTag, resolveAttributeEntities(attributes));

		if (!trackable)
			return;

		LogicalCustomFrame frame;
		frame.tag = tagName;
		if (!customTagSequence.isEmpty())
			frame.closeTags = customTagSequence;
		else
			frame.closeTags = {effectiveTag};
		logicalFrames.push_back(frame);
	};

	QString source = text;
	source.replace(QStringLiteral("\\3"), QString(QChar(3)));
	source.replace(QStringLiteral("\\4"), QString(QChar(4)));
	QString         plainBuffer;
	constexpr QChar mxpOpenMarker(3);
	constexpr QChar mxpCloseMarker(4);
	for (qsizetype i = 0; i < source.size(); ++i)
	{
		if (source.at(i) != mxpOpenMarker)
		{
			plainBuffer.append(source.at(i));
			continue;
		}

		const qsizetype markerEnd = source.indexOf(mxpCloseMarker, i + 1);
		if (markerEnd < 0)
		{
			plainBuffer.append(source.mid(i));
			break;
		}

		appendStyledChunk(plainBuffer);
		plainBuffer.clear();

		QString    markerText = source.mid(i + 1, markerEnd - i - 1).trimmed();
		const bool closing    = markerText.startsWith(QLatin1Char('/'));
		if (closing)
			markerText.remove(0, 1);
		applyMarkerTag(markerText, closing);
		i = markerEnd;
	}
	appendStyledChunk(plainBuffer);

	const QColor fallbackTextColor = colorFromRef(colour);

	QPainter     painter(&surface);
	painter.setClipRect(rect);
	int    x                    = rect.left();
	int    y                    = rect.top();
	int    maxDrawnWidth        = 0;
	int    hotspotStatus        = eOK;
	int    renderedLeft         = rect.right() + 1;
	int    renderedTop          = rect.bottom() + 1;
	int    renderedRight        = rect.left() - 1;
	int    renderedBottom       = rect.top() - 1;
	int    renderedLineCount    = 0;
	int    renderedHotspotCount = 0;
	bool   hasPreviousRunOnLine = false;
	bool   hasCountedLineY      = false;
	int    countedLineY         = 0;
	QColor previousRunBackColor;
	auto   newline = [&]
	{
		x = rect.left();
		y += fontIt.value().metrics.height();
		hasPreviousRunOnLine = false;
		previousRunBackColor = QColor();
	};

	bool abortedForHeight = false;
	for (const ParsedRun &run : std::as_const(parsedRuns))
	{
		if (run.text.isEmpty())
			continue;
		QColor foreColor = run.style.fore.isValid() ? run.style.fore : fallbackTextColor;
		QColor backColor = run.style.back;
		if (run.style.inverse)
			qSwap(foreColor, backColor);
		const bool hasLinkAction =
		    (run.style.actionType == ActionHyperlink || run.style.actionType == ActionSend ||
		     run.style.actionType == ActionPrompt) &&
		    !run.style.action.trimmed().isEmpty() && !hasUnresolvedMxpTextEntityReference(run.style.action);
		if (hasLinkAction && useCustomLinkColour && configuredHyperlinkColour.isValid())
			foreColor = configuredHyperlinkColour;

		QFont drawFont = fontIt.value().font;
		drawFont.setBold(run.style.bold);
		drawFont.setItalic(run.style.italic);
		drawFont.setUnderline(run.style.underline || (hasLinkAction && underlineHyperlinks));
		drawFont.setStrikeOut(run.style.strike);
		QFontMetrics metrics(drawFont);
		const int    lineHeight = metrics.height();
		const int    ascent     = metrics.ascent();
		const int    rightLimit = rect.right() + 1;

		QString      currentLineText;
		int          currentLineWidth = 0;
		auto         flushLineRun     = [&]
		{
			if (currentLineText.isEmpty())
				return;
			if (!MiniWindowUtils::lineFitsVertically(y, lineHeight, rect.bottom()))
			{
				currentLineText.clear();
				currentLineWidth = 0;
				abortedForHeight = true;
				return;
			}
			if (backColor.isValid())
			{
				int fillLeft  = x;
				int fillWidth = currentLineWidth;
				if (hasPreviousRunOnLine && previousRunBackColor.isValid() &&
				    previousRunBackColor == backColor && fillWidth > 1)
				{
					++fillLeft;
					--fillWidth;
				}
				if (fillWidth > 0)
					painter.fillRect(QRect(fillLeft, y, fillWidth, lineHeight), backColor);
			}
			painter.setFont(drawFont);
			painter.setPen(foreColor);
			painter.drawText(x, y + ascent, currentLineText);

			const int runLeft  = x;
			const int runRight = x + currentLineWidth;
			maxDrawnWidth      = std::max(maxDrawnWidth, runRight - rect.left());
			renderedLeft       = std::min(renderedLeft, runLeft);
			renderedTop        = std::min(renderedTop, y);
			renderedRight      = std::max(renderedRight, runRight - 1);
			renderedBottom     = std::max(renderedBottom, y + lineHeight - 1);
			if (!hasCountedLineY || countedLineY != y)
			{
				++renderedLineCount;
				countedLineY    = y;
				hasCountedLineY = true;
			}
			if (hasLinkAction)
			{
				const int addResult = MiniWindowUtils::addGeneratedOutputHotspot(
				    *window, normalizedPrefix, runLeft, y, runRight, y + lineHeight, mouseUp, pluginId,
				    run.style.actionType, run.style.action);
				if (addResult == eOK)
				{
					++renderedHotspotCount;
				}
				else if (hotspotStatus == eOK)
					hotspotStatus = addResult;
			}

			x += currentLineWidth;
			hasPreviousRunOnLine = true;
			previousRunBackColor = backColor;
			currentLineText.clear();
			currentLineWidth = 0;
		};

		for (const QChar ch : run.text)
		{
			if (abortedForHeight)
				break;
			if (ch == QLatin1Char('\r'))
				continue;
			if (ch == QLatin1Char('\b'))
			{
				if (!currentLineText.isEmpty())
				{
					currentLineText.chop(1);
					currentLineWidth = metrics.horizontalAdvance(currentLineText);
				}
				continue;
			}
			if (ch == QLatin1Char('\n'))
			{
				flushLineRun();
				if (abortedForHeight)
					break;
				newline();
				continue;
			}

			int candidateWidth = metrics.horizontalAdvance(currentLineText + ch);
			while (
			    MiniWindowUtils::runNeedsWrap(x, candidateWidth, currentLineWidth, rect.left(), rightLimit))
			{
				int splitIndex = -1;
				for (int i = safeQSizeToInt(currentLineText.size()) - 1; i >= 0; --i)
				{
					if (currentLineText.at(i).isSpace())
					{
						splitIndex = i;
						break;
					}
				}
				if (splitIndex > 0)
				{
					const QString carry = currentLineText.mid(splitIndex + 1);
					currentLineText.truncate(splitIndex);
					currentLineWidth = metrics.horizontalAdvance(currentLineText);
					flushLineRun();
					if (abortedForHeight)
						break;
					newline();
					currentLineText = carry;
					currentLineWidth =
					    currentLineText.isEmpty() ? 0 : metrics.horizontalAdvance(currentLineText);
					candidateWidth = metrics.horizontalAdvance(currentLineText + ch);
					continue;
				}

				flushLineRun();
				if (abortedForHeight)
					break;
				newline();
				candidateWidth = metrics.horizontalAdvance(currentLineText + ch);
			}
			if (abortedForHeight)
				break;
			if (currentLineText.isEmpty() && ch.isSpace())
				continue;
			currentLineText.append(ch);
			currentLineWidth = metrics.horizontalAdvance(currentLineText);
		}
		flushLineRun();
		if (abortedForHeight)
			break;
	}

	if (metricsOut && renderedBottom >= rect.top())
	{
		metricsOut->left         = renderedLeft;
		metricsOut->top          = renderedTop;
		metricsOut->right        = renderedRight;
		metricsOut->bottom       = renderedBottom;
		metricsOut->width        = renderedRight - renderedLeft + 1;
		metricsOut->height       = renderedBottom - renderedTop + 1;
		metricsOut->lineCount    = renderedLineCount;
		metricsOut->hotspotCount = renderedHotspotCount;
		metricsOut->hasOutput    = true;
	}
	if (hotspotStatus != eOK)
		return hotspotStatus;
	return qMin(maxDrawnWidth, rect.width());
}

int WorldRuntime::windowOutputText(const QString &name, const QString &fontId, const QString &text, int left,
                                   int top, int right, int bottom, long colour, const QString &mouseUp,
                                   const QString &hotspotPrefix, const QString &pluginId,
                                   WindowOutputMetrics *metricsOut)
{
	if (name.isEmpty())
	{
		if (metricsOut)
			*metricsOut = WindowOutputMetrics{};
		return -1;
	}

	MiniWindow *window = miniWindow(name);
	if (!window)
	{
		if (metricsOut)
			*metricsOut = WindowOutputMetrics{};
		return -1;
	}

	WindowOutputTextRenderContext context;
	context.ansiStreamState = m_ansiStreamState;
	context.ansiRenderState = m_ansiRenderState;
	context.mxpStyle        = m_mxpRenderStyle;
	context.mxpStyleStack   = m_mxpRenderStack;
	context.mxpBlockStack   = m_mxpRenderBlockStack;
	context.mxpLinkOpen     = m_mxpRenderLinkOpen;
	context.mxpPreDepth     = m_mxpRenderPreDepth;
	context.worldAttributes = m_worldAttributes;
	context.normalAnsi.resize(8);
	context.boldAnsi.resize(8);
	for (int index = 1; index <= 8; ++index)
	{
		context.normalAnsi[index - 1] = colorFromRef(normalColour(index));
		context.boldAnsi[index - 1]   = ansiColour(true, index);
	}
	context.customText.resize(MAX_CUSTOM);
	context.customBack.resize(MAX_CUSTOM);
	for (int index = 1; index <= MAX_CUSTOM; ++index)
	{
		context.customText[index - 1] = colorFromRef(customColourText(index));
		context.customBack[index - 1] = colorFromRef(customColourBackground(index));
	}
	context.customElementResolver = [this](const QByteArray                   &elementName,
	                                       TelnetProcessor::CustomElementInfo &info) -> bool
	{ return m_telnet.getCustomElementInfo(elementName, info); };
	context.entityResolver = [this](const QByteArray &entityName, QByteArray &value) -> bool
	{ return m_telnet.resolveEntityValue(entityName, value); };

	const int result =
	    renderWindowOutputText(*window, fontId, text, left, top, right, bottom, colour, mouseUp,
	                           hotspotPrefix, pluginId, std::move(context), metricsOut);
	if (result >= 0)
		emitMiniWindowsChangedCoalesced();
	return result;
}

int WorldRuntime::windowOutputTextPreview(const QString &name, const QString &fontId, const QString &text,
                                          const int left, const int top, const int right, const int bottom,
                                          const long colour, const QString &mouseUp,
                                          const QString &hotspotPrefix, const QString &pluginId,
                                          WindowOutputMetrics *metricsOut)
{
	if (metricsOut)
		*metricsOut = WindowOutputMetrics{};
	if (name.isEmpty())
		return -1;

	MiniWindow *window = miniWindow(name);
	if (!window)
		return -1;

	const MiniWindow             miniWindowSnapshot        = *window;
	const QMudAnsiStreamState    ansiStreamStateSnapshot   = m_ansiStreamState;
	const AnsiRenderState        ansiRenderStateSnapshot   = m_ansiRenderState;
	const MxpStyleState          mxpRenderStyleSnapshot    = m_mxpRenderStyle;
	const QVector<MxpStyleFrame> mxpRenderStackSnapshot    = m_mxpRenderStack;
	const QVector<QByteArray>    mxpRenderBlocksSnapshot   = m_mxpRenderBlockStack;
	const bool                   mxpRenderLinkSnapshot     = m_mxpRenderLinkOpen;
	const int                    mxpRenderPreDepthSnapshot = m_mxpRenderPreDepth;
	const bool                   suppressSignalsSnapshot   = m_suppressMiniWindowsChangedSignal;
	const auto                   restoreState              = qScopeGuard(
	    [&]()
	    {
		    *window                            = miniWindowSnapshot;
		    m_ansiStreamState                  = ansiStreamStateSnapshot;
		    m_ansiRenderState                  = ansiRenderStateSnapshot;
		    m_mxpRenderStyle                   = mxpRenderStyleSnapshot;
		    m_mxpRenderStack                   = mxpRenderStackSnapshot;
		    m_mxpRenderBlockStack              = mxpRenderBlocksSnapshot;
		    m_mxpRenderLinkOpen                = mxpRenderLinkSnapshot;
		    m_mxpRenderPreDepth                = mxpRenderPreDepthSnapshot;
		    m_suppressMiniWindowsChangedSignal = suppressSignalsSnapshot;
	    });

	m_suppressMiniWindowsChangedSignal = true;
	return windowOutputText(name, fontId, text, left, top, right, bottom, colour, mouseUp, hotspotPrefix,
	                        pluginId, metricsOut);
}

int WorldRuntime::windowTextWidth(const QString &name, const QString &fontId, const QString &text) const
{
	if (name.isEmpty())
		return -1;
	const MiniWindow *window = miniWindow(name);
	if (!window)
		return -1;
	const auto it = window->fonts.find(fontId);
	if (it == window->fonts.end())
		return -2;
	return MiniWindowUtils::textWidth(*window, fontId, text);
}

int WorldRuntime::windowSetPixel(const QString &name, int x, int y, long colour)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	MiniWindowUtils::setPixel(*window, x, y, colour);
	emitMiniWindowsChangedCoalesced();
	return eOK;
}

QVariant WorldRuntime::windowGetPixel(const QString &name, int x, int y) const
{
	if (name.isEmpty())
		return {static_cast<qlonglong>(-2)};
	const MiniWindow *window = miniWindow(name);
	if (!window)
		return {static_cast<qlonglong>(-2)};
	return {static_cast<qlonglong>(MiniWindowUtils::pixelValue(*window, x, y))};
}

int WorldRuntime::windowCreateImage(const QString &name, const QString &imageId, long row1, long row2,
                                    long row3, long row4, long row5, long row6, long row7, long row8)
{
	if (name.isEmpty())
		return eNoNameSpecified;
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	MiniWindowUtils::createImage(*window, imageId, row1, row2, row3, row4, row5, row6, row7, row8);
	return eOK;
}

int WorldRuntime::windowLoadImage(const QString &name, const QString &imageId, const QString &filename)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;

	window->images.remove(imageId);
	const QString trimmed = filename.trimmed();
	if (trimmed.isEmpty())
		return eOK;
	if (trimmed.size() < 5)
		return eBadParameter;

	const QString lower = trimmed.toLower();
	if (!lower.endsWith(QStringLiteral(".bmp")) && !lower.endsWith(QStringLiteral(".png")))
		return eBadParameter;

	QString resolved;
	QString error;
	if (!QMudPluginPathUtils::resolveInsideQmudHome(m_startupDirectory, trimmed, &resolved, &error))
		return eFileNotFound;

	if (!QFileInfo::exists(resolved))
		return eFileNotFound;

	if (!MiniWindowUtils::loadImage(*window, imageId, resolved))
		return eUnableToLoadImage;
	return eOK;
}

int WorldRuntime::windowLoadImageMemory(const QString &name, const QString &imageId, const QByteArray &data,
                                        bool swapAlpha)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;

	if (!MiniWindowUtils::loadImageMemory(*window, imageId, data, swapAlpha))
		return eUnableToLoadImage;
	return eOK;
}

QStringList WorldRuntime::windowImageList(const QString &name) const
{
	const MiniWindow *window = miniWindow(name);
	if (!window)
		return {};
	return window->images.keys();
}

QVariant WorldRuntime::windowImageInfo(const QString &name, const QString &imageId, int infoType) const
{
	const MiniWindow *window = miniWindow(name);
	if (!window)
		return {};
	return MiniWindowUtils::imageInfo(*window, imageId, infoType);
}

bool WorldRuntime::windowImageHasAlpha(const QString &name, const QString &imageId, bool &hasAlpha) const
{
	const MiniWindow *window = miniWindow(name);
	if (!window)
		return false;
	const auto it = window->images.find(imageId);
	if (it == window->images.end())
		return false;
	hasAlpha = it.value().hasAlpha;
	return true;
}

int WorldRuntime::windowImageFromWindow(const QString &name, const QString &imageId,
                                        const QString &sourceWindow)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const MiniWindow *src = miniWindow(sourceWindow);
	if (!src)
		return eNoSuchWindow;
	MiniWindowUtils::imageFromWindow(*window, imageId, *src);
	return eOK;
}

int WorldRuntime::windowDrawImage(const QString &name, const QString &imageId, int left, int top, int right,
                                  int bottom, int mode, int srcLeft, int srcTop, int srcRight, int srcBottom)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const int result = MiniWindowUtils::drawImage(*window, imageId, left, top, right, bottom, mode, srcLeft,
	                                              srcTop, srcRight, srcBottom);
	if (result == eOK)
		emitMiniWindowsChangedCoalesced();
	return result;
}

int WorldRuntime::windowDrawImageAlpha(const QString &name, const QString &imageId, int left, int top,
                                       int right, int bottom, double opacity, int srcLeft, int srcTop)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const int result =
	    MiniWindowUtils::drawImageAlpha(*window, imageId, left, top, right, bottom, opacity, srcLeft, srcTop);
	if (result == eOK)
		emitMiniWindowsChangedCoalesced();
	return result;
}

int WorldRuntime::windowImageOp(const QString &name, int action, int left, int top, int right, int bottom,
                                long penColour, long penStyle, int penWidth, long brushColour,
                                const QString &imageId, int ellipseWidth, int ellipseHeight)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const int result =
	    MiniWindowUtils::imageOp(*window, action, left, top, right, bottom, penColour, penStyle, penWidth,
	                             brushColour, imageId, ellipseWidth, ellipseHeight);
	if (result == eOK)
		emitMiniWindowsChangedCoalesced();
	return result;
}

int WorldRuntime::windowMergeImageAlpha(const QString &name, const QString &imageId, const QString &maskId,
                                        int left, int top, int right, int bottom, int mode, double opacity,
                                        int srcLeft, int srcTop, int srcRight, int srcBottom)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const int result = MiniWindowUtils::mergeImageAlpha(*window, imageId, maskId, left, top, right, bottom,
	                                                    mode, opacity, srcLeft, srcTop, srcRight, srcBottom);
	if (result == eOK)
		emitMiniWindowsChangedCoalesced();
	return result;
}

int WorldRuntime::windowGetImageAlpha(const QString &name, const QString &imageId, int left, int top,
                                      int right, int bottom, int srcLeft, int srcTop)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const int result =
	    MiniWindowUtils::getImageAlpha(*window, imageId, left, top, right, bottom, srcLeft, srcTop);
	if (result == eOK)
		emitMiniWindowsChangedCoalesced();
	return result;
}

int WorldRuntime::windowBlendImage(const QString &name, const QString &imageId, int left, int top, int right,
                                   int bottom, int mode, double opacity, int srcLeft, int srcTop,
                                   int srcRight, int srcBottom)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const int result = MiniWindowUtils::blendImage(*window, imageId, left, top, right, bottom, mode, opacity,
	                                               srcLeft, srcTop, srcRight, srcBottom, runtimeRandomUnit);
	if (result == eOK)
		emitMiniWindowsChangedCoalesced();
	return result;
}

int WorldRuntime::windowBlendImageWithRandomSeed(const QString &name, const QString &imageId, int left,
                                                 int top, int right, int bottom, int mode, double opacity,
                                                 int srcLeft, int srcTop, int srcRight, int srcBottom,
                                                 quint32 randomSeed)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const int result =
	    MiniWindowUtils::blendImage(*window, imageId, left, top, right, bottom, mode, opacity, srcLeft,
	                                srcTop, srcRight, srcBottom, seededMiniWindowRandomUnit(randomSeed));
	if (result == eOK)
		emitMiniWindowsChangedCoalesced();
	return result;
}

int WorldRuntime::windowFilter(const QString &name, int left, int top, int right, int bottom, int operation,
                               double options, int extra)
{
	Q_UNUSED(extra);
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const int result =
	    MiniWindowUtils::filter(*window, left, top, right, bottom, operation, options, runtimeRandomUnit);
	if (result == eOK)
		emitMiniWindowsChangedCoalesced();
	return result;
}

int WorldRuntime::windowFilterWithRandomSeed(const QString &name, int left, int top, int right, int bottom,
                                             int operation, double options, int extra, quint32 randomSeed)
{
	Q_UNUSED(extra);
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const int result = MiniWindowUtils::filter(*window, left, top, right, bottom, operation, options,
	                                           seededMiniWindowRandomUnit(randomSeed));
	if (result == eOK)
		emitMiniWindowsChangedCoalesced();
	return result;
}

int WorldRuntime::windowTransformImage(const QString &name, const QString &imageId, float left, float top,
                                       int mode, float mxx, float mxy, float myx, float myy)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const int result = MiniWindowUtils::transformImage(*window, imageId, left, top, mode, mxx, mxy, myx, myy);
	if (result == eOK)
		emitMiniWindowsChangedCoalesced();
	return result;
}

int WorldRuntime::windowWrite(const QString &name, const QString &filename)
{
	const MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const QString trimmedFilename = filename.trimmed();
	if (trimmedFilename.isEmpty())
		return eNoNameSpecified;
	if (trimmedFilename.size() < 5)
		return eBadParameter;

	const QString lower = trimmedFilename.toLower();
	if (!lower.endsWith(QStringLiteral(".bmp")) && !lower.endsWith(QStringLiteral(".png")))
		return eBadParameter;

	QString resolved;
	QString error;
	if (!QMudPluginPathUtils::resolveInsideQmudHome(m_startupDirectory, trimmedFilename, &resolved, &error))
		return eCouldNotOpenFile;

	if (!MiniWindowUtils::saveWindowImage24Bit(*window, resolved))
		return eCouldNotOpenFile;
	return eOK;
}

int WorldRuntime::windowSnapshotImage(const QString &name, QImage &image) const
{
	const MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;

	image = window->backingSurface().copy();
	return eOK;
}

int WorldRuntime::windowPosition(const QString &name, int left, int top, int position, int flags)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;

	if ((flags & kMiniWindowAbsoluteLocation) != 0 && m_view && m_view->isMiniWindowCaptureActive())
	{
		const int maxWidth  = m_view->outputClientWidth();
		const int maxHeight = m_view->outputClientHeight();

		if (maxWidth > 0)
		{
			if (window->width >= maxWidth)
				left = 0;
			else
				left = qBound(0, left, maxWidth - window->width);
		}
		if (maxHeight > 0)
		{
			if (window->height >= maxHeight)
				top = 0;
			else
				top = qBound(0, top, maxHeight - window->height);
		}
	}

	MiniWindowUtils::position(*window, left, top, position, flags);
	emitMiniWindowsChangedCoalesced();
	return eOK;
}

int WorldRuntime::windowResize(const QString &name, int width, int height, long colour)
{
	if (width < 0 || height < 0)
		return eBadParameter;
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	if (window->width == width && window->height == height)
		return eOK;
	MiniWindowUtils::resize(*window, width, height, colour);
	emitMiniWindowsChangedCoalesced();
	return eOK;
}

int WorldRuntime::windowSetZOrder(const QString &name, int zOrder)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	MiniWindowUtils::setZOrder(*window, zOrder);
	emitMiniWindowsChangedCoalesced();
	return eOK;
}

int WorldRuntime::windowAddHotspot(const QString &name, const QString &hotspotId, int left, int top,
                                   int right, int bottom, const QString &mouseOver,
                                   const QString &cancelMouseOver, const QString &mouseDown,
                                   const QString &cancelMouseDown, const QString &mouseUp,
                                   const QString &tooltip, int cursor, int flags, const QString &pluginId)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;

	static thread_local bool s_inWindowAddHotspot = false;
	if (s_inWindowAddHotspot)
		return eItemInUse;
	struct AddHotspotGuard
	{
			bool &flag;
			~AddHotspotGuard()
			{
				flag = false;
			}
	} const guard{s_inWindowAddHotspot};
	s_inWindowAddHotspot = true;

	if ((!mouseOver.isEmpty() && !isValidScriptLabel(mouseOver)) ||
	    (!cancelMouseOver.isEmpty() && !isValidScriptLabel(cancelMouseOver)) ||
	    (!mouseDown.isEmpty() && !isValidScriptLabel(mouseDown)) ||
	    (!cancelMouseDown.isEmpty() && !isValidScriptLabel(cancelMouseDown)) ||
	    (!mouseUp.isEmpty() && !isValidScriptLabel(mouseUp)))
		return eInvalidObjectLabel;

	const int result =
	    MiniWindowUtils::addHotspot(*window, hotspotId, left, top, right, bottom, mouseOver, cancelMouseOver,
	                                mouseDown, cancelMouseDown, mouseUp, tooltip, cursor, flags, pluginId);
	if (result != eOK)
		return result;
	const MiniWindowHotspot &hotspot = window->hotspots.value(hotspotId);
	if (m_view && !m_view->isMiniWindowCaptureActive() && m_view->hasLastMousePosition())
	{
		const QPoint lastGlobal = m_view->lastMousePosition();
		if (window->rect.contains(lastGlobal) && hotspot.rect.contains(window->lastMousePosition))
			m_view->recheckMiniWindowHover();
	}
	return eOK;
}

int WorldRuntime::windowDeleteHotspot(const QString &name, const QString &hotspotId)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	return MiniWindowUtils::deleteHotspot(*window, hotspotId);
}

int WorldRuntime::windowDeleteAllHotspots(const QString &name)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	MiniWindowUtils::deleteAllHotspots(*window);
	return eOK;
}

QStringList WorldRuntime::windowHotspotList(const QString &name) const
{
	const MiniWindow *window = miniWindow(name);
	if (!window)
		return {};
	return window->hotspots.keys();
}

QVariant WorldRuntime::windowHotspotInfo(const QString &name, const QString &hotspotId, int infoType) const
{
	const MiniWindow *window = miniWindow(name);
	if (!window)
		return {};
	return MiniWindowUtils::hotspotInfo(*window, hotspotId, infoType);
}

int WorldRuntime::windowOutputActivate(const QString &name, const QString &hotspotId,
                                       const bool deferDispatch)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;

	const auto hotspotIt = window->hotspots.find(hotspotId);
	if (hotspotIt == window->hotspots.end())
		return eHotspotNotInstalled;

	const MiniWindowHotspot &hotspot = hotspotIt.value();
	if (!MiniWindowUtils::hasActivatableAction(hotspot.outputActionType, hotspot.outputAction, ActionNone))
		return eBadParameter;

	const int     actionType = hotspot.outputActionType;
	const QString action     = hotspot.outputAction;
	if (deferDispatch)
	{
		qmudInvokeMethodQueued(this, [this, actionType, action]
		                       { emit miniWindowOutputActionActivated(actionType, action); });
	}
	else
		emit miniWindowOutputActionActivated(actionType, action);
	return eOK;
}

int WorldRuntime::windowHotspotTooltip(const QString &name, const QString &hotspotId, const QString &tooltip)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	return MiniWindowUtils::setHotspotTooltip(*window, hotspotId, tooltip);
}

int WorldRuntime::windowMoveHotspot(const QString &name, const QString &hotspotId, int left, int top,
                                    int right, int bottom)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	return MiniWindowUtils::moveHotspot(*window, hotspotId, left, top, right, bottom);
}

int WorldRuntime::windowDragHandler(const QString &name, const QString &hotspotId,
                                    const QString &moveCallback, const QString &releaseCallback, int flags,
                                    const QString &pluginId)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	if ((!moveCallback.isEmpty() && !isValidScriptLabel(moveCallback)) ||
	    (!releaseCallback.isEmpty() && !isValidScriptLabel(releaseCallback)))
		return eInvalidObjectLabel;
	return MiniWindowUtils::setHotspotDragHandler(*window, hotspotId, moveCallback, releaseCallback, flags,
	                                              pluginId);
}

int WorldRuntime::windowScrollwheelHandler(const QString &name, const QString &hotspotId,
                                           const QString &moveCallback, const QString &pluginId)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	if (!moveCallback.isEmpty() && !isValidScriptLabel(moveCallback))
		return eInvalidObjectLabel;
	return MiniWindowUtils::setHotspotScrollwheelHandler(*window, hotspotId, moveCallback, pluginId);
}

QString WorldRuntime::windowMenu(const QString &name, int left, int top, const QString &items,
                                 const QString &pluginId)
{
	Q_UNUSED(pluginId);
	if (!m_view)
		return {};

	MiniWindow const *window = miniWindow(name);
	if (!window)
		return {};

	return qmudShowLuaMiniWindowMenuDialog(m_view, window, left, top, items);
}

void WorldRuntime::setView(WorldView *view)
{
	if (m_viewDestroyedConnection)
	{
		QObject::disconnect(m_viewDestroyedConnection);
		m_viewDestroyedConnection = QMetaObject::Connection{};
	}
	m_view = view;
	if (m_view)
	{
		m_viewDestroyedConnection = connect(m_view, &QObject::destroyed, this,
		                                    [this]
		                                    {
			                                    m_view                    = nullptr;
			                                    m_viewDestroyedConnection = QMetaObject::Connection{};
		                                    });
	}
	else
	{
		return;
	}

	syncMiniWindowDevicePixelRatioForView();
}

bool WorldRuntime::syncMiniWindowDevicePixelRatioForView()
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::syncMiniWindowDevicePixelRatioForView");
	if (!m_view)
		return false;

	const double ratio = miniWindowDevicePixelRatioForView(m_view);
	bool         changed{false};
	for (MiniWindow &window : m_miniWindows)
	{
		const QSize physicalSize = MiniWindow::backingStoreSize(window.width, window.height, ratio);
		if (!qFuzzyCompare(window.devicePixelRatio, ratio) ||
		    !qFuzzyCompare(window.backingSurfaceDevicePixelRatio(), ratio) ||
		    window.backingSurfaceSize() != physicalSize)
		{
			MiniWindowUtils::setDevicePixelRatio(window, ratio);
			changed = true;
		}
	}
	if (changed)
		emitMiniWindowsChangedCoalesced();
	return changed;
}

WorldView *WorldRuntime::view() const
{
	return m_view;
}

void WorldRuntime::queueDeferredConnectAfterPluginInstall(const QString &host, const quint16 port)
{
	if (host.trimmed().isEmpty() || port == 0)
	{
		m_deferredConnectAfterPluginInstallPending = false;
		m_deferredConnectHost.clear();
		m_deferredConnectPort = 0;
		return;
	}
	m_deferredConnectAfterPluginInstallPending = true;
	m_deferredConnectHost                      = host;
	m_deferredConnectPort                      = port;
}

bool WorldRuntime::pluginInstallViewReady() const
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::pluginInstallViewReady");
	if (!m_view)
		return false;
	if (m_view->outputClientWidth() <= 0 || m_view->outputClientHeight() <= 0)
		return false;
	return true;
}

void WorldRuntime::schedulePendingPluginInstallRetry()
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::schedulePendingPluginInstallRetry");
	if (m_pluginInstallDeferred || m_pluginInstallInProgress)
		return;
	if (!hasPendingPluginInstallWork())
	{
		flushPluginInstallCommittedCompletions();
		return;
	}
	installPendingPlugins();
}

bool WorldRuntime::hasPendingPluginInstallWork() const
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::hasPendingPluginInstallWork");
	return std::ranges::any_of(m_plugins,
	                           [](const Plugin &plugin)
	                           {
		                           return plugin.installPending &&
		                                  (plugin.enabled || plugin.disableAfterInstall) && plugin.lua &&
		                                  hasValidPluginId(plugin);
	                           });
}

void WorldRuntime::flushPluginInstallCommittedCompletions()
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::flushPluginInstallCommittedCompletions");
	if (m_pluginInstallDeferred || m_pluginInstallInProgress || hasPendingPluginInstallWork())
		return;
	if (m_pluginInstallCommittedWaiters.isEmpty())
		return;

	QVector<std::function<void()>> completions;
	completions.swap(m_pluginInstallCommittedWaiters);
	for (auto &completion : completions)
	{
		if (completion)
			completion();
	}
}

void WorldRuntime::maybeRunDeferredConnectAfterPluginInstall()
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::maybeRunDeferredConnectAfterPluginInstall");
	if (!m_deferredConnectAfterPluginInstallPending)
		return;
	if (m_pluginInstallDeferred || m_pluginInstallInProgress || hasPendingPluginInstallWork())
		return;

	const QString host                         = m_deferredConnectHost.trimmed();
	const quint16 port                         = m_deferredConnectPort;
	m_deferredConnectAfterPluginInstallPending = false;
	m_deferredConnectHost.clear();
	m_deferredConnectPort = 0;
	if (host.isEmpty() || port == 0)
		return;
	if (isConnected() || isConnecting())
		return;
	static_cast<void>(connectToWorld(host, port));
}

void WorldRuntime::maybeRunDeferredWorldConnectHandlers()
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::maybeRunDeferredWorldConnectHandlers");
	if (!m_deferredWorldConnectHandlersPending)
		return;
	if (!isConnected())
	{
		m_deferredWorldConnectHandlersPending = false;
		return;
	}
	if (m_pluginInstallDeferred || m_pluginInstallInProgress || hasPendingPluginInstallWork())
		return;
	m_deferredWorldConnectHandlersPending = false;
	fireWorldConnectHandlers();
}

void WorldRuntime::queuePluginInstall(Plugin &plugin)
{
	if (!plugin.lua || !hasValidPluginId(plugin))
		return;

	const auto setInstallPending = [this, &plugin](const bool pending)
	{
		if (plugin.installPending == pending)
			return;
		plugin.installPending = pending;
		invalidatePluginCallbackPresenceCache();
	};

	if (!plugin.enabled && !plugin.disableAfterInstall)
	{
		setInstallPending(true);
		return;
	}
	if (m_loadingDocument || m_pluginInstallDeferred || m_pluginInstallInProgress)
	{
		setInstallPending(true);
		return;
	}

	setInstallPending(true);
	installPendingPluginsAsyncDrain();
}

void WorldRuntime::installPendingPlugins()
{
	installPendingPluginsAsyncDrain();
}

void WorldRuntime::installPendingPluginsAsyncDrain()
{
	if (m_pluginInstallDeferred || m_pluginInstallInProgress)
		return;

	QVector<QString> pendingPluginIds;
	pendingPluginIds.reserve(m_plugins.size());
	for (const Plugin &plugin : m_plugins)
	{
		if (plugin.installPending && (plugin.enabled || plugin.disableAfterInstall) && plugin.lua &&
		    hasValidPluginId(plugin))
		{
			const QString pluginId = normalizedPluginId(plugin.attributes.value(QStringLiteral("id")));
			if (!pluginId.isEmpty())
				pendingPluginIds.push_back(pluginId);
		}
	}
	if (pendingPluginIds.isEmpty())
	{
		maybeRunDeferredConnectAfterPluginInstall();
		maybeRunDeferredWorldConnectHandlers();
		flushPluginInstallCommittedCompletions();
		return;
	}

	if (!pluginInstallViewReady())
	{
		return;
	}

	m_pluginInstallInProgress = true;
	continuePendingPluginInstallAsync(std::move(pendingPluginIds), false);
}

void WorldRuntime::continuePendingPluginInstallAsync(QVector<QString> pendingPluginIds,
                                                     const bool       installedAny)
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::continuePendingPluginInstallAsync");
	if (!m_pluginInstallInProgress)
		return;

	while (!pendingPluginIds.isEmpty())
	{
		const QString pluginId = pendingPluginIds.takeFirst();
		const int     index    = findPluginIndex(m_plugins, pluginId);
		if (index < 0)
			continue;
		Plugin &plugin = m_plugins[index];
		if (!plugin.lua || (!plugin.enabled && !plugin.disableAfterInstall))
			continue;

		QSharedPointer<LuaCallbackEngine> installEngine = plugin.lua;

		LuaBatchDispatchRequest           installRequest;
		installRequest.kind          = LuaBatchDispatchKind::NoArgs;
		installRequest.engines       = {installEngine};
		installRequest.functionName  = QStringLiteral("OnPluginInstall");
		installRequest.defaultResult = true;

		queuePluginCallbackDispatchAsync(
		    installRequest,
		    [this, pluginId, pendingPluginIds = std::move(pendingPluginIds)](
		        const LuaBatchDispatchResult &dispatchResult) mutable
		    {
			    const int currentIndex = findPluginIndex(m_plugins, pluginId);
			    if (currentIndex < 0)
			    {
				    continuePendingPluginInstallAsync(std::move(pendingPluginIds), true);
				    return;
			    }

			    Plugin &currentPlugin       = m_plugins[currentIndex];
			    auto    clearInstallPending = [this, &currentPlugin]()
			    {
				    if (!currentPlugin.installPending)
					    return;
				    currentPlugin.installPending = false;
				    invalidatePluginCallbackPresenceCache();
			    };
			    if (dispatchResult.boolResultValid && !dispatchResult.boolResult)
			    {
				    currentPlugin.enabled = false;
				    currentPlugin.attributes.insert(QStringLiteral("enabled"), QStringLiteral("0"));
				    currentPlugin.disableAfterInstall = false;
				    clearInstallPending();
				    invalidateLuaCallbackDispatchSnapshot();
				    if (!m_loadingDocument)
					    m_worldFileModified = true;
				    continuePendingPluginInstallAsync(std::move(pendingPluginIds), true);
				    return;
			    }
			    if (!currentPlugin.disableAfterInstall)
			    {
				    clearInstallPending();
				    continuePendingPluginInstallAsync(std::move(pendingPluginIds), true);
				    return;
			    }

			    if (!currentPlugin.enabled)
			    {
				    currentPlugin.attributes.insert(QStringLiteral("enabled"), QStringLiteral("0"));
				    currentPlugin.disableAfterInstall = false;
				    clearInstallPending();
				    continuePendingPluginInstallAsync(std::move(pendingPluginIds), true);
				    return;
			    }

			    currentPlugin.enabled = false;
			    currentPlugin.attributes.insert(QStringLiteral("enabled"), QStringLiteral("0"));
			    currentPlugin.disableAfterInstall = false;

			    LuaBatchDispatchRequest disableRequest;
			    disableRequest.kind          = LuaBatchDispatchKind::NoArgs;
			    disableRequest.engines       = {currentPlugin.lua};
			    disableRequest.functionName  = QStringLiteral("OnPluginDisable");
			    disableRequest.defaultResult = true;

			    queuePluginCallbackDispatchAsync(
			        disableRequest,
			        [this, pluginId, pendingPluginIds = std::move(pendingPluginIds)](
			            const LuaBatchDispatchResult & /*unused*/) mutable
			        {
				        if (const int disabledIndex = findPluginIndex(m_plugins, pluginId);
				            disabledIndex >= 0 && m_plugins[disabledIndex].installPending)
				        {
					        m_plugins[disabledIndex].installPending = false;
					        invalidatePluginCallbackPresenceCache();
				        }
				        continuePendingPluginInstallAsync(std::move(pendingPluginIds), true);
			        });
		    });
		return;
	}

	finishPendingPluginInstallAsync(installedAny);
}

void WorldRuntime::finishPendingPluginInstallAsync(const bool installedAny)
{
	qmudAssertObjectThreadAffinity(this, "WorldRuntime::finishPendingPluginInstallAsync");
	m_pluginInstallInProgress = false;

	if (installedAny)
	{
		invalidatePluginCallbackPresenceCache();
		notifyWorldOutputResized();
		notifyDrawOutputWindow(1, 0);
	}

	if (!m_pluginInstallDeferred && hasPendingPluginInstallWork())
	{
		installPendingPluginsAsyncDrain();
		return;
	}

	maybeRunDeferredConnectAfterPluginInstall();
	maybeRunDeferredWorldConnectHandlers();
	flushPluginInstallCommittedCompletions();
}

void WorldRuntime::installPendingPluginsAsync(std::function<void()>       completion,
                                              PluginInstallCompletionMode mode)
{
	if (QThread::currentThread() != thread())
	{
		const bool queued =
		    qmudInvokeMethodQueued(this, [this, completion = std::move(completion), mode]() mutable
		                           { installPendingPluginsAsync(std::move(completion), mode); });
		if (!queued)
		{
			qWarning().noquote() << QStringLiteral(
			    "[QMud][LuaBridge] failed to queue installPendingPluginsAsync to runtime thread");
		}
		return;
	}

	qmudAssertObjectThreadAffinity(this, "WorldRuntime::installPendingPluginsAsync");
	if (mode == PluginInstallCompletionMode::Committed)
	{
		if (completion)
			m_pluginInstallCommittedWaiters.push_back(std::move(completion));
		installPendingPlugins();
		return;
	}

	installPendingPlugins();
	if (completion)
		completion();
}

void WorldRuntime::setPluginInstallDeferred(bool deferred)
{
	m_pluginInstallDeferred = deferred;
	if (deferred)
	{
		m_pluginInstallRetryQueued = false;
		m_pluginInstallInProgress  = false;
		return;
	}
	installPendingPluginsAsync({}, PluginInstallCompletionMode::Staged);
}

bool WorldRuntime::pluginInstallDeferred() const
{
	return m_pluginInstallDeferred;
}

void WorldRuntime::setBackgroundColour(long colour)
{
	m_backgroundColour = colour;
	if (m_view)
	{
		m_view->applyRuntimeSettings();
		m_view->update();
	}
}

long WorldRuntime::backgroundColour() const
{
	return m_backgroundColour;
}

bool WorldRuntime::doingSimulate() const
{
	return m_doingSimulate;
}

void WorldRuntime::setDoingSimulate(bool value)
{
	m_doingSimulate = value;
}
