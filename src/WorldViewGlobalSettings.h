/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldViewGlobalSettings.h
 * Role: Shared typed application settings consumed by every world presentation.
 */

#ifndef QMUD_WORLDVIEWGLOBALSETTINGS_H
#define QMUD_WORLDVIEWGLOBALSETTINGS_H

#include <QString>

/**
 * @brief Application-wide presentation and interaction settings shared by all world views.
 */
struct WorldViewGlobalSettings
{
		QString            wordDelimiters;
		QString            doubleClickWordDelimiters;
		bool               smoothScrolling{false};
		bool               smootherScrolling{false};
		bool               allTypingToCommandWindow{false};
		bool               bleedBackground{false};
		int                splitViewDividerWidth{5};

		/**
		 * @brief Compares all shared view settings.
		 * @return `true` when every setting has the same value.
		 */
		[[nodiscard]] bool operator==(const WorldViewGlobalSettings &) const = default;
};

#endif // QMUD_WORLDVIEWGLOBALSETTINGS_H
