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

class LuaCallbackEngine;
class WorldRuntime;
class QObject;
struct LuaStyleRun;
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
		WorldRuntime *runtime{nullptr};
		QString       id;
		QString       name;
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
};

/**
 * @brief Style span payload used by callback-scope output-buffer snapshots.
 */
struct LuaCallbackLineStyleSnapshot
{
		int     length{0};
		long    fore{0};
		long    back{0};
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
		int                                      lineBufferCount{0};
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
	CountAndRecentText,
	CountAndRecent,
	Full
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

		QStringList                                windowNames;
		QHash<QString, QStringList>                fontIdsByWindow;
		QHash<QString, QStringList>                imageIdsByWindow;
		QHash<QString, QStringList>                hotspotIdsByWindow;
		QHash<QString, bool>                       imageHasAlphaByKey;
		QHash<QString, WindowInfoSnapshot>         windowInfoByWindow;
		QHash<QString, QSharedPointer<MiniWindow>> miniWindowsByWindow;
		bool                                       miniWindowLookupCacheValid{false};
		QSet<QString>                              normalizedMiniWindowIds;
		QSet<QString>                              normalizedMiniWindowFontKeys;
		QSet<QString>                              normalizedMiniWindowImageKeys;
		QSet<QString>                              normalizedMiniWindowHotspotKeys;
		void                                      *framePointer{nullptr};
		bool                                       hasFramePointer{false};
		bool                                       hasCommandUiSnapshot{false};
		bool                                       commandUiHasView{false};
		bool                                       commandUiHasFrameData{false};
		int                                        commandUiOutputClientHeight{0};
		int                                        commandUiOutputClientWidth{0};
		int                                        commandUiViewHeight{0};
		int                                        commandUiViewWidth{0};
		bool                                       hasRuntimeCountersSnapshot{false};
		int                                        runtimeOutputFontHeight{0};
		int                                        runtimeOutputFontWidth{0};
		void                                       rebuildMiniWindowLookupCaches()
		{
			const auto normalized = [](const QString &value) { return value.trimmed().toLower(); };
			const auto itemKey    = [](const QString &windowKey, const QString &itemId)
			{ return windowKey + QLatin1Char('|') + itemId; };
			auto addWindowKey = [this, &normalized](const QString &windowName)
			{
				const QString windowKey = normalized(windowName);
				if (!windowKey.isEmpty())
					normalizedMiniWindowIds.insert(windowKey);
				return windowKey;
			};
			auto addItemKeys =
			    [&addWindowKey, &normalized, &itemKey](const QHash<QString, QStringList> &itemsByWindow,
			                                           QSet<QString>                     &destination)
			{
				for (auto it = itemsByWindow.constBegin(); it != itemsByWindow.constEnd(); ++it)
				{
					const QString windowKey = addWindowKey(it.key());
					if (windowKey.isEmpty())
						continue;
					for (const QString &item : it.value())
					{
						const QString itemId = normalized(item);
						if (!itemId.isEmpty())
							destination.insert(itemKey(windowKey, itemId));
					}
				}
			};

			normalizedMiniWindowIds.clear();
			normalizedMiniWindowFontKeys.clear();
			normalizedMiniWindowImageKeys.clear();
			normalizedMiniWindowHotspotKeys.clear();

			for (const QString &windowName : windowNames)
				addWindowKey(windowName);
			for (auto it = miniWindowsByWindow.constBegin(); it != miniWindowsByWindow.constEnd(); ++it)
				addWindowKey(it.key());
			for (auto it = windowInfoByWindow.constBegin(); it != windowInfoByWindow.constEnd(); ++it)
				addWindowKey(it.key());

			addItemKeys(fontIdsByWindow, normalizedMiniWindowFontKeys);
			addItemKeys(imageIdsByWindow, normalizedMiniWindowImageKeys);
			addItemKeys(hotspotIdsByWindow, normalizedMiniWindowHotspotKeys);
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
		bool                                                  hasLineBufferDeltaSnapshot{false};
		bool                                                  hasLineBufferCountDelta{false};
		int                                                   lineBufferDeltaCount{0};
		QHash<int, LuaCallbackLineEntrySnapshot>              lineEntryDeltasByBufferIndex;
		QSet<int>                                             missingLineEntryDeltasByBufferIndex;
		bool                                                  hasRecentLinesSnapshot{false};
		QStringList                                           recentLinesSnapshot;
		bool                                                  hasDatabaseListSnapshot{false};
		bool                                                  databaseListSnapshotDirty{false};
		QStringList                                           databaseNamesSnapshot;
		bool                                                  hasDatabaseSnapshot{false};
		QHash<QString, LuaCallbackDatabaseSnapshot>           databaseSnapshotsByName;
		QHash<QString, int>                                   databaseColumnsByName;
		QHash<QString, QString>                               databaseErrorsByName;
		QHash<QString, QString>                               databaseColumnNamesByKey;
		QSet<QString>                                         missingDatabaseColumnNameKeys;
		QHash<QString, QString>                               databaseColumnTextByKey;
		QSet<QString>                                         missingDatabaseColumnTextKeys;
		QHash<QString, QVariant>                              databaseColumnValuesByKey;
		QSet<QString>                                         missingDatabaseColumnValueKeys;
		QHash<QString, int>                                   databaseColumnTypesByKey;
		QHash<QString, QVariant>                              databaseInfoByKey;
		QSet<QString>                                         missingDatabaseInfoKeys;
		QHash<QString, QStringList>                           databaseColumnNamesByName;
		QSet<QString>                                         missingDatabaseColumnNamesByName;
		QHash<QString, QVector<QVariant>>                     databaseColumnValuesByName;
		QSet<QString>                                         missingDatabaseColumnValuesByName;
		QHash<QString, int>                                   databaseTotalChangesByName;
		QHash<QString, int>                                   databaseChangesByName;
		QHash<QString, QString>                               databaseLastInsertRowidByName;
		bool                                                  hasMacroEntriesSnapshot{false};
		QList<LuaCallbackAttributeChildrenSnapshot>           macroEntriesSnapshot;
		bool                                                  hasVariableEntriesSnapshot{false};
		QList<LuaCallbackAttributeContentSnapshot>            variableEntriesSnapshot;
		bool                                                  hasKeypadEntriesSnapshot{false};
		QList<LuaCallbackAttributeContentSnapshot>            keypadEntriesSnapshot;
		bool                                                  hasAcceleratorSnapshot{false};
		QVector<LuaCallbackAcceleratorSnapshot>               acceleratorSnapshot;
		QVariantHash                                          commandUiValues;
		QVariantHash                                          runtimeCounterValues;
		QHash<QString, QString>                               pluginNamesById;
		QHash<QString, QString>                               pluginDirectoriesById;
		QHash<QString, bool>                                  pluginEnabledById;
		QHash<QString, QSharedPointer<LuaCallbackEngine>>     pluginEnginesById;
		QHash<QString, QSet<QString>>                         pluginLuaFunctionsById;
		QStringList                                           broadcastPluginIdsSnapshot;
		QVector<QSharedPointer<LuaCallbackEngine>>            broadcastPluginEnginesSnapshot;
		bool                                                  hasBroadcastPluginSnapshot{false};
		QStringList                                           pluginIdsSnapshot;
		QHash<QString, QString>                               pluginIdsByLookupKey;
		QHash<QString, QHash<int, QVariant>>                  pluginInfoValuesById;
		QHash<QString, bool>                                  pluginCallbackPresenceByName;
		bool                                                  hasEntitySnapshot{false};
		QHash<QString, QString>                               entityValuesByName;
		bool                                                  hasUiSnapshot{false};
		QVariantMap                                           guiSystemValues;
		bool                                                  hasClipboardText{false};
		QString                                               clipboardText;
		QHash<int, QRect>                                     mainWindowPositionsByMode;
		bool                                                  mainWindowPositionsDirty{false};
		QHash<QString, QRect>                                 worldWindowPositionsByKey;
		QSet<QString>                                         missingWorldWindowPositionKeys;
		QSet<int>                                             dirtyWorldWindowPositionOrdinals;
		QHash<QString, QRect>                                 notepadWindowPositionsByKey;
		QSet<QString>                                         missingNotepadWindowPositionKeys;
		QSet<QString>                                         dirtyNotepadWindowPositionKeys;
		QSet<QString>                                         dirtyNotepadDocumentKeys;
		QHash<QString, int>                                   notepadLengthByKey;
		QSet<QString>                                         missingNotepadLengthKeys;
		QHash<QString, QString>                               notepadTextByKey;
		QSet<QString>                                         missingNotepadTextKeys;
		QSet<QString>                                         dirtyNotepadListKeys;
		QHash<QString, QStringList>                           notepadListByKey;
		QSet<QString>                                         missingNotepadListKeys;
		QVector<LuaCallbackWorldRuntimeSnapshot>              worldRuntimeSnapshot;
		QVector<LuaCallbackWorldWindowPositionSnapshot>       worldWindowPositionSnapshot;
		QVector<LuaCallbackNotepadSnapshot>                   notepadSnapshot;
		int                                                   actionSourceOverride{0};
		bool                                                  hasActionSourceOverride{false};
};

/**
 * @brief Batch callback dispatch request payload.
 */
struct LuaBatchDispatchRequest
{
		LuaBatchDispatchKind                       kind{LuaBatchDispatchKind::NoArgs};
		LuaBatchDispatchLane                       lane{LuaBatchDispatchLane::Callback};
		LuaCallbackLineSnapshotPolicy              lineSnapshotPolicy{LuaCallbackLineSnapshotPolicy::None};
		QVector<QSharedPointer<LuaCallbackEngine>> engines;
		QString                                    functionName;
		QString                                    stringArg;
		QString                                    stringArg2;
		QString                                    miniWindowExecutionName;
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
		WorldRuntime                  *runtime{nullptr};
		QVector<std::function<void()>> mutations;
};

/**
 * @brief Pending string-result modal request produced by a yielded Lua callback.
 */
struct LuaPendingModalStringRequest
{
		std::function<QString()>                             guiCallable;
		std::function<void(WorldRuntime &, const QString &)> beforeRuntimeResumeCallback;
		std::function<void(const QString &)>                 beforeResumeCallback;
		std::function<void(quint64, QString)>                resultCallback;
};

/**
 * @brief Batch callback dispatch result payload.
 */
struct LuaBatchDispatchResult
{
		bool                                     boolResult{false};
		bool                                     boolResultValid{false};
		bool                                     hasFunction{false};
		bool                                     hasFunctionValid{false};
		int                                      countResult{0};
		bool                                     countResultValid{false};
		QString                                  stringResult;
		QByteArray                               bytesResult;
		int                                      marshallingError{0};
		bool                                     marshallingErrorValid{false};
		int                                      marshallingIndex{0};
		QByteArray                               marshallingTypeName;
		QString                                  marshallingRuntimeError;
		int                                      marshallingReturnCount{0};
		bool                                     marshallingSameState{false};
		bool                                     suspended{false};
		quint64                                  modalResumeId{0};
		int                                      suspendedEngineIndex{-1};
		bool                                     hasPendingModalStringRequest{false};
		LuaPendingModalStringRequest             pendingModalStringRequest;
		QVector<LuaDeferredRuntimeMutationBatch> deferredRuntimeMutationBatches;
};

/**
 * @brief Engine bootstrap payload used for batched plugin Lua initialization.
 */
struct LuaEngineObservedInitializationRequest
{
		LuaCallbackEngine                                                                 *engine{nullptr};
		WorldRuntime                                                                      *runtime{nullptr};
		QString                                                                            scriptText;
		QString                                                                            pluginId;
		QString                                                                            pluginName;
		QString                                                                            pluginDirectory;
		QSet<QString>                                                                      callbackNames;
		std::function<void(const QString &, const QSet<QString> &, const QSet<QString> &)> observer;
};

/**
 * @brief Execution seam for invoking Lua callback engine operations.
 *
 * Production currently uses @ref LuaExecutorDirect (same-thread direct dispatch),
 * while future threaded backends can implement this interface with identical
 * call semantics.
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
		 * @brief Dispatches one structured batch callback command asynchronously.
		 * @param request Command payload and arguments.
		 */
		virtual void dispatchBatchAsync(const LuaBatchDispatchRequest &request) const;
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
 * Currently, returns the direct/same-thread backend; future threading work will
 * switch this factory based on configuration once parity gates are satisfied.
 */
std::unique_ptr<ILuaExecutor> makeLuaExecutor();

#endif // QMUD_LUAEXECUTOR_H
