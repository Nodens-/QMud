/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: PluginStatePersistence.h
 * Role: Owning plugin-state handoff and throttled, coalescing background persistence.
 */
#ifndef QMUD_PLUGINSTATEPERSISTENCE_H
#define QMUD_PLUGINSTATEPERSISTENCE_H

#include "helpers/VariableMapUtils.h"
#include <QDateTime>
#include <QHash>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

class PluginStatePersistenceTestAccess;

/** @brief Process-wide file worker accepting only owning plugin-state values. */
class PluginStatePersistence final
{
	public:
		using TimePoint = std::chrono::steady_clock::time_point;
		using Clock     = std::function<TimePoint()>;
		/** @brief Complete requested state, independent of runtime and Lua lifetimes. */
		struct Snapshot
		{
				QString                     targetFilePath;
				QMudVariableMapUtils::Store variables;
				QDateTime                   savedAt;
				quint64                     revision{0};
		};
		using Writer = std::function<bool(const Snapshot &, QString &)>;

		/**
		 * @brief Creates a lazily started writer with optional deterministic clock and backend.
		 * @param clock Monotonic time source; empty selects steady_clock.
		 * @param writer File backend; empty selects the production XML writer.
		 */
		explicit PluginStatePersistence(Clock clock = {}, Writer writer = {});
		/** @brief Drains requested state and joins the managed file worker. */
		~PluginStatePersistence();
		Q_DISABLE_COPY_MOVE(PluginStatePersistence)
		/**
		 * @brief Obtains the shared process writer, also available to standalone runtimes.
		 * @return Persistence service.
		 */
		static PluginStatePersistence &instance();
		/**
		 * @brief Assigns save order before a potentially yielding save callback.
		 * @return Monotonic save revision.
		 */
		quint64                        reserveRevision();
		/**
		 * @brief Accepts an owning snapshot, replacing older pending state for its destination.
		 * @param snapshot Complete state with an absolute resolved destination.
		 * @param immediate Bypass the ordinary minute delay for a final sealed snapshot.
		 * @return Whether the destination was accepted.
		 */
		bool                           submit(Snapshot snapshot, bool immediate = false);
		/**
		 * @brief Forces and waits for currently published state for one file or all files.
		 * @param targetFilePath Absolute destination; empty drains every published destination.
		 */
		void                           flush(const QString &targetFilePath = {});

	private:
		/** @brief Per-file throttle and latest pending state; no per-request receipts. */
		struct Destination
		{
				std::optional<Snapshot>  pending;
				std::optional<TimePoint> lastStarted;
				quint64                  latestRevision{0};
				quint64                  completedRevision{0};
				quint64                  forcedRevision{0};
		};
		/** @brief Runs due file jobs and waits for new work or the fixed minute deadline. */
		void                        run();
		/**
		 * @brief Serializes and atomically commits one complete state on the file worker.
		 * @param snapshot Owning state to serialize.
		 * @param error Receives failure details.
		 * @return Whether the file was committed.
		 */
		static bool                 writeSnapshot(const Snapshot &snapshot, QString &error);

		Clock                       m_clock;
		Writer                      m_writer;
		std::mutex                  m_mutex;
		std::condition_variable     m_wake;
		QHash<QString, Destination> m_destinations;
		std::thread                 m_worker;
		quint64                     m_nextRevision{1};
		bool                        m_stopping{false};
		friend class PluginStatePersistenceTestAccess;
};
#endif
