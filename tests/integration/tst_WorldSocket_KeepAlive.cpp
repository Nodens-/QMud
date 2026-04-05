/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldSocket_KeepAlive.cpp
 * Role: Integration coverage for world socket keepalive behavior and OS-level tuning.
 */

#include "WorldOptions.h"
#include "WorldSocket.h"

#include <QTcpServer>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#if defined(Q_OS_WIN)
#include <winsock2.h>
#elif defined(Q_OS_UNIX)
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
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
} // namespace

/**
 * @brief QTest fixture covering `WorldSocket` keepalive behavior.
 */
class tst_WorldSocket_KeepAlive : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		/**
		 * @brief Verifies keepalive enable/disable updates the live socket option.
		 */
		void togglesKeepAliveOnConnectedSocket()
		{
			QTcpServer server;
			if (!server.listen(QHostAddress::AnyIPv4, 0))
				QSKIP("Local TCP listen is unavailable in this environment.");

			WorldSocket socket;
			socket.setProxySettings(eProxyServerNone, QString(), 0, QString(), QString());
			socket.setKeepAliveEnabled(true);

			QSignalSpy connectedSpy(&socket, &WorldSocket::connected);
			QVERIFY(socket.connectToHost(QStringLiteral("127.0.0.1"), server.serverPort()));
			QVERIFY(connectedSpy.wait(5000));
			QVERIFY(server.waitForNewConnection(5000));

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
			if (!server.listen(QHostAddress::AnyIPv4, 0))
				QSKIP("Local TCP listen is unavailable in this environment.");

			WorldSocket socket;
			socket.setProxySettings(eProxyServerNone, QString(), 0, QString(), QString());
			socket.setKeepAliveEnabled(true);

			QSignalSpy connectedSpy(&socket, &WorldSocket::connected);
			QVERIFY(socket.connectToHost(QStringLiteral("127.0.0.1"), server.serverPort()));
			QVERIFY(connectedSpy.wait(5000));
			QVERIFY(server.waitForNewConnection(5000));

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

QTEST_GUILESS_MAIN(tst_WorldSocket_KeepAlive)

#if __has_include("tst_WorldSocket_KeepAlive.moc")
#include "tst_WorldSocket_KeepAlive.moc"
#endif
