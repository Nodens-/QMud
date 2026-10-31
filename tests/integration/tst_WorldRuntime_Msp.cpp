/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldRuntime_Msp.cpp
 * Role: Integration coverage for native MSP negotiation, stream handling, and playback dispatch.
 */

#include "AcceptingWorldSocketService.h"
#include "AppController.h"
#include "SoundEngine.h"
#include "WorldRuntime.h"
#include "WorldRuntimeTestAccess.h"

#include <QtTest/QTest>

#include <utility>

/** @brief Provides focused access to application startup sound availability for MSP integration coverage. */
class AppControllerTestAccess final
{
	public:
		/**
		 * @brief Delivers a sound-initialization result through the production application connection.
		 * @param controller Application controller receiving the result.
		 * @param available Whether playback initialization succeeded.
		 */
		static void reportSoundInitialization(const AppController &controller, const bool available)
		{
			controller.m_soundEngine->playbackInitializationFinished(available);
		}

		/**
		 * @brief Applies application-owned execution capabilities to a runtime.
		 * @param controller Application controller supplying startup state.
		 * @param runtime Runtime being initialized.
		 */
		static void initializeRuntime(const AppController &controller, WorldRuntime *runtime)
		{
			controller.initializeWorldRuntime(runtime);
		}
};

namespace
{
	/** @brief Mutable sound state used to verify native MSP playback behavior. */
	struct SoundBackendState
	{
			QList<QMudNativeSoundRequest> requests;
	};

	/**
	 * @brief Installs a deterministic playback recorder at the runtime sound boundary.
	 * @param runtime Runtime receiving MSP input.
	 * @param state Playback state populated by sound operations.
	 */
	void installSoundBackend(WorldRuntime &runtime, SoundBackendState &state)
	{
		WorldRuntime::TestSoundBackend backend;
		backend.nativeSound = [&state](const QMudNativeSoundRequest &request)
		{ state.requests.push_back(request); };
		runtime.setSoundBackendForTest(std::move(backend));
	}

	/**
	 * @brief Installs an accepting transport for MSP negotiation and playback integration input.
	 * @param runtime Runtime representing the connected test session.
	 */
	void installMspTransport(WorldRuntime &runtime)
	{
		WorldRuntimeTestAccess::setSocket(runtime, new AcceptingWorldSocketService(&runtime));
		WorldRuntimeTestAccess::setConnectPhase(runtime, WorldRuntime::eConnectConnectedToMud);
	}

	/** @brief QTest fixture covering native MSP runtime behavior. */
	class tst_WorldRuntime_Msp : public QObject
	{
			Q_OBJECT

			// NOLINTBEGIN(readability-convert-member-functions-to-static)
		private slots:
			/** @brief Verifies native MSP consumes commands and controls independent sound channels. */
			void nativeMspUsesProtocolAndDirectSoundBoundaries()
			{
				WorldRuntime runtime;
				installMspTransport(runtime);
				runtime.setSoundAvailable(true);
				SoundBackendState soundState;
				installSoundBackend(runtime, soundState);

				QVERIFY(!runtime.isMspNegotiated());
				WorldRuntimeTestAccess::processRawDataPayload(runtime, QByteArray::fromHex("fffb5a"));
				QVERIFY(runtime.isMspNegotiated());

				int partialLines  = 0;
				int completeLines = 0;
				QObject::connect(&runtime, &WorldRuntime::incomingStyledLinePartialReceived, &runtime,
				                 [&partialLines](const QString &, const QVector<WorldRuntime::StyleSpan> &,
				                                 quint64, bool) { ++partialLines; });
				QObject::connect(&runtime, &WorldRuntime::incomingLineReceived, &runtime,
				                 [&completeLines](const QString &) { ++completeLines; });
				WorldRuntimeTestAccess::processRawDataPayload(runtime, QByteArrayLiteral("!!SO"));
				QCOMPARE(partialLines, 0);
				WorldRuntimeTestAccess::processRawDataPayload(
				    runtime, QByteArrayLiteral("UND(coin.wav V=40 P=60)\r\n"));
				QCOMPARE(soundState.requests.size(), 1);
				QCOMPARE(soundState.requests.constLast().filePattern, QStringLiteral("coin.wav"));
				QCOMPARE(soundState.requests.constLast().volume, 40);
				QCOMPARE(soundState.requests.constLast().priority, 60);
				QCOMPARE(completeLines, 0);
				WorldRuntimeTestAccess::processRawDataPayload(
				    runtime, QByteArrayLiteral("!!SOUND(coin.wav P=50)\r\n"));
				QCOMPARE(soundState.requests.size(), 2);
				WorldRuntimeTestAccess::processRawDataPayload(
				    runtime, QByteArrayLiteral("!!SOUND(coin.wav P=70)\r\n"));
				QCOMPARE(soundState.requests.size(), 3);
				WorldRuntimeTestAccess::processRawDataPayload(
				    runtime, QByteArrayLiteral("text !!SOUND(coin.wav C=1)\r\n"));
				QCOMPARE(soundState.requests.size(), 3);
				QCOMPARE(completeLines, 1);
				WorldRuntimeTestAccess::processRawDataPayload(
				    runtime, QByteArrayLiteral("!!MUSIC(coin.wav V=30 L=2)\r\n"));
				QCOMPARE(soundState.requests.size(), 4);
				WorldRuntimeTestAccess::processRawDataPayload(runtime, QByteArrayLiteral("!!MUSIC(Off)\r\n"));
				QCOMPARE(soundState.requests.size(), 5);
				QVERIFY(soundState.requests.constLast().stop);
				QCOMPARE(soundState.requests.constLast().media, QMudNativeSoundMedia::Music);
				WorldRuntimeTestAccess::processRawDataPayload(
				    runtime, QByteArrayLiteral("!!SOUND(Off U=https://audio.example/sounds/)\r\n"));
				QCOMPARE(runtime.mspDefaultSoundUrl(), QStringLiteral("https://audio.example/sounds/"));
				WorldRuntimeTestAccess::processRawDataPayload(runtime, QByteArrayLiteral("!!SOUND(Off)\r\n"));
				QCOMPARE(soundState.requests.size(), 6);
				QVERIFY(soundState.requests.constLast().stop);
				QCOMPARE(soundState.requests.constLast().media, QMudNativeSoundMedia::Sound);

				WorldRuntimeTestAccess::processRawDataPayload(runtime,
				                                              QByteArrayLiteral("!!SOUND(chime P=80)\r\n"));
				QCOMPARE(soundState.requests.size(), 7);
				QCOMPARE(soundState.requests.constLast().filePattern, QStringLiteral("chime"));
				WorldRuntimeTestAccess::processRawDataPayload(runtime, QByteArray::fromHex("fffc5a"));
				QVERIFY(!runtime.isMspNegotiated());
				WorldRuntimeTestAccess::processRawDataPayload(runtime,
				                                              QByteArrayLiteral("!!SOUND(coin.wav)\r\n"));
				QCOMPARE(soundState.requests.size(), 7);
				QCOMPARE(completeLines, 2);
			}

			/** @brief Verifies application startup failure prevents later runtimes from negotiating MSP. */
			void applicationSoundInitializationControlsMspAvailability()
			{
				AppController controller;
				AppControllerTestAccess::reportSoundInitialization(controller, false);
				WorldRuntime unavailableRuntime;
				installMspTransport(unavailableRuntime);
				AppControllerTestAccess::initializeRuntime(controller, &unavailableRuntime);
				WorldRuntimeTestAccess::processRawDataPayload(unavailableRuntime,
				                                              QByteArray::fromHex("fffb5a"));
				QVERIFY(!unavailableRuntime.isMspNegotiated());

				AppControllerTestAccess::reportSoundInitialization(controller, true);
				WorldRuntime availableRuntime;
				installMspTransport(availableRuntime);
				AppControllerTestAccess::initializeRuntime(controller, &availableRuntime);
				WorldRuntimeTestAccess::processRawDataPayload(availableRuntime,
				                                              QByteArray::fromHex("fffb5a"));
				QVERIFY(availableRuntime.isMspNegotiated());
			}

			/** @brief Verifies sound failure immediately withdraws active MSP on a connected world. */
			void soundFailureWithdrawsMspFromConnectedWorld()
			{
				WorldRuntime runtime;
				runtime.setSoundAvailable(true);
				auto *socket = new AcceptingWorldSocketService(&runtime);
				WorldRuntimeTestAccess::setSocket(runtime, socket);
				WorldRuntimeTestAccess::setConnectPhase(runtime, WorldRuntime::eConnectConnectedToMud);

				WorldRuntimeTestAccess::processRawDataPayload(runtime, QByteArray::fromHex("fffb5a"));
				QVERIFY(runtime.isMspNegotiated());
				QCOMPARE(socket->packets.constLast(), QByteArray::fromHex("fffd5a"));

				runtime.setSoundAvailable(false);
				QVERIFY(!runtime.isMspNegotiated());
				QCOMPARE(socket->packets.constLast(), QByteArray::fromHex("fffe5a"));
			}

			/** @brief Verifies embedded and multiple MSP commands are hidden and execute in stream order. */
			void nativeMspConsumesEmbeddedAndMultipleCommands()
			{
				WorldRuntime runtime;
				installMspTransport(runtime);
				runtime.setSoundAvailable(true);
				runtime.setWorldAttribute(QStringLiteral("enable_embedded_msp"), QStringLiteral("y"));
				SoundBackendState soundState;
				installSoundBackend(runtime, soundState);
				WorldRuntimeTestAccess::processRawDataPayload(runtime, QByteArray::fromHex("fffb5a"));

				QStringList                      displayedLines;
				QStringList                      partialLines;
				QVector<WorldRuntime::StyleSpan> displayedSpans;
				QObject::connect(&runtime, &WorldRuntime::incomingLineReceived, &runtime,
				                 [&displayedLines](const QString &line) { displayedLines.push_back(line); });
				QObject::connect(&runtime, &WorldRuntime::incomingStyledLineReceived, &runtime,
				                 [&displayedSpans](const QString &,
				                                   const QVector<WorldRuntime::StyleSpan> &spans, quint64,
				                                   bool) { displayedSpans = spans; });
				QObject::connect(&runtime, &WorldRuntime::incomingStyledLinePartialReceived, &runtime,
				                 [&partialLines](const QString &line,
				                                 const QVector<WorldRuntime::StyleSpan> &, quint64, bool)
				                 { partialLines.push_back(line); });

				WorldRuntimeTestAccess::processRawDataPayload(runtime,
				                                              QByteArrayLiteral("\x1B[31mbefore !!SO"));
				QCOMPARE(partialLines.constLast(), QStringLiteral("before "));
				QVERIFY(soundState.requests.isEmpty());
				WorldRuntimeTestAccess::processRawDataPayload(
				    runtime, QByteArrayLiteral("UND(one.wav V=40) middle \x1B[32m!!MUSIC(two.ogg) after"));
				QCOMPARE(partialLines.constLast(), QStringLiteral("before  middle  after"));
				QCOMPARE(soundState.requests.size(), 2);
				WorldRuntimeTestAccess::processRawDataPayload(runtime, QByteArrayLiteral("\r\n"));

				QCOMPARE(soundState.requests.size(), 2);
				QCOMPARE(soundState.requests.at(0).filePattern, QStringLiteral("one.wav"));
				QCOMPARE(soundState.requests.at(1).filePattern, QStringLiteral("two.ogg"));
				QCOMPARE(displayedLines, QStringList{QStringLiteral("before  middle  after")});
				int styledLength = 0;
				for (const WorldRuntime::StyleSpan &span : std::as_const(displayedSpans))
					styledLength += span.length;
				QCOMPARE(styledLength, displayedLines.constFirst().size());
			}

			/** @brief Verifies playback suppression preserves MSP parsing, negotiation, and URL state. */
			void disabledMspPlaybackConsumesCommandsWithoutStartingSounds()
			{
				WorldRuntime runtime;
				installMspTransport(runtime);
				runtime.setSoundAvailable(true);
				SoundBackendState soundState;
				installSoundBackend(runtime, soundState);
				WorldRuntimeTestAccess::processRawDataPayload(runtime, QByteArray::fromHex("fffb5a"));
				QVERIFY(runtime.isMspNegotiated());

				WorldRuntimeTestAccess::processRawDataPayload(runtime,
				                                              QByteArrayLiteral("!!SOUND(first.wav)\r\n"));
				QCOMPARE(soundState.requests.size(), 1);

				runtime.setWorldAttribute(QStringLiteral("disable_msp_playback"), QStringLiteral("y"));
				QCOMPARE(soundState.requests.size(), 3);
				QVERIFY(soundState.requests.at(1).stop);
				QVERIFY(soundState.requests.at(2).stop);
				QCOMPARE(soundState.requests.at(1).protocol, QMudNativeSoundProtocol::Msp);
				QCOMPARE(soundState.requests.at(2).protocol, QMudNativeSoundProtocol::Msp);

				WorldRuntimeTestAccess::processRawDataPayload(
				    runtime, QByteArrayLiteral("!!SOUND(suppressed.wav)\r\n"));
				QCOMPARE(soundState.requests.size(), 3);
				QVERIFY(runtime.isMspNegotiated());
				WorldRuntimeTestAccess::processRawDataPayload(
				    runtime, QByteArrayLiteral("!!SOUND(Off U=https://audio.example/sounds/)\r\n"));
				QCOMPARE(runtime.mspDefaultSoundUrl(), QStringLiteral("https://audio.example/sounds/"));
				QCOMPARE(soundState.requests.size(), 3);

				runtime.setWorldAttribute(QStringLiteral("disable_msp_playback"), QStringLiteral("n"));
				WorldRuntimeTestAccess::processRawDataPayload(runtime,
				                                              QByteArrayLiteral("!!SOUND(resumed.wav)\r\n"));
				QCOMPARE(soundState.requests.size(), 4);
				QCOMPARE(soundState.requests.constLast().filePattern, QStringLiteral("resumed.wav"));
			}

			/** @brief Verifies unsupported MSP extensions do not suppress otherwise valid playback. */
			void unknownMspParametersAreIgnoredForPlayback()
			{
				WorldRuntime runtime;
				installMspTransport(runtime);
				runtime.setSoundAvailable(true);
				SoundBackendState soundState;
				installSoundBackend(runtime, soundState);
				WorldRuntimeTestAccess::processRawDataPayload(runtime, QByteArray::fromHex("fffb5a"));

				WorldRuntimeTestAccess::processRawDataPayload(
				    runtime, QByteArrayLiteral("!!SOUND(extension.wav X=2.0 V=45)\r\n"));
				QCOMPARE(soundState.requests.size(), 1);
				QCOMPARE(soundState.requests.constLast().filePattern, QStringLiteral("extension.wav"));
				QCOMPARE(soundState.requests.constLast().volume, 45);
			}

			/** @brief Verifies MSP negotiation changes affect lines at their exact stream positions. */
			void nativeMspNegotiationAppliesInStreamOrder()
			{
				WorldRuntime runtime;
				installMspTransport(runtime);
				runtime.setSoundAvailable(true);
				SoundBackendState soundState;
				installSoundBackend(runtime, soundState);
				QStringList displayedLines;
				QObject::connect(&runtime, &WorldRuntime::incomingLineReceived, &runtime,
				                 [&displayedLines](const QString &line) { displayedLines.push_back(line); });

				QByteArray enablePayload = QByteArrayLiteral("!!SOUND(coin.wav)\r\n");
				enablePayload.append(QByteArray::fromHex("fffb5a"));
				enablePayload.append(QByteArrayLiteral("!!SOUND(coin.wav)\r\n"));
				WorldRuntimeTestAccess::processRawDataPayload(runtime, enablePayload);
				QCOMPARE(displayedLines, QStringList{QStringLiteral("!!SOUND(coin.wav)")});
				QCOMPARE(soundState.requests.size(), 1);

				QByteArray disablePayload = QByteArray::fromHex("fffc5a");
				disablePayload.append(QByteArrayLiteral("!!SOUND(coin.wav)\r\n"));
				WorldRuntimeTestAccess::processRawDataPayload(runtime, disablePayload);
				QCOMPARE(displayedLines, QStringList({QStringLiteral("!!SOUND(coin.wav)"),
				                                      QStringLiteral("!!SOUND(coin.wav)")}));
				QCOMPARE(soundState.requests.size(), 1);

				QByteArray interruptedCommand = QByteArray::fromHex("fffb5a");
				interruptedCommand.append(QByteArrayLiteral("before !!SOU"));
				interruptedCommand.append(QByteArray::fromHex("fffc5a"));
				interruptedCommand.append(QByteArrayLiteral("ND(coin.wav) after\r\n"));
				WorldRuntimeTestAccess::processRawDataPayload(runtime, interruptedCommand);
				QCOMPARE(displayedLines.constLast(), QStringLiteral("before !!SOUND(coin.wav) after"));
				QCOMPARE(soundState.requests.size(), 1);
			}

			/** @brief Verifies MUSIC continuation reaches the native worker boundary with all parameters. */
			void musicContinuationParametersReachSoundWorker()
			{
				WorldRuntime runtime;
				installMspTransport(runtime);
				runtime.setSoundAvailable(true);
				SoundBackendState soundState;
				installSoundBackend(runtime, soundState);
				WorldRuntimeTestAccess::processRawDataPayload(runtime, QByteArray::fromHex("fffb5a"));

				WorldRuntimeTestAccess::processRawDataPayload(
				    runtime, QByteArrayLiteral("!!MUSIC(theme.ogg V=40 L=1)\r\n"));
				QCOMPARE(soundState.requests.size(), 1);
				QCOMPARE(soundState.requests.constLast().filePattern, QStringLiteral("theme.ogg"));
				WorldRuntimeTestAccess::processRawDataPayload(
				    runtime, QByteArrayLiteral("!!MUSIC(theme.ogg V=60 L=-1 C=1)\r\n"));

				QCOMPARE(soundState.requests.size(), 2);
				const QMudNativeSoundRequest &continuation = soundState.requests.constLast();
				QCOMPARE(continuation.filePattern, QStringLiteral("theme.ogg"));
				QVERIFY(continuation.continuePlayback);
				QCOMPARE(continuation.repeatCount, -1);
				QCOMPARE(continuation.volume, 60);
			}
			// NOLINTEND(readability-convert-member-functions-to-static)
	};
} // namespace

QTEST_MAIN(tst_WorldRuntime_Msp)

#include "tst_WorldRuntime_Msp.moc"
