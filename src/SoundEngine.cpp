/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: SoundEngine.cpp
 * Role: Process-wide threaded sound playback service and reusable multimedia-object cache.
 */

#include "SoundEngine.h"

#include "AtomicSharedPtrCompat.h"
#include "helpers/PluginPathUtils.h"
#include "helpers/SoundCacheUtils.h"
#include "helpers/SoundFileRoutingUtils.h"
#include "scripting/ScriptingErrors.h"

#include <QCoreApplication>
// ReSharper disable once CppUnusedIncludeDirective
#include <QCryptographicHash>
#include <QDataStream>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
// ReSharper disable once CppUnusedIncludeDirective
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMetaObject>
// ReSharper disable once CppUnusedIncludeDirective
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
// ReSharper disable once CppUnusedIncludeDirective
#include <QMutex>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSet>
#include <QSignalBlocker>
// ReSharper disable once CppUnusedIncludeDirective
#include <QRunnable>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryFile>
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
#include <QFileSystemWatcher>
#include <QMediaDevices>
#include <QMediaPlayer>
#include <QTimer>
#include <QUrl>
#endif

namespace
{
	constexpr qsizetype kCatalogEntriesPerStep                 = 128;
	constexpr int       kSoundDownloadStallTimeoutMilliseconds = 60'000;
	constexpr quint32   kSoundCatalogIndexMagic                = 0x514D5349U;
	constexpr quint32   kSoundCatalogIndexVersion              = 1U;
	constexpr auto      kSoundCatalogIndexPath                 = "worlds/state/sound-index.qss";

#if QMUD_ENABLE_SOUND
	struct SoundMemoryResource;
#endif

	struct SoundSessionPublishedState
	{
			QMudAtomicSharedPtr<const SoundEngineSession::Snapshot> snapshot{
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
		ConfigureSoundDirectory,
		ConfigurePluginDirectory,
		UpdateWorldSoundDirectory,
		PlayFile,
		PlayMemory,
		MemoryDigestReady,
		MemoryFileReady,
		CatalogIndexBatch,
		CatalogIndexLoaded,
		CatalogScanStarted,
		CatalogDirectoryBatch,
		CatalogDirectoryScanned,
		CatalogFileAdded,
		PluginDirectoryChanged,
		CatalogCleared,
		CatalogReady,
		CatalogWatchState,
		DownloadFinished,
		ReserveLuaAudioChannel,
		ConfirmLuaAudioOwnership,
		NativeSoundRequest,
		Stop,
		PrepareFile,
		CommitPrepared,
		CancelPrepared,
		CloseSession,
		ConfigureCache
	};

	enum class SoundOwnershipKind : quint8
	{
		None,
		LuaAudio,
		NativeSound
	};

	struct SoundCommand
	{
			SoundCommandKind                            kind{SoundCommandKind::PlayFile};
			quint64                                     sessionId{0};
			quint64                                     token{0};
			quint64                                     catalogRevision{0};
			int                                         buffer{0};
			QString                                     text;
			QString                                     url;
			QString                                     worldSoundDirectory;
			QString                                     parentDirectory;
			QStringList                                 files;
			QStringList                                 changedFiles;
			QStringList                                 removedFiles;
			QStringList                                 directories;
			QStringList                                 removedDirectories;
			QByteArray                                  bytes;
			QByteArray                                  digest;
			bool                                        loop{false};
			bool                                        music{false};
			bool                                        continueNativePlayback{false};
			bool                                        stopNativePlayback{false};
			int                                         repeatCount{1};
			int                                         priority{50};
			double                                      volume{0.0};
			double                                      pan{0.0};
			QMudNativeSoundProtocol                     nativeProtocol{QMudNativeSoundProtocol::Msp};
			quint64                                     ownershipGeneration{0};
			SoundOwnershipKind                          ownershipKind{SoundOwnershipKind::None};
			bool                                        requireReservation{false};
			bool                                        success{false};
			bool                                        directoryWatched{false};
			int                                         watchedDirectoryCount{0};
			int                                         failedWatchCount{0};
			qsizetype                                   maximumReconciliationStepSize{0};
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
	struct SoundSourceKey
	{
			QString       identity;
			qint64        size{0};
			qint64        modifiedMsecs{0};
			QByteArray    deviceId;

			bool          operator==(const SoundSourceKey &) const = default;

			/**
			 * @brief Hashes one loaded-source identity for process-wide cache accounting.
			 * @param key Source identity to hash.
			 * @param seed Caller-provided hash seed.
			 * @return Hash value for the source identity.
			 */
			friend size_t qHash(const SoundSourceKey &key, const size_t seed = 0) noexcept
			{
				return qHashMulti(seed, key.identity, key.size, key.modifiedMsecs, key.deviceId);
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
				if (!fileName.isEmpty())
					static_cast<void>(QFile::remove(fileName));
			}
	};

	struct SoundBackend
	{
			SoundSourceKey                       key;
			quint64                              backendId{0};
			std::shared_ptr<SoundMemoryResource> memoryResource;
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
			qint64        retainedAtMsecs{0};
	};

	struct SoundChannel
	{
			SoundBackend      *backend{nullptr};
			quint64            assignmentGeneration{0};
			quint64            ownershipGeneration{0};
			SoundOwnershipKind ownershipKind{SoundOwnershipKind::None};
			int                repeatCount{1};
			int                remainingPlays{0};
			QStringList        playbackFiles;
			QString            nativeFilePattern;
			QString            nativeDownloadUrl;
			QString            nativeDownloadDestination;
			QString            nativeSoundCatalogPrefix;
			quint64            pendingMemoryOperationId{0};
			bool               allocated{false};
			bool               playing{false};
			bool               playbackStarted{false};
			bool               looping{false};
			bool               backendLooping{false};
			bool               nativeMusic{false};
			bool               nativeResolutionPending{false};
			bool               luaAudioOwnershipConfirmed{false};
			double             volume{1.0};
			double             pan{0.0};
	};

	/** @brief One supported regular file indexed below the configured sounds root. */
	struct SoundCatalogFile
	{
			QString absolutePath;
			QString relativePath;
			QString fileName;
	};

	/** @brief Filesystem metadata used to identify an overwritten catalog file. */
	struct SoundCatalogFileState
	{
			qint64 size{0};
			qint64 modifiedMsecs{0};

			bool   operator==(const SoundCatalogFileState &) const = default;
	};

	/** @brief One indexed directory, its parent, direct files, and direct child directories. */
	struct SoundCatalogDirectoryState
	{
			QString                               parent;
			QHash<QString, SoundCatalogFileState> files;
			QSet<QString>                         children;
	};

	/** @brief One directly watched plugin directory and its supported audio files. */
	struct PluginSoundDirectoryState
	{
			QHash<QString, QPair<QString, SoundCatalogFileState>> files;
	};

	/** @brief Playback-side membership for one indexed directory. */
	struct SoundPlaybackCatalogDirectory
	{
			QString       parent;
			QSet<QString> files;
			QSet<QString> children;
	};

	/** @brief One in-flight native protocol download shared by every waiter for its destination. */
	struct PendingSoundDownload
	{
			QString                         destinationPath;
			std::unique_ptr<QTemporaryFile> output;
			QPointer<QNetworkReply>         reply;
			bool                            writeFailed{false};
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
			qint64        retainedAtMsecs{0};
	};

	struct WorkerSession
	{
			struct NativePlayback
			{
					int     buffer{0};
					quint64 generation{0};
					QString filePattern;
					int     priority{0};
			};

			quint64                                                     sessionId{0};
			QString                                                     worldSoundDirectorySetting;
			QString                                                     worldSoundCatalogPrefix;
			QHash<QString, SoundSourceKey>                              cachedFileRequestKeys;
			QMultiHash<QString, QString>                                cachedFileRequestsBySource;
			std::shared_ptr<SoundSessionPublishedState>                 publishedState;
			std::array<SoundChannel, SoundEngineSession::kChannelCount> channels{};
			std::array<NativePlayback, 4>                               nativePlayback{};
			QHash<quint64, PreparedSound>                               prepared;
			QVector<QPair<int, quint64>>                                pendingLuaAudioCompletions;
			std::function<void(int, quint64)>                           luaAudioOwnershipReleased;
			quint64                                                     nextAssignmentGeneration{1};
			quint64                                                     nextNativeSoundGeneration{1};
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

	/** @brief Source-free media-player readiness before the first sound request. */
	struct SoundWorkerPlaybackInitializationState
	{
			bool mediaPlayerReady{false};
	};

	/** @brief Worker sound-file catalog state sampled by tests. */
	struct SoundWorkerFileCatalogState
	{
			qsizetype fileCount{0};
			qsizetype directoryCount{0};
			qsizetype watchedDirectoryCount{0};
			qsizetype failedWatchCount{0};
			qsizetype maximumIndexBatchSize{0};
			qsizetype maximumDirectoryBatchSize{0};
			qsizetype maximumReconciliationStepSize{0};
			qsizetype watchStatePathEntryCount{0};
			quint64   revision{0};
			bool      ready{false};
	};

	/** @brief Worker-owned native playback state sampled by tests. */
	struct SoundWorkerNativePlaybackState
	{
			int     buffer{0};
			QString filePattern;
			int     priority{0};
			bool    current{false};
	};

	/**
	 * @brief Produces the process-wide case-insensitive identity used by the sound catalog.
	 * @param value Catalog name or path.
	 * @return Case-folded identity.
	 */
	[[nodiscard]] QString soundCatalogComparisonKey(const QString &value)
	{
		return value.toCaseFolded();
	}

	/**
	 * @brief Compares two normalized sound paths with MUSHclient-compatible casing.
	 * @param lhs First path.
	 * @param rhs Second path.
	 * @return `true` when both paths identify the same catalog location.
	 */
	[[nodiscard]] bool sameSoundFilesystemPath(const QString &lhs, const QString &rhs)
	{
		return lhs.compare(rhs, Qt::CaseInsensitive) == 0;
	}

	/** @brief Filesystem, watcher, persistent-index, and network work isolated from playback. */
	class SoundIoWorker final : public QObject
	{
		public:
			using Publish = std::function<void(SoundCommand)>;

			/**
			 * @brief Creates the sound I/O worker.
			 * @param publish Thread-safe destination for playback-worker results.
			 */
			explicit SoundIoWorker(Publish publish) : m_publish(std::move(publish))
			{
			}

			/** @brief Initializes codec filters and event-loop-owned watcher state on the I/O thread. */
			void initialize()
			{
				Q_ASSERT(QThread::currentThread() == thread());
				const QSet<QString> &suffixes = soundPlaybackSupportedFileSuffixes();
				m_soundNameFilters.reserve(suffixes.size());
				for (const QString &suffix : suffixes)
					m_soundNameFilters.push_back(QStringLiteral("*.%1").arg(suffix));
				m_fileWatcher = std::make_unique<QFileSystemWatcher>();
				connect(m_fileWatcher.get(), &QFileSystemWatcher::directoryChanged, this,
				        [this](const QString &path) { handleWatchedDirectoryChanged(path); });
			}

			/**
			 * @brief Configures a new QMud home, loads its cache, and begins background reconciliation.
			 * @param qmudHomeDirectory Existing QMud home directory.
			 * @param generation Playback-worker configuration generation.
			 * @param pluginGeneration Plugin-root configuration generation.
			 */
			void configureSoundDirectory(const QString &qmudHomeDirectory, const quint64 generation,
			                             const quint64 pluginGeneration)
			{
				Q_ASSERT(QThread::currentThread() == thread());
				cancelDownloads();
				clearCatalogState();
				m_generation       = generation;
				m_pluginGeneration = pluginGeneration;
				m_qmudHome         = QMudPluginPathUtils::normalizeSeparators(
				    QFileInfo(qmudHomeDirectory).canonicalFilePath());
				if (m_qmudHome.isEmpty())
				{
					publishCatalogCleared(true);
					return;
				}
				static_cast<void>(watchDirectory(m_qmudHome));
				m_soundsPath = QMudPluginPathUtils::normalizeSeparators(
				    QDir(m_qmudHome).filePath(QStringLiteral("sounds")));
				if (!QDir().mkpath(m_soundsPath) || soundPathContainsSymlink(m_qmudHome, m_soundsPath))
				{
					publishCatalogCleared(true);
					return;
				}
				m_soundsRoot =
				    QMudPluginPathUtils::normalizeSeparators(QFileInfo(m_soundsPath).canonicalFilePath());
				if (m_soundsRoot.isEmpty() ||
				    !QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(m_soundsRoot, m_qmudHome))
				{
					publishCatalogCleared(true);
					return;
				}
				static_cast<void>(watchDirectory(m_soundsRoot));
				loadPersistentIndex();
				queueDirectory(m_soundsRoot);
				publishWatchState();
			}

			/**
			 * @brief Replaces the configured plugin-sound root without disturbing sounds-tree work.
			 * @param pluginsDirectory Validated canonical directory, or an empty string to disable plugin sounds.
			 * @param generation Plugin-root generation owning later tracking requests and results.
			 */
			void configurePluginDirectory(const QString &pluginsDirectory, const quint64 generation)
			{
				if (m_shuttingDown)
					return;
				clearPluginDirectoryState();
				m_pluginGeneration = generation;
				if (pluginsDirectory.isEmpty() || m_qmudHome.isEmpty())
					return;

				const QString   normalized = QMudPluginPathUtils::normalizeSeparators(pluginsDirectory);
				const QFileInfo info(normalized);
				const QString canonical = QMudPluginPathUtils::normalizeSeparators(info.canonicalFilePath());
				if (!info.isDir() || info.isSymLink() || canonical.isEmpty() ||
				    !QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(canonical, m_qmudHome) ||
				    soundPathContainsSymlink(m_qmudHome, normalized))
				{
					return;
				}
				m_pluginsRoot = canonical;
			}

			/** @brief Cancels I/O and releases all event-loop-owned state before thread shutdown. */
			void shutdown()
			{
				Q_ASSERT(QThread::currentThread() == thread());
				m_shuttingDown = true;
				cancelDownloads();
				finishPersistentIndexWriteForShutdown();
				clearCatalogState();
				m_fileWatcher.reset();
			}

			/**
			 * @brief Starts or joins one native sound download.
			 * @param generation Plugin-root generation owning the request.
			 * @param destination Contained final file path.
			 * @param requestUrl Valid HTTP(S) URL.
			 */
			void startDownload(const quint64 generation, const QString &destination, const QUrl &requestUrl)
			{
				Q_ASSERT(QThread::currentThread() == thread());
				if (m_shuttingDown || generation != m_generation)
					return;
				const QString downloadKey = soundCatalogComparisonKey(destination);
				if (m_downloads.contains(downloadKey))
					return;

				const QFileInfo existing(destination);
				if (existing.exists())
				{
					publishDownloadFinished(destination, publishAvailableCatalogFile(existing));
					return;
				}

				const QString parentPath = QFileInfo(destination).absolutePath();
				if (!QDir().mkpath(parentPath) || soundPathContainsSymlink(m_soundsRoot, parentPath))
				{
					publishDownloadFinished(destination, false);
					return;
				}
				const QString canonicalParent =
				    QMudPluginPathUtils::normalizeSeparators(QFileInfo(parentPath).canonicalFilePath());
				if (canonicalParent.isEmpty() ||
				    !QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(canonicalParent, m_soundsRoot))
				{
					publishDownloadFinished(destination, false);
					return;
				}

				auto download             = std::make_shared<PendingSoundDownload>();
				download->destinationPath = destination;
				download->output =
				    std::make_unique<QTemporaryFile>(destination + QStringLiteral(".download-XXXXXX"));
				download->output->setAutoRemove(true);
				if (!download->output->open())
				{
					publishDownloadFinished(destination, false);
					return;
				}

				if (!m_networkManager)
					m_networkManager = std::make_unique<QNetworkAccessManager>();
				QNetworkRequest request(requestUrl);
				request.setRawHeader("User-Agent", "QMud");
				request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
				                     QNetworkRequest::NoLessSafeRedirectPolicy);
				request.setTransferTimeout(m_downloadStallTimeoutMilliseconds);
				QNetworkReply *reply = m_networkManager->get(request);
				if (!reply)
				{
					publishDownloadFinished(destination, false);
					return;
				}
				download->reply = reply;
				m_downloads.insert(downloadKey, download);
				connect(reply, &QIODevice::readyRead, this,
				        [download]
				        {
					        if (!download->reply || !download->output || download->writeFailed)
						        return;
					        const QByteArray bytes = download->reply->readAll();
					        if (!bytes.isEmpty() && download->output->write(bytes) != bytes.size())
					        {
						        download->writeFailed = true;
						        download->reply->abort();
					        }
				        });
				connect(reply, &QNetworkReply::finished, this,
				        [this, downloadKey] { finishDownload(downloadKey); });
			}

			/**
			 * @brief Changes the no-progress timeout for later downloads.
			 * @param milliseconds Positive timeout in milliseconds.
			 */
			void setDownloadStallTimeout(const int milliseconds)
			{
				Q_ASSERT(milliseconds > 0);
				m_downloadStallTimeoutMilliseconds = milliseconds;
			}

			/**
			 * @brief Processes one existing destination through the production download-completion entry path.
			 * @param destination Existing sound file beneath the configured sounds root.
			 */
			void processExistingDownloadForTest(const QString &destination)
			{
				startDownload(m_generation, destination, QUrl(QStringLiteral("https://example.invalid/")));
			}

			/**
			 * @brief Pauses or resumes catalog work for deterministic tests.
			 * @param paused Whether catalog events must remain suspended.
			 */
			void setCatalogPaused(const bool paused)
			{
				m_catalogPaused = paused;
				if (m_indexRead ||
				    (!paused && (m_activeScan || m_directoryPublication || !m_scanQueue.empty() ||
				                 m_indexWrite || m_indexDirty || !m_unreconciledIndexDirectories.isEmpty())))
					scheduleCatalogStep();
			}

			/**
			 * @brief Pauses or resumes persistent-index ingestion for deterministic tests.
			 * @param paused Whether index batches must remain suspended.
			 */
			void setIndexReadPaused(const bool paused)
			{
				m_indexReadPausedForTest = paused;
				if (!paused && m_indexRead)
					scheduleCatalogStep();
			}

			/**
			 * @brief Continues persistent-index loading after the playback worker applies one batch.
			 * @param generation Catalog generation owning the acknowledged batch.
			 */
			void acknowledgeIndexBatch(const quint64 generation)
			{
				if (!m_indexRead || generation != m_generation || !m_indexRead->batchPending)
					return;
				m_indexRead->batchPending = false;
				if (m_indexRead->remainingDirectories == 0 && m_indexRead->directory.isEmpty())
					finishPersistentIndexLoad(true);
				else
					scheduleCatalogStep();
			}

			/**
			 * @brief Continues directory publication after the playback worker applies one bounded batch.
			 * @param generation Catalog generation owning the acknowledged batch.
			 * @param directory Directory whose reconciliation batch was applied.
			 */
			void acknowledgeDirectoryBatch(const quint64 generation, const QString &directory)
			{
				if (!m_directoryPublication || generation != m_generation ||
				    !sameSoundFilesystemPath(m_directoryPublication->directory, directory) ||
				    !m_directoryPublication->batchPending)
				{
					return;
				}
				m_directoryPublication->batchPending = false;
				scheduleCatalogStep();
			}

			/**
			 * @brief Publishes a synthetic directory through the production bounded handoff for tests.
			 * @param fileCount Number of lexical catalog paths to publish.
			 * @return `true` when the quiescent catalog accepted the publication.
			 */
			[[nodiscard]] bool publishSyntheticDirectory(const qsizetype fileCount)
			{
				if (fileCount < 0 || m_soundsRoot.isEmpty() || m_indexRead || m_activeScan ||
				    m_directoryPublication || !m_scanQueue.empty() ||
				    !m_unreconciledIndexDirectories.isEmpty())
				{
					return false;
				}
				const QString directory = QMudPluginPathUtils::normalizeSeparators(
				    QDir(m_soundsRoot).filePath(QStringLiteral("synthetic-directory")));
				QStringList files;
				files.reserve(fileCount);
				for (qsizetype index = 0; index < fileCount; ++index)
				{
					files.push_back(QMudPluginPathUtils::normalizeSeparators(
					    QDir(directory).filePath(QStringLiteral("sound-%1.wav").arg(index))));
				}
				publishScanStarted();
				startDirectoryPublication(directory, std::move(files), {}, {}, {}, {}, true);
				return true;
			}

			/**
			 * @brief Attempts one watcher registration through the production path.
			 * @param directory Directory to watch.
			 * @return `true` when registration succeeds.
			 */
			[[nodiscard]] bool attemptWatch(const QString &directory)
			{
				return watchDirectory(directory);
			}

			/** @brief Reconciles the configured sounds root through its production transition. */
			void reconcileRoot()
			{
				handleWatchedDirectoryChanged(m_qmudHome);
			}

			/**
			 * @brief Reconciles one directory through the production watcher transition.
			 * @param directory Changed catalog directory.
			 */
			void reconcileDirectory(const QString &directory)
			{
				handleWatchedDirectoryChanged(directory);
			}

			/**
			 * @brief Starts tracking one immediate plugin resource directory after an explicit sound request.
			 * @param generation Configuration generation owning the request.
			 * @param directory Existing plugin directory containing the requested sound.
			 */
			void trackPluginDirectory(const quint64 generation, const QString &directory)
			{
				if (m_shuttingDown || generation != m_pluginGeneration || m_pluginsRoot.isEmpty())
					return;
				const QString   normalized = QMudPluginPathUtils::normalizeSeparators(directory);
				const QFileInfo info(normalized);
				const QString canonical = QMudPluginPathUtils::normalizeSeparators(info.canonicalFilePath());
				if (!info.isDir() || info.isSymLink() || canonical.isEmpty() ||
				    !QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(canonical, m_pluginsRoot) ||
				    soundPathContainsSymlink(m_pluginsRoot, canonical))
				{
					publishPluginDirectoryChange(normalized, {}, {}, {}, false, false);
					return;
				}
				m_pluginDirectories.tryEmplace(canonical);
				reconcilePluginDirectory(canonical);
			}

			/**
			 * @brief Creates, watches, and reconciles a directory explicitly selected in world preferences.
			 * @param directory Validated absolute directory beneath the configured sounds root.
			 */
			void reconcilePreferredDirectory(const QString &directory)
			{
				if (m_shuttingDown || m_soundsRoot.isEmpty())
					return;
				const QString normalized = QMudPluginPathUtils::normalizeSeparators(directory);
				if (!QMudPluginPathUtils::pathIsWithinOrEqualTo(normalized, m_soundsRoot) ||
				    !QDir().mkpath(normalized) || soundPathContainsSymlink(m_soundsRoot, normalized))
				{
					return;
				}
				const QString canonical =
				    QMudPluginPathUtils::normalizeSeparators(QFileInfo(normalized).canonicalFilePath());
				if (canonical.isEmpty() ||
				    !QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(canonical, m_soundsRoot))
				{
					return;
				}
				recordRuntimeDirectoryHierarchy(canonical);
				publishScanStarted();
				queueDirectory(canonical);
			}

			/** @brief Retries one failed watcher without creating another maintenance timer. */
			void retryFailedWatch()
			{
				if (m_failedWatches.isEmpty())
					return;
				const QString directory = *m_failedWatches.cbegin();
				if (m_pluginDirectories.contains(directory))
				{
					if (watchPluginDirectory(directory))
						reconcilePluginDirectory(directory);
					return;
				}
				if (!watchDirectory(directory))
					return;
				if (sameSoundFilesystemPath(directory, m_qmudHome))
					handleWatchedDirectoryChanged(directory);
				else
				{
					publishScanStarted();
					queueDirectory(directory);
				}
			}

			/**
			 * @brief Runs one bounded directory-enumeration step.
			 * @param event Posted catalog event.
			 * @return `true` when the event was consumed.
			 */
			bool event(QEvent *event) override
			{
				if (event && event->type() == catalogEventType())
				{
					m_catalogStepPending = false;
					if (!m_shuttingDown)
						processCatalogStep();
					return true;
				}
				return QObject::event(event);
			}

		private:
			using CatalogDirectories = QHash<QString, SoundCatalogDirectoryState>;

			/** @brief Incremental state for one directory scan. */
			struct DirectoryScan
			{
					/** @brief Incremental cursor for one removed directory subtree. */
					struct RemovedDirectory
					{
							QString                               path;
							QHash<QString, SoundCatalogFileState> files;
							QSet<QString>                         children;
					};

					QString                               canonicalPath;
					std::unique_ptr<QDirIterator>         iterator;
					QHash<QString, SoundCatalogFileState> previousFiles;
					QSet<QString>                         previousChildren;
					QHash<QString, SoundCatalogFileState> files;
					QSet<QString>                         children;
					QSet<QString>                         protectedDirectories;
					QStringList                           addedFiles;
					QStringList                           changedFiles;
					QStringList                           removedFiles;
					QStringList                           addedDirectories;
					QStringList                           removedDirectories;
					std::deque<QString>                   pendingRemovedDirectories;
					std::optional<RemovedDirectory>       removedDirectory;
					qsizetype                             previousFileCount{0};
					bool                                  directoryPresent{true};
					bool                                  catalogChanged{false};
					bool                                  watchStateChanged{false};
			};

			/** @brief Incremental reader for one validated persistent catalog index. */
			struct PersistentIndexRead
			{
					std::unique_ptr<QFile>       file;
					std::unique_ptr<QDataStream> stream;
					quint64                      remainingDirectories{0};
					quint64                      remainingFiles{0};
					QString                      directory;
					QString                      parentDirectory;
					QSet<QString>                seenDirectories;
					bool                         batchPending{false};
			};

			/** @brief Stable catalog snapshot being written through one atomic replacement. */
			struct PersistentIndexWrite
			{
					std::unique_ptr<QSaveFile>                            file;
					std::unique_ptr<QDataStream>                          stream;
					CatalogDirectories                                    snapshot;
					CatalogDirectories::const_iterator                    directory;
					QHash<QString, SoundCatalogFileState>::const_iterator entry;
					bool                                                  directoryHeaderWritten{false};
					quint64                                               revision{0};
			};

			/** @brief Bounded publication state for one completed directory reconciliation. */
			struct DirectoryPublication
			{
					QString     directory;
					QStringList files;
					QStringList changedFiles;
					QStringList removedFiles;
					QStringList directories;
					QStringList removedDirectories;
					qsizetype   fileOffset{0};
					qsizetype   changedFileOffset{0};
					qsizetype   removedFileOffset{0};
					qsizetype   directoryOffset{0};
					qsizetype   removedDirectoryOffset{0};
					bool        directoryWatched{false};
					bool        directoryPresent{false};
					bool        batchPending{false};
			};

			/** @brief Returns the custom event type used by I/O catalog work. */
			[[nodiscard]] static QEvent::Type catalogEventType()
			{
				static const auto type = static_cast<QEvent::Type>(QEvent::registerEventType());
				return type;
			}

			/** @brief Posts one coalesced low-priority catalog event. */
			void scheduleCatalogStep()
			{
				if (m_catalogStepPending || m_shuttingDown)
					return;
				m_catalogStepPending = true;
				QCoreApplication::postEvent(this, new QEvent(catalogEventType()), Qt::LowEventPriority);
			}

			/**
			 * @brief Queues one directory when it is not already pending.
			 * @param directory Canonical directory path.
			 */
			void queueDirectory(const QString &directory)
			{
				const QString normalized = QMudPluginPathUtils::normalizeSeparators(directory);
				if (normalized.isEmpty() || m_queuedDirectories.contains(normalized))
					return;
				m_queuedDirectories.insert(normalized);
				m_scanQueue.push_back(normalized);
				scheduleCatalogStep();
			}

			/**
			 * @brief Removes an exact path from the unsent portion of one publication list.
			 * @param paths Publication paths whose prefix may already have been sent.
			 * @param offset First unsent entry.
			 * @param path Exact case-insensitive sound path to remove.
			 */
			static void removePendingPublishedPath(QStringList &paths, const qsizetype offset,
			                                       const QString &path)
			{
				for (qsizetype index = offset; index < paths.size();)
				{
					if (sameSoundFilesystemPath(paths.at(index), path))
						paths.removeAt(index);
					else
						++index;
				}
			}

			/**
			 * @brief Installs one directory record and its direct parent relationship.
			 * @param directory Catalog directory beneath the configured sounds root.
			 * @param parent Direct parent directory, or an empty string for the sounds root.
			 * @param indexSnapshot Whether this record came from the persistent index.
			 */
			void recordDirectory(const QString &directory, const QString &parent, const bool indexSnapshot)
			{
				const QString child            = QMudPluginPathUtils::normalizeSeparators(directory);
				const QString normalizedParent = QMudPluginPathUtils::normalizeSeparators(parent);
				if (child.isEmpty() || !QMudPluginPathUtils::pathIsWithinOrEqualTo(child, m_soundsRoot) ||
				    (!sameSoundFilesystemPath(child, m_soundsRoot) &&
				     (normalizedParent.isEmpty() || sameSoundFilesystemPath(normalizedParent, child) ||
				      !QMudPluginPathUtils::pathIsWithinOrEqualTo(normalizedParent, m_soundsRoot))))
				{
					return;
				}

				SoundCatalogDirectoryState &state = m_directories[child];
				state.parent = sameSoundFilesystemPath(child, m_soundsRoot) ? QString() : normalizedParent;
				if (!state.parent.isEmpty())
					m_directories[state.parent].children.insert(child);
				if (indexSnapshot)
					m_unreconciledIndexDirectories.insert(child);
			}

			/**
			 * @brief Ensures runtime-created directory records exist through the sounds root.
			 * @param directory Catalog directory beneath the configured sounds root.
			 */
			void recordRuntimeDirectoryHierarchy(const QString &directory)
			{
				QString child = QMudPluginPathUtils::normalizeSeparators(directory);
				if (!QMudPluginPathUtils::pathIsWithinOrEqualTo(child, m_soundsRoot))
					return;
				if (m_directories.contains(child) ||
				    (m_activeScan && sameSoundFilesystemPath(child, m_activeScan->canonicalPath)))
				{
					return;
				}
				while (!sameSoundFilesystemPath(child, m_soundsRoot))
				{
					const QString parent =
					    QMudPluginPathUtils::normalizeSeparators(QFileInfo(child).absolutePath());
					if (parent.isEmpty() || sameSoundFilesystemPath(parent, child) ||
					    !QMudPluginPathUtils::pathIsWithinOrEqualTo(parent, m_soundsRoot))
					{
						return;
					}
					bool       inserted = false;
					const bool parentActive =
					    m_activeScan && sameSoundFilesystemPath(m_activeScan->canonicalPath, parent);
					bool parentKnown = parentActive;
					if (parentActive)
					{
						inserted = !m_activeScan->children.contains(child);
						m_activeScan->children.insert(child);
						m_directories[child].parent = parent;
						if (m_activeScan->previousChildren.remove(child) > 0)
							inserted = false;
						else if (inserted)
						{
							m_activeScan->addedDirectories.push_back(child);
							m_activeScan->catalogChanged = true;
						}
					}
					else
					{
						parentKnown                             = m_directories.contains(parent);
						SoundCatalogDirectoryState &parentState = m_directories[parent];
						inserted                                = !parentState.children.contains(child);
						parentState.children.insert(child);
						m_directories[child].parent = parent;
					}
					if (m_activeScan && QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(
					                        child, m_activeScan->canonicalPath))
					{
						m_activeScan->protectedDirectories.insert(child);
					}
					if (inserted || !m_watchedDirectories.contains(child))
						queueDirectory(child);
					if (parentKnown)
						return;
					child = parent;
				}
				recordDirectory(m_soundsRoot, {}, false);
			}

			/**
			 * @brief Merges one downloaded file into an overlapping directory scan snapshot.
			 * @param absolutePath Canonical downloaded file path.
			 * @param fileState Current downloaded-file metadata.
			 * @return `true` when the active scan owns the file's directory state.
			 */
			[[nodiscard]] bool recordActiveScanFile(const QString               &absolutePath,
			                                        const SoundCatalogFileState &fileState)
			{
				if (!m_activeScan)
					return false;
				const QString directory =
				    QMudPluginPathUtils::normalizeSeparators(QFileInfo(absolutePath).absolutePath());
				if (sameSoundFilesystemPath(directory, m_activeScan->canonicalPath))
				{
					const auto current      = m_activeScan->files.constFind(absolutePath);
					const bool currentFile  = current != m_activeScan->files.cend();
					const auto previous     = m_activeScan->previousFiles.find(absolutePath);
					const bool previousFile = previous != m_activeScan->previousFiles.end();
					const bool fileChanged  = (currentFile && current.value() != fileState) ||
					                          (previousFile && previous.value() != fileState);
					if (previousFile)
						m_activeScan->previousFiles.erase(previous);
					m_activeScan->files.insert(absolutePath, fileState);
					m_activeScan->removedFiles.removeAll(absolutePath);
					if (fileChanged && !m_activeScan->changedFiles.contains(absolutePath))
						m_activeScan->changedFiles.push_back(absolutePath);
					if (fileChanged || (!currentFile && !previousFile))
						m_activeScan->catalogChanged = true;
					return true;
				}
				if (!QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(directory,
				                                                         m_activeScan->canonicalPath))
				{
					return false;
				}
				const QString relative = QMudPluginPathUtils::normalizeSeparators(
				    QDir(m_activeScan->canonicalPath).relativeFilePath(directory));
				const qsizetype separator       = relative.indexOf(QLatin1Char('/'));
				const QString   directChildName = separator < 0 ? relative : relative.first(separator);
				const QString   directChild     = QMudPluginPathUtils::normalizeSeparators(
				    QFileInfo(QDir(m_activeScan->canonicalPath).filePath(directChildName))
				        .canonicalFilePath());
				if (!directChild.isEmpty() && QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(
				                                  directChild, m_activeScan->canonicalPath))
				{
					m_activeScan->children.insert(directChild);
					m_activeScan->previousChildren.remove(directChild);
					m_activeScan->removedDirectories.removeAll(directChild);
				}
				return false;
			}

			/**
			 * @brief Begins one queued directory scan.
			 * @param directory Directory to enumerate.
			 * @return `true` when enumeration started.
			 */
			[[nodiscard]] bool beginDirectoryScan(const QString &directory)
			{
				const QFileInfo info(directory);
				const QString canonical = QMudPluginPathUtils::normalizeSeparators(info.canonicalFilePath());
				if (!info.exists() || !info.isDir() || info.isSymLink() || canonical.isEmpty() ||
				    !QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(canonical, m_soundsRoot))
				{
					DirectoryScan scan;
					scan.canonicalPath    = QMudPluginPathUtils::normalizeSeparators(directory);
					scan.directoryPresent = false;
					scan.pendingRemovedDirectories.push_back(scan.canonicalPath);
					m_activeScan = std::move(scan);
					return false;
				}
				static_cast<void>(watchDirectory(canonical));
				m_unreconciledIndexDirectories.remove(canonical);
				DirectoryScan scan;
				scan.canonicalPath = canonical;
				if (auto record = m_directories.find(canonical); record != m_directories.end())
				{
					scan.previousFiles    = std::move(record->files);
					scan.previousChildren = std::move(record->children);
					m_directories.erase(record);
				}
				scan.previousFileCount = scan.previousFiles.size();
				if (m_soundNameFilters.isEmpty())
				{
					scan.iterator = std::make_unique<QDirIterator>(
					    canonical, QDir::Dirs | QDir::Readable | QDir::NoSymLinks | QDir::NoDotAndDotDot,
					    QDirIterator::NoIteratorFlags);
				}
				else
				{
					scan.iterator =
					    std::make_unique<QDirIterator>(canonical, m_soundNameFilters,
					                                   QDir::AllDirs | QDir::Files | QDir::Readable |
					                                       QDir::NoSymLinks | QDir::NoDotAndDotDot,
					                                   QDirIterator::NoIteratorFlags);
				}
				m_activeScan = std::move(scan);
				return true;
			}

			/** @brief Processes a bounded amount of directory enumeration. */
			void processCatalogStep()
			{
				if (m_indexRead)
				{
					if (m_indexReadPausedForTest)
						return;
					processPersistentIndexReadStep();
					return;
				}
				if (m_catalogPaused)
					return;
				if (m_directoryPublication)
				{
					processDirectoryPublicationStep();
					return;
				}
				if (!m_activeScan)
				{
					if (m_scanQueue.empty())
					{
						if (!m_unreconciledIndexDirectories.isEmpty())
						{
							const QString stale = *m_unreconciledIndexDirectories.cbegin();
							m_unreconciledIndexDirectories.remove(stale);
							if (m_directories.contains(stale))
							{
								static_cast<void>(beginDirectoryScan(stale));
								scheduleCatalogStep();
								return;
							}
							scheduleCatalogStep();
							return;
						}
						publishCatalogReady();
						if (m_indexWrite)
						{
							processPersistentIndexWriteStep();
							return;
						}
						if (m_indexDirty && !m_indexWriteFailed && beginPersistentIndexWrite())
						{
							scheduleCatalogStep();
							return;
						}
						return;
					}
					const QString directory = std::move(m_scanQueue.front());
					m_scanQueue.pop_front();
					if (m_queuedDirectories.remove(directory) == 0)
					{
						scheduleCatalogStep();
						return;
					}
					if (!beginDirectoryScan(directory))
					{
						scheduleCatalogStep();
						return;
					}
				}

				DirectoryScan &scan      = *m_activeScan;
				qsizetype      processed = 0;
				while (processed < kCatalogEntriesPerStep && scan.iterator && scan.iterator->hasNext())
				{
					const QFileInfo entry = scan.iterator->nextFileInfo();
					++processed;
					if (entry.isSymLink())
						continue;
					if (entry.isDir())
					{
						const QString child =
						    QMudPluginPathUtils::normalizeSeparators(entry.canonicalFilePath());
						if (!child.isEmpty() &&
						    QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(child, m_soundsRoot))
						{
							const bool added       = !scan.children.contains(child);
							const bool wasPrevious = scan.previousChildren.remove(child) > 0;
							scan.children.insert(child);
							if (!wasPrevious && added)
							{
								scan.addedDirectories.push_back(child);
								scan.catalogChanged = true;
							}
							if ((!wasPrevious && added) || m_unreconciledIndexDirectories.contains(child))
								queueDirectory(child);
						}
						continue;
					}
					if (fileIsIndexable(entry))
					{
						const QString file =
						    QMudPluginPathUtils::normalizeSeparators(entry.canonicalFilePath());
						const SoundCatalogFileState fileState{entry.size(),
						                                      entry.lastModified().toMSecsSinceEpoch()};
						const bool                  added = !scan.files.contains(file);
						scan.files.insert(file, fileState);
						if (const auto previous = scan.previousFiles.find(file);
						    previous != scan.previousFiles.end())
						{
							if (previous.value() != fileState)
								scan.changedFiles.push_back(file);
							scan.previousFiles.erase(previous);
						}
						else if (added)
							scan.addedFiles.push_back(file);
					}
				}
				if (scan.iterator && scan.iterator->hasNext())
				{
					m_maximumReconciliationStepSize = qMax(m_maximumReconciliationStepSize, processed);
					scheduleCatalogStep();
					return;
				}
				scan.iterator.reset();
				const auto takeOne = [](auto &values)
				{
					auto    it    = values.begin();
					QString value = it.key();
					values.erase(it);
					return value;
				};
				const auto takeOneSet = [](QSet<QString> &values)
				{
					auto    it    = values.begin();
					QString value = *it;
					values.erase(it);
					return value;
				};
				while (processed < kCatalogEntriesPerStep && !scan.previousFiles.isEmpty())
				{
					const QString removedFile = takeOne(scan.previousFiles);
					scan.removedFiles.push_back(removedFile);
					scan.catalogChanged = true;
					++processed;
				}
				while (processed < kCatalogEntriesPerStep && !scan.previousChildren.isEmpty())
				{
					scan.pendingRemovedDirectories.push_back(takeOneSet(scan.previousChildren));
					scan.catalogChanged = true;
					++processed;
				}
				while (processed < kCatalogEntriesPerStep &&
				       (scan.removedDirectory || !scan.pendingRemovedDirectories.empty()))
				{
					if (!scan.removedDirectory)
					{
						const QString removed = std::move(scan.pendingRemovedDirectories.front());
						scan.pendingRemovedDirectories.pop_front();
						DirectoryScan::RemovedDirectory state;
						state.path = removed;
						if (!scan.protectedDirectories.contains(removed))
						{
							const SoundCatalogDirectoryState record = m_directories.take(removed);
							state.files                             = record.files;
							state.children                          = record.children;
							m_catalogFileCount -= state.files.size();
							scan.catalogChanged = scan.catalogChanged || !state.files.isEmpty();
							m_unreconciledIndexDirectories.remove(removed);
							m_queuedDirectories.remove(removed);
							if (m_watchedDirectories.remove(removed) > 0)
							{
								if (m_fileWatcher)
									m_fileWatcher->removePath(removed);
								scan.watchStateChanged = true;
							}
							if (m_failedWatches.remove(removed) > 0)
								scan.watchStateChanged = true;
						}
						scan.removedDirectory = std::move(state);
						++processed;
						continue;
					}
					DirectoryScan::RemovedDirectory &removed = *scan.removedDirectory;
					if (!removed.files.isEmpty())
					{
						const QString removedFile = takeOne(removed.files);
						scan.removedFiles.push_back(removedFile);
						++processed;
						continue;
					}
					if (!removed.children.isEmpty())
					{
						scan.pendingRemovedDirectories.push_back(takeOneSet(removed.children));
						++processed;
						continue;
					}
					if (!scan.protectedDirectories.contains(removed.path))
						scan.removedDirectories.push_back(removed.path);
					scan.removedDirectory.reset();
					++processed;
				}
				m_maximumReconciliationStepSize = qMax(m_maximumReconciliationStepSize, processed);
				if (!scan.previousFiles.isEmpty() || !scan.previousChildren.isEmpty() ||
				    scan.removedDirectory || !scan.pendingRemovedDirectories.empty())
				{
					scheduleCatalogStep();
					return;
				}

				DirectoryScan completed = std::move(scan);
				m_activeScan.reset();
				if (completed.directoryPresent)
				{
					m_catalogFileCount += completed.files.size() - completed.previousFileCount;
					SoundCatalogDirectoryState &record = m_directories[completed.canonicalPath];
					record.parent   = sameSoundFilesystemPath(completed.canonicalPath, m_soundsRoot)
					                      ? QString()
					                      : QMudPluginPathUtils::normalizeSeparators(
					                            QFileInfo(completed.canonicalPath).absolutePath());
					record.files    = std::move(completed.files);
					record.children = std::move(completed.children);
				}
				else if (!completed.protectedDirectories.contains(completed.canonicalPath))
				{
					const QString parent = QMudPluginPathUtils::normalizeSeparators(
					    QFileInfo(completed.canonicalPath).absolutePath());
					if (auto parentRecord = m_directories.find(parent); parentRecord != m_directories.end())
					{
						parentRecord->children.remove(completed.canonicalPath);
					}
				}
				if (completed.catalogChanged || !completed.addedFiles.isEmpty() ||
				    !completed.changedFiles.isEmpty())
					markCatalogChanged();
				if (completed.watchStateChanged)
					publishWatchState();
				startDirectoryPublication(
				    completed.canonicalPath, std::move(completed.addedFiles),
				    std::move(completed.changedFiles), std::move(completed.removedFiles),
				    std::move(completed.addedDirectories), std::move(completed.removedDirectories),
				    completed.directoryPresent);
				scheduleCatalogStep();
			}

			/**
			 * @brief Returns whether one filesystem entry is safe and playable.
			 * @param info File metadata.
			 * @return `true` for a supported regular non-symlink file.
			 */
			[[nodiscard]] bool fileIsIndexable(const QFileInfo &info) const
			{
				if (!info.exists() || !info.isFile() || info.isSymLink() || !info.isReadable() ||
				    !soundPlaybackSupportsFile(info.fileName()))
				{
					return false;
				}
				const QString canonical = QMudPluginPathUtils::normalizeSeparators(info.canonicalFilePath());
				return !canonical.isEmpty() &&
				       QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(canonical, m_soundsRoot);
			}

			/**
			 * @brief Publishes changes from one directly tracked plugin resource directory.
			 * @param directory Canonical immediate directory.
			 * @param added Newly present supported audio files.
			 * @param changed Supported audio files whose metadata changed.
			 * @param removed Supported audio files no longer present.
			 * @param watched Whether the directory watcher is active.
			 * @param valid Whether the immediate directory still exists within the plugin root.
			 */
			void publishPluginDirectoryChange(const QString &directory, QStringList added,
			                                  QStringList changed, QStringList removed, const bool watched,
			                                  const bool valid) const
			{
				SoundCommand result;
				result.kind             = SoundCommandKind::PluginDirectoryChanged;
				result.token            = m_pluginGeneration;
				result.text             = directory;
				result.files            = std::move(added);
				result.changedFiles     = std::move(changed);
				result.removedFiles     = std::move(removed);
				result.directoryWatched = watched;
				result.success          = valid;
				m_publish(std::move(result));
			}

			/**
			 * @brief Registers one explicitly used plugin resource directory without watching its hierarchy.
			 * @param directory Existing canonical immediate directory beneath the plugin root.
			 * @return `true` when the direct directory is watched.
			 */
			[[nodiscard]] bool watchPluginDirectory(const QString &directory)
			{
				const QString normalized = QMudPluginPathUtils::normalizeSeparators(directory);
				if (!m_fileWatcher || normalized.isEmpty() || m_pluginsRoot.isEmpty() ||
				    !m_pluginDirectories.contains(normalized) ||
				    !QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(normalized, m_pluginsRoot) ||
				    soundPathContainsSymlink(m_pluginsRoot, normalized))
				{
					if (m_failedWatches.remove(normalized) > 0)
						publishWatchState();
					return false;
				}
				if (m_watchedDirectories.contains(normalized))
					return true;
				if (!m_fileWatcher->addPath(normalized))
				{
					m_failedWatches.insert(normalized);
					publishWatchState();
					return false;
				}
				m_failedWatches.remove(normalized);
				m_watchedDirectories.insert(normalized);
				publishWatchState();
				return true;
			}

			/**
			 * @brief Reconciles supported direct audio files in one explicitly used plugin directory.
			 * @param directory Canonical immediate directory beneath the plugin root.
			 */
			void reconcilePluginDirectory(const QString &directory)
			{
				auto state = m_pluginDirectories.find(directory);
				if (state == m_pluginDirectories.end())
					return;

				QStringList                                           added;
				QStringList                                           changed;
				QStringList                                           removed;
				QHash<QString, QPair<QString, SoundCatalogFileState>> currentFiles;
				const QFileInfo                                       directoryInfo(directory);
				const QString                                         currentDirectory =
				    QMudPluginPathUtils::normalizeSeparators(directoryInfo.canonicalFilePath());
				const bool directoryValid =
				    directoryInfo.isDir() && !directoryInfo.isSymLink() && !currentDirectory.isEmpty() &&
				    sameSoundFilesystemPath(currentDirectory, directory) &&
				    QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(currentDirectory, m_pluginsRoot) &&
				    !soundPathContainsSymlink(m_pluginsRoot, currentDirectory);
				if (directoryValid)
				{
					const QFileInfoList entries =
					    m_soundNameFilters.isEmpty()
					        ? QFileInfoList()
					        : QDir(currentDirectory)
					              .entryInfoList(m_soundNameFilters,
					                             QDir::Files | QDir::Readable | QDir::Hidden | QDir::System |
					                                 QDir::NoSymLinks,
					                             QDir::Name | QDir::IgnoreCase);
					for (const QFileInfo &entry : entries)
					{
						const QString canonical =
						    QMudPluginPathUtils::normalizeSeparators(entry.canonicalFilePath());
						if (canonical.isEmpty() || entry.isSymLink() ||
						    !sameSoundFilesystemPath(
						        QMudPluginPathUtils::normalizeSeparators(QFileInfo(canonical).absolutePath()),
						        currentDirectory) ||
						    !QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(canonical, m_pluginsRoot))
						{
							continue;
						}
						const QString               key = soundCatalogComparisonKey(canonical);
						const SoundCatalogFileState metadata{entry.size(),
						                                     entry.lastModified().toMSecsSinceEpoch()};
						currentFiles.insert(key, qMakePair(canonical, metadata));
						const auto previous = state->files.constFind(key);
						if (previous == state->files.cend())
							added.push_back(canonical);
						else if (previous->first != canonical)
						{
							removed.push_back(previous->first);
							added.push_back(canonical);
						}
						else if (previous->second != metadata)
							changed.push_back(canonical);
					}
				}

				for (auto previous = state->files.cbegin(); previous != state->files.cend(); ++previous)
				{
					if (!currentFiles.contains(previous.key()))
						removed.push_back(previous->first);
				}
				state->files       = std::move(currentFiles);
				const bool watched = directoryValid && watchPluginDirectory(currentDirectory);
				if (!watched)
				{
					const bool watchRemoved = m_watchedDirectories.remove(directory) > 0;
					if (m_fileWatcher)
						m_fileWatcher->removePath(directory);
					if (watchRemoved)
						publishWatchState();
				}
				publishPluginDirectoryChange(directory, std::move(added), std::move(changed),
				                             std::move(removed), watched, directoryValid);
				if (!directoryValid)
					m_pluginDirectories.erase(state);
			}

			/**
			 * @brief Registers a directory watcher.
			 * @param directory Existing directory.
			 * @return `true` when watched.
			 */
			[[nodiscard]] bool watchDirectory(const QString &directory)
			{
				const QString normalized = QMudPluginPathUtils::normalizeSeparators(directory);
				if (!m_fileWatcher || normalized.isEmpty())
					return false;
				const bool qmudHome = sameSoundFilesystemPath(normalized, m_qmudHome);
				if (!qmudHome && (m_soundsRoot.isEmpty() ||
				                  !QMudPluginPathUtils::pathIsWithinOrEqualTo(normalized, m_soundsRoot) ||
				                  soundPathContainsSymlink(m_soundsRoot, normalized)))
				{
					if (m_failedWatches.remove(normalized) > 0)
						publishWatchState();
					return false;
				}
				if (m_watchedDirectories.contains(normalized))
					return true;
				if (!m_fileWatcher->addPath(normalized))
				{
					m_failedWatches.insert(normalized);
					publishWatchState();
					return false;
				}
				m_failedWatches.remove(normalized);
				m_watchedDirectories.insert(normalized);
				publishWatchState();
				return true;
			}

			/**
			 * @brief Reconciles one changed watched directory.
			 * @param directory Changed directory.
			 */
			void handleWatchedDirectoryChanged(const QString &directory)
			{
				if (m_shuttingDown || m_qmudHome.isEmpty())
					return;
				const QString normalized = QMudPluginPathUtils::normalizeSeparators(directory);
				if (m_pluginDirectories.contains(normalized))
				{
					reconcilePluginDirectory(normalized);
					return;
				}
				if (sameSoundFilesystemPath(normalized, m_qmudHome))
				{
					const QString currentRoot =
					    QMudPluginPathUtils::normalizeSeparators(QFileInfo(m_soundsPath).canonicalFilePath());
					if (currentRoot.isEmpty() || soundPathContainsSymlink(m_qmudHome, m_soundsPath) ||
					    !QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(currentRoot, m_qmudHome))
					{
						clearSoundTreeState();
						m_soundsRoot.clear();
						publishCatalogCleared(true);
						return;
					}
					if (!sameSoundFilesystemPath(currentRoot, m_soundsRoot))
					{
						publishScanStarted();
						clearSoundTreeState();
						m_soundsRoot = currentRoot;
						static_cast<void>(watchDirectory(m_soundsRoot));
						queueDirectory(m_soundsRoot);
					}
					return;
				}
				if (m_soundsRoot.isEmpty() ||
				    !QMudPluginPathUtils::pathIsWithinOrEqualTo(normalized, m_soundsRoot))
				{
					return;
				}
				publishScanStarted();
				queueDirectory(normalized);
			}

			/** @brief Discards the old sounds-tree state after the configured root itself changes. */
			void clearSoundTreeState()
			{
				QCoreApplication::removePostedEvents(this, catalogEventType());
				cancelPersistentIndexWrite();
				m_indexRead.reset();
				if (m_fileWatcher)
				{
					QStringList watched = m_fileWatcher->directories();
					for (qsizetype index = watched.size(); index > 0; --index)
					{
						if (sameSoundFilesystemPath(watched.at(index - 1), m_qmudHome) ||
						    m_pluginDirectories.contains(watched.at(index - 1)))
							watched.removeAt(index - 1);
					}
					if (!watched.isEmpty())
						m_fileWatcher->removePaths(watched);
				}
				m_directories.clear();
				m_catalogFileCount = 0;
				m_scanQueue.clear();
				m_queuedDirectories.clear();
				m_activeScan.reset();
				m_directoryPublication.reset();
				m_unreconciledIndexDirectories.clear();
				for (auto watched = m_watchedDirectories.begin(); watched != m_watchedDirectories.end();)
				{
					if (sameSoundFilesystemPath(*watched, m_qmudHome) ||
					    m_pluginDirectories.contains(*watched))
						++watched;
					else
						watched = m_watchedDirectories.erase(watched);
				}
				for (auto failed = m_failedWatches.begin(); failed != m_failedWatches.end();)
				{
					if (m_pluginDirectories.contains(*failed))
						++failed;
					else
						failed = m_failedWatches.erase(failed);
				}
				m_catalogStepPending    = false;
				m_catalogReadyPublished = false;
				m_indexDirty            = true;
				m_indexWriteFailed      = false;
				++m_catalogRevision;
				publishWatchState();
			}

			/** @brief Removes plugin directory state and watches while retaining sounds-tree ownership. */
			void clearPluginDirectoryState()
			{
				bool watchStateChanged{false};
				for (auto directory = m_pluginDirectories.cbegin(); directory != m_pluginDirectories.cend();
				     ++directory)
				{
					const bool catalogOwnsWatch =
					    sameSoundFilesystemPath(directory.key(), m_qmudHome) ||
					    (!m_soundsRoot.isEmpty() &&
					     QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(directory.key(), m_soundsRoot));
					if (!catalogOwnsWatch)
					{
						if (m_fileWatcher)
							m_fileWatcher->removePath(directory.key());
						watchStateChanged =
						    m_watchedDirectories.remove(directory.key()) > 0 || watchStateChanged;
						watchStateChanged = m_failedWatches.remove(directory.key()) > 0 || watchStateChanged;
					}
				}
				m_pluginDirectories.clear();
				m_pluginsRoot.clear();
				if (watchStateChanged)
					publishWatchState();
			}

			/** @brief Loads the persistent-index header and schedules bounded entry ingestion. */
			void loadPersistentIndex()
			{
				QString indexPath;
				if (!resolvePersistentIndexPath(&indexPath))
				{
					finishPersistentIndexLoad(false);
					return;
				}

				auto file = std::make_unique<QFile>(indexPath);
				if (!file->open(QIODevice::ReadOnly))
				{
					finishPersistentIndexLoad(false);
					return;
				}
				auto stream = std::make_unique<QDataStream>(file.get());
				stream->setVersion(QDataStream::Qt_6_0);
				quint32 magic          = 0;
				quint32 version        = 0;
				quint64 directoryCount = 0;
				*stream >> magic >> version >> directoryCount;
				if (stream->status() != QDataStream::Ok || magic != kSoundCatalogIndexMagic ||
				    version != kSoundCatalogIndexVersion)
				{
					finishPersistentIndexLoad(false);
					return;
				}

				m_indexRead.emplace();
				m_indexRead->file                 = std::move(file);
				m_indexRead->stream               = std::move(stream);
				m_indexRead->remainingDirectories = directoryCount;
				m_indexDirty                      = false;
				m_indexWriteFailed                = false;
				if (directoryCount == 0)
					finishPersistentIndexLoad(true);
				else
					scheduleCatalogStep();
			}

			/** @brief Reads and publishes at most one bounded persistent-index batch. */
			void processPersistentIndexReadStep()
			{
				if (!m_indexRead || m_indexRead->batchPending)
					return;
				PersistentIndexRead &read = *m_indexRead;
				SoundCommand         batch;
				batch.kind          = SoundCommandKind::CatalogIndexBatch;
				batch.token         = m_generation;
				qsizetype processed = 0;
				if (read.directory.isEmpty())
				{
					if (read.remainingDirectories == 0)
					{
						finishPersistentIndexLoad(true);
						return;
					}
					QString relativeDirectory;
					QString relativeParent;
					quint64 fileCount = 0;
					*read.stream >> relativeDirectory >> relativeParent >> fileCount;
					if (read.stream->status() != QDataStream::Ok)
					{
						finishPersistentIndexLoad(false);
						return;
					}
					--read.remainingDirectories;
					relativeDirectory =
					    QDir::cleanPath(QMudPluginPathUtils::normalizeSeparators(relativeDirectory));
					relativeParent = QMudPluginPathUtils::normalizeSeparators(relativeParent);
					if (relativeDirectory.isEmpty() || relativeDirectory == QLatin1String("..") ||
					    relativeDirectory.startsWith(QStringLiteral("../")) ||
					    QFileInfo(relativeDirectory).isAbsolute())
					{
						finishPersistentIndexLoad(false);
						return;
					}
					const QString directory = relativeDirectory == QLatin1String(".")
					                              ? m_soundsRoot
					                              : QMudPluginPathUtils::normalizeSeparators(
					                                    QDir(m_soundsRoot).filePath(relativeDirectory));
					QString       parent;
					if (!sameSoundFilesystemPath(directory, m_soundsRoot))
					{
						relativeParent = QDir::cleanPath(relativeParent);
						if (relativeParent.isEmpty() || relativeParent == QLatin1String("..") ||
						    relativeParent.startsWith(QStringLiteral("../")) ||
						    QFileInfo(relativeParent).isAbsolute())
						{
							finishPersistentIndexLoad(false);
							return;
						}
						parent = relativeParent == QLatin1String(".")
						             ? m_soundsRoot
						             : QMudPluginPathUtils::normalizeSeparators(
						                   QDir(m_soundsRoot).filePath(relativeParent));
						const QString expectedParent =
						    QMudPluginPathUtils::normalizeSeparators(QFileInfo(directory).absolutePath());
						if (!sameSoundFilesystemPath(parent, expectedParent))
						{
							finishPersistentIndexLoad(false);
							return;
						}
					}
					else if (!relativeParent.isEmpty())
					{
						finishPersistentIndexLoad(false);
						return;
					}
					const QString directoryKey = soundCatalogComparisonKey(directory);
					if (!QMudPluginPathUtils::pathIsWithinOrEqualTo(directory, m_soundsRoot) ||
					    read.seenDirectories.contains(directoryKey))
					{
						finishPersistentIndexLoad(false);
						return;
					}
					read.seenDirectories.insert(directoryKey);
					read.directory       = directory;
					read.parentDirectory = parent;
					read.remainingFiles  = fileCount;
					recordDirectory(directory, parent, true);
					++processed;
				}

				batch.text            = read.directory;
				batch.parentDirectory = read.parentDirectory;
				while (processed < kCatalogEntriesPerStep && read.remainingFiles > 0)
				{
					QString fileName;
					qint64  size          = 0;
					qint64  modifiedMsecs = 0;
					*read.stream >> fileName >> size >> modifiedMsecs;
					if (read.stream->status() != QDataStream::Ok)
					{
						finishPersistentIndexLoad(false);
						return;
					}
					--read.remainingFiles;
					++processed;
					fileName = QMudPluginPathUtils::normalizeSeparators(fileName);
					if (fileName.isEmpty() || fileName == QLatin1String(".") ||
					    fileName == QLatin1String("..") || fileName.contains(QLatin1Char('/')) ||
					    QFileInfo(fileName).isAbsolute() || !soundPlaybackSupportsFile(fileName) ||
					    size < 0 || modifiedMsecs < 0)
					{
						finishPersistentIndexLoad(false);
						return;
					}
					const QString absolute =
					    QMudPluginPathUtils::normalizeSeparators(QDir(read.directory).filePath(fileName));
					if (!QMudPluginPathUtils::pathIsWithinOrEqualTo(absolute, read.directory))
					{
						finishPersistentIndexLoad(false);
						return;
					}
					SoundCatalogDirectoryState &record = m_directories[read.directory];
					if (record.files.contains(absolute))
					{
						finishPersistentIndexLoad(false);
						return;
					}
					record.files.insert(absolute, {size, modifiedMsecs});
					++m_catalogFileCount;
					batch.files.push_back(absolute);
				}
				if (read.remainingFiles == 0)
				{
					read.directory.clear();
					read.parentDirectory.clear();
				}
				read.batchPending = true;
				m_publish(std::move(batch));
			}

			/**
			 * @brief Completes persistent-index loading and publishes whether the snapshot was valid.
			 * @param success Whether the entire declared index was read successfully.
			 */
			void finishPersistentIndexLoad(const bool success)
			{
				m_indexRead.reset();
				if (!success)
				{
					m_directories.clear();
					m_catalogFileCount = 0;
					m_unreconciledIndexDirectories.clear();
					m_indexDirty       = true;
					m_indexWriteFailed = false;
				}
				SoundCommand loaded;
				loaded.kind    = SoundCommandKind::CatalogIndexLoaded;
				loaded.token   = m_generation;
				loaded.success = success;
				m_publish(std::move(loaded));
				scheduleCatalogStep();
			}

			/**
			 * @brief Resolves the persistent-index path through the repository path authority.
			 * @param resolvedPath Destination receiving the contained absolute path.
			 * @return `true` when the index path remains inside QMud home.
			 */
			[[nodiscard]] bool resolvePersistentIndexPath(QString *resolvedPath) const
			{
				return QMudPluginPathUtils::resolveInsideQmudHome(
				    m_qmudHome, QString::fromLatin1(kSoundCatalogIndexPath), resolvedPath);
			}

			/** @brief Begins one atomic bounded write of the current reconciled catalog. */
			[[nodiscard]] bool beginPersistentIndexWrite()
			{
				if (m_soundsRoot.isEmpty())
					return false;
				QString indexPath;
				if (!resolvePersistentIndexPath(&indexPath) ||
				    !QDir().mkpath(QFileInfo(indexPath).absolutePath()) ||
				    !resolvePersistentIndexPath(&indexPath))
				{
					m_indexWriteFailed = true;
					return false;
				}

				m_indexWrite.emplace();
				PersistentIndexWrite &write = *m_indexWrite;
				write.file                  = std::make_unique<QSaveFile>(indexPath);
				if (!write.file->open(QIODevice::WriteOnly))
				{
					m_indexWrite.reset();
					m_indexWriteFailed = true;
					return false;
				}
				write.stream = std::make_unique<QDataStream>(write.file.get());
				write.stream->setVersion(QDataStream::Qt_6_0);
				write.snapshot            = m_directories;
				const auto directoryCount = static_cast<quint64>(write.snapshot.size());
				*write.stream << kSoundCatalogIndexMagic << kSoundCatalogIndexVersion << directoryCount;
				if (write.stream->status() != QDataStream::Ok)
				{
					write.file->cancelWriting();
					m_indexWrite.reset();
					m_indexWriteFailed = true;
					return false;
				}
				write.directory = write.snapshot.cbegin();
				write.revision  = m_catalogRevision;
				return true;
			}

			/**
			 * @brief Writes at most one bounded persistent-index batch.
			 * @param scheduleNext Whether unfinished work should post another low-priority event.
			 */
			void processPersistentIndexWriteStep(const bool scheduleNext = true)
			{
				if (!m_indexWrite)
					return;
				PersistentIndexWrite &write     = *m_indexWrite;
				qsizetype             processed = 0;
				while (processed < kCatalogEntriesPerStep && write.directory != write.snapshot.cend())
				{
					if (!write.directoryHeaderWritten)
					{
						const QString relativeDirectory =
						    sameSoundFilesystemPath(write.directory.key(), m_soundsRoot)
						        ? QStringLiteral(".")
						        : QMudPluginPathUtils::normalizeSeparators(
						              QDir(m_soundsRoot).relativeFilePath(write.directory.key()));
						const QString relativeParent =
						    write.directory->parent.isEmpty()
						        ? QString()
						        : (sameSoundFilesystemPath(write.directory->parent, m_soundsRoot)
						               ? QStringLiteral(".")
						               : QMudPluginPathUtils::normalizeSeparators(
						                     QDir(m_soundsRoot).relativeFilePath(write.directory->parent)));
						*write.stream << relativeDirectory << relativeParent
						              << static_cast<quint64>(write.directory->files.size());
						write.entry                  = write.directory->files.cbegin();
						write.directoryHeaderWritten = true;
						++processed;
						continue;
					}
					if (write.entry == write.directory->files.cend())
					{
						++write.directory;
						write.directoryHeaderWritten = false;
						continue;
					}
					*write.stream << QFileInfo(write.entry.key()).fileName() << write.entry->size
					              << write.entry->modifiedMsecs;
					++write.entry;
					++processed;
				}
				if (write.directory != write.snapshot.cend() && write.stream->status() == QDataStream::Ok)
				{
					if (scheduleNext)
						scheduleCatalogStep();
					return;
				}

				const quint64 revision = write.revision;
				const bool    streamOk = write.stream->status() == QDataStream::Ok;
				write.stream.reset();
				const bool committed = streamOk && write.file->commit();
				if (!committed)
					write.file->cancelWriting();
				m_indexWrite.reset();
				if (committed && revision == m_catalogRevision)
					m_indexDirty = false;
				else if (!committed)
					m_indexWriteFailed = true;
			}

			/** @brief Cancels an uncommitted index replacement without touching the previous index. */
			void cancelPersistentIndexWrite()
			{
				if (m_indexWrite && m_indexWrite->file)
					m_indexWrite->file->cancelWriting();
				m_indexWrite.reset();
			}

			/** @brief Records one real catalog-content change and invalidates an obsolete write snapshot. */
			void markCatalogChanged()
			{
				cancelPersistentIndexWrite();
				m_catalogReadyPublished = false;
				m_indexDirty            = true;
				m_indexWriteFailed      = false;
				++m_catalogRevision;
				scheduleCatalogStep();
			}

			/** @brief Finishes a valid quiescent index replacement before orderly worker shutdown. */
			void finishPersistentIndexWriteForShutdown()
			{
				if (m_indexRead || m_activeScan || m_directoryPublication || !m_scanQueue.empty() ||
				    !m_unreconciledIndexDirectories.isEmpty())
					return;
				if (!m_indexWrite && m_indexDirty && !m_indexWriteFailed)
					static_cast<void>(beginPersistentIndexWrite());
				while (m_indexWrite)
					processPersistentIndexWriteStep(false);
			}

			/**
			 * @brief Starts bounded publication of one completed directory reconciliation.
			 * @param directory Canonical directory.
			 * @param files Newly added supported files.
			 * @param changedFiles Supported files whose contents changed.
			 * @param removedFiles Files removed from this directory or a removed child subtree.
			 * @param directories Newly discovered direct child directories.
			 * @param removedDirectories Removed directories in child subtrees.
			 * @param directoryPresent Whether the scanned directory still exists.
			 */
			void startDirectoryPublication(const QString &directory, QStringList files,
			                               QStringList changedFiles, QStringList removedFiles,
			                               QStringList directories, QStringList removedDirectories,
			                               const bool directoryPresent)
			{
				Q_ASSERT(!m_directoryPublication);
				m_directoryPublication.emplace();
				DirectoryPublication &publication = *m_directoryPublication;
				publication.directory             = directory;
				publication.files                 = std::move(files);
				publication.changedFiles          = std::move(changedFiles);
				publication.removedFiles          = std::move(removedFiles);
				publication.directories           = std::move(directories);
				publication.removedDirectories    = std::move(removedDirectories);
				publication.directoryWatched      = m_watchedDirectories.contains(directory);
				publication.directoryPresent      = directoryPresent;
				scheduleCatalogStep();
			}

			/** @brief Publishes at most one acknowledged directory reconciliation batch. */
			void processDirectoryPublicationStep()
			{
				if (!m_directoryPublication || m_directoryPublication->batchPending)
					return;
				DirectoryPublication &publication = *m_directoryPublication;

				SoundCommand          batch;
				batch.kind            = SoundCommandKind::CatalogDirectoryBatch;
				batch.token           = m_generation;
				batch.text            = publication.directory;
				batch.parentDirectory = sameSoundFilesystemPath(publication.directory, m_soundsRoot)
				                            ? QString()
				                            : QMudPluginPathUtils::normalizeSeparators(
				                                  QFileInfo(publication.directory).absolutePath());
				qsizetype remaining   = kCatalogEntriesPerStep;
				auto      appendBatch =
				    [&remaining](const QStringList &source, qsizetype &offset, QStringList &destination)
				{
					while (remaining > 0 && offset < source.size())
					{
						destination.push_back(source.at(offset++));
						--remaining;
					}
				};
				appendBatch(publication.removedFiles, publication.removedFileOffset, batch.removedFiles);
				appendBatch(publication.changedFiles, publication.changedFileOffset, batch.changedFiles);
				appendBatch(publication.removedDirectories, publication.removedDirectoryOffset,
				            batch.removedDirectories);
				appendBatch(publication.files, publication.fileOffset, batch.files);
				appendBatch(publication.directories, publication.directoryOffset, batch.directories);
				if (remaining != kCatalogEntriesPerStep)
				{
					publication.batchPending = true;
					m_publish(std::move(batch));
					return;
				}

				SoundCommand completed;
				completed.kind             = SoundCommandKind::CatalogDirectoryScanned;
				completed.token            = m_generation;
				completed.text             = publication.directory;
				completed.directoryWatched = publication.directoryWatched;
				completed.success          = publication.directoryPresent;
				m_directoryPublication.reset();
				m_publish(std::move(completed));
				scheduleCatalogStep();
			}

			/** @brief Publishes that the current catalog is being reconciled. */
			void publishScanStarted()
			{
				cancelPersistentIndexWrite();
				m_catalogReadyPublished = false;
				SoundCommand result;
				result.kind  = SoundCommandKind::CatalogScanStarted;
				result.token = m_generation;
				m_publish(std::move(result));
			}

			/** @brief Publishes one completed reconciliation exactly once. */
			void publishCatalogReady()
			{
				if (m_catalogReadyPublished)
					return;
				m_catalogReadyPublished = true;
				SoundCommand ready;
				ready.kind                          = SoundCommandKind::CatalogReady;
				ready.token                         = m_generation;
				ready.catalogRevision               = m_catalogRevision;
				ready.maximumReconciliationStepSize = m_maximumReconciliationStepSize;
				m_publish(std::move(ready));
			}

			/**
			 * @brief Publishes a complete catalog reset.
			 * @param ready Whether the empty catalog is definitive.
			 */
			void publishCatalogCleared(const bool ready) const
			{
				SoundCommand result;
				result.kind    = SoundCommandKind::CatalogCleared;
				result.token   = m_generation;
				result.success = ready;
				m_publish(std::move(result));
			}

			/** @brief Publishes scalar watcher accounting for coherent test observation. */
			void publishWatchState() const
			{
				SoundCommand result;
				result.kind                  = SoundCommandKind::CatalogWatchState;
				result.token                 = m_generation;
				result.watchedDirectoryCount = static_cast<int>(m_watchedDirectories.size());
				result.failedWatchCount      = static_cast<int>(m_failedWatches.size());
				m_publish(std::move(result));
			}

			/**
			 * @brief Publishes a download result.
			 * @param destination Final requested path.
			 * @param success Whether a playable file is installed there.
			 */
			void publishDownloadFinished(const QString &destination, const bool success) const
			{
				SoundCommand result;
				result.kind    = SoundCommandKind::DownloadFinished;
				result.token   = m_generation;
				result.text    = destination;
				result.success = success;
				m_publish(std::move(result));
			}

			/**
			 * @brief Adds one already-present playable file to both catalog workers before resolving waiters.
			 * @param info Existing candidate beneath the configured sounds root.
			 * @return `true` when the validated file was published to playback lookup.
			 */
			[[nodiscard]] bool publishAvailableCatalogFile(const QFileInfo &info)
			{
				if (!fileIsIndexable(info))
					return false;
				const QString canonical = QMudPluginPathUtils::normalizeSeparators(info.canonicalFilePath());
				if (canonical.isEmpty())
					return false;

				if (m_activeScan)
				{
					m_activeScan->removedFiles.removeAll(canonical);
					if (m_activeScan->removedDirectory)
						m_activeScan->removedDirectory->files.remove(canonical);
					for (qsizetype index = 0; index < m_activeScan->removedDirectories.size();)
					{
						if (QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(
						        canonical, m_activeScan->removedDirectories.at(index)))
						{
							m_activeScan->removedDirectories.removeAt(index);
						}
						else
							++index;
					}
				}
				if (m_directoryPublication)
				{
					QStringList &pendingRemovals = m_directoryPublication->removedFiles;
					for (qsizetype index = m_directoryPublication->removedFileOffset;
					     index < pendingRemovals.size();)
					{
						if (sameSoundFilesystemPath(pendingRemovals.at(index), canonical))
							pendingRemovals.removeAt(index);
						else
							++index;
					}
					QStringList &pendingRemovedDirectories = m_directoryPublication->removedDirectories;
					for (qsizetype index = m_directoryPublication->removedDirectoryOffset;
					     index < pendingRemovedDirectories.size();)
					{
						if (QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(
						        canonical, pendingRemovedDirectories.at(index)))
						{
							pendingRemovedDirectories.removeAt(index);
						}
						else
							++index;
					}
				}

				const QString directory = QMudPluginPathUtils::normalizeSeparators(info.canonicalPath());
				const SoundCatalogFileState fileState{info.size(), info.lastModified().toMSecsSinceEpoch()};
				std::optional<SoundCatalogFileState> previousState;
				if (m_activeScan && sameSoundFilesystemPath(m_activeScan->canonicalPath, directory))
				{
					if (const auto current = m_activeScan->files.constFind(canonical);
					    current != m_activeScan->files.cend())
					{
						previousState = current.value();
					}
					else if (const auto previous = m_activeScan->previousFiles.constFind(canonical);
					         previous != m_activeScan->previousFiles.cend())
					{
						previousState = previous.value();
					}
				}
				else if (const auto directoryRecord = m_directories.constFind(directory);
				         directoryRecord != m_directories.cend())
				{
					if (const auto previous = directoryRecord->files.constFind(canonical);
					    previous != directoryRecord->files.cend())
					{
						previousState = previous.value();
					}
				}

				recordRuntimeDirectoryHierarchy(directory);
				const bool activeScanOwnsDirectory = recordActiveScanFile(canonical, fileState);
				if (!activeScanOwnsDirectory)
				{
					SoundCatalogDirectoryState &record = m_directories[directory];
					if (!previousState)
					{
						++m_catalogFileCount;
					}
					record.files.insert(canonical, fileState);
					if (!previousState || *previousState != fileState)
						markCatalogChanged();
				}
				const bool metadataChanged = !previousState || *previousState != fileState;
				if (!metadataChanged)
					return true;
				if (activeScanOwnsDirectory)
					m_activeScan->changedFiles.removeAll(canonical);
				if (m_directoryPublication)
				{
					removePendingPublishedPath(m_directoryPublication->files,
					                           m_directoryPublication->fileOffset, canonical);
					removePendingPublishedPath(m_directoryPublication->changedFiles,
					                           m_directoryPublication->changedFileOffset, canonical);
				}
				SoundCommand added;
				added.kind  = SoundCommandKind::CatalogFileAdded;
				added.token = m_generation;
				if (previousState)
					added.changedFiles = {canonical};
				else
					added.files = {canonical};
				m_publish(std::move(added));
				return true;
			}

			/**
			 * @brief Completes one network download and atomically installs its payload.
			 * @param downloadKey Case-insensitive destination identity.
			 */
			void finishDownload(const QString &downloadKey)
			{
				const auto it = m_downloads.find(downloadKey);
				if (it == m_downloads.end())
					return;
				auto download = std::move(it.value());
				m_downloads.erase(it);
				if (download->reply && download->reply->isOpen() && download->output &&
				    !download->writeFailed)
				{
					const QByteArray tail = download->reply->readAll();
					if (!tail.isEmpty() && download->output->write(tail) != tail.size())
						download->writeFailed = true;
				}
				const int statusCode =
				    download->reply
				        ? download->reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
				        : 0;
				const bool networkSucceeded = download->reply && download->output &&
				                              download->output->size() > 0 && !download->writeFailed &&
				                              download->reply->error() == QNetworkReply::NoError &&
				                              statusCode >= 200 && statusCode < 400;
				if (download->reply)
					download->reply->deleteLater();

				bool installed = false;
				if (networkSucceeded && download->output)
				{
					if (!download->output->flush())
						download->writeFailed = true;
					download->output->close();
					const QString parent = QFileInfo(download->destinationPath).absolutePath();
					const QString canonicalParent =
					    QMudPluginPathUtils::normalizeSeparators(QFileInfo(parent).canonicalFilePath());
					if (!download->writeFailed && !QFileInfo::exists(download->destinationPath) &&
					    !soundPathContainsSymlink(m_soundsRoot, parent) && !canonicalParent.isEmpty() &&
					    QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(canonicalParent, m_soundsRoot))
					{
						installed = QFile::rename(download->output->fileName(), download->destinationPath);
					}
					if (installed)
						download->output->setAutoRemove(false);
				}
				const QFileInfo finalInfo(download->destinationPath);
				const bool      available =
				    (installed || finalInfo.exists()) && publishAvailableCatalogFile(finalInfo);
				publishDownloadFinished(download->destinationPath, available);
			}

			/** @brief Aborts every active download and removes its temporary file. */
			void cancelDownloads()
			{
				for (const std::shared_ptr<PendingSoundDownload> &download : m_downloads)
				{
					if (download->reply)
					{
						disconnect(download->reply, nullptr, this, nullptr);
						download->reply->abort();
						download->reply->deleteLater();
					}
				}
				m_downloads.clear();
				m_networkManager.reset();
			}

			/** @brief Clears scans, snapshots, watches, and queued events for reconfiguration. */
			void clearCatalogState()
			{
				QCoreApplication::removePostedEvents(this, catalogEventType());
				cancelPersistentIndexWrite();
				m_indexRead.reset();
				if (m_fileWatcher)
				{
					const QStringList watched = m_fileWatcher->directories();
					if (!watched.isEmpty())
						m_fileWatcher->removePaths(watched);
				}
				m_directories.clear();
				m_catalogFileCount = 0;
				m_scanQueue.clear();
				m_queuedDirectories.clear();
				m_activeScan.reset();
				m_directoryPublication.reset();
				m_unreconciledIndexDirectories.clear();
				m_pluginDirectories.clear();
				m_watchedDirectories.clear();
				m_failedWatches.clear();
				m_catalogStepPending            = false;
				m_catalogReadyPublished         = false;
				m_indexDirty                    = false;
				m_indexWriteFailed              = false;
				m_catalogRevision               = 0;
				m_maximumReconciliationStepSize = 0;
				m_pluginsRoot.clear();
				m_pluginGeneration = 0;
			}

			Publish                                               m_publish;
			CatalogDirectories                                    m_directories;
			QHash<QString, PluginSoundDirectoryState>             m_pluginDirectories;
			QHash<QString, std::shared_ptr<PendingSoundDownload>> m_downloads;
			QSet<QString>                                         m_queuedDirectories;
			QSet<QString>                                         m_watchedDirectories;
			QSet<QString>                                         m_failedWatches;
			std::deque<QString>                                   m_scanQueue;
			std::optional<DirectoryScan>                          m_activeScan;
			std::optional<DirectoryPublication>                   m_directoryPublication;
			std::optional<PersistentIndexRead>                    m_indexRead;
			std::optional<PersistentIndexWrite>                   m_indexWrite;
			std::unique_ptr<QFileSystemWatcher>                   m_fileWatcher;
			std::unique_ptr<QNetworkAccessManager>                m_networkManager;
			QStringList                                           m_soundNameFilters;
			QString                                               m_qmudHome;
			QString                                               m_soundsPath;
			QString                                               m_soundsRoot;
			QString                                               m_pluginsRoot;
			QSet<QString>                                         m_unreconciledIndexDirectories;
			quint64                                               m_generation{0};
			quint64                                               m_pluginGeneration{0};
			quint64                                               m_catalogRevision{0};
			qsizetype                                             m_catalogFileCount{0};
			qsizetype                                             m_maximumReconciliationStepSize{0};
			int  m_downloadStallTimeoutMilliseconds{kSoundDownloadStallTimeoutMilliseconds};
			bool m_catalogStepPending{false};
			bool m_catalogPaused{false};
			bool m_indexReadPausedForTest{false};
			bool m_catalogReadyPublished{false};
			bool m_indexDirty{false};
			bool m_indexWriteFailed{false};
			bool m_shuttingDown{false};
	};

	class SoundWorker final : public QObject
	{
		public:
			using PlaybackError                = std::function<void(quint64, const QString &)>;
			using PlaybackInitializationActive = std::function<bool()>;
			using PlaybackInitialized          = std::function<bool()>;
			using PlaybackInitializationFailed = std::function<void()>;

			/**
			 * @brief Creates the worker-side sound engine state.
			 * @param ingress Shared command queue owned with the public engine.
			 * @param playbackError Callback that reports playback failures to the engine owner.
			 * @param playbackInitializationActive Callback that reports whether startup still needs initialization.
			 * @param playbackInitialized Callback that reports completion of explicit playback initialization.
			 * @param playbackInitializationFailed Callback that rejects startup after device discovery fails.
			 */
			SoundWorker(std::shared_ptr<SoundCommandIngress> ingress, PlaybackError playbackError,
			            PlaybackInitializationActive playbackInitializationActive,
			            PlaybackInitialized          playbackInitialized,
			            PlaybackInitializationFailed playbackInitializationFailed)
			    : m_ingress(std::move(ingress)), m_playbackError(std::move(playbackError)),
			      m_playbackInitializationActive(std::move(playbackInitializationActive)),
			      m_playbackInitialized(std::move(playbackInitialized)),
			      m_playbackInitializationFailed(std::move(playbackInitializationFailed))
			{
				m_clock.start();
			}

			void initialize()
			{
				Q_ASSERT(QThread::currentThread() == thread());
				m_ioWorker = new SoundIoWorker([ingress = m_ingress](SoundCommand command)
				                               { enqueueAsyncResult(ingress, std::move(command)); });
				m_ioWorker->moveToThread(&m_ioThread);
				connect(&m_ioThread, &QThread::started, m_ioWorker,
				        [worker = m_ioWorker] { worker->initialize(); });
				connect(&m_ioThread, &QThread::finished, m_ioWorker, &QObject::deleteLater);
				m_ioThread.setObjectName(QStringLiteral("QMud sound I/O"));
				m_ioThread.start(QThread::LowPriority);
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
				m_preinitializedMediaBackend.reset();
				if (m_ioWorker && m_ioThread.isRunning())
				{
					QMetaObject::invokeMethod(
					    m_ioWorker, [worker = m_ioWorker] { worker->shutdown(); },
					    Qt::BlockingQueuedConnection);
					m_ioThread.quit();
					m_ioThread.wait();
					m_ioWorker = nullptr;
				}
				m_catalogFiles.clear();
				m_catalogFileByRelativePath.clear();
				m_cachedFileSourceKeys.clear();
				m_catalogFilesByName.clear();
				m_catalogFilesByStem.clear();
				m_catalogDirectories.clear();
				m_watchedCatalogDirectories.clear();
				m_pluginFileByRelativePath.clear();
				m_watchedPluginDirectories.clear();
				m_trackedPluginDirectories.clear();
				m_pendingPluginDirectories.clear();
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

			/** @brief Forces startup device discovery to report no usable output for deterministic tests. */
			void forceNoAudioOutputForTest()
			{
				Q_ASSERT(QThread::currentThread() == thread());
				m_forceNoAudioOutputForTest = true;
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
			 * @brief Seeds one real file request into the production forward and reverse cache indexes.
			 * @param sessionId Session owning the request.
			 * @param request Request text used as the session cache key.
			 * @param absolutePath Existing catalog file identity.
			 * @return `true` when the cache identity was installed.
			 */
			[[nodiscard]] bool cacheFileRequestForTest(const quint64 sessionId, const QString &request,
			                                           const QString &absolutePath)
			{
				auto session = m_sessions.find(sessionId);
				if (session == m_sessions.end())
					return false;
				const QFileInfo info(absolutePath);
				if (!info.exists() || !info.isFile())
					return false;
				SoundSourceKey key;
				key.identity = QMudPluginPathUtils::normalizeSeparators(info.canonicalFilePath());
				if (key.identity.isEmpty() ||
				    !QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(key.identity, m_soundsRoot) ||
				    !m_catalogFiles.contains(key.identity))
				{
					return false;
				}
				key.size                = info.size();
				key.modifiedMsecs       = info.lastModified().toMSecsSinceEpoch();
				key.deviceId            = m_deviceId;
				auto backend            = std::make_unique<SoundBackend>();
				backend->key            = key;
				backend->backendId      = m_nextBackendId++;
				const quint64 backendId = backend->backendId;
				const auto    inserted =
				    m_cache.insertActive(key, key.size, backendId, std::move(backend), currentTimeMsecs());
				if (!inserted.backend)
					return false;
				m_cache.markIdle(key, backendId, currentTimeMsecs());
				m_cachedFileSourceKeys.insert(soundCatalogComparisonKey(key.identity), key);
				cacheFileRequest(session.value(), request, key);
				return true;
			}

			/**
			 * @brief Reports whether one request still has a reusable exact-file cache association.
			 * @param sessionId Session owning the request.
			 * @param request Request text used as the session cache key.
			 * @return `true` when both cache indexes and a backend retain the request.
			 */
			[[nodiscard]] bool fileRequestIsCachedForTest(const quint64  sessionId,
			                                              const QString &request) const
			{
				const auto session = m_sessions.constFind(sessionId);
				if (session == m_sessions.cend())
					return false;
				const auto requestKey = session->cachedFileRequestKeys.constFind(request);
				return requestKey != session->cachedFileRequestKeys.cend() &&
				       m_cachedFileSourceKeys.contains(soundCatalogComparisonKey(requestKey->identity)) &&
				       m_cache.backendCount(*requestKey) > 0;
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
				    channel.looping || channel.ownershipGeneration == 0)
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
				channel.nativeResolutionPending        = false;
				inserted.backend->sessionId            = sessionId;
				inserted.backend->channel              = channelIndex;
				inserted.backend->assignmentGeneration = channel.assignmentGeneration;
				inserted.backend->started              = true;
				publishSnapshotIfChanged(session);
				completeChannelPlayback(session, channelIndex);
				return true;
			}

			/**
			 * @brief Completes an active assignment through normal worker completion logic.
			 * @param sessionId Session owning the active assignment.
			 * @param buffer One-based active channel to complete.
			 * @return `true` when the active assignment was completed.
			 */
			[[nodiscard]] bool completeActiveChannelForTest(const quint64 sessionId, const int buffer)
			{
				Q_ASSERT(QThread::currentThread() == thread());
				auto sessionIt = m_sessions.find(sessionId);
				if (sessionIt == m_sessions.end() || buffer < 1 || buffer > SoundEngineSession::kChannelCount)
					return false;
				SoundChannel &channel = sessionIt->channels[static_cast<std::size_t>(buffer - 1)];
				if (!channel.backend || !channel.playing)
					return false;
				std::unique_ptr<QSignalBlocker> playerBlocker;
				if (channel.backend->player)
					playerBlocker = std::make_unique<QSignalBlocker>(channel.backend->player.get());
				stopBackend(*channel.backend);
				channel.playbackStarted = true;
				completeChannelPlayback(sessionIt.value(), buffer - 1);
				return true;
			}

			/**
			 * @brief Returns the file identity assigned to one active channel.
			 * @param sessionId Worker session identity.
			 * @param buffer One-based channel to inspect.
			 * @return Absolute source identity, or an empty string when no backend is assigned.
			 */
			[[nodiscard]] QString activeFileIdentityForTest(const quint64 sessionId, const int buffer) const
			{
				Q_ASSERT(QThread::currentThread() == thread());
				const auto sessionIt = m_sessions.constFind(sessionId);
				if (sessionIt == m_sessions.cend() || buffer < 1 ||
				    buffer > SoundEngineSession::kChannelCount)
				{
					return {};
				}
				const SoundChannel &channel = sessionIt->channels[static_cast<std::size_t>(buffer - 1)];
				return channel.backend ? channel.backend->key.identity : QString();
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
				return {m_preinitializedMediaBackend != nullptr};
			}

			/**
			 * @brief Returns the worker-owned sound-file catalog state.
			 * @return Catalog sizes and initial-scan completion state.
			 */
			[[nodiscard]] SoundWorkerFileCatalogState fileCatalogStateForTest() const
			{
				Q_ASSERT(QThread::currentThread() == thread());
				return {m_catalogFiles.size(),           m_catalogDirectories.size(),
				        m_watchedDirectoryCount,         m_failedWatchCount,
				        m_maximumIndexBatchSize,         m_maximumDirectoryBatchSize,
				        m_maximumReconciliationStepSize, m_watchStatePathEntryCount,
				        m_publishedCatalogRevision,      m_catalogReady};
			}

			/**
			 * @brief Pauses or resumes low-priority catalog events for deterministic tests.
			 * @param paused Whether later catalog events should remain suspended.
			 */
			void setCatalogPausedForTest(const bool paused)
			{
				Q_ASSERT(QThread::currentThread() == thread());
				if (m_ioWorker)
					QMetaObject::invokeMethod(
					    m_ioWorker, [worker = m_ioWorker, paused] { worker->setCatalogPaused(paused); },
					    Qt::BlockingQueuedConnection);
			}

			/**
			 * @brief Pauses or resumes persistent-index ingestion for deterministic tests.
			 * @param paused Whether index batches must remain suspended.
			 */
			void setIndexReadPausedForTest(const bool paused)
			{
				Q_ASSERT(QThread::currentThread() == thread());
				if (m_ioWorker)
					QMetaObject::invokeMethod(
					    m_ioWorker, [worker = m_ioWorker, paused] { worker->setIndexReadPaused(paused); },
					    Qt::BlockingQueuedConnection);
			}

			/**
			 * @brief Publishes synthetic catalog paths through the production directory handoff.
			 * @param fileCount Number of paths to publish.
			 * @return `true` when the I/O worker accepted the publication.
			 */
			[[nodiscard]] bool publishSyntheticCatalogDirectoryForTest(const qsizetype fileCount) const
			{
				Q_ASSERT(QThread::currentThread() == thread());
				if (!m_ioWorker)
					return false;
				bool accepted = false;
				QMetaObject::invokeMethod(
				    m_ioWorker, [worker = m_ioWorker, fileCount, &accepted]
				    { accepted = worker->publishSyntheticDirectory(fileCount); },
				    Qt::BlockingQueuedConnection);
				return accepted;
			}

			/**
			 * @brief Returns the number of file requests that required full filesystem resolution.
			 * @return Count of file-cache misses handled by the slow resolution path.
			 */
			[[nodiscard]] quint64 slowFileResolutionCountForTest() const
			{
				Q_ASSERT(QThread::currentThread() == thread());
				return m_slowFileResolutionCount;
			}

			/**
			 * @brief Reads one worker-owned native playback stream.
			 * @param sessionId Session owning the logical playback.
			 * @param protocol Protocol playback namespace.
			 * @param media Media-class playback namespace.
			 * @return Current logical playback state.
			 */
			[[nodiscard]] SoundWorkerNativePlaybackState
			nativePlaybackStateForTest(const quint64 sessionId, const QMudNativeSoundProtocol protocol,
			                           const QMudNativeSoundMedia media) const
			{
				Q_ASSERT(QThread::currentThread() == thread());
				const auto sessionIt = m_sessions.constFind(sessionId);
				if (sessionIt == m_sessions.cend())
					return {};
				const WorkerSession::NativePlayback &playback =
				    sessionIt->nativePlayback[nativePlaybackIndex(protocol, media)];
				return {playback.buffer, playback.filePattern, playback.priority,
				        nativePlaybackCurrent(*sessionIt, playback)};
			}

			/**
			 * @brief Attempts one watcher registration through the production path.
			 * @param directory Directory to register.
			 * @return `true` when registration succeeded.
			 */
			[[nodiscard]] bool attemptCatalogWatchForTest(const QString &directory)
			{
				Q_ASSERT(QThread::currentThread() == thread());
				if (!m_ioWorker)
					return false;
				bool watched = false;
				QMetaObject::invokeMethod(
				    m_ioWorker, [worker = m_ioWorker, directory, &watched]
				    { watched = worker->attemptWatch(directory); }, Qt::BlockingQueuedConnection);
				return watched;
			}

			/** @brief Reconciles the configured sounds root through the production watcher transition. */
			void reconcileSoundRootForTest()
			{
				Q_ASSERT(QThread::currentThread() == thread());
				if (m_ioWorker)
				{
					QMetaObject::invokeMethod(
					    m_ioWorker, [worker = m_ioWorker] { worker->reconcileRoot(); },
					    Qt::BlockingQueuedConnection);
					drain();
				}
			}

			/**
			 * @brief Reconciles one catalog directory through the production watcher transition.
			 * @param directory Changed catalog directory.
			 */
			void reconcileCatalogDirectoryForTest(const QString &directory)
			{
				Q_ASSERT(QThread::currentThread() == thread());
				if (m_ioWorker)
				{
					QMetaObject::invokeMethod(
					    m_ioWorker, [worker = m_ioWorker, directory]
					    { worker->reconcileDirectory(directory); }, Qt::BlockingQueuedConnection);
					drain();
				}
			}

			/** @brief Runs one production periodic maintenance pass. */
			void runPeriodicMaintenanceForTest()
			{
				Q_ASSERT(QThread::currentThread() == thread());
				m_releaseCompletedPending = true;
				m_failedWatchRetryPending = true;
				performMaintenance();
			}

			/**
			 * @brief Replaces the native-download inactivity timeout for deterministic tests.
			 * @param milliseconds Positive no-progress interval applied to later requests.
			 */
			void setDownloadStallTimeoutForTest(const int milliseconds)
			{
				Q_ASSERT(QThread::currentThread() == thread());
				Q_ASSERT(milliseconds > 0);
				if (m_ioWorker)
					QMetaObject::invokeMethod(
					    m_ioWorker, [worker = m_ioWorker, milliseconds]
					    { worker->setDownloadStallTimeout(milliseconds); }, Qt::BlockingQueuedConnection);
			}

			/**
			 * @brief Processes an existing file through native download completion for deterministic tests.
			 * @param destination Existing sound file beneath the configured sounds root.
			 */
			void processExistingDownloadForTest(const QString &destination) const
			{
				Q_ASSERT(QThread::currentThread() == thread());
				if (m_ioWorker)
					QMetaObject::invokeMethod(
					    m_ioWorker, [worker = m_ioWorker, destination]
					    { worker->processExistingDownloadForTest(destination); },
					    Qt::BlockingQueuedConnection);
			}

		public:
			/**
			 * @brief Runs queued sound-worker background events at low event-loop priority.
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
					if (!m_shuttingDown &&
					    (m_releaseCompletedPending || cacheNeedsEviction(currentTimeMsecs())))
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
					if (!initializePlaybackBackends())
					{
						m_playbackInitializationAbandoned = true;
						m_playbackInitializationFailed();
						break;
					}
					if (!m_playbackInitialized())
					{
						m_playbackInitializationAbandoned = true;
						m_preinitializedMediaBackend.reset();
						m_mediaDevices.reset();
						m_deviceId.clear();
					}
					result = eOK;
					break;
				case SoundCommandKind::ConfigureSoundDirectory:
					configureSoundDirectory(command.text);
					result = eOK;
					break;
				case SoundCommandKind::ConfigurePluginDirectory:
					configurePluginDirectory(command.text);
					result = eOK;
					break;
				case SoundCommandKind::RegisterSession:
				{
					WorkerSession session;
					session.sessionId = command.sessionId;
					initializeWorldSoundDirectory(session, command.worldSoundDirectory);
					session.publishedState            = command.publishedState;
					session.luaAudioOwnershipReleased = std::move(command.luaAudioOwnershipReleased);
					m_sessions.insert(command.sessionId, std::move(session));
					result = eOK;
					break;
				}
				case SoundCommandKind::PlayFile:
					result = playFile(command, selectedBuffer);
					break;
				case SoundCommandKind::UpdateWorldSoundDirectory:
				{
					auto sessionIt = m_sessions.find(command.sessionId);
					if (sessionIt != m_sessions.end())
					{
						updateWorldSoundDirectory(sessionIt.value(), command.worldSoundDirectory);
						result = eOK;
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
				case SoundCommandKind::CatalogIndexBatch:
					handleCatalogIndexBatch(command);
					result = eOK;
					break;
				case SoundCommandKind::CatalogIndexLoaded:
					handleCatalogIndexLoaded(command);
					result = eOK;
					break;
				case SoundCommandKind::CatalogScanStarted:
					if (command.token == m_catalogGeneration)
						m_catalogReady = false;
					result = eOK;
					break;
				case SoundCommandKind::CatalogDirectoryBatch:
					handleCatalogDirectoryBatch(command);
					result = eOK;
					break;
				case SoundCommandKind::CatalogDirectoryScanned:
					handleCatalogDirectoryScanned(command);
					result = eOK;
					break;
				case SoundCommandKind::CatalogFileAdded:
					if (command.token == m_catalogGeneration)
					{
						for (const QString &file : command.removedFiles)
							removeCatalogFile(file);
						for (const QString &file : command.files)
							static_cast<void>(indexCatalogPath(file));
						for (const QString &file : command.changedFiles)
						{
							invalidateCachedFileSource(file);
							static_cast<void>(indexCatalogPathAfterCacheInvalidation(file));
						}
					}
					result = eOK;
					break;
				case SoundCommandKind::PluginDirectoryChanged:
					handlePluginDirectoryChanged(command);
					result = eOK;
					break;
				case SoundCommandKind::CatalogCleared:
					handleCatalogCleared(command);
					result = eOK;
					break;
				case SoundCommandKind::CatalogReady:
					if (command.token == m_catalogGeneration)
					{
						m_catalogAvailable = true;
						m_catalogReady     = true;
						m_maximumReconciliationStepSize =
						    qMax(m_maximumReconciliationStepSize, command.maximumReconciliationStepSize);
						m_publishedCatalogRevision = command.catalogRevision;
						resumePendingNativeResolutions();
					}
					result = eOK;
					break;
				case SoundCommandKind::CatalogWatchState:
					if (command.token == m_catalogGeneration)
					{
						m_watchStatePathEntryCount += command.directories.size();
						m_watchedDirectoryCount = command.watchedDirectoryCount;
						m_failedWatchCount      = command.failedWatchCount;
					}
					result = eOK;
					break;
				case SoundCommandKind::DownloadFinished:
					handleDownloadFinished(command);
					result = eOK;
					break;
				case SoundCommandKind::ReserveLuaAudioChannel:
					result = reserveLuaAudioChannel(command, selectedBuffer);
					break;
				case SoundCommandKind::ConfirmLuaAudioOwnership:
					result = confirmLuaAudioOwnership(command);
					break;
				case SoundCommandKind::NativeSoundRequest:
					executeNativeSoundRequest(command);
					result = eOK;
					break;
				case SoundCommandKind::Stop:
					result = stop(command.sessionId, command.buffer, command.ownershipKind,
					              command.ownershipGeneration);
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
			 * @brief Resolves one session's initial preferred directory without scheduling catalog work.
			 * @param session Worker session to update.
			 * @param configuredDirectory Persisted per-world sound directory.
			 */
			void initializeWorldSoundDirectory(WorkerSession &session,
			                                   const QString &configuredDirectory) const
			{
				session.worldSoundDirectorySetting = configuredDirectory;
				session.worldSoundCatalogPrefix.clear();
				if (m_qmudHome.isEmpty())
					return;
				WorldSoundDirectoryPaths rootPaths;
				static_cast<void>(resolveWorldSoundDirectory(
				    m_qmudHome, QString::fromLatin1(kDefaultWorldSoundDirectory), &rootPaths));
				WorldSoundDirectoryPaths paths;
				if (!resolveWorldSoundDirectory(m_qmudHome, configuredDirectory, &paths))
					paths = rootPaths;
				session.worldSoundCatalogPrefix = paths.catalogPrefix;
			}

			/**
			 * @brief Applies an explicitly accepted world-preference directory change.
			 * @param session Worker session to update.
			 * @param configuredDirectory Validated persisted per-world sound directory.
			 */
			void updateWorldSoundDirectory(WorkerSession &session, const QString &configuredDirectory)
			{
				const QString previousPrefix = session.worldSoundCatalogPrefix;
				initializeWorldSoundDirectory(session, configuredDirectory);
				if (sameSoundFilesystemPath(previousPrefix, session.worldSoundCatalogPrefix))
					return;
				clearCachedFileRequests(session);
				if (!m_ioWorker || m_soundsRoot.isEmpty())
					return;
				const QString directory = QMudPluginPathUtils::normalizeSeparators(
				    QDir(m_soundsRoot).filePath(session.worldSoundCatalogPrefix));
				QMetaObject::invokeMethod(
				    m_ioWorker, [worker = m_ioWorker, directory]
				    { worker->reconcilePreferredDirectory(directory); }, Qt::QueuedConnection);
			}

			/** @brief Re-resolves every session directory after the process-wide sounds root changes. */
			void refreshWorldSoundDirectories()
			{
				for (WorkerSession &session : m_sessions)
					initializeWorldSoundDirectory(session, session.worldSoundDirectorySetting);
			}

			/**
			 * @brief Converts one request to a path relative to the configured sounds tree.
			 * @param fileName Native, portable, or legacy sound path.
			 * @param explicitlyRooted Optional destination reporting whether the request named `sounds`.
			 * @return Portable sounds-tree-relative path, or an empty string when invalid.
			 */
			[[nodiscard]] QString soundRequestRelativePath(const QString &fileName,
			                                               bool          *explicitlyRooted = nullptr) const
			{
				QString       portableInput = fileName;
				const QString normalized    = QMudPluginPathUtils::normalizeSeparators(fileName);
				if (!m_qmudHome.isEmpty() && QFileInfo(normalized).isAbsolute())
				{
					QString resolvedHomePath;
					QString error;
					if (m_soundsRoot.isEmpty() || !QMudPluginPathUtils::resolveInsideQmudHome(
					                                  m_qmudHome, normalized, &resolvedHomePath, &error))
					{
						return {};
					}
					const QString soundsRelative = QMudPluginPathUtils::normalizeSeparators(
					    QDir(m_soundsRoot).relativeFilePath(resolvedHomePath));
					QString resolvedSoundPath;
					if (!QMudPluginPathUtils::resolveInsideDirectory(m_soundsRoot, soundsRelative,
					                                                 &resolvedSoundPath, &error))
					{
						return {};
					}
					portableInput =
					    QMudPluginPathUtils::qmudHomeRelativePath(m_qmudHome, resolvedSoundPath, false);
				}
				return soundTreeRelativePath(std::move(portableInput), explicitlyRooted);
			}

			/**
			 * @brief Recognizes an explicit plugin-bundled sound path without widening ordinary sound lookup.
			 * @param fileName Portable, native, or legacy path supplied by a compatibility API.
			 * @return Path relative to the configured plugins directory, or an empty string otherwise.
			 */
			[[nodiscard]] QString pluginSoundRequestRelativePath(const QString &fileName) const
			{
				if (m_pluginsRoot.isEmpty() || !soundPlaybackSupportsFile(fileName))
					return {};
				QString       portable;
				const QString normalized = QMudPluginPathUtils::normalizeSeparators(fileName);
				QString       error;
				if (QMudPluginPathUtils::resolveInsideDirectory(m_qmudHome, normalized, nullptr, &error))
					portable = QMudPluginPathUtils::normalizeSeparators(QDir::cleanPath(normalized));
				else
				{
					QString resolved;
					if (!QMudPluginPathUtils::resolveInsideQmudHome(m_qmudHome, normalized, &resolved,
					                                                &error))
						return {};
					portable = QMudPluginPathUtils::qmudHomeRelativePath(m_qmudHome, resolved, false);
				}

				const QString configuredRelative =
				    QMudPluginPathUtils::qmudHomeRelativePath(m_qmudHome, m_pluginsRoot, false);
				QStringList configuredParts =
				    configuredRelative == QLatin1String(".")
				        ? QStringList()
				        : configuredRelative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
				const QStringList requestedParts = portable.split(QLatin1Char('/'), Qt::SkipEmptyParts);
				if (requestedParts.size() <= configuredParts.size())
					return {};
				for (qsizetype index = 0; index < configuredParts.size(); ++index)
				{
					if (requestedParts.at(index).compare(configuredParts.at(index), Qt::CaseInsensitive) != 0)
						return {};
				}
				return requestedParts.mid(configuredParts.size()).join(QLatin1Char('/'));
			}

			/**
			 * @brief Adds one validated plugin sound to exact-path lookup only.
			 * @param absolutePath Canonical supported audio file beneath the configured plugins directory.
			 */
			void indexPluginSoundFile(const QString &absolutePath)
			{
				const QString normalized = QMudPluginPathUtils::normalizeSeparators(absolutePath);
				if (m_pluginsRoot.isEmpty() || !soundPlaybackSupportsFile(normalized) ||
				    !QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(normalized, m_pluginsRoot))
				{
					return;
				}
				const QString relative = QMudPluginPathUtils::normalizeSeparators(
				    QDir(m_pluginsRoot).relativeFilePath(normalized));
				m_pluginFileByRelativePath.insert(soundCatalogComparisonKey(relative), normalized);
			}

			/**
			 * @brief Queues lazy tracking for one explicit plugin sound's immediate directory.
			 * @param directory Canonical direct parent beneath the configured plugins directory.
			 * @param reconcileKnownDirectory Whether a newly discovered file requires refreshing existing metadata.
			 */
			void requestPluginDirectoryTracking(const QString &directory, const bool reconcileKnownDirectory)
			{
				const QString normalized = QMudPluginPathUtils::normalizeSeparators(directory);
				if (!m_ioWorker || normalized.isEmpty() || m_pendingPluginDirectories.contains(normalized) ||
				    (!reconcileKnownDirectory && m_trackedPluginDirectories.contains(normalized)))
				{
					return;
				}
				m_pendingPluginDirectories.insert(normalized);
				QMetaObject::invokeMethod(
				    m_ioWorker,
				    [worker = m_ioWorker, generation = m_pluginDirectoryGeneration, directory = normalized]
				    { worker->trackPluginDirectory(generation, directory); }, Qt::QueuedConnection);
			}

			/**
			 * @brief Resolves one request through the in-memory sound catalog without filesystem access.
			 * @param session Session supplying world-first lookup precedence.
			 * @param fileName Relative sound-tree path or an absolute indexed sound path.
			 * @return Canonical indexed file path, or an empty string when the catalog has no match.
			 */
			[[nodiscard]] QString resolveCatalogFile(const WorkerSession &session,
			                                         const QString       &fileName) const
			{
				if (!m_catalogConfigured)
					return {};
				bool    explicitlyRooted = false;
				QString relative         = soundRequestRelativePath(fileName, &explicitlyRooted);
				if (relative.isEmpty())
					return {};
				if (!explicitlyRooted && !session.worldSoundCatalogPrefix.isEmpty())
				{
					const QString preferred = QMudPluginPathUtils::normalizeSeparators(
					    QDir(session.worldSoundCatalogPrefix).filePath(relative));
					const auto preferredFile =
					    m_catalogFileByRelativePath.constFind(soundCatalogComparisonKey(preferred));
					if (preferredFile != m_catalogFileByRelativePath.cend())
						return *preferredFile;
				}

				const auto sharedFile =
				    m_catalogFileByRelativePath.constFind(soundCatalogComparisonKey(relative));
				return sharedFile == m_catalogFileByRelativePath.cend() ? QString() : *sharedFile;
			}

			/**
			 * @brief Returns a reusable source key without repeating file validation or backend inspection.
			 * @param session Session supplying world-first lookup precedence.
			 * @param fileName Requested sound path.
			 * @param resolved Destination receiving the cached canonical file identity.
			 * @return Cached source key while at least one backend for it remains resident.
			 */
			std::optional<SoundSourceKey> cachedFileSourceKey(WorkerSession &session, const QString &fileName,
			                                                  QString &resolved)
			{
				auto cachedRequest = session.cachedFileRequestKeys.find(fileName);
				if (cachedRequest != session.cachedFileRequestKeys.end())
				{
					if (cachedRequest->deviceId == m_deviceId && m_cache.backendCount(*cachedRequest) > 0)
					{
						resolved = cachedRequest->identity;
						return *cachedRequest;
					}
					removeCachedFileRequest(session, fileName);
				}

				QString catalogFile = resolveCatalogFile(session, fileName);
				if (catalogFile.isEmpty())
				{
					const QString pluginRelative = pluginSoundRequestRelativePath(fileName);
					if (!pluginRelative.isEmpty())
					{
						const auto pluginFile =
						    m_pluginFileByRelativePath.constFind(soundCatalogComparisonKey(pluginRelative));
						if (pluginFile != m_pluginFileByRelativePath.cend())
							catalogFile = *pluginFile;
					}
				}
				if (!catalogFile.isEmpty())
				{
					const QString cacheKey = soundCatalogComparisonKey(catalogFile);
					auto          cached   = m_cachedFileSourceKeys.find(cacheKey);
					if (cached != m_cachedFileSourceKeys.end())
					{
						if (cached->deviceId == m_deviceId && m_cache.backendCount(*cached) > 0)
						{
							cacheFileRequest(session, fileName, *cached);
							resolved = catalogFile;
							return *cached;
						}
						m_cachedFileSourceKeys.erase(cached);
					}
				}
				return std::nullopt;
			}

			/**
			 * @brief Resolves and validates a file-backed sound without initializing multimedia state.
			 * @param session Session providing the QMud-home root.
			 * @param fileName Relative sound-tree path or an absolute path beneath QMud home sounds.
			 * @param resolved Destination receiving the contained absolute file path.
			 * @param allowPluginSounds Whether explicit paths beneath the configured plugins directory are accepted.
			 * @return Existing regular-file metadata, or no value when resolution fails.
			 */
			std::optional<QFileInfo> resolveFileInfo(const WorkerSession &session, const QString &fileName,
			                                         QString &resolved, const bool allowPluginSounds)
			{
				const QString pluginRelative =
				    allowPluginSounds ? pluginSoundRequestRelativePath(fileName) : QString();
				if (!pluginRelative.isEmpty())
				{
					QString candidate = pluginRelative;
					bool    indexedFile{false};
					if (const auto indexed =
					        m_pluginFileByRelativePath.constFind(soundCatalogComparisonKey(pluginRelative));
					    indexed != m_pluginFileByRelativePath.cend())
					{
						indexedFile = true;
						candidate   = QMudPluginPathUtils::normalizeSeparators(
						    QDir(m_pluginsRoot).relativeFilePath(*indexed));
					}
					if (!resolveSoundFileCaseInsensitively(m_pluginsRoot, candidate, &resolved))
					{
						if (indexedFile)
						{
							requestPluginDirectoryTracking(
							    QMudPluginPathUtils::normalizeSeparators(
							        QFileInfo(QDir(m_pluginsRoot).filePath(candidate)).absolutePath()),
							    true);
						}
						return std::nullopt;
					}
					indexPluginSoundFile(resolved);
					requestPluginDirectoryTracking(
					    QMudPluginPathUtils::normalizeSeparators(QFileInfo(resolved).absolutePath()),
					    !indexedFile);
					return QFileInfo(resolved);
				}

				auto resolveCandidate = [this,
				                         &resolved](const QString &candidate) -> std::optional<QFileInfo>
				{
					QString normalized;
					QString error;
					if (m_soundsRoot.isEmpty() || !QMudPluginPathUtils::resolveInsideDirectory(
					                                  m_soundsRoot, candidate, &normalized, &error))
						return std::nullopt;
					if (soundPathContainsSymlink(m_soundsRoot, normalized))
						return std::nullopt;
					QFileInfo info(normalized);
					if (!info.exists() || !info.isFile())
						return std::nullopt;
					const QString canonical =
					    QMudPluginPathUtils::normalizeSeparators(info.canonicalFilePath());
					if (canonical.isEmpty())
						return std::nullopt;
					resolved = canonical;
					return QFileInfo(canonical);
				};
				auto resolveCatalogCandidate =
				    [this, &resolveCandidate](const QString &candidate) -> std::optional<QFileInfo>
				{
					if (!m_catalogConfigured)
						return std::nullopt;
					QString normalized;
					QString error;
					if (m_soundsRoot.isEmpty() || !QMudPluginPathUtils::resolveInsideDirectory(
					                                  m_soundsRoot, candidate, &normalized, &error))
					{
						return std::nullopt;
					}
					const QString relative = QMudPluginPathUtils::normalizeSeparators(
					    QDir(m_soundsRoot).relativeFilePath(normalized));
					const auto catalogFile =
					    m_catalogFileByRelativePath.constFind(soundCatalogComparisonKey(relative));
					if (catalogFile == m_catalogFileByRelativePath.cend())
						return std::nullopt;
					return resolveCandidate(QDir(m_soundsRoot).relativeFilePath(*catalogFile));
				};
				auto resolveCaseInsensitiveCandidate =
				    [this, &resolved](const QString &candidate) -> std::optional<QFileInfo>
				{
					return resolveSoundFileCaseInsensitively(m_soundsRoot, candidate, &resolved)
					           ? std::optional<QFileInfo>(QFileInfo(resolved))
					           : std::nullopt;
				};
				auto resolveWithCatalogFallback =
				    [this, &resolveCandidate, &resolveCatalogCandidate,
				     &resolveCaseInsensitiveCandidate](const QString &candidate) -> std::optional<QFileInfo>
				{
					if (auto info = resolveCandidate(candidate))
						return info;
					if (auto info = resolveCatalogCandidate(candidate))
						return info;
					if (!m_catalogReady)
						return resolveCaseInsensitiveCandidate(candidate);
					return std::nullopt;
				};

				bool          explicitlyRooted = false;
				const QString relative         = soundRequestRelativePath(fileName, &explicitlyRooted);
				if (relative.isEmpty())
					return std::nullopt;
				if (!explicitlyRooted)
				{
					if (!session.worldSoundCatalogPrefix.isEmpty())
					{
						if (auto info = resolveWithCatalogFallback(
						        QDir(session.worldSoundCatalogPrefix).filePath(relative)))
						{
							return info;
						}
					}
					return resolveWithCatalogFallback(relative);
				}
				return resolveWithCatalogFallback(relative);
			}

			/**
			 * @brief Resolves a sound and builds its device-specific backend cache identity.
			 * @param session Session providing the QMud-home root.
			 * @param fileName Relative sound-tree path or an absolute path beneath QMud home sounds.
			 * @param resolved Destination receiving the contained absolute file path.
			 * @return Playback source key, or no value when resolution fails.
			 */
			std::optional<SoundSourceKey> resolveFile(WorkerSession &session, const QString &fileName,
			                                          QString &resolved)
			{
				ensureMediaDevices();
				if (auto cached = cachedFileSourceKey(session, fileName, resolved))
					return cached;
				++m_slowFileResolutionCount;
				const auto info = resolveFileInfo(session, fileName, resolved, true);
				if (!info)
					return std::nullopt;
				SoundSourceKey key;
				key.identity      = resolved;
				key.size          = info->size();
				key.modifiedMsecs = info->lastModified().toMSecsSinceEpoch();
				key.deviceId      = m_deviceId;
				const QString sourceDirectory =
				    QMudPluginPathUtils::normalizeSeparators(QFileInfo(resolved).path());
				const bool watchedSoundDirectory =
				    m_catalogConfigured &&
				    QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(sourceDirectory, m_soundsRoot) &&
				    m_watchedCatalogDirectories.contains(sourceDirectory);
				const bool watchedPluginDirectory =
				    !m_pluginsRoot.isEmpty() &&
				    QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(sourceDirectory, m_pluginsRoot) &&
				    m_watchedPluginDirectories.contains(sourceDirectory);
				if (watchedSoundDirectory || watchedPluginDirectory)
				{
					const QString sourceCacheKey = soundCatalogComparisonKey(resolved);
					m_cachedFileSourceKeys.insert(sourceCacheKey, key);
					cacheFileRequest(session, fileName, key);
				}
				return key;
			}

			/**
			 * @brief Normalizes a protocol sound pattern without permitting an absolute or escaping path.
			 * @param filePattern Protocol-provided file pattern.
			 * @return Safe relative pattern, or an empty string when invalid.
			 */
			[[nodiscard]] QString normalizeNativeSoundPattern(const QString &filePattern) const
			{
				return soundRequestRelativePath(filePattern);
			}

			/**
			 * @brief Resolves one exact native filename before the background catalog is complete.
			 * @param session Session supplying world-first lookup precedence.
			 * @param filePattern Protocol-provided exact filename.
			 * @return Indexed canonical file path, or no value for a pattern, miss, or completed catalog.
			 */
			std::optional<QString> resolveNativeExactFileBeforeCatalog(const WorkerSession &session,
			                                                           const QString       &filePattern)
			{
				if (m_catalogReady)
					return std::nullopt;
				const QString requested = normalizeNativeSoundPattern(filePattern);
				if (requested.isEmpty() || requested.contains(QLatin1Char('*')) ||
				    requested.contains(QLatin1Char('?')) || QFileInfo(requested).suffix().isEmpty() ||
				    !soundPlaybackSupportsFile(requested))
				{
					return std::nullopt;
				}

				QString    resolved;
				const auto info = resolveFileInfo(session, requested, resolved, false);
				if (!info)
					return std::nullopt;
				const QString indexed = indexCatalogFile(*info);
				return indexed.isEmpty() ? std::nullopt : std::optional<QString>(indexed);
			}

			/**
			 * @brief Returns the prefix matched by an extension-less MSP file request.
			 * @param fileName Catalog file name.
			 * @return Portion before the first extension separator.
			 */
			[[nodiscard]] static QString catalogFileStem(const QString &fileName)
			{
				const qsizetype separator = fileName.indexOf(QLatin1Char('.'));
				return separator > 0 ? fileName.first(separator) : fileName;
			}

			/** @brief Invalidates every retained file-source key after a global routing or device change. */
			void invalidateCachedFileSources()
			{
				m_cachedFileSourceKeys.clear();
				for (WorkerSession &session : m_sessions)
					clearCachedFileRequests(session);
			}

			/**
			 * @brief Removes every request-to-source association for one session.
			 * @param session Session whose request cache is cleared.
			 */
			static void clearCachedFileRequests(WorkerSession &session)
			{
				session.cachedFileRequestKeys.clear();
				session.cachedFileRequestsBySource.clear();
			}

			/**
			 * @brief Removes one request and its reverse source association.
			 * @param session Session owning the cached request.
			 * @param request Exact request key to remove.
			 */
			static void removeCachedFileRequest(WorkerSession &session, const QString &request)
			{
				const auto cached = session.cachedFileRequestKeys.find(request);
				if (cached == session.cachedFileRequestKeys.end())
					return;
				session.cachedFileRequestsBySource.remove(soundCatalogComparisonKey(cached->identity),
				                                          request);
				session.cachedFileRequestKeys.erase(cached);
			}

			/**
			 * @brief Caches one request and records its exact source for direct invalidation.
			 * @param session Session owning the request.
			 * @param request Original request text.
			 * @param key Resolved source identity.
			 */
			static void cacheFileRequest(WorkerSession &session, const QString &request,
			                             const SoundSourceKey &key)
			{
				removeCachedFileRequest(session, request);
				session.cachedFileRequestKeys.insert(request, key);
				session.cachedFileRequestsBySource.insert(soundCatalogComparisonKey(key.identity), request);
			}

			/**
			 * @brief Invalidates retained source keys that resolve to one removed or replaced file.
			 * @param absolutePath Canonical file identity becoming stale.
			 */
			void invalidateCachedFileSource(const QString &absolutePath)
			{
				const QString sourceKey = soundCatalogComparisonKey(absolutePath);
				m_cachedFileSourceKeys.remove(sourceKey);
				for (WorkerSession &session : m_sessions)
				{
					const QList<QString> requests = session.cachedFileRequestsBySource.values(sourceKey);
					session.cachedFileRequestsBySource.remove(sourceKey);
					for (const QString &request : requests)
					{
						const auto cached = session.cachedFileRequestKeys.find(request);
						if (cached != session.cachedFileRequestKeys.end() &&
						    soundCatalogComparisonKey(cached->identity) == sourceKey)
						{
							session.cachedFileRequestKeys.erase(cached);
						}
					}
				}
			}

			/**
			 * @brief Erases one file from catalog lookup indexes without touching source caches.
			 * @param absolutePath Canonical file path used as the primary catalog key.
			 */
			void eraseCatalogFileRecord(const QString &absolutePath)
			{
				const auto it = m_catalogFiles.find(absolutePath);
				if (it == m_catalogFiles.end())
					return;
				const QString relativeKey = soundCatalogComparisonKey(it->relativePath);
				if (const auto relative = m_catalogFileByRelativePath.constFind(relativeKey);
				    relative != m_catalogFileByRelativePath.cend() && *relative == absolutePath)
				{
					m_catalogFileByRelativePath.remove(relativeKey);
				}
				m_catalogFilesByName.remove(soundCatalogComparisonKey(it->fileName), absolutePath);
				m_catalogFilesByStem.remove(soundCatalogComparisonKey(catalogFileStem(it->fileName)),
				                            absolutePath);
				const QString directory =
				    QMudPluginPathUtils::normalizeSeparators(QFileInfo(it->absolutePath).absolutePath());
				if (auto directoryIt = m_catalogDirectories.find(directory);
				    directoryIt != m_catalogDirectories.end())
				{
					directoryIt->files.remove(absolutePath);
				}
				m_catalogFiles.erase(it);
			}

			/**
			 * @brief Removes one file from every catalog index.
			 * @param absolutePath Canonical file path used as the primary catalog key.
			 */
			void removeCatalogFile(const QString &absolutePath)
			{
				if (!m_catalogFiles.contains(absolutePath))
					return;
				invalidateCachedFileSource(absolutePath);
				eraseCatalogFileRecord(absolutePath);
			}

			/**
			 * @brief Adds or replaces one supported regular file in the worker catalog.
			 * @param info Existing file metadata.
			 * @return Canonical indexed path, or an empty string when the file is not indexable.
			 */
			[[nodiscard]] QString indexCatalogFile(const QFileInfo &info)
			{
				if (!info.exists() || !info.isFile() || info.isSymLink() || !info.isReadable() ||
				    !soundPlaybackSupportsFile(info.fileName()))
				{
					return {};
				}
				const QString absolute = QMudPluginPathUtils::normalizeSeparators(info.canonicalFilePath());
				if (absolute.isEmpty() ||
				    !QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(absolute, m_soundsRoot))
				{
					return {};
				}
				return indexCatalogPath(absolute);
			}

			/**
			 * @brief Adds a path already validated by the I/O worker to the lookup catalog.
			 * @param absolutePath Canonical file path beneath the sounds root.
			 * @return Normalized indexed path, or an empty string when lexically invalid.
			 */
			[[nodiscard]] QString indexCatalogPath(const QString &absolutePath)
			{
				const QString normalized = QMudPluginPathUtils::normalizeSeparators(absolutePath);
				return indexCatalogPathAfterCacheInvalidation(normalized);
			}

			/**
			 * @brief Adds a validated path after any stale exact-file cache entry was invalidated.
			 * @param absolutePath Canonical file path beneath the sounds root.
			 * @param knownDirectory Validated direct parent supplied by a directory record, when available.
			 * @return Normalized indexed path, or an empty string when lexically invalid.
			 */
			[[nodiscard]] QString indexCatalogPathAfterCacheInvalidation(const QString &absolutePath,
			                                                             const QString &knownDirectory = {})
			{
				QString absolute = QMudPluginPathUtils::normalizeSeparators(absolutePath);
				if (absolute.isEmpty() || m_soundsRoot.isEmpty() ||
				    !QMudPluginPathUtils::pathIsWithinOrEqualTo(absolute, m_soundsRoot) ||
				    !soundPlaybackSupportsFile(absolute))
				{
					return {};
				}
				const QFileInfo info(absolute);
				const QString directory = knownDirectory.isEmpty()
				                              ? QMudPluginPathUtils::normalizeSeparators(info.absolutePath())
				                              : QMudPluginPathUtils::normalizeSeparators(knownDirectory);
				SoundCatalogFile record;
				record.absolutePath = absolute;
				record.relativePath =
				    QMudPluginPathUtils::normalizeSeparators(QDir(m_soundsRoot).relativeFilePath(absolute));
				record.fileName = info.fileName();
				eraseCatalogFileRecord(absolute);
				m_catalogFiles.insert(absolute, std::move(record));
				const SoundCatalogFile &indexed = m_catalogFiles[absolute];
				m_catalogFileByRelativePath.insert(soundCatalogComparisonKey(indexed.relativePath), absolute);
				m_catalogFilesByName.insert(soundCatalogComparisonKey(indexed.fileName), absolute);
				m_catalogFilesByStem.insert(soundCatalogComparisonKey(catalogFileStem(indexed.fileName)),
				                            absolute);
				m_catalogDirectories[directory].files.insert(absolute);
				return absolute;
			}

			/**
			 * @brief Installs one playback-side directory record and its direct parent relationship.
			 * @param directory Catalog directory beneath the sounds root.
			 * @param parent Direct parent directory, or empty for the sounds root.
			 */
			void ensureCatalogDirectory(const QString &directory, const QString &parent)
			{
				const QString normalized              = QMudPluginPathUtils::normalizeSeparators(directory);
				const QString normalizedParent        = QMudPluginPathUtils::normalizeSeparators(parent);
				SoundPlaybackCatalogDirectory &record = m_catalogDirectories[normalized];
				record.parent                         = normalizedParent;
				if (!normalizedParent.isEmpty())
					m_catalogDirectories[normalizedParent].children.insert(normalized);
			}

			/**
			 * @brief Installs one bounded persistent-index batch and acknowledges its application.
			 * @param command I/O result containing validated paths from one bounded read.
			 */
			void handleCatalogIndexBatch(const SoundCommand &command)
			{
				if (command.token != m_catalogGeneration)
					return;
				m_maximumIndexBatchSize = qMax(m_maximumIndexBatchSize, command.files.size());
				ensureCatalogDirectory(command.text, command.parentDirectory);
				for (const QString &file : command.files)
					static_cast<void>(indexCatalogPathAfterCacheInvalidation(file, command.text));
				if (m_ioWorker)
				{
					QMetaObject::invokeMethod(
					    m_ioWorker, [worker = m_ioWorker, generation = command.token]
					    { worker->acknowledgeIndexBatch(generation); }, Qt::QueuedConnection);
				}
			}

			/**
			 * @brief Completes persistent-index installation or rejects every partial batch.
			 * @param command Completion result for the entire declared index.
			 */
			void handleCatalogIndexLoaded(const SoundCommand &command)
			{
				if (command.token != m_catalogGeneration)
					return;
				if (!command.success)
				{
					m_catalogFiles.clear();
					m_catalogFileByRelativePath.clear();
					m_catalogFilesByName.clear();
					m_catalogFilesByStem.clear();
					m_catalogDirectories.clear();
					invalidateCachedFileSources();
				}
				m_catalogAvailable = command.success;
				if (m_catalogAvailable)
					resumePendingNativeResolutions();
			}

			/**
			 * @brief Applies and acknowledges one bounded directory reconciliation batch.
			 * @param command Current files and structural changes for one directory.
			 */
			void handleCatalogDirectoryBatch(const SoundCommand &command)
			{
				if (command.token != m_catalogGeneration || command.text.isEmpty())
					return;
				const qsizetype batchSize   = command.files.size() + command.changedFiles.size() +
				                              command.removedFiles.size() + command.directories.size() +
				                              command.removedDirectories.size();
				m_maximumDirectoryBatchSize = qMax(m_maximumDirectoryBatchSize, batchSize);
				const QString directory     = QMudPluginPathUtils::normalizeSeparators(command.text);
				ensureCatalogDirectory(directory, command.parentDirectory);
				for (const QString &file : command.removedFiles)
					removeCatalogFile(file);
				for (const QString &file : command.changedFiles)
				{
					invalidateCachedFileSource(file);
					static_cast<void>(indexCatalogPathAfterCacheInvalidation(file, directory));
				}
				for (const QString &file : command.files)
					static_cast<void>(indexCatalogPathAfterCacheInvalidation(file, directory));
				for (const QString &child : command.directories)
					ensureCatalogDirectory(child, directory);
				for (const QString &removed : command.removedDirectories)
					eraseCatalogDirectoryRecord(removed);
				if (m_ioWorker)
				{
					QMetaObject::invokeMethod(
					    m_ioWorker,
					    [worker = m_ioWorker, generation = command.token, directory = command.text]
					    { worker->acknowledgeDirectoryBatch(generation, directory); }, Qt::QueuedConnection);
				}
			}

			/**
			 * @brief Completes one already-applied directory reconciliation.
			 * @param command Directory presence and watcher result.
			 */
			void handleCatalogDirectoryScanned(const SoundCommand &command)
			{
				if (command.token != m_catalogGeneration || command.text.isEmpty())
					return;
				const QString directory = QMudPluginPathUtils::normalizeSeparators(command.text);
				if (command.success)
					ensureCatalogDirectory(directory, sameSoundFilesystemPath(directory, m_soundsRoot)
					                                      ? QString()
					                                      : QMudPluginPathUtils::normalizeSeparators(
					                                            QFileInfo(directory).absolutePath()));
				if (command.directoryWatched)
					m_watchedCatalogDirectories.insert(directory);
				else
					m_watchedCatalogDirectories.remove(directory);
			}

			/**
			 * @brief Erases one catalog directory after its files have been removed.
			 * @param directory Canonical directory removed by reconciliation.
			 */
			void eraseCatalogDirectoryRecord(const QString &directory)
			{
				m_watchedCatalogDirectories.remove(directory);
				const auto    record = m_catalogDirectories.constFind(directory);
				const QString parent =
				    record == m_catalogDirectories.cend()
				        ? QMudPluginPathUtils::normalizeSeparators(QFileInfo(directory).absolutePath())
				        : record->parent;
				m_catalogDirectories.remove(directory);
				if (auto parentIt = m_catalogDirectories.find(parent); parentIt != m_catalogDirectories.end())
				{
					parentIt->children.remove(directory);
				}
			}

			/**
			 * @brief Applies exact-file changes from one directly watched plugin resource directory.
			 * @param command I/O result containing supported direct audio-file changes only.
			 */
			void handlePluginDirectoryChanged(const SoundCommand &command)
			{
				if (command.token != m_pluginDirectoryGeneration || command.text.isEmpty() ||
				    m_pluginsRoot.isEmpty())
					return;
				const QString directory = QMudPluginPathUtils::normalizeSeparators(command.text);
				m_pendingPluginDirectories.remove(directory);
				if (command.success)
					m_trackedPluginDirectories.insert(directory);
				else
					m_trackedPluginDirectories.remove(directory);
				if (command.directoryWatched)
					m_watchedPluginDirectories.insert(directory);
				else
					m_watchedPluginDirectories.remove(directory);

				auto removeFile = [this](const QString &file)
				{
					invalidateCachedFileSource(file);
					const QString relative =
					    QMudPluginPathUtils::normalizeSeparators(QDir(m_pluginsRoot).relativeFilePath(file));
					const QString key     = soundCatalogComparisonKey(relative);
					const auto    indexed = m_pluginFileByRelativePath.constFind(key);
					if (indexed != m_pluginFileByRelativePath.cend() &&
					    sameSoundFilesystemPath(*indexed, file))
					{
						m_pluginFileByRelativePath.remove(key);
					}
				};
				for (const QString &file : command.removedFiles)
					removeFile(file);
				for (const QString &file : command.changedFiles)
				{
					removeFile(file);
					indexPluginSoundFile(file);
				}
				for (const QString &file : command.files)
				{
					invalidateCachedFileSource(file);
					indexPluginSoundFile(file);
				}
			}

			/**
			 * @brief Clears playback lookup state after the I/O worker loses the sounds root.
			 * @param command Reset result carrying readiness state.
			 */
			void handleCatalogCleared(const SoundCommand &command)
			{
				if (command.token != m_catalogGeneration)
					return;
				m_catalogFiles.clear();
				m_catalogFileByRelativePath.clear();
				m_catalogFilesByName.clear();
				m_catalogFilesByStem.clear();
				m_catalogDirectories.clear();
				m_watchedCatalogDirectories.clear();
				invalidateCachedFileSources();
				m_catalogAvailable = true;
				m_catalogReady     = command.success;
				if (m_catalogReady)
					resumePendingNativeResolutions();
			}

			/**
			 * @brief Replaces the process-wide catalog root and starts its lazy initial scan.
			 * @param qmudHomeDirectory Resolved QMud home directory.
			 */
			void configureSoundDirectory(const QString &qmudHomeDirectory)
			{
				const QString canonicalHome = QMudPluginPathUtils::normalizeSeparators(
				    QFileInfo(qmudHomeDirectory).canonicalFilePath());
				if (canonicalHome.isEmpty() ||
				    (m_catalogConfigured && sameSoundFilesystemPath(canonicalHome, m_qmudHome)))
				{
					return;
				}

				++m_catalogGeneration;
				if (m_catalogGeneration == 0)
					++m_catalogGeneration;
				advancePluginDirectoryGeneration();
				m_catalogFiles.clear();
				m_catalogFileByRelativePath.clear();
				invalidateCachedFileSources();
				m_catalogFilesByName.clear();
				m_catalogFilesByStem.clear();
				m_catalogDirectories.clear();
				m_watchedCatalogDirectories.clear();
				m_pluginFileByRelativePath.clear();
				m_watchedPluginDirectories.clear();
				m_trackedPluginDirectories.clear();
				m_pendingPluginDirectories.clear();
				m_qmudHome = canonicalHome;
				m_soundsPath.clear();
				m_soundsRoot.clear();
				m_pluginsRoot.clear();
				m_catalogConfigured             = true;
				m_catalogAvailable              = false;
				m_catalogReady                  = false;
				m_watchedDirectoryCount         = 0;
				m_failedWatchCount              = 0;
				m_maximumIndexBatchSize         = 0;
				m_maximumDirectoryBatchSize     = 0;
				m_maximumReconciliationStepSize = 0;
				m_watchStatePathEntryCount      = 0;
				m_publishedCatalogRevision      = 0;
				auto configureIo                = [this]
				{
					if (!m_ioWorker)
						return;
					QMetaObject::invokeMethod(
					    m_ioWorker,
					    [worker = m_ioWorker, home = m_qmudHome, generation = m_catalogGeneration,
					     pluginGeneration = m_pluginDirectoryGeneration]
					    { worker->configureSoundDirectory(home, generation, pluginGeneration); },
					    Qt::QueuedConnection);
				};

				QString soundsPath;
				QString error;
				if (!QMudPluginPathUtils::resolveInsideQmudHome(m_qmudHome, QStringLiteral("sounds"),
				                                                &soundsPath, &error) ||
				    !QDir().mkpath(soundsPath) || soundPathContainsSymlink(m_qmudHome, soundsPath))
				{
					refreshWorldSoundDirectories();
					configureIo();
					return;
				}
				m_soundsPath = QMudPluginPathUtils::normalizeSeparators(soundsPath);
				m_soundsRoot =
				    QMudPluginPathUtils::normalizeSeparators(QFileInfo(soundsPath).canonicalFilePath());
				if (m_soundsRoot.isEmpty() ||
				    !QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(m_soundsRoot, m_qmudHome))
				{
					refreshWorldSoundDirectories();
					configureIo();
					return;
				}
				refreshWorldSoundDirectories();
				configureIo();
			}

			/** @brief Advances the independent plugin-root generation without permitting zero. */
			void advancePluginDirectoryGeneration()
			{
				++m_pluginDirectoryGeneration;
				if (m_pluginDirectoryGeneration == 0)
					++m_pluginDirectoryGeneration;
			}

			/**
			 * @brief Applies a configured global plugins directory without restarting sounds-tree work.
			 * @param pluginsDirectory Directory selected by global preferences.
			 */
			void configurePluginDirectory(const QString &pluginsDirectory)
			{
				QString canonicalPlugins;
				if (!m_qmudHome.isEmpty() && !pluginsDirectory.trimmed().isEmpty() &&
				    !QMudPluginPathUtils::pluginDirectoryConflictsWithSoundStorage(m_qmudHome,
				                                                                   pluginsDirectory))
				{
					QString resolved;
					QString error;
					if (QMudPluginPathUtils::resolveInsideQmudHome(m_qmudHome, pluginsDirectory, &resolved,
					                                               &error))
					{
						const QFileInfo info(resolved);
						const QString   canonical =
						    QMudPluginPathUtils::normalizeSeparators(info.canonicalFilePath());
						if (info.isDir() && !info.isSymLink() && !canonical.isEmpty() &&
						    QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(canonical, m_qmudHome) &&
						    !soundPathContainsSymlink(m_qmudHome, resolved))
						{
							canonicalPlugins = canonical;
						}
					}
				}
				if (sameSoundFilesystemPath(canonicalPlugins, m_pluginsRoot))
					return;

				for (const QString &file : std::as_const(m_pluginFileByRelativePath))
					invalidateCachedFileSource(file);
				m_pluginFileByRelativePath.clear();
				m_watchedPluginDirectories.clear();
				m_trackedPluginDirectories.clear();
				m_pendingPluginDirectories.clear();
				m_pluginsRoot = canonicalPlugins;
				advancePluginDirectoryGeneration();
				if (m_ioWorker)
				{
					QMetaObject::invokeMethod(
					    m_ioWorker,
					    [worker = m_ioWorker, directory = m_pluginsRoot,
					     generation = m_pluginDirectoryGeneration]
					    { worker->configurePluginDirectory(directory, generation); },
					    Qt::QueuedConnection);
				}
			}

			/**
			 * @brief Resolves one protocol pattern with a preferred per-world catalog prefix.
			 * @param filePattern Protocol-provided file name or wildcard pattern.
			 * @param worldSoundCatalogPrefix Directory below sounds searched before the shared root.
			 * @return Stable sorted absolute candidates, or an empty list.
			 */
			[[nodiscard]] QStringList resolveNativeSoundFiles(const QString &filePattern,
			                                                  const QString &worldSoundCatalogPrefix) const
			{
				const QString requested = normalizeNativeSoundPattern(filePattern);
				if (requested.isEmpty())
					return {};
				const QString   preferredRequest   = worldSoundCatalogPrefix.isEmpty()
				                                         ? requested
				                                         : QDir(worldSoundCatalogPrefix).filePath(requested);
				const qsizetype leafStart          = requested.lastIndexOf(QLatin1Char('/')) + 1;
				const QString   requestedDirectory = requested.left(qMax<qsizetype>(0, leafStart - 1));
				const QString   preferredDirectory =
				    QFileInfo(preferredRequest).path() == QLatin1String(".")
				        ? QString()
				        : QMudPluginPathUtils::normalizeSeparators(QFileInfo(preferredRequest).path());
				const QString leaf   = requested.mid(leafStart);
				const QString suffix = QFileInfo(leaf).suffix();
				const bool    wildcardSuffix =
				    suffix.contains(QLatin1Char('*')) || suffix.contains(QLatin1Char('?'));
				if (!suffix.isEmpty() && !wildcardSuffix && !soundPlaybackSupportsFile(leaf))
					return {};
				const QString pattern =
				    (suffix.isEmpty() ? leaf + QStringLiteral(".*") : leaf).toCaseFolded();

				const bool  wildcard = leaf.contains(QLatin1Char('*')) || leaf.contains(QLatin1Char('?'));
				QStringList candidates;
				if (!wildcard && !suffix.isEmpty())
				{
					const auto preferred =
					    m_catalogFileByRelativePath.constFind(soundCatalogComparisonKey(preferredRequest));
					if (preferred != m_catalogFileByRelativePath.cend())
						return {*preferred};
					const auto direct =
					    m_catalogFileByRelativePath.constFind(soundCatalogComparisonKey(requested));
					if (direct != m_catalogFileByRelativePath.cend())
						return {*direct};
					candidates = m_catalogFilesByName.values(soundCatalogComparisonKey(leaf));
				}
				else if (!wildcard)
				{
					candidates = m_catalogFilesByStem.values(soundCatalogComparisonKey(leaf));
				}
				else
				{
					candidates.reserve(m_catalogFiles.size());
					for (auto it = m_catalogFiles.cbegin(); it != m_catalogFiles.cend(); ++it)
						candidates.push_back(it.key());
				}

				QStringList preferredMatches;
				QStringList directMatches;
				QStringList fallbackMatches;
				for (const QString &candidate : std::as_const(candidates))
				{
					const auto fileIt = m_catalogFiles.constFind(candidate);
					if (fileIt == m_catalogFiles.cend())
						continue;
					const SoundCatalogFile &file = fileIt.value();
					if (!QDir::match(pattern, file.fileName.toCaseFolded()))
						continue;
					fallbackMatches.push_back(file.absolutePath);
					const QString fileDirectory = QFileInfo(file.relativePath).path();
					const QString normalizedDirectory =
					    fileDirectory == QLatin1String(".") ? QString() : fileDirectory;
					if (sameSoundFilesystemPath(normalizedDirectory, preferredDirectory))
						preferredMatches.push_back(file.absolutePath);
					if (sameSoundFilesystemPath(normalizedDirectory, requestedDirectory))
						directMatches.push_back(file.absolutePath);
				}
				QStringList &matches = !preferredMatches.isEmpty() ? preferredMatches
				                       : !directMatches.isEmpty()  ? directMatches
				                                                   : fallbackMatches;
				std::ranges::sort(matches);
				return matches;
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

			/**
			 * @brief Ensures the configured sounds root exists after external removal.
			 * @return `true` when a contained root is available.
			 */
			[[nodiscard]] bool ensureSoundsRoot()
			{
				if (!m_soundsRoot.isEmpty() && QFileInfo(m_soundsRoot).isDir())
					return true;
				if (m_soundsPath.isEmpty() || !QDir().mkpath(m_soundsPath))
					return false;
				if (soundPathContainsSymlink(m_qmudHome, m_soundsPath))
					return false;
				const QString canonicalRoot =
				    QMudPluginPathUtils::normalizeSeparators(QFileInfo(m_soundsPath).canonicalFilePath());
				if (canonicalRoot.isEmpty() ||
				    !QMudPluginPathUtils::canonicalPathIsWithinOrEqualTo(canonicalRoot, m_qmudHome))
				{
					return false;
				}
				m_soundsRoot = canonicalRoot;
				if (m_ioWorker)
				{
					QMetaObject::invokeMethod(
					    m_ioWorker,
					    [worker = m_ioWorker, home = m_qmudHome, generation = m_catalogGeneration,
					     pluginsRoot = m_pluginsRoot, pluginGeneration = m_pluginDirectoryGeneration]
					    {
						    worker->configureSoundDirectory(home, generation, pluginGeneration);
						    worker->configurePluginDirectory(pluginsRoot, pluginGeneration);
					    },
					    Qt::QueuedConnection);
				}
				return true;
			}

			/**
			 * @brief Builds a contained download destination and remote URL for one missing protocol file.
			 * @param filePattern Valid protocol file pattern.
			 * @param baseUrl Explicit or protocol-default base URL.
			 * @param music Whether the request uses the MUSIC default extension.
			 * @param worldSoundCatalogPrefix Directory below sounds used for the installed file.
			 * @param destination Destination receiving the absolute sounds-tree path.
			 * @param requestUrl Destination receiving the composed HTTP(S) URL.
			 * @return `true` when the request can be downloaded safely.
			 */
			[[nodiscard]] bool prepareNativeDownload(const QString &filePattern, const QString &baseUrl,
			                                         const bool music, const QString &worldSoundCatalogPrefix,
			                                         QString &destination, QUrl &requestUrl)
			{
				QString relative = normalizeNativeSoundPattern(filePattern);
				if (relative.isEmpty() || relative.contains(QLatin1Char('*')) ||
				    relative.contains(QLatin1Char('?')) || baseUrl.trimmed().isEmpty() ||
				    m_soundsPath.isEmpty() || !ensureSoundsRoot())
				{
					return false;
				}
				if (QFileInfo(relative).suffix().isEmpty())
					relative += music ? QStringLiteral(".mid") : QStringLiteral(".wav");
				if (!soundPlaybackSupportsFile(relative))
					return false;

				QUrl base(baseUrl.trimmed(), QUrl::StrictMode);
				if (!base.isValid() || base.host().isEmpty() ||
				    (base.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) != 0 &&
				     base.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0))
				{
					return false;
				}
				QString basePath = base.path();
				if (!basePath.endsWith(QLatin1Char('/')))
					basePath += QLatin1Char('/');
				base.setPath(basePath);
				base.setQuery({});
				base.setFragment({});
				requestUrl = base.resolved(QUrl::fromEncoded(
				    QUrl::toPercentEncoding(relative, QByteArrayLiteral("/")), QUrl::StrictMode));
				if (!requestUrl.isValid() ||
				    (requestUrl.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) != 0 &&
				     requestUrl.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0))
				{
					return false;
				}

				QString       error;
				const QString destinationRelative = worldSoundCatalogPrefix.isEmpty()
				                                        ? relative
				                                        : QDir(worldSoundCatalogPrefix).filePath(relative);
				if (!QMudPluginPathUtils::resolveInsideDirectory(m_soundsRoot, destinationRelative,
				                                                 &destination, &error))
				{
					return false;
				}
				if (soundPathContainsSymlink(m_soundsRoot, destination))
					return false;
				return true;
			}

			/**
			 * @brief Reserves one channel while catalog completion or a download is pending.
			 * @param session Session receiving the reservation.
			 * @param channelIndex Zero-based channel selected for the request.
			 * @param command Native protocol request.
			 */
			void reserveNativeResolution(WorkerSession &session, const int channelIndex,
			                             const SoundCommand &command)
			{
				releaseChannel(session, channelIndex);
				SoundChannel &channel            = session.channels[static_cast<std::size_t>(channelIndex)];
				channel.assignmentGeneration     = session.nextAssignmentGeneration++;
				channel.ownershipGeneration      = command.ownershipGeneration;
				channel.ownershipKind            = SoundOwnershipKind::NativeSound;
				channel.repeatCount              = command.repeatCount;
				channel.remainingPlays           = command.repeatCount < 0 ? -1 : command.repeatCount - 1;
				channel.allocated                = true;
				channel.looping                  = command.repeatCount < 0;
				channel.volume                   = normalizeSoundVolume(command.volume);
				channel.pan                      = command.pan;
				channel.nativeMusic              = command.music;
				channel.nativeResolutionPending  = true;
				channel.nativeFilePattern        = command.text;
				channel.nativeDownloadUrl        = command.url;
				channel.nativeSoundCatalogPrefix = session.worldSoundCatalogPrefix;
				publishSnapshotIfChanged(session);
			}

			/**
			 * @brief Starts or joins an asynchronous download for one reserved native request.
			 * @param destination Contained final sound-file path.
			 * @param requestUrl Composed HTTP(S) request URL.
			 * @return `true` when the destination is available or has an active download.
			 */
			[[nodiscard]] bool startNativeSoundDownload(const QString &destination, const QUrl &requestUrl)
			{
				if (!m_ioWorker || !m_ioThread.isRunning())
					return false;
				QMetaObject::invokeMethod(
				    m_ioWorker,
				    [worker = m_ioWorker, generation = m_catalogGeneration, destination, requestUrl]
				    { worker->startDownload(generation, destination, requestUrl); }, Qt::QueuedConnection);
				return true;
			}

			/**
			 * @brief Applies one I/O-thread download result to every matching reserved channel.
			 * @param command Completed destination and availability result.
			 */
			void handleDownloadFinished(const SoundCommand &command)
			{
				if (command.token != m_catalogGeneration || command.text.isEmpty())
					return;
				for (auto sessionIt = m_sessions.begin(); sessionIt != m_sessions.end(); ++sessionIt)
				{
					WorkerSession &session = sessionIt.value();
					for (int channelIndex = 0; channelIndex < SoundEngineSession::kChannelCount;
					     ++channelIndex)
					{
						SoundChannel &channel = session.channels[static_cast<std::size_t>(channelIndex)];
						if (channel.nativeResolutionPending &&
						    sameSoundFilesystemPath(channel.nativeDownloadDestination, command.text))
						{
							if (command.success)
								resumePendingNativeResolution(session, channelIndex);
							else
							{
								releaseChannel(session, channelIndex);
								publishSnapshotIfChanged(session);
							}
						}
					}
				}
			}

			/**
			 * @brief Resolves or downloads one reserved native request and starts it on the same channel.
			 * @param session Session owning the pending channel.
			 * @param channelIndex Zero-based pending channel.
			 */
			void resumePendingNativeResolution(WorkerSession &session, const int channelIndex)
			{
				SoundChannel &channel = session.channels[static_cast<std::size_t>(channelIndex)];
				if (!channel.nativeResolutionPending ||
				    channel.ownershipKind != SoundOwnershipKind::NativeSound)
					return;
				QStringList files =
				    resolveNativeSoundFiles(channel.nativeFilePattern, channel.nativeSoundCatalogPrefix);
				if (files.isEmpty())
				{
					if (!m_catalogReady)
						return;
					QString destination;
					QUrl    requestUrl;
					if (!prepareNativeDownload(channel.nativeFilePattern, channel.nativeDownloadUrl,
					                           channel.nativeMusic, channel.nativeSoundCatalogPrefix,
					                           destination, requestUrl))
					{
						releaseChannel(session, channelIndex);
						publishSnapshotIfChanged(session);
						return;
					}
					channel.nativeDownloadDestination = destination;
					if (!startNativeSoundDownload(destination, requestUrl))
					{
						releaseChannel(session, channelIndex);
						publishSnapshotIfChanged(session);
					}
					return;
				}

				SoundCommand command;
				command.kind                = SoundCommandKind::PlayFile;
				command.sessionId           = session.sessionId;
				command.buffer              = channelIndex + 1;
				command.files               = std::move(files);
				command.repeatCount         = channel.repeatCount;
				command.volume              = (channel.volume - 1.0) * 100.0;
				command.pan                 = channel.pan;
				command.ownershipGeneration = channel.ownershipGeneration;
				command.ownershipKind       = SoundOwnershipKind::NativeSound;
				command.requireReservation  = true;
				int selectedBuffer          = 0;
				static_cast<void>(playFile(command, selectedBuffer));
			}

			/** @brief Resumes every channel waiting for a definitive catalog lookup or completed download. */
			void resumePendingNativeResolutions()
			{
				for (auto sessionIt = m_sessions.begin(); sessionIt != m_sessions.end(); ++sessionIt)
				{
					WorkerSession &session = sessionIt.value();
					for (int channelIndex = 0; channelIndex < SoundEngineSession::kChannelCount;
					     ++channelIndex)
						resumePendingNativeResolution(session, channelIndex);
				}
			}

			/**
			 * @brief Maps a native protocol and media class to its independent worker-owned playback slot.
			 * @param protocol Protocol that originated the request.
			 * @param media Requested media class.
			 * @return Stable zero-based playback-slot index.
			 */
			[[nodiscard]] static std::size_t nativePlaybackIndex(const QMudNativeSoundProtocol protocol,
			                                                     const QMudNativeSoundMedia    media)
			{
				const std::size_t protocolOffset = protocol == QMudNativeSoundProtocol::Mxp ? 2U : 0U;
				return protocolOffset + (media == QMudNativeSoundMedia::Music ? 1U : 0U);
			}

			/**
			 * @brief Returns whether a logical native playback still owns its assigned worker channel.
			 * @param session Worker session containing both logical and physical playback state.
			 * @param playback Logical playback state to validate.
			 * @return `true` while the channel retains the matching native generation.
			 */
			[[nodiscard]] static bool nativePlaybackCurrent(const WorkerSession                 &session,
			                                                const WorkerSession::NativePlayback &playback)
			{
				if (playback.buffer < 1 || playback.buffer > SoundEngineSession::kChannelCount ||
				    playback.generation == 0)
				{
					return false;
				}
				const SoundChannel &channel = session.channels[static_cast<std::size_t>(playback.buffer - 1)];
				return channel.ownershipKind == SoundOwnershipKind::NativeSound &&
				       channel.ownershipGeneration == playback.generation;
			}

			/**
			 * @brief Stops one logical native playback while preserving a subsequently reassigned channel.
			 * @param session Worker session owning the playback.
			 * @param playback Logical playback state to stop and clear.
			 */
			void stopNativePlayback(WorkerSession &session, WorkerSession::NativePlayback &playback)
			{
				if (nativePlaybackCurrent(session, playback))
				{
					releaseChannel(session, playback.buffer - 1);
					publishSnapshotIfChanged(session);
				}
				playback = {};
			}

			/**
			 * @brief Executes one parsed native request entirely on the ordered sound worker.
			 * @param command Protocol-independent playback request and owning session.
			 */
			void executeNativeSoundRequest(const SoundCommand &command)
			{
				auto sessionIt = m_sessions.find(command.sessionId);
				if (sessionIt == m_sessions.end())
					return;
				WorkerSession             &session = sessionIt.value();
				const QMudNativeSoundMedia media =
				    command.music ? QMudNativeSoundMedia::Music : QMudNativeSoundMedia::Sound;
				WorkerSession::NativePlayback &playback =
				    session.nativePlayback[nativePlaybackIndex(command.nativeProtocol, media)];
				if (!nativePlaybackCurrent(session, playback))
					playback = {};
				if (command.stopNativePlayback)
				{
					stopNativePlayback(session, playback);
					return;
				}
				const bool soundIsPlaying =
				    media == QMudNativeSoundMedia::Sound && playback.buffer > 0 &&
				    session.channels[static_cast<std::size_t>(playback.buffer - 1)].playing;
				if (soundIsPlaying && command.priority <= playback.priority)
				{
					return;
				}

				const double volume = command.volume - 100.0;
				if (media == QMudNativeSoundMedia::Music && command.continueNativePlayback &&
				    playback.buffer > 0 &&
				    playback.filePattern.compare(command.text, Qt::CaseInsensitive) == 0)
				{
					if (updateChannel(session, playback.buffer - 1, command.repeatCount, volume, 0.0) == eOK)
						return;
					stopNativePlayback(session, playback);
				}

				do
				{
					++session.nextNativeSoundGeneration;
				} while (session.nextNativeSoundGeneration == 0);
				const quint64 generation = session.nextNativeSoundGeneration;
				SoundCommand  playbackCommand;
				playbackCommand.kind                = SoundCommandKind::PlayFile;
				playbackCommand.sessionId           = command.sessionId;
				playbackCommand.buffer              = playback.buffer;
				playbackCommand.text                = command.text;
				playbackCommand.url                 = command.url;
				playbackCommand.music               = command.music;
				playbackCommand.loop                = command.repeatCount < 0;
				playbackCommand.repeatCount         = command.repeatCount;
				playbackCommand.volume              = volume;
				playbackCommand.pan                 = 0.0;
				playbackCommand.ownershipGeneration = generation;
				playbackCommand.ownershipKind       = SoundOwnershipKind::NativeSound;
				int selectedBuffer                  = 0;
				if (beginNativeSoundRequest(playbackCommand, selectedBuffer) != eOK || selectedBuffer <= 0)
					return;
				playback.buffer      = selectedBuffer;
				playback.generation  = generation;
				playback.filePattern = command.text;
				playback.priority    = command.priority;
			}

			/**
			 * @brief Accepts one native request into local playback, initial-index waiting, or lazy download.
			 * @param command Native protocol request.
			 * @param selectedBuffer Destination receiving its reserved or playing channel.
			 * @return MUSHclient-compatible acceptance result.
			 */
			int beginNativeSoundRequest(const SoundCommand &command, int &selectedBuffer)
			{
				auto sessionIt = m_sessions.find(command.sessionId);
				if (sessionIt == m_sessions.end() || normalizeNativeSoundPattern(command.text).isEmpty())
					return eBadParameter;
				if (!m_catalogConfigured)
					return eFileNotFound;
				QStringList files;
				if (m_catalogAvailable)
					files = resolveNativeSoundFiles(command.text, sessionIt->worldSoundCatalogPrefix);
				if (files.isEmpty())
				{
					if (const auto exact =
					        resolveNativeExactFileBeforeCatalog(sessionIt.value(), command.text))
						files.push_back(*exact);
				}
				if (!files.isEmpty())
				{
					SoundCommand resolved = command;
					resolved.files        = files;
					const int result      = playFile(resolved, selectedBuffer);
					if (result != eFileNotFound)
						return result;
					for (const QString &file : std::as_const(files))
					{
						const QFileInfo info(file);
						if (!info.exists() || !info.isFile() || info.isSymLink() || !info.isReadable())
							removeCatalogFile(file);
					}
				}

				QString    destination;
				QUrl       requestUrl;
				const bool canDownload =
				    prepareNativeDownload(command.text, command.url, command.music,
				                          sessionIt->worldSoundCatalogPrefix, destination, requestUrl);
				if (m_catalogReady && !canDownload)
					return eFileNotFound;
				const int channelIndex = selectChannel(sessionIt.value(), command.buffer);
				if (channelIndex < 0)
					return eCannotPlaySound;
				reserveNativeResolution(sessionIt.value(), channelIndex, command);
				selectedBuffer = channelIndex + 1;
				if (!m_catalogReady)
					return eOK;

				SoundChannel &channel = sessionIt->channels[static_cast<std::size_t>(channelIndex)];
				channel.nativeDownloadDestination = destination;
				if (startNativeSoundDownload(destination, requestUrl))
					return eOK;
				releaseChannel(sessionIt.value(), channelIndex);
				publishSnapshotIfChanged(sessionIt.value());
				selectedBuffer = 0;
				return eFileNotFound;
			}

			int playFile(const SoundCommand &command, int &selectedBuffer)
			{
				auto sessionIt = m_sessions.find(command.sessionId);
				if (sessionIt == m_sessions.end())
					return eCannotPlaySound;
				WorkerSession &session = sessionIt.value();
				if (command.buffer >= 1 && command.buffer <= SoundEngineSession::kChannelCount &&
				    command.text.isEmpty() && command.files.isEmpty())
				{
					const SoundChannel &channel =
					    session.channels[static_cast<std::size_t>(command.buffer - 1)];
					if (command.ownershipKind != SoundOwnershipKind::None &&
					    (channel.ownershipKind != command.ownershipKind ||
					     channel.ownershipGeneration != command.ownershipGeneration))
					{
						return eCannotPlaySound;
					}
					const int result = updateChannel(session, command.buffer - 1, command.repeatCount,
					                                 command.volume, command.pan);
					if (result == eOK)
						selectedBuffer = command.buffer;
					return result;
				}
				if (command.ownershipKind == SoundOwnershipKind::NativeSound && command.files.isEmpty())
					return beginNativeSoundRequest(command, selectedBuffer);
				const bool nativeSound = command.ownershipKind == SoundOwnershipKind::NativeSound;
				if ((!nativeSound && command.text.isEmpty()) || (nativeSound && command.files.isEmpty()) ||
				    command.buffer < 0 || command.buffer > SoundEngineSession::kChannelCount)
					return eBadParameter;
				const int channelIndex = selectChannel(session, command.buffer);
				if (channelIndex < 0)
					return eCannotPlaySound;
				SoundChannel &channel          = session.channels[static_cast<std::size_t>(channelIndex)];
				const bool consumesReservation = command.requireReservation && command.buffer > 0 &&
				                                 channel.allocated && !channel.backend &&
				                                 channel.ownershipKind == command.ownershipKind &&
				                                 channel.ownershipGeneration == command.ownershipGeneration;
				const bool reservationOwnershipConfirmed =
				    consumesReservation && channel.luaAudioOwnershipConfirmed;
				if (command.requireReservation && !consumesReservation)
					return eCannotPlaySound;

				QString                       resolved;
				std::optional<SoundSourceKey> key;
				if (nativeSound)
				{
					const qsizetype start = QRandomGenerator::global()->bounded(command.files.size());
					for (qsizetype offset = 0; offset < command.files.size(); ++offset)
					{
						const QString &candidate = command.files.at((start + offset) % command.files.size());
						key                      = resolveFile(session, candidate, resolved);
						if (key)
						{
							break;
						}
					}
				}
				else
					key = resolveFile(session, command.text, resolved);
				if (!key)
				{
					const bool retainPendingNativeResolution = consumesReservation && nativeSound &&
					                                           !m_catalogReady &&
					                                           channel.nativeResolutionPending;
					if (consumesReservation && !retainPendingNativeResolution)
					{
						releaseChannel(session, channelIndex);
						publishSnapshotIfChanged(session);
					}
					return eFileNotFound;
				}
				if (channel.backend && channel.backend->key == *key)
				{
					if (channel.ownershipKind == SoundOwnershipKind::LuaAudio)
						finishLuaAudioOwnership(session, channelIndex, channel.ownershipGeneration,
						                        channel.luaAudioOwnershipConfirmed);
					SoundBackend *backend         = channel.backend;
					channel                       = {};
					backend->sessionId            = 0;
					backend->channel              = -1;
					backend->assignmentGeneration = 0;
					stopBackend(*backend);
					assignAndPlay(session, channelIndex, *backend, command, command.ownershipGeneration);
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
				assignAndPlay(session, channelIndex, *lease.backend, command, command.ownershipGeneration,
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
				channel.ownershipGeneration      = command.ownershipGeneration;
				channel.ownershipKind            = command.ownershipKind;
				channel.pendingMemoryOperationId = operationId;

				PendingMemoryPlay pending;
				pending.sessionId            = command.sessionId;
				pending.operationId          = operationId;
				pending.assignmentGeneration = channel.assignmentGeneration;
				pending.channelIndex         = channelIndex;
				pending.loop                 = command.loop;
				pending.volume               = command.volume;
				pending.pan                  = command.pan;
				pending.luaAudioGeneration   = command.ownershipGeneration;
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
				play.sessionId   = pending.sessionId;
				play.loop        = pending.loop;
				play.repeatCount = pending.loop ? -1 : 1;
				play.volume      = pending.volume;
				play.pan         = pending.pan;
				play.ownershipKind =
				    pending.luaAudioGeneration == 0 ? SoundOwnershipKind::None : SoundOwnershipKind::LuaAudio;
				channel = {};
				assignAndPlay(session, pending.channelIndex, *lease.backend, play,
				              pending.luaAudioGeneration);
				if (pending.completion)
					pending.completion->complete(eOK, pending.channelIndex + 1);
			}

			int reserveLuaAudioChannel(const SoundCommand &command, int &selectedBuffer)
			{
				auto sessionIt = m_sessions.find(command.sessionId);
				if (sessionIt == m_sessions.end() || command.text.isEmpty() ||
				    command.ownershipGeneration == 0)
					return eCannotPlaySound;
				WorkerSession &session = sessionIt.value();
				QString        resolved;
				if (!resolveFileInfo(session, command.text, resolved, true))
					return eFileNotFound;
				for (int channelIndex = 0; channelIndex < SoundEngineSession::kChannelCount; ++channelIndex)
				{
					const SoundChannel &candidate = session.channels[static_cast<std::size_t>(channelIndex)];
					const bool reusable = !candidate.allocated || (candidate.playbackStarted &&
					                                               !candidate.looping && !candidate.playing);
					if (!reusable)
						continue;
					releaseChannel(session, channelIndex);
					SoundChannel &reserved       = session.channels[static_cast<std::size_t>(channelIndex)];
					reserved.allocated           = true;
					reserved.ownershipGeneration = command.ownershipGeneration;
					reserved.ownershipKind       = SoundOwnershipKind::LuaAudio;
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
				    command.buffer > SoundEngineSession::kChannelCount || command.ownershipGeneration == 0)
					return eCannotPlaySound;
				WorkerSession &session = sessionIt.value();
				SoundChannel  &channel = session.channels[static_cast<std::size_t>(command.buffer - 1)];
				if (channel.ownershipKind == SoundOwnershipKind::LuaAudio &&
				    channel.ownershipGeneration == command.ownershipGeneration)
				{
					channel.luaAudioOwnershipConfirmed = true;
					return eOK;
				}
				const QPair<int, quint64> completed(command.buffer, command.ownershipGeneration);
				if (!session.pendingLuaAudioCompletions.removeOne(completed))
					return eCannotPlaySound;
				notifyLuaAudioOwnershipReleased(session, command.buffer, command.ownershipGeneration);
				return eOK;
			}

			static int updateChannel(WorkerSession &session, const int channelIndex, const int repeatCount,
			                         const double volume, const double pan)
			{
				SoundChannel &channel = session.channels[static_cast<std::size_t>(channelIndex)];
				if (!channel.backend)
				{
					if (!channel.nativeResolutionPending ||
					    channel.ownershipKind != SoundOwnershipKind::NativeSound)
					{
						return channel.allocated && channel.playbackStarted ? eCannotPlaySound
						                                                    : eBadParameter;
					}
					channel.repeatCount    = repeatCount;
					channel.remainingPlays = repeatCount < 0 ? -1 : repeatCount - 1;
					channel.looping        = repeatCount < 0;
					channel.volume         = normalizeSoundVolume(volume);
					channel.pan            = pan;
					publishSnapshotIfChanged(session);
					return eOK;
				}
				if (!channel.playing)
					return eCannotPlaySound;
				channel.repeatCount = repeatCount;
				channel.looping     = repeatCount < 0;
				if (channel.ownershipKind == SoundOwnershipKind::NativeSound)
				{
					channel.remainingPlays = repeatCount < 0 ? -1 : repeatCount - 1;
					channel.backendLooping = false;
				}
				else
					channel.backendLooping = repeatCount < 0;
				channel.volume = normalizeSoundVolume(volume);
				channel.pan    = pan;
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
				session.prepared.insert(command.token, {lease.backend, eOK, {}, lease.retainedAtMsecs});
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
					releaseUncommittedBackend(*prepared.backend, prepared.retainedAtMsecs);
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
				SoundBackend *backend         = preparedIt->backend;
				const qint64  retainedAtMsecs = preparedIt->retainedAtMsecs;
				sessionIt->prepared.erase(preparedIt);
				if (backend)
				{
					backend->preparationToken = 0;
					releaseUncommittedBackend(*backend, retainedAtMsecs);
				}
			}

			/**
			 * @brief Stops one channel or every channel, optionally guarded by native ownership.
			 * @param sessionId Worker session identity.
			 * @param buffer Fixed one-based channel, or zero for every channel.
			 * @param ownershipKind Expected native owner kind, or none for an unguarded stop.
			 * @param generation Expected native generation, or zero for an unguarded stop.
			 * @return MUSHclient-compatible status code.
			 */
			int stop(const quint64 sessionId, const int buffer, const SoundOwnershipKind ownershipKind,
			         const quint64 generation)
			{
				auto sessionIt = m_sessions.find(sessionId);
				if (sessionIt == m_sessions.end())
					return eCannotPlaySound;
				if (buffer < 0 || buffer > SoundEngineSession::kChannelCount)
					return eBadParameter;
				if (buffer == 0)
				{
					if (ownershipKind != SoundOwnershipKind::None || generation != 0)
						return eBadParameter;
					for (int i = 0; i < SoundEngineSession::kChannelCount; ++i)
						releaseChannel(sessionIt.value(), i);
					publishSnapshotIfChanged(sessionIt.value());
					return eOK;
				}
				const SoundChannel &channel = sessionIt->channels[static_cast<std::size_t>(buffer - 1)];
				if (ownershipKind != SoundOwnershipKind::None &&
				    (channel.ownershipKind != ownershipKind || channel.ownershipGeneration != generation))
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
						releaseUncommittedBackend(*prepared.backend, prepared.retainedAtMsecs);
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
					return {&backend, {}, 0};
				QString              failureMessage = std::move(backend.failureMessage);
				const SoundSourceKey key            = backend.key;
				const quint64        backendId      = backend.backendId;
				removeBackend(key, backendId);
				return {nullptr, std::move(failureMessage), 0};
			}

			/**
			 * @brief Leases a file-backed multimedia object and preserves synchronous initialization errors.
			 * @param key Resolved source identity.
			 * @return Backend lease and any initialization diagnostic.
			 */
			SoundBackendLease leaseBackend(const SoundSourceKey &key)
			{
				const qint64 now = currentTimeMsecs();
				if (const SoundCache::RetainedResult retained = m_cache.takeRetained(key, now);
				    retained.backend)
				{
					return {retained.backend, {}, retained.completedAtMsecs};
				}
				if (SoundBackend *candidate = m_cache.takeIdle(key, now))
					return {candidate, {}, 0};

				auto backend = takePreinitializedBackend();
				if (!backend)
					return {nullptr, QStringLiteral("No audio output device is available."), 0};
				backend->backendId                = m_nextBackendId++;
				backend->key                      = key;
				backend->initializationInProgress = true;
				const quint64 backendId           = backend->backendId;
				const auto inserted = m_cache.insertActive(key, key.size, backendId, std::move(backend), now);
				SoundBackend *result = inserted.backend;
				if (!result)
					return {};
				return initializeBackendSource(*result, key.identity);
			}

			/**
			 * @brief Leases a memory-backed media player and preserves synchronous initialization errors.
			 * @param key Content-addressed source identity.
			 * @param resource Materialized sound bytes retained by the backend.
			 * @return Backend lease and any initialization diagnostic.
			 */
			SoundBackendLease leaseMemoryBackend(const SoundSourceKey                       &key,
			                                     const std::shared_ptr<SoundMemoryResource> &resource)
			{
				const qint64 now = currentTimeMsecs();
				if (const SoundCache::RetainedResult retained = m_cache.takeRetained(key, now);
				    retained.backend)
				{
					if (retained.backend->memoryResource)
						m_memoryResources.insert(key.identity, retained.backend->memoryResource);
					return {retained.backend, {}, retained.completedAtMsecs};
				}
				if (SoundBackend *candidate = m_cache.takeIdle(key, now))
				{
					if (candidate->memoryResource)
						m_memoryResources.insert(key.identity, candidate->memoryResource);
					return {candidate, {}, 0};
				}

				if (!resource)
					return {};

				auto backend = takePreinitializedBackend();
				if (!backend)
					return {nullptr, QStringLiteral("No audio output device is available."), 0};
				backend->backendId                = m_nextBackendId++;
				backend->key                      = key;
				backend->memoryResource           = resource;
				backend->initializationInProgress = true;
				const quint64 backendId           = backend->backendId;
				const auto inserted = m_cache.insertActive(key, key.size, backendId, std::move(backend), now);
				SoundBackend *result = inserted.backend;
				if (!result)
					return {};
				SoundBackendLease lease = initializeBackendSource(*result, resource->fileName);
				if (!lease.backend)
					return lease;
				if (result->memoryResource)
					m_memoryResources.insert(key.identity, result->memoryResource);
				return lease;
			}

			/**
			 * @brief Connects a fresh backend to worker lifecycle handling and assigns its local source.
			 * @param backend Fresh cache-owned media-player backend.
			 * @param fileName Absolute local file supplied to the media player.
			 * @return Backend lease and any synchronous initialization diagnostic.
			 */
			SoundBackendLease initializeBackendSource(SoundBackend &backend, const QString &fileName)
			{
				Q_ASSERT(backend.player);
				Q_ASSERT(backend.audioOutput);
				connect(backend.player.get(), &QMediaPlayer::errorOccurred, this,
				        [this, raw = &backend](const QMediaPlayer::Error error, const QString &text)
				        { handlePlayerError(*raw, error, text); });
				connect(backend.player.get(), &QMediaPlayer::playbackStateChanged, this,
				        [this, raw = &backend](const QMediaPlayer::PlaybackState state)
				        { handlePlayerState(*raw, state); });
				backend.player->setSource(QUrl::fromLocalFile(fileName));
				return finishBackendInitialization(backend);
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

			/**
			 * @brief Attaches a backend to a channel and starts playback.
			 * @param session Worker session owning the channel.
			 * @param channelIndex Zero-based destination channel.
			 * @param backend Prepared backend to attach.
			 * @param command Playback parameters and ownership kind.
			 * @param ownershipGeneration Generation associated with the native owner.
			 * @param luaAudioOwnershipConfirmed Whether LuaAudio has confirmed its reserved ownership.
			 */
			void assignAndPlay(WorkerSession &session, const int channelIndex, SoundBackend &backend,
			                   const SoundCommand &command, const quint64 ownershipGeneration,
			                   const bool luaAudioOwnershipConfirmed = false)
			{
				SoundChannel &channel              = session.channels[static_cast<std::size_t>(channelIndex)];
				channel.backend                    = &backend;
				channel.assignmentGeneration       = session.nextAssignmentGeneration++;
				channel.ownershipGeneration        = ownershipGeneration;
				channel.ownershipKind              = command.ownershipKind;
				channel.luaAudioOwnershipConfirmed = luaAudioOwnershipConfirmed;
				channel.allocated                  = true;
				channel.playing                    = true;
				channel.playbackStarted            = false;
				channel.repeatCount                = command.repeatCount;
				channel.looping                    = command.repeatCount < 0;
				channel.backendLooping =
				    command.ownershipKind != SoundOwnershipKind::NativeSound && command.repeatCount < 0;
				if (command.ownershipKind == SoundOwnershipKind::NativeSound)
				{
					channel.playbackFiles  = command.files;
					channel.remainingPlays = command.repeatCount < 0 ? -1 : command.repeatCount - 1;
				}
				channel.volume               = normalizeSoundVolume(command.volume);
				channel.pan                  = command.pan;
				backend.sessionId            = command.sessionId;
				backend.channel              = channelIndex;
				backend.assignmentGeneration = channel.assignmentGeneration;
				backend.preparationToken     = 0;
				backend.started              = false;
				m_cache.noteUse(backend.key, currentTimeMsecs());
				applyBackendParameters(backend, channel);
				if (backend.player)
					backend.player->play();
				publishSnapshotIfChanged(session);
			}

			static void applyBackendParameters(const SoundBackend &backend, const SoundChannel &channel)
			{
				if (backend.player)
					backend.player->setLoops(channel.backendLooping ? QMediaPlayer::Infinite
					                                                : QMediaPlayer::Once);
				if (backend.audioOutput)
					backend.audioOutput->setVolume(static_cast<float>(channel.volume));
			}

			static void stopBackend(SoundBackend &backend)
			{
				if (backend.player)
					backend.player->stop();
				backend.started = false;
			}

			void releaseChannel(WorkerSession &session, const int channelIndex,
			                    const bool reportLuaAudioCompletion = true)
			{
				SoundChannel &channel      = session.channels[static_cast<std::size_t>(channelIndex)];
				const bool    stopPlayback = channel.playing;
				if (channel.pendingMemoryOperationId != 0)
					cancelPendingMemoryPlay(channel.pendingMemoryOperationId);
				if (reportLuaAudioCompletion && channel.ownershipKind == SoundOwnershipKind::LuaAudio)
					finishLuaAudioOwnership(session, channelIndex, channel.ownershipGeneration,
					                        channel.luaAudioOwnershipConfirmed);
				SoundBackend *backend = channel.backend;
				channel               = {};
				if (backend)
				{
					backend->sessionId            = 0;
					backend->channel              = -1;
					backend->assignmentGeneration = 0;
					if (stopPlayback)
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

			/**
			 * @brief Retains one naturally completed backend for immediate source-matched reuse.
			 * @param backend Completed backend becoming detached from playback.
			 * @param completedAtMsecs Positive monotonic completion timestamp.
			 */
			void retainCompletedBackend(SoundBackend &backend, const qint64 completedAtMsecs)
			{
				backend.sessionId            = 0;
				backend.channel              = -1;
				backend.assignmentGeneration = 0;
				backend.preparationToken     = 0;
				backend.started              = false;
				m_cache.markRetained(backend.key, backend.backendId, completedAtMsecs);
			}

			/**
			 * @brief Returns an uncommitted lease to its retained grace state or the idle cache.
			 * @param backend Backend whose preparation was canceled or could not be committed.
			 * @param retainedAtMsecs Original completion timestamp, or zero for an idle/new lease.
			 */
			void releaseUncommittedBackend(SoundBackend &backend, const qint64 retainedAtMsecs)
			{
				if (retainedAtMsecs > 0)
					retainCompletedBackend(backend, retainedAtMsecs);
				else
					releaseBackend(backend);
			}

			/**
			 * @brief Starts the next iteration of a native protocol sound on its existing channel.
			 * @param session Worker session owning the channel.
			 * @param channelIndex Zero-based channel whose current iteration completed.
			 * @return `true` when another candidate started successfully.
			 */
			bool startNextNativeSoundIteration(WorkerSession &session, const int channelIndex)
			{
				SoundChannel &channel = session.channels[static_cast<std::size_t>(channelIndex)];
				if (channel.ownershipKind != SoundOwnershipKind::NativeSound || channel.remainingPlays == 0 ||
				    channel.playbackFiles.isEmpty() || !channel.backend)
				{
					return false;
				}

				SoundBackend *completedBackend         = channel.backend;
				channel.backend                        = nullptr;
				channel.playbackStarted                = false;
				completedBackend->sessionId            = 0;
				completedBackend->channel              = -1;
				completedBackend->assignmentGeneration = 0;
				const qint64 completedAtMsecs          = currentTimeMsecs();
				retainCompletedBackend(*completedBackend, completedAtMsecs);

				const qsizetype start = QRandomGenerator::global()->bounded(channel.playbackFiles.size());
				QString         failureMessage;
				for (qsizetype offset = 0; offset < channel.playbackFiles.size(); ++offset)
				{
					const QString &candidate =
					    channel.playbackFiles.at((start + offset) % channel.playbackFiles.size());
					QString    resolved;
					const auto key = resolveFile(session, candidate, resolved);
					if (!key)
						continue;
					SoundBackendLease lease = leaseBackend(*key);
					if (!lease.backend)
					{
						if (!lease.failureMessage.isEmpty())
							failureMessage = std::move(lease.failureMessage);
						continue;
					}
					if (channel.remainingPlays > 0)
						--channel.remainingPlays;
					channel.backend                     = lease.backend;
					channel.assignmentGeneration        = session.nextAssignmentGeneration++;
					channel.playing                     = true;
					lease.backend->sessionId            = session.sessionId;
					lease.backend->channel              = channelIndex;
					lease.backend->assignmentGeneration = channel.assignmentGeneration;
					lease.backend->preparationToken     = 0;
					lease.backend->started              = false;
					m_cache.noteUse(lease.backend->key, currentTimeMsecs());
					applyBackendParameters(*lease.backend, channel);
					if (lease.backend->player)
						lease.backend->player->play();
					return true;
				}
				if (!failureMessage.isEmpty())
					reportPlaybackError(session.sessionId, failureMessage);
				return false;
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
				if (startNextNativeSoundIteration(session, channelIndex))
					return;
				if (!channel.backend)
				{
					channel = {};
					publishSnapshotIfChanged(session);
					return;
				}
				channel.playing                = false;
				SoundBackend *completedBackend = std::exchange(channel.backend, nullptr);
				const qint64  completedAtMsecs = currentTimeMsecs();
				m_cache.noteUse(completedBackend->key, completedAtMsecs);
				retainCompletedBackend(*completedBackend, completedAtMsecs);
				const quint64 ownershipGeneration = std::exchange(channel.ownershipGeneration, 0);
				const bool    ownershipConfirmed  = std::exchange(channel.luaAudioOwnershipConfirmed, false);
				const SoundOwnershipKind ownershipKind =
				    std::exchange(channel.ownershipKind, SoundOwnershipKind::None);
				publishSnapshotIfChanged(session);
				if (ownershipKind == SoundOwnershipKind::LuaAudio)
					finishLuaAudioOwnership(session, channelIndex, ownershipGeneration, ownershipConfirmed);
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
				if (state != QMediaPlayer::StoppedState || !channel.playbackStarted ||
				    channel.backendLooping || !channel.playing)
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
				const quint64            ownershipGeneration = channel.ownershipGeneration;
				const bool               ownershipConfirmed  = channel.luaAudioOwnershipConfirmed;
				const SoundOwnershipKind ownershipKind       = channel.ownershipKind;
				channel                                      = {};
				publishSnapshotIfChanged(sessionIt.value());
				if (ownershipKind == SoundOwnershipKind::LuaAudio)
					finishLuaAudioOwnership(sessionIt.value(), channelIndex, ownershipGeneration,
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
				invalidateCachedFileSources();
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
				m_preinitializedMediaBackend.reset();
				static_cast<void>(initializePlaybackBackends());
			}

			/**
			 * @brief Constructs one source-free backend against a validated output device.
			 * @param device Non-null output device used by the backend.
			 * @return Initialized backend without cache or channel ownership.
			 */
			[[nodiscard]] static std::unique_ptr<SoundBackend>
			createPlaybackBackend(const QAudioDevice &device)
			{
				Q_ASSERT(!device.isNull());
				auto backend         = std::make_unique<SoundBackend>();
				backend->player      = std::make_unique<QMediaPlayer>();
				backend->audioOutput = new QAudioOutput(backend->player.get());
				backend->audioOutput->setDevice(device);
				backend->player->setAudioOutput(backend->audioOutput);
				return backend;
			}

			/**
			 * @brief Initializes playback against one usable default output.
			 * @return `true` when a non-null output device and source-free backend are available.
			 */
			[[nodiscard]] bool initializePlaybackBackends()
			{
				ensureMediaDevices();
				const QAudioDevice device = QMediaDevices::defaultAudioOutput();
				if (m_forceNoAudioOutputForTest || device.isNull())
					return false;
				if (!m_preinitializedMediaBackend)
					m_preinitializedMediaBackend = createPlaybackBackend(device);
				return m_preinitializedMediaBackend != nullptr;
			}

			/**
			 * @brief Takes the startup-initialized backend or creates another one.
			 * @return Source-free backend ready to receive cache identity and source assignment.
			 */
			[[nodiscard]] std::unique_ptr<SoundBackend> takePreinitializedBackend()
			{
				if (m_preinitializedMediaBackend)
					return std::exchange(m_preinitializedMediaBackend, nullptr);
				const QAudioDevice device = QMediaDevices::defaultAudioOutput();
				if (device.isNull())
					return {};
				return createPlaybackBackend(device);
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
			 * @param beginPeriodicPass Whether to begin the work scheduled by the once-per-minute timer.
			 */
			void scheduleMaintenance(const bool beginPeriodicPass)
			{
				if (beginPeriodicPass)
				{
					m_releaseCompletedPending = true;
					m_failedWatchRetryPending = true;
				}
				if (m_maintenancePending || m_shuttingDown)
					return;
				m_maintenancePending = true;
				QCoreApplication::postEvent(this, new QEvent(maintenanceEventType()), Qt::LowEventPriority);
			}

			/**
			 * @brief Releases one bounded batch of naturally completed backends after the retention grace.
			 * @return `true` when another expired backend requires a continuation event.
			 */
			[[nodiscard]] bool releaseCompletedBackends()
			{
				const qint64     now = currentTimeMsecs();
				constexpr qint64 graceMsecs =
				    static_cast<qint64>(SoundEngine::CachePolicy::kCompletedChannelGraceSeconds) * 1000;
				return m_cache
				    .releaseExpiredRetained(
				        now, graceMsecs, SoundEngine::CachePolicy::kMaximumRetainedTransitionsPerMaintenance)
				    .expiredRemain;
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

			/** @brief Performs periodic cleanup, queues one failed-watch retry, and evicts at most one source. */
			void performMaintenance()
			{
				if (std::exchange(m_releaseCompletedPending, false))
					m_releaseCompletedPending = releaseCompletedBackends();
				if (std::exchange(m_failedWatchRetryPending, false))
				{
					if (m_ioWorker)
					{
						QMetaObject::invokeMethod(
						    m_ioWorker, [worker = m_ioWorker] { worker->retryFailedWatch(); },
						    Qt::QueuedConnection);
					}
				}
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
			PlaybackInitializationFailed                       m_playbackInitializationFailed;
			std::function<void()>                              m_playbackInitializationBarrierForTest;
			QHash<quint64, WorkerSession>                      m_sessions;
			SoundCache                                         m_cache;
			QHash<QString, std::weak_ptr<SoundMemoryResource>> m_memoryResources;
			QHash<quint64, PendingMemoryPlay>                  m_pendingMemoryPlays;
			QHash<QString, QVector<quint64>>                   m_pendingMemoryMaterializations;
			QHash<QString, SoundCatalogFile>                   m_catalogFiles;
			QHash<QString, QString>                            m_catalogFileByRelativePath;
			QHash<QString, SoundSourceKey>                     m_cachedFileSourceKeys;
			QMultiHash<QString, QString>                       m_catalogFilesByName;
			QMultiHash<QString, QString>                       m_catalogFilesByStem;
			QHash<QString, SoundPlaybackCatalogDirectory>      m_catalogDirectories;
			QSet<QString>                                      m_watchedCatalogDirectories;
			QHash<QString, QString>                            m_pluginFileByRelativePath;
			QSet<QString>                                      m_watchedPluginDirectories;
			QSet<QString>                                      m_trackedPluginDirectories;
			QSet<QString>                                      m_pendingPluginDirectories;
			std::unique_ptr<QMediaDevices>                     m_mediaDevices;
			std::unique_ptr<SoundBackend>                      m_preinitializedMediaBackend;
			std::unique_ptr<QTimer>                            m_maintenanceTimer;
			std::unique_ptr<QThreadPool>                       m_memoryPreparationPool;
			QElapsedTimer                                      m_clock;
			QThread                                            m_ioThread;
			SoundIoWorker                                     *m_ioWorker{nullptr};
			int                                                m_watchedDirectoryCount{0};
			int                                                m_failedWatchCount{0};
			qsizetype                                          m_maximumIndexBatchSize{0};
			qsizetype                                          m_maximumDirectoryBatchSize{0};
			qsizetype                                          m_maximumReconciliationStepSize{0};
			qsizetype                                          m_watchStatePathEntryCount{0};
			QByteArray                                         m_deviceId;
			QString                                            m_qmudHome;
			QString                                            m_soundsPath;
			QString                                            m_soundsRoot;
			QString                                            m_pluginsRoot;
			SoundEngine::CachePolicy                           m_cachePolicy;
			quint64                                            m_nextBackendId{1};
			quint64                                            m_nextMemoryOperationId{1};
			quint64                                            m_slowFileResolutionCount{0};
			quint64                                            m_catalogGeneration{0};
			quint64                                            m_pluginDirectoryGeneration{0};
			quint64                                            m_publishedCatalogRevision{0};
			bool                                               m_maintenancePending{false};
			bool                                               m_catalogConfigured{false};
			bool                                               m_catalogAvailable{false};
			bool                                               m_catalogReady{false};
			bool                                               m_releaseCompletedPending{false};
			bool                                               m_failedWatchRetryPending{false};
			bool                                               m_playbackInitializationAbandoned{false};
			bool                                               m_forceNoAudioOutputForTest{false};
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
			    },
			    [this] { static_cast<void>(abandonPlaybackInitialization(true)); });
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
			if (SoundEngine *target = owner.load(std::memory_order_acquire))
			{
				auto finishOnOwnerThread = [this, notifyOwner]
				{
#if QMUD_ENABLE_SOUND
					playbackInitializationWatchdog.stop();
#endif
					if (notifyOwner)
					{
						if (SoundEngine *currentOwner = owner.load(std::memory_order_acquire))
							emit currentOwner->playbackInitializationFinished(false);
					}
				};
				if (QThread::currentThread() == target->thread())
					finishOnOwnerThread();
				else
					QMetaObject::invokeMethod(target, std::move(finishOnOwnerThread), Qt::QueuedConnection);
			}
			return true;
		}

		/** @brief Queues orderly worker-thread cleanup after startup initialization returns. */
		void requestWorkerRetirement()
		{
#if QMUD_ENABLE_SOUND
			if (!worker || !thread.isRunning() ||
			    retirementRequested.exchange(true, std::memory_order_acq_rel))
				return;
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
		QThread          thread;
		QTimer           playbackInitializationWatchdog;
		SoundWorker     *worker{nullptr};
		std::atomic_bool retirementRequested{false};
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

void SoundEngine::configureSoundDirectory(const QString &qmudHomeDirectory) const
{
	if (qmudHomeDirectory.trimmed().isEmpty())
		return;
	SoundCommand command;
	command.kind = SoundCommandKind::ConfigureSoundDirectory;
	command.text = qmudHomeDirectory;
	static_cast<void>(m_private->submit(std::move(command), false));
}

void SoundEngine::configurePluginDirectory(const QString &pluginsDirectory) const
{
	SoundCommand command;
	command.kind = SoundCommandKind::ConfigurePluginDirectory;
	command.text = pluginsDirectory;
	static_cast<void>(m_private->submit(std::move(command), false));
}

std::shared_ptr<SoundEngineSession> SoundEngine::registerSession(SessionCallbacks callbacks,
                                                                 const QString   &worldSoundDirectory) const
{
	auto state       = std::make_shared<SoundEngineSession::State>();
	state->ingress   = m_private->ingress;
	state->sessionId = m_private->nextSessionId.fetch_add(1, std::memory_order_relaxed);
	SoundCommand command;
	command.kind                      = SoundCommandKind::RegisterSession;
	command.sessionId                 = state->sessionId;
	command.worldSoundDirectory       = worldSoundDirectory;
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

bool SoundEngine::cacheFileRequestForTest(const std::shared_ptr<SoundEngineSession> &session,
                                          const QString &request, const QString &absolutePath) const
{
#if QMUD_ENABLE_SOUND
	if (!session || !session->m_state || session->m_state->ingress != m_private->ingress ||
	    session->m_state->closed.load(std::memory_order_acquire))
	{
		return false;
	}
	bool cached = false;
	QMetaObject::invokeMethod(
	    m_private->worker,
	    [worker = m_private->worker, sessionId = session->id(), request, absolutePath, &cached]
	    { cached = worker->cacheFileRequestForTest(sessionId, request, absolutePath); },
	    Qt::BlockingQueuedConnection);
	return cached;
#else
	Q_UNUSED(session);
	Q_UNUSED(request);
	Q_UNUSED(absolutePath);
	return false;
#endif
}

bool SoundEngine::fileRequestIsCachedForTest(const std::shared_ptr<SoundEngineSession> &session,
                                             const QString                             &request) const
{
#if QMUD_ENABLE_SOUND
	if (!session || !session->m_state || session->m_state->ingress != m_private->ingress ||
	    session->m_state->closed.load(std::memory_order_acquire))
	{
		return false;
	}
	bool cached = false;
	QMetaObject::invokeMethod(
	    m_private->worker, [worker = m_private->worker, sessionId = session->id(), request, &cached]
	    { cached = worker->fileRequestIsCachedForTest(sessionId, request); }, Qt::BlockingQueuedConnection);
	return cached;
#else
	Q_UNUSED(session);
	Q_UNUSED(request);
	return false;
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

bool SoundEngine::completeActiveChannelForTest(const std::shared_ptr<SoundEngineSession> &session,
                                               const int                                  buffer) const
{
#if QMUD_ENABLE_SOUND
	if (!session || !session->m_state || session->m_state->ingress != m_private->ingress ||
	    session->m_state->closed.load(std::memory_order_acquire) || buffer < 1 ||
	    buffer > SoundEngineSession::kChannelCount)
	{
		return false;
	}
	bool completed = false;
	QMetaObject::invokeMethod(
	    m_private->worker, [worker = m_private->worker, sessionId = session->id(), buffer, &completed]
	    { completed = worker->completeActiveChannelForTest(sessionId, buffer); },
	    Qt::BlockingQueuedConnection);
	return completed;
#else
	Q_UNUSED(session);
	Q_UNUSED(buffer);
	return false;
#endif
}

QString SoundEngine::activeFileIdentityForTest(const std::shared_ptr<SoundEngineSession> &session,
                                               const int                                  buffer) const
{
#if QMUD_ENABLE_SOUND
	if (!session || !session->m_state || session->m_state->ingress != m_private->ingress ||
	    session->m_state->closed.load(std::memory_order_acquire) || buffer < 1 ||
	    buffer > SoundEngineSession::kChannelCount)
	{
		return {};
	}
	QString identity;
	QMetaObject::invokeMethod(
	    m_private->worker, [worker = m_private->worker, sessionId = session->id(), buffer, &identity]
	    { identity = worker->activeFileIdentityForTest(sessionId, buffer); }, Qt::BlockingQueuedConnection);
	return identity;
#else
	Q_UNUSED(session);
	Q_UNUSED(buffer);
	return {};
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
		    state = {workerState.mediaPlayerReady};
	    },
	    Qt::BlockingQueuedConnection);
	return state;
#else
	return {};
#endif
}

SoundEngine::FileCatalogStateForTest SoundEngine::fileCatalogStateForTest() const
{
#if QMUD_ENABLE_SOUND
	SoundWorkerFileCatalogState state;
	QMetaObject::invokeMethod(
	    m_private->worker, [worker = m_private->worker, &state]
	    { state = worker->fileCatalogStateForTest(); }, Qt::BlockingQueuedConnection);
	return {state.fileCount,
	        state.directoryCount,
	        state.watchedDirectoryCount,
	        state.failedWatchCount,
	        state.maximumIndexBatchSize,
	        state.maximumDirectoryBatchSize,
	        state.maximumReconciliationStepSize,
	        state.watchStatePathEntryCount,
	        state.revision,
	        state.ready};
#else
	return {};
#endif
}

void SoundEngine::setCatalogPausedForTest(const bool paused) const
{
#if QMUD_ENABLE_SOUND
	QMetaObject::invokeMethod(
	    m_private->worker, [worker = m_private->worker, paused] { worker->setCatalogPausedForTest(paused); },
	    Qt::BlockingQueuedConnection);
#else
	Q_UNUSED(paused);
#endif
}

void SoundEngine::setIndexReadPausedForTest(const bool paused) const
{
#if QMUD_ENABLE_SOUND
	QMetaObject::invokeMethod(
	    m_private->worker, [worker = m_private->worker, paused]
	    { worker->setIndexReadPausedForTest(paused); }, Qt::BlockingQueuedConnection);
#else
	Q_UNUSED(paused);
#endif
}

bool SoundEngine::publishSyntheticCatalogDirectoryForTest(const qsizetype fileCount) const
{
#if QMUD_ENABLE_SOUND
	bool accepted = false;
	QMetaObject::invokeMethod(
	    m_private->worker, [worker = m_private->worker, fileCount, &accepted]
	    { accepted = worker->publishSyntheticCatalogDirectoryForTest(fileCount); },
	    Qt::BlockingQueuedConnection);
	return accepted;
#else
	Q_UNUSED(fileCount);
	return false;
#endif
}

quint64 SoundEngine::slowFileResolutionCountForTest() const
{
#if QMUD_ENABLE_SOUND
	quint64 count = 0;
	QMetaObject::invokeMethod(
	    m_private->worker, [worker = m_private->worker, &count]
	    { count = worker->slowFileResolutionCountForTest(); }, Qt::BlockingQueuedConnection);
	return count;
#else
	return 0;
#endif
}

SoundEngine::NativePlaybackStateForTest
SoundEngine::nativePlaybackStateForTest(const std::shared_ptr<SoundEngineSession> &session,
                                        const QMudNativeSoundProtocol              protocol,
                                        const QMudNativeSoundMedia                 media) const
{
#if QMUD_ENABLE_SOUND
	if (!session || !session->m_state || session->m_state->ingress != m_private->ingress ||
	    session->m_state->closed.load(std::memory_order_acquire))
	{
		return {};
	}
	SoundWorkerNativePlaybackState state;
	QMetaObject::invokeMethod(
	    m_private->worker, [worker = m_private->worker, sessionId = session->id(), protocol, media, &state]
	    { state = worker->nativePlaybackStateForTest(sessionId, protocol, media); },
	    Qt::BlockingQueuedConnection);
	return {state.buffer, state.filePattern, state.priority, state.current};
#else
	Q_UNUSED(session);
	Q_UNUSED(protocol);
	Q_UNUSED(media);
	return {};
#endif
}

bool SoundEngine::attemptCatalogWatchForTest(const QString &directory) const
{
#if QMUD_ENABLE_SOUND
	bool watched = false;
	QMetaObject::invokeMethod(
	    m_private->worker, [worker = m_private->worker, directory, &watched]
	    { watched = worker->attemptCatalogWatchForTest(directory); }, Qt::BlockingQueuedConnection);
	return watched;
#else
	Q_UNUSED(directory);
	return false;
#endif
}

void SoundEngine::reconcileSoundRootForTest() const
{
#if QMUD_ENABLE_SOUND
	QMetaObject::invokeMethod(
	    m_private->worker, [worker = m_private->worker] { worker->reconcileSoundRootForTest(); },
	    Qt::BlockingQueuedConnection);
#endif
}

void SoundEngine::reconcileCatalogDirectoryForTest(const QString &directory) const
{
#if QMUD_ENABLE_SOUND
	QMetaObject::invokeMethod(
	    m_private->worker, [worker = m_private->worker, directory]
	    { worker->reconcileCatalogDirectoryForTest(directory); }, Qt::BlockingQueuedConnection);
#else
	Q_UNUSED(directory);
#endif
}

void SoundEngine::runPeriodicMaintenanceForTest() const
{
#if QMUD_ENABLE_SOUND
	QMetaObject::invokeMethod(
	    m_private->worker, [worker = m_private->worker] { worker->runPeriodicMaintenanceForTest(); },
	    Qt::BlockingQueuedConnection);
#endif
}

void SoundEngine::setDownloadStallTimeoutForTest(const int milliseconds) const
{
#if QMUD_ENABLE_SOUND
	QMetaObject::invokeMethod(
	    m_private->worker, [worker = m_private->worker, milliseconds]
	    { worker->setDownloadStallTimeoutForTest(milliseconds); }, Qt::BlockingQueuedConnection);
#else
	Q_UNUSED(milliseconds);
#endif
}

void SoundEngine::processExistingDownloadForTest(const QString &destination) const
{
#if QMUD_ENABLE_SOUND
	QMetaObject::invokeMethod(
	    m_private->worker, [worker = m_private->worker, destination]
	    { worker->processExistingDownloadForTest(destination); }, Qt::BlockingQueuedConnection);
#else
	Q_UNUSED(destination);
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

void SoundEngine::forceNoAudioOutputForTest() const
{
#if QMUD_ENABLE_SOUND
	QMetaObject::invokeMethod(
	    m_private->worker, [worker = m_private->worker] { worker->forceNoAudioOutputForTest(); },
	    Qt::BlockingQueuedConnection);
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
	command.kind                = SoundCommandKind::PlayFile;
	command.buffer              = buffer;
	command.text                = fileName;
	command.loop                = loop;
	command.repeatCount         = loop ? -1 : 1;
	command.volume              = volume;
	command.pan                 = pan;
	command.ownershipGeneration = luaAudioGeneration;
	command.ownershipKind = luaAudioGeneration == 0 ? SoundOwnershipKind::None : SoundOwnershipKind::LuaAudio;
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
	command.kind                = SoundCommandKind::ReserveLuaAudioChannel;
	command.text                = fileName;
	command.ownershipGeneration = luaAudioGeneration;
	command.ownershipKind       = SoundOwnershipKind::LuaAudio;
	int result                  = eCannotPlaySound;
	int selectedBuffer          = 0;
	if (!m_state || !m_state->submit(std::move(command), true, &result, &selectedBuffer) || result != eOK)
		return 0;
	return selectedBuffer;
}

bool SoundEngineSession::confirmLuaAudioOwnership(const int buffer, const quint64 luaAudioGeneration) const
{
	if (buffer < 1 || buffer > kChannelCount || luaAudioGeneration == 0)
		return false;
	SoundCommand command;
	command.kind                = SoundCommandKind::ConfirmLuaAudioOwnership;
	command.buffer              = buffer;
	command.ownershipGeneration = luaAudioGeneration;
	command.ownershipKind       = SoundOwnershipKind::LuaAudio;
	int result                  = eCannotPlaySound;
	return m_state && m_state->submit(std::move(command), true, &result) && result == eOK;
}

int SoundEngineSession::stopLuaAudioChannel(const int buffer, const quint64 luaAudioGeneration) const
{
	if (buffer < 1 || buffer > kChannelCount || luaAudioGeneration == 0)
		return eBadParameter;
	SoundCommand command;
	command.kind                = SoundCommandKind::Stop;
	command.buffer              = buffer;
	command.ownershipGeneration = luaAudioGeneration;
	command.ownershipKind       = SoundOwnershipKind::LuaAudio;
	int result                  = eCannotPlaySound;
	return m_state && m_state->submit(std::move(command), true, &result) ? result : eCannotPlaySound;
}

void SoundEngineSession::updateWorldSoundDirectory(const QString &worldSoundDirectory) const
{
	SoundCommand command;
	command.kind                = SoundCommandKind::UpdateWorldSoundDirectory;
	command.worldSoundDirectory = worldSoundDirectory;
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
	command.kind                = SoundCommandKind::PlayMemory;
	command.buffer              = buffer;
	command.bytes               = data;
	command.loop                = loop;
	command.repeatCount         = loop ? -1 : 1;
	command.volume              = volume;
	command.pan                 = pan;
	command.ownershipGeneration = luaAudioGeneration;
	command.ownershipKind = luaAudioGeneration == 0 ? SoundOwnershipKind::None : SoundOwnershipKind::LuaAudio;
	int result            = eCannotPlaySound;
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

bool SoundEngineSession::submitNativeSound(QMudNativeSoundRequest request) const
{
	if ((!request.stop && request.filePattern.trimmed().isEmpty()) || request.repeatCount == 0 ||
	    request.repeatCount < -1 || request.volume < 0 || request.volume > 100 || request.priority < 0 ||
	    request.priority > 100)
	{
		return false;
	}
	SoundCommand command;
	command.kind                   = SoundCommandKind::NativeSoundRequest;
	command.text                   = std::move(request.filePattern);
	command.url                    = std::move(request.downloadUrl);
	command.music                  = request.media == QMudNativeSoundMedia::Music;
	command.continueNativePlayback = request.continuePlayback;
	command.stopNativePlayback     = request.stop;
	command.repeatCount            = request.repeatCount;
	command.priority               = request.priority;
	command.volume                 = request.volume;
	command.nativeProtocol         = request.protocol;
	return m_state && m_state->submit(std::move(command), false);
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
	command.kind        = SoundCommandKind::CommitPrepared;
	command.token       = token;
	command.buffer      = buffer;
	command.loop        = loop;
	command.repeatCount = loop ? -1 : 1;
	command.volume      = volume;
	command.pan         = pan;
	int result          = eCannotPlaySound;
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
