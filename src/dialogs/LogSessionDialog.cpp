/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: LogSessionDialog.cpp
 * Role: Session logging dialog behavior for validating log settings and applying logging state changes.
 */

#include "dialogs/LogSessionDialog.h"

#include <QCheckBox>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QSpinBox>
#include <QTextEdit>
#include <memory>

LogSessionDialog::LogSessionDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QStringLiteral("Log Session"));
	setMinimumSize(620, 480);

	auto mainLayout = std::make_unique<QVBoxLayout>();

	auto captureGroup  = std::make_unique<QGroupBox>(QStringLiteral("Capture"), this);
	auto captureLayout = std::make_unique<QFormLayout>();

	m_lines = new QSpinBox(captureGroup.get());
	m_lines->setRange(0, 500000);
	m_lines->setSingleStep(10);
	captureLayout->addRow(QStringLiteral("Lines to include from scrollback:"), m_lines);

	m_append = new QCheckBox(QStringLiteral("Append to existing log file"), captureGroup.get());
	captureLayout->addRow(m_append);

	m_writeWorldName = new QCheckBox(QStringLiteral("Write world name to log"), captureGroup.get());
	captureLayout->addRow(m_writeWorldName);
	captureGroup->setLayout(captureLayout.release());

	mainLayout->addWidget(captureGroup.release());

	auto optionsGroup  = std::make_unique<QGroupBox>(QStringLiteral("What to log"), this);
	auto optionsLayout = std::make_unique<QVBoxLayout>();
	m_logOutput = new QCheckBox(QStringLiteral("Log output from the MUD"), optionsGroup.get());
	m_logInput  = new QCheckBox(QStringLiteral("Log your input"), optionsGroup.get());
	m_logNotes  = new QCheckBox(QStringLiteral("Log notes"), optionsGroup.get());
	optionsLayout->addWidget(m_logOutput);
	optionsLayout->addWidget(m_logInput);
	optionsLayout->addWidget(m_logNotes);
	optionsGroup->setLayout(optionsLayout.release());
	mainLayout->addWidget(optionsGroup.release());

	auto preambleGroup  = std::make_unique<QGroupBox>(QStringLiteral("Log file preamble"), this);
	auto preambleLayout = std::make_unique<QVBoxLayout>();
	m_preamble = new QTextEdit(preambleGroup.get());
	m_preamble->setAcceptRichText(false);
	preambleLayout->addWidget(m_preamble);
	preambleGroup->setLayout(preambleLayout.release());
	mainLayout->addWidget(preambleGroup.release());

	auto buttons = std::make_unique<QDialogButtonBox>(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons.get(), &QDialogButtonBox::accepted, this, &LogSessionDialog::accept);
	connect(buttons.get(), &QDialogButtonBox::rejected, this, &LogSessionDialog::reject);
	mainLayout->addWidget(buttons.release());

	setLayout(mainLayout.release());
}

void LogSessionDialog::setLines(const int lines) const
{
	if (m_lines)
		m_lines->setValue(lines);
}

int LogSessionDialog::lines() const
{
	return m_lines ? m_lines->value() : 0;
}

void LogSessionDialog::setAppendToLogFile(const bool append) const
{
	if (m_append)
		m_append->setChecked(append);
}

bool LogSessionDialog::appendToLogFile() const
{
	return m_append && m_append->isChecked();
}

void LogSessionDialog::setWriteWorldName(const bool write) const
{
	if (m_writeWorldName)
		m_writeWorldName->setChecked(write);
}

bool LogSessionDialog::writeWorldName() const
{
	return m_writeWorldName && m_writeWorldName->isChecked();
}

void LogSessionDialog::setPreamble(const QString &text) const
{
	if (m_preamble)
		m_preamble->setPlainText(text);
}

QString LogSessionDialog::preamble() const
{
	return m_preamble ? m_preamble->toPlainText() : QString();
}

void LogSessionDialog::setLogOutput(const bool enabled) const
{
	if (m_logOutput)
		m_logOutput->setChecked(enabled);
}

bool LogSessionDialog::logOutput() const
{
	return m_logOutput && m_logOutput->isChecked();
}

void LogSessionDialog::setLogInput(const bool enabled) const
{
	if (m_logInput)
		m_logInput->setChecked(enabled);
}

bool LogSessionDialog::logInput() const
{
	return m_logInput && m_logInput->isChecked();
}

void LogSessionDialog::setLogNotes(const bool enabled) const
{
	if (m_logNotes)
		m_logNotes->setChecked(enabled);
}

bool LogSessionDialog::logNotes() const
{
	return m_logNotes && m_logNotes->isChecked();
}

void LogSessionDialog::accept()
{
	if (m_logOutput && !m_logOutput->isChecked())
	{
		const QMessageBox::StandardButton result = QMessageBox::question(
		    this, QStringLiteral("QMud"),
		    QStringLiteral("You are not logging output from the MUD - is this intentional?"),
		    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (result != QMessageBox::Yes)
			return;
	}
	QDialog::accept();
}
