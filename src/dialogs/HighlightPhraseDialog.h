/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: HighlightPhraseDialog.h
 * Role: Dialog interfaces for defining highlight phrases and associated style attributes used in output rendering.
 */

#ifndef QMUD_HIGHLIGHTPHRASEDIALOG_H
#define QMUD_HIGHLIGHTPHRASEDIALOG_H

#include <QDialog>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;

/**
 * @brief Dialog for defining highlight phrase colors and behavior.
 */
class HighlightPhraseDialog : public QDialog
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates highlight-phrase editor dialog.
		 * @param parent Optional Qt parent widget.
		 */
		explicit HighlightPhraseDialog(QWidget *parent = nullptr);

		/**
		 * @brief Sets custom palette used in color selector.
		 * @param textColours Custom text-colour entries.
		 * @param backColours Custom background-color entries.
		 * @param names Display names for custom colors.
		 */
		void setCustomColours(const QVector<QColor> &textColours, const QVector<QColor> &backColours,
		                      const QVector<QString> &names);
		/**
		 * @brief Sets custom “other” foreground/background colors.
		 * @param fore Custom foreground color.
		 * @param back Custom background color.
		 */
		void setOtherColours(const QColor &fore, const QColor &back);
		/**
		 * @brief Sets initial phrase text.
		 * @param text Initial phrase text.
		 */
		void setInitialText(const QString &text) const;
		/**
		 * @brief Enables/disables whole-word matching.
		 * @param enabled Enable whole-word matching when `true`.
		 */
		void setWholeWord(bool enabled) const;
		/**
		 * @brief Enables/disables case-sensitive matching.
		 * @param enabled Enable case-sensitive matching when `true`.
		 */
		void setMatchCase(bool enabled) const;
		/**
		 * @brief Sets selected color index in combo.
		 * @param index Selected color index.
		 */
		void setSelectedColourIndex(int index);

		/**
		 * @brief Returns phrase text.
		 * @return Phrase text.
		 */
		[[nodiscard]] QString phraseText() const;
		/**
		 * @brief Returns case-sensitive flag.
		 * @return `true` when case-sensitive matching is enabled.
		 */
		[[nodiscard]] bool    matchCase() const;
		/**
		 * @brief Returns whole-word flag.
		 * @return `true` when whole-word matching is enabled.
		 */
		[[nodiscard]] bool    wholeWord() const;
		/**
		 * @brief Returns selected color index.
		 * @return Selected color index.
		 */
		[[nodiscard]] int     selectedColourIndex() const;
		/**
		 * @brief Returns custom “other” text color.
		 * @return Custom “other” text color.
		 */
		[[nodiscard]] QColor  otherTextColour() const;
		/**
		 * @brief Returns custom “other” background color.
		 * @return Custom “other” background color.
		 */
		[[nodiscard]] QColor  otherBackColour() const;

	protected:
		/**
		 * @brief Validates and commits highlight phrase settings.
		 */
		void accept() override;

	private:
		/**
		 * @brief Refreshes swatch buttons from current color selection.
		 */
		void             updateSwatches();
		/**
		 * @brief Applies solid color preview styling to button.
		 * @param button Target swatch button.
		 * @param color Swatch colour.
		 */
		static void      updateSwatchButton(QPushButton *button, const QColor &color);

		QLineEdit       *m_text{nullptr};
		QCheckBox       *m_wholeWord{nullptr};
		QCheckBox       *m_matchCase{nullptr};
		QComboBox       *m_colourCombo{nullptr};
		QPushButton     *m_textSwatch{nullptr};
		QPushButton     *m_backSwatch{nullptr};

		QVector<QColor>  m_customText;
		QVector<QColor>  m_customBack;
		QVector<QString> m_customNames;

		QColor           m_otherForeground;
		QColor           m_otherBackground;
		QColor           m_otherForegroundOrig;
		QColor           m_otherBackgroundOrig;
};

#endif // QMUD_HIGHLIGHTPHRASEDIALOG_H
