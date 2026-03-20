/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldOptionDefaults.cpp
 * Role: QTest coverage for WorldOptionDefaults behavior.
 */

#include "WorldOptionDefaults.h"

#include <QMap>
#include <QtTest/QTest>

/**
 * @brief QTest fixture covering WorldOptionDefaults scenarios.
 */
class tst_WorldOptionDefaults : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void generateWorldUniqueIdFormat()
		{
			const QString            first  = QMudWorldOptionDefaults::generateWorldUniqueId();
			const QString            second = QMudWorldOptionDefaults::generateWorldUniqueId();

			const QRegularExpression hex24(QStringLiteral("^[0-9a-f]{24}$"));
			QVERIFY(hex24.match(first).hasMatch());
			QVERIFY(hex24.match(second).hasMatch());
			QVERIFY(first != second);
		}

		void applyDefaultsPopulatesExpectedValues()
		{
			QMap<QString, QString> attrs;
			QMap<QString, QString> multilineAttrs;
			QMudWorldOptionDefaults::applyWorldOptionDefaults(attrs, multilineAttrs);

			QVERIFY(attrs.contains(QStringLiteral("id")));
			QVERIFY(attrs.contains(QStringLiteral("name")));
			QVERIFY(attrs.contains(QStringLiteral("wrap")));
			QCOMPARE(attrs.value(QStringLiteral("wrap")), QStringLiteral("y"));
			QCOMPARE(attrs.value(QStringLiteral("beep_sound")), QStringLiteral("sounds/QMud/beep.wav"));
			QCOMPARE(attrs.value(QStringLiteral("confirm_before_replacing_typing")), QStringLiteral("n"));
			QCOMPARE(attrs.value(QStringLiteral("utf_8")), QStringLiteral("y"));
			QCOMPARE(attrs.value(QStringLiteral("persist_output_buffer")), QStringLiteral("y"));
			QCOMPARE(attrs.value(QStringLiteral("persist_command_history")), QStringLiteral("y"));

			QVERIFY(multilineAttrs.contains(QStringLiteral("connect_text")));
			QVERIFY(multilineAttrs.contains(QStringLiteral("notes")));
		}

		void applyDefaultsPreservesExistingValues()
		{
			QMap<QString, QString> attrs;
			QMap<QString, QString> multilineAttrs;

			attrs.insert(QStringLiteral("name"), QStringLiteral("CustomWorld"));
			multilineAttrs.insert(QStringLiteral("notes"), QStringLiteral("keep me"));

			QMudWorldOptionDefaults::applyWorldOptionDefaults(attrs, multilineAttrs);

			QCOMPARE(attrs.value(QStringLiteral("name")), QStringLiteral("CustomWorld"));
			QCOMPARE(multilineAttrs.value(QStringLiteral("notes")), QStringLiteral("keep me"));
		}

		void alphaOptionLookupAndTableSanity()
		{
			const WorldAlphaOption *opt =
			    QMudWorldOptionDefaults::findWorldAlphaOption(QStringLiteral("  NAME "));
			QVERIFY(opt != nullptr);
			QCOMPARE(QString::fromLatin1(opt->name), QStringLiteral("name"));

			const WorldAlphaOption *table = worldAlphaOptions();
			const int               count = worldAlphaOptionCount();
			QVERIFY(table != nullptr);
			QVERIFY(count > 0);
			QVERIFY(table[count].name == nullptr);
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_WorldOptionDefaults)

#include "tst_WorldOptionDefaults.moc"
