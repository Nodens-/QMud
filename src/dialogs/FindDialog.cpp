/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: FindDialog.cpp
 * Role: Find dialog behavior that executes text searches and coordinates match navigation with active views.
 */

#include "FindDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSizePolicy>

FindDialog::FindDialog(QStringList &findHistory, QWidget *parent)
    : QDialog(parent), m_findHistory(findHistory)
{
}

void FindDialog::setTitleText(const QString &title)
{
	m_title = title;
}

QString FindDialog::titleText() const
{
	return m_title;
}

void FindDialog::setFindText(const QString &text)
{
	m_findText = text;
}

QString FindDialog::findText() const
{
	return m_findText;
}

void FindDialog::setMatchCase(const bool enabled)
{
	m_matchCase = enabled;
}

bool FindDialog::matchCase() const
{
	return m_matchCase;
}

void FindDialog::setRegexp(const bool enabled)
{
	m_regexp = enabled;
}

bool FindDialog::regexp() const
{
	return m_regexp;
}

void FindDialog::setForwards(const bool enabled)
{
	m_forwards = enabled;
}

bool FindDialog::forwards() const
{
	return m_forwards;
}

void FindDialog::onRegexpHelp()
{
	QMessageBox::information(nullptr, QStringLiteral("Regular expressions"),
	                         QStringLiteral("Regular expression help is not available yet."));
}

int FindDialog::execModal()
{
	QDialog dlg(parentWidget());
	dlg.setWindowTitle(m_title.isEmpty() ? QStringLiteral("Find") : m_title);
	dlg.setMinimumSize(520, 210);
	dlg.resize(520, 210);

	QVBoxLayout mainLayout(&dlg);
	mainLayout.setContentsMargins(10, 10, 10, 10);
	mainLayout.setSpacing(8);

	QHBoxLayout findLayout;
	findLayout.setSpacing(8);

	QLabel    findLabel(QStringLiteral("Find what:"), &dlg);
	QComboBox findCombo(&dlg);
	findCombo.setEditable(true);
	findCombo.setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	for (const QString &item : m_findHistory)
		findCombo.addItem(item);
	if (!m_findText.isEmpty())
		findCombo.setCurrentText(m_findText);
	else if (findCombo.count() > 0)
		findCombo.setCurrentIndex(0);
	findLabel.setBuddy(&findCombo);

	findLayout.addWidget(&findLabel);
	findLayout.addWidget(&findCombo, 1);
	mainLayout.addLayout(&findLayout);

	QGridLayout bottomLayout;
	bottomLayout.setHorizontalSpacing(12);
	bottomLayout.setVerticalSpacing(6);
	bottomLayout.setColumnStretch(0, 1);

	QWidget     optionsWidget(&dlg);
	QGridLayout optionsLayout(&optionsWidget);
	optionsLayout.setContentsMargins(0, 0, 0, 0);
	optionsLayout.setHorizontalSpacing(8);
	optionsLayout.setVerticalSpacing(6);
	optionsLayout.setColumnStretch(0, 1);

	QCheckBox matchCase(QStringLiteral("Match case"), &optionsWidget);
	matchCase.setChecked(m_matchCase);
	QCheckBox useRegexp(QStringLiteral("Regular expression"), &optionsWidget);
	useRegexp.setChecked(m_regexp);
	optionsLayout.addWidget(&matchCase, 0, 0, 1, 2, Qt::AlignLeft);
	optionsLayout.addWidget(&useRegexp, 1, 0, 1, 1, Qt::AlignLeft);

	QPushButton regexpHelp(QStringLiteral("?"), &optionsWidget);
	regexpHelp.setFixedSize(28, 24);
	regexpHelp.setToolTip(QStringLiteral("Regular expression help"));
	optionsLayout.addWidget(&regexpHelp, 1, 1, 1, 1, Qt::AlignLeft | Qt::AlignVCenter);

	QGroupBox directionGroup(QStringLiteral("Direction"), &dlg);
	directionGroup.setMinimumWidth(140);
	QVBoxLayout  directionLayout(&directionGroup);
	QRadioButton up(QStringLiteral("Up"), &directionGroup);
	QRadioButton down(QStringLiteral("Down"), &directionGroup);
	directionLayout.addWidget(&up);
	directionLayout.addWidget(&down);
	(m_forwards ? down : up).setChecked(true);

	QWidget     buttonWidget(&dlg);
	QVBoxLayout buttonLayout(&buttonWidget);
	buttonLayout.setContentsMargins(0, 0, 0, 0);
	buttonLayout.setSpacing(8);

	QPushButton findButton(QStringLiteral("Find"), &buttonWidget);
	findButton.setDefault(true);
	findButton.setMinimumWidth(100);
	QPushButton cancelButton(QStringLiteral("Cancel"), &buttonWidget);
	cancelButton.setMinimumWidth(100);
	buttonLayout.addWidget(&findButton);
	buttonLayout.addWidget(&cancelButton);

	bottomLayout.addWidget(&optionsWidget, 0, 0, Qt::AlignTop);
	bottomLayout.addWidget(&directionGroup, 0, 1, Qt::AlignTop);
	bottomLayout.addWidget(&buttonWidget, 0, 2, Qt::AlignTop);
	mainLayout.addLayout(&bottomLayout);

	connect(&regexpHelp, &QPushButton::clicked, &dlg, [] { FindDialog::onRegexpHelp(); });
	connect(&findButton, &QPushButton::clicked, &dlg, &QDialog::accept);
	connect(&cancelButton, &QPushButton::clicked, &dlg, &QDialog::reject);

	while (dlg.exec() == Accepted)
	{
		const QString text = findCombo.currentText().trimmed();
		if (text.isEmpty())
		{
			QMessageBox::information(&dlg, QStringLiteral("Find"),
			                         QStringLiteral("You must specify something to search for."));
			findCombo.setFocus();
			continue;
		}

		if (useRegexp.isChecked())
		{
			if (const QRegularExpression regex(text); !regex.isValid())
			{
				QMessageBox::warning(&dlg, QStringLiteral("Find"),
				                     QStringLiteral("Regular expression error: %1").arg(regex.errorString()));
				continue;
			}
		}

		m_findText  = text;
		m_matchCase = matchCase.isChecked();
		m_regexp    = useRegexp.isChecked();
		m_forwards  = down.isChecked();
		return Accepted;
	}

	return Rejected;
}
