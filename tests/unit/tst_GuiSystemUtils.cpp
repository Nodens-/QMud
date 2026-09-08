/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_GuiSystemUtils.cpp
 * Role: Unit coverage for legacy GUI system colors, metrics, and device-capability snapshots.
 */

#include "ColorPacking.h"
#include "helpers/GuiSystemUtils.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QColor>
// ReSharper disable once CppUnusedIncludeDirective
#include <QGuiApplication>
// ReSharper disable once CppUnusedIncludeDirective
#include <QPalette>
// ReSharper disable once CppUnusedIncludeDirective
#include <QScreen>
#include <QtTest/QTest>

namespace
{
	/**
	 * @brief Restores the application palette after a test changes it.
	 */
	class PaletteRestorer final
	{
		public:
			/** Captures the current application palette. */
			PaletteRestorer() : m_palette(QGuiApplication::palette())
			{
			}

			/** Restores the captured application palette. */
			~PaletteRestorer()
			{
				QGuiApplication::setPalette(m_palette);
			}

			PaletteRestorer(const PaletteRestorer &)            = delete;
			PaletteRestorer &operator=(const PaletteRestorer &) = delete;

		private:
			QPalette m_palette;
	};

	/**
	 * @brief QTest fixture covering GUI system snapshot compatibility values.
	 */
	class tst_GuiSystemUtils final : public QObject
	{
			Q_OBJECT

		private slots:
			static void systemColorIndicesMapToPaletteRoles_data()
			{
				QTest::addColumn<int>("index");
				QTest::addColumn<int>("role");

				QTest::newRow("mid") << 0 << static_cast<int>(QPalette::Mid);
				QTest::newRow("window") << 1 << static_cast<int>(QPalette::Window);
				QTest::newRow("highlight") << 2 << static_cast<int>(QPalette::Highlight);
				QTest::newRow("dark") << 3 << static_cast<int>(QPalette::Dark);
				QTest::newRow("button") << 4 << static_cast<int>(QPalette::Button);
				QTest::newRow("window-alias") << 5 << static_cast<int>(QPalette::Window);
				QTest::newRow("shadow") << 6 << static_cast<int>(QPalette::Shadow);
				QTest::newRow("button-text") << 7 << static_cast<int>(QPalette::ButtonText);
				QTest::newRow("window-text") << 8 << static_cast<int>(QPalette::WindowText);
				QTest::newRow("highlighted-text") << 9 << static_cast<int>(QPalette::HighlightedText);
				QTest::newRow("dark-alias") << 10 << static_cast<int>(QPalette::Dark);
				QTest::newRow("mid-alias") << 11 << static_cast<int>(QPalette::Mid);
				QTest::newRow("base") << 12 << static_cast<int>(QPalette::Base);
				QTest::newRow("highlight-alias") << 13 << static_cast<int>(QPalette::Highlight);
				QTest::newRow("highlighted-text-alias") << 14 << static_cast<int>(QPalette::HighlightedText);
				QTest::newRow("button-alias") << 15 << static_cast<int>(QPalette::Button);
				QTest::newRow("shadow-alias") << 16 << static_cast<int>(QPalette::Shadow);
				QTest::newRow("mid-second-alias") << 17 << static_cast<int>(QPalette::Mid);
				QTest::newRow("button-text-alias") << 18 << static_cast<int>(QPalette::ButtonText);
				QTest::newRow("window-text-alias") << 19 << static_cast<int>(QPalette::WindowText);
				QTest::newRow("light") << 20 << static_cast<int>(QPalette::Light);
				QTest::newRow("dark-second-alias") << 21 << static_cast<int>(QPalette::Dark);
				QTest::newRow("light-alias") << 22 << static_cast<int>(QPalette::Light);
				QTest::newRow("tooltip-text") << 23 << static_cast<int>(QPalette::ToolTipText);
				QTest::newRow("tooltip-base") << 24 << static_cast<int>(QPalette::ToolTipBase);
			}

			static void systemColorIndicesMapToPaletteRoles()
			{
				QFETCH(int, index);
				QFETCH(int, role);

				PaletteRestorer restorer;
				QPalette        palette = QGuiApplication::palette();
				palette.setColor(static_cast<QPalette::ColorRole>(role), QColor(17, 83, 149));
				QGuiApplication::setPalette(palette);

				const auto colour = static_cast<QMudColorRef>(qmudGuiSystemColor(index));
				QCOMPARE(colour, QMudColorRef{0x00955311U});
				QCOMPARE(qmudRed(colour), quint8{17});
				QCOMPARE(qmudGreen(colour), quint8{83});
				QCOMPARE(qmudBlue(colour), quint8{149});
			}

			static void unsupportedSystemColorIndicesReturnZero()
			{
				QCOMPARE(qmudGuiSystemColor(-1), 0L);
				QCOMPARE(qmudGuiSystemColor(25), 0L);
			}

			static void collectionContainsCurrentDisplayAndInputSnapshot()
			{
				QScreen *screen = QGuiApplication::primaryScreen();
				QVERIFY(screen);

				const QVariantMap values = qmudCollectGuiSystemValues();
				QCOMPARE(qmudGuiSystemValueKey(QStringLiteral("metric"), 78), QStringLiteral("metric:78"));
				QCOMPARE(values.value(QStringLiteral("device:8")).toInt(), screen->geometry().width());
				QCOMPARE(values.value(QStringLiteral("device:10")).toInt(), screen->geometry().height());
				QCOMPARE(values.value(QStringLiteral("metric:0")).toInt(), screen->geometry().width());
				QCOMPARE(values.value(QStringLiteral("metric:1")).toInt(), screen->geometry().height());
				QCOMPARE(values.value(QStringLiteral("metric:80")).toInt(),
				         QGuiApplication::screens().size());
				QVERIFY(values.contains(QStringLiteral("inputMask")));
				QVERIFY(values.contains(QStringLiteral("menuFontSize")));

				for (int index = 0; index <= 24; ++index)
				{
					const QString key = qmudGuiSystemValueKey(QStringLiteral("syscolor"), index);
					QCOMPARE(values.value(key).toLongLong(),
					         static_cast<qlonglong>(qmudGuiSystemColor(index)));
				}
			}
	};
} // namespace

QTEST_MAIN(tst_GuiSystemUtils)

#if __has_include("tst_GuiSystemUtils.moc")
#include "tst_GuiSystemUtils.moc"
#endif
