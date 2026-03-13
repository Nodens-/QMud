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
