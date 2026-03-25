/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_Dialog_LogSession.cpp
 * Role: QTest coverage for Dialog LogSession behavior.
 */

#include "dialogs/LogSessionDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QSpinBox>
#include <QTextEdit>
#include <QtTest/QTest>

/**
 * @brief QTest fixture covering LogSessionDialog scenarios.
 */
class tst_Dialog_LogSession : public QObject
{
		Q_OBJECT

	// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void constructsDialogWidgets()
		{
			LogSessionDialog dialog;

			auto *lines = dialog.findChild<QSpinBox *>();
			QVERIFY(lines);
			QCOMPARE(lines->minimum(), 0);
			QCOMPARE(lines->maximum(), 500000);

			const auto checkBoxes = dialog.findChildren<QCheckBox *>();
			QCOMPARE(checkBoxes.size(), 5);

			auto *preamble = dialog.findChild<QTextEdit *>();
			QVERIFY(preamble);

			auto *buttons = dialog.findChild<QDialogButtonBox *>();
			QVERIFY(buttons);
			QVERIFY(buttons->standardButtons().testFlag(QDialogButtonBox::Ok));
			QVERIFY(buttons->standardButtons().testFlag(QDialogButtonBox::Cancel));
		}

		void roundTripsDialogState()
		{
			LogSessionDialog dialog;

			dialog.setLines(1234);
			QCOMPARE(dialog.lines(), 1234);

			dialog.setAppendToLogFile(true);
			QVERIFY(dialog.appendToLogFile());
			dialog.setAppendToLogFile(false);
			QVERIFY(!dialog.appendToLogFile());

			dialog.setWriteWorldName(true);
			QVERIFY(dialog.writeWorldName());
			dialog.setWriteWorldName(false);
			QVERIFY(!dialog.writeWorldName());

			dialog.setPreamble(QStringLiteral("test preamble"));
			QCOMPARE(dialog.preamble(), QStringLiteral("test preamble"));

			dialog.setLogOutput(true);
			QVERIFY(dialog.logOutput());
			dialog.setLogInput(true);
			QVERIFY(dialog.logInput());
			dialog.setLogNotes(true);
			QVERIFY(dialog.logNotes());
		}
	// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_MAIN(tst_Dialog_LogSession)


#if __has_include("tst_Dialog_LogSession.moc")
#include "tst_Dialog_LogSession.moc"
#endif
