/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldRuleEnableUtils.h
 * Role: Shared enable-gating helpers for world-owned vs plugin-owned rule evaluation.
 */

#ifndef QMUD_WORLDRULEENABLEUTILS_H
#define QMUD_WORLDRULEENABLEUTILS_H

#include <QMap>
#include <QString>

/**
 * @brief Rule families that can be owned by world configuration or plugins.
 */
enum class WorldRuleKind
{
	Alias,
	Trigger,
	Timer
};

/**
 * @brief Returns whether a world attribute value should be treated as enabled.
 * @param value Raw world attribute value.
 * @return `true` when value is one of `1`, `y`, or `true` (case-insensitive), else `false`.
 */
inline bool isWorldRuleValueEnabled(const QString &value)
{
	return value == QStringLiteral("1") || value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
	       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
}

/**
 * @brief Resolves world enable-attribute key for a rule family.
 * @param kind Rule family.
 * @return World attribute key (`enable_aliases`, `enable_triggers`, `enable_timers`).
 */
inline QString worldRuleEnableKey(const WorldRuleKind kind)
{
	switch (kind)
	{
	case WorldRuleKind::Alias:
		return QStringLiteral("enable_aliases");
	case WorldRuleKind::Trigger:
		return QStringLiteral("enable_triggers");
	case WorldRuleKind::Timer:
		return QStringLiteral("enable_timers");
	}

	return QStringLiteral("enable_aliases");
}

/**
 * @brief Returns whether a rule collection should be evaluated for current scope.
 * @param worldAttributes World attribute map.
 * @param kind Rule family to evaluate.
 * @param pluginOwned `true` for plugin-owned rules, `false` for world-owned rules.
 * @return `true` for all plugin-owned rules; for world-owned rules, follows matching enable flag.
 */
inline bool shouldEvaluateRuleCollection(const QMap<QString, QString> &worldAttributes,
                                         const WorldRuleKind kind, const bool pluginOwned)
{
	if (pluginOwned)
		return true;
	return isWorldRuleValueEnabled(worldAttributes.value(worldRuleEnableKey(kind)));
}

#endif // QMUD_WORLDRULEENABLEUTILS_H
