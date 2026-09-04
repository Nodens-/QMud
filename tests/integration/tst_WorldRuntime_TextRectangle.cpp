/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldRuntime_TextRectangle.cpp
 * Role: Integration coverage for TextRectangle invalidation and native-output repainting.
 */

#include "WorldRuntime.h"
#include "WorldView.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QCoreApplication>
#include <QImage>
#include <QPaintEvent>
#include <QRegion>
#include <QWidget>
#include <QtTest/QTest>

namespace
{
	/**
	 * @brief Records regions delivered through real widget paint events.
	 */
	class PaintRegionObserver final : public QObject
	{
		public:
			/**
			 * @brief Clears all observed paint-event state.
			 */
			void reset()
			{
				m_paintCount    = 0;
				m_paintedRegion = {};
			}

			/**
			 * @brief Returns the number of observed paint events.
			 * @return Paint-event count since the last reset.
			 */
			[[nodiscard]] int paintCount() const
			{
				return m_paintCount;
			}

			/**
			 * @brief Returns the accumulated paint region.
			 * @return Union of paint-event regions since the last reset.
			 */
			[[nodiscard]] const QRegion &paintedRegion() const
			{
				return m_paintedRegion;
			}

			/**
			 * @brief Records paint events while preserving normal event delivery.
			 * @param watched Object receiving the event.
			 * @param event Event being delivered.
			 * @return Always `false` so the widget handles the event normally.
			 */
			bool eventFilter(QObject *watched, QEvent *event) override
			{
				Q_UNUSED(watched);
				if (const auto *paintEvent = dynamic_cast<const QPaintEvent *>(event))
				{
					m_paintedRegion += paintEvent->region();
					++m_paintCount;
				}
				return false;
			}

		private:
			int     m_paintCount{0};
			QRegion m_paintedRegion;
	};

	/**
	 * @brief QTest fixture covering runtime-driven TextRectangle repaint invalidation.
	 */
	class tst_WorldRuntime_TextRectangle final : public QObject
	{
			Q_OBJECT

		private slots:
			static void geometryChangeInvalidatesFormerInsideAndOutsidePixels()
			{
				WorldRuntime runtime;
				WorldView    view;
				view.resize(900, 640);
				view.setRuntime(&runtime);
				view.show();
				QCoreApplication::processEvents();

				auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
				QVERIFY(nativeCanvas);
				QVERIFY(nativeCanvas->width() > 650);
				QVERIFY(nativeCanvas->height() > 300);

				constexpr int                       kWhiteColorRef   = 0x00FFFFFF;
				constexpr int                       kOutsideColorRef = 0x00FF00FF;
				constexpr QColor                    outsideColour(255, 0, 255);
				WorldRuntime::TextRectangleSettings initialSettings;
				initialSettings.left              = 60;
				initialSettings.top               = 60;
				initialSettings.right             = 300;
				initialSettings.bottom            = 240;
				initialSettings.borderOffset      = 0;
				initialSettings.borderWidth       = 0;
				initialSettings.borderColour      = kWhiteColorRef;
				initialSettings.outsideFillStyle  = 0;
				initialSettings.outsideFillColour = kOutsideColorRef;

				constexpr QPoint formerInside(100, 100);
				constexpr QPoint formerOutside(420, 100);
				QVERIFY(nativeCanvas->rect().contains(formerInside));
				QVERIFY(nativeCanvas->rect().contains(formerOutside));

				runtime.setTextRectangle(initialSettings);
				QCoreApplication::processEvents();

				PaintRegionObserver paintObserver;
				nativeCanvas->installEventFilter(&paintObserver);
				paintObserver.reset();

				auto movedSettings   = initialSettings;
				movedSettings.left   = 340;
				movedSettings.right  = 600;
				movedSettings.top    = 60;
				movedSettings.bottom = 240;
				runtime.setTextRectangle(movedSettings);
				QCoreApplication::processEvents();

				QVERIFY(paintObserver.paintCount() > 0);
				QVERIFY(paintObserver.paintedRegion().contains(formerInside));
				QVERIFY(paintObserver.paintedRegion().contains(formerOutside));

				const QImage rendered = nativeCanvas->grab().toImage();
				QCOMPARE(rendered.pixelColor(formerInside), outsideColour);
				QVERIFY(rendered.pixelColor(formerOutside) != outsideColour);
			}
	};
} // namespace

QTEST_MAIN(tst_WorldRuntime_TextRectangle)

#if __has_include("tst_WorldRuntime_TextRectangle.moc")
#include "tst_WorldRuntime_TextRectangle.moc"
#endif
