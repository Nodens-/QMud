/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MdiTabs.cpp
 * Role: MDI tab strip behavior for creating, selecting, and synchronizing tabs with active world child windows.
 */

#include "MdiTabs.h"

#include "WorldChildWindow.h"
#include "WorldRuntime.h"
#include <QMdiArea>
#include <QMenu>
#include <QSignalBlocker>

MdiTabs::MdiTabs()
{
	setExpanding(false);
	connect(this, &QTabBar::currentChanged, this, &MdiTabs::onCurrentChanged);
	connect(this, &QTabBar::tabMoved, this, &MdiTabs::onTabMoved);
}

void MdiTabs::create(QMdiArea *mdiArea, int style)
{
	m_mdiArea         = mdiArea;
	m_tabsOnTop       = (style & kMdiTabsTop) != 0;
	m_minVisibleViews = (style & kMdiTabsHideLt2Views) ? 2 : 1;
	m_showTabIcons    = (style & kMdiTabsImages) != 0;

	setDrawBase(false);
	setMovable(true);
	setTabsClosable(false);
	setElideMode(Qt::ElideRight);
	setDocumentMode(true);
	setShape(m_tabsOnTop ? QTabBar::RoundedNorth : QTabBar::RoundedSouth);
}

void MdiTabs::updateTabs()
{
	if (!m_mdiArea)
		return;

	const int               previousCurrentIndex = currentIndex();
	QPointer<QMdiSubWindow> previousCurrentWindow;
	if (previousCurrentIndex >= 0 && previousCurrentIndex < m_orderedWindows.size())
		previousCurrentWindow = m_orderedWindows[previousCurrentIndex];

	setUpdatesEnabled(false);
	const QSignalBlocker         blockSignals(this);

	QMdiSubWindow               *activeWindow = m_mdiArea->activeSubWindow();
	const QList<QMdiSubWindow *> windows      = m_mdiArea->subWindowList(QMdiArea::CreationOrder);

	if (!m_userOrdered)
	{
		// Keep tab positions stable by default. Activation-history ordering causes
		// tabs to appear to swap when the active subwindow changes.
		m_orderedWindows.clear();
		for (QMdiSubWindow *sub : windows)
			m_orderedWindows.push_back(sub);
	}
	else
	{
		for (qsizetype i = m_orderedWindows.size(); i-- > 0;)
		{
			QPointer<QMdiSubWindow> sub = m_orderedWindows[i];
			if (!sub)
			{
				m_orderedWindows.removeAt(i);
				continue;
			}
			if (!windows.contains(sub))
				m_orderedWindows.removeAt(i);
		}

		for (QMdiSubWindow *sub : windows)
		{
			if (!m_orderedWindows.contains(sub))
				m_orderedWindows.push_back(sub);
		}
	}

	while (count() > 0)
		removeTab(0);

	auto tabTextForWindow = [](QMdiSubWindow *sub) -> QString
	{
		QString text = sub ? sub->windowTitle() : QString();

		if (auto *world = qobject_cast<WorldChildWindow *>(sub))
		{
			if (WorldRuntime *runtime = world->runtime())
			{
				QString customTitle = runtime->windowTitleOverride();
				if (customTitle.isEmpty())
					customTitle = runtime->worldAttributes().value(QStringLiteral("window_title"));
				if (!customTitle.isEmpty())
					text = customTitle;
				else
				{
					const QString worldName = runtime->worldAttributes().value(QStringLiteral("name"));
					if (!worldName.isEmpty())
						text = worldName;
				}

				if (!runtime->doNotShowOutstandingLines())
				{
					const int newLines = runtime->newLines();
					if (newLines > 0)
						text += QStringLiteral(" (%1)").arg(newLines);
				}
			}
		}
		else if (auto *textWindow = qobject_cast<TextChildWindow *>(sub))
		{
			auto *editor = qobject_cast<QPlainTextEdit *>(textWindow->widget());
			if (editor && editor->document() && editor->document()->isModified() && !editor->isReadOnly())
				text += QStringLiteral(" *");
		}

		text.replace(QStringLiteral("&"), QStringLiteral("&&"));
		return text;
	};

	int         activeTabIndex   = -1;
	int         previousTabIndex = -1;
	const auto &orderedWindows   = m_orderedWindows;
	for (QMdiSubWindow *sub : orderedWindows)
	{
		if (!sub)
			continue;

		QString text = tabTextForWindow(sub);
		if (text.isEmpty())
			text = QStringLiteral("(untitled)");

		const int index = m_showTabIcons ? addTab(sub->windowIcon(), text) : addTab(text);
		if (sub == activeWindow)
			activeTabIndex = index;
		if (previousCurrentWindow && sub == previousCurrentWindow)
			previousTabIndex = index;
	}

	if (activeTabIndex >= 0)
		setCurrentIndex(activeTabIndex);
	else if (previousTabIndex >= 0)
		setCurrentIndex(previousTabIndex);
	else if (count() > 0)
		setCurrentIndex(0);

	const bool showTabs = count() >= m_minVisibleViews;
	if (!showTabs && isVisible())
		setVisible(false);
	else if (showTabs && !isVisible())
		setVisible(true);

	setUpdatesEnabled(true);
	update();
}

QList<QMdiSubWindow *> MdiTabs::orderedWindows() const
{
	QList<QMdiSubWindow *> ordered;
	ordered.reserve(m_orderedWindows.size());
	for (const QPointer<QMdiSubWindow> &sub : m_orderedWindows)
	{
		if (sub)
			ordered.push_back(sub);
	}
	return ordered;
}

void MdiTabs::onCurrentChanged(int index)
{
	if (!m_mdiArea)
		return;
	if (index < 0 || index >= count())
		return;

	if (index >= m_orderedWindows.size())
		return;

	if (QMdiSubWindow *sub = m_orderedWindows[index])
		m_mdiArea->setActiveSubWindow(sub);
}

void MdiTabs::onTabMoved(int from, int to)
{
	if (from < 0 || to < 0 || from >= m_orderedWindows.size() || to >= m_orderedWindows.size())
		return;

	m_orderedWindows.move(from, to);
	m_userOrdered = true;
}

void MdiTabs::contextMenuEvent(QContextMenuEvent *event)
{
	if (!m_mdiArea)
		return;

	const int index = tabAt(event->pos());
	if (index < 0)
		return;

	if (index >= m_orderedWindows.size())
		return;

	QMdiSubWindow *sub = m_orderedWindows[index];
	if (!sub)
		return;

	QMenu    menu;
	QAction *actRestore = menu.addAction(QStringLiteral("Restore"));
	QAction *actMove    = menu.addAction(QStringLiteral("Move"));
	QAction *actSize    = menu.addAction(QStringLiteral("Size"));
	QAction *actMin     = menu.addAction(QStringLiteral("Minimize"));
	QAction *actMax     = menu.addAction(QStringLiteral("Maximize"));
	menu.addSeparator();
	QAction *actClose = menu.addAction(QStringLiteral("Close"));

	actMove->setEnabled(false);
	actSize->setEnabled(false);
	actRestore->setEnabled(sub->windowState() & Qt::WindowMaximized);

	QAction *chosen = menu.exec(event->globalPos());
	if (!chosen)
		return;
	if (chosen == actRestore)
	{
		setCurrentIndex(index);
		m_mdiArea->setActiveSubWindow(sub);
		sub->showNormal();
	}
	else if (chosen == actMin)
	{
		setCurrentIndex(index);
		m_mdiArea->setActiveSubWindow(sub);
		sub->showMinimized();
	}
	else if (chosen == actMax)
	{
		setCurrentIndex(index);
		m_mdiArea->setActiveSubWindow(sub);
		sub->showMaximized();
	}
	else if (chosen == actClose)
	{
		// Close exactly the tab that was right-clicked, even if active tab changed.
		sub->close();
	}
}

void MdiTabs::mouseDoubleClickEvent(QMouseEvent *event)
{
	if (!m_mdiArea)
		return;

	const int index = tabAt(event->pos());
	if (index < 0)
		return;

	if (index >= m_orderedWindows.size())
		return;

	if (QMdiSubWindow *sub = m_orderedWindows[index])
		sub->showMaximized();
}

void MdiTabs::mouseReleaseEvent(QMouseEvent *event)
{
	if (event && event->button() == Qt::MiddleButton && m_mdiArea)
	{
		const int index = tabAt(event->pos());
		if (index >= 0 && index < m_orderedWindows.size())
		{
			if (QMdiSubWindow *sub = m_orderedWindows[index])
			{
				sub->close();
				event->accept();
				return;
			}
		}
	}

	QTabBar::mouseReleaseEvent(event);
}
