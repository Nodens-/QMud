/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldSocket.cpp
 * Role: World socket transport implementation that manages TCP lifecycle, buffering, and event emission for runtime
 * consumers.
 */

#include "WorldSocket.h"

#include "WorldOptions.h"
#include <QElapsedTimer>
#include <QMetaObject>
#include <QNetworkProxy>
#include <QTcpSocket>
#if defined(Q_OS_WIN)
#include <winsock2.h>
#include <ws2tcpip.h>
#elif defined(Q_OS_UNIX)
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

namespace
{
#if defined(Q_OS_WIN)
#ifndef SIO_KEEPALIVE_VALS
#define SIO_KEEPALIVE_VALS _WSAIOW(IOC_VENDOR, 4)
#endif

	/**
	 * @brief Windows keepalive tuning payload used with `SIO_KEEPALIVE_VALS`.
	 *
	 * Defined locally to avoid toolchain/header-order sensitivity between MSVC
	 * and MinGW while keeping WinSock API-compatible field layout.
	 */
	struct WinTcpKeepAlive
	{
			u_long onoff;
			u_long keepalivetime;
			u_long keepaliveinterval;
	};
#endif

	void applyMushclientKeepAliveSettings(QTcpSocket *socket, const bool enabled)
	{
		if (!socket)
			return;

		socket->setSocketOption(QAbstractSocket::KeepAliveOption, enabled ? 1 : 0);
		if (!enabled)
			return;

		const qintptr descriptor = socket->socketDescriptor();
		if (descriptor < 0)
			return;

#if defined(Q_OS_WIN)
		WinTcpKeepAlive keepaliveData{};
		keepaliveData.onoff             = 1;
		keepaliveData.keepalivetime     = 120000; // 2 minutes
		keepaliveData.keepaliveinterval = 6000;   // 6 seconds
		DWORD bytesReturned             = 0;
		(void)WSAIoctl(static_cast<SOCKET>(descriptor), SIO_KEEPALIVE_VALS, &keepaliveData,
		               static_cast<DWORD>(sizeof(keepaliveData)), nullptr, 0, &bytesReturned, nullptr,
		               nullptr);
#elif defined(Q_OS_UNIX)
		const int     fd              = static_cast<int>(descriptor);
		constexpr int idleSeconds     = 120;
		constexpr int intervalSeconds = 6;
#if defined(TCP_KEEPIDLE)
		(void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idleSeconds, sizeof(idleSeconds));
#elif defined(TCP_KEEPALIVE)
		(void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idleSeconds, sizeof(idleSeconds));
#endif
#if defined(TCP_KEEPINTVL)
		(void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intervalSeconds, sizeof(intervalSeconds));
#endif
#endif
	}
} // namespace

WorldSocket::WorldSocket(QObject *parent) : WorldSocketService(parent)
{
	m_socket = new QTcpSocket(this);
	connect(m_socket, &QTcpSocket::readyRead, this, &WorldSocket::onReadyRead);
	connect(m_socket, &QTcpSocket::connected, this, &WorldSocket::onConnected);
	connect(m_socket, &QTcpSocket::disconnected, this, &WorldSocket::onDisconnected);
	connect(m_socket, &QTcpSocket::stateChanged, this,
	        [this](QAbstractSocket::SocketState state) { emit socketStateChanged(state); });
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	connect(m_socket, &QTcpSocket::errorOccurred, this, &WorldSocket::onErrorOccurred);
#else
	connect(m_socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error), this,
	        &WorldSocket::onErrorOccurred);
#endif
}

void WorldSocket::setUseUtf8(bool enabled)
{
	m_useUtf8 = enabled;
}

void WorldSocket::setKeepAliveEnabled(bool enabled)
{
	m_keepAliveEnabled = enabled;
	if (m_socket && m_socket->state() == QAbstractSocket::ConnectedState)
		applyMushclientKeepAliveSettings(m_socket, enabled);
}

void WorldSocket::setProxySettings(int proxyType, const QString &server, quint16 port,
                                   const QString &username, const QString &password)
{
	if (!m_socket)
		return;

	QNetworkProxy proxy;
	if (proxyType == eProxyServerSocks4)
	{
		// Qt has no native SOCKS4 type; emulate the closest behavior by disabling
		// proxy-side host lookup capability and relying on pre-resolved IPv4 targets.
		proxy.setType(QNetworkProxy::Socks5Proxy);
		proxy.setCapabilities(QNetworkProxy::TunnelingCapability);
		proxy.setHostName(server.trimmed());
		proxy.setPort(port);
		proxy.setUser(username);
		proxy.setPassword(password);
	}
	else if (proxyType == eProxyServerSocks5)
	{
		proxy.setType(QNetworkProxy::Socks5Proxy);
		proxy.setHostName(server.trimmed());
		proxy.setPort(port);
		proxy.setUser(username);
		proxy.setPassword(password);
	}
	else
	{
		proxy.setType(QNetworkProxy::NoProxy);
	}
	m_socket->setProxy(proxy);
}

void WorldSocket::applyConnectionSettings(const WorldSocketConnectionSettings &settings)
{
	setUseUtf8(settings.useUtf8);
	setKeepAliveEnabled(settings.keepAlive);
	setProxySettings(settings.proxyType, settings.proxyServer, settings.proxyPort, settings.proxyUsername,
	                 settings.proxyPassword);
}

bool WorldSocket::connectToHost(const QString &host, quint16 port)
{
	if (!m_socket)
		return false;

	if (m_socket->state() != QAbstractSocket::UnconnectedState)
		m_socket->abort();
	applyMushclientKeepAliveSettings(m_socket, m_keepAliveEnabled);
	m_socket->connectToHost(host, port);
	return true;
}

void WorldSocket::disconnectFromHost()
{
	if (!m_socket)
		return;
	if (m_socket->state() == QAbstractSocket::ConnectedState)
		m_socket->disconnectFromHost();
	else
		m_socket->abort();
}

qint64 WorldSocket::sendPacket(const QByteArray &payload)
{
	if (!m_socket)
		return -1;

	if (payload.isEmpty())
		return 0;

	if (m_socket->state() != QAbstractSocket::ConnectedState)
		return -1;

	return m_socket->write(payload);
}

QString WorldSocket::peerAddressString() const
{
	if (!m_socket)
		return {};
	const QHostAddress addr = m_socket->peerAddress();
	return addr.isNull() ? QString() : addr.toString();
}

quint32 WorldSocket::peerAddressV4() const
{
	if (!m_socket)
		return 0;
	const QHostAddress addr = m_socket->peerAddress();
	if (addr.protocol() != QAbstractSocket::IPv4Protocol)
		return 0;
	return addr.toIPv4Address();
}

qintptr WorldSocket::nativeSocketDescriptor() const
{
	if (!m_socket)
		return static_cast<qintptr>(-1);
	return m_socket->socketDescriptor();
}

bool WorldSocket::adoptConnectedSocketDescriptor(const qintptr descriptor, QString *errorMessage)
{
	if (errorMessage)
		errorMessage->clear();
	if (!m_socket)
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Socket transport is not initialized.");
		return false;
	}
	if (descriptor < 0)
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Socket descriptor is invalid.");
		return false;
	}

	if (m_socket->state() != QAbstractSocket::UnconnectedState)
		m_socket->abort();
	m_pendingInbound.clear();
	m_drainingNow    = false;
	m_drainScheduled = false;

	if (!m_socket->setSocketDescriptor(descriptor, QAbstractSocket::ConnectedState, QIODevice::ReadWrite))
	{
		if (errorMessage)
			*errorMessage = m_socket->errorString();
		return false;
	}
	applyMushclientKeepAliveSettings(m_socket, m_keepAliveEnabled);
	return true;
}

void WorldSocket::abortSocket()
{
	if (!m_socket)
		return;
	m_socket->abort();
	m_pendingInbound.clear();
	m_drainScheduled = false;
	m_drainingNow    = false;
}

bool WorldSocket::isConnecting() const
{
	if (!m_socket)
		return false;
	const QAbstractSocket::SocketState state = m_socket->state();
	return state == QAbstractSocket::HostLookupState || state == QAbstractSocket::ConnectingState;
}

bool WorldSocket::isConnected() const
{
	if (!m_socket)
		return false;
	return m_socket->state() == QAbstractSocket::ConnectedState;
}

void WorldSocket::onReadyRead()
{
	if (!m_socket)
		return;

	const QByteArray incoming = m_socket->readAll();
	if (incoming.isEmpty())
		return;
	m_pendingInbound.append(incoming);
	if (m_drainingNow)
		return;
	// Kick drain immediately so first lines render without waiting for queued
	// events after the socket notifier burst completes.
	drainPendingReads();
}

void WorldSocket::drainPendingReads()
{
	if (m_drainingNow)
		return;

	m_drainingNow    = true;
	m_drainScheduled = false;
	if (m_pendingInbound.isEmpty())
	{
		m_drainingNow = false;
		return;
	}

	constexpr qsizetype kEmitChunkSize     = 16 * 1024;
	constexpr int       kMaxChunksPerDrain = 4;
	constexpr int       kDrainBudgetMs     = 2;
	int                 emitted            = 0;
	QElapsedTimer       budget;
	budget.start();

	while (!m_pendingInbound.isEmpty())
	{
		const qsizetype emitSize = qMin(kEmitChunkSize, m_pendingInbound.size());
		QByteArray      chunk    = m_pendingInbound.left(emitSize);
		m_pendingInbound.remove(0, emitSize);
		if (!chunk.isEmpty())
			emit rawDataReceived(chunk);
		++emitted;
		if (emitted >= kMaxChunksPerDrain || budget.elapsed() >= kDrainBudgetMs)
			break;
	}

	m_drainingNow = false;

	if (!m_pendingInbound.isEmpty())
	{
		m_drainScheduled = true;
		QMetaObject::invokeMethod(this, &WorldSocket::drainPendingReads, Qt::QueuedConnection);
	}
}

void WorldSocket::onConnected()
{
	if (m_socket)
		applyMushclientKeepAliveSettings(m_socket, m_keepAliveEnabled);
	emit connected();
}

void WorldSocket::onDisconnected()
{
	m_pendingInbound.clear();
	m_drainScheduled = false;
	m_drainingNow    = false;
	emit disconnected();
}

void WorldSocket::onErrorOccurred()
{
	if (!m_socket)
		return;

	emit socketError(m_socket->errorString());
}
