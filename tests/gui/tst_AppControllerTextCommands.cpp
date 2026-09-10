/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_AppControllerTextCommands.cpp
 * Role: Regression coverage for application text-window commands targeting their associated world.
 */

#include <QFileInfo>

#include "AppController.h"
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
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QtTest/QTest>

#include <array>

namespace
{

	/**
	 * @brief Verifies text-window commands use their recorded world ownership and preserve user-action policy.
	 */
	class tst_AppControllerTextCommands final : public QObject
	{
			Q_OBJECT

		private slots:
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
				    &ownedNotepadSendToWorldTerminatesPartialPrompt,
				    &textWindowActionsRequireRecordedLiveOwner,
				    &packetDebugActionUsesOwnedTextWindow,
				    &clickedCommandLinkTerminatesPartialPrompt,
				    &queuedCommandRetainsInteractiveSource,
				    &generatedNameSendTerminatesPartialPrompt,
				};
				static_assert(testFunctions.size() == 6);
			}
	};

} // namespace

QTEST_MAIN(tst_AppControllerTextCommands)

#if __has_include("tst_AppControllerTextCommands.moc")
#include "tst_AppControllerTextCommands.moc"
#endif
