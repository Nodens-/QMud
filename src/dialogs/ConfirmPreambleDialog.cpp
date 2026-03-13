/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: ConfirmPreambleDialog.cpp
 * Role: Confirmation dialog logic for preamble review so potentially impactful text/actions require explicit user
 * approval.
 */

#include "dialogs/ConfirmPreambleDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <memory>

ConfirmPreambleDialog::ConfirmPreambleDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QStringLiteral("Confirm preamble and postamble for pasting or sending file to the world"));

	auto  root       = std::make_unique<QVBoxLayout>();
	auto *rootLayout = root.get();
	setLayout(root.release());

	auto grid = std::make_unique<QGridLayout>();

	auto startLabel   = std::make_unique<QLabel>(QStringLiteral("&1. Send this at the start:"));
	auto filePreamble = std::make_unique<QPlainTextEdit>();
	m_filePreamble    = filePreamble.release();
	m_filePreamble->setMinimumHeight(60);
	startLabel->setBuddy(m_filePreamble);

	auto linePreLabel = std::make_unique<QLabel>(QStringLiteral("&2. Send this at the start of each line:"));
	auto linePreamble = std::make_unique<QLineEdit>();
	m_linePreamble    = linePreamble.release();
	linePreLabel->setBuddy(m_linePreamble);

	auto linePostLabel = std::make_unique<QLabel>(QStringLiteral("&3. Send this at the end of each line:"));
	auto linePostamble = std::make_unique<QLineEdit>();
	m_linePostamble    = linePostamble.release();
	linePostLabel->setBuddy(m_linePostamble);

	auto endLabel      = std::make_unique<QLabel>(QStringLiteral("&4. Send this at the end:"));
	auto filePostamble = std::make_unique<QPlainTextEdit>();
	m_filePostamble    = filePostamble.release();
	m_filePostamble->setMinimumHeight(55);
	endLabel->setBuddy(m_filePostamble);

	grid->addWidget(startLabel.release(), 0, 0);
	grid->addWidget(m_filePreamble, 1, 0);
	grid->addWidget(linePreLabel.release(), 2, 0);
	grid->addWidget(m_linePreamble, 3, 0);
	grid->addWidget(linePostLabel.release(), 4, 0);
	grid->addWidget(m_linePostamble, 5, 0);
	grid->addWidget(endLabel.release(), 6, 0);
	grid->addWidget(m_filePostamble, 7, 0);
	rootLayout->addLayout(grid.release());

	auto delayRow   = std::make_unique<QHBoxLayout>();
	auto delayLabel = std::make_unique<QLabel>(QStringLiteral("&Delay between lines:"));
	auto lineDelay  = std::make_unique<QSpinBox>();
	m_lineDelay     = lineDelay.release();
	m_lineDelay->setRange(0, 10000);
	delayLabel->setBuddy(m_lineDelay);
	auto delayNote = std::make_unique<QLabel>(QStringLiteral("milliseconds (1000 milliseconds = 1 second)"));
	delayRow->addWidget(delayLabel.release());
	delayRow->addWidget(m_lineDelay);
	delayRow->addWidget(delayNote.release(), 1);
	rootLayout->addLayout(delayRow.release());

	auto perRow        = std::make_unique<QHBoxLayout>();
	auto perLabel      = std::make_unique<QLabel>(QStringLiteral("E&very:"));
	auto delayPerLines = std::make_unique<QSpinBox>();
	m_delayPerLines    = delayPerLines.release();
	m_delayPerLines->setRange(1, 100000);
	perLabel->setBuddy(m_delayPerLines);
	auto perNote = std::make_unique<QLabel>(QStringLiteral("lines."));
	perRow->addWidget(perLabel.release());
	perRow->addWidget(m_delayPerLines);
	perRow->addWidget(perNote.release());
	perRow->addStretch(1);
	rootLayout->addLayout(perRow.release());

	auto options           = std::make_unique<QHBoxLayout>();
	auto commentedSoftcode = std::make_unique<QCheckBox>(QStringLiteral("&Commented softcode"));
	auto echo              = std::make_unique<QCheckBox>(QStringLiteral("&Echo each line to output window"));
	m_commentedSoftcode    = commentedSoftcode.release();
	m_echo                 = echo.release();
	options->addWidget(m_commentedSoftcode);
	options->addStretch(1);
	options->addWidget(m_echo);
	rootLayout->addLayout(options.release());

	auto message = std::make_unique<QLabel>();
	m_message    = message.release();
	m_message->setWordWrap(true);
	rootLayout->addWidget(m_message);

	auto buttons = std::make_unique<QDialogButtonBox>(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	connect(buttons.get(), &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons.get(), &QDialogButtonBox::rejected, this, &QDialog::reject);
	rootLayout->addWidget(buttons.release());
}

void ConfirmPreambleDialog::setPasteMessage(const QString &message) const
{
	if (m_message)
		m_message->setText(message);
}

void ConfirmPreambleDialog::setFilePreamble(const QString &text) const
{
	if (m_filePreamble)
		m_filePreamble->setPlainText(text);
}

void ConfirmPreambleDialog::setLinePreamble(const QString &text) const
{
	if (m_linePreamble)
		m_linePreamble->setText(text);
}

void ConfirmPreambleDialog::setLinePostamble(const QString &text) const
{
	if (m_linePostamble)
		m_linePostamble->setText(text);
}

void ConfirmPreambleDialog::setFilePostamble(const QString &text) const
{
	if (m_filePostamble)
		m_filePostamble->setPlainText(text);
}

void ConfirmPreambleDialog::setCommentedSoftcode(const bool enabled) const
{
	if (m_commentedSoftcode)
		m_commentedSoftcode->setChecked(enabled);
}

void ConfirmPreambleDialog::setLineDelayMs(const int delayMs) const
{
	if (m_lineDelay)
		m_lineDelay->setValue(delayMs);
}

void ConfirmPreambleDialog::setDelayPerLines(const int lines) const
{
	if (m_delayPerLines)
		m_delayPerLines->setValue(lines);
}

void ConfirmPreambleDialog::setEcho(const bool enabled) const
{
	if (m_echo)
		m_echo->setChecked(enabled);
}

QString ConfirmPreambleDialog::filePreamble() const
{
	return m_filePreamble ? m_filePreamble->toPlainText() : QString();
}

QString ConfirmPreambleDialog::linePreamble() const
{
	return m_linePreamble ? m_linePreamble->text() : QString();
}

QString ConfirmPreambleDialog::linePostamble() const
{
	return m_linePostamble ? m_linePostamble->text() : QString();
}

QString ConfirmPreambleDialog::filePostamble() const
{
	return m_filePostamble ? m_filePostamble->toPlainText() : QString();
}

bool ConfirmPreambleDialog::commentedSoftcode() const
{
	return m_commentedSoftcode && m_commentedSoftcode->isChecked();
}

int ConfirmPreambleDialog::lineDelayMs() const
{
	return m_lineDelay ? m_lineDelay->value() : 0;
}

int ConfirmPreambleDialog::delayPerLines() const
{
	return m_delayPerLines ? m_delayPerLines->value() : 1;
}

bool ConfirmPreambleDialog::echo() const
{
	return m_echo && m_echo->isChecked();
}
