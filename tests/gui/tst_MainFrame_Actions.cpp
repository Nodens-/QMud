/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_MainFrame_Actions.cpp
 * Role: QTest coverage for MainFrame Actions behavior.
 */

#include "MainFrameActionUtils.h"

#include <QtTest/QTest>

namespace
{

	/**
	 * @brief QTest fixture covering MainFrame Actions scenarios.
	 */
	class tst_MainFrame_Actions : public QObject
	{
			Q_OBJECT

			// NOLINTBEGIN(readability-convert-member-functions-to-static)
		private slots:
			void worldSlotCommandName_data()
			{
				QTest::addColumn<int>("slot");
				QTest::addColumn<QString>("expected");

				QTest::newRow("first-slot") << 1 << QStringLiteral("World1");
				QTest::newRow("ninth-slot") << 9 << QStringLiteral("World9");
				QTest::newRow("tenth-slot") << 10 << QStringLiteral("World10");
				QTest::newRow("overflow-slot") << 42 << QStringLiteral("World42");
			}

			void worldSlotCommandName()
			{
				QFETCH(int, slot);
				QFETCH(QString, expected);
				QCOMPARE(QMudMainFrameActionUtils::worldCommandNameForSlot(slot), expected);
			}

			void worldSlotTooltip_data()
			{
				QTest::addColumn<int>("slot");
				QTest::addColumn<QString>("expected");

				QTest::newRow("first-slot")
				    << 1
				    << QMudMainFrameActionUtils::toolbarTooltipWithShortcut(
				           QStringLiteral("Activates world #1"), QStringLiteral("Ctrl+1"));
				QTest::newRow("ninth-slot")
				    << 9
				    << QMudMainFrameActionUtils::toolbarTooltipWithShortcut(
				           QStringLiteral("Activates world #9"), QStringLiteral("Ctrl+9"));
				QTest::newRow("tenth-slot")
				    << 10
				    << QMudMainFrameActionUtils::toolbarTooltipWithShortcut(
				           QStringLiteral("Activates world #10"), QStringLiteral("Ctrl+0"));
				QTest::newRow("overflow-slot") << 12 << QStringLiteral("Activates world #12");
			}

			void worldSlotTooltip()
			{
				QFETCH(int, slot);
				QFETCH(QString, expected);
				QCOMPARE(QMudMainFrameActionUtils::worldButtonTooltipForSlot(slot), expected);
			}

			void menuRoleForCommand_data()
			{
				QTest::addColumn<QString>("commandName");
				QTest::addColumn<QAction::MenuRole>("expected");

				QTest::newRow("application-quit") << QStringLiteral("ExitClient") << QAction::QuitRole;
#ifdef Q_OS_MACOS
				QTest::newRow("world-quit") << QStringLiteral("QuitFromWorld") << QAction::NoRole;
				QTest::newRow("ordinary-action") << QStringLiteral("Open") << QAction::NoRole;
				QTest::newRow("preferences-action") << QStringLiteral("Preferences") << QAction::NoRole;
#else
				QTest::newRow("world-quit") << QStringLiteral("QuitFromWorld") << QAction::NoRole;
				QTest::newRow("ordinary-action") << QStringLiteral("Open") << QAction::TextHeuristicRole;
#endif
			}

			void menuRoleForCommand()
			{
				QFETCH(QString, commandName);
				QFETCH(QAction::MenuRole, expected);
				QCOMPARE(QMudMainFrameActionUtils::menuRoleForCommand(commandName), expected);
			}

			void shortcutForCommand_data()
			{
				QTest::addColumn<QString>("commandName");
				QTest::addColumn<QString>("configuredShortcutText");
				QTest::addColumn<bool>("expectedStandardQuit");
				QTest::addColumn<QString>("expectedShortcutText");

				QTest::newRow("application-quit-default")
				    << QStringLiteral("ExitClient") << QString() << true << QString();
				QTest::newRow("application-quit-explicit")
				    << QStringLiteral("ExitClient") << QStringLiteral("Ctrl+Alt+Q") << false
				    << QStringLiteral("Ctrl+Alt+Q");
				QTest::newRow("world-quit")
				    << QStringLiteral("QuitFromWorld") << QStringLiteral("Ctrl+Shift+Q") << false
				    << QStringLiteral("Ctrl+Shift+Q");
			}

			void shortcutForCommand()
			{
				QFETCH(QString, commandName);
				QFETCH(QString, configuredShortcutText);
				QFETCH(bool, expectedStandardQuit);
				QFETCH(QString, expectedShortcutText);

				const QKeySequence configuredShortcut =
				    QKeySequence::fromString(configuredShortcutText, QKeySequence::PortableText);
				const QKeySequence actual =
				    QMudMainFrameActionUtils::shortcutForCommand(commandName, configuredShortcut);
				if (expectedStandardQuit)
					QCOMPARE(actual, QKeySequence(QKeySequence::Quit));
				else
					QCOMPARE(actual.toString(QKeySequence::PortableText), expectedShortcutText);
			}

			void toolbarTooltipWithShortcut()
			{
				const QString nativeShortcut =
				    QKeySequence::fromString(QStringLiteral("Shift+Ctrl+8"), QKeySequence::PortableText)
				        .toString(QKeySequence::NativeText);
				QCOMPARE(QMudMainFrameActionUtils::toolbarTooltipWithShortcut(QStringLiteral("Triggers"),
				                                                              QStringLiteral("Shift+Ctrl+8")),
				         QStringLiteral("Triggers (%1)").arg(nativeShortcut));
				QCOMPARE(QMudMainFrameActionUtils::toolbarTooltipWithShortcut(QStringLiteral("Triggers"),
				                                                              QString()),
				         QStringLiteral("Triggers"));
			}

			void adjacentTabShortcutStep_data()
			{
				QTest::addColumn<int>("key");
				QTest::addColumn<Qt::KeyboardModifiers>("modifiers");
				QTest::addColumn<int>("expected");

				QTest::newRow("ctrl-shift-left")
				    << static_cast<int>(Qt::Key_Left) << (Qt::ControlModifier | Qt::ShiftModifier) << -1;
				QTest::newRow("ctrl-shift-right")
				    << static_cast<int>(Qt::Key_Right) << (Qt::ControlModifier | Qt::ShiftModifier) << 1;
				QTest::newRow("shift-left-old-binding")
				    << static_cast<int>(Qt::Key_Left) << Qt::KeyboardModifiers(Qt::ShiftModifier) << 0;
				QTest::newRow("shift-right-old-binding")
				    << static_cast<int>(Qt::Key_Right) << Qt::KeyboardModifiers(Qt::ShiftModifier) << 0;
				QTest::newRow("ctrl-left")
				    << static_cast<int>(Qt::Key_Left) << Qt::KeyboardModifiers(Qt::ControlModifier) << 0;
				QTest::newRow("ctrl-shift-alt-left")
				    << static_cast<int>(Qt::Key_Left)
				    << (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier) << 0;
				QTest::newRow("ctrl-shift-up")
				    << static_cast<int>(Qt::Key_Up) << (Qt::ControlModifier | Qt::ShiftModifier) << 0;
			}

			void adjacentTabShortcutStep()
			{
				QFETCH(int, key);
				QFETCH(Qt::KeyboardModifiers, modifiers);
				QFETCH(int, expected);

				QCOMPARE(QMudMainFrameActionUtils::adjacentTabShortcutStep(key, modifiers), expected);
			}

			void shouldAttemptIncomingLineTaskbarFlash_data()
			{
				QTest::addColumn<bool>("worldFlashEnabled");
				QTest::addColumn<bool>("appFocused");
				QTest::addColumn<bool>("expected");

				QTest::newRow("enabled-unfocused") << true << false << true;
				QTest::newRow("enabled-focused") << true << true << false;
				QTest::newRow("disabled-unfocused") << false << false << false;
				QTest::newRow("disabled-focused") << false << true << false;
			}

			void shouldAttemptIncomingLineTaskbarFlash()
			{
				QFETCH(bool, worldFlashEnabled);
				QFETCH(bool, appFocused);
				QFETCH(bool, expected);
				QCOMPARE(QMudMainFrameActionUtils::shouldAttemptIncomingLineTaskbarFlash(worldFlashEnabled,
				                                                                         appFocused),
				         expected);
			}

			void shouldRequestBackgroundTaskbarFlash_data()
			{
				QTest::addColumn<bool>("appFocused");
				QTest::addColumn<bool>("alreadyRequested");
				QTest::addColumn<bool>("expected");

				QTest::newRow("unfocused-not-requested") << false << false << true;
				QTest::newRow("unfocused-already-requested") << false << true << false;
				QTest::newRow("focused-not-requested") << true << false << false;
				QTest::newRow("focused-already-requested") << true << true << false;
			}

			void shouldRequestBackgroundTaskbarFlash()
			{
				QFETCH(bool, appFocused);
				QFETCH(bool, alreadyRequested);
				QFETCH(bool, expected);
				QCOMPARE(QMudMainFrameActionUtils::shouldRequestBackgroundTaskbarFlash(appFocused,
				                                                                       alreadyRequested),
				         expected);
			}

			void resolveIncomingLineFocusForFlash_data()
			{
				QTest::addColumn<bool>("qtAppFocused");
				QTest::addColumn<bool>("windowFocused");
				QTest::addColumn<bool>("expected");

				QTest::newRow("both-focused") << true << true << true;
				QTest::newRow("qt-focused-window-unfocused") << true << false << true;
				QTest::newRow("qt-unfocused-window-focused") << false << true << true;
				QTest::newRow("both-unfocused") << false << false << false;
			}

			void resolveIncomingLineFocusForFlash()
			{
				QFETCH(bool, qtAppFocused);
				QFETCH(bool, windowFocused);
				QFETCH(bool, expected);
				QCOMPARE(
				    QMudMainFrameActionUtils::resolveIncomingLineFocusForFlash(qtAppFocused, windowFocused),
				    expected);
			}

			void resolveIncomingLineFocusForActivitySound_data()
			{
				QTest::addColumn<bool>("qtAppFocused");
				QTest::addColumn<bool>("windowFocused");
				QTest::addColumn<bool>("expected");

				QTest::newRow("both-focused") << true << true << true;
				QTest::newRow("qt-focused-window-unfocused") << true << false << false;
				QTest::newRow("qt-unfocused-window-focused") << false << true << false;
				QTest::newRow("both-unfocused") << false << false << false;
			}

			void resolveIncomingLineFocusForActivitySound()
			{
				QFETCH(bool, qtAppFocused);
				QFETCH(bool, windowFocused);
				QFETCH(bool, expected);
				QCOMPARE(QMudMainFrameActionUtils::resolveIncomingLineFocusForActivitySound(qtAppFocused,
				                                                                            windowFocused),
				         expected);
			}

			void shouldResetBackgroundFlashLatch_data()
			{
				QTest::addColumn<bool>("previousFocused");
				QTest::addColumn<bool>("currentFocused");
				QTest::addColumn<bool>("expected");

				QTest::newRow("focused-to-unfocused") << true << false << true;
				QTest::newRow("unfocused-to-focused") << false << true << true;
				QTest::newRow("focused-to-focused") << true << true << false;
				QTest::newRow("unfocused-to-unfocused") << false << false << false;
			}

			void shouldResetBackgroundFlashLatch()
			{
				QFETCH(bool, previousFocused);
				QFETCH(bool, currentFocused);
				QFETCH(bool, expected);
				QCOMPARE(QMudMainFrameActionUtils::shouldResetBackgroundFlashLatch(previousFocused,
				                                                                   currentFocused),
				         expected);
			}
			// NOLINTEND(readability-convert-member-functions-to-static)
	};

} // namespace

QTEST_MAIN(tst_MainFrame_Actions)

#if __has_include("tst_MainFrame_Actions.moc")
#include "tst_MainFrame_Actions.moc"
#endif
