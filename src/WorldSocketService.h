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

#include "GmcpProtocol.h"
#include <QAbstractSocket>
#include <QByteArray>
#include <QDateTime>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QtGlobal>

/**
 * @brief Read accounting and statistics belonging to one decoded input batch.
 *
 * receivedBytes and debugPacket are delivered only with the first chunk of a socket read, including
 * reads with no projected text. The runtime fills bytesIn and inputPacketCount before queuing transport-decoded text.
 * Compression counts carry the worker contribution for legacy input, and combined world totals
 * after runtime admission for transport-decoded input.
 */
struct WorldSocketInputMetadata
{
		bool       transportDecoded{false};
		qint64     receivedBytes{0};
		QByteArray debugPacket;
		qint64     bytesIn{0};
		int        inputPacketCount{0};
		qint64     totalCompressedBytes{0};
		qint64     totalUncompressedBytes{0};
		int        mccpType{0};
};
Q_DECLARE_METATYPE(WorldSocketInputMetadata)

/**
 * @brief One automatic protocol write accepted by the transport and awaiting runtime accounting.
 */
struct WorldSocketProtocolWrite
{
		QByteArray payload;
		QDateTime  timestamp;
};

/**
 * @brief Application write result and earlier automatic writes not yet accounted for by the runtime.
 */
struct WorldSocketWriteResult
{
		qint64                          accepted{-1};
		QList<WorldSocketProtocolWrite> precedingProtocolWrites;
};

/**
 * @brief Immutable transport statistics; cumulative native decoder counts last for the world's lifetime.
 */
struct WorldSocketTransportStatistics
{
		qint64 compressedBytes{0};
		qint64 uncompressedBytes{0};
		int    mccpType{0};
		bool   decodedByTransport{false};
		/**
		 * @brief Compares all transport statistic fields.
		 * @param other Other snapshot.
		 * @return Whether the statistics are identical.
		 */
		bool   operator==(const WorldSocketTransportStatistics &other) const = default;
};
/**
 * @brief Lifetime-independent transport statistics endpoint, written only on the socket worker.
 */
struct WorldSocketStatisticsPublication
{
		QMudAtomicSharedPtr<const WorldSocketTransportStatistics> current{
		    std::make_shared<const WorldSocketTransportStatistics>()};
};

/**
 * @brief Connection preferences bundle applied to world socket services.
 */
struct WorldSocketConnectionSettings
{
		bool                  useUtf8{false};
		bool                  nativeGmcp{false};
		QMudGmcp::CoreOptions gmcpCore;
		bool                  disableCompression{false};
		bool                  keepAlive{false};
		bool                  tlsEncryption{false};
		int                   tlsMethod{0};
		bool                  disableTlsCertificateValidation{false};
		QString               tlsServerName;
		int                   proxyType{0};
		QString               proxyServer;
		quint16               proxyPort{0};
		QString               proxyUsername;
		QString               proxyPassword;
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
		 * @brief Starts TLS client encryption on an already-connected socket.
		 * @param errorMessage Optional output error text.
		 * @return `true` when upgrade attempt was started or already active.
		 */
		virtual bool   startClientEncryption(QString *errorMessage) = 0;
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
		 * @brief Writes an application packet and transfers any preceding unreported protocol writes.
		 * @param payload Framed bytes to send.
		 * @param gmcpSession Required native GMCP session identity, or zero for an ordinary write.
		 * @return Accepted byte count and preceding protocol writes in physical write order, even on failure.
		 */
		virtual WorldSocketWriteResult writePacket(const QByteArray &payload, quint64 gmcpSession)
		{
			if (gmcpSession == 0)
				return {sendPacket(payload), {}};
			const QByteArray raw = QByteArray(payload.mid(3, payload.size() - 5))
			                           .replace(QByteArray::fromHex("ffff"), QByteArray::fromHex("ff"));
			return {sendGmcp(raw, gmcpSession) ? payload.size() : -1, {}};
		}
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
		 * @brief Returns native descriptor for active transport socket.
		 * @return Native descriptor, or `-1` when unavailable.
		 */
		[[nodiscard]] virtual qintptr nativeSocketDescriptor() const = 0;
		/**
		 * @brief Adopts an already-connected native socket descriptor.
		 * @param descriptor Descriptor to adopt.
		 * @param errorMessage Optional output error text.
		 * @return `true` when adoption succeeds.
		 */
		virtual bool adoptConnectedSocketDescriptor(qintptr descriptor, QString *errorMessage) = 0;
		/**
		 * @brief Immediately aborts the underlying socket.
		 */
		virtual void abortSocket() = 0;
		/**
		 * @brief Reports whether socket is currently connecting.
		 * @return `true` when connection attempt is in progress.
		 */
		[[nodiscard]] virtual bool                                              isConnecting() const = 0;
		/**
		 * @brief Reports whether socket is currently connected.
		 * @return `true` when socket is connected.
		 */
		[[nodiscard]] virtual bool                                              isConnected() const = 0;

		/**
		 * @brief Returns independently published transport statistics when supported.
		 * @return Shared endpoint, or empty for services without worker decoding.
		 */
		[[nodiscard]] virtual std::shared_ptr<WorldSocketStatisticsPublication> transportStatistics() const
		{
			return {};
		}
		/**
		 * @brief Returns native publication when available.
		 * @return Shared endpoint or empty for legacy-only services.
		 */
		[[nodiscard]] virtual std::shared_ptr<QMudGmcp::Publication> gmcpPublication() const
		{
			return {};
		}
		/**
		 * @brief Sends native data.
		 * @param raw Unframed message.
		 * @param generation Required identity.
		 * @return Whether accepted.
		 */
		virtual bool sendGmcp(const QByteArray &raw, quint64 generation)
		{
			Q_UNUSED(raw);
			Q_UNUSED(generation);
			return false;
		}
		/**
		 * @brief Sets an inheritable world merge policy.
		 * @param name Package or message path whose descendants inherit this policy.
		 * @param mode Merge mode.
		 * @param force Override server.
		 * @return Whether valid.
		 */
		virtual bool setGmcpMergePolicy(const QString &name, QMudGmcp::MergeMode mode, bool force)
		{
			Q_UNUSED(name);
			Q_UNUSED(mode);
			Q_UNUSED(force);
			return false;
		}
		/**
		 * @brief Updates advertised modules.
		 * @param modules Effective support list.
		 */
		virtual void setGmcpModules(const QMap<QString, int> &modules)
		{
			Q_UNUSED(modules);
		}
		/**
		 * @brief Controls capture of incoming packet bytes for runtime diagnostics.
		 * @param enabled Whether subsequent reads retain their packet bytes.
		 */
		virtual void setIncomingPacketDebugEnabled(bool enabled)
		{
			Q_UNUSED(enabled);
		}
		/**
		 * @brief Serializes a quiesced native connection.
		 * @return Handoff data.
		 */
		[[nodiscard]] virtual QByteArray saveNativeState() const
		{
			return {};
		}
		/**
		 * @brief Restores native state.
		 * @param state Handoff data.
		 * @return Whether restored.
		 */
		virtual bool restoreNativeState(const QByteArray &state)
		{
			return state.isEmpty();
		}
		/**
		 * @brief Pauses socket delivery for reload.
		 * @param paused Pause state.
		 */
		virtual void pauseTransport(bool paused)
		{
			Q_UNUSED(paused);
		}

		/**
		 * @brief Freezes reception after processing bytes already buffered by Qt.
		 */
		virtual void freezeForReload()
		{
		}
		/**
		 * @brief Checks frozen input delivery.
		 * @return Whether input and notifications have reached the runtime.
		 */
		[[nodiscard]] virtual bool reloadInputDrained() const
		{
			return true;
		}
		/**
		 * @brief Flushes output without resuming reception.
		 * @return Whether no writes remain buffered.
		 */
		virtual bool flushForReload()
		{
			return true;
		}

		/**
		 * @brief Atomically adopts and restores native framing before reading.
		 * @param descriptor Socket descriptor.
		 * @param state Serialized native state.
		 * @param error Output diagnostic.
		 * @return Whether adopted.
		 */
		virtual bool adoptNativeSocketDescriptor(qintptr descriptor, const QByteArray &state, QString *error)
		{
			if (!state.isEmpty())
			{
				if (error)
					*error = QStringLiteral("Native handoff is unavailable.");
				return false;
			}
			return adoptConnectedSocketDescriptor(descriptor, error);
		}

		/**
		 * @brief Updates MCCP negotiation preference.
		 * @param disabled Whether to refuse subsequent offers.
		 */
		virtual void setCompressionDisabled(bool disabled)
		{
			Q_UNUSED(disabled);
		}

		/**
		 * @brief Updates automatic GMCP Core scheduling independently of connection handler ownership.
		 * @param options Persistent world settings.
		 */
		virtual void setGmcpCoreOptions(const QMudGmcp::CoreOptions &options)
		{
			Q_UNUSED(options);
		}

	signals:
		/**
		 * @brief Reports a measured GMCP RTT or -1 when unavailable.
		 * @param milliseconds Latest measured round-trip duration.
		 */
		void gmcpRoundTripChanged(qint64 milliseconds);
		/**
		 * @brief Delivers a received GMCP Goodbye string.
		 * @param message Server text for world output.
		 */
		void gmcpGoodbye(const QString &message);
		/**
		 * @brief Reports a native negotiation transition.
		 * @param enabled Whether native GMCP is active.
		 */
		void gmcpStateChanged(bool enabled);
		/**
		 * @brief Reports a native GMCP event.
		 * @param record Raw history record.
		 * @param message Optional application event.
		 */
		void gmcpObserved(const QMudGmcp::Record                         &record,
		                  const std::shared_ptr<const QMudGmcp::Message> &message);
		/**
		 * @brief Reports native protocol bytes accepted for writing.
		 * @param write Accepted bytes and their write timestamp.
		 */
		void nativeProtocolWritten(const WorldSocketProtocolWrite &write);
		/**
		 * @brief Delivers ordered socket input and its receive accounting.
		 * @param data Projected bytes, or empty for a native read containing only consumed protocol data.
		 * @param metadata Read accounting and statistics belonging to this batch; empty for legacy processing.
		 */
		void rawDataReceived(const QByteArray &data, const WorldSocketInputMetadata &metadata = {});
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
		 * @brief Emitted when TLS encryption becomes active on the socket.
		 */
		void tlsEncrypted();
		/**
		 * @brief Emitted after connection teardown.
		 */
		void disconnected();
};

#endif // QMUD_WORLDSOCKETSERVICE_H
