/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: BraceMatch.h
 * Role: Brace-matching interfaces used by editors/input controls to locate matching delimiters in text buffers.
 */

#ifndef QMUD_BRACEMATCH_H
#define QMUD_BRACEMATCH_H

#include <QPlainTextEdit>

namespace QMudBraceMatch
{
	/**
	 * @brief Finds matching brace around cursor and optionally selects range.
	 * @param edit Target plain-text editor.
	 * @param selectRange Select brace range when `true`.
	 * @param flags Matching behavior flags.
	 * @return `true` when matching brace is found.
	 */
	bool findMatchingBrace(QPlainTextEdit *edit, bool selectRange, int flags);
} // namespace QMudBraceMatch

#endif // QMUD_BRACEMATCH_H
