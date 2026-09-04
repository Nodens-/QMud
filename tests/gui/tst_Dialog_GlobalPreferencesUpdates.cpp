/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_Dialog_GlobalPreferencesUpdates.cpp
 * Role: QTest coverage for Global Preferences update settings behavior and persistence.
 */

#include "AppController.h"
#include "ShortcutPreferenceUtils.h"
#include "dialogs/GlobalPreferencesDialog.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFileInfo>
// ReSharper disable once CppUnusedIncludeDirective
#include <QFocusEvent>
#include <QFont>
#include <QHeaderView>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QScreen>
#include <QSettings>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextEdit>
#include <QWidget>
#include <QtTest/QTest>

#include <memory>

namespace
{
	constexpr int kShortcutDefaultPreferenceKeyRole         = Qt::UserRole + 1;
	constexpr int kShortcutDisabledDefaultPortableTextsRole = Qt::UserRole + 4;

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
	 * @brief Finds a checkbox by exact text within a widget tree.
	 * @param root Root object for recursive search.
	 * @param text Checkbox text to match.
	 * @return Matching checkbox, or `nullptr`.
	 */
	QCheckBox *findCheckBoxByText(const QObject &root, const QString &text)
	{
		const QList<QCheckBox *> boxes = root.findChildren<QCheckBox *>();
		for (QCheckBox *box : boxes)
		{
			if (box && box->text() == text)
				return box;
		}
		return nullptr;
	}

	/**
	 * @brief Finds a label by exact text within a widget tree.
	 * @param root Root object for recursive search.
	 * @param text Label text to match.
	 * @return Matching label, or `nullptr`.
	 */
	QLabel *findLabelByText(const QObject &root, const QString &text)
	{
		const QList<QLabel *> labels = root.findChildren<QLabel *>();
		for (QLabel *label : labels)
		{
			if (label && label->text() == text)
				return label;
		}
		return nullptr;
	}

	/**
	 * @brief Finds a push button by exact text within a widget tree.
	 * @param root Root object for recursive search.
	 * @param text Button text to match.
	 * @return Matching button, or `nullptr`.
	 */
	QPushButton *findButtonByText(const QObject &root, const QString &text)
	{
		const QList<QPushButton *> buttons = root.findChildren<QPushButton *>();
		for (QPushButton *button : buttons)
		{
			if (button && button->text() == text)
				return button;
		}
		return nullptr;
	}

	/**
	 * @brief Finds shortcut override item by global preference key.
	 * @param table Table to scan.
	 * @param preferenceKey Shortcut preference key.
	 * @return Matching table item, or `nullptr`.
	 */
	QTableWidgetItem *findShortcutOverrideItemByPreferenceKey(const QTableWidget &table,
	                                                          const QString      &preferenceKey)
	{
		for (int row = 0; row < table.rowCount(); ++row)
		{
			for (int column = 0; column < table.columnCount(); ++column)
			{
				QTableWidgetItem *item = table.item(row, column);
				if (item && item->data(Qt::UserRole).toString() == preferenceKey)
					return item;
			}
		}
		return nullptr;
	}

	/**
	 * @brief Finds shortcut default item by global preference key.
	 * @param table Table to scan.
	 * @param preferenceKey Shortcut preference key.
	 * @return Matching default-column table item, or `nullptr`.
	 */
	QTableWidgetItem *findShortcutDefaultItemByPreferenceKey(const QTableWidget &table,
	                                                         const QString      &preferenceKey)
	{
		for (int row = 0; row < table.rowCount(); ++row)
		{
			QTableWidgetItem *item = table.item(row, 2);
			if (item && item->data(kShortcutDefaultPreferenceKeyRole).toString() == preferenceKey)
				return item;
		}
		return nullptr;
	}

	/**
	 * @brief Finds a spin box by exact suffix within a widget tree.
	 * @param root Root object for recursive search.
	 * @param suffix Spin-box suffix to match.
	 * @return Matching spin box, or `nullptr`.
	 */
	QSpinBox *findSpinBySuffix(const QObject &root, const QString &suffix)
	{
		const QList<QSpinBox *> spins = root.findChildren<QSpinBox *>();
		for (QSpinBox *spin : spins)
		{
			if (spin && spin->suffix() == suffix)
				return spin;
		}
		return nullptr;
	}
} // namespace

namespace
{
	/**
	 * @brief QTest fixture covering Global Preferences update-settings behavior.
	 */
	class tst_Dialog_GlobalPreferencesUpdates : public QObject
	{
			Q_OBJECT

			// NOLINTBEGIN(readability-convert-member-functions-to-static)
		private slots:
			/**
			 * @brief Creates an isolated production preferences store under the test binary directory.
			 */
			void initTestCase()
			{
				m_originalCurrentPath = QDir::currentPath();
				m_hadOriginalQmudHome = qEnvironmentVariableIsSet("QMUD_HOME");
				m_originalQmudHome    = qgetenv("QMUD_HOME");
				const QString artifactDirectory =
				    QDir(QCoreApplication::applicationDirPath())
				        .filePath(QStringLiteral("test-artifacts/tst_Dialog_GlobalPreferencesUpdates/%1")
				                      .arg(QCoreApplication::applicationPid()));
				QVERIFY(QDir().mkpath(artifactDirectory));
				QVERIFY(QDir::setCurrent(artifactDirectory));
				QVERIFY(qputenv("QMUD_HOME", artifactDirectory.toUtf8()));

				m_app = std::make_unique<AppController>();
				QVERIFY(m_app->initialize());
			}

			/**
			 * @brief Releases production state and restores the process environment.
			 */
			void cleanupTestCase()
			{
				m_app.reset();
				if (m_hadOriginalQmudHome)
					QVERIFY(qputenv("QMUD_HOME", m_originalQmudHome));
				else
					qunsetenv("QMUD_HOME");
				QVERIFY(QDir::setCurrent(m_originalCurrentPath));
			}

			/**
			 * @brief Verifies update-check controls are disabled when mechanism is unavailable.
			 */
			void updateControlsDisableWhenMechanismUnavailable() const
			{
				resetPreferences();
				const bool       hadDisableUpdate      = qEnvironmentVariableIsSet("QMUD_DISABLE_UPDATE");
				const QByteArray originalDisableUpdate = qgetenv("QMUD_DISABLE_UPDATE");
				QVERIFY(qputenv("QMUD_DISABLE_UPDATE", QByteArrayLiteral("1")));
				const auto restoreDisableUpdate = qScopeGuard(
				    [hadDisableUpdate, originalDisableUpdate]
				    {
					    if (hadDisableUpdate)
						    (void)qputenv("QMUD_DISABLE_UPDATE", originalDisableUpdate);
					    else
						    qunsetenv("QMUD_DISABLE_UPDATE");
				    });
				QVERIFY(!AppController::isUpdateMechanismAvailable());

				GlobalPreferencesDialog dialog;
				dialog.show();

				QCheckBox *autoCheck =
				    findCheckBoxByText(dialog, QStringLiteral("Automatically check for updates"));
				QCheckBox *enableReload =
				    findCheckBoxByText(dialog, QStringLiteral("Enable reload feature (Linux/MacOS)"));
				QPushButton *checkNowButton  = findButtonByText(dialog, QStringLiteral("Check now"));
				QSpinBox    *hoursSpin       = findSpinBySuffix(dialog, QStringLiteral(" hour(s)"));
				QSpinBox    *timeoutSpin     = findSpinBySuffix(dialog, QStringLiteral(" ms"));
				QLabel      *checkEveryLabel = findLabelByText(dialog, QStringLiteral("Check every:"));
				QVERIFY(autoCheck);
				QVERIFY(enableReload);
				QVERIFY(checkNowButton);
				QVERIFY(hoursSpin);
				QVERIFY(timeoutSpin);
				QVERIFY(checkEveryLabel);

				QVERIFY(!autoCheck->isEnabled());
				QVERIFY(!checkNowButton->isEnabled());
				QVERIFY(!hoursSpin->isEnabled());
				QVERIFY(!checkEveryLabel->isEnabled());
				QVERIFY(enableReload->isEnabled());
				QVERIFY(timeoutSpin->isEnabled());
				QCOMPARE(autoCheck->toolTip(), AppController::updateMechanismUnavailableReason());
			}

			/**
			 * @brief Verifies hidden tab container does not trap keyboard focus traversal.
			 */
			void hiddenTabContainerIsNotFocusable() const
			{
				resetPreferences();

				GlobalPreferencesDialog dialog;

				auto                   *tabs = dialog.findChild<QTabWidget *>();
				QVERIFY(tabs);
				QCOMPARE(tabs->focusPolicy(), Qt::NoFocus);
				QVERIFY(tabs->tabBar());
				QCOMPARE(tabs->tabBar()->focusPolicy(), Qt::NoFocus);
			}

			/**
			 * @brief Verifies split-view divider placement, focus order, range, and persistence.
			 */
			void splitViewDividerPreferenceFollowsWindowTabsAndPersists() const
			{
				resetPreferences();

				{
					GlobalPreferencesDialog dialog;
					auto                   *tabs = dialog.findChild<QTabWidget *>();
					QVERIFY(tabs);
					tabs->setCurrentIndex(1);
					dialog.show();
					QVERIFY(QTest::qWaitForWindowExposed(&dialog));
					QCoreApplication::processEvents();

					QLabel *windowTabsLabel = findLabelByText(dialog, QStringLiteral("Window Tabs:"));
					QLabel *dividerLabel    = findLabelByText(dialog, QStringLiteral("Split-view divider:"));
					QVERIFY(windowTabsLabel);
					QVERIFY(dividerLabel);
					auto *windowTabs = qobject_cast<QComboBox *>(windowTabsLabel->buddy());
					auto *divider    = qobject_cast<QSpinBox *>(dividerLabel->buddy());
					QVERIFY(windowTabs);
					QVERIFY(divider);
					QCOMPARE(divider->minimum(), 1);
					QCOMPARE(divider->maximum(), 200);
					QCOMPARE(divider->value(), 1);
					QCOMPARE(divider->suffix(), QStringLiteral(" px"));
					QVERIFY(divider->mapTo(&dialog, QPoint()).x() > windowTabs->mapTo(&dialog, QPoint()).x());
					windowTabs->setFocus(Qt::OtherFocusReason);
					QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget *>(windowTabs));
					QTest::keyClick(windowTabs, Qt::Key_Tab);
					QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget *>(divider));

					divider->setValue(13);
					dialog.accept();
				}

				QCOMPARE(m_app->getGlobalOption(QStringLiteral("SplitViewDividerWidth")).toInt(), 13);

				GlobalPreferencesDialog restoredDialog;
				QSpinBox *restoredDivider = findSpinBySuffix(restoredDialog, QStringLiteral(" px"));
				QVERIFY(restoredDivider);
				QCOMPARE(restoredDivider->value(), 13);
			}

			/**
			 * @brief Verifies the dialog and combo boxes follow enlarged application fonts.
			 */
			void dialogMinimumTracksFontChanges() const
			{
				resetPreferences();

				const QFont originalApplicationFont = QApplication::font();
				const auto  restoreApplicationFont  = qScopeGuard(
                    [originalApplicationFont] { QApplication::setFont(originalApplicationFont); });

				QList<QSize> initialPageHints;
				QList<QSize> initialComboHints;
				QSize        initialDialogMinimum;
				QSize        initialDialogSize;
				{
					GlobalPreferencesDialog baselineDialog;
					baselineDialog.show();
					QVERIFY(QTest::qWaitForWindowExposed(&baselineDialog));
					QCoreApplication::processEvents();
					initialDialogMinimum = baselineDialog.minimumSize();
					initialDialogSize    = baselineDialog.size();
					auto *baselineTabs   = baselineDialog.findChild<QTabWidget *>();
					QVERIFY(baselineTabs);
					initialPageHints.reserve(baselineTabs->count());
					for (int index = 0; index < baselineTabs->count(); ++index)
					{
						QWidget *const page = baselineTabs->widget(index);
						QVERIFY(page);
						QVERIFY(page->layout());
						initialPageHints.push_back(page->layout()->sizeHint());
					}
					for (const QComboBox *combo : baselineDialog.findChildren<QComboBox *>())
						initialComboHints.push_back(combo->minimumSizeHint());
				}

				QFont enlargedFont = originalApplicationFont;
				if (enlargedFont.pointSizeF() > 0.0)
					enlargedFont.setPointSizeF(enlargedFont.pointSizeF() * 6.0);
				else
					enlargedFont.setPixelSize(qMax(1, enlargedFont.pixelSize() * 6));
				QApplication::setFont(enlargedFont);

				GlobalPreferencesDialog dialog;
				dialog.show();
				QVERIFY(QTest::qWaitForWindowExposed(&dialog));
				QCoreApplication::processEvents();
				QVERIFY(dialog.screen());

				constexpr QSize baselineMinimum(748, 600);
				QVERIFY(dialog.minimumWidth() >= baselineMinimum.width() ||
				        dialog.frameGeometry().width() >= dialog.screen()->availableGeometry().width());
				QVERIFY(dialog.minimumHeight() >= baselineMinimum.height() ||
				        dialog.frameGeometry().height() >= dialog.screen()->availableGeometry().height());
				QVERIFY(dialog.maximumWidth() > dialog.minimumWidth());
				QVERIFY(dialog.maximumHeight() > dialog.minimumHeight());

				const QList<QComboBox *> combos = dialog.findChildren<QComboBox *>();
				QVERIFY(!combos.isEmpty());
				QCOMPARE(combos.size(), initialComboHints.size());
				for (qsizetype index = 0; index < combos.size(); ++index)
				{
					const QComboBox *const combo = combos.at(index);
					QVERIFY(combo->minimumSizeHint().height() >= combo->fontMetrics().height());
					QVERIFY(combo->minimumSizeHint().width() > initialComboHints.at(index).width());
					QVERIFY(combo->minimumSizeHint().height() > initialComboHints.at(index).height());
				}

				auto *tabs    = dialog.findChild<QTabWidget *>();
				auto *buttons = dialog.findChild<QDialogButtonBox *>();
				QVERIFY(tabs);
				QVERIFY(buttons);
				QVERIFY(dialog.rect().contains(tabs->geometry()));
				QVERIFY(dialog.rect().contains(buttons->geometry()));
				QCOMPARE(static_cast<qsizetype>(tabs->count()), initialPageHints.size());
				const QSize dialogSize    = dialog.size();
				const QSize dialogMinimum = dialog.minimumSize();
				bool        pageHintGrew  = false;
				for (int index = 0; index < tabs->count(); ++index)
				{
					tabs->setCurrentIndex(index);
					QCoreApplication::processEvents();
					QCOMPARE(dialog.size(), dialogSize);
					QCOMPARE(dialog.minimumSize(), dialogMinimum);
					QWidget *const page = tabs->widget(index);
					QVERIFY(page);
					QVERIFY(page->layout());
					const int frameWidth =
					    tabs->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, tabs);
					const int pageOuterHint   = page->layout()->sizeHint().width() + (frameWidth * 2);
					const int maximumTabWidth = dialog.width() - tabs->mapTo(&dialog, QPoint()).x() -
					                            dialog.layout()->contentsMargins().right();
					QVERIFY(tabs->minimumWidth() >= qMin(pageOuterHint, maximumTabWidth));
					QVERIFY(tabs->width() <= maximumTabWidth);
					const int pageOuterHeight = page->layout()->sizeHint().height() + (frameWidth * 2);
					QVERIFY(tabs->minimumHeight() >= qMin(pageOuterHeight, tabs->height()));
					for (const QTabBar *tabBar : dialog.findChildren<QTabBar *>())
					{
						if (tabBar->isVisible())
							QVERIFY(tabBar->width() >= tabBar->sizeHint().width());
					}
					pageHintGrew = pageHintGrew ||
					               page->layout()->sizeHint().width() > initialPageHints.at(index).width() ||
					               page->layout()->sizeHint().height() > initialPageHints.at(index).height();
				}
				QVERIFY(pageHintGrew);
				QVERIFY(dialog.width() >= dialog.minimumWidth());
				QVERIFY(dialog.height() >= dialog.minimumHeight());
				QTRY_VERIFY(dialog.screen()->availableGeometry().contains(dialog.frameGeometry()));

				ResizeEventCounter resizeEvents;
				dialog.installEventFilter(&resizeEvents);
				QCoreApplication::processEvents();
				resizeEvents.reset();
				QApplication::setFont(originalApplicationFont);
				QTRY_VERIFY(dialog.minimumWidth() < dialogMinimum.width() ||
				            dialog.minimumHeight() < dialogMinimum.height() ||
				            dialog.frameGeometry().width() >= dialog.screen()->availableGeometry().width() ||
				            dialog.frameGeometry().height() >= dialog.screen()->availableGeometry().height());
				QVERIFY(dialog.minimumWidth() >= initialDialogMinimum.width());
				QVERIFY(dialog.minimumHeight() >= initialDialogMinimum.height());
				QTRY_VERIFY(dialog.width() < dialogSize.width() || dialog.height() < dialogSize.height() ||
				            dialog.frameGeometry().width() >= dialog.screen()->availableGeometry().width() ||
				            dialog.frameGeometry().height() >= dialog.screen()->availableGeometry().height());
				QVERIFY(dialog.width() >= initialDialogSize.width());
				QVERIFY(dialog.height() >= initialDialogSize.height());
				QCoreApplication::processEvents();
				QVERIFY(resizeEvents.count() <= 1);
			}

			/**
			 * @brief Verifies long user paths wrap without determining dialog width.
			 */
			void longPathsDoNotExpandDialogWidth() const
			{
				resetPreferences();
				GlobalPreferencesDialog baselineDialog;
				const int               baselineMinimumWidth = baselineDialog.minimumWidth();

				resetPreferences();
				const QString longPath = QStringLiteral("/") + QStringLiteral("directory/").repeated(20) +
				                         QStringLiteral("file.xml");
				m_app->setGlobalOptionString(QStringLiteral("WorldList"), longPath);
				m_app->setGlobalOptionString(QStringLiteral("PluginList"), longPath);
				m_app->setGlobalOptionString(QStringLiteral("DefaultWorldFileDirectory"), longPath);
				m_app->setGlobalOptionString(QStringLiteral("DefaultLogFileDirectory"), longPath);
				m_app->setGlobalOptionString(QStringLiteral("PluginsDirectory"), longPath);
				QStringList unmatchedPaths{
				    m_app->getGlobalOption(QStringLiteral("WorldList")).toString(),
				    m_app->getGlobalOption(QStringLiteral("PluginList")).toString(),
				    m_app->getGlobalOption(QStringLiteral("DefaultWorldFileDirectory")).toString(),
				    m_app->getGlobalOption(QStringLiteral("DefaultLogFileDirectory")).toString(),
				    m_app->getGlobalOption(QStringLiteral("PluginsDirectory")).toString()};

				GlobalPreferencesDialog longPathDialog;
				QVERIFY(longPathDialog.minimumWidth() <= baselineMinimumWidth);
				longPathDialog.show();
				QVERIFY(QTest::qWaitForWindowExposed(&longPathDialog));
				auto *tabs = longPathDialog.findChild<QTabWidget *>();
				QVERIFY(tabs);
				int verifiedLabels = 0;
				for (int index = 0; index < tabs->count(); ++index)
				{
					tabs->setCurrentIndex(index);
					QCoreApplication::processEvents();
					for (const QLabel *label : tabs->widget(index)->findChildren<QLabel *>())
					{
						if (!unmatchedPaths.removeOne(label->text()))
							continue;
						QVERIFY(label->wordWrap());
						QVERIFY(label->width() > 0);
						const int requiredHeight = label->heightForWidth(label->width());
						QVERIFY(requiredHeight < 0 || label->height() >= requiredHeight);
						++verifiedLabels;
					}
				}
				QCOMPARE(verifiedLabels, 5);
				QVERIFY(unmatchedPaths.isEmpty());
				QVERIFY(longPathDialog.screen());
				QTRY_VERIFY(
				    longPathDialog.screen()->availableGeometry().contains(longPathDialog.frameGeometry()));
			}

			/**
			 * @brief Verifies Shortcuts page is placed between General and Closing.
			 */
			void shortcutsTabIsBetweenGeneralAndClosing() const
			{
				resetPreferences();

				GlobalPreferencesDialog dialog;

				auto                   *tabs = dialog.findChild<QTabWidget *>();
				QVERIFY(tabs);
				QCOMPARE(tabs->tabText(1), QStringLiteral("General"));
				QCOMPARE(tabs->tabText(2), QStringLiteral("Shortcuts"));
				QCOMPARE(tabs->tabText(3), QStringLiteral("Closing"));
			}

			/**
			 * @brief Verifies Shortcuts table sorts by category and leaves Tab for focus traversal.
			 */
			void shortcutsTableSortsAndDoesNotConsumeTabNavigation() const
			{
				resetPreferences();

				GlobalPreferencesDialog dialog;

				auto                   *table = dialog.findChild<QTableWidget *>();
				QVERIFY(table);
				QVERIFY(table->isSortingEnabled());
				QVERIFY(!table->tabKeyNavigation());
				QVERIFY(table->horizontalHeader());
				QCOMPARE(table->horizontalHeader()->sortIndicatorSection(), 0);
				QCOMPARE(table->horizontalHeader()->sortIndicatorOrder(), Qt::AscendingOrder);
				for (int row = 0; row < table->rowCount(); ++row)
					QVERIFY(!table->cellWidget(row, 3));

				QString previousCategory;
				for (int row = 0; row < table->rowCount(); ++row)
				{
					QTableWidgetItem *categoryItem = table->item(row, 0);
					QVERIFY(categoryItem);
					const QString category = categoryItem->text();
					QVERIFY(previousCategory <= category);
					previousCategory = category;
					for (int column = 1; column < table->columnCount(); ++column)
						QVERIFY(table->item(row, column));
				}

				table->sortItems(1, Qt::AscendingOrder);
				QCOMPARE(table->horizontalHeader()->sortIndicatorSection(), 1);
				QString previousAction;
				for (int row = 0; row < table->rowCount(); ++row)
				{
					const QString action = table->item(row, 1)->text();
					QVERIFY(previousAction <= action);
					previousAction = action;
				}

				QTableWidgetItem *worldShortcut =
				    findShortcutOverrideItemByPreferenceKey(*table, QStringLiteral("Shortcut.World10"));
				QVERIFY(worldShortcut);
				worldShortcut->setText(QStringLiteral("Ctrl+Alt+0"));

				table->sortItems(3, Qt::DescendingOrder);
				QCOMPARE(table->horizontalHeader()->sortIndicatorSection(), 3);
				QVERIFY(table->item(0, 3));
				QCOMPARE(table->item(0, 3)->text(), QStringLiteral("Ctrl+Alt+0"));

				table->setCurrentCell(table->rowCount() - 1, 0);
				QFocusEvent tabFocusEvent(QEvent::FocusIn, Qt::TabFocusReason);
				QCoreApplication::sendEvent(table, &tabFocusEvent);
				QCOMPARE(table->currentRow(), 0);
				QCOMPARE(table->currentColumn(), 0);
			}

			/**
			 * @brief Supplies Return variants that should open the shortcut override editor.
			 */
			void shortcutsTableEnterStartsOverrideEditing_data()
			{
				QTest::addColumn<int>("key");
				QTest::addColumn<Qt::KeyboardModifiers>("modifiers");
				QTest::newRow("return")
				    << static_cast<int>(Qt::Key_Return) << Qt::KeyboardModifiers(Qt::NoModifier);
				QTest::newRow("keypad-enter")
				    << static_cast<int>(Qt::Key_Enter) << Qt::KeyboardModifiers(Qt::KeypadModifier);
			}

			/**
			 * @brief Verifies Enter opens the current row's override editor without accepting the dialog.
			 */
			void shortcutsTableEnterStartsOverrideEditing() const
			{
				resetPreferences();
				QFETCH(int, key);
				QFETCH(Qt::KeyboardModifiers, modifiers);

				GlobalPreferencesDialog dialog;
				auto                   *tabs  = dialog.findChild<QTabWidget *>();
				auto                   *table = dialog.findChild<QTableWidget *>();
				QVERIFY(tabs);
				QVERIFY(table);
				tabs->setCurrentIndex(2);
				dialog.show();
				QVERIFY(QTest::qWaitForWindowExposed(&dialog));

				QTableWidgetItem *overrideItem =
				    findShortcutOverrideItemByPreferenceKey(*table, QStringLiteral("Shortcut.DisplayStart"));
				QVERIFY(overrideItem);
				table->setCurrentCell(overrideItem->row(), 0);
				table->setFocus(Qt::OtherFocusReason);
				QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget *>(table));

				QTest::keyClick(table, static_cast<Qt::Key>(key), modifiers);

				QVERIFY(dialog.isVisible());
				QCOMPARE(table->currentItem(), overrideItem);
				auto *editor = table->findChild<QLineEdit *>();
				QTRY_VERIFY(editor);
				QTRY_VERIFY(editor->hasFocus());
				QTest::keyClicks(editor, QStringLiteral("Ctrl+Alt+G"));
				QCOMPARE(editor->text(), QStringLiteral("Ctrl+Alt+G"));
				QVERIFY(dialog.isVisible());
			}

			/**
			 * @brief Verifies shortcut overrides persist through existing global preferences.
			 */
			void acceptPersistsShortcutOverride() const
			{
				resetPreferences();

				GlobalPreferencesDialog dialog;
				dialog.show();

				auto *table = dialog.findChild<QTableWidget *>();
				QVERIFY(table);
				QTableWidgetItem *displayStart =
				    findShortcutOverrideItemByPreferenceKey(*table, QStringLiteral("Shortcut.DisplayStart"));
				QVERIFY(displayStart);
				displayStart->setText(QStringLiteral("Ctrl+Alt+Home"));

				dialog.accept();

				QCOMPARE(m_app->getGlobalOption(QStringLiteral("Shortcut.DisplayStart")).toString(),
				         QStringLiteral("Ctrl+Alt+Home"));
			}

			/**
			 * @brief Verifies explicit overrides can claim another action's default shortcut.
			 */
			void overrideClaimsDefaultShortcut() const
			{
				resetPreferences();

				GlobalPreferencesDialog dialog;
				dialog.show();

				auto *table = dialog.findChild<QTableWidget *>();
				QVERIFY(table);
				QTableWidgetItem *displayStart =
				    findShortcutOverrideItemByPreferenceKey(*table, QStringLiteral("Shortcut.DisplayStart"));
				QVERIFY(displayStart);
				displayStart->setText(QStringLiteral("PgUp"));
				QTableWidgetItem *displayStartDefault =
				    findShortcutDefaultItemByPreferenceKey(*table, QStringLiteral("Shortcut.DisplayStart"));
				QVERIFY(displayStartDefault);
				QCOMPARE(displayStartDefault->data(kShortcutDisabledDefaultPortableTextsRole).toStringList(),
				         QStringList{});
				QTableWidgetItem *displayPageUpDefault =
				    findShortcutDefaultItemByPreferenceKey(*table, QStringLiteral("Shortcut.DisplayPageUp"));
				QVERIFY(displayPageUpDefault);
				QCOMPARE(displayPageUpDefault->data(kShortcutDisabledDefaultPortableTextsRole).toStringList(),
				         QStringList{QStringLiteral("PgUp")});

				dialog.accept();

				QCOMPARE(m_app->getGlobalOption(QStringLiteral("Shortcut.DisplayStart")).toString(),
				         QStringLiteral("PgUp"));
				QCOMPARE(
				    QMudShortcutPreferenceUtils::shortcutListToPortableText(
				        QMudShortcutPreferenceUtils::effectiveShortcutsForId(QStringLiteral("DisplayStart"))),
				    QStringLiteral("PgUp"));
				QCOMPARE(QMudShortcutPreferenceUtils::shortcutListToPortableText(
				             QMudShortcutPreferenceUtils::effectiveShortcutsForId(
				                 QStringLiteral("DisplayPageUp"))),
				         QString());
			}

			/**
			 * @brief Verifies stored duplicate explicit overrides do not become active duplicate shortcuts.
			 */
			void duplicateStoredOverridesFallBackToDefaults() const
			{
				resetPreferences();
				m_app->setGlobalOptionString(QStringLiteral("Shortcut.DisplayStart"), QStringLiteral("PgUp"));
				m_app->setGlobalOptionString(QStringLiteral("Shortcut.DisplayPageDown"),
				                             QStringLiteral("PgUp"));

				GlobalPreferencesDialog dialog;
				auto                   *table = dialog.findChild<QTableWidget *>();
				QVERIFY(table);
				QTableWidgetItem *displayPageUpDefault =
				    findShortcutDefaultItemByPreferenceKey(*table, QStringLiteral("Shortcut.DisplayPageUp"));
				QVERIFY(displayPageUpDefault);
				QCOMPARE(displayPageUpDefault->data(kShortcutDisabledDefaultPortableTextsRole).toStringList(),
				         QStringList{});

				QCOMPARE(
				    QMudShortcutPreferenceUtils::shortcutListToPortableText(
				        QMudShortcutPreferenceUtils::effectiveShortcutsForId(QStringLiteral("DisplayStart"))),
				    QStringLiteral("Ctrl+Home"));
				QCOMPARE(QMudShortcutPreferenceUtils::shortcutListToPortableText(
				             QMudShortcutPreferenceUtils::effectiveShortcutsForId(
				                 QStringLiteral("DisplayPageDown"))),
				         QStringLiteral("PgDown"));
				QCOMPARE(QMudShortcutPreferenceUtils::shortcutListToPortableText(
				             QMudShortcutPreferenceUtils::effectiveShortcutsForId(
				                 QStringLiteral("DisplayPageUp"))),
				         QStringLiteral("PgUp"));
			}

			/**
			 * @brief Verifies world-slot actions are configurable shortcut preferences.
			 */
			void worldSlotShortcutsArePreferenceDefinitions() const
			{
				resetPreferences();

				const auto *firstWorld =
				    QMudShortcutPreferenceUtils::definitionForId(QStringLiteral("World1"));
				QVERIFY(firstWorld);
				QCOMPARE(firstWorld->preferenceKey, QStringLiteral("Shortcut.World1"));
				QCOMPARE(QMudShortcutPreferenceUtils::shortcutListToPortableText(firstWorld->defaults),
				         QStringLiteral("Ctrl+1"));

				const auto *tenthWorld =
				    QMudShortcutPreferenceUtils::definitionForId(QStringLiteral("World10"));
				QVERIFY(tenthWorld);
				QCOMPARE(tenthWorld->preferenceKey, QStringLiteral("Shortcut.World10"));
				QCOMPARE(QMudShortcutPreferenceUtils::shortcutListToPortableText(tenthWorld->defaults),
				         QStringLiteral("Ctrl+0"));

				GlobalPreferencesDialog dialog;
				auto                   *table = dialog.findChild<QTableWidget *>();
				QVERIFY(table);
				QVERIFY(findShortcutOverrideItemByPreferenceKey(*table, firstWorld->preferenceKey));
				QVERIFY(findShortcutOverrideItemByPreferenceKey(*table, tenthWorld->preferenceKey));
			}

			/**
			 * @brief Verifies global shortcut validation reserves per-world macro slots.
			 */
			void macroShortcutReservations()
			{
				QVERIFY(QMudShortcutPreferenceUtils::isReservedMacroShortcut(
				    QKeySequence::fromString(QStringLiteral("F2"), QKeySequence::PortableText)));
				QVERIFY(QMudShortcutPreferenceUtils::isReservedMacroShortcut(
				    QKeySequence::fromString(QStringLiteral("Ctrl+F6"), QKeySequence::PortableText)));
				QVERIFY(QMudShortcutPreferenceUtils::isReservedMacroShortcut(
				    QKeySequence::fromString(QStringLiteral("Alt+N"), QKeySequence::PortableText)));
				QVERIFY(!QMudShortcutPreferenceUtils::isReservedMacroShortcut(
				    QKeySequence::fromString(QStringLiteral("Ctrl+Alt+Home"), QKeySequence::PortableText)));
			}

			/**
			 * @brief Verifies Lua page script editor passes Tab to focus traversal.
			 */
			void luaScriptEditorAllowsTabFocusTraversal() const
			{
				resetPreferences();
				m_app->setGlobalOptionString(QStringLiteral("LuaScript"),
				                             QStringLiteral("test-lua-script-editor-tab-focus"));

				GlobalPreferencesDialog dialog;

				const auto              editors = dialog.findChildren<QTextEdit *>();
				QTextEdit              *luaEditor{nullptr};
				for (QTextEdit *editor : editors)
				{
					if (editor &&
					    editor->toPlainText().contains(QStringLiteral("test-lua-script-editor-tab-focus")))
					{
						luaEditor = editor;
						break;
					}
				}

				QVERIFY(luaEditor);
				QVERIFY(luaEditor->tabChangesFocus());
			}

			/**
			 * @brief Verifies interval controls track auto-check toggle while check-now remains enabled.
			 */
			void updateIntervalTracksAutoCheckStateWhenMechanismAvailable() const
			{
				resetPreferences();
				m_app->setGlobalOptionInt(QStringLiteral("AutoCheckForUpdates"), 1);
				m_app->setGlobalOptionInt(QStringLiteral("UpdateCheckIntervalHours"), 6);

				const bool       hadDisableUpdate      = qEnvironmentVariableIsSet("QMUD_DISABLE_UPDATE");
				const QByteArray originalDisableUpdate = qgetenv("QMUD_DISABLE_UPDATE");
				const bool       hadAppImage           = qEnvironmentVariableIsSet("APPIMAGE");
				const QByteArray originalAppImage      = qgetenv("APPIMAGE");
				QVERIFY(qputenv("QMUD_DISABLE_UPDATE", QByteArrayLiteral("0")));
				QVERIFY(qputenv("APPIMAGE", QByteArrayLiteral("qmud-test.AppImage")));
				const auto restoreUpdateEnvironment = qScopeGuard(
				    [hadDisableUpdate, originalDisableUpdate, hadAppImage, originalAppImage]
				    {
					    if (hadDisableUpdate)
						    (void)qputenv("QMUD_DISABLE_UPDATE", originalDisableUpdate);
					    else
						    qunsetenv("QMUD_DISABLE_UPDATE");
					    if (hadAppImage)
						    (void)qputenv("APPIMAGE", originalAppImage);
					    else
						    qunsetenv("APPIMAGE");
				    });
				QVERIFY(AppController::isUpdateMechanismAvailable());

				GlobalPreferencesDialog dialog;
				dialog.show();

				QCheckBox *autoCheck =
				    findCheckBoxByText(dialog, QStringLiteral("Automatically check for updates"));
				QPushButton *checkNowButton  = findButtonByText(dialog, QStringLiteral("Check now"));
				QSpinBox    *hoursSpin       = findSpinBySuffix(dialog, QStringLiteral(" hour(s)"));
				QLabel      *checkEveryLabel = findLabelByText(dialog, QStringLiteral("Check every:"));
				QVERIFY(autoCheck);
				QVERIFY(checkNowButton);
				QVERIFY(hoursSpin);
				QVERIFY(checkEveryLabel);

				QVERIFY(autoCheck->isEnabled());
				QVERIFY(checkNowButton->isEnabled());
				QVERIFY(hoursSpin->isEnabled());
				QVERIFY(checkEveryLabel->isEnabled());
				QCOMPARE(hoursSpin->value(), 6);

				autoCheck->setChecked(false);
				QCoreApplication::processEvents();
				QVERIFY(!hoursSpin->isEnabled());
				QVERIFY(!checkEveryLabel->isEnabled());
				QVERIFY(checkNowButton->isEnabled());

				autoCheck->setChecked(true);
				QCoreApplication::processEvents();
				QVERIFY(hoursSpin->isEnabled());
				QVERIFY(checkEveryLabel->isEnabled());
				QVERIFY(checkNowButton->isEnabled());
			}

			/**
			 * @brief Verifies update-related options persist through dialog acceptance.
			 */
			void acceptPersistsUpdateSettings() const
			{
				resetPreferences();

				GlobalPreferencesDialog dialog;
				dialog.show();

				QCheckBox *autoCheck =
				    findCheckBoxByText(dialog, QStringLiteral("Automatically check for updates"));
				QCheckBox *enableReload =
				    findCheckBoxByText(dialog, QStringLiteral("Enable reload feature (Linux/MacOS)"));
				QSpinBox *hoursSpin   = findSpinBySuffix(dialog, QStringLiteral(" hour(s)"));
				QSpinBox *timeoutSpin = findSpinBySuffix(dialog, QStringLiteral(" ms"));
				QVERIFY(autoCheck);
				QVERIFY(enableReload);
				QVERIFY(hoursSpin);
				QVERIFY(timeoutSpin);

				autoCheck->setChecked(false);
				hoursSpin->setValue(24);
				enableReload->setChecked(false);
				timeoutSpin->setValue(850);

				dialog.accept();

				QCOMPARE(m_app->getGlobalOption(QStringLiteral("AutoCheckForUpdates")).toInt(), 0);
				QCOMPARE(m_app->getGlobalOption(QStringLiteral("UpdateCheckIntervalHours")).toInt(), 24);
				QCOMPARE(m_app->getGlobalOption(QStringLiteral("EnableReloadFeature")).toInt(), 0);
				QCOMPARE(m_app->getGlobalOption(QStringLiteral("ReloadMccpDisableTimeoutMs")).toInt(), 850);
			}

			/**
			 * @brief Verifies printer font family and style are displayed and persisted separately.
			 */
			void printerFontStyleLoadsAndPersists() const
			{
				resetPreferences();
				m_app->setGlobalOptionString(QStringLiteral("PrinterFont"), QStringLiteral("Menlo"));
				m_app->setGlobalOptionInt(QStringLiteral("PrinterFontSize"), 11);
				m_app->setGlobalOptionInt(QStringLiteral("PrinterFontWeight"), QFont::Bold);
				m_app->setGlobalOptionInt(QStringLiteral("PrinterFontItalic"), 1);

				GlobalPreferencesDialog dialog;
				dialog.show();

				QVERIFY(findLabelByText(dialog, QStringLiteral("Menlo")));
				QVERIFY(findLabelByText(dialog, QStringLiteral("11 pt. Bold Italic")));

				dialog.accept();

				QCOMPARE(m_app->getGlobalOption(QStringLiteral("PrinterFont")).toString(),
				         QStringLiteral("Menlo"));
				QCOMPARE(m_app->getGlobalOption(QStringLiteral("PrinterFontSize")).toInt(), 11);
				QCOMPARE(m_app->getGlobalOption(QStringLiteral("PrinterFontWeight")).toInt(),
				         static_cast<int>(QFont::Bold));
				QCOMPARE(m_app->getGlobalOption(QStringLiteral("PrinterFontItalic")).toInt(), 1);
			}

			/**
			 * @brief Verifies world-list entries persist relative to QMUD_HOME when possible.
			 */
			void acceptPersistsWorldListRelativeToQmudHome() const
			{
				resetPreferences();

				const QString qmudHome = QFileInfo(m_app->iniFilePath()).absolutePath();
				const QString worldPath =
				    QDir::cleanPath(QDir(qmudHome).filePath(QStringLiteral("worlds/test-world.mcl")));
				m_app->setGlobalOptionString(QStringLiteral("WorldList"), worldPath);

				GlobalPreferencesDialog dialog;
				dialog.show();
				dialog.accept();

				QCOMPARE(m_app->getGlobalOption(QStringLiteral("WorldList")).toString(),
				         QStringLiteral("./worlds/test-world.qdl"));
			}
			// NOLINTEND(readability-convert-member-functions-to-static)

		private:
			/**
			 * @brief Restores production preference values used by this fixture.
			 */
			void resetPreferences() const
			{
				Q_ASSERT(m_app);
				m_app->setGlobalOptionString(QStringLiteral("Locale"), QStringLiteral("en"));
				m_app->setGlobalOptionInt(QStringLiteral("SplitViewDividerWidth"), 1);
				m_app->setGlobalOptionInt(QStringLiteral("AutoCheckForUpdates"), 1);
				m_app->setGlobalOptionInt(QStringLiteral("UpdateCheckIntervalHours"), 1);
				m_app->setGlobalOptionInt(QStringLiteral("EnableReloadFeature"), 1);
				m_app->setGlobalOptionInt(QStringLiteral("ReloadMccpDisableTimeoutMs"), 1000);
				m_app->setGlobalOptionString(QStringLiteral("PrinterFont"), QStringLiteral("Courier"));
				m_app->setGlobalOptionInt(QStringLiteral("PrinterFontSize"), 10);
				m_app->setGlobalOptionInt(QStringLiteral("PrinterFontWeight"), QFont::Normal);
				m_app->setGlobalOptionInt(QStringLiteral("PrinterFontItalic"), 0);
				m_app->setGlobalOptionString(QStringLiteral("WorldList"), QString());
				m_app->setGlobalOptionString(QStringLiteral("PluginList"), QString());
				m_app->setGlobalOptionString(QStringLiteral("DefaultWorldFileDirectory"),
				                             QStringLiteral("./worlds/"));
				m_app->setGlobalOptionString(QStringLiteral("DefaultLogFileDirectory"),
				                             QStringLiteral("./logs/"));
				m_app->setGlobalOptionString(QStringLiteral("PluginsDirectory"),
				                             QStringLiteral("./worlds/plugins/"));
				m_app->setGlobalOptionString(QStringLiteral("LuaScript"), QString());
				for (const auto &definition : QMudShortcutPreferenceUtils::shortcutDefinitions())
					m_app->setGlobalOptionString(definition.preferenceKey, QString());

				QSettings settings(m_app->iniFilePath(), QSettings::IniFormat);
				settings.remove(QStringLiteral("GlobalPreferencesDialog"));
				settings.sync();
			}

			std::unique_ptr<AppController> m_app;
			QString                        m_originalCurrentPath;
			QByteArray                     m_originalQmudHome;
			bool                           m_hadOriginalQmudHome{false};
	};
} // namespace

QTEST_MAIN(tst_Dialog_GlobalPreferencesUpdates)

#if __has_include("tst_Dialog_GlobalPreferencesUpdates.moc")
#include "tst_Dialog_GlobalPreferencesUpdates.moc"
#endif
