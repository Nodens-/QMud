/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_TraceDispatchUtils.cpp
 * Role: Unit coverage for shared trace-dispatch behavior used by runtime and Lua trace paths.
 */

#include "TraceDispatchUtils.h"

#include <QtTest/QTest>

namespace
{
	struct TraceHarness
	{
			bool        traceEnabled{true};
			int         pluginCalls{0};
			bool        traceEnabledDuringPluginCall{true};
			bool        pluginHandles{false};
			QStringList pluginMessages;
			QStringList outputLines;
	};

	QMudTraceDispatch::Callbacks makeCallbacks(TraceHarness &harness)
	{
		return QMudTraceDispatch::Callbacks{
		    [&harness]() { return harness.traceEnabled; },
		    [&harness](const bool enabled) { harness.traceEnabled = enabled; },
		    [&harness](const QString &message)
		    {
			    harness.pluginCalls++;
			    harness.traceEnabledDuringPluginCall = harness.traceEnabled;
			    harness.pluginMessages.push_back(message);
			    return harness.pluginHandles;
		    },
		    [&harness](const QString &line) { harness.outputLines.push_back(line); }};
	}
} // namespace

/**
 * @brief QTest fixture covering shared trace dispatch behavior.
 */
class tst_TraceDispatchUtils : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void traceDisabledDoesNotDispatch()
		{
			TraceHarness harness;
			harness.traceEnabled = false;

			QMudTraceDispatch::emitTrace(QStringLiteral("hello"), makeCallbacks(harness));

			QCOMPARE(harness.pluginCalls, 0);
			QVERIFY(harness.pluginMessages.isEmpty());
			QVERIFY(harness.outputLines.isEmpty());
			QVERIFY(!harness.traceEnabled);
		}

		void pluginHandledTraceSuppressesFallbackOutput()
		{
			TraceHarness harness;
			harness.pluginHandles = true;

			QMudTraceDispatch::emitTrace(QStringLiteral("matched trigger"), makeCallbacks(harness));

			QCOMPARE(harness.pluginCalls, 1);
			QCOMPARE(harness.pluginMessages, QStringList{QStringLiteral("matched trigger")});
			QVERIFY(!harness.traceEnabledDuringPluginCall);
			QVERIFY(harness.outputLines.isEmpty());
			QVERIFY(harness.traceEnabled);
		}

		void unhandledTraceFallsBackToOutput()
		{
			TraceHarness harness;
			harness.pluginHandles = false;

			QMudTraceDispatch::emitTrace(QStringLiteral("matched alias"), makeCallbacks(harness));

			QCOMPARE(harness.pluginCalls, 1);
			QCOMPARE(harness.pluginMessages, QStringList{QStringLiteral("matched alias")});
			QVERIFY(!harness.traceEnabledDuringPluginCall);
			QCOMPARE(harness.outputLines, QStringList{QStringLiteral("TRACE: matched alias")});
			QVERIFY(harness.traceEnabled);
		}

		void emptyTraceMessageIsIgnored()
		{
			TraceHarness harness;

			QMudTraceDispatch::emitTrace(QString(), makeCallbacks(harness));

			QCOMPARE(harness.pluginCalls, 0);
			QVERIFY(harness.pluginMessages.isEmpty());
			QVERIFY(harness.outputLines.isEmpty());
			QVERIFY(harness.traceEnabled);
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_TraceDispatchUtils)

#if __has_include("tst_TraceDispatchUtils.moc")
#include "tst_TraceDispatchUtils.moc"
#endif
