/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldSessionStateUtils.h
 * Role: Binary persistence helpers for per-world output-buffer and command-history session state.
 */

#ifndef QMUD_WORLD_SESSION_STATE_UTILS_H
#define QMUD_WORLD_SESSION_STATE_UTILS_H

#include "WorldRuntime.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QStringList>

namespace QMudWorldSessionState
{
	/**
	 * @brief Serialized session-state payload for one world.
	 */
	struct WorldSessionStateData
	{
			bool                                      hasOutputBuffer{false};
			bool                                      hasCommandHistory{false};
			bool                                      hasCustomMxpElements{false};
			bool                                      hasMxpSessionState{false};
			QVector<WorldRuntime::LineEntry>          outputLines;
			QStringList                               commandHistory;
			QList<TelnetProcessor::CustomElementInfo> customMxpElements;
			TelnetProcessor::MxpSessionState          mxpSessionState;
	};

	/**
	 * @brief Writes one world session-state file atomically.
	 * @param filePath Absolute target path.
	 * @param state Session-state payload to persist.
	 * @param errorMessage Optional output error text.
	 * @return `true` when write succeeds.
	 */
	[[nodiscard]] bool writeSessionStateFile(const QString &filePath, const WorldSessionStateData &state,
	                                         QString *errorMessage = nullptr);
	/**
	 * @brief Reads one world session-state file.
	 * @param filePath Absolute source path.
	 * @param state Output payload.
	 * @param errorMessage Optional output error text.
	 * @return `true` when parse succeeds.
	 */
	[[nodiscard]] bool readSessionStateFile(const QString &filePath, WorldSessionStateData *state,
	                                        QString *errorMessage = nullptr);
	/**
	 * @brief Removes one world session-state file if it exists.
	 * @param filePath Absolute target path.
	 * @param errorMessage Optional output error text.
	 * @return `true` when file does not exist or was removed.
	 */
	[[nodiscard]] bool removeSessionStateFile(const QString &filePath, QString *errorMessage = nullptr);
} // namespace QMudWorldSessionState

#endif // QMUD_WORLD_SESSION_STATE_UTILS_H
