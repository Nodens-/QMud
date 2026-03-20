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
