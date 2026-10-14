/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_Dialog_WorldPreferences.cpp
 * Role: QTest coverage for Dialog WorldPreferences behavior.
 */

#include "AppController.h"
#include "WorldDocument.h"
#include "WorldOptions.h"
#include "WorldPreferencesRoutingUtils.h"
#include "WorldRuntime.h"
#include "dialogs/WorldPreferencesDialog.h"
#include "helpers/DialogSizingUtils.h"
#include "helpers/EncodingUtils.h"
#include "helpers/StringUtils.h"
#include "scripting/ScriptingErrors.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDialogButtonBox>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMargins>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyleOptionSpinBox>
#include <QTableWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QtTest/QTest>

/**
 * @brief Narrow test access to new-world runtime initialization.
 */
class AppControllerTestAccess final
{
	public:
		/**
		 * @brief Initializes one runtime through the same path used by File/New.
		 * @param controller Controller under test.
		 * @param runtime Runtime to initialize.
		 */
		static void initializeWorldRuntime(const AppController &controller, WorldRuntime *runtime)
		{
			controller.initializeWorldRuntime(runtime);
		}
};

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

	enum class RuleKind
	{
		Trigger,
		Alias,
		Timer
	};

	struct RuleViewWidgets
	{
			QTableWidget *table{nullptr};
			QTreeWidget  *tree{nullptr};
			QPushButton  *editButton{nullptr};
			QPushButton  *removeButton{nullptr};
			QPushButton  *copyButton{nullptr};
			QPushButton  *pasteButton{nullptr};
			QPushButton  *moveUpButton{nullptr};
			QPushButton  *moveDownButton{nullptr};
			QCheckBox    *treeToggle{nullptr};
	};

	/**
	 * @brief Returns the preferences page containing one rule kind.
	 * @param kind Rule kind to resolve.
	 * @return Corresponding preferences page.
	 */
	WorldPreferencesDialog::Page rulePage(const RuleKind kind)
	{
		switch (kind)
		{
		case RuleKind::Trigger:
			return WorldPreferencesDialog::PageTriggers;
		case RuleKind::Alias:
			return WorldPreferencesDialog::PageAliases;
		case RuleKind::Timer:
			return WorldPreferencesDialog::PageTimers;
		}
		return WorldPreferencesDialog::PageTriggers;
	}

	/**
	 * @brief Returns the first-column heading identifying one rule table.
	 * @param kind Rule kind to resolve.
	 * @return Exact first-column heading.
	 */
	QString ruleTableHeading(const RuleKind kind)
	{
		switch (kind)
		{
		case RuleKind::Trigger:
			return QStringLiteral("Trigger");
		case RuleKind::Alias:
			return QStringLiteral("Alias");
		case RuleKind::Timer:
			return QStringLiteral("Type");
		}
		return {};
	}

	/**
	 * @brief Returns the column carrying the sortable display value for one rule kind.
	 * @param kind Rule kind to resolve.
	 * @return Display-value column.
	 */
	int ruleDisplayColumn(const RuleKind kind)
	{
		return kind == RuleKind::Timer ? 1 : 0;
	}

	/**
	 * @brief Locates the rule views and selection-sensitive buttons for one rule page.
	 * @param dialog Preferences dialog to inspect.
	 * @param kind Rule kind to locate.
	 * @return Located widgets; unavailable entries remain null.
	 */
	RuleViewWidgets findRuleViewWidgets(const WorldPreferencesDialog &dialog, const RuleKind kind)
	{
		RuleViewWidgets result;
		for (QTableWidget *table : dialog.findChildren<QTableWidget *>())
		{
			if (!table || !table->horizontalHeaderItem(0) ||
			    table->horizontalHeaderItem(0)->text() != ruleTableHeading(kind))
				continue;
			result.table = table;
			break;
		}
		if (!result.table)
			return result;

		auto *stack = qobject_cast<QStackedWidget *>(result.table->parentWidget());
		if (!stack)
			return result;
		result.tree   = stack->findChild<QTreeWidget *>(QString(), Qt::FindDirectChildrenOnly);
		QWidget *page = stack->parentWidget();
		if (!page)
			return result;
		for (QPushButton *button : page->findChildren<QPushButton *>())
		{
			if (!button)
				continue;
			if (button->text() == QStringLiteral("&Edit..."))
				result.editButton = button;
			else if (button->text() == QStringLiteral("&Remove"))
				result.removeButton = button;
			else if (button->text() == QStringLiteral("&Copy"))
				result.copyButton = button;
			else if (button->text() == QStringLiteral("&Paste"))
				result.pasteButton = button;
			else if (button->text() == QStringLiteral("Move &Up"))
				result.moveUpButton = button;
			else if (button->text() == QStringLiteral("Move &Down"))
				result.moveDownButton = button;
		}
		for (QCheckBox *checkBox : page->findChildren<QCheckBox *>())
		{
			if (checkBox && checkBox->text() == QStringLiteral("Tree View"))
			{
				result.treeToggle = checkBox;
				break;
			}
		}
		return result;
	}

	/**
	 * @brief Replaces a rule table's selection with the requested rows.
	 * @param table Rule table to update.
	 * @param rows Rows to select.
	 * @param currentRow Row to make current without changing the selection.
	 */
	void selectRuleRows(QTableWidget &table, const QList<int> &rows, const int currentRow)
	{
		QItemSelectionModel *const selection = table.selectionModel();
		QVERIFY(selection);
		table.clearSelection();
		for (const int row : rows)
		{
			QVERIFY(row >= 0);
			QVERIFY(row < table.rowCount());
			selection->select(table.model()->index(row, 0),
			                  QItemSelectionModel::Select | QItemSelectionModel::Rows);
		}
		selection->setCurrentIndex(table.model()->index(currentRow, 0), QItemSelectionModel::NoUpdate);
	}

	/**
	 * @brief Returns selected rule identities from a table in display order.
	 * @param table Rule table to inspect.
	 * @return Selected runtime identities.
	 */
	QList<quint64> selectedRuleIds(const QTableWidget &table)
	{
		QList<quint64> result;
		for (int row = 0; row < table.rowCount(); ++row)
		{
			const QTableWidgetItem *const item = table.item(row, 0);
			if (item && item->isSelected())
				result.push_back(item->data(Qt::UserRole).toULongLong());
		}
		return result;
	}

	/**
	 * @brief Returns selected rule identities from a grouped tree in display order.
	 * @param tree Rule tree to inspect.
	 * @return Selected runtime identities.
	 */
	QList<quint64> selectedRuleIds(const QTreeWidget &tree)
	{
		QList<quint64> result;
		for (int groupIndex = 0; groupIndex < tree.topLevelItemCount(); ++groupIndex)
		{
			const QTreeWidgetItem *const group = tree.topLevelItem(groupIndex);
			for (int childIndex = 0; group && childIndex < group->childCount(); ++childIndex)
			{
				const QTreeWidgetItem *const child = group->child(childIndex);
				if (child && child->isSelected())
					result.push_back(child->data(0, Qt::UserRole).toULongLong());
			}
		}
		return result;
	}

	/**
	 * @brief Replaces a grouped rule tree's selection with the requested identities.
	 * @param tree Rule tree to update.
	 * @param runtimeIds Runtime identities to select.
	 * @param currentRuntimeId Identity to make current without changing the selection.
	 */
	void selectRuleTreeItems(QTreeWidget &tree, const QList<quint64> &runtimeIds,
	                         const quint64 currentRuntimeId)
	{
		tree.clearSelection();
		QTreeWidgetItem *current = nullptr;
		for (int groupIndex = 0; groupIndex < tree.topLevelItemCount(); ++groupIndex)
		{
			QTreeWidgetItem *const group = tree.topLevelItem(groupIndex);
			for (int childIndex = 0; group && childIndex < group->childCount(); ++childIndex)
			{
				QTreeWidgetItem *const child     = group->child(childIndex);
				const quint64          runtimeId = child ? child->data(0, Qt::UserRole).toULongLong() : 0;
				if (!runtimeIds.contains(runtimeId))
					continue;
				child->setSelected(true);
				if (runtimeId == currentRuntimeId)
					current = child;
			}
		}
		QVERIFY(current);
		tree.setCurrentItem(current, 0, QItemSelectionModel::NoUpdate);
	}

	/**
	 * @brief Finds a grouped rule-tree parent by its exact group name.
	 * @param tree Tree to search.
	 * @param groupName Group name to match.
	 * @return Matching group item, or `nullptr` when absent.
	 */
	QTreeWidgetItem *findRuleGroup(const QTreeWidget &tree, const QString &groupName)
	{
		for (int index = 0; index < tree.topLevelItemCount(); ++index)
		{
			QTreeWidgetItem *group = tree.topLevelItem(index);
			if (group && group->text(0) == groupName)
				return group;
		}
		return nullptr;
	}

	/**
	 * @brief Finds a rule-table row carrying one runtime identity.
	 * @param table Rule table to inspect.
	 * @param runtimeId Runtime identity to locate.
	 * @return Matching row, or `-1` when absent.
	 */
	int findRuleRowByRuntimeId(const QTableWidget &table, const quint64 runtimeId)
	{
		for (int row = 0; row < table.rowCount(); ++row)
		{
			bool ok = false;
			if (const quint64 candidate = table.item(row, 0)->data(Qt::UserRole).toULongLong(&ok);
			    ok && candidate == runtimeId)
				return row;
		}
		return -1;
	}

	/**
	 * @brief Selects one runtime identity in the currently displayed rule view.
	 * @param widgets Rule views to update.
	 * @param treeMode Whether the grouped tree is displayed.
	 * @param runtimeId Runtime identity to select.
	 */
	void selectRuleInActiveView(const RuleViewWidgets &widgets, const bool treeMode, const quint64 runtimeId)
	{
		if (treeMode)
			selectRuleTreeItems(*widgets.tree, {runtimeId}, runtimeId);
		else
		{
			const int row = findRuleRowByRuntimeId(*widgets.table, runtimeId);
			QVERIFY(row >= 0);
			selectRuleRows(*widgets.table, {row}, row);
		}
	}

	/**
	 * @brief Populates one runtime with deliberately storage-unsorted rule fixtures.
	 * @param runtime Runtime receiving the fixtures.
	 * @param kind Rule kind to populate.
	 * @param treeMode Enable grouped tree presentation when `true`.
	 * @param includeSoleGroup Add a second group containing one rule when `true`.
	 */
	void populateRuleSelectionFixtures(WorldRuntime &runtime, const RuleKind kind, const bool treeMode,
	                                   const bool includeSoleGroup)
	{
		const QString treeValue = treeMode ? QStringLiteral("1") : QStringLiteral("0");
		switch (kind)
		{
		case RuleKind::Trigger:
		{
			auto makeTrigger = [](const QString &match, const QString &group)
			{
				WorldRuntime::Trigger trigger;
				trigger.attributes.insert(QStringLiteral("match"), match);
				trigger.attributes.insert(QStringLiteral("group"), group);
				return trigger;
			};
			QList<WorldRuntime::Trigger> triggers = {
			    makeTrigger(QStringLiteral("Mike"), QStringLiteral("primary")),
			    makeTrigger(QStringLiteral("Zulu"), QStringLiteral("primary")),
			    makeTrigger(QStringLiteral("Alpha"), QStringLiteral("primary"))};
			if (includeSoleGroup)
				triggers.push_back(makeTrigger(QStringLiteral("Only"), QStringLiteral("sole")));
			runtime.setTriggers(triggers);
			runtime.setWorldAttribute(QStringLiteral("treeview_triggers"), treeValue);
			break;
		}
		case RuleKind::Alias:
		{
			auto makeAlias = [](const QString &match, const QString &group)
			{
				WorldRuntime::Alias alias;
				alias.attributes.insert(QStringLiteral("match"), match);
				alias.attributes.insert(QStringLiteral("group"), group);
				return alias;
			};
			QList<WorldRuntime::Alias> aliases = {
			    makeAlias(QStringLiteral("Mike"), QStringLiteral("primary")),
			    makeAlias(QStringLiteral("Zulu"), QStringLiteral("primary")),
			    makeAlias(QStringLiteral("Alpha"), QStringLiteral("primary"))};
			if (includeSoleGroup)
				aliases.push_back(makeAlias(QStringLiteral("Only"), QStringLiteral("sole")));
			runtime.setAliases(aliases);
			runtime.setWorldAttribute(QStringLiteral("treeview_aliases"), treeValue);
			break;
		}
		case RuleKind::Timer:
		{
			auto makeTimer = [](const int hour, const QString &group)
			{
				WorldRuntime::Timer timer;
				timer.attributes.insert(QStringLiteral("at_time"), QStringLiteral("1"));
				timer.attributes.insert(QStringLiteral("hour"), QString::number(hour));
				timer.attributes.insert(QStringLiteral("group"), group);
				return timer;
			};
			QList<WorldRuntime::Timer> timers = {makeTimer(12, QStringLiteral("primary")),
			                                     makeTimer(18, QStringLiteral("primary")),
			                                     makeTimer(6, QStringLiteral("primary"))};
			if (includeSoleGroup)
				timers.push_back(makeTimer(23, QStringLiteral("sole")));
			runtime.setTimers(timers);
			runtime.setWorldAttribute(QStringLiteral("treeview_timers"), treeValue);
			break;
		}
		}
	}

	/**
	 * @brief Returns a unique serialized attribute for one fixture rule.
	 * @param runtime Runtime containing the fixture.
	 * @param kind Rule kind to inspect.
	 * @param runtimeId Runtime identity to resolve.
	 * @return Attribute fragment expected in copied XML.
	 */
	QString copiedRuleIdentity(const WorldRuntime &runtime, const RuleKind kind, const quint64 runtimeId)
	{
		switch (kind)
		{
		case RuleKind::Trigger:
			for (const auto &rule : runtime.triggers())
				if (rule.runtimeId == runtimeId)
					return QStringLiteral("match=\"%1\"").arg(rule.attributes.value(QStringLiteral("match")));
			break;
		case RuleKind::Alias:
			for (const auto &rule : runtime.aliases())
				if (rule.runtimeId == runtimeId)
					return QStringLiteral("match=\"%1\"").arg(rule.attributes.value(QStringLiteral("match")));
			break;
		case RuleKind::Timer:
			for (const auto &rule : runtime.timers())
				if (rule.runtimeId == runtimeId)
					return QStringLiteral("hour=\"%1\"").arg(rule.attributes.value(QStringLiteral("hour")));
			break;
		}
		return {};
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

			/**
			 * @brief Verifies construction and initial selection populate only the requested page.
			 */
			void initialSelectionPopulatesOnlyRequestedPage()
			{
				WorldRuntime runtime;
				runtime.applyDefaultWorldOptions();
				runtime.setWorldAttribute(QStringLiteral("command_stack_character"), QStringLiteral("::"));

				WorldPreferencesDialog dialog(&runtime, nullptr);
				auto *const            pages =
				    dialog.findChild<QStackedWidget *>(QStringLiteral("worldPreferencesPages"));
				QVERIFY(pages);
				for (int index = 0; index < pages->count(); ++index)
					QVERIFY(!pages->widget(index)->property("qmud_page_populated").toBool());

				dialog.setInitialPage(WorldPreferencesDialog::PageCommands);
				QVERIFY(pages->widget(WorldPreferencesDialog::PageCommands)
				            ->property("qmud_page_populated")
				            .toBool());
				QVERIFY(!pages->widget(WorldPreferencesDialog::PageGeneral)
				             ->property("qmud_page_populated")
				             .toBool());
				QVERIFY(!pages->widget(WorldPreferencesDialog::PageLogging)
				             ->property("qmud_page_populated")
				             .toBool());
				QVERIFY(!pages->widget(WorldPreferencesDialog::PageInfo)
				             ->property("qmud_page_populated")
				             .toBool());

				auto *const separator =
				    dialog.findChild<QLineEdit *>(QStringLiteral("commandStackCharacterEdit"));
				QVERIFY(separator);
				QCOMPARE(separator->text(), QStringLiteral("::"));
			}

			/**
			 * @brief Verifies the existing per-world page state is persisted by world identity.
			 */
			void lastPagePersistenceIsPerWorldAndRemovesGlobalIndex()
			{
				QVERIFY(!AppController::instance());
				AppController controller;
				const QString worldId = QStringLiteral("0123456789abcdef01234567");
				{
					QSettings settings(controller.iniFilePath(), QSettings::IniFormat);
					settings.beginGroup(QStringLiteral("WorldPreferencesDialog"));
					settings.setValue(QStringLiteral("PageIndex"),
					                  static_cast<int>(WorldPreferencesDialog::PageLogging));
					settings.beginGroup(QStringLiteral("LastPageByWorld"));
					settings.remove(worldId);
					settings.endGroup();
					settings.endGroup();
				}

				WorldRuntime runtime;
				runtime.applyDefaultWorldOptions();
				runtime.setWorldAttribute(QStringLiteral("id"), worldId);
				runtime.setWorldAttribute(QStringLiteral("name"), QStringLiteral("Persistence Test"));
				runtime.setWorldAttribute(QStringLiteral("site"), QStringLiteral("example.invalid"));
				runtime.setWorldAttribute(QStringLiteral("port"), QStringLiteral("4000"));
				{
					WorldPreferencesDialog dialog(&runtime, nullptr);
					dialog.setInitialPage(WorldPreferencesDialog::PageTimers);
					dialog.accept();
				}

				{
					QSettings settings(controller.iniFilePath(), QSettings::IniFormat);
					settings.beginGroup(QStringLiteral("WorldPreferencesDialog"));
					QVERIFY(!settings.contains(QStringLiteral("PageIndex")));
					settings.beginGroup(QStringLiteral("LastPageByWorld"));
					QCOMPARE(settings.value(worldId).toInt(),
					         static_cast<int>(WorldPreferencesDialog::PageTimers));
					settings.endGroup();
					settings.endGroup();
				}

				runtime.setLastPreferencesPage(WorldPreferencesDialog::PageGeneral);
				WorldPreferencesDialog restored(&runtime, nullptr);
				QCOMPARE(runtime.lastPreferencesPage(), static_cast<int>(WorldPreferencesDialog::PageTimers));

				QSettings settings(controller.iniFilePath(), QSettings::IniFormat);
				settings.beginGroup(QStringLiteral("WorldPreferencesDialog"));
				settings.beginGroup(QStringLiteral("LastPageByWorld"));
				settings.remove(worldId);
				settings.endGroup();
				settings.endGroup();
			}

			/**
			 * @brief Verifies acceptance neither populates hidden pages nor writes unchanged options.
			 */
			void acceptanceCommitsOnlyChangedOptions()
			{
				WorldRuntime runtime;
				runtime.applyDefaultWorldOptions();
				runtime.setWorldAttribute(QStringLiteral("name"), QStringLiteral("Delta Test"));
				runtime.setWorldAttribute(QStringLiteral("site"), QStringLiteral("example.invalid"));
				runtime.setWorldAttribute(QStringLiteral("port"), QStringLiteral("4000"));
				runtime.setWorldAttribute(QStringLiteral("auto_log_file_name"), QStringLiteral("keep.log"));

				WorldPreferencesDialog dialog(&runtime, nullptr);
				dialog.setInitialPage(WorldPreferencesDialog::PageCommands);
				auto *const pages =
				    dialog.findChild<QStackedWidget *>(QStringLiteral("worldPreferencesPages"));
				QVERIFY(pages);
				auto *const separator =
				    dialog.findChild<QLineEdit *>(QStringLiteral("commandStackCharacterEdit"));
				QCheckBox *const enabled =
				    findCheckBoxByText(dialog, QStringLiteral("&Command Stacking Using:"));
				QVERIFY(separator);
				QVERIFY(enabled);

				const QString concurrentHistoryValue = QStringLiteral("777");
				runtime.setWorldAttribute(QStringLiteral("history_lines"), concurrentHistoryValue);
				enabled->setChecked(true);
				separator->setText(QStringLiteral("::"));
				dialog.accept();

				QVERIFY(!pages->widget(WorldPreferencesDialog::PageGeneral)
				             ->property("qmud_page_populated")
				             .toBool());
				QVERIFY(!pages->widget(WorldPreferencesDialog::PageLogging)
				             ->property("qmud_page_populated")
				             .toBool());
				QCOMPARE(runtime.worldAttributes().value(QStringLiteral("enable_command_stack")),
				         QStringLiteral("1"));
				QCOMPARE(runtime.worldAttributes().value(QStringLiteral("command_stack_character")),
				         QStringLiteral("::"));
				QCOMPARE(runtime.worldAttributes().value(QStringLiteral("history_lines")),
				         concurrentHistoryValue);
				QCOMPARE(runtime.worldAttributes().value(QStringLiteral("auto_log_file_name")),
				         QStringLiteral("keep.log"));
			}

			/**
			 * @brief Verifies File/New enables global fonts without enabling configured collection files.
			 */
			void newWorldEnablesDefaultFontsOnly()
			{
				QVERIFY(!AppController::instance());
				AppController     controller;
				const QStringList collectionPreferences = {
				    QStringLiteral("DefaultTriggersFile"), QStringLiteral("DefaultAliasesFile"),
				    QStringLiteral("DefaultTimersFile"), QStringLiteral("DefaultMacrosFile"),
				    QStringLiteral("DefaultColoursFile")};
				QMap<QString, QString> previousValues;
				for (const QString &preference : collectionPreferences)
				{
					previousValues.insert(preference, controller.getGlobalOption(preference).toString());
					controller.setGlobalOptionString(preference, QStringLiteral("configured-default.xml"));
				}
				const QString previousInputFont =
				    controller.getGlobalOption(QStringLiteral("DefaultInputFont")).toString();
				const QString previousOutputFont =
				    controller.getGlobalOption(QStringLiteral("DefaultOutputFont")).toString();
				const auto cleanup = qScopeGuard(
				    [&controller, previousValues, previousInputFont, previousOutputFont]
				    {
					    for (auto it = previousValues.cbegin(); it != previousValues.cend(); ++it)
						    controller.setGlobalOptionString(it.key(), it.value());
					    controller.setGlobalOptionString(QStringLiteral("DefaultInputFont"),
					                                     previousInputFont);
					    controller.setGlobalOptionString(QStringLiteral("DefaultOutputFont"),
					                                     previousOutputFont);
				    });
				controller.setGlobalOptionString(QStringLiteral("DefaultInputFont"),
				                                 QStringLiteral("Input Font"));
				controller.setGlobalOptionString(QStringLiteral("DefaultOutputFont"),
				                                 QStringLiteral("Output Font"));

				WorldRuntime runtime;
				AppControllerTestAccess::initializeWorldRuntime(controller, &runtime);
				for (const QString &optionName :
				     {QStringLiteral("use_default_triggers"), QStringLiteral("use_default_aliases"),
				      QStringLiteral("use_default_timers"), QStringLiteral("use_default_macros"),
				      QStringLiteral("use_default_colours")})
				{
					QVERIFY(!qmudIsEnabledFlag(runtime.worldAttributes().value(optionName)));
				}
				QVERIFY(qmudIsEnabledFlag(
				    runtime.worldAttributes().value(QStringLiteral("use_default_input_font"))));
				QVERIFY(qmudIsEnabledFlag(
				    runtime.worldAttributes().value(QStringLiteral("use_default_output_font"))));
			}

			/**
			 * @brief Verifies enabling default fonts preserves the world's custom font attributes.
			 */
			void defaultFontOverridesPreserveCustomWorldFonts()
			{
				QVERIFY(!AppController::instance());
				AppController controller;
				const QString previousInputFont =
				    controller.getGlobalOption(QStringLiteral("DefaultInputFont")).toString();
				const QString previousOutputFont =
				    controller.getGlobalOption(QStringLiteral("DefaultOutputFont")).toString();
				const auto cleanup = qScopeGuard(
				    [&controller, previousInputFont, previousOutputFont]
				    {
					    controller.setGlobalOptionString(QStringLiteral("DefaultInputFont"),
					                                     previousInputFont);
					    controller.setGlobalOptionString(QStringLiteral("DefaultOutputFont"),
					                                     previousOutputFont);
				    });
				controller.setGlobalOptionString(QStringLiteral("DefaultInputFont"),
				                                 QStringLiteral("Global Input Font"));
				controller.setGlobalOptionString(QStringLiteral("DefaultOutputFont"),
				                                 QStringLiteral("Global Output Font"));

				WorldRuntime runtime;
				runtime.applyDefaultWorldOptions();
				runtime.setWorldAttribute(QStringLiteral("input_font_name"), QStringLiteral("Custom Input"));
				runtime.setWorldAttribute(QStringLiteral("input_font_height"), QStringLiteral("13"));
				runtime.setWorldAttribute(QStringLiteral("output_font_name"),
				                          QStringLiteral("Custom Output"));
				runtime.setWorldAttribute(QStringLiteral("output_font_height"), QStringLiteral("14"));
				runtime.setWorldFileModified(false);

				WorldPreferencesDialog dialog(&runtime, nullptr);
				dialog.setInitialPage(WorldPreferencesDialog::PageCommands);
				QCheckBox *const useDefaultInput =
				    findCheckBoxByText(dialog, QStringLiteral("O&verride With Default"));
				QVERIFY(useDefaultInput);
				useDefaultInput->setChecked(true);
				dialog.setInitialPage(WorldPreferencesDialog::PageOutput);
				QCheckBox *const useDefaultOutput =
				    findCheckBoxByText(dialog, QStringLiteral("Override with default"));
				QVERIFY(useDefaultOutput);
				useDefaultOutput->setChecked(true);
				dialog.accept();

				QVERIFY(qmudIsEnabledFlag(
				    runtime.worldAttributes().value(QStringLiteral("use_default_input_font"))));
				QVERIFY(qmudIsEnabledFlag(
				    runtime.worldAttributes().value(QStringLiteral("use_default_output_font"))));
				QCOMPARE(runtime.worldAttributes().value(QStringLiteral("input_font_name")),
				         QStringLiteral("Custom Input"));
				QCOMPARE(runtime.worldAttributes().value(QStringLiteral("input_font_height")),
				         QStringLiteral("13"));
				QCOMPARE(runtime.worldAttributes().value(QStringLiteral("output_font_name")),
				         QStringLiteral("Custom Output"));
				QCOMPARE(runtime.worldAttributes().value(QStringLiteral("output_font_height")),
				         QStringLiteral("14"));
			}

			/**
			 * @brief Verifies default overrides apply at runtime while saved world data remains restorable.
			 */
			void defaultOverridesApplyAtRuntimeAndPreserveWorldData()
			{
				QVERIFY(!AppController::instance());
				AppController controller;
				const QString defaultsPath =
				    QDir(QCoreApplication::applicationDirPath())
				        .filePath(QStringLiteral("world_preferences_default_transition_test.qdt"));
				const QString worldPath =
				    QDir(QCoreApplication::applicationDirPath())
				        .filePath(QStringLiteral("world_preferences_override_persistence_test.qdl"));
				const QString configuredDefaultsPath = QDir::current().relativeFilePath(defaultsPath);
				const QString previousTriggersFile =
				    controller.getGlobalOption(QStringLiteral("DefaultTriggersFile")).toString();
				const QString previousAliasesFile =
				    controller.getGlobalOption(QStringLiteral("DefaultAliasesFile")).toString();
				const auto cleanup = qScopeGuard(
				    [&controller, defaultsPath, worldPath, previousTriggersFile, previousAliasesFile]
				    {
					    controller.setGlobalOptionString(QStringLiteral("DefaultTriggersFile"),
					                                     previousTriggersFile);
					    controller.setGlobalOptionString(QStringLiteral("DefaultAliasesFile"),
					                                     previousAliasesFile);
					    QFile::remove(defaultsPath);
					    QFile::remove(worldPath);
				    });
				QFile::remove(defaultsPath);
				QFile::remove(worldPath);
				QFile defaultsFile(defaultsPath);
				QVERIFY(defaultsFile.open(QIODevice::WriteOnly | QIODevice::Text));
				QVERIFY(defaultsFile.write(QByteArrayLiteral(
				            "<qmud><world world_file_version=\"1000\" qmud_version=\"10.01\" "
				            "date_saved=\"2026-10-08 12:00:00\" id=\"1234567890abcdef12345678\" "
				            "name=\"Defaults\" site=\"example.invalid\" port=\"4000\"></world>"
				            "<triggers><trigger name=\"Default trigger\" match=\"default\">"
				            "<send>trigger default</send></trigger></triggers>"
				            "<aliases><alias name=\"Default alias\" match=\"default\">"
				            "<send>alias default</send></alias></aliases></qmud>")) > 0);
				defaultsFile.close();
				controller.setGlobalOptionString(QStringLiteral("DefaultTriggersFile"),
				                                 configuredDefaultsPath);
				controller.setGlobalOptionString(QStringLiteral("DefaultAliasesFile"),
				                                 configuredDefaultsPath);
				const QString configuredTriggers =
				    controller.getGlobalOption(QStringLiteral("DefaultTriggersFile")).toString();
				QVERIFY2(QFileInfo::exists(controller.makeAbsolutePath(configuredTriggers)),
				         qPrintable(
				             QStringLiteral("Configured trigger path: %1; resolved path: %2")
				                 .arg(configuredTriggers, controller.makeAbsolutePath(configuredTriggers))));

				WorldRuntime runtime;
				runtime.setStartupDirectory(QCoreApplication::applicationDirPath());
				runtime.applyDefaultWorldOptions();
				runtime.setWorldAttribute(QStringLiteral("name"), QStringLiteral("Defaults Test"));
				runtime.setWorldAttribute(QStringLiteral("site"), QStringLiteral("example.invalid"));
				runtime.setWorldAttribute(QStringLiteral("port"), QStringLiteral("4000"));
				runtime.setWorldAttribute(QStringLiteral("use_default_triggers"), QStringLiteral("0"));
				runtime.setWorldAttribute(QStringLiteral("use_default_aliases"), QStringLiteral("0"));
				WorldRuntime::Trigger liveTrigger;
				liveTrigger.attributes.insert(QStringLiteral("name"), QStringLiteral("Live trigger"));
				liveTrigger.attributes.insert(QStringLiteral("match"), QStringLiteral("live"));
				WorldRuntime::Alias liveAlias;
				liveAlias.attributes.insert(QStringLiteral("name"), QStringLiteral("Live alias"));
				liveAlias.attributes.insert(QStringLiteral("match"), QStringLiteral("live"));
				runtime.setTriggers({liveTrigger});
				runtime.setAliases({liveAlias});
				QString saveError;
				QVERIFY2(runtime.saveWorldFile(worldPath, &saveError), qPrintable(saveError));
				runtime.setWorldFilePath(worldPath);
				runtime.setWorldAttribute(QStringLiteral("use_default_aliases"), QStringLiteral("1"));

				WorldPreferencesDialog triggerDialog(&runtime, nullptr);
				triggerDialog.setInitialPage(WorldPreferencesDialog::PageTriggers);
				const RuleViewWidgets triggerWidgets = findRuleViewWidgets(triggerDialog, RuleKind::Trigger);
				QVERIFY(triggerWidgets.pasteButton);
				QClipboard *const clipboard = QGuiApplication::clipboard();
				QVERIFY(clipboard);
				clipboard->setText(QStringLiteral(
				    "<qmud><triggers><trigger name=\"Discarded enabling edit\" "
				    "match=\"discard-enable\"><send>discard</send></trigger></triggers></qmud>"));
				triggerWidgets.pasteButton->click();
				QCOMPARE(runtime.triggers().size(), 1);
				QCOMPARE(triggerWidgets.table->rowCount(), 2);
				QCheckBox *const useDefaultTriggers =
				    findCheckBoxByText(triggerDialog, QStringLiteral("O&verride with default triggers"));
				QVERIFY(useDefaultTriggers);
				QVERIFY(!useDefaultTriggers->isChecked());
				QVERIFY(QMetaObject::invokeMethod(
				    &triggerDialog,
				    []
				    {
					    if (auto *const message =
					            qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
					        message)
						    message->button(QMessageBox::No)->click();
				    },
				    Qt::QueuedConnection));
				useDefaultTriggers->setChecked(true);
				QVERIFY(!useDefaultTriggers->isChecked());
				QCOMPARE(triggerWidgets.table->rowCount(), 2);
				QString confirmationText;
				QVERIFY(QMetaObject::invokeMethod(
				    &triggerDialog,
				    [&confirmationText]
				    {
					    auto *const message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
					    if (!message)
						    return;
					    confirmationText = message->text();
					    message->button(QMessageBox::Yes)->click();
				    },
				    Qt::QueuedConnection));
				useDefaultTriggers->setChecked(true);
				QCOMPARE(triggerWidgets.table->rowCount(), 1);
				QVERIFY(!triggerWidgets.table->findItems(QStringLiteral("Default trigger"), Qt::MatchExactly)
				             .isEmpty());
				triggerDialog.accept();
				QVERIFY(confirmationText.contains(QStringLiteral("temporarily replace")));
				QVERIFY(confirmationText.contains(QStringLiteral("will be preserved")));
				QVERIFY(!confirmationText.contains(QStringLiteral("PERMANENTLY")));
				QCOMPARE(runtime.worldAttributes().value(QStringLiteral("use_default_triggers")),
				         QStringLiteral("1"));
				QStringList triggerNames;
				for (const WorldRuntime::Trigger &trigger : runtime.triggers())
					triggerNames.push_back(trigger.attributes.value(QStringLiteral("name")));
				QVERIFY2(triggerNames.contains(QStringLiteral("Default trigger")),
				         qPrintable(QStringLiteral("Loaded trigger names: %1").arg(triggerNames.join(", "))));
				QCOMPARE(runtime.aliases().size(), 1);
				QCOMPARE(runtime.aliases().constFirst().attributes.value(QStringLiteral("name")),
				         QStringLiteral("Live alias"));

				QVERIFY2(runtime.saveWorldFile(worldPath, &saveError), qPrintable(saveError));
				WorldDocument savedWorld;
				QVERIFY2(savedWorld.loadFromFile(worldPath), qPrintable(savedWorld.errorString()));
				QCOMPARE(savedWorld.triggers().size(), 1);
				QCOMPARE(savedWorld.triggers().constFirst().attributes.value(QStringLiteral("name")),
				         QStringLiteral("Live trigger"));
				QCOMPARE(savedWorld.aliases().constFirst().attributes.value(QStringLiteral("name")),
				         QStringLiteral("Live alias"));
				QCOMPARE(savedWorld.worldAttributes().value(QStringLiteral("use_default_triggers")),
				         QStringLiteral("y"));

				WorldPreferencesDialog restoreDialog(&runtime, nullptr);
				restoreDialog.setInitialPage(WorldPreferencesDialog::PageTriggers);
				const RuleViewWidgets restoreWidgets = findRuleViewWidgets(restoreDialog, RuleKind::Trigger);
				QVERIFY(restoreWidgets.pasteButton);
				QCheckBox *const restoreTriggers =
				    findCheckBoxByText(restoreDialog, QStringLiteral("O&verride with default triggers"));
				QVERIFY(restoreTriggers);
				QVERIFY(restoreTriggers->isChecked());
				restoreTriggers->setChecked(false);
				QVERIFY(!restoreTriggers->isChecked());
				QVERIFY(restoreWidgets.pasteButton->isEnabled());
				clipboard->setText(
				    QStringLiteral("<qmud><triggers><trigger name=\"Post-disable edit\" "
				                   "match=\"post-disable\"><send>kept</send></trigger></triggers></qmud>"));
				restoreWidgets.pasteButton->click();
				QVERIFY(!restoreTriggers->isChecked());
				QCOMPARE(restoreWidgets.table->rowCount(), 2);
				QVERIFY(QMetaObject::invokeMethod(
				    &restoreDialog,
				    []
				    {
					    if (auto *const message =
					            qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
					        message)
						    message->button(QMessageBox::Yes)->click();
				    },
				    Qt::QueuedConnection));
				restoreTriggers->setChecked(true);
				QCOMPARE(restoreWidgets.table->rowCount(), 1);
				restoreTriggers->setChecked(false);
				QCOMPARE(restoreWidgets.table->rowCount(), 1);
				restoreWidgets.pasteButton->click();
				QCOMPARE(runtime.triggers().size(), 1);
				restoreDialog.accept();
				QCOMPARE(restoreDialog.result(), static_cast<int>(QDialog::Accepted));
				QCOMPARE(runtime.worldAttributes().value(QStringLiteral("use_default_triggers")),
				         QStringLiteral("0"));
				QCOMPARE(runtime.triggers().size(), 2);
				QStringList restoredTriggerNames;
				for (const WorldRuntime::Trigger &trigger : runtime.triggers())
					restoredTriggerNames.push_back(trigger.attributes.value(QStringLiteral("name")));
				QVERIFY(restoredTriggerNames.contains(QStringLiteral("Live trigger")));
				QVERIFY(restoredTriggerNames.contains(QStringLiteral("Post-disable edit")));

				WorldPreferencesDialog aliasDialog(&runtime, nullptr);
				aliasDialog.setInitialPage(WorldPreferencesDialog::PageAliases);
				QCheckBox *const useDefaultAliases =
				    findCheckBoxByText(aliasDialog, QStringLiteral("O&verride with default aliases"));
				QVERIFY(useDefaultAliases);
				QVERIFY(useDefaultAliases->isChecked());
				aliasDialog.accept();
				QCOMPARE(runtime.aliases().size(), 1);
				QCOMPARE(runtime.aliases().constFirst().attributes.value(QStringLiteral("name")),
				         QStringLiteral("Live alias"));
			}

			/**
			 * @brief Verifies refreshing a restored macro editor does not re-enable its runtime override.
			 */
			void disabledMacroOverrideSurvivesTableRefresh()
			{
				QVERIFY(!AppController::instance());
				AppController controller;
				const QString worldPath =
				    QDir(QCoreApplication::applicationDirPath())
				        .filePath(QStringLiteral("world_preferences_macro_refresh_test.qdl"));
				const auto cleanup = qScopeGuard([worldPath] { QFile::remove(worldPath); });
				QFile::remove(worldPath);

				WorldRuntime runtime;
				runtime.setStartupDirectory(QCoreApplication::applicationDirPath());
				runtime.applyDefaultWorldOptions();
				WorldRuntime::Macro persistedMacro;
				persistedMacro.attributes.insert(QStringLiteral("name"), QStringLiteral("F1"));
				persistedMacro.attributes.insert(QStringLiteral("type"), QStringLiteral("send_now"));
				persistedMacro.children.insert(QStringLiteral("send"), QStringLiteral("persisted macro"));
				runtime.setMacros({persistedMacro});
				QString saveError;
				QVERIFY2(runtime.saveWorldFile(worldPath, &saveError), qPrintable(saveError));
				runtime.setWorldAttribute(QStringLiteral("use_default_macros"), QStringLiteral("1"));
				WorldRuntime::Macro runtimeMacro = persistedMacro;
				runtimeMacro.children.insert(QStringLiteral("send"), QStringLiteral("runtime default"));
				runtime.setMacros({runtimeMacro});
				runtime.setWorldFileModified(false);

				WorldPreferencesDialog dialog(&runtime, nullptr);
				dialog.setInitialPage(WorldPreferencesDialog::PageMacros);
				QCheckBox *const useDefaultMacros =
				    findCheckBoxByText(dialog, QStringLiteral("Override with default macros"));
				QVERIFY(useDefaultMacros);
				QVERIFY(useDefaultMacros->isChecked());
				useDefaultMacros->setChecked(false);
				QVERIFY(!useDefaultMacros->isChecked());

				QTableWidget *macrosTable = nullptr;
				for (QTableWidget *table : dialog.findChildren<QTableWidget *>())
				{
					if (table->horizontalHeaderItem(0) &&
					    table->horizontalHeaderItem(0)->text() == QStringLiteral("Macro Name"))
					{
						macrosTable = table;
						break;
					}
				}
				QVERIFY(macrosTable);
				QCOMPARE(macrosTable->rowCount(), 1);
				QCOMPARE(macrosTable->item(0, 2)->text(), QStringLiteral("persisted macro"));
				macrosTable->horizontalHeader()->sectionClicked(0);
				QVERIFY(!useDefaultMacros->isChecked());
				QCOMPARE(macrosTable->item(0, 2)->text(), QStringLiteral("persisted macro"));
			}

			/**
			 * @brief Verifies an active colour override disables every palette mutation control.
			 */
			void customColourPageInitializesSharedOverrideState()
			{
				WorldRuntime runtime;
				runtime.applyDefaultWorldOptions();
				runtime.setWorldAttribute(QStringLiteral("use_default_colours"), QStringLiteral("1"));

				WorldPreferencesDialog dialog(&runtime, nullptr);
				dialog.setInitialPage(WorldPreferencesDialog::PageCustomColours);
				QCheckBox *const useDefaultColours =
				    findCheckBoxByText(dialog, QStringLiteral("Override with default colours"));
				QVERIFY(useDefaultColours);
				QVERIFY(useDefaultColours->isChecked());
				for (int index = 0; index < MAX_CUSTOM; ++index)
				{
					auto *name =
					    dialog.findChild<QLineEdit *>(QStringLiteral("customColourName%1").arg(index));
					auto *text =
					    dialog.findChild<QPushButton *>(QStringLiteral("customTextSwatch%1").arg(index));
					auto *back =
					    dialog.findChild<QPushButton *>(QStringLiteral("customBackSwatch%1").arg(index));
					QVERIFY(name);
					QVERIFY(text);
					QVERIFY(back);
					QVERIFY(!name->isEnabled());
					QVERIFY(!text->isEnabled());
					QVERIFY(!back->isEnabled());
				}
				for (int index = 0; index < 8; ++index)
				{
					auto *normal =
					    dialog.findChild<QPushButton *>(QStringLiteral("ansiNormalSwatch%1").arg(index));
					auto *bold =
					    dialog.findChild<QPushButton *>(QStringLiteral("ansiBoldSwatch%1").arg(index));
					QVERIFY(normal);
					QVERIFY(bold);
					QVERIFY(!normal->isEnabled());
					QVERIFY(!bold->isEnabled());
				}
				const QStringList mutationButtonNames = {
				    QStringLiteral("customColourDefaultsButton"), QStringLiteral("customColourLighterButton"),
				    QStringLiteral("customColourDarkerButton"),   QStringLiteral("customColourMoreButton"),
				    QStringLiteral("customColourLessButton"),     QStringLiteral("customColourInvertButton"),
				    QStringLiteral("customColourSwapButton"),     QStringLiteral("customColourRandomButton"),
				    QStringLiteral("ansiNormalLighterButton"),    QStringLiteral("ansiNormalDarkerButton"),
				    QStringLiteral("ansiBoldLighterButton"),      QStringLiteral("ansiBoldDarkerButton"),
				    QStringLiteral("ansiColourDefaultsButton"),   QStringLiteral("ansiColourSwapButton"),
				    QStringLiteral("ansiColourInvertButton"),     QStringLiteral("ansiColourLighterButton"),
				    QStringLiteral("ansiColourDarkerButton"),     QStringLiteral("ansiColourMoreButton"),
				    QStringLiteral("ansiColourLessButton"),       QStringLiteral("ansiColourRandomButton"),
				    QStringLiteral("ansiColourLoadButton"),       QStringLiteral("ansiCopyToCustomButton")};
				for (const QString &buttonName : mutationButtonNames)
				{
					auto *button = dialog.findChild<QPushButton *>(buttonName);
					QVERIFY2(button, qPrintable(QStringLiteral("Missing %1").arg(buttonName)));
					QVERIFY2(
					    !button->isEnabled(),
					    qPrintable(QStringLiteral("%1 remains enabled under the override").arg(buttonName)));
				}

				auto *saveButton = dialog.findChild<QPushButton *>(QStringLiteral("ansiColourSaveButton"));
				QVERIFY(saveButton);
				QVERIFY(saveButton->isEnabled());
			}

			/**
			 * @brief Verifies every default override is rejected by the shared SetOption writer.
			 */
			void scriptDefaultOverrideOptionsAreReadOnly()
			{
				WorldRuntime runtime;
				runtime.applyDefaultWorldOptions();
				for (const QString &optionName :
				     {QStringLiteral("use_default_triggers"), QStringLiteral("use_default_aliases"),
				      QStringLiteral("use_default_timers"), QStringLiteral("use_default_macros"),
				      QStringLiteral("use_default_colours"), QStringLiteral("use_default_input_font"),
				      QStringLiteral("use_default_output_font")})
				{
					const WorldNumericOption *const option =
					    QMudWorldOptions::findWorldNumericOption(optionName);
					QVERIFY(option);
					QVERIFY((option->flags & OPT_CANNOT_WRITE) != 0);
					const QString before = runtime.worldAttributes().value(optionName);
					QCOMPARE(runtime.setOptionItem(*option, 1), ePluginCannotSetOption);
					QCOMPARE(runtime.worldAttributes().value(optionName), before);
				}
			}

			/**
			 * @brief Verifies one invalid configured default does not block other enabled defaults.
			 */
			void configuredDefaultFailuresAreIndependent()
			{
				QVERIFY(!AppController::instance());
				AppController controller;
				const QString defaultsPath =
				    QDir(QCoreApplication::applicationDirPath())
				        .filePath(QStringLiteral("world_preferences_independent_defaults_test.qdt"));
				const QString previousTriggersFile =
				    controller.getGlobalOption(QStringLiteral("DefaultTriggersFile")).toString();
				const QString previousAliasesFile =
				    controller.getGlobalOption(QStringLiteral("DefaultAliasesFile")).toString();
				const QString previousTimersFile =
				    controller.getGlobalOption(QStringLiteral("DefaultTimersFile")).toString();
				const auto cleanup = qScopeGuard(
				    [&controller, defaultsPath, previousTriggersFile, previousAliasesFile, previousTimersFile]
				    {
					    controller.setGlobalOptionString(QStringLiteral("DefaultTriggersFile"),
					                                     previousTriggersFile);
					    controller.setGlobalOptionString(QStringLiteral("DefaultAliasesFile"),
					                                     previousAliasesFile);
					    controller.setGlobalOptionString(QStringLiteral("DefaultTimersFile"),
					                                     previousTimersFile);
					    QFile::remove(defaultsPath);
				    });
				QFile::remove(defaultsPath);
				QFile defaultsFile(defaultsPath);
				QVERIFY(defaultsFile.open(QIODevice::WriteOnly | QIODevice::Text));
				QVERIFY(defaultsFile.write(QByteArrayLiteral(
				            "<qmud><world name=\"Defaults\" site=\"example.invalid\" port=\"4000\"/>"
				            "<triggers><trigger name=\"Default trigger\" match=\"default\"/>"
				            "</triggers><timers><timer name=\"Default timer\" second=\"10\"/>"
				            "</timers></qmud>")) > 0);
				defaultsFile.close();
				const QString configuredDefaultsPath = QDir::current().relativeFilePath(defaultsPath);
				controller.setGlobalOptionString(QStringLiteral("DefaultTriggersFile"),
				                                 configuredDefaultsPath);
				controller.setGlobalOptionString(QStringLiteral("DefaultAliasesFile"),
				                                 QStringLiteral("missing-default-aliases.qda"));
				controller.setGlobalOptionString(QStringLiteral("DefaultTimersFile"), configuredDefaultsPath);

				WorldRuntime runtime;
				runtime.setStartupDirectory(QCoreApplication::applicationDirPath());
				runtime.applyDefaultWorldOptions();
				runtime.setWorldAttribute(QStringLiteral("use_default_triggers"), QStringLiteral("1"));
				runtime.setWorldAttribute(QStringLiteral("use_default_aliases"), QStringLiteral("1"));
				runtime.setWorldAttribute(QStringLiteral("use_default_timers"), QStringLiteral("1"));
				WorldRuntime::Alias existingAlias;
				existingAlias.attributes.insert(QStringLiteral("name"), QStringLiteral("Existing alias"));
				existingAlias.attributes.insert(QStringLiteral("match"), QStringLiteral("existing"));
				runtime.setAliases({existingAlias});

				QString error;
				QVERIFY(!controller.applyConfiguredWorldDefaults(&runtime, {}, {}, &error));
				QVERIFY(error.contains(QStringLiteral("missing-default-aliases.qda")));
				QCOMPARE(runtime.triggers().size(), 1);
				QCOMPARE(runtime.triggers().constFirst().attributes.value(QStringLiteral("name")),
				         QStringLiteral("Default trigger"));
				QCOMPARE(runtime.aliases().size(), 1);
				QCOMPARE(runtime.aliases().constFirst().attributes.value(QStringLiteral("name")),
				         QStringLiteral("Existing alias"));
				QCOMPARE(runtime.timers().size(), 1);
				QCOMPARE(runtime.timers().constFirst().attributes.value(QStringLiteral("name")),
				         QStringLiteral("Default timer"));
			}

			/**
			 * @brief Verifies an active override can be disabled after its configured default is removed.
			 */
			void activeOverrideWithoutConfiguredDefaultCanBeDisabled()
			{
				QVERIFY(!AppController::instance());
				AppController controller;
				const QString previousTriggersFile =
				    controller.getGlobalOption(QStringLiteral("DefaultTriggersFile")).toString();
				const QString worldPath =
				    QDir(QCoreApplication::applicationDirPath())
				        .filePath(QStringLiteral("world_preferences_removed_default_test.qdl"));
				const auto cleanup = qScopeGuard(
				    [&controller, previousTriggersFile, worldPath]
				    {
					    controller.setGlobalOptionString(QStringLiteral("DefaultTriggersFile"),
					                                     previousTriggersFile);
					    QFile::remove(worldPath);
				    });
				QFile::remove(worldPath);
				controller.setGlobalOptionString(QStringLiteral("DefaultTriggersFile"), QString());

				WorldRuntime runtime;
				runtime.setStartupDirectory(QCoreApplication::applicationDirPath());
				runtime.applyDefaultWorldOptions();
				runtime.setWorldAttribute(QStringLiteral("name"), QStringLiteral("Removed Default"));
				runtime.setWorldAttribute(QStringLiteral("site"), QStringLiteral("example.invalid"));
				runtime.setWorldAttribute(QStringLiteral("port"), QStringLiteral("4000"));
				runtime.setWorldAttribute(QStringLiteral("use_default_triggers"), QStringLiteral("0"));
				WorldRuntime::Trigger persistentTrigger;
				persistentTrigger.attributes.insert(QStringLiteral("name"),
				                                    QStringLiteral("Persistent trigger"));
				persistentTrigger.attributes.insert(QStringLiteral("match"), QStringLiteral("persistent"));
				runtime.setTriggers({persistentTrigger});
				QString saveError;
				QVERIFY2(runtime.saveWorldFile(worldPath, &saveError), qPrintable(saveError));
				runtime.setWorldFilePath(worldPath);
				runtime.setWorldFileModified(false);

				WorldRuntime::Trigger runtimeDefault;
				runtimeDefault.attributes.insert(QStringLiteral("name"), QStringLiteral("Runtime default"));
				runtimeDefault.attributes.insert(QStringLiteral("match"), QStringLiteral("default"));
				runtime.setWorldAttribute(QStringLiteral("use_default_triggers"), QStringLiteral("1"));
				runtime.setTriggers({runtimeDefault});
				runtime.setWorldFileModified(false);

				WorldPreferencesDialog dialog(&runtime, nullptr);
				dialog.setInitialPage(WorldPreferencesDialog::PageTriggers);
				QCheckBox *const useDefaultTriggers =
				    findCheckBoxByText(dialog, QStringLiteral("O&verride with default triggers"));
				const RuleViewWidgets widgets = findRuleViewWidgets(dialog, RuleKind::Trigger);
				QVERIFY(useDefaultTriggers);
				QVERIFY(widgets.pasteButton);
				QVERIFY(useDefaultTriggers->isChecked());
				QVERIFY(useDefaultTriggers->isEnabled());
				QVERIFY(!widgets.pasteButton->isEnabled());

				useDefaultTriggers->setChecked(false);
				QVERIFY(widgets.pasteButton->isEnabled());
				dialog.accept();
				QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
				QCOMPARE(runtime.worldAttributes().value(QStringLiteral("use_default_triggers")),
				         QStringLiteral("0"));
				QCOMPARE(runtime.triggers().size(), 1);
				QCOMPARE(runtime.triggers().constFirst().attributes.value(QStringLiteral("name")),
				         QStringLiteral("Persistent trigger"));
			}

			/**
			 * @brief Verifies a failed default load leaves the override and runtime collection unchanged.
			 */
			void failedDefaultOverrideLoadDoesNotCommitTheOverride()
			{
				QVERIFY(!AppController::instance());
				AppController controller;
				const QString previousTriggersFile =
				    controller.getGlobalOption(QStringLiteral("DefaultTriggersFile")).toString();
				const QString worldPath =
				    QDir(QCoreApplication::applicationDirPath())
				        .filePath(QStringLiteral("world_preferences_failed_default_test.qdl"));
				const auto cleanup = qScopeGuard(
				    [&controller, previousTriggersFile, worldPath]
				    {
					    controller.setGlobalOptionString(QStringLiteral("DefaultTriggersFile"),
					                                     previousTriggersFile);
					    QFile::remove(worldPath);
				    });
				QFile::remove(worldPath);
				controller.setGlobalOptionString(QStringLiteral("DefaultTriggersFile"),
				                                 QStringLiteral("missing-default-triggers.qdt"));

				WorldRuntime runtime;
				runtime.setStartupDirectory(QCoreApplication::applicationDirPath());
				runtime.applyDefaultWorldOptions();
				runtime.setWorldAttribute(QStringLiteral("name"), QStringLiteral("Failed Default Test"));
				runtime.setWorldAttribute(QStringLiteral("site"), QStringLiteral("example.invalid"));
				runtime.setWorldAttribute(QStringLiteral("port"), QStringLiteral("4000"));
				runtime.setWorldAttribute(QStringLiteral("use_default_triggers"), QStringLiteral("0"));
				const QString persistedHistoryLines =
				    runtime.worldAttributes().value(QStringLiteral("history_lines"));
				QString saveError;
				QVERIFY2(runtime.saveWorldFile(worldPath, &saveError), qPrintable(saveError));
				runtime.setWorldFilePath(worldPath);
				runtime.setWorldAttribute(QStringLiteral("history_lines"), QStringLiteral("4321"));

				WorldPreferencesDialog dialog(&runtime, nullptr);
				dialog.setInitialPage(WorldPreferencesDialog::PageTriggers);
				QCheckBox *const useDefaultTriggers =
				    findCheckBoxByText(dialog, QStringLiteral("O&verride with default triggers"));
				QVERIFY(useDefaultTriggers);
				QString warningText;
				QVERIFY(QMetaObject::invokeMethod(
				    &dialog,
				    [&warningText]
				    {
					    auto *const message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
					    if (!message)
						    return;
					    warningText = message->text();
					    message->button(QMessageBox::Ok)->click();
				    },
				    Qt::QueuedConnection));
				useDefaultTriggers->setChecked(true);
				QVERIFY(!useDefaultTriggers->isChecked());
				dialog.accept();
				QVERIFY(warningText.contains(QStringLiteral("does not exist")));
				QCOMPARE(runtime.worldAttributes().value(QStringLiteral("use_default_triggers")),
				         QStringLiteral("0"));
				QCOMPARE(runtime.worldAttributes().value(QStringLiteral("history_lines")),
				         QStringLiteral("4321"));
				QVERIFY(runtime.worldFileModified());
				QVERIFY(runtime.triggers().isEmpty());
				WorldDocument savedWorld;
				QVERIFY2(savedWorld.loadFromFile(worldPath), qPrintable(savedWorld.errorString()));
				QCOMPARE(savedWorld.worldAttributes().value(QStringLiteral("history_lines")),
				         persistedHistoryLines);
			}

			/**
			 * @brief Verifies every runtime-only override preserves the corresponding world-file data.
			 */
			void overriddenRuntimeCollectionsDoNotReplacePersistentWorldData()
			{
				const QString worldPath =
				    QDir(QCoreApplication::applicationDirPath())
				        .filePath(QStringLiteral("world_preferences_all_override_persistence_test.qdl"));
				const auto cleanup = qScopeGuard([worldPath] { QFile::remove(worldPath); });
				QFile::remove(worldPath);

				WorldRuntime runtime;
				runtime.setStartupDirectory(QCoreApplication::applicationDirPath());
				runtime.applyDefaultWorldOptions();
				runtime.setWorldAttribute(QStringLiteral("name"), QStringLiteral("All Overrides"));
				runtime.setWorldAttribute(QStringLiteral("site"), QStringLiteral("example.invalid"));
				runtime.setWorldAttribute(QStringLiteral("port"), QStringLiteral("4000"));
				runtime.setWorldAttribute(QStringLiteral("input_font_name"), QStringLiteral("Saved Input"));
				runtime.setWorldAttribute(QStringLiteral("output_font_name"), QStringLiteral("Saved Output"));

				WorldRuntime::Trigger trigger;
				trigger.attributes.insert(QStringLiteral("name"), QStringLiteral("Saved trigger"));
				trigger.attributes.insert(QStringLiteral("match"), QStringLiteral("saved trigger"));
				WorldRuntime::Alias alias;
				alias.attributes.insert(QStringLiteral("name"), QStringLiteral("Saved alias"));
				alias.attributes.insert(QStringLiteral("match"), QStringLiteral("saved alias"));
				WorldRuntime::Timer timer;
				timer.attributes.insert(QStringLiteral("name"), QStringLiteral("Saved timer"));
				timer.attributes.insert(QStringLiteral("second"), QStringLiteral("10"));
				WorldRuntime::Macro macro;
				macro.attributes.insert(QStringLiteral("name"), QStringLiteral("F1"));
				macro.attributes.insert(QStringLiteral("type"), QStringLiteral("replace"));
				macro.children.insert(QStringLiteral("send"), QStringLiteral("saved macro"));
				WorldRuntime::Colour colour;
				colour.group = QStringLiteral("custom");
				colour.attributes.insert(QStringLiteral("seq"), QStringLiteral("1"));
				colour.attributes.insert(QStringLiteral("name"), QStringLiteral("Saved colour"));
				colour.attributes.insert(QStringLiteral("text"), QStringLiteral("#123456"));
				colour.attributes.insert(QStringLiteral("back"), QStringLiteral("#654321"));
				runtime.setTriggers({trigger});
				runtime.setAliases({alias});
				runtime.setTimers({timer});
				runtime.setMacros({macro});
				runtime.setColours({colour});
				QString saveError;
				QVERIFY2(runtime.saveWorldFile(worldPath, &saveError), qPrintable(saveError));
				runtime.setWorldFilePath(worldPath);

				trigger.attributes.insert(QStringLiteral("name"), QStringLiteral("Runtime trigger"));
				alias.attributes.insert(QStringLiteral("name"), QStringLiteral("Runtime alias"));
				timer.attributes.insert(QStringLiteral("name"), QStringLiteral("Runtime timer"));
				macro.children.insert(QStringLiteral("send"), QStringLiteral("runtime macro"));
				colour.attributes.insert(QStringLiteral("name"), QStringLiteral("Runtime colour"));
				runtime.setTriggers({trigger});
				runtime.setAliases({alias});
				runtime.setTimers({timer});
				runtime.setMacros({macro});
				runtime.setColours({colour});
				for (const QString &option :
				     {QStringLiteral("use_default_triggers"), QStringLiteral("use_default_aliases"),
				      QStringLiteral("use_default_timers"), QStringLiteral("use_default_macros"),
				      QStringLiteral("use_default_colours"), QStringLiteral("use_default_input_font"),
				      QStringLiteral("use_default_output_font")})
				{
					runtime.setWorldAttribute(option, QStringLiteral("1"));
				}
				QVERIFY2(runtime.saveWorldFile(worldPath, &saveError), qPrintable(saveError));

				WorldDocument savedWorld;
				QVERIFY2(savedWorld.loadFromFile(worldPath), qPrintable(savedWorld.errorString()));
				QCOMPARE(savedWorld.triggers().constFirst().attributes.value(QStringLiteral("name")),
				         QStringLiteral("Saved trigger"));
				QCOMPARE(savedWorld.aliases().constFirst().attributes.value(QStringLiteral("name")),
				         QStringLiteral("Saved alias"));
				QCOMPARE(savedWorld.timers().constFirst().attributes.value(QStringLiteral("name")),
				         QStringLiteral("Saved timer"));
				QCOMPARE(savedWorld.macros().constFirst().children.value(QStringLiteral("send")),
				         QStringLiteral("saved macro"));
				const auto savedColour = std::ranges::find_if(
				    savedWorld.colours(),
				    [](const WorldDocument::Colour &entry)
				    {
					    return entry.group == QStringLiteral("custom") &&
					           entry.attributes.value(QStringLiteral("seq")) == QStringLiteral("1");
				    });
				QVERIFY(savedColour != savedWorld.colours().cend());
				QCOMPARE(savedColour->attributes.value(QStringLiteral("name")),
				         QStringLiteral("Saved colour"));
				QCOMPARE(savedWorld.worldAttributes().value(QStringLiteral("input_font_name")),
				         QStringLiteral("Saved Input"));
				QCOMPARE(savedWorld.worldAttributes().value(QStringLiteral("output_font_name")),
				         QStringLiteral("Saved Output"));
			}

			/**
			 * @brief Verifies editing a default printing cell creates its missing persistent style entry.
			 */
			void printingEditCreatesMissingStyleEntry()
			{
				WorldRuntime runtime;
				runtime.applyDefaultWorldOptions();
				QVERIFY(runtime.printingStyles().isEmpty());

				WorldPreferencesDialog dialog(&runtime, nullptr);
				dialog.setInitialPage(WorldPreferencesDialog::PagePrinting);
				auto *const pages =
				    dialog.findChild<QStackedWidget *>(QStringLiteral("worldPreferencesPages"));
				QVERIFY(pages);
				QGroupBox *normalGroup = nullptr;
				for (QGroupBox *group :
				     pages->widget(WorldPreferencesDialog::PagePrinting)->findChildren<QGroupBox *>())
				{
					if (group->title() == QStringLiteral("Normal"))
					{
						normalGroup = group;
						break;
					}
				}
				QVERIFY(normalGroup);
				auto *const printingGrid = qobject_cast<QGridLayout *>(normalGroup->layout());
				QVERIFY(printingGrid);
				QLayoutItem *const firstBoldItem = printingGrid->itemAtPosition(1, 1);
				QVERIFY(firstBoldItem);
				auto *const firstBold = qobject_cast<QCheckBox *>(firstBoldItem->widget());
				QVERIFY(firstBold);
				firstBold->setChecked(true);
				dialog.accept();

				QCOMPARE(runtime.printingStyles().size(), 1);
				const WorldRuntime::PrintingStyle &style = runtime.printingStyles().constFirst();
				QCOMPARE(style.group, QStringLiteral("ansi/normal"));
				QCOMPARE(style.attributes.value(QStringLiteral("seq")), QStringLiteral("1"));
				QCOMPARE(style.attributes.value(QStringLiteral("bold")), QStringLiteral("1"));
				QCOMPARE(style.attributes.value(QStringLiteral("italic")), QString());
				QCOMPARE(style.attributes.value(QStringLiteral("underline")), QString());
			}

			/**
			 * @brief Verifies queued population stays size-neutral and untouched pages preserve later runtime values.
			 */
			void backgroundPopulationPreservesSizeAndUntouchedRuntimeChanges()
			{
				WorldRuntime runtime;
				runtime.applyDefaultWorldOptions();
				runtime.setWorldAttribute(QStringLiteral("name"), QStringLiteral("Background Test"));
				runtime.setWorldAttribute(QStringLiteral("site"), QStringLiteral("example.invalid"));
				runtime.setWorldAttribute(QStringLiteral("port"), QStringLiteral("4000"));
				runtime.setWorldAttribute(QStringLiteral("log_output"), QStringLiteral("1"));

				WorldPreferencesDialog dialog(&runtime, nullptr);
				dialog.setInitialPage(WorldPreferencesDialog::PageGeneral);
				dialog.show();
				QVERIFY(QTest::qWaitForWindowExposed(&dialog));
				const QSize initialSize = dialog.size();
				auto *const pages =
				    dialog.findChild<QStackedWidget *>(QStringLiteral("worldPreferencesPages"));
				QVERIFY(pages);
				for (int index = 0; index < pages->count(); ++index)
				{
					if (index == WorldPreferencesDialog::PageSound)
						continue;
					QTRY_VERIFY(pages->widget(index)->property("qmud_page_populated").toBool());
				}
				QCOMPARE(dialog.size(), initialSize);

				runtime.setWorldAttribute(QStringLiteral("log_output"), QStringLiteral("0"));
				dialog.accept();
				QCOMPARE(runtime.worldAttributes().value(QStringLiteral("log_output")), QStringLiteral("0"));
			}

			/**
			 * @brief Verifies Info action statistics populate automatically and memory calculation stays separate.
			 */
			void infoActionStatisticsAreAutomaticAndMemoryCalculationIsSeparate()
			{
				WorldRuntime runtime;
				runtime.applyDefaultWorldOptions();
				WorldRuntime::StyleSpan span;
				span.length     = 1;
				span.actionType = WorldRuntime::ActionSend;
				span.action     = QStringLiteral("look");
				const QVector spans{span};
				for (int index = 0; index < 1001; ++index)
					runtime.addLine(QStringLiteral("x"), WorldRuntime::LineOutput, spans);

				WorldPreferencesDialog dialog(&runtime, nullptr);
				dialog.setInitialPage(WorldPreferencesDialog::PageInfo);
				auto *const actions    = dialog.findChild<QLabel *>(QStringLiteral("infoMxpActionsCached"));
				auto *const references = dialog.findChild<QLabel *>(QStringLiteral("infoMxpReferenceCount"));
				auto *const memory     = dialog.findChild<QLabel *>(QStringLiteral("infoMemoryUsed"));
				auto *const calculate =
				    dialog.findChild<QPushButton *>(QStringLiteral("infoCalculateMemory"));
				QVERIFY(actions);
				QVERIFY(references);
				QVERIFY(memory);
				QVERIFY(calculate);
				QCOMPARE(actions->text(), QStringLiteral("1"));
				QCOMPARE(references->text(), QStringLiteral("1001"));
				QCOMPARE(memory->text(), QStringLiteral("-"));
				QVERIFY(calculate->isEnabled());

				span.action = QStringLiteral("score");
				runtime.addLine(QStringLiteral("x"), WorldRuntime::LineOutput, QVector{span});
				calculate->click();
				QCOMPARE(actions->text(), QStringLiteral("1"));
				QCOMPARE(references->text(), QStringLiteral("1001"));
				QVERIFY(memory->text() != QStringLiteral("-"));
				QVERIFY(!calculate->isEnabled());
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

				auto *pages   = dialog.findChild<QStackedWidget *>(QStringLiteral("worldPreferencesPages"));
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

			void echoForceTerminatesPartialPromptsDefaultsOffAndPersists()
			{
				WorldRuntime runtime;
				runtime.applyDefaultWorldOptions();
				QCOMPARE(
				    runtime.worldAttributes().value(QStringLiteral("echo_force_terminates_partial_prompts")),
				    QStringLiteral("n"));

				WorldPreferencesDialog dialog(&runtime, nullptr);
				dialog.setInitialPage(WorldPreferencesDialog::PageCommands);
				QCheckBox *const option =
				    findCheckBoxByText(dialog, QStringLiteral("Echo Force-Terminates Partial Prompts"));
				QVERIFY(option);
				QVERIFY(!option->isChecked());
				QCOMPARE(option->toolTip(),
				         QStringLiteral("Enable if the MUD's prompt does not appear properly."));

				option->setChecked(true);
				dialog.accept();
				QCOMPARE(
				    runtime.worldAttributes().value(QStringLiteral("echo_force_terminates_partial_prompts")),
				    QStringLiteral("1"));

				WorldPreferencesDialog restoredDialog(&runtime, nullptr);
				restoredDialog.setInitialPage(WorldPreferencesDialog::PageCommands);
				QCheckBox *const restoredOption = findCheckBoxByText(
				    restoredDialog, QStringLiteral("Echo Force-Terminates Partial Prompts"));
				QVERIFY(restoredOption);
				QVERIFY(restoredOption->isChecked());
			}

			void automaticTelnetProtectionOwnsManualOptionAndPreservesItsValue()
			{
				WorldRuntime runtime;
				runtime.applyDefaultWorldOptions();
				QCOMPARE(runtime.worldAttributes().value(QStringLiteral(
				             "automatically_protect_against_telnet_option_renegotiation_loops")),
				         QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("only_negotiate_telnet_options_once"),
				                          QStringLiteral("1"));

				WorldPreferencesDialog dialog(&runtime, nullptr);
				dialog.setInitialPage(WorldPreferencesDialog::PageConnecting);
				QCheckBox *const automatic = findCheckBoxByText(
				    dialog,
				    QStringLiteral("Automatically protect against Telnet option renegotiation loops"));
				QCheckBox *const manual =
				    findCheckBoxByText(dialog, QStringLiteral("Only negotiate telnet options once"));
				QVERIFY(automatic);
				QVERIFY(manual);
				QVERIFY(automatic->isChecked());
				QVERIFY(manual->isChecked());
				QVERIFY(!manual->isEnabled());

				automatic->setChecked(false);
				QVERIFY(manual->isEnabled());
				QVERIFY(manual->isChecked());
				dialog.accept();
				QCOMPARE(runtime.worldAttributes().value(QStringLiteral(
				             "automatically_protect_against_telnet_option_renegotiation_loops")),
				         QStringLiteral("0"));
				QCOMPARE(
				    runtime.worldAttributes().value(QStringLiteral("only_negotiate_telnet_options_once")),
				    QStringLiteral("1"));

				WorldPreferencesDialog restoredDialog(&runtime, nullptr);
				restoredDialog.setInitialPage(WorldPreferencesDialog::PageConnecting);
				QCheckBox *const restoredAutomatic = findCheckBoxByText(
				    restoredDialog,
				    QStringLiteral("Automatically protect against Telnet option renegotiation loops"));
				QCheckBox *const restoredManual =
				    findCheckBoxByText(restoredDialog, QStringLiteral("Only negotiate telnet options once"));
				QVERIFY(restoredAutomatic);
				QVERIFY(restoredManual);
				QVERIFY(!restoredAutomatic->isChecked());
				QVERIFY(restoredManual->isEnabled());
				QVERIFY(restoredManual->isChecked());
			}

			void customColourEditPersistsWithoutCreatingUntouchedEntries()
			{
				WorldRuntime::Colour existing;
				existing.group = QStringLiteral("custom");
				existing.attributes.insert(QStringLiteral("seq"), QStringLiteral("01"));
				existing.attributes.insert(QStringLiteral("seq_index"), QStringLiteral("0"));
				existing.attributes.insert(QStringLiteral("name"), QStringLiteral("Existing"));
				existing.attributes.insert(QStringLiteral("text"), QStringLiteral("#123456"));
				existing.attributes.insert(QStringLiteral("back"), QStringLiteral("#654321"));

				WorldRuntime runtime;
				runtime.applyDefaultWorldOptions();
				runtime.setColours({existing});

				WorldPreferencesDialog dialog(&runtime, nullptr);
				dialog.setInitialPage(WorldPreferencesDialog::PageCustomColours);
				auto *name       = dialog.findChild<QLineEdit *>(QStringLiteral("customColourName0"));
				auto *textSwatch = dialog.findChild<QPushButton *>(QStringLiteral("customTextSwatch0"));
				auto *backSwatch = dialog.findChild<QPushButton *>(QStringLiteral("customBackSwatch0"));
				QVERIFY(name);
				QVERIFY(textSwatch);
				QVERIFY(backSwatch);
				QCOMPARE(name->text(), QStringLiteral("Existing"));
				QCOMPARE(QColor(textSwatch->property("swatchColor").toString()),
				         QColor(QStringLiteral("#123456")));
				QCOMPARE(QColor(backSwatch->property("swatchColor").toString()),
				         QColor(QStringLiteral("#654321")));

				name->setText(QStringLiteral("Changed"));
				textSwatch->setProperty("swatchColor", QStringLiteral("#13579b"));
				backSwatch->setProperty("swatchColor", QStringLiteral("#2468ac"));
				dialog.accept();

				const WorldRuntime::Colour *changed     = nullptr;
				int                         customCount = 0;
				for (const WorldRuntime::Colour &colour : runtime.colours())
				{
					if (colour.group != QStringLiteral("custom"))
						continue;
					++customCount;
					if (colour.attributes.value(QStringLiteral("seq")) == QStringLiteral("1"))
						changed = &colour;
				}
				QCOMPARE(customCount, 1);
				QVERIFY(changed);
				QCOMPARE(changed->attributes.value(QStringLiteral("seq")), QStringLiteral("1"));
				QCOMPARE(changed->attributes.value(QStringLiteral("name")), QStringLiteral("Changed"));
				QCOMPARE(QColor(changed->attributes.value(QStringLiteral("text"))),
				         QColor(QStringLiteral("#13579b")));
				QCOMPARE(QColor(changed->attributes.value(QStringLiteral("back"))),
				         QColor(QStringLiteral("#2468ac")));

				WorldPreferencesDialog restoredDialog(&runtime, nullptr);
				restoredDialog.setInitialPage(WorldPreferencesDialog::PageCustomColours);
				auto *restoredName =
				    restoredDialog.findChild<QLineEdit *>(QStringLiteral("customColourName0"));
				auto *restoredText =
				    restoredDialog.findChild<QPushButton *>(QStringLiteral("customTextSwatch0"));
				auto *restoredBack =
				    restoredDialog.findChild<QPushButton *>(QStringLiteral("customBackSwatch0"));
				QVERIFY(restoredName);
				QVERIFY(restoredText);
				QVERIFY(restoredBack);
				QCOMPARE(restoredName->text(), QStringLiteral("Changed"));
				QCOMPARE(QColor(restoredText->property("swatchColor").toString()),
				         QColor(QStringLiteral("#13579b")));
				QCOMPARE(QColor(restoredBack->property("swatchColor").toString()),
				         QColor(QStringLiteral("#2468ac")));
			}

			void partialSaveCharacterThresholdFollowsParentAndPersists()
			{
				WorldRuntime runtime;
				runtime.applyDefaultWorldOptions();
				QCOMPARE(runtime.worldAttributes().value(QStringLiteral("partial_save_character_threshold")),
				         QStringLiteral("10"));

				WorldPreferencesDialog dialog(&runtime, nullptr);
				dialog.setInitialPage(WorldPreferencesDialog::PageCommands);
				dialog.show();
				QVERIFY(QTest::qWaitForWindowExposed(&dialog));
				QCoreApplication::processEvents();

				QPushButton *keyboardPreferences = nullptr;
				for (QPushButton *button : dialog.findChildren<QPushButton *>())
				{
					if (button && button->text() == QStringLiteral("Keyboard preferences..."))
					{
						keyboardPreferences = button;
						break;
					}
				}
				QVERIFY(keyboardPreferences);

				bool inspectedKeyboardDialog = false;
				QVERIFY(QMetaObject::invokeMethod(
				    &dialog,
				    [&inspectedKeyboardDialog]
				    {
					    auto *keyboardDialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
					    QVERIFY(keyboardDialog);
					    const auto closeDialog = qScopeGuard(
					        [keyboardDialog]
					        {
						        if (keyboardDialog->isVisible())
							        keyboardDialog->reject();
					        });

					    QCheckBox *saveDeleted =
					        findCheckBoxByText(*keyboardDialog, QStringLiteral("Save Deleted Command"));
					    QVERIFY(saveDeleted);
					    QLabel *thresholdLabel = nullptr;
					    for (QLabel *label : keyboardDialog->findChildren<QLabel *>())
					    {
						    if (label && label->text() == QStringLiteral("Partial Save Character Threshold"))
						    {
							    thresholdLabel = label;
							    break;
						    }
					    }
					    QVERIFY(thresholdLabel);
					    auto *threshold = qobject_cast<QSpinBox *>(thresholdLabel->buddy());
					    QVERIFY(threshold);
					    QCOMPARE(threshold->minimum(), 0);
					    QCOMPARE(threshold->maximum(), 200);
					    QCOMPARE(threshold->value(), 10);
					    QVERIFY(!threshold->isEnabled());
					    QVERIFY(thresholdLabel->mapTo(keyboardDialog, QPoint()).x() >
					            saveDeleted->mapTo(keyboardDialog, QPoint()).x());

					    saveDeleted->setChecked(true);
					    QVERIFY(threshold->isEnabled());
					    threshold->setValue(37);
					    inspectedKeyboardDialog = true;
					    keyboardDialog->accept();
				    },
				    Qt::QueuedConnection));

				QTest::mouseClick(keyboardPreferences, Qt::LeftButton);
				QVERIFY(inspectedKeyboardDialog);
				dialog.accept();
				QCOMPARE(runtime.worldAttributes().value(QStringLiteral("save_deleted_command")),
				         QStringLiteral("1"));
				QCOMPARE(runtime.worldAttributes().value(QStringLiteral("partial_save_character_threshold")),
				         QStringLiteral("37"));
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
				dialog.setInitialPage(WorldPreferencesDialog::PageOutput);
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

			void commandStackSeparatorAcceptsTwoCharacters()
			{
				WorldRuntime runtime;
				runtime.applyDefaultWorldOptions();

				WorldPreferencesDialog dialog(&runtime, nullptr);
				dialog.setInitialPage(WorldPreferencesDialog::PageCommands);
				auto *separator = dialog.findChild<QLineEdit *>(QStringLiteral("commandStackCharacterEdit"));
				QVERIFY(separator);
				QCOMPARE(separator->maxLength(), 2);

				QCheckBox *enabled = findCheckBoxByText(dialog, QStringLiteral("&Command Stacking Using:"));
				QVERIFY(enabled);
				enabled->setChecked(true);
				separator->setText(QStringLiteral("::"));
				dialog.accept();

				QCOMPARE(runtime.worldAttributes().value(QStringLiteral("enable_command_stack")),
				         QStringLiteral("1"));
				QCOMPARE(runtime.worldAttributes().value(QStringLiteral("command_stack_character")),
				         QStringLiteral("::"));
			}

			void ruleViewsSupportMultiSelectionForCopyAndRemoval()
			{
				for (const RuleKind kind : {RuleKind::Trigger, RuleKind::Alias, RuleKind::Timer})
				{
					for (const bool treeMode : {false, true})
					{
						WorldRuntime runtime;
						populateRuleSelectionFixtures(runtime, kind, treeMode, false);
						WorldPreferencesDialog dialog(&runtime, nullptr);
						dialog.setInitialPage(rulePage(kind));
						const RuleViewWidgets widgets = findRuleViewWidgets(dialog, kind);
						QVERIFY(widgets.table);
						QVERIFY(widgets.tree);
						QVERIFY(widgets.editButton);
						QVERIFY(widgets.removeButton);
						QVERIFY(widgets.copyButton);
						QVERIFY(widgets.treeToggle);
						QCOMPARE(widgets.table->selectionMode(), QAbstractItemView::ExtendedSelection);
						QCOMPARE(widgets.tree->selectionMode(), QAbstractItemView::ExtendedSelection);
						QCOMPARE(widgets.treeToggle->isChecked(), treeMode);
						QCOMPARE(widgets.table->rowCount(), 3);

						const QList<quint64> expectedIds = {
						    widgets.table->item(0, 0)->data(Qt::UserRole).toULongLong(),
						    widgets.table->item(2, 0)->data(Qt::UserRole).toULongLong()};
						if (treeMode)
							selectRuleTreeItems(*widgets.tree, expectedIds, expectedIds.constLast());
						else
							selectRuleRows(*widgets.table, {0, 2}, 2);

						QCOMPARE(selectedRuleIds(*widgets.table), treeMode ? QList<quint64>{} : expectedIds);
						QCOMPARE(selectedRuleIds(*widgets.tree), treeMode ? expectedIds : QList<quint64>{});
						QVERIFY(widgets.removeButton->isEnabled());
						QVERIFY(widgets.copyButton->isEnabled());
						if (kind != RuleKind::Timer)
						{
							QVERIFY(widgets.moveUpButton);
							QVERIFY(widgets.moveDownButton);
						}

						QClipboard *const clipboard = QGuiApplication::clipboard();
						QVERIFY(clipboard);
						clipboard->clear();
						widgets.copyButton->click();
						const QString xml       = clipboard->text();
						const QString itemStart = kind == RuleKind::Trigger ? QStringLiteral("<trigger ")
						                          : kind == RuleKind::Alias ? QStringLiteral("<alias ")
						                                                    : QStringLiteral("<timer ");
						QCOMPARE(xml.count(itemStart), 2);
						QVERIFY(xml.contains(copiedRuleIdentity(runtime, kind, expectedIds.at(0))));
						QVERIFY(xml.contains(copiedRuleIdentity(runtime, kind, expectedIds.at(1))));

						widgets.treeToggle->setChecked(!treeMode);
						QCOMPARE(selectedRuleIds(*widgets.table), expectedIds);
						QCOMPARE(selectedRuleIds(*widgets.tree), expectedIds);
						const quint64 middleId = widgets.table->item(1, 0)->data(Qt::UserRole).toULongLong();
						QVERIFY(!xml.contains(copiedRuleIdentity(runtime, kind, middleId)));
						if (treeMode)
							selectRuleRows(*widgets.table, {1}, 1);
						else
							selectRuleTreeItems(*widgets.tree, {middleId}, middleId);
						QCOMPARE(treeMode ? selectedRuleIds(*widgets.tree) : selectedRuleIds(*widgets.table),
						         expectedIds);
						QCOMPARE(treeMode ? selectedRuleIds(*widgets.table) : selectedRuleIds(*widgets.tree),
						         QList<quint64>{middleId});
						widgets.treeToggle->setChecked(treeMode);
						QCOMPARE(selectedRuleIds(*widgets.table), QList<quint64>{middleId});
						QCOMPARE(selectedRuleIds(*widgets.tree), QList<quint64>{middleId});
					}
				}
			}

			/** @brief Verifies table-to-tree selection transfer exposes every selected rule group. */
			void switchingToTreeExpandsAllSelectedRuleGroups()
			{
				for (const RuleKind kind : {RuleKind::Trigger, RuleKind::Alias, RuleKind::Timer})
				{
					WorldRuntime runtime;
					populateRuleSelectionFixtures(runtime, kind, false, true);
					WorldPreferencesDialog dialog(&runtime, nullptr);
					dialog.setInitialPage(rulePage(kind));
					const RuleViewWidgets widgets = findRuleViewWidgets(dialog, kind);
					QVERIFY(widgets.table);
					QVERIFY(widgets.tree);
					QVERIFY(widgets.treeToggle);

					QTreeWidgetItem *const primaryGroup =
					    findRuleGroup(*widgets.tree, QStringLiteral("primary"));
					QTreeWidgetItem *const soleGroup = findRuleGroup(*widgets.tree, QStringLiteral("sole"));
					QVERIFY(primaryGroup);
					QVERIFY(soleGroup);
					QVERIFY(!primaryGroup->isExpanded());
					QVERIFY(!soleGroup->isExpanded());

					QList<int>     rows;
					QList<quint64> runtimeIds;
					for (const QString &groupName : {QStringLiteral("primary"), QStringLiteral("sole")})
					{
						for (int row = 0; row < widgets.table->rowCount(); ++row)
						{
							const QTableWidgetItem *const groupItem = widgets.table->item(row, 4);
							if (!groupItem || groupItem->text() != groupName)
								continue;
							rows.push_back(row);
							runtimeIds.push_back(
							    widgets.table->item(row, 0)->data(Qt::UserRole).toULongLong());
							break;
						}
					}
					QCOMPARE(rows.size(), 2);
					selectRuleRows(*widgets.table, rows, rows.constLast());
					QVERIFY(selectedRuleIds(*widgets.tree).isEmpty());
					QVERIFY(!primaryGroup->isExpanded());
					QVERIFY(!soleGroup->isExpanded());

					widgets.treeToggle->setChecked(true);
					QVERIFY(primaryGroup->isExpanded());
					QVERIFY(soleGroup->isExpanded());
					const QList<quint64> selectedTreeIds = selectedRuleIds(*widgets.tree);
					QCOMPARE(QSet<quint64>(selectedTreeIds.cbegin(), selectedTreeIds.cend()),
					         QSet<quint64>(runtimeIds.cbegin(), runtimeIds.cend()));

					auto lastExpandedGroup = [&runtime, kind]
					{
						switch (kind)
						{
						case RuleKind::Trigger:
							return runtime.lastTriggerTreeExpandedGroup();
						case RuleKind::Alias:
							return runtime.lastAliasTreeExpandedGroup();
						case RuleKind::Timer:
							return runtime.lastTimerTreeExpandedGroup();
						}
						return QString();
					};
					QVERIFY(lastExpandedGroup().isEmpty());
					primaryGroup->setExpanded(false);
					QCOMPARE(lastExpandedGroup(), QStringLiteral("sole"));
					primaryGroup->setExpanded(true);
					QVERIFY(primaryGroup->isExpanded());
					QVERIFY(!soleGroup->isExpanded());
					QCOMPARE(lastExpandedGroup(), QStringLiteral("primary"));
				}
			}

			void multipleRuleRemovalIsConfirmedOnceAndStagedByIdentity()
			{
				AppController app;
				QCOMPARE(app.getGlobalOption(QStringLiteral("TriggerRemoveCheck")).toInt(), 1);
				for (const RuleKind kind : {RuleKind::Trigger, RuleKind::Alias, RuleKind::Timer})
				{
					for (const bool treeMode : {false, true})
					{
						WorldRuntime runtime;
						runtime.applyDefaultWorldOptions();
						runtime.setWorldAttribute(QStringLiteral("name"), QStringLiteral("Test World"));
						runtime.setWorldAttribute(QStringLiteral("site"), QStringLiteral("example.invalid"));
						runtime.setWorldAttribute(QStringLiteral("port"), QStringLiteral("4000"));
						populateRuleSelectionFixtures(runtime, kind, treeMode, false);
						WorldPreferencesDialog dialog(&runtime, nullptr);
						dialog.setInitialPage(rulePage(kind));
						const RuleViewWidgets widgets = findRuleViewWidgets(dialog, kind);
						QVERIFY(widgets.table);
						QVERIFY(widgets.tree);
						QVERIFY(widgets.removeButton);

						const QList<quint64> removedIds = {
						    widgets.table->item(0, 0)->data(Qt::UserRole).toULongLong(),
						    widgets.table->item(2, 0)->data(Qt::UserRole).toULongLong()};
						const quint64 survivorId =
						    widgets.table->item(1, 0)->data(Qt::UserRole).toULongLong();
						auto selectRemovedRules = [&]
						{
							if (treeMode)
								selectRuleTreeItems(*widgets.tree, removedIds, removedIds.constLast());
							else
								selectRuleRows(*widgets.table, {0, 2}, 2);
						};
						selectRemovedRules();

						bool cancellationHandled = false;
						QVERIFY(QMetaObject::invokeMethod(
						    &dialog,
						    [&cancellationHandled]
						    {
							    auto *const message =
							        qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
							    if (!message)
								    return;
							    cancellationHandled = true;
							    message->button(QMessageBox::No)->click();
						    },
						    Qt::QueuedConnection));
						widgets.removeButton->click();
						QVERIFY(cancellationHandled);
						QCOMPARE(widgets.table->rowCount(), 3);
						QCOMPARE(selectedRuleIds(*widgets.table), treeMode ? QList<quint64>{} : removedIds);
						QCOMPARE(selectedRuleIds(*widgets.tree), treeMode ? removedIds : QList<quint64>{});

						QString confirmationText;
						QVERIFY(QMetaObject::invokeMethod(
						    &dialog,
						    [&confirmationText]
						    {
							    auto *const message =
							        qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
							    if (!message)
								    return;
							    confirmationText = message->text();
							    message->button(QMessageBox::Yes)->click();
						    },
						    Qt::QueuedConnection));
						widgets.removeButton->click();
						const QString plural = kind == RuleKind::Alias     ? QStringLiteral("aliases")
						                       : kind == RuleKind::Trigger ? QStringLiteral("triggers")
						                                                   : QStringLiteral("timers");
						QCOMPARE(confirmationText,
						         QStringLiteral("Delete these 2 %1 - are you sure?").arg(plural));
						QCOMPARE(widgets.table->rowCount(), 1);
						QCOMPARE(widgets.table->item(0, 0)->data(Qt::UserRole).toULongLong(), survivorId);

						auto runtimeIds = [&runtime, kind]
						{
							QList<quint64> result;
							switch (kind)
							{
							case RuleKind::Trigger:
								for (const auto &rule : runtime.triggers())
									result.push_back(rule.runtimeId);
								break;
							case RuleKind::Alias:
								for (const auto &rule : runtime.aliases())
									result.push_back(rule.runtimeId);
								break;
							case RuleKind::Timer:
								for (const auto &rule : runtime.timers())
									result.push_back(rule.runtimeId);
								break;
							}
							return result;
						};
						QCOMPARE(runtimeIds().size(), 3);
						dialog.accept();
						QCOMPARE(runtimeIds(), QList<quint64>{survivorId});
					}
				}
			}

			void ruleRemovalUsesAdjacentItemInDisplayedSortOrder()
			{
				const QList<RuleKind> ruleKinds = {RuleKind::Trigger, RuleKind::Alias, RuleKind::Timer};
				for (const RuleKind kind : ruleKinds)
				{
					WorldRuntime runtime;
					populateRuleSelectionFixtures(runtime, kind, false, false);
					WorldPreferencesDialog dialog(&runtime, nullptr);
					dialog.setInitialPage(rulePage(kind));
					const RuleViewWidgets widgets = findRuleViewWidgets(dialog, kind);
					QVERIFY(widgets.table);
					QVERIFY(widgets.tree);
					QVERIFY(widgets.removeButton);
					QCOMPARE(widgets.table->rowCount(), 3);

					const int     displayColumn    = ruleDisplayColumn(kind);
					const QString expectedPrevious = widgets.table->item(0, displayColumn)->text();
					widgets.table->setCurrentCell(1, 0);
					QVERIFY(widgets.removeButton->isEnabled());
					widgets.removeButton->click();

					QCOMPARE(widgets.table->rowCount(), 2);
					QCOMPARE(widgets.table->currentRow(), 0);
					QCOMPARE(widgets.table->item(widgets.table->currentRow(), displayColumn)->text(),
					         expectedPrevious);

					const QString expectedNext = widgets.table->item(1, displayColumn)->text();
					widgets.removeButton->click();
					QCOMPARE(widgets.table->rowCount(), 1);
					QCOMPARE(widgets.table->currentRow(), 0);
					QCOMPARE(widgets.table->item(widgets.table->currentRow(), displayColumn)->text(),
					         expectedNext);

					QVERIFY(widgets.removeButton->isEnabled());
					widgets.removeButton->click();
					QCOMPARE(widgets.table->rowCount(), 0);
					QCOMPARE(widgets.table->currentRow(), -1);
					QVERIFY(widgets.table->selectedItems().isEmpty());
				}
			}

			void groupedRuleRemovalUsesSortedSiblingAndNotAnotherGroup()
			{
				const QList<RuleKind> ruleKinds = {RuleKind::Trigger, RuleKind::Alias, RuleKind::Timer};
				for (const RuleKind kind : ruleKinds)
				{
					WorldRuntime runtime;
					populateRuleSelectionFixtures(runtime, kind, true, true);
					WorldPreferencesDialog dialog(&runtime, nullptr);
					dialog.setInitialPage(rulePage(kind));
					const RuleViewWidgets widgets = findRuleViewWidgets(dialog, kind);
					QVERIFY(widgets.table);
					QVERIFY(widgets.tree);
					QVERIFY(widgets.removeButton);

					QTreeWidgetItem *primaryGroup = findRuleGroup(*widgets.tree, QStringLiteral("primary"));
					QVERIFY(primaryGroup);
					QCOMPARE(primaryGroup->childCount(), 3);
					primaryGroup->setExpanded(true);
					const QString expectedPrevious = primaryGroup->child(0)->text(0);
					widgets.tree->setCurrentItem(primaryGroup->child(1));
					QVERIFY(widgets.removeButton->isEnabled());
					widgets.removeButton->click();

					primaryGroup = findRuleGroup(*widgets.tree, QStringLiteral("primary"));
					QVERIFY(primaryGroup);
					QCOMPARE(primaryGroup->childCount(), 2);
					QVERIFY(widgets.tree->currentItem());
					QCOMPARE(widgets.tree->currentItem()->parent(), primaryGroup);
					QCOMPARE(widgets.tree->currentItem()->text(0), expectedPrevious);
					const QString expectedNext = primaryGroup->child(1)->text(0);
					widgets.removeButton->click();

					primaryGroup = findRuleGroup(*widgets.tree, QStringLiteral("primary"));
					QVERIFY(primaryGroup);
					QCOMPARE(primaryGroup->childCount(), 1);
					QVERIFY(widgets.tree->currentItem());
					QCOMPARE(widgets.tree->currentItem()->parent(), primaryGroup);
					QCOMPARE(widgets.tree->currentItem()->text(0), expectedNext);
					widgets.removeButton->click();

					QVERIFY(!findRuleGroup(*widgets.tree, QStringLiteral("primary")));
					QVERIFY(!widgets.tree->currentItem());
					QVERIFY(widgets.table->selectedItems().isEmpty());

					QTreeWidgetItem *soleGroup = findRuleGroup(*widgets.tree, QStringLiteral("sole"));
					QVERIFY(soleGroup);
					QCOMPARE(soleGroup->childCount(), 1);
					soleGroup->setExpanded(true);
					widgets.tree->setCurrentItem(soleGroup->child(0));
					QVERIFY(widgets.removeButton->isEnabled());
					widgets.removeButton->click();

					QVERIFY(!findRuleGroup(*widgets.tree, QStringLiteral("sole")));
					QVERIFY(!widgets.tree->currentItem());
					QVERIFY(widgets.table->selectedItems().isEmpty());
				}
			}

			void ruleDeletionIsStagedUntilMainDialogAcceptance()
			{
				AppController app;
				QCOMPARE(app.getGlobalOption(QStringLiteral("TriggerRemoveCheck")).toInt(), 1);
				for (const RuleKind kind : {RuleKind::Trigger, RuleKind::Alias, RuleKind::Timer})
				{
					auto exerciseDeletion = [kind](const bool acceptDialog)
					{
						WorldRuntime runtime;
						runtime.applyDefaultWorldOptions();
						runtime.setWorldAttribute(QStringLiteral("name"), QStringLiteral("Test World"));
						runtime.setWorldAttribute(QStringLiteral("site"), QStringLiteral("example.invalid"));
						runtime.setWorldAttribute(QStringLiteral("port"), QStringLiteral("4000"));
						populateRuleSelectionFixtures(runtime, kind, false, false);
						WorldPreferencesDialog dialog(&runtime, nullptr);
						dialog.setInitialPage(rulePage(kind));
						const RuleViewWidgets widgets = findRuleViewWidgets(dialog, kind);
						QVERIFY(widgets.table);
						QVERIFY(widgets.removeButton);
						widgets.table->setCurrentCell(0, 0);

						bool confirmationHandled = false;
						QVERIFY(QMetaObject::invokeMethod(
						    &dialog,
						    [&confirmationHandled]
						    {
							    auto *message =
							        qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
							    if (!message)
								    return;
							    confirmationHandled = true;
							    message->button(QMessageBox::Yes)->click();
						    },
						    Qt::QueuedConnection));
						widgets.removeButton->click();
						QVERIFY(confirmationHandled);
						QCOMPARE(widgets.table->rowCount(), 2);

						auto runtimeSize = [&runtime, kind]() -> qsizetype
						{
							switch (kind)
							{
							case RuleKind::Trigger:
								return runtime.triggers().size();
							case RuleKind::Alias:
								return runtime.aliases().size();
							case RuleKind::Timer:
								return runtime.timers().size();
							}
							return 0;
						};
						QCOMPARE(runtimeSize(), 3);
						if (acceptDialog)
							dialog.accept();
						else
							dialog.reject();
						QCOMPARE(runtimeSize(), acceptDialog ? 2 : 3);
					};
					exerciseDeletion(false);
					exerciseDeletion(true);
				}
			}

			void pastedNamedDuplicateIsStagedAndBlocksAcceptance()
			{
				WorldRuntime runtime;
				runtime.applyDefaultWorldOptions();
				runtime.setWorldAttribute(QStringLiteral("name"), QStringLiteral("Test World"));
				runtime.setWorldAttribute(QStringLiteral("site"), QStringLiteral("example.invalid"));
				runtime.setWorldAttribute(QStringLiteral("port"), QStringLiteral("4000"));
				WorldRuntime::Trigger trigger;
				trigger.attributes.insert(QStringLiteral("name"), QStringLiteral("Existing"));
				trigger.attributes.insert(QStringLiteral("match"), QStringLiteral("original"));
				runtime.setTriggers({trigger});

				WorldPreferencesDialog dialog(&runtime, nullptr);
				dialog.setInitialPage(WorldPreferencesDialog::PageTriggers);
				const RuleViewWidgets widgets = findRuleViewWidgets(dialog, RuleKind::Trigger);
				QVERIFY(widgets.table);
				QVERIFY(widgets.pasteButton);
				QClipboard *clipboard = QGuiApplication::clipboard();
				QVERIFY(clipboard);
				clipboard->setText(
				    QStringLiteral("<qmud><triggers><trigger name=\"existing\" match=\"pasted\">"
				                   "<send>pasted</send></trigger>"
				                   "<trigger name=\"EXISTING\" match=\"second pasted\">"
				                   "<send>second pasted</send></trigger></triggers></qmud>"));

				widgets.pasteButton->click();
				QCOMPARE(widgets.table->rowCount(), 3);
				QCOMPARE(runtime.triggers().size(), 1);

				QString duplicateMessage;
				QVERIFY(QMetaObject::invokeMethod(
				    &dialog,
				    [&duplicateMessage]
				    {
					    auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
					    if (!message)
						    return;
					    duplicateMessage = message->text();
					    message->button(QMessageBox::Ok)->click();
				    },
				    Qt::QueuedConnection));
				dialog.accept();
				QVERIFY(
				    duplicateMessage.contains(QStringLiteral("Named rules and variables should be unique:")));
				QVERIFY(duplicateMessage.contains(QStringLiteral("Triggers\n  3 named Existing")));
				QCOMPARE(runtime.triggers().size(), 1);
				QCOMPARE(dialog.result(), 0);
			}

			void inlineVariableEditIsStagedUntilMainDialogAcceptance()
			{
				auto configureRuntime = [](WorldRuntime &runtime)
				{
					runtime.applyDefaultWorldOptions();
					runtime.setWorldAttribute(QStringLiteral("name"), QStringLiteral("Test World"));
					runtime.setWorldAttribute(QStringLiteral("site"), QStringLiteral("example.invalid"));
					runtime.setWorldAttribute(QStringLiteral("port"), QStringLiteral("4000"));
					WorldRuntime::Variable variable;
					variable.attributes.insert(QStringLiteral("name"), QStringLiteral("health"));
					variable.content = QStringLiteral("10");
					runtime.setVariables({variable});
				};
				auto findVariablesTable = [](const WorldPreferencesDialog &dialog) -> QTableWidget *
				{
					for (QTableWidget *table : dialog.findChildren<QTableWidget *>())
					{
						if (table && table->horizontalHeaderItem(0) && table->horizontalHeaderItem(1) &&
						    table->horizontalHeaderItem(0)->text() == QStringLiteral("Name") &&
						    table->horizontalHeaderItem(1)->text() == QStringLiteral("Value"))
							return table;
					}
					return nullptr;
				};

				{
					WorldRuntime runtime;
					configureRuntime(runtime);
					WorldPreferencesDialog dialog(&runtime, nullptr);
					dialog.setInitialPage(WorldPreferencesDialog::PageVariables);
					QTableWidget *table = findVariablesTable(dialog);
					QVERIFY(table);
					table->item(0, 1)->setText(QStringLiteral("20"));
					QCOMPARE(runtime.variables().front().content, QStringLiteral("10"));
					dialog.reject();
					QCOMPARE(runtime.variables().front().content, QStringLiteral("10"));
				}

				{
					WorldRuntime runtime;
					configureRuntime(runtime);
					WorldPreferencesDialog dialog(&runtime, nullptr);
					dialog.setInitialPage(WorldPreferencesDialog::PageVariables);
					QTableWidget *table = findVariablesTable(dialog);
					QVERIFY(table);
					table->item(0, 1)->setText(QStringLiteral("20"));
					QCOMPARE(runtime.variables().front().content, QStringLiteral("10"));
					dialog.accept();
					QCOMPARE(runtime.variables().front().content, QStringLiteral("20"));
				}
			}

			void ruleEditsFollowRuntimeIdentityAfterEarlierRuleRemoval()
			{
				const QList<RuleKind> ruleKinds = {RuleKind::Trigger, RuleKind::Alias, RuleKind::Timer};
				for (const RuleKind kind : ruleKinds)
				{
					WorldRuntime runtime;
					switch (kind)
					{
					case RuleKind::Trigger:
					{
						auto makeTrigger = [](const QString &name, const QString &match, const QString &group)
						{
							WorldRuntime::Trigger trigger;
							trigger.attributes.insert(QStringLiteral("name"), name);
							trigger.attributes.insert(QStringLiteral("match"), match);
							trigger.attributes.insert(QStringLiteral("group"), group);
							trigger.children.insert(QStringLiteral("send"), QStringLiteral("noop"));
							return trigger;
						};
						QList<WorldRuntime::Trigger> triggers = {
						    makeTrigger(QStringLiteral("remove_trigger"), QStringLiteral("Alpha"),
						                QStringLiteral("remove-group")),
						    makeTrigger(QStringLiteral("target_trigger"), QStringLiteral("Mike"),
						                QStringLiteral("target-group")),
						    makeTrigger(QStringLiteral("other_trigger"), QStringLiteral("Zulu"),
						                QStringLiteral("other-group"))};
						triggers[1].matched            = 9;
						triggers[1].invocationCount    = 4;
						triggers[1].matchCount         = 3;
						triggers[1].matchAttempts      = 12;
						triggers[1].executionTimeNs    = 17'000'000;
						triggers[1].lastMatchWildcards = {QStringLiteral("whole"), QStringLiteral("capture")};
						runtime.setTriggers(triggers);
						break;
					}
					case RuleKind::Alias:
					{
						auto makeAlias = [](const QString &name, const QString &match, const QString &group)
						{
							WorldRuntime::Alias alias;
							alias.attributes.insert(QStringLiteral("name"), name);
							alias.attributes.insert(QStringLiteral("match"), match);
							alias.attributes.insert(QStringLiteral("group"), group);
							alias.children.insert(QStringLiteral("send"), QStringLiteral("noop"));
							return alias;
						};
						QList<WorldRuntime::Alias> aliases = {
						    makeAlias(QStringLiteral("remove_alias"), QStringLiteral("Alpha"),
						              QStringLiteral("remove-group")),
						    makeAlias(QStringLiteral("target_alias"), QStringLiteral("Mike"),
						              QStringLiteral("target-group")),
						    makeAlias(QStringLiteral("other_alias"), QStringLiteral("Zulu"),
						              QStringLiteral("other-group"))};
						aliases[1].matched            = 9;
						aliases[1].invocationCount    = 4;
						aliases[1].matchCount         = 3;
						aliases[1].matchAttempts      = 12;
						aliases[1].executionTimeNs    = 19'000'000;
						aliases[1].lastMatchWildcards = {QStringLiteral("whole"), QStringLiteral("capture")};
						runtime.setAliases(aliases);
						break;
					}
					case RuleKind::Timer:
					{
						auto makeTimer = [](const QString &name, const int hour, const QString &group)
						{
							WorldRuntime::Timer timer;
							timer.attributes.insert(QStringLiteral("name"), name);
							timer.attributes.insert(QStringLiteral("at_time"), QStringLiteral("1"));
							timer.attributes.insert(QStringLiteral("hour"), QString::number(hour));
							timer.attributes.insert(QStringLiteral("group"), group);
							timer.children.insert(QStringLiteral("send"), QStringLiteral("noop"));
							return timer;
						};
						QList<WorldRuntime::Timer> timers = {
						    makeTimer(QStringLiteral("remove_timer"), 6, QStringLiteral("remove-group")),
						    makeTimer(QStringLiteral("target_timer"), 12, QStringLiteral("target-group")),
						    makeTimer(QStringLiteral("other_timer"), 18, QStringLiteral("other-group"))};
						timers[1].firedCount      = 9;
						timers[1].invocationCount = 4;
						timers[1].lastFired       = QDateTime::fromSecsSinceEpoch(1'700'000'000);
						timers[1].nextFireTime    = QDateTime::fromSecsSinceEpoch(1'700'000'300);
						runtime.setTimers(timers);
						break;
					}
					}

					WorldPreferencesDialog dialog(&runtime, nullptr);
					dialog.setInitialPage(rulePage(kind));
					const RuleViewWidgets widgets = findRuleViewWidgets(dialog, kind);
					QVERIFY(widgets.table);
					QVERIFY(widgets.editButton);
					quint64 targetRuntimeId{0};
					switch (kind)
					{
					case RuleKind::Trigger:
						targetRuntimeId = runtime.triggers().at(1).runtimeId;
						break;
					case RuleKind::Alias:
						targetRuntimeId = runtime.aliases().at(1).runtimeId;
						break;
					case RuleKind::Timer:
						targetRuntimeId = runtime.timers().at(1).runtimeId;
						break;
					}
					QVERIFY(targetRuntimeId != 0);
					const int targetRow = findRuleRowByRuntimeId(*widgets.table, targetRuntimeId);
					QVERIFY(targetRow >= 0);
					widgets.table->setCurrentCell(targetRow, 0);
					QStringList observedEditorLabelTexts;
					QString     editorCallbackFailure;
					bool        editorCallbackInvoked = false;

					QVERIFY(QMetaObject::invokeMethod(
					    &dialog,
					    [&runtime, &observedEditorLabelTexts, &editorCallbackFailure, &editorCallbackInvoked,
					     kind]
					    {
						    editorCallbackInvoked = true;
						    QWidget *activeModal  = QApplication::activeModalWidget();
						    auto    *editor       = qobject_cast<QDialog *>(activeModal);
						    if (!editor)
						    {
							    editorCallbackFailure = QStringLiteral("Rule editor was not active.");
							    if (activeModal)
								    activeModal->close();
							    return;
						    }

						    for (const QLabel *label : editor->findChildren<QLabel *>())
						    {
							    if (label)
								    observedEditorLabelTexts.push_back(label->text());
						    }

						    switch (kind)
						    {
						    case RuleKind::Trigger:
						    {
							    QList<WorldRuntime::Trigger> triggers = runtime.triggers();
							    triggers.removeFirst();
							    runtime.setTriggers(triggers);
							    break;
						    }
						    case RuleKind::Alias:
						    {
							    QList<WorldRuntime::Alias> aliases = runtime.aliases();
							    aliases.removeFirst();
							    runtime.setAliases(aliases);
							    break;
						    }
						    case RuleKind::Timer:
						    {
							    QList<WorldRuntime::Timer> timers = runtime.timers();
							    timers.removeFirst();
							    runtime.setTimers(timers);
							    break;
						    }
						    }
						    QLineEdit *groupEdit = nullptr;
						    for (QLineEdit *lineEdit : editor->findChildren<QLineEdit *>())
						    {
							    if (lineEdit && lineEdit->text() == QStringLiteral("target-group"))
							    {
								    groupEdit = lineEdit;
								    break;
							    }
						    }
						    if (!groupEdit)
						    {
							    editorCallbackFailure =
							        QStringLiteral("Rule editor group field was not found.");
							    editor->reject();
							    return;
						    }
						    groupEdit->setText(QStringLiteral("edited-group"));

						    auto *buttons = editor->findChild<QDialogButtonBox *>();
						    if (!buttons || !buttons->button(QDialogButtonBox::Ok))
						    {
							    editorCallbackFailure =
							        QStringLiteral("Rule editor OK button was not found.");
							    editor->reject();
							    return;
						    }
						    buttons->button(QDialogButtonBox::Ok)->click();
					    },
					    Qt::QueuedConnection));
					widgets.editButton->click();
					QVERIFY2(editorCallbackInvoked, "Rule editor callback did not run.");
					QVERIFY2(editorCallbackFailure.isEmpty(), qPrintable(editorCallbackFailure));
					dialog.accept();

					switch (kind)
					{
					case RuleKind::Trigger:
						QVERIFY(observedEditorLabelTexts.contains(QStringLiteral("9 matches.")));
						QVERIFY(observedEditorLabelTexts.contains(QStringLiteral("4 calls.")));
						QVERIFY(observedEditorLabelTexts.contains(QStringLiteral("0.017000 sec.")));
						break;
					case RuleKind::Alias:
						QVERIFY(observedEditorLabelTexts.contains(QStringLiteral("9 matches.")));
						QVERIFY(observedEditorLabelTexts.contains(QStringLiteral("4 calls.")));
						break;
					case RuleKind::Timer:
						QVERIFY(observedEditorLabelTexts.contains(QStringLiteral("Fired 9 times.")));
						QVERIFY(observedEditorLabelTexts.contains(QStringLiteral("4 calls.")));
						break;
					}

					QVERIFY(targetRuntimeId != 0);
					switch (kind)
					{
					case RuleKind::Trigger:
					{
						const QList<WorldRuntime::Trigger> &triggers = runtime.triggers();
						QCOMPARE(triggers.size(), 2);
						QCOMPARE(triggers.at(0).runtimeId, targetRuntimeId);
						QCOMPARE(triggers.at(0).attributes.value(QStringLiteral("group")),
						         QStringLiteral("edited-group"));
						QCOMPARE(triggers.at(0).matched, 9);
						QCOMPARE(triggers.at(0).invocationCount, 4);
						QCOMPARE(triggers.at(0).matchCount, 3);
						QCOMPARE(triggers.at(0).matchAttempts, 12);
						QCOMPARE(triggers.at(0).executionTimeNs, qint64{17'000'000});
						QCOMPARE(triggers.at(0).lastMatchWildcards,
						         QStringList({QStringLiteral("whole"), QStringLiteral("capture")}));
						QCOMPARE(triggers.at(1).attributes.value(QStringLiteral("group")),
						         QStringLiteral("other-group"));
						break;
					}
					case RuleKind::Alias:
					{
						const QList<WorldRuntime::Alias> &aliases = runtime.aliases();
						QCOMPARE(aliases.size(), 2);
						QCOMPARE(aliases.at(0).runtimeId, targetRuntimeId);
						QCOMPARE(aliases.at(0).attributes.value(QStringLiteral("group")),
						         QStringLiteral("edited-group"));
						QCOMPARE(aliases.at(0).matched, 9);
						QCOMPARE(aliases.at(0).invocationCount, 4);
						QCOMPARE(aliases.at(0).matchCount, 3);
						QCOMPARE(aliases.at(0).matchAttempts, 12);
						QCOMPARE(aliases.at(0).executionTimeNs, qint64{19'000'000});
						QCOMPARE(aliases.at(0).lastMatchWildcards,
						         QStringList({QStringLiteral("whole"), QStringLiteral("capture")}));
						QCOMPARE(aliases.at(1).attributes.value(QStringLiteral("group")),
						         QStringLiteral("other-group"));
						break;
					}
					case RuleKind::Timer:
					{
						const QList<WorldRuntime::Timer> &timers = runtime.timers();
						QCOMPARE(timers.size(), 2);
						QCOMPARE(timers.at(0).runtimeId, targetRuntimeId);
						QCOMPARE(timers.at(0).attributes.value(QStringLiteral("group")),
						         QStringLiteral("edited-group"));
						QCOMPARE(timers.at(0).firedCount, 9);
						QCOMPARE(timers.at(0).invocationCount, 4);
						QCOMPARE(timers.at(0).lastFired, QDateTime::fromSecsSinceEpoch(1'700'000'000));
						QCOMPARE(timers.at(0).nextFireTime, QDateTime::fromSecsSinceEpoch(1'700'000'300));
						QCOMPARE(timers.at(1).attributes.value(QStringLiteral("group")),
						         QStringLiteral("other-group"));
						break;
					}
					}
				}
			}

			void staleRuleViewEditResolvesRuntimeIdentityBeforeOpeningEditor()
			{
				const QList<RuleKind> ruleKinds = {RuleKind::Trigger, RuleKind::Alias, RuleKind::Timer};
				for (const RuleKind kind : ruleKinds)
				{
					WorldRuntime runtime;
					populateRuleSelectionFixtures(runtime, kind, false, false);
					switch (kind)
					{
					case RuleKind::Trigger:
					{
						QList<WorldRuntime::Trigger> triggers = runtime.triggers();
						triggers[1].attributes.insert(QStringLiteral("name"), QStringLiteral("target_rule"));
						triggers[1].attributes.insert(QStringLiteral("group"),
						                              QStringLiteral("target-group"));
						runtime.setTriggers(triggers);
						break;
					}
					case RuleKind::Alias:
					{
						QList<WorldRuntime::Alias> aliases = runtime.aliases();
						aliases[1].attributes.insert(QStringLiteral("name"), QStringLiteral("target_rule"));
						aliases[1].attributes.insert(QStringLiteral("group"), QStringLiteral("target-group"));
						aliases[1].children.insert(QStringLiteral("send"), QStringLiteral("noop"));
						runtime.setAliases(aliases);
						break;
					}
					case RuleKind::Timer:
					{
						QList<WorldRuntime::Timer> timers = runtime.timers();
						timers[1].attributes.insert(QStringLiteral("name"), QStringLiteral("target_rule"));
						timers[1].attributes.insert(QStringLiteral("group"), QStringLiteral("target-group"));
						timers[1].children.insert(QStringLiteral("send"), QStringLiteral("noop"));
						runtime.setTimers(timers);
						break;
					}
					}

					WorldPreferencesDialog dialog(&runtime, nullptr);
					dialog.setInitialPage(rulePage(kind));
					const RuleViewWidgets widgets = findRuleViewWidgets(dialog, kind);
					QVERIFY(widgets.table);
					QVERIFY(widgets.editButton);

					quint64 targetRuntimeId{0};
					switch (kind)
					{
					case RuleKind::Trigger:
						targetRuntimeId = runtime.triggers().at(1).runtimeId;
						break;
					case RuleKind::Alias:
						targetRuntimeId = runtime.aliases().at(1).runtimeId;
						break;
					case RuleKind::Timer:
						targetRuntimeId = runtime.timers().at(1).runtimeId;
						break;
					}
					QVERIFY(targetRuntimeId != 0);
					const int targetRow = findRuleRowByRuntimeId(*widgets.table, targetRuntimeId);
					QVERIFY(targetRow >= 0);
					widgets.table->setCurrentCell(targetRow, 0);

					switch (kind)
					{
					case RuleKind::Trigger:
					{
						QList<WorldRuntime::Trigger> triggers = runtime.triggers();
						triggers.removeFirst();
						runtime.setTriggers(triggers);
						break;
					}
					case RuleKind::Alias:
					{
						QList<WorldRuntime::Alias> aliases = runtime.aliases();
						aliases.removeFirst();
						runtime.setAliases(aliases);
						break;
					}
					case RuleKind::Timer:
					{
						QList<WorldRuntime::Timer> timers = runtime.timers();
						timers.removeFirst();
						runtime.setTimers(timers);
						break;
					}
					}

					QString editorCallbackFailure;
					bool    editorCallbackInvoked = false;
					QVERIFY(QMetaObject::invokeMethod(
					    &dialog,
					    [&editorCallbackFailure, &editorCallbackInvoked]
					    {
						    editorCallbackInvoked = true;
						    QWidget *activeModal  = QApplication::activeModalWidget();
						    auto    *editor       = qobject_cast<QDialog *>(activeModal);
						    if (!editor)
						    {
							    editorCallbackFailure = QStringLiteral("Rule editor was not active.");
							    if (activeModal)
								    activeModal->close();
							    return;
						    }
						    QLineEdit *groupEdit = nullptr;
						    for (QLineEdit *lineEdit : editor->findChildren<QLineEdit *>())
						    {
							    if (lineEdit && lineEdit->text() == QStringLiteral("target-group"))
							    {
								    groupEdit = lineEdit;
								    break;
							    }
						    }
						    if (!groupEdit)
						    {
							    editorCallbackFailure =
							        QStringLiteral("Rule editor group field was not found.");
							    editor->reject();
							    return;
						    }
						    groupEdit->setText(QStringLiteral("edited-group"));
						    auto *buttons = editor->findChild<QDialogButtonBox *>();
						    if (!buttons || !buttons->button(QDialogButtonBox::Ok))
						    {
							    editorCallbackFailure =
							        QStringLiteral("Rule editor OK button was not found.");
							    editor->reject();
							    return;
						    }
						    buttons->button(QDialogButtonBox::Ok)->click();
					    },
					    Qt::QueuedConnection));
					widgets.editButton->click();
					QVERIFY2(editorCallbackInvoked, "Rule editor callback did not run.");
					QVERIFY2(editorCallbackFailure.isEmpty(), qPrintable(editorCallbackFailure));
					dialog.accept();

					switch (kind)
					{
					case RuleKind::Trigger:
						QCOMPARE(runtime.triggers().at(0).runtimeId, targetRuntimeId);
						QCOMPARE(runtime.triggers().at(0).attributes.value(QStringLiteral("group")),
						         QStringLiteral("edited-group"));
						break;
					case RuleKind::Alias:
						QCOMPARE(runtime.aliases().at(0).runtimeId, targetRuntimeId);
						QCOMPARE(runtime.aliases().at(0).attributes.value(QStringLiteral("group")),
						         QStringLiteral("edited-group"));
						break;
					case RuleKind::Timer:
						QCOMPARE(runtime.timers().at(0).runtimeId, targetRuntimeId);
						QCOMPARE(runtime.timers().at(0).attributes.value(QStringLiteral("group")),
						         QStringLiteral("edited-group"));
						break;
					}
				}
			}

			void ruleRemovalReResolvesIdentityAfterConfirmation()
			{
				AppController app;
				QCOMPARE(app.getGlobalOption(QStringLiteral("TriggerRemoveCheck")).toInt(), 1);
				const QList<RuleKind> ruleKinds = {RuleKind::Trigger, RuleKind::Alias, RuleKind::Timer};
				for (const RuleKind kind : ruleKinds)
				{
					for (const bool treeMode : {false, true})
					{
						for (const bool removeSelectedDuringConfirmation : {false, true})
						{
							WorldRuntime runtime;
							populateRuleSelectionFixtures(runtime, kind, treeMode, false);
							WorldPreferencesDialog dialog(&runtime, nullptr);
							dialog.setInitialPage(rulePage(kind));
							const RuleViewWidgets widgets = findRuleViewWidgets(dialog, kind);
							QVERIFY(widgets.table);
							QVERIFY(widgets.tree);
							QVERIFY(widgets.removeButton);

							quint64 targetRuntimeId{0};
							quint64 secondSelectedRuntimeId{0};
							quint64 expectedSelectionRuntimeId{0};
							switch (kind)
							{
							case RuleKind::Trigger:
								targetRuntimeId            = runtime.triggers().at(1).runtimeId;
								secondSelectedRuntimeId    = runtime.triggers().at(0).runtimeId;
								expectedSelectionRuntimeId = runtime.triggers().at(2).runtimeId;
								break;
							case RuleKind::Alias:
								targetRuntimeId            = runtime.aliases().at(1).runtimeId;
								secondSelectedRuntimeId    = runtime.aliases().at(0).runtimeId;
								expectedSelectionRuntimeId = runtime.aliases().at(2).runtimeId;
								break;
							case RuleKind::Timer:
								targetRuntimeId            = runtime.timers().at(1).runtimeId;
								secondSelectedRuntimeId    = runtime.timers().at(0).runtimeId;
								expectedSelectionRuntimeId = runtime.timers().at(2).runtimeId;
								break;
							}
							if (removeSelectedDuringConfirmation)
							{
								if (treeMode)
									selectRuleTreeItems(*widgets.tree,
									                    {secondSelectedRuntimeId, targetRuntimeId},
									                    targetRuntimeId);
								else
								{
									const int secondRow =
									    findRuleRowByRuntimeId(*widgets.table, secondSelectedRuntimeId);
									const int targetRow =
									    findRuleRowByRuntimeId(*widgets.table, targetRuntimeId);
									QVERIFY(secondRow >= 0);
									QVERIFY(targetRow >= 0);
									selectRuleRows(*widgets.table, {secondRow, targetRow}, targetRow);
								}
							}
							else
								selectRuleInActiveView(widgets, treeMode, targetRuntimeId);
							if (treeMode)
							{
								QVERIFY(widgets.tree->currentItem());
								QCOMPARE(widgets.tree->currentItem()->data(0, Qt::UserRole).toULongLong(),
								         targetRuntimeId);
							}

							QString confirmationCallbackFailure;
							bool    confirmationCallbackInvoked = false;
							QVERIFY(QMetaObject::invokeMethod(
							    &dialog,
							    [&runtime, &confirmationCallbackFailure, &confirmationCallbackInvoked, kind,
							     removeSelectedDuringConfirmation]
							    {
								    confirmationCallbackInvoked = true;
								    switch (kind)
								    {
								    case RuleKind::Trigger:
								    {
									    QList<WorldRuntime::Trigger> triggers = runtime.triggers();
									    triggers.removeAt(removeSelectedDuringConfirmation ? 1 : 0);
									    runtime.setTriggers(triggers);
									    break;
								    }
								    case RuleKind::Alias:
								    {
									    QList<WorldRuntime::Alias> aliases = runtime.aliases();
									    aliases.removeAt(removeSelectedDuringConfirmation ? 1 : 0);
									    runtime.setAliases(aliases);
									    break;
								    }
								    case RuleKind::Timer:
								    {
									    QList<WorldRuntime::Timer> timers = runtime.timers();
									    timers.removeAt(removeSelectedDuringConfirmation ? 1 : 0);
									    runtime.setTimers(timers);
									    break;
								    }
								    }
								    QWidget *activeModal = QApplication::activeModalWidget();
								    auto    *message     = qobject_cast<QMessageBox *>(activeModal);
								    if (!message)
								    {
									    confirmationCallbackFailure =
									        QStringLiteral("Removal confirmation was not active.");
									    if (activeModal)
										    activeModal->close();
									    return;
								    }
								    QAbstractButton *yesButton = message->button(QMessageBox::Yes);
								    if (!yesButton)
								    {
									    confirmationCallbackFailure =
									        QStringLiteral("Removal confirmation Yes button was not found.");
									    message->reject();
									    return;
								    }
								    yesButton->click();
							    },
							    Qt::QueuedConnection));
							widgets.removeButton->click();
							QVERIFY2(confirmationCallbackInvoked,
							         "Removal confirmation callback did not run.");
							QVERIFY2(confirmationCallbackFailure.isEmpty(),
							         qPrintable(confirmationCallbackFailure));
							QCOMPARE(widgets.table->rowCount(), 2);
							if (removeSelectedDuringConfirmation)
							{
								QVERIFY(findRuleRowByRuntimeId(*widgets.table, targetRuntimeId) >= 0);
								QCOMPARE(findRuleRowByRuntimeId(*widgets.table, secondSelectedRuntimeId), -1);
							}

							switch (kind)
							{
							case RuleKind::Trigger:
								QCOMPARE(runtime.triggers().size(), 2);
								break;
							case RuleKind::Alias:
								QCOMPARE(runtime.aliases().size(), 2);
								break;
							case RuleKind::Timer:
								QCOMPARE(runtime.timers().size(), 2);
								break;
							}
							if (treeMode)
							{
								QVERIFY(widgets.tree->currentItem());
								QCOMPARE(widgets.tree->currentItem()->data(0, Qt::UserRole).toULongLong(),
								         expectedSelectionRuntimeId);
								QVERIFY(widgets.tree->currentItem()->parent());
								QCOMPARE(widgets.tree->currentItem()->parent()->text(0),
								         QStringLiteral("primary"));
							}
							else
							{
								const int remainingRow =
								    findRuleRowByRuntimeId(*widgets.table, expectedSelectionRuntimeId);
								QVERIFY(remainingRow >= 0);
								QCOMPARE(widgets.table->currentRow(), remainingRow);
							}
						}
					}
				}
			}

			void rejectedRemovalLeavesStaleRuleViewUntouched()
			{
				AppController app;
				QCOMPARE(app.getGlobalOption(QStringLiteral("TriggerRemoveCheck")).toInt(), 1);
				for (const RuleKind kind : {RuleKind::Trigger, RuleKind::Alias, RuleKind::Timer})
				{
					for (const bool treeMode : {false, true})
					{
						WorldRuntime runtime;
						populateRuleSelectionFixtures(runtime, kind, treeMode, false);
						WorldPreferencesDialog dialog(&runtime, nullptr);
						dialog.setInitialPage(rulePage(kind));
						const RuleViewWidgets widgets = findRuleViewWidgets(dialog, kind);
						QVERIFY(widgets.table);
						QVERIFY(widgets.tree);
						QVERIFY(widgets.removeButton);

						const QList<quint64> selectedRuntimeIds = {
						    widgets.table->item(0, 0)->data(Qt::UserRole).toULongLong(),
						    widgets.table->item(2, 0)->data(Qt::UserRole).toULongLong()};
						const quint64 removedRuntimeId = selectedRuntimeIds.constFirst();
						if (treeMode)
							selectRuleTreeItems(*widgets.tree, selectedRuntimeIds,
							                    selectedRuntimeIds.constLast());
						else
							selectRuleRows(*widgets.table, {0, 2}, 2);
						bool confirmationHandled = false;
						QVERIFY(QMetaObject::invokeMethod(
						    &dialog,
						    [&runtime, &confirmationHandled, kind, removedRuntimeId]
						    {
							    switch (kind)
							    {
							    case RuleKind::Trigger:
							    {
								    QList<WorldRuntime::Trigger> rules = runtime.triggers();
								    rules.removeIf([removedRuntimeId](const auto &rule)
								                   { return rule.runtimeId == removedRuntimeId; });
								    runtime.setTriggers(rules);
								    break;
							    }
							    case RuleKind::Alias:
							    {
								    QList<WorldRuntime::Alias> rules = runtime.aliases();
								    rules.removeIf([removedRuntimeId](const auto &rule)
								                   { return rule.runtimeId == removedRuntimeId; });
								    runtime.setAliases(rules);
								    break;
							    }
							    case RuleKind::Timer:
							    {
								    QList<WorldRuntime::Timer> rules = runtime.timers();
								    rules.removeIf([removedRuntimeId](const auto &rule)
								                   { return rule.runtimeId == removedRuntimeId; });
								    runtime.setTimers(rules);
								    break;
							    }
							    }
							    auto *const message =
							        qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
							    if (!message)
								    return;
							    confirmationHandled = true;
							    message->button(QMessageBox::No)->click();
						    },
						    Qt::QueuedConnection));
						widgets.removeButton->click();
						QVERIFY(confirmationHandled);
						QCOMPARE(widgets.table->rowCount(), 3);
						QVERIFY(findRuleRowByRuntimeId(*widgets.table, removedRuntimeId) >= 0);
						QCOMPARE(selectedRuleIds(*widgets.table),
						         treeMode ? QList<quint64>{} : selectedRuntimeIds);
						QCOMPARE(selectedRuleIds(*widgets.tree),
						         treeMode ? selectedRuntimeIds : QList<quint64>{});
					}
				}
			}

			void staleRuleViewRemovalDoesNotDeleteAnotherRule()
			{
				AppController app;
				QCOMPARE(app.getGlobalOption(QStringLiteral("TriggerRemoveCheck")).toInt(), 1);
				const QList<RuleKind> ruleKinds = {RuleKind::Trigger, RuleKind::Alias, RuleKind::Timer};
				for (const RuleKind kind : ruleKinds)
				{
					for (const bool treeMode : {false, true})
					{
						WorldRuntime runtime;
						populateRuleSelectionFixtures(runtime, kind, treeMode, false);
						WorldPreferencesDialog dialog(&runtime, nullptr);
						dialog.setInitialPage(rulePage(kind));
						const RuleViewWidgets widgets = findRuleViewWidgets(dialog, kind);
						QVERIFY(widgets.table);
						QVERIFY(widgets.tree);
						QVERIFY(widgets.removeButton);

						quint64 targetRuntimeId{0};
						quint64 expectedSelectionRuntimeId{0};
						quint64 otherRuntimeId{0};
						switch (kind)
						{
						case RuleKind::Trigger:
							targetRuntimeId            = runtime.triggers().at(1).runtimeId;
							expectedSelectionRuntimeId = runtime.triggers().at(0).runtimeId;
							otherRuntimeId             = runtime.triggers().at(2).runtimeId;
							break;
						case RuleKind::Alias:
							targetRuntimeId            = runtime.aliases().at(1).runtimeId;
							expectedSelectionRuntimeId = runtime.aliases().at(0).runtimeId;
							otherRuntimeId             = runtime.aliases().at(2).runtimeId;
							break;
						case RuleKind::Timer:
							targetRuntimeId            = runtime.timers().at(1).runtimeId;
							expectedSelectionRuntimeId = runtime.timers().at(0).runtimeId;
							otherRuntimeId             = runtime.timers().at(2).runtimeId;
							break;
						}
						selectRuleInActiveView(widgets, treeMode, targetRuntimeId);
						if (treeMode)
						{
							QVERIFY(widgets.tree->currentItem());
							QCOMPARE(widgets.tree->currentItem()->data(0, Qt::UserRole).toULongLong(),
							         targetRuntimeId);
						}

						switch (kind)
						{
						case RuleKind::Trigger:
						{
							QList<WorldRuntime::Trigger> triggers = runtime.triggers();
							triggers.removeAt(1);
							runtime.setTriggers(triggers);
							break;
						}
						case RuleKind::Alias:
						{
							QList<WorldRuntime::Alias> aliases = runtime.aliases();
							aliases.removeAt(1);
							runtime.setAliases(aliases);
							break;
						}
						case RuleKind::Timer:
						{
							QList<WorldRuntime::Timer> timers = runtime.timers();
							timers.removeAt(1);
							runtime.setTimers(timers);
							break;
						}
						}

						bool removeClickReturned    = false;
						bool unexpectedConfirmation = false;
						QVERIFY(QMetaObject::invokeMethod(
						    &dialog,
						    [&removeClickReturned, &unexpectedConfirmation]
						    {
							    if (removeClickReturned)
								    return;
							    if (QWidget *activeModal = QApplication::activeModalWidget())
							    {
								    unexpectedConfirmation = true;
								    activeModal->close();
							    }
						    },
						    Qt::QueuedConnection));
						widgets.removeButton->click();
						removeClickReturned = true;
						QCoreApplication::sendPostedEvents(&dialog, QEvent::MetaCall);
						QVERIFY(!unexpectedConfirmation);
						QCOMPARE(widgets.table->rowCount(), 3);

						switch (kind)
						{
						case RuleKind::Trigger:
							QCOMPARE(runtime.triggers().size(), 2);
							QCOMPARE(runtime.triggers().at(0).runtimeId, expectedSelectionRuntimeId);
							QCOMPARE(runtime.triggers().at(1).runtimeId, otherRuntimeId);
							break;
						case RuleKind::Alias:
							QCOMPARE(runtime.aliases().size(), 2);
							QCOMPARE(runtime.aliases().at(0).runtimeId, expectedSelectionRuntimeId);
							QCOMPARE(runtime.aliases().at(1).runtimeId, otherRuntimeId);
							break;
						case RuleKind::Timer:
							QCOMPARE(runtime.timers().size(), 2);
							QCOMPARE(runtime.timers().at(0).runtimeId, expectedSelectionRuntimeId);
							QCOMPARE(runtime.timers().at(1).runtimeId, otherRuntimeId);
							break;
						}
						if (treeMode)
						{
							QVERIFY(widgets.tree->currentItem());
							QCOMPARE(widgets.tree->currentItem()->data(0, Qt::UserRole).toULongLong(),
							         targetRuntimeId);
							QVERIFY(widgets.tree->currentItem()->parent());
							QCOMPARE(widgets.tree->currentItem()->parent()->text(0),
							         QStringLiteral("primary"));
						}
						else
						{
							const int selectedRow = findRuleRowByRuntimeId(*widgets.table, targetRuntimeId);
							QVERIFY(selectedRow >= 0);
							QCOMPARE(widgets.table->currentRow(), selectedRow);
						}
					}
				}
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

#include "tst_Dialog_WorldPreferences.moc"
