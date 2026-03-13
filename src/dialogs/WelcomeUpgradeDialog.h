/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WelcomeUpgradeDialog.h
 * Role: Upgrade welcome dialog interfaces used to communicate post-upgrade notes and migration guidance.
 */

#ifndef QMUD_WELCOMEUPGRADEDIALOG_H
#define QMUD_WELCOMEUPGRADEDIALOG_H

#include <QDialog>

/**
 * @brief Upgrade-notes dialog shown after version transitions.
 */
class WelcomeUpgradeDialog : public QDialog
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates upgrade-welcome dialog with primary/secondary messages.
		 * @param message1 Primary upgrade message.
		 * @param message2 Secondary upgrade message.
		 * @param parent Optional Qt parent widget.
		 */
		WelcomeUpgradeDialog(const QString &message1, const QString &message2, QWidget *parent = nullptr);
};

#endif // QMUD_WELCOMEUPGRADEDIALOG_H
