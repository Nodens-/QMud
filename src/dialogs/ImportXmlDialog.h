/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: ImportXmlDialog.h
 * Role: Dialog interfaces for importing XML world/plugin assets and selecting import scope/options.
 */

#ifndef QMUD_IMPORTXMLDIALOG_H
#define QMUD_IMPORTXMLDIALOG_H

#include <QDialog>

class QCheckBox;
class QPushButton;
class AppController;

/**
 * @brief Dialog for importing legacy XML data into the active world.
 */
class ImportXmlDialog : public QDialog
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates XML import dialog bound to app controller.
		 * @param app App controller context.
		 * @param parent Optional Qt parent widget.
		 */
		explicit ImportXmlDialog(AppController *app, QWidget *parent = nullptr);

	private slots:
		/**
		 * @brief Imports XML content from a selected file.
		 */
		void onImportFile();
		/**
		 * @brief Imports XML content from clipboard text.
		 */
		void onImportClipboard();
		/**
		 * @brief Shows available plugin list/details before import.
		 */
		void onPluginsList();

	private:
		/**
		 * @brief Builds import section mask from checkbox state.
		 * @return Import section mask.
		 */
		[[nodiscard]] unsigned long buildMask() const;
		/**
		 * @brief Imports XML payload text with current mask/options.
		 * @param text XML payload text.
		 * @return `true` on successful import.
		 */
		bool                        importXmlText(const QString &text);
		/**
		 * @brief Returns true when text appears to be XML payload.
		 * @param text Candidate text.
		 * @return `true` when text looks like XML.
		 */
		static bool                 isXmlText(const QString &text);
		/**
		 * @brief Returns true when file extension/content indicates XML.
		 * @param path Candidate file path.
		 * @return `true` when file appears to be XML.
		 */
		static bool                 isXmlFile(const QString &path);
		/**
		 * @brief Enables/disables clipboard import action by content validity.
		 */
		void                        updateClipboardState() const;
		/**
		 * @brief Shows post-import element counts summary.
		 * @param title Summary title text.
		 * @param triggers Imported trigger count.
		 * @param aliases Imported alias count.
		 * @param timers Imported timer count.
		 * @param macros Imported macro count.
		 * @param variables Imported variable count.
		 * @param colours Imported colour count.
		 * @param keypad Imported keypad count.
		 * @param printing Imported printing-style count.
		 * @param duplicates Duplicate/skipped count.
		 */
		void showImportCounts(const QString &title, int triggers, int aliases, int timers, int macros,
		                      int variables, int colours, int keypad, int printing, int duplicates);

		AppController *m_app{nullptr};
		QCheckBox     *m_general{nullptr};
		QCheckBox     *m_triggers{nullptr};
		QCheckBox     *m_aliases{nullptr};
		QCheckBox     *m_timers{nullptr};
		QCheckBox     *m_macros{nullptr};
		QCheckBox     *m_variables{nullptr};
		QCheckBox     *m_colours{nullptr};
		QCheckBox     *m_keypad{nullptr};
		QCheckBox     *m_printing{nullptr};
		QPushButton   *m_importClipboard{nullptr};
};

#endif // QMUD_IMPORTXMLDIALOG_H
