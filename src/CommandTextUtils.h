/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: CommandTextUtils.h
 * Role: Pure text/escaping helpers for command send-target transformations.
 */

#ifndef QMUD_COMMANDTEXTUTILS_H
#define QMUD_COMMANDTEXTUTILS_H

#include <QString>

namespace QMudCommandText
{
	/**
	 * @brief Expands escaped control sequences in command text.
	 * @param source Raw command text with escape sequences.
	 * @return Command text with escapes normalized for send path.
	 */
	QString fixupEscapeSequences(const QString &source);

	/**
	 * @brief Applies wildcard replacement/post-processing for send-target transforms.
	 * @param wildcard Wildcard value to process.
	 * @param makeLowerCase Force lowercase conversion when `true`.
	 * @param sendTo Active send-target mode identifier.
	 * @param language Active language option used by casing rules.
	 * @return Processed wildcard text.
	 */
	QString fixWildcard(const QString &wildcard, bool makeLowerCase, int sendTo, const QString &language);
} // namespace QMudCommandText

#endif // QMUD_COMMANDTEXTUTILS_H
