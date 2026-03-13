/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: LogSessionDialog.h
 * Role: Dialog interfaces for configuring session logging destinations, modes, and start/stop behavior.
 */

#ifndef QMUD_LOGSESSIONDIALOG_H
#define QMUD_LOGSESSIONDIALOG_H

#include <QDialog>

class QCheckBox;
class QSpinBox;
class QTextEdit;

/**
 * @brief Dialog for configuring per-session log/paste options.
 */
class LogSessionDialog : public QDialog
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates session logging options dialog.
		 * @param parent Optional Qt parent widget.
		 */
		explicit LogSessionDialog(QWidget *parent = nullptr);

		/**
		 * @brief Sets number of recent lines to include.
		 * @param lines Number of recent lines.
		 */
		void                  setLines(int lines) const;
		/**
		 * @brief Returns number of recent lines to include.
		 * @return Number of recent lines.
		 */
		[[nodiscard]] int     lines() const;

		/**
		 * @brief Sets append-vs-overwrite log mode.
		 * @param append Append to existing log when `true`.
		 */
		void                  setAppendToLogFile(bool append) const;
		/**
		 * @brief Returns append-vs-overwrite log mode.
		 * @return `true` when append mode is enabled.
		 */
		[[nodiscard]] bool    appendToLogFile() const;

		/**
		 * @brief Sets whether world name should be written in log.
		 * @param write Write world name when `true`.
		 */
		void                  setWriteWorldName(bool write) const;
		/**
		 * @brief Returns world-name-in-log option.
		 * @return `true` when world name logging is enabled.
		 */
		[[nodiscard]] bool    writeWorldName() const;

		/**
		 * @brief Sets log preamble text.
		 * @param text Preamble text.
		 */
		void                  setPreamble(const QString &text) const;
		/**
		 * @brief Returns log preamble text.
		 * @return Preamble text.
		 */
		[[nodiscard]] QString preamble() const;

		/**
		 * @brief Enables/disables output line logging.
		 * @param enabled Enable output logging when `true`.
		 */
		void                  setLogOutput(bool enabled) const;
		/**
		 * @brief Returns output line logging option.
		 * @return `true` when output logging is enabled.
		 */
		[[nodiscard]] bool    logOutput() const;

		/**
		 * @brief Enables/disables input line logging.
		 * @param enabled Enable input logging when `true`.
		 */
		void                  setLogInput(bool enabled) const;
		/**
		 * @brief Returns input line logging option.
		 * @return `true` when input logging is enabled.
		 */
		[[nodiscard]] bool    logInput() const;

		/**
		 * @brief Enables/disables note line logging.
		 * @param enabled Enable note logging when `true`.
		 */
		void                  setLogNotes(bool enabled) const;
		/**
		 * @brief Returns note line logging option.
		 * @return `true` when note logging is enabled.
		 */
		[[nodiscard]] bool    logNotes() const;

	protected:
		/**
		 * @brief Validates and commits logging options.
		 */
		void accept() override;

	private:
		QSpinBox  *m_lines{nullptr};
		QCheckBox *m_append{nullptr};
		QCheckBox *m_writeWorldName{nullptr};
		QTextEdit *m_preamble{nullptr};
		QCheckBox *m_logOutput{nullptr};
		QCheckBox *m_logInput{nullptr};
		QCheckBox *m_logNotes{nullptr};
};

#endif // QMUD_LOGSESSIONDIALOG_H
