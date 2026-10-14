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
#include <array>
#include <functional>
#include <memory>

class SoundEngineSession;
class SoundEngineTestAccess;

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
				static constexpr int             kMinimumRamCacheSizeMiB       = 64;
				static constexpr int             kDefaultRamCacheSizeMiB       = 128;
				static constexpr int             kMaximumRamCacheSizeMiB       = 1024;
				static constexpr int             kMinimumIdleEvictionSeconds   = 1800;
				static constexpr int             kDefaultIdleEvictionSeconds   = 7200;
				static constexpr int             kMaximumIdleEvictionSeconds   = 86400;
				static constexpr int             kCompletedChannelGraceSeconds = 300;
				static constexpr int             kMaintenanceIntervalSeconds   = 60;

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

		SoundEngine(const SoundEngine &)                                                 = delete;
		SoundEngine                                      &operator=(const SoundEngine &) = delete;

		/** @brief Queues process-wide device and playback-backend initialization during application startup. */
		void                                              initializePlayback();

		/**
		 * @brief Creates a sound session rooted at one world's startup directory.
		 * @param startupDirectory Captured QMud home used for legacy sound-path resolution.
		 * @param callbacks Runtime notifications with the thread contracts documented by SessionCallbacks.
		 * @return Thread-safe session handle, or an empty pointer when the worker is unavailable.
		 */
		[[nodiscard]] std::shared_ptr<SoundEngineSession> registerSession(const QString   &startupDirectory,
		                                                                  SessionCallbacks callbacks) const;
		/**
		 * @brief Removes notification routing for a closing session and closes its worker state.
		 * @param session Session being closed.
		 */
		void                      unregisterSession(const std::shared_ptr<SoundEngineSession> &session) const;
		/**
		 * @brief Applies cache limits used by the next periodic maintenance pass.
		 * @param policy Requested process-wide cache policy.
		 */
		void                      configureCache(CachePolicy policy) const;
		/**
		 * @brief Returns the normalized process-wide cache policy.
		 * @return Current cache policy.
		 */
		[[nodiscard]] CachePolicy cachePolicy() const noexcept;

	signals:
		/**
		 * @brief Reports completion of startup playback initialization.
		 * @param available Whether both playback implementations initialized before the watchdog expired.
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
		/** @brief Test-only source-free playback backend readiness. */
		struct PlaybackInitializationStateForTest
		{
				bool effectReady{false};
				bool mediaPlayerReady{false};
		};

		/**
		 * @brief Adds a lightweight source record to the worker cache for deterministic integration tests.
		 * @param identity Distinct test source identity.
		 * @param sourceBytes Accounted source size.
		 * @param idle Whether the source is fully idle and eviction-eligible.
		 * @param usedAtMsecs Positive last-use timestamp used for LRU ordering.
		 */
		void addCacheSourceForTest(const QString &identity, qint64 sourceBytes, bool idle,
		                           qint64 usedAtMsecs) const;
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
		 * @brief Completes one reserved channel through the worker's production completion transition.
		 * @param session Session owning the reserved channel.
		 * @param buffer One-based reserved channel to complete.
		 * @return `true` when the channel was transitioned to completed state.
		 */
		[[nodiscard]] bool completeReservedChannelForTest(const std::shared_ptr<SoundEngineSession> &session,
		                                                  int buffer) const;
		/**
		 * @brief Creates a prepared occurrence and fails it through the production backend-error transition.
		 * @param session Session receiving the prepared occurrence.
		 * @param message Diagnostic supplied to the backend-error transition.
		 * @param initializationFailure Whether to fail during backend initialization rather than preparation.
		 * @return Preparation token accepted by commitPrepared() or cancelPrepared(), or zero on failure.
		 */
		[[nodiscard]] quint64 failedPreparedSoundForTest(const std::shared_ptr<SoundEngineSession> &session,
		                                                 const QString                             &message,
		                                                 bool initializationFailure) const;
		/**
		 * @brief Reads cache accounting and maintenance state on the worker thread.
		 * @return Coherent test-only cache state.
		 */
		[[nodiscard]] CacheStateForTest                  cacheStateForTest() const;
		/**
		 * @brief Reads startup playback backend readiness on the worker thread.
		 * @return Availability of the source-free effect and media-player backends.
		 */
		[[nodiscard]] PlaybackInitializationStateForTest playbackInitializationStateForTest() const;
		/**
		 * @brief Installs a one-shot barrier at the entered startup-initialization boundary.
		 * @param barrier Barrier invoked on the sound worker before multimedia construction.
		 */
		void                     setPlaybackInitializationBarrierForTest(std::function<void()> barrier) const;
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
				quint64                        revision{1};
				std::array<int, kChannelCount> status{};

				/** @brief Initializes every channel as unallocated. */
				Snapshot()
				{
					status.fill(-2);
				}
		};

		/**
		 * @brief Updates the world root used by later sound path resolution.
		 * @param startupDirectory New captured QMud home for the session.
		 */
		void              updateStartupDirectory(const QString &startupDirectory) const;

		/**
		 * @brief Plays or updates a file-backed sound without plugin interception.
		 * @param buffer Fixed channel, or zero for automatic selection.
		 * @param fileName Legacy QMud-home-relative or absolute-safe sound path.
		 * @param loop Whether playback repeats indefinitely.
		 * @param volume MUSHclient volume in the range -100 through 0.
		 * @param pan Compatibility pan value retained for callers.
		 * @param luaAudioGeneration Optional LuaAudio owner generation released on natural completion.
		 * @param selectedBuffer Optional destination receiving the selected one-based channel on success.
		 * @param requireReservation Require a matching delayed-play reservation on the fixed channel.
		 * @return MUSHclient-compatible status code after the worker accepts or rejects the operation.
		 */
		int               playFile(int buffer, const QString &fileName, bool loop, double volume, double pan,
		                           quint64 luaAudioGeneration = 0, int *selectedBuffer = nullptr,
		                           bool requireReservation = false) const;
		/**
		 * @brief Plays encoded sound bytes without plugin interception.
		 * @param buffer Fixed channel, or zero for automatic selection.
		 * @param data Encoded sound payload.
		 * @param loop Whether playback repeats indefinitely.
		 * @param volume MUSHclient volume in the range -100 through 0.
		 * @param pan Compatibility pan value retained for callers.
		 * @param luaAudioGeneration Optional LuaAudio owner generation released on natural completion.
		 * @param selectedBuffer Optional destination receiving the selected one-based channel on success.
		 * @return MUSHclient-compatible status code after the worker accepts or rejects the operation.
		 */
		int               playMemory(int buffer, const QByteArray &data, bool loop, double volume, double pan,
		                             quint64 luaAudioGeneration = 0, int *selectedBuffer = nullptr) const;
		/**
		 * @brief Validates a delayed LuaAudio file and reserves the first reusable channel.
		 * @param fileName Legacy QMud-home-relative or absolute-safe sound path.
		 * @param luaAudioGeneration Ownership generation attached to the reservation.
		 * @return Selected one-based channel, or zero when none is reusable.
		 */
		[[nodiscard]] int reserveLuaAudioChannel(const QString &fileName, quint64 luaAudioGeneration) const;
		/**
		 * @brief Synchronously confirms that runtime ownership exists for a submitted LuaAudio generation.
		 * @param buffer One-based channel returned by play or reservation.
		 * @param luaAudioGeneration Ownership generation to confirm.
		 * @return `true` when the active or already-completed generation was reconciled.
		 */
		[[nodiscard]] bool    confirmLuaAudioOwnership(int buffer, quint64 luaAudioGeneration) const;
		/**
		 * @brief Stops one LuaAudio channel only while its ownership generation remains current.
		 * @param buffer Fixed one-based channel.
		 * @param luaAudioGeneration Expected LuaAudio ownership generation.
		 * @return MUSHclient-compatible status code.
		 */
		[[nodiscard]] int     stopLuaAudioChannel(int buffer, quint64 luaAudioGeneration) const;
		/**
		 * @brief Stops one logical channel or all channels immediately.
		 * @param buffer Fixed channel, or zero for all channels.
		 * @return MUSHclient-compatible status code.
		 */
		[[nodiscard]] int     stop(int buffer) const;
		/**
		 * @brief Begins loading an interceptable sound without making it audible.
		 * @param fileName Legacy QMud-home-relative or absolute-safe sound path.
		 * @return Nonzero preparation token, or zero when the request cannot be accepted.
		 */
		[[nodiscard]] quint64 prepareFile(const QString &fileName) const;
		/**
		 * @brief Commits a previously prepared sound after plugin callbacks decline it.
		 * @param token Preparation token returned by prepareFile().
		 * @param buffer Fixed channel, or zero for automatic selection.
		 * @param loop Whether playback repeats indefinitely.
		 * @param volume MUSHclient volume in the range -100 through 0.
		 * @param pan Compatibility pan value retained for callers.
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
