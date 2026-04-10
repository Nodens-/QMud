/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: ReloadPlannerUtils.h
 * Role: Stateless helpers for deciding per-world socket reload policies (reattach vs reconnect fallback).
 */

#ifndef QMUD_RELOADPLANNERUTILS_H
#define QMUD_RELOADPLANNERUTILS_H

#include "ReloadStateUtils.h"

/**
 * @brief Input snapshot used to decide reload policy for one world.
 */
struct ReloadWorldPolicyInput
{
		bool connected{false};
		bool connecting{false};
		int  socketDescriptor{-1};
		bool mccpWasActive{false};
		bool mccpDisableSucceeded{false};
};

/**
 * @brief Computed reload policy decision for one world.
 */
struct ReloadWorldPolicyDecision
{
		bool               connectedAtReload{false};
		bool               shouldAttemptDescriptorInheritance{false};
		ReloadSocketPolicy policy{ReloadSocketPolicy::Reattach};
};

/**
 * @brief Returns whether reload planning should attempt MCCP shutdown for this world.
 * @param connected `true` when socket is currently connected.
 * @param socketDescriptor Native socket descriptor, or `-1`.
 * @param mccpWasActive `true` when MCCP was active at reload time.
 * @return `true` when MCCP disable attempt should be issued.
 */
[[nodiscard]] bool shouldAttemptReloadMccpDisable(bool connected, int socketDescriptor, bool mccpWasActive);

/**
 * @brief Computes per-world reload socket policy.
 * @param input Per-world connection and MCCP state.
 * @return Decision containing connected-at-reload state, inheritance hint, and selected policy.
 */
[[nodiscard]] ReloadWorldPolicyDecision computeReloadWorldPolicy(const ReloadWorldPolicyInput &input);

#endif // QMUD_RELOADPLANNERUTILS_H
