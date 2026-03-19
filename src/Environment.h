/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: Environment.h
 * Role: Centralized lookup helpers for QMud-scoped environment variables with system-config fallback support.
 */

#ifndef QMUD_ENVIRONMENT_H
#define QMUD_ENVIRONMENT_H

// ReSharper disable once CppUnusedIncludeDirective
#include <QString>
#include <QStringList>

/**
 * @brief Resolves an environment variable with QMud config fallback.
 *
 * Lookup order:
 * 1. Process environment variable.
 * 2. For names starting with `QMUD_`, fallback to OS config files (user-first then system where applicable).
 *
 * @param name Environment variable name.
 * @return Variable value, or empty string when unavailable.
 */
QString qmudEnvironmentVariable(const QString &name);
/**
 * @brief Checks whether an environment variable is available from env or config fallback.
 * @param name Environment variable name.
 * @return `true` when variable is defined.
 */
bool    qmudEnvironmentVariableIsSet(const QString &name);
/**
 * @brief Checks whether resolved environment variable value is empty.
 * @param name Environment variable name.
 * @return `true` when value is empty or unavailable.
 */
bool    qmudEnvironmentVariableIsEmpty(const QString &name);
/**
 * @brief Enables or disables user/system config fallback for `QMUD_*` variables.
 * @param enabled `true` to allow fallback, `false` to resolve only process environment.
 */
void    qmudSetEnvironmentConfigFallbackEnabled(bool enabled);
/**
 * @brief Overrides config-file search paths for tests and clears cached parsed values.
 * @param paths Explicit config search paths in priority order.
 */
void    qmudSetEnvironmentConfigSearchPathsForTesting(const QStringList &paths);
/**
 * @brief Clears test config search path overrides and cached parsed values.
 */
void    qmudClearEnvironmentConfigSearchPathsForTesting();

#endif // QMUD_ENVIRONMENT_H
