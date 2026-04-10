/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldSessionRestoreFlowUtils.cpp
 * Role: Sequencing helpers for post-session-restore startup and auto-connect flow.
 */

#include "WorldSessionRestoreFlowUtils.h"

#include <QtGlobal>

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

	bool shouldTrackScrollbackRestoreStatus(const bool                 persistOutputBuffer,
	                                        const SessionStateLoadPlan loadPlan)
	{
		return persistOutputBuffer && loadPlan != SessionStateLoadPlan::RemoveFileAndSucceed;
	}

	bool shouldShowDeferredUpgradeWelcome(const bool deferUpgradeWelcome,
	                                      const bool startupRestoreDispatchComplete,
	                                      const int  restoreScrollbackInFlight)
	{
		return deferUpgradeWelcome && startupRestoreDispatchComplete && restoreScrollbackInFlight <= 0;
	}

	QString restoreScrollbackStatusMessage(const int restoreScrollbackInFlight)
	{
		return QStringLiteral("Restoring scrollback buffers (%1 remaining)")
		    .arg(qMax(0, restoreScrollbackInFlight));
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
