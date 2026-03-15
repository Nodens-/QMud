/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: ImportXmlDialog.cpp
 * Role: XML import dialog implementation handling file selection, option toggles, and import execution flow.
 */

#include "dialogs/ImportXmlDialog.h"

#include "AppController.h"
#include "MainFrame.h"
#include "WorldChildWindow.h"
#include "WorldDocument.h"
#include "WorldRuntime.h"
#include "dialogs/PluginsDialog.h"

#include <QCheckBox>
#include <QClipboard>
#include <QFileDialog>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

ImportXmlDialog::ImportXmlDialog(AppController *app, QWidget *parent) : QDialog(parent), m_app(app)
{
	setWindowTitle(QStringLiteral("Import"));
	setMinimumSize(420, 380);

	auto  layout     = std::make_unique<QVBoxLayout>();
	auto *mainLayout = layout.get();
	setLayout(layout.release());
	auto label = std::make_unique<QLabel>(QStringLiteral("Import other XML data for:"), this);
	mainLayout->addWidget(label.release());

	m_general   = new QCheckBox(QStringLiteral("General"), this);
	m_triggers  = new QCheckBox(QStringLiteral("Triggers"), this);
	m_aliases   = new QCheckBox(QStringLiteral("Aliases"), this);
	m_timers    = new QCheckBox(QStringLiteral("Timers"), this);
	m_macros    = new QCheckBox(QStringLiteral("Macros"), this);
	m_variables = new QCheckBox(QStringLiteral("Variables"), this);
	m_colours   = new QCheckBox(QStringLiteral("Colours"), this);
	m_keypad    = new QCheckBox(QStringLiteral("Keypad"), this);
	m_printing  = new QCheckBox(QStringLiteral("Printing"), this);

	mainLayout->addWidget(m_general);
	mainLayout->addWidget(m_triggers);
	mainLayout->addWidget(m_aliases);
	mainLayout->addWidget(m_timers);
	mainLayout->addWidget(m_macros);
	mainLayout->addWidget(m_variables);
	mainLayout->addWidget(m_colours);
	mainLayout->addWidget(m_keypad);
	mainLayout->addWidget(m_printing);

	auto  buttonsRow       = std::make_unique<QHBoxLayout>();
	auto *buttonsRowLayout = buttonsRow.get();
	auto  importFile       = std::make_unique<QPushButton>(QStringLiteral("Import File..."), this);
	auto *importFileButton = importFile.get();
	buttonsRowLayout->addWidget(importFile.release());

	auto importClipboard = std::make_unique<QPushButton>(QStringLiteral("Import Clipboard"), this);
	m_importClipboard    = importClipboard.release();
	buttonsRowLayout->addWidget(m_importClipboard);

	auto  pluginsList       = std::make_unique<QPushButton>(QStringLiteral("Plugins..."), this);
	auto *pluginsListButton = pluginsList.get();
	buttonsRowLayout->addWidget(pluginsList.release());
	buttonsRowLayout->addStretch(1);

	auto  closeBox       = std::make_unique<QDialogButtonBox>(QDialogButtonBox::Close, this);
	auto *closeButtonBox = closeBox.get();
	buttonsRowLayout->addWidget(closeBox.release());
	mainLayout->addLayout(buttonsRow.release());

	connect(importFileButton, &QPushButton::clicked, this, &ImportXmlDialog::onImportFile);
	connect(m_importClipboard, &QPushButton::clicked, this, &ImportXmlDialog::onImportClipboard);
	connect(pluginsListButton, &QPushButton::clicked, this, &ImportXmlDialog::onPluginsList);
	connect(closeButtonBox, &QDialogButtonBox::rejected, this, &ImportXmlDialog::reject);

	updateClipboardState();
	if (const QClipboard *clipboard = QGuiApplication::clipboard())
		connect(clipboard, &QClipboard::dataChanged, this, &ImportXmlDialog::updateClipboardState);
}

unsigned long ImportXmlDialog::buildMask() const
{
	unsigned long mask = 0;
	if (m_general->isChecked())
		mask |= WorldDocument::XML_GENERAL;
	if (m_triggers->isChecked())
		mask |= WorldDocument::XML_TRIGGERS;
	if (m_aliases->isChecked())
		mask |= WorldDocument::XML_ALIASES;
	if (m_timers->isChecked())
		mask |= WorldDocument::XML_TIMERS;
	if (m_macros->isChecked())
		mask |= WorldDocument::XML_MACROS;
	if (m_variables->isChecked())
		mask |= WorldDocument::XML_VARIABLES;
	if (m_colours->isChecked())
		mask |= WorldDocument::XML_COLOURS;
	if (m_keypad->isChecked())
		mask |= WorldDocument::XML_KEYPAD;
	if (m_printing->isChecked())
		mask |= WorldDocument::XML_PRINTING;
	return mask;
}

void ImportXmlDialog::onImportFile()
{
	if (!m_app)
		return;

	m_app->changeToFileBrowsingDirectory();
	const QString fileName = QFileDialog::getOpenFileName(
	    this, QStringLiteral("Import"), QString(),
	    QStringLiteral(
	        "All files (*.*);;XML files (*.xml);;Text files (*.txt);;QMud worlds (*.qdl *.mcl);;QMud "
	        "triggers (*.qdt *.mct);;QMud aliases (*.qda *.mca);;QMud colours (*.qdc *.mcc);;QMud macros "
	        "(*.qdm *.mcm);;QMud timers (*.qdi *.mci);;QMud variables (*.qdv *.mcv)"));
	m_app->changeToStartupDirectory();
	if (fileName.isEmpty())
		return;

	if (!isXmlFile(fileName))
	{
		QMessageBox::warning(this, QStringLiteral("Import"), QStringLiteral("Not in XML format"));
		return;
	}

	const auto [ok, errorMessage, triggers, aliases, timers, macros, variables, colours, keypad, printing,
	            duplicates] = m_app->importXmlFromFile(fileName, buildMask());
	if (!ok)
	{
		QMessageBox::warning(this, QStringLiteral("Import"), errorMessage);
		return;
	}

	showImportCounts(QStringLiteral("Import"), triggers, aliases, timers, macros, variables, colours, keypad,
	                 printing, duplicates);
	accept();
}

void ImportXmlDialog::onImportClipboard()
{
	const QClipboard *clipboard = QGuiApplication::clipboard();
	if (!clipboard)
		return;

	const QString text = clipboard->text();
	if (text.isEmpty())
		return;

	importXmlText(text);
}

bool ImportXmlDialog::importXmlText(const QString &text)
{
	if (!m_app)
		return false;
	if (!isXmlText(text))
	{
		QMessageBox::warning(this, QStringLiteral("Import"), QStringLiteral("Not in XML format"));
		return false;
	}

	const auto [ok, errorMessage, triggers, aliases, timers, macros, variables, colours, keypad, printing,
	            duplicates] = m_app->importXmlFromText(text, buildMask());
	if (!ok)
	{
		QMessageBox::warning(this, QStringLiteral("Import"), errorMessage);
		return false;
	}

	showImportCounts(QStringLiteral("Import"), triggers, aliases, timers, macros, variables, colours, keypad,
	                 printing, duplicates);
	accept();
	return true;
}

bool ImportXmlDialog::isXmlText(const QString &text)
{
	if (text.isEmpty())
		return false;
	QString trimmed = text;
	int     pos     = 0;
	while (pos < trimmed.size() && trimmed.at(pos).isSpace())
		++pos;
	trimmed = trimmed.mid(pos);
	if (trimmed.size() < 15)
		return false;
	static const QStringList sigs = {
	    QStringLiteral("<?xml"),     QStringLiteral("<!--"),       QStringLiteral("<!DOCTYPE"),
	    QStringLiteral("<muclient"), QStringLiteral("<qmud"),      QStringLiteral("<world"),
	    QStringLiteral("<triggers"), QStringLiteral("<aliases"),   QStringLiteral("<timers"),
	    QStringLiteral("<macros"),   QStringLiteral("<variables"), QStringLiteral("<colours"),
	    QStringLiteral("<keypad"),   QStringLiteral("<printing"),  QStringLiteral("<comment"),
	    QStringLiteral("<include"),  QStringLiteral("<plugin"),    QStringLiteral("<script")};
	return std::ranges::any_of(sigs, [&trimmed](const QString &sig) { return trimmed.startsWith(sig); });
}

bool ImportXmlDialog::isXmlFile(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return false;
	QByteArray   buf(500, 0);
	QByteArray   buf2(500, 0);
	const qint64 readBytes = file.read(buf.data(), buf.size() - 2);
	if (readBytes <= 0)
		return false;
	if (static_cast<unsigned char>(buf[0]) == 0xFF && static_cast<unsigned char>(buf[1]) == 0xFE)
	{
		const auto   *wide      = reinterpret_cast<const char16_t *>(buf.constData() + 2);
		const int     wideLen   = static_cast<int>((readBytes - 2) / 2);
		const QString converted = QString::fromUtf16(wide, wideLen);
		buf2                    = converted.toUtf8();
	}
	else if (static_cast<unsigned char>(buf[0]) == 0xEF && static_cast<unsigned char>(buf[1]) == 0xBB &&
	         static_cast<unsigned char>(buf[2]) == 0xBF)
		buf2 = QByteArray(buf.constData() + 3);
	else
		buf2 = QByteArray(buf.constData());

	return isXmlText(QString::fromUtf8(buf2));
}

void ImportXmlDialog::updateClipboardState() const
{
	if (!m_importClipboard)
		return;
	const QClipboard *clipboard = QGuiApplication::clipboard();
	const QString     text      = clipboard ? clipboard->text() : QString();
	m_importClipboard->setEnabled(isXmlText(text));
}

void ImportXmlDialog::showImportCounts(const QString &title, const int triggers, const int aliases,
                                       const int timers, const int macros, const int variables,
                                       const int colours, const int keypad, const int printing,
                                       const int duplicates)
{
	QString message = QStringLiteral("%1 trigger%2, %3 alias%4, %5 timer%6, %7 macro%8, %9 variable%10, %11 "
	                                 "colour%12, %13 keypad%14, %15 printing style%16 loaded.")
	                      .arg(triggers)
	                      .arg(triggers == 1 ? QString() : QStringLiteral("s"))
	                      .arg(aliases)
	                      .arg(aliases == 1 ? QString() : QStringLiteral("es"))
	                      .arg(timers)
	                      .arg(timers == 1 ? QString() : QStringLiteral("s"))
	                      .arg(macros)
	                      .arg(macros == 1 ? QString() : QStringLiteral("s"))
	                      .arg(variables)
	                      .arg(variables == 1 ? QString() : QStringLiteral("s"))
	                      .arg(colours)
	                      .arg(colours == 1 ? QString() : QStringLiteral("s"))
	                      .arg(keypad)
	                      .arg(keypad == 1 ? QString() : QStringLiteral("s"))
	                      .arg(printing)
	                      .arg(printing == 1 ? QString() : QStringLiteral("s"));
	if (duplicates > 0)
		message += QStringLiteral("\n%1 duplicate item%2 overwritten.")
		               .arg(duplicates)
		               .arg(duplicates == 1 ? QString() : QStringLiteral("s"));
	QMessageBox::information(this, title, message);
}

void ImportXmlDialog::onPluginsList()
{
	if (!m_app || !m_app->mainWindow())
		return;
	auto *frame = m_app->mainWindow();
	if (!frame)
		return;
	const auto *world = frame->activeWorldChildWindow();
	if (!world)
		return;
	WorldRuntime *runtime = world->runtime();
	if (!runtime)
		return;
	PluginsDialog dlg(runtime, frame, this);
	dlg.exec();
}
