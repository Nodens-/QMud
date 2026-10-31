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
#include "TestSoundData.h"
#include "WorldDocument.h"
#include "WorldRuntime.h"
#include "WorldRuntimeTestAccess.h"

#include "scripting/ScriptingErrors.h"

#include <QDataStream>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTcpServer>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTcpSocket>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace
{
	/**
	 * @brief Returns the test QMud home containing the repository sound fixture.
	 * @return Absolute test QMud home path.
	 */
	QString testSoundHome()
	{
		return qEnvironmentVariable("QMUD_HOME");
	}

	/**
	 * @brief Returns a repository sound fixture accepted by the production path resolver.
	 * @return Absolute fixture path contained by the test QMud home.
	 */
	QString testSoundFileName()
	{
		return QDir(testSoundHome()).filePath(QStringLiteral("sounds/QMud/beep.wav"));
	}

	/** @brief One directory record written to the version-1 persistent sound-index fixture. */
	struct TestSoundIndexDirectory
	{
			QString     relativePath;
			QString     relativeParent;
			QStringList files;
	};

	/**
	 * @brief Writes a version-1 directory-record sound index for catalog tests.
	 * @param path Destination index path.
	 * @param directories Directory records and their direct file names.
	 * @return `true` when the complete fixture was written.
	 */
	bool writeSoundIndex(const QString &path, const QList<TestSoundIndexDirectory> &directories)
	{
		QFile index(path);
		if (!index.open(QIODevice::WriteOnly))
			return false;
		QDataStream stream(&index);
		stream.setVersion(QDataStream::Qt_6_0);
		stream << quint32{0x514D5349U} << quint32{1U} << static_cast<quint64>(directories.size());
		for (const TestSoundIndexDirectory &directory : directories)
		{
			stream << directory.relativePath << directory.relativeParent
			       << static_cast<quint64>(directory.files.size());
			for (const QString &file : directory.files)
				stream << file << qint64{1} << qint64{1};
		}
		return stream.status() == QDataStream::Ok;
	}

	/**
	 * @brief Observes asynchronous state using Qt's deadline-based wait and returns its last sampled value.
	 * @tparam Probe Callable reading the state under test.
	 * @tparam Expected Expected state type.
	 * @param probe State reader invoked while Qt events continue to be processed.
	 * @param expected State that completes the wait.
	 * @return Last observed state for the caller's assertion, including on timeout or an earlier test failure.
	 */
	template <typename Probe, typename Expected> auto waitForSoundValue(Probe probe, const Expected &expected)
	{
		auto observed = probe();
		if (observed != expected)
			static_cast<void>(QTest::qWaitFor(
			    [&]
			    {
				    observed = probe();
				    return observed == expected || (QTest::runningTest() && QTest::currentTestResolved());
			    }));
		return observed;
	}

	/**
	 * @brief Configures the production sounds catalog around the repository fixture and waits for its lazy scan.
	 * @param engine Sound engine whose worker owns the catalog.
	 */
	void configureTestSoundCatalog(SoundEngine &engine)
	{
		engine.initializePlayback();
		engine.configureSoundDirectory(testSoundHome());
		QVERIFY(waitForSoundValue(
		    [&] { return static_cast<bool>(SoundEngineTestAccess::fileCatalogState(engine).ready); }, true));
	}

	/**
	 * @brief Configures the test QMud home and registers one session through the production ordering boundary.
	 * @param engine Sound engine receiving the process-wide directory before the session command.
	 * @param callbacks Optional session callbacks.
	 * @param worldSoundDirectory Optional persisted per-world preferred sound directory.
	 * @return Registered session, or an empty pointer when the worker is unavailable.
	 */
	std::shared_ptr<SoundEngineSession> registerTestSession(const SoundEngine            &engine,
	                                                        SoundEngine::SessionCallbacks callbacks = {},
	                                                        const QString &worldSoundDirectory      = {})
	{
		engine.configureSoundDirectory(testSoundHome());
		return engine.registerSession(std::move(callbacks), worldSoundDirectory);
	}

	/**
	 * @brief Builds one valid native protocol playback request for sound-worker tests.
	 * @param filePattern Relative native sound pattern.
	 * @param media Native media class.
	 * @return Request with protocol defaults and the supplied identity.
	 */
	QMudNativeSoundRequest nativeSoundRequest(const QString             &filePattern,
	                                          const QMudNativeSoundMedia media = QMudNativeSoundMedia::Sound)
	{
		QMudNativeSoundRequest request;
		request.protocol    = QMudNativeSoundProtocol::Msp;
		request.media       = media;
		request.filePattern = filePattern;
		return request;
	}

	/**
	 * @brief Writes an isolated encoded-audio fixture beneath a build-local test home.
	 * @param root Isolated QMud home.
	 * @param relative Home-relative file.
	 * @return Absolute created filename, or an empty string on failure.
	 */
	QString soundFixture(const QString &root, const QString &relative)
	{
		const QString path = QDir(root).filePath(relative);
		if (!QDir().mkpath(QFileInfo(path).absolutePath()))
			return {};
		QFile            file(path);
		const QByteArray bytes = QMudTestSoundData::silentPcmWave(300);
		return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() ? path : QString();
	}

	/** @brief Holds one real preparation-pool task without blocking sound-worker commands. */
	class MemoryPreparationBlocker
	{
			Q_DISABLE_COPY_MOVE(MemoryPreparationBlocker)
			struct Gates
			{
					QSemaphore entered;
					QSemaphore resume;
			};
			std::shared_ptr<Gates> m_gates{std::make_shared<Gates>()};
			bool                   m_released{false};

		public:
			/**
			 * @brief Queues a barrier ahead of later hashing or materialization work.
			 * @param engine Sound engine whose ordered pool is held.
			 */
			explicit MemoryPreparationBlocker(const SoundEngine &engine)
			{
				SoundEngineTestAccess::queueMemoryPreparationTask(engine,
				                                                  [gates = m_gates]
				                                                  {
					                                                  gates->entered.release();
					                                                  gates->resume.acquire();
				                                                  });
			}
			/** @brief Releases queued work on every test exit path. */
			~MemoryPreparationBlocker()
			{
				release();
			}
			/**
			 * @brief Waits for entry into the queued synchronization task.
			 * @return Whether the task entered within the test failure deadline.
			 */
			[[nodiscard]] bool waitUntilEntered() const
			{
				return m_gates->entered.tryAcquire(1, 5000);
			}
			/** @brief Allows the ordered preparation pool to continue exactly once. */
			void release()
			{
				if (!std::exchange(m_released, true))
					m_gates->resume.release();
			}
	};

	/** @brief Owns an isolated synchronous caller and guarantees its cancellation before joining. */
	struct PendingSoundCall
	{
			std::shared_ptr<SoundEngineSession> session;
			std::atomic_bool                    finished{false};
			int                                 result{eCannotPlaySound};
			std::jthread                        caller;

			/**
			 * @brief Submits an operation from another thread while the test controls catalog progress.
			 * @param owner Session whose close operation releases a blocked caller on test failure.
			 * @param operation Synchronous sound operation.
			 */
			PendingSoundCall(std::shared_ptr<SoundEngineSession> owner, std::function<int()> operation)
			    : session(std::move(owner)), caller(
			                                     [this, operation = std::move(operation)]
			                                     {
				                                     result = operation();
				                                     finished.store(true, std::memory_order_release);
			                                     })
			{
			}

			/** @brief Closes the session before joining so failed assertions cannot strand a waiting caller. */
			~PendingSoundCall()
			{
				session->close();
				caller.join();
			}
	};

	/**
	 * @brief QTest fixture covering sound-session lifecycle, ordering, snapshots, and cache maintenance.
	 */
	class tst_WorldRuntime_SoundLifetime : public QObject
	{
			Q_OBJECT

			// NOLINTBEGIN(readability-convert-member-functions-to-static)
		private slots:
			/** @brief Verifies the media-player backend is initialized before the first request. */
			void playbackBackendInitializesDuringApplicationStartup()
			{
				QVERIFY(SoundEngine::isOutputMuted());
				SoundEngine engine;
				QSignalSpy  initializedSpy(&engine, &SoundEngine::playbackInitializationFinished);
				engine.initializePlayback();
				const SoundEngineTestAccess::PlaybackInitializationState state =
				    SoundEngineTestAccess::playbackInitializationState(engine);
				QVERIFY(state.mediaPlayerReady);
				QCOMPARE(state.outputVolume, 0.0);
				QTRY_COMPARE(initializedSpy.count(), 1);
				QCOMPARE(initializedSpy.first().first().toBool(), true);
				const SoundEngineTestAccess::CacheState cache = SoundEngineTestAccess::cacheState(engine);
				QCOMPARE(cache.sourceCount, 0);
				QCOMPARE(cache.backendCount, 0);
				QCOMPARE(cache.totalBytes, 0);
			}

			/** @brief Verifies new, reused, and recreated sinks stay silent while real playback controls operate. */
			void testOutputMuteSurvivesPlaybackControlsAndDeviceReset()
			{
				QVERIFY(SoundEngine::isOutputMuted());
				SoundEngine engine;
				const auto  session = registerTestSession(engine);
				QVERIFY(session);
				const auto payload = QMudTestSoundData::silentPcmWave();
				for (const int buffer : {1, 2})
				{
					QCOMPARE(session->playMemory(buffer, payload, true, 0.0, 0.0), eOK);
					QCOMPARE(session->status(buffer), 2);
					QCOMPARE(session->snapshot()->volume[static_cast<std::size_t>(buffer - 1)], 100.0);
					QCOMPARE(SoundEngineTestAccess::activeAttribute(engine, session, buffer,
					                                                QMudSoundAttribute::Volume),
					         100.0);
					QCOMPARE(SoundEngineTestAccess::activeOutputVolume(engine, session, buffer), 0.0);
				}
				QCOMPARE(SoundEngineTestAccess::cacheState(engine).backendCount, 2);
				QCOMPARE(session->playFile(1, {}, true, -25.0, 0.0), eOK);
				QCOMPARE(
				    SoundEngineTestAccess::activeAttribute(engine, session, 1, QMudSoundAttribute::Volume),
				    75.0);
				QCOMPARE(SoundEngineTestAccess::activeOutputVolume(engine, session, 1), 0.0);
				QCOMPARE(session->stop(1), eOK);
				QCOMPARE(session->playMemory(1, payload, true, 0.0, 0.0), eOK);
				QCOMPARE(SoundEngineTestAccess::cacheState(engine).backendCount, 2);
				QCOMPARE(SoundEngineTestAccess::activeOutputVolume(engine, session, 1), 0.0);
				SoundEngineTestAccess::resetForAudioDeviceChange(engine);
				const auto initialized = SoundEngineTestAccess::playbackInitializationState(engine);
				QVERIFY(initialized.mediaPlayerReady);
				QCOMPARE(initialized.outputVolume, 0.0);
				QCOMPARE(session->playMemory(1, payload, true, 0.0, 0.0), eOK);
				QCOMPARE(SoundEngineTestAccess::activeOutputVolume(engine, session, 1), 0.0);
				QCOMPARE(session->stop(0), eOK);
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
				QVERIFY(!registerTestSession(engine));

				releaseWorker.release();
				releaseGuard.dismiss();
				QTRY_VERIFY(!SoundEngineTestAccess::soundWorkerRunning(engine));
				QCOMPARE(initializationSpy.count(), 1);
			}

			/** @brief Verifies a null default output rejects startup instead of advertising sound support. */
			void playbackInitializationRejectsMissingOutputDevice()
			{
				SoundEngine engine;
				QSignalSpy  initializationSpy(&engine, &SoundEngine::playbackInitializationFinished);
				SoundEngineTestAccess::forceNoAudioOutput(engine);
				engine.initializePlayback();

				QTRY_COMPARE(initializationSpy.count(), 1);
				QCOMPARE(initializationSpy.first().first().toBool(), false);
				QVERIFY(!registerTestSession(engine));
				QTRY_VERIFY(!SoundEngineTestAccess::soundWorkerRunning(engine));
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
				const auto  session = registerTestSession(engine);
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

				QTemporaryDir outsideSounds(
				    QDir::current().filePath(QStringLiteral("sound-outside-home-XXXXXX")));
				QVERIFY(outsideSounds.isValid());
				const QString outsideFile =
				    QDir(outsideSounds.path()).filePath(QStringLiteral("outside.wav"));
				QVERIFY(QFile::copy(testSoundFileName(), outsideFile));
				QCOMPARE(session->playFile(0, outsideFile, false, 0.0, 0.0), eFileNotFound);
				QCOMPARE(session->reserveLuaAudioChannel(outsideFile, 42), 0);
			}

			/** @brief Supplies each non-finite value for every initial playback control. */
			void nonFinitePlaybackControlsPreserveOwnership_data()
			{
				QTest::addColumn<double>("invalid");
				QTest::addColumn<int>("control");
				constexpr std::array values{std::numeric_limits<double>::quiet_NaN(),
				                            std::numeric_limits<double>::infinity(),
				                            -std::numeric_limits<double>::infinity()};
				for (int value = 0; value < 3; ++value)
					for (int control = 0; control < 3; ++control)
						QTest::newRow(
						    qPrintable(QStringLiteral("value-%1-control-%2").arg(value).arg(control)))
						    << values[static_cast<std::size_t>(value)] << control;
			}

			/** @brief Verifies rejected plays, updates, reservations, and commits cannot disturb an existing owner. */
			void nonFinitePlaybackControlsPreserveOwnership()
			{
				QFETCH(double, invalid);
				QFETCH(int, control);
				SoundEngine engine;
				const auto  session = registerTestSession(engine);
				QVERIFY(session);
				QCOMPARE(
				    session->playFile(1, testSoundFileName(), true, -55.0, 15.0, 71, nullptr, false, 3.0),
				    eOK);
				QVERIFY(session->confirmLuaAudioOwnership(1, 71));
				QCOMPARE(session->reserveLuaAudioChannel(testSoundFileName(), 72, 45.0, 15.0, 3.0), 2);
				QVERIFY(session->confirmLuaAudioOwnership(2, 72));
				const quint64 prepared = session->prepareFile(testSoundFileName());
				QVERIFY(prepared != 0);
				const auto   backends  = SoundEngineTestAccess::cacheState(engine).backendCount;
				const auto   before    = session->snapshot();
				const double volume    = control == 0 ? invalid : -55.0;
				const double pan       = control == 1 ? invalid : 15.0;
				const double pitch     = control == 2 ? invalid : 3.0;
				int          selected  = -1;
				auto         postState = before;
				QCOMPARE(session->playFile(1, testSoundFileName(), false, volume, pan, 73, &selected, false,
				                           pitch, &postState),
				         eBadParameter);
				QCOMPARE(session->playFile(1, QString(), false, volume, pan, 0, &selected, false, pitch,
				                           &postState),
				         eBadParameter);
				QCOMPARE(session->playFile(2, testSoundFileName(), false, volume, pan, 72, &selected, true,
				                           pitch, &postState),
				         eBadParameter);
				QCOMPARE(session->reserveLuaAudioChannel(testSoundFileName(), 73, volume, pan, pitch), 0);
				QCOMPARE(
				    session->setLuaAudioAttribute(1, 71, static_cast<QMudSoundAttribute>(control), invalid),
				    eBadParameter);
				QCOMPARE(
				    session->setLuaAudioAttribute(2, 72, static_cast<QMudSoundAttribute>(control), invalid),
				    eBadParameter);
				if (control != 2)
				{
					QCOMPARE(session->playMemory(1, QMudTestSoundData::silentPcmWave(300), false, volume, pan,
					                             0, &selected, &postState),
					         eBadParameter);
					QCOMPARE(session->commitPrepared(prepared, 1, false, volume, pan), eBadParameter);
				}
				QCOMPARE(selected, -1);
				QVERIFY(postState == before);
				QVERIFY(session->snapshot() == before);
				QCOMPARE(SoundEngineTestAccess::cacheState(engine).backendCount, backends);
				QCOMPARE(SoundEngineTestAccess::pendingFileCount(engine, session), 0);
				QCOMPARE(
				    SoundEngineTestAccess::activeAttribute(engine, session, 1, QMudSoundAttribute::Volume),
				    45.0);
				QCOMPARE(SoundEngineTestAccess::activeAttribute(engine, session, 1, QMudSoundAttribute::Pan),
				         15.0);
				QCOMPARE(
				    SoundEngineTestAccess::activeAttribute(engine, session, 1, QMudSoundAttribute::Pitch),
				    3.0);
				// Invalid commits retain the preparation; invalid reservation plays retain their owner and controls.
				QCOMPARE(session->commitPrepared(prepared, 1, true, -55.0, 15.0), eOK);
				QCOMPARE(session->playFile(2, testSoundFileName(), true, 0.0, 0.0, 72, &selected, true), eOK);
				QCOMPARE(selected, 2);
				QCOMPARE(
				    SoundEngineTestAccess::activeAttribute(engine, session, 2, QMudSoundAttribute::Volume),
				    45.0);
				QCOMPARE(SoundEngineTestAccess::activeAttribute(engine, session, 2, QMudSoundAttribute::Pan),
				         15.0);
				QCOMPARE(
				    SoundEngineTestAccess::activeAttribute(engine, session, 2, QMudSoundAttribute::Pitch),
				    3.0);
			}

			/** @brief Verifies an absolute path beneath QMud home cannot alias a file beneath sounds. */
			void absolutePathOutsideSoundsIsRejected()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-absolute-path-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("sounds/logs")));
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("logs")));
				const QString outsideFile = QDir(root.path()).filePath(QStringLiteral("logs/collision.wav"));
				const QString aliasedFile =
				    QDir(root.path()).filePath(QStringLiteral("sounds/logs/collision.wav"));
				QVERIFY(QFile::copy(testSoundFileName(), outsideFile));
				QVERIFY(QFile::copy(testSoundFileName(), aliasedFile));

				SoundEngine engine;
				engine.initializePlayback();
				engine.configureSoundDirectory(root.path());
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				const auto session = engine.registerSession({});
				QVERIFY(session);
				QCOMPARE(session->playFile(0, outsideFile, false, 0.0, 0.0), eFileNotFound);
				QCOMPARE(session->reserveLuaAudioChannel(outsideFile, 43), 0);
			}

			/** @brief Verifies native sound ownership never stops a channel reassigned to another caller. */
			void nativeSoundOwnershipIsGenerationChecked()
			{
				SoundEngine engine;
				configureTestSoundCatalog(engine);
				const auto session = registerTestSession(engine);
				QVERIFY(session);
				QMudNativeSoundRequest request = nativeSoundRequest(QStringLiteral("QMud/beep.wav"));
				request.repeatCount            = -1;
				QVERIFY(session->submitNativeSound(request));
				SoundEngineTestAccess::NativePlaybackState playback =
				    SoundEngineTestAccess::nativePlaybackState(engine, session, QMudNativeSoundProtocol::Msp,
				                                               QMudNativeSoundMedia::Sound);
				QVERIFY(playback.current);
				const int selectedBuffer = playback.buffer;
				QCOMPARE(session->status(selectedBuffer), 2);

				QCOMPARE(session->playFile(selectedBuffer, testSoundFileName(), true, 0.0, 0.0), eOK);
				request.stop = true;
				QVERIFY(session->submitNativeSound(request));
				QVERIFY(!SoundEngineTestAccess::nativePlaybackState(
				             engine, session, QMudNativeSoundProtocol::Msp, QMudNativeSoundMedia::Sound)
				             .current);
				QCOMPARE(session->status(selectedBuffer), 2);
				QCOMPARE(session->stop(selectedBuffer), eOK);
			}

			/** @brief Verifies native priority and protocol namespaces are applied in worker queue order. */
			void nativeSoundPriorityAndProtocolStateAreWorkerOwned()
			{
				SoundEngine engine;
				configureTestSoundCatalog(engine);
				const auto session = registerTestSession(engine);
				QVERIFY(session);

				QMudNativeSoundRequest request = nativeSoundRequest(QStringLiteral("QMud/beep.wav"));
				request.priority               = 80;
				request.repeatCount            = -1;
				QVERIFY(session->submitNativeSound(request));
				SoundEngineTestAccess::NativePlaybackState mspPlayback =
				    SoundEngineTestAccess::nativePlaybackState(engine, session, QMudNativeSoundProtocol::Msp,
				                                               QMudNativeSoundMedia::Sound);
				QVERIFY(mspPlayback.current);
				QCOMPARE(mspPlayback.filePattern, QStringLiteral("QMud/beep.wav"));

				request.filePattern = QStringLiteral("beep.wav");
				request.priority    = 70;
				QVERIFY(session->submitNativeSound(request));
				mspPlayback = SoundEngineTestAccess::nativePlaybackState(
				    engine, session, QMudNativeSoundProtocol::Msp, QMudNativeSoundMedia::Sound);
				QCOMPARE(mspPlayback.filePattern, QStringLiteral("QMud/beep.wav"));
				QCOMPARE(mspPlayback.priority, 80);

				request.priority = 90;
				QVERIFY(session->submitNativeSound(request));
				mspPlayback = SoundEngineTestAccess::nativePlaybackState(
				    engine, session, QMudNativeSoundProtocol::Msp, QMudNativeSoundMedia::Sound);
				QVERIFY(mspPlayback.current);
				QCOMPARE(mspPlayback.filePattern, QStringLiteral("beep.wav"));
				QCOMPARE(mspPlayback.priority, 90);

				request.protocol    = QMudNativeSoundProtocol::Mxp;
				request.filePattern = QStringLiteral("QMud/beep.wav");
				request.priority    = 50;
				QVERIFY(session->submitNativeSound(request));
				const SoundEngineTestAccess::NativePlaybackState mxpPlayback =
				    SoundEngineTestAccess::nativePlaybackState(engine, session, QMudNativeSoundProtocol::Mxp,
				                                               QMudNativeSoundMedia::Sound);
				QVERIFY(mxpPlayback.current);
				QVERIFY(mxpPlayback.buffer != mspPlayback.buffer);

				request.protocol = QMudNativeSoundProtocol::Msp;
				request.stop     = true;
				QVERIFY(session->submitNativeSound(request));
				QVERIFY(!SoundEngineTestAccess::nativePlaybackState(
				             engine, session, QMudNativeSoundProtocol::Msp, QMudNativeSoundMedia::Sound)
				             .current);
				QVERIFY(SoundEngineTestAccess::nativePlaybackState(
				            engine, session, QMudNativeSoundProtocol::Mxp, QMudNativeSoundMedia::Sound)
				            .current);
			}

			/** @brief Verifies SOUND priority does not suppress playback while resolution is pending. */
			void pendingNativeSoundDoesNotApplyPlaybackPriority()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-pending-priority-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("sounds")));
				const QString localSound = QDir(root.path()).filePath(QStringLiteral("sounds/local.wav"));
				QVERIFY(QFile::copy(testSoundFileName(), localSound));

				QTcpServer server;
				QVERIFY(server.listen(QHostAddress::LocalHost));
				connect(&server, &QTcpServer::newConnection, &server,
				        [&server]
				        {
					        while (QTcpSocket *socket = server.nextPendingConnection())
						        socket->setParent(&server);
				        });

				SoundEngine engine;
				engine.initializePlayback();
				engine.configureSoundDirectory(root.path());
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				const auto session = engine.registerSession({});
				QVERIFY(session);

				QMudNativeSoundRequest request = nativeSoundRequest(QStringLiteral("pending.wav"));
				request.priority               = 80;
				request.downloadUrl = QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort());
				QVERIFY(session->submitNativeSound(request));
				SoundEngineTestAccess::NativePlaybackState playback =
				    SoundEngineTestAccess::nativePlaybackState(engine, session, QMudNativeSoundProtocol::Msp,
				                                               QMudNativeSoundMedia::Sound);
				QVERIFY(playback.current);
				QVERIFY(
				    SoundEngineTestAccess::activeFileIdentity(engine, session, playback.buffer).isEmpty());

				request.filePattern = QStringLiteral("local.wav");
				request.downloadUrl.clear();
				request.priority = 70;
				QVERIFY(session->submitNativeSound(request));
				playback = SoundEngineTestAccess::nativePlaybackState(
				    engine, session, QMudNativeSoundProtocol::Msp, QMudNativeSoundMedia::Sound);
				QCOMPARE(playback.filePattern, QStringLiteral("local.wav"));
				QCOMPARE(playback.priority, 70);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, playback.buffer),
				         localSound);
			}

			/** @brief Verifies exact native files play before the lazy catalog completes while patterns wait. */
			void exactNativeSoundStartsWhileCatalogScanIsPending()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-pending-catalog-XXXXXX")));
				QVERIFY(root.isValid());
				const QString worldDirectory = QDir(root.path()).filePath(QStringLiteral("sounds/world-one"));
				QVERIFY(QDir().mkpath(worldDirectory));
				const QString target       = QDir(worldDirectory).filePath(QStringLiteral("tone.wav"));
				const QString sharedTarget = QDir(root.path()).filePath(QStringLiteral("sounds/tone.wav"));
				QVERIFY(QFile::copy(testSoundFileName(), target));
				QVERIFY(QFile::copy(testSoundFileName(), sharedTarget));

				SoundEngine engine;
				engine.initializePlayback();
				SoundEngineTestAccess::setCatalogPaused(engine, true);
				[[maybe_unused]] auto resumeCatalog =
				    qScopeGuard([&engine] { SoundEngineTestAccess::setCatalogPaused(engine, false); });
				engine.configureSoundDirectory(root.path());
				const auto session = engine.registerSession({}, QStringLiteral("./sounds/world-one/"));
				QVERIFY(session);
				QVERIFY(!SoundEngineTestAccess::fileCatalogState(engine).ready);

				QMudNativeSoundRequest request = nativeSoundRequest(QStringLiteral("tone.wav"));
				QVERIFY(session->submitNativeSound(request));
				int selectedBuffer =
				    SoundEngineTestAccess::nativePlaybackState(engine, session, QMudNativeSoundProtocol::Msp,
				                                               QMudNativeSoundMedia::Sound)
				        .buffer;
				QVERIFY(selectedBuffer > 0);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, selectedBuffer),
				         QFileInfo(target).canonicalFilePath());
				QVERIFY(!SoundEngineTestAccess::fileCatalogState(engine).ready);

				request.stop = true;
				QVERIFY(session->submitNativeSound(request));
				static_cast<void>(SoundEngineTestAccess::nativePlaybackState(
				    engine, session, QMudNativeSoundProtocol::Msp, QMudNativeSoundMedia::Sound));
				request = nativeSoundRequest(QStringLiteral("*.wav"));
				QVERIFY(session->submitNativeSound(request));
				selectedBuffer =
				    SoundEngineTestAccess::nativePlaybackState(engine, session, QMudNativeSoundProtocol::Msp,
				                                               QMudNativeSoundMedia::Sound)
				        .buffer;
				QVERIFY(selectedBuffer > 0);
				QVERIFY(SoundEngineTestAccess::activeFileIdentity(engine, session, selectedBuffer).isEmpty());

				SoundEngineTestAccess::setCatalogPaused(engine, false);
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				QCOMPARE(SoundEngineTestAccess::fileCatalogState(engine).fileCount, 2);
				QTRY_COMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, selectedBuffer),
				             QFileInfo(target).canonicalFilePath());
			}

			/** @brief Verifies native finite repeats and continuation updates use completion boundaries. */
			void nativeSoundRepeatsAdvanceAndResetOnUpdate()
			{
				SoundEngine engine;
				configureTestSoundCatalog(engine);
				const auto session = registerTestSession(engine);
				QVERIFY(session);
				QMudNativeSoundRequest request = nativeSoundRequest(QStringLiteral("QMud/beep.wav"));
				request.repeatCount            = 2;
				QVERIFY(session->submitNativeSound(request));
				int selectedBuffer =
				    SoundEngineTestAccess::nativePlaybackState(engine, session, QMudNativeSoundProtocol::Msp,
				                                               QMudNativeSoundMedia::Sound)
				        .buffer;
				QVERIFY(selectedBuffer > 0);
				QCOMPARE(SoundEngineTestAccess::slowFileResolutionCount(engine), 1);
				QVERIFY(SoundEngineTestAccess::completeActiveChannel(engine, session, selectedBuffer));
				QCOMPARE(SoundEngineTestAccess::slowFileResolutionCount(engine), 1);
				QVERIFY(SoundEngineTestAccess::nativePlaybackState(
				            engine, session, QMudNativeSoundProtocol::Msp, QMudNativeSoundMedia::Sound)
				            .current);
				QCOMPARE(session->status(selectedBuffer), 1);
				QVERIFY(SoundEngineTestAccess::completeActiveChannel(engine, session, selectedBuffer));
				QVERIFY(!SoundEngineTestAccess::nativePlaybackState(
				             engine, session, QMudNativeSoundProtocol::Msp, QMudNativeSoundMedia::Sound)
				             .current);
				QCOMPARE(session->status(selectedBuffer), 0);

				request = nativeSoundRequest(QStringLiteral("QMud/beep.wav"), QMudNativeSoundMedia::Music);
				QVERIFY(session->submitNativeSound(request));
				const int musicBuffer =
				    SoundEngineTestAccess::nativePlaybackState(engine, session, QMudNativeSoundProtocol::Msp,
				                                               QMudNativeSoundMedia::Music)
				        .buffer;
				QVERIFY(musicBuffer > 0);
				request.filePattern      = QStringLiteral("qmud/BEEP.WAV");
				request.continuePlayback = true;
				request.repeatCount      = 3;
				QVERIFY(session->submitNativeSound(request));
				const SoundEngineTestAccess::NativePlaybackState continuedMusic =
				    SoundEngineTestAccess::nativePlaybackState(engine, session, QMudNativeSoundProtocol::Msp,
				                                               QMudNativeSoundMedia::Music);
				QCOMPARE(continuedMusic.buffer, musicBuffer);
				QCOMPARE(continuedMusic.filePattern, QStringLiteral("QMud/beep.wav"));

				QTemporaryDir pendingRoot(
				    QDir::current().filePath(QStringLiteral("sound-update-pending-XXXXXX")));
				QVERIFY(pendingRoot.isValid());
				engine.configureSoundDirectory(pendingRoot.path());
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				const auto pendingSession = engine.registerSession({});
				QVERIFY(pendingSession);
				QTcpServer pendingServer;
				QVERIFY(pendingServer.listen(QHostAddress::LocalHost));
				const QString pendingUrl =
				    QStringLiteral("http://127.0.0.1:%1/").arg(pendingServer.serverPort());
				request = nativeSoundRequest(QStringLiteral("pending.wav"), QMudNativeSoundMedia::Music);
				request.downloadUrl      = pendingUrl;
				request.repeatCount      = 5;
				request.continuePlayback = false;
				QVERIFY(pendingSession->submitNativeSound(request));
				selectedBuffer = SoundEngineTestAccess::nativePlaybackState(engine, pendingSession,
				                                                            QMudNativeSoundProtocol::Msp,
				                                                            QMudNativeSoundMedia::Music)
				                     .buffer;
				QVERIFY(selectedBuffer > 0);
				request.repeatCount      = 1;
				request.volume           = 80;
				request.continuePlayback = true;
				QVERIFY(pendingSession->submitNativeSound(request));
				QVERIFY(
				    SoundEngineTestAccess::completeReservedChannel(engine, pendingSession, selectedBuffer));
				QVERIFY(!SoundEngineTestAccess::nativePlaybackState(engine, pendingSession,
				                                                    QMudNativeSoundProtocol::Msp,
				                                                    QMudNativeSoundMedia::Music)
				             .current);
				QCOMPARE(pendingSession->status(selectedBuffer), 0);
			}

			/** @brief Verifies catalog changes invalidate only cache entries for files that actually changed. */
			void nativeSoundCatalogTracksDirectoryChanges()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-catalog-XXXXXX")));
				QVERIFY(root.isValid());
				const QString nested = QDir(root.path()).filePath(QStringLiteral("sounds/nested"));
				QVERIFY(QDir().mkpath(nested));
				const QString first = QDir(nested).filePath(QStringLiteral("first.wav"));
				QVERIFY(QFile::copy(testSoundFileName(), first));

				SoundEngine engine;
				engine.configureSoundDirectory(root.path());
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				QCOMPARE(SoundEngineTestAccess::fileCatalogState(engine).fileCount, 1);
				const auto session = engine.registerSession({});
				QVERIFY(session);
				const QString firstRequest  = QStringLiteral("nested/first.wav");
				const QString secondRequest = QStringLiteral("nested/second.wav");
				QVERIFY(SoundEngineTestAccess::cacheFileRequest(engine, session, firstRequest, first));
				QVERIFY(SoundEngineTestAccess::fileRequestIsCached(engine, session, firstRequest));

				const QString second = QDir(nested).filePath(QStringLiteral("second.wav"));
				QVERIFY(QFile::copy(testSoundFileName(), second));
				QTRY_COMPARE(SoundEngineTestAccess::fileCatalogState(engine).fileCount, 2);
				QVERIFY(SoundEngineTestAccess::fileRequestIsCached(engine, session, firstRequest));
				QVERIFY(SoundEngineTestAccess::cacheFileRequest(engine, session, secondRequest, second));
				QVERIFY(SoundEngineTestAccess::fileRequestIsCached(engine, session, secondRequest));

				const quint64 revisionBeforeOverwrite =
				    SoundEngineTestAccess::fileCatalogState(engine).revision;
				QFile replacement(testSoundFileName());
				QVERIFY(replacement.open(QIODevice::ReadOnly));
				const QByteArray replacementPayload = replacement.readAll();
				QFile            overwritten(first);
				QVERIFY(overwritten.open(QIODevice::WriteOnly | QIODevice::Truncate));
				QCOMPARE(overwritten.write(replacementPayload), replacementPayload.size());
				QVERIFY(overwritten.setFileTime(QFileInfo(first).lastModified().addSecs(2),
				                                QFileDevice::FileModificationTime));
				overwritten.close();
				SoundEngineTestAccess::reconcileCatalogDirectory(engine, nested);
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).revision >
				            revisionBeforeOverwrite);
				QVERIFY(!SoundEngineTestAccess::fileRequestIsCached(engine, session, firstRequest));
				QVERIFY(SoundEngineTestAccess::fileRequestIsCached(engine, session, secondRequest));

				QVERIFY(QFile::remove(first));
				QTRY_COMPARE(SoundEngineTestAccess::fileCatalogState(engine).fileCount, 1);
				QVERIFY(!SoundEngineTestAccess::fileRequestIsCached(engine, session, firstRequest));
				QVERIFY(SoundEngineTestAccess::fileRequestIsCached(engine, session, secondRequest));
			}

			/** @brief Exercises deleted-subtree lookup with ordinary watcher events and forced reconciliation. */
			void nativeSoundCatalogRemovesOnlyDeletedSubtree_data()
			{
				QTest::addColumn<bool>("reconcileDuringLookup");
				QTest::newRow("watcher-events") << false;
				QTest::newRow("pending-reconciliation") << true;
			}

			/** @brief Verifies subtree removal retains sibling sounds and rejects deleted files after lookup completes. */
			void nativeSoundCatalogRemovesOnlyDeletedSubtree()
			{
				QFETCH(bool, reconcileDuringLookup);

				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-subtree-removal-XXXXXX")));
				QVERIFY(root.isValid());
				const QString removedDirectory =
				    QDir(root.path()).filePath(QStringLiteral("sounds/removed/child"));
				const QString retainedDirectory =
				    QDir(root.path()).filePath(QStringLiteral("sounds/retained"));
				QVERIFY(QDir().mkpath(removedDirectory));
				QVERIFY(QDir().mkpath(retainedDirectory));
				QVERIFY(QFile::copy(testSoundFileName(),
				                    QDir(removedDirectory).filePath(QStringLiteral("removed.wav"))));
				const QString retainedFile = QDir(retainedDirectory).filePath(QStringLiteral("retained.wav"));
				QVERIFY(QFile::copy(testSoundFileName(), retainedFile));

				SoundEngine engine;
				engine.initializePlayback();
				engine.configureSoundDirectory(root.path());
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				QCOMPARE(SoundEngineTestAccess::fileCatalogState(engine).fileCount, 2);

				QVERIFY(
				    QDir(QDir(root.path()).filePath(QStringLiteral("sounds/removed"))).removeRecursively());
				QTRY_COMPARE(SoundEngineTestAccess::fileCatalogState(engine).fileCount, 1);
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				const auto session = engine.registerSession({});
				QVERIFY(session);
				QVERIFY(
				    session->submitNativeSound(nativeSoundRequest(QStringLiteral("retained/retained.wav"))));
				const int selectedBuffer =
				    SoundEngineTestAccess::nativePlaybackState(engine, session, QMudNativeSoundProtocol::Msp,
				                                               QMudNativeSoundMedia::Sound)
				        .buffer;
				QVERIFY(selectedBuffer > 0);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, selectedBuffer),
				         retainedFile);
				QCOMPARE(session->stop(selectedBuffer), eOK);

				[[maybe_unused]] auto resumeCatalog =
				    qScopeGuard([&engine] { SoundEngineTestAccess::setCatalogPaused(engine, false); });
				if (reconcileDuringLookup)
				{
					SoundEngineTestAccess::setCatalogPaused(engine, true);
					SoundEngineTestAccess::reconcileCatalogDirectory(engine, retainedDirectory);
					QVERIFY(!SoundEngineTestAccess::fileCatalogState(engine).ready);
				}

				auto request = nativeSoundRequest(QStringLiteral("removed/child/removed.wav"));
				// A wrongly selected sound must not finish naturally and satisfy the rejection check.
				request.repeatCount = -1;
				QVERIFY(session->submitNativeSound(request));
				if (reconcileDuringLookup)
				{
					const auto pending = SoundEngineTestAccess::nativePlaybackState(
					    engine, session, request.protocol, request.media);
					QVERIFY(pending.current);
					QVERIFY(pending.buffer > 0);
					QVERIFY(
					    SoundEngineTestAccess::activeFileIdentity(engine, session, pending.buffer).isEmpty());
					SoundEngineTestAccess::setCatalogPaused(engine, false);
				}

				// Readiness can change after any observation when another watcher event arrives.
				QTRY_VERIFY(!SoundEngineTestAccess::nativePlaybackState(engine, session, request.protocol,
				                                                        request.media)
				                 .current);
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				QCOMPARE(SoundEngineTestAccess::fileCatalogState(engine).fileCount, 1);
			}

			/** @brief Verifies startup restores the last complete catalog before background reconciliation. */
			void nativeSoundCatalogRestoresPersistentIndexBeforeScanning()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-catalog-index-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("sounds/nested")));
				const QString indexed =
				    QDir(root.path()).filePath(QStringLiteral("sounds/nested/indexed.wav"));
				QVERIFY(QFile::copy(testSoundFileName(), indexed));

				{
					SoundEngine engine;
					engine.configureSoundDirectory(root.path());
					QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
					QCOMPARE(SoundEngineTestAccess::fileCatalogState(engine).fileCount, 1);
					QTRY_VERIFY(QFileInfo::exists(
					    QDir(root.path()).filePath(QStringLiteral("worlds/state/sound-index.qss"))));
				}

				SoundEngine restored;
				restored.initializePlayback();
				SoundEngineTestAccess::setCatalogPaused(restored, true);
				SoundEngineTestAccess::setIndexReadPaused(restored, true);
				auto resumeCatalog = qScopeGuard(
				    [&restored]
				    {
					    SoundEngineTestAccess::setIndexReadPaused(restored, false);
					    SoundEngineTestAccess::setCatalogPaused(restored, false);
				    });
				restored.configureSoundDirectory(root.path());
				const auto session = restored.registerSession({});
				QVERIFY(session);
				QVERIFY(session->submitNativeSound(nativeSoundRequest(QStringLiteral("*.wav"))));
				SoundEngineTestAccess::NativePlaybackState playback =
				    SoundEngineTestAccess::nativePlaybackState(
				        restored, session, QMudNativeSoundProtocol::Msp, QMudNativeSoundMedia::Sound);
				QVERIFY(playback.current);
				QVERIFY(
				    SoundEngineTestAccess::activeFileIdentity(restored, session, playback.buffer).isEmpty());
				SoundEngineTestAccess::setIndexReadPaused(restored, false);
				QTRY_COMPARE(SoundEngineTestAccess::fileCatalogState(restored).fileCount, 1);
				QVERIFY(
				    SoundEngineTestAccess::activeFileIdentity(restored, session, playback.buffer).isEmpty());
				QVERIFY(!SoundEngineTestAccess::fileCatalogState(restored).ready);
				SoundEngineTestAccess::setCatalogPaused(restored, false);
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(restored).ready);
				QTRY_COMPARE(SoundEngineTestAccess::activeFileIdentity(restored, session, playback.buffer),
				             QFileInfo(indexed).canonicalFilePath());
			}

			/** @brief Verifies every indexed hierarchy level is reconciled before a deep file is retained. */
			void persistentSoundCatalogReconcilesDeepHierarchy()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-index-deep-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("sounds/one/two")));
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("worlds/state")));
				const QString indexed =
				    QDir(root.path()).filePath(QStringLiteral("sounds/one/two/indexed.wav"));
				QVERIFY(QFile::copy(testSoundFileName(), indexed));
				QVERIFY(writeSoundIndex(
				    QDir(root.path()).filePath(QStringLiteral("worlds/state/sound-index.qss")),
				    {
				        {QStringLiteral("."),       {},                  {}},
				        {QStringLiteral("one"),     QStringLiteral("."), {}},
				        {QStringLiteral("one/two"),
				         QStringLiteral("one"),
				         {QStringLiteral("indexed.wav")}                   }
                }));

				SoundEngine engine;
				engine.initializePlayback();
				engine.configureSoundDirectory(root.path());
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				const SoundEngineTestAccess::FileCatalogState state =
				    SoundEngineTestAccess::fileCatalogState(engine);
				QCOMPARE(state.fileCount, 1);
				QCOMPARE(state.watchedDirectoryCount, 4);
				const auto session = engine.registerSession({});
				QVERIFY(session);
				int selectedBuffer = 0;
				QCOMPARE(session->playFile(0, QStringLiteral("one/two/indexed.wav"), false, 0.0, 0.0, 0,
				                           &selectedBuffer),
				         eOK);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, selectedBuffer), indexed);
			}

			/** @brief Verifies case-only file renames retain case-insensitive catalog lookup after reconciliation. */
			void persistentSoundCatalogReconcilesCaseOnlyFileRename()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-index-case-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("sounds/nested")));
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("worlds/state")));
				const QString renamed =
				    QDir(root.path()).filePath(QStringLiteral("sounds/nested/Renamed.WAV"));
				QVERIFY(QFile::copy(testSoundFileName(), renamed));
				QVERIFY(writeSoundIndex(
				    QDir(root.path()).filePath(QStringLiteral("worlds/state/sound-index.qss")),
				    {
				        {QStringLiteral("."),      {},                  {}                             },
				        {QStringLiteral("nested"), QStringLiteral("."), {QStringLiteral("renamed.wav")}}
                }));

				SoundEngine engine;
				engine.initializePlayback();
				engine.configureSoundDirectory(root.path());
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				QCOMPARE(SoundEngineTestAccess::fileCatalogState(engine).fileCount, 1);
				const auto session = engine.registerSession({});
				QVERIFY(session);
				int selectedBuffer = 0;
				QCOMPARE(session->playFile(0, QStringLiteral("nested/renamed.wav"), false, 0.0, 0.0, 0,
				                           &selectedBuffer),
				         eOK);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, selectedBuffer), renamed);
			}

			/** @brief Verifies a large persistent catalog reaches playback only through bounded batches. */
			void persistentSoundCatalogLoadsInBoundedBatches()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-index-batches-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("sounds")));
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("worlds/state")));
				constexpr qsizetype fileCount = 257;
				QStringList         indexedFiles;
				indexedFiles.reserve(fileCount);
				for (qsizetype fileIndex = 0; fileIndex < fileCount; ++fileIndex)
					indexedFiles.push_back(QStringLiteral("sound-%1.wav").arg(fileIndex));
				QVERIFY(writeSoundIndex(
				    QDir(root.path()).filePath(QStringLiteral("worlds/state/sound-index.qss")),
				    {
				        {QStringLiteral("."), {}, std::move(indexedFiles)}
                }));

				SoundEngine restored;
				SoundEngineTestAccess::setCatalogPaused(restored, true);
				auto resumeCatalog =
				    qScopeGuard([&restored] { SoundEngineTestAccess::setCatalogPaused(restored, false); });
				restored.configureSoundDirectory(root.path());
				QTRY_COMPARE(SoundEngineTestAccess::fileCatalogState(restored).fileCount, fileCount);
				const SoundEngineTestAccess::FileCatalogState loaded =
				    SoundEngineTestAccess::fileCatalogState(restored);
				QCOMPARE(loaded.maximumIndexBatchSize, 128);
				QVERIFY(!loaded.ready);
			}

			/** @brief Verifies directory reconciliation reaches playback only through bounded batches. */
			void soundCatalogDirectoryPublicationIsBounded()
			{
				QTemporaryDir root(
				    QDir::current().filePath(QStringLiteral("sound-directory-batches-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("sounds")));

				SoundEngine engine;
				engine.configureSoundDirectory(root.path());
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				constexpr qsizetype fileCount = 257;
				QVERIFY(SoundEngineTestAccess::publishSyntheticCatalogDirectory(engine, fileCount));
				QTRY_COMPARE(SoundEngineTestAccess::fileCatalogState(engine).fileCount, fileCount);
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				QCOMPARE(SoundEngineTestAccess::fileCatalogState(engine).maximumDirectoryBatchSize, 128);
			}

			/** @brief Verifies stale indexed subtrees are reconciled in bounded low-priority steps. */
			void stalePersistentSoundSubtreeRemovalIsBounded()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-index-removal-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("sounds")));
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("worlds/state")));
				constexpr qsizetype fileCount = 257;
				QStringList         indexedFiles;
				indexedFiles.reserve(fileCount);
				for (qsizetype fileIndex = 0; fileIndex < fileCount; ++fileIndex)
					indexedFiles.push_back(QStringLiteral("sound-%1.wav").arg(fileIndex));
				QVERIFY(writeSoundIndex(
				    QDir(root.path()).filePath(QStringLiteral("worlds/state/sound-index.qss")),
				    {
				        {QStringLiteral("."),            {},                        {}},
				        {QStringLiteral("missing"),      QStringLiteral("."),       {}},
				        {QStringLiteral("missing/deep"), QStringLiteral("missing"),
				         std::move(indexedFiles)                                      }
                }));

				SoundEngine engine;
				engine.configureSoundDirectory(root.path());
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				const SoundEngineTestAccess::FileCatalogState state =
				    SoundEngineTestAccess::fileCatalogState(engine);
				QCOMPARE(state.fileCount, 0);
				QCOMPARE(state.maximumReconciliationStepSize, 128);
			}

			/** @brief Verifies a truncated persistent index falls through to scanning and is rebuilt. */
			void truncatedPersistentSoundIndexDoesNotStrandCatalogScan()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-index-truncated-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("sounds/nested")));
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("worlds/state")));
				QVERIFY(QFile::copy(testSoundFileName(),
				                    QDir(root.path()).filePath(QStringLiteral("sounds/nested/indexed.wav"))));
				QFile index(QDir(root.path()).filePath(QStringLiteral("worlds/state/sound-index.qss")));
				QVERIFY(index.open(QIODevice::WriteOnly));
				QDataStream stream(&index);
				stream.setVersion(QDataStream::Qt_6_0);
				stream << quint32{0x514D5349U} << quint32{1U} << quint64{2U} << QStringLiteral(".")
				       << QString{} << quint64{0U};
				QCOMPARE(stream.status(), QDataStream::Ok);
				index.close();

				SoundEngine engine;
				engine.configureSoundDirectory(root.path());
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				QCOMPARE(SoundEngineTestAccess::fileCatalogState(engine).fileCount, 1);
			}

			/** @brief Verifies watcher accounting never republishes the growing directory membership set. */
			void catalogWatcherPublicationRemainsConstantSize()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-watch-scaling-XXXXXX")));
				QVERIFY(root.isValid());
				constexpr qsizetype directoryCount = 32;
				for (qsizetype index = 0; index < directoryCount; ++index)
				{
					QVERIFY(QDir().mkpath(
					    QDir(root.path()).filePath(QStringLiteral("sounds/directory-%1").arg(index))));
				}

				SoundEngine engine;
				engine.configureSoundDirectory(root.path());
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				const SoundEngineTestAccess::FileCatalogState state =
				    SoundEngineTestAccess::fileCatalogState(engine);
				QCOMPARE(state.watchedDirectoryCount, directoryCount + 2);
				QCOMPARE(state.watchStatePathEntryCount, 0);
			}

			/** @brief Verifies persistent-index I/O rejects a state-directory symlink escaping QMud home. */
			void persistentSoundIndexRejectsEscapingStateSymlink()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-index-escape-XXXXXX")));
				QTemporaryDir outside(QDir::current().filePath(QStringLiteral("sound-index-outside-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(outside.isValid());
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("sounds")));
				QVERIFY(QFile::copy(testSoundFileName(),
				                    QDir(root.path()).filePath(QStringLiteral("sounds/indexed.wav"))));
				if (!QFile::link(outside.path(), QDir(root.path()).filePath(QStringLiteral("worlds"))))
					QSKIP("Directory symlinks are unavailable on this platform");

				SoundEngine engine;
				engine.configureSoundDirectory(root.path());
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				QVERIFY(!QFileInfo::exists(
				    QDir(outside.path()).filePath(QStringLiteral("state/sound-index.qss"))));
			}

			/** @brief Verifies persistent-index I/O permits a contained state-directory symlink. */
			void persistentSoundIndexAcceptsContainedStateSymlink()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-index-contained-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("sounds")));
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("worlds")));
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("contained-state")));
				QVERIFY(QFile::copy(testSoundFileName(),
				                    QDir(root.path()).filePath(QStringLiteral("sounds/indexed.wav"))));
				const QString containedState = QDir(root.path()).filePath(QStringLiteral("contained-state"));
				if (!QFile::link(containedState, QDir(root.path()).filePath(QStringLiteral("worlds/state"))))
				{
					QSKIP("Directory symlinks are unavailable on this platform");
				}

				SoundEngine engine;
				engine.configureSoundDirectory(root.path());
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				QVERIFY(QFileInfo::exists(QDir(containedState).filePath(QStringLiteral("sound-index.qss"))));
			}

			/** @brief Verifies a runtime sounds-root symlink cannot escape the configured QMud home. */
			void nativeSoundCatalogRejectsRuntimeRootEscape()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-root-escape-XXXXXX")));
				QTemporaryDir outside(QDir::current().filePath(QStringLiteral("sound-root-external-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(outside.isValid());
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("sounds")));
				const QString contained = QDir(root.path()).filePath(QStringLiteral("sounds/contained.wav"));
				QVERIFY(QFile::copy(testSoundFileName(), contained));
				QVERIFY(QFile::copy(testSoundFileName(),
				                    QDir(outside.path()).filePath(QStringLiteral("external-one.wav"))));
				QVERIFY(QFile::copy(testSoundFileName(),
				                    QDir(outside.path()).filePath(QStringLiteral("external-two.wav"))));

				SoundEngine engine;
				engine.initializePlayback();
				engine.configureSoundDirectory(root.path());
				QTRY_COMPARE(SoundEngineTestAccess::fileCatalogState(engine).fileCount, 1);

				QVERIFY(QFile::remove(contained));
				const QString sounds = QDir(root.path()).filePath(QStringLiteral("sounds"));
				QVERIFY(QDir().rmdir(sounds));
				if (!QFile::link(outside.path(), sounds))
					QSKIP("Directory symlinks are unavailable on this platform");
				SoundEngineTestAccess::reconcileSoundRoot(engine);
				QVERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				QCOMPARE(SoundEngineTestAccess::fileCatalogState(engine).fileCount, 0);

				const auto session = engine.registerSession({});
				QVERIFY(session);
				int selectedBuffer = 0;
				QCOMPARE(session->playFile(0, QStringLiteral("external-one.wav"), false, 0.0, 0.0, 0,
				                           &selectedBuffer),
				         eFileNotFound);
				QCOMPARE(selectedBuffer, 0);
			}

			/** @brief Verifies a failed catalog watch is retried without rescanning until registration succeeds. */
			void failedCatalogWatchRecoversDuringPeriodicMaintenance()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-watch-retry-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("sounds")));

				SoundEngine engine;
				engine.initializePlayback();
				engine.configureSoundDirectory(root.path());
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				const SoundEngineTestAccess::FileCatalogState initial =
				    SoundEngineTestAccess::fileCatalogState(engine);
				const QString delayedDirectory = QDir(root.path()).filePath(QStringLiteral("sounds/delayed"));
				QVERIFY(!SoundEngineTestAccess::attemptCatalogWatch(engine, delayedDirectory));
				QCOMPARE(SoundEngineTestAccess::fileCatalogState(engine).failedWatchCount, 1);

				QVERIFY(QDir().mkpath(delayedDirectory));
				SoundEngineTestAccess::runPeriodicMaintenance(engine);
				QTRY_COMPARE(SoundEngineTestAccess::fileCatalogState(engine).failedWatchCount, 0);
				const SoundEngineTestAccess::FileCatalogState recovered =
				    SoundEngineTestAccess::fileCatalogState(engine);
				QCOMPARE(recovered.watchedDirectoryCount, initial.watchedDirectoryCount + 1);
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);

				const QString added = QDir(delayedDirectory).filePath(QStringLiteral("added.wav"));
				QVERIFY(QFile::copy(testSoundFileName(), added));
				QTRY_COMPARE(SoundEngineTestAccess::fileCatalogState(engine).fileCount, 1);
			}

			/** @brief Verifies a missing native sound is downloaded once into the sounds tree and then played. */
			void nativeSoundDownloadsMissingFileFromProtocolUrl()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-download-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("sounds/World-One")));

				QFile fixture(testSoundFileName());
				QVERIFY(fixture.open(QIODevice::ReadOnly));
				const QByteArray payload = fixture.readAll();
				QVERIFY(!payload.isEmpty());

				QTcpServer server;
				QVERIFY(server.listen(QHostAddress::LocalHost));
				int     requestCount = 0;
				QString requestedTarget;
				connect(&server, &QTcpServer::newConnection, &server,
				        [&server, &requestCount, &requestedTarget, payload]
				        {
					        while (QTcpSocket *socket = server.nextPendingConnection())
					        {
						        QObject::connect(
						            socket, &QTcpSocket::readyRead, socket,
						            [socket, &requestCount, &requestedTarget, payload]
						            {
							            QByteArray request = socket->property("request").toByteArray();
							            request += socket->readAll();
							            socket->setProperty("request", request);
							            if (!request.contains("\r\n\r\n") ||
							                socket->property("answered").toBool())
								            return;
							            socket->setProperty("answered", true);
							            ++requestCount;
							            const QList<QByteArray> requestLine =
							                request.first(request.indexOf("\r\n")).split(' ');
							            if (requestLine.size() >= 2)
								            requestedTarget = QString::fromLatin1(requestLine.at(1));
							            const QByteArray response =
							                QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Length: ") +
							                QByteArray::number(payload.size()) +
							                QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + payload;
							            socket->write(response);
							            socket->disconnectFromHost();
						            });
					        }
				        });

				SoundEngine engine;
				engine.initializePlayback();
				engine.configureSoundDirectory(root.path());
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				const auto session = engine.registerSession({}, QStringLiteral("./sounds/world-one/"));
				QVERIFY(session);
				const QString baseUrl = QStringLiteral("http://127.0.0.1:%1/base/").arg(server.serverPort());
				QMudNativeSoundRequest request = nativeSoundRequest(QStringLiteral("nested/downloaded.wav"));
				request.downloadUrl            = baseUrl;
				QVERIFY(session->submitNativeSound(request));
				int selectedBuffer =
				    SoundEngineTestAccess::nativePlaybackState(engine, session, QMudNativeSoundProtocol::Msp,
				                                               QMudNativeSoundMedia::Sound)
				        .buffer;
				QVERIFY(selectedBuffer > 0);

				const QString downloaded =
				    QDir(root.path()).filePath(QStringLiteral("sounds/World-One/nested/downloaded.wav"));
				QTRY_VERIFY(QFileInfo::exists(downloaded));
				QCOMPARE(requestCount, 1);
				QCOMPARE(requestedTarget, QStringLiteral("/base/nested/downloaded.wav"));
				QFile installed(downloaded);
				QVERIFY(installed.open(QIODevice::ReadOnly));
				QCOMPARE(installed.readAll(), payload);
				QTRY_COMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, selectedBuffer),
				             QFileInfo(downloaded).canonicalFilePath());

				request.stop = true;
				QVERIFY(session->submitNativeSound(request));
			}

			/** @brief Verifies an existing download destination is indexed before completion is published. */
			void existingNativeDownloadDestinationPublishesCatalogFile()
			{
				QTemporaryDir root(
				    QDir::current().filePath(QStringLiteral("sound-existing-download-XXXXXX")));
				QVERIFY(root.isValid());
				const QString directory = QDir(root.path()).filePath(QStringLiteral("sounds/world"));
				QVERIFY(QDir().mkpath(directory));

				SoundEngine engine;
				engine.initializePlayback();
				engine.configureSoundDirectory(root.path());
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				SoundEngineTestAccess::setCatalogPaused(engine, true);
				auto resumeCatalog =
				    qScopeGuard([&engine] { SoundEngineTestAccess::setCatalogPaused(engine, false); });
				const QString existing = QDir(directory).filePath(QStringLiteral("existing.wav"));
				QVERIFY(QFile::copy(testSoundFileName(), existing));
				SoundEngineTestAccess::processExistingDownload(engine, existing);
				QTRY_COMPARE(SoundEngineTestAccess::fileCatalogState(engine).fileCount, 1);

				const auto session = engine.registerSession({}, QStringLiteral("./sounds/world/"));
				QVERIFY(session);
				QVERIFY(session->submitNativeSound(nativeSoundRequest(QStringLiteral("existing.wav"))));
				const int selectedBuffer =
				    SoundEngineTestAccess::nativePlaybackState(engine, session, QMudNativeSoundProtocol::Msp,
				                                               QMudNativeSoundMedia::Sound)
				        .buffer;
				QVERIFY(selectedBuffer > 0);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, selectedBuffer),
				         existing);
			}

			/** @brief Verifies a failed native sound download releases its reserved playback channel. */
			void failedNativeSoundDownloadReleasesReservation()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-failed-download-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("sounds")));

				QTcpServer server;
				QVERIFY(server.listen(QHostAddress::LocalHost));
				int requestCount = 0;
				connect(&server, &QTcpServer::newConnection, &server,
				        [&server, &requestCount]
				        {
					        while (QTcpSocket *socket = server.nextPendingConnection())
					        {
						        QObject::connect(socket, &QTcpSocket::readyRead, socket,
						                         [socket, &requestCount]
						                         {
							                         QByteArray request =
							                             socket->property("request").toByteArray();
							                         request += socket->readAll();
							                         socket->setProperty("request", request);
							                         if (!request.contains("\r\n\r\n") ||
							                             socket->property("answered").toBool())
							                         {
								                         return;
							                         }
							                         socket->setProperty("answered", true);
							                         ++requestCount;
							                         socket->write(QByteArrayLiteral(
							                             "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
							                             "Connection: close\r\n\r\n"));
							                         socket->disconnectFromHost();
						                         });
					        }
				        });

				SoundEngine engine;
				engine.initializePlayback();
				engine.configureSoundDirectory(root.path());
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				const auto session = engine.registerSession({}, QStringLiteral("./sounds/world-one/"));
				QVERIFY(session);
				QMudNativeSoundRequest request = nativeSoundRequest(QStringLiteral("missing.wav"));
				request.downloadUrl = QStringLiteral("http://127.0.0.1:%1/base/").arg(server.serverPort());
				QVERIFY(session->submitNativeSound(request));
				const int failedBuffer =
				    SoundEngineTestAccess::nativePlaybackState(engine, session, QMudNativeSoundProtocol::Msp,
				                                               QMudNativeSoundMedia::Sound)
				        .buffer;
				QVERIFY(failedBuffer > 0);
				QTRY_COMPARE(requestCount, 1);
				QTRY_VERIFY(!SoundEngineTestAccess::nativePlaybackState(
				                 engine, session, QMudNativeSoundProtocol::Msp, QMudNativeSoundMedia::Sound)
				                 .current);
				QVERIFY(!QFileInfo::exists(
				    QDir(root.path()).filePath(QStringLiteral("sounds/world-one/missing.wav"))));
			}

			/** @brief Supplies protocols, media, pending entry paths, transport failures, and later cancellation. */
			void failedNativeDownloadPreservesLaterFile_data()
			{
				QTest::addColumn<bool>("mxp");
				QTest::addColumn<bool>("music");
				QTest::addColumn<bool>("prepared");
				QTest::addColumn<bool>("disconnect");
				QTest::addColumn<int>("transition");
				for (bool mxp : {false, true})
					for (bool music : {false, true})
						for (bool prepared : {false, true})
							for (bool disconnect : {false, true})
								QTest::newRow(
								    qPrintable(QStringLiteral("mxp-%1-music-%2-prepared-%3-disconnect-%4")
								                   .arg(mxp)
								                   .arg(music)
								                   .arg(prepared)
								                   .arg(disconnect)))
								    << mxp << music << prepared << disconnect << 0;
				for (int transition = 1; transition <= 5; ++transition)
					QTest::newRow(qPrintable(QStringLiteral("cancellation-%1").arg(transition)))
					    << false << false << true << false << transition;
			}

			/** @brief Verifies a failed shared download releases only its owner and cannot cancel later occurrences. */
			void failedNativeDownloadPreservesLaterFile()
			{
				QFETCH(bool, mxp);
				QFETCH(bool, music);
				QFETCH(bool, prepared);
				QFETCH(bool, disconnect);
				QFETCH(int, transition);
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-download-owner-XXXXXX")));
				QVERIFY(root.isValid());
				const QString preferred =
				    soundFixture(root.path(), QStringLiteral("sounds/TestWorld/relocated/cue.wav"));
				QVERIFY(!preferred.isEmpty());
				QTcpServer server;
				QVERIFY(server.listen(QHostAddress::LocalHost));
				SoundEngine engine;
				engine.initializePlayback();
				engine.configureSoundDirectory(root.path());
				QVERIFY(waitForSoundValue(
				    [&] { return static_cast<bool>(SoundEngineTestAccess::fileCatalogState(engine).ready); },
				    true));
				const auto first  = engine.registerSession({}, QStringLiteral("sounds/TestWorld"));
				const auto second = engine.registerSession({}, QStringLiteral("sounds/TestWorld"));
				QVERIFY(first && second);
				auto request =
				    nativeSoundRequest(QStringLiteral("download.wav"),
				                       music ? QMudNativeSoundMedia::Music : QMudNativeSoundMedia::Sound);
				request.protocol    = mxp ? QMudNativeSoundProtocol::Mxp : QMudNativeSoundProtocol::Msp;
				request.downloadUrl = QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort());
				request.volume      = 0;
				QVERIFY(first->submitNativeSound(request));
				QVERIFY(second->submitNativeSound(request));
				QVERIFY(waitForSoundValue([&] { return static_cast<bool>(server.hasPendingConnections()); },
				                          true));
				const std::unique_ptr<QTcpSocket> socket(server.nextPendingConnection());
				QVERIFY(socket);
				QVERIFY(
				    waitForSoundValue([&] { return static_cast<bool>(socket->bytesAvailable() > 0); }, true));
				const auto firstNative = SoundEngineTestAccess::nativePlaybackState(
				    engine, first, request.protocol, request.media);
				const auto secondNative = SoundEngineTestAccess::nativePlaybackState(
				    engine, second, request.protocol, request.media);
				QVERIFY(firstNative.current && secondNative.current);
				SoundEngineTestAccess::setCatalogPaused(engine, true);
				QVERIFY(!soundFixture(root.path(), QStringLiteral("sounds/TestWorld/relocated/change.wav"))
				             .isEmpty());
				QVERIFY(waitForSoundValue(
				    [&] { return static_cast<bool>(!SoundEngineTestAccess::fileCatalogState(engine).ready); },
				    true));
				const QString    file        = QStringLiteral("legacy/cue.wav");
				const auto       firstToken  = prepared ? first->prepareFile(file) : 0;
				const auto       secondToken = prepared ? second->prepareFile(file) : 0;
				PendingSoundCall firstCall(
				    first,
				    [&]
				    {
					    return prepared
					               ? first->commitPrepared(firstToken, firstNative.buffer, true, -100.0, 0.0)
					               : first->playFile(firstNative.buffer, file, true, -100.0, 0.0);
				    });
				PendingSoundCall secondCall(
				    second,
				    [&]
				    {
					    return prepared ? second->commitPrepared(secondToken, secondNative.buffer, true,
					                                             -100.0, 0.0)
					                    : second->playFile(secondNative.buffer, file, true, -100.0, 0.0);
				    });
				QCOMPARE(
				    waitForSoundValue(
				        [&] { return SoundEngineTestAccess::pendingFileCount(engine, first, prepared); }, 1),
				    1);
				QCOMPARE(
				    waitForSoundValue(
				        [&] { return SoundEngineTestAccess::pendingFileCount(engine, second, prepared); }, 1),
				    1);
				QVERIFY(!server.hasPendingConnections());
				if (disconnect)
				{
					socket->write(QByteArrayLiteral(
					    "HTTP/1.1 200 OK\r\nContent-Length: 1024\r\nConnection: close\r\n\r\nx"));
					socket->disconnectFromHost();
				}
				else
				{
					socket->write(QByteArrayLiteral(
					    "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"));
					socket->disconnectFromHost();
				}
				QVERIFY(waitForSoundValue(
				    [&]
				    {
					    return static_cast<bool>(!SoundEngineTestAccess::nativePlaybackState(
					                                  engine, first, request.protocol, request.media)
					                                  .current);
				    },
				    true));
				QVERIFY(waitForSoundValue(
				    [&]
				    {
					    return static_cast<bool>(!SoundEngineTestAccess::nativePlaybackState(
					                                  engine, second, request.protocol, request.media)
					                                  .current);
				    },
				    true));
				QCOMPARE(SoundEngineTestAccess::pendingFileCount(engine, first, prepared), 1);
				QCOMPARE(SoundEngineTestAccess::pendingFileCount(engine, second, prepared), 1);
				QVERIFY(!firstCall.finished.load(std::memory_order_acquire));
				QVERIFY(!secondCall.finished.load(std::memory_order_acquire));
				QCOMPARE(first->status(firstNative.buffer), -2);
				QCOMPARE(second->status(secondNative.buffer), -2);
				QTemporaryDir replacement(
				    QDir::current().filePath(QStringLiteral("sound-download-new-root-XXXXXX")));
				QVERIFY(replacement.isValid());
				switch (transition)
				{
				case 0:
					break;
				case 1:
					QCOMPARE(first->stop(firstNative.buffer), eOK);
					QCOMPARE(second->stop(0), eOK);
					break;
				case 2:
					first->close();
					second->close();
					break;
				case 3:
					engine.configureSoundDirectory(replacement.path());
					break;
				case 4:
					SoundEngineTestAccess::resetForAudioDeviceChange(engine);
					break;
				case 5:
					QCOMPARE(first->playFile(firstNative.buffer, QStringLiteral("relocated/cue.wav"), true,
					                         -100.0, 0.0),
					         eOK);
					QCOMPARE(second->playFile(secondNative.buffer, QStringLiteral("relocated/cue.wav"), true,
					                          -100.0, 0.0),
					         eOK);
					break;
				default:
					QFAIL("Unknown pending transition");
				}
				SoundEngineTestAccess::setCatalogPaused(engine, false);
				QVERIFY(waitForSoundValue(
				    [&] { return static_cast<bool>(firstCall.finished.load(std::memory_order_acquire)); },
				    true));
				QVERIFY(waitForSoundValue(
				    [&] { return static_cast<bool>(secondCall.finished.load(std::memory_order_acquire)); },
				    true));
				QCOMPARE(firstCall.result, transition == 0 ? eOK : eCannotPlaySound);
				QCOMPARE(secondCall.result, transition == 0 ? eOK : eCannotPlaySound);
				QCOMPARE(SoundEngineTestAccess::pendingFileCount(engine, first), 0);
				QCOMPARE(SoundEngineTestAccess::pendingFileCount(engine, second), 0);
				if (transition == 0 || transition == 5)
				{
					QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, first, firstNative.buffer),
					         preferred);
					QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, second, secondNative.buffer),
					         preferred);
				}
			}

			/** @brief Verifies a native sound download with no transfer progress times out and releases waiters. */
			void stalledNativeSoundDownloadTimesOut()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-stalled-download-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("sounds")));

				QTcpServer server;
				QVERIFY(server.listen(QHostAddress::LocalHost));
				int requestCount = 0;
				connect(&server, &QTcpServer::newConnection, &server,
				        [&server, &requestCount]
				        {
					        while (QTcpSocket *socket = server.nextPendingConnection())
					        {
						        QObject::connect(socket, &QTcpSocket::readyRead, socket,
						                         [socket, &requestCount]
						                         {
							                         QByteArray request =
							                             socket->property("request").toByteArray();
							                         request += socket->readAll();
							                         socket->setProperty("request", request);
							                         if (!request.contains("\r\n\r\n") ||
							                             socket->property("counted").toBool())
							                         {
								                         return;
							                         }
							                         socket->setProperty("counted", true);
							                         ++requestCount;
						                         });
					        }
				        });

				SoundEngine engine;
				engine.initializePlayback();
				engine.configureSoundDirectory(root.path());
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				SoundEngineTestAccess::setDownloadStallTimeout(engine, 500);
				const auto session = engine.registerSession({}, QStringLiteral("./sounds/world/"));
				QVERIFY(session);
				QMudNativeSoundRequest request = nativeSoundRequest(QStringLiteral("stalled.wav"));
				request.downloadUrl = QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort());
				QVERIFY(session->submitNativeSound(request));
				const int selectedBuffer =
				    SoundEngineTestAccess::nativePlaybackState(engine, session, QMudNativeSoundProtocol::Msp,
				                                               QMudNativeSoundMedia::Sound)
				        .buffer;
				QVERIFY(selectedBuffer > 0);
				QTRY_COMPARE(requestCount, 1);
				QTRY_VERIFY_WITH_TIMEOUT(
				    !SoundEngineTestAccess::nativePlaybackState(engine, session, QMudNativeSoundProtocol::Msp,
				                                                QMudNativeSoundMedia::Sound)
				         .current,
				    5000);
				QVERIFY(!QFileInfo::exists(
				    QDir(root.path()).filePath(QStringLiteral("sounds/world/stalled.wav"))));
			}

			/** @brief Verifies continuing progress permits a download whose total duration exceeds the timeout. */
			void slowNativeSoundDownloadContinuesWhileProgressing()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-slow-download-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("sounds")));
				QFile fixture(testSoundFileName());
				QVERIFY(fixture.open(QIODevice::ReadOnly));
				const QByteArray payload = fixture.readAll();
				QVERIFY(payload.size() > 15);

				QTcpServer server;
				QVERIFY(server.listen(QHostAddress::LocalHost));
				connect(
				    &server, &QTcpServer::newConnection, &server,
				    [&server, payload]
				    {
					    while (QTcpSocket *socket = server.nextPendingConnection())
					    {
						    QObject::connect(
						        socket, &QTcpSocket::readyRead, socket,
						        [socket, payload]
						        {
							        QByteArray request = socket->property("request").toByteArray();
							        request += socket->readAll();
							        socket->setProperty("request", request);
							        if (!request.contains("\r\n\r\n") ||
							            socket->property("answered").toBool())
							        {
								        return;
							        }
							        socket->setProperty("answered", true);
							        socket->write(QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Length: ") +
							                      QByteArray::number(payload.size()) +
							                      QByteArrayLiteral("\r\nConnection: close\r\n\r\n"));
							        auto *timer = new QTimer(socket);
							        timer->setInterval(100);
							        const qsizetype chunkSize = (payload.size() + 14) / 15;
							        QObject::connect(
							            timer, &QTimer::timeout, socket,
							            [socket, timer, payload, chunkSize]
							            {
								            const qsizetype  offset = socket->property("offset").toLongLong();
								            const QByteArray chunk  = payload.mid(offset, chunkSize);
								            socket->write(chunk);
								            socket->setProperty(
								                "offset", static_cast<qlonglong>(offset + chunk.size()));
								            if (offset + chunk.size() >= payload.size())
								            {
									            timer->stop();
									            socket->disconnectFromHost();
								            }
							            });
							        timer->start();
						        });
					    }
				    });

				SoundEngine engine;
				engine.initializePlayback();
				engine.configureSoundDirectory(root.path());
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				SoundEngineTestAccess::setDownloadStallTimeout(engine, 1000);
				const auto session = engine.registerSession({}, QStringLiteral("./sounds/world/"));
				QVERIFY(session);
				QMudNativeSoundRequest request = nativeSoundRequest(QStringLiteral("slow.wav"));
				request.downloadUrl = QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort());
				QVERIFY(session->submitNativeSound(request));
				QVERIFY(SoundEngineTestAccess::nativePlaybackState(
				            engine, session, QMudNativeSoundProtocol::Msp, QMudNativeSoundMedia::Sound)
				            .buffer > 0);
				const QString downloaded =
				    QDir(root.path()).filePath(QStringLiteral("sounds/world/slow.wav"));
				QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(downloaded), 5000);
				QFile installed(downloaded);
				QVERIFY(installed.open(QIODevice::ReadOnly));
				QCOMPARE(installed.readAll(), payload);
			}

			/** @brief Verifies a missing native sound without a download URL remains a no-op. */
			void nativeSoundMissingWithoutProtocolUrlIsNoop()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-missing-XXXXXX")));
				QVERIFY(root.isValid());
				SoundEngine engine;
				engine.initializePlayback();
				engine.configureSoundDirectory(root.path());
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				const auto session = engine.registerSession({});
				QVERIFY(session);
				QVERIFY(session->submitNativeSound(nativeSoundRequest(QStringLiteral("missing.wav"))));
				QVERIFY(!SoundEngineTestAccess::nativePlaybackState(
				             engine, session, QMudNativeSoundProtocol::Msp, QMudNativeSoundMedia::Sound)
				             .current);
			}

			/** @brief Verifies world-first lookup is case-insensitive without widening outside the sounds tree. */
			void worldSoundDirectoryHasLookupPriority()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-world-routing-XXXXXX")));
				QVERIFY(root.isValid());
				const QString sharedDirectory = QDir(root.path()).filePath(QStringLiteral("sounds/Test"));
				const QString firstDirectory =
				    QDir(root.path()).filePath(QStringLiteral("sounds/World-One/Test"));
				const QString secondDirectory =
				    QDir(root.path()).filePath(QStringLiteral("sounds/World-Two/Test"));
				QVERIFY(QDir().mkpath(sharedDirectory));
				QVERIFY(QDir().mkpath(firstDirectory));
				QVERIFY(QDir().mkpath(secondDirectory));
				const QString sharedFile = QDir(sharedDirectory).filePath(QStringLiteral("FiLe.WaV"));
				const QString firstFile  = QDir(firstDirectory).filePath(QStringLiteral("FiLe.WaV"));
				const QString secondFile = QDir(secondDirectory).filePath(QStringLiteral("FiLe.WaV"));
				QVERIFY(QFile::copy(testSoundFileName(), sharedFile));
				QVERIFY(QFile::copy(testSoundFileName(), firstFile));
				QVERIFY(QFile::copy(testSoundFileName(), secondFile));

				SoundEngine engine;
				engine.initializePlayback();
				engine.configureSoundDirectory(root.path());
				QTRY_COMPARE(SoundEngineTestAccess::fileCatalogState(engine).fileCount, 3);
				const auto session = engine.registerSession({}, QStringLiteral("./sounds/world-one/"));
				QVERIFY(session);

				QVERIFY(session->submitNativeSound(nativeSoundRequest(QStringLiteral(R"(test\file.wav)"))));
				int selectedBuffer =
				    SoundEngineTestAccess::nativePlaybackState(engine, session, QMudNativeSoundProtocol::Msp,
				                                               QMudNativeSoundMedia::Sound)
				        .buffer;
				QVERIFY(selectedBuffer > 0);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, selectedBuffer),
				         firstFile);
				QCOMPARE(SoundEngineTestAccess::cacheState(engine).sourceCount, 1);
				QCOMPARE(SoundEngineTestAccess::slowFileResolutionCount(engine), 1);
				QCOMPARE(session->stop(selectedBuffer), eOK);
				QVERIFY(session->submitNativeSound(nativeSoundRequest(QStringLiteral("TEST/*.WAV"))));
				selectedBuffer =
				    SoundEngineTestAccess::nativePlaybackState(engine, session, QMudNativeSoundProtocol::Msp,
				                                               QMudNativeSoundMedia::Sound)
				        .buffer;
				QVERIFY(selectedBuffer > 0);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, selectedBuffer),
				         firstFile);
				QCOMPARE(SoundEngineTestAccess::slowFileResolutionCount(engine), 1);
				QCOMPARE(session->stop(selectedBuffer), eOK);

				QCOMPARE(session->playFile(0, QStringLiteral(R"(test\file.wav)"), false, 0.0, 0.0, 0,
				                           &selectedBuffer),
				         eOK);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, selectedBuffer),
				         firstFile);
				QCOMPARE(SoundEngineTestAccess::cacheState(engine).sourceCount, 1);
				QCOMPARE(SoundEngineTestAccess::slowFileResolutionCount(engine), 1);
				QCOMPARE(session->stop(selectedBuffer), eOK);

				session->updateWorldSoundDirectory(QStringLiteral("./sounds/world-two/"));
				QCOMPARE(session->playFile(0, QStringLiteral("test/file.wav"), false, 0.0, 0.0, 0,
				                           &selectedBuffer),
				         eOK);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, selectedBuffer),
				         secondFile);
				QCOMPARE(SoundEngineTestAccess::slowFileResolutionCount(engine), 2);
				QCOMPARE(session->stop(selectedBuffer), eOK);
			}

			/** @brief Verifies caller teardown stops its cross-world voices through opaque sessions only. */
			void luaAudioOwnerCleanupIncludesOtherWorldSessions()
			{
				SoundEngine engine;
				configureTestSoundCatalog(engine);
				WorldRuntime first;
				WorldRuntime second;
				first.setStartupDirectory(testSoundHome());
				second.setStartupDirectory(testSoundHome());
				first.attachSoundEngine(&engine);
				second.attachSoundEngine(&engine);
				constexpr int                                 caller    = 0;
				constexpr int                                 unrelated = 0;
				QMudNativePluginRegistry::LuaAudioCallContext context;
				context.ownerKey = &caller;
				const QVector<QVariant> play{testSoundFileName(), 1};
				QCOMPARE(QMudNativePluginRegistry::callLuaAudio(&first, QStringLiteral("play"), play, context)
				             .errorCode,
				         eOK);
				QCOMPARE(
				    QMudNativePluginRegistry::callLuaAudio(&second, QStringLiteral("play"), play, context)
				        .errorCode,
				    eOK);
				context.ownerKey = &unrelated;
				const auto other =
				    QMudNativePluginRegistry::callLuaAudio(&second, QStringLiteral("play"), play, context);
				QCOMPARE(other.errorCode, eOK);
				const int otherBuffer = other.returnValues.front().toInt();
				QCOMPARE(otherBuffer, 2);
				QMudNativePluginRegistry::luaAudioStopRuntimeOwner(&first, &caller);
				QCOMPARE(first.soundStatus(1), -2);
				QCOMPARE(second.soundStatus(1), -2);
				QCOMPARE(second.soundStatus(otherBuffer), 2);
				context.ownerKey = &caller;
				QCOMPARE(QMudNativePluginRegistry::callLuaAudio(&second, QStringLiteral("playDelayLooped"),
				                                                {testSoundFileName(), 60000}, context)
				             .errorCode,
				         eOK);
				QMudNativePluginRegistry::LuaAudioRuntimeBufferState pending;
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeBufferState(&second, 1, pending));
				QVERIFY(pending.pendingCancel);
				QMudNativePluginRegistry::luaAudioStopRuntimeOwner(&first, &caller);
				QVERIFY(pending.pendingCancel->load(std::memory_order_acquire));
				QCOMPARE(second.soundStatus(1), -2);
				QCOMPARE(second.soundStatus(otherBuffer), 2);
			}

			/** @brief Supplies stale ranks, filename-only matches, and native matching forms. */
			void staleCatalogSelectionPreservesScopeRanking_data()
			{
				QTest::addColumn<int>("mode");
				QTest::addColumn<int>("scenario");
				for (int mode = 0; mode < 3; ++mode)
					for (int scenario = 0; scenario < 8; ++scenario)
						QTest::newRow(
						    qPrintable(QStringLiteral("mode-%1-scenario-%2").arg(mode).arg(scenario)))
						    << mode << scenario;
			}

			/** @brief Verifies stale groups are exhausted within their scope and unrelated names never match. */
			void staleCatalogSelectionPreservesScopeRanking()
			{
				QFETCH(int, mode);
				QFETCH(int, scenario);
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-stale-ranking-XXXXXX")));
				QVERIFY(root.isValid());
				QStringList   removed;
				QStringList   survivors;
				QStringList   expected;
				const QString worldExact  = QStringLiteral("sounds/TestWorld/legacy/alerts/cue.wav");
				const QString worldBest   = QStringLiteral("sounds/TestWorld/a/legacy/alerts/cue.wav");
				const QString worldTie    = QStringLiteral("sounds/TestWorld/b/legacy/alerts/cue.wav");
				const QString worldNext   = QStringLiteral("sounds/TestWorld/moved/alerts/cue.wav");
				const QString worldPlain  = QStringLiteral("sounds/TestWorld/cue.wav");
				const QString sharedExact = QStringLiteral("sounds/legacy/alerts/cue.wav");
				const QString sharedBest  = QStringLiteral("sounds/moved/legacy/alerts/cue.wav");
				const QString sharedNext  = QStringLiteral("sounds/moved/alerts/cue.wav");
				const QString sharedPlain = QStringLiteral("sounds/cue.wav");
				switch (scenario)
				{
				case 0:
					removed   = {worldExact};
					survivors = {worldBest, worldPlain, sharedExact};
					expected  = {worldBest};
					break;
				case 1:
					removed   = {worldBest, worldTie};
					survivors = {worldNext, worldPlain, sharedExact};
					expected  = {worldNext};
					break;
				case 2:
					removed   = {worldExact, worldBest, worldPlain};
					survivors = {sharedExact, sharedBest};
					expected  = {sharedExact};
					break;
				case 3:
					removed   = {sharedExact};
					survivors = {sharedBest, sharedPlain};
					expected  = {sharedBest};
					break;
				case 4:
					removed = {worldExact, worldBest, worldPlain, sharedExact, sharedBest, sharedPlain};
					break;
				case 5:
					removed   = {worldBest};
					survivors = {worldTie, QStringLiteral("sounds/TestWorld/c/legacy/alerts/cue.wav"),
					             sharedExact};
					expected  = mode == 0 ? QStringList{survivors.at(1)} : survivors.first(2);
					break;
				case 6:
					survivors = {worldPlain, sharedExact};
					expected  = {worldPlain};
					break;
				case 7:
					survivors = {sharedBest, sharedNext, sharedPlain};
					expected  = {sharedBest};
					break;
				default:
					QFAIL("Unknown ranking scenario");
				}
				const QString unrelated =
				    soundFixture(root.path(), QStringLiteral("sounds/TestWorld/legacy/alerts/different.wav"));
				QVERIFY(!unrelated.isEmpty());
				for (const QString &file : removed + survivors)
					QVERIFY(!soundFixture(root.path(), file).isEmpty());
				for (QString &file : removed)
					file = QDir(root.path()).filePath(file);
				for (QString &file : expected)
					file = QDir(root.path()).filePath(file);
				SoundEngine engine;
				engine.initializePlayback();
				engine.configureSoundDirectory(root.path());
				const auto session = engine.registerSession({}, QStringLiteral("sounds/TestWorld"));
				QVERIFY(session);
				QVERIFY(waitForSoundValue(
				    [&] { return static_cast<bool>(SoundEngineTestAccess::fileCatalogState(engine).ready); },
				    true));
				for (const QString &file : expected)
					QVERIFY(SoundEngineTestAccess::cacheFileRequest(engine, session, file, file));
				SoundEngineTestAccess::setCatalogPaused(engine, true);
				const QString request = mode == 1   ? QStringLiteral("legacy/alerts/cu*.wav")
				                        : mode == 2 ? QStringLiteral("legacy/alerts/cue")
				                                    : QStringLiteral("legacy/alerts/cue.wav");
				QCOMPARE(SoundEngineTestAccess::selectFilesAfterRemoval(engine, session, request, removed,
				                                                        mode != 0),
				         expected);
				for (const QString &file : removed)
					QVERIFY(!QFileInfo::exists(file));
				SoundEngineTestAccess::setCatalogPaused(engine, false);
				QVERIFY(waitForSoundValue(
				    [&] { return static_cast<bool>(SoundEngineTestAccess::fileCatalogState(engine).ready); },
				    true));
				QCOMPARE(waitForSoundValue(
				             [&] { return SoundEngineTestAccess::fileCatalogState(engine).fileCount; },
				             survivors.size() + 1),
				         survivors.size() + 1);
				if (mode == 0)
				{
					QCOMPARE(session->playFile(1, request, true, -100.0, 0.0),
					         expected.isEmpty() ? eFileNotFound : eOK);
					if (expected.isEmpty())
						return;
					QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, 1), expected.front());
					const quint64 slow     = SoundEngineTestAccess::slowFileResolutionCount(engine);
					const auto    backends = SoundEngineTestAccess::cacheState(engine).backendCount;
					QCOMPARE(session->playFile(1, request, true, -100.0, 0.0), eOK);
					QCOMPARE(SoundEngineTestAccess::slowFileResolutionCount(engine), slow);
					QCOMPARE(SoundEngineTestAccess::cacheState(engine).backendCount, backends);
					QCOMPARE(session->stop(1), eOK);
					const auto token = session->prepareFile(request);
					QVERIFY(token != 0);
					QCOMPARE(session->commitPrepared(token, 2, true, -100.0, 0.0), eOK);
					QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, 2), expected.front());
					QVERIFY(session->reserveLuaAudioChannel(request, 91) > 0);
				}
				else
				{
					auto native     = nativeSoundRequest(request);
					native.protocol = mode == 1 ? QMudNativeSoundProtocol::Msp : QMudNativeSoundProtocol::Mxp;
					native.repeatCount = -1;
					native.volume      = 0;
					QVERIFY(session->submitNativeSound(native));
					const auto playback = SoundEngineTestAccess::nativePlaybackState(
					    engine, session, native.protocol, QMudNativeSoundMedia::Sound);
					QCOMPARE(playback.current, !expected.isEmpty());
					if (expected.isEmpty())
						return;
					QVERIFY(expected.contains(
					    SoundEngineTestAccess::activeFileIdentity(engine, session, playback.buffer)));
					QVERIFY(SoundEngineTestAccess::completeActiveChannel(engine, session, playback.buffer));
					QVERIFY(expected.contains(
					    SoundEngineTestAccess::activeFileIdentity(engine, session, playback.buffer)));
				}
			}

			/** @brief Supplies file entry paths and path spellings for incomplete-catalog selection. */
			void incompleteCatalogPreservesWorldPriority_data()
			{
				QTest::addColumn<int>("entry");
				QTest::addColumn<QString>("request");
				for (int entry = 0; entry < 5; ++entry)
				{
					const QByteArray name = QByteArray::number(entry);
					QTest::newRow((name + "-relative").constData())
					    << entry << QStringLiteral("legacy/cue.wav");
					QTest::newRow((name + "-portable").constData())
					    << entry << QStringLiteral("sounds/legacy/cue.wav");
					QTest::newRow((name + "-absolute").constData()) << entry << QStringLiteral("absolute");
					if (entry < 3)
						QTest::newRow((name + "-plugin").constData())
						    << entry << QStringLiteral("worlds/plugins/AddOn/cue.wav");
					if (entry >= 3)
					{
						QTest::newRow((name + "-wildcard").constData())
						    << entry << QStringLiteral("legacy/cu*.wav");
						QTest::newRow((name + "-extensionless").constData())
						    << entry << QStringLiteral("legacy/cue");
					}
				}
			}

			/** @brief Verifies every cold entry waits rather than letting a shared exact beat a world fallback. */
			void incompleteCatalogPreservesWorldPriority()
			{
				QFETCH(int, entry);
				QFETCH(QString, request);
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-pending-priority-XXXXXX")));
				QVERIFY(root.isValid());
				const QString preferred =
				    soundFixture(root.path(), QStringLiteral("sounds/TestWorld/relocated/cue.wav"));
				const QString shared = soundFixture(root.path(), QStringLiteral("sounds/legacy/cue.wav"));
				QVERIFY(!preferred.isEmpty() && !shared.isEmpty());
				QVERIFY(!soundFixture(root.path(), QStringLiteral("worlds/plugins/AddOn/cue.wav")).isEmpty());
				if (request == QStringLiteral("absolute"))
					request = shared;
				SoundEngine engine;
				engine.initializePlayback();
				SoundEngineTestAccess::setCatalogPaused(engine, true);
				engine.configureSoundDirectory(root.path());
				engine.configurePluginDirectory(QDir(root.path()).filePath(QStringLiteral("worlds/plugins")));
				const auto session = engine.registerSession({}, QStringLiteral("sounds/TestWorld"));
				QVERIFY(session);
				QVERIFY(!SoundEngineTestAccess::fileCatalogState(engine).ready);
				if (entry >= 3)
				{
					auto requestValue = nativeSoundRequest(request);
					requestValue.protocol =
					    entry == 3 ? QMudNativeSoundProtocol::Msp : QMudNativeSoundProtocol::Mxp;
					requestValue.repeatCount = -1;
					requestValue.volume      = 0;
					QVERIFY(session->submitNativeSound(requestValue));
					const auto state = SoundEngineTestAccess::nativePlaybackState(
					    engine, session, requestValue.protocol, QMudNativeSoundMedia::Sound);
					QVERIFY(state.current);
					QVERIFY(
					    SoundEngineTestAccess::activeFileIdentity(engine, session, state.buffer).isEmpty());
					SoundEngineTestAccess::setCatalogPaused(engine, false);
					QTRY_COMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, state.buffer),
					             preferred);
					return;
				}
				int              buffer = 0;
				const quint64    token  = entry == 1 ? session->prepareFile(request) : 0;
				PendingSoundCall call(
				    session,
				    [&]() -> int
				    {
					    if (entry == 0)
						    return session->playFile(1, request, true, -100.0, 0.0, 0, &buffer);
					    if (entry == 1)
					    {
						    buffer = 1;
						    return session->commitPrepared(token, buffer, true, -100.0, 0.0);
					    }
					    buffer = session->reserveLuaAudioChannel(request, 71, 0.0);
					    return buffer > 0 ? eOK : eCannotPlaySound;
				    });
				QTRY_COMPARE(SoundEngineTestAccess::pendingFileCount(engine, session, entry == 1), 1);
				QVERIFY(!call.finished.load(std::memory_order_acquire));
				QVERIFY(SoundEngineTestAccess::activeFileIdentity(engine, session, 1).isEmpty());
				SoundEngineTestAccess::setCatalogPaused(engine, false);
				QTRY_VERIFY(call.finished.load(std::memory_order_acquire));
				QCOMPARE(call.result, eOK);
				if (entry == 2)
					QCOMPARE(session->playFile(buffer, request, true, -100.0, 0.0, 71, &buffer, true), eOK);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, buffer), preferred);
			}

			/** @brief Supplies both acceptance orders and replacement, control, and teardown transitions. */
			void delayedLookupKeepsIndependentReplacement_data()
			{
				QTest::addColumn<bool>("startFirst");
				QTest::addColumn<int>("transition");
				const QStringList names{QStringLiteral("replacement-success"),
				                        QStringLiteral("replacement-missing"),
				                        QStringLiteral("prepared-cancel"),
				                        QStringLiteral("guarded-stop"),
				                        QStringLiteral("stop-all"),
				                        QStringLiteral("module-free"),
				                        QStringLiteral("device-change"),
				                        QStringLiteral("root-change"),
				                        QStringLiteral("session-close"),
				                        QStringLiteral("shutdown"),
				                        QStringLiteral("ordinary-controls"),
				                        QStringLiteral("owned-controls"),
				                        QStringLiteral("reserved-source-missing"),
				                        QStringLiteral("exact-replacement"),
				                        QStringLiteral("stale-stop"),
				                        QStringLiteral("attribute-controls"),
				                        QStringLiteral("duplicate-start"),
				                        QStringLiteral("prepared-success"),
				                        QStringLiteral("prepared-missing"),
				                        QStringLiteral("stop-channel"),
				                        QStringLiteral("immediate-fade"),
				                        QStringLiteral("plugin-root-change"),
				                        QStringLiteral("controls-after-prepared-cancel")};
				for (const bool first : {false, true})
					for (int index = 0; index < names.size(); ++index)
						QTest::newRow(
						    qPrintable(names[index] + (first ? "-start-first" : "-replacement-first")))
						    << first << index;
			}

			/** @brief Verifies a deferred reservation start never steals an independent claim or outlives its owner. */
			void delayedLookupKeepsIndependentReplacement()
			{
				QFETCH(bool, startFirst);
				QFETCH(int, transition);
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-owned-lookup-XXXXXX")));
				QVERIFY(root.isValid());
				const QString old =
				    soundFixture(root.path(), QStringLiteral("sounds/TestWorld/moved/old.wav"));
				const QString replacement =
				    soundFixture(root.path(), QStringLiteral("sounds/TestWorld/moved/new.wav"));
				QVERIFY(!old.isEmpty());
				QVERIFY(!replacement.isEmpty());
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("plugins")));
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("other-plugins")));
				std::atomic_int               released{0};
				SoundEngine::SessionCallbacks callbacks;
				callbacks.luaAudioOwnershipReleased = [&released](int, const quint64 generation)
				{
					if (generation == 91)
						released.fetch_add(1, std::memory_order_relaxed);
				};
				auto engine = std::make_unique<SoundEngine>();
				engine->initializePlayback();
				SoundEngineTestAccess::setCatalogPaused(*engine, true);
				engine->configureSoundDirectory(root.path());
				engine->configurePluginDirectory(QDir(root.path()).filePath(QStringLiteral("plugins")));
				const auto session =
				    engine->registerSession(std::move(callbacks), QStringLiteral("sounds/TestWorld"));
				QVERIFY(session);
				const int buffer = session->reserveLuaAudioChannel(old, 91, 34.0);
				QCOMPARE(buffer, 1);
				QVERIFY(session->confirmLuaAudioOwnership(buffer, 91));
				if (transition == 12)
					QVERIFY(QFile::remove(old));
				const bool prepared =
				    transition == 2 || transition == 17 || transition == 18 || transition == 22;
				const bool    missing = transition == 1 || transition == 10 || transition == 11 ||
				                        transition == 15 || transition == 18;
				const QString startRequest =
				    transition == 21 ? QDir(root.path()).filePath(QStringLiteral("plugins/old.wav"))
				                     : QStringLiteral("legacy/old.wav");
				const QString nextRequest =
				    missing ? QStringLiteral("legacy/missing.wav") : QStringLiteral("legacy/new.wav");
				quint64                           token = 0;
				std::unique_ptr<PendingSoundCall> start;
				std::unique_ptr<PendingSoundCall> next;
				// Each first submission is observed in worker state before accepting the second.
				for (int index = 0; index < 2; ++index)
				{
					if ((index == 0) == startFirst)
						start = std::make_unique<PendingSoundCall>(session,
						                                           [&]
						                                           {
							                                           return session->playFile(
							                                               buffer, startRequest, true, 0.0,
							                                               100.0, 91, nullptr, true, 10.0);
						                                           });
					else
					{
						if (prepared)
						{
							token = session->prepareFile(nextRequest);
							QVERIFY(token != 0);
						}
						next = std::make_unique<PendingSoundCall>(
						    session,
						    [&]
						    {
							    return prepared ? session->commitPrepared(token, buffer, true, -80.0, 0.0)
							                    : session->playFile(buffer, nextRequest, true, -80.0, 0.0);
						    });
					}
					QCOMPARE(waitForSoundValue(
					             [&] { return SoundEngineTestAccess::pendingFileCount(*engine, session); },
					             index + 1),
					         index + 1);
				}
				if (prepared)
					QCOMPARE(waitForSoundValue(
					             [&]
					             { return SoundEngineTestAccess::pendingFileCount(*engine, session, true); },
					             1),
					         1);
				QVERIFY(!start->finished.load(std::memory_order_acquire));
				QVERIFY(!next->finished.load(std::memory_order_acquire));
				switch (transition)
				{
				case 2:
				case 22:
					session->cancelPrepared(token);
					QVERIFY(waitForSoundValue(
					    [&] { return static_cast<bool>(next->finished.load(std::memory_order_acquire)); },
					    true));
					QVERIFY(!start->finished.load(std::memory_order_acquire));
					if (transition == 22)
						QCOMPARE(session->playFile(buffer, {}, true, -39.0, -25.0), eOK);
					break;
				case 3:
					QCOMPARE(session->stopLuaAudioChannel(buffer, 91), eOK);
					break;
				case 4:
					QCOMPARE(session->stop(0), eOK);
					break;
				case 5:
					QCOMPARE(session->setLuaAudioInitialized(false), eOK);
					break;
				case 6:
					SoundEngineTestAccess::resetForAudioDeviceChange(*engine);
					break;
				case 7:
					engine->configureSoundDirectory(testSoundHome());
					break;
				case 8:
					session->close();
					break;
				case 9:
					engine.reset();
					break;
				case 10:
					QCOMPARE(session->playFile(buffer, {}, true, -39.0, -25.0), eOK);
					break;
				case 11:
					QCOMPARE(session->playFile(buffer, {}, true, -59.0, -25.0, 91), eOK);
					break;
				case 13:
					QCOMPARE(session->playFile(buffer, replacement, true, -80.0, 0.0), eOK);
					break;
				case 14:
					QCOMPARE(session->stopLuaAudioChannel(buffer, 92), eCannotPlaySound);
					break;
				case 15:
					QCOMPARE(session->setLuaAudioAttribute(buffer, 91, QMudSoundAttribute::Volume, 35.25),
					         eOK);
					QCOMPARE(session->setLuaAudioAttribute(buffer, 91, QMudSoundAttribute::Pan, -50.0), eOK);
					QCOMPARE(session->setLuaAudioAttribute(buffer, 91, QMudSoundAttribute::Pitch, 8.5), eOK);
					break;
				case 16:
					QCOMPARE(session->playFile(buffer, startRequest, true, 0.0, 0.0, 91, nullptr, true),
					         eCannotPlaySound);
					QCOMPARE(SoundEngineTestAccess::pendingFileCount(*engine, session), 2);
					break;
				case 19:
					QCOMPARE(session->stop(buffer), eOK);
					break;
				case 20:
					QCOMPARE(
					    session->setLuaAudioAttribute(buffer, 91, QMudSoundAttribute::Volume, 0.0, {}, true),
					    eOK);
					break;
				case 21:
					engine->configurePluginDirectory(
					    QDir(root.path()).filePath(QStringLiteral("other-plugins")));
					break;
				default:
					break;
				}
				const bool canceledBoth  = transition == 4 || (transition >= 6 && transition <= 9) ||
				                           transition == 13 || transition == 19;
				const bool canceledOwner = canceledBoth || transition == 3 || transition == 5 ||
				                           transition == 20 || transition == 21;
				if (canceledOwner)
				{
					QVERIFY(waitForSoundValue(
					    [&] { return static_cast<bool>(start->finished.load(std::memory_order_acquire)); },
					    true));
					QCOMPARE(start->result, eCannotPlaySound);
					QCOMPARE(released.load(), 1);
					if (!canceledBoth)
					{
						QVERIFY(!next->finished.load(std::memory_order_acquire));
						QCOMPARE(SoundEngineTestAccess::pendingFileCount(*engine, session), 1);
					}
				}
				if (engine && transition != 8)
					SoundEngineTestAccess::setCatalogPaused(*engine, false);
				QVERIFY(waitForSoundValue(
				    [&] { return static_cast<bool>(start->finished.load(std::memory_order_acquire)); },
				    true));
				QVERIFY(waitForSoundValue(
				    [&] { return static_cast<bool>(next->finished.load(std::memory_order_acquire)); }, true));
				const bool canceledPrepared = transition == 2 || transition == 22;
				QCOMPARE(next->result, canceledBoth || canceledPrepared ? eCannotPlaySound
				                       : missing                        ? eFileNotFound
				                                                        : eOK);
				if (!canceledOwner)
					QCOMPARE(start->result, !startFirst && !missing && !canceledPrepared ? eCannotPlaySound
					                        : transition == 12                           ? eFileNotFound
					                                                                     : eOK);
				if (engine && transition != 8)
				{
					QCOMPARE(SoundEngineTestAccess::pendingFileCount(*engine, session), 0);
					const bool oldActive = missing || canceledPrepared;
					if (!canceledBoth || transition == 13)
						QCOMPARE(SoundEngineTestAccess::activeFileIdentity(*engine, session, buffer),
						         oldActive ? old : replacement);
					else
						QVERIFY(
						    SoundEngineTestAccess::activeFileIdentity(*engine, session, buffer).isEmpty());
					if (oldActive)
					{
						QCOMPARE(released.load(), 0);
						const double gain = transition == 10 || transition == 22 ? 61.0
						                    : transition == 11                   ? 41.0
						                    : transition == 15                   ? 35.25
						                                                         : 34.0;
						QCOMPARE(SoundEngineTestAccess::activeAttribute(*engine, session, buffer,
						                                                QMudSoundAttribute::Volume),
						         gain);
						QCOMPARE(SoundEngineTestAccess::activeAttribute(*engine, session, buffer,
						                                                QMudSoundAttribute::Pan),
						         transition == 10 || transition == 11 || transition == 22 ? -25.0
						         : transition == 15                                       ? -50.0
						                                                                  : 0.0);
						QCOMPARE(SoundEngineTestAccess::activeAttribute(*engine, session, buffer,
						                                                QMudSoundAttribute::Pitch),
						         transition == 15 ? 8.5 : 0.0);
					}
					QCOMPARE(session->stop(buffer), eOK);
					QCOMPARE(released.load(), 1);
				}
			}

			/** @brief Supplies cancellation and failure transitions for a pending replacement. */
			void pendingFileReplacementLifecycle_data()
			{
				QTest::addColumn<int>("transition");
				QTest::newRow("missing-keeps-old-voice") << 0;
				QTest::newRow("stop-channel") << 1;
				QTest::newRow("stop-all") << 2;
				QTest::newRow("same-source-replacement") << 3;
				QTest::newRow("session-close") << 4;
				QTest::newRow("device-change") << 5;
				QTest::newRow("luaaudio-free") << 6;
				QTest::newRow("engine-shutdown") << 7;
				QTest::newRow("root-change") << 8;
			}

			/** @brief Verifies pending lookup preserves old playback and cannot revive canceled assignments. */
			void pendingFileReplacementLifecycle()
			{
				QFETCH(int, transition);
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-pending-life-XXXXXX")));
				QVERIFY(root.isValid());
				const QString old = soundFixture(root.path(), QStringLiteral("sounds/TestWorld/keep.wav"));
				QVERIFY(!old.isEmpty());
				QVERIFY(!soundFixture(root.path(), QStringLiteral("sounds/TestWorld/relocated/cue.wav"))
				             .isEmpty());
				auto engine = std::make_unique<SoundEngine>();
				engine->initializePlayback();
				SoundEngineTestAccess::setCatalogPaused(*engine, true);
				engine->configureSoundDirectory(root.path());
				const auto session = engine->registerSession({}, QStringLiteral("sounds/TestWorld"));
				QVERIFY(session);
				QCOMPARE(session->playFile(1, QStringLiteral("keep.wav"), true, -100.0, 0.0), eOK);
				const QString request =
				    transition == 0 ? QStringLiteral("missing.wav") : QStringLiteral("legacy/cue.wav");
				PendingSoundCall call(
				    session, [&]
				    { return session->playFile(1, request, true, -100.0, 0.0, transition == 6 ? 72 : 0); });
				QTRY_COMPARE(SoundEngineTestAccess::pendingFileCount(*engine, session), 1);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(*engine, session, 1), old);
				switch (transition)
				{
				case 0:
					SoundEngineTestAccess::setCatalogPaused(*engine, false);
					break;
				case 1:
					QCOMPARE(session->stop(1), eOK);
					break;
				case 2:
					QCOMPARE(session->stop(0), eOK);
					break;
				case 3:
					QCOMPARE(session->playFile(1, QStringLiteral("keep.wav"), true, -100.0, 0.0), eOK);
					break;
				case 4:
					session->close();
					break;
				case 5:
					SoundEngineTestAccess::resetForAudioDeviceChange(*engine);
					break;
				case 6:
					QCOMPARE(session->setLuaAudioInitialized(false), eOK);
					break;
				case 7:
					engine.reset();
					break;
				case 8:
					engine->configureSoundDirectory(testSoundHome());
					break;
				default:
					QFAIL("Unexpected pending file lifecycle transition");
				}
				QTRY_VERIFY(call.finished.load(std::memory_order_acquire));
				QCOMPARE(call.result, transition == 0 ? eFileNotFound : eCannotPlaySound);
				if (engine && transition != 4)
				{
					SoundEngineTestAccess::setCatalogPaused(*engine, false);
					QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(*engine).ready);
					QCOMPARE(SoundEngineTestAccess::pendingFileCount(*engine, session), 0);
					if (transition == 0 || transition == 3 || transition == 6 || transition == 8)
						QCOMPARE(SoundEngineTestAccess::activeFileIdentity(*engine, session, 1), old);
					else
						QVERIFY(SoundEngineTestAccess::activeFileIdentity(*engine, session, 1).isEmpty());
				}
			}

			/** @brief Supplies older-assignment transitions that must preserve a pending ordinary play. */
			void pendingFileSurvivesOlderOwnershipTransitions_data()
			{
				QTest::addColumn<int>("transition");
				QTest::newRow("volume-update") << 0;
				QTest::newRow("generation-stop") << 1;
				QTest::newRow("natural-completion") << 2;
				QTest::newRow("immediate-fade") << 3;
				QTest::newRow("module-free") << 4;
			}

			/** @brief Verifies old ownership completion and controls cannot cancel or overwrite later accepted file work. */
			void pendingFileSurvivesOlderOwnershipTransitions()
			{
				QFETCH(int, transition);
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-pending-owner-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(!soundFixture(root.path(), QStringLiteral("sounds/TestWorld/keep.wav")).isEmpty());
				const QString replacement =
				    soundFixture(root.path(), QStringLiteral("sounds/TestWorld/relocated/cue.wav"));
				QVERIFY(!replacement.isEmpty());
				SoundEngine engine;
				engine.initializePlayback();
				SoundEngineTestAccess::setCatalogPaused(engine, true);
				engine.configureSoundDirectory(root.path());
				const auto session = engine.registerSession({}, QStringLiteral("sounds/TestWorld"));
				QVERIFY(session);
				QCOMPARE(session->playFile(1, QStringLiteral("keep.wav"), true, -100.0, 0.0, 73), eOK);
				QVERIFY(session->confirmLuaAudioOwnership(1, 73));
				PendingSoundCall call(
				    session,
				    [&] { return session->playFile(1, QStringLiteral("cue.wav"), true, -100.0, 0.0); });
				QTRY_COMPARE(SoundEngineTestAccess::pendingFileCount(engine, session), 1);
				switch (transition)
				{
				case 0:
					QCOMPARE(session->playFile(1, QString(), true, -80.0, 0.0), eOK);
					break;
				case 1:
					QCOMPARE(session->stopLuaAudioChannel(1, 73), eOK);
					break;
				case 2:
					QVERIFY(SoundEngineTestAccess::completeActiveChannel(engine, session, 1));
					break;
				case 3:
					QCOMPARE(session->setLuaAudioAttribute(1, 73, QMudSoundAttribute::Volume, 0.0, {}, true),
					         eOK);
					break;
				case 4:
					QCOMPARE(session->setLuaAudioInitialized(false), eOK);
					break;
				default:
					QFAIL("Unexpected older ownership transition");
				}
				QCOMPARE(SoundEngineTestAccess::pendingFileCount(engine, session), 1);
				SoundEngineTestAccess::setCatalogPaused(engine, false);
				QTRY_VERIFY(call.finished.load(std::memory_order_acquire));
				QCOMPARE(call.result, eOK);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, 1), replacement);
				QCOMPARE(session->snapshot()->volume[0], transition == 0 ? 20.0 : 0.0);
			}

			/** @brief Verifies direct completion snapshots retain accepted values across subsequent commands. */
			void soundMutationReturnsItsOwnPublishedState()
			{
				SoundEngine engine;
				configureTestSoundCatalog(engine);
				const auto session = engine.registerSession({});
				QVERIFY(session);
				std::shared_ptr<const SoundEngineSession::Snapshot> started;
				std::shared_ptr<const SoundEngineSession::Snapshot> updated;
				std::shared_ptr<const SoundEngineSession::Snapshot> stopped;
				QCOMPARE(session->playFile(1, testSoundFileName(), true, -80.0, 0.0, 0, nullptr, false, 0.0,
				                           &started),
				         eOK);
				QCOMPARE(session->playFile(1, QString(), true, -40.0, 0.0, 0, nullptr, false, 0.0, &updated),
				         eOK);
				QCOMPARE(session->stop(1, &stopped), eOK);
				QVERIFY(started && updated && stopped);
				QCOMPARE(started->volume[0], 20.0);
				QCOMPARE(updated->volume[0], 60.0);
				QCOMPARE(stopped->volume[0], 0.0);
				QCOMPARE(stopped->status[0], -2);
				QCOMPARE(session->playMemory(1, QMudTestSoundData::silentPcmWave(300), true, -70.0, 0.0, 0,
				                             nullptr, &started),
				         eOK);
				QVERIFY(started);
				QCOMPARE(started->volume[0], 30.0);
			}

			/** @brief Verifies cancellation can retire a prepared token with a commit already waiting on lookup. */
			void pendingPreparationCanBeCancelled()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-pending-prepare-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(!soundFixture(root.path(), QStringLiteral("sounds/TestWorld/relocated/cue.wav"))
				             .isEmpty());
				SoundEngine engine;
				engine.initializePlayback();
				SoundEngineTestAccess::setCatalogPaused(engine, true);
				engine.configureSoundDirectory(root.path());
				const auto session = engine.registerSession({}, QStringLiteral("sounds/TestWorld"));
				QVERIFY(session);
				const quint64 intercepted = session->prepareFile(QStringLiteral("cue.wav"));
				QTRY_COMPARE(SoundEngineTestAccess::pendingFileCount(engine, session), 1);
				session->cancelPrepared(intercepted);
				QTRY_COMPARE(SoundEngineTestAccess::pendingFileCount(engine, session), 0);
				const quint64    token = session->prepareFile(QStringLiteral("cue.wav"));
				PendingSoundCall call(session,
				                      [&] { return session->commitPrepared(token, 1, true, -100.0, 0.0); });
				QTRY_COMPARE(SoundEngineTestAccess::pendingFileCount(engine, session, true), 1);
				session->cancelPrepared(token);
				QTRY_VERIFY(call.finished.load(std::memory_order_acquire));
				QCOMPARE(call.result, eCannotPlaySound);
				SoundEngineTestAccess::setCatalogPaused(engine, false);
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				QVERIFY(SoundEngineTestAccess::activeFileIdentity(engine, session, 1).isEmpty());
			}

			/** @brief Verifies a deferred occurrence keeps its captured world context while later requests use new preferences. */
			void pendingFileKeepsCapturedWorldContext()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-pending-context-XXXXXX")));
				QVERIFY(root.isValid());
				const QString first = soundFixture(
				    root.path(), QStringLiteral("sounds/TestWorld/z/worlds/pluginsB/alerts/cue.wav"));
				const QString second =
				    soundFixture(root.path(), QStringLiteral("sounds/Other/relocated/cue.wav"));
				QVERIFY(!first.isEmpty() && !second.isEmpty());
				QVERIFY(!soundFixture(root.path(), QStringLiteral("sounds/TestWorld/a/alerts/cue.wav"))
				             .isEmpty());
				SoundEngine engine;
				engine.initializePlayback();
				SoundEngineTestAccess::setCatalogPaused(engine, true);
				engine.configureSoundDirectory(root.path());
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("worlds/plugins")));
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("worlds/pluginsB")));
				engine.configurePluginDirectory(QDir(root.path()).filePath(QStringLiteral("worlds/plugins")));
				const auto       session = engine.registerSession({}, QStringLiteral("sounds/TestWorld"));
				PendingSoundCall call(session,
				                      [&]
				                      {
					                      return session->playFile(
					                          1, QStringLiteral("worlds/pluginsB/alerts/cue.wav"), true,
					                          -100.0, 0.0);
				                      });
				QTRY_COMPARE(SoundEngineTestAccess::pendingFileCount(engine, session), 1);
				session->updateWorldSoundDirectory(QStringLiteral("sounds/Other"));
				engine.configurePluginDirectory(
				    QDir(root.path()).filePath(QStringLiteral("worlds/pluginsB")));
				QCOMPARE(SoundEngineTestAccess::pendingFileCount(engine, session), 1);
				SoundEngineTestAccess::setCatalogPaused(engine, false);
				QTRY_VERIFY(call.finished.load(std::memory_order_acquire));
				QCOMPARE(call.result, eOK);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, 1), first);
				QCOMPARE(
				    session->playFile(2, QStringLiteral("worlds/pluginsB/alerts/cue.wav"), true, -100.0, 0.0),
				    eOK);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, 2), second);
			}

			/** @brief Verifies plugin-root changes invalidate affected request aliases even when the selected file is a world override. */
			void pluginRootChangesInvalidateWorldOverrideAliases()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-plugin-alias-XXXXXX")));
				QVERIFY(root.isValid());
				const QString pluginWorld =
				    soundFixture(root.path(), QStringLiteral("sounds/TestWorld/AddOn/prompt.wav"));
				const QString ordinaryWorld = soundFixture(
				    root.path(), QStringLiteral("sounds/TestWorld/worlds/plugins/AddOn/prompt.wav"));
				const QString unaffected =
				    soundFixture(root.path(), QStringLiteral("sounds/TestWorld/unrelated.wav"));
				QVERIFY(!pluginWorld.isEmpty() && !ordinaryWorld.isEmpty() && !unaffected.isEmpty());
				QVERIFY(
				    !soundFixture(root.path(), QStringLiteral("worlds/plugins/AddOn/prompt.wav")).isEmpty());
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("custom/plugins")));
				SoundEngine engine;
				engine.initializePlayback();
				engine.configureSoundDirectory(root.path());
				engine.configurePluginDirectory(QDir(root.path()).filePath(QStringLiteral("worlds/plugins")));
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				const auto session = engine.registerSession({}, QStringLiteral("sounds/TestWorld"));
				QVERIFY(session);
				const QString request = QStringLiteral("worlds/plugins/AddOn/prompt.wav");
				QCOMPARE(session->playFile(1, request, true, -100.0, 0.0), eOK);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, 1), pluginWorld);
				QCOMPARE(session->playFile(2, QStringLiteral("unrelated.wav"), true, -100.0, 0.0), eOK);
				engine.configurePluginDirectory(QDir(root.path()).filePath(QStringLiteral("custom/plugins")));
				QCOMPARE(session->playFile(1, request, true, -100.0, 0.0), eOK);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, 1), ordinaryWorld);
				const quint64 resolved = SoundEngineTestAccess::slowFileResolutionCount(engine);
				QCOMPARE(session->playFile(2, QStringLiteral("unrelated.wav"), true, -100.0, 0.0), eOK);
				QCOMPARE(SoundEngineTestAccess::slowFileResolutionCount(engine), resolved);
				engine.configurePluginDirectory(QDir(root.path()).filePath(QStringLiteral("worlds/plugins")));
				QCOMPARE(session->playFile(1, request, true, -100.0, 0.0), eOK);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, 1), pluginWorld);
				QCOMPARE(SoundEngineTestAccess::slowFileResolutionCount(engine), resolved);
			}

			/** @brief Verifies root reconfiguration cancels pending native catalog requests across both protocols. */
			void nativePendingLookupIsCancelledOnRootChange()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-native-root-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(!soundFixture(root.path(), QStringLiteral("sounds/TestWorld/relocated/cue.wav"))
				             .isEmpty());
				SoundEngine engine;
				engine.initializePlayback();
				SoundEngineTestAccess::setCatalogPaused(engine, true);
				engine.configureSoundDirectory(root.path());
				const auto session = engine.registerSession({}, QStringLiteral("sounds/TestWorld"));
				QVERIFY(session);
				for (const auto protocol : {QMudNativeSoundProtocol::Msp, QMudNativeSoundProtocol::Mxp})
				{
					auto request     = nativeSoundRequest(QStringLiteral("legacy/cue.wav"));
					request.protocol = protocol;
					request.volume   = 0;
					QVERIFY(session->submitNativeSound(request));
					QVERIFY(SoundEngineTestAccess::nativePlaybackState(engine, session, protocol,
					                                                   QMudNativeSoundMedia::Sound)
					            .current);
				}
				engine.configureSoundDirectory(testSoundHome());
				for (const auto protocol : {QMudNativeSoundProtocol::Msp, QMudNativeSoundProtocol::Mxp})
					QVERIFY(!SoundEngineTestAccess::nativePlaybackState(engine, session, protocol,
					                                                    QMudNativeSoundMedia::Sound)
					             .current);
				SoundEngineTestAccess::setCatalogPaused(engine, false);
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				QCOMPARE(session->status(1), -2);
				QCOMPARE(session->status(2), -2);
			}

			/** @brief Verifies natural completion releases LuaAudio query ownership while retaining its cached backend. */
			void completedLuaAudioVolumeDoesNotBecomeMasterVolume()
			{
				SoundEngine engine;
				configureTestSoundCatalog(engine);
				WorldRuntime runtime;
				runtime.setStartupDirectory(testSoundHome());
				runtime.applyDefaultWorldOptions();
				runtime.attachSoundEngine(&engine);
				const auto played = QMudNativePluginRegistry::callLuaAudio(&runtime, QStringLiteral("play"),
				                                                           {testSoundFileName(), 0, 0, 0});
				QCOMPARE(played.errorCode, eOK);
				const int buffer = played.returnValues.front().toInt();
				QVERIFY(buffer > 0);
				const auto session = WorldRuntimeTestAccess::soundSession(runtime);
				QVERIFY(SoundEngineTestAccess::completeActiveChannel(engine, session, buffer));
				QCOMPARE(runtime.soundStatus(buffer), 0);
				QCOMPARE(
				    QMudNativePluginRegistry::callLuaAudio(&runtime, QStringLiteral("getVolume"), {buffer})
				        .returnValues.front()
				        .toDouble(),
				    0.0);
				QCOMPARE(QMudNativePluginRegistry::callLuaAudio(&runtime, QStringLiteral("getVolume"), {})
				             .returnValues.front()
				             .toDouble(),
				         100.0);
				QVERIFY(SoundEngineTestAccess::cacheState(engine).backendCount > 0);
			}

			/** @brief Verifies world-first selection for every path form before and after catalog completion. */
			void worldDirectoryPriorityAcrossFileEntryPaths()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("world-priority-XXXXXX")));
				QVERIFY(root.isValid());
				const auto fixture = [&](const QString &relative)
				{
					QString path = QDir(root.path()).filePath(relative);
					if (!QDir().mkpath(QFileInfo(path).absolutePath()) ||
					    !QFile::copy(testSoundFileName(), path))
						return QString();
					return path;
				};
				const QString shared         = fixture(QStringLiteral("sounds/shared/voice.wav"));
				const QString preferred      = fixture(QStringLiteral("sounds/TestWorld/shared/voice.wav"));
				const QString other          = fixture(QStringLiteral("sounds/Other/shared/voice.wav"));
				const QString plugin         = fixture(QStringLiteral("worlds/plugins/AddOn/prompt.wav"));
				const QString pluginOverride = fixture(QStringLiteral("sounds/TestWorld/AddOn/prompt.wav"));
				const QString relocated      = fixture(QStringLiteral("sounds/TestWorld/relocated/cue.wav"));
				QVERIFY(!shared.isEmpty() && !preferred.isEmpty() && !other.isEmpty() && !plugin.isEmpty() &&
				        !pluginOverride.isEmpty() && !relocated.isEmpty());
				QVERIFY(!fixture(QStringLiteral("sounds/legacy/cue.wav")).isEmpty());
				SoundEngine engine;
				engine.initializePlayback();
				SoundEngineTestAccess::setCatalogPaused(engine, true);
				engine.configureSoundDirectory(root.path());
				engine.configurePluginDirectory(QDir(root.path()).filePath(QStringLiteral("worlds/plugins")));
				const auto session = engine.registerSession({}, QStringLiteral("sounds/TestWorld"));
				QVERIFY(session);
				QVERIFY(!SoundEngineTestAccess::fileCatalogState(engine).ready);
				quint64 generation = 100;
				for (const bool catalogReady : {false, true})
				{
					if (catalogReady)
					{
						SoundEngineTestAccess::setCatalogPaused(engine, false);
						QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
					}
					for (const QString &request :
					     QStringList{QStringLiteral("shared/voice.wav"),
					                 QStringLiteral("sounds/shared/voice.wav"), shared, preferred})
					{
						int buffer = 0;
						QCOMPARE(session->playFile(0, request, true, 0.0, 0.0, 0, &buffer), eOK);
						QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, buffer),
						         preferred);
						QCOMPARE(session->stop(buffer), eOK);
						const quint64 prepared = session->prepareFile(request);
						QVERIFY(prepared != 0);
						QCOMPARE(session->commitPrepared(prepared, 1, false, 0.0, 0.0), eOK);
						QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, 1), preferred);
						QCOMPARE(session->stop(1), eOK);
						++generation;
						const int reserved = session->reserveLuaAudioChannel(request, generation);
						QVERIFY(reserved > 0);
						QCOMPARE(
						    session->playFile(reserved, request, true, 0.0, 0.0, generation, &buffer, true),
						    eOK);
						QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, buffer),
						         preferred);
						QVERIFY(session->confirmLuaAudioOwnership(buffer, generation));
						QCOMPARE(session->stop(buffer), eOK);
						QVERIFY(session->submitNativeSound(nativeSoundRequest(request)));
						const auto native = SoundEngineTestAccess::nativePlaybackState(
						    engine, session, QMudNativeSoundProtocol::Msp, QMudNativeSoundMedia::Sound);
						QVERIFY(native.current);
						QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, native.buffer),
						         preferred);
						QCOMPARE(session->stop(native.buffer), eOK);
					}
					for (const QString &request :
					     QStringList{QStringLiteral("worlds/plugins/AddOn/prompt.wav"), plugin})
					{
						int buffer = 0;
						QCOMPARE(session->playFile(0, request, true, 0.0, 0.0, 0, &buffer), eOK);
						QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, buffer),
						         pluginOverride);
						QCOMPARE(session->stop(buffer), eOK);
					}
				}
				int buffer = 0;
				QCOMPARE(
				    session->playFile(0, QStringLiteral("sounds/legacy/cue.wav"), true, 0.0, 0.0, 0, &buffer),
				    eOK);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, buffer), relocated);
				const quint64 slow = SoundEngineTestAccess::slowFileResolutionCount(engine);
				QCOMPARE(session->playFile(buffer, QStringLiteral("sounds/legacy/cue.wav"), true, 0.0, 0.0),
				         eOK);
				QCOMPARE(SoundEngineTestAccess::slowFileResolutionCount(engine), slow);
				QCOMPARE(session->stop(buffer), eOK);
				for (const QString &pattern :
				     {QStringLiteral("sounds/shared/vo*.wav"), QStringLiteral("sounds/shared/voice")})
				{
					QVERIFY(session->submitNativeSound(nativeSoundRequest(pattern)));
					const auto native = SoundEngineTestAccess::nativePlaybackState(
					    engine, session, QMudNativeSoundProtocol::Msp, QMudNativeSoundMedia::Sound);
					QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, native.buffer),
					         preferred);
					QCOMPARE(session->stop(native.buffer), eOK);
				}
				const quint64 prepared = session->prepareFile(shared);
				QVERIFY(prepared != 0);
				auto nativeRequest        = nativeSoundRequest(shared);
				nativeRequest.repeatCount = 2;
				QVERIFY(session->submitNativeSound(nativeRequest));
				const auto native = SoundEngineTestAccess::nativePlaybackState(
				    engine, session, QMudNativeSoundProtocol::Msp, QMudNativeSoundMedia::Sound);
				session->updateWorldSoundDirectory(QStringLiteral("sounds/Other"));
				QCOMPARE(session->commitPrepared(prepared, 2, false, 0.0, 0.0), eOK);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, 2), preferred);
				QVERIFY(SoundEngineTestAccess::completeActiveChannel(engine, session, native.buffer));
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, native.buffer),
				         preferred);
				QCOMPARE(session->playFile(3, shared, true, 0.0, 0.0), eOK);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, 3), other);
			}

			/** @brief Supplies each entry boundary that assigns a normalized playback gain. */
			void playbackGainNormalization_data()
			{
				QTest::addColumn<int>("entry");
				QTest::newRow("direct-file") << 0;
				QTest::newRow("prepared-file") << 1;
				QTest::newRow("memory") << 2;
				QTest::newRow("reserved-file") << 3;
			}

			/** @brief Verifies every integer percentage and fractional reserved gains survive playback assignment exactly. */
			void playbackGainNormalization()
			{
				QFETCH(int, entry);
				SoundEngine engine;
				configureTestSoundCatalog(engine);
				const auto session = engine.registerSession({});
				QVERIFY(session);
				const QByteArray memory = QMudTestSoundData::silentPcmWave(300);
				for (int index = 0; index <= (entry == 3 ? 103 : 100); ++index)
				{
					const double percent = index <= 100   ? static_cast<double>(index)
					                       : index == 101 ? 34.25
					                       : index == 102 ? 41.125
					                                      : 0.00125;
					if (entry == 0)
						QCOMPARE(session->playFile(1, testSoundFileName(), true, percent - 100.0, 0.0), eOK);
					else if (entry == 1)
					{
						const quint64 token = session->prepareFile(testSoundFileName());
						QVERIFY(token != 0);
						QCOMPARE(session->commitPrepared(token, 1, true, percent - 100.0, 0.0), eOK);
					}
					else if (entry == 2)
						QCOMPARE(session->playMemory(1, memory, true, percent - 100.0, 0.0), eOK);
					else
					{
						const auto generation = static_cast<quint64>(index) + 1;
						QCOMPARE(session->reserveLuaAudioChannel(testSoundFileName(), generation), 1);
						QCOMPARE(
						    session->setLuaAudioAttribute(1, generation, QMudSoundAttribute::Volume, percent),
						    eOK);
						QCOMPARE(session->playFile(1, testSoundFileName(), true, 0.0, 0.0, generation,
						                           nullptr, true),
						         eOK);
					}
					const double observed = SoundEngineTestAccess::activeAttribute(
					    engine, session, 1, QMudSoundAttribute::Volume);
					const double expected = (percent / 100.0) * 100.0;
					QVERIFY2(observed == expected,
					         qPrintable(QStringLiteral("%1 percent: actual %2, expected %3")
					                        .arg(percent)
					                        .arg(observed, 0, 'g', 17)
					                        .arg(expected, 0, 'g', 17)));
					QCOMPARE(session->stop(1), eOK);
				}
			}

			/** @brief Verifies native catalog continuations consume the current linear gain without a unit round trip. */
			void nativeLookupPreservesCurrentGain()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-native-gain-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(
				    !soundFixture(root.path(), QStringLiteral("sounds/TestWorld/moved/cue.wav")).isEmpty());
				SoundEngine engine;
				engine.initializePlayback();
				SoundEngineTestAccess::setCatalogPaused(engine, true);
				engine.configureSoundDirectory(root.path());
				std::vector<std::shared_ptr<SoundEngineSession>> sessions;
				for (const QMudNativeSoundProtocol protocol :
				     {QMudNativeSoundProtocol::Msp, QMudNativeSoundProtocol::Mxp})
					for (const QMudNativeSoundMedia media :
					     {QMudNativeSoundMedia::Sound, QMudNativeSoundMedia::Music})
					{
						const auto session = engine.registerSession({}, QStringLiteral("sounds/TestWorld"));
						QVERIFY(session);
						auto request        = nativeSoundRequest(QStringLiteral("legacy/cue.wav"), media);
						request.protocol    = protocol;
						request.repeatCount = -1;
						request.volume      = 34;
						QVERIFY(session->submitNativeSound(request));
						const int buffer =
						    SoundEngineTestAccess::nativePlaybackState(engine, session, protocol, media)
						        .buffer;
						QVERIFY(buffer > 0);
						QCOMPARE(session->playFile(buffer, {}, true, -59.0, 0.0), eOK);
						sessions.push_back(session);
					}
				SoundEngineTestAccess::setCatalogPaused(engine, false);
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				for (const auto &session : sessions)
				{
					int playing = 0;
					for (int buffer = 1; buffer <= SoundEngineSession::kChannelCount; ++buffer)
						if (!SoundEngineTestAccess::activeFileIdentity(engine, session, buffer).isEmpty())
						{
							++playing;
							QVERIFY(SoundEngineTestAccess::activeAttribute(engine, session, buffer,
							                                               QMudSoundAttribute::Volume) ==
							        (41.0 / 100.0) * 100.0);
						}
					QCOMPARE(playing, 1);
				}
			}

			/** @brief Supplies terminal-event orders and control, cancellation, failure, and backpressure cases. */
			void loopUpdateSurvivesDecoderCompletion_data()
			{
				QTest::addColumn<int>("ordering");
				QTest::addColumn<int>("transition");
				const QStringList orders{QStringLiteral("update-before-end"),
				                         QStringLiteral("end-update-stop"), QStringLiteral("stop-update-end"),
				                         QStringLiteral("end-stop-update"),
				                         QStringLiteral("stop-end-update")};
				const QStringList transitions{QStringLiteral("continue"),
				                              QStringLiteral("disable-loop"),
				                              QStringLiteral("stop"),
				                              QStringLiteral("replace-owner"),
				                              QStringLiteral("module-free"),
				                              QStringLiteral("decoder-error"),
				                              QStringLiteral("completed-fade"),
				                              QStringLiteral("native-repeat-policy"),
				                              QStringLiteral("session-close"),
				                              QStringLiteral("device-change"),
				                              QStringLiteral("pending-replacement"),
				                              QStringLiteral("backpressure-hysteresis"),
				                              QStringLiteral("disable-reenable-loop"),
				                              QStringLiteral("low-water-continuation")};
				for (int order = 0; order < orders.size(); ++order)
					for (int transition = 0; transition < transitions.size(); ++transition)
						QTest::newRow(qPrintable(orders[order] + QLatin1Char('-') + transitions[transition]))
						    << order << transition;
			}

			/** @brief Verifies repeat updates and terminal events retain PCM and ownership through production worker paths. */
			void loopUpdateSurvivesDecoderCompletion()
			{
				QFETCH(int, ordering);
				QFETCH(int, transition);
				SoundEngine engine;
				engine.initializePlayback();
				const auto session = engine.registerSession({});
				QVERIFY(session);
				QVERIFY(SoundEngineTestAccess::decoderContinuation(engine, session, ordering, transition));
			}

			/** @brief Supplies representative integer and fractional gains for real direct and reserved playback. */
			void globalVolumeNoOpPreservesStopEnvelope_data()
			{
				QTest::addColumn<double>("percent");
				QTest::addColumn<bool>("reserved");
				for (const double value : {34.0, 34.25})
					for (const bool delayed : {false, true})
						QTest::newRow(
						    qPrintable(QString::number(value) + (delayed ? "-reserved" : "-direct")))
						    << value << delayed;
			}

			/** @brief Verifies a global volume no-op preserves a pending stop on the real audio transport. */
			void globalVolumeNoOpPreservesStopEnvelope()
			{
				QFETCH(double, percent);
				QFETCH(bool, reserved);
				SoundEngine engine;
				configureTestSoundCatalog(engine);
				const auto session = engine.registerSession({});
				QVERIFY(session);
				if (reserved)
				{
					QCOMPARE(session->reserveLuaAudioChannel(testSoundFileName(), 91, 100.0), 1);
					QVERIFY(session->confirmLuaAudioOwnership(1, 91));
					QCOMPARE(session->setLuaAudioAttribute(1, 91, QMudSoundAttribute::Volume, percent), eOK);
				}
				QCOMPARE(session->playFile(1, testSoundFileName(), true, percent - 100.0, 0.0, 91, nullptr,
				                           reserved),
				         eOK);
				QVERIFY(session->confirmLuaAudioOwnership(1, 91));
				// Flat gain makes the no-op condition independent of audio callback timing.
				QCOMPARE(session->setLuaAudioAttribute(1, 91, QMudSoundAttribute::Volume, percent,
				                                       std::chrono::milliseconds{100}, true),
				         eOK);
				QCOMPARE(session->setLuaAudioAttribute(0, 0, QMudSoundAttribute::Volume, percent), eOK);
				QTRY_COMPARE(session->status(1), -2);
			}

			/** @brief Verifies sample-clock controls, published reads, cancellation, and backend reuse share one generation. */
			void luaAudioControlsPublishAndCancelAcrossAssignments()
			{
				std::atomic<quint64>          released{0};
				SoundEngine::SessionCallbacks callbacks;
				callbacks.luaAudioOwnershipReleased = [&released](int, const quint64 generation)
				{ released.store(generation, std::memory_order_release); };
				SoundEngine engine;
				configureTestSoundCatalog(engine);
				const auto session = engine.registerSession(callbacks);
				QVERIFY(session);
				QCOMPARE(session->playFile(1, testSoundFileName(), true, 0.0, 0.0, 11), eOK);
				QVERIFY(session->confirmLuaAudioOwnership(1, 11));
				const auto original = session->snapshot();
				QCOMPARE(original->volume[0], 100.0);
				QCOMPARE(session->setLuaAudioAttribute(1, 11, QMudSoundAttribute::Volume, 50.0), eOK);
				QCOMPARE(session->snapshot()->volume[0], 50.0);
				QCOMPARE(original->volume[0], 100.0);
				QCOMPARE(session->setLuaAudioAttribute(1, 11, QMudSoundAttribute::Pan, 100.0), eOK);
				QCOMPARE(session->setLuaAudioAttribute(1, 11, QMudSoundAttribute::Pitch, 4.0), eOK);
				QCOMPARE(session->setLuaAudioAttribute(1, 11, QMudSoundAttribute::Pitch, 3.0, {}, false,
				                                       QMudSoundAttributeTarget::Relative),
				         eOK);
				QCOMPARE(SoundEngineTestAccess::activeAttribute(engine, session, 1, QMudSoundAttribute::Pan),
				         100.0);
				QCOMPARE(
				    SoundEngineTestAccess::activeAttribute(engine, session, 1, QMudSoundAttribute::Pitch),
				    7.0);
				const quint64 firstClock = SoundEngineTestAccess::controlPcmClock(engine, session, 1);
				QVERIFY(firstClock != 0);
				QCOMPARE(session->setLuaAudioAttribute(1, 11, QMudSoundAttribute::Volume, 0.0,
				                                       std::chrono::milliseconds{1000}),
				         eOK);
				QVERIFY(SoundEngineTestAccess::advancePcmClock(engine, session, 1, firstClock,
				                                               std::chrono::milliseconds{1000}));
				QCOMPARE(session->snapshot()->volume[0], 0.0);
				QCOMPARE(session->status(1), 2);
				QCOMPARE(session->stopLuaAudioChannel(1, 11), eOK);
				QCOMPARE(released.load(std::memory_order_acquire), 11);
				const auto backendCount = SoundEngineTestAccess::cacheState(engine).backendCount;
				QCOMPARE(session->playFile(1, testSoundFileName(), true, 0.0, 0.0, 22), eOK);
				QVERIFY(session->confirmLuaAudioOwnership(1, 22));
				QVERIFY(!SoundEngineTestAccess::advancePcmClock(engine, session, 1, firstClock,
				                                                std::chrono::milliseconds{1000}));
				QCOMPARE(SoundEngineTestAccess::cacheState(engine).backendCount, backendCount);
				QCOMPARE(SoundEngineTestAccess::activeAttribute(engine, session, 1, QMudSoundAttribute::Pan),
				         0.0);
				QCOMPARE(
				    SoundEngineTestAccess::activeAttribute(engine, session, 1, QMudSoundAttribute::Pitch),
				    0.0);
				QCOMPARE(session->setLuaAudioAttribute(1, 11, QMudSoundAttribute::Volume, 10.0),
				         eCannotPlaySound);
				QCOMPARE(session->snapshot()->volume[0], 100.0);
				const quint64 secondClock = SoundEngineTestAccess::controlPcmClock(engine, session, 1);
				QVERIFY(secondClock != 0);
				QCOMPARE(session->setLuaAudioAttribute(1, 22, QMudSoundAttribute::Volume, 0.0,
				                                       std::chrono::milliseconds{1000}, true),
				         eOK);
				QVERIFY(SoundEngineTestAccess::advancePcmClock(engine, session, 1, secondClock,
				                                               std::chrono::milliseconds{1000}));
				QCOMPARE(session->status(1), -2);
				QCOMPARE(released.load(std::memory_order_acquire), 22);
				const int reserved =
				    session->reserveLuaAudioChannel(testSoundFileName(), 33, 70.0, 20.0, 2.0);
				QVERIFY(reserved > 0);
				QVERIFY(session->confirmLuaAudioOwnership(reserved, 33));
				QCOMPARE(session->setLuaAudioAttribute(reserved, 33, QMudSoundAttribute::Volume, 35.0), eOK);
				QCOMPARE(session->setLuaAudioAttribute(reserved, 33, QMudSoundAttribute::Pan, -50.0), eOK);
				QCOMPARE(session->setLuaAudioAttribute(reserved, 33, QMudSoundAttribute::Pitch, 6.0), eOK);
				QCOMPARE(session->setLuaAudioAttribute(reserved, 33, QMudSoundAttribute::Pitch, 2.0, {},
				                                       false, QMudSoundAttributeTarget::Relative),
				         eOK);
				int selected = 0;
				QCOMPARE(session->playFile(reserved, testSoundFileName(), true, -30.0, 20.0, 33, &selected,
				                           true, 2.0),
				         eOK);
				QCOMPARE(selected, reserved);
				QCOMPARE(SoundEngineTestAccess::activeAttribute(engine, session, selected,
				                                                QMudSoundAttribute::Volume),
				         35.0);
				QCOMPARE(SoundEngineTestAccess::activeAttribute(engine, session, selected,
				                                                QMudSoundAttribute::Pan),
				         -50.0);
				QCOMPARE(SoundEngineTestAccess::activeAttribute(engine, session, selected,
				                                                QMudSoundAttribute::Pitch),
				         8.0);
				QCOMPARE(
				    session->setLuaAudioAttribute(selected, 33, QMudSoundAttribute::Volume, 0.0, {}, true),
				    eOK);
				QCOMPARE(session->status(selected), -2);
				QCOMPARE(released.load(std::memory_order_acquire), 33);
				QCOMPARE(session->playFile(1, testSoundFileName(), true, 0.0, 0.0, 44), eOK);
				QCOMPARE(session->playFile(2, testSoundFileName(), true, 0.0, 0.0), eOK);
				QCOMPARE(session->setLuaAudioAttribute(0, 0, QMudSoundAttribute::Volume, 25.0), eOK);
				QCOMPARE(session->snapshot()->volume[0], 25.0);
				QCOMPARE(session->snapshot()->volume[1], 100.0);
				QCOMPARE(session->setLuaAudioInitialized(false), eOK);
				QCOMPARE(session->status(1), -2);
				QCOMPARE(session->status(2), 2);
				QVERIFY(session->confirmLuaAudioOwnership(1, 44));
				QCOMPARE(released.load(std::memory_order_acquire), 44);
				QCOMPARE(session->playFile(1, testSoundFileName(), true, 0.0, 0.0, 55), eCannotPlaySound);
				QCOMPARE(session->reserveLuaAudioChannel(testSoundFileName(), 55), 0);
				QCOMPARE(session->setLuaAudioInitialized(true), eOK);
				QCOMPARE(session->playFile(1, testSoundFileName(), true, 0.0, 0.0, 55), eOK);
				QVERIFY(session->confirmLuaAudioOwnership(1, 55));
				session->close();
				QCOMPARE(session->setLuaAudioAttribute(1, 22, QMudSoundAttribute::Pan, 25.0),
				         eCannotPlaySound);
			}

			/** @brief Verifies all file entry paths share fallback while warm reuse yields only to an exact match. */
			void sharedFallbackPreservesWarmReuseAndPreparedIdentity()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-fallback-XXXXXX")));
				QVERIFY(root.isValid());
				const QString sourceDirectory =
				    QDir(root.path()).filePath(QStringLiteral("sounds/TestWorld/notifications"));
				const QString collisionDirectory =
				    QDir(root.path()).filePath(QStringLiteral("sounds/Other/combat"));
				QVERIFY(QDir().mkpath(sourceDirectory));
				QVERIFY(QDir().mkpath(collisionDirectory));
				const QString source = QDir(sourceDirectory).filePath(QStringLiteral("crash.wav"));
				QVERIFY(QFile::copy(testSoundFileName(), source));
				QVERIFY(QFile::copy(testSoundFileName(),
				                    QDir(collisionDirectory).filePath(QStringLiteral("crash.wav"))));
				SoundEngine engine;
				engine.initializePlayback();
				engine.configureSoundDirectory(root.path());
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				const auto session = engine.registerSession({}, QStringLiteral("./sounds/TestWorld/"));
				QVERIFY(session);
				const QString request = QStringLiteral(R"(worlds\sounds\notifications\crash.wav)");
				int           buffer  = 0;
				QCOMPARE(session->playFile(0, request, false, 0.0, 0.0, 0, &buffer), eOK);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, buffer), source);
				const quint64   slow     = SoundEngineTestAccess::slowFileResolutionCount(engine);
				const qsizetype backends = SoundEngineTestAccess::cacheState(engine).backendCount;
				QCOMPARE(session->playFile(buffer, request, false, 0.0, 0.0), eOK);
				QCOMPARE(SoundEngineTestAccess::slowFileResolutionCount(engine), slow);
				QCOMPARE(SoundEngineTestAccess::cacheState(engine).backendCount, backends);
				QCOMPARE(session->stop(buffer), eOK);
				const int reserved = session->reserveLuaAudioChannel(request, 11);
				QVERIFY(reserved > 0);
				QCOMPARE(session->stopLuaAudioChannel(reserved, 11), eOK);
				const quint64 prepared = session->prepareFile(request);
				QVERIFY(prepared != 0);
				QVERIFY(session->submitNativeSound(nativeSoundRequest(request)));
				const int nativeBuffer =
				    SoundEngineTestAccess::nativePlaybackState(engine, session, QMudNativeSoundProtocol::Msp,
				                                               QMudNativeSoundMedia::Sound)
				        .buffer;
				QVERIFY(nativeBuffer > 0);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, nativeBuffer), source);
				QCOMPARE(session->stop(nativeBuffer), eOK);
				const QString otherDirectory =
				    QDir(root.path()).filePath(QStringLiteral("sounds/Other/notifications"));
				QVERIFY(QDir().mkpath(otherDirectory));
				QVERIFY(QFile::copy(testSoundFileName(),
				                    QDir(otherDirectory).filePath(QStringLiteral("crash.wav"))));
				SoundEngineTestAccess::reconcileSoundRoot(engine);
				QTRY_COMPARE(SoundEngineTestAccess::fileCatalogState(engine).fileCount, 3);
				const quint64 afterNative = SoundEngineTestAccess::slowFileResolutionCount(engine);
				QCOMPARE(session->playFile(0, request, false, 0.0, 0.0, 0, &buffer), eOK);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, buffer), source);
				QCOMPARE(SoundEngineTestAccess::slowFileResolutionCount(engine), afterNative);
				QCOMPARE(session->stop(buffer), eOK);
				const QString exactDirectory =
				    QDir(root.path()).filePath(QStringLiteral("sounds/worlds/sounds/notifications"));
				QVERIFY(QDir().mkpath(exactDirectory));
				const QString exact = QDir(exactDirectory).filePath(QStringLiteral("crash.wav"));
				QVERIFY(QFile::copy(testSoundFileName(), exact));
				SoundEngineTestAccess::reconcileSoundRoot(engine);
				QTRY_COMPARE(SoundEngineTestAccess::fileCatalogState(engine).fileCount, 4);
				QCOMPARE(session->playFile(0, request, false, 0.0, 0.0, 0, &buffer), eOK);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, buffer), source);
				QCOMPARE(session->stop(buffer), eOK);
				QCOMPARE(session->commitPrepared(prepared, 1, false, 0.0, 0.0), eOK);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, 1), source);
				QCOMPARE(session->stop(1), eOK);
				const QString preferredDirectory =
				    QDir(root.path())
				        .filePath(QStringLiteral("sounds/TestWorld/worlds/sounds/notifications"));
				QVERIFY(QDir().mkpath(preferredDirectory));
				QVERIFY(QFile::copy(testSoundFileName(),
				                    QDir(preferredDirectory).filePath(QStringLiteral("crash.wav"))));
				SoundEngineTestAccess::reconcileSoundRoot(engine);
				QTRY_COMPARE(SoundEngineTestAccess::fileCatalogState(engine).fileCount, 5);
				const quint64 exactSlow = SoundEngineTestAccess::slowFileResolutionCount(engine);
				QCOMPARE(session->playFile(0, request, false, 0.0, 0.0, 0, &buffer), eOK);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, buffer),
				         QDir(preferredDirectory).filePath(QStringLiteral("crash.wav")));
				const quint64 promotedSlow = SoundEngineTestAccess::slowFileResolutionCount(engine);
				QVERIFY(promotedSlow >= exactSlow);
				QCOMPARE(session->playFile(buffer, request, false, 0.0, 0.0), eOK);
				QCOMPARE(SoundEngineTestAccess::slowFileResolutionCount(engine), promotedSlow);
				QCOMPARE(session->stop(buffer), eOK);
			}

			/**
			 * @brief Verifies explicit plugin sounds use direct-directory watches and audio-only invalidation.
			 */
			void pluginBundledSoundsRemainExactAndTrackOnlyTheirImmediateDirectories()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("plugin-sound-routing-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("sounds")));
				const QString pluginsRoot = QDir(root.path()).filePath(QStringLiteral("custom/plugins"));
				const QString pluginDirectory =
				    QDir(pluginsRoot).filePath(QStringLiteral("PromptHelper/audio"));
				const QString nestedDirectory = QDir(pluginDirectory).filePath(QStringLiteral("nested"));
				QVERIFY(QDir().mkpath(nestedDirectory));
				const QString pluginSound = QDir(pluginDirectory).filePath(QStringLiteral("Prompt.WaV"));
				const QString nestedSound = QDir(nestedDirectory).filePath(QStringLiteral("Nested.WaV"));
				QVERIFY(QFile::copy(testSoundFileName(), pluginSound));
				QVERIFY(QFile::copy(testSoundFileName(), nestedSound));
				const QString replacementPluginsRoot =
				    QDir(root.path()).filePath(QStringLiteral("replacement/plugins"));
				const QString replacementDirectory =
				    QDir(replacementPluginsRoot).filePath(QStringLiteral("PromptHelper/audio"));
				QVERIFY(QDir().mkpath(replacementDirectory));
				const QString replacementSound =
				    QDir(replacementDirectory).filePath(QStringLiteral("Prompt.WaV"));
				QVERIFY(QFile::copy(testSoundFileName(), replacementSound));
				QFile nonAudio(QDir(pluginDirectory).filePath(QStringLiteral("settings.xml")));
				QVERIFY(nonAudio.open(QIODevice::WriteOnly));
				QCOMPARE(nonAudio.write("initial"), 7);
				nonAudio.close();

				SoundEngine engine;
				engine.initializePlayback();
				engine.configureSoundDirectory(root.path());
				engine.configurePluginDirectory(pluginsRoot);
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
				const qsizetype baselineWatches =
				    SoundEngineTestAccess::fileCatalogState(engine).watchedDirectoryCount;
				const auto session = engine.registerSession({});
				QVERIFY(session);

				int selectedBuffer = 0;
				QCOMPARE(session->playFile(0, pluginSound, false, 0.0, 0.0, 0, &selectedBuffer), eOK);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, selectedBuffer),
				         pluginSound);
				QCOMPARE(session->stop(selectedBuffer), eOK);
				QTRY_COMPARE(SoundEngineTestAccess::fileCatalogState(engine).watchedDirectoryCount,
				             baselineWatches + 1);

				const QString portableRequest =
				    QStringLiteral(R"(CUSTOM\PLUGINS\prompthelper\AUDIO\prompt.wav)");
				QCOMPARE(session->playFile(0, portableRequest, false, 0.0, 0.0, 0, &selectedBuffer), eOK);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, selectedBuffer),
				         pluginSound);
				QCOMPARE(session->stop(selectedBuffer), eOK);
				const quint64 cachedResolutionCount = SoundEngineTestAccess::slowFileResolutionCount(engine);
				QCOMPARE(session->playFile(0, portableRequest, false, 0.0, 0.0, 0, &selectedBuffer), eOK);
				QCOMPARE(SoundEngineTestAccess::slowFileResolutionCount(engine), cachedResolutionCount);
				QCOMPARE(session->stop(selectedBuffer), eOK);
				QCOMPARE(session->playFile(0, QStringLiteral("prompt.wav"), false, 0.0, 0.0), eFileNotFound);
				QVERIFY(session->submitNativeSound(nativeSoundRequest(portableRequest)));
				QCOMPARE(session->stop(0), eOK);
				QVERIFY(!SoundEngineTestAccess::nativePlaybackState(
				             engine, session, QMudNativeSoundProtocol::Msp, QMudNativeSoundMedia::Sound)
				             .current);
				const quint64 postMissResolutionCount =
				    SoundEngineTestAccess::slowFileResolutionCount(engine);

				QVERIFY(nonAudio.open(QIODevice::Append));
				QCOMPARE(nonAudio.write("changed"), 7);
				nonAudio.close();
				SoundEngineTestAccess::reconcileCatalogDirectory(engine, pluginDirectory);
				QCOMPARE(session->playFile(0, portableRequest, false, 0.0, 0.0, 0, &selectedBuffer), eOK);
				QCOMPARE(SoundEngineTestAccess::slowFileResolutionCount(engine), postMissResolutionCount);
				QCOMPARE(session->stop(selectedBuffer), eOK);

				QFile changedSound(pluginSound);
				QVERIFY(changedSound.open(QIODevice::Append));
				QCOMPARE(changedSound.write("x"), 1);
				changedSound.close();
				SoundEngineTestAccess::reconcileCatalogDirectory(engine, pluginDirectory);
				QCOMPARE(session->playFile(0, portableRequest, false, 0.0, 0.0, 0, &selectedBuffer), eOK);
				QCOMPARE(SoundEngineTestAccess::slowFileResolutionCount(engine), postMissResolutionCount + 1);
				QCOMPARE(session->stop(selectedBuffer), eOK);

				const QString nestedRequest =
				    QStringLiteral("custom/plugins/PromptHelper/audio/nested/Nested.WaV");
				QCOMPARE(session->playFile(0, nestedRequest, false, 0.0, 0.0, 0, &selectedBuffer), eOK);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, selectedBuffer),
				         nestedSound);
				QCOMPARE(session->stop(selectedBuffer), eOK);
				QTRY_COMPARE(SoundEngineTestAccess::fileCatalogState(engine).watchedDirectoryCount,
				             baselineWatches + 2);

				QVERIFY(QFile::remove(nestedSound));
				QVERIFY(QDir().rmdir(nestedDirectory));
				SoundEngineTestAccess::reconcileCatalogDirectory(engine, nestedDirectory);
				QTRY_COMPARE(SoundEngineTestAccess::fileCatalogState(engine).watchedDirectoryCount,
				             baselineWatches + 1);
				QCOMPARE(session->playFile(0, nestedRequest, false, 0.0, 0.0), eFileNotFound);

				engine.configurePluginDirectory(replacementPluginsRoot);
				QCOMPARE(session->playFile(0, portableRequest, false, 0.0, 0.0), eFileNotFound);
				const QString replacementRequest =
				    QStringLiteral(R"(REPLACEMENT\PLUGINS\prompthelper\AUDIO\prompt.wav)");
				QCOMPARE(session->playFile(0, replacementRequest, false, 0.0, 0.0, 0, &selectedBuffer), eOK);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, selectedBuffer),
				         replacementSound);
				QCOMPARE(session->stop(selectedBuffer), eOK);
				QTRY_COMPARE(SoundEngineTestAccess::fileCatalogState(engine).watchedDirectoryCount,
				             baselineWatches + 1);
			}

			/** @brief Verifies loading a world applies its persisted sound directory to an attached session. */
			void persistedWorldSoundDirectoryAppliesDuringDocumentLoad()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-world-load-XXXXXX")));
				QVERIFY(root.isValid());
				const QString sharedDirectory = QDir(root.path()).filePath(QStringLiteral("sounds/Test"));
				const QString worldDirectory =
				    QDir(root.path()).filePath(QStringLiteral("sounds/World-Two/Test"));
				QVERIFY(QDir().mkpath(sharedDirectory));
				QVERIFY(QDir().mkpath(worldDirectory));
				QVERIFY(QFile::copy(testSoundFileName(),
				                    QDir(sharedDirectory).filePath(QStringLiteral("File.wav"))));
				const QString worldFile = QDir(worldDirectory).filePath(QStringLiteral("File.wav"));
				QVERIFY(QFile::copy(testSoundFileName(), worldFile));

				SoundEngine engine;
				engine.initializePlayback();
				engine.configureSoundDirectory(root.path());
				QTRY_COMPARE(SoundEngineTestAccess::fileCatalogState(engine).fileCount, 2);

				WorldRuntime runtime;
				runtime.setStartupDirectory(root.path());
				WorldDocument document;
				QVERIFY(document.loadFromXml(QStringLiteral(
				    R"xml(<qmud><world default_world_sound_directory="./sounds/world-two/"/></qmud>)xml")));
				runtime.applyFromDocument(document);
				runtime.attachSoundEngine(&engine);

				const std::shared_ptr<SoundEngineSession> session =
				    WorldRuntimeTestAccess::soundSession(runtime);
				QVERIFY(session);
				int selectedBuffer = 0;
				QCOMPARE(session->playFile(0, QStringLiteral("test/file.wav"), false, 0.0, 0.0, 0,
				                           &selectedBuffer),
				         eOK);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, selectedBuffer),
				         worldFile);
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
				engine.configureSoundDirectory(soundRoot.path());
				const auto session = engine.registerSession({});
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
				const auto  session = registerTestSession(engine);
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
				const auto session      = registerTestSession(engine, std::move(callbacks));
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
			 * @brief Verifies playback without progress is failed, retired, and cannot enter a native repeat.
			 */
			void playbackNoProgressWatchdogRetiresBackendAndStopsNativeRepeat()
			{
				QStringList                   errors;
				SoundEngine                   engine;
				SoundEngine::SessionCallbacks callbacks;
				callbacks.playbackError = [&errors](const QString &message) { errors.push_back(message); };
				configureTestSoundCatalog(engine);
				const auto session = registerTestSession(engine, std::move(callbacks));
				QVERIFY(session);

				QMudNativeSoundRequest request = nativeSoundRequest(QStringLiteral("QMud/beep.wav"));
				request.repeatCount            = 2;
				QVERIFY(session->submitNativeSound(request));
				const int buffer =
				    SoundEngineTestAccess::nativePlaybackState(engine, session, QMudNativeSoundProtocol::Msp,
				                                               QMudNativeSoundMedia::Sound)
				        .buffer;
				QVERIFY(buffer > 0);
				QCOMPARE(session->status(buffer), 1);
				QCOMPARE(SoundEngineTestAccess::cacheState(engine).backendCount, 1);

				QVERIFY(SoundEngineTestAccess::expireActivePlaybackWatchdog(engine, session, buffer));
				QCOMPARE(session->status(buffer), -2);
				QVERIFY(SoundEngineTestAccess::activeFileIdentity(engine, session, buffer).isEmpty());
				QVERIFY(!SoundEngineTestAccess::nativePlaybackState(
				             engine, session, QMudNativeSoundProtocol::Msp, QMudNativeSoundMedia::Sound)
				             .current);
				QTRY_COMPARE(errors.size(), 1);
				QVERIFY(errors.constFirst().contains(QStringLiteral("5 seconds without progress")));
				QTRY_COMPARE(SoundEngineTestAccess::cacheState(engine).backendCount, 0);

				int replacementBuffer = 0;
				QCOMPARE(session->playFile(0, testSoundFileName(), false, 0.0, 0.0, 0, &replacementBuffer),
				         eOK);
				QVERIFY(replacementBuffer > 0);
				QCOMPARE(session->status(replacementBuffer), 1);
				QCOMPARE(SoundEngineTestAccess::cacheState(engine).backendCount, 1);
				QVERIFY(SoundEngineTestAccess::completeActiveChannel(engine, session, replacementBuffer));
			}

			/**
			 * @brief Verifies completed backends are reused before idle cache entries or new allocations.
			 */
			void completedBackendIsReusedAcrossPreparedAndDirectPlayback()
			{
				SoundEngine engine;
				configureTestSoundCatalog(engine);
				const auto session = registerTestSession(engine);
				QVERIFY(session);

				int firstBuffer = 0;
				QCOMPARE(session->playFile(0, testSoundFileName(), false, 0.0, 0.0, 0, &firstBuffer), eOK);
				QVERIFY(firstBuffer > 0);
				QCOMPARE(SoundEngineTestAccess::cacheState(engine).backendCount, 1);
				QVERIFY(SoundEngineTestAccess::completeActiveChannel(engine, session, firstBuffer));
				QCOMPARE(session->status(firstBuffer), 0);

				const quint64 canceledToken = session->prepareFile(testSoundFileName());
				QVERIFY(canceledToken != 0);
				session->cancelPrepared(canceledToken);
				const quint64 committedToken = session->prepareFile(testSoundFileName());
				QVERIFY(committedToken != 0);
				QCOMPARE(session->commitPrepared(committedToken, 0, false, 0.0, 0.0), eOK);
				QCOMPARE(SoundEngineTestAccess::cacheState(engine).backendCount, 1);

				int preparedBuffer = 0;
				for (int buffer = 1; buffer <= SoundEngineSession::kChannelCount; ++buffer)
				{
					if (session->status(buffer) == 1)
					{
						preparedBuffer = buffer;
						break;
					}
				}
				QVERIFY(preparedBuffer > 0);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, preparedBuffer),
				         QFileInfo(testSoundFileName()).canonicalFilePath());
				QVERIFY(SoundEngineTestAccess::completeActiveChannel(engine, session, preparedBuffer));

				int directBuffer = 0;
				QCOMPARE(session->playFile(0, testSoundFileName(), false, 0.0, 0.0, 0, &directBuffer), eOK);
				QVERIFY(directBuffer > 0);
				QCOMPARE(SoundEngineTestAccess::cacheState(engine).backendCount, 1);
				QCOMPARE(SoundEngineTestAccess::activeFileIdentity(engine, session, directBuffer),
				         QFileInfo(testSoundFileName()).canonicalFilePath());

				for (int repetition = 0; repetition < 12; ++repetition)
				{
					QVERIFY(SoundEngineTestAccess::completeActiveChannel(engine, session, directBuffer));
					int nextBuffer = 0;
					QCOMPARE(session->playFile(0, testSoundFileName(), false, 0.0, 0.0, 0, &nextBuffer), eOK);
					QVERIFY(nextBuffer > 0);
					QCOMPARE(SoundEngineTestAccess::cacheState(engine).backendCount, 1);
					directBuffer = nextBuffer;
				}
			}

			/**
			 * @brief Verifies delayed LuaAudio reservations exclude automatic selection and require their generation.
			 */
			void luaAudioReservationIsWorkerAuthoritative()
			{
				SoundEngine engine;
				const auto  session = registerTestSession(engine);
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
				const auto  first  = registerTestSession(engine);
				const auto  second = registerTestSession(engine);
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
					session = registerTestSession(engine);
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

			/** @brief Supplies preparation phases and resident-resource reuse for pending control updates. */
			void pendingMemoryControls_data()
			{
				QTest::addColumn<int>("phase");
				QTest::addColumn<bool>("loop");
				for (int phase = 0; phase < 3; ++phase)
					for (const bool loop : {false, true})
						QTest::newRow(qPrintable(QStringLiteral("phase-%1-loop-%2").arg(phase).arg(loop)))
						    << phase << loop;
			}

			/** @brief Verifies pending updates survive hashing, materialization, reuse, and exact result publication. */
			void pendingMemoryControls()
			{
				QFETCH(int, phase);
				QFETCH(bool, loop);
				SoundEngine engine;
				const auto  session = registerTestSession(engine);
				QVERIFY(session);
				const auto payload = QMudTestSoundData::silentPcmWave(300);
				if (phase == 2)
				{
					QCOMPARE(session->playMemory(1, payload, true, 0.0, 0.0), eOK);
					QCOMPARE(session->stop(1), eOK);
				}
				MemoryPreparationBlocker digest(engine);
				QVERIFY(digest.waitUntilEntered());
				std::shared_ptr<const SoundEngineSession::Snapshot> started;
				int                                                 selected = 0;
				PendingSoundCall                                    call(
				    session, [&]
				    { return session->playMemory(1, payload, !loop, -80.0, 10.0, 0, &selected, &started); });
				QTRY_COMPARE(session->status(1), 0);
				QCOMPARE(session->snapshot()->volume[0], 20.0);
				std::unique_ptr<MemoryPreparationBlocker> materialization;
				if (phase == 1)
				{
					materialization = std::make_unique<MemoryPreparationBlocker>(engine);
					digest.release();
					QVERIFY(materialization->waitUntilEntered());
				}
				QCOMPARE(session->playFile(1, {}, !loop, -70.0, -25.0), eOK);
				std::shared_ptr<const SoundEngineSession::Snapshot> updated;
				QCOMPARE(session->playFile(1, {}, loop, -58.875, 35.0, 0, nullptr, false, 0.0, &updated),
				         eOK);
				QVERIFY(updated);
				QCOMPARE(updated->status[0], 0);
				QCOMPARE(updated->volume[0], 41.125);
				QCOMPARE(session->playFile(1, {}, !loop, std::numeric_limits<double>::quiet_NaN(), 0.0),
				         eBadParameter);
				QVERIFY(!call.finished.load(std::memory_order_acquire));
				digest.release();
				if (materialization)
					materialization->release();
				QTRY_VERIFY(call.finished.load(std::memory_order_acquire));
				QCOMPARE(call.result, eOK);
				QCOMPARE(selected, 1);
				QVERIFY(started);
				QCOMPARE(started->status[0], loop ? 2 : 1);
				QCOMPARE(started->volume[0], 41.125);
				QCOMPARE(SoundEngineTestAccess::activeAttribute(engine, session, 1, QMudSoundAttribute::Pan),
				         35.0);
				QCOMPARE(SoundEngineTestAccess::cacheState(engine).backendCount, 1);
				QCOMPARE(session->stop(1), eOK);
				QCOMPARE(updated->volume[0], 41.125);
				QCOMPARE(started->volume[0], 41.125);
			}

			/** @brief Supplies both preparation boundaries and cancellation/replacement outcomes. */
			void pendingMemoryCancellation_data()
			{
				QTest::addColumn<int>("phase");
				QTest::addColumn<int>("transition");
				for (int phase = 0; phase < 2; ++phase)
					for (int transition = 0; transition < 5; ++transition)
						QTest::newRow(
						    qPrintable(QStringLiteral("phase-%1-transition-%2").arg(phase).arg(transition)))
						    << phase << transition;
			}

			/** @brief Verifies updated controls cannot revive a canceled assignment or affect its replacement. */
			void pendingMemoryCancellation()
			{
				QFETCH(int, phase);
				QFETCH(int, transition);
				SoundEngine engine;
				const auto  session = registerTestSession(engine);
				QVERIFY(session);
				MemoryPreparationBlocker digest(engine);
				QVERIFY(digest.waitUntilEntered());
				PendingSoundCall call(session,
				                      [&]
				                      {
					                      return session->playMemory(1, QMudTestSoundData::silentPcmWave(300),
					                                                 false, -80.0, 0.0);
				                      });
				QTRY_COMPARE(session->status(1), 0);
				std::unique_ptr<MemoryPreparationBlocker> materialization;
				if (phase == 1)
				{
					materialization = std::make_unique<MemoryPreparationBlocker>(engine);
					digest.release();
					QVERIFY(materialization->waitUntilEntered());
				}
				QCOMPARE(session->playFile(1, {}, true, -60.0, 35.0), eOK);
				if (transition == 0)
					QCOMPARE(session->stop(1), eOK);
				else if (transition == 1)
					QCOMPARE(session->playFile(1, testSoundFileName(), true, -25.0, -10.0), eOK);
				else if (transition == 2)
					session->close();
				else if (transition == 3)
					SoundEngineTestAccess::resetForAudioDeviceChange(engine);
				else
					QCOMPARE(session->playFile(1, QStringLiteral("missing-memory-replacement.wav"), true, 0.0,
					                           0.0),
					         eFileNotFound);
				if (transition != 4)
				{
					QTRY_VERIFY(call.finished.load(std::memory_order_acquire));
					QCOMPARE(call.result, eCannotPlaySound);
				}
				digest.release();
				if (materialization)
					materialization->release();
				MemoryPreparationBlocker drained(engine);
				QVERIFY(drained.waitUntilEntered());
				drained.release();
				QTRY_VERIFY(call.finished.load(std::memory_order_acquire));
				if (transition == 4)
				{
					QCOMPARE(call.result, eOK);
					QCOMPARE(session->status(1), 2);
					QCOMPARE(session->snapshot()->volume[0], 40.0);
					QCOMPARE(
					    SoundEngineTestAccess::activeAttribute(engine, session, 1, QMudSoundAttribute::Pan),
					    35.0);
				}
				else if (transition == 1)
				{
					QCOMPARE(session->status(1), 2);
					QCOMPARE(session->snapshot()->volume[0], 75.0);
					QCOMPARE(
					    SoundEngineTestAccess::activeAttribute(engine, session, 1, QMudSoundAttribute::Pan),
					    -10.0);
				}
				else if (transition != 2)
					QCOMPARE(session->status(1), -2);
			}

			/** @brief Supplies direct/prepared replacements, both completion orders, misses, and cancellation. */
			void pendingMemoryAndFileReplacement_data()
			{
				QTest::addColumn<bool>("memoryFirst");
				QTest::addColumn<int>("replacement");
				for (const bool memoryFirst : {false, true})
					for (int replacement = 0; replacement < 5; ++replacement)
						QTest::newRow(qPrintable(QStringLiteral("memory-first-%1-replacement-%2")
						                             .arg(memoryFirst)
						                             .arg(replacement)))
						    << memoryFirst << replacement;
			}

			/** @brief Verifies one update reaches both pending occurrences and preserves replacement ordering. */
			void pendingMemoryAndFileReplacement()
			{
				QFETCH(bool, memoryFirst);
				QFETCH(int, replacement);
				QTemporaryDir root(
				    QDir::current().filePath(QStringLiteral("sound-memory-replacement-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(
				    !soundFixture(root.path(), QStringLiteral("sounds/TestWorld/moved/next.wav")).isEmpty());
				SoundEngine engine;
				engine.initializePlayback();
				SoundEngineTestAccess::setCatalogPaused(engine, true);
				engine.configureSoundDirectory(root.path());
				const auto session = engine.registerSession({}, QStringLiteral("sounds/TestWorld"));
				QVERIFY(session);
				MemoryPreparationBlocker preparation(engine);
				QVERIFY(preparation.waitUntilEntered());
				PendingSoundCall memory(session,
				                        [&]
				                        {
					                        return session->playMemory(
					                            1, QMudTestSoundData::silentPcmWave(300), false, -80.0, 0.0);
				                        });
				QTRY_COMPARE(session->status(1), 0);
				const bool    prepared = replacement >= 2;
				const bool    missing  = replacement == 1 || replacement == 3;
				const QString request =
				    missing ? QStringLiteral("legacy/missing.wav") : QStringLiteral("legacy/next.wav");
				const quint64    token = prepared ? session->prepareFile(request) : 0;
				PendingSoundCall next(session,
				                      [&]
				                      {
					                      return prepared
					                                 ? session->commitPrepared(token, 1, false, -90.0, 0.0)
					                                 : session->playFile(1, request, false, -90.0, 0.0);
				                      });
				QTRY_COMPARE(SoundEngineTestAccess::pendingFileCount(engine, session), 1);
				if (prepared)
					QTRY_COMPARE(SoundEngineTestAccess::pendingFileCount(engine, session, true), 1);
				QCOMPARE(session->playFile(1, {}, true, -58.875, -35.0), eOK);
				if (replacement == 4)
					session->cancelPrepared(token);
				if (memoryFirst)
				{
					preparation.release();
					QTRY_VERIFY(memory.finished.load(std::memory_order_acquire));
					QCOMPARE(memory.result, eOK);
					QCOMPARE(session->status(1), 2);
					QCOMPARE(session->snapshot()->volume[0], 41.125);
					QCOMPARE(
					    SoundEngineTestAccess::activeAttribute(engine, session, 1, QMudSoundAttribute::Pan),
					    -35.0);
				}
				SoundEngineTestAccess::setCatalogPaused(engine, false);
				QTRY_VERIFY(next.finished.load(std::memory_order_acquire));
				QCOMPARE(next.result, replacement == 4 ? eCannotPlaySound : missing ? eFileNotFound : eOK);
				preparation.release();
				QTRY_VERIFY(memory.finished.load(std::memory_order_acquire));
				QCOMPARE(memory.result,
				         !memoryFirst && !missing && replacement != 4 ? eCannotPlaySound : eOK);
				QCOMPARE(session->status(1), 2);
				QCOMPARE(session->snapshot()->volume[0], 41.125);
				QCOMPARE(SoundEngineTestAccess::activeAttribute(engine, session, 1, QMudSoundAttribute::Pan),
				         -35.0);
			}

			/** @brief Verifies shared materialization retains per-channel controls and confirmed ownership. */
			void pendingMemorySharingPreservesIndependentControls()
			{
				SoundEngine                   engine;
				std::atomic<quint64>          released{0};
				SoundEngine::SessionCallbacks callbacks;
				callbacks.luaAudioOwnershipReleased = [&](const int, const quint64 generation)
				{ released.store(generation); };
				const auto first  = registerTestSession(engine, std::move(callbacks));
				const auto second = engine.registerSession({});
				QVERIFY(first && second);
				const auto               payload = QMudTestSoundData::silentPcmWave(300);
				MemoryPreparationBlocker preparation(engine);
				QVERIFY(preparation.waitUntilEntered());
				PendingSoundCall one(first,
				                     [&] { return first->playMemory(1, payload, true, -80.0, 0.0, 71); });
				PendingSoundCall two(second,
				                     [&] { return second->playMemory(1, payload, true, -20.0, 0.0); });
				QTRY_COMPARE(first->status(1), 0);
				QTRY_COMPARE(second->status(1), 0);
				QVERIFY(first->confirmLuaAudioOwnership(1, 71));
				QCOMPARE(first->playFile(1, {}, true, -60.0, 15.0, 72), eCannotPlaySound);
				QCOMPARE(first->playFile(1, {}, true, -60.0, 15.0, 71), eOK);
				QCOMPARE(first->setLuaAudioAttribute(1, 71, QMudSoundAttribute::Volume, 35.25), eOK);
				QCOMPARE(first->setLuaAudioAttribute(1, 71, QMudSoundAttribute::Pan, -33.0), eOK);
				QCOMPARE(first->setLuaAudioAttribute(1, 71, QMudSoundAttribute::Pitch, 8.0), eOK);
				QCOMPARE(second->playFile(1, {}, true, -30.0, 25.0), eOK);
				preparation.release();
				QTRY_VERIFY(one.finished.load(std::memory_order_acquire));
				QTRY_VERIFY(two.finished.load(std::memory_order_acquire));
				QCOMPARE(one.result, eOK);
				QCOMPARE(two.result, eOK);
				QCOMPARE(first->snapshot()->volume[0], 35.25);
				QCOMPARE(second->snapshot()->volume[0], 70.0);
				QCOMPARE(SoundEngineTestAccess::activeAttribute(engine, first, 1, QMudSoundAttribute::Pan),
				         -33.0);
				QCOMPARE(SoundEngineTestAccess::activeAttribute(engine, first, 1, QMudSoundAttribute::Pitch),
				         8.0);
				QCOMPARE(SoundEngineTestAccess::activeAttribute(engine, second, 1, QMudSoundAttribute::Pan),
				         25.0);
				QCOMPARE(SoundEngineTestAccess::cacheState(engine).sourceCount, 1);
				QCOMPARE(SoundEngineTestAccess::cacheState(engine).backendCount, 2);
				QCOMPARE(first->stopLuaAudioChannel(1, 71), eOK);
				QCOMPARE(released.load(), 71);
			}

			/** @brief Verifies a canceled prepared-memory resource never enters the reusable-resource index. */
			void canceledMemoryResourceIsNotIndexed()
			{
				SoundEngine engine;
				QVERIFY(!SoundEngineTestAccess::canceledMemoryResourceIsIndexed(engine));
			}

			/** @brief Verifies in-memory PCM plays to completion and reuses its materialized backend. */
			void memorySoundCompletesAndReusesItsBackend()
			{
				SoundEngine engine;
				engine.initializePlayback();
				QStringList                   errors;
				SoundEngine::SessionCallbacks callbacks;
				callbacks.playbackError   = [&errors](const QString &message) { errors.push_back(message); };
				const auto session        = registerTestSession(engine, std::move(callbacks));
				const auto sound          = QMudTestSoundData::silentPcmWave(250);
				int        selectedBuffer = 0;

				QCOMPARE(session->playMemory(0, sound, false, 0.0, 0.0, 0, &selectedBuffer), eOK);
				QVERIFY(selectedBuffer > 0);
				QCOMPARE(session->status(selectedBuffer), 1);
				QTRY_COMPARE(session->status(selectedBuffer), 0);
				QVERIFY(errors.isEmpty());
				QCOMPARE(SoundEngineTestAccess::cacheState(engine).backendCount, 1);

				int reusedBuffer = 0;
				QCOMPARE(session->playMemory(0, sound, false, 0.0, 0.0, 0, &reusedBuffer), eOK);
				QVERIFY(reusedBuffer > 0);
				QTRY_COMPARE(session->status(reusedBuffer), 0);
				QVERIFY(errors.isEmpty());
				QCOMPARE(SoundEngineTestAccess::cacheState(engine).backendCount, 1);
			}

			/**
			 * @brief Verifies status-preserving completion reuse does not publish an internal-state snapshot.
			 */
			void snapshotsRemainCoherentAndSkipInvisibleTransitions()
			{
				SoundEngine engine;
				const auto  session = registerTestSession(engine);
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
				const auto session = registerTestSession(engine, std::move(callbacks));
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
				const auto session = registerTestSession(engine, std::move(callbacks));
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
				const auto  session = registerTestSession(engine);
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
				const auto session = registerTestSession(engine, std::move(callbacks));
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
