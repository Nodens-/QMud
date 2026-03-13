/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: LuaApiExport.cpp
 * Role: Runtime Lua-state inspector that exports grouped API inventory files for regression tracking.
 */

#include "LuaApiExport.h"

#include "LuaCallbackEngine.h"
#ifdef QMUD_ENABLE_LUA_SCRIPTING
#include "LuaHeaders.h"
#include <QDir>
#include <QSaveFile>
#include <QSet>
#include <QStringList>
#include <QTextStream>
#endif

namespace
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	QStringList sortedUnique(QStringList values)
	{
		values.removeAll(QString());
		const QSet unique(values.cbegin(), values.cend());
		values = unique.values();
		values.sort(Qt::CaseSensitive);
		return values;
	}

	QStringList functionKeysFromTable(lua_State &state, const bool includeMetaFunctions)
	{
		QStringList names;
		const int   tableIndex = lua_absindex(&state, -1);
		lua_pushnil(&state);
		while (lua_next(&state, tableIndex) != 0)
		{
			if (lua_type(&state, -2) == LUA_TSTRING && lua_type(&state, -1) == LUA_TFUNCTION)
			{
				size_t length = 0;
				if (const char *key = lua_tolstring(&state, -2, &length); key && length > 0)
				{
					if (const QString name = QString::fromUtf8(key, static_cast<int>(length));
					    includeMetaFunctions || !name.startsWith(QLatin1String("__")))
						names.append(name);
				}
			}
			lua_pop(&state, 1);
		}

		return sortedUnique(names);
	}

	QStringList functionKeysFromGlobalTable(lua_State &state, const char *globalName,
	                                        const bool includeMetaFunctions)
	{
		lua_getglobal(&state, globalName);
		if (!lua_istable(&state, -1))
		{
			lua_pop(&state, 1);
			return {};
		}

		const QStringList names = functionKeysFromTable(state, includeMetaFunctions);
		lua_pop(&state, 1);
		return names;
	}

	QStringList functionKeysFromNamedMetatable(lua_State &state, const char *metatableName,
	                                           const bool includeMetaFunctions)
	{
		luaL_getmetatable(&state, metatableName);
		if (!lua_istable(&state, -1))
		{
			lua_pop(&state, 1);
			return {};
		}

		const QStringList names = functionKeysFromTable(state, includeMetaFunctions);
		lua_pop(&state, 1);
		return names;
	}

	QStringList functionKeysFromWorldMetatable(lua_State &state)
	{
		lua_getglobal(&state, "world");
		if (!lua_istable(&state, -1))
		{
			lua_pop(&state, 1);
			return {};
		}

		if (!lua_getmetatable(&state, -1))
		{
			lua_pop(&state, 1);
			return {};
		}

		const QStringList names = functionKeysFromTable(state, true);
		lua_pop(&state, 2);
		return names;
	}

	bool requireSqlite3Module(lua_State &state)
	{
		lua_getglobal(&state, "require");
		if (!lua_isfunction(&state, -1))
		{
			lua_pop(&state, 1);
			return false;
		}

		lua_pushstring(&state, "sqlite3");
		if (lua_pcall(&state, 1, 1, 0) != 0)
		{
			lua_pop(&state, 1);
			return false;
		}

		lua_pop(&state, 1);
		return true;
	}

	QStringList prefixed(const QStringList &names, const QString &prefix)
	{
		QStringList out;
		out.reserve(names.size());
		for (const QString &name : names)
			out.append(prefix + name);
		return out;
	}

	bool writeInventoryFile(const QString &path, const QStringList &entries,
	                        const QString &exportTimestampUtc, QString *errorMessage)
	{
		QSaveFile outFile(path);
		if (!outFile.open(QIODevice::WriteOnly | QIODevice::Text))
		{
			if (errorMessage)
				*errorMessage =
				    QStringLiteral("Failed to open '%1' for writing: %2").arg(path, outFile.errorString());
			return false;
		}

		QTextStream stream(&outFile);
		stream << "# QMud Lua API inventory export\n";
		stream << "# Exported (UTC): " << exportTimestampUtc << "\n";
		stream << "\n";
		for (const QString &entry : entries)
			stream << entry << "\n";

		if (stream.status() != QTextStream::Ok)
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Failed while writing '%1'.").arg(path);
			return false;
		}

		if (!outFile.commit())
		{
			if (errorMessage)
				*errorMessage =
				    QStringLiteral("Failed to finalize '%1': %2").arg(path, outFile.errorString());
			return false;
		}

		return true;
	}

	QStringList collectUtilsCompressFunctions(const QStringList &utilsUnion)
	{
		static const char *kCompressNames[] = {"base64decode", "base64encode", "compress", "decompress",
		                                       "fromhex",      "hash",         "md5",      "readdir",
		                                       "sha256",       "split",        "tohex",    nullptr};

		const QSet         available(utilsUnion.cbegin(), utilsUnion.cend());
		QStringList        out;
		for (const char **name = kCompressNames; *name; ++name)
		{
			if (const QString key = QString::fromLatin1(*name); available.contains(key))
				out.append(key);
		}
		return sortedUnique(out);
	}

	QStringList subtractEntries(const QStringList &left, const QStringList &right)
	{
		QSet values(left.cbegin(), left.cend());
		for (const QString &entry : right)
			values.remove(entry);

		QStringList out = values.values();
		out.sort(Qt::CaseSensitive);
		return out;
	}

	QStringList functionSignatureLinesFromUtils(lua_State *state)
	{
		if (!state)
			return {};

		lua_getglobal(state, "utils");
		if (!lua_istable(state, -1))
		{
			lua_pop(state, 1);
			return {};
		}

		lua_getfield(state, -1, "functionargs");
		if (!lua_isfunction(state, -1))
		{
			lua_pop(state, 2);
			return {};
		}

		if (lua_pcall(state, 0, 1, 0) != 0)
		{
			lua_pop(state, 2);
			return {};
		}

		if (!lua_istable(state, -1))
		{
			lua_pop(state, 2);
			return {};
		}

		QStringList lines;
		const int   tableIndex = lua_absindex(state, -1);
		lua_pushnil(state);
		while (lua_next(state, tableIndex) != 0)
		{
			if (lua_type(state, -2) == LUA_TSTRING && lua_type(state, -1) == LUA_TSTRING)
			{
				size_t      nameLen      = 0;
				size_t      signatureLen = 0;
				const char *name         = lua_tolstring(state, -2, &nameLen);
				if (const char *signature = lua_tolstring(state, -1, &signatureLen); name && signature)
					lines.append(QString::fromUtf8(name, static_cast<int>(nameLen)) + QLatin1Char('\t') +
					             QString::fromUtf8(signature, static_cast<int>(signatureLen)));
			}
			lua_pop(state, 1);
		}

		lua_pop(state, 2);
		lines.sort(Qt::CaseSensitive);
		return lines;
	}

#endif
} // namespace

bool exportLuaApiInventory(const QString &outputDirectory, QString *errorMessage)
{
#ifndef QMUD_ENABLE_LUA_SCRIPTING
	if (errorMessage)
		*errorMessage = QStringLiteral("Lua scripting support is disabled in this build.");
	return false;
#else
	if (outputDirectory.trimmed().isEmpty())
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Output directory is empty.");
		return false;
	}

	QDir outDir(outputDirectory);
	if (!outDir.exists() && !outDir.mkpath(QStringLiteral(".")))
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Failed to create output directory '%1'.")
			                    .arg(QDir::toNativeSeparators(outputDirectory));
		return false;
	}

	LuaCallbackEngine engine;
	if (!engine.loadScript())
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Failed to initialize Lua callback engine.");
		return false;
	}

	lua_State *state = engine.luaState();
	if (!state)
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Lua callback engine did not expose a Lua state.");
		return false;
	}

	requireSqlite3Module(*state);

	const QStringList  worldLib          = functionKeysFromGlobalTable(*state, "world", false);
	const QStringList &worldBindingTable = worldLib;

	QStringList        worldMeta = functionKeysFromWorldMetatable(*state);
	worldMeta.append(functionKeysFromNamedMetatable(*state, "QMud.WorldProxy", true));
	worldMeta = sortedUnique(worldMeta);

	const QStringList  utilsUnion             = functionKeysFromGlobalTable(*state, "utils", false);
	const QStringList &utilsBindingTable      = utilsUnion;
	const QStringList  utilsCompress          = collectUtilsCompressFunctions(utilsUnion);
	const QStringList  utilsXml               = subtractEntries(utilsUnion, utilsCompress);
	const QStringList  functionSignatureTable = functionSignatureLinesFromUtils(state);

	const QStringList  sqliteDb =
	    prefixed(functionKeysFromNamedMetatable(*state, "qmud.sqlite3.db", false), QStringLiteral("db:"));
	const QStringList sqliteStmt =
	    prefixed(functionKeysFromNamedMetatable(*state, "qmud.sqlite3.stmt", false), QStringLiteral("stmt:"));
	const QStringList sqliteModule =
	    prefixed(functionKeysFromGlobalTable(*state, "sqlite3", false), QStringLiteral("sqlite3."));
	QStringList sqliteCnt = sqliteDb;
	sqliteCnt.append(sqliteModule);
	sqliteCnt.append(sqliteStmt);
	sqliteCnt = sortedUnique(sqliteCnt);

	const QStringList bitLib      = functionKeysFromGlobalTable(*state, "bit", false);
	const QStringList rexLib      = functionKeysFromGlobalTable(*state, "rex", false);
	const QStringList rexPcreMeta = functionKeysFromNamedMetatable(*state, "rex_pcremeta", true);

	const QStringList bcLib = functionKeysFromGlobalTable(*state, "bc", true);

	const QStringList lpegLib  = functionKeysFromGlobalTable(*state, "lpeg", false);
	const QStringList lpegMeta = functionKeysFromNamedMetatable(*state, "lpeg-pattern", true);

	const QStringList progressLib = functionKeysFromGlobalTable(*state, "progress", false);
	const QStringList progressMeta =
	    functionKeysFromNamedMetatable(*state, "mushclient.progress_dialog_handle", true);

	const QString exportTimestampUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

	struct OutputSpec
	{
			const char *fileSuffix{nullptr};
			QStringList lines;
	};

	const QList<OutputSpec> outputs = {
	    {"world_binding_table",      worldBindingTable     },
	    {"utils_binding_table",      utilsBindingTable     },
	    {"function_signature_table", functionSignatureTable},
	    {"worldlib",                 worldLib              },
	    {"worldlib_meta",            worldMeta             },
	    {"utils_union",              utilsUnion            },
	    {"utils_xmllib",             utilsXml              },
	    {"utils_compresslib",        utilsCompress         },
	    {"sqlite_cnt",               sqliteCnt             },
	    {"bitlib",	               bitLib                },
	    {"rexlib",	               rexLib                },
	    {"rex_pcremeta",             rexPcreMeta           },
	    {"bc",	                   bcLib                 },
	    {"lpeg",	                 lpegLib               },
	    {"lpeg_meta",                lpegMeta              },
	    {"progress_lib",             progressLib           },
	    {"progress_meta",            progressMeta          }
    };

	QStringList apiCountLines;
	for (const auto &[fileSuffix, lines] : outputs)
		apiCountLines.append(QStringLiteral("%1,%2").arg(QString::fromLatin1(fileSuffix)).arg(lines.size()));
	apiCountLines.sort(Qt::CaseSensitive);

	for (const auto &[fileSuffix, lines] : outputs)
	{
		const QString path =
		    outDir.filePath(QStringLiteral("qmud_%1.txt").arg(QString::fromLatin1(fileSuffix)));
		if (!writeInventoryFile(path, lines, exportTimestampUtc, errorMessage))
			return false;
	}

	if (const QString countsPath = outDir.filePath(QStringLiteral("qmud_api_count.txt"));
	    !writeInventoryFile(countsPath, apiCountLines, exportTimestampUtc, errorMessage))
		return false;

	return true;
#endif
}
