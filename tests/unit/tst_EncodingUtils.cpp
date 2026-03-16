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

		void decodeUtf8KeepsSplitMultibyteCarry()
		{
			QByteArray carry;
			bool       hadInvalid = false;

			const QByteArray part1 = QByteArrayLiteral("language") + QByteArray::fromHex("E2");
			const QByteArray part2 = QByteArray::fromHex("8094") + QByteArrayLiteral("marks");

			const QString first = qmudDecodeUtf8WithWindows1252Fallback(part1, carry, &hadInvalid);
			QCOMPARE(first, QStringLiteral("language"));
			QVERIFY(!hadInvalid);
			QCOMPARE(carry, QByteArray::fromHex("E2"));

			const QString second = qmudDecodeUtf8WithWindows1252Fallback(part2, carry, &hadInvalid);
			const QString expectedSecond = QString(QChar(0x2014)) + QStringLiteral("marks");
			QCOMPARE(second, expectedSecond);
			QVERIFY(!hadInvalid);
			QVERIFY(carry.isEmpty());
		}

		void decodeUtf8FallsBackToWindows1252OnInvalidByte()
		{
			QByteArray bytes = QByteArrayLiteral("language");
			bytes.append(static_cast<char>(0x97));
			bytes.append("marks");

			QByteArray carry;
			bool       hadInvalid = false;
			const QString decoded = qmudDecodeUtf8WithWindows1252Fallback(bytes, carry, &hadInvalid);

			const QString expected = QStringLiteral("language") + QChar(0x2014) + QStringLiteral("marks");
			QCOMPARE(decoded, expected);
			QVERIFY(hadInvalid);
			QVERIFY(carry.isEmpty());
		}
	// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_EncodingUtils)

#include "tst_EncodingUtils.moc"
