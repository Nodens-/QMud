/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_QTestSmoke.cpp
 * Role: QTest coverage for QTestSmoke behavior.
 */

#include "TestEnvironment.h"

#include <QtTest/QTest>

/**
 * @brief QTest fixture covering QTestSmoke scenarios.
 */
class tst_QTestSmoke : public QObject
{
		Q_OBJECT

	// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void initTestCase()
		{
			QMudTest::applyDeterministicTestEnvironment();
		}

		void sanity()
		{
			QVERIFY(true);
			QCOMPARE(QStringLiteral("QMud"), QStringLiteral("QMud"));
		}
	// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_QTestSmoke)



#if __has_include("tst_QTestSmoke.moc")
#include "tst_QTestSmoke.moc"
#endif
