/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MushclientImportUtils.h
 * Role: Shared MUSHclient import and legacy path/config migration helper APIs.
 */

#ifndef QMUD_MUSHCLIENTIMPORTUTILS_H
#define QMUD_MUSHCLIENTIMPORTUTILS_H

// ReSharper disable once CppUnusedIncludeDirective
#include <QString>
#include <QStringList>

class QSqlDatabase;

namespace QMudMushclientImportUtils
{
	/**
	 * @brief Summary counters returned by an explicit MUSHclient directory import.
	 */
	struct ImportStats
	{
			int         filesCopied{0};
			int         filesSkippedExisting{0};
			int         filesSkippedFiltered{0};
			int         worldsConverted{0};
			int         configsImported{0};
			int         preferenceDatabasesImported{0};
			int         warnings{0};
			int         errors{0};
			QStringList warningDetails;
			QStringList errorDetails;
	};

	/**
	 * @brief Migrates one legacy or modern world path to QMud storage form.
	 * @param sourceBaseDir Directory where legacy source files are resolved.
	 * @param destinationBaseDir Directory where migrated QMud files are written.
	 * @param path Legacy path value to migrate.
	 * @param archiveLegacySource Move the legacy source to destination `migrated/` after conversion.
	 * @return QMud storage path for world values, even when the referenced source cannot be read.
	 */
	[[nodiscard]] QString migrateLegacyWorldFilePath(const QString &sourceBaseDir,
	                                                 const QString &destinationBaseDir, const QString &path,
	                                                 bool archiveLegacySource);
	/**
	 * @brief Migrates a serialized world-list path value.
	 * @param baseDir QMUD_HOME/source root for startup migration.
	 * @param worldList Serialized `*`-delimited world-list value.
	 * @param changed Optional output indicating whether the serialized value changed.
	 * @return Migrated serialized world-list value.
	 */
	[[nodiscard]] QString migrateWorldListPaths(const QString &baseDir, const QString &worldList,
	                                            bool *changed = nullptr);
	/**
	 * @brief Canonicalizes a serialized world list for runtime use.
	 * @param worldList Serialized world-list value.
	 * @return Canonical serialized value.
	 */
	[[nodiscard]] QString canonicalizeWorldListForRuntime(const QString &worldList);
	/**
	 * @brief Migrates one legacy plugin path to QMud storage form.
	 * @param baseDir QMUD_HOME/source root for startup migration.
	 * @param path Legacy plugin path value.
	 * @return QMud storage path for plugin values.
	 */
	[[nodiscard]] QString migrateLegacyPluginFilePath(const QString &baseDir, const QString &path);
	/**
	 * @brief Migrates a serialized plugin-list path value.
	 * @param baseDir QMUD_HOME/source root for startup migration.
	 * @param pluginList Serialized `*`-delimited plugin-list value.
	 * @param changed Optional output indicating whether the serialized value changed.
	 * @return Migrated serialized plugin-list value.
	 */
	[[nodiscard]] QString migratePluginListPaths(const QString &baseDir, const QString &pluginList,
	                                             bool *changed = nullptr);
	/**
	 * @brief Canonicalizes a serialized plugin list for runtime use.
	 * @param pluginList Serialized plugin-list value.
	 * @return Canonical serialized value.
	 */
	[[nodiscard]] QString canonicalizePluginListForRuntime(const QString &pluginList);
	/**
	 * @brief Migrates all legacy `.mcl` files under a world directory.
	 * @param baseDir QMUD_HOME/source root for startup migration.
	 * @param worldDirectory World directory setting value.
	 */
	void                  migrateLegacyWorldTree(const QString &baseDir, const QString &worldDirectory);
	/**
	 * @brief Migrates a single MUSHclient INI path value.
	 * @param baseDir QMUD_HOME/source root for startup migration.
	 * @param key INI key name.
	 * @param value INI value.
	 * @return Migrated value.
	 */
	[[nodiscard]] QString migrateLegacyIniPathValue(const QString &baseDir, const QString &key,
	                                                const QString &value);
	/**
	 * @brief Migrates a single MUSHclient INI path value with separate source/destination roots.
	 * @param sourceBaseDir Directory where legacy source paths are resolved.
	 * @param destinationBaseDir QMUD_HOME where migrated paths are stored.
	 * @param key INI key name.
	 * @param value INI value.
	 * @param archiveLegacySource Move legacy world files after conversion when `true`.
	 * @return Migrated value.
	 */
	[[nodiscard]] QString migrateLegacyIniPathValue(const QString &sourceBaseDir,
	                                                const QString &destinationBaseDir, const QString &key,
	                                                const QString &value, bool archiveLegacySource);
	/**
	 * @brief Imports/converts MUSHclient.ini to QMud.conf.
	 * @param sourceDir Directory containing MUSHclient.ini.
	 * @param destinationDir QMUD_HOME where QMud.conf is written.
	 * @param archiveSource Move source INI into `migrated/` after conversion.
	 */
	void                  migrateLegacyIniToQmudConf(const QString &sourceDir, const QString &destinationDir,
	                                                 bool archiveSource);
	/**
	 * @brief Checks the legacy corrupted relative world path shape.
	 * @param path Path value to inspect.
	 * @return `true` for the known `.world`/`.mcl` corruption shape.
	 */
	[[nodiscard]] bool    isLikelyCorruptedRelativeWorldPath(const QString &path);
	/**
	 * @brief Imports an explicit MUSHclient root directory into QMUD_HOME.
	 * @param mushclientRoot Selected MUSHclient root, treated read-only.
	 * @param qmudHome QMud data root destination.
	 * @return Import counters and errors.
	 */
	[[nodiscard]] ImportStats importDirectory(const QString &mushclientRoot, const QString &qmudHome);
	/**
	 * @brief Migrates MUSHclient preference database content into an already-open QMud database.
	 * @param mushclientRoot Selected MUSHclient root, treated read-only.
	 * @param destinationDb Open QMud preferences database connection to flush and populate.
	 * @param stats Import counters updated with success or failure details.
	 */
	void importPreferencesDatabase(const QString &mushclientRoot, QSqlDatabase &destinationDb,
	                               ImportStats &stats);
} // namespace QMudMushclientImportUtils

#endif // QMUD_MUSHCLIENTIMPORTUTILS_H
