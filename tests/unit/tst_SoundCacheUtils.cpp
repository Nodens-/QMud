/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_SoundCacheUtils.cpp
 * Role: Unit coverage for owning sound-source cache reuse, accounting, and eviction ordering.
 */

#include "SoundCacheUtils.h"
#include "SoundEngine.h"

#include <QtTest/QTest>

namespace
{
	struct TestBackend
	{
			int   value{0};
			bool *destroyed{nullptr};

			~TestBackend()
			{
				if (destroyed)
					*destroyed = true;
			}
	};

	using Cache = QMudSoundCache<QString, TestBackend>;

	/**
	 * @brief Creates one test backend with optional destruction observation.
	 * @param value Test payload value.
	 * @param destroyed Optional flag set when the backend is destroyed.
	 * @return Owned test backend.
	 */
	std::unique_ptr<TestBackend> makeBackend(const int value, bool *destroyed = nullptr)
	{
		auto backend       = std::make_unique<TestBackend>();
		backend->value     = value;
		backend->destroyed = destroyed;
		return backend;
	}

	/** @brief QTest fixture for the production owning sound cache. */
	class tst_SoundCacheUtils : public QObject
	{
			Q_OBJECT

		private slots:
			/** @brief Verifies source bytes are counted once while each backend remains directly reusable. */
			static void sourceAccountingAndDirectReuse()
			{
				Cache      cache;
				const auto first =
				    cache.insertActive(QStringLiteral("effect:file"), 40, 1, makeBackend(10), 100);
				QVERIFY(first.backend);
				QVERIFY(first.addedSource);
				const auto second =
				    cache.insertActive(QStringLiteral("effect:file"), 40, 2, makeBackend(20), 110);
				QVERIFY(second.backend);
				QVERIFY(!second.addedSource);
				QCOMPARE(cache.totalBytes(), 40);
				QCOMPARE(cache.sourceCount(), 1);
				QCOMPARE(cache.backendCount(), 2);

				cache.markIdle(QStringLiteral("effect:file"), 1, 120);
				QCOMPARE(cache.idleBackendCount(QStringLiteral("effect:file")), 1);
				QVERIFY(!cache.hasIdleSource());
				cache.markIdle(QStringLiteral("effect:file"), 2, 130);
				QVERIFY(cache.hasIdleSource());
				QCOMPARE(cache.oldestIdleLastUsedMsecs(), 130);

				TestBackend *reused = cache.takeIdle(QStringLiteral("effect:file"), 140);
				QVERIFY(reused);
				QVERIFY(reused == first.backend || reused == second.backend);
				QVERIFY(!cache.hasIdleSource());
				QCOMPARE(cache.idleBackendCount(QStringLiteral("effect:file")), 1);
			}

			/** @brief Verifies completed backends precede idle reuse and enter idle state only after grace. */
			static void retainedCompletionPrecedesIdleCacheAndHonorsGrace()
			{
				Cache      cache;
				const auto retained =
				    cache.insertActive(QStringLiteral("effect:file"), 40, 1, makeBackend(10), 100);
				const auto idle =
				    cache.insertActive(QStringLiteral("effect:file"), 40, 2, makeBackend(20), 110);
				QVERIFY(retained.backend);
				QVERIFY(idle.backend);
				cache.markRetained(QStringLiteral("effect:file"), 1, 120);
				cache.markIdle(QStringLiteral("effect:file"), 2, 130);
				QCOMPARE(cache.retainedBackendCount(QStringLiteral("effect:file")), 1);
				QCOMPARE(cache.idleBackendCount(QStringLiteral("effect:file")), 1);

				const Cache::RetainedResult reused = cache.takeRetained(QStringLiteral("effect:file"), 140);
				QCOMPARE(reused.backend, retained.backend);
				QCOMPARE(reused.completedAtMsecs, 120);
				QCOMPARE(cache.retainedBackendCount(QStringLiteral("effect:file")), 0);
				QCOMPARE(cache.idleBackendCount(QStringLiteral("effect:file")), 1);

				cache.markRetained(QStringLiteral("effect:file"), 1, reused.completedAtMsecs);
				const Cache::RetainedReleaseResult beforeExpiry = cache.releaseExpiredRetained(419, 300, 1);
				QCOMPARE(beforeExpiry.released, 0);
				QVERIFY(!beforeExpiry.expiredRemain);
				QCOMPARE(cache.retainedBackendCount(QStringLiteral("effect:file")), 1);
				const Cache::RetainedReleaseResult atExpiry = cache.releaseExpiredRetained(420, 300, 1);
				QCOMPARE(atExpiry.released, 1);
				QVERIFY(!atExpiry.expiredRemain);
				QCOMPARE(cache.retainedBackendCount(QStringLiteral("effect:file")), 0);
				QCOMPARE(cache.idleBackendCount(QStringLiteral("effect:file")), 2);
				QVERIFY(cache.hasIdleSource());
			}

			/** @brief Verifies retained maintenance is bounded and reports only real continuation work. */
			static void retainedReleaseIsBoundedAndOldestFirst()
			{
				Cache               cache;
				constexpr qsizetype limit =
				    SoundEngine::CachePolicy::kMaximumRetainedTransitionsPerMaintenance;
				for (qsizetype index = 0; index <= limit; ++index)
				{
					const QString key = QString::number(index);
					static_cast<void>(cache.insertActive(key, 1, static_cast<quint64>(index + 1),
					                                     makeBackend(static_cast<int>(index)), index + 1));
					cache.markRetained(key, static_cast<quint64>(index + 1), index + 1);
				}

				const Cache::RetainedReleaseResult first = cache.releaseExpiredRetained(limit + 2, 1, limit);
				QCOMPARE(first.released, limit);
				QVERIFY(first.expiredRemain);
				QCOMPARE(cache.retainedBackendCount(QString::number(limit)), 1);

				const Cache::RetainedReleaseResult second = cache.releaseExpiredRetained(limit + 2, 1, limit);
				QCOMPARE(second.released, 1);
				QVERIFY(!second.expiredRemain);
				QCOMPARE(cache.retainedBackendCount(QString::number(limit)), 0);
			}

			/** @brief Verifies only fully idle sources enter least-recently-used eviction order. */
			static void evictionProtectsPartlyActiveSourcesAndUsesLruOrder()
			{
				Cache cache;
				static_cast<void>(cache.insertActive(QStringLiteral("old"), 40, 1, makeBackend(1), 50));
				static_cast<void>(cache.insertActive(QStringLiteral("old"), 40, 2, makeBackend(2), 60));
				static_cast<void>(cache.insertActive(QStringLiteral("new"), 60, 3, makeBackend(3), 70));
				cache.markIdle(QStringLiteral("old"), 1, 100);
				cache.markIdle(QStringLiteral("new"), 3, 300);
				QVERIFY(cache.hasIdleSource());
				QCOMPARE(cache.oldestIdleLastUsedMsecs(), 300);

				cache.markIdle(QStringLiteral("old"), 2, 200);
				QCOMPARE(cache.oldestIdleLastUsedMsecs(), 200);
				const auto firstEvicted = cache.evictOldestIdle([](TestBackend &) {});
				QCOMPARE(firstEvicted, std::optional<QString>(QStringLiteral("old")));
				QCOMPARE(cache.totalBytes(), 60);
				QCOMPARE(cache.backendCount(), 1);
				const auto secondEvicted = cache.evictOldestIdle([](TestBackend &) {});
				QCOMPARE(secondEvicted, std::optional<QString>(QStringLiteral("new")));
				QCOMPARE(cache.totalBytes(), 0);
			}

			/** @brief Verifies exact retirement cannot remove a replacement or disturb unrelated sources. */
			static void exactRemovalUsesSourceAndBackendIdentity()
			{
				Cache cache;
				bool  firstDestroyed  = false;
				bool  secondDestroyed = false;
				static_cast<void>(cache.insertActive(QStringLiteral("shared"), 10, 1,
				                                     makeBackend(1, &firstDestroyed), 100));
				static_cast<void>(cache.insertActive(QStringLiteral("shared"), 10, 2,
				                                     makeBackend(2, &secondDestroyed), 110));
				cache.markIdle(QStringLiteral("shared"), 1, 120);
				cache.markUnavailable(QStringLiteral("shared"), 1);
				QVERIFY(cache.eraseBackend(QStringLiteral("shared"), 1,
				                           [](TestBackend &backend) { backend.value = -1; }));
				QVERIFY(firstDestroyed);
				QVERIFY(!secondDestroyed);
				QCOMPARE(cache.backendCount(QStringLiteral("shared")), 1);
				QVERIFY(!cache.eraseBackend(QStringLiteral("shared"), 1, [](TestBackend &) {}));
				QCOMPARE(cache.backendCount(QStringLiteral("shared")), 1);
			}

			/** @brief Verifies clearing destroys all owned backends and resets accounting and ordering. */
			static void clearResetsOwnershipAndAccounting()
			{
				Cache cache;
				bool  firstDestroyed  = false;
				bool  secondDestroyed = false;
				static_cast<void>(
				    cache.insertActive(QStringLiteral("one"), 20, 1, makeBackend(1, &firstDestroyed), 100));
				static_cast<void>(
				    cache.insertActive(QStringLiteral("two"), 30, 2, makeBackend(2, &secondDestroyed), 200));
				cache.markIdle(QStringLiteral("one"), 1, 100);
				cache.markRetained(QStringLiteral("two"), 2, 200);
				cache.clear([](TestBackend &backend) { backend.value = -1; });
				QVERIFY(firstDestroyed);
				QVERIFY(secondDestroyed);
				QCOMPARE(cache.totalBytes(), 0);
				QCOMPARE(cache.sourceCount(), 0);
				QCOMPARE(cache.backendCount(), 0);
				QVERIFY(!cache.hasIdleSource());
			}

			/** @brief Verifies age checks honor the exact grace boundary and clock reversal. */
			static void elapsedBoundaryProtectsCompletionGrace()
			{
				constexpr qint64 completedAt = 1000;
				constexpr qint64 grace =
				    static_cast<qint64>(SoundEngine::CachePolicy::kCompletedChannelGraceSeconds) * 1000;
				QCOMPARE(grace, 300000);
				QVERIFY(!Cache::elapsedAtLeast(completedAt + grace - 1, completedAt, grace));
				QVERIFY(Cache::elapsedAtLeast(completedAt + grace, completedAt, grace));
				QVERIFY(!Cache::elapsedAtLeast(completedAt - 1, completedAt, grace));
				QVERIFY(!Cache::elapsedAtLeast(completedAt + grace, 0, grace));
			}
	};
} // namespace

QTEST_APPLESS_MAIN(tst_SoundCacheUtils)

#include "tst_SoundCacheUtils.moc"
