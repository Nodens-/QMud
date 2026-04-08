/**
 * @file LuaLuacomShim.cpp
 * @brief Lua `luacom` compatibility layer for Mushclient SAPI-style plugins.
 */

#include "LuaLuacomShim.h"
#include "TtsEngine.h"

#include <QFile>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDebug>
// ReSharper disable once CppUnusedIncludeDirective
#include <QHash>
// ReSharper disable once CppUnusedIncludeDirective
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
// ReSharper disable once CppUnusedIncludeDirective
#include <QString>
// ReSharper disable once CppUnusedIncludeDirective
#include <QStringList>
// ReSharper disable once CppUnusedIncludeDirective
#include <QSet>
// ReSharper disable once CppUnusedIncludeDirective
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

#ifndef QMUD_ENABLE_TEXT_TO_SPEECH
#define QMUD_ENABLE_TEXT_TO_SPEECH 0
#endif

#if QMUD_ENABLE_TEXT_TO_SPEECH
#include <QVoice>
#endif

namespace
{
	constexpr char kSpVoiceMeta[]                = "qmud.luacom.spvoice";
	constexpr char kVoicesCollectionMeta[]       = "qmud.luacom.voices_collection";
	constexpr char kVoiceMeta[]                  = "qmud.luacom.voice";
	constexpr char kAudioOutputsCollectionMeta[] = "qmud.luacom.audio_outputs_collection";
	constexpr char kAudioOutputMeta[]            = "qmud.luacom.audio_output";
	constexpr char kVoiceStatusMeta[]            = "qmud.luacom.voice_status";
	constexpr char kTokenMeta[]                  = "qmud.luacom.token";
	constexpr char kTokensCollectionMeta[]       = "qmud.luacom.tokens_collection";
	constexpr char kTokenCategoryMeta[]          = "qmud.luacom.token_category";
	constexpr char kLexiconMeta[]                = "qmud.luacom.lexicon";
	constexpr char kFileStreamMeta[]             = "qmud.luacom.file_stream";
	constexpr char kAudioFormatMeta[]            = "qmud.luacom.audio_format";
	constexpr char kEnumeratorMeta[]             = "qmud.luacom.enumerator";
	constexpr char kTypeInfoMeta[]               = "qmud.luacom.typeinfo";
	constexpr char kTypeLibMeta[]                = "qmud.luacom.typelib";

	constexpr int  kSvsfDefault          = 0x0;
	constexpr int  kSvsFlagsAsync        = 0x1;
	constexpr int  kSvsfPurgeBeforeSpeak = 0x2;
	constexpr int  kSvsfIsFilename       = 0x4;
	constexpr int  kSvsfIsXml            = 0x8;
	constexpr int  kSvsfIsNotXml         = 0x10;
	constexpr int  kSvsfPersistXml       = 0x20;
	constexpr int  kSvsfNlpSpeakPunc     = 0x40;
	constexpr int  kSvsfParseSapi        = 0x80;
	constexpr int  kSvsfParseSsml        = 0x100;
	constexpr int  kSvsfParseAutodetect  = 0x0;
	constexpr int  kSvsfNlpMask          = 0x40;
	constexpr int  kSvsfParseMask        = 0x180;
	constexpr int  kSvsfVoiceMask        = 0x1ff;
	constexpr int  kSvsfUnusedFlags      = ~kSvsfVoiceMask;
	constexpr int  kSrseDone             = 1;
	constexpr int  kSrseIsSpeaking       = 2;
	constexpr int  kSvpNormal            = 0;
	constexpr int  kSvpAlert             = 1;
	constexpr int  kSvpOver              = 2;
	constexpr int  kSveStartInputStream  = 0x2;
	constexpr int  kSveEndInputStream    = 0x4;
	constexpr int  kSveVoiceChange       = 0x8;
	constexpr int  kSveBookmark          = 0x10;
	constexpr int  kSveWordBoundary      = 0x20;
	constexpr int  kSvePhoneme           = 0x40;
	constexpr int  kSveSentenceBoundary  = 0x80;
	constexpr int  kSveViseme            = 0x100;
	constexpr int  kSveAudioLevel        = 0x200;
	constexpr int  kSvePrivate           = 0x8000;
	constexpr int kSveAllEvents = kSveStartInputStream | kSveEndInputStream | kSveVoiceChange | kSveBookmark |
	                              kSveWordBoundary | kSvePhoneme | kSveSentenceBoundary | kSveViseme |
	                              kSveAudioLevel | kSvePrivate;
	constexpr int kDefaultVolumePercent  = 100;
	constexpr int kDefaultSyncTimeoutMs  = 10000;
	constexpr int kEnumeratorKindVoices  = 1;
	constexpr int kEnumeratorKindOutputs = 2;
	constexpr int kEnumeratorKindTokens  = 3;
	constexpr int kTokenKindUnknown      = 0;
	constexpr int kTokenKindVoices       = 1;
	constexpr int kTokenKindAudioOutputs = 2;
	constexpr int kSsfmOpenForRead       = 0;
	constexpr int kSsfmOpenReadWrite     = 1;
	constexpr int kSsfmCreate            = 2;
	constexpr int kSsfmCreateForWrite    = 3;
	constexpr int kSltApp                = 1;
	constexpr int kSltUser               = 2;
	constexpr int kSpsUnknown            = 0;
	constexpr int kSpsNoun               = 4096;
	constexpr int kSpsVerb               = 8192;
	constexpr int kSpsModifier           = 12288;
	constexpr int kSpsFunction           = 16384;
	constexpr int kSpsInterjection       = 20480;
	constexpr int kSaftDefault           = -1;
	constexpr int kSaftNoAssignedFormat  = 0;
	constexpr int kSaft8kHz16BitMono     = 6;
	constexpr int kSaft11kHz16BitMono    = 10;
	constexpr int kSaft16kHz16BitMono    = 14;
	constexpr int kSaft22kHz16BitMono    = 18;
	constexpr int kSaft44kHz16BitMono    = 22;

	struct LuacomTokenData
	{
			int                     kind{kTokenKindUnknown};
			QString                 id;
			QString                 description;
			QHash<QString, QString> attributes;
	};

	struct LuacomAudioFormatState
	{
			int     type{0};
			QString guid;
	};

	struct LuacomFileStreamState
	{
			QString                                 path;
			std::shared_ptr<LuacomAudioFormatState> format{std::make_shared<LuacomAudioFormatState>()};
			mutable QMutex                          mutex;
			QFile                                   file;
			bool                                    writeFailed{false};
			QString                                 writeError;
	};

	struct LuacomLexiconState
	{
			QHash<QString, QString> pronunciations;
			QHash<QString, QString> phoneIdPronunciations;
	};

	using TtsVoiceRuntime = QMudTtsEngine::TtsEngine;
	QMutex                         gRuntimeFactoryMutex;
	QMudLuacomShim::RuntimeFactory gRuntimeFactory;

	struct LuacomSpVoice
	{
			std::shared_ptr<TtsVoiceRuntime>       runtime;
			std::shared_ptr<LuacomFileStreamState> outputStream;
	};

	struct LuacomVoicesCollection
	{
			std::shared_ptr<TtsVoiceRuntime> runtime;
			QVector<int>                     indexes;
	};

	struct LuacomVoice
	{
			std::shared_ptr<TtsVoiceRuntime> runtime;
			int                              index{0};
	};

	struct LuacomAudioOutputsCollection
	{
			std::shared_ptr<TtsVoiceRuntime> runtime;
			QVector<int>                     indexes;
	};

	struct LuacomAudioOutput
	{
			std::shared_ptr<TtsVoiceRuntime> runtime;
			int                              index{0};
	};

	struct LuacomVoiceStatus
	{
			std::shared_ptr<TtsVoiceRuntime> runtime;
	};

	struct LuacomToken
	{
			std::shared_ptr<LuacomTokenData> token;
	};

	struct LuacomTokensCollection
	{
			QVector<std::shared_ptr<LuacomTokenData>> tokens;
	};

	struct LuacomTokenCategory
	{
			std::shared_ptr<TtsVoiceRuntime> runtime;
			QString                          categoryId;
			int                              kind{kTokenKindUnknown};
	};

	struct LuacomLexicon
	{
			std::shared_ptr<LuacomLexiconState> state;
	};

	struct LuacomFileStream
	{
			std::shared_ptr<LuacomFileStreamState> state;
	};

	struct LuacomAudioFormat
	{
			std::shared_ptr<LuacomAudioFormatState> state;
	};

	struct LuacomEnumerator
	{
			std::shared_ptr<TtsVoiceRuntime>          runtime;
			QVector<int>                              indexes;
			QVector<std::shared_ptr<LuacomTokenData>> tokens;
			int                                       position{0};
			int                                       kind{kEnumeratorKindVoices};
	};

	struct LuacomTypeInfo
	{
	};

	struct LuacomTypeLib
	{
	};

	template <typename T> T *checkHandle(lua_State *L, const char *metaName, const char *what)
	{
		auto **ud = static_cast<T **>(luaL_checkudata(L, 1, metaName));
		if (!ud || !*ud)
			luaL_argerror(L, 1, what);
		return ud ? *ud : nullptr;
	}

	template <typename T> void pushHandle(lua_State *L, T *handle, const char *metaName)
	{
		auto **ud = static_cast<T **>(lua_newuserdata(L, sizeof(T *)));
		*ud       = handle;
		luaL_getmetatable(L, metaName);
		lua_setmetatable(L, -2);
	}

#if QMUD_ENABLE_TEXT_TO_SPEECH
	QString buildVoiceDescription(const QVoice &voice)
	{
		QString name   = voice.name().trimmed();
		QString locale = voice.locale().name().trimmed();
		if (name.isEmpty() && locale.isEmpty())
			return QStringLiteral("QMud voice");
		if (locale.isEmpty())
			return name;
		if (name.isEmpty())
			return locale;
		return QStringLiteral("%1 (%2)").arg(name, locale);
	}

	QString buildVoiceAttribute(const QVoice &voice, const QString &attributeName)
	{
		const QString normalized = attributeName.trimmed().toLower();
		if (normalized == QStringLiteral("name"))
			return voice.name().trimmed();
		if (normalized == QStringLiteral("vendor"))
			return QStringLiteral("QMud");
		if (normalized == QStringLiteral("language"))
		{
			const QString language = voice.locale().name().trimmed();
			return language.isEmpty() ? QStringLiteral("und") : language;
		}
		if (normalized == QStringLiteral("gender"))
		{
			switch (voice.gender())
			{
			case QVoice::Gender::Male:
				return QStringLiteral("Male");
			case QVoice::Gender::Female:
				return QStringLiteral("Female");
			default:
				return QStringLiteral("Unknown");
			}
		}
		if (normalized == QStringLiteral("age"))
		{
			switch (voice.age())
			{
			case QVoice::Age::Child:
				return QStringLiteral("Child");
			case QVoice::Age::Teenager:
				return QStringLiteral("Teen");
			case QVoice::Age::Adult:
				return QStringLiteral("Adult");
			case QVoice::Age::Senior:
				return QStringLiteral("Senior");
			default:
				return QStringLiteral("Unknown");
			}
		}
		return {};
	}

	using VoiceAttributeConstraint = QPair<QString, QString>;

	QVector<VoiceAttributeConstraint> parseVoiceAttributeConstraints(const char *rawText)
	{
		QVector<VoiceAttributeConstraint> constraints;
		const QString                     text = QString::fromUtf8(rawText ? rawText : "").trimmed();
		if (text.isEmpty())
			return constraints;

		const QStringList parts           = text.split(';', Qt::SkipEmptyParts);
		constexpr auto    kMaxConstraints = 64;
		for (const QString &part : parts)
		{
			const qsizetype eq = part.indexOf('=');
			if (eq <= 0)
				continue;
			const QString key   = part.left(eq).trimmed().toLower();
			const QString value = part.mid(eq + 1).trimmed();
			if (key.isEmpty() || value.isEmpty())
				continue;
			constraints.push_back({key, value});
			if (constraints.size() >= kMaxConstraints)
				break;
		}
		return constraints;
	}

	std::optional<int> primaryLanguageIdFromLocaleName(const QString &localeName)
	{
		const QString                    isoLanguage         = localeName.left(2).toLower();
		static const QHash<QString, int> kPrimaryLanguageIds = {
		    {QStringLiteral("ar"), 0x01},
            {QStringLiteral("bg"), 0x02},
            {QStringLiteral("ca"), 0x03},
		    {QStringLiteral("zh"), 0x04},
            {QStringLiteral("cs"), 0x05},
            {QStringLiteral("da"), 0x06},
		    {QStringLiteral("de"), 0x07},
            {QStringLiteral("el"), 0x08},
            {QStringLiteral("en"), 0x09},
		    {QStringLiteral("es"), 0x0a},
            {QStringLiteral("fi"), 0x0b},
            {QStringLiteral("fr"), 0x0c},
		    {QStringLiteral("he"), 0x0d},
            {QStringLiteral("hu"), 0x0e},
            {QStringLiteral("is"), 0x0f},
		    {QStringLiteral("it"), 0x10},
            {QStringLiteral("ja"), 0x11},
            {QStringLiteral("ko"), 0x12},
		    {QStringLiteral("nl"), 0x13},
            {QStringLiteral("no"), 0x14},
            {QStringLiteral("pl"), 0x15},
		    {QStringLiteral("pt"), 0x16},
            {QStringLiteral("rm"), 0x17},
            {QStringLiteral("ro"), 0x18},
		    {QStringLiteral("ru"), 0x19},
            {QStringLiteral("hr"), 0x1a},
            {QStringLiteral("sk"), 0x1b},
		    {QStringLiteral("sq"), 0x1c},
            {QStringLiteral("sv"), 0x1d},
            {QStringLiteral("th"), 0x1e},
		    {QStringLiteral("tr"), 0x1f},
            {QStringLiteral("ur"), 0x20},
            {QStringLiteral("id"), 0x21},
		    {QStringLiteral("uk"), 0x22},
            {QStringLiteral("be"), 0x23},
            {QStringLiteral("sl"), 0x24},
		    {QStringLiteral("et"), 0x25},
            {QStringLiteral("lv"), 0x26},
            {QStringLiteral("lt"), 0x27},
		    {QStringLiteral("fa"), 0x29},
            {QStringLiteral("vi"), 0x2a},
		};
		const auto it = kPrimaryLanguageIds.constFind(isoLanguage);
		if (it == kPrimaryLanguageIds.cend())
			return std::nullopt;
		return it.value();
	}

	std::optional<int> lcidFromLocaleName(QString localeName)
	{
		localeName                                    = localeName.trimmed().toLower();
		static const QHash<QString, int> kLocaleLcids = {
		    {QStringLiteral("ar_sa"), 0x0401},
            {QStringLiteral("bg_bg"), 0x0402},
		    {QStringLiteral("ca_es"), 0x0403},
            {QStringLiteral("zh_tw"), 0x0404},
		    {QStringLiteral("cs_cz"), 0x0405},
            {QStringLiteral("da_dk"), 0x0406},
		    {QStringLiteral("de_de"), 0x0407},
            {QStringLiteral("el_gr"), 0x0408},
		    {QStringLiteral("en_us"), 0x0409},
            {QStringLiteral("es_es"), 0x0c0a},
		    {QStringLiteral("fi_fi"), 0x040b},
            {QStringLiteral("fr_fr"), 0x040c},
		    {QStringLiteral("he_il"), 0x040d},
            {QStringLiteral("hu_hu"), 0x040e},
		    {QStringLiteral("is_is"), 0x040f},
            {QStringLiteral("it_it"), 0x0410},
		    {QStringLiteral("ja_jp"), 0x0411},
            {QStringLiteral("ko_kr"), 0x0412},
		    {QStringLiteral("nl_nl"), 0x0413},
            {QStringLiteral("nb_no"), 0x0414},
		    {QStringLiteral("nn_no"), 0x0814},
            {QStringLiteral("pl_pl"), 0x0415},
		    {QStringLiteral("pt_br"), 0x0416},
            {QStringLiteral("pt_pt"), 0x0816},
		    {QStringLiteral("rm_ch"), 0x0417},
            {QStringLiteral("ro_ro"), 0x0418},
		    {QStringLiteral("ru_ru"), 0x0419},
            {QStringLiteral("hr_hr"), 0x041a},
		    {QStringLiteral("sk_sk"), 0x041b},
            {QStringLiteral("sq_al"), 0x041c},
		    {QStringLiteral("sv_se"), 0x041d},
            {QStringLiteral("th_th"), 0x041e},
		    {QStringLiteral("tr_tr"), 0x041f},
            {QStringLiteral("ur_pk"), 0x0420},
		    {QStringLiteral("id_id"), 0x0421},
            {QStringLiteral("uk_ua"), 0x0422},
		    {QStringLiteral("be_by"), 0x0423},
            {QStringLiteral("sl_si"), 0x0424},
		    {QStringLiteral("et_ee"), 0x0425},
            {QStringLiteral("lv_lv"), 0x0426},
		    {QStringLiteral("lt_lt"), 0x0427},
            {QStringLiteral("fa_ir"), 0x0429},
		    {QStringLiteral("vi_vn"), 0x042a},
            {QStringLiteral("zh_cn"), 0x0804},
		    {QStringLiteral("en_gb"), 0x0809},
            {QStringLiteral("en_au"), 0x0c09},
		    {QStringLiteral("en_ca"), 0x1009},
            {QStringLiteral("en_nz"), 0x1409},
		    {QStringLiteral("en_ie"), 0x1809},
            {QStringLiteral("en_za"), 0x1c09},
		    {QStringLiteral("en_in"), 0x4009},
		};
		const auto it = kLocaleLcids.constFind(localeName);
		if (it == kLocaleLcids.cend())
			return std::nullopt;
		return it.value();
	}

	QVector<int> parseLanguageCodes(const QString &raw)
	{
		const QString trimmed = raw.trimmed();
		if (trimmed.isEmpty())
			return {};
		QSet<int> values;
		bool      ok = false;
		if (trimmed.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
		{
			const int parsed = trimmed.mid(2).toInt(&ok, 16);
			if (ok)
				values.insert(parsed);
		}
		else
		{
			const int parsedDecimal = trimmed.toInt(&ok, 10);
			if (ok)
				values.insert(parsedDecimal);
			const int parsedHex = trimmed.toInt(&ok, 16);
			if (ok)
				values.insert(parsedHex);
		}
		QVector<int> result;
		result.reserve(values.size());
		for (const int value : values)
			result.push_back(value);
		return result;
	}

	bool matchesLanguageConstraint(QString localeName, const QString &value)
	{
		localeName = localeName.trimmed().toLower();
		if (localeName.isEmpty())
			return false;
		QString localeHyphen = localeName;
		localeHyphen.replace('_', '-');

		const QString normalizedValue = value.trimmed().toLower();
		if (normalizedValue.isEmpty())
			return false;
		if (normalizedValue == localeName || normalizedValue == localeHyphen)
			return true;
		if (normalizedValue == localeName.section('_', 0, 0))
			return true;

		const std::optional<int> exactLcid     = lcidFromLocaleName(localeName);
		const std::optional<int> primaryLcid   = primaryLanguageIdFromLocaleName(localeName);
		const QVector<int>       languageCodes = parseLanguageCodes(normalizedValue);
		return std::ranges::any_of(languageCodes,
		                           [&](const int filterCode)
		                           {
			                           if (filterCode <= 0)
				                           return false;
			                           if (exactLcid && filterCode == *exactLcid)
				                           return true;
			                           const int normalizedCode =
			                               filterCode > 0x3ff ? (filterCode & 0x3ff) : filterCode;
			                           return primaryLcid && normalizedCode == *primaryLcid;
		                           });
	}

	bool voiceMatchesConstraint(const QVoice &voice, const VoiceAttributeConstraint &constraint)
	{
		if (constraint.first == QStringLiteral("language"))
			return matchesLanguageConstraint(voice.locale().name(), constraint.second);

		const QString actual = buildVoiceAttribute(voice, constraint.first);
		if (actual.isEmpty())
			return false;
		return actual.compare(constraint.second, Qt::CaseInsensitive) == 0 ||
		       actual.contains(constraint.second, Qt::CaseInsensitive);
	}

	QVector<int> collectVoiceIndexes(const std::shared_ptr<TtsVoiceRuntime> &runtime, const char *requiredRaw,
	                                 const char *optionalRaw)
	{
		QVector<int> indexes;
		if (!runtime || !runtime->hasSpeech())
			return indexes;

		const QVector<VoiceAttributeConstraint> required = parseVoiceAttributeConstraints(requiredRaw);
		const QVector<VoiceAttributeConstraint> optional = parseVoiceAttributeConstraints(optionalRaw);

		for (int i = 0; i < runtime->voices.size(); ++i)
		{
			const QVoice &voice = runtime->voices.at(i);

			bool          allRequired = true;
			for (const VoiceAttributeConstraint &constraint : required)
			{
				if (voiceMatchesConstraint(voice, constraint))
					continue;
				allRequired = false;
				break;
			}
			if (!allRequired)
				continue;

			if (!optional.isEmpty())
			{
				bool anyOptional = false;
				for (const VoiceAttributeConstraint &constraint : optional)
				{
					if (!voiceMatchesConstraint(voice, constraint))
						continue;
					anyOptional = true;
					break;
				}
				if (!anyOptional)
					continue;
			}

			indexes.push_back(i);
		}
		return indexes;
	}
#else
	QVector<int> collectVoiceIndexes(const std::shared_ptr<TtsVoiceRuntime> &runtime, const char *requiredRaw,
	                                 const char *optionalRaw)
	{
		Q_UNUSED(runtime);
		Q_UNUSED(requiredRaw);
		Q_UNUSED(optionalRaw);
		return {};
	}
#endif

	QString tokenCategoryIdForKind(const int kind)
	{
		switch (kind)
		{
		case kTokenKindVoices:
			return QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech\\Voices");
		case kTokenKindAudioOutputs:
			return QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech\\AudioOutput");
		default:
			return {};
		}
	}

	int tokenKindFromCategoryId(QString categoryId)
	{
		categoryId = categoryId.trimmed().toLower();
		if (categoryId.isEmpty())
			return kTokenKindUnknown;
		if (categoryId.contains(QStringLiteral("voice")))
			return kTokenKindVoices;
		if (categoryId.contains(QStringLiteral("audiooutput")))
			return kTokenKindAudioOutputs;
		return kTokenKindUnknown;
	}

	std::shared_ptr<LuacomTokenData> buildVoiceToken(const std::shared_ptr<TtsVoiceRuntime> &runtime,
	                                                 const int                               index)
	{
#if QMUD_ENABLE_TEXT_TO_SPEECH
		if (!runtime || !runtime->hasSpeech() || index < 0 || index >= runtime->voices.size())
			return {};
		const QVoice &voice = runtime->voices.at(index);
		auto          token = std::make_shared<LuacomTokenData>();
		token->kind         = kTokenKindVoices;
		token->id           = runtime->voiceIds.value(index);
		token->description  = buildVoiceDescription(voice);
		token->attributes.insert(QStringLiteral("name"), buildVoiceAttribute(voice, QStringLiteral("name")));
		token->attributes.insert(QStringLiteral("vendor"),
		                         buildVoiceAttribute(voice, QStringLiteral("vendor")));
		token->attributes.insert(QStringLiteral("language"),
		                         buildVoiceAttribute(voice, QStringLiteral("language")));
		token->attributes.insert(QStringLiteral("gender"),
		                         buildVoiceAttribute(voice, QStringLiteral("gender")));
		token->attributes.insert(QStringLiteral("age"), buildVoiceAttribute(voice, QStringLiteral("age")));
		token->attributes.insert(QStringLiteral("id"), token->id);
		return token;
#else
		Q_UNUSED(runtime);
		Q_UNUSED(index);
		return {};
#endif
	}

	std::shared_ptr<LuacomTokenData> buildAudioOutputToken(const std::shared_ptr<TtsVoiceRuntime> &runtime,
	                                                       const int                               index)
	{
		if (!runtime || index < 0 || index >= runtime->audioOutputIds.size())
			return {};
		auto token  = std::make_shared<LuacomTokenData>();
		token->kind = kTokenKindAudioOutputs;
		token->id   = runtime->audioOutputIds.value(index, QStringLiteral("QMUD\\TTS\\Audio\\UNKNOWN"));
		token->description =
		    runtime->audioOutputDescriptions.value(index, QStringLiteral("Default Audio Output"));
		token->attributes.insert(QStringLiteral("name"), token->description);
		token->attributes.insert(QStringLiteral("vendor"), QStringLiteral("QMud"));
		token->attributes.insert(QStringLiteral("id"), token->id);
		return token;
	}

	void ensureAudioOutputsEnumerated(const std::shared_ptr<TtsVoiceRuntime> &runtime);

	QVector<std::shared_ptr<LuacomTokenData>> collectTokenSet(const std::shared_ptr<TtsVoiceRuntime> &runtime,
	                                                          const int                               kind)
	{
		QVector<std::shared_ptr<LuacomTokenData>> tokens;
		if (!runtime)
			return tokens;
		if (kind == kTokenKindVoices)
		{
#if QMUD_ENABLE_TEXT_TO_SPEECH
			tokens.reserve(runtime->voices.size());
			for (int i = 0; i < runtime->voices.size(); ++i)
			{
				if (const auto token = buildVoiceToken(runtime, i); token)
					tokens.push_back(token);
			}
#endif
			return tokens;
		}
		if (kind == kTokenKindAudioOutputs)
		{
			ensureAudioOutputsEnumerated(runtime);
			tokens.reserve(runtime->audioOutputIds.size());
			for (int i = 0; i < runtime->audioOutputIds.size(); ++i)
			{
				if (const auto token = buildAudioOutputToken(runtime, i); token)
					tokens.push_back(token);
			}
			return tokens;
		}
		return tokens;
	}

	bool tokenMatchesConstraint(const std::shared_ptr<LuacomTokenData> &token,
	                            const VoiceAttributeConstraint         &constraint)
	{
		if (!token)
			return false;
		if (constraint.first == QStringLiteral("language"))
			return matchesLanguageConstraint(token->attributes.value(QStringLiteral("language")),
			                                 constraint.second);
		QString actual;
		if (constraint.first == QStringLiteral("id"))
			actual = token->id;
		else if (constraint.first == QStringLiteral("description"))
			actual = token->description;
		else
			actual = token->attributes.value(constraint.first);
		if (actual.isEmpty())
			return false;
		return actual.compare(constraint.second, Qt::CaseInsensitive) == 0 ||
		       actual.contains(constraint.second, Qt::CaseInsensitive);
	}

	QVector<std::shared_ptr<LuacomTokenData>>
	filterTokensByAttributes(const QVector<std::shared_ptr<LuacomTokenData>> &tokens, const char *requiredRaw,
	                         const char *optionalRaw)
	{
		const QVector<VoiceAttributeConstraint> required = parseVoiceAttributeConstraints(requiredRaw);
		const QVector<VoiceAttributeConstraint> optional = parseVoiceAttributeConstraints(optionalRaw);
		if (required.isEmpty() && optional.isEmpty())
			return tokens;

		QVector<std::shared_ptr<LuacomTokenData>> filtered;
		filtered.reserve(tokens.size());
		for (const auto &token : tokens)
		{
			bool allRequired = true;
			for (const auto &constraint : required)
			{
				if (tokenMatchesConstraint(token, constraint))
					continue;
				allRequired = false;
				break;
			}
			if (!allRequired)
				continue;

			if (!optional.isEmpty())
			{
				bool anyOptional = false;
				for (const auto &constraint : optional)
				{
					if (!tokenMatchesConstraint(token, constraint))
						continue;
					anyOptional = true;
					break;
				}
				if (!anyOptional)
					continue;
			}
			filtered.push_back(token);
		}
		return filtered;
	}

	int findVoiceIndexByTokenId(const std::shared_ptr<TtsVoiceRuntime> &runtime, const QString &tokenId)
	{
		if (!runtime)
			return -1;
		const qsizetype index = runtime->voiceIds.indexOf(tokenId);
		if (index < 0 || index > static_cast<qsizetype>(std::numeric_limits<int>::max()))
			return -1;
		return static_cast<int>(index);
	}

	int findAudioOutputIndexByTokenId(const std::shared_ptr<TtsVoiceRuntime> &runtime, const QString &tokenId)
	{
		if (!runtime)
			return -1;
		ensureAudioOutputsEnumerated(runtime);
		const qsizetype index = runtime->audioOutputIds.indexOf(tokenId);
		if (index < 0 || index > static_cast<qsizetype>(std::numeric_limits<int>::max()))
			return -1;
		return static_cast<int>(index);
	}

	QVector<int> collectAudioOutputIndexes(const std::shared_ptr<TtsVoiceRuntime> &runtime,
	                                       const char *requiredRaw, const char *optionalRaw)
	{
		QVector<int> indexes;
		if (!runtime)
			return indexes;
		auto tokens = collectTokenSet(runtime, kTokenKindAudioOutputs);
		tokens      = filterTokensByAttributes(tokens, requiredRaw, optionalRaw);
		indexes.reserve(tokens.size());
		for (const auto &token : tokens)
		{
			if (!token)
				continue;
			const int index = findAudioOutputIndexByTokenId(runtime, token->id);
			if (index < 0 || indexes.contains(index))
				continue;
			indexes.push_back(index);
		}
		return indexes;
	}

	QString lexiconKey(const QString &word, const int langId, const int partOfSpeech)
	{
		return QStringLiteral("%1|%2|%3")
		    .arg(word.trimmed().toLower(), QString::number(langId), QString::number(partOfSpeech));
	}

	QString canonicalPhoneIds(const QString &raw)
	{
		const QStringList tokens =
		    raw.split(QRegularExpression(QStringLiteral("[\\s,;]+")), Qt::SkipEmptyParts);
		QStringList numeric;
		numeric.reserve(tokens.size());
		for (const QString &token : tokens)
		{
			bool ok      = false;
			int  phoneId = token.toInt(&ok, 10);
			if (!ok || phoneId < 0)
				continue;
			numeric.push_back(QString::number(phoneId));
		}
		return numeric.join(' ');
	}

	QString normalizeSsmlLikeText(QString text)
	{
		if (text.isEmpty())
			return text;
		text.replace(QRegularExpression(QStringLiteral("<\\?.*?\\?>")), QStringLiteral(" "));
		text.replace(
		    QRegularExpression(QStringLiteral("<!--.*?-->"), QRegularExpression::DotMatchesEverythingOption),
		    QStringLiteral(" "));
		text.replace(QRegularExpression(QStringLiteral("<[^>]+>")), QStringLiteral(" "));
		text.replace(QStringLiteral("&lt;"), QStringLiteral("<"), Qt::CaseInsensitive);
		text.replace(QStringLiteral("&gt;"), QStringLiteral(">"), Qt::CaseInsensitive);
		text.replace(QStringLiteral("&amp;"), QStringLiteral("&"), Qt::CaseInsensitive);
		text.replace(QStringLiteral("&quot;"), QStringLiteral("\""), Qt::CaseInsensitive);
		text.replace(QStringLiteral("&apos;"), QStringLiteral("'"), Qt::CaseInsensitive);
		text.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
		return text.trimmed();
	}

	QString loadSpeakInputFromFile(const QString &path)
	{
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly))
			return {};
		const QByteArray bytes = file.readAll();
		return QString::fromUtf8(bytes.constData(), bytes.size());
	}

	QString preprocessSpeakText(const QString &rawInput, const int flags, bool *ok)
	{
		if (ok)
			*ok = true;
		QString input = rawInput;
		if ((flags & kSvsfIsFilename) != 0)
		{
			const QString loaded = loadSpeakInputFromFile(rawInput);
			if (loaded.isNull())
			{
				if (ok)
					*ok = false;
				return {};
			}
			input = loaded;
		}

		const bool explicitNotXml = (flags & kSvsfIsNotXml) != 0;
		const bool wantsXmlParse  = (flags & (kSvsfIsXml | kSvsfParseSapi | kSvsfParseSsml)) != 0;
		if (wantsXmlParse && !explicitNotXml)
			return normalizeSsmlLikeText(input);
		return input;
	}

	int qtRateToSapi(const double qtRate)
	{
		return TtsVoiceRuntime::qtRateToSapi(qtRate);
	}

	double sapiRateToQt(const int sapiRate)
	{
		return TtsVoiceRuntime::sapiRateToQt(sapiRate);
	}

	void ensureAudioOutputsEnumerated(const std::shared_ptr<TtsVoiceRuntime> &runtime)
	{
		if (!runtime)
			return;
		runtime->ensureAudioOutputsEnumerated();
	}

	bool hasVoiceAt(const std::shared_ptr<TtsVoiceRuntime> &runtime, const int index)
	{
		return runtime && runtime->hasVoiceAt(index);
	}

	bool hasAudioOutputAt(const std::shared_ptr<TtsVoiceRuntime> &runtime, const int index)
	{
		return runtime && runtime->hasAudioOutputAt(index);
	}

	void selectVoice(const std::shared_ptr<TtsVoiceRuntime> &runtime, const int index)
	{
		if (!runtime)
			return;
		runtime->selectVoice(index);
	}

	void selectAudioOutput(const std::shared_ptr<TtsVoiceRuntime> &runtime, const int index)
	{
		if (!runtime)
			return;
		runtime->selectAudioOutput(index);
	}

	void enqueueUtterance(const std::shared_ptr<TtsVoiceRuntime> &runtime, QString utterance,
	                      const bool purgeBeforeSpeak)
	{
		if (!runtime)
			return;
		runtime->enqueueUtterance(std::move(utterance), purgeBeforeSpeak);
	}

	bool waitForSpeechIdle(const std::shared_ptr<TtsVoiceRuntime> &runtime, const int timeoutMs)
	{
		if (!runtime)
			return true;
		return runtime->waitForSpeechIdle(timeoutMs);
	}

	int skipQueuedUtterances(const std::shared_ptr<TtsVoiceRuntime> &runtime, const int count)
	{
		if (!runtime)
			return 0;
		return runtime->skipQueuedUtterances(count);
	}

	bool synthesizeToOpenOutputStream(const LuacomSpVoice *voice, const QString &utterance,
	                                  const bool purgeBeforeSpeak)
	{
		if (!voice || !voice->runtime || !voice->runtime->hasSpeech() || !voice->outputStream)
		{
			return false;
		}
		auto stream = voice->outputStream;
		{
			QMutexLocker locker(&stream->mutex);
			if (!stream->file.isOpen())
				return false;
			stream->writeFailed = false;
			stream->writeError.clear();
		}
		auto                                       runtime     = voice->runtime;
		const std::weak_ptr<LuacomFileStreamState> streamState = stream;
		return runtime->synthesizeToStream(
		    utterance, purgeBeforeSpeak,
		    [streamState](const QByteArray &data)
		    {
			    if (data.isEmpty())
				    return;
			    const auto stream = streamState.lock();
			    if (!stream)
				    return;
			    QMutexLocker locker(&stream->mutex);
			    if (!stream->file.isOpen() || stream->writeFailed)
				    return;
			    const qint64 written = stream->file.write(data);
			    if (written == data.size())
				    return;
			    stream->writeFailed = true;
			    if (stream->writeError.isEmpty())
			    {
				    const QString fileError = stream->file.errorString().trimmed();
				    stream->writeError =
				        fileError.isEmpty() ? QStringLiteral("stream write failed") : fileError;
			    }
			    qWarning().noquote() << "Lua luacom stream synthesis write failed:" << stream->writeError;
		    });
	}

	QString speakPunctuation(const QString &utterance)
	{
		if (utterance.isEmpty())
			return utterance;
		QString transformed;
		transformed.reserve(utterance.size() * 2);
		for (const QChar ch : utterance)
		{
			switch (ch.unicode())
			{
			case '.':
				transformed.append(QStringLiteral(" period "));
				break;
			case ',':
				transformed.append(QStringLiteral(" comma "));
				break;
			case '!':
				transformed.append(QStringLiteral(" exclamation mark "));
				break;
			case '?':
				transformed.append(QStringLiteral(" question mark "));
				break;
			case ':':
				transformed.append(QStringLiteral(" colon "));
				break;
			case ';':
				transformed.append(QStringLiteral(" semicolon "));
				break;
			case '-':
				transformed.append(QStringLiteral(" dash "));
				break;
			case '(':
				transformed.append(QStringLiteral(" left parenthesis "));
				break;
			case ')':
				transformed.append(QStringLiteral(" right parenthesis "));
				break;
			case '[':
				transformed.append(QStringLiteral(" left bracket "));
				break;
			case ']':
				transformed.append(QStringLiteral(" right bracket "));
				break;
			case '{':
				transformed.append(QStringLiteral(" left brace "));
				break;
			case '}':
				transformed.append(QStringLiteral(" right brace "));
				break;
			case '"':
				transformed.append(QStringLiteral(" quote "));
				break;
			case '\'':
				transformed.append(QStringLiteral(" apostrophe "));
				break;
			case '/':
				transformed.append(QStringLiteral(" slash "));
				break;
			case '\\':
				transformed.append(QStringLiteral(" backslash "));
				break;
			default:
				transformed.append(ch);
				break;
			}
		}
		return transformed;
	}

	std::shared_ptr<TtsVoiceRuntime> makeRuntime()
	{
		QMudLuacomShim::RuntimeFactory factory;
		{
			QMutexLocker locker(&gRuntimeFactoryMutex);
			factory = gRuntimeFactory;
		}
		if (factory)
			return factory();
		return TtsVoiceRuntime::create();
	}

	void pushVoiceByIndex(lua_State *L, const std::shared_ptr<TtsVoiceRuntime> &runtime, const int index)
	{
		if (!hasVoiceAt(runtime, index))
		{
			lua_pushnil(L);
			return;
		}
		pushHandle(L, new LuacomVoice{runtime, index}, kVoiceMeta);
	}

	void pushAudioOutputByIndex(lua_State *L, const std::shared_ptr<TtsVoiceRuntime> &runtime,
	                            const int index)
	{
		if (!hasAudioOutputAt(runtime, index))
		{
			lua_pushnil(L);
			return;
		}
		pushHandle(L, new LuacomAudioOutput{runtime, index}, kAudioOutputMeta);
	}

	void pushToken(lua_State *L, const std::shared_ptr<LuacomTokenData> &token)
	{
		if (!token)
		{
			lua_pushnil(L);
			return;
		}
		pushHandle(L, new LuacomToken{token}, kTokenMeta);
	}

	int spVoiceGc(lua_State *L)
	{
		auto **ud = static_cast<LuacomSpVoice **>(luaL_checkudata(L, 1, kSpVoiceMeta));
		if (ud && *ud)
		{
			delete *ud;
			*ud = nullptr;
		}
		return 0;
	}

	int voicesCollectionGc(lua_State *L)
	{
		auto **ud = static_cast<LuacomVoicesCollection **>(luaL_checkudata(L, 1, kVoicesCollectionMeta));
		if (ud && *ud)
		{
			delete *ud;
			*ud = nullptr;
		}
		return 0;
	}

	int voiceGc(lua_State *L)
	{
		auto **ud = static_cast<LuacomVoice **>(luaL_checkudata(L, 1, kVoiceMeta));
		if (ud && *ud)
		{
			delete *ud;
			*ud = nullptr;
		}
		return 0;
	}

	int audioOutputsCollectionGc(lua_State *L)
	{
		auto **ud =
		    static_cast<LuacomAudioOutputsCollection **>(luaL_checkudata(L, 1, kAudioOutputsCollectionMeta));
		if (ud && *ud)
		{
			delete *ud;
			*ud = nullptr;
		}
		return 0;
	}

	int audioOutputGc(lua_State *L)
	{
		auto **ud = static_cast<LuacomAudioOutput **>(luaL_checkudata(L, 1, kAudioOutputMeta));
		if (ud && *ud)
		{
			delete *ud;
			*ud = nullptr;
		}
		return 0;
	}

	int voiceStatusGc(lua_State *L)
	{
		auto **ud = static_cast<LuacomVoiceStatus **>(luaL_checkudata(L, 1, kVoiceStatusMeta));
		if (ud && *ud)
		{
			delete *ud;
			*ud = nullptr;
		}
		return 0;
	}

	int tokenGc(lua_State *L)
	{
		auto **ud = static_cast<LuacomToken **>(luaL_checkudata(L, 1, kTokenMeta));
		if (ud && *ud)
		{
			delete *ud;
			*ud = nullptr;
		}
		return 0;
	}

	int tokensCollectionGc(lua_State *L)
	{
		auto **ud = static_cast<LuacomTokensCollection **>(luaL_checkudata(L, 1, kTokensCollectionMeta));
		if (ud && *ud)
		{
			delete *ud;
			*ud = nullptr;
		}
		return 0;
	}

	int tokenCategoryGc(lua_State *L)
	{
		auto **ud = static_cast<LuacomTokenCategory **>(luaL_checkudata(L, 1, kTokenCategoryMeta));
		if (ud && *ud)
		{
			delete *ud;
			*ud = nullptr;
		}
		return 0;
	}

	int lexiconGc(lua_State *L)
	{
		auto **ud = static_cast<LuacomLexicon **>(luaL_checkudata(L, 1, kLexiconMeta));
		if (ud && *ud)
		{
			delete *ud;
			*ud = nullptr;
		}
		return 0;
	}

	int fileStreamGc(lua_State *L)
	{
		auto **ud = static_cast<LuacomFileStream **>(luaL_checkudata(L, 1, kFileStreamMeta));
		if (ud && *ud)
		{
			if ((*ud)->state && (*ud)->state.use_count() <= 1)
			{
				QMutexLocker locker(&(*ud)->state->mutex);
				if ((*ud)->state->file.isOpen())
					(*ud)->state->file.close();
			}
			delete *ud;
			*ud = nullptr;
		}
		return 0;
	}

	int audioFormatGc(lua_State *L)
	{
		auto **ud = static_cast<LuacomAudioFormat **>(luaL_checkudata(L, 1, kAudioFormatMeta));
		if (ud && *ud)
		{
			delete *ud;
			*ud = nullptr;
		}
		return 0;
	}

	int enumeratorGc(lua_State *L)
	{
		auto **ud = static_cast<LuacomEnumerator **>(luaL_checkudata(L, 1, kEnumeratorMeta));
		if (ud && *ud)
		{
			delete *ud;
			*ud = nullptr;
		}
		return 0;
	}

	int typeInfoGc(lua_State *L)
	{
		auto **ud = static_cast<LuacomTypeInfo **>(luaL_checkudata(L, 1, kTypeInfoMeta));
		if (ud && *ud)
		{
			delete *ud;
			*ud = nullptr;
		}
		return 0;
	}

	int typeLibGc(lua_State *L)
	{
		auto **ud = static_cast<LuacomTypeLib **>(luaL_checkudata(L, 1, kTypeLibMeta));
		if (ud && *ud)
		{
			delete *ud;
			*ud = nullptr;
		}
		return 0;
	}

	bool assignVoiceHandle(const std::shared_ptr<TtsVoiceRuntime> &runtime, const LuacomVoice *selectedVoice)
	{
		if (!runtime || !selectedVoice || runtime != selectedVoice->runtime)
			return false;
		if (!hasVoiceAt(runtime, selectedVoice->index))
			return false;
		selectVoice(runtime, selectedVoice->index);
		return true;
	}

	bool assignAudioOutputHandle(const std::shared_ptr<TtsVoiceRuntime> &runtime,
	                             const LuacomAudioOutput                *selectedOutput)
	{
		if (!runtime || !selectedOutput || runtime != selectedOutput->runtime)
			return false;
		if (!hasAudioOutputAt(runtime, selectedOutput->index))
			return false;
		selectAudioOutput(runtime, selectedOutput->index);
		return true;
	}

	bool assignVoiceToken(const std::shared_ptr<TtsVoiceRuntime> &runtime, const LuacomToken *token)
	{
		if (!runtime || !token || !token->token)
			return false;
		if (token->token->kind == kTokenKindAudioOutputs)
			return false;
		const int index = findVoiceIndexByTokenId(runtime, token->token->id);
		if (index < 0 || !hasVoiceAt(runtime, index))
			return false;
		selectVoice(runtime, index);
		return true;
	}

	bool assignAudioOutputToken(const std::shared_ptr<TtsVoiceRuntime> &runtime, const LuacomToken *token)
	{
		if (!runtime || !token || !token->token)
			return false;
		if (token->token->kind == kTokenKindVoices)
			return false;
		const int index = findAudioOutputIndexByTokenId(runtime, token->token->id);
		if (index < 0 || !hasAudioOutputAt(runtime, index))
			return false;
		selectAudioOutput(runtime, index);
		return true;
	}

	int spVoiceSpeak(lua_State *L)
	{
		const auto *voice = checkHandle<LuacomSpVoice>(L, kSpVoiceMeta, "SpVoice expected");
		const char *text  = luaL_optstring(L, 2, "");
		const int   flags = static_cast<int>(luaL_optinteger(L, 3, 0));
		int         streamNumber{0};
		if (voice->runtime)
		{
			voice->runtime->lastStreamNumber = std::max(0, voice->runtime->lastStreamNumber) + 1;
			streamNumber                     = voice->runtime->lastStreamNumber;
		}
#if QMUD_ENABLE_TEXT_TO_SPEECH
		if (voice->runtime && voice->runtime->hasSpeech())
		{
			const bool purgeBeforeSpeak = (flags & kSvsfPurgeBeforeSpeak) != 0;
			const bool speakAsync       = (flags & kSvsFlagsAsync) != 0;
			bool       ok               = true;
			QString    utterance = preprocessSpeakText(QString::fromUtf8(text ? text : ""), flags, &ok);
			if (!ok)
				return luaL_error(L, "Unable to open filename for speech input");
			if ((flags & kSvsfNlpSpeakPunc) != 0)
				utterance = speakPunctuation(utterance);
			const std::shared_ptr<LuacomFileStreamState> activeOutputStream = voice->outputStream;
			const bool wroteToStream = synthesizeToOpenOutputStream(voice, utterance, purgeBeforeSpeak);
			if (!wroteToStream)
				enqueueUtterance(voice->runtime, std::move(utterance), purgeBeforeSpeak);
			if (!speakAsync)
			{
				waitForSpeechIdle(voice->runtime, voice->runtime->synchronousSpeakTimeoutMs);
				if (wroteToStream && activeOutputStream)
				{
					QString streamWriteError;
					{
						QMutexLocker locker(&activeOutputStream->mutex);
						if (activeOutputStream->writeFailed)
							streamWriteError = activeOutputStream->writeError;
					}
					if (!streamWriteError.isEmpty())
					{
						const QByteArray errorBytes = streamWriteError.toUtf8();
						return luaL_error(L, "Unable to write synthesized audio stream: %s",
						                  errorBytes.constData());
					}
				}
			}
		}
#else
		Q_UNUSED(voice);
		Q_UNUSED(text);
		Q_UNUSED(flags);
#endif
		lua_pushinteger(L, streamNumber);
		return 1;
	}

	int spVoiceSkip(lua_State *L)
	{
		const auto *voice = checkHandle<LuacomSpVoice>(L, kSpVoiceMeta, "SpVoice expected");
		luaL_optstring(L, 2, "Sentence");
		const int count      = std::max(0, static_cast<int>(luaL_optinteger(L, 3, 1)));
		int       skipResult = 0;
#if QMUD_ENABLE_TEXT_TO_SPEECH
		skipResult = skipQueuedUtterances(voice->runtime, count);
#else
		Q_UNUSED(voice);
		Q_UNUSED(count);
#endif
		lua_pushinteger(L, skipResult);
		return 1;
	}

	int spVoiceGetVoices(lua_State *L)
	{
		const auto  *voice              = checkHandle<LuacomSpVoice>(L, kSpVoiceMeta, "SpVoice expected");
		const char  *requiredAttributes = luaL_optstring(L, 2, nullptr);
		const char  *optionalAttributes = luaL_optstring(L, 3, nullptr);
		QVector<int> indexes;
#if QMUD_ENABLE_TEXT_TO_SPEECH
		indexes = collectVoiceIndexes(voice->runtime, requiredAttributes, optionalAttributes);
#else
		Q_UNUSED(requiredAttributes);
		Q_UNUSED(optionalAttributes);
#endif
		pushHandle(L, new LuacomVoicesCollection{voice->runtime, std::move(indexes)}, kVoicesCollectionMeta);
		return 1;
	}

	int spVoiceGetAudioOutputs(lua_State *L)
	{
		const auto *voice              = checkHandle<LuacomSpVoice>(L, kSpVoiceMeta, "SpVoice expected");
		const char *requiredAttributes = luaL_optstring(L, 2, nullptr);
		const char *optionalAttributes = luaL_optstring(L, 3, nullptr);
		const auto  indexes =
		    collectAudioOutputIndexes(voice->runtime, requiredAttributes, optionalAttributes);
		pushHandle(L, new LuacomAudioOutputsCollection{voice->runtime, indexes}, kAudioOutputsCollectionMeta);
		return 1;
	}

	int spVoiceSetVoice(lua_State *L)
	{
		const auto *voice = checkHandle<LuacomSpVoice>(L, kSpVoiceMeta, "SpVoice expected");
		if (lua_isnoneornil(L, 2))
			return 0;
		auto **ud = static_cast<LuacomVoice **>(luaL_testudata(L, 2, kVoiceMeta));
		if (ud && *ud)
		{
			assignVoiceHandle(voice->runtime, *ud);
			return 0;
		}
		auto **token = static_cast<LuacomToken **>(luaL_testudata(L, 2, kTokenMeta));
		if (token && *token)
		{
			assignVoiceToken(voice->runtime, *token);
			return 0;
		}
		return luaL_argerror(L, 2, "Voice or token expected");
	}

	int spVoiceSetAudioOutput(lua_State *L)
	{
		const auto *voice = checkHandle<LuacomSpVoice>(L, kSpVoiceMeta, "SpVoice expected");
		if (lua_isnoneornil(L, 2))
			return 0;
		auto **ud = static_cast<LuacomAudioOutput **>(luaL_testudata(L, 2, kAudioOutputMeta));
		if (ud && *ud)
		{
			assignAudioOutputHandle(voice->runtime, *ud);
			return 0;
		}
		auto **token = static_cast<LuacomToken **>(luaL_testudata(L, 2, kTokenMeta));
		if (token && *token)
		{
			assignAudioOutputToken(voice->runtime, *token);
			return 0;
		}
		return luaL_argerror(L, 2, "Audio output or token expected");
	}

	int spVoiceSetAudioOutputStream(lua_State *L)
	{
		auto *voice = checkHandle<LuacomSpVoice>(L, kSpVoiceMeta, "SpVoice expected");
		if (lua_isnoneornil(L, 2))
		{
			voice->outputStream.reset();
			return 0;
		}
		auto **ud = static_cast<LuacomFileStream **>(luaL_testudata(L, 2, kFileStreamMeta));
		if (!ud || !*ud)
			return luaL_argerror(L, 2, "File stream expected");
		voice->outputStream = (*ud)->state;
		return 0;
	}

	int spVoicePause(lua_State *L)
	{
		const auto *voice = checkHandle<LuacomSpVoice>(L, kSpVoiceMeta, "SpVoice expected");
#if QMUD_ENABLE_TEXT_TO_SPEECH
		if (voice->runtime && voice->runtime->hasSpeech())
		{
			voice->runtime->pause();
		}
#else
		Q_UNUSED(voice);
#endif
		return 0;
	}

	int spVoiceResume(lua_State *L)
	{
		const auto *voice = checkHandle<LuacomSpVoice>(L, kSpVoiceMeta, "SpVoice expected");
#if QMUD_ENABLE_TEXT_TO_SPEECH
		if (voice->runtime && voice->runtime->hasSpeech())
		{
			voice->runtime->resume();
		}
#else
		Q_UNUSED(voice);
#endif
		return 0;
	}

	int spVoiceWaitUntilDone(lua_State *L)
	{
		const auto *voice     = checkHandle<LuacomSpVoice>(L, kSpVoiceMeta, "SpVoice expected");
		const int   timeoutMs = static_cast<int>(luaL_optinteger(
            L, 2, voice->runtime ? voice->runtime->synchronousSpeakTimeoutMs : kDefaultSyncTimeoutMs));
		bool        completed = true;
#if QMUD_ENABLE_TEXT_TO_SPEECH
		if (voice->runtime && voice->runtime->hasSpeech())
			completed = waitForSpeechIdle(voice->runtime, timeoutMs);
#else
		Q_UNUSED(voice);
		Q_UNUSED(timeoutMs);
#endif
		lua_pushboolean(L, completed ? 1 : 0);
		return lua_gettop(L) > 0 ? 1 : 0;
	}

	int spVoiceIsUiSupported(lua_State *L)
	{
		checkHandle<LuacomSpVoice>(L, kSpVoiceMeta, "SpVoice expected");
		luaL_checkstring(L, 2);
		luaL_optstring(L, 3, nullptr);
		lua_pushboolean(L, 0);
		return 1;
	}

	int spVoiceDisplayUi(lua_State *L)
	{
		checkHandle<LuacomSpVoice>(L, kSpVoiceMeta, "SpVoice expected");
		luaL_optstring(L, 2, nullptr);
		luaL_optstring(L, 3, nullptr);
		luaL_optstring(L, 4, nullptr);
		return 0;
	}

	int spVoiceIndex(lua_State *L)
	{
		auto       *voice = checkHandle<LuacomSpVoice>(L, kSpVoiceMeta, "SpVoice expected");
		const char *key   = luaL_checkstring(L, 2);
		if (strcmp(key, "Rate") == 0)
		{
#if QMUD_ENABLE_TEXT_TO_SPEECH
			if (voice->runtime && voice->runtime->hasSpeech())
			{
				const double rate    = voice->runtime->speechRate();
				voice->runtime->rate = qtRateToSapi(rate);
				lua_pushinteger(L, voice->runtime->rate);
			}
			else
#endif
				lua_pushinteger(L, voice->runtime ? voice->runtime->rate : 0);
			return 1;
		}
		if (strcmp(key, "Voice") == 0)
		{
			pushVoiceByIndex(L, voice->runtime, voice->runtime ? voice->runtime->currentVoiceIndex : -1);
			return 1;
		}
		if (strcmp(key, "Volume") == 0)
		{
#if QMUD_ENABLE_TEXT_TO_SPEECH
			if (voice->runtime && voice->runtime->hasSpeech())
			{
				const double volumeLevel      = voice->runtime->speechVolume();
				const int    volume           = static_cast<int>(std::lround(volumeLevel * 100.0));
				voice->runtime->volumePercent = std::clamp(volume, 0, 100);
			}
#endif
			lua_pushinteger(L, voice->runtime ? voice->runtime->volumePercent : kDefaultVolumePercent);
			return 1;
		}
		if (strcmp(key, "AudioOutput") == 0)
		{
			pushAudioOutputByIndex(L, voice->runtime,
			                       voice->runtime ? voice->runtime->currentAudioOutputIndex : -1);
			return 1;
		}
		if (strcmp(key, "AudioOutputStream") == 0)
		{
			if (!voice->outputStream)
				return 0;
			pushHandle(L, new LuacomFileStream{voice->outputStream}, kFileStreamMeta);
			return 1;
		}
		if (strcmp(key, "Status") == 0)
		{
			pushHandle(L, new LuacomVoiceStatus{voice->runtime}, kVoiceStatusMeta);
			return 1;
		}
		if (strcmp(key, "Priority") == 0)
		{
			lua_pushinteger(L, voice->runtime ? voice->runtime->priority : kSvpNormal);
			return 1;
		}
		if (strcmp(key, "EventInterests") == 0)
		{
			lua_pushinteger(L, voice->runtime ? voice->runtime->eventInterests : kSveAllEvents);
			return 1;
		}
		if (strcmp(key, "AlertBoundary") == 0)
		{
			lua_pushinteger(L, voice->runtime ? voice->runtime->alertBoundary : 0);
			return 1;
		}
		if (strcmp(key, "SynchronousSpeakTimeout") == 0)
		{
			lua_pushinteger(L, voice->runtime ? voice->runtime->synchronousSpeakTimeoutMs
			                                  : kDefaultSyncTimeoutMs);
			return 1;
		}
		if (strcmp(key, "AllowAudioOutputFormatChangesOnNextSet") == 0)
		{
			lua_pushboolean(
			    L, (voice->runtime && voice->runtime->allowAudioOutputFormatChangesOnNextSet) ? 1 : 0);
			return 1;
		}
		if (strcmp(key, "SpeakCompleteEvent") == 0)
		{
			lua_pushinteger(L, voice->runtime ? voice->runtime->lastStreamNumber : 0);
			return 1;
		}

		luaL_getmetatable(L, kSpVoiceMeta);
		lua_pushvalue(L, 2);
		lua_rawget(L, -2);
		lua_remove(L, -2);
		if (lua_isnil(L, -1))
		{
			lua_pop(L, 1);
			return 0;
		}
		return 1;
	}

	int spVoiceNewIndex(lua_State *L)
	{
		auto       *voice = checkHandle<LuacomSpVoice>(L, kSpVoiceMeta, "SpVoice expected");
		const char *key   = luaL_checkstring(L, 2);
		if (strcmp(key, "Rate") == 0)
		{
			if (voice->runtime)
			{
				const int sapiRate   = static_cast<int>(luaL_checkinteger(L, 3));
				voice->runtime->rate = std::clamp(sapiRate, -10, 10);
#if QMUD_ENABLE_TEXT_TO_SPEECH
				if (voice->runtime->hasSpeech())
					voice->runtime->setSpeechRate(sapiRateToQt(voice->runtime->rate));
#endif
			}
			return 0;
		}
		if (strcmp(key, "Voice") == 0)
		{
			if (lua_isnoneornil(L, 3))
				return 0;
			auto **ud = static_cast<LuacomVoice **>(luaL_testudata(L, 3, kVoiceMeta));
			if (ud && *ud)
			{
				assignVoiceHandle(voice->runtime, *ud);
				return 0;
			}
			auto **token = static_cast<LuacomToken **>(luaL_testudata(L, 3, kTokenMeta));
			if (token && *token)
			{
				assignVoiceToken(voice->runtime, *token);
				return 0;
			}
			return luaL_argerror(L, 3, "Voice or token expected");
		}
		if (strcmp(key, "AudioOutput") == 0)
		{
			if (lua_isnoneornil(L, 3))
				return 0;
			auto **ud = static_cast<LuacomAudioOutput **>(luaL_testudata(L, 3, kAudioOutputMeta));
			if (ud && *ud)
			{
				assignAudioOutputHandle(voice->runtime, *ud);
				return 0;
			}
			auto **token = static_cast<LuacomToken **>(luaL_testudata(L, 3, kTokenMeta));
			if (token && *token)
			{
				assignAudioOutputToken(voice->runtime, *token);
				return 0;
			}
			return luaL_argerror(L, 3, "Audio output or token expected");
		}
		if (strcmp(key, "AudioOutputStream") == 0)
		{
			if (lua_isnoneornil(L, 3))
			{
				voice->outputStream.reset();
				return 0;
			}
			auto **ud = static_cast<LuacomFileStream **>(luaL_testudata(L, 3, kFileStreamMeta));
			if (!ud || !*ud)
				return luaL_argerror(L, 3, "File stream expected");
			voice->outputStream = (*ud)->state;
			return 0;
		}
		if (strcmp(key, "Volume") == 0)
		{
			if (voice->runtime)
			{
				const int volume              = std::clamp(static_cast<int>(luaL_checkinteger(L, 3)), 0, 100);
				voice->runtime->volumePercent = volume;
#if QMUD_ENABLE_TEXT_TO_SPEECH
				if (voice->runtime->hasSpeech())
					voice->runtime->setSpeechVolume(static_cast<double>(volume) / 100.0);
#endif
			}
			return 0;
		}
		if (strcmp(key, "Priority") == 0)
		{
			if (voice->runtime)
				voice->runtime->priority =
				    std::clamp(static_cast<int>(luaL_checkinteger(L, 3)), kSvpNormal, kSvpOver);
			return 0;
		}
		if (strcmp(key, "EventInterests") == 0)
		{
			if (voice->runtime)
				voice->runtime->eventInterests = static_cast<int>(luaL_checkinteger(L, 3));
			return 0;
		}
		if (strcmp(key, "AlertBoundary") == 0)
		{
			if (voice->runtime)
				voice->runtime->alertBoundary = static_cast<int>(luaL_checkinteger(L, 3));
			return 0;
		}
		if (strcmp(key, "SynchronousSpeakTimeout") == 0)
		{
			if (voice->runtime)
				voice->runtime->synchronousSpeakTimeoutMs =
				    std::max(0, static_cast<int>(luaL_checkinteger(L, 3)));
			return 0;
		}
		if (strcmp(key, "AllowAudioOutputFormatChangesOnNextSet") == 0)
		{
			if (voice->runtime)
				voice->runtime->allowAudioOutputFormatChangesOnNextSet = lua_toboolean(L, 3) != 0;
			return 0;
		}
		return luaL_error(L, "Unsupported SpVoice property assignment: %s", key);
	}

	int voicesCollectionItem(lua_State *L)
	{
		const auto *voices =
		    checkHandle<LuacomVoicesCollection>(L, kVoicesCollectionMeta, "Voices collection expected");
		const int index     = static_cast<int>(luaL_checkinteger(L, 2));
		const int itemIndex = (index >= 0 && index < voices->indexes.size()) ? voices->indexes.at(index) : -1;
		pushVoiceByIndex(L, voices->runtime, itemIndex);
		return 1;
	}

	int voicesCollectionIndex(lua_State *L)
	{
		auto *voices =
		    checkHandle<LuacomVoicesCollection>(L, kVoicesCollectionMeta, "Voices collection expected");
		const char *key = luaL_checkstring(L, 2);
		if (strcmp(key, "Count") == 0)
		{
			lua_pushinteger(L, voices->indexes.size());
			return 1;
		}

		luaL_getmetatable(L, kVoicesCollectionMeta);
		lua_pushvalue(L, 2);
		lua_rawget(L, -2);
		lua_remove(L, -2);
		if (lua_isnil(L, -1))
		{
			lua_pop(L, 1);
			return 0;
		}
		return 1;
	}

	int audioOutputsCollectionItem(lua_State *L)
	{
		const auto *outputs = checkHandle<LuacomAudioOutputsCollection>(L, kAudioOutputsCollectionMeta,
		                                                                "Audio outputs collection expected");
		const int   index   = static_cast<int>(luaL_checkinteger(L, 2));
		const int   itemIndex =
            (index >= 0 && index < outputs->indexes.size()) ? outputs->indexes.at(index) : -1;
		pushAudioOutputByIndex(L, outputs->runtime, itemIndex);
		return 1;
	}

	int audioOutputsCollectionIndex(lua_State *L)
	{
		const auto *outputs = checkHandle<LuacomAudioOutputsCollection>(L, kAudioOutputsCollectionMeta,
		                                                                "Audio outputs collection expected");
		const char *key     = luaL_checkstring(L, 2);
		if (strcmp(key, "Count") == 0)
		{
			lua_pushinteger(L, outputs->indexes.size());
			return 1;
		}

		luaL_getmetatable(L, kAudioOutputsCollectionMeta);
		lua_pushvalue(L, 2);
		lua_rawget(L, -2);
		lua_remove(L, -2);
		if (lua_isnil(L, -1))
		{
			lua_pop(L, 1);
			return 0;
		}
		return 1;
	}

	int audioOutputGetDescription(lua_State *L)
	{
		const auto *output = checkHandle<LuacomAudioOutput>(L, kAudioOutputMeta, "Audio output expected");
		if (!output->runtime || output->index < 0 ||
		    output->index >= output->runtime->audioOutputDescriptions.size())
		{
			return luaL_error(L, "Audio output handle is no longer valid");
		}
		const QByteArray description = output->runtime->audioOutputDescriptions.at(output->index).toUtf8();
		lua_pushlstring(L, description.constData(), description.size());
		return 1;
	}

	int audioOutputIndex(lua_State *L)
	{
		const auto *output = checkHandle<LuacomAudioOutput>(L, kAudioOutputMeta, "Audio output expected");
		const char *key    = luaL_checkstring(L, 2);
		if (strcmp(key, "ID") == 0)
		{
			if (output->runtime && output->index >= 0 &&
			    output->index < output->runtime->audioOutputIds.size())
			{
				lua_pushstring(L, output->runtime->audioOutputIds.at(output->index).toUtf8().constData());
				return 1;
			}
			lua_pushstring(L, R"(QMUD\TTS\Audio\UNKNOWN)");
			return 1;
		}

		luaL_getmetatable(L, kAudioOutputMeta);
		lua_pushvalue(L, 2);
		lua_rawget(L, -2);
		lua_remove(L, -2);
		if (lua_isnil(L, -1))
		{
			lua_pop(L, 1);
			return 0;
		}
		return 1;
	}

	int tokenSetId(lua_State *L)
	{
		auto       *token    = checkHandle<LuacomToken>(L, kTokenMeta, "Token expected");
		const char *id       = luaL_checkstring(L, 2);
		const char *category = luaL_optstring(L, 3, nullptr);
		lua_toboolean(L, 4);
		if (!token->token)
			token->token = std::make_shared<LuacomTokenData>();
		token->token->id = QString::fromUtf8(id ? id : "").trimmed();
		if (category)
			token->token->kind = tokenKindFromCategoryId(QString::fromUtf8(category));
		else if (token->token->kind == kTokenKindUnknown)
			token->token->kind = tokenKindFromCategoryId(token->token->id);
		if (token->token->description.isEmpty())
			token->token->description = token->token->id;
		token->token->attributes.insert(QStringLiteral("id"), token->token->id);
		return 0;
	}

	int tokenGetDescription(lua_State *L)
	{
		const auto *token = checkHandle<LuacomToken>(L, kTokenMeta, "Token expected");
		luaL_optstring(L, 2, nullptr);
		const QByteArray description = (token->token ? token->token->description : QString()).toUtf8();
		lua_pushlstring(L, description.constData(), description.size());
		return 1;
	}

	int tokenGetAttribute(lua_State *L)
	{
		const auto *token = checkHandle<LuacomToken>(L, kTokenMeta, "Token expected");
		const char *name  = luaL_checkstring(L, 2);
		if (!token->token)
		{
			lua_pushstring(L, "");
			return 1;
		}
		const QString key   = QString::fromUtf8(name ? name : "").trimmed().toLower();
		QString       value = token->token->attributes.value(key);
		if (value.isEmpty() && key == QStringLiteral("id"))
			value = token->token->id;
		if (value.isEmpty() && key == QStringLiteral("description"))
			value = token->token->description;
		const QByteArray bytes = value.toUtf8();
		lua_pushlstring(L, bytes.constData(), bytes.size());
		return 1;
	}

	int tokenIndex(lua_State *L)
	{
		const auto *token = checkHandle<LuacomToken>(L, kTokenMeta, "Token expected");
		const char *key   = luaL_checkstring(L, 2);
		if (strcmp(key, "Id") == 0 || strcmp(key, "ID") == 0)
		{
			const QByteArray id = (token->token ? token->token->id : QString()).toUtf8();
			lua_pushlstring(L, id.constData(), id.size());
			return 1;
		}
		luaL_getmetatable(L, kTokenMeta);
		lua_pushvalue(L, 2);
		lua_rawget(L, -2);
		lua_remove(L, -2);
		if (lua_isnil(L, -1))
		{
			lua_pop(L, 1);
			return 0;
		}
		return 1;
	}

	int tokenNewIndex(lua_State *L)
	{
		auto       *token = checkHandle<LuacomToken>(L, kTokenMeta, "Token expected");
		const char *key   = luaL_checkstring(L, 2);
		if (strcmp(key, "Id") == 0 || strcmp(key, "ID") == 0)
		{
			if (!token->token)
				token->token = std::make_shared<LuacomTokenData>();
			const char *id   = luaL_checkstring(L, 3);
			token->token->id = QString::fromUtf8(id ? id : "");
			if (token->token->kind == kTokenKindUnknown)
				token->token->kind = tokenKindFromCategoryId(token->token->id);
			token->token->attributes.insert(QStringLiteral("id"), token->token->id);
			return 0;
		}
		return luaL_error(L, "Unsupported token property assignment: %s", key);
	}

	int tokensCollectionItem(lua_State *L)
	{
		const auto *tokens =
		    checkHandle<LuacomTokensCollection>(L, kTokensCollectionMeta, "Tokens collection expected");
		const int  index = static_cast<int>(luaL_checkinteger(L, 2));
		const auto item  = (index >= 0 && index < tokens->tokens.size()) ? tokens->tokens.at(index)
		                                                                 : std::shared_ptr<LuacomTokenData>{};
		pushToken(L, item);
		return 1;
	}

	int tokensCollectionIndex(lua_State *L)
	{
		const auto *tokens =
		    checkHandle<LuacomTokensCollection>(L, kTokensCollectionMeta, "Tokens collection expected");
		const char *key = luaL_checkstring(L, 2);
		if (strcmp(key, "Count") == 0)
		{
			lua_pushinteger(L, tokens->tokens.size());
			return 1;
		}
		luaL_getmetatable(L, kTokensCollectionMeta);
		lua_pushvalue(L, 2);
		lua_rawget(L, -2);
		lua_remove(L, -2);
		if (lua_isnil(L, -1))
		{
			lua_pop(L, 1);
			return 0;
		}
		return 1;
	}

	int tokenCategorySetId(lua_State *L)
	{
		auto *category = checkHandle<LuacomTokenCategory>(L, kTokenCategoryMeta, "Token category expected");
		const char *id = luaL_checkstring(L, 2);
		lua_toboolean(L, 3);
		category->categoryId = QString::fromUtf8(id ? id : "").trimmed();
		category->kind       = tokenKindFromCategoryId(category->categoryId);
		return 0;
	}

	int tokenCategoryEnumerateTokens(lua_State *L)
	{
		const auto *category =
		    checkHandle<LuacomTokenCategory>(L, kTokenCategoryMeta, "Token category expected");
		const char *requiredAttributes = luaL_optstring(L, 2, nullptr);
		const char *optionalAttributes = luaL_optstring(L, 3, nullptr);
		auto        tokens             = collectTokenSet(category->runtime, category->kind);
		tokens = filterTokensByAttributes(tokens, requiredAttributes, optionalAttributes);
		pushHandle(L, new LuacomTokensCollection{std::move(tokens)}, kTokensCollectionMeta);
		return 1;
	}

	int tokenCategoryGetDefaultTokenId(lua_State *L)
	{
		const auto *category =
		    checkHandle<LuacomTokenCategory>(L, kTokenCategoryMeta, "Token category expected");
		const auto tokens = collectTokenSet(category->runtime, category->kind);
		if (tokens.isEmpty() || !tokens.constFirst())
			return 0;
		const QByteArray id = tokens.constFirst()->id.toUtf8();
		lua_pushlstring(L, id.constData(), id.size());
		return 1;
	}

	int tokenCategoryIndex(lua_State *L)
	{
		const auto *category =
		    checkHandle<LuacomTokenCategory>(L, kTokenCategoryMeta, "Token category expected");
		const char *key = luaL_checkstring(L, 2);
		if (strcmp(key, "Id") == 0 || strcmp(key, "ID") == 0)
		{
			const QByteArray id = category->categoryId.toUtf8();
			lua_pushlstring(L, id.constData(), id.size());
			return 1;
		}
		luaL_getmetatable(L, kTokenCategoryMeta);
		lua_pushvalue(L, 2);
		lua_rawget(L, -2);
		lua_remove(L, -2);
		if (lua_isnil(L, -1))
		{
			lua_pop(L, 1);
			return 0;
		}
		return 1;
	}

	int tokenCategoryNewIndex(lua_State *L)
	{
		auto *category  = checkHandle<LuacomTokenCategory>(L, kTokenCategoryMeta, "Token category expected");
		const char *key = luaL_checkstring(L, 2);
		if (strcmp(key, "Id") == 0 || strcmp(key, "ID") == 0)
		{
			const char *id       = luaL_checkstring(L, 3);
			category->categoryId = QString::fromUtf8(id ? id : "").trimmed();
			category->kind       = tokenKindFromCategoryId(category->categoryId);
			return 0;
		}
		return luaL_error(L, "Unsupported token category property assignment: %s", key);
	}

	int lexiconAddPronunciation(lua_State *L)
	{
		auto       *lexicon = checkHandle<LuacomLexicon>(L, kLexiconMeta, "Lexicon expected");
		const char *word    = luaL_checkstring(L, 2);
		const int   langId  = static_cast<int>(luaL_checkinteger(L, 3));
		const int   pos     = static_cast<int>(luaL_checkinteger(L, 4));
		const char *pron    = luaL_checkstring(L, 5);
		if (!lexicon->state)
			lexicon->state = std::make_shared<LuacomLexiconState>();
		lexicon->state->pronunciations.insert(lexiconKey(QString::fromUtf8(word ? word : ""), langId, pos),
		                                      QString::fromUtf8(pron ? pron : ""));
		return 0;
	}

	QString parsePhoneIdsArgument(lua_State *L, const int argIndex)
	{
		if (lua_isnoneornil(L, argIndex))
			return {};
		if (lua_istable(L, argIndex))
		{
			const int         top = lua_gettop(L);
			QStringList       values;

			const lua_Integer arrayLength = std::max<lua_Integer>(0, luaL_len(L, argIndex));
			for (lua_Integer i = 1; i <= arrayLength; ++i)
			{
				lua_rawgeti(L, argIndex, i);
				if (lua_isnumber(L, -1))
					values.push_back(QString::number(static_cast<int>(lua_tointeger(L, -1))));
				else if (lua_isstring(L, -1))
					values.push_back(QString::fromUtf8(lua_tostring(L, -1)));
				lua_pop(L, 1);
			}

			// Also collect non-array entries and sparse numeric keys deterministically.
			QVector<QPair<lua_Integer, QString>> numericEntries;
			QVector<QPair<QString, QString>>     stringEntries;
			lua_pushnil(L);
			while (lua_next(L, argIndex) != 0)
			{
				std::optional<QString> parsedValue;
				if (lua_isnumber(L, -1))
					parsedValue = QString::number(static_cast<int>(lua_tointeger(L, -1)));
				else if (lua_isstring(L, -1))
					parsedValue = QString::fromUtf8(lua_tostring(L, -1));
				if (parsedValue.has_value())
				{
					if (lua_isinteger(L, -2))
					{
						const lua_Integer key = lua_tointeger(L, -2);
						if (key < 1 || key > arrayLength)
							numericEntries.push_back({key, *parsedValue});
					}
					else if (lua_isstring(L, -2))
					{
						stringEntries.push_back({QString::fromUtf8(lua_tostring(L, -2)), *parsedValue});
					}
				}
				lua_pop(L, 1);
			}
			std::ranges::sort(numericEntries,
			                  [](const QPair<lua_Integer, QString> &lhs,
			                     const QPair<lua_Integer, QString> &rhs) { return lhs.first < rhs.first; });
			std::ranges::sort(stringEntries,
			                  [](const QPair<QString, QString> &lhs, const QPair<QString, QString> &rhs)
			                  { return lhs.first < rhs.first; });
			for (const auto &[key, value] : numericEntries)
			{
				Q_UNUSED(key)
				values.push_back(value);
			}
			for (const auto &[key, value] : stringEntries)
			{
				Q_UNUSED(key)
				values.push_back(value);
			}
			lua_settop(L, top);
			return canonicalPhoneIds(values.join(' '));
		}
		if (lua_isnumber(L, argIndex))
			return canonicalPhoneIds(QString::number(static_cast<int>(lua_tointeger(L, argIndex))));
		const char *text = luaL_checkstring(L, argIndex);
		return canonicalPhoneIds(QString::fromUtf8(text ? text : ""));
	}

	int lexiconAddPronunciationByPhoneIds(lua_State *L)
	{
		auto         *lexicon  = checkHandle<LuacomLexicon>(L, kLexiconMeta, "Lexicon expected");
		const char   *word     = luaL_checkstring(L, 2);
		const int     langId   = static_cast<int>(luaL_checkinteger(L, 3));
		const int     pos      = static_cast<int>(luaL_checkinteger(L, 4));
		const QString phoneIds = parsePhoneIdsArgument(L, 5);
		if (!lexicon->state)
			lexicon->state = std::make_shared<LuacomLexiconState>();
		lexicon->state->phoneIdPronunciations.insert(
		    lexiconKey(QString::fromUtf8(word ? word : ""), langId, pos), phoneIds);
		return 0;
	}

	int lexiconRemovePronunciation(lua_State *L)
	{
		auto       *lexicon = checkHandle<LuacomLexicon>(L, kLexiconMeta, "Lexicon expected");
		const char *word    = luaL_checkstring(L, 2);
		const int   langId  = static_cast<int>(luaL_checkinteger(L, 3));
		const int   pos     = static_cast<int>(luaL_checkinteger(L, 4));
		luaL_optstring(L, 5, nullptr);
		if (!lexicon->state)
			return 0;
		lexicon->state->pronunciations.remove(lexiconKey(QString::fromUtf8(word ? word : ""), langId, pos));
		return 0;
	}

	int lexiconRemovePronunciationByPhoneIds(lua_State *L)
	{
		auto         *lexicon  = checkHandle<LuacomLexicon>(L, kLexiconMeta, "Lexicon expected");
		const char   *word     = luaL_checkstring(L, 2);
		const int     langId   = static_cast<int>(luaL_checkinteger(L, 3));
		const int     pos      = static_cast<int>(luaL_checkinteger(L, 4));
		const QString phoneIds = parsePhoneIdsArgument(L, 5);
		if (!lexicon->state)
			return 0;
		const QString key = lexiconKey(QString::fromUtf8(word ? word : ""), langId, pos);
		if (phoneIds.isEmpty())
		{
			lexicon->state->phoneIdPronunciations.remove(key);
			return 0;
		}
		if (lexicon->state->phoneIdPronunciations.value(key) == phoneIds)
			lexicon->state->phoneIdPronunciations.remove(key);
		return 0;
	}

	int lexiconGetPronunciations(lua_State *L)
	{
		const auto *lexicon = checkHandle<LuacomLexicon>(L, kLexiconMeta, "Lexicon expected");
		const char *word    = luaL_checkstring(L, 2);
		const int   langId  = static_cast<int>(luaL_checkinteger(L, 3));
		const int   pos     = static_cast<int>(luaL_optinteger(L, 4, 0));
		lua_newtable(L);
		if (!lexicon->state)
			return 1;
		const QString key   = lexiconKey(QString::fromUtf8(word ? word : ""), langId, pos);
		const QString value = lexicon->state->pronunciations.value(key);
		if (!value.isEmpty())
		{
			const QByteArray bytes = value.toUtf8();
			lua_pushlstring(L, bytes.constData(), bytes.size());
			lua_rawseti(L, -2, 1);
		}
		const QString phoneIds = lexicon->state->phoneIdPronunciations.value(key);
		if (!phoneIds.isEmpty())
		{
			const QByteArray bytes = phoneIds.toUtf8();
			lua_pushlstring(L, bytes.constData(), bytes.size());
			const lua_Unsigned currentLen = lua_rawlen(L, -2);
			const lua_Unsigned nextIndex =
			    currentLen >= std::numeric_limits<lua_Unsigned>::max() ? currentLen : currentLen + 1;
			const lua_Integer safeIndex =
			    nextIndex > static_cast<lua_Unsigned>(std::numeric_limits<lua_Integer>::max())
			        ? std::numeric_limits<lua_Integer>::max()
			        : static_cast<lua_Integer>(nextIndex);
			lua_rawseti(L, -2, safeIndex);
		}
		return 1;
	}

	int lexiconIndex(lua_State *L)
	{
		luaL_getmetatable(L, kLexiconMeta);
		lua_pushvalue(L, 2);
		lua_rawget(L, -2);
		lua_remove(L, -2);
		if (lua_isnil(L, -1))
		{
			lua_pop(L, 1);
			return 0;
		}
		return 1;
	}

	QIODevice::OpenMode fileStreamOpenModeFromSapi(const int mode)
	{
		switch (mode)
		{
		case kSsfmOpenForRead:
			return QIODevice::ReadOnly;
		case kSsfmOpenReadWrite:
			return QIODevice::ReadWrite;
		case kSsfmCreate:
			return QIODevice::ReadWrite | QIODevice::Truncate;
		case kSsfmCreateForWrite:
			[[fallthrough]];
		default:
			return QIODevice::WriteOnly | QIODevice::Truncate;
		}
	}

	int fileStreamOpen(lua_State *L)
	{
		auto       *stream = checkHandle<LuacomFileStream>(L, kFileStreamMeta, "File stream expected");
		const char *path   = luaL_checkstring(L, 2);
		const int   mode   = static_cast<int>(luaL_optinteger(L, 3, 3));
		lua_toboolean(L, 4);
		if (!stream->state)
			stream->state = std::make_shared<LuacomFileStreamState>();
		bool opened = false;
		{
			QMutexLocker locker(&stream->state->mutex);
			if (stream->state->file.isOpen())
				stream->state->file.close();
			stream->state->path = QString::fromUtf8(path ? path : "");
			stream->state->file.setFileName(stream->state->path);
			opened                     = stream->state->file.open(fileStreamOpenModeFromSapi(mode));
			stream->state->writeFailed = false;
			stream->state->writeError.clear();
		}
		if (!opened)
			return luaL_error(L, "Unable to open file stream path: %s", path ? path : "");
		return 0;
	}

	int fileStreamClose(lua_State *L)
	{
		auto *stream = checkHandle<LuacomFileStream>(L, kFileStreamMeta, "File stream expected");
		if (stream->state)
		{
			QMutexLocker locker(&stream->state->mutex);
			if (stream->state->file.isOpen())
				stream->state->file.close();
			stream->state->writeFailed = false;
			stream->state->writeError.clear();
		}
		return 0;
	}

	int fileStreamRead(lua_State *L)
	{
		auto     *stream = checkHandle<LuacomFileStream>(L, kFileStreamMeta, "File stream expected");
		const int size   = static_cast<int>(luaL_optinteger(L, 2, -1));
		if (!stream->state)
			return 0;
		QByteArray data;
		{
			QMutexLocker locker(&stream->state->mutex);
			if (!stream->state->file.isOpen())
				return 0;
			if (size < 0)
				data = stream->state->file.readAll();
			else
				data = stream->state->file.read(size);
		}
		lua_pushlstring(L, data.constData(), data.size());
		return 1;
	}

	int fileStreamWrite(lua_State *L)
	{
		auto       *stream = checkHandle<LuacomFileStream>(L, kFileStreamMeta, "File stream expected");
		size_t      length = 0;
		const char *text   = luaL_checklstring(L, 2, &length);
		if (!stream->state)
		{
			lua_pushinteger(L, 0);
			return 1;
		}
		qint64 written = -1;
		{
			QMutexLocker locker(&stream->state->mutex);
			if (stream->state->file.isOpen())
				written = stream->state->file.write(text ? text : "", static_cast<qint64>(length));
		}
		lua_pushinteger(L, written < 0 ? 0 : static_cast<lua_Integer>(written));
		return 1;
	}

	int fileStreamSeek(lua_State *L)
	{
		auto             *stream = checkHandle<LuacomFileStream>(L, kFileStreamMeta, "File stream expected");
		const lua_Integer position = luaL_checkinteger(L, 2);
		if (!stream->state)
		{
			lua_pushboolean(L, 0);
			return 1;
		}
		bool seekOk = false;
		{
			QMutexLocker locker(&stream->state->mutex);
			if (stream->state->file.isOpen())
				seekOk = stream->state->file.seek(std::max<qint64>(0, position));
		}
		lua_pushboolean(L, seekOk ? 1 : 0);
		return 1;
	}

	int fileStreamIndex(lua_State *L)
	{
		auto       *stream = checkHandle<LuacomFileStream>(L, kFileStreamMeta, "File stream expected");
		const char *key    = luaL_checkstring(L, 2);
		if (strcmp(key, "Format") == 0)
		{
			if (!stream->state)
				stream->state = std::make_shared<LuacomFileStreamState>();
			pushHandle(L, new LuacomAudioFormat{stream->state->format}, kAudioFormatMeta);
			return 1;
		}
		if (strcmp(key, "Path") == 0)
		{
			QByteArray path;
			if (stream->state)
			{
				QMutexLocker locker(&stream->state->mutex);
				path = stream->state->path.toUtf8();
			}
			lua_pushlstring(L, path.constData(), path.size());
			return 1;
		}
		if (strcmp(key, "IsOpen") == 0)
		{
			bool isOpen = false;
			if (stream->state)
			{
				QMutexLocker locker(&stream->state->mutex);
				isOpen = stream->state->file.isOpen();
			}
			lua_pushboolean(L, isOpen ? 1 : 0);
			return 1;
		}
		luaL_getmetatable(L, kFileStreamMeta);
		lua_pushvalue(L, 2);
		lua_rawget(L, -2);
		lua_remove(L, -2);
		if (lua_isnil(L, -1))
		{
			lua_pop(L, 1);
			return 0;
		}
		return 1;
	}

	int fileStreamNewIndex(lua_State *L)
	{
		auto       *stream = checkHandle<LuacomFileStream>(L, kFileStreamMeta, "File stream expected");
		const char *key    = luaL_checkstring(L, 2);
		if (strcmp(key, "Format") == 0)
		{
			auto **ud = static_cast<LuacomAudioFormat **>(luaL_testudata(L, 3, kAudioFormatMeta));
			if (!ud || !*ud)
				return luaL_argerror(L, 3, "Audio format expected");
			if (!stream->state)
				stream->state = std::make_shared<LuacomFileStreamState>();
			stream->state->format = (*ud)->state;
			return 0;
		}
		if (strcmp(key, "Path") == 0)
		{
			const char *path = luaL_checkstring(L, 3);
			if (!stream->state)
				stream->state = std::make_shared<LuacomFileStreamState>();
			{
				QMutexLocker locker(&stream->state->mutex);
				stream->state->path = QString::fromUtf8(path ? path : "");
			}
			return 0;
		}
		return luaL_error(L, "Unsupported file stream property assignment: %s", key);
	}

	int audioFormatIndex(lua_State *L)
	{
		auto       *format = checkHandle<LuacomAudioFormat>(L, kAudioFormatMeta, "Audio format expected");
		const char *key    = luaL_checkstring(L, 2);
		if (strcmp(key, "Type") == 0)
		{
			lua_pushinteger(L, format->state ? format->state->type : 0);
			return 1;
		}
		if (strcmp(key, "Guid") == 0)
		{
			const QByteArray guid = (format->state ? format->state->guid : QString()).toUtf8();
			lua_pushlstring(L, guid.constData(), guid.size());
			return 1;
		}
		luaL_getmetatable(L, kAudioFormatMeta);
		lua_pushvalue(L, 2);
		lua_rawget(L, -2);
		lua_remove(L, -2);
		if (lua_isnil(L, -1))
		{
			lua_pop(L, 1);
			return 0;
		}
		return 1;
	}

	int audioFormatNewIndex(lua_State *L)
	{
		auto       *format = checkHandle<LuacomAudioFormat>(L, kAudioFormatMeta, "Audio format expected");
		const char *key    = luaL_checkstring(L, 2);
		if (!format->state)
			format->state = std::make_shared<LuacomAudioFormatState>();
		if (strcmp(key, "Type") == 0)
		{
			format->state->type = static_cast<int>(luaL_checkinteger(L, 3));
			return 0;
		}
		if (strcmp(key, "Guid") == 0)
		{
			const char *guid    = luaL_checkstring(L, 3);
			format->state->guid = QString::fromUtf8(guid ? guid : "");
			return 0;
		}
		return luaL_error(L, "Unsupported audio format property assignment: %s", key);
	}

	int voiceStatusIndex(lua_State *L)
	{
		const auto *status = checkHandle<LuacomVoiceStatus>(L, kVoiceStatusMeta, "Status expected");
		const char *key    = luaL_checkstring(L, 2);
		if (strcmp(key, "RunningState") == 0)
		{
			bool isSpeaking = false;
#if QMUD_ENABLE_TEXT_TO_SPEECH
			isSpeaking = status->runtime && (status->runtime->isSpeaking || status->runtime->isPaused);
#endif
			lua_pushinteger(L, isSpeaking ? kSrseIsSpeaking : kSrseDone);
			return 1;
		}
		if (strcmp(key, "CurrentStreamNumber") == 0 || strcmp(key, "LastStreamNumber") == 0)
		{
			lua_pushinteger(L, status->runtime ? status->runtime->lastStreamNumber : 0);
			return 1;
		}
		if (strcmp(key, "InputWordPosition") == 0 || strcmp(key, "InputWordLength") == 0 ||
		    strcmp(key, "InputSentencePosition") == 0 || strcmp(key, "InputSentenceLength") == 0 ||
		    strcmp(key, "LastBookmarkId") == 0 || strcmp(key, "LastResult") == 0 ||
		    strcmp(key, "PhonemeId") == 0 || strcmp(key, "VisemeId") == 0)
		{
			lua_pushinteger(L, 0);
			return 1;
		}
		if (strcmp(key, "LastBookmark") == 0)
		{
			lua_pushstring(L, "");
			return 1;
		}

		luaL_getmetatable(L, kVoiceStatusMeta);
		lua_pushvalue(L, 2);
		lua_rawget(L, -2);
		lua_remove(L, -2);
		if (lua_isnil(L, -1))
		{
			lua_pop(L, 1);
			return 0;
		}
		return 1;
	}

	int voiceGetDescription(lua_State *L)
	{
		const auto *voice = checkHandle<LuacomVoice>(L, kVoiceMeta, "Voice expected");
		luaL_optstring(L, 2, nullptr);
#if QMUD_ENABLE_TEXT_TO_SPEECH
		if (!hasVoiceAt(voice->runtime, voice->index))
			return luaL_error(L, "Voice handle is no longer valid");
		const QByteArray description =
		    buildVoiceDescription(voice->runtime->voices.at(voice->index)).toUtf8();
		lua_pushlstring(L, description.constData(), description.size());
		return 1;
#else
		Q_UNUSED(voice);
		return luaL_error(L, "Text-to-speech support is not enabled");
#endif
	}

	int voiceGetAttribute(lua_State *L)
	{
		const auto *voice = checkHandle<LuacomVoice>(L, kVoiceMeta, "Voice expected");
		const char *name  = luaL_checkstring(L, 2);
#if QMUD_ENABLE_TEXT_TO_SPEECH
		if (!hasVoiceAt(voice->runtime, voice->index))
			return luaL_error(L, "Voice handle is no longer valid");
		const QString attribute =
		    buildVoiceAttribute(voice->runtime->voices.at(voice->index), QString::fromUtf8(name ? name : ""));
		const QByteArray bytes = attribute.toUtf8();
		lua_pushlstring(L, bytes.constData(), bytes.size());
		return 1;
#else
		Q_UNUSED(name);
		return luaL_error(L, "Text-to-speech support is not enabled");
#endif
	}

	int voiceIndex(lua_State *L)
	{
		const auto *voice = checkHandle<LuacomVoice>(L, kVoiceMeta, "Voice expected");
		const char *key   = luaL_checkstring(L, 2);
		if (strcmp(key, "ID") == 0)
		{
			if (voice->runtime && voice->index >= 0 && voice->index < voice->runtime->voiceIds.size())
			{
				lua_pushstring(L, voice->runtime->voiceIds.at(voice->index).toUtf8().constData());
				return 1;
			}
			lua_pushstring(L, "QMUD\\TTS\\UNKNOWN");
			return 1;
		}

		luaL_getmetatable(L, kVoiceMeta);
		lua_pushvalue(L, 2);
		lua_rawget(L, -2);
		lua_remove(L, -2);
		if (lua_isnil(L, -1))
		{
			lua_pop(L, 1);
			return 0;
		}
		return 1;
	}

	int enumeratorNext(lua_State *L)
	{
		auto *enumerator = checkHandle<LuacomEnumerator>(L, kEnumeratorMeta, "Enumerator expected");
		if (enumerator->position < 0)
			return 0;

		if (enumerator->kind == kEnumeratorKindTokens)
		{
			if (enumerator->position >= enumerator->tokens.size())
				return 0;
			pushToken(L, enumerator->tokens.at(enumerator->position));
			++enumerator->position;
			return 1;
		}

		if (!enumerator->runtime || enumerator->position >= enumerator->indexes.size())
			return 0;
		const int index = enumerator->indexes.at(enumerator->position);
		++enumerator->position;
		if (enumerator->kind == kEnumeratorKindVoices)
		{
			pushVoiceByIndex(L, enumerator->runtime, index);
			return 1;
		}
		if (enumerator->kind == kEnumeratorKindOutputs)
		{
			pushAudioOutputByIndex(L, enumerator->runtime, index);
			return 1;
		}
		return 0;
	}

	int typeInfoGetTypeLib(lua_State *L)
	{
		checkHandle<LuacomTypeInfo>(L, kTypeInfoMeta, "TypeInfo expected");
		pushHandle(L, new LuacomTypeLib{}, kTypeLibMeta);
		return 1;
	}

	int typeInfoIndex(lua_State *L)
	{
		luaL_getmetatable(L, kTypeInfoMeta);
		lua_pushvalue(L, 2);
		lua_rawget(L, -2);
		lua_remove(L, -2);
		return 1;
	}

	int typeLibExportEnumerations(lua_State *L)
	{
		checkHandle<LuacomTypeLib>(L, kTypeLibMeta, "TypeLib expected");
		lua_newtable(L);

		lua_newtable(L);
		lua_pushinteger(L, kSvsfDefault);
		lua_setfield(L, -2, "SVSFDefault");
		lua_pushinteger(L, kSvsFlagsAsync);
		lua_setfield(L, -2, "SVSFlagsAsync");
		lua_pushinteger(L, kSvsfPurgeBeforeSpeak);
		lua_setfield(L, -2, "SVSFPurgeBeforeSpeak");
		lua_pushinteger(L, kSvsfIsFilename);
		lua_setfield(L, -2, "SVSFIsFilename");
		lua_pushinteger(L, kSvsfIsXml);
		lua_setfield(L, -2, "SVSFIsXML");
		lua_pushinteger(L, kSvsfIsNotXml);
		lua_setfield(L, -2, "SVSFIsNotXML");
		lua_pushinteger(L, kSvsfPersistXml);
		lua_setfield(L, -2, "SVSFPersistXML");
		lua_pushinteger(L, kSvsfNlpSpeakPunc);
		lua_setfield(L, -2, "SVSFNLPSpeakPunc");
		lua_pushinteger(L, kSvsfParseSapi);
		lua_setfield(L, -2, "SVSFParseSapi");
		lua_pushinteger(L, kSvsfParseSsml);
		lua_setfield(L, -2, "SVSFParseSsml");
		lua_pushinteger(L, kSvsfParseAutodetect);
		lua_setfield(L, -2, "SVSFParseAutodetect");
		lua_pushinteger(L, kSvsfNlpMask);
		lua_setfield(L, -2, "SVSFNLPMask");
		lua_pushinteger(L, kSvsfParseMask);
		lua_setfield(L, -2, "SVSFParseMask");
		lua_pushinteger(L, kSvsfVoiceMask);
		lua_setfield(L, -2, "SVSFVoiceMask");
		lua_pushinteger(L, kSvsfUnusedFlags);
		lua_setfield(L, -2, "SVSFUnusedFlags");
		lua_setfield(L, -2, "SpeechVoiceSpeakFlags");

		lua_newtable(L);
		lua_pushinteger(L, kSrseDone);
		lua_setfield(L, -2, "SRSEDone");
		lua_pushinteger(L, kSrseIsSpeaking);
		lua_setfield(L, -2, "SRSEIsSpeaking");
		lua_setfield(L, -2, "SpeechRunState");

		lua_newtable(L);
		lua_pushinteger(L, kSvpNormal);
		lua_setfield(L, -2, "SVPNormal");
		lua_pushinteger(L, kSvpAlert);
		lua_setfield(L, -2, "SVPAlert");
		lua_pushinteger(L, kSvpOver);
		lua_setfield(L, -2, "SVPOver");
		lua_setfield(L, -2, "SpeechVoicePriority");

		lua_newtable(L);
		lua_pushinteger(L, kSveStartInputStream);
		lua_setfield(L, -2, "SVEStartInputStream");
		lua_pushinteger(L, kSveEndInputStream);
		lua_setfield(L, -2, "SVEEndInputStream");
		lua_pushinteger(L, kSveVoiceChange);
		lua_setfield(L, -2, "SVEVoiceChange");
		lua_pushinteger(L, kSveBookmark);
		lua_setfield(L, -2, "SVEBookmark");
		lua_pushinteger(L, kSveWordBoundary);
		lua_setfield(L, -2, "SVEWordBoundary");
		lua_pushinteger(L, kSvePhoneme);
		lua_setfield(L, -2, "SVEPhoneme");
		lua_pushinteger(L, kSveSentenceBoundary);
		lua_setfield(L, -2, "SVESentenceBoundary");
		lua_pushinteger(L, kSveViseme);
		lua_setfield(L, -2, "SVEViseme");
		lua_pushinteger(L, kSveAudioLevel);
		lua_setfield(L, -2, "SVEAudioLevel");
		lua_pushinteger(L, kSvePrivate);
		lua_setfield(L, -2, "SVEPrivate");
		lua_pushinteger(L, kSveAllEvents);
		lua_setfield(L, -2, "SVEAllEvents");
		lua_setfield(L, -2, "SpeechVoiceEvents");

		lua_newtable(L);
		lua_pushinteger(L, kSsfmOpenForRead);
		lua_setfield(L, -2, "SSFMOpenForRead");
		lua_pushinteger(L, kSsfmOpenReadWrite);
		lua_setfield(L, -2, "SSFMOpenReadWrite");
		lua_pushinteger(L, kSsfmCreate);
		lua_setfield(L, -2, "SSFMCreate");
		lua_pushinteger(L, kSsfmCreateForWrite);
		lua_setfield(L, -2, "SSFMCreateForWrite");
		lua_setfield(L, -2, "SpeechStreamFileMode");

		lua_newtable(L);
		lua_pushinteger(L, kSltApp);
		lua_setfield(L, -2, "SLTApp");
		lua_pushinteger(L, kSltUser);
		lua_setfield(L, -2, "SLTUser");
		lua_setfield(L, -2, "SpeechLexiconType");

		lua_newtable(L);
		lua_pushinteger(L, kSpsUnknown);
		lua_setfield(L, -2, "SPSUnknown");
		lua_pushinteger(L, kSpsNoun);
		lua_setfield(L, -2, "SPSNoun");
		lua_pushinteger(L, kSpsVerb);
		lua_setfield(L, -2, "SPSVerb");
		lua_pushinteger(L, kSpsModifier);
		lua_setfield(L, -2, "SPSModifier");
		lua_pushinteger(L, kSpsFunction);
		lua_setfield(L, -2, "SPSFunction");
		lua_pushinteger(L, kSpsInterjection);
		lua_setfield(L, -2, "SPSInterjection");
		lua_setfield(L, -2, "SpeechPartOfSpeech");

		lua_newtable(L);
		lua_pushinteger(L, kSaftDefault);
		lua_setfield(L, -2, "SAFTDefault");
		lua_pushinteger(L, kSaftNoAssignedFormat);
		lua_setfield(L, -2, "SAFTNoAssignedFormat");
		lua_pushinteger(L, kSaft8kHz16BitMono);
		lua_setfield(L, -2, "SAFT8kHz16BitMono");
		lua_pushinteger(L, kSaft11kHz16BitMono);
		lua_setfield(L, -2, "SAFT11kHz16BitMono");
		lua_pushinteger(L, kSaft16kHz16BitMono);
		lua_setfield(L, -2, "SAFT16kHz16BitMono");
		lua_pushinteger(L, kSaft22kHz16BitMono);
		lua_setfield(L, -2, "SAFT22kHz16BitMono");
		lua_pushinteger(L, kSaft44kHz16BitMono);
		lua_setfield(L, -2, "SAFT44kHz16BitMono");
		lua_setfield(L, -2, "SpeechAudioFormatType");

		return 1;
	}

	int typeLibIndex(lua_State *L)
	{
		luaL_getmetatable(L, kTypeLibMeta);
		lua_pushvalue(L, 2);
		lua_rawget(L, -2);
		lua_remove(L, -2);
		return 1;
	}

	int luacomCreateObject(lua_State *L)
	{
		const QString programId = QString::fromUtf8(luaL_checkstring(L, 1)).trimmed();
		luaL_optstring(L, 2, nullptr);

		if (programId.compare(QStringLiteral("SAPI.SpVoice"), Qt::CaseInsensitive) == 0)
		{
			auto runtime = makeRuntime();
#if QMUD_ENABLE_TEXT_TO_SPEECH
			if (!runtime || !runtime->hasSpeech())
#else
			if (!runtime)
#endif
				return 0;
			pushHandle(L, new LuacomSpVoice{runtime, {}}, kSpVoiceMeta);
			return 1;
		}

		if (programId.compare(QStringLiteral("SAPI.SpObjectTokenCategory"), Qt::CaseInsensitive) == 0)
		{
			auto  runtime = makeRuntime();
			auto *obj =
			    new LuacomTokenCategory{runtime, tokenCategoryIdForKind(kTokenKindVoices), kTokenKindVoices};
			pushHandle(L, obj, kTokenCategoryMeta);
			return 1;
		}

		if (programId.compare(QStringLiteral("SAPI.SpObjectToken"), Qt::CaseInsensitive) == 0)
		{
			auto token = std::make_shared<LuacomTokenData>();
			pushHandle(L, new LuacomToken{token}, kTokenMeta);
			return 1;
		}

		if (programId.compare(QStringLiteral("SAPI.SpLexicon"), Qt::CaseInsensitive) == 0)
		{
			pushHandle(L, new LuacomLexicon{std::make_shared<LuacomLexiconState>()}, kLexiconMeta);
			return 1;
		}

		if (programId.compare(QStringLiteral("SAPI.SpFileStream"), Qt::CaseInsensitive) == 0)
		{
			pushHandle(L, new LuacomFileStream{std::make_shared<LuacomFileStreamState>()}, kFileStreamMeta);
			return 1;
		}

		if (programId.compare(QStringLiteral("SAPI.SpAudioFormat"), Qt::CaseInsensitive) == 0)
		{
			pushHandle(L, new LuacomAudioFormat{std::make_shared<LuacomAudioFormatState>()},
			           kAudioFormatMeta);
			return 1;
		}

		return 0;
	}

	int luacomGetTypeInfo(lua_State *L)
	{
		const bool isKnown =
		    luaL_testudata(L, 1, kSpVoiceMeta) || luaL_testudata(L, 1, kVoiceMeta) ||
		    luaL_testudata(L, 1, kVoicesCollectionMeta) || luaL_testudata(L, 1, kAudioOutputMeta) ||
		    luaL_testudata(L, 1, kAudioOutputsCollectionMeta) || luaL_testudata(L, 1, kVoiceStatusMeta) ||
		    luaL_testudata(L, 1, kTokenMeta) || luaL_testudata(L, 1, kTokensCollectionMeta) ||
		    luaL_testudata(L, 1, kTokenCategoryMeta) || luaL_testudata(L, 1, kLexiconMeta) ||
		    luaL_testudata(L, 1, kFileStreamMeta) || luaL_testudata(L, 1, kAudioFormatMeta);
		if (!isKnown)
			return luaL_argerror(L, 1, "Luacom object expected");
		pushHandle(L, new LuacomTypeInfo{}, kTypeInfoMeta);
		return 1;
	}

	int luacomGetEnumerator(lua_State *L)
	{
		QVector<int>                              indexes;
		QVector<std::shared_ptr<LuacomTokenData>> tokens;
		std::shared_ptr<TtsVoiceRuntime>          runtime;
		int                                       kind = kEnumeratorKindVoices;

		auto **voices = static_cast<LuacomVoicesCollection **>(luaL_testudata(L, 1, kVoicesCollectionMeta));
		if (voices && *voices)
		{
			runtime = (*voices)->runtime;
			indexes = (*voices)->indexes;
		}
		else
		{
			auto **outputs = static_cast<LuacomAudioOutputsCollection **>(
			    luaL_testudata(L, 1, kAudioOutputsCollectionMeta));
			if (outputs && *outputs)
			{
				runtime = (*outputs)->runtime;
				kind    = kEnumeratorKindOutputs;
				indexes = (*outputs)->indexes;
			}
			else
			{
				auto **tokenCollection =
				    static_cast<LuacomTokensCollection **>(luaL_testudata(L, 1, kTokensCollectionMeta));
				if (!tokenCollection || !*tokenCollection)
					return luaL_argerror(L, 1, "Collection expected");
				kind   = kEnumeratorKindTokens;
				tokens = (*tokenCollection)->tokens;
			}
		}

		pushHandle(L, new LuacomEnumerator{runtime, std::move(indexes), std::move(tokens), 0, kind},
		           kEnumeratorMeta);
		return 1;
	}

	void registerMetatables(lua_State *L)
	{
		luaL_newmetatable(L, kSpVoiceMeta);
		if (lua_istable(L, -1))
		{
			lua_pushcfunction(L, spVoiceGc);
			lua_setfield(L, -2, "__gc");
			lua_pushcfunction(L, spVoiceIndex);
			lua_setfield(L, -2, "__index");
			lua_pushcfunction(L, spVoiceNewIndex);
			lua_setfield(L, -2, "__newindex");
			lua_pushcfunction(L, spVoiceSpeak);
			lua_setfield(L, -2, "Speak");
			lua_pushcfunction(L, spVoiceSkip);
			lua_setfield(L, -2, "Skip");
			lua_pushcfunction(L, spVoiceGetVoices);
			lua_setfield(L, -2, "GetVoices");
			lua_pushcfunction(L, spVoiceGetAudioOutputs);
			lua_setfield(L, -2, "GetAudioOutputs");
			lua_pushcfunction(L, spVoiceSetVoice);
			lua_setfield(L, -2, "setVoice");
			lua_pushcfunction(L, spVoiceSetAudioOutput);
			lua_setfield(L, -2, "setAudioOutput");
			lua_pushcfunction(L, spVoiceSetAudioOutputStream);
			lua_setfield(L, -2, "setAudioOutputStream");
			lua_pushcfunction(L, spVoicePause);
			lua_setfield(L, -2, "Pause");
			lua_pushcfunction(L, spVoiceResume);
			lua_setfield(L, -2, "Resume");
			lua_pushcfunction(L, spVoiceWaitUntilDone);
			lua_setfield(L, -2, "WaitUntilDone");
			lua_pushcfunction(L, spVoiceIsUiSupported);
			lua_setfield(L, -2, "IsUISupported");
			lua_pushcfunction(L, spVoiceDisplayUi);
			lua_setfield(L, -2, "DisplayUI");
		}
		lua_pop(L, 1);

		luaL_newmetatable(L, kVoicesCollectionMeta);
		if (lua_istable(L, -1))
		{
			lua_pushcfunction(L, voicesCollectionGc);
			lua_setfield(L, -2, "__gc");
			lua_pushcfunction(L, voicesCollectionIndex);
			lua_setfield(L, -2, "__index");
			lua_pushcfunction(L, voicesCollectionItem);
			lua_setfield(L, -2, "Item");
		}
		lua_pop(L, 1);

		luaL_newmetatable(L, kVoiceMeta);
		if (lua_istable(L, -1))
		{
			lua_pushcfunction(L, voiceGc);
			lua_setfield(L, -2, "__gc");
			lua_pushcfunction(L, voiceIndex);
			lua_setfield(L, -2, "__index");
			lua_pushcfunction(L, voiceGetDescription);
			lua_setfield(L, -2, "GetDescription");
			lua_pushcfunction(L, voiceGetAttribute);
			lua_setfield(L, -2, "GetAttribute");
		}
		lua_pop(L, 1);

		luaL_newmetatable(L, kAudioOutputsCollectionMeta);
		if (lua_istable(L, -1))
		{
			lua_pushcfunction(L, audioOutputsCollectionGc);
			lua_setfield(L, -2, "__gc");
			lua_pushcfunction(L, audioOutputsCollectionIndex);
			lua_setfield(L, -2, "__index");
			lua_pushcfunction(L, audioOutputsCollectionItem);
			lua_setfield(L, -2, "Item");
		}
		lua_pop(L, 1);

		luaL_newmetatable(L, kAudioOutputMeta);
		if (lua_istable(L, -1))
		{
			lua_pushcfunction(L, audioOutputGc);
			lua_setfield(L, -2, "__gc");
			lua_pushcfunction(L, audioOutputIndex);
			lua_setfield(L, -2, "__index");
			lua_pushcfunction(L, audioOutputGetDescription);
			lua_setfield(L, -2, "GetDescription");
		}
		lua_pop(L, 1);

		luaL_newmetatable(L, kVoiceStatusMeta);
		if (lua_istable(L, -1))
		{
			lua_pushcfunction(L, voiceStatusGc);
			lua_setfield(L, -2, "__gc");
			lua_pushcfunction(L, voiceStatusIndex);
			lua_setfield(L, -2, "__index");
		}
		lua_pop(L, 1);

		luaL_newmetatable(L, kTokenMeta);
		if (lua_istable(L, -1))
		{
			lua_pushcfunction(L, tokenGc);
			lua_setfield(L, -2, "__gc");
			lua_pushcfunction(L, tokenIndex);
			lua_setfield(L, -2, "__index");
			lua_pushcfunction(L, tokenNewIndex);
			lua_setfield(L, -2, "__newindex");
			lua_pushcfunction(L, tokenSetId);
			lua_setfield(L, -2, "SetId");
			lua_pushcfunction(L, tokenGetDescription);
			lua_setfield(L, -2, "GetDescription");
			lua_pushcfunction(L, tokenGetAttribute);
			lua_setfield(L, -2, "GetAttribute");
		}
		lua_pop(L, 1);

		luaL_newmetatable(L, kTokensCollectionMeta);
		if (lua_istable(L, -1))
		{
			lua_pushcfunction(L, tokensCollectionGc);
			lua_setfield(L, -2, "__gc");
			lua_pushcfunction(L, tokensCollectionIndex);
			lua_setfield(L, -2, "__index");
			lua_pushcfunction(L, tokensCollectionItem);
			lua_setfield(L, -2, "Item");
		}
		lua_pop(L, 1);

		luaL_newmetatable(L, kTokenCategoryMeta);
		if (lua_istable(L, -1))
		{
			lua_pushcfunction(L, tokenCategoryGc);
			lua_setfield(L, -2, "__gc");
			lua_pushcfunction(L, tokenCategoryIndex);
			lua_setfield(L, -2, "__index");
			lua_pushcfunction(L, tokenCategoryNewIndex);
			lua_setfield(L, -2, "__newindex");
			lua_pushcfunction(L, tokenCategorySetId);
			lua_setfield(L, -2, "SetId");
			lua_pushcfunction(L, tokenCategoryEnumerateTokens);
			lua_setfield(L, -2, "EnumerateTokens");
			lua_pushcfunction(L, tokenCategoryGetDefaultTokenId);
			lua_setfield(L, -2, "GetDefaultTokenId");
		}
		lua_pop(L, 1);

		luaL_newmetatable(L, kLexiconMeta);
		if (lua_istable(L, -1))
		{
			lua_pushcfunction(L, lexiconGc);
			lua_setfield(L, -2, "__gc");
			lua_pushcfunction(L, lexiconIndex);
			lua_setfield(L, -2, "__index");
			lua_pushcfunction(L, lexiconAddPronunciation);
			lua_setfield(L, -2, "AddPronunciation");
			lua_pushcfunction(L, lexiconRemovePronunciation);
			lua_setfield(L, -2, "RemovePronunciation");
			lua_pushcfunction(L, lexiconAddPronunciationByPhoneIds);
			lua_setfield(L, -2, "AddPronunciationByPhoneIds");
			lua_pushcfunction(L, lexiconRemovePronunciationByPhoneIds);
			lua_setfield(L, -2, "RemovePronunciationByPhoneIds");
			lua_pushcfunction(L, lexiconGetPronunciations);
			lua_setfield(L, -2, "GetPronunciations");
		}
		lua_pop(L, 1);

		luaL_newmetatable(L, kFileStreamMeta);
		if (lua_istable(L, -1))
		{
			lua_pushcfunction(L, fileStreamGc);
			lua_setfield(L, -2, "__gc");
			lua_pushcfunction(L, fileStreamIndex);
			lua_setfield(L, -2, "__index");
			lua_pushcfunction(L, fileStreamNewIndex);
			lua_setfield(L, -2, "__newindex");
			lua_pushcfunction(L, fileStreamOpen);
			lua_setfield(L, -2, "Open");
			lua_pushcfunction(L, fileStreamClose);
			lua_setfield(L, -2, "Close");
			lua_pushcfunction(L, fileStreamRead);
			lua_setfield(L, -2, "Read");
			lua_pushcfunction(L, fileStreamWrite);
			lua_setfield(L, -2, "Write");
			lua_pushcfunction(L, fileStreamSeek);
			lua_setfield(L, -2, "Seek");
		}
		lua_pop(L, 1);

		luaL_newmetatable(L, kAudioFormatMeta);
		if (lua_istable(L, -1))
		{
			lua_pushcfunction(L, audioFormatGc);
			lua_setfield(L, -2, "__gc");
			lua_pushcfunction(L, audioFormatIndex);
			lua_setfield(L, -2, "__index");
			lua_pushcfunction(L, audioFormatNewIndex);
			lua_setfield(L, -2, "__newindex");
		}
		lua_pop(L, 1);

		luaL_newmetatable(L, kEnumeratorMeta);
		if (lua_istable(L, -1))
		{
			lua_pushcfunction(L, enumeratorGc);
			lua_setfield(L, -2, "__gc");
			lua_pushcfunction(L, enumeratorNext);
			lua_setfield(L, -2, "Next");
			lua_pushvalue(L, -1);
			lua_setfield(L, -2, "__index");
		}
		lua_pop(L, 1);

		luaL_newmetatable(L, kTypeInfoMeta);
		if (lua_istable(L, -1))
		{
			lua_pushcfunction(L, typeInfoGc);
			lua_setfield(L, -2, "__gc");
			lua_pushcfunction(L, typeInfoIndex);
			lua_setfield(L, -2, "__index");
			lua_pushcfunction(L, typeInfoGetTypeLib);
			lua_setfield(L, -2, "GetTypeLib");
		}
		lua_pop(L, 1);

		luaL_newmetatable(L, kTypeLibMeta);
		if (lua_istable(L, -1))
		{
			lua_pushcfunction(L, typeLibGc);
			lua_setfield(L, -2, "__gc");
			lua_pushcfunction(L, typeLibIndex);
			lua_setfield(L, -2, "__index");
			lua_pushcfunction(L, typeLibExportEnumerations);
			lua_setfield(L, -2, "ExportEnumerations");
		}
		lua_pop(L, 1);
	}
} // namespace

extern "C" int luaopen_luacom(lua_State *L)
{
	registerMetatables(L);

	lua_newtable(L);
	lua_pushcfunction(L, luacomCreateObject);
	lua_setfield(L, -2, "CreateObject");
	lua_pushcfunction(L, luacomGetTypeInfo);
	lua_setfield(L, -2, "GetTypeInfo");
	lua_pushcfunction(L, luacomGetEnumerator);
	lua_setfield(L, -2, "GetEnumerator");

	lua_pushvalue(L, -1);
	lua_setglobal(L, "luacom");

	return 1;
}

void QMudLuacomShim::registerPreload(lua_State *L)
{
	if (!L)
		return;

	lua_getglobal(L, LUA_LOADLIBNAME);
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		return;
	}

	lua_getfield(L, -1, "preload");
	if (lua_istable(L, -1))
	{
		lua_pushcfunction(L, luaopen_luacom);
		lua_setfield(L, -2, "luacom");
		lua_pushcfunction(L, luaopen_luacom);
		lua_setfield(L, -2, "com");
	}
	lua_pop(L, 2);
}

void QMudLuacomShim::setRuntimeFactoryForTesting(RuntimeFactory factory)
{
	QMutexLocker locker(&gRuntimeFactoryMutex);
	gRuntimeFactory = std::move(factory);
}

void QMudLuacomShim::resetRuntimeFactoryForTesting()
{
	QMutexLocker locker(&gRuntimeFactoryMutex);
	gRuntimeFactory = {};
}
