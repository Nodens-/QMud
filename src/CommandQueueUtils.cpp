/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: CommandQueueUtils.cpp
 * Role: Pure helper utilities for outbound command queue encoding/decoding and batching behavior.
 */

#include "CommandQueueUtils.h"

#include <limits>

namespace QMudCommandQueue
{
	bool shouldQueueCommand(const int speedWalkDelayMs, const bool queueRequested, const bool queueNotEmpty)
	{
		return speedWalkDelayMs > 0 && (queueRequested || queueNotEmpty);
	}

	QString encodeQueueEntry(const QString &payload, const bool queueRequested, const bool echo, const bool logIt)
	{
		QString flag;
		if (queueRequested)
			flag = echo ? QStringLiteral("E") : QStringLiteral("e");
		else
			flag = echo ? QStringLiteral("I") : QStringLiteral("i");

		if (!logIt)
			flag = flag.toLower();
		return flag + payload;
	}

	QueueEntry decodeQueueEntry(const QString &entry)
	{
		if (entry.isEmpty())
			return {};

		const QChar kind  = entry.at(0);
		const QChar upper = kind.toUpper();

		QueueEntry out;
		out.withEcho   = (upper == QLatin1Char('E') || upper == QLatin1Char('I'));
		out.logIt      = kind.isUpper();
		out.queuedType = (upper == QLatin1Char('E'));
		out.payload    = entry.mid(1);
		return out;
	}

	QStringList takeDispatchBatch(QStringList &queue, const bool flushAll)
	{
		QStringList batch;
		while (!queue.isEmpty())
		{
			const QString queued = queue.takeFirst();
			if (queued.isEmpty())
				continue;

			batch.append(queued);
			if (!flushAll && decodeQueueEntry(queued).queuedType)
				break;
		}
		return batch;
	}

	int discardAll(QStringList &queue)
	{
		const qsizetype size = queue.size();
		queue.clear();
		if (size <= 0)
			return 0;
		constexpr qsizetype kMaxInt = std::numeric_limits<int>::max();
		return size > kMaxInt ? std::numeric_limits<int>::max() : static_cast<int>(size);
	}
} // namespace QMudCommandQueue
