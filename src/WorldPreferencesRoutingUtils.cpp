/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldPreferencesRoutingUtils.cpp
 * Role: Pure routing helpers for resolving world-preferences dialog commands to initial page selections.
 */

#include "WorldPreferencesRoutingUtils.h"

#include <QString>

namespace
{
bool matchesCommand(const QMudWorldPreferencesRouting::CommandMatcher &isCommand, const QString &commandName)
{
	return isCommand && isCommand(commandName);
}
} // namespace

namespace QMudWorldPreferencesRouting
{
bool isPreferencesCommand(const QString &cmdName, const CommandMatcher &isCommand)
{
	return cmdName == QStringLiteral("Preferences") || matchesCommand(isCommand, QStringLiteral("ConfigureMudAddress")) ||
	       cmdName == QStringLiteral("ConfigureConnecting") || cmdName == QStringLiteral("ConfigureChat") ||
	       cmdName == QStringLiteral("ConfigureLogging") || cmdName == QStringLiteral("ConfigureOutput") ||
	       matchesCommand(isCommand, QStringLiteral("ConfigureMxp")) ||
	       cmdName == QStringLiteral("ConfigureAnsiColours") ||
	       cmdName == QStringLiteral("ConfigureCustomColours") ||
	       cmdName == QStringLiteral("ConfigurePrinting") || cmdName == QStringLiteral("ConfigureCommands") ||
	       cmdName == QStringLiteral("ConfigureKeypad") || cmdName == QStringLiteral("ConfigureMacros") ||
	       matchesCommand(isCommand, QStringLiteral("ConfigureAutoSay")) ||
	       matchesCommand(isCommand, QStringLiteral("ConfigurePaste")) ||
	       cmdName == QStringLiteral("ConfigureSendFile") || cmdName == QStringLiteral("ConfigureScripting") ||
	       cmdName == QStringLiteral("ConfigureVariables") || cmdName == QStringLiteral("ConfigureTimers") ||
	       cmdName == QStringLiteral("ConfigureTriggers") || cmdName == QStringLiteral("ConfigureAliases") ||
	       cmdName == QStringLiteral("ConfigureInfo") || cmdName == QStringLiteral("ConfigureNotes");
}

WorldPreferencesDialog::Page initialPageForCommand(const QString &cmdName, const int lastPreferencesPage,
                                                   const CommandMatcher &isCommand)
{
	WorldPreferencesDialog::Page page = WorldPreferencesDialog::PageGeneral;
	if (cmdName == QStringLiteral("Preferences"))
	{
		if (constexpr int maxPage = WorldPreferencesDialog::PageChat;
		    lastPreferencesPage >= 0 && lastPreferencesPage <= maxPage)
			page = static_cast<WorldPreferencesDialog::Page>(lastPreferencesPage);
	}
	else if (matchesCommand(isCommand, QStringLiteral("ConfigureMudAddress")))
		page = WorldPreferencesDialog::PageGeneral;
	else if (cmdName == QStringLiteral("ConfigureConnecting"))
		page = WorldPreferencesDialog::PageConnecting;
	else if (cmdName == QStringLiteral("ConfigureChat"))
		page = WorldPreferencesDialog::PageChat;
	else if (cmdName == QStringLiteral("ConfigureLogging"))
		page = WorldPreferencesDialog::PageLogging;
	else if (cmdName == QStringLiteral("ConfigureOutput"))
		page = WorldPreferencesDialog::PageOutput;
	else if (matchesCommand(isCommand, QStringLiteral("ConfigureMxp")))
		page = WorldPreferencesDialog::PageMxp;
	else if (cmdName == QStringLiteral("ConfigureAnsiColours"))
		page = WorldPreferencesDialog::PageAnsiColours;
	else if (cmdName == QStringLiteral("ConfigureCustomColours"))
		page = WorldPreferencesDialog::PageCustomColours;
	else if (cmdName == QStringLiteral("ConfigurePrinting"))
		page = WorldPreferencesDialog::PagePrinting;
	else if (cmdName == QStringLiteral("ConfigureCommands"))
		page = WorldPreferencesDialog::PageCommands;
	else if (cmdName == QStringLiteral("ConfigureKeypad"))
		page = WorldPreferencesDialog::PageKeypad;
	else if (cmdName == QStringLiteral("ConfigureMacros"))
		page = WorldPreferencesDialog::PageMacros;
	else if (matchesCommand(isCommand, QStringLiteral("ConfigureAutoSay")))
		page = WorldPreferencesDialog::PageAutoSay;
	else if (matchesCommand(isCommand, QStringLiteral("ConfigurePaste")))
		page = WorldPreferencesDialog::PagePaste;
	else if (cmdName == QStringLiteral("ConfigureSendFile"))
		page = WorldPreferencesDialog::PageSendToWorld;
	else if (cmdName == QStringLiteral("ConfigureScripting"))
		page = WorldPreferencesDialog::PageScripting;
	else if (cmdName == QStringLiteral("ConfigureVariables"))
		page = WorldPreferencesDialog::PageVariables;
	else if (cmdName == QStringLiteral("ConfigureTimers"))
		page = WorldPreferencesDialog::PageTimers;
	else if (cmdName == QStringLiteral("ConfigureTriggers"))
		page = WorldPreferencesDialog::PageTriggers;
	else if (cmdName == QStringLiteral("ConfigureAliases"))
		page = WorldPreferencesDialog::PageAliases;
	else if (cmdName == QStringLiteral("ConfigureInfo"))
		page = WorldPreferencesDialog::PageInfo;
	else if (cmdName == QStringLiteral("ConfigureNotes"))
		page = WorldPreferencesDialog::PageNotes;
	return page;
}
} // namespace QMudWorldPreferencesRouting
