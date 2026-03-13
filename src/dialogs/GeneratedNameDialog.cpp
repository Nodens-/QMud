/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: GeneratedNameDialog.cpp
 * Role: Generated-name dialog implementation wiring name-synthesis helpers to user selection and confirmation UI.
 */

#include "GeneratedNameDialog.h"

#include "AppController.h"
#include "NameGeneration.h"
#include "WorldRuntime.h"
#include "WorldView.h"

#include <QClipboard>
#include <QFileDialog>
#include <QGridLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>

#include <exception>
#include <memory>

GeneratedNameDialog::GeneratedNameDialog(WorldRuntime *runtime, QWidget *parent)
    : QDialog(parent), m_runtime(runtime)
{
	setWindowTitle(QStringLiteral("Generated Name"));
	setModal(true);
	setMinimumSize(620, 220);

	auto  layout     = std::make_unique<QGridLayout>();
	auto *gridLayout = layout.get();
	setLayout(layout.release());

	auto nameLabel = std::make_unique<QLabel>(QStringLiteral("Generated name:"), this);
	auto nameEdit  = std::make_unique<QLineEdit>(this);
	m_nameEdit     = nameEdit.release();
	m_nameEdit->setReadOnly(false);
	gridLayout->addWidget(nameLabel.release(), 0, 0);
	gridLayout->addWidget(m_nameEdit, 0, 1, 1, 3);

	auto fileLabel = std::make_unique<QLabel>(QStringLiteral("Names file:"), this);
	auto fileEdit  = std::make_unique<QLineEdit>(this);
	m_fileEdit     = fileEdit.release();
	m_fileEdit->setReadOnly(true);
	gridLayout->addWidget(fileLabel.release(), 1, 0);
	gridLayout->addWidget(m_fileEdit, 1, 1, 1, 3);

	auto  tryAgain          = std::make_unique<QPushButton>(QStringLiteral("Try Again"), this);
	auto  copy              = std::make_unique<QPushButton>(QStringLiteral("Copy"), this);
	auto  sendToWorld       = std::make_unique<QPushButton>(QStringLiteral("Send To World"), this);
	auto  browse            = std::make_unique<QPushButton>(QStringLiteral("Browse"), this);
	auto *tryAgainButton    = tryAgain.get();
	auto *copyButton        = copy.get();
	auto *sendToWorldButton = sendToWorld.get();
	auto *browseButton      = browse.get();

	gridLayout->addWidget(tryAgain.release(), 2, 0);
	gridLayout->addWidget(copy.release(), 2, 1);
	gridLayout->addWidget(sendToWorld.release(), 2, 2);
	gridLayout->addWidget(browse.release(), 2, 3);

	auto  buttons    = std::make_unique<QDialogButtonBox>(QDialogButtonBox::Close, this);
	auto *buttonsBox = buttons.get();
	gridLayout->addWidget(buttons.release(), 3, 0, 1, 4);

	connect(tryAgainButton, &QPushButton::clicked, this, &GeneratedNameDialog::onTryAgain);
	connect(copyButton, &QPushButton::clicked, this, &GeneratedNameDialog::onCopy);
	connect(sendToWorldButton, &QPushButton::clicked, this, &GeneratedNameDialog::onSendToWorld);
	connect(browseButton, &QPushButton::clicked, this, &GeneratedNameDialog::onBrowseName);
	connect(buttonsBox, &QDialogButtonBox::rejected, this, &GeneratedNameDialog::reject);

	updateFileName();
	onTryAgain();
}

void GeneratedNameDialog::setGeneratedName(const QString &name) const
{
	if (!m_nameEdit)
		return;
	m_nameEdit->setText(name);
	m_nameEdit->selectAll();
}

void GeneratedNameDialog::updateFileName() const
{
	if (!m_fileEdit)
		return;
	QString fileName;
	if (const AppController *app = AppController::instance())
		fileName = app->getGlobalOption(QStringLiteral("DefaultNameGenerationFile")).toString();
	m_fileEdit->setText(fileName);
}

void GeneratedNameDialog::onTryAgain() const
{
	setGeneratedName(qmudGenerateName());
}

void GeneratedNameDialog::onCopy() const
{
	if (!m_nameEdit)
		return;
	QGuiApplication::clipboard()->setText(m_nameEdit->text());
}

void GeneratedNameDialog::onSendToWorld() const
{
	if (!m_runtime || !m_nameEdit)
		return;
	const QString name = m_nameEdit->text();
	if (name.isEmpty())
		return;
	const bool echo =
	    m_runtime->worldAttributes().value(QStringLiteral("display_my_input")) == QStringLiteral("1");
	static_cast<void>(m_runtime->sendCommand(name, echo, false, false, true, false));
}

void GeneratedNameDialog::onBrowseName()
{
	QString startDir;
	if (const AppController *app = AppController::instance())
		startDir = app->getGlobalOption(QStringLiteral("DefaultNameGenerationFile")).toString();
	const QString fileName =
	    QFileDialog::getOpenFileName(this, QStringLiteral("Select names file"), startDir,
	                                 QStringLiteral("Name files (*.txt *.nam);;All files (*.*)"));
	if (fileName.isEmpty())
		return;
	try
	{
		qmudReadNames(fileName, true);
		if (AppController *app = AppController::instance())
			app->setGlobalOptionString(QStringLiteral("DefaultNameGenerationFile"), fileName);
		updateFileName();
		onTryAgain();
	}
	catch (const std::exception &e)
	{
		QMessageBox::warning(this, QStringLiteral("Name Generation"), QString::fromUtf8(e.what()));
	}
}
