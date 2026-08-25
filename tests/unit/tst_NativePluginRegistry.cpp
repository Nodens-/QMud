/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_NativePluginRegistry.cpp
 * Role: Unit coverage for native compatibility plugin registry behavior.
 */

#include "NativePluginRegistry.h"
#include "WorldDocument.h"
#include "WorldRuntime.h"
#include "scripting/ScriptingErrors.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QCoreApplication>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
// ReSharper disable once CppUnusedIncludeDirective
#include <QUuid>
#include <QtTest/QTest>

#include <utility>

namespace
{
	/**
	 * @brief Mutable state exposed through the production runtime sound-test boundary.
	 */
	struct SoundBackendState
	{
			QHash<int, int>  statusByBuffer;
			QHash<int, bool> reusableByBuffer;
			QStringList      failingPlayFiles;
			int              playCalls{0};
			int              stopCalls{0};
			bool             playRequiresLuaAudioState{false};
			bool             playReleasesLuaAudioState{false};
			bool             sawLuaAudioStateDuringPlay{false};
	};

	/**
	 * @brief Installs a deterministic backend through the production runtime test seam.
	 * @param runtime Runtime receiving the backend.
	 * @param state Mutable backend state observed by the test.
	 */
	void installSoundBackend(WorldRuntime &runtime, SoundBackendState &state)
	{
		WorldRuntime::TestSoundBackend backend;
		backend.play =
		    [&runtime, &state](const int buffer, const QString &fileName, const bool loop, double, double)
		{
			++state.playCalls;
			if (fileName.isEmpty())
			{
				if (!state.statusByBuffer.contains(buffer))
					return eBadParameter;
				state.statusByBuffer.insert(buffer, loop ? 2 : 1);
				return eOK;
			}
			if (state.failingPlayFiles.contains(fileName))
				return eCannotPlaySound;
			const int                                            targetBuffer = buffer > 0 ? buffer : 1;
			QMudNativePluginRegistry::LuaAudioRuntimeBufferState bufferState;
			const bool                                           hasLuaAudioState =
			    QMudNativePluginRegistry::luaAudioRuntimeBufferState(&runtime, targetBuffer, bufferState);
			if (state.playRequiresLuaAudioState)
				state.sawLuaAudioStateDuringPlay = hasLuaAudioState;
			if (hasLuaAudioState && state.playReleasesLuaAudioState)
				QMudNativePluginRegistry::luaAudioReleaseRuntimeBufferIfGeneration(&runtime, targetBuffer,
				                                                                   bufferState.generation);
			state.statusByBuffer.insert(targetBuffer, loop ? 2 : 1);
			return eOK;
		};
		backend.stop = [&state](const int buffer)
		{
			++state.stopCalls;
			if (buffer == 0)
				state.statusByBuffer.clear();
			else
				state.statusByBuffer.remove(buffer);
			return eOK;
		};
		backend.status = [&state](const int buffer)
		{
			if (buffer < 1 || buffer > WorldRuntime::kMaxSoundBuffers)
				return -1;
			return state.statusByBuffer.value(buffer, -2);
		};
		backend.reusable = [&state](const int buffer)
		{
			if (buffer < 1 || buffer > WorldRuntime::kMaxSoundBuffers)
				return false;
			if (state.reusableByBuffer.contains(buffer))
				return state.reusableByBuffer.value(buffer);
			const int status = state.statusByBuffer.value(buffer, -2);
			return status == -2 || status == 0;
		};
		runtime.setSoundBackendForTest(std::move(backend));
	}

	/**
	 * @brief Creates the sound file expected by native LuaAudio path resolution.
	 * @param root Runtime startup directory.
	 * @return `true` when the fixture file was written.
	 */
	bool writeSoundFixture(const QString &root)
	{
		if (!QDir(root).mkpath(QStringLiteral("sounds")))
			return false;
		QFile soundFile(QDir(root).filePath(QStringLiteral("sounds/coin.wav")));
		if (!soundFile.open(QIODevice::WriteOnly))
			return false;
		return soundFile.write("RIFF") == 4;
	}

	/**
	 * @brief Captures production runtime output requests as plain lines.
	 * @param runtime Runtime whose output signal is observed.
	 * @param lines Destination line list.
	 */
	void captureOutputLines(const WorldRuntime &runtime, QStringList &lines)
	{
		QObject::connect(&runtime, &WorldRuntime::outputRequested, &runtime,
		                 [&lines](const QString &text, bool, bool) { lines.push_back(text); });
	}

	QMudNativePluginRegistry::NativeCallContext nativeCallContext(const WorldRuntime &runtime,
	                                                              const QString      &pluginId)
	{
		return runtime.nativePluginCallContext(pluginId);
	}

	QString writePluginXml(const QString &directory, const QString &id, const QString &script)
	{
		QString path = QDir(directory).filePath(QStringLiteral("%1.xml").arg(id));
		QFile   file(path);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
			return {};
		const QString xml = QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin name="Plugin" author="Tester" id="%1" language="lua" purpose="test">
    <description>test plugin</description>
    <script>%2</script>
  </plugin>
</muclient>
)xml")
		                        .arg(id, script);
		file.write(xml.toUtf8());
		file.close();
		return path;
	}
} // namespace

namespace
{
	/**
	 * @brief QTest fixture covering native plugin registry shim and blacklist rules.
	 */
	class tst_NativePluginRegistry : public QObject
	{
			Q_OBJECT

			// NOLINTBEGIN(readability-convert-member-functions-to-static)
		private slots:
			void initTestCase()
			{
				m_artifactRoot = QDir(QCoreApplication::applicationDirPath())
				                     .filePath(QStringLiteral("test-artifacts/tst_NativePluginRegistry/%1")
				                                   .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
				QVERIFY(QDir().mkpath(m_artifactRoot));
			}

			void init()
			{
				QMudNativePluginRegistry::setTestSpeechSink({});
			}

			void cleanup()
			{
				QMudNativePluginRegistry::setTestSpeechSink({});
			}

			void shimMetadataAndRoutineSurface()
			{
				const QString shimId      = QMudNativePluginRegistry::mushReaderPluginId();
				const QString audioShimId = QMudNativePluginRegistry::luaAudioPluginId();
				QVERIFY(QMudNativePluginRegistry::isShimId(shimId));
				QVERIFY(QMudNativePluginRegistry::isShimId(audioShimId));
				QVERIFY(QMudNativePluginRegistry::isProtectedId(shimId));
				QVERIFY(QMudNativePluginRegistry::isProtectedId(audioShimId));
				QCOMPARE(QMudNativePluginRegistry::resolveShimIdOrName(QStringLiteral("MushReader")), shimId);
				QCOMPARE(QMudNativePluginRegistry::resolveShimIdOrName(QStringLiteral("LuaAudio")),
				         audioShimId);

				QMudNativePluginRegistry::NativePluginMetadata metadata;
				QVERIFY(QMudNativePluginRegistry::metadataForShim(shimId, metadata));
				QCOMPARE(metadata.id, shimId);
				QCOMPARE(metadata.name, QStringLiteral("MushReader"));
				QCOMPARE(QMudNativePluginRegistry::pluginInfo(shimId, 1).toString(),
				         QStringLiteral("MushReader"));
				QCOMPARE(QMudNativePluginRegistry::pluginInfo(shimId, 17).toBool(), false);
				QCOMPARE(QMudNativePluginRegistry::pluginInfo(shimId, 17, 0, true).toBool(), true);
				QCOMPARE(QMudNativePluginRegistry::pluginInfo(shimId, 21, 7).toInt(), 7);

				QCOMPARE(QMudNativePluginRegistry::pluginSupports(shimId, QStringLiteral("say")), eOK);
				QCOMPARE(QMudNativePluginRegistry::pluginSupports(shimId, QStringLiteral("interrupt")), eOK);
				QCOMPARE(QMudNativePluginRegistry::pluginSupports(shimId, QStringLiteral("stop")), eOK);
				QCOMPARE(
				    QMudNativePluginRegistry::pluginSupports(shimId, QStringLiteral("plugin_update_url")),
				    eOK);
				QCOMPARE(QMudNativePluginRegistry::pluginSupports(shimId, QStringLiteral("missing")),
				         eNoSuchRoutine);

				QVERIFY(QMudNativePluginRegistry::metadataForShim(audioShimId, metadata));
				QCOMPARE(metadata.id, audioShimId);
				QCOMPARE(metadata.name, QStringLiteral("LuaAudio"));
				QCOMPARE(metadata.source, QStringLiteral("qmud:native/LuaAudio"));
				QCOMPARE(QMudNativePluginRegistry::pluginInfo(audioShimId, 1).toString(),
				         QStringLiteral("LuaAudio"));
				QCOMPARE(QMudNativePluginRegistry::pluginSupports(audioShimId, QStringLiteral("play")), eOK);
				QCOMPARE(QMudNativePluginRegistry::pluginSupports(audioShimId, QStringLiteral("setVol")),
				         eOK);
				QCOMPARE(QMudNativePluginRegistry::pluginSupports(audioShimId,
				                                                  QStringLiteral("plugin_update_url")),
				         eOK);

				QCOMPARE(
				    QMudNativePluginRegistry::normalizeNativeSource(QStringLiteral("qmud:native/MushReader")),
				    QStringLiteral("qmud:native/MushReader"));
				QCOMPARE(
				    QMudNativePluginRegistry::normalizeNativeSource(QStringLiteral("./qmud:native/LuaAudio")),
				    QStringLiteral("qmud:native/LuaAudio"));
				QCOMPARE(QMudNativePluginRegistry::normalizeNativeSource(
				             QStringLiteral(R"(qmud:native\MushReader)")),
				         QStringLiteral("qmud:native/MushReader"));
				QCOMPARE(QMudNativePluginRegistry::normalizeNativeSource(
				             QStringLiteral("worlds/plugins/qmud:native/MushReader")),
				         QStringLiteral("qmud:native/MushReader"));
				QCOMPARE(
				    QMudNativePluginRegistry::normalizeNativeSource(QStringLiteral("QMUD:NATIVE/mushreader")),
				    QStringLiteral("qmud:native/MushReader"));
				QCOMPARE(QMudNativePluginRegistry::normalizeNativeSource(QStringLiteral("MushReader.xml")),
				         QString());

				QMudNativePluginRegistry::NativePluginMetadata sourceMetadata;
				QVERIFY(QMudNativePluginRegistry::metadataForNativeSource(
				    QStringLiteral("worlds/plugins/qmud:native/MushReader"), sourceMetadata));
				QCOMPARE(sourceMetadata.id, shimId);
				QVERIFY(!QMudNativePluginRegistry::metadataForNativeSource(
				    QStringLiteral("qmud:native/UnknownShim"), sourceMetadata));
			}

			void callPluginRoutesToNativeSpeech() const
			{
				WorldRuntime runtime;
				runtime.setStartupDirectory(QCoreApplication::applicationDirPath());
				QVector<QMudNativePluginRegistry::TestSpeechEvent> events;
				QMudNativePluginRegistry::setTestSpeechSink(
				    [&events](const QMudNativePluginRegistry::TestSpeechEvent &event)
				    { events.push_back(event); });
				const auto restoreSpeechSink =
				    qScopeGuard([] { QMudNativePluginRegistry::setTestSpeechSink({}); });

				const QString shimId        = QMudNativePluginRegistry::mushReaderPluginId();
				const QString shimDirectory = testDirectory(QStringLiteral("call-plugin-mushreader"));
				QVERIFY(!shimDirectory.isEmpty());
				const QString shimPath = writePluginXml(shimDirectory, shimId, {});
				QVERIFY(!shimPath.isEmpty());
				QString shimError;
				QVERIFY2(runtime.loadPluginFile(shimPath, &shimError), qPrintable(shimError));
				const QMudNativePluginRegistry::NativeCallContext mushReaderContext =
				    nativeCallContext(runtime, shimId);
				QCOMPARE(QMudNativePluginRegistry::callRoutine(&runtime, shimId, QStringLiteral("say"),
				                                               {QStringLiteral("line")}, mushReaderContext)
				             .errorCode,
				         eOK);
				QCOMPARE(QMudNativePluginRegistry::callRoutine(&runtime, shimId, QStringLiteral("interrupt"),
				                                               {QStringLiteral("urgent")}, mushReaderContext)
				             .errorCode,
				         eOK);
				QCOMPARE(QMudNativePluginRegistry::callRoutine(&runtime, shimId, QStringLiteral("stop"), {},
				                                               mushReaderContext)
				             .errorCode,
				         eOK);
				const QMudNativePluginRegistry::NativeCallResult update =
				    QMudNativePluginRegistry::callRoutine(
				        &runtime, shimId, QStringLiteral("plugin_update_url"), {}, mushReaderContext);
				QCOMPARE(update.errorCode, eOK);
				QCOMPARE(update.returnValues.size(), 1);
				QCOMPARE(update.returnValues.constFirst().toString(),
				         QStringLiteral("qmud:native/MushReader"));
				const QMudNativePluginRegistry::NativeCallResult audioUpdate =
				    QMudNativePluginRegistry::callRoutine(
				        &runtime, QMudNativePluginRegistry::luaAudioPluginId(),
				        QStringLiteral("plugin_update_url"), {},
				        nativeCallContext(runtime, QMudNativePluginRegistry::luaAudioPluginId()));
				QCOMPARE(audioUpdate.errorCode, eOK);
				QCOMPARE(audioUpdate.returnValues.size(), 1);
				QCOMPARE(audioUpdate.returnValues.constFirst().toString(),
				         QStringLiteral("qmud:native/LuaAudio"));
				const QString soundRoot = testDirectory(QStringLiteral("call-plugin-routes"));
				QVERIFY(!soundRoot.isEmpty());
				QVERIFY(writeSoundFixture(soundRoot));
				runtime.setStartupDirectory(soundRoot);
				SoundBackendState soundState;
				installSoundBackend(runtime, soundState);
				const QMudNativePluginRegistry::NativeCallContext audioContext =
				    nativeCallContext(runtime, QMudNativePluginRegistry::luaAudioPluginId());
				QCOMPARE(QMudNativePluginRegistry::callRoutine(
				             &runtime, QMudNativePluginRegistry::luaAudioPluginId(), QStringLiteral("setVol"),
				             {55.0}, audioContext,
				             QMudNativePluginRegistry::NativeCallExecutionMode::ValidateOnly)
				             .errorCode,
				         eOK);
				QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeMasterState(&runtime).volume, 100.0);
				const QMudNativePluginRegistry::NativeCallResult validationPlay =
				    QMudNativePluginRegistry::callRoutine(
				        &runtime, QMudNativePluginRegistry::luaAudioPluginId(), QStringLiteral("play"),
				        {QStringLiteral("coin.wav"), false, 0.0, 100.0}, audioContext,
				        QMudNativePluginRegistry::NativeCallExecutionMode::ValidateOnly);
				QCOMPARE(validationPlay.errorCode, eOK);
				QCOMPARE(validationPlay.returnValues.size(), 1);
				QCOMPARE(validationPlay.returnValues.constFirst().toInt(), 0);
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
				const QMudNativePluginRegistry::NativeCallResult audioPlay =
				    QMudNativePluginRegistry::callRoutine(
				        &runtime, QMudNativePluginRegistry::luaAudioPluginId(), QStringLiteral("play"),
				        {QStringLiteral("coin.wav"), false, 0.0, 100.0}, audioContext);
				QCOMPARE(audioPlay.errorCode, eOK);
				QCOMPARE(audioPlay.returnValues.size(), 1);
				QCOMPARE(audioPlay.returnValues.constFirst().typeId(), QMetaType::Int);
				QCOMPARE(audioPlay.returnValues.constFirst().toInt(), 1);

				QCOMPARE(events.size(), 3);
				QCOMPARE(events.at(0).text, QStringLiteral("       line"));
				QVERIFY(!events.at(0).interrupt);
				QCOMPARE(events.at(1).text, QStringLiteral("urgent"));
				QVERIFY(events.at(1).interrupt);
				QVERIFY(events.at(2).stop);
				QCOMPARE(QMudNativePluginRegistry::callRoutine(&runtime, shimId, QStringLiteral("missing"),
				                                               {}, mushReaderContext)
				             .errorCode,
				         eNoSuchRoutine);

				events.clear();
				QMudNativePluginRegistry::setMushReaderPluginEnabled(&runtime, false);
				events.clear();
				const QMudNativePluginRegistry::NativeCallContext mutedContext =
				    nativeCallContext(runtime, shimId);
				QCOMPARE(QMudNativePluginRegistry::callRoutine(&runtime, shimId, QStringLiteral("say"),
				                                               {QStringLiteral("muted")}, mutedContext)
				             .errorCode,
				         eOK);
				QVERIFY(events.isEmpty());

				QVERIFY(runtime.enablePlugin(shimId, false));
				events.clear();
				const QMudNativePluginRegistry::NativeCallResult disabledCall =
				    QMudNativePluginRegistry::callRoutine(&runtime, shimId, QStringLiteral("say"),
				                                          {QStringLiteral("disabled")},
				                                          nativeCallContext(runtime, shimId));
				QCOMPARE(disabledCall.errorCode, ePluginDisabled);
				QVERIFY(disabledCall.errorText.contains(QStringLiteral("disabled")));
				QVERIFY(events.isEmpty());
			}

			void luaAudioRuntimeStateIsSharedAndMutable() const
			{
				WorldRuntime  runtime;
				const QString soundRoot = testDirectory(QStringLiteral("runtime-state"));
				QVERIFY(!soundRoot.isEmpty());
				QVERIFY(writeSoundFixture(soundRoot));
				runtime.setStartupDirectory(soundRoot);
				SoundBackendState soundState;
				installSoundBackend(runtime, soundState);

				const QString audioId = QMudNativePluginRegistry::luaAudioPluginId();
				const QMudNativePluginRegistry::NativeCallContext audioContext =
				    nativeCallContext(runtime, audioId);
				const QMudNativePluginRegistry::NativeCallResult delayedPlay =
				    QMudNativePluginRegistry::callRoutine(&runtime, audioId, QStringLiteral("playDelay"),
				                                          {QStringLiteral("coin.wav"), 10.0, 0.0, 100.0},
				                                          audioContext);
				QCOMPARE(delayedPlay.errorCode, eOK);
				QCOMPARE(delayedPlay.returnValues.size(), 1);
				QCOMPARE(delayedPlay.returnValues.constFirst().toInt(), 1);
				QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).size(), 1);

				QCOMPARE(QMudNativePluginRegistry::callRoutine(&runtime, audioId, QStringLiteral("setVol"),
				                                               {25.0, 1}, audioContext)
				             .errorCode,
				         eOK);
				QCOMPARE(QMudNativePluginRegistry::callRoutine(&runtime, audioId, QStringLiteral("setPan"),
				                                               {3.0, 1}, audioContext)
				             .errorCode,
				         eOK);
				QCOMPARE(QMudNativePluginRegistry::callRoutine(&runtime, audioId, QStringLiteral("setPitch"),
				                                               {7.0, 1}, audioContext)
				             .errorCode,
				         eOK);

				QMudNativePluginRegistry::LuaAudioRuntimeBufferState bufferState;
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeBufferState(&runtime, 1, bufferState));
				QCOMPARE(bufferState.volume, 25.0);
				QCOMPARE(bufferState.pan, 3.0);
				QCOMPARE(bufferState.pitch, 7.0);

				const QMudNativePluginRegistry::NativeCallResult volume =
				    QMudNativePluginRegistry::callRoutine(&runtime, audioId, QStringLiteral("getVolume"), {1},
				                                          audioContext);
				QCOMPARE(volume.errorCode, eOK);
				QCOMPARE(volume.returnValues.size(), 1);
				QCOMPARE(volume.returnValues.constFirst().toDouble(), 25.0);

				const int reserved = QMudNativePluginRegistry::luaAudioReserveRuntimeBuffer(
				    &runtime, [](const int) { return true; });
				QCOMPARE(reserved, 2);
				QMudNativePluginRegistry::luaAudioReleaseRuntimeBuffer(&runtime, reserved);

				QCOMPARE(QMudNativePluginRegistry::callRoutine(&runtime, audioId, QStringLiteral("stop"), {1},
				                                               audioContext)
				             .errorCode,
				         eOK);
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());

				QCOMPARE(QMudNativePluginRegistry::callRoutine(&runtime, audioId, QStringLiteral("setVol"),
				                                               {33.0}, audioContext)
				             .errorCode,
				         eOK);
				const QMudNativePluginRegistry::NativeCallResult resetPlay =
				    QMudNativePluginRegistry::callRoutine(&runtime, audioId, QStringLiteral("playDelay"),
				                                          {QStringLiteral("coin.wav"), 10.0, 0.0, 33.0},
				                                          audioContext);
				QCOMPARE(resetPlay.errorCode, eOK);
				QCOMPARE(resetPlay.returnValues.constFirst().toInt(), 1);
				QVERIFY(!QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
				QCOMPARE(QMudNativePluginRegistry::callRoutine(&runtime, audioId, QStringLiteral("slideVol"),
				                                               {10.0, 1, 0.02}, audioContext)
				             .errorCode,
				         eOK);
				QCOMPARE(QMudNativePluginRegistry::callRoutine(&runtime, audioId, QStringLiteral("fadeout"),
				                                               {1, 0.02}, audioContext)
				             .errorCode,
				         eOK);
				QMudNativePluginRegistry::luaAudioResetRuntime(&runtime);
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
				QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeMasterState(&runtime).volume, 100.0);
				const int playCallsBeforePostReset = soundState.playCalls;
				const QMudNativePluginRegistry::NativeCallResult postResetPlay =
				    QMudNativePluginRegistry::callRoutine(&runtime, audioId, QStringLiteral("play"),
				                                          {QStringLiteral("coin.wav"), false, 0.0, 33.0},
				                                          audioContext);
				QCOMPARE(postResetPlay.errorCode, eOK);
				QCOMPARE(postResetPlay.returnValues.constFirst().toInt(), 1);
				QCOMPARE(soundState.playCalls, playCallsBeforePostReset + 1);
				QMudNativePluginRegistry::LuaAudioRuntimeBufferState postResetState;
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeBufferState(&runtime, 1, postResetState));
				QCOMPARE(postResetState.volume, 33.0);
				QCOMPARE(soundState.statusByBuffer.value(1), 1);

				soundState.statusByBuffer.insert(1, 0);
				soundState.reusableByBuffer.insert(1, false);
				const QMudNativePluginRegistry::NativeCallResult transientStoppedPlay =
				    QMudNativePluginRegistry::callRoutine(&runtime, audioId, QStringLiteral("play"),
				                                          {QStringLiteral("coin.wav"), false, 0.0, 44.0},
				                                          audioContext);
				QCOMPARE(transientStoppedPlay.errorCode, eOK);
				QCOMPARE(transientStoppedPlay.returnValues.constFirst().toInt(), 2);
				QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).size(), 2);
				QMudNativePluginRegistry::luaAudioReleaseRuntimeBuffer(&runtime, 2);
				static_cast<void>(runtime.stopSoundBypassingPluginCallbacks(2));
				soundState.reusableByBuffer.remove(1);

				QVERIFY(QMudNativePluginRegistry::luaAudioReleaseRuntimeBufferIfGeneration(
				    &runtime, 1, postResetState.generation));
				const QMudNativePluginRegistry::NativeCallResult reclaimedPlay =
				    QMudNativePluginRegistry::callRoutine(&runtime, audioId, QStringLiteral("play"),
				                                          {QStringLiteral("coin.wav"), false, 0.0, 55.0},
				                                          audioContext);
				QCOMPARE(reclaimedPlay.errorCode, eOK);
				QCOMPARE(reclaimedPlay.returnValues.constFirst().toInt(), 1);
				QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).size(), 1);
				QMudNativePluginRegistry::LuaAudioRuntimeBufferState reclaimedState;
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeBufferState(&runtime, 1, reclaimedState));
				QCOMPARE(reclaimedState.volume, 55.0);
				QCOMPARE(soundState.statusByBuffer.value(1), 1);

				QMudNativePluginRegistry::luaAudioReleaseRuntimeBuffer(&runtime, 1);
				static_cast<void>(runtime.stopSoundBypassingPluginCallbacks(1));
				soundState.playRequiresLuaAudioState = true;
				soundState.playReleasesLuaAudioState = true;
				const QMudNativePluginRegistry::NativeCallResult immediateReleasePlay =
				    QMudNativePluginRegistry::callRoutine(&runtime, audioId, QStringLiteral("play"),
				                                          {QStringLiteral("coin.wav"), false, 0.0, 65.0},
				                                          audioContext);
				QCOMPARE(immediateReleasePlay.errorCode, eOK);
				QCOMPARE(immediateReleasePlay.returnValues.constFirst().toInt(), 1);
				QVERIFY(soundState.sawLuaAudioStateDuringPlay);
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
			}

			void luaAudioCommandsAndPlaySoundCallbackMirrorLegacyPlugin() const
			{
				WorldRuntime  runtime;
				const QString soundRoot = testDirectory(QStringLiteral("commands-and-play-sound"));
				QVERIFY(!soundRoot.isEmpty());
				QVERIFY(writeSoundFixture(soundRoot));
				runtime.setStartupDirectory(soundRoot);
				SoundBackendState soundState;
				installSoundBackend(runtime, soundState);
				QStringList outputLines;
				captureOutputLines(runtime, outputLines);

				QVERIFY(!QMudNativePluginRegistry::handleLuaAudioCommand(&runtime,
				                                                         QStringLiteral("LuaAudio help")));
				QVERIFY(
				    !QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QStringLiteral("pan=15")));
				QVERIFY(outputLines.isEmpty());
				QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeMasterState(&runtime).pan, 0.0);

				WorldRuntime::Plugin audioPlugin;
				audioPlugin.enabled    = false;
				audioPlugin.nativeShim = true;
				audioPlugin.attributes.insert(QStringLiteral("id"),
				                              QMudNativePluginRegistry::luaAudioPluginId());
				runtime.pluginsMutable().push_back(audioPlugin);
				QVERIFY(
				    !QMudNativePluginRegistry::handleLuaAudioCommand(&runtime, QStringLiteral("LuaAudio")));

				runtime.pluginsMutable().back().enabled = true;

				QVERIFY(QMudNativePluginRegistry::handleLuaAudioCommand(&runtime,
				                                                        QStringLiteral("LuaAudio help")));
				QVERIFY(outputLines.constLast().contains(QStringLiteral("LuaAudio")));

				QVERIFY(
				    QMudNativePluginRegistry::handleLuaAudioCommand(&runtime, QStringLiteral("volume_down")));
				QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeMasterState(&runtime).volume, 95.0);
				QVERIFY(QMudNativePluginRegistry::handleLuaAudioCommand(&runtime,
				                                                        QStringLiteral("sound_toggle")));
				QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeMasterState(&runtime).volume, 0.0);
				QVERIFY(QMudNativePluginRegistry::handleLuaAudioCommand(&runtime,
				                                                        QStringLiteral("sound_toggle")));
				QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeMasterState(&runtime).volume, 95.0);

				QVERIFY(
				    QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QStringLiteral("pan=15")));
				QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeMasterState(&runtime).pan, 15.0);
				QVERIFY(
				    QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QStringLiteral("volume=45")));
				QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeMasterState(&runtime).volume, 45.0);
				QVERIFY(
				    QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QStringLiteral("freq=3")));
				QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeMasterState(&runtime).pitch, 3.0);

				QVERIFY(
				    QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QStringLiteral("coin.wav")));
				QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).size(), 1);
				QCOMPARE(runtime.soundStatus(1), 1);
				QCOMPARE(soundState.playCalls, 1);
				soundState.failingPlayFiles.push_back(QStringLiteral("coin.wav"));
				QVERIFY(
				    !QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QStringLiteral("coin.wav")));
				QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).size(), 1);
				QCOMPARE(runtime.soundStatus(1), 1);
				QCOMPARE(soundState.playCalls, 2);
				soundState.failingPlayFiles.clear();
				QVERIFY(QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QString()));
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
				QCOMPARE(runtime.soundStatus(1), -2);
				QCOMPARE(soundState.stopCalls, 1);

				soundState.playRequiresLuaAudioState  = true;
				soundState.playReleasesLuaAudioState  = true;
				soundState.sawLuaAudioStateDuringPlay = false;
				QVERIFY(QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime,
				                                                          QStringLiteral("loop=coin.wav")));
				QVERIFY(soundState.sawLuaAudioStateDuringPlay);
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
				QVERIFY(QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime,
				                                                          QStringLiteral("stop=coin.wav")));
				QCOMPARE(soundState.stopCalls, 1);
				soundState.playRequiresLuaAudioState = false;
				soundState.playReleasesLuaAudioState = false;
				soundState.statusByBuffer.remove(1);

				QVERIFY(QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime,
				                                                          QStringLiteral("loop=coin.wav")));
				QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).size(), 1);
				QCOMPARE(runtime.soundStatus(1), 2);
				QVERIFY(QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime,
				                                                          QStringLiteral("stop=coin.wav")));
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
				QCOMPARE(runtime.soundStatus(1), -2);
				QCOMPARE(soundState.stopCalls, 2);

				QVERIFY(QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime,
				                                                          QStringLiteral("loop=coin.wav")));
				QVERIFY(!QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
				QVERIFY(runtime.enablePlugin(QMudNativePluginRegistry::luaAudioPluginId(), false));
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
				QCOMPARE(runtime.soundStatus(1), -2);
				QCOMPARE(soundState.stopCalls, 3);
				QVERIFY(!QMudNativePluginRegistry::handleLuaAudioCommand(&runtime,
				                                                         QStringLiteral("LuaAudio help")));

				QVERIFY(runtime.enablePlugin(QMudNativePluginRegistry::luaAudioPluginId(), true));
				QVERIFY(QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime,
				                                                          QStringLiteral("loop=coin.wav")));
				QVERIFY(!QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
				QString unloadError;
				QVERIFY(runtime.unloadPlugin(QMudNativePluginRegistry::luaAudioPluginId(), &unloadError));
				QVERIFY(unloadError.isEmpty());
				QVERIFY(runtime.plugins().isEmpty());
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
				QCOMPARE(runtime.soundStatus(1), -2);
				QCOMPARE(soundState.stopCalls, 4);
				QVERIFY(!QMudNativePluginRegistry::handleLuaAudioCommand(&runtime,
				                                                         QStringLiteral("LuaAudio help")));
			}

			void commandFallbackAndSubstitutionBehavior() const
			{
				const QString directory = testDirectory(QStringLiteral("command-fallback"));
				QVERIFY(!directory.isEmpty());
				WorldRuntime runtime;
				runtime.setStartupDirectory(directory);
				QStringList outputLines;
				captureOutputLines(runtime, outputLines);

				QVector<QMudNativePluginRegistry::TestSpeechEvent> events;
				QMudNativePluginRegistry::setTestSpeechSink(
				    [&events](const QMudNativePluginRegistry::TestSpeechEvent &event)
				    { events.push_back(event); });
				const auto restoreSpeechSink =
				    qScopeGuard([] { QMudNativePluginRegistry::setTestSpeechSink({}); });
				QMudNativePluginRegistry::setMushReaderPluginEnabled(&runtime, true);

				QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime,
				                                                          QStringLiteral("tts_note hello")));
				QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(
				    &runtime, QStringLiteral("tts_interrupt now")));
				QVERIFY(
				    QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("tts_stop")));
				QCOMPARE(events.size(), 3);
				QCOMPARE(events.at(0).text, QStringLiteral("hello"));
				QVERIFY(!events.at(0).interrupt);
				QCOMPARE(events.at(1).text, QStringLiteral("now"));
				QVERIFY(events.at(1).interrupt);
				QVERIFY(events.at(2).stop);
				events.clear();

				QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime,
				                                                          QStringLiteral("MushReader help")));
				QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(
				    &runtime, QStringLiteral("subst add source line==replacement line")));
				QVERIFY(outputLines.contains(QStringLiteral("MushReader native QMud shim.")));
				QVERIFY(QFileInfo::exists(QDir(directory).filePath(QStringLiteral("substitutions.mush"))));

				QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("tts")));
				QVERIFY(!QMudNativePluginRegistry::isMushReaderSpeechEnabled(&runtime));
				events.clear();
				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0,
				                                                     QStringLiteral("source line"));
				QVERIFY(events.isEmpty());

				QMudNativePluginRegistry::setMushReaderPluginEnabled(&runtime, true);
				events.clear();
				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0,
				                                                     QStringLiteral("source line"));
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.constFirst().text, QStringLiteral("replacement line"));
				events.clear();
				QMudNativePluginRegistry::handleMushReaderPartialLine(&runtime,
				                                                      QStringLiteral("source line"));
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.constFirst().text, QStringLiteral("replacement line"));

				QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(
				    &runtime, QStringLiteral("subst add muted==!skip")));
				events.clear();
				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0, QStringLiteral("muted"));
				QMudNativePluginRegistry::handleMushReaderPartialLine(&runtime, QStringLiteral("muted"));
				QVERIFY(events.isEmpty());

				QVERIFY(
				    QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("subst off")));
				events.clear();
				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0,
				                                                     QStringLiteral("source line"));
				QVERIFY(events.isEmpty());
			}

			void screenDrawTabCompleteAndToggleSpeech()
			{
				WorldRuntime                                       runtime;
				QVector<QMudNativePluginRegistry::TestSpeechEvent> events;
				QMudNativePluginRegistry::setTestSpeechSink(
				    [&events](const QMudNativePluginRegistry::TestSpeechEvent &event)
				    { events.push_back(event); });
				const auto restoreSpeechSink =
				    qScopeGuard([] { QMudNativePluginRegistry::setTestSpeechSink({}); });

				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 2, 0,
				                                                     QStringLiteral("ignored"));
				QVERIFY(events.isEmpty());
				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0,
				                                                     QStringLiteral("spoken"));
				QVERIFY(events.isEmpty());
				QMudNativePluginRegistry::handleMushReaderPartialLine(&runtime, QStringLiteral("<prompt> "));
				QVERIFY(events.isEmpty());

				QMudNativePluginRegistry::handleMushReaderTabComplete(&runtime, QStringLiteral("north"));
				QVERIFY(events.isEmpty());

				QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("tts")));
				QVERIFY(!QMudNativePluginRegistry::isQtAccessibilitySpeechEnabled(&runtime));
				QVERIFY(events.isEmpty());

				QMudNativePluginRegistry::setMushReaderPluginEnabled(&runtime, true);

				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0,
				                                                     QStringLiteral("spoken"));
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.at(0).text, QStringLiteral("spoken"));

				QMudNativePluginRegistry::handleMushReaderTabComplete(&runtime, QStringLiteral("north"));
				QCOMPARE(events.size(), 2);
				QCOMPARE(events.at(1).text, QStringLiteral("north"));

				QMudNativePluginRegistry::handleMushReaderPartialLine(&runtime, QStringLiteral("<prompt> "));
				QCOMPARE(events.size(), 3);
				QCOMPARE(events.at(2).text, QStringLiteral("<prompt> "));
				QVERIFY(!events.at(2).interrupt);
				QMudNativePluginRegistry::handleMushReaderPartialLine(&runtime, QStringLiteral("<prompt> "));
				QCOMPARE(events.size(), 3);
				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0,
				                                                     QStringLiteral("<prompt> "));
				QCOMPARE(events.size(), 3);
				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0,
				                                                     QStringLiteral("<prompt> "));
				QCOMPARE(events.size(), 4);
				QCOMPARE(events.at(3).text, QStringLiteral("<prompt> "));

				events.clear();
				QMudNativePluginRegistry::handleMushReaderPartialLine(&runtime, QStringLiteral("<cleared> "));
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.constLast().text, QStringLiteral("<cleared> "));
				QMudNativePluginRegistry::handleMushReaderPartialLine(&runtime, QString());
				events.clear();
				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0,
				                                                     QStringLiteral("<cleared> "));
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.constLast().text, QStringLiteral("<cleared> "));

				events.clear();
				QMudNativePluginRegistry::handleMushReaderPartialLine(&runtime, QStringLiteral("<stale> "));
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.constLast().text, QStringLiteral("<stale> "));
				QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("tts")));
				QVERIFY(!QMudNativePluginRegistry::isMushReaderSpeechEnabled(&runtime));
				QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("tts")));
				QVERIFY(QMudNativePluginRegistry::isMushReaderSpeechEnabled(&runtime));
				events.clear();
				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0,
				                                                     QStringLiteral("<stale> "));
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.constLast().text, QStringLiteral("<stale> "));

				events.clear();
				QMudNativePluginRegistry::handleMushReaderPartialLine(&runtime,
				                                                      QStringLiteral("<plugin-toggle> "));
				QCOMPARE(events.size(), 1);
				QMudNativePluginRegistry::setMushReaderPluginEnabled(&runtime, false);
				QMudNativePluginRegistry::setMushReaderPluginEnabled(&runtime, true);
				events.clear();
				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0,
				                                                     QStringLiteral("<plugin-toggle> "));
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.constLast().text, QStringLiteral("<plugin-toggle> "));

				QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("tts")));
				QCOMPARE(events.size(), 3);
				QVERIFY(events.at(1).stop);
				QCOMPARE(events.at(2).text, QStringLiteral("speech off"));
				QVERIFY(events.at(2).interrupt);

				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0, QStringLiteral("muted"));
				QMudNativePluginRegistry::handleMushReaderPartialLine(&runtime, QStringLiteral("<muted> "));
				QCOMPARE(events.size(), 3);
			}

			void partialLineDeliveryFailureDoesNotSuppressScreenDraw()
			{
				WorldRuntime                                       runtime;
				QVector<QMudNativePluginRegistry::TestSpeechEvent> events;
				bool                                               acceptSpeech = false;
				QMudNativePluginRegistry::setTestSpeechSinkWithResult(
				    [&events, &acceptSpeech](const QMudNativePluginRegistry::TestSpeechEvent &event)
				    {
					    events.push_back(event);
					    return acceptSpeech;
				    });
				const auto restoreSpeechSink =
				    qScopeGuard([] { QMudNativePluginRegistry::setTestSpeechSink({}); });
				QMudNativePluginRegistry::setMushReaderPluginEnabled(&runtime, true);

				QMudNativePluginRegistry::handleMushReaderPartialLine(&runtime, QStringLiteral("<failed> "));
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.constLast().text, QStringLiteral("<failed> "));

				events.clear();
				acceptSpeech = true;
				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0,
				                                                     QStringLiteral("<failed> "));
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.constLast().text, QStringLiteral("<failed> "));
			}

			void clearPartialLineAllowsLaterScreenDrawOfSameText()
			{
				WorldRuntime                                       runtime;
				QVector<QMudNativePluginRegistry::TestSpeechEvent> events;
				QMudNativePluginRegistry::setTestSpeechSink(
				    [&events](const QMudNativePluginRegistry::TestSpeechEvent &event)
				    { events.push_back(event); });
				const auto restoreSpeechSink =
				    qScopeGuard([] { QMudNativePluginRegistry::setTestSpeechSink({}); });
				QMudNativePluginRegistry::setMushReaderPluginEnabled(&runtime, true);

				QMudNativePluginRegistry::handleMushReaderPartialLine(&runtime, QStringLiteral("<omitted> "));
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.constLast().text, QStringLiteral("<omitted> "));

				QMudNativePluginRegistry::clearMushReaderPartialLine(&runtime);
				events.clear();

				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0,
				                                                     QStringLiteral("<omitted> "));
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.constLast().text, QStringLiteral("<omitted> "));
			}

			void blankScreenDrawClearsPartialLineSuppression()
			{
				WorldRuntime                                       runtime;
				QVector<QMudNativePluginRegistry::TestSpeechEvent> events;
				QMudNativePluginRegistry::setTestSpeechSink(
				    [&events](const QMudNativePluginRegistry::TestSpeechEvent &event)
				    { events.push_back(event); });
				const auto restoreSpeechSink =
				    qScopeGuard([] { QMudNativePluginRegistry::setTestSpeechSink({}); });
				QMudNativePluginRegistry::setMushReaderPluginEnabled(&runtime, true);

				const QString prompt = QStringLiteral("<blank-cleared> ");
				QMudNativePluginRegistry::handleMushReaderPartialLine(&runtime, prompt);
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.constLast().text, prompt);

				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0, QString());
				events.clear();

				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0, prompt);
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.constLast().text, prompt);
			}

			void stopSpeechKeepsPartialScreenDrawSuppression()
			{
				WorldRuntime                                       runtime;
				QVector<QMudNativePluginRegistry::TestSpeechEvent> events;
				QMudNativePluginRegistry::setTestSpeechSink(
				    [&events](const QMudNativePluginRegistry::TestSpeechEvent &event)
				    { events.push_back(event); });
				const auto restoreSpeechSink =
				    qScopeGuard([] { QMudNativePluginRegistry::setTestSpeechSink({}); });
				QMudNativePluginRegistry::setMushReaderPluginEnabled(&runtime, true);

				QMudNativePluginRegistry::handleMushReaderPartialLine(&runtime, QStringLiteral("<stopped> "));
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.constLast().text, QStringLiteral("<stopped> "));

				QVERIFY(
				    QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("tts_stop")));
				QCOMPARE(events.size(), 2);
				QVERIFY(events.constLast().stop);

				events.clear();
				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0,
				                                                     QStringLiteral("<stopped> "));
				QVERIFY(events.isEmpty());
			}

			void reviewSpeechUsesActiveMushReaderState() const
			{
				const QString directory = testDirectory(QStringLiteral("review-speech"));
				QVERIFY(!directory.isEmpty());
				WorldRuntime runtime;
				runtime.setStartupDirectory(directory);

				QVector<QMudNativePluginRegistry::TestSpeechEvent> events;
				QMudNativePluginRegistry::setTestSpeechSink(
				    [&events](const QMudNativePluginRegistry::TestSpeechEvent &event)
				    { events.push_back(event); });
				const auto restoreSpeechSink =
				    qScopeGuard([] { QMudNativePluginRegistry::setTestSpeechSink({}); });

				QVERIFY(!QMudNativePluginRegistry::speakMushReaderReviewText(&runtime,
				                                                             QStringLiteral("review line")));
				QVERIFY(events.isEmpty());

				QMudNativePluginRegistry::setMushReaderPluginEnabled(&runtime, true);
				QVERIFY(QMudNativePluginRegistry::speakMushReaderReviewText(&runtime,
				                                                            QStringLiteral("review line")));
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.constLast().text, QStringLiteral("review line"));
				QVERIFY(events.constLast().interrupt);

				QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(
				    &runtime, QStringLiteral("subst add review line==spoken review")));
				events.clear();
				QVERIFY(QMudNativePluginRegistry::speakMushReaderReviewText(&runtime,
				                                                            QStringLiteral("review line")));
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.constLast().text, QStringLiteral("spoken review"));
				QVERIFY(events.constLast().interrupt);

				QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(
				    &runtime, QStringLiteral("subst add quiet line==!skip")));
				events.clear();
				QVERIFY(QMudNativePluginRegistry::speakMushReaderReviewText(&runtime,
				                                                            QStringLiteral("quiet line")));
				QVERIFY(events.isEmpty());
			}

			void silentStopPathsDoNotInitializeSpeechBackends()
			{
				WorldRuntime runtime;
				QCOMPARE(QMudNativePluginRegistry::mushReaderTestBackendCount(&runtime), std::size_t{0});

				QMudNativePluginRegistry::setMushReaderPluginEnabled(&runtime, false);
				QCOMPARE(QMudNativePluginRegistry::mushReaderTestBackendCount(&runtime), std::size_t{0});

				QMudNativePluginRegistry::setQtAccessibilitySpeechEnabled(&runtime, false);
				QCOMPARE(QMudNativePluginRegistry::mushReaderTestBackendCount(&runtime), std::size_t{0});

				QVERIFY(
				    QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("tts_stop")));
				QCOMPARE(QMudNativePluginRegistry::mushReaderTestBackendCount(&runtime), std::size_t{0});
			}

			void runtimeSetupRegistersNativeAccelerator()
			{
				WorldRuntime runtime;
				QMudNativePluginRegistry::ensureMushReaderRuntimeSetup(&runtime);

				bool found = false;
				for (const qint64 key : runtime.acceleratorKeys())
				{
					const int commandId = runtime.acceleratorCommandForKey(key);
					if (runtime.acceleratorCommandText(commandId) == QStringLiteral("tts"))
					{
						found = true;
						QCOMPARE(runtime.acceleratorSendTarget(commandId), eSendToExecute);
						QCOMPARE(runtime.acceleratorPluginId(commandId),
						         QMudNativePluginRegistry::mushReaderPluginId());
					}
				}
				QVERIFY(found);
			}

			void ttsToggleUsesMushReaderSpeechGateOnlyWhenMushReaderIsEnabled()
			{
				WorldRuntime                                       runtime;
				QVector<QMudNativePluginRegistry::TestSpeechEvent> events;
				QMudNativePluginRegistry::setTestSpeechSink(
				    [&events](const QMudNativePluginRegistry::TestSpeechEvent &event)
				    { events.push_back(event); });
				const auto restoreSpeechSink =
				    qScopeGuard([] { QMudNativePluginRegistry::setTestSpeechSink({}); });

				QVERIFY(!QMudNativePluginRegistry::isMushReaderPluginEnabled(&runtime));
				QVERIFY(QMudNativePluginRegistry::isQtAccessibilitySpeechEnabled(&runtime));
				QVERIFY(!runtime.hasMushReaderLiveSpeechOwner());

				QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("tts")));
				QVERIFY(!QMudNativePluginRegistry::isQtAccessibilitySpeechEnabled(&runtime));
				QVERIFY(!QMudNativePluginRegistry::isMushReaderPluginEnabled(&runtime));
				QVERIFY(!runtime.hasMushReaderLiveSpeechOwner());
				QVERIFY(events.isEmpty());

				QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("tts")));
				QVERIFY(QMudNativePluginRegistry::isQtAccessibilitySpeechEnabled(&runtime));
				QVERIFY(!QMudNativePluginRegistry::isMushReaderPluginEnabled(&runtime));
				QVERIFY(events.isEmpty());

				QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("tts")));
				QVERIFY(!QMudNativePluginRegistry::isQtAccessibilitySpeechEnabled(&runtime));
				QVERIFY(!runtime.hasMushReaderLiveSpeechOwner());
				QVERIFY(events.isEmpty());

				WorldRuntime::Plugin mushReaderPlugin;
				mushReaderPlugin.nativeShim = true;
				mushReaderPlugin.enabled    = false;
				mushReaderPlugin.attributes.insert(QStringLiteral("id"),
				                                   QMudNativePluginRegistry::mushReaderPluginId());
				runtime.pluginsMutable().push_back(mushReaderPlugin);
				QVERIFY(!runtime.hasMushReaderLiveSpeechOwner());

				QVERIFY(runtime.enablePlugin(QMudNativePluginRegistry::mushReaderPluginId(), true));
				QVERIFY(QMudNativePluginRegistry::isMushReaderPluginEnabled(&runtime));
				QVERIFY(!QMudNativePluginRegistry::isQtAccessibilitySpeechEnabled(&runtime));
				QVERIFY(runtime.hasMushReaderLiveSpeechOwner());

				events.clear();
				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0,
				                                                     QStringLiteral("mushreader line"));
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.constFirst().text, QStringLiteral("mushreader line"));

				QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("tts")));
				QVERIFY(QMudNativePluginRegistry::isMushReaderPluginEnabled(&runtime));
				QVERIFY(!QMudNativePluginRegistry::isQtAccessibilitySpeechEnabled(&runtime));
				QVERIFY(runtime.hasMushReaderLiveSpeechOwner());
				QVERIFY(events.size() >= 3);
				QVERIFY(events.at(events.size() - 2).stop);
				QCOMPARE(events.constLast().text, QStringLiteral("speech off"));

				events.clear();
				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0,
				                                                     QStringLiteral("muted line"));
				QVERIFY(events.isEmpty());

				QVERIFY(runtime.enablePlugin(QMudNativePluginRegistry::mushReaderPluginId(), true));
				QVERIFY(QMudNativePluginRegistry::isMushReaderPluginEnabled(&runtime));
				QVERIFY(!QMudNativePluginRegistry::isQtAccessibilitySpeechEnabled(&runtime));
				QVERIFY(runtime.hasMushReaderLiveSpeechOwner());
				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0,
				                                                     QStringLiteral("still muted line"));
				QVERIFY(events.isEmpty());

				QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("tts")));
				QVERIFY(QMudNativePluginRegistry::isMushReaderPluginEnabled(&runtime));
				QVERIFY(!QMudNativePluginRegistry::isQtAccessibilitySpeechEnabled(&runtime));
				QVERIFY(runtime.hasMushReaderLiveSpeechOwner());

				events.clear();
				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0,
				                                                     QStringLiteral("enabled line"));
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.constFirst().text, QStringLiteral("enabled line"));

				QVERIFY(runtime.enablePlugin(QMudNativePluginRegistry::mushReaderPluginId(), false));
				QVERIFY(!QMudNativePluginRegistry::isMushReaderPluginEnabled(&runtime));
				QVERIFY(!QMudNativePluginRegistry::isQtAccessibilitySpeechEnabled(&runtime));
				QVERIFY(!runtime.hasMushReaderLiveSpeechOwner());

				events.clear();
				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0,
				                                                     QStringLiteral("passive disabled line"));
				QVERIFY(events.isEmpty());

				QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("tts")));
				QVERIFY(!QMudNativePluginRegistry::isMushReaderPluginEnabled(&runtime));
				QVERIFY(QMudNativePluginRegistry::isQtAccessibilitySpeechEnabled(&runtime));
				QVERIFY(!runtime.hasMushReaderLiveSpeechOwner());
				events.clear();
				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0,
				                                                     QStringLiteral("passive line"));
				QVERIFY(events.isEmpty());

				QVERIFY(runtime.enablePlugin(QMudNativePluginRegistry::mushReaderPluginId(), true));
				QVERIFY(QMudNativePluginRegistry::isMushReaderPluginEnabled(&runtime));
				QVERIFY(QMudNativePluginRegistry::isQtAccessibilitySpeechEnabled(&runtime));
				QVERIFY(runtime.hasMushReaderLiveSpeechOwner());

				QString unloadError;
				QVERIFY(runtime.unloadPlugin(QMudNativePluginRegistry::mushReaderPluginId(), &unloadError));
				QVERIFY(unloadError.isEmpty());
				QVERIFY(!QMudNativePluginRegistry::isMushReaderPluginEnabled(&runtime));
				QVERIFY(QMudNativePluginRegistry::isQtAccessibilitySpeechEnabled(&runtime));
				QVERIFY(!runtime.hasMushReaderLiveSpeechOwner());
			}

			void blacklistAndProtectedPluginXmlClassification() const
			{
				const QStringList blacklistIds{QStringLiteral("bb6a05ed7534b5db1ed40511"),
				                               QStringLiteral("b8e6dac1ee7fe8e3de931fb7"),
				                               QStringLiteral("8238deec7c06bade8ebc3819")};
				for (const QString &id : blacklistIds)
				{
					QVERIFY(QMudNativePluginRegistry::isBlacklistedId(id));
					QVERIFY(QMudNativePluginRegistry::isProtectedId(id));
					QVERIFY(!QMudNativePluginRegistry::isShimId(id));
					QVERIFY(!QMudNativePluginRegistry::pluginInfo(id, 1).isValid());
					QCOMPARE(QMudNativePluginRegistry::pluginSupports(id, QStringLiteral("say")),
					         eNoSuchPlugin);
				}

				const QString directory = testDirectory(QStringLiteral("plugin-xml-classification"));
				QVERIFY(!directory.isEmpty());
				for (const QString &id :
				     blacklistIds + QStringList{QMudNativePluginRegistry::mushReaderPluginId(),
				                                QMudNativePluginRegistry::luaAudioPluginId()})
				{
					const QString path =
					    writePluginXml(directory, id, QStringLiteral("package.loadlib('x','y')"));
					QVERIFY(!path.isEmpty());
					WorldDocument doc;
					QVERIFY(doc.loadFromPluginFile(path));
					QVERIFY(!doc.plugins().isEmpty());
					const QString parsedId =
					    doc.plugins().front().attributes.value(QStringLiteral("id")).trimmed().toLower();
					QCOMPARE(parsedId, id);
					QVERIFY(QMudNativePluginRegistry::isProtectedId(parsedId));
				}
			}
			// NOLINTEND(readability-convert-member-functions-to-static)

		private:
			/**
			 * @brief Returns and creates a test-specific directory under the fixture artifact root.
			 * @param name Directory name within the fixture root.
			 * @return Absolute directory path, or an empty string when creation fails.
			 */
			[[nodiscard]] QString testDirectory(const QString &name) const
			{
				const QString path = QDir(m_artifactRoot).filePath(name);
				return QDir().mkpath(path) ? path : QString();
			}

			QString m_artifactRoot;
	};
} // namespace

QTEST_MAIN(tst_NativePluginRegistry)

#if __has_include("tst_NativePluginRegistry.moc")
#include "tst_NativePluginRegistry.moc"
#endif
