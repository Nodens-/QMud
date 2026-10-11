/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldCommandProcessor.h
 * Role: Command-processing interfaces for parsing user input, expanding aliases, and dispatching executable world
 * actions.
 */

#ifndef QMUD_WORLDCOMMANDPROCESSOR_H
#define QMUD_WORLDCOMMANDPROCESSOR_H

#include "WorldRuntime.h"
#include <QColor>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QQueue>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>

class WorldRuntime;
class WorldView;

/**
 * @brief Command execution pipeline for one world session.
 *
 * Parses input, applies queue/speedwalk/alias logic, runs plugin hooks, and
 * dispatches resolved sends through the bound world runtime.
 */
class WorldCommandProcessor : public QObject
{
		Q_OBJECT
		friend class WorldRuntime;
		friend class tst_LuaCallbackEngine;

	public:
		/**
		 * @brief Creates a command processor for one world/session.
		 * @param parent Optional Qt parent object.
		 */
		explicit WorldCommandProcessor(QObject *parent = nullptr);

		/**
		 * @brief Binds the target runtime used for command execution.
		 * @param runtime Runtime instance.
		 */
		void               setRuntime(WorldRuntime *runtime);
		/**
		 * @brief Reloads command-processing options from the bound runtime.
		 */
		void               refreshRuntimeOptions();
		/**
		 * @brief Applies one table-bound numeric world option to the command processor cache.
		 * @param binding Exact cache binding declared by the numeric option table.
		 * @param value Stored numeric option value.
		 */
		void               applyNumericWorldOption(WorldNumericOptionBinding binding, long long value);
		/**
		 * @brief Selects whether this processor owns session-wide timer evaluation.
		 * @param owner `true` for the runtime's primary command processor.
		 */
		void               setRuntimeAutomationOwner(bool owner);
		/**
		 * @brief Binds the owning world view for UI interactions.
		 * @param view World view instance.
		 */
		void               setView(WorldView *view);
		/**
		 * @brief Returns currently queued outbound commands.
		 * @return Queued command list.
		 */
		const QStringList &queuedCommands() const;
		/**
		 * @brief Returns count of trigger-priority commands at the queue front.
		 * @return Number of front-band trigger-priority queued commands.
		 */
		int                triggerPriorityQueuedCommandCount() const;
		/**
		 * @brief Clears queued commands and returns number discarded.
		 * @return Discarded command count.
		 */
		int                discardQueuedCommands();
		/**
		 * @brief Expands speedwalk notation into plain command text.
		 * @param speedWalkString Speedwalk expression.
		 * @return Expanded command text.
		 */
		QString            evaluateSpeedwalk(const QString &speedWalkString) const;
		/**
		 * @brief Executes one user command through alias/queue/send pipeline.
		 * @param text Command text.
		 * @return Execution status/result code.
		 */
		int                executeCommand(const QString &text);
		/**
		 * @brief Executes one Send Now macro through alias/queue/send pipeline.
		 *
		 * The caller owns action-source setup; set the runtime source to
		 * `WorldRuntime::eUserMacro` when macro source semantics are required.
		 *
		 * @param text Macro Send Now text.
		 * @param history Add original macro text to history when `true`.
		 * @return Execution status/result code.
		 */
		int                executeUserMacroSendNow(const QString &text, bool history);
		/**
		 * @brief Executes one direct trigger-script command with priority over queued movement.
		 * @param text Command text.
		 * @return Execution status/result code.
		 */
		int                executeCommandWithTriggerPriority(const QString &text);
		/**
		 * @brief Sends text using explicit send-target from accelerator context.
		 * @param sendTo Send-target enum value.
		 * @param text Text to send.
		 * @param description Optional description text.
		 * @param plugin Optional originating plugin.
		 */
		void               sendToFromAccelerator(int sendTo, const QString &text, const QString &description,
		                                         const WorldRuntime::Plugin *plugin);

	signals:
		/**
		 * @brief Reports a committed change to the queued-command presentation.
		 */
		void queuedCommandsChanged();
		/**
		 * @brief Requests script-send execution through the processor script dispatch channel.
		 * @param pluginId Origin plugin ID, or empty for world script context.
		 * @param text Script text/code to execute.
		 * @param description Script description for diagnostics.
		 * @param callbackEvent Runtime event that caused this script invocation.
		 * @param styleRuns Optional style runs to expose as `TriggerStyleRuns`.
		 * @param hasTriggerContext Execute with trigger line context when `true`.
		 * @param replaceMatchedLineOutput First output replaces matched line when `true`.
		 * @param triggerMatchedLineBufferIndex One-based matched line index captured at dispatch.
		 * @param triggerMatchedLineAbsoluteNumber Stable matched line number captured at dispatch.
		 */
		void sendToScriptRequested(const QString &pluginId, const QString &text, const QString &description,
		                           LuaCallbackEvent callbackEvent, const QVector<LuaStyleRun> *styleRuns,
		                           bool hasTriggerContext, bool replaceMatchedLineOutput,
		                           int    triggerMatchedLineBufferIndex,
		                           qint64 triggerMatchedLineAbsoluteNumber);

	public slots:
		/**
		 * @brief Handles text entered from input control.
		 * @param text Entered command text.
		 */
		void onCommandEntered(const QString &text);
		/**
		 * @brief Processes one plain incoming server line.
		 * @param line Incoming line text.
		 */
		void onIncomingLineReceived(const QString &line);
		/**
		 * @brief Processes one styled incoming server line.
		 * @param line Incoming line text.
		 * @param spans Style spans.
		 * @param partialLineRevision Stable identity of the partial line completed by this line.
		 * @param serverSideWrapActive Whether NAWS governed wrapping when this logical line began.
		 */
		void onIncomingStyledLineReceived(const QString &line, const QVector<WorldRuntime::StyleSpan> &spans,
		                                  quint64 partialLineRevision = 0, bool serverSideWrapActive = false);
		/**
		 * @brief Processes partial styled line updates.
		 * @param line Partial line text.
		 * @param spans Style spans.
		 * @param partialLineRevision Stable identity of the partial line update.
		 * @param serverSideWrapActive Whether NAWS governed wrapping when this logical line began.
		 */
		void onIncomingStyledLinePartialReceived(const QString                          &line,
		                                         const QVector<WorldRuntime::StyleSpan> &spans,
		                                         quint64 partialLineRevision  = 0,
		                                         bool    serverSideWrapActive = false);
		/**
		 * @brief Dispatches activated hyperlink actions.
		 * @param href Hyperlink target.
		 */
		void onHyperlinkActivated(const QString &href);
		/**
		 * @brief Dispatches miniwindow output actions using world-link semantics.
		 * @param actionType Action type.
		 * @param action Action payload.
		 */
		void onMiniWindowOutputActionActivated(int actionType, const QString &action);
		/**
		 * @brief Starts timers/queue flow after world connects.
		 */
		void handleWorldConnected();
		/**
		 * @brief Stops connection-dependent processing after disconnect.
		 */
		void handleWorldDisconnected();
		/**
		 * @brief Completes disconnect housekeeping after lifecycle callbacks finish.
		 */
		void finishWorldDisconnected();
		/**
		 * @brief Emits note text to output path.
		 * @param text Note text.
		 * @param newLine Append newline when `true`.
		 */
		void note(const QString &text, bool newLine = true) const;
		/**
		 * @brief Sends raw command text with echo/queue/log/history options.
		 * @param text Command text.
		 * @param echo Echo command when `true`.
		 * @param queue Queue command when `true`.
		 * @param log Log command when `true`.
		 * @param history Add command to history when `true`.
		 */
		void sendRawText(const QString &text, bool echo, bool queue, bool log, bool history);
		/**
		 * @brief Sends direct trigger-script raw command text with priority over queued movement.
		 * @param text Command text.
		 * @param echo Echo command when `true`.
		 * @param queue Queue command when `true`.
		 * @param log Log command when `true`.
		 * @param history Add command to history when `true`.
		 */
		void sendRawTextWithTriggerPriority(const QString &text, bool echo, bool queue, bool log,
		                                    bool history);
		/**
		 * @brief Sends text immediately bypassing queue delay.
		 * @param text Command text.
		 * @param echo Echo command when `true`.
		 * @param log Log command when `true`.
		 * @param history Add command to history when `true`.
		 */
		void sendImmediateText(const QString &text, bool echo, bool log, bool history);
		/**
		 * @brief Writes input line to log according to settings.
		 * @param text Input text.
		 */
		void logInputCommand(const QString &text) const;
		/**
		 * @brief Enables or disables local echo suppression.
		 * @param enabled Enable no-echo mode when `true`.
		 */
		void setNoEcho(bool enabled);
		/**
		 * @brief Returns current no-echo flag.
		 * @return `true` when no-echo mode is active.
		 */
		bool noEcho() const;
		/**
		 * @brief Returns whether any plugin has sent during current processing.
		 * @return `true` when plugin send occurred.
		 */
		bool pluginProcessingSent() const;

	private:
		struct CommandHistoryState
		{
				bool omitFromHistory{false};
				bool sendFailed{false};
		};
		using CommandCompletion = std::function<void(bool)>;
		struct LogicalOperationCompletion;
		using LogicalOperationCompletionPtr  = QSharedPointer<LogicalOperationCompletion>;
		using InputCriticalOperationScopePtr = LogicalOperationCompletionPtr;
		struct CommandExecutionContext
		{
				unsigned short                      actionSource{WorldRuntime::eUnknownActionSource};
				int                                 executionDepth{0};
				bool                                suppressInputLog{false};
				bool                                processingAutoSay{false};
				int                                 triggerCommandPriorityDepth{0};
				int                                 triggerPriorityActionExecutionDepth{0};
				QSharedPointer<CommandHistoryState> historyState;
				LogicalOperationCompletionPtr       operationCompletion;
				InputCriticalOperationScopePtr      inputCriticalOperationScope;
		};
		using CallerContextRunner = RuntimeCallerContext;
		struct PendingPluginCommandEvaluation;
		struct PendingPluginCommandEntered;
		struct PendingPluginSendDecision;
		struct PendingPluginSentNotification;
		struct PendingOutboundCommand;
		struct CommandEvaluationSequence;
		struct AliasEvaluationSequence;
		struct TimerEvaluationSequence;
		struct PendingEnteredCommand
		{
				QString                             text;
				unsigned short                      actionSource{WorldRuntime::eUnknownActionSource};
				QSharedPointer<CommandHistoryState> historyState;
				CommandCompletion                   completion;
		};
		struct PendingTriggerPriorityAction
		{
				enum class Kind
				{
					Execute,
					Send
				};

				Kind                          kind{Kind::Execute};
				QString                       text;
				bool                          echo{false};
				bool                          queueIt{false};
				bool                          logIt{false};
				CallerContextRunner           callerContext;
				LogicalOperationCompletionPtr operationCompletion;
		};
		/**
		 * @brief Parses and executes one command line.
		 * @param input Command input text.
		 * @param allowCommandStacking Apply command stacking when `true`; stacked children pass `false` so escaped
		 * separators remain literal during their normal command processing.
		 * @param completion Callback invoked when this command has fully completed, including suspended callbacks.
		 * @return `true` when command was processed successfully.
		 */
		bool evaluateCommand(const QString &input, bool allowCommandStacking = true,
		                     CommandCompletion completion = {});
		/**
		 * @brief Starts one logical command and retains its nesting depth through suspension.
		 * @param text Command text.
		 * @param completion Callback invoked when the logical command completes.
		 * @return Execution status/result code.
		 */
		int  executeCommandWithCompletion(const QString &text, CommandCompletion completion);
		/**
		 * @brief Creates a logical completion group joined to the currently active parent operation.
		 * @param completion Callback invoked after this group and all of its retained work complete.
		 * @param joinActiveOperation Join the current logical operation as this group's parent when `true`.
		 * @return New logical completion group.
		 */
		[[nodiscard]] LogicalOperationCompletionPtr
		createLogicalOperationCompletion(CommandCompletion completion, bool joinActiveOperation = true);
		[[nodiscard]] CommandExecutionContext captureCommandExecutionContext() const;
		[[nodiscard]] CallerContextRunner     captureCallerContextRunner() const;
		/**
		 * @brief Captures command and runtime causal state for work that will resume later.
		 * @return Runner restoring the complete runtime caller context.
		 */
		[[nodiscard]] CallerContextRunner     captureRuntimeCallerContextRunner() const;
		/**
		 * @brief Captures caller state without joining later work to the current logical operation.
		 * @return Runner restoring ambient command state for asynchronous independent work.
		 */
		[[nodiscard]] CallerContextRunner     captureAmbientCallerContextRunner() const;
		void    runWithCommandExecutionContext(const CommandExecutionContext &context,
		                                       const std::function<void()>   &work);
		void    runWithInputCriticalOperationScope(const InputCriticalOperationScopePtr &scope,
		                                           const std::function<void()>          &work);
		/**
		 * @brief Queues one trigger-priority action while retaining its originating logical operation.
		 * @param action Action payload to queue.
		 */
		void    enqueuePendingTriggerPriorityAction(PendingTriggerPriorityAction action);
		void    ensureTriggerPriorityOutboundGroup();
		void    releaseTriggerPriorityOutboundGroup();
		void    runWithTriggerPriorityOutboundGroup(const std::function<void()> &work) const;
		/**
		 * @brief Completes one suspended trigger-priority command and advances later direct actions.
		 */
		void    finishTriggerPriorityCommand(bool completed);
		/**
		 * @brief Cancels queued trigger-priority actions and releases their originating operations.
		 */
		void    cancelPendingTriggerPriorityActions();
		/**
		 * @brief Advances direct trigger actions queued behind a result-dependent command.
		 */
		void    drainPendingTriggerPriorityActions();
		/**
		 * @brief Continues command evaluation after OnPluginCommand has accepted the command.
		 * @param line Normalized command text.
		 * @param echoInput Whether ordinary command sends should be echoed.
		 * @return `true` when command processing reached its successful terminal path.
		 */
		bool    continueEvaluateCommandAfterPluginCommand(const QString &line, bool echoInput);
		/**
		 * @brief Starts ordered evaluation of an expanded command stack.
		 * @param commands Expanded commands in execution order.
		 * @param completion Callback invoked after the final command completes.
		 * @param autoSayEvaluation Continue through the auto-say fallback when `true`.
		 */
		void    evaluateCommandSequence(QStringList commands, CommandCompletion completion,
		                                bool autoSayEvaluation = false);
		/**
		 * @brief Runs one rule action under a child logical-completion group.
		 * @param action Action to start in the captured command context.
		 * @param completion Completion invoked after all asynchronous descendants finish.
		 */
		void    runOrderedRuleAction(const std::function<void()> &action, CommandCompletion completion);
		/**
		 * @brief Continues entered-command processing after its plugin transform completes.
		 * @param commandText Transformed command text.
		 */
		void    continueCommandEnteredAfterPluginTransform(QString commandText);
		/**
		 * @brief Starts one entered command with its captured caller context.
		 * @param command Entered text and action source captured at submission.
		 */
		void    startEnteredCommand(PendingEnteredCommand command);
		/**
		 * @brief Finalizes entered-command history and action-source state.
		 * @param historyText Transformed command text retained for history.
		 * @param historyState History state accumulated while processing the entered command.
		 * @param executionAccepted Whether command evaluation was accepted.
		 * @param completed Whether the entered command and its asynchronous descendants completed.
		 */
		void    finishCommandEntered(const QString                             &historyText,
		                             const QSharedPointer<CommandHistoryState> &historyState,
		                             bool executionAccepted, bool completed);
		/**
		 * @brief Starts entered commands queued while an earlier command was suspended.
		 */
		void    cancelActiveEnteredCommand();
		/**
		 * @brief Internal speedwalk evaluator used by public wrapper.
		 * @param speedWalkString Speedwalk expression.
		 * @return Expanded command text.
		 */
		QString doEvaluateSpeedwalk(const QString &speedWalkString) const;
		/**
		 * @brief Formats standardized speedwalk parser error text.
		 * @param message Error detail.
		 * @return Formatted error string.
		 */
		static QString makeSpeedWalkErrorString(const QString &message);
		/**
		 * @brief Converts wildcard syntax into regular expression text.
		 * @param matchString Wildcard pattern.
		 * @param wholeLine Anchor regex to whole line when `true`.
		 * @param makeAsterisksWildcards Treat `*` as wildcard when `true`.
		 * @return Converted regular-expression string.
		 */
		static QString convertToRegularExpression(const QString &matchString, bool wholeLine = true,
		                                          bool makeAsterisksWildcards = true);
		/**
		 * @brief Applies escape-sequence expansion rules.
		 * @param source Source text.
		 * @return Expanded text.
		 */
		static QString fixupEscapeSequences(const QString &source);
		/**
		 * @brief Returns cached wildcard regex conversion.
		 * @param matchText Wildcard pattern text.
		 * @return Cached/converted regex text.
		 */
		QString        wildcardToRegexCached(const QString &matchText);
		/**
		 * @brief Normalizes wildcard value for script/send contexts.
		 * @param wildcard Wildcard text.
		 * @param makeLowerCase Lowercase result when `true`.
		 * @param sendTo Send-target enum value.
		 * @param language Scripting language identifier.
		 * @return Normalized wildcard text.
		 */
		static QString fixWildcard(const QString &wildcard, bool makeLowerCase, int sendTo,
		                           const QString &language);
		/**
		 * @brief Escapes string for HTML-safe output.
		 * @param source Source text.
		 * @return HTML-escaped text.
		 */
		static QString fixHtmlString(const QString &source);
		/**
		 * @brief Expands variables/wildcards and applies send-target formatting.
		 * @param source Source text.
		 * @param sendTo Send-target enum value.
		 * @param wildcards Positional wildcard values.
		 * @param namedWildcards Named wildcard values.
		 * @param language Scripting language identifier.
		 * @param makeWildcardsLower Lowercase wildcard expansions when `true`.
		 * @param expandVariables Expand variables when `true`.
		 * @param expandWildcards Expand wildcards when `true`.
		 * @param fixRegexps Apply regex fixups when `true`.
		 * @param isRegexp Source is regex pattern when `true`.
		 * @param throwExceptions Throw on errors when `true`.
		 * @param name Context label/name.
		 * @param pluginId Plugin scope ID, or empty for world variables.
		 * @param ok Optional output success flag.
		 * @return Formatted send text.
		 */
		QString        fixSendText(const QString &source, int sendTo, const QStringList &wildcards,
		                           const QMap<QString, QString> &namedWildcards, const QString &language,
		                           bool makeWildcardsLower, bool expandVariables, bool expandWildcards,
		                           bool fixRegexps, bool isRegexp, bool throwExceptions, const QString &name,
		                           const QString &pluginId, bool *ok) const;
		/**
		 * @brief Matches subject text using configured regex options.
		 * @param pattern Regex pattern text.
		 * @param subject Subject text.
		 * @param ignoreCase Ignore case when `true`.
		 * @param wildcards Output positional wildcard captures.
		 * @param namedWildcards Output named wildcard captures.
		 * @param startCol Optional output start column.
		 * @param endCol Optional output end column.
		 * @param startOffset Start offset in subject.
		 * @param multiLine Enable multiline mode when `true`.
		 * @param executionTimeNs Optional cumulative regex-execution time in nanoseconds.
		 * @param matchCount Optional successful-match capture count.
		 * @param matchAttempts Optional valid-regex execution counter.
		 * @param matchOptions Match-time regular-expression options.
		 * @return `true` when pattern matches.
		 */
		bool
		regexMatch(const QString &pattern, const QString &subject, bool ignoreCase, QStringList &wildcards,
		           QMap<QString, QString> &namedWildcards, int *startCol = nullptr, int *endCol = nullptr,
		           int startOffset = 0, bool multiLine = false, qint64 *executionTimeNs = nullptr,
		           int *matchCount = nullptr, int *matchAttempts = nullptr,
		           QRegularExpression::MatchOptions matchOptions = QRegularExpression::NoMatchOption) const;
		/**
		 * @brief Matches subject text with an already compiled regex.
		 * @param regex Compiled regex.
		 * @param warningKey Stable key used to coalesce invalid-regex warnings.
		 * @param subject Subject text.
		 * @param wildcards Output positional wildcard captures.
		 * @param namedWildcards Output named wildcard captures.
		 * @param startCol Optional output start column.
		 * @param endCol Optional output end column.
		 * @param startOffset Start offset in subject.
		 * @param executionTimeNs Optional cumulative regex-execution time in nanoseconds.
		 * @param matchCount Optional successful-match capture count.
		 * @param matchAttempts Optional valid-regex execution counter.
		 * @param matchOptions Qt match options applied to this execution.
		 * @return `true` when the regex matches.
		 */
		bool compiledRegexMatch(
		    const QRegularExpression &regex, const QString &warningKey, const QString &subject,
		    QStringList &wildcards, QMap<QString, QString> &namedWildcards, int *startCol = nullptr,
		    int *endCol = nullptr, int startOffset = 0, qint64 *executionTimeNs = nullptr,
		    int *matchCount = nullptr, int *matchAttempts = nullptr,
		    QRegularExpression::MatchOptions matchOptions = QRegularExpression::NoMatchOption) const;
		struct TriggerScript
		{
				quint64                runtimeId{0};
				quint64                runtimeLocationHandle{0};
				quint64                pluginEngineInstanceId{0};
				quint64                pluginRuntimeInstanceId{0};
				quint64                pluginRuntimeLocationHandle{0};
				int                    pluginIndexHint{-1};
				int                    indexHint{-1};
				QString                pluginId;
				QString                label;
				QString                scriptName;
				QString                line;
				QStringList            wildcards;
				QMap<QString, QString> namedWildcards;
				bool                   replaceMatchedLineOutput{false};
		};
		struct DeferredScript
		{
				quint64                                    triggerRuntimeId{0};
				quint64                                    triggerRuntimeLocationHandle{0};
				int                                        triggerIndexHint{-1};
				quint64                                    pluginEngineInstanceId{0};
				quint64                                    pluginRuntimeInstanceId{0};
				quint64                                    pluginRuntimeLocationHandle{0};
				QString                                    pluginId;
				QString                                    scriptText;
				QString                                    description;
				QSharedPointer<const QVector<LuaStyleRun>> styleRuns;
				bool                                       trackTriggerExecution{false};
				bool                                       replaceMatchedLineOutput{false};
		};
		struct RuleRef
		{
				quint64 runtimeId{0};
				quint64 runtimeLocationHandle{0};
				quint64 pluginRuntimeInstanceId{0};
				quint64 pluginRuntimeLocationHandle{0};
				int     pluginIndexHint{-1};
				int     indexHint{-1};
				QString pluginId;
		};
		struct TriggerEvaluationResult
		{
				bool                             omitFromOutput{false};
				bool                             omitFromLog{false};
				QVector<WorldRuntime::StyleSpan> spans;
				QString                          extraOutput;
				QVector<DeferredScript>          deferredScripts;
				QVector<TriggerScript>           triggerScripts;
				QVector<RuleRef>                 oneShotTriggers;
		};
		struct PendingIncomingStreamItem
		{
				using OrderedAction       = std::function<void()>;
				using InputCriticalAction = std::function<void(CommandCompletion)>;

				enum class Kind : quint8
				{
					CompleteLine,
					PartialLine,
					OrderedAction,
					InputCriticalAction
				};

				Kind                                                          kind{Kind::CompleteLine};
				QString                                                       line;
				QVector<WorldRuntime::StyleSpan>                              spans;
				OrderedAction                                                 action;
				InputCriticalAction                                           inputCriticalAction;
				CommandExecutionContext                                       inputCriticalActionContext;
				CallerContextRunner                                           inputCriticalCallerContext;
				std::shared_ptr<LuaDeferredRuntimeMutationPlayback>           nestingPlayback;
				QSharedPointer<WorldRuntime::IncomingStreamPresentationBatch> presentationBatch;
				InputCriticalOperationScopePtr                                inputCriticalOperationScope;
				LuaCallbackIncomingLineObservation                            observation;
				unsigned short actionSource{WorldRuntime::eUnknownActionSource};
				quint64        partialLineRevision{0};
				bool           serverSideWrapActive{false};
				bool           doingSimulate{false};
		};
		struct PendingIncomingLineOperation
		{
				QPointer<WorldCommandProcessor>                               processor;
				QSharedPointer<PendingIncomingLineOperation>                  parent;
				std::function<void()>                                         restoreParentLineContext;
				std::shared_ptr<LuaDeferredRuntimeMutationPlayback>           nestingPlayback;
				QSharedPointer<WorldRuntime::IncomingStreamPresentationBatch> presentationBatch;
				InputCriticalOperationScopePtr                                inputCriticalOperationScope;
				LuaCallbackIncomingLineObservation                            observation;
				QString                                                       line;
				QVector<WorldRuntime::StyleSpan>                              spans;
				TriggerEvaluationResult                                       triggerResult;
				QSharedPointer<const QVector<LuaStyleRun>>                    styleRuns;
				QHash<QString, bool>                                          worldFunctionPresence;
				int       triggerMatchedLineBufferIndex{0};
				qint64    triggerMatchedLineAbsoluteNumber{0};
				qsizetype deferredScriptIndex{0};
				qsizetype triggerScriptIndex{0};
				int       wrapColumn{0};
				quint64   partialLineRevision{0};
				bool      runtimeWrap{false};
				bool      indentParas{true};
				bool      logOutput{false};
		};
		struct PendingIncomingStreamDrainFrame
		{
				const PendingIncomingLineOperation       *parent{nullptr};
				const LuaDeferredRuntimeMutationPlayback *nestingPlayback{nullptr};
				bool                                      nestedOnly{false};
		};
		struct PendingPluginLineDecision;
		void
		startIncomingLine(PendingIncomingStreamItem                                  request,
		                  const QSharedPointer<PendingIncomingLineOperation>        &parent          = {},
		                  const std::shared_ptr<LuaDeferredRuntimeMutationPlayback> &nestingPlayback = {});
		void
		continueIncomingLineAfterPluginDecision(const QSharedPointer<PendingIncomingLineOperation> &operation,
		                                        bool                                                accepted);
		void advanceIncomingLineScripts(const QSharedPointer<PendingIncomingLineOperation> &operation);
		void finishIncomingLine(const QSharedPointer<PendingIncomingLineOperation> &operation);
		void cancelIncomingLine(const QSharedPointer<PendingIncomingLineOperation> &operation);
		/**
		 * @brief Runs or queues a non-suspending protocol notification at its exact incoming-stream position.
		 * @param action Notification publication to execute on the runtime thread.
		 */
		void enqueueOrRunIncomingStreamAction(PendingIncomingStreamItem::OrderedAction action);
		void enqueueOrRunInputCriticalAction(PendingIncomingStreamItem::InputCriticalAction action);
		[[nodiscard]] InputCriticalOperationScopePtr
		                   createInputCriticalOperationScope(bool joinActiveOperation = false);
		static void        finishInputCriticalOperation(const InputCriticalOperationScopePtr &scope,
		                                                bool                                  completed = true);
		void               completeInputCriticalOperation(const InputCriticalOperationScopePtr &scope);
		void               cancelInputCriticalOperations();
		[[nodiscard]] bool hasCurrentInputCriticalOperation() const;
		void               adoptIncomingStreamBacklogPresentationBatch(PendingIncomingStreamItem &item);
		void               finishIncomingStreamBacklogPresentationBatchIfIdle();
		/**
		 * @brief Processes one queued incoming stream item.
		 * @param item Queued line, partial presentation, or protocol action.
		 * @param parent Parent line operation for nested playback, if any.
		 * @param nestingPlayback Playback grouping for nested input.
		 * @return `true` when processing became asynchronous and draining must stop.
		 */
		bool               processPendingIncomingStreamItem(
		    PendingIncomingStreamItem item, const QSharedPointer<PendingIncomingLineOperation> &parent,
		    const std::shared_ptr<LuaDeferredRuntimeMutationPlayback> &nestingPlayback);
		void drainPendingIncomingLines();
		void
		drainNestedIncomingLines(const QSharedPointer<PendingIncomingLineOperation>        &parent,
		                         const std::shared_ptr<LuaDeferredRuntimeMutationPlayback> &nestingPlayback);
		void drainPendingIncomingStreamItems(
		    const QSharedPointer<PendingIncomingLineOperation>        &parent,
		    const std::shared_ptr<LuaDeferredRuntimeMutationPlayback> &nestingPlayback, bool nestedOnly);
		void presentIncomingPartialLine(const QString &line, const QVector<WorldRuntime::StyleSpan> &spans,
		                                quint64 partialLineRevision, bool serverSideWrapActive) const;
		/**
		 * @brief Evaluates all trigger sets for an incoming line.
		 * @param line Incoming line text.
		 * @param spans Incoming style spans.
		 * @return Trigger evaluation result bundle.
		 */
		TriggerEvaluationResult processTriggersForLine(const QString                          &line,
		                                               const QVector<WorldRuntime::StyleSpan> &spans);
		/**
		 * @brief Resolves variable value from world or plugin scope.
		 * @param name Variable name.
		 * @param value Output variable value.
		 * @param pluginId Plugin scope ID, or empty for world variables.
		 * @return `true` when variable is found.
		 */
		bool findVariable(const QString &name, QString &value, const QString &pluginId) const;
		struct AliasRef
		{
				quint64                runtimeId{0};
				quint64                runtimeLocationHandle{0};
				quint64                pluginEngineInstanceId{0};
				quint64                pluginRuntimeInstanceId{0};
				quint64                pluginRuntimeLocationHandle{0};
				int                    pluginIndexHint{-1};
				QString                pluginId;
				QString                label;
				QString                scriptName;
				QString                line;
				QStringList            wildcards;
				QMap<QString, QString> namedWildcards;
				int                    indexHint{-1};
		};
		struct DecodedTrigger
		{
				int     index{0};
				int     sequence{0};
				QString matchText;
				QString sendText;
				QString sound;
				QString label;
				QString scriptLabel;
				QString variableName;
				QString scriptName;
				QString otherTextColour;
				QString otherBackColour;
				bool    enabled{false};
				bool    isRegexp{false};
				bool    ignoreCase{false};
				bool    multiLine{false};
				bool    expandVariables{false};
				bool    matchTextColour{false};
				bool    matchBack{false};
				bool    matchBold{false};
				bool    matchItalic{false};
				bool    matchUnderline{false};
				bool    matchInverse{false};
				bool    desiredBold{false};
				bool    desiredItalic{false};
				bool    desiredUnderline{false};
				bool    desiredInverse{false};
				bool    oneShot{false};
				bool    lowerWildcards{false};
				bool    omitFromLog{false};
				bool    omitFromOutput{false};
				bool    makeBold{false};
				bool    makeItalic{false};
				bool    makeUnderline{false};
				bool    repeatMatches{false};
				bool    keepEvaluating{false};
				bool    soundIfInactive{false};
				int     linesToMatch{0};
				int     sendToValue{0};
				int     textColour{-1};
				int     backColour{-1};
				int     colourChange{-1};
				int     changeType{0};
				int     clipboardArg{0};
				quint64 runtimeId{0};
				quint64 runtimeLocationHandle{0};
				qint64  executionTimeNs{0};
				int     matchCount{0};
				int     matchAttempts{0};
				bool    dynamicPattern{false};
				int     matcherIndex{-1};
		};
		struct CompiledTriggerMatcher
		{
				QString            warningKey;
				QRegularExpression regex;
		};
		struct TriggerEvaluationCacheEntry
		{
				quint64                         generation{0};
				int                             count{0};
				QVector<DecodedTrigger>         triggers;
				QVector<CompiledTriggerMatcher> matchers;
		};
		struct DecodedAlias
		{
				int     index{0};
				int     sequence{0};
				QString matchText;
				QString sendText;
				QString label;
				QString scriptLabel;
				QString variableName;
				QString scriptName;
				bool    enabled{false};
				bool    isRegexp{false};
				bool    ignoreCase{false};
				bool    omitCommandHistory{false};
				bool    echoMatchedAlias{false};
				bool    expandVariables{false};
				bool    omitFromOutput{false};
				bool    omitFromLog{false};
				bool    oneShot{false};
				bool    keepEvaluating{false};
				int     sendToValue{0};
		};
		struct AliasEvaluationPlanEntry
		{
				DecodedAlias decoded;
				quint64      runtimeId{0};
				quint64      runtimeLocationHandle{0};
				qint64       executionTimeNs{0};
				int          matchCount{0};
				int          matchAttempts{0};
		};
		struct AliasEvaluationCacheEntry
		{
				quint64               generation{0};
				int                   count{0};
				int                   enabledCount{0};
				QVector<DecodedAlias> aliases;
		};
		struct PluginOrderCacheEntry
		{
				int          count{0};
				quint64      signature{0};
				QVector<int> indices;
		};
		/**
		 * @brief Returns plugin indices in configured execution order.
		 * @return Ordered plugin index list.
		 */
		const QVector<int> &sortedPluginIndices();
		/**
		 * @brief Returns decoded trigger evaluation data for a trigger list.
		 * @param triggers Trigger list.
		 * @param plugin Owning plugin, or null for world triggers.
		 * @return Cached decoded trigger data.
		 */
		QSharedPointer<const TriggerEvaluationCacheEntry>
		decodedTriggerEvaluationCache(QList<WorldRuntime::Trigger> &triggers, WorldRuntime::Plugin *plugin);
		/**
		 * @brief Returns decoded alias evaluation data for an alias list.
		 * @param aliases Alias list.
		 * @return Cached decoded alias data.
		 */
		const AliasEvaluationCacheEntry      &
        decodedAliasEvaluationCache(const QList<WorldRuntime::Alias> &aliases);
		/**
		 * @brief Clears cached decoded trigger evaluation data.
		 */
		void invalidateTriggerEvaluationCache();
		/**
		 * @brief Ensures cached palette/default output colours match runtime settings.
		 * @param attrs World attributes used by output rendering.
		 */
		void ensurePaletteCache(const QMap<QString, QString> &attrs) const;
		/**
		 * @brief Executes due timers.
		 */
		void checkTimers();
		/**
		 * @brief Routes resolved text to selected send target.
		 * @param sendTo Send-target enum value.
		 * @param text Text to send.
		 * @param omitFromOutput Omit from output when `true`.
		 * @param omitFromLog Omit from log when `true`.
		 * @param variableName Variable name for variable targets.
		 * @param description Description text for UI/logging.
		 * @param pluginId Originating plugin ID, or empty for world scope.
		 * @param styleRuns Optional matched trigger style runs for script targets.
		 * @param hasTriggerContext Execute script target with trigger line context when `true`.
		 * @param replaceMatchedLineOutput First script output replaces matched line when `true`.
		 * @param triggerMatchedLineBufferIndex One-based matched line index captured at dispatch.
		 * @param triggerMatchedLineAbsoluteNumber Stable matched line number captured at dispatch.
		 * @param callbackEvent Runtime event that caused this script invocation.
		 */
		void sendTo(int sendTo, const QString &text, bool omitFromOutput, bool omitFromLog,
		            const QString &variableName, const QString &description, const QString &pluginId,
		            const QVector<LuaStyleRun> *styleRuns = nullptr, bool hasTriggerContext = false,
		            bool replaceMatchedLineOutput = false, int triggerMatchedLineBufferIndex = 0,
		            qint64           triggerMatchedLineAbsoluteNumber = 0,
		            LuaCallbackEvent callbackEvent                    = LuaCallbackEvent::None);
		/**
		 * @brief Executes one send-to-script request on the active runtime/script context.
		 * @param pluginId Origin plugin ID, or empty for world script context.
		 * @param text Script text/code to execute.
		 * @param description Script description for diagnostics.
		 * @param callbackEvent Runtime event that caused this script invocation.
		 * @param styleRuns Optional style runs to expose as `TriggerStyleRuns`.
		 * @param hasTriggerContext Execute with trigger line context when `true`.
		 * @param replaceMatchedLineOutput First output replaces matched line when `true`.
		 * @param triggerMatchedLineBufferIndex One-based matched line index captured at dispatch.
		 * @param triggerMatchedLineAbsoluteNumber Stable matched line number captured at dispatch.
		 */
		void dispatchScriptSend(const QString &pluginId, const QString &text, const QString &description,
		                        LuaCallbackEvent callbackEvent, const QVector<LuaStyleRun> *styleRuns,
		                        bool hasTriggerContext, bool replaceMatchedLineOutput,
		                        int triggerMatchedLineBufferIndex, qint64 triggerMatchedLineAbsoluteNumber);
		/**
		 * @brief Emits one trace line through plugin trace callbacks/world output.
		 * @param message Trace message without `TRACE:` prefix.
		 */
		void emitTrace(const QString &message) const;
		/**
		 * @brief Verifies world connection state before socket sends.
		 * @return `true` when connection state allows sending.
		 */
		bool ensureConnectedForSend() const;
		/**
		 * @brief Processes queued commands, optionally flushing all.
		 * @param flushAll Flush entire queue when `true`.
		 */
		void processQueuedCommands(bool flushAll);
		/**
		 * @brief Refreshes queued-command status line.
		 */
		void updateQueuedCommandsStatusLine();
		/**
		 * @brief Entry point for raw send path with queue controls.
		 * @param text Text to send.
		 * @param echo Echo text when `true`.
		 * @param queueIt Queue send when `true`.
		 * @param logIt Log send when `true`.
		 */
		void sendMsg(const QString &text, bool echo, bool queueIt, bool logIt);
		/**
		 * @brief Rejects an outbound request before it can mutate queue or presentation state.
		 * @return `true` when the request belongs to an earlier connection or disconnect has begun.
		 */
		[[nodiscard]] bool rejectInvalidOutboundRequest() const;
		enum class OutboundTransport
		{
			Immediate,
			CausalDeferred,
			CausalDeferredWithTriggerPriority
		};
		enum class AcceptedOutboundCompletionMode
		{
			ContinueOriginatingWork,
			CancelOriginatingWork
		};
		struct SpamCommitState
		{
				QString command;
				int     count{0};
		};
		struct PendingOutboundSend
		{
				enum class Phase
				{
					Start,
					AfterPluginSend
				};

				Phase                         phase{Phase::Start};
				QString                       text;
				bool                          echo{false};
				bool                          logIt{false};
				OutboundTransport             transport{OutboundTransport::Immediate};
				CallerContextRunner           callerContext;
				std::function<void(bool)>     mutationCompletion;
				LogicalOperationCompletionPtr operationCompletion;
				bool                          bypassSpamPrevention{false};
		};
		/**
		 * @brief Processes a send and either transmits or stages its encoded payload.
		 * @param text Text to send.
		 * @param echo Echo text when `true`.
		 * @param logIt Log send when `true`.
		 * @param transport Transport timing and priority.
		 * @param mutationCompletion Deferred mutation completion retained by this send.
		 * @param operationCompletion Logical operation retained by this send.
		 * @param bypassSpamPrevention Skip insertion of another spam filler for this send.
		 */
		void doSendMsg(const QString &text, bool echo, bool logIt, OutboundTransport transport,
		               std::function<void(bool)>     mutationCompletion   = {},
		               LogicalOperationCompletionPtr operationCompletion  = {},
		               bool                          bypassSpamPrevention = false);
		/**
		 * @brief Cancels one active OnPluginSend decision and resumes queued sends individually.
		 * @param mutationCompletion Deferred mutation completion retained by the active send.
		 * @param operationCompletion Logical operation retained by the active send.
		 */
		void cancelPluginSendDecision(const std::function<void(bool)>     &mutationCompletion,
		                              const LogicalOperationCompletionPtr &operationCompletion);
		/**
		 * @brief Cancels every queued plugin send while the owning runtime is shutting down.
		 */
		void cancelPendingPluginSendsForShutdown();
		/**
		 * @brief Settles outbound work owned by the runtime that is about to detach.
		 * @param runtime Runtime whose outbound binding is ending.
		 */
		void cancelOutboundWorkForRuntimeDetach(WorldRuntime *runtime);
		/**
		 * @brief Cancels every queued plugin send and releases its retained completions.
		 */
		void cancelPendingPluginSends();
		/**
		 * @brief Finishes a send after OnPluginSend accepts it.
		 * @param text Newline-terminated outbound text.
		 * @param echo Echo text when `true`.
		 * @param logIt Log send when `true`.
		 * @param transport Transport timing and priority.
		 * @param mutationCompletion Deferred mutation completion retained by this send.
		 * @param operationCompletion Logical operation retained by this send.
		 * @param bypassSpamPrevention Skip insertion of another spam filler for this send.
		 */
		void continueDoSendMsgAfterPluginSend(const QString &text, bool echo, bool logIt,
		                                      OutboundTransport             transport,
		                                      std::function<void(bool)>     mutationCompletion,
		                                      LogicalOperationCompletionPtr operationCompletion,
		                                      bool                          bypassSpamPrevention);
		/**
		 * @brief Submits a command transaction after send preprocessing has completed.
		 * @param text Newline-terminated outbound text.
		 * @param echo Echo text when `true`.
		 * @param logIt Log send when `true`.
		 * @param transport Transport timing and priority.
		 * @param mutationCompletion Deferred mutation completion retained by this send.
		 * @param operationCompletion Logical operation retained by this send.
		 * @param spamCommitState State committed only after the physical write succeeds.
		 */
		void startPluginSentNotification(const QString &text, bool echo, bool logIt,
		                                 OutboundTransport             transport,
		                                 std::function<void(bool)>     mutationCompletion,
		                                 LogicalOperationCompletionPtr operationCompletion,
		                                 SpamCommitState               spamCommitState);
		/**
		 * @brief Starts the fixed OnPluginSent roster after the socket accepts the command bytes.
		 * @param command Accepted outbound command state.
		 * @param transaction Physical-write transaction and causal continuation.
		 */
		void
		beginAcceptedPluginSentNotification(const std::shared_ptr<PendingOutboundCommand>    &command,
		                                    const QSharedPointer<OutboundCommandTransaction> &transaction);
		/**
		 * @brief Finishes host-side accounting after OnPluginSent completes or is cancelled after acceptance.
		 * @param command Accepted outbound command state.
		 * @param transaction Physical-write transaction and causal continuation.
		 * @param completionMode Whether work retained by the accepted command may continue.
		 */
		void completePluginSentNotification(const std::shared_ptr<PendingOutboundCommand>    &command,
		                                    const QSharedPointer<OutboundCommandTransaction> &transaction,
		                                    AcceptedOutboundCompletionMode                    completionMode =
		                                        AcceptedOutboundCompletionMode::ContinueOriginatingWork);
		/**
		 * @brief Rejects a command before its socket write is accepted.
		 * @param command Rejected outbound command state.
		 * @param transaction Rejected physical-write transaction.
		 */
		void rejectOutboundCommand(const std::shared_ptr<PendingOutboundCommand>    &command,
		                           const QSharedPointer<OutboundCommandTransaction> &transaction);
		/**
		 * @brief Clears the active outbound slot when it still belongs to the supplied transaction.
		 * @param command Command expected to own the slot.
		 * @param transaction Transaction expected to own the slot.
		 */
		void clearActiveOutboundCommand(const std::shared_ptr<PendingOutboundCommand>    &command,
		                                const QSharedPointer<OutboundCommandTransaction> &transaction);
		/**
		 * @brief Applies a completed OnPluginSend decision and advances queued sends.
		 * @param text Newline-terminated outbound text.
		 * @param echo Echo text when `true`.
		 * @param logIt Log send when `true`.
		 * @param transport Transport timing and priority.
		 * @param accepted Whether plugins accepted the send.
		 * @param mutationCompletion Deferred mutation completion retained by this send.
		 * @param operationCompletion Logical operation retained by this send.
		 * @param bypassSpamPrevention Skip insertion of another spam filler for this send.
		 */
		void completePluginSendDecision(const QString &text, bool echo, bool logIt,
		                                OutboundTransport transport, bool accepted,
		                                std::function<void(bool)>     mutationCompletion,
		                                LogicalOperationCompletionPtr operationCompletion,
		                                bool                          bypassSpamPrevention);
		/**
		 * @brief Completes a send that cannot cross its captured connection epoch.
		 * @param mutationCompletion Deferred mutation continuation retained by this send.
		 * @param operationCompletion Logical operation retained by this send.
		 * @param drainPendingSends Continue draining queued sends when `true`.
		 */
		void finishRejectedOutboundSend(std::function<void(bool)>     &mutationCompletion,
		                                LogicalOperationCompletionPtr &operationCompletion,
		                                bool                           drainPendingSends);
		/**
		 * @brief Rejects the current send when its context is stale or disconnect has begun.
		 * @param mutationCompletion Deferred mutation continuation retained by this send.
		 * @param operationCompletion Logical operation retained by this send.
		 * @param drainPendingSends Continue draining queued sends when `true`.
		 * @return `true` when the send was rejected and completed.
		 */
		[[nodiscard]] bool rejectInvalidOutboundContext(std::function<void(bool)>     &mutationCompletion,
		                                                LogicalOperationCompletionPtr &operationCompletion,
		                                                bool                           drainPendingSends);
		/**
		 * @brief Sends queued outbound operations after the active OnPluginSend decision completes.
		 */
		void               drainPendingPluginSends();
		/**
		 * @brief Writes line to session log with optional wrappers.
		 * @param text Line text.
		 * @param preambleKey Preamble option key.
		 * @param postambleKey Postamble option key.
		 * @param spans Optional output style spans for coloured HTML logging.
		 */
		void logLine(const QString &text, const QString &preambleKey, const QString &postambleKey,
		             const QVector<WorldRuntime::StyleSpan> &spans = {}) const;
		/**
		 * @brief Checks whether a world Lua callback can be executed.
		 * @param functionType Callback type label.
		 * @param functionName Callback function name.
		 * @param lua Lua callback engine.
		 * @return `true` when callback can execute.
		 */
		bool canExecuteWorldScript(const QString &functionType, const QString &functionName,
		                           const LuaCallbackEngine *lua) const;
		/**
		 * @brief Sends configured auto-connect strings and fires connect callbacks.
		 * @param connectMethod Auto-connect method value.
		 * @param player Character/player name.
		 * @param password Password text.
		 * @param connectText Configured connect text.
		 * @param echoInput Echo player-name send when `true`.
		 */
		void runWorldConnectActions(int connectMethod, const QString &player, const QString &password,
		                            const QString &connectText, bool echoInput);
		/**
		 * @brief Emits world-script missing-function warning when configured.
		 * @param functionType Callback type label.
		 * @param functionName Callback function name.
		 */
		void warnMissingWorldScriptFunction(const QString &functionType, const QString &functionName) const;

		WorldRuntime                                *m_runtime{nullptr};
		WorldView                                   *m_view{nullptr};
		QStringList                                  m_queuedCommands;
		int                                          m_triggerPriorityQueuedCommands{0};
		int                                          m_triggerCommandPriorityDepth{0};
		int                                          m_pendingTriggerPriorityCommands{0};
		int                                          m_triggerPriorityActionExecutionDepth{0};
		bool                                         m_drainingPendingTriggerPriorityActions{false};
		QQueue<PendingTriggerPriorityAction>         m_pendingTriggerPriorityActions;
		QSharedPointer<CausalOutboundGroup>          m_triggerPriorityOutboundGroup;
		bool                                         m_queueStatusOwnsMessage{false};
		int                                          m_speedWalkDelay{0};
		QString                                      m_speedWalkFiller;
		bool                                         m_pluginProcessingSend{false};
		bool                                         m_pluginProcessingSent{false};
		bool                                         m_pluginSendDecisionPending{false};
		bool                                         m_pluginSentNotificationPending{false};
		bool                                         m_drainingPendingPluginSends{false};
		bool                                         m_outboundBindingCancellationActive{false};
		quint64                                      m_pluginSendDecisionTicket{0};
		quint64                                      m_pluginSentNotificationTicket{0};
		std::shared_ptr<PendingOutboundCommand>      m_activeOutboundCommand;
		QSharedPointer<OutboundCommandTransaction>   m_activeOutboundTransaction;
		QQueue<PendingOutboundSend>                  m_pendingPluginSends;
		bool                                         m_noEcho{false};
		bool                                         m_translateGerman{false};
		bool                                         m_translateBackslashSequences{false};
		bool                                         m_processingAutoSay{false};
		bool                                         m_enableSpamPrevention{false};
		bool                                         m_doNotTranslateIac{false};
		bool                                         m_regexpMatchEmpty{true};
		bool                                         m_utf8{false};
		QString                                      m_legacyEncodingName;
		int                                          m_spamLineCount{0};
		quint64                                      m_autoConnectDelayGeneration{0};
		QString                                      m_spamMessage;
		QString                                      m_lastCommandSent;
		int                                          m_lastCommandCount{0};
		QTimer                                      *m_timerCheck{nullptr};
		std::shared_ptr<TimerEvaluationSequence>     m_timerEvaluationSequence;
		bool                                         m_timerEvaluationRerunPending{false};
		quint64                                      m_ruleEvaluationGeneration{0};
		QSet<quint64>                                m_reservedOneShotAliasRuntimeIds;
		QElapsedTimer                                m_queueDispatchTimer;
		bool                                         m_suppressInputLog{false};
		bool                                         m_processingEnteredCommand{false};
		QSharedPointer<CommandHistoryState>          m_activeCommandHistoryState;
		LogicalOperationCompletionPtr                m_activeOperationCompletion;
		CommandCompletion                            m_activeEnteredCommandCompletion;
		quint64                                      m_nextInputCriticalOperationId{1};
		InputCriticalOperationScopePtr               m_activeInputCriticalOperationScope;
		InputCriticalOperationScopePtr               m_currentInputCriticalOperationScope;
		bool                                         m_processingInputCriticalOperation{false};
		bool                                         m_processingIncomingLine{false};
		bool                                         m_processingIncomingStreamAction{false};
		QVector<PendingIncomingStreamDrainFrame>     m_pendingIncomingStreamDrainStack;
		QSharedPointer<PendingIncomingLineOperation> m_activeIncomingLineOperation;
		QQueue<PendingIncomingStreamItem>            m_pendingIncomingStreamItems;
		QSharedPointer<WorldRuntime::IncomingStreamPresentationBatch>
		                                           m_incomingStreamBacklogPresentationBatch;
		bool                                       m_runtimeAutomationOwner{true};
		int                                        m_executionDepth{0};
		mutable QHash<QString, QRegularExpression> m_regexCache;
		QHash<QString, QString>                    m_wildcardRegexCache;
		mutable QSet<QString>                      m_invalidRegexWarnings;
		QHash<quintptr, QSharedPointer<const TriggerEvaluationCacheEntry>> m_triggerEvaluationCache;
		quint64                                    m_triggerEvaluationCacheGeneration{0};
		QHash<quintptr, AliasEvaluationCacheEntry> m_aliasEvaluationCache;
		quint64                                    m_aliasEvaluationCacheGeneration{0};
		PluginOrderCacheEntry                      m_pluginOrderCache;
		mutable bool                               m_paletteCacheValid{false};
		mutable quint64                            m_paletteCacheSignature{0};
		mutable QVector<QColor>                    m_paletteCacheNormal;
		mutable QVector<QColor>                    m_paletteCacheBold;
		mutable QVector<QColor>                    m_paletteCacheCustomText;
		mutable QVector<QColor>                    m_paletteCacheCustomBack;
		mutable QColor                             m_paletteCacheDefaultFore;
		mutable QColor                             m_paletteCacheDefaultBack;
};

#endif // QMUD_WORLDCOMMANDPROCESSOR_H
