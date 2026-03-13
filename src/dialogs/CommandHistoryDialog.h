/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: CommandHistoryDialog.h
 * Role: Dialog interfaces for browsing, filtering, and reusing previously entered commands from session history.
 */

#ifndef QMUD_COMMANDHISTORYDIALOG_H
#define QMUD_COMMANDHISTORYDIALOG_H

#include <QDialog>
#include <QStringList>

class QListWidget;
class QLineEdit;
class QPushButton;
class QRegularExpression;
class WorldView;
struct CommandHistoryFindState;

/**
 * @brief Dialog for searching and replaying command history entries.
 */
class CommandHistoryDialog : public QDialog
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates command-history dialog.
		 * @param parent Optional Qt parent widget.
		 */
		explicit CommandHistoryDialog(QWidget *parent = nullptr);

		QStringList             *m_msgList{nullptr};
		WorldView               *m_sendview{nullptr};
		CommandHistoryFindState *m_pHistoryFindInfo{nullptr};
		/**
		 * @brief Rebuilds list widget from current history/filter settings.
		 */
		void                     populateList() const;

	protected:
		/**
		 * @brief Executes find/find-next behavior over command history.
		 * @param again Continue from current match when `true`.
		 */
		void        doFind(bool again);
		/**
		 * @brief Syncs editor and list selection state.
		 */
		void        updateSelection() const;
		/**
		 * @brief Matches one line against current query settings.
		 * @param line Candidate line text.
		 * @param needle Query text.
		 * @param matchCase Match case-sensitively when `true`.
		 * @param regexp Treat query as regular expression when `true`.
		 * @param regex Precompiled regular expression.
		 * @return `true` when line matches query settings.
		 */
		static bool matchLine(const QString &line, const QString &needle, bool matchCase, bool regexp,
		                      const QRegularExpression &regex);

	private:
		QListWidget *m_list{nullptr};
		QLineEdit   *m_historyItem{nullptr};
		QPushButton *m_findButton{nullptr};
		QPushButton *m_findNextButton{nullptr};
		QPushButton *m_doButton{nullptr};
		QPushButton *m_notepadButton{nullptr};
		QPushButton *m_helpButton{nullptr};
};

#endif // QMUD_COMMANDHISTORYDIALOG_H
