/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_TtsEngine.cpp
 * Role: Unit coverage for centralized TTS runtime behavior in TtsEngine.
 */

#include "TtsEngine.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QCoreApplication>
#include <QtTest/QTest>

#include <type_traits>

using QMudTtsEngine::TtsEngine;

namespace
{
#if QMUD_ENABLE_TEXT_TO_SPEECH
	class RebindSpyRuntime final : public TtsEngine
	{
		public:
			bool               hasSpeechFlag{true};
			bool               rebindResult{false};
			int                rebindCallCount{0};
			int                lastRebindIndex{-1};

			[[nodiscard]] bool hasSpeech() const override
			{
				return hasSpeechFlag;
			}

		protected:
			bool rebindSpeechForAudioOutput(const int outputIndex) override
			{
				++rebindCallCount;
				lastRebindIndex = outputIndex;
				return rebindResult;
			}
	};
#endif

	class SynthesizeQueueSpyRuntime final : public TtsEngine
	{
		public:
			bool               hasSpeechFlag{true};
			bool               supportsSynthesizeFlag{true};
			bool               queueResult{false};
			int                queueCallCount{0};

			[[nodiscard]] bool hasSpeech() const override
			{
				return hasSpeechFlag;
			}

		protected:
			[[nodiscard]] bool supportsSynthesizeCapability() const override
			{
				return supportsSynthesizeFlag;
			}

			bool queueSynthesizeUtterance(QString                                utterance,
			                              const std::function<void(QByteArray)> &onChunk) override
			{
				Q_UNUSED(utterance);
				Q_UNUSED(onChunk);
				++queueCallCount;
				return queueResult;
			}
	};

	class BrokenBackendRuntime final : public TtsEngine
	{
		public:
			[[nodiscard]] bool hasSpeech() const override
			{
				return true;
			}
	};

	class QueueBoundRuntime final : public TtsEngine
	{
		public:
			QueueBoundRuntime() = default;
	};
} // namespace

class tst_TtsEngine : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void rateConversionClampsAsExpected()
		{
			QCOMPARE(TtsEngine::qtRateToSapi(-2.0), -10);
			QCOMPARE(TtsEngine::qtRateToSapi(-1.0), -10);
			QCOMPARE(TtsEngine::qtRateToSapi(-0.49), -5);
			QCOMPARE(TtsEngine::qtRateToSapi(0.0), 0);
			QCOMPARE(TtsEngine::qtRateToSapi(0.51), 5);
			QCOMPARE(TtsEngine::qtRateToSapi(1.0), 10);
			QCOMPARE(TtsEngine::qtRateToSapi(2.0), 10);

			QCOMPARE(TtsEngine::sapiRateToQt(-50), -1.0);
			QCOMPARE(TtsEngine::sapiRateToQt(-10), -1.0);
			QCOMPARE(TtsEngine::sapiRateToQt(0), 0.0);
			QCOMPARE(TtsEngine::sapiRateToQt(10), 1.0);
			QCOMPARE(TtsEngine::sapiRateToQt(50), 1.0);
		}

		void audioOutputSelectorStorageUsesBinarySafeType()
		{
			static_assert(std::is_same_v<decltype(TtsEngine::audioOutputSelectors), QList<QByteArray>>,
			              "audioOutputSelectors must store binary-safe byte arrays");

			const std::shared_ptr<TtsEngine> runtime = TtsEngine::create();
			QVERIFY(runtime);
			const QByteArray selectorWithNullBytes = QByteArray::fromHex("00ff4162430080");
			runtime->audioOutputSelectors.push_back(selectorWithNullBytes);
			QCOMPARE(runtime->audioOutputSelectors.constLast(), selectorWithNullBytes);
		}

		void preferredEnginePolicySelectionIsDeterministic()
		{
			using EnginePolicy = TtsEngine::EnginePlatformPolicy;

			QCOMPARE(TtsEngine::preferredEngineIdFor({}, EnginePolicy::Linux), QString());
			QCOMPARE(TtsEngine::preferredEngineIdFor({QStringLiteral("speechd"), QStringLiteral("flite")},
			                                         EnginePolicy::Linux),
			         QStringLiteral("speechd"));
			QCOMPARE(TtsEngine::preferredEngineIdFor({QStringLiteral("FlItE"), QStringLiteral("dummy")},
			                                         EnginePolicy::Linux),
			         QStringLiteral("FlItE"));
			QCOMPARE(TtsEngine::preferredEngineIdFor({QStringLiteral("dummy"), QStringLiteral("another")},
			                                         EnginePolicy::Linux),
			         QString());

			QCOMPARE(TtsEngine::preferredEngineIdFor({QStringLiteral("dummy"), QStringLiteral("SaPi")},
			                                         EnginePolicy::Windows),
			         QStringLiteral("SaPi"));
			QCOMPARE(TtsEngine::preferredEngineIdFor({QStringLiteral("dummy"), QStringLiteral("another")},
			                                         EnginePolicy::Windows),
			         QString());

			QCOMPARE(TtsEngine::preferredEngineIdFor({QStringLiteral("first"), QStringLiteral("second")},
			                                         EnginePolicy::Other),
			         QStringLiteral("first"));
		}

		void synthesizeQueueDispatchFailureRollsBackSpeakingState()
		{
			const std::shared_ptr<SynthesizeQueueSpyRuntime> runtime =
			    std::make_shared<SynthesizeQueueSpyRuntime>();
			QVERIFY(runtime);
			runtime->hasSpeechFlag          = true;
			runtime->supportsSynthesizeFlag = true;
			runtime->queueResult            = false;
			runtime->isSpeaking             = false;
			runtime->isPaused               = true;

			const bool queued = runtime->synthesizeToStream(QStringLiteral("qmud synth rollback"), false,
			                                                [](const QByteArray &) {});
			QVERIFY(!queued);
			QCOMPARE(runtime->queueCallCount, 1);
			QVERIFY(!runtime->isSpeaking);
			QVERIFY(!runtime->isPaused);

			runtime->queueResult = true;
			const bool secondQueued =
			    runtime->synthesizeToStream(QStringLiteral("qmud synth success"), false, {});
			QVERIFY(secondQueued);
			QCOMPARE(runtime->queueCallCount, 2);
			QVERIFY(runtime->isSpeaking);
		}

		void waitForSpeechIdleReturnsFalseWhenBackendHandleIsMissing()
		{
			const std::shared_ptr<BrokenBackendRuntime> runtime = std::make_shared<BrokenBackendRuntime>();
			QVERIFY(runtime);
			runtime->isSpeaking = true;

			QVERIFY(!runtime->waitForSpeechIdle(0));
			QVERIFY(!runtime->waitForSpeechIdle(-1));

			runtime->isSpeaking = false;
			runtime->isPaused   = false;
			runtime->pendingUtterances.clear();
			runtime->stopInProgress = false;
			QVERIFY(runtime->waitForSpeechIdle(0));
		}

#if QMUD_ENABLE_TEXT_TO_SPEECH
		void selectAudioOutputUsesRebindResult()
		{
			const std::shared_ptr<RebindSpyRuntime> runtime = std::make_shared<RebindSpyRuntime>();
			QVERIFY(runtime);
			runtime->audioOutputIds          = {QStringLiteral("default"), QStringLiteral("second")};
			runtime->currentAudioOutputIndex = 0;
			runtime->hasSpeechFlag           = true;

			runtime->rebindResult = false;
			runtime->selectAudioOutput(1);
			QCOMPARE(runtime->rebindCallCount, 1);
			QCOMPARE(runtime->lastRebindIndex, 1);
			QCOMPARE(runtime->currentAudioOutputIndex, 0);

			runtime->rebindResult = true;
			runtime->selectAudioOutput(1);
			QCOMPARE(runtime->rebindCallCount, 2);
			QCOMPARE(runtime->lastRebindIndex, 1);
			QCOMPARE(runtime->currentAudioOutputIndex, 1);

			runtime->selectAudioOutput(-1);
			runtime->selectAudioOutput(99);
			QCOMPARE(runtime->rebindCallCount, 2);
			QCOMPARE(runtime->currentAudioOutputIndex, 1);

			runtime->hasSpeechFlag = false;
			runtime->selectAudioOutput(0);
			QCOMPARE(runtime->rebindCallCount, 2);
			QCOMPARE(runtime->currentAudioOutputIndex, 0);
		}

		void speechRuntimeUsesDedicatedWorkerThreadWhenBackendIsAvailable()
		{
			const std::shared_ptr<TtsEngine> runtime = TtsEngine::create();
			QVERIFY(runtime);
			if (!runtime->hasSpeech())
				QSKIP("No Qt TextToSpeech backend available in this environment.");
			QVERIFY(runtime->speech != nullptr);
			QVERIFY(runtime->speechThread != nullptr);
			QVERIFY(runtime->speechThread->isRunning());
			QCOMPARE(runtime->speech->thread(), runtime->speechThread.get());
			QVERIFY(runtime->speech->thread() != QThread::currentThread());
		}
#endif

		void createInitializesDefaultAudioOutputMetadata()
		{
			const std::shared_ptr<TtsEngine> runtime = TtsEngine::create();
			QVERIFY(runtime);
			QVERIFY(!runtime->audioOutputIds.empty());
			QVERIFY(!runtime->audioOutputDescriptions.empty());
			QVERIFY(!runtime->audioOutputSelectors.empty());
			QCOMPARE(runtime->audioOutputIds.constFirst(), QStringLiteral(R"(QMUD\TTS\Audio\Default)"));
			QCOMPARE(runtime->audioOutputDescriptions.constFirst(), QStringLiteral("Default Audio Output"));
			QVERIFY(runtime->audioOutputSelectors.constFirst().isEmpty());
			QCOMPARE(runtime->currentAudioOutputIndex, 0);
		}

		void publicApiOperationsRemainSafeAcrossAvailabilityStates()
		{
			const std::shared_ptr<TtsEngine> runtime = TtsEngine::create();
			QVERIFY(runtime);

			runtime->pause();
			runtime->resume();
			runtime->setSpeechRate(0.5);
			runtime->setSpeechVolume(0.75);
			runtime->enqueueUtterance(QStringLiteral("qmud tts test"), false);

			const bool idleResult = runtime->waitForSpeechIdle(0);
			if (!runtime->hasSpeech())
				QVERIFY(idleResult);
			QVERIFY(runtime->skipQueuedUtterances(1) >= 0);

			bool       chunkDelivered = false;
			const bool synthQueued =
			    runtime->synthesizeToStream(QStringLiteral("test"), false,
			                                [&chunkDelivered](const QByteArray &) { chunkDelivered = true; });
			if (!runtime->hasSpeech())
			{
				QVERIFY(!synthQueued);
				QVERIFY(!chunkDelivered);
			}
		}

#if QMUD_ENABLE_TEXT_TO_SPEECH
		void queueBoundingEnforcesSizeAndCharacterCaps()
		{
			const std::shared_ptr<TtsEngine> runtime = std::make_shared<QueueBoundRuntime>();
			QVERIFY(runtime);

			runtime->pendingUtterances.clear();
			runtime->pendingCharacters = 0;

			const QString chunk(2048, QLatin1Char('x'));
			for (int i = 0; i < 900; ++i)
				runtime->enqueueBoundedUtterance(chunk);

			QVERIFY(runtime->pendingUtterances.size() <= 512);
			QVERIFY(runtime->pendingCharacters <= (512 * 1024));

			qsizetype actualCharacters = 0;
			for (const QString &entry : runtime->pendingUtterances)
				actualCharacters += entry.size();
			QCOMPARE(runtime->pendingCharacters, actualCharacters);
		}
#endif
		// NOLINTEND(readability-convert-member-functions-to-static)
};

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	tst_TtsEngine    tc;
	return QTest::qExec(&tc, argc, argv);
}

#if __has_include("tst_TtsEngine.moc")
#include "tst_TtsEngine.moc"
#endif
