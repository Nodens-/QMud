/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_AcceleratorUtils.cpp
 * Role: QTest coverage for AcceleratorUtils behavior.
 */

#include "AcceleratorUtils.h"

#include <QtTest/QTest>

/**
 * @brief QTest fixture covering AcceleratorUtils scenarios.
 */
class tst_AcceleratorUtils : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void stringRoundTrip()
		{
			quint32 virt = 0;
			quint16 key  = 0;
			QVERIFY(AcceleratorUtils::stringToAccelerator(QStringLiteral("Ctrl+Shift+K"), virt, key));
			QVERIFY((virt & AcceleratorUtils::kControlFlag) != 0);
			QVERIFY((virt & AcceleratorUtils::kShiftFlag) != 0);
			QCOMPARE(AcceleratorUtils::acceleratorToString(virt, key), QStringLiteral("Ctrl+Shift+K"));
		}

		void metaModifierIsRejected()
		{
			quint32 virt = 0;
			quint16 key  = 0;
			QVERIFY(!AcceleratorUtils::stringToAccelerator(QStringLiteral("Meta+K"), virt, key));
		}

		void keypadMapping()
		{
			const quint16 key = AcceleratorUtils::qtKeyToVirtualKey(Qt::Key_1, true);
			QVERIFY(key != 0);
			QVERIFY(AcceleratorUtils::virtualKeyUsesKeypadModifier(key));
		}

		void legacyNumpadAcceleratorParsing()
		{
			quint32 virt = 0;
			quint16 key  = 0;
			QVERIFY(AcceleratorUtils::stringToAccelerator(QStringLiteral("alt+numpad6"), virt, key));
			QVERIFY((virt & AcceleratorUtils::kAltFlag) != 0);
			QCOMPARE(key, AcceleratorUtils::qtKeyToVirtualKey(Qt::Key_6, true));
		}

		void legacyAddSubtractParsing()
		{
			quint32 virt = 0;
			quint16 key  = 0;
			QVERIFY(AcceleratorUtils::stringToAccelerator(QStringLiteral("subtract"), virt, key));
			QCOMPARE(key, AcceleratorUtils::qtKeyToVirtualKey(Qt::Key_Minus, true));
			QVERIFY((virt & AcceleratorUtils::kAltFlag) == 0);

			QVERIFY(AcceleratorUtils::stringToAccelerator(QStringLiteral("ctrl+alt+add"), virt, key));
			QVERIFY((virt & AcceleratorUtils::kControlFlag) != 0);
			QVERIFY((virt & AcceleratorUtils::kAltFlag) != 0);
			QCOMPARE(key, AcceleratorUtils::qtKeyToVirtualKey(Qt::Key_Plus, true));
		}

		void virtualKeySymmetry()
		{
			constexpr quint16 keys[] = {0x41, 0x70, 0x25};
			for (const quint16 vk : keys)
			{
				const Qt::Key qtKey = AcceleratorUtils::virtualKeyToQtKey(vk);
				QVERIFY(static_cast<int>(qtKey) != 0);
				QCOMPARE(AcceleratorUtils::qtKeyToVirtualKey(qtKey, false), vk);
			}
		}

		void unknownKeyFormatting()
		{
			const QString text = AcceleratorUtils::acceleratorToString(AcceleratorUtils::kVirtKeyFlag,
			                                                           static_cast<quint16>(0));
			QVERIFY(text.isEmpty());
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_GUILESS_MAIN(tst_AcceleratorUtils)

#if __has_include("tst_AcceleratorUtils.moc")
#include "tst_AcceleratorUtils.moc"
#endif
