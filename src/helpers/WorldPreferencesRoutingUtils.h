/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldPreferencesRoutingUtils.h
 * Role: Pure routing helpers for resolving world-preferences dialog commands to initial page selections.
 */

#ifndef QMUD_WORLDPREFERENCESROUTINGUTILS_H
#define QMUD_WORLDPREFERENCESROUTINGUTILS_H

#include "dialogs/WorldPreferencesDialog.h"

#include <functional>

class QString;

namespace QMudWorldPreferencesRouting
{
	using CommandMatcher = std::function<bool(const QString &)>;

	/**
	 * @brief Returns whether command should route to WorldPreferences dialog.
	 * @param cmdName Triggered command name.
	 * @param isCommand Predicate compatible with AppController command-id alias checks.
	 * @return `true` when command should open world preferences.
	 */
	bool isPreferencesCommand(const QString &cmdName, const CommandMatcher &isCommand);

	/**
	 * @brief Resolves world preferences initial page for a command.
	 * @param cmdName Triggered command name.
	 * @param lastPreferencesPage Last persisted page index from runtime.
	 * @param isCommand Predicate compatible with AppController command-id alias checks.
	 * @return Initial page enum.
	 */
	WorldPreferencesDialog::Page initialPageForCommand(const QString &cmdName, int lastPreferencesPage,
	                                                   const CommandMatcher &isCommand);
} // namespace QMudWorldPreferencesRouting

#endif // QMUD_WORLDPREFERENCESROUTINGUTILS_H
