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
#include <QFlags>
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
#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
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
 * @brief Runtime-maintained placement state for callback output delayed past its capture point.
 *
 * Queued and suspended callbacks promote an output anchor into this state. A callback that already owns an output
 * stream aliases the state stored in that stream, avoiding another allocation when a nested callback or suspension
 * needs the same positional updates. Middle edits update the state through the cursor registry, while head eviction
 * advances one shared counter from which every surviving state derives its current index. Callback dispatch therefore
 * never has to search the output buffer, and ordinary capped-buffer appends never walk the cursor registry.
 */
struct LuaCallbackOutputAnchorState
{
		struct Presentation
		{
				int  bufferIndex{0};
				bool activeIncoming{false};
				bool hidden{false};
		};

		explicit LuaCallbackOutputAnchorState(
		    const int initialBufferIndex = 0, const bool initiallyActiveIncoming = false,
		    const bool initiallyHidden = false, const quint64 promotedOutputStreamId = 0,
		    std::shared_ptr<const std::atomic<quint64>> headRemovalCount = {}) noexcept
		    : outputStreamId(promotedOutputStreamId), m_headRemovalCount(std::move(headRemovalCount)),
		      m_presentation(packPresentation(shiftedBufferIndex(initialBufferIndex), initialBufferIndex > 0,
		                                      initiallyActiveIncoming, initiallyHidden))
		{
		}

		[[nodiscard]] Presentation
		presentation(const std::memory_order order = std::memory_order_relaxed) const noexcept
		{
			return unpackPresentation(m_presentation.load(order), currentHeadRemovalCount());
		}

		void storePresentation(const int bufferIndex, const bool activeIncoming, const bool hidden,
		                       const std::memory_order order = std::memory_order_relaxed) noexcept
		{
			m_presentation.store(
			    packPresentation(shiftedBufferIndex(bufferIndex), bufferIndex > 0, activeIncoming, hidden),
			    order);
		}

		void storeBufferIndex(const int               bufferIndex,
		                      const std::memory_order order = std::memory_order_relaxed) noexcept
		{
			quint64 current = m_presentation.load(std::memory_order_relaxed);
			quint64 desired = 0;
			do
			{
				desired = (current & kPresentationStyleFlagsMask) | shiftedBufferIndex(bufferIndex) |
				          (bufferIndex > 0 ? kBufferIndexValidMask : quint64{0});
			} while (
			    !m_presentation.compare_exchange_weak(current, desired, order, std::memory_order_relaxed));
		}

		void clearPresentation(const std::memory_order order = std::memory_order_relaxed) noexcept
		{
			m_presentation.store(0, order);
		}

		/**
		 * @brief Attaches the runtime's cumulative head-removal counter before the state becomes observable again.
		 * @param headRemovalCount Counter shared by every promoted anchor owned by one runtime.
		 */
		void bindHeadRemovalCount(std::shared_ptr<const std::atomic<quint64>> headRemovalCount) noexcept
		{
			if (!headRemovalCount || m_headRemovalCount == headRemovalCount)
				return;
			Q_ASSERT(!m_headRemovalCount);
			if (m_headRemovalCount)
				return;
			const Presentation current = presentation();
			m_headRemovalCount         = std::move(headRemovalCount);
			storePresentation(current.bufferIndex, current.activeIncoming, current.hidden);
		}

		std::atomic<quint64> outputStreamId;

	private:
		static constexpr quint64 kBufferIndexMask            = (quint64{1} << 61) - 1;
		static constexpr quint64 kActiveIncomingMask         = quint64{1} << 61;
		static constexpr quint64 kHiddenMask                 = quint64{1} << 62;
		static constexpr quint64 kBufferIndexValidMask       = quint64{1} << 63;
		static constexpr quint64 kPresentationStyleFlagsMask = kActiveIncomingMask | kHiddenMask;

		[[nodiscard]] quint64    currentHeadRemovalCount() const noexcept
		{
			return m_headRemovalCount ? m_headRemovalCount->load(std::memory_order_relaxed) : 0;
		}

		[[nodiscard]] quint64 shiftedBufferIndex(const int bufferIndex) const noexcept
		{
			if (bufferIndex <= 0)
				return 0;
			return (currentHeadRemovalCount() + static_cast<quint64>(bufferIndex)) & kBufferIndexMask;
		}

		[[nodiscard]] static constexpr quint64 packPresentation(const quint64 shiftedIndex, const bool valid,
		                                                        const bool activeIncoming,
		                                                        const bool hidden) noexcept
		{
			return shiftedIndex | (valid ? kBufferIndexValidMask : quint64{0}) |
			       (activeIncoming ? kActiveIncomingMask : quint64{0}) | (hidden ? kHiddenMask : quint64{0});
		}

		[[nodiscard]] static constexpr Presentation
		unpackPresentation(const quint64 value, const quint64 headRemovalCount) noexcept
		{
			const quint64 shiftedIndex = value & kBufferIndexMask;
			const quint64 bufferIndex =
			    (shiftedIndex - (headRemovalCount & kBufferIndexMask)) & kBufferIndexMask;
			const int resolvedBufferIndex =
			    (value & kBufferIndexValidMask) != 0 && bufferIndex <= std::numeric_limits<int>::max()
			        ? static_cast<int>(bufferIndex)
			        : 0;
			return {resolvedBufferIndex, (value & kActiveIncomingMask) != 0, (value & kHiddenMask) != 0};
		}

		std::shared_ptr<const std::atomic<quint64>> m_headRemovalCount;
		std::atomic<quint64>                        m_presentation;
};

/**
 * @brief Immutable identity of one plugin-owned asynchronous API request.
 *
 * The plugin id locates the recipient while `engineInstanceId` proves that the recipient is still the exact Lua
 * engine that initiated the work. Keeping these values in one object prevents an id-only completion path from
 * delivering a pre-reload result to a same-id replacement.
 */
struct LuaPluginAsyncResultRequest
{
		QString            pluginId;
		quint64            engineInstanceId{0};
		quint64            requestId{0};

		[[nodiscard]] bool isValid() const noexcept
		{
			return !pluginId.isEmpty() && engineInstanceId != 0 && requestId != 0;
		}
};

/**
 * @brief Attribute/children payload used by callback-scope runtime snapshots.
 */
struct LuaCallbackAttributeChildrenSnapshot
{
		QMap<QString, QString> attributes;
		QMap<QString, QString> children;
		bool                   operator==(const LuaCallbackAttributeChildrenSnapshot &) const = default;
};

/**
 * @brief Attribute/content payload used by callback-scope runtime snapshots.
 */
struct LuaCallbackAttributeContentSnapshot
{
		QMap<QString, QString> attributes;
		QString                content;
		bool                   operator==(const LuaCallbackAttributeContentSnapshot &) const = default;
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
		int                    matchCount{0};
		int                    matchAttempts{0};
		qint64                 executionTimeNs{0};
		QString                lastMatchTarget;
		QStringList            lastMatchWildcards;
		QDateTime              lastMatched;
		quint64                runtimeId{0};
		int                    executingScriptDepth{0};
		bool                   executingScript{false};
		bool                   operator==(const LuaCallbackTriggerSnapshot &) const = default;
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
		int                    matchCount{0};
		int                    matchAttempts{0};
		qint64                 executionTimeNs{0};
		QString                lastMatchTarget;
		QStringList            lastMatchWildcards;
		QDateTime              lastMatched;
		quint64                runtimeId{0};
		int                    executingScriptDepth{0};
		bool                   executingScript{false};
		bool                   operator==(const LuaCallbackAliasSnapshot &) const = default;
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
		bool                   operator==(const LuaCallbackTimerSnapshot &) const = default;
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
		bool    operator==(const LuaCallbackAcceleratorSnapshot &) const = default;
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
		bool              operator==(const LuaCallbackDatabaseSnapshot &) const = default;
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
 * @brief How a suspended dispatch is completed after its current recipient resumes.
 *
 * This is dispatch semantics, not WorldRuntime policy. `ResultOnly` returns the resumed operation as-is,
 * `AggregateSingleRecipient` merges it into the pre-yield result and finishes, and
 * `AggregateRemainingRecipients` additionally continues through the request's later recipients.
 */
enum class LuaBatchResumeContinuationShape
{
	ResultOnly,
	AggregateSingleRecipient,
	AggregateRemainingRecipients
};

/**
 * @brief Extra neutral-result contract not derivable from recipient aggregation.
 */
enum class LuaBatchFallbackShape
{
	AggregateNeutral,
	HasFunctionFalse,
	BoolFalse,
	BoolAndFunctionFalse
};

/**
 * @brief Whether a queued callback dispatch requires a Lua function name.
 */
enum class LuaBatchFunctionNamePolicy
{
	Required,
	NotRequired
};

/**
 * @brief Authoritative registry for all batch-kind dispatch policies.
 *
 * Adding a dispatch kind must classify aggregation, continuation, fallback, and function-name handling here.
 * The enum and every policy query are generated from this registry so no dispatch site can acquire a second
 * hand-maintained kind list.
 */
#define QMUD_LUA_BATCH_DISPATCH_KIND_REGISTRY(X)                                                             \
	X(NoArgs, BooleanAndFunction, AggregateRemainingRecipients, AggregateNeutral, Required)                  \
	X(HasFunction, None, ResultOnly, HasFunctionFalse, Required)                                             \
	X(String, LastResult, AggregateRemainingRecipients, AggregateNeutral, Required)                          \
	X(StringStopOnFalse, StopOnFalse, AggregateRemainingRecipients, AggregateNeutral, Required)              \
	X(StringHandled, FunctionHandled, AggregateRemainingRecipients, AggregateNeutral, Required)              \
	X(Bytes, LastResult, AggregateRemainingRecipients, AggregateNeutral, Required)                           \
	X(BytesInOut, BytesInOut, AggregateRemainingRecipients, AggregateNeutral, Required)                      \
	X(StringInOut, StringInOut, AggregateRemainingRecipients, AggregateNeutral, Required)                    \
	X(NumberAndStringStopOnTrue, StopOnTrue, AggregateRemainingRecipients, AggregateNeutral, Required)       \
	X(NumberAndStringStopOnFalse, StopOnFalse, AggregateRemainingRecipients, AggregateNeutral, Required)     \
	X(NumberAndString, LastResult, AggregateRemainingRecipients, AggregateNeutral, Required)                 \
	X(TwoNumbersAndStringStopOnFalse, StopOnFalse, AggregateRemainingRecipients, AggregateNeutral, Required) \
	X(TwoNumbersAndString, LastResult, AggregateRemainingRecipients, AggregateNeutral, Required)             \
	X(NumberAndBytesStopOnTrue, StopOnTrue, AggregateRemainingRecipients, AggregateNeutral, Required)        \
	X(NumberAndBytes, LastResult, AggregateRemainingRecipients, AggregateNeutral, Required)                  \
	X(NumberAndUtf8StringsCount, FunctionCount, AggregateRemainingRecipients, AggregateNeutral, Required)    \
	X(StringsAndWildcards, FunctionAny, AggregateRemainingRecipients, AggregateNeutral, Required)            \
	X(ExecuteScript, LastResult, AggregateSingleRecipient, BoolFalse, NotRequired)                           \
	X(ResetAndLoadScript, None, ResultOnly, BoolFalse, Required)                                             \
	X(InitializeEnginesWithObservedCallbacksMany, None, ResultOnly, AggregateNeutral, Required)              \
	X(UpdateObservedCallbacksMany, None, ResultOnly, AggregateNeutral, Required)                             \
	X(CallPluginLuaMarshalling, None, ResultOnly, BoolFalse, Required)                                       \
	X(TeardownEnginesMany, None, ResultOnly, AggregateNeutral, Required)                                     \
	X(ApplyPackageRestrictionsMany, None, ResultOnly, AggregateNeutral, Required)                            \
	X(ProcedureWithString, LastResult, AggregateSingleRecipient, BoolAndFunctionFalse, Required)             \
	X(MxpError, LastResult, AggregateSingleRecipient, BoolFalse, Required)                                   \
	X(MxpStartUp, None, AggregateRemainingRecipients, AggregateNeutral, Required)                            \
	X(MxpShutDown, None, AggregateRemainingRecipients, AggregateNeutral, Required)                           \
	X(MxpStartTag, LastResult, AggregateSingleRecipient, BoolFalse, Required)                                \
	X(MxpEndTag, None, AggregateRemainingRecipients, AggregateNeutral, Required)                             \
	X(MxpSetVariable, None, AggregateRemainingRecipients, AggregateNeutral, Required)                        \
	X(CancelSuspendedModalString, None, ResultOnly, AggregateNeutral, NotRequired)                           \
	X(ResumeSuspendedModalString, None, ResultOnly, AggregateNeutral, NotRequired)

/**
 * @brief Batch callback command kinds routed through the Lua executor.
 */
enum class LuaBatchDispatchKind
{
#define QMUD_DECLARE_LUA_BATCH_DISPATCH_KIND(name, aggregateShape, continuationShape, fallbackShape,         \
                                             functionNamePolicy)                                             \
	name,
	QMUD_LUA_BATCH_DISPATCH_KIND_REGISTRY(QMUD_DECLARE_LUA_BATCH_DISPATCH_KIND)
#undef QMUD_DECLARE_LUA_BATCH_DISPATCH_KIND
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
 * @brief Authoritative callback-event identity and input-priority registry.
 *
 * A routine name does not identify an event: aliases, timers, and triggers may call a function whose name also
 * happens to be a MUSHclient plugin callback name. Only the event-producing runtime path assigns one of these values.
 * The registry is the single source for both the standard plugin function name and input-critical classification.
 */
#define QMUD_LUA_CALLBACK_EVENT_REGISTRY(X)                                                                  \
	X(None, "", false)                                                                                       \
	X(TriggerRoutine, "", true)                                                                              \
	X(TriggerScript, "", true)                                                                               \
	X(AliasRoutine, "", true)                                                                                \
	X(AliasScript, "", true)                                                                                 \
	X(Hotspot, "", true)                                                                                     \
	X(AsyncResult, "OnPluginAsyncResult", false)                                                             \
	X(Broadcast, "OnPluginBroadcast", false)                                                                 \
	X(ChatAccept, "OnPluginChatAccept", false)                                                               \
	X(ChatDisplay, "OnPluginChatDisplay", false)                                                             \
	X(ChatMessage, "OnPluginChatMessage", false)                                                             \
	X(ChatMessageOut, "OnPluginChatMessageOut", false)                                                       \
	X(ChatNewUser, "OnPluginChatNewUser", false)                                                             \
	X(ChatUserDisconnect, "OnPluginChatUserDisconnect", false)                                               \
	X(Close, "OnPluginClose", true)                                                                          \
	X(Command, "OnPluginCommand", true)                                                                      \
	X(CommandChanged, "OnPluginCommandChanged", false)                                                       \
	X(CommandEntered, "OnPluginCommandEntered", true)                                                        \
	X(Connect, "OnPluginConnect", false)                                                                     \
	X(Disable, "OnPluginDisable", false)                                                                     \
	X(Disconnect, "OnPluginDisconnect", false)                                                               \
	X(DrawOutputWindow, "OnPluginDrawOutputWindow", false)                                                   \
	X(Enable, "OnPluginEnable", false)                                                                       \
	X(GetFocus, "OnPluginGetFocus", false)                                                                   \
	X(IacGa, "OnPluginIacGa", false)                                                                         \
	X(Install, "OnPluginInstall", false)                                                                     \
	X(LineReceived, "OnPluginLineReceived", true)                                                            \
	X(ListChanged, "OnPluginListChanged", false)                                                             \
	X(LoseFocus, "OnPluginLoseFocus", false)                                                                 \
	X(MouseMoved, "OnPluginMouseMoved", false)                                                               \
	X(MxpCloseTag, "OnPluginMXPcloseTag", true)                                                              \
	X(MxpError, "OnPluginMXPerror", false)                                                                   \
	X(MxpOpenTag, "OnPluginMXPopenTag", true)                                                                \
	X(MxpSetEntity, "OnPluginMXPsetEntity", true)                                                            \
	X(MxpSetVariable, "OnPluginMXPsetVariable", true)                                                        \
	X(MxpStart, "OnPluginMXPstart", false)                                                                   \
	X(MxpStop, "OnPluginMXPstop", false)                                                                     \
	X(PacketReceived, "OnPluginPacketReceived", true)                                                        \
	X(PartialLine, "OnPluginPartialLine", false)                                                             \
	X(PlaySound, "OnPluginPlaySound", false)                                                                 \
	X(SaveState, "OnPluginSaveState", true)                                                                  \
	X(ScreenDraw, "OnPluginScreendraw", false)                                                               \
	X(SelectionChanged, "OnPluginSelectionChanged", false)                                                   \
	X(Send, "OnPluginSend", true)                                                                            \
	X(Sent, "OnPluginSent", false)                                                                           \
	X(TabComplete, "OnPluginTabComplete", true)                                                              \
	X(TelnetOption, "OnPluginTelnetOption", true)                                                            \
	X(TelnetRequest, "OnPluginTelnetRequest", true)                                                          \
	X(TelnetSubnegotiation, "OnPluginTelnetSubnegotiation", true)                                            \
	X(Tick, "OnPluginTick", false)                                                                           \
	X(Trace, "OnPluginTrace", false)                                                                         \
	X(PacketDebug, "OnPluginPacketDebug", false)                                                             \
	X(WorldOutputResized, "OnPluginWorldOutputResized", false)                                               \
	X(WorldSave, "OnPluginWorldSave", false)

enum class LuaCallbackEvent : quint8
{
#define QMUD_DECLARE_LUA_CALLBACK_EVENT(name, functionName, inputCritical) name,
	QMUD_LUA_CALLBACK_EVENT_REGISTRY(QMUD_DECLARE_LUA_CALLBACK_EVENT)
#undef QMUD_DECLARE_LUA_CALLBACK_EVENT
	    Count
};

inline constexpr std::size_t kLuaCallbackEventCount = static_cast<std::size_t>(LuaCallbackEvent::Count);

inline constexpr std::array<bool, kLuaCallbackEventCount> kLuaCallbackEventInputCritical = {
#define QMUD_APPEND_LUA_CALLBACK_EVENT_INPUT_CRITICAL(name, functionName, inputCritical) inputCritical,
    QMUD_LUA_CALLBACK_EVENT_REGISTRY(QMUD_APPEND_LUA_CALLBACK_EVENT_INPUT_CRITICAL)
#undef QMUD_APPEND_LUA_CALLBACK_EVENT_INPUT_CRITICAL
};

inline const std::array<QString, kLuaCallbackEventCount> kLuaCallbackEventFunctionNames = {
#define QMUD_APPEND_LUA_CALLBACK_EVENT_FUNCTION_NAME(name, functionName, inputCritical)                      \
	QStringLiteral(functionName),
    QMUD_LUA_CALLBACK_EVENT_REGISTRY(QMUD_APPEND_LUA_CALLBACK_EVENT_FUNCTION_NAME)
#undef QMUD_APPEND_LUA_CALLBACK_EVENT_FUNCTION_NAME
};

[[nodiscard]] constexpr bool luaCallbackEventIsInputCritical(const LuaCallbackEvent event) noexcept
{
	const auto index = static_cast<std::size_t>(event);
	return index < kLuaCallbackEventInputCritical.size() && kLuaCallbackEventInputCritical[index];
}

[[nodiscard]] inline QString luaCallbackEventFunctionName(const LuaCallbackEvent event)
{
	const auto index = static_cast<std::size_t>(event);
	return index < kLuaCallbackEventFunctionNames.size() ? kLuaCallbackEventFunctionNames[index] : QString();
}

[[nodiscard]] inline const QStringList &luaPluginCallbackFunctionNames()
{
	static const QStringList names = []
	{
		QStringList result;
		result.reserve(static_cast<qsizetype>(kLuaCallbackEventFunctionNames.size()));
		for (const QString &name : kLuaCallbackEventFunctionNames)
		{
			if (!name.isEmpty())
				result.push_back(name);
		}
		return result;
	}();
	return names;
}

#undef QMUD_LUA_CALLBACK_EVENT_REGISTRY

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
 * @brief Immutable callback request view assembled from one runtime-owned stable base.
 *
 * WorldRuntime owns and patches a private base on its object thread. Every dispatch receives a
 * top-level copy; Qt containers stay shallow until either copy is written, so later base patches
 * detach and cannot alter an in-flight worker view. Stable fields are patched at their mutation
 * source. Dispatch-local fields are cleared from the cached base and populated after each clone.
 * Callback-local read-your-writes live in LuaCallbackEngine overlays rather than this shared
 * snapshot.
 *
 * Extension contract for a new callback-visible field:
 * 1. Classify it as stable, dispatch-local/asynchronously sampled, or callback-local.
 * 2. Put stable state in the domain-state type selected by LuaCallbackStableSnapshotDomain, or add a new
 *    entry to its single registry. The aggregate inherits every registered state, and causal replay verifies
 *    the complete selected state against the captured revision. Mark the entry plugin-indexed when plugin load/unload changes the
 *    field; a Plugins patch then expands mechanically, with no second dependent-domain list. Put cold
 *    construction and patch refresh in the same WorldRuntime population helper and handle the domain in
 *    populateLuaCallbackStableSnapshotDomains(), the sole domain-to-population registry used by both
 *    cold construction and patching; masks are derived mechanically from the enum. Call
 *    patchLuaCallbackStableSnapshot() from every authoritative mutation source. Plugin-scoped rule
 *    collections use the scoped overload of that same patch function, never a parallel registry.
 * 3. If Lua can mutate it, also journal the mutation and provide a LuaCallbackEngine overlay when
 *    the writing callback must read its own uncommitted value (which is 99% of the time). A field
 *    indexed by plugin id must also be handled by markCallbackPluginTopologyUnavailable() and
 *    reconcileCallbackPluginTopology(), the two ends of the single callback-local lifecycle overlay;
 *    do not add an API-specific unload/reload fanout. Plugin ids are canonicalized only by XML validation and
 *    plugin-facing Lua API argument decoders that actually accept a plugin id. Runtime service methods, metadata,
 *    journals, snapshots, overlays, and cache keys must copy the canonical id unchanged; they must not normalize
 *    or independently validate its spelling.
 * 4. For dispatch-local/sampled state, populate it only after cloning and clear it from the cached
 *    base in clearLuaCallbackDispatchVolatileSnapshot(); do not add an incidental stable-base patch.
 * 5. Cover cold construction, cache-hit visibility, immutability of an issued snapshot, and later
 *    recipient/nested-callback visibility with tests. A new field is incomplete until these paths
 *    agree.
 */
#define QMUD_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN_REGISTRY(X)                                                 \
	X(WorldVariables, false, false, LuaCallbackWorldVariablesSnapshotState)                                  \
	X(PluginVariables, true, true, LuaCallbackPluginVariablesSnapshotState)                                  \
	X(Triggers, true, true, LuaCallbackTriggersSnapshotState)                                                \
	X(Aliases, true, true, LuaCallbackAliasesSnapshotState)                                                  \
	X(Timers, true, true, LuaCallbackTimersSnapshotState)                                                    \
	X(WorldAttributes, false, false, LuaCallbackWorldAttributesSnapshotState)                                \
	X(Arrays, false, false, LuaCallbackArraysSnapshotState)                                                  \
	X(Colours, false, false, LuaCallbackColoursSnapshotState)                                                \
	X(Mapper, false, false, LuaCallbackMapperSnapshotState)                                                  \
	X(Databases, false, false, LuaCallbackDatabasesSnapshotState)                                            \
	X(Macros, false, false, LuaCallbackMacrosSnapshotState)                                                  \
	X(Keypad, false, false, LuaCallbackKeypadSnapshotState)                                                  \
	X(Accelerators, false, false, LuaCallbackAcceleratorsSnapshotState)                                      \
	X(Plugins, false, false, LuaCallbackPluginsSnapshotState)                                                \
	X(MiniWindows, false, false, LuaCallbackMiniWindowsSnapshotState)                                        \
	X(TriggerWildcards, true, false, LuaCallbackTriggerWildcardsSnapshotState)                               \
	X(AliasWildcards, true, false, LuaCallbackAliasWildcardsSnapshotState)

enum class LuaCallbackStableSnapshotDomain : quint8
{
#define QMUD_DECLARE_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN(name, pluginIndexed, pluginInfoCoupled, stateType)  \
	name,
	QMUD_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN_REGISTRY(QMUD_DECLARE_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN)
#undef QMUD_DECLARE_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN
	    Count
};

[[nodiscard]] constexpr quint32
luaCallbackStableSnapshotDomainBit(const LuaCallbackStableSnapshotDomain domain)
{
	return quint32{1} << static_cast<quint32>(domain);
}

[[nodiscard]] constexpr quint32 luaCallbackAllStableSnapshotDomainsMask()
{
	static_assert(static_cast<quint32>(LuaCallbackStableSnapshotDomain::Count) < 32U);
	return luaCallbackStableSnapshotDomainBit(LuaCallbackStableSnapshotDomain::Count) - 1U;
}

[[nodiscard]] constexpr quint32 luaCallbackPluginIndexedStableSnapshotDomainsMask()
{
	return 0U
#define QMUD_PLUGIN_INDEXED_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN(name, pluginIndexed, pluginInfoCoupled,      \
                                                                stateType)                                   \
	| (pluginIndexed ? luaCallbackStableSnapshotDomainBit(LuaCallbackStableSnapshotDomain::name) : 0U)
	    QMUD_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN_REGISTRY(
	        QMUD_PLUGIN_INDEXED_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN)
#undef QMUD_PLUGIN_INDEXED_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN
	        ;
}

[[nodiscard]] constexpr quint32 luaCallbackCausalStableSnapshotDomainMask(const quint32 domainMask)
{
	quint32 expandedMask = domainMask;
	if ((domainMask & luaCallbackStableSnapshotDomainBit(LuaCallbackStableSnapshotDomain::Plugins)) != 0U)
		expandedMask |= luaCallbackPluginIndexedStableSnapshotDomainsMask();
#define QMUD_PLUGIN_INFO_COUPLED_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN(name, pluginIndexed, pluginInfoCoupled, \
                                                                     stateType)                              \
	if (pluginInfoCoupled &&                                                                                 \
	    (domainMask & luaCallbackStableSnapshotDomainBit(LuaCallbackStableSnapshotDomain::name)) != 0U)      \
		expandedMask |= luaCallbackStableSnapshotDomainBit(LuaCallbackStableSnapshotDomain::Plugins);
	QMUD_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN_REGISTRY(
	    QMUD_PLUGIN_INFO_COUPLED_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN)
#undef QMUD_PLUGIN_INFO_COUPLED_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN
	return expandedMask;
}

struct LuaCallbackWorldVariablesSnapshotState
{
		bool                                       hasWorldVariablesSnapshot{false};
		QMap<QString, QString>                     worldVariablesSnapshot;
		bool                                       hasVariableEntriesSnapshot{false};
		QList<LuaCallbackAttributeContentSnapshot> variableEntriesSnapshot;
		bool operator==(const LuaCallbackWorldVariablesSnapshotState &) const = default;
};

struct LuaCallbackPluginVariablesSnapshotState
{
		QHash<QString, QMap<QString, QString>> pluginVariablesSnapshotById;
		QSet<QString>                          unavailablePluginVariableSnapshotIds;
		bool operator==(const LuaCallbackPluginVariablesSnapshotState &) const = default;
};

struct LuaCallbackTriggersSnapshotState
{
		QHash<QString, QList<LuaCallbackTriggerSnapshot>> triggerListsByPluginId;
		QSet<QString>                                     missingTriggerListPluginIds;
		bool operator==(const LuaCallbackTriggersSnapshotState &) const = default;
};

struct LuaCallbackAliasesSnapshotState
{
		QHash<QString, QList<LuaCallbackAliasSnapshot>> aliasListsByPluginId;
		QSet<QString>                                   missingAliasListPluginIds;
		bool operator==(const LuaCallbackAliasesSnapshotState &) const = default;
};

struct LuaCallbackTimersSnapshotState
{
		QHash<QString, QList<LuaCallbackTimerSnapshot>> timerListsByPluginId;
		QSet<QString>                                   missingTimerListPluginIds;
		bool operator==(const LuaCallbackTimersSnapshotState &) const = default;
};

struct LuaCallbackWorldAttributesSnapshotState
{
		bool                   hasWorldAttributeSnapshot{false};
		QMap<QString, QString> worldAttributesSnapshot;
		QMap<QString, QString> worldMultilineAttributesSnapshot;
		bool                   operator==(const LuaCallbackWorldAttributesSnapshotState &) const = default;
};

struct LuaCallbackArraysSnapshotState
{
		bool                                   hasArraySnapshot{false};
		QStringList                            arrayNamesSnapshot;
		QHash<QString, QMap<QString, QString>> arraysByName;
		bool operator==(const LuaCallbackArraysSnapshotState &) const = default;
};

struct LuaCallbackColoursSnapshotState
{
		QHash<int, long>    boldAnsiColoursByIndex;
		QHash<int, long>    normalAnsiColoursByIndex;
		QHash<int, long>    customTextColoursByIndex;
		QHash<int, long>    customBackgroundColoursByIndex;
		QHash<int, QString> customColourNamesByIndex;
		bool                operator==(const LuaCallbackColoursSnapshotState &) const = default;
};

struct LuaCallbackMapperSnapshotState
{
		bool             hasMapColourSnapshot{false};
		QMap<long, long> mapColourSnapshot;
		bool             hasMappingEntriesSnapshot{false};
		QStringList      mappingEntriesSnapshot;
		bool             operator==(const LuaCallbackMapperSnapshotState &) const = default;
};

struct LuaCallbackDatabasesSnapshotState
{
		bool                                        hasDatabaseListSnapshot{false};
		bool                                        databaseListSnapshotDirty{false};
		QStringList                                 databaseNamesSnapshot;
		bool                                        hasDatabaseSnapshot{false};
		QHash<QString, LuaCallbackDatabaseSnapshot> databaseSnapshotsByName;
		QHash<QString, int>                         databaseColumnsByName;
		QHash<QString, QString>                     databaseErrorsByName;
		QHash<QString, QString>                     databaseColumnNamesByKey;
		QSet<QString>                               missingDatabaseColumnNameKeys;
		QHash<QString, QString>                     databaseColumnTextByKey;
		QSet<QString>                               missingDatabaseColumnTextKeys;
		QHash<QString, QVariant>                    databaseColumnValuesByKey;
		QSet<QString>                               missingDatabaseColumnValueKeys;
		QHash<QString, int>                         databaseColumnTypesByKey;
		QHash<QString, QVariant>                    databaseInfoByKey;
		QSet<QString>                               missingDatabaseInfoKeys;
		QHash<QString, QStringList>                 databaseColumnNamesByName;
		QSet<QString>                               missingDatabaseColumnNamesByName;
		QHash<QString, QVector<QVariant>>           databaseColumnValuesByName;
		QSet<QString>                               missingDatabaseColumnValuesByName;
		QHash<QString, int>                         databaseTotalChangesByName;
		QHash<QString, int>                         databaseChangesByName;
		QHash<QString, QString>                     databaseLastInsertRowidByName;
		bool operator==(const LuaCallbackDatabasesSnapshotState &) const = default;
};

struct LuaCallbackMacrosSnapshotState
{
		bool                                        hasMacroEntriesSnapshot{false};
		QList<LuaCallbackAttributeChildrenSnapshot> macroEntriesSnapshot;
		bool operator==(const LuaCallbackMacrosSnapshotState &) const = default;
};

struct LuaCallbackKeypadSnapshotState
{
		bool                                       hasKeypadEntriesSnapshot{false};
		QList<LuaCallbackAttributeContentSnapshot> keypadEntriesSnapshot;
		bool operator==(const LuaCallbackKeypadSnapshotState &) const = default;
};

struct LuaCallbackAcceleratorsSnapshotState
{
		bool                                    hasAcceleratorSnapshot{false};
		QVector<LuaCallbackAcceleratorSnapshot> acceleratorSnapshot;
		bool operator==(const LuaCallbackAcceleratorsSnapshotState &) const = default;
};

struct LuaCallbackPluginsSnapshotState
{
		QHash<QString, QString>                           pluginNamesById;
		QHash<QString, QString>                           pluginDirectoriesById;
		QHash<QString, bool>                              pluginEnabledById;
		QHash<QString, bool>                              pluginInstallPendingById;
		QHash<QString, bool>                              pluginGlobalById;
		QHash<QString, bool>                              nativePluginSpeechEnabledById;
		QHash<QString, QSharedPointer<LuaCallbackEngine>> pluginEnginesById;
		QHash<QString, QSet<QString>>                     pluginLuaFunctionsById;
		QStringList                                       broadcastPluginIdsSnapshot;
		bool                                              hasBroadcastPluginSnapshot{false};
		QStringList                                       pluginIdsSnapshot;
		QHash<QString, QString>                           pluginIdsByLookupKey;
		QHash<QString, QHash<int, QVariant>>              pluginInfoValuesById;
		QHash<QString, bool>                              pluginCallbackPresenceByName;
		bool operator==(const LuaCallbackPluginsSnapshotState &) const = default;
};

/**
 * @brief One accepted callback-local mapper operation replayed into causally later snapshots.
 */
struct LuaCallbackMapperSnapshotOperation
{
		enum class Kind
		{
			Append,
			RemoveLast,
			Clear,
			SetEnabled
		};

		Kind    kind{Kind::Append};
		QString entry;
		bool    enabled{false};
};

/**
 * @brief Final persistent dirty-flag assignments made by one accepted causal callback mutation batch.
 */
struct LuaCallbackRuntimeCounterUpdates
{
		std::optional<bool> variablesChanged;
		std::optional<bool> worldFileModified;
};

/**
 * @brief Derived miniwindow snapshot indexes affected by one window mutation.
 *
 * Every mutation still replaces the immutable MiniWindow snapshot. These flags restrict rebuilding the
 * independent lookup containers used by WindowInfo and the font, image, and hotspot list/existence APIs.
 */
enum class LuaCallbackMiniWindowSnapshotChange : quint8
{
	WindowInfo        = 0x01,
	Fonts             = 0x02,
	Images            = 0x04,
	HotspotMembership = 0x08,
	Membership        = 0x10
};
Q_DECLARE_FLAGS(LuaCallbackMiniWindowSnapshotChanges, LuaCallbackMiniWindowSnapshotChange)
Q_DECLARE_OPERATORS_FOR_FLAGS(LuaCallbackMiniWindowSnapshotChanges)

[[nodiscard]] constexpr LuaCallbackMiniWindowSnapshotChanges
luaCallbackAllMiniWindowSnapshotChanges() noexcept
{
	using Change = LuaCallbackMiniWindowSnapshotChange;
	return Change::WindowInfo | Change::Fonts | Change::Images | Change::HotspotMembership |
	       Change::Membership;
}

struct LuaCallbackMiniWindowsSnapshotState
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
				bool      operator==(const WindowInfoSnapshot &) const = default;
		};

		QStringList                                      windowNames;
		QHash<QString, QStringList>                      fontIdsByWindow;
		QHash<QString, QStringList>                      imageIdsByWindow;
		QHash<QString, QStringList>                      hotspotIdsByWindow;
		QHash<LuaCallbackMiniWindowResourceKey, bool>    imageHasAlphaByKey;
		QHash<QString, WindowInfoSnapshot>               windowInfoByWindow;
		QHash<QString, QSharedPointer<const MiniWindow>> miniWindowsByWindow;
		bool                                             miniWindowLookupCacheValid{false};
		QSet<QString>                                    miniWindowIds;
		QSet<LuaCallbackMiniWindowResourceKey>           miniWindowFontKeys;
		QSet<LuaCallbackMiniWindowResourceKey>           miniWindowImageKeys;
		QSet<LuaCallbackMiniWindowResourceKey>           miniWindowHotspotKeys;

		void                                             rebuildMiniWindowLookupCaches()
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
		bool operator==(const LuaCallbackMiniWindowsSnapshotState &) const = default;
};

struct LuaCallbackTriggerWildcardsSnapshotState
{
		QMap<QString, QStringList>                            triggerWildcardsSnapshot;
		QMap<QString, QMap<QString, QString>>                 triggerNamedWildcardsSnapshot;
		QHash<QString, QMap<QString, QStringList>>            pluginTriggerWildcardsSnapshotById;
		QHash<QString, QMap<QString, QMap<QString, QString>>> pluginTriggerNamedWildcardsSnapshotById;
		bool operator==(const LuaCallbackTriggerWildcardsSnapshotState &) const = default;
};

struct LuaCallbackAliasWildcardsSnapshotState
{
		QMap<QString, QStringList>                            aliasWildcardsSnapshot;
		QMap<QString, QMap<QString, QString>>                 aliasNamedWildcardsSnapshot;
		QHash<QString, QMap<QString, QStringList>>            pluginAliasWildcardsSnapshotById;
		QHash<QString, QMap<QString, QMap<QString, QString>>> pluginAliasNamedWildcardsSnapshotById;
		bool operator==(const LuaCallbackAliasWildcardsSnapshotState &) const = default;
};

struct LuaCallbackStableSnapshotRoot
{
};

struct LuaCallbackStableSnapshotState
    : LuaCallbackStableSnapshotRoot
#define QMUD_INHERIT_LUA_CALLBACK_STABLE_SNAPSHOT_STATE(name, pluginIndexed, pluginInfoCoupled, stateType)   \
	, public stateType
      QMUD_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN_REGISTRY(QMUD_INHERIT_LUA_CALLBACK_STABLE_SNAPSHOT_STATE)
#undef QMUD_INHERIT_LUA_CALLBACK_STABLE_SNAPSHOT_STATE
{
};

[[nodiscard]] inline bool luaCallbackStableSnapshotDomainsEqual(const LuaCallbackStableSnapshotState &lhs,
                                                                const LuaCallbackStableSnapshotState &rhs,
                                                                const quint32 domainMask)
{
	bool equal = true;
#define QMUD_COMPARE_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN(name, pluginIndexed, pluginInfoCoupled, stateType)  \
	if ((domainMask & luaCallbackStableSnapshotDomainBit(LuaCallbackStableSnapshotDomain::name)) != 0U)      \
		equal = equal && static_cast<const stateType &>(lhs) == static_cast<const stateType &>(rhs);
	QMUD_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN_REGISTRY(QMUD_COMPARE_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN)
#undef QMUD_COMPARE_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN
	return equal;
}

#undef QMUD_LUA_CALLBACK_STABLE_SNAPSHOT_DOMAIN_REGISTRY
struct LuaCallbackSnapshot : LuaCallbackStableSnapshotState
{
		// Dispatch-local presentation and runtime counters.
		void                                               *framePointer{nullptr};
		bool                                                hasFramePointer{false};
		bool                                                hasCommandUiSnapshot{false};
		bool                                                hasCommandHistorySnapshot{false};
		QStringList                                         commandHistorySnapshot;
		bool                                                commandUiHasView{false};
		bool                                                commandUiHasFrameData{false};
		int                                                 commandUiOutputClientHeight{0};
		int                                                 commandUiOutputClientWidth{0};
		int                                                 commandUiViewHeight{0};
		int                                                 commandUiViewWidth{0};
		QString                                             activeMiniWindowExecutionName;
		/** MUSHclient WindowAddHotspot recursion guard inherited by this callback dispatch. */
		bool                                                windowAddHotspotReentryBlocked{false};
		QString                                             geometryConstrainedMiniWindowName;
		int                                                 geometryConstraintDisplayClientHeight{0};
		int                                                 geometryConstraintDisplayClientWidth{0};
		double                                              absoluteMiniWindowScaleXOver{1.0};
		double                                              absoluteMiniWindowScaleYOver{1.0};
		double                                              absoluteMiniWindowScaleXUnder{1.0};
		double                                              absoluteMiniWindowScaleYUnder{1.0};
		bool                                                hasRuntimeCountersSnapshot{false};
		int                                                 runtimeOutputFontHeight{0};
		int                                                 runtimeOutputFontWidth{0};
		// Dispatch-sampled external/asynchronous domains.
		bool                                                hasChatSnapshot{false};
		QList<long>                                         chatConnectionIdsSnapshot;
		QHash<long, QHash<int, QVariant>>                   chatInfoValuesById;
		QHash<long, QHash<QString, QVariant>>               chatOptionValuesById;
		QHash<QString, long>                                chatIdsByLookupKey;
		// Dispatch-local parser/render state.
		bool                                                hasWindowOutputTextRenderSnapshot{false};
		QMudAnsiStreamState                                 windowOutputTextAnsiStreamState;
		LuaCallbackAnsiRenderStateSnapshot                  windowOutputTextAnsiRenderState;
		LuaCallbackMxpStyleStateSnapshot                    windowOutputTextMxpStyleState;
		QVector<LuaCallbackMxpStyleFrameSnapshot>           windowOutputTextMxpStyleStack;
		QVector<QByteArray>                                 windowOutputTextMxpBlockStack;
		bool                                                windowOutputTextMxpLinkOpen{false};
		int                                                 windowOutputTextMxpPreDepth{0};
		QVector<LuaCallbackMxpCustomElementSnapshot>        windowOutputTextCustomElements;
		// Dispatch-sampled cross-runtime and backend state.
		bool                                                hasUdpPortSnapshot{false};
		QList<int>                                          udpPortsSnapshot;
		QHash<int, QString>                                 udpListenerPluginIdsByPort;
		bool                                                hasUsedUdpPortsSnapshot{false};
		QSet<int>                                           usedUdpPortsSnapshot;
		QHash<int, int>                                     usedUdpPortReferenceCountsSnapshot;
		QHash<int, int>                                     soundStatusByBuffer;
		QHash<int, bool>                                    soundBufferReusableByBuffer;
		// Request-local callback line context and overlays.
		bool                                                hasLineBufferSnapshot{false};
		int                                                 lineBufferCount{0};
		QSharedPointer<const LuaCallbackLineBufferSnapshot> lineBufferSnapshot;
		QHash<int, LuaCallbackLineEntrySnapshot>            lineEntriesByBufferIndex;
		bool                                                hasCallbackOutputAnchor{false};
		int                                                 callbackOutputAnchorBufferIndex{0};
		qint64                                              callbackOutputAnchorAbsoluteNumber{0};
		bool                                                callbackOutputAnchorIsActiveIncoming{false};
		bool                                                callbackOutputAnchorIsHidden{false};
		// Deferred output playback and promoted runtime cursors share this state across immutable
		// snapshots. Queued and suspended callbacks therefore retain position and presentation without a scan.
		std::shared_ptr<LuaCallbackOutputAnchorState>       callbackOutputAnchorState;
		bool                                                triggerMatchedLineSnapshotResolved{false};
		bool                                                hasTriggerMatchedLineSnapshot{false};
		int                                                 triggerMatchedLineBufferIndex{0};
		qint64                                              triggerMatchedLineAbsoluteNumber{0};
		LuaCallbackLineEntrySnapshot                        triggerMatchedLineSnapshot;
		bool                                                hasLineBufferDeltaSnapshot{false};
		bool                                                hasLineBufferCountDelta{false};
		int                                                 lineBufferDeltaCount{0};
		QHash<int, LuaCallbackLineEntrySnapshot>            lineEntryDeltasByBufferIndex;
		QSet<int>                                           missingLineEntryDeltasByBufferIndex;
		bool                                                linePresentationRequiresRefresh{false};
		bool                                                outputScrollPositionRequiresRefresh{false};
		bool                                            miniWindowGeometryConstraintRequiresRefresh{false};
		bool                                            recentLinesSnapshotInvalidated{false};
		bool                                            hasRecentLinesSnapshot{false};
		QStringList                                     recentLinesSnapshot;
		QVariantHash                                    commandUiValues;
		QVariantHash                                    runtimeCounterValues;
		// Dispatch-local entity, main-window, clipboard, and notepad presentation.
		bool                                            hasEntitySnapshot{false};
		QHash<QString, QString>                         entityValuesByName;
		bool                                            hasUiSnapshot{false};
		QVariantMap                                     guiSystemValues;
		bool                                            hasClipboardText{false};
		QString                                         clipboardText;
		QHash<int, QRect>                               mainWindowPositionsByMode;
		bool                                            mainWindowPositionsDirty{false};
		QHash<QString, QRect>                           worldWindowPositionsByKey;
		QSet<QString>                                   missingWorldWindowPositionKeys;
		QSet<int>                                       dirtyWorldWindowPositionOrdinals;
		QHash<QString, QRect>                           notepadWindowPositionsByKey;
		QSet<QString>                                   missingNotepadWindowPositionKeys;
		QSet<QString>                                   dirtyNotepadWindowPositionKeys;
		QSet<QString>                                   dirtyNotepadDocumentKeys;
		QHash<QString, int>                             notepadLengthByKey;
		QSet<QString>                                   missingNotepadLengthKeys;
		QHash<QString, QString>                         notepadTextByKey;
		QSet<QString>                                   missingNotepadTextKeys;
		QSet<QString>                                   dirtyNotepadListKeys;
		QHash<QString, QStringList>                     notepadListByKey;
		QSet<QString>                                   missingNotepadListKeys;
		QVector<LuaCallbackWorldRuntimeSnapshot>        worldRuntimeSnapshot;
		QVector<LuaCallbackWorldWindowPositionSnapshot> worldWindowPositionSnapshot;
		bool                                            hasNotepadPresentationSnapshot{false};
		QVector<LuaCallbackNotepadSnapshot>             notepadSnapshot;
		int                                             actionSourceOverride{0};
		bool                                            hasActionSourceOverride{false};
};

/**
 * @brief Batch callback dispatch request payload.
 */
struct LuaBatchDispatchRequest
{
		LuaBatchDispatchKind          kind{LuaBatchDispatchKind::NoArgs};
		LuaBatchDispatchLane          lane{LuaBatchDispatchLane::Callback};
		LuaCallbackEvent              callbackEvent{LuaCallbackEvent::None};
		LuaCallbackLineSnapshotPolicy lineSnapshotPolicy{LuaCallbackLineSnapshotPolicy::CountAndLast};
		QVector<QSharedPointer<LuaCallbackEngine>>                            engines;
		/** First engine in the shared recipient vector that this request dispatches. */
		int                                                                   firstEngineIndex{0};
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
		QSharedPointer<const LuaCallbackSnapshot>                             callbackSnapshotArg;
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
		bool    executeScriptHasTriggerContext{false};
		bool    triggerOutputReplacesMatchedLine{false};
		bool    applyCallingPluginContext{false};
		bool    screendrawExecutionGuard{false};
		bool    drawOutputWindowExecutionGuard{false};
		/** Keep WindowAddHotspot guarded while processing mouse callbacks induced by an add. */
		bool    windowAddHotspotReentryBlocked{false};
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
		/** Whether callbacks raised while applying this batch must retain its callback-lane position. */
		bool                           requiresCausalOrdering{true};
		/** Whether callbacks raised while applying this batch inherit WindowAddHotspot reentry blocking. */
		bool                           windowAddHotspotReentryBlocked{false};
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
		QSharedPointer<const LuaCallbackSnapshot>          callbackSnapshotAfterMutations;
		bool                                               callbackSnapshotIsPresentationOnly{false};
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
 * @brief Complete behavior classification for one batch dispatch kind.
 *
 * The policy array is generated in enum order from the same registry that declares LuaBatchDispatchKind. This
 * retains one authoritative classification without generating switches containing repeated return branches.
 */
struct LuaBatchDispatchPolicy
{
		LuaBatchAggregateShape          aggregateShape;
		LuaBatchResumeContinuationShape continuationShape;
		LuaBatchFallbackShape           fallbackShape;
		LuaBatchFunctionNamePolicy      functionNamePolicy;
};

inline constexpr auto kLuaBatchDispatchPolicies = std::to_array<LuaBatchDispatchPolicy>({
#define QMUD_DECLARE_LUA_BATCH_DISPATCH_POLICY(name, aggregateShape, continuationShape, fallbackShape,       \
                                               functionNamePolicy)                                           \
	LuaBatchDispatchPolicy{                                                                                  \
	    LuaBatchAggregateShape::aggregateShape, LuaBatchResumeContinuationShape::continuationShape,          \
	    LuaBatchFallbackShape::fallbackShape, LuaBatchFunctionNamePolicy::functionNamePolicy},
    QMUD_LUA_BATCH_DISPATCH_KIND_REGISTRY(QMUD_DECLARE_LUA_BATCH_DISPATCH_POLICY)
#undef QMUD_DECLARE_LUA_BATCH_DISPATCH_POLICY
});

inline constexpr LuaBatchDispatchPolicy kInvalidLuaBatchDispatchPolicy{
    LuaBatchAggregateShape::None,
    LuaBatchResumeContinuationShape::ResultOnly,
    LuaBatchFallbackShape::AggregateNeutral,
    LuaBatchFunctionNamePolicy::Required,
};

[[nodiscard]] constexpr const LuaBatchDispatchPolicy &
luaBatchDispatchPolicy(const LuaBatchDispatchKind kind) noexcept
{
	const auto index = static_cast<std::size_t>(kind);
	return index < kLuaBatchDispatchPolicies.size() ? kLuaBatchDispatchPolicies[index]
	                                                : kInvalidLuaBatchDispatchPolicy;
}

/**
 * @brief Returns the aggregate shape for a batch dispatch kind.
 * @param kind Dispatch kind to classify.
 * @return Shared aggregate result shape.
 */
[[nodiscard]] constexpr LuaBatchAggregateShape luaBatchDispatchAggregateShape(const LuaBatchDispatchKind kind)
{
	return luaBatchDispatchPolicy(kind).aggregateShape;
}

/**
 * @brief Returns the authoritative post-resume continuation shape for a dispatch kind.
 */
[[nodiscard]] constexpr LuaBatchResumeContinuationShape
luaBatchDispatchResumeContinuationShape(const LuaBatchDispatchKind kind)
{
	return luaBatchDispatchPolicy(kind).continuationShape;
}

/**
 * @brief Returns the authoritative neutral-result contract for a dispatch kind.
 */
[[nodiscard]] constexpr LuaBatchFallbackShape luaBatchDispatchFallbackShape(const LuaBatchDispatchKind kind)
{
	return luaBatchDispatchPolicy(kind).fallbackShape;
}

/**
 * @brief Returns whether a dispatch kind requires a non-empty Lua function name.
 */
[[nodiscard]] constexpr bool luaBatchDispatchRequiresFunctionName(const LuaBatchDispatchKind kind)
{
	return luaBatchDispatchPolicy(kind).functionNamePolicy == LuaBatchFunctionNamePolicy::Required;
}

#undef QMUD_LUA_BATCH_DISPATCH_KIND_REGISTRY

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

	switch (luaBatchDispatchFallbackShape(request.kind))
	{
	case LuaBatchFallbackShape::HasFunctionFalse:
		result.hasFunction      = false;
		result.hasFunctionValid = true;
		break;
	case LuaBatchFallbackShape::BoolFalse:
		result.boolResult      = false;
		result.boolResultValid = true;
		break;
	case LuaBatchFallbackShape::BoolAndFunctionFalse:
		result.boolResult       = false;
		result.boolResultValid  = true;
		result.hasFunction      = false;
		result.hasFunctionValid = true;
		break;
	case LuaBatchFallbackShape::AggregateNeutral:
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
 * @brief Carries a cumulative in/out aggregate into the request for the next recipient.
 * @param kind Original batch dispatch kind.
 * @param request Next-recipient request to update.
 * @param aggregate Cumulative result produced by the preceding recipients.
 *
 * In/out continuation is an aggregation policy, not a runtime dispatch-kind policy. Keeping this switch beside the
 * authoritative aggregate implementation prevents continuation sites from maintaining their own kind lists.
 */
inline void carryLuaBatchAggregateIntoNextRecipientRequest(const LuaBatchDispatchKind    kind,
                                                           LuaBatchDispatchRequest      &request,
                                                           const LuaBatchDispatchResult &aggregate)
{
	switch (luaBatchDispatchAggregateShape(kind))
	{
	case LuaBatchAggregateShape::BytesInOut:
		request.bytesArg = aggregate.bytesResult;
		break;
	case LuaBatchAggregateShape::StringInOut:
		request.stringArg = aggregate.stringResult;
		break;
	case LuaBatchAggregateShape::BooleanAndFunction:
	case LuaBatchAggregateShape::StopOnFalse:
	case LuaBatchAggregateShape::FunctionHandled:
	case LuaBatchAggregateShape::StopOnTrue:
	case LuaBatchAggregateShape::FunctionCount:
	case LuaBatchAggregateShape::FunctionAny:
	case LuaBatchAggregateShape::LastResult:
	case LuaBatchAggregateShape::None:
		break;
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
 * @brief Merges an executor-produced recipient-range aggregate into an earlier aggregate.
 * @param kind Original batch dispatch kind.
 * @param aggregate Aggregate produced by recipients before the dispatched range.
 * @param rangeResult Aggregate produced by the dispatched recipient range.
 *
 * Unlike mergeLuaBatchRecipientResult(), this preserves raw partial values when the range ends in a
 * suspended recipient and therefore marks its public result fields invalid. Those raw values are required
 * when the suspended recipient resumes and completes the aggregate.
 */
inline void mergeLuaBatchRecipientRangeResult(const LuaBatchDispatchKind    kind,
                                              LuaBatchDispatchResult       &aggregate,
                                              const LuaBatchDispatchResult &rangeResult)
{
	mergeLuaBatchPresentationRefreshFlags(aggregate, rangeResult);
	switch (luaBatchDispatchAggregateShape(kind))
	{
	case LuaBatchAggregateShape::BooleanAndFunction:
	case LuaBatchAggregateShape::StopOnFalse:
		aggregate.boolResult       = aggregate.boolResult && rangeResult.boolResult;
		aggregate.boolResultValid  = rangeResult.boolResultValid;
		aggregate.hasFunction      = aggregate.hasFunction || rangeResult.hasFunction;
		aggregate.hasFunctionValid = rangeResult.hasFunctionValid;
		break;
	case LuaBatchAggregateShape::FunctionHandled:
	case LuaBatchAggregateShape::StopOnTrue:
		aggregate.boolResult       = aggregate.boolResult || rangeResult.boolResult;
		aggregate.boolResultValid  = rangeResult.boolResultValid;
		aggregate.hasFunction      = aggregate.hasFunction || rangeResult.hasFunction;
		aggregate.hasFunctionValid = rangeResult.hasFunctionValid;
		break;
	case LuaBatchAggregateShape::FunctionCount:
		aggregate.countResult += rangeResult.countResult;
		aggregate.countResultValid = rangeResult.countResultValid;
		break;
	case LuaBatchAggregateShape::FunctionAny:
		aggregate.hasFunction      = aggregate.hasFunction || rangeResult.hasFunction;
		aggregate.hasFunctionValid = rangeResult.hasFunctionValid;
		break;
	case LuaBatchAggregateShape::BytesInOut:
		aggregate.bytesResult = rangeResult.bytesResult;
		break;
	case LuaBatchAggregateShape::StringInOut:
		aggregate.stringResult = rangeResult.stringResult;
		break;
	case LuaBatchAggregateShape::LastResult:
		if (rangeResult.boolResultValid)
		{
			aggregate.boolResult      = rangeResult.boolResult;
			aggregate.boolResultValid = true;
		}
		if (rangeResult.hasFunctionValid)
		{
			aggregate.hasFunction      = rangeResult.hasFunction;
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

/**
 * @brief Returns whether a callback result published committed or deferred runtime mutations.
 * @param result Dispatch result to inspect before its deferred mutation batches are consumed.
 * @return `true` when the runtime must advance the request snapshot before another recipient runs.
 *
 * Owner-thread callbacks normally apply mutations synchronously and publish the resulting immutable snapshot.
 * Worker callbacks additionally carry deferred mutation batches. Both are representations of the same mutation
 * boundary; continuation code must use this predicate rather than testing either representation independently.
 */
[[nodiscard]] inline bool luaBatchPublishedMutationBoundary(const LuaBatchDispatchResult &result)
{
	if (result.callbackSnapshotAfterMutations && !result.callbackSnapshotIsPresentationOnly)
		return true;
	return std::ranges::any_of(result.deferredRuntimeMutationBatches,
	                           [](const LuaDeferredRuntimeMutationBatch &batch)
	                           { return batch.requiresCausalOrdering; });
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
 * Every runtime uses the worker-thread executor. Its worker lane invokes the
 * same-thread direct primitive through this interface.
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
 * @brief Same-thread execution primitive used exclusively on a Lua worker lane.
 */
class LuaExecutorDirect final : public ILuaExecutor
{
};

/**
 * @brief Creates the runtime Lua executor backend.
 * @return Worker-thread Lua executor used by every runtime.
 */
std::unique_ptr<ILuaExecutor> makeLuaExecutor(LuaDeferredRuntimeMutationConsumer shutdownMutationConsumer);

#endif // QMUD_LUAEXECUTOR_H
