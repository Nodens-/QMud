/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_Dialog_Plugins.cpp
 * Role: QTest coverage for Dialog Plugins behavior.
 */

#include "AppController.h"
#include "LuaExecutor.h"
#include "MainFrame.h"
// ReSharper disable once CppUnusedIncludeDirective
#include "NameGeneration.h"
#include "TelnetProcessor.h"
#include "WorldRuntime.h"
#include "dialogs/PluginsDialog.h"
#include "scripting/ScriptingErrors.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QApplication>
#include <QCoreApplication>
#include <QFont>
#include <QHeaderView>
#include <QPoint>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QTableWidget>
#include <QtTest/QSignalSpy>
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

	struct RuntimeStubState
	{
			QList<WorldRuntime::Plugin> plugins;
			QMap<QString, QString>      worldAttributes;
			QString                     pluginsDirectory;
			QString                     stateFilesDirectory;
			int                         reloadCalls{0};
			QString                     lastReloadPluginId;
	};

	QHash<const WorldRuntime *, RuntimeStubState> &runtimeStates()
	{
		static QHash<const WorldRuntime *, RuntimeStubState> states;
		return states;
	}

	RuntimeStubState &stateFor(const WorldRuntime *runtime)
	{
		return runtimeStates()[runtime];
	}

	QPushButton *findButtonByText(const QObject &root, const QString &text)
	{
		const auto buttons = root.findChildren<QPushButton *>();
		for (QPushButton *button : buttons)
		{
			if (button && button->text() == text)
				return button;
		}
		return nullptr;
	}

	WorldRuntime::Plugin makePlugin(const QString &id, const QString &name, const bool enabled = true)
	{
		WorldRuntime::Plugin plugin;
		plugin.attributes.insert(QStringLiteral("id"), id);
		plugin.attributes.insert(QStringLiteral("name"), name);
		plugin.source  = QStringLiteral("/tmp/%1.xml").arg(id);
		plugin.enabled = enabled;
		plugin.version = 1.0;
		return plugin;
	}

	bool isBlacklistedPluginIdForDialogTest(const QString &pluginId)
	{
		const QString id = pluginId.trimmed().toLower();
		return id == QStringLiteral("bb6a05ed7534b5db1ed40511") ||
		       id == QStringLiteral("b8e6dac1ee7fe8e3de931fb7") ||
		       id == QStringLiteral("8238deec7c06bade8ebc3819");
	}

	/**
	 * @brief Removes persisted Plugins dialog geometry and header state for an isolated test.
	 */
	void clearPluginsDialogSettings()
	{
		QSettings settings(AppController::instance()->iniFilePath(), QSettings::IniFormat);
		settings.remove(QStringLiteral("PluginsDialog"));
	}

	/**
	 * @brief Verifies every plugin header label fits within its current section.
	 * @param table Plugin table to inspect.
	 */
	void verifyHeaderLabelsFit(const QTableWidget &table)
	{
		const QHeaderView *const header = table.horizontalHeader();
		QVERIFY(header);
		for (int column = 0; column < table.columnCount(); ++column)
			QVERIFY(table.columnWidth(column) >= header->sectionSizeHint(column));
	}

	/**
	 * @brief Reports whether every plugin header label fits within its current section.
	 * @param table Plugin table to inspect.
	 * @return `true` when no label is clipped.
	 */
	[[nodiscard]] bool headerLabelsFit(const QTableWidget &table)
	{
		const QHeaderView *const header = table.horizontalHeader();
		if (!header)
			return false;
		for (int column = 0; column < table.columnCount(); ++column)
		{
			if (!table.horizontalHeaderItem(column) ||
			    table.columnWidth(column) < header->sectionSizeHint(column))
				return false;
		}
		return true;
	}

} // namespace

// NOLINTBEGIN(readability-convert-member-functions-to-static,readability-make-member-function-const)
AppController::AppController(QObject *parent) : QObject(parent)
{
}

AppController::~AppController() = default;

AppController *AppController::instance()
{
	static AppController app;
	return &app;
}

QString AppController::iniFilePath() const
{
	return QStringLiteral("/tmp/qmud-test-plugins-dialog.ini");
}

bool AppController::openDocumentFile(const QString &)
{
	return true;
}

void AppController::onCommandTriggered(const QString &)
{
}

TelnetProcessor::TelnetProcessor() = default;

WorldRuntime::WorldRuntime(QObject *parent) : QObject(parent)
{
	RuntimeStubState state;
	state.pluginsDirectory    = QStringLiteral("/tmp");
	state.stateFilesDirectory = QStringLiteral("/tmp");
	state.worldAttributes.insert(QStringLiteral("id"), QStringLiteral("world-id"));
	runtimeStates().insert(this, state);
}

WorldRuntime::~WorldRuntime()
{
	runtimeStates().remove(this);
}

const QMap<QString, QString> &WorldRuntime::worldAttributes() const
{
	return stateFor(this).worldAttributes;
}

const QList<WorldRuntime::Plugin> &WorldRuntime::plugins() const
{
	return stateFor(this).plugins;
}

QList<WorldRuntime::Plugin> &WorldRuntime::pluginsMutable()
{
	return stateFor(this).plugins;
}

QStringList WorldRuntime::pluginIdList() const
{
	QStringList ids;
	for (const Plugin &plugin : stateFor(this).plugins)
	{
		const QString id = plugin.attributes.value(QStringLiteral("id"));
		if (id.isEmpty() || isBlacklistedPluginIdForDialogTest(id) || ids.contains(id, Qt::CaseInsensitive))
			continue;
		ids.push_back(id);
	}
	return ids;
}

bool WorldRuntime::loadPluginFile(const QString &, QString *, bool)
{
	return true;
}

bool WorldRuntime::unloadPlugin(const QString &pluginId, QString *)
{
	QList<Plugin> &plugins = stateFor(this).plugins;
	for (qsizetype i = 0; i < plugins.size(); ++i)
	{
		if (plugins.at(i).attributes.value(QStringLiteral("id")) == pluginId)
		{
			plugins.removeAt(i);
			return true;
		}
	}
	return false;
}

bool WorldRuntime::enablePlugin(const QString &pluginId, const bool enable)
{
	QList<Plugin> &plugins = stateFor(this).plugins;
	for (Plugin &plugin : plugins)
	{
		if (plugin.attributes.value(QStringLiteral("id")) == pluginId)
		{
			plugin.enabled = enable;
			return true;
		}
	}
	return false;
}

int WorldRuntime::reloadPlugin(const QString &pluginId, QString *)
{
	RuntimeStubState &state = stateFor(this);
	++state.reloadCalls;
	state.lastReloadPluginId = pluginId;
	return eOK;
}

QString WorldRuntime::pluginsDirectory() const
{
	return stateFor(this).pluginsDirectory;
}

QString WorldRuntime::stateFilesDirectory() const
{
	return stateFor(this).stateFilesDirectory;
}

WorldChildWindow *MainWindow::activeWorldChildWindow() const
{
	return nullptr;
}

bool MainWindow::sendToNotepad(const QString &, const QString &, WorldRuntime *)
{
	return true;
}
// NOLINTEND(readability-convert-member-functions-to-static,readability-make-member-function-const)

namespace
{
	/**
	 * @brief QTest fixture covering Dialog Plugins scenarios.
	 */
	class tst_Dialog_Plugins : public QObject
	{
			Q_OBJECT

			// NOLINTBEGIN(readability-convert-member-functions-to-static)
		private slots:
			void tablePopulationAndSelectionState()
			{
				WorldRuntime runtime;
				runtime.pluginsMutable().push_back(makePlugin(QStringLiteral("a"), QStringLiteral("Alpha")));
				runtime.pluginsMutable().push_back(
				    makePlugin(QStringLiteral("b"), QStringLiteral("Beta"), false));

				PluginsDialog dialog(&runtime, nullptr);
				dialog.show();

				auto *table = dialog.findChild<QTableWidget *>();
				QVERIFY(table);
				QCOMPARE(table->rowCount(), 2);

				QPushButton *enableButton  = findButtonByText(dialog, QStringLiteral("Enable"));
				QPushButton *disableButton = findButtonByText(dialog, QStringLiteral("Disable"));
				QPushButton *reloadButton  = findButtonByText(dialog, QStringLiteral("ReInstall"));
				QVERIFY(enableButton);
				QVERIFY(disableButton);
				QVERIFY(reloadButton);

				QVERIFY(!enableButton->isEnabled());
				QVERIFY(!disableButton->isEnabled());
				QVERIFY(!reloadButton->isEnabled());

				QSignalSpy selectionChangedSpy(table->selectionModel(),
				                               &QItemSelectionModel::selectionChanged);
				table->selectRow(0);
				QCoreApplication::processEvents();
				QVERIFY(selectionChangedSpy.count() >= 1);

				QVERIFY(enableButton->isEnabled());
				QVERIFY(disableButton->isEnabled());
				QVERIFY(reloadButton->isEnabled());
			}

			/**
			 * @brief Verifies the dialog and action buttons follow enlarged application fonts.
			 */
			void dialogMinimumTracksFontChanges()
			{
				clearPluginsDialogSettings();
				const QFont originalApplicationFont = QApplication::font();
				const auto  restoreApplicationFont  = qScopeGuard(
				    [originalApplicationFont] { QApplication::setFont(originalApplicationFont); });
				WorldRuntime  runtime;
				PluginsDialog dialog(&runtime, nullptr);
				dialog.show();
				QVERIFY(QTest::qWaitForWindowExposed(&dialog));
				QCoreApplication::processEvents();
				QVERIFY(dialog.screen());

				constexpr QSize baselineMinimum(1250, 420);
				QVERIFY(dialog.minimumWidth() >= baselineMinimum.width() ||
				        dialog.frameGeometry().width() >= dialog.screen()->availableGeometry().width());
				QVERIFY(dialog.minimumHeight() >= baselineMinimum.height() ||
				        dialog.frameGeometry().height() >= dialog.screen()->availableGeometry().height());
				const QSize initialDialogMinimum = dialog.minimumSize();
				const QSize initialDialogSize    = dialog.size();
				QVERIFY(dialog.minimumSize().width() <= dialog.screen()->availableGeometry().width());
				QVERIFY(dialog.minimumSize().height() <= dialog.screen()->availableGeometry().height());

				QList<QPushButton *> buttons;
				for (QPushButton *button : dialog.findChildren<QPushButton *>())
				{
					if (button->isVisible())
						buttons.push_back(button);
				}
				QVERIFY(!buttons.isEmpty());
				QList<int> initialButtonWidths;
				initialButtonWidths.reserve(buttons.size());
				for (const QPushButton *button : buttons)
					initialButtonWidths.push_back(button->width());
				ResizeEventCounter resizeEvents;
				dialog.installEventFilter(&resizeEvents);
				QCoreApplication::processEvents();
				resizeEvents.reset();
				QFont enlargedFont = dialog.font();
				if (enlargedFont.pointSizeF() > 0.0)
					enlargedFont.setPointSizeF(enlargedFont.pointSizeF() * 6.0);
				else
					enlargedFont.setPixelSize(qMax(1, enlargedFont.pixelSize() * 6));
				QApplication::setFont(enlargedFont);

				for (qsizetype index = 0; index < buttons.size(); ++index)
				{
					QTRY_VERIFY(buttons.at(index)->width() > initialButtonWidths.at(index));
					QVERIFY(buttons.at(index)->height() >= buttons.at(index)->sizeHint().height());
					QCOMPARE(buttons.at(index)->width(), buttons.front()->width());
				}
				QTRY_VERIFY(dialog.minimumWidth() > initialDialogMinimum.width() ||
				            dialog.minimumHeight() > initialDialogMinimum.height() ||
				            dialog.minimumSize() == dialog.screen()->availableGeometry().size());
				QVERIFY(dialog.width() >= dialog.minimumWidth());
				QVERIFY(dialog.height() >= dialog.minimumHeight());
				auto *table = dialog.findChild<QTableWidget *>();
				QVERIFY(table);
				verifyHeaderLabelsFit(*table);
				QTRY_VERIFY(dialog.screen()->availableGeometry().contains(dialog.frameGeometry()));
				QCoreApplication::processEvents();
				QVERIFY(resizeEvents.count() <= 1);

				resizeEvents.reset();
				QApplication::setFont(originalApplicationFont);
				QTRY_COMPARE(dialog.minimumSize(), initialDialogMinimum);
				QTRY_COMPARE(dialog.size(), initialDialogSize);
				QCoreApplication::processEvents();
				QVERIFY(resizeEvents.count() <= 1);
			}

			/**
			 * @brief Verifies persisted user column widths are not replaced by defaults.
			 */
			void restoredColumnWidthsArePreserved()
			{
				clearPluginsDialogSettings();
				const auto   clearSettings = qScopeGuard([] { clearPluginsDialogSettings(); });
				WorldRuntime runtime;
				int          expectedWidth = 0;
				QSize        expectedDialogSize;
				{
					PluginsDialog dialog(&runtime, nullptr);
					dialog.show();
					QVERIFY(QTest::qWaitForWindowExposed(&dialog));
					auto *table = dialog.findChild<QTableWidget *>();
					QVERIFY(table);
					QHeaderView *const header = table->horizontalHeader();
					QVERIFY(header);
					const int    initialWidth = table->columnWidth(0);
					const int    boundary = header->sectionViewportPosition(0) + header->sectionSize(0) - 1;
					const QPoint pressPosition(boundary, header->height() / 2);
					QTest::mousePress(header->viewport(), Qt::LeftButton, Qt::NoModifier, pressPosition);
					QTest::mouseMove(header->viewport(), pressPosition + QPoint(48, 0));
					QTest::mouseRelease(header->viewport(), Qt::LeftButton, Qt::NoModifier,
					                    pressPosition + QPoint(48, 0));
					QTRY_VERIFY(table->columnWidth(0) > initialWidth);
					expectedWidth      = table->columnWidth(0);
					expectedDialogSize = dialog.size();
					dialog.reject();
				}
				{
					QSettings settings(AppController::instance()->iniFilePath(), QSettings::IniFormat);
					settings.beginGroup(QStringLiteral("PluginsDialog"));
					QCOMPARE(settings.value(QStringLiteral("UsingDefaultColumnWidths")).toBool(), false);
					QCOMPARE(settings.value(QStringLiteral("PreferredColumnWidths")).toList().size(), 6);
					settings.endGroup();
				}

				PluginsDialog restoredDialog(&runtime, nullptr);
				restoredDialog.show();
				QVERIFY(QTest::qWaitForWindowExposed(&restoredDialog));
				QTRY_COMPARE(restoredDialog.size(), expectedDialogSize);
				auto *restoredTable = restoredDialog.findChild<QTableWidget *>();
				QVERIFY(restoredTable);
				QCOMPARE(restoredTable->columnWidth(0), expectedWidth);

				const QFont originalFont = QApplication::font();
				const auto  restoreApplicationFont =
				    qScopeGuard([originalFont] { QApplication::setFont(originalFont); });
				QFont enlargedFont = originalFont;
				if (enlargedFont.pointSizeF() > 0.0)
					enlargedFont.setPointSizeF(enlargedFont.pointSizeF() * 6.0);
				else
					enlargedFont.setPixelSize(qMax(1, enlargedFont.pixelSize() * 6));
				QApplication::setFont(enlargedFont);
				QTRY_VERIFY(restoredTable->columnWidth(0) >=
				            restoredTable->horizontalHeader()->fontMetrics().horizontalAdvance(
				                restoredTable->horizontalHeaderItem(0)->text()));
				QApplication::setFont(originalFont);
				QTRY_COMPARE(restoredTable->columnWidth(0), expectedWidth);
			}

			/**
			 * @brief Verifies a user preference narrower than its header is retained without clipping the header.
			 */
			void userColumnWidthCannotClipHeader()
			{
				clearPluginsDialogSettings();
				const auto   clearSettings = qScopeGuard([] { clearPluginsDialogSettings(); });
				WorldRuntime runtime;
				int          effectiveWidth = 0;
				{
					PluginsDialog dialog(&runtime, nullptr);
					dialog.show();
					QVERIFY(QTest::qWaitForWindowExposed(&dialog));
					auto *table = dialog.findChild<QTableWidget *>();
					QVERIFY(table);
					QHeaderView *const header = table->horizontalHeader();
					QVERIFY(header);
					const int initialWidth = table->columnWidth(0);
					const int dragDistance = initialWidth - header->minimumSectionSize();
					QVERIFY(dragDistance > 0);
					const int    boundary = header->sectionViewportPosition(0) + header->sectionSize(0) - 1;
					const QPoint pressPosition(boundary, header->height() / 2);
					const QPoint releasePosition = pressPosition - QPoint(dragDistance, 0);
					QTest::mousePress(header->viewport(), Qt::LeftButton, Qt::NoModifier, pressPosition);
					QTest::mouseMove(header->viewport(), releasePosition);
					QTest::mouseRelease(header->viewport(), Qt::LeftButton, Qt::NoModifier, releasePosition);

					effectiveWidth = table->columnWidth(0);
					verifyHeaderLabelsFit(*table);
					dialog.reject();
				}

				QSettings settings(AppController::instance()->iniFilePath(), QSettings::IniFormat);
				settings.beginGroup(QStringLiteral("PluginsDialog"));
				QCOMPARE(settings.value(QStringLiteral("UsingDefaultColumnWidths")).toBool(), false);
				const QVariantList preferredWidths =
				    settings.value(QStringLiteral("PreferredColumnWidths")).toList();
				QCOMPARE(preferredWidths.size(), 6);
				QVERIFY(preferredWidths.at(0).toInt() < effectiveWidth);
				settings.endGroup();

				PluginsDialog restoredDialog(&runtime, nullptr);
				restoredDialog.show();
				QVERIFY(QTest::qWaitForWindowExposed(&restoredDialog));
				auto *restoredTable = restoredDialog.findChild<QTableWidget *>();
				QVERIFY(restoredTable);
				QCOMPARE(restoredTable->columnWidth(0), effectiveWidth);
				verifyHeaderLabelsFit(*restoredTable);
			}

			/**
			 * @brief Verifies a sort-indicator change reapplies current preferred-header requirements.
			 */
			void sortIndicatorChangeReappliesPreferredHeaderRequirements()
			{
				clearPluginsDialogSettings();
				const auto   clearSettings = qScopeGuard([] { clearPluginsDialogSettings(); });
				WorldRuntime runtime;
				runtime.pluginsMutable().push_back(
				    makePlugin(QStringLiteral("sort-test"), QStringLiteral("Sort Test")));
				PluginsDialog dialog(&runtime, nullptr);
				dialog.show();
				QVERIFY(QTest::qWaitForWindowExposed(&dialog));
				auto *table = dialog.findChild<QTableWidget *>();
				QVERIFY(table);
				QHeaderView *const header = table->horizontalHeader();
				QVERIFY(header);

				constexpr int column       = 2;
				const int     initialWidth = table->columnWidth(column);
				const int     dragDistance = initialWidth - header->minimumSectionSize();
				QVERIFY(dragDistance > 0);
				const int boundary =
				    header->sectionViewportPosition(column) + header->sectionSize(column) - 1;
				const QPoint pressPosition(boundary, header->height() / 2);
				const QPoint releasePosition = pressPosition - QPoint(dragDistance, 0);
				QTest::mousePress(header->viewport(), Qt::LeftButton, Qt::NoModifier, pressPosition);
				QTest::mouseMove(header->viewport(), releasePosition);
				QTest::mouseRelease(header->viewport(), Qt::LeftButton, Qt::NoModifier, releasePosition);

				QVERIFY(header->sortIndicatorSection() != column);
				const int               unsortedWidth = table->columnWidth(column);
				QTableWidgetItem *const headerItem    = table->horizontalHeaderItem(column);
				QVERIFY(headerItem);
				const QString originalHeaderText = headerItem->text();
				headerItem->setText(QStringLiteral("Author whose full heading must remain visible"));
				QCOMPARE(table->columnWidth(column), unsortedWidth);
				table->sortByColumn(column, Qt::AscendingOrder);
				QTRY_COMPARE(header->sortIndicatorSection(), column);
				QVERIFY(header->isSortIndicatorShown());
				QTRY_VERIFY(table->columnWidth(column) > unsortedWidth);
				QVERIFY(table->columnWidth(column) >= header->sectionSizeHint(column));

				headerItem->setText(originalHeaderText);
				table->sortByColumn(0, Qt::AscendingOrder);
				QTRY_COMPARE(table->columnWidth(column), unsortedWidth);
			}

			/**
			 * @brief Verifies clicking a section to sort is not persisted as a manual column resize.
			 */
			void sortClickDoesNotBecomeUserResize()
			{
				clearPluginsDialogSettings();
				const auto    clearSettings = qScopeGuard([] { clearPluginsDialogSettings(); });
				WorldRuntime  runtime;
				PluginsDialog dialog(&runtime, nullptr);
				dialog.show();
				QVERIFY(QTest::qWaitForWindowExposed(&dialog));
				auto *table = dialog.findChild<QTableWidget *>();
				QVERIFY(table);
				QHeaderView *const header = table->horizontalHeader();
				QVERIFY(header);

				constexpr int           column     = 2;
				QTableWidgetItem *const headerItem = table->horizontalHeaderItem(column);
				QVERIFY(headerItem);
				const int widthBeforeSort = table->columnWidth(column);
				headerItem->setText(
				    QStringLiteral("Author whose enlarged heading changes the section width"));
				QCOMPARE(table->columnWidth(column), widthBeforeSort);
				const QPoint clickPosition(header->sectionViewportPosition(column) +
				                               (header->sectionSize(column) / 2),
				                           header->height() / 2);
				QVERIFY(header->sectionsClickable());
				QVERIFY(header->viewport()->rect().contains(clickPosition));
				QTest::mouseMove(header->viewport(), clickPosition);
				QTest::mousePress(header->viewport(), Qt::LeftButton, Qt::NoModifier, clickPosition);
				QTest::mouseRelease(header->viewport(), Qt::LeftButton, Qt::NoModifier, clickPosition);

				QTRY_COMPARE(header->sortIndicatorSection(), column);
				QTRY_VERIFY(table->columnWidth(column) > widthBeforeSort);
				dialog.reject();

				QSettings settings(AppController::instance()->iniFilePath(), QSettings::IniFormat);
				settings.beginGroup(QStringLiteral("PluginsDialog"));
				QCOMPARE(settings.value(QStringLiteral("UsingDefaultColumnWidths")).toBool(), true);
				QVERIFY(!settings.contains(QStringLiteral("PreferredColumnWidths")));
				settings.endGroup();
			}

			/**
			 * @brief Verifies version-two header widths migrate into the new preferred-width metadata.
			 */
			void versionTwoHeaderStateMigratesToPreferredWidths()
			{
				clearPluginsDialogSettings();
				const auto  clearSettings           = qScopeGuard([] { clearPluginsDialogSettings(); });
				const QFont originalApplicationFont = QApplication::font();
				const auto  restoreApplicationFont  = qScopeGuard(
				    [originalApplicationFont] { QApplication::setFont(originalApplicationFont); });
				QTableWidget legacyTable;
				legacyTable.setColumnCount(6);
				QVector<int> expectedWidths;
				expectedWidths.reserve(legacyTable.columnCount());
				for (int column = 0; column < legacyTable.columnCount(); ++column)
				{
					legacyTable.setColumnWidth(column, 170 + (column * 17));
					expectedWidths.push_back(legacyTable.columnWidth(column));
				}
				{
					QSettings settings(AppController::instance()->iniFilePath(), QSettings::IniFormat);
					settings.beginGroup(QStringLiteral("PluginsDialog"));
					settings.setValue(QStringLiteral("HeaderVersion"), 2);
					settings.setValue(QStringLiteral("HeaderState"),
					                  legacyTable.horizontalHeader()->saveState());
					settings.endGroup();
				}

				WorldRuntime  runtime;
				PluginsDialog dialog(&runtime, nullptr);
				dialog.show();
				QVERIFY(QTest::qWaitForWindowExposed(&dialog));
				auto *table = dialog.findChild<QTableWidget *>();
				QVERIFY(table);
				for (int column = 0; column < table->columnCount(); ++column)
					QTRY_COMPARE(table->columnWidth(column), expectedWidths.at(column));

				QFont enlargedFont = dialog.font();
				if (enlargedFont.pointSizeF() > 0.0)
					enlargedFont.setPointSizeF(enlargedFont.pointSizeF() * 6.0);
				else
					enlargedFont.setPixelSize(qMax(1, enlargedFont.pixelSize() * 6));
				QApplication::setFont(enlargedFont);
				QTRY_VERIFY(table->columnWidth(0) >=
				            table->horizontalHeader()->fontMetrics().horizontalAdvance(
				                table->horizontalHeaderItem(0)->text()));
				verifyHeaderLabelsFit(*table);

				QApplication::setFont(originalApplicationFont);
				for (int column = 0; column < table->columnCount(); ++column)
					QTRY_COMPARE(table->columnWidth(column), expectedWidths.at(column));
				dialog.reject();

				QSettings settings(AppController::instance()->iniFilePath(), QSettings::IniFormat);
				settings.beginGroup(QStringLiteral("PluginsDialog"));
				QCOMPARE(settings.value(QStringLiteral("HeaderVersion")).toInt(), 2);
				QCOMPARE(settings.value(QStringLiteral("UsingDefaultColumnWidths")).toBool(), false);
				const QVariantList preferredWidths =
				    settings.value(QStringLiteral("PreferredColumnWidths")).toList();
				QCOMPARE(preferredWidths.size(), expectedWidths.size());
				for (qsizetype index = 0; index < preferredWidths.size(); ++index)
					QCOMPARE(preferredWidths.at(index).toInt(), expectedWidths.at(index));
				settings.endGroup();
			}

			/**
			 * @brief Verifies persisted default columns still follow later font changes.
			 */
			void restoredDefaultColumnWidthsRemainFontResponsive()
			{
				clearPluginsDialogSettings();
				const auto  clearSettings           = qScopeGuard([] { clearPluginsDialogSettings(); });
				const QFont originalApplicationFont = QApplication::font();
				const auto  restoreApplicationFont  = qScopeGuard(
				    [originalApplicationFont] { QApplication::setFont(originalApplicationFont); });
				WorldRuntime runtime;
				{
					PluginsDialog dialog(&runtime, nullptr);
					dialog.show();
					QVERIFY(QTest::qWaitForWindowExposed(&dialog));
					dialog.reject();
				}

				PluginsDialog restoredDialog(&runtime, nullptr);
				restoredDialog.show();
				QVERIFY(QTest::qWaitForWindowExposed(&restoredDialog));
				auto *table = restoredDialog.findChild<QTableWidget *>();
				QVERIFY(table);
				const int programmaticWidth = table->horizontalHeader()->minimumSectionSize();
				table->setColumnWidth(4, programmaticWidth);
				QSignalSpy sectionResizedSpy(table->horizontalHeader(), &QHeaderView::sectionResized);

				QFont      enlargedFont = restoredDialog.font();
				if (enlargedFont.pointSizeF() > 0.0)
					enlargedFont.setPointSizeF(enlargedFont.pointSizeF() * 6.0);
				else
					enlargedFont.setPixelSize(qMax(1, enlargedFont.pixelSize() * 6));
				QApplication::setFont(enlargedFont);

				QTRY_VERIFY(sectionResizedSpy.count() > 0);
				QTRY_VERIFY(table->columnWidth(4) > programmaticWidth);
				QTRY_VERIFY(headerLabelsFit(*table));
				verifyHeaderLabelsFit(*table);
			}

			/**
			 * @brief Verifies metric changes raised during a sizing pass receive a complete follow-up pass.
			 */
			void reentrantFontChangeCompletesSizingRefresh()
			{
				clearPluginsDialogSettings();
				const auto  clearSettings           = qScopeGuard([] { clearPluginsDialogSettings(); });
				const QFont originalApplicationFont = QApplication::font();
				const auto  restoreApplicationFont  = qScopeGuard(
				    [originalApplicationFont] { QApplication::setFont(originalApplicationFont); });
				WorldRuntime  runtime;
				PluginsDialog dialog(&runtime, nullptr);
				dialog.show();
				QVERIFY(QTest::qWaitForWindowExposed(&dialog));
				auto *table = dialog.findChild<QTableWidget *>();
				QVERIFY(table);
				QHeaderView *const header = table->horizontalHeader();
				QVERIFY(header);

				QFont intermediateFont = dialog.font();
				QFont finalFont        = dialog.font();
				if (intermediateFont.pointSizeF() > 0.0)
				{
					intermediateFont.setPointSizeF(intermediateFont.pointSizeF() * 4.0);
					finalFont.setPointSizeF(finalFont.pointSizeF() * 6.0);
				}
				else
				{
					intermediateFont.setPixelSize(qMax(1, intermediateFont.pixelSize() * 4));
					finalFont.setPixelSize(qMax(1, finalFont.pixelSize() * 6));
				}

				bool reentrantChangeMade = false;
				QApplication::setFont(intermediateFont);
				table->setColumnWidth(4, header->minimumSectionSize());
				connect(header, &QHeaderView::sectionResized, &dialog,
				        [&](int, int, int)
				        {
					        if (reentrantChangeMade)
						        return;
					        reentrantChangeMade = true;
					        QApplication::setFont(finalFont);
				        });

				QTRY_VERIFY(reentrantChangeMade);
				for (QPushButton *button : dialog.findChildren<QPushButton *>())
				{
					if (button->isVisible())
						QTRY_VERIFY(button->width() >= button->sizeHint().width());
				}
				verifyHeaderLabelsFit(*table);
			}

			void blacklistedPluginsAreHiddenFromTable()
			{
				WorldRuntime runtime;
				runtime.pluginsMutable().push_back(makePlugin(QStringLiteral("bb6a05ed7534b5db1ed40511"),
				                                              QStringLiteral("Automatic Backup")));
				runtime.pluginsMutable().push_back(
				    makePlugin(QStringLiteral("visible"), QStringLiteral("Visible")));

				PluginsDialog dialog(&runtime, nullptr);
				dialog.show();

				auto *table = dialog.findChild<QTableWidget *>();
				QVERIFY(table);
				QCOMPARE(table->rowCount(), 1);
				QVERIFY(table->item(0, 0));
				QCOMPARE(table->item(0, 0)->text(), QStringLiteral("Visible"));
				QCOMPARE(table->item(0, 0)->data(Qt::UserRole).toString(), QStringLiteral("visible"));
			}

			void tabLeavesPluginTableForActionButtons()
			{
				WorldRuntime runtime;
				runtime.pluginsMutable().push_back(
				    makePlugin(QStringLiteral("plug"), QStringLiteral("Plugin")));

				PluginsDialog dialog(&runtime, nullptr);
				dialog.show();
				QVERIFY(QTest::qWaitForWindowExposed(&dialog));

				auto *table = dialog.findChild<QTableWidget *>();
				QVERIFY(table);
				QPushButton *addButton = findButtonByText(dialog, QStringLiteral("Add..."));
				QVERIFY(addButton);

				table->setFocus();
				QTRY_COMPARE(QApplication::focusWidget(), table);

				QTest::keyClick(table, Qt::Key_Tab);
				QTRY_COMPARE(QApplication::focusWidget(), addButton);
			}

			void enableDisableAndReloadActOnSelectedPlugin()
			{
				WorldRuntime runtime;
				runtime.pluginsMutable().push_back(
				    makePlugin(QStringLiteral("plug"), QStringLiteral("Plugin"), true));

				PluginsDialog dialog(&runtime, nullptr);
				dialog.show();

				auto *table = dialog.findChild<QTableWidget *>();
				QVERIFY(table);
				QCOMPARE(table->rowCount(), 1);
				table->selectRow(0);

				QPushButton *disableButton = findButtonByText(dialog, QStringLiteral("Disable"));
				QPushButton *enableButton  = findButtonByText(dialog, QStringLiteral("Enable"));
				QPushButton *reloadButton  = findButtonByText(dialog, QStringLiteral("ReInstall"));
				QVERIFY(disableButton);
				QVERIFY(enableButton);
				QVERIFY(reloadButton);

				QTest::mouseClick(disableButton, Qt::LeftButton);
				QVERIFY(!runtime.plugins().front().enabled);

				table->selectRow(0);
				QTest::mouseClick(enableButton, Qt::LeftButton);
				QVERIFY(runtime.plugins().front().enabled);

				table->selectRow(0);
				QTest::mouseClick(reloadButton, Qt::LeftButton);
				QCOMPARE(stateFor(&runtime).reloadCalls, 1);
				QCOMPARE(stateFor(&runtime).lastReloadPluginId, QStringLiteral("plug"));
			}
			// NOLINTEND(readability-convert-member-functions-to-static)
	};
} // namespace

QTEST_MAIN(tst_Dialog_Plugins)

#if __has_include("tst_Dialog_Plugins.moc")
#include "tst_Dialog_Plugins.moc"
#endif
