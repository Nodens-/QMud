/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_LuaBridgeDispatcherIntegration.cpp
 * Role: Integration coverage for Lua bridge dispatcher timeout and cancellation behavior.
 */

#include "helpers/LuaExecutionUtils.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QCoreApplication>
#include <QElapsedTimer>
// ReSharper disable once CppUnusedIncludeDirective
#include <QFile>
#include <QMetaObject>
// ReSharper disable once CppUnusedIncludeDirective
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryDir>
#include <QTextStream>
// ReSharper disable once CppUnusedIncludeDirective
#include <QThread>
#include <QtTest/QTest>

#include <atomic>

namespace
{
	constexpr int    kBridgeTestInvokeTimeoutMs = 250;
	QString          g_fatalLogPath;
	QtMessageHandler g_prevMessageHandler{nullptr};

	void             bridgeFatalChildMessageHandler(const QtMsgType type, const QMessageLogContext &context,
	                                                const QString &message)
	{
		if (!g_fatalLogPath.isEmpty())
		{
			QFile file(g_fatalLogPath);
			if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
			{
				QTextStream stream(&file);
				stream << message << '\n';
			}
		}
		if (g_prevMessageHandler)
			g_prevMessageHandler(type, context, message);
	}

	int runBridgeInFlightTimeoutChild()
	{
		qmudSetLuaBridgeInvokeTimeoutMs(kBridgeTestInvokeTimeoutMs);
		g_fatalLogPath = qEnvironmentVariable("QMUD_BRIDGE_FATAL_LOG");
		if (!g_fatalLogPath.isEmpty())
			g_prevMessageHandler = qInstallMessageHandler(bridgeFatalChildMessageHandler);

		QThread workerThread;
		workerThread.setObjectName(QStringLiteral("tst_LuaBridgeFatalChildWorker"));
		QObject  target;
		QThread *mainThread = QThread::currentThread();
		target.moveToThread(&workerThread);
		workerThread.start();
		const auto stopWorker = qScopeGuard(
		    [&]()
		    {
			    if (target.thread() == &workerThread)
			    {
				    static_cast<void>(qmudLuaBridgeInvokeOnObjectThread(
				        &target, [&target, mainThread] { target.moveToThread(mainThread); }));
			    }
			    workerThread.quit();
			    static_cast<void>(workerThread.wait());
		    });

		if (!qmudLuaBridgeEnsureObjectThreadReady(&target))
		{
			qWarning().noquote()
			    << QStringLiteral("Fatal-timeout child setup failed: %1").arg(qmudLuaBridgeLastError());
			return 20;
		}

		QElapsedTimer timer;
		timer.start();
		if (!qmudLuaBridgeInvokeOnObjectThread(&target, []() { QThread::sleep(1); }))
		{
			qWarning().noquote()
			    << QStringLiteral(
			           "Expected in-flight invoke success in child path, but invoke failed unexpectedly: %1")
			           .arg(qmudLuaBridgeLastError());
			return 21;
		}
		const qint64 elapsedMs = timer.elapsed();
		if (elapsedMs < 900)
		{
			qWarning().noquote()
			    << QStringLiteral("In-flight invoke returned too quickly; expected execution wait, got %1 ms")
			           .arg(elapsedMs);
			return 23;
		}
		if (qmudLuaBridgeLastStatus() != LuaBridgeInvokeStatus::Success)
		{
			qWarning().noquote() << QStringLiteral("Unexpected in-flight invoke status: %1")
			                            .arg(static_cast<int>(qmudLuaBridgeLastStatus()));
			return 22;
		}
		return 0;
	}

	/**
	 * @brief QTest fixture covering bridge dispatcher timeout behavior.
	 */
	class tst_LuaBridgeDispatcherIntegration : public QObject
	{
			Q_OBJECT

			// NOLINTBEGIN(readability-convert-member-functions-to-static)
		private slots:
			void init()
			{
				qmudSetLuaBridgeInvokeTimeoutMs(30000);
			}

			void cleanup()
			{
				qmudSetLuaBridgeInvokeTimeoutMs(30000);
			}

			void sameThreadInvokeRunsInlineAndSucceeds()
			{
				QObject          target;
				std::atomic_bool invoked{false};
				QVERIFY(qmudLuaBridgeInvokeOnObjectThread(&target, [&]() { invoked.store(true); }));
				QVERIFY(invoked.load());
				QCOMPARE(qmudLuaBridgeLastStatus(), LuaBridgeInvokeStatus::Success);
			}

			void reentrantBridgeCycleCompletes()
			{
				QThread workerThread;
				workerThread.setObjectName(QStringLiteral("tst_LuaBridgeReentrantWorker"));
				QObject  workerTarget;
				QObject  mainTarget;
				QThread *mainThread = QThread::currentThread();
				workerTarget.moveToThread(&workerThread);
				workerThread.start();
				const auto stopWorker = qScopeGuard(
				    [&]()
				    {
					    if (workerTarget.thread() == &workerThread)
					    {
						    static_cast<void>(qmudLuaBridgeInvokeOnObjectThread(
						        &workerTarget,
						        [&workerTarget, mainThread] { workerTarget.moveToThread(mainThread); }));
					    }
					    workerThread.quit();
					    static_cast<void>(workerThread.wait());
				    });

				QVERIFY(qmudLuaBridgeEnsureObjectThreadReady(&workerTarget));
				QVERIFY(qmudLuaBridgeEnsureObjectThreadReady(&mainTarget));

				std::atomic_bool workerCallbackInvoked{false};
				std::atomic_bool mainCallbackInvoked{false};
				const bool       outerOk = qmudLuaBridgeInvokeOnObjectThread(
				    &workerTarget,
				    [&]()
				    {
					    const bool innerOk = qmudLuaBridgeInvokeOnObjectThread(
					        &mainTarget, [&]() { mainCallbackInvoked.store(true); });
					    if (innerOk)
						    workerCallbackInvoked.store(true);
				    });

				QVERIFY(outerOk);
				QVERIFY(workerCallbackInvoked.load());
				QVERIFY(mainCallbackInvoked.load());
				QCOMPARE(qmudLuaBridgeLastStatus(), LuaBridgeInvokeStatus::Success);
			}

			void queuedInvokeTimeoutCancelsRequest()
			{
				qmudSetLuaBridgeInvokeTimeoutMs(kBridgeTestInvokeTimeoutMs);
				QThread workerThread;
				workerThread.setObjectName(QStringLiteral("tst_LuaBridgeTimeoutWorker"));
				QObject  target;
				QObject  blocker;
				QThread *mainThread = QThread::currentThread();
				target.moveToThread(&workerThread);
				blocker.moveToThread(&workerThread);
				workerThread.start();
				const auto stopWorker = qScopeGuard(
				    [&]()
				    {
					    QElapsedTimer deadline;
					    deadline.start();
					    while (target.thread() == &workerThread && deadline.elapsed() < 5000)
					    {
						    if (qmudLuaBridgeInvokeOnObjectThread(&target, [&target, mainThread]
						                                          { target.moveToThread(mainThread); }))
						    {
							    break;
						    }
						    QThread::msleep(25);
					    }
					    workerThread.quit();
					    static_cast<void>(workerThread.wait());
				    });

				QVERIFY(qmudLuaBridgeEnsureObjectThreadReady(&target));

				std::atomic_bool blockerEntered{false};
				QVERIFY(QMetaObject::invokeMethod(
				    &blocker,
				    [&]()
				    {
					    blockerEntered.store(true);
					    QThread::sleep(1);
				    },
				    Qt::QueuedConnection));
				QTRY_VERIFY_WITH_TIMEOUT(blockerEntered.load(), 2000);

				std::atomic_bool callbackInvoked{false};
				QVERIFY(!qmudLuaBridgeInvokeOnObjectThread(&target, [&]() { callbackInvoked.store(true); }));
				QCOMPARE(qmudLuaBridgeLastStatus(), LuaBridgeInvokeStatus::RequestCanceledBeforeDispatch);
				QVERIFY(qmudLuaBridgeLastError().contains(
				    QStringLiteral("timed out and canceled queued request")));
				QVERIFY(!callbackInvoked.load());
			}

			void inFlightRequestTimeoutWaitsForCompletion()
			{
				qmudSetLuaBridgeInvokeTimeoutMs(kBridgeTestInvokeTimeoutMs);
				QTemporaryDir tempDir;
				QVERIFY(tempDir.isValid());
				const QString logPath = tempDir.filePath(QStringLiteral("bridge_fatal.log"));

				QProcess      process;
				process.setProgram(QCoreApplication::applicationFilePath());
				process.setArguments({QStringLiteral("--bridge-inflight-timeout-child")});
				QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
				env.insert(QStringLiteral("QMUD_BRIDGE_FATAL_LOG"), logPath);
				process.setProcessEnvironment(env);
				process.start();
				QVERIFY(process.waitForStarted());
				QVERIFY(process.waitForFinished(10000));

				QFile      logFile(logPath);
				QByteArray fatalLog;
				if (logFile.open(QIODevice::ReadOnly | QIODevice::Text))
					fatalLog = logFile.readAll();

				const QByteArray output =
				    process.readAllStandardError() + process.readAllStandardOutput() + fatalLog;
				QCOMPARE(process.exitStatus(), QProcess::NormalExit);
				QCOMPARE(process.exitCode(), 0);
				QVERIFY(!output.contains(
				    QStringLiteral("Invoke timed out after %1 ms while request was already executing")
				        .arg(kBridgeTestInvokeTimeoutMs)
				        .toUtf8()));
			}

			void queuedInvokeThroughputStaysWithinBudget()
			{
				QThread workerThread;
				workerThread.setObjectName(QStringLiteral("tst_LuaBridgeThroughputWorker"));
				QObject  target;
				QThread *mainThread = QThread::currentThread();
				target.moveToThread(&workerThread);
				workerThread.start();
				const auto stopWorker = qScopeGuard(
				    [&]()
				    {
					    if (target.thread() == &workerThread)
					    {
						    static_cast<void>(qmudLuaBridgeInvokeOnObjectThread(
						        &target, [&target, mainThread] { target.moveToThread(mainThread); }));
					    }
					    workerThread.quit();
					    static_cast<void>(workerThread.wait());
				    });

				QVERIFY(qmudLuaBridgeEnsureObjectThreadReady(&target));

				constexpr int invokeCount = 1000;
				int           executed    = 0;
				QElapsedTimer timer;
				timer.start();
				for (int i = 0; i < invokeCount; ++i)
				{
					QVERIFY(qmudLuaBridgeInvokeOnObjectThread(&target, [&]() { ++executed; }));
				}
				const qint64 elapsedMs = timer.elapsed();
				QCOMPARE(executed, invokeCount);
				QVERIFY2(
				    elapsedMs < 5000,
				    qPrintable(
				        QStringLiteral("Bridge queued invoke throughput regression: %1 ms").arg(elapsedMs)));
			}

			void nestedInvokeThroughputStaysWithinBudget()
			{
				QThread workerThread;
				workerThread.setObjectName(QStringLiteral("tst_LuaBridgeNestedThroughputWorker"));
				QObject  workerTarget;
				QObject  mainTarget;
				QThread *mainThread = QThread::currentThread();
				workerTarget.moveToThread(&workerThread);
				workerThread.start();
				const auto stopWorker = qScopeGuard(
				    [&]()
				    {
					    if (workerTarget.thread() == &workerThread)
					    {
						    static_cast<void>(qmudLuaBridgeInvokeOnObjectThread(
						        &workerTarget,
						        [&workerTarget, mainThread] { workerTarget.moveToThread(mainThread); }));
					    }
					    workerThread.quit();
					    static_cast<void>(workerThread.wait());
				    });

				QVERIFY(qmudLuaBridgeEnsureObjectThreadReady(&workerTarget));
				QVERIFY(qmudLuaBridgeEnsureObjectThreadReady(&mainTarget));

				constexpr int outerCalls     = 400;
				int           outerRan       = 0;
				int           nestedRan      = 0;
				bool          nestedInvokeOk = true;
				QElapsedTimer timer;
				timer.start();
				for (int i = 0; i < outerCalls; ++i)
				{
					const bool ok = qmudLuaBridgeInvokeOnObjectThread(
					    &workerTarget,
					    [&]()
					    {
						    ++outerRan;
						    if (!qmudLuaBridgeInvokeOnObjectThread(&mainTarget, [&]() { ++nestedRan; }))
							    nestedInvokeOk = false;
					    });
					QVERIFY(ok);
				}
				const qint64 elapsedMs = timer.elapsed();
				QCOMPARE(outerRan, outerCalls);
				QCOMPARE(nestedRan, outerCalls);
				QVERIFY(nestedInvokeOk);
				QVERIFY2(
				    elapsedMs < 7000,
				    qPrintable(
				        QStringLiteral("Bridge nested invoke throughput regression: %1 ms").arg(elapsedMs)));
			}

			void pluginBootstrapStyleBridgeWorkloadStaysWithinBudget()
			{
				QThread workerThread;
				workerThread.setObjectName(QStringLiteral("tst_LuaBridgeBootstrapWorker"));
				QThread *mainThread = QThread::currentThread();
				workerThread.start();
				const auto stopWorker = qScopeGuard(
				    [&]()
				    {
					    workerThread.quit();
					    static_cast<void>(workerThread.wait());
				    });

				constexpr int      objectCount = 250;
				QVector<QObject *> targets;
				targets.reserve(objectCount);
				for (int i = 0; i < objectCount; ++i)
				{
					auto *target = new QObject();
					target->moveToThread(&workerThread);
					targets.push_back(target);
				}
				const auto destroyTargets = qScopeGuard([&]() { qDeleteAll(targets); });
				const auto restoreTargets = qScopeGuard(
				    [&]()
				    {
					    for (QObject *target : targets)
					    {
						    if (!target || target->thread() != &workerThread)
							    continue;
						    static_cast<void>(qmudLuaBridgeInvokeOnObjectThread(
						        target, [target, mainThread] { target->moveToThread(mainThread); }));
					    }
				    });

				QElapsedTimer timer;
				timer.start();
				int executed = 0;
				for (QObject *target : targets)
				{
					QVERIFY(target != nullptr);
					QVERIFY(qmudLuaBridgeEnsureObjectThreadReady(target));
					QVERIFY(qmudLuaBridgeInvokeOnObjectThread(target, [&]() { ++executed; }));
				}
				const qint64 elapsedMs = timer.elapsed();
				QCOMPARE(executed, objectCount);
				QVERIFY2(
				    elapsedMs < 6000,
				    qPrintable(QStringLiteral("Bridge bootstrap workload regression: %1 ms").arg(elapsedMs)));
			}
			// NOLINTEND(readability-convert-member-functions-to-static)
	};

} // namespace

int main(int argc, char **argv)
{
	QCoreApplication  app(argc, argv);
	const QStringList args = QCoreApplication::arguments();
	if (args.contains(QStringLiteral("--bridge-inflight-timeout-child")))
		return runBridgeInFlightTimeoutChild();

	tst_LuaBridgeDispatcherIntegration test;
	return QTest::qExec(&test, argc, argv);
}

#if __has_include("tst_LuaBridgeDispatcherIntegration.moc")
#include "tst_LuaBridgeDispatcherIntegration.moc"
#endif
