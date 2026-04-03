/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MainFrameMdiUtils.h
 * Role: Pure helpers for MDI active-window fallback and non-activating add restore behavior.
 */

#ifndef QMUD_MAINFRAMEMDIUTILS_H
#define QMUD_MAINFRAMEMDIUTILS_H

class QMdiSubWindow;
template <typename T> class QList;

namespace QMudMainFrameMdiUtils
{
	/**
	 * @brief Resolves the effective active subwindow using current active first, then last active fallback.
	 * @param active Current active subwindow from QMdiArea.
	 * @param lastActive Last known active subwindow tracked by MainWindow.
	 * @param creationOrder Current QMdiArea creation-order window list.
	 * @return Active/fallback subwindow when valid, otherwise `nullptr`.
	 */
	QMdiSubWindow *resolveCurrentOrLastActiveSubWindow(QMdiSubWindow *active, QMdiSubWindow *lastActive,
	                                                   const QList<QMdiSubWindow *> &creationOrder);

	/**
	 * @brief Resolves which subwindow should be restored after adding a window with activation disabled.
	 * @param active Current active subwindow from QMdiArea.
	 * @param lastActive Last known active subwindow tracked by MainWindow.
	 * @param creationOrder Current QMdiArea creation-order window list.
	 * @param addedSubWindow Newly added subwindow.
	 * @return Restore target, or `nullptr` when no restore is needed.
	 */
	QMdiSubWindow *resolveBackgroundAddRestoreTarget(QMdiSubWindow *active, QMdiSubWindow *lastActive,
	                                                 const QList<QMdiSubWindow *> &creationOrder,
	                                                 const QMdiSubWindow          *addedSubWindow);
} // namespace QMudMainFrameMdiUtils

#endif // QMUD_MAINFRAMEMDIUTILS_H
