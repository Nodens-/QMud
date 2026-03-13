/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WelcomeUpgradeDialog.cpp
 * Role: Upgrade welcome dialog implementation presenting release changes and user actions after version upgrades.
 */

#include "dialogs/WelcomeUpgradeDialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QVBoxLayout>
#include <memory>

WelcomeUpgradeDialog::WelcomeUpgradeDialog(const QString &message1, const QString &message2, QWidget *parent)
    : QDialog(parent)
{
	setWindowTitle(QStringLiteral("Welcome"));
	setMinimumSize(560, 280);

	auto layout = std::make_unique<QVBoxLayout>();
	auto label1 = std::make_unique<QLabel>(message1, this);
	label1->setWordWrap(true);
	layout->addWidget(label1.release());

	auto label2 = std::make_unique<QLabel>(message2, this);
	label2->setWordWrap(true);
	layout->addWidget(label2.release());

	auto buttons = std::make_unique<QDialogButtonBox>(QDialogButtonBox::Ok, this);
	layout->addWidget(buttons.get());
	setLayout(layout.release());

	connect(buttons.release(), &QDialogButtonBox::accepted, this, &QDialog::accept);
}
