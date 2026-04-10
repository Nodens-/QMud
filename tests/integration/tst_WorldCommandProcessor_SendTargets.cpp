/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldCommandProcessor_SendTargets.cpp
 * Role: QTest coverage for WorldCommandProcessor SendTargets behavior.
 */

#include "CommandTextUtils.h"
#include "WorldCommandProcessor.h"
#include "WorldOptions.h"
#include "WorldRuntime.h"

#include <QtTest/QTest>

namespace
{
	WorldRuntime::Trigger makeTrigger(const QString &name)
	{
		WorldRuntime::Trigger trigger;
		trigger.attributes.insert(QStringLiteral("name"), name);
		trigger.attributes.insert(QStringLiteral("enabled"), QStringLiteral("1"));
		trigger.attributes.insert(QStringLiteral("match"), QStringLiteral("abc"));
		trigger.attributes.insert(QStringLiteral("regexp"), QStringLiteral("0"));
		trigger.attributes.insert(QStringLiteral("multi_line"), QStringLiteral("0"));
		trigger.attributes.insert(QStringLiteral("send_to"), QString::number(eSendToWorld));
		return trigger;
	}
} // namespace

/**
 * @brief QTest fixture covering WorldCommandProcessor SendTargets scenarios.
 */
class tst_WorldCommandProcessor_SendTargets : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void scriptWildcardEscaping_data()
		{
			QTest::addColumn<QString>("input");
			QTest::addColumn<int>("sendTo");
			QTest::addColumn<QString>("language");
			QTest::addColumn<bool>("lowercase");
			QTest::addColumn<QString>("expected");

			QTest::newRow("lua-script")
			    << QStringLiteral("He said \"A\"\\B$") << static_cast<int>(eSendToScript)
			    << QStringLiteral("lua") << false << QStringLiteral("He said \\\"A\\\"\\\\B$");
			QTest::newRow("perl-script")
			    << QStringLiteral("He said \"A\"\\B$") << static_cast<int>(eSendToScriptAfterOmit)
			    << QStringLiteral("perlscript") << false << QStringLiteral("He said \\\"A\\\"\\\\B\\$");
			QTest::newRow("vbscript")
			    << QStringLiteral("He said \"A\"\\B$") << static_cast<int>(eSendToScript)
			    << QStringLiteral("vbscript") << false << QStringLiteral("He said \"\"A\"\"\\B$");
			QTest::newRow("world-no-script-escaping")
			    << QStringLiteral("AbC\\\"$") << static_cast<int>(eSendToWorld) << QStringLiteral("lua")
			    << false << QStringLiteral("AbC\\\"$");
			QTest::newRow("lowercase-non-script") << QStringLiteral("MiXeD") << static_cast<int>(eSendToWorld)
			                                      << QStringLiteral("lua") << true << QStringLiteral("mixed");
		}

		void scriptWildcardEscaping()
		{
			QFETCH(QString, input);
			QFETCH(int, sendTo);
			QFETCH(QString, language);
			QFETCH(bool, lowercase);
			QFETCH(QString, expected);

			QCOMPARE(QMudCommandText::fixWildcard(input, lowercase, sendTo, language), expected);
		}

		void escapeSequenceDecoding()
		{
			const QString source   = QStringLiteral("A\\nB\\t\\x41\\q\\\\");
			const QString expected = QStringLiteral("A\nB\tAq\\");
			QCOMPARE(QMudCommandText::fixupEscapeSequences(source), expected);
		}

		void escapeSequenceTrailingBackslashDropped()
		{
			QCOMPARE(QMudCommandText::fixupEscapeSequences(QStringLiteral("abc\\")), QStringLiteral("abc"));
		}

		void triggerMatchTargetPreservesTrailingWhitespaceForRegexp()
		{
			const QString line = QStringLiteral("<274hp 930sp 448st> ");
			QCOMPARE(QMudCommandText::normalizeTriggerMatchLine(line, true), line);
		}

		void triggerMatchTargetTrimsTrailingWhitespaceForWildcard()
		{
			const QString line = QStringLiteral("<274hp 930sp 448st> \t");
			QCOMPARE(QMudCommandText::normalizeTriggerMatchLine(line, false),
			         QStringLiteral("<274hp 930sp 448st>"));
		}

		void triggerMultilineTargetPreservesTrailingWhitespaceForRegexp()
		{
			const QStringList lines = {QStringLiteral("line 1 "), QStringLiteral("line 2\t")};
			QCOMPARE(QMudCommandText::buildTriggerMultilineTarget(lines, true),
			         QStringLiteral("line 1 \nline 2\t\n"));
		}

		void triggerMultilineTargetTrimsTrailingWhitespaceForWildcard()
		{
			const QStringList lines = {QStringLiteral("line 1 "), QStringLiteral("line 2\t")};
			QCOMPARE(QMudCommandText::buildTriggerMultilineTarget(lines, false),
			         QStringLiteral("line 1\nline 2\n"));
		}

		void keepEvaluatingMatchesAgainstOriginalInputSpans()
		{
			WorldRuntime          runtime;
			WorldCommandProcessor processor;
			processor.setRuntime(&runtime);
			runtime.setCommandProcessor(&processor);
			runtime.setWorldAttribute(QStringLiteral("enable_triggers"), QStringLiteral("1"));

			WorldRuntime::Trigger styleRewrite = makeTrigger(QStringLiteral("style-rewrite"));
			styleRewrite.attributes.insert(QStringLiteral("keep_evaluating"), QStringLiteral("1"));
			styleRewrite.attributes.insert(QStringLiteral("make_bold"), QStringLiteral("1"));

			WorldRuntime::Trigger styleMatch = makeTrigger(QStringLiteral("style-match"));
			styleMatch.attributes.insert(QStringLiteral("match_bold"), QStringLiteral("1"));
			styleMatch.attributes.insert(QStringLiteral("bold"), QStringLiteral("0"));

			runtime.setTriggers({styleRewrite, styleMatch});

			WorldRuntime::StyleSpan inputSpan;
			inputSpan.length = 3;
			inputSpan.fore   = QColor(QStringLiteral("#ffffff"));
			inputSpan.back   = QColor(QStringLiteral("#000000"));
			inputSpan.bold   = false;
			inputSpan.italic = false;
			inputSpan.blink  = false;

			processor.onIncomingStyledLineReceived(QStringLiteral("abc"), {inputSpan});

			const QList<WorldRuntime::Trigger> triggers = runtime.triggers();
			QCOMPARE(triggers.size(), 2);
			QCOMPARE(triggers.at(0).matched, 1);
			QCOMPARE(triggers.at(1).matched, 1);
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_MAIN(tst_WorldCommandProcessor_SendTargets)

#if __has_include("tst_WorldCommandProcessor_SendTargets.moc")
#include "tst_WorldCommandProcessor_SendTargets.moc"
#endif
