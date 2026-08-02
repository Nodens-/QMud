/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_NativePluginRegistry.cpp
 * Role: Unit coverage for native compatibility plugin registry behavior.
 */

#include "NativePluginRegistry.h"
#include "TelnetProcessor.h"
#include "WorldDocument.h"
#include "WorldOptions.h"
#include "WorldRuntime.h"
#include "scripting/ScriptingErrors.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryDir>
#include <QtTest/QTest>

namespace
{
	struct RuntimeStubState
	{
			QString                     startupDirectory;
			QStringList                 outputLines;
			QList<WorldRuntime::Plugin> plugins;
			QHash<int, int>             soundStatusByBuffer;
			QHash<int, bool>            reusableByBuffer;
			QStringList                 failingDirectPlayFiles;
			int                         publicPlaySoundCalls{0};
			int                         directPlaySoundCalls{0};
			int                         publicStopSoundCalls{0};
			int                         directStopSoundCalls{0};
			int                         nextAcceleratorCommand{WorldRuntime::kAcceleratorFirstCommand};
			QHash<qint64, int>          acceleratorKeyToCommand;
			QHash<int, QString>         acceleratorTextByCommand;
			QHash<int, int>             acceleratorSendToByCommand;
			QHash<int, QString>         acceleratorPluginByCommand;
			bool                        directPlayRequiresLuaAudioState{false};
			bool                        directPlayReleasesLuaAudioState{false};
			bool                        sawLuaAudioStateDuringDirectPlay{false};
	};

	QHash<const WorldRuntime *, RuntimeStubState> &runtimeStates()
	{
		static QHash<const WorldRuntime *, RuntimeStubState> states;
		return states;
	}

	RuntimeStubState &stateFor(const WorldRuntime *runtime)
	{
		return runtimeStates()[runtime];
	}

	QMudNativePluginRegistry::NativeCallContext nativeCallContext(const QString &pluginId,
	                                                              const bool     pluginEnabled       = true,
	                                                              const bool mushReaderSpeechEnabled = true)
	{
		QMudNativePluginRegistry::NativeCallContext context;
		context.installed               = true;
		context.pluginEnabled           = pluginEnabled;
		context.mushReaderSpeechEnabled = mushReaderSpeechEnabled;
		QMudNativePluginRegistry::NativePluginMetadata metadata;
		if (QMudNativePluginRegistry::metadataForShim(pluginId, metadata))
			context.pluginName = metadata.name;
		return context;
	}

	QMudNativePluginRegistry::NativeCallContext disabledNativeCallContext(const QString &pluginId)
	{
		return nativeCallContext(pluginId, false);
	}

	QString writePluginXml(const QTemporaryDir &dir, const QString &id, const QString &script)
	{
		QString path = dir.filePath(QStringLiteral("%1.xml").arg(id));
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

// NOLINTBEGIN(readability-convert-member-functions-to-static,readability-make-member-function-const)
TelnetProcessor::TelnetProcessor() = default;

WorldRuntime::WorldRuntime(QObject *parent) : QObject(parent)
{
	RuntimeStubState state;
	state.startupDirectory = QDir::currentPath();
	runtimeStates().insert(this, state);
}

WorldRuntime::~WorldRuntime()
{
	QMudNativePluginRegistry::discardRuntimeState(this);
	runtimeStates().remove(this);
}

QString WorldRuntime::startupDirectory() const
{
	return stateFor(this).startupDirectory;
}

void WorldRuntime::outputText(const QString &text, bool, bool)
{
	stateFor(this).outputLines.push_back(text);
}

void WorldRuntime::notifyNativePluginStateChanged()
{
}

int WorldRuntime::allocateAcceleratorCommand()
{
	return stateFor(this).nextAcceleratorCommand++;
}

void WorldRuntime::registerAccelerator(const qint64 key, const int commandId, const AcceleratorEntry &entry)
{
	RuntimeStubState &state = stateFor(this);
	state.acceleratorKeyToCommand.insert(key, commandId);
	state.acceleratorTextByCommand.insert(commandId, entry.text);
	state.acceleratorSendToByCommand.insert(commandId, entry.sendTo);
	state.acceleratorPluginByCommand.insert(commandId, entry.pluginId);
}

int WorldRuntime::acceleratorCommandForKey(const qint64 key) const
{
	return stateFor(this).acceleratorKeyToCommand.value(key, -1);
}

QString WorldRuntime::acceleratorCommandText(const int commandId) const
{
	return stateFor(this).acceleratorTextByCommand.value(commandId);
}

int WorldRuntime::acceleratorSendTarget(const int commandId) const
{
	return stateFor(this).acceleratorSendToByCommand.value(commandId, -1);
}

QString WorldRuntime::acceleratorPluginId(const int commandId) const
{
	return stateFor(this).acceleratorPluginByCommand.value(commandId);
}

int WorldRuntime::playSound(const int buffer, const QString &fileName, const bool loop, double, double)
{
	++stateFor(this).publicPlaySoundCalls;
	return playSoundBypassingPluginCallbacks(buffer, fileName, loop, 0.0, 0.0);
}

int WorldRuntime::playSoundBypassingPluginCallbacks(const int buffer, const QString &fileName,
                                                    const bool loop, double, double)
{
	++stateFor(this).directPlaySoundCalls;
	if (fileName.isEmpty())
	{
		if (!stateFor(this).soundStatusByBuffer.contains(buffer))
			return eBadParameter;
		stateFor(this).soundStatusByBuffer.insert(buffer, loop ? 2 : 1);
		return eOK;
	}
	if (stateFor(this).failingDirectPlayFiles.contains(fileName))
		return eCannotPlaySound;
	const int                                            targetBuffer = buffer > 0 ? buffer : 1;
	QMudNativePluginRegistry::LuaAudioRuntimeBufferState bufferState;
	const bool                                           hasLuaAudioState =
	    QMudNativePluginRegistry::luaAudioRuntimeBufferState(this, targetBuffer, bufferState);
	if (stateFor(this).directPlayRequiresLuaAudioState)
		stateFor(this).sawLuaAudioStateDuringDirectPlay = hasLuaAudioState;
	if (hasLuaAudioState && stateFor(this).directPlayReleasesLuaAudioState)
		QMudNativePluginRegistry::luaAudioReleaseRuntimeBufferIfGeneration(this, targetBuffer,
		                                                                   bufferState.generation);
	stateFor(this).soundStatusByBuffer.insert(targetBuffer, loop ? 2 : 1);
	return eOK;
}

int WorldRuntime::stopSound(const int buffer)
{
	++stateFor(this).publicStopSoundCalls;
	return stopSoundBypassingPluginCallbacks(buffer);
}

int WorldRuntime::stopSoundBypassingPluginCallbacks(const int buffer)
{
	++stateFor(this).directStopSoundCalls;
	RuntimeStubState &state = stateFor(this);
	if (buffer == 0)
		state.soundStatusByBuffer.clear();
	else
		state.soundStatusByBuffer.remove(buffer);
	return eOK;
}

int WorldRuntime::soundStatus(const int buffer) const
{
	if (buffer < 1 || buffer > WorldRuntime::kMaxSoundBuffers)
		return -1;
	return stateFor(this).soundStatusByBuffer.value(buffer, -2);
}

bool WorldRuntime::soundBufferReusableForNativeAudio(const int buffer) const
{
	if (buffer < 1 || buffer > WorldRuntime::kMaxSoundBuffers)
		return false;
	const RuntimeStubState &state = stateFor(this);
	if (state.reusableByBuffer.contains(buffer))
		return state.reusableByBuffer.value(buffer);
	const int status = state.soundStatusByBuffer.value(buffer, -2);
	return status == -2 || status == 0;
}

const QList<WorldRuntime::Plugin> &WorldRuntime::plugins() const
{
	return stateFor(this).plugins;
}

QList<WorldRuntime::Plugin> &WorldRuntime::pluginsMutable()
{
	return stateFor(this).plugins;
}

bool WorldRuntime::enablePlugin(const QString &pluginId, const bool enable)
{
	const QString shimId = QMudNativePluginRegistry::resolveShimIdOrName(pluginId);
	const QString id     = shimId.isEmpty() ? pluginId.trimmed().toLower() : shimId;
	for (Plugin &plugin : stateFor(this).plugins)
	{
		if (plugin.attributes.value(QStringLiteral("id")).compare(id, Qt::CaseInsensitive) != 0)
			continue;
		if (plugin.enabled == enable)
			return true;
		plugin.enabled = enable;
		plugin.attributes.insert(QStringLiteral("enabled"),
		                         enable ? QStringLiteral("1") : QStringLiteral("0"));
		if (plugin.nativeShim && id == QMudNativePluginRegistry::mushReaderPluginId())
			QMudNativePluginRegistry::setMushReaderPluginEnabled(this, enable);
		if (plugin.nativeShim && id == QMudNativePluginRegistry::luaAudioPluginId() && !enable)
			QMudNativePluginRegistry::luaAudioStopRuntime(this);
		return true;
	}
	return QMudNativePluginRegistry::isShimId(id);
}

bool WorldRuntime::hasMushReaderLiveSpeechOwner() const
{
	const QString mushReaderId = QMudNativePluginRegistry::mushReaderPluginId();
	for (const Plugin &plugin : stateFor(this).plugins)
	{
		if (plugin.attributes.value(QStringLiteral("id")).compare(mushReaderId, Qt::CaseInsensitive) == 0)
			return plugin.enabled;
	}
	return QMudNativePluginRegistry::isMushReaderPluginEnabled(this);
}

bool WorldRuntime::isQtAccessibilitySpeechEnabled() const
{
	return QMudNativePluginRegistry::isQtAccessibilitySpeechEnabled(this);
}

bool WorldRuntime::unloadPlugin(const QString &pluginId, QString *error)
{
	const QString     shimId = QMudNativePluginRegistry::resolveShimIdOrName(pluginId);
	const QString     id     = shimId.isEmpty() ? pluginId.trimmed().toLower() : shimId;
	RuntimeStubState &state  = stateFor(this);
	for (int i = 0; i < state.plugins.size(); ++i)
	{
		if (state.plugins.at(i).attributes.value(QStringLiteral("id")).compare(id, Qt::CaseInsensitive) != 0)
			continue;
		if (state.plugins.at(i).nativeShim && id == QMudNativePluginRegistry::mushReaderPluginId())
			QMudNativePluginRegistry::setMushReaderPluginEnabled(this, false);
		if (state.plugins.at(i).nativeShim && id == QMudNativePluginRegistry::luaAudioPluginId())
			QMudNativePluginRegistry::luaAudioStopRuntime(this);
		state.plugins.removeAt(i);
		return true;
	}
	if (QMudNativePluginRegistry::isShimId(id))
		return true;
	if (error)
		*error = QStringLiteral("Plugin not found");
	return false;
}
// NOLINTEND(readability-convert-member-functions-to-static,readability-make-member-function-const)

/**
 * @brief QTest fixture covering native plugin registry shim and blacklist rules.
 */
class tst_NativePluginRegistry : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
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
			QCOMPARE(QMudNativePluginRegistry::resolveShimIdOrName(QStringLiteral("LuaAudio")), audioShimId);

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
			QCOMPARE(QMudNativePluginRegistry::pluginSupports(shimId, QStringLiteral("plugin_update_url")),
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
			QCOMPARE(QMudNativePluginRegistry::pluginSupports(audioShimId, QStringLiteral("setVol")), eOK);
			QCOMPARE(
			    QMudNativePluginRegistry::pluginSupports(audioShimId, QStringLiteral("plugin_update_url")),
			    eOK);

			QCOMPARE(
			    QMudNativePluginRegistry::normalizeNativeSource(QStringLiteral("qmud:native/MushReader")),
			    QStringLiteral("qmud:native/MushReader"));
			QCOMPARE(
			    QMudNativePluginRegistry::normalizeNativeSource(QStringLiteral("./qmud:native/LuaAudio")),
			    QStringLiteral("qmud:native/LuaAudio"));
			QCOMPARE(
			    QMudNativePluginRegistry::normalizeNativeSource(QStringLiteral(R"(qmud:native\MushReader)")),
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

		void callPluginRoutesToNativeSpeech()
		{
			WorldRuntime                                       runtime;
			QVector<QMudNativePluginRegistry::TestSpeechEvent> events;
			QMudNativePluginRegistry::setTestSpeechSink(
			    [&events](const QMudNativePluginRegistry::TestSpeechEvent &event)
			    { events.push_back(event); });
			const auto restoreSpeechSink =
			    qScopeGuard([] { QMudNativePluginRegistry::setTestSpeechSink({}); });

			const QString shimId = QMudNativePluginRegistry::mushReaderPluginId();
			const QMudNativePluginRegistry::NativeCallContext mushReaderContext = nativeCallContext(shimId);
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
			const QMudNativePluginRegistry::NativeCallResult update = QMudNativePluginRegistry::callRoutine(
			    &runtime, shimId, QStringLiteral("plugin_update_url"), {}, mushReaderContext);
			QCOMPARE(update.errorCode, eOK);
			QCOMPARE(update.returnValues.size(), 1);
			QCOMPARE(update.returnValues.constFirst().toString(), QStringLiteral("qmud:native/MushReader"));
			const QMudNativePluginRegistry::NativeCallResult audioUpdate =
			    QMudNativePluginRegistry::callRoutine(
			        &runtime, QMudNativePluginRegistry::luaAudioPluginId(),
			        QStringLiteral("plugin_update_url"), {},
			        nativeCallContext(QMudNativePluginRegistry::luaAudioPluginId()));
			QCOMPARE(audioUpdate.errorCode, eOK);
			QCOMPARE(audioUpdate.returnValues.size(), 1);
			QCOMPARE(audioUpdate.returnValues.constFirst().toString(),
			         QStringLiteral("qmud:native/LuaAudio"));
			QTemporaryDir soundRoot;
			QVERIFY(soundRoot.isValid());
			QVERIFY(QDir(soundRoot.path()).mkpath(QStringLiteral("sounds")));
			QFile soundFile(QDir(soundRoot.path()).filePath(QStringLiteral("sounds/coin.wav")));
			QVERIFY(soundFile.open(QIODevice::WriteOnly));
			soundFile.write("RIFF");
			soundFile.close();
			stateFor(&runtime).startupDirectory = soundRoot.path();
			const QMudNativePluginRegistry::NativeCallContext audioContext =
			    nativeCallContext(QMudNativePluginRegistry::luaAudioPluginId());
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
			QCOMPARE(QMudNativePluginRegistry::callRoutine(&runtime, shimId, QStringLiteral("missing"), {},
			                                               mushReaderContext)
			             .errorCode,
			         eNoSuchRoutine);

			events.clear();
			const QMudNativePluginRegistry::NativeCallContext mutedContext =
			    nativeCallContext(shimId, true, false);
			QCOMPARE(QMudNativePluginRegistry::callRoutine(&runtime, shimId, QStringLiteral("say"),
			                                               {QStringLiteral("muted")}, mutedContext)
			             .errorCode,
			         eOK);
			QVERIFY(events.isEmpty());

			const QMudNativePluginRegistry::NativeCallResult disabledCall =
			    QMudNativePluginRegistry::callRoutine(&runtime, shimId, QStringLiteral("say"),
			                                          {QStringLiteral("disabled")},
			                                          disabledNativeCallContext(shimId));
			QCOMPARE(disabledCall.errorCode, ePluginDisabled);
			QVERIFY(disabledCall.errorText.contains(QStringLiteral("disabled")));
			QVERIFY(events.isEmpty());
		}

		void luaAudioRuntimeStateIsSharedAndMutable()
		{
			WorldRuntime  runtime;
			QTemporaryDir soundRoot;
			QVERIFY(soundRoot.isValid());
			QVERIFY(QDir(soundRoot.path()).mkpath(QStringLiteral("sounds")));
			QFile soundFile(QDir(soundRoot.path()).filePath(QStringLiteral("sounds/coin.wav")));
			QVERIFY(soundFile.open(QIODevice::WriteOnly));
			soundFile.write("RIFF");
			soundFile.close();
			stateFor(&runtime).startupDirectory = soundRoot.path();

			const QString audioId = QMudNativePluginRegistry::luaAudioPluginId();
			const QMudNativePluginRegistry::NativeCallContext audioContext = nativeCallContext(audioId);
			const QMudNativePluginRegistry::NativeCallResult  delayedPlay =
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

			const QMudNativePluginRegistry::NativeCallResult volume = QMudNativePluginRegistry::callRoutine(
			    &runtime, audioId, QStringLiteral("getVolume"), {1}, audioContext);
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
			const QMudNativePluginRegistry::NativeCallResult postResetPlay =
			    QMudNativePluginRegistry::callRoutine(&runtime, audioId, QStringLiteral("play"),
			                                          {QStringLiteral("coin.wav"), false, 0.0, 33.0},
			                                          audioContext);
			QCOMPARE(postResetPlay.errorCode, eOK);
			QCOMPARE(postResetPlay.returnValues.constFirst().toInt(), 1);
			QTest::qWait(60);
			QMudNativePluginRegistry::LuaAudioRuntimeBufferState postResetState;
			QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeBufferState(&runtime, 1, postResetState));
			QCOMPARE(postResetState.volume, 33.0);
			QCOMPARE(stateFor(&runtime).soundStatusByBuffer.value(1), 1);

			stateFor(&runtime).soundStatusByBuffer.insert(1, 0);
			stateFor(&runtime).reusableByBuffer.insert(1, false);
			const QMudNativePluginRegistry::NativeCallResult transientStoppedPlay =
			    QMudNativePluginRegistry::callRoutine(&runtime, audioId, QStringLiteral("play"),
			                                          {QStringLiteral("coin.wav"), false, 0.0, 44.0},
			                                          audioContext);
			QCOMPARE(transientStoppedPlay.errorCode, eOK);
			QCOMPARE(transientStoppedPlay.returnValues.constFirst().toInt(), 2);
			QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).size(), 2);
			QMudNativePluginRegistry::luaAudioReleaseRuntimeBuffer(&runtime, 2);
			static_cast<void>(runtime.stopSoundBypassingPluginCallbacks(2));
			stateFor(&runtime).reusableByBuffer.remove(1);

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
			QCOMPARE(stateFor(&runtime).soundStatusByBuffer.value(1), 1);

			QMudNativePluginRegistry::luaAudioReleaseRuntimeBuffer(&runtime, 1);
			static_cast<void>(runtime.stopSoundBypassingPluginCallbacks(1));
			stateFor(&runtime).directPlayRequiresLuaAudioState = true;
			stateFor(&runtime).directPlayReleasesLuaAudioState = true;
			const QMudNativePluginRegistry::NativeCallResult immediateReleasePlay =
			    QMudNativePluginRegistry::callRoutine(&runtime, audioId, QStringLiteral("play"),
			                                          {QStringLiteral("coin.wav"), false, 0.0, 65.0},
			                                          audioContext);
			QCOMPARE(immediateReleasePlay.errorCode, eOK);
			QCOMPARE(immediateReleasePlay.returnValues.constFirst().toInt(), 1);
			QVERIFY(stateFor(&runtime).sawLuaAudioStateDuringDirectPlay);
			QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
		}

		void luaAudioCommandsAndPlaySoundCallbackMirrorLegacyPlugin()
		{
			WorldRuntime  runtime;
			QTemporaryDir soundRoot;
			QVERIFY(soundRoot.isValid());
			QVERIFY(QDir(soundRoot.path()).mkpath(QStringLiteral("sounds")));
			QFile soundFile(QDir(soundRoot.path()).filePath(QStringLiteral("sounds/coin.wav")));
			QVERIFY(soundFile.open(QIODevice::WriteOnly));
			soundFile.write("RIFF");
			soundFile.close();
			stateFor(&runtime).startupDirectory = soundRoot.path();

			QVERIFY(
			    !QMudNativePluginRegistry::handleLuaAudioCommand(&runtime, QStringLiteral("LuaAudio help")));
			QVERIFY(!QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QStringLiteral("pan=15")));
			QVERIFY(stateFor(&runtime).outputLines.isEmpty());
			QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeMasterState(&runtime).pan, 0.0);

			WorldRuntime::Plugin audioPlugin;
			audioPlugin.enabled    = false;
			audioPlugin.nativeShim = true;
			audioPlugin.attributes.insert(QStringLiteral("id"), QMudNativePluginRegistry::luaAudioPluginId());
			stateFor(&runtime).plugins.push_back(audioPlugin);
			QVERIFY(!QMudNativePluginRegistry::handleLuaAudioCommand(&runtime, QStringLiteral("LuaAudio")));

			stateFor(&runtime).plugins.back().enabled = true;

			QVERIFY(
			    QMudNativePluginRegistry::handleLuaAudioCommand(&runtime, QStringLiteral("LuaAudio help")));
			QVERIFY(stateFor(&runtime).outputLines.constLast().contains(QStringLiteral("LuaAudio")));

			QVERIFY(QMudNativePluginRegistry::handleLuaAudioCommand(&runtime, QStringLiteral("volume_down")));
			QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeMasterState(&runtime).volume, 95.0);
			QVERIFY(
			    QMudNativePluginRegistry::handleLuaAudioCommand(&runtime, QStringLiteral("sound_toggle")));
			QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeMasterState(&runtime).volume, 0.0);
			QVERIFY(
			    QMudNativePluginRegistry::handleLuaAudioCommand(&runtime, QStringLiteral("sound_toggle")));
			QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeMasterState(&runtime).volume, 95.0);

			QVERIFY(QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QStringLiteral("pan=15")));
			QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeMasterState(&runtime).pan, 15.0);
			QVERIFY(QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QStringLiteral("volume=45")));
			QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeMasterState(&runtime).volume, 45.0);
			QVERIFY(QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QStringLiteral("freq=3")));
			QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeMasterState(&runtime).pitch, 3.0);

			QVERIFY(QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QStringLiteral("coin.wav")));
			QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).size(), 1);
			QCOMPARE(runtime.soundStatus(1), 1);
			QCOMPARE(stateFor(&runtime).publicPlaySoundCalls, 0);
			QCOMPARE(stateFor(&runtime).directPlaySoundCalls, 1);
			stateFor(&runtime).failingDirectPlayFiles.push_back(QStringLiteral("coin.wav"));
			QVERIFY(!QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QStringLiteral("coin.wav")));
			QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).size(), 1);
			QCOMPARE(runtime.soundStatus(1), 1);
			QCOMPARE(stateFor(&runtime).directPlaySoundCalls, 2);
			stateFor(&runtime).failingDirectPlayFiles.clear();
			QVERIFY(QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QString()));
			QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
			QCOMPARE(runtime.soundStatus(1), -2);
			QCOMPARE(stateFor(&runtime).publicStopSoundCalls, 0);
			QCOMPARE(stateFor(&runtime).directStopSoundCalls, 1);

			stateFor(&runtime).directPlayRequiresLuaAudioState  = true;
			stateFor(&runtime).directPlayReleasesLuaAudioState  = true;
			stateFor(&runtime).sawLuaAudioStateDuringDirectPlay = false;
			QVERIFY(
			    QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QStringLiteral("loop=coin.wav")));
			QVERIFY(stateFor(&runtime).sawLuaAudioStateDuringDirectPlay);
			QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
			QVERIFY(
			    QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QStringLiteral("stop=coin.wav")));
			QCOMPARE(stateFor(&runtime).directStopSoundCalls, 1);
			stateFor(&runtime).directPlayRequiresLuaAudioState = false;
			stateFor(&runtime).directPlayReleasesLuaAudioState = false;
			stateFor(&runtime).soundStatusByBuffer.remove(1);

			QVERIFY(
			    QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QStringLiteral("loop=coin.wav")));
			QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).size(), 1);
			QCOMPARE(runtime.soundStatus(1), 2);
			QVERIFY(
			    QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QStringLiteral("stop=coin.wav")));
			QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
			QCOMPARE(runtime.soundStatus(1), -2);
			QCOMPARE(stateFor(&runtime).publicStopSoundCalls, 0);
			QCOMPARE(stateFor(&runtime).directStopSoundCalls, 2);

			QVERIFY(
			    QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QStringLiteral("loop=coin.wav")));
			QVERIFY(!QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
			QVERIFY(runtime.enablePlugin(QMudNativePluginRegistry::luaAudioPluginId(), false));
			QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
			QCOMPARE(runtime.soundStatus(1), -2);
			QCOMPARE(stateFor(&runtime).publicStopSoundCalls, 0);
			QCOMPARE(stateFor(&runtime).directStopSoundCalls, 3);
			QVERIFY(
			    !QMudNativePluginRegistry::handleLuaAudioCommand(&runtime, QStringLiteral("LuaAudio help")));

			QVERIFY(runtime.enablePlugin(QMudNativePluginRegistry::luaAudioPluginId(), true));
			QVERIFY(
			    QMudNativePluginRegistry::handleLuaAudioPlaySound(&runtime, QStringLiteral("loop=coin.wav")));
			QVERIFY(!QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
			QString unloadError;
			QVERIFY(runtime.unloadPlugin(QMudNativePluginRegistry::luaAudioPluginId(), &unloadError));
			QVERIFY(unloadError.isEmpty());
			QVERIFY(stateFor(&runtime).plugins.isEmpty());
			QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
			QCOMPARE(runtime.soundStatus(1), -2);
			QCOMPARE(stateFor(&runtime).publicStopSoundCalls, 0);
			QCOMPARE(stateFor(&runtime).directStopSoundCalls, 4);
			QVERIFY(
			    !QMudNativePluginRegistry::handleLuaAudioCommand(&runtime, QStringLiteral("LuaAudio help")));
		}

		void commandFallbackAndSubstitutionBehavior()
		{
			QTemporaryDir dir;
			QVERIFY(dir.isValid());
			WorldRuntime runtime;
			stateFor(&runtime).startupDirectory = dir.path();

			QVector<QMudNativePluginRegistry::TestSpeechEvent> events;
			QMudNativePluginRegistry::setTestSpeechSink(
			    [&events](const QMudNativePluginRegistry::TestSpeechEvent &event)
			    { events.push_back(event); });
			const auto restoreSpeechSink =
			    qScopeGuard([] { QMudNativePluginRegistry::setTestSpeechSink({}); });
			QMudNativePluginRegistry::setMushReaderPluginEnabled(&runtime, true);

			QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime,
			                                                          QStringLiteral("tts_note hello")));
			QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime,
			                                                          QStringLiteral("tts_interrupt now")));
			QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("tts_stop")));
			QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime,
			                                                          QStringLiteral("MushReader help")));
			QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(
			    &runtime, QStringLiteral("subst add source line==replacement line")));

			QCOMPARE(events.size(), 3);
			QCOMPARE(events.at(0).text, QStringLiteral("hello"));
			QVERIFY(!events.at(0).interrupt);
			QCOMPARE(events.at(1).text, QStringLiteral("now"));
			QVERIFY(events.at(1).interrupt);
			QVERIFY(events.at(2).stop);
			QVERIFY(stateFor(&runtime).outputLines.contains(QStringLiteral("MushReader native QMud shim.")));
			QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("substitutions.mush"))));

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
			QMudNativePluginRegistry::handleMushReaderPartialLine(&runtime, QStringLiteral("source line"));
			QCOMPARE(events.size(), 1);
			QCOMPARE(events.constFirst().text, QStringLiteral("replacement line"));

			QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(
			    &runtime, QStringLiteral("subst add muted==!skip")));
			events.clear();
			QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0, QStringLiteral("muted"));
			QMudNativePluginRegistry::handleMushReaderPartialLine(&runtime, QStringLiteral("muted"));
			QVERIFY(events.isEmpty());

			QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("subst off")));
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

			QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 2, 0, QStringLiteral("ignored"));
			QVERIFY(events.isEmpty());
			QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0, QStringLiteral("spoken"));
			QVERIFY(events.isEmpty());
			QMudNativePluginRegistry::handleMushReaderPartialLine(&runtime, QStringLiteral("<prompt> "));
			QVERIFY(events.isEmpty());

			QMudNativePluginRegistry::handleMushReaderTabComplete(&runtime, QStringLiteral("north"));
			QVERIFY(events.isEmpty());

			QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("tts")));
			QVERIFY(!QMudNativePluginRegistry::isQtAccessibilitySpeechEnabled(&runtime));
			QVERIFY(events.isEmpty());

			QMudNativePluginRegistry::setMushReaderPluginEnabled(&runtime, true);

			QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0, QStringLiteral("spoken"));
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
			QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0, QStringLiteral("<prompt> "));
			QCOMPARE(events.size(), 3);
			QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0, QStringLiteral("<prompt> "));
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
			QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0, QStringLiteral("<stale> "));
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
			QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0, QStringLiteral("<failed> "));
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

			QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("tts_stop")));
			QCOMPARE(events.size(), 2);
			QVERIFY(events.constLast().stop);

			events.clear();
			QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0,
			                                                     QStringLiteral("<stopped> "));
			QVERIFY(events.isEmpty());
		}

		void reviewSpeechUsesActiveMushReaderState()
		{
			QTemporaryDir dir;
			QVERIFY(dir.isValid());
			WorldRuntime runtime;
			stateFor(&runtime).startupDirectory = dir.path();

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
			QVERIFY(
			    QMudNativePluginRegistry::speakMushReaderReviewText(&runtime, QStringLiteral("review line")));
			QCOMPARE(events.size(), 1);
			QCOMPARE(events.constLast().text, QStringLiteral("review line"));
			QVERIFY(events.constLast().interrupt);

			QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(
			    &runtime, QStringLiteral("subst add review line==spoken review")));
			events.clear();
			QVERIFY(
			    QMudNativePluginRegistry::speakMushReaderReviewText(&runtime, QStringLiteral("review line")));
			QCOMPARE(events.size(), 1);
			QCOMPARE(events.constLast().text, QStringLiteral("spoken review"));
			QVERIFY(events.constLast().interrupt);

			QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(
			    &runtime, QStringLiteral("subst add quiet line==!skip")));
			events.clear();
			QVERIFY(
			    QMudNativePluginRegistry::speakMushReaderReviewText(&runtime, QStringLiteral("quiet line")));
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

			QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("tts_stop")));
			QCOMPARE(QMudNativePluginRegistry::mushReaderTestBackendCount(&runtime), std::size_t{0});
		}

		void runtimeSetupRegistersNativeAccelerator()
		{
			WorldRuntime runtime;
			QMudNativePluginRegistry::ensureMushReaderRuntimeSetup(&runtime);

			bool found = false;
			for (auto it = stateFor(&runtime).acceleratorKeyToCommand.constBegin();
			     it != stateFor(&runtime).acceleratorKeyToCommand.constEnd(); ++it)
			{
				const int commandId = it.value();
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
			stateFor(&runtime).plugins.push_back(mushReaderPlugin);
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

		void blacklistAndProtectedPluginXmlClassification()
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
				QCOMPARE(QMudNativePluginRegistry::pluginSupports(id, QStringLiteral("say")), eNoSuchPlugin);
			}

			QTemporaryDir dir;
			QVERIFY(dir.isValid());
			for (const QString &id :
			     blacklistIds + QStringList{QMudNativePluginRegistry::mushReaderPluginId(),
			                                QMudNativePluginRegistry::luaAudioPluginId()})
			{
				const QString path = writePluginXml(dir, id, QStringLiteral("package.loadlib('x','y')"));
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
};

QTEST_MAIN(tst_NativePluginRegistry)

#if __has_include("tst_NativePluginRegistry.moc")
#include "tst_NativePluginRegistry.moc"
#endif
