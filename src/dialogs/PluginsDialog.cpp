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
#include "scripting/ScriptingErrors.h"

#include <QAbstractItemView>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>
#include <memory>

namespace
{
	constexpr int kColumnName    = 0;
	constexpr int kColumnPurpose = 1;
	constexpr int kColumnAuthor  = 2;
	constexpr int kColumnFile    = 3;
	constexpr int kColumnEnabled = 4;
	constexpr int kColumnVersion = 5;
	constexpr int kColumnCount   = 6;
	constexpr int kHeaderVersion = 2;

	void          applyDefaultColumnWidths(QTableWidget *table)
	{
		if (!table)
			return;
		const int total = table->viewport()->width();
		if (total <= 0)
			return;
		const int     nameWidth    = qMin(200, qMax(120, static_cast<int>(total * 0.18)));
		const int     authorWidth  = qMax(120, static_cast<int>(total * 0.12));
		constexpr int enabledWidth = 70;
		constexpr int versionWidth = 60;
		int           remaining    = total - (nameWidth + authorWidth + enabledWidth + versionWidth);
		if (remaining < 200)
			remaining = 200;
		int fileWidth    = qMax(220, static_cast<int>(remaining * 0.55));
		int purposeWidth = remaining - fileWidth;
		if (purposeWidth < 180)
		{
			purposeWidth = 180;
			fileWidth    = qMax(200, remaining - purposeWidth);
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
		const QString                      key     = pluginId.trimmed().toLower();
		for (const WorldRuntime::Plugin &plugin : plugins)
		{
			if (const QString id = plugin.attributes.value(QStringLiteral("id")).trimmed().toLower();
			    id == key)
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
	setMinimumSize(1250, 420);

	auto  root    = std::make_unique<QVBoxLayout>();
	auto *rootPtr = root.get();
	setLayout(root.release());

	auto *table = new QTableWidget(this);
	m_table     = table;
	m_table->setColumnCount(kColumnCount);
	m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
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

	m_moveUpButton->setVisible(false);
	m_moveDownButton->setVisible(false);

	QPushButton *const buttons[] = {
	    m_addButton,    m_removeButton,  m_deleteStateButton, m_reloadButton, m_showDescriptionButton,
	    m_enableButton, m_disableButton, m_editButton,        closeButtonPtr};
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
		if (!restored)
			applyDefaultColumnWidths(m_table);
		clampColumnsToViewport(m_table);
		const int  sortColumn = settings.value(QStringLiteral("SortColumn"), kColumnName).toInt();
		const auto sortOrder  = static_cast<Qt::SortOrder>(
            settings.value(QStringLiteral("SortOrder"), Qt::AscendingOrder).toInt());
		if (sortColumn >= 0)
			m_table->sortByColumn(sortColumn, sortOrder);
		settings.endGroup();
	}
	onSelectionChanged();

	QTimer::singleShot(0, this,
	                   [this]
	                   {
		                   applyDefaultColumnWidths(m_table);
		                   clampColumnsToViewport(m_table);
	                   });
}

void PluginsDialog::reloadList() const
{
	m_table->setSortingEnabled(false);
	m_table->clearContents();
	m_table->setRowCount(0);

	if (!m_runtime)
		return;

	const QList<WorldRuntime::Plugin> &plugins     = m_runtime->plugins();
	const int                          pluginCount = plugins.size() > std::numeric_limits<int>::max()
	                                                     ? std::numeric_limits<int>::max()
	                                                     : static_cast<int>(plugins.size());
	m_table->setRowCount(pluginCount);

	for (int row = 0; row < pluginCount; ++row)
	{
		const WorldRuntime::Plugin &plugin   = plugins.at(row);
		const QString               pluginId = plugin.attributes.value(QStringLiteral("id"));
		const QString               name     = plugin.attributes.value(QStringLiteral("name"));
		const QString               purpose  = plugin.attributes.value(QStringLiteral("purpose"));
		const QString               author   = plugin.attributes.value(QStringLiteral("author"));
		const QString               file     = plugin.source;
		const QString               enabled  = plugin.enabled ? QStringLiteral("Yes") : QStringLiteral("No");
		const QString               version  = QString::number(plugin.version, 'f', 2);

		auto                        addItem = [&](const int column, const QString &text)
		{
			auto item = std::make_unique<QTableWidgetItem>(text);
			item->setData(Qt::UserRole, pluginId);
			item->setFlags(item->flags() & ~Qt::ItemIsEditable);
			m_table->setItem(row, column, item.release());
		};

		addItem(kColumnName, name.isEmpty() ? pluginId : name);
		addItem(kColumnAuthor, author);

		auto purposeItem = std::make_unique<QTableWidgetItem>(purpose);
		purposeItem->setData(Qt::UserRole, pluginId);
		purposeItem->setFlags(purposeItem->flags() & ~Qt::ItemIsEditable);
		purposeItem->setToolTip(purpose);
		m_table->setItem(row, kColumnPurpose, purposeItem.release());

		auto fileItem = std::make_unique<QTableWidgetItem>(file);
		fileItem->setData(Qt::UserRole, pluginId);
		fileItem->setFlags(fileItem->flags() & ~Qt::ItemIsEditable);
		fileItem->setToolTip(file);
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
		const QString pluginId = pluginIdForRow(row).trimmed();
		if (pluginId.isEmpty())
			continue;

		QStringList candidates;
		candidates << pluginId;
		if (const QString lower = pluginId.toLower(); !candidates.contains(lower))
			candidates << lower;
		if (const QString upper = pluginId.toUpper(); !candidates.contains(upper))
			candidates << upper;

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

	QList<WorldRuntime::Plugin> &plugins = m_runtime->pluginsMutable();
	const QString                key     = pluginId.trimmed().toLower();
	int                          index   = -1;
	for (int i = 0; i < plugins.size(); ++i)
	{
		if (const QString id = plugins.at(i).attributes.value(QStringLiteral("id")).trimmed().toLower();
		    id == key)
		{
			index = i;
			break;
		}
	}
	if (index < 0)
		return;
	const int other = index + delta;
	if (other < 0 || other >= plugins.size())
		return;

	const int seqA = plugins.at(index).sequence;
	int       seqB = plugins.at(other).sequence;
	if (seqA == seqB)
		seqB += delta < 0 ? 1 : -1;
	plugins[index].sequence = seqB;
	plugins[other].sequence = seqA;
	std::ranges::stable_sort(plugins,
	                         [](const WorldRuntime::Plugin &a, const WorldRuntime::Plugin &b)
	                         {
		                         if (a.sequence != b.sequence)
			                         return a.sequence < b.sequence;
		                         return a.attributes.value(QStringLiteral("id")) <
		                                b.attributes.value(QStringLiteral("id"));
	                         });
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
	settings.setValue(QStringLiteral("SortColumn"), m_table->horizontalHeader()->sortIndicatorSection());
	settings.setValue(QStringLiteral("SortOrder"), m_table->horizontalHeader()->sortIndicatorOrder());
	settings.endGroup();
}
