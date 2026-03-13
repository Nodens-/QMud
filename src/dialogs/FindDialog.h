/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: FindDialog.h
 * Role: Find dialog interfaces used to search output buffers and navigate matches within world/activity text views.
 */

#ifndef QMUD_FINDDIALOG_H
#define QMUD_FINDDIALOG_H

#include <QDialog>
#include <QString>

/**
 * @brief Reusable find/search dialog with history support.
 */
class FindDialog : public QDialog
{
	public:
		/**
		 * @brief Creates dialog bound to shared find-history list.
		 * @param findHistory Shared find-history list.
		 * @param parent Optional Qt parent widget.
		 */
		FindDialog(QStringList &findHistory, QWidget *parent = nullptr);
		/**
		 * @brief Shows dialog modally and persists control state.
		 * @return Dialog result code.
		 */
		int                   execModal();

		/**
		 * @brief Sets dialog title text.
		 * @param title Dialog title text.
		 */
		void                  setTitleText(const QString &title);
		/**
		 * @brief Returns dialog title text.
		 * @return Dialog title text.
		 */
		[[nodiscard]] QString titleText() const;
		/**
		 * @brief Sets active search string.
		 * @param text Active search text.
		 */
		void                  setFindText(const QString &text);
		/**
		 * @brief Returns active search string.
		 * @return Active search text.
		 */
		[[nodiscard]] QString findText() const;
		/**
		 * @brief Enables/disables case-sensitive matching.
		 * @param enabled Enable case-sensitive matching when `true`.
		 */
		void                  setMatchCase(bool enabled);
		/**
		 * @brief Returns case-sensitive matching flag.
		 * @return `true` when match-case mode is enabled.
		 */
		[[nodiscard]] bool    matchCase() const;
		/**
		 * @brief Enables/disables regex search mode.
		 * @param enabled Enable regex mode when `true`.
		 */
		void                  setRegexp(bool enabled);
		/**
		 * @brief Returns regex search mode flag.
		 * @return `true` when regex mode is enabled.
		 */
		[[nodiscard]] bool    regexp() const;
		/**
		 * @brief Sets forward/backward search direction.
		 * @param enabled Search forward when `true`, backward otherwise.
		 */
		void                  setForwards(bool enabled);
		/**
		 * @brief Returns forward/backward search direction.
		 * @return `true` when searching forwards.
		 */
		[[nodiscard]] bool    forwards() const;

	private:
		/**
		 * @brief Opens help for supported regular expression syntax.
		 */
		static void  onRegexpHelp();

		bool         m_matchCase{false};
		bool         m_forwards{false};
		bool         m_regexp{false};
		QString      m_findText;
		QString      m_title;
		QStringList &m_findHistory;
};

#endif // QMUD_FINDDIALOG_H
