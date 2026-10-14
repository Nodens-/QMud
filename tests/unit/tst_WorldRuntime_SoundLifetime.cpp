/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldRuntime_SoundLifetime.cpp
 * Role: Unit coverage for threaded sound-session lifetime, command ordering, and cache maintenance.
 */

#include "NativePluginRegistry.h"
#include "SoundEngine.h"
#include "SoundEngineTestAccess.h"
#include "WorldRuntime.h"

#include "scripting/ScriptingErrors.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QSemaphore>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryDir>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include <atomic>
#include <limits>
#include <memory>

namespace
{
	/**
	 * @brief Returns a repository sound fixture accepted by the production path resolver.
	 * @return Absolute fixture path contained by the test QMud home.
	 */
	QString testSoundFileName()
	{
		return QDir(QStringLiteral(QMUD_TEST_SOURCE_DIR))
		    .filePath(QStringLiteral("skeleton/sounds/QMud/beep.wav"));
	}

	/**
	 * @brief QTest fixture covering sound-session lifecycle, ordering, snapshots, and cache maintenance.
	 */
	class tst_WorldRuntime_SoundLifetime : public QObject
	{
			Q_OBJECT

			// NOLINTBEGIN(readability-convert-member-functions-to-static)
		private slots:
			/** @brief Verifies both real playback implementations are initialized before the first request. */
			void playbackBackendsInitializeDuringApplicationStartup()
			{
				SoundEngine engine;
				QSignalSpy  initializedSpy(&engine, &SoundEngine::playbackInitializationFinished);
				engine.initializePlayback();
				const SoundEngineTestAccess::PlaybackInitializationState state =
				    SoundEngineTestAccess::playbackInitializationState(engine);
				QVERIFY(state.effectReady);
				QVERIFY(state.mediaPlayerReady);
				QTRY_COMPARE(initializedSpy.count(), 1);
				QCOMPARE(initializedSpy.first().first().toBool(), true);
				const SoundEngineTestAccess::CacheState cache = SoundEngineTestAccess::cacheState(engine);
				QCOMPARE(cache.sourceCount, 0);
				QCOMPARE(cache.backendCount, 0);
				QCOMPARE(cache.totalBytes, 0);
			}

			/** @brief Verifies a stalled startup disables sound and reports completion without waiting for the worker. */
			void playbackInitializationWatchdogDisablesStalledEngine()
			{
				SoundEngine engine;
				QSemaphore  workerEntered;
				QSemaphore  releaseWorker;
				auto        releaseGuard = qScopeGuard([&releaseWorker] { releaseWorker.release(); });
				QSignalSpy  initializationSpy(&engine, &SoundEngine::playbackInitializationFinished);
				SoundEngineTestAccess::setPlaybackInitializationBarrier(engine,
				                                                        [&workerEntered, &releaseWorker]
				                                                        {
					                                                        workerEntered.release();
					                                                        releaseWorker.acquire();
				                                                        });
				engine.initializePlayback();
				QVERIFY(workerEntered.tryAcquire(1, 5'000));
				SoundEngineTestAccess::expirePlaybackInitializationWatchdog(engine);
				QCOMPARE(initializationSpy.count(), 1);
				QCOMPARE(initializationSpy.first().first().toBool(), false);
				QVERIFY(!engine.registerSession(QStringLiteral(QMUD_TEST_SOURCE_DIR), {}));

				releaseWorker.release();
				releaseGuard.dismiss();
				QTRY_VERIFY(!SoundEngineTestAccess::soundWorkerRunning(engine));
				QCOMPARE(initializationSpy.count(), 1);
			}

			/** @brief Verifies destruction retains blocked initialization and reclaims it after worker retirement. */
			void timedOutInitializationDoesNotBlockEngineDestruction()
			{
				QSemaphore workerEntered;
				QSemaphore releaseWorker;
				auto       releaseGuard = qScopeGuard([&releaseWorker] { releaseWorker.release(); });
				auto       engine       = std::make_unique<SoundEngine>();
				SoundEngineTestAccess::setPlaybackInitializationBarrier(*engine,
				                                                        [&workerEntered, &releaseWorker]
				                                                        {
					                                                        workerEntered.release();
					                                                        releaseWorker.acquire();
				                                                        });
				engine->initializePlayback();
				QVERIFY(workerEntered.tryAcquire(1, 5'000));
				SoundEngineTestAccess::expirePlaybackInitializationWatchdog(*engine);
				QCOMPARE(SoundEngineTestAccess::retiredWorkerCount(), 0);

				engine.reset();
				QCOMPARE(SoundEngineTestAccess::retiredWorkerCount(), 1);
				releaseWorker.release();
				releaseGuard.dismiss();
				QTRY_COMPARE(SoundEngineTestAccess::retiredWorkerCount(), 0);
			}

			/**
			 * @brief Verifies invalid playback completes on the worker without allocating a channel.
			 */
			void rejectedPlaybackLeavesChannelFree()
			{
				SoundEngine engine;
				const auto  session = engine.registerSession(QStringLiteral(QMUD_TEST_SOURCE_DIR), {});
				QVERIFY(session);
				QCOMPARE(session->status(1), -2);
				const auto initialSnapshot = session->snapshot();

				QCOMPARE(session->playFile(1, QStringLiteral("missing-sound-file.wav"), false, 0.0, 0.0),
				         eFileNotFound);
				QCOMPARE(session->status(1), -2);
				QCOMPARE(session->reserveLuaAudioChannel(QStringLiteral("missing-sound-file.wav"), 39), 0);
				QCOMPARE(session->reserveLuaAudioChannel(QFileInfo(testSoundFileName()).absolutePath(), 40),
				         0);
				QVERIFY(session->snapshot() == initialSnapshot);
				QCOMPARE(session->reserveLuaAudioChannel(testSoundFileName(), 41), 1);
				QVERIFY(session->confirmLuaAudioOwnership(1, 41));
				QCOMPARE(session->stop(1), eOK);
			}

			/**
			 * @brief Verifies delayed playback revalidates a file that disappears after reservation.
			 */
			void delayedPlaybackRevalidatesReservedFile()
			{
				QTemporaryDir soundRoot(QDir::current().filePath(QStringLiteral("sound-delay-XXXXXX")));
				QVERIFY(soundRoot.isValid());
				QVERIFY(QDir(soundRoot.path()).mkpath(QStringLiteral("sounds")));
				const QString source = testSoundFileName();
				const QString target = QDir(soundRoot.path()).filePath(QStringLiteral("sounds/delayed.wav"));
				QVERIFY(QFile::copy(source, target));

				SoundEngine engine;
				const auto  session = engine.registerSession(soundRoot.path(), {});
				QVERIFY(session);
				constexpr quint64 generation = 52;
				QCOMPARE(session->reserveLuaAudioChannel(QStringLiteral("delayed.wav"), generation), 1);
				QVERIFY(session->confirmLuaAudioOwnership(1, generation));
				QCOMPARE(session->status(1), 0);
				QVERIFY(QFile::remove(target));

				int selectedBuffer = 0;
				QCOMPARE(session->playFile(1, QStringLiteral("delayed.wav"), false, 0.0, 0.0, generation,
				                           &selectedBuffer, true),
				         eFileNotFound);
				QCOMPARE(selectedBuffer, 0);
				QCOMPARE(session->status(1), -2);
			}

			/**
			 * @brief Verifies prepare and commit retain FIFO order on the worker ingress.
			 */
			void preparedFailureIsObservedByFollowingCommit()
			{
				SoundEngine engine;
				const auto  session = engine.registerSession(QStringLiteral(QMUD_TEST_SOURCE_DIR), {});
				QVERIFY(session);

				const quint64 token = session->prepareFile(QStringLiteral("missing-prepared-sound.wav"));
				QVERIFY(token != 0);
				QCOMPARE(session->commitPrepared(token, 1, false, 0.0, 0.0), eFileNotFound);
				QCOMPARE(session->status(1), -2);
			}

			/**
			 * @brief Verifies prepared decoder failures are reported only when playback is committed.
			 */
			void preparedBackendFailureHonorsCommitAndCancellation()
			{
				QStringList                   errors;
				SoundEngine                   engine;
				SoundEngine::SessionCallbacks callbacks;
				callbacks.playbackError = [&errors](const QString &message) { errors.push_back(message); };
				const auto session =
				    engine.registerSession(QStringLiteral(QMUD_TEST_SOURCE_DIR), std::move(callbacks));
				QVERIFY(session);

				const QString committedError = QStringLiteral("prepared decoder failure");
				const quint64 committedToken =
				    SoundEngineTestAccess::failedInitializedPreparedSound(engine, session, committedError);
				QVERIFY(committedToken != 0);
				QCoreApplication::processEvents();
				QVERIFY(errors.isEmpty());
				QCOMPARE(session->commitPrepared(committedToken, 1, false, 0.0, 0.0), eCannotPlaySound);
				QTRY_COMPARE(errors, QStringList{committedError});

				const quint64 canceledToken = SoundEngineTestAccess::failedPreparedSound(
				    engine, session, QStringLiteral("suppressed decoder failure"));
				QVERIFY(canceledToken != 0);
				session->cancelPrepared(canceledToken);
				QCOMPARE(session->stop(0), eOK);
				QCoreApplication::processEvents();
				QCOMPARE(errors, QStringList{committedError});
			}

			/**
			 * @brief Verifies delayed LuaAudio reservations exclude automatic selection and require their generation.
			 */
			void luaAudioReservationIsWorkerAuthoritative()
			{
				SoundEngine engine;
				const auto  session = engine.registerSession(QStringLiteral(QMUD_TEST_SOURCE_DIR), {});
				QVERIFY(session);

				constexpr quint64 firstGeneration = 41;
				for (int buffer = 1; buffer <= SoundEngineSession::kChannelCount; ++buffer)
				{
					const quint64 generation = firstGeneration + static_cast<quint64>(buffer - 1);
					QCOMPARE(session->reserveLuaAudioChannel(testSoundFileName(), generation), buffer);
					QVERIFY(session->confirmLuaAudioOwnership(buffer, generation));
					QCOMPARE(session->status(buffer), 0);
				}

				int selectedBuffer = 0;
				QCOMPARE(session->playFile(0, QStringLiteral("missing-auto.wav"), false, 0.0, 0.0, 0,
				                           &selectedBuffer),
				         eCannotPlaySound);
				QCOMPARE(selectedBuffer, 0);
				QCOMPARE(session->status(1), 0);

				QCOMPARE(session->playFile(1, QStringLiteral("missing-reserved.wav"), false, 0.0, 0.0, 99,
				                           &selectedBuffer, true),
				         eCannotPlaySound);
				QCOMPARE(session->status(1), 0);
				QCOMPARE(session->playFile(1, QStringLiteral("missing-reserved.wav"), false, 0.0, 0.0,
				                           firstGeneration, &selectedBuffer, true),
				         eFileNotFound);
				QCOMPARE(session->status(1), -2);

				for (int buffer = 2; buffer <= SoundEngineSession::kChannelCount; ++buffer)
				{
					const quint64 generation = firstGeneration + static_cast<quint64>(buffer - 1);
					QCOMPARE(session->stopLuaAudioChannel(buffer, generation), eOK);
					QCOMPARE(session->status(buffer), -2);
				}
				constexpr quint64 replacementGeneration = 100;
				QCOMPARE(session->reserveLuaAudioChannel(testSoundFileName(), replacementGeneration), 1);
				QVERIFY(session->confirmLuaAudioOwnership(1, replacementGeneration));
				QCOMPARE(session->stopLuaAudioChannel(1, replacementGeneration), eOK);
			}

			/**
			 * @brief Verifies closing one session rejects late work without affecting another session.
			 */
			void closeRejectsLateCommandsAndKeepsOtherSessionsAlive()
			{
				SoundEngine engine;
				const auto  first  = engine.registerSession(QStringLiteral(QMUD_TEST_SOURCE_DIR), {});
				const auto  second = engine.registerSession(QStringLiteral(QMUD_TEST_SOURCE_DIR), {});
				QVERIFY(first);
				QVERIFY(second);

				engine.unregisterSession(first);
				QCOMPARE(first->status(1), -3);
				QCOMPARE(first->playFile(1, QStringLiteral("late.wav"), false, 0.0, 0.0), eCannotPlaySound);

				QCOMPARE(second->status(1), -2);
				QCOMPARE(second->playFile(1, QStringLiteral("other-missing.wav"), false, 0.0, 0.0),
				         eFileNotFound);
			}

			/**
			 * @brief Verifies an externally retained session becomes unavailable when its engine shuts down.
			 */
			void engineShutdownClosesExternallyRetainedSession()
			{
				std::shared_ptr<SoundEngineSession> session;
				{
					SoundEngine engine;
					session = engine.registerSession(QStringLiteral(QMUD_TEST_SOURCE_DIR), {});
					QVERIFY(session);
					QCOMPARE(session->status(1), -2);
				}

				QCOMPARE(session->status(1), -3);
				QCOMPARE(session->stop(0), eCannotPlaySound);
			}

			/** @brief Verifies cache policy defaults and normalization use the engine's authoritative bounds. */
			void cachePolicyUsesAuthoritativeBounds()
			{
				SoundEngine engine;
				QCOMPARE(engine.cachePolicy(), SoundEngine::CachePolicy{});

				engine.configureCache({1, 1});
				QCOMPARE(engine.cachePolicy(),
				         (SoundEngine::CachePolicy{SoundEngine::CachePolicy::kMinimumRamCacheSizeMiB,
				                                   SoundEngine::CachePolicy::kMinimumIdleEvictionSeconds}));

				engine.configureCache({std::numeric_limits<int>::max(), std::numeric_limits<int>::max()});
				QCOMPARE(engine.cachePolicy(),
				         (SoundEngine::CachePolicy{SoundEngine::CachePolicy::kMaximumRamCacheSizeMiB,
				                                   SoundEngine::CachePolicy::kMaximumIdleEvictionSeconds}));
			}

			/**
			 * @brief Verifies only the periodic pass evicts one idle source per event until under budget.
			 */
			void cachePolicyMaintenanceIsIncrementalAndProtectsActiveSources()
			{
				constexpr qint64 mebibyte = 1024 * 1024;
				SoundEngine      engine;
				SoundEngineTestAccess::addCacheSource(engine, QStringLiteral("active"), 50 * mebibyte, false,
				                                      50);
				SoundEngineTestAccess::addCacheSource(engine, QStringLiteral("old-idle"), 20 * mebibyte, true,
				                                      100);
				SoundEngineTestAccess::addCacheSource(engine, QStringLiteral("new-idle"), 20 * mebibyte, true,
				                                      200);
				QCOMPARE(SoundEngineTestAccess::cacheState(engine).totalBytes, 90 * mebibyte);

				const SoundEngineTestAccess::CacheMaintenanceRun maintenanceRun =
				    SoundEngineTestAccess::configureCacheAndRunMaintenance(
				        engine, {SoundEngine::CachePolicy::kMinimumRamCacheSizeMiB,
				                 SoundEngine::CachePolicy::kMaximumIdleEvictionSeconds});
				const SoundEngineTestAccess::CacheState &configured = maintenanceRun.configured;
				QCOMPARE(configured.totalBytes, 90 * mebibyte);
				QCOMPARE(configured.sourceCount, 3);
				QCOMPARE(configured.backendCount, 3);
				QVERIFY(!configured.maintenancePending);

				const SoundEngineTestAccess::CacheState &firstStep = maintenanceRun.firstStep;
				QCOMPARE(firstStep.totalBytes, 70 * mebibyte);
				QCOMPARE(firstStep.sourceCount, 2);
				QCOMPARE(firstStep.backendCount, 2);
				QVERIFY(firstStep.maintenancePending);

				const SoundEngineTestAccess::CacheState &secondStep = maintenanceRun.secondStep;
				QCOMPARE(secondStep.totalBytes, 50 * mebibyte);
				QCOMPARE(secondStep.sourceCount, 1);
				QCOMPARE(secondStep.backendCount, 1);
				QVERIFY(!secondStep.maintenancePending);
			}

			/** @brief Verifies a canceled prepared-memory resource never enters the reusable-resource index. */
			void canceledMemoryResourceIsNotIndexed()
			{
				SoundEngine engine;
				QVERIFY(!SoundEngineTestAccess::canceledMemoryResourceIsIndexed(engine));
			}

			/**
			 * @brief Verifies status-preserving completion reuse does not publish an internal-state snapshot.
			 */
			void snapshotsRemainCoherentAndSkipInvisibleTransitions()
			{
				SoundEngine engine;
				const auto  session = engine.registerSession(QStringLiteral(QMUD_TEST_SOURCE_DIR), {});
				QVERIFY(session);
				const auto initial = session->snapshot();
				QVERIFY(initial);
				QCOMPARE(initial->status.at(0), -2);

				QCOMPARE(session->reserveLuaAudioChannel(testSoundFileName(), 41), 1);
				const auto reserved = session->snapshot();
				QVERIFY(reserved);
				QVERIFY(reserved != initial);
				QCOMPARE(reserved->revision, initial->revision + 1);
				QCOMPARE(reserved->status.at(0), 0);

				QVERIFY(session->confirmLuaAudioOwnership(1, 41));
				QVERIFY(session->snapshot() == reserved);
				QVERIFY(SoundEngineTestAccess::completeReservedChannel(engine, session, 1));
				const auto completed = session->snapshot();
				QVERIFY(completed);
				QVERIFY(completed != reserved);
				QCOMPARE(completed->revision, reserved->revision + 2);
				QCOMPARE(completed->status.at(0), 0);

				QCOMPARE(session->reserveLuaAudioChannel(testSoundFileName(), 42), 1);
				QVERIFY(session->snapshot() == completed);
				QVERIFY(session->confirmLuaAudioOwnership(1, 42));
				QVERIFY(session->snapshot() == completed);
				QCOMPARE(session->stopLuaAudioChannel(1, 42), eOK);
				const auto stopped = session->snapshot();
				QVERIFY(stopped);
				QVERIFY(stopped != completed);
				QCOMPARE(stopped->revision, completed->revision + 1);
				QCOMPARE(stopped->status.at(0), -2);

				QCOMPARE(session->stop(1), eOK);
				QVERIFY(session->snapshot() == stopped);
			}

			/**
			 * @brief Verifies confirmed LuaAudio ownership is released before a worker command returns.
			 */
			void confirmedLuaAudioOwnershipIsReleasedSynchronously()
			{
				WorldRuntime                  runtime;
				std::atomic_int               releaseCount{0};
				SoundEngine                   engine;
				SoundEngine::SessionCallbacks callbacks;
				callbacks.luaAudioOwnershipReleased =
				    [&runtime, &releaseCount](const int buffer, const quint64 generation)
				{
					static_cast<void>(QMudNativePluginRegistry::luaAudioReleaseRuntimeBufferIfGeneration(
					    &runtime, buffer, generation));
					releaseCount.fetch_add(1, std::memory_order_relaxed);
				};
				const auto session =
				    engine.registerSession(QStringLiteral(QMUD_TEST_SOURCE_DIR), std::move(callbacks));
				QVERIFY(session);

				constexpr quint64 generation = 41;
				QCOMPARE(session->reserveLuaAudioChannel(testSoundFileName(), generation), 1);
				QMudNativePluginRegistry::LuaAudioRuntimeBufferState state;
				state.generation = generation;
				QMudNativePluginRegistry::luaAudioMarkRuntimeBuffer(&runtime, 1, state);
				QMudNativePluginRegistry::LuaAudioRuntimeOperationGuard operationGuard;
				QVERIFY(QMudNativePluginRegistry::luaAudioCaptureRuntimeBufferOperationGuard(&runtime, 1,
				                                                                             operationGuard));
				const auto reservedSnapshot = session->snapshot();
				QVERIFY(session->confirmLuaAudioOwnership(1, generation));
				QVERIFY(session->snapshot() == reservedSnapshot);

				QCOMPARE(session->stop(1), eOK);
				QCOMPARE(releaseCount.load(std::memory_order_relaxed), 1);
				QVERIFY(operationGuard.cancelled->load(std::memory_order_acquire));
				QVERIFY(!QMudNativePluginRegistry::luaAudioRuntimeBufferState(&runtime, 1, state));
			}

			/**
			 * @brief Verifies confirmation synchronously reconciles ownership released before registration.
			 */
			void completedLuaAudioOwnershipIsReleasedDuringConfirmation()
			{
				WorldRuntime                  runtime;
				std::atomic_int               releaseCount{0};
				SoundEngine                   engine;
				SoundEngine::SessionCallbacks callbacks;
				callbacks.luaAudioOwnershipReleased =
				    [&runtime, &releaseCount](const int buffer, const quint64 generation)
				{
					static_cast<void>(QMudNativePluginRegistry::luaAudioReleaseRuntimeBufferIfGeneration(
					    &runtime, buffer, generation));
					releaseCount.fetch_add(1, std::memory_order_relaxed);
				};
				const auto session =
				    engine.registerSession(QStringLiteral(QMUD_TEST_SOURCE_DIR), std::move(callbacks));
				QVERIFY(session);

				constexpr quint64 generation = 42;
				QCOMPARE(session->reserveLuaAudioChannel(testSoundFileName(), generation), 1);
				QCOMPARE(session->stop(1), eOK);
				QCOMPARE(releaseCount.load(std::memory_order_relaxed), 0);
				QMudNativePluginRegistry::LuaAudioRuntimeBufferState state;
				state.generation = generation;
				QMudNativePluginRegistry::luaAudioMarkRuntimeBuffer(&runtime, 1, state);

				QVERIFY(session->confirmLuaAudioOwnership(1, generation));
				QCOMPARE(releaseCount.load(std::memory_order_relaxed), 1);
				QVERIFY(!QMudNativePluginRegistry::luaAudioRuntimeBufferState(&runtime, 1, state));
			}

			/** @brief Verifies stale LuaAudio updates and stops cannot affect a reused channel generation. */
			void luaAudioCommandsRequireTheCurrentGeneration()
			{
				SoundEngine engine;
				const auto  session = engine.registerSession(QStringLiteral(QMUD_TEST_SOURCE_DIR), {});
				QVERIFY(session);

				constexpr quint64 generation = 44;
				QCOMPARE(session->reserveLuaAudioChannel(testSoundFileName(), generation), 1);
				QVERIFY(session->confirmLuaAudioOwnership(1, generation));
				const auto reservedSnapshot = session->snapshot();
				QCOMPARE(session->playFile(1, QString(), false, -20.0, 0.0, generation + 1),
				         eCannotPlaySound);
				QVERIFY(session->snapshot() == reservedSnapshot);
				QCOMPARE(session->stopLuaAudioChannel(1, generation + 1), eCannotPlaySound);
				QVERIFY(session->snapshot() == reservedSnapshot);
				QCOMPARE(session->stopLuaAudioChannel(1, generation), eOK);
				QVERIFY(session->snapshot() != reservedSnapshot);
			}

			/** @brief Verifies session closure releases confirmed ownership before forming its callback barrier. */
			void sessionCloseReleasesConfirmedLuaAudioOwnership()
			{
				WorldRuntime                  runtime;
				SoundEngine                   engine;
				SoundEngine::SessionCallbacks callbacks;
				callbacks.luaAudioOwnershipReleased = [&runtime](const int buffer, const quint64 generation)
				{
					static_cast<void>(QMudNativePluginRegistry::luaAudioReleaseRuntimeBufferIfGeneration(
					    &runtime, buffer, generation));
				};
				const auto session =
				    engine.registerSession(QStringLiteral(QMUD_TEST_SOURCE_DIR), std::move(callbacks));
				QVERIFY(session);

				constexpr quint64 generation = 43;
				QCOMPARE(session->reserveLuaAudioChannel(testSoundFileName(), generation), 1);
				QMudNativePluginRegistry::LuaAudioRuntimeBufferState state;
				state.generation = generation;
				QMudNativePluginRegistry::luaAudioMarkRuntimeBuffer(&runtime, 1, state);
				QVERIFY(session->confirmLuaAudioOwnership(1, generation));

				session->close();
				QVERIFY(!QMudNativePluginRegistry::luaAudioRuntimeBufferState(&runtime, 1, state));
				QCOMPARE(session->stop(1), eCannotPlaySound);
			}
			// NOLINTEND(readability-convert-member-functions-to-static)
	};
} // namespace

QTEST_MAIN(tst_WorldRuntime_SoundLifetime)

#include "tst_WorldRuntime_SoundLifetime.moc"
