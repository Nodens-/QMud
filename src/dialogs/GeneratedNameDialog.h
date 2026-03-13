/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: GeneratedNameDialog.h
 * Role: Dialog interfaces for presenting generated-name suggestions and returning user selections to calling workflows.
 */

#ifndef QMUD_GENERATED_NAME_DIALOG_H
#define QMUD_GENERATED_NAME_DIALOG_H

#include <QDialog>

class WorldRuntime;
class QLineEdit;

/**
 * @brief Dialog that generates and previews random character/world names.
 */
class GeneratedNameDialog : public QDialog
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates generated-name dialog bound to a runtime context.
		 * @param runtime Runtime context used for send actions.
		 * @param parent Optional Qt parent widget.
		 */
		explicit GeneratedNameDialog(WorldRuntime *runtime, QWidget *parent = nullptr);

	private slots:
		/**
		 * @brief Generates a new candidate name.
		 */
		void onTryAgain() const;
		/**
		 * @brief Copies generated name to clipboard.
		 */
		void onCopy() const;
		/**
		 * @brief Sends generated name to world command input/output.
		 */
		void onSendToWorld() const;
		/**
		 * @brief Opens name source browser or file chooser.
		 */
		void onBrowseName();

	private:
		/**
		 * @brief Updates derived filename from current generated name.
		 */
		void          updateFileName() const;
		/**
		 * @brief Writes generated name to UI controls.
		 * @param name Generated name text.
		 */
		void          setGeneratedName(const QString &name) const;

		WorldRuntime *m_runtime{nullptr};
		QLineEdit    *m_nameEdit{nullptr};
		QLineEdit    *m_fileEdit{nullptr};
};

#endif // QMUD_GENERATED_NAME_DIALOG_H
