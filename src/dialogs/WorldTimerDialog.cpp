/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldTimerDialog.cpp
 * Role: Timer editor dialog behavior for configuring schedule parameters and persisting timer automation entries.
 */

#include "dialogs/WorldTimerDialog.h"
#include "StringUtils.h"
#include "WorldOptions.h"
#include "dialogs/WorldEditUtils.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <memory>
#include <utility>

namespace
{
	bool editTextDialog(QWidget *parent, const QString &title, QString &text)
	{
		QDialog dlg(parent);
		dlg.setWindowTitle(title);
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
		auto *buttonsPtr = buttons.get();
		QObject::connect(buttonsPtr, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		QObject::connect(buttonsPtr, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
		layoutPtr->addWidget(buttons.release());
		if (dlg.exec() != QDialog::Accepted)
			return false;
		text = editPtr->toPlainText();
		return true;
	}
} // namespace

WorldTimerDialog::WorldTimerDialog(WorldRuntime *runtime, WorldRuntime::Timer timer,
                                   QList<WorldRuntime::Timer> timers, int currentIndex, QWidget *parent)
    : QDialog(parent), m_runtime(runtime), m_timer(std::move(timer)), m_existing(std::move(timers)),
      m_currentIndex(currentIndex)
{
	setWindowTitle(currentIndex < 0 ? QStringLiteral("Add timer") : QStringLiteral("Edit timer"));
	setMinimumSize(920, 690);
	buildUi();
	loadTimer();
	updateControls();
}

WorldRuntime::Timer WorldTimerDialog::timer() const
{
	return m_timer;
}

void WorldTimerDialog::accept()
{
	if (!validateTimer())
		return;

	QMap<QString, QString> &attrs = m_timer.attributes;
	attrs.insert(QStringLiteral("at_time"), qmudBoolToYn(isAtTime()));
	attrs.insert(QStringLiteral("hour"),
	             QString::number(isAtTime() ? m_atHour->value() : m_everyHour->value()));
	attrs.insert(QStringLiteral("minute"),
	             QString::number(isAtTime() ? m_atMinute->value() : m_everyMinute->value()));
	attrs.insert(QStringLiteral("second"),
	             QString::number(isAtTime() ? m_atSecond->value() : m_everySecond->value(), 'f', 4));
	attrs.insert(QStringLiteral("offset_hour"), QString::number(m_offsetHour->value()));
	attrs.insert(QStringLiteral("offset_minute"), QString::number(m_offsetMinute->value()));
	attrs.insert(QStringLiteral("offset_second"), QString::number(m_offsetSecond->value(), 'f', 4));
	attrs.insert(QStringLiteral("name"), m_label->text());
	attrs.insert(QStringLiteral("script"), m_script->text());
	attrs.insert(QStringLiteral("group"), m_group->text());
	attrs.insert(QStringLiteral("variable"), m_variable->text());
	attrs.insert(QStringLiteral("send_to"), QString::number(sendToValue()));
	attrs.insert(QStringLiteral("enabled"), qmudBoolToYn(m_enabled->isChecked()));
	attrs.insert(QStringLiteral("one_shot"), qmudBoolToYn(m_oneShot->isChecked()));
	attrs.insert(QStringLiteral("temporary"), qmudBoolToYn(m_temporary->isChecked()));
	attrs.insert(QStringLiteral("active_closed"), qmudBoolToYn(m_activeWhenClosed->isChecked()));
	attrs.insert(QStringLiteral("omit_from_output"), qmudBoolToYn(m_omitFromOutput->isChecked()));
	attrs.insert(QStringLiteral("omit_from_log"), qmudBoolToYn(m_omitFromLog->isChecked()));
	m_timer.children.insert(QStringLiteral("send"), m_send->toPlainText());

	QDialog::accept();
}

void WorldTimerDialog::onEditSend()
{
	if (QString text = m_send->toPlainText();
	    editTextDialog(this, QStringLiteral("Edit timer 'send' text"), text))
		m_send->setPlainText(text);
}

void WorldTimerDialog::onSendToChanged(int) const
{
	const bool variableEnabled = sendToValue() == eSendToVariable;
	m_variable->setEnabled(variableEnabled);
	updateControls();
}

void WorldTimerDialog::updateControls() const
{
	const bool included        = isIncluded();
	const bool hasSendOrScript = !m_send->toPlainText().isEmpty() || !m_script->text().isEmpty();
	if (m_buttons)
	{
		if (QAbstractButton *ok = m_buttons->button(QDialogButtonBox::Ok); ok)
			ok->setEnabled(!included && hasSendOrScript);
	}

	const bool at = isAtTime();
	m_everyHour->setEnabled(!at);
	m_everyMinute->setEnabled(!at);
	m_everySecond->setEnabled(!at);
	m_offsetHour->setEnabled(!at);
	m_offsetMinute->setEnabled(!at);
	m_offsetSecond->setEnabled(!at);
	m_atHour->setEnabled(at);
	m_atMinute->setEnabled(at);
	m_atSecond->setEnabled(at);
}

void WorldTimerDialog::buildUi()
{
	auto  mainLayout    = std::make_unique<QVBoxLayout>();
	auto *mainLayoutPtr = mainLayout.get();
	setLayout(mainLayout.release());

	auto  eventGroup     = std::make_unique<QGroupBox>(QStringLiteral("Event"), this);
	auto *eventGroupPtr  = eventGroup.get();
	auto  eventLayout    = std::make_unique<QGridLayout>();
	auto *eventLayoutPtr = eventLayout.get();
	eventGroupPtr->setLayout(eventLayout.release());
	eventLayoutPtr->setContentsMargins(6, 6, 6, 6);
	eventLayoutPtr->setHorizontalSpacing(8);
	eventLayoutPtr->setVerticalSpacing(2);
	m_everyInterval = new QRadioButton(QStringLiteral("&Every interval:"), this);
	m_atTime        = new QRadioButton(QStringLiteral("&At the time:"), this);
	m_everyHour     = new QSpinBox(this);
	m_everyHour->setRange(0, 23);
	m_everyMinute = new QSpinBox(this);
	m_everyMinute->setRange(0, 59);
	m_everySecond = new QDoubleSpinBox(this);
	m_everySecond->setRange(0.0, 59.9999);
	m_everySecond->setDecimals(4);
	m_offsetHour = new QSpinBox(this);
	m_offsetHour->setRange(0, 23);
	m_offsetMinute = new QSpinBox(this);
	m_offsetMinute->setRange(0, 59);
	m_offsetSecond = new QDoubleSpinBox(this);
	m_offsetSecond->setRange(0.0, 59.9999);
	m_offsetSecond->setDecimals(4);
	m_atHour = new QSpinBox(this);
	m_atHour->setRange(0, 23);
	m_atMinute = new QSpinBox(this);
	m_atMinute->setRange(0, 59);
	m_atSecond = new QDoubleSpinBox(this);
	m_atSecond->setRange(0.0, 59.9999);
	m_atSecond->setDecimals(4);

	eventLayoutPtr->addWidget(m_everyInterval, 0, 0);
	eventLayoutPtr->addWidget(m_everyHour, 0, 1);
	eventLayoutPtr->addWidget(new QLabel(QStringLiteral("hour"), this), 0, 2,
	                          Qt::AlignLeft | Qt::AlignVCenter);
	eventLayoutPtr->addWidget(m_everyMinute, 0, 3);
	eventLayoutPtr->addWidget(new QLabel(QStringLiteral("minute"), this), 0, 4,
	                          Qt::AlignLeft | Qt::AlignVCenter);
	eventLayoutPtr->addWidget(m_everySecond, 0, 5);
	eventLayoutPtr->addWidget(new QLabel(QStringLiteral("second"), this), 0, 6,
	                          Qt::AlignLeft | Qt::AlignVCenter);

	eventLayoutPtr->addWidget(new QLabel(QStringLiteral("Offset:"), this), 1, 0);
	eventLayoutPtr->addWidget(m_offsetHour, 1, 1);
	eventLayoutPtr->addWidget(new QLabel(QStringLiteral("hour"), this), 1, 2,
	                          Qt::AlignLeft | Qt::AlignVCenter);
	eventLayoutPtr->addWidget(m_offsetMinute, 1, 3);
	eventLayoutPtr->addWidget(new QLabel(QStringLiteral("minute"), this), 1, 4,
	                          Qt::AlignLeft | Qt::AlignVCenter);
	eventLayoutPtr->addWidget(m_offsetSecond, 1, 5);
	eventLayoutPtr->addWidget(new QLabel(QStringLiteral("second"), this), 1, 6,
	                          Qt::AlignLeft | Qt::AlignVCenter);

	eventLayoutPtr->addWidget(m_atTime, 2, 0);
	eventLayoutPtr->addWidget(m_atHour, 2, 1);
	eventLayoutPtr->addWidget(new QLabel(QStringLiteral("hour"), this), 2, 2,
	                          Qt::AlignLeft | Qt::AlignVCenter);
	eventLayoutPtr->addWidget(m_atMinute, 2, 3);
	eventLayoutPtr->addWidget(new QLabel(QStringLiteral("minute"), this), 2, 4,
	                          Qt::AlignLeft | Qt::AlignVCenter);
	eventLayoutPtr->addWidget(m_atSecond, 2, 5);
	eventLayoutPtr->addWidget(new QLabel(QStringLiteral("second"), this), 2, 6,
	                          Qt::AlignLeft | Qt::AlignVCenter);

	mainLayoutPtr->addWidget(eventGroup.release());

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
	m_enabled              = new QCheckBox(QStringLiteral("Enable&d"), this);
	m_oneShot              = new QCheckBox(QStringLiteral("Only &Fire Once"), this);
	m_temporary            = new QCheckBox(QStringLiteral("&Temporary"), this);
	m_activeWhenClosed     = new QCheckBox(QStringLiteral("Active When Disconnected"), this);
	m_omitFromOutput       = new QCheckBox(QStringLiteral("Omit From Output"), this);
	m_omitFromLog          = new QCheckBox(QStringLiteral("Omit From Log"), this);
	optionsLayoutPtr->addWidget(m_enabled);
	optionsLayoutPtr->addWidget(m_oneShot);
	optionsLayoutPtr->addWidget(m_temporary);
	optionsLayoutPtr->addWidget(m_activeWhenClosed);
	optionsLayoutPtr->addWidget(m_omitFromOutput);
	optionsLayoutPtr->addWidget(m_omitFromLog);
	optionsLayoutPtr->addStretch();
	sendLayoutPtr->addLayout(optionsLayout.release(), 1);
	mainLayoutPtr->addLayout(sendLayout.release());

	auto  bottomLayout    = std::make_unique<QGridLayout>();
	auto *bottomLayoutPtr = bottomLayout.get();
	auto  sendToLabel     = std::make_unique<QLabel>(QStringLiteral("Send To:"), this);
	m_sendTo              = new QComboBox(this);
	WorldEditUtils::populateSendToCombo(m_sendTo);
	sendToLabel->setBuddy(m_sendTo);
	auto labelLabel = std::make_unique<QLabel>(QStringLiteral("La&bel:"), this);
	m_label         = new QLineEdit(this);
	labelLabel->setBuddy(m_label);
	auto scriptLabel = std::make_unique<QLabel>(QStringLiteral("Sc&ript:"), this);
	m_script         = new QLineEdit(this);
	scriptLabel->setBuddy(m_script);
	auto groupLabel = std::make_unique<QLabel>(QStringLiteral("&Group:"), this);
	m_group         = new QLineEdit(this);
	groupLabel->setBuddy(m_group);
	auto variableLabel = std::make_unique<QLabel>(QStringLiteral("Variable:"), this);
	m_variable         = new QLineEdit(this);
	variableLabel->setBuddy(m_variable);

	bottomLayoutPtr->addWidget(sendToLabel.release(), 0, 0);
	bottomLayoutPtr->addWidget(m_sendTo, 0, 1);
	bottomLayoutPtr->addWidget(labelLabel.release(), 1, 0);
	bottomLayoutPtr->addWidget(m_label, 1, 1);
	bottomLayoutPtr->addWidget(scriptLabel.release(), 2, 0);
	bottomLayoutPtr->addWidget(m_script, 2, 1);
	bottomLayoutPtr->addWidget(groupLabel.release(), 3, 0);
	bottomLayoutPtr->addWidget(m_group, 3, 1);
	bottomLayoutPtr->addWidget(variableLabel.release(), 4, 0);
	bottomLayoutPtr->addWidget(m_variable, 4, 1);

	m_includedLabel = new QLabel(QStringLiteral("(included)"), this);
	m_matchesLabel  = new QLabel(QStringLiteral("Fired 0 times."), this);
	m_callsLabel    = new QLabel(QStringLiteral("0 calls."), this);
	bottomLayoutPtr->addWidget(m_includedLabel, 0, 2, 1, 1, Qt::AlignRight);
	bottomLayoutPtr->addWidget(m_matchesLabel, 1, 2, 1, 1, Qt::AlignRight);
	bottomLayoutPtr->addWidget(m_callsLabel, 2, 2, 1, 1, Qt::AlignRight);

	mainLayoutPtr->addLayout(bottomLayout.release());

	m_buttons =
	    new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Help, this);
	connect(m_buttons, &QDialogButtonBox::accepted, this, &WorldTimerDialog::accept);
	connect(m_buttons, &QDialogButtonBox::rejected, this, &WorldTimerDialog::reject);
	connect(m_buttons->button(QDialogButtonBox::Help), &QPushButton::clicked, this,
	        [this]
	        {
		        QMessageBox::information(this, QStringLiteral("Help"),
		                                 QStringLiteral("Help is not available for this dialog."));
	        });
	mainLayoutPtr->addWidget(m_buttons);

	connect(m_editSendButton, &QPushButton::clicked, this, &WorldTimerDialog::onEditSend);
	connect(m_sendTo, qOverload<int>(&QComboBox::currentIndexChanged), this,
	        &WorldTimerDialog::onSendToChanged);
	connect(m_everyInterval, &QRadioButton::toggled, this, &WorldTimerDialog::updateControls);
	connect(m_atTime, &QRadioButton::toggled, this, &WorldTimerDialog::updateControls);
	connect(m_send, &QPlainTextEdit::textChanged, this, &WorldTimerDialog::updateControls);
	connect(m_script, &QLineEdit::textChanged, this, &WorldTimerDialog::updateControls);
}

void WorldTimerDialog::loadTimer() const
{
	const QMap<QString, QString> &attrs  = m_timer.attributes;
	const bool                    atTime = qmudIsEnabledFlag(attrs.value(QStringLiteral("at_time")));
	m_atTime->setChecked(atTime);
	m_everyInterval->setChecked(!atTime);
	m_everyHour->setValue(attrs.value(QStringLiteral("hour")).toInt());
	m_everyMinute->setValue(attrs.value(QStringLiteral("minute")).toInt());
	m_everySecond->setValue(attrs.value(QStringLiteral("second")).toDouble());
	m_offsetHour->setValue(attrs.value(QStringLiteral("offset_hour")).toInt());
	m_offsetMinute->setValue(attrs.value(QStringLiteral("offset_minute")).toInt());
	m_offsetSecond->setValue(attrs.value(QStringLiteral("offset_second")).toDouble());
	m_atHour->setValue(attrs.value(QStringLiteral("hour")).toInt());
	m_atMinute->setValue(attrs.value(QStringLiteral("minute")).toInt());
	m_atSecond->setValue(attrs.value(QStringLiteral("second")).toDouble());

	m_label->setText(attrs.value(QStringLiteral("name")));
	m_script->setText(attrs.value(QStringLiteral("script")));
	m_group->setText(attrs.value(QStringLiteral("group")));
	m_variable->setText(attrs.value(QStringLiteral("variable")));
	m_send->setPlainText(m_timer.children.value(QStringLiteral("send")));

	m_enabled->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("enabled"))));
	m_oneShot->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("one_shot"))));
	m_temporary->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("temporary"))));
	m_activeWhenClosed->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("active_closed"))));
	m_omitFromOutput->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("omit_from_output"))));
	m_omitFromLog->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("omit_from_log"))));

	const int sendTo = attrs.value(QStringLiteral("send_to")).toInt();
	if (const int sendToIndex = m_sendTo->findData(sendTo); sendToIndex >= 0)
		m_sendTo->setCurrentIndex(sendToIndex);

	const QString matches = attrs.value(QStringLiteral("times_fired"));
	const QString calls   = attrs.value(QStringLiteral("invocation_count"));
	if (!matches.isEmpty())
		m_matchesLabel->setText(QStringLiteral("Fired %1 time%2.")
		                            .arg(matches)
		                            .arg(matches == QStringLiteral("1") ? QString() : QStringLiteral("s")));
	if (!calls.isEmpty())
		m_callsLabel->setText(QStringLiteral("%1 call%2.")
		                          .arg(calls)
		                          .arg(calls == QStringLiteral("1") ? QString() : QStringLiteral("s")));

	m_includedLabel->setVisible(isIncluded());
	onSendToChanged(m_sendTo->currentIndex());
}

bool WorldTimerDialog::validateTimer()
{
	const QString label     = m_label->text().trimmed();
	const QString procedure = m_script->text().trimmed();
	const QString group     = m_group->text().trimmed();
	const QString variable  = m_variable->text().trimmed();
	const QString contents  = m_send->toPlainText();

	m_label->setText(label);
	m_script->setText(procedure);
	m_group->setText(group);
	m_variable->setText(variable);

	if (!isAtTime())
	{
		const double interval =
		    m_everyHour->value() * 3600.0 + m_everyMinute->value() * 60.0 + m_everySecond->value();
		const double offset =
		    m_offsetHour->value() * 3600.0 + m_offsetMinute->value() * 60.0 + m_offsetSecond->value();
		if (interval <= 0.0)
		{
			QMessageBox::warning(this, QStringLiteral("QMud"),
			                     QStringLiteral("The timer interval must be greater than zero."));
			m_everyHour->setFocus();
			return false;
		}
		if (offset >= interval)
		{
			QMessageBox::warning(this, QStringLiteral("QMud"),
			                     QStringLiteral("The timer offset must be less than the timer period."));
			m_offsetHour->setFocus();
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
				    QStringLiteral("The timer label \"%1\" is already in the list of timers.").arg(label));
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

	if (sendToValue() == eSendToSpeedwalk)
	{
		const QString filler =
		    m_runtime ? m_runtime->worldAttributes().value(QStringLiteral("speed_walk_filler")) : QString();
		if (const QString result = WorldEditUtils::evaluateSpeedwalk(contents, filler);
		    !result.isEmpty() && result.at(0) == QLatin1Char('*'))
		{
			QMessageBox::warning(this, QStringLiteral("QMud"), result.mid(1));
			m_send->setFocus();
			return false;
		}
	}

	if (contents.isEmpty() && procedure.isEmpty())
	{
		QMessageBox::warning(
		    this, QStringLiteral("QMud"),
		    QStringLiteral("The timer contents cannot be blank unless you specify a script subroutine."));
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

bool WorldTimerDialog::isIncluded() const
{
	if (m_timer.included)
		return true;
	const QString included = m_timer.attributes.value(QStringLiteral("included"));
	return qmudIsEnabledFlag(included);
}

int WorldTimerDialog::sendToValue() const
{
	if (const QVariant data = m_sendTo->currentData(); data.isValid())
		return data.toInt();
	return eSendToWorld;
}

bool WorldTimerDialog::isAtTime() const
{
	return m_atTime->isChecked();
}
