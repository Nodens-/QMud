/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MainFrameActionUtils.h
 * Role: Pure helpers for action ids/tooltips used by main-frame world-slot toolbar wiring.
 */

#ifndef QMUD_MAINFRAMEACTIONUTILS_H
#define QMUD_MAINFRAMEACTIONUTILS_H

class QString;

namespace QMudMainFrameActionUtils
{
	/**
	 * @brief Builds command id string for one world-toolbar slot.
	 * @param slot 1-based slot index.
	 * @return Command name used by action dispatch.
	 */
	QString worldCommandNameForSlot(int slot);

	/**
	 * @brief Builds user-facing toolbar tooltip text for one world slot.
	 * @param slot 1-based slot index.
	 * @return Tooltip string including shortcut hint when available.
	 */
	QString worldButtonTooltipForSlot(int slot);
} // namespace QMudMainFrameActionUtils

#endif // QMUD_MAINFRAMEACTIONUTILS_H
