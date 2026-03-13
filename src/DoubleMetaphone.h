/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: DoubleMetaphone.h
 * Role: Qt-friendly phonetic matching helpers so runtime/script code can use Double Metaphone results.
 */

#ifndef QMUD_DOUBLEMETAPHONE_H
#define QMUD_DOUBLEMETAPHONE_H

#include <QString>

[[nodiscard]]
/**
 * @brief Computes primary/secondary Double Metaphone codes.
 * @param input Source text.
 * @param length Maximum output code length.
 * @return Pair of primary and secondary metaphone codes.
 */
QPair<QString, QString> qmudDoubleMetaphone(const QString &input, int length);

#endif // QMUD_DOUBLEMETAPHONE_H
