/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: Sqlite3Lua.cpp
 * Role: Lua-facing SQLite binding implementation that exposes database operations to embedded scripts.
 */

#include "LuaHeaders.h"
#include "SqliteCompat.h"

#include <QMetaType>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>
#include <QtSql/QSqlRecord>

namespace
{

	struct LuaSqliteStmt;
	struct LuaSqliteIter;

	struct LuaSqliteDb
	{
			QSqlDatabase          db;
			QString               connectionName;
			bool                  open{false};
			int                   lastErrCode{SQLITE_OK};
			QString               lastErrMsg;
			QSet<LuaSqliteStmt *> statements;
			QSet<LuaSqliteIter *> iterators;
	};

	struct LuaSqliteStmt
	{
			LuaSqliteDb *db{nullptr};
			QSqlQuery    query;
			bool         prepared{false};
			bool         executed{false};
			bool         hasRow{false};
			int          columns{0};
			QStringList  paramNames;
			lua_State   *ownerState{nullptr};
			int          dbRef{LUA_NOREF};
	};

	struct LuaSqliteIter
	{
			LuaSqliteDb *db{nullptr};
			QSqlQuery    query;
			bool         named{false};
			bool         unpacked{false};
			bool         active{false};
			int          columns{0};
			lua_State   *ownerState{nullptr};
			int          dbRef{LUA_NOREF};
	};

	struct LuaSqliteStmtIter
	{
			LuaSqliteStmt *stmt{nullptr};
			bool           named{false};
			bool           unpacked{false};
			lua_State     *ownerState{nullptr};
			int            stmtRef{LUA_NOREF};
	};

	int mapSqlErrorToSqlite(const QSqlError &err)
	{
		if (!err.isValid())
			return SQLITE_OK;
		const QString text = err.text().toLower();
		if (text.contains(QStringLiteral("locked")) || text.contains(QStringLiteral("busy")))
			return SQLITE_BUSY;
		if (text.contains(QStringLiteral("readonly")))
			return SQLITE_READONLY;
		return SQLITE_ERROR;
	}

	int applySqliteWalAndNormalSynchronous(const QSqlDatabase &db, QString &errorMessage)
	{
		if (!db.isValid() || !db.isOpen())
		{
			errorMessage = QStringLiteral("Database connection is not open.");
			return SQLITE_ERROR;
		}

		QSqlQuery query(db);
		if (!query.exec(QStringLiteral("PRAGMA journal_mode=WAL")))
		{
			errorMessage = query.lastError().text();
			return mapSqlErrorToSqlite(query.lastError());
		}

		if (const QString mode = query.next() ? query.value(0).toString().trimmed() : QString();
		    mode.compare(QStringLiteral("wal"), Qt::CaseInsensitive) != 0)
		{
			errorMessage = QStringLiteral("PRAGMA journal_mode returned '%1' instead of 'wal'.").arg(mode);
			return SQLITE_ERROR;
		}

		if (!query.exec(QStringLiteral("PRAGMA synchronous=NORMAL")))
		{
			errorMessage = query.lastError().text();
			return mapSqlErrorToSqlite(query.lastError());
		}

		return SQLITE_OK;
	}

	void setDbStatus(LuaSqliteDb *db, const int code, const QString &message)
	{
		if (!db)
			return;
		db->lastErrCode = code;
		db->lastErrMsg  = message;
	}

	void setDbStatusFromError(LuaSqliteDb *db, const QSqlError &err)
	{
		if (!db)
			return;
		setDbStatus(db, mapSqlErrorToSqlite(err), err.text());
	}

	bool isDbUsable(const LuaSqliteDb *db)
	{
		return db && db->open && db->db.isValid() && db->db.isOpen();
	}

	void trackStatement(LuaSqliteDb *db, LuaSqliteStmt *stmt)
	{
		if (!db || !stmt)
			return;
		db->statements.insert(stmt);
	}

	void untrackStatement(LuaSqliteDb *db, LuaSqliteStmt *stmt)
	{
		if (!db)
			return;
		db->statements.remove(stmt);
	}

	void trackIterator(LuaSqliteDb *db, LuaSqliteIter *iter)
	{
		if (!db || !iter)
			return;
		db->iterators.insert(iter);
	}

	void untrackIterator(LuaSqliteDb *db, LuaSqliteIter *iter)
	{
		if (!db)
			return;
		db->iterators.remove(iter);
	}

	void releaseDbRef(lua_State *L, int &ref)
	{
		if (!L || ref == LUA_NOREF)
			return;
		luaL_unref(L, LUA_REGISTRYINDEX, ref);
		ref = LUA_NOREF;
	}

	int luaReturnCount(lua_State *L, const int topBefore)
	{
		return lua_gettop(L) - topBefore;
	}

	void invalidateStatement(LuaSqliteStmt *stmt)
	{
		if (!stmt)
			return;
		stmt->query.finish();
		// Fully detach from the connection before removeDatabase(); finished
		// queries can still keep internal handles alive.
		stmt->query    = QSqlQuery();
		stmt->prepared = false;
		stmt->executed = false;
		stmt->hasRow   = false;
		stmt->columns  = 0;
		stmt->paramNames.clear();
		untrackStatement(stmt->db, stmt);
		stmt->db = nullptr;
	}

	void invalidateIterator(LuaSqliteIter *iter)
	{
		if (!iter)
			return;
		iter->query.finish();
		// Fully detach from the connection before removeDatabase(); finished
		// queries can still keep internal handles alive.
		iter->query   = QSqlQuery();
		iter->active  = false;
		iter->columns = 0;
		untrackIterator(iter->db, iter);
		iter->db = nullptr;
	}

	int mapVariantTypeToSqlite(const QVariant &value)
	{
		if (value.isNull())
			return SQLITE_NULL;
		switch (value.typeId())
		{
		case QMetaType::Int:
		case QMetaType::UInt:
		case QMetaType::LongLong:
		case QMetaType::ULongLong:
		case QMetaType::Short:
		case QMetaType::UShort:
		case QMetaType::Char:
		case QMetaType::UChar:
			return SQLITE_INTEGER;
		case QMetaType::Double:
		case QMetaType::Float:
			return SQLITE_FLOAT;
		case QMetaType::QByteArray:
			return SQLITE_BLOB;
		default:
			return SQLITE_TEXT;
		}
	}

	QStringList collectParamNames(const QString &sql)
	{
		QStringList     names;
		const qsizetype len = sql.size();
		for (qsizetype i = 0; i < len; ++i)
		{
			const QChar ch = sql.at(i);
			if (ch == QLatin1Char('?'))
			{
				names << QStringLiteral("?");
				continue;
			}
			if (ch == QLatin1Char(':') || ch == QLatin1Char('@') || ch == QLatin1Char('$'))
			{
				const qsizetype start = i;
				if (i + 1 < len && (sql.at(i + 1).isLetter() || sql.at(i + 1) == QLatin1Char('_')))
				{
					qsizetype j = i + 2;
					while (j < len && (sql.at(j).isLetterOrNumber() || sql.at(j) == QLatin1Char('_')))
						++j;
					names << sql.mid(start, j - start);
					i = j - 1;
				}
			}
		}
		return names;
	}

	QStringList splitSqlStatements(const QString &sqlText)
	{
		enum class ParseMode
		{
			Normal,
			SingleQuote,
			DoubleQuote,
			BracketIdentifier,
			BacktickIdentifier,
			LineComment,
			BlockComment
		};

		QStringList     statements;
		QString         current;
		auto            mode = ParseMode::Normal;
		const qsizetype len  = sqlText.size();

		for (qsizetype i = 0; i < len; ++i)
		{
			const QChar ch   = sqlText.at(i);
			const QChar next = i + 1 < len ? sqlText.at(i + 1) : QChar();

			switch (mode)
			{
			case ParseMode::Normal:
				if (ch == QLatin1Char('\''))
				{
					mode = ParseMode::SingleQuote;
					current += ch;
				}
				else if (ch == QLatin1Char('"'))
				{
					mode = ParseMode::DoubleQuote;
					current += ch;
				}
				else if (ch == QLatin1Char('['))
				{
					mode = ParseMode::BracketIdentifier;
					current += ch;
				}
				else if (ch == QLatin1Char('`'))
				{
					mode = ParseMode::BacktickIdentifier;
					current += ch;
				}
				else if (ch == QLatin1Char('-') && next == QLatin1Char('-'))
				{
					mode = ParseMode::LineComment;
					++i;
				}
				else if (ch == QLatin1Char('/') && next == QLatin1Char('*'))
				{
					mode = ParseMode::BlockComment;
					++i;
				}
				else if (ch == QLatin1Char(';'))
				{
					if (const QString trimmed = current.trimmed(); !trimmed.isEmpty())
						statements << trimmed;
					current.clear();
				}
				else
				{
					current += ch;
				}
				break;

			case ParseMode::SingleQuote:
				current += ch;
				if (ch == QLatin1Char('\''))
				{
					if (next == QLatin1Char('\''))
					{
						current += next;
						++i;
					}
					else
					{
						mode = ParseMode::Normal;
					}
				}
				break;

			case ParseMode::DoubleQuote:
				current += ch;
				if (ch == QLatin1Char('"'))
				{
					if (next == QLatin1Char('"'))
					{
						current += next;
						++i;
					}
					else
					{
						mode = ParseMode::Normal;
					}
				}
				break;

			case ParseMode::BracketIdentifier:
				current += ch;
				if (ch == QLatin1Char(']'))
					mode = ParseMode::Normal;
				break;

			case ParseMode::BacktickIdentifier:
				current += ch;
				if (ch == QLatin1Char('`'))
				{
					if (next == QLatin1Char('`'))
					{
						current += next;
						++i;
					}
					else
					{
						mode = ParseMode::Normal;
					}
				}
				break;

			case ParseMode::LineComment:
				if (ch == QLatin1Char('\n') || ch == QLatin1Char('\r'))
					mode = ParseMode::Normal;
				break;

			case ParseMode::BlockComment:
				if (ch == QLatin1Char('*') && next == QLatin1Char('/'))
				{
					++i;
					mode = ParseMode::Normal;
				}
				break;
			}
		}

		if (const QString trailing = current.trimmed(); !trailing.isEmpty())
			statements << trailing;

		return statements;
	}

	bool isSqlComplete(const QString &sqlText)
	{
		enum class ParseMode
		{
			Normal,
			SingleQuote,
			DoubleQuote,
			BracketIdentifier,
			BacktickIdentifier,
			LineComment,
			BlockComment
		};

		auto            mode                    = ParseMode::Normal;
		bool            sawTerminatingSemicolon = false;
		const qsizetype len                     = sqlText.size();

		for (qsizetype i = 0; i < len; ++i)
		{
			const QChar ch   = sqlText.at(i);
			const QChar next = i + 1 < len ? sqlText.at(i + 1) : QChar();

			switch (mode)
			{
			case ParseMode::Normal:
				if (ch == QLatin1Char('\''))
				{
					mode                    = ParseMode::SingleQuote;
					sawTerminatingSemicolon = false;
				}
				else if (ch == QLatin1Char('"'))
				{
					mode                    = ParseMode::DoubleQuote;
					sawTerminatingSemicolon = false;
				}
				else if (ch == QLatin1Char('['))
				{
					mode                    = ParseMode::BracketIdentifier;
					sawTerminatingSemicolon = false;
				}
				else if (ch == QLatin1Char('`'))
				{
					mode                    = ParseMode::BacktickIdentifier;
					sawTerminatingSemicolon = false;
				}
				else if (ch == QLatin1Char('-') && next == QLatin1Char('-'))
				{
					mode = ParseMode::LineComment;
					++i;
				}
				else if (ch == QLatin1Char('/') && next == QLatin1Char('*'))
				{
					mode = ParseMode::BlockComment;
					++i;
				}
				else if (ch == QLatin1Char(';'))
				{
					sawTerminatingSemicolon = true;
				}
				else if (!ch.isSpace())
				{
					sawTerminatingSemicolon = false;
				}
				break;

			case ParseMode::SingleQuote:
				if (ch == QLatin1Char('\''))
				{
					if (next == QLatin1Char('\''))
						++i;
					else
						mode = ParseMode::Normal;
				}
				break;

			case ParseMode::DoubleQuote:
				if (ch == QLatin1Char('"'))
				{
					if (next == QLatin1Char('"'))
						++i;
					else
						mode = ParseMode::Normal;
				}
				break;

			case ParseMode::BracketIdentifier:
				if (ch == QLatin1Char(']'))
					mode = ParseMode::Normal;
				break;

			case ParseMode::BacktickIdentifier:
				if (ch == QLatin1Char('`'))
				{
					if (next == QLatin1Char('`'))
						++i;
					else
						mode = ParseMode::Normal;
				}
				break;

			case ParseMode::LineComment:
				if (ch == QLatin1Char('\n') || ch == QLatin1Char('\r'))
					mode = ParseMode::Normal;
				break;

			case ParseMode::BlockComment:
				if (ch == QLatin1Char('*') && next == QLatin1Char('/'))
				{
					++i;
					mode = ParseMode::Normal;
				}
				break;
			}
		}

		return mode == ParseMode::Normal && sawTerminatingSemicolon;
	}

	QString sqliteDriverVersion()
	{
		static QString cachedVersion;
		static int     probeCounter = 0;

		if (!cachedVersion.isEmpty())
			return cachedVersion;

		const QString probeConnection = QStringLiteral("lua_sqlite_probe_%1").arg(++probeCounter);
		{
			QSqlDatabase probe = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), probeConnection);
			probe.setDatabaseName(QStringLiteral(":memory:"));
			if (probe.open())
			{
				if (QSqlQuery query(probe);
				    query.exec(QStringLiteral("SELECT sqlite_version()")) && query.next())
					cachedVersion = query.value(0).toString();
				probe.close();
			}
		}
		QSqlDatabase::removeDatabase(probeConnection);

		if (cachedVersion.isEmpty())
			cachedVersion = QStringLiteral("unknown");

		return cachedVersion;
	}

	LuaSqliteDb *checkdb(lua_State *L)
	{
		return static_cast<LuaSqliteDb *>(luaL_checkudata(L, 1, "qmud.sqlite3.db"));
	}

	LuaSqliteStmt *checkstmt(lua_State *L)
	{
		return static_cast<LuaSqliteStmt *>(luaL_checkudata(L, 1, "qmud.sqlite3.stmt"));
	}

	void bindValue(LuaSqliteStmt *stmt, const int index, lua_State *L, const int valueIndex)
	{
		if (lua_isnil(L, valueIndex))
			stmt->query.bindValue(index, QVariant());
		else if (lua_isnumber(L, valueIndex))
			stmt->query.bindValue(index, lua_tonumber(L, valueIndex));
		else if (lua_isboolean(L, valueIndex))
			stmt->query.bindValue(index, lua_toboolean(L, valueIndex) != 0);
		else
			stmt->query.bindValue(index, QString::fromUtf8(lua_tostring(L, valueIndex)));
	}

	void bindValueNamed(LuaSqliteStmt *stmt, const QString &name, lua_State *L)
	{
		if (lua_isnil(L, -1))
			stmt->query.bindValue(name, QVariant());
		else if (lua_isnumber(L, -1))
			stmt->query.bindValue(name, lua_tonumber(L, -1));
		else if (lua_isboolean(L, -1))
			stmt->query.bindValue(name, lua_toboolean(L, -1) != 0);
		else
			stmt->query.bindValue(name, QString::fromUtf8(lua_tostring(L, -1)));
	}

	int dbIsOpen(lua_State *L)
	{
		const LuaSqliteDb *db = checkdb(L);
		lua_pushboolean(L, isDbUsable(db));
		return 1;
	}

	int dbClose(lua_State *L)
	{
		LuaSqliteDb *db = checkdb(L);

		if (!db->statements.isEmpty())
		{
			QVector<LuaSqliteStmt *> statements;
			statements.reserve(db->statements.size());
			for (LuaSqliteStmt *stmt : db->statements)
				statements.push_back(stmt);
			for (LuaSqliteStmt *stmt : statements)
			{
				invalidateStatement(stmt);
				releaseDbRef(stmt->ownerState, stmt->dbRef);
				stmt->ownerState = nullptr;
			}
			db->statements.clear();
		}

		if (!db->iterators.isEmpty())
		{
			QVector<LuaSqliteIter *> iterators;
			iterators.reserve(db->iterators.size());
			for (LuaSqliteIter *iter : db->iterators)
				iterators.push_back(iter);
			for (LuaSqliteIter *iter : iterators)
			{
				invalidateIterator(iter);
				releaseDbRef(iter->ownerState, iter->dbRef);
				iter->ownerState = nullptr;
			}
			db->iterators.clear();
		}

		if (db->open)
		{
			db->db.close();
			db->open = false;
		}
		db->db = QSqlDatabase();
		QSqlDatabase::removeDatabase(db->connectionName);
		db->lastErrCode = SQLITE_OK;
		db->lastErrMsg.clear();
		lua_pushnumber(L, SQLITE_OK);
		return 1;
	}

	int dbToString(lua_State *L)
	{
		const LuaSqliteDb *db    = checkdb(L);
		const char        *state = isDbUsable(db) ? "open" : "closed";
		lua_pushfstring(L, "sqlite database (%s)", state);
		return 1;
	}

	int stmtToString(lua_State *L)
	{
		lua_pushstring(L, "sqlite statement");
		return 1;
	}

	int stmtIsOpen(lua_State *L)
	{
		const LuaSqliteStmt *stmt = checkstmt(L);
		lua_pushboolean(L, stmt->prepared);
		return 1;
	}

	int stmtFinalize(lua_State *L)
	{
		LuaSqliteStmt *stmt = checkstmt(L);
		invalidateStatement(stmt);
		releaseDbRef(stmt->ownerState, stmt->dbRef);
		stmt->ownerState = nullptr;
		lua_pushnumber(L, SQLITE_OK);
		return 1;
	}

	int stmtReset(lua_State *L)
	{
		const int      topBefore = lua_gettop(L);
		LuaSqliteStmt *stmt      = checkstmt(L);
		if (!isDbUsable(stmt->db))
		{
			setDbStatus(stmt->db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite database"));
			lua_pushnumber(L, SQLITE_MISUSE);
			return luaReturnCount(L, topBefore);
		}
		stmt->query.finish();
		stmt->executed = false;
		stmt->hasRow   = false;
		stmt->columns  = 0;
		lua_pushnumber(L, SQLITE_OK);
		return luaReturnCount(L, topBefore);
	}

	int stmtColumns(lua_State *L)
	{
		const LuaSqliteStmt *stmt = checkstmt(L);
		lua_pushnumber(L, stmt->columns);
		return 1;
	}

	int stmtStep(lua_State *L)
	{
		const int      topBefore = lua_gettop(L);
		LuaSqliteStmt *stmt      = checkstmt(L);
		if (!isDbUsable(stmt->db))
		{
			setDbStatus(stmt->db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite database"));
			lua_pushnumber(L, SQLITE_MISUSE);
			return luaReturnCount(L, topBefore);
		}
		if (!stmt->prepared)
		{
			setDbStatus(stmt->db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite statement"));
			lua_pushnumber(L, SQLITE_MISUSE);
			return luaReturnCount(L, topBefore);
		}
		if (!stmt->executed)
		{
			if (!stmt->query.exec())
			{
				setDbStatusFromError(stmt->db, stmt->query.lastError());
				lua_pushnumber(L, stmt->db ? stmt->db->lastErrCode
				                           : mapSqlErrorToSqlite(stmt->query.lastError()));
				return luaReturnCount(L, topBefore);
			}
			stmt->executed = true;
			stmt->columns  = stmt->query.record().count();
			setDbStatus(stmt->db, SQLITE_OK, QString());
		}

		if (stmt->query.next())
		{
			stmt->hasRow  = true;
			stmt->columns = stmt->query.record().count();
			lua_pushnumber(L, SQLITE_ROW);
			return luaReturnCount(L, topBefore);
		}

		stmt->hasRow = false;
		lua_pushnumber(L, SQLITE_DONE);
		return luaReturnCount(L, topBefore);
	}

	bool advanceStmt(LuaSqliteStmt *stmt)
	{
		if (!stmt)
			return false;
		if (!isDbUsable(stmt->db))
		{
			setDbStatus(stmt->db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite database"));
			return false;
		}
		if (!stmt->prepared)
		{
			setDbStatus(stmt->db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite statement"));
			return false;
		}
		if (!stmt->executed)
		{
			if (!stmt->query.exec())
			{
				setDbStatusFromError(stmt->db, stmt->query.lastError());
				return false;
			}
			stmt->executed = true;
			stmt->columns  = stmt->query.record().count();
			setDbStatus(stmt->db, SQLITE_OK, QString());
		}
		if (stmt->query.next())
		{
			stmt->hasRow  = true;
			stmt->columns = stmt->query.record().count();
			return true;
		}
		stmt->hasRow = false;
		return false;
	}

	void pushValue(lua_State *L, const QVariant &value)
	{
		if (value.isNull())
		{
			lua_pushnil(L);
			return;
		}
		switch (value.typeId())
		{
		case QMetaType::Int:
		case QMetaType::UInt:
		case QMetaType::LongLong:
		case QMetaType::ULongLong:
		case QMetaType::Short:
		case QMetaType::UShort:
		case QMetaType::Char:
		case QMetaType::UChar:
			lua_pushinteger(L, value.toLongLong());
			return;
		case QMetaType::Double:
		case QMetaType::Float:
			lua_pushnumber(L, value.toDouble());
			return;
		case QMetaType::QByteArray:
		{
			const QByteArray bytes = value.toByteArray();
			lua_pushlstring(L, bytes.constData(), bytes.size());
			return;
		}
		default:
		{
			const QByteArray bytes = value.toString().toUtf8();
			lua_pushlstring(L, bytes.constData(), bytes.size());
		}
		}
	}

	void setNamedValue(lua_State *L, const QString &fieldName, const QVariant &value)
	{
		if (fieldName.isEmpty())
			return;
		const int        absTableIndex = lua_gettop(L);

		const QByteArray rawKey = fieldName.toUtf8();
		pushValue(L, value);
		lua_setfield(L, absTableIndex, rawKey.constData());

		if (const QString lowerName = fieldName.toLower(); lowerName != fieldName)
		{
			const QByteArray lowerKey = lowerName.toUtf8();
			pushValue(L, value);
			lua_setfield(L, absTableIndex, lowerKey.constData());
		}

		if (const qsizetype dot = fieldName.lastIndexOf(QLatin1Char('.'));
		    dot > 0 && dot + 1 < fieldName.size())
		{
			const QString    shortName = fieldName.mid(dot + 1);
			const QByteArray shortKey  = shortName.toUtf8();
			pushValue(L, value);
			lua_setfield(L, absTableIndex, shortKey.constData());

			if (const QString lowerShort = shortName.toLower(); lowerShort != shortName)
			{
				const QByteArray lowerShortKey = lowerShort.toUtf8();
				pushValue(L, value);
				lua_setfield(L, absTableIndex, lowerShortKey.constData());
			}
		}
	}

	int stmtGetValue(lua_State *L)
	{
		const int            topBefore = lua_gettop(L);
		const LuaSqliteStmt *stmt      = checkstmt(L);
		if (!isDbUsable(stmt->db))
		{
			setDbStatus(stmt->db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite database"));
			lua_pushnil(L);
			return luaReturnCount(L, topBefore);
		}
		const lua_Integer indexValue = luaL_checkinteger(L, 2);
		if (!stmt->hasRow || indexValue < 1 || indexValue > stmt->columns)
		{
			lua_pushnil(L);
			return luaReturnCount(L, topBefore);
		}
		const int index = static_cast<int>(indexValue);
		pushValue(L, stmt->query.value(index - 1));
		return luaReturnCount(L, topBefore);
	}

	int stmtGetValues(lua_State *L)
	{
		const int            topBefore = lua_gettop(L);
		const LuaSqliteStmt *stmt      = checkstmt(L);
		if (!isDbUsable(stmt->db))
		{
			setDbStatus(stmt->db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite database"));
			lua_newtable(L);
			return luaReturnCount(L, topBefore);
		}
		lua_newtable(L);
		if (!stmt->hasRow)
			return luaReturnCount(L, topBefore);
		for (int i = 0; i < stmt->columns; ++i)
		{
			pushValue(L, stmt->query.value(i));
			lua_rawseti(L, -2, i + 1);
		}
		return luaReturnCount(L, topBefore);
	}

	int stmtGetName(lua_State *L)
	{
		const int            topBefore = lua_gettop(L);
		const LuaSqliteStmt *stmt      = checkstmt(L);
		if (!isDbUsable(stmt->db))
		{
			setDbStatus(stmt->db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite database"));
			lua_pushnil(L);
			return luaReturnCount(L, topBefore);
		}
		const lua_Integer indexValue = luaL_checkinteger(L, 2);
		if (indexValue < 1 || indexValue > stmt->columns)
		{
			lua_pushnil(L);
			return luaReturnCount(L, topBefore);
		}
		const int        index = static_cast<int>(indexValue);
		const QByteArray bytes = stmt->query.record().fieldName(index - 1).toUtf8();
		lua_pushlstring(L, bytes.constData(), bytes.size());
		return luaReturnCount(L, topBefore);
	}

	int stmtGetNames(lua_State *L)
	{
		const int            topBefore = lua_gettop(L);
		const LuaSqliteStmt *stmt      = checkstmt(L);
		if (!isDbUsable(stmt->db))
		{
			setDbStatus(stmt->db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite database"));
			lua_newtable(L);
			return luaReturnCount(L, topBefore);
		}
		lua_newtable(L);
		for (int i = 0; i < stmt->columns; ++i)
		{
			const QByteArray bytes = stmt->query.record().fieldName(i).toUtf8();
			lua_pushlstring(L, bytes.constData(), bytes.size());
			lua_rawseti(L, -2, i + 1);
		}
		return luaReturnCount(L, topBefore);
	}

	int stmtGetType(lua_State *L)
	{
		const int            topBefore = lua_gettop(L);
		const LuaSqliteStmt *stmt      = checkstmt(L);
		if (!isDbUsable(stmt->db))
		{
			setDbStatus(stmt->db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite database"));
			lua_pushnil(L);
			return luaReturnCount(L, topBefore);
		}
		const lua_Integer indexValue = luaL_checkinteger(L, 2);
		if (!stmt->hasRow || indexValue < 1 || indexValue > stmt->columns)
		{
			lua_pushnil(L);
			return luaReturnCount(L, topBefore);
		}
		const int index = static_cast<int>(indexValue);
		lua_pushnumber(L, mapVariantTypeToSqlite(stmt->query.value(index - 1)));
		return luaReturnCount(L, topBefore);
	}

	int stmtGetTypes(lua_State *L)
	{
		const int            topBefore = lua_gettop(L);
		const LuaSqliteStmt *stmt      = checkstmt(L);
		if (!isDbUsable(stmt->db))
		{
			setDbStatus(stmt->db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite database"));
			lua_newtable(L);
			return luaReturnCount(L, topBefore);
		}
		lua_newtable(L);
		if (!stmt->hasRow)
			return luaReturnCount(L, topBefore);
		for (int i = 0; i < stmt->columns; ++i)
		{
			lua_pushnumber(L, mapVariantTypeToSqlite(stmt->query.value(i)));
			lua_rawseti(L, -2, i + 1);
		}
		return luaReturnCount(L, topBefore);
	}

	int stmtGetNamedValues(lua_State *L)
	{
		const int            topBefore = lua_gettop(L);
		const LuaSqliteStmt *stmt      = checkstmt(L);
		if (!isDbUsable(stmt->db))
		{
			setDbStatus(stmt->db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite database"));
			lua_newtable(L);
			return luaReturnCount(L, topBefore);
		}
		lua_newtable(L);
		if (!stmt->hasRow)
			return luaReturnCount(L, topBefore);
		const QSqlRecord record = stmt->query.record();
		for (int i = 0; i < stmt->columns; ++i)
		{
			setNamedValue(L, record.fieldName(i), stmt->query.value(i));
		}
		return luaReturnCount(L, topBefore);
	}

	int stmtGetNamedTypes(lua_State *L)
	{
		const int            topBefore = lua_gettop(L);
		const LuaSqliteStmt *stmt      = checkstmt(L);
		if (!isDbUsable(stmt->db))
		{
			setDbStatus(stmt->db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite database"));
			lua_newtable(L);
			return luaReturnCount(L, topBefore);
		}
		lua_newtable(L);
		if (!stmt->hasRow)
			return luaReturnCount(L, topBefore);
		const QSqlRecord record = stmt->query.record();
		for (int i = 0; i < stmt->columns; ++i)
		{
			const QString fieldName = record.fieldName(i);
			const int     type      = mapVariantTypeToSqlite(stmt->query.value(i));
			setNamedValue(L, fieldName, type);
		}
		return luaReturnCount(L, topBefore);
	}

	int stmtBind(lua_State *L)
	{
		const int      topBefore = lua_gettop(L);
		LuaSqliteStmt *stmt      = checkstmt(L);
		if (!isDbUsable(stmt->db))
		{
			setDbStatus(stmt->db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite database"));
			lua_pushnumber(L, SQLITE_MISUSE);
			return luaReturnCount(L, topBefore);
		}
		if (!stmt->prepared)
		{
			setDbStatus(stmt->db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite statement"));
			lua_pushnumber(L, SQLITE_MISUSE);
			return luaReturnCount(L, topBefore);
		}

		if (lua_type(L, 2) == LUA_TNUMBER)
		{
			const int index = static_cast<int>(lua_tonumber(L, 2));
			if (index <= 0)
			{
				setDbStatus(stmt->db, SQLITE_RANGE, QStringLiteral("column index out of range"));
				lua_pushnumber(L, SQLITE_RANGE);
				return luaReturnCount(L, topBefore);
			}
			bindValue(stmt, index - 1, L, 3);
		}
		else if (lua_type(L, 2) == LUA_TSTRING)
		{
			const QString raw   = QString::fromUtf8(lua_tostring(L, 2));
			const QChar   first = raw.isEmpty() ? QChar() : raw.at(0);
			const QString name =
			    first == QLatin1Char(':') || first == QLatin1Char('@') || first == QLatin1Char('$')
			        ? raw
			        : QStringLiteral(":") + raw;
			if (lua_isnil(L, 3))
				stmt->query.bindValue(name, QVariant());
			else if (lua_isnumber(L, 3))
				stmt->query.bindValue(name, lua_tonumber(L, 3));
			else if (lua_isboolean(L, 3))
				stmt->query.bindValue(name, lua_toboolean(L, 3) != 0);
			else
				stmt->query.bindValue(name, QString::fromUtf8(lua_tostring(L, 3)));
		}
		else
		{
			setDbStatus(stmt->db, SQLITE_MISUSE, QStringLiteral("bad bind parameter"));
			lua_pushnumber(L, SQLITE_MISUSE);
			return luaReturnCount(L, topBefore);
		}

		setDbStatus(stmt->db, SQLITE_OK, QString());
		lua_pushnumber(L, SQLITE_OK);
		return luaReturnCount(L, topBefore);
	}

	int stmtBindValues(lua_State *L)
	{
		const int      topBefore = lua_gettop(L);
		LuaSqliteStmt *stmt      = checkstmt(L);
		if (!isDbUsable(stmt->db) || !stmt->prepared)
		{
			setDbStatus(stmt->db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite statement"));
			lua_pushnumber(L, SQLITE_MISUSE);
			return luaReturnCount(L, topBefore);
		}
		const int top = lua_gettop(L);
		for (int i = 2; i <= top; ++i)
			bindValue(stmt, i - 2, L, i);
		setDbStatus(stmt->db, SQLITE_OK, QString());
		lua_pushnumber(L, SQLITE_OK);
		return luaReturnCount(L, topBefore);
	}

	int stmtBindNames(lua_State *L)
	{
		const int      topBefore = lua_gettop(L);
		LuaSqliteStmt *stmt      = checkstmt(L);
		if (!isDbUsable(stmt->db) || !stmt->prepared)
		{
			setDbStatus(stmt->db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite statement"));
			lua_pushnumber(L, SQLITE_MISUSE);
			return luaReturnCount(L, topBefore);
		}
		luaL_checktype(L, 2, LUA_TTABLE);
		lua_pushnil(L);
		while (lua_next(L, 2))
		{
			if (const char *key = lua_tostring(L, -2); key)
			{
				const QString raw   = QString::fromUtf8(key);
				const QChar   first = raw.isEmpty() ? QChar() : raw.at(0);
				const QString name =
				    first == QLatin1Char(':') || first == QLatin1Char('@') || first == QLatin1Char('$')
				        ? raw
				        : QStringLiteral(":") + raw;
				bindValueNamed(stmt, name, L);
			}
			lua_pop(L, 1);
		}
		setDbStatus(stmt->db, SQLITE_OK, QString());
		lua_pushnumber(L, SQLITE_OK);
		return luaReturnCount(L, topBefore);
	}

	int stmtBindBlob(lua_State *L)
	{
		const int      topBefore = lua_gettop(L);
		LuaSqliteStmt *stmt      = checkstmt(L);
		if (!isDbUsable(stmt->db) || !stmt->prepared)
		{
			setDbStatus(stmt->db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite statement"));
			lua_pushnumber(L, SQLITE_MISUSE);
			return luaReturnCount(L, topBefore);
		}
		const int   index = static_cast<int>(luaL_checkinteger(L, 2));
		size_t      len   = 0;
		const char *data  = luaL_checklstring(L, 3, &len);
		stmt->query.bindValue(index - 1, QByteArray(data, static_cast<int>(len)));
		setDbStatus(stmt->db, SQLITE_OK, QString());
		lua_pushnumber(L, SQLITE_OK);
		return luaReturnCount(L, topBefore);
	}

	int stmtBindParameterCount(lua_State *L)
	{
		const int            topBefore = lua_gettop(L);
		const LuaSqliteStmt *stmt      = checkstmt(L);
		if (!isDbUsable(stmt->db))
		{
			setDbStatus(stmt->db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite database"));
			lua_pushnumber(L, 0);
			return luaReturnCount(L, topBefore);
		}
		lua_pushnumber(L, static_cast<lua_Number>(stmt->paramNames.size()));
		return luaReturnCount(L, topBefore);
	}

	int stmtBindParameterName(lua_State *L)
	{
		const int            topBefore = lua_gettop(L);
		const LuaSqliteStmt *stmt      = checkstmt(L);
		if (!isDbUsable(stmt->db))
		{
			setDbStatus(stmt->db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite database"));
			lua_pushnil(L);
			return luaReturnCount(L, topBefore);
		}
		const int index = static_cast<int>(luaL_checkinteger(L, 2));
		if (index < 1 || index > stmt->paramNames.size())
		{
			lua_pushnil(L);
			return luaReturnCount(L, topBefore);
		}
		const QByteArray bytes = stmt->paramNames.at(index - 1).toUtf8();
		lua_pushlstring(L, bytes.constData(), bytes.size());
		return luaReturnCount(L, topBefore);
	}

	int iterGc(lua_State *L)
	{
		if (auto *iter = static_cast<LuaSqliteIter *>(lua_touserdata(L, 1)); iter)
		{
			invalidateIterator(iter);
			releaseDbRef(iter->ownerState, iter->dbRef);
			iter->ownerState = nullptr;
		}
		return 0;
	}

	int stmtIterGc(lua_State *L)
	{
		if (auto *iter = static_cast<LuaSqliteStmtIter *>(lua_touserdata(L, 1)); iter)
		{
			releaseDbRef(iter->ownerState, iter->stmtRef);
			iter->ownerState = nullptr;
			iter->stmt       = nullptr;
		}
		return 0;
	}

	int iterNext(lua_State *L)
	{
		auto *iter = static_cast<LuaSqliteIter *>(lua_touserdata(L, lua_upvalueindex(1)));
		if (!iter || !iter->active)
			return 0;
		if (!isDbUsable(iter->db))
		{
			setDbStatus(iter->db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite database"));
			invalidateIterator(iter);
			releaseDbRef(iter->ownerState, iter->dbRef);
			iter->ownerState = nullptr;
			return 0;
		}
		if (!iter->query.next())
		{
			if (iter->query.lastError().isValid() && iter->query.lastError().type() != QSqlError::NoError)
			{
				setDbStatusFromError(iter->db, iter->query.lastError());
				const QByteArray msg = iter->query.lastError().text().toUtf8();
				lua_pushlstring(L, msg.constData(), msg.size());
				lua_error(L);
				return 0;
			}
			setDbStatus(iter->db, SQLITE_OK, QString());
			iter->active = false;
			iter->query.finish();
			return 0;
		}
		const QSqlRecord record = iter->query.record();
		iter->columns           = record.count();

		if (iter->unpacked)
		{
			for (int i = 0; i < iter->columns; ++i)
				pushValue(L, iter->query.value(i));
			return iter->columns;
		}

		lua_newtable(L);
		if (iter->named)
		{
			for (int i = 0; i < iter->columns; ++i)
			{
				setNamedValue(L, record.fieldName(i), iter->query.value(i));
			}
		}
		else
		{
			for (int i = 0; i < iter->columns; ++i)
			{
				pushValue(L, iter->query.value(i));
				lua_rawseti(L, -2, i + 1);
			}
		}
		return 1;
	}

	int stmtIterNext(lua_State *L)
	{
		auto *iter = static_cast<LuaSqliteStmtIter *>(lua_touserdata(L, lua_upvalueindex(1)));
		if (!iter || !iter->stmt)
			return 0;
		if (!isDbUsable(iter->stmt->db))
		{
			setDbStatus(iter->stmt->db, SQLITE_MISUSE,
			            QStringLiteral("attempt to use closed sqlite database"));
			releaseDbRef(iter->ownerState, iter->stmtRef);
			iter->ownerState = nullptr;
			iter->stmt       = nullptr;
			return 0;
		}
		if (!advanceStmt(iter->stmt))
		{
			if (iter->stmt->query.lastError().isValid() &&
			    iter->stmt->query.lastError().type() != QSqlError::NoError)
			{
				setDbStatusFromError(iter->stmt->db, iter->stmt->query.lastError());
				const QByteArray msg = iter->stmt->query.lastError().text().toUtf8();
				lua_pushlstring(L, msg.constData(), msg.size());
				lua_error(L);
				return 0;
			}
			return 0;
		}
		iter->stmt->columns = iter->stmt->query.record().count();

		if (iter->unpacked)
		{
			for (int i = 0; i < iter->stmt->columns; ++i)
				pushValue(L, iter->stmt->query.value(i));
			return iter->stmt->columns;
		}

		lua_newtable(L);
		if (iter->named)
		{
			const QSqlRecord record = iter->stmt->query.record();
			for (int i = 0; i < iter->stmt->columns; ++i)
			{
				setNamedValue(L, record.fieldName(i), iter->stmt->query.value(i));
			}
		}
		else
		{
			for (int i = 0; i < iter->stmt->columns; ++i)
			{
				pushValue(L, iter->stmt->query.value(i));
				lua_rawseti(L, -2, i + 1);
			}
		}
		return 1;
	}

	int dbDoRows(lua_State *L, const bool named, const bool unpacked)
	{
		LuaSqliteDb *db  = checkdb(L);
		const char  *sql = luaL_checkstring(L, 2);
		if (!isDbUsable(db))
		{
			setDbStatus(db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite database"));
			return luaL_error(L, "attempt to use closed sqlite database");
		}

		// Keep at most one active rows iterator per SQLite connection. The Qt SQLite
		// driver can behave unstably when multiple live query cursors overlap on the
		// same connection and one is abandoned by script code.
		if (!db->iterators.isEmpty())
		{
			QVector<LuaSqliteIter *> activeIterators;
			activeIterators.reserve(db->iterators.size());
			for (LuaSqliteIter *activeIter : db->iterators)
				activeIterators.push_back(activeIter);
			for (LuaSqliteIter *activeIter : activeIterators)
			{
				invalidateIterator(activeIter);
				releaseDbRef(activeIter->ownerState, activeIter->dbRef);
				activeIter->ownerState = nullptr;
			}
			db->iterators.clear();
		}

		auto *iter = static_cast<LuaSqliteIter *>(lua_newuserdata(L, sizeof(LuaSqliteIter)));
		new (iter) LuaSqliteIter;
		iter->db         = db;
		iter->named      = named;
		iter->unpacked   = unpacked;
		iter->ownerState = L;
		lua_pushvalue(L, 1);
		iter->dbRef = luaL_ref(L, LUA_REGISTRYINDEX);
		trackIterator(db, iter);
		iter->query = QSqlQuery(db->db);
		if (!iter->query.exec(QString::fromUtf8(sql)))
		{
			setDbStatusFromError(db, iter->query.lastError());
			const QByteArray msg = iter->query.lastError().text().toUtf8();
			invalidateIterator(iter);
			releaseDbRef(iter->ownerState, iter->dbRef);
			iter->ownerState = nullptr;
			lua_pop(L, 1);
			lua_pushlstring(L, msg.constData(), msg.size());
			lua_error(L);
			return 0;
		}
		iter->columns = iter->query.record().count();
		iter->active  = true;
		setDbStatus(db, SQLITE_OK, QString());

		luaL_getmetatable(L, "qmud.sqlite3.iter");
		lua_setmetatable(L, -2);

		lua_pushvalue(L, -1);
		lua_pushcclosure(L, iterNext, 1);
		lua_insert(L, -2);
		return 2;
	}

	int dbRows(lua_State *L)
	{
		return dbDoRows(L, false, false);
	}
	int dbNRows(lua_State *L)
	{
		return dbDoRows(L, true, false);
	}
	int dbURows(lua_State *L)
	{
		return dbDoRows(L, false, true);
	}

	int stmtDoRows(lua_State *L, const bool named, const bool unpacked)
	{
		LuaSqliteStmt *stmt = checkstmt(L);
		if (!isDbUsable(stmt->db) || !stmt->prepared)
		{
			setDbStatus(stmt->db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite statement"));
			return luaL_error(L, "attempt to use closed sqlite statement");
		}
		auto *iter = static_cast<LuaSqliteStmtIter *>(lua_newuserdata(L, sizeof(LuaSqliteStmtIter)));
		new (iter) LuaSqliteStmtIter;
		iter->stmt       = stmt;
		iter->named      = named;
		iter->unpacked   = unpacked;
		iter->ownerState = L;
		lua_pushvalue(L, 1);
		iter->stmtRef = luaL_ref(L, LUA_REGISTRYINDEX);

		luaL_getmetatable(L, "qmud.sqlite3.stmt_iter");
		lua_setmetatable(L, -2);

		lua_pushvalue(L, -1);
		lua_pushcclosure(L, stmtIterNext, 1);
		lua_insert(L, -2);
		return 2;
	}

	int stmtRows(lua_State *L)
	{
		return stmtDoRows(L, false, false);
	}
	int stmtNRows(lua_State *L)
	{
		return stmtDoRows(L, true, false);
	}
	int stmtURows(lua_State *L)
	{
		return stmtDoRows(L, false, true);
	}

	int dbExec(lua_State *L)
	{
		LuaSqliteDb *db  = checkdb(L);
		const char  *sql = luaL_checkstring(L, 2);
		if (!isDbUsable(db))
		{
			setDbStatus(db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite database"));
			return luaL_error(L, "attempt to use closed sqlite database");
		}

		QSqlQuery         query(db->db);
		const QString     sqlText    = QString::fromUtf8(sql);
		const QStringList statements = splitSqlStatements(sqlText);
		if (!lua_isnoneornil(L, 3))
		{
			luaL_checktype(L, 3, LUA_TFUNCTION);
			for (const QString &stmt : statements)
			{
				if (stmt.isEmpty())
					continue;
				if (!query.exec(stmt))
				{
					setDbStatusFromError(db, query.lastError());
					lua_pushnumber(L, db->lastErrCode);
					const QByteArray msg = db->lastErrMsg.toUtf8();
					lua_pushlstring(L, msg.constData(), msg.size());
					return 2;
				}
				while (query.next())
				{
					const QSqlRecord record  = query.record();
					const int        columns = record.count();
					lua_pushvalue(L, 3);
					if (lua_isnoneornil(L, 4))
						lua_pushnil(L);
					else
						lua_pushvalue(L, 4);
					lua_pushnumber(L, columns);

					lua_newtable(L);
					for (int i = 0; i < columns; ++i)
					{
						pushValue(L, query.value(i));
						lua_rawseti(L, -2, i + 1);
					}

					lua_newtable(L);
					for (int i = 0; i < columns; ++i)
					{
						const QByteArray key = record.fieldName(i).toUtf8();
						lua_pushlstring(L, key.constData(), key.size());
						lua_rawseti(L, -2, i + 1);
					}

					if (lua_pcall(L, 4, 1, 0) == 0)
					{
						if (lua_isnumber(L, -1) && lua_tonumber(L, -1) != 0)
						{
							lua_pop(L, 1);
							setDbStatus(db, SQLITE_ABORT, QStringLiteral("query aborted by callback"));
							lua_pushnumber(L, SQLITE_ABORT);
							return 1;
						}
					}
					lua_pop(L, 1);
				}
			}

			setDbStatus(db, SQLITE_OK, QString());
			lua_pushnumber(L, SQLITE_OK);
			return 1;
		}

		for (const QString &stmt : statements)
		{
			if (stmt.isEmpty())
				continue;
			if (!query.exec(stmt))
			{
				setDbStatusFromError(db, query.lastError());
				lua_pushnumber(L, db->lastErrCode);
				const QByteArray msg = db->lastErrMsg.toUtf8();
				lua_pushlstring(L, msg.constData(), msg.size());
				return 2;
			}
		}

		setDbStatus(db, SQLITE_OK, QString());
		lua_pushnumber(L, SQLITE_OK);
		return 1;
	}

	int dbPrepare(lua_State *L)
	{
		const int    topBefore = lua_gettop(L);
		LuaSqliteDb *db        = checkdb(L);
		const char  *sql       = luaL_checkstring(L, 2);
		if (!isDbUsable(db))
		{
			setDbStatus(db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite database"));
			lua_pushnil(L);
			lua_pushnumber(L, SQLITE_MISUSE);
			return luaReturnCount(L, topBefore);
		}

		auto *stmt = static_cast<LuaSqliteStmt *>(lua_newuserdata(L, sizeof(LuaSqliteStmt)));
		new (stmt) LuaSqliteStmt;
		stmt->db    = db;
		stmt->query = QSqlQuery(db->db);
		if (!stmt->query.prepare(QString::fromUtf8(sql)))
		{
			setDbStatusFromError(db, stmt->query.lastError());
			lua_pop(L, 1);
			lua_pushnil(L);
			lua_pushnumber(L, db->lastErrCode);
			return luaReturnCount(L, topBefore);
		}

		stmt->prepared   = true;
		stmt->executed   = false;
		stmt->hasRow     = false;
		stmt->columns    = 0;
		stmt->paramNames = collectParamNames(QString::fromUtf8(sql));
		stmt->ownerState = L;
		lua_pushvalue(L, 1);
		stmt->dbRef = luaL_ref(L, LUA_REGISTRYINDEX);
		trackStatement(db, stmt);
		setDbStatus(db, SQLITE_OK, QString());

		luaL_getmetatable(L, "qmud.sqlite3.stmt");
		lua_setmetatable(L, -2);

		lua_pushstring(L, "");
		return luaReturnCount(L, topBefore);
	}

	int dbChanges(lua_State *L)
	{
		const int    topBefore = lua_gettop(L);
		LuaSqliteDb *db        = checkdb(L);
		if (!isDbUsable(db))
		{
			setDbStatus(db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite database"));
			lua_pushnumber(L, 0);
			return luaReturnCount(L, topBefore);
		}
		QSqlQuery query(db->db);
		int       result = 0;
		if (query.exec(QStringLiteral("SELECT changes()")) && query.next())
		{
			result = query.value(0).toInt();
			setDbStatus(db, SQLITE_OK, QString());
		}
		else
		{
			setDbStatusFromError(db, query.lastError());
		}
		lua_pushnumber(L, result);
		return luaReturnCount(L, topBefore);
	}

	int dbTotalChanges(lua_State *L)
	{
		const int    topBefore = lua_gettop(L);
		LuaSqliteDb *db        = checkdb(L);
		if (!isDbUsable(db))
		{
			setDbStatus(db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite database"));
			lua_pushnumber(L, 0);
			return luaReturnCount(L, topBefore);
		}
		QSqlQuery query(db->db);
		int       result = 0;
		if (query.exec(QStringLiteral("SELECT total_changes()")) && query.next())
		{
			result = query.value(0).toInt();
			setDbStatus(db, SQLITE_OK, QString());
		}
		else
		{
			setDbStatusFromError(db, query.lastError());
		}
		lua_pushnumber(L, result);
		return luaReturnCount(L, topBefore);
	}

	int dbLastInsertRowId(lua_State *L)
	{
		const int    topBefore = lua_gettop(L);
		LuaSqliteDb *db        = checkdb(L);
		if (!isDbUsable(db))
		{
			setDbStatus(db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite database"));
			lua_pushnumber(L, 0);
			return luaReturnCount(L, topBefore);
		}
		QSqlQuery query(db->db);
		qlonglong result = 0;
		if (query.exec(QStringLiteral("SELECT last_insert_rowid()")) && query.next())
		{
			result = query.value(0).toLongLong();
			setDbStatus(db, SQLITE_OK, QString());
		}
		else
		{
			setDbStatusFromError(db, query.lastError());
		}
		lua_pushnumber(L, static_cast<lua_Number>(result));
		return luaReturnCount(L, topBefore);
	}

	int dbErrCode(lua_State *L)
	{
		const LuaSqliteDb *db = checkdb(L);
		lua_pushnumber(L, db->lastErrCode);
		return 1;
	}

	int dbErrMsg(lua_State *L)
	{
		const LuaSqliteDb *db    = checkdb(L);
		const QByteArray   bytes = db->lastErrMsg.toUtf8();
		lua_pushlstring(L, bytes.constData(), bytes.size());
		return 1;
	}

	int dbBusyTimeout(lua_State *L)
	{
		const int    topBefore = lua_gettop(L);
		LuaSqliteDb *db        = checkdb(L);
		if (!isDbUsable(db))
		{
			setDbStatus(db, SQLITE_MISUSE, QStringLiteral("attempt to use closed sqlite database"));
			lua_pushnumber(L, SQLITE_MISUSE);
			return luaReturnCount(L, topBefore);
		}
		const int ms = static_cast<int>(luaL_checkinteger(L, 2));
		if (QSqlQuery query(db->db); !query.exec(QStringLiteral("PRAGMA busy_timeout = %1").arg(ms)))
		{
			setDbStatusFromError(db, query.lastError());
			lua_pushnumber(L, db->lastErrCode);
			return luaReturnCount(L, topBefore);
		}
		setDbStatus(db, SQLITE_OK, QString());
		lua_pushnumber(L, SQLITE_OK);
		return luaReturnCount(L, topBefore);
	}

	int dbNotSupported(lua_State *L)
	{
		lua_pushnumber(L, SQLITE_ERROR);
		return 1;
	}

	int dbCloseVm(lua_State *)
	{
		return 0;
	}

	int luaSqliteVersion(lua_State *L)
	{
		const QByteArray bytes = sqliteDriverVersion().toUtf8();
		lua_pushlstring(L, bytes.constData(), bytes.size());
		return 1;
	}

	int luaSqliteComplete(lua_State *L)
	{
		const char *sql = luaL_checkstring(L, 1);
		lua_pushboolean(L, isSqlComplete(QString::fromUtf8(sql)));
		return 1;
	}

	int luaSqliteDoOpen(lua_State *L, const QString &filename)
	{
		static int connectionCounter = 0;
		auto      *db                = static_cast<LuaSqliteDb *>(lua_newuserdata(L, sizeof(LuaSqliteDb)));
		new (db) LuaSqliteDb;
		db->connectionName = QStringLiteral("lua_sqlite_%1").arg(++connectionCounter);
		db->db             = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), db->connectionName);
		db->db.setDatabaseName(filename);
		if (!db->db.open())
		{
			const int        rc  = mapSqlErrorToSqlite(db->db.lastError());
			const QByteArray msg = db->db.lastError().text().toUtf8();
			db->db               = QSqlDatabase();
			QSqlDatabase::removeDatabase(db->connectionName);
			lua_pop(L, 1);
			lua_pushnil(L);
			lua_pushnumber(L, rc);
			lua_pushlstring(L, msg.constData(), msg.size());
			return 3;
		}

		if (filename != QStringLiteral(":memory:"))
		{
			QString pragmaError;
			if (const int rc = applySqliteWalAndNormalSynchronous(db->db, pragmaError); rc != SQLITE_OK)
			{
				const QByteArray msg = pragmaError.toUtf8();
				db->db.close();
				db->db = QSqlDatabase();
				QSqlDatabase::removeDatabase(db->connectionName);
				lua_pop(L, 1);
				lua_pushnil(L);
				lua_pushnumber(L, rc);
				lua_pushlstring(L, msg.constData(), msg.size());
				return 3;
			}
		}

		db->open        = true;
		db->lastErrCode = SQLITE_OK;
		db->lastErrMsg.clear();
		luaL_getmetatable(L, "qmud.sqlite3.db");
		lua_setmetatable(L, -2);
		return 1;
	}

	int luaSqliteOpen(lua_State *L)
	{
		const char *filename = luaL_checkstring(L, 1);
		return luaSqliteDoOpen(L, QString::fromUtf8(filename));
	}

	int luaSqliteOpenMemory(lua_State *L)
	{
		return luaSqliteDoOpen(L, QStringLiteral(":memory:"));
	}

} // namespace

extern "C" int luaopen_lsqlite3(lua_State *L)
{
	static const luaL_Reg kDbMethods[] = {
	    {"isopen",            dbIsOpen         },
	    {"last_insert_rowid", dbLastInsertRowId},
	    {"changes",           dbChanges        },
	    {"total_changes",     dbTotalChanges   },
	    {"errcode",           dbErrCode        },
	    {"error_code",        dbErrCode        },
	    {"errmsg",            dbErrMsg         },
	    {"error_message",     dbErrMsg         },
	    {"interrupt",         dbNotSupported   },
	    {"create_function",   dbNotSupported   },
	    {"create_aggregate",  dbNotSupported   },
	    {"create_collation",  dbNotSupported   },
	    {"trace",             dbNotSupported   },
	    {"progress_handler",  dbNotSupported   },
	    {"busy_timeout",      dbBusyTimeout    },
	    {"busy_handler",      dbNotSupported   },
	    {"prepare",           dbPrepare        },
	    {"rows",              dbRows           },
	    {"urows",             dbURows          },
	    {"nrows",             dbNRows          },
	    {"exec",              dbExec           },
	    {"execute",           dbExec           },
	    {"close",             dbClose          },
	    {"close_vm",          dbCloseVm        },
	    {"__tostring",        dbToString       },
	    {"__gc",              dbClose          },
	    {nullptr,             nullptr          }
    };

	static const luaL_Reg kStmtMethods[] = {
	    {"isopen",               stmtIsOpen            },
	    {"step",	             stmtStep              },
	    {"reset",                stmtReset             },
	    {"finalize",             stmtFinalize          },
	    {"columns",              stmtColumns           },
	    {"bind",	             stmtBind              },
	    {"bind_values",          stmtBindValues        },
	    {"bind_names",           stmtBindNames         },
	    {"bind_blob",            stmtBindBlob          },
	    {"bind_parameter_count", stmtBindParameterCount},
	    {"bind_parameter_name",  stmtBindParameterName },
	    {"get_value",            stmtGetValue          },
	    {"get_values",           stmtGetValues         },
	    {"get_name",             stmtGetName           },
	    {"get_names",            stmtGetNames          },
	    {"get_type",             stmtGetType           },
	    {"get_types",            stmtGetTypes          },
	    {"get_uvalues",          stmtGetValues         },
	    {"get_unames",           stmtGetNames          },
	    {"get_utypes",           stmtGetTypes          },
	    {"get_named_values",     stmtGetNamedValues    },
	    {"get_named_types",      stmtGetNamedTypes     },
	    {"rows",	             stmtRows              },
	    {"urows",                stmtURows             },
	    {"nrows",                stmtNRows             },
	    {"idata",                stmtGetValues         },
	    {"inames",               stmtGetNames          },
	    {"itypes",               stmtGetTypes          },
	    {"__tostring",           stmtToString          },
	    {"__gc",	             stmtFinalize          },
	    {nullptr,                nullptr               }
    };

	static constexpr luaL_Reg kIterMethods[] = {
	    {"__gc",  iterGc },
        {nullptr, nullptr}
    };

	static constexpr luaL_Reg kStmtIterMethods[] = {
	    {"__gc",  stmtIterGc},
        {nullptr, nullptr   }
    };

	luaL_newmetatable(L, "qmud.sqlite3.db");
	luaL_setfuncs(L, kDbMethods, 0);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);

	luaL_newmetatable(L, "qmud.sqlite3.stmt");
	luaL_setfuncs(L, kStmtMethods, 0);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);

	luaL_newmetatable(L, "qmud.sqlite3.iter");
	luaL_setfuncs(L, kIterMethods, 0);
	lua_pop(L, 1);

	luaL_newmetatable(L, "qmud.sqlite3.stmt_iter");
	luaL_setfuncs(L, kStmtIterMethods, 0);
	lua_pop(L, 1);

	lua_newtable(L);
	lua_pushcfunction(L, luaSqliteOpen);
	lua_setfield(L, -2, "open");
	lua_pushcfunction(L, luaSqliteOpenMemory);
	lua_setfield(L, -2, "open_memory");
	lua_pushcfunction(L, luaSqliteVersion);
	lua_setfield(L, -2, "version");
	lua_pushcfunction(L, luaSqliteComplete);
	lua_setfield(L, -2, "complete");

	struct
	{
			const char *name;
			int         value;
	} const kConstants[] = {
	    {"OK",         SQLITE_OK        },
	    {"ERROR",      SQLITE_ERROR     },
	    {"INTERNAL",   SQLITE_INTERNAL  },
	    {"PERM",       SQLITE_PERM      },
	    {"ABORT",      SQLITE_ABORT     },
	    {"BUSY",       SQLITE_BUSY      },
	    {"LOCKED",     SQLITE_LOCKED    },
	    {"NOMEM",      SQLITE_NOMEM     },
	    {"READONLY",   SQLITE_READONLY  },
	    {"INTERRUPT",  SQLITE_INTERRUPT },
	    {"IOERR",      SQLITE_IOERR     },
	    {"CORRUPT",    SQLITE_CORRUPT   },
	    {"NOTFOUND",   SQLITE_NOTFOUND  },
	    {"FULL",       SQLITE_FULL      },
	    {"CANTOPEN",   SQLITE_CANTOPEN  },
	    {"PROTOCOL",   SQLITE_PROTOCOL  },
	    {"EMPTY",      SQLITE_EMPTY     },
	    {"SCHEMA",     SQLITE_SCHEMA    },
	    {"TOOBIG",     SQLITE_TOOBIG    },
	    {"CONSTRAINT", SQLITE_CONSTRAINT},
	    {"MISMATCH",   SQLITE_MISMATCH  },
	    {"MISUSE",     SQLITE_MISUSE    },
	    {"NOLFS",      SQLITE_NOLFS     },
	    {"FORMAT",     SQLITE_FORMAT    },
	    {"RANGE",      SQLITE_RANGE     },
	    {"NOTADB",     SQLITE_NOTADB    },
	    {"ROW",        SQLITE_ROW       },
	    {"DONE",       SQLITE_DONE      },
	    {"INTEGER",    SQLITE_INTEGER   },
	    {"FLOAT",      SQLITE_FLOAT     },
	    {"TEXT",       SQLITE_TEXT      },
	    {"BLOB",       SQLITE_BLOB      },
	    {"NULL",       SQLITE_NULL      },
	    {nullptr,      0                }
    };
	for (int i = 0; kConstants[i].name; ++i)
	{
		lua_pushnumber(L, kConstants[i].value);
		lua_setfield(L, -2, kConstants[i].name);
	}

	// Preserve behavior: expose module globals for scripts that use sqlite3 directly.
	lua_pushvalue(L, -1);
	lua_setglobal(L, "sqlite3");
	lua_pushvalue(L, -1);
	lua_setglobal(L, "lsqlite3");

	return 1;
}
