/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: StartTlsFallbackUtils.h
 * Role: START-TLS timeout fallback guard helpers shared by runtime and tests.
 */

#ifndef QMUD_STARTTLSFALLBACKUTILS_H
#define QMUD_STARTTLSFALLBACKUTILS_H

#include "WorldOptions.h"

#include <QtGlobal>

/**
 * @brief Snapshot used by START-TLS timeout fallback guard checks.
 */
struct StartTlsFallbackContext
{
		quint64 armedGeneration{0};
		quint64 currentGeneration{0};
		bool    socketAvailable{false};
		bool    socketConnected{false};
		bool    socketReadyForWorld{false};
		bool    tlsEncryptionEnabled{false};
		int     tlsMethod{eTlsDirect};
};

/**
 * @brief Returns whether START-TLS timeout should downgrade to plain session setup.
 *
 * The fallback only applies when the currently-armed timer generation is still
 * current, the socket remains connected but not yet world-ready, and the world
 * is explicitly in START-TLS mode.
 *
 * @param context Evaluated runtime snapshot.
 * @return `true` when timeout fallback should run.
 */
[[nodiscard]] inline bool shouldFallbackToPlainOnStartTlsTimeout(const StartTlsFallbackContext &context)
{
	return context.armedGeneration == context.currentGeneration && context.socketAvailable &&
	       context.socketConnected && !context.socketReadyForWorld && context.tlsEncryptionEnabled &&
	       context.tlsMethod == eTlsStartTls;
}

#endif // QMUD_STARTTLSFALLBACKUTILS_H
