/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_ReloadIntegration.cpp
 * Role: Consolidated integration-level QTest coverage for reload startup recovery and probe flow.
 */

#include "ReloadUtils.h"

#include <QMap>
// ReSharper disable once CppUnusedIncludeDirective
#include <QFile>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryDir>
#include <QtTest/QTest>

namespace
{
	struct FakeRecoveryRuntime final : ReloadSocketRecoveryOps, ReloadReconnectOps
	{
			bool                   adoptResult{true};
			bool                   connectResult{true};
			bool                   suppressConnectActionsMarked{false};
			int                    adoptedDescriptor{-1};
			int                    connectCalls{0};
			int                    closeCalls{0};
			QString                connectHost;
			quint16                connectPort{0};
			bool                   paused{false};
			QStringList            events;
			QMap<QString, QString> attrs;

			bool adoptConnectedSocketDescriptor(const int descriptor, QString *errorMessage) override
			{
				adoptedDescriptor = descriptor;
				if (adoptResult)
					return true;
				if (errorMessage)
					*errorMessage = QStringLiteral("adopt failed");
				return false;
			}

			void setIncomingSocketDataPaused(const bool value) override
			{
				paused = value;
				events.push_back(value ? QStringLiteral("pause_on") : QStringLiteral("pause_off"));
			}

			void markReloadReattachConnectActionsSuppressed() override
			{
				suppressConnectActionsMarked = true;
			}

			bool consumeReloadReattachConnectActionsSuppressed() override
			{
				const bool value             = suppressConnectActionsMarked;
				suppressConnectActionsMarked = false;
				return value;
			}

			void closeSocketForReloadReconnect() override
			{
				++closeCalls;
				events.push_back(QStringLiteral("close"));
			}

			[[nodiscard]] QString worldAttribute(const QString &name) const override
			{
				return attrs.value(name);
			}

			void setWorldAttribute(const QString &name, const QString &value) override
			{
				attrs.insert(name, value);
			}

			bool connectToWorld(const QString &host, const quint16 port) override
			{
				connectHost = host;
				connectPort = port;
				++connectCalls;
				events.push_back(QStringLiteral("connect"));
				return connectResult;
			}
	};

	struct FakePostReattachRuntime final : ReloadPostReattachOps
	{
			bool        probeEnabled{false};
			int         configureProbeCalls{0};
			int         resumeCalls{0};
			int         lookCalls{0};
			QList<bool> configuredProbeValues;

			void        configureReloadMccpReattachProbe(const bool enabled) override
			{
				probeEnabled = enabled;
				++configureProbeCalls;
				configuredProbeValues.push_back(enabled);
			}

			void requestMccpResumeAfterReloadReattach() override
			{
				++resumeCalls;
			}

			void sendReloadReattachLookProbe() override
			{
				++lookCalls;
			}
	};
} // namespace

/**
 * @brief QTest fixture covering reload-related integration helper behavior.
 */
class tst_ReloadIntegration : public QObject
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
			snapshot.targetExecutable = QStringLiteral("/tmp/qmud-integration");

			QString error;
			QVERIFY(writeReloadStateSnapshot(path, snapshot, &error));
			QVERIFY(QFile::exists(path));

			const ReloadStartupValidationInput input{
			    QStringLiteral("integration-token"),
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

		void reattachPolicyAdoptsSocketWithoutReconnect()
		{
			FakeRecoveryRuntime runtime;
			ReloadWorldState    state;
			state.connectedAtReload = true;
			state.socketDescriptor  = 42;
			state.policy            = ReloadSocketPolicy::Reattach;

			const ReloadRecoverySocketDecision decision = applyReloadSocketRecovery(runtime, state);
			QCOMPARE(decision.outcome, ReloadRecoverySocketOutcome::Reattached);
			QCOMPARE(runtime.adoptedDescriptor, 42);
			QCOMPARE(runtime.connectCalls, 0);
			QVERIFY(runtime.suppressConnectActionsMarked);
			QVERIFY(runtime.events.isEmpty());
		}

		void parkReconnectPolicyKeepsReattachedSocketWhenAdoptSucceeds()
		{
			FakeRecoveryRuntime runtime;
			ReloadWorldState    state;
			state.connectedAtReload = true;
			state.socketDescriptor  = 77;
			state.policy            = ReloadSocketPolicy::ParkReconnect;

			const ReloadRecoverySocketDecision decision = applyReloadSocketRecovery(runtime, state);
			QCOMPARE(decision.outcome, ReloadRecoverySocketOutcome::Reattached);
			QVERIFY(!decision.closeSocketFirst);
			QVERIFY(decision.error.isEmpty());
			QCOMPARE(runtime.adoptedDescriptor, 77);
			QVERIFY(runtime.suppressConnectActionsMarked);
			QVERIFY(!runtime.paused);
			QVERIFY(runtime.events.isEmpty());
		}

		void failedAdoptQueuesReconnectWithoutCloseFirst()
		{
			FakeRecoveryRuntime runtime;
			runtime.adoptResult = false;
			ReloadWorldState state;
			state.connectedAtReload = true;
			state.socketDescriptor  = 99;
			state.policy            = ReloadSocketPolicy::Reattach;

			const ReloadRecoverySocketDecision decision = applyReloadSocketRecovery(runtime, state);
			QCOMPARE(decision.outcome, ReloadRecoverySocketOutcome::ReconnectQueued);
			QVERIFY(!decision.closeSocketFirst);
			QVERIFY(!decision.error.isEmpty());
			QCOMPARE(runtime.connectCalls, 0);
			QVERIFY(!runtime.suppressConnectActionsMarked);
		}

		void reconnectHelperClosesSocketBeforeReconnectWhenRequested()
		{
			FakeRecoveryRuntime runtime;
			ReloadWorldState    state;
			state.displayName = QStringLiteral("Test");
			state.host        = QStringLiteral("127.0.0.1");
			state.port        = 4000;

			const QString warning = reconnectRecoveredRuntime(runtime, state, true);
			QVERIFY(warning.isEmpty());
			QCOMPARE(runtime.closeCalls, 1);
			QCOMPARE(runtime.connectCalls, 1);
			QCOMPARE(runtime.connectHost, QStringLiteral("127.0.0.1"));
			QCOMPARE(runtime.connectPort, static_cast<quint16>(4000));
			const QStringList expectedEvents = {QStringLiteral("close"), QStringLiteral("pause_off"),
			                                    QStringLiteral("connect")};
			QCOMPARE(runtime.events, expectedEvents);
		}

		void reconnectHelperReturnsWarningWhenConnectCannotStart()
		{
			FakeRecoveryRuntime runtime;
			runtime.connectResult = false;
			ReloadWorldState state;
			state.displayName = QStringLiteral("FailingWorld");
			state.host        = QStringLiteral("127.0.0.1");
			state.port        = 4242;

			const QString warning = reconnectRecoveredRuntime(runtime, state, false);
			QVERIFY(!warning.isEmpty());
			QCOMPARE(runtime.connectCalls, 1);
		}

		void reconnectPolicyWithoutDescriptorSkipsAdopt()
		{
			FakeRecoveryRuntime runtime;
			ReloadWorldState    state;
			state.connectedAtReload = true;
			state.socketDescriptor  = -1;
			state.policy            = ReloadSocketPolicy::ParkReconnect;

			const ReloadRecoverySocketDecision decision = applyReloadSocketRecovery(runtime, state);
			QCOMPARE(decision.outcome, ReloadRecoverySocketOutcome::ReconnectQueued);
			QVERIFY(!decision.closeSocketFirst);
			QVERIFY(decision.error.isEmpty());
			QCOMPARE(runtime.adoptedDescriptor, -1);
			QCOMPARE(runtime.connectCalls, 0);
			QVERIFY(!runtime.suppressConnectActionsMarked);
			QVERIFY(!runtime.paused);
			QVERIFY(runtime.events.isEmpty());
		}

		void mccpTimeoutArmsProbeAndSkipsResume()
		{
			FakePostReattachRuntime runtime;
			ReloadWorldState        worldState;
			worldState.mccpWasActive        = true;
			worldState.mccpDisableSucceeded = false;

			applyReloadPostReattachActions(runtime, worldState);

			QCOMPARE(runtime.configureProbeCalls, 1);
			QCOMPARE(runtime.configuredProbeValues, QList<bool>({true}));
			QCOMPARE(runtime.resumeCalls, 0);
			QCOMPARE(runtime.lookCalls, 1);
		}

		void mccpDisableSuccessResumesWithoutProbe()
		{
			FakePostReattachRuntime runtime;
			ReloadWorldState        worldState;
			worldState.mccpWasActive        = true;
			worldState.mccpDisableSucceeded = true;

			applyReloadPostReattachActions(runtime, worldState);

			QCOMPARE(runtime.configureProbeCalls, 1);
			QCOMPARE(runtime.configuredProbeValues, QList<bool>({false}));
			QCOMPARE(runtime.resumeCalls, 1);
			QCOMPARE(runtime.lookCalls, 1);
		}

		void noMccpOnlySendsLook()
		{
			FakePostReattachRuntime runtime;
			ReloadWorldState        worldState;
			worldState.mccpWasActive        = false;
			worldState.mccpDisableSucceeded = false;

			applyReloadPostReattachActions(runtime, worldState);

			QCOMPARE(runtime.configureProbeCalls, 1);
			QCOMPARE(runtime.configuredProbeValues, QList<bool>({false}));
			QCOMPARE(runtime.resumeCalls, 0);
			QCOMPARE(runtime.lookCalls, 1);
		}

		void classificationUsesOnlyPostLookPayloadWhileReplayKeepsEverything()
		{
			QByteArray full;
			full.append(static_cast<char>(0x80));
			full.append(static_cast<char>(0x81));
			full.append(static_cast<char>(0x82));
			full.append(static_cast<char>(0x83));
			const qsizetype lookOffset = full.size();
			full.append(QByteArrayLiteral("You are in a room with exits north and east.\r\n"));

			const ReloadMccpProbePayloads payloads = makeReloadMccpProbePayloads(full, true, lookOffset);
			QCOMPARE(payloads.replayPayload, full);
			QCOMPARE(payloads.decisionPayload,
			         QByteArrayLiteral("You are in a room with exits north and east.\r\n"));
			QVERIFY(!isLikelyResidualMccpPayload(payloads.decisionPayload));
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_ReloadIntegration)

#if __has_include("tst_ReloadIntegration.moc")
#include "tst_ReloadIntegration.moc"
#endif
