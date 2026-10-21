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
#include <limits>
#include <memory>
#include <utility>

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
	 * @brief Configures the production sounds catalog around the repository fixture and waits for its lazy scan.
	 * @param engine Sound engine whose worker owns the catalog.
	 */
	void configureTestSoundCatalog(SoundEngine &engine)
	{
		engine.initializePlayback();
		engine.configureSoundDirectory(testSoundHome());
		QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(engine).ready);
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
				SoundEngine engine;
				QSignalSpy  initializedSpy(&engine, &SoundEngine::playbackInitializationFinished);
				engine.initializePlayback();
				const SoundEngineTestAccess::PlaybackInitializationState state =
				    SoundEngineTestAccess::playbackInitializationState(engine);
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

			/** @brief Verifies removing one directory subtree retains catalog state for sibling directories. */
			void nativeSoundCatalogRemovesOnlyDeletedSubtree()
			{
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
				QVERIFY(session->submitNativeSound(
				    nativeSoundRequest(QStringLiteral("removed/child/removed.wav"))));
				QVERIFY(!SoundEngineTestAccess::nativePlaybackState(
				             engine, session, QMudNativeSoundProtocol::Msp, QMudNativeSoundMedia::Sound)
				             .current);
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
				QTRY_COMPARE(SoundEngineTestAccess::activeFileIdentity(restored, session, playback.buffer),
				             QFileInfo(indexed).canonicalFilePath());
				QVERIFY(!SoundEngineTestAccess::fileCatalogState(restored).ready);
				SoundEngineTestAccess::setCatalogPaused(restored, false);
				QTRY_VERIFY(SoundEngineTestAccess::fileCatalogState(restored).ready);
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
