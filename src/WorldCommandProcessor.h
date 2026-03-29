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

#include "LuaCallbackEngine.h"
#include "WorldRuntime.h"
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QTimer>

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
		 * @brief Sends text using explicit send-target from accelerator context.
		 * @param sendTo Send-target enum value.
		 * @param text Text to send.
		 * @param description Optional description text.
		 * @param plugin Optional originating plugin.
		 */
		void               sendToFromAccelerator(int sendTo, const QString &text, const QString &description,
		                                         const WorldRuntime::Plugin *plugin);

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
		 */
		void onIncomingStyledLineReceived(const QString &line, const QVector<WorldRuntime::StyleSpan> &spans);
		/**
		 * @brief Processes partial styled line updates.
		 * @param line Partial line text.
		 * @param spans Style spans.
		 */
		void onIncomingStyledLinePartialReceived(const QString                          &line,
		                                         const QVector<WorldRuntime::StyleSpan> &spans) const;
		/**
		 * @brief Dispatches activated hyperlink actions.
		 * @param href Hyperlink target.
		 */
		void onHyperlinkActivated(const QString &href);
		/**
		 * @brief Starts timers/queue flow after world connects.
		 */
		void handleWorldConnected();
		/**
		 * @brief Stops connection-dependent processing after disconnect.
		 */
		void handleWorldDisconnected() const;
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

	signals:
		/**
		 * @brief Requests script send-target processing.
		 * @param text Script send text.
		 * @param afterOmit Invoke after omit step when `true`.
		 */
		void sendToScriptRequested(const QString &text, bool afterOmit);

	private:
		/**
		 * @brief Parses and executes one command line.
		 * @param input Command input text.
		 * @return `true` when command was processed successfully.
		 */
		bool           evaluateCommand(const QString &input);
		/**
		 * @brief Internal speedwalk evaluator used by public wrapper.
		 * @param speedWalkString Speedwalk expression.
		 * @return Expanded command text.
		 */
		QString        doEvaluateSpeedwalk(const QString &speedWalkString) const;
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
		 * @param plugin Optional plugin scope.
		 * @param ok Optional output success flag.
		 * @return Formatted send text.
		 */
		QString        fixSendText(const QString &source, int sendTo, const QStringList &wildcards,
		                           const QMap<QString, QString> &namedWildcards, const QString &language,
		                           bool makeWildcardsLower, bool expandVariables, bool expandWildcards,
		                           bool fixRegexps, bool isRegexp, bool throwExceptions, const QString &name,
		                           const WorldRuntime::Plugin *plugin, bool *ok) const;
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
		 * @return `true` when pattern matches.
		 */
		bool           regexMatch(const QString &pattern, const QString &subject, bool ignoreCase,
		                          QStringList &wildcards, QMap<QString, QString> &namedWildcards,
		                          int *startCol = nullptr, int *endCol = nullptr, int startOffset = 0,
		                          bool multiLine = false) const;
		struct TriggerScript
		{
				WorldRuntime::Plugin  *plugin{nullptr};
				int                    index{-1};
				QString                label;
				QString                line;
				QStringList            wildcards;
				QMap<QString, QString> namedWildcards;
		};
		struct DeferredScript
		{
				WorldRuntime::Plugin *plugin{nullptr};
				QString               scriptText;
				QString               description;
		};
		struct TriggerEvaluationResult
		{
				bool                                            omitFromOutput{false};
				bool                                            omitFromLog{false};
				QVector<WorldRuntime::StyleSpan>                spans;
				QString                                         extraOutput;
				QVector<DeferredScript>                         deferredScripts;
				QVector<TriggerScript>                          triggerScripts;
				QVector<QPair<WorldRuntime::Plugin *, QString>> oneShotTriggers;
		};
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
		 * @param plugin Optional plugin scope.
		 * @return `true` when variable is found.
		 */
		bool findVariable(const QString &name, QString &value, const WorldRuntime::Plugin *plugin) const;
		struct AliasRef
		{
				WorldRuntime::Plugin  *plugin{nullptr};
				int                    index{-1};
				QString                name;
				QStringList            wildcards;
				QMap<QString, QString> namedWildcards;
		};
		struct TriggerOrderCacheEntry
		{
				int          count{0};
				quint64      signature{0};
				QVector<int> indices;
		};
		struct AliasOrderCacheEntry
		{
				int          count{0};
				quint64      signature{0};
				QVector<int> indices;
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
		 * @brief Evaluates one alias pass sequence.
		 * @param currentLine Current command line.
		 * @param countThem Count match stats when `true`.
		 * @param omitFromLog Output omit-from-log flag.
		 * @param echoAlias Output echo-alias flag.
		 * @param omitFromHistory Output omit-from-history flag.
		 * @param matchedAliases Output matched alias refs.
		 * @param oneShotAliases Output one-shot alias refs.
		 * @param plugin Optional plugin scope.
		 * @return `true` when any alias matched.
		 */
		bool processOneAliasSequence(const QString &currentLine, bool countThem, bool &omitFromLog,
		                             bool &echoAlias, bool &omitFromHistory,
		                             QVector<AliasRef> &matchedAliases, QVector<AliasRef> &oneShotAliases,
		                             WorldRuntime::Plugin *plugin);
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
		 * @param plugin Optional originating plugin.
		 */
		void sendTo(int sendTo, const QString &text, bool omitFromOutput, bool omitFromLog,
		            const QString &variableName, const QString &description,
		            const WorldRuntime::Plugin *plugin);
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
		 * @brief Executes immediate send path.
		 * @param text Text to send.
		 * @param echo Echo text when `true`.
		 * @param logIt Log send when `true`.
		 */
		void doSendMsg(const QString &text, bool echo, bool logIt);
		/**
		 * @brief Writes line to session log with optional wrappers.
		 * @param text Line text.
		 * @param preambleKey Preamble option key.
		 * @param postambleKey Postamble option key.
		 */
		void logLine(const QString &text, const QString &preambleKey, const QString &postambleKey) const;
		/**
		 * @brief Checks whether a world Lua callback can be executed.
		 * @param functionType Callback type label.
		 * @param functionName Callback function name.
		 * @param lua Lua callback engine.
		 * @param requireFunction Require function to exist when `true`.
		 * @return `true` when callback can execute.
		 */
		bool canExecuteWorldScript(const QString &functionType, const QString &functionName,
		                           LuaCallbackEngine *lua, bool requireFunction) const;

		WorldRuntime                              *m_runtime{nullptr};
		WorldView                                 *m_view{nullptr};
		QStringList                                m_queuedCommands;
		bool                                       m_queueStatusOwnsMessage{false};
		int                                        m_speedWalkDelay{0};
		QString                                    m_speedWalkFiller;
		bool                                       m_pluginProcessingSend{false};
		bool                                       m_pluginProcessingSent{false};
		bool                                       m_noEcho{false};
		bool                                       m_translateGerman{false};
		bool                                       m_translateBackslashSequences{false};
		bool                                       m_processingAutoSay{false};
		bool                                       m_enableSpamPrevention{false};
		bool                                       m_doNotTranslateIac{false};
		bool                                       m_regexpMatchEmpty{true};
		bool                                       m_utf8{false};
		int                                        m_spamLineCount{0};
		QString                                    m_spamMessage;
		QString                                    m_lastCommandSent;
		int                                        m_lastCommandCount{0};
		QTimer                                    *m_timerCheck{nullptr};
		QElapsedTimer                              m_queueDispatchTimer;
		bool                                       m_suppressInputLog{false};
		bool                                       m_processingEnteredCommand{false};
		bool                                       m_omitFromHistoryForEnteredCommand{false};
		bool                                       m_enteredCommandSendFailed{false};
		int                                        m_executionDepth{0};
		mutable QHash<QString, QRegularExpression> m_regexCache;
		QHash<QString, QString>                    m_wildcardRegexCache;
		mutable QSet<QString>                      m_invalidRegexWarnings;
		QHash<quintptr, TriggerOrderCacheEntry>    m_triggerOrderCache;
		QHash<quintptr, AliasOrderCacheEntry>      m_aliasOrderCache;
		PluginOrderCacheEntry                      m_pluginOrderCache;
};

#endif // QMUD_WORLDCOMMANDPROCESSOR_H
