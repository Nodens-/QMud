/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: PluginWizardDialog.cpp
 * Role: Plugin wizard implementation guiding users through plugin template creation and initial configuration.
 */

#include "PluginWizardDialog.h"

#include "AppController.h"
#include "StringUtils.h"
#include "Version.h"
#include "WorldOptionDefaults.h"
#include "WorldRuntime.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QTextEdit>
#include <algorithm>
#include <limits>
#include <memory>

namespace
{
	int sizeToInt(const qsizetype value)
	{
		return static_cast<int>(qBound(static_cast<qsizetype>(0), value,
		                               static_cast<qsizetype>(std::numeric_limits<int>::max())));
	}

	QString timerDisplayText(const WorldRuntime::Timer &timer)
	{
		const QMap<QString, QString> &attrs  = timer.attributes;
		const bool                    atTime = qmudIsEnabledFlag(attrs.value(QStringLiteral("at_time")));

		auto formatParts = [](const int hour, const int minute, const double second) -> QString
		{
			return QStringLiteral("%1:%2:%3")
			    .arg(hour, 2, 10, QLatin1Char('0'))
			    .arg(minute, 2, 10, QLatin1Char('0'))
			    .arg(QString::asprintf("%05.2f", second));
		};

		const int     hour      = attrs.value(QStringLiteral("hour")).toInt();
		const int     minute    = attrs.value(QStringLiteral("minute")).toInt();
		const double  second    = attrs.value(QStringLiteral("second")).toDouble();
		const QString formatted = formatParts(hour, minute, second);
		return atTime ? QStringLiteral("At %1").arg(formatted) : QStringLiteral("Every %1").arg(formatted);
	}

	void setColumnTitles(QTableWidget *table, const QStringList &headers)
	{
		table->setColumnCount(sizeToInt(headers.size()));
		table->setHorizontalHeaderLabels(headers);
		table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
		table->horizontalHeader()->setStretchLastSection(true);
		table->setSelectionBehavior(QAbstractItemView::SelectRows);
		table->setSelectionMode(QAbstractItemView::ExtendedSelection);
		table->setSortingEnabled(false);
		if (const AppController *app = AppController::instance())
			table->setShowGrid(app->getGlobalOption(QStringLiteral("ShowGridLinesInListViews")).toInt() != 0);
		table->horizontalHeader()->setSectionsClickable(true);
		table->horizontalHeader()->setSortIndicatorShown(true);
	}

	void selectAllRows(QTableWidget *table)
	{
		if (!table)
			return;
		table->selectAll();
	}

	void selectNoRows(QTableWidget *table)
	{
		if (!table)
			return;
		table->clearSelection();
	}

	bool editTextDialog(QWidget *parent, const QString &title, QString &text)
	{
		QDialog dlg(parent);
		dlg.setWindowTitle(title);
		QVBoxLayout      layout(&dlg);
		QTextEdit        edit(&dlg);
		QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
		edit.setPlainText(text);
		layout.addWidget(&edit);
		layout.addWidget(&buttons);
		QObject::connect(&buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		QObject::connect(&buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
		if (dlg.exec() != QDialog::Accepted)
			return false;
		text = edit.toPlainText();
		return true;
	}

	int compareCaseInsensitive(const QString &a, const QString &b)
	{
		return QString::compare(a, b, Qt::CaseInsensitive);
	}

	double timerSeconds(const WorldRuntime::Timer &timer)
	{
		const QMap<QString, QString> &attrs  = timer.attributes;
		const int                     hour   = attrs.value(QStringLiteral("hour")).toInt();
		const int                     minute = attrs.value(QStringLiteral("minute")).toInt();
		const double                  second = attrs.value(QStringLiteral("second")).toDouble();
		return hour * 3600.0 + minute * 60.0 + second;
	}

	void setOwnedItem(QTableWidget *table, const int row, const int column,
	                  std::unique_ptr<QTableWidgetItem> item)
	{
		table->setItem(row, column, item.release());
	}
} // namespace

PluginWizardDialog::PluginWizardDialog(WorldRuntime *runtime, QWidget *parent)
    : QWizard(parent), m_runtime(runtime)
{
	setWindowTitle(QStringLiteral("Plugin Wizard"));
	setOption(NoBackButtonOnStartPage, true);
	setOption(HaveFinishButtonOnEarlyPages, false);
	setButtonText(FinishButton, QStringLiteral("&Create..."));
	setMinimumSize(560, 420);
	setMinimumWidth(560);

	buildPage1();
	buildPage2();
	buildPage3();
	buildPage4();
	buildPage5();
	buildPage6();
	buildPage7();
	buildPage8();
}

const PluginWizardState &PluginWizardDialog::state() const
{
	return m_state;
}

void PluginWizardDialog::accept()
{
	if (m_descriptionEdit)
	{
		if (const QString description = m_descriptionEdit->toPlainText();
		    description.contains(QStringLiteral("]]>")))
		{
			QMessageBox::warning(this, QStringLiteral("Plugin Wizard"),
			                     QStringLiteral("Description may not contain the sequence \"\"]]>\""));
			return;
		}
	}

	if (m_scriptEdit)
	{
		if (const QString script = m_scriptEdit->toPlainText(); script.contains(QStringLiteral("]]>")))
		{
			QMessageBox::warning(this, QStringLiteral("Plugin Wizard"),
			                     QStringLiteral("Script may not contain the sequence \"\"]]>\""));
			return;
		}
	}

	if (m_commentsEdit)
	{
		if (const QString comments = m_commentsEdit->toPlainText(); comments.contains(QStringLiteral("--")))
		{
			QMessageBox::warning(this, QStringLiteral("Plugin Wizard"),
			                     QStringLiteral("Comments may not contain the sequence \"--\""));
			return;
		}
	}

	m_state.name            = m_nameEdit ? m_nameEdit->text().trimmed() : QString();
	m_state.author          = m_authorEdit ? m_authorEdit->text().trimmed() : QString();
	m_state.purpose         = m_purposeEdit ? m_purposeEdit->text().trimmed() : QString();
	m_state.version         = m_versionEdit ? m_versionEdit->text().trimmed() : QString();
	m_state.id              = m_idEdit ? m_idEdit->text().trimmed() : QString();
	m_state.dateWritten     = m_dateEdit ? m_dateEdit->text().trimmed() : QString();
	m_state.requiresVersion = m_requiresSpin ? m_requiresSpin->value() : 0.0;
	m_state.removeItems     = m_removeItems ? m_removeItems->isChecked() : false;

	m_state.description  = m_descriptionEdit ? m_descriptionEdit->toPlainText() : QString();
	m_state.generateHelp = m_generateHelp ? m_generateHelp->isChecked() : false;
	m_state.helpAlias    = m_helpAliasEdit ? m_helpAliasEdit->text().trimmed() : QString();
	if (m_state.helpAlias.isEmpty() && !m_state.name.isEmpty())
		m_state.helpAlias = m_state.name + QStringLiteral(":help");

	m_state.selectedTriggers.clear();
	if (m_triggerTable)
	{
		for (const QModelIndex &idx : m_triggerTable->selectionModel()->selectedRows())
		{
			const int row = idx.row();
			if (const QTableWidgetItem *item = m_triggerTable->item(row, 0))
				m_state.selectedTriggers.insert(item->data(Qt::UserRole).toInt());
		}
	}

	m_state.selectedAliases.clear();
	if (m_aliasTable)
	{
		for (const QModelIndex &idx : m_aliasTable->selectionModel()->selectedRows())
		{
			const int row = idx.row();
			if (const QTableWidgetItem *item = m_aliasTable->item(row, 0))
				m_state.selectedAliases.insert(item->data(Qt::UserRole).toInt());
		}
	}

	m_state.selectedTimers.clear();
	if (m_timerTable)
	{
		for (const QModelIndex &idx : m_timerTable->selectionModel()->selectedRows())
		{
			const int row = idx.row();
			if (const QTableWidgetItem *item = m_timerTable->item(row, 0))
				m_state.selectedTimers.insert(item->data(Qt::UserRole).toInt());
		}
	}

	m_state.selectedVariables.clear();
	if (m_variableTable)
	{
		for (const QModelIndex &idx : m_variableTable->selectionModel()->selectedRows())
		{
			const int row = idx.row();
			if (const QTableWidgetItem *item = m_variableTable->item(row, 0))
				m_state.selectedVariables.insert(item->data(Qt::UserRole).toInt());
		}
	}

	m_state.saveState = m_saveState ? m_saveState->isChecked() : false;
	m_state.language  = m_languageCombo ? m_languageCombo->currentText().trimmed() : QStringLiteral("Lua");
	m_state.standardConstants = m_standardConstants ? m_standardConstants->isChecked() : false;
	m_state.script            = m_scriptEdit ? m_scriptEdit->toPlainText() : QString();
	m_state.comments          = m_commentsEdit ? m_commentsEdit->toPlainText() : QString();

	if (m_state.name.isEmpty() || m_state.id.isEmpty())
	{
		QMessageBox::warning(this, QStringLiteral("Plugin Wizard"),
		                     QStringLiteral("Plugin name and ID are required."));
		return;
	}
	if (const QRegularExpression labelRx(QStringLiteral("^[A-Za-z][A-Za-z0-9_]*$"));
	    !labelRx.match(m_state.name).hasMatch())
	{
		QMessageBox::warning(
		    this, QStringLiteral("Plugin Wizard"),
		    QStringLiteral("The plugin name must start with a letter and consist of letters, "
		                   "numbers or the underscore character."));
		return;
	}
	if (m_state.name.size() > 32)
	{
		QMessageBox::warning(this, QStringLiteral("Plugin Wizard"),
		                     QStringLiteral("Plugin name must be 32 characters or fewer."));
		return;
	}
	if (m_state.id.size() != 24)
	{
		QMessageBox::warning(this, QStringLiteral("Plugin Wizard"),
		                     QStringLiteral("Plugin ID must be 24 characters."));
		return;
	}

	QWizard::accept();
}

void PluginWizardDialog::buildPage1()
{
	auto  page    = std::make_unique<QWizardPage>(this);
	auto *pagePtr = page.get();
	pagePtr->setTitle(QStringLiteral("Plugin Details"));

	auto layout = std::make_unique<QVBoxLayout>();
	auto form   = std::make_unique<QFormLayout>();

	m_nameEdit = new QLineEdit(pagePtr);
	m_nameEdit->setMaxLength(32);
	m_authorEdit = new QLineEdit(pagePtr);
	m_authorEdit->setMaxLength(32);
	m_purposeEdit = new QLineEdit(pagePtr);
	m_purposeEdit->setMaxLength(100);
	m_versionEdit = new QLineEdit(QStringLiteral("1.0"), pagePtr);
	m_idEdit      = new QLineEdit(QMudWorldOptionDefaults::generateWorldUniqueId(), pagePtr);
	m_idEdit->setMaxLength(24);
	const QFontMetrics idMetrics(m_idEdit->font());
	const int          idWidth = idMetrics.horizontalAdvance(QStringLiteral("W").repeated(24)) + 20;
	m_idEdit->setMinimumWidth(idWidth);
	m_dateEdit =
	    new QLineEdit(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")), pagePtr);

	m_requiresSpin = new QDoubleSpinBox(pagePtr);
	m_requiresSpin->setDecimals(2);
	m_requiresSpin->setRange(0.0, 100000.0);
	m_requiresSpin->setValue(0.0);

	const QRegularExpression versionRx(QStringLiteral("^(\\d+(?:\\.\\d+)?)"));
	if (const QRegularExpressionMatch versionMatch = versionRx.match(QString::fromLatin1(kVersionString));
	    versionMatch.hasMatch())
		m_requiresSpin->setValue(versionMatch.captured(1).toDouble());

	form->addRow(QStringLiteral("Name:"), m_nameEdit);
	form->addRow(QStringLiteral("Author:"), m_authorEdit);
	form->addRow(QStringLiteral("Purpose:"), m_purposeEdit);
	form->addRow(QStringLiteral("Version:"), m_versionEdit);
	form->addRow(QStringLiteral("ID:"), m_idEdit);
	form->addRow(QStringLiteral("Date written:"), m_dateEdit);
	form->addRow(QStringLiteral("Requires:"), m_requiresSpin);
	layout->addLayout(form.release());

	m_removeItems = new QCheckBox(QStringLiteral("Remove exported items from the world"), pagePtr);
	m_removeItems->setChecked(true);
	layout->addWidget(m_removeItems);

	connect(m_nameEdit, &QLineEdit::textChanged, pagePtr,
	        [this](const QString &text)
	        {
		        if (!m_helpAliasEdit || !m_helpAliasEdit->text().trimmed().isEmpty())
			        return;
		        if (const QString trimmed = text.trimmed(); !trimmed.isEmpty())
			        m_helpAliasEdit->setText(trimmed + QStringLiteral(":help"));
	        });

	pagePtr->setLayout(layout.release());
	addPage(page.release());
}

void PluginWizardDialog::buildPage2()
{
	auto  page    = std::make_unique<QWizardPage>(this);
	auto *pagePtr = page.get();
	pagePtr->setTitle(QStringLiteral("Description"));

	auto layout     = std::make_unique<QVBoxLayout>();
	auto descHeader = std::make_unique<QHBoxLayout>(pagePtr);
	descHeader->addWidget(new QLabel(QStringLiteral("Description:"), pagePtr));
	descHeader->addStretch(1);
	auto  editButton    = std::make_unique<QPushButton>(QStringLiteral("Edit..."), pagePtr);
	auto *editButtonPtr = editButton.get();
	descHeader->addWidget(editButton.release());
	layout->addLayout(descHeader.release());

	m_descriptionEdit = new QTextEdit(pagePtr);
	layout->addWidget(m_descriptionEdit);

	auto helpRow   = std::make_unique<QHBoxLayout>(pagePtr);
	m_generateHelp = new QCheckBox(QStringLiteral("Generate help alias"), pagePtr);
	m_generateHelp->setChecked(true);
	m_helpAliasEdit = new QLineEdit(pagePtr);
	if (m_nameEdit && m_helpAliasEdit->text().trimmed().isEmpty())
	{
		if (const QString name = m_nameEdit->text().trimmed(); !name.isEmpty())
			m_helpAliasEdit->setText(name + QStringLiteral(":help"));
	}
	helpRow->addWidget(m_generateHelp);
	helpRow->addWidget(new QLabel(QStringLiteral("Help alias:"), pagePtr));
	helpRow->addWidget(m_helpAliasEdit);
	layout->addLayout(helpRow.release());

	connect(m_descriptionEdit, &QTextEdit::textChanged, pagePtr, [this] { updateHelpEnabled(); });
	connect(m_generateHelp, &QCheckBox::toggled, pagePtr, [this] { updateHelpEnabled(); });
	connect(editButtonPtr, &QPushButton::clicked, pagePtr,
	        [this]
	        {
		        if (!m_descriptionEdit)
			        return;
		        if (QString text = m_descriptionEdit->toPlainText();
		            editTextDialog(this, QStringLiteral("Edit plugin description"), text))
			        m_descriptionEdit->setPlainText(text);
	        });
	updateHelpEnabled();

	pagePtr->setLayout(layout.release());
	addPage(page.release());
}

void PluginWizardDialog::buildPage3()
{
	auto  page    = std::make_unique<QWizardPage>(this);
	auto *pagePtr = page.get();
	pagePtr->setTitle(QStringLiteral("Triggers"));

	auto layout    = std::make_unique<QVBoxLayout>();
	m_triggerTable = new QTableWidget(pagePtr);
	setColumnTitles(m_triggerTable, {"Name", "Match", "Send", "Group"});
	layout->addWidget(m_triggerTable);

	auto  buttons       = std::make_unique<QHBoxLayout>(pagePtr);
	auto  selectAll     = std::make_unique<QPushButton>(QStringLiteral("Select All"), pagePtr);
	auto  selectNone    = std::make_unique<QPushButton>(QStringLiteral("Select None"), pagePtr);
	auto *selectAllPtr  = selectAll.get();
	auto *selectNonePtr = selectNone.get();
	buttons->addWidget(selectAll.release());
	buttons->addWidget(selectNone.release());
	buttons->addStretch(1);
	layout->addLayout(buttons.release());

	connect(selectAllPtr, &QPushButton::clicked, pagePtr, [this] { selectAllRows(m_triggerTable); });
	connect(selectNonePtr, &QPushButton::clicked, pagePtr, [this] { selectNoRows(m_triggerTable); });

	populateTriggers();
	connect(m_triggerTable->horizontalHeader(), &QHeaderView::sectionClicked, pagePtr,
	        [this](const int col) { sortTriggers(col); });
	pagePtr->setLayout(layout.release());
	addPage(page.release());
}

void PluginWizardDialog::buildPage4()
{
	auto  page    = std::make_unique<QWizardPage>(this);
	auto *pagePtr = page.get();
	pagePtr->setTitle(QStringLiteral("Aliases"));

	auto layout  = std::make_unique<QVBoxLayout>();
	m_aliasTable = new QTableWidget(pagePtr);
	setColumnTitles(m_aliasTable, {"Name", "Match", "Send", "Group"});
	layout->addWidget(m_aliasTable);

	auto  buttons       = std::make_unique<QHBoxLayout>(pagePtr);
	auto  selectAll     = std::make_unique<QPushButton>(QStringLiteral("Select All"), pagePtr);
	auto  selectNone    = std::make_unique<QPushButton>(QStringLiteral("Select None"), pagePtr);
	auto *selectAllPtr  = selectAll.get();
	auto *selectNonePtr = selectNone.get();
	buttons->addWidget(selectAll.release());
	buttons->addWidget(selectNone.release());
	buttons->addStretch(1);
	layout->addLayout(buttons.release());

	connect(selectAllPtr, &QPushButton::clicked, pagePtr, [this] { selectAllRows(m_aliasTable); });
	connect(selectNonePtr, &QPushButton::clicked, pagePtr, [this] { selectNoRows(m_aliasTable); });

	populateAliases();
	connect(m_aliasTable->horizontalHeader(), &QHeaderView::sectionClicked, pagePtr,
	        [this](const int col) { sortAliases(col); });
	pagePtr->setLayout(layout.release());
	addPage(page.release());
}

void PluginWizardDialog::buildPage5()
{
	auto  page    = std::make_unique<QWizardPage>(this);
	auto *pagePtr = page.get();
	pagePtr->setTitle(QStringLiteral("Timers"));

	auto layout  = std::make_unique<QVBoxLayout>();
	m_timerTable = new QTableWidget(pagePtr);
	setColumnTitles(m_timerTable, {"Name", "Time", "Send", "Group"});
	layout->addWidget(m_timerTable);

	auto  buttons       = std::make_unique<QHBoxLayout>(pagePtr);
	auto  selectAll     = std::make_unique<QPushButton>(QStringLiteral("Select All"), pagePtr);
	auto  selectNone    = std::make_unique<QPushButton>(QStringLiteral("Select None"), pagePtr);
	auto *selectAllPtr  = selectAll.get();
	auto *selectNonePtr = selectNone.get();
	buttons->addWidget(selectAll.release());
	buttons->addWidget(selectNone.release());
	buttons->addStretch(1);
	layout->addLayout(buttons.release());

	connect(selectAllPtr, &QPushButton::clicked, pagePtr, [this] { selectAllRows(m_timerTable); });
	connect(selectNonePtr, &QPushButton::clicked, pagePtr, [this] { selectNoRows(m_timerTable); });

	populateTimers();
	connect(m_timerTable->horizontalHeader(), &QHeaderView::sectionClicked, pagePtr,
	        [this](const int col) { sortTimers(col); });
	pagePtr->setLayout(layout.release());
	addPage(page.release());
}

void PluginWizardDialog::buildPage6()
{
	auto  page    = std::make_unique<QWizardPage>(this);
	auto *pagePtr = page.get();
	pagePtr->setTitle(QStringLiteral("Variables"));

	auto layout     = std::make_unique<QVBoxLayout>();
	m_variableTable = new QTableWidget(pagePtr);
	setColumnTitles(m_variableTable, {"Name", "Contents"});
	layout->addWidget(m_variableTable);

	auto  buttons       = std::make_unique<QHBoxLayout>(pagePtr);
	auto  selectAll     = std::make_unique<QPushButton>(QStringLiteral("Select All"), pagePtr);
	auto  selectNone    = std::make_unique<QPushButton>(QStringLiteral("Select None"), pagePtr);
	auto *selectAllPtr  = selectAll.get();
	auto *selectNonePtr = selectNone.get();
	buttons->addWidget(selectAll.release());
	buttons->addWidget(selectNone.release());
	buttons->addStretch(1);
	layout->addLayout(buttons.release());

	m_saveState = new QCheckBox(QStringLiteral("Save plugin state"), pagePtr);
	m_saveState->setChecked(m_runtime && !m_runtime->variables().isEmpty());
	layout->addWidget(m_saveState);

	connect(selectAllPtr, &QPushButton::clicked, pagePtr, [this] { selectAllRows(m_variableTable); });
	connect(selectNonePtr, &QPushButton::clicked, pagePtr, [this] { selectNoRows(m_variableTable); });

	populateVariables();
	connect(m_variableTable->horizontalHeader(), &QHeaderView::sectionClicked, pagePtr,
	        [this](const int col) { sortVariables(col); });
	pagePtr->setLayout(layout.release());
	addPage(page.release());
}

void PluginWizardDialog::buildPage7()
{
	auto  page    = std::make_unique<QWizardPage>(this);
	auto *pagePtr = page.get();
	pagePtr->setTitle(QStringLiteral("Script"));

	auto layout     = std::make_unique<QVBoxLayout>();
	auto langRow    = std::make_unique<QHBoxLayout>(pagePtr);
	m_languageCombo = new QComboBox(pagePtr);
	m_languageCombo->addItem(QStringLiteral("Lua"));
	m_languageCombo->setCurrentIndex(0);
	langRow->addWidget(new QLabel(QStringLiteral("Language:"), pagePtr));
	langRow->addWidget(m_languageCombo);
	langRow->addStretch(1);
	layout->addLayout(langRow.release());

	m_standardConstants = new QCheckBox(QStringLiteral("Include standard constants"), pagePtr);
	m_standardConstants->setChecked(false);
	layout->addWidget(m_standardConstants);

	auto scriptHeader = std::make_unique<QHBoxLayout>(pagePtr);
	scriptHeader->addWidget(new QLabel(QStringLiteral("Script:"), pagePtr));
	scriptHeader->addStretch(1);
	auto  editButton    = std::make_unique<QPushButton>(QStringLiteral("Edit..."), pagePtr);
	auto *editButtonPtr = editButton.get();
	scriptHeader->addWidget(editButton.release());
	layout->addLayout(scriptHeader.release());

	m_scriptEdit = new QTextEdit(pagePtr);
	layout->addWidget(m_scriptEdit);

	if (m_runtime)
	{
		if (const QString language = m_runtime->worldAttributes().value(QStringLiteral("script_language"));
		    !language.isEmpty() && language.compare(QStringLiteral("Lua"), Qt::CaseInsensitive) == 0)
			m_languageCombo->setCurrentText(QStringLiteral("Lua"));
		QString                       scriptText;
		const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
		if (const QString scriptFile = attrs.value(QStringLiteral("script_filename")).trimmed();
		    !scriptFile.isEmpty())
		{
			const QString path = AppController::instance()
			                         ? AppController::instance()->makeAbsolutePath(scriptFile)
			                         : scriptFile;
			if (QFile script(path); script.open(QIODevice::ReadOnly))
				scriptText = QString::fromUtf8(script.readAll());
		}
		m_scriptEdit->setPlainText(scriptText);
	}

	connect(editButtonPtr, &QPushButton::clicked, pagePtr,
	        [this]
	        {
		        if (!m_scriptEdit)
			        return;
		        if (QString text = m_scriptEdit->toPlainText();
		            editTextDialog(this, QStringLiteral("Edit plugin script"), text))
			        m_scriptEdit->setPlainText(text);
	        });

	pagePtr->setLayout(layout.release());
	addPage(page.release());
}

void PluginWizardDialog::buildPage8()
{
	auto  page    = std::make_unique<QWizardPage>(this);
	auto *pagePtr = page.get();
	pagePtr->setTitle(QStringLiteral("Comments"));

	auto layout = std::make_unique<QVBoxLayout>();
	auto header = std::make_unique<QHBoxLayout>(pagePtr);
	header->addWidget(new QLabel(QStringLiteral("Comments:"), pagePtr));
	header->addStretch(1);
	auto  editButton    = std::make_unique<QPushButton>(QStringLiteral("Edit..."), pagePtr);
	auto *editButtonPtr = editButton.get();
	header->addWidget(editButton.release());
	layout->addLayout(header.release());
	m_commentsEdit = new QTextEdit(pagePtr);
	layout->addWidget(m_commentsEdit);
	connect(editButtonPtr, &QPushButton::clicked, pagePtr,
	        [this]
	        {
		        if (!m_commentsEdit)
			        return;
		        if (QString text = m_commentsEdit->toPlainText();
		            editTextDialog(this, QStringLiteral("Edit plugin comments"), text))
			        m_commentsEdit->setPlainText(text);
	        });
	pagePtr->setLayout(layout.release());
	addPage(page.release());
}

void PluginWizardDialog::populateTriggers() const
{
	if (!m_runtime || !m_triggerTable)
		return;
	const QList<WorldRuntime::Trigger> &triggers = m_runtime->triggers();
	QVector<int>                        order;
	order.reserve(triggers.size());
	for (int i = 0; i < triggers.size(); ++i)
	{
		if (const WorldRuntime::Trigger &tr = triggers.at(i);
		    qmudIsEnabledFlag(tr.attributes.value(QStringLiteral("temporary"))))
			continue;
		order.push_back(i);
	}
	fillTriggerTable(order, {});
	selectAllRows(m_triggerTable);
}

void PluginWizardDialog::populateAliases() const
{
	if (!m_runtime || !m_aliasTable)
		return;
	const QList<WorldRuntime::Alias> &aliases = m_runtime->aliases();
	QVector<int>                      order;
	order.reserve(aliases.size());
	for (int i = 0; i < aliases.size(); ++i)
	{
		if (const WorldRuntime::Alias &al = aliases.at(i);
		    qmudIsEnabledFlag(al.attributes.value(QStringLiteral("temporary"))))
			continue;
		order.push_back(i);
	}
	fillAliasTable(order, {});
	selectAllRows(m_aliasTable);
}

void PluginWizardDialog::populateTimers() const
{
	if (!m_runtime || !m_timerTable)
		return;
	const QList<WorldRuntime::Timer> &timers = m_runtime->timers();
	QVector<int>                      order;
	order.reserve(timers.size());
	for (int i = 0; i < timers.size(); ++i)
	{
		if (const WorldRuntime::Timer &tm = timers.at(i);
		    qmudIsEnabledFlag(tm.attributes.value(QStringLiteral("temporary"))))
			continue;
		order.push_back(i);
	}
	fillTimerTable(order, {});
	selectAllRows(m_timerTable);
}

void PluginWizardDialog::populateVariables() const
{
	if (!m_runtime || !m_variableTable)
		return;
	const QList<WorldRuntime::Variable> &vars = m_runtime->variables();
	QVector<int>                         order;
	order.reserve(vars.size());
	for (int i = 0; i < vars.size(); ++i)
		order.push_back(i);
	fillVariableTable(order, {});
	selectAllRows(m_variableTable);
}

QSet<int> PluginWizardDialog::selectedIndices(const QTableWidget *table)
{
	QSet<int> selected;
	if (!table || !table->selectionModel())
		return selected;
	for (const QModelIndex &idx : table->selectionModel()->selectedRows())
	{
		if (const QTableWidgetItem *item = table->item(idx.row(), 0))
			selected.insert(item->data(Qt::UserRole).toInt());
	}
	return selected;
}

void PluginWizardDialog::fillTriggerTable(const QVector<int> &order, const QSet<int> &selected) const
{
	if (!m_runtime || !m_triggerTable)
		return;
	const QList<WorldRuntime::Trigger> &triggers = m_runtime->triggers();
	m_triggerTable->setRowCount(0);
	int row = 0;
	for (int idx : order)
	{
		if (idx < 0 || idx >= triggers.size())
			continue;
		const WorldRuntime::Trigger &tr = triggers.at(idx);
		if (qmudIsEnabledFlag(tr.attributes.value(QStringLiteral("temporary"))))
			continue;
		m_triggerTable->insertRow(row);
		auto name = std::make_unique<QTableWidgetItem>(tr.attributes.value(QStringLiteral("name")));
		name->setData(Qt::UserRole, idx);
		setOwnedItem(m_triggerTable, row, 0, std::move(name));
		setOwnedItem(m_triggerTable, row, 1,
		             std::make_unique<QTableWidgetItem>(tr.attributes.value(QStringLiteral("match"))));
		setOwnedItem(m_triggerTable, row, 2,
		             std::make_unique<QTableWidgetItem>(tr.children.value(QStringLiteral("send"))));
		setOwnedItem(m_triggerTable, row, 3,
		             std::make_unique<QTableWidgetItem>(tr.attributes.value(QStringLiteral("group"))));
		if (selected.contains(idx))
			m_triggerTable->selectRow(row);
		++row;
	}
}

void PluginWizardDialog::fillAliasTable(const QVector<int> &order, const QSet<int> &selected) const
{
	if (!m_runtime || !m_aliasTable)
		return;
	const QList<WorldRuntime::Alias> &aliases = m_runtime->aliases();
	m_aliasTable->setRowCount(0);
	int row = 0;
	for (int idx : order)
	{
		if (idx < 0 || idx >= aliases.size())
			continue;
		const WorldRuntime::Alias &al = aliases.at(idx);
		if (qmudIsEnabledFlag(al.attributes.value(QStringLiteral("temporary"))))
			continue;
		m_aliasTable->insertRow(row);
		auto name = std::make_unique<QTableWidgetItem>(al.attributes.value(QStringLiteral("name")));
		name->setData(Qt::UserRole, idx);
		setOwnedItem(m_aliasTable, row, 0, std::move(name));
		setOwnedItem(m_aliasTable, row, 1,
		             std::make_unique<QTableWidgetItem>(al.attributes.value(QStringLiteral("match"))));
		setOwnedItem(m_aliasTable, row, 2,
		             std::make_unique<QTableWidgetItem>(al.children.value(QStringLiteral("send"))));
		setOwnedItem(m_aliasTable, row, 3,
		             std::make_unique<QTableWidgetItem>(al.attributes.value(QStringLiteral("group"))));
		if (selected.contains(idx))
			m_aliasTable->selectRow(row);
		++row;
	}
}

void PluginWizardDialog::fillTimerTable(const QVector<int> &order, const QSet<int> &selected) const
{
	if (!m_runtime || !m_timerTable)
		return;
	const QList<WorldRuntime::Timer> &timers = m_runtime->timers();
	m_timerTable->setRowCount(0);
	int row = 0;
	for (int idx : order)
	{
		if (idx < 0 || idx >= timers.size())
			continue;
		const WorldRuntime::Timer &tm = timers.at(idx);
		if (qmudIsEnabledFlag(tm.attributes.value(QStringLiteral("temporary"))))
			continue;
		m_timerTable->insertRow(row);
		auto name = std::make_unique<QTableWidgetItem>(tm.attributes.value(QStringLiteral("name")));
		name->setData(Qt::UserRole, idx);
		setOwnedItem(m_timerTable, row, 0, std::move(name));
		setOwnedItem(m_timerTable, row, 1, std::make_unique<QTableWidgetItem>(timerDisplayText(tm)));
		setOwnedItem(m_timerTable, row, 2,
		             std::make_unique<QTableWidgetItem>(tm.children.value(QStringLiteral("send"))));
		setOwnedItem(m_timerTable, row, 3,
		             std::make_unique<QTableWidgetItem>(tm.attributes.value(QStringLiteral("group"))));
		if (selected.contains(idx))
			m_timerTable->selectRow(row);
		++row;
	}
}

void PluginWizardDialog::fillVariableTable(const QVector<int> &order, const QSet<int> &selected) const
{
	if (!m_runtime || !m_variableTable)
		return;
	const QList<WorldRuntime::Variable> &vars = m_runtime->variables();
	m_variableTable->setRowCount(0);
	int row = 0;
	for (int idx : order)
	{
		if (idx < 0 || idx >= vars.size())
			continue;
		const auto &[attributes, content] = vars.at(idx);
		m_variableTable->insertRow(row);
		auto name = std::make_unique<QTableWidgetItem>(attributes.value(QStringLiteral("name")));
		name->setData(Qt::UserRole, idx);
		setOwnedItem(m_variableTable, row, 0, std::move(name));
		setOwnedItem(m_variableTable, row, 1, std::make_unique<QTableWidgetItem>(content));
		if (selected.contains(idx))
			m_variableTable->selectRow(row);
		++row;
	}
}

void PluginWizardDialog::sortTriggers(const int column)
{
	if (!m_runtime || !m_triggerTable)
		return;
	if (column == m_triggerSortColumn)
		m_triggerSortReverse = !m_triggerSortReverse;
	else
		m_triggerSortReverse = false;
	m_triggerSortColumn = column;

	const QSet<int>                     selected = selectedIndices(m_triggerTable);
	const QList<WorldRuntime::Trigger> &triggers = m_runtime->triggers();
	QVector<int>                        order;
	for (int i = 0; i < triggers.size(); ++i)
	{
		if (qmudIsEnabledFlag(triggers.at(i).attributes.value(QStringLiteral("temporary"))))
			continue;
		order.push_back(i);
	}
	auto cmp = [&](const int a, const int b)
	{
		const WorldRuntime::Trigger &t1    = triggers.at(a);
		const WorldRuntime::Trigger &t2    = triggers.at(b);
		auto                         label = [](const WorldRuntime::Trigger &t)
		{ return t.attributes.value(QStringLiteral("name")); };
		auto match = [](const WorldRuntime::Trigger &t)
		{ return t.attributes.value(QStringLiteral("match")); };
		auto send  = [](const WorldRuntime::Trigger &t) { return t.children.value(QStringLiteral("send")); };
		auto group = [](const WorldRuntime::Trigger &t)
		{ return t.attributes.value(QStringLiteral("group")); };

		int result = 0;
		switch (column)
		{
		case 3:
			result = compareCaseInsensitive(group(t1), group(t2));
			if (result)
				break;
			[[fallthrough]];
		case 0:
			result = compareCaseInsensitive(label(t1), label(t2));
			if (result)
				break;
			[[fallthrough]];
		case 1:
			result = compareCaseInsensitive(match(t1), match(t2));
			if (result)
				break;
			[[fallthrough]];
		case 2:
			result = compareCaseInsensitive(send(t1), send(t2));
			break;
		default:
			result = 0;
		}
		if (m_triggerSortReverse)
			result *= -1;
		return result < 0;
	};
	std::ranges::stable_sort(order, cmp);
	fillTriggerTable(order, selected);
	m_triggerTable->horizontalHeader()->setSortIndicator(column, m_triggerSortReverse ? Qt::DescendingOrder
	                                                                                  : Qt::AscendingOrder);
}

void PluginWizardDialog::sortAliases(const int column)
{
	if (!m_runtime || !m_aliasTable)
		return;
	if (column == m_aliasSortColumn)
		m_aliasSortReverse = !m_aliasSortReverse;
	else
		m_aliasSortReverse = false;
	m_aliasSortColumn = column;

	const QSet<int>                   selected = selectedIndices(m_aliasTable);
	const QList<WorldRuntime::Alias> &aliases  = m_runtime->aliases();
	QVector<int>                      order;
	for (int i = 0; i < aliases.size(); ++i)
	{
		if (qmudIsEnabledFlag(aliases.at(i).attributes.value(QStringLiteral("temporary"))))
			continue;
		order.push_back(i);
	}
	auto cmp = [&](const int a, const int b)
	{
		const WorldRuntime::Alias &t1 = aliases.at(a);
		const WorldRuntime::Alias &t2 = aliases.at(b);
		auto label = [](const WorldRuntime::Alias &t) { return t.attributes.value(QStringLiteral("name")); };
		auto match = [](const WorldRuntime::Alias &t) { return t.attributes.value(QStringLiteral("match")); };
		auto send  = [](const WorldRuntime::Alias &t) { return t.children.value(QStringLiteral("send")); };
		auto group = [](const WorldRuntime::Alias &t) { return t.attributes.value(QStringLiteral("group")); };

		int  result = 0;
		switch (column)
		{
		case 3:
			result = compareCaseInsensitive(group(t1), group(t2));
			if (result)
				break;
			[[fallthrough]];
		case 0:
			result = compareCaseInsensitive(label(t1), label(t2));
			if (result)
				break;
			[[fallthrough]];
		case 1:
			result = compareCaseInsensitive(match(t1), match(t2));
			if (result)
				break;
			[[fallthrough]];
		case 2:
			result = compareCaseInsensitive(send(t1), send(t2));
			break;
		default:
			result = 0;
		}
		if (m_aliasSortReverse)
			result *= -1;
		return result < 0;
	};
	std::ranges::stable_sort(order, cmp);
	fillAliasTable(order, selected);
	m_aliasTable->horizontalHeader()->setSortIndicator(column, m_aliasSortReverse ? Qt::DescendingOrder
	                                                                              : Qt::AscendingOrder);
}

void PluginWizardDialog::sortTimers(const int column)
{
	if (!m_runtime || !m_timerTable)
		return;
	if (column == m_timerSortColumn)
		m_timerSortReverse = !m_timerSortReverse;
	else
		m_timerSortReverse = false;
	m_timerSortColumn = column;

	const QSet<int>                   selected = selectedIndices(m_timerTable);
	const QList<WorldRuntime::Timer> &timers   = m_runtime->timers();
	QVector<int>                      order;
	for (int i = 0; i < timers.size(); ++i)
	{
		if (qmudIsEnabledFlag(timers.at(i).attributes.value(QStringLiteral("temporary"))))
			continue;
		order.push_back(i);
	}
	auto cmp = [&](const int a, const int b)
	{
		const WorldRuntime::Timer &t1 = timers.at(a);
		const WorldRuntime::Timer &t2 = timers.at(b);
		auto label = [](const WorldRuntime::Timer &t) { return t.attributes.value(QStringLiteral("name")); };
		auto send  = [](const WorldRuntime::Timer &t) { return t.children.value(QStringLiteral("send")); };
		auto group = [](const WorldRuntime::Timer &t) { return t.attributes.value(QStringLiteral("group")); };
		const double time1 = timerSeconds(t1);
		const double time2 = timerSeconds(t2);

		int          result = 0;
		switch (column)
		{
		case 3:
			result = compareCaseInsensitive(group(t1), group(t2));
			if (result)
				break;
			[[fallthrough]];
		case 0:
			result = compareCaseInsensitive(label(t1), label(t2));
			if (result)
				break;
			[[fallthrough]];
		case 1:
			if (time1 < time2)
				result = -1;
			else if (time1 > time2)
				result = 1;
			if (result)
				break;
			[[fallthrough]];
		case 2:
			result = compareCaseInsensitive(send(t1), send(t2));
			break;
		default:
			result = 0;
		}
		if (m_timerSortReverse)
			result *= -1;
		return result < 0;
	};
	std::ranges::stable_sort(order, cmp);
	fillTimerTable(order, selected);
	m_timerTable->horizontalHeader()->setSortIndicator(column, m_timerSortReverse ? Qt::DescendingOrder
	                                                                              : Qt::AscendingOrder);
}

void PluginWizardDialog::sortVariables(const int column)
{
	if (!m_runtime || !m_variableTable)
		return;
	if (column == m_variableSortColumn)
		m_variableSortReverse = !m_variableSortReverse;
	else
		m_variableSortReverse = false;
	m_variableSortColumn = column;

	const QSet<int>                      selected = selectedIndices(m_variableTable);
	const QList<WorldRuntime::Variable> &vars     = m_runtime->variables();
	QVector<int>                         order;
	for (int i = 0; i < vars.size(); ++i)
		order.push_back(i);
	auto cmp = [&](const int a, const int b)
	{
		const auto &[attributes1, content1] = vars.at(a);
		const auto &[attributes2, content2] = vars.at(b);
		const QString name1                 = attributes1.value(QStringLiteral("name"));
		const QString name2                 = attributes2.value(QStringLiteral("name"));
		const QString c1                    = content1;
		const QString c2                    = content2;

		int           result = 0;
		switch (column)
		{
		case 0:
			result = compareCaseInsensitive(name1, name2);
			if (result)
				break;
			[[fallthrough]];
		case 1:
			result = compareCaseInsensitive(c1, c2);
			break;
		default:
			result = 0;
		}
		if (m_variableSortReverse)
			result *= -1;
		return result < 0;
	};
	std::ranges::stable_sort(order, cmp);
	fillVariableTable(order, selected);
	m_variableTable->horizontalHeader()->setSortIndicator(column, m_variableSortReverse ? Qt::DescendingOrder
	                                                                                    : Qt::AscendingOrder);
}

void PluginWizardDialog::updateHelpEnabled() const
{
	const bool hasDescription = m_descriptionEdit && !m_descriptionEdit->toPlainText().trimmed().isEmpty();
	if (m_generateHelp)
		m_generateHelp->setEnabled(hasDescription);
	const bool enable =
	    m_generateHelp && m_generateHelp->isEnabled() && m_generateHelp->isChecked() && hasDescription;
	if (m_helpAliasEdit)
		m_helpAliasEdit->setEnabled(enable);
}
