/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MainWindowHostResolver.h
 * Role: Resolver interfaces that locate the active MainWindowHost from application state without hard UI coupling.
 */

#ifndef QMUD_MAINWINDOWHOSTRESOLVER_H
#define QMUD_MAINWINDOWHOSTRESOLVER_H

class MainWindowHost;
class QObject;
class WorldRuntime;

/**
 * @brief Resolves a `MainWindowHost` instance from arbitrary QObject context.
 * @param context QObject context used to locate the host.
 * @return Resolved host pointer, or `nullptr` when unavailable.
 */
MainWindowHost *resolveMainWindowHost(QObject *context);
/**
 * @brief Resolves a `MainWindowHost` associated with a specific world runtime.
 * @param runtime Runtime whose host should be resolved.
 * @return Resolved host pointer, or `nullptr` when unavailable.
 */
MainWindowHost *resolveMainWindowHostForRuntime(const WorldRuntime *runtime);

#endif // QMUD_MAINWINDOWHOSTRESOLVER_H
