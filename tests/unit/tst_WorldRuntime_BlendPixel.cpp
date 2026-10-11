/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldRuntime_BlendPixel.cpp
 * Role: Unit coverage for MUSHclient-compatible WorldRuntime pixel blending.
 */

#include "WorldRuntime.h"

#include "ColorPacking.h"

#include <QtTest/QTest>

#include <array>

namespace
{
	/**
	 * @brief QTest fixture covering the public WorldRuntime pixel blend API.
	 */
	class tst_WorldRuntime_BlendPixel : public QObject
	{
			Q_OBJECT

			struct BlendCase
			{
					QMudColorRef                blend{0};
					QMudColorRef                base{0};
					std::array<QMudColorRef, 5> expected{};
			};

			const std::array<BlendCase, 4> m_cases = {
			    BlendCase{qmudRgb(255, 0,   0),
			              qmudRgb(0,   0,   255),
			              {qmudRgb(255, 0, 0), qmudRgb(0, 0, 255), qmudRgb(255, 0, 0), qmudRgb(0, 0, 255),
			               qmudRgb(0, 0, 0)}                      },
			    BlendCase{qmudRgb(192, 96,  32),
			              qmudRgb(64,  128, 224),
			              {qmudRgb(224, 128, 64), qmudRgb(65, 128, 223), qmudRgb(223, 128, 65),
			               qmudRgb(31, 96, 193), qmudRgb(0, 0, 0)}},
			    BlendCase{qmudRgb(80,  80,  80),
			              qmudRgb(10,  100, 200),
			              {qmudRgb(200, 10, 10), qmudRgb(105, 105, 105), qmudRgb(105, 105, 105),
			               qmudRgb(8, 76, 152), qmudRgb(0, 0, 0)} },
			    BlendCase{qmudRgb(0,   255, 0),
			              qmudRgb(255, 0,   0),
			              {qmudRgb(0, 255, 0), qmudRgb(255, 0, 0), qmudRgb(0, 255, 0), qmudRgb(255, 0, 0),
			               qmudRgb(0, 0, 0)}                      },
			};

		private slots:
			/**
			 * @brief Verifies HSL modes reproduce MUSHclient results for chromatic and achromatic inputs.
			 */
			void hslModesMatchMushclientReferenceValues() const
			{
				for (const BlendCase &blendCase : m_cases)
				{
					for (short mode = 60; mode <= 64; ++mode)
					{
						const auto actual = static_cast<QMudColorRef>(
						    WorldRuntime::blendPixel(static_cast<long>(blendCase.blend),
						                             static_cast<long>(blendCase.base), mode, 1.0));
						const QMudColorRef expected = blendCase.expected[static_cast<size_t>(mode - 60)];
						QVERIFY2(actual == expected,
						         qPrintable(QStringLiteral("blend mode %1: expected 0x%2, got 0x%3")
						                        .arg(mode)
						                        .arg(expected, 6, 16, QLatin1Char('0'))
						                        .arg(actual, 6, 16, QLatin1Char('0'))));
					}
				}
			}

			/**
			 * @brief Verifies HSL blend results are combined with the base using the requested opacity.
			 */
			void hslModeAppliesOpacityAfterColourConversion() const
			{
				const BlendCase &blendCase = m_cases[1];
				QCOMPARE(static_cast<QMudColorRef>(WorldRuntime::blendPixel(
				             static_cast<long>(blendCase.blend), static_cast<long>(blendCase.base), 60, 0.5)),
				         qmudRgb(144, 128, 144));
			}
	};
} // namespace

QTEST_APPLESS_MAIN(tst_WorldRuntime_BlendPixel)

#include "tst_WorldRuntime_BlendPixel.moc"
