/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: SoundEngineTestAccess.h
 * Role: Purpose-built test access to sound-worker lifecycle, cache, and memory-resource state.
 */

#pragma once

#include "SoundEngine.h"

/** @brief Keeps deterministic sound-worker state inspection out of SoundEngine's production API. */
class SoundEngineTestAccess final
{
	public:
		/** @brief Cache accounting and deferred-maintenance state observed on the worker thread. */
		struct CacheState
		{
				qint64    totalBytes{0};
				qsizetype sourceCount{0};
				qsizetype backendCount{0};
				bool      maintenancePending{false};
		};
		/** @brief Cache states observed after consecutive queued maintenance events. */
		struct CacheMaintenanceRun
		{
				CacheState configured;
				CacheState firstStep;
				CacheState secondStep;
		};
		/** @brief Source-free playback backend readiness observed on the worker thread. */
		struct PlaybackInitializationState
		{
				bool effectReady{false};
				bool mediaPlayerReady{false};
		};

		/**
		 * @brief Adds one lightweight source record to the sound worker cache.
		 * @param engine Sound engine whose worker owns the cache.
		 * @param identity Distinct test source identity.
		 * @param sourceBytes Accounted source size.
		 * @param idle Whether the source is fully idle and eviction-eligible.
		 * @param usedAtMsecs Positive last-use timestamp used for LRU ordering.
		 */
		static void addCacheSource(const SoundEngine &engine, const QString &identity,
		                           const qint64 sourceBytes, const bool idle, const qint64 usedAtMsecs)
		{
			engine.addCacheSourceForTest(identity, sourceBytes, idle, usedAtMsecs);
		}

		/**
		 * @brief Applies a cache policy, simulates one timer tick, and delivers its incremental events.
		 * @param engine Sound engine whose worker receives the policy.
		 * @param policy Cache policy to apply.
		 * @return Worker cache state after configuration and each consecutive maintenance event.
		 */
		[[nodiscard]] static CacheMaintenanceRun
		configureCacheAndRunMaintenance(const SoundEngine &engine, const SoundEngine::CachePolicy policy)
		{
			const SoundEngine::CacheMaintenanceRunForTest run =
			    engine.configureCacheAndRunMaintenanceForTest(policy);
			return {
			    {run.configured.totalBytes, run.configured.sourceCount, run.configured.backendCount,
			     run.configured.maintenancePending},
			    {run.firstStep.totalBytes,  run.firstStep.sourceCount,  run.firstStep.backendCount,
			     run.firstStep.maintenancePending },
			    {run.secondStep.totalBytes, run.secondStep.sourceCount, run.secondStep.backendCount,
			     run.secondStep.maintenancePending}
            };
		}

		/**
		 * @brief Delivers a prepared-memory resource after its waiting play was canceled.
		 * @param engine Sound engine whose worker receives the resource.
		 * @return `true` when the worker incorrectly indexes the canceled resource.
		 */
		[[nodiscard]] static bool canceledMemoryResourceIsIndexed(const SoundEngine &engine)
		{
			return engine.canceledMemoryResourceIsIndexedForTest();
		}

		/**
		 * @brief Completes one reservation through the sound worker's production completion transition.
		 * @param engine Sound engine whose worker owns the session.
		 * @param session Session containing the reservation.
		 * @param buffer One-based reserved channel to complete.
		 * @return `true` when the worker completed the reserved channel.
		 */
		[[nodiscard]] static bool completeReservedChannel(const SoundEngine                         &engine,
		                                                  const std::shared_ptr<SoundEngineSession> &session,
		                                                  const int                                  buffer)
		{
			return engine.completeReservedChannelForTest(session, buffer);
		}

		/**
		 * @brief Fails a prepared occurrence through the sound worker's production error transition.
		 * @param engine Sound engine whose worker owns the session.
		 * @param session Session receiving the prepared occurrence.
		 * @param message Diagnostic supplied to the backend-error transition.
		 * @return Preparation token accepted by commitPrepared() or cancelPrepared(), or zero on failure.
		 */
		[[nodiscard]] static quint64 failedPreparedSound(const SoundEngine                         &engine,
		                                                 const std::shared_ptr<SoundEngineSession> &session,
		                                                 const QString                             &message)
		{
			return engine.failedPreparedSoundForTest(session, message, false);
		}

		/**
		 * @brief Fails backend initialization and retains its diagnostic for a prepared occurrence.
		 * @param engine Sound engine whose worker owns the session.
		 * @param session Session receiving the failed prepared occurrence.
		 * @param message Diagnostic supplied during backend initialization.
		 * @return Preparation token accepted by commitPrepared(), or zero on failure.
		 */
		[[nodiscard]] static quint64
		failedInitializedPreparedSound(const SoundEngine                         &engine,
		                               const std::shared_ptr<SoundEngineSession> &session,
		                               const QString                             &message)
		{
			return engine.failedPreparedSoundForTest(session, message, true);
		}

		/**
		 * @brief Reads coherent cache accounting and maintenance state from the worker.
		 * @param engine Sound engine whose worker cache is inspected.
		 * @return Worker cache state.
		 */
		[[nodiscard]] static CacheState cacheState(const SoundEngine &engine)
		{
			const SoundEngine::CacheStateForTest state = engine.cacheStateForTest();
			return {state.totalBytes, state.sourceCount, state.backendCount, state.maintenancePending};
		}

		/**
		 * @brief Reads startup playback backend readiness from the sound worker.
		 * @param engine Sound engine whose initialized playback objects are inspected.
		 * @return Availability of the source-free effect and media-player backends.
		 */
		[[nodiscard]] static PlaybackInitializationState
		playbackInitializationState(const SoundEngine &engine)
		{
			const SoundEngine::PlaybackInitializationStateForTest state =
			    engine.playbackInitializationStateForTest();
			return {state.effectReady, state.mediaPlayerReady};
		}

		/**
		 * @brief Installs a one-shot barrier at the entered startup-initialization boundary.
		 * @param engine Sound engine whose worker receives the barrier.
		 * @param barrier Barrier invoked before multimedia construction.
		 */
		static void setPlaybackInitializationBarrier(const SoundEngine &engine, std::function<void()> barrier)
		{
			engine.setPlaybackInitializationBarrierForTest(std::move(barrier));
		}

		/**
		 * @brief Expires the production playback-initialization watchdog immediately.
		 * @param engine Sound engine whose watchdog is expired.
		 */
		static void expirePlaybackInitializationWatchdog(const SoundEngine &engine)
		{
			engine.expirePlaybackInitializationWatchdogForTest();
		}

		/**
		 * @brief Reports whether the sound worker is still running.
		 * @param engine Sound engine whose worker is inspected.
		 * @return `true` while the sound worker thread is running.
		 */
		[[nodiscard]] static bool soundWorkerRunning(const SoundEngine &engine)
		{
			return engine.soundWorkerRunningForTest();
		}

		/**
		 * @brief Returns the number of worker states retained after public-engine destruction.
		 * @return Current retained-worker count.
		 */
		[[nodiscard]] static int retiredWorkerCount()
		{
			return SoundEngine::retiredWorkerCountForTest();
		}
};
