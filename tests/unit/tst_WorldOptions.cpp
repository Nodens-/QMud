/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldOptions.cpp
 * Role: QTest coverage for WorldOptions behavior.
 */

#include "WorldOptions.h"

#include <QtTest/QTest>

/**
 * @brief QTest fixture covering WorldOptions scenarios.
 */
class tst_WorldOptions : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void numericTableSanity()
		{
			const WorldNumericOption *table = worldNumericOptions();
			const int                 count = worldNumericOptionCount();

			QVERIFY(table != nullptr);
			QVERIFY(count > 0);
			QVERIFY(table[count].name == nullptr);

			QSet<QString> names;
			for (int i = 0; table[i].name; ++i)
			{
				const QString name = QString::fromLatin1(table[i].name);
				QVERIFY(!names.contains(name));
				names.insert(name);
				QVERIFY(table[i].minValue <= table[i].maxValue);
			}

			QCOMPARE(names.size(), count);
		}

		void findNormalizesInput()
		{
			const WorldNumericOption *opt =
			    QMudWorldOptions::findWorldNumericOption(QStringLiteral("  WRAP_COLUMN "));
			QVERIFY(opt != nullptr);
			QCOMPARE(QString::fromLatin1(opt->name), QStringLiteral("wrap_column"));
			QCOMPARE(opt->minValue, 20LL);
			QCOMPARE(opt->maxValue, static_cast<long long>(MAX_LINE_WIDTH));
		}

		void regexpMatchEmptyOptionIsExposed()
		{
			const WorldNumericOption *opt =
			    QMudWorldOptions::findWorldNumericOption(QStringLiteral("regexp_match_empty"));
			QVERIFY(opt != nullptr);
			QCOMPARE(opt->defaultValue, 1LL);
			QCOMPARE(opt->minValue, 0LL);
			QCOMPARE(opt->maxValue, 0LL);
		}

		void sendKeepAlivesOptionIsBooleanAndDefaultOff()
		{
			const WorldNumericOption *opt =
			    QMudWorldOptions::findWorldNumericOption(QStringLiteral("send_keep_alives"));
			QVERIFY(opt != nullptr);
			QCOMPARE(opt->defaultValue, 0LL);
			QCOMPARE(opt->minValue, 0LL);
			QCOMPARE(opt->maxValue, 0LL);
		}

		void tlsDisableCertificateValidationOptionIsBooleanAndDefaultOff()
		{
			const WorldNumericOption *opt = QMudWorldOptions::findWorldNumericOption(
			    QStringLiteral("tls_disable_certificate_validation"));
			QVERIFY(opt != nullptr);
			QCOMPARE(opt->defaultValue, 0LL);
			QCOMPARE(opt->minValue, 0LL);
			QCOMPARE(opt->maxValue, 0LL);
		}

		void findUnknownReturnsNull()
		{
			QCOMPARE(QMudWorldOptions::findWorldNumericOption(QStringLiteral("not_an_option")),
			         static_cast<const WorldNumericOption *>(nullptr));
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_WorldOptions)

#if __has_include("tst_WorldOptions.moc")
#include "tst_WorldOptions.moc"
#endif
