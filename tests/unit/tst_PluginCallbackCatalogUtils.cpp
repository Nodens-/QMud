/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_PluginCallbackCatalogUtils.cpp
 * Role: Unit coverage for observed-plugin-callback generation tracking/pruning helpers.
 */

#include "PluginCallbackCatalogUtils.h"

#include <QtTest/QTest>

/**
 * @brief QTest fixture for generation-based observed callback tracking helper functions.
 */
class tst_PluginCallbackCatalogUtils : public QObject
{
		Q_OBJECT

	private slots:
		/**
		 * @brief Verifies alternating callback queries stay retained inside retention window.
		 */
		static void alternatingQueriesRemainTrackedWithinRetentionWindow()
		{
			QSet<QString>           observedCallbacks;
			QHash<QString, quint64> queryGenerations;
			quint64                 generation = 1;

			noteObservedPluginCallbackQuery(queryGenerations, QStringLiteral("OnA"), generation);
			QVERIFY(ensureObservedPluginCallback(observedCallbacks, QStringLiteral("OnA")));
			noteObservedPluginCallbackQuery(queryGenerations, QStringLiteral("OnB"), generation);
			QVERIFY(ensureObservedPluginCallback(observedCallbacks, QStringLiteral("OnB")));

			for (int i = 0; i < 128; ++i)
			{
				const QString queried = (i % 2 == 0) ? QStringLiteral("OnA") : QStringLiteral("OnB");
				noteObservedPluginCallbackQuery(queryGenerations, queried, generation);
				pruneStaleObservedPluginCallbacks(observedCallbacks, queryGenerations, generation,
				                                  observedPluginCallbackRetentionGenerations());
				QVERIFY2(observedCallbacks.contains(QStringLiteral("OnA")), "OnA was pruned too early");
				QVERIFY2(observedCallbacks.contains(QStringLiteral("OnB")), "OnB was pruned too early");
				advanceObservedPluginCallbackGeneration(generation);
			}
		}

		/**
		 * @brief Verifies stale callback names are pruned after retention generations elapse.
		 */
		static void callbackPrunedAfterRetentionWindowExpires()
		{
			QSet<QString>           observedCallbacks;
			QHash<QString, quint64> queryGenerations;
			quint64                 generation = 1;

			noteObservedPluginCallbackQuery(queryGenerations, QStringLiteral("OnIdle"), generation);
			QVERIFY(ensureObservedPluginCallback(observedCallbacks, QStringLiteral("OnIdle")));
			QVERIFY(observedCallbacks.contains(QStringLiteral("OnIdle")));

			for (quint64 i = 0; i <= observedPluginCallbackRetentionGenerations(); ++i)
			{
				pruneStaleObservedPluginCallbacks(observedCallbacks, queryGenerations, generation,
				                                  observedPluginCallbackRetentionGenerations());
				advanceObservedPluginCallbackGeneration(generation);
			}

			pruneStaleObservedPluginCallbacks(observedCallbacks, queryGenerations, generation,
			                                  observedPluginCallbackRetentionGenerations());
			QVERIFY(!observedCallbacks.contains(QStringLiteral("OnIdle")));
			QVERIFY(!queryGenerations.contains(QStringLiteral("OnIdle")));
		}

		/**
		 * @brief Verifies tracking reset clears all tracked names/query generations and resets counter.
		 */
		static void resetClearsTrackingState()
		{
			QSet<QString>           observedCallbacks;
			QHash<QString, quint64> queryGenerations;
			quint64                 generation = 42;

			noteObservedPluginCallbackQuery(queryGenerations, QStringLiteral("OnTest"), generation);
			QVERIFY(ensureObservedPluginCallback(observedCallbacks, QStringLiteral("OnTest")));

			resetObservedPluginCallbackTracking(observedCallbacks, queryGenerations, generation);
			QVERIFY(observedCallbacks.isEmpty());
			QVERIFY(queryGenerations.isEmpty());
			QCOMPARE(generation, static_cast<quint64>(1));
		}

		/**
		 * @brief Verifies recipient filtering preserves order and removes out-of-range entries.
		 */
		static void filtersRecipientIndicesByPluginCount()
		{
			const QVector<int> cached{2, -1, 0, 9, 1, 3};
			const QVector<int> filtered = qmudFilterValidPluginRecipientIndices(cached, 3);
			QCOMPARE(filtered, QVector<int>({2, 0, 1}));
		}

		/**
		 * @brief Verifies recipient filtering returns empty list for non-positive plugin count.
		 */
		static void filtersRecipientIndicesHandlesNonPositivePluginCount()
		{
			const QVector<int> cached{0, 1, 2};
			QVERIFY(qmudFilterValidPluginRecipientIndices(cached, 0).isEmpty());
			QVERIFY(qmudFilterValidPluginRecipientIndices(cached, -1).isEmpty());
		}

		/**
		 * @brief Verifies single-recipient self-broadcast skip logic.
		 */
		static void selfOnlyBroadcastCanBeSkipped()
		{
			const QVector<int> recipients{1};
			const bool skip = qmudShouldSkipSelfOnlyPluginBroadcast(recipients, 3, QStringLiteral("plugin-b"),
			                                                        [](const int index)
			                                                        {
				                                                        switch (index)
				                                                        {
				                                                        case 0:
					                                                        return QStringLiteral("plugin-a");
				                                                        case 1:
					                                                        return QStringLiteral("plugin-B");
				                                                        default:
					                                                        return QStringLiteral("plugin-c");
				                                                        }
			                                                        });
			QVERIFY(skip);
		}

		/**
		 * @brief Verifies skip logic stays disabled for empty caller or multiple recipients.
		 */
		static void selfOnlySkipRequiresSingleCallerRecipient()
		{
			const QVector<int> single{0};
			QVERIFY(!qmudShouldSkipSelfOnlyPluginBroadcast(single, 1, QString(), [](const int)
			                                               { return QStringLiteral("plugin-a"); }));

			const QVector<int> multiple{0, 1};
			QVERIFY(!qmudShouldSkipSelfOnlyPluginBroadcast(
			    multiple, 2, QStringLiteral("plugin-a"), [](const int index)
			    { return index == 0 ? QStringLiteral("plugin-a") : QStringLiteral("plugin-b"); }));
		}

		/**
		 * @brief Verifies skip logic does not trigger when callback accessor is missing.
		 */
		static void selfOnlySkipRequiresValidAccessor()
		{
			const QVector<int> recipients{0};
			QVERIFY(!qmudShouldSkipSelfOnlyPluginBroadcast(recipients, 1, QStringLiteral("plugin-a"),
			                                               std::function<QString(int)>()));
		}
};

QTEST_APPLESS_MAIN(tst_PluginCallbackCatalogUtils)

#if __has_include("tst_PluginCallbackCatalogUtils.moc")
#include "tst_PluginCallbackCatalogUtils.moc"
#endif
