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
// ReSharper disable once CppUnusedIncludeDirective
#include <QList>
#include <QString>
#include <QStringDecoder>
#include <QStringList>

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
/**
 * @brief Returns the default encoding for non-UTF-8 world traffic.
 * @return Canonical Qt encoding name.
 */
QString qmudDefaultLegacyWorldEncodingName();
/**
 * @brief Returns the Qt-supported encodings suitable for manual world text selection.
 * @return Sorted legacy-safe encoding storage names.
 */
QStringList    qmudAvailableWorldTextEncodings();
/**
 * @brief Returns a user-facing legacy encoding label with language/region context.
 * @param encodingName Encoding name.
 * @return Display label containing the codec name and language/region hint.
 */
QString        qmudWorldTextEncodingDisplayName(const QString &encodingName);
/**
 * @brief Normalizes a world text encoding name to a supported Qt codec name.
 * @param name User or persisted encoding name.
 * @return Supported encoding name, or the default legacy encoding when invalid.
 */
QString        qmudNormalizeWorldTextEncodingName(const QString &name);
/**
 * @brief Creates a streaming decoder for a world text encoding.
 * @param encodingName Encoding name.
 * @return Decoder initialized for the normalized encoding.
 */
QStringDecoder qmudCreateWorldTextDecoder(const QString &encodingName);
/**
 * @brief Decodes world text bytes through an existing streaming decoder.
 * @param bytes Input bytes.
 * @param decoder Streaming decoder.
 * @param hadInvalidBytes Optional output flag set when conversion reports invalid bytes.
 * @return Decoded Unicode text.
 */
QString        qmudDecodeWorldText(QByteArrayView bytes, QStringDecoder &decoder, bool *hadInvalidBytes);
/**
 * @brief Decodes an isolated world text byte array using the requested encoding.
 * @param bytes Input bytes.
 * @param encodingName Encoding name.
 * @param hadInvalidBytes Optional output flag set when conversion reports invalid bytes.
 * @return Decoded Unicode text.
 */
QString qmudDecodeWorldTextIsolated(QByteArrayView bytes, const QString &encodingName, bool *hadInvalidBytes);
/**
 * @brief Encodes Unicode text for outbound world traffic using the requested encoding.
 * @param text Unicode text.
 * @param encodingName Encoding name.
 * @param hadInvalidCharacters Optional output flag set when conversion reports unmappable characters.
 * @return Encoded bytes.
 */
QByteArray qmudEncodeWorldText(const QString &text, const QString &encodingName, bool *hadInvalidCharacters);
/**
 * @brief Returns preferred TELNET CHARSET names for the selected world encoding mode.
 * @param useUtf8 Whether UTF-8 mode is active.
 * @param legacyEncodingName Encoding used when UTF-8 mode is inactive.
 * @return Preference list for matching server-offered CHARSET names.
 */
QList<QByteArray> qmudWorldTextTelnetCharsetNames(bool useUtf8, const QString &legacyEncodingName);

#endif // QMUD_ENCODINGUTILS_H
