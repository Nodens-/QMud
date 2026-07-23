/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_Dialog_WorldPreferences.cpp
 * Role: QTest coverage for Dialog WorldPreferences behavior.
 */

#include "WorldPreferencesRoutingUtils.h"
#include "WorldRuntime.h"
#include "dialogs/WorldPreferencesDialog.h"
#include "helpers/EncodingUtils.h"

#include <QCheckBox>
#include <QComboBox>
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
			};

			QCOMPARE(QMudWorldPreferencesRouting::isPreferencesCommand(cmdName, matcher), expectedRecognized);
			QCOMPARE(static_cast<int>(
			             QMudWorldPreferencesRouting::initialPageForCommand(cmdName, lastPage, matcher)),
			         expectedPage);
		}

		void spinBoxesUseRangeBasedWidthPolicy()
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

			QVERIFY2(firstMatchLine(QStringLiteral(
			             "\\bconfigureSpinBoxWidthForRange\\s*\\(\\s*QSpinBox\\s*\\*\\s*\\w+\\s*\\)")) > 0,
			         "Expected range-based spin-box width helper was not found.");
			QVERIFY2(firstMatchLine(QStringLiteral("\\bQStyleOptionSpinBox\\b")) > 0,
			         "Expected style-aware spin-box width calculation was not found.");

			const QStringList spinNames = {QStringLiteral("m_maxLines"),
			                               QStringLiteral("m_wrapColumn"),
			                               QStringLiteral("m_speedWalkDelay"),
			                               QStringLiteral("m_autoResizeMinimumLines"),
			                               QStringLiteral("m_autoResizeMaximumLines"),
			                               QStringLiteral("m_spamLineCount"),
			                               QStringLiteral("m_historyLines")};

			for (const QString &spinName : spinNames)
			{
				const QString escaped = QRegularExpression::escape(spinName);
				const int     createLine =
				    firstMatchLine(QStringLiteral("\\b%1\\s*=\\s*new\\s+QSpinBox\\s*\\(").arg(escaped));
				const int setRangeLine =
				    firstMatchLine(QStringLiteral("\\b%1\\s*->\\s*setRange\\s*\\(").arg(escaped));
				const int configureLine = firstMatchLine(
				    QStringLiteral("\\bconfigureSpinBoxWidthForRange\\s*\\(\\s*%1\\s*\\)\\s*;").arg(escaped));

				QVERIFY2(createLine > 0,
				         qPrintable(QStringLiteral("No QSpinBox construction found for %1").arg(spinName)));
				QVERIFY2(setRangeLine > 0,
				         qPrintable(QStringLiteral("No setRange(...) call found for %1").arg(spinName)));
				QVERIFY2(configureLine > 0,
				         qPrintable(QStringLiteral("No configureSpinBoxWidthForRange(...) call found for %1")
				                        .arg(spinName)));
				QVERIFY2(setRangeLine > createLine,
				         qPrintable(
				             QStringLiteral("Expected setRange(...) after %1 construction.").arg(spinName)));
				QVERIFY2(configureLine > setRangeLine,
				         qPrintable(
				             QStringLiteral("Expected configureSpinBoxWidthForRange(%1) after setRange(...).")
				                 .arg(spinName)));

				const QRegularExpression fixedWidthPattern(
				    QStringLiteral("\\b%1\\s*->\\s*set(?:Maximum|Minimum|Fixed)Width\\s*\\(").arg(escaped));
				QVERIFY2(!fixedWidthPattern.match(sourceText).hasMatch(),
				         qPrintable(QStringLiteral("Unexpected fixed-width policy for %1").arg(spinName)));
			}
		}

		void listedEditorsAndTablesAllowTabFocusTraversal()
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

			const QStringList textEditNames = {QStringLiteral("m_connectText"),
			                                   QStringLiteral("m_logFilePreamble"),
			                                   QStringLiteral("m_logFilePostamble"),
			                                   QStringLiteral("m_notes"),
			                                   QStringLiteral("m_pastePreamble"),
			                                   QStringLiteral("m_pastePostamble"),
			                                   QStringLiteral("m_sendToWorldFilePreamble"),
			                                   QStringLiteral("m_sendToWorldFilePostamble")};
			for (const QString &editName : textEditNames)
			{
				const QString escaped = QRegularExpression::escape(editName);
				const int createLine  = firstMatchLine(QStringLiteral("\\b%1\\s*=\\s*new\\b").arg(escaped));
				const int tabLine     = firstMatchLine(
				    QStringLiteral("\\b%1\\s*->\\s*setTabChangesFocus\\s*\\(\\s*true\\s*\\)").arg(escaped));

				QVERIFY2(createLine > 0,
				         qPrintable(QStringLiteral("No construction found for %1").arg(editName)));
				QVERIFY2(
				    tabLine > createLine,
				    qPrintable(
				        QStringLiteral("No setTabChangesFocus(true) after %1 construction.").arg(editName)));
			}
			QVERIFY2(sourceText.contains(QStringLiteral("words->setTabChangesFocus(true);")),
			         "Expected Tab Completion word list editor to pass Tab to focus traversal.");

			const QStringList tableNames = {QStringLiteral("m_macrosTable"),
			                                QStringLiteral("m_variablesTable")};
			for (const QString &tableName : tableNames)
			{
				const QString escaped    = QRegularExpression::escape(tableName);
				const int     createLine = firstMatchLine(QStringLiteral("\\b%1\\s*=").arg(escaped));
				const int     tabLine    = firstMatchLine(
				    QStringLiteral("\\b%1\\s*->\\s*setTabKeyNavigation\\s*\\(\\s*false\\s*\\)").arg(escaped));

				QVERIFY2(createLine > 0,
				         qPrintable(QStringLiteral("No construction found for %1").arg(tableName)));
				QVERIFY2(tabLine > createLine,
				         qPrintable(QStringLiteral("No setTabKeyNavigation(false) after %1 construction.")
				                        .arg(tableName)));
			}
		}

		void legacyEncodingControlFollowsUtf8Option()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("name"), QStringLiteral("Legacy Encoding Test"));
			runtime.setWorldAttribute(QStringLiteral("site"), QStringLiteral("localhost"));
			runtime.setWorldAttribute(QStringLiteral("port"), QStringLiteral("4000"));
			runtime.setWorldAttribute(QStringLiteral("utf_8"), QStringLiteral("0"));
			runtime.setWorldAttribute(QStringLiteral("legacy_encoding"), QStringLiteral("GB18030"));
			runtime.setWorldAttribute(QStringLiteral("proxy_type"), QStringLiteral("0"));
			runtime.setWorldAttribute(QStringLiteral("connect_method"), QStringLiteral("0"));
			runtime.setWorldAttribute(QStringLiteral("enable_command_stack"), QStringLiteral("0"));
			runtime.setWorldAttribute(QStringLiteral("command_stack_character"), QStringLiteral(";"));
			runtime.setWorldAttribute(QStringLiteral("enable_speed_walk"), QStringLiteral("0"));
			runtime.setWorldAttribute(QStringLiteral("speed_walk_prefix"), QStringLiteral("#"));
			runtime.setWorldAttribute(QStringLiteral("enable_auto_say"), QStringLiteral("0"));
			runtime.setWorldAttribute(QStringLiteral("auto_say_string"), QStringLiteral("say "));
			runtime.setWorldAttribute(QStringLiteral("log_output"), QStringLiteral("1"));
			runtime.setWorldAttribute(QStringLiteral("log_raw"), QStringLiteral("0"));

			WorldPreferencesDialog dialog(&runtime, nullptr);
			auto *combo = dialog.findChild<QComboBox *>(QStringLiteral("legacyEncodingCombo"));
			QVERIFY(combo);

			QCheckBox *utf8 = nullptr;
			for (QCheckBox *box : dialog.findChildren<QCheckBox *>())
			{
				if (box->text() == QStringLiteral("UTF-8 (Unicode)"))
				{
					utf8 = box;
					break;
				}
			}
			if (utf8 == nullptr)
				QFAIL("UTF-8 checkbox not found");
			QCheckBox        &utf8CheckBox = *utf8;

			const QStringList encodings = qmudAvailableWorldTextEncodings();
			QCOMPARE(combo->count(), encodings.size());
			for (const QString &encodingName : encodings)
			{
				const int index = combo->findData(encodingName);
				QVERIFY2(index >= 0,
				         qPrintable(QStringLiteral("Missing legacy encoding item for %1").arg(encodingName)));
				QCOMPARE(combo->itemText(index), qmudWorldTextEncodingDisplayName(encodingName));
				QVERIFY(combo->itemText(index).contains(QLatin1Char('(')));
				QVERIFY(combo->itemText(index).endsWith(QLatin1Char(')')));
			}

			QCOMPARE(combo->currentData().toString(), QStringLiteral("GB18030"));
			QVERIFY(combo->isEnabled());

			utf8CheckBox.setChecked(true);
			QVERIFY(!combo->isEnabled());
			utf8CheckBox.setChecked(false);
			QVERIFY(combo->isEnabled());

			const QString targetEncoding = qmudNormalizeWorldTextEncodingName(QStringLiteral("windows-1252"));
			const int     targetIndex    = combo->findData(targetEncoding);
			QVERIFY(targetIndex >= 0);
			combo->setCurrentIndex(targetIndex);

			dialog.accept();

			QCOMPARE(runtime.worldAttributes().value(QStringLiteral("legacy_encoding")), targetEncoding);
		}

		void scriptingNoteColourApplyUpdatesRuntimeState()
		{
			const QString sourcePath =
			    QDir(QStringLiteral(QMUD_TEST_SOURCE_DIR))
			        .filePath(QStringLiteral("src/dialogs/WorldPreferencesDialog.cpp"));
			QFile sourceFile(sourcePath);
			QVERIFY2(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text),
			         qPrintable(QStringLiteral("Failed to open %1").arg(sourcePath)));
			const QString sourceText = QString::fromUtf8(sourceFile.readAll());

			QVERIFY2(sourceText.contains(QStringLiteral("QMudNoteColour::worldAttributeFromPublicIndex")),
			         "Expected scripting Note colour persistence to use shared note-colour encoding.");
			QVERIFY2(sourceText.contains(QStringLiteral("m_runtime->setNoteTextColour(notePublicIndex)")),
			         "Expected scripting Note colour apply path to update runtime Note() colour state.");
		}

		void scriptingNoteColourComboItemsUseCustomColours()
		{
			const QString sourcePath =
			    QDir(QStringLiteral(QMUD_TEST_SOURCE_DIR))
			        .filePath(QStringLiteral("src/dialogs/WorldPreferencesDialog.cpp"));
			QFile sourceFile(sourcePath);
			QVERIFY2(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text),
			         qPrintable(QStringLiteral("Failed to open %1").arg(sourcePath)));
			const QString sourceText = QString::fromUtf8(sourceFile.readAll());

			QVERIFY2(sourceText.contains(QStringLiteral("updateScriptNoteColourItems")),
			         "Expected scripting Note colour combo item role updater.");
			QVERIFY2(sourceText.contains(QStringLiteral("Qt::ForegroundRole")),
			         "Expected scripting Note colour combo entries to carry foreground colours.");
			QVERIFY2(sourceText.contains(QStringLiteral("Qt::BackgroundRole")),
			         "Expected scripting Note colour combo entries to carry background colours.");
		}
};
// NOLINTEND(readability-convert-member-functions-to-static)

QTEST_MAIN(tst_Dialog_WorldPreferences)

#if __has_include("tst_Dialog_WorldPreferences.moc")
#include "tst_Dialog_WorldPreferences.moc"
#endif
