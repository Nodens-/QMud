/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldTimerDialog.h
 * Role: Dialog interfaces for defining timer objects that schedule automated commands/scripts during a session.
 */

#ifndef QMUD_WORLDTIMERDIALOG_H
#define QMUD_WORLDTIMERDIALOG_H

#include "WorldRuntime.h"
#include <QDialog>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QPlainTextEdit;
class QRadioButton;
class QSpinBox;
class QDoubleSpinBox;

/**
 * @brief Editor dialog for creating/updating a world timer entry.
 */
class WorldTimerDialog : public QDialog
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates timer editor initialized from existing timer data.
		 * @param runtime Runtime context.
		 * @param timer Timer value to edit.
		 * @param timers Existing timer list for validation.
		 * @param currentIndex Index of timer being edited, or `-1` for new timer.
		 * @param parent Optional Qt parent widget.
		 */
		WorldTimerDialog(WorldRuntime *runtime, WorldRuntime::Timer timer, QList<WorldRuntime::Timer> timers,
		                 int currentIndex, QWidget *parent = nullptr);

		/**
		 * @brief Returns edited timer value.
		 * @return Edited timer value.
		 */
		[[nodiscard]] WorldRuntime::Timer timer() const;

	protected:
		/**
		 * @brief Validates fields and commits edited timer.
		 */
		void accept() override;

	private slots:
		/**
		 * @brief Opens expression editor for send text.
		 */
		void onEditSend();
		/**
		 * @brief Reacts to send-target mode changes.
		 * @param index Selected send-target index.
		 */
		void onSendToChanged(int index) const;
		/**
		 * @brief Updates control enablement based on timer mode.
		 */
		void updateControls() const;

	private:
		/**
		 * @brief Builds dialog controls.
		 */
		void                       buildUi();
		/**
		 * @brief Loads timer values into controls.
		 */
		void                       loadTimer() const;
		/**
		 * @brief Performs semantic timer validation.
		 * @return `true` when timer settings are valid.
		 */
		bool                       validateTimer();
		/**
		 * @brief Reports whether timer comes from included source.
		 * @return `true` when timer is from an included file.
		 */
		[[nodiscard]] bool         isIncluded() const;
		/**
		 * @brief Returns selected send-target enum value.
		 * @return Selected send-target enum value.
		 */
		[[nodiscard]] int          sendToValue() const;
		/**
		 * @brief Returns true when timer uses absolute time mode.
		 * @return `true` when absolute-time mode is selected.
		 */
		[[nodiscard]] bool         isAtTime() const;

		WorldRuntime              *m_runtime{nullptr};
		WorldRuntime::Timer        m_timer;
		QList<WorldRuntime::Timer> m_existing;
		int                        m_currentIndex{-1};

		QRadioButton              *m_everyInterval{nullptr};
		QRadioButton              *m_atTime{nullptr};
		QSpinBox                  *m_everyHour{nullptr};
		QSpinBox                  *m_everyMinute{nullptr};
		QDoubleSpinBox            *m_everySecond{nullptr};
		QSpinBox                  *m_offsetHour{nullptr};
		QSpinBox                  *m_offsetMinute{nullptr};
		QDoubleSpinBox            *m_offsetSecond{nullptr};
		QSpinBox                  *m_atHour{nullptr};
		QSpinBox                  *m_atMinute{nullptr};
		QDoubleSpinBox            *m_atSecond{nullptr};

		QPlainTextEdit            *m_send{nullptr};
		QComboBox                 *m_sendTo{nullptr};
		QLineEdit                 *m_label{nullptr};
		QLineEdit                 *m_script{nullptr};
		QLineEdit                 *m_group{nullptr};
		QLineEdit                 *m_variable{nullptr};

		QCheckBox                 *m_enabled{nullptr};
		QCheckBox                 *m_oneShot{nullptr};
		QCheckBox                 *m_temporary{nullptr};
		QCheckBox                 *m_activeWhenClosed{nullptr};
		QCheckBox                 *m_omitFromOutput{nullptr};
		QCheckBox                 *m_omitFromLog{nullptr};

		QLabel                    *m_includedLabel{nullptr};
		QLabel                    *m_matchesLabel{nullptr};
		QLabel                    *m_callsLabel{nullptr};

		QPushButton               *m_editSendButton{nullptr};
		QDialogButtonBox          *m_buttons{nullptr};
};

#endif // QMUD_WORLDTIMERDIALOG_H
