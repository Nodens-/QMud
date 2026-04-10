/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: HyperlinkActionUtils.h
 * Role: Stateless helpers for decoding hyperlink action text and selecting hyperlink dispatch behavior.
 */

#ifndef QMUD_HYPERLINKACTIONUTILS_H
#define QMUD_HYPERLINKACTIONUTILS_H

#include "WorldRuntime.h"

#include <QString>
// ReSharper disable once CppUnusedIncludeDirective
#include <QVector>

/**
 * @brief Parsed Mushclient-compatible plugin callback hyperlink action.
 */
struct PluginHyperlinkCall
{
		QString pluginId; ///< 24-hex plugin identifier.
		QString routine;  ///< Callback routine name.
		QString argument; ///< Raw argument payload inside parentheses.
};

/**
 * @brief Hyperlink dispatch policy for one activated action.
 */
enum class MxpHyperlinkDispatchPolicy
{
	DirectSend,        ///< Send directly to world (no alias/command processing).
	PromptInput,       ///< Fill command input box (MXP prompt behavior).
	CommandProcessing, ///< Route through command processor (aliases/scripts enabled).
};

/**
 * @brief Decodes MXP action payload text (percent-decoding + common HTML entities).
 * @param text Raw action text from span/href payload.
 * @return Decoded action text.
 */
[[nodiscard]] QString decodeMxpActionText(QString text);

/**
 * @brief Returns first executable send action from a potentially piped MXP action string.
 * @param href Decoded href/action text.
 * @return First non-empty action segment (or trimmed input when no segments found).
 */
[[nodiscard]] QString firstMxpSendAction(const QString &href);

/**
 * @brief Parses `!!pluginid:routine(argument)` hyperlink callback payloads.
 * @param action Decoded send action text.
 * @param parsed Parsed callback fields when function returns `true`.
 * @return `true` when payload matches Mushclient callback syntax.
 */
[[nodiscard]] bool    parsePluginHyperlinkCall(const QString &action, PluginHyperlinkCall &parsed);

/**
 * @brief Chooses hyperlink dispatch policy using rendered line/span context.
 * @param lines Runtime line buffer.
 * @param normalizedHref Decoded/trimmed activated href/action.
 * @return Selected dispatch policy.
 */
[[nodiscard]] MxpHyperlinkDispatchPolicy
resolveMxpHyperlinkDispatchPolicy(const QVector<WorldRuntime::LineEntry> &lines,
                                  const QString                          &normalizedHref);

#endif // QMUD_HYPERLINKACTIONUTILS_H
