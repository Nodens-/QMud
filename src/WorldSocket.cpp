/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldSocket.cpp
 * Role: World socket facade, worker transport, and shared OOB stream implementation.
 */
#include "WorldSocket.h"
#include "WorldOptions.h"
#include <QDataStream>
#include <QElapsedTimer>
#include <QIODevice>
#include <QMetaObject>
// ReSharper disable once CppUnusedIncludeDirective
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkProxy>
#include <QPointer>
#include <QQueue>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QSemaphore>
#include <QSslSocket>
#include <QTcpSocket>
#include <QThread>
#include <array>
#include <functional>
#include <limits>
#include <tuple>
#include <type_traits>
#include <zlib.h>
#if defined(Q_OS_WIN)
#include <winsock2.h>
#include <ws2tcpip.h>
#elif defined(Q_OS_UNIX)
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

/**
 * @brief Owns one transport thread; no worker operation synchronously calls the runtime.
 */
struct WorldSocket::Worker
{
		QThread                                           thread;
		QObject                                          *invoker{nullptr};
		WorldSocketTransport                             *transport{nullptr};
		WorldSocket                                      *owner{nullptr};
		quint64                                           generation{0};
		/**
		 * @brief Connection whose runtime-side initialization has completed; runtime thread only.
		 */
		quint64                                           inputReadyGeneration{0};
		std::shared_ptr<QMudGmcp::Publication>            publication;
		std::shared_ptr<WorldSocketStatisticsPublication> statistics;

		/**
		 * @brief Checks runtime-side input admission for the active connection.
		 * @return Whether its connected handler or synchronous adoption has completed.
		 */
		[[nodiscard]] bool                                inputReady() const noexcept
		{
			return inputReadyGeneration != 0 && inputReadyGeneration == generation;
		}

		/**
		 * @brief Immutable socket metadata sampled without cross-thread calls.
		 */
		struct Status
		{
				QString address;
				quint32 ipv4{0};
				qintptr descriptor{-1};
				bool    connecting{false};
				bool    connected{false};
		};
		QMudAtomicSharedPtr<const Status> status{std::make_shared<const Status>()};
		/**
		 * @brief Publishes socket metadata on its owning thread.
		 */
		void                              publishStatus()
		{
			status.store(std::make_shared<const Status>(
			                 Status{transport->peerAddressString(), transport->peerAddressV4(),
							        transport->nativeSocketDescriptor(), transport->isConnecting(),
							        transport->isConnected()}),
			             std::memory_order_release);
		}

		/**
		 * @brief A transport projection awaiting runtime delivery.
		 */
		struct Input
		{
				quint64                  generation;
				QByteArray               bytes;
				WorldSocketInputMetadata metadata;
				qsizetype                offset{0};
		};
		mutable QMutex                       inputMutex;
		QList<Input>                         pendingInput;
		bool                                 inputNotificationQueued{false};
		bool                                 handoffFrozen{false};
		mutable QMutex                       controlMutex;
		mutable QList<std::function<void()>> controlCommands;
		mutable QSemaphore                   controlReady;
		/**
		 * @brief Worker-written totals, read only after freeze/control completion semaphores establish
		 * ordering.
		 */
		quint64                              notificationCount{0};
		quint64                              deliveredNotifications{0};
		/**
		 * @brief Worker-owned write observation identified across queued and synchronous delivery paths.
		 */
		struct ProtocolWrite
		{
				quint64                  sequence;
				quint64                  generation;
				WorldSocketProtocolWrite write;
		};
		QQueue<ProtocolWrite> protocolWrites;
		quint64               nextProtocolWriteSequence{0};
		std::atomic<quint64>  consumedProtocolWriteSequence{0};
		/**
		 * @brief Removes observations already consumed on the runtime thread.
		 */
		void                  pruneProtocolWrites()
		{
			if (protocolWrites.isEmpty())
				return;
			const quint64 consumed = consumedProtocolWriteSequence.load(std::memory_order_acquire);
			while (!protocolWrites.isEmpty() && protocolWrites.head().sequence <= consumed)
				protocolWrites.dequeue();
		}
		/**
		 * @brief Claims a write on the runtime thread without repeating a previous handoff.
		 * @param observation Worker-captured write observation.
		 * @return Whether this is an unreported write belonging to the current connection.
		 */
		bool consumeProtocolWrite(const ProtocolWrite &observation)
		{
			if (observation.sequence <= consumedProtocolWriteSequence.load(std::memory_order_relaxed))
				return false;
			consumedProtocolWriteSequence.store(observation.sequence, std::memory_order_release);
			return observation.generation == generation;
		}
		/**
		 * @brief Queues automatic write accounting while retaining it for the next application write.
		 */
		void forwardProtocolWrites()
		{
			QObject::connect(
			    transport, &WorldSocketService::nativeProtocolWritten, invoker,
			    [this](const WorldSocketProtocolWrite &write)
			    {
				    pruneProtocolWrites();
				    const ProtocolWrite observation{++nextProtocolWriteSequence, transport->generation(),
					                                write};
				    protocolWrites.enqueue(observation);
				    ++notificationCount;
				    QMetaObject::invokeMethod(
				        owner,
				        [this, observation]
				        {
					        const QPointer<WorldSocket> guard(owner);
					        if (consumeProtocolWrite(observation))
						        emit owner->nativeProtocolWritten(observation.write);
					        if (guard)
						        ++deliveredNotifications;
				        },
				        Qt::QueuedConnection);
			    },
			    Qt::DirectConnection);
		}
		/**
		 * @brief Queues an operation while the Qt network event loop is frozen.
		 * @param operation Worker operation, or empty to resume the event loop.
		 */
		void control(std::function<void()> operation) const
		{
			{
				QMutexLocker lock(&controlMutex);
				controlCommands.push_back(std::move(operation));
			}
			controlReady.release();
		}
		/**
		 * @brief Releases the network event loop after canceled or failed reload preparation.
		 */
		void resumeHandoff()
		{
			if (std::exchange(handoffFrozen, false))
				control({});
		}

		/**
		 * @brief Transfers decoded bytes and their statistics to the runtime mailbox.
		 * @param identity Connection identity.
		 * @param bytes Projected bytes.
		 * @param metadata Statistics captured for these bytes.
		 */
		void queueInput(quint64 identity, const QByteArray &bytes, const WorldSocketInputMetadata &metadata)
		{
			{
				QMutexLocker lock(&inputMutex);
				pendingInput.push_back({identity, bytes, metadata});
				if (inputNotificationQueued)
					return;
				inputNotificationQueued = true;
			}
			QMetaObject::invokeMethod(owner, &WorldSocket::onReadyRead, Qt::QueuedConnection);
		}

		/**
		 * @brief Executes a service operation on the socket thread.
		 * @param operation Operation to execute.
		 * @return Its result, if any.
		 */
		template <typename F> auto call(F operation) const -> std::invoke_result_t<F>
		{
			using Result = std::invoke_result_t<F>;
			if (QThread::currentThread() == &thread)
				return operation();
			Q_ASSERT(QThread::currentThread() == owner->thread());
			if (handoffFrozen)
			{
				QSemaphore completed;
				if constexpr (std::is_void_v<Result>)
				{
					control(
					    [&]
					    {
						    operation();
						    completed.release();
					    });
					completed.acquire();
					return;
				}
				else
				{
					Result result{};
					control(
					    [&]
					    {
						    result = operation();
						    completed.release();
					    });
					completed.acquire();
					return result;
				}
			}
			if constexpr (std::is_void_v<Result>)
			{
				QMetaObject::invokeMethod(invoker, std::move(operation), Qt::BlockingQueuedConnection);
				return;
			}
			else
			{
				Result     result{};
				const bool accepted = QMetaObject::invokeMethod(
				    invoker, [&] { result = operation(); }, Qt::BlockingQueuedConnection);
				Q_ASSERT(accepted);
				return result;
			}
		}
		/**
		 * @brief Forwards TCP connection establishment and admits runtime input after its handler returns.
		 */
		void forwardConnected()
		{
			QObject::connect(
			    transport, &WorldSocketService::connected, invoker,
			    [this]
			    {
				    const quint64 identity = transport->generation();
				    ++notificationCount;
				    QMetaObject::invokeMethod(
				        owner,
				        [this, identity]
				        {
					        if (identity != generation)
					        {
						        ++deliveredNotifications;
						        return;
					        }
					        const QPointer<WorldSocket> guard(owner);
					        emit                        owner->connected();
					        if (!guard)
						        return;
					        ++deliveredNotifications;
					        if (identity != generation)
						        return;
					        // A queued receive task may predate this connection. Admit its bytes only
					        // after the runtime has reset parser state in the connected handler.
					        inputReadyGeneration = identity;
					        owner->onReadyRead();
				        },
				        Qt::QueuedConnection);
			    },
			    Qt::DirectConnection);
		}
		/**
		 * @brief Forwards one signal with its emission-time connection identity.
		 * @param signal Source signal.
		 * @param destination Facade signal.
		 */
		template <typename Signal> void forward(Signal signal, Signal destination)
		{
			QObject::connect(
			    transport, signal, invoker,
			    [this, destination](const auto &...values)
			    {
				    const quint64 identity  = transport->generation();
				    const auto    arguments = std::make_tuple(values...);
				    ++notificationCount;
				    QMetaObject::invokeMethod(
				        owner,
				        [this, identity, destination, arguments]
				        {
					        const QPointer<WorldSocket> guard(owner);
					        if (identity == generation)
						        std::apply([this, destination](const auto &...args)
								           { (owner->*destination)(args...); }, arguments);
					        if (guard)
						        ++deliveredNotifications;
				        },
				        Qt::QueuedConnection);
			    },
			    Qt::DirectConnection);
		}
};

WorldSocket::WorldSocket(QObject *parent) : WorldSocketService(parent), m_worker(std::make_unique<Worker>())
{
	m_worker->owner = this;
	connect(this, &WorldSocketService::disconnected, this, &WorldSocket::clearPendingReads);
	m_worker->thread.setObjectName(QStringLiteral("QMud transport"));
	m_worker->invoker = new QObject;
	m_worker->invoker->moveToThread(&m_worker->thread);
	connect(&m_worker->thread, &QThread::finished, m_worker->invoker, &QObject::deleteLater);
	m_worker->thread.start();
	m_worker->call(
	    [this]
	    {
		    m_worker->transport   = new WorldSocketTransport(m_worker->invoker);
		    m_worker->publication = m_worker->transport->gmcpPublication();
		    QObject::connect(
		        m_worker->transport, &WorldSocketService::socketStateChanged, m_worker->invoker,
		        [this] { m_worker->publishStatus(); }, Qt::DirectConnection);
		    QObject::connect(
		        m_worker->transport, &WorldSocketService::connected, m_worker->invoker,
		        [this] { m_worker->publishStatus(); }, Qt::DirectConnection);
		    QObject::connect(
		        m_worker->transport, &WorldSocketService::disconnected, m_worker->invoker,
		        [this] { m_worker->publishStatus(); }, Qt::DirectConnection);
		    m_worker->statistics = m_worker->transport->transportStatistics();
		    m_worker->publishStatus();

		    QObject::connect(
		        m_worker->transport, &WorldSocketService::rawDataReceived, m_worker->invoker,
		        [this](const QByteArray &bytes, const WorldSocketInputMetadata &metadata)
		        { m_worker->queueInput(m_worker->transport->generation(), bytes, metadata); },
		        Qt::DirectConnection);
		    m_worker->forward(&WorldSocketService::socketError, &WorldSocketService::socketError);
		    m_worker->forward(&WorldSocketService::socketStateChanged,
			                  &WorldSocketService::socketStateChanged);
		    m_worker->forwardConnected();
		    m_worker->forward(&WorldSocketService::disconnected, &WorldSocketService::disconnected);
		    m_worker->forward(&WorldSocketService::tlsEncrypted, &WorldSocketService::tlsEncrypted);
		    m_worker->forward(&WorldSocketService::gmcpStateChanged, &WorldSocketService::gmcpStateChanged);
		    m_worker->forward(&WorldSocketService::gmcpObserved, &WorldSocketService::gmcpObserved);
		    m_worker->forward(&WorldSocketService::gmcpRoundTripChanged,
			                  &WorldSocketService::gmcpRoundTripChanged);
		    m_worker->forward(&WorldSocketService::gmcpGoodbye, &WorldSocketService::gmcpGoodbye);
		    m_worker->forwardProtocolWrites();
	    });
}
WorldSocket::~WorldSocket()
{
	m_worker->resumeHandoff();
	++m_worker->generation;
	m_worker->call(
	    [this]
	    {
		    delete m_worker->transport;
		    m_worker->transport = nullptr;
	    });
	m_worker->thread.quit();
	m_worker->thread.wait();
}
void WorldSocket::onReadyRead()
{
	{
		QMutexLocker lock(&m_worker->inputMutex);
		m_worker->inputNotificationQueued = false;
	}
	if (!m_worker->inputReady())
	{
		const auto status = m_worker->status.load(std::memory_order_acquire);
		if (!status->connected && !status->connecting)
			clearPendingReads();
		return;
	}
	if (m_drainingNow || m_drainScheduled || m_deliveryPaused || !m_worker->inputReady())
		return;
	drainPendingReads();
}

void WorldSocket::drainPendingReads()
{
	m_drainScheduled = false;
	if (m_drainingNow || m_deliveryPaused || !m_worker->inputReady())
		return;
	m_drainingNow = true;
	const QPointer<WorldSocket> guard(this);
	const auto                  finish = qScopeGuard(
	    [this, guard]
	    {
		    if (!guard)
			    return;
		    m_drainingNow = false;
		    bool pending;
		    {
			    QMutexLocker lock(&m_worker->inputMutex);
			    pending = !m_worker->pendingInput.isEmpty();
		    }
		    if (pending && !m_deliveryPaused && !m_drainScheduled && m_worker->inputReady())
		    {
			    m_drainScheduled = true;
			    QMetaObject::invokeMethod(this, &WorldSocket::drainPendingReads, Qt::QueuedConnection);
		    }
	    });

	constexpr qsizetype kEmitChunkSize     = 16 * 1024;
	constexpr int       kMaxChunksPerDrain = 4;
	constexpr int       kDrainBudgetMs     = 2;
	int                 emitted            = 0;
	QElapsedTimer       budget;
	budget.start();
	while (guard && !m_deliveryPaused && m_worker->inputReady())
	{
		QByteArray               chunk;
		WorldSocketInputMetadata metadata;
		{
			QMutexLocker lock(&m_worker->inputMutex);
			while (!m_worker->pendingInput.isEmpty() &&
			       m_worker->pendingInput.constFirst().generation != m_worker->generation)
				m_worker->pendingInput.removeFirst();
			if (m_worker->pendingInput.isEmpty())
				return;
			auto &entry                  = m_worker->pendingInput.first();
			metadata                     = entry.metadata;
			entry.metadata.receivedBytes = 0;
			entry.metadata.debugPacket.clear();
			const qsizetype size = qMin(kEmitChunkSize, entry.bytes.size() - entry.offset);
			chunk                = entry.bytes.mid(entry.offset, size);
			entry.offset += size;
			if (entry.offset == entry.bytes.size())
				m_worker->pendingInput.removeFirst();
		}
		if (!chunk.isEmpty() || metadata.receivedBytes > 0)
			emit rawDataReceived(chunk, metadata);
		++emitted;
		if (emitted >= kMaxChunksPerDrain || budget.elapsed() >= kDrainBudgetMs)
			return;
	}
}

void WorldSocket::clearPendingReads()
{
	m_worker->inputReadyGeneration = 0;
	QList<Worker::Input> discarded;
	{
		QMutexLocker lock(&m_worker->inputMutex);
		discarded.swap(m_worker->pendingInput);
	}
	const QPointer<WorldSocket> guard(this);
	for (const auto &entry : discarded)
	{
		if (!guard)
			return;
		if (entry.generation == m_worker->generation && entry.metadata.receivedBytes > 0)
			emit rawDataReceived({}, entry.metadata);
	}
}

void WorldSocket::setUseUtf8(bool enabled)
{
	m_worker->call([&] { m_worker->transport->setUseUtf8(enabled); });
}
void WorldSocket::setKeepAliveEnabled(bool enabled)
{
	m_worker->call([&] { m_worker->transport->setKeepAliveEnabled(enabled); });
}
void WorldSocket::setProxySettings(int type, const QString &server, quint16 port, const QString &user,
                                   const QString &password)
{
	m_worker->call([&] { m_worker->transport->setProxySettings(type, server, port, user, password); });
}
void WorldSocket::applyConnectionSettings(const WorldSocketConnectionSettings &settings)
{
	m_worker->call([&] { m_worker->transport->applyConnectionSettings(settings); });
}
bool WorldSocket::startClientEncryption(QString *error)
{
	return m_worker->call([&] { return m_worker->transport->startClientEncryption(error); });
}
bool WorldSocket::connectToHost(const QString &host, quint16 port)
{
	const quint64 generation       = ++m_worker->generation;
	m_worker->inputReadyGeneration = 0;
	return m_worker->call(
	    [&]
	    {
		    m_worker->transport->prepareGeneration(generation);
		    return m_worker->transport->connectToHost(host, port);
	    });
}
void WorldSocket::disconnectFromHost()
{
	m_worker->call([&] { m_worker->transport->disconnectFromHost(); });
}
qint64 WorldSocket::sendPacket(const QByteArray &bytes)
{
	return m_worker->call([&] { return m_worker->transport->sendPacket(bytes); });
}
WorldSocketWriteResult WorldSocket::writePacket(const QByteArray &payload, quint64 gmcpSession)
{
	QQueue<Worker::ProtocolWrite> preceding;
	WorldSocketWriteResult        result = m_worker->call(
	    [&]
	    {
		    m_worker->pruneProtocolWrites();
		    preceding = std::exchange(m_worker->protocolWrites, {});
		    // Extract the prefix before writing; later worker writes belong to the next handoff.
		    return m_worker->transport->writePacket(payload, gmcpSession);
	    });
	for (const Worker::ProtocolWrite &observation : preceding)
		if (m_worker->consumeProtocolWrite(observation))
			result.precedingProtocolWrites.push_back(observation.write);
	return result;
}
QString WorldSocket::peerAddressString() const
{
	return m_worker->status.load(std::memory_order_acquire)->address;
}
quint32 WorldSocket::peerAddressV4() const
{
	return m_worker->status.load(std::memory_order_acquire)->ipv4;
}
qintptr WorldSocket::nativeSocketDescriptor() const
{
	return m_worker->status.load(std::memory_order_acquire)->descriptor;
}
bool WorldSocket::adoptConnectedSocketDescriptor(qintptr descriptor, QString *error,
                                                 const QByteArray &telnetInputState)
{
	const quint64 generation       = ++m_worker->generation;
	m_worker->inputReadyGeneration = 0;
	const bool adopted             = m_worker->call(
	    [&]
	    {
		    const auto publishStatus = qScopeGuard([&] { m_worker->publishStatus(); });
		    const bool restoring     = m_worker->transport->restorationPrepared();
		    m_worker->transport->prepareGeneration(generation, restoring);
		    if (!m_worker->transport->adoptConnectedSocketDescriptor(descriptor, error, telnetInputState))
		    {
			    m_worker->transport->abortSocket();
			    return false;
		    }
		    if (restoring)
			    m_worker->transport->activateRestoration();
		    return true;
	    });
	if (adopted)
	{
		// Descriptor adoption completes synchronously; no queued connected signal will admit its input.
		m_worker->inputReadyGeneration = generation;
		QMetaObject::invokeMethod(this, &WorldSocket::onReadyRead, Qt::QueuedConnection);
	}
	return adopted;
}
void WorldSocket::abortSocket()
{
	m_worker->call([&] { m_worker->transport->abortSocket(); });
	clearPendingReads();
}
bool WorldSocket::isConnecting() const
{
	return m_worker->status.load(std::memory_order_acquire)->connecting;
}
bool WorldSocket::isConnected() const
{
	return m_worker->status.load(std::memory_order_acquire)->connected;
}
std::shared_ptr<QMudGmcp::Publication> WorldSocket::gmcpPublication() const
{
	return m_worker->publication;
}
bool WorldSocket::sendGmcp(const QByteArray &raw, quint64 generation)
{
	return m_worker->call([&] { return m_worker->transport->sendGmcp(raw, generation); });
}
bool WorldSocket::setGmcpMergePolicy(const QString &name, QMudGmcp::MergeMode mode, bool force)
{
	return m_worker->call([&] { return m_worker->transport->setGmcpMergePolicy(name, mode, force); });
}
bool WorldSocket::clearGmcpMergePolicy(const QString &name)
{
	return m_worker->call([&] { return m_worker->transport->clearGmcpMergePolicy(name); });
}
void WorldSocket::setGmcpMergePolicies(const QMudGmcp::MergeOverrides &policies)
{
	m_worker->call([&] { m_worker->transport->setGmcpMergePolicies(policies); });
}
void WorldSocket::setGmcpCoreOptions(const QMudGmcp::CoreOptions &options)
{
	m_worker->call([&] { m_worker->transport->setGmcpCoreOptions(options); });
}
void WorldSocket::setGmcpModules(const QMap<QString, int> &modules)
{
	m_worker->call([&] { m_worker->transport->setGmcpModules(modules); });
}
void WorldSocket::setIncomingPacketDebugEnabled(bool enabled)
{
	m_worker->call([&] { m_worker->transport->setIncomingPacketDebugEnabled(enabled); });
}

void WorldSocket::freezeForReload()
{
	if (m_worker->handoffFrozen)
		return;
	QSemaphore frozen;
	QMetaObject::invokeMethod(
	    m_worker->invoker,
	    [worker = m_worker.get(), &frozen]
	    {
		    worker->transport->freezeForReload();
		    frozen.release();
		    for (;;)
		    {
			    worker->controlReady.acquire();
			    std::function<void()> operation;
			    {
				    QMutexLocker lock(&worker->controlMutex);
				    operation = worker->controlCommands.takeFirst();
			    }
			    if (!operation)
				    break;
			    operation();
		    }
	    },
	    Qt::QueuedConnection);
	frozen.acquire();
	m_worker->handoffFrozen = true;
	m_deliveryPaused        = false;
	QMetaObject::invokeMethod(this, &WorldSocket::onReadyRead, Qt::QueuedConnection);
}
bool WorldSocket::reloadInputDrained() const
{
	QMutexLocker lock(&m_worker->inputMutex);
	return m_worker->handoffFrozen && m_worker->pendingInput.isEmpty() && !m_drainingNow &&
	       m_worker->notificationCount == m_worker->deliveredNotifications;
}
bool WorldSocket::flushForReload()
{
	return m_worker->call([&] { return m_worker->transport->flushForReload(); });
}
QByteArray WorldSocket::saveTelnetInputState() const
{
	return m_worker->call([&] { return m_worker->transport->saveTelnetInputState(); });
}
QByteArray WorldSocket::saveNativeState() const
{
	return m_worker->call(
	    [&]
	    {
		    m_worker->pruneProtocolWrites();
		    return m_worker->transport->saveNativeState();
	    });
}
bool WorldSocket::restoreNativeState(const QByteArray &state)
{
	return m_worker->call([&] { return m_worker->transport->restoreNativeState(state); });
}

void WorldSocket::pauseTransport(bool paused)
{
	if (!paused)
		m_worker->resumeHandoff();
	m_deliveryPaused = paused;
	m_worker->call([&] { m_worker->transport->pauseTransport(paused); });
	if (!paused)
		QMetaObject::invokeMethod(this, &WorldSocket::onReadyRead, Qt::QueuedConnection);
}

void WorldSocket::setCompressionDisabled(bool disabled)
{
	m_worker->call([&] { m_worker->transport->setCompressionDisabled(disabled); });
}

std::shared_ptr<WorldSocketStatisticsPublication> WorldSocket::transportStatistics() const
{
	return m_worker->statistics;
}

namespace
{
#if QT_CONFIG(ssl)
	/**
	 * @brief Resolves TLS socket capabilities.
	 * @param socket Transport socket.
	 * @return SSL socket, or null.
	 */
	QSslSocket *asSslSocket(QTcpSocket *socket)
	{
		return qobject_cast<QSslSocket *>(socket);
	}
#endif

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

	/**
	 * @brief Applies compatible TCP keepalive tuning.
	 * @param socket Connected socket.
	 * @param enabled Whether keepalive is enabled.
	 */
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

WorldSocketTransport::WorldSocketTransport(QObject *parent) : WorldSocketService(parent)
{
	m_socket     = new QSslSocket(this);
	m_gmcp.write = [this](const QByteArray &bytes)
	{
		const qint64 accepted = sendPacket(bytes);
		if (accepted > 0 && !m_externalGmcpWrite)
			emit nativeProtocolWritten({bytes.left(accepted), QDateTime::currentDateTime()});
		return accepted;
	};
	m_gmcp.observed =
	    [this](const QMudGmcp::Record &record, const std::shared_ptr<const QMudGmcp::Message> &message)
	{ emit gmcpObserved(record, message); };
	m_stream.startTlsFollowsSent = [this]
	{
		if (!m_tlsEncryption || m_tlsMethod != eTlsStartTls)
			return;
		QString error;
		if (!startClientEncryption(&error))
		{
			emit socketError(error);
			m_socket->abort();
		}
	};
	m_gmcp.stateChanged     = [this](bool enabled) { emit gmcpStateChanged(enabled); };
	m_gmcp.roundTripChanged = [this](qint64 milliseconds) { emit gmcpRoundTripChanged(milliseconds); };
	m_gmcp.goodbye          = [this](const QString &message) { emit gmcpGoodbye(message); };
	m_gmcp.fatal = m_stream.fatal = [this](const QString &error)
	{
		m_stream.stopDecoding();
		if (m_activeReadError)
		{
			*m_activeReadError = error;
			return;
		}
		emit socketError(error);
		m_socket->abort();
	};

	m_stream.write = [this](const QByteArray &bytes) -> qint64
	{
		const auto accepted = sendPacket(bytes);
		if (accepted > 0)
			emit nativeProtocolWritten({bytes.left(accepted), QDateTime::currentDateTime()});
		return accepted;
	};

	connect(m_socket, &QTcpSocket::readyRead, this, &WorldSocketTransport::onReadyRead);
	connect(m_socket, &QTcpSocket::connected, this, &WorldSocketTransport::onConnected);
	connect(m_socket, &QTcpSocket::disconnected, this, &WorldSocketTransport::onDisconnected);
	connect(m_socket, &QTcpSocket::stateChanged, this,
	        [this](QAbstractSocket::SocketState state)
	        {
		        if (state == QAbstractSocket::UnconnectedState && !m_restorePrepared && m_generation != 0)
		        {
			        m_stream.close();
			        publishStatistics();
			        m_gmcp.close();
		        }
		        emit socketStateChanged(state);
	        });
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	connect(m_socket, &QTcpSocket::errorOccurred, this, &WorldSocketTransport::onErrorOccurred);
#else
	connect(m_socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error), this,
	        &WorldSocketTransport::onErrorOccurred);
#endif
#if QT_CONFIG(ssl)
	if (QSslSocket *sslSocket = asSslSocket(m_socket))
		connect(sslSocket, &QSslSocket::encrypted, this, &WorldSocketTransport::onTlsEncrypted);
#endif
}

WorldSocketTransport::~WorldSocketTransport()
{
	m_stream.close();
	publishStatistics();
}

void WorldSocketTransport::setUseUtf8(bool enabled)
{
	m_useUtf8 = enabled;
}

void WorldSocketTransport::setKeepAliveEnabled(bool enabled)
{
	m_keepAliveEnabled = enabled;
	if (m_socket && m_socket->state() == QAbstractSocket::ConnectedState)
		applyMushclientKeepAliveSettings(m_socket, enabled);
}

void WorldSocketTransport::setProxySettings(int proxyType, const QString &server, quint16 port,
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

void WorldSocketTransport::applyConnectionSettings(const WorldSocketConnectionSettings &settings)
{
	setUseUtf8(settings.useUtf8);
	m_nativeGmcp = settings.nativeGmcp;
	m_gmcp.setCoreOptions(settings.gmcpCore);
	m_disableCompression = settings.disableCompression;
	setKeepAliveEnabled(settings.keepAlive);
	m_tlsEncryption                   = settings.tlsEncryption;
	m_tlsMethod                       = settings.tlsMethod;
	m_disableTlsCertificateValidation = settings.disableTlsCertificateValidation;
	m_tlsServerName                   = settings.tlsServerName.trimmed();
	setProxySettings(settings.proxyType, settings.proxyServer, settings.proxyPort, settings.proxyUsername,
	                 settings.proxyPassword);
}

bool WorldSocketTransport::startClientEncryption(QString *errorMessage)
{
	if (errorMessage)
		errorMessage->clear();
	if (!m_socket)
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Socket transport is not initialized.");
		return false;
	}
	if (!m_tlsEncryption || m_tlsMethod != eTlsStartTls)
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("START-TLS is not enabled for this connection.");
		return false;
	}

#if QT_CONFIG(ssl)
	QSslSocket *sslSocket = asSslSocket(m_socket);
	if (!sslSocket)
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("TLS transport is unavailable.");
		return false;
	}
	if (!QSslSocket::supportsSsl())
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("TLS support is unavailable in this build.");
		return false;
	}
	if (sslSocket->state() != QAbstractSocket::ConnectedState)
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Socket is not connected.");
		return false;
	}
	if (sslSocket->isEncrypted() || sslSocket->mode() == QSslSocket::SslClientMode)
		return true;

	const QString verifyName = !m_tlsServerName.isEmpty() ? m_tlsServerName : m_lastConnectHost;
	sslSocket->setPeerVerifyMode(m_disableTlsCertificateValidation ? QSslSocket::VerifyNone
	                                                               : QSslSocket::VerifyPeer);
	if (!m_disableTlsCertificateValidation && !verifyName.isEmpty())
		sslSocket->setPeerVerifyName(verifyName);
	else
		sslSocket->setPeerVerifyName(QString());
	sslSocket->flush();
	sslSocket->startClientEncryption();
	return true;
#else
	if (errorMessage)
		*errorMessage = QStringLiteral("TLS support is unavailable in this build.");
	return false;
#endif
}

bool WorldSocketTransport::connectToHost(const QString &host, quint16 port)
{
	if (!m_socket)
		return false;

	m_lastConnectHost = host.trimmed();
	if (m_socket->state() != QAbstractSocket::UnconnectedState)
		m_socket->abort();
	applyMushclientKeepAliveSettings(m_socket, m_keepAliveEnabled);
#if QT_CONFIG(ssl)
	QSslSocket *sslSocket = asSslSocket(m_socket);
	if (m_tlsEncryption && (!sslSocket || !QSslSocket::supportsSsl()))
	{
		m_gmcp.close();
		emit socketError(QStringLiteral("TLS support is unavailable in this build."));
		return false;
	}
	if (m_tlsEncryption && m_tlsMethod == eTlsDirect)
	{
		const QString verifyName = !m_tlsServerName.isEmpty() ? m_tlsServerName : m_lastConnectHost;
		sslSocket->setPeerVerifyMode(m_disableTlsCertificateValidation ? QSslSocket::VerifyNone
		                                                               : QSslSocket::VerifyPeer);
		if (!m_disableTlsCertificateValidation && !verifyName.isEmpty())
			sslSocket->setPeerVerifyName(verifyName);
		else
			sslSocket->setPeerVerifyName(QString());
		sslSocket->connectToHostEncrypted(host, port);
	}
	else
#endif
	{
		m_socket->connectToHost(host, port);
	}
	return true;
}

void WorldSocketTransport::disconnectFromHost()
{
	if (!m_socket)
		return;
	if (m_socket->state() == QAbstractSocket::ConnectedState)
		m_socket->disconnectFromHost();
	else
		m_socket->abort();
}

qint64 WorldSocketTransport::sendPacket(const QByteArray &payload)
{
	if (!m_socket)
		return -1;

	if (payload.isEmpty())
		return 0;

	if (m_socket->state() != QAbstractSocket::ConnectedState)
		return -1;

	const qint64 accepted = m_socket->write(payload);
	if (accepted > 0)
		m_stream.observeSent(payload.left(accepted));
	return accepted;
}

QString WorldSocketTransport::peerAddressString() const
{
	if (!m_socket)
		return {};
	const QHostAddress addr = m_socket->peerAddress();
	return addr.isNull() ? QString() : addr.toString();
}

quint32 WorldSocketTransport::peerAddressV4() const
{
	if (!m_socket)
		return 0;
	const QHostAddress addr = m_socket->peerAddress();
	if (addr.protocol() != QAbstractSocket::IPv4Protocol)
		return 0;
	return addr.toIPv4Address();
}

qintptr WorldSocketTransport::nativeSocketDescriptor() const
{
	if (!m_socket)
		return static_cast<qintptr>(-1);
	return m_socket->socketDescriptor();
}

bool WorldSocketTransport::adoptConnectedSocketDescriptor(const qintptr descriptor, QString *errorMessage,
                                                          const QByteArray &telnetInputState)
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

	if (!m_stream.restoreTelnetInputState(telnetInputState))
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Invalid saved worker Telnet parser state.");
		return false;
	}

	if (!m_socket->setSocketDescriptor(descriptor, QAbstractSocket::ConnectedState, QIODevice::ReadWrite))
	{
		if (errorMessage)
			*errorMessage = m_socket->errorString();
		return false;
	}
	applyMushclientKeepAliveSettings(m_socket, m_keepAliveEnabled);
	return true;
}

void WorldSocketTransport::abortSocket()
{
	if (!m_socket)
		return;
	m_socket->abort();
	m_restorePrepared = false;
	m_stream.close();
	publishStatistics();
	m_gmcp.close();
}

bool WorldSocketTransport::isConnecting() const
{
	if (!m_socket)
		return false;
	const QAbstractSocket::SocketState state = m_socket->state();
	return state == QAbstractSocket::HostLookupState || state == QAbstractSocket::ConnectingState;
}

bool WorldSocketTransport::isConnected() const
{
	if (!m_socket)
		return false;
	return m_socket->state() == QAbstractSocket::ConnectedState;
}

void WorldSocketTransport::onReadyRead()
{
	if (m_paused || !m_socket)
		return;
	const QByteArray incoming = m_socket->readAll();
	if (incoming.isEmpty())
		return;
	QString    readError;
	QByteArray projected;
	{
		const QScopedValueRollback<QString *> readScope(m_activeReadError, &readError);
		projected = m_stream.receive(incoming);
	}
	publishStatistics();
	if (m_stream.statistics().decodedByTransport || !projected.isEmpty())
	{
		WorldSocketInputMetadata metadata;
		const auto              &state  = m_stream.statistics();
		metadata.totalCompressedBytes   = state.compressedBytes;
		metadata.totalUncompressedBytes = state.uncompressedBytes;
		metadata.mccpType               = state.mccpType == 85 ? 1 : (state.mccpType == 86 ? 2 : 0);
		if (state.decodedByTransport)
		{
			metadata.transportDecoded = true;
			metadata.receivedBytes    = incoming.size();
			metadata.debugPacket      = m_captureIncomingPackets ? incoming : QByteArray{};
		}
		emit rawDataReceived(projected, metadata);
	}
	if (!readError.isEmpty())
	{
		emit socketError(readError);
		m_socket->abort();
	}
}

void WorldSocketTransport::onConnected()
{
	if (m_socket)
		applyMushclientKeepAliveSettings(m_socket, m_keepAliveEnabled);
	emit connected();
}

void WorldSocketTransport::onDisconnected()
{
	m_stream.close();
	publishStatistics();
	m_gmcp.close();
	emit disconnected();
}

void WorldSocketTransport::onErrorOccurred()
{
	if (!m_socket)
		return;

	emit socketError(m_socket->errorString());
}

void WorldSocketTransport::onTlsEncrypted()
{
	emit tlsEncrypted();
}

void WorldSocketTransport::prepareGeneration(quint64 generation, bool preserveRestoration)
{
	m_socket->abort();
	m_generation = generation;
	m_paused     = false;
	if (!preserveRestoration)
	{
		m_restorePrepared = false;
		configureProtocolHandlers();
		m_stream.reset(m_disableCompression);
		publishStatistics();
		m_gmcp.reset(m_nativeGmcp, generation);
	}
}
quint64 WorldSocketTransport::generation() const
{
	return m_generation;
}
std::shared_ptr<QMudGmcp::Publication> WorldSocketTransport::gmcpPublication() const
{
	return m_gmcp.publication();
}
bool WorldSocketTransport::sendGmcp(const QByteArray &raw, quint64 generation)
{
	m_externalGmcpWrite = true;
	const bool accepted = m_gmcp.send(raw, generation);
	m_externalGmcpWrite = false;
	return accepted;
}
bool WorldSocketTransport::setGmcpMergePolicy(const QString &name, QMudGmcp::MergeMode mode, bool force)
{
	return m_gmcp.setMergePolicy(name, mode, force);
}
bool WorldSocketTransport::clearGmcpMergePolicy(const QString &name)
{
	return m_gmcp.clearMergePolicy(name);
}
void WorldSocketTransport::setGmcpMergePolicies(const QMudGmcp::MergeOverrides &policies)
{
	m_gmcp.setMergePolicies(policies);
}
void WorldSocketTransport::setGmcpCoreOptions(const QMudGmcp::CoreOptions &options)
{
	m_gmcp.setCoreOptions(options);
}
void WorldSocketTransport::setGmcpModules(const QMap<QString, int> &modules)
{
	m_gmcp.setModules(modules);
}
void WorldSocketTransport::setIncomingPacketDebugEnabled(bool enabled)
{
	m_captureIncomingPackets = enabled;
}

void WorldSocketTransport::freezeForReload()
{
	m_paused = false;
	onReadyRead();
	m_paused = true;
}
bool WorldSocketTransport::flushForReload()
{
	if (!m_socket)
		return true;
	m_socket->flush();
	return m_socket->bytesToWrite() == 0;
}
QByteArray WorldSocketTransport::saveTelnetInputState() const
{
	return m_stream.saveTelnetInputState();
}
QByteArray WorldSocketTransport::saveNativeState() const
{
	return m_gmcp.publication()->current.load()->native ? m_gmcp.save() : QByteArray{};
}
bool WorldSocketTransport::restoreNativeState(const QByteArray &state)
{
	if (isConnected() || isConnecting())
		return false;
	if (state.isEmpty())
		m_gmcp.prepareLegacyRestoration();
	else if (!m_gmcp.restore(state, m_generation, false))
		return false;
	m_nativeGmcp = m_gmcp.publication()->current.load()->native;
	configureProtocolHandlers();
	m_stream.reset(m_disableCompression);
	publishStatistics();
	m_restorePrepared = true;
	return true;
}
void WorldSocketTransport::activateRestoration()
{
	Q_ASSERT(m_restorePrepared);
	m_restorePrepared = false;
	m_gmcp.activateRestoration(m_generation);
}

void WorldSocketTransport::pauseTransport(bool paused)
{
	m_paused = paused;
	if (!paused)
		onReadyRead();
}

void WorldSocketTransport::setCompressionDisabled(bool disabled)
{
	m_disableCompression = disabled;
	m_stream.setCompressionDisabled(disabled);
}

namespace
{
	constexpr unsigned char Iac = 255, Will = 251, Wont = 252, Do = 253, Dont = 254, Sb = 250, Se = 240;
	constexpr unsigned char Mccp1 = 85, Mccp2 = 86;
} // namespace
/**
 * @brief Worker-owned Telnet framing and compression state, independent of protocol handlers.
 */
struct WorldSocketStream::Impl
{
		/**
		 * @brief Binds stream callbacks to their owner before any decoding operation can run.
		 * @param stream Stream owning this implementation.
		 */
		explicit Impl(WorldSocketStream &stream) : owner(&stream)
		{
		}
		WorldSocketStream *const       owner;
		QMap<unsigned char, Handler>   handlers;
		WorldSocketTransportStatistics statistics;
		bool                           disableCompression{false};
		std::array<bool, 2>            compressionAccepted{};
		std::array<int, 2>             compressionOffers{-1, -1};
		bool                           failed{false};
		bool                           inflaterReady{false};
		z_stream                       inflater{};
		int                            phase{0};
		bool                           handledInput{false};
		unsigned char                  verb{0}, option{0};
		QByteArray                     pending, payload;
		int                            outgoingPhase{0};
		unsigned char                  outgoingOption{0};
		bool                           captureOutgoing{false};
		QByteArray                     outgoingPayload;
		/**
		 * @brief Releases the active inflater without changing lifetime statistics.
		 */
		void                           endInflater()
		{
			if (inflaterReady)
				inflateEnd(&inflater);
			inflater            = {};
			inflaterReady       = false;
			statistics.mccpType = 0;
		}
		/**
		 * @brief Stops decoding and forwards a transport error.
		 * @param error Error text.
		 */
		void fail(const QString &error)
		{
			failed = true;
			if (owner->fatal)
				owner->fatal(error);
		}
		/**
		 * @brief Writes a complete transport negotiation.
		 * @param bytes Framed command.
		 * @return Whether all bytes were accepted.
		 */
		bool write(const QByteArray &bytes)
		{
			if (owner->write && owner->write(bytes) == bytes.size())
				return true;
			fail(QStringLiteral("Unable to send complete Telnet protocol data."));
			return false;
		}
		/**
		 * @brief Dispatches a completed Telnet token to its registered handler or the runtime projection.
		 * @param output Bytes for runtime processing.
		 */
		void token(QByteArray &output)
		{
			if (const auto handler = handlers.constFind(option); handler != handlers.cend())
			{
				if (handler->received)
					handler->received(verb, payload);
			}
			else if (option == Mccp1 || option == Mccp2)
			{
				if (verb == Will)
				{
					const size_t slot = option == Mccp2 ? 1 : 0;
					const bool   accept =
					    !disableCompression && !(option == Mccp1 && compressionOffers[1] == 1);
					if (compressionOffers[slot] != (accept ? 1 : 0))
					{
						compressionOffers[slot]   = accept ? 1 : 0;
						compressionAccepted[slot] = accept;
						write(QByteArray::fromHex(accept ? "fffd" : "fffe") +
						      QByteArray(1, static_cast<char>(option)));
					}
				}
				else if (verb == Wont)
				{
					const size_t slot = option == Mccp2 ? 1 : 0;
					if (compressionAccepted[slot])
						write(QByteArray::fromHex("fffe") + QByteArray(1, static_cast<char>(option)));
					compressionAccepted[slot] = false;
					compressionOffers[slot]   = -1;
				}
				else if (verb == Sb && !inflaterReady)
				{
					// The preference controls offers; bytes following this marker are compressed
					// even when the preference changed after negotiation or a refusal is in flight.
					if (inflateInit(&inflater) != Z_OK)
						fail(QStringLiteral("Cannot initialize native MCCP decoder."));
					else
					{
						inflaterReady       = true;
						statistics.mccpType = option;
					}
				}
				else if (verb == Do)
					write(QByteArray::fromHex("fffc") + QByteArray(1, static_cast<char>(option)));
			}
			else
				output += pending;
			pending.clear();
			payload.clear();
			phase = 0;
		}
		/**
		 * @brief Consumes one decompressed byte.
		 * @param c Byte value.
		 * @param output Legacy projection.
		 */
		void byte(unsigned char c, QByteArray &output)
		{
			if (phase == 0)
			{
				if (c == Iac)
				{
					pending = QByteArray(1, static_cast<char>(c));
					phase   = 1;
				}
				else
					output += static_cast<char>(c);
				return;
			}
			if (phase < 4 || !handledInput)
				pending += static_cast<char>(c);
			switch (phase)
			{
			case 1:
				verb = c;
				if (c == Will || c == Wont || c == Do || c == Dont)
					phase = 2;
				else if (c == Sb)
					phase = 3;
				else
				{
					output += pending;
					pending.clear();
					phase = 0;
				}
				break;
			case 2:
				option = c;
				token(output);
				break;
			case 3:
				option       = c;
				handledInput = handlers.contains(option) || option == Mccp1 || option == Mccp2;
				payload.clear();
				phase = 4;
				break;
			case 4:
				if (c == Iac)
					phase = 5;
				else if (option == Mccp1 && c == Se && payload == QByteArray(1, static_cast<char>(Will)))
					token(output);
				else if (handledInput)
					payload += static_cast<char>(c);
				break;
			case 5:
				if (c == Se)
					token(output);
				else
				{
					if (c == Iac && handledInput)
						payload += static_cast<char>(Iac);
					else if (handledInput)
					{
						payload += static_cast<char>(Iac);
						payload += static_cast<char>(c);
					}
					phase = 4;
				}
				break;
			default:
				Q_UNREACHABLE();
			}
		}
};
WorldSocketStream::WorldSocketStream() : d(std::make_unique<Impl>(*this))
{
}
WorldSocketStream::~WorldSocketStream()
{
	impl().endInflater();
}
void WorldSocketStream::setHandler(unsigned char option, const Handler &handler)
{
	impl().handlers.insert(option, handler);
}
void WorldSocketStream::clearHandlers()
{
	impl().handlers.clear();
}
void WorldSocketStream::reset(bool disableCompression)
{
	impl().endInflater();
	impl().statistics.decodedByTransport = !impl().handlers.isEmpty();
	impl().disableCompression            = disableCompression;
	impl().compressionAccepted           = {};
	impl().compressionOffers             = {-1, -1};
	impl().failed                        = false;
	impl().phase = impl().outgoingPhase = 0;
	impl().pending.clear();
	impl().payload.clear();
	impl().outgoingPayload.clear();
}
void WorldSocketStream::close()
{
	impl().endInflater();
	impl().failed = true;
}
QByteArray WorldSocketStream::saveTelnetInputState() const
{
	const auto &data = impl();
	if (data.phase == 0)
		return {};
	QByteArray  state;
	QDataStream stream(&state, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_6_0);
	stream << static_cast<quint8>(data.phase) << data.verb << data.option << data.pending << data.payload;
	return state;
}
bool WorldSocketStream::restoreTelnetInputState(const QByteArray &state)
{
	quint8     phase = 0, verb = 0, option = 0;
	QByteArray pending, payload;
	if (!state.isEmpty())
	{
		QDataStream stream(state);
		stream.setVersion(QDataStream::Qt_6_0);
		stream >> phase >> verb >> option >> pending >> payload;
		if (stream.status() != QDataStream::Ok || !stream.atEnd() || phase < 1 || phase > 5)
			return false;
	}
	auto &data        = impl();
	data.phase        = phase;
	data.verb         = verb;
	data.option       = option;
	data.pending      = pending;
	data.payload      = payload;
	data.handledInput = data.handlers.contains(option) || option == Mccp1 || option == Mccp2;
	return true;
}
void WorldSocketStream::setCompressionDisabled(bool disabled)
{
	impl().disableCompression = disabled;
}
const WorldSocketTransportStatistics &WorldSocketStream::statistics() const
{
	return impl().statistics;
}
QByteArray WorldSocketStream::receive(const QByteArray &input)
{
	auto &data = impl();
	if (data.handlers.isEmpty())
		return input;
	QByteArray output;
	qsizetype  offset = 0;
	while (offset < input.size() && !data.failed)
	{
		if (!data.inflaterReady)
		{
			if (data.phase == 0)
			{
				const qsizetype next = input.indexOf(static_cast<char>(Iac), offset);
				const qsizetype end  = next < 0 ? input.size() : next;
				output += QByteArrayView(input).sliced(offset, end - offset);
				offset = end;
				if (offset == input.size())
					break;
			}
			data.byte(static_cast<unsigned char>(input.at(offset++)), output);
			continue;
		}
		const auto remaining =
		    static_cast<uInt>(qMin<qsizetype>(input.size() - offset, std::numeric_limits<uInt>::max()));
		data.inflater.next_in  = reinterpret_cast<Bytef *>(const_cast<char *>(input.constData() + offset));
		data.inflater.avail_in = remaining;
		std::array<char, 16384> buffer{};
		int                     result;
		do
		{
			data.inflater.next_out  = reinterpret_cast<Bytef *>(buffer.data());
			data.inflater.avail_out = buffer.size();
			result                  = inflate(&data.inflater, Z_SYNC_FLUSH);
			const auto count        = static_cast<qsizetype>(buffer.size() - data.inflater.avail_out);
			data.statistics.uncompressedBytes += count;
			for (qsizetype i = 0; i < count && !data.failed;)
			{
				if (data.phase == 0)
				{
					const auto view = QByteArrayView(buffer.data(), count);
					const auto next = view.indexOf(static_cast<char>(Iac), i);
					const auto end  = next < 0 ? count : next;
					output += view.sliced(i, end - i);
					i = end;
					if (i == count)
						break;
				}
				data.byte(static_cast<unsigned char>(buffer[static_cast<size_t>(i++)]), output);
			}
			if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR)
			{
				data.fail(QStringLiteral("Invalid native MCCP stream."));
				break;
			}
		} while (!data.failed && data.inflaterReady && result == Z_OK &&
		         (data.inflater.avail_in > 0 || data.inflater.avail_out == 0));
		const auto consumed = remaining - data.inflater.avail_in;
		offset += consumed;
		data.statistics.compressedBytes += consumed;
		if (result == Z_STREAM_END)
			data.endInflater();
		else if (consumed == 0)
			break;
	}
	return output;
}
void WorldSocketStream::observeSent(const QByteArray &bytes)
{
	auto     &data   = impl();
	qsizetype offset = 0;
	while (offset < bytes.size())
	{
		if (data.outgoingPhase == 0)
		{
			const auto next = bytes.indexOf(static_cast<char>(Iac), offset);
			if (next < 0)
				return;
			offset = next;
		}
		const auto c = static_cast<unsigned char>(bytes.at(offset++));
		switch (data.outgoingPhase)
		{
		case 0:
			if (c == Iac)
				data.outgoingPhase = 1;
			break;
		case 1:
			if (c == Sb)
				data.outgoingPhase = 2;
			else if (c == Will || c == Wont || c == Do || c == Dont)
				data.outgoingPhase = 5;
			else
				data.outgoingPhase = 0;
			break;
		case 2:
			data.outgoingOption  = c;
			data.captureOutgoing = data.handlers.contains(c) || c == 46;
			data.outgoingPayload.clear();
			data.outgoingPhase = 3;
			break;
		case 3:
			if (c == Iac)
				data.outgoingPhase = 4;
			else if (data.captureOutgoing)
				data.outgoingPayload += static_cast<char>(c);
			break;
		case 4:
			if (c == Se)
			{
				if (const auto handler = data.handlers.constFind(data.outgoingOption);
				    handler != data.handlers.cend() && handler->sent)
					handler->sent(data.outgoingPayload);
				if (data.outgoingOption == 46 && data.outgoingPayload == QByteArray(1, 1) &&
				    startTlsFollowsSent)
					startTlsFollowsSent();
				data.outgoingPayload.clear();
				data.outgoingPhase = 0;
			}
			else
			{
				if (data.captureOutgoing)
				{
					if (c != Iac)
						data.outgoingPayload += static_cast<char>(Iac);
					data.outgoingPayload += static_cast<char>(c);
				}
				data.outgoingPhase = 3;
			}
			break;
		case 5:
			data.outgoingPhase = 0;
			break;
		default:
			Q_UNREACHABLE();
		}
	}
}
void WorldSocketTransport::configureProtocolHandlers()
{
	m_stream.clearHandlers();
	if (m_nativeGmcp)
		m_stream.setHandler(201, {[this](unsigned char verb, const QByteArray &payload)
		                          { m_gmcp.receive(verb, payload); },
		                          [this](const QByteArray &payload) { m_gmcp.observeSent(payload); }});
}
void WorldSocketTransport::publishStatistics()
{
	const auto &statistics = m_stream.statistics();
	if (m_publishedStatistics != statistics)
	{
		m_publishedStatistics = statistics;
		m_statistics->current.store(std::make_shared<const WorldSocketTransportStatistics>(statistics),
		                            std::memory_order_release);
	}
}
std::shared_ptr<WorldSocketStatisticsPublication> WorldSocketTransport::transportStatistics() const
{
	return m_statistics;
}
void WorldSocketStream::stopDecoding()
{
	impl().failed = true;
}
