/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MdiTabs.h
 * Role: MDI tab-management interfaces that represent and control the tab strip for open world child windows.
 */

#ifndef QMUD_MDITABS_H
#define QMUD_MDITABS_H

#include <QPointer>
#include <QTabBar>
#include <QVector>

class QMdiArea;
class QMdiSubWindow;

// Styles for customizing the appearance of MdiTabs.
enum MdiTabStyle
{
	kMdiTabsBottom       = 0x0000, // places tabs at bottom (default)
	kMdiTabsTop          = 0x0001, // place tabs at top
	kMdiTabsImages       = 0x0002, // show images
	kMdiTabsHideLt2Views = 0x0004, // Hide Tabs when less than two views are open (default is one view)
	kMdiTabsToolTips     = 0x0008, // not implemented (a tooltip can appear about a tab)
	kMdiTabsButtons      = 0x0010, // not implemented (show tabs as buttons)
	kMdiTabsAutoSize     = 0x0020, // not implemented (tabs are sized to fit the entire width of the control)
	kMdiTabsTaskbar      = 0x0038  // kMdiTabsToolTips|kMdiTabsButtons|kMdiTabsAutoSize
};

/**
 * @brief Tab-bar facade synchronized with an MDI area.
 *
 * Mirrors active MDI child windows as tabs and applies legacy-style tab behavior flags.
 */
class MdiTabs : public QTabBar
{
		Q_OBJECT
	public:
		/**
		 * @brief Constructs an unbound MDI tabs control.
		 */
		MdiTabs();

		/**
		 * @brief Binds tab control to an MDI area and applies style flags.
		 * @param mdiArea Target MDI area to mirror.
		 * @param style Bitmask of `MdiTabStyle` flags.
		 */
		void create(QMdiArea *mdiArea, int style = kMdiTabsBottom | kMdiTabsImages);
		/**
		 * @brief Synchronizes tabs with current MDI child window set.
		 */
		void updateTabs(); // sync the tabctrl with all views
		/**
		 * @brief Returns current tab ordering as MDI windows.
		 * @return Ordered list of currently tracked subwindows.
		 */
		[[nodiscard]] QList<QMdiSubWindow *> orderedWindows() const;

		/**
		 * @brief Sets minimum number of views required before tabs are shown.
		 * @param minViews Minimum visible-view count before showing tabs.
		 */
		void                                 setMinViews(int minViews)
		{
			m_minVisibleViews = minViews;
		}

	private:
		QMdiArea                        *m_mdiArea{nullptr};
		int                              m_minVisibleViews{0}; // minimum number of views
		bool                             m_showTabIcons{false};
		bool                             m_tabsOnTop{false};
		bool                             m_userOrdered{false};
		QVector<QPointer<QMdiSubWindow>> m_orderedWindows;

	protected:
		/**
		 * @brief Shows context actions for the tab under cursor.
		 * @param event Context-menu event payload.
		 */
		void contextMenuEvent(QContextMenuEvent *event) override;
		/**
		 * @brief Handles tab double-click behavior.
		 * @param event Mouse event payload.
		 */
		void mouseDoubleClickEvent(QMouseEvent *event) override;
		/**
		 * @brief Handles tab activation/reordering interactions.
		 * @param event Mouse event payload.
		 */
		void mouseReleaseEvent(QMouseEvent *event) override;

	private slots:
		/**
		 * @brief Activates corresponding MDI child after current tab change.
		 * @param index Current tab index.
		 */
		void onCurrentChanged(int index);
		/**
		 * @brief Persists explicit user tab order after drag-move.
		 * @param from Source tab index.
		 * @param to Destination tab index.
		 */
		void onTabMoved(int from, int to);
};

#endif // QMUD_MDITABS_H
