/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_MdiTabs.cpp
 * Role: QTest coverage for MdiTabs behavior.
 */

#include "MdiTabs.h"

#include <QMdiArea>
#include <QMdiSubWindow>
#include <QVBoxLayout>
#include <QWidget>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

namespace
{
	QMdiSubWindow *addWindow(QMdiArea &mdiArea, const QString &title)
	{
		auto *container = new QWidget(&mdiArea);
		auto *sub       = mdiArea.addSubWindow(container);
		sub->setAttribute(Qt::WA_DeleteOnClose, true);
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

			void middleClickClosesClickedTab()
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
				tabs.updateTabs();
				QCOMPARE(tabs.count(), 2);

				QTest::mouseClick(&tabs, Qt::MiddleButton, Qt::NoModifier, tabs.tabRect(0).center());
				QCoreApplication::processEvents();
				tabs.updateTabs();

				QCOMPARE(tabs.count(), 1);
				QCOMPARE(tabTexts(tabs), QStringList({QStringLiteral("Two")}));
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

#if __has_include("tst_MdiTabs.moc")
#include "tst_MdiTabs.moc"
#endif
