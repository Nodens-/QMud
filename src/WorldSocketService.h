/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldSocketService.h
 * Role: Service/factory interface for creating and managing world socket instances within the runtime architecture.
 */

// Qt-native transport service contract used by world runtime networking.

#ifndef QMUD_WORLDSOCKETSERVICE_H
#define QMUD_WORLDSOCKETSERVICE_H

#include <QAbstractSocket>
#include <QObject>
#include <QString>

/**
 * @brief Connection preferences bundle applied to world socket services.
 */
struct WorldSocketConnectionSettings
{
		bool    useUtf8{false};
		bool    keepAlive{false};
		int     proxyType{0};
		QString proxyServer;
		quint16 proxyPort{0};
		QString proxyUsername;
		QString proxyPassword;
};

/**
 * @brief Abstract socket service interface used by world runtimes.
 *
 * Defines connection, proxy, transport, and signal surface shared by concrete
 * socket implementations.
 */
class WorldSocketService : public QObject
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates abstract socket service base object.
		 * @param parent Optional Qt parent object.
		 */
		explicit WorldSocketService(QObject *parent = nullptr) : QObject(parent)
		{
		}
		/**
		 * @brief Virtual destructor.
		 */
		~WorldSocketService() override = default;

		/**
		 * @brief Enables UTF-8 text mode on transport where applicable.
		 * @param enabled Enable UTF-8 mode when `true`.
		 */
		virtual void   setUseUtf8(bool enabled) = 0;
		/**
		 * @brief Enables TCP keepalive for the active socket.
		 * @param enabled Enable keepalive when `true`.
		 */
		virtual void   setKeepAliveEnabled(bool enabled) = 0;
		/**
		 * @brief Applies manual proxy settings used for subsequent connects.
		 * @param proxyType Proxy type code.
		 * @param server Proxy hostname or IP.
		 * @param port Proxy port.
		 * @param username Proxy username.
		 * @param password Proxy password.
		 */
		virtual void   setProxySettings(int proxyType, const QString &server, quint16 port,
		                                const QString &username, const QString &password) = 0;
		/**
		 * @brief Applies a complete connection settings bundle.
		 * @param settings Connection settings bundle.
		 */
		virtual void   applyConnectionSettings(const WorldSocketConnectionSettings &settings) = 0;
		/**
		 * @brief Initiates connection to host and port.
		 * @param host Target host.
		 * @param port Target port.
		 * @return `true` when connection attempt was started.
		 */
		virtual bool   connectToHost(const QString &host, quint16 port) = 0;
		/**
		 * @brief Disconnects from remote host.
		 */
		virtual void   disconnectFromHost() = 0;
		/**
		 * @brief Sends raw bytes to the server.
		 * @param payload Raw byte payload.
		 * @return Number of bytes accepted for send.
		 */
		virtual qint64 sendPacket(const QByteArray &payload) = 0;
		/**
		 * @brief Returns peer IP string for active connection.
		 * @return Peer IP address string.
		 */
		[[nodiscard]] virtual QString peerAddressString() const = 0;
		/**
		 * @brief Returns peer IPv4 value in host byte order.
		 * @return Peer IPv4 address.
		 */
		[[nodiscard]] virtual quint32 peerAddressV4() const = 0;
		/**
		 * @brief Reports whether socket is currently connecting.
		 * @return `true` when connection attempt is in progress.
		 */
		[[nodiscard]] virtual bool    isConnecting() const = 0;
		/**
		 * @brief Reports whether socket is currently connected.
		 * @return `true` when socket is connected.
		 */
		[[nodiscard]] virtual bool    isConnected() const = 0;

	signals:
		/**
		 * @brief Emitted when raw bytes arrive from server.
		 * @param data Incoming raw bytes.
		 */
		void rawDataReceived(const QByteArray &data);
		/**
		 * @brief Emitted on transport/proxy/socket errors.
		 * @param message Error text.
		 */
		void socketError(const QString &message);
		/**
		 * @brief Emitted on socket state transitions.
		 * @param state New socket state.
		 */
		void socketStateChanged(QAbstractSocket::SocketState state);
		/**
		 * @brief Emitted after successful connection establishment.
		 */
		void connected();
		/**
		 * @brief Emitted after connection teardown.
		 */
		void disconnected();
};

#endif // QMUD_WORLDSOCKETSERVICE_H
