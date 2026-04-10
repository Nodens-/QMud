/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_Dialog_WorldPreferences.cpp
 * Role: QTest coverage for Dialog WorldPreferences behavior.
 */

#include "WorldPreferencesRoutingUtils.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFile>
#include <QtTest/QTest>

/**
 * @brief QTest fixture covering Dialog WorldPreferences scenarios.
 */
class tst_Dialog_WorldPreferences : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void worldPreferencesButtonConnectionsAreAfterConstruction()
		{
			const QString sourcePath =
			    QDir(QStringLiteral(QMUD_TEST_SOURCE_DIR))
			        .filePath(QStringLiteral("src/dialogs/WorldPreferencesDialog.cpp"));
			QFile sourceFile(sourcePath);
			QVERIFY2(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text),
			         qPrintable(QStringLiteral("Failed to open %1").arg(sourcePath)));
			const QString sourceText = QString::fromUtf8(sourceFile.readAll());

			auto          firstMatchLine = [&sourceText](const QString &pattern) -> int
			{
				const QRegularExpression      regex(pattern);
				const QRegularExpressionMatch match = regex.match(sourceText);
				if (!match.hasMatch())
					return -1;
				return static_cast<int>(sourceText.left(match.capturedStart()).count(QLatin1Char('\n'))) + 1;
			};

			const QStringList mustBeConstructedBeforeConnect = {QStringLiteral("m_browseLogFile"),
			                                                    QStringLiteral("m_standardPreamble"),
			                                                    QStringLiteral("m_editPreamble"),
			                                                    QStringLiteral("m_editPostamble"),
			                                                    QStringLiteral("m_substitutionHelp"),
			                                                    QStringLiteral("m_connectText"),
			                                                    QStringLiteral("m_keypadControl"),
			                                                    QStringLiteral("m_infoCalculateMemory"),
			                                                    QStringLiteral("m_chatSaveBrowse"),
			                                                    QStringLiteral("m_resetMxpTagsButton"),
			                                                    QStringLiteral("m_loadNotesButton"),
			                                                    QStringLiteral("m_saveNotesButton"),
			                                                    QStringLiteral("m_editNotesButton"),
			                                                    QStringLiteral("m_findNotesButton"),
			                                                    QStringLiteral("m_findNextNotesButton"),
			                                                    QStringLiteral("m_editTriggersFilter"),
			                                                    QStringLiteral("m_filterTriggers"),
			                                                    QStringLiteral("m_editAliasesFilter"),
			                                                    QStringLiteral("m_filterAliases"),
			                                                    QStringLiteral("m_editTimersFilter"),
			                                                    QStringLiteral("m_filterTimers"),
			                                                    QStringLiteral("m_ansiDefaults"),
			                                                    QStringLiteral("m_ansiSwap"),
			                                                    QStringLiteral("m_ansiInvert"),
			                                                    QStringLiteral("m_ansiLighter"),
			                                                    QStringLiteral("m_ansiDarker"),
			                                                    QStringLiteral("m_ansiMoreColour"),
			                                                    QStringLiteral("m_ansiLessColour"),
			                                                    QStringLiteral("m_ansiRandom"),
			                                                    QStringLiteral("m_ansiLoad"),
			                                                    QStringLiteral("m_ansiSave"),
			                                                    QStringLiteral("m_copyAnsiToCustom")};

			for (const QString &widgetName : mustBeConstructedBeforeConnect)
			{
				const QString escaped = QRegularExpression::escape(widgetName);
				const int createLine  = firstMatchLine(QStringLiteral("\\b%1\\s*=\\s*new\\b").arg(escaped));
				const int connectLine =
				    firstMatchLine(QStringLiteral("\\bconnect\\s*\\(\\s*%1\\s*,").arg(escaped));

				QVERIFY2(createLine > 0,
				         qPrintable(QStringLiteral("No construction found for %1").arg(widgetName)));
				QVERIFY2(connectLine > 0,
				         qPrintable(QStringLiteral("No connect found for %1").arg(widgetName)));
				QVERIFY2(connectLine > createLine,
				         qPrintable(QStringLiteral("%1 connect line %2 is before construction line %3")
				                        .arg(widgetName)
				                        .arg(connectLine)
				                        .arg(createLine)));
			}
		}

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

#if __has_include("tst_Dialog_WorldPreferences.moc")
#include "tst_Dialog_WorldPreferences.moc"
#endif
