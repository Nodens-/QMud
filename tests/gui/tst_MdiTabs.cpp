/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_MdiTabs.cpp
 * Role: QTest coverage for MdiTabs behavior.
 */

#include "MdiTabs.h"

#if QMUD_ENABLE_SOUND
#include "SoundEngine.h"
#include "WorldChildWindow.h"
#include "WorldRuntime.h"
// ReSharper disable once CppUnusedIncludeDirective
#include <QApplication>
#include <QContextMenuEvent>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFile>
#include <QMenu>
#include <QScopeGuard>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryDir>
#include <QTimer>
#endif
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QVBoxLayout>
#include <QWidget>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

namespace
{
	QMdiSubWindow *addWindow(QMdiArea &mdiArea, const QString &title, const bool deleteOnClose = true)
	{
		auto *container = new QWidget(&mdiArea);
		auto *sub       = mdiArea.addSubWindow(container);
		sub->setAttribute(Qt::WA_DeleteOnClose, deleteOnClose);
		sub->setWindowTitle(title);
		sub->show();
		return sub;
	}

	QStringList tabTexts(const MdiTabs &tabs)
	{
		QStringList out;
		for (int i = 0; i < tabs.count(); ++i)
			out.push_back(tabs.tabText(i));
		return out;
	}

	void createTabsForActivePresentation(MdiTabs &tabs, QMdiArea &mdiArea, const int style)
	{
		mdiArea.setOption(QMdiArea::DontMaximizeSubWindowOnActivation);
		tabs.create(&mdiArea, style);
		QObject::connect(&tabs, &MdiTabs::windowActivationRequested, &mdiArea,
		                 [&mdiArea](QMdiSubWindow *subWindow) { mdiArea.setActiveSubWindow(subWindow); });
	}

	/**
	 * @brief QTest fixture covering MdiTabs scenarios.
	 */
	class tst_MdiTabs : public QObject
	{
			Q_OBJECT

			// NOLINTBEGIN(readability-convert-member-functions-to-static)
		private slots:
#if QMUD_ENABLE_SOUND
			/** @brief Toggles both sound settings from an inactive world tab without activating it. */
			void muteMenuTargetsTheClickedWorld()
			{
				SoundEngine  engine;
				WorldRuntime runtime;
				runtime.attachSoundEngine(&engine);
				const auto session = runtime.soundPlaybackSession();
				QVERIFY(session);
				const QString pluginId = QStringLiteral("abcdef1234567890abcdef12");
				QTemporaryDir fixture(QDir::current().filePath(QStringLiteral("mdi-mute-XXXXXX")));
				QVERIFY(fixture.isValid());
				runtime.setStartupDirectory(fixture.path());
				QFile plugin(fixture.filePath(QStringLiteral("sound.xml")));
				QVERIFY(plugin.open(QIODevice::WriteOnly));
				const QByteArray xml =
				    QStringLiteral("<qmud><plugin id=\"%1\" name=\"Synthetic_sound\"/></qmud>")
				        .arg(pluginId)
				        .toUtf8();
				QCOMPARE(plugin.write(xml), xml.size());
				plugin.close();
				QString error;
				QVERIFY2(runtime.loadPluginFile(plugin.fileName(), &error), qPrintable(error));
				QWidget     host;
				QVBoxLayout layout(&host);
				QMdiArea    mdiArea;
				MdiTabs     tabs;
				layout.addWidget(&tabs);
				layout.addWidget(&mdiArea);
				host.resize(640, 480);
				host.show();
				createTabsForActivePresentation(tabs, mdiArea, kMdiTabsTop);
				auto *world = new WorldChildWindow(QStringLiteral("Sound world"));
				world->setRuntime(&runtime);
				mdiArea.addSubWindow(world);
				world->show();
				QMdiSubWindow *other = addWindow(mdiArea, QStringLiteral("Other tab"));
				mdiArea.setActiveSubWindow(other);
				tabs.updateTabs();
				QCOMPARE(tabs.count(), 2);
				QCOMPARE(tabs.currentIndex(), 1);
				QVERIFY(!tabs.tabButton(0, QTabBar::RightSide));

				const auto openMenu = [&](const int tabIndex, auto choose)
				{
					QTimer::singleShot(0, &tabs,
					                   [choose]
					                   {
						                   auto *menu =
						                       qobject_cast<QMenu *>(QApplication::activePopupWidget());
						                   QVERIFY(menu);
						                   const auto close = qScopeGuard([menu] { menu->close(); });
						                   choose(*menu);
					                   });
					const QPoint      position = tabs.tabRect(tabIndex).center();
					QContextMenuEvent event(QContextMenuEvent::Mouse, position, tabs.mapToGlobal(position));
					QCoreApplication::sendEvent(&tabs, &event);
					QCOMPARE(mdiArea.activeSubWindow(), other);
					QCOMPARE(tabs.currentIndex(), 1);
				};
				openMenu(0,
				         [](QMenu &menu)
				         {
					         const auto actions = menu.actions();
					         QVERIFY(actions.size() > 3);
					         QCOMPARE(actions.at(0)->text(), QStringLiteral("Mute"));
					         QCOMPARE(actions.at(1)->text(), QStringLiteral("Mute Plugin"));
					         QVERIFY(actions.at(2)->isSeparator());
					         QCOMPARE(actions.at(3)->text(), QStringLiteral("Restore"));
					         QVERIFY(!actions.at(0)->isChecked());
					         menu.setActiveAction(actions.at(0));
					         QTest::keyClick(&menu, Qt::Key_Return);
				         });
				QVERIFY(session->isMuted());
				QVERIFY(tabs.tabButton(0, QTabBar::RightSide));
				QCOMPARE(tabs.tabToolTip(0), QStringLiteral("World muted"));
				const auto choosePluginMute = [&](QMenu &menu)
				{
					QAction *submenuAction = menu.actions().at(1);
					QMenu   *submenu       = submenuAction->menu();
					QVERIFY(submenu);
					QAction *pluginAction = nullptr;
					for (QAction *action : submenu->actions())
						if (action->data().toString() == pluginId)
							pluginAction = action;
					QVERIFY(pluginAction && pluginAction->isCheckable());
					QCOMPARE(pluginAction->text(), QStringLiteral("Synthetic_sound"));
					menu.setActiveAction(submenuAction);
					QTest::keyClick(&menu, Qt::Key_Right);
					QVERIFY(submenu->isVisible());
					submenu->setActiveAction(pluginAction);
					QTest::keyClick(submenu, Qt::Key_Return);
				};
				openMenu(0, choosePluginMute);
				QVERIFY(session->isMuted());
				QVERIFY(session->isMuted(pluginId));
				QCOMPARE(tabs.tabToolTip(0), QStringLiteral("World muted"));
				openMenu(0,
				         [](QMenu &menu)
				         {
					         menu.setActiveAction(menu.actions().front());
					         QTest::keyClick(&menu, Qt::Key_Return);
				         });
				QVERIFY(!session->isMuted());
				QCOMPARE(tabs.tabToolTip(0), QStringLiteral("1 plugin muted"));
				const QPointer<QWidget> badge(tabs.tabButton(0, QTabBar::RightSide));
				QVERIFY(badge);
				QSignalSpy activations(&tabs, &MdiTabs::windowActivationRequested);
				runtime.setNewLines(3);
				tabs.updateTabs();
				QCOMPARE(tabs.tabText(0), QStringLiteral("Sound world (3)"));
				QCOMPARE(tabs.tabButton(0, QTabBar::RightSide), badge.data());
				QCOMPARE(tabs.tabToolTip(0), QStringLiteral("1 plugin muted"));
				QCOMPARE(mdiArea.activeSubWindow(), other);
				QCOMPARE(activations.count(), 0);

				tabs.moveTab(0, 1);
				auto *extra = addWindow(mdiArea, QStringLiteral("Extra tab"));
				mdiArea.setActiveSubWindow(other);
				tabs.updateTabs();
				QCOMPARE(tabs.orderedWindows(), (QList<QMdiSubWindow *>{other, world, extra}));
				QCOMPARE(tabs.tabButton(1, QTabBar::RightSide), badge.data());
				mdiArea.removeSubWindow(extra);
				delete extra;
				tabs.updateTabs();
				QCOMPARE(tabs.orderedWindows(), (QList<QMdiSubWindow *>{other, world}));
				QCOMPARE(tabs.tabButton(1, QTabBar::RightSide), badge.data());
				tabs.moveTab(1, 0);
				QVERIFY2(runtime.unloadPlugin(pluginId, &error), qPrintable(error));
				QTRY_VERIFY(tabs.tabToolTip(0).isEmpty());
				QVERIFY(!tabs.tabButton(0, QTabBar::RightSide));
				QVERIFY(session->isMuted(pluginId));
				QVERIFY2(runtime.loadPluginFile(plugin.fileName(), &error), qPrintable(error));
				QTRY_COMPARE(tabs.tabToolTip(0), QStringLiteral("1 plugin muted"));
				openMenu(0,
				         [](QMenu &menu)
				         {
					         menu.setActiveAction(menu.actions().front());
					         QTest::keyClick(&menu, Qt::Key_Return);
				         });
				QVERIFY2(runtime.unloadPlugin(pluginId, &error), qPrintable(error));
				QVERIFY2(runtime.loadPluginFile(plugin.fileName(), &error), qPrintable(error));
				QCoreApplication::processEvents();
				QCOMPARE(tabs.tabToolTip(0), QStringLiteral("World muted"));
				openMenu(0,
				         [](QMenu &menu)
				         {
					         menu.setActiveAction(menu.actions().front());
					         QTest::keyClick(&menu, Qt::Key_Return);
				         });
				openMenu(0, choosePluginMute);
				QVERIFY(!session->isMuted(pluginId));
				QVERIFY(!tabs.tabButton(0, QTabBar::RightSide));
				QVERIFY(tabs.tabToolTip(0).isEmpty());
				openMenu(1, [](const QMenu &menu)
				         { QCOMPARE(menu.actions().front()->text(), QStringLiteral("Restore")); });
			}
#endif

			void tabCreationVisibilityAndActiveTab()
			{
				QWidget     host;
				QVBoxLayout layout(&host);
				QMdiArea    mdiArea;
				MdiTabs     tabs;

				layout.addWidget(&tabs);
				layout.addWidget(&mdiArea);
				host.resize(640, 480);
				host.show();

				createTabsForActivePresentation(tabs, mdiArea, kMdiTabsTop | kMdiTabsHideLt2Views);

				QMdiSubWindow *first = addWindow(mdiArea, QStringLiteral("One"));
				tabs.updateTabs();
				QCOMPARE(tabs.count(), 1);
				QVERIFY(!tabs.isVisible());

				QMdiSubWindow *second = addWindow(mdiArea, QStringLiteral("Two"));
				tabs.updateTabs();
				QCOMPARE(tabs.count(), 2);
				QVERIFY(tabs.isVisible());

				mdiArea.setActiveSubWindow(second);
				tabs.updateTabs();
				QCOMPARE(tabs.currentIndex(), 1);

				QSignalSpy currentChangedSpy(&tabs, &QTabBar::currentChanged);
				tabs.setCurrentIndex(0);
				QCoreApplication::processEvents();
				QCOMPARE(mdiArea.activeSubWindow(), first);
				QVERIFY(currentChangedSpy.count() >= 1);
			}

			void tabReorderPersistsAcrossSyncs()
			{
				QWidget     host;
				QVBoxLayout layout(&host);
				QMdiArea    mdiArea;
				MdiTabs     tabs;

				layout.addWidget(&tabs);
				layout.addWidget(&mdiArea);
				host.resize(640, 480);
				host.show();

				createTabsForActivePresentation(tabs, mdiArea, kMdiTabsTop);
				addWindow(mdiArea, QStringLiteral("One"));
				addWindow(mdiArea, QStringLiteral("Two"));
				addWindow(mdiArea, QStringLiteral("Three"));
				tabs.updateTabs();

				QCOMPARE(tabTexts(tabs), QStringList({QStringLiteral("One"), QStringLiteral("Two"),
				                                      QStringLiteral("Three")}));

				tabs.moveTab(0, 2);
				tabs.updateTabs();
				QCOMPARE(tabTexts(tabs), QStringList({QStringLiteral("Two"), QStringLiteral("Three"),
				                                      QStringLiteral("One")}));

				addWindow(mdiArea, QStringLiteral("Four"));
				tabs.updateTabs();
				QCOMPARE(tabTexts(tabs), QStringList({QStringLiteral("Two"), QStringLiteral("Three"),
				                                      QStringLiteral("One"), QStringLiteral("Four")}));
			}

			void middleClickImmediatelyClosesClickedInactiveTab()
			{
				QWidget     host;
				QVBoxLayout layout(&host);
				QMdiArea    mdiArea;
				MdiTabs     tabs;

				layout.addWidget(&tabs);
				layout.addWidget(&mdiArea);
				host.resize(640, 480);
				host.show();

				createTabsForActivePresentation(tabs, mdiArea, kMdiTabsTop);
				addWindow(mdiArea, QStringLiteral("One"));
				QMdiSubWindow *second = addWindow(mdiArea, QStringLiteral("Two"));
				mdiArea.setActiveSubWindow(second);
				tabs.updateTabs();
				QCOMPARE(tabs.count(), 2);

				QTest::mouseClick(&tabs, Qt::MiddleButton, Qt::NoModifier, tabs.tabRect(0).center());

				QCOMPARE(tabs.count(), 1);
				QCOMPARE(tabTexts(tabs), QStringList({QStringLiteral("Two")}));
				QCOMPARE(mdiArea.subWindowList(), QList<QMdiSubWindow *>({second}));
			}

			void middleClickPreservesMdiOwnershipForHideOnCloseWindow()
			{
				QWidget     host;
				QVBoxLayout layout(&host);
				QMdiArea    mdiArea;
				MdiTabs     tabs;

				layout.addWidget(&tabs);
				layout.addWidget(&mdiArea);
				host.resize(640, 480);
				host.show();

				createTabsForActivePresentation(tabs, mdiArea, kMdiTabsTop);
				QMdiSubWindow *persistent = addWindow(mdiArea, QStringLiteral("Persistent"), false);
				QMdiSubWindow *active     = addWindow(mdiArea, QStringLiteral("Active"));
				mdiArea.setActiveSubWindow(active);
				tabs.updateTabs();

				QTest::mouseClick(&tabs, Qt::MiddleButton, Qt::NoModifier, tabs.tabRect(0).center());
				QCoreApplication::processEvents();

				QVERIFY(persistent->isHidden());
				QVERIFY(mdiArea.subWindowList().contains(persistent));
			}

			void mixedTabsKeepOrderAndActiveIndexAcrossWorldTitleUpdates()
			{
				QWidget     host;
				QVBoxLayout layout(&host);
				QMdiArea    mdiArea;
				MdiTabs     tabs;

				layout.addWidget(&tabs);
				layout.addWidget(&mdiArea);
				host.resize(640, 480);
				host.show();

				createTabsForActivePresentation(tabs, mdiArea, kMdiTabsTop);
				QMdiSubWindow *worldA  = addWindow(mdiArea, QStringLiteral("World A"));
				QMdiSubWindow *notepad = addWindow(mdiArea, QStringLiteral("Notepad"));
				addWindow(mdiArea, QStringLiteral("World B"));
				tabs.updateTabs();

				QCOMPARE(tabTexts(tabs), QStringList({QStringLiteral("World A"), QStringLiteral("Notepad"),
				                                      QStringLiteral("World B")}));

				mdiArea.setActiveSubWindow(notepad);
				tabs.updateTabs();
				QCOMPARE(tabs.currentIndex(), 1);

				worldA->setWindowTitle(QStringLiteral("World A (active)"));
				tabs.updateTabs();
				QCOMPARE(tabTexts(tabs), QStringList({QStringLiteral("World A (active)"),
				                                      QStringLiteral("Notepad"), QStringLiteral("World B")}));
				QCOMPARE(tabs.currentIndex(), 1);
				QCOMPARE(mdiArea.activeSubWindow(), notepad);
			}
			// NOLINTEND(readability-convert-member-functions-to-static)
	};
} // namespace

QTEST_MAIN(tst_MdiTabs)

#include "tst_MdiTabs.moc"
