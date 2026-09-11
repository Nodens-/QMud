/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_SingleInstanceIpc.cpp
 * Role: Verifies framed and acknowledged request delivery between QMud instances.
 */

#include "SingleInstanceIpc.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QCoreApplication>
#include <QDeadlineTimer>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QEventLoop>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
// ReSharper disable once CppUnusedIncludeDirective
#include <QPointer>
#include <QProcess>
#include <QScopeGuard>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QtEndian>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace
{
	/** @brief Returns a unique-directory template rooted beside the test executable. */
	QString socketDirectoryTemplate()
	{
		return QDir(QCoreApplication::applicationDirPath())
		    .filePath(QStringLiteral("tst_SingleInstanceIpc-XXXXXX"));
	}

	/** @brief Writes one deliberate protocol fragment and waits until Qt has flushed it. */
	bool writeFragment(QLocalSocket &socket, const QByteArray &fragment)
	{
		if (socket.write(fragment) != fragment.size())
			return false;
		return socket.bytesToWrite() == 0 || socket.waitForBytesWritten(1000);
	}

	/** @brief Converts a shared finite deadline to the remaining interval expected by QProcess. */
	int remainingWaitTime(const QDeadlineTimer &deadline)
	{
		return static_cast<int>(std::max<qint64>(0, deadline.remainingTime()));
	}

	/** @brief Runs one independently synchronized contender for the startup-race integration test. */
	int runStartupRaceHelper(const QString &serverName)
	{
		QTextStream output(stdout, QIODevice::WriteOnly);
		QTextStream input(stdin, QIODevice::ReadOnly);
		output << "READY" << Qt::endl;
		if (input.readLine() != QLatin1String("GO"))
			return 2;

		QLocalServer                                     server;
		QString                                          errorMessage;
		const QMudSingleInstanceIpc::ServerStartupResult result = QMudSingleInstanceIpc::startServerOrForward(
		    server, serverName, QMudSingleInstanceIpc::Request{}, 1000, 3000, 3000, &errorMessage);
		if (result == QMudSingleInstanceIpc::ServerStartupResult::Forwarded)
		{
			output << "FORWARDED" << Qt::endl;
			return 0;
		}
		if (result == QMudSingleInstanceIpc::ServerStartupResult::Failed)
		{
			QTextStream(stderr, QIODevice::WriteOnly) << errorMessage << Qt::endl;
			return 3;
		}

		bool       requestReceived{false};
		QEventLoop eventLoop;
		QObject::connect(&server, &QLocalServer::newConnection, &eventLoop,
		                 [&server, &eventLoop, &requestReceived]
		                 {
			                 while (QLocalSocket *socket = server.nextPendingConnection())
			                 {
				                 QMudSingleInstanceIpc::receiveRequest(
				                     socket,
				                     [&eventLoop, &requestReceived](const auto &)
				                     {
					                     requestReceived = true;
					                     eventLoop.quit();
				                     });
			                 }
		                 });
		eventLoop.exec();
		if (!requestReceived)
			return 4;
		output << "LISTENING" << Qt::endl;
		return 0;
	}

	/** @brief Starts one startup-race helper that waits for the parent's protocol barrier. */
	void startRaceHelper(QProcess &process, const QString &serverName)
	{
		process.setProgram(QCoreApplication::applicationFilePath());
		process.setArguments({QStringLiteral("--startup-race-helper"), serverName});
		process.start();
	}

	/** @brief Waits for one helper's READY message within a deadline shared by all helpers. */
	bool waitForRaceHelperReady(QProcess &process, const QDeadlineTimer &deadline)
	{
		if (process.state() == QProcess::Starting && !process.waitForStarted(remainingWaitTime(deadline)))
			return false;
		if (process.state() != QProcess::Running)
			return false;
		while (!process.canReadLine())
		{
			if (deadline.hasExpired() || !process.waitForReadyRead(remainingWaitTime(deadline)))
				return false;
		}
		return process.readLine().trimmed() == QByteArrayLiteral("READY");
	}

	/** @brief Waits for one concurrently running helper within the shared completion deadline. */
	bool waitForRaceHelperFinished(QProcess &process, const QDeadlineTimer &deadline)
	{
		return process.state() == QProcess::NotRunning ||
		       (!deadline.hasExpired() && process.waitForFinished(remainingWaitTime(deadline)));
	}

	/**
	 * @brief Integration fixture for the single-instance local-socket protocol.
	 */
	class tst_SingleInstanceIpc final : public QObject
	{
			Q_OBJECT

		private slots:
			/** @brief Supplies every supported request action to acknowledged-delivery coverage. */
			static void deliversAndAcknowledgesCompleteRequest_data();
			/** @brief Verifies complete requests are dispatched and acknowledged. */
			static void deliversAndAcknowledgesCompleteRequest();
			/** @brief Verifies simultaneous processes serialize startup and forward to the first endpoint owner. */
			static void simultaneousProcessesSerializeServerStartup();
			/** @brief Verifies an independently held startup lock prevents endpoint probing and replacement. */
			static void heldStartupLockPreventsEndpointClaim();
			/** @brief Verifies split header and payload fragments are accumulated without early dispatch. */
			static void accumulatesFragmentedRequest();
			/** @brief Verifies buffered complete requests survive an already-observed peer disconnect. */
			static void consumesCompleteFrameAfterPeerDisconnected();
			/** @brief Verifies payload lengths beyond the protocol bound are rejected. */
			static void rejectsOversizedFrame();
			/** @brief Verifies a genuinely absent endpoint is distinguished from delivery failure. */
			static void reportsUnavailableServer();
			/** @brief Supplies missing and invalid acknowledgement responses. */
			static void preservesExistingServerAfterFailedDelivery_data();
			/** @brief Verifies ambiguous delivery failures do not replace the existing server. */
			static void preservesExistingServerAfterFailedDelivery();
	};

	void tst_SingleInstanceIpc::deliversAndAcknowledgesCompleteRequest_data()
	{
		QTest::addColumn<int>("action");
		QTest::addColumn<QString>("path");
		QTest::newRow("raise") << static_cast<int>(QMudSingleInstanceIpc::Action::Raise) << QString();
		QTest::newRow("open-file-association")
		    << static_cast<int>(QMudSingleInstanceIpc::Action::OpenFileAssociation)
		    << QStringLiteral("/home/test/QMud Home/world.qdl");
	}

	void tst_SingleInstanceIpc::deliversAndAcknowledgesCompleteRequest()
	{
		QFETCH(int, action);
		QFETCH(QString, path);
		QTemporaryDir socketDirectory(socketDirectoryTemplate());
		QVERIFY(socketDirectory.isValid());
		const QString serverName = socketDirectory.filePath(QStringLiteral("socket"));
		QLocalServer  server;
		server.setSocketOptions(QLocalServer::UserAccessOption);
		QVERIFY2(server.listen(serverName), qPrintable(server.errorString()));

		bool                           received{false};
		QMudSingleInstanceIpc::Request receivedRequest;
		QPointer<QLocalSocket>         acceptedSocket;
		connect(&server, &QLocalServer::newConnection, &server,
		        [&server, &received, &receivedRequest, &acceptedSocket]
		        {
			        while (QLocalSocket *socket = server.nextPendingConnection())
			        {
				        acceptedSocket = socket;
				        QMudSingleInstanceIpc::receiveRequest(
				            socket,
				            [&received, &receivedRequest](const QMudSingleInstanceIpc::Request &request)
				            {
					            received        = true;
					            receivedRequest = request;
				            });
			        }
		        });

		constexpr int                     requestTimeoutMs = 3000;
		const auto                        expectedAction = static_cast<QMudSingleInstanceIpc::Action>(action);
		std::atomic_bool                  finished{false};
		QMudSingleInstanceIpc::SendResult sendResult = QMudSingleInstanceIpc::SendResult::Failed;
		QString                           errorMessage;
		std::jthread                      sender(
            [&]
            {
                sendResult = QMudSingleInstanceIpc::sendRequest(
                    serverName, {expectedAction, path}, requestTimeoutMs, requestTimeoutMs, &errorMessage);
                finished.store(true, std::memory_order_release);
            });

		QTRY_VERIFY_WITH_TIMEOUT(finished.load(std::memory_order_acquire), 5000);
		sender.join();
		QCOMPARE(sendResult, QMudSingleInstanceIpc::SendResult::Delivered);
		QVERIFY2(errorMessage.isEmpty(), qPrintable(errorMessage));
		QVERIFY(received);
		QCOMPARE(receivedRequest.action, expectedAction);
		QCOMPARE(receivedRequest.fileAssociationPath, path);
		QTRY_VERIFY_WITH_TIMEOUT(acceptedSocket.isNull(), 1000);
	}

	void tst_SingleInstanceIpc::simultaneousProcessesSerializeServerStartup()
	{
		QTemporaryDir socketDirectory(socketDirectoryTemplate());
		QVERIFY(socketDirectory.isValid());
		const QString serverName = socketDirectory.filePath(QStringLiteral("socket"));
		QProcess      firstProcess;
		QProcess      secondProcess;
		const auto    stopProcesses = qScopeGuard(
            [&firstProcess, &secondProcess]
            {
                for (QProcess *process : {&firstProcess, &secondProcess})
                {
                    if (process->state() == QProcess::NotRunning)
                        continue;
                    process->kill();
                    static_cast<void>(process->waitForFinished(1000));
                }
            });
		startRaceHelper(firstProcess, serverName);
		startRaceHelper(secondProcess, serverName);
		const QDeadlineTimer readinessDeadline(3000);
		QVERIFY2(waitForRaceHelperReady(firstProcess, readinessDeadline),
		         qPrintable(firstProcess.errorString()));
		QVERIFY2(waitForRaceHelperReady(secondProcess, readinessDeadline),
		         qPrintable(secondProcess.errorString()));

		QCOMPARE(firstProcess.write(QByteArrayLiteral("GO\n")), 3);
		QCOMPARE(secondProcess.write(QByteArrayLiteral("GO\n")), 3);
		const QDeadlineTimer completionDeadline(5000);
		QVERIFY(firstProcess.bytesToWrite() == 0 ||
		        firstProcess.waitForBytesWritten(remainingWaitTime(completionDeadline)));
		QVERIFY(secondProcess.bytesToWrite() == 0 ||
		        secondProcess.waitForBytesWritten(remainingWaitTime(completionDeadline)));
		QVERIFY2(waitForRaceHelperFinished(firstProcess, completionDeadline),
		         qPrintable(firstProcess.errorString()));
		QVERIFY2(waitForRaceHelperFinished(secondProcess, completionDeadline),
		         qPrintable(secondProcess.errorString()));

		const QByteArray firstError  = firstProcess.readAllStandardError();
		const QByteArray secondError = secondProcess.readAllStandardError();
		QCOMPARE(firstProcess.exitStatus(), QProcess::NormalExit);
		QVERIFY2(firstProcess.exitCode() == 0, firstError.constData());
		QCOMPARE(secondProcess.exitStatus(), QProcess::NormalExit);
		QVERIFY2(secondProcess.exitCode() == 0, secondError.constData());
		const QSet<QByteArray> outcomes = {firstProcess.readAllStandardOutput().trimmed(),
		                                   secondProcess.readAllStandardOutput().trimmed()};
		QCOMPARE(outcomes,
		         QSet<QByteArray>({QByteArrayLiteral("LISTENING"), QByteArrayLiteral("FORWARDED")}));
	}

	void tst_SingleInstanceIpc::heldStartupLockPreventsEndpointClaim()
	{
		QTemporaryDir socketDirectory(socketDirectoryTemplate());
		QVERIFY(socketDirectory.isValid());
		const QString serverName = socketDirectory.filePath(QStringLiteral("socket"));
		QLockFile     heldLock(QMudSingleInstanceIpc::serverStartupLockFilePath(serverName));
		QVERIFY(heldLock.tryLock(std::chrono::milliseconds::zero()));

		QLocalServer contender;
		QString      errorMessage;
		QCOMPARE(QMudSingleInstanceIpc::startServerOrForward(
		             contender, serverName, QMudSingleInstanceIpc::Request{}, 100, 100, 0, &errorMessage),
		         QMudSingleInstanceIpc::ServerStartupResult::Failed);
		QVERIFY(!contender.isListening());
		QVERIFY(!errorMessage.isEmpty());
	}

	void tst_SingleInstanceIpc::accumulatesFragmentedRequest()
	{
		QTemporaryDir socketDirectory(socketDirectoryTemplate());
		QVERIFY(socketDirectory.isValid());
		const QString serverName = socketDirectory.filePath(QStringLiteral("socket"));
		QLocalServer  server;
		server.setSocketOptions(QLocalServer::UserAccessOption);
		QVERIFY2(server.listen(serverName), qPrintable(server.errorString()));

		QLocalSocket client;
		client.connectToServer(serverName, QIODevice::ReadWrite);
		QVERIFY2(client.waitForConnected(1000), qPrintable(client.errorString()));
		QVERIFY(server.waitForNewConnection(1000));
		QLocalSocket *accepted = server.nextPendingConnection();
		QVERIFY(accepted);
		QPointer<QLocalSocket>         acceptedSocket = accepted;

		int                            receivedCount{0};
		QMudSingleInstanceIpc::Request receivedRequest;
		QSignalSpy                     readyReadSpy(accepted, &QLocalSocket::readyRead);
		const QString                  expectedPath = QStringLiteral("inside/QMud Home/world.qdl");
		const QByteArray               frame        = QMudSingleInstanceIpc::encodeRequestFrame(
            {QMudSingleInstanceIpc::Action::OpenFileAssociation, expectedPath});
		QMudSingleInstanceIpc::receiveRequest(accepted,
		                                      [&receivedCount, &receivedRequest](const auto &request)
		                                      {
			                                      ++receivedCount;
			                                      receivedRequest = request;
		                                      });

		QVERIFY(writeFragment(client, frame.first(2)));
		QTRY_VERIFY_WITH_TIMEOUT(readyReadSpy.count() > 0, 1000);
		QCOMPARE(receivedCount, 0);
		readyReadSpy.clear();
		QVERIFY(writeFragment(client, frame.sliced(2, 2)));
		QTRY_VERIFY_WITH_TIMEOUT(readyReadSpy.count() > 0, 1000);
		QCOMPARE(receivedCount, 0);
		readyReadSpy.clear();
		constexpr qsizetype headerSize       = sizeof(quint32);
		const qsizetype     payloadSplit     = headerSize + ((frame.size() - headerSize) / 2);
		const qsizetype     firstPayloadSize = payloadSplit - headerSize;
		QVERIFY(writeFragment(client, frame.sliced(headerSize, firstPayloadSize)));
		QTRY_VERIFY_WITH_TIMEOUT(readyReadSpy.count() > 0, 1000);
		QCOMPARE(receivedCount, 0);
		readyReadSpy.clear();
		QVERIFY(writeFragment(client, frame.sliced(payloadSplit)));
		QTRY_COMPARE_WITH_TIMEOUT(receivedCount, 1, 1000);
		QCOMPARE(receivedRequest.action, QMudSingleInstanceIpc::Action::OpenFileAssociation);
		QCOMPARE(receivedRequest.fileAssociationPath, expectedPath);
		QTRY_VERIFY_WITH_TIMEOUT(acceptedSocket.isNull(), 1000);
	}

	void tst_SingleInstanceIpc::consumesCompleteFrameAfterPeerDisconnected()
	{
		QTemporaryDir socketDirectory(socketDirectoryTemplate());
		QVERIFY(socketDirectory.isValid());
		const QString serverName = socketDirectory.filePath(QStringLiteral("socket"));
		QLocalServer  server;
		server.setSocketOptions(QLocalServer::UserAccessOption);
		QVERIFY2(server.listen(serverName), qPrintable(server.errorString()));

		QLocalSocket client;
		client.connectToServer(serverName, QIODevice::ReadWrite);
		QVERIFY2(client.waitForConnected(1000), qPrintable(client.errorString()));
		QVERIFY(server.waitForNewConnection(1000));
		QLocalSocket *accepted = server.nextPendingConnection();
		QVERIFY(accepted);
		QPointer<QLocalSocket> acceptedSocket = accepted;

		const QString          expectedPath = QStringLiteral("/home/test/QMud Home/world.qdl");
		const QByteArray       frame        = QMudSingleInstanceIpc::encodeRequestFrame(
            {QMudSingleInstanceIpc::Action::OpenFileAssociation, expectedPath});
		QCOMPARE(client.write(frame), frame.size());
		if (client.bytesToWrite() > 0)
			QVERIFY(client.waitForBytesWritten(1000));
		client.disconnectFromServer();
		if (client.state() != QLocalSocket::UnconnectedState)
			QVERIFY(client.waitForDisconnected(1000));
		QTRY_COMPARE_WITH_TIMEOUT(accepted->state(), QLocalSocket::UnconnectedState, 1000);

		QString receivedPath;
		QMudSingleInstanceIpc::receiveRequest(accepted,
		                                      [&receivedPath](const QMudSingleInstanceIpc::Request &request)
		                                      {
			                                      if (request.action ==
			                                          QMudSingleInstanceIpc::Action::OpenFileAssociation)
				                                      receivedPath = request.fileAssociationPath;
		                                      });
		QCOMPARE(receivedPath, expectedPath);
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		QVERIFY(acceptedSocket.isNull());
	}

	void tst_SingleInstanceIpc::rejectsOversizedFrame()
	{
		QTemporaryDir socketDirectory(socketDirectoryTemplate());
		QVERIFY(socketDirectory.isValid());
		const QString serverName = socketDirectory.filePath(QStringLiteral("socket"));
		QLocalServer  server;
		server.setSocketOptions(QLocalServer::UserAccessOption);
		QVERIFY2(server.listen(serverName), qPrintable(server.errorString()));

		QLocalSocket client;
		client.connectToServer(serverName, QIODevice::ReadWrite);
		QVERIFY2(client.waitForConnected(1000), qPrintable(client.errorString()));
		QVERIFY(server.waitForNewConnection(1000));
		QLocalSocket *accepted = server.nextPendingConnection();
		QVERIFY(accepted);
		QPointer<QLocalSocket> acceptedSocket = accepted;

		bool                   handled{false};
		QMudSingleInstanceIpc::receiveRequest(accepted, [&handled](const auto &) { handled = true; });
		QByteArray oversizedHeader(sizeof(quint32), Qt::Uninitialized);
		qToBigEndian<quint32>(QMudSingleInstanceIpc::kMaximumPayloadSize + 1U,
		                      reinterpret_cast<uchar *>(oversizedHeader.data()));
		QVERIFY(writeFragment(client, oversizedHeader));
		QTRY_COMPARE_WITH_TIMEOUT(client.state(), QLocalSocket::UnconnectedState, 1000);
		QVERIFY(!handled);
		QTRY_VERIFY_WITH_TIMEOUT(acceptedSocket.isNull(), 1000);
	}

	void tst_SingleInstanceIpc::reportsUnavailableServer()
	{
		QTemporaryDir socketDirectory(socketDirectoryTemplate());
		QVERIFY(socketDirectory.isValid());
		const QString serverName = socketDirectory.filePath(QStringLiteral("absent-socket"));
		QString       errorMessage;
		QCOMPARE(QMudSingleInstanceIpc::sendRequest(serverName, {}, 100, 100, &errorMessage),
		         QMudSingleInstanceIpc::SendResult::ServerUnavailable);
		QVERIFY(errorMessage.isEmpty());
	}

	void tst_SingleInstanceIpc::preservesExistingServerAfterFailedDelivery_data()
	{
		QTest::addColumn<QByteArray>("acknowledgement");
		QTest::newRow("missing-acknowledgement") << QByteArray();
		QTest::newRow("invalid-acknowledgement") << QByteArrayLiteral("QMUD-BAD-1");
	}

	void tst_SingleInstanceIpc::preservesExistingServerAfterFailedDelivery()
	{
		QFETCH(QByteArray, acknowledgement);
		QTemporaryDir socketDirectory(socketDirectoryTemplate());
		QVERIFY(socketDirectory.isValid());
		const QString serverName = socketDirectory.filePath(QStringLiteral("socket"));
		QLocalServer  existingServer;
		existingServer.setSocketOptions(QLocalServer::UserAccessOption);
		QVERIFY2(existingServer.listen(serverName), qPrintable(existingServer.errorString()));

		QPointer<QLocalSocket> acceptedSocket;
		connect(&existingServer, &QLocalServer::newConnection, &existingServer,
		        [&existingServer, &acceptedSocket, acknowledgement]
		        {
			        while (QLocalSocket *socket = existingServer.nextPendingConnection())
			        {
				        acceptedSocket = socket;
				        connect(socket, &QLocalSocket::readyRead, socket,
				                [socket, acknowledgement]
				                {
					                socket->readAll();
					                if (!acknowledgement.isNull())
					                {
						                socket->write(acknowledgement);
						                socket->flush();
					                }
					                socket->disconnectFromServer();
				                });
				        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
			        }
		        });

		std::atomic_bool                           finished{false};
		QMudSingleInstanceIpc::ServerStartupResult startupResult =
		    QMudSingleInstanceIpc::ServerStartupResult::Listening;
		QString      errorMessage;
		std::jthread competingProcess(
		    [&]
		    {
			    QLocalServer competingServer;
			    startupResult = QMudSingleInstanceIpc::startServerOrForward(competingServer, serverName,
			                                                                QMudSingleInstanceIpc::Request{},
			                                                                1000, 3000, 5000, &errorMessage);
			    finished.store(true, std::memory_order_release);
		    });

		QTRY_VERIFY_WITH_TIMEOUT(finished.load(std::memory_order_acquire), 5000);
		competingProcess.join();
		QCOMPARE(startupResult, QMudSingleInstanceIpc::ServerStartupResult::Failed);
		QVERIFY(!errorMessage.isEmpty());
		QVERIFY(existingServer.isListening());
		QTRY_VERIFY_WITH_TIMEOUT(acceptedSocket.isNull(), 1000);
	}

} // namespace

int main(int argc, char **argv)
{
	QCoreApplication  application(argc, argv);
	const QStringList arguments = QCoreApplication::arguments();
	if (arguments.size() == 3 && arguments.at(1) == QLatin1String("--startup-race-helper"))
		return runStartupRaceHelper(arguments.at(2));

	tst_SingleInstanceIpc test;
	return QTest::qExec(&test, argc, argv);
}

#include "tst_SingleInstanceIpc.moc"
