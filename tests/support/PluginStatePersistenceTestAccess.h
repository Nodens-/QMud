/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: PluginStatePersistenceTestAccess.h
 * Role: Deterministic writer gates and persistence-aware temporary directories for tests.
 */
#ifndef QMUD_PLUGINSTATEPERSISTENCETESTACCESS_H
#define QMUD_PLUGINSTATEPERSISTENCETESTACCESS_H
#include "PluginStatePersistence.h"
// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QTemporaryDir>
#include <utility>

/** @brief Test-only access to worker progress without forcing ordinary throttle deadlines. */
class PluginStatePersistenceTestAccess
{
	public:
		/**
		 * @brief Wakes the worker after advancing a controlled clock.
		 * @param service Writer being tested.
		 */
		static void wake(PluginStatePersistence &service)
		{
			std::lock_guard lock(service.m_mutex);
			service.m_wake.notify_all();
		}
		/**
		 * @brief Waits for an attempted revision without bypassing its ordinary deadline.
		 * @param service Writer being tested.
		 * @param path Destination being observed.
		 * @param revision Required attempted revision.
		 * @return Whether the revision completed before the liveness timeout.
		 */
		static bool waitCompleted(PluginStatePersistence &service, const QString &path, quint64 revision)
		{
			std::unique_lock lock(service.m_mutex);
			return service.m_wake.wait_for(lock, std::chrono::seconds(5),
			                               [&]
			                               {
				                               const auto found = service.m_destinations.constFind(path);
				                               return found != service.m_destinations.cend() &&
				                                      found->completedRevision >= revision;
			                               });
		}
		/**
		 * @brief Reads the latest pending test value while synchronizing with worker decisions.
		 * @param service Writer being tested.
		 * @param path Destination being observed.
		 * @return Pending value, or empty when there is no pending state.
		 */
		static QString pendingValue(PluginStatePersistence &service, const QString &path)
		{
			std::lock_guard lock(service.m_mutex);
			const auto      found = service.m_destinations.constFind(path);
			return found != service.m_destinations.cend() && found->pending
			           ? found->pending->variables.value(QStringLiteral("value"))
			           : QString();
		}
		/**
		 * @brief Waits until a loader requests a published-state durability boundary.
		 * @param service Writer being tested.
		 * @param path Destination being observed.
		 * @return Whether a forced drain was requested before the liveness timeout.
		 */
		static bool waitForced(PluginStatePersistence &service, const QString &path)
		{
			std::unique_lock lock(service.m_mutex);
			return service.m_wake.wait_for(lock, std::chrono::seconds(5),
			                               [&]
			                               {
				                               const auto found = service.m_destinations.constFind(path);
				                               return found != service.m_destinations.cend() &&
				                                      found->forcedRevision > 0;
			                               });
		}
		/**
		 * @brief Reads the latest explicitly expedited revision for a destination.
		 * @param service Writer being tested.
		 * @param path Destination being observed.
		 * @return Expedited revision, or zero when none was forced.
		 */
		static quint64 forcedRevision(PluginStatePersistence &service, const QString &path)
		{
			std::lock_guard lock(service.m_mutex);
			const auto      found = service.m_destinations.constFind(path);
			return found == service.m_destinations.cend() ? 0 : found->forcedRevision;
		}
		/**
		 * @brief Replaces the clock after draining work from the previous clock domain.
		 * @param service Writer being tested.
		 * @param clock Replacement monotonic clock.
		 * @return Previous clock.
		 */
		static PluginStatePersistence::Clock replaceClock(PluginStatePersistence       &service,
		                                                  PluginStatePersistence::Clock clock)
		{
			service.flush();
			std::lock_guard lock(service.m_mutex);
			return std::exchange(service.m_clock, std::move(clock));
		}
		/**
		 * @brief Replaces the backend after draining its previous work.
		 * @param service Writer being tested.
		 * @param writer Replacement backend.
		 * @return Previous backend.
		 */
		static PluginStatePersistence::Writer replaceWriter(PluginStatePersistence        &service,
		                                                    PluginStatePersistence::Writer writer)
		{
			service.flush();
			std::lock_guard lock(service.m_mutex);
			return std::exchange(service.m_writer, std::move(writer));
		}
};

/** @brief Keeps fixture destinations alive until all owning writer jobs have drained. */
class PluginStateTestDirectory : public QTemporaryDir
{
	public:
		using QTemporaryDir::QTemporaryDir;
		/** @brief Creates default test artifacts inside the active project build directory. */
		PluginStateTestDirectory()
		    : QTemporaryDir(QDir::current().filePath(QStringLiteral("plugin-state-test-XXXXXX")))
		{
		}
		/** @brief Drains state before QTemporaryDir removes its directory. */
		~PluginStateTestDirectory()
		{
			PluginStatePersistence::instance().flush();
		}
};
#endif
