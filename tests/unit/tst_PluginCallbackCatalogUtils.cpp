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
};

QTEST_APPLESS_MAIN(tst_PluginCallbackCatalogUtils)

#if __has_include("tst_PluginCallbackCatalogUtils.moc")
#include "tst_PluginCallbackCatalogUtils.moc"
#endif
