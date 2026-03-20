/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: ReloadStateUtils.h
 * Role: Data model and serialization helpers for reload/copyover state handoff between old/new QMud process
 * images.
 */

#ifndef QMUD_RELOADSTATEUTILS_H
#define QMUD_RELOADSTATEUTILS_H

#include <QDateTime>
#include <QList>
#include <QString>
// ReSharper disable once CppUnusedIncludeDirective
#include <QStringList>

/**
 * @brief Policy describing how one world socket should be handled during reload startup.
 */
enum class ReloadSocketPolicy
{
	Reattach,
	ParkReconnect
};

/**
 * @brief Serialized handoff state for one world entry during reload.
 */
struct ReloadWorldState
{
		int                sequence{0};
		QString            worldId;
		QString            displayName;
		QString            worldFilePath;
		QString            host;
		quint16            port{0};
		int                socketDescriptor{-1};
		ReloadSocketPolicy policy{ReloadSocketPolicy::Reattach};
		bool               connectedAtReload{false};
		bool               mccpWasActive{false};
		bool               mccpDisableAttempted{false};
		bool               mccpDisableSucceeded{false};
		bool               utf8Enabled{false};
		QString            notes;
};

/**
 * @brief Full reload manifest persisted before `exec` and consumed on startup.
 */
struct ReloadStateSnapshot
{
		int                     schemaVersion{1};
		QDateTime               createdAtUtc;
		QString                 reloadToken;
		QString                 targetExecutable;
		int                     activeWorldSequence{0};
		QStringList             arguments;
		QList<ReloadWorldState> worlds;
};

/**
 * @brief Validation expectations used for startup reload manifest checks.
 */
struct ReloadStartupValidationInput
{
		QString expectedToken;
		QString expectedTargetExecutable;
};

/**
 * @brief Returns default absolute reload-state file path under startup/data directory.
 * @param workingDirectory Startup/data directory path.
 * @return Absolute reload-state file path.
 */
[[nodiscard]] QString     reloadStateDefaultPath(const QString &workingDirectory);
/**
 * @brief Parses reload startup arguments from command line.
 * @param arguments Full process argument list.
 * @param statePath Output reload-state file path.
 * @param token Output reload token.
 * @return `true` when both required reload arguments were found.
 */
[[nodiscard]] bool        parseReloadStartupArguments(const QStringList &arguments, QString *statePath,
                                                      QString *token);
/**
 * @brief Returns startup arguments with reload-only flags removed.
 * @param arguments Full process argument list.
 * @return Argument list without `--reload-state=` / `--reload-token=` entries.
 */
[[nodiscard]] QStringList filterReloadStartupArguments(const QStringList &arguments);
/**
 * @brief Serializes one reload snapshot to disk atomically.
 * @param filePath Target JSON file path.
 * @param snapshot Snapshot payload to write.
 * @param errorMessage Optional output error text.
 * @return `true` on successful write.
 */
[[nodiscard]] bool writeReloadStateSnapshot(const QString &filePath, const ReloadStateSnapshot &snapshot,
                                            QString *errorMessage = nullptr);
/**
 * @brief Reads one reload snapshot from disk.
 * @param filePath Source JSON file path.
 * @param snapshot Output parsed snapshot.
 * @param errorMessage Optional output error text.
 * @return `true` when parsing/validation succeeds.
 */
[[nodiscard]] bool readReloadStateSnapshot(const QString &filePath, ReloadStateSnapshot *snapshot,
                                           QString *errorMessage = nullptr);
/**
 * @brief Validates startup snapshot metadata against expected token/executable values.
 * @param snapshot Parsed snapshot payload.
 * @param input Expected startup validation values.
 * @param errorMessage Optional output error text.
 * @return `true` when snapshot matches expected startup state.
 */
[[nodiscard]] bool validateReloadStartupSnapshot(const ReloadStateSnapshot          &snapshot,
                                                 const ReloadStartupValidationInput &input,
                                                 QString                            *errorMessage = nullptr);
/**
 * @brief Loads, validates, and consumes a startup reload-state snapshot.
 *
 * On parse/validation failure this helper still attempts to remove the manifest,
 * matching startup fallback cleanup behavior.
 *
 * @param filePath Source JSON file path.
 * @param input Expected startup validation values.
 * @param snapshot Output parsed snapshot on success.
 * @param errorMessage Optional output error text.
 * @param cleanupWarning Optional output warning when manifest cleanup fails.
 * @return `true` when load+validation succeeds; `false` otherwise.
 */
[[nodiscard]] bool loadValidatedAndConsumeReloadStateSnapshot(const QString                      &filePath,
                                                              const ReloadStartupValidationInput &input,
                                                              ReloadStateSnapshot                *snapshot,
                                                              QString *errorMessage   = nullptr,
                                                              QString *cleanupWarning = nullptr);
/**
 * @brief Removes reload-state file if it exists.
 * @param filePath Target JSON file path.
 * @param errorMessage Optional output error text.
 * @return `true` when file does not exist or deletion succeeds.
 */
[[nodiscard]] bool removeReloadStateFile(const QString &filePath, QString *errorMessage = nullptr);
/**
 * @brief Determines whether a reload-state file should be treated as stale.
 * @param filePath Reload-state file path.
 * @param maxAgeSeconds Maximum accepted file age in seconds.
 * @param nowUtc Reference UTC timestamp used for age calculation.
 * @return `true` when file exists and exceeds stale-age threshold.
 */
[[nodiscard]] bool isReloadStateFileStale(const QString &filePath, int maxAgeSeconds,
                                          const QDateTime &nowUtc = QDateTime::currentDateTimeUtc());

#endif // QMUD_RELOADSTATEUTILS_H
