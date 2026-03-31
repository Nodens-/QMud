/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldPreferencesDialog.h
 * Role: Dialog interfaces for per-world preferences spanning display, networking, scripting, logging, and behavior
 * settings.
 */

#ifndef QMUD_WORLD_PREFERENCES_DIALOG_H
#define QMUD_WORLD_PREFERENCES_DIALOG_H

#include <QDialog>
#include <QFont>
#include <QHash>
#include <QMap>
#include <QVector>

class QTreeWidget;
class QTreeWidgetItem;
class QStackedWidget;
class QLineEdit;
class QSpinBox;
class QCheckBox;
class QFrame;
class QComboBox;
class QTextEdit;
class QTableWidget;
class QLabel;
class QPushButton;

class WorldRuntime;
class WorldView;

/**
 * @brief Tabbed per-world preferences editor dialog.
 *
 * Maps UI pages to world attributes and persists validated option changes.
 */
class WorldPreferencesDialog : public QDialog
{
		Q_OBJECT
	public:
		enum Page
		{
			PageGeneral = 0,
			PageSound,
			PageCustomColours,
			PageLogging,
			PageAnsiColours,
			PageMacros,
			PageAliases,
			PageTriggers,
			PageCommands,
			PageSendToWorld,
			PageNotes,
			PageKeypad,
			PagePaste,
			PageOutput,
			PageInfo,
			PageTimers,
			PageScripting,
			PageVariables,
			PageAutoSay,
			PagePrinting,
			PageConnecting,
			PageMxp,
			PageChat
		};

		/**
		 * @brief Creates per-world preferences dialog bound to runtime/view.
		 * @param runtime Runtime whose world attributes are edited by this dialog.
		 * @param view World view used for preview-dependent controls.
		 * @param parent Optional Qt parent widget.
		 */
		explicit WorldPreferencesDialog(WorldRuntime *runtime, WorldView *view, QWidget *parent = nullptr);

		/**
		 * @brief Selects initially visible preferences page.
		 * @param page Page enum value to activate.
		 */
		void setInitialPage(Page page);
		/**
		 * @brief Validates and applies all edited world preferences.
		 */
		void accept() override;

	protected:
		/**
		 * @brief Handles dialog-level event filtering hooks.
		 * @param obj Object that received the event.
		 * @param event Event instance to inspect/handle.
		 * @return `true` when the event is consumed by the dialog.
		 */
		bool eventFilter(QObject *obj, QEvent *event) override;

	private:
		/**
		 * @brief UI construction and import/export helpers.
		 */
		void                          buildUi();
		/**
		 * @brief Applies pending inline list/table edits.
		 */
		void                          applyListEdits();
		/**
		 * @brief Opens color editor for table cell.
		 * @param table Target table containing the editable color cell.
		 * @param row Row index of the color cell.
		 * @param column Column index of the color cell.
		 */
		void                          editColourCell(QTableWidget *table, int row, int column);
		/**
		 * @brief Opens macro editor for the selected row.
		 * @param row Macro row index to edit.
		 */
		void                          editMacroAtRow(int row);
		/**
		 * @brief Finds macro rows matching text criteria.
		 * @param text Search text or regex pattern.
		 * @param continueFromCurrent Continue from current selection when `true`.
		 */
		void                          findMacro(const QString &text, bool continueFromCurrent);
		/**
		 * @brief Saves macro list to XML/file.
		 * @param fileName Target file path.
		 * @return `true` on successful write.
		 */
		[[nodiscard]] bool            saveMacrosToFile(const QString &fileName) const;
		/**
		 * @brief Loads macro list from XML/file.
		 * @param fileName Source file path.
		 * @return `true` on successful load and parse.
		 */
		bool                          loadMacrosFromFile(const QString &fileName);
		/**
		 * @brief Saves trigger list to XML/file.
		 * @param fileName Target file path.
		 * @return `true` on successful write.
		 */
		[[nodiscard]] bool            saveTriggersToFile(const QString &fileName) const;
		/**
		 * @brief Loads triggers from XML/file.
		 * @param fileName Source file path.
		 * @param replace Replace existing triggers when `true`.
		 * @return `true` on successful load and merge/replace.
		 */
		bool                          loadTriggersFromFile(const QString &fileName, bool replace);
		/**
		 * @brief Saves alias list to XML/file.
		 * @param fileName Target file path.
		 * @return `true` on successful write.
		 */
		[[nodiscard]] bool            saveAliasesToFile(const QString &fileName) const;
		/**
		 * @brief Loads aliases from XML/file.
		 * @param fileName Source file path.
		 * @param replace Replace existing aliases when `true`.
		 * @return `true` on successful load and merge/replace.
		 */
		bool                          loadAliasesFromFile(const QString &fileName, bool replace);
		/**
		 * @brief Saves timer list to XML/file.
		 * @param fileName Target file path.
		 * @return `true` on successful write.
		 */
		[[nodiscard]] bool            saveTimersToFile(const QString &fileName) const;
		/**
		 * @brief Loads timers from XML/file.
		 * @param fileName Source file path.
		 * @param replace Replace existing timers when `true`.
		 * @return `true` on successful load and merge/replace.
		 */
		bool                          loadTimersFromFile(const QString &fileName, bool replace);
		/**
		 * @brief Saves variables to XML/file.
		 * @param fileName Target file path.
		 * @return `true` on successful write.
		 */
		[[nodiscard]] bool            saveVariablesToFile(const QString &fileName) const;
		/**
		 * @brief Loads variables from XML/file.
		 * @param fileName Source file path.
		 * @return `true` on successful load.
		 */
		bool                          loadVariablesFromFile(const QString &fileName);
		/**
		 * @brief Page population helpers.
		 */
		void                          populateGeneral();
		/**
		 * @brief Populates sound page controls.
		 */
		void                          populateSound() const;
		/**
		 * @brief Populates custom color page controls.
		 */
		void                          populateCustomColours();
		/**
		 * @brief Populates logging page controls.
		 */
		void                          populateLogging() const;
		/**
		 * @brief Populates ANSI color page controls.
		 */
		void                          populateAnsiColours();
		/**
		 * @brief Populates macro list page controls.
		 */
		void                          populateMacros();
		/**
		 * @brief Populates output page controls.
		 */
		void                          populateOutput();
		/**
		 * @brief Populates command page controls.
		 */
		void                          populateCommands();
		/**
		 * @brief Populates scripting page controls.
		 */
		void                          populateScripting() const;
		/**
		 * @brief Populates send-to-world page controls.
		 */
		void                          populateSendToWorld() const;
		/**
		 * @brief Populates notes page controls.
		 */
		void                          populateNotes() const;
		/**
		 * @brief Populates keypad page controls.
		 */
		void                          populateKeypad();
		/**
		 * @brief Populates paste page controls.
		 */
		void                          populatePaste() const;
		/**
		 * @brief Populates info/statistics page controls.
		 */
		void                          populateInfo();
		/**
		 * @brief Populates trigger list page controls.
		 */
		void                          populateTriggers();
		/**
		 * @brief Populates alias list page controls.
		 */
		void                          populateAliases();
		/**
		 * @brief Populates timer list page controls.
		 */
		void                          populateTimers();
		/**
		 * @brief Populates variable list page controls.
		 */
		void                          populateVariables();
		/**
		 * @brief Populates auto-say page controls.
		 */
		void                          populateAutoSay() const;
		/**
		 * @brief Populates printing page controls.
		 */
		void                          populatePrinting();
		/**
		 * @brief Populates connecting page controls.
		 */
		void                          populateConnecting() const;
		/**
		 * @brief Populates MXP page controls.
		 */
		void                          populateMxp() const;
		/**
		 * @brief Populates chat page controls.
		 */
		void                          populateChat() const;
		/**
		 * @brief Derived-state and control-update helpers.
		 */
		void                          updateScriptNoteSwatches() const;
		/**
		 * @brief Recalculates memory usage estimates for scripts/rules.
		 * @param allowProgress Show progress UI for expensive runs when `true`.
		 */
		void                          calculateMemoryUsage(bool allowProgress);
		/**
		 * @brief Updates enable state of clear-cached-data button.
		 */
		void                          updateClearCachedButton() const;
		/**
		 * @brief Finds text within notes editor.
		 * @param again Continue search from current position when `true`.
		 */
		void                          doNotesFind(bool again);
		/**
		 * @brief Applies swatch color style to line-edit field.
		 * @param edit Target line edit showing a color value.
		 * @param colour Color to apply.
		 */
		void                          setLineEditSwatch(QLineEdit *edit, const QColor &colour) const;
		/**
		 * @brief Opens color picker for line-edit swatch field.
		 * @param edit Target line edit whose color value will be updated.
		 * @param title Dialog title text.
		 */
		void                          openLineEditColourPicker(QLineEdit *edit, const QString &title);
		/**
		 * @brief Updates auto-copy-html option dependencies.
		 */
		void                          updateAutoCopyHtmlState() const;
		/**
		 * @brief Updates output font controls.
		 */
		void                          updateOutputFontControls() const;
		/**
		 * @brief Updates default-colours option dependencies.
		 */
		void                          updateDefaultColoursState();
		/**
		 * @brief Updates input font controls.
		 */
		void                          updateInputFontControls() const;
		/**
		 * @brief Updates command auto-resize controls.
		 */
		void                          updateCommandAutoResizeControls() const;
		/**
		 * @brief Updates macro page button states.
		 */
		void                          updateMacroControls() const;
		/**
		 * @brief Updates alias page button states.
		 */
		void                          updateAliasControls() const;
		/**
		 * @brief Updates trigger page button states.
		 */
		void                          updateTriggerControls() const;
		/**
		 * @brief Updates timer page button states.
		 */
		void                          updateTimerControls() const;
		/**
		 * @brief Updates variable page button states.
		 */
		void                          updateVariableControls() const;
		/**
		 * @brief Switches between table/tree rule views.
		 */
		void                          updateRuleViewModes();
		/**
		 * @brief Updates spell-check UI enable state.
		 */
		void                          updateSpellCheckState() const;
		/**
		 * @brief Default-resource availability and font helpers.
		 * @return `true` when the default output font global option is available.
		 */
		static bool                   hasDefaultOutputFont();
		/**
		 * @brief Returns true when default colors file exists.
		 * @return `true` when default colors resource file is present.
		 */
		static bool                   hasDefaultColoursFile();
		/**
		 * @brief Returns true when default input font exists.
		 * @return `true` when default input font global option is available.
		 */
		static bool                   hasDefaultInputFont();
		/**
		 * @brief Returns true when default macros file exists.
		 * @return `true` when default macros resource file is present.
		 */
		static bool                   hasDefaultMacrosFile();
		/**
		 * @brief Returns true when default triggers file exists.
		 * @return `true` when default triggers resource file is present.
		 */
		static bool                   hasDefaultTriggersFile();
		/**
		 * @brief Returns true when default timers file exists.
		 * @return `true` when default timers resource file is present.
		 */
		static bool                   hasDefaultTimersFile();
		/**
		 * @brief Returns true when default aliases file exists.
		 * @return `true` when default aliases resource file is present.
		 */
		static bool                   hasDefaultAliasesFile();
		/**
		 * @brief Builds output font from dialog controls.
		 * @return Font built from current output-font widgets.
		 */
		[[nodiscard]] QFont           outputFontFromDialog() const;
		/**
		 * @brief Returns current output font from world view.
		 * @return Effective output font currently used by the view.
		 */
		[[nodiscard]] QFont           outputFontFromView() const;
		/**
		 * @brief Computes average character width for a font.
		 * @param font Font to measure.
		 * @return Average character width in pixels.
		 */
		static int                    averageCharWidth(const QFont &font);
		/**
		 * @brief Keypad view-mode data marshaling helpers.
		 * @param ctrlView Use Ctrl-modifier keypad view when `true`, base view otherwise.
		 */
		void                          storeKeypadFields(bool ctrlView);
		/**
		 * @brief Loads keypad controls from selected view-mode buffer.
		 * @param ctrlView Use Ctrl-modifier keypad view when `true`, base view otherwise.
		 */
		void                          loadKeypadFields(bool ctrlView);

		WorldRuntime                 *m_runtime{nullptr};
		WorldView                    *m_view{nullptr};
		QTreeWidget                  *m_pageTree{nullptr};
		QStackedWidget               *m_pages{nullptr};
		QHash<int, QTreeWidgetItem *> m_pageItems;

		// General
		QLineEdit                    *m_worldName{nullptr};
		QLineEdit                    *m_host{nullptr};
		QSpinBox                     *m_port{nullptr};
		QCheckBox                    *m_saveWorldAutomatically{nullptr};
		QSpinBox                     *m_autosaveMinutes{nullptr};
		QComboBox                    *m_proxyType{nullptr};
		QLineEdit                    *m_proxyServer{nullptr};
		QSpinBox                     *m_proxyPort{nullptr};
		QPushButton                  *m_proxyAuthButton{nullptr};
		QLabel                       *m_bugReportLink{nullptr};
		QPushButton                  *m_clearCachedButton{nullptr};
		QString                       m_proxyUsername;
		QString                       m_proxyPassword;

		// Sound
		QLineEdit                    *m_beepSound{nullptr};
		QPushButton                  *m_browseBeepSound{nullptr};
		QPushButton                  *m_testBeepSound{nullptr};
		QLineEdit                    *m_newActivitySound{nullptr};
		QPushButton                  *m_browseActivitySound{nullptr};
		QPushButton                  *m_testActivitySound{nullptr};
		QPushButton                  *m_noActivitySound{nullptr};
		QCheckBox                    *m_playSoundsInBackground{nullptr};

		// Custom colors / ANSI colors
		QVector<QLineEdit *>          m_customColourNames;
		QVector<QPushButton *>        m_customTextSwatches;
		QVector<QPushButton *>        m_customBackSwatches;
		QVector<QPushButton *>        m_ansiNormalSwatches;
		QVector<QPushButton *>        m_ansiBoldSwatches;
		QCheckBox                    *m_useDefaultColours{nullptr};
		QCheckBox                    *m_custom16IsDefaultColour{nullptr};
		QVector<QFrame *>             m_customAnsiNormal;
		QVector<QFrame *>             m_customAnsiBold;
		QPushButton                  *m_customInvert{nullptr};
		QPushButton                  *m_customLighter{nullptr};
		QPushButton                  *m_customDarker{nullptr};
		QPushButton                  *m_customMoreColour{nullptr};
		QPushButton                  *m_customLessColour{nullptr};
		QPushButton                  *m_customDefaults{nullptr};
		QPushButton                  *m_customSwap{nullptr};
		QPushButton                  *m_customRandom{nullptr};
		QPushButton                  *m_ansiDefaults{nullptr};
		QPushButton                  *m_ansiSwap{nullptr};
		QPushButton                  *m_ansiInvert{nullptr};
		QPushButton                  *m_ansiLighter{nullptr};
		QPushButton                  *m_ansiDarker{nullptr};
		QPushButton                  *m_ansiMoreColour{nullptr};
		QPushButton                  *m_ansiLessColour{nullptr};
		QPushButton                  *m_ansiRandom{nullptr};
		QPushButton                  *m_ansiLoad{nullptr};
		QPushButton                  *m_ansiSave{nullptr};
		QPushButton                  *m_copyAnsiToCustom{nullptr};

		// Logging
		QCheckBox                    *m_logOutput{nullptr};
		QCheckBox                    *m_logInput{nullptr};
		QCheckBox                    *m_logNotes{nullptr};
		QCheckBox                    *m_logHtml{nullptr};
		QCheckBox                    *m_logRaw{nullptr};
		QCheckBox                    *m_logInColour{nullptr};
		QCheckBox                    *m_writeWorldNameToLog{nullptr};
		QSpinBox                     *m_logRotateMb{nullptr};
		QCheckBox                    *m_logRotateGzip{nullptr};
		QLineEdit                    *m_autoLogFileName{nullptr};
		QPushButton                  *m_browseLogFile{nullptr};
		QTextEdit                    *m_logFilePreamble{nullptr};
		QTextEdit                    *m_logFilePostamble{nullptr};
		QPushButton                  *m_standardPreamble{nullptr};
		QPushButton                  *m_editPreamble{nullptr};
		QPushButton                  *m_editPostamble{nullptr};
		QPushButton                  *m_substitutionHelp{nullptr};
		QLineEdit                    *m_logLinePreambleOutput{nullptr};
		QLineEdit                    *m_logLinePreambleInput{nullptr};
		QLineEdit                    *m_logLinePreambleNotes{nullptr};
		QLineEdit                    *m_logLinePostambleOutput{nullptr};
		QLineEdit                    *m_logLinePostambleInput{nullptr};
		QLineEdit                    *m_logLinePostambleNotes{nullptr};

		// Macros
		QTableWidget                 *m_macrosTable{nullptr};
		QCheckBox                    *m_useDefaultMacros{nullptr};
		QPushButton                  *m_editMacroButton{nullptr};
		QPushButton                  *m_findMacroButton{nullptr};
		QPushButton                  *m_findNextMacroButton{nullptr};
		QPushButton                  *m_loadMacroButton{nullptr};
		QPushButton                  *m_saveMacroButton{nullptr};
		QString                       m_macroFindText;
		int                           m_macroFindRow{-1};
		QStringList                   m_macroFindHistory;
		bool                          m_macroFindMatchCase{false};
		bool                          m_macroFindForwards{false};
		bool                          m_macroFindRegex{false};
		int                           m_macroSortColumn{0};
		bool                          m_macroSortAscending{true};
		bool                          m_initialUseDefaultMacros{false};

		// Output
		QSpinBox                     *m_wrapColumn{nullptr};
		QSpinBox                     *m_maxLines{nullptr};
		QCheckBox                    *m_wrapOutput{nullptr};
		QCheckBox                    *m_autoWrapWindow{nullptr};
		QCheckBox                    *m_lineInformation{nullptr};
		QCheckBox                    *m_startPaused{nullptr};
		QCheckBox                    *m_autoPause{nullptr};
		QCheckBox                    *m_unpauseOnSend{nullptr};
		QCheckBox                    *m_keepPauseAtBottomOption{nullptr};
		QCheckBox                    *m_doNotShowOutstandingLines{nullptr};
		QCheckBox                    *m_indentParas{nullptr};
		QCheckBox                    *m_alternativeInverse{nullptr};
		QCheckBox                    *m_enableBeeps{nullptr};
		QCheckBox                    *m_disableCompression{nullptr};
		QCheckBox                    *m_flashIcon{nullptr};
		QCheckBox                    *m_showBold{nullptr};
		QCheckBox                    *m_showItalic{nullptr};
		QCheckBox                    *m_showUnderline{nullptr};
		QCheckBox                    *m_useDefaultOutputFont{nullptr};
		QPushButton                  *m_outputFontButton{nullptr};
		QLineEdit                    *m_outputFontName{nullptr};
		QSpinBox                     *m_outputFontHeight{nullptr};
		int                           m_outputFontWeight{0};
		int                           m_outputFontCharset{0};
		QSpinBox                     *m_lineSpacing{nullptr};
		QSpinBox                     *m_pixelOffset{nullptr};
		QCheckBox                    *m_naws{nullptr};
		QLineEdit                    *m_terminalIdentification{nullptr};
		QCheckBox                    *m_showConnectDisconnect{nullptr};
		QCheckBox                    *m_copySelectionToClipboard{nullptr};
		QCheckBox                    *m_autoCopyHtml{nullptr};
		QCheckBox                    *m_utf8{nullptr};
		QCheckBox                    *m_carriageReturnClearsLine{nullptr};
		QCheckBox                    *m_convertGaToNewline{nullptr};
		QCheckBox                    *m_persistOutputBuffer{nullptr};
		QSpinBox                     *m_fadeOutputBufferAfterSeconds{nullptr};
		QSpinBox                     *m_fadeOutputOpacityPercent{nullptr};
		QSpinBox                     *m_fadeOutputSeconds{nullptr};
		QSpinBox                     *m_toolTipVisibleTime{nullptr};
		QSpinBox                     *m_toolTipStartTime{nullptr};
		bool                          m_initialUseDefaultColours{false};
		QMap<QString, QString>        m_keypadValues;

		// Commands
		QCheckBox                    *m_arrowsChangeHistory{nullptr};
		QCheckBox                    *m_arrowKeysWrap{nullptr};
		QCheckBox                    *m_arrowRecallsPartial{nullptr};
		QCheckBox                    *m_altArrowRecallsPartial{nullptr};
		QCheckBox                    *m_keepCommandsOnSameLine{nullptr};
		QCheckBox                    *m_confirmBeforeReplacingTyping{nullptr};
		QCheckBox                    *m_escapeDeletesInput{nullptr};
		QCheckBox                    *m_doubleClickInserts{nullptr};
		QCheckBox                    *m_doubleClickSends{nullptr};
		QCheckBox                    *m_saveDeletedCommand{nullptr};
		QCheckBox                    *m_ctrlZToEnd{nullptr};
		QCheckBox                    *m_ctrlPToPrev{nullptr};
		QCheckBox                    *m_ctrlNToNext{nullptr};
		QCheckBox                    *m_ctrlBackspaceDeletesLastWord{nullptr};
		QCheckBox                    *m_enableCommandStack{nullptr};
		QLineEdit                    *m_commandStackCharacter{nullptr};
		QCheckBox                    *m_enableSpeedWalk{nullptr};
		QLineEdit                    *m_speedWalkPrefix{nullptr};
		QLineEdit                    *m_speedWalkFiller{nullptr};
		QSpinBox                     *m_speedWalkDelay{nullptr};
		QCheckBox                    *m_displayMyInput{nullptr};
		QSpinBox                     *m_historyLines{nullptr};
		QCheckBox                    *m_persistCommandHistory{nullptr};
		QCheckBox                    *m_alwaysRecordCommandHistory{nullptr};
		QCheckBox                    *m_doNotAddMacrosToCommandHistory{nullptr};
		QCheckBox                    *m_autoResizeCommandWindow{nullptr};
		QSpinBox                     *m_autoResizeMinimumLines{nullptr};
		QSpinBox                     *m_autoResizeMaximumLines{nullptr};
		QCheckBox                    *m_autoRepeat{nullptr};
		QCheckBox                    *m_translateGerman{nullptr};
		QCheckBox                    *m_spellCheckOnSend{nullptr};
		QCheckBox                    *m_lowerCaseTabCompletion{nullptr};
		QCheckBox                    *m_translateBackslash{nullptr};
		QTextEdit                    *m_tabCompletionDefaults{nullptr};
		QSpinBox                     *m_tabCompletionLines{nullptr};
		QCheckBox                    *m_tabCompletionSpace{nullptr};
		QCheckBox                    *m_useDefaultInputFont{nullptr};
		QPushButton                  *m_inputFontButton{nullptr};
		QLineEdit                    *m_inputFontName{nullptr};
		QSpinBox                     *m_inputFontHeight{nullptr};
		QLabel                       *m_inputFontStyle{nullptr};
		int                           m_inputFontWeight{0};
		bool                          m_inputFontItalic{false};
		int                           m_inputFontCharset{0};
		QCheckBox                    *m_noEchoOff{nullptr};
		QCheckBox                    *m_enableSpamPrevention{nullptr};
		QSpinBox                     *m_spamLineCount{nullptr};
		QLineEdit                    *m_spamMessage{nullptr};
		QLineEdit                    *m_inputTextColour{nullptr};
		QLineEdit                    *m_inputBackColour{nullptr};
		QComboBox                    *m_echoColour{nullptr};
		QPushButton                  *m_inputEchoSwatch{nullptr};
		QPushButton                  *m_inputEchoSwatch2{nullptr};
		QPushButton                  *m_commandTextSwatch{nullptr};
		QPushButton                  *m_commandBackSwatch{nullptr};
		QCheckBox                    *m_defaultAliasExpandVariables{nullptr};
		QCheckBox                    *m_defaultAliasIgnoreCase{nullptr};
		QCheckBox                    *m_defaultAliasKeepEvaluating{nullptr};
		QCheckBox                    *m_defaultAliasRegexp{nullptr};
		QSpinBox                     *m_defaultAliasSendTo{nullptr};
		QSpinBox                     *m_defaultAliasSequence{nullptr};
		QSpinBox                     *m_defaultTimerSendTo{nullptr};
		QCheckBox                    *m_defaultTriggerExpandVariables{nullptr};
		QCheckBox                    *m_defaultTriggerIgnoreCase{nullptr};
		QCheckBox                    *m_defaultTriggerKeepEvaluating{nullptr};
		QCheckBox                    *m_defaultTriggerRegexp{nullptr};
		QSpinBox                     *m_defaultTriggerSendTo{nullptr};
		QSpinBox                     *m_defaultTriggerSequence{nullptr};

		// Scripting
		QCheckBox                    *m_enableScripts{nullptr};
		QComboBox                    *m_scriptLanguage{nullptr};
		QLineEdit                    *m_scriptFile{nullptr};
		QLineEdit                    *m_scriptPrefix{nullptr};
		QLineEdit                    *m_scriptEditor{nullptr};
		QComboBox                    *m_scriptTextColour{nullptr};
		QPushButton                  *m_scriptTextSwatch{nullptr};
		QPushButton                  *m_scriptBackSwatch{nullptr};
		QComboBox                    *m_scriptReloadOption{nullptr};
		QPushButton                  *m_browseScriptFile{nullptr};
		QPushButton                  *m_newScriptFile{nullptr};
		QPushButton                  *m_editScriptFile{nullptr};
		QPushButton                  *m_chooseScriptEditor{nullptr};
		QCheckBox                    *m_editScriptWithNotepad{nullptr};
		QCheckBox                    *m_warnIfScriptingInactive{nullptr};
		QCheckBox                    *m_scriptErrorsToOutput{nullptr};
		QCheckBox                    *m_logScriptErrors{nullptr};
		QLineEdit                    *m_editorWindowName{nullptr};
		QLabel                       *m_scriptIsActive{nullptr};
		QLabel                       *m_scriptExecutionTime{nullptr};
		QLineEdit                    *m_onWorldOpen{nullptr};
		QLineEdit                    *m_onWorldClose{nullptr};
		QLineEdit                    *m_onWorldConnect{nullptr};
		QLineEdit                    *m_onWorldDisconnect{nullptr};
		QLineEdit                    *m_onWorldSave{nullptr};
		QLineEdit                    *m_onWorldGetFocus{nullptr};
		QLineEdit                    *m_onWorldLoseFocus{nullptr};
		QLineEdit                    *m_onMxpStart{nullptr};
		QLineEdit                    *m_onMxpStop{nullptr};
		QLineEdit                    *m_onMxpOpenTag{nullptr};
		QLineEdit                    *m_onMxpCloseTag{nullptr};
		QLineEdit                    *m_onMxpSetVariable{nullptr};
		QLineEdit                    *m_onMxpError{nullptr};

		// Send to world
		QTextEdit                    *m_sendToWorldFilePreamble{nullptr};
		QTextEdit                    *m_sendToWorldFilePostamble{nullptr};
		QLineEdit                    *m_sendToWorldLinePreamble{nullptr};
		QLineEdit                    *m_sendToWorldLinePostamble{nullptr};
		QCheckBox                    *m_sendConfirm{nullptr};
		QCheckBox                    *m_sendCommentedSoftcode{nullptr};
		QSpinBox                     *m_sendLineDelay{nullptr};
		QSpinBox                     *m_sendDelayPerLines{nullptr};
		QCheckBox                    *m_sendEcho{nullptr};

		// Notes
		QTextEdit                    *m_notes{nullptr};
		QPushButton                  *m_loadNotesButton{nullptr};
		QPushButton                  *m_saveNotesButton{nullptr};
		QPushButton                  *m_editNotesButton{nullptr};
		QPushButton                  *m_findNotesButton{nullptr};
		QPushButton                  *m_findNextNotesButton{nullptr};
		QString                       m_notesFindText;
		int                           m_notesFindIndex{-1};
		QStringList                   m_notesFindHistory;
		bool                          m_notesFindMatchCase{false};
		bool                          m_notesFindForwards{false};
		bool                          m_notesUpdating{false};

		// Keypad
		QCheckBox                    *m_keypadEnabled{nullptr};
		QCheckBox                    *m_keypadControl{nullptr};
		QMap<QString, QLineEdit *>    m_keypadFields;

		// Paste
		QTextEdit                    *m_pastePreamble{nullptr};
		QTextEdit                    *m_pastePostamble{nullptr};
		QLineEdit                    *m_pasteLinePreamble{nullptr};
		QLineEdit                    *m_pasteLinePostamble{nullptr};
		QCheckBox                    *m_confirmOnPaste{nullptr};
		QCheckBox                    *m_commentedSoftcodePaste{nullptr};
		QSpinBox                     *m_pasteLineDelay{nullptr};
		QSpinBox                     *m_pasteDelayPerLines{nullptr};
		QCheckBox                    *m_pasteEcho{nullptr};

		// Info
		QLabel                       *m_infoWorldFile{nullptr};
		QLabel                       *m_infoWorldFileVersion{nullptr};
		QLabel                       *m_infoQmudVersion{nullptr};
		QLineEdit                    *m_infoWorldId{nullptr};
		QLabel                       *m_infoDateSaved{nullptr};
		QLabel                       *m_infoBufferLines{nullptr};
		QLabel                       *m_infoConnectionDuration{nullptr};
		QLabel                       *m_infoConnectionTime{nullptr};
		QLabel                       *m_infoAliases{nullptr};
		QLabel                       *m_infoTriggers{nullptr};
		QLabel                       *m_infoTimers{nullptr};
		QLabel                       *m_infoCompressionRatio{nullptr};
		QLabel                       *m_infoBytesSent{nullptr};
		QLabel                       *m_infoBytesReceived{nullptr};
		QLabel                       *m_infoTriggerTimeTaken{nullptr};
		QLabel                       *m_infoIpAddress{nullptr};
		QLabel                       *m_infoMxpBuiltinElements{nullptr};
		QLabel                       *m_infoMxpBuiltinEntities{nullptr};
		QLabel                       *m_infoMxpEntitiesReceived{nullptr};
		QLabel                       *m_infoMxpErrors{nullptr};
		QLabel                       *m_infoMxpMudElements{nullptr};
		QLabel                       *m_infoMxpMudEntities{nullptr};
		QLabel                       *m_infoMxpTagsReceived{nullptr};
		QLabel                       *m_infoMxpUnclosedTags{nullptr};
		QLabel                       *m_infoCompressedIn{nullptr};
		QLabel                       *m_infoCompressedOut{nullptr};
		QLabel                       *m_infoTimeTakenCompressing{nullptr};
		QLabel                       *m_infoMxpActionsCached{nullptr};
		QLabel                       *m_infoMxpReferenceCount{nullptr};
		QLabel                       *m_infoMemoryUsed{nullptr};
		QPushButton                  *m_infoCalculateMemory{nullptr};

		// Auto-say
		QCheckBox                    *m_enableAutoSay{nullptr};
		QCheckBox                    *m_reEvaluateAutoSay{nullptr};
		QCheckBox                    *m_autoSayExcludeNonAlpha{nullptr};
		QCheckBox                    *m_autoSayExcludeMacros{nullptr};
		QLineEdit                    *m_autoSayString{nullptr};
		QLineEdit                    *m_autoSayOverridePrefix{nullptr};

		// Printing
		QVector<QCheckBox *>          m_printingNormalBold;
		QVector<QCheckBox *>          m_printingNormalItalic;
		QVector<QCheckBox *>          m_printingNormalUnderline;
		QVector<QCheckBox *>          m_printingBoldBold;
		QVector<QCheckBox *>          m_printingBoldItalic;
		QVector<QCheckBox *>          m_printingBoldUnderline;

		// Connecting
		QLineEdit                    *m_playerName{nullptr};
		QLineEdit                    *m_password{nullptr};
		QTextEdit                    *m_connectText{nullptr};
		QComboBox                    *m_connectMethod{nullptr};
		QCheckBox                    *m_onlyNegotiateTelnetOptionsOnce{nullptr};
		QLabel                       *m_connectLineCount{nullptr};

		// MXP
		QComboBox                    *m_useMxp{nullptr};
		QLabel                       *m_mxpActive{nullptr};
		QComboBox                    *m_mxpDebugLevel{nullptr};
		QCheckBox                    *m_detectPueblo{nullptr};
		QLineEdit                    *m_hyperlinkColour{nullptr};
		QCheckBox                    *m_useCustomLinkColour{nullptr};
		QCheckBox                    *m_mudCanChangeLinkColour{nullptr};
		QCheckBox                    *m_underlineHyperlinks{nullptr};
		QCheckBox                    *m_mudCanRemoveUnderline{nullptr};
		QCheckBox                    *m_hyperlinkAddsToCommandHistory{nullptr};
		QCheckBox                    *m_echoHyperlinkInOutput{nullptr};
		QCheckBox                    *m_ignoreMxpColourChanges{nullptr};
		QCheckBox                    *m_sendMxpAfkResponse{nullptr};
		QCheckBox                    *m_mudCanChangeOptions{nullptr};
		QPushButton                  *m_resetMxpTagsButton{nullptr};

		// Chat
		QLineEdit                    *m_chatName{nullptr};
		QCheckBox                    *m_autoAllowSnooping{nullptr};
		QCheckBox                    *m_acceptIncomingChatConnections{nullptr};
		QSpinBox                     *m_incomingChatPort{nullptr};
		QCheckBox                    *m_validateIncomingCalls{nullptr};
		QCheckBox                    *m_ignoreChatColours{nullptr};
		QLineEdit                    *m_chatMessagePrefix{nullptr};
		QSpinBox                     *m_maxChatLines{nullptr};
		QSpinBox                     *m_maxChatBytes{nullptr};
		QLineEdit                    *m_chatSaveDirectory{nullptr};
		QPushButton                  *m_chatSaveBrowse{nullptr};
		QCheckBox                    *m_autoAllowFiles{nullptr};
		QLineEdit                    *m_chatTextColour{nullptr};
		QLineEdit                    *m_chatBackColour{nullptr};

		// Lists
		QTableWidget                 *m_triggersTable{nullptr};
		QTableWidget                 *m_aliasesTable{nullptr};
		QTableWidget                 *m_timersTable{nullptr};
		QTreeWidget                  *m_triggersTree{nullptr};
		QTreeWidget                  *m_aliasesTree{nullptr};
		QTreeWidget                  *m_timersTree{nullptr};
		QStackedWidget               *m_triggersViewStack{nullptr};
		QStackedWidget               *m_aliasesViewStack{nullptr};
		QStackedWidget               *m_timersViewStack{nullptr};
		QTableWidget                 *m_variablesTable{nullptr};
		QCheckBox                    *m_filterVariables{nullptr};
		QPushButton                  *m_editVariablesFilter{nullptr};
		QLabel                       *m_variablesCount{nullptr};
		QPushButton                  *m_addVariableButton{nullptr};
		QPushButton                  *m_editVariableButton{nullptr};
		QPushButton                  *m_deleteVariableButton{nullptr};
		QPushButton                  *m_findVariableButton{nullptr};
		QPushButton                  *m_findNextVariableButton{nullptr};
		QPushButton                  *m_loadVariablesButton{nullptr};
		QPushButton                  *m_saveVariablesButton{nullptr};
		QPushButton                  *m_copyVariableButton{nullptr};
		QPushButton                  *m_pasteVariableButton{nullptr};
		QString                       m_variableFindText;
		int                           m_variableFindRow{-1};
		QString                       m_variableFilterText;
		bool                          m_variableFilterLoaded{false};
		QStringList                   m_variableFindHistory;
		bool                          m_variableFindMatchCase{false};
		bool                          m_variableFindForwards{false};
		bool                          m_variableFindRegex{false};
		QLabel                       *m_triggersCount{nullptr};
		QLabel                       *m_aliasesCount{nullptr};
		QLabel                       *m_timersCount{nullptr};
		QCheckBox                    *m_enableAliases{nullptr};
		QCheckBox                    *m_useDefaultAliases{nullptr};
		QCheckBox                    *m_filterAliases{nullptr};
		QCheckBox                    *m_aliasTreeView{nullptr};
		QPushButton                  *m_editAliasesFilter{nullptr};
		QPushButton                  *m_addAliasButton{nullptr};
		QPushButton                  *m_editAliasButton{nullptr};
		QPushButton                  *m_deleteAliasButton{nullptr};
		QPushButton                  *m_findAliasButton{nullptr};
		QPushButton                  *m_findNextAliasButton{nullptr};
		QPushButton                  *m_moveAliasUpButton{nullptr};
		QPushButton                  *m_moveAliasDownButton{nullptr};
		QPushButton                  *m_copyAliasButton{nullptr};
		QPushButton                  *m_pasteAliasButton{nullptr};
		QCheckBox                    *m_enableTriggers{nullptr};
		QCheckBox                    *m_enableTriggerSounds{nullptr};
		QCheckBox                    *m_useDefaultTriggers{nullptr};
		QCheckBox                    *m_filterTriggers{nullptr};
		QCheckBox                    *m_triggerTreeView{nullptr};
		QPushButton                  *m_editTriggersFilter{nullptr};
		QPushButton                  *m_addTriggerButton{nullptr};
		QPushButton                  *m_editTriggerButton{nullptr};
		QPushButton                  *m_deleteTriggerButton{nullptr};
		QPushButton                  *m_findTriggerButton{nullptr};
		QPushButton                  *m_findNextTriggerButton{nullptr};
		QPushButton                  *m_moveTriggerUpButton{nullptr};
		QPushButton                  *m_moveTriggerDownButton{nullptr};
		QPushButton                  *m_copyTriggerButton{nullptr};
		QPushButton                  *m_pasteTriggerButton{nullptr};
		QCheckBox                    *m_enableTimers{nullptr};
		QCheckBox                    *m_useDefaultTimers{nullptr};
		QCheckBox                    *m_timerTreeView{nullptr};
		QCheckBox                    *m_filterTimers{nullptr};
		QPushButton                  *m_editTimersFilter{nullptr};
		QPushButton                  *m_addTimerButton{nullptr};
		QPushButton                  *m_editTimerButton{nullptr};
		QPushButton                  *m_deleteTimerButton{nullptr};
		QPushButton                  *m_findTimerButton{nullptr};
		QPushButton                  *m_findNextTimerButton{nullptr};
		QPushButton                  *m_resetTimersButton{nullptr};
		QPushButton                  *m_copyTimerButton{nullptr};
		QPushButton                  *m_pasteTimerButton{nullptr};
		QPushButton                  *m_loadTriggersButton{nullptr};
		QPushButton                  *m_saveTriggersButton{nullptr};
		QPushButton                  *m_loadAliasesButton{nullptr};
		QPushButton                  *m_saveAliasesButton{nullptr};
		QPushButton                  *m_loadTimersButton{nullptr};
		QPushButton                  *m_saveTimersButton{nullptr};

		QString                       m_aliasFindText;
		int                           m_aliasFindIndex{-1};
		QStringList                   m_aliasFindHistory;
		bool                          m_aliasFindMatchCase{false};
		bool                          m_aliasFindRegex{false};
		bool                          m_aliasFindForwards{false};
		QString                       m_aliasFilterText;
		bool                          m_aliasFilterLoaded{false};
		QString                       m_triggerFindText;
		int                           m_triggerFindIndex{-1};
		QStringList                   m_triggerFindHistory;
		bool                          m_triggerFindMatchCase{false};
		bool                          m_triggerFindForwards{false};
		bool                          m_triggerFindRegex{false};
		QString                       m_triggerFilterText;
		bool                          m_triggerFilterLoaded{false};
		bool                          m_initialUseDefaultTriggers{false};
		bool                          m_useDefaultTriggersLoaded{false};
		bool                          m_initialUseDefaultAliases{false};
		bool                          m_useDefaultAliasesLoaded{false};
		QString                       m_timerFindText;
		int                           m_timerFindIndex{-1};
		QStringList                   m_timerFindHistory;
		bool                          m_timerFindMatchCase{false};
		bool                          m_timerFindForwards{false};
		bool                          m_timerFindRegex{false};
		QString                       m_timerFilterText;
		bool                          m_timerFilterLoaded{false};
		bool                          m_initialUseDefaultTimers{false};
		bool                          m_useDefaultTimersLoaded{false};
		bool                          m_syncingRuleSelection{false};
};

#endif // QMUD_WORLD_PREFERENCES_DIALOG_H
