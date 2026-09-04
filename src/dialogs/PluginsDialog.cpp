/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: PluginsDialog.cpp
 * Role: Plugin management dialog implementation coordinating plugin inventory, state changes, and related commands.
 */

#include "PluginsDialog.h"

#include "AppController.h"
#include "MainFrame.h"
#include "WorldRuntime.h"
#include "helpers/DialogSizingUtils.h"
#include "scripting/ScriptingErrors.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QColor>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPalette>
#include <QPushButton>
#include <QSettings>
#include <QStyle>
#include <QVBoxLayout>
#include <QVector>
#include <QWindow>
#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <utility>

namespace
{
	constexpr int   kColumnName    = 0;
	constexpr int   kColumnPurpose = 1;
	constexpr int   kColumnAuthor  = 2;
	constexpr int   kColumnFile    = 3;
	constexpr int   kColumnEnabled = 4;
	constexpr int   kColumnVersion = 5;
	constexpr int   kColumnCount   = 6;
	constexpr int   kHeaderVersion = 2;
	constexpr QSize kPluginsBaselineMinimum(1250, 420);

	/**
	 * @brief Header exposing fresh content metrics and reporting only completed user resizes.
	 */
	class FontAwareHeaderView final : public QHeaderView
	{
		public:
			using UserResizeCallback = std::function<void(const QVector<int> &, const QVector<int> &)>;

			explicit FontAwareHeaderView(QWidget *parent) : QHeaderView(Qt::Horizontal, parent)
			{
			}

			/**
			 * @brief Returns the style-derived content width for a logical section.
			 * @param section Logical section index.
			 * @return Width required by the section's current text, icon, font, and sort indicator.
			 */
			[[nodiscard]] int sectionContentsWidth(const int section) const
			{
				return sectionSizeFromContents(section).width();
			}

			/**
			 * @brief Installs the callback invoked after a user changes section widths.
			 * @param callback Callback to invoke after a completed mouse resize.
			 */
			void setUserResizeCallback(UserResizeCallback callback)
			{
				m_userResizeCallback = std::move(callback);
			}

		protected:
			/**
			 * @brief Reports whether a point lies on a visible section-resize grip.
			 * @param position Header viewport position to inspect.
			 * @return `true` when the position is within the current style's grip margin.
			 */
			[[nodiscard]] bool isSectionResizeHandle(const QPoint position) const
			{
				const int gripMargin =
				    qMax(2, style()->pixelMetric(QStyle::PM_HeaderGripMargin, nullptr, this));
				for (int section = 0; section < count(); ++section)
				{
					if (isSectionHidden(section))
						continue;
					const qint64 boundary =
					    static_cast<qint64>(sectionViewportPosition(section)) + sectionSize(section);
					if (qAbs(static_cast<qint64>(position.x()) - boundary) <= gripMargin)
						return true;
				}
				return false;
			}

			/**
			 * @brief Captures section widths at the start of a possible user resize.
			 * @param event Mouse press event.
			 */
			void mousePressEvent(QMouseEvent *event) override
			{
				m_widthsBeforeUserInteraction.clear();
				if (event && event->button() == Qt::LeftButton &&
				    isSectionResizeHandle(event->position().toPoint()))
				{
					m_widthsBeforeUserInteraction.reserve(count());
					for (int section = 0; section < count(); ++section)
						m_widthsBeforeUserInteraction.push_back(sectionSize(section));
				}
				QHeaderView::mousePressEvent(event);
			}

			/**
			 * @brief Reports a completed user resize while ignoring programmatic section changes.
			 * @param event Mouse release event.
			 */
			void mouseReleaseEvent(QMouseEvent *event) override
			{
				QHeaderView::mouseReleaseEvent(event);
				QVector<int> widthsAfterUserInteraction;
				widthsAfterUserInteraction.reserve(count());
				for (int section = 0; section < count(); ++section)
					widthsAfterUserInteraction.push_back(sectionSize(section));
				if (m_widthsBeforeUserInteraction.size() == count() &&
				    m_widthsBeforeUserInteraction != widthsAfterUserInteraction && m_userResizeCallback)
				{
					m_userResizeCallback(m_widthsBeforeUserInteraction, widthsAfterUserInteraction);
				}
				m_widthsBeforeUserInteraction.clear();
			}

		private:
			QVector<int>       m_widthsBeforeUserInteraction;
			UserResizeCallback m_userResizeCallback;
	};

	int headerSectionWidth(const QTableWidget *table, const int column)
	{
		if (!table || !table->horizontalHeaderItem(column))
			return 0;
		const auto *header = dynamic_cast<const FontAwareHeaderView *>(table->horizontalHeader());
		return header ? header->sectionContentsWidth(column) : 0;
	}

	QVector<int> columnWidths(const QTableWidget *table)
	{
		QVector<int> widths;
		if (!table)
			return widths;
		widths.reserve(table->columnCount());
		for (int column = 0; column < table->columnCount(); ++column)
			widths.push_back(table->columnWidth(column));
		return widths;
	}

	QVector<int> columnWidthsFromSettings(const QVariant &value)
	{
		const QVariantList values = value.toList();
		QVector<int>       widths;
		widths.reserve(values.size());
		for (const QVariant &entry : values)
		{
			const int width = entry.toInt();
			if (width <= 0)
				return {};
			widths.push_back(width);
		}
		return widths;
	}

	QVariantList columnWidthsForSettings(const QVector<int> &widths)
	{
		QVariantList values;
		values.reserve(widths.size());
		for (const int width : widths)
			values.push_back(width);
		return values;
	}

	void applyPreferredColumnWidths(QTableWidget *table, const QVector<int> &preferredWidths)
	{
		if (!table || preferredWidths.size() != table->columnCount())
			return;
		for (int column = 0; column < table->columnCount(); ++column)
		{
			const int effectiveWidth = qMax(preferredWidths.at(column), headerSectionWidth(table, column));
			table->setColumnWidth(column, effectiveWidth);
		}
	}

	void applyDefaultColumnWidths(QTableWidget *table)
	{
		if (!table)
			return;
		const int total = table->viewport()->width();
		if (total <= 0)
			return;
		const int nameWidth = qMax(headerSectionWidth(table, kColumnName),
		                           qMin(200, qMax(120, static_cast<int>(total * 0.18))));
		const int authorWidth =
		    qMax(headerSectionWidth(table, kColumnAuthor), qMax(120, static_cast<int>(total * 0.12)));
		const int enabledWidth = qMax(70, headerSectionWidth(table, kColumnEnabled));
		const int versionWidth = qMax(60, headerSectionWidth(table, kColumnVersion));
		int       remaining    = total - (nameWidth + authorWidth + enabledWidth + versionWidth);
		if (remaining < 200)
			remaining = 200;
		int fileWidth =
		    qMax(headerSectionWidth(table, kColumnFile), qMax(220, static_cast<int>(remaining * 0.55)));
		int purposeWidth = qMax(headerSectionWidth(table, kColumnPurpose), remaining - fileWidth);
		if (purposeWidth < 180)
		{
			purposeWidth = 180;
			fileWidth    = qMax(headerSectionWidth(table, kColumnFile), qMax(200, remaining - purposeWidth));
		}
		table->setColumnWidth(kColumnName, nameWidth);
		table->setColumnWidth(kColumnPurpose, purposeWidth);
		table->setColumnWidth(kColumnAuthor, authorWidth);
		table->setColumnWidth(kColumnFile, fileWidth);
		table->setColumnWidth(kColumnEnabled, enabledWidth);
		table->setColumnWidth(kColumnVersion, versionWidth);
	}

	void clampColumnsToViewport(QTableWidget *table)
	{
		if (!table)
			return;
		const int total = table->viewport()->width();
		if (total <= 0)
			return;
		int sum = 0;
		for (int i = 0; i < kColumnCount; ++i)
			sum += table->columnWidth(i);
		if (sum > total)
			applyDefaultColumnWidths(table);
	}

	bool findPluginById(const WorldRuntime *runtime, const QString &pluginId, WorldRuntime::Plugin &out)
	{
		if (!runtime)
			return false;
		const QList<WorldRuntime::Plugin> &plugins = runtime->plugins();
		for (const WorldRuntime::Plugin &plugin : plugins)
		{
			const QString id = plugin.attributes.value(QStringLiteral("id"));
			if (id == pluginId)
			{
				out = plugin;
				return true;
			}
		}
		return false;
	}
} // namespace

PluginsDialog::PluginsDialog(WorldRuntime *runtime, MainWindow *main, QWidget *parent)
    : QDialog(parent), m_runtime(runtime), m_main(main)
{
	setWindowTitle(QStringLiteral("Plugins"));
	setModal(true);
	resize(kPluginsBaselineMinimum);

	auto  root    = std::make_unique<QVBoxLayout>();
	auto *rootPtr = root.get();
	setLayout(root.release());

	auto *table  = new QTableWidget(this);
	m_table      = table;
	auto *header = new FontAwareHeaderView(m_table);
	m_table->setHorizontalHeader(header);
	header->setSectionsClickable(true);
	m_table->setColumnCount(kColumnCount);
	m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
	m_table->setTabKeyNavigation(false);
	m_table->setSortingEnabled(true);
	m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
	m_table->horizontalHeader()->setStretchLastSection(false);
	m_table->setTextElideMode(Qt::ElideRight);
	m_table->setWordWrap(false);
	m_table->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Purpose"),
	                                    QStringLiteral("Author"), QStringLiteral("File"),
	                                    QStringLiteral("Enabled"), QStringLiteral("Ver")});

	rootPtr->addWidget(m_table, 1);

	auto  buttonGrid    = std::make_unique<QGridLayout>();
	auto *buttonGridPtr = buttonGrid.get();
	buttonGrid->setHorizontalSpacing(0);
	buttonGrid->setVerticalSpacing(6);
	auto *addButton         = new QPushButton(QStringLiteral("Add..."), this);
	auto *removeButton      = new QPushButton(QStringLiteral("Remove"), this);
	auto *deleteStateButton = new QPushButton(QStringLiteral("Delete State"), this);
	auto *moveUpButton      = new QPushButton(QStringLiteral("Move Up"), this);
	auto *moveDownButton    = new QPushButton(QStringLiteral("Move Down"), this);
	auto *reloadButton      = new QPushButton(QStringLiteral("ReInstall"), this);
	auto *showInfoButton    = new QPushButton(QStringLiteral("Show Info"), this);
	auto *enableButton      = new QPushButton(QStringLiteral("Enable"), this);
	auto *disableButton     = new QPushButton(QStringLiteral("Disable"), this);
	auto *editButton        = new QPushButton(QStringLiteral("Edit"), this);
	auto  closeButton       = std::make_unique<QPushButton>(QStringLiteral("Close"), this);
	auto *closeButtonPtr    = closeButton.get();
	m_addButton             = addButton;
	m_removeButton          = removeButton;
	m_deleteStateButton     = deleteStateButton;
	m_moveUpButton          = moveUpButton;
	m_moveDownButton        = moveDownButton;
	m_reloadButton          = reloadButton;
	m_showDescriptionButton = showInfoButton;
	m_enableButton          = enableButton;
	m_disableButton         = disableButton;
	m_editButton            = editButton;
	m_closeButton           = closeButtonPtr;

	m_moveUpButton->setVisible(false);
	m_moveDownButton->setVisible(false);

	QPalette deletePalette = m_deleteStateButton->palette();
	deletePalette.setColor(QPalette::ButtonText, Qt::red);
	m_deleteStateButton->setPalette(deletePalette);
	m_deleteStateButton->setToolTip(
	    QStringLiteral("Irreversible: Deletes the saved state of the selected plugin."));

	buttonGridPtr->addWidget(m_addButton, 0, 0, Qt::AlignLeft);
	buttonGridPtr->addWidget(m_removeButton, 1, 0, Qt::AlignLeft);
	buttonGridPtr->addWidget(m_deleteStateButton, 1, 1, Qt::AlignHCenter);
	buttonGridPtr->addWidget(m_reloadButton, 0, 2, Qt::AlignHCenter);
	buttonGridPtr->addWidget(m_showDescriptionButton, 1, 2, Qt::AlignHCenter);
	buttonGridPtr->addWidget(m_enableButton, 0, 4, Qt::AlignHCenter);
	buttonGridPtr->addWidget(m_disableButton, 1, 4, Qt::AlignHCenter);
	buttonGridPtr->addWidget(m_editButton, 0, 6, Qt::AlignRight);
	buttonGridPtr->addWidget(closeButton.release(), 1, 6, Qt::AlignRight);
	buttonGridPtr->setColumnStretch(1, 1);
	buttonGridPtr->setColumnStretch(3, 1);
	buttonGridPtr->setColumnStretch(5, 1);
	rootPtr->addLayout(buttonGrid.release());

	connect(m_addButton, &QPushButton::clicked, this, &PluginsDialog::onAddPlugin);
	connect(m_removeButton, &QPushButton::clicked, this, &PluginsDialog::onRemovePlugin);
	connect(m_deleteStateButton, &QPushButton::clicked, this, &PluginsDialog::onDeleteState);
	connect(m_moveUpButton, &QPushButton::clicked, this, &PluginsDialog::onMoveUp);
	connect(m_moveDownButton, &QPushButton::clicked, this, &PluginsDialog::onMoveDown);
	connect(m_reloadButton, &QPushButton::clicked, this, &PluginsDialog::onReloadPlugin);
	connect(m_editButton, &QPushButton::clicked, this, &PluginsDialog::onEditPlugin);
	connect(m_enableButton, &QPushButton::clicked, this, &PluginsDialog::onEnablePlugin);
	connect(m_disableButton, &QPushButton::clicked, this, &PluginsDialog::onDisablePlugin);
	connect(m_showDescriptionButton, &QPushButton::clicked, this, &PluginsDialog::onShowDescription);
	connect(closeButtonPtr, &QPushButton::clicked, this, &PluginsDialog::reject);
	connect(this, &QDialog::finished, this, [this] { saveSettings(); });
	connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged, this,
	        &PluginsDialog::onSelectionChanged);

	reloadList();
	{
		QSettings settings(AppController::instance()->iniFilePath(), QSettings::IniFormat);
		settings.beginGroup(QStringLiteral("PluginsDialog"));
		if (const QByteArray geometry = settings.value(QStringLiteral("Geometry")).toByteArray();
		    !geometry.isEmpty())
			restoreGeometry(geometry);
		const int        headerVersion = settings.value(QStringLiteral("HeaderVersion"), 0).toInt();
		const QByteArray headerState   = settings.value(QStringLiteral("HeaderState")).toByteArray();
		bool             restored      = false;
		if (headerVersion == kHeaderVersion && !headerState.isEmpty())
			restored = m_table->horizontalHeader()->restoreState(headerState);
		m_usingDefaultColumnWidths =
		    !restored || settings.value(QStringLiteral("UsingDefaultColumnWidths"), false).toBool();
		m_preferredColumnWidths =
		    columnWidthsFromSettings(settings.value(QStringLiteral("PreferredColumnWidths")));
		if (m_usingDefaultColumnWidths)
		{
			applyDefaultColumnWidths(m_table);
			clampColumnsToViewport(m_table);
		}
		else
		{
			if (m_preferredColumnWidths.size() != m_table->columnCount())
				m_preferredColumnWidths = columnWidths(m_table);
			applyPreferredColumnWidths(m_table, m_preferredColumnWidths);
		}
		const int  sortColumn = settings.value(QStringLiteral("SortColumn"), kColumnName).toInt();
		const auto sortOrder  = static_cast<Qt::SortOrder>(
		    settings.value(QStringLiteral("SortOrder"), Qt::AscendingOrder).toInt());
		if (sortColumn >= 0)
			m_table->sortByColumn(sortColumn, sortOrder);
		settings.endGroup();
	}
	onSelectionChanged();
	header->setUserResizeCallback(
	    [this](const QVector<int> &before, const QVector<int> &after)
	    {
		    if (before.size() != m_table->columnCount() || after.size() != m_table->columnCount())
			    return;
		    if (m_usingDefaultColumnWidths || m_preferredColumnWidths.size() != m_table->columnCount())
			    m_preferredColumnWidths = before;
		    for (int column = 0; column < m_table->columnCount(); ++column)
		    {
			    if (before.at(column) != after.at(column))
				    m_preferredColumnWidths[column] = after.at(column);
		    }
		    m_usingDefaultColumnWidths = false;
		    applyPreferredColumnWidths(m_table, m_preferredColumnWidths);
	    });
	connect(header, SIGNAL(sortIndicatorChanged(int, Qt::SortOrder)), this,
	        SLOT(refreshColumnWidthsForSortChange()));
	refreshDialogSizing();

	QMetaObject::invokeMethod(
	    this,
	    [this]
	    {
		    if (m_usingDefaultColumnWidths)
		    {
			    applyDefaultColumnWidths(m_table);
			    clampColumnsToViewport(m_table);
		    }
	    },
	    Qt::QueuedConnection);
}

bool PluginsDialog::event(QEvent *event)
{
	if (!event)
		return QDialog::event(event);

	const bool  firstShow              = event->type() == QEvent::Show && !m_initialDialogSizingFinalized;
	const bool  applicationFontChanged = event->type() == QEvent::ApplicationFontChange;
	const QSize sizeBeforeShow         = firstShow ? size() : QSize();
	if (applicationFontChanged && m_initialDialogSizingFinalized && !m_dialogSizingRefreshPending)
		m_dialogSizeBeforeRefresh = size();
	const bool handled = QDialog::event(event);
	if (firstShow)
	{
		m_dialogSizeBeforeRefresh = sizeBeforeShow;
		if (QWindow *const nativeWindow = windowHandle())
		{
			nativeWindow->installEventFilter(this);
			if (nativeWindow->isExposed())
				finalizeInitialDialogSizing();
		}
	}
	if (applicationFontChanged && m_initialDialogSizingFinalized)
		scheduleDialogSizingRefresh();
	return handled;
}

bool PluginsDialog::eventFilter(QObject *watched, QEvent *event)
{
	QWindow *const nativeWindow = windowHandle();
	if (event && nativeWindow && watched == nativeWindow && event->type() == QEvent::Expose &&
	    nativeWindow->isExposed())
		finalizeInitialDialogSizing();
	return QDialog::eventFilter(watched, event);
}

void PluginsDialog::refreshDialogSizing()
{
	QLayout *const dialogLayout = layout();
	if (!dialogLayout)
		return;
	const QLayout::SizeConstraint originalSizeConstraint = dialogLayout->sizeConstraint();
	dialogLayout->setSizeConstraint(QLayout::SetNoConstraint);

	QPushButton *const buttons[] = {
	    m_addButton,    m_removeButton,  m_deleteStateButton, m_reloadButton, m_showDescriptionButton,
	    m_enableButton, m_disableButton, m_editButton,        m_closeButton};
	for (QPushButton *const button : buttons)
	{
		if (!button)
			continue;
		button->setMinimumWidth(0);
		button->setMaximumWidth(QWIDGETSIZE_MAX);
	}

	int buttonWidth = 0;
	for (QPushButton *const button : buttons)
	{
		if (button)
			buttonWidth = qMax(buttonWidth, button->sizeHint().width());
	}
	for (QPushButton *const button : buttons)
	{
		if (button)
			button->setFixedWidth(buttonWidth);
	}

	dialogLayout->invalidate();
	dialogLayout->activate();

	const QSize unboundedMinimum =
	    dialogLayout->minimumSize().expandedTo(dialogLayout->sizeHint()).expandedTo(kPluginsBaselineMinimum);
	applyDialogContentSize(unboundedMinimum);
	if (m_usingDefaultColumnWidths)
	{
		applyDefaultColumnWidths(m_table);
		clampColumnsToViewport(m_table);
	}
	else
		applyPreferredColumnWidths(m_table, m_preferredColumnWidths);
	dialogLayout->setSizeConstraint(originalSizeConstraint);
}

void PluginsDialog::refreshColumnWidthsForSortChange() const
{
	if (m_usingDefaultColumnWidths)
	{
		applyDefaultColumnWidths(m_table);
		clampColumnsToViewport(m_table);
	}
	else
	{
		applyPreferredColumnWidths(m_table, m_preferredColumnWidths);
	}
}

void PluginsDialog::applyDialogContentSize(const QSize contentMinimum)
{
	if (!contentMinimum.isValid())
		return;
	const QSize maximumSize      = DialogSizingUtils::maximumClientSize(this);
	const QSize requiredMinimum  = contentMinimum.boundedTo(maximumSize);
	const QSize sizeBeforeChange = m_dialogSizeBeforeRefresh.isValid() ? m_dialogSizeBeforeRefresh : size();
	const QSize desiredSize      = DialogSizingUtils::desiredClientSizeForContentChange(
	    sizeBeforeChange, m_lastAppliedDialogSize, m_desiredDialogSize, m_lastDialogContentMinimum,
	    contentMinimum);
	const QSize targetSize     = desiredSize.boundedTo(maximumSize);
	m_lastDialogContentMinimum = contentMinimum;
	m_desiredDialogSize        = desiredSize;
	m_dialogSizeBeforeRefresh  = {};
	const QSize relaxedMinimum = minimumSize().boundedTo(requiredMinimum);
	if (minimumSize() != relaxedMinimum)
		setMinimumSize(relaxedMinimum);
	if (isVisible())
		DialogSizingUtils::applyAvailableGeometry(this, targetSize);
	else
		resize(targetSize);
	m_lastAppliedDialogSize = size();
	if (minimumSize() != requiredMinimum)
		setMinimumSize(requiredMinimum);
}

void PluginsDialog::scheduleDialogSizingRefresh()
{
	if (m_dialogSizingRefreshPending)
		return;
	m_dialogSizingRefreshPending = true;
	QMetaObject::invokeMethod(
	    this,
	    [this]
	    {
		    m_dialogSizingRefreshPending = false;
		    refreshDialogSizing();
	    },
	    Qt::QueuedConnection);
}

void PluginsDialog::finalizeInitialDialogSizing()
{
	if (m_initialDialogSizingFinalized)
		return;
	m_initialDialogSizingFinalized = true;
	if (QWindow *const nativeWindow = windowHandle())
		nativeWindow->removeEventFilter(this);
	scheduleDialogSizingRefresh();
}

void PluginsDialog::reloadList() const
{
	m_table->setSortingEnabled(false);
	m_table->clearContents();
	m_table->setRowCount(0);

	if (!m_runtime)
		return;

	const QList<WorldRuntime::Plugin> &plugins    = m_runtime->plugins();
	const QStringList                  visibleIds = m_runtime->pluginIdList();
	QVector<int>                       visiblePluginRows;
	visiblePluginRows.reserve(plugins.size());
	for (int pluginIndex = 0; pluginIndex < plugins.size(); ++pluginIndex)
	{
		const QString pluginId = plugins.at(pluginIndex).attributes.value(QStringLiteral("id"));
		if (!pluginId.isEmpty() && !visibleIds.contains(pluginId))
			continue;
		visiblePluginRows.push_back(pluginIndex);
	}
	constexpr auto maxPluginRows = static_cast<qsizetype>(std::numeric_limits<int>::max());
	const int      pluginCount   = visiblePluginRows.size() > maxPluginRows
	                                   ? std::numeric_limits<int>::max()
	                                   : static_cast<int>(visiblePluginRows.size());
	m_table->setRowCount(pluginCount);

	for (int row = 0; row < pluginCount; ++row)
	{
		const WorldRuntime::Plugin &plugin   = plugins.at(visiblePluginRows.at(row));
		const QString               pluginId = plugin.attributes.value(QStringLiteral("id"));
		const QString               name     = plugin.attributes.value(QStringLiteral("name"));
		const QString               purpose =
		    plugin.nativeShim ? plugin.nativeShimMarker : plugin.attributes.value(QStringLiteral("purpose"));
		const QString author  = plugin.attributes.value(QStringLiteral("author"));
		const QString file    = plugin.source;
		const QString enabled = plugin.enabled ? QStringLiteral("Yes") : QStringLiteral("No");
		const QString version = QString::number(plugin.version, 'f', 2);

		auto          addItem = [&](const int column, const QString &text)
		{
			auto item = std::make_unique<QTableWidgetItem>(text);
			item->setData(Qt::UserRole, pluginId);
			item->setFlags(item->flags() & ~Qt::ItemIsEditable);
			if (plugin.nativeShim)
			{
				item->setForeground(QBrush(QColor(0, 120, 48)));
				item->setBackground(QBrush(QColor(223, 245, 229)));
			}
			m_table->setItem(row, column, item.release());
		};

		addItem(kColumnName, name.isEmpty() ? pluginId : name);
		addItem(kColumnAuthor, author);

		auto purposeItem = std::make_unique<QTableWidgetItem>(purpose);
		purposeItem->setData(Qt::UserRole, pluginId);
		purposeItem->setFlags(purposeItem->flags() & ~Qt::ItemIsEditable);
		purposeItem->setToolTip(purpose);
		if (plugin.nativeShim)
		{
			purposeItem->setForeground(QBrush(QColor(0, 120, 48)));
			purposeItem->setBackground(QBrush(QColor(223, 245, 229)));
		}
		m_table->setItem(row, kColumnPurpose, purposeItem.release());

		auto fileItem = std::make_unique<QTableWidgetItem>(file);
		fileItem->setData(Qt::UserRole, pluginId);
		fileItem->setFlags(fileItem->flags() & ~Qt::ItemIsEditable);
		fileItem->setToolTip(file);
		if (plugin.nativeShim)
		{
			fileItem->setForeground(QBrush(QColor(0, 120, 48)));
			fileItem->setBackground(QBrush(QColor(223, 245, 229)));
		}
		m_table->setItem(row, kColumnFile, fileItem.release());

		addItem(kColumnEnabled, enabled);
		addItem(kColumnVersion, version);
	}

	m_table->setSortingEnabled(true);
}

QString PluginsDialog::pluginIdForRow(const int row) const
{
	if (!m_table)
		return {};
	auto *const item = m_table->item(row, kColumnName);
	if (!item)
		return {};
	return item->data(Qt::UserRole).toString();
}

QList<int> PluginsDialog::selectedRows() const
{
	QList<int> rows;
	if (!m_table)
		return rows;
	const QModelIndexList selection = m_table->selectionModel()->selectedRows();
	rows.reserve(selection.size());
	for (const QModelIndex &index : selection)
		rows.push_back(index.row());
	return rows;
}

void PluginsDialog::onSelectionChanged() const
{
	const QList<int> rows            = selectedRows();
	const bool       hasSelection    = !rows.isEmpty();
	const bool       singleSelection = rows.size() == 1;
	m_removeButton->setEnabled(hasSelection);
	m_deleteStateButton->setEnabled(hasSelection);
	m_moveUpButton->setEnabled(singleSelection);
	m_moveDownButton->setEnabled(singleSelection);
	m_reloadButton->setEnabled(hasSelection);
	m_editButton->setEnabled(hasSelection);
	m_enableButton->setEnabled(hasSelection);
	m_disableButton->setEnabled(hasSelection);
	m_showDescriptionButton->setEnabled(hasSelection);
}

void PluginsDialog::onAddPlugin()
{
	if (!m_runtime)
		return;

	const QString initialDir = m_runtime->pluginsDirectory();
	const QString path =
	    QFileDialog::getOpenFileName(this, QStringLiteral("Select Plugin"), initialDir,
	                                 QStringLiteral("Plugin files (*.xml);;All files (*.*)"));
	if (path.isEmpty())
		return;
	installPluginFile(path);
}

void PluginsDialog::installPluginFile(const QString &path)
{
	if (!m_runtime)
		return;
	QString error;
	if (!m_runtime->loadPluginFile(path, &error, false))
	{
		QMessageBox::warning(this, QStringLiteral("Plugins"),
		                     error.isEmpty() ? QStringLiteral("Unable to load plugin.") : error);
		return;
	}
	reloadList();
}

void PluginsDialog::onRemovePlugin()
{
	if (!m_runtime)
		return;

	const QList<int> rows = selectedRows();
	if (rows.isEmpty())
		return;

	for (const int row : rows)
	{
		const QString pluginId = pluginIdForRow(row);
		if (pluginId.isEmpty())
			continue;
		QString error;
		if (!m_runtime->unloadPlugin(pluginId, &error))
		{
			QMessageBox::warning(this, QStringLiteral("Plugins"),
			                     error.isEmpty() ? QStringLiteral("Unable to unload plugin.") : error);
		}
	}
	reloadList();
}

void PluginsDialog::onDeleteState()
{
	if (!m_runtime)
		return;

	const QList<int> rows = selectedRows();
	if (rows.isEmpty())
		return;

	const QString worldId  = m_runtime->worldAttributes().value(QStringLiteral("id")).trimmed();
	const QString stateDir = m_runtime->stateFilesDirectory();
	if (worldId.isEmpty() || stateDir.isEmpty())
	{
		QMessageBox::warning(this, QStringLiteral("Plugins"),
		                     QStringLiteral("Unable to resolve plugin state file location."));
		return;
	}

	QStringList failures;
	for (const int row : rows)
	{
		const QString pluginId = pluginIdForRow(row);
		if (pluginId.isEmpty())
			continue;

		// Canonical state filenames are lowercase. Retain the uppercase candidate only to remove
		// files written by older releases; this does not alter the runtime plugin id.
		const QStringList candidates{pluginId, pluginId.toUpper()};

		for (const QString &candidateId : candidates)
		{
			const QString fileName  = worldId + QLatin1Char('-') + candidateId + QStringLiteral("-state.xml");
			const QString stateFile = QDir(stateDir).filePath(fileName);
			if (const QFileInfo info(stateFile); !info.exists())
				continue;
			if (QFile::remove(stateFile))
				break;
			failures << stateFile;
			break;
		}
	}

	if (!failures.isEmpty())
	{
		QMessageBox::warning(this, QStringLiteral("Plugins"),
		                     QStringLiteral("Unable to delete one or more state files:\n%1")
		                         .arg(failures.join(QLatin1Char('\n'))));
	}
}

void PluginsDialog::onEnablePlugin() const
{
	if (!m_runtime)
		return;

	const QList<int> rows = selectedRows();
	if (rows.isEmpty())
		return;

	for (const int row : rows)
	{
		if (const QString pluginId = pluginIdForRow(row); !pluginId.isEmpty())
			m_runtime->enablePlugin(pluginId, true);
	}

	reloadList();
}

void PluginsDialog::onDisablePlugin() const
{
	if (!m_runtime)
		return;

	const QList<int> rows = selectedRows();
	if (rows.isEmpty())
		return;

	for (const int row : rows)
	{
		if (const QString pluginId = pluginIdForRow(row); !pluginId.isEmpty())
			m_runtime->enablePlugin(pluginId, false);
	}

	reloadList();
}

void PluginsDialog::onReloadPlugin()
{
	if (!m_runtime)
		return;

	const QList<int> rows = selectedRows();
	if (rows.isEmpty())
		return;

	for (const int row : rows)
	{
		const QString pluginId = pluginIdForRow(row);
		if (pluginId.isEmpty())
			continue;
		QString error;
		if (const int result = m_runtime->reloadPlugin(pluginId, &error); result != eOK)
		{
			QMessageBox::warning(this, QStringLiteral("Plugins"),
			                     error.isEmpty() ? QStringLiteral("Unable to reload plugin.") : error);
		}
	}

	reloadList();
}

void PluginsDialog::onEditPlugin() const
{
	const QList<int> rows = selectedRows();
	if (rows.isEmpty())
		return;

	const QString        pluginId = pluginIdForRow(rows.front());
	WorldRuntime::Plugin plugin;
	if (!findPluginById(m_runtime, pluginId, plugin))
		return;
	if (plugin.source.isEmpty())
		return;

	if (AppController *app = AppController::instance())
		app->openDocumentFile(plugin.source);
}

void PluginsDialog::onShowDescription() const
{
	const QList<int> rows = selectedRows();
	if (rows.isEmpty())
		return;

	const QString        pluginId = pluginIdForRow(rows.front());
	WorldRuntime::Plugin plugin;
	if (!findPluginById(m_runtime, pluginId, plugin))
		return;

	QString title = plugin.attributes.value(QStringLiteral("name"));
	if (title.isEmpty())
		title = pluginId;

	QString text = plugin.description.trimmed();
	if (text.isEmpty())
		text = QStringLiteral("(No description)");

	if (m_main)
		m_main->sendToNotepad(title, text + QLatin1Char('\n'));
	else
		QMessageBox::information(m_table, title, text);
}

void PluginsDialog::onMoveUp() const
{
	movePlugin(-1);
}

void PluginsDialog::onMoveDown() const
{
	movePlugin(1);
}

void PluginsDialog::movePlugin(const int delta) const
{
	if (!m_runtime)
		return;

	const QList<int> rows = selectedRows();
	if (rows.size() != 1)
		return;

	const QString pluginId = pluginIdForRow(rows.front());
	if (pluginId.isEmpty())
		return;

	if (!m_runtime->reorderPlugin(pluginId, delta))
		return;
	m_table->setSortingEnabled(false);
	m_table->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
	reloadList();
	restoreSelection(pluginId);
}

void PluginsDialog::restoreSelection(const QString &pluginId) const
{
	if (!m_table)
		return;
	for (int row = 0; row < m_table->rowCount(); ++row)
	{
		if (pluginIdForRow(row) == pluginId)
		{
			m_table->selectRow(row);
			break;
		}
	}
}

void PluginsDialog::saveSettings() const
{
	if (!m_table)
		return;
	QSettings settings(AppController::instance()->iniFilePath(), QSettings::IniFormat);
	settings.beginGroup(QStringLiteral("PluginsDialog"));
	settings.setValue(QStringLiteral("Geometry"), saveGeometry());
	settings.setValue(QStringLiteral("HeaderVersion"), kHeaderVersion);
	settings.setValue(QStringLiteral("HeaderState"), m_table->horizontalHeader()->saveState());
	settings.setValue(QStringLiteral("UsingDefaultColumnWidths"), m_usingDefaultColumnWidths);
	if (m_usingDefaultColumnWidths)
		settings.remove(QStringLiteral("PreferredColumnWidths"));
	else
		settings.setValue(QStringLiteral("PreferredColumnWidths"),
		                  columnWidthsForSettings(m_preferredColumnWidths));
	settings.setValue(QStringLiteral("SortColumn"), m_table->horizontalHeader()->sortIndicatorSection());
	settings.setValue(QStringLiteral("SortOrder"), m_table->horizontalHeader()->sortIndicatorOrder());
	settings.endGroup();
}
