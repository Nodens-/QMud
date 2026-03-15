/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_DoubleMetaphone.cpp
 * Role: QTest coverage for DoubleMetaphone behavior.
 */

#include "DoubleMetaphone.h"

#include <QtTest/QTest>

/**
 * @brief QTest fixture covering DoubleMetaphone scenarios.
 */
class tst_DoubleMetaphone : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void emptyInput()
		{
			const auto result = qmudDoubleMetaphone(QString(), 8);
			QVERIFY(result.first.isEmpty());
			QVERIFY(result.second.isEmpty());
		}

		void caseInsensitiveAndDeterministic()
		{
			const auto upper = qmudDoubleMetaphone(QStringLiteral("SMITH"), 8);
			const auto mixed = qmudDoubleMetaphone(QStringLiteral("SmItH"), 8);
			const auto again = qmudDoubleMetaphone(QStringLiteral("SmItH"), 8);

			QCOMPARE(upper, mixed);
			QCOMPARE(mixed, again);
			QVERIFY(!mixed.first.isEmpty());
		}

		void lengthLimitIsRespected()
		{
			const auto result = qmudDoubleMetaphone(QStringLiteral("Washington"), 1);
			QVERIFY(result.first.size() <= 1);
			QVERIFY(result.second.size() <= 1);

			const auto zero = qmudDoubleMetaphone(QStringLiteral("Washington"), 0);
			QVERIFY(zero.first.isEmpty());
			QVERIFY(zero.second.isEmpty());
		}

		void outputCharacterSet()
		{
			const auto               result = qmudDoubleMetaphone(QStringLiteral("Schmidt"), 8);
			const QRegularExpression re(QStringLiteral("^[A-Z0-9]*$"));
			QVERIFY(re.match(result.first).hasMatch());
			QVERIFY(re.match(result.second).hasMatch());
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_DoubleMetaphone)

#include "tst_DoubleMetaphone.moc"
