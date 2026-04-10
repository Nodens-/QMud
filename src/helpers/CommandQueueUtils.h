/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: CommandQueueUtils.h
 * Role: Pure helper utilities for outbound command queue encoding/decoding and batching behavior.
 */

#ifndef QMUD_COMMANDQUEUEUTILS_H
#define QMUD_COMMANDQUEUEUTILS_H

#include <QStringList>

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

#endif // QMUD_COMMANDQUEUEUTILS_H
