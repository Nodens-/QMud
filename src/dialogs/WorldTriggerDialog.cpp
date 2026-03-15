/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldTriggerDialog.cpp
 * Role: Trigger editor dialog implementation for pattern/action configuration and trigger persistence.
 */

#include "dialogs/WorldTriggerDialog.h"
#include "StringUtils.h"
#include "WorldOptions.h"
#include "dialogs/WorldEditUtils.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QSpinBox>
#include <memory>
#include <utility>

namespace
{
	QStringList matchColourNames()
	{
		return {QStringLiteral("black"), QStringLiteral("maroon"),  QStringLiteral("green"),
		        QStringLiteral("olive"), QStringLiteral("navy"),    QStringLiteral("purple"),
		        QStringLiteral("teal"),  QStringLiteral("silver"),  QStringLiteral("gray"),
		        QStringLiteral("red"),   QStringLiteral("lime"),    QStringLiteral("yellow"),
		        QStringLiteral("blue"),  QStringLiteral("fuchsia"), QStringLiteral("aqua"),
		        QStringLiteral("white")};
	}

	bool editTextDialog(QWidget *parent, const QString &title, QString &text, const bool multiline)
	{
		QDialog dlg(parent);
		dlg.setWindowTitle(title);
		dlg.setMinimumSize(700, 440);
		auto  layout    = std::make_unique<QVBoxLayout>();
		auto *layoutPtr = layout.get();
		dlg.setLayout(layout.release());

		auto  edit    = std::make_unique<QPlainTextEdit>(&dlg);
		auto *editPtr = edit.get();
		editPtr->setPlainText(text);
		WorldEditUtils::applyEditorPreferences(editPtr);
		layoutPtr->addWidget(edit.release());

		auto buttons =
		    std::make_unique<QDialogButtonBox>(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
		const auto *buttonsPtr = buttons.get();
		QObject::connect(buttonsPtr, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		QObject::connect(buttonsPtr, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
		layoutPtr->addWidget(buttons.release());

		if (dlg.exec() != QDialog::Accepted)
			return false;

		text = editPtr->toPlainText();
		if (!multiline)
		{
			text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
			text.replace(QLatin1Char('\n'), QLatin1Char(' '));
		}
		return true;
	}

	constexpr int kMaxRecentLines = 200;
} // namespace

WorldTriggerDialog::WorldTriggerDialog(WorldRuntime *runtime, WorldRuntime::Trigger trigger,
                                       QList<WorldRuntime::Trigger> triggers, const int currentIndex,
                                       QWidget *parent)
    : QDialog(parent), m_runtime(runtime), m_trigger(std::move(trigger)), m_existing(std::move(triggers)),
      m_currentIndex(currentIndex)
{
	setWindowTitle(currentIndex < 0 ? QStringLiteral("Add trigger") : QStringLiteral("Edit trigger"));
	resize(520, 360);
	setMinimumSize(920, 690);
	buildUi();
	loadTrigger();
	updateControls();
}

WorldRuntime::Trigger WorldTriggerDialog::trigger() const
{
	return m_trigger;
}

void WorldTriggerDialog::accept()
{
	if (!validateTrigger())
		return;

	QMap<QString, QString> &attrs = m_trigger.attributes;
	attrs.insert(QStringLiteral("match"), m_match->text());
	attrs.insert(QStringLiteral("name"), m_label->text());
	attrs.insert(QStringLiteral("script"), m_script->text());
	attrs.insert(QStringLiteral("group"), m_group->text());
	attrs.insert(QStringLiteral("variable"), m_variable->text());
	attrs.insert(QStringLiteral("sequence"), QString::number(m_sequence->value()));
	attrs.insert(QStringLiteral("send_to"), QString::number(sendToValue()));
	attrs.insert(QStringLiteral("enabled"), qmudBoolToYn(m_enabled->isChecked()));
	attrs.insert(QStringLiteral("ignore_case"), qmudBoolToYn(m_ignoreCase->isChecked()));
	attrs.insert(QStringLiteral("omit_from_log"), qmudBoolToYn(m_omitFromLog->isChecked()));
	attrs.insert(QStringLiteral("omit_from_output"), qmudBoolToYn(m_omitFromOutput->isChecked()));
	attrs.insert(QStringLiteral("keep_evaluating"), qmudBoolToYn(m_keepEvaluating->isChecked()));
	attrs.insert(QStringLiteral("regexp"), qmudBoolToYn(m_regexp->isChecked()));
	attrs.insert(QStringLiteral("repeat"), qmudBoolToYn(m_repeat->isChecked()));
	attrs.insert(QStringLiteral("expand_variables"), qmudBoolToYn(m_expandVariables->isChecked()));
	attrs.insert(QStringLiteral("temporary"), qmudBoolToYn(m_temporary->isChecked()));
	attrs.insert(QStringLiteral("multi_line"), qmudBoolToYn(m_multiLine->isChecked()));
	attrs.insert(QStringLiteral("lines_to_match"), QString::number(m_linesToMatch->value()));
	attrs.insert(QStringLiteral("lowercase_wildcard"), qmudBoolToYn(m_lowercaseWildcard->isChecked()));
	attrs.insert(QStringLiteral("one_shot"), qmudBoolToYn(m_oneShot->isChecked()));
	attrs.insert(QStringLiteral("clipboard_arg"),
	             QString::number(m_wildcardClipboard->currentData().toInt()));

	const int matchTextIndex = m_matchTextColour->currentIndex();
	attrs.insert(QStringLiteral("match_text_colour"), qmudBoolToYn(matchTextIndex > 0));
	attrs.insert(QStringLiteral("text_colour"), QString::number(matchTextIndex > 0 ? matchTextIndex - 1 : 0));

	const int matchBackIndex = m_matchBackColour->currentIndex();
	attrs.insert(QStringLiteral("match_back_colour"), qmudBoolToYn(matchBackIndex > 0));
	attrs.insert(QStringLiteral("back_colour"), QString::number(matchBackIndex > 0 ? matchBackIndex - 1 : 0));

	applyMatchState(m_matchBold, QStringLiteral("match_bold"), QStringLiteral("bold"));
	applyMatchState(m_matchItalic, QStringLiteral("match_italic"), QStringLiteral("italic"));
	applyMatchState(m_matchInverse, QStringLiteral("match_inverse"), QStringLiteral("inverse"));

	attrs.insert(QStringLiteral("colour_change_type"),
	             QString::number(m_colourChangeType->currentData().toInt()));
	attrs.insert(QStringLiteral("make_bold"), qmudBoolToYn(m_makeBold->isChecked()));
	attrs.insert(QStringLiteral("make_italic"), qmudBoolToYn(m_makeItalic->isChecked()));
	attrs.insert(QStringLiteral("make_underline"), qmudBoolToYn(m_makeUnderline->isChecked()));

	const int customColour = m_triggerColour->currentData().toInt();
	attrs.insert(QStringLiteral("custom_colour"), QString::number(customColour));

	attrs.insert(QStringLiteral("sound_if_inactive"), qmudBoolToYn(m_soundIfInactive->isChecked()));
	if (const QString soundText = m_soundPathLabel->text(); soundText == QStringLiteral("(No sound)"))
		attrs.remove(QStringLiteral("sound"));
	else
		attrs.insert(QStringLiteral("sound"), soundText);

	attrs.insert(QStringLiteral("other_text_colour"), m_otherTextColour->property("colourValue").toString());
	attrs.insert(QStringLiteral("other_back_colour"), m_otherBackColour->property("colourValue").toString());

	m_trigger.children.insert(QStringLiteral("send"), m_send->toPlainText());

	QDialog::accept();
}

void WorldTriggerDialog::onEditMatch()
{
	if (QString text = m_match->text();
	    editTextDialog(this, QStringLiteral("Edit trigger 'match' text"), text, false))
		m_match->setText(text);
}

void WorldTriggerDialog::onEditSend()
{
	if (QString text = m_send->toPlainText();
	    editTextDialog(this, QStringLiteral("Edit trigger 'send' text"), text, true))
		m_send->setPlainText(text);
}

void WorldTriggerDialog::onConvertToRegexp()
{
	const QString current = m_match->text();
	if (current.contains(QStringLiteral("**")) && !m_regexp->isChecked())
	{
		WorldEditUtils::showMultipleAsterisksWarning(this);
		return;
	}

	m_match->setText(WorldEditUtils::convertToRegularExpression(current));
	m_regexp->setChecked(true);
}

void WorldTriggerDialog::onBrowseSound()
{
	const QString fileName =
	    QFileDialog::getOpenFileName(this, QStringLiteral("Choose sound file"), QString(),
	                                 QStringLiteral("Sound files (*.wav *.mp3 *.ogg);;All files (*.*)"));
	if (!fileName.isEmpty())
		m_soundPathLabel->setText(fileName);
}

void WorldTriggerDialog::onNoSound() const
{
	m_soundPathLabel->setText(QStringLiteral("(No sound)"));
}

void WorldTriggerDialog::onTestSound()
{
	QMessageBox::information(this, QStringLiteral("Sound"),
	                         QStringLiteral("Use a world trigger to test sound playback."));
}

void WorldTriggerDialog::onSendToChanged(int) const
{
	const bool variableEnabled = sendToValue() == eSendToVariable;
	m_variable->setEnabled(variableEnabled);
	if (m_variableLabel)
		m_variableLabel->setText(variableEnabled ? QStringLiteral("Variable:") : QStringLiteral("n/a:"));
	updateControls();
}

void WorldTriggerDialog::updateControls() const
{
	const bool included = isIncluded();
	if (m_buttons)
	{
		if (QAbstractButton *ok = m_buttons->button(QDialogButtonBox::Ok); ok)
			ok->setEnabled(!included && !m_match->text().trimmed().isEmpty());
	}

	const bool regexpChecked = m_regexp->isChecked();
	m_convertRegexpButton->setEnabled(!regexpChecked);

	const bool multiline = m_multiLine->isChecked();
	m_linesToMatch->setEnabled(multiline);
	m_matchTextColour->setEnabled(!multiline);
	m_matchBackColour->setEnabled(!multiline);
	m_matchBold->setEnabled(!multiline);
	m_matchItalic->setEnabled(!multiline);
	m_matchInverse->setEnabled(!multiline);

	m_makeBold->setEnabled(!multiline);
	m_makeItalic->setEnabled(!multiline);
	m_makeUnderline->setEnabled(!multiline);
	m_triggerColour->setEnabled(!multiline);
	m_colourChangeType->setEnabled(!multiline);
	const bool otherSelected = m_triggerColour->currentData().toInt() == OTHER_CUSTOM + 1;
	m_otherTextColour->setEnabled(!multiline && otherSelected);
	m_otherBackColour->setEnabled(!multiline && otherSelected);
	updateTriggerColourSwatches();

	m_omitFromLog->setEnabled(!multiline);
	m_omitFromOutput->setEnabled(!multiline);
	m_repeat->setEnabled(regexpChecked && !multiline);
	m_multiLine->setEnabled(regexpChecked);
	m_testSound->setEnabled(m_soundPathLabel->text() != QStringLiteral("(No sound)"));
}

void WorldTriggerDialog::buildUi()
{
	auto  mainLayout    = std::make_unique<QVBoxLayout>();
	auto *mainLayoutPtr = mainLayout.get();
	setLayout(mainLayout.release());

	auto  topLayout    = std::make_unique<QVBoxLayout>();
	auto *topLayoutPtr = topLayout.get();
	topLayoutPtr->setContentsMargins(0, 0, 0, 0);
	topLayoutPtr->setSpacing(2);
	auto  matchRow    = std::make_unique<QHBoxLayout>();
	auto *matchRowPtr = matchRow.get();
	matchRowPtr->setContentsMargins(0, 0, 0, 0);
	matchRowPtr->setSpacing(2);
	auto  matchLabel    = std::make_unique<QLabel>(QStringLiteral("Trigger:"), this);
	auto *matchLabelPtr = matchLabel.get();
	matchLabelPtr->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
	matchLabelPtr->setFixedWidth(matchLabelPtr->sizeHint().width());
	m_match = new QLineEdit(this);
	matchLabelPtr->setBuddy(m_match);
	WorldEditUtils::applyEditorPreferences(m_match);
	m_editMatchButton = new QPushButton(QStringLiteral("..."), this);
	m_editMatchButton->setFixedWidth(24);
	m_convertRegexpButton = new QPushButton(QStringLiteral("Convert to Regular Expression"), this);
	matchRowPtr->addWidget(matchLabel.release());
	matchRowPtr->addWidget(m_match, 1);
	auto  matchButtonRow    = std::make_unique<QHBoxLayout>();
	auto *matchButtonRowPtr = matchButtonRow.get();
	matchButtonRowPtr->setContentsMargins(0, 0, 0, 0);
	matchButtonRowPtr->setSpacing(6);
	const int matchFieldIndent = matchLabelPtr->sizeHint().width() + matchRowPtr->spacing();
	matchButtonRowPtr->addSpacing(matchFieldIndent);
	matchButtonRowPtr->addWidget(m_editMatchButton);
	matchButtonRowPtr->addWidget(m_convertRegexpButton);
	matchButtonRowPtr->addStretch();
	topLayoutPtr->addLayout(matchRow.release());
	topLayoutPtr->addLayout(matchButtonRow.release());
	mainLayoutPtr->addLayout(topLayout.release());

	auto  matchStyleLayout    = std::make_unique<QGridLayout>();
	auto *matchStyleLayoutPtr = matchStyleLayout.get();
	auto  matchColourLabel    = std::make_unique<QLabel>(QStringLiteral("Match:"), this);
	m_matchTextColour         = new QComboBox(this);
	m_matchBackColour         = new QComboBox(this);
	m_matchTextColour->addItem(QStringLiteral("Any"));
	m_matchBackColour->addItem(QStringLiteral("Any"));
	for (const QString &name : matchColourNames())
	{
		m_matchTextColour->addItem(name);
		m_matchBackColour->addItem(name);
	}
	auto onLabel   = std::make_unique<QLabel>(QStringLiteral("on"), this);
	m_matchBold    = new QCheckBox(QStringLiteral("Bold"), this);
	m_matchItalic  = new QCheckBox(QStringLiteral("Italic"), this);
	m_matchInverse = new QCheckBox(QStringLiteral("Inverse"), this);
	m_matchBold->setTristate(true);
	m_matchItalic->setTristate(true);
	m_matchInverse->setTristate(true);

	matchStyleLayoutPtr->addWidget(matchColourLabel.release(), 0, 0);
	matchStyleLayoutPtr->addWidget(m_matchTextColour, 0, 1);
	matchStyleLayoutPtr->addWidget(onLabel.release(), 0, 2);
	matchStyleLayoutPtr->addWidget(m_matchBackColour, 0, 3);
	matchStyleLayoutPtr->addWidget(m_matchBold, 0, 4);
	matchStyleLayoutPtr->addWidget(m_matchItalic, 0, 5);
	matchStyleLayoutPtr->addWidget(m_matchInverse, 0, 6);
	mainLayoutPtr->addLayout(matchStyleLayout.release());

	auto  sendLayout    = std::make_unique<QHBoxLayout>();
	auto *sendLayoutPtr = sendLayout.get();
	auto  sendLeft      = std::make_unique<QVBoxLayout>();
	auto *sendLeftPtr   = sendLeft.get();
	auto  sendLabel     = std::make_unique<QLabel>(QStringLiteral("Send:"), this);
	m_send              = new QPlainTextEdit(this);
	WorldEditUtils::applyEditorPreferences(m_send);
	m_editSendButton = new QPushButton(QStringLiteral("..."), this);
	m_editSendButton->setFixedWidth(24);
	auto  sendButtonLayout    = std::make_unique<QHBoxLayout>();
	auto *sendButtonLayoutPtr = sendButtonLayout.get();
	sendButtonLayoutPtr->addWidget(m_editSendButton);
	sendButtonLayoutPtr->addStretch();
	sendLeftPtr->addWidget(sendLabel.release());
	sendLeftPtr->addWidget(m_send);
	sendLeftPtr->addLayout(sendButtonLayout.release());
	sendLayoutPtr->addLayout(sendLeft.release(), 4);

	auto  optionsLayout    = std::make_unique<QVBoxLayout>();
	auto *optionsLayoutPtr = optionsLayout.get();
	m_enabled              = new QCheckBox(QStringLiteral("Enabled"), this);
	m_ignoreCase           = new QCheckBox(QStringLiteral("Ignore case"), this);
	m_omitFromLog          = new QCheckBox(QStringLiteral("Omit from log file"), this);
	m_omitFromOutput       = new QCheckBox(QStringLiteral("Omit from output"), this);
	m_keepEvaluating       = new QCheckBox(QStringLiteral("Keep evaluating"), this);
	m_regexp               = new QCheckBox(QStringLiteral("Regular expression"), this);
	m_repeat               = new QCheckBox(QStringLiteral("Repeat on same line"), this);
	m_expandVariables      = new QCheckBox(QStringLiteral("Expand variables"), this);
	m_temporary            = new QCheckBox(QStringLiteral("Temporary"), this);
	m_multiLine            = new QCheckBox(QStringLiteral("Multi-line, match:"), this);
	m_linesToMatch         = new QSpinBox(this);
	m_linesToMatch->setRange(0, kMaxRecentLines);
	m_lowercaseWildcard = new QCheckBox(QStringLiteral("Make wildcards lower-case"), this);
	m_oneShot           = new QCheckBox(QStringLiteral("One-shot"), this);

	optionsLayoutPtr->addWidget(m_enabled);
	optionsLayoutPtr->addWidget(m_ignoreCase);
	optionsLayoutPtr->addWidget(m_omitFromLog);
	optionsLayoutPtr->addWidget(m_omitFromOutput);
	optionsLayoutPtr->addWidget(m_keepEvaluating);
	optionsLayoutPtr->addWidget(m_regexp);
	optionsLayoutPtr->addWidget(m_repeat);
	optionsLayoutPtr->addWidget(m_expandVariables);
	optionsLayoutPtr->addWidget(m_temporary);
	auto  multiLayout    = std::make_unique<QHBoxLayout>();
	auto *multiLayoutPtr = multiLayout.get();
	multiLayoutPtr->addWidget(m_multiLine);
	multiLayoutPtr->addWidget(m_linesToMatch);
	auto multiLinesLabel = std::make_unique<QLabel>(QStringLiteral("lines."), this);
	multiLayoutPtr->addWidget(multiLinesLabel.release());
	optionsLayoutPtr->addLayout(multiLayout.release());
	optionsLayoutPtr->addWidget(m_lowercaseWildcard);
	optionsLayoutPtr->addWidget(m_oneShot);
	optionsLayoutPtr->addStretch();
	sendLayoutPtr->addLayout(optionsLayout.release(), 1);
	mainLayoutPtr->addLayout(sendLayout.release());

	auto  bottomLayout    = std::make_unique<QGridLayout>();
	auto *bottomLayoutPtr = bottomLayout.get();
	auto  sendToLabel     = std::make_unique<QLabel>(QStringLiteral("Send to:"), this);
	m_sendTo              = new QComboBox(this);
	WorldEditUtils::populateSendToCombo(m_sendTo);
	sendToLabel->setBuddy(m_sendTo);
	auto seqLabel = std::make_unique<QLabel>(QStringLiteral("Sequence:"), this);
	m_sequence    = new QSpinBox(this);
	m_sequence->setRange(0, 10000);
	seqLabel->setBuddy(m_sequence);
	auto wildcardLabel  = std::make_unique<QLabel>(QStringLiteral("Copy wildcard:"), this);
	m_wildcardClipboard = new QComboBox(this);
	m_wildcardClipboard->addItem(QStringLiteral("None"), 0);
	for (int i = 1; i < MAX_WILDCARDS; ++i)
		m_wildcardClipboard->addItem(QString::number(i), i);
	auto labelLabel = std::make_unique<QLabel>(QStringLiteral("Label:"), this);
	m_label         = new QLineEdit(this);
	labelLabel->setBuddy(m_label);
	auto scriptLabel = std::make_unique<QLabel>(QStringLiteral("Script:"), this);
	m_script         = new QLineEdit(this);
	scriptLabel->setBuddy(m_script);
	auto groupLabel = std::make_unique<QLabel>(QStringLiteral("Group:"), this);
	m_group         = new QLineEdit(this);
	groupLabel->setBuddy(m_group);
	m_variableLabel = new QLabel(QStringLiteral("Variable:"), this);
	m_variable      = new QLineEdit(this);
	m_variableLabel->setBuddy(m_variable);

	bottomLayoutPtr->addWidget(sendToLabel.release(), 0, 0);
	bottomLayoutPtr->addWidget(m_sendTo, 0, 1);
	bottomLayoutPtr->addWidget(seqLabel.release(), 1, 0);
	bottomLayoutPtr->addWidget(m_sequence, 1, 1);
	bottomLayoutPtr->addWidget(wildcardLabel.release(), 2, 0);
	bottomLayoutPtr->addWidget(m_wildcardClipboard, 2, 1);
	bottomLayoutPtr->addWidget(labelLabel.release(), 3, 0);
	bottomLayoutPtr->addWidget(m_label, 3, 1);
	bottomLayoutPtr->addWidget(scriptLabel.release(), 4, 0);
	bottomLayoutPtr->addWidget(m_script, 4, 1);
	bottomLayoutPtr->addWidget(groupLabel.release(), 5, 0);
	bottomLayoutPtr->addWidget(m_group, 5, 1);
	bottomLayoutPtr->addWidget(m_variableLabel, 6, 0);
	bottomLayoutPtr->addWidget(m_variable, 6, 1);

	auto  colourGroup     = std::make_unique<QGroupBox>(QStringLiteral("Change colour and style to:"), this);
	auto *colourGroupPtr  = colourGroup.get();
	auto  colourLayout    = std::make_unique<QGridLayout>();
	auto *colourLayoutPtr = colourLayout.get();
	colourGroupPtr->setLayout(colourLayout.release());
	m_triggerColour = new QComboBox(this);
	m_triggerColour->addItem(QStringLiteral("(no change)"), 0);
	QStringList customNames;
	customNames.reserve(MAX_CUSTOM);
	for (int i = 0; i < MAX_CUSTOM; ++i)
		customNames.append(QStringLiteral("Custom%1").arg(i + 1));
	if (m_runtime)
	{
		for (const auto &[group, attributes] : m_runtime->colours())
		{
			if (!group.startsWith(QStringLiteral("custom/")))
				continue;
			bool      ok  = false;
			const int seq = attributes.value(QStringLiteral("seq")).toInt(&ok);
			if (!ok || seq < 1 || seq > MAX_CUSTOM)
				continue;
			if (const QString name = attributes.value(QStringLiteral("name")); !name.isEmpty())
				customNames[seq - 1] = name;
			const QString textColour = attributes.value(QStringLiteral("text"));
			const QString backColour = attributes.value(QStringLiteral("back"));
			m_customColours.insert(seq,
			                       qMakePair(parseColourValue(textColour), parseColourValue(backColour)));
		}
	}
	for (int i = 0; i < MAX_CUSTOM; ++i)
		m_triggerColour->addItem(customNames.at(i), i + 1);
	m_triggerColour->addItem(QStringLiteral("Other ..."), OTHER_CUSTOM + 1);
	m_colourChangeType = new QComboBox(this);
	m_colourChangeType->addItem(QStringLiteral("Both"), TRIGGER_COLOUR_CHANGE_BOTH);
	m_colourChangeType->addItem(QStringLiteral("Text"), TRIGGER_COLOUR_CHANGE_FOREGROUND);
	m_colourChangeType->addItem(QStringLiteral("Back"), TRIGGER_COLOUR_CHANGE_BACKGROUND);

	m_makeBold        = new QCheckBox(QStringLiteral("Bold"), this);
	m_makeItalic      = new QCheckBox(QStringLiteral("Italic"), this);
	m_makeUnderline   = new QCheckBox(QStringLiteral("Underline"), this);
	m_otherTextColour = new QPushButton(QString(), this);
	m_otherBackColour = new QPushButton(QString(), this);
	m_otherTextColour->setFixedSize(15, 12);
	m_otherBackColour->setFixedSize(15, 12);
	m_otherTextColour->setProperty("colourValue", QStringLiteral("black"));
	m_otherBackColour->setProperty("colourValue", QStringLiteral("black"));
	m_otherTextColour->setToolTip(QStringLiteral("Other text colour"));
	m_otherBackColour->setToolTip(QStringLiteral("Other back colour"));

	colourLayoutPtr->addWidget(m_triggerColour, 0, 0, 1, 2);
	colourLayoutPtr->addWidget(m_colourChangeType, 0, 2);
	colourLayoutPtr->addWidget(m_makeBold, 1, 0);
	colourLayoutPtr->addWidget(m_makeItalic, 1, 1);
	colourLayoutPtr->addWidget(m_makeUnderline, 1, 2);
	colourLayoutPtr->addWidget(m_otherTextColour, 2, 0);
	colourLayoutPtr->addWidget(m_otherBackColour, 2, 1);

	auto  soundGroup     = std::make_unique<QGroupBox>(QStringLiteral("Sound"), this);
	auto *soundGroupPtr  = soundGroup.get();
	auto  soundLayout    = std::make_unique<QGridLayout>();
	auto *soundLayoutPtr = soundLayout.get();
	soundGroupPtr->setLayout(soundLayout.release());
	m_browseSound     = new QPushButton(QStringLiteral("Browse..."), this);
	m_testSound       = new QPushButton(QStringLiteral("Test"), this);
	m_noSound         = new QPushButton(QStringLiteral("No sound"), this);
	m_soundIfInactive = new QCheckBox(QStringLiteral("Only if inactive"), this);
	m_soundPathLabel  = new QLabel(QStringLiteral("(No sound)"), this);
	soundLayoutPtr->addWidget(m_browseSound, 0, 0);
	soundLayoutPtr->addWidget(m_testSound, 0, 1);
	soundLayoutPtr->addWidget(m_noSound, 0, 2);
	soundLayoutPtr->addWidget(m_soundIfInactive, 0, 3);
	soundLayoutPtr->addWidget(m_soundPathLabel, 1, 0, 1, 4);

	bottomLayoutPtr->addWidget(colourGroup.release(), 0, 2, 4, 2);
	bottomLayoutPtr->addWidget(soundGroup.release(), 4, 2, 3, 2);

	m_includedLabel  = new QLabel(QStringLiteral("(included)"), this);
	m_matchesLabel   = new QLabel(QStringLiteral("0 matches."), this);
	m_callsLabel     = new QLabel(QStringLiteral("0 calls."), this);
	m_timeTakenLabel = new QLabel(QStringLiteral("0.0 sec."), this);
	bottomLayoutPtr->addWidget(m_includedLabel, 0, 4, 1, 1, Qt::AlignRight);
	bottomLayoutPtr->addWidget(m_matchesLabel, 1, 4, 1, 1, Qt::AlignRight);
	bottomLayoutPtr->addWidget(m_timeTakenLabel, 2, 4, 1, 1, Qt::AlignRight);
	bottomLayoutPtr->addWidget(m_callsLabel, 3, 4, 1, 1, Qt::AlignRight);

	mainLayoutPtr->addLayout(bottomLayout.release());

	m_buttons =
	    new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Help, this);
	connect(m_buttons, &QDialogButtonBox::accepted, this, &WorldTriggerDialog::accept);
	connect(m_buttons, &QDialogButtonBox::rejected, this, &WorldTriggerDialog::reject);
	connect(m_buttons->button(QDialogButtonBox::Help), &QPushButton::clicked, this,
	        [this]
	        {
		        QMessageBox::information(this, QStringLiteral("Help"),
		                                 QStringLiteral("Help is not available for this dialog."));
	        });
	mainLayoutPtr->addWidget(m_buttons);

	connect(m_editMatchButton, &QPushButton::clicked, this, &WorldTriggerDialog::onEditMatch);
	connect(m_editSendButton, &QPushButton::clicked, this, &WorldTriggerDialog::onEditSend);
	connect(m_convertRegexpButton, &QPushButton::clicked, this, &WorldTriggerDialog::onConvertToRegexp);
	connect(m_browseSound, &QPushButton::clicked, this, &WorldTriggerDialog::onBrowseSound);
	connect(m_testSound, &QPushButton::clicked, this, &WorldTriggerDialog::onTestSound);
	connect(m_noSound, &QPushButton::clicked, this, &WorldTriggerDialog::onNoSound);
	connect(m_sendTo, qOverload<int>(&QComboBox::currentIndexChanged), this,
	        [this](const int index) { onSendToChanged(index); });
	connect(m_match, &QLineEdit::textChanged, this, [this] { updateControls(); });
	connect(m_regexp, &QCheckBox::toggled, this, [this] { updateControls(); });
	connect(m_multiLine, &QCheckBox::toggled, this, [this] { updateControls(); });
	connect(m_triggerColour, qOverload<int>(&QComboBox::currentIndexChanged), this,
	        [this] { updateControls(); });
	connect(m_otherTextColour, &QPushButton::clicked, this,
	        [this]
	        {
		        const QColor current(m_otherTextColour->property("colourValue").toString());
		        const QColor chosen =
		            QColorDialog::getColor(current, this, QStringLiteral("Choose text colour"));
		        if (chosen.isValid())
		        {
			        m_otherTextColour->setProperty("colourValue", chosen.name());
			        updateTriggerColourSwatches();
		        }
	        });
	connect(m_otherBackColour, &QPushButton::clicked, this,
	        [this]
	        {
		        const QColor current(m_otherBackColour->property("colourValue").toString());
		        const QColor chosen =
		            QColorDialog::getColor(current, this, QStringLiteral("Choose back colour"));
		        if (chosen.isValid())
		        {
			        m_otherBackColour->setProperty("colourValue", chosen.name());
			        updateTriggerColourSwatches();
		        }
	        });
}

void WorldTriggerDialog::loadTrigger() const
{
	const QMap<QString, QString> &attrs = m_trigger.attributes;
	m_match->setText(attrs.value(QStringLiteral("match")));
	m_label->setText(attrs.value(QStringLiteral("name")));
	m_script->setText(attrs.value(QStringLiteral("script")));
	m_group->setText(attrs.value(QStringLiteral("group")));
	m_variable->setText(attrs.value(QStringLiteral("variable")));
	m_sequence->setValue(attrs.value(QStringLiteral("sequence")).toInt());
	m_send->setPlainText(m_trigger.children.value(QStringLiteral("send")));
	m_enabled->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("enabled"))));
	m_ignoreCase->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("ignore_case"))));
	m_omitFromLog->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("omit_from_log"))));
	m_omitFromOutput->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("omit_from_output"))));
	m_keepEvaluating->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("keep_evaluating"))));
	m_regexp->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("regexp"))));
	m_repeat->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("repeat"))));
	m_expandVariables->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("expand_variables"))));
	m_temporary->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("temporary"))));
	m_multiLine->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("multi_line"))));
	m_linesToMatch->setValue(attrs.value(QStringLiteral("lines_to_match")).toInt());
	m_lowercaseWildcard->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("lowercase_wildcard"))));
	m_oneShot->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("one_shot"))));
	const int clipboardIndex =
	    m_wildcardClipboard->findData(attrs.value(QStringLiteral("clipboard_arg")).toInt());
	m_wildcardClipboard->setCurrentIndex(clipboardIndex >= 0 ? clipboardIndex : 0);

	const int sendTo = attrs.value(QStringLiteral("send_to")).toInt();
	if (const int sendToIndex = m_sendTo->findData(sendTo); sendToIndex >= 0)
		m_sendTo->setCurrentIndex(sendToIndex);

	const bool matchText = qmudIsEnabledFlag(attrs.value(QStringLiteral("match_text_colour")));
	const bool matchBack = qmudIsEnabledFlag(attrs.value(QStringLiteral("match_back_colour")));
	if (matchText)
		m_matchTextColour->setCurrentIndex(attrs.value(QStringLiteral("text_colour")).toInt() + 1);
	else
		m_matchTextColour->setCurrentIndex(0);
	if (matchBack)
		m_matchBackColour->setCurrentIndex(attrs.value(QStringLiteral("back_colour")).toInt() + 1);
	else
		m_matchBackColour->setCurrentIndex(0);

	setMatchState(m_matchBold, QStringLiteral("match_bold"), QStringLiteral("bold"));
	setMatchState(m_matchItalic, QStringLiteral("match_italic"), QStringLiteral("italic"));
	setMatchState(m_matchInverse, QStringLiteral("match_inverse"), QStringLiteral("inverse"));

	const int colourType = attrs.value(QStringLiteral("colour_change_type")).toInt();
	if (const int typeIndex = m_colourChangeType->findData(colourType); typeIndex >= 0)
		m_colourChangeType->setCurrentIndex(typeIndex);
	m_makeBold->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("make_bold"))));
	m_makeItalic->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("make_italic"))));
	m_makeUnderline->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("make_underline"))));

	const int customColour = attrs.value(QStringLiteral("custom_colour")).toInt();
	if (const int customIndex = m_triggerColour->findData(customColour); customIndex >= 0)
		m_triggerColour->setCurrentIndex(customIndex);

	m_soundIfInactive->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("sound_if_inactive"))));
	if (const QString sound = attrs.value(QStringLiteral("sound")); !sound.isEmpty())
		m_soundPathLabel->setText(sound);

	const QString otherText = attrs.value(QStringLiteral("other_text_colour"));
	const QString otherBack = attrs.value(QStringLiteral("other_back_colour"));
	if (!otherText.isEmpty())
		m_otherTextColour->setProperty("colourValue", otherText);
	if (!otherBack.isEmpty())
		m_otherBackColour->setProperty("colourValue", otherBack);

	const QString matches   = attrs.value(QStringLiteral("times_matched"));
	const QString calls     = attrs.value(QStringLiteral("invocation_count"));
	const QString timeTaken = attrs.value(QStringLiteral("execution_time"));
	if (!matches.isEmpty())
		m_matchesLabel->setText(QStringLiteral("%1 match%2.")
		                            .arg(matches)
		                            .arg(matches == QStringLiteral("1") ? QString() : QStringLiteral("es")));
	if (!calls.isEmpty())
		m_callsLabel->setText(QStringLiteral("%1 call%2.")
		                          .arg(calls)
		                          .arg(calls == QStringLiteral("1") ? QString() : QStringLiteral("s")));
	if (!timeTaken.isEmpty())
		m_timeTakenLabel->setText(QStringLiteral("%1 sec.").arg(timeTaken));

	m_includedLabel->setVisible(isIncluded());
	onSendToChanged(m_sendTo->currentIndex());
	updateTriggerColourSwatches();
}

bool WorldTriggerDialog::validateTrigger()
{
	const QString matchText = m_match->text();
	const QString sendText  = m_send->toPlainText();
	const QString label     = m_label->text().trimmed();
	const QString procedure = m_script->text().trimmed();
	const QString group     = m_group->text().trimmed();
	const QString variable  = m_variable->text().trimmed();

	m_label->setText(label);
	m_script->setText(procedure);
	m_group->setText(group);
	m_variable->setText(variable);

	if (matchText.isEmpty())
	{
		QMessageBox::warning(this, QStringLiteral("QMud"),
		                     QStringLiteral("The trigger match text cannot be blank."));
		m_match->setFocus();
		return false;
	}

	// check for foolishly using ** in a non-regular expression
	if (matchText.contains(QStringLiteral("**")) && !m_regexp->isChecked())
	{
		WorldEditUtils::showMultipleAsterisksWarning(this);
		m_match->setFocus();
		return false;
	}

	const QList<ushort> invalidMatch = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
	                                    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
	if (const int invalidMatchPos = WorldEditUtils::findInvalidChar(matchText, invalidMatch);
	    invalidMatchPos != -1)
	{
		const ushort code = matchText.at(invalidMatchPos).unicode();
		QMessageBox::warning(
		    this, QStringLiteral("QMud"),
		    QStringLiteral("The trigger 'match' text contains an invalid non-printable character "
		                   "(hex %1) at position %2.")
		        .arg(code, 2, 16, QLatin1Char('0'))
		        .arg(invalidMatchPos + 1));
		m_match->setFocus();
		return false;
	}

	// we allow carriage-return, linefeed, tab here
	const QList<ushort> invalidSend = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
	                                   0x07, 0x08, 0x0B, 0x0C, 0x0E, 0x0F};
	if (const int invalidSendPos = WorldEditUtils::findInvalidChar(sendText, invalidSend);
	    invalidSendPos != -1)
	{
		const ushort code = sendText.at(invalidSendPos).unicode();
		QMessageBox::warning(
		    this, QStringLiteral("QMud"),
		    QStringLiteral("The trigger 'send' text contains an invalid non-printable character "
		                   "(hex %1) at position %2.")
		        .arg(code, 2, 16, QLatin1Char('0'))
		        .arg(invalidSendPos + 1));
		m_send->setFocus();
		return false;
	}

	if (m_regexp->isChecked())
	{
		QRegularExpression::PatternOptions opts = QRegularExpression::NoPatternOption;
		if (m_multiLine->isChecked())
			opts |= QRegularExpression::MultilineOption;
		if (!WorldEditUtils::checkRegularExpression(this, matchText, opts))
		{
			m_match->setFocus();
			return false;
		}
	}

	if (!label.isEmpty())
	{
		for (int i = 0; i < m_existing.size(); ++i)
		{
			if (i == m_currentIndex)
				continue;
			if (const QString other = m_existing.at(i).attributes.value(QStringLiteral("name"));
			    !other.isEmpty() && other.compare(label, Qt::CaseInsensitive) == 0)
			{
				QMessageBox::warning(
				    this, QStringLiteral("QMud"),
				    QStringLiteral("The trigger label \"%1\" is already in the list of triggers.")
				        .arg(label));
				m_label->setFocus();
				return false;
			}
		}

		if (WorldEditUtils::checkLabelInvalid(label, false))
		{
			QMessageBox::warning(
			    this, QStringLiteral("QMud"),
			    QStringLiteral("The label must start with a letter and consist of letters, numbers "
			                   "or the underscore character."));
			m_label->setFocus();
			return false;
		}
	}

	if (variable.isEmpty())
	{
		if (sendToValue() == eSendToVariable)
		{
			QMessageBox::warning(
			    this, QStringLiteral("QMud"),
			    QStringLiteral("When sending to a variable you must specify a variable name."));
			m_variable->setFocus();
			return false;
		}
	}
	else
	{
		if (WorldEditUtils::checkLabelInvalid(variable, false))
		{
			QMessageBox::warning(
			    this, QStringLiteral("QMud"),
			    QStringLiteral("The variable must start with a letter and consist of letters, "
			                   "numbers or the underscore character."));
			m_variable->setFocus();
			return false;
		}
	}

	if (!procedure.isEmpty())
	{
		if (WorldEditUtils::checkLabelInvalid(procedure, true))
		{
			QMessageBox::warning(
			    this, QStringLiteral("QMud"),
			    QStringLiteral("The script subroutine name must start with a letter and consist of "
			                   "letters, numbers or the underscore character."));
			m_script->setFocus();
			return false;
		}
	}

	if (sendToValue() != eSendToWorld && sendText.isEmpty())
	{
		const int choice = QMessageBox::question(
		    this, QStringLiteral("QMud"),
		    QStringLiteral("Your trigger is set to 'send to %1' however the 'Send:' field is blank.\n\n"
		                   "You can use \"%%0\" to send the entire matching line to the specified place.\n\n"
		                   "(You can eliminate this message by sending to 'world')\n\n"
		                   "Do you want to change the trigger to fix this?")
		        .arg(WorldEditUtils::sendToLabel(sendToValue())),
		    QMessageBox::Yes | QMessageBox::No);
		if (choice == QMessageBox::Yes)
		{
			m_send->setFocus();
			return false;
		}
	}

	if (m_multiLine->isChecked() && !m_regexp->isChecked())
	{
		QMessageBox::warning(this, QStringLiteral("QMud"),
		                     QStringLiteral("Multi-line triggers must be a regular expression"));
		return false;
	}

	if (m_multiLine->isChecked() && m_linesToMatch->value() < 2)
	{
		QMessageBox::warning(this, QStringLiteral("QMud"),
		                     QStringLiteral("Multi-line triggers must match at least 2 lines"));
		m_linesToMatch->setFocus();
		return false;
	}

	return true;
}

bool WorldTriggerDialog::isIncluded() const
{
	if (m_trigger.included)
		return true;
	const QString included = m_trigger.attributes.value(QStringLiteral("included"));
	return qmudIsEnabledFlag(included);
}

int WorldTriggerDialog::sendToValue() const
{
	if (const QVariant data = m_sendTo->currentData(); data.isValid())
		return data.toInt();
	return eSendToWorld;
}

void WorldTriggerDialog::applyMatchState(const QCheckBox *box, const QString &matchKey,
                                         const QString &valueKey)
{
	if (!box)
		return;

	const Qt::CheckState state = box->checkState();
	if (state == Qt::PartiallyChecked)
	{
		m_trigger.attributes.insert(matchKey, qmudBoolToYn(false));
		m_trigger.attributes.insert(valueKey, qmudBoolToYn(false));
		return;
	}

	m_trigger.attributes.insert(matchKey, qmudBoolToYn(true));
	m_trigger.attributes.insert(valueKey, qmudBoolToYn(state == Qt::Checked));
}

void WorldTriggerDialog::setMatchState(QCheckBox *box, const QString &matchKey, const QString &valueKey) const
{
	if (!box)
		return;

	if (const bool matchEnabled = qmudIsEnabledFlag(m_trigger.attributes.value(matchKey)); !matchEnabled)
	{
		box->setCheckState(Qt::PartiallyChecked);
		return;
	}

	const bool value = qmudIsEnabledFlag(m_trigger.attributes.value(valueKey));
	box->setCheckState(value ? Qt::Checked : Qt::Unchecked);
}

QColor WorldTriggerDialog::parseColourValue(const QString &value)
{
	if (value.isEmpty())
		return {};
	if (const QColor colour(value); colour.isValid())
		return colour;
	bool       ok  = false;
	const uint rgb = value.toUInt(&ok, 0);
	if (ok)
		return QColor::fromRgb(rgb);
	return {};
}

void WorldTriggerDialog::updateTriggerColourSwatches() const
{
	const int customColour = m_triggerColour->currentData().toInt();
	QColor    text;
	QColor    back;
	if (customColour == OTHER_CUSTOM + 1)
	{
		text = parseColourValue(m_otherTextColour->property("colourValue").toString());
		back = parseColourValue(m_otherBackColour->property("colourValue").toString());
	}
	else if (customColour > 0)
	{
		const auto [textColour, backColour] = m_customColours.value(customColour);
		text                                = textColour;
		back                                = backColour;
	}

	if (text.isValid())
		m_otherTextColour->setStyleSheet(QStringLiteral("background-color: %1;").arg(text.name()));
	else
		m_otherTextColour->setStyleSheet(QString());

	if (back.isValid())
		m_otherBackColour->setStyleSheet(QStringLiteral("background-color: %1;").arg(back.name()));
	else
		m_otherBackColour->setStyleSheet(QString());

	const QString textTip = text.isValid() ? text.name() : QStringLiteral("(unset)");
	const QString backTip = back.isValid() ? back.name() : QStringLiteral("(unset)");
	m_otherTextColour->setToolTip(QStringLiteral("Other text colour: %1").arg(textTip));
	m_otherBackColour->setToolTip(QStringLiteral("Other back colour: %1").arg(backTip));
}
