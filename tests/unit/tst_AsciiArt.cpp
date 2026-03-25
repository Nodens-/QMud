/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_AsciiArt.cpp
 * Role: QTest coverage for AsciiArt behavior.
 */

#include "AsciiArt.h"

#include <QtTest/QTest>

/**
 * @brief QTest fixture covering AsciiArt scenarios.
 */
class tst_AsciiArt : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void nullOutputLinesPointer()
		{
			QString error;
			QVERIFY(!QMudAsciiArt::render(QStringLiteral("test"), QStringLiteral("missing.flf"), 0, nullptr,
			                              &error));
		}

		void missingFontFile()
		{
			QStringList lines;
			QString     error;
			QVERIFY(!QMudAsciiArt::render(QStringLiteral("test"), QStringLiteral("missing.flf"), 0, &lines,
			                              &error));
			QVERIFY(error.contains(QStringLiteral("Could not open FIGlet font")));
		}

		void invalidFontHeader()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString badFontPath = tempDir.filePath(QStringLiteral("bad.flf"));
			QFile         badFont(badFontPath);
			QVERIFY(badFont.open(QIODevice::WriteOnly | QIODevice::Text));
			badFont.write("not-a-figlet-font\n");
			badFont.close();

			QStringList lines;
			QString     error;
			QVERIFY(!QMudAsciiArt::render(QStringLiteral("test"), badFontPath, 0, &lines, &error));
			QVERIFY(error.contains(QStringLiteral("Not a FIGlet 2 font file")));
		}

		void renderFromBundledFont()
		{
			const QString fontPath = QDir(QStringLiteral(QMUD_TEST_SOURCE_DIR))
			                             .filePath(QStringLiteral("skeleton/fonts/small.flf"));
			QVERIFY2(QFileInfo::exists(fontPath), qPrintable(fontPath));

			QStringList lines;
			QString     error;
			QVERIFY(QMudAsciiArt::render(QStringLiteral("QMud"), fontPath, 0, &lines, &error));
			QVERIFY(lines.size() > 0);

			bool hasVisibleContent = false;
			for (const QString &line : lines)
			{
				if (!line.trimmed().isEmpty())
				{
					hasVisibleContent = true;
					break;
				}
			}
			QVERIFY(hasVisibleContent);
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_AsciiArt)


#if __has_include("tst_AsciiArt.moc")
#include "tst_AsciiArt.moc"
#endif
