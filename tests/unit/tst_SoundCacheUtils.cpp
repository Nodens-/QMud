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
