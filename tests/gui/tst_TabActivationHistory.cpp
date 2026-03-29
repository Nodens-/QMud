/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_TabActivationHistory.cpp
 * Role: QTest coverage for tab close fallback selection from activation history.
 */

#include "TabActivationHistory.h"

#include <QMdiSubWindow>
#include <QtTest/QTest>

namespace
{
	QMdiSubWindow *makeSubWindow()
	{
		auto *sub = new QMdiSubWindow();
		sub->setAttribute(Qt::WA_DeleteOnClose, true);
		return sub;
	}
} // namespace

/**
 * @brief QTest fixture covering tab-activation history fallback behavior.
 */
class tst_TabActivationHistory : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void closeActiveRestoresPreviousActive()
		{
			TabActivationHistory         history;
			auto                        *first  = makeSubWindow();
			auto                        *second = makeSubWindow();
			auto                        *third  = makeSubWindow();

			const QList<QMdiSubWindow *> openWindows{first, second, third};
			history.onActivated(first, openWindows);
			history.onActivated(second, openWindows);
			history.onActivated(third, openWindows);
			history.onCloseEvent(third, third, openWindows);

			const QList<QMdiSubWindow *> afterClose{first, second};
			const auto                   fallback = history.takeFallbackOnDestroyed(third, afterClose);
			QCOMPARE(fallback.data(), second);

			delete third;
			delete second;
			delete first;
		}

		void closeActiveSkipsStaleHistoryEntries()
		{
			TabActivationHistory         history;
			auto                        *first  = makeSubWindow();
			auto                        *second = makeSubWindow();
			auto                        *third  = makeSubWindow();

			const QList<QMdiSubWindow *> openWindows{first, second, third};
			history.onActivated(first, openWindows);
			history.onActivated(second, openWindows);
			history.onActivated(third, openWindows);

			const QList<QMdiSubWindow *> secondRemoved{first, third};
			history.onCloseEvent(third, third, secondRemoved);

			const QList<QMdiSubWindow *> afterClose{first};
			const auto                   fallback = history.takeFallbackOnDestroyed(third, afterClose);
			QCOMPARE(fallback.data(), first);

			delete third;
			delete second;
			delete first;
		}

		void closeInactiveWindowDoesNotQueueFallback()
		{
			TabActivationHistory         history;
			auto                        *first  = makeSubWindow();
			auto                        *second = makeSubWindow();
			auto                        *third  = makeSubWindow();

			const QList<QMdiSubWindow *> openWindows{first, second, third};
			history.onActivated(first, openWindows);
			history.onActivated(second, openWindows);
			history.onActivated(third, openWindows);

			history.onCloseEvent(first, third, openWindows);

			const QList<QMdiSubWindow *> afterClose{second, third};
			const auto                   fallback = history.takeFallbackOnDestroyed(first, afterClose);
			QVERIFY(!fallback);

			delete third;
			delete second;
			delete first;
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_MAIN(tst_TabActivationHistory)

#if __has_include("tst_TabActivationHistory.moc")
#include "tst_TabActivationHistory.moc"
#endif
