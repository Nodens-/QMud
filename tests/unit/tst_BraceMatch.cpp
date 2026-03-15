/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_BraceMatch.cpp
 * Role: QTest coverage for BraceMatch behavior.
 */

#include "BraceMatch.h"

#include <QPlainTextEdit>
#include <QTextCursor>
#include <QtTest/QTest>

/**
 * @brief QTest fixture covering BraceMatch scenarios.
 */
class tst_BraceMatch : public QObject
{
		Q_OBJECT

	// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void findsForwardMatchWithSelection()
		{
			constexpr int nestedPairs = 0x0001;

			QPlainTextEdit edit;
			edit.setPlainText(QStringLiteral("a(b[c]d)e"));
			QTextCursor cursor = edit.textCursor();
			cursor.setPosition(1);
			edit.setTextCursor(cursor);

			QVERIFY(QMudBraceMatch::findMatchingBrace(&edit, true, nestedPairs));
			QCOMPARE(edit.textCursor().selectedText(), QStringLiteral("(b[c]d)"));
		}

		void findsBackwardMatchWithoutSelection()
		{
			constexpr int nestedPairs = 0x0001;

			QPlainTextEdit edit;
			edit.setPlainText(QStringLiteral("a(b[c]d)e"));
			QTextCursor cursor = edit.textCursor();
			cursor.setPosition(7);
			edit.setTextCursor(cursor);

			QVERIFY(QMudBraceMatch::findMatchingBrace(&edit, false, nestedPairs));
			QCOMPARE(edit.textCursor().position(), 1);
		}

		void escapedDelimiterIsIgnored()
		{
			constexpr int nestedPairs     = 0x0001;
			constexpr int backslashEscape = 0x0020;

			QPlainTextEdit edit;
			edit.setPlainText(QStringLiteral("\\(x)"));
			QTextCursor cursor = edit.textCursor();
			cursor.setPosition(1);
			edit.setTextCursor(cursor);

			QVERIFY(!QMudBraceMatch::findMatchingBrace(&edit, false, nestedPairs | backslashEscape));
		}

		void noMatchReturnsFalse()
		{
			QPlainTextEdit edit;
			edit.setPlainText(QStringLiteral("(abc"));
			QTextCursor cursor = edit.textCursor();
			cursor.setPosition(0);
			edit.setTextCursor(cursor);

			QVERIFY(!QMudBraceMatch::findMatchingBrace(&edit, true, 0));
		}
	// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_MAIN(tst_BraceMatch)

#include "tst_BraceMatch.moc"

