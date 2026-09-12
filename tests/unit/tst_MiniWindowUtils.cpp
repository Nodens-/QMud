/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_MiniWindowUtils.cpp
 * Role: QTest coverage for MiniWindowUtils behavior.
 */

#include "MiniWindowUtils.h"

#include "ColorPacking.h"
#include "MiniWindow.h"
#include "scripting/ScriptingErrors.h"

#include <QBuffer>
#include <QImage>
#include <QPen>
#include <QtTest/QTest>

#include <array>

namespace
{
	[[nodiscard]] long colourRef(const QColor &colour)
	{
		return static_cast<long>(qmudRgb(colour.red(), colour.green(), colour.blue()));
	}

	[[nodiscard]] MiniWindow makeWindow(const QSize &size, const QColor &background = QColor(Qt::black),
	                                    const double devicePixelRatio = 1.0)
	{
		MiniWindow window;
		MiniWindowUtils::create(window, QStringLiteral("test"), 0, 0, size.width(), size.height(), 0, 0,
		                        background, QString(), devicePixelRatio);
		return window;
	}

	[[nodiscard]] QByteArray encodePng(const QImage &image)
	{
		QByteArray bytes;
		QBuffer    buffer(&bytes);
		if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
			return {};
		return bytes;
	}

	[[nodiscard]] bool installPng(MiniWindow &window, const QString &imageId, const QImage &image,
	                              const bool swapAlpha = false)
	{
		const QByteArray bytes = encodePng(image);
		return !bytes.isEmpty() && MiniWindowUtils::loadImageMemory(window, imageId, bytes, swapAlpha);
	}

	[[nodiscard]] qsizetype changedPixelCount(const MiniWindow &window, const QColor &background)
	{
		qsizetype count = 0;
		for (int y = 0; y < window.height; ++y)
		{
			for (int x = 0; x < window.width; ++x)
			{
				if (window.backingSurface().pixelColor(x, y) != background)
					++count;
			}
		}
		return count;
	}

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
				const QColor color = MiniWindowUtils::colorFromRef(qmudRgb(0x12, 0x34, 0x56));
				QCOMPARE(color.red(), 0x12);
				QCOMPARE(color.green(), 0x34);
				QCOMPARE(color.blue(), 0x56);
				QCOMPARE(static_cast<QMudColorRef>(MiniWindowUtils::colorToRef(color)),
				         qmudRgb(0x12, 0x34, 0x56));
			}

			void colorFromRefOrTransparentHandlesMinusOneAsTransparent()
			{
				const QColor color = MiniWindowUtils::colorFromRefOrTransparent(-1);
				QCOMPARE(color.alpha(), 0);
			}

			void penAndBrushValidationMatchesLegacyRanges()
			{
				for (long style = 0; style <= 6; ++style)
					QCOMPARE(MiniWindowUtils::validatePenStyle(style, 1), eOK);
				for (long style = 1; style <= 4; ++style)
					QCOMPARE(MiniWindowUtils::validatePenStyle(style, 2), ePenStyleNotValid);
				QCOMPARE(MiniWindowUtils::validatePenStyle(7, 1), ePenStyleNotValid);
				QCOMPARE(MiniWindowUtils::validatePenStyle(0x0300, 1), ePenStyleNotValid);
				QCOMPARE(MiniWindowUtils::validatePenStyle(0x3000, 1), ePenStyleNotValid);

				for (long style = 0; style <= 12; ++style)
					QCOMPARE(MiniWindowUtils::validateBrushStyle(style), eOK);
				QCOMPARE(MiniWindowUtils::validateBrushStyle(-1), eBrushStyleNotValid);
				QCOMPARE(MiniWindowUtils::validateBrushStyle(13), eBrushStyleNotValid);
			}

			void penConstructionMapsStyleCapAndJoinBits()
			{
				const QPen dashed =
				    MiniWindowUtils::makePen(qmudRgb(0x11, 0x22, 0x33), 1 | 0x0100 | 0x1000, 0);
				QCOMPARE(dashed.color(), QColor(0x11, 0x22, 0x33));
				QCOMPARE(dashed.width(), 1);
				QCOMPARE(dashed.style(), Qt::DashLine);
				QCOMPARE(dashed.capStyle(), Qt::SquareCap);
				QCOMPARE(dashed.joinStyle(), Qt::BevelJoin);

				const QPen dotted = MiniWindowUtils::makePen(0, 2 | 0x0200 | 0x2000, 3);
				QCOMPARE(dotted.style(), Qt::DotLine);
				QCOMPARE(dotted.capStyle(), Qt::FlatCap);
				QCOMPARE(dotted.joinStyle(), Qt::MiterJoin);
			}

			void brushConstructionCoversSolidTransparentAndPatternStyles()
			{
				bool         ok = false;
				const QBrush solid =
				    MiniWindowUtils::makeBrush(0, colourRef(Qt::red), colourRef(Qt::blue), &ok);
				QVERIFY(ok);
				QCOMPARE(solid.style(), Qt::SolidPattern);
				QCOMPARE(solid.color(), QColor(Qt::blue));

				QCOMPARE(MiniWindowUtils::makeBrush(1, 0, 0, &ok).style(), Qt::NoBrush);
				QVERIFY(ok);
				QCOMPARE(MiniWindowUtils::makeBrush(0, 0, -1, &ok).style(), Qt::NoBrush);
				QVERIFY(ok);

				for (long style = 2; style <= 12; ++style)
				{
					const QBrush pattern =
					    MiniWindowUtils::makeBrush(style, colourRef(Qt::red), colourRef(Qt::blue), &ok);
					QVERIFY(ok);
					QCOMPARE(pattern.style(), Qt::TexturePattern);
					QCOMPARE(pattern.textureImage().size(), QSize(8, 8));
				}

				const QBrush cached =
				    MiniWindowUtils::makeBrush(2, colourRef(Qt::red), colourRef(Qt::blue), &ok);
				QVERIFY(ok);
				QCOMPARE(
				    cached.textureImage(),
				    MiniWindowUtils::makeBrush(2, colourRef(Qt::red), colourRef(Qt::blue)).textureImage());

				QCOMPARE(MiniWindowUtils::makeBrush(13, 0, 0, &ok).style(), Qt::NoBrush);
				QVERIFY(!ok);
			}

			void coordinateAndBitmapHelpersHandleRelativeBoundsAndScanlinePadding()
			{
				const MiniWindow window = makeWindow(QSize(20, 15));
				QCOMPARE(MiniWindowUtils::rectFromCoords(window, 2, 3, 0, 0), QRect(2, 3, 18, 12));
				QCOMPARE(MiniWindowUtils::rectFromCoords(window, 2, 3, -2, -1), QRect(2, 3, 16, 11));
				QCOMPARE(MiniWindowUtils::bytesPerLine(1, 1), 4);
				QCOMPARE(MiniWindowUtils::bytesPerLine(8, 1), 4);
				QCOMPARE(MiniWindowUtils::bytesPerLine(9, 1), 4);
				QCOMPARE(MiniWindowUtils::bytesPerLine(2, 24), 8);
			}

			void positionAndResizeUpdateCanonicalGeometryState()
			{
				MiniWindow window;
				MiniWindowUtils::create(window, QStringLiteral("geom"), 5, 7, 32, 24, 0, 0, QColor(Qt::black),
				                        QString());
				MiniWindowUtils::rectOp(window, 2, 0, 0, 0, 0, MiniWindowUtils::colorToRef(QColor(Qt::red)),
				                        0);

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

			void absolutePositionConstraintPreservesSizeAtClientEdges()
			{
				const QPoint constrained = MiniWindowUtils::constrainCapturedAbsolutePosition(
				    QPoint(950, -20), QSize(120, 80), QSize(1000, 700));

				QCOMPARE(constrained, QPoint(880, 0));
			}

			void absoluteResizeConstraintStopsAtOccupiedRightEdge()
			{
				const QSize constrained = MiniWindowUtils::constrainCapturedAbsoluteResize(
				    QPoint(800, 40), QSize(200, 120), QSize(260, 120), QSize(1000, 700));

				QCOMPARE(constrained, QSize(200, 120));
			}

			void absoluteResizeConstraintKeepsIndependentAxisGrowth()
			{
				const QSize constrained = MiniWindowUtils::constrainCapturedAbsoluteResize(
				    QPoint(800, 40), QSize(200, 120), QSize(260, 180), QSize(1000, 700));

				QCOMPARE(constrained, QSize(200, 180));
			}

			void absoluteResizeConstraintPreservesOversizedCurrentAxis()
			{
				const QSize constrained = MiniWindowUtils::constrainCapturedAbsoluteResize(
				    QPoint(540, 40), QSize(100, 120), QSize(140, 180), QSize(600, 700));

				QCOMPARE(constrained, QSize(100, 180));
			}

			void absoluteGeometryConstraintRejectsOutwardLeftAndTopResize()
			{
				const QRect currentGeometry(0, 0, 100, 80);
				QCOMPARE(MiniWindowUtils::constrainCapturedAbsoluteGeometry(
				             currentGeometry.topLeft(), currentGeometry.size(), QPoint(-20, -10),
				             QSize(120, 90), QSize(1000, 700)),
				         currentGeometry);
				QCOMPARE(MiniWindowUtils::constrainCapturedAbsoluteGeometry(
				             currentGeometry.topLeft(), currentGeometry.size(), QPoint(-20, -10),
				             QSize(140, 110), QSize(1000, 700)),
				         QRect(0, 0, 120, 100));
			}

			void absoluteGeometryConstraintRejectsOutwardRightAndBottomResize()
			{
				const QRect currentGeometry(900, 620, 100, 80);
				QCOMPARE(MiniWindowUtils::constrainCapturedAbsoluteGeometry(
				             currentGeometry.topLeft(), currentGeometry.size(), currentGeometry.topLeft(),
				             QSize(140, 110), QSize(1000, 700)),
				         currentGeometry);
			}

			void absoluteGeometryConstraintClampsMoveWithoutChangingSize()
			{
				const QRect currentGeometry(40, 50, 100, 80);
				QCOMPARE(MiniWindowUtils::constrainCapturedAbsoluteGeometry(
				             currentGeometry.topLeft(), currentGeometry.size(), QPoint(-20, 650),
				             currentGeometry.size(), QSize(1000, 700)),
				         QRect(0, 620, 100, 80));
			}

			void absoluteGeometryConstraintPreservesExistingOverflow()
			{
				const QRect currentGeometry(-20, 0, 1020, 80);
				QCOMPARE(MiniWindowUtils::constrainCapturedAbsoluteGeometry(
				             currentGeometry.topLeft(), currentGeometry.size(), currentGeometry.topLeft(),
				             currentGeometry.size(), QSize(1000, 700)),
				         currentGeometry);
				QCOMPARE(MiniWindowUtils::constrainCapturedAbsoluteGeometry(
				             currentGeometry.topLeft(), currentGeometry.size(), QPoint(-30, 0),
				             QSize(1030, 80), QSize(1000, 700)),
				         currentGeometry);
			}

			void absoluteGeometryConstraintClampsActualMoveAfterExistingOverflow()
			{
				const QRect currentGeometry(-20, 0, 100, 80);
				QCOMPARE(MiniWindowUtils::constrainCapturedAbsoluteGeometry(
				             currentGeometry.topLeft(), currentGeometry.size(), QPoint(-10, 0),
				             currentGeometry.size(), QSize(1000, 700)),
				         QRect(0, 0, 100, 80));
			}

			void absoluteGeometryConstraintClampsChangedEdgesAfterExistingOverflow()
			{
				const QRect currentGeometry(-20, 0, 100, 80);
				QCOMPARE(MiniWindowUtils::constrainCapturedAbsoluteGeometry(
				             currentGeometry.topLeft(), currentGeometry.size(), QPoint(-10, 0),
				             QSize(110, 80), QSize(1000, 700)),
				         QRect(0, 0, 100, 80));
			}

			void absoluteGeometryConstraintPlacesWhollyOutsideRequestAtNearestEdge()
			{
				const QRect currentGeometry(40, 50, 100, 80);
				QCOMPARE(MiniWindowUtils::constrainCapturedAbsoluteGeometry(
				             currentGeometry.topLeft(), currentGeometry.size(), QPoint(1200, 900),
				             QSize(500, 500), QSize(1000, 700)),
				         QRect(900, 620, 100, 80));
			}

			void absoluteGeometryConstraintDoesNotCollapseEdgeTouchingRequest()
			{
				const QRect currentGeometry(40, 50, 100, 80);
				QCOMPARE(MiniWindowUtils::constrainCapturedAbsoluteGeometry(
				             currentGeometry.topLeft(), currentGeometry.size(), QPoint(1000, 50),
				             QSize(500, 80), QSize(1000, 700)),
				         QRect(900, 50, 100, 80));
				QCOMPARE(MiniWindowUtils::constrainCapturedAbsoluteGeometry(
				             currentGeometry.topLeft(), currentGeometry.size(), QPoint(-500, 50),
				             QSize(500, 80), QSize(1000, 700)),
				         QRect(0, 50, 100, 80));
			}

			void absoluteGeometryConstraintKeepsOutsideZeroExtentNonNegative()
			{
				const QRect currentGeometry(40, 50, 100, 80);
				QCOMPARE(MiniWindowUtils::constrainCapturedAbsoluteGeometry(
				             currentGeometry.topLeft(), currentGeometry.size(), QPoint(1200, 50),
				             QSize(0, 80), QSize(1000, 700)),
				         QRect(1000, 50, 0, 80));
				QCOMPARE(MiniWindowUtils::constrainCapturedAbsoluteGeometry(
				             currentGeometry.topLeft(), currentGeometry.size(), QPoint(-200, 50),
				             QSize(0, 80), QSize(1000, 700)),
				         QRect(0, 50, 0, 80));
			}

			void capturedDragPositionUsesGeometryEdges()
			{
				QCOMPARE(MiniWindowUtils::constrainCapturedDragPosition(QPoint(1200, -40), QSize(1000, 700)),
				         QPoint(999, 0));
			}

			void capturedDragPositionConvertsThroughIndependentLayerScales()
			{
				QCOMPARE(MiniWindowUtils::capturedDragPositionToCanonical(QPoint(799, 349), 0.8, 0.5),
				         QPoint(999, 698));
				QCOMPARE(MiniWindowUtils::capturedDragPositionToCanonical(QPoint(799, 349), 1.0, 1.0),
				         QPoint(799, 349));
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
				MiniWindowUtils::create(window, QStringLiteral("hidpi"), 5, 7, 32, 24, 0, 0,
				                        QColor(Qt::black), QString(), 1.5);

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

				QCOMPARE(MiniWindowUtils::pixelValue(window, 2, 3),
				         MiniWindowUtils::colorToRef(QColor(Qt::red)));
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
				MiniWindowUtils::rectOp(window, 2, 0, 0, 0, 0, MiniWindowUtils::colorToRef(QColor(Qt::blue)),
				                        0);

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
				MiniWindowUtils::rectOp(window, 2, 0, 0, 0, 0, MiniWindowUtils::colorToRef(QColor(Qt::green)),
				                        0);
				MiniWindowImage image;
				image.image = QImage(8, 6, QImage::Format_ARGB32_Premultiplied);
				image.image.fill(QColor(Qt::yellow));
				window.images.insert(QStringLiteral("img"), image);

				const MiniWindow copy = window.detachedImageCopy();

				MiniWindowUtils::rectOp(window, 2, 0, 0, 0, 0, MiniWindowUtils::colorToRef(QColor(Qt::red)),
				                        0);
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

			void drawingPrimitivesRenderAndRejectInvalidStyles()
			{
				const QColor background(Qt::black);
				const long   red   = colourRef(Qt::red);
				const long   green = colourRef(Qt::green);

				MiniWindow   lineWindow = makeWindow(QSize(32, 32), background);
				QCOMPARE(MiniWindowUtils::line(lineWindow, 1, 1, 30, 1, red, 0, 1), eOK);
				QVERIFY(changedPixelCount(lineWindow, background) > 0);
				QCOMPARE(MiniWindowUtils::line(lineWindow, 0, 0, 1, 1, red, 1, 2), ePenStyleNotValid);

				MiniWindow arcWindow = makeWindow(QSize(32, 32), background);
				QCOMPARE(MiniWindowUtils::arc(arcWindow, 2, 2, 30, 30, 30, 16, 16, 2, red, 0, 1), eOK);
				QVERIFY(changedPixelCount(arcWindow, background) > 0);

				for (int action = 1; action <= 5; ++action)
				{
					MiniWindow circleWindow = makeWindow(QSize(32, 32), background);
					QCOMPARE(MiniWindowUtils::circleOp(circleWindow, action, 2, 2, 30, 30, red, 0, 1, green,
					                                   0, 30, 16, 16, 2),
					         eOK);
					QVERIFY(changedPixelCount(circleWindow, background) > 0);
				}
				QCOMPARE(
				    MiniWindowUtils::circleOp(lineWindow, 6, 2, 2, 30, 30, red, 0, 1, green, 0, 0, 0, 0, 0),
				    eUnknownOption);
				QCOMPARE(
				    MiniWindowUtils::circleOp(lineWindow, 1, 2, 2, 30, 30, red, 7, 1, green, 0, 0, 0, 0, 0),
				    ePenStyleNotValid);
				QCOMPARE(
				    MiniWindowUtils::circleOp(lineWindow, 1, 2, 2, 30, 30, red, 0, 1, green, 13, 0, 0, 0, 0),
				    eBrushStyleNotValid);

				MiniWindow bezierWindow = makeWindow(QSize(32, 32), background);
				QCOMPARE(
				    MiniWindowUtils::bezier(bezierWindow, QStringLiteral("1,1,8,1,24,30,30,30"), red, 0, 1),
				    eOK);
				QVERIFY(changedPixelCount(bezierWindow, background) > 0);

				MiniWindow polygonWindow = makeWindow(QSize(32, 32), background);
				QCOMPARE(MiniWindowUtils::polygon(polygonWindow, QStringLiteral("2,2,29,2,29,29,2,29"), red,
				                                  0, 1, green, 0, true, true),
				         eOK);
				QCOMPARE(polygonWindow.backingSurface().pixelColor(16, 16), QColor(Qt::green));
				QCOMPARE(MiniWindowUtils::polygon(polygonWindow, QStringLiteral("2,2,29,2,29,29"), red, 0, 1,
				                                  green, 0, false, false),
				         eOK);
			}

			void rectangleAndGradientOperationsProduceExpectedPixels()
			{
				const long red   = colourRef(Qt::red);
				const long green = colourRef(Qt::green);
				const long blue  = colourRef(Qt::blue);

				MiniWindow window = makeWindow(QSize(16, 16), QColor(10, 20, 30));
				QCOMPARE(MiniWindowUtils::rectOp(window, 1, 1, 1, 8, 8, red, 0), eOK);
				QCOMPARE(window.backingSurface().pixelColor(1, 1), QColor(Qt::red));
				QCOMPARE(MiniWindowUtils::rectOp(window, 2, 2, 2, 7, 7, green, 0), eOK);
				QCOMPARE(window.backingSurface().pixelColor(4, 4), QColor(Qt::green));
				QCOMPARE(MiniWindowUtils::rectOp(window, 3, 2, 2, 7, 7, 0, 0), eOK);
				QCOMPARE(window.backingSurface().pixelColor(4, 4), QColor(255, 0, 255));
				QCOMPARE(MiniWindowUtils::rectOp(window, 4, 0, 0, 16, 16, red, blue), eOK);
				QCOMPARE(window.backingSurface().pixelColor(0, 0), QColor(Qt::red));
				QCOMPARE(window.backingSurface().pixelColor(15, 15), QColor(Qt::blue));
				QCOMPARE(MiniWindowUtils::rectOp(window, 5, 1, 1, 15, 15, 4, 0x0F), eBadParameter);
				QCOMPARE(MiniWindowUtils::rectOp(window, 5, 1, 1, 15, 15, 5, 0x20), eBadParameter);
				QCOMPARE(MiniWindowUtils::rectOp(window, 5, 1, 1, 15, 15, 5, 0x080F), eOK);
				QCOMPARE(MiniWindowUtils::rectOp(window, 8, 0, 0, 1, 1, 0, 0), eUnknownOption);

				MiniWindow horizontal = makeWindow(QSize(17, 5));
				QCOMPARE(MiniWindowUtils::gradient(horizontal, 0, 0, 17, 5, red, blue, 1), eOK);
				QVERIFY(horizontal.backingSurface().pixelColor(1, 2).red() >
				        horizontal.backingSurface().pixelColor(15, 2).red());
				QVERIFY(horizontal.backingSurface().pixelColor(1, 2).blue() <
				        horizontal.backingSurface().pixelColor(15, 2).blue());

				MiniWindow vertical = makeWindow(QSize(5, 17));
				QCOMPARE(MiniWindowUtils::gradient(vertical, 0, 0, 5, 17, red, blue, 2), eOK);
				QVERIFY(vertical.backingSurface().pixelColor(2, 1).red() >
				        vertical.backingSurface().pixelColor(2, 15).red());

				MiniWindow texture = makeWindow(QSize(8, 8), Qt::white);
				QCOMPARE(MiniWindowUtils::gradient(texture, 0, 0, 8, 8, red, blue, 3), eOK);
				QCOMPARE(texture.backingSurface().pixelColor(0, 0), QColor(Qt::black));
				QCOMPARE(texture.backingSurface().pixelColor(1, 0), QColor(Qt::red));
				QCOMPARE(MiniWindowUtils::gradient(texture, 0, 0, 8, 8, red, blue, 4), eUnknownOption);
			}

			void memoryImagesPreserveMetadataAndOptionalAlphaSwap()
			{
				MiniWindow window = makeWindow(QSize(8, 8));
				QImage     image(2, 2, QImage::Format_ARGB32);
				image.fill(qRgba(10, 20, 30, 40));
				image.setPixel(1, 0, qRgba(50, 60, 70, 80));
				const QByteArray png = encodePng(image);
				QVERIFY(!png.isEmpty());

				QVERIFY(MiniWindowUtils::loadImageMemory(window, QStringLiteral("plain"), png, false));
				QCOMPARE(MiniWindowUtils::imageInfo(window, QStringLiteral("plain"), 2).toInt(), 2);
				QCOMPARE(MiniWindowUtils::imageInfo(window, QStringLiteral("plain"), 3).toInt(), 2);
				QCOMPARE(MiniWindowUtils::imageInfo(window, QStringLiteral("plain"), 4).toInt(), 8);
				QCOMPARE(MiniWindowUtils::imageInfo(window, QStringLiteral("plain"), 5).toInt(), 1);
				QCOMPARE(MiniWindowUtils::imageInfo(window, QStringLiteral("plain"), 6).toInt(), 32);
				QCOMPARE(window.images.value(QStringLiteral("plain")).source, QStringLiteral("<memory>"));
				QCOMPARE(window.images.value(QStringLiteral("plain")).image.pixelColor(0, 0),
				         QColor(10, 20, 30, 40));

				QVERIFY(MiniWindowUtils::loadImageMemory(window, QStringLiteral("cached"), png, false));
				QCOMPARE(window.images.value(QStringLiteral("cached")).image, image);
				QVERIFY(MiniWindowUtils::loadImageMemory(window, QStringLiteral("swapped"), png, true));
				QCOMPARE(window.images.value(QStringLiteral("swapped")).image.pixelColor(0, 0),
				         QColor(20, 30, 40, 10));
				QVERIFY(!MiniWindowUtils::loadImageMemory(window, QStringLiteral("invalid"),
				                                          QByteArrayLiteral("not a png"), false));
				QVERIFY(!window.images.contains(QStringLiteral("invalid")));
				QVERIFY(!MiniWindowUtils::imageInfo(window, QStringLiteral("missing"), 2).isValid());
				QVERIFY(!MiniWindowUtils::imageInfo(window, QStringLiteral("plain"), 99).isValid());
			}

			void generatedAndCopiedImagesRetainLogicalPixelData()
			{
				MiniWindow source = makeWindow(QSize(4, 3), QColor(Qt::blue), 2.0);
				MiniWindowUtils::setPixel(source, 1, 1, colourRef(Qt::red));
				MiniWindow destination = makeWindow(QSize(8, 8));

				MiniWindowUtils::imageFromWindow(destination, QStringLiteral("copy"), source);
				const MiniWindowImage copied = destination.images.value(QStringLiteral("copy"));
				QCOMPARE(copied.image.size(), QSize(4, 3));
				QCOMPARE(copied.image.pixelColor(1, 1), QColor(Qt::red));
				QCOMPARE(copied.source, source.name);
				QCOMPARE(copied.bitmapBitsPixel, 24);

				MiniWindowUtils::createImage(destination, QStringLiteral("bits"), 0x80, 0x40, 0x20, 0x10,
				                             0x08, 0x04, 0x02, 0x01);
				const MiniWindowImage generated = destination.images.value(QStringLiteral("bits"));
				QCOMPARE(generated.image.size(), QSize(8, 8));
				QVERIFY(generated.monochrome);
				QCOMPARE(generated.bitmapBitsPixel, 1);
				QCOMPARE(generated.bitmapWidthBytes, 2);
				QCOMPARE(generated.image.pixelColor(0, 0), QColor(Qt::white));
				QCOMPARE(generated.image.pixelColor(1, 0), QColor(Qt::black));
				QCOMPARE(generated.image.pixelColor(7, 7), QColor(Qt::white));
			}

			void imageDrawingModesRespectScalingColourKeyAndAlpha()
			{
				QImage opaque(2, 2, QImage::Format_RGB32);
				opaque.fill(Qt::magenta);
				opaque.setPixelColor(1, 0, Qt::green);
				opaque.setPixelColor(0, 1, Qt::red);
				opaque.setPixelColor(1, 1, Qt::yellow);

				MiniWindow direct = makeWindow(QSize(8, 8), Qt::blue);
				QVERIFY(installPng(direct, QStringLiteral("image"), opaque));
				QCOMPARE(
				    MiniWindowUtils::drawImage(direct, QStringLiteral("image"), 2, 3, 0, 0, 1, 0, 0, 0, 0),
				    eOK);
				QCOMPARE(direct.backingSurface().pixelColor(3, 3), QColor(Qt::green));

				MiniWindow scaled = makeWindow(QSize(8, 8), Qt::black);
				QVERIFY(installPng(scaled, QStringLiteral("image"), opaque));
				QCOMPARE(
				    MiniWindowUtils::drawImage(scaled, QStringLiteral("image"), 1, 1, 7, 7, 2, 0, 0, 0, 0),
				    eOK);
				QCOMPARE(scaled.backingSurface().pixelColor(1, 1), QColor(Qt::magenta));
				QCOMPARE(scaled.backingSurface().pixelColor(6, 6), QColor(Qt::yellow));

				MiniWindow keyed = makeWindow(QSize(8, 8), Qt::blue);
				QVERIFY(installPng(keyed, QStringLiteral("image"), opaque));
				QCOMPARE(
				    MiniWindowUtils::drawImage(keyed, QStringLiteral("image"), 2, 2, 0, 0, 3, 0, 0, 0, 0),
				    eOK);
				QCOMPARE(keyed.backingSurface().pixelColor(2, 2), QColor(Qt::blue));
				QCOMPARE(keyed.backingSurface().pixelColor(3, 2), QColor(Qt::green));
				QCOMPARE(
				    MiniWindowUtils::drawImage(keyed, QStringLiteral("missing"), 0, 0, 1, 1, 1, 0, 0, 1, 1),
				    eImageNotInstalled);
				QCOMPARE(
				    MiniWindowUtils::drawImage(keyed, QStringLiteral("image"), 0, 0, 1, 1, 4, 0, 0, 1, 1),
				    eBadParameter);

				QImage alpha(1, 1, QImage::Format_ARGB32);
				alpha.fill(qRgba(255, 0, 0, 128));
				MiniWindow alphaWindow = makeWindow(QSize(2, 2), Qt::blue);
				QVERIFY(installPng(alphaWindow, QStringLiteral("alpha"), alpha));
				QCOMPARE(MiniWindowUtils::drawImageAlpha(alphaWindow, QStringLiteral("alpha"), 0, 0, 1, 1,
				                                         1.0, 0, 0),
				         eOK);
				QCOMPARE(alphaWindow.backingSurface().pixelColor(0, 0), QColor(128, 0, 127));
				QCOMPARE(MiniWindowUtils::drawImageAlpha(alphaWindow, QStringLiteral("alpha"), 0, 0, 1, 1,
				                                         -0.1, 0, 0),
				         eBadParameter);
			}

			void imageAlphaExtractionAndMaskMergeUseSourceChannels()
			{
				QImage alpha(2, 1, QImage::Format_ARGB32);
				alpha.setPixel(0, 0, qRgba(10, 20, 30, 64));
				alpha.setPixel(1, 0, qRgba(40, 50, 60, 192));
				MiniWindow extracted = makeWindow(QSize(2, 1), Qt::black);
				QVERIFY(installPng(extracted, QStringLiteral("alpha"), alpha));
				QCOMPARE(MiniWindowUtils::getImageAlpha(extracted, QStringLiteral("alpha"), 0, 0, 2, 1, 0, 0),
				         eOK);
				QCOMPARE(extracted.backingSurface().pixelColor(0, 0), QColor(64, 64, 64));
				QCOMPARE(extracted.backingSurface().pixelColor(1, 0), QColor(192, 192, 192));

				QImage source(1, 1, QImage::Format_RGB32);
				source.fill(Qt::red);
				QImage mask(1, 1, QImage::Format_RGB32);
				mask.fill(Qt::white);
				MiniWindow merged = makeWindow(QSize(1, 1), Qt::blue);
				QVERIFY(installPng(merged, QStringLiteral("source"), source));
				QVERIFY(installPng(merged, QStringLiteral("mask"), mask));
				QCOMPARE(MiniWindowUtils::mergeImageAlpha(merged, QStringLiteral("source"),
				                                          QStringLiteral("mask"), 0, 0, 1, 1, 0, 0.5, 0, 0, 1,
				                                          1),
				         eOK);
				QCOMPARE(merged.backingSurface().pixelColor(0, 0), QColor(128, 0, 128));
				QCOMPARE(MiniWindowUtils::mergeImageAlpha(merged, QStringLiteral("source"),
				                                          QStringLiteral("mask"), 0, 0, 1, 1, 2, 1.0, 0, 0, 1,
				                                          1),
				         eUnknownOption);
				QCOMPARE(MiniWindowUtils::mergeImageAlpha(merged, QStringLiteral("missing"),
				                                          QStringLiteral("mask"), 0, 0, 1, 1, 0, 1.0, 0, 0, 1,
				                                          1),
				         eImageNotInstalled);
			}

			void blendImageMatchesLegacyModeResults()
			{
				constexpr QColor blendColour(192, 96, 32);
				constexpr QColor baseColour(64, 128, 224);
				QImage           blendImage(1, 1, QImage::Format_RGB32);
				blendImage.fill(blendColour);
				MiniWindow window = makeWindow(QSize(1, 1), baseColour);
				QVERIFY(installPng(window, QStringLiteral("blend"), blendImage));

				constexpr std::array<QMudColorRef, 59> expected = {
				    qmudRgb(192, 96, 32),   qmudRgb(128, 112, 128), qmudRgb(129, 104, 128),
				    qmudRgb(192, 96, 32),   qmudRgb(64, 96, 32),    qmudRgb(48, 48, 28),
				    qmudRgb(1, 173, 7),     qmudRgb(1, 0, 1),       qmudRgb(3, 193, 1),
				    qmudRgb(1, 0, 1),       qmudRgb(192, 128, 224), qmudRgb(208, 177, 228),
				    qmudRgb(255, 206, 255), qmudRgb(255, 224, 255), qmudRgb(255, 193, 255),
				    qmudRgb(255, 224, 255), qmudRgb(96, 97, 201),   qmudRgb(88, 112, 203),
				    qmudRgb(161, 96, 56),   qmudRgb(129, 86, 131),  qmudRgb(192, 65, 33),
				    qmudRgb(128, 128, 64),  qmudRgb(255, 0, 255),   qmudRgb(128, 32, 192),
				    qmudRgb(160, 128, 200), qmudRgb(193, 72, 33),   qmudRgb(65, 103, 225),
				    qmudRgb(65, 87, 225),   qmudRgb(193, 58, 33),   qmudRgb(254, 224, 254),
				    qmudRgb(127, 223, 63),  qmudRgb(192, 64, 32),   qmudRgb(128, 224, 192),
				    qmudRgb(64, 0, 32),     qmudRgb(192, 224, 224), qmudRgb(192, 128, 224),
				    qmudRgb(64, 96, 224),   qmudRgb(64, 128, 32),   qmudRgb(192, 96, 224),
				    qmudRgb(64, 96, 32),    qmudRgb(192, 128, 32),  qmudRgb(192, 96, 32),
				    qmudRgb(192, 32, 32),   qmudRgb(192, 96, 32),   qmudRgb(192, 96, 32),
				    qmudRgb(192, 96, 32),   qmudRgb(192, 96, 32),   qmudRgb(96, 96, 32),
				    qmudRgb(32, 96, 32),    qmudRgb(64, 96, 32),    qmudRgb(192, 0, 0),
				    qmudRgb(0, 96, 0),      qmudRgb(0, 0, 32),      qmudRgb(0, 96, 32),
				    qmudRgb(192, 0, 32),    qmudRgb(192, 96, 0),    qmudRgb(192, 192, 192),
				    qmudRgb(96, 96, 96),    qmudRgb(32, 32, 32)};

				for (int mode = 1; mode <= static_cast<int>(expected.size()); ++mode)
				{
					MiniWindowUtils::rectOp(window, 2, 0, 0, 0, 0, colourRef(baseColour), 0);
					QCOMPARE(MiniWindowUtils::blendImage(window, QStringLiteral("blend"), 0, 0, 1, 1, mode,
					                                     1.0, 0, 0, 1, 1, [] { return 0.0; }),
					         eOK);
					const auto actual = static_cast<QMudColorRef>(MiniWindowUtils::pixelValue(window, 0, 0));
					QVERIFY2(
					    actual == expected[static_cast<size_t>(mode - 1)],
					    qPrintable(QStringLiteral("blend mode %1: expected 0x%2, got 0x%3")
					                   .arg(mode)
					                   .arg(expected[static_cast<size_t>(mode - 1)], 6, 16, QLatin1Char('0'))
					                   .arg(actual, 6, 16, QLatin1Char('0'))));
				}
			}

			void blendImageHandlesHslModesOpacityRandomnessAndErrors()
			{
				QImage red(1, 1, QImage::Format_RGB32);
				red.fill(Qt::red);
				MiniWindow window = makeWindow(QSize(1, 1), Qt::blue);
				QVERIFY(installPng(window, QStringLiteral("blend"), red));
				constexpr std::array<QMudColorRef, 5> expected = {qmudRgb(255, 0, 0), qmudRgb(0, 0, 255),
				                                                  qmudRgb(255, 0, 0), qmudRgb(0, 0, 255),
				                                                  qmudRgb(0, 255, 128)};
				for (int mode = 60; mode <= 64; ++mode)
				{
					MiniWindowUtils::rectOp(window, 2, 0, 0, 0, 0, colourRef(Qt::blue), 0);
					QCOMPARE(MiniWindowUtils::blendImage(window, QStringLiteral("blend"), 0, 0, 1, 1, mode,
					                                     1.0, 0, 0, 1, 1, {}),
					         eOK);
					QCOMPARE(static_cast<QMudColorRef>(MiniWindowUtils::pixelValue(window, 0, 0)),
					         expected[static_cast<size_t>(mode - 60)]);
				}

				QImage arbitraryBlend(1, 1, QImage::Format_RGB32);
				arbitraryBlend.fill(QColor(192, 96, 32));
				QVERIFY(installPng(window, QStringLiteral("blend"), arbitraryBlend));
				constexpr QColor                      arbitraryBase(64, 128, 224);
				constexpr std::array<QMudColorRef, 5> arbitraryExpected = {
				    qmudRgb(224, 128, 64), qmudRgb(65, 128, 223), qmudRgb(223, 128, 65), qmudRgb(31, 96, 193),
				    qmudRgb(17, 182, 112)};
				for (int mode = 60; mode <= 64; ++mode)
				{
					MiniWindowUtils::rectOp(window, 2, 0, 0, 0, 0, colourRef(arbitraryBase), 0);
					QCOMPARE(MiniWindowUtils::blendImage(window, QStringLiteral("blend"), 0, 0, 1, 1, mode,
					                                     1.0, 0, 0, 1, 1, {}),
					         eOK);
					QCOMPARE(static_cast<QMudColorRef>(MiniWindowUtils::pixelValue(window, 0, 0)),
					         arbitraryExpected[static_cast<size_t>(mode - 60)]);
				}

				QImage greyBlend(1, 1, QImage::Format_RGB32);
				greyBlend.fill(QColor(80, 80, 80));
				QVERIFY(installPng(window, QStringLiteral("blend"), greyBlend));
				constexpr QColor                      greyBase(10, 100, 200);
				constexpr std::array<QMudColorRef, 5> greyExpected = {
				    qmudRgb(200, 10, 10), qmudRgb(105, 105, 105), qmudRgb(105, 105, 105), qmudRgb(8, 76, 152),
				    qmudRgb(0, 0, 80)};
				for (int mode = 60; mode <= 64; ++mode)
				{
					MiniWindowUtils::rectOp(window, 2, 0, 0, 0, 0, colourRef(greyBase), 0);
					QCOMPARE(MiniWindowUtils::blendImage(window, QStringLiteral("blend"), 0, 0, 1, 1, mode,
					                                     1.0, 0, 0, 1, 1, {}),
					         eOK);
					QCOMPARE(static_cast<QMudColorRef>(MiniWindowUtils::pixelValue(window, 0, 0)),
					         greyExpected[static_cast<size_t>(mode - 60)]);
				}

				QImage green(1, 1, QImage::Format_RGB32);
				green.fill(Qt::green);
				QVERIFY(installPng(window, QStringLiteral("blend"), green));
				constexpr std::array<QMudColorRef, 5> greenExpected = {qmudRgb(0, 255, 0), qmudRgb(255, 0, 0),
				                                                       qmudRgb(0, 255, 0), qmudRgb(255, 0, 0),
				                                                       qmudRgb(85, 255, 128)};
				for (int mode = 60; mode <= 64; ++mode)
				{
					MiniWindowUtils::rectOp(window, 2, 0, 0, 0, 0, colourRef(Qt::red), 0);
					QCOMPARE(MiniWindowUtils::blendImage(window, QStringLiteral("blend"), 0, 0, 1, 1, mode,
					                                     1.0, 0, 0, 1, 1, {}),
					         eOK);
					QCOMPARE(static_cast<QMudColorRef>(MiniWindowUtils::pixelValue(window, 0, 0)),
					         greenExpected[static_cast<size_t>(mode - 60)]);
				}

				QVERIFY(installPng(window, QStringLiteral("blend"), red));
				MiniWindowUtils::rectOp(window, 2, 0, 0, 0, 0, colourRef(Qt::blue), 0);
				QCOMPARE(MiniWindowUtils::blendImage(window, QStringLiteral("blend"), 0, 0, 1, 1, 1, 0.5, 0,
				                                     0, 1, 1, {}),
				         eOK);
				QCOMPARE(window.backingSurface().pixelColor(0, 0), QColor(128, 0, 128));

				MiniWindowUtils::rectOp(window, 2, 0, 0, 0, 0, colourRef(Qt::blue), 0);
				QCOMPARE(MiniWindowUtils::blendImage(window, QStringLiteral("blend"), 0, 0, 1, 1, 4, 0.5, 0,
				                                     0, 1, 1, [] { return 0.75; }),
				         eOK);
				QCOMPARE(window.backingSurface().pixelColor(0, 0), QColor(Qt::blue));
				QCOMPARE(MiniWindowUtils::blendImage(window, QStringLiteral("blend"), 0, 0, 1, 1, 4, 0.5, 0,
				                                     0, 1, 1, [] { return 0.25; }),
				         eOK);
				QCOMPARE(window.backingSurface().pixelColor(0, 0), QColor(Qt::red));

				QCOMPARE(MiniWindowUtils::blendImage(window, QStringLiteral("missing"), 0, 0, 1, 1, 1, 1.0, 0,
				                                     0, 1, 1, {}),
				         eImageNotInstalled);
				QCOMPARE(MiniWindowUtils::blendImage(window, QStringLiteral("blend"), 0, 0, 1, 1, 0, 1.0, 0,
				                                     0, 1, 1, {}),
				         eUnknownOption);
				QCOMPARE(MiniWindowUtils::blendImage(window, QStringLiteral("blend"), 0, 0, 1, 1, 1, 1.1, 0,
				                                     0, 1, 1, {}),
				         eBadParameter);
			}

			void filtersApplyDeterministicNoiseAndChannelTransforms()
			{
				MiniWindow noise = makeWindow(QSize(2, 2), QColor(100, 100, 100));
				QCOMPARE(MiniWindowUtils::filter(noise, 0, 0, 2, 2, 1, 20.0, [] { return 1.0; }), eOK);
				QCOMPARE(noise.backingSurface().pixelColor(0, 0), QColor(74, 74, 74));

				MiniWindow monoNoise = makeWindow(QSize(2, 2), QColor(100, 120, 140));
				QCOMPARE(MiniWindowUtils::filter(monoNoise, 0, 0, 2, 2, 2, 20.0, [] { return 0.0; }), eOK);
				QCOMPARE(monoNoise.backingSurface().pixelColor(0, 0), QColor(126, 146, 166));

				MiniWindow brightness = makeWindow(QSize(2, 2), QColor(100, 120, 140));
				QCOMPARE(MiniWindowUtils::filter(brightness, 0, 0, 2, 2, 7, 20.0, {}), eOK);
				QCOMPARE(brightness.backingSurface().pixelColor(0, 0), QColor(120, 140, 160));

				MiniWindow redOnly = makeWindow(QSize(2, 2), QColor(100, 120, 140));
				QCOMPARE(MiniWindowUtils::filter(redOnly, 0, 0, 2, 2, 10, 20.0, {}), eOK);
				QCOMPARE(redOnly.backingSurface().pixelColor(0, 0), QColor(120, 120, 140));

				MiniWindow greenOnly = makeWindow(QSize(2, 2), QColor(100, 120, 140));
				QCOMPARE(MiniWindowUtils::filter(greenOnly, 0, 0, 2, 2, 13, 20.0, {}), eOK);
				QCOMPARE(greenOnly.backingSurface().pixelColor(0, 0), QColor(100, 140, 140));

				MiniWindow blueOnly = makeWindow(QSize(2, 2), QColor(100, 120, 140));
				QCOMPARE(MiniWindowUtils::filter(blueOnly, 0, 0, 2, 2, 16, 20.0, {}), eOK);
				QCOMPARE(blueOnly.backingSurface().pixelColor(0, 0), QColor(100, 120, 160));

				MiniWindow greyscale = makeWindow(QSize(2, 2), QColor(30, 90, 150));
				QCOMPARE(MiniWindowUtils::filter(greyscale, 0, 0, 2, 2, 20, 0.0, {}), eOK);
				QCOMPARE(greyscale.backingSurface().pixelColor(0, 0), QColor(78, 78, 78));

				QCOMPARE(MiniWindowUtils::filter(greyscale, 0, 0, 2, 2, 28, 0.0, {}), eUnknownOption);
			}

			void remainingFiltersProduceExactResults()
			{
				struct PixelFilterCase
				{
						int    operation{0};
						double options{0.0};
						QColor input{};
						QColor expected{};
				};
				constexpr std::array pixelCases = {
				    PixelFilterCase{8,  1.5, QColor(80, 100, 120), QColor(56,  86,  116)},
				    PixelFilterCase{9,  2.0, QColor(80, 100, 120), QColor(25,  39,  56) },
				    PixelFilterCase{11, 1.5, QColor(80, 100, 120), QColor(56,  100, 120)},
				    PixelFilterCase{12, 2.0, QColor(80, 100, 120), QColor(25,  100, 120)},
				    PixelFilterCase{14, 1.5, QColor(80, 100, 120), QColor(80,  86,  120)},
				    PixelFilterCase{15, 2.0, QColor(80, 100, 120), QColor(80,  39,  120)},
				    PixelFilterCase{17, 1.5, QColor(80, 100, 120), QColor(80,  100, 116)},
				    PixelFilterCase{18, 2.0, QColor(80, 100, 120), QColor(80,  100, 56) },
				    PixelFilterCase{19, 0.0, QColor(80, 100, 120), QColor(100, 100, 100)},
				    PixelFilterCase{21, 1.5, QColor(81, 101, 121), QColor(122, 152, 182)},
				    PixelFilterCase{22, 1.5, QColor(81, 101, 121), QColor(122, 101, 121)},
				    PixelFilterCase{23, 1.5, QColor(81, 101, 121), QColor(81,  152, 121)},
				    PixelFilterCase{24, 1.5, QColor(81, 101, 121), QColor(81,  101, 182)},
				};
				for (const PixelFilterCase &filterCase : pixelCases)
				{
					MiniWindow window = makeWindow(QSize(1, 1), filterCase.input);
					QCOMPARE(MiniWindowUtils::filter(window, 0, 0, 1, 1, filterCase.operation,
					                                 filterCase.options, {}),
					         eOK);
					const QColor actual = window.backingSurface().pixelColor(0, 0);
					QVERIFY2(actual == filterCase.expected,
					         qPrintable(QStringLiteral("filter operation %1: expected %2, got %3")
					                        .arg(filterCase.operation)
					                        .arg(filterCase.expected.name(QColor::HexArgb),
					                             actual.name(QColor::HexArgb))));
				}

				struct ConvolutionCase
				{
						int operation{0};
						int expected{0};
				};
				constexpr std::array convolutionCases = {
				    ConvolutionCase{3,  46},
                    ConvolutionCase{4,  56},
                    ConvolutionCase{5,  55},
				    ConvolutionCase{6,  41},
                    ConvolutionCase{25, 64},
                    ConvolutionCase{26, 61},
				};
				for (const ConvolutionCase &filterCase : convolutionCases)
				{
					constexpr std::array<int, 5> sourceValues = {10, 61, 50, 81, 30};
					MiniWindow                   horizontal   = makeWindow(QSize(5, 1));
					for (int x = 0; x < static_cast<int>(sourceValues.size()); ++x)
					{
						const int value = sourceValues[static_cast<size_t>(x)];
						MiniWindowUtils::setPixel(horizontal, x, 0, colourRef(QColor(value, value, value)));
					}
					QCOMPARE(MiniWindowUtils::filter(horizontal, 0, 0, 5, 1, filterCase.operation, 1.0, {}),
					         eOK);
					QCOMPARE(horizontal.backingSurface().pixelColor(2, 0),
					         QColor(filterCase.expected, filterCase.expected, filterCase.expected));

					MiniWindow vertical = makeWindow(QSize(1, 5));
					for (int y = 0; y < static_cast<int>(sourceValues.size()); ++y)
					{
						const int value = sourceValues[static_cast<size_t>(y)];
						MiniWindowUtils::setPixel(vertical, 0, y, colourRef(QColor(value, value, value)));
					}
					QCOMPARE(MiniWindowUtils::filter(vertical, 0, 0, 1, 5, filterCase.operation, 2.0, {}),
					         eOK);
					QCOMPARE(vertical.backingSurface().pixelColor(0, 2),
					         QColor(filterCase.expected, filterCase.expected, filterCase.expected));
				}

				constexpr std::array<std::array<int, 5>, 5> twoPassSource = {
				    std::array{0, 0, 0, 4,  0},
                    std::array{0, 0, 2, 29, 0},
                    std::array{0, 0, 0, 20, 0},
				    std::array{0, 0, 2, 37, 0},
                    std::array{0, 0, 0, 12, 0},
				};
				MiniWindow twoPass = makeWindow(QSize(5, 5));
				for (int y = 0; y < static_cast<int>(twoPassSource.size()); ++y)
				{
					for (int x = 0; x < static_cast<int>(twoPassSource[static_cast<size_t>(y)].size()); ++x)
					{
						const int value = twoPassSource[static_cast<size_t>(y)][static_cast<size_t>(x)];
						MiniWindowUtils::setPixel(twoPass, x, y, colourRef(QColor(value, value, value)));
					}
				}
				QCOMPARE(MiniWindowUtils::filter(twoPass, 0, 0, 5, 5, 5, 0.0, {}), eOK);
				QCOMPARE(twoPass.backingSurface().pixelColor(2, 2), QColor(55, 55, 55));

				MiniWindow average = makeWindow(QSize(2, 1));
				MiniWindowUtils::setPixel(average, 0, 0, colourRef(QColor(10, 20, 30)));
				MiniWindowUtils::setPixel(average, 1, 0, colourRef(QColor(50, 80, 110)));
				QCOMPARE(MiniWindowUtils::filter(average, 0, 0, 2, 1, 27, 0.0, {}), eOK);
				QCOMPARE(average.backingSurface().pixelColor(0, 0), QColor(30, 50, 70));
				QCOMPARE(average.backingSurface().pixelColor(1, 0), QColor(30, 50, 70));
			}

			void horizontalConvolutionClampsPixelsWithoutMixingChannels()
			{
				constexpr std::array source   = {QColor(10, 20, 30), QColor(40, 50, 60), QColor(70, 80, 90),
				                                 QColor(100, 110, 120), QColor(130, 140, 150)};
				constexpr std::array expected = {QColor(28, 38, 48), QColor(46, 56, 66), QColor(70, 80, 90),
				                                 QColor(94, 104, 114), QColor(112, 122, 132)};

				MiniWindow           window = makeWindow(QSize(static_cast<int>(source.size()), 1));
				for (int x = 0; x < static_cast<int>(source.size()); ++x)
					MiniWindowUtils::setPixel(window, x, 0, colourRef(source[static_cast<size_t>(x)]));

				QCOMPARE(MiniWindowUtils::filter(window, 0, 0, window.width, 1, 3, 1.0, {}), eOK);
				for (int x = 0; x < static_cast<int>(expected.size()); ++x)
					QCOMPARE(window.backingSurface().pixelColor(x, 0), expected[static_cast<size_t>(x)]);
			}

			void hotspotLifecycleEnforcesPluginOwnershipAndMaintainsMetadata()
			{
				MiniWindow window = makeWindow(QSize(40, 30));
				QCOMPARE(MiniWindowUtils::addHotspot(window, QStringLiteral("hot"), 2, 3, 20, 15,
				                                     QStringLiteral("over"), QStringLiteral("cancel-over"),
				                                     QStringLiteral("down"), QStringLiteral("cancel-down"),
				                                     QStringLiteral("up"), QStringLiteral("tip"), 7, 9,
				                                     QStringLiteral("plugin-a")),
				         eOK);
				QCOMPARE(window.callbackPlugin, QStringLiteral("plugin-a"));
				QCOMPARE(MiniWindowUtils::hotspotInfo(window, QStringLiteral("hot"), 1).toInt(), 2);
				QCOMPARE(MiniWindowUtils::hotspotInfo(window, QStringLiteral("hot"), 2).toInt(), 3);
				QCOMPARE(MiniWindowUtils::hotspotInfo(window, QStringLiteral("hot"), 3).toInt(), 19);
				QCOMPARE(MiniWindowUtils::hotspotInfo(window, QStringLiteral("hot"), 4).toInt(), 14);
				QCOMPARE(MiniWindowUtils::hotspotInfo(window, QStringLiteral("hot"), 5).toString(),
				         QStringLiteral("over"));
				QCOMPARE(MiniWindowUtils::hotspotInfo(window, QStringLiteral("hot"), 6).toString(),
				         QStringLiteral("cancel-over"));
				QCOMPARE(MiniWindowUtils::hotspotInfo(window, QStringLiteral("hot"), 7).toString(),
				         QStringLiteral("down"));
				QCOMPARE(MiniWindowUtils::hotspotInfo(window, QStringLiteral("hot"), 8).toString(),
				         QStringLiteral("cancel-down"));
				QCOMPARE(MiniWindowUtils::hotspotInfo(window, QStringLiteral("hot"), 9).toString(),
				         QStringLiteral("up"));
				QCOMPARE(MiniWindowUtils::hotspotInfo(window, QStringLiteral("hot"), 10).toString(),
				         QStringLiteral("tip"));
				QCOMPARE(MiniWindowUtils::hotspotInfo(window, QStringLiteral("hot"), 11).toInt(), 7);
				QCOMPARE(MiniWindowUtils::hotspotInfo(window, QStringLiteral("hot"), 12).toInt(), 9);

				QCOMPARE(MiniWindowUtils::setHotspotTooltip(window, QStringLiteral("hot"),
				                                            QStringLiteral("updated")),
				         eOK);
				QCOMPARE(MiniWindowUtils::moveHotspot(window, QStringLiteral("hot"), 4, 5, 24, 20), eOK);
				QCOMPARE(MiniWindowUtils::setHotspotDragHandler(
				             window, QStringLiteral("hot"), QStringLiteral("move"), QStringLiteral("release"),
				             12, QStringLiteral("plugin-a")),
				         eOK);
				QCOMPARE(MiniWindowUtils::setHotspotScrollwheelHandler(window, QStringLiteral("hot"),
				                                                       QStringLiteral("wheel"),
				                                                       QStringLiteral("plugin-a")),
				         eOK);
				QCOMPARE(MiniWindowUtils::hotspotInfo(window, QStringLiteral("hot"), 10).toString(),
				         QStringLiteral("updated"));
				QCOMPARE(MiniWindowUtils::hotspotInfo(window, QStringLiteral("hot"), 13).toString(),
				         QStringLiteral("move"));
				QCOMPARE(MiniWindowUtils::hotspotInfo(window, QStringLiteral("hot"), 14).toString(),
				         QStringLiteral("release"));
				QCOMPARE(MiniWindowUtils::hotspotInfo(window, QStringLiteral("hot"), 15).toInt(), 12);
				QCOMPARE(window.hotspots.value(QStringLiteral("hot")).scrollwheelCallback,
				         QStringLiteral("wheel"));

				QCOMPARE(MiniWindowUtils::addHotspot(window, QStringLiteral("other"), 0, 0, 1, 1, {}, {}, {},
				                                     {}, {}, {}, 0, 0, QStringLiteral("plugin-b")),
				         eHotspotPluginChanged);
				QCOMPARE(MiniWindowUtils::setHotspotDragHandler(window, QStringLiteral("hot"), {}, {}, 0,
				                                                QStringLiteral("plugin-b")),
				         eHotspotPluginChanged);
				QCOMPARE(MiniWindowUtils::deleteHotspot(window, QStringLiteral("missing")),
				         eHotspotNotInstalled);
				QCOMPARE(MiniWindowUtils::deleteHotspot(window, QStringLiteral("hot")), eOK);
				QVERIFY(window.hotspots.isEmpty());
				QVERIFY(window.callbackPlugin.isEmpty());
			}

			void generatedHotspotsClearOnlyTheirRequestedScope()
			{
				MiniWindow window = makeWindow(QSize(40, 30));
				QCOMPARE(MiniWindowUtils::addGeneratedOutputHotspot(
				             window, QStringLiteral("left"), 0, 0, 10, 10, QStringLiteral("mouseup"),
				             QStringLiteral("plugin"), 2, QStringLiteral("look")),
				         eOK);
				QCOMPARE(MiniWindowUtils::addGeneratedOutputHotspot(
				             window, QStringLiteral("right"), 10, 0, 20, 10, QStringLiteral("mouseup"),
				             QStringLiteral("plugin"), 3, QStringLiteral("help")),
				         eOK);
				QCOMPARE(window.outputGeneratedHotspots.size(), 2);
				QCOMPARE(window.hotspots.value(QStringLiteral("left_1")).outputActionType, 2);
				QCOMPARE(window.hotspots.value(QStringLiteral("left_1")).outputAction,
				         QStringLiteral("look"));

				window.mouseOverHotspot = QStringLiteral("left_1");
				window.mouseDownHotspot = QStringLiteral("left_1");
				MiniWindowUtils::clearGeneratedOutputHotspots(window, QStringLiteral("left"));
				QVERIFY(!window.hotspots.contains(QStringLiteral("left_1")));
				QVERIFY(window.hotspots.contains(QStringLiteral("right_2")));
				QVERIFY(window.mouseOverHotspot.isEmpty());
				QVERIFY(window.mouseDownHotspot.isEmpty());
				QCOMPARE(window.outputHotspotSerial, static_cast<quint64>(2));

				MiniWindowUtils::deleteAllHotspots(window);
				QVERIFY(window.hotspots.isEmpty());
				QVERIFY(window.outputGeneratedHotspots.isEmpty());
				QCOMPARE(window.outputHotspotSerial, static_cast<quint64>(0));
				QVERIFY(window.callbackPlugin.isEmpty());
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

				QCOMPARE(MiniWindowUtils::polygon(window, QStringLiteral("0,0,,10,20,20"), 0, 0, 1, 0, 0,
				                                  true, false),
				         eInvalidPoint);
				QCOMPARE(MiniWindowUtils::polygon(window, QStringLiteral("0,0,10e1,10"), 0, 0, 1, 0, 0, true,
				                                  false),
				         eInvalidPoint);
			}
			// NOLINTEND(readability-convert-member-functions-to-static)
	};
} // namespace

QTEST_APPLESS_MAIN(tst_MiniWindowUtils)

#if __has_include("tst_MiniWindowUtils.moc")
#include "tst_MiniWindowUtils.moc"
#endif
