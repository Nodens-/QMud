/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_NoteColourUtils.cpp
 * Role: Unit coverage for MUSHclient-compatible note colour option encoding.
 */

#include "WorldOptions.h"
#include "helpers/NoteColourUtils.h"

#include <QColor>
#include <QVector>
#include <QtTest/QTest>

namespace
{

	/**
	 * @brief QTest fixture for note colour option conversion helpers.
	 */
	class tst_NoteColourUtils : public QObject
	{
			Q_OBJECT

		private slots:
			/**
			 * @brief Verifies MUSHclient COLORREF-style persisted values map to public note indexes.
			 */
			static void worldAttributeValuesMapToPublicIndexes()
			{
				QCOMPARE(QMudNoteColour::publicIndexFromWorldAttribute(QStringLiteral("#040000")), 5);
				QCOMPARE(QMudNoteColour::publicIndexFromWorldAttribute(QStringLiteral("4")), 5);
				QCOMPARE(QMudNoteColour::publicIndexFromWorldAttribute(QStringLiteral("#ffffff")), 0);
				QCOMPARE(QMudNoteColour::publicIndexFromWorldAttribute(QStringLiteral("-1")), 0);
			}

			/**
			 * @brief Verifies invalid values use the supplied public-index fallback.
			 */
			static void invalidWorldAttributeValuesUseFallback()
			{
				QCOMPARE(QMudNoteColour::publicIndexFromWorldAttribute(QString(), 7), 7);
				QCOMPARE(QMudNoteColour::publicIndexFromWorldAttribute(QStringLiteral("not-a-colour"), 8), 8);
				QCOMPARE(QMudNoteColour::publicIndexFromWorldAttribute(QStringLiteral("#00ff00"), 9), 9);
			}

			/**
			 * @brief Verifies legacy QMud RGB note colours map back to matching custom colour slots.
			 */
			static void legacyRgbWorldAttributeValuesMatchCustomTextColours()
			{
				QVector<QColor> customTextColours(MAX_CUSTOM, QColor(Qt::white));
				customTextColours[3] = QColor(0, 255, 255);
				customTextColours[8] = QColor(255, 0, 255);

				QCOMPARE(QMudNoteColour::publicIndexFromWorldAttribute(QStringLiteral("#00ffff"),
				                                                       customTextColours, 7),
				         4);
				QCOMPARE(QMudNoteColour::publicIndexFromWorldAttribute(QStringLiteral("#ff00ff"),
				                                                       customTextColours, 7),
				         9);
				QCOMPARE(QMudNoteColour::publicIndexFromWorldAttribute(QStringLiteral("#040000"),
				                                                       customTextColours, 7),
				         5);
				QCOMPARE(QMudNoteColour::publicIndexFromWorldAttribute(QStringLiteral("#ffffff"),
				                                                       customTextColours, 7),
				         0);
			}

			/**
			 * @brief Verifies public note indexes persist in MUSHclient-compatible form.
			 */
			static void publicIndexesMapToWorldAttributeValues()
			{
				QCOMPARE(QMudNoteColour::worldAttributeFromPublicIndex(0), QStringLiteral("#ffffff"));
				QCOMPARE(QMudNoteColour::worldAttributeFromPublicIndex(5), QStringLiteral("#040000"));
				QCOMPARE(QMudNoteColour::worldAttributeFromPublicIndex(16), QStringLiteral("#0f0000"));
			}

			/**
			 * @brief Verifies runtime note indexes map to public SetNoteColour/GetNoteColour indexes.
			 */
			static void runtimeIndexesMapToPublicIndexes()
			{
				QCOMPARE(QMudNoteColour::publicIndexFromRuntimeIndex(-1), 0);
				QCOMPARE(QMudNoteColour::publicIndexFromRuntimeIndex(0), 1);
				QCOMPARE(QMudNoteColour::publicIndexFromRuntimeIndex(4), 5);
			}
	};

} // namespace

QTEST_APPLESS_MAIN(tst_NoteColourUtils)

#if __has_include("tst_NoteColourUtils.moc")
#include "tst_NoteColourUtils.moc"
#endif
