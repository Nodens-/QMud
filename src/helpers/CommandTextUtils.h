/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: CommandTextUtils.h
 * Role: Pure text/escaping helpers for command send-target transformations.
 */

#ifndef QMUD_COMMANDTEXTUTILS_H
#define QMUD_COMMANDTEXTUTILS_H

// ReSharper disable once CppUnusedIncludeDirective
#include <QString>
#include <QStringList>

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

	/**
	 * @brief Normalizes one trigger match line according to regexp/wildcard mode.
	 * @param line Incoming line text.
	 * @param preserveTrailingWhitespace Keep trailing spaces/tabs when `true`.
	 * @return Normalized trigger match target line.
	 */
	QString normalizeTriggerMatchLine(const QString &line, bool preserveTrailingWhitespace);

	/**
	 * @brief Builds multiline trigger target text from recent lines.
	 * @param recentLines Recent line history in chronological order.
	 * @param preserveTrailingWhitespace Keep trailing spaces/tabs per line when `true`.
	 * @return Multiline target text joined with trailing `\n` per line.
	 */
	QString buildTriggerMultilineTarget(const QStringList &recentLines, bool preserveTrailingWhitespace);
} // namespace QMudCommandText

#endif // QMUD_COMMANDTEXTUTILS_H
