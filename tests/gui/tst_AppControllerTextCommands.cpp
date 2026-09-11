/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_AppControllerTextCommands.cpp
 * Role: Regression coverage for application text-window commands targeting their associated world.
 */

#include <QFileInfo>

#include "AppController.h"
#include "LuaCallbackEngine.h"
#include "LuaExecutor.h"
#include "LuaSupport.h"
#include "MainFrame.h"
#include "NameGeneration.h"
#include "WorldChildWindow.h"
#include "WorldCommandProcessor.h"
#include "WorldOptions.h"
#include "WorldRuntime.h"
#include "WorldRuntimeTestAccess.h"
#include "WorldView.h"
#include "dialogs/GeneratedNameDialog.h"
#include "scripting/ScriptingErrors.h"

#include <QAction>
#include <QCoreApplication>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryDir>
#include <QThread>
#include <QtTest/QTest>

#include <algorithm>
#include <array>
#include <functional>
#include <memory>

/**
 * @brief Provides narrowly scoped access to the spell-check working directory for GUI tests.
 */
class AppControllerTestAccess final
{
	public:
		/**
		 * @brief Directs spell-check asset discovery to an isolated test directory.
		 * @param controller Application controller under test.
		 * @param workingDirectory Directory containing the test spell subdirectory.
		 */
		static void setWorkingDirectory(AppController &controller, const QString &workingDirectory)
		{
			controller.m_workingDir = QDir::cleanPath(workingDirectory) + QLatin1Char('/');
		}
#ifdef QMUD_ENABLE_LUA_SCRIPTING
		/**
		 * @brief Executes setup code in the dedicated spell-check Lua state.
		 * @param controller Application controller under test.
		 * @param worldOwner World owning the state.
		 * @param script Lua setup code.
		 * @return `true` when the setup code completed successfully.
		 */
		static bool executeSpellCheckerScript(AppController &controller, const WorldRuntime *worldOwner,
		                                      const QByteArray &script)
		{
			const AppController::SpellCheckerStateLease lease =
			    controller.acquireSpellCheckerState(worldOwner);
			lua_State *state = lease.state;
			if (!state)
				return false;
			bool       reusable     = true;
			const auto releaseState = qScopeGuard([&controller, lease, &reusable]
			                                      { controller.releaseSpellCheckerState(lease, reusable); });
			Q_UNUSED(releaseState);
			lua_settop(state, 0);
			const int loadStatus =
			    luaL_loadbuffer(state, script.constData(), script.size(), "spell-check test");
			const int callStatus =
			    loadStatus == LUA_OK ? QMudLuaSupport::callLuaProtected(state, 0, 0, 0) : loadStatus;
			lua_settop(state, 0);
			reusable = callStatus == LUA_OK;
			return callStatus == LUA_OK;
		}

		/**
		 * @brief Runs the public SpellCheck API through a world's production worker executor.
		 * @param runtime World whose callback worker performs the spell check.
		 * @param text Text expected to produce a spelling-error table.
		 * @return `true` when SpellCheck executes on the worker and returns a table.
		 */
		static bool executeWorkerSpellCheck(WorldRuntime &runtime, const QString &text)
		{
			LuaCallbackEngine  *engine   = runtime.luaCallbacks();
			const ILuaExecutor *executor = runtime.luaExecutor();
			if (!engine || !executor)
				return false;

			runtime.setLuaScriptText(QStringLiteral(R"lua(
function qmud_test_worker_spellcheck(value)
  return type(SpellCheck(value))
end
)lua"));
			const QSharedPointer<LuaCallbackEngine> engineRef(engine, [](LuaCallbackEngine * /*unused*/) {});
			LuaBatchDispatchRequest                 request;
			request.engines      = {engineRef};
			request.kind         = LuaBatchDispatchKind::StringInOut;
			request.functionName = QStringLiteral("qmud_test_worker_spellcheck");
			request.stringArg    = text;
			return executor->dispatchBatch(request).stringResult == QStringLiteral("table");
		}

		/**
		 * @brief Verifies concurrent leases use independent Lua states sharing dictionary storage.
		 * @param controller Application controller under test.
		 * @param worldOwner World owning the states.
		 * @param word Word to add through a third concurrent state.
		 * @return `true` when an already-open independent state observes the added word.
		 */
		static bool verifyConcurrentSpellCheckerStates(AppController      &controller,
		                                               const WorldRuntime *worldOwner, const QString &word)
		{
			const AppController::SpellCheckerStateLease first =
			    controller.acquireSpellCheckerState(worldOwner);
			const AppController::SpellCheckerStateLease second =
			    controller.acquireSpellCheckerState(worldOwner);
			bool       firstReusable  = first.state != nullptr;
			bool       secondReusable = second.state != nullptr;
			const auto releaseStates  = qScopeGuard(
			    [&controller, first, second, &firstReusable, &secondReusable]
			    {
				    controller.releaseSpellCheckerState(second, secondReusable);
				    controller.releaseSpellCheckerState(first, firstReusable);
			    });
			Q_UNUSED(releaseStates);
			if (!first.state || !second.state || first.state == second.state)
				return false;

			if (controller.addSpellCheckWord(worldOwner, word.toUtf8(), QByteArrayLiteral("i"), {}) != eOK)
				return false;

			lua_State *reader = second.state;
			lua_settop(reader, 0);
			lua_getglobal(reader, "spellcheck_string");
			if (!lua_isfunction(reader, -1))
			{
				secondReusable = false;
				return false;
			}
			const QByteArray wordBytes = word.toUtf8();
			lua_pushlstring(reader, wordBytes.constData(), wordBytes.size());
			if (QMudLuaSupport::callLuaWithTraceback(reader, 1, 1) != LUA_OK)
			{
				secondReusable = false;
				lua_settop(reader, 0);
				return false;
			}
			const bool found = lua_istable(reader, -1) && lua_rawlen(reader, -1) == 0;
			lua_settop(reader, 0);
			return found;
		}

		/**
		 * @brief Counts idle spell-check states belonging to a world.
		 * @param controller Application controller under test.
		 * @param worldOwner World identity to count.
		 * @return Number of matching idle states.
		 */
		static qsizetype idleSpellCheckerStateCount(AppController &controller, const WorldRuntime *worldOwner)
		{
			QMutexLocker locker(&controller.m_spellCheckerPoolMutex);
			return std::ranges::count_if(controller.m_idleSpellCheckerStates,
			                             [worldOwner](const AppController::IdleSpellCheckerState &entry)
			                             { return entry.worldOwner == worldOwner; });
		}

		/**
		 * @brief Reports whether a world owns an idle spell-check state on a worker thread.
		 * @param controller Application controller under test.
		 * @param worldOwner World identity to inspect.
		 * @return `true` when a matching state is owned outside the calling thread.
		 */
		static bool hasWorkerSpellCheckerState(AppController &controller, const WorldRuntime *worldOwner)
		{
			QMutexLocker locker(&controller.m_spellCheckerPoolMutex);
			return std::ranges::any_of(
			    controller.m_idleSpellCheckerStates,
			    [worldOwner](const AppController::IdleSpellCheckerState &entry)
			    { return entry.worldOwner == worldOwner && entry.ownerThread != QThread::currentThread(); });
		}

		/**
		 * @brief Returns the number of worlds registered with the spell-check pool.
		 * @param controller Application controller under test.
		 * @return Registered world count.
		 */
		static qsizetype spellCheckerWorldCount(AppController &controller)
		{
			QMutexLocker locker(&controller.m_spellCheckerPoolMutex);
			return controller.m_spellCheckerWorldOwners.size();
		}

		/**
		 * @brief Returns the current spell-check state generation.
		 * @param controller Application controller under test.
		 * @return Current generation value.
		 */
		static quint64 spellCheckerGeneration(AppController &controller)
		{
			QMutexLocker locker(&controller.m_spellCheckerPoolMutex);
			return controller.m_spellCheckerGeneration;
		}
#endif
};

namespace
{

	/**
	 * @brief Verifies text-window commands use their recorded world ownership and preserve user-action policy.
	 */
	class tst_AppControllerTextCommands final : public QObject
	{
			Q_OBJECT

		private slots:
#ifdef QMUD_ENABLE_LUA_SCRIPTING
			/**
			 * @brief Verifies every public spell-check path has the utilities required by the spell script.
			 */
			static void spellCheckerStateProvidesRequiredUtilities()
			{
				const QString sourceScript = QFINDTESTDATA("../../skeleton/spell/spellchecker.lua");
				QVERIFY(!sourceScript.isEmpty());
				const QDir    sourceDirectory = QFileInfo(sourceScript).absoluteDir();
				QTemporaryDir temporaryDirectory(QDir(QCoreApplication::applicationDirPath())
				                                     .filePath(QStringLiteral("qmud-spell-XXXXXX")));
				QVERIFY(temporaryDirectory.isValid());
				QDir testRoot(temporaryDirectory.path());
				QVERIFY(testRoot.mkpath(QStringLiteral("spell")));
				for (const QString &fileName :
				     {QStringLiteral("spellchecker.lua"), QStringLiteral("spell.sqlite"),
				      QStringLiteral("userdict.txt")})
				{
					const QString source = sourceDirectory.filePath(fileName);
					const QString target = testRoot.filePath(QStringLiteral("spell/") + fileName);
					QVERIFY2(QFile::copy(source, target),
					         qPrintable(QStringLiteral("Unable to copy %1").arg(source)));
				}

				AppController controller;
				WorldRuntime  runtime;
				AppControllerTestAccess::setWorkingDirectory(controller, testRoot.absolutePath());
				QVERIFY(controller.ensureSpellCheckerLoaded());
				QVERIFY(AppControllerTestAccess::executeSpellCheckerScript(controller, &runtime,
				                                                           QByteArrayLiteral(R"lua(
assert(package.loadlib == nil, "spell checker must not load native libraries")
assert(package.searchers[3] == nil and package.searchers[4] == nil,
       "spell checker must not have native module searchers")
)lua")));

				const QString  unknownWord = QStringLiteral("qmudzzzxxyy");
				const QVariant initial =
				    controller.spellCheckString(&runtime, unknownWord, QStringLiteral("test.SpellCheck"));
				QVERIFY(initial.isValid());
				QCOMPARE(initial.toStringList(), QStringList({unknownWord}));

				QVERIFY(AppControllerTestAccess::verifyConcurrentSpellCheckerStates(controller, &runtime,
				                                                                    unknownWord));
				QVERIFY(AppControllerTestAccess::idleSpellCheckerStateCount(controller, &runtime) > 0);
				QCOMPARE(AppControllerTestAccess::spellCheckerWorldCount(controller), 1);
				const WorldRuntime *closedWorldIdentity = nullptr;
				{
					auto closingWorld                 = std::make_unique<WorldRuntime>();
					closedWorldIdentity               = closingWorld.get();
					const QVariant closingWorldResult = controller.spellCheckString(
					    closingWorld.get(), unknownWord, QStringLiteral("test.worldOwnership"));
					QVERIFY(closingWorldResult.isValid());
					QVERIFY(AppControllerTestAccess::idleSpellCheckerStateCount(controller,
					                                                            closingWorld.get()) > 0);
					QCOMPARE(AppControllerTestAccess::spellCheckerWorldCount(controller), 2);
				}
				QCOMPARE(AppControllerTestAccess::idleSpellCheckerStateCount(controller, closedWorldIdentity),
				         0);
				QVERIFY(AppControllerTestAccess::idleSpellCheckerStateCount(controller, &runtime) > 0);
				QCOMPARE(AppControllerTestAccess::spellCheckerWorldCount(controller), 1);
				const QVariant added = controller.spellCheckString(&runtime, unknownWord,
				                                                   QStringLiteral("test.AddSpellCheckWord"));
				QVERIFY(added.isValid());
				QVERIFY(added.toStringList().isEmpty());

				const QVariant smith = controller.spellCheckString(&runtime, QStringLiteral("smith"),
				                                                   QStringLiteral("test.edit_distance"));
				QVERIFY(smith.isValid());
				QVERIFY(smith.toStringList().isEmpty());
				const QVariant smyth = controller.spellCheckString(&runtime, QStringLiteral("smyth"),
				                                                   QStringLiteral("test.edit_distance"));
				QVERIFY(smyth.isValid());
				QCOMPARE(smyth.toStringList(), QStringList({QStringLiteral("smyth")}));
				QVERIFY(AppControllerTestAccess::executeSpellCheckerScript(controller, &runtime,
				                                                           QByteArrayLiteral(R"lua(
local real_edit_distance = utils.edit_distance
edit_distance_calls = 0
utils.edit_distance = function(...)
  edit_distance_calls = edit_distance_calls + 1
  return real_edit_distance(...)
end
utils.spellcheckdialog = function(word)
  return "ignore", word
end
)lua")));
				const AppController::SpellCommandResult commandResult = controller.spellCheckCommandText(
				    &runtime, QStringLiteral("smyth"), true, QStringLiteral("test.SpellCheckCommand"));
				QCOMPARE(commandResult.status, 1);
				QCOMPARE(commandResult.replacement, QStringLiteral("smyth"));
				QVERIFY(AppControllerTestAccess::executeSpellCheckerScript(
				    controller, &runtime,
				    QByteArrayLiteral("assert(edit_distance_calls > 0, 'edit_distance was not called')")));
				QCOMPARE(controller.addSpellCheckWord(&runtime, QByteArrayLiteral("smyth"),
				                                      QByteArrayLiteral("i"), {}),
				         eOK);

				const AppController::SpellCommandResult dialogResult = controller.spellCheckDialogText(
				    &runtime, QStringLiteral("smyth"), QStringLiteral("test.SpellCheckDlg"));
				QCOMPARE(dialogResult.status, 1);
				QCOMPARE(dialogResult.replacement, QStringLiteral("smyth"));

				QVERIFY(AppControllerTestAccess::executeSpellCheckerScript(controller, &runtime,
				                                                           QByteArrayLiteral(R"lua(
function spellcheck(...)
  assert(select("#", ...) == 1, "SpellCheckDlg must pass exactly one argument")
  return "dialog:" .. (...)
end
)lua")));

				LuaCallbackEngine engine;
				engine.setWorldRuntime(&runtime);
				engine.setPluginInfo(QStringLiteral("spell-test"), QStringLiteral("Spell test"));
				engine.setScriptText(QStringLiteral(R"lua(
function check_dialog_binding()
  return SpellCheckDlg("probe") == "dialog:probe"
end

function check_command_binding()
  local result = SpellCheckCommand()
  return result == 1 and
         GetCommand() == "corrected" and
         GetInfo(236) == 4 and
         GetInfo(237) == 0
end
)lua"));
				QVERIFY(engine.loadScript());

				bool                         hasFunction = false;
				bool                         suspended   = false;
				quint64                      resumeId    = 0;
				LuaPendingModalStringRequest modalRequest;
				QVERIFY(!engine.callFunctionNoArgs(QStringLiteral("check_dialog_binding"), &hasFunction,
				                                   false, -1, &suspended, &resumeId, &modalRequest));
				QVERIFY(hasFunction);
				QVERIFY(suspended);
				QVERIFY(resumeId != 0);
				QVERIFY(modalRequest.guiCallable);
				QVERIFY(modalRequest.resultCallback);
				const QString          dialogResumeResult = modalRequest.guiCallable();
				LuaBatchDispatchResult resumed =
				    engine.resumeSuspendedModalString(resumeId, dialogResumeResult);
				QVERIFY(!resumed.suspended);
				QVERIFY(resumed.boolResultValid);
				QVERIFY(resumed.boolResult);

				QVERIFY(AppControllerTestAccess::executeSpellCheckerScript(controller, &runtime,
				                                                           QByteArrayLiteral(R"lua(
function spellcheck(...)
  assert(select("#", ...) == 2, "SpellCheckCommand must pass exactly two arguments")
  local text, all = ...
  assert(all == true, "SpellCheckCommand must identify whole-input checking")
  return "corrected"
end
)lua")));

				MainWindow frame;
				controller.setMainWindow(&frame);
				auto *world = new WorldChildWindow(QStringLiteral("Spell-check world"));
				world->setRuntime(&runtime);
				const QPointer worldGuard(world);
				const auto     unbindRuntime = qScopeGuard(
				    [&worldGuard]
				    {
					    if (worldGuard)
						    worldGuard->setRuntime(nullptr);
				    });
				frame.addMdiSubWindow(world, true);
				frame.resize(900, 600);
				frame.show();
				QCoreApplication::processEvents();
				QVERIFY(world->view());
				QVERIFY(world->view()->inputEditor());
				QPlainTextEdit *const input = world->view()->inputEditor();
				input->setPlainText(QStringLiteral("mispelt"));
				QTextCursor originalCursor = input->textCursor();
				originalCursor.setPosition(3);
				input->setTextCursor(originalCursor);
				const QSharedPointer<const LuaCallbackSnapshot> commandSnapshot =
				    runtime.luaCallbackSnapshotForBridgedCall();
				QVERIFY(commandSnapshot);
				QVERIFY(commandSnapshot->hasCommandUiSnapshot);
				QVERIFY(commandSnapshot->commandUiHasView);
				QVERIFY(commandSnapshot->commandUiHasFrameData);
				QCOMPARE(
				    commandSnapshot->commandUiValues.value(QStringLiteral("commandInputText")).toString(),
				    QStringLiteral("mispelt"));
				QCOMPARE(commandSnapshot->commandUiValues.value(QStringLiteral("inputSelectionStartColumn"))
				             .toInt(),
				         4);
				QCOMPARE(
				    commandSnapshot->commandUiValues.value(QStringLiteral("inputSelectionEndColumn")).toInt(),
				    0);
				hasFunction  = false;
				suspended    = false;
				resumeId     = 0;
				modalRequest = {};
				{
					engine.pushDispatchSnapshot(commandSnapshot);
					const auto popSnapshot = qScopeGuard([&engine] { engine.popDispatchSnapshot(); });
					QVERIFY(!engine.callFunctionNoArgs(QStringLiteral("check_command_binding"), &hasFunction,
					                                   false, -1, &suspended, &resumeId, &modalRequest));
				}
				QVERIFY(hasFunction);
				QVERIFY(suspended);
				QVERIFY(resumeId != 0);
				QVERIFY(modalRequest.guiCallable);
				QVERIFY(modalRequest.resultCallback);
				const QString commandResumeResult = modalRequest.guiCallable();
				resumed = engine.resumeSuspendedModalString(resumeId, commandResumeResult);
				QVERIFY(!resumed.suspended);
				QVERIFY(resumed.boolResultValid);
				QVERIFY(resumed.boolResult);
				for (LuaDeferredRuntimeMutationBatch &batch : resumed.deferredRuntimeMutationBatches)
				{
					for (std::function<void()> &mutation : batch.mutations)
					{
						if (mutation)
							mutation();
					}
				}
				QCOMPARE(input->toPlainText(), QStringLiteral("corrected"));
				QCOMPARE(input->textCursor().selectionStart(), 3);
				QCOMPARE(input->textCursor().selectionEnd(), 3);

				controller.releaseSpellCheckerForWorld(&runtime);
				QCOMPARE(AppControllerTestAccess::idleSpellCheckerStateCount(controller, &runtime), 0);
				QCOMPARE(AppControllerTestAccess::spellCheckerWorldCount(controller), 0);

				const quint64 generationBeforeUnusedWorld =
				    AppControllerTestAccess::spellCheckerGeneration(controller);
				{
					WorldRuntime unusedRuntime;
				}
				QCOMPARE(AppControllerTestAccess::spellCheckerGeneration(controller),
				         generationBeforeUnusedWorld);

				auto                firstWorkerWorld     = std::make_unique<WorldRuntime>();
				auto                secondWorkerWorld    = std::make_unique<WorldRuntime>();
				const WorldRuntime *firstWorkerIdentity  = firstWorkerWorld.get();
				const WorldRuntime *secondWorkerIdentity = secondWorkerWorld.get();
				QVERIFY(AppControllerTestAccess::executeWorkerSpellCheck(
				    *firstWorkerWorld, QStringLiteral("qmudworkerfirstzzzxxyy")));
				QVERIFY(AppControllerTestAccess::executeWorkerSpellCheck(
				    *secondWorkerWorld, QStringLiteral("qmudworkersecondzzzxxyy")));
				QVERIFY(AppControllerTestAccess::hasWorkerSpellCheckerState(controller, firstWorkerIdentity));
				QVERIFY(
				    AppControllerTestAccess::hasWorkerSpellCheckerState(controller, secondWorkerIdentity));
				QCOMPARE(AppControllerTestAccess::spellCheckerWorldCount(controller), 2);

				secondWorkerWorld.reset();
				QCOMPARE(
				    AppControllerTestAccess::idleSpellCheckerStateCount(controller, secondWorkerIdentity), 0);
				QVERIFY(AppControllerTestAccess::hasWorkerSpellCheckerState(controller, firstWorkerIdentity));
				QCOMPARE(AppControllerTestAccess::spellCheckerWorldCount(controller), 1);

				firstWorkerWorld.reset();
				QCOMPARE(AppControllerTestAccess::idleSpellCheckerStateCount(controller, firstWorkerIdentity),
				         0);
				QCOMPARE(AppControllerTestAccess::spellCheckerWorldCount(controller), 0);
			}
#endif
			/**
			 * @brief Verifies notepad Send To World targets its exact owner and terminates its partial prompt.
			 */
			static void ownedNotepadSendToWorldTerminatesPartialPrompt()
			{
				AppController controller;
				MainWindow    frame;
				WorldRuntime  decoyRuntime;
				WorldRuntime  runtime;
				for (WorldRuntime *candidate : {&decoyRuntime, &runtime})
				{
					candidate->setWorldAttribute(QStringLiteral("id"), QStringLiteral("shared-world-id"));
					candidate->setWorldAttribute(QStringLiteral("name"),
					                             QStringLiteral("Text command world"));
					candidate->setWorldAttribute(QStringLiteral("display_my_input"), QStringLiteral("1"));
					candidate->setWorldAttribute(QStringLiteral("echo_force_terminates_partial_prompts"),
					                             QStringLiteral("1"));
					WorldRuntimeTestAccess::setConnectPhase(*candidate, WorldRuntime::eConnectConnectedToMud);
				}
				const auto resetConnectPhase = qScopeGuard(
				    [&decoyRuntime, &runtime]
				    {
					    WorldRuntimeTestAccess::setConnectPhase(decoyRuntime,
					                                            WorldRuntime::eConnectNotConnected);
					    WorldRuntimeTestAccess::setConnectPhase(runtime, WorldRuntime::eConnectNotConnected);
				    });

				auto *decoyWorld = new WorldChildWindow(QStringLiteral("Text command world"));
				decoyWorld->setRuntime(&decoyRuntime);
				auto *world = new WorldChildWindow(QStringLiteral("Text command world"));
				world->setRuntime(&runtime);
				const QPointer decoyWorldGuard(decoyWorld);
				const QPointer worldGuard(world);
				const auto     unbindRuntimes = qScopeGuard(
				    [&decoyWorldGuard, &worldGuard]
				    {
					    if (decoyWorldGuard)
						    decoyWorldGuard->setRuntime(nullptr);
					    if (worldGuard)
						    worldGuard->setRuntime(nullptr);
				    });
				controller.setMainWindow(&frame);
				frame.addMdiSubWindow(decoyWorld, true);
				frame.addMdiSubWindow(world, true);
				frame.resize(900, 600);
				frame.show();
				QCoreApplication::processEvents();
				const QVector<WorldWindowDescriptor> descriptors = frame.worldRuntimeDescriptors();
				QCOMPARE(descriptors.size(), qsizetype{2});
				QCOMPARE(descriptors.at(0).runtime, &decoyRuntime);
				QCOMPARE(descriptors.at(1).runtime, &runtime);

				WorldView *const view = world->view();
				QVERIFY(view);
				view->applyRuntimeSettings();
				runtime.receiveRawData(QByteArrayLiteral("prompt> "));
				QTRY_COMPARE_WITH_TIMEOUT(view->outputLines().constLast(), QStringLiteral("prompt> "), 5000);

				QVERIFY(frame.switchToNotepad());
				QCoreApplication::processEvents();
				TextChildWindow *const notepad = frame.activeTextChildWindow();
				QVERIFY(notepad);
				QCOMPARE(frame.resolveRuntimeForTextWindow(notepad), &runtime);
				QVERIFY(notepad->editor());
				notepad->editor()->setPlainText(QStringLiteral("look"));

				runtime.setCurrentActionSource(WorldRuntime::eWorldAction);
				controller.onCommandTriggered(QStringLiteral("SendToWorld"));
				QCOMPARE(runtime.currentActionSource(),
				         static_cast<unsigned short>(WorldRuntime::eWorldAction));

				QCOMPARE(runtime.lines().size(), qsizetype{2});
				QCOMPARE(runtime.lines().at(0).text, QStringLiteral("prompt> "));
				QVERIFY(runtime.lines().at(0).hardReturn);
				QCOMPARE(runtime.lines().at(1).text, QStringLiteral("look"));
				QVERIFY(runtime.lines().at(1).hardReturn);
				QCOMPARE(view->outputLines(),
				         QStringList({QStringLiteral("prompt> "), QStringLiteral("look")}));
				QVERIFY(decoyRuntime.lines().isEmpty());

				notepad->setProperty("worldRuntimeToken", QVariant::fromValue<qulonglong>(1));
				QCOMPARE(frame.resolveRuntimeForTextWindow(notepad), nullptr);
			}
			/**
			 * @brief Verifies only explicitly owned text windows expose world-targeting actions.
			 */
			static void textWindowActionsRequireRecordedLiveOwner()
			{
				AppController controller;
				MainWindow    frame;
				WorldRuntime  runtime;
				runtime.setWorldAttribute(QStringLiteral("id"), QStringLiteral("owned-text-world-id"));
				runtime.setWorldAttribute(QStringLiteral("name"), QStringLiteral("Owned text world"));
				runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("1"));
				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
#ifdef QMUD_ENABLE_LUA_SCRIPTING
				QVERIFY(runtime.luaScriptingAvailable());
#else
				QVERIFY(!runtime.luaScriptingAvailable());
#endif

				auto *world = new WorldChildWindow(QStringLiteral("Owned text world"));
				world->setRuntime(&runtime);
				const QPointer worldGuard(world);
				const auto     unbindRuntime = qScopeGuard(
				    [&worldGuard]
				    {
					    if (worldGuard)
						    worldGuard->setRuntime(nullptr);
				    });
				controller.setMainWindow(&frame);
				frame.addMdiSubWindow(world, true);
				frame.resize(900, 600);
				frame.show();
				QCoreApplication::processEvents();

				const QString scriptPath = QFINDTESTDATA("../../CMakeLists.txt");
				QVERIFY(!scriptPath.isEmpty());
				runtime.setWorldAttribute(QStringLiteral("script_filename"), scriptPath);
				runtime.setWorldAttribute(QStringLiteral("edit_script_with_notepad"), QStringLiteral("1"));
				controller.onCommandTriggered(QStringLiteral("EditScriptFile"));
				QVERIFY(frame.activateNotepad(QFileInfo(scriptPath).fileName(), &runtime));
				QCoreApplication::processEvents();
				TextChildWindow *const associatedDocument = frame.activeTextChildWindow();
				QVERIFY(associatedDocument);
				QCOMPARE(frame.resolveRuntimeForTextWindow(associatedDocument), &runtime);

				auto *lookalike = new TextChildWindow(QStringLiteral("Notepad: Owned text world"), QString());
				frame.addMdiSubWindow(lookalike, true);
				QCoreApplication::processEvents();
				QCOMPARE(frame.activeTextChildWindow(), lookalike);
				QCOMPARE(frame.resolveRuntimeForTextWindow(lookalike), nullptr);

				QAction *const sendToCommandWindow =
				    frame.actionForCommand(QStringLiteral("SendToCommandWindow"));
				QAction *const sendToScript = frame.actionForCommand(QStringLiteral("SendToScript"));
				QAction *const sendToWorld  = frame.actionForCommand(QStringLiteral("SendToWorld"));
				QVERIFY(sendToCommandWindow);
				QVERIFY(sendToScript);
				QVERIFY(sendToWorld);
				frame.updateEditActions();
				QVERIFY(!sendToCommandWindow->isEnabled());
				QVERIFY(!sendToScript->isEnabled());
				QVERIFY(!sendToWorld->isEnabled());

				MainWindow::associateTextWindowWithRuntime(lookalike, &runtime);
				QCOMPARE(frame.resolveRuntimeForTextWindow(lookalike), &runtime);
				frame.updateEditActions();
				QVERIFY(sendToCommandWindow->isEnabled());
				QCOMPARE(sendToScript->isEnabled(), runtime.luaScriptingAvailable());
				QVERIFY(sendToWorld->isEnabled());

				runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("0"));
				QVERIFY(!runtime.luaScriptingAvailable());
				QVERIFY(sendToCommandWindow->isEnabled());
				QVERIFY(!sendToScript->isEnabled());
				QVERIFY(sendToWorld->isEnabled());

				runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("1"));
				QVERIFY(sendToCommandWindow->isEnabled());
				QCOMPARE(sendToScript->isEnabled(), runtime.luaScriptingAvailable());
				QVERIFY(sendToWorld->isEnabled());

				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("VBScript"));
				QVERIFY(!runtime.luaScriptingAvailable());
				QVERIFY(!sendToScript->isEnabled());
				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
				QCOMPARE(sendToScript->isEnabled(), runtime.luaScriptingAvailable());

				MainWindow::associateTextWindowWithRuntime(lookalike, nullptr);
				QCOMPARE(frame.resolveRuntimeForTextWindow(lookalike), nullptr);
				frame.updateEditActions();
				QVERIFY(!sendToCommandWindow->isEnabled());
				QVERIFY(!sendToScript->isEnabled());
				QVERIFY(!sendToWorld->isEnabled());
			}
			/**
			 * @brief Verifies packet debugging can be toggled from its owned text window and recreated after close.
			 */
			static void packetDebugActionUsesOwnedTextWindow()
			{
				AppController controller;
				MainWindow    frame;
				WorldRuntime  runtime;
				runtime.setWorldAttribute(QStringLiteral("id"), QStringLiteral("packet-debug-world-id"));
				runtime.setWorldAttribute(QStringLiteral("name"), QStringLiteral("Packet debug world"));

				auto *world = new WorldChildWindow(QStringLiteral("Packet debug world"));
				world->setRuntime(&runtime);
				const QPointer worldGuard(world);
				const auto     unbindRuntime = qScopeGuard(
				    [&worldGuard]
				    {
					    if (worldGuard)
						    worldGuard->setRuntime(nullptr);
				    });
				controller.setMainWindow(&frame);
				frame.addMdiSubWindow(world, true);
				frame.resize(900, 600);
				frame.show();
				QCoreApplication::processEvents();

				QAction *const debugPackets = frame.actionForCommand(QStringLiteral("DebugPackets"));
				QVERIFY(debugPackets);
				QVERIFY(debugPackets->isEnabled());
				QVERIFY(!debugPackets->isChecked());

				controller.onCommandTriggered(QStringLiteral("DebugPackets"));
				QVERIFY(runtime.debugIncomingPackets());
				QVERIFY(debugPackets->isChecked());
				runtime.receiveRawData(QByteArrayLiteral("packet-one\r\n"));

				const QString title = QStringLiteral("Packet debug - Packet debug world");
				QVERIFY(frame.activateNotepad(title, &runtime));
				QCoreApplication::processEvents();
				TextChildWindow *const firstDebugWindow = frame.activeTextChildWindow();
				QVERIFY(firstDebugWindow);
				QCOMPARE(frame.resolveRuntimeForTextWindow(firstDebugWindow), &runtime);
				QVERIFY(debugPackets->isEnabled());
				QVERIFY(debugPackets->isChecked());

				controller.onCommandTriggered(QStringLiteral("DebugPackets"));
				QVERIFY(!runtime.debugIncomingPackets());
				QVERIFY(!debugPackets->isChecked());

				QPointer<TextChildWindow> closedDebugWindow(firstDebugWindow);
				firstDebugWindow->setQuerySaveOnClose(false);
				firstDebugWindow->close();
				QTRY_VERIFY(closedDebugWindow.isNull());
				QTRY_COMPARE(frame.activeWorldChildWindow(), world);
				QVERIFY(debugPackets->isEnabled());
				QVERIFY(!debugPackets->isChecked());
				controller.onCommandTriggered(QStringLiteral("DebugPackets"));
				QVERIFY(runtime.debugIncomingPackets());
				runtime.receiveRawData(QByteArrayLiteral("packet-two\r\n"));
				QVERIFY(frame.activateNotepad(title, &runtime));
				QCoreApplication::processEvents();
				TextChildWindow *const secondDebugWindow = frame.activeTextChildWindow();
				QVERIFY(secondDebugWindow);
				QCOMPARE(frame.resolveRuntimeForTextWindow(secondDebugWindow), &runtime);
				controller.onCommandTriggered(QStringLiteral("DebugPackets"));
				QVERIFY(!runtime.debugIncomingPackets());
				secondDebugWindow->setQuerySaveOnClose(false);
			}
			/**
			 * @brief Verifies clicked command hyperlinks are treated as typed user commands.
			 */
			static void clickedCommandLinkTerminatesPartialPrompt()
			{
				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("display_my_input"), QStringLiteral("1"));
				runtime.setWorldAttribute(QStringLiteral("echo_hyperlink_in_output_window"),
				                          QStringLiteral("1"));
				runtime.setWorldAttribute(QStringLiteral("echo_force_terminates_partial_prompts"),
				                          QStringLiteral("1"));
				WorldRuntimeTestAccess::setConnectPhase(runtime, WorldRuntime::eConnectConnectedToMud);
				const auto resetConnectPhase = qScopeGuard(
				    [&runtime]
				    {
					    WorldRuntimeTestAccess::setConnectPhase(runtime, WorldRuntime::eConnectNotConnected);
				    });

				WorldChildWindow world;
				world.setRuntime(&runtime);
				WorldView *const view = world.view();
				QVERIFY(view);
				auto *const processor = world.findChild<WorldCommandProcessor *>();
				QVERIFY(processor);

				runtime.receiveRawData(QByteArrayLiteral("prompt> "));
				QTRY_COMPARE_WITH_TIMEOUT(view->outputLines().constLast(), QStringLiteral("prompt> "), 5000);
				runtime.setCurrentActionSource(WorldRuntime::eWorldAction);
				processor->onHyperlinkActivated(QStringLiteral("look"));

				QCOMPARE(runtime.currentActionSource(),
				         static_cast<unsigned short>(WorldRuntime::eWorldAction));
				QCOMPARE(runtime.lines().size(), qsizetype{2});
				QVERIFY(runtime.lines().at(0).hardReturn);
				QCOMPARE(runtime.lines().at(1).text, QStringLiteral("look"));
				QCOMPARE(view->outputLines(),
				         QStringList({QStringLiteral("prompt> "), QStringLiteral("look")}));
			}
			/**
			 * @brief Verifies delayed commands retain the source that queued them.
			 */
			static void queuedCommandRetainsInteractiveSource()
			{
				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("display_my_input"), QStringLiteral("1"));
				runtime.setWorldAttribute(QStringLiteral("echo_force_terminates_partial_prompts"),
				                          QStringLiteral("1"));
				runtime.setWorldAttribute(QStringLiteral("speed_walk_delay"), QStringLiteral("1000"));
				WorldRuntimeTestAccess::setConnectPhase(runtime, WorldRuntime::eConnectConnectedToMud);
				const auto resetConnectPhase = qScopeGuard(
				    [&runtime]
				    {
					    WorldRuntimeTestAccess::setConnectPhase(runtime, WorldRuntime::eConnectNotConnected);
				    });

				WorldChildWindow world;
				world.setRuntime(&runtime);
				WorldView *const view = world.view();
				QVERIFY(view);
				auto *const processor = world.findChild<WorldCommandProcessor *>();
				QVERIFY(processor);

				runtime.receiveRawData(QByteArrayLiteral("prompt> "));
				QTRY_COMPARE_WITH_TIMEOUT(view->outputLines().constLast(), QStringLiteral("prompt> "), 5000);
				runtime.setCurrentActionSource(WorldRuntime::eUserTyping);
				QCOMPARE(runtime.sendCommand(QStringLiteral("queued-look"), true, true, false, false, false),
				         eOK);
				QCOMPARE(processor->queuedCommands().size(), qsizetype{1});
				QVERIFY(runtime.lines().isEmpty());

				runtime.setCurrentActionSource(WorldRuntime::eTriggerFired);
				processor->applyNumericWorldOption(WorldNumericOptionBinding::CommandSpeedWalkDelay, 0);

				QCOMPARE(runtime.currentActionSource(),
				         static_cast<unsigned short>(WorldRuntime::eTriggerFired));
				QVERIFY(processor->queuedCommands().isEmpty());
				QCOMPARE(runtime.lines().size(), qsizetype{2});
				QVERIFY(runtime.lines().at(0).hardReturn);
				QCOMPARE(runtime.lines().at(1).text, QStringLiteral("queued-look"));
				QCOMPARE(view->outputLines(),
				         QStringList({QStringLiteral("prompt> "), QStringLiteral("queued-look")}));
			}
			/**
			 * @brief Verifies the generated-name send button is treated as typed user input.
			 */
			static void generatedNameSendTerminatesPartialPrompt()
			{
				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("display_my_input"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("echo_force_terminates_partial_prompts"),
				                          QStringLiteral("1"));
				WorldRuntimeTestAccess::setConnectPhase(runtime, WorldRuntime::eConnectConnectedToMud);
				const auto resetConnectPhase = qScopeGuard(
				    [&runtime]
				    {
					    WorldRuntimeTestAccess::setConnectPhase(runtime, WorldRuntime::eConnectNotConnected);
				    });

				WorldChildWindow world;
				world.setRuntime(&runtime);
				WorldView *const view = world.view();
				QVERIFY(view);

				runtime.receiveRawData(QByteArrayLiteral("prompt> "));
				QTRY_COMPARE_WITH_TIMEOUT(view->outputLines().constLast(), QStringLiteral("prompt> "), 5000);

				const QString namesFile = QFINDTESTDATA("../../skeleton/names/names.txt");
				QVERIFY(!namesFile.isEmpty());
				qmudReadNames(namesFile, true);
				const GeneratedNameDialog dialog(&runtime);
				QLineEdit                *nameEdit = nullptr;
				for (QLineEdit *candidate : dialog.findChildren<QLineEdit *>())
				{
					if (!candidate->isReadOnly())
					{
						nameEdit = candidate;
						break;
					}
				}
				QVERIFY(nameEdit);
				nameEdit->setText(QStringLiteral("generated-name"));
				QPushButton *sendButton = nullptr;
				for (QPushButton *candidate : dialog.findChildren<QPushButton *>())
				{
					if (candidate->text() == QStringLiteral("Send To World"))
					{
						sendButton = candidate;
						break;
					}
				}
				QVERIFY(sendButton);
				runtime.setCurrentActionSource(WorldRuntime::eWorldAction);
				sendButton->click();

				QCOMPARE(runtime.currentActionSource(),
				         static_cast<unsigned short>(WorldRuntime::eWorldAction));
				QCOMPARE(runtime.lines().size(), qsizetype{2});
				QVERIFY(runtime.lines().at(0).hardReturn);
				QCOMPARE(runtime.lines().at(1).text, QStringLiteral("generated-name"));
				QCOMPARE(view->outputLines(),
				         QStringList({QStringLiteral("prompt> "), QStringLiteral("generated-name")}));
			}

		public:
			/** @brief Validates the source-visible Qt test entry-point signatures. */
			tst_AppControllerTextCommands()
			{
				constexpr std::array testFunctions = {
#ifdef QMUD_ENABLE_LUA_SCRIPTING
				    &spellCheckerStateProvidesRequiredUtilities,
#endif
				    &ownedNotepadSendToWorldTerminatesPartialPrompt,
				    &textWindowActionsRequireRecordedLiveOwner,
				    &packetDebugActionUsesOwnedTextWindow,
				    &clickedCommandLinkTerminatesPartialPrompt,
				    &queuedCommandRetainsInteractiveSource,
				    &generatedNameSendTerminatesPartialPrompt,
				};
#ifdef QMUD_ENABLE_LUA_SCRIPTING
				static_assert(testFunctions.size() == 7);
#else
				static_assert(testFunctions.size() == 6);
#endif
			}
	};

} // namespace

QTEST_MAIN(tst_AppControllerTextCommands)

#if __has_include("tst_AppControllerTextCommands.moc")
#include "tst_AppControllerTextCommands.moc"
#endif
