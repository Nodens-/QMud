/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MainFrameActionUtils.cpp
 * Role: Pure helpers for action ids/tooltips used by main-frame world-slot toolbar wiring.
 */

#include "MainFrameActionUtils.h"

#include <QString>

namespace QMudMainFrameActionUtils
{
	QString worldCommandNameForSlot(const int slot)
	{
		return QStringLiteral("World%1").arg(slot);
	}

	QString worldButtonTooltipForSlot(const int slot)
	{
		if (slot >= 1 && slot <= 9)
			return QStringLiteral("Activates world #%1 (Ctrl+%1)").arg(slot);
		if (slot == 10)
			return QStringLiteral("Activates world #10 (Ctrl+0)");
		return QStringLiteral("Activates world #%1").arg(slot);
	}

	bool shouldAttemptIncomingLineTaskbarFlash(const bool worldFlashEnabled, const bool appFocused)
	{
		return worldFlashEnabled && !appFocused;
	}

	bool resolveIncomingLineFocusForFlash(const bool qtAppFocused, const bool windowFocused)
	{
		return qtAppFocused || windowFocused;
	}

	bool resolveIncomingLineFocusForActivitySound(const bool qtAppFocused, const bool windowFocused)
	{
		return qtAppFocused && windowFocused;
	}

	bool shouldRequestBackgroundTaskbarFlash(const bool appFocused, const bool flashAlreadyRequested)
	{
		return !appFocused && !flashAlreadyRequested;
	}

	bool shouldResetBackgroundFlashLatch(const bool previousFocused, const bool currentFocused)
	{
		return previousFocused != currentFocused;
	}
} // namespace QMudMainFrameActionUtils
