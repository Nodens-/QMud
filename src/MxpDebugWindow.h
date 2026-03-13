/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MxpDebugWindow.h
 * Role: Debug-window interfaces for inspecting MXP parsing/events while troubleshooting protocol or rendering issues.
 */

#ifndef QMUD_MXPDEBUGWINDOW_H
#define QMUD_MXPDEBUGWINDOW_H

#include <QWidget>

class QPlainTextEdit;

/**
 * @brief Utility window that displays MXP diagnostic output.
 */
class MxpDebugWindow : public QWidget
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates the MXP debug output window.
		 * @param parent Optional Qt parent widget.
		 */
		explicit MxpDebugWindow(QWidget *parent = nullptr);
		/**
		 * @brief Updates the window title.
		 * @param title Window title text.
		 */
		void setTitle(const QString &title);
		/**
		 * @brief Appends one diagnostic line to output.
		 * @param message Diagnostic message text.
		 */
		void appendMessage(const QString &message) const;

	protected:
		/**
		 * @brief Handles close requests and emits hide semantics if needed.
		 * @param event Close-event payload.
		 */
		void closeEvent(QCloseEvent *event) override;

	private:
		QPlainTextEdit *m_output{nullptr};
};

#endif // QMUD_MXPDEBUGWINDOW_H
