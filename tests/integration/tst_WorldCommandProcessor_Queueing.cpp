/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldCommandProcessor_Queueing.cpp
 * Role: QTest coverage for WorldCommandProcessor Queueing behavior.
 */

#include "WorldCommandProcessorUtils.h"

#include <QtTest/QTest>

/**
 * @brief QTest fixture covering WorldCommandProcessor Queueing scenarios.
 */
class tst_WorldCommandProcessor_Queueing : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void queueDecision_data()
		{
			QTest::addColumn<int>("speedWalkDelayMs");
			QTest::addColumn<bool>("queueRequested");
			QTest::addColumn<bool>("queueNotEmpty");
			QTest::addColumn<bool>("expected");

			QTest::newRow("no-delay") << 0 << true << true << false;
			QTest::newRow("queue-requested") << 10 << true << false << true;
			QTest::newRow("already-has-queued") << 10 << false << true << true;
			QTest::newRow("nothing-to-queue") << 10 << false << false << false;
		}

		void queueDecision()
		{
			QFETCH(int, speedWalkDelayMs);
			QFETCH(bool, queueRequested);
			QFETCH(bool, queueNotEmpty);
			QFETCH(bool, expected);

			QCOMPARE(QMudCommandQueue::shouldQueueCommand(speedWalkDelayMs, queueRequested, queueNotEmpty),
			         expected);
		}

		void queueEntryEncoding_data()
		{
			QTest::addColumn<bool>("queueRequested");
			QTest::addColumn<bool>("echo");
			QTest::addColumn<bool>("logIt");
			QTest::addColumn<QString>("expected");

			QTest::newRow("queued-echo-log") << true << true << true << QStringLiteral("Ecmd");
			QTest::newRow("queued-echo-no-log") << true << true << false << QStringLiteral("ecmd");
			QTest::newRow("queued-no-echo") << true << false << true << QStringLiteral("ecmd");
			QTest::newRow("immediate-echo-log") << false << true << true << QStringLiteral("Icmd");
			QTest::newRow("immediate-echo-no-log") << false << true << false << QStringLiteral("icmd");
			QTest::newRow("immediate-no-echo") << false << false << true << QStringLiteral("icmd");
		}

		void queueEntryEncoding()
		{
			QFETCH(bool, queueRequested);
			QFETCH(bool, echo);
			QFETCH(bool, logIt);
			QFETCH(QString, expected);

			QCOMPARE(QMudCommandQueue::encodeQueueEntry(QStringLiteral("cmd"), queueRequested, echo, logIt),
			         expected);
		}

		void queueEntryDecoding_data()
		{
			QTest::addColumn<QString>("entry");
			QTest::addColumn<bool>("withEcho");
			QTest::addColumn<bool>("logIt");
			QTest::addColumn<bool>("queuedType");
			QTest::addColumn<QString>("payload");

			QTest::newRow("queued-echo-log")
			    << QStringLiteral("Ego") << true << true << true << QStringLiteral("go");
			QTest::newRow("queued-no-log")
			    << QStringLiteral("ego") << true << false << true << QStringLiteral("go");
			QTest::newRow("immediate-echo-log")
			    << QStringLiteral("Igo") << true << true << false << QStringLiteral("go");
			QTest::newRow("immediate-no-log")
			    << QStringLiteral("igo") << true << false << false << QStringLiteral("go");
			QTest::newRow("unknown-flag-falls-back")
			    << QStringLiteral("Xgo") << false << true << false << QStringLiteral("go");
			QTest::newRow("empty-entry") << QString() << false << false << false << QString();
		}

		void queueEntryDecoding()
		{
			QFETCH(QString, entry);
			QFETCH(bool, withEcho);
			QFETCH(bool, logIt);
			QFETCH(bool, queuedType);
			QFETCH(QString, payload);

			const QMudCommandQueue::QueueEntry decoded = QMudCommandQueue::decodeQueueEntry(entry);
			QCOMPARE(decoded.withEcho, withEcho);
			QCOMPARE(decoded.logIt, logIt);
			QCOMPARE(decoded.queuedType, queuedType);
			QCOMPARE(decoded.payload, payload);
		}

		void takeDispatchBatchStopsAtQueuedType()
		{
			QStringList       queue = {QStringLiteral("Ifirst"), QStringLiteral("Esecond"),
			                           QStringLiteral("ithird")};
			const QStringList batch = QMudCommandQueue::takeDispatchBatch(queue, false);
			QCOMPARE(batch, (QStringList{QStringLiteral("Ifirst"), QStringLiteral("Esecond")}));
			QCOMPARE(queue, (QStringList{QStringLiteral("ithird")}));
		}

		void takeDispatchBatchFlushAllConsumesEverything()
		{
			QStringList       queue = {QString(), QStringLiteral("ifirst"), QStringLiteral("esecond"),
			                           QStringLiteral("Ithird")};
			const QStringList batch = QMudCommandQueue::takeDispatchBatch(queue, true);
			QCOMPARE(batch, (QStringList{QStringLiteral("ifirst"), QStringLiteral("esecond"),
			                             QStringLiteral("Ithird")}));
			QVERIFY(queue.isEmpty());
		}

		void discardClearsQueuedCommands()
		{
			QStringList queue = {QStringLiteral("Eone"), QStringLiteral("Itwo")};
			QCOMPARE(QMudCommandQueue::discardAll(queue), 2);
			QVERIFY(queue.isEmpty());
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_WorldCommandProcessor_Queueing)

#if __has_include("tst_WorldCommandProcessor_Queueing.moc")
#include "tst_WorldCommandProcessor_Queueing.moc"
#endif
