/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_Dialog_WorldPreferences.cpp
 * Role: QTest coverage for Dialog WorldPreferences behavior.
 */

#include "WorldPreferencesRoutingUtils.h"

#include <QtTest/QTest>

/**
 * @brief QTest fixture covering Dialog WorldPreferences scenarios.
 */
class tst_Dialog_WorldPreferences : public QObject
{
		Q_OBJECT

	// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void commandRecognitionAndPageMapping_data()
		{
			QTest::addColumn<QString>("cmdName");
			QTest::addColumn<bool>("mudAddressAlias");
			QTest::addColumn<bool>("mxpAlias");
			QTest::addColumn<bool>("autoSayAlias");
			QTest::addColumn<bool>("pasteAlias");
			QTest::addColumn<int>("lastPage");
			QTest::addColumn<bool>("expectedRecognized");
			QTest::addColumn<int>("expectedPage");

			QTest::newRow("preferences-valid-last-page")
			    << QStringLiteral("Preferences") << false << false << false << false
			    << static_cast<int>(WorldPreferencesDialog::PageTimers) << true
			    << static_cast<int>(WorldPreferencesDialog::PageTimers);
			QTest::newRow("preferences-negative-last-page")
			    << QStringLiteral("Preferences") << false << false << false << false << -1 << true
			    << static_cast<int>(WorldPreferencesDialog::PageGeneral);
			QTest::newRow("preferences-overflow-last-page")
			    << QStringLiteral("Preferences") << false << false << false << false << 999 << true
			    << static_cast<int>(WorldPreferencesDialog::PageGeneral);
			QTest::newRow("configure-logging-direct")
			    << QStringLiteral("ConfigureLogging") << false << false << false << false << 0 << true
			    << static_cast<int>(WorldPreferencesDialog::PageLogging);
			QTest::newRow("configure-chat-direct")
			    << QStringLiteral("ConfigureChat") << false << false << false << false << 0 << true
			    << static_cast<int>(WorldPreferencesDialog::PageChat);
			QTest::newRow("configure-mud-address-alias")
			    << QStringLiteral("AliasForMudAddress") << true << false << false << false << 0 << true
			    << static_cast<int>(WorldPreferencesDialog::PageGeneral);
			QTest::newRow("configure-mxp-alias")
			    << QStringLiteral("AliasForMxp") << false << true << false << false << 0 << true
			    << static_cast<int>(WorldPreferencesDialog::PageMxp);
			QTest::newRow("configure-autosay-alias")
			    << QStringLiteral("AliasForAutoSay") << false << false << true << false << 0 << true
			    << static_cast<int>(WorldPreferencesDialog::PageAutoSay);
			QTest::newRow("configure-paste-alias")
			    << QStringLiteral("AliasForPaste") << false << false << false << true << 0 << true
			    << static_cast<int>(WorldPreferencesDialog::PagePaste);
			QTest::newRow("unrelated-command")
			    << QStringLiteral("NotAWorldPreferenceCommand") << false << false << false << false << 0
			    << false << static_cast<int>(WorldPreferencesDialog::PageGeneral);
		}

		void commandRecognitionAndPageMapping()
		{
			QFETCH(QString, cmdName);
			QFETCH(bool, mudAddressAlias);
			QFETCH(bool, mxpAlias);
			QFETCH(bool, autoSayAlias);
			QFETCH(bool, pasteAlias);
			QFETCH(int, lastPage);
			QFETCH(bool, expectedRecognized);
			QFETCH(int, expectedPage);

			const auto matcher = [mudAddressAlias, mxpAlias, autoSayAlias,
			                      pasteAlias](const QString &commandName) -> bool
			{
				if (commandName == QStringLiteral("ConfigureMudAddress"))
					return mudAddressAlias;
				if (commandName == QStringLiteral("ConfigureMxp"))
					return mxpAlias;
				if (commandName == QStringLiteral("ConfigureAutoSay"))
					return autoSayAlias;
				if (commandName == QStringLiteral("ConfigurePaste"))
					return pasteAlias;
				return false;
	// NOLINTEND(readability-convert-member-functions-to-static)
			};

			QCOMPARE(QMudWorldPreferencesRouting::isPreferencesCommand(cmdName, matcher), expectedRecognized);
			QCOMPARE(static_cast<int>(
			             QMudWorldPreferencesRouting::initialPageForCommand(cmdName, lastPage, matcher)),
			         expectedPage);
		}
};

QTEST_APPLESS_MAIN(tst_Dialog_WorldPreferences)

#include "tst_Dialog_WorldPreferences.moc"
