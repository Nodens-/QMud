/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: SpellCheckDialog.h
 * Role: Spell-check dialog interfaces for presenting suggestions and applying replacements in text-editing contexts.
 */

#ifndef QMUD_SPELLCHECKDIALOG_H
#define QMUD_SPELLCHECKDIALOG_H

#include <QDialog>
#include <QString>

class QLineEdit;
class QListWidget;

/**
 * @brief Spell-check suggestion dialog for replacing flagged words.
 */
class SpellCheckDialog : public QDialog
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates spell-check suggestion dialog for one word.
		 * @param word Misspelled/original word.
		 * @param suggestions Suggested replacements.
		 * @param parent Optional Qt parent widget.
		 */
		SpellCheckDialog(const QString &word, const QStringList &suggestions, QWidget *parent = nullptr);

		/**
		 * @brief Returns selected action keyword.
		 * @return Selected action keyword.
		 */
		[[nodiscard]] QString action() const;
		/**
		 * @brief Returns replacement text selected/entered by user.
		 * @return Replacement text.
		 */
		[[nodiscard]] QString replacement() const;

	protected:
		/**
		 * @brief Commits selected spell-check action.
		 */
		void accept() override;

	private:
		/**
		 * @brief Auto-selects suggestion if user has not changed input.
		 */
		void         chooseSuggestionIfUnchanged() const;

		QString      m_original;
		QString      m_action;
		QLineEdit   *m_wordEdit{nullptr};
		QListWidget *m_suggestions{nullptr};
};

#endif // QMUD_SPELLCHECKDIALOG_H
