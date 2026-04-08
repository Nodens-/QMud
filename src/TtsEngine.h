/**
 * @file TtsEngine.h
 * @brief Centralized Qt TextToSpeech runtime used by compatibility shims and future native APIs.
 */

#ifndef QMUD_TTSENGINE_H
#define QMUD_TTSENGINE_H

// ReSharper disable once CppUnusedIncludeDirective
#include <QByteArray>
// ReSharper disable once CppUnusedIncludeDirective
#include <QList>
// ReSharper disable once CppUnusedIncludeDirective
#include <QStringList>

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#ifndef QMUD_ENABLE_TEXT_TO_SPEECH
#define QMUD_ENABLE_TEXT_TO_SPEECH 0
#endif

#if QMUD_ENABLE_TEXT_TO_SPEECH
// ReSharper disable once CppUnusedIncludeDirective
#include <QObject>
#include <QTextToSpeech>
#include <QThread>
#include <QVoice>
#endif

class tst_TtsEngine;

namespace QMudTtsEngine
{
	/**
	 * @brief Stateful TextToSpeech runtime with SAPI-compatible control semantics.
	 *
	 * This class owns Qt TextToSpeech engine selection, worker-thread affinity, queueing,
	 * and blocking/queued speech operations. The current luacom SAPI shim uses this runtime,
	 * and future native TTS APIs should also build on it.
	 */
	class TtsEngine : public std::enable_shared_from_this<TtsEngine>
	{
		public:
			/**
			 * @brief Platform policy used for preferred TextToSpeech engine selection.
			 */
			enum class EnginePlatformPolicy
			{
				Auto, /**< Use build target platform policy. */
				Linux,
				Windows,
				Other
			};

			/**
			 * @brief Creates and initializes a runtime instance.
			 * @return Shared runtime instance (may have no active speech backend when unavailable).
			 */
			static std::shared_ptr<TtsEngine> create();

			/**
			 * @brief Selects preferred engine id for a candidate list and platform policy.
			 * @param engines Candidate engine ids reported by Qt.
			 * @param platform Platform policy. `Auto` uses build target platform.
			 * @return Preferred engine id or empty string when no suitable engine exists.
			 */
			static QString                    preferredEngineIdFor(const QStringList   &engines,
			                                                       EnginePlatformPolicy platform = EnginePlatformPolicy::Auto);

			/**
			 * @brief Tears down the speech object and worker thread.
			 */
			virtual ~TtsEngine();

			/**
			 * @brief Returns whether a Qt TextToSpeech backend is active.
			 */
			[[nodiscard]] virtual bool hasSpeech() const;

#if QMUD_ENABLE_TEXT_TO_SPEECH
			/** Cached voices provided by the active engine. */
			QList<QVoice> voices;
			/** Pending utterance queue for async speaking. */
			QStringList   pendingUtterances;
			/** Sum of pending utterance characters used for queue bounding. */
			qsizetype     pendingCharacters{0};
			/** True while backend is currently speaking/synthesizing. */
			bool          isSpeaking{false};
			/** True while backend is paused. */
			bool          isPaused{false};
			/** Prevents auto-advance while a stop transition is in progress. */
			bool          suppressAutoAdvance{false};
			/** True while stop() has been requested and completion is pending. */
			bool          stopInProgress{false};
#endif
			/** Stable token IDs for current voice list. */
			QStringList        voiceIds;
			/** Stable token IDs for available audio outputs. */
			QStringList        audioOutputIds;
			/** Human-readable descriptions for audio outputs. */
			QStringList        audioOutputDescriptions;
			/** Backend selectors used to rebind audio device output. */
			QList<QByteArray>  audioOutputSelectors;
			/** Currently selected voice index in @ref voices. */
			int                currentVoiceIndex{0};
			/** Currently selected audio output index in @ref audioOutputIds. */
			int                currentAudioOutputIndex{0};
			/** SAPI-style rate value (-10..10). */
			int                rate{0};
			/** SAPI-style volume percentage (0..100). */
			int                volumePercent{100};
			/** SAPI priority compatibility value. */
			int                priority{0};
			/** SAPI event-interest flags compatibility value. */
			int                eventInterests{0};
			/** SAPI alert-boundary compatibility value. */
			int                alertBoundary{0};
			/** Timeout used for synchronous speak/wait semantics. */
			int                synchronousSpeakTimeoutMs{10000};
			/** SAPI compatibility flag for stream/audio format behavior. */
			bool               allowAudioOutputFormatChangesOnNextSet{false};
			/** Last stream number emitted by compatibility layer. */
			int                lastStreamNumber{0};
			/** True once host audio outputs have been enumerated. */
			bool               audioOutputsEnumerated{false};

			/**
			 * @brief Enumerates host audio outputs once and populates token metadata.
			 */
			void               ensureAudioOutputsEnumerated();

			/**
			 * @brief Refreshes voice cache and active voice selection state.
			 */
			void               refreshVoices();

			/**
			 * @brief Returns whether a voice index is valid.
			 * @param index Zero-based voice index.
			 */
			[[nodiscard]] bool hasVoiceAt(int index) const;

			/**
			 * @brief Returns whether an audio-output index is valid.
			 * @param index Zero-based audio-output index.
			 */
			[[nodiscard]] bool hasAudioOutputAt(int index) const;

			/**
			 * @brief Selects the active voice by index.
			 * @param index Zero-based voice index.
			 */
			void               selectVoice(int index);

			/**
			 * @brief Selects the active audio output by index.
			 * @param index Zero-based audio-output index.
			 */
			void               selectAudioOutput(int index);

			/**
			 * @brief Converts Qt rate (-1..1) to SAPI rate (-10..10).
			 */
			static int         qtRateToSapi(double qtRate);

			/**
			 * @brief Converts SAPI rate (-10..10) to Qt rate (-1..1).
			 */
			static double      sapiRateToQt(int sapiRate);

			/**
			 * @brief Dequeues and starts the next pending utterance, if any.
			 */
			void               startNextUtterance();

			/**
			 * @brief Enqueues speech text, optionally purging current/pending speech first.
			 * @param utterance Text to enqueue.
			 * @param purgeBeforeSpeak Whether to stop and purge before enqueueing.
			 */
			void               enqueueUtterance(QString utterance, bool purgeBeforeSpeak);

			/**
			 * @brief Waits until speaking queue becomes idle or timeout expires.
			 * @param timeoutMs Timeout in milliseconds; negative means no timeout.
			 * @return True when queue became idle.
			 */
			virtual bool       waitForSpeechIdle(int timeoutMs);

			/**
			 * @brief Skips current/pending utterances up to @p count entries.
			 * @param count Number of entries to skip.
			 * @return Actual number skipped.
			 */
			int                skipQueuedUtterances(int count);

			/**
			 * @brief Synthesizes utterance audio to callback chunks when backend supports synthesis.
			 * @param utterance Text to synthesize.
			 * @param purgeBeforeSpeak Whether to stop and purge queue first.
			 * @param onChunk Callback receiving synthesized PCM chunks.
			 * @return True when synthesis was queued; false when unsupported/unavailable.
			 */
			virtual bool       synthesizeToStream(const QString &utterance, bool purgeBeforeSpeak,
			                                      const std::function<void(QByteArray)> &onChunk);

			/**
			 * @brief Pauses active speech.
			 */
			void               pause();

			/**
			 * @brief Resumes paused speech.
			 */
			void               resume();

			/**
			 * @brief Returns current Qt speech rate.
			 */
			double             speechRate() const;

			/**
			 * @brief Sets Qt speech rate.
			 * @param qtRate Rate in Qt range (-1..1).
			 */
			void               setSpeechRate(double qtRate) const;

			/**
			 * @brief Returns current Qt speech volume.
			 */
			double             speechVolume() const;

			/**
			 * @brief Sets Qt speech volume.
			 * @param volume Volume in Qt range (0..1).
			 */
			void               setSpeechVolume(double volume) const;

		private:
			friend class ::tst_TtsEngine;

		protected:
			TtsEngine() = default;

			[[nodiscard]] virtual bool supportsSynthesizeCapability() const;
			virtual bool               queueSynthesizeUtterance(QString                                utterance,
			                                                    const std::function<void(QByteArray)> &onChunk);

#if QMUD_ENABLE_TEXT_TO_SPEECH
			std::unique_ptr<QTextToSpeech> speech;
			std::unique_ptr<QThread>       speechThread;
			std::unique_ptr<QObject>       callbackDispatcher;

			template <typename Fn>
			auto invokeSpeechBlocking(Fn &&fn) const -> std::invoke_result_t<Fn, QTextToSpeech &>
			{
				using Result = std::invoke_result_t<Fn, QTextToSpeech &>;
				if (!speech)
				{
					if constexpr (std::is_void_v<Result>)
						return;
					else
						return Result{};
				}

				QTextToSpeech *speechObject = speech.get();
				if (speechObject->thread() == QThread::currentThread())
				{
					if constexpr (std::is_void_v<Result>)
					{
						fn(*speechObject);
						return;
					}
					else
					{
						return fn(*speechObject);
					}
				}

				if constexpr (std::is_void_v<Result>)
				{
					QMetaObject::invokeMethod(
					    speechObject,
					    [self = shared_from_this(), fn = std::forward<Fn>(fn)]() mutable
					    {
						    if (!self || !self->speech)
							    return;
						    fn(*self->speech);
					    },
					    Qt::BlockingQueuedConnection);
					return;
				}
				else
				{
					Result     result{};
					const bool invoked = QMetaObject::invokeMethod(
					    speechObject,
					    [self = shared_from_this(), fn = std::forward<Fn>(fn), &result]() mutable
					    {
						    if (!self || !self->speech)
							    return;
						    result = fn(*self->speech);
					    },
					    Qt::BlockingQueuedConnection);
					if (!invoked)
						return Result{};
					return result;
				}
			}

			template <typename Fn> bool invokeSpeechQueued(Fn &&fn) const
			{
				if (!speech)
					return false;
				QTextToSpeech *speechObject = speech.get();
				if (speechObject->thread() == QThread::currentThread())
				{
					fn(*speechObject);
					return true;
				}
				return QMetaObject::invokeMethod(
				    speechObject,
				    [self = shared_from_this(), fn = std::forward<Fn>(fn)]() mutable
				    {
					    if (!self || !self->speech)
						    return;
					    fn(*self->speech);
				    },
				    Qt::QueuedConnection);
			}

			virtual bool rebindSpeechForAudioOutput(int outputIndex);

		private:
			void initializeAudioOutputs();
			void wireSpeechStateCallbacks();
			void destroySpeechObject();
			void resolveStopTransitionIfTerminal();
			void enqueueBoundedUtterance(QString utterance);
#else
			void initializeAudioOutputs();
#endif
	};
} // namespace QMudTtsEngine

#endif // QMUD_TTSENGINE_H
