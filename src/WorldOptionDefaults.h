/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldOptionDefaults.h
 * Role: Default-value declarations for world options used when creating worlds or resetting unset configuration fields.
 */

#ifndef QMUD_WORLDOPTIONDEFAULTS_H
#define QMUD_WORLDOPTIONDEFAULTS_H

#include <QMap>
#include <QString>

/**
 * @brief Alpha/string world-option metadata entry used by default tables.
 */
struct WorldAlphaOption
{
		constexpr WorldAlphaOption(const char *name = nullptr, const char *value = nullptr, int flags = 0)
		    : name(name), value(value), flags(flags)
		{
		}

		const char *name;
		const char *value;
		int         flags;
};

namespace QMudWorldOptionDefaults
{
	/**
	 * @brief Generates stable unique id for new world configuration.
	 * @return Generated unique id string.
	 */
	QString generateWorldUniqueId();
	/**
	 * @brief Applies default world attributes and multiline attributes.
	 * @param attrs World attribute map to populate.
	 * @param multilineAttrs Multiline-attribute map to populate.
	 */
	void    applyWorldOptionDefaults(QMap<QString, QString> &attrs, QMap<QString, QString> &multilineAttrs);
	/**
	 * @brief Looks up alpha option metadata by option name.
	 * @param name Alpha option name.
	 * @return Matching alpha option pointer, or `nullptr`.
	 */
	const WorldAlphaOption *findWorldAlphaOption(const QString &name);
} // namespace QMudWorldOptionDefaults

/**
 * @brief Returns pointer to static alpha-option table.
 * @return Pointer to alpha-option table.
 */
const WorldAlphaOption *worldAlphaOptions();
/**
 * @brief Returns number of entries in alpha-option table.
 * @return Alpha-option table entry count.
 */
int                     worldAlphaOptionCount();

#endif // QMUD_WORLDOPTIONDEFAULTS_H
