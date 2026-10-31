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
		/**
		 * @brief Queues deterministic test synchronization on the real memory-preparation pool.
		 * @param engine Sound engine whose pool receives the task.
		 * @param task Task retaining its own synchronization objects.
		 */
		static void queueMemoryPreparationTask(const SoundEngine &engine, std::function<void()> task)
		{
			engine.queueMemoryPreparationTaskForTest(std::move(task));
		}
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
		/** @brief Source-free playback readiness and physical sink gain observed on the worker thread. */
		struct PlaybackInitializationState
		{
				bool   mediaPlayerReady{false};
				double outputVolume{-1.0};
		};
		/** @brief Sound-file catalog state observed on the worker thread. */
		struct FileCatalogState
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
		/** @brief Worker-owned state for one native protocol playback stream. */
		struct NativePlaybackState
		{
				int     buffer{0};
				QString filePattern;
				int     priority{0};
				bool    current{false};
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
		 * @brief Seeds one real file request into the production cache indexes without multimedia objects.
		 * @param engine Sound engine whose worker owns the cache.
		 * @param session Session owning the request.
		 * @param request Request text used as the session cache key.
		 * @param absolutePath Existing catalog file identity.
		 * @return `true` when the cache identity was installed.
		 */
		[[nodiscard]] static bool cacheFileRequest(const SoundEngine                         &engine,
		                                           const std::shared_ptr<SoundEngineSession> &session,
		                                           const QString &request, const QString &absolutePath)
		{
			return engine.cacheFileRequestForTest(session, request, absolutePath);
		}

		/**
		 * @brief Reports whether one request retains a reusable exact-file cache association.
		 * @param engine Sound engine whose worker owns the cache.
		 * @param session Session owning the request.
		 * @param request Request text used as the session cache key.
		 * @return `true` when the request remains cached.
		 */
		[[nodiscard]] static bool fileRequestIsCached(const SoundEngine                         &engine,
		                                              const std::shared_ptr<SoundEngineSession> &session,
		                                              const QString                             &request)
		{
			return engine.fileRequestIsCachedForTest(session, request);
		}

		/**
		 * @brief Resolves a request after fixture removal in the same sound-worker turn.
		 * @param engine Owning sound engine.
		 * @param session Session supplying the world scope and request cache.
		 * @param request Request to resolve through production selection.
		 * @param files Absolute indexed sound fixtures to remove before selection.
		 * @param allowPatterns Whether to return the native candidate pool instead of one file source.
		 * @return Selected canonical files, or an empty list after a miss or invalid test setup.
		 */
		[[nodiscard]] static QStringList
		selectFilesAfterRemoval(const SoundEngine &engine, const std::shared_ptr<SoundEngineSession> &session,
		                        const QString &request, const QStringList &files, const bool allowPatterns)
		{
			return engine.selectFilesAfterRemovalForTest(session, request, files, allowPatterns);
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
		 * @brief Completes one active channel through the sound worker's production transition.
		 * @param engine Sound engine whose worker owns the session.
		 * @param session Session containing the active channel.
		 * @param buffer One-based active channel to complete.
		 * @return `true` when the worker completed the active assignment.
		 */
		[[nodiscard]] static bool completeActiveChannel(const SoundEngine                         &engine,
		                                                const std::shared_ptr<SoundEngineSession> &session,
		                                                const int                                  buffer)
		{
			return engine.completeActiveChannelForTest(session, buffer);
		}

		/**
		 * @brief Stops physical delivery while preserving an assignment for sample-clock control tests.
		 * @param engine Sound engine whose worker owns the assignment.
		 * @param session Session containing the assignment.
		 * @param buffer One-based channel to control.
		 * @return Assignment generation of the controlled clock, or zero when unavailable.
		 */
		[[nodiscard]] static quint64 controlPcmClock(const SoundEngine                         &engine,
		                                             const std::shared_ptr<SoundEngineSession> &session,
		                                             const int                                  buffer)
		{
			return engine.controlPcmClockForTest(session, buffer);
		}

		/**
		 * @brief Advances one still-current controlled assignment through the production PCM control tick.
		 * @param engine Sound engine whose worker owns the assignment.
		 * @param session Session containing the assignment.
		 * @param buffer One-based controlled channel.
		 * @param generation Assignment generation returned when the clock was controlled.
		 * @param duration Sample-clock interval to render.
		 * @return Whether the stopped assignment still owned that generation and was advanced.
		 */
		[[nodiscard]] static bool advancePcmClock(const SoundEngine                         &engine,
		                                          const std::shared_ptr<SoundEngineSession> &session,
		                                          const int buffer, const quint64 generation,
		                                          const std::chrono::milliseconds duration)
		{
			return engine.advancePcmClockForTest(session, buffer, generation, duration);
		}

		/**
		 * @brief Expires one active assignment through the production playback-progress watchdog.
		 * @param engine Sound engine whose worker owns the session.
		 * @param session Session containing the active channel.
		 * @param buffer One-based active channel to expire.
		 * @return `true` when the watchdog removed the active assignment.
		 */
		[[nodiscard]] static bool
		expireActivePlaybackWatchdog(const SoundEngine                         &engine,
		                             const std::shared_ptr<SoundEngineSession> &session, const int buffer)
		{
			return engine.expireActivePlaybackWatchdogForTest(session, buffer);
		}

		/**
		 * @brief Reads the resolved file identity assigned to one active channel.
		 * @param engine Sound engine whose worker owns the session.
		 * @param session Session owning the channel.
		 * @param buffer One-based channel to inspect.
		 * @return Absolute file identity, or an empty string when no file backend is assigned.
		 */
		[[nodiscard]] static QString activeFileIdentity(const SoundEngine                         &engine,
		                                                const std::shared_ptr<SoundEngineSession> &session,
		                                                const int                                  buffer)
		{
			return engine.activeFileIdentityForTest(session, buffer);
		}
		/**
		 * @brief Exercises the worker's ordered decoder continuation transitions.
		 * @param engine Engine owning the worker.
		 * @param session Session receiving the decoder fixture.
		 * @param ordering Placement of the loop update relative to terminal notifications.
		 * @param transition Subsequent repeat, ownership, failure, or backpressure transition.
		 * @return Whether the expected lifecycle and PCM invariants hold.
		 */
		[[nodiscard]] static bool decoderContinuation(const SoundEngine                         &engine,
		                                              const std::shared_ptr<SoundEngineSession> &session,
		                                              const int ordering, const int transition)
		{
			return engine.decoderContinuationForTest(session, ordering, transition);
		}
		/**
		 * @brief Samples the active PCM transport's audible control rather than registry bookkeeping.
		 * @param engine Sound engine to inspect.
		 * @param session Session owning the channel.
		 * @param buffer One-based channel.
		 * @param attribute Control to sample.
		 * @return Current control in LuaAudio units.
		 */
		[[nodiscard]] static double activeAttribute(const SoundEngine                         &engine,
		                                            const std::shared_ptr<SoundEngineSession> &session,
		                                            int buffer, QMudSoundAttribute attribute)
		{
			return engine.activeAttributeForTest(session, buffer, attribute);
		}
		/**
		 * @brief Reads physical sink gain independently of callback-visible and PCM volume.
		 * @param engine Sound engine owning the sink.
		 * @param session Session owning the channel.
		 * @param buffer One-based channel to inspect.
		 * @return Linear sink gain, or minus one when the channel has no sink.
		 */
		[[nodiscard]] static double activeOutputVolume(const SoundEngine                         &engine,
		                                               const std::shared_ptr<SoundEngineSession> &session,
		                                               int                                        buffer)
		{
			return engine.activeOutputVolumeForTest(session, buffer);
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
		 * @brief Reads startup playback readiness and physical sink gain from the sound worker.
		 * @param engine Sound engine whose initialized playback objects are inspected.
		 * @return Source-free backend availability and sink gain, or minus one without a sink.
		 */
		[[nodiscard]] static PlaybackInitializationState
		playbackInitializationState(const SoundEngine &engine)
		{
			const SoundEngine::PlaybackInitializationStateForTest state =
			    engine.playbackInitializationStateForTest();
			return {state.mediaPlayerReady, state.outputVolume};
		}

		/**
		 * @brief Reads the process-wide sound-file catalog state from the worker.
		 * @param engine Sound engine whose catalog is inspected.
		 * @return Catalog sizes and initial-scan completion state.
		 */
		[[nodiscard]] static FileCatalogState fileCatalogState(const SoundEngine &engine)
		{
			const SoundEngine::FileCatalogStateForTest state = engine.fileCatalogStateForTest();
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
		}

		/**
		 * @brief Pauses or resumes low-priority catalog events.
		 * @param engine Sound engine whose worker owns the catalog.
		 * @param paused Whether later catalog events should remain suspended.
		 */
		static void setCatalogPaused(const SoundEngine &engine, const bool paused)
		{
			engine.setCatalogPausedForTest(paused);
		}

		/**
		 * @brief Pauses or resumes persistent-index ingestion.
		 * @param engine Sound engine whose I/O worker owns the index reader.
		 * @param paused Whether index batches must remain suspended.
		 */
		static void setIndexReadPaused(const SoundEngine &engine, const bool paused)
		{
			engine.setIndexReadPausedForTest(paused);
		}

		/**
		 * @brief Publishes synthetic catalog paths through the production bounded directory handoff.
		 * @param engine Sound engine whose workers receive the paths.
		 * @param fileCount Number of paths to publish.
		 * @return `true` when the quiescent catalog accepted the publication.
		 */
		[[nodiscard]] static bool publishSyntheticCatalogDirectory(const SoundEngine &engine,
		                                                           const qsizetype    fileCount)
		{
			return engine.publishSyntheticCatalogDirectoryForTest(fileCount);
		}

		/**
		 * @brief Applies the production output-device reset on the owning worker.
		 * @param engine Owning sound engine.
		 */
		static void resetForAudioDeviceChange(const SoundEngine &engine)
		{
			engine.resetAudioDeviceForTest();
		}

		/**
		 * @brief Counts pending file occurrences or waiting commits through the worker's ordered test boundary.
		 * @param engine Owning engine.
		 * @param session Owning session.
		 * @param commitsOnly Whether only requests retaining a commit are counted.
		 * @return Number of unresolved occurrences.
		 */
		[[nodiscard]] static qsizetype pendingFileCount(const SoundEngine                         &engine,
		                                                const std::shared_ptr<SoundEngineSession> &session,
		                                                const bool commitsOnly = false)
		{
			return engine.pendingFileCountForTest(session, commitsOnly);
		}

		/**
		 * @brief Reads how many file requests required full filesystem resolution.
		 * @param engine Sound engine whose worker resolution path is inspected.
		 * @return Number of slow file-resolution attempts.
		 */
		[[nodiscard]] static quint64 slowFileResolutionCount(const SoundEngine &engine)
		{
			return engine.slowFileResolutionCountForTest();
		}

		/**
		 * @brief Reads one worker-owned native playback stream.
		 * @param engine Sound engine whose worker owns the session.
		 * @param session Session owning the playback stream.
		 * @param protocol Native protocol namespace.
		 * @param media Native media namespace.
		 * @return Current logical playback state.
		 */
		[[nodiscard]] static NativePlaybackState
		nativePlaybackState(const SoundEngine &engine, const std::shared_ptr<SoundEngineSession> &session,
		                    const QMudNativeSoundProtocol protocol, const QMudNativeSoundMedia media)
		{
			const SoundEngine::NativePlaybackStateForTest state =
			    engine.nativePlaybackStateForTest(session, protocol, media);
			return {state.buffer, state.filePattern, state.priority, state.current};
		}

		/**
		 * @brief Attempts one catalog watch through the production registration path.
		 * @param engine Sound engine whose watcher receives the path.
		 * @param directory Directory to register.
		 * @return `true` when registration succeeded.
		 */
		[[nodiscard]] static bool attemptCatalogWatch(const SoundEngine &engine, const QString &directory)
		{
			return engine.attemptCatalogWatchForTest(directory);
		}

		/**
		 * @brief Reconciles the configured sounds root through the production watcher transition.
		 * @param engine Sound engine whose catalog root is reconciled.
		 */
		static void reconcileSoundRoot(const SoundEngine &engine)
		{
			engine.reconcileSoundRootForTest();
		}

		/**
		 * @brief Reconciles one catalog directory through the production watcher transition.
		 * @param engine Sound engine whose catalog directory is reconciled.
		 * @param directory Changed catalog directory.
		 */
		static void reconcileCatalogDirectory(const SoundEngine &engine, const QString &directory)
		{
			engine.reconcileCatalogDirectoryForTest(directory);
		}

		/**
		 * @brief Runs one production periodic maintenance pass.
		 * @param engine Sound engine whose worker performs maintenance.
		 */
		static void runPeriodicMaintenance(const SoundEngine &engine)
		{
			engine.runPeriodicMaintenanceForTest();
		}

		/**
		 * @brief Replaces the native-download inactivity timeout for deterministic tests.
		 * @param engine Sound engine whose worker starts later download requests.
		 * @param milliseconds Positive no-progress interval.
		 */
		static void setDownloadStallTimeout(const SoundEngine &engine, const int milliseconds)
		{
			engine.setDownloadStallTimeoutForTest(milliseconds);
		}

		/**
		 * @brief Processes an existing file through native download completion.
		 * @param engine Sound engine whose I/O worker owns the download path.
		 * @param destination Existing sound file beneath the configured sounds root.
		 */
		static void processExistingDownload(const SoundEngine &engine, const QString &destination)
		{
			engine.processExistingDownloadForTest(destination);
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
		 * @brief Forces startup device discovery to report no usable output.
		 * @param engine Sound engine whose next initialization attempt is rejected.
		 */
		static void forceNoAudioOutput(const SoundEngine &engine)
		{
			engine.forceNoAudioOutputForTest();
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
