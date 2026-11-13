/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_GmcpProtocol.cpp
 * Role: Synthetic GMCP framing, policy, publication and handoff coverage.
 */
#include "GmcpProtocol.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSemaphore>
#include <QTimer>
// ReSharper disable once CppUnusedIncludeDirective
#include <QThread>
#include <QtTest/QTest>
#include <algorithm>
#include <array>
#include <vector>

namespace
{
	/**
	 * @brief Activates a synthetic native session.
	 * @param session Session under test.
	 */
	void activate(QMudGmcp::Session &session)
	{
		session.write = [](const QByteArray &bytes) -> qint64 { return bytes.size(); };
		session.reset(true, 17);
		session.receive(251);
	}
	/**
	 * @brief Reads the advertised support list from a complete synthetic GMCP frame.
	 * @param packet Captured Telnet frame.
	 * @return Advertised entries, or an empty array for an unrelated frame.
	 */
	QJsonArray advertisedModules(const QByteArray &packet)
	{
		const auto prefix = QByteArrayLiteral("Core.Supports.Set ");
		const auto raw    = packet.mid(3, packet.size() - 5);
		return raw.startsWith(prefix) ? QJsonDocument::fromJson(raw.mid(prefix.size())).array()
		                              : QJsonArray{};
	}

	/**
	 * @brief Exercises protocol behavior using no game or installation data.
	 */
	class tst_GmcpProtocol final : public QObject
	{
			Q_OBJECT
		private slots:
			/**
			 * @brief Exercises Core schedules, RTT disclosure, server-initiated Ping, Goodbye, and reload state.
			 */
			static void coreMessages()
			{
				QMudGmcp::Session session;
				QList<QByteArray> writes;
				auto              attach = [&](QMudGmcp::Session &target)
				{
					target.write = [&target, &writes](const QByteArray &bytes) -> qint64
					{
						writes.append(bytes);
						if (bytes.startsWith(QByteArray::fromHex("fffac9")))
							target.observeSent(bytes.mid(3, bytes.size() - 5));
						return bytes.size();
					};
				};
				attach(session);
				session.reset(true, 1);
				auto *ping      = session.findChild<QTimer *>(QStringLiteral("gmcpCorePing"));
				auto *keepAlive = session.findChild<QTimer *>(QStringLiteral("gmcpCoreKeepAlive"));
				QVERIFY(ping && keepAlive);
				QVERIFY(!ping->isActive() && !keepAlive->isActive());
				session.receive(251);
				QCOMPARE(writes.size(), 3);
				QVERIFY(ping->isActive() && keepAlive->isActive());
				QCOMPARE(ping->interval(), 5 * 60 * 1000);
				QCOMPARE(keepAlive->interval(), 15 * 60 * 1000);
				writes.clear();
				QVERIFY(QMetaObject::invokeMethod(ping, "timeout", Qt::DirectConnection));
				QCOMPARE(writes, QList<QByteArray>{QMudGmcp::frame("Core.Ping")});
				session.receive(250, "Core.Ping");
				const auto rtt = session.publication()->current.load()->roundTripMs;
				QVERIFY(rtt >= 0);
				QCOMPARE(writes.size(), 1); // A response completes the measurement without an echo.
				writes.clear();
				session.receive(250, "Core.Ping");
				QCOMPARE(writes, QList<QByteArray>{QMudGmcp::frame("Core.Ping " + QByteArray::number(rtt))});
				QCOMPARE(session.publication()->current.load()->roundTripMs, rtt);
				writes.clear();
				QVERIFY(QMetaObject::invokeMethod(ping, "timeout", Qt::DirectConnection));
				QCOMPARE(writes, QList<QByteArray>{QMudGmcp::frame("Core.Ping " + QByteArray::number(rtt))});
				session.receive(250, "Core.Ping");
				QCOMPARE(writes.size(), 1);
				writes.clear();
				QMudGmcp::CoreOptions options;
				options.sendRtt                  = false;
				options.pingIntervalMinutes      = 1;
				options.keepAliveIntervalMinutes = 120;
				session.setCoreOptions(options);
				QCOMPARE(ping->interval(), 60 * 1000);
				QCOMPARE(keepAlive->interval(), 120 * 60 * 1000);
				QVERIFY(QMetaObject::invokeMethod(ping, "timeout", Qt::DirectConnection));
				QCOMPARE(writes, QList<QByteArray>{QMudGmcp::frame("Core.Ping")});
				const auto        pending = session.save();
				QMudGmcp::Session restored;
				attach(restored);
				QVERIFY(restored.restore(pending, 2, false));
				QVERIFY(!restored.findChild<QTimer *>(QStringLiteral("gmcpCorePing"))->isActive());
				restored.activateRestoration(2);
				writes.clear();
				restored.receive(250, "Core.Ping");
				QVERIFY(restored.publication()->current.load()->roundTripMs >= 0);
				QVERIFY(writes.isEmpty()); // The outstanding request survives socket reattachment.
				session.receive(250, "Core.Ping");
				QVERIFY(writes.isEmpty());
				session.receive(250, "Core.Ping");
				QCOMPARE(writes, QList<QByteArray>{QMudGmcp::frame("Core.Ping")});
				writes.clear();
				QVERIFY(QMetaObject::invokeMethod(keepAlive, "timeout", Qt::DirectConnection));
				QCOMPARE(writes, QList<QByteArray>{QMudGmcp::frame("Core.KeepAlive")});
				QString goodbye;
				session.goodbye = [&](const QString &text) { goodbye = text; };
				session.receive(250, "Core.Goodbye \"Synthetic farewell\"");
				QCOMPARE(goodbye, QStringLiteral("Synthetic farewell"));
				options.pingEnabled = options.keepAliveEnabled = false;
				session.setCoreOptions(options);
				QVERIFY(!ping->isActive() && !keepAlive->isActive());
				writes.clear();
				session.receive(250, "Core.Ping");
				QVERIFY(writes.isEmpty());
				session.close();
				QCOMPARE(session.publication()->current.load()->roundTripMs, qint64{-1});
				restored.close();
				QVERIFY(!restored.findChild<QTimer *>(QStringLiteral("gmcpCorePing"))->isActive());
			}

			/**
			 * @brief Keeps merge extension version 1 independent of module registration changes.
			 */
			static void nativeMergeCapability()
			{
				QMudGmcp::Session session;
				QList<QByteArray> writes;
				session.write = [&writes](const QByteArray &bytes) -> qint64
				{
					writes.append(bytes);
					return bytes.size();
				};
				session.reset(false, 1);
				session.receive(251);
				QVERIFY(writes.isEmpty());
				session.reset(true, 2);
				session.receive(251);
				QCOMPARE(writes.size(), 3);
				const auto initial = advertisedModules(writes.constLast());
				QCOMPARE(initial.size(), 4);
				QVERIFY(initial.contains(QStringLiteral("Client.GMCPMerge 1")));
				QVERIFY(initial.contains(QStringLiteral("Char 1")));
				QVERIFY(initial.contains(QStringLiteral("Comm 1")));
				QVERIFY(initial.contains(QStringLiteral("Room 1")));
				QMap<QString, int> modules{
				    {QStringLiteral("Char"),             1},
				    {QStringLiteral("Comm"),             1},
				    {QStringLiteral("Room"),             1},
				    {QStringLiteral("Client.GMCPMerge"), 2},
				    {QStringLiteral("client.gmcpmerge"), 3},
				    {QStringLiteral("CLIENT.GMCPMERGE"), 4}
                };
				writes.clear();
				session.setModules(modules);
				QVERIFY2(writes.isEmpty(), writes.value(0).toHex().constData());
				modules.insert(QStringLiteral("Test.Feature"), 2);
				session.setModules(modules);
				QCOMPARE(writes.size(), 1);
				const auto updated = advertisedModules(writes.constLast());
				QCOMPARE(updated.size(), initial.size() + 1);
				for (const auto &entry : initial)
					QVERIFY(updated.contains(entry));
				QVERIFY(updated.contains(QStringLiteral("Test.Feature 2")));

				writes.clear();
				modules.remove(QStringLiteral("Test.Feature"));
				session.setModules(modules);
				QCOMPARE(writes.size(), 1);
				QCOMPARE(advertisedModules(writes.constLast()), initial);
			}
			/**
			 * @brief Retains pages through partial-tail appends and multiple index-height boundaries.
			 */
			static void historyPageSharing()
			{
				QMudGmcp::History history;
				QMudGmcp::Record  record;
				for (quint64 index = 0; index < 8191; ++index)
				{
					record.sequence = index;
					history.push_back(record);
				}
				const auto paused = history;
				QCOMPARE(&paused.at(0), &history.at(0));
				record.sequence = 8191;
				history.push_back(record);
				QCOMPARE(paused.size(), 8191);
				QCOMPARE(history.size(), 8192);
				for (qsizetype index = 0; index < 8128; ++index)
					QCOMPARE(&paused.at(index), &history.at(index));
				QVERIFY(&paused.last() != &history.at(8190));
				QCOMPARE(paused.last().sequence, quint64{8190});
				const auto fullTail = history;
				for (quint64 index = 8192; index < 66000; ++index)
				{
					record.sequence = index;
					history.push_back(record);
				}
				for (qsizetype index = 0; index < fullTail.size(); ++index)
					QCOMPARE(&fullTail.at(index), &history.at(index));
				QCOMPARE(history.first().sequence, quint64{0});
				QCOMPARE(history.last().sequence, quint64{65999});
				history.remove(0, 65000);
				QCOMPARE(history.size(), 1000);
				for (qsizetype index = 0; index < history.size(); ++index)
					QCOMPARE(history.at(index).sequence, static_cast<quint64>(index + 65000));
				QCOMPARE(fullTail.size(), 8192);
			}

			/**
			 * @brief Checks deterministic page-boundary appends, head/middle/tail removals, captured
			 * versions, assignment, and reuse against an independent flat sequence.
			 */
			static void historyRankEdits()
			{
				QMudGmcp::History                                               history;
				std::vector<quint64>                                            expected;
				std::vector<std::pair<QMudGmcp::History, std::vector<quint64>>> versions;
				quint64                                                         sequence = 0;
				for (std::size_t step = 0; step < 1200; ++step)
				{
					if (step % 37 == 0)
						versions.emplace_back(history, expected);
					constexpr std::array<std::size_t, 8> batchSizes{1, 63, 64, 65, 127, 128, 129, 257};
					if (expected.empty() || step % 3 != 2)
					{
						const auto count = batchSizes[(step / 3) % batchSizes.size()];
						for (std::size_t index = 0; index < count; ++index)
						{
							QMudGmcp::Record record;
							record.sequence = ++sequence;
							history.push_back(record);
							expected.push_back(sequence);
						}
					}
					else
					{
						const auto cycle = step / 3;
						const auto count =
						    cycle % 29 == 28
						        ? expected.size()
						        : std::min(expected.size(), batchSizes[(cycle + 3) % batchSizes.size()]);
						std::size_t first = 0;
						switch (cycle % 4)
						{
						case 0:
							break;
						case 1:
							first = (expected.size() - count) / 2;
							break;
						case 2:
							first = expected.size() - count;
							break;
						default:
							first = std::min<std::size_t>(64, expected.size() - count);
							break;
						}
						history.remove(static_cast<qsizetype>(first), static_cast<qsizetype>(count));
						expected.erase(expected.begin() + static_cast<std::ptrdiff_t>(first),
						               expected.begin() + static_cast<std::ptrdiff_t>(first + count));
					}
					QCOMPARE(history.size(), static_cast<qsizetype>(expected.size()));
					for (std::size_t index = 0; index < expected.size(); ++index)
						QCOMPARE(history.at(static_cast<qsizetype>(index)).sequence, expected[index]);
				}
				for (const auto &[version, values] : versions)
				{
					QCOMPARE(version.size(), static_cast<qsizetype>(values.size()));
					for (std::size_t index = 0; index < values.size(); ++index)
						QCOMPARE(version.at(static_cast<qsizetype>(index)).sequence, values[index]);
				}
				history.remove(0, history.size());
				QVERIFY(history.isEmpty());
				QMudGmcp::Record record;
				record.sequence = ++sequence;
				history.push_back(record);
				QMudGmcp::History assigned;
				assigned = history;
				history.clear();
				QCOMPARE(assigned.first().sequence, sequence);
				history = std::move(assigned);
				QVERIFY(assigned.isEmpty());
				QCOMPARE(history.last().sequence, sequence);
				assigned.push_back(record);
				QCOMPARE(assigned.size(), 1);
			}

			/**
			 * @brief Retains recent and protected ranks through page-spanning out-of-order expiration.
			 */
			static void historyExpirationRanges()
			{
				QMudGmcp::History history;
				const auto now = QDateTime::fromString(QStringLiteral("2026-01-02T12:00:00Z"), Qt::ISODate);
				std::vector<quint64> retained;
				for (quint64 index = 0; index < 4097; ++index)
				{
					QMudGmcp::Record record;
					record.sequence = index;
					record.time     = index % 3 == 0 ? now.addSecs(-3601) : now;
					history.push_back(record);
					if (index % 3 != 0 || index >= 4092)
						retained.push_back(index);
				}
				const auto paused = history;
				const auto ranges = history.expiredRanges(now.addSecs(-3600));
				history.removeRanges(ranges, {}, {});
				QCOMPARE(history.size(), static_cast<qsizetype>(retained.size()));
				for (std::size_t index = 0; index < retained.size(); ++index)
					QCOMPARE(history.at(static_cast<qsizetype>(index)).sequence, retained[index]);
				QCOMPARE(paused.size(), 4097);
				QCOMPARE(paused.at(0).sequence, quint64{0});
				QCOMPARE(paused.last().sequence, quint64{4096});
			}

			/**
			 * @brief Releases the last expired version on a pool thread after paused ownership ends.
			 */
			static void historyRetirement()
			{
				QSemaphore        destroyed;
				QThread          *destructionThread = nullptr;
				QMudGmcp::History history;
				{
					QMudGmcp::Record record;
					record.messages = record.messages.withUpdates({
					    {QStringLiteral("test.state"),
					     QSharedPointer<const QMudGmcp::Message>(new QMudGmcp::Message,
                         [&](const QMudGmcp::Message *message)
					                                             {
						                                             destructionThread =
						                                                 QThread::currentThread();
						                                             delete message;
						                                             destroyed.release();
					                                             })}
                    });
					history.push_back(record);
				}
				auto paused = history;
				history.remove(0, history.size());
				QVERIFY(!destroyed.tryAcquire());
				paused.clear();
				QVERIFY(destroyed.tryAcquire(1, 10000));
				QVERIFY(destructionThread != QThread::currentThread());
			}

			/**
			 * @brief Ensures session teardown invalidates publication without calling external observers.
			 */
			static void destruction()
			{
				std::shared_ptr<QMudGmcp::Publication> publication;
				int                                    changes = 0;
				{
					QMudGmcp::Session session;
					session.stateChanged = [&](bool) { ++changes; };
					activate(session);
					publication = session.publication();
					QCOMPARE(changes, 1);
				}
				QCOMPARE(changes, 1);
				QVERIFY(publication->current.load()->closed);
				QVERIFY(!publication->current.load()->negotiated);
			}
			/**
			 * @brief Confirms updates share untouched package values instead of copying the whole store.
			 */
			static void structuralSharing()
			{
				QMudGmcp::Session session;
				activate(session);
				for (int i = 0; i < 1024; ++i)
					session.receive(250, "Test.P" + QByteArray::number(i) + " {\"value\":1}");
				const auto before    = session.publication()->current.load();
				const auto untouched = before->messages.value(QStringLiteral("test.p1023"));
				session.receive(250, "Test.P0 {\"value\":2}");
				const auto after = session.publication()->current.load();
				QCOMPARE(after->messages.value(QStringLiteral("test.p1023")), untouched);
				QVERIFY(before->packages.branches.sharesStorageWith(after->packages.branches));
				QCOMPARE(after->packages.branches.value(QStringLiteral("test"))->children.size(), 1024);
				QVERIFY(before->messages.value(QStringLiteral("test.p0")) !=
				        after->messages.value(QStringLiteral("test.p0")));
				QCOMPARE(before->messages.value(QStringLiteral("test.p0"))
				             ->state.toObject()
				             .value(QStringLiteral("value"))
				             .toInt(),
				         1);
			}

			/**
			 * @brief Checks every JSON value kind and strict syntax/encoding rejection.
			 */
			static void jsonKinds()
			{
				for (const QByteArray &body :
				     {QByteArray("{}"), QByteArray("[]"), QByteArray("null"), QByteArray("false"),
				      QByteArray("42"), QByteArray("1.25"), QByteArray("\"synthetic\"")})
				{
					QMudGmcp::Message message;
					QString           error;
					QVERIFY2(QMudGmcp::decode("Test.Value " + body, message, error), qPrintable(error));
					QVERIFY(!message.payload.isUndefined());
				}
				QMudGmcp::Message message;
				QString           error;
				QVERIFY(QMudGmcp::decode("Test.Ping", message, error));
				QVERIFY(message.payload.isUndefined());
				for (const QByteArray &raw :
				     {QByteArray(), QByteArray("Test.Value "), QByteArray("Test.Value {} {}"),
				      QByteArray("Test.Value 1,2"), QByteArray("Test.Value \"\xc3\""),
				      QByteArray("Test.Value \"\xff\"")})
					QVERIFY(!QMudGmcp::decode(raw, message, error));
			}

			/**
			 * @brief Verifies replacement, shallow and recursive policy precedence without stale-view
			 * mutation.
			 */
			static void mergePolicies()
			{
				QMudGmcp::Session session;
				activate(session);
				const auto receive = [&](const QByteArray &json)
				{ session.receive(250, QByteArrayLiteral("Test.State ") + json); };
				receive(R"({"a":1,"b":2})");
				const auto old = session.publication()->current.load();
				receive("{\"a\":3}");
				QVERIFY(!session.publication()
				             ->current.load()
				             ->messages.value(QStringLiteral("test.state"))
				             ->state.toObject()
				             .contains(QStringLiteral("b")));
				QVERIFY(session.setMergePolicy(QStringLiteral("TEST.State"), QMudGmcp::MergeMode::Recursive));
				receive(R"({"nested":{"a":1,"b":2},"items":[1,2]})");
				receive(R"({"nested":{"a":3},"items":[4],"nil":null})");
				auto value = session.publication()
				                 ->current.load()
				                 ->messages.value(QStringLiteral("test.state"))
				                 ->state.toObject();
				QCOMPARE(value.value(QStringLiteral("nested")).toObject().value(QStringLiteral("b")).toInt(),
				         2);
				QCOMPARE(value.value(QStringLiteral("items")).toArray().size(), 1);
				QVERIFY(value.value(QStringLiteral("nil")).isNull());
				receive(R"({"__gmcp_merge":"replace","a":9})");
				value = session.publication()
				            ->current.load()
				            ->messages.value(QStringLiteral("test.state"))
				            ->state.toObject();
				QCOMPARE(value.size(), 1);
				QVERIFY(
				    session.setMergePolicy(QStringLiteral("Test.State"), QMudGmcp::MergeMode::Shallow, true));
				receive("{\"b\":10}");
				value = session.publication()
				            ->current.load()
				            ->messages.value(QStringLiteral("test.state"))
				            ->state.toObject();
				QVERIFY(value.contains(QStringLiteral("a")));
				QVERIFY(session.setMergePolicy(QStringLiteral("Test.State"), QMudGmcp::MergeMode::Recursive,
				                               false));
				receive("{\"c\":11}");
				QCOMPARE(session.publication()
				             ->current.load()
				             ->messages.value(QStringLiteral("test.state"))
				             ->state.toObject()
				             .size(),
				         1);
				QCOMPARE(old->messages.value(QStringLiteral("test.state"))
				             ->state.toObject()
				             .value(QStringLiteral("b"))
				             .toInt(),
				         2);
				session.receive(250, "Char.Status {\"rank\":1}");
				session.receive(250, "Char.Status {\"role\":2}");
				QCOMPARE(session.publication()
				             ->current.load()
				             ->messages.value(QStringLiteral("char.status"))
				             ->state.toObject()
				             .size(),
				         2);
			}
			/**
			 * @brief Rejects malformed policy changes atomically and keeps original raw data.
			 */
			static void metadataAndErrors()
			{
				QMudGmcp::Session session;
				activate(session);
				session.receive(250, R"(Test.State {"__gmcp_merge":"shallow","a":1})");
				session.receive(250, R"(Test.State {"__gmcp_merge":"replace",broken})");
				session.receive(250, "Test.State {\"b\":2}");
				const auto state = session.publication()->current.load();
				QCOMPARE(state->serverModes.value(QStringLiteral("test.state")),
				         QMudGmcp::MergeMode::Shallow);
				QCOMPARE(state->messages.value(QStringLiteral("test.state"))->state.toObject().size(), 2);
				session.receive(250, R"(Test.State {"__gmcp_merge":17,"nested":{"__gmcp_merge":"data"}})");
				const auto message =
				    session.publication()->current.load()->messages.value(QStringLiteral("test.state"));
				QVERIFY(!message->payload.toObject().contains(QStringLiteral("__gmcp_merge")));
				QVERIFY(message->raw.contains("__gmcp_merge"));
				QVERIFY(message->state.toObject()
				            .value(QStringLiteral("nested"))
				            .toObject()
				            .contains(QStringLiteral("__gmcp_merge")));
			}
			/**
			 * @brief Invalidates old sends across copyover without changing the socket generation.
			 */
			static void copyover()
			{
				QMudGmcp::Session session;
				QList<bool>       transitions;
				session.stateChanged = [&](bool enabled) { transitions.push_back(enabled); };
				activate(session);
				const auto before = session.publication()->current.load();
				QVERIFY(session.send("Test.Ping", before->session));
				session.receive(252);
				session.receive(251);
				const auto after = session.publication()->current.load();
				QVERIFY(after->negotiated);
				QVERIFY(before->session != after->session);
				QVERIFY(!session.send("Test.Ping", before->session));
				QVERIFY(session.send("Test.Ping", after->session));
				QCOMPARE(transitions, (QList<bool>{true, false, true}));
			}

			/**
			 * @brief Saves only complete GMCP session data and restores remote policy without a handshake.
			 */
			static void handoff()
			{
				QMudGmcp::Session session;
				activate(session);
				session.receive(250, R"(Test.State {"__gmcp_merge":"recursive","a":1})");
				session.receive(250, "Test.State {\"b\":2}");
				const auto saved = session.save();
				QVERIFY(!saved.isEmpty());
				QMudGmcp::Session restored;
				int               writes = 0;
				restored.write           = [&](const QByteArray &bytes) -> qint64
				{
					++writes;
					return bytes.size();
				};
				QVERIFY(restored.restore(saved, 44));
				QVERIFY(restored.publication()->current.load()->negotiated);
				QCOMPARE(writes, 0);
				QCOMPARE(restored.publication()
				             ->current.load()
				             ->messages.value(QStringLiteral("test.state"))
				             ->state.toObject()
				             .size(),
				         2);
				const auto before = restored.publication()->current.load();
				QCOMPARE(before->packages.roots, QStringList{QStringLiteral("test")});
				QCOMPARE(before->packages.branches.value(QStringLiteral("test"))->children,
				         QStringList{QStringLiteral("test.state")});
				QCOMPARE(before->packages.branches.value(QStringLiteral("test.state"))->label,
				         QStringLiteral("State"));
				QVERIFY(!restored.restore(saved.chopped(1), 45));
				QCOMPARE(restored.publication()->current.load(), before);
			}
	};
} // namespace
QTEST_GUILESS_MAIN(tst_GmcpProtocol)
#include "tst_GmcpProtocol.moc"
