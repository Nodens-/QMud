/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: TelnetProcessor.cpp
 * Role: Telnet parser/negotiation implementation that turns raw socket bytes into structured runtime events.
 */

// Telnet stream processing for incoming MUD data.
//
// Handles telnet phase/state transitions together with IAC/MCCP decoding.

#include "TelnetProcessor.h"
#include "MxpDiagnostics.h"

#include <QList>
#include <QSet>
#include <QString>
#include <QtGlobal>
#include <zlib.h>

/* Phases for processing input stream ...


  To allow for these to be broken between packets we only do one
  character at a time, and maintain a "state" (Phase) which indicates
  how far through the sequence we are.

  The two "triggering" codes we might get in normal text (i.e. phase NONE) are
  ESC or IAC.

  Tested (5 Feb 2010):

  * MCCP v1
  * MCCP v2
  * IAC GA              \ff\f9
  * IAC EOR             \ff\ef
  * IAC IAC inside subnegotiation   \FF\FD\66 \ff\fa\66hello\ff\ffboys\ff\f0
  * IAC IAC in normal text      what\ff\ffever
  * MXP
  * Charset: \FF\FD\2A \ff\fa\2A\01,UTF-8,US-ASCII\ff\f0
  * terminal type: \FF\FD\18 \ff\fa\18\01\ff\f0
  * NAWS: \FF\FD\1f \ff\fa\1f\ff\f0
  * Chat system


*/

// Telnet command codes (see RFC 854 and related RFCs)
static constexpr unsigned char IAC  = 0xFF;
static constexpr unsigned char DONT = 0xFE;
static constexpr unsigned char DO   = 0xFD;
static constexpr unsigned char WONT = 0xFC;
static constexpr unsigned char WILL = 0xFB;
static constexpr unsigned char SB   = 0xFA;
static constexpr unsigned char GA   = 0xF9; // GO AHEAD
static constexpr unsigned char EOR  = 0xEF; // End of Record
static constexpr unsigned char SE   = 0xF0;

// MCCP (Mud Client Compression Protocol) stuff
// NB 85 is MCCP v1, 86 is MCCP v2
// see http://www.randomly.org/projects/MCCP/protocol.html
static constexpr unsigned char TELOPT_COMPRESS      = 85; // MCCP v1
static constexpr unsigned char TELOPT_COMPRESS2     = 86; // MCCP v2
static constexpr unsigned char TELOPT_ECHO          = 1;
static constexpr unsigned char TELOPT_NAWS          = 31;
static constexpr unsigned char TELOPT_CHARSET       = 42;
static constexpr unsigned char TELOPT_TERMINAL_TYPE = 24;
static constexpr unsigned char TELOPT_MUD_SPECIFIC  = 102;
static constexpr unsigned char TELOPT_MXP           = 91;
static constexpr unsigned char SGA                  = 3; // suppress go-ahead
static constexpr unsigned char WILL_END_OF_RECORD   = 25;

static constexpr int           eMXP_open        = 0;
static constexpr int           eMXP_secure      = 1;
static constexpr int           eMXP_locked      = 2;
static constexpr int           eMXP_reset       = 3;
static constexpr int           eMXP_secure_once = 4;
static constexpr int           eMXP_perm_open   = 5;
static constexpr int           eMXP_perm_secure = 6;
static constexpr int           eMXP_perm_locked = 7;

static constexpr int           eOnCommandMXP = 0;
static constexpr int           eQueryMXP     = 1;
static constexpr int           eUseMXP       = 2;
static constexpr int           eNoMXP        = 3;

static constexpr unsigned char CHARSET_REQUEST         = 1;
static constexpr unsigned char CHARSET_ACCEPTED        = 2;
static constexpr unsigned char CHARSET_REJECTED        = 3;
static constexpr unsigned char TTYPE_IS                = 0;
static constexpr unsigned char TTYPE_SEND              = 1;
static constexpr int           MCCP_INFLATE_CHUNK_SIZE = 8192;
static constexpr int           kMaxMxpPendingBytes     = 8192;
static constexpr int           kMaxMxpEventsPending    = 4096;
static constexpr int           kMaxMxpCustomDefinitions = 1024;
static constexpr int           kMaxMxpAttlistBytes     = 16384;
static constexpr int           DBG_ERROR               = 1;
static constexpr int           DBG_WARNING             = 2;

namespace
{
	bool isAsciiDigit(const unsigned char c)
	{
		return c >= '0' && c <= '9';
	}

	bool isAsciiAlpha(const unsigned char c)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
	}

	bool isAsciiAlnum(const unsigned char c)
	{
		return isAsciiAlpha(c) || isAsciiDigit(c);
	}

	bool isAsciiHexDigit(const unsigned char c)
	{
		return isAsciiDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
	}

	int asciiHexValue(const unsigned char c)
	{
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if (c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return -1;
	}

	bool isAsciiSpace(const unsigned char c)
	{
		switch (c)
		{
		case ' ':
		case '\t':
		case '\n':
		case '\r':
		case '\f':
		case '\v':
			return true;
		default:
			return false;
		}
	}

	struct LegacyEntityPair
	{
			const char *name;
			const char *value;
	};

	QByteArray decodeLegacyEntityValue(const QByteArray &raw)
	{
		if (raw.size() >= 4 && raw.startsWith("&#") && raw.endsWith(';'))
		{
			bool ok = false;
			if (const int numeric = raw.mid(2, raw.size() - 3).toInt(&ok);
			    ok && numeric >= 0 && numeric <= 255)
			{
				const char c = static_cast<char>(numeric);
				return QByteArray{&c, 1};
			}
		}
		return raw;
	}

	const QHash<QByteArray, QByteArray> &builtinEntityMap()
	{
		static const QHash<QByteArray, QByteArray> kMap = []
		{
			static const LegacyEntityPair kPairs[] = {
			    {"lt",     "<"     },
                {"micro",  "&#181;"},
                {"Icirc",  "&#206;"},
                {"ccedil", "&#231;"},
			    {"gt",     ">"     },
                {"para",   "&#182;"},
                {"Iuml",   "&#207;"},
                {"egrave", "&#232;"},
			    {"amp",    "&"     },
                {"middot", "&#183;"},
                {"ETH",    "&#208;"},
                {"eacute", "&#233;"},
			    {"quot",   "\""    },
                {"cedil",  "&#184;"},
                {"Ntilde", "&#209;"},
                {"ecirc",  "&#234;"},
			    {"nbsp",   "&#160;"},
                {"sup1",   "&#185;"},
                {"Ograve", "&#210;"},
                {"euml",   "&#235;"},
			    {"iexcl",  "&#161;"},
                {"ordm",   "&#186;"},
                {"Oacute", "&#211;"},
                {"igrave", "&#236;"},
			    {"cent",   "&#162;"},
                {"raquo",  "&#187;"},
                {"Ocirc",  "&#212;"},
                {"iacute", "&#237;"},
			    {"pound",  "&#163;"},
                {"frac14", "&#188;"},
                {"Otilde", "&#213;"},
                {"icirc",  "&#238;"},
			    {"curren", "&#164;"},
                {"frac12", "&#189;"},
                {"Ouml",   "&#214;"},
                {"iuml",   "&#239;"},
			    {"yen",    "&#165;"},
                {"frac34", "&#190;"},
                {"times",  "&#215;"},
                {"eth",    "&#240;"},
			    {"brvbar", "&#166;"},
                {"iquest", "&#191;"},
                {"Oslash", "&#216;"},
                {"ntilde", "&#241;"},
			    {"sect",   "&#167;"},
                {"Agrave", "&#192;"},
                {"Ugrave", "&#217;"},
                {"ograve", "&#242;"},
			    {"uml",    "&#168;"},
                {"Aacute", "&#193;"},
                {"Uacute", "&#218;"},
                {"oacute", "&#243;"},
			    {"copy",   "&#169;"},
                {"Acirc",  "&#194;"},
                {"Ucirc",  "&#219;"},
                {"ocirc",  "&#244;"},
			    {"ordf",   "&#170;"},
                {"Atilde", "&#195;"},
                {"Uuml",   "&#220;"},
                {"otilde", "&#245;"},
			    {"laquo",  "&#171;"},
                {"Auml",   "&#196;"},
                {"Yacute", "&#221;"},
                {"ouml",   "&#246;"},
			    {"not",    "&#172;"},
                {"Aring",  "&#197;"},
                {"THORN",  "&#222;"},
                {"divide", "&#247;"},
			    {"shy",    "&#173;"},
                {"AElig",  "&#198;"},
                {"szlig",  "&#223;"},
                {"oslash", "&#248;"},
			    {"reg",    "&#174;"},
                {"Ccedil", "&#199;"},
                {"agrave", "&#224;"},
                {"ugrave", "&#249;"},
			    {"macr",   "&#175;"},
                {"Egrave", "&#200;"},
                {"aacute", "&#225;"},
                {"uacute", "&#250;"},
			    {"deg",    "&#176;"},
                {"Eacute", "&#201;"},
                {"acirc",  "&#226;"},
                {"ucirc",  "&#251;"},
			    {"plusmn", "&#177;"},
                {"Ecirc",  "&#202;"},
                {"atilde", "&#227;"},
                {"uuml",   "&#252;"},
			    {"sup2",   "&#178;"},
                {"Euml",   "&#203;"},
                {"auml",   "&#228;"},
                {"yacute", "&#253;"},
			    {"sup3",   "&#179;"},
                {"Igrave", "&#204;"},
                {"aring",  "&#229;"},
                {"thorn",  "&#254;"},
			    {"acute",  "&#180;"},
                {"Iacute", "&#205;"},
                {"aelig",  "&#230;"},
                {"yuml",   "&#255;"},
			    {"apos",   "'"     },
                {nullptr,  nullptr }
            };

			QHash<QByteArray, QByteArray> map;
			for (int i = 0; kPairs[i].name; ++i)
				map.insert(QByteArray(kPairs[i].name), decodeLegacyEntityValue(QByteArray(kPairs[i].value)));
			return map;
		}();
		return kMap;
	}

	bool resolveBuiltinEntity(const QByteArray &name, QByteArray &value)
	{
		const auto &map = builtinEntityMap();
		const auto  it  = map.constFind(name);
		if (it == map.constEnd())
			return false;
		value = it.value();
		return true;
	}

	bool isBuiltinEntityDefinitionName(const QByteArray &lowerName)
	{
		static const QSet<QByteArray> kLowerNames = []
		{
			QSet<QByteArray> names;
			const auto      &map = builtinEntityMap();
			for (auto it = map.constBegin(); it != map.constEnd(); ++it)
				names.insert(it.key().toLower());
			return names;
		}();
		return kLowerNames.contains(lowerName);
	}

	bool isBuiltinElementName(const QByteArray &lowerName)
	{
		static const QSet<QByteArray> kNames = {"bold",      "b",
		                                        "high",      "h",
		                                        "underline", "u",
		                                        "italic",    "i",
		                                        "em",        "color",
		                                        "c",         "s",
		                                        "strike",    "strong",
		                                        "small",     "tt",
		                                        "font",      "frame",
		                                        "dest",      "image",
		                                        "filter",    "a",
		                                        "h1",        "h2",
		                                        "h3",        "h4",
		                                        "h5",        "h6",
		                                        "hr",        "nobr",
		                                        "p",         "script",
		                                        "send",      "ul",
		                                        "ol",        "samp",
		                                        "center",    "var",
		                                        "v",         "gauge",
		                                        "stat",      "expire",
		                                        "li",        "sound",
		                                        "music",     "br",
		                                        "username",  "user",
		                                        "password",  "pass",
		                                        "relocate",  "version",
		                                        "reset",     "mxp",
		                                        "support",   "option",
		                                        "afk",       "recommend_option",
		                                        "pre",       "body",
		                                        "head",      "html",
		                                        "title",     "img",
		                                        "xch_page",  "xch_pane"};
		return kNames.contains(lowerName);
	}
} // namespace

struct TelnetProcessor::ZStreamWrapper
{
		z_stream stream{};
};

TelnetProcessor::TelnetProcessor()
{
	m_zlib = new ZStreamWrapper;
}

void TelnetProcessor::setCallbacks(const Callbacks &callbacks)
{
	m_callbacks = callbacks;
}

void TelnetProcessor::setConvertGAtoNewline(const bool enabled)
{
	m_convertGAtoNewline = enabled;
}

void TelnetProcessor::setUseUtf8(const bool enabled)
{
	m_utf8 = enabled;
}

void TelnetProcessor::setNoEchoOff(const bool enabled)
{
	m_noEchoOff = enabled;
}

void TelnetProcessor::setUseMxp(const int mode)
{
	if (mode == m_useMxp)
	{
		if (mode == eUseMXP && !m_mxpEnabled)
			mxpOn(false, false);
		return;
	}
	m_useMxp = mode;
	if (m_useMxp == eNoMXP)
		mxpOff(true);
	else if (m_useMxp == eUseMXP && !m_mxpEnabled)
		mxpOn(false, false);
}

void TelnetProcessor::setNawsEnabled(const bool enabled)
{
	m_naws = enabled;
}

void TelnetProcessor::setWindowSize(const int columns, const int rows)
{
	m_wrapColumns = columns;
	m_wrapRows    = rows;
}

void TelnetProcessor::setTerminalIdentification(const QString &value)
{
	if (!value.isEmpty())
		m_terminalIdentification = value;
}

void TelnetProcessor::setDisableCompression(const bool enabled)
{
	m_disableCompression = enabled;
}

void TelnetProcessor::resetMxp()
{
	mxpOff(false);
}

void TelnetProcessor::resetConnectionState()
{
	const bool hadNoEcho = m_noEcho;
	m_phase              = NONE;
	m_code               = 0;
	m_subnegotiationType = 0;
	m_subnegotiationData.clear();
	m_outbound.clear();
	m_ansiBuffer.clear();
	m_outputSize = 0;
	m_mxpPhase   = MXP_NONE;
	m_mxpString.clear();
	m_mxpEvents.clear();
	m_mxpModeChanges.clear();
	m_mxpEventsOverflowed = false;
	m_mxpEventSequence    = 0;
	m_compress            = false;
	m_mccpType            = 0;
	m_compressInitOk      = false;
	m_supportsMccp2       = false;
	m_seenCompressWillIac = false;
	m_pendingCompressed.clear();
	m_compressInput.clear();
	m_postCompressionRemainder.clear();
	m_compressInputOffset = 0;
	m_requestedSga        = false;
	m_requestedEor        = false;
	m_noEcho              = false;
	if (hadNoEcho && m_callbacks.onNoEchoChanged)
		m_callbacks.onNoEchoChanged(false);
	if (m_zlib)
		inflateReset(&m_zlib->stream);
}

void TelnetProcessor::queueInitialNegotiation(const bool requestSga, const bool requestEor)
{
	if (requestSga && !m_requestedSga)
	{
		sendIacDo(SGA);
		m_requestedSga = true;
	}
	if (requestEor && !m_requestedEor)
	{
		sendIacDo(WILL_END_OF_RECORD);
		m_requestedEor = true;
	}
}

QByteArray TelnetProcessor::processBytes(const QByteArray &data)
{
	if (data.isEmpty())
		return {};

	m_outputSize = 0;

	QByteArray output;
	QByteArray tailPlain;
	auto       takePostCompressionRemainder = [&]
	{
		if (m_postCompressionRemainder.isEmpty())
			return;
		tailPlain.append(m_postCompressionRemainder);
		m_postCompressionRemainder.clear();
	};

	if (m_compress)
	{
		m_totalCompressedBytes += data.size();
		if (const QByteArray decompressed = inflateIfNeeded(data); !decompressed.isEmpty())
		{
			m_totalUncompressedBytes += decompressed.size();
			output.append(processPlainBytes(decompressed));
		}
		takePostCompressionRemainder();
	}
	else
	{
		output = processPlainBytes(data);
	}

	if (m_compress && !m_pendingCompressed.isEmpty())
	{
		m_totalCompressedBytes += m_pendingCompressed.size();
		const QByteArray more = inflateIfNeeded(m_pendingCompressed);
		m_pendingCompressed.clear();
		if (!more.isEmpty())
		{
			m_totalUncompressedBytes += more.size();
			output.append(processPlainBytes(more));
		}
		takePostCompressionRemainder();
	}
	if (!tailPlain.isEmpty())
		output.append(processPlainBytes(tailPlain));

	return output;
}

QByteArray TelnetProcessor::takeOutboundData()
{
	QByteArray out = m_outbound;
	m_outbound.clear();
	return out;
}

QList<TelnetProcessor::MxpEvent> TelnetProcessor::takeMxpEvents()
{
	QList<MxpEvent> events = m_mxpEvents;
	m_mxpEvents.clear();
	m_mxpEventsOverflowed = false;
	return events;
}

QList<TelnetProcessor::MxpModeChange> TelnetProcessor::takeMxpModeChanges()
{
	QList<MxpModeChange> changes = m_mxpModeChanges;
	m_mxpModeChanges.clear();
	return changes;
}

bool TelnetProcessor::getCustomEntityValue(const QByteArray &name, QByteArray &value) const
{
	const QByteArray key = name.toLower();
	if (!m_customEntities.contains(key))
		return false;
	value = m_customEntities.value(key);
	return true;
}

void TelnetProcessor::setCustomEntity(const QByteArray &name, const QByteArray &value)
{
	const QByteArray key = name.toLower();
	if (key.isEmpty())
		return;
	if (value.isEmpty())
		m_customEntities.remove(key);
	else
	{
		if (!m_customEntities.contains(key) && m_customEntities.size() >= kMaxMxpCustomDefinitions)
		{
			emitMxpDiagnosticLazy(
			    DBG_WARNING, wrnMXP_CustomDefinitionLimitExceeded,
			    [key]
			    {
				    return QStringLiteral("MXP custom definition limit reached (%1). Ignoring entity &%2;.")
				        .arg(kMaxMxpCustomDefinitions)
				        .arg(QString::fromLocal8Bit(key));
			    });
			return;
		}
		m_customEntities.insert(key, value);
	}
}

bool TelnetProcessor::resolveEntityValue(const QByteArray &name, QByteArray &value) const
{
	value = mxpGetEntity(name);
	return !value.isEmpty();
}

bool TelnetProcessor::getCustomElementInfo(const QByteArray &name, CustomElementInfo &info) const
{
	const QByteArray key = name.toLower();
	const auto       it  = m_customElements.constFind(key);
	if (it == m_customElements.cend())
		return false;
	const auto &[elementName, elementOpen, elementCommand, elementTag, elementFlag, elementDefinition,
	             elementAttributes] = it.value();
	info.name                       = elementName;
	info.open                       = elementOpen;
	info.command                    = elementCommand;
	info.tag                        = elementTag;
	info.flag                       = elementFlag;
	info.definition                 = elementDefinition;
	info.attributes                 = elementAttributes;
	return true;
}

bool TelnetProcessor::isMxpSecure() const
{
	return mxpSecure();
}

bool TelnetProcessor::isMxpOpen() const
{
	return mxpOpen();
}

bool TelnetProcessor::isMxpEnabled() const
{
	return m_mxpEnabled;
}

void TelnetProcessor::activatePuebloMode()
{
	mxpOn(true, false);
}

int TelnetProcessor::mccpType() const
{
	return m_mccpType;
}

qint64 TelnetProcessor::totalCompressedBytes() const
{
	return m_totalCompressedBytes;
}

qint64 TelnetProcessor::totalUncompressedBytes() const
{
	return m_totalUncompressedBytes;
}

qint64 TelnetProcessor::mxpTagCount() const
{
	return m_mxpTagCount;
}

qint64 TelnetProcessor::mxpEntityCount() const
{
	return m_mxpEntityCount;
}

int TelnetProcessor::customElementCount() const
{
	return static_cast<int>(m_customElements.size());
}

int TelnetProcessor::customEntityCount() const
{
	return static_cast<int>(m_customEntities.size());
}

int TelnetProcessor::builtinEntityCount()
{
	return static_cast<int>(builtinEntityMap().size());
}

bool TelnetProcessor::isCompressing() const
{
	return m_compress;
}

bool TelnetProcessor::isPuebloActive() const
{
	return m_puebloActive;
}

void TelnetProcessor::disableMxp()
{
	mxpOff(true);
}

void TelnetProcessor::setMxpDefaultMode(const MxpDefaultMode mode)
{
	switch (mode)
	{
	case MxpDefaultMode::Open:
		m_mxpDefaultMode = eMXP_open;
		break;
	case MxpDefaultMode::Secure:
		m_mxpDefaultMode = eMXP_secure;
		break;
	case MxpDefaultMode::Locked:
		m_mxpDefaultMode = eMXP_locked;
		break;
	}
}

QByteArray TelnetProcessor::inflateIfNeeded(const QByteArray &data)
{
	if (!m_compress)
		return data;

	// MCCP inflate path using a reusable chunk buffer and Z_SYNC_FLUSH until the
	// current input segment is drained. Reuse avoids per-iteration allocation
	// churn in high-volume compressed output.

	if (!m_compressInitOk)
		return {};

	if (!data.isEmpty())
		m_compressInput.append(data);

	if (m_compressInput.isEmpty())
		return {};

	if (m_compressInputOffset > 0 && m_compressInputOffset >= m_compressInput.size())
	{
		m_compressInput.clear();
		m_compressInputOffset = 0;
		return {};
	}

	if (m_compressOutputChunk.size() != MCCP_INFLATE_CHUNK_SIZE)
		m_compressOutputChunk.resize(MCCP_INFLATE_CHUNK_SIZE);

	QByteArray   output;
	bool         streamEnded   = false;
	const Bytef *remainderPtr  = nullptr;
	int          remainderSize = 0;

	const int    initialAvailIn =
	    static_cast<int>(m_compressInput.size() - static_cast<qsizetype>(m_compressInputOffset));
	m_zlib->stream.next_in  = reinterpret_cast<Bytef *>(m_compressInput.data() + m_compressInputOffset);
	m_zlib->stream.avail_in = static_cast<uInt>(initialAvailIn);

	output.reserve(qMax(initialAvailIn * 2, MCCP_INFLATE_CHUNK_SIZE));

	while (true)
	{
		m_zlib->stream.next_out  = reinterpret_cast<Bytef *>(m_compressOutputChunk.data());
		m_zlib->stream.avail_out = static_cast<uInt>(m_compressOutputChunk.size());

		const uInt beforeIn  = m_zlib->stream.avail_in;
		const uInt beforeOut = m_zlib->stream.avail_out;
		const int  result    = inflate(&m_zlib->stream, Z_SYNC_FLUSH);

		const auto produced =
		    static_cast<int>(m_compressOutputChunk.size() - static_cast<qsizetype>(m_zlib->stream.avail_out));

		if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR)
		{
			// Legacy behavior: decompression failure is fatal for the current session.
			QString message;
			if (m_zlib->stream.msg && m_zlib->stream.msg[0] != '\0')
			{
				message = QStringLiteral("Could not decompress text from MUD: %1")
				              .arg(QString::fromLatin1(m_zlib->stream.msg));
			}
			else
			{
				message = QStringLiteral("Could not decompress text from MUD: %1").arg(result);
			}
			if (m_callbacks.onFatalProtocolError)
				m_callbacks.onFatalProtocolError(message);
			m_compress = false;
			m_mccpType = 0;
			m_compressInput.clear();
			m_compressInputOffset = 0;
			m_pendingCompressed.clear();
			m_postCompressionRemainder.clear();
			break;
		}

		if (produced > 0)
			output.append(m_compressOutputChunk.constData(), produced);

		if (result == Z_BUF_ERROR &&
		    (m_zlib->stream.avail_in == 0 ||
		     (m_zlib->stream.avail_in == beforeIn && m_zlib->stream.avail_out == beforeOut)))
		{
			// no progress with available input
			break;
		}

		if (m_zlib->stream.avail_in == 0 && produced == 0)
			break;

		if (result == Z_STREAM_END)
		{
			streamEnded   = true;
			remainderPtr  = m_zlib->stream.next_in;
			remainderSize = static_cast<int>(m_zlib->stream.avail_in);
			break;
		}
	}

	if (streamEnded)
	{
		if (remainderSize > 0 && remainderPtr)
			m_postCompressionRemainder =
			    QByteArray(reinterpret_cast<const char *>(remainderPtr), remainderSize);
		else
			m_postCompressionRemainder.clear();
		m_compress = false;
		m_mccpType = 0;
		m_compressInput.clear();
		m_compressInputOffset = 0;
		return output;
	}

	const int consumed = initialAvailIn - static_cast<int>(m_zlib->stream.avail_in);
	m_compressInputOffset += consumed;

	if (m_compressInputOffset > 0)
	{
		if (m_compressInputOffset >= m_compressInput.size())
		{
			m_compressInput.clear();
			m_compressInputOffset = 0;
		}
		else if (m_compressInputOffset > 8192)
		{
			m_compressInput.remove(0, m_compressInputOffset);
			m_compressInputOffset = 0;
		}
	}

	return output;
}

// ESC x
//
// ESC sequences gate ANSI/MXP token collection before line processing.
// Telnet phases are handled here; ANSI/MXP rendering is handled later.

QByteArray TelnetProcessor::processPlainBytes(const QByteArray &data)
{
	QByteArray output;
	output.reserve(data.size());
	int outputSize = m_outputSize;
	auto abortMxpCollectionOnOverflow = [this]
	{
		const char *phaseName = "token";
		switch (m_mxpPhase)
		{
		case HAVE_MXP_ELEMENT:
			phaseName = "element";
			break;
		case HAVE_MXP_COMMENT:
			phaseName = "comment";
			break;
		case HAVE_MXP_QUOTE:
			phaseName = "quoted attribute";
			break;
		case HAVE_MXP_ENTITY:
			phaseName = "entity";
			break;
		case MXP_NONE:
		default:
			break;
		}
		emitMxpDiagnosticLazy(
		    DBG_ERROR, errMXP_CollectionTooLong,
		    [phaseName]
		    {
			    return QStringLiteral("MXP %1 exceeded %2-byte limit; discarding partial token.")
			        .arg(QString::fromLatin1(phaseName))
			        .arg(kMaxMxpPendingBytes);
		    });
		m_mxpPhase = MXP_NONE;
		m_mxpString.clear();
	};
	auto appendMxpPendingByte = [&](const unsigned char byte)
	{
		m_mxpString.append(static_cast<char>(byte));
		if (m_mxpString.size() <= kMaxMxpPendingBytes)
			return true;
		abortMxpCollectionOnOverflow();
		return false;
	};

	for (int i = 0; i < data.size(); ++i)
	{
		const auto c = static_cast<unsigned char>(data.at(i));

		// mxp phases
		// MXP phase-state transition handling.
		if (m_mxpPhase != MXP_NONE)
		{
			switch (m_mxpPhase)
			{
				// end of element
			case HAVE_MXP_ELEMENT:
				switch (c)
				{
				case '>':
					// here when element collection complete
					// here at end of element collection
					// we look for opening tag, closing tag or element/entity definition
					mxpCollectedElement();
					m_mxpPhase = MXP_NONE;
					m_mxpString.clear();
					break;
				case '<':
					// shouldn't have a < inside a <
					mxpUnterminatedElement(R"(Got "<" inside "<")");
					m_mxpString.clear();
					break;
				case '\'':
				case '\"':
					// quote inside element
					m_mxpQuoteTerminator = c;
					m_mxpPhase           = HAVE_MXP_QUOTE;
					if (!appendMxpPendingByte(c))
						continue;
					break;
				case '-':
					// may be a comment? check on a hyphen
					if (!appendMxpPendingByte(c))
						continue;
					if (m_mxpString.left(3) == QByteArray("!--"))
						m_mxpPhase = HAVE_MXP_COMMENT;
					break;
				default:
					// any other character, add to string
					if (!appendMxpPendingByte(c))
						continue;
					break;
				}
				continue;

			case HAVE_MXP_COMMENT:
				if (c == '>' && m_mxpString.right(2) == QByteArray("--"))
				{
					// discard comment, just switch phase back to none
					m_mxpPhase = MXP_NONE;
					m_mxpString.clear();
				}
				else
				{
					// any other character, add to string
					if (!appendMxpPendingByte(c))
						continue;
				}
				continue;

			case HAVE_MXP_QUOTE:
				// closing quote? changes phases back
				if (c == m_mxpQuoteTerminator)
					m_mxpPhase = HAVE_MXP_ELEMENT;

				if (!appendMxpPendingByte(c))
					continue;
				continue;

			case HAVE_MXP_ENTITY:
				switch (c)
				{
				case ';':
					m_mxpPhase = MXP_NONE;
					// here when entity collection complete
					if (QByteArray resolved = mxpCollectedEntity(); !resolved.isEmpty())
					{
						output.append(resolved);
						outputSize += static_cast<int>(resolved.size());
						m_outputSize = outputSize;
					}
					m_mxpString.clear();
					break;
				case '&':
					// shouldn't have a & inside a &
					mxpUnterminatedElement(R"(Got "&" inside "&")");
					m_mxpString.clear();
					break;
				case '<':
					// shouldn't have a < inside a &
					mxpUnterminatedElement(R"(Got "<" inside "&")");
					m_mxpPhase = HAVE_MXP_ELEMENT; // however we are now collecting an element
					m_mxpString.clear();
					break;
				default:
					if (!appendMxpPendingByte(c))
						continue;
					break;
				}
				continue;

			default:
				break;
			}
		}

		switch (m_phase)
		{
		case NONE:
			// Legacy behavior: ignore NUL bytes in normal text stream.
			if (c == 0)
				break;
			if (c == IAC)
			{
				m_phase = HAVE_IAC;
				continue;
			}
			if (c == 0x1B) // ESC
			{
				m_phase = HAVE_ESC;
				m_ansiBuffer.clear();
				m_ansiBuffer.append(static_cast<char>(c));
				continue;
			}

			if (m_mxpEnabled && m_mxpMode == eMXP_secure_once && c != '<')
				mxpRestoreMode();

			if (m_mxpEnabled && m_useMxp != eNoMXP && m_mxpMode != eMXP_locked && m_mxpMode != eMXP_perm_locked &&
			    c == '<') // MXP element start
			{
				m_mxpPhase = HAVE_MXP_ELEMENT;
				m_mxpString.clear();
				continue;
			}
			if (m_mxpEnabled && m_useMxp != eNoMXP && m_mxpMode != eMXP_locked && m_mxpMode != eMXP_perm_locked &&
			    c == '&') // MXP entity start
			{
				m_mxpPhase = HAVE_MXP_ENTITY;
				m_mxpString.clear();
				continue;
			}

			output.append(static_cast<char>(c));
			outputSize += 1;
			m_outputSize = outputSize;
			break;

		case HAVE_ESC:
			// ESC x
			if (c == '[')
			{
				m_phase = DOING_CODE;
				m_code  = 0;
				m_ansiBuffer.append(static_cast<char>(c));
			}
			else
			{
				m_phase = NONE;
				if (!m_ansiBuffer.isEmpty())
				{
					output.append(m_ansiBuffer);
					outputSize += static_cast<int>(m_ansiBuffer.size());
					m_outputSize = outputSize;
					m_ansiBuffer.clear();
				}
				output.append(static_cast<char>(c));
				outputSize += 1;
				m_outputSize = outputSize;
			}
			break;

		case DOING_CODE:
			// ANSI - We have ESC [ x
			m_ansiBuffer.append(static_cast<char>(c));
			if (isAsciiDigit(c))
			{
				m_code *= 10;
				m_code += c - '0';
			}
			else if (c == ';' || c == ':') // separator, eg. ESC[ 38:5:<n>
			{
				m_code = 0;
			}
			if (c >= 0x40 && c <= 0x7E)
			{
				if (c == 'm')
				{
					if (!m_ansiBuffer.isEmpty())
					{
						output.append(m_ansiBuffer);
						outputSize += static_cast<int>(m_ansiBuffer.size());
						m_outputSize = outputSize;
					}
				}
				else if (c == 'z') // MXP line security mode
				{
					if (m_code == eMXP_reset)
						mxpOff(false);
					else
						mxpModeChange(m_code);
				}
				m_phase = NONE;
				m_ansiBuffer.clear();
				m_code = 0;
			}
			break;

		// IAC - we have IAC x
		case HAVE_IAC:
			switch (c)
			{
			case IAC:
				output.append(static_cast<char>(IAC));
				m_phase = NONE;
				break;
			case GA:
			case EOR:
				if (m_callbacks.onIacGa)
					m_callbacks.onIacGa();
				if (m_convertGAtoNewline)
				{
					output.append('\n');
					outputSize += 1;
					m_outputSize = outputSize;
				}
				m_phase = NONE;
				break;
			case SB:
				m_phase              = HAVE_SB;
				m_subnegotiationType = 0;
				m_subnegotiationData.clear();
				break;
			case WILL:
				m_phase = HAVE_WILL;
				break;
			case WONT:
				m_phase = HAVE_WONT;
				break;
			case DO:
				m_phase = HAVE_DO;
				break;
			case DONT:
				m_phase = HAVE_DONT;
				break;
			default:
				m_phase = NONE;
				break;
			}
			break;

		case HAVE_WILL:
			// WILL - we have IAC WILL x   - reply DO or DONT (generally based on client option settings)
			// for unknown types we query plugins: function OnPluginTelnetRequest (num, type)
			//    e.g. num = 200, type = WILL
			// They reply true or false to handle or not handle that telnet type
			//
			// telnet negotiation : in response to WILL, we say DONT
			// (except for compression, MXP, TERMINAL_TYPE and SGA), we *will* handle that)
			switch (c)
			{
			case TELOPT_COMPRESS2:
			case TELOPT_COMPRESS:
				// initialise compression library if not already decompressing
				if (!m_disableCompression)
				{
					if (!(c == TELOPT_COMPRESS && m_supportsMccp2)) // don't agree to MCCP1 and MCCP2
					{
						sendIacDo(c);
						if (c == TELOPT_COMPRESS2)
							m_supportsMccp2 = true;
					}
					else
					{
						sendIacDont(c);
					}
				}
				else
				{
					sendIacDont(c);
				}
				break; // end of TELOPT_COMPRESS

			// here for SGA (Suppress GoAhead) and TELOPT_MUD_SPECIFIC
			case SGA:
			case TELOPT_MUD_SPECIFIC:
				sendIacDo(c);
				break;

			case TELOPT_ECHO:
				if (!m_noEchoOff)
				{
					m_noEcho = true;
					if (m_callbacks.onNoEchoChanged)
						m_callbacks.onNoEchoChanged(true);
					sendIacDo(c);
				}
				else
				{
					sendIacDont(c);
				}
				break; // end of TELOPT_ECHO

			case TELOPT_MXP:
				if (m_useMxp == eNoMXP)
				{
					sendIacDont(c);
				} // end of no MXP wanted
				else
				{
					sendIacDo(c);
					if (m_useMxp == eQueryMXP)
						mxpOn(false, false);
				} // end of MXP wanted
				break; // end of MXP

			// here for EOR (End of record)
			case WILL_END_OF_RECORD:
				if (m_convertGAtoNewline)
					sendIacDo(c);
				else
					sendIacDont(c);
				break; // end of WILL_END_OF_RECORD

			// character set negotiations
			case TELOPT_CHARSET:
				sendIacDo(c);
				break;

			default:
				if (m_callbacks.onTelnetRequest && m_callbacks.onTelnetRequest(c, QStringLiteral("WILL")))
				{
					sendIacDo(c);
					if (m_callbacks.onTelnetRequest)
						m_callbacks.onTelnetRequest(c, QStringLiteral("SENT_DO"));
				}
				else
				{
					sendIacDont(c);
				}
				break; // end of others
			} // end of switch
			m_phase = NONE;
			break;

		case HAVE_WONT:
			// Received: IAC WONT x
			//
			// telnet negotiation : in response to WONT, we say DONT
			if (c == TELOPT_ECHO && !m_noEchoOff)
			{
				m_noEcho = false;
				if (m_callbacks.onNoEchoChanged)
					m_callbacks.onNoEchoChanged(false);
			}
			sendIacDont(c);
			m_phase = NONE;
			break;

		case HAVE_DO:
			// Received: IAC DO x
			//
			// for unknown types we query plugins: function OnPluginTelnetRequest (num, type)
			//    e.g. num = 200, type = DO
			// They reply true or false to handle or not handle that telnet type
			//
			// telnet negotiation : in response to DO, we say WILL for:
			//  <102> (Aardwolf), SGA, echo, NAWS, CHARSET, MXP and Terminal type
			// for others we query plugins to see if they want to handle it or not
			switch (c)
			{
			case SGA:
			case TELOPT_MUD_SPECIFIC:
			case TELOPT_ECHO:
			case TELOPT_CHARSET:
				sendIacWill(c);
				break; // end of things we will do

			// for MTTS start back at sequence 0
			case TELOPT_TERMINAL_TYPE:
				m_ttypeSequence = 0;
				sendIacWill(c);
				break;

			case TELOPT_NAWS:
				// option off - must be server initiated
				if (m_naws)
				{
					sendIacWill(c);
					m_nawsWanted = true;
					sendWindowSize();
				}
				else
				{
					sendIacWont(c);
				}
				break;

			case TELOPT_MXP:
				if (m_useMxp == eNoMXP)
				{
					sendIacWont(c);
				}
				else
				{
					sendIacWill(c);
					if (m_useMxp == eQueryMXP)
						mxpOn(false, false);
				} // end of MXP wanted
				break; // end of MXP

			default:
				if (m_callbacks.onTelnetRequest && m_callbacks.onTelnetRequest(c, QStringLiteral("DO")))
				{
					sendIacWill(c);
					if (m_callbacks.onTelnetRequest)
						m_callbacks.onTelnetRequest(c, QStringLiteral("SENT_WILL"));
				}
				else
				{
					sendIacWont(c);
				}
				break; // end of others
			} // end of switch
			m_phase = NONE;
			break;

		case HAVE_DONT:
			// Received: IAC DONT x
			//
			// telnet negotiation : in response to DONT, we say WONT
			sendIacWont(c);
			switch (c)
			{
			case TELOPT_MXP:
				mxpOff(true);
				break; // end of MXP

			// for MTTS start back at sequence 0
			case TELOPT_TERMINAL_TYPE:
				m_ttypeSequence = 0;
				break;
			default:
				break;
			} // end of switch
			m_phase = NONE;
			break;

		case HAVE_SB:
			// begin subnegotiation; first byte is the option type
			m_subnegotiationType = c;
			if (c == TELOPT_COMPRESS)
				m_phase = HAVE_COMPRESS;
			else
				m_phase = HAVE_SUBNEGOTIATION;
			break;

		case HAVE_SUBNEGOTIATION:
			if (c == IAC)
				m_phase = HAVE_SUBNEGOTIATION_IAC;
			else
				m_subnegotiationData.append(static_cast<char>(c));
			break;

		case HAVE_SUBNEGOTIATION_IAC:
			if (c == IAC)
			{
				m_subnegotiationData.append(static_cast<char>(IAC));
				m_phase = HAVE_SUBNEGOTIATION;
				break;
			}

			// treat any non-IAC byte as end-of-subnegotiation (align with expected behavior)
			if (m_subnegotiationType == TELOPT_COMPRESS2)
			{
				QString compressionError;
				if (startCompression(2, &compressionError))
				{
					if (const int nextIndex = i + 1; nextIndex < data.size())
						m_pendingCompressed = data.mid(nextIndex);
					m_subnegotiationData.clear();
					m_phase = NONE;
					return output;
				}
				if (m_callbacks.onFatalProtocolError)
					m_callbacks.onFatalProtocolError(compressionError);
			}
			else if (m_subnegotiationType == TELOPT_MXP)
			{
				// turn MXP on, if required on subnegotiation
				if (m_useMxp == eOnCommandMXP)
					mxpOn(false, false);
			}
			else if (m_subnegotiationType == TELOPT_CHARSET)
			{
				// IAC SB CHARSET REQUEST DELIMITER <name> DELIMITER
				/*

			For legacy interoperability:

			Server sends:  IAC DO CHARSET
			Client sends:  IAC WILL CHARSET

			  or:

			See: https://tools.ietf.org/html/rfc2066

			Server sends:  IAC WILL CHARSET
			Client sends:  IAC DO CHARSET

			Server sends:  IAC SB CHARSET REQUEST DELIM NAME IAC SE
			Client sends:  IAC SB CHARSET ACCEPTED NAME IAC SE
			or
			Client sends:  IAC SB CHARSET REJECTED IAC SE

			where:

			  CHARSET: 0x2A
			  REQUEST: 0x01
			  ACCEPTED:0x02
			  REJECTED:0x03
			  DELIM:   some character that does not appear in the charset name, other than IAC, e.g. comma, space
			  NAME:    the character string "UTF-8" (or some other name like "S-JIS")

			*/
				if (m_subnegotiationData.size() >= 3)
				{
					if (const auto request = static_cast<unsigned char>(m_subnegotiationData.at(0));
					    request == CHARSET_REQUEST)
					{
						const char              delim   = m_subnegotiationData.at(1);
						const QByteArray        names   = m_subnegotiationData.mid(2);
						const QList<QByteArray> options = names.split(delim);
						QByteArray desired = m_utf8 ? QByteArray("UTF-8") : QByteArray("US-ASCII");
						bool       found   = false;
						for (const QByteArray &opt : options)
						{
							if (opt.trimmed().toUpper() == desired)
							{
								found = true;
								break;
							}
						}
						if (found)
							sendCharsetAccepted(desired);
						else
							sendCharsetRejected();
					}
				}
			}
			else if (m_subnegotiationType == TELOPT_TERMINAL_TYPE)
			{
				// terminal type request
				if (const int tt = m_subnegotiationData.isEmpty() ? -1 : m_subnegotiationData.at(0);
				    tt == TTYPE_SEND)
				{
					sendTerminalType();
				}
			}
			else if (m_subnegotiationType == TELOPT_MUD_SPECIFIC)
			{
				// stuff for Aardwolf (telopt 102) - call specific plugin handler: OnPluginTelnetOption
				if (m_callbacks.onTelnetOption)
					m_callbacks.onTelnetOption(m_subnegotiationData);
				if (m_callbacks.onTelnetSubnegotiation)
					m_callbacks.onTelnetSubnegotiation(m_subnegotiationType, m_subnegotiationData);
			}
			else if (m_callbacks.onTelnetSubnegotiation)
			{
				m_callbacks.onTelnetSubnegotiation(m_subnegotiationType, m_subnegotiationData);
			}
			m_phase = NONE;
			break;

		// COMPRESSION - we have IAC SB COMPRESS x
		case HAVE_COMPRESS:
			if (c == WILL) // should get COMPRESS WILL
			{
				m_phase               = HAVE_COMPRESS_WILL;
				m_seenCompressWillIac = false;
			}
			else
			{
				m_phase = NONE; // error
			}
			break;

		// COMPRESSION - we have IAC SB COMPRESS IAC/WILL x   (MCCP v1)
		//
		// we will return one of:
		//  0 - error in starting compression - close world and display strMessage
		//  1 - got IAC or unexpected input, do nothing
		//  2 - compression OK - prepare for it
		case HAVE_COMPRESS_WILL:
			if (c == IAC)
			{
				m_seenCompressWillIac = true;
			}
			else if (c == SE && m_seenCompressWillIac)
			{
				QString compressionError;
				if (startCompression(1, &compressionError))
				{
					if (const int nextIndex = i + 1; nextIndex < data.size())
						m_pendingCompressed = data.mid(nextIndex);
					m_phase = NONE;
					return output;
				}
				if (m_callbacks.onFatalProtocolError)
					m_callbacks.onFatalProtocolError(compressionError);
				m_phase = NONE;
			}
			else
			{
				m_phase = NONE;
			}
			break;

		default:
			m_phase = NONE;
			break;
		}
	}

	return output;
}

bool TelnetProcessor::startCompression(const int type, QString *errorMessage)
{
	m_mccpType = type;
	if (errorMessage)
		errorMessage->clear();

	// initialize compression library if not already done
	if (!m_compressInitOk)
	{
		m_zlib->stream.zalloc = Z_NULL;
		m_zlib->stream.zfree  = Z_NULL;
		m_zlib->stream.opaque = Z_NULL;
		const int initResult  = inflateInit(&m_zlib->stream);
		m_compressInitOk      = initResult == Z_OK;
		if (!m_compressInitOk && errorMessage)
			*errorMessage = QStringLiteral("Cannot process compressed output. World closed.");
	}

	if (!m_compressInitOk)
		return false;

	if (const int resetResult = inflateReset(&m_zlib->stream); resetResult != Z_OK)
	{
		if (errorMessage)
		{
			if (m_zlib->stream.msg && m_zlib->stream.msg[0] != '\0')
			{
				*errorMessage = QStringLiteral("Could not reset zlib decompression engine: %1")
				                    .arg(QString::fromLatin1(m_zlib->stream.msg));
			}
			else
			{
				*errorMessage =
				    QStringLiteral("Could not reset zlib decompression engine: %1").arg(resetResult);
			}
		}
		return false;
	}

	m_compress = true;
	m_compressInput.clear();
	m_compressInputOffset = 0;
	m_pendingCompressed.clear();
	return true;
}

// send to all plugins: function OnPluginTelnetRequest (num, type)
//    e.g. num = 200, type = DO
// They reply true or false to handle or not handle that telnet type

void TelnetProcessor::sendIacDo(const unsigned char option)
{
	const unsigned char bytes[3] = {IAC, DO, option};
	m_outbound.append(reinterpret_cast<const char *>(bytes), sizeof bytes);
}

void TelnetProcessor::sendIacDont(const unsigned char option)
{
	const unsigned char bytes[3] = {IAC, DONT, option};
	m_outbound.append(reinterpret_cast<const char *>(bytes), sizeof bytes);
}

void TelnetProcessor::sendIacWill(const unsigned char option)
{
	const unsigned char bytes[3] = {IAC, WILL, option};
	m_outbound.append(reinterpret_cast<const char *>(bytes), sizeof bytes);
}

void TelnetProcessor::sendIacWont(const unsigned char option)
{
	const unsigned char bytes[3] = {IAC, WONT, option};
	m_outbound.append(reinterpret_cast<const char *>(bytes), sizeof bytes);
}

void TelnetProcessor::sendCharsetAccepted(const QByteArray &charset)
{
	constexpr unsigned char p1[] = {IAC, SB, TELOPT_CHARSET, CHARSET_ACCEPTED};
	constexpr unsigned char p2[] = {IAC, SE};
	QByteArray              response(reinterpret_cast<const char *>(p1), sizeof p1);
	const QByteArray        trimmed = charset.left(20); // ensure max of 20 so we don't overflow the field
	response.append(trimmed);
	response.append(reinterpret_cast<const char *>(p2), sizeof p2);
	m_outbound.append(response);
}

void TelnetProcessor::sendCharsetRejected()
{
	constexpr unsigned char p[] = {IAC, SB, TELOPT_CHARSET, CHARSET_REJECTED, IAC, SE};
	m_outbound.append(reinterpret_cast<const char *>(p), sizeof p);
}

void TelnetProcessor::sendTerminalType()
{
	// we reply: IAC SB TERMINAL-TYPE IS ... IAC SE
	// see: RFC 930 and RFC 1060
	// also see: http://tintin.sourceforge.net/mtts/
	constexpr unsigned char p1[4] = {IAC, SB, TELOPT_TERMINAL_TYPE, TTYPE_IS};
	constexpr unsigned char p2[2] = {IAC, SE};

	QByteArray              response(reinterpret_cast<const char *>(p1), sizeof p1);

	/*
  On the first TTYPE SEND request the client should return its name, preferably without a version number and in all caps.

  On the second TTYPE SEND request the client should return a terminal type, preferably in all caps.
	Console clients should report the name of the terminal emulator,
	other clients should report one of the four most generic terminal types.

	  "DUMB"              Terminal has no ANSI color or VT100 support.
	  "ANSI"              Terminal supports all ANSI color codes. Supporting blink and underline is optional.
	  "VT100"             Terminal supports most VT100 codes, including ANSI color codes.
	  "XTERM"             Terminal supports all VT100 and ANSI color codes, xterm 256 colors, mouse tracking, and the OSC color palette.

  If 256 color detection for non MTTS compliant servers is a must it's an option
	to report "ANSI-256COLOR", "VT100-256COLOR", or "XTERM-256COLOR".
	The terminal is expected to support VT100, mouse tracking, and the OSC color palette if "XTERM-256COLOR" is reported.

  On the third TTYPE SEND request the client should return MTTS followed by a bitvector. The bit values and their names are defined below.

	      1 "ANSI"              Client supports all ANSI color codes. Supporting blink and underline is optional.
	      2 "VT100"             Client supports most VT100 codes.
	      4 "UTF-8"             Client is using UTF-8 character encoding.
	      8 "256 COLORS"        Client supports all xterm 256 color codes.
	     16 "MOUSE TRACKING"    Client supports xterm mouse tracking.
	     32 "OSC COLOR PALETTE" Client supports the OSC color palette.
	     64 "SCREEN READER"     Client is using a screen reader.
	    128 "PROXY"             Client is a proxy allowing different users to connect from the same IP address.

  */

	QByteArray              strTemp;
	switch (m_ttypeSequence)
	{
	case 0:
		strTemp = m_terminalIdentification.toUtf8().left(20);
		m_ttypeSequence++;
		break;

	case 1:
		strTemp = QByteArray("ANSI");
		m_ttypeSequence++;
		break;

	case 2:
		if (m_utf8)
			strTemp = QByteArray("MTTS 13");
		else
			strTemp = QByteArray("MTTS 9");
		break;
	default:
		break;
	}

	response.append(strTemp);
	response.append(reinterpret_cast<const char *>(p2), sizeof p2);
	m_outbound.append(response);
}

void TelnetProcessor::sendWindowSize()
{
	if (!m_nawsWanted || m_wrapColumns <= 0 || m_wrapRows <= 0)
		return;

	// NAWS subnegotiation: IAC SB NAWS <cols> <rows> IAC SE
	constexpr unsigned char p1[3] = {IAC, SB, TELOPT_NAWS};
	constexpr unsigned char p2[2] = {IAC, SE};

	unsigned char           sizeBytes[4];
	sizeBytes[0] = m_wrapColumns >> 8 & 0xFF;
	sizeBytes[1] = m_wrapColumns & 0xFF;
	sizeBytes[2] = m_wrapRows >> 8 & 0xFF;
	sizeBytes[3] = m_wrapRows & 0xFF;

	QByteArray response(reinterpret_cast<const char *>(p1), sizeof p1);
	// RFC 1073/Telnet requirement: data 0xFF bytes inside subnegotiation payload
	// must be doubled to avoid ambiguity with IAC.
	for (const unsigned char byte : sizeBytes)
	{
		response.append(static_cast<char>(byte));
		if (byte == IAC)
			response.append(static_cast<char>(IAC));
	}
	response.append(reinterpret_cast<const char *>(p2), sizeof p2);
	m_outbound.append(response);
}

// here when element collection complete
//
// here at end of element collection
// we look for opening tag, closing tag or element/entity definition

void TelnetProcessor::mxpCollectedElement()
{
	m_mxpString = trimMxp(m_mxpString);

	const bool wasSecureOnce = m_mxpMode == eMXP_secure_once;

	//  TRACE1 ("MXP collected element: <%s>\n", (LPCTSTR) m_strMXPstring);
	//
	//  MXP_error (DBG_ALL, msgMXP_CollectedElement,
	//              TFormat ("MXP element: <%s>",
	//              (LPCTSTR) m_strMXPstring));

	if (m_mxpString.isEmpty())
	{
		emitMxpDiagnosticLazy(DBG_ERROR, errMXP_EmptyElement,
		                      [] { return QStringLiteral("Empty MXP element supplied."); });
		if (wasSecureOnce)
			mxpRestoreMode();
		return;
	}

	// we have four possibilities here ...

	// <bold>  - normal element
	// </bold> - terminating element
	// <!element blah >  - defining thing

	const char c = m_mxpString.at(0);

	if (!isAsciiAlnum(static_cast<unsigned char>(c)) && m_mxpString.size() < 2)
	{
		emitMxpDiagnosticLazy(
		    DBG_ERROR, errMXP_ElementTooShort,
		    [this]
		    {
			    return QStringLiteral("MXP element too short: <%1>").arg(QString::fromLocal8Bit(m_mxpString));
		    });
		if (wasSecureOnce)
			mxpRestoreMode();
		return;
	}

	auto appendMxpEvent = [this](const MxpEvent &event)
	{
		if (m_mxpEvents.size() >= kMaxMxpEventsPending)
		{
			if (!m_mxpEventsOverflowed)
			{
				m_mxpEventsOverflowed = true;
				emitMxpDiagnosticLazy(
				    DBG_WARNING, wrnMXP_EventQueueLimitExceeded,
				    []
				    {
					    return QStringLiteral(
					        "MXP event queue exceeded %1 entries; dropping further MXP events until drained.")
					        .arg(kMaxMxpEventsPending);
				    });
			}
			return;
		}
		m_mxpEvents.append(event);
	};

	// test first character, that will tell us
	switch (c)
	{
	case '!': // comment or definition
		// MXP_Definition (m_strMXPstring.Mid (1));  // process the definition
		mxpDefinition(m_mxpString.mid(1));
		{
			MxpEvent ev;
			ev.type     = MxpEvent::Definition;
			ev.raw      = m_mxpString;
			ev.offset   = m_outputSize;
			ev.sequence = m_mxpEventSequence++;
			ev.secure   = mxpSecure();
			appendMxpEvent(ev);
		}
		break;
	case '/': // end of tag
		// MXP_EndTag (m_strMXPstring.Mid (1));  // process the tag ending
		{
			const QByteArray                   tagBody = m_mxpString.mid(1);
			const QMap<QByteArray, QByteArray> attrs   = parseTagArguments(tagBody);
			const QByteArray name = attrs.contains("_tag") ? attrs.value("_tag") : QByteArray();
			MxpEvent         ev;
			ev.type       = MxpEvent::EndTag;
			ev.name       = name;
			ev.raw        = m_mxpString;
			ev.attributes = attrs;
			ev.offset     = m_outputSize;
			ev.sequence   = m_mxpEventSequence++;
			ev.secure     = mxpSecure();
			appendMxpEvent(ev);
		}
		break;
	default: // start of tag
		// MXP_StartTag (m_strMXPstring);  // process the tag
		{
			const QMap<QByteArray, QByteArray> attrs = parseTagArguments(m_mxpString);
			const QByteArray name = attrs.contains("_tag") ? attrs.value("_tag") : QByteArray();
			m_mxpTagCount++;
			MxpEvent ev;
			ev.type       = MxpEvent::StartTag;
			ev.name       = name;
			ev.raw        = m_mxpString;
			ev.attributes = attrs;
			ev.offset     = m_outputSize;
			ev.sequence   = m_mxpEventSequence++;
			ev.secure     = mxpSecure();
			appendMxpEvent(ev);
		}
		break;
	} // end of switch

	if (wasSecureOnce)
		mxpRestoreMode();
}

// here when entity collection complete

QByteArray TelnetProcessor::mxpCollectedEntity()
{
	m_mxpString = trimMxp(m_mxpString);

	// case-insensitive
	//  m_strMXPstring.MakeLower ();

	// count them
	m_mxpEntityCount++;

	//  TRACE1 ("MXP collected entity %s\n", (LPCTSTR) m_strMXPstring);

	// MXP_error (DBG_ALL, msgMXP_CollectedEntity,
	//             TFormat ("MXP entity: &%s;",
	//             (LPCTSTR) m_strMXPstring));

	if (m_mxpString.isEmpty())
		return {};

	if (!isAsciiAlnum(static_cast<unsigned char>(m_mxpString.at(0))) && m_mxpString.at(0) != '#')
	{
		emitMxpDiagnosticLazy(DBG_ERROR, errMXP_InvalidEntityName,
		                      [this]
		                      {
			                      return QStringLiteral("Invalid MXP entity name \"%1\" supplied.")
			                          .arg(QString::fromLocal8Bit(m_mxpString));
		                      });
		return {};
	}

	// look for &#nnn;

	if (m_mxpString.at(0) == '#')
	{
		int iResult = 0;

		// validate and work out number
		if (m_mxpString.size() > 1 && m_mxpString.at(1) == 'x')
		{
			for (int i = 2; i < m_mxpString.size(); i++)
			{
				if (!isAsciiHexDigit(static_cast<unsigned char>(m_mxpString.at(i))))
				{
					emitMxpDiagnosticLazy(DBG_ERROR, errMXP_InvalidEntityNumber,
					                      [this]
					                      {
						                      return QStringLiteral("Invalid hex number in MXP entity: &%1;")
						                          .arg(QString::fromLocal8Bit(m_mxpString));
					                      });
					return {};
				}

				const int iNewDigit = asciiHexValue(static_cast<unsigned char>(m_mxpString.at(i)));
				if (iNewDigit < 0)
				{
					emitMxpDiagnosticLazy(DBG_ERROR, errMXP_InvalidEntityNumber,
					                      [this]
					                      {
						                      return QStringLiteral("Invalid hex number in MXP entity: &%1;")
						                          .arg(QString::fromLocal8Bit(m_mxpString));
					                      });
					return {};
				}
				if (iResult & 0xF0)
				{
					emitMxpDiagnosticLazy(
					    DBG_ERROR, errMXP_InvalidEntityNumber,
					    [this]
					    {
						    return QStringLiteral(
						               "Invalid hex number in MXP entity: &%1; - maximum of 2 hex digits")
						        .arg(QString::fromLocal8Bit(m_mxpString));
					    });
					return {};
				}
				iResult = (iResult << 4) + iNewDigit;
			}
		} // end of hex entity
		else
		{
			for (int i = 1; i < m_mxpString.size(); i++)
			{
				if (!isAsciiDigit(static_cast<unsigned char>(m_mxpString.at(i))))
				{
					emitMxpDiagnosticLazy(DBG_ERROR, errMXP_InvalidEntityNumber,
					                      [this]
					                      {
						                      return QStringLiteral("Invalid number in MXP entity: &%1;")
						                          .arg(QString::fromLocal8Bit(m_mxpString));
					                      });
					return {};
				}
				iResult *= 10;
				iResult += m_mxpString.at(i) - '0';
			}
		}
		if (iResult != 9 && iResult != 10 &&
		    iResult != 13) // we will accept tabs, newlines and carriage-returns ;)
		{
			if (iResult < 32 || // don't allow nonprintable characters
			    iResult > 255)  // don't allow characters more than 1 byte
			{
				emitMxpDiagnosticLazy(DBG_ERROR, errMXP_DisallowedEntityNumber,
				                      [this]
				                      {
					                      return QStringLiteral("Disallowed number in MXP entity: &%1;")
					                          .arg(QString::fromLocal8Bit(m_mxpString));
				                      });
				return {};
			}
		}
		const unsigned char cOneCharacterLine[2] = {static_cast<unsigned char>(iResult), 0};
		return QByteArray{reinterpret_cast<const char *>(cOneCharacterLine), 1};
	} // end of entity starting with #

	if (QByteArray resolved = mxpGetEntity(m_mxpString); !resolved.isEmpty())
		return resolved;

	emitMxpDiagnosticLazy(
	    DBG_ERROR, errMXP_UnknownEntity, [this]
	    { return QStringLiteral("Unknown MXP entity: &%1;").arg(QString::fromLocal8Bit(m_mxpString)); });
	return {};
}

// here at an unexpected termination of element collection, eg. <blah \n
void TelnetProcessor::mxpUnterminatedElement(const char *reason)
{
	const auto *type = "thing";
	long        code = errMXP_Unknown;
	switch (m_mxpPhase)
	{
	case HAVE_MXP_ELEMENT:
		type = "element";
		code = errMXP_UnterminatedElement;
		break;
	case HAVE_MXP_COMMENT:
		type = "comment";
		code = errMXP_UnterminatedComment;
		break;
	case HAVE_MXP_ENTITY:
		type = "entity";
		code = errMXP_UnterminatedEntity;
		break;
	case HAVE_MXP_QUOTE:
		type = "quote";
		code = errMXP_UnterminatedQuote;
		break;
	default:
		break;
	}

	emitMxpDiagnosticLazy(DBG_ERROR, code,
	                      [this, type, reason]
	                      {
		                      return QStringLiteral("Unterminated MXP %1: %2 (%3)")
		                          .arg(QString::fromLatin1(type), QString::fromLocal8Bit(m_mxpString),
		                               QString::fromLatin1(reason));
	                      });
}

QByteArray TelnetProcessor::trimMxp(const QByteArray &input)
{
	qsizetype start = 0;
	qsizetype end   = input.size();
	while (start < end && isAsciiSpace(static_cast<unsigned char>(input.at(start))))
		start++;
	while (end > start && isAsciiSpace(static_cast<unsigned char>(input.at(end - 1))))
		end--;
	return input.mid(start, end - start);
}

// mxputils.cpp - MXP utilities (partial)

// extract a word, return it in strResult, remove from str
// return true if end, false if not end
bool TelnetProcessor::getWord(QByteArray &result, QByteArray &input)
{
	const char *p   = input.constData();
	int         len = 0;

	// skip leading spaces
	while (*p == ' ')
		p++;

	// is it quoted string?
	if (*p == '\'' || *p == '\"')
	{
		const char  quote  = *p;
		const char *pStart = ++p; // bypass opening quote
		for (; *p != quote && *p; p++)
			len++; // count up to closing quote
		result = QByteArray(pStart, len);
		input  = QByteArray(++p); // return rest of line
		return false;             // not end, even if empty
	}

	// where word starts
	const char *pStart = p;

	// is it a word or number?
	if (isAsciiAlnum(static_cast<unsigned char>(*p)) || *p == '+' || *p == '-')
	{
		// skip initial character, and then look for terminator
		for (p++, len++; *p; p++, len++)
		{
			if (isAsciiAlnum(static_cast<unsigned char>(*p)))
				continue;
			if (*p == '_' || // underscore
			    *p == '-' || // hyphen
			    *p == '.' || // period
			    *p == ','    // comma
			)
				continue;

			break; // stop if not alpha, digit, or above special characters
		}
	}
	else
		// is it a colour?  ie. #xxxxxx
		if (*p == '#')
		{
			for (p++, len++; *p; p++, len++)
			{
				if (isAsciiHexDigit(static_cast<unsigned char>(*p)))
					continue;
				break; // stop if not hex digit
			}
		}
		else
			// is it an argument?  i.e. &xxx;
			if (*p == '&')
			{
				for (p++, len++; *p && *p != ';'; p++, len++)
					; // keep going until ;
				      // include the ; in the word
				if (*p == ';')
				{
					len++;
					p++;
				}
			}
			else if (*p) // provided not end of line
			// assume single character, e.g. '=' or ',' or something
			{
				len = 1;
				p++;
			}

	result = QByteArray(pStart, len);
	input  = QByteArray(p); // return rest of line
	return len == 0;        // true if nothing found
}

bool TelnetProcessor::isValidName(const QByteArray &input)
{
	const char *p = input.constData();

	// must start with a letter
	if (!isAsciiAlpha(static_cast<unsigned char>(*p)))
		return false;

	for (; *p; p++)
	{
		if (isAsciiAlnum(static_cast<unsigned char>(*p)))
			continue;
		if (*p == '_' || // underscore
		    *p == '-' || // hyphen
		    *p == '.'    // period
		)
			continue;

		return false; // stop if not alpha, digit, or above special characters
	}

	return true;
}

// builds an argument list from a string (e.g. "color=red blink=yes number=10")
// un-named arguments are numbered from 1 upwards
//
// returns true if invalid name (e.g. *434='22' )
// returns false if OK

QMap<QByteArray, QByteArray> TelnetProcessor::parseTagArguments(QByteArray input)
{
	QMap<QByteArray, QByteArray> result;
	QByteArray                   name;
	getWord(name, input);
	result.insert("_tag", name);

	struct ParsedArg
	{
			QByteArray argName;
			QByteArray argValue;
			int        position{0};
	};

	QVector<ParsedArg> args;
	int                positional = 0;
	QByteArray         argName;
	bool               end = getWord(argName, input);
	while (!end)
	{
		QByteArray equals;
		end = getWord(equals, input);
		if (equals == "=")
		{
			if (!isValidName(argName))
				return result;
			QByteArray argValue;
			getWord(argValue, input);
			ParsedArg arg;
			arg.argName  = argName.toLower();
			arg.argValue = argValue;
			args.append(arg);
			end = getWord(argName, input);
		}
		else
		{
			ParsedArg arg;
			arg.position = ++positional;
			arg.argValue = argName;
			args.append(arg);
			argName = equals;
		}
	}

	for (const ParsedArg &arg : args)
	{
		if (const auto &[entryArgName, entryArgValue, entryPosition] = arg; entryPosition > 0)
		{
			result.insert(QByteArray::number(entryPosition), entryArgValue);
		}
		else if (!entryArgName.isEmpty())
		{
			result.insert(entryArgName, entryArgValue);
		}
	}

	return result;
}

void TelnetProcessor::emitMxpDiagnostic(const int level, const long messageNumber,
                                        const QString &message) const
{
	if (!m_callbacks.onMxpDiagnostic)
		return;
	if (m_callbacks.onMxpDiagnosticNeeded && !m_callbacks.onMxpDiagnosticNeeded(level))
		return;
	m_callbacks.onMxpDiagnostic(level, messageNumber, message);
}

// mxpDefs.cpp - MXP definitions (partial)

// handle definition-style tag, eg. <!ELEMENT blah> or <!ENTITY blah>

void TelnetProcessor::mxpDefinition(QByteArray definition)
{
	if (m_puebloActive)
	{
		emitMxpDiagnosticLazy(DBG_ERROR, errMXP_DefinitionAttemptInPueblo,
		                      [definition]
		                      {
			                      return QStringLiteral(
			                                 "Defining elements/entities not valid in Pueblo: <!%1>")
			                          .arg(QString::fromLocal8Bit(definition));
		                      });
		return;
	}

	const bool isSecure = mxpSecure();
	if (m_mxpMode == eMXP_secure_once)
		mxpRestoreMode();

	if (!isSecure)
	{
		emitMxpDiagnosticLazy(DBG_ERROR, errMXP_DefinitionWhenNotSecure,
		                      [definition]
		                      {
			                      return QStringLiteral(
			                                 "MXP definition ignored when not in secure mode: <!%1>")
			                          .arg(QString::fromLocal8Bit(definition));
		                      });
		return;
	}

	QByteArray strDefinition;

	// get first word (e.g. ELEMENT or ENTITY)
	getWord(strDefinition, definition);

	if (!isValidName(strDefinition))
	{
		emitMxpDiagnosticLazy(DBG_ERROR, errMXP_InvalidDefinition,
		                      [definition]
		                      {
			                      return QStringLiteral("Invalid MXP definition name: <!%1>")
			                          .arg(QString::fromLocal8Bit(definition));
		                      });
		return;
	}

	strDefinition = strDefinition.toLower(); // case-insensitive?

	// get name of what we are defining
	QByteArray strName;
	getWord(strName, definition);

	if (!isValidName(strName))
	{
		emitMxpDiagnosticLazy(DBG_ERROR, errMXP_InvalidElementName,
		                      [strName]
		                      {
			                      return QStringLiteral("Invalid MXP element/entity name: \"%1\"")
			                          .arg(QString::fromLocal8Bit(strName));
		                      });
		return;
	}

	// debugging
	// MXP_error (DBG_INFO, msgMXP_GotDefinition,
	//           TFormat ("Got Definition: !%s %s %s",
	//                   (LPCTSTR) strDefinition,
	//                   (LPCTSTR) strName,
	//                   (LPCTSTR) strTag));

	if (strDefinition == "element" || strDefinition == "el")
	{
		// MXP_Element (strName, strTag);
		mxpElement(strName, definition);
	}
	else if (strDefinition == "entity" || strDefinition == "en")
	{
		mxpEntity(strName, definition);
	}
	else if (strDefinition == "attlist" || strDefinition == "at")
	{
		// MXP_Attlist (strName, strTag);
		mxpAttlist(strName, definition);
	}
	else
	{
		emitMxpDiagnosticLazy(DBG_ERROR, errMXP_InvalidDefinition,
		                      [strDefinition]
		                      {
			                      return QStringLiteral("Unknown definition type: <!%1>")
			                          .arg(QString::fromLocal8Bit(strDefinition));
		                      });
	}
}

// here for <!ENTITY blah>
void TelnetProcessor::mxpEntity(const QByteArray &name, const QByteArray &tagRemainder)
{
	QByteArray       mutableTagRemainder = tagRemainder;

	// case-insensitive
	const QByteArray lowerName = name.toLower();

	if (isBuiltinEntityDefinitionName(lowerName))
	{
		emitMxpDiagnosticLazy(
		    DBG_ERROR, errMXP_CannotRedefineEntity,
		    [lowerName]
		    {
			    return QStringLiteral("Cannot redefine entity: &%1;").arg(QString::fromLocal8Bit(lowerName));
		    });
		return;
	}

	// look for entities imbedded in the definition, e.g. <!EN blah '&lt;Nick&gt;'>
	QByteArray strEntityContents;
	getWord(strEntityContents, mutableTagRemainder);

	// blank contents deletes the entity
	if (strEntityContents.isEmpty())
	{
		m_customEntities.remove(lowerName);
		return;
	}

	const char *p = strEntityContents.constData();
	QByteArray  strFixedValue;

	for (; *p; p++)
	{
		if (*p == '&')
		{
			p++; // skip ampersand
			const char *pStart = p;
			for (; *p && *p != ';'; p++) // look for closing semicolon
				;                        // just keep looking
			if (*p != ';')
			{
				emitMxpDiagnosticLazy(DBG_ERROR, errMXP_NoClosingSemicolon,
				                      [strEntityContents]
				                      {
					                      return QStringLiteral(
					                                 "No closing ';' in MXP entity argument \"%1\"")
					                          .arg(QString::fromLocal8Bit(strEntityContents));
				                      });
				return;
			}

			QByteArray s(pStart, p - pStart);
			strFixedValue += mxpGetEntity(s); // add to list
		} // end of having an ampersand
		else
			strFixedValue += *p; // just add ordinary characters to list

	} // end of processing the value

	// add entity to map
	if (!m_customEntities.contains(lowerName) && m_customEntities.size() >= kMaxMxpCustomDefinitions)
	{
		emitMxpDiagnosticLazy(
		    DBG_WARNING, wrnMXP_CustomDefinitionLimitExceeded,
		    [lowerName]
		    {
			    return QStringLiteral("MXP custom definition limit reached (%1). Ignoring entity &%2;.")
			        .arg(kMaxMxpCustomDefinitions)
			        .arg(QString::fromLocal8Bit(lowerName));
		    });
		return;
	}
	m_customEntities.insert(lowerName, strFixedValue);

	// check they didn't supply any other arguments
	mutableTagRemainder = trimMxp(mutableTagRemainder);
	while (!mutableTagRemainder.isEmpty())
	{
		QByteArray strKeyword;
		getWord(strKeyword, mutableTagRemainder);

		strKeyword = strKeyword.toLower();

		if (strKeyword == "desc")
		{
			getWord(strKeyword, mutableTagRemainder);
			if (strKeyword != "=")
				getWord(strKeyword, mutableTagRemainder); // get description
		}
		else if (strKeyword == "private" || strKeyword == "publish" || strKeyword == "add")
		{
			// do nothing
		}
		else if (strKeyword == "delete" || strKeyword == "remove")
		{
			m_customEntities.remove(lowerName);
		}
		else
		{
			emitMxpDiagnosticLazy(
			    DBG_WARNING, errMXP_UnexpectedEntityArguments,
			    [strKeyword, lowerName]
			    {
				    return QStringLiteral("Unexpected word \"%1\" in entity definition for &%2; ignored")
				        .arg(QString::fromLocal8Bit(strKeyword), QString::fromLocal8Bit(lowerName));
			    });
			return;
		}

	} // of processing optional words
}

// here for <!ELEMENT blah>
void TelnetProcessor::mxpElement(const QByteArray &name, const QByteArray &tagRemainder)
{
	struct ParsedArg
	{
			QByteArray argName;
			QByteArray argValue;
			int        position{0};
			bool       used{false};
			bool       keyword{false};
	};

	auto buildArgumentList = [&](QVector<ParsedArg> &args, QByteArray input) -> bool
	{
		args.clear();
		int        positional = 0;
		QByteArray argName;
		bool       end = getWord(argName, input);
		while (!end)
		{
			if (argName == "/")
			{
				input = trimMxp(input);
				return !input.isEmpty();
			}

			QByteArray equals;
			end = getWord(equals, input);
			if (equals == "=")
			{
				if (!isValidName(argName))
					return true;
				QByteArray argValue;
				end = getWord(argValue, input);
				if (end)
					return true;
				ParsedArg arg;
				arg.argName  = argName.toLower();
				arg.argValue = argValue;
				args.append(arg);
				end = getWord(argName, input);
			}
			else
			{
				ParsedArg arg;
				arg.position = ++positional;
				arg.argValue = argName;
				args.append(arg);
				argName = equals;
			}
		}
		return false;
	};

	auto getKeyword = [&](QVector<ParsedArg> &args, const QByteArray &keyword) -> bool
	{
		const QByteArray lower = keyword.toLower();
		for (ParsedArg &arg : args)
		{
			auto &[argName, argValue, position, used, isKeyword] = arg;
			if (!argName.isEmpty())
				continue;
			if (argValue.toLower() != lower)
				continue;
			used      = true;
			isKeyword = true;
			if (const int pos = position; pos > 0)
			{
				for (ParsedArg &entry : args)
				{
					if (entry.position > pos)
						--entry.position;
				}
				position = 0;
			}
			return true;
		}
		return false;
	};

	auto getArgument = [&](const QVector<ParsedArg> &args, const QByteArray &argName, const int position,
	                       const bool lowerCase) -> QByteArray
	{
		const QByteArray wanted = argName.toLower();
		const ParsedArg *found  = nullptr;
		for (const ParsedArg &arg : args)
		{
			if (arg.used || arg.keyword)
				continue;
			if (!wanted.isEmpty() && arg.argName == wanted)
			{
				found = &arg;
				break;
			}
			if (position > 0 && arg.position == position)
				found = &arg;
		}
		if (!found)
			return {};
		QByteArray value = found->argValue;
		if (lowerCase)
			value = value.toLower();
		return value;
	};

	QVector<ParsedArg> args;
	if (buildArgumentList(args, tagRemainder))
		return;

	const bool       doDelete = getKeyword(args, QByteArrayLiteral("delete"));

	const QByteArray lowerName = name.toLower(); // case-insensitive?

	if (isBuiltinElementName(lowerName))
	{
		emitMxpDiagnosticLazy(DBG_ERROR, errMXP_CannotRedefineElement,
		                      [lowerName]
		                      {
			                      return QStringLiteral("Cannot redefine built-in MXP element: <%1>")
			                          .arg(QString::fromLocal8Bit(lowerName));
		                      });
		return;
	}

	// if element already defined, delete old one
	if (m_customElements.contains(lowerName))
	{
		m_customElements.remove(lowerName);
	}
	if (doDelete)
		return;

	CustomElement element;
	element.name    = lowerName;
	element.open    = getKeyword(args, QByteArrayLiteral("open"));
	element.command = getKeyword(args, QByteArrayLiteral("empty"));

	element.definition = getArgument(args, QByteArray(), 1, false);
	element.attributes = getArgument(args, QByteArrayLiteral("att"), 2, false);

	// get tag (TAG=22)
	if (const QByteArray tagValue = getArgument(args, QByteArrayLiteral("tag"), 3, true); !tagValue.isEmpty())
	{
		bool ok = false;
		if (const int i = tagValue.toInt(&ok); ok && i >= 20 && i <= 99)
			element.tag = i;
	}

	// get tag (FLAG=roomname)
	if (const QByteArray flag = getArgument(args, QByteArrayLiteral("flag"), 4, true); !flag.isEmpty())
	{
		if (flag.left(4).toLower() == "set ")
			element.flag = flag.mid(4);
		else
			element.flag = flag;

		element.flag = trimMxp(element.flag);
		element.flag.replace(' ', '_');
	}

	if (!m_customElements.contains(lowerName) && m_customElements.size() >= kMaxMxpCustomDefinitions)
	{
		emitMxpDiagnosticLazy(
		    DBG_WARNING, wrnMXP_CustomDefinitionLimitExceeded,
		    [lowerName]
		    {
			    return QStringLiteral("MXP custom definition limit reached (%1). Ignoring element <%2>.")
			        .arg(kMaxMxpCustomDefinitions)
			        .arg(QString::fromLocal8Bit(lowerName));
		    });
		return;
	}
	m_customElements.insert(lowerName, element);
}

// here for <!ATTLIST blah>
void TelnetProcessor::mxpAttlist(const QByteArray &name, const QByteArray &tagRemainder)
{
	struct ParsedArg
	{
			QByteArray argName;
			QByteArray argValue;
	};

	auto buildArgumentList = [&](QVector<ParsedArg> &args, QByteArray input) -> bool
	{
		args.clear();
		QByteArray argName;
		bool       end = getWord(argName, input);
		while (!end)
		{
			if (argName == "/")
			{
				input = trimMxp(input);
				return !input.isEmpty();
			}
			QByteArray equals;
			end = getWord(equals, input);
			if (equals == "=")
			{
				if (!isValidName(argName))
					return true;
				QByteArray argValue;
				end = getWord(argValue, input);
				if (end)
					return true;
				ParsedArg arg;
				arg.argName  = argName.toLower();
				arg.argValue = argValue;
				args.append(arg);
				end = getWord(argName, input);
			}
			else
			{
				ParsedArg arg;
				arg.argValue = argName;
				args.append(arg);
				argName = equals;
			}
		}
		return false;
	};

	if (QVector<ParsedArg> args; buildArgumentList(args, tagRemainder))
		return;

	// append attributes strTag to element strName

	const QByteArray lowerName = name.toLower(); // case-insensitive?

	// check element already defined
	if (!m_customElements.contains(lowerName))
	{
		emitMxpDiagnosticLazy(DBG_ERROR, errMXP_UnknownElementInAttlist,
		                      [lowerName]
		                      {
			                      return QStringLiteral(
			                                 "Cannot add attributes to undefined MXP element: <%1>")
			                          .arg(QString::fromLocal8Bit(lowerName));
		                      });
		return;
	} // end of no element matching

	CustomElement element = m_customElements.value(lowerName);

	const qsizetype separatorBytes = element.attributes.isEmpty() ? 0 : 1;
	const qsizetype appendBytes    = separatorBytes + tagRemainder.size();
	if (appendBytes > 0 && element.attributes.size() + appendBytes > kMaxMxpAttlistBytes)
	{
		emitMxpDiagnosticLazy(
		    DBG_WARNING, wrnMXP_AttlistLimitExceeded,
		    [lowerName]
		    {
			    return QStringLiteral("MXP ATTLIST for <%1> exceeded %2 bytes; ignoring additional attributes.")
			        .arg(QString::fromLocal8Bit(lowerName))
			        .arg(kMaxMxpAttlistBytes);
		    });
		return;
	}

	// add to any existing arguments - is this wise? :)
	if (!element.attributes.isEmpty())
	{
		element.attributes.append(' ');
		element.attributes.append(tagRemainder);
	}
	else
		element.attributes = tagRemainder;

	m_customElements.insert(lowerName, element);
}

QByteArray TelnetProcessor::mxpGetEntity(const QByteArray &name) const
{
	if (QByteArray builtin; resolveBuiltinEntity(name, builtin))
		return builtin;

	if (const QByteArray lower = name.toLower(); m_customEntities.contains(lower))
		return m_customEntities.value(lower);

	return {};
}

bool TelnetProcessor::mxpOpen() const
{
	return m_mxpMode == eMXP_open || m_mxpMode == eMXP_perm_open;
}

bool TelnetProcessor::mxpSecure() const
{
	return m_mxpMode == eMXP_secure || m_mxpMode == eMXP_secure_once || m_mxpMode == eMXP_perm_secure;
}

void TelnetProcessor::mxpRestoreMode()
{
	if (m_mxpMode == eMXP_secure_once)
		m_mxpMode = m_mxpPreviousMode;
}

void TelnetProcessor::mxpModeChange(int newMode)
{
	if (newMode == -1)
		newMode = m_mxpDefaultMode;

	const int  oldMode = m_mxpMode;
	const bool oldPermanent =
	    oldMode == eMXP_perm_open || oldMode == eMXP_perm_secure || oldMode == eMXP_perm_locked;
	const bool newPermanent =
	    newMode == eMXP_perm_open || newMode == eMXP_perm_secure || newMode == eMXP_perm_locked;
	const bool shouldLog = newMode != oldMode && (oldPermanent || newPermanent);

	switch (newMode)
	{
	case eMXP_open:
	case eMXP_secure:
	case eMXP_locked:
		m_mxpDefaultMode = eMXP_open;
		break;
	case eMXP_secure_once:
		m_mxpPreviousMode = m_mxpMode;
		break;
	case eMXP_perm_open:
	case eMXP_perm_secure:
	case eMXP_perm_locked:
		m_mxpDefaultMode = newMode;
		break;
	default:
		break;
	}

	m_mxpMode = newMode;

	if (oldMode != newMode)
	{
		MxpModeChange marker;
		marker.offset    = m_outputSize;
		marker.sequence  = m_mxpEventSequence++;
		marker.oldMode   = oldMode;
		marker.newMode   = newMode;
		marker.shouldLog = shouldLog;
		m_mxpModeChanges.push_back(marker);
	}

	if (m_callbacks.onMxpModeChange)
		m_callbacks.onMxpModeChange(oldMode, newMode, shouldLog);
}

void TelnetProcessor::mxpOn(const bool pueblo, const bool manual)
{
	if (m_mxpEnabled)
		return;

	m_mxpEnabled   = true;
	m_puebloActive = pueblo;

	if (!manual)
	{
		m_mxpDefaultMode = eMXP_open;
		m_mxpMode        = eMXP_open;
		m_customElements.clear();
		m_customEntities.clear();
	}

	if (m_callbacks.onMxpStart)
		m_callbacks.onMxpStart(pueblo, manual);
}

void TelnetProcessor::mxpOff(const bool completely)
{
	if (!m_mxpEnabled)
		return;

	if (completely)
	{
		const bool wasPuebloActive = m_puebloActive;
		mxpModeChange(eMXP_open);
		if (m_mxpPhase != MXP_NONE)
		{
			m_mxpPhase = MXP_NONE;
			m_mxpString.clear();
		}
		m_mxpEnabled   = false;
		m_puebloActive = false;
		if (m_callbacks.onMxpStop)
			m_callbacks.onMxpStop(true, wasPuebloActive);
	}
	else if (m_callbacks.onMxpReset)
	{
		m_callbacks.onMxpReset();
	}
}
