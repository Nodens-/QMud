/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldSocket.h
 * Role: World socket facade, worker-owned transport, and shared OOB stream interfaces.
 */

// Registered OOB handlers receive complete transport frames on the socket worker.
// Legacy connections retain WorldRuntime/TelnetProcessor processing.

#ifndef QMUD_WORLDSOCKET_H
#define QMUD_WORLDSOCKET_H

#include "GmcpProtocol.h"
#include "WorldSocketService.h"
#include <memory>

class QTcpSocket;

/**
 * @brief Shared OOB transport framing and MCCP decoder, with complete-message protocol dispatch.
 *
 * All operations belong to the owning transport thread. Handlers are configured at connection boundaries.
 */
class WorldSocketStream
{
	public:
		/**
		 * @brief Complete-frame callbacks for one Telnet protocol option.
		 */
		struct Handler
		{
				std::function<void(unsigned char, const QByteArray &)> received;
				std::function<void(const QByteArray &)>                sent;
		};
		/**
		 * @brief Constructs an inactive stream decoder.
		 */
		WorldSocketStream();
		/**
		 * @brief Releases decompression resources without invoking handlers.
		 */
		~WorldSocketStream();
		/**
		 * @brief Installs a protocol handler before starting or restoring a connection.
		 * @param option Telnet option number.
		 * @param handler Complete-frame operations.
		 */
		void               setHandler(unsigned char option, const Handler &handler);
		/**
		 * @brief Removes protocol handlers before a connection is reconfigured.
		 */
		void               clearHandlers();
		/**
		 * @brief Resets connection parser state while preserving lifetime byte totals.
		 * @param disableCompression Whether to refuse future MCCP offers.
		 */
		void               reset(bool disableCompression);
		/**
		 * @brief Ends decoding and releases the inflater without clearing cumulative counts.
		 */
		void               close();
		/**
		 * @brief Stops the current decode loop without invalidating inflater accounting during a read.
		 */
		void               stopDecoding();
		/**
		 * @brief Processes decrypted bytes and dispatches complete registered OOB frames.
		 * @param input Socket bytes.
		 * @return Bytes remaining for the runtime's Telnet/text processor.
		 */
		QByteArray         receive(const QByteArray &input);
		/**
		 * @brief Observes accepted writes for complete outgoing protocol frames and START-TLS boundaries.
		 * @param bytes Bytes accepted by the socket.
		 */
		void               observeSent(const QByteArray &bytes);
		/**
		 * @brief Changes future MCCP negotiation preference without interrupting an active stream.
		 * @param disabled Whether to refuse offers.
		 */
		void               setCompressionDisabled(bool disabled);
		/**
		 * @brief Checks stream eligibility for descriptor preservation.
		 * @return Whether neither direction has a partial frame and no inflater is active.
		 */
		[[nodiscard]] bool atBoundary() const;
		/**
		 * @brief Borrows statistics on the owning worker thread.
		 * @return Current cumulative counters, compression type, and decoder ownership.
		 */
		[[nodiscard]] const WorldSocketTransportStatistics &statistics() const;
		/**
		 * @brief Writes complete transport negotiations and returns the accepted byte count.
		 */
		std::function<qint64(const QByteArray &)>           write;
		/**
		 * @brief Reports stream failures to the transport owner.
		 */
		std::function<void(const QString &)>                fatal;
		/**
		 * @brief Requests TLS activation after the accepted FOLLOWS frame.
		 */
		std::function<void()>                               startTlsFollowsSent;

	private:
		struct Impl;
		std::unique_ptr<Impl> d;
		/**
		 * @brief Borrows mutable stream state for an operation on its owner thread.
		 * @return Implementation owned by this stream.
		 */
		[[nodiscard]] Impl   &impl() noexcept
		{
			return *d;
		}
		/**
		 * @brief Borrows read-only stream state for a query on its owner thread.
		 * @return Immutable implementation view.
		 */
		[[nodiscard]] const Impl &impl() const noexcept
		{
			return *d;
		}
};

/**
 * @brief TCP socket-backed implementation of `WorldSocketService`.
 *
 * Handles world/proxy connection setup, byte transport, and socket event
 * forwarding on top of Qt networking primitives.
 */
class WorldSocketTransport : public WorldSocketService
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates transport socket and wiring.
		 * @param parent Optional Qt parent object.
		 */
		explicit WorldSocketTransport(QObject *parent = nullptr);

		/**
		 * @brief Closes stream resources and publishes inactive transport statistics on the owner thread.
		 */
		~WorldSocketTransport() override;

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

		/**
		 * @brief Installs a connection generation before starting transport.
		 * @param generation New identity.
		 * @param preserveRestoration Whether to retain prepared session state for descriptor adoption.
		 */
		void               prepareGeneration(quint64 generation, bool preserveRestoration = false);
		/**
		 * @brief Returns the current generation.
		 * @return Connection identity.
		 */
		[[nodiscard]] quint64                                           generation() const;
		/**
		 * @brief Returns transport statistics independently of protocol publications.
		 * @return Shared transport endpoint.
		 */
		[[nodiscard]] std::shared_ptr<WorldSocketStatisticsPublication> transportStatistics() const override;
		/**
		 * @brief Returns native publication.
		 * @return Shared endpoint.
		 */
		[[nodiscard]] std::shared_ptr<QMudGmcp::Publication>            gmcpPublication() const override;
		/**
		 * @brief Sends native data.
		 * @param raw Unframed message.
		 * @param generation Required identity.
		 * @return Whether accepted.
		 */
		bool sendGmcp(const QByteArray &raw, quint64 generation) override;
		/**
		 * @brief Sets an inheritable world merge policy.
		 * @param name Package or message path whose descendants inherit this policy.
		 * @param mode Merge mode.
		 * @param force Override server.
		 * @return Whether valid.
		 */
		bool setGmcpMergePolicy(const QString &name, QMudGmcp::MergeMode mode, bool force) override;
		/**
		 * @brief Updates automatic GMCP Core scheduling on the transport thread.
		 * @param options Persistent world settings.
		 */
		void setGmcpCoreOptions(const QMudGmcp::CoreOptions &options) override;
		/**
		 * @brief Updates advertised modules.
		 * @param modules Effective support list.
		 */
		void setGmcpModules(const QMap<QString, int> &modules) override;
		/**
		 * @brief Controls capture at the socket read boundary.
		 * @param enabled Whether subsequent reads retain diagnostic bytes.
		 */
		void setIncomingPacketDebugEnabled(bool enabled) override;
		/**
		 * @brief Serializes a quiesced native connection.
		 * @return Handoff data.
		 */
		[[nodiscard]] QByteArray saveNativeState() const override;
		/**
		 * @brief Restores native state.
		 * @param state Handoff data.
		 * @return Whether restored.
		 */
		bool                     restoreNativeState(const QByteArray &state) override;
		/**
		 * @brief Pauses socket delivery for reload.
		 * @param paused Pause state.
		 */
		void                     pauseTransport(bool paused) override;
		/**
		 * @brief Processes buffered Qt input before its event loop is frozen.
		 */
		void                     freezeForReload() override;
		/**
		 * @brief Flushes buffered writes without reading input.
		 * @return Whether no writes remain buffered.
		 */
		bool                     flushForReload() override;
		/**
		 * @brief Checks whether connection restoration was prepared before descriptor adoption.
		 * @return Whether transport input must await restoration activation.
		 */
		[[nodiscard]] bool       restorationPrepared() const
		{
			return m_restorePrepared;
		}

		/**
		 * @brief Activates prepared native or legacy session state after descriptor adoption.
		 */
		void activateRestoration();

		/**
		 * @brief Updates MCCP negotiation preference.
		 * @param disabled Whether to refuse subsequent offers.
		 */
		void setCompressionDisabled(bool disabled) override;

	private slots:
		/**
		 * @brief Immediately reads and decodes available transport bytes without runtime delivery pacing.
		 */
		void onReadyRead();
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
		/**
		 * @brief Configures complete-frame protocol dispatch for the selected connection.
		 */
		void                                              configureProtocolHandlers();
		/**
		 * @brief Publishes changed transport statistics on the owning worker.
		 */
		void                                              publishStatistics();
		WorldSocketStream                                 m_stream;
		std::shared_ptr<WorldSocketStatisticsPublication> m_statistics{
		    std::make_shared<WorldSocketStatisticsPublication>()};
		WorldSocketTransportStatistics m_publishedStatistics;
		QMudGmcp::Session              m_gmcp;
		bool                           m_externalGmcpWrite{false};
		/**
		 * @brief Receives a decoder failure during a read so its accounting is queued before socket closure.
		 */
		QString                       *m_activeReadError{nullptr};
		quint64                        m_generation{0};
		bool                           m_nativeGmcp{false};
		bool                           m_captureIncomingPackets{false};
		bool                           m_disableCompression{false};
		bool                           m_paused{false};
		QTcpSocket                    *m_socket{nullptr};
		bool                           m_useUtf8{false};
		bool                           m_keepAliveEnabled{false};
		bool                           m_tlsEncryption{false};
		int                            m_tlsMethod{0};
		bool                           m_disableTlsCertificateValidation{false};
		QString                        m_tlsServerName;
		QString                        m_lastConnectHost;
		/**
		 * @brief Whether a preserved native or legacy session is awaiting descriptor adoption.
		 */
		bool                           m_restorePrepared{false};
};

/**
 * @brief Runtime-thread socket facade preserving service semantics across the transport worker.
 *
 * Owns the worker and ordered delivery mailbox; socket operations execute on the transport thread.
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
		 * @brief Sends an application packet with earlier automatic write observations in the same handoff.
		 * @param payload Framed bytes to send.
		 * @param gmcpSession Required native GMCP session identity, or zero for an ordinary write.
		 * @return Accepted byte count and preceding unreported protocol writes, including on failure.
		 */
		WorldSocketWriteResult writePacket(const QByteArray &payload, quint64 gmcpSession) override;
		/**
		 * @brief Returns remote address string.
		 * @return Remote peer address string.
		 */
		[[nodiscard]] QString  peerAddressString() const override;
		/**
		 * @brief Returns remote IPv4 address value.
		 * @return Remote peer IPv4 address.
		 */
		[[nodiscard]] quint32  peerAddressV4() const override;
		/**
		 * @brief Returns native descriptor for active socket.
		 * @return Native descriptor, or `-1` when unavailable.
		 */
		[[nodiscard]] qintptr  nativeSocketDescriptor() const override;
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

		/**
		 * @brief Stops and joins the transport worker.
		 */
		~WorldSocket() override;
		/**
		 * @brief Returns native publication.
		 * @return Shared endpoint.
		 */
		[[nodiscard]] std::shared_ptr<QMudGmcp::Publication>            gmcpPublication() const override;
		/**
		 * @brief Returns the worker's transport statistics without accessing its socket.
		 * @return Shared statistics endpoint.
		 */
		[[nodiscard]] std::shared_ptr<WorldSocketStatisticsPublication> transportStatistics() const override;

		/**
		 * @brief Sends native data.
		 * @param raw Unframed message.
		 * @param generation Required identity.
		 * @return Whether accepted.
		 */
		bool sendGmcp(const QByteArray &raw, quint64 generation) override;
		/**
		 * @brief Sets an inheritable world merge policy.
		 * @param name Package or message path whose descendants inherit this policy.
		 * @param mode Merge mode.
		 * @param force Override server.
		 * @return Whether valid.
		 */
		bool setGmcpMergePolicy(const QString &name, QMudGmcp::MergeMode mode, bool force) override;
		/**
		 * @brief Updates automatic GMCP Core scheduling on the transport thread.
		 * @param options Persistent world settings.
		 */
		void setGmcpCoreOptions(const QMudGmcp::CoreOptions &options) override;
		/**
		 * @brief Updates advertised modules.
		 * @param modules Effective support list.
		 */
		void setGmcpModules(const QMap<QString, int> &modules) override;
		/**
		 * @brief Controls transport-side capture of incoming packet bytes.
		 * @param enabled Whether subsequent reads retain diagnostic bytes.
		 */
		void setIncomingPacketDebugEnabled(bool enabled) override;
		/**
		 * @brief Serializes a quiesced native connection.
		 * @return Handoff data.
		 */
		[[nodiscard]] QByteArray saveNativeState() const override;
		/**
		 * @brief Restores native state.
		 * @param state Handoff data.
		 * @return Whether restored.
		 */
		bool                     restoreNativeState(const QByteArray &state) override;
		/**
		 * @brief Pauses socket delivery for reload.
		 * @param paused Pause state.
		 */
		void                     pauseTransport(bool paused) override;
		/**
		 * @brief Freezes reception after decoding already buffered bytes.
		 */
		void                     freezeForReload() override;
		/**
		 * @brief Checks frozen delivery completion.
		 * @return Whether all input and notifications reached the runtime.
		 */
		[[nodiscard]] bool       reloadInputDrained() const override;
		/**
		 * @brief Flushes output without resuming reception.
		 * @return Whether all writes reached the kernel.
		 */
		bool                     flushForReload() override;

		/**
		 * @brief Atomically adopts and restores native framing before reading.
		 * @param descriptor Socket descriptor.
		 * @param state Serialized native state.
		 * @param error Output diagnostic.
		 * @return Whether adopted.
		 */
		bool                     adoptNativeSocketDescriptor(qintptr descriptor, const QByteArray &state,
		                                                     QString *error) override;

		/**
		 * @brief Updates MCCP negotiation preference.
		 * @param disabled Whether to refuse subsequent offers.
		 */
		void                     setCompressionDisabled(bool disabled) override;

	private:
		/**
		 * @brief Admits worker input to the existing runtime-thread delivery drain.
		 */
		void onReadyRead();
		/**
		 * @brief Delivers ordered 16 KiB chunks with the existing four-chunk/2 ms budget and reentrancy
		 * guard.
		 */
		void drainPendingReads();
		/**
		 * @brief Accounts for unread native batches before discarding text on disconnect or abort.
		 */
		void clearPendingReads();
		bool m_deliveryPaused{false};
		bool m_drainScheduled{false};
		bool m_drainingNow{false};
		struct Worker;
		std::unique_ptr<Worker> m_worker;
};
#endif
