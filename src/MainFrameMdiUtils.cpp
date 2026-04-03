/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MainFrameMdiUtils.cpp
 * Role: Pure helpers for MDI active-window fallback and non-activating add restore behavior.
 */

#include "MainFrameMdiUtils.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QList>
#include <QMdiSubWindow>

namespace QMudMainFrameMdiUtils
{
	QMdiSubWindow *resolveCurrentOrLastActiveSubWindow(QMdiSubWindow *active, QMdiSubWindow *lastActive,
	                                                   const QList<QMdiSubWindow *> &creationOrder)
	{
		if (active)
			return active;
		if (lastActive && creationOrder.contains(lastActive))
			return lastActive;
		return nullptr;
	}

	QMdiSubWindow *resolveBackgroundAddRestoreTarget(QMdiSubWindow *active, QMdiSubWindow *lastActive,
	                                                 const QList<QMdiSubWindow *> &creationOrder,
	                                                 const QMdiSubWindow          *addedSubWindow)
	{
		QMdiSubWindow *const target = resolveCurrentOrLastActiveSubWindow(active, lastActive, creationOrder);
		if (!target || target == addedSubWindow)
			return nullptr;
		return target;
	}
} // namespace QMudMainFrameMdiUtils
