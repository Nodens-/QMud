/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldRuntime_MiniWindowGeometry.cpp
 * Role: Integration coverage for world-relative absolute miniwindow movement and resize constraints.
 */

#include "AppController.h"
#include "LuaCallbackEngine.h"
#include "LuaExecutor.h"
#include "MiniWindow.h"
#include "WorldRuntime.h"
#include "WorldView.h"
#include "helpers/MiniWindowUtils.h"
#include "scripting/ScriptingErrors.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QCoreApplication>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QtTest/QTest>

#include <limits>

namespace
{
	/**
	 * @brief QTest fixture covering runtime miniwindow geometry constraints against a real WorldView.
	 */
	class tst_WorldRuntime_MiniWindowGeometry final : public QObject
	{
			Q_OBJECT

		private slots:
			static void hiddenAbsoluteBackingRetainsCanonicalGeometry()
			{
				WorldRuntime runtime;
				WorldView    view;
				view.resize(800, 600);
				view.setRuntime(&runtime);
				view.show();
				QCoreApplication::processEvents();

				const int clientWidth  = view.outputClientWidth();
				const int clientHeight = view.outputClientHeight();
				QVERIFY(clientWidth > 240);
				QVERIFY(clientHeight > 240);

				const QString windowId = QStringLiteral("offscreen-buffer");
				const QSize   requestedSize(clientWidth + 160, clientHeight + 120);
				const QPoint  requestedLocation(clientWidth + 40, clientHeight + 30);
				QCOMPARE(runtime.windowCreate(windowId, requestedLocation.x(), requestedLocation.y(),
				                              requestedSize.width(), requestedSize.height(), 0,
				                              kMiniWindowAbsoluteLocation, QColor(Qt::black), QString()),
				         eOK);

				MiniWindow *window = runtime.miniWindow(windowId);
				QVERIFY(window);
				QVERIFY(!window->show);
				QCOMPARE(window->location, requestedLocation);
				QCOMPARE(window->logicalSize(), requestedSize);
				MiniWindowUtils::setPixel(*window, 1, 1, MiniWindowUtils::colorToRef(QColor(Qt::red)));

				const QSize resizedSize(clientWidth + 240, clientHeight + 180);
				QCOMPARE(runtime.windowResize(windowId, resizedSize.width(), resizedSize.height(),
				                              MiniWindowUtils::colorToRef(QColor(Qt::blue))),
				         eOK);
				QCOMPARE(window->location, requestedLocation);
				QCOMPARE(window->logicalSize(), resizedSize);
				QCOMPARE(MiniWindowUtils::pixelValue(*window, 1, 1),
				         MiniWindowUtils::colorToRef(QColor(Qt::red)));
			}

			static void activeLayerScaleReachesLuaDragConstraintSnapshot_data()
			{
				QTest::addColumn<int>("destinationFlags");
				QTest::newRow("overlay") << kMiniWindowAbsoluteLocation;
				QTest::newRow("underlay") << (kMiniWindowAbsoluteLocation | kMiniWindowDrawUnderneath);
			}

			static void activeLayerScaleReachesLuaDragConstraintSnapshot()
			{
				QFETCH(int, destinationFlags);
				QVERIFY(!AppController::instance());

				WorldRuntime runtime;
				WorldView    view;
				view.resize(800, 600);
				view.setRuntime(&runtime);
				view.show();
				QCoreApplication::processEvents();

				const int clientWidth  = view.outputClientWidth();
				const int clientHeight = view.outputClientHeight();
				QVERIFY(clientWidth > 240);
				QVERIFY(clientHeight > 160);

				runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
				runtime.setLuaScriptText(QStringLiteral(R"lua(
local scaled_drag_start_x = 0

function qmud_scaled_drag_down(flags, hotspot_id)
	scaled_drag_start_x = WindowInfo("scaled-drag", 17)
	SetVariable("scaled_drag_down_x", string.format("%d", scaled_drag_start_x))
end

function qmud_scaled_drag_move(flags, hotspot_id)
	local mouse_x = WindowInfo("scaled-drag", 17)
	WindowResize("scaled-drag", 100 + mouse_x - scaled_drag_start_x, 80, 0)
	WindowPosition("scaled-drag", 100000, 40, 0, %1)
	SetVariable("scaled_drag_observed", string.format("%d:%d:%d:%d",
		scaled_drag_start_x, mouse_x, WindowInfo("scaled-drag", 1), WindowInfo("scaled-drag", 3)))
end

function qmud_scaled_drag_release(flags, hotspot_id)
	SetVariable("scaled_drag_release_x", string.format("%d", WindowInfo("scaled-drag", 17)))
end
)lua")
				                             .arg(destinationFlags));

				const QString windowId = QStringLiteral("scaled-drag");
				QCOMPARE(runtime.windowCreate(windowId, clientWidth, 40, 100, 80, 0,
				                              kMiniWindowAbsoluteLocation, QColor(Qt::black), QString()),
				         eOK);
				QCOMPARE(runtime.windowAddHotspot(windowId, QStringLiteral("drag"), 0, 0, 100, 80, QString(),
				                                  QString(), QStringLiteral("qmud_scaled_drag_down"),
				                                  QString(), QString(), QString(), 0, 0, QString()),
				         eOK);
				QCOMPARE(runtime.windowDragHandler(windowId, QStringLiteral("drag"),
				                                   QStringLiteral("qmud_scaled_drag_move"),
				                                   QStringLiteral("qmud_scaled_drag_release"), 0, QString()),
				         eOK);
				QCOMPARE(runtime.windowShow(windowId, true), eOK);
				const bool destinationUnderneath = (destinationFlags & kMiniWindowDrawUnderneath) != 0;
				if (destinationUnderneath)
				{
					const QString anchorId = QStringLiteral("underlay-scale-anchor");
					QCOMPARE(runtime.windowCreate(anchorId, clientWidth + 100, 140, 100, 80, 0,
					                              kMiniWindowAbsoluteLocation | kMiniWindowDrawUnderneath,
					                              QColor(Qt::black), QString()),
					         eOK);
					QCOMPARE(runtime.windowShow(anchorId, true), eOK);
				}

				const QVector<MiniWindow *> windows = runtime.sortedMiniWindows();
				runtime.layoutMiniWindows(QSize(clientWidth, clientHeight), view.size(), true, &windows);
				runtime.layoutMiniWindows(QSize(clientWidth, clientHeight), view.size(), false, &windows);
				const MiniWindow *window = runtime.miniWindow(windowId);
				QVERIFY(window);
				QVERIFY(!window->rect.isEmpty());
				QVERIFY(window->rect.right() < clientWidth);

				auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
				QVERIFY(nativeCanvas);
				QWidget *outputStack = nativeCanvas->parentWidget();
				QVERIFY(outputStack);
				QVERIFY(outputStack->rect().contains(window->rect.center()));

				const QPoint pressPosition = window->rect.center();
				QTest::mousePress(outputStack, Qt::LeftButton, Qt::NoModifier, pressPosition);
				QCoreApplication::processEvents();
				QTRY_VERIFY(view.isMiniWindowCaptureActive());
				const double overlayScale =
				    static_cast<double>(clientWidth) / static_cast<double>(clientWidth + 100);
				const int expectedDownX = qRound(static_cast<double>(pressPosition.x()) / overlayScale);
				QCOMPARE(runtime.windowInfo(windowId, 17).toInt(), expectedDownX);
				QString observedDownX;
				QTRY_VERIFY(runtime.findVariable(QStringLiteral("scaled_drag_down_x"), observedDownX));
				QCOMPARE(observedDownX.toInt(), expectedDownX);

				const QPoint movePosition = pressPosition - QPoint(6, 0);
				const QPoint moveGlobal   = outputStack->mapToGlobal(movePosition);
				QMouseEvent  moveEvent(QEvent::MouseMove, QPointF(movePosition), QPointF(movePosition),
				                       QPointF(moveGlobal), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
				QCoreApplication::sendEvent(outputStack, &moveEvent);
				QCoreApplication::processEvents();

				QString observed;
				QTRY_VERIFY_WITH_TIMEOUT(
				    runtime.findVariable(QStringLiteral("scaled_drag_observed"), observed), 5000);
				const int expectedMoveX = qRound(static_cast<double>(movePosition.x()) / overlayScale);
				const int expectedWidth = 100 + expectedMoveX - expectedDownX;
				const int destinationCanonicalWidth = clientWidth + (destinationUnderneath ? 200 : 100);
				const int expectedLeft              = destinationCanonicalWidth - expectedWidth;
				QCOMPARE(observed, QStringLiteral("%1:%2:%3:%4")
				                       .arg(expectedDownX)
				                       .arg(expectedMoveX)
				                       .arg(expectedLeft)
				                       .arg(expectedWidth));
				QCOMPARE(runtime.windowInfo(windowId, 1).toInt(), expectedLeft);
				QCOMPARE(runtime.windowInfo(windowId, 3).toInt(), expectedWidth);

				QTest::mouseRelease(outputStack, Qt::LeftButton, Qt::NoModifier, movePosition);
				QCoreApplication::processEvents();
				QTRY_VERIFY(!view.isMiniWindowCaptureActive());
				QString observedReleaseX;
				QTRY_VERIFY(runtime.findVariable(QStringLiteral("scaled_drag_release_x"), observedReleaseX));
				const double releaseScale =
				    static_cast<double>(clientWidth) /
				    static_cast<double>(clientWidth + (destinationUnderneath ? 200 : 100));
				const int expectedReleaseX = qRound(static_cast<double>(movePosition.x()) / releaseScale);
				QCOMPARE(observedReleaseX.toInt(), expectedReleaseX);
			}

			static void absoluteScaleRecomputesWhenDragCaptureEnds_data()
			{
				QTest::addColumn<int>("destinationFlags");
				QTest::addColumn<bool>("expectHoverCallback");
				QTest::newRow("overlay") << kMiniWindowAbsoluteLocation << true;
				QTest::newRow("underlay")
				    << (kMiniWindowAbsoluteLocation | kMiniWindowDrawUnderneath) << false;
			}

			static void absoluteScaleRecomputesWhenDragCaptureEnds()
			{
				QFETCH(int, destinationFlags);
				QFETCH(bool, expectHoverCallback);
				QVERIFY(!AppController::instance());

				WorldRuntime runtime;
				WorldView    view;
				view.resize(800, 600);
				view.setRuntime(&runtime);
				view.show();
				QCoreApplication::processEvents();

				const int clientWidth  = view.outputClientWidth();
				const int clientHeight = view.outputClientHeight();
				QVERIFY(clientWidth > 240);
				QVERIFY(clientHeight > 160);

				runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
				runtime.setLuaScriptText(QStringLiteral(R"lua(
function qmud_scale_reset_move(flags, hotspot_id)
	WindowResize("scale-reset", 50, 80, 0)
	WindowPosition("scale-reset", %1, 40, 0, %2)
	WindowShow("scale-reset-anchor", false)
	WindowDragHandler("scale-reset", "drag", "", "", 0)
	SetVariable("scale_reset_moved", "1")
end

function qmud_scale_reset_over(flags, hotspot_id)
	SetVariable("scale_reset_hovered", hotspot_id)
end
)lua")
				                             .arg(clientWidth)
				                             .arg(destinationFlags));

				const QString windowId = QStringLiteral("scale-reset");
				QCOMPARE(runtime.windowCreate(windowId, clientWidth, 40, 100, 80, 0,
				                              kMiniWindowAbsoluteLocation, QColor(Qt::black), QString()),
				         eOK);
				QCOMPARE(runtime.windowAddHotspot(windowId, QStringLiteral("drag"), 0, 0, 100, 80,
				                                  QStringLiteral("qmud_scale_reset_over"), QString(),
				                                  QString(), QString(), QString(), QString(), 0, 0,
				                                  QString()),
				         eOK);
				QCOMPARE(runtime.windowDragHandler(windowId, QStringLiteral("drag"),
				                                   QStringLiteral("qmud_scale_reset_move"), QString(), 0,
				                                   QString()),
				         eOK);
				QCOMPARE(runtime.windowShow(windowId, true), eOK);
				if ((destinationFlags & kMiniWindowDrawUnderneath) != 0)
				{
					const QString anchorId = QStringLiteral("scale-reset-anchor");
					QCOMPARE(runtime.windowCreate(anchorId, clientWidth, 140, 100, 80, 0,
					                              kMiniWindowAbsoluteLocation | kMiniWindowDrawUnderneath,
					                              QColor(Qt::black), QString()),
					         eOK);
					QCOMPARE(runtime.windowShow(anchorId, true), eOK);
				}

				const QVector<MiniWindow *> windows = runtime.sortedMiniWindows();
				runtime.layoutMiniWindows(QSize(clientWidth, clientHeight), view.size(), true, &windows);
				runtime.layoutMiniWindows(QSize(clientWidth, clientHeight), view.size(), false, &windows);
				const MiniWindow *window = runtime.miniWindow(windowId);
				QVERIFY(window);
				QVERIFY(!window->rect.isEmpty());

				auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
				QVERIFY(nativeCanvas);
				QWidget *outputStack = nativeCanvas->parentWidget();
				QVERIFY(outputStack);
				const QPoint pressPosition = window->rect.center();
				QVERIFY(outputStack->rect().contains(pressPosition));

				QTest::mousePress(outputStack, Qt::LeftButton, Qt::NoModifier, pressPosition);
				QCoreApplication::processEvents();
				QTRY_VERIFY(view.isMiniWindowCaptureActive());

				const QPoint triggerMovePosition = pressPosition - QPoint(1, 0);
				const QPoint triggerMoveGlobal   = outputStack->mapToGlobal(triggerMovePosition);
				QMouseEvent  moveEvent(QEvent::MouseMove, QPointF(triggerMovePosition),
				                       QPointF(triggerMovePosition), QPointF(triggerMoveGlobal), Qt::NoButton,
				                       Qt::LeftButton, Qt::NoModifier);
				QCoreApplication::sendEvent(outputStack, &moveEvent);
				QString moved;
				QTRY_VERIFY_WITH_TIMEOUT(runtime.findVariable(QStringLiteral("scale_reset_moved"), moved),
				                         5000);
				QCOMPARE(moved, QStringLiteral("1"));
				QVERIFY(view.isMiniWindowDragCaptureActive());
				QTRY_VERIFY(runtime.miniWindow(windowId)
				                ->hotspots.value(QStringLiteral("drag"))
				                .moveCallback.isEmpty());

				runtime.layoutMiniWindows(QSize(clientWidth, clientHeight), view.size(), false, &windows);
				runtime.layoutMiniWindows(QSize(clientWidth, clientHeight), view.size(), true, &windows);
				QCOMPARE(window->flags, destinationFlags);
				const int capturedWidth =
				    qRound(50.0 * static_cast<double>(clientWidth) / static_cast<double>(clientWidth + 100));
				QCOMPARE(window->rect.width(), capturedWidth);
				const QPoint releasePosition(window->rect.right() + 1, window->rect.center().y());
				QVERIFY(outputStack->rect().contains(releasePosition));
				QVERIFY(!window->rect.contains(releasePosition));
				const QPoint releaseGlobal = outputStack->mapToGlobal(releasePosition);
				QMouseEvent  releaseMoveEvent(QEvent::MouseMove, QPointF(releasePosition),
				                              QPointF(releasePosition), QPointF(releaseGlobal), Qt::NoButton,
				                              Qt::LeftButton, Qt::NoModifier);
				QCoreApplication::sendEvent(outputStack, &releaseMoveEvent);
				QCoreApplication::processEvents();
				runtime.setVariable(QStringLiteral("scale_reset_hovered"), QStringLiteral("before-release"));

				QTest::mouseRelease(outputStack, Qt::LeftButton, Qt::NoModifier, releasePosition);
				QVERIFY(!view.isMiniWindowCaptureActive());
				const int releasedWidth =
				    qRound(50.0 * static_cast<double>(clientWidth) / static_cast<double>(clientWidth + 50));
				QCOMPARE(window->rect.width(), releasedWidth);
				QVERIFY(releasedWidth > capturedWidth);
				QCOMPARE(window->rect.right(), clientWidth - 1);
				QVERIFY(window->rect.contains(releasePosition));
				QString hoveredHotspot;
				if (expectHoverCallback)
				{
					QTRY_VERIFY_WITH_TIMEOUT(
					    runtime.findVariable(QStringLiteral("scale_reset_hovered"), hoveredHotspot) &&
					        hoveredHotspot == QStringLiteral("drag"),
					    5000);
					QCOMPARE(hoveredHotspot, QStringLiteral("drag"));
				}
				else
				{
					QVERIFY(runtime.findVariable(QStringLiteral("scale_reset_hovered"), hoveredHotspot));
					QCOMPARE(hoveredHotspot, QStringLiteral("before-release"));
				}
			}

			static void dragHandlerInstalledDuringMouseDownAppliesToNextCapture()
			{
				QVERIFY(!AppController::instance());

				WorldRuntime runtime;
				WorldView    view;
				view.resize(800, 600);
				view.setRuntime(&runtime);
				view.show();
				QCoreApplication::processEvents();

				runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
				runtime.setLuaScriptText(QStringLiteral(R"lua(
local install_drag_down_count = 0

function qmud_install_drag_down(flags, hotspot_id)
	install_drag_down_count = install_drag_down_count + 1
	SetVariable("install_drag_down_x_" .. install_drag_down_count,
		string.format("%d", WindowInfo("install-drag", 17)))
	WindowDragHandler("install-drag", hotspot_id, "qmud_installed_drag_move", "", 0)
	SetVariable("install_drag_ready", string.format("%d", install_drag_down_count))
end

function qmud_installed_drag_move(flags, hotspot_id)
	SetVariable("installed_drag_moved_x", string.format("%d", WindowInfo("install-drag", 17)))
end
)lua"));

				const int     clientWidth  = view.outputClientWidth();
				const int     clientHeight = view.outputClientHeight();
				const QString windowId     = QStringLiteral("install-drag");
				QVERIFY(clientWidth > 240);
				QVERIFY(clientHeight > 160);
				QCOMPARE(runtime.windowCreate(windowId, clientWidth, 40, 120, 80, 0,
				                              kMiniWindowAbsoluteLocation, QColor(Qt::black), QString()),
				         eOK);
				QCOMPARE(runtime.windowAddHotspot(windowId, QStringLiteral("drag"), 0, 0, 120, 80, QString(),
				                                  QString(), QStringLiteral("qmud_install_drag_down"),
				                                  QString(), QString(), QString(), 0, 0, QString()),
				         eOK);
				QCOMPARE(runtime.windowShow(windowId, true), eOK);

				const QVector<MiniWindow *> windows = runtime.sortedMiniWindows();
				runtime.layoutMiniWindows(QSize(clientWidth, clientHeight), view.size(), false, &windows);
				const MiniWindow *window = runtime.miniWindow(windowId);
				QVERIFY(window);
				const double activeScale =
				    static_cast<double>(clientWidth) / static_cast<double>(clientWidth + 120);
				QVERIFY(activeScale < 1.0);
				QCOMPARE(window->rect.right(), clientWidth - 1);
				auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
				QVERIFY(nativeCanvas);
				QWidget *outputStack = nativeCanvas->parentWidget();
				QVERIFY(outputStack);

				const QPoint pressPosition = window->rect.center();
				QTest::mousePress(outputStack, Qt::LeftButton, Qt::NoModifier, pressPosition);
				QString ready;
				QTRY_VERIFY(runtime.findVariable(QStringLiteral("install_drag_ready"), ready) &&
				            ready == QStringLiteral("1"));
				QVERIFY(view.isMiniWindowCaptureActive());
				QVERIFY(!view.isMiniWindowDragCaptureActive());
				QString firstDownX;
				QVERIFY(runtime.findVariable(QStringLiteral("install_drag_down_x_1"), firstDownX));
				QCOMPARE(firstDownX.toInt(), pressPosition.x());

				const QPoint firstMovePosition = pressPosition + QPoint(10, 0);
				const QPoint firstMoveGlobal   = outputStack->mapToGlobal(firstMovePosition);
				QMouseEvent firstMoveEvent(QEvent::MouseMove, QPointF(firstMovePosition),
				                           QPointF(firstMovePosition), QPointF(firstMoveGlobal), Qt::NoButton,
				                           Qt::LeftButton, Qt::NoModifier);
				QCoreApplication::sendEvent(outputStack, &firstMoveEvent);
				QCoreApplication::processEvents();
				QString movedX;
				QVERIFY(!runtime.findVariable(QStringLiteral("installed_drag_moved_x"), movedX));
				QCOMPARE(runtime.windowInfo(windowId, 17).toInt(), firstMovePosition.x());

				QTest::mouseRelease(outputStack, Qt::LeftButton, Qt::NoModifier, firstMovePosition);
				QVERIFY(!view.isMiniWindowCaptureActive());
				QCOMPARE(runtime.windowInfo(windowId, 17).toInt(), firstMovePosition.x());

				QTest::mousePress(outputStack, Qt::LeftButton, Qt::NoModifier, pressPosition);
				QTRY_VERIFY(runtime.findVariable(QStringLiteral("install_drag_ready"), ready) &&
				            ready == QStringLiteral("2"));
				QVERIFY(view.isMiniWindowCaptureActive());
				QVERIFY(view.isMiniWindowDragCaptureActive());
				QString secondDownX;
				QVERIFY(runtime.findVariable(QStringLiteral("install_drag_down_x_2"), secondDownX));
				const int expectedSecondDownX = qRound(static_cast<double>(pressPosition.x()) / activeScale);
				QVERIFY(expectedSecondDownX != pressPosition.x());
				QCOMPARE(secondDownX.toInt(), expectedSecondDownX);
				const QPoint secondMovePosition = pressPosition + QPoint(20, 0);
				const QPoint secondMoveGlobal   = outputStack->mapToGlobal(secondMovePosition);
				QMouseEvent  secondMoveEvent(QEvent::MouseMove, QPointF(secondMovePosition),
				                             QPointF(secondMovePosition), QPointF(secondMoveGlobal),
				                             Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
				QCoreApplication::sendEvent(outputStack, &secondMoveEvent);
				QTRY_VERIFY(runtime.findVariable(QStringLiteral("installed_drag_moved_x"), movedX));
				const int expectedSecondMoveX =
				    qRound(static_cast<double>(secondMovePosition.x()) / activeScale);
				QCOMPARE(movedX.toInt(), expectedSecondMoveX);
				QCOMPARE(runtime.windowInfo(windowId, 17).toInt(), expectedSecondMoveX);
				QTest::mouseRelease(outputStack, Qt::LeftButton, Qt::NoModifier, secondMovePosition);
				QVERIFY(!view.isMiniWindowCaptureActive());
				QCOMPARE(runtime.windowInfo(windowId, 17).toInt(), expectedSecondMoveX);
			}

			static void capturedCallbackCannotDeleteAndRecreateOutsideBounds()
			{
				QVERIFY(!AppController::instance());

				WorldRuntime runtime;
				WorldView    view;
				view.resize(800, 600);
				view.setRuntime(&runtime);
				view.show();
				QCoreApplication::processEvents();

				const int clientWidth  = view.outputClientWidth();
				const int clientHeight = view.outputClientHeight();
				QVERIFY(clientWidth > 240);
				QVERIFY(clientHeight > 160);

				runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
				runtime.setLuaScriptText(QStringLiteral(R"lua(
function qmud_guarded_drag_move(flags, hotspot_id)
	local delete_result = WindowDelete("guarded-drag")
	local create_result = WindowCreate("guarded-drag", 100000, 100000, 500, 500, 0, 2, 0)
	SetVariable("guarded_drag_results", string.format("%d:%d", delete_result, create_result))
end
)lua"));

				const QString   windowId = QStringLiteral("guarded-drag");
				constexpr QSize windowSize(100, 80);
				QCOMPARE(runtime.windowCreate(windowId, 40, 40, windowSize.width(), windowSize.height(), 0,
				                              kMiniWindowAbsoluteLocation, QColor(Qt::black), QString()),
				         eOK);
				QCOMPARE(runtime.windowAddHotspot(windowId, QStringLiteral("drag"), 0, 0, windowSize.width(),
				                                  windowSize.height(), QString(), QString(), QString(),
				                                  QString(), QString(), QString(), 0, 0, QString()),
				         eOK);
				QCOMPARE(runtime.windowDragHandler(windowId, QStringLiteral("drag"),
				                                   QStringLiteral("qmud_guarded_drag_move"), QString(), 0,
				                                   QString()),
				         eOK);
				QCOMPARE(runtime.windowShow(windowId, true), eOK);

				const QVector<MiniWindow *> windows = runtime.sortedMiniWindows();
				runtime.layoutMiniWindows(QSize(clientWidth, clientHeight), view.size(), true, &windows);
				runtime.layoutMiniWindows(QSize(clientWidth, clientHeight), view.size(), false, &windows);
				const MiniWindow *window = runtime.miniWindow(windowId);
				QVERIFY(window);

				auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
				QVERIFY(nativeCanvas);
				QWidget *outputStack = nativeCanvas->parentWidget();
				QVERIFY(outputStack);

				const QPoint pressPosition = window->rect.center();
				QTest::mousePress(outputStack, Qt::LeftButton, Qt::NoModifier, pressPosition);
				QCoreApplication::processEvents();
				QTRY_VERIFY(view.isMiniWindowCaptureActive());

				const QPoint movePosition = pressPosition + QPoint(1, 0);
				const QPoint moveGlobal   = outputStack->mapToGlobal(movePosition);
				QMouseEvent  moveEvent(QEvent::MouseMove, QPointF(movePosition), QPointF(movePosition),
				                       QPointF(moveGlobal), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
				QCoreApplication::sendEvent(outputStack, &moveEvent);
				QCoreApplication::processEvents();

				QString callbackResults;
				QTRY_VERIFY_WITH_TIMEOUT(
				    runtime.findVariable(QStringLiteral("guarded_drag_results"), callbackResults), 5000);
				QCOMPARE(callbackResults, QStringLiteral("%1:%2").arg(eItemInUse).arg(eOK));

				window = runtime.miniWindow(windowId);
				QVERIFY(window);
				QCOMPARE(window->logicalSize(), windowSize);
				QCOMPARE(window->location,
				         QPoint(clientWidth - windowSize.width(), clientHeight - windowSize.height()));

				QTest::mouseRelease(outputStack, Qt::LeftButton, Qt::NoModifier, movePosition);
				QCoreApplication::processEvents();
				QTRY_VERIFY(!view.isMiniWindowCaptureActive());
			}

			static void capturedRightEdgeResizePreservesGeometryAndContents()
			{
				QVERIFY(!AppController::instance());

				WorldRuntime runtime;
				WorldView    view;
				view.resize(800, 600);
				view.setRuntime(&runtime);
				view.show();
				QCoreApplication::processEvents();

				const int clientWidth  = view.outputClientWidth();
				const int clientHeight = view.outputClientHeight();
				QVERIFY(clientWidth > 240);
				QVERIFY(clientHeight > 160);

				runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
				runtime.setLuaScriptText(QStringLiteral(R"lua(
local resize_start_x = 0

function qmud_right_edge_resize_down(flags, hotspot_id)
	resize_start_x = WindowInfo("right-edge-resize", 17)
end

function qmud_right_edge_resize_move(flags, hotspot_id)
	local mouse_x = WindowInfo("right-edge-resize", 17)
	local result = WindowResize("right-edge-resize", 100 + mouse_x - resize_start_x, 80, 0)
	SetVariable("right_edge_resize_result", string.format("%d:%d:%d",
		result, WindowInfo("right-edge-resize", 3), mouse_x))
end
)lua"));

				const QString   windowId = QStringLiteral("right-edge-resize");
				constexpr QSize windowSize(100, 80);
				const QPoint    windowLocation(clientWidth - windowSize.width(), 40);
				QCOMPARE(runtime.windowCreate(windowId, windowLocation.x(), windowLocation.y(),
				                              windowSize.width(), windowSize.height(), 0,
				                              kMiniWindowAbsoluteLocation, QColor(Qt::black), QString()),
				         eOK);
				QCOMPARE(runtime.windowAddHotspot(windowId, QStringLiteral("resize"), windowSize.width() - 12,
				                                  0, windowSize.width(), windowSize.height(), QString(),
				                                  QString(), QStringLiteral("qmud_right_edge_resize_down"),
				                                  QString(), QString(), QString(), 0, 0, QString()),
				         eOK);
				QCOMPARE(runtime.windowDragHandler(windowId, QStringLiteral("resize"),
				                                   QStringLiteral("qmud_right_edge_resize_move"), QString(),
				                                   0, QString()),
				         eOK);
				QCOMPARE(runtime.windowShow(windowId, true), eOK);

				MiniWindow *window = runtime.miniWindow(windowId);
				QVERIFY(window);
				MiniWindowUtils::setPixel(*window, 1, 1, MiniWindowUtils::colorToRef(QColor(Qt::red)));
				const qint64                backingCacheKey = window->backingSurface().cacheKey();

				const QVector<MiniWindow *> windows = runtime.sortedMiniWindows();
				runtime.layoutMiniWindows(QSize(clientWidth, clientHeight), view.size(), true, &windows);
				runtime.layoutMiniWindows(QSize(clientWidth, clientHeight), view.size(), false, &windows);
				QCOMPARE(window->rect, QRect(windowLocation, windowSize));

				auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
				QVERIFY(nativeCanvas);
				QWidget *outputStack = nativeCanvas->parentWidget();
				QVERIFY(outputStack);

				const QPoint pressPosition(window->rect.right() - 2, window->rect.center().y());
				QTest::mousePress(outputStack, Qt::LeftButton, Qt::NoModifier, pressPosition);
				QCoreApplication::processEvents();
				QTRY_VERIFY(view.isMiniWindowCaptureActive());

				const QPoint movePosition(outputStack->width() + 40, pressPosition.y());
				const QPoint moveGlobal = outputStack->mapToGlobal(movePosition);
				QMouseEvent  moveEvent(QEvent::MouseMove, QPointF(movePosition), QPointF(movePosition),
				                       QPointF(moveGlobal), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
				QCoreApplication::sendEvent(outputStack, &moveEvent);
				QCoreApplication::processEvents();

				QString callbackResult;
				QTRY_VERIFY_WITH_TIMEOUT(
				    runtime.findVariable(QStringLiteral("right_edge_resize_result"), callbackResult), 5000);
				const QStringList resultParts = callbackResult.split(QLatin1Char(':'));
				QCOMPARE(resultParts.size(), 3);
				QCOMPARE(resultParts.at(0).toInt(), eOK);
				QCOMPARE(resultParts.at(1).toInt(), windowSize.width());
				QCOMPARE(resultParts.at(2).toInt(), clientWidth - 1);

				window = runtime.miniWindow(windowId);
				QVERIFY(window);
				QCOMPARE(window->location, windowLocation);
				QCOMPARE(window->logicalSize(), windowSize);
				QCOMPARE(window->backingSurface().cacheKey(), backingCacheKey);
				QCOMPARE(MiniWindowUtils::pixelValue(*window, 1, 1),
				         MiniWindowUtils::colorToRef(QColor(Qt::red)));
				QVERIFY(window->hotspots.contains(QStringLiteral("resize")));

				QMouseEvent releaseEvent(QEvent::MouseButtonRelease, QPointF(movePosition),
				                         QPointF(movePosition), QPointF(moveGlobal), Qt::LeftButton,
				                         Qt::NoButton, Qt::NoModifier);
				QCoreApplication::sendEvent(outputStack, &releaseEvent);
				QCoreApplication::processEvents();
				QTRY_VERIFY(!view.isMiniWindowCaptureActive());
			}

			static void fullyRejectedCapturedCreatePreservesWindowState()
			{
				QVERIFY(!AppController::instance());

				WorldRuntime runtime;
				WorldView    view;
				view.resize(800, 600);
				view.setRuntime(&runtime);
				view.show();
				QCoreApplication::processEvents();

				const int clientWidth  = view.outputClientWidth();
				const int clientHeight = view.outputClientHeight();
				QVERIFY(clientWidth > 240);
				QVERIFY(clientHeight > 160);

				runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
				runtime.setLuaScriptText(QStringLiteral(R"lua(
function qmud_fully_blocked_create(flags, hotspot_id)
	local result = WindowCreate("blocked-create", 100000, 100000, 500, 500, 0, 2, 0)
	SetVariable("blocked_create_result", string.format("%d", result))
end
)lua"));

				const QString   windowId = QStringLiteral("blocked-create");
				constexpr QSize windowSize(100, 80);
				const QPoint    windowLocation(clientWidth - windowSize.width(),
				                               clientHeight - windowSize.height());
				QCOMPARE(runtime.windowCreate(windowId, windowLocation.x(), windowLocation.y(),
				                              windowSize.width(), windowSize.height(), 0,
				                              kMiniWindowAbsoluteLocation, QColor(Qt::black), QString()),
				         eOK);
				QCOMPARE(runtime.windowAddHotspot(windowId, QStringLiteral("drag"), 0, 0, windowSize.width(),
				                                  windowSize.height(), QString(), QString(), QString(),
				                                  QString(), QString(), QString(), 0, 0, QString()),
				         eOK);
				QCOMPARE(runtime.windowDragHandler(windowId, QStringLiteral("drag"),
				                                   QStringLiteral("qmud_fully_blocked_create"), QString(), 0,
				                                   QString()),
				         eOK);
				QCOMPARE(runtime.windowShow(windowId, true), eOK);

				MiniWindow *window = runtime.miniWindow(windowId);
				QVERIFY(window);
				MiniWindowUtils::setPixel(*window, 1, 1, MiniWindowUtils::colorToRef(QColor(Qt::red)));
				const QDateTime             installedAt = window->installedAt;

				const QVector<MiniWindow *> windows = runtime.sortedMiniWindows();
				runtime.layoutMiniWindows(QSize(clientWidth, clientHeight), view.size(), true, &windows);
				runtime.layoutMiniWindows(QSize(clientWidth, clientHeight), view.size(), false, &windows);

				auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
				QVERIFY(nativeCanvas);
				QWidget *outputStack = nativeCanvas->parentWidget();
				QVERIFY(outputStack);

				const QPoint pressPosition = window->rect.center();
				QTest::mousePress(outputStack, Qt::LeftButton, Qt::NoModifier, pressPosition);
				QCoreApplication::processEvents();
				QTRY_VERIFY(view.isMiniWindowCaptureActive());

				const QPoint movePosition = pressPosition + QPoint(1, 0);
				const QPoint moveGlobal   = outputStack->mapToGlobal(movePosition);
				QMouseEvent  moveEvent(QEvent::MouseMove, QPointF(movePosition), QPointF(movePosition),
				                       QPointF(moveGlobal), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
				QCoreApplication::sendEvent(outputStack, &moveEvent);
				QCoreApplication::processEvents();

				QString callbackResult;
				QTRY_VERIFY_WITH_TIMEOUT(
				    runtime.findVariable(QStringLiteral("blocked_create_result"), callbackResult), 5000);
				QCOMPARE(callbackResult.toInt(), eOK);

				window = runtime.miniWindow(windowId);
				QVERIFY(window);
				QCOMPARE(window->location, windowLocation);
				QCOMPARE(window->logicalSize(), windowSize);
				QVERIFY(window->show);
				QCOMPARE(window->installedAt, installedAt);
				QVERIFY(window->hotspots.contains(QStringLiteral("drag")));
				QCOMPARE(MiniWindowUtils::pixelValue(*window, 1, 1),
				         MiniWindowUtils::colorToRef(QColor(Qt::red)));

				QTest::mouseRelease(outputStack, Qt::LeftButton, Qt::NoModifier, movePosition);
				QCoreApplication::processEvents();
				QTRY_VERIFY(!view.isMiniWindowCaptureActive());
			}

			static void unchangedWindowPositionPreservesCacheAndDoesNotSignal()
			{
				WorldRuntime  runtime;
				const QString windowId = QStringLiteral("position-no-op");
				QCOMPARE(runtime.windowCreate(windowId, 40, 50, 100, 80, 0, kMiniWindowAbsoluteLocation,
				                              QColor(Qt::black), QString()),
				         eOK);

				MiniWindow *window = runtime.miniWindow(windowId);
				QVERIFY(window);
				window->transparentSurfaceCache = QImage(4, 4, QImage::Format_ARGB32);
				window->transparentSurfaceCache.fill(QColor(Qt::red));
				window->transparentSurfaceSourceKey = 1234;
				const qint64 cacheKey               = window->transparentSurfaceCache.cacheKey();

				QSignalSpy   changedSpy(&runtime, &WorldRuntime::miniWindowsChanged);
				QVERIFY(changedSpy.isValid());
				QCOMPARE(runtime.windowPosition(windowId, 40, 50, 0, kMiniWindowAbsoluteLocation), eOK);

				QCOMPARE(changedSpy.count(), 0);
				QCOMPARE(window->transparentSurfaceSourceKey, 1234);
				QCOMPARE(window->transparentSurfaceCache.cacheKey(), cacheKey);
				QCOMPARE(window->transparentSurfaceCache.pixelColor(0, 0), QColor(Qt::red));
			}

			static void capturedCallbackPreservesDistinctMiniWindowIdentity_data()
			{
				QTest::addColumn<QString>("capturedWindowId");
				QTest::addColumn<QString>("distinctWindowId");

				QTest::newRow("case-distinct") << QStringLiteral("Map") << QStringLiteral("map");
				QTest::newRow("whitespace-distinct") << QStringLiteral("Map ") << QStringLiteral("Map");
			}

			static void capturedCallbackPreservesDistinctMiniWindowIdentity()
			{
				QFETCH(QString, capturedWindowId);
				QFETCH(QString, distinctWindowId);
				QVERIFY(!AppController::instance());

				WorldRuntime runtime;
				WorldView    view;
				view.resize(800, 600);
				view.setRuntime(&runtime);
				view.show();
				QCoreApplication::processEvents();

				const int clientWidth  = view.outputClientWidth();
				const int clientHeight = view.outputClientHeight();
				QVERIFY(clientWidth > 320);
				QVERIFY(clientHeight > 160);

				runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
				runtime.setLuaScriptText(QStringLiteral(R"lua(
local distinct_window_id = "%1"

function qmud_distinct_drag_move(flags, hotspot_id)
	local delete_result = WindowDelete(distinct_window_id)
	local create_result = WindowCreate(distinct_window_id, 10000, 10000, 500, 500, 0, 2, 0)
	SetVariable("distinct_identity_results", string.format("%d:%d", delete_result, create_result))
end
)lua")
				                             .arg(distinctWindowId));

				constexpr QSize  capturedSize(100, 80);
				constexpr QSize  distinctInitialSize(90, 70);
				constexpr QPoint capturedLocation(40, 40);
				constexpr QPoint distinctInitialLocation(200, 40);
				QCOMPARE(runtime.windowCreate(capturedWindowId, capturedLocation.x(), capturedLocation.y(),
				                              capturedSize.width(), capturedSize.height(), 0,
				                              kMiniWindowAbsoluteLocation, QColor(Qt::black), QString()),
				         eOK);
				QCOMPARE(runtime.windowCreate(distinctWindowId, distinctInitialLocation.x(),
				                              distinctInitialLocation.y(), distinctInitialSize.width(),
				                              distinctInitialSize.height(), 0, kMiniWindowAbsoluteLocation,
				                              QColor(Qt::black), QString()),
				         eOK);
				QCOMPARE(runtime.windowAddHotspot(capturedWindowId, QStringLiteral("drag"), 0, 0,
				                                  capturedSize.width(), capturedSize.height(), QString(),
				                                  QString(), QString(), QString(), QString(), QString(), 0, 0,
				                                  QString()),
				         eOK);
				QCOMPARE(runtime.windowDragHandler(capturedWindowId, QStringLiteral("drag"),
				                                   QStringLiteral("qmud_distinct_drag_move"), QString(), 0,
				                                   QString()),
				         eOK);
				QCOMPARE(runtime.windowShow(capturedWindowId, true), eOK);
				QCOMPARE(runtime.windowShow(distinctWindowId, true), eOK);

				const QSize                 clientSize(clientWidth, clientHeight);
				const QVector<MiniWindow *> windows = runtime.sortedMiniWindows();
				runtime.layoutMiniWindows(clientSize, view.size(), true, &windows);
				runtime.layoutMiniWindows(clientSize, view.size(), false, &windows);
				const MiniWindow *capturedWindow = runtime.miniWindow(capturedWindowId);
				QVERIFY(capturedWindow);

				auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
				QVERIFY(nativeCanvas);
				QWidget *outputStack = nativeCanvas->parentWidget();
				QVERIFY(outputStack);

				const QPoint pressPosition = capturedWindow->rect.center();
				QTest::mousePress(outputStack, Qt::LeftButton, Qt::NoModifier, pressPosition);
				QCoreApplication::processEvents();
				QTRY_VERIFY(view.isMiniWindowCaptureActive());

				const QPoint movePosition = pressPosition + QPoint(1, 0);
				const QPoint moveGlobal   = outputStack->mapToGlobal(movePosition);
				QMouseEvent  moveEvent(QEvent::MouseMove, QPointF(movePosition), QPointF(movePosition),
				                       QPointF(moveGlobal), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
				QCoreApplication::sendEvent(outputStack, &moveEvent);
				QCoreApplication::processEvents();

				QString callbackResults;
				QTRY_VERIFY_WITH_TIMEOUT(
				    runtime.findVariable(QStringLiteral("distinct_identity_results"), callbackResults), 5000);
				QCOMPARE(callbackResults, QStringLiteral("%1:%2").arg(eOK).arg(eOK));

				capturedWindow = runtime.miniWindow(capturedWindowId);
				QVERIFY(capturedWindow);
				QCOMPARE(capturedWindow->location, capturedLocation);
				QCOMPARE(capturedWindow->logicalSize(), capturedSize);
				const MiniWindow *distinctWindow = runtime.miniWindow(distinctWindowId);
				QVERIFY(distinctWindow);
				QCOMPARE(distinctWindow->location, QPoint(10000, 10000));
				QCOMPARE(distinctWindow->logicalSize(), QSize(500, 500));

				QTest::mouseRelease(outputStack, Qt::LeftButton, Qt::NoModifier, movePosition);
				QCoreApplication::processEvents();
				QTRY_VERIFY(!view.isMiniWindowCaptureActive());
			}

			static void callbackMiniWindowFontCachesKeepDelimiterDistinctKeys()
			{
				WorldRuntime runtime;
				auto         engine = QSharedPointer<LuaCallbackEngine>::create();
				engine->setWorldRuntime(&runtime);
				engine->setPluginInfo(QStringLiteral("Plugin.Id"), QStringLiteral("Plugin Name"),
				                      QStringLiteral("/tmp/plugin"));
				engine->setScriptText(QStringLiteral(R"lua(
function OnPluginEnable()
  assert(WindowFont("font-a", "b|c", "DejaVu Sans Mono", 12, false, false) == 0)
  assert(WindowFont("font-a|b", "c", "DejaVu Sans Mono", 12, false, true) == 0)
  assert(WindowFontInfo("font-a", "b|c", 16) == 0)
  assert(WindowFontInfo("font-a|b", "c", 16) == 1)

  assert(WindowFont("text-a", "b", "DejaVu Sans Mono", 6, false, false) == 0)
  assert(WindowFont("text-a|b", "c", "DejaVu Sans Mono", 40, false, false) == 0)
  local narrow_width = WindowTextWidth("text-a", "b", "c|D")
  local wide_width = WindowTextWidth("text-a|b", "c", "D")
  assert(narrow_width >= 0)
  assert(wide_width >= 0)
  assert(narrow_width ~= wide_width)
  structured_font_cache_result = "ok"
end
function structured_font_cache_status(value)
  return structured_font_cache_result or ""
end
)lua"));
				QVERIFY(engine->loadScript());

				auto snapshot         = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
				snapshot->windowNames = {QStringLiteral("font-a"), QStringLiteral("font-a|b"),
				                         QStringLiteral("text-a"), QStringLiteral("text-a|b")};
				snapshot->rebuildMiniWindowLookupCaches();

				LuaExecutorDirect       executor;
				LuaBatchDispatchRequest request;
				request.engines               = {engine};
				request.kind                  = LuaBatchDispatchKind::NoArgs;
				request.functionName          = QStringLiteral("OnPluginEnable");
				request.miniWindowSnapshotArg = snapshot;
				LuaBatchDispatchResult result = executor.dispatchBatch(request);
				QVERIFY(result.hasFunctionValid);
				QVERIFY(result.hasFunction);

				request.kind         = LuaBatchDispatchKind::StringInOut;
				request.functionName = QStringLiteral("structured_font_cache_status");
				request.stringArg    = QStringLiteral("ignored");
				request.miniWindowSnapshotArg.reset();
				result = executor.dispatchBatch(request);
				QCOMPARE(result.stringResult, QStringLiteral("ok"));
			}

			static void queuedHotspotCallbackSeesMiniWindowExecutionGuard()
			{
				QVERIFY(!AppController::instance());

				WorldRuntime runtime;
				WorldView    view;
				view.resize(800, 600);
				view.setRuntime(&runtime);
				view.show();
				QCoreApplication::processEvents();

				runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
				runtime.setLuaScriptText(QStringLiteral(R"lua(
function qmud_queued_hotspot(flags, hotspot_id)
	local delete_result = WindowDelete("queued-guard")
	WindowCreate("queued-guard", 40, 40, 100, 80, 0, 2, 0)
	SetVariable("queued_guard_delete_result", string.format("%d", delete_result))
end
)lua"));

				const QString windowId = QStringLiteral("queued-guard");
				QCOMPARE(runtime.windowCreate(windowId, 40, 40, 100, 80, 0, kMiniWindowAbsoluteLocation,
				                              QColor(Qt::black), QString()),
				         eOK);
				QCOMPARE(runtime.windowAddHotspot(windowId, QStringLiteral("right-click"), 0, 0, 100, 80,
				                                  QString(), QString(), QString(), QString(),
				                                  QStringLiteral("qmud_queued_hotspot"), QString(), 0, 0,
				                                  QString()),
				         eOK);
				QCOMPARE(runtime.windowShow(windowId, true), eOK);

				const QSize                 clientSize(view.outputClientWidth(), view.outputClientHeight());
				const QVector<MiniWindow *> windows = runtime.sortedMiniWindows();
				runtime.layoutMiniWindows(clientSize, view.size(), true, &windows);
				runtime.layoutMiniWindows(clientSize, view.size(), false, &windows);
				const MiniWindow *window = runtime.miniWindow(windowId);
				QVERIFY(window);
				auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
				QVERIFY(nativeCanvas);
				QWidget *outputStack = nativeCanvas->parentWidget();
				QVERIFY(outputStack);
				const QPoint clickPosition = window->rect.center();
				QTest::mousePress(outputStack, Qt::RightButton, Qt::NoModifier, clickPosition);
				QCoreApplication::processEvents();
				QTRY_VERIFY(view.isMiniWindowCaptureActive());

				QSharedPointer<LuaCallbackEngine> engine(runtime.luaCallbacks(),
				                                         [](LuaCallbackEngine * /*unused*/) {});
				QVERIFY(engine);
				bool asynchronousDispatchCompleted = false;
				runtime.dispatchLuaExecuteScriptAsync(
				    engine, QStringLiteral("SetVariable('queued_guard_predecessor', 'done')"),
				    QStringLiteral("queued miniwindow guard predecessor"), nullptr, false, false, 0, 0,
				    [&asynchronousDispatchCompleted](const bool ok) { asynchronousDispatchCompleted = ok; });

				QTest::mouseRelease(outputStack, Qt::RightButton, Qt::NoModifier, clickPosition);
				QVERIFY(!view.isMiniWindowCaptureActive());
				QString deleteResult;
				QVERIFY(!runtime.findVariable(QStringLiteral("queued_guard_delete_result"), deleteResult));

				QTRY_VERIFY_WITH_TIMEOUT(
				    runtime.findVariable(QStringLiteral("queued_guard_delete_result"), deleteResult), 5000);
				QCOMPARE(deleteResult.toInt(), eItemInUse);
				QTRY_VERIFY_WITH_TIMEOUT(asynchronousDispatchCompleted, 5000);
				QVERIFY(runtime.miniWindow(windowId));
			}

			static void scalerPoliciesRetainCanonicalGeometry()
			{
				QVERIFY(!AppController::instance());
				{
					WorldRuntime runtime;
					WorldView    view;
					view.resize(800, 600);
					view.setRuntime(&runtime);
					view.show();
					QCoreApplication::processEvents();

					const QSize clientSize(view.outputClientWidth(), view.outputClientHeight());
					QVERIFY(clientSize.width() > 240);
					QVERIFY(clientSize.height() > 240);

					const QString   windowId = QStringLiteral("scaled-overflow");
					const QPoint    location(clientSize.width() - 60, 40);
					constexpr QSize logicalSize(120, 80);
					QCOMPARE(runtime.windowCreate(windowId, location.x(), location.y(), logicalSize.width(),
					                              logicalSize.height(), 0, kMiniWindowAbsoluteLocation,
					                              QColor(Qt::black), QString()),
					         eOK);
					QCOMPARE(runtime.windowShow(windowId, true), eOK);

					const QVector<MiniWindow *> windows = runtime.sortedMiniWindows();
					runtime.layoutMiniWindows(clientSize, view.size(), false, &windows);
					const MiniWindow *window = runtime.miniWindow(windowId);
					QVERIFY(window);
					QCOMPARE(window->location, location);
					QCOMPARE(window->logicalSize(), logicalSize);
					QVERIFY(window->rect.right() < clientSize.width());
					QVERIFY(window->rect.width() < logicalSize.width());
				}

				{
					AppController app;
					QCOMPARE(app.getGlobalOption(QStringLiteral("DisableWindowScaler")).toInt(), 1);

					WorldRuntime runtime;
					WorldView    view;
					view.resize(800, 600);
					view.setRuntime(&runtime);
					view.show();
					QCoreApplication::processEvents();

					const int clientWidth  = view.outputClientWidth();
					const int clientHeight = view.outputClientHeight();
					QVERIFY(clientWidth > 240);
					QVERIFY(clientHeight > 240);

					const QString windowId     = QStringLiteral("unscaled-overflow");
					constexpr int windowWidth  = 120;
					constexpr int windowHeight = 80;
					const QPoint  location(clientWidth - 60, clientHeight - 40);
					QCOMPARE(runtime.windowCreate(windowId, location.x(), location.y(), windowWidth,
					                              windowHeight, 0, kMiniWindowAbsoluteLocation,
					                              QColor(Qt::black), QString()),
					         eOK);
					QCOMPARE(runtime.windowShow(windowId, true), eOK);

					const QVector<MiniWindow *> windows = runtime.sortedMiniWindows();
					runtime.layoutMiniWindows(QSize(clientWidth, clientHeight), view.size(), false, &windows);
					const MiniWindow *window = runtime.miniWindow(windowId);
					QVERIFY(window);
					QCOMPARE(window->location, location);
					QCOMPARE(window->logicalSize(), QSize(windowWidth, windowHeight));
					QCOMPARE(window->rect, QRect(location, QSize(windowWidth, windowHeight)));
					QVERIFY(window->rect.right() >= clientWidth);
					QVERIFY(window->rect.bottom() >= clientHeight);
				}
				QVERIFY(!AppController::instance());
			}

			static void absoluteExtentArithmeticSaturatesBeforeScaling()
			{
				QVERIFY(!AppController::instance());

				WorldRuntime     runtime;
				const QString    windowId      = QStringLiteral("saturated-absolute-extent");
				constexpr int    kWindowWidth  = 100;
				constexpr int    kWindowHeight = 80;
				constexpr QPoint location(std::numeric_limits<int>::max() - 40,
				                          std::numeric_limits<int>::max() - 30);
				QCOMPARE(runtime.windowCreate(windowId, location.x(), location.y(), kWindowWidth,
				                              kWindowHeight, 0, kMiniWindowAbsoluteLocation,
				                              QColor(Qt::black), QString()),
				         eOK);
				QCOMPARE(runtime.windowShow(windowId, true), eOK);

				constexpr QSize             clientSize(640, 480);
				const QVector<MiniWindow *> windows = runtime.sortedMiniWindows();
				runtime.layoutMiniWindows(clientSize, clientSize, false, &windows);

				const MiniWindow *window = runtime.miniWindow(windowId);
				QVERIFY(window);
				QCOMPARE(window->location, location);
				QCOMPARE(window->logicalSize(), QSize(kWindowWidth, kWindowHeight));
				QVERIFY(!window->rect.isEmpty());
				QVERIFY(QRect(QPoint(0, 0), clientSize).contains(window->rect));
			}
	};
} // namespace

QTEST_MAIN(tst_WorldRuntime_MiniWindowGeometry)

#if __has_include("tst_WorldRuntime_MiniWindowGeometry.moc")
#include "tst_WorldRuntime_MiniWindowGeometry.moc"
#endif
