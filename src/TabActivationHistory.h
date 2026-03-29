/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: TabActivationHistory.h
 * Role: Tracks MDI activation history and computes close-fallback tab selection.
 */

#ifndef QMUD_TABACTIVATIONHISTORY_H
#define QMUD_TABACTIVATIONHISTORY_H

#include <QHash>
#include <QList>
#include <QPointer>
#include <QVector>

class QObject;
class QMdiSubWindow;

/**
 * @brief Tracks recent MDI activation order for close-fallback restoration.
 *
 * Maintains a compact activation history and maps closing windows to the
 * immediate previous active candidate so callers can restore focus after close.
 */
class TabActivationHistory
{
	public:
		/**
		 * @brief Records current active subwindow in activation history.
		 * @param window Newly activated subwindow.
		 * @param currentWindows Current valid subwindow set.
		 */
		void onActivated(QMdiSubWindow *window, const QList<QMdiSubWindow *> &currentWindows);
		/**
		 * @brief Captures fallback candidate when an active subwindow starts closing.
		 * @param closingSubWindow Subwindow being closed.
		 * @param activeSubWindow Current active subwindow.
		 * @param currentWindows Current valid subwindow set.
		 */
		void onCloseEvent(QMdiSubWindow *closingSubWindow, const QMdiSubWindow *activeSubWindow,
		                  const QList<QMdiSubWindow *> &currentWindows);
		/**
		 * @brief Finalizes close flow and returns stored fallback, if still valid.
		 * @param destroyedObject Destroyed subwindow object.
		 * @param currentWindows Current valid subwindow set.
		 * @return Fallback subwindow pointer, or `nullptr`.
		 */
		[[nodiscard]] QPointer<QMdiSubWindow>
		takeFallbackOnDestroyed(QObject *destroyedObject, const QList<QMdiSubWindow *> &currentWindows);

	private:
		void                                      pruneHistory(const QList<QMdiSubWindow *> &currentWindows);

		QVector<QPointer<QMdiSubWindow>>          m_activationHistory;
		QHash<QObject *, QPointer<QMdiSubWindow>> m_closeFallbackByWindow;
};

#endif // QMUD_TABACTIVATIONHISTORY_H
