/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_ScriptErrorRoutingUtils.cpp
 * Role: Unit coverage for script-error routing decision helpers.
 */

#include "WorldCommandProcessorUtils.h"

#include <QtTest/QTest>

/**
 * @brief QTest fixture for script-error routing decision behavior.
 */
class tst_ScriptErrorRoutingUtils : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void shouldForceWorldErrorOutput_data()
		{
			QTest::addColumn<bool>("hasRuntime");
			QTest::addColumn<bool>("hasPluginScript");
			QTest::addColumn<bool>("expected");

			QTest::newRow("world-script") << true << false << true;
			QTest::newRow("plugin-script") << true << true << false;
			QTest::newRow("no-runtime-world-script") << false << false << false;
			QTest::newRow("no-runtime-plugin-script") << false << true << false;
		}

		void shouldForceWorldErrorOutput()
		{
			QFETCH(bool, hasRuntime);
			QFETCH(bool, hasPluginScript);
			QFETCH(bool, expected);

			QCOMPARE(QMudScriptErrorRouting::shouldForceWorldErrorOutput(hasRuntime, hasPluginScript),
			         expected);
		}

		void executeWithWorldErrorRoutingForcedWorldScriptCallsPushExecutePop()
		{
			int depth    = 0;
			int executed = 0;

			QMudScriptErrorRouting::executeWithWorldErrorRouting(
			    true, false,
			    [&]
			    {
				    QCOMPARE(depth, 1);
				    ++executed;
			    },
			    [&] { ++depth; }, [&] { --depth; });

			QCOMPARE(executed, 1);
			QCOMPARE(depth, 0);
		}

		void executeWithWorldErrorRoutingPluginScriptSkipsPushPop()
		{
			bool enterCalled = false;
			bool exitCalled  = false;
			bool executed    = false;

			QMudScriptErrorRouting::executeWithWorldErrorRouting(
			    true, true, [&] { executed = true; }, [&] { enterCalled = true; },
			    [&] { exitCalled = true; });

			QVERIFY(executed);
			QVERIFY(!enterCalled);
			QVERIFY(!exitCalled);
		}

		void executeWithWorldErrorRoutingDeferredAndDirectContextsUseSameOrdering()
		{
			int depth      = 0;
			int enterCount = 0;
			int exitCount  = 0;

			{
				QMudScriptErrorRouting::executeWithWorldErrorRouting(
				    true, false,
				    [&]
				    {
					    QCOMPARE(depth, 1);
					    QMudScriptErrorRouting::executeWithWorldErrorRouting(
					        true, false, [&] { QCOMPARE(depth, 2); },
					        [&]
					        {
						        ++enterCount;
						        ++depth;
					        },
					        [&]
					        {
						        ++exitCount;
						        --depth;
					        });
					    QCOMPARE(depth, 1);
				    },
				    [&]
				    {
					    ++enterCount;
					    ++depth;
				    },
				    [&]
				    {
					    ++exitCount;
					    --depth;
				    });
				QCOMPARE(depth, 0);
			}

			QCOMPARE(enterCount, 2);
			QCOMPARE(exitCount, 2);
			QCOMPARE(depth, 0);
		}

		void executeWithWorldErrorRoutingNoRuntimeStillExecutesBody()
		{
			bool executed = false;
			QMudScriptErrorRouting::executeWithWorldErrorRouting(
			    false, false, [&] { executed = true; }, [] {}, [] {});
			QVERIFY(executed);
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_ScriptErrorRoutingUtils)

#if __has_include("tst_ScriptErrorRoutingUtils.moc")
#include "tst_ScriptErrorRoutingUtils.moc"
#endif
