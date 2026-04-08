/**
 * @file TtsEngine.cpp
 * @brief Implementation of the centralized Qt TextToSpeech runtime.
 */

#include "TtsEngine.h"

#include <QCoreApplication>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <limits>

#if QMUD_ENABLE_TEXT_TO_SPEECH
#include <QAudioDevice>
#include <QCryptographicHash>
#include <QDataStream>
#include <QEventLoop>
#include <QMediaDevices>
#include <QPointer>
#include <QTimer>
#endif

namespace
{
	constexpr int kMaxPendingUtterances = 512;
	constexpr int kMaxPendingChars      = 512 * 1024;

#if QMUD_ENABLE_TEXT_TO_SPEECH
	QString buildVoiceIdBase(const QVoice &voice)
	{
		const QString locale = voice.locale().name().trimmed();
		const QString name   = voice.name().trimmed();
		return QStringLiteral("QMUD\\TTS\\%1\\%2\\%3\\%4")
		    .arg(locale.isEmpty() ? QStringLiteral("und") : locale,
		         name.isEmpty() ? QStringLiteral("voice") : name,
		         QString::number(static_cast<int>(voice.gender())),
		         QString::number(static_cast<int>(voice.age())));
	}

	QString buildVoiceId(const QVoice &voice)
	{
		QString    baseId = buildVoiceIdBase(voice);
		QByteArray serializedVoice;
		{
			QDataStream stream(&serializedVoice, QIODeviceBase::WriteOnly);
			stream.setVersion(QDataStream::Qt_6_0);
			stream << voice;
			if (stream.status() != QDataStream::Ok)
				return baseId;
		}
		const QByteArray digest = QCryptographicHash::hash(serializedVoice, QCryptographicHash::Sha1).toHex();
		if (digest.isEmpty())
			return baseId;
		return QStringLiteral("%1\\h%2").arg(baseId, QString::fromLatin1(digest));
	}

	bool isSpeechQueueIdle(const QMudTtsEngine::TtsEngine &runtime)
	{
		if (!runtime.hasSpeech())
			return true;
		return !runtime.isSpeaking && !runtime.isPaused && !runtime.stopInProgress &&
		       runtime.pendingUtterances.isEmpty();
	}
#endif
} // namespace

namespace QMudTtsEngine
{
	QString TtsEngine::preferredEngineIdFor(const QStringList &engines, const EnginePlatformPolicy platform)
	{
		if (engines.isEmpty())
			return {};

		auto findEngineId = [&engines](const QString &name) -> QString
		{
			for (const QString &engineId : engines)
			{
				if (engineId.compare(name, Qt::CaseInsensitive) == 0)
					return engineId;
			}
			return {};
		};

		EnginePlatformPolicy effectivePolicy = platform;
		if (effectivePolicy == EnginePlatformPolicy::Auto)
		{
#ifdef Q_OS_LINUX
			effectivePolicy = EnginePlatformPolicy::Linux;
#elif defined(Q_OS_WIN)
			effectivePolicy = EnginePlatformPolicy::Windows;
#else
			effectivePolicy = EnginePlatformPolicy::Other;
#endif
		}

		if (effectivePolicy == EnginePlatformPolicy::Linux)
		{
			QString speechd = findEngineId(QStringLiteral("speechd"));
			if (!speechd.isEmpty())
				return speechd;
			return findEngineId(QStringLiteral("flite"));
		}
		if (effectivePolicy == EnginePlatformPolicy::Windows)
			return findEngineId(QStringLiteral("sapi"));
		return engines.constFirst();
	}

	std::shared_ptr<TtsEngine> TtsEngine::create()
	{
		auto runtime = std::shared_ptr<TtsEngine>(new TtsEngine());
		runtime->initializeAudioOutputs();

#if QMUD_ENABLE_TEXT_TO_SPEECH
		const QStringList engines = QTextToSpeech::availableEngines();
		if (engines.isEmpty())
			return runtime;

		const QString selectedEngineId = preferredEngineIdFor(engines);
		if (selectedEngineId.isEmpty())
			return runtime;

		runtime->callbackDispatcher = std::make_unique<QObject>();
		runtime->speech             = std::make_unique<QTextToSpeech>(selectedEngineId);
		if (!runtime->speech)
			return runtime;

		runtime->speechThread = std::make_unique<QThread>();
		runtime->speechThread->setObjectName(QStringLiteral("QMudTtsWorker"));
		runtime->speechThread->start();
		runtime->speech->moveToThread(runtime->speechThread.get());

		runtime->invokeSpeechBlocking(
		    [runtime](QTextToSpeech &speech)
		    {
			    speech.setVolume(static_cast<double>(runtime->volumePercent) / 100.0);
			    speech.setRate(sapiRateToQt(runtime->rate));
		    });
		runtime->wireSpeechStateCallbacks();
		runtime->refreshVoices();
#endif
		return runtime;
	}

	TtsEngine::~TtsEngine()
	{
#if QMUD_ENABLE_TEXT_TO_SPEECH
		destroySpeechObject();
		if (speechThread && speechThread->isRunning())
		{
			speechThread->quit();
			if (QThread::currentThread() != speechThread.get())
				speechThread->wait();
		}
#endif
	}

	bool TtsEngine::hasSpeech() const
	{
#if QMUD_ENABLE_TEXT_TO_SPEECH
		return speech != nullptr;
#else
		return false;
#endif
	}

	void TtsEngine::initializeAudioOutputs()
	{
		audioOutputIds.clear();
		audioOutputDescriptions.clear();
		audioOutputSelectors.clear();
		audioOutputIds.push_back(QStringLiteral("QMUD\\TTS\\Audio\\Default"));
		audioOutputDescriptions.push_back(QStringLiteral("Default Audio Output"));
		audioOutputSelectors.push_back({});
		currentAudioOutputIndex = 0;
		audioOutputsEnumerated  = false;
	}

#if QMUD_ENABLE_TEXT_TO_SPEECH
	void TtsEngine::wireSpeechStateCallbacks()
	{
		if (!speech)
			return;
		QObject *callbackContext = callbackDispatcher ? callbackDispatcher.get() : speech.get();
		const std::weak_ptr<TtsEngine> weakRuntime = shared_from_this();
		QObject::connect(speech.get(), &QTextToSpeech::stateChanged, callbackContext,
		                 [weakRuntime](const QTextToSpeech::State state)
		                 {
			                 const auto runtime = weakRuntime.lock();
			                 if (!runtime)
				                 return;
			                 if (state == QTextToSpeech::Speaking || state == QTextToSpeech::Synthesizing)
			                 {
				                 runtime->isPaused   = false;
				                 runtime->isSpeaking = true;
				                 return;
			                 }
			                 if (state == QTextToSpeech::Paused)
			                 {
				                 runtime->isSpeaking = false;
				                 runtime->isPaused   = true;
				                 return;
			                 }
			                 if (state != QTextToSpeech::Ready && state != QTextToSpeech::Error)
				                 return;
			                 runtime->isSpeaking = false;
			                 runtime->isPaused   = false;
			                 if (runtime->stopInProgress)
			                 {
				                 runtime->stopInProgress      = false;
				                 runtime->suppressAutoAdvance = false;
			                 }
			                 if (runtime->suppressAutoAdvance)
				                 return;
			                 runtime->startNextUtterance();
		                 });
	}

	void TtsEngine::destroySpeechObject()
	{
		if (!speech)
			return;
		QTextToSpeech *speechObject = speech.release();
		if (!speechObject)
			return;
		if (speechObject->thread() == QThread::currentThread())
		{
			delete speechObject;
			return;
		}
		const bool invoked = QMetaObject::invokeMethod(
		    speechObject, [speechObject]() { delete speechObject; }, Qt::BlockingQueuedConnection);
		if (!invoked)
			delete speechObject;
	}

	void TtsEngine::enqueueBoundedUtterance(QString utterance)
	{
		if (utterance.size() > kMaxPendingChars)
			utterance.truncate(kMaxPendingChars);
		if (utterance.isEmpty())
			return;

		const qsizetype newChars = utterance.size();
		while (!pendingUtterances.isEmpty() && (pendingUtterances.size() >= kMaxPendingUtterances ||
		                                        pendingCharacters + newChars > kMaxPendingChars))
		{
			const QString dropped = pendingUtterances.front();
			pendingUtterances.removeFirst();
			pendingCharacters -= dropped.size();
			if (pendingCharacters < 0)
				pendingCharacters = 0;
		}
		if (pendingUtterances.size() >= kMaxPendingUtterances ||
		    pendingCharacters + newChars > kMaxPendingChars)
			return;
		pendingUtterances.push_back(std::move(utterance));
		pendingCharacters += newChars;
	}

	void TtsEngine::resolveStopTransitionIfTerminal()
	{
		if (!hasSpeech() || !stopInProgress)
			return;
		const QTextToSpeech::State state =
		    invokeSpeechBlocking([](const QTextToSpeech &speech) { return speech.state(); });
		if (state == QTextToSpeech::Speaking || state == QTextToSpeech::Synthesizing ||
		    state == QTextToSpeech::Paused)
		{
			return;
		}
		stopInProgress      = false;
		suppressAutoAdvance = false;
	}

	bool TtsEngine::rebindSpeechForAudioOutput(const int outputIndex)
	{
		if (!hasSpeech() || outputIndex < 0 || outputIndex >= audioOutputSelectors.size())
			return false;

		const QString preferredVoiceTokenId = voiceIds.value(currentVoiceIndex, QString());
		const QString engineId =
		    invokeSpeechBlocking([](const QTextToSpeech &speech) { return speech.engine(); });
		if (engineId.isEmpty())
			return false;

		QVariantMap      params;
		const QByteArray selector = audioOutputSelectors.at(outputIndex);
		if (!selector.isEmpty())
		{
			const QString selectorString = QString::fromLatin1(selector);
			params.insert(QStringLiteral("audioDevice"), selectorString);
			params.insert(QStringLiteral("audioDeviceId"), selector);
			params.insert(QStringLiteral("device"), selectorString);
		}

		auto replacementSpeech = std::make_unique<QTextToSpeech>(engineId, params);
		if (!replacementSpeech || replacementSpeech->errorReason() != QTextToSpeech::ErrorReason::NoError)
			return false;

		if (speechThread && speechThread->isRunning())
			replacementSpeech->moveToThread(speechThread.get());

		destroySpeechObject();
		speech = std::move(replacementSpeech);
		invokeSpeechBlocking(
		    [this](QTextToSpeech &speechObject)
		    {
			    speechObject.setVolume(static_cast<double>(volumePercent) / 100.0);
			    speechObject.setRate(sapiRateToQt(rate));
		    });
		isSpeaking          = false;
		isPaused            = false;
		suppressAutoAdvance = false;
		stopInProgress      = false;
		wireSpeechStateCallbacks();
		refreshVoices();
		const qsizetype preferredIndex = voiceIds.indexOf(preferredVoiceTokenId);
		if (preferredIndex >= 0 && preferredIndex <= static_cast<qsizetype>(std::numeric_limits<int>::max()))
			selectVoice(static_cast<int>(preferredIndex));
		if (!pendingUtterances.isEmpty())
			startNextUtterance();
		return true;
	}
#endif

	void TtsEngine::ensureAudioOutputsEnumerated()
	{
		if (audioOutputsEnumerated)
			return;
#if QMUD_ENABLE_TEXT_TO_SPEECH
		if (!QCoreApplication::instance())
			return;
		const auto outputs = QMediaDevices::audioOutputs();
		for (const auto &device : outputs)
		{
			const QByteArray rawId = device.id();
			if (rawId.isEmpty())
				continue;
			const QString idSuffix = QString::fromLatin1(rawId.toHex());
			const QString tokenId  = QStringLiteral("QMUD\\TTS\\Audio\\Device\\%1")
			                            .arg(idSuffix.isEmpty() ? QStringLiteral("unknown") : idSuffix);
			const QString description =
			    device.description().trimmed().isEmpty() ? tokenId : device.description().trimmed();
			audioOutputIds.push_back(tokenId);
			audioOutputDescriptions.push_back(description);
			audioOutputSelectors.push_back(rawId);
		}
#endif
		audioOutputsEnumerated = true;
	}

	void TtsEngine::refreshVoices()
	{
#if QMUD_ENABLE_TEXT_TO_SPEECH
		if (!hasSpeech())
			return;
		voices = invokeSpeechBlocking([](const QTextToSpeech &speechObject)
		                              { return speechObject.availableVoices(); });
		voiceIds.clear();
		voiceIds.reserve(voices.size());
		for (const QVoice &voice : voices)
			voiceIds.push_back(buildVoiceId(voice));
		if (voices.isEmpty())
		{
			currentVoiceIndex = 0;
			return;
		}
		const QVoice activeVoice =
		    invokeSpeechBlocking([](const QTextToSpeech &speechObject) { return speechObject.voice(); });
		const qsizetype activeIndex = voices.indexOf(activeVoice);
		if (activeIndex >= 0 && activeIndex <= static_cast<qsizetype>(std::numeric_limits<int>::max()))
			currentVoiceIndex = static_cast<int>(activeIndex);
		else
			currentVoiceIndex = 0;
		invokeSpeechBlocking([voice = voices.at(currentVoiceIndex)](QTextToSpeech &speechObject)
		                     { speechObject.setVoice(voice); });
#else
		voiceIds.clear();
#endif
	}

	bool TtsEngine::hasVoiceAt(const int index) const
	{
#if QMUD_ENABLE_TEXT_TO_SPEECH
		return hasSpeech() && index >= 0 && index < voices.size();
#else
		Q_UNUSED(index);
		return false;
#endif
	}

	bool TtsEngine::hasAudioOutputAt(const int index) const
	{
		return index >= 0 && index < audioOutputIds.size();
	}

	void TtsEngine::selectVoice(const int index)
	{
		if (!hasVoiceAt(index))
			return;
		currentVoiceIndex = index;
#if QMUD_ENABLE_TEXT_TO_SPEECH
		invokeSpeechBlocking([voice = voices.at(index)](QTextToSpeech &speechObject)
		                     { speechObject.setVoice(voice); });
#endif
	}

	void TtsEngine::selectAudioOutput(const int index)
	{
		if (!hasAudioOutputAt(index))
			return;
		const int previousIndex = currentAudioOutputIndex;
		currentAudioOutputIndex = index;
#if QMUD_ENABLE_TEXT_TO_SPEECH
		if (hasSpeech() && !rebindSpeechForAudioOutput(index))
			currentAudioOutputIndex = previousIndex;
#endif
	}

	int TtsEngine::qtRateToSapi(const double qtRate)
	{
		const double normalized = std::clamp(qtRate, -1.0, 1.0);
		return static_cast<int>(std::lround(normalized * 10.0));
	}

	double TtsEngine::sapiRateToQt(const int sapiRate)
	{
		const int clampedRate = std::clamp(sapiRate, -10, 10);
		return static_cast<double>(clampedRate) / 10.0;
	}

	void TtsEngine::startNextUtterance()
	{
#if QMUD_ENABLE_TEXT_TO_SPEECH
		if (!hasSpeech() || isSpeaking)
			return;
		while (!pendingUtterances.isEmpty())
		{
			const QString nextUtterance = pendingUtterances.front();
			pendingUtterances.removeFirst();
			pendingCharacters -= nextUtterance.size();
			if (pendingCharacters < 0)
				pendingCharacters = 0;
			if (nextUtterance.isEmpty())
				continue;
			isSpeaking = true;
			if (!invokeSpeechQueued([utterance = nextUtterance](QTextToSpeech &speechObject)
			                        { speechObject.say(utterance); }))
			{
				isSpeaking = false;
			}
			return;
		}
#endif
	}

	void TtsEngine::enqueueUtterance(QString utterance, const bool purgeBeforeSpeak)
	{
#if QMUD_ENABLE_TEXT_TO_SPEECH
		if (!hasSpeech())
			return;
		if (purgeBeforeSpeak)
		{
			pendingUtterances.clear();
			pendingCharacters = 0;
			if (isSpeaking)
			{
				suppressAutoAdvance = true;
				stopInProgress      = true;
				invokeSpeechBlocking([](QTextToSpeech &speechObject) { speechObject.stop(); });
				isSpeaking = false;
				resolveStopTransitionIfTerminal();
			}
		}
		if (!utterance.isEmpty() && (!purgeBeforeSpeak || !utterance.trimmed().isEmpty()))
			enqueueBoundedUtterance(std::move(utterance));
		if (!isSpeaking && !stopInProgress)
			startNextUtterance();
		if (purgeBeforeSpeak && !stopInProgress)
			suppressAutoAdvance = false;
#else
		Q_UNUSED(utterance);
		Q_UNUSED(purgeBeforeSpeak);
#endif
	}

	bool TtsEngine::waitForSpeechIdle(const int timeoutMs)
	{
#if QMUD_ENABLE_TEXT_TO_SPEECH
		if (!hasSpeech())
			return true;
		if (isSpeechQueueIdle(*this))
			return true;
		if (!QCoreApplication::instance())
			return false;

		QObject   *callbackContext = callbackDispatcher ? callbackDispatcher.get() : speech.get();
		QEventLoop loop;
		QTimer     timeoutTimer;
		if (timeoutMs >= 0)
		{
			timeoutTimer.setSingleShot(true);
			QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
			timeoutTimer.start(timeoutMs);
		}

		QPointer<QTextToSpeech>       speechPtr = speech.get();
		const QMetaObject::Connection stateConn =
		    QObject::connect(speech.get(), &QTextToSpeech::stateChanged, callbackContext,
		                     [this, &loop](const QTextToSpeech::State state)
		                     {
			                     Q_UNUSED(state);
			                     if (isSpeechQueueIdle(*this))
				                     loop.quit();
		                     });
		while (!isSpeechQueueIdle(*this))
		{
			if (timeoutMs >= 0 && !timeoutTimer.isActive())
				break;
			if (!speechPtr)
				break;
			loop.exec(QEventLoop::AllEvents);
		}
		QObject::disconnect(stateConn);
		return isSpeechQueueIdle(*this);
#else
		Q_UNUSED(timeoutMs);
		return true;
#endif
	}

	int TtsEngine::skipQueuedUtterances(const int count)
	{
#if QMUD_ENABLE_TEXT_TO_SPEECH
		if (!hasSpeech() || count <= 0)
			return 0;
		int skipped         = 0;
		suppressAutoAdvance = true;
		if (isSpeaking)
		{
			stopInProgress = true;
			invokeSpeechBlocking([](QTextToSpeech &speechObject) { speechObject.stop(); });
			isSpeaking = false;
			++skipped;
			resolveStopTransitionIfTerminal();
		}
		while (skipped < count && !pendingUtterances.isEmpty())
		{
			const QString removed = pendingUtterances.front();
			pendingUtterances.removeFirst();
			pendingCharacters -= removed.size();
			if (pendingCharacters < 0)
				pendingCharacters = 0;
			++skipped;
		}
		if (!stopInProgress)
			suppressAutoAdvance = false;
		if (!isSpeaking && !stopInProgress)
			startNextUtterance();
		return skipped;
#else
		Q_UNUSED(count);
		return 0;
#endif
	}

	bool TtsEngine::synthesizeToStream(const QString &utterance, const bool purgeBeforeSpeak,
	                                   const std::function<void(QByteArray)> &onChunk)
	{
#if QMUD_ENABLE_TEXT_TO_SPEECH
		if (!hasSpeech())
			return false;
		const bool supportsSynthesize = supportsSynthesizeCapability();
		if (!supportsSynthesize)
			return false;

		if (purgeBeforeSpeak)
		{
			pendingUtterances.clear();
			pendingCharacters = 0;
			if (isSpeaking)
			{
				suppressAutoAdvance = true;
				stopInProgress      = true;
				invokeSpeechBlocking([](QTextToSpeech &speechObject) { speechObject.stop(); });
				isSpeaking = false;
				resolveStopTransitionIfTerminal();
			}
		}

		isPaused          = false;
		isSpeaking        = true;
		const bool queued = queueSynthesizeUtterance(utterance, onChunk);
		if (!queued)
			isSpeaking = false;
		return queued;
#else
		Q_UNUSED(utterance);
		Q_UNUSED(purgeBeforeSpeak);
		Q_UNUSED(onChunk);
		return false;
#endif
	}

	bool TtsEngine::supportsSynthesizeCapability() const
	{
#if QMUD_ENABLE_TEXT_TO_SPEECH
		if (!hasSpeech())
			return false;
		return invokeSpeechBlocking(
		    [](const QTextToSpeech &speechObject)
		    { return speechObject.engineCapabilities().testFlag(QTextToSpeech::Capability::Synthesize); });
#else
		return false;
#endif
	}

	bool TtsEngine::queueSynthesizeUtterance(QString                                utterance,
	                                         const std::function<void(QByteArray)> &onChunk)
	{
#if QMUD_ENABLE_TEXT_TO_SPEECH
		if (!hasSpeech())
			return false;
		return invokeSpeechQueued(
		    [utterance = std::move(utterance), onChunk](QTextToSpeech &speechObject)
		    {
			    speechObject.synthesize(utterance, &speechObject,
			                            [onChunk](const auto &format, const auto &data)
			                            {
				                            Q_UNUSED(format);
				                            if (data.isEmpty() || !onChunk)
					                            return;
				                            onChunk(QByteArray(data));
			                            });
		    });
#else
		Q_UNUSED(utterance);
		Q_UNUSED(onChunk);
		return false;
#endif
	}

	void TtsEngine::pause()
	{
#if QMUD_ENABLE_TEXT_TO_SPEECH
		if (!hasSpeech())
			return;
		invokeSpeechBlocking([](QTextToSpeech &speechObject) { speechObject.pause(); });
		isSpeaking = false;
		isPaused   = true;
#endif
	}

	void TtsEngine::resume()
	{
#if QMUD_ENABLE_TEXT_TO_SPEECH
		if (!hasSpeech())
			return;
		invokeSpeechBlocking([](QTextToSpeech &speechObject) { speechObject.resume(); });
		isPaused = false;
#endif
	}

	double TtsEngine::speechRate() const
	{
#if QMUD_ENABLE_TEXT_TO_SPEECH
		if (!hasSpeech())
			return sapiRateToQt(rate);
		return invokeSpeechBlocking([](const QTextToSpeech &speechObject) { return speechObject.rate(); });
#else
		return sapiRateToQt(rate);
#endif
	}

	void TtsEngine::setSpeechRate(const double qtRate) const
	{
#if QMUD_ENABLE_TEXT_TO_SPEECH
		if (!hasSpeech())
			return;
		invokeSpeechBlocking([qtRate](QTextToSpeech &speechObject) { speechObject.setRate(qtRate); });
#else
		Q_UNUSED(qtRate);
#endif
	}

	double TtsEngine::speechVolume() const
	{
#if QMUD_ENABLE_TEXT_TO_SPEECH
		if (!hasSpeech())
			return static_cast<double>(volumePercent) / 100.0;
		return invokeSpeechBlocking([](const QTextToSpeech &speechObject) { return speechObject.volume(); });
#else
		return static_cast<double>(volumePercent) / 100.0;
#endif
	}

	void TtsEngine::setSpeechVolume(const double volume) const
	{
#if QMUD_ENABLE_TEXT_TO_SPEECH
		if (!hasSpeech())
			return;
		invokeSpeechBlocking([volume](QTextToSpeech &speechObject) { speechObject.setVolume(volume); });
#else
		Q_UNUSED(volume);
#endif
	}
} // namespace QMudTtsEngine
