/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldCommandProcessor_CommandStacking.cpp
 * Role: Integration coverage for command-stack separator parsing through WorldCommandProcessor.
 */

#include "WorldCommandProcessor.h"
#include "WorldOptions.h"
#include "WorldRuntime.h"
#include "helpers/WorldCommandProcessorUtils.h"
#include "scripting/ScriptingErrors.h"

#include <QRegularExpression>
#include <QtTest/QTest>

namespace
{
	/**
	 * @brief Creates an alias that records an exact command-stack result through normal alias matching.
	 * @param command Exact command text to match.
	 * @return Enabled output alias for the requested command.
	 */
	WorldRuntime::Alias recordingAlias(const QString &command)
	{
		WorldRuntime::Alias alias;
		alias.attributes.insert(QStringLiteral("enabled"), QStringLiteral("y"));
		alias.attributes.insert(QStringLiteral("match"), QStringLiteral("^") +
		                                                     QRegularExpression::escape(command) +
		                                                     QStringLiteral("$"));
		alias.attributes.insert(QStringLiteral("regexp"), QStringLiteral("y"));
		alias.attributes.insert(QStringLiteral("send_to"), QString::number(eSendToOutput));
		alias.children.insert(QStringLiteral("send"), QStringLiteral("matched"));
		return alias;
	}

	/**
	 * @brief QTest fixture covering command-stack separator behavior.
	 */
	class tst_WorldCommandProcessor_CommandStacking final : public QObject
	{
			Q_OBJECT

		private slots:
			static void separatorValidationRejectsUnsupportedValues()
			{
				QVERIFY(QMudCommandStack::isValidSeparator(QStringLiteral(";")));
				QVERIFY(QMudCommandStack::isValidSeparator(QStringLiteral("::")));
				QVERIFY(!QMudCommandStack::isValidSeparator(QString()));
				QVERIFY(!QMudCommandStack::isValidSeparator(QStringLiteral(":::")));
				QVERIFY(!QMudCommandStack::isValidSeparator(QStringLiteral(" :")));
				QVERIFY(!QMudCommandStack::isValidSeparator(QStringLiteral("\n")));
				QCOMPARE(QMudCommandStack::expand(QStringLiteral("north::south"), QString()),
				         QStringList{QStringLiteral("north::south")});
			}

			static void singleCharacterSeparatorRetainsEscapingAcrossStackedCommands()
			{
				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("enable_aliases"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("enable_command_stack"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("command_stack_character"), QStringLiteral(";"));
				runtime.setAliases(
				    {recordingAlias(QStringLiteral("say ;thing")), recordingAlias(QStringLiteral("look"))});

				WorldCommandProcessor processor;
				processor.setRuntime(&runtime);

				QCOMPARE(processor.executeCommand(QStringLiteral("say ;;thing;look")), eOK);
				QCOMPARE(runtime.aliases().at(0).matched, 1);
				QCOMPARE(runtime.aliases().at(1).matched, 1);
			}

			static void twoCharacterSeparatorSplitsEscapesAndDisablesPerLine()
			{
				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("enable_aliases"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("enable_command_stack"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("command_stack_character"), QStringLiteral("::"));
				runtime.setAliases({recordingAlias(QStringLiteral("north")),
				                    recordingAlias(QStringLiteral("south")),
				                    recordingAlias(QStringLiteral("say :::::x")),
				                    recordingAlias(QStringLiteral("north::south")),
				                    recordingAlias(QString(5, QLatin1Char(':')))});

				WorldCommandProcessor processor;
				processor.setRuntime(&runtime);

				QCOMPARE(processor.executeCommand(QStringLiteral("north::south")), eOK);
				QCOMPARE(runtime.aliases().at(0).matched, 1);
				QCOMPARE(runtime.aliases().at(1).matched, 1);

				QCOMPARE(processor.executeCommand(QStringLiteral("north\nsouth")), eOK);
				QCOMPARE(runtime.aliases().at(0).matched, 2);
				QCOMPARE(runtime.aliases().at(1).matched, 2);

				const QString escapedLiteral =
				    QStringLiteral("say ") + QString(9, QLatin1Char(':')) + QStringLiteral("x::south");
				QCOMPARE(processor.executeCommand(escapedLiteral), eOK);
				QCOMPARE(runtime.aliases().at(2).matched, 1);
				QCOMPARE(runtime.aliases().at(1).matched, 3);

				QCOMPARE(processor.executeCommand(QStringLiteral("north::::south")), eOK);
				QCOMPARE(runtime.aliases().at(3).matched, 1);

				QCOMPARE(processor.executeCommand(QStringLiteral("::north::south")), eOK);
				QCOMPARE(runtime.aliases().at(3).matched, 2);

				QCOMPARE(processor.executeCommand(QString(7, QLatin1Char(':'))), eOK);
				QCOMPARE(runtime.aliases().at(4).matched, 1);
			}

			static void distinctTwoCharacterSeparatorSplitsAndEscapesAsOneToken()
			{
				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("enable_aliases"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("enable_command_stack"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("command_stack_character"), QStringLiteral("->"));
				runtime.setAliases({recordingAlias(QStringLiteral("north")),
				                    recordingAlias(QStringLiteral("south")),
				                    recordingAlias(QStringLiteral("say north->south"))});

				WorldCommandProcessor processor;
				processor.setRuntime(&runtime);

				QCOMPARE(processor.executeCommand(QStringLiteral("north->south")), eOK);
				QCOMPARE(runtime.aliases().at(0).matched, 1);
				QCOMPARE(runtime.aliases().at(1).matched, 1);

				QCOMPARE(processor.executeCommand(QStringLiteral("say north->->south")), eOK);
				QCOMPARE(runtime.aliases().at(2).matched, 1);
			}
	};
} // namespace

QTEST_MAIN(tst_WorldCommandProcessor_CommandStacking)

#if __has_include("tst_WorldCommandProcessor_CommandStacking.moc")
#include "tst_WorldCommandProcessor_CommandStacking.moc"
#endif
