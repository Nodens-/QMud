/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: EncodingUtils.h
 * Role: Text/binary encoding helper interfaces used by runtime serialization and script-visible utility APIs.
 */

#ifndef QMUD_ENCODINGUTILS_H
#define QMUD_ENCODINGUTILS_H

// ReSharper disable once CppUnusedIncludeDirective
#include <QByteArray>
// ReSharper disable once CppUnusedIncludeDirective
#include <QByteArrayView>
#include <QString>

/**
 * @brief Encodes bytes into Base64 text, optionally wrapped.
 * @param plaintext Source bytes.
 * @param multiLine Wrap output across multiple lines when `true`.
 * @return Base64-encoded text.
 */
QString qmudEncodeBase64Text(const QByteArray &plaintext, bool multiLine);
/**
 * @brief Encodes C-string input into Base64 text, optionally wrapped.
 * @param plaintext Source C-string.
 * @param multiLine Wrap output across multiple lines when `true`.
 * @return Base64-encoded text.
 */
QString qmudEncodeBase64Text(const char *plaintext, bool multiLine);
/**
 * @brief Decodes bytes as Windows-1252 text.
 * @param bytes Input byte sequence.
 * @return Decoded Unicode text.
 */
QString qmudDecodeWindows1252(QByteArrayView bytes);
/**
 * @brief Decodes UTF-8 stream bytes and falls back to Windows-1252 for invalid bytes.
 * @param bytes Incoming stream bytes.
 * @param carry Incomplete trailing UTF-8 bytes carried across calls.
 * @param hadInvalidBytes Optional output flag set when invalid UTF-8 bytes were recovered.
 * @return Decoded Unicode text.
 */
QString qmudDecodeUtf8WithWindows1252Fallback(QByteArrayView bytes, QByteArray &carry, bool *hadInvalidBytes);

#endif // QMUD_ENCODINGUTILS_H
