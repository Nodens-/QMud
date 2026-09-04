/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * @file PluginBroadcastSelectionUtils.h
 * @brief Shared helpers for selecting plugin broadcast recipients.
 *
 * Role: Keep broadcast recipient selection deterministic and reusable across runtime and tests
 * without pulling heavyweight runtime sources into dedicated test targets.
 */

#pragma once

// ReSharper disable once CppUnusedIncludeDirective
#include <QString>
#include <QVector>

/**
 * @brief Collects executable broadcast recipient indices, excluding the caller.
 *
 * @tparam PluginContainer Container type exposing `.size()`.
 * @tparam IsExecutableFn Callable `bool(int index)` returning executable status.
 * @tparam IdAtFn Callable `QString(int index)` returning plugin id for the index.
 * @param plugins Plugin container.
 * @param callingPluginId Caller plugin id (case-insensitive exclusion).
 * @param isExecutable Predicate for executable plugin checks.
 * @param idAt Accessor returning plugin id for each index.
 * @return Recipient indices in source order.
 */
template <typename PluginContainer, typename IsExecutableFn, typename IdAtFn>
QVector<int> qmudCollectBroadcastRecipientIndices(const PluginContainer &plugins,
                                                  const QString         &callingPluginId,
                                                  IsExecutableFn &&isExecutable, IdAtFn &&idAt)
{
	QVector<int> recipients;
	recipients.reserve(plugins.size());
	const bool hasCaller = !callingPluginId.isEmpty();
	for (int pluginIndex = 0; pluginIndex < plugins.size(); ++pluginIndex)
	{
		if (!isExecutable(pluginIndex))
			continue;
		if (hasCaller)
		{
			const QString pluginId = idAt(pluginIndex);
			if (pluginId == callingPluginId)
				continue;
		}
		recipients.push_back(pluginIndex);
	}
	return recipients;
}
