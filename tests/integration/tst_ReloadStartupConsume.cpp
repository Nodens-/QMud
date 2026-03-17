/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_ReloadStartupConsume.cpp
 * Role: Integration-level QTest coverage for reload startup manifest consumption and malformed-state cleanup.
 */

#include "ReloadStateUtils.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QFile>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryDir>
#include <QtTest/QTest>

/**
 * @brief QTest fixture covering startup consume/cleanup behavior for reload manifests.
 */
class tst_ReloadStartupConsume : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void startupConsumeRemovesValidManifest()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString       path = reloadStateDefaultPath(tempDir.path());

			ReloadStateSnapshot snapshot;
			snapshot.schemaVersion    = 1;
			snapshot.createdAtUtc     = QDateTime::currentDateTimeUtc();
			snapshot.reloadToken      = QStringLiteral("integration-token");
			snapshot.sourcePid        = 1010;
			snapshot.targetExecutable = QStringLiteral("/tmp/qmud-integration");

			QString error;
			QVERIFY(writeReloadStateSnapshot(path, snapshot, &error));
			QVERIFY(QFile::exists(path));

			const ReloadStartupValidationInput input{
			    QStringLiteral("integration-token"),
			    1010,
			    QStringLiteral("/tmp/qmud-integration"),
			};
			ReloadStateSnapshot parsed;
			QString             cleanupWarning;
			QVERIFY(
			    loadValidatedAndConsumeReloadStateSnapshot(path, input, &parsed, &error, &cleanupWarning));
			QVERIFY(error.isEmpty());
			QVERIFY(cleanupWarning.isEmpty());
			QVERIFY(!QFile::exists(path));
			QCOMPARE(parsed.reloadToken, QStringLiteral("integration-token"));
		}

		void startupConsumeCleansUpMalformedManifest()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString path = reloadStateDefaultPath(tempDir.path());

			QFile         file(path);
			QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
			const QByteArray payload = R"json(
{
  "schema_version": 999,
  "created_at_utc": "2026-03-17T12:00:00.000Z",
  "reload_token": "bad",
  "source_pid": 1,
  "target_executable": "/tmp/qmud-integration",
  "arguments": [],
  "worlds": []
}
)json";
			QCOMPARE(file.write(payload), payload.size());
			file.close();
			QVERIFY(QFile::exists(path));

			const ReloadStartupValidationInput input{
			    QStringLiteral("bad"),
			    1,
			    QStringLiteral("/tmp/qmud-integration"),
			};
			ReloadStateSnapshot parsed;
			QString             error;
			QString             cleanupWarning;
			QVERIFY(
			    !loadValidatedAndConsumeReloadStateSnapshot(path, input, &parsed, &error, &cleanupWarning));
			QVERIFY(!error.isEmpty());
			QVERIFY(cleanupWarning.isEmpty());
			QVERIFY(!QFile::exists(path));
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_ReloadStartupConsume)

#include "tst_ReloadStartupConsume.moc"
