/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: FindDialog.cpp
 * Role: Find dialog behavior that executes text searches and coordinates match navigation with active views.
 */

#include "FindDialog.h"
#include "dialogs/WorldPreferencesDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSizePolicy>

FindDialog::FindDialog(QStringList &findHistory, QWidget *parent)
    : QDialog(parent), m_findHistory(findHistory)
{
	buildUi();
}

void FindDialog::setTitleText(const QString &title)
{
	m_title = title;
	setWindowTitle(m_title.isEmpty() ? QStringLiteral("Find") : m_title);
}

QString FindDialog::titleText() const
{
	return m_title;
}

void FindDialog::setFindText(const QString &text)
{
	m_findText = text;
	m_findCombo->setCurrentText(text);
	if (m_findCombo->lineEdit())
		m_findCombo->lineEdit()->selectAll();
}

QString FindDialog::findText() const
{
	return m_findText;
}

void FindDialog::setMatchCase(const bool enabled)
{
	m_matchCase = enabled;
	m_matchCaseControl->setChecked(enabled);
}

bool FindDialog::matchCase() const
{
	return m_matchCase;
}

void FindDialog::setRegexp(const bool enabled)
{
	m_regexp = enabled;
	m_regexpControl->setChecked(enabled);
}

bool FindDialog::regexp() const
{
	return m_regexp;
}

void FindDialog::setForwards(const bool enabled)
{
	m_forwards = enabled;
	m_down->setChecked(enabled);
	if (!enabled)
		for (auto *button : m_down->parentWidget()->findChildren<QRadioButton *>())
			if (button != m_down)
				button->setChecked(true);
}

bool FindDialog::forwards() const
{
	return m_forwards;
}

void FindDialog::onRegexpHelp()
{
	QMudPreferences::information(this, QStringLiteral("Regular expressions"),
	                             QStringLiteral("Regular expression help is not available yet."));
}

int FindDialog::execModal()
{
	return exec();
}

void FindDialog::buildUi()
{
	setWindowTitle(QStringLiteral("Find"));
	setMinimumSize(520, 210);
	resize(520, 210);
	auto *mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(10, 10, 10, 10);
	mainLayout->setSpacing(8);
	auto *findLayout = new QHBoxLayout;
	findLayout->setSpacing(8);
	auto *findLabel = new QLabel(QStringLiteral("Find what:"), this);
	m_findCombo     = new QComboBox(this);
	m_findCombo->setEditable(true);
	m_findCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	m_findCombo->addItems(m_findHistory);
	if (auto *edit = m_findCombo->lineEdit())
	{
		edit->selectAll();
		edit->setFocus(Qt::OtherFocusReason);
	}
	findLabel->setBuddy(m_findCombo);
	findLayout->addWidget(findLabel);
	findLayout->addWidget(m_findCombo, 1);
	mainLayout->addLayout(findLayout);
	auto *bottomLayout = new QGridLayout;
	bottomLayout->setHorizontalSpacing(12);
	bottomLayout->setVerticalSpacing(6);
	bottomLayout->setColumnStretch(0, 1);
	auto *optionsWidget = new QWidget(this);
	auto *optionsLayout = new QGridLayout(optionsWidget);
	optionsLayout->setContentsMargins(0, 0, 0, 0);
	optionsLayout->setHorizontalSpacing(8);
	optionsLayout->setVerticalSpacing(6);
	optionsLayout->setColumnStretch(0, 1);
	m_matchCaseControl = new QCheckBox(QStringLiteral("Match case"), optionsWidget);
	m_regexpControl    = new QCheckBox(QStringLiteral("Regular expression"), optionsWidget);
	optionsLayout->addWidget(m_matchCaseControl, 0, 0, 1, 2, Qt::AlignLeft);
	optionsLayout->addWidget(m_regexpControl, 1, 0, 1, 1, Qt::AlignLeft);
	auto *regexpHelp = new QPushButton(QStringLiteral("?"), optionsWidget);
	regexpHelp->setFixedSize(28, 24);
	regexpHelp->setToolTip(QStringLiteral("Regular expression help"));
	optionsLayout->addWidget(regexpHelp, 1, 1, 1, 1, Qt::AlignLeft | Qt::AlignVCenter);
	auto *directionGroup = new QGroupBox(QStringLiteral("Direction"), this);
	directionGroup->setMinimumWidth(140);
	auto *directionLayout = new QVBoxLayout(directionGroup);
	auto *up              = new QRadioButton(QStringLiteral("Up"), directionGroup);
	m_down                = new QRadioButton(QStringLiteral("Down"), directionGroup);
	up->setChecked(true);
	directionLayout->addWidget(up);
	directionLayout->addWidget(m_down);
	auto *buttonWidget = new QWidget(this);
	auto *buttonLayout = new QVBoxLayout(buttonWidget);
	buttonLayout->setContentsMargins(0, 0, 0, 0);
	buttonLayout->setSpacing(8);
	auto *findButton = new QPushButton(QStringLiteral("Find"), buttonWidget);
	findButton->setDefault(true);
	findButton->setMinimumWidth(100);
	auto *cancelButton = new QPushButton(QStringLiteral("Cancel"), buttonWidget);
	cancelButton->setMinimumWidth(100);
	buttonLayout->addWidget(findButton);
	buttonLayout->addWidget(cancelButton);
	bottomLayout->addWidget(optionsWidget, 0, 0, Qt::AlignTop);
	bottomLayout->addWidget(directionGroup, 0, 1, Qt::AlignTop);
	bottomLayout->addWidget(buttonWidget, 0, 2, Qt::AlignTop);
	mainLayout->addLayout(bottomLayout);
	connect(regexpHelp, &QPushButton::clicked, this, &FindDialog::onRegexpHelp);
	connect(findButton, &QPushButton::clicked, this, &FindDialog::accept);
	connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
}

void FindDialog::accept()
{
	const QString text = m_findCombo->currentText();
	if (text.isEmpty())
	{
		QMudPreferences::information(this, QStringLiteral("Find"),
		                             QStringLiteral("You must specify something to search for."));
		m_findCombo->setFocus();
		return;
	}
	if (m_regexpControl->isChecked())
		if (const QRegularExpression regex(text); !regex.isValid())
		{
			QMudPreferences::warning(this, QStringLiteral("Find"),
			                         QStringLiteral("Regular expression error: %1").arg(regex.errorString()));
			return;
		}
	m_findText  = text;
	m_matchCase = m_matchCaseControl->isChecked();
	m_regexp    = m_regexpControl->isChecked();
	m_forwards  = m_down->isChecked();
	QDialog::accept();
}
