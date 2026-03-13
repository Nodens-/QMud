/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WelcomeDialog.h
 * Role: Welcome/onboarding dialog interfaces shown for first-run or initial setup guidance.
 */

#ifndef QMUD_WELCOMEDIALOG_H
#define QMUD_WELCOMEDIALOG_H

#include <QDialog>

/**
 * @brief Initial welcome/info dialog shown to users on startup paths.
 */
class WelcomeDialog : public QDialog
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates welcome dialog with supplied message text.
		 * @param message Welcome message text.
		 * @param parent Optional Qt parent widget.
		 */
		explicit WelcomeDialog(const QString &message, QWidget *parent = nullptr);
};

#endif // QMUD_WELCOMEDIALOG_H
