/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: SpellCheckDialog.cpp
 * Role: Spell-check dialog behavior for candidate suggestion display, replacement choice, and editor integration.
 */

#include "SpellCheckDialog.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <memory>

SpellCheckDialog::SpellCheckDialog(const QString &word, const QStringList &suggestions, QWidget *parent)
    : QDialog(parent), m_original(word)
{
	setWindowTitle(QStringLiteral("Spell Check"));
	setMinimumSize(620, 360);
	auto mainLayout = std::make_unique<QVBoxLayout>();

	auto topRow               = std::make_unique<QHBoxLayout>();
	auto notInDictionaryLabel = std::make_unique<QLabel>(QStringLiteral("Not in dictionary:"), this);
	topRow->addWidget(notInDictionaryLabel.release());
	auto wordEdit = std::make_unique<QLineEdit>(this);
	m_wordEdit    = wordEdit.release();
	m_wordEdit->setText(word);
	topRow->addWidget(m_wordEdit, 1);
	mainLayout->addLayout(topRow.release());

	auto suggestionsLabel = std::make_unique<QLabel>(QStringLiteral("Suggestions:"), this);
	mainLayout->addWidget(suggestionsLabel.release());
	auto suggestionsList = std::make_unique<QListWidget>(this);
	m_suggestions        = suggestionsList.release();
	m_suggestions->addItems(suggestions);
	if (!suggestions.isEmpty())
		m_suggestions->setCurrentRow(0);
	mainLayout->addWidget(m_suggestions, 1);

	auto buttonGrid = std::make_unique<QGridLayout>();
	auto ignore     = std::make_unique<QPushButton>(QStringLiteral("Ignore"), this);
	auto ignoreAll  = std::make_unique<QPushButton>(QStringLiteral("Ignore All"), this);
	auto add        = std::make_unique<QPushButton>(QStringLiteral("Add"), this);
	auto change     = std::make_unique<QPushButton>(QStringLiteral("Change"), this);
	auto changeAll  = std::make_unique<QPushButton>(QStringLiteral("Change All"), this);
	auto cancel     = std::make_unique<QPushButton>(QStringLiteral("Cancel"), this);

	for (QPushButton *btn :
	     {ignore.get(), ignoreAll.get(), add.get(), change.get(), changeAll.get(), cancel.get()})
	{
		btn->setDefault(false);
		btn->setAutoDefault(false);
	}

	buttonGrid->addWidget(ignore.get(), 0, 0);
	buttonGrid->addWidget(ignoreAll.get(), 0, 1);
	buttonGrid->addWidget(add.get(), 0, 2);
	buttonGrid->addWidget(change.get(), 1, 0);
	buttonGrid->addWidget(changeAll.get(), 1, 1);
	buttonGrid->addWidget(cancel.get(), 1, 2);
	mainLayout->addLayout(buttonGrid.release());
	setLayout(mainLayout.release());

	connect(ignore.release(), &QPushButton::clicked, this,
	        [this]
	        {
		        m_action = QStringLiteral("ignore");
		        QDialog::accept();
	        });
	connect(ignoreAll.release(), &QPushButton::clicked, this,
	        [this]
	        {
		        m_action = QStringLiteral("ignoreall");
		        QDialog::accept();
	        });
	connect(add.release(), &QPushButton::clicked, this,
	        [this]
	        {
		        m_action = QStringLiteral("add");
		        QDialog::accept();
	        });
	connect(change.release(), &QPushButton::clicked, this,
	        [this]
	        {
		        chooseSuggestionIfUnchanged();
		        m_action = QStringLiteral("change");
		        QDialog::accept();
	        });
	connect(changeAll.release(), &QPushButton::clicked, this,
	        [this]
	        {
		        chooseSuggestionIfUnchanged();
		        m_action = QStringLiteral("changeall");
		        QDialog::accept();
	        });
	connect(cancel.release(), &QPushButton::clicked, this, &QDialog::reject);
	connect(m_suggestions, &QListWidget::itemDoubleClicked, this,
	        [this]
	        {
		        chooseSuggestionIfUnchanged();
		        m_action = QStringLiteral("change");
		        QDialog::accept();
	        });
}

QString SpellCheckDialog::action() const
{
	return m_action;
}

QString SpellCheckDialog::replacement() const
{
	if (!m_wordEdit)
		return {};
	return m_wordEdit->text();
}

void SpellCheckDialog::accept()
{
	if (m_action.isEmpty())
		return;
	QDialog::accept();
}

void SpellCheckDialog::chooseSuggestionIfUnchanged() const
{
	if (!m_wordEdit || !m_suggestions)
		return;
	if (m_wordEdit->text() != m_original)
		return;
	const QListWidgetItem *item = m_suggestions->currentItem();
	if (!item)
		return;
	m_wordEdit->setText(item->text());
}
