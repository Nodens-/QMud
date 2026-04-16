/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldSocket.h
 * Role: Socket abstraction interfaces for world connections, exposing Qt-signal-driven networking events to runtime
 * code.
 */

// Telnet/MCCP/MXP protocol handling lives in WorldRuntime/TelnetProcessor.
// This class is intentionally transport-only and applies keepalive/proxy settings.

#ifndef QMUD_WORLDSOCKET_H
#define QMUD_WORLDSOCKET_H

#include "WorldSocketService.h"

class QTcpSocket;

/**
 * @brief TCP socket-backed implementation of `WorldSocketService`.
 *
 * Handles world/proxy connection setup, byte transport, and socket event
 * forwarding on top of Qt networking primitives.
 */
class WorldSocket : public WorldSocketService
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates transport socket and wiring.
		 * @param parent Optional Qt parent object.
		 */
		explicit WorldSocket(QObject *parent = nullptr);

		/**
		 * @brief Enables UTF-8 mode for connection payload handling.
		 * @param enabled Enable UTF-8 mode when `true`.
		 */
		void   setUseUtf8(bool enabled) override;
		/**
		 * @brief Enables or disables TCP keepalive.
		 * @param enabled Enable keepalive when `true`.
		 */
		void   setKeepAliveEnabled(bool enabled) override;
		/**
		 * @brief Configures proxy settings used for future connects.
		 * @param proxyType Proxy type code.
		 * @param server Proxy hostname or IP.
		 * @param port Proxy port.
		 * @param username Proxy username.
		 * @param password Proxy password.
		 */
		void   setProxySettings(int proxyType, const QString &server, quint16 port, const QString &username,
		                        const QString &password) override;
		/**
		 * @brief Applies complete connection setting bundle.
		 * @param settings Connection settings bundle.
		 */
		void   applyConnectionSettings(const WorldSocketConnectionSettings &settings) override;
		/**
		 * @brief Starts TLS client encryption for START-TLS upgrade flow.
		 * @param errorMessage Optional output error text.
		 * @return `true` when upgrade attempt was started or already active.
		 */
		bool   startClientEncryption(QString *errorMessage) override;
		/**
		 * @brief Connects to MUD host.
		 * @param host Target host.
		 * @param port Target port.
		 * @return `true` when connection attempt was started.
		 */
		bool   connectToHost(const QString &host, quint16 port) override;
		/**
		 * @brief Disconnects socket from host.
		 */
		void   disconnectFromHost() override;
		/**
		 * @brief Sends one payload packet.
		 * @param payload Raw bytes to send.
		 * @return Number of bytes accepted for send.
		 */
		qint64 sendPacket(const QByteArray &payload) override;
		/**
		 * @brief Returns remote address string.
		 * @return Remote peer address string.
		 */
		[[nodiscard]] QString peerAddressString() const override;
		/**
		 * @brief Returns remote IPv4 address value.
		 * @return Remote peer IPv4 address.
		 */
		[[nodiscard]] quint32 peerAddressV4() const override;
		/**
		 * @brief Returns native descriptor for active socket.
		 * @return Native descriptor, or `-1` when unavailable.
		 */
		[[nodiscard]] qintptr nativeSocketDescriptor() const override;
		/**
		 * @brief Adopts an already-connected native descriptor.
		 * @param descriptor Native descriptor to adopt.
		 * @param errorMessage Optional output error text.
		 * @return `true` when descriptor adoption succeeds.
		 */
		bool               adoptConnectedSocketDescriptor(qintptr descriptor, QString *errorMessage) override;
		/**
		 * @brief Immediately aborts active transport socket.
		 */
		void               abortSocket() override;
		/**
		 * @brief Reports whether connection attempt is in progress.
		 * @return `true` when socket is connecting.
		 */
		[[nodiscard]] bool isConnecting() const override;
		/**
		 * @brief Reports whether socket is connected.
		 * @return `true` when socket is connected.
		 */
		[[nodiscard]] bool isConnected() const override;

	private slots:
		/**
		 * @brief Reads available socket bytes into pending buffer.
		 */
		void onReadyRead();
		/**
		 * @brief Emits pending inbound bytes to service consumers.
		 */
		void drainPendingReads();
		/**
		 * @brief Handles connected event propagation.
		 */
		void onConnected();
		/**
		 * @brief Handles disconnected event propagation.
		 */
		void onDisconnected();
		/**
		 * @brief Converts Qt socket errors to service error messages.
		 */
		void onErrorOccurred();
		/**
		 * @brief Forwards Qt encrypted-socket readiness.
		 */
		void onTlsEncrypted();

	private:
		QTcpSocket *m_socket{nullptr};
		bool        m_useUtf8{false};
		bool        m_keepAliveEnabled{false};
		bool        m_tlsEncryption{false};
		int         m_tlsMethod{0};
		bool        m_disableTlsCertificateValidation{false};
		QString     m_tlsServerName;
		QString     m_lastConnectHost;
		QByteArray  m_pendingInbound;
		bool        m_drainScheduled{false};
		bool        m_drainingNow{false};
};

#endif // QMUD_WORLDSOCKET_H
