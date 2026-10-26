/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_NativePluginRegistry.cpp
 * Role: Unit coverage for native compatibility plugin registry behavior.
 */

#include "LuaSupport.h"
#include "NativePluginRegistry.h"
#include "SoundEngine.h"
#include "WorldDocument.h"
#include "WorldRuntime.h"
#include "WorldRuntimeTestAccess.h"
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

#include <limits>
#include <utility>

namespace
{
	/**
	 * @brief Mutable state exposed through the production runtime sound-test boundary.
	 */
	struct SoundBackendState
	{
			QHash<int, int>    statusByBuffer;
			QHash<int, double> volumeByBuffer;
			QHash<int, double> panByBuffer;
			QHash<int, double> pitchByBuffer;
			QHash<int, bool>   reusableByBuffer;
			QSet<int>          reservedBuffers;
			QStringList        reservedFiles;
			QStringList        failingPlayFiles;
			int                playCalls{0};
			int                stopCalls{0};
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
		    [&state](const int buffer, const QString &fileName, const bool loop, const double volume, double)
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
			const int targetBuffer = buffer > 0 ? buffer : 1;
			state.reservedBuffers.remove(targetBuffer);
			state.statusByBuffer.insert(targetBuffer, loop ? 2 : 1);
			state.volumeByBuffer.insert(targetBuffer, volume + 100.0);
			return eOK;
		};
		backend.stop = [&state](const int buffer)
		{
			++state.stopCalls;
			if (buffer == 0)
			{
				state.statusByBuffer.clear();
				state.reservedBuffers.clear();
			}
			else
			{
				state.statusByBuffer.remove(buffer);
				state.reservedBuffers.remove(buffer);
			}
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
			if (state.reservedBuffers.contains(buffer))
				return false;
			if (state.reusableByBuffer.contains(buffer))
				return state.reusableByBuffer.value(buffer);
			const int status = state.statusByBuffer.value(buffer, -2);
			return status == -2 || status == 0;
		};
		backend.reserve = [&state](const QString &fileName, const quint64)
		{
			state.reservedFiles.push_back(fileName);
			for (int buffer = 1; buffer <= WorldRuntime::kMaxSoundBuffers; ++buffer)
			{
				const int  status   = state.statusByBuffer.value(buffer, -2);
				const bool reusable = state.reusableByBuffer.contains(buffer)
				                          ? state.reusableByBuffer.value(buffer)
				                          : status == -2 || status == 0;
				if (!state.reservedBuffers.contains(buffer) && reusable)
				{
					state.reservedBuffers.insert(buffer);
					return buffer;
				}
			}
			return 0;
		};
		backend.volume    = [&state](const int buffer) { return state.volumeByBuffer.value(buffer, 0.0); };
		backend.attribute = [&state](const int buffer, quint64, const QMudSoundAttribute attribute,
		                             const double value, const std::chrono::milliseconds duration, bool,
		                             const QMudSoundAttributeTarget targetMode)
		{
			if (attribute == QMudSoundAttribute::Volume && duration.count() == 0)
				state.volumeByBuffer.insert(buffer, value);
			if (attribute == QMudSoundAttribute::Pan && duration.count() == 0)
				state.panByBuffer.insert(buffer, value);
			if (attribute == QMudSoundAttribute::Pitch && duration.count() == 0)
				state.pitchByBuffer.insert(buffer, targetMode == QMudSoundAttributeTarget::Relative
				                                       ? state.pitchByBuffer.value(buffer) + value
				                                       : value);
			return eOK;
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
#ifdef QMUD_ENABLE_LUA_SCRIPTING
				const QStringList ppiRoutines{QStringLiteral("setPitch"),
				                              QStringLiteral("slidePitch"),
				                              QStringLiteral("isPlaying"),
				                              QStringLiteral("getVolume"),
				                              QStringLiteral("stop"),
				                              QStringLiteral("setPan"),
				                              QStringLiteral("play"),
				                              QStringLiteral("playDelay"),
				                              QStringLiteral("playDelayLooped"),
				                              QStringLiteral("setVol"),
				                              QStringLiteral("slideVol"),
				                              QStringLiteral("fadeout"),
				                              QStringLiteral("slidePan"),
				                              QStringLiteral("playLooped")};
				for (const QString &routine : ppiRoutines)
				{
					QCOMPARE(QMudNativePluginRegistry::pluginSupports(
					             audioShimId, QStringLiteral("PPI_%1_PPI_").arg(routine)),
					         eOK);
				}
#endif
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
				SoundBackendState soundState;
				WorldRuntime      runtime;
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
				        {QStringLiteral("coin.wav"), 0, 0.0, 100.0}, audioContext,
				        QMudNativePluginRegistry::NativeCallExecutionMode::ValidateOnly);
				QCOMPARE(validationPlay.errorCode, eOK);
				QCOMPARE(validationPlay.returnValues.size(), 1);
				QCOMPARE(validationPlay.returnValues.constFirst().toInt(), 0);
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
				const QMudNativePluginRegistry::NativeCallResult audioPlay =
				    QMudNativePluginRegistry::callRoutine(
				        &runtime, QMudNativePluginRegistry::luaAudioPluginId(), QStringLiteral("play"),
				        {QStringLiteral("coin.wav"), 0, 0.0, 100.0}, audioContext);
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

			/** @brief Verifies numeric status, active global volume, and the original delayed-loop default. */
			void luaAudioOriginalPlaybackContracts() const
			{
				SoundBackendState state;
				WorldRuntime      runtime;
				installSoundBackend(runtime, state);
				const auto played = QMudNativePluginRegistry::callLuaAudio(
				    &runtime, QStringLiteral("play"), {QStringLiteral("coin.wav"), 1, 0, 80});
				QCOMPARE(played.errorCode, eOK);
				QCOMPARE(played.returnValues.front().toInt(), 1);
				const auto playing =
				    QMudNativePluginRegistry::callLuaAudio(&runtime, QStringLiteral("isPlaying"), {1});
				QCOMPARE(playing.returnValues.front().typeId(), QMetaType::Int);
				QCOMPARE(playing.returnValues.front().toInt(), 1);
				QCOMPARE(QMudNativePluginRegistry::callLuaAudio(&runtime, QStringLiteral("pitch"), {80, 1})
				             .errorCode,
				         eOK);
				QCOMPARE(
				    QMudNativePluginRegistry::callLuaAudio(&runtime, QStringLiteral("slidePitch"), {80, 1, 0})
				        .errorCode,
				    eOK);
				QCOMPARE(state.pitchByBuffer.value(1), 160.0);
				QCOMPARE(QMudNativePluginRegistry::callLuaAudio(&runtime, QStringLiteral("slidePitch"),
				                                                {-40, 1, 0})
				             .errorCode,
				         eOK);
				QCOMPARE(state.pitchByBuffer.value(1), 120.0);
				QCOMPARE(QMudNativePluginRegistry::callLuaAudio(&runtime, QStringLiteral("volume"), {35})
				             .errorCode,
				         eOK);
				QCOMPARE(state.volumeByBuffer.value(1), 35.0);
				QCOMPARE(QMudNativePluginRegistry::callLuaAudio(&runtime, QStringLiteral("getVolume"), {1})
				             .returnValues.front()
				             .toDouble(),
				         35.0);
				QCOMPARE(QMudNativePluginRegistry::callLuaAudio(&runtime, QStringLiteral("volume"), {0, 1})
				             .errorCode,
				         eOK);
				QCOMPARE(QMudNativePluginRegistry::callLuaAudio(&runtime, QStringLiteral("volume"), {70})
				             .errorCode,
				         eOK);
				QCOMPARE(state.volumeByBuffer.value(1), 0.0);
				QCOMPARE(QMudNativePluginRegistry::callLuaAudio(&runtime, QStringLiteral("getVolume"), {})
				             .returnValues.front()
				             .toDouble(),
				         70.0);
				QCOMPARE(
				    QMudNativePluginRegistry::callLuaAudio(&runtime, QStringLiteral("stop"), {0}).errorCode,
				    eOK);
				QCOMPARE(QMudNativePluginRegistry::callLuaAudio(&runtime, QStringLiteral("getVolume"), {1})
				             .returnValues.front()
				             .toDouble(),
				         0.0);
				QCOMPARE(QMudNativePluginRegistry::callLuaAudio(&runtime, QStringLiteral("getVolume"), {0})
				             .returnValues.front()
				             .toDouble(),
				         70.0);
				const int  before  = state.playCalls;
				const auto delayed = QMudNativePluginRegistry::callLuaAudio(
				    &runtime, QStringLiteral("playDelayLooped"), {QStringLiteral("coin.wav")});
				QCOMPARE(delayed.errorCode, eOK);
				QCOMPARE(delayed.returnValues.size(), 1);
				QVERIFY(!delayed.returnValues.front().isValid());
				QCOMPARE(state.playCalls, before);
				QCOMPARE(state.reservedBuffers.size(), 1);
				QTRY_COMPARE(state.playCalls, before + 1);
				QCOMPARE(state.statusByBuffer.value(1), 2);
				runtime.setPluginVariableValue(QMudNativePluginRegistry::luaAudioPluginId(),
				                               QStringLiteral("vol"), QStringLiteral("57"));
				QCOMPARE(
				    QMudNativePluginRegistry::callLuaAudio(&runtime, QStringLiteral("init"), {}).errorCode,
				    eOK);
				QCOMPARE(state.volumeByBuffer.value(1), 57.0);
				QCOMPARE(
				    QMudNativePluginRegistry::callLuaAudio(&runtime, QStringLiteral("free"), {}).errorCode,
				    eOK);
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
				QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeMasterState(&runtime).volume, 57.0);
				QCOMPARE(QMudNativePluginRegistry::callLuaAudio(&runtime, QStringLiteral("playLooped"),
				                                                {QStringLiteral("coin.wav")})
				             .returnValues.front()
				             .toInt(),
				         0);
				QCOMPARE(
				    QMudNativePluginRegistry::callLuaAudio(&runtime, QStringLiteral("init"), {}).errorCode,
				    eOK);
				QCOMPARE(QMudNativePluginRegistry::callLuaAudio(&runtime, QStringLiteral("playLooped"),
				                                                {QStringLiteral("coin.wav")})
				             .returnValues.front()
				             .toInt(),
				         1);
				QCOMPARE(
				    QMudNativePluginRegistry::callLuaAudio(&runtime, QStringLiteral("free"), {}).errorCode,
				    eOK);
			}

			/** @brief Verifies default frequency updates preserve active voices and accepted reservations. */
			void luaAudioFrequencyControlsFuturePlays() const
			{
				using namespace QMudNativePluginRegistry;
				SoundBackendState state;
				WorldRuntime      runtime;
				installSoundBackend(runtime, state);
				QCOMPARE(callLuaAudio(&runtime, QStringLiteral("freq"), {3.9}).errorCode, eOK);
				QCOMPARE(luaAudioRuntimeMasterState(&runtime).pitch, 3.0);
				QCOMPARE(callLuaAudio(&runtime, QStringLiteral("play"), {QStringLiteral("coin.wav"), 0})
				             .returnValues.front()
				             .toInt(),
				         1);
				QCOMPARE(state.pitchByBuffer.value(1), 3.0);
				QCOMPARE(
				    callLuaAudio(&runtime, QStringLiteral("playDelay"), {QStringLiteral("coin.wav"), 60000})
				        .errorCode,
				    eOK);
				QCOMPARE(state.pitchByBuffer.value(2), 3.0);
				QCOMPARE(callLuaAudio(&runtime, QStringLiteral("pitch"), {8, 2}).errorCode, eOK);
				QCOMPARE(callLuaAudio(&runtime, QStringLiteral("freq"), {-7.9}).errorCode, eOK);
				QCOMPARE(luaAudioRuntimeMasterState(&runtime).pitch, -7.0);
				QCOMPARE(state.pitchByBuffer.value(1), 3.0);
				QCOMPARE(state.pitchByBuffer.value(2), 8.0);
				LuaAudioRuntimeBufferState reserved;
				QVERIFY(luaAudioRuntimeBufferState(&runtime, 2, reserved));
				QCOMPARE(reserved.pitch, 3.0);
				QVERIFY(reserved.pendingCancel && !reserved.pendingCancel->load());
				const auto pitches = state.pitchByBuffer;
				for (const QVector<QVariant> &arguments : {QVector<QVariant>{},
				                                           {QVariant{}},
				                                           {true},
				                                           {QStringLiteral("bad")},
				                                           {std::numeric_limits<double>::quiet_NaN()},
				                                           {std::numeric_limits<double>::infinity()},
				                                           {-std::numeric_limits<double>::infinity()}})
				{
					QCOMPARE(callLuaAudio(&runtime, QStringLiteral("freq"), arguments).errorCode,
					         eErrorCallingPluginRoutine);
					QCOMPARE(luaAudioRuntimeMasterState(&runtime).pitch, -7.0);
					QCOMPARE(state.pitchByBuffer, pitches);
				}
				QCOMPARE(callLuaAudio(&runtime, QStringLiteral("freq"), {20}, {},
				                      NativeCallExecutionMode::ValidateOnly)
				             .errorCode,
				         eOK);
				QCOMPARE(luaAudioRuntimeMasterState(&runtime).pitch, -7.0);
				QCOMPARE(callLuaAudio(&runtime, QStringLiteral("pitch"), {20}).errorCode,
				         eErrorCallingPluginRoutine);
				QCOMPARE(callLuaAudio(&runtime, QStringLiteral("setPitch"), {20}).errorCode,
				         eErrorCallingPluginRoutine);
				QCOMPARE(callLuaAudio(&runtime, QStringLiteral("pitch"), {20, 0}).errorCode, eOK);
				QCOMPARE(luaAudioRuntimeMasterState(&runtime).pitch, -7.0);
				QCOMPARE(callLuaAudio(&runtime, QStringLiteral("play"), {QStringLiteral("coin.wav"), 0})
				             .returnValues.front()
				             .toInt(),
				         3);
				QCOMPARE(state.pitchByBuffer.value(3), -7.0);
				QCOMPARE(state.pitchByBuffer.value(1), 3.0);
				QCOMPARE(state.pitchByBuffer.value(2), 8.0);
				QCOMPARE(callLuaAudio(&runtime, QStringLiteral("stop"), {0}).errorCode, eOK);
				QVERIFY(reserved.pendingCancel->load());
			}

			/** @brief Verifies shared primitive marshalling preserves Lua types, precision, and string bytes. */
			void callPluginPrimitiveValues() const
			{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
				LuaStateOwner lua(luaL_newstate());
				QVERIFY(lua.get());
				for (const QVariant &value : {QVariant{}, QVariant{QMetaType::fromType<QString>()}})
				{
					QVERIFY(!value.isValid() || value.isNull());
					QMudLuaSupport::pushCallPluginValue(lua.get(), value);
					QCOMPARE(lua_type(lua.get(), -1), LUA_TNIL);
					lua_pop(lua.get(), 1);
				}
				for (const bool value : {false, true})
				{
					QMudLuaSupport::pushCallPluginValue(lua.get(), value);
					QCOMPARE(lua_type(lua.get(), -1), LUA_TBOOLEAN);
					QCOMPARE(lua_toboolean(lua.get(), -1) != 0, value);
					lua_pop(lua.get(), 1);
				}
				const QVector<QVariant> integers{QVariant::fromValue<char>(12),
				                                 QVariant::fromValue<signed char>(-12),
				                                 QVariant::fromValue<short>(-123),
				                                 QVariant{-1234},
				                                 QVariant::fromValue<long>(-12345),
				                                 QVariant::fromValue<qlonglong>(-9007199254740993LL),
				                                 QVariant::fromValue<unsigned char>(250),
				                                 QVariant::fromValue<unsigned short>(60000),
				                                 QVariant::fromValue<unsigned int>(12345),
				                                 QVariant::fromValue<unsigned long>(123456),
				                                 QVariant::fromValue<qulonglong>(9007199254740993ULL)};
				for (const QVariant &value : integers)
				{
					QMudLuaSupport::pushCallPluginValue(lua.get(), value);
					QVERIFY(lua_isinteger(lua.get(), -1));
					QCOMPARE(lua_tointeger(lua.get(), -1), value.toLongLong());
					lua_pop(lua.get(), 1);
				}
				for (const QVariant &value :
				     {QVariant::fromValue<float>(1.25f), QVariant{0.0}, QVariant{-4.75}})
				{
					QMudLuaSupport::pushCallPluginValue(lua.get(), value);
					QCOMPARE(lua_type(lua.get(), -1), LUA_TNUMBER);
					QVERIFY(!lua_isinteger(lua.get(), -1));
					QCOMPARE(lua_tonumber(lua.get(), -1), value.toDouble());
					lua_pop(lua.get(), 1);
				}
				for (const QByteArray &bytes :
				     {QByteArray(""), QStringLiteral("ήχος").toUtf8(), QByteArray("a\0b", 3)})
				{
					QMudLuaSupport::pushCallPluginValue(lua.get(), QString::fromUtf8(bytes));
					QCOMPARE(lua_type(lua.get(), -1), LUA_TSTRING);
					size_t      length = 0;
					const char *text   = lua_tolstring(lua.get(), -1, &length);
					QCOMPARE(QByteArray(text, static_cast<qsizetype>(length)), bytes);
					lua_pop(lua.get(), 1);
				}
				QCOMPARE(lua_gettop(lua.get()), 0);
				QMudLuaSupport::pushCallPluginValue(lua.get(), QString{});
				QCOMPARE(lua_type(lua.get(), -1), LUA_TSTRING);
				QCOMPARE(lua_rawlen(lua.get(), -1), 0u);
#endif
			}

			/** @brief Supplies delayed wrapper cases across reservation outcomes and validation. */
			void luaAudioDelayedWrapperResults_data() const
			{
				QTest::addColumn<QString>("routine");
				QTest::addColumn<int>("delay");
				QTest::addColumn<QString>("outcome");
				for (const QString &routine :
				     {QStringLiteral("playDelay"), QStringLiteral("playDelayLooped")})
					for (const int delay : {0, 60000})
						for (const QString &outcome :
						     {QStringLiteral("accepted"), QStringLiteral("full"), QStringLiteral("disabled"),
						      QStringLiteral("empty"), QStringLiteral("validate")})
							QTest::newRow(
							    qPrintable(QStringLiteral("%1-%2-%3").arg(routine).arg(delay).arg(outcome)))
							    << routine << delay << outcome;
			}

			/** @brief Verifies canonical nil, runtime CallPlugin nil, and legacy PPI nil omission. */
			void luaAudioDelayedWrapperResults() const
			{
				using namespace QMudNativePluginRegistry;
				QFETCH(QString, routine);
				QFETCH(int, delay);
				QFETCH(QString, outcome);
				SoundBackendState state;
				WorldRuntime      runtime;
				installSoundBackend(runtime, state);
				if (outcome == QStringLiteral("full"))
					for (int buffer = 1; buffer <= WorldRuntime::kMaxSoundBuffers; ++buffer)
						state.statusByBuffer.insert(buffer, 1);
				if (outcome == QStringLiteral("disabled"))
					QCOMPARE(callLuaAudio(&runtime, QStringLiteral("free"), {}).errorCode, eOK);
				const auto    mode = outcome == QStringLiteral("validate")
				                         ? NativeCallExecutionMode::ValidateOnly
				                         : NativeCallExecutionMode::ExecuteSideEffects;
				const QString file =
				    outcome == QStringLiteral("empty") ? QStringLiteral("") : QStringLiteral("coin.wav");
				const auto result = callLuaAudio(&runtime, routine, {file, delay}, {}, mode);
				QCOMPARE(result.errorCode, eOK);
				QCOMPARE(result.returnValues.size(), 1);
				QVERIFY(!result.returnValues.front().isValid());
				const bool accepted = outcome == QStringLiteral("accepted");
				QCOMPARE(luaAudioRuntimeOwnedBuffers(&runtime).size(), accepted ? 1 : 0);
				QCOMPARE(callLuaAudio(&runtime, QStringLiteral("stop"), {0}).errorCode, eOK);
#ifdef QMUD_ENABLE_LUA_SCRIPTING
				const QString audioId    = luaAudioPluginId();
				const auto    context    = nativeCallContext(runtime, audioId);
				const QString serialized = QStringLiteral("{[1]=\"%1\",[2]=%2}").arg(file).arg(delay);
				const QString sentinel   = QStringLiteral("unchanged");
				runtime.setPluginVariableValue(audioId, luaAudioPpiReturnVariableName(), sentinel);
				const auto ppi = callRoutine(&runtime, audioId, QStringLiteral("PPI_%1_PPI_").arg(routine),
				                             {serialized}, context, mode);
				QCOMPARE(ppi.errorCode, eOK);
				QVERIFY(ppi.returnValues.isEmpty());
				QString stored;
				QVERIFY(runtime.findPluginVariable(audioId, luaAudioPpiReturnVariableName(), stored));
				QCOMPARE(stored,
				         mode == NativeCallExecutionMode::ValidateOnly ? sentinel : QStringLiteral("{}"));
				QCOMPARE(callLuaAudio(&runtime, QStringLiteral("stop"), {0}).errorCode, eOK);
				if (mode == NativeCallExecutionMode::ValidateOnly)
					return;
				LuaStateOwner lua(luaL_newstate());
				QVERIFY(lua.get());
				QMudLuaSupport::pushCallPluginValue(lua.get(), file);
				lua_pushinteger(lua.get(), delay);
				const int originalTop = lua_gettop(lua.get());
				QCOMPARE(runtime.callPluginLua(audioId, routine, lua.get(), 1), 2);
				QCOMPARE(lua_gettop(lua.get()), originalTop + 2);
				QCOMPARE(lua_type(lua.get(), -2), LUA_TNUMBER);
				QCOMPARE(lua_tonumber(lua.get(), -2), eOK);
				QCOMPARE(lua_type(lua.get(), -1), LUA_TNIL);
#endif
			}

			void luaAudioRuntimeStateIsSharedAndMutable() const
			{
				SoundBackendState soundState;
				WorldRuntime      runtime;
				const QString     soundRoot = testDirectory(QStringLiteral("runtime-state"));
				QVERIFY(!soundRoot.isEmpty());
				QVERIFY(writeSoundFixture(soundRoot));
				runtime.setStartupDirectory(soundRoot);
				installSoundBackend(runtime, soundState);

				const QString audioId = QMudNativePluginRegistry::luaAudioPluginId();
				const QMudNativePluginRegistry::NativeCallContext audioContext =
				    nativeCallContext(runtime, audioId);
				const QMudNativePluginRegistry::NativeCallResult delayedPlay =
				    QMudNativePluginRegistry::callRoutine(&runtime, audioId, QStringLiteral("playDelay"),
				                                          {QStringLiteral("coin.wav"), 60000.0, 0.0, 100.0},
				                                          audioContext);
				QCOMPARE(delayedPlay.errorCode, eOK);
				QCOMPARE(delayedPlay.returnValues.size(), 1);
				QVERIFY(!delayedPlay.returnValues.front().isValid());
				QCOMPARE(soundState.reservedFiles, QStringList{QStringLiteral("coin.wav")});
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
				QCOMPARE(soundState.volumeByBuffer.value(1), 25.0);
				QCOMPARE(soundState.panByBuffer.value(1), 3.0);
				QCOMPARE(soundState.pitchByBuffer.value(1), 7.0);

				const QMudNativePluginRegistry::NativeCallResult volume =
				    QMudNativePluginRegistry::callRoutine(&runtime, audioId, QStringLiteral("getVolume"), {1},
				                                          audioContext);
				QCOMPARE(volume.errorCode, eOK);
				QCOMPARE(volume.returnValues.size(), 1);
				QCOMPARE(volume.returnValues.constFirst().toDouble(), 25.0);

#ifdef QMUD_ENABLE_LUA_SCRIPTING
				const QMudNativePluginRegistry::NativeCallResult ppiVolume =
				    QMudNativePluginRegistry::callRoutine(&runtime, audioId,
				                                          QStringLiteral("PPI_getVolume_PPI_"),
				                                          {QStringLiteral("{[1]=1}")}, audioContext);
				QCOMPARE(ppiVolume.errorCode, eOK);
				QVERIFY(ppiVolume.returnValues.isEmpty());
				QString serializedPpiReturns;
				QVERIFY(runtime.findPluginVariable(audioId,
				                                   QMudNativePluginRegistry::luaAudioPpiReturnVariableName(),
				                                   serializedPpiReturns));
				QVERIFY(runtime.pluginVariableList(audioId).contains(
				    QMudNativePluginRegistry::luaAudioPpiReturnVariableName()));
				QCOMPARE(runtime.pluginInfo(audioId, 12).toInt(), 2);
				QVector<QVariant> decodedPpiReturns;
				QString           decodeError;
				QVERIFY2(QMudLuaSupport::decodePpiArguments(serializedPpiReturns, 1, decodedPpiReturns,
				                                            decodeError),
				         qPrintable(decodeError));
				QCOMPARE(decodedPpiReturns.size(), 1);
				QCOMPARE(decodedPpiReturns.constFirst().toDouble(), 25.0);
				QCOMPARE(QMudNativePluginRegistry::callRoutine(
				             &runtime, audioId, QStringLiteral("PPI_setVol_PPI_"),
				             {QStringLiteral("{[1]=31,[2]=1}")}, audioContext)
				             .errorCode,
				         eOK);
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeBufferState(&runtime, 1, bufferState));
				QCOMPARE(soundState.volumeByBuffer.value(1), 31.0);
				QCOMPARE(QMudNativePluginRegistry::callRoutine(
				             &runtime, audioId, QStringLiteral("PPI_setVol_PPI_"),
				             {QStringLiteral("not a serialized table")}, audioContext)
				             .errorCode,
				         eErrorCallingPluginRoutine);
				QCOMPARE(
				    QMudNativePluginRegistry::callRoutine(&runtime, audioId, QStringLiteral("PPI_stop_PPI_"),
				                                          {QStringLiteral("{[1]=1,[2]=10}")}, audioContext)
				        .errorCode,
				    eOK);
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
				QCOMPARE(QMudNativePluginRegistry::callRoutine(
				             &runtime, audioId, QStringLiteral("PPI_play_PPI_"),
				             {QStringLiteral("{[1]=\"coin.wav\",[2]=0,[3]=4,[4]=70}")}, audioContext)
				             .errorCode,
				         eOK);
				QVERIFY(runtime.findPluginVariable(audioId,
				                                   QMudNativePluginRegistry::luaAudioPpiReturnVariableName(),
				                                   serializedPpiReturns));
				QVERIFY2(QMudLuaSupport::decodePpiArguments(serializedPpiReturns, 1, decodedPpiReturns,
				                                            decodeError),
				         qPrintable(decodeError));
				QCOMPARE(decodedPpiReturns.size(), 1);
				QCOMPARE(decodedPpiReturns.constFirst().toInt(), 1);
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeBufferState(&runtime, 1, bufferState));
				QCOMPARE(bufferState.loop, false);
				QCOMPARE(bufferState.pan, 4.0);
				QCOMPARE(bufferState.volume, 70.0);
				QCOMPARE(QMudNativePluginRegistry::callRoutine(&runtime, audioId,
				                                               QStringLiteral("PPI_stop_PPI_"),
				                                               {QStringLiteral("{[1]=1}")}, audioContext)
				             .errorCode,
				         eOK);
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
#endif

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
				                                          {QStringLiteral("coin.wav"), 60000.0, 0.0, 33.0},
				                                          audioContext);
				QCOMPARE(resetPlay.errorCode, eOK);
				QCOMPARE(resetPlay.returnValues.size(), 1);
				QVERIFY(!resetPlay.returnValues.front().isValid());
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
				                                          {QStringLiteral("coin.wav"), 0, 0.0, 33.0},
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
				                                          {QStringLiteral("coin.wav"), 0, 0.0, 44.0},
				                                          audioContext);
				QCOMPARE(transientStoppedPlay.errorCode, eOK);
				QCOMPARE(transientStoppedPlay.returnValues.constFirst().toInt(), 2);
				QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).size(), 2);
				QMudNativePluginRegistry::luaAudioReleaseRuntimeBuffer(&runtime, 2);
				static_cast<void>(runtime.stopSoundDirect(2));
				soundState.reusableByBuffer.remove(1);

				QVERIFY(QMudNativePluginRegistry::luaAudioReleaseRuntimeBufferIfGeneration(
				    &runtime, 1, postResetState.generation));
				const QMudNativePluginRegistry::NativeCallResult reclaimedPlay =
				    QMudNativePluginRegistry::callRoutine(&runtime, audioId, QStringLiteral("play"),
				                                          {QStringLiteral("coin.wav"), 0, 0.0, 55.0},
				                                          audioContext);
				QCOMPARE(reclaimedPlay.errorCode, eOK);
				QCOMPARE(reclaimedPlay.returnValues.constFirst().toInt(), 1);
				QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).size(), 1);
				QMudNativePluginRegistry::LuaAudioRuntimeBufferState reclaimedState;
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeBufferState(&runtime, 1, reclaimedState));
				QCOMPARE(reclaimedState.volume, 55.0);
				QCOMPARE(soundState.statusByBuffer.value(1), 1);

				QMudNativePluginRegistry::luaAudioReleaseRuntimeBuffer(&runtime, 1);
				static_cast<void>(runtime.stopSoundDirect(1));
				const QMudNativePluginRegistry::NativeCallResult immediateReleasePlay =
				    QMudNativePluginRegistry::callRoutine(&runtime, audioId, QStringLiteral("play"),
				                                          {QStringLiteral("coin.wav"), 0, 0.0, 65.0},
				                                          audioContext);
				QCOMPARE(immediateReleasePlay.errorCode, eOK);
				QCOMPARE(immediateReleasePlay.returnValues.constFirst().toInt(), 1);
				QMudNativePluginRegistry::LuaAudioRuntimeBufferState immediateReleaseState;
				QVERIFY(
				    QMudNativePluginRegistry::luaAudioRuntimeBufferState(&runtime, 1, immediateReleaseState));
				QVERIFY(QMudNativePluginRegistry::luaAudioReleaseRuntimeBufferIfGeneration(
				    &runtime, 1, immediateReleaseState.generation));
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
			}

			/**
			 * @brief Verifies a new generation atomically removes every buffer-scoped artifact of its predecessor.
			 */
			void luaAudioGenerationReplacementClearsPriorBufferState() const
			{
				SoundBackendState soundState;
				WorldRuntime      runtime;
				installSoundBackend(runtime, soundState);
				WorldRuntime::Plugin audioPlugin;
				audioPlugin.enabled    = true;
				audioPlugin.nativeShim = true;
				audioPlugin.attributes.insert(QStringLiteral("id"),
				                              QMudNativePluginRegistry::luaAudioPluginId());
				WorldRuntimeTestAccess::plugins(runtime).push_back(audioPlugin);
				QVERIFY(QMudNativePluginRegistry::handleLuaAudioPlaySound(
				    &runtime, QStringLiteral("loop=prior-generation.wav")));

				QMudNativePluginRegistry::LuaAudioRuntimeBufferState priorState;
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeBufferState(&runtime, 1, priorState));
				auto pendingPlayCancel   = std::make_shared<std::atomic_bool>(false);
				priorState.pendingCancel = pendingPlayCancel;
				QMudNativePluginRegistry::luaAudioMarkRuntimeBuffer(&runtime, 1, priorState);
				QMudNativePluginRegistry::LuaAudioRuntimeOperationGuard firstGuard;
				QMudNativePluginRegistry::LuaAudioRuntimeOperationGuard secondGuard;
				QVERIFY(QMudNativePluginRegistry::luaAudioCaptureRuntimeBufferOperationGuard(&runtime, 1,
				                                                                             firstGuard));
				QVERIFY(QMudNativePluginRegistry::luaAudioCaptureRuntimeBufferOperationGuard(&runtime, 1,
				                                                                             secondGuard));
				QCOMPARE(firstGuard.generation, priorState.generation);
				QVERIFY(firstGuard.cancelled == secondGuard.cancelled);
				QVERIFY(!firstGuard.cancelled->load(std::memory_order_acquire));

				QMudNativePluginRegistry::LuaAudioRuntimeBufferState replacementState;
				replacementState.generation        = priorState.generation + 1;
				replacementState.playbackSubmitted = true;
				QMudNativePluginRegistry::luaAudioMarkRuntimeBuffer(&runtime, 1, replacementState);

				QVERIFY(pendingPlayCancel->load(std::memory_order_acquire));
				QVERIFY(firstGuard.cancelled->load(std::memory_order_acquire));
				QMudNativePluginRegistry::LuaAudioRuntimeBufferState guardedState;
				QVERIFY(!QMudNativePluginRegistry::luaAudioResolveRuntimeBufferOperation(
				    &runtime, 1, firstGuard, guardedState));
				QMudNativePluginRegistry::LuaAudioRuntimeOperationGuard replacementGuard;
				QVERIFY(QMudNativePluginRegistry::luaAudioCaptureRuntimeBufferOperationGuard(
				    &runtime, 1, replacementGuard));
				QCOMPARE(replacementGuard.generation, replacementState.generation);
				QVERIFY(replacementGuard.cancelled != firstGuard.cancelled);
				const int stopsBeforeOldLoopCommand = soundState.stopCalls;
				QVERIFY(QMudNativePluginRegistry::handleLuaAudioPlaySound(
				    &runtime, QStringLiteral("stop=prior-generation.wav")));
				QCOMPARE(soundState.stopCalls, stopsBeforeOldLoopCommand);
				QVERIFY(!QMudNativePluginRegistry::luaAudioReleaseRuntimeBufferIfGeneration(
				    &runtime, 1, priorState.generation));
				QMudNativePluginRegistry::LuaAudioRuntimeBufferState currentState;
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeBufferState(&runtime, 1, currentState));
				QCOMPARE(currentState.generation, replacementState.generation);
				priorState.volume = 12.0;
				QVERIFY(!QMudNativePluginRegistry::luaAudioUpdateRuntimeBufferIfGeneration(&runtime, 1,
				                                                                           priorState));
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeBufferState(&runtime, 1, currentState));
				QCOMPARE(currentState.generation, replacementState.generation);
				QCOMPARE(currentState.volume, replacementState.volume);
				QMudNativePluginRegistry::luaAudioReleaseRuntimeBuffer(&runtime, 1);
				QVERIFY(replacementGuard.cancelled->load(std::memory_order_acquire));
				QVERIFY(!QMudNativePluginRegistry::luaAudioResolveRuntimeBufferOperation(
				    &runtime, 1, replacementGuard, guardedState));
				QVERIFY(!QMudNativePluginRegistry::luaAudioUpdateRuntimeBufferIfGeneration(&runtime, 1,
				                                                                           replacementState));
				QVERIFY(!QMudNativePluginRegistry::luaAudioRuntimeBufferState(&runtime, 1, currentState));
			}

			/** @brief Verifies non-finite legacy commands and master updates preserve defaults and existing voices. */
			void luaAudioRejectsNonFiniteMasterControls() const
			{
				SoundBackendState soundState;
				WorldRuntime      runtime;
				installSoundBackend(runtime, soundState);
				WorldRuntime::Plugin plugin;
				plugin.enabled    = true;
				plugin.nativeShim = true;
				plugin.attributes.insert(QStringLiteral("id"), QMudNativePluginRegistry::luaAudioPluginId());
				WorldRuntimeTestAccess::plugins(runtime).push_back(plugin);
				QVERIFY(
				    QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QStringLiteral("volume=45")));
				QVERIFY(
				    QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QStringLiteral("pan=15")));
				QVERIFY(
				    QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QStringLiteral("freq=3")));
				QVERIFY(QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime,
				                                                          QStringLiteral("loop=coin.wav")));
				const auto volumes = soundState.volumeByBuffer;
				const auto pitches = soundState.pitchByBuffer;
				for (const QString &value :
				     {QStringLiteral("nan"), QStringLiteral("inf"), QStringLiteral("-inf"),
				      QStringLiteral("1e999"), QStringLiteral("not-a-number")})
				{
					for (const QString &key :
					     {QStringLiteral("volume"), QStringLiteral("pan"), QStringLiteral("freq")})
						QVERIFY(QMudNativePluginRegistry::handleLuaAudioPlaySound(
						    &runtime, key + QLatin1Char('=') + value));
				}
				for (const double invalid :
				     {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
				      -std::numeric_limits<double>::infinity()})
				{
					QMudNativePluginRegistry::luaAudioSetRuntimeMasterVolume(&runtime, invalid);
					QMudNativePluginRegistry::luaAudioSetRuntimeMasterPan(&runtime, invalid);
					QMudNativePluginRegistry::luaAudioSetRuntimeMasterPitch(&runtime, invalid);
					QMudNativePluginRegistry::luaAudioRestorePersistentVolume(&runtime, invalid);
					for (const bool invalidVolume : {false, true})
					{
						int completions = 0;
						QCOMPARE(runtime.playSoundInterceptedWithCompletion(
						             1, QStringLiteral("freq=8"), false, invalidVolume ? invalid : 0.0,
						             invalidVolume ? 0.0 : invalid,
						             [&](const int result)
						             {
							             ++completions;
							             QCOMPARE(result, eBadParameter);
						             }),
						         eBadParameter);
						QCOMPARE(completions, 1);
					}
				}
				const auto master = QMudNativePluginRegistry::luaAudioRuntimeMasterState(&runtime);
				QCOMPARE(master.volume, 45.0);
				QCOMPARE(master.pan, 15.0);
				QCOMPARE(master.pitch, 3.0);
				QCOMPARE(soundState.volumeByBuffer, volumes);
				QCOMPARE(soundState.pitchByBuffer, pitches);
				QCOMPARE(soundState.playCalls, 1);
				QCOMPARE(soundState.stopCalls, 0);
				QVERIFY(QMudNativePluginRegistry::handleLuaAudioCommand(&runtime,
				                                                        QStringLiteral("sound_toggle")));
				QVERIFY(QMudNativePluginRegistry::handleLuaAudioCommand(&runtime,
				                                                        QStringLiteral("sound_toggle")));
				QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeMasterState(&runtime).volume, 45.0);
				QVERIFY(QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QString()));
				const auto context = nativeCallContext(runtime, QMudNativePluginRegistry::luaAudioPluginId());
				const auto played  = QMudNativePluginRegistry::callRoutine(
				    &runtime, QMudNativePluginRegistry::luaAudioPluginId(), QStringLiteral("playlooped"),
				    {QStringLiteral("coin.wav")}, context);
				QCOMPARE(played.errorCode, eOK);
				QMudNativePluginRegistry::LuaAudioRuntimeBufferState voice;
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeBufferState(&runtime, 1, voice));
				QCOMPARE(voice.volume, 45.0);
				QCOMPARE(voice.pan, 15.0);
				QCOMPARE(voice.pitch, 3.0);
				QCOMPARE(soundState.pitchByBuffer.value(1), 3.0);
			}

			void luaAudioCommandsAndPlaySoundCallbackMirrorLegacyPlugin() const
			{
				SoundBackendState soundState;
				WorldRuntime      runtime;
				const QString     soundRoot = testDirectory(QStringLiteral("commands-and-play-sound"));
				QVERIFY(!soundRoot.isEmpty());
				QVERIFY(writeSoundFixture(soundRoot));
				runtime.setStartupDirectory(soundRoot);
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
				WorldRuntimeTestAccess::plugins(runtime).push_back(audioPlugin);
				QVERIFY(
				    !QMudNativePluginRegistry::handleLuaAudioCommand(&runtime, QStringLiteral("LuaAudio")));

				WorldRuntimeTestAccess::plugins(runtime).back().enabled = true;

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
				    QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QStringLiteral("coin.wav")));
				QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).size(), 1);
				QCOMPARE(runtime.soundStatus(1), 1);
				QCOMPARE(soundState.playCalls, 2);
				soundState.failingPlayFiles.clear();
				QVERIFY(QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QString()));
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
				QCOMPARE(runtime.soundStatus(1), -2);
				QCOMPARE(soundState.stopCalls, 1);

				QVERIFY(QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime,
				                                                          QStringLiteral("loop=coin.wav")));
				QMudNativePluginRegistry::LuaAudioRuntimeBufferState loopState;
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeBufferState(&runtime, 1, loopState));
				QVERIFY(QMudNativePluginRegistry::luaAudioReleaseRuntimeBufferIfGeneration(
				    &runtime, 1, loopState.generation));
				QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
				QVERIFY(QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime,
				                                                          QStringLiteral("stop=coin.wav")));
				QCOMPARE(soundState.stopCalls, 1);
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

				QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(
				    &runtime, QStringLiteral("subst add muted==!skip")));
				events.clear();
				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0, QStringLiteral("muted"));
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

				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0,
				                                                     QStringLiteral("<prompt> "));
				QCOMPARE(events.size(), 3);
				QCOMPARE(events.at(2).text, QStringLiteral("<prompt> "));
				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0,
				                                                     QStringLiteral("<prompt> "));
				QCOMPARE(events.size(), 4);
				QCOMPARE(events.at(3).text, QStringLiteral("<prompt> "));

				QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("tts")));
				QVERIFY(!QMudNativePluginRegistry::isMushReaderSpeechEnabled(&runtime));
				QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("tts")));
				QVERIFY(QMudNativePluginRegistry::isMushReaderSpeechEnabled(&runtime));
				events.clear();
				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0,
				                                                     QStringLiteral("<enabled> "));
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.constLast().text, QStringLiteral("<enabled> "));

				QMudNativePluginRegistry::setMushReaderPluginEnabled(&runtime, false);
				QMudNativePluginRegistry::setMushReaderPluginEnabled(&runtime, true);
				events.clear();
				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0,
				                                                     QStringLiteral("<reenabled> "));
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.constLast().text, QStringLiteral("<reenabled> "));

				QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("tts")));
				QCOMPARE(events.size(), 3);
				QVERIFY(events.at(1).stop);
				QCOMPARE(events.at(2).text, QStringLiteral("speech off"));
				QVERIFY(events.at(2).interrupt);

				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0, QStringLiteral("muted"));
				QCOMPARE(events.size(), 3);
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
				WorldRuntimeTestAccess::plugins(runtime).push_back(mushReaderPlugin);
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
				const QStringList blacklistIds{
				    QStringLiteral("bb6a05ed7534b5db1ed40511"), QStringLiteral("b8e6dac1ee7fe8e3de931fb7"),
				    QStringLiteral("8238deec7c06bade8ebc3819"), QStringLiteral("7da63d78d9f91bebb5285127"),
				    QStringLiteral("1df4fa3b486a36ededa19433")};
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
					const QString parsedId = doc.plugins().front().attributes.value(QStringLiteral("id"));
					QCOMPARE(parsedId, id);
					QVERIFY(QMudNativePluginRegistry::isProtectedId(parsedId));
					if (QMudNativePluginRegistry::isBlacklistedId(id))
					{
						WorldRuntime runtime;
						runtime.setStartupDirectory(directory);
						QString error;
						QVERIFY2(runtime.loadPluginFile(path, &error), qPrintable(error));
						QVERIFY(!runtime.isPluginInstalled(id));
					}
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

#include "tst_NativePluginRegistry.moc"
