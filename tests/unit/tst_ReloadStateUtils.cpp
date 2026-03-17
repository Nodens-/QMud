/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_ReloadStateUtils.cpp
 * Role: QTest coverage for reload-state manifest parsing, persistence, and stale-file helpers.
 */

#include "ReloadStateUtils.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QFile>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryDir>
#include <QtTest/QTest>

/**
 * @brief QTest fixture covering reload-state helper behavior.
 */
class tst_ReloadStateUtils : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void parseReloadStartupArgumentsRequiresBothValues()
		{
			QString statePath;
			QString token;
			QVERIFY(!parseReloadStartupArguments({QStringLiteral("QMud"), QStringLiteral("--reload-state=a")},
			                                     &statePath, &token));
			QVERIFY(!parseReloadStartupArguments({QStringLiteral("QMud"), QStringLiteral("--reload-token=b")},
			                                     &statePath, &token));
			QVERIFY(
			    parseReloadStartupArguments({QStringLiteral("QMud"), QStringLiteral("--reload-state=/tmp/x"),
			                                 QStringLiteral("--reload-token=tkn")},
			                                &statePath, &token));
			QCOMPARE(statePath, QStringLiteral("/tmp/x"));
			QCOMPARE(token, QStringLiteral("tkn"));
		}

		void filterReloadStartupArgumentsRemovesReloadFlagsOnly()
		{
			const QStringList filtered = filterReloadStartupArguments(
			    {QStringLiteral("QMud"), QStringLiteral("--reload-state=/tmp/state.json"),
			     QStringLiteral("--reload-token=abc123"), QStringLiteral("--noauto"),
			     QStringLiteral("worlds/test.qdl")});
			const QStringList expected = {QStringLiteral("QMud"), QStringLiteral("--noauto"),
			                              QStringLiteral("worlds/test.qdl")};
			QCOMPARE(filtered, expected);
		}

		void writeAndReadRoundTrip()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString       path = reloadStateDefaultPath(tempDir.path());

			ReloadStateSnapshot snapshot;
			snapshot.schemaVersion    = 1;
			snapshot.createdAtUtc     = QDateTime::currentDateTimeUtc();
			snapshot.reloadToken      = QStringLiteral("reload-token");
			snapshot.sourcePid        = 1234;
			snapshot.targetExecutable = QStringLiteral("/tmp/QMud");
			snapshot.arguments        = {QStringLiteral("--foo"), QStringLiteral("--bar")};

			ReloadWorldState world;
			world.sequence             = 1;
			world.worldId              = QStringLiteral("world-1");
			world.displayName          = QStringLiteral("World One");
			world.worldFilePath        = QStringLiteral("/tmp/world.qdl");
			world.host                 = QStringLiteral("mud.example.org");
			world.port                 = 4000;
			world.socketDescriptor     = 42;
			world.policy               = ReloadSocketPolicy::ParkReconnect;
			world.connectedAtReload    = true;
			world.mccpWasActive        = true;
			world.mccpDisableAttempted = true;
			world.mccpDisableSucceeded = false;
			world.utf8Enabled          = true;
			world.notes                = QStringLiteral("test");
			snapshot.worlds.push_back(world);

			QString error;
			QVERIFY(writeReloadStateSnapshot(path, snapshot, &error));
			QVERIFY2(error.isEmpty(), qPrintable(error));

			ReloadStateSnapshot parsed;
			QVERIFY(readReloadStateSnapshot(path, &parsed, &error));
			QVERIFY2(error.isEmpty(), qPrintable(error));

			QCOMPARE(parsed.schemaVersion, 1);
			QCOMPARE(parsed.reloadToken, QStringLiteral("reload-token"));
			QCOMPARE(parsed.sourcePid, 1234);
			QCOMPARE(parsed.targetExecutable, QStringLiteral("/tmp/QMud"));
			QCOMPARE(parsed.arguments.size(), 2);
			QCOMPARE(parsed.worlds.size(), 1);
			QCOMPARE(parsed.worlds.at(0).worldId, QStringLiteral("world-1"));
			QCOMPARE(parsed.worlds.at(0).policy, ReloadSocketPolicy::ParkReconnect);
			QCOMPARE(parsed.worlds.at(0).socketDescriptor, 42);
			QVERIFY(parsed.worlds.at(0).connectedAtReload);
			QVERIFY(parsed.worlds.at(0).mccpWasActive);
			QVERIFY(parsed.worlds.at(0).mccpDisableAttempted);
			QVERIFY(!parsed.worlds.at(0).mccpDisableSucceeded);
		}

		void staleFileDetectionUsesModificationTimestamp()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString       path = reloadStateDefaultPath(tempDir.path());

			ReloadStateSnapshot snapshot;
			snapshot.createdAtUtc     = QDateTime::currentDateTimeUtc();
			snapshot.reloadToken      = QStringLiteral("reload-token");
			snapshot.targetExecutable = QStringLiteral("/tmp/QMud");

			QString error;
			QVERIFY(writeReloadStateSnapshot(path, snapshot, &error));
			QVERIFY2(error.isEmpty(), qPrintable(error));
			QVERIFY(!isReloadStateFileStale(path, 60, QDateTime::currentDateTimeUtc()));
			QVERIFY(isReloadStateFileStale(path, 60, QDateTime::currentDateTimeUtc().addSecs(600)));
		}

		void removeReloadStateFileDeletesExistingFile()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString       path = reloadStateDefaultPath(tempDir.path());

			ReloadStateSnapshot snapshot;
			snapshot.createdAtUtc     = QDateTime::currentDateTimeUtc();
			snapshot.reloadToken      = QStringLiteral("reload-token");
			snapshot.targetExecutable = QStringLiteral("/tmp/QMud");

			QString error;
			QVERIFY(writeReloadStateSnapshot(path, snapshot, &error));
			QVERIFY(QFile::exists(path));

			QVERIFY(removeReloadStateFile(path, &error));
			QVERIFY2(error.isEmpty(), qPrintable(error));
			QVERIFY(!QFile::exists(path));
			QVERIFY(removeReloadStateFile(path, &error));
		}

		void readRejectsUnsupportedSchemaVersion()
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
  "reload_token": "abc",
  "source_pid": 1,
  "target_executable": "/tmp/QMud",
  "arguments": [],
  "worlds": []
}
)json";
			QCOMPARE(file.write(payload), payload.size());
			file.close();

			ReloadStateSnapshot snapshot;
			QString             error;
			QVERIFY(!readReloadStateSnapshot(path, &snapshot, &error));
			QVERIFY(!error.isEmpty());
		}

		void readRejectsInvalidWorldPolicy()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString path = reloadStateDefaultPath(tempDir.path());

			QFile         file(path);
			QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
			const QByteArray payload = R"json(
{
  "schema_version": 1,
  "created_at_utc": "2026-03-17T12:00:00.000Z",
  "reload_token": "abc",
  "source_pid": 1,
  "target_executable": "/tmp/QMud",
  "arguments": [],
  "worlds": [
    {
      "sequence": 1,
      "world_id": "w",
      "display_name": "W",
      "world_file_path": "/tmp/w.qdl",
      "host": "example.org",
      "port": 4000,
      "fd": 10,
      "policy": "invalid_policy",
      "connected_at_reload": true,
      "mccp_was_active": false,
      "mccp_disable_attempted": false,
      "mccp_disable_succeeded": false,
      "utf8_enabled": true,
      "notes": ""
    }
  ]
}
)json";
			QCOMPARE(file.write(payload), payload.size());
			file.close();

			ReloadStateSnapshot snapshot;
			QString             error;
			QVERIFY(!readReloadStateSnapshot(path, &snapshot, &error));
			QVERIFY(!error.isEmpty());
		}

		void readRejectsOutOfRangeWorldPort()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString path = reloadStateDefaultPath(tempDir.path());

			QFile         file(path);
			QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
			const QByteArray payload = R"json(
{
  "schema_version": 1,
  "created_at_utc": "2026-03-17T12:00:00.000Z",
  "reload_token": "abc",
  "source_pid": 1,
  "target_executable": "/tmp/QMud",
  "arguments": [],
  "worlds": [
    {
      "sequence": 1,
      "world_id": "w",
      "display_name": "W",
      "world_file_path": "/tmp/w.qdl",
      "host": "example.org",
      "port": 70000,
      "fd": 10,
      "policy": "reattach",
      "connected_at_reload": true,
      "mccp_was_active": false,
      "mccp_disable_attempted": false,
      "mccp_disable_succeeded": false,
      "utf8_enabled": true,
      "notes": ""
    }
  ]
}
)json";
			QCOMPARE(file.write(payload), payload.size());
			file.close();

			ReloadStateSnapshot snapshot;
			QString             error;
			QVERIFY(!readReloadStateSnapshot(path, &snapshot, &error));
			QVERIFY(!error.isEmpty());
		}

		void readRejectsFractionalWorldNumericFields()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString path = reloadStateDefaultPath(tempDir.path());

			QFile         file(path);
			QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
			const QByteArray payload = R"json(
{
  "schema_version": 1,
  "created_at_utc": "2026-03-17T12:00:00.000Z",
  "reload_token": "abc",
  "source_pid": 1,
  "target_executable": "/tmp/QMud",
  "arguments": [],
  "worlds": [
    {
      "sequence": 1.5,
      "world_id": "w",
      "display_name": "W",
      "world_file_path": "/tmp/w.qdl",
      "host": "example.org",
      "port": 4000,
      "fd": 10,
      "policy": "reattach",
      "connected_at_reload": true,
      "mccp_was_active": false,
      "mccp_disable_attempted": false,
      "mccp_disable_succeeded": false,
      "utf8_enabled": true,
      "notes": ""
    }
  ]
}
)json";
			QCOMPARE(file.write(payload), payload.size());
			file.close();

			ReloadStateSnapshot snapshot;
			QString             error;
			QVERIFY(!readReloadStateSnapshot(path, &snapshot, &error));
			QVERIFY(!error.isEmpty());
		}

		void staleDetectionReturnsFalseForMissingFile()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString missingPath = tempDir.filePath(QStringLiteral("missing_reload_state.json"));
			QVERIFY(!QFile::exists(missingPath));
			QVERIFY(!isReloadStateFileStale(missingPath, 60, QDateTime::currentDateTimeUtc()));
		}

		void removeReloadStateFileAcceptsEmptyPath()
		{
			QString error;
			QVERIFY(removeReloadStateFile(QString(), &error));
			QVERIFY(error.isEmpty());
		}

		void validateReloadStartupSnapshotAcceptsMatchingMetadata()
		{
			const QString       executable = QStringLiteral("/tmp/qmud-test-executable");
			ReloadStateSnapshot snapshot;
			snapshot.reloadToken      = QStringLiteral("token");
			snapshot.sourcePid        = 4242;
			snapshot.targetExecutable = executable;

			const ReloadStartupValidationInput input{
			    QStringLiteral("token"),
			    4242,
			    executable,
			};
			QString error;
			QVERIFY(validateReloadStartupSnapshot(snapshot, input, &error));
			QVERIFY(error.isEmpty());
		}

		void validateReloadStartupSnapshotRejectsTokenMismatch()
		{
			const QString       executable = QStringLiteral("/tmp/qmud-test-executable");
			ReloadStateSnapshot snapshot;
			snapshot.reloadToken      = QStringLiteral("token-a");
			snapshot.sourcePid        = 4242;
			snapshot.targetExecutable = executable;

			const ReloadStartupValidationInput input{
			    QStringLiteral("token-b"),
			    4242,
			    executable,
			};
			QString error;
			QVERIFY(!validateReloadStartupSnapshot(snapshot, input, &error));
			QVERIFY(!error.isEmpty());
		}

		void validateReloadStartupSnapshotRejectsPidMismatch()
		{
			const QString       executable = QStringLiteral("/tmp/qmud-test-executable");
			ReloadStateSnapshot snapshot;
			snapshot.reloadToken      = QStringLiteral("token");
			snapshot.sourcePid        = 4242;
			snapshot.targetExecutable = executable;

			const ReloadStartupValidationInput input{
			    QStringLiteral("token"),
			    5252,
			    executable,
			};
			QString error;
			QVERIFY(!validateReloadStartupSnapshot(snapshot, input, &error));
			QVERIFY(!error.isEmpty());
		}

		void validateReloadStartupSnapshotRejectsExecutableMismatch()
		{
			const QString       executableA = QStringLiteral("/tmp/qmud-test-executable-a");
			const QString       executableB = QStringLiteral("/tmp/qmud-test-executable-b");
			ReloadStateSnapshot snapshot;
			snapshot.reloadToken      = QStringLiteral("token");
			snapshot.sourcePid        = 4242;
			snapshot.targetExecutable = executableA;

			const ReloadStartupValidationInput input{
			    QStringLiteral("token"),
			    4242,
			    executableB,
			};
			QString error;
			QVERIFY(!validateReloadStartupSnapshot(snapshot, input, &error));
			QVERIFY(!error.isEmpty());
		}

		void singleUseFlowConsumesManifestAfterValidation()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString       path = reloadStateDefaultPath(tempDir.path());

			ReloadStateSnapshot snapshot;
			snapshot.schemaVersion    = 1;
			snapshot.createdAtUtc     = QDateTime::currentDateTimeUtc();
			snapshot.reloadToken      = QStringLiteral("single-use-token");
			snapshot.sourcePid        = 7777;
			snapshot.targetExecutable = QStringLiteral("/tmp/qmud-single-use");

			QString error;
			QVERIFY(writeReloadStateSnapshot(path, snapshot, &error));
			QVERIFY(QFile::exists(path));

			ReloadStateSnapshot parsed;
			QVERIFY(readReloadStateSnapshot(path, &parsed, &error));
			const ReloadStartupValidationInput validationInput{
			    QStringLiteral("single-use-token"),
			    7777,
			    QStringLiteral("/tmp/qmud-single-use"),
			};
			QVERIFY(validateReloadStartupSnapshot(parsed, validationInput, &error));
			QVERIFY(removeReloadStateFile(path, &error));
			QVERIFY(!QFile::exists(path));
		}

		void loadValidatedAndConsumeRemovesStateOnSuccess()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString       path = reloadStateDefaultPath(tempDir.path());

			ReloadStateSnapshot snapshot;
			snapshot.schemaVersion    = 1;
			snapshot.createdAtUtc     = QDateTime::currentDateTimeUtc();
			snapshot.reloadToken      = QStringLiteral("consume-token");
			snapshot.sourcePid        = 3030;
			snapshot.targetExecutable = QStringLiteral("/tmp/qmud-consume");

			QString error;
			QVERIFY(writeReloadStateSnapshot(path, snapshot, &error));
			QVERIFY(QFile::exists(path));

			const ReloadStartupValidationInput input{
			    QStringLiteral("consume-token"),
			    3030,
			    QStringLiteral("/tmp/qmud-consume"),
			};
			ReloadStateSnapshot parsed;
			QString             cleanupWarning;
			QVERIFY(
			    loadValidatedAndConsumeReloadStateSnapshot(path, input, &parsed, &error, &cleanupWarning));
			QVERIFY(error.isEmpty());
			QVERIFY(cleanupWarning.isEmpty());
			QVERIFY(!QFile::exists(path));
		}

		void loadValidatedAndConsumeRejectsMalformedSchemaAndCleansUp()
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
  "target_executable": "/tmp/QMud",
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
			    QStringLiteral("/tmp/QMud"),
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

		void loadValidatedAndConsumeRejectsTokenMismatchAndCleansUp()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString       path = reloadStateDefaultPath(tempDir.path());

			ReloadStateSnapshot snapshot;
			snapshot.schemaVersion    = 1;
			snapshot.createdAtUtc     = QDateTime::currentDateTimeUtc();
			snapshot.reloadToken      = QStringLiteral("token-a");
			snapshot.sourcePid        = 4242;
			snapshot.targetExecutable = QStringLiteral("/tmp/qmud-token-check");

			QString error;
			QVERIFY(writeReloadStateSnapshot(path, snapshot, &error));
			QVERIFY(QFile::exists(path));

			const ReloadStartupValidationInput input{
			    QStringLiteral("token-b"),
			    4242,
			    QStringLiteral("/tmp/qmud-token-check"),
			};
			ReloadStateSnapshot parsed;
			QString             cleanupWarning;
			QVERIFY(
			    !loadValidatedAndConsumeReloadStateSnapshot(path, input, &parsed, &error, &cleanupWarning));
			QVERIFY(!error.isEmpty());
			QVERIFY(cleanupWarning.isEmpty());
			QVERIFY(!QFile::exists(path));
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_ReloadStateUtils)

#include "tst_ReloadStateUtils.moc"
