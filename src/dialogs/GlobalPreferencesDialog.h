/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: GlobalPreferencesDialog.h
 * Role: Global preferences dialog interfaces for client-wide settings that apply across all worlds.
 */

#ifndef QMUD_GLOBALPREFERENCESDIALOG_H
#define QMUD_GLOBALPREFERENCESDIALOG_H

#include <QDialog>
#include <QHash>

class QTabWidget;
class QPushButton;
class QCheckBox;
class QSpinBox;
class QComboBox;
class QLineEdit;
class QLabel;
class QListWidget;
class QButtonGroup;
class QRadioButton;
class QTextEdit;

/**
 * @brief Application-wide preferences editor dialog.
 *
 * Presents and applies global client settings outside per-world configuration.
 */
class GlobalPreferencesDialog : public QDialog
{
		Q_OBJECT

	public:
		/**
		 * @brief Creates global preferences editor dialog.
		 * @param parent Optional Qt parent widget.
		 */
		explicit GlobalPreferencesDialog(QWidget *parent = nullptr);
		/**
		 * @brief Validates and applies global settings changes.
		 */
		void accept() override;

	private:
		QTabWidget                 *m_tabs{nullptr};
		QHash<QString, QCheckBox *> m_intChecks;
		QHash<QString, QSpinBox *>  m_intSpins;
		QHash<QString, QComboBox *> m_intCombos;
		QHash<QString, QLineEdit *> m_stringEdits;
		QHash<QString, QLabel *>    m_stringLabels;

		QListWidget                *m_worldList{nullptr};
		QLabel                     *m_worldCountLabel{nullptr};
		QLabel                     *m_worldSelected{nullptr};
		QLabel                     *m_worldDefaultDir{nullptr};
		QLabel                     *m_pluginsCountLabel{nullptr};
		QLabel                     *m_pluginsSelected{nullptr};
		QLabel                     *m_pluginsDefaultDir{nullptr};
		QListWidget                *m_pluginsList{nullptr};

		QPushButton                *m_printerFontButton{nullptr};
		QLabel                     *m_printerFontLabel{nullptr};
		int                         m_printerFontSize{0};
		QSpinBox                   *m_printerTopMargin{nullptr};
		QSpinBox                   *m_printerLeftMargin{nullptr};
		QSpinBox                   *m_printerLinesPerPage{nullptr};

		QCheckBox                  *m_autoLogCheck{nullptr};
		QLabel                     *m_logDefaultDirLabel{nullptr};

		QSpinBox                   *m_timerInterval{nullptr};

		QCheckBox                  *m_openActivityWindow{nullptr};
		QRadioButton               *m_activityOnNew{nullptr};
		QRadioButton               *m_activityPeriodic{nullptr};
		QRadioButton               *m_activityBoth{nullptr};
		QSpinBox                   *m_activityPeriod{nullptr};
		QComboBox                  *m_activityBarStyle{nullptr};

		QPushButton                *m_outputFontButton{nullptr};
		QLineEdit                  *m_outputFontName{nullptr};
		QLineEdit                  *m_outputFontStyle{nullptr};
		int                         m_outputFontHeight{0};
		QPushButton                *m_inputFontButton{nullptr};
		QLineEdit                  *m_inputFontName{nullptr};
		QLineEdit                  *m_inputFontStyle{nullptr};
		int                         m_inputFontHeight{0};
		int                         m_inputFontWeight{0};
		int                         m_inputFontItalic{0};
		QLineEdit                  *m_defaultColoursEdit{nullptr};
		QLineEdit                  *m_defaultTriggersEdit{nullptr};
		QLineEdit                  *m_defaultAliasesEdit{nullptr};
		QLineEdit                  *m_defaultMacrosEdit{nullptr};
		QLineEdit                  *m_defaultTimersEdit{nullptr};

		QCheckBox                  *m_notepadWordWrap{nullptr};
		QPushButton                *m_notepadTextSwatch{nullptr};
		QPushButton                *m_notepadBackSwatch{nullptr};
		int                         m_notepadTextColorRef{0};
		int                         m_notepadBackColorRef{0};
		QLineEdit                  *m_notepadQuote{nullptr};
		QCheckBox                  *m_parenNestBraces{nullptr};
		QCheckBox                  *m_parenBackslash{nullptr};
		QCheckBox                  *m_parenPercent{nullptr};
		QCheckBox                  *m_parenSingleQuotes{nullptr};
		QCheckBox                  *m_parenSingleEscape{nullptr};
		QCheckBox                  *m_parenDoubleQuotes{nullptr};
		QCheckBox                  *m_parenDoubleEscape{nullptr};

		QComboBox                  *m_tabsStyle{nullptr};
		QLineEdit                  *m_localeEdit{nullptr};

		QComboBox                  *m_iconPlacement{nullptr};
		QButtonGroup               *m_trayIconGroup{nullptr};
		QRadioButton               *m_customIconRadio{nullptr};
		QPushButton                *m_customIconButton{nullptr};
		QLabel                     *m_customIconLabel{nullptr};

		QCheckBox                  *m_allowDllsCheck{nullptr};
		QTextEdit                  *m_luaScript{nullptr};

		/**
		 * @brief Builds world lists/settings page.
		 * @return Worlds page widget.
		 */
		QWidget                    *buildWorldsPage();
		/**
		 * @brief Builds general application settings page.
		 * @return General page widget.
		 */
		QWidget                    *buildGeneralPage();
		/**
		 * @brief Builds closing/exit behavior page.
		 * @return Closing page widget.
		 */
		QWidget                    *buildClosingPage();
		/**
		 * @brief Builds printing defaults page.
		 * @return Printing page widget.
		 */
		QWidget                    *buildPrintingPage();
		/**
		 * @brief Builds logging defaults page.
		 * @return Logging page widget.
		 */
		QWidget                    *buildLoggingPage();
		/**
		 * @brief Builds timer/tick settings page.
		 * @return Timers page widget.
		 */
		QWidget                    *buildTimersPage();
		/**
		 * @brief Builds activity-window settings page.
		 * @return Activity page widget.
		 */
		QWidget                    *buildActivityPage();
		/**
		 * @brief Builds defaults/files page.
		 * @return Defaults page widget.
		 */
		QWidget                    *buildDefaultsPage();
		/**
		 * @brief Builds notepad/editor settings page.
		 * @return Notepad page widget.
		 */
		QWidget                    *buildNotepadPage();
		/**
		 * @brief Builds tray/icon settings page.
		 * @return Tray page widget.
		 */
		QWidget                    *buildTrayPage();
		/**
		 * @brief Builds plugin settings page.
		 * @return Plugins page widget.
		 */
		QWidget                    *buildPluginsPage();
		/**
		 * @brief Builds Lua/security settings page.
		 * @return Lua page widget.
		 */
		QWidget                    *buildLuaPage();

		/**
		 * @brief Creates a small color swatch button control.
		 * @return Newly created swatch button.
		 */
		static QPushButton         *makeSwatchButton();
		/**
		 * @brief Registers checkbox preference binding by key.
		 * @param key Preference key.
		 * @param box Bound checkbox.
		 */
		void                        registerCheck(const QString &key, QCheckBox *box);
		/**
		 * @brief Registers spinbox preference binding by key.
		 * @param key Preference key.
		 * @param spin Bound spinbox.
		 */
		void                        registerSpin(const QString &key, QSpinBox *spin);
		/**
		 * @brief Registers combo preference binding by key.
		 * @param key Preference key.
		 * @param combo Bound combobox.
		 */
		void                        registerCombo(const QString &key, QComboBox *combo);
		/**
		 * @brief Registers line-edit preference binding by key.
		 * @param key Preference key.
		 * @param edit Bound line edit.
		 */
		void                        registerEdit(const QString &key, QLineEdit *edit);
		/**
		 * @brief Registers label preference binding by key.
		 * @param key Preference key.
		 * @param label Bound label.
		 */
		void                        registerLabel(const QString &key, QLabel *label);
		/**
		 * @brief Loads current global preferences into UI controls.
		 */
		void                        loadPreferences();
		/**
		 * @brief Applies UI values to persisted global preferences.
		 * @return `true` when all settings were applied successfully.
		 */
		bool                        applyPreferences();
		/**
		 * @brief Synchronizes selected world-list summary labels.
		 */
		void                        syncWorldListSelection() const;
		/**
		 * @brief Synchronizes selected plugin-list summary labels.
		 */
		void                        syncPluginListSelection() const;
		/**
		 * @brief Updates button swatch appearance for the given color.
		 * @param button Target swatch button.
		 * @param color Swatch colour.
		 */
		static void                 updateSwatchButton(QPushButton *button, const QColor &color);
		/**
		 * @brief Converts legacy color-ref integer to QColor.
		 * @param colorRef Legacy COLORREF value.
		 * @return Converted colour value.
		 */
		static QColor               colorFromColorRef(int colorRef);
		/**
		 * @brief Converts QColor to legacy color-ref integer.
		 * @param color Source colour.
		 * @return Legacy COLORREF value.
		 */
		static int                  colorRefFromColor(const QColor &color);
		/**
		 * @brief Extracts item texts from list widget.
		 * @param list Source list widget.
		 * @return Item text list.
		 */
		static QStringList          listFromWidget(const QListWidget *list);
		/**
		 * @brief Populates list widget from string items.
		 * @param list Target list widget.
		 * @param items Source string list.
		 */
		static void                 setListFromStrings(QListWidget *list, const QStringList &items);
};

#endif // QMUD_GLOBALPREFERENCESDIALOG_H
