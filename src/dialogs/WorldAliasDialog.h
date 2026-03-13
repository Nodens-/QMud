/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldAliasDialog.h
 * Role: Dialog interfaces for creating/editing alias definitions that transform user commands at runtime.
 */

#ifndef QMUD_WORLDALIASDIALOG_H
#define QMUD_WORLDALIASDIALOG_H

#include "WorldRuntime.h"
#include <QDialog>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QPlainTextEdit;
class QSpinBox;

/**
 * @brief Editor dialog for creating/updating a world alias entry.
 */
class WorldAliasDialog : public QDialog
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates alias editor initialized from existing alias data.
		 * @param runtime Runtime context.
		 * @param alias Alias value to edit.
		 * @param aliases Existing alias list for validation.
		 * @param currentIndex Index of alias being edited, or `-1` for new alias.
		 * @param parent Optional Qt parent widget.
		 */
		WorldAliasDialog(WorldRuntime *runtime, WorldRuntime::Alias alias, QList<WorldRuntime::Alias> aliases,
		                 int currentIndex, QWidget *parent = nullptr);

		/**
		 * @brief Returns edited alias value.
		 * @return Edited alias value.
		 */
		[[nodiscard]] WorldRuntime::Alias alias() const;

	protected:
		/**
		 * @brief Validates fields and commits edited alias.
		 */
		void accept() override;

	private slots:
		/**
		 * @brief Opens expression editor for match pattern.
		 */
		void onEditMatch();
		/**
		 * @brief Opens expression editor for send text.
		 */
		void onEditSend();
		/**
		 * @brief Converts wildcard pattern to regex form.
		 */
		void onConvertToRegexp();
		/**
		 * @brief Generates reverse speedwalk text from send field.
		 */
		void onReverseSpeedwalk();
		/**
		 * @brief Reacts to send-target mode changes.
		 * @param index Selected send-target index.
		 */
		void onSendToChanged(int index) const;
		/**
		 * @brief Updates action-button enablement.
		 */
		void updateButtons() const;

	private:
		/**
		 * @brief Builds dialog controls.
		 */
		void                       buildUi();
		/**
		 * @brief Loads alias values into controls.
		 */
		void                       loadAlias() const;
		/**
		 * @brief Performs semantic alias validation.
		 * @return `true` when alias settings are valid.
		 */
		bool                       validateAlias();
		/**
		 * @brief Reports whether alias comes from included source.
		 * @return `true` when alias is from an included file.
		 */
		[[nodiscard]] bool         isIncluded() const;
		/**
		 * @brief Returns selected send-target enum value.
		 * @return Selected send-target enum value.
		 */
		[[nodiscard]] int          sendToValue() const;

		WorldRuntime              *m_runtime{nullptr};
		WorldRuntime::Alias        m_alias;
		QList<WorldRuntime::Alias> m_existing;
		int                        m_currentIndex{-1};

		QLineEdit                 *m_match{nullptr};
		QLineEdit                 *m_label{nullptr};
		QLineEdit                 *m_script{nullptr};
		QLineEdit                 *m_group{nullptr};
		QLineEdit                 *m_variable{nullptr};
		QSpinBox                  *m_sequence{nullptr};
		QComboBox                 *m_sendTo{nullptr};
		QPlainTextEdit            *m_send{nullptr};

		QCheckBox                 *m_enabled{nullptr};
		QCheckBox                 *m_omitFromLog{nullptr};
		QCheckBox                 *m_ignoreCase{nullptr};
		QCheckBox                 *m_regexp{nullptr};
		QCheckBox                 *m_expandVariables{nullptr};
		QCheckBox                 *m_omitFromOutput{nullptr};
		QCheckBox                 *m_temporary{nullptr};
		QCheckBox                 *m_keepEvaluating{nullptr};
		QCheckBox                 *m_echoAlias{nullptr};
		QCheckBox                 *m_omitFromCommandHistory{nullptr};
		QCheckBox                 *m_menu{nullptr};
		QCheckBox                 *m_oneShot{nullptr};

		QLabel                    *m_includedLabel{nullptr};
		QLabel                    *m_matchesLabel{nullptr};
		QLabel                    *m_callsLabel{nullptr};

		QPushButton               *m_editMatchButton{nullptr};
		QPushButton               *m_editSendButton{nullptr};
		QPushButton               *m_convertRegexpButton{nullptr};
		QPushButton               *m_reverseSpeedwalkButton{nullptr};
		QDialogButtonBox          *m_buttons{nullptr};
};

#endif // QMUD_WORLDALIASDIALOG_H
