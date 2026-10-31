/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldCommandProcessor_Trigger.cpp
 * Role: Runtime-backed QTest coverage for WorldCommandProcessor trigger evaluation behavior.
 */

#include "WorldCommandProcessor.h"

#include "WorldCommandProcessorUtils.h"
#include "WorldOptions.h"
#include "WorldRuntimeTestAccess.h"
#include "WorldView.h"
#include "scripting/ScriptingErrors.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QCoreApplication>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFile>
// ReSharper disable once CppUnusedIncludeDirective
#include <QHostAddress>
#include <QMessageBox>
#include <QScopeGuard>
#include <QScopedPointer>
#include <QTcpServer>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTcpSocket>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

namespace
{
	/**
	 * @brief Captures and dismisses modal processor warnings so integration tests cannot block.
	 */
	class MessageBoxObserver final : public QObject
	{
		public:
			/** @brief Installs the observer on the test application. */
			MessageBoxObserver()
			{
				qApp->installEventFilter(this);
			}

			/** @brief Removes the observer from the test application. */
			~MessageBoxObserver() override
			{
				qApp->removeEventFilter(this);
			}

			/**
			 * @brief Returns the number of captured warning dialogs.
			 * @return Number of message boxes shown while the observer was installed.
			 */
			[[nodiscard]] int messageCount() const
			{
				return m_messageCount;
			}

			/**
			 * @brief Counts shown message boxes and queues their rejection.
			 * @param watched Object receiving the event.
			 * @param event Event delivered to the watched object.
			 * @return Base event-filter result.
			 */
			bool eventFilter(QObject *watched, QEvent *event) override
			{
				if (event && event->type() == QEvent::Show)
				{
					if (auto *messageBox = qobject_cast<QMessageBox *>(watched))
					{
						++m_messageCount;
						QMetaObject::invokeMethod(messageBox, &QDialog::reject, Qt::QueuedConnection);
					}
				}
				return QObject::eventFilter(watched, event);
			}

		private:
			int m_messageCount{0};
	};

	/**
	 * @brief Creates a two-line regexp trigger that captures trailing whitespace.
	 * @return Trigger configured to store captures in a world variable.
	 */
	WorldRuntime::Trigger makeMultilineWhitespaceTrigger()
	{
		WorldRuntime::Trigger trigger;
		trigger.attributes.insert(QStringLiteral("enabled"), QStringLiteral("y"));
		trigger.attributes.insert(QStringLiteral("match"), QStringLiteral("^(.*) (\\d+) (.*)\\n(.*?)\\n$"));
		trigger.attributes.insert(QStringLiteral("regexp"), QStringLiteral("y"));
		trigger.attributes.insert(QStringLiteral("multi_line"), QStringLiteral("y"));
		trigger.attributes.insert(QStringLiteral("lines_to_match"), QStringLiteral("2"));
		trigger.attributes.insert(QStringLiteral("send_to"), QString::number(eSendToVariable));
		trigger.attributes.insert(QStringLiteral("variable"), QStringLiteral("captured_multiline"));
		trigger.attributes.insert(QStringLiteral("sequence"), QStringLiteral("100"));
		trigger.children.insert(QStringLiteral("send"), QStringLiteral("%1|%2|%3|%4"));
		return trigger;
	}

	/**
	 * @brief Creates a trigger that sends matched input to script.
	 * @return Trigger configured for script send-target coverage.
	 */
	WorldRuntime::Trigger makeScriptTrigger()
	{
		WorldRuntime::Trigger trigger;
		trigger.attributes.insert(QStringLiteral("enabled"), QStringLiteral("y"));
		trigger.attributes.insert(QStringLiteral("match"), QStringLiteral("*"));
		trigger.attributes.insert(QStringLiteral("send_to"), QString::number(eSendToScript));
		trigger.attributes.insert(QStringLiteral("sequence"), QStringLiteral("100"));
		trigger.children.insert(QStringLiteral("send"), QStringLiteral("return true"));
		return trigger;
	}

	/**
	 * @brief Decodes queued command payloads for assertions.
	 * @param processor Processor whose queue should be inspected.
	 * @return Queue payloads in dispatch order.
	 */
	QStringList queuedPayloads(const WorldCommandProcessor &processor)
	{
		QStringList payloads;
		for (const QString &entry : processor.queuedCommands())
			payloads.push_back(QMudCommandQueue::decodeQueueEntry(entry).payload);
		return payloads;
	}

	/**
	 * @brief Decodes queued command delayed-queue flags for assertions.
	 * @param processor Processor whose queue should be inspected.
	 * @return Queue delayed flags in dispatch order.
	 */
	QList<bool> queuedTypes(const WorldCommandProcessor &processor)
	{
		QList<bool> queuedTypes;
		for (const QString &entry : processor.queuedCommands())
			queuedTypes.push_back(QMudCommandQueue::decodeQueueEntry(entry).queuedType);
		return queuedTypes;
	}

	/**
	 * @brief QTest fixture covering real WorldCommandProcessor trigger evaluation.
	 */
	class tst_WorldCommandProcessor_Trigger : public QObject
	{
			Q_OBJECT

		private slots:
			/**
			 * @brief Verifies static trigger validity remains lazy and invalid execution stays warning-coalesced.
			 */
			static void staticRegexValidityIsLazyAndCached()
			{
				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("enable_triggers"), QStringLiteral("y"));

				WorldRuntime::Trigger trigger;
				trigger.attributes.insert(QStringLiteral("enabled"), QStringLiteral("n"));
				trigger.attributes.insert(QStringLiteral("match"), QStringLiteral("("));
				trigger.attributes.insert(QStringLiteral("regexp"), QStringLiteral("y"));
				trigger.attributes.insert(QStringLiteral("sequence"), QStringLiteral("100"));
				runtime.setTriggers({trigger});

				WorldCommandProcessor processor;
				processor.setRuntime(&runtime);
				MessageBoxObserver observer;

				processor.onIncomingLineReceived(QStringLiteral("disabled"));
				QCOMPARE(observer.messageCount(), 0);
				QCOMPARE(runtime.triggers().constFirst().matchAttempts, 0);

				WorldRuntime::Trigger &storedTrigger = WorldRuntimeTestAccess::triggers(runtime).first();
				storedTrigger.attributes.insert(QStringLiteral("enabled"), QStringLiteral("y"));
				runtime.markTriggersChanged();

				processor.onIncomingLineReceived(QStringLiteral("invalid"));
				QCOMPARE(observer.messageCount(), 1);
				QCOMPARE(runtime.triggers().constFirst().matchAttempts, 0);
				processor.onIncomingLineReceived(QStringLiteral("invalid-again"));
				QCOMPARE(observer.messageCount(), 1);
				QCOMPARE(runtime.triggers().constFirst().matchAttempts, 0);

				storedTrigger.attributes.insert(QStringLiteral("match"), QStringLiteral("^valid$"));
				runtime.markTriggersChanged();

				processor.onIncomingLineReceived(QStringLiteral("miss"));
				QCOMPARE(runtime.triggers().constFirst().matchAttempts, 1);
				processor.onIncomingLineReceived(QStringLiteral("valid"));
				QCOMPARE(runtime.triggers().constFirst().matchAttempts, 2);
				QCOMPARE(runtime.triggers().constFirst().matchCount, 1);
				QCOMPARE(runtime.triggers().constFirst().matched, 1);
			}

			/**
			 * @brief Verifies trigger output notes and matched world output use their own log options.
			 */
			static void sendToOutputLoggingUsesIndependentWorldAndNoteOptions()
			{
				const QString rootPath =
				    QDir(QCoreApplication::applicationDirPath())
				        .filePath(QStringLiteral("test-artifacts/tst_WorldCommandProcessor_Trigger/"
				                                 "send-to-output-log-options"));
				if (QDir(rootPath).exists())
					QVERIFY(QDir(rootPath).removeRecursively());
				QVERIFY(QDir().mkpath(QDir(rootPath).filePath(QStringLiteral("logs"))));
				[[maybe_unused]] const auto cleanup =
				    qScopeGuard([rootPath] { static_cast<void>(QDir(rootPath).removeRecursively()); });

				auto runCase = [&rootPath](const QString &name, const bool logOutput, const bool logNotes)
				{
					WorldRuntime runtime;
					runtime.setStartupDirectory(rootPath);
					runtime.setDefaultLogDirectory(QDir(rootPath).filePath(QStringLiteral("logs")));
					runtime.setWorldAttribute(QStringLiteral("enable_triggers"), QStringLiteral("1"));
					runtime.setWorldAttribute(QStringLiteral("log_output"),
					                          logOutput ? QStringLiteral("1") : QStringLiteral("0"));
					runtime.setWorldAttribute(QStringLiteral("log_notes"),
					                          logNotes ? QStringLiteral("1") : QStringLiteral("0"));
					runtime.setWorldAttribute(QStringLiteral("log_html"), QStringLiteral("0"));
					runtime.setWorldAttribute(QStringLiteral("log_raw"), QStringLiteral("0"));
					runtime.setWorldAttribute(QStringLiteral("log_rotate_gzip"), QStringLiteral("0"));

					WorldRuntime::Trigger trigger;
					trigger.attributes.insert(QStringLiteral("enabled"), QStringLiteral("1"));
					trigger.attributes.insert(QStringLiteral("match"), QStringLiteral("source-line"));
					trigger.attributes.insert(QStringLiteral("send_to"), QString::number(eSendToOutput));
					trigger.children.insert(QStringLiteral("send"), QStringLiteral("generated-note"));
					WorldRuntimeTestAccess::triggers(runtime).push_back(std::move(trigger));
					runtime.markTriggersChanged();

					WorldView             view;
					WorldCommandProcessor processor;
					processor.setView(&view);
					processor.setRuntime(&runtime);
					runtime.setCommandProcessor(&processor);
					view.setRuntime(&runtime);

					const QString path = QDir(rootPath).filePath(QStringLiteral("logs/%1.txt").arg(name));
					QCOMPARE(runtime.openLog(path, false), eOK);
					const QString           openedPath = runtime.logFileName();
					WorldRuntime::StyleSpan span;
					span.length = 11;
					processor.onIncomingStyledLineReceived(QStringLiteral("source-line"), {span});
					QCOMPARE(runtime.closeLog(), eOK);

					QFile logFile(openedPath);
					QVERIFY(logFile.open(QIODevice::ReadOnly));
					const QString contents = QString::fromLocal8Bit(logFile.readAll());
					QCOMPARE(contents.contains(QStringLiteral("source-line")), logOutput);
					QCOMPARE(contents.contains(QStringLiteral("generated-note")), logNotes);

					view.setRuntime(nullptr);
					runtime.setCommandProcessor(nullptr);
					processor.setRuntime(nullptr);
				};

				runCase(QStringLiteral("world-only"), true, false);
				runCase(QStringLiteral("notes-only"), false, true);
			}

			/**
			 * @brief Verifies live styled output uses HTML colours without changing the disabled-colour path.
			 */
			static void liveHtmlColourLoggingUsesOutputSpansAndPreservesDisabledBehaviour()
			{
				const QString rootPath = QDir(QCoreApplication::applicationDirPath())
				                             .filePath(QStringLiteral("test-artifacts/"
				                                                      "tst_WorldCommandProcessor_Trigger/"
				                                                      "html-colour-logging"));
				if (QDir(rootPath).exists())
					QVERIFY(QDir(rootPath).removeRecursively());
				QVERIFY(QDir().mkpath(QDir(rootPath).filePath(QStringLiteral("logs"))));
				[[maybe_unused]] const auto cleanup =
				    qScopeGuard([rootPath] { static_cast<void>(QDir(rootPath).removeRecursively()); });

				WorldRuntime runtime;
				runtime.setStartupDirectory(rootPath);
				runtime.setDefaultLogDirectory(QDir(rootPath).filePath(QStringLiteral("logs")));
				runtime.setWorldAttribute(QStringLiteral("enable_triggers"), QStringLiteral("0"));
				runtime.setWorldAttribute(QStringLiteral("log_output"), QStringLiteral("1"));
				runtime.setWorldAttribute(QStringLiteral("log_html"), QStringLiteral("1"));
				runtime.setWorldAttribute(QStringLiteral("log_in_colour"), QStringLiteral("1"));
				runtime.setWorldAttribute(QStringLiteral("log_raw"), QStringLiteral("0"));
				runtime.setWorldAttribute(QStringLiteral("log_rotate_gzip"), QStringLiteral("0"));
				runtime.setWorldAttribute(QStringLiteral("alternative_inverse"), QStringLiteral("0"));

				WorldCommandProcessor processor;
				processor.setRuntime(&runtime);

				WorldRuntime::StyleSpan first;
				first.length    = 5;
				first.fore      = QColor(QStringLiteral("#ff0000"));
				first.back      = QColor(QStringLiteral("#0000ff"));
				first.underline = true;
				WorldRuntime::StyleSpan second;
				second.length      = 6;
				second.fore        = QColor(QStringLiteral("#00ff00"));
				second.back        = QColor(QStringLiteral("#000000"));
				second.inverse     = true;
				const QString line = QStringLiteral("red<&invert");

				const QString colouredPath = QDir(rootPath).filePath(QStringLiteral("logs/coloured.html"));
				QCOMPARE(runtime.openLog(colouredPath, false), eOK);
				const QString openedColouredPath = runtime.logFileName();
				processor.onIncomingStyledLineReceived(line, {first, second});

				WorldRuntime::Trigger trigger;
				trigger.attributes.insert(QStringLiteral("enabled"), QStringLiteral("1"));
				trigger.attributes.insert(QStringLiteral("match"), QStringLiteral("*"));
				trigger.attributes.insert(QStringLiteral("send_to"), QString::number(eSendToOutput));
				trigger.attributes.insert(QStringLiteral("custom_colour"), QString::number(OTHER_CUSTOM + 1));
				trigger.attributes.insert(QStringLiteral("colour_change_type"),
				                          QString::number(TRIGGER_COLOUR_CHANGE_BOTH));
				trigger.attributes.insert(QStringLiteral("other_text_colour"), QStringLiteral("#112233"));
				trigger.attributes.insert(QStringLiteral("other_back_colour"), QStringLiteral("#445566"));
				WorldRuntimeTestAccess::triggers(runtime).push_back(trigger);
				runtime.markTriggersChanged();
				runtime.setWorldAttribute(QStringLiteral("enable_triggers"), QStringLiteral("1"));
				WorldRuntime::StyleSpan triggerInput;
				triggerInput.length = 9;
				triggerInput.fore   = QColor(QStringLiteral("#ffffff"));
				triggerInput.back   = QColor(QStringLiteral("#000000"));
				processor.onIncomingStyledLineReceived(QStringLiteral("triggered"), {triggerInput});
				QCOMPARE(runtime.closeLog(), eOK);
				QFile colouredFile(openedColouredPath);
				QVERIFY(colouredFile.open(QIODevice::ReadOnly));
				QCOMPARE(QString::fromLocal8Bit(colouredFile.readAll()),
				         QStringLiteral("<font color=\"#ff0000\"><span style=\"color: #ff0000; "
				                        "background: #0000ff\"><u>red&lt;&amp;</u></span></font>"
				                        "<font color=\"#000000\"><span style=\"color: #000000; "
				                        "background: #00ff00\">invert</span></font>\n"
				                        "<font color=\"#112233\"><span style=\"color: #112233; "
				                        "background: #445566\">triggered</span></font>\n"));

				runtime.setWorldAttribute(QStringLiteral("log_in_colour"), QStringLiteral("0"));
				const QString plainPath = QDir(rootPath).filePath(QStringLiteral("logs/plain.html"));
				QCOMPARE(runtime.openLog(plainPath, false), eOK);
				const QString openedPlainPath = runtime.logFileName();
				processor.onIncomingStyledLineReceived(line, {first, second});
				QCOMPARE(runtime.closeLog(), eOK);
				QFile plainFile(openedPlainPath);
				QVERIFY(plainFile.open(QIODevice::ReadOnly));
				QCOMPARE(QString::fromLocal8Bit(plainFile.readAll()), QStringLiteral("red&lt;&amp;invert\n"));
			}

			static void triggerMatchTargetPreservesTrailingWhitespaceForRegexp()
			{
				const QString line = QStringLiteral("<100aa 200bb 300cc> ");
				QCOMPARE(QMudCommandText::normalizeTriggerMatchLine(line, true), line);
			}

			static void triggerMatchTargetTrimsTrailingWhitespaceForWildcard()
			{
				const QString line = QStringLiteral("<100aa 200bb 300cc> \t");
				QCOMPARE(QMudCommandText::normalizeTriggerMatchLine(line, false),
				         QStringLiteral("<100aa 200bb 300cc>"));
			}

			static void triggerMultilineTargetPreservesTrailingWhitespaceForRegexp()
			{
				const QStringList lines = {QStringLiteral("line 1 "), QStringLiteral("line 2\t")};
				QCOMPARE(QMudCommandText::buildTriggerMultilineTarget(lines, true),
				         QStringLiteral("line 1 \nline 2\t\n"));
			}

			static void triggerMultilineTargetTrimsTrailingWhitespaceForWildcard()
			{
				const QStringList lines = {QStringLiteral("line 1 "), QStringLiteral("line 2\t")};
				QCOMPARE(QMudCommandText::buildTriggerMultilineTarget(lines, false),
				         QStringLiteral("line 1\nline 2\n"));
			}

			static void pluginTriggerSoundBypassesWorldToggle()
			{
				QVERIFY(QMudTriggerSound::shouldPlayTriggerSound(true, false));
				QVERIFY(QMudTriggerSound::shouldPlayTriggerSound(true, true));
			}

			static void worldTriggerSoundFollowsWorldToggle()
			{
				QVERIFY(!QMudTriggerSound::shouldPlayTriggerSound(false, false));
				QVERIFY(QMudTriggerSound::shouldPlayTriggerSound(false, true));
			}

			static void multilineRegexpTriggerPreservesTrailingWhitespaceInProcessor()
			{
				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("enable_triggers"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("enable_trigger_sounds"), QStringLiteral("n"));
				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
				WorldRuntimeTestAccess::triggers(runtime).push_back(makeMultilineWhitespaceTrigger());

				WorldCommandProcessor processor;
				processor.setRuntime(&runtime);
				processor.onIncomingLineReceived(QStringLiteral("Multi Line:  1 Trigger "));
				processor.onIncomingLineReceived(QStringLiteral("Expires on Sat Jul  4 18:08:48 2026."));

				QString captured;
				QVERIFY(runtime.findVariable(QStringLiteral("captured_multiline"), captured));
				QCOMPARE(captured,
				         QStringLiteral("Multi Line: |1|Trigger |Expires on Sat Jul  4 18:08:48 2026."));
				QCOMPARE(runtime.triggers().constFirst().matched, 1);
			}

			static void triggerStyleRunsUseMushclientPaneStyleProjection()
			{
				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("enable_triggers"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("enable_trigger_sounds"), QStringLiteral("n"));
				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
				WorldRuntimeTestAccess::triggers(runtime).push_back(makeScriptTrigger());

				WorldCommandProcessor processor;
				processor.setRuntime(&runtime);

				bool                 sawScriptSend = false;
				QVector<LuaStyleRun> capturedRuns;
				QObject::connect(&processor, &WorldCommandProcessor::sendToScriptRequested, &processor,
				                 [&](const QString &, const QString &, const QString &, LuaCallbackEvent,
				                     const QVector<LuaStyleRun> *styleRuns, bool, bool, int, qint64)
				                 {
					                 sawScriptSend = true;
					                 if (styleRuns)
						                 capturedRuns = *styleRuns;
				                 });

				WorldRuntime::StyleSpan implicitDefault;
				implicitDefault.length = 3;

				WorldRuntime::StyleSpan explicitDefault;
				explicitDefault.length     = 3;
				explicitDefault.fore       = QColor(QStringLiteral("#c0c0c0"));
				explicitDefault.back       = QColor(QStringLiteral("#000000"));
				explicitDefault.actionType = WorldRuntime::ActionSend;
				explicitDefault.action     = QStringLiteral("look");
				explicitDefault.hint       = QStringLiteral("Look");
				explicitDefault.startTag   = true;

				WorldRuntime::StyleSpan inverseEquivalent;
				inverseEquivalent.length  = 3;
				inverseEquivalent.fore    = QColor(QStringLiteral("#000000"));
				inverseEquivalent.back    = QColor(QStringLiteral("#c0c0c0"));
				inverseEquivalent.inverse = true;

				WorldRuntime::StyleSpan italicSplit = explicitDefault;
				italicSplit.length                  = 3;
				italicSplit.actionType              = WorldRuntime::ActionNone;
				italicSplit.action.clear();
				italicSplit.hint.clear();
				italicSplit.startTag = false;
				italicSplit.italic   = true;

				WorldRuntime::StyleSpan visibleSplit = explicitDefault;
				visibleSplit.length                  = 3;
				visibleSplit.fore                    = QColor(QStringLiteral("#ff0000"));
				visibleSplit.actionType              = WorldRuntime::ActionNone;
				visibleSplit.action.clear();
				visibleSplit.hint.clear();
				visibleSplit.startTag = false;

				processor.onIncomingStyledLineReceived(
				    QStringLiteral("aaabbbcccdddfff"),
				    {implicitDefault, explicitDefault, inverseEquivalent, italicSplit, visibleSplit});

				QVERIFY(sawScriptSend);
				QCOMPARE(capturedRuns.size(), 3);
				QCOMPARE(capturedRuns.at(0).text, QStringLiteral("aaabbbccc"));
				QCOMPARE(capturedRuns.at(0).textColour, 0xc0c0c0);
				QCOMPARE(capturedRuns.at(0).backColour, 0x000000);
				QCOMPARE(capturedRuns.at(0).style, 0);
				QCOMPARE(capturedRuns.at(0).sourceSpans.size(), 3);
				const WorldRuntime::StyleSpan firstSourceSpan =
				    QMudLuaCallbackLineSnapshot::toStyleSpan(capturedRuns.at(0).sourceSpans.at(0));
				QCOMPARE(firstSourceSpan.fore, QColor(QStringLiteral("#c0c0c0")));
				QCOMPARE(firstSourceSpan.back, QColor(QStringLiteral("#000000")));
				QCOMPARE(capturedRuns.at(0).sourceSpans.at(1).actionType, WorldRuntime::ActionSend);
				QCOMPARE(capturedRuns.at(0).sourceSpans.at(1).action, QStringLiteral("look"));
				QCOMPARE(capturedRuns.at(0).sourceSpans.at(1).hint, QStringLiteral("Look"));
				QVERIFY(capturedRuns.at(0).sourceSpans.at(1).startTag);
				QVERIFY(capturedRuns.at(0).sourceSpans.at(2).inverse);
				const WorldRuntime::StyleSpan inverseSourceSpan =
				    QMudLuaCallbackLineSnapshot::toStyleSpan(capturedRuns.at(0).sourceSpans.at(2));
				QCOMPARE(inverseSourceSpan.fore, QColor(QStringLiteral("#c0c0c0")));
				QCOMPARE(inverseSourceSpan.back, QColor(QStringLiteral("#000000")));
				QCOMPARE(capturedRuns.at(1).text, QStringLiteral("ddd"));
				QCOMPARE(capturedRuns.at(1).textColour, 0xc0c0c0);
				QCOMPARE(capturedRuns.at(1).backColour, 0x000000);
				QCOMPARE(capturedRuns.at(1).style, 0x0004);
				QCOMPARE(capturedRuns.at(1).sourceSpans.size(), 1);
				QVERIFY(capturedRuns.at(1).sourceSpans.constFirst().italic);
				QCOMPARE(capturedRuns.at(2).text, QStringLiteral("fff"));
				QCOMPARE(capturedRuns.at(2).textColour, 0x0000ff);
				QCOMPARE(capturedRuns.at(2).backColour, 0x000000);
				QCOMPARE(capturedRuns.at(2).style, 0);
			}

			static void triggerStyleRunsNormalizeMalformedSpanCoverage()
			{
				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("enable_triggers"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("enable_trigger_sounds"), QStringLiteral("n"));
				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
				WorldRuntimeTestAccess::triggers(runtime).push_back(makeScriptTrigger());

				WorldCommandProcessor processor;
				processor.setRuntime(&runtime);
				QVector<LuaStyleRun> capturedRuns;
				QObject::connect(&processor, &WorldCommandProcessor::sendToScriptRequested, &processor,
				                 [&](const QString &, const QString &, const QString &, LuaCallbackEvent,
				                     const QVector<LuaStyleRun> *styleRuns, bool, bool, int, qint64)
				                 {
					                 if (styleRuns)
						                 capturedRuns = *styleRuns;
				                 });

				WorldRuntime::StyleSpan partial;
				partial.length = 2;
				partial.fore   = QColor(QStringLiteral("#ff0000"));
				partial.back   = QColor(QStringLiteral("#000000"));
				processor.onIncomingStyledLineReceived(QStringLiteral("abcdef"), {partial});
				QCOMPARE(capturedRuns.size(), 2);
				QCOMPARE(capturedRuns.at(0).text, QStringLiteral("ab"));
				QCOMPARE(capturedRuns.at(1).text, QStringLiteral("cdef"));
				QCOMPARE(capturedRuns.at(1).sourceSpans.size(), 1);
				QCOMPARE(capturedRuns.at(1).sourceSpans.constFirst().length, 4);

				WorldRuntime::StyleSpan overlong = partial;
				overlong.length                  = 100;
				capturedRuns.clear();
				processor.onIncomingStyledLineReceived(QStringLiteral("xyz"), {overlong});
				QCOMPARE(capturedRuns.size(), 1);
				QCOMPARE(capturedRuns.constFirst().text, QStringLiteral("xyz"));
				QCOMPARE(capturedRuns.constFirst().sourceSpans.size(), 1);
				QCOMPARE(capturedRuns.constFirst().sourceSpans.constFirst().length, 3);
			}

			static void repeatedTriggerSeparatesInitialInfoFromFinalRegexCaptures()
			{
				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("enable_triggers"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("enable_trigger_sounds"), QStringLiteral("n"));
				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));

				WorldRuntime::Trigger trigger;
				trigger.attributes.insert(QStringLiteral("name"), QStringLiteral("repeated_capture"));
				trigger.attributes.insert(QStringLiteral("enabled"), QStringLiteral("y"));
				trigger.attributes.insert(QStringLiteral("match"), QStringLiteral("(?<word>[A-Z]+)"));
				trigger.attributes.insert(QStringLiteral("regexp"), QStringLiteral("y"));
				trigger.attributes.insert(QStringLiteral("repeat"), QStringLiteral("y"));
				trigger.attributes.insert(QStringLiteral("lowercase_wildcard"), QStringLiteral("y"));
				trigger.attributes.insert(QStringLiteral("make_bold"), QStringLiteral("y"));
				trigger.attributes.insert(QStringLiteral("sequence"), QStringLiteral("100"));
				runtime.setTriggers({trigger});

				WorldCommandProcessor processor;
				processor.setRuntime(&runtime);
				processor.onIncomingLineReceived(QStringLiteral("ONE TWO"));

				const WorldRuntime::Trigger &stored = runtime.triggers().constFirst();
				QCOMPARE(stored.matched, 1);
				QCOMPARE(stored.matchCount, 2);
				QCOMPARE(stored.matchAttempts, 3);
				QCOMPARE(stored.lastMatchTarget, QStringLiteral("ONE TWO"));
				QCOMPARE(stored.lastMatchWildcards,
				         (QStringList{QStringLiteral("one"), QStringLiteral("one")}));

				QString wildcard;
				QVERIFY(runtime.triggerWildcard(QStringLiteral("repeated_capture"), QStringLiteral("0"),
				                                wildcard));
				QCOMPARE(wildcard, QStringLiteral("TWO"));
				QVERIFY(runtime.triggerWildcard(QStringLiteral("repeated_capture"), QStringLiteral("word"),
				                                wildcard));
				QCOMPARE(wildcard, QStringLiteral("TWO"));
			}

			static void pluginReorderDuringTriggerEvaluationUsesStableFallback()
			{
				const QString firstPluginId  = QStringLiteral("111111111111111111111111");
				const QString secondPluginId = QStringLiteral("222222222222222222222222");
				auto makeTrigger = [](const QString &name, const QString &sendText, const int sequence)
				{
					WorldRuntime::Trigger trigger = makeScriptTrigger();
					trigger.attributes.insert(QStringLiteral("name"), name);
					trigger.attributes.insert(QStringLiteral("sequence"), QString::number(sequence));
					trigger.attributes.insert(QStringLiteral("keep_evaluating"), QStringLiteral("y"));
					trigger.children.insert(QStringLiteral("send"), sendText);
					return trigger;
				};

				WorldRuntime::Plugin firstPlugin;
				firstPlugin.attributes.insert(QStringLiteral("id"), firstPluginId);
				firstPlugin.attributes.insert(QStringLiteral("name"), QStringLiteral("First"));
				firstPlugin.enabled  = true;
				firstPlugin.sequence = 100;
				firstPlugin.triggers = {
				    makeTrigger(QStringLiteral("first"), QStringLiteral("first-send"), 100),
				    makeTrigger(QStringLiteral("second"), QStringLiteral("second-send"), 200)};
				firstPlugin.triggers[0].runtimeId = 42;
				firstPlugin.triggers[1].runtimeId = 42;

				WorldRuntime::Plugin secondPlugin;
				secondPlugin.attributes.insert(QStringLiteral("id"), secondPluginId);
				secondPlugin.attributes.insert(QStringLiteral("name"), QStringLiteral("Second"));
				secondPlugin.enabled  = true;
				secondPlugin.sequence = 200;

				WorldRuntime runtime;
				WorldRuntimeTestAccess::plugins(runtime) = {firstPlugin, secondPlugin};
				WorldRuntimeTestAccess::commitPluginStructure(runtime);
				const QList<WorldRuntime::Trigger> &runtimeTriggers =
				    WorldRuntimeTestAccess::plugins(runtime).front().triggers;
				QVERIFY(runtimeTriggers.at(0).runtimeId != 0);
				QVERIFY(runtimeTriggers.at(1).runtimeId != 0);
				QVERIFY(runtimeTriggers.at(0).runtimeId != runtimeTriggers.at(1).runtimeId);

				WorldCommandProcessor processor;
				processor.setRuntime(&runtime);

				QStringList sends;
				bool        reordered        = false;
				bool        reorderSucceeded = false;
				QObject::connect(&processor, &WorldCommandProcessor::sendToScriptRequested, &processor,
				                 [&](const QString &pluginId, const QString &scriptText, const QString &,
				                     LuaCallbackEvent, const QVector<LuaStyleRun> *, bool, bool, int, qint64)
				                 {
					                 if (pluginId != firstPluginId)
						                 return;
					                 sends.push_back(scriptText);
					                 if (!reordered)
					                 {
						                 reordered        = true;
						                 reorderSucceeded = runtime.reorderPlugin(firstPluginId, 1);
					                 }
				                 });

				const quint64 generationBefore = runtime.triggerRuleGeneration();
				processor.onIncomingLineReceived(QStringLiteral("line"));

				QVERIFY(reorderSucceeded);
				QVERIFY(runtime.triggerRuleGeneration() > generationBefore);
				QCOMPARE(sends, (QStringList{QStringLiteral("first-send"), QStringLiteral("second-send")}));
			}

			static void triggerSendsInsertAtPriorityQueueBoundary()
			{
				QTcpServer server;
				if (!server.listen(QHostAddress::LocalHost, 0))
					QSKIP("Local TCP listen is unavailable in this environment.");

				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("speed_walk_delay"), QStringLiteral("1000"));

				WorldCommandProcessor processor;
				processor.setRuntime(&runtime);

				QSignalSpy connectedSpy(&runtime, &WorldRuntime::connected);
				QVERIFY(connectedSpy.isValid());
				QSignalSpy serverAcceptedSpy(&server, &QTcpServer::newConnection);
				QVERIFY(serverAcceptedSpy.isValid());

				QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
				QVERIFY(connectedSpy.wait(5000));
				QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() || serverAcceptedSpy.count() > 0,
				                         5000);
				QScopedPointer<QTcpSocket> acceptedSocket(server.nextPendingConnection());
				QVERIFY(!acceptedSocket.isNull());

				processor.sendRawText(QStringLiteral("qcmd-tail-9f31"), false, true, true, false);
				QCOMPARE(queuedPayloads(processor), (QStringList{QStringLiteral("qcmd-tail-9f31")}));
				QCOMPARE(queuedTypes(processor), (QList<bool>{true}));

				processor.sendRawTextWithTriggerPriority(QStringLiteral("qcmd-priority-a17"), false, false,
				                                         true, false);
				processor.sendRawTextWithTriggerPriority(QStringLiteral("qcmd-priority-b42"), false, false,
				                                         true, false);
				processor.sendRawTextWithTriggerPriority(QStringLiteral("qcmd-priority-c83"), false, false,
				                                         true, false);

				QCOMPARE(
				    queuedPayloads(processor),
				    (QStringList{QStringLiteral("qcmd-priority-a17"), QStringLiteral("qcmd-priority-b42"),
				                 QStringLiteral("qcmd-priority-c83"), QStringLiteral("qcmd-tail-9f31")}));
				QCOMPARE(queuedTypes(processor), (QList<bool>{false, false, false, true}));

				processor.sendRawText(QStringLiteral("qcmd-normal-d64"), false, false, true, false);
				QCOMPARE(
				    queuedPayloads(processor),
				    (QStringList{QStringLiteral("qcmd-priority-a17"), QStringLiteral("qcmd-priority-b42"),
				                 QStringLiteral("qcmd-priority-c83"), QStringLiteral("qcmd-tail-9f31"),
				                 QStringLiteral("qcmd-normal-d64")}));
				QCOMPARE(queuedTypes(processor), (QList<bool>{false, false, false, true, false}));

				processor.sendRawTextWithTriggerPriority(QStringLiteral("qcmd-priority-e05"), false, false,
				                                         true, false);

				QCOMPARE(
				    queuedPayloads(processor),
				    (QStringList{QStringLiteral("qcmd-priority-a17"), QStringLiteral("qcmd-priority-b42"),
				                 QStringLiteral("qcmd-priority-c83"), QStringLiteral("qcmd-priority-e05"),
				                 QStringLiteral("qcmd-tail-9f31"), QStringLiteral("qcmd-normal-d64")}));
				QCOMPARE(queuedTypes(processor), (QList<bool>{false, false, false, false, true, false}));
			}

			static void fragmentedWorldTriggerEchoFollowsFinalizedLine()
			{
				QTcpServer server;
				if (!server.listen(QHostAddress::LocalHost, 0))
					QSKIP("Local TCP listen is unavailable in this environment.");

				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("display_my_input"), QStringLiteral("1"));
				runtime.setWorldAttribute(QStringLiteral("enable_triggers"), QStringLiteral("1"));
				runtime.setWorldAttribute(QStringLiteral("echo_force_terminates_partial_prompts"),
				                          QStringLiteral("1"));

				WorldRuntime::Trigger trigger;
				trigger.attributes.insert(QStringLiteral("enabled"), QStringLiteral("1"));
				trigger.attributes.insert(QStringLiteral("match"), QStringLiteral("^fragmented line$"));
				trigger.attributes.insert(QStringLiteral("regexp"), QStringLiteral("1"));
				trigger.attributes.insert(QStringLiteral("send_to"), QString::number(eSendToWorld));
				trigger.attributes.insert(QStringLiteral("sequence"), QStringLiteral("100"));
				trigger.children.insert(QStringLiteral("send"), QStringLiteral("trigger command"));
				WorldRuntimeTestAccess::triggers(runtime).push_back(trigger);
				runtime.markTriggersChanged();

				WorldView             view;
				WorldCommandProcessor processor;
				view.setRuntime(&runtime);
				view.applyRuntimeSettings();
				processor.setView(&view);
				processor.setRuntime(&runtime);
				runtime.setCommandProcessor(&processor);
				const auto detachRuntime = qScopeGuard(
				    [&runtime, &view, &processor]
				    {
					    view.setRuntime(nullptr);
					    runtime.setCommandProcessor(nullptr);
					    processor.setRuntime(nullptr);
				    });
				QObject::connect(&runtime, &WorldRuntime::incomingStyledLineReceived, &processor,
				                 &WorldCommandProcessor::onIncomingStyledLineReceived);
				QObject::connect(&runtime, &WorldRuntime::incomingStyledLinePartialReceived, &processor,
				                 &WorldCommandProcessor::onIncomingStyledLinePartialReceived);

				QSignalSpy connectedSpy(&runtime, &WorldRuntime::connected);
				QVERIFY(connectedSpy.isValid());
				QSignalSpy serverAcceptedSpy(&server, &QTcpServer::newConnection);
				QVERIFY(serverAcceptedSpy.isValid());
				QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
				QVERIFY(connectedSpy.wait(5000));
				QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() || serverAcceptedSpy.count() > 0,
				                         5000);
				QScopedPointer<QTcpSocket> acceptedSocket(server.nextPendingConnection());
				QVERIFY(!acceptedSocket.isNull());

				runtime.receiveRawData(QByteArrayLiteral("fragmented "));
				QTRY_COMPARE(view.outputLines().constLast(), QStringLiteral("fragmented "));
				runtime.receiveRawData(QByteArrayLiteral("line\n"));

				QTRY_COMPARE(runtime.triggers().constFirst().matched, 1);
				QCOMPARE(runtime.lines().size(), qsizetype{2});
				QCOMPARE(runtime.lines().at(0).text, QStringLiteral("fragmented line"));
				QCOMPARE(runtime.lines().at(1).text, QStringLiteral("trigger command"));
				QCOMPARE(view.outputLines(),
				         QStringList({QStringLiteral("fragmented line"), QStringLiteral("trigger command")}));

				QByteArray received;
				auto       receivedTriggerCommand = [&acceptedSocket, &received]
				{
					if (acceptedSocket->bytesAvailable() == 0)
						acceptedSocket->waitForReadyRead(10);
					received += acceptedSocket->readAll();
					return received.contains("trigger command\r\n");
				};
				QTRY_VERIFY_WITH_TIMEOUT(receivedTriggerCommand(), 5000);
			}

			static void userMacroCommandSuppressesAutoSayDuringEvaluation()
			{
				QTcpServer server;
				if (!server.listen(QHostAddress::LocalHost, 0))
					QSKIP("Local TCP listen is unavailable in this environment.");

				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("enable_auto_say"), QStringLiteral("1"));
				runtime.setWorldAttribute(QStringLiteral("auto_say_string"), QStringLiteral("say "));
				runtime.setCurrentActionSource(WorldRuntime::eUserMacro);

				WorldCommandProcessor processor;
				processor.setRuntime(&runtime);

				QSignalSpy connectedSpy(&runtime, &WorldRuntime::connected);
				QVERIFY(connectedSpy.isValid());
				QSignalSpy serverAcceptedSpy(&server, &QTcpServer::newConnection);
				QVERIFY(serverAcceptedSpy.isValid());

				QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
				QVERIFY(connectedSpy.wait(5000));
				QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() || serverAcceptedSpy.count() > 0,
				                         5000);
				QScopedPointer<QTcpSocket> acceptedSocket(server.nextPendingConnection());
				QVERIFY(!acceptedSocket.isNull());

				QCOMPARE(processor.executeUserMacroSendNow(QStringLiteral("hello"), false), eOK);
				QByteArray received;
				auto       receivedMacroCommand = [&acceptedSocket, &received]
				{
					if (acceptedSocket->bytesAvailable() == 0)
						acceptedSocket->waitForReadyRead(10);
					received += acceptedSocket->readAll();
					return received.contains("hello\r\n");
				};
				QTRY_VERIFY_WITH_TIMEOUT(receivedMacroCommand(), 5000);
				QVERIFY(!received.contains("say hello\r\n"));
			}
	};
} // namespace

QTEST_MAIN(tst_WorldCommandProcessor_Trigger)

#include "tst_WorldCommandProcessor_Trigger.moc"
