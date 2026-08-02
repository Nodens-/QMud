/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_MiniWindowUtils.cpp
 * Role: QTest coverage for MiniWindowUtils behavior.
 */

#include "MiniWindowUtils.h"

#include "MiniWindow.h"
#include "scripting/ScriptingErrors.h"

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

		void hasActivatableActionRejectsUnresolvedMxpTextEntityReference()
		{
			QVERIFY(!MiniWindowUtils::hasActivatableAction(1, QStringLiteral("help &text;"), 0));
			QVERIFY(!MiniWindowUtils::hasActivatableAction(1, QStringLiteral("help &amp;text;"), 0));
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

		void positionAndResizeUpdateCanonicalGeometryState()
		{
			MiniWindow window;
			MiniWindowUtils::create(window, QStringLiteral("geom"), 5, 7, 32, 24, 0, 0, QColor(Qt::black),
			                        QString());
			MiniWindowUtils::rectOp(window, 2, 0, 0, 0, 0, MiniWindowUtils::colorToRef(QColor(Qt::red)), 0);

			MiniWindowUtils::position(window, 40, 50, 4, kMiniWindowAbsoluteLocation);
			MiniWindowUtils::resize(window, 48, 36, 0x000000);

			QCOMPARE(window.location, QPoint(40, 50));
			QCOMPARE(window.position, 4);
			QCOMPARE(window.flags, kMiniWindowAbsoluteLocation);
			QCOMPARE(window.width, 48);
			QCOMPARE(window.height, 36);
			QCOMPARE(window.backingSurfaceSize(), QSize(48, 36));
			QCOMPARE(window.backingSurface().pixelColor(0, 0), QColor(Qt::red));
		}

		void apiRectKeepsAbsoluteMiniWindowScalerInternal()
		{
			MiniWindow window;
			MiniWindowUtils::create(window, QStringLiteral("absolute-api"), 100, 200, 50, 40, 0,
			                        kMiniWindowAbsoluteLocation, QColor(Qt::black), QString());
			window.rect = QRect(25, 50, 13, 10);

			QCOMPARE(window.apiRect(), QRect(100, 200, 50, 40));

			MiniWindowUtils::position(window, 4, 6, 0, 0);
			window.rect = QRect(4, 6, 50, 40);

			QCOMPARE(window.apiRect(), QRect(4, 6, 50, 40));
		}

		void highDpiCreateKeepsLogicalGeometryAndUsesPhysicalBacking()
		{
			MiniWindow window;
			MiniWindowUtils::create(window, QStringLiteral("hidpi"), 5, 7, 32, 24, 0, 0, QColor(Qt::black),
			                        QString(), 1.5);

			QCOMPARE(window.width, 32);
			QCOMPARE(window.height, 24);
			QCOMPARE(window.backingSurfaceSize(), QSize(48, 36));
			QCOMPARE(window.backingSurfaceDevicePixelRatio(), 1.5);
		}

		void setBackingSurfaceNormalizesImageDevicePixelRatio()
		{
			MiniWindow window;
			QImage     backing(QSize(8, 6), QImage::Format_ARGB32);
			backing.setDevicePixelRatio(0.5);

			window.setBackingSurface(backing);

			QCOMPARE(window.devicePixelRatio, 1.0);
			QCOMPARE(window.backingSurfaceDevicePixelRatio(), 1.0);
			QCOMPARE(window.backingSurface().devicePixelRatio(), 1.0);
		}

		void highDpiLogicalPixelAccessUsesApiCoordinates()
		{
			MiniWindow window;
			MiniWindowUtils::create(window, QStringLiteral("hidpi-pixel"), 0, 0, 16, 12, 0, 0,
			                        QColor(Qt::black), QString(), 2.0);

			MiniWindowUtils::setPixel(window, 2, 3, MiniWindowUtils::colorToRef(QColor(Qt::red)));

			QCOMPARE(MiniWindowUtils::pixelValue(window, 2, 3), MiniWindowUtils::colorToRef(QColor(Qt::red)));
			QCOMPARE(window.backingSurface().pixelColor(4, 6), QColor(Qt::red));
		}

		void highDpiResizeKeepsDpiAndLogicalContent()
		{
			MiniWindow window;
			MiniWindowUtils::create(window, QStringLiteral("hidpi-resize"), 0, 0, 16, 12, 0, 0,
			                        QColor(Qt::black), QString(), 1.5);
			MiniWindowUtils::setPixel(window, 1, 1, MiniWindowUtils::colorToRef(QColor(Qt::green)));

			MiniWindowUtils::resize(window, 20, 14, MiniWindowUtils::colorToRef(QColor(Qt::black)));

			QCOMPARE(window.width, 20);
			QCOMPARE(window.height, 14);
			QCOMPARE(window.backingSurfaceSize(), QSize(30, 21));
			QCOMPARE(window.backingSurfaceDevicePixelRatio(), 1.5);
			QCOMPARE(MiniWindowUtils::pixelValue(window, 1, 1),
			         MiniWindowUtils::colorToRef(QColor(Qt::green)));
		}

		void detachedImageCopyPreservesCanonicalGeometryState()
		{
			MiniWindow window;
			MiniWindowUtils::create(window, QStringLiteral("geom-copy"), 5, 7, 32, 24, 0, 0,
			                        QColor(Qt::black), QStringLiteral("plugin-id"));
			MiniWindowUtils::position(window, 44, 55, 4, kMiniWindowAbsoluteLocation);
			MiniWindowUtils::resize(window, 64, 40, 0x000000);
			window.rect = QRect(window.location, QSize(window.width, window.height));
			MiniWindowUtils::rectOp(window, 2, 0, 0, 0, 0, MiniWindowUtils::colorToRef(QColor(Qt::blue)), 0);

			const MiniWindow copy = window.detachedImageCopy();

			QCOMPARE(copy.name, window.name);
			QCOMPARE(copy.location, QPoint(44, 55));
			QCOMPARE(copy.position, 4);
			QCOMPARE(copy.flags, kMiniWindowAbsoluteLocation);
			QCOMPARE(copy.width, 64);
			QCOMPARE(copy.height, 40);
			QCOMPARE(copy.rect, QRect(44, 55, 64, 40));
			QCOMPARE(copy.backingSurfaceSize(), QSize(64, 40));
			QCOMPARE(copy.backingSurface().pixelColor(0, 0), QColor(Qt::blue));
			QVERIFY(copy.backingSurface().constBits() != window.backingSurface().constBits());
		}

		void detachedImageCopySurvivesSourceMutationAndClear()
		{
			MiniWindow window;
			MiniWindowUtils::create(window, QStringLiteral("snapshot-copy"), 1, 2, 16, 12, 0, 0,
			                        QColor(Qt::black), QStringLiteral("plugin-id"));
			MiniWindowUtils::rectOp(window, 2, 0, 0, 0, 0, MiniWindowUtils::colorToRef(QColor(Qt::green)), 0);
			MiniWindowImage image;
			image.image = QImage(8, 6, QImage::Format_ARGB32_Premultiplied);
			image.image.fill(QColor(Qt::yellow));
			window.images.insert(QStringLiteral("img"), image);

			const MiniWindow copy = window.detachedImageCopy();

			MiniWindowUtils::rectOp(window, 2, 0, 0, 0, 0, MiniWindowUtils::colorToRef(QColor(Qt::red)), 0);
			window.images.clear();
			window.width  = 0;
			window.height = 0;

			QCOMPARE(copy.width, 16);
			QCOMPARE(copy.height, 12);
			QCOMPARE(copy.backingSurface().pixelColor(0, 0), QColor(Qt::green));
			QVERIFY(copy.images.contains(QStringLiteral("img")));
			QCOMPARE(copy.images.value(QStringLiteral("img")).image.pixelColor(0, 0), QColor(Qt::yellow));
		}

		void highDpiFloodFillStopsAtLogicalBorder()
		{
			MiniWindow window;
			MiniWindowUtils::create(window, QStringLiteral("flood-border"), 0, 0, 8, 8, 0, 0,
			                        QColor(Qt::black), QString(), 2.0);
			const long red   = MiniWindowUtils::colorToRef(QColor(Qt::red));
			const long green = MiniWindowUtils::colorToRef(QColor(Qt::green));
			for (int x = 1; x <= 6; ++x)
			{
				MiniWindowUtils::setPixel(window, x, 1, red);
				MiniWindowUtils::setPixel(window, x, 6, red);
			}
			for (int y = 1; y <= 6; ++y)
			{
				MiniWindowUtils::setPixel(window, 1, y, red);
				MiniWindowUtils::setPixel(window, 6, y, red);
			}

			QCOMPARE(MiniWindowUtils::rectOp(window, 6, 2, 2, 0, 0, red, green), eOK);

			QCOMPARE(MiniWindowUtils::pixelValue(window, 2, 2), green);
			QCOMPARE(MiniWindowUtils::pixelValue(window, 5, 5), green);
			QCOMPARE(MiniWindowUtils::pixelValue(window, 1, 1), red);
			QCOMPARE(MiniWindowUtils::pixelValue(window, 0, 0),
			         MiniWindowUtils::colorToRef(QColor(Qt::black)));
		}

		void highDpiFloodFillActionSevenOnlyReplacesBorderColourRuns()
		{
			MiniWindow window;
			MiniWindowUtils::create(window, QStringLiteral("flood-border-only"), 0, 0, 8, 8, 0, 0,
			                        QColor(Qt::black), QString(), 2.0);
			const long red  = MiniWindowUtils::colorToRef(QColor(Qt::red));
			const long blue = MiniWindowUtils::colorToRef(QColor(Qt::blue));
			MiniWindowUtils::setPixel(window, 2, 2, red);
			MiniWindowUtils::setPixel(window, 3, 2, red);
			MiniWindowUtils::setPixel(window, 4, 2, red);

			QCOMPARE(MiniWindowUtils::rectOp(window, 7, 2, 2, 0, 0, red, blue), eOK);

			QCOMPARE(MiniWindowUtils::pixelValue(window, 2, 2), blue);
			QCOMPARE(MiniWindowUtils::pixelValue(window, 3, 2), blue);
			QCOMPARE(MiniWindowUtils::pixelValue(window, 4, 2), blue);
			QCOMPARE(MiniWindowUtils::pixelValue(window, 5, 2),
			         MiniWindowUtils::colorToRef(QColor(Qt::black)));
		}

		void bezierRejectsLegacyInvalidPointList()
		{
			MiniWindow window;
			MiniWindowUtils::create(window, QStringLiteral("test"), 0, 0, 32, 32, 0, 0, QColor(Qt::black),
			                        QString());

			QCOMPARE(MiniWindowUtils::bezier(window, QStringLiteral("0,0,,10,10,20,20,30,30"), 0, 0, 1),
			         eInvalidNumberOfPoints);
			QCOMPARE(MiniWindowUtils::bezier(window, QStringLiteral("0,0,10.5,10,20,20,30,30"), 0, 0, 1),
			         eInvalidPoint);
		}

		void polygonRejectsLegacyInvalidPointList()
		{
			MiniWindow window;
			MiniWindowUtils::create(window, QStringLiteral("test"), 0, 0, 32, 32, 0, 0, QColor(Qt::black),
			                        QString());

			QCOMPARE(
			    MiniWindowUtils::polygon(window, QStringLiteral("0,0,,10,20,20"), 0, 0, 1, 0, 0, true, false),
			    eInvalidPoint);
			QCOMPARE(
			    MiniWindowUtils::polygon(window, QStringLiteral("0,0,10e1,10"), 0, 0, 1, 0, 0, true, false),
			    eInvalidPoint);
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_MiniWindowUtils)

#if __has_include("tst_MiniWindowUtils.moc")
#include "tst_MiniWindowUtils.moc"
#endif
