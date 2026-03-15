/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: HighlightPhraseDialog.cpp
 * Role: Highlight-phrase dialog logic that edits phrase/style rules consumed by runtime text highlighting.
 */

#include "HighlightPhraseDialog.h"

#include "ColourPickerDialog.h"
#include "WorldOptions.h"

#include <QCheckBox>
#include <QComboBox>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>

#include <memory>

HighlightPhraseDialog::HighlightPhraseDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QStringLiteral("Highlight phrase"));
	setMinimumSize(520, 260);

	auto  layout     = std::make_unique<QVBoxLayout>();
	auto *mainLayout = layout.get();
	setLayout(layout.release());

	auto  form       = std::make_unique<QFormLayout>();
	auto *formLayout = form.get();
	m_text           = new QLineEdit(this);
	m_text->setMaxLength(255);
	formLayout->addRow(QStringLiteral("Text:"), m_text);

	m_wholeWord = new QCheckBox(QStringLiteral("Whole word"), this);
	m_matchCase = new QCheckBox(QStringLiteral("Match case"), this);
	formLayout->addRow(QString(), m_wholeWord);
	formLayout->addRow(QString(), m_matchCase);

	m_colourCombo = new QComboBox(this);
	formLayout->addRow(QStringLiteral("Colour:"), m_colourCombo);
	mainLayout->addLayout(form.release());

	auto  swatches       = std::make_unique<QHBoxLayout>();
	auto *swatchesLayout = swatches.get();
	auto  textLabel      = std::make_unique<QLabel>(QStringLiteral("Text:"), this);
	swatchesLayout->addWidget(textLabel.release());
	m_textSwatch = new QPushButton(this);
	m_textSwatch->setFixedSize(32, 16);
	swatchesLayout->addWidget(m_textSwatch);
	swatchesLayout->addSpacing(10);
	auto backLabel = std::make_unique<QLabel>(QStringLiteral("Background:"), this);
	swatchesLayout->addWidget(backLabel.release());
	m_backSwatch = new QPushButton(this);
	m_backSwatch->setFixedSize(32, 16);
	swatchesLayout->addWidget(m_backSwatch);
	swatchesLayout->addStretch(1);
	mainLayout->addLayout(swatches.release());

	auto  buttons = std::make_unique<QDialogButtonBox>(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	auto *buttonsBox = buttons.get();
	mainLayout->addWidget(buttons.release());

	connect(buttonsBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttonsBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(m_colourCombo, &QComboBox::currentIndexChanged, this, [this](int) { updateSwatches(); });
	connect(m_textSwatch, &QPushButton::clicked, this,
	        [this]
	        {
		        if (m_colourCombo->currentIndex() != OTHER_CUSTOM + 1)
			        return;
		        ColourPickerDialog dlg(this);
		        dlg.setPickColour(true);
		        dlg.setInitialColour(m_otherForeground);
		        if (dlg.exec() != Accepted)
			        return;
		        m_otherForeground = dlg.selectedColour();
		        updateSwatches();
	        });
	connect(m_backSwatch, &QPushButton::clicked, this,
	        [this]
	        {
		        if (m_colourCombo->currentIndex() != OTHER_CUSTOM + 1)
			        return;
		        ColourPickerDialog dlg(this);
		        dlg.setPickColour(true);
		        dlg.setInitialColour(m_otherBackground);
		        if (dlg.exec() != Accepted)
			        return;
		        m_otherBackground = dlg.selectedColour();
		        updateSwatches();
	        });
}

void HighlightPhraseDialog::setCustomColours(const QVector<QColor>  &textColours,
                                             const QVector<QColor>  &backColours,
                                             const QVector<QString> &names)
{
	m_customText  = textColours;
	m_customBack  = backColours;
	m_customNames = names;

	m_colourCombo->clear();
	m_colourCombo->addItem(QStringLiteral("(no change)"));
	for (int i = 0; i < m_customNames.size(); ++i)
	{
		const QString name =
		    m_customNames[i].isEmpty() ? QStringLiteral("Custom%1").arg(i + 1) : m_customNames[i];
		m_colourCombo->addItem(name);
	}
	m_colourCombo->addItem(QStringLiteral("Other ..."));
	m_colourCombo->setCurrentIndex(1);
	updateSwatches();
}

void HighlightPhraseDialog::setOtherColours(const QColor &fore, const QColor &back)
{
	m_otherForeground     = fore;
	m_otherBackground     = back;
	m_otherForegroundOrig = fore;
	m_otherBackgroundOrig = back;
	updateSwatches();
}

void HighlightPhraseDialog::setInitialText(const QString &text) const
{
	if (m_text)
		m_text->setText(text);
}

void HighlightPhraseDialog::setWholeWord(const bool enabled) const
{
	if (m_wholeWord)
		m_wholeWord->setChecked(enabled);
}

void HighlightPhraseDialog::setMatchCase(const bool enabled) const
{
	if (m_matchCase)
		m_matchCase->setChecked(enabled);
}

void HighlightPhraseDialog::setSelectedColourIndex(const int index)
{
	if (!m_colourCombo)
		return;
	m_colourCombo->setCurrentIndex(qBound(0, index, m_colourCombo->count() - 1));
	updateSwatches();
}

QString HighlightPhraseDialog::phraseText() const
{
	return m_text ? m_text->text() : QString();
}

bool HighlightPhraseDialog::matchCase() const
{
	return m_matchCase && m_matchCase->isChecked();
}

bool HighlightPhraseDialog::wholeWord() const
{
	return m_wholeWord && m_wholeWord->isChecked();
}

int HighlightPhraseDialog::selectedColourIndex() const
{
	return m_colourCombo ? m_colourCombo->currentIndex() : 0;
}

QColor HighlightPhraseDialog::otherTextColour() const
{
	return m_otherForeground;
}

QColor HighlightPhraseDialog::otherBackColour() const
{
	return m_otherBackground;
}

void HighlightPhraseDialog::accept()
{
	if (!m_text || !m_colourCombo)
		return;
	if (m_text->text().trimmed().isEmpty())
	{
		QMessageBox::warning(this, QStringLiteral("Highlight phrase"),
		                     QStringLiteral("The text to highlight cannot be empty."));
		m_text->setFocus();
		return;
	}
	const int colourIndex = m_colourCombo->currentIndex();
	if (colourIndex <= 0)
	{
		QMessageBox::warning(this, QStringLiteral("Highlight phrase"),
		                     QStringLiteral("Please choose a colour other than '(no change)'."));
		m_colourCombo->setFocus();
		return;
	}
	if (colourIndex == OTHER_CUSTOM + 1 && m_otherForeground == m_otherForegroundOrig &&
	    m_otherBackground == m_otherBackgroundOrig)
	{
		QMessageBox::warning(this, QStringLiteral("Highlight phrase"),
		                     QStringLiteral("Please choose a different colour than the original one."));
		return;
	}
	QDialog::accept();
}

void HighlightPhraseDialog::updateSwatches()
{
	if (!m_colourCombo || !m_textSwatch || !m_backSwatch)
		return;

	const int index = m_colourCombo->currentIndex();
	if (index == 0)
	{
		m_textSwatch->setVisible(false);
		m_backSwatch->setVisible(false);
		return;
	}

	m_textSwatch->setVisible(true);
	m_backSwatch->setVisible(true);

	if (index - 1 == OTHER_CUSTOM)
	{
		m_textSwatch->setEnabled(true);
		m_backSwatch->setEnabled(true);
		updateSwatchButton(m_textSwatch, m_otherForeground);
		updateSwatchButton(m_backSwatch, m_otherBackground);
		return;
	}

	const int customIndex = index - 1;
	if (customIndex < 0 || customIndex >= m_customText.size() || customIndex >= m_customBack.size())
		return;

	m_textSwatch->setEnabled(false);
	m_backSwatch->setEnabled(false);
	updateSwatchButton(m_textSwatch, m_customText[customIndex]);
	updateSwatchButton(m_backSwatch, m_customBack[customIndex]);
}

void HighlightPhraseDialog::updateSwatchButton(QPushButton *button, const QColor &color)
{
	if (!button)
		return;
	const QString css = QStringLiteral("background-color: %1;").arg(color.name());
	button->setStyleSheet(css);
}
