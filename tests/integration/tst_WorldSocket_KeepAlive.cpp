/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldSocket_KeepAlive.cpp
 * Role: Integration coverage for world socket keepalive, low-delay behavior, and OS-level tuning.
 */

#include "WorldOptions.h"
#include "WorldSocket.h"

#include <QScopeGuard>
#include <QScopedPointer>
#include <QTcpServer>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#if defined(Q_OS_WIN)
#include <winsock2.h>
#elif defined(Q_OS_UNIX)
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace
{
	/**
	 * @brief Reads an integer socket option value from a native descriptor.
	 * @param descriptor Native socket descriptor.
	 * @param level Socket option protocol level.
	 * @param option Socket option name.
	 * @param value Receives option value on success.
	 * @return `true` when option read succeeds.
	 */
	bool readSocketIntOption(const qintptr descriptor, const int level, const int option, int &value)
	{
#if defined(Q_OS_WIN)
		int  size = sizeof(value);
		auto rc   = ::getsockopt(static_cast<SOCKET>(descriptor), level, option,
		                         reinterpret_cast<char *>(&value), &size);
		return rc == 0 && size == static_cast<int>(sizeof(value));
#elif defined(Q_OS_UNIX)
		socklen_t size = sizeof(value);
		auto      rc   = ::getsockopt(static_cast<int>(descriptor), level, option, &value, &size);
		return rc == 0 && size == static_cast<socklen_t>(sizeof(value));
#else
		Q_UNUSED(descriptor);
		Q_UNUSED(level);
		Q_UNUSED(option);
		Q_UNUSED(value);
		return false;
#endif
	}
	/**
	 * @brief QTest fixture covering `WorldSocket` keepalive and low-delay behavior.
	 */
	class tst_WorldSocket_KeepAlive : public QObject
	{
			Q_OBJECT

			// NOLINTBEGIN(readability-convert-member-functions-to-static)
		private slots:
			/**
			 * @brief Verifies new connections and reconnects disable Nagle independently of keepalive.
			 */
			void enablesLowDelayOnConnectionsAndReconnects()
			{
				QTcpServer server;
				if (!server.listen(QHostAddress::LocalHost, 0))
					QSKIP("Local TCP listen is unavailable in this environment.");

				WorldSocket socket;
				socket.setProxySettings(eProxyServerNone, QString(), 0, QString(), QString());
				QSignalSpy connectedSpy(&socket, &WorldSocket::connected);
				QVERIFY(connectedSpy.isValid());
				for (const bool keepAlive : {false, true})
				{
					socket.setKeepAliveEnabled(keepAlive);
					connectedSpy.clear();
					QVERIFY(socket.connectToHost(QStringLiteral("127.0.0.1"), server.serverPort()));
					QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 5000);
					QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 5000);
					const QScopedPointer<QTcpSocket> peer(server.nextPendingConnection());
					QVERIFY(peer);

					const qintptr descriptor = socket.nativeSocketDescriptor();
					QVERIFY(descriptor >= 0);
					int lowDelay = 0;
					QVERIFY(readSocketIntOption(descriptor, IPPROTO_TCP, TCP_NODELAY, lowDelay));
					QCOMPARE(lowDelay, 1);

					socket.setKeepAliveEnabled(!keepAlive);
					lowDelay = 0;
					QVERIFY(readSocketIntOption(descriptor, IPPROTO_TCP, TCP_NODELAY, lowDelay));
					QCOMPARE(lowDelay, 1);
					socket.abortSocket();
				}
			}

			/**
			 * @brief Verifies reload descriptor duplication and adoption retain the existing low-delay option.
			 */
			void retainsLowDelayThroughDescriptorAdoption()
			{
#if defined(Q_OS_UNIX)
				QTcpServer server;
				if (!server.listen(QHostAddress::LocalHost, 0))
					QSKIP("Local TCP listen is unavailable in this environment.");

				WorldSocket original;
				original.setProxySettings(eProxyServerNone, QString(), 0, QString(), QString());
				original.setKeepAliveEnabled(false);
				QSignalSpy connectedSpy(&original, &WorldSocket::connected);
				QVERIFY(connectedSpy.isValid());
				QVERIFY(original.connectToHost(QStringLiteral("127.0.0.1"), server.serverPort()));
				QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 5000);
				QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 5000);
				const QScopedPointer<QTcpSocket> peer(server.nextPendingConnection());
				QVERIFY(peer);

				int descriptor = ::dup(static_cast<int>(original.nativeSocketDescriptor()));
				QVERIFY(descriptor >= 0);
				const auto closeDescriptor = qScopeGuard(
				    [&]
				    {
					    if (descriptor >= 0)
						    ::close(descriptor);
				    });
				WorldSocket adopted;
				adopted.setKeepAliveEnabled(false);
				QString error;
				QVERIFY2(adopted.adoptConnectedSocketDescriptor(descriptor, &error, {}), qPrintable(error));
				descriptor   = -1;
				int lowDelay = 0;
				QVERIFY(readSocketIntOption(adopted.nativeSocketDescriptor(), IPPROTO_TCP, TCP_NODELAY,
				                            lowDelay));
				QCOMPARE(lowDelay, 1);
#else
				QSKIP("Reload descriptor duplication is exercised on Unix.");
#endif
			}

			/**
			 * @brief Verifies keepalive enable/disable updates the live socket option.
			 */
			void togglesKeepAliveOnConnectedSocket()
			{
				QTcpServer server;
				if (!server.listen(QHostAddress::LocalHost, 0))
					QSKIP("Local TCP listen is unavailable in this environment.");

				WorldSocket socket;
				socket.setProxySettings(eProxyServerNone, QString(), 0, QString(), QString());
				socket.setKeepAliveEnabled(true);

				QSignalSpy serverAcceptedSpy(&server, &QTcpServer::newConnection);
				QVERIFY(serverAcceptedSpy.isValid());
				QSignalSpy connectedSpy(&socket, &WorldSocket::connected);
				QVERIFY(socket.connectToHost(QStringLiteral("127.0.0.1"), server.serverPort()));
				QVERIFY(connectedSpy.wait(5000));
				QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() || serverAcceptedSpy.count() > 0,
				                         5000);
				QVERIFY(server.hasPendingConnections());
				if (QTcpSocket *accepted = server.nextPendingConnection())
					accepted->deleteLater();

				const qintptr descriptor = socket.nativeSocketDescriptor();
				QVERIFY(descriptor >= 0);

				int keepAliveValue = 0;
				QVERIFY(readSocketIntOption(descriptor, SOL_SOCKET, SO_KEEPALIVE, keepAliveValue));
				QCOMPARE(keepAliveValue, 1);

				socket.setKeepAliveEnabled(false);
				keepAliveValue = 1;
				QVERIFY(readSocketIntOption(descriptor, SOL_SOCKET, SO_KEEPALIVE, keepAliveValue));
				QCOMPARE(keepAliveValue, 0);

				socket.abortSocket();
			}

			/**
			 * @brief Verifies MUSHclient-parity keepalive timings where platform supports querying them.
			 */
			void appliesMushclientParityKeepAliveTimings()
			{
				QTcpServer server;
				if (!server.listen(QHostAddress::LocalHost, 0))
					QSKIP("Local TCP listen is unavailable in this environment.");

				WorldSocket socket;
				socket.setProxySettings(eProxyServerNone, QString(), 0, QString(), QString());
				socket.setKeepAliveEnabled(true);

				QSignalSpy serverAcceptedSpy(&server, &QTcpServer::newConnection);
				QVERIFY(serverAcceptedSpy.isValid());
				QSignalSpy connectedSpy(&socket, &WorldSocket::connected);
				QVERIFY(socket.connectToHost(QStringLiteral("127.0.0.1"), server.serverPort()));
				QVERIFY(connectedSpy.wait(5000));
				QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() || serverAcceptedSpy.count() > 0,
				                         5000);
				QVERIFY(server.hasPendingConnections());
				if (QTcpSocket *accepted = server.nextPendingConnection())
					accepted->deleteLater();

				const qintptr descriptor = socket.nativeSocketDescriptor();
				QVERIFY(descriptor >= 0);

#if defined(TCP_KEEPIDLE)
				int idleSeconds = 0;
				QVERIFY(readSocketIntOption(descriptor, IPPROTO_TCP, TCP_KEEPIDLE, idleSeconds));
				QCOMPARE(idleSeconds, 120);
#elif defined(TCP_KEEPALIVE)
				int idleSeconds = 0;
				QVERIFY(readSocketIntOption(descriptor, IPPROTO_TCP, TCP_KEEPALIVE, idleSeconds));
				QCOMPARE(idleSeconds, 120);
#else
				QSKIP("TCP keepalive idle tuning option is not available on this platform.");
#endif

#if defined(TCP_KEEPINTVL)
				int intervalSeconds = 0;
				QVERIFY(readSocketIntOption(descriptor, IPPROTO_TCP, TCP_KEEPINTVL, intervalSeconds));
				QCOMPARE(intervalSeconds, 6);
#endif

				socket.abortSocket();
			}
			// NOLINTEND(readability-convert-member-functions-to-static)
	};

} // namespace

QTEST_GUILESS_MAIN(tst_WorldSocket_KeepAlive)

#include "tst_WorldSocket_KeepAlive.moc"
