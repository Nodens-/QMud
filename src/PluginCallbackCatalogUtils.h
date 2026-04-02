/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: PluginCallbackCatalogUtils.h
 * Role: Helpers for tracking/pruning observed plugin callback names in generation-based caches.
 */

#ifndef QMUD_PLUGINCALLBACKCATALOGUTILS_H
#define QMUD_PLUGINCALLBACKCATALOGUTILS_H

#include <QHash>
#include <QSet>
#include <QString>

/**
 * @brief Returns number of generations an observed callback name should be retained without queries.
 * @return Retention window size in generations.
 */
inline quint64 observedPluginCallbackRetentionGenerations()
{
	return 256;
}

/**
 * @brief Records that a callback name was queried in the current generation.
 * @param queryGenerationByName Mutable map from callback name to last query generation.
 * @param functionName Callback function name that was queried.
 * @param generation Current callback-catalog generation.
 */
inline void noteObservedPluginCallbackQuery(QHash<QString, quint64> &queryGenerationByName,
                                            const QString &functionName, const quint64 generation)
{
	if (functionName.isEmpty())
		return;
	queryGenerationByName.insert(functionName, generation);
}

/**
 * @brief Ensures callback name exists in observed-callback set.
 * @param observedCallbackNames Mutable set of observed callback names.
 * @param functionName Callback function name to ensure.
 * @return `true` if name was inserted, `false` if empty or already present.
 */
inline bool ensureObservedPluginCallback(QSet<QString> &observedCallbackNames, const QString &functionName)
{
	if (functionName.isEmpty() || observedCallbackNames.contains(functionName))
		return false;
	observedCallbackNames.insert(functionName);
	return true;
}

/**
 * @brief Removes observed callback names that exceeded retention window.
 * @param observedCallbackNames Mutable set of observed callback names.
 * @param queryGenerationByName Mutable map from callback name to last query generation.
 * @param currentGeneration Current callback-catalog generation.
 * @param retentionGenerations Maximum age (in generations) before pruning.
 */
inline void pruneStaleObservedPluginCallbacks(QSet<QString>           &observedCallbackNames,
                                              QHash<QString, quint64> &queryGenerationByName,
                                              const quint64            currentGeneration,
                                              const quint64            retentionGenerations)
{
	for (auto it = observedCallbackNames.begin(); it != observedCallbackNames.end();)
	{
		const QString &name          = *it;
		const quint64  lastQueriedAt = queryGenerationByName.value(name, 0);
		if (lastQueriedAt != 0)
		{
			const quint64 age = currentGeneration >= lastQueriedAt ? (currentGeneration - lastQueriedAt) : 0;
			if (age <= retentionGenerations)
			{
				++it;
				continue;
			}
		}
		queryGenerationByName.remove(name);
		it = observedCallbackNames.erase(it);
	}
}

/**
 * @brief Advances callback-catalog generation counter while keeping zero reserved as invalid.
 * @param generation Mutable generation counter.
 */
inline void advanceObservedPluginCallbackGeneration(quint64 &generation)
{
	++generation;
	if (generation == 0)
		++generation;
}

/**
 * @brief Clears observed-callback tracking state and resets generation counter.
 * @param observedCallbackNames Mutable set of observed callback names.
 * @param queryGenerationByName Mutable map from callback name to last query generation.
 * @param generation Mutable generation counter.
 */
inline void resetObservedPluginCallbackTracking(QSet<QString>           &observedCallbackNames,
                                                QHash<QString, quint64> &queryGenerationByName,
                                                quint64                 &generation)
{
	observedCallbackNames.clear();
	queryGenerationByName.clear();
	generation = 1;
}

#endif // QMUD_PLUGINCALLBACKCATALOGUTILS_H
