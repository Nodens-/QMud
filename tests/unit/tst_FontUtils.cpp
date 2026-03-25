/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_FontUtils.cpp
 * Role: QTest coverage for FontUtils behavior.
 */

#include "FontUtils.h"

#include <QFont>
#include <QFontDatabase>
#include <QtTest/QTest>

/**
 * @brief QTest fixture covering FontUtils scenarios.
 */
class tst_FontUtils : public QObject
{
		Q_OBJECT

	// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void applyMonospaceFallback()
		{
			QFont font(QStringLiteral("RandomFamily"));
			qmudApplyMonospaceFallback(font);
			QVERIFY(font.fixedPitch());
			QCOMPARE(font.styleHint(), QFont::Monospace);
		}

		void preferredMonospaceFontHonorsSize()
		{
			const QFont font = qmudPreferredMonospaceFont(QStringLiteral("DejaVu Sans Mono"), 13);
			QVERIFY(font.fixedPitch());
			QCOMPARE(font.pointSize(), 13);
		}

		void mapCharsetKnownAndUnknown()
		{
			QFontDatabase::WritingSystem ws = QFontDatabase::Any;
			QVERIFY(qmudMapWindowsCharsetToWritingSystem(128, &ws));
			QCOMPARE(ws, QFontDatabase::Japanese);

			ws = QFontDatabase::Korean;
			QVERIFY(!qmudMapWindowsCharsetToWritingSystem(999, &ws));
			QCOMPARE(ws, QFontDatabase::Korean);
		}

		void familyForUnknownCharsetReturnsPreferred()
		{
			QCOMPARE(qmudFamilyForCharset(QStringLiteral("MyPreferredFamily"), 999),
			         QStringLiteral("MyPreferredFamily"));
		}
	// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_GUILESS_MAIN(tst_FontUtils)



#if __has_include("tst_FontUtils.moc")
#include "tst_FontUtils.moc"
#endif
