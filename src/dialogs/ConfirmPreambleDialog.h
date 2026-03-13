/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: ConfirmPreambleDialog.h
 * Role: Dialog interfaces for confirming preamble text/actions before applying scripted imports or generated content.
 */

#ifndef QMUD_CONFIRMPREAMBLEDIALOG_H
#define QMUD_CONFIRMPREAMBLEDIALOG_H

#include <QDialog>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;

/**
 * @brief Confirmation dialog for preamble/postamble and delayed-send options.
 */
class ConfirmPreambleDialog : public QDialog
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates confirmation dialog for preamble/paste options.
		 * @param parent Optional Qt parent widget.
		 */
		explicit ConfirmPreambleDialog(QWidget *parent = nullptr);

		/**
		 * @brief Sets informational message shown in dialog.
		 * @param message Message text.
		 */
		void                  setPasteMessage(const QString &message) const;
		/**
		 * @brief Sets file-level preamble text.
		 * @param text File preamble text.
		 */
		void                  setFilePreamble(const QString &text) const;
		/**
		 * @brief Sets per-line preamble text.
		 * @param text Line preamble text.
		 */
		void                  setLinePreamble(const QString &text) const;
		/**
		 * @brief Sets per-line postamble text.
		 * @param text Line postamble text.
		 */
		void                  setLinePostamble(const QString &text) const;
		/**
		 * @brief Sets file-level postamble text.
		 * @param text File postamble text.
		 */
		void                  setFilePostamble(const QString &text) const;
		/**
		 * @brief Enables/disables commented softcode mode.
		 * @param enabled Enable commented softcode mode when `true`.
		 */
		void                  setCommentedSoftcode(bool enabled) const;
		/**
		 * @brief Sets delay between sent lines in milliseconds.
		 * @param delayMs Delay in milliseconds.
		 */
		void                  setLineDelayMs(int delayMs) const;
		/**
		 * @brief Sets grouping for delayed-send line batching.
		 * @param lines Number of lines per delay batch.
		 */
		void                  setDelayPerLines(int lines) const;
		/**
		 * @brief Enables/disables local echo during send.
		 * @param enabled Enable local echo when `true`.
		 */
		void                  setEcho(bool enabled) const;

		/**
		 * @brief Returns file-level preamble text.
		 * @return File preamble text.
		 */
		[[nodiscard]] QString filePreamble() const;
		/**
		 * @brief Returns per-line preamble text.
		 * @return Line preamble text.
		 */
		[[nodiscard]] QString linePreamble() const;
		/**
		 * @brief Returns per-line postamble text.
		 * @return Line postamble text.
		 */
		[[nodiscard]] QString linePostamble() const;
		/**
		 * @brief Returns file-level postamble text.
		 * @return File postamble text.
		 */
		[[nodiscard]] QString filePostamble() const;
		/**
		 * @brief Returns commented softcode mode flag.
		 * @return `true` when commented softcode mode is enabled.
		 */
		[[nodiscard]] bool    commentedSoftcode() const;
		/**
		 * @brief Returns per-line delay in milliseconds.
		 * @return Delay in milliseconds.
		 */
		[[nodiscard]] int     lineDelayMs() const;
		/**
		 * @brief Returns delayed-send line grouping value.
		 * @return Lines per delay batch.
		 */
		[[nodiscard]] int     delayPerLines() const;
		/**
		 * @brief Returns local echo flag.
		 * @return `true` when local echo is enabled.
		 */
		[[nodiscard]] bool    echo() const;

	private:
		QLabel         *m_message{nullptr};
		QPlainTextEdit *m_filePreamble{nullptr};
		QLineEdit      *m_linePreamble{nullptr};
		QLineEdit      *m_linePostamble{nullptr};
		QPlainTextEdit *m_filePostamble{nullptr};
		QCheckBox      *m_commentedSoftcode{nullptr};
		QCheckBox      *m_echo{nullptr};
		QSpinBox       *m_lineDelay{nullptr};
		QSpinBox       *m_delayPerLines{nullptr};
};

#endif // QMUD_CONFIRMPREAMBLEDIALOG_H
