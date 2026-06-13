/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldCommandProcessorUtils.h
 * Role: Consolidated pure helper utilities used by world command processing and related tests.
 */

#ifndef QMUD_WORLDCOMMANDPROCESSORUTILS_H
#define QMUD_WORLDCOMMANDPROCESSORUTILS_H

// ReSharper disable once CppUnusedIncludeDirective
#include <QString>
#include <QStringList>

namespace QMudCommandPattern
{
	/**
	 * @brief Converts wildcard/regex command pattern into a normalized Qt regex string.
	 * @param matchString Raw user pattern text.
	 * @param wholeLine Anchor pattern to full line when `true`.
	 * @param makeAsterisksWildcards Interpret `*` as wildcard groups when `true`.
	 * @return Regex pattern string suitable for `QRegularExpression`.
	 */
	QString convertToRegularExpression(const QString &matchString, bool wholeLine = true,
	                                   bool makeAsterisksWildcards = true);
} // namespace QMudCommandPattern

namespace QMudCommandQueue
{
	/**
	 * @brief Decoded queue entry fields extracted from encoded command payload.
	 */
	struct QueueEntry
	{
			bool    withEcho{false};
			bool    logIt{false};
			bool    queuedType{false};
			QString payload;
	};

	/**
	 * @brief Determines whether command should be enqueued instead of immediate dispatch.
	 * @param speedWalkDelayMs Active speedwalk delay in milliseconds.
	 * @param queueRequested Explicit queue/send-to-queue flag.
	 * @param queueNotEmpty Whether command queue already contains entries.
	 * @return `true` when command should be queued.
	 */
	bool        shouldQueueCommand(int speedWalkDelayMs, bool queueRequested, bool queueNotEmpty);

	/**
	 * @brief Encodes command and flags into queue storage format.
	 * @param payload Command text.
	 * @param queueRequested Explicit queue/send-to-queue flag.
	 * @param echo Echo command locally when dispatched.
	 * @param logIt Log command when dispatched.
	 * @return Encoded queue entry string.
	 */
	QString     encodeQueueEntry(const QString &payload, bool queueRequested, bool echo, bool logIt);

	/**
	 * @brief Decodes one queue storage entry into structured fields.
	 * @param entry Encoded queue entry string.
	 * @return Decoded queue entry fields.
	 */
	QueueEntry  decodeQueueEntry(const QString &entry);

	/**
	 * @brief Removes and returns one dispatch batch from queue.
	 * @param queue Mutable queue list.
	 * @param flushAll Remove all queued commands when `true`.
	 * @return Commands selected for immediate dispatch.
	 */
	QStringList takeDispatchBatch(QStringList &queue, bool flushAll);

	/**
	 * @brief Clears queue and reports number of discarded entries.
	 * @param queue Mutable queue list.
	 * @return Number of removed queue entries.
	 */
	int         discardAll(QStringList &queue);
} // namespace QMudCommandQueue

namespace QMudCommandText
{
	/**
	 * @brief Expands escaped control sequences in command text.
	 * @param source Raw command text with escape sequences.
	 * @return Command text with escapes normalized for send path.
	 */
	QString fixupEscapeSequences(const QString &source);

	/**
	 * @brief Converts action command body line endings to the send pipeline terminator.
	 * @param source Action command text from send-to-world/queue bodies.
	 * @return Command text with CRLF, LF, and CR represented as CRLF.
	 */
	QString normalizeActionCommandTextForSend(const QString &source);

	/**
	 * @brief Applies wildcard replacement/post-processing for send-target transforms.
	 * @param wildcard Wildcard value to process.
	 * @param makeLowerCase Force lowercase conversion when `true`.
	 * @param sendTo Active send-target mode identifier.
	 * @param language Active language option used by casing rules.
	 * @return Processed wildcard text.
	 */
	QString fixWildcard(const QString &wildcard, bool makeLowerCase, int sendTo, const QString &language);

	/**
	 * @brief Normalizes one trigger match line according to regexp/wildcard mode.
	 * @param line Incoming line text.
	 * @param preserveTrailingWhitespace Keep trailing spaces/tabs when `true`.
	 * @return Normalized trigger match target line.
	 */
	QString normalizeTriggerMatchLine(const QString &line, bool preserveTrailingWhitespace);

	/**
	 * @brief Builds multiline trigger target text from recent lines.
	 * @param recentLines Recent line history in chronological order.
	 * @param preserveTrailingWhitespace Keep trailing spaces/tabs per line when `true`.
	 * @return Multiline target text joined with trailing `\n` per line.
	 */
	QString buildTriggerMultilineTarget(const QStringList &recentLines, bool preserveTrailingWhitespace);
} // namespace QMudCommandText

namespace QMudTriggerSound
{
	/**
	 * @brief Determines whether trigger sound playback should be enabled for a trigger context.
	 * @param pluginScoped `true` when the trigger belongs to a plugin.
	 * @param worldTriggerSoundsEnabled `true` when world trigger sounds option is enabled.
	 * @return `true` when trigger sound playback should be allowed.
	 */
	bool shouldPlayTriggerSound(bool pluginScoped, bool worldTriggerSoundsEnabled);
} // namespace QMudTriggerSound

#endif // QMUD_WORLDCOMMANDPROCESSORUTILS_H
