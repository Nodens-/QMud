/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WelcomeDialog.cpp
 * Role: Welcome dialog behavior for onboarding actions, first-run messaging, and startup flow decisions.
 */

#include "dialogs/WelcomeDialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QVBoxLayout>
#include <memory>

WelcomeDialog::WelcomeDialog(const QString &message, QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QStringLiteral("Welcome"));
	setMinimumSize(480, 220);

	auto layout = std::make_unique<QVBoxLayout>();
	auto label  = std::make_unique<QLabel>(message, this);
	label->setWordWrap(true);
	layout->addWidget(label.release());

	auto buttons = std::make_unique<QDialogButtonBox>(QDialogButtonBox::Ok, this);
	layout->addWidget(buttons.get());
	setLayout(layout.release());

	connect(buttons.release(), &QDialogButtonBox::accepted, this, &QDialog::accept);
}
