/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_ReloadUnit.cpp
 * Role: Consolidated unit-level QTest coverage for reload state, planning, and MCCP probe helpers.
 */

#include "ReloadUtils.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QFile>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryDir>
#include <QtTest/QTest>

/**
 * @brief QTest fixture covering reload-related unit helper behavior.
 */
class tst_ReloadUnit : public QObject
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
			snapshot.schemaVersion       = 1;
			snapshot.createdAtUtc        = QDateTime::currentDateTimeUtc();
			snapshot.reloadToken         = QStringLiteral("reload-token");
			snapshot.targetExecutable    = QStringLiteral("/tmp/QMud");
			snapshot.activeWorldSequence = 1;
			snapshot.arguments           = {QStringLiteral("--foo"), QStringLiteral("--bar")};

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
			QCOMPARE(parsed.targetExecutable, QStringLiteral("/tmp/QMud"));
			QCOMPARE(parsed.activeWorldSequence, 1);
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

		void readDefaultsActiveWorldSequenceWhenFieldMissing()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString path = reloadStateDefaultPath(tempDir.path());

			QFile         file(path);
			QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
			const QByteArray payload = R"json(
{
  "schema_version": 1,
  "created_at_utc": "2026-03-20T12:00:00.000Z",
  "reload_token": "token",
  "target_executable": "/tmp/qmud",
  "arguments": [],
  "worlds": []
}
)json";
			QCOMPARE(file.write(payload), payload.size());
			file.close();

			ReloadStateSnapshot snapshot;
			QString             error;
			QVERIFY(readReloadStateSnapshot(path, &snapshot, &error));
			QVERIFY2(error.isEmpty(), qPrintable(error));
			QCOMPARE(snapshot.activeWorldSequence, 0);
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
			snapshot.targetExecutable = executable;

			const ReloadStartupValidationInput input{
			    QStringLiteral("token"),
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
			snapshot.targetExecutable = executable;

			const ReloadStartupValidationInput input{
			    QStringLiteral("token-b"),
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
			snapshot.targetExecutable = executableA;

			const ReloadStartupValidationInput input{
			    QStringLiteral("token"),
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
			snapshot.targetExecutable = QStringLiteral("/tmp/qmud-single-use");

			QString error;
			QVERIFY(writeReloadStateSnapshot(path, snapshot, &error));
			QVERIFY(QFile::exists(path));

			ReloadStateSnapshot parsed;
			QVERIFY(readReloadStateSnapshot(path, &parsed, &error));
			const ReloadStartupValidationInput validationInput{
			    QStringLiteral("single-use-token"),
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
			snapshot.targetExecutable = QStringLiteral("/tmp/qmud-consume");

			QString error;
			QVERIFY(writeReloadStateSnapshot(path, snapshot, &error));
			QVERIFY(QFile::exists(path));

			const ReloadStartupValidationInput input{
			    QStringLiteral("consume-token"),
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
			snapshot.targetExecutable = QStringLiteral("/tmp/qmud-token-check");

			QString error;
			QVERIFY(writeReloadStateSnapshot(path, snapshot, &error));
			QVERIFY(QFile::exists(path));

			const ReloadStartupValidationInput input{
			    QStringLiteral("token-b"),
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

		void shouldAttemptMccpDisableRequiresConnectedDescriptorAndActiveMccp()
		{
			QVERIFY(shouldAttemptReloadMccpDisable(true, 10, false, true));
			QVERIFY(!shouldAttemptReloadMccpDisable(false, 10, false, true));
			QVERIFY(!shouldAttemptReloadMccpDisable(true, -1, false, true));
			QVERIFY(!shouldAttemptReloadMccpDisable(true, 10, false, false));
			QVERIFY(!shouldAttemptReloadMccpDisable(true, 10, true, true));
		}

		void disconnectedWorldIsNotRecovered()
		{
			const ReloadWorldPolicyDecision decision =
			    computeReloadWorldPolicy({false, false, -1, false, false, false});
			QVERIFY(!decision.connectedAtReload);
			QVERIFY(!decision.shouldAttemptDescriptorInheritance);
			QCOMPARE(decision.policy, ReloadSocketPolicy::Reattach);
		}

		void connectedWorldWithoutMccpUsesReattach()
		{
			const ReloadWorldPolicyDecision decision =
			    computeReloadWorldPolicy({true, false, 42, false, false, false});
			QVERIFY(decision.connectedAtReload);
			QVERIFY(decision.shouldAttemptDescriptorInheritance);
			QCOMPARE(decision.policy, ReloadSocketPolicy::Reattach);
		}

		void connectedWorldWithMccpTimeoutStillUsesReattach()
		{
			const ReloadWorldPolicyDecision decision =
			    computeReloadWorldPolicy({true, false, 42, false, true, false});
			QVERIFY(decision.connectedAtReload);
			QVERIFY(decision.shouldAttemptDescriptorInheritance);
			QCOMPARE(decision.policy, ReloadSocketPolicy::Reattach);
		}

		void connectedWorldWithMccpDisableSuccessReattaches()
		{
			const ReloadWorldPolicyDecision decision =
			    computeReloadWorldPolicy({true, false, 42, false, true, true});
			QVERIFY(decision.connectedAtReload);
			QVERIFY(decision.shouldAttemptDescriptorInheritance);
			QCOMPARE(decision.policy, ReloadSocketPolicy::Reattach);
		}

		void tlsWorldAlwaysUsesReconnectPolicy()
		{
			const ReloadWorldPolicyDecision decision =
			    computeReloadWorldPolicy({true, false, 42, true, false, true});
			QVERIFY(decision.connectedAtReload);
			QVERIFY(!decision.shouldAttemptDescriptorInheritance);
			QCOMPARE(decision.policy, ReloadSocketPolicy::ParkReconnect);
		}

		void connectedWorldWithoutDescriptorFallsBackToReconnect()
		{
			const ReloadWorldPolicyDecision decision =
			    computeReloadWorldPolicy({true, false, -1, false, false, false});
			QVERIFY(decision.connectedAtReload);
			QVERIFY(!decision.shouldAttemptDescriptorInheritance);
			QCOMPARE(decision.policy, ReloadSocketPolicy::ParkReconnect);
		}

		void connectingWorldUsesReconnectPolicy()
		{
			const ReloadWorldPolicyDecision decision =
			    computeReloadWorldPolicy({false, true, 7, false, false, false});
			QVERIFY(decision.connectedAtReload);
			QVERIFY(decision.shouldAttemptDescriptorInheritance);
			QCOMPARE(decision.policy, ReloadSocketPolicy::ParkReconnect);
		}

		void asciiPayloadIsNotFlagged()
		{
			const QByteArray payload =
			    QByteArrayLiteral("You are standing in a dusty room with exits north and east.\r\n");
			QVERIFY(!isLikelyResidualMccpPayload(payload));
		}

		void exactlyHalfPrintablePayloadIsNotFlagged()
		{
			QByteArray payload;
			payload.append(QByteArrayLiteral("room here"));
			for (int i = 0; i < 8; ++i)
				payload.append(static_cast<char>(0x80 + i));
			QVERIFY(!isLikelyResidualMccpPayload(payload));
		}

		void belowHalfPrintablePayloadIsFlagged()
		{
			QByteArray payload;
			payload.append(QByteArrayLiteral("room"));
			for (int i = 0; i < 8; ++i)
				payload.append(static_cast<char>(0x80 + i));
			QVERIFY(isLikelyResidualMccpPayload(payload));
		}

		void binaryPayloadIsFlagged()
		{
			QByteArray payload;
			payload.reserve(48);
			for (int i = 0; i < 48; ++i)
				payload.append(static_cast<char>(0x80 + (i % 48)));
			QVERIFY(isLikelyResidualMccpPayload(payload));
		}

		void emptyPayloadIsFlagged()
		{
			QVERIFY(isLikelyResidualMccpPayload(QByteArray()));
		}

		void timeoutWithNoProbeBytesReconnects()
		{
			QVERIFY(shouldReconnectOnReloadMccpProbeTimeout(QByteArray()));
		}

		void timeoutWithPrintableProbeBytesKeepsReattach()
		{
			const QByteArray payload = QByteArrayLiteral("look output room description\r\n");
			QVERIFY(!shouldReconnectOnReloadMccpProbeTimeout(payload));
		}

		void timeoutWithMostlyBinaryProbeBytesReconnects()
		{
			QByteArray payload;
			payload.append(QByteArrayLiteral("look"));
			for (int i = 0; i < 8; ++i)
				payload.append(static_cast<char>(0x80 + i));
			QVERIFY(shouldReconnectOnReloadMccpProbeTimeout(payload));
		}

		void firstPassWithoutPayloadDefersToSecondPass()
		{
			QCOMPARE(resolveReloadMccpProbeTimeoutAction(QByteArray(), 0),
			         ReloadMccpProbeTimeoutAction::WaitSecondPass);
		}

		void secondPassWithoutPayloadReconnects()
		{
			QCOMPARE(resolveReloadMccpProbeTimeoutAction(QByteArray(), 1),
			         ReloadMccpProbeTimeoutAction::Reconnect);
		}

		void twoPassFlowKeepsWhenReplyArrivesOnSecondPass()
		{
			QCOMPARE(resolveReloadMccpProbeTimeoutAction(QByteArray(), 0),
			         ReloadMccpProbeTimeoutAction::WaitSecondPass);
			const QByteArray lateReply =
			    QByteArrayLiteral("You are in a room with exits north and east.\r\n");
			QCOMPARE(resolveReloadMccpProbeTimeoutAction(lateReply, 1),
			         ReloadMccpProbeTimeoutAction::KeepReattach);
		}

		void noDecisionBytesBeforeLookProbeSend()
		{
			const QByteArray full = QByteArrayLiteral("prelook bytes\r\n");
			QVERIFY(extractReloadMccpProbeDecisionPayload(full, false, 0).isEmpty());
		}

		void noDecisionBytesWhenOffsetAtTail()
		{
			const QByteArray full = QByteArrayLiteral("abc");
			QVERIFY(extractReloadMccpProbeDecisionPayload(full, true, full.size()).isEmpty());
		}

		void decisionBytesStartExactlyAtLookOffset()
		{
			const QByteArray full = QByteArrayLiteral("prelook bytesPOSTLOOK PAYLOAD\r\n");
			const QByteArray out  = extractReloadMccpProbeDecisionPayload(full, true, 13);
			QCOMPARE(out, QByteArrayLiteral("POSTLOOK PAYLOAD\r\n"));
		}

		void payloadBuilderPreservesReplayAndDecisionSlices()
		{
			const QByteArray full = QByteArrayLiteral("AAAApost-look reply\r\n");
			const auto       out  = makeReloadMccpProbePayloads(full, true, 4);
			QCOMPARE(out.replayPayload, full);
			QCOMPARE(out.decisionPayload, QByteArrayLiteral("post-look reply\r\n"));
		}

		void probeIngressSkipsWhenNotPending()
		{
			qint64                            bytesIn     = 10;
			int                               packetCount = 2;
			QByteArray                        probeBuffer = QByteArrayLiteral("existing");
			const ReloadMccpProbeIngressInput input{
			    false,
			    false,
			    5,
			    QByteArrayLiteral("processed"),
			};

			QVERIFY(!ingestReloadMccpProbeChunk(input, &bytesIn, &packetCount, &probeBuffer));
			QCOMPARE(bytesIn, static_cast<qint64>(10));
			QCOMPARE(packetCount, 2);
			QCOMPARE(probeBuffer, QByteArrayLiteral("existing"));
		}

		void probeIngressSkipsForSimulatedInput()
		{
			qint64                            bytesIn     = 10;
			int                               packetCount = 2;
			QByteArray                        probeBuffer = QByteArrayLiteral("existing");
			const ReloadMccpProbeIngressInput input{
			    true,
			    true,
			    5,
			    QByteArrayLiteral("processed"),
			};

			QVERIFY(!ingestReloadMccpProbeChunk(input, &bytesIn, &packetCount, &probeBuffer));
			QCOMPARE(bytesIn, static_cast<qint64>(10));
			QCOMPARE(packetCount, 2);
			QCOMPARE(probeBuffer, QByteArrayLiteral("existing"));
		}

		void probeIngressAccumulatesCountersAndProcessedPayload()
		{
			qint64                            bytesIn     = 10;
			int                               packetCount = 2;
			QByteArray                        probeBuffer = QByteArrayLiteral("existing");
			const ReloadMccpProbeIngressInput input{
			    false,
			    true,
			    5,
			    QByteArrayLiteral("processed"),
			};

			QVERIFY(ingestReloadMccpProbeChunk(input, &bytesIn, &packetCount, &probeBuffer));
			QCOMPARE(bytesIn, static_cast<qint64>(15));
			QCOMPARE(packetCount, 3);
			QCOMPARE(probeBuffer, QByteArrayLiteral("existingprocessed"));
		}

		void probeIngressCountsRawChunkEvenWhenProcessedIsEmpty()
		{
			qint64                            bytesIn     = 0;
			int                               packetCount = 0;
			QByteArray                        probeBuffer;
			const ReloadMccpProbeIngressInput input{
			    false,
			    true,
			    12,
			    QByteArray(),
			};

			QVERIFY(ingestReloadMccpProbeChunk(input, &bytesIn, &packetCount, &probeBuffer));
			QCOMPARE(bytesIn, static_cast<qint64>(12));
			QCOMPARE(packetCount, 1);
			QVERIFY(probeBuffer.isEmpty());
		}

		void takeDeferredPayloadReturnsAndClearsBuffer()
		{
			QByteArray       probeBuffer = QByteArrayLiteral("deferred-processed-data");

			const QByteArray payload = takeDeferredReloadMccpProbePayload(&probeBuffer);
			QCOMPARE(payload, QByteArrayLiteral("deferred-processed-data"));
			QVERIFY(probeBuffer.isEmpty());
		}

		void takeDeferredPayloadIsOneShot()
		{
			QByteArray       probeBuffer = QByteArrayLiteral("deferred-processed-data");

			const QByteArray firstTake  = takeDeferredReloadMccpProbePayload(&probeBuffer);
			const QByteArray secondTake = takeDeferredReloadMccpProbePayload(&probeBuffer);

			QCOMPARE(firstTake, QByteArrayLiteral("deferred-processed-data"));
			QVERIFY(secondTake.isEmpty());
			QVERIFY(probeBuffer.isEmpty());
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_ReloadUnit)

#if __has_include("tst_ReloadUnit.moc")
#include "tst_ReloadUnit.moc"
#endif
