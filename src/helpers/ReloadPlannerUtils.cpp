/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: ReloadPlannerUtils.cpp
 * Role: Stateless policy selection helpers for reload/copyover socket handling.
 */

#include "ReloadPlannerUtils.h"

bool shouldAttemptReloadMccpDisable(const bool connected, const int socketDescriptor,
                                    const bool mccpWasActive)
{
	return connected && socketDescriptor >= 0 && mccpWasActive;
}

ReloadWorldPolicyDecision computeReloadWorldPolicy(const ReloadWorldPolicyInput &input)
{
	ReloadWorldPolicyDecision decision;
	decision.connectedAtReload                  = input.connected || input.connecting;
	decision.shouldAttemptDescriptorInheritance = decision.connectedAtReload && input.socketDescriptor >= 0;

	if (!decision.connectedAtReload)
	{
		decision.policy = ReloadSocketPolicy::Reattach;
		return decision;
	}

	if (input.connected && input.socketDescriptor >= 0)
	{
		if (input.mccpWasActive && !input.mccpDisableSucceeded)
		{
			decision.policy = ReloadSocketPolicy::ParkReconnect;
			return decision;
		}
		decision.policy = ReloadSocketPolicy::Reattach;
		return decision;
	}

	decision.policy = ReloadSocketPolicy::ParkReconnect;
	return decision;
}
