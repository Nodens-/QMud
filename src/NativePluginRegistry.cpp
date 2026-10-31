/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: NativePluginRegistry.cpp
 * Role: Native compatibility plugin registry and MushReader shim implementation.
 */

#include "NativePluginRegistry.h"

#include "AcceleratorUtils.h"
#ifdef QMUD_ENABLE_LUA_SCRIPTING
#include "LuaSupport.h"
#endif
#include "SoundEngine.h"
#include "TtsEngine.h"
#include "WorldOptions.h"
#include "WorldRuntime.h"
#include "scripting/ScriptingErrors.h"
#include <QDir>

#include <QCoreApplication>
#include <QFile>
#include <QLibrary>
#include <QMutex>
// ReSharper disable once CppUnusedIncludeDirective
#include <QMutexLocker>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <objbase.h>
#include <oleauto.h>
#include <windows.h>
#endif

namespace
{
	constexpr auto kMushReaderPluginId = "925cdd0331023d9f0b8f05a7"; // MushReader.xml
	constexpr auto kLuaAudioPluginId   = "aedf0cb0be5bf045860d54b7"; // LuaAudio.xml
	constexpr auto kNativeShimMarker   = "QMud native compatibility shim - Legacy XML ignored.";
	constexpr auto kPpiReturnVariable  = "PPI__returns__PPI_";

	QString        normalizedRoutine(const QString &value)
	{
		return value.trimmed().toLower();
	}

	const QSet<QString> &shimIds()
	{
		static const QSet<QString> ids{QString::fromLatin1(kMushReaderPluginId),
		                               QString::fromLatin1(kLuaAudioPluginId)};
		return ids;
	}

	const QSet<QString> &blacklistedIds()
	{
		static const QSet<QString> ids{
		    QStringLiteral("bb6a05ed7534b5db1ed40511"), // Automatic_Backup.xml
		    QStringLiteral("b8e6dac1ee7fe8e3de931fb7"), // Automatic_Backup_7Days.xml
		    QStringLiteral("8238deec7c06bade8ebc3819"), // AutoSave.xml
		    QStringLiteral("7da63d78d9f91bebb5285127"), // MSP_lua.xml
		    QStringLiteral("1df4fa3b486a36ededa19433")  // MM/msp_lua.xml
		};
		return ids;
	}

	QString qmudNativeSourceName(const QString &name)
	{
		return QStringLiteral("qmud:native/%1").arg(name);
	}

	QString normalizeNativeSourceSyntax(QString source)
	{
		source.replace(QLatin1Char('\\'), QLatin1Char('/'));
		source = QDir::cleanPath(source.trimmed());
		while (source.startsWith(QStringLiteral("./")))
			source.remove(0, 2);

		const QStringList segments = source.split(QLatin1Char('/'), Qt::SkipEmptyParts);
		for (qsizetype i = 0; i + 1 < segments.size(); ++i)
		{
			if (segments.at(i).compare(QStringLiteral("qmud:native"), Qt::CaseInsensitive) != 0)
				continue;
			const QString nativeName = segments.at(i + 1).trimmed();
			if (nativeName.isEmpty())
				return {};
			return qmudNativeSourceName(nativeName);
		}
		return {};
	}

	QMudNativePluginRegistry::NativePluginMetadata mushReaderMetadata()
	{
		QMudNativePluginRegistry::NativePluginMetadata metadata;
		metadata.id      = QString::fromLatin1(kMushReaderPluginId);
		metadata.name    = QStringLiteral("MushReader");
		metadata.author  = QStringLiteral("QMud native compatibility layer");
		metadata.purpose = QStringLiteral("Native screen-reader compatibility shim");
		metadata.description =
		    QStringLiteral("MushReader is implemented natively by QMud. Legacy XML and DLL content with "
		                   "this plugin id is intentionally shadowed and never executed. %1")
		        .arg(QString::fromLatin1(kNativeShimMarker));
		metadata.language               = QStringLiteral("native");
		metadata.source                 = qmudNativeSourceName(metadata.name);
		metadata.directory              = QStringLiteral("qmud:native/");
		metadata.version                = 1.10;
		metadata.requiredVersion        = 0.0;
		metadata.sequence               = QMudPluginSequence::kDefault;
		metadata.availableWithoutShadow = false;
		metadata.dateWritten  = QDateTime::fromString(QStringLiteral("2026-06-06T00:00:00Z"), Qt::ISODate);
		metadata.dateModified = metadata.dateWritten;
		return metadata;
	}

	QMudNativePluginRegistry::NativePluginMetadata luaAudioMetadata()
	{
		QMudNativePluginRegistry::NativePluginMetadata metadata;
		metadata.id      = QString::fromLatin1(kLuaAudioPluginId);
		metadata.name    = QStringLiteral("LuaAudio");
		metadata.author  = QStringLiteral("QMud native compatibility layer");
		metadata.purpose = QStringLiteral("Native legacy audio compatibility shim");
		metadata.description =
		    QStringLiteral("LuaAudio is implemented natively by QMud. Legacy XML and audio.dll content "
		                   "with this plugin id is intentionally shadowed and never executed. %1")
		        .arg(QString::fromLatin1(kNativeShimMarker));
		metadata.language               = QStringLiteral("native");
		metadata.source                 = qmudNativeSourceName(metadata.name);
		metadata.directory              = QStringLiteral("qmud:native/");
		metadata.version                = 1.0;
		metadata.requiredVersion        = 0.0;
		metadata.sequence               = QMudPluginSequence::kDefault;
		metadata.availableWithoutShadow = true;
		metadata.saveState              = true;
		metadata.dateWritten  = QDateTime::fromString(QStringLiteral("2026-06-07T00:00:00Z"), Qt::ISODate);
		metadata.dateModified = metadata.dateWritten;
		return metadata;
	}

	QStringList mushReaderRoutines()
	{
		return {QStringLiteral("say"), QStringLiteral("interrupt"), QStringLiteral("stop"),
		        QStringLiteral("plugin_update_url")};
	}

	/**
	 * @brief Describes one function exposed by the original LuaAudio plugin through PPI.
	 */
	struct LuaAudioPpiEndpoint
	{
			QString   ppiRoutine;
			QString   targetRoutine;
			qsizetype argumentCount{0};
	};

	/**
	 * @brief Lists the direct LuaAudio routines and declared arities exposed by the original plugin through PPI.
	 * @return PPI endpoint definitions in original exposure order.
	 */
	const QList<LuaAudioPpiEndpoint> &luaAudioPpiEndpoints()
	{
		static const QList<LuaAudioPpiEndpoint> endpoints{
		    {QStringLiteral("PPI_init_PPI_"),            QStringLiteral("init"),            0},
		    {QStringLiteral("PPI_unload_PPI_"),          QStringLiteral("unload"),          0},
		    {QStringLiteral("PPI_setPitch_PPI_"),        QStringLiteral("setPitch"),        2},
		    {QStringLiteral("PPI_slidePitch_PPI_"),      QStringLiteral("slidePitch"),      3},
		    {QStringLiteral("PPI_isPlaying_PPI_"),       QStringLiteral("isPlaying"),       1},
		    {QStringLiteral("PPI_getVolume_PPI_"),       QStringLiteral("getVolume"),       1},
		    {QStringLiteral("PPI_stop_PPI_"),            QStringLiteral("stop"),            1},
		    {QStringLiteral("PPI_setPan_PPI_"),          QStringLiteral("setPan"),          2},
		    {QStringLiteral("PPI_play_PPI_"),            QStringLiteral("play"),            4},
		    {QStringLiteral("PPI_playDelay_PPI_"),       QStringLiteral("playDelay"),       4},
		    {QStringLiteral("PPI_playDelayLooped_PPI_"), QStringLiteral("playDelayLooped"), 4},
		    {QStringLiteral("PPI_setVol_PPI_"),          QStringLiteral("setVol"),          2},
		    {QStringLiteral("PPI_slideVol_PPI_"),        QStringLiteral("slideVol"),        3},
		    {QStringLiteral("PPI_fadeout_PPI_"),         QStringLiteral("fadeout"),         2},
		    {QStringLiteral("PPI_slidePan_PPI_"),        QStringLiteral("slidePan"),        3},
		    {QStringLiteral("PPI_playLooped_PPI_"),      QStringLiteral("playLooped"),      1},
		};
		return endpoints;
	}

	/**
	 * @brief Resolves a legacy LuaAudio PPI routine name to its endpoint definition.
	 * @param routine Candidate PPI routine name.
	 * @return Endpoint definition, or `nullptr` when the name is not a PPI endpoint.
	 */
	const LuaAudioPpiEndpoint *findLuaAudioPpiEndpoint(const QString &routine)
	{
		if (!routine.startsWith(QStringLiteral("PPI_"), Qt::CaseInsensitive) &&
		    (routine.isEmpty() || !routine.front().isSpace()))
			return nullptr;
		const QString normalized = normalizedRoutine(routine);
		if (!normalized.startsWith(QStringLiteral("ppi_")))
			return nullptr;
		for (const LuaAudioPpiEndpoint &endpoint : luaAudioPpiEndpoints())
		{
			if (normalized.compare(endpoint.ppiRoutine, Qt::CaseInsensitive) == 0)
				return &endpoint;
		}
		return nullptr;
	}

	/**
	 * @brief Resolves a legacy LuaAudio PPI routine name to its direct native target.
	 * @param routine Candidate PPI routine name.
	 * @return Direct target name, or an empty string when the name is not a PPI endpoint.
	 */
	QString luaAudioPpiTargetRoutineName(const QString &routine)
	{
		const LuaAudioPpiEndpoint *endpoint = findLuaAudioPpiEndpoint(routine);
		return endpoint ? endpoint->targetRoutine : QString();
	}

	/**
	 * @brief Resolves the declared Lua parameter count for a legacy LuaAudio PPI routine.
	 * @param routine Candidate PPI routine name.
	 * @return Nonnegative parameter count, or `-1` when the name is not a PPI endpoint.
	 */
	qsizetype luaAudioPpiArgumentCountValue(const QString &routine)
	{
		const LuaAudioPpiEndpoint *endpoint = findLuaAudioPpiEndpoint(routine);
		return endpoint ? endpoint->argumentCount : -1;
	}

	QStringList luaAudioRoutines()
	{
		QStringList routines;
		routines.reserve(luaAudioPpiEndpoints().size() * 2 + 1);
		for (const LuaAudioPpiEndpoint &endpoint : luaAudioPpiEndpoints())
			routines.push_back(endpoint.targetRoutine);
		routines.push_back(QStringLiteral("plugin_update_url"));
#ifdef QMUD_ENABLE_LUA_SCRIPTING
		for (const LuaAudioPpiEndpoint &endpoint : luaAudioPpiEndpoints())
			routines.push_back(endpoint.ppiRoutine);
#endif
		return routines;
	}

	QString luaQuote(const QString &value)
	{
		QString quoted;
		quoted.reserve(value.size() + 8);
		for (const QChar ch : value)
		{
			switch (ch.unicode())
			{
			case '\\':
				quoted += QStringLiteral("\\\\");
				break;
			case '"':
				quoted += QStringLiteral("\\\"");
				break;
			case '\n':
				quoted += QStringLiteral("\\n");
				break;
			case '\r':
				quoted += QStringLiteral("\\r");
				break;
			case '\t':
				quoted += QStringLiteral("\\t");
				break;
			default:
				quoted += ch;
				break;
			}
		}
		return quoted;
	}

	QString luaUnquote(const QString &value)
	{
		QString unquoted;
		unquoted.reserve(value.size());
		bool escape = false;
		for (const QChar ch : value)
		{
			if (!escape)
			{
				if (ch == QLatin1Char('\\'))
				{
					escape = true;
					continue;
				}
				unquoted += ch;
				continue;
			}
			escape = false;
			switch (ch.unicode())
			{
			case 'n':
				unquoted += QLatin1Char('\n');
				break;
			case 'r':
				unquoted += QLatin1Char('\r');
				break;
			case 't':
				unquoted += QLatin1Char('\t');
				break;
			default:
				unquoted += ch;
				break;
			}
		}
		if (escape)
			unquoted += QLatin1Char('\\');
		return unquoted;
	}

	class ReaderBackend
	{
		public:
			virtual ~ReaderBackend()                                = default;
			virtual bool speak(const QString &text, bool interrupt) = 0;
			virtual bool stop()                                     = 0;
	};

#ifdef Q_OS_WIN
	class NvdaReaderBackend final : public ReaderBackend
	{
		public:
			NvdaReaderBackend()
			{
				const QString appDir    = QCoreApplication::applicationDirPath();
				const QString nativeDir = QDir(appDir).filePath(QStringLiteral("lua/native/windows-x86_64"));
				for (const QString &name : {QStringLiteral("nvdaControllerClient64.dll"),
				                            QStringLiteral("nvdaControllerClient.dll")})
				{
					for (const QString &candidate :
					     {QDir(appDir).filePath(name), QDir(nativeDir).filePath(name), name})
					{
						m_library.setFileName(candidate);
						if (!m_library.load())
							continue;
						m_testIfRunning = reinterpret_cast<TestIfRunning>(
						    m_library.resolve("nvdaController_testIfRunning"));
						m_speakText =
						    reinterpret_cast<SpeakText>(m_library.resolve("nvdaController_speakText"));
						m_cancelSpeech =
						    reinterpret_cast<CancelSpeech>(m_library.resolve("nvdaController_cancelSpeech"));
						if (m_testIfRunning && m_speakText && m_cancelSpeech)
							return;
						m_library.unload();
					}
				}
			}

			bool speak(const QString &text, const bool interrupt) override
			{
				if (!available())
					return false;
				if (interrupt)
					static_cast<void>(m_cancelSpeech());
				return m_speakText(reinterpret_cast<const wchar_t *>(text.utf16())) == 0;
			}

			bool stop() override
			{
				return available() && m_cancelSpeech() == 0;
			}

		private:
			using TestIfRunning = long(__stdcall *)();
			using SpeakText     = long(__stdcall *)(const wchar_t *);
			using CancelSpeech  = long(__stdcall *)();

			bool available() const
			{
				return m_testIfRunning && m_speakText && m_cancelSpeech && m_testIfRunning() == 0;
			}

			QLibrary      m_library;
			TestIfRunning m_testIfRunning{nullptr};
			SpeakText     m_speakText{nullptr};
			CancelSpeech  m_cancelSpeech{nullptr};
	};

	class JawsReaderBackend final : public ReaderBackend
	{
		public:
			JawsReaderBackend()
			{
				const HRESULT initResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
				m_uninitializeCom        = SUCCEEDED(initResult);
				if (!SUCCEEDED(initResult) && initResult != RPC_E_CHANGED_MODE)
					return;

				IDispatch *dispatch = nullptr;
				if (!createDispatch(L"freedomsci.jawsapi", &dispatch) &&
				    !createDispatch(L"jfwapi", &dispatch))
					return;
				m_dispatch = dispatch;

				OLECHAR *name = const_cast<OLECHAR *>(L"SayString");
				if (FAILED(
				        m_dispatch->GetIDsOfNames(IID_NULL, &name, 1, LOCALE_USER_DEFAULT, &m_sayStringId)))
				{
					m_dispatch->Release();
					m_dispatch = nullptr;
				}
			}

			~JawsReaderBackend() override
			{
				if (m_dispatch)
					m_dispatch->Release();
				if (m_uninitializeCom)
					CoUninitialize();
			}

			bool speak(const QString &text, const bool interrupt) override
			{
				return sayString(text, interrupt ? 1 : 0);
			}

			bool stop() override
			{
				return sayString(QString(), 1);
			}

		private:
			static bool createDispatch(const wchar_t *progId, IDispatch **dispatch)
			{
				CLSID clsid;
				if (FAILED(CLSIDFromProgID(progId, &clsid)))
					return false;
				return SUCCEEDED(CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER | CLSCTX_INPROC_SERVER,
				                                  IID_IDispatch, reinterpret_cast<void **>(dispatch)));
			}

			bool sayString(const QString &text, const int interrupt) const
			{
				if (!m_dispatch || m_sayStringId == DISPID_UNKNOWN)
					return false;

				VARIANTARG args[2];
				VariantInit(&args[0]);
				VariantInit(&args[1]);
				args[0].vt      = VT_I4;
				args[0].lVal    = interrupt;
				args[1].vt      = VT_BSTR;
				args[1].bstrVal = SysAllocString(reinterpret_cast<const OLECHAR *>(text.utf16()));
				if (!args[1].bstrVal)
					return false;

				DISPPARAMS params{};
				params.cArgs  = 2;
				params.rgvarg = args;
				VARIANT result;
				VariantInit(&result);
				const HRESULT hr = m_dispatch->Invoke(m_sayStringId, IID_NULL, LOCALE_USER_DEFAULT,
				                                      DISPATCH_METHOD, &params, &result, nullptr, nullptr);
				VariantClear(&result);
				VariantClear(&args[1]);
				return SUCCEEDED(hr);
			}

			IDispatch *m_dispatch{nullptr};
			DISPID     m_sayStringId{DISPID_UNKNOWN};
			bool       m_uninitializeCom{false};
	};
#endif

	class TtsReaderBackend final : public ReaderBackend
	{
		public:
			bool speak(const QString &text, const bool interrupt) override
			{
				if (!m_tts)
					m_tts = QMudTtsEngine::TtsEngine::create();
				if (!m_tts)
					return false;
				m_tts->enqueueUtterance(text, interrupt);
				return true;
			}

			bool stop() override
			{
				if (!m_tts)
					return false;
				m_tts->enqueueUtterance(QString(), true);
				return true;
			}

		private:
			std::shared_ptr<QMudTtsEngine::TtsEngine> m_tts;
	};

	struct MushReaderState
	{
			bool                                        mushReaderPluginEnabled{false};
			bool                                        mushReaderSpeechEnabled{true};
			bool                                        qtAccessibilitySpeechEnabled{true};
			bool                                        substitutionsEnabled{true};
			bool                                        substitutionsLoaded{false};
			bool                                        runtimeSetupComplete{false};
			QMap<QString, QString>                      substitutions;
			std::vector<std::unique_ptr<ReaderBackend>> backends;
	};

	QMutex &stateMutex()
	{
		static QMutex mutex;
		return mutex;
	}

	/**
	 * @brief Returns the mutex protecting LuaAudio runtime state independently of other native shims.
	 * @return Process-wide LuaAudio state mutex.
	 */
	QMutex &luaAudioStateMutex()
	{
		static QMutex mutex;
		return mutex;
	}

	std::unordered_map<const WorldRuntime *, std::shared_ptr<MushReaderState>> &states()
	{
		static std::unordered_map<const WorldRuntime *, std::shared_ptr<MushReaderState>> runtimeStates;
		return runtimeStates;
	}

	QMudNativePluginRegistry::TestSpeechEvent makeTestSpeechEvent(const QString &text, const bool interrupt,
	                                                              const bool stop)
	{
		QMudNativePluginRegistry::TestSpeechEvent event;
		event.text      = text;
		event.interrupt = interrupt;
		event.stop      = stop;
		return event;
	}

	std::function<void(const QMudNativePluginRegistry::TestSpeechEvent &)> &testSpeechSink()
	{
		static std::function<void(const QMudNativePluginRegistry::TestSpeechEvent &)> sink;
		return sink;
	}

	bool dispatchTestSpeechEvent(const QMudNativePluginRegistry::TestSpeechEvent &event)
	{
		std::function<void(const QMudNativePluginRegistry::TestSpeechEvent &)> sink;
		{
			QMutexLocker locker(&stateMutex());
			sink = testSpeechSink();
		}
		if (!sink)
			return false;
		sink(event);
		return true;
	}

	std::shared_ptr<MushReaderState> stateFor(const WorldRuntime *runtime)
	{
		QMutexLocker locker(&stateMutex());
		auto        &slot = states()[runtime];
		if (!slot)
			slot = std::make_shared<MushReaderState>();
		return slot;
	}

	std::shared_ptr<const MushReaderState> existingStateFor(const WorldRuntime *runtime)
	{
		QMutexLocker locker(&stateMutex());
		const auto   it = states().find(runtime);
		return it == states().end() ? std::shared_ptr<const MushReaderState>() : it->second;
	}

	QString substitutionsFilePath(const WorldRuntime *runtime)
	{
		const QString base = runtime->startupDirectory().trimmed();
		return QDir(base.isEmpty() ? QDir::currentPath() : base)
		    .filePath(QStringLiteral("substitutions.mush"));
	}

	void ensureBackends(MushReaderState &state)
	{
		if (!state.backends.empty())
			return;
#ifdef Q_OS_WIN
		state.backends.push_back(std::make_unique<NvdaReaderBackend>());
		state.backends.push_back(std::make_unique<JawsReaderBackend>());
#endif
		state.backends.push_back(std::make_unique<TtsReaderBackend>());
	}

	void loadSubstitutions(const WorldRuntime *runtime, MushReaderState &state)
	{
		if (state.substitutionsLoaded)
			return;
		state.substitutionsLoaded = true;
		const QString path        = substitutionsFilePath(runtime);
		QFile         file(path);
		if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
			return;
		const QString            text = QString::fromUtf8(file.readAll());
		const QRegularExpression statusRegex(QStringLiteral(R"(\bstatus\s*=\s*([01]))"));
		if (const QRegularExpressionMatch match = statusRegex.match(text); match.hasMatch())
			state.substitutionsEnabled = match.captured(1) != QStringLiteral("0");

		const QRegularExpression entryRegex(
		    QStringLiteral(R"rx(\[\s*"((?:\\.|[^"\\])*)"\s*\]\s*=\s*"((?:\\.|[^"\\])*)")rx"));
		auto it = entryRegex.globalMatch(text);
		while (it.hasNext())
		{
			const QRegularExpressionMatch match = it.next();
			state.substitutions.insert(luaUnquote(match.captured(1)), luaUnquote(match.captured(2)));
		}
	}

	bool saveSubstitutions(const WorldRuntime *runtime, const MushReaderState &state)
	{
		const QString path = substitutionsFilePath(runtime);
		QSaveFile     file(path);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
			return false;
		QTextStream out(&file);
		out << "return {\n";
		out << "  status = " << (state.substitutionsEnabled ? 1 : 0) << ",\n";
		for (auto it = state.substitutions.constBegin(); it != state.substitutions.constEnd(); ++it)
			out << "  [\"" << luaQuote(it.key()) << "\"] = \"" << luaQuote(it.value()) << "\",\n";
		out << "}\n";
		return file.commit();
	}

	bool speak(const WorldRuntime *runtime, const QString &text, const bool interrupt)
	{
		if (!runtime)
			return false;
		if (dispatchTestSpeechEvent(makeTestSpeechEvent(text, interrupt, false)))
			return true;
		const std::shared_ptr<MushReaderState> state = stateFor(runtime);
		ensureBackends(*state);
		for (const auto &backend : state->backends)
		{
			if (backend && backend->speak(text, interrupt))
				return true;
		}
		return false;
	}

	bool stopSpeech(const WorldRuntime *runtime)
	{
		if (!runtime)
			return false;
		if (dispatchTestSpeechEvent(makeTestSpeechEvent(QString(), true, true)))
			return true;
		const std::shared_ptr<const MushReaderState> state = existingStateFor(runtime);
		if (!state)
			return false;
		bool stopped = false;
		for (const auto &backend : state->backends)
		{
			if (backend)
				stopped = backend->stop() || stopped;
		}
		return stopped;
	}

	bool effectiveMushReaderSpeechEnabled(const MushReaderState &state)
	{
		return state.mushReaderPluginEnabled && state.mushReaderSpeechEnabled;
	}

	bool effectiveMushReaderSpeechEnabled(const QMudNativePluginRegistry::NativeCallContext &context)
	{
		return context.pluginEnabled && context.mushReaderSpeechEnabled;
	}

	void installMushReaderAccelerator(WorldRuntime *runtime, MushReaderState &state)
	{
		if (state.runtimeSetupComplete)
			return;
		state.runtimeSetupComplete = true;

		quint32 virt = 0;
		quint16 key  = 0;
		if (!AcceleratorUtils::stringToAccelerator(QStringLiteral("Ctrl+Shift+F12"), virt, key))
			return;

		const qint64 mapKey = AcceleratorUtils::acceleratorMapKey(virt, key);
		if (runtime->acceleratorCommandForKey(mapKey) >= 0)
			return;

		const int commandId = runtime->allocateAcceleratorCommand();
		if (commandId < 0)
			return;

		WorldRuntime::AcceleratorEntry entry;
		entry.text     = QStringLiteral("tts");
		entry.sendTo   = eSendToExecute;
		entry.pluginId = QMudNativePluginRegistry::mushReaderPluginId();
		runtime->registerAccelerator(mapKey, commandId, entry);
	}

	void outputLine(WorldRuntime &runtime, const QString &line)
	{
		runtime.outputText(line, true, true);
	}

	void handleSubstitutionCommand(WorldRuntime *runtime, const QString &argument)
	{
		const std::shared_ptr<MushReaderState> state = stateFor(runtime);
		loadSubstitutions(runtime, *state);

		const QString trimmed = argument.trimmed();
		const QString lower   = trimmed.toLower();
		if (trimmed.isEmpty() || lower == QStringLiteral("help"))
		{
			outputLine(*runtime, QStringLiteral(
			                         "subst on|off|list|clear|add <text>==<replacement>|remove <text>|save"));
			return;
		}
		if (lower == QStringLiteral("on"))
		{
			state->substitutionsEnabled = true;
			saveSubstitutions(runtime, *state);
			outputLine(*runtime, QStringLiteral("Substitutions on."));
			return;
		}
		if (lower == QStringLiteral("off"))
		{
			state->substitutionsEnabled = false;
			saveSubstitutions(runtime, *state);
			outputLine(*runtime, QStringLiteral("Substitutions off."));
			return;
		}
		if (lower == QStringLiteral("list"))
		{
			if (state->substitutions.isEmpty())
			{
				outputLine(*runtime, QStringLiteral("No substitutions."));
				return;
			}
			for (auto it = state->substitutions.constBegin(); it != state->substitutions.constEnd(); ++it)
				outputLine(*runtime, QStringLiteral("%1 == %2").arg(it.key(), it.value()));
			return;
		}
		if (lower == QStringLiteral("clear"))
		{
			state->substitutions.clear();
			saveSubstitutions(runtime, *state);
			outputLine(*runtime, QStringLiteral("Substitutions cleared."));
			return;
		}
		if (lower == QStringLiteral("save"))
		{
			outputLine(*runtime, saveSubstitutions(runtime, *state)
			                         ? QStringLiteral("Substitutions saved.")
			                         : QStringLiteral("Unable to save substitutions."));
			return;
		}
		if (lower.startsWith(QStringLiteral("remove ")))
		{
			const QString key = trimmed.mid(7).trimmed();
			state->substitutions.remove(key);
			saveSubstitutions(runtime, *state);
			outputLine(*runtime, QStringLiteral("Substitution removed."));
			return;
		}
		if (lower.startsWith(QStringLiteral("add ")))
		{
			const QString   payload = trimmed.mid(4);
			const qsizetype sep     = payload.indexOf(QStringLiteral("=="));
			if (sep < 0)
			{
				outputLine(*runtime, QStringLiteral("Substitution add requires <text>==<replacement>."));
				return;
			}
			const QString key   = payload.left(sep).trimmed();
			const QString value = payload.mid(sep + 2).trimmed();
			if (key.isEmpty())
			{
				outputLine(*runtime, QStringLiteral("Substitution text cannot be empty."));
				return;
			}
			state->substitutions.insert(key, value);
			saveSubstitutions(runtime, *state);
			outputLine(*runtime, QStringLiteral("Substitution added."));
			return;
		}
		outputLine(*runtime, QStringLiteral("Unknown substitution command."));
	}

	QString substitutionAppliedText(const WorldRuntime *runtime, const QString &text, bool &skip)
	{
		skip                                         = false;
		const std::shared_ptr<MushReaderState> state = stateFor(runtime);
		loadSubstitutions(runtime, *state);
		if (!state->substitutionsEnabled)
		{
			skip = true;
			return {};
		}
		const auto it = state->substitutions.constFind(text);
		if (it == state->substitutions.constEnd())
			return text;
		if (it.value() == QStringLiteral("!skip"))
		{
			skip = true;
			return {};
		}
		return it.value();
	}

	QString firstArgumentString(const QVector<QVariant> &arguments)
	{
		return arguments.isEmpty() ? QString() : arguments.constFirst().toString();
	}

	double luaAudioVolumeToQmudVolume(const double volume)
	{
		return qBound(0.0, volume, 100.0) - 100.0;
	}

	struct LuaAudioNativeState
	{
			QMudNativePluginRegistry::LuaAudioRuntimeMasterState                master;
			QSet<int>                                                           ownedBuffers;
			QHash<int, QMudNativePluginRegistry::LuaAudioRuntimeBufferState>    buffers;
			QHash<QString, int>                                                 loopBuffersByName;
			QHash<int, QMudNativePluginRegistry::LuaAudioRuntimeOperationGuard> operationGuards;
			quint64                                                             nextBufferGeneration{0};
			double                                                              commandVolume{100.0};
			bool                                                                soundsEnabled{true};
	};

	std::unordered_map<const WorldRuntime *, LuaAudioNativeState> &luaAudioNativeStates()
	{
		static std::unordered_map<const WorldRuntime *, LuaAudioNativeState> states;
		return states;
	}

	LuaAudioNativeState &luaAudioNativeStateFor(const WorldRuntime *runtime)
	{
		return luaAudioNativeStates()[runtime];
	}

	void cancelLuaAudioPendingWork(const QMudNativePluginRegistry::LuaAudioRuntimeBufferState &state)
	{
		if (state.pendingCancel)
			state.pendingCancel->store(true, std::memory_order_release);
	}

	/**
	 * @brief Cancels the shared delayed-operation guard for one buffer generation.
	 * @param state Runtime LuaAudio state containing the guard.
	 * @param buffer Buffer whose current operation generation is canceled.
	 */
	void cancelLuaAudioOperationGeneration(LuaAudioNativeState &state, const int buffer)
	{
		const QMudNativePluginRegistry::LuaAudioRuntimeOperationGuard guard =
		    state.operationGuards.take(buffer);
		if (guard.cancelled)
			guard.cancelled->store(true, std::memory_order_release);
	}

	/**
	 * @brief Ensures one shared delayed-operation guard exists for a buffer generation.
	 * @param state Runtime LuaAudio state receiving the guard.
	 * @param buffer Buffer owning the guard.
	 * @param generation Current ownership generation.
	 */
	void ensureLuaAudioOperationGeneration(LuaAudioNativeState &state, const int buffer,
	                                       const quint64 generation)
	{
		const auto pending = state.buffers.value(buffer).pendingCancel;
		auto       guardIt = state.operationGuards.find(buffer);
		if (guardIt != state.operationGuards.end() && guardIt->generation == generation &&
		    guardIt->cancelled && (!pending || guardIt->cancelled == pending) &&
		    !guardIt->cancelled->load(std::memory_order_acquire))
		{
			return;
		}
		if (guardIt != state.operationGuards.end() && guardIt->cancelled)
			guardIt->cancelled->store(true, std::memory_order_release);
		QMudNativePluginRegistry::LuaAudioRuntimeOperationGuard guard;
		guard.generation = generation;
		guard.cancelled  = pending;
		if (!guard.cancelled)
			guard.cancelled = std::make_shared<std::atomic_bool>(false);
		state.operationGuards.insert(buffer, std::move(guard));
	}

	/**
	 * @brief Cancels every buffer-generation guard during runtime-wide teardown.
	 * @param state Runtime LuaAudio state whose guards are canceled.
	 */
	void cancelAllLuaAudioOperationGenerations(LuaAudioNativeState &state)
	{
		for (const QMudNativePluginRegistry::LuaAudioRuntimeOperationGuard &guard :
		     std::as_const(state.operationGuards))
		{
			if (guard.cancelled)
				guard.cancelled->store(true, std::memory_order_release);
		}
	}

	/**
	 * @brief Removes all registry state belonging to one LuaAudio buffer while holding stateMutex().
	 * @param state Runtime LuaAudio state containing the buffer.
	 * @param buffer Buffer whose ownership and delayed work are removed.
	 */
	void eraseLuaAudioBufferState(LuaAudioNativeState &state, const int buffer)
	{
		cancelLuaAudioOperationGeneration(state, buffer);
		state.ownedBuffers.remove(buffer);
		state.buffers.remove(buffer);
		for (auto loopIt = state.loopBuffersByName.begin(); loopIt != state.loopBuffersByName.end();)
		{
			if (loopIt.value() == buffer)
				loopIt = state.loopBuffersByName.erase(loopIt);
			else
				++loopIt;
		}
	}

	/**
	 * @brief Captures initial arguments and ownership metadata independently of live worker controls.
	 * @param loop Whether playback loops.
	 * @param volume Captured initial volume.
	 * @param pan Captured initial balance.
	 * @param pitch Captured initial frequency offset.
	 * @param pendingCancel Optional delayed-play cancellation identity.
	 * @return Initial LuaAudio ownership record.
	 */
	QMudNativePluginRegistry::LuaAudioRuntimeBufferState
	luaAudioInitialBufferState(const bool loop, const double volume, const double pan, const double pitch,
	                           std::shared_ptr<std::atomic_bool> pendingCancel = {})
	{
		QMudNativePluginRegistry::LuaAudioRuntimeBufferState state;
		state.loop              = loop;
		state.volume            = qBound(0.0, volume, 100.0);
		state.pan               = pan;
		state.pitch             = pitch;
		state.pendingCancel     = std::move(pendingCancel);
		state.playbackSubmitted = !state.pendingCancel;
		return state;
	}

	/**
	 * @brief Stops the currently owned generation and optionally captures its exact worker result.
	 * @param runtime Owning runtime.
	 * @param buffer One-based channel.
	 * @param postState Optional immutable post-command state destination.
	 */
	void stopLuaAudioNativeBuffer(const WorldRuntime *runtime, const int buffer,
	                              std::shared_ptr<const SoundEngineSession::Snapshot> *postState = nullptr)
	{
		QMudNativePluginRegistry::LuaAudioRuntimeBufferState state;
		if (!runtime || buffer < 1 || buffer > WorldRuntime::kMaxSoundBuffers ||
		    !QMudNativePluginRegistry::luaAudioRuntimeBufferState(runtime, buffer, state))
		{
			return;
		}
		auto *mutableRuntime = const_cast<WorldRuntime *>(runtime);
		static_cast<void>(mutableRuntime->stopSoundForNativeAudio(buffer, state.generation, postState));
		static_cast<void>(QMudNativePluginRegistry::luaAudioReleaseRuntimeBufferIfGeneration(
		    runtime, buffer, state.generation));
	}

	void stopLuaAudioNativeBuffers(const WorldRuntime *runtime)
	{
		const QList<int> buffers = QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(runtime);
		for (const int buffer : buffers)
			stopLuaAudioNativeBuffer(runtime, buffer);
	}

	QHash<QString, int> luaAudioLoopBuffers(const WorldRuntime *runtime)
	{
		QMutexLocker locker(&luaAudioStateMutex());
		const auto   it = luaAudioNativeStates().find(runtime);
		return it == luaAudioNativeStates().end() ? QHash<QString, int>() : it->second.loopBuffersByName;
	}

	void markLuaAudioLoopBuffer(const WorldRuntime *runtime, const QString &fileName, const int buffer)
	{
		if (fileName.isEmpty() || buffer < 1)
			return;
		QMutexLocker locker(&luaAudioStateMutex());
		const auto   it = luaAudioNativeStates().find(runtime);
		if (it == luaAudioNativeStates().end() || !it->second.ownedBuffers.contains(buffer) ||
		    !it->second.buffers.contains(buffer) || !it->second.buffers.value(buffer).loop)
		{
			return;
		}
		it->second.loopBuffersByName.insert(fileName, buffer);
	}

	int takeLuaAudioLoopBuffer(const WorldRuntime *runtime, const QString &fileName)
	{
		if (fileName.isEmpty())
			return 0;
		QMutexLocker locker(&luaAudioStateMutex());
		const auto   it = luaAudioNativeStates().find(runtime);
		if (it == luaAudioNativeStates().end())
			return 0;
		return it->second.loopBuffersByName.take(fileName);
	}

	void clearLuaAudioLoopBuffers(const WorldRuntime *runtime)
	{
		QMutexLocker locker(&luaAudioStateMutex());
		const auto   it = luaAudioNativeStates().find(runtime);
		if (it != luaAudioNativeStates().end())
			it->second.loopBuffersByName.clear();
	}

	/**
	 * @brief Starts or reserves a generation-scoped LuaAudio playback with captured defaults.
	 * @param runtime Owning sound runtime.
	 * @param fileName Unresolved compatibility path.
	 * @param loop Whether playback loops indefinitely.
	 * @param volume Initial LuaAudio volume.
	 * @param pan Initial balance.
	 * @param delayMs Deliberate playback delay in milliseconds.
	 * @param ownerKey Opaque calling-engine identity used only for teardown.
	 * @param pitch Captured initial frequency offset in units of 100 Hz.
	 * @param postState Optional immutable post-command state destination.
	 * @return Internal one-based channel, or zero when the request fails.
	 */
	int playLuaAudioNativeBuffer(const WorldRuntime *runtime, const QString &fileName, const bool loop,
	                             const double volume, const double pan, const int delayMs,
	                             const void *ownerKey = nullptr, const double pitch = 0.0,
	                             std::shared_ptr<const SoundEngineSession::Snapshot> *postState = nullptr)
	{
		if (fileName.isEmpty() || !qIsFinite(volume) || !qIsFinite(pan) || !qIsFinite(pitch))
			return 0;
		const quint64 generation = QMudNativePluginRegistry::luaAudioNextRuntimeBufferGeneration(runtime);
		if (generation == 0)
			return 0;
		if (delayMs > 0)
		{
			auto     *mutableRuntime = const_cast<WorldRuntime *>(runtime);
			const int buffer = mutableRuntime->reserveSoundBufferForNativeAudio(fileName, generation, volume,
			                                                                    pan, pitch, postState);
			if (buffer < 1)
				return 0;
			auto cancelToken         = std::make_shared<std::atomic_bool>(false);
			auto bufferState         = luaAudioInitialBufferState(loop, volume, pan, pitch, cancelToken);
			bufferState.generation   = generation;
			bufferState.ownerKey     = ownerKey;
			bufferState.soundSession = runtime->soundSessionForNativeAudio();
			QMudNativePluginRegistry::luaAudioMarkRuntimeBuffer(runtime, buffer, bufferState);
			mutableRuntime->confirmSoundBufferForNativeAudio(buffer, generation);
			QPointer<WorldRuntime> runtimeGuard(const_cast<WorldRuntime *>(runtime));
			QTimer::singleShot(delayMs, const_cast<WorldRuntime *>(runtime),
			                   [runtimeGuard, cancelToken, buffer, fileName, generation]
			                   {
				                   if (!runtimeGuard)
					                   return;
				                   QMudNativePluginRegistry::LuaAudioRuntimeBufferState bufferState;
				                   if (!QMudNativePluginRegistry::luaAudioConsumeRuntimeBufferPendingPlay(
				                           runtimeGuard, buffer, generation, cancelToken, bufferState))
					                   return;
				                   int       selectedBuffer = 0;
				                   const int status         = runtimeGuard->playSoundForNativeAudio(
				                       buffer, fileName, bufferState.loop,
				                       luaAudioVolumeToQmudVolume(bufferState.volume), bufferState.pan,
				                       generation, true, &selectedBuffer);
				                   if (status != eOK)
					                   QMudNativePluginRegistry::luaAudioReleaseRuntimeBufferIfGeneration(
					                       runtimeGuard, buffer, generation);
			                   });
			return buffer;
		}
		auto     *mutableRuntime = const_cast<WorldRuntime *>(runtime);
		int       buffer         = 0;
		const int status =
		    mutableRuntime->playSoundForNativeAudio(0, fileName, loop, luaAudioVolumeToQmudVolume(volume),
		                                            pan, generation, false, &buffer, pitch, postState);
		if (status != eOK)
			return 0;
		auto bufferState         = luaAudioInitialBufferState(loop, volume, pan, pitch);
		bufferState.generation   = generation;
		bufferState.ownerKey     = ownerKey;
		bufferState.soundSession = runtime->soundSessionForNativeAudio();
		QMudNativePluginRegistry::luaAudioMarkRuntimeBuffer(runtime, buffer, bufferState);
		mutableRuntime->confirmSoundBufferForNativeAudio(buffer, generation);
		return buffer;
	}

	/**
	 * @brief Applies a finite volume to active voices and both LuaAudio volume defaults.
	 * @param runtime Owning runtime, or null to ignore the update.
	 * @param volume Requested percentage; non-finite values leave all state unchanged.
	 * @param postState Optional immutable post-command state destination.
	 */
	void setLuaAudioMasterVolumeAndCommandVolume(
	    const WorldRuntime *runtime, const double volume,
	    std::shared_ptr<const SoundEngineSession::Snapshot> *postState = nullptr)
	{
		if (!runtime || !qIsFinite(volume))
			return;
		QMudNativePluginRegistry::luaAudioSetRuntimeMasterVolume(runtime, volume, postState);
		QMutexLocker locker(&luaAudioStateMutex());
		luaAudioNativeStateFor(runtime).commandVolume = qBound(0.0, volume, 100.0);
	}

	bool luaAudioSoundsEnabled(const WorldRuntime *runtime)
	{
		QMutexLocker locker(&luaAudioStateMutex());
		const auto   it = luaAudioNativeStates().find(runtime);
		return it == luaAudioNativeStates().end() ? true : it->second.soundsEnabled;
	}

	void setLuaAudioSoundsEnabled(const WorldRuntime *runtime, const bool enabled)
	{
		QMutexLocker locker(&luaAudioStateMutex());
		luaAudioNativeStateFor(runtime).soundsEnabled = enabled;
	}

	double luaAudioCommandVolume(const WorldRuntime *runtime)
	{
		QMutexLocker locker(&luaAudioStateMutex());
		const auto   it = luaAudioNativeStates().find(runtime);
		return it == luaAudioNativeStates().end() ? 100.0 : it->second.commandVolume;
	}

	bool isLuaAudioShadowPluginEnabled(const WorldRuntime *runtime)
	{
		if (!runtime)
			return false;
		const QString audioId = QMudNativePluginRegistry::luaAudioPluginId();
		return std::ranges::any_of(runtime->plugins(),
		                           [&audioId](const WorldRuntime::Plugin &plugin)
		                           {
			                           const QString pluginId = plugin.attributes.value(QStringLiteral("id"));
			                           return plugin.enabled && !plugin.installPending &&
			                                  plugin.sequence >= 0 && pluginId == audioId;
		                           });
	}

	void handleLuaAudioVolumeCommand(WorldRuntime *runtime, const int delta)
	{
		if (!luaAudioSoundsEnabled(runtime))
		{
			outputLine(
			    *runtime,
			    QStringLiteral(
			        "You're not supposed to use this function while sounds are off you weird person."));
			return;
		}
		double volume = luaAudioCommandVolume(runtime);
		if (delta < 0 && volume <= 0.0)
		{
			outputLine(*runtime, QStringLiteral("It's muted you noob."));
			return;
		}
		if (delta > 0 && volume >= 100.0)
		{
			outputLine(*runtime, QStringLiteral("Volume range can't be greater than 100."));
			return;
		}
		volume = qBound(0.0, volume + static_cast<double>(delta), 100.0);
		setLuaAudioMasterVolumeAndCommandVolume(runtime, volume);
		runtime->setPluginVariableValue(QMudNativePluginRegistry::luaAudioPluginId(), QStringLiteral("vol"),
		                                QString::number(volume));
		static_cast<void>(runtime->savePluginState(QMudNativePluginRegistry::luaAudioPluginId(), true));
	}

	/**
	 * @brief Validates and executes every LuaAudio entry through one operation path.
	 * @param runtime Owning runtime or null during validation.
	 * @param routine Original DLL function or XML wrapper name.
	 * @param arguments Primitive arguments retaining nil positions.
	 * @param executionMode Whether to perform side effects.
	 * @param context Dispatch-local reads and write visibility.
	 * @return XML wrapper results or a routine error.
	 */
	QMudNativePluginRegistry::NativeCallResult
	callLuaAudioRoutine(WorldRuntime *runtime, const QString &routine, const QVector<QVariant> &arguments,
	                    const QMudNativePluginRegistry::NativeCallExecutionMode executionMode,
	                    const QMudNativePluginRegistry::LuaAudioCallContext    &context = {})
	{
		using namespace QMudNativePluginRegistry;
		NativeCallResult result;
		result.errorCode = eOK;
		QString name     = normalizedRoutine(routine);
#ifdef QMUD_ENABLE_LUA_SCRIPTING
		if (const QString target = luaAudioPpiTargetRoutineName(routine); !target.isEmpty())
		{
			QVector<QVariant> decoded;
			QString           error;
			if (arguments.isEmpty() ||
			    !QMudLuaSupport::decodePpiArguments(firstArgumentString(arguments),
			                                        luaAudioPpiArgumentCountValue(routine), decoded, error))
			{
				result.errorCode = eErrorCallingPluginRoutine;
				result.errorText =
				    error.isEmpty() ? QStringLiteral("Missing serialized PPI arguments") : error;
				return result;
			}
			result = callLuaAudioRoutine(runtime, target, decoded, executionMode, context);
			if (result.errorCode == eOK && executionMode == NativeCallExecutionMode::ExecuteSideEffects)
			{
				if (!runtime)
				{
					result.errorCode = eErrorCallingPluginRoutine;
					result.errorText = QStringLiteral("LuaAudio PPI runtime is unavailable");
				}
				else
					runtime->setPluginVariableValue(
					    luaAudioPluginId(), luaAudioPpiReturnVariableName(),
					    QMudLuaSupport::serializePpiReturnValues(result.returnValues));
			}
			result.returnValues.clear();
			return result;
		}
#endif
		if (name == QStringLiteral("volume"))
			name = QStringLiteral("setvol");
		if (name == QStringLiteral("pan"))
			name = QStringLiteral("setpan");
		if (name == QStringLiteral("pitch"))
			name = QStringLiteral("setpitch");
		const bool execute = executionMode == NativeCallExecutionMode::ExecuteSideEffects && runtime;
		auto       number  = [&](const qsizetype index, const std::optional<double> fallback = {})
		{
			bool   ok    = false;
			double value = 0.0;
			if (index >= arguments.size() || !arguments.at(index).isValid() || arguments.at(index).isNull())
			{
				ok    = fallback.has_value();
				value = fallback.value_or(0.0);
			}
			else if (arguments.at(index).typeId() != QMetaType::Bool)
				value = arguments.at(index).toDouble(&ok);
			if (!ok || !qIsFinite(value))
			{
				result.errorCode = eErrorCallingPluginRoutine;
				result.errorText = QStringLiteral("Bad argument #%1 to LuaAudio '%2' (number expected)")
				                       .arg(index + 1)
				                       .arg(routine);
				return 0.0;
			}
			return std::trunc(value);
		};
		auto bufferArgument = [&](const qsizetype index, const std::optional<double> fallback = {})
		{
			const double value = number(index, fallback);
			return value >= 0 && value <= WorldRuntime::kMaxSoundBuffers ? static_cast<int>(value) : -1;
		};
		auto milliseconds = [&](const qsizetype index, const std::optional<double> fallback = {})
		{
			const double value = number(index, fallback);
			if (value < 0 || value > std::numeric_limits<int>::max())
			{
				result.errorCode = eErrorCallingPluginRoutine;
				result.errorText =
				    QStringLiteral("LuaAudio duration is outside the supported millisecond range");
				return std::chrono::milliseconds{0};
			}
			return std::chrono::milliseconds{static_cast<int>(value)};
		};
		auto status = [&](const int buffer)
		{
			return context.status ? context.status(buffer) : runtime ? runtime->soundStatus(buffer) : -3;
		};
		auto volume = [&](const int buffer, const LuaAudioRuntimeBufferState &)
		{
			return context.volume ? context.volume(buffer)
			       : runtime      ? runtime->soundVolumeForNativeAudio(buffer)
			                      : 0.0;
		};
		auto changed = [&](const int buffer, const std::shared_ptr<const SoundEngineSession::Snapshot> &state)
		{
			if (context.changed && state)
				context.changed(buffer, state);
		};
		auto changedVolumes =
		    [&](const QList<int> &buffers, const std::shared_ptr<const SoundEngineSession::Snapshot> &state)
		{
			for (const int buffer : buffers)
				changed(buffer, state);
		};
		if (name == QStringLiteral("freq"))
		{
			const double value = number(0);
			if (result.errorCode == eOK && execute)
				luaAudioSetRuntimeMasterPitch(runtime, value);
			return result;
		}
		if (name == QStringLiteral("plugin_update_url"))
		{
			result.returnValues.push_back(QStringLiteral("qmud:native/LuaAudio"));
			return result;
		}
		if (name == QStringLiteral("isplaying") || name == QStringLiteral("getvolume"))
		{
			const int buffer = bufferArgument(
			    0, name == QStringLiteral("getvolume") ? std::optional<double>(0) : std::nullopt);
			LuaAudioRuntimeBufferState state;
			const bool owned = runtime && buffer > 0 && luaAudioRuntimeBufferState(runtime, buffer, state);
			if (result.errorCode != eOK)
				return result;
			if (name == QStringLiteral("isplaying"))
			{
				const int current = owned ? status(buffer) : -3;
				result.returnValues.push_back(current == 1 || current == 2 ? 1 : 0);
			}
			else
				result.returnValues.push_back(buffer == 0 ? luaAudioRuntimeMasterState(runtime).volume
				                              : owned     ? volume(buffer, state)
				                                          : 0.0);
			return result;
		}
		if (name == QStringLiteral("play") || name == QStringLiteral("playlooped") ||
		    name == QStringLiteral("playdelay") || name == QStringLiteral("playdelaylooped"))
		{
			QString file;
			if (arguments.isEmpty() || arguments.front().isNull() ||
			    (arguments.front().typeId() != QMetaType::QString &&
			     !arguments.front().canConvert<double>()) ||
			    arguments.front().typeId() == QMetaType::Bool)
			{
				result.errorCode = eErrorCallingPluginRoutine;
				result.errorText = QStringLiteral("Bad argument #1 to LuaAudio play (string expected)");
				return result;
			}
			file                 = arguments.front().toString();
			const auto   master  = luaAudioRuntimeMasterState(runtime);
			const bool   delayed = name.startsWith(QStringLiteral("playdelay"));
			const bool   loop    = name == QStringLiteral("playlooped") ||
			                       name == QStringLiteral("playdelaylooped") ||
			                       (name == QStringLiteral("play") && number(1) != 0.0);
			const auto   delay   = delayed ? milliseconds(1, name == QStringLiteral("playdelaylooped")
			                                                     ? std::optional<double>(250)
			                                                     : std::nullopt)
			                               : std::chrono::milliseconds{0};
			const double pan     = name == QStringLiteral("playlooped") ? master.pan : number(2, master.pan);
			const double gain =
			    name == QStringLiteral("playlooped") ? master.volume : number(3, master.volume);
			if (result.errorCode != eOK)
				return result;
			std::shared_ptr<const SoundEngineSession::Snapshot> postState;
			const int buffer = execute ? playLuaAudioNativeBuffer(runtime, file, loop, gain, pan,
			                                                      static_cast<int>(delay.count()),
			                                                      context.ownerKey, master.pitch, &postState)
			                           : 0;
			if (buffer > 0)
				changed(buffer, postState);
			result.returnValues.push_back(delayed ? QVariant{} : QVariant{buffer});
			return result;
		}
		if (name == QStringLiteral("free") || name == QStringLiteral("unload") ||
		    name == QStringLiteral("init"))
		{
			if (execute)
			{
				if (name != QStringLiteral("init"))
				{
					const auto owned = luaAudioRuntimeOwnedBuffers(runtime);
					std::shared_ptr<const SoundEngineSession::Snapshot> stoppedState;
					static_cast<void>(runtime->setLuaAudioInitialized(false, &stoppedState));
					for (const int buffer : owned)
					{
						std::shared_ptr<const SoundEngineSession::Snapshot> postState;
						stopLuaAudioNativeBuffer(runtime, buffer, &postState);
						changed(buffer, postState ? postState : stoppedState);
					}
				}
				else
				{
					static_cast<void>(runtime->setLuaAudioInitialized(true));
					std::optional<QString> stored;
					if (context.storedVolume)
						stored = context.storedVolume();
					else
					{
						QString value;
						if (runtime->findPluginVariable(luaAudioPluginId(), QStringLiteral("vol"), value))
							stored = value;
					}
					bool         ok       = true;
					const double restored = stored ? stored->toDouble(&ok) : 100.0;
					if (!ok || !qIsFinite(restored))
					{
						result.errorCode = eErrorCallingPluginRoutine;
						result.errorText = QStringLiteral("LuaAudio's stored volume is not a number");
						return result;
					}
					const auto owned = luaAudioRuntimeOwnedBuffers(runtime);
					std::shared_ptr<const SoundEngineSession::Snapshot> postState;
					luaAudioRestorePersistentVolume(runtime, std::trunc(restored), &postState);
					changedVolumes(owned, postState);
					if (!stored)
					{
						if (context.storeVolume)
						{
							if (!context.storeVolume(QStringLiteral("100")))
							{
								result.errorCode = eErrorCallingPluginRoutine;
								result.errorText = QStringLiteral("Unable to save LuaAudio's initial volume");
							}
						}
						else
						{
							runtime->setPluginVariableValue(luaAudioPluginId(), QStringLiteral("vol"),
							                                QStringLiteral("100"));
							static_cast<void>(runtime->savePluginState(luaAudioPluginId(), true));
						}
					}
				}
			}
			return result;
		}
		const bool stop   = name == QStringLiteral("stop");
		const bool fade   = name == QStringLiteral("fadeout");
		const bool slide  = name == QStringLiteral("slidevol") || name == QStringLiteral("slidepan") ||
		                    name == QStringLiteral("slidepitch");
		const bool setter = name == QStringLiteral("setvol") || name == QStringLiteral("setpan") ||
		                    name == QStringLiteral("setpitch");
		if (stop || fade || slide || setter)
		{
			const auto   attribute = name.endsWith(QStringLiteral("pan"))     ? QMudSoundAttribute::Pan
			                         : name.endsWith(QStringLiteral("pitch")) ? QMudSoundAttribute::Pitch
			                                                                  : QMudSoundAttribute::Volume;
			const double value     = stop || fade ? 0.0 : number(0);
			const int    buffer    = bufferArgument(
			    stop || fade ? 0 : 1,
			    setter && attribute != QMudSoundAttribute::Pitch ? std::optional<double>(0) : std::nullopt);
			const auto duration =
			    fade || slide ? milliseconds(fade ? 1 : 2, 1000) : std::chrono::milliseconds{0};
			if (result.errorCode != eOK || !execute || buffer < 0)
				return result;
			if (buffer == 0 && setter)
			{
				if (attribute == QMudSoundAttribute::Pitch)
					return result;
				if (attribute == QMudSoundAttribute::Volume)
				{
					const auto owned = luaAudioRuntimeOwnedBuffers(runtime);
					std::shared_ptr<const SoundEngineSession::Snapshot> postState;
					luaAudioSetRuntimeMasterVolume(runtime, value, &postState);
					changedVolumes(owned, postState);
				}
				else if (attribute == QMudSoundAttribute::Pan)
					luaAudioSetRuntimeMasterPan(runtime, value);
				return result;
			}
			if (buffer == 0 && !stop)
				return result;
			const QList<int> buffers =
			    buffer == 0 ? luaAudioRuntimeOwnedBuffers(runtime) : QList<int>{buffer};
			for (const int owned : buffers)
			{
				LuaAudioRuntimeBufferState state;
				if (!luaAudioRuntimeBufferState(runtime, owned, state))
					continue;
				if (stop)
				{
					std::shared_ptr<const SoundEngineSession::Snapshot> postState;
					stopLuaAudioNativeBuffer(runtime, owned, &postState);
					changed(owned, postState);
					continue;
				}
				const double target =
				    attribute == QMudSoundAttribute::Volume ? qBound(0.0, value, 100.0) : value;
				std::shared_ptr<const SoundEngineSession::Snapshot> postState;
				if (runtime->setSoundAttributeForNativeAudio(
				        owned, state.generation, attribute, target, duration, fade,
				        name == QStringLiteral("slidepitch") ? QMudSoundAttributeTarget::Relative
				                                             : QMudSoundAttributeTarget::Absolute,
				        &postState) != eOK)
					continue;
				changed(owned, postState);
			}
			return result;
		}
		result.errorCode = eNoSuchRoutine;
		result.errorText = QStringLiteral("No function '%1' in native shim 'LuaAudio' (%2)")
		                       .arg(routine, luaAudioPluginId());
		return result;
	}

} // namespace

namespace QMudNativePluginRegistry
{
	NativeCallResult callLuaAudio(WorldRuntime *runtime, const QString &routine,
	                              const QVector<QVariant> &arguments, const LuaAudioCallContext &context,
	                              const NativeCallExecutionMode executionMode)
	{
		return callLuaAudioRoutine(runtime, routine, arguments, executionMode, context);
	}
	QString nativeShimMarkerText()
	{
		return QString::fromLatin1(kNativeShimMarker);
	}

	QString mushReaderPluginId()
	{
		return QString::fromLatin1(kMushReaderPluginId);
	}

	QString luaAudioPluginId()
	{
		return QString::fromLatin1(kLuaAudioPluginId);
	}

	QString luaAudioPpiTargetRoutine(const QString &routine)
	{
		return luaAudioPpiTargetRoutineName(routine);
	}

	qsizetype luaAudioPpiArgumentCount(const QString &routine)
	{
		return luaAudioPpiArgumentCountValue(routine);
	}

	QString luaAudioPpiReturnVariableName()
	{
		return QString::fromLatin1(kPpiReturnVariable);
	}

	bool metadataForShim(const QString &pluginId, NativePluginMetadata &metadata)
	{
		const QString &id = pluginId;
		if (id == QString::fromLatin1(kMushReaderPluginId))
		{
			metadata = mushReaderMetadata();
			return true;
		}
		if (id == QString::fromLatin1(kLuaAudioPluginId))
		{
			metadata = luaAudioMetadata();
			return true;
		}
		return false;
	}

	bool metadataForNativeName(const QString &nativeName, NativePluginMetadata &metadata)
	{
		for (const QString &shimId : shimIds())
		{
			NativePluginMetadata candidate;
			if (metadataForShim(shimId, candidate) &&
			    candidate.name.compare(nativeName.trimmed(), Qt::CaseInsensitive) == 0)
			{
				metadata = candidate;
				return true;
			}
		}
		return false;
	}

	QString normalizeNativeSource(const QString &source)
	{
		QString normalizedSource = normalizeNativeSourceSyntax(source);
		if (normalizedSource.isEmpty())
			return {};
		const QString nativeName = normalizedSource.mid(QStringLiteral("qmud:native/").size()).trimmed();
		if (nativeName.isEmpty())
			return {};
		NativePluginMetadata metadata;
		if (metadataForNativeName(nativeName, metadata))
			return metadata.source;
		return normalizedSource;
	}

	bool metadataForNativeSource(const QString &source, NativePluginMetadata &metadata)
	{
		const QString normalizedSource = normalizeNativeSourceSyntax(source);
		if (normalizedSource.isEmpty())
			return false;
		const QString nativeName = normalizedSource.mid(QStringLiteral("qmud:native/").size()).trimmed();
		if (nativeName.isEmpty())
			return false;
		return metadataForNativeName(nativeName, metadata);
	}

	QStringList supportedRoutines(const QString &pluginId)
	{
		const QString &id = pluginId;
		if (id == QString::fromLatin1(kMushReaderPluginId))
			return mushReaderRoutines();
		if (id == QString::fromLatin1(kLuaAudioPluginId))
			return luaAudioRoutines();
		return {};
	}

	LuaAudioRuntimeMasterState luaAudioRuntimeMasterState(const WorldRuntime *runtime)
	{
		if (!runtime)
			return {};
		QMutexLocker locker(&luaAudioStateMutex());
		const auto   it = luaAudioNativeStates().find(runtime);
		return it == luaAudioNativeStates().end() ? LuaAudioRuntimeMasterState{} : it->second.master;
	}

	void luaAudioSetRuntimeMasterVolume(const WorldRuntime *runtime, const double volume,
	                                    std::shared_ptr<const SoundEngineSession::Snapshot> *postState)
	{
		if (!runtime || !qIsFinite(volume))
			return;
		const double bounded = qBound(0.0, volume, 100.0);
		{
			QMutexLocker locker(&luaAudioStateMutex());
			luaAudioNativeStateFor(runtime).master.volume = bounded;
		}
		static_cast<void>(
		    runtime->setSoundAttributeForNativeAudio(0, 0, QMudSoundAttribute::Volume, bounded, {}, false,
		                                             QMudSoundAttributeTarget::Absolute, postState));
	}

	void luaAudioRestorePersistentVolume(const WorldRuntime *runtime, const double volume,
	                                     std::shared_ptr<const SoundEngineSession::Snapshot> *postState)
	{
		if (!runtime)
			return;
		setLuaAudioMasterVolumeAndCommandVolume(runtime, volume, postState);
	}

	void luaAudioSetRuntimeMasterPan(const WorldRuntime *runtime, const double pan)
	{
		if (!runtime || !qIsFinite(pan))
			return;
		QMutexLocker locker(&luaAudioStateMutex());
		luaAudioNativeStateFor(runtime).master.pan = pan;
	}

	void luaAudioSetRuntimeMasterPitch(const WorldRuntime *runtime, const double pitch)
	{
		if (!runtime || !qIsFinite(pitch))
			return;
		QMutexLocker locker(&luaAudioStateMutex());
		luaAudioNativeStateFor(runtime).master.pitch = pitch;
	}

	quint64 luaAudioNextRuntimeBufferGeneration(const WorldRuntime *runtime)
	{
		if (!runtime)
			return 0;
		QMutexLocker locker(&luaAudioStateMutex());
		return ++luaAudioNativeStateFor(runtime).nextBufferGeneration;
	}

	void luaAudioMarkRuntimeBuffer(const WorldRuntime *runtime, const int buffer,
	                               const LuaAudioRuntimeBufferState &bufferState)
	{
		if (!runtime || buffer < 1 || buffer > WorldRuntime::kMaxSoundBuffers)
			return;
		QMutexLocker               locker(&luaAudioStateMutex());
		LuaAudioNativeState       &state    = luaAudioNativeStateFor(runtime);
		LuaAudioRuntimeBufferState newState = bufferState;
		if (newState.generation == 0)
		{
			if (state.buffers.contains(buffer))
				newState.generation = state.buffers.value(buffer).generation;
			else
				newState.generation = ++state.nextBufferGeneration;
		}
		else if (newState.generation > state.nextBufferGeneration)
		{
			state.nextBufferGeneration = newState.generation;
		}
		const auto currentIt = state.buffers.constFind(buffer);
		if (currentIt != state.buffers.constEnd() && currentIt->generation != newState.generation)
		{
			eraseLuaAudioBufferState(state, buffer);
		}
		else if (currentIt != state.buffers.constEnd() && currentIt->pendingCancel &&
		         currentIt->pendingCancel != newState.pendingCancel)
		{
			cancelLuaAudioPendingWork(currentIt.value());
		}
		state.ownedBuffers.insert(buffer);
		state.buffers[buffer] = newState;
		ensureLuaAudioOperationGeneration(state, buffer, newState.generation);
	}

	bool luaAudioUpdateRuntimeBufferIfGeneration(const WorldRuntime *runtime, const int buffer,
	                                             const LuaAudioRuntimeBufferState &bufferState)
	{
		if (!runtime || buffer < 1 || buffer > WorldRuntime::kMaxSoundBuffers || bufferState.generation == 0)
			return false;
		QMutexLocker locker(&luaAudioStateMutex());
		const auto   stateIt = luaAudioNativeStates().find(runtime);
		if (stateIt == luaAudioNativeStates().end() || !stateIt->second.ownedBuffers.contains(buffer))
			return false;
		auto bufferIt = stateIt->second.buffers.find(buffer);
		if (bufferIt == stateIt->second.buffers.end() || bufferIt->generation != bufferState.generation)
			return false;
		if (bufferIt->pendingCancel && bufferIt->pendingCancel != bufferState.pendingCancel)
			cancelLuaAudioPendingWork(bufferIt.value());
		bufferIt.value() = bufferState;
		return true;
	}

	bool luaAudioCaptureRuntimeBufferOperationGuard(const WorldRuntime *runtime, const int buffer,
	                                                LuaAudioRuntimeOperationGuard &guard)
	{
		if (!runtime || buffer < 1 || buffer > WorldRuntime::kMaxSoundBuffers)
			return false;
		QMutexLocker locker(&luaAudioStateMutex());
		const auto   it = luaAudioNativeStates().find(runtime);
		if (it == luaAudioNativeStates().end() || !it->second.ownedBuffers.contains(buffer) ||
		    !it->second.buffers.contains(buffer) || !it->second.operationGuards.contains(buffer))
		{
			return false;
		}
		const LuaAudioRuntimeBufferState    &bufferState = it->second.buffers.value(buffer);
		const LuaAudioRuntimeOperationGuard &current     = it->second.operationGuards.value(buffer);
		if (current.generation != bufferState.generation || !current.cancelled ||
		    current.cancelled->load(std::memory_order_acquire))
		{
			return false;
		}
		guard = current;
		return true;
	}

	bool luaAudioResolveRuntimeBufferOperation(const WorldRuntime *runtime, const int buffer,
	                                           const LuaAudioRuntimeOperationGuard &guard,
	                                           LuaAudioRuntimeBufferState          &bufferState)
	{
		if (!runtime || buffer < 1 || buffer > WorldRuntime::kMaxSoundBuffers || guard.generation == 0 ||
		    !guard.cancelled || guard.cancelled->load(std::memory_order_acquire))
		{
			return false;
		}
		QMutexLocker locker(&luaAudioStateMutex());
		const auto   it = luaAudioNativeStates().find(runtime);
		if (it == luaAudioNativeStates().end() || !it->second.ownedBuffers.contains(buffer) ||
		    !it->second.buffers.contains(buffer) || !it->second.operationGuards.contains(buffer))
		{
			return false;
		}
		const LuaAudioRuntimeBufferState    &currentState = it->second.buffers.value(buffer);
		const LuaAudioRuntimeOperationGuard &currentGuard = it->second.operationGuards.value(buffer);
		if (currentState.generation != guard.generation || currentGuard.generation != guard.generation ||
		    currentGuard.cancelled != guard.cancelled ||
		    currentGuard.cancelled->load(std::memory_order_acquire))
		{
			return false;
		}
		bufferState = currentState;
		return true;
	}

	bool luaAudioConsumeRuntimeBufferPendingPlay(const WorldRuntime *runtime, const int buffer,
	                                             const quint64                            generation,
	                                             const std::shared_ptr<std::atomic_bool> &cancelToken,
	                                             LuaAudioRuntimeBufferState              &bufferState)
	{
		if (!runtime || buffer < 1 || buffer > WorldRuntime::kMaxSoundBuffers || generation == 0 ||
		    !cancelToken || cancelToken->load(std::memory_order_acquire))
		{
			return false;
		}
		QMutexLocker locker(&luaAudioStateMutex());
		const auto   it = luaAudioNativeStates().find(runtime);
		if (it == luaAudioNativeStates().end() || !it->second.ownedBuffers.contains(buffer) ||
		    !it->second.buffers.contains(buffer))
		{
			return false;
		}
		LuaAudioRuntimeBufferState current = it->second.buffers.value(buffer);
		if (current.generation != generation || current.pendingCancel != cancelToken)
			return false;
		current.pendingCancel.reset();
		current.playbackSubmitted  = true;
		it->second.buffers[buffer] = current;
		bufferState                = current;
		return true;
	}

	bool luaAudioRuntimeBufferState(const WorldRuntime *runtime, const int buffer,
	                                LuaAudioRuntimeBufferState &bufferState)
	{
		if (!runtime || buffer < 1 || buffer > WorldRuntime::kMaxSoundBuffers)
			return false;
		QMutexLocker locker(&luaAudioStateMutex());
		const auto   it = luaAudioNativeStates().find(runtime);
		if (it == luaAudioNativeStates().end() || !it->second.ownedBuffers.contains(buffer) ||
		    !it->second.buffers.contains(buffer))
		{
			return false;
		}
		bufferState = it->second.buffers.value(buffer);
		return true;
	}

	QList<int> luaAudioRuntimeOwnedBuffers(const WorldRuntime *runtime, const void *ownerKey)
	{
		if (!runtime)
			return {};
		QMutexLocker locker(&luaAudioStateMutex());
		const auto   it = luaAudioNativeStates().find(runtime);
		if (it == luaAudioNativeStates().end())
			return {};
		QList<int> buffers;
		for (const int buffer : it->second.ownedBuffers)
		{
			if (!ownerKey || it->second.buffers.value(buffer).ownerKey == ownerKey)
				buffers.push_back(buffer);
		}
		return buffers;
	}

	void luaAudioReleaseRuntimeBuffer(const WorldRuntime *runtime, const int buffer)
	{
		if (!runtime || buffer < 1 || buffer > WorldRuntime::kMaxSoundBuffers)
			return;
		QMutexLocker locker(&luaAudioStateMutex());
		const auto   it = luaAudioNativeStates().find(runtime);
		if (it == luaAudioNativeStates().end())
			return;
		eraseLuaAudioBufferState(it->second, buffer);
	}

	bool luaAudioReleaseRuntimeBufferIfGeneration(const WorldRuntime *runtime, const int buffer,
	                                              const quint64 generation)
	{
		if (!runtime || buffer < 1 || buffer > WorldRuntime::kMaxSoundBuffers || generation == 0)
			return false;
		QMutexLocker locker(&luaAudioStateMutex());
		const auto   it = luaAudioNativeStates().find(runtime);
		if (it == luaAudioNativeStates().end() || !it->second.buffers.contains(buffer) ||
		    it->second.buffers.value(buffer).generation != generation)
		{
			return false;
		}
		eraseLuaAudioBufferState(it->second, buffer);
		return true;
	}

	void luaAudioReleaseRuntimeBuffers(const WorldRuntime *runtime, const QList<int> &buffers)
	{
		for (const int buffer : buffers)
			luaAudioReleaseRuntimeBuffer(runtime, buffer);
	}

	void luaAudioReleaseRuntimeOwner(const WorldRuntime *runtime, const void *ownerKey)
	{
		if (!runtime || !ownerKey)
			return;
		luaAudioReleaseRuntimeBuffers(runtime, luaAudioRuntimeOwnedBuffers(runtime, ownerKey));
	}

	void luaAudioStopRuntime(const WorldRuntime *runtime)
	{
		if (!runtime)
			return;
		const QList<int> buffers = luaAudioRuntimeOwnedBuffers(runtime);
		for (const int buffer : buffers)
			stopLuaAudioNativeBuffer(runtime, buffer);
	}

	void luaAudioStopRuntimeOwner(const WorldRuntime *runtime, const void *ownerKey)
	{
		if (!ownerKey)
			return;
		const QList<int> buffers = luaAudioRuntimeOwnedBuffers(runtime, ownerKey);
		for (const int buffer : buffers)
			stopLuaAudioNativeBuffer(runtime, buffer);
		/** @brief Generation-scoped owner cleanup that never retains or dereferences another runtime. */
		struct OwnedSessionChannel
		{
				std::shared_ptr<SoundEngineSession> session;
				int                                 buffer;
				quint64                             generation;
		};
		QList<OwnedSessionChannel> others;
		{
			QMutexLocker locker(&luaAudioStateMutex());
			for (const auto &[runtimeKey, state] : luaAudioNativeStates())
			{
				Q_UNUSED(runtimeKey);
				for (auto it = state.buffers.cbegin(); it != state.buffers.cend(); ++it)
					if (it->ownerKey == ownerKey)
						if (auto session = it->soundSession.lock())
							others.push_back({std::move(session), it.key(), it->generation});
			}
		}
		for (const auto &voice : others)
			static_cast<void>(voice.session->stopLuaAudioChannel(voice.buffer, voice.generation));
	}

	void luaAudioResetRuntime(const WorldRuntime *runtime)
	{
		if (!runtime)
			return;
		luaAudioStopRuntime(runtime);
		QMutexLocker locker(&luaAudioStateMutex());
		if (const auto it = luaAudioNativeStates().find(runtime); it != luaAudioNativeStates().end())
		{
			cancelAllLuaAudioOperationGenerations(it->second);
			luaAudioNativeStates().erase(it);
		}
	}

	bool isShimId(const QString &pluginId)
	{
		return shimIds().contains(pluginId);
	}

	bool isBlacklistedId(const QString &pluginId)
	{
		return blacklistedIds().contains(pluginId);
	}

	bool isProtectedId(const QString &pluginId)
	{
		return isShimId(pluginId) || isBlacklistedId(pluginId);
	}

	QString resolveShimIdOrName(const QString &pluginIdOrName)
	{
		const QString &key = pluginIdOrName;
		if (key.isEmpty())
			return {};
		if (isShimId(key))
			return key;
		for (const QString &shimId : shimIds())
		{
			NativePluginMetadata metadata;
			if (metadataForShim(shimId, metadata) &&
			    metadata.name.compare(pluginIdOrName, Qt::CaseInsensitive) == 0)
			{
				return metadata.id;
			}
		}
		return {};
	}

	int pluginSupports(const QString &pluginId, const QString &routine)
	{
		const QString normalized = normalizedRoutine(routine);
		if (!isShimId(pluginId))
			return eNoSuchPlugin;
		if (normalized.isEmpty())
			return eNoSuchRoutine;
		for (const QString &candidate : supportedRoutines(pluginId))
		{
			if (candidate.compare(normalized, Qt::CaseInsensitive) == 0)
				return eOK;
		}
		return eNoSuchRoutine;
	}

	QVariant pluginInfo(const QString &pluginId, const int infoType, const int visibleIndex,
	                    const bool enabled)
	{
		NativePluginMetadata metadata;
		if (!metadataForShim(pluginId, metadata))
			return {};
		switch (infoType)
		{
		case 1:
			return metadata.name;
		case 2:
			return metadata.author;
		case 3:
			return metadata.description;
		case 4:
			return QString();
		case 5:
			return metadata.language;
		case 6:
			return metadata.source;
		case 7:
			return metadata.id;
		case 8:
			return metadata.purpose;
		case 9:
		case 10:
		case 11:
		case 12:
			return 0;
		case 13:
			return metadata.dateWritten;
		case 14:
			return metadata.dateModified;
		case 15:
			return metadata.saveState;
		case 16:
			return false;
		case 17:
			return enabled;
		case 18:
			return metadata.requiredVersion;
		case 19:
			return metadata.version;
		case 20:
			return metadata.directory;
		case 21:
			return visibleIndex;
		case 22:
			return metadata.dateModified;
		case 23:
			return QString();
		case 24:
			return 0.0;
		case 25:
			return metadata.sequence;
		default:
			return {};
		}
	}

	NativeCallResult callRoutine(WorldRuntime *runtime, const QString &pluginId, const QString &routine,
	                             const QVector<QVariant> &arguments, const NativeCallContext &context,
	                             const NativeCallExecutionMode executionMode)
	{
		NativeCallResult result;
		result.errorCode  = eOK;
		const QString &id = pluginId;
		if (!context.installed)
		{
			result.errorCode = eNoSuchPlugin;
			result.errorText = QStringLiteral("Plugin ID (%1) is not installed").arg(pluginId);
			return result;
		}
		if (!context.pluginEnabled)
		{
			QString pluginName = context.pluginName.trimmed();
			if (pluginName.isEmpty())
			{
				NativePluginMetadata metadata;
				if (metadataForShim(id, metadata))
					pluginName = metadata.name;
			}
			if (pluginName.isEmpty())
				pluginName = pluginId;
			result.errorCode = ePluginDisabled;
			result.errorText = QStringLiteral("Plugin '%1' (%2) disabled").arg(pluginName, id);
			return result;
		}
		if (id == luaAudioPluginId())
		{
			return callLuaAudioRoutine(runtime, routine, arguments, executionMode);
		}
		if (id != mushReaderPluginId())
		{
			result.errorCode = eNoSuchPlugin;
			result.errorText = QStringLiteral("Plugin ID (%1) is not installed").arg(pluginId);
			return result;
		}

		const QString name = normalizedRoutine(routine);
		if (name == QStringLiteral("say"))
		{
			if (executionMode == NativeCallExecutionMode::ExecuteSideEffects &&
			    effectiveMushReaderSpeechEnabled(context))
				speak(runtime, QStringLiteral("       ") + firstArgumentString(arguments), false);
			return result;
		}
		if (name == QStringLiteral("interrupt"))
		{
			if (executionMode == NativeCallExecutionMode::ExecuteSideEffects &&
			    effectiveMushReaderSpeechEnabled(context))
				speak(runtime, firstArgumentString(arguments), true);
			return result;
		}
		if (name == QStringLiteral("stop"))
		{
			if (executionMode == NativeCallExecutionMode::ExecuteSideEffects)
				stopSpeech(runtime);
			return result;
		}
		if (name == QStringLiteral("plugin_update_url"))
		{
			result.returnValues.push_back(QStringLiteral("qmud:native/MushReader"));
			return result;
		}
		result.errorCode = eNoSuchRoutine;
		result.errorText = QStringLiteral("No function '%1' in native shim '%2' (%3)")
		                       .arg(routine, QStringLiteral("MushReader"), id);
		return result;
	}

	bool callRoutineRequiresRuntimeThread(const QString &pluginId, const QString &routine)
	{
		const QString &id   = pluginId;
		const QString  name = normalizedRoutine(routine);
		if (id == luaAudioPluginId())
		{
			return name == QStringLiteral("init") || !luaAudioPpiTargetRoutineName(routine).isEmpty();
		}
		if (id != mushReaderPluginId())
			return false;
		return name == QStringLiteral("say") || name == QStringLiteral("interrupt") ||
		       name == QStringLiteral("stop");
	}

	bool handleMushReaderCommand(WorldRuntime *runtime, const QString &command)
	{
		if (!runtime)
			return false;
		const QString trimmed = command.trimmed();
		const QString lower   = trimmed.toLower();
		if (lower == QStringLiteral("tts_stop"))
		{
			stopSpeech(runtime);
			return true;
		}
		if (lower == QStringLiteral("tts_interrupt") || lower.startsWith(QStringLiteral("tts_interrupt ")))
		{
			const std::shared_ptr<MushReaderState> state = stateFor(runtime);
			if (effectiveMushReaderSpeechEnabled(*state))
			{
				QString text = trimmed.mid(QStringLiteral("tts_interrupt").size()).trimmed();
				speak(runtime, text, true);
			}
			return true;
		}
		if (lower == QStringLiteral("tts_note") || lower.startsWith(QStringLiteral("tts_note ")))
		{
			const std::shared_ptr<MushReaderState> state = stateFor(runtime);
			if (effectiveMushReaderSpeechEnabled(*state))
			{
				QString text = trimmed.mid(QStringLiteral("tts_note").size()).trimmed();
				speak(runtime, text, false);
			}
			return true;
		}
		if (lower == QStringLiteral("tts"))
		{
			const std::shared_ptr<MushReaderState> state = stateFor(runtime);
			if (state->mushReaderPluginEnabled)
			{
				state->mushReaderSpeechEnabled = !state->mushReaderSpeechEnabled;
				const bool enabled             = state->mushReaderSpeechEnabled;
				runtime->notifyNativePluginStateChanged();
				stopSpeech(runtime);
				speak(runtime, enabled ? QStringLiteral("speech on") : QStringLiteral("speech off"), true);
				return true;
			}
			state->qtAccessibilitySpeechEnabled = !state->qtAccessibilitySpeechEnabled;
			runtime->notifyNativePluginStateChanged();
			return true;
		}
		if (lower == QStringLiteral("mushreader help") || lower == QStringLiteral("mushreader:help"))
		{
			outputLine(*runtime, QStringLiteral("MushReader native QMud shim."));
			outputLine(*runtime,
			           QStringLiteral("Commands: tts, tts_note <text>, tts_interrupt <text>, tts_stop."));
			outputLine(
			    *runtime,
			    QStringLiteral(
			        "Substitutions: subst on|off|list|clear|add <text>==<replacement>|remove <text>|save."));
			return true;
		}
		if (lower == QStringLiteral("subst") || lower.startsWith(QStringLiteral("subst ")))
		{
			handleSubstitutionCommand(runtime, trimmed.mid(QStringLiteral("subst").size()));
			return true;
		}
		return false;
	}

	bool handleLuaAudioCommand(WorldRuntime *runtime, const QString &command)
	{
		if (!isLuaAudioShadowPluginEnabled(runtime))
			return false;
		const QString trimmed = command.trimmed();
		const QString lower   = trimmed.toLower();
		if (lower == QStringLiteral("luaaudio") || lower == QStringLiteral("luaaudio help"))
		{
			NativePluginMetadata metadata;
			if (metadataForShim(luaAudioPluginId(), metadata))
				outputLine(*runtime, metadata.description);
			else
				outputLine(*runtime, QStringLiteral("LuaAudio native QMud shim."));
			return true;
		}
		if (lower == QStringLiteral("sound_toggle"))
		{
			if (luaAudioSoundsEnabled(runtime))
			{
				const double currentVolume = luaAudioRuntimeMasterState(runtime).volume;
				setLuaAudioMasterVolumeAndCommandVolume(runtime, currentVolume);
				setLuaAudioSoundsEnabled(runtime, false);
				luaAudioSetRuntimeMasterVolume(runtime, 0.0);
				outputLine(*runtime, QStringLiteral("Sounds off."));
			}
			else
			{
				setLuaAudioSoundsEnabled(runtime, true);
				luaAudioSetRuntimeMasterVolume(runtime, luaAudioCommandVolume(runtime));
				outputLine(*runtime, QStringLiteral("Sounds on."));
			}
			return true;
		}
		if (lower == QStringLiteral("volume_down"))
		{
			handleLuaAudioVolumeCommand(runtime, -5);
			return true;
		}
		if (lower == QStringLiteral("volume_up"))
		{
			handleLuaAudioVolumeCommand(runtime, 5);
			return true;
		}
		if (lower == QStringLiteral("vol"))
		{
			outputLine(*runtime, QString::number(luaAudioRuntimeMasterState(runtime).volume));
			outputLine(*runtime, QString::number(luaAudioCommandVolume(runtime)));
			outputLine(*runtime, luaAudioSoundsEnabled(runtime) ? QStringLiteral("1") : QStringLiteral("0"));
			return true;
		}
		if (lower == QStringLiteral("sound_close"))
		{
			static_cast<void>(callLuaAudio(runtime, QStringLiteral("free"), {}));
			return true;
		}
		return false;
	}

	bool handleLuaAudioPlaySound(const WorldRuntime *runtime, const QString &sound)
	{
		if (!isLuaAudioShadowPluginEnabled(runtime))
			return false;
		const QString trimmed = sound.trimmed();
		if (trimmed.isEmpty())
		{
			stopLuaAudioNativeBuffers(runtime);
			clearLuaAudioLoopBuffers(runtime);
			return true;
		}

		const qsizetype separator = trimmed.indexOf(QLatin1Char('='));
		if (separator > 0)
		{
			const QString key   = trimmed.left(separator).trimmed().toLower();
			const QString value = trimmed.mid(separator + 1).trimmed();
			bool          ok    = false;
			if (key == QStringLiteral("pan") || key == QStringLiteral("volume") ||
			    key == QStringLiteral("freq"))
			{
				const double numericValue = value.toDouble(&ok);
				if (ok && qIsFinite(numericValue))
				{
					if (key == QStringLiteral("pan"))
						luaAudioSetRuntimeMasterPan(runtime, numericValue);
					else if (key == QStringLiteral("volume"))
						setLuaAudioMasterVolumeAndCommandVolume(runtime, numericValue);
					else
						luaAudioSetRuntimeMasterPitch(runtime, numericValue);
				}
				return true;
			}
			if (key == QStringLiteral("loop"))
			{
				const LuaAudioRuntimeMasterState master = luaAudioRuntimeMasterState(runtime);
				const int buffer = playLuaAudioNativeBuffer(runtime, value, true, master.volume, master.pan,
				                                            0, nullptr, master.pitch);
				if (buffer != 0)
					markLuaAudioLoopBuffer(runtime, value, buffer);
				return true;
			}
			if (key == QStringLiteral("stop"))
			{
				if (value.compare(QStringLiteral("all"), Qt::CaseInsensitive) == 0)
				{
					const QHash<QString, int> loops = luaAudioLoopBuffers(runtime);
					for (const int buffer : loops)
						stopLuaAudioNativeBuffer(runtime, buffer);
					clearLuaAudioLoopBuffers(runtime);
				}
				else
				{
					const int buffer = takeLuaAudioLoopBuffer(runtime, value);
					if (buffer != 0)
						stopLuaAudioNativeBuffer(runtime, buffer);
				}
				return true;
			}
			return true;
		}

		const LuaAudioRuntimeMasterState master = luaAudioRuntimeMasterState(runtime);
		static_cast<void>(playLuaAudioNativeBuffer(runtime, trimmed, false, master.volume, master.pan, 0,
		                                           nullptr, master.pitch));
		return true;
	}

	void handleMushReaderScreenDraw(const WorldRuntime *runtime, const int type, const int /*log*/,
	                                const QString &text)
	{
		if (!runtime || (type != 0 && type != 1))
			return;
		const std::shared_ptr<MushReaderState> state = stateFor(runtime);
		if (!effectiveMushReaderSpeechEnabled(*state))
			return;
		if (text.trimmed().isEmpty())
			return;
		bool    skip = false;
		QString line = substitutionAppliedText(runtime, text, skip);
		if (!skip && !line.isEmpty())
			speak(runtime, line, false);
	}

	bool speakMushReaderReviewText(const WorldRuntime *runtime, const QString &text)
	{
		if (!runtime || text.trimmed().isEmpty())
			return false;
		const std::shared_ptr<MushReaderState> state = stateFor(runtime);
		if (!effectiveMushReaderSpeechEnabled(*state))
			return false;
		bool    skip = false;
		QString line = substitutionAppliedText(runtime, text, skip);
		if (skip || line.isEmpty())
			return true;
		static_cast<void>(speak(runtime, line, true));
		return true;
	}

	void handleMushReaderTabComplete(const WorldRuntime *runtime, const QString &text)
	{
		if (!runtime || text.trimmed().isEmpty())
			return;
		const std::shared_ptr<MushReaderState> state = stateFor(runtime);
		if (effectiveMushReaderSpeechEnabled(*state))
			speak(runtime, text, false);
	}

	void setMushReaderPluginEnabled(const WorldRuntime *runtime, const bool enable)
	{
		if (!runtime)
			return;
		const std::shared_ptr<MushReaderState> state = stateFor(runtime);
		state->mushReaderPluginEnabled               = enable;
		state->mushReaderSpeechEnabled               = enable;
		if (!enable)
			stopSpeech(runtime);
	}

	bool isMushReaderPluginEnabled(const WorldRuntime *runtime)
	{
		if (!runtime)
			return false;
		QMutexLocker locker(&stateMutex());
		const auto   it = states().find(runtime);
		return it != states().end() && it->second && it->second->mushReaderPluginEnabled;
	}

	bool isMushReaderSpeechEnabled(const WorldRuntime *runtime)
	{
		if (!runtime)
			return false;
		QMutexLocker locker(&stateMutex());
		const auto   it = states().find(runtime);
		return it != states().end() && it->second && it->second->mushReaderSpeechEnabled;
	}

	void setQtAccessibilitySpeechEnabled(const WorldRuntime *runtime, const bool enable)
	{
		if (!runtime)
			return;
		const std::shared_ptr<MushReaderState> state = stateFor(runtime);
		state->qtAccessibilitySpeechEnabled          = enable;
	}

	bool isQtAccessibilitySpeechEnabled(const WorldRuntime *runtime)
	{
		if (!runtime)
			return true;
		QMutexLocker locker(&stateMutex());
		const auto   it = states().find(runtime);
		return it == states().end() || !it->second || it->second->qtAccessibilitySpeechEnabled;
	}

	void ensureMushReaderRuntimeSetup(WorldRuntime *runtime)
	{
		if (!runtime)
			return;
		const std::shared_ptr<MushReaderState> state = stateFor(runtime);
		installMushReaderAccelerator(runtime, *state);
	}

	void discardRuntimeState(const WorldRuntime *runtime)
	{
		std::shared_ptr<MushReaderState> removedMushReaderState;
		{
			QMutexLocker locker(&stateMutex());
			if (const auto it = states().find(runtime); it != states().end())
			{
				removedMushReaderState = std::move(it->second);
				states().erase(it);
			}
		}
		removedMushReaderState.reset();
		{
			QMutexLocker locker(&luaAudioStateMutex());
			if (const auto it = luaAudioNativeStates().find(runtime); it != luaAudioNativeStates().end())
			{
				cancelAllLuaAudioOperationGenerations(it->second);
				luaAudioNativeStates().erase(it);
			}
		}
	}

	std::size_t mushReaderTestBackendCount(const WorldRuntime *runtime)
	{
		const std::shared_ptr<const MushReaderState> state = existingStateFor(runtime);
		return state ? state->backends.size() : 0U;
	}

	void setTestSpeechSink(std::function<void(const TestSpeechEvent &)> sink)
	{
		QMutexLocker locker(&stateMutex());
		testSpeechSink() = std::move(sink);
	}

} // namespace QMudNativePluginRegistry
