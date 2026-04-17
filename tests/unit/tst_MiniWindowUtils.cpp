/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_MiniWindowUtils.cpp
 * Role: QTest coverage for MiniWindowUtils behavior.
 */

#include "MiniWindowUtils.h"

#include <QtTest/QTest>

/**
 * @brief QTest fixture covering MiniWindowUtils scenarios.
 */
class tst_MiniWindowUtils : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void lineFitsVerticallyAllowsExactBottomFit()
		{
			QVERIFY(MiniWindowUtils::lineFitsVertically(10, 11, 20));
		}

		void lineFitsVerticallyRejectsOverflowPastBottom()
		{
			QVERIFY(!MiniWindowUtils::lineFitsVertically(10, 12, 20));
		}

		void runNeedsWrapDoesNotWrapOnExactRightFit()
		{
			QVERIFY(!MiniWindowUtils::runNeedsWrap(5, 15, 10, 5, 20));
		}

		void runNeedsWrapWrapsWhenCandidateExceedsRightBound()
		{
			QVERIFY(MiniWindowUtils::runNeedsWrap(5, 16, 10, 5, 20));
		}

		void runNeedsWrapSkipsWrapForFirstGlyphAtLineStart()
		{
			QVERIFY(!MiniWindowUtils::runNeedsWrap(5, 16, 0, 5, 20));
		}

		void hasActivatableActionRequiresNonNoneAndNonBlankPayload()
		{
			QVERIFY(!MiniWindowUtils::hasActivatableAction(0, QStringLiteral("say hi"), 0));
			QVERIFY(!MiniWindowUtils::hasActivatableAction(1, QStringLiteral("   "), 0));
			QVERIFY(MiniWindowUtils::hasActivatableAction(1, QStringLiteral("say hi"), 0));
		}

		void colorFromRefRoundTripsRgbTriplet()
		{
			const QColor color = MiniWindowUtils::colorFromRef(0x563412);
			QCOMPARE(color.red(), 0x12);
			QCOMPARE(color.green(), 0x34);
			QCOMPARE(color.blue(), 0x56);
		}

		void colorFromRefOrTransparentHandlesMinusOneAsTransparent()
		{
			const QColor color = MiniWindowUtils::colorFromRefOrTransparent(-1);
			QCOMPARE(color.alpha(), 0);
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_MiniWindowUtils)

#if __has_include("tst_MiniWindowUtils.moc")
#include "tst_MiniWindowUtils.moc"
#endif
