/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldRuntimeAttributeUtils.h
 * Role: Small helpers for attribute-map mutations used by WorldRuntime setters.
 */

#ifndef QMUD_WORLDRUNTIMEATTRIBUTEUTILS_H
#define QMUD_WORLDRUNTIMEATTRIBUTEUTILS_H

#include <QMap>
#include <QString>

/**
 * @brief Inserts or updates multiline world attribute only when value changed.
 * @param attributes Mutable multiline-attribute map.
 * @param key Attribute key.
 * @param value Attribute value to upsert.
 * @return `true` when map was modified, `false` when existing value already matched.
 */
inline bool upsertWorldMultilineAttributeIfChanged(QMap<QString, QString> &attributes, const QString &key,
                                                   const QString &value)
{
	if (const auto existing = attributes.constFind(key);
	    existing != attributes.constEnd() && existing.value() == value)
	{
		return false;
	}
	attributes.insert(key, value);
	return true;
}

#endif // QMUD_WORLDRUNTIMEATTRIBUTEUTILS_H
