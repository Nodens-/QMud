/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_MainFrameMdiUtils.cpp
 * Role: QTest coverage for MainFrame MDI fallback/restore helper behavior.
 */

#include "MainFrameMdiUtils.h"

#include <QMdiSubWindow>
#include <QtTest/QTest>

/**
 * @brief QTest fixture covering MainFrame MDI helper scenarios.
 */
class tst_MainFrameMdiUtils : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void resolveCurrentOrLastPrefersCurrentActive()
		{
			QMdiSubWindow                current;
			QMdiSubWindow                last;
			const QList<QMdiSubWindow *> windows{&current, &last};
			QCOMPARE(QMudMainFrameMdiUtils::resolveCurrentOrLastActiveSubWindow(&current, &last, windows),
			         &current);
		}

		void resolveCurrentOrLastFallsBackToLastActive()
		{
			QMdiSubWindow                last;
			const QList<QMdiSubWindow *> windows{&last};
			QCOMPARE(QMudMainFrameMdiUtils::resolveCurrentOrLastActiveSubWindow(nullptr, &last, windows),
			         &last);
		}

		void resolveCurrentOrLastReturnsNullWhenLastMissing()
		{
			QMdiSubWindow                last;
			const QList<QMdiSubWindow *> windows;
			QCOMPARE(QMudMainFrameMdiUtils::resolveCurrentOrLastActiveSubWindow(nullptr, &last, windows),
			         nullptr);
		}

		void resolveBackgroundAddRestoreTargetUsesLastWhenCurrentIsNull()
		{
			QMdiSubWindow                last;
			QMdiSubWindow                added;
			const QList<QMdiSubWindow *> windows{&last};
			QCOMPARE(
			    QMudMainFrameMdiUtils::resolveBackgroundAddRestoreTarget(nullptr, &last, windows, &added),
			    &last);
		}

		void resolveBackgroundAddRestoreTargetIgnoresAddedWindow()
		{
			QMdiSubWindow                added;
			const QList<QMdiSubWindow *> windows{&added};
			QCOMPARE(
			    QMudMainFrameMdiUtils::resolveBackgroundAddRestoreTarget(&added, &added, windows, &added),
			    nullptr);
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_MAIN(tst_MainFrameMdiUtils)

#if __has_include("tst_MainFrameMdiUtils.moc")
#include "tst_MainFrameMdiUtils.moc"
#endif
