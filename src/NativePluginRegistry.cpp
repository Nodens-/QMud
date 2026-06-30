/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: NativePluginRegistry.cpp
 * Role: Native compatibility plugin registry and MushReader shim implementation.
 */

#include "NativePluginRegistry.h"

#include "AcceleratorUtils.h"
#include "TtsEngine.h"
#include "WorldOptions.h"
#include "WorldRuntime.h"
#include "helpers/PluginPathUtils.h"
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
#include <memory>
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

	QString        normalizedId(const QString &value)
	{
		return value.trimmed().toLower();
	}

	QString normalizedRoutine(const QString &value)
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
		    QStringLiteral("8238deec7c06bade8ebc3819")  // AutoSave.xml
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
		metadata.language        = QStringLiteral("native");
		metadata.source          = qmudNativeSourceName(metadata.name);
		metadata.directory       = QStringLiteral("qmud:native/");
		metadata.version         = 1.10;
		metadata.requiredVersion = 0.0;
		metadata.sequence        = 5000;
		metadata.dateWritten     = QDateTime::fromString(QStringLiteral("2026-06-06T00:00:00Z"), Qt::ISODate);
		metadata.dateModified    = metadata.dateWritten;
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
		metadata.language        = QStringLiteral("native");
		metadata.source          = qmudNativeSourceName(metadata.name);
		metadata.directory       = QStringLiteral("qmud:native/");
		metadata.version         = 1.0;
		metadata.requiredVersion = 0.0;
		metadata.sequence        = 5000;
		metadata.dateWritten     = QDateTime::fromString(QStringLiteral("2026-06-07T00:00:00Z"), Qt::ISODate);
		metadata.dateModified    = metadata.dateWritten;
		return metadata;
	}

	QStringList mushReaderRoutines()
	{
		return {QStringLiteral("say"), QStringLiteral("interrupt"), QStringLiteral("stop"),
		        QStringLiteral("plugin_update_url")};
	}

	QStringList luaAudioRoutines()
	{
		return {
		    QStringLiteral("setPitch"),  QStringLiteral("slidePitch"), QStringLiteral("isPlaying"),
		    QStringLiteral("getVolume"), QStringLiteral("stop"),       QStringLiteral("setPan"),
		    QStringLiteral("play"),      QStringLiteral("playDelay"),  QStringLiteral("playDelayLooped"),
		    QStringLiteral("setVol"),    QStringLiteral("slideVol"),   QStringLiteral("fadeout"),
		    QStringLiteral("slidePan"),  QStringLiteral("playLooped"), QStringLiteral("plugin_update_url")};
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
			bool                                        passiveSpeechEnabled{false};
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
		QMutexLocker locker(&stateMutex());
		auto        &sink = testSpeechSink();
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
		return state.mushReaderPluginEnabled ? state.mushReaderSpeechEnabled : state.passiveSpeechEnabled;
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

	double argumentNumber(const QVector<QVariant> &arguments, const qsizetype index, const double fallback)
	{
		if (index >= arguments.size() || !arguments.at(index).isValid())
			return fallback;
		bool         ok    = false;
		const double value = arguments.at(index).toDouble(&ok);
		return ok ? value : fallback;
	}

	bool secondArgumentBoolOrFalse(const QVector<QVariant> &arguments)
	{
		constexpr qsizetype kIndex = 1;
		if (kIndex >= arguments.size() || !arguments.at(kIndex).isValid())
			return false;
		const QVariant &value = arguments.at(kIndex);
		if (value.typeId() == QMetaType::Bool)
			return value.toBool();
		bool         ok     = false;
		const double number = value.toDouble(&ok);
		if (ok)
			return number != 0.0;
		const QString text = value.toString().trimmed().toLower();
		if (text == QStringLiteral("true") || text == QStringLiteral("yes") || text == QStringLiteral("y"))
			return true;
		if (text == QStringLiteral("false") || text == QStringLiteral("no") || text == QStringLiteral("n"))
			return false;
		return false;
	}

	double luaAudioVolumeToQmudVolume(const double volume)
	{
		return qBound(0.0, volume, 100.0) - 100.0;
	}

	struct LuaAudioNativeState
	{
			QMudNativePluginRegistry::LuaAudioRuntimeMasterState             master;
			QSet<int>                                                        ownedBuffers;
			QHash<int, QMudNativePluginRegistry::LuaAudioRuntimeBufferState> buffers;
			QHash<QString, int>                                              loopBuffersByName;
			QHash<int, QList<std::shared_ptr<std::atomic_bool>>>             pendingOperationCancels;
			quint64                                                          nextBufferGeneration{0};
			double                                                           commandVolume{100.0};
			bool                                                             soundsEnabled{true};
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

	bool luaAudioFileExists(const WorldRuntime *runtime, const QString &fileName)
	{
		if (fileName.isEmpty())
			return false;
		const QString relative = QMudPluginPathUtils::legacyPathRelativeToQmudHome(fileName);
		QStringList   candidates;
		if (!relative.isEmpty() && !relative.startsWith(QStringLiteral("sounds/"), Qt::CaseInsensitive))
			candidates.push_back(QStringLiteral("sounds/%1").arg(relative));
		candidates.push_back(fileName);
		for (const QString &candidate : candidates)
		{
			QString normalizedCandidate;
			QString error;
			if (!QMudPluginPathUtils::resolveInsideQmudHome(runtime->startupDirectory(), candidate,
			                                                &normalizedCandidate, &error))
				continue;
			if (QFileInfo::exists(normalizedCandidate))
				return true;
		}
		return false;
	}

	void cancelLuaAudioPendingWork(const QMudNativePluginRegistry::LuaAudioRuntimeBufferState &state)
	{
		if (state.pendingCancel)
			state.pendingCancel->store(true, std::memory_order_release);
	}

	void cancelLuaAudioPendingOperations(LuaAudioNativeState &state, const int buffer)
	{
		const QList<std::shared_ptr<std::atomic_bool>> tokens = state.pendingOperationCancels.take(buffer);
		for (const std::shared_ptr<std::atomic_bool> &token : tokens)
		{
			if (token)
				token->store(true, std::memory_order_release);
		}
	}

	QMudNativePluginRegistry::LuaAudioRuntimeBufferState
	luaAudioBufferStateFromMaster(const WorldRuntime *runtime, const bool loop, const double volume,
	                              const double pan, std::shared_ptr<std::atomic_bool> pendingCancel = {})
	{
		QMudNativePluginRegistry::LuaAudioRuntimeBufferState state;
		state.loop          = loop;
		state.volume        = qBound(0.0, volume, 100.0);
		state.pan           = pan;
		state.pitch         = QMudNativePluginRegistry::luaAudioRuntimeMasterState(runtime).pitch;
		state.pendingCancel = std::move(pendingCancel);
		return state;
	}

	void stopLuaAudioNativeBuffer(const WorldRuntime *runtime, const int buffer)
	{
		QMudNativePluginRegistry::LuaAudioRuntimeBufferState state;
		if (!runtime || buffer < 1 || buffer > WorldRuntime::kMaxSoundBuffers ||
		    !QMudNativePluginRegistry::luaAudioRuntimeBufferState(runtime, buffer, state))
		{
			return;
		}
		auto *mutableRuntime = const_cast<WorldRuntime *>(runtime);
		static_cast<void>(mutableRuntime->stopSoundBypassingPluginCallbacks(buffer));
		QMudNativePluginRegistry::luaAudioReleaseRuntimeBuffer(runtime, buffer);
	}

	void stopLuaAudioNativeBuffers(const WorldRuntime *runtime)
	{
		const QList<int> buffers = QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(runtime);
		for (const int buffer : buffers)
			stopLuaAudioNativeBuffer(runtime, buffer);
	}

	QHash<QString, int> luaAudioLoopBuffers(const WorldRuntime *runtime)
	{
		QMutexLocker locker(&stateMutex());
		const auto   it = luaAudioNativeStates().find(runtime);
		return it == luaAudioNativeStates().end() ? QHash<QString, int>() : it->second.loopBuffersByName;
	}

	void markLuaAudioLoopBuffer(const WorldRuntime *runtime, const QString &fileName, const int buffer)
	{
		if (fileName.isEmpty() || buffer < 1)
			return;
		QMutexLocker locker(&stateMutex());
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
		QMutexLocker locker(&stateMutex());
		const auto   it = luaAudioNativeStates().find(runtime);
		if (it == luaAudioNativeStates().end())
			return 0;
		return it->second.loopBuffersByName.take(fileName);
	}

	void clearLuaAudioLoopBuffers(const WorldRuntime *runtime)
	{
		QMutexLocker locker(&stateMutex());
		const auto   it = luaAudioNativeStates().find(runtime);
		if (it != luaAudioNativeStates().end())
			it->second.loopBuffersByName.clear();
	}

	void scheduleLuaAudioNativeStop(const WorldRuntime *runtime, const int buffer, const int delayMs)
	{
		if (!runtime)
			return;
		if (buffer == 0)
		{
			for (const int ownedBuffer : QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(runtime))
				scheduleLuaAudioNativeStop(runtime, ownedBuffer, delayMs);
			return;
		}
		auto    cancelToken = std::make_shared<std::atomic_bool>(false);
		quint64 generation  = 0;
		if (!QMudNativePluginRegistry::luaAudioTrackRuntimeBufferPendingOperation(runtime, buffer,
		                                                                          cancelToken, &generation))
			return;
		QPointer<WorldRuntime> runtimeGuard(const_cast<WorldRuntime *>(runtime));
		QTimer::singleShot(delayMs, const_cast<WorldRuntime *>(runtime),
		                   [runtimeGuard, cancelToken, buffer, generation]
		                   {
			                   if (!runtimeGuard)
				                   return;
			                   QMudNativePluginRegistry::LuaAudioRuntimeBufferState bufferState;
			                   if (!QMudNativePluginRegistry::luaAudioConsumeRuntimeBufferPendingOperation(
			                           runtimeGuard, buffer, generation, cancelToken, bufferState))
				                   return;
			                   stopLuaAudioNativeBuffer(runtimeGuard, buffer);
		                   });
	}

	void scheduleLuaAudioNativeSetVolume(const WorldRuntime *runtime, const int buffer, const double volume,
	                                     const int delayMs)
	{
		auto    cancelToken = std::make_shared<std::atomic_bool>(false);
		quint64 generation  = 0;
		if (!QMudNativePluginRegistry::luaAudioTrackRuntimeBufferPendingOperation(runtime, buffer,
		                                                                          cancelToken, &generation))
			return;
		QPointer<WorldRuntime> runtimeGuard(const_cast<WorldRuntime *>(runtime));
		QTimer::singleShot(delayMs, const_cast<WorldRuntime *>(runtime),
		                   [runtimeGuard, cancelToken, buffer, volume, generation]
		                   {
			                   if (!runtimeGuard)
				                   return;
			                   QMudNativePluginRegistry::LuaAudioRuntimeBufferState bufferState;
			                   if (!QMudNativePluginRegistry::luaAudioConsumeRuntimeBufferPendingOperation(
			                           runtimeGuard, buffer, generation, cancelToken, bufferState))
				                   return;
			                   bufferState.volume = qBound(0.0, volume, 100.0);
			                   QMudNativePluginRegistry::luaAudioMarkRuntimeBuffer(runtimeGuard, buffer,
			                                                                       bufferState);
			                   static_cast<void>(runtimeGuard->playSoundBypassingPluginCallbacks(
			                       buffer, QString(), bufferState.loop,
			                       luaAudioVolumeToQmudVolume(bufferState.volume), bufferState.pan));
		                   });
	}

	void scheduleLuaAudioNativeSetPan(const WorldRuntime *runtime, const int buffer, const double pan,
	                                  const int delayMs)
	{
		auto    cancelToken = std::make_shared<std::atomic_bool>(false);
		quint64 generation  = 0;
		if (!QMudNativePluginRegistry::luaAudioTrackRuntimeBufferPendingOperation(runtime, buffer,
		                                                                          cancelToken, &generation))
			return;
		QPointer<WorldRuntime> runtimeGuard(const_cast<WorldRuntime *>(runtime));
		QTimer::singleShot(delayMs, const_cast<WorldRuntime *>(runtime),
		                   [runtimeGuard, cancelToken, buffer, pan, generation]
		                   {
			                   if (!runtimeGuard)
				                   return;
			                   QMudNativePluginRegistry::LuaAudioRuntimeBufferState bufferState;
			                   if (!QMudNativePluginRegistry::luaAudioConsumeRuntimeBufferPendingOperation(
			                           runtimeGuard, buffer, generation, cancelToken, bufferState))
				                   return;
			                   bufferState.pan = pan;
			                   QMudNativePluginRegistry::luaAudioMarkRuntimeBuffer(runtimeGuard, buffer,
			                                                                       bufferState);
		                   });
	}

	void scheduleLuaAudioNativeSetPitch(const WorldRuntime *runtime, const int buffer, const double pitch,
	                                    const int delayMs)
	{
		auto    cancelToken = std::make_shared<std::atomic_bool>(false);
		quint64 generation  = 0;
		if (!QMudNativePluginRegistry::luaAudioTrackRuntimeBufferPendingOperation(runtime, buffer,
		                                                                          cancelToken, &generation))
			return;
		QPointer<WorldRuntime> runtimeGuard(const_cast<WorldRuntime *>(runtime));
		QTimer::singleShot(delayMs, const_cast<WorldRuntime *>(runtime),
		                   [runtimeGuard, cancelToken, buffer, pitch, generation]
		                   {
			                   if (!runtimeGuard)
				                   return;
			                   QMudNativePluginRegistry::LuaAudioRuntimeBufferState bufferState;
			                   if (!QMudNativePluginRegistry::luaAudioConsumeRuntimeBufferPendingOperation(
			                           runtimeGuard, buffer, generation, cancelToken, bufferState))
				                   return;
			                   bufferState.pitch = pitch;
			                   QMudNativePluginRegistry::luaAudioMarkRuntimeBuffer(runtimeGuard, buffer,
			                                                                       bufferState);
		                   });
	}

	int playLuaAudioNativeBuffer(const WorldRuntime *runtime, const QString &fileName, const bool loop,
	                             const double volume, const double pan, const int delayMs)
	{
		if (fileName.isEmpty() || !luaAudioFileExists(runtime, fileName))
			return 0;
		const int buffer = QMudNativePluginRegistry::luaAudioReserveRuntimeBuffer(
		    runtime,
		    [runtime](const int candidate) { return runtime->soundBufferReusableForNativeAudio(candidate); });
		if (buffer < 1)
			return 0;
		if (delayMs > 0)
		{
			auto cancelToken = std::make_shared<std::atomic_bool>(false);
			QMudNativePluginRegistry::luaAudioMarkRuntimeBuffer(
			    runtime, buffer, luaAudioBufferStateFromMaster(runtime, loop, volume, pan, cancelToken));
			QMudNativePluginRegistry::LuaAudioRuntimeBufferState pendingState;
			if (!QMudNativePluginRegistry::luaAudioRuntimeBufferState(runtime, buffer, pendingState))
			{
				QMudNativePluginRegistry::luaAudioReleaseRuntimeBuffer(runtime, buffer);
				return 0;
			}
			const quint64          generation = pendingState.generation;
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
				                   const int status = runtimeGuard->playSoundBypassingPluginCallbacks(
				                       buffer, fileName, bufferState.loop,
				                       luaAudioVolumeToQmudVolume(bufferState.volume), bufferState.pan);
				                   if (status != eOK)
					                   QMudNativePluginRegistry::luaAudioReleaseRuntimeBufferIfGeneration(
					                       runtimeGuard, buffer, generation);
			                   });
			return buffer;
		}
		QMudNativePluginRegistry::luaAudioMarkRuntimeBuffer(
		    runtime, buffer, luaAudioBufferStateFromMaster(runtime, loop, volume, pan));
		auto     *mutableRuntime = const_cast<WorldRuntime *>(runtime);
		const int status         = mutableRuntime->playSoundBypassingPluginCallbacks(
		    buffer, fileName, loop, luaAudioVolumeToQmudVolume(volume), pan);
		if (status != eOK)
		{
			QMudNativePluginRegistry::luaAudioReleaseRuntimeBuffer(runtime, buffer);
			return 0;
		}
		return buffer;
	}

	void setLuaAudioMasterVolumeAndCommandVolume(const WorldRuntime *runtime, const double volume)
	{
		QMudNativePluginRegistry::luaAudioSetRuntimeMasterVolume(runtime, volume);
		QMutexLocker locker(&stateMutex());
		luaAudioNativeStateFor(runtime).commandVolume = qBound(0.0, volume, 100.0);
	}

	bool luaAudioSoundsEnabled(const WorldRuntime *runtime)
	{
		QMutexLocker locker(&stateMutex());
		const auto   it = luaAudioNativeStates().find(runtime);
		return it == luaAudioNativeStates().end() ? true : it->second.soundsEnabled;
	}

	void setLuaAudioSoundsEnabled(const WorldRuntime *runtime, const bool enabled)
	{
		QMutexLocker locker(&stateMutex());
		luaAudioNativeStateFor(runtime).soundsEnabled = enabled;
	}

	double luaAudioCommandVolume(const WorldRuntime *runtime)
	{
		QMutexLocker locker(&stateMutex());
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
			                           return plugin.enabled && !plugin.installPending &&
			                                  plugin.sequence >= 0 &&
			                                  normalizedId(plugin.attributes.value(QStringLiteral("id"))) ==
			                                      audioId;
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
	}

	QMudNativePluginRegistry::NativeCallResult callLuaAudioRoutine(const WorldRuntime      *runtime,
	                                                               const QString           &routine,
	                                                               const QVector<QVariant> &arguments)
	{
		QMudNativePluginRegistry::NativeCallResult result;
		result.errorCode   = eOK;
		const QString name = normalizedRoutine(routine);
		if (name == QStringLiteral("plugin_update_url"))
		{
			result.returnValues.push_back(QStringLiteral("qmud:native/LuaAudio"));
			return result;
		}
		if (name == QStringLiteral("isplaying"))
		{
			const int buffer = static_cast<int>(argumentNumber(arguments, 0, 0.0));
			QMudNativePluginRegistry::LuaAudioRuntimeBufferState bufferState;
			result.returnValues.push_back(
			    runtime && buffer > 0 &&
			    QMudNativePluginRegistry::luaAudioRuntimeBufferState(runtime, buffer, bufferState) &&
			    runtime->soundStatus(buffer) > 0);
			return result;
		}
		if (name == QStringLiteral("getvolume"))
		{
			const int buffer = static_cast<int>(argumentNumber(arguments, 0, 0.0));
			QMudNativePluginRegistry::LuaAudioRuntimeBufferState bufferState;
			if (runtime && buffer > 0 &&
			    QMudNativePluginRegistry::luaAudioRuntimeBufferState(runtime, buffer, bufferState))
				result.returnValues.push_back(bufferState.volume);
			else if (runtime)
				result.returnValues.push_back(
				    QMudNativePluginRegistry::luaAudioRuntimeMasterState(runtime).volume);
			else
				result.returnValues.push_back(100.0);
			return result;
		}
		if (name == QStringLiteral("stop") || name == QStringLiteral("fadeout"))
		{
			if (runtime)
			{
				const int buffer = static_cast<int>(argumentNumber(arguments, 0, 0.0));
				const int delay  = qMax(0, qRound(argumentNumber(arguments, 1, 0.0) * 1000.0));
				if (delay > 0)
					scheduleLuaAudioNativeStop(runtime, buffer, delay);
				else if (buffer == 0)
					stopLuaAudioNativeBuffers(runtime);
				else
					stopLuaAudioNativeBuffer(runtime, buffer);
			}
			return result;
		}
		if (name == QStringLiteral("play") || name == QStringLiteral("playlooped") ||
		    name == QStringLiteral("playdelay") || name == QStringLiteral("playdelaylooped"))
		{
			if (!runtime)
			{
				result.returnValues.push_back(0);
				return result;
			}
			const QString fileName = firstArgumentString(arguments);
			if (fileName.isEmpty())
			{
				result.returnValues.push_back(0);
				return result;
			}
			const QMudNativePluginRegistry::LuaAudioRuntimeMasterState master =
			    QMudNativePluginRegistry::luaAudioRuntimeMasterState(runtime);
			const bool isDelayed =
			    name == QStringLiteral("playdelay") || name == QStringLiteral("playdelaylooped");
			const bool   loop   = name == QStringLiteral("playlooped") ||
			                      name == QStringLiteral("playdelaylooped") ||
			                      (!isDelayed && secondArgumentBoolOrFalse(arguments));
			const int    delay  = isDelayed ? qMax(0, qRound(argumentNumber(arguments, 1, 0.0) * 1000.0)) : 0;
			const double pan    = argumentNumber(arguments, 2, master.pan);
			const double volume = argumentNumber(arguments, 3, master.volume);
			result.returnValues.push_back(
			    playLuaAudioNativeBuffer(runtime, fileName, loop, volume, pan, delay));
			return result;
		}
		if (name == QStringLiteral("setpitch") || name == QStringLiteral("slidepitch"))
		{
			if (runtime)
			{
				const QMudNativePluginRegistry::LuaAudioRuntimeMasterState master =
				    QMudNativePluginRegistry::luaAudioRuntimeMasterState(runtime);
				const double pitch  = argumentNumber(arguments, 0, master.pitch);
				const int    buffer = static_cast<int>(argumentNumber(arguments, 1, 0.0));
				const int    delay  = name == QStringLiteral("slidepitch")
				                          ? qMax(0, qRound(argumentNumber(arguments, 2, 0.0) * 1000.0))
				                          : 0;
				QMudNativePluginRegistry::LuaAudioRuntimeBufferState bufferState;
				if (buffer > 0 &&
				    QMudNativePluginRegistry::luaAudioRuntimeBufferState(runtime, buffer, bufferState))
				{
					if (delay > 0)
						scheduleLuaAudioNativeSetPitch(runtime, buffer, pitch, delay);
					else
					{
						bufferState.pitch = pitch;
						QMudNativePluginRegistry::luaAudioMarkRuntimeBuffer(runtime, buffer, bufferState);
					}
				}
				else if (buffer <= 0)
					QMudNativePluginRegistry::luaAudioSetRuntimeMasterPitch(runtime, pitch);
			}
			return result;
		}
		if (name == QStringLiteral("setpan") || name == QStringLiteral("slidepan"))
		{
			if (runtime)
			{
				const QMudNativePluginRegistry::LuaAudioRuntimeMasterState master =
				    QMudNativePluginRegistry::luaAudioRuntimeMasterState(runtime);
				const double pan    = argumentNumber(arguments, 0, master.pan);
				const int    buffer = static_cast<int>(argumentNumber(arguments, 1, 0.0));
				const int    delay  = name == QStringLiteral("slidepan")
				                          ? qMax(0, qRound(argumentNumber(arguments, 2, 0.0) * 1000.0))
				                          : 0;
				QMudNativePluginRegistry::LuaAudioRuntimeBufferState bufferState;
				if (buffer > 0 &&
				    QMudNativePluginRegistry::luaAudioRuntimeBufferState(runtime, buffer, bufferState))
				{
					if (delay > 0)
						scheduleLuaAudioNativeSetPan(runtime, buffer, pan, delay);
					else
					{
						bufferState.pan = pan;
						QMudNativePluginRegistry::luaAudioMarkRuntimeBuffer(runtime, buffer, bufferState);
					}
				}
				else if (buffer <= 0)
					QMudNativePluginRegistry::luaAudioSetRuntimeMasterPan(runtime, pan);
			}
			return result;
		}
		if (name == QStringLiteral("setvol") || name == QStringLiteral("slidevol"))
		{
			if (runtime)
			{
				const QMudNativePluginRegistry::LuaAudioRuntimeMasterState master =
				    QMudNativePluginRegistry::luaAudioRuntimeMasterState(runtime);
				const double volume = argumentNumber(arguments, 0, master.volume);
				const int    buffer = static_cast<int>(argumentNumber(arguments, 1, 0.0));
				const int    delay  = name == QStringLiteral("slidevol")
				                          ? qMax(0, qRound(argumentNumber(arguments, 2, 0.0) * 1000.0))
				                          : 0;
				QMudNativePluginRegistry::LuaAudioRuntimeBufferState bufferState;
				if (buffer > 0 &&
				    QMudNativePluginRegistry::luaAudioRuntimeBufferState(runtime, buffer, bufferState))
				{
					if (delay > 0)
						scheduleLuaAudioNativeSetVolume(runtime, buffer, volume, delay);
					else
					{
						bufferState.volume = qBound(0.0, volume, 100.0);
						QMudNativePluginRegistry::luaAudioMarkRuntimeBuffer(runtime, buffer, bufferState);
						auto *mutableRuntime = const_cast<WorldRuntime *>(runtime);
						static_cast<void>(mutableRuntime->playSoundBypassingPluginCallbacks(
						    buffer, QString(), bufferState.loop,
						    luaAudioVolumeToQmudVolume(bufferState.volume), bufferState.pan));
					}
				}
				else if (buffer <= 0)
				{
					QMudNativePluginRegistry::luaAudioSetRuntimeMasterVolume(runtime, volume);
				}
			}
			return result;
		}
		result.errorCode = eNoSuchRoutine;
		result.errorText =
		    QStringLiteral("No function '%1' in native shim '%2' (%3)")
		        .arg(routine, QStringLiteral("LuaAudio"), QMudNativePluginRegistry::luaAudioPluginId());
		return result;
	}
} // namespace

namespace QMudNativePluginRegistry
{
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

	bool metadataForShim(const QString &pluginId, NativePluginMetadata &metadata)
	{
		const QString id = normalizedId(pluginId);
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
		const QString id = normalizedId(pluginId);
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
		QMutexLocker locker(&stateMutex());
		const auto   it = luaAudioNativeStates().find(runtime);
		return it == luaAudioNativeStates().end() ? LuaAudioRuntimeMasterState{} : it->second.master;
	}

	void luaAudioSetRuntimeMasterVolume(const WorldRuntime *runtime, const double volume)
	{
		if (!runtime)
			return;
		QMutexLocker locker(&stateMutex());
		luaAudioNativeStateFor(runtime).master.volume = qBound(0.0, volume, 100.0);
	}

	void luaAudioSetRuntimeMasterPan(const WorldRuntime *runtime, const double pan)
	{
		if (!runtime)
			return;
		QMutexLocker locker(&stateMutex());
		luaAudioNativeStateFor(runtime).master.pan = pan;
	}

	void luaAudioSetRuntimeMasterPitch(const WorldRuntime *runtime, const double pitch)
	{
		if (!runtime)
			return;
		QMutexLocker locker(&stateMutex());
		luaAudioNativeStateFor(runtime).master.pitch = pitch;
	}

	int luaAudioReserveRuntimeBuffer(const WorldRuntime             *runtime,
	                                 const std::function<bool(int)> &bufferReusableResolver)
	{
		if (!runtime || !bufferReusableResolver)
			return 0;
		for (int buffer = 1; buffer <= WorldRuntime::kMaxSoundBuffers; ++buffer)
		{
			bool                                                 owned = false;
			QMudNativePluginRegistry::LuaAudioRuntimeBufferState ownedState;
			{
				QMutexLocker locker(&stateMutex());
				const auto   it = luaAudioNativeStates().find(runtime);
				owned = it != luaAudioNativeStates().end() && it->second.ownedBuffers.contains(buffer);
				if (owned)
					ownedState = it->second.buffers.value(buffer);
			}
			const bool reusable = bufferReusableResolver(buffer);
			if (owned)
			{
				const bool hasPendingDelayedPlay =
				    ownedState.pendingCancel && !ownedState.pendingCancel->load(std::memory_order_acquire);
				if (hasPendingDelayedPlay || !reusable)
					continue;
				luaAudioReleaseRuntimeBuffer(runtime, buffer);
			}
			if (!reusable)
				continue;
			QMutexLocker         locker(&stateMutex());
			LuaAudioNativeState &state = luaAudioNativeStateFor(runtime);
			if (state.ownedBuffers.contains(buffer))
				continue;
			state.ownedBuffers.insert(buffer);
			LuaAudioRuntimeBufferState bufferState;
			bufferState.generation = ++state.nextBufferGeneration;
			state.buffers.insert(buffer, bufferState);
			return buffer;
		}
		return 0;
	}

	void luaAudioMarkRuntimeBuffer(const WorldRuntime *runtime, const int buffer,
	                               const LuaAudioRuntimeBufferState &bufferState)
	{
		if (!runtime || buffer < 1 || buffer > WorldRuntime::kMaxSoundBuffers)
			return;
		QMutexLocker               locker(&stateMutex());
		LuaAudioNativeState       &state    = luaAudioNativeStateFor(runtime);
		LuaAudioRuntimeBufferState newState = bufferState;
		if (state.buffers.contains(buffer) && state.buffers[buffer].pendingCancel &&
		    state.buffers[buffer].pendingCancel != newState.pendingCancel)
		{
			cancelLuaAudioPendingWork(state.buffers[buffer]);
		}
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
		state.ownedBuffers.insert(buffer);
		state.buffers[buffer] = newState;
	}

	bool luaAudioTrackRuntimeBufferPendingOperation(const WorldRuntime *runtime, const int buffer,
	                                                const std::shared_ptr<std::atomic_bool> &cancelToken,
	                                                quint64                                 *generation)
	{
		if (!runtime || buffer < 1 || buffer > WorldRuntime::kMaxSoundBuffers || !cancelToken)
			return false;
		QMutexLocker locker(&stateMutex());
		const auto   it = luaAudioNativeStates().find(runtime);
		if (it == luaAudioNativeStates().end() || !it->second.ownedBuffers.contains(buffer) ||
		    !it->second.buffers.contains(buffer))
			return false;
		it->second.pendingOperationCancels[buffer].push_back(cancelToken);
		if (generation)
			*generation = it->second.buffers.value(buffer).generation;
		return true;
	}

	void luaAudioForgetRuntimeBufferPendingOperation(const WorldRuntime *runtime, const int buffer,
	                                                 const std::shared_ptr<std::atomic_bool> &cancelToken)
	{
		if (!runtime || buffer < 1 || buffer > WorldRuntime::kMaxSoundBuffers || !cancelToken)
			return;
		QMutexLocker locker(&stateMutex());
		const auto   it = luaAudioNativeStates().find(runtime);
		if (it == luaAudioNativeStates().end())
			return;
		QList<std::shared_ptr<std::atomic_bool>> &tokens = it->second.pendingOperationCancels[buffer];
		tokens.removeAll(cancelToken);
		if (tokens.isEmpty())
			it->second.pendingOperationCancels.remove(buffer);
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
		QMutexLocker locker(&stateMutex());
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
		it->second.buffers[buffer] = current;
		bufferState                = current;
		return true;
	}

	bool luaAudioConsumeRuntimeBufferPendingOperation(const WorldRuntime *runtime, const int buffer,
	                                                  const quint64                            generation,
	                                                  const std::shared_ptr<std::atomic_bool> &cancelToken,
	                                                  LuaAudioRuntimeBufferState              &bufferState)
	{
		if (!runtime || buffer < 1 || buffer > WorldRuntime::kMaxSoundBuffers || generation == 0 ||
		    !cancelToken || cancelToken->load(std::memory_order_acquire))
		{
			return false;
		}
		QMutexLocker locker(&stateMutex());
		const auto   it = luaAudioNativeStates().find(runtime);
		if (it == luaAudioNativeStates().end() || !it->second.ownedBuffers.contains(buffer) ||
		    !it->second.buffers.contains(buffer))
		{
			return false;
		}
		LuaAudioRuntimeBufferState current = it->second.buffers.value(buffer);
		if (current.generation != generation)
			return false;
		QList<std::shared_ptr<std::atomic_bool>> &tokens = it->second.pendingOperationCancels[buffer];
		if (!tokens.removeOne(cancelToken))
			return false;
		if (tokens.isEmpty())
			it->second.pendingOperationCancels.remove(buffer);
		bufferState = current;
		return true;
	}

	bool luaAudioRuntimeBufferState(const WorldRuntime *runtime, const int buffer,
	                                LuaAudioRuntimeBufferState &bufferState)
	{
		if (!runtime || buffer < 1 || buffer > WorldRuntime::kMaxSoundBuffers)
			return false;
		QMutexLocker locker(&stateMutex());
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
		QMutexLocker locker(&stateMutex());
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
		QMutexLocker locker(&stateMutex());
		const auto   it = luaAudioNativeStates().find(runtime);
		if (it == luaAudioNativeStates().end())
			return;
		if (it->second.buffers.contains(buffer))
			cancelLuaAudioPendingWork(it->second.buffers.value(buffer));
		cancelLuaAudioPendingOperations(it->second, buffer);
		it->second.ownedBuffers.remove(buffer);
		it->second.buffers.remove(buffer);
		for (auto loopIt = it->second.loopBuffersByName.begin();
		     loopIt != it->second.loopBuffersByName.end();)
		{
			if (loopIt.value() == buffer)
				loopIt = it->second.loopBuffersByName.erase(loopIt);
			else
				++loopIt;
		}
	}

	bool luaAudioReleaseRuntimeBufferIfGeneration(const WorldRuntime *runtime, const int buffer,
	                                              const quint64 generation)
	{
		if (!runtime || buffer < 1 || buffer > WorldRuntime::kMaxSoundBuffers || generation == 0)
			return false;
		QMutexLocker locker(&stateMutex());
		const auto   it = luaAudioNativeStates().find(runtime);
		if (it == luaAudioNativeStates().end() || !it->second.buffers.contains(buffer) ||
		    it->second.buffers.value(buffer).generation != generation)
		{
			return false;
		}
		if (it->second.buffers.contains(buffer))
			cancelLuaAudioPendingWork(it->second.buffers.value(buffer));
		cancelLuaAudioPendingOperations(it->second, buffer);
		it->second.ownedBuffers.remove(buffer);
		it->second.buffers.remove(buffer);
		for (auto loopIt = it->second.loopBuffersByName.begin();
		     loopIt != it->second.loopBuffersByName.end();)
		{
			if (loopIt.value() == buffer)
				loopIt = it->second.loopBuffersByName.erase(loopIt);
			else
				++loopIt;
		}
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
		const QList<int> buffers        = luaAudioRuntimeOwnedBuffers(runtime);
		auto            *mutableRuntime = const_cast<WorldRuntime *>(runtime);
		for (const int buffer : buffers)
			static_cast<void>(mutableRuntime->stopSoundBypassingPluginCallbacks(buffer));
		luaAudioReleaseRuntimeBuffers(runtime, buffers);
	}

	void luaAudioStopRuntimeOwner(const WorldRuntime *runtime, const void *ownerKey)
	{
		if (!runtime || !ownerKey)
			return;
		const QList<int> buffers        = luaAudioRuntimeOwnedBuffers(runtime, ownerKey);
		auto            *mutableRuntime = const_cast<WorldRuntime *>(runtime);
		for (const int buffer : buffers)
			static_cast<void>(mutableRuntime->stopSoundBypassingPluginCallbacks(buffer));
		luaAudioReleaseRuntimeBuffers(runtime, buffers);
	}

	void luaAudioResetRuntime(const WorldRuntime *runtime)
	{
		if (!runtime)
			return;
		luaAudioStopRuntime(runtime);
		QMutexLocker locker(&stateMutex());
		luaAudioNativeStates().erase(runtime);
	}

	bool isShimId(const QString &pluginId)
	{
		return shimIds().contains(normalizedId(pluginId));
	}

	bool isBlacklistedId(const QString &pluginId)
	{
		return blacklistedIds().contains(normalizedId(pluginId));
	}

	bool isProtectedId(const QString &pluginId)
	{
		return isShimId(pluginId) || isBlacklistedId(pluginId);
	}

	QString resolveShimIdOrName(const QString &pluginIdOrName)
	{
		QString key = normalizedId(pluginIdOrName);
		if (key.isEmpty())
			return {};
		if (isShimId(key))
			return key;
		for (const QString &shimId : shimIds())
		{
			NativePluginMetadata metadata;
			if (metadataForShim(shimId, metadata) &&
			    metadata.name.compare(pluginIdOrName.trimmed(), Qt::CaseInsensitive) == 0)
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

	NativeCallResult callRoutine(const WorldRuntime *runtime, const QString &pluginId, const QString &routine,
	                             const QVector<QVariant> &arguments)
	{
		NativeCallResult result;
		result.errorCode = eOK;
		const QString id = normalizedId(pluginId);
		if (id == luaAudioPluginId())
		{
			return callLuaAudioRoutine(runtime, routine, arguments);
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
			speak(runtime, QStringLiteral("       ") + firstArgumentString(arguments), false);
			return result;
		}
		if (name == QStringLiteral("interrupt"))
		{
			speak(runtime, firstArgumentString(arguments), true);
			return result;
		}
		if (name == QStringLiteral("stop"))
		{
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
			QString text = trimmed.mid(QStringLiteral("tts_interrupt").size()).trimmed();
			speak(runtime, text, true);
			return true;
		}
		if (lower == QStringLiteral("tts_note") || lower.startsWith(QStringLiteral("tts_note ")))
		{
			QString text = trimmed.mid(QStringLiteral("tts_note").size()).trimmed();
			speak(runtime, text, false);
			return true;
		}
		if (lower == QStringLiteral("tts"))
		{
			const std::shared_ptr<MushReaderState> state = stateFor(runtime);
			bool                                   enabled;
			if (state->mushReaderPluginEnabled)
			{
				state->passiveSpeechEnabled    = false;
				state->mushReaderSpeechEnabled = !state->mushReaderSpeechEnabled;
				enabled                        = state->mushReaderSpeechEnabled;
			}
			else
			{
				state->passiveSpeechEnabled = !state->passiveSpeechEnabled;
				enabled                     = state->passiveSpeechEnabled;
			}
			runtime->notifyNativePluginStateChanged();
			stopSpeech(runtime);
			speak(runtime, enabled ? QStringLiteral("speech on") : QStringLiteral("speech off"), true);
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
			if (key == QStringLiteral("pan"))
			{
				const double pan = value.toDouble(&ok);
				if (ok)
					luaAudioSetRuntimeMasterPan(runtime, pan);
				return true;
			}
			if (key == QStringLiteral("volume"))
			{
				const double volume = value.toDouble(&ok);
				if (ok)
					setLuaAudioMasterVolumeAndCommandVolume(runtime, volume);
				return true;
			}
			if (key == QStringLiteral("freq"))
			{
				const double pitch = value.toDouble(&ok);
				if (ok)
					luaAudioSetRuntimeMasterPitch(runtime, pitch);
				return true;
			}
			if (key == QStringLiteral("loop"))
			{
				const LuaAudioRuntimeMasterState master = luaAudioRuntimeMasterState(runtime);
				const int                        buffer =
				    playLuaAudioNativeBuffer(runtime, value, true, master.volume, master.pan, 0);
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
		}

		const LuaAudioRuntimeMasterState master = luaAudioRuntimeMasterState(runtime);
		return playLuaAudioNativeBuffer(runtime, trimmed, false, master.volume, master.pan, 0) != 0;
	}

	void handleMushReaderScreenDraw(const WorldRuntime *runtime, const int type, const int /*log*/,
	                                const QString &text)
	{
		if (!runtime || (type != 0 && type != 1) || text.trimmed().isEmpty())
			return;
		const std::shared_ptr<MushReaderState> state = stateFor(runtime);
		if (!effectiveMushReaderSpeechEnabled(*state))
			return;
		bool    skip = false;
		QString line = substitutionAppliedText(runtime, text, skip);
		if (!skip && !line.isEmpty())
			speak(runtime, line, false);
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
		const bool                             stopPassiveSpeech =
		    enable && !state->mushReaderPluginEnabled && state->passiveSpeechEnabled;
		state->mushReaderPluginEnabled = enable;
		state->mushReaderSpeechEnabled = enable;
		if (enable)
			state->passiveSpeechEnabled = false;
		if (stopPassiveSpeech)
			stopSpeech(runtime);
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

	void setMushReaderPassiveSpeechEnabled(const WorldRuntime *runtime, const bool enable)
	{
		if (!runtime)
			return;
		const std::shared_ptr<MushReaderState> state = stateFor(runtime);
		state->passiveSpeechEnabled                  = enable && !state->mushReaderPluginEnabled;
		if (!enable)
			stopSpeech(runtime);
	}

	bool isMushReaderPassiveSpeechEnabled(const WorldRuntime *runtime)
	{
		if (!runtime)
			return false;
		QMutexLocker locker(&stateMutex());
		const auto   it = states().find(runtime);
		return it != states().end() && it->second && it->second->passiveSpeechEnabled;
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
			luaAudioNativeStates().erase(runtime);
		}
		removedMushReaderState.reset();
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
