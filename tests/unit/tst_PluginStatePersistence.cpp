/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_PluginStatePersistence.cpp
 * Role: Deterministic coverage of coalescing, minute deadlines, worker ownership and final drains.
 */
#include "PluginStatePersistenceTestAccess.h"
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QSemaphore>
#include <QtTest/QTest>

namespace
{
	/** @brief Monotonic test clock with a barrier for worker scheduling decisions. */
	class ControlledClock
	{
		public:
			/**
			 * @brief Samples current test time and announces that the worker observed it.
			 * @return Controlled monotonic time.
			 */
			PluginStatePersistence::TimePoint now()
			{
				std::lock_guard lock(m_mutex);
				++m_samples;
				m_observed.notify_all();
				return PluginStatePersistence::TimePoint(std::chrono::seconds(m_seconds));
			}
			/**
			 * @brief Advances time without waiting for a deliberately blocked writer.
			 * @param seconds New monotonic seconds.
			 */
			void set(qint64 seconds)
			{
				std::lock_guard lock(m_mutex);
				m_seconds = seconds;
			}
			/**
			 * @brief Advances time and waits for the worker to observe the new deadline.
			 * @param service Worker to wake.
			 * @param seconds New monotonic seconds.
			 * @return Whether the worker sampled time before the liveness timeout.
			 */
			bool advance(PluginStatePersistence &service, qint64 seconds)
			{
				std::unique_lock lock(m_mutex);
				m_seconds          = seconds;
				const quint64 goal = m_samples + 1;
				lock.unlock();
				PluginStatePersistenceTestAccess::wake(service);
				lock.lock();
				return m_observed.wait_for(lock, std::chrono::seconds(5), [&] { return m_samples >= goal; });
			}

		private:
			std::mutex              m_mutex;
			std::condition_variable m_observed;
			qint64                  m_seconds{0};
			quint64                 m_samples{0};
	};

	/**
	 * @brief Makes a complete owning test snapshot.
	 * @param path Absolute destination.
	 * @param value Distinct payload to observe.
	 * @param revision Save order assigned by the service.
	 * @return Owning snapshot.
	 */
	PluginStatePersistence::Snapshot state(const QString &path, const QString &value, quint64 revision)
	{
		PluginStatePersistence::Snapshot snapshot;
		snapshot.targetFilePath = path;
		snapshot.variables.insert(QStringLiteral("Value"), value);
		snapshot.revision = revision;
		snapshot.savedAt  = QDateTime::currentDateTime();
		return snapshot;
	}

	/** @brief Worker-observed values, protected independently from the writer mailbox. */
	class RecordedWrites
	{
		public:
			/**
			 * @brief Records a physical write attempt.
			 * @param snapshot Claimed state.
			 */
			void append(const PluginStatePersistence::Snapshot &snapshot)
			{
				std::lock_guard lock(m_mutex);
				m_values.push_back(snapshot.variables.value(QStringLiteral("value")));
				m_thread = std::this_thread::get_id();
			}
			/**
			 * @brief Reads attempted payloads after synchronizing with worker decisions.
			 * @return Attempted payloads in order.
			 */
			QStringList values()
			{
				std::lock_guard lock(m_mutex);
				return m_values;
			}
			/**
			 * @brief Reads the execution thread of the last physical attempt.
			 * @return Backend thread identifier.
			 */
			std::thread::id thread()
			{
				std::lock_guard lock(m_mutex);
				return m_thread;
			}

		private:
			std::mutex      m_mutex;
			QStringList     m_values;
			std::thread::id m_thread;
	};

	/** @brief Exercises reachable mailbox, throttle, ordering and lifecycle sequences. */
	class tst_PluginStatePersistence : public QObject
	{
			Q_OBJECT
		private slots:
			/** @brief Replaces distinct pending payloads without moving the minute boundary. */
			static void coalescesDistinctStatesAtFixedDeadline()
			{
				ControlledClock        clock;
				RecordedWrites         writes;
				QSemaphore             started, release;
				PluginStatePersistence service([&] { return clock.now(); },
				                               [&](const auto &snapshot, QString &)
				                               {
					                               writes.append(snapshot);
					                               if (snapshot.variables.value(QStringLiteral("value")) ==
					                                   QStringLiteral("A"))
					                               {
						                               started.release();
						                               release.acquire();
					                               }
					                               return true;
				                               });
				const auto             unblock = qScopeGuard([&] { release.release(); });
				const QString          path = QDir::current().filePath(QStringLiteral("coalesced-state.xml"));
				const auto             a    = service.reserveRevision();
				QVERIFY(service.submit(state(path, QStringLiteral("A"), a)));
				QVERIFY(started.tryAcquire(1, 5000));
				clock.set(10);
				QVERIFY(service.submit(state(path, QStringLiteral("B"), service.reserveRevision())));
				clock.set(59);
				const auto c = service.reserveRevision();
				QVERIFY(service.submit(state(path, QStringLiteral("C"), c)));
				QCOMPARE(PluginStatePersistenceTestAccess::pendingValue(service, path), QStringLiteral("C"));
				release.release();
				QVERIFY(PluginStatePersistenceTestAccess::waitCompleted(service, path, a));
				QVERIFY(clock.advance(service, 59));
				QCOMPARE(PluginStatePersistenceTestAccess::pendingValue(service, path), QStringLiteral("C"));
				QCOMPARE(writes.values(), QStringList({QStringLiteral("A")}));
				QVERIFY(clock.advance(service, 60));
				QVERIFY(PluginStatePersistenceTestAccess::waitCompleted(service, path, c));
				QCOMPARE(writes.values(), QStringList({QStringLiteral("A"), QStringLiteral("C")}));
				QVERIFY(writes.thread() != std::this_thread::get_id());
			}

			/** @brief Replacing pending state releases its maps even while another write is blocked. */
			static void discardedSnapshotsAreReleasedBeforeActiveWriteFinishes()
			{
				ControlledClock        clock;
				QSemaphore             started, release;
				PluginStatePersistence service([&] { return clock.now(); },
				                               [&](const auto &, QString &)
				                               {
					                               started.release();
					                               release.acquire();
					                               return true;
				                               });
				const auto             unblock = qScopeGuard([&] { release.release(2); });
				const QString          path = QDir::current().filePath(QStringLiteral("released-state.xml"));
				QVERIFY(service.submit(state(path, QStringLiteral("active"), service.reserveRevision())));
				QVERIFY(started.tryAcquire(1, 5000));
				auto pending     = state(path, QStringLiteral("pending"), service.reserveRevision());
				auto previousMap = pending.variables.values();
				QVERIFY(service.submit(std::move(pending)));
				QVERIFY(!previousMap.isDetached());
				for (int i = 0; i < 100; ++i)
				{
					auto next       = state(path, QString::number(i), service.reserveRevision());
					auto currentMap = next.variables.values();
					QVERIFY(service.submit(std::move(next)));
					QVERIFY(previousMap.isDetached());
					QVERIFY(!currentMap.isDetached());
					previousMap = std::move(currentMap);
				}
				auto obsolete    = state(path, QStringLiteral("obsolete"), 1);
				auto obsoleteMap = obsolete.variables.values();
				QVERIFY(service.submit(std::move(obsolete)));
				QVERIFY(obsoleteMap.isDetached());
				QCOMPARE(PluginStatePersistenceTestAccess::pendingValue(service, path), QStringLiteral("99"));
				release.release(2);
				service.flush(path);
				QVERIFY(previousMap.isDetached());
			}

			/** @brief Each destination has its own interval and idle time never creates periodic writes. */
			static void independentDestinationsAndIdleExpiry()
			{
				ControlledClock        clock;
				RecordedWrites         writes;
				PluginStatePersistence service([&] { return clock.now(); },
				                               [&](const auto &snapshot, QString &)
				                               {
					                               writes.append(snapshot);
					                               return true;
				                               });
				const QString          x = QDir::current().filePath(QStringLiteral("state-x.xml"));
				const QString          y = QDir::current().filePath(QStringLiteral("state-y.xml"));
				const auto             a = service.reserveRevision();
				QVERIFY(service.submit(state(x, QStringLiteral("X0"), a)));
				QVERIFY(PluginStatePersistenceTestAccess::waitCompleted(service, x, a));
				clock.set(20);
				const auto b = service.reserveRevision();
				QVERIFY(service.submit(state(y, QStringLiteral("Y20"), b)));
				QVERIFY(PluginStatePersistenceTestAccess::waitCompleted(service, y, b));
				clock.set(59);
				const auto c = service.reserveRevision(), d = service.reserveRevision();
				QVERIFY(service.submit(state(x, QStringLiteral("X60"), c)));
				QVERIFY(service.submit(state(y, QStringLiteral("Y80"), d)));
				QVERIFY(clock.advance(service, 60));
				QVERIFY(PluginStatePersistenceTestAccess::waitCompleted(service, x, c));
				QCOMPARE(PluginStatePersistenceTestAccess::pendingValue(service, y), QStringLiteral("Y80"));
				QCOMPARE(writes.values().size(), 3);
				QVERIFY(clock.advance(service, 80));
				QVERIFY(PluginStatePersistenceTestAccess::waitCompleted(service, y, d));
				QVERIFY(clock.advance(service, 210));
				QVERIFY(PluginStatePersistenceTestAccess::pendingValue(service, y).isEmpty());
				QCOMPARE(writes.values().size(), 4);
				const auto e = service.reserveRevision();
				QVERIFY(service.submit(state(x, QStringLiteral("X210"), e)));
				QVERIFY(PluginStatePersistenceTestAccess::waitCompleted(service, x, e));
				QCOMPARE(writes.values().last(), QStringLiteral("X210"));
			}

			/** @brief Waiting behind another file anchors the interval to the actual worker start. */
			static void delayedStartDefinesNextDeadline()
			{
				ControlledClock        clock;
				RecordedWrites         writes;
				QSemaphore             started, release;
				PluginStatePersistence service([&] { return clock.now(); },
				                               [&](const auto &snapshot, QString &)
				                               {
					                               writes.append(snapshot);
					                               if (snapshot.variables.value(QStringLiteral("value")) ==
					                                   QStringLiteral("block"))
					                               {
						                               started.release();
						                               release.acquire();
					                               }
					                               return true;
				                               });
				const auto             unblock = qScopeGuard([&] { release.release(); });
				const QString          x       = QDir::current().filePath(QStringLiteral("delayed-x.xml"));
				const QString          y       = QDir::current().filePath(QStringLiteral("delayed-y.xml"));
				QVERIFY(service.submit(state(x, QStringLiteral("block"), service.reserveRevision())));
				QVERIFY(started.tryAcquire(1, 5000));
				const auto a = service.reserveRevision();
				QVERIFY(service.submit(state(y, QStringLiteral("first"), a)));
				clock.set(120);
				release.release();
				QVERIFY(PluginStatePersistenceTestAccess::waitCompleted(service, y, a));
				const auto b = service.reserveRevision();
				QVERIFY(service.submit(state(y, QStringLiteral("second"), b)));
				QVERIFY(clock.advance(service, 179));
				QCOMPARE(PluginStatePersistenceTestAccess::pendingValue(service, y),
				         QStringLiteral("second"));
				QCOMPARE(writes.values().size(), 2);
				QVERIFY(clock.advance(service, 180));
				QVERIFY(PluginStatePersistenceTestAccess::waitCompleted(service, y, b));
				QCOMPARE(writes.values().size(), 3);
			}

			/** @brief Final snapshots, explicit drains and destruction bypass ordinary throttling. */
			static void finalDrainAndLateOlderCapture()
			{
				ControlledClock clock;
				RecordedWrites  writes;
				{
					const QString          path = QDir::current().filePath(QStringLiteral("final-state.xml"));
					PluginStatePersistence service([&] { return clock.now(); },
					                               [&](const auto &snapshot, QString &)
					                               {
						                               writes.append(snapshot);
						                               return true;
					                               });
					const auto             older = service.reserveRevision();
					const auto             newer = service.reserveRevision();
					QVERIFY(service.submit(state(path, QStringLiteral("newer"), newer)));
					QVERIFY(PluginStatePersistenceTestAccess::waitCompleted(service, path, newer));
					QVERIFY(service.submit(state(path, QStringLiteral("obsolete"), older)));
					const auto final = service.reserveRevision();
					QVERIFY(service.submit(state(path, QStringLiteral("final"), final), true));
					QVERIFY(PluginStatePersistenceTestAccess::waitCompleted(service, path, final));
					const auto latest = service.reserveRevision();
					QVERIFY(service.submit(state(path, QStringLiteral("latest"), latest)));
					QVERIFY(service.submit(state(path, QStringLiteral("obsolete-final"), older), true));
					QVERIFY(PluginStatePersistenceTestAccess::waitCompleted(service, path, latest));
					QVERIFY(service.submit(state(path, QStringLiteral("flush"), service.reserveRevision())));
					service.flush(path);
					QCOMPARE(writes.values().last(), QStringLiteral("flush"));
					QVERIFY(
					    service.submit(state(path, QStringLiteral("destructor"), service.reserveRevision())));
				}
				QCOMPARE(writes.values(), QStringList({QStringLiteral("newer"), QStringLiteral("final"),
				                                       QStringLiteral("latest"), QStringLiteral("flush"),
				                                       QStringLiteral("destructor")}));
			}

			/** @brief Failure consumes the interval, emits stderr, and retries only after another request. */
			static void failureDoesNotRetryOrBypassInterval()
			{
				ControlledClock        clock;
				RecordedWrites         writes;
				PluginStatePersistence service([&] { return clock.now(); },
				                               [&](const auto &snapshot, QString &error)
				                               {
					                               writes.append(snapshot);
					                               error = QStringLiteral("injected failure");
					                               return false;
				                               });
				const QString          path = QDir::current().filePath(QStringLiteral("failed-state.xml"));
				const auto             a    = service.reserveRevision();
				QVERIFY(service.submit(state(path, QStringLiteral("fail"), a)));
				QVERIFY(PluginStatePersistenceTestAccess::waitCompleted(service, path, a));
				const auto b = service.reserveRevision();
				QVERIFY(service.submit(state(path, QStringLiteral("retry"), b)));
				QVERIFY(clock.advance(service, 59));
				QCOMPARE(PluginStatePersistenceTestAccess::pendingValue(service, path),
				         QStringLiteral("retry"));
				QCOMPARE(writes.values().size(), 1);
				QVERIFY(clock.advance(service, 60));
				QVERIFY(PluginStatePersistenceTestAccess::waitCompleted(service, path, b));
				QVERIFY(clock.advance(service, 600));
				QVERIFY(PluginStatePersistenceTestAccess::pendingValue(service, path).isEmpty());
				QCOMPARE(writes.values().size(), 2);
			}

			/** @brief Production XML preserves labels and captured state after its producer changes. */
			static void owningSnapshotPreservesCapturedState()
			{
				QTemporaryDir directory(QDir::current().filePath(QStringLiteral("writer-XXXXXX")));
				QVERIFY(directory.isValid());
				const QString          path = QDir(directory.path()).filePath(QStringLiteral("state.xml"));
				PluginStatePersistence service;
				auto       snapshot = state(path, QStringLiteral("A<&>é"), service.reserveRevision());
				const auto owner    = snapshot.variables;
				QVERIFY(service.submit(std::move(snapshot)));
				snapshot.variables.insert(QStringLiteral("Value"), QStringLiteral("unsaved"));
				service.flush(path);
				QFile file(path);
				QVERIFY(file.open(QIODevice::ReadOnly));
				const auto xml = QString::fromUtf8(file.readAll());
				QVERIFY(xml.contains(QStringLiteral("name=\"Value\">A&lt;&amp;&gt;é</variable>")));
				QVERIFY(!xml.contains(QStringLiteral("unsaved")));
				QCOMPARE(owner.value(QStringLiteral("value")), QStringLiteral("A<&>é"));
			}
	};
} // namespace

QTEST_APPLESS_MAIN(tst_PluginStatePersistence)
#include "tst_PluginStatePersistence.moc"
