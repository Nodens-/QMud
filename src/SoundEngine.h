/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: SoundEngine.h
 * Role: Process-wide threaded sound playback service and per-world sound-session interface.
 */

#ifndef QMUD_SOUNDENGINE_H
#define QMUD_SOUNDENGINE_H

// ReSharper disable once CppUnusedIncludeDirective
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <array>
#include <chrono>
#include <functional>
#include <memory>

class SoundEngineSession;
class SoundEngineTestAccess;

/** @brief Independently controlled LuaAudio playback attribute. */
enum class QMudSoundAttribute : quint8
{
	Volume,
	Pan,
	Pitch
};

/** @brief Whether a control target replaces or offsets the current sample-clock value. */
enum class QMudSoundAttributeTarget : quint8
{
	Absolute,
	Relative
};

/** @brief Native protocol that originated a sound-engine request. */
enum class QMudNativeSoundProtocol : quint8
{
	Msp,
	Mxp
};

/** @brief Native protocol media class with independent playback semantics. */
enum class QMudNativeSoundMedia : quint8
{
	Sound,
	Music
};

/** @brief Parsed protocol-independent request submitted to the sound worker. */
struct QMudNativeSoundRequest
{
		QMudNativeSoundProtocol protocol{QMudNativeSoundProtocol::Msp};
		QMudNativeSoundMedia    media{QMudNativeSoundMedia::Sound};
		QString                 filePattern;
		QString                 downloadUrl;
		int                     volume{100};
		int                     repeatCount{1};
		int                     priority{50};
		bool                    continuePlayback{false};
		bool                    stop{false};
};

/**
 * @brief Owns the process-wide sound worker and creates isolated per-world sound sessions.
 *
 * All Qt multimedia objects live on the worker thread. Public session methods are thread-safe and may be called
 * directly by Lua worker lanes without routing through the world runtime thread.
 */
class SoundEngine final : public QObject
{
		Q_OBJECT

	public:
		/**
		 * @brief Configures the process-wide loaded-source cache.
		 */
		struct CachePolicy
		{
				static constexpr int             kMinimumRamCacheSizeMiB                   = 64;
				static constexpr int             kDefaultRamCacheSizeMiB                   = 128;
				static constexpr int             kMaximumRamCacheSizeMiB                   = 1024;
				static constexpr int             kMinimumIdleEvictionSeconds               = 1800;
				static constexpr int             kDefaultIdleEvictionSeconds               = 7200;
				static constexpr int             kMaximumIdleEvictionSeconds               = 86400;
				static constexpr int             kCompletedChannelGraceSeconds             = 300;
				static constexpr int             kMaintenanceIntervalSeconds               = 60;
				static constexpr qsizetype       kMaximumRetainedTransitionsPerMaintenance = 128;

				int                              ramCacheSizeMiB{kDefaultRamCacheSizeMiB};
				int                              idleEvictionSeconds{kDefaultIdleEvictionSeconds};

				/**
				 * @brief Clamps cache settings to the supported engine ranges.
				 * @param policy Requested cache policy.
				 * @return Normalized cache policy.
				 */
				[[nodiscard]] static CachePolicy normalized(CachePolicy policy) noexcept;

				/** @brief Compares both cache policy values. */
				bool                             operator==(const CachePolicy &) const = default;
		};

		/** @brief Notifications emitted by one sound session. */
		struct SessionCallbacks
		{
				/**
				 * @brief Releases LuaAudio ownership synchronously on the sound worker.
				 *
				 * The callback must be thread-safe, bounded, and must not access QObject state.
				 */
				std::function<void(int, quint64)>    luaAudioOwnershipReleased;
				/** @brief Reports playback failures on the sound engine owner's thread. */
				std::function<void(const QString &)> playbackError;
		};

		/**
		 * @brief Starts the process-wide sound worker.
		 * @param parent Optional Qt parent object.
		 */
		explicit SoundEngine(QObject *parent = nullptr);
		/** @brief Stops a responsive worker or retires initialization that is still blocked during destruction. */
		~SoundEngine() override;

		SoundEngine(const SoundEngine &)                         = delete;
		SoundEngine              &operator=(const SoundEngine &) = delete;

		/**
		 * @brief Reports the immutable physical output policy supplied by the application or test executable.
		 * @return `true` for test executables and `false` for the QMud application.
		 */
		[[nodiscard]] static bool isOutputMuted() noexcept;

		/** @brief Queues process-wide device and playback-backend initialization during application startup. */
		void                      initializePlayback();
		/**
		 * @brief Configures the QMud home whose sounds tree is indexed lazily by the sound worker.
		 * @param qmudHomeDirectory Resolved QMud home directory.
		 */
		void                      configureSoundDirectory(const QString &qmudHomeDirectory) const;
		/**
		 * @brief Configures the global plugins directory accepted for explicit plugin-bundled sounds.
		 * @param pluginsDirectory Resolved plugins directory contained beneath QMud home.
		 */
		void                      configurePluginDirectory(const QString &pluginsDirectory) const;

		/**
		 * @brief Creates a sound session rooted at the process-wide configured sounds tree.
		 * @param callbacks Runtime notifications with the thread contracts documented by SessionCallbacks.
		 * @param worldSoundDirectory Persisted per-world directory searched before the shared sounds tree.
		 * @return Thread-safe session handle, or an empty pointer when the worker is unavailable.
		 */
		[[nodiscard]] std::shared_ptr<SoundEngineSession>
		     registerSession(SessionCallbacks callbacks, const QString &worldSoundDirectory = {}) const;
		/**
		 * @brief Removes notification routing for a closing session and closes its worker state.
		 * @param session Session being closed.
		 */
		void unregisterSession(const std::shared_ptr<SoundEngineSession> &session) const;
		/**
		 * @brief Applies cache limits used by the next periodic maintenance pass.
		 * @param policy Requested process-wide cache policy.
		 */
		void configureCache(CachePolicy policy) const;
		/**
		 * @brief Returns the normalized process-wide cache policy.
		 * @return Current cache policy.
		 */
		[[nodiscard]] CachePolicy cachePolicy() const noexcept;

	signals:
		/**
		 * @brief Reports completion of startup playback initialization.
		 * @param available Whether playback initialized before the watchdog expired.
		 */
		void playbackInitializationFinished(bool available);

	private:
		/** @brief Test-only view of worker cache accounting and pending maintenance state. */
		struct CacheStateForTest
		{
				qint64    totalBytes{0};
				qsizetype sourceCount{0};
				qsizetype backendCount{0};
				bool      maintenancePending{false};
		};
		/** @brief Test-only states captured after consecutive queued maintenance events. */
		struct CacheMaintenanceRunForTest
		{
				CacheStateForTest configured;
				CacheStateForTest firstStep;
				CacheStateForTest secondStep;
		};
		/** @brief Test-only source-free playback readiness and physical sink gain. */
		struct PlaybackInitializationStateForTest
		{
				bool   mediaPlayerReady{false};
				double outputVolume{-1.0};
		};
		/** @brief Test-only view of the worker-owned sound-file catalog. */
		struct FileCatalogStateForTest
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
		/** @brief Test-only view of one worker-owned native protocol playback. */
		struct NativePlaybackStateForTest
		{
				int     buffer{0};
				QString filePattern;
				int     priority{0};
				bool    current{false};
		};

		/**
		 * @brief Adds a lightweight source record to the worker cache for deterministic integration tests.
		 * @param identity Distinct test source identity.
		 * @param sourceBytes Accounted source size.
		 * @param idle Whether the source is fully idle and eviction-eligible.
		 * @param usedAtMsecs Positive last-use timestamp used for LRU ordering.
		 */
		void               addCacheSourceForTest(const QString &identity, qint64 sourceBytes, bool idle,
		                                         qint64 usedAtMsecs) const;
		/**
		 * @brief Seeds one real file request into the production cache indexes without multimedia objects.
		 * @param session Session owning the request.
		 * @param request Request text used as the session cache key.
		 * @param absolutePath Existing catalog file identity.
		 * @return `true` when the cache identity was installed.
		 */
		[[nodiscard]] bool cacheFileRequestForTest(const std::shared_ptr<SoundEngineSession> &session,
		                                           const QString &request, const QString &absolutePath) const;
		/**
		 * @brief Reports whether one request retains a reusable exact-file cache association.
		 * @param session Session owning the request.
		 * @param request Request text used as the session cache key.
		 * @return `true` when the request remains cached.
		 */
		[[nodiscard]] bool fileRequestIsCachedForTest(const std::shared_ptr<SoundEngineSession> &session,
		                                              const QString &request) const;
		/**
		 * @brief Removes fixtures and resolves a request before queued watcher notifications can execute.
		 * @param session Session supplying the world scope and request cache.
		 * @param request Request to resolve through production selection.
		 * @param files Absolute indexed sound fixtures to remove in the same worker turn.
		 * @param allowPatterns Whether to return the native candidate pool instead of one file source.
		 * @return Selected canonical files, or an empty list after a miss or invalid test setup.
		 */
		[[nodiscard]] QStringList
		selectFilesAfterRemovalForTest(const std::shared_ptr<SoundEngineSession> &session,
		                               const QString &request, const QStringList &files,
		                               bool allowPatterns) const;
		/**
		 * @brief Applies a policy, simulates one timer tick, and delivers its incremental maintenance events.
		 * @param policy Cache policy to apply.
		 * @return Worker cache state after configuration and each consecutive maintenance event.
		 */
		[[nodiscard]] CacheMaintenanceRunForTest
		                   configureCacheAndRunMaintenanceForTest(CachePolicy policy) const;
		/**
		 * @brief Delivers a prepared-memory resource after its waiting play was canceled.
		 * @return `true` when the worker incorrectly indexes the canceled resource.
		 */
		[[nodiscard]] bool canceledMemoryResourceIsIndexedForTest() const;
		/**
		 * @brief Queues a test synchronization task on the ordered memory-preparation pool.
		 * @param task Task whose captured synchronization objects outlive its execution.
		 */
		void               queueMemoryPreparationTaskForTest(std::function<void()> task) const;
		/**
		 * @brief Completes one reserved channel through the worker's production completion transition.
		 * @param session Session owning the reserved channel.
		 * @param buffer One-based reserved channel to complete.
		 * @return `true` when the channel was transitioned to completed state.
		 */
		[[nodiscard]] bool completeReservedChannelForTest(const std::shared_ptr<SoundEngineSession> &session,
		                                                  int buffer) const;
		/**
		 * @brief Completes one active channel through the worker's production completion transition.
		 * @param session Session owning the active channel.
		 * @param buffer One-based active channel to complete.
		 * @return `true` when the active assignment was completed.
		 */
		[[nodiscard]] bool completeActiveChannelForTest(const std::shared_ptr<SoundEngineSession> &session,
		                                                int buffer) const;
		/**
		 * @brief Replaces an active assignment's audio delivery with a worker-driven sample clock.
		 * @param session Session owning the active assignment.
		 * @param buffer One-based channel whose decoder and sink are stopped before clock replacement.
		 * @return Assignment generation identifying the controlled clock, or zero when unavailable.
		 */
		[[nodiscard]] quint64 controlPcmClockForTest(const std::shared_ptr<SoundEngineSession> &session,
		                                             int                                        buffer) const;
		/**
		 * @brief Advances a controlled sample clock and applies the production PCM control transition.
		 * @param session Session owning the controlled assignment.
		 * @param buffer One-based controlled channel.
		 * @param generation Assignment generation returned when the clock was controlled.
		 * @param duration Sample-clock interval to render.
		 * @return Whether the stopped backend still owned that generation and its clock was advanced.
		 */
		[[nodiscard]] bool    advancePcmClockForTest(const std::shared_ptr<SoundEngineSession> &session,
		                                             int buffer, quint64 generation,
		                                             std::chrono::milliseconds duration) const;
		/**
		 * @brief Checks ordered decoder continuation and lifecycle transitions on the worker.
		 * @param session Session receiving an isolated decoder fixture.
		 * @param ordering Placement of the loop update relative to terminal notifications.
		 * @param transition Subsequent repeat, ownership, failure, or backpressure transition.
		 * @return Whether the production transitions preserve the expected assignment and PCM state.
		 */
		[[nodiscard]] bool    decoderContinuationForTest(const std::shared_ptr<SoundEngineSession> &session,
		                                                 int ordering, int transition) const;
		/**
		 * @brief Expires one active assignment through the production playback-progress watchdog.
		 * @param session Session owning the active channel.
		 * @param buffer One-based active channel to expire.
		 * @return `true` when the watchdog removed the active assignment.
		 */
		[[nodiscard]] bool
		expireActivePlaybackWatchdogForTest(const std::shared_ptr<SoundEngineSession> &session,
		                                    int                                        buffer) const;
		/**
		 * @brief Reads the resolved file identity assigned to one active channel.
		 * @param session Session owning the channel.
		 * @param buffer One-based channel to inspect.
		 * @return Absolute file identity, or an empty string when no file backend is assigned.
		 */
		[[nodiscard]] QString   activeFileIdentityForTest(const std::shared_ptr<SoundEngineSession> &session,
		                                                  int buffer) const;
		/** @brief Applies the output-device reset transition on the worker for deterministic tests. */
		void                    resetAudioDeviceForTest() const;

		/**
		 * @brief Counts unresolved file requests on the sound worker for deterministic lifecycle tests.
		 * @param session Owning session.
		 * @param commitsOnly Whether only requests retaining a waiting commit are counted.
		 * @return Number of unresolved occurrences.
		 */
		[[nodiscard]] qsizetype pendingFileCountForTest(const std::shared_ptr<SoundEngineSession> &session,
		                                                bool commitsOnly) const;

		/**
		 * @brief Samples an active PCM control through the worker for deterministic integration tests.
		 * @param session Session owning the channel.
		 * @param buffer One-based channel.
		 * @param attribute Control to sample.
		 * @return Current control in LuaAudio units, or zero without an active transport.
		 */
		[[nodiscard]] double    activeAttributeForTest(const std::shared_ptr<SoundEngineSession> &session,
		                                               int buffer, QMudSoundAttribute attribute) const;
		/**
		 * @brief Reads an assigned sink's physical gain without changing logical playback controls.
		 * @param session Session owning the channel.
		 * @param buffer One-based channel to inspect.
		 * @return Linear sink gain, or minus one when the channel has no sink.
		 */
		[[nodiscard]] double    activeOutputVolumeForTest(const std::shared_ptr<SoundEngineSession> &session,
		                                                  int buffer) const;
		/**
		 * @brief Creates a prepared occurrence and fails it through the production backend-error transition.
		 * @param session Session receiving the prepared occurrence.
		 * @param message Diagnostic supplied to the backend-error transition.
		 * @param initializationFailure Whether to fail during backend initialization rather than preparation.
		 * @return Preparation token accepted by commitPrepared() or cancelPrepared(), or zero on failure.
		 */
		[[nodiscard]] quint64   failedPreparedSoundForTest(const std::shared_ptr<SoundEngineSession> &session,
		                                                   const QString                             &message,
		                                                   bool initializationFailure) const;
		/**
		 * @brief Reads cache accounting and maintenance state on the worker thread.
		 * @return Coherent test-only cache state.
		 */
		[[nodiscard]] CacheStateForTest                  cacheStateForTest() const;
		/**
		 * @brief Reads startup playback readiness and physical sink gain on the worker thread.
		 * @return Source-free backend availability and sink gain, or minus one without a sink.
		 */
		[[nodiscard]] PlaybackInitializationStateForTest playbackInitializationStateForTest() const;
		/**
		 * @brief Reads the worker-owned sound-file catalog state.
		 * @return Current catalog size and initial-scan state.
		 */
		[[nodiscard]] FileCatalogStateForTest            fileCatalogStateForTest() const;
		/**
		 * @brief Pauses or resumes low-priority catalog events for deterministic tests.
		 * @param paused Whether later catalog events should remain suspended.
		 */
		void                                             setCatalogPausedForTest(bool paused) const;
		/**
		 * @brief Pauses or resumes persistent-index ingestion for deterministic tests.
		 * @param paused Whether index batches must remain suspended.
		 */
		void                                             setIndexReadPausedForTest(bool paused) const;
		/**
		 * @brief Publishes synthetic catalog paths through the production bounded directory handoff.
		 * @param fileCount Number of paths to publish.
		 * @return `true` when the quiescent I/O worker accepted the publication.
		 */
		[[nodiscard]] bool    publishSyntheticCatalogDirectoryForTest(qsizetype fileCount) const;
		/**
		 * @brief Reads how many file requests used full filesystem resolution on the sound worker.
		 * @return Number of slow file-resolution attempts.
		 */
		[[nodiscard]] quint64 slowFileResolutionCountForTest() const;
		/**
		 * @brief Reads one worker-owned native protocol playback.
		 * @param session Session owning the logical playback.
		 * @param protocol Protocol playback namespace.
		 * @param media Media-class playback namespace.
		 * @return Current logical playback state.
		 */
		[[nodiscard]] NativePlaybackStateForTest
		nativePlaybackStateForTest(const std::shared_ptr<SoundEngineSession> &session,
		                           QMudNativeSoundProtocol protocol, QMudNativeSoundMedia media) const;
		/**
		 * @brief Attempts one catalog watch through the production registration path.
		 * @param directory Directory to register.
		 * @return `true` when registration succeeded.
		 */
		[[nodiscard]] bool       attemptCatalogWatchForTest(const QString &directory) const;
		/** @brief Reconciles the configured sounds root through the production watcher transition. */
		void                     reconcileSoundRootForTest() const;
		/**
		 * @brief Reconciles one catalog directory through the production watcher transition.
		 * @param directory Changed catalog directory.
		 */
		void                     reconcileCatalogDirectoryForTest(const QString &directory) const;
		/** @brief Runs one production periodic maintenance pass on the worker. */
		void                     runPeriodicMaintenanceForTest() const;
		/**
		 * @brief Replaces the native-download inactivity timeout for deterministic tests.
		 * @param milliseconds Positive no-progress interval applied to later requests.
		 */
		void                     setDownloadStallTimeoutForTest(int milliseconds) const;
		/**
		 * @brief Processes an existing file through native download completion for deterministic tests.
		 * @param destination Existing sound file beneath the configured sounds root.
		 */
		void                     processExistingDownloadForTest(const QString &destination) const;
		/**
		 * @brief Installs a one-shot barrier at the entered startup-initialization boundary.
		 * @param barrier Barrier invoked on the sound worker before multimedia construction.
		 */
		void                     setPlaybackInitializationBarrierForTest(std::function<void()> barrier) const;
		/** @brief Forces startup discovery to reject its output device for deterministic tests. */
		void                     forceNoAudioOutputForTest() const;
		/** @brief Expires the startup initialization watchdog through its production transition. */
		void                     expirePlaybackInitializationWatchdogForTest() const;
		/**
		 * @brief Reports whether the sound worker thread is still running.
		 * @return `true` while the sound worker thread is running.
		 */
		[[nodiscard]] bool       soundWorkerRunningForTest() const;
		/**
		 * @brief Returns the number of abandoned worker states retained until their threads finish.
		 * @return Current retained-worker count.
		 */
		[[nodiscard]] static int retiredWorkerCountForTest() noexcept;

		class Private;
		std::unique_ptr<Private> m_private;

		friend class SoundEngineTestAccess;
};

/**
 * @brief Thread-safe handle for one world's ten logical sound channels.
 */
class SoundEngineSession final
{
	public:
		static constexpr int kChannelCount = 10;

		/** @brief Immutable callback-visible state for all channels in one sound session. */
		struct Snapshot
		{
				quint64                           sessionId{0};
				quint64                           revision{1};
				std::array<int, kChannelCount>    status{};
				std::array<double, kChannelCount> volume{};

				/** @brief Initializes every channel as unallocated. */
				Snapshot()
				{
					status.fill(-2);
				}
		};

		/**
		 * @brief Updates the preferred directory used by later sound requests.
		 * @param worldSoundDirectory Persisted per-world directory beneath the sounds tree.
		 */
		void              updateWorldSoundDirectory(const QString &worldSoundDirectory) const;

		/**
		 * @brief Plays or updates a file-backed sound without plugin interception.
		 * @param buffer Fixed channel, or zero for automatic selection.
		 * @param fileName Relative sound-tree path or an absolute path beneath QMud home sounds.
		 * @param loop Whether playback repeats indefinitely.
		 * @param volume Finite MUSHclient volume in the range -100 through 0.
		 * @param pan Finite compatibility pan value retained for callers.
		 * @param luaAudioGeneration Optional LuaAudio owner generation released on natural completion.
		 * @param selectedBuffer Optional destination receiving the selected one-based channel on success.
		 * @param requireReservation Require a matching delayed-play reservation on the fixed channel.
		 * @param pitch Finite initial LuaAudio frequency offset in units of 100 Hz.
		 * @param postState Optional destination receiving the immutable post-command channel state.
		 * @return MUSHclient-compatible status code after the worker accepts or rejects the operation.
		 */
		int               playFile(int buffer, const QString &fileName, bool loop, double volume, double pan,
		                           quint64 luaAudioGeneration = 0, int *selectedBuffer = nullptr,
		                           bool requireReservation = false, double pitch = 0.0,
		                           std::shared_ptr<const Snapshot> *postState = nullptr) const;
		/**
		 * @brief Plays encoded sound bytes without plugin interception.
		 * @param buffer Fixed channel, or zero for automatic selection.
		 * @param data Encoded sound payload.
		 * @param loop Whether playback repeats indefinitely.
		 * @param volume Finite MUSHclient volume in the range -100 through 0.
		 * @param pan Finite compatibility pan value retained for callers.
		 * @param luaAudioGeneration Optional LuaAudio owner generation released on natural completion.
		 * @param selectedBuffer Optional destination receiving the selected one-based channel on success.
		 * @param postState Optional destination receiving the immutable post-command channel state.
		 * @return MUSHclient-compatible status code after the worker accepts or rejects the operation.
		 */
		int               playMemory(int buffer, const QByteArray &data, bool loop, double volume, double pan,
		                             quint64 luaAudioGeneration = 0, int *selectedBuffer = nullptr,
		                             std::shared_ptr<const Snapshot> *postState = nullptr) const;
		/**
		 * @brief Validates a delayed LuaAudio file and reserves the first reusable channel.
		 * @param fileName Relative sound-tree path or an absolute path beneath QMud home sounds.
		 * @param luaAudioGeneration Ownership generation attached to the reservation.
		 * @param volume Finite captured initial LuaAudio volume from zero through 100.
		 * @param pan Finite captured initial balance.
		 * @param pitch Finite captured initial frequency offset in units of 100 Hz.
		 * @param postState Optional immutable post-command state destination.
		 * @return Selected one-based channel, or zero when none is reusable.
		 */
		[[nodiscard]] int reserveLuaAudioChannel(const QString &fileName, quint64 luaAudioGeneration,
		                                         double volume = 100.0, double pan = 0.0, double pitch = 0.0,
		                                         std::shared_ptr<const Snapshot> *postState = nullptr) const;
		/**
		 * @brief Synchronously confirms that runtime ownership exists for a submitted LuaAudio generation.
		 * @param buffer One-based channel returned by play or reservation.
		 * @param luaAudioGeneration Ownership generation to confirm.
		 * @return `true` when the active or already-completed generation was reconciled.
		 */
		[[nodiscard]] bool confirmLuaAudioOwnership(int buffer, quint64 luaAudioGeneration) const;
		/**
		 * @brief Changes one LuaAudio attribute without replacing unrelated controls or ownership.
		 * @param buffer One-based live or reserved channel, or zero for all active LuaAudio volumes.
		 * @param luaAudioGeneration Expected ownership generation, or zero for a global volume command.
		 * @param attribute Attribute to change.
		 * @param value Target in LuaAudio units.
		 * @param duration Nonnegative slide duration in milliseconds.
		 * @param stopAtEnd Whether completing the volume slide stops playback.
		 * @param targetMode Whether a pitch target offsets its current sample-clock value.
		 * @param postState Optional immutable post-command state destination.
		 * @return MUSHclient-compatible status code.
		 */
		[[nodiscard]] int
		setLuaAudioAttribute(int buffer, quint64 luaAudioGeneration, QMudSoundAttribute attribute,
		                     double value, std::chrono::milliseconds duration = {}, bool stopAtEnd = false,
		                     QMudSoundAttributeTarget         targetMode = QMudSoundAttributeTarget::Absolute,
		                     std::shared_ptr<const Snapshot> *postState  = nullptr) const;
		/**
		 * @brief Initializes or frees this world's LuaAudio playback without shutting down other sound entry paths.
		 * @param initialized Whether later LuaAudio plays are enabled.
		 * @param postState Optional immutable post-command state destination.
		 * @return MUSHclient-compatible status code.
		 */
		[[nodiscard]] int  setLuaAudioInitialized(bool                             initialized,
		                                          std::shared_ptr<const Snapshot> *postState = nullptr) const;
		/**
		 * @brief Stops one LuaAudio channel only while its ownership generation remains current.
		 * @param buffer Fixed one-based channel.
		 * @param luaAudioGeneration Expected LuaAudio ownership generation.
		 * @param postState Optional immutable post-command state destination.
		 * @return MUSHclient-compatible status code.
		 */
		[[nodiscard]] int  stopLuaAudioChannel(int buffer, quint64 luaAudioGeneration,
		                                       std::shared_ptr<const Snapshot> *postState = nullptr) const;
		/**
		 * @brief Queues one parsed native protocol sound request without waiting for worker execution.
		 * @param request Validated protocol-independent playback request.
		 * @return `true` when the worker queue accepted the request.
		 */
		[[nodiscard]] bool submitNativeSound(QMudNativeSoundRequest request) const;
		/**
		 * @brief Stops one logical channel or all channels immediately.
		 * @param buffer Fixed channel, or zero for all channels.
		 * @param postState Optional destination receiving the immutable post-command channel state.
		 * @return MUSHclient-compatible status code.
		 */
		[[nodiscard]] int  stop(int buffer, std::shared_ptr<const Snapshot> *postState = nullptr) const;
		/**
		 * @brief Begins loading an interceptable sound without making it audible.
		 * @param fileName Relative sound-tree path or an absolute path beneath QMud home sounds.
		 * @return Nonzero preparation token, or zero when the request cannot be accepted.
		 */
		[[nodiscard]] quint64 prepareFile(const QString &fileName) const;
		/**
		 * @brief Commits a previously prepared sound after plugin callbacks decline it.
		 * @param token Preparation token returned by prepareFile().
		 * @param buffer Fixed channel, or zero for automatic selection.
		 * @param loop Whether playback repeats indefinitely.
		 * @param volume Finite MUSHclient volume in the range -100 through 0.
		 * @param pan Finite compatibility pan value retained for callers.
		 * @return MUSHclient-compatible status code.
		 */
		[[nodiscard]] int     commitPrepared(quint64 token, int buffer, bool loop, double volume,
		                                     double pan) const;
		/**
		 * @brief Cancels one prepared occurrence while retaining its loaded backend for reuse.
		 * @param token Preparation token returned by prepareFile().
		 */
		void                  cancelPrepared(quint64 token) const;
		/**
		 * @brief Returns the latest published status for one channel without crossing threads.
		 * @param buffer One-based channel number.
		 * @return MUSHclient-compatible sound status.
		 */
		[[nodiscard]] int     status(int buffer) const;
		/**
		 * @brief Loads one coherent immutable view of all callback-visible channel state.
		 * @return Latest session snapshot, or an empty pointer when the session is unavailable.
		 */
		[[nodiscard]] std::shared_ptr<const Snapshot> snapshot() const;
		/**
		 * @brief Closes the session and rejects future commands.
		 */
		void                                          close() const;
		/**
		 * @brief Returns the opaque session identity.
		 * @return Nonzero identity for a registered session.
		 */
		[[nodiscard]] quint64                         id() const noexcept;

	private:
		struct State;
		explicit SoundEngineSession(std::shared_ptr<State> state);

		std::shared_ptr<State> m_state;

		friend class SoundEngine;
};

#endif // QMUD_SOUNDENGINE_H
