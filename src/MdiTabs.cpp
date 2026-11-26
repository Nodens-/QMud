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
#include <QPainter>
#include <QSet>
#include <QSignalBlocker>
#include <QStyle>

namespace
{
	/** @brief Display-only tab badge that paints at the current device pixel ratio. */
	class MuteIndicator final : public QWidget
	{
		public:
			/**
			 * @brief Creates a mouse-transparent mute badge.
			 * @param parent Owning tab bar.
			 */
			explicit MuteIndicator(QWidget *parent) : QWidget(parent)
			{
				setAttribute(Qt::WA_TransparentForMouseEvents);
			}

			/**
			 * @brief Selects the icon to paint.
			 * @param icon Icon representing the current mute status.
			 */
			void setIcon(const QIcon &icon)
			{
				if (m_icon.cacheKey() == icon.cacheKey())
					return;
				m_icon = icon;
				update();
			}

		protected:
			/**
			 * @brief Paints the icon using the current paint device.
			 * @param event Widget paint event.
			 */
			void paintEvent(QPaintEvent *event) override
			{
				Q_UNUSED(event);
				QPainter painter(this);
				m_icon.paint(&painter, rect());
			}

		private:
			QIcon m_icon;
	};
} // namespace

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

	const QSignalBlocker         blockSignals(this);
	QMdiSubWindow               *activeWindow = m_mdiArea->activeSubWindow();
	const QList<QMdiSubWindow *> windows      = m_mdiArea->subWindowList(QMdiArea::CreationOrder);
	QSet<QMdiSubWindow *>        remaining(windows.cbegin(), windows.cend());

	for (int index = count(); index-- > 0;)
	{
		if (!remaining.contains(m_orderedWindows.at(index).data()))
		{
			m_orderedWindows.removeAt(index);
			removeTab(index);
		}
	}

	QList<QMdiSubWindow *> desiredOrder = windows;
	if (m_userOrdered)
	{
		desiredOrder.clear();
		for (const auto &window : std::as_const(m_orderedWindows))
		{
			desiredOrder.append(window.data());
			remaining.remove(window.data());
		}
		for (auto *window : windows)
			if (remaining.contains(window))
				desiredOrder.append(window);
	}

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

	int activeTabIndex   = -1;
	int previousTabIndex = -1;
	for (int index = 0; index < desiredOrder.size(); ++index)
	{
		auto   *sub  = desiredOrder.at(index);
		QString text = tabTextForWindow(sub);
		if (text.isEmpty())
			text = QStringLiteral("(untitled)");
		const QIcon icon = m_showTabIcons ? sub->windowIcon() : QIcon{};

		bool        inserted = false;
		if (index >= m_orderedWindows.size() || m_orderedWindows.at(index) != sub)
		{
			const qsizetype oldIndex =
			    index < m_orderedWindows.size() ? m_orderedWindows.indexOf(sub, index + 1) : -1;
			if (oldIndex >= 0)
			{
				m_orderedWindows.move(oldIndex, index);
				moveTab(static_cast<int>(oldIndex), index);
			}
			else
			{
				m_orderedWindows.insert(index, sub);
				insertTab(index, icon, text);
				inserted = true;
				if (auto *world = qobject_cast<WorldChildWindow *>(sub))
					connect(world, &WorldChildWindow::runtimeBindingChanged, this,
					        &MdiTabs::onWorldRuntimeBindingChanged, Qt::UniqueConnection);
				updateMuteIndicator(index, sub);
			}
		}
		if (!inserted)
		{
			if (tabText(index) != text)
				setTabText(index, text);
			if (tabIcon(index).cacheKey() != icon.cacheKey())
				setTabIcon(index, icon);
		}
		if (sub == activeWindow)
			activeTabIndex = index;
		if (sub == previousCurrentWindow)
			previousTabIndex = index;
	}

	if (activeTabIndex >= 0)
		setCurrentIndex(activeTabIndex);
	else if (previousTabIndex >= 0)
		setCurrentIndex(previousTabIndex);
	else if (count() > 0)
		setCurrentIndex(0);

	const bool showTabs =
	    m_mdiArea->testOption(QMdiArea::DontMaximizeSubWindowOnActivation) && count() >= m_minVisibleViews;
	if (!showTabs && isVisible())
		setVisible(false);
	else if (showTabs && !isVisible())
		setVisible(true);
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
		emit windowActivationRequested(sub);
}

void MdiTabs::activateTabWindow(const int index, QMdiSubWindow *const subWindow)
{
	if (!subWindow)
		return;

	const bool currentChanged = currentIndex() != index;
	setCurrentIndex(index);
	if (!currentChanged)
		emit windowActivationRequested(subWindow);
}

void MdiTabs::closeTabWindow(QMdiSubWindow *const subWindow)
{
	if (!m_mdiArea || !subWindow)
		return;

	const bool              deletesOnClose = subWindow->testAttribute(Qt::WA_DeleteOnClose);
	QPointer<QMdiSubWindow> closingWindow(subWindow);
	if (!subWindow->close())
		return;

	if (deletesOnClose && closingWindow && m_mdiArea->subWindowList().contains(closingWindow))
		m_mdiArea->removeSubWindow(closingWindow.data());
	updateTabs();
}

void MdiTabs::onTabMoved(int from, int to)
{
	if (from < 0 || to < 0 || from >= m_orderedWindows.size() || to >= m_orderedWindows.size())
		return;

	m_orderedWindows.move(from, to);
	m_userOrdered = true;
	emit windowOrderChanged();
}

void MdiTabs::updateMuteIndicator(const int index, QMdiSubWindow *subWindow)
{
	const auto *world   = qobject_cast<WorldChildWindow *>(subWindow);
	const auto *runtime = world ? world->runtime() : nullptr;

	const auto  session = runtime ? runtime->soundPlaybackSession() : nullptr;
	if (runtime)
		connect(runtime, &WorldRuntime::pluginMembershipChanged, this, &MdiTabs::onPluginMembershipChanged,
		        Qt::UniqueConnection);

	const bool worldMuted   = session && session->isMuted();
	const auto mutedIds     = session && !worldMuted ? session->mutedPluginIds() : QSet<QString>{};
	qsizetype  mutedPlugins = 0;
	if (!mutedIds.isEmpty())
	{
		const QStringList pluginIds = runtime->pluginIdList();
		for (const QString &id : pluginIds)
			mutedPlugins += mutedIds.contains(id);
	}
	auto *indicator = dynamic_cast<MuteIndicator *>(tabButton(index, QTabBar::RightSide));
	if (!worldMuted && mutedPlugins == 0)
	{
		if (indicator)
		{
			setTabButton(index, QTabBar::RightSide, nullptr);
			indicator->deleteLater();
			setTabToolTip(index, {});
		}
		return;
	}

	static const QIcon worldIcon(QStringLiteral(":/qmud/res/world_muted.png"));
	static const QIcon pluginsIcon(QStringLiteral(":/qmud/res/plugins_muted.png"));
	const int          extent = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
	const QSize        size(extent, extent);
	if (!indicator)
	{
		indicator = new MuteIndicator(this);
		indicator->setFixedSize(size);
		setTabButton(index, QTabBar::RightSide, indicator);
	}
	indicator->setIcon(worldMuted ? worldIcon : pluginsIcon);
	setTabToolTip(index, worldMuted          ? tr("World muted")
	                     : mutedPlugins == 1 ? tr("1 plugin muted")
	                                         : tr("%1 plugins muted").arg(mutedPlugins));
}

void MdiTabs::refreshMuteIndicators(const WorldRuntime *runtime)
{
	for (int index = 0; index < m_orderedWindows.size(); ++index)
	{
		auto *view = qobject_cast<WorldChildWindow *>(m_orderedWindows.at(index).data());
		if (view && view->runtime() == runtime)
			updateMuteIndicator(index, view);
	}
}

void MdiTabs::onPluginMembershipChanged()
{
	const QPointer<WorldRuntime> runtime(qobject_cast<WorldRuntime *>(sender()));
	QMetaObject::invokeMethod(
	    this,
	    [this, runtime]
	    {
		    if (runtime)
			    refreshMuteIndicators(runtime.data());
	    },
	    Qt::QueuedConnection);
}

void MdiTabs::onWorldRuntimeBindingChanged()
{
	auto           *window = qobject_cast<WorldChildWindow *>(sender());
	const qsizetype index  = m_orderedWindows.indexOf(window);
	if (window && index >= 0 && index < count())
		updateMuteIndicator(static_cast<int>(index), window);
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

	QPointer<QMdiSubWindow> sub = m_orderedWindows[index];
	if (!sub)
		return;

	QMenu                               menu;
	QPointer<WorldRuntime>              soundRuntime;
	std::shared_ptr<SoundEngineSession> soundSession;
	QAction                            *actMute        = nullptr;
	QMenu                              *pluginMuteMenu = nullptr;
	if (auto *world = qobject_cast<WorldChildWindow *>(sub.data()); world && world->runtime())
	{
		soundRuntime = world->runtime();
		soundSession = soundRuntime->soundPlaybackSession();
		actMute      = menu.addAction(QStringLiteral("Mute"));
		actMute->setCheckable(true);
		actMute->setEnabled(static_cast<bool>(soundSession));
		actMute->setChecked(soundSession && soundSession->isMuted());

		pluginMuteMenu = menu.addMenu(QStringLiteral("Mute Plugin"));
		if (soundSession)
		{
			QHash<QString, QString> pluginNames;
			for (const auto &plugin : soundRuntime->plugins())
				pluginNames.insert(plugin.attributes.value(QStringLiteral("id")),
				                   plugin.attributes.value(QStringLiteral("name")));

			const QStringList pluginIds = soundRuntime->pluginIdList();
			for (const QString &pluginId : pluginIds)
			{
				QString name = pluginNames.value(pluginId);
				if (name.isEmpty())
					name = soundRuntime->pluginInfo(pluginId, 1).toString();
				if (name.isEmpty())
					name = pluginId;
				name.replace(QLatin1Char('&'), QStringLiteral("&&"));
				QAction *action = pluginMuteMenu->addAction(name);
				action->setCheckable(true);
				action->setChecked(soundSession->isMuted(pluginId));
				action->setData(pluginId);
			}
		}
		pluginMuteMenu->setEnabled(!pluginMuteMenu->isEmpty());
		menu.addSeparator();
	}
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
	if (!chosen || !sub)
		return;
	const qsizetype storedIndex = m_orderedWindows.indexOf(sub);
	if (storedIndex < 0 || storedIndex >= count())
		return;
	const int currentIndex = static_cast<int>(storedIndex);
	if (chosen == actMute || (pluginMuteMenu && pluginMuteMenu->actions().contains(chosen)))
	{
		const auto *world = qobject_cast<WorldChildWindow *>(sub.data());
		if (!soundRuntime || !world || world->runtime() != soundRuntime || !soundSession ||
		    soundRuntime->soundPlaybackSession() != soundSession)
			return;
		if (chosen == actMute)
			soundSession->setMuted(chosen->isChecked());
		else if (const QString pluginId = chosen->data().toString();
		         soundRuntime->pluginIdList().contains(pluginId))
			soundSession->setMuted(chosen->isChecked(), pluginId);
		refreshMuteIndicators(soundRuntime);
		return;
	}
	if (chosen == actRestore)
	{
		activateTabWindow(currentIndex, sub.data());
		sub->showNormal();
	}
	else if (chosen == actMin)
	{
		activateTabWindow(currentIndex, sub.data());
		sub->showMinimized();
	}
	else if (chosen == actMax)
	{
		activateTabWindow(currentIndex, sub.data());
		sub->showMaximized();
	}
	else if (chosen == actClose)
	{
		// Close exactly the tab that was right-clicked, even if active tab changed.
		closeTabWindow(sub.data());
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
		activateTabWindow(index, sub);
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
				closeTabWindow(sub);
				event->accept();
				return;
			}
		}
	}

	QTabBar::mouseReleaseEvent(event);
}
