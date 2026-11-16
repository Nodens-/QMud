/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldCommandProcessor_CommandStacking.cpp
 * Role: Integration coverage for command-stack separator parsing through WorldCommandProcessor.
 */

#include "AppController.h"
#include "MainFrame.h"
#include "WorldCommandProcessor.h"
#include "WorldOptions.h"
#include "WorldRuntime.h"
#include "WorldView.h"
#include "helpers/WorldCommandProcessorUtils.h"
#include "scripting/ScriptingErrors.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QApplication>
#include <QDialog>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QRegularExpression>
#include <QScopeGuard>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryDir>
#include <QtTest/QTest>

namespace
{
	const QString kCommandDecisionPluginId = QStringLiteral("454545454545454545454545");

	/**
	 * @brief Runs a queued test action when the command-decision dialog enters its event loop.
	 */
	class CommandDecisionObserver final : public QObject
	{
		public:
			/**
			 * @brief Installs the dialog observer on the test application.
			 */
			CommandDecisionObserver()
			{
				qApp->installEventFilter(this);
			}
			/**
			 * @brief Removes the dialog observer from the test application.
			 */
			~CommandDecisionObserver() override
			{
				qApp->removeEventFilter(this);
			}
			std::function<void(QDialog *)> shown;

			/**
			 * @brief Queues the observer action for a newly shown command-decision dialog.
			 * @param watched Object receiving the event.
			 * @param event Delivered event.
			 * @return Base event-filter result.
			 */
			bool                           eventFilter(QObject *watched, QEvent *event) override
			{
				if (event->type() == QEvent::Show)
					if (auto *dialog = qobject_cast<QDialog *>(watched);
					    dialog && dialog->windowTitle() == QStringLiteral("Prepared command decision"))
						QMetaObject::invokeMethod(
						    dialog,
						    [this, dialog]
						    {
							    if (shown)
								    shown(dialog);
						    },
						    Qt::QueuedConnection);
				return QObject::eventFilter(watched, event);
			}
	};

	/**
	 * @brief Writes a synthetic recipient for exercising QMud command callback suspension and veto behavior.
	 * @param path Destination fixture path inside the test build directory.
	 * @return Whether the complete fixture was written.
	 */
	bool writeCommandDecisionFixture(const QString &path)
	{
		QFile file(path);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
			return false;
		const QByteArray xml = QByteArrayLiteral(R"xml(<qmud>
<plugin name="CommandDecisionFixture" id="454545454545454545454545" language="Lua" enabled="y" save_state="n"/>
<script><![CDATA[
local commands = {}
function OnPluginCommandEntered(text)
 SetVariable('entered', text)
 return text
end
function OnPluginCommand(command)
 commands[#commands + 1] = command
 SetVariable('commands', table.concat(commands, '|'))
 if command == 'pause' then utils.inputbox('Synthetic decision', 'Prepared command decision', '') end
 return command ~= 'blocked'
end
]]></script>
</qmud>)xml");
		return file.write(xml) == xml.size();
	}

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
			/**
			 * @brief Supplies normal resumption and world-close cancellation of a suspended command decision.
			 */
			static void preparedCommandCallbacks_data()
			{
				QTest::addColumn<bool>("cancel");
				QTest::newRow("resume-and-veto") << false;
				QTest::newRow("cancel-on-world-close") << true;
			}

			/**
			 * @brief Retains command order and stops remaining prepared work when its suspended callback is canceled.
			 */
			static void preparedCommandCallbacks()
			{
				QFETCH(bool, cancel);
				const QString buildDirectory = QDir::cleanPath(
				    QDir(QStringLiteral(QT_TESTCASE_BUILDDIR)).filePath(QStringLiteral("..")));
				const QByteArray previousHome = qgetenv("QMUD_HOME");
				qputenv("QMUD_HOME", buildDirectory.toUtf8());
				const auto restoreHome = qScopeGuard(
				    [&]
				    {
					    if (previousHome.isNull())
						    qunsetenv("QMUD_HOME");
					    else
						    qputenv("QMUD_HOME", previousHome);
				    });
				QTemporaryDir directory(
				    QDir(buildDirectory).filePath(QStringLiteral("worlds/prepared-command-XXXXXX")));
				QVERIFY(directory.isValid());
				QVERIFY(writeCommandDecisionFixture(
				    QDir(directory.path()).filePath(QStringLiteral("decision.xml"))));
				AppController controller;
				MainWindow    frame;
				controller.setMainWindow(&frame);
				WorldRuntime runtime;
				runtime.setStartupDirectory(buildDirectory);
				runtime.setPluginsDirectory(directory.path());
				runtime.setWorldAttribute(QStringLiteral("enable_aliases"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
				runtime.setWorldAttribute(QStringLiteral("script_prefix"), QStringLiteral("/"));
				runtime.setAliases(
				    {recordingAlias(QStringLiteral("pause")), recordingAlias(QStringLiteral("blocked")),
					 recordingAlias(QStringLiteral("/literal")), recordingAlias(QStringLiteral("last"))});
				WorldView             view;
				WorldCommandProcessor processor;
				processor.setView(&view);
				processor.setRuntime(&runtime);
				runtime.setCommandProcessor(&processor);
				view.setRuntime(&runtime);
				const auto detach = qScopeGuard(
				    [&]
				    {
					    processor.setRuntime(nullptr);
					    runtime.setCommandProcessor(nullptr);
					    view.setRuntime(nullptr);
				    });
				view.resize(640, 480);
				view.show();
				QVERIFY(QTest::qWaitForWindowExposed(&view));
				QString error;
				QVERIFY2(runtime.loadPluginFile(QStringLiteral("decision.xml"), &error), qPrintable(error));
				QTRY_VERIFY(!runtime.pluginForId(kCommandDecisionPluginId)->installPending);
				const QString           input     = QStringLiteral("pause\nblocked\n/literal\nlast");
				bool                    shown     = false;
				bool                    finalized = false;
				QString                 enteredAtPause;
				QString                 commandsAtPause;
				QList<int>              matchesAtPause;
				CommandDecisionObserver observer;
				observer.shown = [&](QDialog *dialog)
				{
					shown = true;
					enteredAtPause =
					    runtime.pluginVariableValue(kCommandDecisionPluginId, QStringLiteral("entered"));
					commandsAtPause =
					    runtime.pluginVariableValue(kCommandDecisionPluginId, QStringLiteral("commands"));
					for (const auto &alias : runtime.aliases())
						matchesAtPause.push_back(alias.matched);
					if (cancel)
						QMetaObject::invokeMethod(
						    &runtime,
						    [&]
						    {
							    runtime.finalizePluginStateForClose();
							    finalized = true;
						    },
						    Qt::QueuedConnection);
					dialog->reject();
				};
				processor.onCommandEntered(input);
				QTRY_VERIFY(shown);
				QCOMPARE(enteredAtPause, input);
				QCOMPARE(commandsAtPause, QStringLiteral("pause"));
				QCOMPARE(matchesAtPause, QList<int>({0, 0, 0, 0}));
				if (cancel)
				{
					QTRY_VERIFY(finalized);
					for (const auto &alias : runtime.aliases())
						QCOMPARE(alias.matched, 0);
				}
				else
				{
					QTRY_COMPARE(runtime.aliases().at(3).matched, 1);
					QCOMPARE(runtime.aliases().at(0).matched, 1);
					QCOMPARE(runtime.aliases().at(1).matched, 0);
					QCOMPARE(runtime.aliases().at(2).matched, 1);
					QCOMPARE(
					    runtime.pluginVariableValue(kCommandDecisionPluginId, QStringLiteral("commands")),
					    QStringLiteral("pause|blocked|/literal|last"));
				}
			}

			/**
			 * @brief Supplies whole-input auto-say exclusions that must survive physical-line splitting.
			 */
			static void preparedLinesRetainAutoSayDecision_data()
			{
				QTest::addColumn<QString>("input");
				QTest::addColumn<QString>("first");
				QTest::addColumn<bool>("excludeNonAlpha");
				QTest::addColumn<bool>("excludeMacro");
				QTest::newRow("override")
				    << QStringLiteral("-first\nsecond") << QStringLiteral("first") << false << false;
				QTest::newRow("non-alpha")
				    << QStringLiteral("!first\nsecond") << QStringLiteral("!first") << true << false;
				QTest::newRow("already-prefixed")
				    << QStringLiteral("say first\nsecond") << QStringLiteral("say first") << false << false;
				QTest::newRow("macro-exclusion")
				    << QStringLiteral("first\nsecond") << QStringLiteral("first") << false << true;
			}

			/**
			 * @brief Executes every prepared line without reconsidering the original auto-say exclusion.
			 */
			static void preparedLinesRetainAutoSayDecision()
			{
				QFETCH(QString, input);
				QFETCH(QString, first);
				QFETCH(bool, excludeNonAlpha);
				QFETCH(bool, excludeMacro);
				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("enable_aliases"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("enable_auto_say"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("re_evaluate_auto_say"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("auto_say_string"), QStringLiteral("say "));
				runtime.setWorldAttribute(QStringLiteral("auto_say_override_prefix"), QStringLiteral("-"));
				runtime.setWorldAttribute(QStringLiteral("autosay_exclude_non_alpha"),
				                          excludeNonAlpha ? QStringLiteral("y") : QStringLiteral("n"));
				runtime.setWorldAttribute(QStringLiteral("autosay_exclude_macros"),
				                          excludeMacro ? QStringLiteral("y") : QStringLiteral("n"));
				if (excludeMacro)
				{
					WorldRuntime::Macro macro;
					macro.attributes.insert(QStringLiteral("name"), QStringLiteral("F1"));
					macro.attributes.insert(QStringLiteral("type"), QStringLiteral("send_now"));
					macro.children.insert(QStringLiteral("send"), first);
					runtime.setMacros({macro});
				}
				runtime.setAliases({recordingAlias(first), recordingAlias(QStringLiteral("second"))});
				WorldCommandProcessor processor;
				processor.setRuntime(&runtime);
				QCOMPARE(processor.executeCommand(input), eOK);
				QTRY_COMPARE(runtime.aliases().at(1).matched, 1);
				QCOMPARE(runtime.aliases().at(0).matched, 1);
			}

			/**
			 * @brief Keeps a later script-prefixed line on the command and alias path.
			 */
			static void laterScriptPrefixRemainsLiteral()
			{
				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("enable_aliases"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
				runtime.setWorldAttribute(QStringLiteral("script_prefix"), QStringLiteral("/"));
				const QString second = QStringLiteral("/SetVariable('unexpected_script', 'yes')");
				runtime.setAliases({recordingAlias(QStringLiteral("first")), recordingAlias(second)});
				WorldCommandProcessor processor;
				processor.setRuntime(&runtime);
				QCOMPARE(processor.executeCommand(QStringLiteral("first\n") + second), eOK);
				QTRY_COMPARE(runtime.aliases().at(1).matched, 1);
				QString unexpected;
				QVERIFY(!runtime.findVariable(QStringLiteral("unexpected_script"), unexpected));
			}

			/**
			 * @brief Starts nested Execute at whole-input preparation and completes it before the next outer line.
			 */
			static void nestedExecutePreparesFreshInput()
			{
				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("enable_aliases"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
				runtime.setWorldAttribute(QStringLiteral("script_prefix"), QStringLiteral("/"));
				runtime.setWorldAttribute(QStringLiteral("enable_auto_say"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("re_evaluate_auto_say"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("auto_say_string"), QStringLiteral("say "));
				runtime.setWorldAttribute(QStringLiteral("auto_say_override_prefix"), QStringLiteral("-"));
				auto first = recordingAlias(QStringLiteral("first"));
				first.attributes.insert(QStringLiteral("send_to"), QString::number(eSendToScript));
				first.children.insert(QStringLiteral("send"), QStringLiteral(R"lua(
assert(Execute("nested") == 0)
assert(Execute("-/SetVariable('nested_script', 'yes')") == 0)
)lua"));
				auto nested = recordingAlias(QStringLiteral("say nested"));
				nested.attributes.insert(QStringLiteral("send_to"), QString::number(eSendToScript));
				nested.children.insert(QStringLiteral("send"),
				                       QStringLiteral("SetVariable('nested_alias', 'yes')"));
				auto second = recordingAlias(QStringLiteral("second"));
				second.attributes.insert(QStringLiteral("send_to"), QString::number(eSendToScript));
				second.children.insert(
				    QStringLiteral("send"),
				    QStringLiteral(
				        "assert(GetVariable('nested_alias') == 'yes' and GetVariable('nested_script') == "
				        "'yes'); SetVariable('outer_completed', 'yes')"));
				runtime.setAliases({first, nested, second});
				WorldCommandProcessor processor;
				processor.setRuntime(&runtime);
				runtime.setCommandProcessor(&processor);
				const auto detach = qScopeGuard(
				    [&]
				    {
					    runtime.setCommandProcessor(nullptr);
					    processor.setRuntime(nullptr);
				    });
				QCOMPARE(processor.executeCommand(QStringLiteral("-first\nsecond")), eOK);
				QString value;
				QTRY_VERIFY(runtime.findVariable(QStringLiteral("outer_completed"), value));
				QCOMPARE(value, QStringLiteral("yes"));
				QCOMPARE(runtime.aliases().at(1).matched, 1);
			}

			/**
			 * @brief Retains full preparation for a Send to Execute action while leaving its separator text literal.
			 */
			static void sendToExecuteRetainsPreparation()
			{
				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("enable_aliases"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
				runtime.setWorldAttribute(QStringLiteral("script_prefix"), QStringLiteral("/"));
				runtime.setWorldAttribute(QStringLiteral("enable_command_stack"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("command_stack_character"), QStringLiteral(";"));
				auto action = recordingAlias(QStringLiteral("action"));
				action.attributes.insert(QStringLiteral("send_to"), QString::number(eSendToExecute));
				action.children.insert(
				    QStringLiteral("send"),
				    QStringLiteral("/SetVariable('execute_script', 'yes')\nfirst;literal"));
				runtime.setAliases({action, recordingAlias(QStringLiteral("first;literal"))});
				WorldCommandProcessor processor;
				processor.setRuntime(&runtime);
				QCOMPARE(processor.executeCommand(QStringLiteral("action")), eOK);
				QTRY_COMPARE(runtime.aliases().at(1).matched, 1);
				QString value;
				QVERIFY(runtime.findVariable(QStringLiteral("execute_script"), value));
				QCOMPARE(value, QStringLiteral("yes"));
			}

			/**
			 * @brief Preserves auto-say origin suppression for nested Execute calls from a reevaluated alias.
			 */
			static void autoSayNestedExecuteRetainsOrigin()
			{
				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("enable_aliases"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
				runtime.setWorldAttribute(QStringLiteral("script_prefix"), QStringLiteral("/"));
				runtime.setWorldAttribute(QStringLiteral("enable_auto_say"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("re_evaluate_auto_say"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("auto_say_string"), QStringLiteral("say "));
				auto outer = recordingAlias(QStringLiteral("say outer"));
				outer.attributes.insert(QStringLiteral("send_to"), QString::number(eSendToScript));
				outer.children.insert(
				    QStringLiteral("send"),
				    QStringLiteral("assert(Execute('nested') == 0); assert(Execute('/literal') == 0)"));
				runtime.setAliases({outer, recordingAlias(QStringLiteral("nested")),
				                    recordingAlias(QStringLiteral("/literal"))});
				WorldCommandProcessor processor;
				processor.setRuntime(&runtime);
				runtime.setCommandProcessor(&processor);
				const auto detach = qScopeGuard(
				    [&]
				    {
					    runtime.setCommandProcessor(nullptr);
					    processor.setRuntime(nullptr);
				    });
				QCOMPARE(processor.executeCommand(QStringLiteral("outer")), eOK);
				QTRY_COMPARE(runtime.aliases().at(2).matched, 1);
				QCOMPARE(runtime.aliases().at(1).matched, 1);
			}

			static void separatorValidationRejectsUnsupportedValues()
			{
				QVERIFY(QMudCommandStack::isValidSeparator(QStringLiteral(";")));
				QVERIFY(QMudCommandStack::isValidSeparator(QStringLiteral("::")));
				QVERIFY(!QMudCommandStack::isValidSeparator(QString()));
				QVERIFY(!QMudCommandStack::isValidSeparator(QStringLiteral(":::")));
				QVERIFY(!QMudCommandStack::isValidSeparator(QStringLiteral(" :")));
				QVERIFY(!QMudCommandStack::isValidSeparator(QStringLiteral("\n")));
				QCOMPARE(QMudCommandStack::expand(QStringLiteral("north::south"), QString()),
				         QStringLiteral("north::south"));
			}

			/**
			 * @brief Verifies independent newline splitting retains blank commands and separator expansion retains text.
			 */
			static void newlineSplittingIsIndependentOfSeparatorExpansion()
			{
				QCOMPARE(QMudCommandText::splitCommandLines(QString()), QStringList{QString()});
				QCOMPARE(QMudCommandText::splitCommandLines(QStringLiteral("\nfirst\n\nsecond\n")),
				         QStringList({QString(), QStringLiteral("first"), QString(), QStringLiteral("second"),
				                      QString()}));
				QCOMPARE(QMudCommandText::splitCommandLines(QStringLiteral("first;second")),
				         QStringList{QStringLiteral("first;second")});
				QCOMPARE(QMudCommandStack::expand(QStringLiteral("first\nsecond;third"), QStringLiteral(";")),
				         QStringLiteral("first\nsecond\nthird"));
				QCOMPARE(QMudCommandStack::expand(QStringLiteral(";first;literal\n;second;literal"),
				                                  QStringLiteral(";")),
				         QStringLiteral("first;literal\n;second;literal"));
			}

			/**
			 * @brief Supplies multiline commands across entry paths and command-stacking configurations.
			 */
			static void multilineAliases_data()
			{
				QTest::addColumn<bool>("stacking");
				QTest::addColumn<QString>("separator");
				QTest::addColumn<QString>("input");
				QTest::addColumn<QStringList>("expected");
				QTest::addColumn<int>("entry");
				const QStringList three{QStringLiteral("first"), QStringLiteral("second"),
				                        QStringLiteral("third")};
				for (int entry = 0; entry < 3; ++entry)
				{
					QTest::newRow(qPrintable(QStringLiteral("stacking-off-entry-%1").arg(entry)))
					    << false << QStringLiteral(";") << QStringLiteral("first\r\nsecond\nthird") << three
					    << entry;
				}
				QTest::newRow("empty-separator")
				    << true << QString() << QStringLiteral("first\nsecond\nthird") << three << 0;
				QTest::newRow("invalid-separator")
				    << true << QStringLiteral(":::") << QStringLiteral("first\nsecond\nthird") << three << 0;
				QTest::newRow("stacking-off-literals")
				    << false << QStringLiteral(";") << QStringLiteral("first;literal\nsecond;;literal")
				    << QStringList({QStringLiteral("first;literal"), QStringLiteral("second;;literal")}) << 0;
				QTest::newRow("stacking-on")
				    << true << QStringLiteral(";") << QStringLiteral("first;second\nthird") << three << 0;
				QTest::newRow("escaped-separator")
				    << true << QStringLiteral(";") << QStringLiteral("say ;;literal;first\nsecond")
				    << QStringList({QStringLiteral("say ;literal"), QStringLiteral("first"),
				                    QStringLiteral("second")})
				    << 0;
				QTest::newRow("two-character-separator")
				    << true << QStringLiteral("::") << QStringLiteral("first::second\nthird") << three << 0;
				QTest::newRow("leading-separator-bypass")
				    << true << QStringLiteral(";") << QStringLiteral(";first;literal\n;second;literal")
				    << QStringList({QStringLiteral("first;literal"), QStringLiteral(";second;literal")}) << 0;
				QTest::newRow("leading-two-character-bypass")
				    << true << QStringLiteral("::") << QStringLiteral("::first::literal\n::second::literal")
				    << QStringList({QStringLiteral("first::literal"), QStringLiteral("::second::literal")})
				    << 0;
			}

			/**
			 * @brief Matches and executes each physical command line in order without requiring command stacking.
			 */
			static void multilineAliases()
			{
				QFETCH(bool, stacking);
				QFETCH(QString, separator);
				QFETCH(QString, input);
				QFETCH(QStringList, expected);
				QFETCH(int, entry);
				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("enable_aliases"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("enable_command_stack"),
				                          stacking ? QStringLiteral("y") : QStringLiteral("n"));
				runtime.setWorldAttribute(QStringLiteral("command_stack_character"), separator);
				QList<WorldRuntime::Alias> aliases;
				for (const QString &command : expected)
				{
					auto alias = recordingAlias(command);
					alias.children.insert(QStringLiteral("send"), command);
					aliases.push_back(std::move(alias));
				}
				runtime.setAliases(aliases);
				QStringList           output;
				WorldCommandProcessor processor;
				processor.setRuntime(&runtime);
				QObject::connect(&runtime, &WorldRuntime::outputRequested, &processor,
				                 [&output](const QString &text)
				                 {
					                 if (!text.isEmpty())
						                 output.push_back(text);
				                 });
				if (entry == 1)
					processor.onCommandEntered(input);
				else if (entry == 2)
				{
					runtime.setCurrentActionSource(WorldRuntime::eUserMacro);
					QCOMPARE(processor.executeUserMacroSendNow(input, false), eOK);
				}
				else
					QCOMPARE(processor.executeCommand(input), eOK);
				for (qsizetype index = 0; index < aliases.size(); ++index)
					QTRY_COMPARE(runtime.aliases().at(index).matched, 1);
				QCOMPARE(output, expected);
			}

			/**
			 * @brief Preserves whole multiline Lua blocks recognized by the command script prefix.
			 */
			static void multilineScriptPrefixRemainsOneScript()
			{
				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
				runtime.setWorldAttribute(QStringLiteral("script_prefix"), QStringLiteral("/"));
				runtime.setWorldAttribute(QStringLiteral("enable_command_stack"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("command_stack_character"), QStringLiteral(";"));
				WorldCommandProcessor processor;
				processor.setRuntime(&runtime);
				QCOMPARE(processor.executeCommand(
				             QStringLiteral("/local text = 'first;literal'\ntext = text .. "
							                "'\\nsecond'\nSetVariable('script_result', text)")),
				         eOK);
				QString result;
				QTRY_VERIFY(runtime.findVariable(QStringLiteral("script_result"), result));
				QCOMPARE(result, QStringLiteral("first;literal\nsecond"));
			}

			/**
			 * @brief Keeps auto-say reevaluation ahead of separator expansion and evaluates every prefixed line.
			 */
			static void multilineAutoSayPreservesLiteralSeparators()
			{
				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("enable_aliases"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("enable_auto_say"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("re_evaluate_auto_say"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("auto_say_string"), QStringLiteral("say "));
				runtime.setWorldAttribute(QStringLiteral("enable_command_stack"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("command_stack_character"), QStringLiteral(";"));
				runtime.setAliases({recordingAlias(QStringLiteral("say first;literal")),
				                    recordingAlias(QStringLiteral("say second"))});
				WorldCommandProcessor processor;
				processor.setRuntime(&runtime);
				QCOMPARE(processor.executeCommand(QStringLiteral("first;literal\nsecond")), eOK);
				QTRY_COMPARE(runtime.aliases().at(0).matched, 1);
				QTRY_COMPARE(runtime.aliases().at(1).matched, 1);
			}

			/**
			 * @brief Waits for each asynchronous alias script before evaluating the next physical command.
			 */
			static void multilineAliasScriptsCompleteInOrder()
			{
				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("enable_aliases"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
				runtime.setWorldAttribute(QStringLiteral("enable_command_stack"), QStringLiteral("n"));
				auto first = recordingAlias(QStringLiteral("first"));
				first.attributes.insert(QStringLiteral("send_to"), QString::number(eSendToScript));
				first.children.insert(QStringLiteral("send"),
				                      QStringLiteral("SetVariable('ordered', 'first')"));
				auto second = recordingAlias(QStringLiteral("second"));
				second.attributes.insert(QStringLiteral("send_to"), QString::number(eSendToScript));
				second.children.insert(
				    QStringLiteral("send"),
				    QStringLiteral("SetVariable('ordered', GetVariable('ordered') .. ',second')"));
				runtime.setAliases({first, second});
				WorldCommandProcessor processor;
				processor.setRuntime(&runtime);
				QCOMPARE(processor.executeCommand(QStringLiteral("first\nsecond")), eOK);
				QString result;
				QTRY_VERIFY(runtime.findVariable(QStringLiteral("ordered"), result) &&
				            result == QStringLiteral("first,second"));
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

#include "tst_WorldCommandProcessor_CommandStacking.moc"
