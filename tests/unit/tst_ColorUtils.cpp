/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_ColorUtils.cpp
 * Role: QTest coverage for ColorUtils behavior.
 */

#include "ColorPacking.h"
#include "ColorUtils.h"

#include <QtTest/QTest>

/**
 * @brief QTest fixture covering ColorUtils scenarios.
 */
class tst_ColorUtils : public QObject
{
		Q_OBJECT

	// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void namedColours()
		{
			QCOMPARE(qmudColourToName(qmudRgb(255, 0, 0)), QStringLiteral("red"));
			QCOMPARE(qmudColourToName(qmudRgb(0, 255, 255)), QStringLiteral("cyan"));
			QCOMPARE(qmudColourToName(qmudRgb(0, 0, 0)), QStringLiteral("black"));
		}

		void fallbackHexFormatting()
		{
			QCOMPARE(qmudColourToName(qmudRgb(1, 2, 3)), QStringLiteral("#010203"));
		}
	// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_ColorUtils)



#if __has_include("tst_ColorUtils.moc")
#include "tst_ColorUtils.moc"
#endif
