/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: EncodingUtils.cpp
 * Role: Text/binary encoding helper implementations used by runtime serialization and script-visible utility APIs.
 */

#include "EncodingUtils.h"

#include <QStringConverter>
// ReSharper disable once CppUnusedIncludeDirective
#include <QStringEncoder>

#include <algorithm>
#include <utility>

namespace
{
	constexpr char16_t kWindows1252C1Map[32] = {
	    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
	    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
	    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

	QChar decodeWindows1252Byte(const unsigned char byte)
	{
		if (byte >= 0x80 && byte <= 0x9F)
			return {kWindows1252C1Map[byte - 0x80]};
		return QChar(byte);
	}

	void appendCodePoint(QString &output, const uint codePoint)
	{
		if (codePoint <= 0xFFFF)
		{
			output.append(QChar(static_cast<char16_t>(codePoint)));
			return;
		}
		const uint adjusted = codePoint - 0x10000;
		output.append(QChar(static_cast<char16_t>(0xD800 + (adjusted >> 10))));
		output.append(QChar(static_cast<char16_t>(0xDC00 + (adjusted & 0x3FF))));
	}

	QByteArray wrapBase64Lines(const QByteArray &encoded)
	{
		QByteArray wrapped;
		wrapped.reserve(encoded.size() + (encoded.size() / 76 + 1) * 2);
		for (int i = 0; i < encoded.size(); i += 76)
		{
			wrapped.append(encoded.mid(i, 76));
			if (i + 76 < encoded.size() || (encoded.size() % 76 == 0))
				wrapped.append("\r\n");
		}
		return wrapped;
	}

	QByteArray normalizedEncodingKey(const QString &name)
	{
		return name.trimmed().toLatin1().toUpper().replace('_', '-');
	}

	QString languageHintForEncodingKey(const QByteArray &key);

	bool    isProtocolSafeLegacyEncodingKey(const QByteArray &key)
	{
		if (key.isEmpty())
			return false;
		if (key.startsWith("UTF") || key.startsWith("UCS"))
			return false;
		if (key == QByteArrayLiteral("SYSTEM") || key == QByteArrayLiteral("CESU-8") ||
		    key == QByteArrayLiteral("BOCU-1") || key == QByteArrayLiteral("SCSU"))
			return false;
		if (key.startsWith("ISO-2022"))
			return false;
		return languageHintForEncodingKey(key) != QStringLiteral("Multiple languages");
	}

	QString languageHintForEncodingKey(const QByteArray &key)
	{
		if (key == QByteArrayLiteral("US-ASCII") || key == QByteArrayLiteral("ASCII") ||
		    key == QByteArrayLiteral("ANSI-X3.4-1968"))
			return QStringLiteral("English/ASCII");

		if (key == QByteArrayLiteral("GB18030") || key == QByteArrayLiteral("GBK") ||
		    key == QByteArrayLiteral("GB2312") || key == QByteArrayLiteral("EUC-CN") ||
		    key == QByteArrayLiteral("CP936") || key == QByteArrayLiteral("WINDOWS-936") ||
		    key == QByteArrayLiteral("IBM-936"))
			return QStringLiteral("Simplified Chinese");
		if (key == QByteArrayLiteral("BIG5") || key == QByteArrayLiteral("BIG5-HKSCS") ||
		    key == QByteArrayLiteral("CP950") || key == QByteArrayLiteral("WINDOWS-950") ||
		    key == QByteArrayLiteral("IBM-950"))
			return QStringLiteral("Traditional Chinese");
		if (key == QByteArrayLiteral("SHIFT-JIS") || key == QByteArrayLiteral("SHIFT-JISX0213") ||
		    key == QByteArrayLiteral("SJIS") || key == QByteArrayLiteral("MS-KANJI") ||
		    key == QByteArrayLiteral("CP932") || key == QByteArrayLiteral("WINDOWS-31J") ||
		    key == QByteArrayLiteral("EUC-JP") || key == QByteArrayLiteral("EUCJP"))
			return QStringLiteral("Japanese");
		if (key == QByteArrayLiteral("EUC-KR") || key == QByteArrayLiteral("CP949") ||
		    key == QByteArrayLiteral("WINDOWS-949") || key == QByteArrayLiteral("IBM-949") ||
		    key == QByteArrayLiteral("KS-C-5601") || key == QByteArrayLiteral("KSC5601"))
			return QStringLiteral("Korean");
		if (key == QByteArrayLiteral("TIS-620") || key == QByteArrayLiteral("ISO-8859-11") ||
		    key == QByteArrayLiteral("CP874") || key == QByteArrayLiteral("WINDOWS-874"))
			return QStringLiteral("Thai");
		if (key == QByteArrayLiteral("VISCII") || key == QByteArrayLiteral("TCVN") ||
		    key == QByteArrayLiteral("CP1258") || key == QByteArrayLiteral("WINDOWS-1258"))
			return QStringLiteral("Vietnamese");

		if (key == QByteArrayLiteral("ISO-8859-1") || key == QByteArrayLiteral("ISO8859-1") ||
		    key == QByteArrayLiteral("LATIN1") || key == QByteArrayLiteral("CP1252") ||
		    key == QByteArrayLiteral("WINDOWS-1252") || key == QByteArrayLiteral("IBM-1252") ||
		    key == QByteArrayLiteral("MACROMAN") || key == QByteArrayLiteral("MACINTOSH") ||
		    key == QByteArrayLiteral("CP437") || key == QByteArrayLiteral("IBM437") ||
		    key == QByteArrayLiteral("CP850") || key == QByteArrayLiteral("IBM850"))
			return QStringLiteral("Western European");
		if (key == QByteArrayLiteral("ISO-8859-15") || key == QByteArrayLiteral("ISO8859-15") ||
		    key == QByteArrayLiteral("LATIN9"))
			return QStringLiteral("Western European");
		if (key == QByteArrayLiteral("ISO-8859-2") || key == QByteArrayLiteral("ISO8859-2") ||
		    key == QByteArrayLiteral("LATIN2") || key == QByteArrayLiteral("CP1250") ||
		    key == QByteArrayLiteral("WINDOWS-1250") || key == QByteArrayLiteral("IBM-1250") ||
		    key == QByteArrayLiteral("CP852") || key == QByteArrayLiteral("IBM852"))
			return QStringLiteral("Central/Eastern European");
		if (key == QByteArrayLiteral("ISO-8859-3") || key == QByteArrayLiteral("ISO8859-3") ||
		    key == QByteArrayLiteral("LATIN3"))
			return QStringLiteral("South European");
		if (key == QByteArrayLiteral("ISO-8859-4") || key == QByteArrayLiteral("ISO8859-4") ||
		    key == QByteArrayLiteral("LATIN4") || key == QByteArrayLiteral("ISO-8859-13") ||
		    key == QByteArrayLiteral("ISO8859-13") || key == QByteArrayLiteral("CP1257") ||
		    key == QByteArrayLiteral("WINDOWS-1257"))
			return QStringLiteral("Baltic");
		if (key == QByteArrayLiteral("ISO-8859-5") || key == QByteArrayLiteral("ISO8859-5") ||
		    key == QByteArrayLiteral("CP1251") || key == QByteArrayLiteral("WINDOWS-1251") ||
		    key == QByteArrayLiteral("IBM-1251") || key == QByteArrayLiteral("CP866") ||
		    key == QByteArrayLiteral("IBM866") || key == QByteArrayLiteral("KOI8-R") ||
		    key == QByteArrayLiteral("KOI8-U") || key == QByteArrayLiteral("KOI8-RU") ||
		    key == QByteArrayLiteral("MACCYRILLIC"))
			return QStringLiteral("Cyrillic");
		if (key == QByteArrayLiteral("ISO-8859-6") || key == QByteArrayLiteral("ISO8859-6") ||
		    key == QByteArrayLiteral("CP1256") || key == QByteArrayLiteral("WINDOWS-1256"))
			return QStringLiteral("Arabic");
		if (key == QByteArrayLiteral("ISO-8859-7") || key == QByteArrayLiteral("ISO8859-7") ||
		    key == QByteArrayLiteral("CP1253") || key == QByteArrayLiteral("WINDOWS-1253"))
			return QStringLiteral("Greek");
		if (key == QByteArrayLiteral("ISO-8859-8") || key == QByteArrayLiteral("ISO8859-8") ||
		    key == QByteArrayLiteral("CP1255") || key == QByteArrayLiteral("WINDOWS-1255"))
			return QStringLiteral("Hebrew");
		if (key == QByteArrayLiteral("ISO-8859-9") || key == QByteArrayLiteral("ISO8859-9") ||
		    key == QByteArrayLiteral("LATIN5") || key == QByteArrayLiteral("CP1254") ||
		    key == QByteArrayLiteral("WINDOWS-1254"))
			return QStringLiteral("Turkish");
		if (key == QByteArrayLiteral("ISO-8859-10") || key == QByteArrayLiteral("ISO8859-10") ||
		    key == QByteArrayLiteral("LATIN6"))
			return QStringLiteral("Nordic");
		if (key == QByteArrayLiteral("ISO-8859-14") || key == QByteArrayLiteral("ISO8859-14") ||
		    key == QByteArrayLiteral("LATIN8"))
			return QStringLiteral("Celtic");
		if (key == QByteArrayLiteral("ISO-8859-16") || key == QByteArrayLiteral("ISO8859-16") ||
		    key == QByteArrayLiteral("LATIN10"))
			return QStringLiteral("South-Eastern European");

		return QStringLiteral("Multiple languages");
	}

	const QStringList &cachedAvailableWorldTextEncodings()
	{
		static const QStringList codecs = []
		{
			QStringList available = QStringConverter::availableCodecs();
			available.removeDuplicates();
			available.erase(std::ranges::remove_if(
			                    available, [](const QString &codec)
			                    { return !isProtocolSafeLegacyEncodingKey(normalizedEncodingKey(codec)); })
			                    .begin(),
			                available.end());
			std::ranges::sort(available, [](const QString &left, const QString &right)
			                  { return QString::compare(left, right, Qt::CaseInsensitive) < 0; });
			return available;
		}();
		return codecs;
	}

	bool isWindows1252EncodingName(const QString &name)
	{
		const QByteArray key = normalizedEncodingKey(name);
		return key == QByteArrayLiteral("WINDOWS-1252") || key == QByteArrayLiteral("CP1252") ||
		       key == QByteArrayLiteral("IBM-1252");
	}

	QString supportedEncodingNameForKey(const QByteArray &key)
	{
		if (!isProtocolSafeLegacyEncodingKey(key))
			return {};
		for (const QString &codec : cachedAvailableWorldTextEncodings())
		{
			if (normalizedEncodingKey(codec) == key)
				return codec;
		}
		return {};
	}

	void appendUniqueCharsetName(QList<QByteArray> &names, const QByteArray &name)
	{
		const QByteArray trimmed = name.trimmed();
		if (trimmed.isEmpty())
			return;
		const QByteArray key = trimmed.toUpper();
		for (const QByteArray &existing : std::as_const(names))
		{
			if (existing.toUpper() == key)
				return;
		}
		names.push_back(trimmed);
	}
} // namespace

QString qmudEncodeBase64Text(const QByteArray &plaintext, const bool multiLine)
{
	QByteArray encoded = plaintext.toBase64(QByteArray::Base64Encoding);
	if (multiLine)
		encoded = wrapBase64Lines(encoded);
	return QString::fromLatin1(encoded);
}

QString qmudEncodeBase64Text(const char *plaintext, const bool multiLine)
{
	if (!plaintext)
		return {};
	return qmudEncodeBase64Text(QByteArray(plaintext), multiLine);
}

QString qmudDecodeWindows1252(const QByteArrayView bytes)
{
	if (bytes.isEmpty())
		return {};
	QString output;
	output.reserve(bytes.size());
	for (const auto byte : bytes)
		output.append(decodeWindows1252Byte(static_cast<unsigned char>(byte)));
	return output;
}

QString qmudDecodeUtf8WithWindows1252Fallback(const QByteArrayView bytes, QByteArray &carry,
                                              bool *hadInvalidBytes)
{
	if (hadInvalidBytes)
		*hadInvalidBytes = false;

	QByteArray buffer;
	buffer.reserve(carry.size() + bytes.size());
	if (!carry.isEmpty())
		buffer.append(carry);
	if (!bytes.isEmpty())
		buffer.append(bytes.data(), static_cast<qsizetype>(bytes.size()));
	carry.clear();

	QString output;
	output.reserve(buffer.size());
	int i = 0;
	while (i < buffer.size())
	{
		const auto head = static_cast<unsigned char>(buffer.at(i));
		if (head < 0x80)
		{
			output.append(QChar(head));
			++i;
			continue;
		}

		int  sequenceLength = 0;
		uint codePoint      = 0;
		uint minimumCode    = 0;
		if (head >= 0xC2 && head <= 0xDF)
		{
			sequenceLength = 2;
			codePoint      = head & 0x1F;
			minimumCode    = 0x80;
		}
		else if (head >= 0xE0 && head <= 0xEF)
		{
			sequenceLength = 3;
			codePoint      = head & 0x0F;
			minimumCode    = 0x800;
		}
		else if (head >= 0xF0 && head <= 0xF4)
		{
			sequenceLength = 4;
			codePoint      = head & 0x07;
			minimumCode    = 0x10000;
		}
		else
		{
			output.append(decodeWindows1252Byte(head));
			if (hadInvalidBytes)
				*hadInvalidBytes = true;
			++i;
			continue;
		}

		if (i + sequenceLength > buffer.size())
		{
			carry = buffer.mid(i);
			break;
		}

		bool continuationValid = true;
		for (int j = 1; j < sequenceLength; ++j)
		{
			const auto continuation = static_cast<unsigned char>(buffer.at(i + j));
			if ((continuation & 0xC0) != 0x80)
			{
				continuationValid = false;
				break;
			}
			codePoint = (codePoint << 6) | static_cast<uint>(continuation & 0x3F);
		}

		if (!continuationValid || codePoint < minimumCode || codePoint > 0x10FFFF ||
		    (codePoint >= 0xD800 && codePoint <= 0xDFFF))
		{
			output.append(decodeWindows1252Byte(head));
			if (hadInvalidBytes)
				*hadInvalidBytes = true;
			++i;
			continue;
		}

		appendCodePoint(output, codePoint);
		i += sequenceLength;
	}

	return output;
}

QString qmudDefaultLegacyWorldEncodingName()
{
	return QStringLiteral("windows-1252");
}

QStringList qmudAvailableWorldTextEncodings()
{
	return cachedAvailableWorldTextEncodings();
}

QString qmudWorldTextEncodingDisplayName(const QString &encodingName)
{
	const QString canonical = qmudNormalizeWorldTextEncodingName(encodingName);
	const QString language  = languageHintForEncodingKey(normalizedEncodingKey(canonical));
	return QStringLiteral("%1 (%2)").arg(canonical, language);
}

QString qmudNormalizeWorldTextEncodingName(const QString &name)
{
	const QByteArray key = normalizedEncodingKey(name);
	if (!key.isEmpty())
	{
		if (const QString supported = supportedEncodingNameForKey(key); !supported.isEmpty())
			return supported;
	}

	if (const QString defaultEncoding =
	        supportedEncodingNameForKey(normalizedEncodingKey(qmudDefaultLegacyWorldEncodingName()));
	    !defaultEncoding.isEmpty())
		return defaultEncoding;

	return qmudDefaultLegacyWorldEncodingName();
}

QStringDecoder qmudCreateWorldTextDecoder(const QString &encodingName)
{
	return QStringDecoder(qmudNormalizeWorldTextEncodingName(encodingName));
}

QString qmudDecodeWorldText(const QByteArrayView bytes, QStringDecoder &decoder, bool *hadInvalidBytes)
{
	if (hadInvalidBytes)
		*hadInvalidBytes = false;
	if (bytes.isEmpty())
		return {};

	const QString decoded = decoder.decode(bytes);
	if (hadInvalidBytes && decoder.hasError())
		*hadInvalidBytes = true;
	return decoded;
}

QString qmudDecodeWorldTextIsolated(const QByteArrayView bytes, const QString &encodingName,
                                    bool *hadInvalidBytes)
{
	if (isWindows1252EncodingName(encodingName))
	{
		if (hadInvalidBytes)
			*hadInvalidBytes = false;
		return qmudDecodeWindows1252(bytes);
	}

	QStringDecoder decoder = qmudCreateWorldTextDecoder(encodingName);
	return qmudDecodeWorldText(bytes, decoder, hadInvalidBytes);
}

QByteArray qmudEncodeWorldText(const QString &text, const QString &encodingName, bool *hadInvalidCharacters)
{
	if (hadInvalidCharacters)
		*hadInvalidCharacters = false;
	if (text.isEmpty())
		return {};

	QStringEncoder encoder(qmudNormalizeWorldTextEncodingName(encodingName));
	QByteArray     encoded = encoder.encode(text);
	if (hadInvalidCharacters && encoder.hasError())
		*hadInvalidCharacters = true;
	return encoded;
}

QList<QByteArray> qmudWorldTextTelnetCharsetNames(const bool useUtf8, const QString &legacyEncodingName)
{
	QList<QByteArray> names;
	if (useUtf8)
	{
		appendUniqueCharsetName(names, QByteArrayLiteral("UTF-8"));
		appendUniqueCharsetName(names, QByteArrayLiteral("UTF8"));
		return names;
	}

	const QString    normalized = qmudNormalizeWorldTextEncodingName(legacyEncodingName);
	const QByteArray canonical  = normalized.toLatin1();
	appendUniqueCharsetName(names, canonical);

	const QByteArray key = normalizedEncodingKey(normalized);
	if (key == QByteArrayLiteral("GB18030"))
		appendUniqueCharsetName(names, QByteArrayLiteral("GBK"));
	else if (key == QByteArrayLiteral("GBK"))
		appendUniqueCharsetName(names, QByteArrayLiteral("GB18030"));
	else if (key == QByteArrayLiteral("WINDOWS-1252"))
		appendUniqueCharsetName(names, QByteArrayLiteral("CP1252"));
	else if (key == QByteArrayLiteral("SHIFT-JIS"))
		appendUniqueCharsetName(names, QByteArrayLiteral("SHIFT_JIS"));

	return names;
}
