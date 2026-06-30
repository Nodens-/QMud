/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: ShortcutPreferenceUtils.cpp
 * Role: Shared shortcut preference definitions and matching logic used by menus, views, and preferences UI.
 */

#include "ShortcutPreferenceUtils.h"

#include "AppController.h"

#include <QKeyEvent>
#include <QSet>
// ReSharper disable once CppUnusedIncludeDirective
#include <QStringList>

#include <algorithm>

namespace
{
	using ShortcutDefinition = QMudShortcutPreferenceUtils::ShortcutDefinition;

	QKeySequence shortcutFromPortable(const QString &portableText)
	{
		return QKeySequence::fromString(portableText, QKeySequence::PortableText);
	}

	QList<QKeySequence> shortcutsFromPortableList(const QString &portableText)
	{
		QList<QKeySequence> shortcuts;
		for (const QString &part : portableText.split(QLatin1Char(';'), Qt::SkipEmptyParts))
		{
			const QKeySequence sequence = shortcutFromPortable(part.trimmed());
			if (!sequence.isEmpty())
				shortcuts.push_back(sequence);
		}
		return shortcuts;
	}

	ShortcutDefinition makeDefinition(const QString &id, const QString &category, const QString &label,
	                                  const QString &defaultShortcuts)
	{
		return {
		    .id            = id,
		    .preferenceKey = QStringLiteral("Shortcut.%1").arg(id),
		    .category      = category,
		    .label         = label,
		    .defaults      = shortcutsFromPortableList(defaultShortcuts),
		};
	}

	QList<ShortcutDefinition> buildShortcutDefinitions()
	{
		QList<ShortcutDefinition> defs;
		defs.reserve(110);
		auto add =
		    [&defs](const QString &id, const QString &category, const QString &label, const QString &defaults)
		{ defs.push_back(makeDefinition(id, category, label, defaults)); };

		add(QStringLiteral("New"), QStringLiteral("File"), QStringLiteral("New World"),
		    QStringLiteral("Ctrl+N"));
		add(QStringLiteral("Open"), QStringLiteral("File"), QStringLiteral("Open World"),
		    QStringLiteral("Ctrl+O"));
		add(QStringLiteral("OpenWorldsInStartupList"), QStringLiteral("File"),
		    QStringLiteral("Open Worlds In Startup List"), QStringLiteral("Ctrl+Alt+O"));
		add(QStringLiteral("Import"), QStringLiteral("File"), QStringLiteral("Import"),
		    QStringLiteral("Ctrl+Alt+I"));
		add(QStringLiteral("Plugins"), QStringLiteral("File"), QStringLiteral("Plugins"),
		    QStringLiteral("Shift+Ctrl+P"));
		add(QStringLiteral("PluginWizard"), QStringLiteral("File"), QStringLiteral("Plugin Wizard"),
		    QStringLiteral("Shift+Ctrl+Alt+P"));
		add(QStringLiteral("Save"), QStringLiteral("File"), QStringLiteral("Save World Details"),
		    QStringLiteral("Ctrl+S"));
		add(QStringLiteral("Print"), QStringLiteral("File"), QStringLiteral("Print"),
		    QStringLiteral("Ctrl+P"));
		add(QStringLiteral("GlobalPreferences"), QStringLiteral("File"), QStringLiteral("Global Preferences"),
		    QStringLiteral("Ctrl+Alt+G"));
		add(QStringLiteral("LogSession"), QStringLiteral("File"), QStringLiteral("Log Session"),
		    QStringLiteral("Shift+Ctrl+J"));
		add(QStringLiteral("ReloadDefaults"), QStringLiteral("File"), QStringLiteral("Reload Defaults"),
		    QStringLiteral("Ctrl+Alt+R"));
		add(QStringLiteral("Preferences"), QStringLiteral("File"), QStringLiteral("World Properties"),
		    QStringLiteral("Alt+Return;Alt+Enter;Ctrl+G"));
		add(QStringLiteral("ExitClient"), QStringLiteral("File"), QStringLiteral("Exit Client"),
		    QKeySequence(QKeySequence::Quit).toString(QKeySequence::PortableText));

		add(QStringLiteral("Undo"), QStringLiteral("Edit"), QStringLiteral("Undo"), QStringLiteral("Ctrl+Z"));
		add(QStringLiteral("Cut"), QStringLiteral("Edit"), QStringLiteral("Cut"), QStringLiteral("Ctrl+X"));
		add(QStringLiteral("Copy"), QStringLiteral("Edit"), QStringLiteral("Copy"), QStringLiteral("Ctrl+C"));
		add(QStringLiteral("CopyAsHTML"), QStringLiteral("Edit"), QStringLiteral("Copy as HTML"),
		    QStringLiteral("Ctrl+Alt+C"));
		add(QStringLiteral("Paste"), QStringLiteral("Edit"), QStringLiteral("Paste"),
		    QStringLiteral("Ctrl+V"));
		add(QStringLiteral("PasteToWorld"), QStringLiteral("Edit"), QStringLiteral("Paste To World"),
		    QStringLiteral("Shift+Ctrl+V"));
		add(QStringLiteral("RecallLastWord"), QStringLiteral("Edit"), QStringLiteral("Recall Last Word"),
		    QStringLiteral("Ctrl+Backspace"));
		add(QStringLiteral("SelectAll"), QStringLiteral("Edit"), QStringLiteral("Select All"),
		    QStringLiteral("Ctrl+A"));
		add(QStringLiteral("SpellCheck"), QStringLiteral("Edit"), QStringLiteral("Spell Check"),
		    QStringLiteral("Ctrl+J"));
		add(QStringLiteral("GenerateCharacterName"), QStringLiteral("Edit"),
		    QStringLiteral("Generate Character Name"), QStringLiteral("Ctrl+Alt+N"));
		add(QStringLiteral("Notepad"), QStringLiteral("Edit"), QStringLiteral("Notepad"),
		    QStringLiteral("Ctrl+Alt+W"));
		add(QStringLiteral("FlipToNotepad"), QStringLiteral("Edit"), QStringLiteral("Flip To Notepad"),
		    QStringLiteral("Ctrl+Alt+Space"));
		add(QStringLiteral("EditColourPicker"), QStringLiteral("Edit"), QStringLiteral("Colour Picker"),
		    QStringLiteral("Ctrl+Alt+P"));
		add(QStringLiteral("DebugPackets"), QStringLiteral("Edit"), QStringLiteral("Debug Packets"),
		    QStringLiteral("Ctrl+Alt+F11"));
		add(QStringLiteral("GoToMatchingBrace"), QStringLiteral("Edit"),
		    QStringLiteral("Go To Matching Brace"), QStringLiteral("Ctrl+E"));
		add(QStringLiteral("SelectToMatchingBrace"), QStringLiteral("Edit"),
		    QStringLiteral("Select To Matching Brace"), QStringLiteral("Shift+Ctrl+E"));
		add(QStringLiteral("ConvertClipboardForumCodes"), QStringLiteral("Edit"),
		    QStringLiteral("Convert Clipboard Forum Codes"), QStringLiteral("Shift+Ctrl+Alt+Q"));
		add(QStringLiteral("AsciiArt"), QStringLiteral("Edit"), QStringLiteral("ASCII Art"),
		    QStringLiteral("Shift+Ctrl+A"));

		add(QStringLiteral("FullScreenMode"), QStringLiteral("View"), QStringLiteral("Full Screen Mode"),
		    QStringLiteral("Ctrl+Alt+F"));

		add(QStringLiteral("QuickConnect"), QStringLiteral("Connection"), QStringLiteral("Quick Connect"),
		    QStringLiteral("Ctrl+Alt+Shift+K"));
		add(QStringLiteral("Connect"), QStringLiteral("Connection"), QStringLiteral("Connect"),
		    QStringLiteral("Ctrl+K"));
		add(QStringLiteral("Disconnect"), QStringLiteral("Connection"), QStringLiteral("Disconnect"),
		    QStringLiteral("Shift+Ctrl+K"));
		add(QStringLiteral("ConnectToAllOpenWorlds"), QStringLiteral("Connection"),
		    QStringLiteral("Connect To All Open Worlds"), QStringLiteral("Ctrl+Alt+K"));

		add(QStringLiteral("ActivateInputArea"), QStringLiteral("Input"),
		    QStringLiteral("Activate Input Area"), QStringLiteral("Tab"));
		add(QStringLiteral("NextCommand"), QStringLiteral("Input"), QStringLiteral("Next Command"),
		    QStringLiteral("Alt+Down"));
		add(QStringLiteral("PreviousCommand"), QStringLiteral("Input"), QStringLiteral("Previous Command"),
		    QStringLiteral("Alt+Up"));
		add(QStringLiteral("CommandOptionNext"), QStringLiteral("Input"),
		    QStringLiteral("Next Command (Ctrl+N world option)"), QStringLiteral("Ctrl+N"));
		add(QStringLiteral("CommandOptionPrevious"), QStringLiteral("Input"),
		    QStringLiteral("Previous Command (Ctrl+P world option)"), QStringLiteral("Ctrl+P"));
		add(QStringLiteral("CommandOptionBufferEnd"), QStringLiteral("Input"),
		    QStringLiteral("Buffer End (Ctrl+Z world option)"), QStringLiteral("Ctrl+Z"));
		add(QStringLiteral("RepeatLastCommand"), QStringLiteral("Input"),
		    QStringLiteral("Repeat Last Command"), QStringLiteral("Ctrl+R"));
		add(QStringLiteral("QuitFromWorld"), QStringLiteral("Input"), QStringLiteral("Quit From This World"),
		    QStringLiteral("Shift+Ctrl+Q"));
		add(QStringLiteral("CommandHistory"), QStringLiteral("Input"), QStringLiteral("Command History"),
		    QStringLiteral("Ctrl+H"));
		add(QStringLiteral("ClearCommandHistory"), QStringLiteral("Input"),
		    QStringLiteral("Clear Command History"), QStringLiteral("Shift+Ctrl+D"));
		add(QStringLiteral("DiscardQueuedCommands"), QStringLiteral("Input"),
		    QStringLiteral("Discard Queued Commands"), QStringLiteral("Ctrl+D"));
		add(QStringLiteral("AutoSay"), QStringLiteral("Input"), QStringLiteral("Auto Say"),
		    QStringLiteral("Shift+Ctrl+A"));
		add(QStringLiteral("SendFile"), QStringLiteral("Input"), QStringLiteral("Send File"),
		    QStringLiteral("Shift+Ctrl+O"));
		add(QStringLiteral("GlobalChange"), QStringLiteral("Input"), QStringLiteral("Global Change"),
		    QStringLiteral("Shift+Ctrl+G"));

		add(QStringLiteral("DisplayStart"), QStringLiteral("Display"), QStringLiteral("Buffer Start"),
		    QStringLiteral("Ctrl+Home"));
		add(QStringLiteral("DisplayPageUp"), QStringLiteral("Display"), QStringLiteral("Buffer Page Up"),
		    QStringLiteral("PgUp"));
		add(QStringLiteral("DisplayPageDown"), QStringLiteral("Display"), QStringLiteral("Buffer Page Down"),
		    QStringLiteral("PgDown"));
		add(QStringLiteral("DisplayEnd"), QStringLiteral("Display"), QStringLiteral("Buffer End"),
		    QStringLiteral("Ctrl+End"));
		add(QStringLiteral("OutputSplitStart"), QStringLiteral("Display"),
		    QStringLiteral("Split Output Start"), QStringLiteral("Ctrl+Shift+Home"));
		add(QStringLiteral("OutputSplitEnd"), QStringLiteral("Display"), QStringLiteral("Split Output End"),
		    QStringLiteral("Ctrl+Shift+End"));
		add(QStringLiteral("DisplayLineUp"), QStringLiteral("Display"), QStringLiteral("Buffer Line Up"),
		    QStringLiteral("Ctrl+Up"));
		add(QStringLiteral("DisplayLineDown"), QStringLiteral("Display"), QStringLiteral("Buffer Line Down"),
		    QStringLiteral("Ctrl+Down"));
		add(QStringLiteral("ActivityList"), QStringLiteral("Display"), QStringLiteral("Activity List"),
		    QStringLiteral("Shift+Ctrl+L"));
		add(QStringLiteral("FreezeOutput"), QStringLiteral("Display"), QStringLiteral("Pause Output"),
		    QStringLiteral("Ctrl+Space"));
		add(QStringLiteral("Find"), QStringLiteral("Display"), QStringLiteral("Find"),
		    QStringLiteral("Ctrl+F"));
		add(QStringLiteral("FindAgain"), QStringLiteral("Display"), QStringLiteral("Find Again"),
		    QStringLiteral("Shift+Ctrl+F"));
		add(QStringLiteral("RecallText"), QStringLiteral("Display"), QStringLiteral("Recall Text"),
		    QStringLiteral("Ctrl+U"));
		add(QStringLiteral("GoToLine"), QStringLiteral("Display"), QStringLiteral("Go To Line"),
		    QStringLiteral("Ctrl+Alt+L"));
		add(QStringLiteral("GoToUrl"), QStringLiteral("Display"), QStringLiteral("Go To URL"),
		    QStringLiteral("Ctrl+Alt+J"));
		add(QStringLiteral("ClearOutputBuffer"), QStringLiteral("Display"),
		    QStringLiteral("Clear Output Buffer"), QStringLiteral("Shift+Ctrl+C"));
		add(QStringLiteral("StopSoundPlaying"), QStringLiteral("Display"),
		    QStringLiteral("Stop Sound Playing"), QStringLiteral("Ctrl+Alt+B"));
		add(QStringLiteral("BookmarkSelection"), QStringLiteral("Display"),
		    QStringLiteral("Bookmark Selection"), QStringLiteral("Shift+Ctrl+B"));
		add(QStringLiteral("GoToBookmark"), QStringLiteral("Display"), QStringLiteral("Go To Bookmark"),
		    QStringLiteral("Ctrl+B"));
		add(QStringLiteral("HighlightWord"), QStringLiteral("Display"), QStringLiteral("Highlight Word"),
		    QStringLiteral("Ctrl+Alt+H"));
		add(QStringLiteral("TextAttributes"), QStringLiteral("Display"), QStringLiteral("Text Attributes"),
		    QStringLiteral("Ctrl+Alt+A"));
		add(QStringLiteral("NoCommandEcho"), QStringLiteral("Display"), QStringLiteral("No Command Echo"),
		    QStringLiteral("Ctrl+Alt+E"));

		add(QStringLiteral("ConfigureMudAddress"), QStringLiteral("World"),
		    QStringLiteral("MUD Name/IP address"), QStringLiteral("Alt+1"));
		add(QStringLiteral("ConfigureConnecting"), QStringLiteral("World"), QStringLiteral("Connecting"),
		    QStringLiteral("Alt+2"));
		add(QStringLiteral("ConfigureChat"), QStringLiteral("World"), QStringLiteral("Chat"),
		    QStringLiteral("Alt+7"));
		add(QStringLiteral("ConfigureLogging"), QStringLiteral("World"), QStringLiteral("Logging"),
		    QStringLiteral("Alt+3"));
		add(QStringLiteral("ConfigureNotes"), QStringLiteral("World"), QStringLiteral("Notes"),
		    QStringLiteral("Alt+4"));
		add(QStringLiteral("ConfigureOutput"), QStringLiteral("World"), QStringLiteral("Output"),
		    QStringLiteral("Alt+5"));
		add(QStringLiteral("ConfigureMxp"), QStringLiteral("World"), QStringLiteral("MXP / Pueblo"),
		    QStringLiteral("Ctrl+Alt+U"));
		add(QStringLiteral("ConfigureAnsiColours"), QStringLiteral("World"), QStringLiteral("ANSI Colours"),
		    QStringLiteral("Alt+6"));
		add(QStringLiteral("ConfigureCustomColours"), QStringLiteral("World"),
		    QStringLiteral("Custom Colours"), QStringLiteral("Alt+8"));
		add(QStringLiteral("ConfigurePrinting"), QStringLiteral("World"), QStringLiteral("Printing"),
		    QStringLiteral("Alt+9"));
		add(QStringLiteral("ConfigureCommands"), QStringLiteral("World"), QStringLiteral("Commands"),
		    QStringLiteral("Alt+0"));
		add(QStringLiteral("ConfigureKeypad"), QStringLiteral("World"), QStringLiteral("Keypad"),
		    QStringLiteral("Shift+Ctrl+1"));
		add(QStringLiteral("ConfigureMacros"), QStringLiteral("World"), QStringLiteral("Macros"),
		    QStringLiteral("Shift+Ctrl+2"));
		add(QStringLiteral("ConfigureAutoSay"), QStringLiteral("World"), QStringLiteral("Auto say"),
		    QStringLiteral("Shift+Ctrl+3"));
		add(QStringLiteral("ConfigurePaste"), QStringLiteral("World"), QStringLiteral("Paste to world"),
		    QStringLiteral("Shift+Ctrl+4"));
		add(QStringLiteral("ConfigureSendFile"), QStringLiteral("World"), QStringLiteral("Send file"),
		    QStringLiteral("Shift+Ctrl+5"));
		add(QStringLiteral("ConfigureScripting"), QStringLiteral("World"), QStringLiteral("Scripting"),
		    QStringLiteral("Shift+Ctrl+6"));
		add(QStringLiteral("ConfigureVariables"), QStringLiteral("World"), QStringLiteral("Variables"),
		    QStringLiteral("Shift+Ctrl+7"));
		add(QStringLiteral("ConfigureTimers"), QStringLiteral("World"), QStringLiteral("Timers"),
		    QStringLiteral("Shift+Ctrl+0"));
		add(QStringLiteral("ConfigureTriggers"), QStringLiteral("World"), QStringLiteral("Triggers"),
		    QStringLiteral("Shift+Ctrl+8"));
		add(QStringLiteral("ConfigureAliases"), QStringLiteral("World"), QStringLiteral("Aliases"),
		    QStringLiteral("Shift+Ctrl+9"));
		add(QStringLiteral("ConfigureInfo"), QStringLiteral("World"), QStringLiteral("Info"),
		    QStringLiteral("Shift+Ctrl+I"));
		add(QStringLiteral("ChatSessions"), QStringLiteral("World"), QStringLiteral("Chat Sessions"),
		    QStringLiteral("Ctrl+Alt+Shift+C"));
		add(QStringLiteral("TestTrigger"), QStringLiteral("World"), QStringLiteral("Test Trigger"),
		    QStringLiteral("Shift+Ctrl+F12"));
		add(QStringLiteral("Minimize"), QStringLiteral("World"), QStringLiteral("Minimize Program"),
		    QStringLiteral("Ctrl+M"));
		add(QStringLiteral("Immediate"), QStringLiteral("World"), QStringLiteral("Immediate"),
		    QStringLiteral("Ctrl+I"));
		add(QStringLiteral("EditScriptFile"), QStringLiteral("World"), QStringLiteral("Edit Script File"),
		    QStringLiteral("Shift+Ctrl+H"));
		add(QStringLiteral("ReloadScriptFile"), QStringLiteral("World"), QStringLiteral("Reload Script File"),
		    QStringLiteral("Shift+Ctrl+R"));
		add(QStringLiteral("Trace"), QStringLiteral("World"), QStringLiteral("Trace"),
		    QStringLiteral("Ctrl+Alt+T"));
		add(QStringLiteral("ResetAllTimers"), QStringLiteral("World"), QStringLiteral("Reset All Timers"),
		    QStringLiteral("Shift+Ctrl+T"));
		add(QStringLiteral("SendToAllWorlds"), QStringLiteral("World"), QStringLiteral("Send To All Worlds"),
		    QStringLiteral("Ctrl+Alt+S"));
		add(QStringLiteral("Mapper"), QStringLiteral("World"), QStringLiteral("Mapper"),
		    QStringLiteral("Ctrl+Alt+M"));
		add(QStringLiteral("MapperSpecial"), QStringLiteral("World"), QStringLiteral("Do Mapper Special"),
		    QStringLiteral("Ctrl+Alt+D"));
		add(QStringLiteral("MapperComment"), QStringLiteral("World"), QStringLiteral("Add Mapper Comment"),
		    QStringLiteral("Ctrl+Alt+Shift+D"));

		add(QStringLiteral("WindowMinimize"), QStringLiteral("Window"), QStringLiteral("Minimize Window"),
		    QStringLiteral("Shift+Ctrl+M"));
		add(QStringLiteral("SelectPreviousTab"), QStringLiteral("Window"),
		    QStringLiteral("Select Previous Tab"), QStringLiteral("Ctrl+Shift+Left"));
		add(QStringLiteral("SelectNextTab"), QStringLiteral("Window"), QStringLiteral("Select Next Tab"),
		    QStringLiteral("Ctrl+Shift+Right"));
		add(QStringLiteral("World1"), QStringLiteral("Window"), QStringLiteral("Select World 1"),
		    QStringLiteral("Ctrl+1"));
		add(QStringLiteral("World2"), QStringLiteral("Window"), QStringLiteral("Select World 2"),
		    QStringLiteral("Ctrl+2"));
		add(QStringLiteral("World3"), QStringLiteral("Window"), QStringLiteral("Select World 3"),
		    QStringLiteral("Ctrl+3"));
		add(QStringLiteral("World4"), QStringLiteral("Window"), QStringLiteral("Select World 4"),
		    QStringLiteral("Ctrl+4"));
		add(QStringLiteral("World5"), QStringLiteral("Window"), QStringLiteral("Select World 5"),
		    QStringLiteral("Ctrl+5"));
		add(QStringLiteral("World6"), QStringLiteral("Window"), QStringLiteral("Select World 6"),
		    QStringLiteral("Ctrl+6"));
		add(QStringLiteral("World7"), QStringLiteral("Window"), QStringLiteral("Select World 7"),
		    QStringLiteral("Ctrl+7"));
		add(QStringLiteral("World8"), QStringLiteral("Window"), QStringLiteral("Select World 8"),
		    QStringLiteral("Ctrl+8"));
		add(QStringLiteral("World9"), QStringLiteral("Window"), QStringLiteral("Select World 9"),
		    QStringLiteral("Ctrl+9"));
		add(QStringLiteral("World10"), QStringLiteral("Window"), QStringLiteral("Select World 10"),
		    QStringLiteral("Ctrl+0"));

		add(QStringLiteral("HelpForum"), QStringLiteral("Help"), QStringLiteral("Discord"),
		    QStringLiteral("Shift+Ctrl+Alt+F"));
		add(QStringLiteral("FunctionsList"), QStringLiteral("Help"), QStringLiteral("Functions List"),
		    QStringLiteral("Shift+Ctrl+Alt+L"));
		add(QStringLiteral("FunctionsWebPage"), QStringLiteral("Help"), QStringLiteral("Functions Web Page"),
		    QStringLiteral("Shift+Ctrl+Alt+U"));
		add(QStringLiteral("WebPage"), QStringLiteral("Help"), QStringLiteral("QMud Web Page"),
		    QStringLiteral("Shift+Ctrl+Alt+W"));

		return defs;
	}

	Qt::KeyboardModifiers normalizedModifiers(const Qt::KeyboardModifiers modifiers)
	{
		return modifiers & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
	}

	QKeyCombination normalizedKeyCombination(const QKeySequence &sequence)
	{
		if (sequence.isEmpty() || sequence.count() != 1)
			return {};
		return sequence[0];
	}

	bool matchesKeyAndModifiers(const QKeyEvent *event, const QKeyCombination combination)
	{
		if (!event || combination.key() == Qt::Key_unknown)
			return false;
		return event->key() == combination.key() && normalizedModifiers(event->modifiers()) ==
		                                                normalizedModifiers(combination.keyboardModifiers());
	}

	bool sequenceListIsValid(const QList<QKeySequence> &shortcuts)
	{
		if (shortcuts.isEmpty())
			return false;
		return std::ranges::all_of(shortcuts,
		                           [](const QKeySequence &shortcut)
		                           {
			                           return !shortcut.isEmpty() && shortcut.count() == 1 &&
			                                  !QMudShortcutPreferenceUtils::isReservedMacroShortcut(shortcut);
		                           });
	}
} // namespace

namespace QMudShortcutPreferenceUtils
{
	const QList<ShortcutDefinition> &shortcutDefinitions()
	{
		static const QList<ShortcutDefinition> definitions = buildShortcutDefinitions();
		return definitions;
	}

	const ShortcutDefinition *definitionForId(const QString &id)
	{
		for (const ShortcutDefinition &definition : shortcutDefinitions())
		{
			if (definition.id.compare(id.trimmed(), Qt::CaseInsensitive) == 0)
				return &definition;
		}
		return nullptr;
	}

	const ShortcutDefinition *definitionForPreferenceKey(const QString &preferenceKey)
	{
		for (const ShortcutDefinition &definition : shortcutDefinitions())
		{
			if (definition.preferenceKey.compare(preferenceKey.trimmed(), Qt::CaseInsensitive) == 0)
				return &definition;
		}
		return nullptr;
	}

	bool isShortcutPreferenceKey(const QString &preferenceKey)
	{
		return definitionForPreferenceKey(preferenceKey) != nullptr;
	}

	QList<QKeySequence> parseShortcutList(const QString &text)
	{
		QList<QKeySequence> shortcuts;
		for (const QString &part : text.split(QLatin1Char(';'), Qt::SkipEmptyParts))
			shortcuts.push_back(shortcutFromPortable(part.trimmed()));
		return shortcuts;
	}

	QString shortcutListToPortableText(const QList<QKeySequence> &shortcuts)
	{
		QStringList parts;
		parts.reserve(shortcuts.size());
		for (const QKeySequence &shortcut : shortcuts)
		{
			if (!shortcut.isEmpty())
				parts.push_back(shortcut.toString(QKeySequence::PortableText));
		}
		return parts.join(QStringLiteral("; "));
	}

	QString shortcutListToNativeText(const QList<QKeySequence> &shortcuts)
	{
		QStringList parts;
		parts.reserve(shortcuts.size());
		for (const QKeySequence &shortcut : shortcuts)
		{
			const QString text = shortcut.toString(QKeySequence::NativeText);
			if (!text.isEmpty())
				parts.push_back(text);
		}
		return parts.join(QStringLiteral("; "));
	}

	QList<QKeySequence> effectiveShortcuts(const ShortcutDefinition &definition, const QString &overrideText)
	{
		const QString trimmed = overrideText.trimmed();
		if (trimmed.isEmpty())
			return definition.defaults;

		QList<QKeySequence> overrideShortcuts = parseShortcutList(trimmed);
		if (!sequenceListIsValid(overrideShortcuts))
			return definition.defaults;
		return overrideShortcuts;
	}

	bool isReservedMacroShortcut(const QKeySequence &sequence)
	{
		const QKeyCombination combination = normalizedKeyCombination(sequence);
		if (combination.key() == Qt::Key_unknown)
			return false;

		const Qt::KeyboardModifiers modifiers = normalizedModifiers(combination.keyboardModifiers());
		const Qt::Key               key       = combination.key();
		const bool                  noMeta    = !modifiers.testFlag(Qt::MetaModifier);
		if (!noMeta)
			return false;

		const bool hasAlt   = modifiers.testFlag(Qt::AltModifier);
		const bool hasCtrl  = modifiers.testFlag(Qt::ControlModifier);
		const bool hasShift = modifiers.testFlag(Qt::ShiftModifier);

		if (hasAlt && !hasCtrl && !hasShift && key >= Qt::Key_A && key <= Qt::Key_Z)
		{
			static const QSet<Qt::Key> reservedAltLetters = {
			    Qt::Key_A, Qt::Key_B, Qt::Key_J, Qt::Key_K, Qt::Key_L, Qt::Key_M,
			    Qt::Key_N, Qt::Key_O, Qt::Key_P, Qt::Key_Q, Qt::Key_R, Qt::Key_S,
			    Qt::Key_T, Qt::Key_U, Qt::Key_X, Qt::Key_Y, Qt::Key_Z,
			};
			return reservedAltLetters.contains(key);
		}

		if (hasAlt || key < Qt::Key_F1 || key > Qt::Key_F12)
			return false;
		if (hasCtrl && hasShift)
			return false;

		if (!hasCtrl && !hasShift)
			return true;
		if (hasShift)
			return true;
		if (hasCtrl)
			return key != Qt::Key_F4;
		return false;
	}

	bool eventMatchesShortcut(const QKeyEvent *event, const QKeySequence &sequence)
	{
		return matchesKeyAndModifiers(event, normalizedKeyCombination(sequence));
	}

	bool eventMatchesAnyShortcut(const QKeyEvent *event, const QList<QKeySequence> &shortcuts)
	{
		return std::ranges::any_of(shortcuts, [event](const QKeySequence &shortcut)
		                           { return eventMatchesShortcut(event, shortcut); });
	}

	QList<QKeySequence> defaultShortcutsForId(const QString &id)
	{
		if (const ShortcutDefinition *definition = definitionForId(id))
			return definition->defaults;
		return {};
	}

	QList<QKeySequence> effectiveShortcutsForId(const QString &id)
	{
		const ShortcutDefinition *definition = definitionForId(id);
		if (!definition)
			return {};
		const AppController *app = AppController::instance();
		return effectiveShortcuts(
		    *definition, app ? app->getGlobalOption(definition->preferenceKey).toString() : QString());
	}

	bool eventMatchesShortcutId(const QKeyEvent *event, const QString &id)
	{
		return eventMatchesAnyShortcut(event, effectiveShortcutsForId(id));
	}
} // namespace QMudShortcutPreferenceUtils
