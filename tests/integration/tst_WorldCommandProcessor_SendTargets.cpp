/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldCommandProcessor_SendTargets.cpp
 * Role: QTest coverage for WorldCommandProcessor SendTargets behavior.
 */

#include "CommandTextUtils.h"
#include "WorldOptions.h"

#include <QtTest/QTest>

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
			    << QStringLiteral("He said \"A\"\\B$")
			    << static_cast<int>(eSendToScript)
			    << QStringLiteral("lua")
			    << false
			    << QStringLiteral("He said \\\"A\\\"\\\\B$");
			QTest::newRow("perl-script")
			    << QStringLiteral("He said \"A\"\\B$")
			    << static_cast<int>(eSendToScriptAfterOmit)
			    << QStringLiteral("perlscript")
			    << false
			    << QStringLiteral("He said \\\"A\\\"\\\\B\\$");
			QTest::newRow("vbscript")
			    << QStringLiteral("He said \"A\"\\B$")
			    << static_cast<int>(eSendToScript)
			    << QStringLiteral("vbscript")
			    << false
			    << QStringLiteral("He said \"\"A\"\"\\B$");
			QTest::newRow("world-no-script-escaping")
			    << QStringLiteral("AbC\\\"$")
			    << static_cast<int>(eSendToWorld)
			    << QStringLiteral("lua")
			    << false
			    << QStringLiteral("AbC\\\"$");
			QTest::newRow("lowercase-non-script")
			    << QStringLiteral("MiXeD")
			    << static_cast<int>(eSendToWorld)
			    << QStringLiteral("lua")
			    << true
			    << QStringLiteral("mixed");
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
			QCOMPARE(QMudCommandText::fixupEscapeSequences(QStringLiteral("abc\\")),
			         QStringLiteral("abc"));
		}
	// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_WorldCommandProcessor_SendTargets)


#if __has_include("tst_WorldCommandProcessor_SendTargets.moc")
#include "tst_WorldCommandProcessor_SendTargets.moc"
#endif
