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
#include "WorldRuntimeTestAccess.h"
#include "WorldView.h"
#include "helpers/WorldSessionStateUtils.h"

#include <QAbstractScrollArea>
#include <QClipboard>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QMdiArea>
#include <QPointer>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSplitter>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

namespace
{
	/**
	 * @brief Finds the output-pane splitter owned by a world view.
	 * @param view World view to inspect.
	 * @return Output splitter, or `nullptr` when unavailable.
	 */
	QSplitter *findOutputSplitter(const WorldView &view)
	{
		for (QSplitter *splitter : view.findChildren<QSplitter *>())
		{
			if (splitter && splitter->count() == 2 &&
			    qobject_cast<QAbstractScrollArea *>(splitter->widget(0)) &&
			    qobject_cast<QAbstractScrollArea *>(splitter->widget(1)))
			{
				return splitter;
			}
		}
		return nullptr;
	}

	/**
	 * @brief Finds the visible scrollable output pane owned by a world view.
	 * @param view World view to inspect.
	 * @return Scrollable output pane, or `nullptr` when unavailable.
	 */
	QAbstractScrollArea *findScrollableOutput(const WorldView &view)
	{
		for (QAbstractScrollArea *candidate : view.findChildren<QAbstractScrollArea *>())
		{
			if (candidate && candidate->isVisible() && candidate->verticalScrollBar() &&
			    candidate->verticalScrollBar()->maximum() > candidate->verticalScrollBar()->minimum())
			{
				return candidate;
			}
		}
		return nullptr;
	}

	class PaintEventCounter final : public QObject
	{
		public:
			/**
			 * @brief Returns the number of observed paint events.
			 * @return Observed paint-event count.
			 */
			[[nodiscard]] int count() const
			{
				return m_count;
			}

			/**
			 * @brief Counts paint events delivered to the watched output canvas.
			 * @param watched Object receiving the event.
			 * @param event Event delivered to the object.
			 * @return Result of the base event filter.
			 */
			bool eventFilter(QObject *watched, QEvent *event) override
			{
				if (watched && event && event->type() == QEvent::Paint)
					++m_count;
				return QObject::eventFilter(watched, event);
			}

		private:
			int m_count{0};
	};

	QString worldVariable(const WorldRuntime &runtime, const QString &name)
	{
		QString value;
		if (!runtime.findVariable(name, value))
			return {};
		return value;
	}
} // namespace

class tst_WorldObserverLifecycle final : public QObject
{
		Q_OBJECT

	private slots:
		static void startupPostRestoreWaitsForPluginInstallCommit()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));
			const QString pluginId   = QStringLiteral("aabbccddeeff001122334455");
			const QString pluginPath = QDir(pluginsDir).filePath(QStringLiteral("startup_commit.xml"));
			QFile         pluginFile(pluginPath);
			QVERIFY(pluginFile.open(QIODevice::WriteOnly | QIODevice::Text));
			const QByteArray pluginXml = QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin name="StartupCommit" id="%1" language="lua" enabled="y" save_state="n">
    <script><![CDATA[
function OnPluginInstall()
  SetVariable("install_completed", "yes")
end
]]></script>
  </plugin>
</muclient>
)xml")
			                                 .arg(pluginId)
			                                 .toUtf8();
			QCOMPARE(pluginFile.write(pluginXml), static_cast<qint64>(pluginXml.size()));
			pluginFile.close();

			AppController app;
			MainWindow    frame;
			app.setMainWindow(&frame);
			frame.resize(900, 700);
			frame.show();

			auto *runtime = new WorldRuntime(&frame);
			runtime->setStartupDirectory(tempDir.path());
			runtime->setPluginsDirectory(pluginsDir);
			runtime->setPluginInstallDeferred(true);
			auto *world = new WorldChildWindow(QStringLiteral("Startup commit"));
			world->setRuntime(runtime);
			frame.addMdiSubWindow(world, false);
			QCoreApplication::processEvents();

			QString error;
			QVERIFY2(runtime->loadPluginFile(pluginPath, &error), qPrintable(error));
			QVERIFY(runtime->pluginForId(pluginId));
			QVERIFY(runtime->pluginForId(pluginId)->installPending);

			bool startupComplete = false;
			app.runWorldStartupPostRestore(runtime, [&startupComplete] { startupComplete = true; });
			QVERIFY(!startupComplete);
			QTRY_VERIFY_WITH_TIMEOUT(startupComplete, 5000);
			QCOMPARE(runtime->pluginVariableValue(pluginId, QStringLiteral("install_completed")),
			         QStringLiteral("yes"));
			QVERIFY(!runtime->pluginForId(pluginId)->installPending);

			world->setRuntime(nullptr);
		}

		static void sessionStateSaveSelectionDeduplicatesFilePaths()
		{
			const QVector<AppController::SessionStateSaveCandidate> candidates = {
			    {nullptr, nullptr, QStringLiteral("/state/shared.qws"), QStringLiteral("First"),  1, false},
			    {nullptr, nullptr, QStringLiteral("/state/shared.qws"), QStringLiteral("Second"), 2, true },
			    {nullptr, nullptr, QStringLiteral("/state/unique.qws"), QStringLiteral("Third"),  3, false},
			};

			const QVector<int> selected = AppController::selectSessionStateSaveCandidateIndexes(candidates);
			QCOMPARE(selected.size(), 2);
			QVERIFY(candidates.at(selected.at(0)).filePath != candidates.at(selected.at(1)).filePath);
		}

		/** @brief Closing an unselected duplicate world preserves the selected session checkpoint. */
		static void closingWorldsPreservesSelectedSessionCheckpoint()
		{
			QTemporaryDir directory(QDir::current().filePath(QStringLiteral("selected-session-XXXXXX")));
			QVERIFY(directory.isValid());
			AppController app;
			app.m_workingDir = directory.path();
			MainWindow frame;
			app.setMainWindow(&frame);
			auto *unselectedRuntime = new WorldRuntime(&frame);
			auto *selectedRuntime   = new WorldRuntime(&frame);
			for (WorldRuntime *runtime : {unselectedRuntime, selectedRuntime})
			{
				runtime->setWorldAttribute(QStringLiteral("id"), QStringLiteral("shared-close-checkpoint"));
				runtime->setWorldAttribute(QStringLiteral("save_world_automatically"), QStringLiteral("0"));
			}
			unselectedRuntime->setWorldAttribute(QStringLiteral("persist_output_buffer"),
			                                     QStringLiteral("0"));
			unselectedRuntime->setWorldAttribute(QStringLiteral("persist_command_history"),
			                                     QStringLiteral("0"));
			selectedRuntime->setWorldAttribute(QStringLiteral("persist_output_buffer"), QStringLiteral("1"));
			selectedRuntime->setWorldAttribute(QStringLiteral("history_lines"), QStringLiteral("20"));
			selectedRuntime->setWorldAttribute(QStringLiteral("persist_command_history"),
			                                   QStringLiteral("1"));
			auto *unselected = new WorldChildWindow(QStringLiteral("Unselected"));
			auto *selected   = new WorldChildWindow(QStringLiteral("Selected"));
			unselected->setRuntime(unselectedRuntime);
			selected->setRuntime(selectedRuntime);
			frame.addMdiSubWindow(unselected, false);
			frame.addMdiSubWindow(selected, false);
			selectedRuntime->addLine(QStringLiteral("selected output"), WorldRuntime::LineOutput, true);
			selected->view()->setCommandHistoryList({QStringLiteral("selected command")});
			QString error;
			QVERIFY2(app.saveOpenWorldStateBeforeShutdown(&error), qPrintable(error));
			const QString path = app.worldSessionStateFilePath(selectedRuntime);
			QMudWorldSessionState::WorldSessionStateData checkpoint;
			QVERIFY2(QMudWorldSessionState::readSessionStateFile(path, &checkpoint, &error),
			         qPrintable(error));
			QCOMPARE(checkpoint.commandHistory, QStringList({QStringLiteral("selected command")}));
			QVERIFY(unselected->close());
			QThreadPool::globalInstance()->waitForDone();
			QVERIFY(unselectedRuntime->isSessionStateOutputBufferSealed());
			QVERIFY(!selectedRuntime->isSessionStateOutputBufferSealed());
			QMudWorldSessionState::WorldSessionStateData afterClose;
			QVERIFY2(QMudWorldSessionState::readSessionStateFile(path, &afterClose, &error),
			         qPrintable(error));
			QCOMPARE(afterClose.commandHistory, checkpoint.commandHistory);
			QCOMPARE(afterClose.outputLines.size(), checkpoint.outputLines.size());
			QVERIFY(selected->close());
			QThreadPool::globalInstance()->waitForDone();
			QVERIFY(selectedRuntime->isSessionStateOutputBufferSealed());
			QVERIFY2(QMudWorldSessionState::readSessionStateFile(path, &afterClose, &error),
			         qPrintable(error));
			QCOMPARE(afterClose.commandHistory, checkpoint.commandHistory);
		}

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
			QTRY_VERIFY(firstRuntime->isActive());
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

		static void tabPresentationExposesOnlyTheActiveWorldWindow()
		{
			MainWindow frame;
			frame.resize(900, 700);
			frame.show();
			frame.setWindowTabsStyle(kMdiTabsTop);

			auto *runtime  = new WorldRuntime(&frame);
			auto *primary  = new WorldChildWindow(QStringLiteral("Primary"));
			auto *observer = new WorldChildWindow(QStringLiteral("Observer"));
			primary->setRuntime(runtime);
			observer->setRuntimeObserver(runtime);
			frame.addMdiSubWindow(primary, true);
			frame.addMdiSubWindow(observer, false);
			QCoreApplication::processEvents();

			QVERIFY(frame.isTabbedWindowPresentationActive());
			QCOMPARE(frame.activeWorldPresentationWindow(), primary);
			QVERIFY(primary->view()->outputPresentationExposed());
			QVERIFY(!observer->view()->outputPresentationExposed());

			frame.activateWorldWindow(observer);
			QCoreApplication::processEvents();
			QCOMPARE(frame.activeWorldPresentationWindow(), observer);
			QVERIFY(!primary->view()->outputPresentationExposed());
			QVERIFY(observer->view()->outputPresentationExposed());

			frame.setWindowTabsStyle(0);
			QCoreApplication::processEvents();
			QVERIFY(!frame.isTabbedWindowPresentationActive());
			QVERIFY(primary->isVisible());
			QVERIFY(observer->isVisible());
			QVERIFY(primary->view()->outputPresentationExposed());
			QVERIFY(observer->view()->outputPresentationExposed());

			observer->setRuntimeObserver(nullptr);
			primary->setRuntime(nullptr);
		}

		/**
		 * @brief Verifies losing application focus preserves the selected tab's live presentation.
		 */
		static void applicationFocusLossKeepsSelectedTabLive()
		{
			MainWindow frame;
			frame.resize(900, 700);
			frame.show();
			frame.setWindowTabsStyle(kMdiTabsTop);

			auto *selectedRuntime = new WorldRuntime(&frame);
			auto *hiddenRuntime   = new WorldRuntime(&frame);
			auto *selectedWorld   = new WorldChildWindow(QStringLiteral("Selected"));
			auto *hiddenWorld     = new WorldChildWindow(QStringLiteral("Hidden"));
			selectedWorld->setRuntime(selectedRuntime);
			hiddenWorld->setRuntime(hiddenRuntime);
			frame.addMdiSubWindow(selectedWorld, true);
			frame.addMdiSubWindow(hiddenWorld, false);
			for (int line = 0; line < 300; ++line)
			{
				selectedRuntime->addLine(QStringLiteral("selected-output-%1").arg(line),
				                         WorldRuntime::LineOutput, true);
			}
			QCoreApplication::processEvents();

			QVERIFY(selectedRuntime->isActive());
			QVERIFY(!hiddenRuntime->isActive());
			QVERIFY(selectedWorld->view()->outputPresentationExposed());
			QVERIFY(!hiddenWorld->view()->outputPresentationExposed());
			const QString selectedFrameTitle = frame.windowTitle();
			QVERIFY(!selectedFrameTitle.isEmpty());

			QAbstractScrollArea *const output = findScrollableOutput(*selectedWorld->view());
			QVERIFY(output);
			QScrollBar *const scroll = output->verticalScrollBar();
			QVERIFY(scroll);
			scroll->setValue(scroll->maximum());
			auto *const canvas =
			    selectedWorld->view()->findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(canvas);
			PaintEventCounter paintCounter;
			canvas->installEventFilter(&paintCounter);

			auto *const mdiArea = frame.findChild<QMdiArea *>();
			QVERIFY(mdiArea);
			QCOMPARE(mdiArea->activeSubWindow(), selectedWorld);
			mdiArea->setActiveSubWindow(nullptr);
			QCoreApplication::processEvents();

			QCOMPARE(mdiArea->activeSubWindow(), nullptr);
			QCOMPARE(frame.activeWorldPresentationWindow(), selectedWorld);
			QCOMPARE(frame.windowTitle(), selectedFrameTitle);
			QVERIFY(selectedRuntime->isActive());
			QVERIFY(!hiddenRuntime->isActive());
			QVERIFY(selectedWorld->view()->outputPresentationExposed());
			QVERIFY(!hiddenWorld->view()->outputPresentationExposed());
			QVERIFY(canvas->updatesEnabled());
			QVERIFY(scroll->updatesEnabled());

			const int priorMaximum    = scroll->maximum();
			const int priorPaintCount = paintCounter.count();
			selectedRuntime->addLine(QStringLiteral("selected-output-after-focus-loss"),
			                         WorldRuntime::LineOutput, true);
			QTRY_VERIFY(scroll->maximum() > priorMaximum);
			QCOMPARE(scroll->value(), scroll->maximum());
			QTRY_VERIFY(paintCounter.count() > priorPaintCount);

			const QPointer<WorldChildWindow> selectedGuard = selectedWorld;
			selectedWorld->close();
			QTRY_VERIFY(selectedGuard.isNull());
			QTRY_COMPARE(frame.activeWorldPresentationWindow(), hiddenWorld);
			QTRY_VERIFY(!selectedRuntime->isActive());
			QTRY_VERIFY(hiddenRuntime->isActive());
			QVERIFY(hiddenWorld->view()->outputPresentationExposed());

			hiddenWorld->setRuntime(nullptr);
		}

		/**
		 * @brief Verifies shared render deltas reach every presentation synchronously and transform only once.
		 */
		static void observerUsesSharedRenderModelOncePerMutation()
		{
			WorldRuntime runtime;
			WorldView    primary;
			WorldView    observer;
			QSignalSpy   renderDeltaSpy(&runtime, &WorldRuntime::outputRenderDeltaCommitted);
			primary.setRuntime(&runtime);
			runtime.addLine(QStringLiteral("before observer attach"), WorldRuntime::LineOutput, true);
			QCOMPARE(primary.outputLines(), QStringList{QStringLiteral("before observer attach")});
			QCOMPARE(renderDeltaSpy.size(), 0);

			const quint64 transformationsBefore = primary.m_nativeRenderModel->transformationCount;
			observer.setRuntimeObserver(&runtime);
			QCOMPARE(primary.m_nativeRenderModel, observer.m_nativeRenderModel);
			QCOMPARE(observer.m_nativeConsumedRenderRevision,
			         primary.m_nativeRenderModel->m_nativeRenderLineCacheRevision);
			QCOMPARE(primary.m_nativeRenderModel->transformationCount, transformationsBefore);

			for (int mutation = 0; mutation < 4; ++mutation)
			{
				runtime.addLine(QStringLiteral("shared transformation %1").arg(mutation),
				                WorldRuntime::LineOutput, true);
				static_cast<void>(primary.outputLines());
				QCOMPARE(observer.m_nativeConsumedRenderRevision,
				         primary.m_nativeRenderModel->m_nativeRenderLineCacheRevision);
			}
			QCOMPARE(observer.outputLines(), primary.outputLines());
			QCOMPARE(primary.m_nativeRenderModel->transformationCount, transformationsBefore + 4);
			QCOMPARE(renderDeltaSpy.size(), 4);

			observer.setRuntimeObserver(nullptr);
			runtime.addLine(QStringLiteral("after observer detach"), WorldRuntime::LineOutput, true);
			static_cast<void>(primary.outputLines());
			QCOMPARE(renderDeltaSpy.size(), 4);
			{
				WorldView scopedObserver;
				scopedObserver.setRuntimeObserver(&runtime);
				runtime.addLine(QStringLiteral("with scoped observer"), WorldRuntime::LineOutput, true);
				static_cast<void>(primary.outputLines());
				QCOMPARE(renderDeltaSpy.size(), 5);
			}
			runtime.addLine(QStringLiteral("after observer destruction"), WorldRuntime::LineOutput, true);
			static_cast<void>(primary.outputLines());
			QCOMPARE(renderDeltaSpy.size(), 5);
			primary.setRuntime(nullptr);
		}

		/**
		 * @brief Verifies rebinding a view cannot retain another runtime's model revision cursor.
		 */
		static void observerRebindConsumesTheNewRuntimeModel()
		{
			WorldRuntime firstRuntime;
			WorldRuntime secondRuntime;
			WorldView    observer;

			firstRuntime.addLine(QStringLiteral("first runtime"), WorldRuntime::LineOutput, true);
			secondRuntime.addLine(QStringLiteral("second runtime"), WorldRuntime::LineOutput, true);
			observer.setRuntimeObserver(&firstRuntime);
			QCOMPARE(observer.outputLines(), QStringList{QStringLiteral("first runtime")});
			QCOMPARE(observer.m_nativeConsumedRenderRevision,
			         observer.m_nativeRenderModel->m_nativeRenderLineCacheRevision);

			observer.setRuntimeObserver(&secondRuntime);
			QCOMPARE(observer.outputLines(), QStringList{QStringLiteral("second runtime")});
			QCOMPARE(observer.m_nativeConsumedRenderRevision,
			         observer.m_nativeRenderModel->m_nativeRenderLineCacheRevision);
			observer.setRuntimeObserver(nullptr);
		}

		/**
		 * @brief Verifies start-paused is applied once and only to the primary startup presentation.
		 */
		static void startPausedAppliesOnceToStartupPresentations()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("start_paused"), QStringLiteral("1"));
			WorldView primary;
			WorldView observer;
			primary.setRuntime(&runtime);
			observer.setRuntimeObserver(&runtime);
			QVERIFY(!primary.isFrozen());
			QVERIFY(!observer.isFrozen());

			runtime.applyInitialStartPausedState();
			QVERIFY(primary.isFrozen());
			QVERIFY(!observer.isFrozen());
			QVERIFY(runtime.outputFrozen());

			primary.setFrozen(false);
			observer.setFrozen(true);
			QVERIFY(!observer.isFrozen());
			runtime.applyInitialStartPausedState();
			QVERIFY(!primary.isFrozen());
			QVERIFY(!observer.isFrozen());

			WorldView lateObserver;
			lateObserver.setRuntimeObserver(&runtime);
			QVERIFY(!lateObserver.isFrozen());

			primary.setRuntime(nullptr);
			observer.setRuntimeObserver(nullptr);
			lateObserver.setRuntimeObserver(nullptr);
		}

		/**
		 * @brief Verifies observer actions remain read-only and cannot freeze their shared output model.
		 */
		static void observerRoutesPresentationActionsWithoutRuntimeMutation()
		{
			AppController app;
			MainWindow    frame;
			app.setMainWindow(&frame);
			auto *runtime  = new WorldRuntime(&frame);
			auto *primary  = new WorldChildWindow(QStringLiteral("Primary"));
			auto *observer = new WorldChildWindow(QStringLiteral("Observer"));
			runtime->addLine(QStringLiteral("observer action text"), WorldRuntime::LineOutput, true);
			runtime->setWorldAttribute(QStringLiteral("recall_line_preamble"), QStringLiteral("original"));
			runtime->setWorldFileModified(false);
			primary->setRuntime(runtime);
			observer->setRuntimeObserver(runtime);
			frame.addMdiSubWindow(primary, true);
			frame.addMdiSubWindow(observer, false);
			frame.show();
			observer->show();
			frame.activateWorldWindow(observer);
			QCoreApplication::processEvents();

			observer->view()->selectOutputLine(0);
			frame.updateEditActions();
			frame.refreshActionState();
			QVERIFY(frame.actionForCommand(QStringLiteral("Copy"))->isEnabled());
			QVERIFY(frame.actionForCommand(QStringLiteral("Find"))->isEnabled());
			QVERIFY(!frame.actionForCommand(QStringLiteral("FreezeOutput"))->isEnabled());
			QVERIFY(!frame.actionForCommand(QStringLiteral("Preferences"))->isEnabled());
			QVERIFY(!frame.actionForCommand(QStringLiteral("RecallText"))->isEnabled());

			app.onCommandTriggered(QStringLiteral("Copy"));
			QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("observer action text"));
			app.onCommandTriggered(QStringLiteral("FreezeOutput"));
			QVERIFY(!observer->view()->isFrozen());
			QVERIFY(!runtime->outputFrozen());
			observer->view()->setFrozen(true);
			QVERIFY(!observer->view()->isFrozen());
			app.onCommandTriggered(QStringLiteral("RecallText"));
			QCOMPARE(runtime->worldAttributes().value(QStringLiteral("recall_line_preamble")),
			         QStringLiteral("original"));
			QVERIFY(!runtime->worldFileModified());

			primary->setRuntime(nullptr);
			observer->setRuntimeObserver(nullptr);
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
			observer->view()->setFrozen(true);
			QVERIFY(!observer->view()->isFrozen());
			QVERIFY(!runtime->outputFrozen());

			primary->setRuntime(nullptr);
			observer->setRuntimeObserver(nullptr);
		}

		/**
		 * @brief Verifies only the primary presentation can enter split scrollback review.
		 */
		static void splitDividerWidthAppliesOnlyToPrimarySplitView()
		{
			AppController app;
			MainWindow    frame;
			app.setMainWindow(&frame);
			const QString dividerPreferenceKey = QStringLiteral("SplitViewDividerWidth");
			const int     originalDividerWidth = app.getGlobalOption(dividerPreferenceKey).toInt();
			const auto    restoreDividerWidth =
			    qScopeGuard([&app, dividerPreferenceKey, originalDividerWidth]
			                { app.setGlobalOptionInt(dividerPreferenceKey, originalDividerWidth); });
			constexpr int inheritedDividerWidth = 11;
			app.setGlobalOptionInt(dividerPreferenceKey, inheritedDividerWidth);
			frame.resize(900, 700);
			frame.setWindowTabsStyle(0);
			auto *runtime = new WorldRuntime(&frame);
			for (int line = 0; line < 300; ++line)
				runtime->addLine(QStringLiteral("divider-buffer-%1").arg(line), WorldRuntime::LineOutput,
				                 true);

			auto *primary  = new WorldChildWindow(QStringLiteral("Primary"));
			auto *observer = new WorldChildWindow(QStringLiteral("Observer"));
			primary->setRuntime(runtime);
			observer->setRuntimeObserver(runtime);
			primary->view()->restoreOutputFromPersistedLines(runtime->lines());
			observer->view()->restoreOutputFromPersistedLines(runtime->lines());
			frame.addMdiSubWindow(primary, true);
			frame.addMdiSubWindow(observer, false);
			frame.show();
			primary->showNormal();
			observer->showNormal();
			QCoreApplication::processEvents();

			frame.activateWorldWindow(primary);
			QCoreApplication::processEvents();
			QAbstractScrollArea *primaryOutput = findScrollableOutput(*primary->view());
			QVERIFY(primaryOutput);
			QTest::keyClick(primaryOutput->viewport(), Qt::Key_PageUp);
			QTRY_VERIFY(primary->view()->isScrollbackSplitActive());

			frame.activateWorldWindow(observer);
			QCoreApplication::processEvents();
			QAbstractScrollArea *observerOutput = findScrollableOutput(*observer->view());
			QVERIFY(observerOutput);
			const int observerEndPosition = observerOutput->verticalScrollBar()->value();
			QTest::keyClick(observerOutput->viewport(), Qt::Key_PageUp);
			QTRY_VERIFY(observerOutput->verticalScrollBar()->value() < observerEndPosition);
			QVERIFY(!observer->view()->isScrollbackSplitActive());

			QSplitter *primarySplitter  = findOutputSplitter(*primary->view());
			QSplitter *observerSplitter = findOutputSplitter(*observer->view());
			QVERIFY(primarySplitter);
			QVERIFY(observerSplitter);
			QCOMPARE(primarySplitter->handleWidth(), inheritedDividerWidth);
			QCOMPARE(observerSplitter->handleWidth(), 0);

			app.setGlobalOptionInt(dividerPreferenceKey, 17);
			app.applyRenderingPreferences();
			QCOMPARE(primarySplitter->handleWidth(), 17);
			QCOMPARE(observerSplitter->handleWidth(), 0);

			primary->setRuntime(nullptr);
			observer->setRuntimeObserver(nullptr);
		}

		/**
		 * @brief Verifies observers follow appended output only while already positioned at the tail.
		 */
		static void observerTailFollowPreservesScrollbackReviewPosition()
		{
			WorldRuntime runtime;
			for (int line = 0; line < 300; ++line)
				runtime.addLine(QStringLiteral("observer-follow-%1").arg(line), WorldRuntime::LineOutput,
				                true);

			WorldView primary;
			WorldView observer;
			primary.setRuntime(&runtime);
			observer.setRuntimeObserver(&runtime);
			observer.resize(720, 360);
			observer.show();
			QVERIFY(QTest::qWaitForWindowExposed(&observer));
			QCoreApplication::processEvents();

			QAbstractScrollArea *const output = findScrollableOutput(observer);
			QVERIFY(output);
			QScrollBar *const scroll = output->verticalScrollBar();
			QVERIFY(scroll);
			scroll->setValue(scroll->maximum());
			const int firstMaximum = scroll->maximum();

			runtime.addLine(QStringLiteral("observer-follow-tail"), WorldRuntime::LineOutput, true);
			QTRY_VERIFY(scroll->maximum() > firstMaximum);
			QTRY_COMPARE(scroll->value(), scroll->maximum());

			scroll->setValue(qMax(scroll->minimum(), scroll->maximum() / 2));
			const int reviewPosition = scroll->value();
			QVERIFY(reviewPosition < scroll->maximum());
			const int reviewMaximum = scroll->maximum();
			runtime.addLine(QStringLiteral("observer-follow-while-reviewing"), WorldRuntime::LineOutput,
			                true);
			QTRY_VERIFY(scroll->maximum() > reviewMaximum);
			QCOMPARE(scroll->value(), reviewPosition);
			QVERIFY(!observer.isScrollbackSplitActive());

			primary.setRuntime(nullptr);
			observer.setRuntimeObserver(nullptr);
		}

		/**
		 * @brief Verifies capped wrapped-line head trimming preserves an observer's reviewed source position.
		 */
		static void observerHeadTrimPreservesReviewedSourcePosition()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("max_output_lines"), QStringLiteral("300"));
			for (int line = 0; line < 300; ++line)
			{
				const QString text =
				    line == 0 ? QStringLiteral("observer-trim-wrapped-head ").repeated(80)
				              : QStringLiteral("observer-trim-%1").arg(line, 3, 10, QLatin1Char('0'));
				runtime.addLine(text, WorldRuntime::LineOutput, true);
			}

			WorldView primary;
			WorldView observer;
			primary.setRuntime(&runtime);
			observer.setRuntimeObserver(&runtime);
			observer.resize(720, 360);
			observer.show();
			QVERIFY(QTest::qWaitForWindowExposed(&observer));
			QCoreApplication::processEvents();

			QAbstractScrollArea *const output = findScrollableOutput(observer);
			QVERIFY(output);
			QScrollBar *const scroll = output->verticalScrollBar();
			QVERIFY(scroll);
			QVERIFY(scroll->maximum() > scroll->minimum());
			scroll->setValue(scroll->maximum() / 2);
			QVERIFY(scroll->value() < scroll->maximum());

			const auto canvas = observer.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(canvas);
			canvas->repaint();
			QCoreApplication::processEvents();
			QVERIFY(observer.m_nativePrimaryPaintState.valid);
			QVERIFY(!observer.m_nativePrimaryPaintState.visibleFirstRuntimeLineNumbers.isEmpty());
			QVERIFY(!observer.m_nativePrimaryPaintState.visibleLineTops.isEmpty());
			const qint64 reviewedLine =
			    observer.m_nativePrimaryPaintState.visibleFirstRuntimeLineNumbers.constFirst();
			const int reviewedTop = observer.m_nativePrimaryPaintState.visibleLineTops.constFirst();

			runtime.addLine(QStringLiteral("observer-trim-300"), WorldRuntime::LineOutput, true);
			QCoreApplication::processEvents();
			canvas->repaint();
			QCoreApplication::processEvents();
			QCOMPARE(runtime.lines().size(), 300);
			QVERIFY(observer.m_nativePrimaryPaintState.valid);
			QCOMPARE(observer.m_nativePrimaryPaintState.visibleFirstRuntimeLineNumbers.constFirst(),
			         reviewedLine);
			QCOMPARE(observer.m_nativePrimaryPaintState.visibleLineTops.constFirst(), reviewedTop);

			scroll->setValue(scroll->maximum());
			runtime.addLine(QStringLiteral("observer-trim-301"), WorldRuntime::LineOutput, true);
			QCoreApplication::processEvents();
			QCOMPARE(scroll->value(), scroll->maximum());
			QVERIFY(!observer.isScrollbackSplitActive());

			primary.setRuntime(nullptr);
			observer.setRuntimeObserver(nullptr);
		}

		/**
		 * @brief Verifies a capped causal restitch preserves an observer's reviewed source position.
		 */
		static void observerCappedCausalRestitchPreservesReviewedSourcePosition()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("max_output_lines"), QStringLiteral("300"));
			runtime.setWorldAttribute(QStringLiteral("wrap"), QStringLiteral("1"));
			runtime.setWorldAttribute(QStringLiteral("auto_wrap_window_width"), QStringLiteral("1"));
			for (int line = 0; line < 300; ++line)
			{
				runtime.addLine(QStringLiteral("observer-restitch-%1").arg(line, 3, 10, QLatin1Char('0')),
				                WorldRuntime::LineOutput, true);
			}

			WorldView primary;
			WorldView observer;
			primary.setRuntime(&runtime);
			observer.setRuntimeObserver(&runtime);
			observer.resize(480, 360);
			observer.show();
			QVERIFY(QTest::qWaitForWindowExposed(&observer));
			QCoreApplication::processEvents();

			QAbstractScrollArea *const output = findScrollableOutput(observer);
			QVERIFY(output);
			QScrollBar *const scroll = output->verticalScrollBar();
			QVERIFY(scroll);
			scroll->setValue(scroll->maximum() / 2);
			QVERIFY(scroll->value() < scroll->maximum());

			const auto canvas = observer.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(canvas);
			canvas->repaint();
			QCoreApplication::processEvents();
			QVERIFY(observer.m_nativePrimaryPaintState.valid);
			QVERIFY(!observer.m_nativePrimaryPaintState.visibleFirstRuntimeLineNumbers.isEmpty());
			QVERIFY(!observer.m_nativePrimaryPaintState.visibleLineTops.isEmpty());
			const qint64 reviewedLine =
			    observer.m_nativePrimaryPaintState.visibleFirstRuntimeLineNumbers.constFirst();
			const int    reviewedTop     = observer.m_nativePrimaryPaintState.visibleLineTops.constFirst();
			const qint64 firstLineNumber = runtime.lines().constFirst().lineNumber;

			runtime.beginOutputViewMutationBatch();
			const bool insertionApplied = runtime.writeLuaCallbackOutputAtLineAnchor(
			    firstLineNumber, 1, 1, false,
			    QStringLiteral("observer wrapped insertion ").repeated(40) +
			        QStringLiteral("\nobserver-insert-2\nobserver-insert-3"),
			    WorldRuntime::LineNote, {}, true);
			runtime.endOutputViewMutationBatch();
			QVERIFY(insertionApplied);
			QCoreApplication::processEvents();
			canvas->repaint();
			QCoreApplication::processEvents();

			QCOMPARE(runtime.lines().size(), 300);
			QVERIFY(observer.m_nativePrimaryPaintState.valid);
			QCOMPARE(observer.m_nativePrimaryPaintState.visibleFirstRuntimeLineNumbers.constFirst(),
			         reviewedLine);
			QCOMPARE(observer.m_nativePrimaryPaintState.visibleLineTops.constFirst(), reviewedTop);
			QVERIFY(!observer.isScrollbackSplitActive());

			primary.setRuntime(nullptr);
			observer.setRuntimeObserver(nullptr);
		}

		/**
		 * @brief Verifies observer text selection cannot replace the runtime word-under-menu state.
		 */
		static void observerSelectionDoesNotMutateRuntimeWordUnderMenu()
		{
			WorldRuntime runtime;
			for (int line = 0; line < 300; ++line)
				runtime.addLine(QStringLiteral("observer-selection-%1").arg(line), WorldRuntime::LineOutput,
				                true);

			WorldView primary;
			WorldView observer;
			primary.setRuntime(&runtime);
			observer.setRuntimeObserver(&runtime);
			observer.resize(720, 360);
			observer.show();
			QVERIFY(QTest::qWaitForWindowExposed(&observer));
			QCoreApplication::processEvents();

			QAbstractScrollArea *const output = findScrollableOutput(observer);
			QVERIFY(output);
			QVERIFY(output->viewport());
			runtime.setWordUnderMenu(QStringLiteral("primary-word"), true);
			QTest::mouseDClick(output->viewport(), Qt::LeftButton, Qt::NoModifier,
			                   QPoint(12, qMax(1, output->fontMetrics().height() / 2)));
			QCoreApplication::processEvents();

			QVERIFY(observer.hasOutputSelection());
			QCOMPARE(runtime.wordUnderMenu(), QStringLiteral("primary-word"));
			QVERIFY(runtime.wordUnderMenuResolved());

			primary.setRuntime(nullptr);
			observer.setRuntimeObserver(nullptr);
		}

		/**
		 * @brief Verifies runtime presentation settings remain synchronized across primary and observer views.
		 */
		static void runtimeSettingsApplyToPrimaryAndObserver()
		{
			MainWindow frame;
			auto      *runtime  = new WorldRuntime(&frame);
			auto      *primary  = new WorldChildWindow(QStringLiteral("Primary"));
			auto      *observer = new WorldChildWindow(QStringLiteral("Observer"));

			primary->setRuntime(runtime);
			observer->setRuntimeObserver(runtime);
			frame.addMdiSubWindow(primary, true);
			frame.addMdiSubWindow(observer, false);

			runtime->setWorldAttribute(QStringLiteral("use_default_output_font"), QStringLiteral("0"));
			runtime->setWorldAttribute(QStringLiteral("output_font_height"), QStringLiteral("17"));
			runtime->applyRuntimeSettingsToPresentationViews();

			QCOMPARE(primary->view()->outputFont().pointSize(), 17);
			QCOMPARE(observer->view()->outputFont().pointSize(), 17);

			primary->setRuntime(nullptr);
			observer->setRuntimeObserver(nullptr);
		}

		/**
		 * @brief Verifies partial output is one shared runtime presentation for current and late observers.
		 */
		static void partialOutputPresentationIsSharedWithObservers()
		{
			WorldRuntime          runtime;
			WorldCommandProcessor processor;
			WorldView             primary;
			WorldView             observer;
			primary.setRuntime(&runtime);
			observer.setRuntimeObserver(&runtime);
			processor.setView(&primary);
			processor.setRuntime(&runtime);
			runtime.setCommandProcessor(&processor);
			QObject::connect(&runtime, &WorldRuntime::incomingStyledLineReceived, &processor,
			                 &WorldCommandProcessor::onIncomingStyledLineReceived);
			QObject::connect(&runtime, &WorldRuntime::incomingStyledLinePartialReceived, &processor,
			                 &WorldCommandProcessor::onIncomingStyledLinePartialReceived);

			primary.setFrozen(true);
			observer.setFrozen(true);
			QVERIFY(primary.isFrozen());
			QVERIFY(!observer.isFrozen());

			runtime.receiveRawData(QByteArrayLiteral("shared partial"));
			QTRY_COMPARE(primary.outputLines(), QStringList{QStringLiteral("shared partial")});
			QTRY_COMPARE(observer.outputLines(), QStringList{QStringLiteral("shared partial")});

			WorldView lateObserver;
			lateObserver.setRuntimeObserver(&runtime);
			QCOMPARE(lateObserver.outputLines(), QStringList{QStringLiteral("shared partial")});

			runtime.receiveRawData(QByteArrayLiteral(" completed\n"));
			const QStringList completed{QStringLiteral("shared partial completed")};
			QTRY_COMPARE(primary.outputLines(), completed);
			QTRY_COMPARE(observer.outputLines(), completed);
			QTRY_COMPARE(lateObserver.outputLines(), completed);

			runtime.setCommandProcessor(nullptr);
			processor.setRuntime(nullptr);
			primary.setRuntime(nullptr);
			observer.setRuntimeObserver(nullptr);
			lateObserver.setRuntimeObserver(nullptr);
		}

		/**
		 * @brief Verifies active output batches pin every view to the last completed presentation.
		 */
		static void outputPresentationRemainsPinnedUntilBatchCompletion()
		{
			WorldRuntime          runtime;
			WorldCommandProcessor processor;
			WorldView             primary;
			WorldView             observer;
			primary.setRuntime(&runtime);
			observer.setRuntimeObserver(&runtime);
			processor.setView(&primary);
			processor.setRuntime(&runtime);
			runtime.setCommandProcessor(&processor);
			QObject::connect(&runtime, &WorldRuntime::incomingStyledLineReceived, &processor,
			                 &WorldCommandProcessor::onIncomingStyledLineReceived);
			QObject::connect(&runtime, &WorldRuntime::incomingStyledLinePartialReceived, &processor,
			                 &WorldCommandProcessor::onIncomingStyledLinePartialReceived);

			QSignalSpy partialCompletionSpy(&runtime, &WorldRuntime::partialOutputPresentationCompleted);
			runtime.receiveRawData(QByteArrayLiteral("presented\n"));
			QTRY_COMPARE(primary.outputLines(), QStringList{QStringLiteral("presented")});
			QTRY_COMPARE(observer.outputLines(), QStringList{QStringLiteral("presented")});
			QCOMPARE(partialCompletionSpy.count(), 0);

			runtime.receiveRawData(QByteArrayLiteral("partial"));
			QTRY_COMPARE(primary.outputLines(),
			             QStringList({QStringLiteral("presented"), QStringLiteral("partial")}));
			QTRY_COMPARE(observer.outputLines(),
			             QStringList({QStringLiteral("presented"), QStringLiteral("partial")}));
			runtime.receiveRawData(QByteArrayLiteral(" completed\n"));
			QTRY_COMPARE(partialCompletionSpy.count(), 1);
			const QStringList presentedLines{QStringLiteral("presented"),
			                                 QStringLiteral("partial completed")};
			QTRY_COMPARE(primary.outputLines(), presentedLines);
			QTRY_COMPARE(observer.outputLines(), presentedLines);

			primary.requestNativeRuntimeOutputPresentationSync(false, true);
			observer.requestNativeRuntimeOutputPresentationSync(false, true);
			QVERIFY(primary.m_nativeRuntimeOutputPresentationQueued);
			QVERIFY(observer.m_nativeRuntimeOutputPresentationQueued);

			runtime.beginOutputViewMutationBatch();
			runtime.addLine(QStringLiteral("held-one"), WorldRuntime::LineOutput, true);
			runtime.addLine(QStringLiteral("held-two"), WorldRuntime::LineOutput, true);
			primary.requestNativeRuntimeOutputPresentationSync(false, true);
			observer.requestNativeRuntimeOutputPresentationSync(false, true);
			QCoreApplication::processEvents();

			QCOMPARE(primary.outputLines(), presentedLines);
			QCOMPARE(observer.outputLines(), presentedLines);
			QVERIFY(primary.m_nativeRuntimeOutputPresentationDeferredUntilBatchEnd);
			QVERIFY(observer.m_nativeRuntimeOutputPresentationDeferredUntilBatchEnd);

			runtime.endOutputViewMutationBatch();
			const QStringList completedLines{QStringLiteral("presented"), QStringLiteral("partial completed"),
			                                 QStringLiteral("held-one"), QStringLiteral("held-two")};
			QTRY_COMPARE(primary.outputLines(), completedLines);
			QTRY_COMPARE(observer.outputLines(), completedLines);
			QVERIFY(!primary.m_nativeRuntimeOutputPresentationDeferredUntilBatchEnd);
			QVERIFY(!observer.m_nativeRuntimeOutputPresentationDeferredUntilBatchEnd);

			runtime.setCommandProcessor(nullptr);
			processor.setRuntime(nullptr);
			primary.setRuntime(nullptr);
			observer.setRuntimeObserver(nullptr);
		}

		/**
		 * @brief Verifies a batched output clear synchronizes every view with the final runtime buffer.
		 */
		static void batchedOutputClearSynchronizesObservers()
		{
			WorldRuntime runtime;
			WorldView    primary;
			WorldView    observer;
			primary.setRuntime(&runtime);
			observer.setRuntimeObserver(&runtime);
			runtime.addLine(QStringLiteral("before-clear"), WorldRuntime::LineOutput, true);
			QTRY_COMPARE(primary.outputLines(), QStringList{QStringLiteral("before-clear")});
			QTRY_COMPARE(observer.outputLines(), QStringList{QStringLiteral("before-clear")});
			primary.setOutputSelection(1, 1, 1, 7);
			observer.setOutputSelection(1, 1, 1, 7);
			QVERIFY(primary.hasOutputSelection());
			QVERIFY(observer.hasOutputSelection());

			runtime.beginOutputViewMutationBatch();
			QVector<WorldRuntime::LineEntry> rebuiltLines;
			rebuiltLines.reserve(runtime.lines().size());
			for (const WorldRuntime::LineEntry &line : runtime.lines())
				rebuiltLines.push_back(line);
			runtime.replaceOutputLines(rebuiltLines);
			QVERIFY(primary.m_runtimeOutputMutationBatchNeedsRebuild);
			QVERIFY(observer.m_runtimeOutputMutationBatchNeedsRebuild);
			runtime.deleteOutput();
			QVERIFY(observer.m_nativeRuntimeOutputPresentationDeferredUntilBatchEnd);
			runtime.addLine(QStringLiteral("after-clear"), WorldRuntime::LineOutput, true);
			runtime.endOutputViewMutationBatch();

			const QStringList expected{QStringLiteral("after-clear")};
			QTRY_COMPARE(primary.outputLines(), expected);
			QTRY_COMPARE(observer.outputLines(), expected);
			QTRY_COMPARE(primary.m_nativeConsumedRenderRevision, observer.m_nativeConsumedRenderRevision);
			QVERIFY(!primary.hasOutputSelection());
			QVERIFY(!observer.hasOutputSelection());

			primary.setRuntime(nullptr);
			observer.setRuntimeObserver(nullptr);
		}

		/**
		 * @brief Verifies observers survive a mid-batch rebuild and attachment without replaying old edits.
		 */
		static void observerAttachedDuringOutputBatchUsesFinalRuntimeState()
		{
			WorldRuntime runtime;
			runtime.addLine(QStringLiteral("before-batch"), WorldRuntime::LineOutput, true);
			WorldView existingObserver;
			existingObserver.setRuntimeObserver(&runtime);
			QCOMPARE(existingObserver.outputLines(), QStringList{QStringLiteral("before-batch")});

			runtime.beginOutputViewMutationBatch();
			runtime.addLine(QStringLiteral("before-attach"), WorldRuntime::LineOutput, true);
			runtime.setWorldAttribute(QStringLiteral("output_font_height"), QStringLiteral("17"));
			runtime.applyRuntimeSettingsToPresentationViews();
			QCOMPARE(existingObserver.outputLines(), QStringList{QStringLiteral("before-batch")});

			WorldView lateObserver;
			lateObserver.setRuntimeObserver(&runtime);
			QCOMPARE(existingObserver.outputLines(), QStringList{QStringLiteral("before-batch")});
			QCOMPARE(lateObserver.outputLines(), QStringList{QStringLiteral("before-batch")});
			runtime.addLine(QStringLiteral("after-attach"), WorldRuntime::LineOutput, true);
			QCOMPARE(existingObserver.outputLines(), QStringList{QStringLiteral("before-batch")});
			QCOMPARE(lateObserver.outputLines(), QStringList{QStringLiteral("before-batch")});
			runtime.endOutputViewMutationBatch();

			const QStringList expected{QStringLiteral("before-batch"), QStringLiteral("before-attach"),
			                           QStringLiteral("after-attach")};
			QCOMPARE(existingObserver.outputLines(), expected);
			QCOMPARE(lateObserver.outputLines(), expected);
			existingObserver.setRuntimeObserver(nullptr);
			lateObserver.setRuntimeObserver(nullptr);
		}

		/**
		 * @brief Verifies alpha-option presentation metadata updates every runtime presentation.
		 */
		static void alphaOptionPresentationEffectsReachObservers()
		{
			WorldRuntime runtime;
			WorldView    primary;
			WorldView    observer;
			primary.setRuntime(&runtime);
			observer.setRuntimeObserver(&runtime);
			runtime.addLine(QStringLiteral("timestamp target"), WorldRuntime::LineOutput, true);

			runtime.applyAlphaWorldOption(QStringLiteral("timestamp_output"), QStringLiteral("[shared] "),
			                              false, OPT_UPDATE_VIEWS);
			QTRY_VERIFY(primary.outputLines().constFirst().startsWith(QStringLiteral("[shared] ")));
			QTRY_VERIFY(observer.outputLines().constFirst().startsWith(QStringLiteral("[shared] ")));

			primary.setRuntime(nullptr);
			observer.setRuntimeObserver(nullptr);
		}

		static void closeUsesPresentationAndFinalRuntimeLifecycles()
		{
			MainWindow frame;
			auto      *runtime = new WorldRuntime(&frame);
			runtime->setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
			runtime->setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
			runtime->setWorldAttribute(QStringLiteral("on_world_close"), QStringLiteral("qcb_close"));
			WorldRuntimeTestAccess::initializeLuaScriptText(*runtime, QStringLiteral(R"lua(
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

QTEST_MAIN(tst_WorldObserverLifecycle)

#include "tst_WorldObserverLifecycle.moc"
