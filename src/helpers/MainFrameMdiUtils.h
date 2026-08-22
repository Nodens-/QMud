/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MainFrameMdiUtils.h
 * Role: Helpers for main-frame MDI layout, state, and shutdown preparation behavior.
 */

#ifndef QMUD_MAINFRAMEMDIUTILS_H
#define QMUD_MAINFRAMEMDIUTILS_H

class QMdiArea;
class QMdiSubWindow;
class QString;
template <typename T> class QList;

#include <QtGlobal>
#include <functional>

namespace QMudMainFrameMdiUtils
{
	/**
	 * @brief Direction in which MDI child windows are arranged as non-overlapping tiles.
	 */
	enum class TileDirection
	{
		Horizontal,
		Vertical
	};

	/**
	 * @brief Tiles visible, non-minimized MDI child windows while leaving minimized children unchanged.
	 * @param mdiArea MDI area whose child windows should be tiled.
	 * @param direction Direction in which the tiles should be arranged.
	 */
	void           tileSubWindows(const QMdiArea *mdiArea, TileDirection direction);

	/**
	 * @brief Restores selected non-minimized maximized children without changing deliberately minimized children.
	 * @param mdiArea MDI area that owns the candidate child windows.
	 * @param windows Candidate child windows to restore.
	 */
	void           restoreMaximizedSubWindows(const QMdiArea *mdiArea, const QList<QMdiSubWindow *> &windows);

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

	/**
	 * @brief Checks whether an MDI subwindow is related to a world runtime identity.
	 * @param window Subwindow to inspect.
	 * @param ownerToken Runtime identity token; `0` means no token constraint.
	 * @param ownerWorldId Runtime world id; empty means no world-id constraint.
	 * @param acceptUnowned Accept windows with no runtime identity properties when `true`.
	 * @return `true` when the subwindow identity is compatible with the runtime identity.
	 */
	[[nodiscard]] bool windowMatchesRuntimeIdentity(const QMdiSubWindow *window, qulonglong ownerToken,
	                                                const QString &ownerWorldId, bool acceptUnowned);

	/**
	 * @brief Resolves first subwindow matching a world runtime identity.
	 * @param windows Candidate subwindows in desired priority order.
	 * @param ownerToken Runtime identity token; `0` means no token constraint.
	 * @param ownerWorldId Runtime world id; empty means no world-id constraint.
	 * @param acceptUnowned Accept windows with no runtime identity properties when `true`.
	 * @return First matching subwindow, or `nullptr`.
	 */
	[[nodiscard]] QMdiSubWindow *firstWindowMatchingRuntimeIdentity(const QList<QMdiSubWindow *> &windows,
	                                                                qulonglong                    ownerToken,
	                                                                const QString &ownerWorldId,
	                                                                bool           acceptUnowned);

	/**
	 * @brief Runs open-world persistence before child windows are closed.
	 * @param saveOpenWorldState Persistence callback, or empty when no controller is available.
	 * @param errorMessage Optional output error text when persistence fails.
	 * @return `true` when shutdown can continue to child-window close.
	 */
	[[nodiscard]] bool
	prepareOpenWorldStateBeforeChildClose(const std::function<bool(QString *)> &saveOpenWorldState,
	                                      QString                              *errorMessage = nullptr);
} // namespace QMudMainFrameMdiUtils

#endif // QMUD_MAINFRAMEMDIUTILS_H
