/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldRuntimeAttributeUtils.cpp
 * Role: Unit coverage for WorldRuntime attribute upsert helpers.
 */

#include "WorldRuntimeAttributeUtils.h"

#include <QtTest/QTest>

/**
 * @brief QTest fixture for multiline world-attribute upsert helper behavior.
 */
class tst_WorldRuntimeAttributeUtils : public QObject
{
		Q_OBJECT

	private slots:
		/**
		 * @brief Verifies helper inserts value when key is not present.
		 */
		static void upsertInsertsWhenMissing()
		{
			QMap<QString, QString> attrs;
			QVERIFY(upsertWorldMultilineAttributeIfChanged(attrs, QStringLiteral("script"),
			                                               QStringLiteral("print('x')")));
			QCOMPARE(attrs.value(QStringLiteral("script")), QStringLiteral("print('x')"));
		}

		/**
		 * @brief Verifies helper is a no-op when existing value is unchanged.
		 */
		static void upsertReturnsFalseWhenUnchanged()
		{
			QMap<QString, QString> attrs;
			attrs.insert(QStringLiteral("script"), QStringLiteral("print('x')"));

			QVERIFY(!upsertWorldMultilineAttributeIfChanged(attrs, QStringLiteral("script"),
			                                                QStringLiteral("print('x')")));
			QCOMPARE(attrs.value(QStringLiteral("script")), QStringLiteral("print('x')"));
		}

		/**
		 * @brief Verifies helper updates value when key exists with different value.
		 */
		static void upsertUpdatesWhenChanged()
		{
			QMap<QString, QString> attrs;
			attrs.insert(QStringLiteral("script"), QStringLiteral("print('x')"));

			QVERIFY(upsertWorldMultilineAttributeIfChanged(attrs, QStringLiteral("script"),
			                                               QStringLiteral("print('y')")));
			QCOMPARE(attrs.value(QStringLiteral("script")), QStringLiteral("print('y')"));
		}
};

QTEST_APPLESS_MAIN(tst_WorldRuntimeAttributeUtils)

#if __has_include("tst_WorldRuntimeAttributeUtils.moc")
#include "tst_WorldRuntimeAttributeUtils.moc"
#endif
