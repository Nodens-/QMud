/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: EncodingUtils.h
 * Role: Text/binary encoding helper interfaces used by runtime serialization and script-visible utility APIs.
 */

#ifndef QMUD_ENCODINGUTILS_H
#define QMUD_ENCODINGUTILS_H

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

#endif // QMUD_ENCODINGUTILS_H
