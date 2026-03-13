/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: SqliteCompat.h
 * Role: SQLite compatibility declarations that smooth platform/version differences for database-backed features.
 */

#ifndef QMUD_SQLITE_COMPAT_H
#define QMUD_SQLITE_COMPAT_H

struct sqlite3;
struct sqlite3_stmt;

#ifndef SQLITE_OK
inline constexpr int SQLITE_OK = 0;
#endif
#ifndef SQLITE_ERROR
inline constexpr int SQLITE_ERROR = 1;
#endif
#ifndef SQLITE_INTERNAL
inline constexpr int SQLITE_INTERNAL = 2;
#endif
#ifndef SQLITE_PERM
inline constexpr int SQLITE_PERM = 3;
#endif
#ifndef SQLITE_ABORT
inline constexpr int SQLITE_ABORT = 4;
#endif
#ifndef SQLITE_BUSY
inline constexpr int SQLITE_BUSY = 5;
#endif
#ifndef SQLITE_LOCKED
inline constexpr int SQLITE_LOCKED = 6;
#endif
#ifndef SQLITE_NOMEM
inline constexpr int SQLITE_NOMEM = 7;
#endif
#ifndef SQLITE_READONLY
inline constexpr int SQLITE_READONLY = 8;
#endif
#ifndef SQLITE_INTERRUPT
inline constexpr int SQLITE_INTERRUPT = 9;
#endif
#ifndef SQLITE_IOERR
inline constexpr int SQLITE_IOERR = 10;
#endif
#ifndef SQLITE_CORRUPT
inline constexpr int SQLITE_CORRUPT = 11;
#endif
#ifndef SQLITE_NOTFOUND
inline constexpr int SQLITE_NOTFOUND = 12;
#endif
#ifndef SQLITE_FULL
inline constexpr int SQLITE_FULL = 13;
#endif
#ifndef SQLITE_CANTOPEN
inline constexpr int SQLITE_CANTOPEN = 14;
#endif
#ifndef SQLITE_PROTOCOL
inline constexpr int SQLITE_PROTOCOL = 15;
#endif
#ifndef SQLITE_EMPTY
inline constexpr int SQLITE_EMPTY = 16;
#endif
#ifndef SQLITE_SCHEMA
inline constexpr int SQLITE_SCHEMA = 17;
#endif
#ifndef SQLITE_TOOBIG
inline constexpr int SQLITE_TOOBIG = 18;
#endif
#ifndef SQLITE_CONSTRAINT
inline constexpr int SQLITE_CONSTRAINT = 19;
#endif
#ifndef SQLITE_MISMATCH
inline constexpr int SQLITE_MISMATCH = 20;
#endif
#ifndef SQLITE_MISUSE
inline constexpr int SQLITE_MISUSE = 21;
#endif
#ifndef SQLITE_NOLFS
inline constexpr int SQLITE_NOLFS = 22;
#endif
#ifndef SQLITE_FORMAT
inline constexpr int SQLITE_FORMAT = 24;
#endif
#ifndef SQLITE_RANGE
inline constexpr int SQLITE_RANGE = 25;
#endif
#ifndef SQLITE_NOTADB
inline constexpr int SQLITE_NOTADB = 26;
#endif

#ifndef SQLITE_ROW
inline constexpr int SQLITE_ROW = 100;
#endif
#ifndef SQLITE_DONE
inline constexpr int SQLITE_DONE = 101;
#endif

#ifndef SQLITE_INTEGER
inline constexpr int SQLITE_INTEGER = 1;
#endif
#ifndef SQLITE_FLOAT
inline constexpr int SQLITE_FLOAT = 2;
#endif
#ifndef SQLITE_TEXT
inline constexpr int SQLITE_TEXT = 3;
#endif
#ifndef SQLITE_BLOB
inline constexpr int SQLITE_BLOB = 4;
#endif
#ifndef SQLITE_NULL
inline constexpr int SQLITE_NULL = 5;
#endif

#ifndef SQLITE_OPEN_READONLY
inline constexpr int SQLITE_OPEN_READONLY = 0x00000001;
#endif
#ifndef SQLITE_OPEN_READWRITE
inline constexpr int SQLITE_OPEN_READWRITE = 0x00000002;
#endif
#ifndef SQLITE_OPEN_CREATE
inline constexpr int SQLITE_OPEN_CREATE = 0x00000004;
#endif
#ifndef SQLITE_OPEN_URI
inline constexpr int SQLITE_OPEN_URI = 0x00000040;
#endif
#ifndef SQLITE_OPEN_MEMORY
inline constexpr int SQLITE_OPEN_MEMORY = 0x00000080;
#endif
#ifndef SQLITE_OPEN_NOMUTEX
inline constexpr int SQLITE_OPEN_NOMUTEX = 0x00008000;
#endif
#ifndef SQLITE_OPEN_FULLMUTEX
inline constexpr int SQLITE_OPEN_FULLMUTEX = 0x00010000;
#endif
#ifndef SQLITE_OPEN_SHAREDCACHE
inline constexpr int SQLITE_OPEN_SHAREDCACHE = 0x00020000;
#endif
#ifndef SQLITE_OPEN_PRIVATECACHE
inline constexpr int SQLITE_OPEN_PRIVATECACHE = 0x00040000;
#endif

#endif // QMUD_SQLITE_COMPAT_H
