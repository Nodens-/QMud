/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: EncodingUtils.cpp
 * Role: Text/binary encoding helper implementations used by runtime serialization and script-visible utility APIs.
 */

#include "EncodingUtils.h"

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
