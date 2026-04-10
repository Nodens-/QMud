/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldAliasDialog.cpp
 * Role: Alias editor dialog behavior for validating, editing, and persisting world alias entries.
 */

#include "dialogs/WorldAliasDialog.h"
#include "StringUtils.h"
#include "WorldOptions.h"
#include "helpers/WorldEditUtils.h"

#include <QCheckBox>
#include <QComboBox>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QSpinBox>
#include <memory>

namespace
{
	bool editTextDialog(QWidget *parent, const QString &title, QString &text, const bool multiline)
	{
		QDialog dlg(parent);
		dlg.setWindowTitle(title);
		dlg.setMinimumSize(700, 440);
		auto  layout    = std::make_unique<QVBoxLayout>();
		auto *layoutPtr = layout.get();
		dlg.setLayout(layout.release());
		if (multiline)
		{
			auto  edit    = std::make_unique<QPlainTextEdit>(&dlg);
			auto *editPtr = edit.get();
			editPtr->setPlainText(text);
			WorldEditUtils::applyEditorPreferences(editPtr);
			layoutPtr->addWidget(edit.release());
			auto buttons =
			    std::make_unique<QDialogButtonBox>(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
			QObject::connect(buttons.get(), &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
			QObject::connect(buttons.get(), &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
			layoutPtr->addWidget(buttons.release());
			if (dlg.exec() != QDialog::Accepted)
				return false;
			text = editPtr->toPlainText();
			return true;
		}

		auto  edit    = std::make_unique<QPlainTextEdit>(&dlg);
		auto *editPtr = edit.get();
		editPtr->setPlainText(text);
		WorldEditUtils::applyEditorPreferences(editPtr);
		layoutPtr->addWidget(edit.release());
		auto buttons =
		    std::make_unique<QDialogButtonBox>(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
		QObject::connect(buttons.get(), &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		QObject::connect(buttons.get(), &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
		layoutPtr->addWidget(buttons.release());
		if (dlg.exec() != QDialog::Accepted)
			return false;
		text = editPtr->toPlainText();
		text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
		text.replace(QLatin1Char('\n'), QLatin1Char(' '));
		return true;
	}
} // namespace

WorldAliasDialog::WorldAliasDialog(WorldRuntime *runtime, WorldRuntime::Alias alias,
                                   QList<WorldRuntime::Alias> aliases, int currentIndex, QWidget *parent)
    : QDialog(parent), m_runtime(runtime), m_alias(std::move(alias)), m_existing(std::move(aliases)),
      m_currentIndex(currentIndex)
{
	setWindowTitle(currentIndex < 0 ? QStringLiteral("Add alias") : QStringLiteral("Edit alias"));
	resize(520, 360);
	setMinimumSize(920, 690);
	buildUi();
	loadAlias();
	updateButtons();
}

WorldRuntime::Alias WorldAliasDialog::alias() const
{
	return m_alias;
}

void WorldAliasDialog::accept()
{
	if (!validateAlias())
		return;

	QMap<QString, QString> &attrs = m_alias.attributes;
	attrs.insert(QStringLiteral("match"), m_match->text());
	attrs.insert(QStringLiteral("name"), m_label->text());
	attrs.insert(QStringLiteral("script"), m_script->text());
	attrs.insert(QStringLiteral("group"), m_group->text());
	attrs.insert(QStringLiteral("variable"), m_variable->text());
	attrs.insert(QStringLiteral("sequence"), QString::number(m_sequence->value()));
	attrs.insert(QStringLiteral("send_to"), QString::number(sendToValue()));
	attrs.insert(QStringLiteral("enabled"), qmudBoolToYn(m_enabled->isChecked()));
	attrs.insert(QStringLiteral("omit_from_log"), qmudBoolToYn(m_omitFromLog->isChecked()));
	attrs.insert(QStringLiteral("ignore_case"), qmudBoolToYn(m_ignoreCase->isChecked()));
	attrs.insert(QStringLiteral("regexp"), qmudBoolToYn(m_regexp->isChecked()));
	attrs.insert(QStringLiteral("expand_variables"), qmudBoolToYn(m_expandVariables->isChecked()));
	attrs.insert(QStringLiteral("omit_from_output"), qmudBoolToYn(m_omitFromOutput->isChecked()));
	attrs.insert(QStringLiteral("temporary"), qmudBoolToYn(m_temporary->isChecked()));
	attrs.insert(QStringLiteral("keep_evaluating"), qmudBoolToYn(m_keepEvaluating->isChecked()));
	attrs.insert(QStringLiteral("echo_alias"), qmudBoolToYn(m_echoAlias->isChecked()));
	attrs.insert(QStringLiteral("omit_from_command_history"),
	             qmudBoolToYn(m_omitFromCommandHistory->isChecked()));
	attrs.insert(QStringLiteral("menu"), qmudBoolToYn(m_menu->isChecked()));
	attrs.insert(QStringLiteral("one_shot"), qmudBoolToYn(m_oneShot->isChecked()));
	m_alias.children.insert(QStringLiteral("send"), m_send->toPlainText());

	QDialog::accept();
}

void WorldAliasDialog::onEditMatch()
{
	if (QString text = m_match->text();
	    editTextDialog(this, QStringLiteral("Edit alias 'match' text"), text, false))
		m_match->setText(text);
}

void WorldAliasDialog::onEditSend()
{
	if (QString text = m_send->toPlainText();
	    editTextDialog(this, QStringLiteral("Edit alias 'send' text"), text, true))
		m_send->setPlainText(text);
}

void WorldAliasDialog::onConvertToRegexp()
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

void WorldAliasDialog::onReverseSpeedwalk()
{
	const QString result = WorldEditUtils::reverseSpeedwalk(m_send->toPlainText());
	if (!result.isEmpty() && result.at(0) == QLatin1Char('*'))
	{
		QMessageBox::warning(this, QStringLiteral("QMud"), result.mid(1));
		return;
	}
	m_send->setPlainText(result);
}

void WorldAliasDialog::onSendToChanged(int) const
{
	const bool variableEnabled = sendToValue() == eSendToVariable;
	m_variable->setEnabled(variableEnabled);
	updateButtons();
}

void WorldAliasDialog::updateButtons() const
{
	const bool included = isIncluded();
	const bool hasLabel = !m_label->text().trimmed().isEmpty();
	if (m_buttons)
	{
		if (QAbstractButton *ok = m_buttons->button(QDialogButtonBox::Ok); ok)
			ok->setEnabled(!included && !m_match->text().trimmed().isEmpty());
	}

	const bool    speedwalk = sendToValue() == eSendToSpeedwalk;
	const QString contents  = m_send->toPlainText();
	m_reverseSpeedwalkButton->setEnabled(speedwalk && !contents.isEmpty() &&
	                                     !contents.contains(QLatin1Char('%')));

	const bool regexpChecked = m_regexp->isChecked();
	m_convertRegexpButton->setEnabled(!regexpChecked);
	m_script->setEnabled(hasLabel);
}

void WorldAliasDialog::buildUi()
{
	auto  mainLayout    = std::make_unique<QVBoxLayout>();
	auto *mainLayoutPtr = mainLayout.get();
	setLayout(mainLayout.release());

	auto topLayout = std::make_unique<QVBoxLayout>();
	topLayout->setContentsMargins(0, 0, 0, 0);
	topLayout->setSpacing(2);
	auto matchRow = std::make_unique<QHBoxLayout>();
	matchRow->setContentsMargins(0, 0, 0, 0);
	matchRow->setSpacing(2);
	auto  matchLabel    = std::make_unique<QLabel>(QStringLiteral("Alias:"), this);
	auto *matchLabelPtr = matchLabel.get();
	matchLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
	matchLabel->setFixedWidth(matchLabel->sizeHint().width());
	auto matchEdit = std::make_unique<QLineEdit>(this);
	m_match        = matchEdit.release();
	matchLabel->setBuddy(m_match);
	WorldEditUtils::applyEditorPreferences(m_match);
	auto editMatchButton = std::make_unique<QPushButton>(QStringLiteral("..."), this);
	m_editMatchButton    = editMatchButton.release();
	m_editMatchButton->setFixedWidth(24);
	auto convertRegexpButton =
	    std::make_unique<QPushButton>(QStringLiteral("Convert to Regular Expression"), this);
	m_convertRegexpButton = convertRegexpButton.release();
	matchRow->addWidget(matchLabel.release());
	matchRow->addWidget(m_match, 1);
	auto matchButtonRow = std::make_unique<QHBoxLayout>();
	matchButtonRow->setContentsMargins(0, 0, 0, 0);
	matchButtonRow->setSpacing(6);
	const int matchFieldIndent = matchLabelPtr->sizeHint().width() + matchRow->spacing();
	matchButtonRow->addSpacing(matchFieldIndent);
	matchButtonRow->addWidget(m_editMatchButton);
	matchButtonRow->addWidget(m_convertRegexpButton);
	matchButtonRow->addStretch();
	topLayout->addLayout(matchRow.release());
	topLayout->addLayout(matchButtonRow.release());
	mainLayoutPtr->addLayout(topLayout.release());

	auto sendLayout = std::make_unique<QHBoxLayout>();
	auto sendLeft   = std::make_unique<QVBoxLayout>();
	auto sendLabel  = std::make_unique<QLabel>(QStringLiteral("Send:"), this);
	auto sendEdit   = std::make_unique<QPlainTextEdit>(this);
	m_send          = sendEdit.release();
	WorldEditUtils::applyEditorPreferences(m_send);
	auto editSendButton = std::make_unique<QPushButton>(QStringLiteral("..."), this);
	m_editSendButton    = editSendButton.release();
	m_editSendButton->setFixedWidth(24);
	auto reverseSpeedwalkButton = std::make_unique<QPushButton>(QStringLiteral("Reverse Speedwalk"), this);
	m_reverseSpeedwalkButton    = reverseSpeedwalkButton.release();
	auto sendButtonLayout       = std::make_unique<QHBoxLayout>();
	sendButtonLayout->addWidget(m_editSendButton);
	sendButtonLayout->addWidget(m_reverseSpeedwalkButton);
	sendButtonLayout->addStretch();
	sendLeft->addWidget(sendLabel.release());
	sendLeft->addWidget(m_send);
	sendLeft->addLayout(sendButtonLayout.release());
	sendLayout->addLayout(sendLeft.release(), 4);

	auto optionsLayout   = std::make_unique<QVBoxLayout>();
	auto enabled         = std::make_unique<QCheckBox>(QStringLiteral("Enabled"), this);
	m_enabled            = enabled.release();
	auto omitFromLog     = std::make_unique<QCheckBox>(QStringLiteral("Omit From Log File"), this);
	m_omitFromLog        = omitFromLog.release();
	auto ignoreCase      = std::make_unique<QCheckBox>(QStringLiteral("Ignore Case"), this);
	m_ignoreCase         = ignoreCase.release();
	auto regexp          = std::make_unique<QCheckBox>(QStringLiteral("Regular Expression"), this);
	m_regexp             = regexp.release();
	auto expandVariables = std::make_unique<QCheckBox>(QStringLiteral("Expand Variables"), this);
	m_expandVariables    = expandVariables.release();
	auto omitFromOutput  = std::make_unique<QCheckBox>(QStringLiteral("Omit From Output"), this);
	m_omitFromOutput     = omitFromOutput.release();
	auto temporary       = std::make_unique<QCheckBox>(QStringLiteral("Temporary"), this);
	m_temporary          = temporary.release();
	auto keepEvaluating  = std::make_unique<QCheckBox>(QStringLiteral("Keep Evaluating"), this);
	m_keepEvaluating     = keepEvaluating.release();
	auto echoAlias       = std::make_unique<QCheckBox>(QStringLiteral("Echo Alias"), this);
	m_echoAlias          = echoAlias.release();
	auto omitFromCommandHistory =
	    std::make_unique<QCheckBox>(QStringLiteral("Omit From Command History"), this);
	m_omitFromCommandHistory = omitFromCommandHistory.release();
	auto menu                = std::make_unique<QCheckBox>(QStringLiteral("Menu"), this);
	m_menu                   = menu.release();
	auto oneShot             = std::make_unique<QCheckBox>(QStringLiteral("One-shot"), this);
	m_oneShot                = oneShot.release();
	optionsLayout->addWidget(m_enabled);
	optionsLayout->addWidget(m_omitFromLog);
	optionsLayout->addWidget(m_ignoreCase);
	optionsLayout->addWidget(m_regexp);
	optionsLayout->addWidget(m_expandVariables);
	optionsLayout->addWidget(m_omitFromOutput);
	optionsLayout->addWidget(m_temporary);
	optionsLayout->addWidget(m_keepEvaluating);
	optionsLayout->addWidget(m_echoAlias);
	optionsLayout->addWidget(m_omitFromCommandHistory);
	optionsLayout->addWidget(m_menu);
	optionsLayout->addWidget(m_oneShot);
	optionsLayout->addStretch();
	sendLayout->addLayout(optionsLayout.release(), 1);

	mainLayoutPtr->addLayout(sendLayout.release());

	auto bottomLayout = std::make_unique<QGridLayout>();
	auto sendToLabel  = std::make_unique<QLabel>(QStringLiteral("Send To:"), this);
	auto sendToCombo  = std::make_unique<QComboBox>(this);
	m_sendTo          = sendToCombo.release();
	WorldEditUtils::populateSendToCombo(m_sendTo);
	sendToLabel->setBuddy(m_sendTo);
	auto seqLabel = std::make_unique<QLabel>(QStringLiteral("Sequence:"), this);
	auto sequence = std::make_unique<QSpinBox>(this);
	m_sequence    = sequence.release();
	m_sequence->setRange(0, 10000);
	seqLabel->setBuddy(m_sequence);
	auto labelLabel = std::make_unique<QLabel>(QStringLiteral("Label:"), this);
	auto labelEdit  = std::make_unique<QLineEdit>(this);
	m_label         = labelEdit.release();
	labelLabel->setBuddy(m_label);
	auto scriptLabel = std::make_unique<QLabel>(QStringLiteral("Script:"), this);
	auto scriptEdit  = std::make_unique<QLineEdit>(this);
	m_script         = scriptEdit.release();
	scriptLabel->setBuddy(m_script);
	auto groupLabel = std::make_unique<QLabel>(QStringLiteral("Group:"), this);
	auto groupEdit  = std::make_unique<QLineEdit>(this);
	m_group         = groupEdit.release();
	groupLabel->setBuddy(m_group);
	auto variableLabel = std::make_unique<QLabel>(QStringLiteral("Variable:"), this);
	auto variableEdit  = std::make_unique<QLineEdit>(this);
	m_variable         = variableEdit.release();
	variableLabel->setBuddy(m_variable);

	bottomLayout->addWidget(sendToLabel.release(), 0, 0);
	bottomLayout->addWidget(m_sendTo, 0, 1);
	bottomLayout->addWidget(seqLabel.release(), 1, 0);
	bottomLayout->addWidget(m_sequence, 1, 1);
	bottomLayout->addWidget(labelLabel.release(), 2, 0);
	bottomLayout->addWidget(m_label, 2, 1);
	bottomLayout->addWidget(scriptLabel.release(), 3, 0);
	bottomLayout->addWidget(m_script, 3, 1);
	bottomLayout->addWidget(groupLabel.release(), 4, 0);
	bottomLayout->addWidget(m_group, 4, 1);
	bottomLayout->addWidget(variableLabel.release(), 5, 0);
	bottomLayout->addWidget(m_variable, 5, 1);

	auto includedLabel = std::make_unique<QLabel>(QStringLiteral("(included)"), this);
	m_includedLabel    = includedLabel.release();
	auto matchesLabel  = std::make_unique<QLabel>(QStringLiteral("0 matches."), this);
	m_matchesLabel     = matchesLabel.release();
	auto callsLabel    = std::make_unique<QLabel>(QStringLiteral("0 calls."), this);
	m_callsLabel       = callsLabel.release();
	bottomLayout->addWidget(m_includedLabel, 0, 2, 1, 2, Qt::AlignRight);
	bottomLayout->addWidget(m_matchesLabel, 1, 2, 1, 2, Qt::AlignRight);
	bottomLayout->addWidget(m_callsLabel, 2, 2, 1, 2, Qt::AlignRight);

	mainLayoutPtr->addLayout(bottomLayout.release());

	auto buttons = std::make_unique<QDialogButtonBox>(
	    QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Help, this);
	m_buttons = buttons.release();
	connect(m_buttons, &QDialogButtonBox::accepted, this, &WorldAliasDialog::accept);
	connect(m_buttons, &QDialogButtonBox::rejected, this, &WorldAliasDialog::reject);
	connect(m_buttons->button(QDialogButtonBox::Help), &QPushButton::clicked, this,
	        [this]
	        {
		        QMessageBox::information(this, QStringLiteral("Help"),
		                                 QStringLiteral("Help is not available for this dialog."));
	        });
	mainLayoutPtr->addWidget(m_buttons);

	connect(m_editMatchButton, &QPushButton::clicked, this, &WorldAliasDialog::onEditMatch);
	connect(m_editSendButton, &QPushButton::clicked, this, &WorldAliasDialog::onEditSend);
	connect(m_convertRegexpButton, &QPushButton::clicked, this, &WorldAliasDialog::onConvertToRegexp);
	connect(m_reverseSpeedwalkButton, &QPushButton::clicked, this, &WorldAliasDialog::onReverseSpeedwalk);
	connect(m_sendTo, qOverload<int>(&QComboBox::currentIndexChanged), this,
	        &WorldAliasDialog::onSendToChanged);
	connect(m_match, &QLineEdit::textChanged, this, &WorldAliasDialog::updateButtons);
	connect(m_label, &QLineEdit::textChanged, this, &WorldAliasDialog::updateButtons);
	connect(m_regexp, &QCheckBox::toggled, this, &WorldAliasDialog::updateButtons);
	connect(m_send, &QPlainTextEdit::textChanged, this, &WorldAliasDialog::updateButtons);
}

void WorldAliasDialog::loadAlias() const
{
	const QMap<QString, QString> &attrs = m_alias.attributes;
	m_match->setText(attrs.value(QStringLiteral("match")));
	m_label->setText(attrs.value(QStringLiteral("name")));
	m_script->setText(attrs.value(QStringLiteral("script")));
	m_group->setText(attrs.value(QStringLiteral("group")));
	m_variable->setText(attrs.value(QStringLiteral("variable")));
	m_sequence->setValue(attrs.value(QStringLiteral("sequence")).toInt());
	m_send->setPlainText(m_alias.children.value(QStringLiteral("send")));
	m_enabled->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("enabled"))));
	m_omitFromLog->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("omit_from_log"))));
	m_ignoreCase->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("ignore_case"))));
	m_regexp->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("regexp"))));
	m_expandVariables->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("expand_variables"))));
	m_omitFromOutput->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("omit_from_output"))));
	m_temporary->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("temporary"))));
	m_keepEvaluating->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("keep_evaluating"))));
	m_echoAlias->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("echo_alias"))));
	m_omitFromCommandHistory->setChecked(
	    qmudIsEnabledFlag(attrs.value(QStringLiteral("omit_from_command_history"))));
	m_menu->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("menu"))));
	m_oneShot->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("one_shot"))));

	if (const int sendToIndex = m_sendTo->findData(attrs.value(QStringLiteral("send_to")).toInt());
	    sendToIndex >= 0)
		m_sendTo->setCurrentIndex(sendToIndex);

	const QString matches = attrs.value(QStringLiteral("times_matched"));
	const QString calls   = attrs.value(QStringLiteral("invocation_count"));
	if (!matches.isEmpty())
		m_matchesLabel->setText(QStringLiteral("%1 match%2.")
		                            .arg(matches)
		                            .arg(matches == QStringLiteral("1") ? QString() : QStringLiteral("es")));
	if (!calls.isEmpty())
		m_callsLabel->setText(QStringLiteral("%1 call%2.")
		                          .arg(calls)
		                          .arg(calls == QStringLiteral("1") ? QString() : QStringLiteral("s")));

	m_includedLabel->setVisible(isIncluded());
	onSendToChanged(m_sendTo->currentIndex());
}

bool WorldAliasDialog::validateAlias()
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
		QMessageBox::warning(this, QStringLiteral("QMud"), QStringLiteral("The alias cannot be blank."));
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
		    QStringLiteral("The alias 'match' text contains an invalid non-printable character "
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
		    QStringLiteral("The alias 'send' text contains an invalid non-printable character "
		                   "(hex %1) at position %2.")
		        .arg(code, 2, 16, QLatin1Char('0'))
		        .arg(invalidSendPos + 1));
		m_send->setFocus();
		return false;
	}

	if (m_regexp->isChecked())
	{
		QRegularExpression::PatternOptions opts = QRegularExpression::NoPatternOption;
		if (m_ignoreCase->isChecked())
			opts |= QRegularExpression::CaseInsensitiveOption;
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
				    QStringLiteral("The alias label \"%1\" is already in the list of aliases.").arg(label));
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
			    QStringLiteral("The variable name must start with a letter and consist of letters, "
			                   "numbers or the underscore character."));
			m_variable->setFocus();
			return false;
		}
	}

	// check for speed walking OK, unless they are substituting
	if ((!m_expandVariables->isChecked() || !sendText.contains(QLatin1Char('@'))) &&
	    sendToValue() == eSendToSpeedwalk && !sendText.contains(QLatin1Char('%')))
	{
		const QString filler =
		    m_runtime ? m_runtime->worldAttributes().value(QStringLiteral("speed_walk_filler")) : QString();
		if (const QString result = WorldEditUtils::evaluateSpeedwalk(sendText, filler);
		    !result.isEmpty() && result.at(0) == QLatin1Char('*'))
		{
			QMessageBox::warning(this, QStringLiteral("QMud"), result.mid(1));
			m_send->setFocus();
			return false;
		}
	}

	if (sendText.isEmpty() && procedure.isEmpty())
	{
		QMessageBox::warning(
		    this, QStringLiteral("QMud"),
		    QStringLiteral("The alias contents cannot be blank unless you specify a script subroutine."));
		m_send->setFocus();
		return false;
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

	return true;
}

bool WorldAliasDialog::isIncluded() const
{
	if (m_alias.included)
		return true;
	const QString included = m_alias.attributes.value(QStringLiteral("included"));
	return qmudIsEnabledFlag(included);
}

int WorldAliasDialog::sendToValue() const
{
	if (const QVariant data = m_sendTo->currentData(); data.isValid())
		return data.toInt();
	return eSendToWorld;
}
