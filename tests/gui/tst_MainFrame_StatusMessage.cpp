/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_MainFrame_StatusMessage.cpp
 * Role: GUI regression coverage for main-frame status-message ownership.
 */

#include "MainFrame.h"
#include "WorldChildWindow.h"
#include "WorldRuntime.h"

#include <QStatusTipEvent>
#include <QtTest/QTest>

#include <algorithm>

namespace
{
	/**
	 * @brief Returns whether any main-window status pane displays the requested text.
	 * @param frame Main window to inspect.
	 * @param text Exact pane text to find.
	 * @return `true` when a status pane contains the text.
	 */
	bool displaysStatusText(const MainWindow &frame, const QString &text)
	{
		const QList<StatusPaneLabel *> panes = frame.findChildren<StatusPaneLabel *>();
		return std::ranges::any_of(panes, [&text](const StatusPaneLabel *pane)
		                           { return pane && pane->text() == text; });
	}

	/**
	 * @brief QTest fixture covering status-message override ownership.
	 */
	class tst_MainFrame_StatusMessage final : public QObject
	{
			Q_OBJECT

		private slots:
			static void overridePreservesDisplayAndRuntimeStatusUpdates()
			{
				MainWindow frame;
				auto      *runtime = new WorldRuntime(&frame);
				auto      *window  = new WorldChildWindow(QStringLiteral("Status test"));
				window->setRuntime(runtime);
				frame.addMdiSubWindow(window, true);
				frame.show();
				QCoreApplication::processEvents();

				frame.setStatusMessageNow(QStringLiteral("Initial status"));
				QCOMPARE(runtime->statusMessage(), QStringLiteral("Initial status"));
				QVERIFY(displaysStatusText(frame, QStringLiteral("Initial status")));

				const quint64 token =
				    frame.acquireStatusMessageOverride(QStringLiteral("Reload recovery: opening worlds..."));
				QVERIFY(token != 0);
				QVERIFY(displaysStatusText(frame, QStringLiteral("Reload recovery: opening worlds...")));

				frame.setStatusMessageNow(QStringLiteral("Plugin status"));
				QCOMPARE(runtime->statusMessage(), QStringLiteral("Plugin status"));
				QVERIFY(displaysStatusText(frame, QStringLiteral("Reload recovery: opening worlds...")));
				QVERIFY(!displaysStatusText(frame, QStringLiteral("Plugin status")));

				frame.showStatusMessage(QStringLiteral("Connected."), 3000);
				QCOMPARE(runtime->statusMessage(), QStringLiteral("Plugin status"));
				QVERIFY(displaysStatusText(frame, QStringLiteral("Reload recovery: opening worlds...")));

				frame.setStatusNormal();
				QCOMPARE(runtime->statusMessage(), QStringLiteral("Ready"));
				QVERIFY(displaysStatusText(frame, QStringLiteral("Reload recovery: opening worlds...")));
				frame.setHyperlinkStatusLock(QStringLiteral("https://example.invalid/"));
				QCOMPARE(runtime->statusMessage(), QStringLiteral("Ready"));
				QVERIFY(displaysStatusText(frame, QStringLiteral("Reload recovery: opening worlds...")));
				QStatusTipEvent statusTipEvent(QStringLiteral("Menu status tip"));
				QCoreApplication::sendEvent(&frame, &statusTipEvent);
				QVERIFY(displaysStatusText(frame, QStringLiteral("Reload recovery: opening worlds...")));

				frame.updateStatusMessageOverride(token + 1, QStringLiteral("Wrong owner"));
				QVERIFY(displaysStatusText(frame, QStringLiteral("Reload recovery: opening worlds...")));
				frame.releaseStatusMessageOverride(token + 1);
				frame.setStatusMessageNow(QStringLiteral("Queued status"));
				QCOMPARE(runtime->statusMessage(), QStringLiteral("Queued status"));
				QVERIFY(displaysStatusText(frame, QStringLiteral("Reload recovery: opening worlds...")));

				frame.updateStatusMessageOverride(token,
				                                  QStringLiteral("Reload recovery: finalizing worlds..."));
				QVERIFY(displaysStatusText(frame, QStringLiteral("Reload recovery: finalizing worlds...")));
				frame.releaseStatusMessageOverride(token);
				frame.setStatusMessageNow(QStringLiteral("Recovery complete"));
				QCOMPARE(runtime->statusMessage(), QStringLiteral("Recovery complete"));
				QVERIFY(displaysStatusText(frame, QStringLiteral("Recovery complete")));

				window->setRuntime(nullptr);
			}
	};
} // namespace

QTEST_MAIN(tst_MainFrame_StatusMessage)

#if __has_include("tst_MainFrame_StatusMessage.moc")
#include "tst_MainFrame_StatusMessage.moc"
#endif
