/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: SoundEngine.cpp
 * Role: Process-wide threaded sound playback service and reusable multimedia-object cache.
 */

#include "SoundEngine.h"

#include "helpers/PluginPathUtils.h"
#include "helpers/SoundCacheUtils.h"
#include "helpers/SoundFileRoutingUtils.h"
#include "scripting/ScriptingErrors.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
// ReSharper disable once CppUnusedIncludeDirective
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMetaObject>
// ReSharper disable once CppUnusedIncludeDirective
#include <QMutex>
#include <QMutexLocker>
// ReSharper disable once CppUnusedIncludeDirective
#include <QRunnable>
// ReSharper disable once CppUnusedIncludeDirective
#include <QThread>
#include <QThreadPool>
#include <QVector>
#include <QWaitCondition>
#include <atomic>
#include <deque>
#include <optional>
#include <utility>

#if QMUD_ENABLE_SOUND
#include <QAudioDevice>
#include <QAudioOutput>
#include <QMediaDevices>
#include <QMediaPlayer>
#include <QSoundEffect>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>
#endif

namespace
{
#if QMUD_ENABLE_SOUND
	struct SoundMemoryResource;
#endif

	struct SoundSessionPublishedState
	{
			std::atomic<std::shared_ptr<const SoundEngineSession::Snapshot>> snapshot{
			    std::make_shared<SoundEngineSession::Snapshot>()};
	};

	struct SoundCommandCompletion
	{
			QMutex         mutex;
			QWaitCondition changed;
			int            result{eCannotPlaySound};
			int            selectedBuffer{0};
			bool           finished{false};

			void           complete(const int value, const int buffer = 0)
			{
				QMutexLocker locker(&mutex);
				result         = value;
				selectedBuffer = buffer;
				finished       = true;
				changed.wakeAll();
			}

			int wait(int *buffer = nullptr)
			{
				QMutexLocker locker(&mutex);
				while (!finished)
					changed.wait(&mutex);
				if (buffer)
					*buffer = selectedBuffer;
				return result;
			}
	};

	enum class SoundCommandKind : quint8
	{
		RegisterSession,
		InitializePlayback,
		UpdateStartupDirectory,
		PlayFile,
		PlayMemory,
		MemoryDigestReady,
		MemoryFileReady,
		ReserveLuaAudioChannel,
		ConfirmLuaAudioOwnership,
		Stop,
		PrepareFile,
		CommitPrepared,
		CancelPrepared,
		CloseSession,
		ConfigureCache
	};

	struct SoundCommand
	{
			SoundCommandKind                            kind{SoundCommandKind::PlayFile};
			quint64                                     sessionId{0};
			quint64                                     token{0};
			int                                         buffer{0};
			QString                                     text;
			QByteArray                                  bytes;
			QByteArray                                  digest;
			bool                                        loop{false};
			double                                      volume{0.0};
			double                                      pan{0.0};
			quint64                                     luaAudioGeneration{0};
			bool                                        requireReservation{false};
			SoundEngine::CachePolicy                    cachePolicy;
			std::shared_ptr<SoundSessionPublishedState> publishedState;
			std::shared_ptr<SoundCommandCompletion>     completion;
			std::function<void(int, quint64)>           luaAudioOwnershipReleased;
#if QMUD_ENABLE_SOUND
			std::shared_ptr<SoundMemoryResource> memoryResource;
#endif
	};

	class SoundWorker;

	struct SoundCommandIngress
	{
			QMutex                   mutex;
			std::deque<SoundCommand> commands;
			SoundWorker             *worker{nullptr};
			bool                     accepting{false};
			bool                     drainPosted{false};
	};

	enum class PlaybackInitializationState : quint8
	{
		Idle,
		Initializing,
		Ready,
		Abandoned
	};

#if QMUD_ENABLE_SOUND
	double normalizeSoundVolume(double volume)
	{
		if (volume > 0.0 || volume < -100.0)
			volume = 0.0;
		return qBound(0.0, 1.0 + (volume / 100.0), 1.0);
	}
#endif
} // namespace

struct SoundEngineSession::State
{
		std::shared_ptr<SoundCommandIngress>        ingress;
		std::shared_ptr<SoundSessionPublishedState> publishedState{
		    std::make_shared<SoundSessionPublishedState>()};
		std::atomic_bool     closed{false};
		std::atomic<quint64> nextPreparationToken{1};
		quint64              sessionId{0};

		bool                 submit(SoundCommand command, bool wait, int *result = nullptr,
		                            int *selectedBuffer = nullptr) const;
};

#if QMUD_ENABLE_SOUND
namespace
{
	enum class SoundBackendKind : quint8
	{
		Effect,
		MediaPlayer
	};

	struct SoundSourceKey
	{
			QString          identity;
			qint64           size{0};
			qint64           modifiedMsecs{0};
			SoundBackendKind backend{SoundBackendKind::Effect};
			QByteArray       deviceId;

			bool             operator==(const SoundSourceKey &) const = default;

			/**
			 * @brief Hashes one loaded-source identity for process-wide cache accounting.
			 * @param key Source identity to hash.
			 * @param seed Caller-provided hash seed.
			 * @return Hash value for the source identity.
			 */
			friend size_t    qHash(const SoundSourceKey &key, const size_t seed = 0) noexcept
			{
				return qHashMulti(seed, key.identity, key.size, key.modifiedMsecs,
				                  static_cast<quint8>(key.backend), key.deviceId);
			}
	};

	struct SoundMemoryResource
	{
			QString    fileName;
			QByteArray digest;
			qint64     size{0};

			/** @brief Removes the retained temporary media file. */
			~SoundMemoryResource()
			{
				static_cast<void>(QFile::remove(fileName));
			}
	};

	struct SoundBackend
	{
			SoundSourceKey                       key;
			quint64                              backendId{0};
			std::shared_ptr<SoundMemoryResource> memoryResource;
			std::unique_ptr<QSoundEffect>        effect;
			std::unique_ptr<QMediaPlayer>        player;
			QAudioOutput                        *audioOutput{nullptr};
			quint64                              sessionId{0};
			quint64                              assignmentGeneration{0};
			quint64                              preparationToken{0};
			int                                  channel{-1};
			bool                                 started{false};
			bool                                 initializationInProgress{false};
			bool                                 failed{false};
			bool                                 retiring{false};
			QString                              failureMessage;
	};

	struct SoundBackendLease
	{
			SoundBackend *backend{nullptr};
			QString       failureMessage;
	};

	struct SoundChannel
	{
			SoundBackend *backend{nullptr};
			quint64       assignmentGeneration{0};
			quint64       luaAudioGeneration{0};
			quint64       pendingMemoryOperationId{0};
			bool          allocated{false};
			bool          playing{false};
			bool          playbackStarted{false};
			bool          looping{false};
			bool          luaAudioOwnershipConfirmed{false};
			qint64        completedAtMsecs{0};
			double        volume{1.0};
			double        pan{0.0};
	};

	struct PendingMemoryPlay
	{
			quint64                                 sessionId{0};
			quint64                                 operationId{0};
			quint64                                 assignmentGeneration{0};
			int                                     channelIndex{-1};
			bool                                    loop{false};
			double                                  volume{0.0};
			double                                  pan{0.0};
			quint64                                 luaAudioGeneration{0};
			QByteArray                              payload;
			std::shared_ptr<SoundCommandCompletion> completion;
	};

	struct PreparedSound
	{
			SoundBackend *backend{nullptr};
			int           result{eOK};
			QString       failureMessage;
	};

	struct WorkerSession
	{
			quint64                                                     sessionId{0};
			QString                                                     startupDirectory;
			std::shared_ptr<SoundSessionPublishedState>                 publishedState;
			std::array<SoundChannel, SoundEngineSession::kChannelCount> channels{};
			QHash<quint64, PreparedSound>                               prepared;
			QVector<QPair<int, quint64>>                                pendingLuaAudioCompletions;
			std::function<void(int, quint64)>                           luaAudioOwnershipReleased;
			quint64                                                     nextAssignmentGeneration{1};
	};

	/** @brief Cache accounting and deferred-maintenance state sampled on the sound worker thread. */
	struct SoundWorkerCacheState
	{
			qint64    totalBytes{0};
			qsizetype sourceCount{0};
			qsizetype backendCount{0};
			bool      maintenancePending{false};
	};

	/** @brief Worker cache states captured after configuration and consecutive queued maintenance events. */
	struct SoundWorkerCacheMaintenanceRun
	{
			SoundWorkerCacheState configured;
			SoundWorkerCacheState firstStep;
			SoundWorkerCacheState secondStep;
	};

	/** @brief Source-free playback backends initialized before the first sound request. */
	struct SoundWorkerPlaybackInitializationState
	{
			bool effectReady{false};
			bool mediaPlayerReady{false};
	};

	class SoundWorker final : public QObject
	{
		public:
			using PlaybackError                = std::function<void(quint64, const QString &)>;
			using PlaybackInitializationActive = std::function<bool()>;
			using PlaybackInitialized          = std::function<bool()>;

			/**
			 * @brief Creates the worker-side sound engine state.
			 * @param ingress Shared command queue owned with the public engine.
			 * @param playbackError Callback that reports playback failures to the engine owner.
			 * @param playbackInitializationActive Callback that reports whether startup still needs initialization.
			 * @param playbackInitialized Callback that reports completion of explicit playback initialization.
			 */
			SoundWorker(std::shared_ptr<SoundCommandIngress> ingress, PlaybackError playbackError,
			            PlaybackInitializationActive playbackInitializationActive,
			            PlaybackInitialized          playbackInitialized)
			    : m_ingress(std::move(ingress)), m_playbackError(std::move(playbackError)),
			      m_playbackInitializationActive(std::move(playbackInitializationActive)),
			      m_playbackInitialized(std::move(playbackInitialized))
			{
				m_clock.start();
			}

			void initialize()
			{
				Q_ASSERT(QThread::currentThread() == thread());
				m_maintenanceTimer = std::make_unique<QTimer>();
				m_maintenanceTimer->setTimerType(Qt::VeryCoarseTimer);
				m_maintenanceTimer->setInterval(SoundEngine::CachePolicy::kMaintenanceIntervalSeconds * 1000);
				connect(m_maintenanceTimer.get(), &QTimer::timeout, this,
				        [this] { scheduleMaintenance(true); });
				m_maintenanceTimer->start();
				ensureMemoryPreparationPool();
			}

			void drain()
			{
				Q_ASSERT(QThread::currentThread() == thread());
				std::deque<SoundCommand> commands;
				{
					QMutexLocker locker(&m_ingress->mutex);
					commands.swap(m_ingress->commands);
					m_ingress->drainPosted = false;
				}
				for (SoundCommand &command : commands)
				{
					if (m_playbackInitializationAbandoned)
					{
						if (command.completion)
							command.completion->complete(eCannotPlaySound);
						continue;
					}
					execute(command);
				}
			}

			void shutdown()
			{
				Q_ASSERT(QThread::currentThread() == thread());
				m_shuttingDown = true;
				drain();
				for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it)
					closeSession(it.key(), false);
				m_sessions.clear();
				if (m_memoryPreparationPool)
					m_memoryPreparationPool->waitForDone();
				clearBackends();
				m_memoryResources.clear();
				m_preinitializedEffectBackend.reset();
				m_preinitializedMediaBackend.reset();
				m_maintenanceTimer.reset();
				m_memoryPreparationPool.reset();
				m_mediaDevices.reset();
			}

			/** @brief Shuts down an abandoned startup worker and exits its event loop. */
			void retire()
			{
				Q_ASSERT(QThread::currentThread() == thread());
				shutdown();
				QThread::currentThread()->quit();
			}

			/**
			 * @brief Installs a one-shot test barrier at the entered initialization boundary.
			 * @param barrier Barrier invoked after initialization is accepted and before multimedia construction.
			 */
			void setPlaybackInitializationBarrierForTest(std::function<void()> barrier)
			{
				Q_ASSERT(QThread::currentThread() == thread());
				m_playbackInitializationBarrierForTest = std::move(barrier);
			}

			/**
			 * @brief Adds an audio-object-free backend record for deterministic cache-maintenance tests.
			 * @param identity Distinct test source identity.
			 * @param sourceBytes Accounted source size.
			 * @param idle Whether the source is fully idle and eviction-eligible.
			 * @param usedAtMsecs Positive last-use timestamp used for LRU ordering.
			 */
			void addCacheSourceForTest(const QString &identity, const qint64 sourceBytes, const bool idle,
			                           const qint64 usedAtMsecs)
			{
				Q_ASSERT(QThread::currentThread() == thread());
				SoundSourceKey key;
				key.identity            = identity;
				key.size                = sourceBytes;
				auto backend            = std::make_unique<SoundBackend>();
				backend->key            = key;
				backend->backendId      = m_nextBackendId++;
				const quint64 backendId = backend->backendId;
				const auto    inserted =
				    m_cache.insertActive(key, sourceBytes, backendId, std::move(backend), usedAtMsecs);
				Q_ASSERT(inserted.backend);
				if (idle && inserted.backend)
					m_cache.markIdle(key, backendId, usedAtMsecs);
			}

			/**
			 * @brief Applies a policy, simulates one timer tick, and delivers its incremental maintenance events.
			 * @param policy Cache policy to apply.
			 * @return Worker cache state after configuration and each consecutive maintenance event.
			 */
			[[nodiscard]] SoundWorkerCacheMaintenanceRun
			configureCacheAndRunMaintenanceForTest(const SoundEngine::CachePolicy policy)
			{
				Q_ASSERT(QThread::currentThread() == thread());
				SoundCommand command;
				command.kind        = SoundCommandKind::ConfigureCache;
				command.cachePolicy = policy;
				execute(command);
				const SoundWorkerCacheState configured = cacheStateForTest();
				scheduleMaintenance(true);
				QCoreApplication::sendPostedEvents(this, maintenanceEventType());
				const SoundWorkerCacheState firstStep = cacheStateForTest();
				QCoreApplication::sendPostedEvents(this, maintenanceEventType());
				return {configured, firstStep, cacheStateForTest()};
			}

			/**
			 * @brief Delivers a prepared memory resource after its waiting play was canceled.
			 * @return `true` when the canceled resource was incorrectly retained in the weak index.
			 */
			[[nodiscard]] bool canceledMemoryResourceIsIndexedForTest()
			{
				Q_ASSERT(QThread::currentThread() == thread());
				SoundCommand command;
				command.kind                   = SoundCommandKind::MemoryFileReady;
				command.text                   = QStringLiteral("memory:test-canceled");
				command.memoryResource         = std::make_shared<SoundMemoryResource>();
				command.memoryResource->digest = QByteArrayLiteral("test-canceled");
				m_pendingMemoryMaterializations.insert(command.text, {m_nextMemoryOperationId++});
				handleMemoryFileReady(command);
				return m_memoryResources.contains(command.text);
			}

			/**
			 * @brief Attaches a lightweight backend to a reservation and completes it through normal worker logic.
			 * @param sessionId Session owning the reservation.
			 * @param buffer One-based reserved channel to complete.
			 * @return `true` when the reservation was transitioned to completed state.
			 */
			[[nodiscard]] bool completeReservedChannelForTest(const quint64 sessionId, const int buffer)
			{
				Q_ASSERT(QThread::currentThread() == thread());
				auto sessionIt = m_sessions.find(sessionId);
				if (sessionIt == m_sessions.end() || buffer < 1 || buffer > SoundEngineSession::kChannelCount)
				{
					return false;
				}
				WorkerSession &session      = sessionIt.value();
				const int      channelIndex = buffer - 1;
				SoundChannel  &channel      = session.channels[static_cast<std::size_t>(channelIndex)];
				if (!channel.allocated || channel.backend || channel.playing || channel.playbackStarted ||
				    channel.looping || channel.luaAudioGeneration == 0)
				{
					return false;
				}

				SoundSourceKey key;
				key.identity            = QStringLiteral("test:completed:%1:%2").arg(sessionId).arg(buffer);
				key.size                = 1;
				auto backend            = std::make_unique<SoundBackend>();
				backend->key            = key;
				backend->backendId      = m_nextBackendId++;
				const quint64 backendId = backend->backendId;
				const qint64  now       = currentTimeMsecs();
				const auto inserted = m_cache.insertActive(key, key.size, backendId, std::move(backend), now);
				if (!inserted.backend)
					return false;

				channel.backend                        = inserted.backend;
				channel.assignmentGeneration           = session.nextAssignmentGeneration++;
				channel.playing                        = true;
				channel.playbackStarted                = true;
				inserted.backend->sessionId            = sessionId;
				inserted.backend->channel              = channelIndex;
				inserted.backend->assignmentGeneration = channel.assignmentGeneration;
				inserted.backend->started              = true;
				publishSnapshotIfChanged(session);
				completeChannelPlayback(session, channelIndex);
				return true;
			}

			/**
			 * @brief Creates a lightweight prepared occurrence and runs its production failure transition.
			 * @param sessionId Session receiving the prepared occurrence.
			 * @param token Preparation identity allocated by the session.
			 * @param message Diagnostic supplied to handleBackendError().
			 * @param initializationFailure Whether to fail during backend initialization rather than preparation.
			 * @return `true` when the failed occurrence remains available for commit or cancellation.
			 */
			[[nodiscard]] bool failedPreparedSoundForTest(const quint64 sessionId, const quint64 token,
			                                              const QString &message,
			                                              const bool     initializationFailure)
			{
				Q_ASSERT(QThread::currentThread() == thread());
				auto sessionIt = m_sessions.find(sessionId);
				if (sessionIt == m_sessions.end() || token == 0 || sessionIt->prepared.contains(token))
					return false;

				SoundSourceKey key;
				key.identity       = QStringLiteral("test:prepared-error:%1:%2").arg(sessionId).arg(token);
				key.size           = 1;
				auto backend       = std::make_unique<SoundBackend>();
				backend->key       = key;
				backend->backendId = m_nextBackendId++;
				const quint64 backendId = backend->backendId;
				const auto    inserted =
				    m_cache.insertActive(key, key.size, backendId, std::move(backend), currentTimeMsecs());
				if (!inserted.backend)
					return false;

				if (initializationFailure)
				{
					inserted.backend->initializationInProgress = true;
					handleBackendError(*inserted.backend, message);
					SoundBackendLease lease = finishBackendInitialization(*inserted.backend);
					sessionIt->prepared.insert(
					    token, {lease.backend, eCannotPlaySound, std::move(lease.failureMessage)});
				}
				else
				{
					inserted.backend->sessionId        = sessionId;
					inserted.backend->preparationToken = token;
					sessionIt->prepared.insert(token, {inserted.backend, eOK, {}});
					handleBackendError(*inserted.backend, message);
				}
				const auto preparedIt = sessionIt->prepared.constFind(token);
				return preparedIt != sessionIt->prepared.constEnd() && !preparedIt->backend &&
				       preparedIt->result == eCannotPlaySound && preparedIt->failureMessage == message;
			}

			/**
			 * @brief Returns cache accounting and pending maintenance state on the worker thread.
			 * @return Coherent test-only cache state.
			 */
			[[nodiscard]] SoundWorkerCacheState cacheStateForTest() const
			{
				Q_ASSERT(QThread::currentThread() == thread());
				return {m_cache.totalBytes(), m_cache.sourceCount(), m_cache.backendCount(),
				        m_maintenancePending};
			}

			/**
			 * @brief Reports whether both real playback backend types were initialized at worker startup.
			 * @return Source-free backend availability for each playback implementation.
			 */
			[[nodiscard]] SoundWorkerPlaybackInitializationState playbackInitializationStateForTest() const
			{
				Q_ASSERT(QThread::currentThread() == thread());
				return {m_preinitializedEffectBackend != nullptr, m_preinitializedMediaBackend != nullptr};
			}

		public:
			/**
			 * @brief Runs a queued cache-maintenance event at low event-loop priority.
			 * @param event Event delivered to the sound worker.
			 * @return `true` when the event was handled.
			 */
			bool event(QEvent *event) override
			{
				if (event && event->type() == maintenanceEventType())
				{
					if (!m_shuttingDown)
						performMaintenance();
					m_maintenancePending = false;
					if (!m_shuttingDown && cacheNeedsEviction(currentTimeMsecs()))
						scheduleMaintenance(false);
					return true;
				}
				return QObject::event(event);
			}

		private:
			void execute(SoundCommand &command)
			{
				int result         = eCannotPlaySound;
				int selectedBuffer = 0;
				switch (command.kind)
				{
				case SoundCommandKind::InitializePlayback:
					if (!m_playbackInitializationActive())
					{
						m_playbackInitializationAbandoned = true;
						break;
					}
					if (m_playbackInitializationBarrierForTest)
						std::exchange(m_playbackInitializationBarrierForTest, {})();
					if (!m_playbackInitializationActive())
					{
						m_playbackInitializationAbandoned = true;
						break;
					}
					initializePlaybackBackends();
					if (!m_playbackInitialized())
					{
						m_playbackInitializationAbandoned = true;
						m_preinitializedEffectBackend.reset();
						m_preinitializedMediaBackend.reset();
						m_mediaDevices.reset();
						m_deviceId.clear();
					}
					result = eOK;
					break;
				case SoundCommandKind::RegisterSession:
				{
					WorkerSession session;
					session.sessionId                 = command.sessionId;
					session.startupDirectory          = command.text;
					session.publishedState            = command.publishedState;
					session.luaAudioOwnershipReleased = std::move(command.luaAudioOwnershipReleased);
					m_sessions.insert(command.sessionId, std::move(session));
					result = eOK;
					break;
				}
				case SoundCommandKind::PlayFile:
					result = playFile(command, selectedBuffer);
					break;
				case SoundCommandKind::UpdateStartupDirectory:
				{
					auto sessionIt = m_sessions.find(command.sessionId);
					if (sessionIt != m_sessions.end())
					{
						sessionIt->startupDirectory = command.text;
						result                      = eOK;
					}
					break;
				}
				case SoundCommandKind::PlayMemory:
					result = beginPlayMemory(command, selectedBuffer);
					break;
				case SoundCommandKind::MemoryDigestReady:
					handleMemoryDigestReady(command);
					result = eOK;
					break;
				case SoundCommandKind::MemoryFileReady:
					handleMemoryFileReady(command);
					result = eOK;
					break;
				case SoundCommandKind::ReserveLuaAudioChannel:
					result = reserveLuaAudioChannel(command, selectedBuffer);
					break;
				case SoundCommandKind::ConfirmLuaAudioOwnership:
					result = confirmLuaAudioOwnership(command);
					break;
				case SoundCommandKind::Stop:
					result = stop(command.sessionId, command.buffer, command.luaAudioGeneration);
					break;
				case SoundCommandKind::PrepareFile:
					prepareFile(command);
					result = eOK;
					break;
				case SoundCommandKind::CommitPrepared:
					result = commitPrepared(command);
					break;
				case SoundCommandKind::CancelPrepared:
					cancelPrepared(command.sessionId, command.token);
					result = eOK;
					break;
				case SoundCommandKind::CloseSession:
					result = closeSession(command.sessionId, true) ? eOK : eCannotPlaySound;
					break;
				case SoundCommandKind::ConfigureCache:
					m_cachePolicy = SoundEngine::CachePolicy::normalized(command.cachePolicy);
					result        = eOK;
					break;
				}
				if (command.completion)
					command.completion->complete(result, selectedBuffer);
			}

			/**
			 * @brief Resolves and validates a file-backed sound without initializing multimedia state.
			 * @param session Session providing the QMud-home root.
			 * @param fileName Legacy QMud-home-relative or absolute-safe sound path.
			 * @param resolved Destination receiving the contained absolute file path.
			 * @return Existing regular-file metadata, or no value when resolution fails.
			 */
			static std::optional<QFileInfo> resolveFileInfo(const WorkerSession &session,
			                                                const QString &fileName, QString &resolved)
			{
				const QString relative = QMudPluginPathUtils::legacyPathRelativeToQmudHome(fileName);
				QStringList   candidates;
				if (!relative.isEmpty() &&
				    !relative.startsWith(QStringLiteral("sounds/"), Qt::CaseInsensitive))
					candidates.push_back(QStringLiteral("sounds/%1").arg(relative));
				candidates.push_back(fileName);
				for (const QString &candidate : std::as_const(candidates))
				{
					QString normalized;
					QString error;
					if (!QMudPluginPathUtils::resolveInsideQmudHome(session.startupDirectory, candidate,
					                                                &normalized, &error))
						continue;
					QFileInfo info(normalized);
					if (!info.exists() || !info.isFile())
						continue;
					resolved = normalized;
					return info;
				}
				return std::nullopt;
			}

			/**
			 * @brief Resolves a sound and builds its device-specific backend cache identity.
			 * @param session Session providing the QMud-home root.
			 * @param fileName Legacy QMud-home-relative or absolute-safe sound path.
			 * @param resolved Destination receiving the contained absolute file path.
			 * @return Playback source key, or no value when resolution fails.
			 */
			std::optional<SoundSourceKey> resolveFile(const WorkerSession &session, const QString &fileName,
			                                          QString &resolved)
			{
				const auto info = resolveFileInfo(session, fileName, resolved);
				if (!info)
					return std::nullopt;
				ensureMediaDevices();
				SoundSourceKey key;
				key.identity      = resolved;
				key.size          = info->size();
				key.modifiedMsecs = info->lastModified().toMSecsSinceEpoch();
				key.backend =
				    soundFilePlaybackBackendForFile(resolved) == SoundFilePlaybackBackend::QMediaPlayer
				        ? SoundBackendKind::MediaPlayer
				        : SoundBackendKind::Effect;
				key.deviceId = m_deviceId;
				return key;
			}

			static int selectChannel(const WorkerSession &session, const int requestedBuffer)
			{
				if (requestedBuffer < 0 || requestedBuffer > SoundEngineSession::kChannelCount)
					return -1;
				if (requestedBuffer > 0)
					return requestedBuffer - 1;
				for (int i = 0; i < SoundEngineSession::kChannelCount / 2; ++i)
				{
					if (!session.channels[static_cast<std::size_t>(i)].allocated)
						return i;
				}
				for (int i = 0; i < SoundEngineSession::kChannelCount; ++i)
				{
					const SoundChannel &channel = session.channels[static_cast<std::size_t>(i)];
					if (!channel.allocated ||
					    (channel.playbackStarted && !channel.looping && !channel.playing))
						return i;
				}
				for (int i = 0; i < SoundEngineSession::kChannelCount; ++i)
				{
					const SoundChannel &channel = session.channels[static_cast<std::size_t>(i)];
					if (channel.backend && channel.playbackStarted)
						return i;
				}
				return -1;
			}

			int playFile(const SoundCommand &command, int &selectedBuffer)
			{
				auto sessionIt = m_sessions.find(command.sessionId);
				if (sessionIt == m_sessions.end())
					return eCannotPlaySound;
				WorkerSession &session = sessionIt.value();
				if (command.buffer >= 1 && command.buffer <= SoundEngineSession::kChannelCount &&
				    command.text.isEmpty())
				{
					const SoundChannel &channel =
					    session.channels[static_cast<std::size_t>(command.buffer - 1)];
					if (command.luaAudioGeneration != 0 &&
					    channel.luaAudioGeneration != command.luaAudioGeneration)
					{
						return eCannotPlaySound;
					}
					const int result =
					    updateChannel(session, command.buffer - 1, command.loop, command.volume, command.pan);
					if (result == eOK)
						selectedBuffer = command.buffer;
					return result;
				}
				if (command.text.isEmpty() || command.buffer < 0 ||
				    command.buffer > SoundEngineSession::kChannelCount)
					return eBadParameter;
				const int channelIndex = selectChannel(session, command.buffer);
				if (channelIndex < 0)
					return eCannotPlaySound;
				SoundChannel &channel             = session.channels[static_cast<std::size_t>(channelIndex)];
				const bool    consumesReservation = command.requireReservation && command.buffer > 0 &&
				                                    channel.allocated && !channel.backend &&
				                                    channel.luaAudioGeneration == command.luaAudioGeneration;
				const bool    reservationOwnershipConfirmed =
				    consumesReservation && channel.luaAudioOwnershipConfirmed;
				if (command.requireReservation && !consumesReservation)
					return eCannotPlaySound;

				QString    resolved;
				const auto key = resolveFile(session, command.text, resolved);
				if (!key)
				{
					if (consumesReservation)
					{
						releaseChannel(session, channelIndex);
						publishSnapshotIfChanged(session);
					}
					return eFileNotFound;
				}
				if (channel.backend && channel.backend->key == *key)
				{
					finishLuaAudioOwnership(session, channelIndex, channel.luaAudioGeneration,
					                        channel.luaAudioOwnershipConfirmed);
					SoundBackend *backend         = channel.backend;
					channel                       = {};
					backend->sessionId            = 0;
					backend->channel              = -1;
					backend->assignmentGeneration = 0;
					stopBackend(*backend);
					assignAndPlay(session, channelIndex, *backend, command, command.luaAudioGeneration);
					selectedBuffer = channelIndex + 1;
					return eOK;
				}

				SoundBackendLease lease = leaseBackend(*key);
				if (!lease.backend)
				{
					if (consumesReservation)
					{
						releaseChannel(session, channelIndex);
						publishSnapshotIfChanged(session);
					}
					reportPlaybackError(command.sessionId, lease.failureMessage);
					return eCannotPlaySound;
				}
				if (consumesReservation)
					channel = {};
				else
					releaseChannel(session, channelIndex);
				assignAndPlay(session, channelIndex, *lease.backend, command, command.luaAudioGeneration,
				              reservationOwnershipConfirmed);
				selectedBuffer = channelIndex + 1;
				return eOK;
			}

			/**
			 * @brief Reserves a channel and starts asynchronous hashing for memory playback.
			 * @param command Submitted memory-play command whose payload and completion are consumed on success.
			 * @param selectedBuffer Destination receiving the reserved one-based channel.
			 * @return Immediate validation or reservation status.
			 */
			int beginPlayMemory(SoundCommand &command, int &selectedBuffer)
			{
				auto sessionIt = m_sessions.find(command.sessionId);
				if (sessionIt == m_sessions.end())
					return eCannotPlaySound;
				if (command.bytes.isEmpty() || command.buffer < 0 ||
				    command.buffer > SoundEngineSession::kChannelCount)
					return eBadParameter;
				WorkerSession &session      = sessionIt.value();
				const int      channelIndex = selectChannel(session, command.buffer);
				if (channelIndex < 0)
					return eCannotPlaySound;
				ensureMemoryPreparationPool();

				releaseChannel(session, channelIndex);
				SoundChannel &channel            = session.channels[static_cast<std::size_t>(channelIndex)];
				const quint64 operationId        = m_nextMemoryOperationId++;
				channel.allocated                = true;
				channel.assignmentGeneration     = session.nextAssignmentGeneration++;
				channel.luaAudioGeneration       = command.luaAudioGeneration;
				channel.pendingMemoryOperationId = operationId;

				PendingMemoryPlay pending;
				pending.sessionId            = command.sessionId;
				pending.operationId          = operationId;
				pending.assignmentGeneration = channel.assignmentGeneration;
				pending.channelIndex         = channelIndex;
				pending.loop                 = command.loop;
				pending.volume               = command.volume;
				pending.pan                  = command.pan;
				pending.luaAudioGeneration   = command.luaAudioGeneration;
				pending.payload              = std::move(command.bytes);
				pending.completion           = std::move(command.completion);
				m_pendingMemoryPlays.insert(operationId, std::move(pending));
				publishSnapshotIfChanged(session);
				selectedBuffer = channelIndex + 1;
				startMemoryDigest(operationId);
				return eOK;
			}

			/** @brief Lazily creates the ordered memory-preparation pool on the sound worker. */
			void ensureMemoryPreparationPool()
			{
				if (m_memoryPreparationPool)
					return;
				m_memoryPreparationPool = std::make_unique<QThreadPool>();
				m_memoryPreparationPool->setMaxThreadCount(1);
				m_memoryPreparationPool->setObjectName(QStringLiteral("QMud sound memory preparation"));
			}

			/**
			 * @brief Hashes one pending memory payload without blocking the sound worker.
			 * @param operationId Pending memory-play identity.
			 */
			void startMemoryDigest(const quint64 operationId) const
			{
				const auto pendingIt = m_pendingMemoryPlays.constFind(operationId);
				if (pendingIt == m_pendingMemoryPlays.constEnd())
					return;
				const QByteArray payload = pendingIt->payload;
				const auto       ingress = m_ingress;
				m_memoryPreparationPool->start(QRunnable::create(
				    [ingress, operationId, payload]
				    {
					    SoundCommand completed;
					    completed.kind   = SoundCommandKind::MemoryDigestReady;
					    completed.token  = operationId;
					    completed.digest = QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
					    enqueueAsyncResult(ingress, std::move(completed));
				    }));
			}

			/**
			 * @brief Reuses or starts materializing the resource for a completed memory digest.
			 * @param command Internal digest-completion command.
			 */
			void handleMemoryDigestReady(const SoundCommand &command)
			{
				auto pendingIt = m_pendingMemoryPlays.find(command.token);
				if (pendingIt == m_pendingMemoryPlays.end())
					return;
				const QString resourceKey = memoryResourceKey(command.digest, pendingIt->payload.size());
				if (const auto resourceIt = m_memoryResources.find(resourceKey);
				    resourceIt != m_memoryResources.end())
				{
					if (const std::shared_ptr<SoundMemoryResource> resource = resourceIt.value().lock())
					{
						pendingIt->payload.clear();
						finishPendingMemoryPlay(command.token, command.digest, resource);
						return;
					}
					m_memoryResources.erase(resourceIt);
				}

				if (auto materializationIt = m_pendingMemoryMaterializations.find(resourceKey);
				    materializationIt != m_pendingMemoryMaterializations.end())
				{
					materializationIt->push_back(command.token);
					pendingIt->payload.clear();
					return;
				}

				m_pendingMemoryMaterializations.insert(resourceKey, {command.token});
				QByteArray payload = std::move(pendingIt->payload);
				const auto ingress = m_ingress;
				m_memoryPreparationPool->start(QRunnable::create(
				    [ingress, digest = command.digest, payload = std::move(payload), resourceKey]() mutable
				    {
					    QTemporaryFile file;
					    const bool     written =
					        file.open() && file.write(payload) == payload.size() && file.flush();
					    std::shared_ptr<SoundMemoryResource> resource;
					    if (written)
					    {
						    file.setAutoRemove(false);
						    file.close();
						    resource           = std::make_shared<SoundMemoryResource>();
						    resource->fileName = file.fileName();
						    resource->digest   = digest;
						    resource->size     = payload.size();
					    }
					    SoundCommand completed;
					    completed.kind           = SoundCommandKind::MemoryFileReady;
					    completed.text           = resourceKey;
					    completed.digest         = digest;
					    completed.memoryResource = std::move(resource);
					    enqueueAsyncResult(ingress, std::move(completed));
				    }));
			}

			/**
			 * @brief Completes all pending plays waiting for one materialized memory resource.
			 * @param command Internal file-materialization completion command.
			 */
			void handleMemoryFileReady(const SoundCommand &command)
			{
				const QVector<quint64> operations = m_pendingMemoryMaterializations.take(command.text);
				for (const quint64 operationId : operations)
					finishPendingMemoryPlay(operationId, command.digest, command.memoryResource);
			}

			/**
			 * @brief Validates and starts one pending memory play after resource preparation.
			 * @param operationId Pending memory-play identity.
			 * @param digest Content digest used in the backend cache key.
			 * @param resource Prepared temporary-file resource, or null after a preparation failure.
			 */
			void finishPendingMemoryPlay(const quint64 operationId, const QByteArray &digest,
			                             const std::shared_ptr<SoundMemoryResource> &resource)
			{
				auto pendingIt = m_pendingMemoryPlays.find(operationId);
				if (pendingIt == m_pendingMemoryPlays.end())
					return;
				PendingMemoryPlay pending = std::move(pendingIt.value());
				m_pendingMemoryPlays.erase(pendingIt);
				auto sessionIt = m_sessions.find(pending.sessionId);
				if (sessionIt == m_sessions.end())
				{
					if (pending.completion)
						pending.completion->complete(eCannotPlaySound);
					return;
				}
				WorkerSession &session = sessionIt.value();
				SoundChannel  &channel = session.channels[static_cast<std::size_t>(pending.channelIndex)];
				if (channel.pendingMemoryOperationId != operationId ||
				    channel.assignmentGeneration != pending.assignmentGeneration)
				{
					if (pending.completion)
						pending.completion->complete(eCannotPlaySound);
					return;
				}
				if (!resource)
				{
					channel = {};
					publishSnapshotIfChanged(session);
					finishLuaAudioOwnership(session, pending.channelIndex, pending.luaAudioGeneration, false);
					if (pending.completion)
						pending.completion->complete(eCannotPlaySound);
					return;
				}

				ensureMediaDevices();
				SoundSourceKey key;
				key.identity            = memoryResourceKey(digest, resource->size);
				key.size                = resource->size;
				key.backend             = SoundBackendKind::Effect;
				key.deviceId            = m_deviceId;
				SoundBackendLease lease = leaseMemoryBackend(key, resource);
				if (!lease.backend)
				{
					channel = {};
					publishSnapshotIfChanged(session);
					finishLuaAudioOwnership(session, pending.channelIndex, pending.luaAudioGeneration, false);
					reportPlaybackError(pending.sessionId, lease.failureMessage);
					if (pending.completion)
						pending.completion->complete(eCannotPlaySound);
					return;
				}

				SoundCommand play;
				play.sessionId = pending.sessionId;
				play.loop      = pending.loop;
				play.volume    = pending.volume;
				play.pan       = pending.pan;
				channel        = {};
				assignAndPlay(session, pending.channelIndex, *lease.backend, play,
				              pending.luaAudioGeneration);
				if (pending.completion)
					pending.completion->complete(eOK, pending.channelIndex + 1);
			}

			int reserveLuaAudioChannel(const SoundCommand &command, int &selectedBuffer)
			{
				auto sessionIt = m_sessions.find(command.sessionId);
				if (sessionIt == m_sessions.end() || command.text.isEmpty() ||
				    command.luaAudioGeneration == 0)
					return eCannotPlaySound;
				WorkerSession &session = sessionIt.value();
				QString        resolved;
				if (!resolveFileInfo(session, command.text, resolved))
					return eFileNotFound;
				for (int channelIndex = 0; channelIndex < SoundEngineSession::kChannelCount; ++channelIndex)
				{
					const SoundChannel &candidate = session.channels[static_cast<std::size_t>(channelIndex)];
					const bool reusable = !candidate.allocated || (candidate.playbackStarted &&
					                                               !candidate.looping && !candidate.playing);
					if (!reusable)
						continue;
					releaseChannel(session, channelIndex);
					SoundChannel &reserved      = session.channels[static_cast<std::size_t>(channelIndex)];
					reserved.allocated          = true;
					reserved.luaAudioGeneration = command.luaAudioGeneration;
					publishSnapshotIfChanged(session);
					selectedBuffer = channelIndex + 1;
					return eOK;
				}
				return eCannotPlaySound;
			}

			int confirmLuaAudioOwnership(const SoundCommand &command)
			{
				auto sessionIt = m_sessions.find(command.sessionId);
				if (sessionIt == m_sessions.end() || command.buffer < 1 ||
				    command.buffer > SoundEngineSession::kChannelCount || command.luaAudioGeneration == 0)
					return eCannotPlaySound;
				WorkerSession &session = sessionIt.value();
				SoundChannel  &channel = session.channels[static_cast<std::size_t>(command.buffer - 1)];
				if (channel.luaAudioGeneration == command.luaAudioGeneration)
				{
					channel.luaAudioOwnershipConfirmed = true;
					return eOK;
				}
				const QPair<int, quint64> completed(command.buffer, command.luaAudioGeneration);
				if (!session.pendingLuaAudioCompletions.removeOne(completed))
					return eCannotPlaySound;
				notifyLuaAudioOwnershipReleased(session, command.buffer, command.luaAudioGeneration);
				return eOK;
			}

			static int updateChannel(WorkerSession &session, const int channelIndex, const bool loop,
			                         const double volume, const double pan)
			{
				SoundChannel &channel = session.channels[static_cast<std::size_t>(channelIndex)];
				if (!channel.backend)
					return channel.allocated && channel.playbackStarted ? eCannotPlaySound : eBadParameter;
				if (!channel.playing)
					return eCannotPlaySound;
				channel.looping = loop;
				channel.volume  = normalizeSoundVolume(volume);
				channel.pan     = pan;
				applyBackendParameters(*channel.backend, channel);
				publishSnapshotIfChanged(session);
				return eOK;
			}

			void prepareFile(const SoundCommand &command)
			{
				auto sessionIt = m_sessions.find(command.sessionId);
				if (sessionIt == m_sessions.end())
					return;
				WorkerSession &session = sessionIt.value();
				QString        resolved;
				const auto     key = resolveFile(session, command.text, resolved);
				if (!key)
				{
					session.prepared.insert(command.token, {nullptr, eFileNotFound, {}});
					return;
				}
				SoundBackendLease lease = leaseBackend(*key);
				if (!lease.backend)
				{
					session.prepared.insert(command.token,
					                        {nullptr, eCannotPlaySound, std::move(lease.failureMessage)});
					return;
				}
				lease.backend->sessionId        = command.sessionId;
				lease.backend->preparationToken = command.token;
				session.prepared.insert(command.token, {lease.backend, eOK, {}});
			}

			int commitPrepared(const SoundCommand &command)
			{
				auto sessionIt = m_sessions.find(command.sessionId);
				if (sessionIt == m_sessions.end())
					return eCannotPlaySound;
				WorkerSession &session    = sessionIt.value();
				auto           preparedIt = session.prepared.find(command.token);
				if (preparedIt == session.prepared.end())
					return eCannotPlaySound;
				const PreparedSound prepared = std::move(preparedIt.value());
				session.prepared.erase(preparedIt);
				if (prepared.result != eOK || !prepared.backend)
				{
					reportPlaybackError(command.sessionId, prepared.failureMessage);
					return prepared.result;
				}
				const int channelIndex = selectChannel(session, command.buffer);
				if (channelIndex < 0)
				{
					releaseBackend(*prepared.backend);
					return command.buffer < 0 || command.buffer > SoundEngineSession::kChannelCount
					           ? eBadParameter
					           : eCannotPlaySound;
				}
				prepared.backend->preparationToken = 0;
				releaseChannel(session, channelIndex);
				assignAndPlay(session, channelIndex, *prepared.backend, command, 0);
				return eOK;
			}

			void cancelPrepared(const quint64 sessionId, const quint64 token)
			{
				auto sessionIt = m_sessions.find(sessionId);
				if (sessionIt == m_sessions.end())
					return;
				auto preparedIt = sessionIt->prepared.find(token);
				if (preparedIt == sessionIt->prepared.end())
					return;
				SoundBackend *backend = preparedIt->backend;
				sessionIt->prepared.erase(preparedIt);
				if (backend)
				{
					backend->preparationToken = 0;
					releaseBackend(*backend);
				}
			}

			/**
			 * @brief Stops one channel or every channel, optionally guarded by LuaAudio ownership.
			 * @param sessionId Worker session identity.
			 * @param buffer Fixed one-based channel, or zero for every channel.
			 * @param luaAudioGeneration Expected LuaAudio generation, or zero for an unguarded stop.
			 * @return MUSHclient-compatible status code.
			 */
			int stop(const quint64 sessionId, const int buffer, const quint64 luaAudioGeneration)
			{
				auto sessionIt = m_sessions.find(sessionId);
				if (sessionIt == m_sessions.end())
					return eCannotPlaySound;
				if (buffer < 0 || buffer > SoundEngineSession::kChannelCount)
					return eBadParameter;
				if (buffer == 0)
				{
					if (luaAudioGeneration != 0)
						return eBadParameter;
					for (int i = 0; i < SoundEngineSession::kChannelCount; ++i)
						releaseChannel(sessionIt.value(), i);
					publishSnapshotIfChanged(sessionIt.value());
					return eOK;
				}
				const SoundChannel &channel = sessionIt->channels[static_cast<std::size_t>(buffer - 1)];
				if (luaAudioGeneration != 0 && channel.luaAudioGeneration != luaAudioGeneration)
					return eCannotPlaySound;
				releaseChannel(sessionIt.value(), buffer - 1);
				publishSnapshotIfChanged(sessionIt.value());
				return eOK;
			}

			bool closeSession(const quint64 sessionId, const bool erase)
			{
				auto sessionIt = m_sessions.find(sessionId);
				if (sessionIt == m_sessions.end())
					return false;
				WorkerSession &session = sessionIt.value();
				for (PreparedSound &prepared : session.prepared)
				{
					if (prepared.backend)
					{
						prepared.backend->preparationToken = 0;
						releaseBackend(*prepared.backend);
					}
				}
				session.prepared.clear();
				for (int i = 0; i < SoundEngineSession::kChannelCount; ++i)
					releaseChannel(session, i);
				session.pendingLuaAudioCompletions.clear();
				if (erase)
					m_sessions.erase(sessionIt);
				return true;
			}

			/**
			 * @brief Completes backend initialization and extracts a synchronous failure before removal.
			 * @param backend Cache-owned backend whose initialization attempt has returned.
			 * @return Usable backend lease or its retained initialization diagnostic.
			 */
			SoundBackendLease finishBackendInitialization(SoundBackend &backend)
			{
				backend.initializationInProgress = false;
				if (!backend.failed)
					return {&backend, {}};
				QString              failureMessage = std::move(backend.failureMessage);
				const SoundSourceKey key            = backend.key;
				const quint64        backendId      = backend.backendId;
				removeBackend(key, backendId);
				return {nullptr, std::move(failureMessage)};
			}

			/**
			 * @brief Leases a file-backed multimedia object and preserves synchronous initialization errors.
			 * @param key Resolved source identity and backend kind.
			 * @return Backend lease and any initialization diagnostic.
			 */
			SoundBackendLease leaseBackend(const SoundSourceKey &key)
			{
				const qint64 now = currentTimeMsecs();
				if (SoundBackend *candidate = m_cache.takeIdle(key, now))
					return {candidate, {}};

				auto backend                      = takePreinitializedBackend(key.backend);
				backend->backendId                = m_nextBackendId++;
				backend->key                      = key;
				backend->initializationInProgress = true;
				const quint64 backendId           = backend->backendId;
				const auto inserted = m_cache.insertActive(key, key.size, backendId, std::move(backend), now);
				SoundBackend *result = inserted.backend;
				if (!result)
					return {};
				if (key.backend == SoundBackendKind::MediaPlayer)
				{
					Q_ASSERT(result->player);
					Q_ASSERT(result->audioOutput);
					connect(result->player.get(), &QMediaPlayer::errorOccurred, this,
					        [this, raw = result](const QMediaPlayer::Error error, const QString &text)
					        { handlePlayerError(*raw, error, text); });
					connect(result->player.get(), &QMediaPlayer::playbackStateChanged, this,
					        [this, raw = result](const QMediaPlayer::PlaybackState state)
					        { handlePlayerState(*raw, state); });
					result->player->setSource(QUrl::fromLocalFile(key.identity));
				}
				else
				{
					Q_ASSERT(result->effect);
					connect(result->effect.get(), &QSoundEffect::statusChanged, this,
					        [this, raw = result] { handleEffectStatus(*raw); });
					connect(result->effect.get(), &QSoundEffect::playingChanged, this,
					        [this, raw = result] { handleEffectPlaying(*raw); });
					result->effect->setSource(QUrl::fromLocalFile(key.identity));
				}
				return finishBackendInitialization(*result);
			}

			/**
			 * @brief Leases a memory-backed sound effect and preserves synchronous initialization errors.
			 * @param key Content-addressed source identity.
			 * @param resource Materialized sound bytes retained by the backend.
			 * @return Backend lease and any initialization diagnostic.
			 */
			SoundBackendLease leaseMemoryBackend(const SoundSourceKey                       &key,
			                                     const std::shared_ptr<SoundMemoryResource> &resource)
			{
				const qint64 now = currentTimeMsecs();
				if (SoundBackend *candidate = m_cache.takeIdle(key, now))
				{
					if (candidate->memoryResource)
						m_memoryResources.insert(key.identity, candidate->memoryResource);
					return {candidate, {}};
				}

				if (!resource)
					return {};

				auto backend                      = takePreinitializedBackend(SoundBackendKind::Effect);
				backend->backendId                = m_nextBackendId++;
				backend->key                      = key;
				backend->memoryResource           = resource;
				backend->initializationInProgress = true;
				const quint64 backendId           = backend->backendId;
				const auto inserted = m_cache.insertActive(key, key.size, backendId, std::move(backend), now);
				SoundBackend *result = inserted.backend;
				if (!result)
					return {};
				Q_ASSERT(result->effect);
				connect(result->effect.get(), &QSoundEffect::statusChanged, this,
				        [this, raw = result] { handleEffectStatus(*raw); });
				connect(result->effect.get(), &QSoundEffect::playingChanged, this,
				        [this, raw = result] { handleEffectPlaying(*raw); });
				result->effect->setSource(QUrl::fromLocalFile(resource->fileName));
				SoundBackendLease lease = finishBackendInitialization(*result);
				if (!lease.backend)
					return lease;
				if (result->memoryResource)
					m_memoryResources.insert(key.identity, result->memoryResource);
				return lease;
			}

			/**
			 * @brief Builds the collision-resistant cache identity for memory sound bytes.
			 * @param digest SHA-256 content digest.
			 * @param size Original payload size.
			 * @return Stable memory-resource cache identity.
			 */
			static QString memoryResourceKey(const QByteArray &digest, const qsizetype size)
			{
				return QStringLiteral("memory:%1:%2").arg(QString::fromLatin1(digest.toHex())).arg(size);
			}

			/**
			 * @brief Appends a preparation result to the worker ingress when the engine still accepts work.
			 * @param ingress Shared sound command ingress.
			 * @param command Internal result command to enqueue.
			 */
			static void enqueueAsyncResult(const std::shared_ptr<SoundCommandIngress> &ingress,
			                               SoundCommand                                command)
			{
				QMutexLocker locker(&ingress->mutex);
				if (!ingress->accepting || !ingress->worker)
					return;
				SoundWorker *target = ingress->worker;
				ingress->commands.push_back(std::move(command));
				if (!ingress->drainPosted)
				{
					ingress->drainPosted = true;
					QMetaObject::invokeMethod(target, [target] { target->drain(); }, Qt::QueuedConnection);
				}
			}

			void assignAndPlay(WorkerSession &session, const int channelIndex, SoundBackend &backend,
			                   const SoundCommand &command, const quint64 luaAudioGeneration,
			                   const bool luaAudioOwnershipConfirmed = false)
			{
				SoundChannel &channel              = session.channels[static_cast<std::size_t>(channelIndex)];
				channel.backend                    = &backend;
				channel.assignmentGeneration       = session.nextAssignmentGeneration++;
				channel.luaAudioGeneration         = luaAudioGeneration;
				channel.luaAudioOwnershipConfirmed = luaAudioOwnershipConfirmed;
				channel.allocated                  = true;
				channel.playing                    = true;
				channel.playbackStarted            = false;
				channel.looping                    = command.loop;
				channel.volume                     = normalizeSoundVolume(command.volume);
				channel.pan                        = command.pan;
				backend.sessionId                  = command.sessionId;
				backend.channel                    = channelIndex;
				backend.assignmentGeneration       = channel.assignmentGeneration;
				backend.preparationToken           = 0;
				backend.started                    = false;
				m_cache.noteUse(backend.key, currentTimeMsecs());
				applyBackendParameters(backend, channel);
				if (backend.effect)
					backend.effect->play();
				else if (backend.player)
					backend.player->play();
				publishSnapshotIfChanged(session);
			}

			static void applyBackendParameters(const SoundBackend &backend, const SoundChannel &channel)
			{
				if (backend.effect)
				{
					backend.effect->setLoopCount(channel.looping ? QSoundEffect::Infinite : 1);
					backend.effect->setVolume(static_cast<float>(channel.volume));
				}
				if (backend.player)
					backend.player->setLoops(channel.looping ? QMediaPlayer::Infinite : QMediaPlayer::Once);
				if (backend.audioOutput)
					backend.audioOutput->setVolume(static_cast<float>(channel.volume));
			}

			static void stopBackend(SoundBackend &backend)
			{
				if (backend.effect)
					backend.effect->stop();
				if (backend.player)
					backend.player->stop();
				backend.started = false;
			}

			void releaseChannel(WorkerSession &session, const int channelIndex,
			                    const bool reportLuaAudioCompletion = true)
			{
				SoundChannel &channel = session.channels[static_cast<std::size_t>(channelIndex)];
				if (channel.pendingMemoryOperationId != 0)
					cancelPendingMemoryPlay(channel.pendingMemoryOperationId);
				if (reportLuaAudioCompletion)
					finishLuaAudioOwnership(session, channelIndex, channel.luaAudioGeneration,
					                        channel.luaAudioOwnershipConfirmed);
				SoundBackend *backend = channel.backend;
				channel               = {};
				if (backend)
				{
					backend->sessionId            = 0;
					backend->channel              = -1;
					backend->assignmentGeneration = 0;
					stopBackend(*backend);
					releaseBackend(*backend);
				}
			}

			/**
			 * @brief Cancels one pending memory preparation and releases its waiting caller.
			 * @param operationId Pending memory-play identity.
			 */
			void cancelPendingMemoryPlay(const quint64 operationId)
			{
				auto pendingIt = m_pendingMemoryPlays.find(operationId);
				if (pendingIt == m_pendingMemoryPlays.end())
					return;
				const std::shared_ptr<SoundCommandCompletion> completion = std::move(pendingIt->completion);
				m_pendingMemoryPlays.erase(pendingIt);
				if (completion)
					completion->complete(eCannotPlaySound);
			}

			/**
			 * @brief Returns one unassigned backend to the idle cache for the next periodic maintenance pass.
			 * @param backend Backend becoming idle.
			 * @param lastUsedMsecs Preserved last-use time, or zero to use the current worker time.
			 */
			void releaseBackend(SoundBackend &backend, const qint64 lastUsedMsecs = 0)
			{
				const qint64 releasedAt      = lastUsedMsecs > 0 ? lastUsedMsecs : currentTimeMsecs();
				backend.sessionId            = 0;
				backend.channel              = -1;
				backend.assignmentGeneration = 0;
				backend.preparationToken     = 0;
				backend.started              = false;
				m_cache.markIdle(backend.key, backend.backendId, releasedAt);
			}

			void handleEffectStatus(SoundBackend &backend)
			{
				if (!backend.effect || backend.effect->status() != QSoundEffect::Error)
					return;
				handleBackendError(
				    backend, QStringLiteral("QMud sound playback failed for %1").arg(backend.key.identity));
			}

			/**
			 * @brief Applies the common natural-completion transition for one active channel.
			 * @param session Worker session owning the channel.
			 * @param channelIndex Zero-based channel that completed playback.
			 */
			void completeChannelPlayback(WorkerSession &session, const int channelIndex)
			{
				SoundChannel &channel = session.channels[static_cast<std::size_t>(channelIndex)];
				Q_ASSERT(channel.backend);
				channel.playing          = false;
				channel.completedAtMsecs = currentTimeMsecs();
				m_cache.noteUse(channel.backend->key, channel.completedAtMsecs);
				const quint64 luaAudioGeneration = std::exchange(channel.luaAudioGeneration, 0);
				const bool    ownershipConfirmed = std::exchange(channel.luaAudioOwnershipConfirmed, false);
				publishSnapshotIfChanged(session);
				finishLuaAudioOwnership(session, channelIndex, luaAudioGeneration, ownershipConfirmed);
			}

			void handleEffectPlaying(SoundBackend &backend)
			{
				if (!backend.effect || backend.channel < 0 || backend.sessionId == 0)
					return;
				auto sessionIt = m_sessions.find(backend.sessionId);
				if (sessionIt == m_sessions.end())
					return;
				SoundChannel &channel = sessionIt->channels[static_cast<std::size_t>(backend.channel)];
				if (channel.backend != &backend ||
				    channel.assignmentGeneration != backend.assignmentGeneration)
					return;
				if (backend.effect->isPlaying())
				{
					backend.started         = true;
					channel.playbackStarted = true;
					channel.playing         = true;
					publishSnapshotIfChanged(sessionIt.value());
					return;
				}
				if (!channel.playbackStarted || channel.looping || !channel.playing)
					return;
				completeChannelPlayback(sessionIt.value(), backend.channel);
			}

			void handlePlayerState(SoundBackend &backend, const QMediaPlayer::PlaybackState state)
			{
				if (backend.channel < 0 || backend.sessionId == 0)
					return;
				auto sessionIt = m_sessions.find(backend.sessionId);
				if (sessionIt == m_sessions.end())
					return;
				SoundChannel &channel = sessionIt->channels[static_cast<std::size_t>(backend.channel)];
				if (channel.backend != &backend ||
				    channel.assignmentGeneration != backend.assignmentGeneration)
					return;
				if (state == QMediaPlayer::PlayingState)
				{
					backend.started         = true;
					channel.playbackStarted = true;
					channel.playing         = true;
					publishSnapshotIfChanged(sessionIt.value());
					return;
				}
				if (state != QMediaPlayer::StoppedState || !channel.playbackStarted || channel.looping ||
				    !channel.playing)
					return;
				completeChannelPlayback(sessionIt.value(), backend.channel);
			}

			void handlePlayerError(SoundBackend &backend, const QMediaPlayer::Error error,
			                       const QString &errorText)
			{
				if (error == QMediaPlayer::NoError)
					return;
				const QString detail = errorText.trimmed().isEmpty()
				                           ? QStringLiteral("unknown media-player error")
				                           : errorText.trimmed();
				handleBackendError(backend, QStringLiteral("QMud sound playback failed: %1 (file: %2)")
				                                .arg(detail, backend.key.identity));
			}

			void handleBackendError(SoundBackend &backend, const QString &message)
			{
				if (backend.initializationInProgress)
				{
					backend.failed = true;
					if (backend.failureMessage.isEmpty())
						backend.failureMessage = message;
					return;
				}
				if (backend.retiring)
					return;
				if (backend.preparationToken != 0)
				{
					auto sessionIt = m_sessions.find(backend.sessionId);
					if (sessionIt != m_sessions.end())
					{
						auto preparedIt = sessionIt->prepared.find(backend.preparationToken);
						if (preparedIt != sessionIt->prepared.end() && preparedIt->backend == &backend)
						{
							preparedIt->backend        = nullptr;
							preparedIt->result         = eCannotPlaySound;
							preparedIt->failureMessage = message;
						}
					}
					retireBackend(backend);
					return;
				}
				if (backend.channel < 0 || backend.sessionId == 0)
				{
					retireBackend(backend);
					return;
				}
				const quint64 sessionId = backend.sessionId;
				auto          sessionIt = m_sessions.find(sessionId);
				if (sessionIt == m_sessions.end())
				{
					retireBackend(backend);
					return;
				}
				const int     channelIndex = backend.channel;
				SoundChannel &channel      = sessionIt->channels[static_cast<std::size_t>(channelIndex)];
				if (channel.backend != &backend ||
				    channel.assignmentGeneration != backend.assignmentGeneration)
					return;
				const quint64 luaAudioGeneration = channel.luaAudioGeneration;
				const bool    ownershipConfirmed = channel.luaAudioOwnershipConfirmed;
				channel                          = {};
				publishSnapshotIfChanged(sessionIt.value());
				finishLuaAudioOwnership(sessionIt.value(), channelIndex, luaAudioGeneration,
				                        ownershipConfirmed);
				reportPlaybackError(sessionId, message);
				retireBackend(backend);
			}

			/**
			 * @brief Reports a nonempty playback diagnostic through the established owner-thread route.
			 * @param sessionId Session associated with the failed occurrence.
			 * @param message Diagnostic retained by the failed backend or prepared occurrence.
			 */
			void reportPlaybackError(const quint64 sessionId, const QString &message) const
			{
				if (!message.isEmpty() && m_playbackError)
					m_playbackError(sessionId, message);
			}

			/**
			 * @brief Synchronously releases one confirmed LuaAudio ownership generation.
			 * @param session Worker session owning the callback and channel.
			 * @param channel One-based channel whose ownership ended.
			 * @param generation Ownership generation that ended.
			 */
			static void notifyLuaAudioOwnershipReleased(const WorkerSession &session, const int channel,
			                                            const quint64 generation)
			{
				if (generation != 0 && session.luaAudioOwnershipReleased)
					session.luaAudioOwnershipReleased(channel, generation);
			}

			/**
			 * @brief Completes or defers release of one LuaAudio ownership generation.
			 * @param session Worker session owning the channel and release callback.
			 * @param channelIndex Zero-based channel whose ownership ended.
			 * @param generation Ownership generation that ended.
			 * @param ownershipConfirmed Whether the registry has confirmed ownership.
			 */
			static void finishLuaAudioOwnership(WorkerSession &session, const int channelIndex,
			                                    const quint64 generation, const bool ownershipConfirmed)
			{
				if (generation == 0)
					return;
				if (ownershipConfirmed)
				{
					notifyLuaAudioOwnershipReleased(session, channelIndex + 1, generation);
					return;
				}
				session.pendingLuaAudioCompletions.push_back({channelIndex + 1, generation});
			}

			/**
			 * @brief Publishes one immutable channel snapshot when callback-visible state changed.
			 * @param session Worker session whose channels are projected.
			 */
			static void publishSnapshotIfChanged(const WorkerSession &session)
			{
				if (!session.publishedState)
					return;
				const auto previous = session.publishedState->snapshot.load(std::memory_order_acquire);
				SoundEngineSession::Snapshot candidate;
				for (int i = 0; i < SoundEngineSession::kChannelCount; ++i)
				{
					const SoundChannel &channel = session.channels[static_cast<std::size_t>(i)];
					if (!channel.allocated)
						candidate.status[static_cast<std::size_t>(i)] = -2;
					else if (!channel.playing)
						candidate.status[static_cast<std::size_t>(i)] = 0;
					else
						candidate.status[static_cast<std::size_t>(i)] = channel.looping ? 2 : 1;
				}
				if (previous && previous->status == candidate.status)
					return;
				candidate.revision = previous ? previous->revision + 1 : 1;
				auto snapshot      = std::make_shared<SoundEngineSession::Snapshot>(candidate);
				session.publishedState->snapshot.store(
				    std::shared_ptr<const SoundEngineSession::Snapshot>(std::move(snapshot)),
				    std::memory_order_release);
			}

			void resetForAudioDeviceChange()
			{
				for (auto sessionIt = m_sessions.begin(); sessionIt != m_sessions.end(); ++sessionIt)
				{
					WorkerSession &session = sessionIt.value();
					for (PreparedSound &prepared : session.prepared)
					{
						prepared.backend = nullptr;
						prepared.result  = eCannotPlaySound;
					}
					for (int i = 0; i < SoundEngineSession::kChannelCount; ++i)
						releaseChannel(session, i);
					publishSnapshotIfChanged(session);
				}
				clearBackends();
				m_memoryResources.clear();
				m_pendingMemoryMaterializations.clear();
				m_preinitializedEffectBackend.reset();
				m_preinitializedMediaBackend.reset();
				initializePlaybackBackends();
			}

			/**
			 * @brief Constructs one source-free backend against the current default output device.
			 * @param kind Playback implementation to initialize.
			 * @return Initialized backend without cache or channel ownership.
			 */
			[[nodiscard]] static std::unique_ptr<SoundBackend>
			createPlaybackBackend(const SoundBackendKind kind)
			{
				auto backend = std::make_unique<SoundBackend>();
				if (kind == SoundBackendKind::MediaPlayer)
				{
					backend->player      = std::make_unique<QMediaPlayer>();
					backend->audioOutput = new QAudioOutput(backend->player.get());
					backend->audioOutput->setDevice(QMediaDevices::defaultAudioOutput());
					backend->player->setAudioOutput(backend->audioOutput);
				}
				else
				{
					backend->effect = std::make_unique<QSoundEffect>();
					backend->effect->setAudioDevice(QMediaDevices::defaultAudioOutput());
				}
				return backend;
			}

			/** @brief Initializes both playback implementations before any session can request a sound. */
			void initializePlaybackBackends()
			{
				ensureMediaDevices();
				if (!m_preinitializedEffectBackend)
					m_preinitializedEffectBackend = createPlaybackBackend(SoundBackendKind::Effect);
				if (!m_preinitializedMediaBackend)
					m_preinitializedMediaBackend = createPlaybackBackend(SoundBackendKind::MediaPlayer);
			}

			/**
			 * @brief Takes the startup-initialized backend for a playback implementation or creates another one.
			 * @param kind Playback implementation required by the resolved source.
			 * @return Source-free backend ready to receive cache identity and source assignment.
			 */
			[[nodiscard]] std::unique_ptr<SoundBackend> takePreinitializedBackend(const SoundBackendKind kind)
			{
				std::unique_ptr<SoundBackend> &prepared = kind == SoundBackendKind::MediaPlayer
				                                              ? m_preinitializedMediaBackend
				                                              : m_preinitializedEffectBackend;
				if (prepared)
					return std::exchange(prepared, nullptr);
				return createPlaybackBackend(kind);
			}

			void ensureMediaDevices()
			{
				if (m_mediaDevices)
					return;
				m_mediaDevices = std::make_unique<QMediaDevices>();
				m_deviceId     = QMediaDevices::defaultAudioOutput().id();
				connect(m_mediaDevices.get(), &QMediaDevices::audioOutputsChanged, this,
				        [this]
				        {
					        const QByteArray currentDevice = QMediaDevices::defaultAudioOutput().id();
					        if (currentDevice == m_deviceId)
						        return;
					        m_deviceId = currentDevice;
					        resetForAudioDeviceChange();
				        });
			}

			using SoundCache = QMudSoundCache<SoundSourceKey, SoundBackend>;

			/**
			 * @brief Returns a positive elapsed-clock value suitable for idle timestamps.
			 * @return Current worker-relative time in milliseconds.
			 */
			[[nodiscard]] qint64 currentTimeMsecs() const
			{
				return qMax<qint64>(1, m_clock.elapsed());
			}

			/**
			 * @brief Returns the custom event type used for low-priority maintenance.
			 * @return Process-unique maintenance event type.
			 */
			[[nodiscard]] static QEvent::Type maintenanceEventType()
			{
				static const auto type = static_cast<QEvent::Type>(QEvent::registerEventType());
				return type;
			}

			/**
			 * @brief Queues one coalesced cache-maintenance step at the lowest event priority.
			 * @param releaseCompletedChannels Whether this maintenance cycle must inspect completed channels.
			 */
			void scheduleMaintenance(const bool releaseCompletedChannels)
			{
				m_releaseCompletedPending = m_releaseCompletedPending || releaseCompletedChannels;
				if (m_maintenancePending || m_shuttingDown)
					return;
				m_maintenancePending = true;
				QCoreApplication::postEvent(this, new QEvent(maintenanceEventType()), Qt::LowEventPriority);
			}

			/** @brief Detaches naturally completed backends after the fixed channel reuse grace period. */
			void releaseCompletedChannels()
			{
				const qint64     now = currentTimeMsecs();
				constexpr qint64 graceMsecs =
				    static_cast<qint64>(SoundEngine::CachePolicy::kCompletedChannelGraceSeconds) * 1000;
				for (auto sessionIt = m_sessions.begin(); sessionIt != m_sessions.end(); ++sessionIt)
				{
					WorkerSession &session = sessionIt.value();
					for (SoundChannel &channel : session.channels)
					{
						if (!channel.backend || channel.playing || channel.looping ||
						    !SoundCache::elapsedAtLeast(now, channel.completedAtMsecs, graceMsecs))
							continue;
						SoundBackend *backend         = channel.backend;
						channel.backend               = nullptr;
						backend->sessionId            = 0;
						backend->channel              = -1;
						backend->assignmentGeneration = 0;
						releaseBackend(*backend, channel.completedAtMsecs);
					}
				}
			}

			/**
			 * @brief Returns whether source accounting exceeds the configured byte limit.
			 * @return `true` when idle sources should be evicted for size.
			 */
			[[nodiscard]] bool cacheExceedsSizeLimit() const
			{
				const qint64 maximumBytes = static_cast<qint64>(m_cachePolicy.ramCacheSizeMiB) * 1024 * 1024;
				return m_cache.totalBytes() > maximumBytes;
			}

			/**
			 * @brief Returns whether the oldest fully idle source must be evicted for age or size.
			 * @param nowMsecs Current worker-relative timestamp.
			 * @return `true` when another incremental eviction step is required.
			 */
			[[nodiscard]] bool cacheNeedsEviction(const qint64 nowMsecs) const
			{
				if (!m_cache.hasIdleSource())
					return false;
				if (cacheExceedsSizeLimit())
					return true;
				const qint64 lifetimeMsecs = static_cast<qint64>(m_cachePolicy.idleEvictionSeconds) * 1000;
				return SoundCache::elapsedAtLeast(nowMsecs, m_cache.oldestIdleLastUsedMsecs(), lifetimeMsecs);
			}

			/** @brief Performs completed-channel work and at most one deferred source eviction. */
			void performMaintenance()
			{
				if (std::exchange(m_releaseCompletedPending, false))
					releaseCompletedChannels();
				const qint64 now = currentTimeMsecs();
				if (!cacheNeedsEviction(now))
					return;
				const auto removedKey =
				    m_cache.evictOldestIdle([](SoundBackend &backend) { backend.retiring = true; });
				if (removedKey)
					pruneMemoryResourceIndex(*removedKey);
			}

			/** @brief Retires and destroys every backend while resetting source-byte accounting. */
			void clearBackends()
			{
				m_cache.clear([](SoundBackend &backend) { backend.retiring = true; });
			}

			/**
			 * @brief Removes an exact backend without examining unrelated sources.
			 * @param key Source identity owning the backend.
			 * @param backendId Process-unique backend identity.
			 */
			void removeBackend(const SoundSourceKey &key, const quint64 backendId)
			{
				static_cast<void>(m_cache.eraseBackend(key, backendId, [](SoundBackend &backend)
				                                       { backend.retiring = true; }));
				pruneMemoryResourceIndex(key);
			}

			/**
			 * @brief Makes a failed backend unavailable immediately and queues exact destruction.
			 * @param backend Failed backend owned by the source cache.
			 */
			void retireBackend(SoundBackend &backend)
			{
				if (backend.retiring)
					return;
				backend.retiring               = true;
				backend.sessionId              = 0;
				backend.channel                = -1;
				backend.assignmentGeneration   = 0;
				backend.preparationToken       = 0;
				const SoundSourceKey key       = backend.key;
				const quint64        backendId = backend.backendId;
				m_cache.markUnavailable(key, backendId);
				QTimer::singleShot(0, this, [this, key, backendId] { removeBackend(key, backendId); });
			}

			/**
			 * @brief Prunes the weak memory-resource index for one removed source when it expired.
			 * @param key Removed source identity.
			 */
			void pruneMemoryResourceIndex(const SoundSourceKey &key)
			{
				const auto it = m_memoryResources.find(key.identity);
				if (it != m_memoryResources.end() && it.value().expired())
					m_memoryResources.erase(it);
			}

			std::shared_ptr<SoundCommandIngress>               m_ingress;
			PlaybackError                                      m_playbackError;
			PlaybackInitializationActive                       m_playbackInitializationActive;
			PlaybackInitialized                                m_playbackInitialized;
			std::function<void()>                              m_playbackInitializationBarrierForTest;
			QHash<quint64, WorkerSession>                      m_sessions;
			SoundCache                                         m_cache;
			QHash<QString, std::weak_ptr<SoundMemoryResource>> m_memoryResources;
			QHash<quint64, PendingMemoryPlay>                  m_pendingMemoryPlays;
			QHash<QString, QVector<quint64>>                   m_pendingMemoryMaterializations;
			std::unique_ptr<QMediaDevices>                     m_mediaDevices;
			std::unique_ptr<SoundBackend>                      m_preinitializedEffectBackend;
			std::unique_ptr<SoundBackend>                      m_preinitializedMediaBackend;
			std::unique_ptr<QTimer>                            m_maintenanceTimer;
			std::unique_ptr<QThreadPool>                       m_memoryPreparationPool;
			QElapsedTimer                                      m_clock;
			QByteArray                                         m_deviceId;
			SoundEngine::CachePolicy                           m_cachePolicy;
			quint64                                            m_nextBackendId{1};
			quint64                                            m_nextMemoryOperationId{1};
			bool                                               m_maintenancePending{false};
			bool                                               m_releaseCompletedPending{false};
			bool                                               m_playbackInitializationAbandoned{false};
			bool                                               m_shuttingDown{false};
	};
} // namespace
#endif

bool SoundEngineSession::State::submit(SoundCommand command, const bool wait, int *result,
                                       int *selectedBuffer) const
{
	if (closed.load(std::memory_order_acquire))
		return false;
	command.sessionId  = sessionId;
	auto completion    = wait ? std::make_shared<SoundCommandCompletion>() : nullptr;
	command.completion = completion;
	{
		QMutexLocker locker(&ingress->mutex);
		if (!ingress->accepting || !ingress->worker || closed.load(std::memory_order_relaxed))
			return false;
#if QMUD_ENABLE_SOUND
		SoundWorker *target = ingress->worker;
		if (completion && QThread::currentThread() == target->thread())
			return false;
#endif
		ingress->commands.push_back(std::move(command));
		if (!ingress->drainPosted)
		{
			ingress->drainPosted = true;
#if QMUD_ENABLE_SOUND
			QMetaObject::invokeMethod(target, [target] { target->drain(); }, Qt::QueuedConnection);
#endif
		}
	}
	if (completion)
	{
		const int value = completion->wait(selectedBuffer);
		if (result)
			*result = value;
	}
	return true;
}

class SoundEngine::Private final
{
	public:
		class RetiredWorkerOwner;

		explicit Private(SoundEngine *owner) : owner(owner), ingress(std::make_shared<SoundCommandIngress>())
		{
#if QMUD_ENABLE_SOUND
			playbackInitializationWatchdog.setSingleShot(true);
			playbackInitializationWatchdog.setTimerType(Qt::PreciseTimer);
			QObject::connect(&playbackInitializationWatchdog, &QTimer::timeout, owner,
			                 [this] { static_cast<void>(abandonPlaybackInitialization(true)); });
			worker = new SoundWorker(
			    ingress,
			    [this](const quint64 sessionId, const QString &message)
			    {
				    SoundEngine *target = this->owner.load(std::memory_order_acquire);
				    if (!target)
					    return;
				    QMetaObject::invokeMethod(
				        target,
				        [this, sessionId, message]
				        {
					        if (!this->owner.load(std::memory_order_acquire))
						        return;
					        const auto it = callbacks.constFind(sessionId);
					        if (it != callbacks.constEnd() && it->playbackError)
						        it->playbackError(message);
				        },
				        Qt::QueuedConnection);
			    },
			    [this]
			    {
				    return playbackInitializationState.load(std::memory_order_acquire) ==
				           PlaybackInitializationState::Initializing;
			    },
			    [this]
			    {
				    auto expected = PlaybackInitializationState::Initializing;
				    if (!playbackInitializationState.compare_exchange_strong(
				            expected, PlaybackInitializationState::Ready, std::memory_order_acq_rel))
					    return false;
				    SoundEngine *target = this->owner.load(std::memory_order_acquire);
				    if (!target)
					    return false;
				    QMetaObject::invokeMethod(
				        target,
				        [this]
				        {
					        playbackInitializationWatchdog.stop();
					        if (SoundEngine *target = this->owner.load(std::memory_order_acquire))
						        emit target->playbackInitializationFinished(true);
				        },
				        Qt::QueuedConnection);
				    return true;
			    });
			worker->moveToThread(&thread);
			QObject::connect(&thread, &QThread::started, worker, [this] { worker->initialize(); });
			QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
			{
				QMutexLocker locker(&ingress->mutex);
				ingress->worker    = worker;
				ingress->accepting = true;
			}
			thread.setObjectName(QStringLiteral("QMud sound engine"));
			thread.start(QThread::HighPriority);
#endif
		}

		/**
		 * @brief Disables sound submissions and retires abandoned initialization work.
		 * @param notifyOwner Whether to report unavailable sound to the engine owner.
		 * @return `true` when this call changed initialization to abandoned.
		 */
		[[nodiscard]] bool abandonPlaybackInitialization(const bool notifyOwner)
		{
			auto expected = PlaybackInitializationState::Initializing;
			if (!playbackInitializationState.compare_exchange_strong(
			        expected, PlaybackInitializationState::Abandoned, std::memory_order_acq_rel))
				return false;

#if QMUD_ENABLE_SOUND
			playbackInitializationWatchdog.stop();
#endif
			std::deque<SoundCommand> abandonedCommands;
			{
				QMutexLocker locker(&ingress->mutex);
				ingress->accepting = false;
				abandonedCommands.swap(ingress->commands);
				ingress->drainPosted = false;
			}
			for (const SoundCommand &command : abandonedCommands)
			{
				if (command.completion)
					command.completion->complete(eCannotPlaySound);
			}
			requestWorkerRetirement();
			if (notifyOwner)
			{
				if (SoundEngine *target = owner.load(std::memory_order_acquire))
					emit target->playbackInitializationFinished(false);
			}
			return true;
		}

		/** @brief Queues orderly worker-thread cleanup after startup initialization returns. */
		void requestWorkerRetirement()
		{
#if QMUD_ENABLE_SOUND
			if (retirementRequested || !worker || !thread.isRunning())
				return;
			retirementRequested = true;
			SoundWorker *target = worker;
			QMetaObject::invokeMethod(target, [target] { target->retire(); }, Qt::QueuedConnection);
#endif
		}

		/**
		 * @brief Detaches owner callbacks and prepares blocked initialization for nonblocking destruction.
		 * @return `true` when this state must outlive the public engine until its worker finishes.
		 */
		[[nodiscard]] bool prepareForOwnerDestruction()
		{
			if (playbackInitializationState.load(std::memory_order_acquire) ==
			    PlaybackInitializationState::Initializing)
				static_cast<void>(abandonPlaybackInitialization(false));
			else if (playbackInitializationState.load(std::memory_order_acquire) ==
			         PlaybackInitializationState::Abandoned)
				requestWorkerRetirement();
#if QMUD_ENABLE_SOUND
			playbackInitializationWatchdog.stop();
			QObject::disconnect(&playbackInitializationWatchdog, nullptr, nullptr, nullptr);
#endif
			owner.store(nullptr, std::memory_order_release);
#if QMUD_ENABLE_SOUND
			return playbackInitializationState.load(std::memory_order_acquire) ==
			           PlaybackInitializationState::Abandoned &&
			       thread.isRunning();
#else
			return false;
#endif
		}

		/**
		 * @brief Retains abandoned private state until its worker thread finishes.
		 * @param state Private state detached from its public engine.
		 */
		static void retainUntilWorkerStops(std::unique_ptr<Private> state);

		~Private()
		{
#if QMUD_ENABLE_SOUND
			{
				QMutexLocker locker(&ingress->mutex);
				ingress->accepting = false;
			}
			for (const std::weak_ptr<SoundEngineSession::State> &state : std::as_const(sessionStates))
			{
				if (const std::shared_ptr<SoundEngineSession::State> retained = state.lock())
					retained->closed.store(true, std::memory_order_release);
			}
			if (worker && thread.isRunning())
				QMetaObject::invokeMethod(
				    worker, [this] { worker->shutdown(); }, Qt::BlockingQueuedConnection);
			{
				QMutexLocker locker(&ingress->mutex);
				ingress->worker = nullptr;
				while (!ingress->commands.empty())
				{
					SoundCommand command = std::move(ingress->commands.front());
					ingress->commands.pop_front();
					if (command.completion)
						command.completion->complete(eCannotPlaySound);
				}
			}
			thread.quit();
			thread.wait();
			worker = nullptr;
#endif
		}

		bool submit(SoundCommand command, const bool wait, int *result = nullptr) const
		{
#if QMUD_ENABLE_SOUND
			std::shared_ptr<SoundCommandCompletion> completion;
			if (wait)
			{
				completion         = std::make_shared<SoundCommandCompletion>();
				command.completion = completion;
			}
			{
				QMutexLocker locker(&ingress->mutex);
				if (!ingress->accepting || !ingress->worker)
					return false;
				SoundWorker *target = ingress->worker;
				if (completion && QThread::currentThread() == target->thread())
					return false;
				ingress->commands.push_back(std::move(command));
				if (!ingress->drainPosted)
				{
					ingress->drainPosted = true;
					QMetaObject::invokeMethod(target, [target] { target->drain(); }, Qt::QueuedConnection);
				}
			}
			if (completion)
			{
				const int value = completion->wait();
				if (result)
					*result = value;
			}
			return true;
#else
			Q_UNUSED(command);
			Q_UNUSED(wait);
			Q_UNUSED(result);
			return false;
#endif
		}

		std::atomic<SoundEngine *>                               owner{nullptr};
		std::shared_ptr<SoundCommandIngress>                     ingress;
		QHash<quint64, SoundEngine::SessionCallbacks>            callbacks;
		QHash<quint64, std::weak_ptr<SoundEngineSession::State>> sessionStates;
		std::atomic<quint64>                                     nextSessionId{1};
		std::atomic<PlaybackInitializationState>                 playbackInitializationState{
		    PlaybackInitializationState::Idle};
		inline static std::atomic_int retiredWorkerCount{0};
		SoundEngine::CachePolicy      cachePolicy;
#if QMUD_ENABLE_SOUND
		QThread      thread;
		QTimer       playbackInitializationWatchdog;
		SoundWorker *worker{nullptr};
		bool         retirementRequested{false};
#endif
};

#if QMUD_ENABLE_SOUND
class SoundEngine::Private::RetiredWorkerOwner final : public QObject
{
	public:
		/**
		 * @brief Keeps abandoned worker state alive until its thread finishes.
		 * @param state Private state detached from the destroyed public engine.
		 * @param parent Application object owning this retirement guard.
		 */
		RetiredWorkerOwner(std::unique_ptr<Private> state, QObject *parent)
		    : QObject(parent), m_state(std::move(state))
		{
			Q_ASSERT(m_state);
			Private::retiredWorkerCount.fetch_add(1, std::memory_order_relaxed);
			QObject::connect(&m_state->thread, &QThread::finished, this, &QObject::deleteLater,
			                 Qt::QueuedConnection);
			if (!m_state->thread.isRunning())
				QMetaObject::invokeMethod(this, &QObject::deleteLater, Qt::QueuedConnection);
		}

		/** @brief Releases permanently blocked state when the application itself is terminating. */
		~RetiredWorkerOwner() override
		{
			if (m_state && m_state->thread.isRunning())
			{
				[[maybe_unused]] Private *const processLifetimeState = m_state.release();
			}
			Private::retiredWorkerCount.fetch_sub(1, std::memory_order_relaxed);
		}

	private:
		std::unique_ptr<Private> m_state;
};

void SoundEngine::Private::retainUntilWorkerStops(std::unique_ptr<Private> state)
{
	QObject *application = QCoreApplication::instance();
	if (!application)
	{
		[[maybe_unused]] Private *const processLifetimeState = state.release();
		return;
	}
	new RetiredWorkerOwner(std::move(state), application);
}
#else
void SoundEngine::Private::retainUntilWorkerStops(std::unique_ptr<Private>)
{
}
#endif

SoundEngine::CachePolicy SoundEngine::CachePolicy::normalized(CachePolicy policy) noexcept
{
	policy.ramCacheSizeMiB = qBound(kMinimumRamCacheSizeMiB, policy.ramCacheSizeMiB, kMaximumRamCacheSizeMiB);
	policy.idleEvictionSeconds =
	    qBound(kMinimumIdleEvictionSeconds, policy.idleEvictionSeconds, kMaximumIdleEvictionSeconds);
	return policy;
}

SoundEngine::SoundEngine(QObject *parent) : QObject(parent), m_private(std::make_unique<Private>(this))
{
}

SoundEngine::~SoundEngine()
{
	if (m_private && m_private->prepareForOwnerDestruction())
		Private::retainUntilWorkerStops(std::move(m_private));
}

void SoundEngine::initializePlayback()
{
	auto expected = PlaybackInitializationState::Idle;
	if (!m_private->playbackInitializationState.compare_exchange_strong(
	        expected, PlaybackInitializationState::Initializing, std::memory_order_acq_rel))
	{
		if (expected == PlaybackInitializationState::Ready ||
		    expected == PlaybackInitializationState::Abandoned)
		{
			const bool available = expected == PlaybackInitializationState::Ready;
			QMetaObject::invokeMethod(
			    this, [this, available] { emit playbackInitializationFinished(available); },
			    Qt::QueuedConnection);
		}
		return;
	}
#if QMUD_ENABLE_SOUND
	static constexpr int kPlaybackInitializationTimeoutMilliseconds = 30'000;
	m_private->playbackInitializationWatchdog.start(kPlaybackInitializationTimeoutMilliseconds);
#endif
	SoundCommand command;
	command.kind = SoundCommandKind::InitializePlayback;
	if (!m_private->submit(std::move(command), false))
		static_cast<void>(m_private->abandonPlaybackInitialization(true));
}

std::shared_ptr<SoundEngineSession> SoundEngine::registerSession(const QString   &startupDirectory,
                                                                 SessionCallbacks callbacks) const
{
	auto state       = std::make_shared<SoundEngineSession::State>();
	state->ingress   = m_private->ingress;
	state->sessionId = m_private->nextSessionId.fetch_add(1, std::memory_order_relaxed);
	SoundCommand command;
	command.kind                      = SoundCommandKind::RegisterSession;
	command.sessionId                 = state->sessionId;
	command.text                      = startupDirectory;
	command.publishedState            = state->publishedState;
	command.luaAudioOwnershipReleased = std::move(callbacks.luaAudioOwnershipReleased);
	int result                        = eCannotPlaySound;
	if (!m_private->submit(std::move(command), true, &result) || result != eOK)
		return {};
	m_private->callbacks.insert(state->sessionId, std::move(callbacks));
	m_private->sessionStates.insert(state->sessionId, state);
	return std::shared_ptr<SoundEngineSession>(new SoundEngineSession(std::move(state)));
}

void SoundEngine::unregisterSession(const std::shared_ptr<SoundEngineSession> &session) const
{
	if (!session)
		return;
	m_private->callbacks.remove(session->id());
	m_private->sessionStates.remove(session->id());
	session->close();
}

void SoundEngine::configureCache(CachePolicy policy) const
{
	policy = CachePolicy::normalized(policy);
	if (policy == m_private->cachePolicy)
		return;
#if QMUD_ENABLE_SOUND
	SoundCommand command;
	command.kind        = SoundCommandKind::ConfigureCache;
	command.cachePolicy = policy;
	if (m_private->submit(std::move(command), false))
		m_private->cachePolicy = policy;
#else
	m_private->cachePolicy = policy;
#endif
}

SoundEngine::CachePolicy SoundEngine::cachePolicy() const noexcept
{
	return m_private->cachePolicy;
}

void SoundEngine::addCacheSourceForTest(const QString &identity, const qint64 sourceBytes, const bool idle,
                                        const qint64 usedAtMsecs) const
{
#if QMUD_ENABLE_SOUND
	QMetaObject::invokeMethod(
	    m_private->worker, [worker = m_private->worker, identity, sourceBytes, idle, usedAtMsecs]
	    { worker->addCacheSourceForTest(identity, sourceBytes, idle, usedAtMsecs); },
	    Qt::BlockingQueuedConnection);
#else
	Q_UNUSED(identity);
	Q_UNUSED(sourceBytes);
	Q_UNUSED(idle);
	Q_UNUSED(usedAtMsecs);
#endif
}

SoundEngine::CacheMaintenanceRunForTest
SoundEngine::configureCacheAndRunMaintenanceForTest(CachePolicy policy) const
{
#if QMUD_ENABLE_SOUND
	CacheMaintenanceRunForTest result;
	QMetaObject::invokeMethod(
	    m_private->worker,
	    [worker = m_private->worker, policy, &result]
	    {
		    const SoundWorkerCacheMaintenanceRun workerResult =
		        worker->configureCacheAndRunMaintenanceForTest(policy);
		    result.configured = {workerResult.configured.totalBytes, workerResult.configured.sourceCount,
		                         workerResult.configured.backendCount,
		                         workerResult.configured.maintenancePending};
		    result.firstStep  = {workerResult.firstStep.totalBytes, workerResult.firstStep.sourceCount,
		                         workerResult.firstStep.backendCount,
		                         workerResult.firstStep.maintenancePending};
		    result.secondStep = {workerResult.secondStep.totalBytes, workerResult.secondStep.sourceCount,
		                         workerResult.secondStep.backendCount,
		                         workerResult.secondStep.maintenancePending};
	    },
	    Qt::BlockingQueuedConnection);
	return result;
#else
	Q_UNUSED(policy);
	return {};
#endif
}

bool SoundEngine::canceledMemoryResourceIsIndexedForTest() const
{
#if QMUD_ENABLE_SOUND
	bool indexed = false;
	QMetaObject::invokeMethod(
	    m_private->worker, [worker = m_private->worker, &indexed]
	    { indexed = worker->canceledMemoryResourceIsIndexedForTest(); }, Qt::BlockingQueuedConnection);
	return indexed;
#else
	return false;
#endif
}

bool SoundEngine::completeReservedChannelForTest(const std::shared_ptr<SoundEngineSession> &session,
                                                 const int                                  buffer) const
{
#if QMUD_ENABLE_SOUND
	if (!session || !session->m_state || session->m_state->ingress != m_private->ingress ||
	    session->m_state->closed.load(std::memory_order_acquire) || buffer < 1 ||
	    buffer > SoundEngineSession::kChannelCount)
		return false;
	bool completed = false;
	QMetaObject::invokeMethod(
	    m_private->worker, [worker = m_private->worker, sessionId = session->id(), buffer, &completed]
	    { completed = worker->completeReservedChannelForTest(sessionId, buffer); },
	    Qt::BlockingQueuedConnection);
	return completed;
#else
	Q_UNUSED(session);
	Q_UNUSED(buffer);
	return false;
#endif
}

quint64 SoundEngine::failedPreparedSoundForTest(const std::shared_ptr<SoundEngineSession> &session,
                                                const QString                             &message,
                                                const bool initializationFailure) const
{
#if QMUD_ENABLE_SOUND
	if (!session || !session->m_state || session->m_state->ingress != m_private->ingress ||
	    session->m_state->closed.load(std::memory_order_acquire) || message.isEmpty())
		return 0;
	const quint64 token = session->m_state->nextPreparationToken.fetch_add(1, std::memory_order_relaxed);
	if (token == 0)
		return 0;
	bool prepared = false;
	QMetaObject::invokeMethod(
	    m_private->worker,
	    [worker = m_private->worker, sessionId = session->id(), token, message, initializationFailure,
	     &prepared]
	    { prepared = worker->failedPreparedSoundForTest(sessionId, token, message, initializationFailure); },
	    Qt::BlockingQueuedConnection);
	return prepared ? token : 0;
#else
	Q_UNUSED(session);
	Q_UNUSED(message);
	Q_UNUSED(initializationFailure);
	return 0;
#endif
}

SoundEngine::CacheStateForTest SoundEngine::cacheStateForTest() const
{
#if QMUD_ENABLE_SOUND
	CacheStateForTest state;
	QMetaObject::invokeMethod(
	    m_private->worker,
	    [worker = m_private->worker, &state]
	    {
		    const SoundWorkerCacheState workerState = worker->cacheStateForTest();
		    state = {workerState.totalBytes, workerState.sourceCount, workerState.backendCount,
		             workerState.maintenancePending};
	    },
	    Qt::BlockingQueuedConnection);
	return state;
#else
	return {};
#endif
}

SoundEngine::PlaybackInitializationStateForTest SoundEngine::playbackInitializationStateForTest() const
{
#if QMUD_ENABLE_SOUND
	PlaybackInitializationStateForTest state;
	QMetaObject::invokeMethod(
	    m_private->worker,
	    [worker = m_private->worker, &state]
	    {
		    const SoundWorkerPlaybackInitializationState workerState =
		        worker->playbackInitializationStateForTest();
		    state = {workerState.effectReady, workerState.mediaPlayerReady};
	    },
	    Qt::BlockingQueuedConnection);
	return state;
#else
	return {};
#endif
}

void SoundEngine::setPlaybackInitializationBarrierForTest(std::function<void()> barrier) const
{
#if QMUD_ENABLE_SOUND
	QMetaObject::invokeMethod(
	    m_private->worker, [worker = m_private->worker, barrier = std::move(barrier)]() mutable
	    { worker->setPlaybackInitializationBarrierForTest(std::move(barrier)); },
	    Qt::BlockingQueuedConnection);
#else
	Q_UNUSED(barrier);
#endif
}

void SoundEngine::expirePlaybackInitializationWatchdogForTest() const
{
	static_cast<void>(m_private->abandonPlaybackInitialization(true));
}

bool SoundEngine::soundWorkerRunningForTest() const
{
#if QMUD_ENABLE_SOUND
	return m_private->thread.isRunning();
#else
	return false;
#endif
}

int SoundEngine::retiredWorkerCountForTest() noexcept
{
	return Private::retiredWorkerCount.load(std::memory_order_relaxed);
}

SoundEngineSession::SoundEngineSession(std::shared_ptr<State> state) : m_state(std::move(state))
{
}

int SoundEngineSession::playFile(const int buffer, const QString &fileName, const bool loop,
                                 const double volume, const double pan, const quint64 luaAudioGeneration,
                                 int *selectedBuffer, const bool requireReservation) const
{
	if (buffer < 0 || buffer > kChannelCount || (buffer == 0 && fileName.isEmpty()))
		return eBadParameter;
	SoundCommand command;
	command.kind               = SoundCommandKind::PlayFile;
	command.buffer             = buffer;
	command.text               = fileName;
	command.loop               = loop;
	command.volume             = volume;
	command.pan                = pan;
	command.luaAudioGeneration = luaAudioGeneration;
	command.requireReservation = requireReservation;
	int result                 = eCannotPlaySound;
	return m_state && m_state->submit(std::move(command), true, &result, selectedBuffer) ? result
	                                                                                     : eCannotPlaySound;
}

int SoundEngineSession::reserveLuaAudioChannel(const QString &fileName,
                                               const quint64  luaAudioGeneration) const
{
	if (fileName.isEmpty() || luaAudioGeneration == 0)
		return 0;
	SoundCommand command;
	command.kind               = SoundCommandKind::ReserveLuaAudioChannel;
	command.text               = fileName;
	command.luaAudioGeneration = luaAudioGeneration;
	int result                 = eCannotPlaySound;
	int selectedBuffer         = 0;
	if (!m_state || !m_state->submit(std::move(command), true, &result, &selectedBuffer) || result != eOK)
		return 0;
	return selectedBuffer;
}

bool SoundEngineSession::confirmLuaAudioOwnership(const int buffer, const quint64 luaAudioGeneration) const
{
	if (buffer < 1 || buffer > kChannelCount || luaAudioGeneration == 0)
		return false;
	SoundCommand command;
	command.kind               = SoundCommandKind::ConfirmLuaAudioOwnership;
	command.buffer             = buffer;
	command.luaAudioGeneration = luaAudioGeneration;
	int result                 = eCannotPlaySound;
	return m_state && m_state->submit(std::move(command), true, &result) && result == eOK;
}

int SoundEngineSession::stopLuaAudioChannel(const int buffer, const quint64 luaAudioGeneration) const
{
	if (buffer < 1 || buffer > kChannelCount || luaAudioGeneration == 0)
		return eBadParameter;
	SoundCommand command;
	command.kind               = SoundCommandKind::Stop;
	command.buffer             = buffer;
	command.luaAudioGeneration = luaAudioGeneration;
	int result                 = eCannotPlaySound;
	return m_state && m_state->submit(std::move(command), true, &result) ? result : eCannotPlaySound;
}

void SoundEngineSession::updateStartupDirectory(const QString &startupDirectory) const
{
	SoundCommand command;
	command.kind = SoundCommandKind::UpdateStartupDirectory;
	command.text = startupDirectory;
	if (m_state)
		static_cast<void>(m_state->submit(std::move(command), false));
}

int SoundEngineSession::playMemory(const int buffer, const QByteArray &data, const bool loop,
                                   const double volume, const double pan, const quint64 luaAudioGeneration,
                                   int *selectedBuffer) const
{
	if (buffer < 0 || buffer > kChannelCount || data.isEmpty())
		return eBadParameter;
	SoundCommand command;
	command.kind               = SoundCommandKind::PlayMemory;
	command.buffer             = buffer;
	command.bytes              = data;
	command.loop               = loop;
	command.volume             = volume;
	command.pan                = pan;
	command.luaAudioGeneration = luaAudioGeneration;
	int result                 = eCannotPlaySound;
	return m_state && m_state->submit(std::move(command), true, &result, selectedBuffer) ? result
	                                                                                     : eCannotPlaySound;
}

int SoundEngineSession::stop(const int buffer) const
{
	if (buffer < 0 || buffer > kChannelCount)
		return eBadParameter;
	SoundCommand command;
	command.kind   = SoundCommandKind::Stop;
	command.buffer = buffer;
	int result     = eCannotPlaySound;
	return m_state && m_state->submit(std::move(command), true, &result) ? result : eCannotPlaySound;
}

quint64 SoundEngineSession::prepareFile(const QString &fileName) const
{
	if (!m_state || fileName.isEmpty() || m_state->closed.load(std::memory_order_acquire))
		return 0;
	const quint64 token = m_state->nextPreparationToken.fetch_add(1, std::memory_order_relaxed);
	SoundCommand  command;
	command.kind  = SoundCommandKind::PrepareFile;
	command.token = token;
	command.text  = fileName;
	return m_state->submit(std::move(command), false) ? token : 0;
}

int SoundEngineSession::commitPrepared(const quint64 token, const int buffer, const bool loop,
                                       const double volume, const double pan) const
{
	if (token == 0 || buffer < 0 || buffer > kChannelCount)
		return eBadParameter;
	SoundCommand command;
	command.kind   = SoundCommandKind::CommitPrepared;
	command.token  = token;
	command.buffer = buffer;
	command.loop   = loop;
	command.volume = volume;
	command.pan    = pan;
	int result     = eCannotPlaySound;
	return m_state && m_state->submit(std::move(command), true, &result) ? result : eCannotPlaySound;
}

void SoundEngineSession::cancelPrepared(const quint64 token) const
{
	if (token == 0)
		return;
	SoundCommand command;
	command.kind  = SoundCommandKind::CancelPrepared;
	command.token = token;
	if (m_state)
		static_cast<void>(m_state->submit(std::move(command), false));
}

int SoundEngineSession::status(const int buffer) const
{
	if (buffer < 1 || buffer > kChannelCount)
		return -1;
	const auto state = snapshot();
	return state ? state->status[static_cast<std::size_t>(buffer - 1)] : -3;
}

std::shared_ptr<const SoundEngineSession::Snapshot> SoundEngineSession::snapshot() const
{
	if (!m_state || m_state->closed.load(std::memory_order_acquire))
		return {};
	return m_state->publishedState->snapshot.load(std::memory_order_acquire);
}

void SoundEngineSession::close() const
{
	if (!m_state || m_state->closed.exchange(true, std::memory_order_acq_rel))
		return;
	SoundCommand command;
	command.kind       = SoundCommandKind::CloseSession;
	command.sessionId  = m_state->sessionId;
	auto completion    = std::make_shared<SoundCommandCompletion>();
	command.completion = completion;
	{
		QMutexLocker locker(&m_state->ingress->mutex);
		if (!m_state->ingress->accepting || !m_state->ingress->worker)
			return;
#if QMUD_ENABLE_SOUND
		SoundWorker *target = m_state->ingress->worker;
		if (QThread::currentThread() == target->thread())
			return;
#endif
		m_state->ingress->commands.push_back(std::move(command));
		if (!m_state->ingress->drainPosted)
		{
			m_state->ingress->drainPosted = true;
#if QMUD_ENABLE_SOUND
			QMetaObject::invokeMethod(target, [target] { target->drain(); }, Qt::QueuedConnection);
#endif
		}
	}
	static_cast<void>(completion->wait());
}

quint64 SoundEngineSession::id() const noexcept
{
	return m_state ? m_state->sessionId : 0;
}
