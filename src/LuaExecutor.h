/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: LuaExecutor.h
 * Role: Lua callback execution abstraction used by runtime dispatch code so execution backends can be swapped
 * without changing callback call sites.
 */

#ifndef QMUD_LUAEXECUTOR_H
#define QMUD_LUAEXECUTOR_H

// ReSharper disable once CppUnusedIncludeDirective
#include "AnsiSgrParseUtils.h"

#include <QByteArray>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDateTime>
// ReSharper disable once CppUnusedIncludeDirective
#include <QHash>
// ReSharper disable once CppUnusedIncludeDirective
#include <QList>
// ReSharper disable once CppUnusedIncludeDirective
#include <QMap>
// ReSharper disable once CppUnusedIncludeDirective
#include <QPointer>
// ReSharper disable once CppUnusedIncludeDirective
#include <QRect>
// ReSharper disable once CppUnusedIncludeDirective
#include <QSet>
// ReSharper disable once CppUnusedIncludeDirective
#include <QSharedPointer>
#include <QString>
// ReSharper disable once CppUnusedIncludeDirective
#include <QStringList>
// ReSharper disable once CppUnusedIncludeDirective
#include <QVariant>
// ReSharper disable once CppUnusedIncludeDirective
#include <QVector>
#include <functional>
#include <memory>
#include <utility>

class LuaCallbackEngine;
class WorldRuntime;
class QObject;
struct LuaEngineObservedInitializationRequest;
struct MiniWindow;
#ifdef QMUD_ENABLE_LUA_SCRIPTING
struct lua_State;
#endif

/**
 * @brief Attribute/children payload used by callback-scope runtime snapshots.
 */
struct LuaCallbackAttributeChildrenSnapshot
{
		QMap<QString, QString> attributes;
		QMap<QString, QString> children;
};

/**
 * @brief Attribute/content payload used by callback-scope runtime snapshots.
 */
struct LuaCallbackAttributeContentSnapshot
{
		QMap<QString, QString> attributes;
		QString                content;
};

/**
 * @brief Trigger payload used by callback-scope rule-list snapshots.
 */
struct LuaCallbackTriggerSnapshot
{
		QMap<QString, QString> attributes;
		QMap<QString, QString> children;
		bool                   included{false};
		int                    matched{0};
		int                    invocationCount{0};
		int                    matchAttempts{0};
		QString                lastMatchTarget;
		QDateTime              lastMatched;
		quint64                runtimeId{0};
		int                    executingScriptDepth{0};
		bool                   executingScript{false};
};

/**
 * @brief Alias payload used by callback-scope rule-list snapshots.
 */
struct LuaCallbackAliasSnapshot
{
		QMap<QString, QString> attributes;
		QMap<QString, QString> children;
		bool                   included{false};
		int                    matched{0};
		int                    invocationCount{0};
		int                    matchAttempts{0};
		QString                lastMatchTarget;
		QDateTime              lastMatched;
		quint64                runtimeId{0};
		int                    executingScriptDepth{0};
		bool                   executingScript{false};
};

/**
 * @brief Timer payload used by callback-scope rule-list snapshots.
 */
struct LuaCallbackTimerSnapshot
{
		QMap<QString, QString> attributes;
		QMap<QString, QString> children;
		bool                   included{false};
		QDateTime              lastFired;
		QDateTime              nextFireTime;
		int                    firedCount{0};
		int                    invocationCount{0};
		bool                   executingScript{false};
		quint64                runtimeId{0};
		int                    executingScriptDepth{0};
};

/**
 * @brief Accelerator payload used by callback-scope runtime snapshots.
 */
struct LuaCallbackAcceleratorSnapshot
{
		qint64  key{0};
		int     commandId{-1};
		QString text;
		int     sendTo{0};
};

/**
 * @brief World entry payload used by callback-scope world-list snapshots.
 */
struct LuaCallbackWorldRuntimeSnapshot
{
		QPointer<WorldRuntime> runtime;
		QString                id;
		QString                name;
};

/**
 * @brief World child-window geometry payload captured before callback dispatch.
 */
struct LuaCallbackWorldWindowPositionSnapshot
{
		WorldRuntime *runtime{nullptr};
		int           ordinal{0};
		QRect         normalGeometry;
		QRect         frameGeometry;
		QRect         screenNormalGeometry;
		QRect         screenFrameGeometry;
};

/**
 * @brief Notepad window payload captured before callback dispatch.
 */
struct LuaCallbackNotepadSnapshot
{
		WorldRuntime *runtime{nullptr};
		QString       worldId;
		QString       title;
		QRect         geometry;
		QString       text;
		bool          hasEditor{false};
		bool          hasGeometry{false};
		bool          hasText{false};
};

/**
 * @brief Style span payload used by callback-scope output-buffer snapshots.
 */
struct LuaCallbackLineStyleSnapshot
{
		int     length{0};
		quint32 foreRgba{0};
		quint32 backRgba{0};
		bool    foreValid{false};
		bool    backValid{false};
		bool    bold{false};
		bool    underline{false};
		bool    italic{false};
		bool    blink{false};
		bool    strike{false};
		bool    inverse{false};
		bool    changed{false};
		int     actionType{0};
		QString action;
		QString hint;
		QString variable;
		bool    startTag{false};

		bool    operator==(const LuaCallbackLineStyleSnapshot &) const = default;
};

/**
 * @brief Styled text run exposed to Lua together with its lossless source-span segmentation.
 *
 * The first four fields are the MUSHclient-compatible callback projection. `sourceSpans` is internal
 * transport used when the same callback line must also back GetStyleInfo without losing action or style
 * metadata while adjacent visually equivalent callback runs are merged.
 */
struct LuaStyleRun
{
		QString                               text;
		int                                   textColour{0};
		int                                   backColour{0};
		int                                   style{0};
		QVector<LuaCallbackLineStyleSnapshot> sourceSpans;
};

/**
 * @brief Output-buffer line payload used by callback-scope read snapshots.
 */
struct LuaCallbackLineEntrySnapshot
{
		QString                               text;
		int                                   flags{0};
		bool                                  hardReturn{true};
		QVector<LuaCallbackLineStyleSnapshot> spans;
		QDateTime                             time;
		qint64                                lineNumber{0};
		double                                ticks{0.0};
		double                                elapsed{0.0};
};

/**
 * @brief Immutable output-buffer snapshot shared by callback dispatches that need line APIs.
 */
struct LuaCallbackLineBufferSnapshot
{
		quint64                                  lineBufferGeneration{0};
		int                                      lineBufferCount{0};
		int                                      firstCapturedLineBufferIndex{0};
		int                                      lastCapturedLineBufferIndex{0};
		QHash<int, LuaCallbackLineEntrySnapshot> lineEntriesByBufferIndex;
		QStringList                              recentLinesSnapshot;
};

/**
 * @brief ANSI style state captured for callback-scope `WindowOutputText` rendering.
 */
struct LuaCallbackAnsiRenderStateSnapshot
{
		bool    bold{false};
		bool    underline{false};
		bool    italic{false};
		bool    blink{false};
		bool    inverse{false};
		bool    strike{false};
		bool    monospace{false};
		QString fore;
		QString back;
		int     foregroundAnsiIndex{-1};
		bool    foregroundAnsiBright{false};
		int     actionType{0};
		QString action;
		QString hint;
		QString variable;
		bool    startTag{false};
};

/**
 * @brief MXP style state captured for callback-scope `WindowOutputText` rendering.
 */
struct LuaCallbackMxpStyleStateSnapshot
{
		bool    bold{false};
		bool    underline{false};
		bool    italic{false};
		bool    blink{false};
		bool    strike{false};
		bool    monospace{false};
		bool    inverse{false};
		QString fore;
		QString back;
		int     foregroundAnsiIndex{-1};
		bool    foregroundAnsiBright{false};
		int     actionType{0};
		QString action;
		QString hint;
		QString variable;
		bool    startTag{false};
};

/**
 * @brief MXP style stack frame captured for callback-scope `WindowOutputText`.
 */
struct LuaCallbackMxpStyleFrameSnapshot
{
		QByteArray                       tag;
		LuaCallbackMxpStyleStateSnapshot state;
		LuaCallbackMxpStyleStateSnapshot actionState;
		int                              actionTextLineNumber{-1};
		int                              actionTextStartColumn{0};
		qint64                           actionTextRuntimeLineNumber{-1};
		quint64                          actionTextPartialLineRevision{0};
};

/**
 * @brief Custom MXP element definition captured for callback-scope `WindowOutputText`.
 */
struct LuaCallbackMxpCustomElementSnapshot
{
		QByteArray name;
		bool       open{false};
		bool       command{false};
		int        tag{0};
		QByteArray flag;
		QByteArray definition;
		QByteArray attributes;
};

/**
 * @brief SQLite handle/query state used by callback-scope database read snapshots.
 */
struct LuaCallbackDatabaseSnapshot
{
		QString           diskName;
		bool              isOpen{false};
		bool              stmtPrepared{false};
		bool              validRow{false};
		int               columns{0};
		int               columnsStatus{0};
		int               lastError{0};
		QString           errorText;
		int               totalChanges{0};
		int               changes{0};
		QString           lastInsertRowid;
		QStringList       columnNames;
		QVector<QVariant> columnValues;
};

/**
 * @brief Batch callback command kinds routed through the Lua executor.
 */
enum class LuaBatchDispatchKind
{
	NoArgs,
	HasFunction,
	String,
	StringStopOnFalse,
	StringHandled,
	Bytes,
	BytesInOut,
	StringInOut,
	NumberAndStringStopOnTrue,
	NumberAndStringStopOnFalse,
	NumberAndString,
	TwoNumbersAndStringStopOnFalse,
	TwoNumbersAndString,
	NumberAndBytesStopOnTrue,
	NumberAndBytes,
	NumberAndUtf8StringsCount,
	StringsAndWildcards,
	ExecuteScript,
	ResetAndLoadScript,
	InitializeEnginesWithObservedCallbacksMany,
	UpdateObservedCallbacksMany,
	CallPluginLuaMarshalling,
	TeardownEnginesMany,
	ApplyPackageRestrictionsMany,
	ProcedureWithString,
	MxpError,
	MxpStartUp,
	MxpShutDown,
	MxpStartTag,
	MxpEndTag,
	MxpSetVariable,
	CancelSuspendedModalString,
	ResumeSuspendedModalString
};

/**
 * @brief Worker lane selection for Lua batch dispatch commands.
 */
enum class LuaBatchDispatchLane
{
	Control,
	Callback
};

/**
 * @brief Output-line snapshot depth requested by a callback dispatch.
 */
enum class LuaCallbackLineSnapshotPolicy
{
	None,
	CountAndLast,
	CountAndRecentText,
	CountAndRecent,
	Full
};

/**
 * @brief Exact miniwindow resource identity used by callback snapshots and caches.
 */
struct LuaCallbackMiniWindowResourceKey
{
		QString       windowName;
		QString       resourceId;

		bool          operator==(const LuaCallbackMiniWindowResourceKey &) const = default;
		friend size_t qHash(const LuaCallbackMiniWindowResourceKey &key, size_t seed = 0) noexcept
		{
			seed = ::qHash(key.windowName, seed);
			return ::qHash(key.resourceId, seed);
		}
};

/**
 * @brief Exact miniwindow identity paired with a WindowInfo selector.
 */
struct LuaCallbackMiniWindowInfoKey
{
		QString       windowName;
		int           infoType{0};

		bool          operator==(const LuaCallbackMiniWindowInfoKey &) const = default;
		friend size_t qHash(const LuaCallbackMiniWindowInfoKey &key, size_t seed = 0) noexcept
		{
			seed = ::qHash(key.windowName, seed);
			return ::qHash(key.infoType, seed);
		}
};

/**
 * @brief Exact miniwindow/font identity paired with a WindowFontInfo selector.
 */
struct LuaCallbackMiniWindowFontInfoKey
{
		QString       windowName;
		QString       fontId;
		int           infoType{0};

		bool          operator==(const LuaCallbackMiniWindowFontInfoKey &) const = default;
		friend size_t qHash(const LuaCallbackMiniWindowFontInfoKey &key, size_t seed = 0) noexcept
		{
			seed = ::qHash(key.windowName, seed);
			seed = ::qHash(key.fontId, seed);
			return ::qHash(key.infoType, seed);
		}
};

/**
 * @brief Exact miniwindow/font/text identity used by callback text-width caches.
 */
struct LuaCallbackMiniWindowTextWidthKey
{
		QString       windowName;
		QString       fontId;
		QString       text;

		bool          operator==(const LuaCallbackMiniWindowTextWidthKey &) const = default;
		friend size_t qHash(const LuaCallbackMiniWindowTextWidthKey &key, size_t seed = 0) noexcept
		{
			seed = ::qHash(key.windowName, seed);
			seed = ::qHash(key.fontId, seed);
			return ::qHash(key.text, seed);
		}
};

/**
 * @brief Runtime-thread miniwindow snapshot used to satisfy callback-lane read validation without
 *        forbidden reentrant runtime bridges.
 */
struct LuaCallbackMiniWindowSnapshot
{
		struct WindowInfoSnapshot
		{
				int       locationX{0};
				int       locationY{0};
				int       width{0};
				int       height{0};
				bool      show{false};
				bool      temporarilyHide{false};
				int       position{0};
				int       flags{0};
				qlonglong backgroundRef{0};
				int       rectLeft{0};
				int       rectTop{0};
				int       rectRight{0};
				int       rectBottom{0};
				int       lastMouseX{0};
				int       lastMouseY{0};
				int       lastMouseUpdate{0};
				int       clientMouseX{0};
				int       clientMouseY{0};
				QString   mouseOverHotspot;
				QString   mouseDownHotspot;
				double    installedAt{0.0};
				int       zOrder{0};
				QString   creatingPlugin;
		};

		QStringList                                   windowNames;
		QHash<QString, QStringList>                   fontIdsByWindow;
		QHash<QString, QStringList>                   imageIdsByWindow;
		QHash<QString, QStringList>                   hotspotIdsByWindow;
		QHash<LuaCallbackMiniWindowResourceKey, bool> imageHasAlphaByKey;
		QHash<QString, WindowInfoSnapshot>            windowInfoByWindow;
		QHash<QString, QSharedPointer<MiniWindow>>    miniWindowsByWindow;
		bool                                          miniWindowLookupCacheValid{false};
		QSet<QString>                                 miniWindowIds;
		QSet<LuaCallbackMiniWindowResourceKey>        miniWindowFontKeys;
		QSet<LuaCallbackMiniWindowResourceKey>        miniWindowImageKeys;
		QSet<LuaCallbackMiniWindowResourceKey>        miniWindowHotspotKeys;
		void                                         *framePointer{nullptr};
		bool                                          hasFramePointer{false};
		bool                                          hasCommandUiSnapshot{false};
		bool                                          hasCommandHistorySnapshot{false};
		QStringList                                   commandHistorySnapshot;
		bool                                          commandUiHasView{false};
		bool                                          commandUiHasFrameData{false};
		int                                           commandUiOutputClientHeight{0};
		int                                           commandUiOutputClientWidth{0};
		int                                           commandUiViewHeight{0};
		int                                           commandUiViewWidth{0};
		QString                                       activeMiniWindowExecutionName;
		QString                                       geometryConstrainedMiniWindowName;
		int                                           geometryConstraintDisplayClientHeight{0};
		int                                           geometryConstraintDisplayClientWidth{0};
		double                                        absoluteMiniWindowScaleXOver{1.0};
		double                                        absoluteMiniWindowScaleYOver{1.0};
		double                                        absoluteMiniWindowScaleXUnder{1.0};
		double                                        absoluteMiniWindowScaleYUnder{1.0};
		bool                                          hasRuntimeCountersSnapshot{false};
		int                                           runtimeOutputFontHeight{0};
		int                                           runtimeOutputFontWidth{0};
		void                                          rebuildMiniWindowLookupCaches()
		{
			auto addWindowKey = [this](const QString &windowName)
			{
				if (!windowName.isEmpty())
					miniWindowIds.insert(windowName);
				return windowName;
			};
			auto addItemKeys = [&addWindowKey](const QHash<QString, QStringList>      &itemsByWindow,
			                                   QSet<LuaCallbackMiniWindowResourceKey> &destination)
			{
				for (auto it = itemsByWindow.constBegin(); it != itemsByWindow.constEnd(); ++it)
				{
					const QString windowKey = addWindowKey(it.key());
					if (windowKey.isEmpty())
						continue;
					for (const QString &item : it.value())
					{
						if (!item.isEmpty())
							destination.insert({windowKey, item});
					}
				}
			};

			miniWindowIds.clear();
			miniWindowFontKeys.clear();
			miniWindowImageKeys.clear();
			miniWindowHotspotKeys.clear();

			for (const QString &windowName : windowNames)
				addWindowKey(windowName);
			for (auto it = miniWindowsByWindow.constBegin(); it != miniWindowsByWindow.constEnd(); ++it)
				addWindowKey(it.key());
			for (auto it = windowInfoByWindow.constBegin(); it != windowInfoByWindow.constEnd(); ++it)
				addWindowKey(it.key());

			addItemKeys(fontIdsByWindow, miniWindowFontKeys);
			addItemKeys(imageIdsByWindow, miniWindowImageKeys);
			addItemKeys(hotspotIdsByWindow, miniWindowHotspotKeys);
			miniWindowLookupCacheValid = true;
		}
		bool                                                  hasWorldVariablesSnapshot{false};
		QMap<QString, QString>                                worldVariablesSnapshot;
		QHash<QString, QMap<QString, QString>>                pluginVariablesSnapshotById;
		QSet<QString>                                         unavailablePluginVariableSnapshotIds;
		QHash<QString, QList<LuaCallbackTriggerSnapshot>>     triggerListsByPluginId;
		QSet<QString>                                         missingTriggerListPluginIds;
		QHash<QString, QList<LuaCallbackAliasSnapshot>>       aliasListsByPluginId;
		QSet<QString>                                         missingAliasListPluginIds;
		QHash<QString, QList<LuaCallbackTimerSnapshot>>       timerListsByPluginId;
		QSet<QString>                                         missingTimerListPluginIds;
		bool                                                  hasWorldAttributeSnapshot{false};
		QMap<QString, QString>                                worldAttributesSnapshot;
		QMap<QString, QString>                                worldMultilineAttributesSnapshot;
		bool                                                  hasArraySnapshot{false};
		QStringList                                           arrayNamesSnapshot;
		QHash<QString, QMap<QString, QString>>                arraysByName;
		bool                                                  hasChatSnapshot{false};
		QList<long>                                           chatConnectionIdsSnapshot;
		QHash<long, QHash<int, QVariant>>                     chatInfoValuesById;
		QHash<long, QHash<QString, QVariant>>                 chatOptionValuesById;
		QHash<QString, long>                                  chatIdsByLookupKey;
		bool                                                  hasWindowOutputTextRenderSnapshot{false};
		QMudAnsiStreamState                                   windowOutputTextAnsiStreamState;
		LuaCallbackAnsiRenderStateSnapshot                    windowOutputTextAnsiRenderState;
		LuaCallbackMxpStyleStateSnapshot                      windowOutputTextMxpStyleState;
		QVector<LuaCallbackMxpStyleFrameSnapshot>             windowOutputTextMxpStyleStack;
		QVector<QByteArray>                                   windowOutputTextMxpBlockStack;
		bool                                                  windowOutputTextMxpLinkOpen{false};
		int                                                   windowOutputTextMxpPreDepth{0};
		QVector<LuaCallbackMxpCustomElementSnapshot>          windowOutputTextCustomElements;
		QHash<int, long>                                      boldAnsiColoursByIndex;
		QHash<int, long>                                      normalAnsiColoursByIndex;
		QHash<int, long>                                      customTextColoursByIndex;
		QHash<int, long>                                      customBackgroundColoursByIndex;
		QHash<int, QString>                                   customColourNamesByIndex;
		QMap<QString, QStringList>                            triggerWildcardsSnapshot;
		QMap<QString, QMap<QString, QString>>                 triggerNamedWildcardsSnapshot;
		QMap<QString, QStringList>                            aliasWildcardsSnapshot;
		QMap<QString, QMap<QString, QString>>                 aliasNamedWildcardsSnapshot;
		QHash<QString, QMap<QString, QStringList>>            pluginTriggerWildcardsSnapshotById;
		QHash<QString, QMap<QString, QMap<QString, QString>>> pluginTriggerNamedWildcardsSnapshotById;
		QHash<QString, QMap<QString, QStringList>>            pluginAliasWildcardsSnapshotById;
		QHash<QString, QMap<QString, QMap<QString, QString>>> pluginAliasNamedWildcardsSnapshotById;
		bool                                                  hasMapColourSnapshot{false};
		QMap<long, long>                                      mapColourSnapshot;
		bool                                                  hasMappingEntriesSnapshot{false};
		QStringList                                           mappingEntriesSnapshot;
		bool                                                  hasUdpPortSnapshot{false};
		QList<int>                                            udpPortsSnapshot;
		QHash<int, QString>                                   udpListenerPluginIdsByPort;
		bool                                                  hasUsedUdpPortsSnapshot{false};
		QSet<int>                                             usedUdpPortsSnapshot;
		QHash<int, int>                                       usedUdpPortReferenceCountsSnapshot;
		QHash<int, int>                                       soundStatusByBuffer;
		QHash<int, bool>                                      soundBufferReusableByBuffer;
		bool                                                  hasLineBufferSnapshot{false};
		int                                                   lineBufferCount{0};
		QSharedPointer<const LuaCallbackLineBufferSnapshot>   lineBufferSnapshot;
		QHash<int, LuaCallbackLineEntrySnapshot>              lineEntriesByBufferIndex;
		bool                                                  hasCallbackOutputAnchor{false};
		int                                                   callbackOutputAnchorBufferIndex{0};
		qint64                                                callbackOutputAnchorAbsoluteNumber{0};
		bool                                                  triggerMatchedLineSnapshotResolved{false};
		bool                                                  hasTriggerMatchedLineSnapshot{false};
		int                                                   triggerMatchedLineBufferIndex{0};
		qint64                                                triggerMatchedLineAbsoluteNumber{0};
		LuaCallbackLineEntrySnapshot                          triggerMatchedLineSnapshot;
		bool                                                  hasLineBufferDeltaSnapshot{false};
		bool                                                  hasLineBufferCountDelta{false};
		int                                                   lineBufferDeltaCount{0};
		QHash<int, LuaCallbackLineEntrySnapshot>              lineEntryDeltasByBufferIndex;
		QSet<int>                                             missingLineEntryDeltasByBufferIndex;
		bool                                                  linePresentationRequiresRefresh{false};
		bool                                                  outputScrollPositionRequiresRefresh{false};
		bool                                              miniWindowGeometryConstraintRequiresRefresh{false};
		bool                                              hasRecentLinesSnapshot{false};
		QStringList                                       recentLinesSnapshot;
		bool                                              hasDatabaseListSnapshot{false};
		bool                                              databaseListSnapshotDirty{false};
		QStringList                                       databaseNamesSnapshot;
		bool                                              hasDatabaseSnapshot{false};
		QHash<QString, LuaCallbackDatabaseSnapshot>       databaseSnapshotsByName;
		QHash<QString, int>                               databaseColumnsByName;
		QHash<QString, QString>                           databaseErrorsByName;
		QHash<QString, QString>                           databaseColumnNamesByKey;
		QSet<QString>                                     missingDatabaseColumnNameKeys;
		QHash<QString, QString>                           databaseColumnTextByKey;
		QSet<QString>                                     missingDatabaseColumnTextKeys;
		QHash<QString, QVariant>                          databaseColumnValuesByKey;
		QSet<QString>                                     missingDatabaseColumnValueKeys;
		QHash<QString, int>                               databaseColumnTypesByKey;
		QHash<QString, QVariant>                          databaseInfoByKey;
		QSet<QString>                                     missingDatabaseInfoKeys;
		QHash<QString, QStringList>                       databaseColumnNamesByName;
		QSet<QString>                                     missingDatabaseColumnNamesByName;
		QHash<QString, QVector<QVariant>>                 databaseColumnValuesByName;
		QSet<QString>                                     missingDatabaseColumnValuesByName;
		QHash<QString, int>                               databaseTotalChangesByName;
		QHash<QString, int>                               databaseChangesByName;
		QHash<QString, QString>                           databaseLastInsertRowidByName;
		bool                                              hasMacroEntriesSnapshot{false};
		QList<LuaCallbackAttributeChildrenSnapshot>       macroEntriesSnapshot;
		bool                                              hasVariableEntriesSnapshot{false};
		QList<LuaCallbackAttributeContentSnapshot>        variableEntriesSnapshot;
		bool                                              hasKeypadEntriesSnapshot{false};
		QList<LuaCallbackAttributeContentSnapshot>        keypadEntriesSnapshot;
		bool                                              hasAcceleratorSnapshot{false};
		QVector<LuaCallbackAcceleratorSnapshot>           acceleratorSnapshot;
		QVariantHash                                      commandUiValues;
		QVariantHash                                      runtimeCounterValues;
		QHash<QString, QString>                           pluginNamesById;
		QHash<QString, QString>                           pluginDirectoriesById;
		QHash<QString, bool>                              pluginEnabledById;
		QHash<QString, bool>                              nativePluginSpeechEnabledById;
		QHash<QString, QSharedPointer<LuaCallbackEngine>> pluginEnginesById;
		QHash<QString, QSet<QString>>                     pluginLuaFunctionsById;
		QStringList                                       broadcastPluginIdsSnapshot;
		QVector<QSharedPointer<LuaCallbackEngine>>        broadcastPluginEnginesSnapshot;
		bool                                              hasBroadcastPluginSnapshot{false};
		QStringList                                       pluginIdsSnapshot;
		QHash<QString, QString>                           pluginIdsByLookupKey;
		QHash<QString, QHash<int, QVariant>>              pluginInfoValuesById;
		QHash<QString, bool>                              pluginCallbackPresenceByName;
		bool                                              hasEntitySnapshot{false};
		QHash<QString, QString>                           entityValuesByName;
		bool                                              hasUiSnapshot{false};
		QVariantMap                                       guiSystemValues;
		bool                                              hasClipboardText{false};
		QString                                           clipboardText;
		QHash<int, QRect>                                 mainWindowPositionsByMode;
		bool                                              mainWindowPositionsDirty{false};
		QHash<QString, QRect>                             worldWindowPositionsByKey;
		QSet<QString>                                     missingWorldWindowPositionKeys;
		QSet<int>                                         dirtyWorldWindowPositionOrdinals;
		QHash<QString, QRect>                             notepadWindowPositionsByKey;
		QSet<QString>                                     missingNotepadWindowPositionKeys;
		QSet<QString>                                     dirtyNotepadWindowPositionKeys;
		QSet<QString>                                     dirtyNotepadDocumentKeys;
		QHash<QString, int>                               notepadLengthByKey;
		QSet<QString>                                     missingNotepadLengthKeys;
		QHash<QString, QString>                           notepadTextByKey;
		QSet<QString>                                     missingNotepadTextKeys;
		QSet<QString>                                     dirtyNotepadListKeys;
		QHash<QString, QStringList>                       notepadListByKey;
		QSet<QString>                                     missingNotepadListKeys;
		QVector<LuaCallbackWorldRuntimeSnapshot>          worldRuntimeSnapshot;
		QVector<LuaCallbackWorldWindowPositionSnapshot>   worldWindowPositionSnapshot;
		bool                                              hasNotepadPresentationSnapshot{false};
		QVector<LuaCallbackNotepadSnapshot>               notepadSnapshot;
		int                                               actionSourceOverride{0};
		bool                                              hasActionSourceOverride{false};
};

/**
 * @brief Batch callback dispatch request payload.
 */
struct LuaBatchDispatchRequest
{
		LuaBatchDispatchKind          kind{LuaBatchDispatchKind::NoArgs};
		LuaBatchDispatchLane          lane{LuaBatchDispatchLane::Callback};
		LuaCallbackLineSnapshotPolicy lineSnapshotPolicy{LuaCallbackLineSnapshotPolicy::CountAndLast};
		QVector<QSharedPointer<LuaCallbackEngine>>                            engines;
		QString                                                               functionName;
		QString                                                               stringArg;
		QString                                                               stringArg2;
		QString                                                               miniWindowExecutionName;
		QSharedPointer<const QVector<LuaEngineObservedInitializationRequest>> initRequestsArg;
		QSet<QString>                                                         observedCallbackNamesArg;
		QStringList                                                           stringListArg;
		QStringList                                                           stringListArg2;
		QByteArray                                                            bytesArg;
		QByteArray                                                            bytesArg2;
		QByteArray                                                            bytesArg3;
		QMap<QString, QString>                                                mapArg;
		QSharedPointer<const QVector<LuaStyleRun>>                            styleRunsArg;
		QSharedPointer<const LuaCallbackMiniWindowSnapshot>                   miniWindowSnapshotArg;
		long                                                                  numberArg1{0};
		long                                                                  numberArg2{0};
		int                                                                   intArg1{0};
		int                                                                   intArg2{0};
		int     triggerMatchedLineBufferIndex{0};
		qint64  triggerMatchedLineAbsoluteNumber{0};
		int     callbackOutputAnchorBufferIndex{0};
		qint64  callbackOutputAnchorAbsoluteNumber{0};
		int     actionSourceOverride{0};
		bool    optionFlag{false};
		bool    defaultResult{false};
		bool    revalidateObservedRecipients{false};
		bool    hasActionSourceOverride{false};
		bool    hasCallbackOutputAnchor{false};
		bool    inputCritical{false};
		bool    lowPriority{false};
		bool    executeScriptHasTriggerContext{false};
		bool    triggerOutputReplacesMatchedLine{false};
		bool    applyCallingPluginContext{false};
		bool    screendrawExecutionGuard{false};
		bool    drawOutputWindowExecutionGuard{false};
		QString callingPluginId;
		quint64 modalResumeId{0};
		quint64 runtimeModalResumeId{0};
#ifdef QMUD_ENABLE_LUA_SCRIPTING
		lua_State *luaStateArg{nullptr};
#endif
		bool refreshCallbackCatalogAfter{false};
};

/**
 * @brief Runtime-owned mutation journal produced by a callback worker.
 *
 * The callback thread records ordered mutations while preserving callback-local read-your-writes
 * semantics. The owning runtime thread applies this journal after callback execution returns.
 */
struct LuaDeferredRuntimeMutationBatch
{
		QPointer<WorldRuntime>         runtime;
		QVector<std::function<void()>> mutations;
};

using LuaDeferredRuntimeMutationConsumer = std::function<void(QVector<LuaDeferredRuntimeMutationBatch>)>;

/**
 * @brief Recovery ownership for deferred mutations carried by an asynchronous dispatch result.
 *
 * Normal result consumption and worker shutdown atomically compete for the same backup. This
 * guarantees that an undelivered completion cannot lose mutations or apply them twice.
 */
class LuaDeferredRuntimeMutationDelivery final
{
	public:
		using DeliveryAction   = std::function<void(QVector<LuaDeferredRuntimeMutationBatch>)>;
		using DeliveryConsumer = std::function<bool(const DeliveryAction &)>;

		LuaDeferredRuntimeMutationDelivery(QVector<LuaDeferredRuntimeMutationBatch> backup,
		                                   DeliveryConsumer                         deliveryConsumer)
		    : m_backup(std::move(backup)), m_deliveryConsumer(std::move(deliveryConsumer))
		{
		}

		[[nodiscard]] bool consumeForDelivery(const DeliveryAction &consumer) const
		{
			return m_deliveryConsumer && m_deliveryConsumer(consumer);
		}

		[[nodiscard]] bool recoverUndelivered(const LuaDeferredRuntimeMutationConsumer &consumer)
		{
			return consumeForDelivery(
			    [this, consumer](QVector<LuaDeferredRuntimeMutationBatch> earlierBatches)
			    {
				    QVector<LuaDeferredRuntimeMutationBatch> backup = takeBackupForRecovery();
				    earlierBatches += std::move(backup);
				    if (!earlierBatches.isEmpty() && consumer)
					    consumer(std::move(earlierBatches));
			    });
		}

		[[nodiscard]] QVector<LuaDeferredRuntimeMutationBatch> takeBackupForRecovery()
		{
			QVector<LuaDeferredRuntimeMutationBatch> backup;
			backup.swap(m_backup);
			return backup;
		}

	private:
		QVector<LuaDeferredRuntimeMutationBatch> m_backup;
		DeliveryConsumer                         m_deliveryConsumer;
};

/**
 * @brief Shared presentation produced by an internal callback line-page request.
 */
struct LuaCallbackLinePageResult
{
		QPointer<WorldRuntime>                              runtime;
		QSharedPointer<const LuaCallbackLineBufferSnapshot> presentation;
		bool                                                hasRecentLinesSnapshot{false};
};

/**
 * @brief Pending string-result modal request produced by a yielded Lua callback.
 */
struct LuaPendingModalStringRequest
{
		std::function<QString()>                             guiCallable;
		std::function<void(WorldRuntime &, const QString &)> beforeRuntimeResumeCallback;
		std::function<void(const QString &)>                 beforeResumeCallback;
		std::function<void()>                                beforeCancelCallback;
		std::function<void(quint64, QString)>                resultCallback;
		QSharedPointer<LuaCallbackLinePageResult>            linePageResult;
		bool                                                 internalImmediateResume{false};
};

/**
 * @brief Batch callback dispatch result payload.
 */
struct LuaBatchDispatchResult
{
		bool                                               boolResult{false};
		bool                                               boolResultValid{false};
		bool                                               hasFunction{false};
		bool                                               hasFunctionValid{false};
		int                                                countResult{0};
		bool                                               countResultValid{false};
		QString                                            stringResult;
		QByteArray                                         bytesResult;
		int                                                marshallingError{0};
		bool                                               marshallingErrorValid{false};
		int                                                marshallingIndex{0};
		QByteArray                                         marshallingTypeName;
		QString                                            marshallingRuntimeError;
		int                                                marshallingReturnCount{0};
		bool                                               marshallingSameState{false};
		bool                                               suspended{false};
		bool                                               recipientMutationBoundary{false};
		int                                                nextEngineIndex{-1};
		bool                                               linePresentationRequiresRefresh{false};
		bool                                               outputScrollPositionRequiresRefresh{false};
		bool                                               outputScrollPositionChanged{false};
		bool                                               commandUiPresentationRequiresRefresh{false};
		bool                                               globalPresentationRequiresRefresh{false};
		bool                                               commandHistoryChanged{false};
		bool                                               notepadPresentationChanged{false};
		bool                                               hasNotepadPresentationSnapshot{false};
		QVector<LuaCallbackNotepadSnapshot>                notepadPresentationSnapshot;
		quint64                                            modalResumeId{0};
		int                                                suspendedEngineIndex{-1};
		bool                                               hasPendingModalStringRequest{false};
		LuaPendingModalStringRequest                       pendingModalStringRequest;
		QVector<LuaDeferredRuntimeMutationBatch>           deferredRuntimeMutationBatches;
		QSharedPointer<LuaDeferredRuntimeMutationDelivery> deferredRuntimeMutationDelivery;
};

enum class LuaBatchRecipientStopCondition
{
	Never,
	FalseResult,
	TrueResult,
	FunctionPresent
};

/**
 * @brief Aggregate result shape shared by initial, resumed, and fallback dispatch handling.
 */
enum class LuaBatchAggregateShape
{
	None,
	BooleanAndFunction,
	StopOnFalse,
	FunctionHandled,
	StopOnTrue,
	FunctionCount,
	FunctionAny,
	BytesInOut,
	StringInOut,
	LastResult
};

/**
 * @brief Returns the aggregate shape for a batch dispatch kind.
 * @param kind Dispatch kind to classify.
 * @return Shared aggregate result shape.
 */
[[nodiscard]] constexpr LuaBatchAggregateShape luaBatchDispatchAggregateShape(const LuaBatchDispatchKind kind)
{
	switch (kind)
	{
	case LuaBatchDispatchKind::NoArgs:
		return LuaBatchAggregateShape::BooleanAndFunction;
	case LuaBatchDispatchKind::StringStopOnFalse:
	case LuaBatchDispatchKind::NumberAndStringStopOnFalse:
	case LuaBatchDispatchKind::TwoNumbersAndStringStopOnFalse:
		return LuaBatchAggregateShape::StopOnFalse;
	case LuaBatchDispatchKind::StringHandled:
		return LuaBatchAggregateShape::FunctionHandled;
	case LuaBatchDispatchKind::NumberAndStringStopOnTrue:
	case LuaBatchDispatchKind::NumberAndBytesStopOnTrue:
		return LuaBatchAggregateShape::StopOnTrue;
	case LuaBatchDispatchKind::NumberAndUtf8StringsCount:
		return LuaBatchAggregateShape::FunctionCount;
	case LuaBatchDispatchKind::StringsAndWildcards:
		return LuaBatchAggregateShape::FunctionAny;
	case LuaBatchDispatchKind::BytesInOut:
		return LuaBatchAggregateShape::BytesInOut;
	case LuaBatchDispatchKind::StringInOut:
		return LuaBatchAggregateShape::StringInOut;
	case LuaBatchDispatchKind::String:
	case LuaBatchDispatchKind::Bytes:
	case LuaBatchDispatchKind::NumberAndString:
	case LuaBatchDispatchKind::TwoNumbersAndString:
	case LuaBatchDispatchKind::NumberAndBytes:
	case LuaBatchDispatchKind::ExecuteScript:
	case LuaBatchDispatchKind::ProcedureWithString:
	case LuaBatchDispatchKind::MxpError:
	case LuaBatchDispatchKind::MxpStartTag:
		return LuaBatchAggregateShape::LastResult;
	default:
		return LuaBatchAggregateShape::None;
	}
}

/**
 * @brief Builds the neutral result returned when a batch dispatch cannot run or continue.
 * @param request Dispatch whose public result contract must be preserved.
 * @return Neutral, fully initialized result for the dispatch kind.
 */
[[nodiscard]] inline LuaBatchDispatchResult
makeLuaBatchDispatchFallback(const LuaBatchDispatchRequest &request)
{
	LuaBatchDispatchResult result{};
	switch (luaBatchDispatchAggregateShape(request.kind))
	{
	case LuaBatchAggregateShape::BooleanAndFunction:
	case LuaBatchAggregateShape::StopOnFalse:
		result.boolResult       = true;
		result.boolResultValid  = true;
		result.hasFunction      = false;
		result.hasFunctionValid = true;
		break;
	case LuaBatchAggregateShape::FunctionHandled:
	case LuaBatchAggregateShape::StopOnTrue:
		result.boolResult       = false;
		result.boolResultValid  = true;
		result.hasFunction      = false;
		result.hasFunctionValid = true;
		break;
	case LuaBatchAggregateShape::FunctionCount:
		result.countResult      = 0;
		result.countResultValid = true;
		break;
	case LuaBatchAggregateShape::FunctionAny:
		result.hasFunction      = false;
		result.hasFunctionValid = true;
		break;
	case LuaBatchAggregateShape::BytesInOut:
		result.bytesResult = request.bytesArg;
		break;
	case LuaBatchAggregateShape::StringInOut:
		result.stringResult = request.stringArg;
		break;
	case LuaBatchAggregateShape::LastResult:
	case LuaBatchAggregateShape::None:
		break;
	}

	switch (request.kind)
	{
	case LuaBatchDispatchKind::HasFunction:
		result.hasFunction      = false;
		result.hasFunctionValid = true;
		break;
	case LuaBatchDispatchKind::ResetAndLoadScript:
	case LuaBatchDispatchKind::ExecuteScript:
	case LuaBatchDispatchKind::MxpError:
	case LuaBatchDispatchKind::MxpStartTag:
	case LuaBatchDispatchKind::CallPluginLuaMarshalling:
		result.boolResult      = false;
		result.boolResultValid = true;
		break;
	case LuaBatchDispatchKind::ProcedureWithString:
		result.boolResult       = false;
		result.boolResultValid  = true;
		result.hasFunction      = false;
		result.hasFunctionValid = true;
		break;
	default:
		break;
	}
	return result;
}

/**
 * @brief Returns the single stop policy shared by initial and continued callback dispatches.
 */
[[nodiscard]] constexpr LuaBatchRecipientStopCondition
luaBatchDispatchRecipientStopCondition(const LuaBatchDispatchKind kind)
{
	switch (luaBatchDispatchAggregateShape(kind))
	{
	case LuaBatchAggregateShape::StopOnFalse:
		return LuaBatchRecipientStopCondition::FalseResult;
	case LuaBatchAggregateShape::FunctionHandled:
		return LuaBatchRecipientStopCondition::FunctionPresent;
	case LuaBatchAggregateShape::StopOnTrue:
		return LuaBatchRecipientStopCondition::TrueResult;
	default:
		return LuaBatchRecipientStopCondition::Never;
	}
}

[[nodiscard]] inline bool luaBatchDispatchStopsAfterRecipient(const LuaBatchDispatchKind kind,
                                                              const bool hasFunction, const bool boolResult)
{
	switch (luaBatchDispatchRecipientStopCondition(kind))
	{
	case LuaBatchRecipientStopCondition::FalseResult:
		return hasFunction && !boolResult;
	case LuaBatchRecipientStopCondition::TrueResult:
		return hasFunction && boolResult;
	case LuaBatchRecipientStopCondition::FunctionPresent:
		return hasFunction;
	case LuaBatchRecipientStopCondition::Never:
		return false;
	}
	return false;
}

[[nodiscard]] inline bool luaBatchDispatchStopsAfterRecipient(const LuaBatchDispatchKind    kind,
                                                              const LuaBatchDispatchResult &result)
{
	switch (luaBatchDispatchRecipientStopCondition(kind))
	{
	case LuaBatchRecipientStopCondition::FalseResult:
		return result.boolResultValid && !result.boolResult;
	case LuaBatchRecipientStopCondition::TrueResult:
		return result.boolResultValid && result.boolResult;
	case LuaBatchRecipientStopCondition::FunctionPresent:
		return result.hasFunctionValid && result.hasFunction;
	case LuaBatchRecipientStopCondition::Never:
		return false;
	}
	return false;
}

inline void mergeLuaBatchPresentationRefreshFlags(LuaBatchDispatchResult       &aggregate,
                                                  const LuaBatchDispatchResult &result)
{
	aggregate.linePresentationRequiresRefresh |= result.linePresentationRequiresRefresh;
	aggregate.outputScrollPositionRequiresRefresh |= result.outputScrollPositionRequiresRefresh;
	aggregate.outputScrollPositionChanged |= result.outputScrollPositionChanged;
	aggregate.commandUiPresentationRequiresRefresh |= result.commandUiPresentationRequiresRefresh;
	aggregate.globalPresentationRequiresRefresh |= result.globalPresentationRequiresRefresh;
	aggregate.commandHistoryChanged |= result.commandHistoryChanged;
	if (result.notepadPresentationChanged)
	{
		aggregate.notepadPresentationChanged     = true;
		aggregate.hasNotepadPresentationSnapshot = result.hasNotepadPresentationSnapshot;
		aggregate.notepadPresentationSnapshot    = result.hasNotepadPresentationSnapshot
		                                               ? result.notepadPresentationSnapshot
		                                               : QVector<LuaCallbackNotepadSnapshot>{};
	}
}

/**
 * @brief Merges one completed recipient into a batch aggregate.
 * @param kind Original batch dispatch kind.
 * @param aggregate Aggregate to update.
 * @param recipient Completed recipient result.
 */
inline void mergeLuaBatchRecipientResult(const LuaBatchDispatchKind kind, LuaBatchDispatchResult &aggregate,
                                         const LuaBatchDispatchResult &recipient)
{
	mergeLuaBatchPresentationRefreshFlags(aggregate, recipient);
	switch (luaBatchDispatchAggregateShape(kind))
	{
	case LuaBatchAggregateShape::BooleanAndFunction:
		aggregate.boolResult      = aggregate.boolResult && recipient.boolResult;
		aggregate.boolResultValid = true;
		aggregate.hasFunction =
		    aggregate.hasFunction || (recipient.hasFunctionValid && recipient.hasFunction);
		aggregate.hasFunctionValid = true;
		break;
	case LuaBatchAggregateShape::StopOnFalse:
		if (recipient.hasFunctionValid && recipient.hasFunction && recipient.boolResultValid &&
		    !recipient.boolResult)
			aggregate.boolResult = false;
		aggregate.boolResultValid = true;
		aggregate.hasFunction =
		    aggregate.hasFunction || (recipient.hasFunctionValid && recipient.hasFunction);
		aggregate.hasFunctionValid = true;
		break;
	case LuaBatchAggregateShape::FunctionHandled:
		if (recipient.hasFunctionValid && recipient.hasFunction)
			aggregate.boolResult = true;
		aggregate.boolResultValid = true;
		aggregate.hasFunction =
		    aggregate.hasFunction || (recipient.hasFunctionValid && recipient.hasFunction);
		aggregate.hasFunctionValid = true;
		break;
	case LuaBatchAggregateShape::StopOnTrue:
		if (recipient.hasFunctionValid && recipient.hasFunction && recipient.boolResultValid &&
		    recipient.boolResult)
			aggregate.boolResult = true;
		aggregate.boolResultValid = true;
		aggregate.hasFunction =
		    aggregate.hasFunction || (recipient.hasFunctionValid && recipient.hasFunction);
		aggregate.hasFunctionValid = true;
		break;
	case LuaBatchAggregateShape::FunctionCount:
		if (recipient.countResultValid)
			aggregate.countResult += recipient.countResult;
		else if (recipient.hasFunctionValid && recipient.hasFunction)
			++aggregate.countResult;
		aggregate.countResultValid = true;
		break;
	case LuaBatchAggregateShape::FunctionAny:
		aggregate.hasFunction =
		    aggregate.hasFunction || (recipient.hasFunctionValid && recipient.hasFunction);
		aggregate.hasFunctionValid = true;
		break;
	case LuaBatchAggregateShape::BytesInOut:
		aggregate.bytesResult = recipient.bytesResult;
		break;
	case LuaBatchAggregateShape::StringInOut:
		aggregate.stringResult = recipient.stringResult;
		break;
	case LuaBatchAggregateShape::LastResult:
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
	case LuaBatchAggregateShape::None:
		break;
	}
}

/**
 * @brief Preserves results produced before a suspended recipient when dispatch cannot resume.
 * @param kind Original multi-recipient dispatch shape.
 * @param fallback Request-level fallback to update.
 * @param partial Aggregate produced before the suspended recipient completed.
 */
inline void preserveLuaBatchPartialResultOnFallback(const LuaBatchDispatchKind    kind,
                                                    LuaBatchDispatchResult       &fallback,
                                                    const LuaBatchDispatchResult &partial)
{
	mergeLuaBatchPresentationRefreshFlags(fallback, partial);
	switch (luaBatchDispatchAggregateShape(kind))
	{
	case LuaBatchAggregateShape::BooleanAndFunction:
	case LuaBatchAggregateShape::StopOnFalse:
	case LuaBatchAggregateShape::StopOnTrue:
	case LuaBatchAggregateShape::FunctionHandled:
		fallback.boolResult       = partial.boolResult;
		fallback.boolResultValid  = true;
		fallback.hasFunction      = partial.hasFunction;
		fallback.hasFunctionValid = true;
		break;
	case LuaBatchAggregateShape::FunctionCount:
		fallback.countResult      = partial.countResult;
		fallback.countResultValid = true;
		break;
	case LuaBatchAggregateShape::FunctionAny:
		fallback.hasFunction      = partial.hasFunction;
		fallback.hasFunctionValid = true;
		break;
	case LuaBatchAggregateShape::BytesInOut:
		fallback.bytesResult = partial.bytesResult;
		break;
	case LuaBatchAggregateShape::StringInOut:
		fallback.stringResult = partial.stringResult;
		break;
	case LuaBatchAggregateShape::LastResult:
	case LuaBatchAggregateShape::None:
		break;
	}
}

[[nodiscard]] inline bool luaBatchPresentationRequiresRefresh(const LuaBatchDispatchResult &result)
{
	return result.linePresentationRequiresRefresh || result.outputScrollPositionRequiresRefresh ||
	       result.outputScrollPositionChanged || result.commandUiPresentationRequiresRefresh ||
	       result.globalPresentationRequiresRefresh || result.commandHistoryChanged ||
	       result.notepadPresentationChanged;
}

namespace QMudLuaDeferredRuntimeMutation
{
	/**
	 * @brief Applies ordered callback cleanup batches on their owning runtime threads.
	 * @param batches Batches to consume.
	 */
	void apply(QVector<LuaDeferredRuntimeMutationBatch> batches);
	/**
	 * @brief Applies local batches immediately and queues foreign-thread batches without waiting.
	 * @param batches Batches to consume.
	 */
	void applyLocallyOrQueue(QVector<LuaDeferredRuntimeMutationBatch> batches);
	/**
	 * @brief Takes and applies every deferred cleanup batch carried by a dispatch result.
	 * @param result Dispatch result whose cleanup batches are consumed.
	 */
	void apply(LuaBatchDispatchResult &result);
} // namespace QMudLuaDeferredRuntimeMutation

/**
 * @brief Engine bootstrap payload used for batched plugin Lua initialization.
 */
struct LuaEngineObservedInitializationRequest
{
		LuaCallbackEngine                *engine{nullptr};
		QSharedPointer<LuaCallbackEngine> workerLifetimeOwner;
		WorldRuntime                     *runtime{nullptr};
		QString                           scriptText;
		QString                           pluginId;
		QString                           pluginName;
		QString                           pluginDirectory;
		QSet<QString>                     callbackNames;
		std::function<void(const QString &, const QSet<QString> &, const QSet<QString> &)> observer;
};

/**
 * @brief Execution seam for invoking Lua callback engine operations.
 *
 * Runtime configuration selects either the worker-thread or same-thread direct
 * backend while preserving identical call semantics through this interface.
 */
class ILuaExecutor
{
	public:
		virtual ~ILuaExecutor() = default;

		/**
		 * @brief Dispatches one structured batch callback command.
		 * @param request Command payload and arguments.
		 * @return Dispatch result payload.
		 */
		[[nodiscard]] virtual LuaBatchDispatchResult
		dispatchBatch(const LuaBatchDispatchRequest &request) const;
		/**
		 * @brief Dispatches one structured batch callback command asynchronously with completion callback.
		 * @param request Command payload and arguments.
		 * @param completionTarget QObject owning thread for completion delivery; may be null.
		 * @param completion Completion callback receiving dispatch result payload.
		 */
		virtual void
		dispatchBatchAsync(const LuaBatchDispatchRequest &request, QObject *completionTarget,
		                   const std::function<void(const LuaBatchDispatchResult &)> &completion) const;
};

/**
 * @brief Same-thread/direct execution backend that preserves current behavior.
 */
class LuaExecutorDirect final : public ILuaExecutor
{
};

/**
 * @brief Creates the runtime Lua executor backend.
 *
 * Default builds return the worker-thread backend. Builds configured with
 * `QMUD_ENABLE_EXPERIMENTAL_THREADED_LUA_EXECUTOR=OFF` return the direct backend.
 */
std::unique_ptr<ILuaExecutor> makeLuaExecutor(LuaDeferredRuntimeMutationConsumer shutdownMutationConsumer);

#endif // QMUD_LUAEXECUTOR_H
