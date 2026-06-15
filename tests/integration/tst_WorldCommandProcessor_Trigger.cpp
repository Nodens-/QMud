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

// ReSharper disable once CppUnusedIncludeDirective
#include <QHostAddress>
#include <QScopedPointer>
#include <QTcpServer>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTcpSocket>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

namespace
{
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
} // namespace

/**
 * @brief QTest fixture covering real WorldCommandProcessor trigger evaluation.
 */
class tst_WorldCommandProcessor_Trigger : public QObject
{
		Q_OBJECT

	private slots:
		static void triggerMatchTargetPreservesTrailingWhitespaceForRegexp()
		{
			const QString line = QStringLiteral("<274hp 930sp 448st> ");
			QCOMPARE(QMudCommandText::normalizeTriggerMatchLine(line, true), line);
		}

		static void triggerMatchTargetTrimsTrailingWhitespaceForWildcard()
		{
			const QString line = QStringLiteral("<274hp 930sp 448st> \t");
			QCOMPARE(QMudCommandText::normalizeTriggerMatchLine(line, false),
			         QStringLiteral("<274hp 930sp 448st>"));
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
			runtime.triggersMutable().push_back(makeMultilineWhitespaceTrigger());

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
			runtime.triggersMutable().push_back(makeScriptTrigger());

			WorldCommandProcessor processor;
			processor.setRuntime(&runtime);

			bool                 sawScriptSend = false;
			QVector<LuaStyleRun> capturedRuns;
			QObject::connect(&processor, &WorldCommandProcessor::sendToScriptRequested, &processor,
			                 [&](const QString &, const QString &, const QString &,
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
			QCOMPARE(capturedRuns.at(1).text, QStringLiteral("ddd"));
			QCOMPARE(capturedRuns.at(1).textColour, 0xc0c0c0);
			QCOMPARE(capturedRuns.at(1).backColour, 0x000000);
			QCOMPARE(capturedRuns.at(1).style, 0x0004);
			QCOMPARE(capturedRuns.at(2).text, QStringLiteral("fff"));
			QCOMPARE(capturedRuns.at(2).textColour, 0x0000ff);
			QCOMPARE(capturedRuns.at(2).backColour, 0x000000);
			QCOMPARE(capturedRuns.at(2).style, 0);
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
			QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() || serverAcceptedSpy.count() > 0, 5000);
			QScopedPointer<QTcpSocket> acceptedSocket(server.nextPendingConnection());
			QVERIFY(!acceptedSocket.isNull());

			processor.sendRawText(QStringLiteral("qcmd-tail-9f31"), false, true, true, false);
			QCOMPARE(queuedPayloads(processor), (QStringList{QStringLiteral("qcmd-tail-9f31")}));
			QCOMPARE(queuedTypes(processor), (QList<bool>{true}));

			runtime.setCurrentActionSource(WorldRuntime::eTriggerFired);
			processor.sendRawText(QStringLiteral("qcmd-priority-a17"), false, false, true, false);
			processor.sendRawText(QStringLiteral("qcmd-priority-b42"), false, false, true, false);
			processor.sendRawText(QStringLiteral("qcmd-priority-c83"), false, false, true, false);
			runtime.setCurrentActionSource(WorldRuntime::eUnknownActionSource);

			QCOMPARE(queuedPayloads(processor),
			         (QStringList{QStringLiteral("qcmd-priority-a17"), QStringLiteral("qcmd-priority-b42"),
			                      QStringLiteral("qcmd-priority-c83"), QStringLiteral("qcmd-tail-9f31")}));
			QCOMPARE(queuedTypes(processor), (QList<bool>{false, false, false, true}));

			processor.sendRawText(QStringLiteral("qcmd-normal-d64"), false, false, true, false);
			QCOMPARE(queuedPayloads(processor),
			         (QStringList{QStringLiteral("qcmd-priority-a17"), QStringLiteral("qcmd-priority-b42"),
			                      QStringLiteral("qcmd-priority-c83"), QStringLiteral("qcmd-tail-9f31"),
			                      QStringLiteral("qcmd-normal-d64")}));
			QCOMPARE(queuedTypes(processor), (QList<bool>{false, false, false, true, false}));

			runtime.setCurrentActionSource(WorldRuntime::eTriggerFired);
			processor.sendRawText(QStringLiteral("qcmd-priority-e05"), false, false, true, false);
			runtime.setCurrentActionSource(WorldRuntime::eUnknownActionSource);

			QCOMPARE(queuedPayloads(processor),
			         (QStringList{QStringLiteral("qcmd-priority-a17"), QStringLiteral("qcmd-priority-b42"),
			                      QStringLiteral("qcmd-priority-c83"), QStringLiteral("qcmd-priority-e05"),
			                      QStringLiteral("qcmd-tail-9f31"), QStringLiteral("qcmd-normal-d64")}));
			QCOMPARE(queuedTypes(processor), (QList<bool>{false, false, false, false, true, false}));
		}
};

QTEST_MAIN(tst_WorldCommandProcessor_Trigger)

#if __has_include("tst_WorldCommandProcessor_Trigger.moc")
#include "tst_WorldCommandProcessor_Trigger.moc"
#endif
