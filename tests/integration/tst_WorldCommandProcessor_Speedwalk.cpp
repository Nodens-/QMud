/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldCommandProcessor_Speedwalk.cpp
 * Role: QTest coverage for WorldCommandProcessor Speedwalk behavior.
 */

#include "SpeedwalkParser.h"

#include <QtTest/QTest>

namespace
{
	QString resolveDirection(const QString &direction)
	{
		static const QHash<QString, QString> kDirections = {
		    {QStringLiteral("n"), QStringLiteral("north")},
            {QStringLiteral("s"), QStringLiteral("south")},
		    {QStringLiteral("e"), QStringLiteral("east") },
            {QStringLiteral("w"), QStringLiteral("west") },
		    {QStringLiteral("u"), QStringLiteral("up")   },
            {QStringLiteral("d"), QStringLiteral("down") }
        };
		return kDirections.value(direction.trimmed().toLower());
	}
} // namespace

/**
 * @brief QTest fixture covering WorldCommandProcessor Speedwalk scenarios.
 */
class tst_WorldCommandProcessor_Speedwalk : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void evaluateValidVectors_data()
		{
			QTest::addColumn<QString>("input");
			QTest::addColumn<QString>("filler");
			QTest::addColumn<QString>("expected");

			QTest::newRow("basic-counts") << QStringLiteral("2n 3e") << QString()
			                              << QStringLiteral("north\r\nnorth\r\neast\r\neast\r\neast\r\n");
			QTest::newRow("action-codes")
			    << QStringLiteral("c n o e l w k s") << QString()
			    << QStringLiteral("close north\r\nopen east\r\nlock west\r\nunlock south\r\n");
			QTest::newRow("comment-and-parentheses") << QStringLiteral("{skip this} (go north/go south) u")
			                                         << QString() << QStringLiteral("go north\r\nup\r\n");
			QTest::newRow("filler") << QStringLiteral("3f") << QStringLiteral(".")
			                        << QStringLiteral(".\r\n.\r\n.\r\n");
			QTest::newRow("mixed") << QStringLiteral("2d (say hi/unused) f") << QStringLiteral("wait")
			                       << QStringLiteral("down\r\ndown\r\nsay hi\r\nwait\r\n");
		}

		void evaluateValidVectors()
		{
			QFETCH(QString, input);
			QFETCH(QString, filler);
			QFETCH(QString, expected);

			const QString actual = QMudSpeedwalk::evaluateSpeedwalk(input, filler, resolveDirection);
			QCOMPARE(actual, expected);
		}

		void evaluateErrorVectors_data()
		{
			QTest::addColumn<QString>("input");
			QTest::addColumn<QString>("expected");

			QTest::newRow("unterminated-comment")
			    << QStringLiteral("{oops") << QStringLiteral("*Comment code of '{' not terminated by a '}'");
			QTest::newRow("counter-exceeds")
			    << QStringLiteral("100n") << QStringLiteral("*Speed walk counter exceeds 99");
			QTest::newRow("counter-without-action")
			    << QStringLiteral("2") << QStringLiteral("*Speed walk counter not followed by an action");
			QTest::newRow("counter-followed-by-comment")
			    << QStringLiteral("2 {x}")
			    << QStringLiteral("*Speed walk counter may not be followed by a comment");
			QTest::newRow("count-before-action-code")
			    << QStringLiteral("2c n")
			    << QStringLiteral("*Action code of C, O, L or K must not follow a speed walk count (1-99)");
			QTest::newRow("action-code-without-direction")
			    << QStringLiteral("c")
			    << QStringLiteral("*Action code of C, O, L or K must be followed by a direction");
			QTest::newRow("unterminated-parentheses")
			    << QStringLiteral("(hello") << QStringLiteral("*Action code of '(' not terminated by a ')'");
			QTest::newRow("invalid-direction")
			    << QStringLiteral("x")
			    << QStringLiteral(
			           "*Invalid direction 'x' in speed walk, must be N, S, E, W, U, D, F, or (something)");
		}

		void evaluateErrorVectors()
		{
			QFETCH(QString, input);
			QFETCH(QString, expected);

			const QString actual =
			    QMudSpeedwalk::evaluateSpeedwalk(input, QStringLiteral("f"), resolveDirection);
			QCOMPARE(actual, expected);
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_WorldCommandProcessor_Speedwalk)


#if __has_include("tst_WorldCommandProcessor_Speedwalk.moc")
#include "tst_WorldCommandProcessor_Speedwalk.moc"
#endif
