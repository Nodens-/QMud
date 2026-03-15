/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_EncodingUtils.cpp
 * Role: QTest coverage for EncodingUtils behavior.
 */

#include "EncodingUtils.h"

#include <QtTest/QTest>

/**
 * @brief QTest fixture covering EncodingUtils scenarios.
 */
class tst_EncodingUtils : public QObject
{
		Q_OBJECT

	// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void encodeSingleLine()
		{
			QCOMPARE(qmudEncodeBase64Text(QByteArrayLiteral("Man"), false), QStringLiteral("TWFu"));
		}

		void encodeMultiLineWrapAt76()
		{
			const QByteArray input(57, 'a');
			const QString    out = qmudEncodeBase64Text(input, true);
			QCOMPARE(out.size(), 78);
			QVERIFY(out.endsWith(QStringLiteral("\r\n")));
		}

		void encodeMultiLinePartialFinalLine()
		{
			const QByteArray input(58, 'a');
			const QString    out = qmudEncodeBase64Text(input, true);
			QCOMPARE(out.count(QStringLiteral("\r\n")), 1);
			QVERIFY(!out.endsWith(QStringLiteral("\r\n")));
		}

		void nullCStringInput()
		{
			QCOMPARE(qmudEncodeBase64Text(static_cast<const char *>(nullptr), false), QString());
		}
	// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_EncodingUtils)

#include "tst_EncodingUtils.moc"

