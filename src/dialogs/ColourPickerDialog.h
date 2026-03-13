/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: ColourPickerDialog.h
 * Role: Dialog interfaces for choosing colors used by style settings, trigger formatting, and script-driven UI
 * customization.
 */

#ifndef QMUD_COLOURPICKERDIALOG_H
#define QMUD_COLOURPICKERDIALOG_H

#include <QDialog>

class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QTableWidget;

/**
 * @brief Color picker dialog for palette and custom-color editing flows.
 */
class ColourPickerDialog : public QDialog
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates color picker dialog with full palette controls.
		 * @param parent Optional Qt parent widget.
		 */
		explicit ColourPickerDialog(QWidget *parent = nullptr);

		/**
		 * @brief Sets picker mode flag.
		 * @param pick Enable pick-only mode when `true`.
		 */
		void                 setPickColour(bool pick);
		/**
		 * @brief Sets initially selected color.
		 * @param colour Initial colour value.
		 */
		void                 setInitialColour(const QColor &colour);
		/**
		 * @brief Returns color selected by user.
		 * @return Selected colour.
		 */
		[[nodiscard]] QColor selectedColour() const;

	private:
		enum SortMode
		{
			SortName       = 0,
			SortHue        = 1,
			SortSaturation = 2,
			SortLuminance  = 3,
			SortRed        = 4,
			SortGreen      = 5,
			SortBlue       = 6
		};

		struct Entry
		{
				QString name;
				QColor  color;
				double  hue{0.0};
				double  saturation{0.0};
				double  luminance{0.0};
		};

		/**
		 * @brief Loads palette entries from predefined colours.
		 */
		void           loadEntries();
		/**
		 * @brief Rebuilds table according to active sort mode.
		 */
		void           rebuildList();
		/**
		 * @brief Selects table row matching current color.
		 */
		void           selectCurrentColour() const;
		/**
		 * @brief Updates numeric/color controls from current colour value.
		 */
		void           updateFromColour();
		/**
		 * @brief Repaints color swatch button.
		 * @param button Target swatch button.
		 * @param color Swatch colour.
		 */
		static void    updateSwatch(QPushButton *button, const QColor &color);
		/**
		 * @brief Enables/disables clipboard-related actions.
		 */
		void           updateClipboardState();
		/**
		 * @brief Sets current color and refreshes dependent widgets.
		 * @param color New colour value.
		 */
		void           setColour(const QColor &color);
		/**
		 * @brief Attempts parsing clipboard text as color.
		 * @param color Output parsed colour.
		 * @return `true` when clipboard contained a parsable colour.
		 */
		static bool    tryParseClipboardColour(QColor &color);
		/**
		 * @brief Returns known color-name aliases for a color.
		 * @param color Source colour.
		 * @return Alias names for the color.
		 */
		static QString colourNamesFor(const QColor &color);

		QTableWidget  *m_list{nullptr};
		QComboBox     *m_sort{nullptr};
		QPushButton   *m_swatch{nullptr};
		QLabel        *m_redValue{nullptr};
		QLabel        *m_greenValue{nullptr};
		QLabel        *m_blueValue{nullptr};
		QLabel        *m_hueValue{nullptr};
		QLabel        *m_satValue{nullptr};
		QLabel        *m_lumValue{nullptr};
		QSlider       *m_redSlider{nullptr};
		QSlider       *m_greenSlider{nullptr};
		QSlider       *m_blueSlider{nullptr};
		QPushButton   *m_colourName{nullptr};
		QPushButton   *m_mxpName{nullptr};
		QPushButton   *m_luaName{nullptr};
		QPushButton   *m_paste{nullptr};
		QPushButton   *m_ok{nullptr};

		QVector<Entry> m_entries;
		QColor         m_colour;
		QColor         m_clipboardColour;
		bool           m_pickColour{false};
		bool           m_updating{false};

		static QColor  s_lastColour;
		static int     s_sort;
};

#endif // QMUD_COLOURPICKERDIALOG_H
