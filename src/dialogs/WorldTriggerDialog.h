/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldTriggerDialog.h
 * Role: Dialog interfaces for defining trigger rules that match incoming text and invoke actions/scripts.
 */

#ifndef QMUD_WORLDTRIGGERDIALOG_H
#define QMUD_WORLDTRIGGERDIALOG_H

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
 * @brief Editor dialog for creating/updating a world trigger entry.
 */
class WorldTriggerDialog : public QDialog
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates trigger editor initialized from existing trigger data.
		 * @param runtime Runtime context.
		 * @param trigger Trigger value to edit.
		 * @param triggers Existing trigger list for validation.
		 * @param currentIndex Index of trigger being edited, or `-1` for new trigger.
		 * @param parent Optional Qt parent widget.
		 */
			WorldTriggerDialog(WorldRuntime *runtime, WorldRuntime::Trigger trigger,
			                   QList<WorldRuntime::Trigger> triggers, int currentIndex,
			                   QWidget *parent = nullptr);

		/**
		 * @brief Returns edited trigger value.
		 * @return Edited trigger value.
		 */
		[[nodiscard]] WorldRuntime::Trigger trigger() const;

	protected:
		/**
		 * @brief Validates fields and commits edited trigger.
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
		 * @brief Opens file chooser for trigger sound.
		 */
		void onBrowseSound();
		/**
		 * @brief Clears configured trigger sound.
		 */
		void onNoSound() const;
		/**
		 * @brief Plays configured trigger sound for testing.
		 */
		void onTestSound();
		/**
		 * @brief Reacts to send-target mode changes.
		 * @param index Selected send-target index.
		 */
		void onSendToChanged(int index) const;
		/**
		 * @brief Updates control enablement based on trigger options.
		 */
		void updateControls() const;

	private:
		/**
		 * @brief Builds dialog controls.
		 */
		void               buildUi();
		/**
		 * @brief Loads trigger values into controls.
		 */
		void               loadTrigger() const;
		/**
		 * @brief Performs semantic trigger validation.
		 * @return `true` when trigger settings are valid.
		 */
		bool               validateTrigger();
		/**
		 * @brief Reports whether trigger comes from included source.
		 * @return `true` when trigger is from an included file.
		 */
		[[nodiscard]] bool isIncluded() const;
		/**
		 * @brief Returns selected send-target enum value.
		 * @return Selected send-target enum value.
		 */
		[[nodiscard]] int  sendToValue() const;
		/**
		 * @brief Applies tri-state match attribute into trigger map values.
		 * @param box Source tri-state checkbox.
		 * @param matchKey Trigger map key for match-state flag.
		 * @param valueKey Trigger map key for explicit value override.
		 */
		void          applyMatchState(const QCheckBox *box, const QString &matchKey, const QString &valueKey);
		/**
		 * @brief Sets tri-state checkbox from stored trigger map values.
		 * @param box Target tri-state checkbox.
		 * @param matchKey Trigger map key for match-state flag.
		 * @param valueKey Trigger map key for explicit value override.
		 */
		void          setMatchState(QCheckBox *box, const QString &matchKey, const QString &valueKey) const;
		/**
		 * @brief Refreshes preview swatches for trigger colors.
		 */
		void          updateTriggerColourSwatches() const;
		/**
		 * @brief Parses trigger color string to QColor value.
		 * @param value Colour string value.
		 * @return Parsed colour value.
		 */
		static QColor parseColourValue(const QString &value);

		WorldRuntime *m_runtime{nullptr};
		WorldRuntime::Trigger            m_trigger;
		QList<WorldRuntime::Trigger>     m_existing;
		int                              m_currentIndex{-1};

		QLineEdit                       *m_match{nullptr};
		QLineEdit                       *m_label{nullptr};
		QLineEdit                       *m_script{nullptr};
		QLineEdit                       *m_group{nullptr};
		QLineEdit                       *m_variable{nullptr};
		QSpinBox                        *m_sequence{nullptr};
		QComboBox                       *m_sendTo{nullptr};
		QPlainTextEdit                  *m_send{nullptr};
		QComboBox                       *m_wildcardClipboard{nullptr};

		QComboBox                       *m_matchTextColour{nullptr};
		QComboBox                       *m_matchBackColour{nullptr};
		QCheckBox                       *m_matchBold{nullptr};
		QCheckBox                       *m_matchItalic{nullptr};
		QCheckBox                       *m_matchInverse{nullptr};

		QCheckBox                       *m_enabled{nullptr};
		QCheckBox                       *m_ignoreCase{nullptr};
		QCheckBox                       *m_omitFromLog{nullptr};
		QCheckBox                       *m_omitFromOutput{nullptr};
		QCheckBox                       *m_keepEvaluating{nullptr};
		QCheckBox                       *m_regexp{nullptr};
		QCheckBox                       *m_repeat{nullptr};
		QCheckBox                       *m_expandVariables{nullptr};
		QCheckBox                       *m_temporary{nullptr};
		QCheckBox                       *m_multiLine{nullptr};
		QSpinBox                        *m_linesToMatch{nullptr};
		QCheckBox                       *m_lowercaseWildcard{nullptr};
		QCheckBox                       *m_oneShot{nullptr};

		QComboBox                       *m_triggerColour{nullptr};
		QComboBox                       *m_colourChangeType{nullptr};
		QCheckBox                       *m_makeBold{nullptr};
		QCheckBox                       *m_makeItalic{nullptr};
		QCheckBox                       *m_makeUnderline{nullptr};
		QPushButton                     *m_otherTextColour{nullptr};
		QPushButton                     *m_otherBackColour{nullptr};
		QMap<int, QPair<QColor, QColor>> m_customColours;

		QPushButton                     *m_browseSound{nullptr};
		QPushButton                     *m_testSound{nullptr};
		QPushButton                     *m_noSound{nullptr};
		QCheckBox                       *m_soundIfInactive{nullptr};
		QLabel                          *m_soundPathLabel{nullptr};

		QLabel                          *m_includedLabel{nullptr};
		QLabel                          *m_matchesLabel{nullptr};
		QLabel                          *m_callsLabel{nullptr};
		QLabel                          *m_timeTakenLabel{nullptr};
		QLabel                          *m_variableLabel{nullptr};

		QPushButton                     *m_editMatchButton{nullptr};
		QPushButton                     *m_editSendButton{nullptr};
		QPushButton                     *m_convertRegexpButton{nullptr};
		QDialogButtonBox                *m_buttons{nullptr};
};

#endif // QMUD_WORLDTRIGGERDIALOG_H
