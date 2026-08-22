/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldObserverLifecycle.cpp
 * Role: Regression coverage for shared-runtime MDI observer ownership and lifecycle behavior.
 */

#include "AppController.h"
#include "MainFrame.h"
#include "WorldChildWindow.h"
#include "WorldCommandProcessor.h"
#include "WorldRuntime.h"
#include "WorldView.h"

#include <QAbstractScrollArea>
#include <QPointer>
#include <QScrollBar>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

namespace
{
	QString worldVariable(const WorldRuntime &runtime, const QString &name)
	{
		QString value;
		if (!runtime.findVariable(name, value))
			return {};
		return value;
	}

	class tst_WorldObserverLifecycle final : public QObject
	{
			Q_OBJECT

		private slots:
			static void newObserverPreservesWindowedPresentation()
			{
				AppController app;
				MainWindow    frame;
				app.setMainWindow(&frame);
				frame.resize(900, 700);
				frame.show();
				frame.setWindowTabsStyle(kMdiTabsTop);

				auto *runtime = new WorldRuntime(&frame);
				auto *primary = new WorldChildWindow(QStringLiteral("Primary"));
				primary->setRuntime(runtime);
				frame.addMdiSubWindow(primary, true);
				QCoreApplication::processEvents();
				QVERIFY(frame.isTabbedWindowPresentationActive());

				primary->showNormal();
				QCoreApplication::processEvents();
				QVERIFY(!frame.isTabbedWindowPresentationActive());
				QVERIFY(!primary->isMaximized());

				app.onCommandTriggered(QStringLiteral("NewWindow"));
				QCoreApplication::processEvents();

				WorldChildWindow *const observer = frame.activeWorldPresentationWindow();
				QVERIFY(observer);
				QVERIFY(observer != primary);
				QVERIFY(!observer->isPrimaryRuntimeBinding());
				QCOMPARE(observer->runtime(), runtime);
				QVERIFY(!frame.isTabbedWindowPresentationActive());
				QVERIFY(!primary->isMaximized());
				QVERIFY(!observer->isMaximized());
			}

			static void runtimeEnumerationAndActivationAreUnique()
			{
				MainWindow frame;
				auto      *firstRuntime  = new WorldRuntime(&frame);
				auto      *secondRuntime = new WorldRuntime(&frame);
				auto      *firstPrimary  = new WorldChildWindow(QStringLiteral("First primary"));
				auto      *firstObserver = new WorldChildWindow(QStringLiteral("First observer"));
				auto      *secondPrimary = new WorldChildWindow(QStringLiteral("Second primary"));

				firstPrimary->setRuntime(firstRuntime);
				firstObserver->setRuntimeObserver(firstRuntime);
				secondPrimary->setRuntime(secondRuntime);
				frame.addMdiSubWindow(firstPrimary, true);
				frame.addMdiSubWindow(firstObserver, false);
				frame.addMdiSubWindow(secondPrimary, false);

				const QVector<WorldWindowDescriptor> presentations = frame.worldWindowDescriptors();
				const QVector<WorldWindowDescriptor> runtimes      = frame.worldRuntimeDescriptors();
				QCOMPARE(presentations.size(), 3);
				QCOMPARE(runtimes.size(), 2);
				QCOMPARE(runtimes.at(0).runtime, firstRuntime);
				QCOMPARE(runtimes.at(0).window, firstPrimary);
				QCOMPARE(frame.findWorldChildWindow(firstRuntime), firstPrimary);
				QCOMPARE(runtimes.at(1).runtime, secondRuntime);
				QCOMPARE(runtimes.at(1).window, secondPrimary);
				int firstRuntimePresentationCount = 0;
				for (const WorldWindowDescriptor &presentation : presentations)
					firstRuntimePresentationCount += presentation.runtime == firstRuntime ? 1 : 0;
				QCOMPARE(firstRuntimePresentationCount, 2);

				frame.activateWorldWindow(firstPrimary);
				QTRY_VERIFY(firstRuntime->isActive());
				QVERIFY(!secondRuntime->isActive());
				QCOMPARE(frame.activeWorldPresentationWindow(), firstPrimary);

				frame.activateWorldWindow(firstObserver);
				QTRY_VERIFY(!firstRuntime->isActive());
				QVERIFY(!secondRuntime->isActive());
				QVERIFY(!frame.activeWorldChildWindow());
				QCOMPARE(frame.activeWorldPresentationWindow(), firstObserver);
				QCOMPARE(frame.worldRuntimeDescriptors().size(), 2);
				QCOMPARE(frame.worldRuntimeDescriptors().at(0).window, firstPrimary);
				QCOMPARE(frame.worldRuntimeDescriptors().at(1).window, secondPrimary);

				frame.activateWorldWindow(secondPrimary);
				QTRY_VERIFY(secondRuntime->isActive());
				QTRY_VERIFY(!firstRuntime->isActive());
				QCOMPARE(frame.activeWorldPresentationWindow(), secondPrimary);

				// This state is intentionally invalid in production: a primary closes every observer with it.
				// Runtime resolution must nevertheless fail closed rather than substituting an observer.
				firstPrimary->setRuntime(nullptr);
				QCOMPARE(frame.findWorldChildWindow(firstRuntime), nullptr);
				QVERIFY(!frame.activateWorldRuntime(firstRuntime));
				const QVector<WorldWindowDescriptor> remainingRuntimes = frame.worldRuntimeDescriptors();
				QCOMPARE(remainingRuntimes.size(), 1);
				QCOMPARE(remainingRuntimes.constFirst().runtime, secondRuntime);
				QCOMPARE(remainingRuntimes.constFirst().window, secondPrimary);

				firstObserver->setRuntimeObserver(nullptr);
				secondPrimary->setRuntime(nullptr);
			}

			static void observerIsPassiveBufferView()
			{
				MainWindow frame;
				auto      *runtime = new WorldRuntime(&frame);
				for (int line = 0; line < 300; ++line)
					runtime->addLine(QStringLiteral("observer-buffer-%1").arg(line), WorldRuntime::LineOutput,
					                 true);
				auto *primary  = new WorldChildWindow(QStringLiteral("Primary"));
				auto *observer = new WorldChildWindow(QStringLiteral("Observer"));

				primary->setRuntime(runtime);
				observer->setRuntimeObserver(runtime);
				frame.addMdiSubWindow(primary, true);
				frame.addMdiSubWindow(observer, false);
				observer->view()->restoreOutputFromPersistedLines(runtime->lines());
				frame.show();
				observer->show();
				QCoreApplication::processEvents();

				QCOMPARE(primary->findChildren<WorldCommandProcessor *>().size(), 1);
				QVERIFY(observer->findChildren<WorldCommandProcessor *>().isEmpty());
				QVERIFY(primary->view()->inputEditor());
				QVERIFY(!observer->view()->inputEditor());
				QVERIFY(primary->isPrimaryRuntimeBinding());
				QVERIFY(!observer->isPrimaryRuntimeBinding());

				QAbstractScrollArea *output = nullptr;
				for (QAbstractScrollArea *candidate : observer->view()->findChildren<QAbstractScrollArea *>())
				{
					if (candidate->isVisible() && candidate->verticalScrollBar() &&
					    candidate->verticalScrollBar()->maximum() > candidate->verticalScrollBar()->minimum())
					{
						output = candidate;
						break;
					}
				}
				QVERIFY(output);
				QScrollBar *scroll = output->verticalScrollBar();
				scroll->setValue(scroll->maximum());
				const int endPosition = scroll->value();
				QVERIFY(endPosition > scroll->minimum());

				QSignalSpy sendSpy(observer->view(), &WorldView::sendText);
				QTest::keyClick(output->viewport(), Qt::Key_PageUp);
				QTRY_VERIFY(scroll->value() < endPosition);
				QTest::keyClick(output->viewport(), Qt::Key_A);
				QTest::keyClick(output->viewport(), Qt::Key_Return);
				QCOMPARE(sendSpy.count(), 0);

				primary->setRuntime(nullptr);
				observer->setRuntimeObserver(nullptr);
			}

			static void closeUsesPresentationAndFinalRuntimeLifecycles()
			{
				MainWindow frame;
				auto      *runtime = new WorldRuntime(&frame);
				runtime->setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
				runtime->setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
				runtime->setWorldAttribute(QStringLiteral("on_world_close"), QStringLiteral("qcb_close"));
				runtime->setLuaScriptText(QStringLiteral(R"lua(
function qcb_close()
  local count = tonumber(GetVariable("close_count") or "0") or 0
  SetVariable("close_count", tostring(count + 1))
end
)lua"));

				auto *primary  = new WorldChildWindow(QStringLiteral("Primary"));
				auto *observer = new WorldChildWindow(QStringLiteral("Observer"));
				primary->setRuntime(runtime);
				observer->setRuntimeObserver(runtime);
				frame.addMdiSubWindow(primary, true);
				frame.addMdiSubWindow(observer, false);

				QPointer<WorldChildWindow> observerGuard(observer);
				observer->close();
				QTRY_VERIFY(observerGuard.isNull());
				QCOMPARE(worldVariable(*runtime, QStringLiteral("close_count")), QString());
				QVERIFY(primary->isPrimaryRuntimeBinding());

				auto *secondObserver = new WorldChildWindow(QStringLiteral("Second observer"));
				secondObserver->setRuntimeObserver(runtime);
				frame.addMdiSubWindow(secondObserver, false);
				QPointer<WorldChildWindow> primaryGuard(primary);
				QPointer<WorldChildWindow> observerGuard2(secondObserver);
				primary->close();
				QTRY_VERIFY(primaryGuard.isNull());
				QTRY_VERIFY(observerGuard2.isNull());
				QTRY_COMPARE(worldVariable(*runtime, QStringLiteral("close_count")), QStringLiteral("1"));
				QVERIFY(!runtime->view());
			}
	};
} // namespace

QTEST_MAIN(tst_WorldObserverLifecycle)

#if __has_include("tst_WorldObserverLifecycle.moc")
#include "tst_WorldObserverLifecycle.moc"
#endif
