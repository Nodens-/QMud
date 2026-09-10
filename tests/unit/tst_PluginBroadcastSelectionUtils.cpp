/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * @file tst_PluginBroadcastSelectionUtils.cpp
 * @brief Unit coverage for broadcast recipient selection helper semantics.
 *
 * Role: Keep off-thread broadcast recipient selection deterministic and independent from observed-callback caches.
 */

#include "PluginBroadcastSelectionUtils.h"

#include <QtTest/QTest>

namespace
{
	struct BroadcastCandidate
	{
			QString id;
			bool    executable{false};
			bool    observedBroadcastCallback{false};
	};
	/**
	 * @brief QTest fixture covering broadcast recipient selection semantics.
	 */
	class tst_PluginBroadcastSelectionUtils : public QObject
	{
			Q_OBJECT

		private slots:
			/**
			 * @brief Ensures the caller is excluded while preserving recipient order.
			 */
			static void excludesCallerAndPreservesOrder()
			{
				const QVector<BroadcastCandidate> candidates{
				    {QStringLiteral("sender"), true, true},
				    {QStringLiteral("recv_a"), true, true},
				    {QStringLiteral("recv_b"), true, true},
				};

				const QVector<int> recipients = qmudCollectBroadcastRecipientIndices(
				    candidates, QStringLiteral("sender"),
				    [&candidates](const int index) { return candidates.at(index).executable; },
				    [&candidates](const int index) { return candidates.at(index).id; });

				QCOMPARE(recipients, QVector<int>({1, 2}));
			}

			/**
			 * @brief Ensures only executable plugins are selected.
			 */
			static void filtersNonExecutablePlugins()
			{
				const QVector<BroadcastCandidate> candidates{
				    {QStringLiteral("sender"),   true,  true},
				    {QStringLiteral("disabled"), false, true},
				    {QStringLiteral("pending"),  false, true},
				    {QStringLiteral("recv"),     true,  true},
				};

				const QVector<int> recipients = qmudCollectBroadcastRecipientIndices(
				    candidates, QStringLiteral("sender"),
				    [&candidates](const int index) { return candidates.at(index).executable; },
				    [&candidates](const int index) { return candidates.at(index).id; });

				QCOMPARE(recipients, QVector<int>({3}));
			}

			/**
			 * @brief Ensures off-thread selection does not depend on observed callback-presence cache state.
			 */
			static void ignoresObservedCallbackPresenceCacheState()
			{
				const QVector<BroadcastCandidate> candidates{
				    {QStringLiteral("sender"),                    true, true },
				    {QStringLiteral("receiver_unknown_presence"), true, false},
				};

				const QVector<int> recipients = qmudCollectBroadcastRecipientIndices(
				    candidates, QStringLiteral("sender"),
				    [&candidates](const int index) { return candidates.at(index).executable; },
				    [&candidates](const int index) { return candidates.at(index).id; });

				QCOMPARE(recipients, QVector<int>({1}));
			}
	};

} // namespace

QTEST_APPLESS_MAIN(tst_PluginBroadcastSelectionUtils)

#if __has_include("tst_PluginBroadcastSelectionUtils.moc")
#include "tst_PluginBroadcastSelectionUtils.moc"
#endif
