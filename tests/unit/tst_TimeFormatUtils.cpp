/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_TimeFormatUtils.cpp
 * Role: QTest coverage for shared TimeFormatUtils behavior.
 */

#include "TimeFormatUtils.h"

#include <QtTest/QTest>

namespace
{
	int     g_fixupCallCount = 0;

	QString fixupRecorder(const QString &value)
	{
		++g_fixupCallCount;
		return QStringLiteral("h[%1]").arg(value);
	}
} // namespace

class tst_TimeFormatUtils : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void resolveWorkingDirPrefersExplicitStartupDir()
		{
			const QString startupDir = QStringLiteral("/tmp/qmud-explicit-startup/");
			QCOMPARE(TimeFormatUtils::resolveWorkingDir(startupDir), startupDir);
		}

		void makeAbsolutePathResolvesRelativeAndPreservesTrailingSlash()
		{
			const QString workingDir = QStringLiteral("/tmp/qmud-working-dir/");
			QCOMPARE(TimeFormatUtils::makeAbsolutePath(QStringLiteral("./logs/session.log"), workingDir),
			         QStringLiteral("/tmp/qmud-working-dir/logs/session.log"));
			QCOMPARE(TimeFormatUtils::makeAbsolutePath(QStringLiteral("./logs/"), workingDir),
			         QStringLiteral("/tmp/qmud-working-dir/logs/"));
		}

		void formatWorldTimeReplacesContextDirectivesAndEscapesPercent()
		{
			TimeFormatUtils::WorldTimeFormatContext context;
			context.workingDir = QStringLiteral("work%dir");
			context.worldName  = QStringLiteral("world%name");
			context.playerName = QStringLiteral("player%name");
			context.worldDir   = QStringLiteral("worlds%dir");
			context.logDir     = QStringLiteral("logs%dir");

			const QDateTime time = QDateTime::fromString(QStringLiteral("2026-03-26T14:15:16"), Qt::ISODate);
			QVERIFY(time.isValid());

			const QString formatted = TimeFormatUtils::formatWorldTime(
			    time, QStringLiteral("%E|%N|%P|%F|%L|%%"), context, false, nullptr);
			QCOMPARE(formatted, QStringLiteral("work%dir|world%name|player%name|worlds%dir|logs%dir|%"));
		}

		void formatWorldTimeFixHtmlUsesProvidedCallback()
		{
			TimeFormatUtils::WorldTimeFormatContext context;
			context.workingDir = QStringLiteral("wd");
			context.worldName  = QStringLiteral("wn");
			context.playerName = QStringLiteral("pn");
			context.worldDir   = QStringLiteral("wdir");
			context.logDir     = QStringLiteral("ldir");

			const QDateTime time = QDateTime::fromString(QStringLiteral("2026-03-26T14:15:16"), Qt::ISODate);
			QVERIFY(time.isValid());

			g_fixupCallCount        = 0;
			const QString formatted = TimeFormatUtils::formatWorldTime(time, QStringLiteral("%E|%N|%P|%F|%L"),
			                                                           context, true, &fixupRecorder);
			QCOMPARE(formatted, QStringLiteral("h[wd]|h[wn]|h[pn]|h[wdir]|h[ldir]"));
			QCOMPARE(g_fixupCallCount, 5);
		}

		void formatWorldTimeNoPadTokensAndUnknownTokenFallback()
		{
			const QDateTime time = QDateTime::fromString(QStringLiteral("2026-03-04T05:06:07"), Qt::ISODate);
			QVERIFY(time.isValid());

			const QString noPad = TimeFormatUtils::formatWorldTime(
			    time, QStringLiteral("%#d-%#m-%Y %#H:%#M:%#S %#I"), {}, false, nullptr);
			QCOMPARE(noPad, QStringLiteral("4-3-2026 5:6:7 5"));

			const QString unknown =
			    TimeFormatUtils::formatWorldTime(time, QStringLiteral("%q %Y"), {}, false, nullptr);
			QCOMPARE(unknown, QStringLiteral("%q 2026"));
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_TimeFormatUtils)

#if __has_include("tst_TimeFormatUtils.moc")
#include "tst_TimeFormatUtils.moc"
#endif
