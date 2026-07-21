/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: ShortcutPreferenceUtils.h
 * Role: Shared shortcut preference definitions, parsing, validation, and key-event matching.
 */

#ifndef QMUD_SHORTCUTPREFERENCEUTILS_H
#define QMUD_SHORTCUTPREFERENCEUTILS_H

#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QString>

class QKeyEvent;

namespace QMudShortcutPreferenceUtils
{
	/**
	 * @brief One configurable global shortcut definition.
	 */
	struct ShortcutDefinition
	{
			QString             id;            ///< Stable shortcut/action identifier.
			QString             preferenceKey; ///< Global preference key used for override persistence.
			QString             category;      ///< User-facing category name.
			QString             label;         ///< User-facing action label.
			QList<QKeySequence> defaults;      ///< Built-in default shortcuts.
	};

	/**
	 * @brief Returns all configurable global shortcut definitions.
	 * @return Ordered shortcut definition list.
	 */
	[[nodiscard]] const QList<ShortcutDefinition> &shortcutDefinitions();
	/**
	 * @brief Finds a shortcut definition by id.
	 * @param id Stable shortcut/action identifier.
	 * @return Matching definition, or `nullptr`.
	 */
	[[nodiscard]] const ShortcutDefinition        *definitionForId(const QString &id);
	/**
	 * @brief Finds a shortcut definition by global preference key.
	 * @param preferenceKey Global preference key.
	 * @return Matching definition, or `nullptr`.
	 */
	[[nodiscard]] const ShortcutDefinition        *definitionForPreferenceKey(const QString &preferenceKey);
	/**
	 * @brief Returns whether key is a registered shortcut preference key.
	 * @param preferenceKey Global preference key.
	 * @return `true` when key belongs to a shortcut definition.
	 */
	[[nodiscard]] bool                             isShortcutPreferenceKey(const QString &preferenceKey);
	/**
	 * @brief Parses semicolon-separated portable shortcut text.
	 * @param text Stored/user-entered shortcut list.
	 * @return Parsed shortcut list. Invalid entries are returned as empty sequences.
	 */
	[[nodiscard]] QList<QKeySequence>              parseShortcutList(const QString &text);
	/**
	 * @brief Formats shortcut list as semicolon-separated portable text.
	 * @param shortcuts Shortcut list.
	 * @return Portable persistence/display text.
	 */
	[[nodiscard]] QString shortcutListToPortableText(const QList<QKeySequence> &shortcuts);
	/**
	 * @brief Formats shortcut list as semicolon-separated native text.
	 * @param shortcuts Shortcut list.
	 * @return Native display text.
	 */
	[[nodiscard]] QString shortcutListToNativeText(const QList<QKeySequence> &shortcuts);
	/**
	 * @brief Resolves accepted explicit override ownership from override strings.
	 * @param overrideTextByPreferenceKey Override text keyed by global shortcut preference key.
	 * @return Accepted override owners keyed by portable shortcut text, with action id as value.
	 */
	[[nodiscard]] QHash<QString, QString>
	acceptedOverrideOwnersByPortableShortcut(const QHash<QString, QString> &overrideTextByPreferenceKey);
	/**
	 * @brief Resolves all effective shortcuts using AppController global preferences.
	 * @return Shortcut lists keyed by stable shortcut/action identifier.
	 */
	[[nodiscard]] QHash<QString, QList<QKeySequence>> effectiveShortcutMapForAppPreferences();
	/**
	 * @brief Returns whether sequence is reserved for per-world macro slots.
	 * @param sequence Shortcut sequence.
	 * @return `true` when the sequence must not be assigned globally.
	 */
	[[nodiscard]] bool                                isReservedMacroShortcut(const QKeySequence &sequence);
	/**
	 * @brief Returns whether event matches a sequence exactly.
	 * @param event Key event.
	 * @param sequence Shortcut sequence.
	 * @return `true` when event matches sequence.
	 */
	[[nodiscard]] bool eventMatchesShortcut(const QKeyEvent *event, const QKeySequence &sequence);
	/**
	 * @brief Returns whether event matches any sequence in a list.
	 * @param event Key event.
	 * @param shortcuts Shortcut list.
	 * @return `true` when event matches one list entry.
	 */
	[[nodiscard]] bool eventMatchesAnyShortcut(const QKeyEvent *event, const QList<QKeySequence> &shortcuts);
	/**
	 * @brief Returns default shortcuts for id.
	 * @param id Stable shortcut/action identifier.
	 * @return Default shortcut list, or empty list.
	 */
	[[nodiscard]] QList<QKeySequence> defaultShortcutsForId(const QString &id);
	/**
	 * @brief Returns effective shortcuts for id using AppController global preferences when available.
	 * @param id Stable shortcut/action identifier.
	 * @return Effective shortcut list, or defaults when no override exists.
	 */
	[[nodiscard]] QList<QKeySequence> effectiveShortcutsForId(const QString &id);
	/**
	 * @brief Returns whether key event matches an effective shortcut id.
	 * @param event Key event.
	 * @param id Stable shortcut/action identifier.
	 * @return `true` when event matches effective shortcuts for id.
	 */
	[[nodiscard]] bool                eventMatchesShortcutId(const QKeyEvent *event, const QString &id);
} // namespace QMudShortcutPreferenceUtils

#endif // QMUD_SHORTCUTPREFERENCEUTILS_H
