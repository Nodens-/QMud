/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_MdiTabs.cpp
 * Role: QTest coverage for MdiTabs behavior.
 */

#include "MdiTabs.h"
#include "WorldChildWindow.h"
#include "WorldRuntime.h"

#include <QMdiArea>
#include <QSignalSpy>
#include <QVBoxLayout>
#include <QWidget>
#include <QtTest/QTest>

// NOLINTBEGIN(readability-convert-member-functions-to-static)
WorldChildWindow::WorldChildWindow(QWidget *parent) : QMdiSubWindow(parent) {}

WorldChildWindow::WorldChildWindow(const QString &title, QWidget *parent) : QMdiSubWindow(parent)
{
	setWindowTitle(title);
}

void WorldChildWindow::setRuntime(WorldRuntime *) {}

void WorldChildWindow::setRuntimeObserver(WorldRuntime *) {}

WorldRuntime *WorldChildWindow::runtime() const
{
	return nullptr;
}

WorldView *WorldChildWindow::view() const
{
	return nullptr;
}

void WorldChildWindow::closeEvent(QCloseEvent *event)
{
	QMdiSubWindow::closeEvent(event);
}

void WorldChildWindow::showEvent(QShowEvent *event)
{
	QMdiSubWindow::showEvent(event);
}

void WorldChildWindow::resizeEvent(QResizeEvent *event)
{
	QMdiSubWindow::resizeEvent(event);
}

bool WorldChildWindow::event(QEvent *event)
{
	return QMdiSubWindow::event(event);
}

ActivityChildWindow::ActivityChildWindow(QWidget *parent) : QMdiSubWindow(parent) {}

ActivityChildWindow::ActivityChildWindow(const QString &title, QWidget *parent) : QMdiSubWindow(parent)
{
	setWindowTitle(title);
}

ActivityWindow *ActivityChildWindow::activityWindow() const
{
	return nullptr;
}

TextChildWindow::TextChildWindow(QWidget *parent) : QMdiSubWindow(parent) {}

TextChildWindow::TextChildWindow(const QString &title, const QString &, QWidget *parent) : QMdiSubWindow(parent)
{
	setWindowTitle(title);
}

QPlainTextEdit *TextChildWindow::editor() const
{
	return nullptr;
}

void TextChildWindow::setText(const QString &) const {}

void TextChildWindow::appendText(const QString &) const {}

QString TextChildWindow::filePath() const
{
	return QString();
}

void TextChildWindow::setFilePath(const QString &) {}

bool TextChildWindow::saveToFile(const QString &, QString *error)
{
	if (error)
		error->clear();
	return false;
}

bool TextChildWindow::saveToCurrentFile(QString *error)
{
	if (error)
		error->clear();
	return false;
}

void TextChildWindow::setQuerySaveOnClose(bool) {}

void TextChildWindow::closeEvent(QCloseEvent *event)
{
	QMdiSubWindow::closeEvent(event);
}

QString WorldRuntime::windowTitleOverride() const
{
	return QString();
}

const QMap<QString, QString> &WorldRuntime::worldAttributes() const
{
	static const QMap<QString, QString> kEmpty;
	return kEmpty;
}

bool WorldRuntime::doNotShowOutstandingLines() const
{
	return true;
}

int WorldRuntime::newLines() const
{
	return 0;
}
// NOLINTEND(readability-convert-member-functions-to-static)

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
} // namespace

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

			tabs.create(&mdiArea, kMdiTabsTop | kMdiTabsHideLt2Views);

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

			tabs.create(&mdiArea, kMdiTabsTop);
			addWindow(mdiArea, QStringLiteral("One"));
			addWindow(mdiArea, QStringLiteral("Two"));
			addWindow(mdiArea, QStringLiteral("Three"));
			tabs.updateTabs();

			QCOMPARE(tabTexts(tabs), QStringList({QStringLiteral("One"), QStringLiteral("Two"), QStringLiteral("Three")}));

			tabs.moveTab(0, 2);
			tabs.updateTabs();
			QCOMPARE(tabTexts(tabs), QStringList({QStringLiteral("Two"), QStringLiteral("Three"), QStringLiteral("One")}));

			addWindow(mdiArea, QStringLiteral("Four"));
			tabs.updateTabs();
			QCOMPARE(tabTexts(tabs),
			         QStringList({QStringLiteral("Two"), QStringLiteral("Three"), QStringLiteral("One"), QStringLiteral("Four")}));
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

			tabs.create(&mdiArea, kMdiTabsTop);
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
	// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_MAIN(tst_MdiTabs)

#include "tst_MdiTabs.moc"
