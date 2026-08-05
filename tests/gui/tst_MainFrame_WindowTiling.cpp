/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_MainFrame_WindowTiling.cpp
 * Role: GUI integration coverage for main-frame MDI tiling action dispatch and layout stability.
 */

#include "MainFrame.h"
#include "MainFrameMdiUtils.h"
#include "StringUtils.h"

#include <QAction>
// ReSharper disable once CppUnusedIncludeDirective
#include <QCoreApplication>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QWidget>
#include <QtTest/QTest>

namespace
{
	QMdiSubWindow *addSubWindow(MainWindow &mainWindow)
	{
		auto *const window = new QMdiSubWindow;
		window->setWidget(new QWidget);
		mainWindow.addMdiSubWindow(window, false);
		return window;
	}

	/**
	 * @brief QTest fixture covering end-to-end main-frame window tiling actions.
	 */
	class tst_MainFrame_WindowTiling final : public QObject
	{
			Q_OBJECT

		private slots:
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
