/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldSessionRestoreFlowUtils.h
 * Role: Sequencing helpers for post-session-restore startup and auto-connect flow.
 */

#ifndef QMUD_WORLD_SESSION_RESTORE_FLOW_UTILS_H
#define QMUD_WORLD_SESSION_RESTORE_FLOW_UTILS_H

#include <QString>
#include <functional>

namespace QMudWorldSessionRestoreFlow
{
	/**
	 * @brief Load strategy for one restore attempt.
	 */
	enum class SessionStateLoadPlan
	{
		RemoveFileAndSucceed,
		ReadFileAndApply,
		SkipApplyAndSucceed
	};

	/**
	 * @brief Computes restore load strategy from persistence flags and file presence.
	 * @param persistOutputBuffer Persist output buffer when `true`.
	 * @param persistCommandHistory Persist command history when `true`.
	 * @param stateFileExists `true` when the session-state file exists.
	 * @return Chosen load strategy.
	 */
	[[nodiscard]] SessionStateLoadPlan
	computeSessionStateLoadPlan(bool persistOutputBuffer, bool persistCommandHistory, bool stateFileExists);
	/**
	 * @brief Returns whether scrollback-restore status tracking should be active.
	 * @param persistOutputBuffer Persist output buffer when `true`.
	 * @param loadPlan Precomputed session-state load plan.
	 * @return `true` when this restore should contribute to scrollback in-flight status.
	 */
	[[nodiscard]] bool    shouldTrackScrollbackRestoreStatus(bool                 persistOutputBuffer,
	                                                         SessionStateLoadPlan loadPlan);
	/**
	 * @brief Returns whether deferred upgrade welcome can be shown now.
	 * @param deferUpgradeWelcome `true` when upgrade welcome is deferred.
	 * @param startupRestoreDispatchComplete `true` when startup restore dispatch has finished.
	 * @param restoreScrollbackInFlight Number of tracked restore jobs still in flight.
	 * @return `true` when upgrade welcome can be shown.
	 */
	[[nodiscard]] bool    shouldShowDeferredUpgradeWelcome(bool deferUpgradeWelcome,
	                                                       bool startupRestoreDispatchComplete,
	                                                       int  restoreScrollbackInFlight);
	/**
	 * @brief Builds status-bar message for tracked scrollback restore progress.
	 * @param restoreScrollbackInFlight Number of tracked restore jobs still in flight.
	 * @return Status message text.
	 */
	[[nodiscard]] QString restoreScrollbackStatusMessage(int restoreScrollbackInFlight);

	/**
	 * @brief Callback bundle used when session-state restore completes.
	 */
	struct PostRestoreActions
	{
			std::function<void()>                runStartup;
			std::function<void()>                runAutoConnect;
			std::function<void(const QString &)> reportRestoreError;
	};

	/**
	 * @brief Runs post-restore flow in a deterministic order.
	 *
	 * Order is:
	 * 1) report restore error (only when failed and message is non-empty)
	 * 2) startup phase (banner/plugins)
	 * 3) auto-connect phase
	 *
	 * @param restoreOk `true` when restore succeeded.
	 * @param restoreError Restore error text (maybe empty).
	 * @param actions Callback bundle for follow-up actions.
	 */
	void runPostRestoreFlow(bool restoreOk, const QString &restoreError, const PostRestoreActions &actions);
} // namespace QMudWorldSessionRestoreFlow

#endif // QMUD_WORLD_SESSION_RESTORE_FLOW_UTILS_H
