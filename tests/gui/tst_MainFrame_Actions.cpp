/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_MainFrame_Actions.cpp
 * Role: QTest coverage for MainFrame Actions behavior.
 */

#include "MainFrameActionUtils.h"

#include <QtTest/QTest>

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

			QTest::newRow("first-slot") << 1 << QStringLiteral("Activates world #1 (Ctrl+1)");
			QTest::newRow("ninth-slot") << 9 << QStringLiteral("Activates world #9 (Ctrl+9)");
			QTest::newRow("tenth-slot") << 10 << QStringLiteral("Activates world #10 (Ctrl+0)");
			QTest::newRow("overflow-slot") << 12 << QStringLiteral("Activates world #12");
		}

		void worldSlotTooltip()
		{
			QFETCH(int, slot);
			QFETCH(QString, expected);
			QCOMPARE(QMudMainFrameActionUtils::worldButtonTooltipForSlot(slot), expected);
		}
	// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_MainFrame_Actions)


#if __has_include("tst_MainFrame_Actions.moc")
#include "tst_MainFrame_Actions.moc"
#endif
