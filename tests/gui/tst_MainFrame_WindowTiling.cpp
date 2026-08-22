/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_MainFrame_WindowTiling.cpp
 * Role: GUI integration coverage for main-frame MDI tiling action dispatch and layout stability.
 */

#include "MainFrame.h"
#include "MainFrameMdiUtils.h"
#include "MdiTabs.h"
#include "StringUtils.h"
#include "WorldChildWindow.h"

#include <QAction>
// ReSharper disable once CppUnusedIncludeDirective
#include <QCoreApplication>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QWidget>
#include <QtTest/QTest>

namespace
{
	QMdiSubWindow *addSubWindow(MainWindow &mainWindow, const bool activate = false)
	{
		auto *const window = new QMdiSubWindow;
		window->setWidget(new QWidget);
		mainWindow.addMdiSubWindow(window, activate);
		return window;
	}

	/**
	 * @brief QTest fixture covering end-to-end main-frame window tiling actions.
	 */
	class tst_MainFrame_WindowTiling final : public QObject
	{
			Q_OBJECT

		private slots:
			static void reapplyingDisabledTabStylePreservesIndividualMaximize()
			{
				MainWindow  mainWindow;
				auto *const mdiArea = mainWindow.findChild<QMdiArea *>();
				auto *const tabs    = mainWindow.findChild<MdiTabs *>();
				QVERIFY(mdiArea);
				QVERIFY(tabs);

				mainWindow.resize(900, 700);
				mainWindow.show();
				mainWindow.setWindowTabsStyle(0);
				auto *const first  = addSubWindow(mainWindow, true);
				auto *const second = addSubWindow(mainWindow);
				second->showMaximized();
				QCoreApplication::processEvents();

				QVERIFY(!mainWindow.isTabbedWindowPresentationActive());
				QVERIFY(!first->isMaximized());
				QVERIFY(second->isMaximized());

				mainWindow.setWindowTabsStyle(0);
				QCoreApplication::processEvents();

				QVERIFY(!mainWindow.isTabbedWindowPresentationActive());
				QVERIFY(tabs->isHidden());
				QVERIFY(!first->isMaximized());
				QVERIFY(second->isMaximized());
			}

			static void reapplyingEnabledTabStylePreservesWindowedPresentation()
			{
				MainWindow  mainWindow;
				auto *const mdiArea = mainWindow.findChild<QMdiArea *>();
				auto *const tabs    = mainWindow.findChild<MdiTabs *>();
				QVERIFY(mdiArea);
				QVERIFY(tabs);

				mainWindow.resize(900, 700);
				mainWindow.show();
				mainWindow.setWindowTabsStyle(kMdiTabsTop);
				auto *const first  = addSubWindow(mainWindow, true);
				auto *const second = addSubWindow(mainWindow);
				QCoreApplication::processEvents();

				second->showNormal();
				QCoreApplication::processEvents();
				QVERIFY(!mainWindow.isTabbedWindowPresentationActive());
				QVERIFY(!first->isMaximized());
				QVERIFY(!second->isMaximized());

				mainWindow.setWindowTabsStyle(kMdiTabsTop);
				QCoreApplication::processEvents();
				QVERIFY(!mainWindow.isTabbedWindowPresentationActive());
				QVERIFY(tabs->isHidden());
				QVERIFY(!first->isMaximized());
				QVERIFY(!second->isMaximized());

				mainWindow.setWindowTabsStyle(kMdiTabsImages);
				QCoreApplication::processEvents();
				QVERIFY(!mainWindow.isTabbedWindowPresentationActive());
				QVERIFY(tabs->isHidden());
				QVERIFY(!first->isMaximized());
				QVERIFY(!second->isMaximized());
			}

			static void tabbedPresentationKeepsEveryChildMaximizedAcrossActivation()
			{
				MainWindow  mainWindow;
				auto *const mdiArea = mainWindow.findChild<QMdiArea *>();
				auto *const tabs    = mainWindow.findChild<MdiTabs *>();
				QVERIFY(mdiArea);
				QVERIFY(tabs);

				mainWindow.resize(900, 700);
				mainWindow.show();
				mainWindow.setWindowTabsStyle(kMdiTabsTop);
				QCoreApplication::processEvents();

				auto *const first  = addSubWindow(mainWindow, true);
				auto *const second = addSubWindow(mainWindow);
				QCoreApplication::processEvents();

				QVERIFY(mainWindow.isTabbedWindowPresentationActive());
				QVERIFY(mdiArea->testOption(QMdiArea::DontMaximizeSubWindowOnActivation));
				QVERIFY(tabs->isVisible());
				QVERIFY(first->isMaximized());
				QVERIFY(second->isMaximized());
				QCOMPARE(mdiArea->activeSubWindow(), first);

				QList<bool> updatesEnabledDuringActivation;
				QObject     activationConnectionContext;
				connect(mdiArea, &QMdiArea::subWindowActivated, &activationConnectionContext,
				        [mdiArea, second, &updatesEnabledDuringActivation](const QMdiSubWindow *activated)
				        {
					        if (activated == second)
						        updatesEnabledDuringActivation.push_back(mdiArea->updatesEnabled());
				        });
				const auto secondTabIndex = tabs->orderedWindows().indexOf(second);
				QVERIFY(secondTabIndex >= 0);
				QVERIFY(secondTabIndex < tabs->count());
				tabs->setCurrentIndex(static_cast<int>(secondTabIndex));
				QCoreApplication::processEvents();

				QCOMPARE(mdiArea->activeSubWindow(), second);
				QVERIFY(first->isMaximized());
				QVERIFY(second->isMaximized());
				QVERIFY(mdiArea->updatesEnabled());
				QVERIFY(!updatesEnabledDuringActivation.isEmpty());
				for (const bool updatesEnabled : updatesEnabledDuringActivation)
					QVERIFY(!updatesEnabled);
			}

			static void addingMaximizedChildAfterRestoreReconcilesPresentation()
			{
				MainWindow  mainWindow;
				auto *const mdiArea = mainWindow.findChild<QMdiArea *>();
				auto *const tabs    = mainWindow.findChild<MdiTabs *>();
				QVERIFY(mdiArea);
				QVERIFY(tabs);

				mainWindow.resize(900, 700);
				mainWindow.show();
				mainWindow.setWindowTabsStyle(kMdiTabsTop);
				auto *const first  = addSubWindow(mainWindow, true);
				auto *const second = addSubWindow(mainWindow);
				QCoreApplication::processEvents();

				second->showNormal();
				QCoreApplication::processEvents();
				QVERIFY(!mainWindow.isTabbedWindowPresentationActive());
				QVERIFY(!first->isMaximized());
				QVERIFY(!second->isMaximized());

				auto *const textWindow = new TextChildWindow;
				mainWindow.addMdiSubWindow(textWindow, true);
				QCoreApplication::processEvents();

				QVERIFY(mainWindow.isTabbedWindowPresentationActive());
				QVERIFY(tabs->isVisible());
				QVERIFY(first->isMaximized());
				QVERIFY(second->isMaximized());
				QVERIFY(textWindow->isMaximized());
			}

			static void addingMaximizedChildWithTabsDisabledStaysWindowed()
			{
				MainWindow  mainWindow;
				auto *const mdiArea = mainWindow.findChild<QMdiArea *>();
				auto *const tabs    = mainWindow.findChild<MdiTabs *>();
				QVERIFY(mdiArea);
				QVERIFY(tabs);

				mainWindow.resize(900, 700);
				mainWindow.show();
				mainWindow.setWindowTabsStyle(0);
				auto *const first      = addSubWindow(mainWindow, true);
				auto *const textWindow = new TextChildWindow;
				mainWindow.addMdiSubWindow(textWindow, true);
				QCoreApplication::processEvents();

				QVERIFY(!mainWindow.isTabbedWindowPresentationActive());
				QVERIFY(tabs->isHidden());
				QVERIFY(!first->isMaximized());
				QVERIFY(textWindow->isMaximized());
			}

			static void childStateChangesTransitionTheWholePresentation()
			{
				MainWindow  mainWindow;
				auto *const mdiArea = mainWindow.findChild<QMdiArea *>();
				auto *const tabs    = mainWindow.findChild<MdiTabs *>();
				QVERIFY(mdiArea);
				QVERIFY(tabs);

				mainWindow.resize(900, 700);
				mainWindow.show();
				mainWindow.setWindowTabsStyle(kMdiTabsTop);
				auto *const first          = addSubWindow(mainWindow, true);
				auto *const second         = addSubWindow(mainWindow);
				const auto  firstTabIndex  = tabs->orderedWindows().indexOf(first);
				const auto  secondTabIndex = tabs->orderedWindows().indexOf(second);
				QVERIFY(firstTabIndex >= 0);
				QVERIFY(secondTabIndex >= 0);
				QVERIFY(firstTabIndex < tabs->count());
				QVERIFY(secondTabIndex < tabs->count());
				tabs->setCurrentIndex(static_cast<int>(firstTabIndex));
				tabs->setCurrentIndex(static_cast<int>(secondTabIndex));
				QCoreApplication::processEvents();

				QVERIFY(second->isMaximized());
				QVERIFY(tabs->isVisible());
				second->showNormal();
				QCoreApplication::processEvents();

				QVERIFY(!mainWindow.isTabbedWindowPresentationActive());
				QVERIFY(tabs->isHidden());
				QVERIFY(!first->isMaximized());
				QVERIFY(!second->isMaximized());

				second->showMaximized();
				QCoreApplication::processEvents();

				QVERIFY(mainWindow.isTabbedWindowPresentationActive());
				QVERIFY(tabs->isVisible());
				QVERIFY(first->isMaximized());
				QVERIFY(second->isMaximized());

				second->showMinimized();
				QCoreApplication::processEvents();

				QVERIFY(!mainWindow.isTabbedWindowPresentationActive());
				QVERIFY(tabs->isHidden());
				QVERIFY(!first->isMaximized());
				QVERIFY(second->isMinimized());

				second->showMaximized();
				QCoreApplication::processEvents();

				QVERIFY(mainWindow.isTabbedWindowPresentationActive());
				QVERIFY(tabs->isVisible());
				QVERIFY(first->isMaximized());
				QVERIFY(second->isMaximized());
			}

			static void windowMenuRestoreAndMaximizeTransitionTheWholePresentation()
			{
				MainWindow  mainWindow;
				auto *const mdiArea = mainWindow.findChild<QMdiArea *>();
				auto *const tabs    = mainWindow.findChild<MdiTabs *>();
				QVERIFY(mdiArea);
				QVERIFY(tabs);

				mainWindow.resize(900, 700);
				mainWindow.show();
				mainWindow.setWindowTabsStyle(kMdiTabsTop);
				auto *const first  = addSubWindow(mainWindow);
				auto *const second = addSubWindow(mainWindow, true);
				QCoreApplication::processEvents();

				QAction *const restoreAction  = mainWindow.actionForCommand(QStringLiteral("WindowRestore"));
				QAction *const maximizeAction = mainWindow.actionForCommand(QStringLiteral("WindowMaximize"));
				QVERIFY(restoreAction);
				QVERIFY(maximizeAction);
				QCOMPARE(mdiArea->activeSubWindow(), second);

				restoreAction->trigger();
				QCoreApplication::processEvents();

				QVERIFY(!mainWindow.isTabbedWindowPresentationActive());
				QVERIFY(tabs->isHidden());
				QVERIFY(!first->isMaximized());
				QVERIFY(!second->isMaximized());

				maximizeAction->trigger();
				QCoreApplication::processEvents();

				QVERIFY(mainWindow.isTabbedWindowPresentationActive());
				QVERIFY(tabs->isVisible());
				QVERIFY(first->isMaximized());
				QVERIFY(second->isMaximized());
			}

			static void maximizingWithTabsDisabledAffectsOnlyTheRequestedWindow()
			{
				MainWindow  mainWindow;
				auto *const mdiArea = mainWindow.findChild<QMdiArea *>();
				auto *const tabs    = mainWindow.findChild<MdiTabs *>();
				QVERIFY(mdiArea);
				QVERIFY(tabs);

				mainWindow.resize(900, 700);
				mainWindow.show();
				mainWindow.setWindowTabsStyle(0);
				auto *const first  = addSubWindow(mainWindow);
				auto *const second = addSubWindow(mainWindow, true);
				QCoreApplication::processEvents();

				QAction *const maximizeAction = mainWindow.actionForCommand(QStringLiteral("WindowMaximize"));
				QVERIFY(maximizeAction);
				QCOMPARE(mdiArea->activeSubWindow(), second);
				maximizeAction->trigger();
				QCoreApplication::processEvents();

				QVERIFY(!mainWindow.isTabbedWindowPresentationActive());
				QVERIFY(tabs->isHidden());
				QVERIFY(!first->isMaximized());
				QVERIFY(second->isMaximized());
			}

			static void cascadeActionLeavesEveryChildWindowed()
			{
				MainWindow  mainWindow;
				auto *const mdiArea = mainWindow.findChild<QMdiArea *>();
				auto *const tabs    = mainWindow.findChild<MdiTabs *>();
				QVERIFY(mdiArea);
				QVERIFY(tabs);

				mainWindow.resize(900, 700);
				mainWindow.show();
				mainWindow.setWindowTabsStyle(kMdiTabsTop);
				const QList<QMdiSubWindow *> windows{addSubWindow(mainWindow, true), addSubWindow(mainWindow),
				                                     addSubWindow(mainWindow)};
				QCoreApplication::processEvents();

				QAction *const cascadeAction = mainWindow.actionForCommand(QStringLiteral("CascadeWindows"));
				QVERIFY(cascadeAction);
				cascadeAction->trigger();
				QCoreApplication::processEvents();

				QVERIFY(!mainWindow.isTabbedWindowPresentationActive());
				QVERIFY(!mdiArea->testOption(QMdiArea::DontMaximizeSubWindowOnActivation));
				QVERIFY(tabs->isHidden());
				for (const QMdiSubWindow *window : windows)
					QVERIFY(!window->isMaximized());
			}

			static void tileActionDispatchesAndSurvivesWindowActivation_data()
			{
				QTest::addColumn<QString>("commandName");
				QTest::addColumn<QMudMainFrameMdiUtils::TileDirection>("direction");

				QTest::newRow("horizontal")
				    << QStringLiteral("TileWindows") << QMudMainFrameMdiUtils::TileDirection::Horizontal;
				QTest::newRow("vertical") << QStringLiteral("TileWindowsVertically")
				                          << QMudMainFrameMdiUtils::TileDirection::Vertical;
			}

			static void tileActionDispatchesAndSurvivesWindowActivation()
			{
				QFETCH(QString, commandName);
				QFETCH(QMudMainFrameMdiUtils::TileDirection, direction);

				const int commandId = qmudStringToCommandId(commandName);
				QVERIFY(commandId != 0);
				QCOMPARE(qmudCommandIdToString(commandId), commandName);

				MainWindow  mainWindow;
				auto *const mdiArea = mainWindow.findChild<QMdiArea *>();
				QVERIFY(mdiArea);
				mdiArea->setParent(nullptr);
				mdiArea->resize(900, 700);
				mdiArea->show();
				QCoreApplication::processEvents();

				QMdiSubWindow *const first  = addSubWindow(mainWindow);
				QMdiSubWindow *const second = addSubWindow(mainWindow);
				QMdiSubWindow *const third  = addSubWindow(mainWindow);
				mdiArea->setActiveSubWindow(first);
				first->showMaximized();
				QCoreApplication::processEvents();
				QVERIFY(first->isMaximized());

				QAction *const action = mainWindow.actionForCommand(commandName);
				QVERIFY(action);
				QCOMPARE(action->objectName(), commandName);
				action->trigger();
				QCoreApplication::processEvents();

				const QRect                  area = mdiArea->viewport()->rect();
				const QList<QMdiSubWindow *> tiled{first, second, third};
				for (const QMdiSubWindow *window : tiled)
					QVERIFY(!window->isMaximized());

				if (direction == QMudMainFrameMdiUtils::TileDirection::Horizontal)
				{
					QCOMPARE(first->geometry().top(), area.top());
					QCOMPARE(third->geometry().bottom(), area.bottom());
					for (qsizetype index = 0; index < tiled.size(); ++index)
					{
						QCOMPARE(tiled.at(index)->geometry().left(), area.left());
						QCOMPARE(tiled.at(index)->geometry().width(), area.width());
						if (index > 0)
							QCOMPARE(tiled.at(index)->geometry().top(),
							         tiled.at(index - 1)->geometry().bottom() + 1);
					}
				}
				else
				{
					QCOMPARE(first->geometry().left(), area.left());
					QCOMPARE(third->geometry().right(), area.right());
					for (qsizetype index = 0; index < tiled.size(); ++index)
					{
						QCOMPARE(tiled.at(index)->geometry().top(), area.top());
						QCOMPARE(tiled.at(index)->geometry().height(), area.height());
						if (index > 0)
							QCOMPARE(tiled.at(index)->geometry().left(),
							         tiled.at(index - 1)->geometry().right() + 1);
					}
				}

				QList<QRect> tiledGeometry;
				tiledGeometry.reserve(tiled.size());
				for (const QMdiSubWindow *window : tiled)
					tiledGeometry.push_back(window->geometry());

				mdiArea->setActiveSubWindow(third);
				QCoreApplication::processEvents();

				QCOMPARE(mdiArea->activeSubWindow(), third);
				for (qsizetype index = 0; index < tiled.size(); ++index)
				{
					QVERIFY(!tiled.at(index)->isMaximized());
					QCOMPARE(tiled.at(index)->geometry(), tiledGeometry.at(index));
				}
			}
	};
} // namespace

QTEST_MAIN(tst_MainFrame_WindowTiling)

#if __has_include("tst_MainFrame_WindowTiling.moc")
#include "tst_MainFrame_WindowTiling.moc"
#endif
