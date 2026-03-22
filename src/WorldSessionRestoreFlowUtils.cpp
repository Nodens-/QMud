/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldSessionRestoreFlowUtils.cpp
 * Role: Sequencing helpers for post-session-restore startup and auto-connect flow.
 */

#include "WorldSessionRestoreFlowUtils.h"

namespace QMudWorldSessionRestoreFlow
{
	SessionStateLoadPlan computeSessionStateLoadPlan(const bool persistOutputBuffer,
	                                                 const bool persistCommandHistory,
	                                                 const bool stateFileExists)
	{
		if (!persistOutputBuffer && !persistCommandHistory)
			return SessionStateLoadPlan::RemoveFileAndSucceed;
		if (stateFileExists)
			return SessionStateLoadPlan::ReadFileAndApply;
		return SessionStateLoadPlan::SkipApplyAndSucceed;
	}

	void runPostRestoreFlow(const bool restoreOk, const QString &restoreError,
	                        const PostRestoreActions &actions)
	{
		if (!restoreOk && !restoreError.trimmed().isEmpty() && actions.reportRestoreError)
			actions.reportRestoreError(restoreError);

		if (actions.runStartup)
			actions.runStartup();

		if (actions.runAutoConnect)
			actions.runAutoConnect();
	}
} // namespace QMudWorldSessionRestoreFlow
