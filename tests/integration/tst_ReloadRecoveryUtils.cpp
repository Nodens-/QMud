/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_ReloadRecoveryUtils.cpp
 * Role: Integration-level QTest coverage for reload startup socket recovery/reconnect sequencing helpers.
 */

#include "ReloadRecoveryUtils.h"

#include <QMap>
#include <QtTest/QTest>

namespace
{
	struct FakeRuntime
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

			bool                   adoptConnectedSocketDescriptor(const int descriptor, QString *errorMessage)
			{
				adoptedDescriptor = descriptor;
				if (adoptResult)
					return true;
				if (errorMessage)
					*errorMessage = QStringLiteral("adopt failed");
				return false;
			}

			void setIncomingSocketDataPaused(const bool value)
			{
				paused = value;
				events.push_back(value ? QStringLiteral("pause_on") : QStringLiteral("pause_off"));
			}

			void markReloadReattachConnectActionsSuppressed()
			{
				suppressConnectActionsMarked = true;
			}

			bool consumeReloadReattachConnectActionsSuppressed()
			{
				const bool value             = suppressConnectActionsMarked;
				suppressConnectActionsMarked = false;
				return value;
			}

			void closeSocketForReloadReconnect()
			{
				++closeCalls;
				events.push_back(QStringLiteral("close"));
			}

			[[nodiscard]] const QMap<QString, QString> &worldAttributes() const
			{
				return attrs;
			}

			void setWorldAttribute(const QString &name, const QString &value)
			{
				attrs.insert(name, value);
			}

			bool connectToWorld(const QString &host, const quint16 port)
			{
				connectHost = host;
				connectPort = port;
				++connectCalls;
				events.push_back(QStringLiteral("connect"));
				return connectResult;
			}
	};
} // namespace

/**
 * @brief QTest fixture covering reload startup socket recovery/reconnect helper behavior.
 */
class tst_ReloadRecoveryUtils : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void reattachPolicyAdoptsSocketWithoutReconnect()
		{
			FakeRuntime      runtime;
			ReloadWorldState state;
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

		void parkReconnectPolicyQueuesReconnectAndPausesInput()
		{
			FakeRuntime      runtime;
			ReloadWorldState state;
			state.connectedAtReload = true;
			state.socketDescriptor  = 77;
			state.policy            = ReloadSocketPolicy::ParkReconnect;

			const ReloadRecoverySocketDecision decision = applyReloadSocketRecovery(runtime, state);
			QCOMPARE(decision.outcome, ReloadRecoverySocketOutcome::ReconnectQueued);
			QVERIFY(decision.closeSocketFirst);
			QVERIFY(decision.error.isEmpty());
			QCOMPARE(runtime.adoptedDescriptor, 77);
			QVERIFY(runtime.suppressConnectActionsMarked);
			QVERIFY(runtime.paused);
			QCOMPARE(runtime.events, QStringList{QStringLiteral("pause_on")});
		}

		void failedAdoptQueuesReconnectWithoutCloseFirst()
		{
			FakeRuntime runtime;
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
			FakeRuntime      runtime;
			ReloadWorldState state;
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
			FakeRuntime runtime;
			runtime.connectResult = false;
			ReloadWorldState state;
			state.displayName = QStringLiteral("FailingWorld");
			state.host        = QStringLiteral("127.0.0.1");
			state.port        = 4242;

			const QString warning = reconnectRecoveredRuntime(runtime, state, false);
			QVERIFY(!warning.isEmpty());
			QCOMPARE(runtime.connectCalls, 1);
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_ReloadRecoveryUtils)


#if __has_include("tst_ReloadRecoveryUtils.moc")
#include "tst_ReloadRecoveryUtils.moc"
#endif
