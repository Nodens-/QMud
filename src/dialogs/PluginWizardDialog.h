/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: PluginWizardDialog.h
 * Role: Wizard dialog interfaces for creating new plugin scaffolds and metadata interactively.
 */

#ifndef QMUD_PLUGINWIZARDDIALOG_H
#define QMUD_PLUGINWIZARDDIALOG_H

#include <QVector>
#include <QWizard>

class QLineEdit;
class QTextEdit;
class QDoubleSpinBox;
class QCheckBox;
class QComboBox;
class QTableWidget;
class WorldRuntime;

/**
 * @brief Aggregated wizard state used to generate a plugin scaffold.
 */
struct PluginWizardState
{
		QString   name;
		QString   author;
		QString   purpose;
		QString   version;
		QString   id;
		QString   dateWritten;
		double    requiresVersion{0.0};
		bool      removeItems{true};

		QString   description;
		bool      generateHelp{true};
		QString   helpAlias;

		QSet<int> selectedTriggers;
		QSet<int> selectedAliases;
		QSet<int> selectedTimers;
		QSet<int> selectedVariables;

		bool      saveState{false};

		QString   language;
		bool      standardConstants{false};
		QString   script;

		QString   comments;
};

/**
 * @brief Multi-step wizard for creating plugin metadata and script scaffolding.
 */
class PluginWizardDialog : public QWizard
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates plugin wizard bound to a runtime for list/source data.
		 * @param runtime Runtime used as source for existing items.
		 * @param parent Optional Qt parent widget.
		 */
		explicit PluginWizardDialog(WorldRuntime *runtime, QWidget *parent = nullptr);
		/**
		 * @brief Returns collected wizard state.
		 * @return Collected wizard state.
		 */
		[[nodiscard]] const PluginWizardState &state() const;

	protected:
		/**
		 * @brief Validates current page values and commits final state.
		 */
		void accept() override;

	private:
		/**
		 * @brief Builds wizard introduction/metadata page.
		 */
		void              buildPage1();
		/**
		 * @brief Builds description/help page.
		 */
		void              buildPage2();
		/**
		 * @brief Builds trigger selection page.
		 */
		void              buildPage3();
		/**
		 * @brief Builds alias selection page.
		 */
		void              buildPage4();
		/**
		 * @brief Builds timer selection page.
		 */
		void              buildPage5();
		/**
		 * @brief Builds variable selection page.
		 */
		void              buildPage6();
		/**
		 * @brief Builds scripting/language page.
		 */
		void              buildPage7();
		/**
		 * @brief Builds final comments/review page.
		 */
		void              buildPage8();

		/**
		 * @brief Loads trigger list from runtime into selection table.
		 */
		void              populateTriggers() const;
		/**
		 * @brief Loads alias list from runtime into selection table.
		 */
		void              populateAliases() const;
		/**
		 * @brief Loads timer list from runtime into selection table.
		 */
		void              populateTimers() const;
		/**
		 * @brief Loads variable list from runtime into selection table.
		 */
		void              populateVariables() const;
		/**
		 * @brief Enables/disables help alias controls.
		 */
		void              updateHelpEnabled() const;
		/**
		 * @brief Sorts trigger table by selected column.
		 * @param column Column index to sort by.
		 */
		void              sortTriggers(int column);
		/**
		 * @brief Sorts alias table by selected column.
		 * @param column Column index to sort by.
		 */
		void              sortAliases(int column);
		/**
		 * @brief Sorts timer table by selected column.
		 * @param column Column index to sort by.
		 */
		void              sortTimers(int column);
		/**
		 * @brief Sorts variable table by selected column.
		 * @param column Column index to sort by.
		 */
		void              sortVariables(int column);
		/**
		 * @brief Returns selected row indices from a table.
		 * @param table Source table widget.
		 * @return Selected row index set.
		 */
		static QSet<int>  selectedIndices(const QTableWidget *table);
		/**
		 * @brief Fills trigger table from ordered indices and selection set.
		 * @param order Row order by source index.
		 * @param selected Selected row index set.
		 */
		void              fillTriggerTable(const QVector<int> &order, const QSet<int> &selected) const;
		/**
		 * @brief Fills alias table from ordered indices and selection set.
		 * @param order Row order by source index.
		 * @param selected Selected row index set.
		 */
		void              fillAliasTable(const QVector<int> &order, const QSet<int> &selected) const;
		/**
		 * @brief Fills timer table from ordered indices and selection set.
		 * @param order Row order by source index.
		 * @param selected Selected row index set.
		 */
		void              fillTimerTable(const QVector<int> &order, const QSet<int> &selected) const;
		/**
		 * @brief Fills variable table from ordered indices and selection set.
		 * @param order Row order by source index.
		 * @param selected Selected row index set.
		 */
		void              fillVariableTable(const QVector<int> &order, const QSet<int> &selected) const;

		WorldRuntime     *m_runtime{nullptr};
		PluginWizardState m_state;

		QLineEdit        *m_nameEdit{nullptr};
		QLineEdit        *m_authorEdit{nullptr};
		QLineEdit        *m_purposeEdit{nullptr};
		QLineEdit        *m_versionEdit{nullptr};
		QLineEdit        *m_idEdit{nullptr};
		QLineEdit        *m_dateEdit{nullptr};
		QDoubleSpinBox   *m_requiresSpin{nullptr};
		QCheckBox        *m_removeItems{nullptr};

		QTextEdit        *m_descriptionEdit{nullptr};
		QCheckBox        *m_generateHelp{nullptr};
		QLineEdit        *m_helpAliasEdit{nullptr};

		QTableWidget     *m_triggerTable{nullptr};
		QTableWidget     *m_aliasTable{nullptr};
		QTableWidget     *m_timerTable{nullptr};
		QTableWidget     *m_variableTable{nullptr};
		QCheckBox        *m_saveState{nullptr};

		QComboBox        *m_languageCombo{nullptr};
		QCheckBox        *m_standardConstants{nullptr};
		QTextEdit        *m_scriptEdit{nullptr};

		QTextEdit        *m_commentsEdit{nullptr};

		int               m_triggerSortColumn{0};
		bool              m_triggerSortReverse{false};
		int               m_aliasSortColumn{0};
		bool              m_aliasSortReverse{false};
		int               m_timerSortColumn{0};
		bool              m_timerSortReverse{false};
		int               m_variableSortColumn{0};
		bool              m_variableSortReverse{false};
};

#endif // QMUD_PLUGINWIZARDDIALOG_H
