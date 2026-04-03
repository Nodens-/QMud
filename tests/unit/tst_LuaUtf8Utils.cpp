/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_LuaUtf8Utils.cpp
 * Role: Unit coverage for UTF-8 argument packing helpers used by Lua callback dispatch.
 */

#include "LuaUtf8Utils.h"

#include <QtTest/QTest>

/**
 * @brief QTest fixture for Lua UTF-8 argument encoding helpers.
 */
class tst_LuaUtf8Utils : public QObject
{
		Q_OBJECT

	private slots:
		/**
		 * @brief Verifies UTF-8 conversion for ASCII and non-ASCII callback arguments.
		 */
		static void encodesAsciiAndUnicode()
		{
			const auto encoded = qmudEncodeUtf8Triplet(QStringLiteral("plain"), QStringLiteral("δοκιμή"),
			                                           QStringLiteral("emoji-\U0001F600"));
			QCOMPARE(encoded[0], QByteArray("plain"));
			QCOMPARE(encoded[1], QStringLiteral("δοκιμή").toUtf8());
			QCOMPARE(encoded[2], QStringLiteral("emoji-\U0001F600").toUtf8());
		}

		/**
		 * @brief Verifies embedded null characters are preserved in encoded payload.
		 */
		static void preservesEmbeddedNullBytes()
		{
			QString withNull;
			withNull.append(QLatin1Char('a'));
			withNull.append(QChar::Null);
			withNull.append(QLatin1Char('b'));

			const auto encoded = qmudEncodeUtf8Triplet(withNull, QStringLiteral("x"), QStringLiteral("y"));
			const QByteArray expected = withNull.toUtf8();
			QCOMPARE(encoded[0].size(), expected.size());
			QCOMPARE(encoded[0], expected);
			QVERIFY(encoded[0].contains('\0'));
		}
};

QTEST_APPLESS_MAIN(tst_LuaUtf8Utils)

#if __has_include("tst_LuaUtf8Utils.moc")
#include "tst_LuaUtf8Utils.moc"
#endif
