/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_Dialog_ColourPicker.cpp
 * Role: QTest coverage for Dialog ColourPicker behavior.
 */

#include "dialogs/ColourPickerDialog.h"

#include <QPushButton>
#include <QSlider>
#include <QtTest/QTest>

namespace
{
	QPushButton *findButtonByText(const QObject &root, const QString &text)
	{
		const auto buttons = root.findChildren<QPushButton *>();
		for (QPushButton *button : buttons)
		{
			if (button && button->text() == text)
				return button;
		}
		return nullptr;
	}
} // namespace

/**
 * @brief QTest fixture covering Dialog ColourPicker scenarios.
 */
class tst_Dialog_ColourPicker : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void initialColourAndInvalidNormalisation()
		{
			ColourPickerDialog dialog;
			dialog.setInitialColour(QColor(12, 34, 56));
			QCOMPARE(dialog.selectedColour(), QColor(12, 34, 56));

			// Invalid colors should not overwrite the current selection.
			dialog.setInitialColour(QColor());
			QCOMPARE(dialog.selectedColour(), QColor(12, 34, 56));
		}

		void sliderEditsUpdateSelectedColour()
		{
			ColourPickerDialog dialog;
			dialog.setInitialColour(QColor(10, 20, 30));

			const auto sliders = dialog.findChildren<QSlider *>();
			QCOMPARE(sliders.size(), 3);

			QSlider *redSlider   = nullptr;
			QSlider *greenSlider = nullptr;
			QSlider *blueSlider  = nullptr;
			for (QSlider *slider : sliders)
			{
				if (!slider)
					continue;
				if (slider->value() == 10 && !redSlider)
					redSlider = slider;
				else if (slider->value() == 20 && !greenSlider)
					greenSlider = slider;
				else if (slider->value() == 30 && !blueSlider)
					blueSlider = slider;
			}

			QVERIFY(redSlider);
			QVERIFY(greenSlider);
			QVERIFY(blueSlider);
			if (!redSlider || !greenSlider || !blueSlider)
				QFAIL("Expected to locate red, green, and blue sliders");

			redSlider->setValue(101);
			greenSlider->setValue(102);
			blueSlider->setValue(103);

			QCOMPARE(dialog.selectedColour(), QColor(101, 102, 103));
		}

		void acceptAndRejectButtonsSetDialogResult()
		{
			{
				ColourPickerDialog dialog;
				dialog.show();
				QPushButton *closeButton = findButtonByText(dialog, QStringLiteral("Close"));
				QVERIFY(closeButton);
				QTest::mouseClick(closeButton, Qt::LeftButton);
				QCOMPARE(dialog.result(), static_cast<int>(QDialog::Rejected));
			}

			{
				ColourPickerDialog dialog;
				dialog.show();
				QPushButton *okButton = findButtonByText(dialog, QStringLiteral("OK"));
				QVERIFY(okButton);
				QTest::mouseClick(okButton, Qt::LeftButton);
				QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
			}
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_MAIN(tst_Dialog_ColourPicker)

#include "tst_Dialog_ColourPicker.moc"
