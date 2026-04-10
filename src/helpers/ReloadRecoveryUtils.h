/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: ReloadRecoveryUtils.h
 * Role: Header-only helpers for reload startup socket recovery/reconnect sequencing.
 */

#ifndef QMUD_RELOADRECOVERYUTILS_H
#define QMUD_RELOADRECOVERYUTILS_H

#include "ReloadStateUtils.h"

#include <QString>

/**
 * @brief Per-world startup recovery socket handling outcome.
 */
enum class ReloadRecoverySocketOutcome
{
	Noop,
	Reattached,
	ReconnectQueued
};

/**
 * @brief Decision result from per-world startup recovery socket handling.
 */
struct ReloadRecoverySocketDecision
{
		ReloadRecoverySocketOutcome outcome{ReloadRecoverySocketOutcome::Noop};
		bool                        closeSocketFirst{false};
		QString                     error;
};

/**
 * @brief Applies startup recovery socket handling for one world.
 *
 * Reattaches the inherited descriptor when possible, otherwise queues reconnect
 * fallback (with optional close-before-reconnect when policy is park/reconnect).
 *
 * @tparam TRuntime Runtime type implementing socket adoption/pause methods.
 * @param runtime Runtime instance.
 * @param worldState Serialized world recovery state.
 * @return Recovery socket decision describing next action.
 */
template <typename TRuntime>
[[nodiscard]] ReloadRecoverySocketDecision applyReloadSocketRecovery(TRuntime &runtime,
                                                                     const ReloadWorldState &worldState)
{
	ReloadRecoverySocketDecision decision;

	if (worldState.connectedAtReload && worldState.socketDescriptor >= 0)
	{
		runtime.markReloadReattachConnectActionsSuppressed();
		QString adoptError;
		if (!runtime.adoptConnectedSocketDescriptor(worldState.socketDescriptor, &adoptError))
		{
			(void)runtime.consumeReloadReattachConnectActionsSuppressed();
			decision.outcome          = ReloadRecoverySocketOutcome::ReconnectQueued;
			decision.closeSocketFirst = false;
			decision.error            = adoptError;
			return decision;
		}

			if (worldState.policy == ReloadSocketPolicy::ParkReconnect)
			{
				runtime.setIncomingSocketDataPaused(true);
				decision.outcome          = ReloadRecoverySocketOutcome::ReconnectQueued;
				decision.closeSocketFirst = true;
				return decision;
			}

			decision.outcome = ReloadRecoverySocketOutcome::Reattached;
			return decision;
		}

	if (worldState.connectedAtReload)
	{
		decision.outcome          = ReloadRecoverySocketOutcome::ReconnectQueued;
		decision.closeSocketFirst = false;
	}

	return decision;
}

/**
 * @brief Executes reconnect for one runtime after startup recovery.
 *
 * @tparam TRuntime Runtime type implementing close/pause/attributes/connect methods.
 * @param runtime Runtime instance.
 * @param worldState Serialized world recovery state.
 * @param closeSocketFirst Close inherited parked socket before reconnect.
 * @return Empty string on success; warning text when reconnect cannot be started.
 */
template <typename TRuntime>
[[nodiscard]] QString reconnectRecoveredRuntime(TRuntime &runtime, const ReloadWorldState &worldState,
                                                const bool closeSocketFirst)
{
	if (closeSocketFirst)
		runtime.closeSocketForReloadReconnect();
	runtime.setIncomingSocketDataPaused(false);

	QString host = worldState.host.trimmed();
	quint16 port = worldState.port;
	if (host.isEmpty())
		host = runtime.worldAttributes().value(QStringLiteral("site")).trimmed();
	if (port == 0)
		port = runtime.worldAttributes().value(QStringLiteral("port")).toUShort();
	if (host.isEmpty() || port == 0)
	{
		return QStringLiteral("Reload reconnect skipped due to missing host/port for world %1")
		    .arg(worldState.displayName);
	}

	runtime.setWorldAttribute(QStringLiteral("site"), host);
	runtime.setWorldAttribute(QStringLiteral("port"), QString::number(port));
	if (!runtime.connectToWorld(host, port))
	{
		return QStringLiteral("Reload reconnect failed to start for world %1 (%2:%3)")
		    .arg(worldState.displayName, host, QString::number(port));
	}
	return {};
}

#endif // QMUD_RELOADRECOVERYUTILS_H
