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
#include "helpers/DialogSizingUtils.h"
#include "helpers/EncodingUtils.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontMetrics>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMargins>
#include <QPushButton>
#include <QScopeGuard>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyleOptionSpinBox>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QtTest/QTest>

namespace
{
	/**
	 * @brief Counts top-level resize events emitted during one application-font update.
	 */
	class ResizeEventCounter final : public QObject
	{
		public:
			/**
			 * @brief Clears the accumulated resize-event count.
			 */
			void reset()
			{
				m_count = 0;
			}

			/**
			 * @brief Returns the accumulated resize-event count.
			 * @return Number of observed resize events.
			 */
			[[nodiscard]] int count() const
			{
				return m_count;
			}

			/**
			 * @brief Counts resize events without consuming them.
			 * @param watched Object receiving the event.
			 * @param event Event delivered to the watched object.
			 * @return Base event-filter result.
			 */
			bool eventFilter(QObject *watched, QEvent *event) override
			{
				if (event && event->type() == QEvent::Resize)
					++m_count;
				return QObject::eventFilter(watched, event);
			}

		private:
			int m_count{0};
	};

	/**
	 * @brief Finds a checkbox by its exact visible label.
	 * @param root Root object to search.
	 * @param text Checkbox label to match.
	 * @return Matching checkbox, or `nullptr` when absent.
	 */
	QCheckBox *findCheckBoxByText(const QObject &root, const QString &text)
	{
		for (QCheckBox *checkbox : root.findChildren<QCheckBox *>())
		{
			if (checkbox && checkbox->text() == text)
				return checkbox;
		}
		return nullptr;
	}

	/**
	 * @brief QTest fixture covering Dialog WorldPreferences scenarios.
	 */
	class tst_Dialog_WorldPreferences : public QObject
	{
			Q_OBJECT

			// NOLINTBEGIN(readability-convert-member-functions-to-static)
		private slots:
			/**
			 * @brief Verifies screen constraints account for frames and non-primary screen origins.
			 */
			void dialogSizingGeometryHandlesFramesAndNegativeOrigins()
			{
				const QRect        available(-1920, -200, 1600, 900);
				constexpr QMargins frameMargins(8, 30, 12, 10);
				QCOMPARE(
				    DialogSizingUtils::mergedFrameMarginsForObservedOverhead(frameMargins, QSize(20, 40)),
				    frameMargins);
				QCOMPARE(
				    DialogSizingUtils::mergedFrameMarginsForObservedOverhead(frameMargins, QSize(24, 45)),
				    QMargins(8, 30, 16, 15));
				QCOMPARE(DialogSizingUtils::mergedFrameMarginsForObservedOverhead(QMargins(-2, -3, 4, 5),
				                                                                  QSize(7, 9)),
				         QMargins(0, 0, 7, 9));
				QCOMPARE(DialogSizingUtils::maximumClientSizeForAvailableSize(available.size(), frameMargins),
				         QSize(1580, 860));
				QCOMPARE(DialogSizingUtils::maximumCentralHeightForLayout(860, 700, 520), 680);
				QCOMPARE(DialogSizingUtils::maximumCentralHeightForLayout(120, 700, 520), 1);

				const QRect frame(100, 100, 1200, 700);
				QCOMPARE(DialogSizingUtils::boundedFrameTopLeft(available, frame), QPoint(-1520, 0));
				const QRect oversizedFrame(100, 100, 1800, 1000);
				QCOMPARE(DialogSizingUtils::boundedFrameTopLeft(available, oversizedFrame),
				         available.topLeft());

				constexpr QSize initialContent(700, 520);
				constexpr QSize enlargedContent(820, 610);
				QCOMPARE(DialogSizingUtils::desiredClientSizeForContentChange(
				             QSize(900, 650), QSize(), QSize(), QSize(), initialContent),
				         QSize(900, 650));
				QCOMPARE(
				    DialogSizingUtils::desiredClientSizeForContentChange(
				        QSize(900, 650), QSize(900, 650), QSize(900, 650), initialContent, enlargedContent),
				    QSize(1020, 740));
				QCOMPARE(DialogSizingUtils::desiredClientSizeForContentChange(
				             QSize(1020, 740), QSize(1020, 740), QSize(1020, 740), enlargedContent,
				             initialContent),
				         QSize(900, 650));

				constexpr QSize screenClampedContent(1400, 1000);
				constexpr QSize desiredBeforeClamp(1000, 700);
				const QSize     desiredWhileClamped = DialogSizingUtils::desiredClientSizeForContentChange(
				    desiredBeforeClamp, desiredBeforeClamp, desiredBeforeClamp, initialContent,
				    screenClampedContent);
				QCOMPARE(desiredWhileClamped, QSize(1700, 1180));
				QCOMPARE(DialogSizingUtils::desiredClientSizeForContentChange(
				             QSize(1100, 800), QSize(1100, 800), desiredWhileClamped, screenClampedContent,
				             initialContent),
				         desiredBeforeClamp);
				QCOMPARE(
				    DialogSizingUtils::desiredClientSizeForContentChange(
				        QSize(950, 680), QSize(900, 650), QSize(900, 650), initialContent, enlargedContent),
				    QSize(1070, 770));
				QCOMPARE(DialogSizingUtils::desiredClientSizeForContentChange(
				             QSize(1100, 850), QSize(1100, 800), desiredWhileClamped, screenClampedContent,
				             screenClampedContent),
				         QSize(1700, 1000));
				QCOMPARE(DialogSizingUtils::desiredClientSizeForContentChange(
				             QSize(1150, 800), QSize(1100, 800), desiredWhileClamped, screenClampedContent,
				             screenClampedContent),
				         QSize(1400, 1180));
			}

			/**
			 * @brief Verifies initial sizing preserves the platform's parent-relative placement.
			 */
			void initialSizingDoesNotForceNativePosition()
			{
				QWidget                parent;
				WorldRuntime           runtime;
				WorldPreferencesDialog dialog(&runtime, nullptr, &parent);

				QVERIFY(!dialog.testAttribute(Qt::WA_Moved));

				parent.resize(700, 650);
				parent.show();
				QVERIFY(QTest::qWaitForWindowExposed(&parent));
				QVERIFY(parent.screen());
				const QRect availableGeometry = parent.screen()->availableGeometry();
				parent.move(parent.pos() + availableGeometry.center() - parent.frameGeometry().center());
				QCoreApplication::processEvents();
				dialog.show();
				QVERIFY(QTest::qWaitForWindowExposed(&dialog));
				QVERIFY(dialog.windowHandle());
				QTRY_VERIFY(dialog.screen()->availableGeometry().contains(dialog.frameGeometry()));
				QTRY_VERIFY(qAbs(dialog.frameGeometry().center().x() - parent.frameGeometry().center().x()) <=
				            4);
				QTRY_VERIFY(qAbs(dialog.frameGeometry().center().y() - parent.frameGeometry().center().y()) <=
				            4);
			}

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
					return static_cast<int>(sourceText.left(match.capturedStart()).count(QLatin1Char('\n'))) +
					       1;
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
					const int     createLine =
					    firstMatchLine(QStringLiteral("\\b%1\\s*=\\s*new\\b").arg(escaped));
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

				QCOMPARE(QMudWorldPreferencesRouting::isPreferencesCommand(cmdName, matcher),
				         expectedRecognized);
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
					return static_cast<int>(sourceText.left(match.capturedStart()).count(QLatin1Char('\n'))) +
					       1;
				};

				QVERIFY2(firstMatchLine(QStringLiteral(
				             "\\bconfigureSpinBoxWidthForRange\\s*\\(\\s*QSpinBox\\s*\\*\\s*\\w+\\s*\\)")) >
				             0,
				         "Expected range-based spin-box width helper was not found.");
				QVERIFY2(firstMatchLine(QStringLiteral("\\bQStyleOptionSpinBox\\b")) > 0,
				         "Expected style-aware spin-box width calculation was not found.");

				const QStringList spinNames = {QStringLiteral("m_maxLines"),
				                               QStringLiteral("m_wrapColumn"),
				                               QStringLiteral("m_pixelOffset"),
				                               QStringLiteral("m_lineSpacing"),
				                               QStringLiteral("m_outputFontHeight"),
				                               QStringLiteral("m_fadeOutputBufferAfterSeconds"),
				                               QStringLiteral("m_fadeOutputOpacityPercent"),
				                               QStringLiteral("m_fadeOutputSeconds"),
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
					    QStringLiteral("\\bconfigureSpinBoxWidthForRange\\s*\\(\\s*%1\\s*\\)\\s*;")
					        .arg(escaped));

					QVERIFY2(
					    createLine > 0,
					    qPrintable(QStringLiteral("No QSpinBox construction found for %1").arg(spinName)));
					QVERIFY2(setRangeLine > 0,
					         qPrintable(QStringLiteral("No setRange(...) call found for %1").arg(spinName)));
					QVERIFY2(
					    configureLine > 0,
					    qPrintable(QStringLiteral("No configureSpinBoxWidthForRange(...) call found for %1")
					                   .arg(spinName)));
					QVERIFY2(
					    setRangeLine > createLine,
					    qPrintable(
					        QStringLiteral("Expected setRange(...) after %1 construction.").arg(spinName)));
					QVERIFY2(configureLine > setRangeLine,
					         qPrintable(QStringLiteral(
					                        "Expected configureSpinBoxWidthForRange(%1) after setRange(...).")
					                        .arg(spinName)));

					const QRegularExpression fixedWidthPattern(
					    QStringLiteral("\\b%1\\s*->\\s*set(?:Maximum|Minimum|Fixed)Width\\s*\\(")
					        .arg(escaped));
					QVERIFY2(
					    !fixedWidthPattern.match(sourceText).hasMatch(),
					    qPrintable(QStringLiteral("Unexpected fixed-width policy for %1").arg(spinName)));
				}
			}

			void dialogMinimumTracksFontChanges()
			{
				const QFont originalApplicationFont = QApplication::font();
				const auto  restoreApplicationFont  = qScopeGuard(
				    [originalApplicationFont] { QApplication::setFont(originalApplicationFont); });
				WorldRuntime           runtime;
				WorldPreferencesDialog dialog(&runtime, nullptr);
				dialog.setInitialPage(WorldPreferencesDialog::PageOutput);
				dialog.show();
				QVERIFY(QTest::qWaitForWindowExposed(&dialog));
				QCoreApplication::processEvents();
				QVERIFY(dialog.screen());

				auto *combo = dialog.findChild<QComboBox *>(QStringLiteral("legacyEncodingCombo"));
				QVERIFY(combo);
				auto *scriptTextColourCombo =
				    dialog.findChild<QComboBox *>(QStringLiteral("scriptTextColourCombo"));
				QVERIFY(scriptTextColourCombo);
				QVERIFY(scriptTextColourCombo->maximumWidth() > scriptTextColourCombo->minimumWidth());
				auto *customSwapButton =
				    dialog.findChild<QPushButton *>(QStringLiteral("customColourSwapButton"));
				auto *customTextSwatch = dialog.findChild<QPushButton *>(QStringLiteral("customTextSwatch0"));
				auto *customBackSwatch = dialog.findChild<QPushButton *>(QStringLiteral("customBackSwatch0"));
				auto *autoSayFooter    = dialog.findChild<QWidget *>(QStringLiteral("autoSayFooter"));
				auto *ansiNormalHeader = dialog.findChild<QLabel *>(QStringLiteral("ansiNormalHeaderLabel"));
				auto *ansiNormalMarker = dialog.findChild<QPushButton *>(QStringLiteral("ansiNormalSwatch0"));
				auto *gradientHeader =
				    dialog.findChild<QWidget *>(QStringLiteral("worldPreferencesGradientHeader"));
				auto *infoPage = dialog.findChild<QWidget *>(QStringLiteral("worldPreferencesInfoPage"));
				QVERIFY(customSwapButton);
				QVERIFY(customTextSwatch);
				QVERIFY(customBackSwatch);
				QVERIFY(autoSayFooter);
				QVERIFY(ansiNormalHeader);
				QVERIFY(ansiNormalMarker);
				QVERIFY(gradientHeader);
				QVERIFY(infoPage);
				auto *autoSayCheckbox = autoSayFooter->findChild<QCheckBox *>();
				QVERIFY(autoSayCheckbox);
				const QStringList compactButtonNames = {
				    QStringLiteral("substitutionHelpButton"),   QStringLiteral("editLogPreambleButton"),
				    QStringLiteral("editLogPostambleButton"),   QStringLiteral("editTriggersFilterButton"),
				    QStringLiteral("editAliasesFilterButton"),  QStringLiteral("editTimersFilterButton"),
				    QStringLiteral("editVariablesFilterButton")};
				QList<QToolButton *> compactTextButtons;
				compactTextButtons.reserve(compactButtonNames.size());
				for (const QString &buttonName : compactButtonNames)
				{
					auto *button = dialog.findChild<QToolButton *>(buttonName);
					QVERIFY2(button, qPrintable(QStringLiteral("Missing %1").arg(buttonName)));
					QCOMPARE(button->toolButtonStyle(), Qt::ToolButtonTextOnly);
					QVERIFY(!button->accessibleName().isEmpty());
					QVERIFY(!button->toolTip().isEmpty());
					compactTextButtons.push_back(button);
				}
				const QStringList responsiveEditNames = {
				    QStringLiteral("customColourName0"), QStringLiteral("scriptPrefixEdit"),
				    QStringLiteral("autoSayOverridePrefixEdit"), QStringLiteral("autoSayStringEdit")};
				QList<QLineEdit *> responsiveEdits;
				responsiveEdits.reserve(responsiveEditNames.size());
				for (const QString &editName : responsiveEditNames)
				{
					auto *edit = dialog.findChild<QLineEdit *>(editName);
					QVERIFY2(edit, qPrintable(QStringLiteral("Missing %1").arg(editName)));
					QVERIFY(edit->maximumWidth() > edit->minimumWidth());
					responsiveEdits.push_back(edit);
				}
				const QStringList  compactEditNames = {QStringLiteral("speedWalkPrefixEdit"),
				                                       QStringLiteral("commandStackCharacterEdit")};
				QList<QLineEdit *> compactEdits;
				compactEdits.reserve(compactEditNames.size());
				for (const QString &editName : compactEditNames)
				{
					auto *edit = dialog.findChild<QLineEdit *>(editName);
					QVERIFY2(edit, qPrintable(QStringLiteral("Missing %1").arg(editName)));
					compactEdits.push_back(edit);
				}

				constexpr QSize baselineMinimum(700, 520);
				QVERIFY(dialog.minimumWidth() >= baselineMinimum.width() ||
				        dialog.frameGeometry().width() >= dialog.screen()->availableGeometry().width());
				QVERIFY(dialog.minimumHeight() >= baselineMinimum.height() ||
				        dialog.frameGeometry().height() >= dialog.screen()->availableGeometry().height());
				const QSize  initialDialogMinimum        = dialog.minimumSize();
				const int    initialComboHeight          = combo->minimumSizeHint().height();
				const int    initialComboWidth           = combo->minimumSizeHint().width();
				const QSize  initialScriptTextColourHint = scriptTextColourCombo->minimumSizeHint();
				const QSize  initialCustomSwapHint       = customSwapButton->sizeHint();
				const QSize  initialAutoSayHint          = autoSayCheckbox->sizeHint();
				const QSize  initialAnsiHeaderSize       = ansiNormalHeader->size();
				const QSize  initialAnsiMarkerSize       = ansiNormalMarker->size();
				const QSize  initialGradientHeaderHint   = gradientHeader->minimumSizeHint();
				const int    initialInfoFontHeight       = infoPage->fontMetrics().height();
				QList<QSize> initialCompactButtonHints;
				initialCompactButtonHints.reserve(compactTextButtons.size());
				for (const QToolButton *button : compactTextButtons)
					initialCompactButtonHints.push_back(button->sizeHint());
				QList<QSize> initialResponsiveEditHints;
				initialResponsiveEditHints.reserve(responsiveEdits.size());
				for (const QLineEdit *edit : responsiveEdits)
					initialResponsiveEditHints.push_back(edit->sizeHint());
				const QFont &initialFont       = originalApplicationFont;
				const QSize  frameSize         = dialog.frameGeometry().size() - dialog.geometry().size();
				const QSize  maximumClientSize = dialog.screen()->availableGeometry().size() - frameSize;
				const QSize  preferredDialogSize(qMin(maximumClientSize.width(), dialog.width() + 80),
				                                 qMin(maximumClientSize.height(), dialog.height() + 60));
				dialog.resize(preferredDialogSize);
				QTRY_COMPARE(dialog.size(), preferredDialogSize);
				const QRect availableGeometry = dialog.screen()->availableGeometry();
				dialog.move(availableGeometry.right() - 10, availableGeometry.bottom() - 10);
				ResizeEventCounter resizeEvents;
				dialog.installEventFilter(&resizeEvents);
				QCoreApplication::processEvents();
				resizeEvents.reset();

				QFont enlargedFont = initialFont;
				if (enlargedFont.pointSizeF() > 0.0)
					enlargedFont.setPointSizeF(enlargedFont.pointSizeF() * 2.0);
				else
					enlargedFont.setPixelSize(qMax(1, enlargedFont.pixelSize() * 2));
				QApplication::setFont(enlargedFont);

				QTRY_VERIFY(combo->minimumSizeHint().height() > initialComboHeight);
				QTRY_VERIFY(combo->minimumSizeHint().width() > initialComboWidth);
				QTRY_VERIFY(scriptTextColourCombo->minimumSizeHint().width() >
				            initialScriptTextColourHint.width());
				QTRY_VERIFY(scriptTextColourCombo->minimumSizeHint().height() >
				            initialScriptTextColourHint.height());
				QVERIFY(scriptTextColourCombo->maximumWidth() >=
				        scriptTextColourCombo->minimumSizeHint().width());
				QTRY_VERIFY(customSwapButton->sizeHint().width() > initialCustomSwapHint.width());
				QTRY_VERIFY(autoSayCheckbox->sizeHint().width() > initialAutoSayHint.width());
				QVERIFY(customSwapButton->parentWidget()->maximumWidth() >=
				        customSwapButton->parentWidget()->minimumSizeHint().width());
				QVERIFY(autoSayFooter->maximumWidth() >= autoSayFooter->minimumSizeHint().width());
				QTRY_VERIFY(ansiNormalHeader->width() > initialAnsiHeaderSize.width() ||
				            ansiNormalHeader->height() > initialAnsiHeaderSize.height());
				QTRY_VERIFY(ansiNormalMarker->width() > initialAnsiMarkerSize.width() ||
				            ansiNormalMarker->height() > initialAnsiMarkerSize.height());
				QVERIFY(ansiNormalHeader->width() >=
				        ansiNormalHeader->fontMetrics().horizontalAdvance(ansiNormalHeader->text()));
				QVERIFY(ansiNormalMarker->width() >=
				        ansiNormalMarker->fontMetrics().horizontalAdvance(ansiNormalMarker->text()));
				QVERIFY(ansiNormalMarker->height() >= ansiNormalMarker->fontMetrics().height());
				QTRY_VERIFY(gradientHeader->minimumSizeHint().width() > initialGradientHeaderHint.width() ||
				            gradientHeader->minimumSizeHint().height() > initialGradientHeaderHint.height());
				QFont gradientTextFont = gradientHeader->font();
				gradientTextFont.setBold(true);
				const QFontMetrics gradientTextMetrics(gradientTextFont);
				QVERIFY(gradientHeader->minimumSizeHint().width() >=
				        gradientTextMetrics.horizontalAdvance(dialog.windowTitle()) + 16);
				QVERIFY(gradientHeader->minimumSizeHint().height() >= gradientTextMetrics.height() + 10);
				QTRY_VERIFY(infoPage->fontMetrics().height() > initialInfoFontHeight);
				if (enlargedFont.pointSizeF() > 0.0)
					QTRY_COMPARE(infoPage->font().pointSizeF(), qMax(8.0, enlargedFont.pointSizeF() - 2.0));
				else
					QTRY_COMPARE(infoPage->font().pixelSize(), qMax(1, enlargedFont.pixelSize() - 2));
				for (qsizetype index = 0; index < compactTextButtons.size(); ++index)
				{
					const QToolButton *const button = compactTextButtons.at(index);
					QVERIFY(button->sizeHint().width() > initialCompactButtonHints.at(index).width() ||
					        button->sizeHint().height() > initialCompactButtonHints.at(index).height());
					QVERIFY(button->maximumWidth() >= button->sizeHint().width());
				}
				for (qsizetype index = 0; index < responsiveEdits.size(); ++index)
				{
					const QLineEdit *const edit = responsiveEdits.at(index);
					QVERIFY(edit->sizeHint().width() > initialResponsiveEditHints.at(index).width());
					QVERIFY(edit->maximumWidth() > edit->minimumWidth());
				}
				for (const QLineEdit *edit : compactEdits)
					QVERIFY(edit->width() >= edit->fontMetrics().horizontalAdvance(QLatin1Char('M')) + 12);
				for (QSpinBox *spin : dialog.findChildren<QSpinBox *>())
				{
					if (!spin->property("qmud_range_sized").toBool())
						continue;
					const auto editFieldWidth = [spin]
					{
						QStyleOptionSpinBox option;
						option.initFrom(spin);
						option.rect        = QRect(QPoint(), QSize(spin->width(), spin->sizeHint().height()));
						option.subControls = QStyle::SC_All;
						return spin->style()
						    ->subControlRect(QStyle::CC_SpinBox, &option, QStyle::SC_SpinBoxEditField, spin)
						    .width();
					};
					const auto displayText = [spin](const int value)
					{ return spin->prefix() + spin->locale().toString(value) + spin->suffix(); };
					const int requiredTextWidth =
					    qMax(spin->fontMetrics().horizontalAdvance(displayText(spin->minimum())),
					         spin->fontMetrics().horizontalAdvance(displayText(spin->maximum())));
					QTRY_VERIFY2(
					    editFieldWidth() >= requiredTextWidth,
					    qPrintable(QStringLiteral("range=[%1,%2], prefix='%3', suffix='%4', widgetWidth=%5, "
					                              "hintWidth=%6, editFieldWidth=%7, requiredTextWidth=%8")
					                   .arg(spin->minimum())
					                   .arg(spin->maximum())
					                   .arg(spin->prefix(), spin->suffix())
					                   .arg(spin->width())
					                   .arg(spin->sizeHint().width())
					                   .arg(editFieldWidth())
					                   .arg(requiredTextWidth)));
				}
				QTRY_VERIFY(dialog.minimumWidth() > initialDialogMinimum.width() ||
				            dialog.minimumHeight() > initialDialogMinimum.height() ||
				            dialog.frameGeometry().width() >= dialog.screen()->availableGeometry().width() ||
				            dialog.frameGeometry().height() >= dialog.screen()->availableGeometry().height());
				QVERIFY(dialog.width() >= dialog.minimumWidth());
				QVERIFY(dialog.height() >= dialog.minimumHeight());
				QTRY_VERIFY(dialog.screen()->availableGeometry().contains(dialog.frameGeometry()));
				QCoreApplication::processEvents();
				QVERIFY(resizeEvents.count() <= 1);

				auto *pages   = dialog.findChild<QStackedWidget *>();
				auto *tree    = dialog.findChild<QTreeWidget *>();
				auto *buttons = dialog.findChild<QDialogButtonBox *>();
				QVERIFY(pages);
				QVERIFY(tree);
				QVERIFY(buttons);
				QVERIFY(dialog.rect().contains(pages->geometry()));
				QVERIFY(dialog.rect().contains(tree->geometry()));
				QVERIFY(dialog.rect().contains(buttons->geometry()));
				int itemCount = 0;
				for (QTreeWidgetItemIterator iterator(tree); *iterator; ++iterator)
					++itemCount;
				QVERIFY(itemCount > 0);
				const int completeTreeHeight = tree->sizeHintForRow(0) * itemCount;
				QTRY_VERIFY(tree->minimumHeight() >= qMin(completeTreeHeight, tree->height()));
				const QSize enlargedDialogSize    = dialog.size();
				const QSize enlargedDialogMinimum = dialog.minimumSize();
				for (int index = 0; index < pages->count(); ++index)
				{
					pages->setCurrentIndex(index);
					QCoreApplication::processEvents();
					QCOMPARE(dialog.size(), enlargedDialogSize);
					QCOMPARE(dialog.minimumSize(), enlargedDialogMinimum);
					if (index == WorldPreferencesDialog::PageCustomColours)
					{
						const int swapCentreTwice = customSwapButton->geometry().center().x() * 2;
						const int swatchCentres   = customTextSwatch->geometry().center().x() +
						                            customBackSwatch->geometry().center().x();
						QVERIFY(qAbs(swapCentreTwice - swatchCentres) <= 2);
					}
					QWidget *const page = pages->widget(index);
					QVERIFY(page);
					if (!page->layout())
						continue;
					for (const QWidget *child : page->findChildren<QWidget *>(Qt::FindDirectChildrenOnly))
					{
						if (child->isVisible() && !child->isWindow())
							QVERIFY(page->rect().contains(child->geometry()));
					}
					const QSize pageHint         = page->layout()->sizeHint();
					const int   pageOuterHint    = pageHint.width() + (pages->frameWidth() * 2);
					const int   maximumPageWidth = dialog.width() - pages->mapTo(&dialog, QPoint()).x() -
					                               dialog.layout()->contentsMargins().right();
					QVERIFY(pages->minimumWidth() >= qMin(pageOuterHint, maximumPageWidth));
					QVERIFY(pages->width() <= maximumPageWidth);
					const int pageOuterHeight = pageHint.height() + (pages->frameWidth() * 2);
					QVERIFY(pages->minimumHeight() >= qMin(pageOuterHeight, pages->height()));
					if (page->layout()->hasHeightForWidth())
					{
						const int availablePageWidth = qMax(1, pages->width() - (pages->frameWidth() * 2));
						const int requiredHeight     = page->layout()->heightForWidth(availablePageWidth);
						QVERIFY(requiredHeight < 0 ||
						        pages->minimumHeight() >=
						            qMin(requiredHeight + (pages->frameWidth() * 2), pages->height()));
					}
				}

				resizeEvents.reset();
				QApplication::setFont(initialFont);
				QTRY_VERIFY(dialog.minimumWidth() < enlargedDialogMinimum.width() ||
				            dialog.minimumHeight() < enlargedDialogMinimum.height() ||
				            dialog.frameGeometry().width() >= dialog.screen()->availableGeometry().width() ||
				            dialog.frameGeometry().height() >= dialog.screen()->availableGeometry().height());
				QVERIFY(dialog.minimumWidth() >= initialDialogMinimum.width());
				QVERIFY(dialog.minimumHeight() >= initialDialogMinimum.height());
				QTRY_COMPARE(dialog.size(), preferredDialogSize);
				QCoreApplication::processEvents();
				QVERIFY(resizeEvents.count() <= 1);
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
					return static_cast<int>(sourceText.left(match.capturedStart()).count(QLatin1Char('\n'))) +
					       1;
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
					const int     createLine =
					    firstMatchLine(QStringLiteral("\\b%1\\s*=\\s*new\\b").arg(escaped));
					const int tabLine = firstMatchLine(
					    QStringLiteral("\\b%1\\s*->\\s*setTabChangesFocus\\s*\\(\\s*true\\s*\\)")
					        .arg(escaped));

					QVERIFY2(createLine > 0,
					         qPrintable(QStringLiteral("No construction found for %1").arg(editName)));
					QVERIFY2(tabLine > createLine,
					         qPrintable(QStringLiteral("No setTabChangesFocus(true) after %1 construction.")
					                        .arg(editName)));
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
					    QStringLiteral("\\b%1\\s*->\\s*setTabKeyNavigation\\s*\\(\\s*false\\s*\\)")
					        .arg(escaped));

					QVERIFY2(createLine > 0,
					         qPrintable(QStringLiteral("No construction found for %1").arg(tableName)));
					QVERIFY2(tabLine > createLine,
					         qPrintable(QStringLiteral("No setTabKeyNavigation(false) after %1 construction.")
					                        .arg(tableName)));
				}
			}

			void tabCompletionSymbolOptionsDefaultPersistAndFollowFocusOrder()
			{
				WorldRuntime runtime;
				runtime.applyDefaultWorldOptions();
				QCOMPARE(
				    runtime.worldAttributes().value(QStringLiteral("tab_completion_excludes_symbol_prefix")),
				    QStringLiteral("y"));
				QCOMPARE(
				    runtime.worldAttributes().value(QStringLiteral("tab_completion_excludes_symbol_suffix")),
				    QStringLiteral("y"));

				WorldPreferencesDialog dialog(&runtime, nullptr);
				dialog.setInitialPage(WorldPreferencesDialog::PageCommands);
				dialog.show();
				QVERIFY(QTest::qWaitForWindowExposed(&dialog));
				QCoreApplication::processEvents();

				QCheckBox *lowerCase =
				    findCheckBoxByText(dialog, QStringLiteral("Tab Completion In Lower Case"));
				QCheckBox *excludeSymbolPrefix =
				    findCheckBoxByText(dialog, QStringLiteral("Tab Completion Excludes Symbol Prefix"));
				QCheckBox *excludeSymbolSuffix =
				    findCheckBoxByText(dialog, QStringLiteral("Tab Completion Excludes Symbol Suffix"));
				QCheckBox *translateGerman =
				    findCheckBoxByText(dialog, QStringLiteral("Translate German characters"));
				QVERIFY(lowerCase);
				QVERIFY(excludeSymbolPrefix);
				QVERIFY(excludeSymbolSuffix);
				QVERIFY(translateGerman);
				QVERIFY(excludeSymbolPrefix->isChecked());
				QVERIFY(excludeSymbolSuffix->isChecked());

				const int lowerCaseTop = lowerCase->mapTo(&dialog, QPoint()).y();
				const int excludeTop   = excludeSymbolPrefix->mapTo(&dialog, QPoint()).y();
				const int suffixTop    = excludeSymbolSuffix->mapTo(&dialog, QPoint()).y();
				const int germanTop    = translateGerman->mapTo(&dialog, QPoint()).y();
				QVERIFY(excludeTop > lowerCaseTop);
				QVERIFY(suffixTop > excludeTop);
				QVERIFY(suffixTop < germanTop);

				lowerCase->setFocus(Qt::OtherFocusReason);
				QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget *>(lowerCase));
				QTest::keyClick(lowerCase, Qt::Key_Tab);
				QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget *>(excludeSymbolPrefix));
				QTest::keyClick(excludeSymbolPrefix, Qt::Key_Tab);
				QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget *>(excludeSymbolSuffix));
				QTest::keyClick(excludeSymbolSuffix, Qt::Key_Tab);
				QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget *>(translateGerman));

				excludeSymbolPrefix->setChecked(false);
				excludeSymbolSuffix->setChecked(false);
				dialog.accept();
				QCOMPARE(
				    runtime.worldAttributes().value(QStringLiteral("tab_completion_excludes_symbol_prefix")),
				    QStringLiteral("0"));
				QCOMPARE(
				    runtime.worldAttributes().value(QStringLiteral("tab_completion_excludes_symbol_suffix")),
				    QStringLiteral("0"));

				WorldPreferencesDialog restoredDialog(&runtime, nullptr);
				QCheckBox             *restoredExcludeSymbolPrefix = findCheckBoxByText(
				    restoredDialog, QStringLiteral("Tab Completion Excludes Symbol Prefix"));
				QCheckBox *restoredExcludeSymbolSuffix = findCheckBoxByText(
				    restoredDialog, QStringLiteral("Tab Completion Excludes Symbol Suffix"));
				QVERIFY(restoredExcludeSymbolPrefix);
				QVERIFY(restoredExcludeSymbolSuffix);
				QVERIFY(!restoredExcludeSymbolPrefix->isChecked());
				QVERIFY(!restoredExcludeSymbolSuffix->isChecked());
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
					QVERIFY2(
					    index >= 0,
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

				const QString targetEncoding =
				    qmudNormalizeWorldTextEncodingName(QStringLiteral("windows-1252"));
				const int targetIndex = combo->findData(targetEncoding);
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
} // namespace
// NOLINTEND(readability-convert-member-functions-to-static)

QTEST_MAIN(tst_Dialog_WorldPreferences)

#if __has_include("tst_Dialog_WorldPreferences.moc")
#include "tst_Dialog_WorldPreferences.moc"
#endif
