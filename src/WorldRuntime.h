/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldRuntime.h
 * Role: Core per-world runtime interfaces covering network I/O, output processing, triggers, aliases, timers, and
 * script integration.
 */

#ifndef QMUD_WORLDRUNTIME_H
#define QMUD_WORLDRUNTIME_H

#include "MiniWindow.h"
#include "SqliteCompat.h"
#include "TelnetProcessor.h"
#include <QByteArray>
#include <QColor>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QSharedPointer>
#include <QString>
#include <QStringDecoder>
#include <QVariant>
#include <QVector>
#include <QtSql/QSqlQuery>
#include <functional>

class WorldDocument;
class WorldSocketService;
class QFileSystemWatcher;
class LuaCallbackEngine;
class WorldCommandProcessor;
class WorldView;
class QUdpSocket;
class QSoundEffect;
class QTemporaryFile;
class QTcpServer;
#ifdef QMUD_ENABLE_LUA_SCRIPTING
struct lua_State;
#endif
/**
 * @brief Per-world runtime coordinator for connection, automation, and UI-facing state.
 *
 * Manages world options, telnet/MXP processing, plugin/Lua callbacks, logging, and
 * miniwindow/chat/database utility surfaces consumed by the rest of the client.
 */
class WorldRuntime : public QObject
{
		Q_OBJECT
		friend class WorldView;
		class ChatConnection;
		friend class ChatConnection;

	public:
		static constexpr int kAcceleratorFirstCommand = 12000;
		static constexpr int kAcceleratorCount        = 1000;
		static constexpr int kSameColour              = 65535;
		static constexpr int kMaxSoundBuffers         = 10;

		enum ActionSource : unsigned short
		{
			eDontChangeAction    = 999,
			eUnknownActionSource = 0,
			eUserTyping          = 1,
			eUserMacro           = 2,
			eUserKeypad          = 3,
			eUserAccelerator     = 4,
			eUserMenuAction      = 5,
			eTriggerFired        = 6,
			eTimerFired          = 7,
			eInputFromServer     = 8,
			eWorldAction         = 9,
			eLuaSandbox          = 10,
			eHotspotCallback     = 11
		};

		/**
		 * @brief Creates runtime container for a world session.
		 * @param parent Optional Qt parent object.
		 */
		explicit WorldRuntime(QObject *parent = nullptr);
		/**
		 * @brief Destroys runtime and releases world-scoped resources.
		 */
		~WorldRuntime() override;

		/**
		 * @brief Initializes runtime state from loaded world document.
		 * @param doc Loaded world document used as source state.
		 */
		void                                        applyFromDocument(const WorldDocument &doc);

		/**
		 * @brief World metadata and script-time accounting accessors.
		 */
		/**
		 * @brief Returns single-line world attribute map.
		 * @return Immutable map of single-line world attributes.
		 */
		[[nodiscard]] const QMap<QString, QString> &worldAttributes() const;
		/**
		 * @brief Returns multiline world attribute map.
		 * @return Immutable map of multiline world attributes.
		 */
		[[nodiscard]] const QMap<QString, QString> &worldMultilineAttributes() const;
		/**
		 * @brief Sets single-line world attribute value by key.
		 * @param key Attribute name.
		 * @param value Attribute value.
		 */
		void                    setWorldAttribute(const QString &key, const QString &value);
		/**
		 * @brief Sets multiline world attribute value by key.
		 * @param key Attribute name.
		 * @param value Attribute value.
		 */
		void                    setWorldMultilineAttribute(const QString &key, const QString &value);
		/**
		 * @brief Returns parsed world file format version.
		 * @return Parsed world file version number.
		 */
		[[nodiscard]] int       worldFileVersion() const;
		/**
		 * @brief Returns persisted QMud version string from file.
		 * @return Persisted client version string from world file.
		 */
		[[nodiscard]] QString   qmudVersion() const;
		/**
		 * @brief Returns persisted save timestamp from world file.
		 * @return World file save timestamp.
		 */
		[[nodiscard]] QDateTime dateSaved() const;
		/**
		 * @brief Adds script execution time sample in nanoseconds.
		 * @param nanos Duration sample in nanoseconds.
		 */
		void                    addScriptTime(qint64 nanos);
		/**
		 * @brief Returns cumulative script time in seconds.
		 * @return Total script time in seconds.
		 */
		[[nodiscard]] double    scriptTimeSeconds() const;

		/**
		 * @brief Live trigger state including runtime counters and last-match metadata.
		 */
		struct Trigger
		{
				QMap<QString, QString> attributes;
				QMap<QString, QString> children;
				bool                   included{false};
				int                    matched{0};
				int                    invocationCount{0};
				int                    matchAttempts{0};
				QString                lastMatchTarget;
				QDateTime              lastMatched;
				bool                   executingScript{false};
		};
		/**
		 * @brief Live alias state including runtime counters and last-match metadata.
		 */
		struct Alias
		{
				QMap<QString, QString> attributes;
				QMap<QString, QString> children;
				bool                   included{false};
				int                    matched{0};
				int                    invocationCount{0};
				int                    matchAttempts{0};
				QString                lastMatchTarget;
				QDateTime              lastMatched;
				bool                   executingScript{false};
		};
		/**
		 * @brief Live timer state including scheduling and invocation metadata.
		 */
		struct Timer
		{
				QMap<QString, QString> attributes;
				QMap<QString, QString> children;
				bool                   included{false};
				QDateTime              lastFired;
				QDateTime              nextFireTime;
				int                    firedCount{0};
				int                    invocationCount{0};
				bool                   executingScript{false};
		};
		/**
		 * @brief Macro definition payload loaded into runtime.
		 */
		struct Macro
		{
				QMap<QString, QString> attributes;
				QMap<QString, QString> children;
		};
		/**
		 * @brief Variable definition payload loaded into runtime.
		 */
		struct Variable
		{
				QMap<QString, QString> attributes;
				QString                content;
		};
		/**
		 * @brief Color-group customization payload loaded into runtime.
		 */
		struct Colour
		{
				QString                group;
				QMap<QString, QString> attributes;
		};
		/**
		 * @brief Keypad mapping payload loaded into runtime.
		 */
		struct Keypad
		{
				QMap<QString, QString> attributes;
				QString                content;
		};
		/**
		 * @brief Printing-style customization payload loaded into runtime.
		 */
		struct PrintingStyle
		{
				QString                group;
				QMap<QString, QString> attributes;
		};
		/**
		 * @brief Runtime plugin state including script VM, rule lists, and metadata.
		 */
		struct Plugin
		{
				QMap<QString, QString>                attributes;
				QString                               description;
				QString                               script;
				QString                               source;
				QString                               directory;
				QString                               callingPluginId;
				bool                                  enabled{true};
				bool                                  global{false};
				bool                                  saveState{false};
				bool                                  savingStateNow{false};
				bool                                  installPending{false};
				int                                   sequence{5000};
				double                                version{0.0};
				double                                requiredVersion{0.0};
				QDateTime                             dateWritten;
				QDateTime                             dateModified;
				QDateTime                             dateInstalled;
				QSharedPointer<LuaCallbackEngine>     lua;
				QList<Trigger>                        triggers;
				QList<Alias>                          aliases;
				QList<Timer>                          timers;
				QMap<QString, QStringList>            triggerWildcards;
				QMap<QString, QMap<QString, QString>> triggerNamedWildcards;
				QMap<QString, QStringList>            aliasWildcards;
				QMap<QString, QMap<QString, QString>> aliasNamedWildcards;
				QMap<QString, QString>                variables;
		};
		/**
		 * @brief Generic key/value array entry used by scripting APIs.
		 */
		struct ArrayEntry
		{
				QMap<QString, QString> values;
		};
		/**
		 * @brief Runtime database handle and prepared-statement execution state.
		 */
		struct DatabaseEntry
		{
				QString                   diskName;
				QString                   connectionName;
				QSqlDatabase              db;
				QSharedPointer<QSqlQuery> stmt;
				bool                      stmtPrepared{false};
				bool                      stmtExecuted{false};
				bool                      validRow{false};
				int                       columns{0};
				int                       lastError{SQLITE_OK};
				QString                   lastErrorMessage;
		};
		/**
		 * @brief Include metadata loaded from world/plugin configuration.
		 */
		struct Include
		{
				QMap<QString, QString> attributes;
		};
		/**
		 * @brief Script content block loaded from world/plugin configuration.
		 */
		struct Script
		{
				QString content;
		};
		enum LineFlag
		{
			// Mirrors legacy CLine flag bits where applicable, with an extra output marker bit.
			LineNote           = 0x01, // COMMENT
			LineInput          = 0x02, // USER_INPUT
			LineLog            = 0x04, // LOG_LINE
			LineBookmark       = 0x08, // BOOKMARK
			LineHorizontalRule = 0x10, // HORIZ_RULE
			LineOutput         = 0x20  // Qt-only marker for server output flow control
		};
		enum ActionType
		{
			ActionNone      = 0,
			ActionSend      = 1,
			ActionHyperlink = 2,
			ActionPrompt    = 3
		};
		enum ConnectPhase
		{
			eConnectNotConnected           = 0,
			eConnectMudNameLookup          = 1,
			eConnectProxyNameLookup        = 2,
			eConnectConnectingToMud        = 3,
			eConnectConnectingToProxy      = 4,
			eConnectAwaitingProxyResponse1 = 5,
			eConnectAwaitingProxyResponse2 = 6,
			eConnectAwaitingProxyResponse3 = 7,
			eConnectConnectedToMud         = 8,
			eConnectDisconnecting          = 9
		};
		/**
		 * @brief Command mapping entry used by accelerator command routing.
		 */
		struct AcceleratorEntry
		{
				QString text;
				int     sendTo{0};
				QString pluginId;
		};
		enum StopTriggerEvaluation
		{
			KeepEvaluating      = 0,
			StopCurrentSequence = 1,
			StopAllSequences    = 2
		};
		/**
		 * @brief Styled segment metadata for rendered output text.
		 */
		struct StyleSpan
		{
				int     length{0};
				QColor  fore;
				QColor  back;
				bool    bold{false};
				bool    underline{false};
				bool    italic{false};
				bool    blink{false};
				bool    strike{false};
				bool    inverse{false};
				bool    changed{false};
				int     actionType{ActionNone};
				QString action;
				QString hint;
				QString variable;
				bool    startTag{false};
		};
		/**
		 * @brief Incremental ANSI rendering state carried across parsed text chunks.
		 */
		struct AnsiRenderState
		{
				bool    bold{false};
				bool    underline{false};
				bool    italic{false};
				bool    blink{false};
				bool    inverse{false};
				bool    strike{false};
				QString fore;
				QString back;
				int     actionType{ActionNone};
				QString action;
		};
		/**
		 * @brief Incremental MXP style state carried across packet boundaries.
		 */
		struct MxpStyleState
		{
				bool    bold{false};
				bool    underline{false};
				bool    italic{false};
				bool    blink{false};
				bool    strike{false};
				bool    monospace{false};
				bool    inverse{false};
				QString fore;
				QString back;
				int     actionType{ActionNone};
				QString action;
				QString hint;
				QString variable;
				bool    startTag{false};
		};
		/**
		 * @brief MXP style stack frame used while tags open and close.
		 */
		struct MxpStyleFrame
		{
				QByteArray    tag;
				MxpStyleState state;
		};
		/**
		 * @brief One output buffer line with text, style spans, and timing metadata.
		 */
		struct LineEntry
		{
				QString            text;
				int                flags{0};
				bool               hardReturn{true};
				QVector<StyleSpan> spans;
				QDateTime          time;
				qint64             lineNumber{0};
				double             ticks{0.0};
				double             elapsed{0.0};
		};
		/**
		 * @brief Rectangle/border/fill settings used by text-rectangle rendering APIs.
		 */
		struct TextRectangleSettings
		{
				int left{0};
				int top{0};
				int right{0};
				int bottom{0};
				int borderOffset{0};
				int borderColour{0};
				int borderWidth{0};
				int outsideFillColour{0};
				int outsideFillStyle{0};
		};
		/**
		 * @brief UDP listener registration owned by runtime/plugin callback plumbing.
		 */
		struct UdpListener
		{
				QUdpSocket *socket{nullptr};
				QString     pluginId;
				QString     script;
				QString     bindAddress;
		};
		/**
		 * @brief Managed audio playback slot for queued/looping sound commands.
		 */
		struct SoundBuffer
		{
				QSoundEffect   *effect{nullptr};
				QTemporaryFile *tempFile{nullptr};
				bool            looping{false};
				double          volume{1.0};
				double          pan{0.0};
		};

		/**
		 * @brief Rule/list collections and mutable access helpers.
		 */
		/**
		 * @brief Returns trigger list.
		 * @return Immutable trigger list.
		 */
		[[nodiscard]] const QList<Trigger>  &triggers() const;
		/**
		 * @brief Returns mutable trigger list.
		 * @return Mutable trigger list.
		 */
		QList<Trigger>                      &triggersMutable();
		/**
		 * @brief Replaces trigger list.
		 * @param triggers New trigger list.
		 */
		void                                 setTriggers(const QList<Trigger> &triggers);
		/**
		 * @brief Marks trigger collection as modified.
		 */
		void                                 markTriggersChanged();
		/**
		 * @brief Returns alias list.
		 * @return Immutable alias list.
		 */
		[[nodiscard]] const QList<Alias>    &aliases() const;
		/**
		 * @brief Returns mutable alias list.
		 * @return Mutable alias list.
		 */
		QList<Alias>                        &aliasesMutable();
		/**
		 * @brief Replaces alias list.
		 * @param aliases New alias list.
		 */
		void                                 setAliases(const QList<Alias> &aliases);
		/**
		 * @brief Marks alias collection as modified.
		 */
		void                                 markAliasesChanged();
		/**
		 * @brief Returns timer list.
		 * @return Immutable timer list.
		 */
		[[nodiscard]] const QList<Timer>    &timers() const;
		/**
		 * @brief Returns mutable timer list.
		 * @return Mutable timer list.
		 */
		QList<Timer>                        &timersMutable();
		/**
		 * @brief Replaces timer list.
		 * @param timers New timer list.
		 */
		void                                 setTimers(const QList<Timer> &timers);
		/**
		 * @brief Marks timer collection as modified.
		 */
		void                                 markTimersChanged();
		/**
		 * @brief Returns macro list.
		 * @return Immutable macro list.
		 */
		[[nodiscard]] const QList<Macro>    &macros() const;
		/**
		 * @brief Replaces macro list.
		 * @param macros New macro list.
		 */
		void                                 setMacros(const QList<Macro> &macros);
		/**
		 * @brief Returns variable list.
		 * @return Immutable variable list.
		 */
		[[nodiscard]] const QList<Variable> &variables() const;
		/**
		 * @brief Finds variable by name.
		 * @param name Variable name.
		 * @param value Output value when found.
		 * @return `true` when variable exists.
		 */
		bool                                 findVariable(const QString &name, QString &value) const;
		/**
		 * @brief Sets or creates variable value.
		 * @param name Variable name.
		 * @param value Variable value.
		 */
		void                                 setVariable(const QString &name, const QString &value);
		/**
		 * @brief Replaces variable list.
		 * @param variables New variable list.
		 */
		void                                 setVariables(const QList<Variable> &variables);
		/**
		 * @brief Lua-style associative array APIs.
		 */
		/**
		 * @brief Creates a named associative array.
		 * @param name Array name.
		 * @return Error/status code.
		 */
		int                                  arrayCreate(const QString &name);
		/**
		 * @brief Deletes named associative array.
		 * @param name Array name.
		 * @return Error/status code.
		 */
		int                                  arrayDelete(const QString &name);
		/**
		 * @brief Clears all key/value pairs in array.
		 * @param name Array name.
		 * @return Error/status code.
		 */
		int                                  arrayClear(const QString &name);
		/**
		 * @brief Returns true when named array exists.
		 * @param name Array name.
		 * @return `true` when the array exists.
		 */
		[[nodiscard]] bool                   arrayExists(const QString &name) const;
		/**
		 * @brief Returns number of named arrays.
		 * @return Total array count.
		 */
		[[nodiscard]] int                    arrayCount() const;
		/**
		 * @brief Returns key/value count for named array.
		 * @param name Array name.
		 * @return Number of entries in the array.
		 */
		[[nodiscard]] int                    arraySize(const QString &name) const;
		/**
		 * @brief Returns true when key exists in named array.
		 * @param name Array name.
		 * @param key Entry key.
		 * @return `true` when the key exists.
		 */
		[[nodiscard]] bool                   arrayKeyExists(const QString &name, const QString &key) const;
		/**
		 * @brief Reads array value by key.
		 * @param name Array name.
		 * @param key Entry key.
		 * @param value Output value when found.
		 * @return `true` when key lookup succeeds.
		 */
		bool                      arrayGet(const QString &name, const QString &key, QString &value) const;
		/**
		 * @brief Sets array key/value pair.
		 * @param name Array name.
		 * @param key Entry key.
		 * @param value Entry value.
		 * @return Error/status code.
		 */
		int                       arraySet(const QString &name, const QString &key, const QString &value);
		/**
		 * @brief Removes single key from array.
		 * @param name Array name.
		 * @param key Entry key.
		 * @return Error/status code.
		 */
		int                       arrayDeleteKey(const QString &name, const QString &key);
		/**
		 * @brief Returns first key in sorted order.
		 * @param name Array name.
		 * @param key Output key.
		 * @return `true` when at least one key exists.
		 */
		bool                      arrayFirstKey(const QString &name, QString &key) const;
		/**
		 * @brief Returns last key in sorted order.
		 * @param name Array name.
		 * @param key Output key.
		 * @return `true` when at least one key exists.
		 */
		bool                      arrayLastKey(const QString &name, QString &key) const;
		/**
		 * @brief Lists all associative array names.
		 * @return Array-name list.
		 */
		[[nodiscard]] QStringList arrayListAll() const;
		/**
		 * @brief Lists all keys for a named associative array.
		 * @param name Array name.
		 * @return Key list.
		 */
		[[nodiscard]] QStringList arrayListKeys(const QString &name) const;
		/**
		 * @brief Lists all values for a named associative array.
		 * @param name Array name.
		 * @return Value list.
		 */
		[[nodiscard]] QStringList arrayListValues(const QString &name) const;
		/**
		 * @brief Embedded SQLite wrapper APIs.
		 */
		/**
		 * @brief Opens named SQLite database connection.
		 * @param name Logical database handle name.
		 * @param filename Database file path.
		 * @param flags Open flags.
		 * @return SQLite-style status code.
		 */
		int                       databaseOpen(const QString &name, const QString &filename, int flags);
		/**
		 * @brief Closes named database connection.
		 * @param name Logical database handle name.
		 * @return SQLite-style status code.
		 */
		int                       databaseClose(const QString &name);
		/**
		 * @brief Prepares SQL statement for named database.
		 * @param name Logical database handle name.
		 * @param sql SQL text to prepare.
		 * @return SQLite-style status code.
		 */
		int                       databasePrepare(const QString &name, const QString &sql);
		/**
		 * @brief Finalizes prepared statement.
		 * @param name Logical database handle name.
		 * @return SQLite-style status code.
		 */
		int                       databaseFinalize(const QString &name);
		/**
		 * @brief Resets prepared statement cursor.
		 * @param name Logical database handle name.
		 * @return SQLite-style status code.
		 */
		int                       databaseReset(const QString &name);
		/**
		 * @brief Returns column count for prepared statement result.
		 * @param name Logical database handle name.
		 * @return Column count.
		 */
		[[nodiscard]] int         databaseColumns(const QString &name) const;
		/**
		 * @brief Steps prepared statement to next row.
		 * @param name Logical database handle name.
		 * @return SQLite-style step status code.
		 */
		int                       databaseStep(const QString &name);
		/**
		 * @brief Returns last error message for named database.
		 * @param name Logical database handle name.
		 * @return Last error message string.
		 */
		[[nodiscard]] QString     databaseError(const QString &name) const;
		/**
		 * @brief Returns result-column name by zero-based index.
		 * @param name Logical database handle name.
		 * @param column Zero-based column index.
		 * @return Column name.
		 */
		[[nodiscard]] QString     databaseColumnName(const QString &name, int column) const;
		/**
		 * @brief Returns result-column text value.
		 * @param name Logical database handle name.
		 * @param column Zero-based column index.
		 * @param ok Optional output success flag.
		 * @return Text value for the column.
		 */
		QString                   databaseColumnText(const QString &name, int column, bool *ok) const;
		/**
		 * @brief Returns result-column value as QVariant.
		 * @param name Logical database handle name.
		 * @param column Zero-based column index.
		 * @param value Output variant value.
		 * @return `true` when extraction succeeds.
		 */
		bool                      databaseColumnValue(const QString &name, int column, QVariant &value) const;
		/**
		 * @brief Returns SQLite column type code for result column.
		 * @param name Logical database handle name.
		 * @param column Zero-based column index.
		 * @return SQLite column type code.
		 */
		[[nodiscard]] int         databaseColumnType(const QString &name, int column) const;
		/**
		 * @brief Returns total row changes for database connection.
		 * @param name Logical database handle name.
		 * @return Total change count.
		 */
		[[nodiscard]] int         databaseTotalChanges(const QString &name) const;
		/**
		 * @brief Returns row changes from last statement execution.
		 * @param name Logical database handle name.
		 * @return Last statement change count.
		 */
		[[nodiscard]] int         databaseChanges(const QString &name) const;
		/**
		 * @brief Returns last inserted rowid as string.
		 * @param name Logical database handle name.
		 * @return Last insert rowid as text.
		 */
		[[nodiscard]] QString     databaseLastInsertRowid(const QString &name) const;
		/**
		 * @brief Lists open named database handles.
		 * @return List of open database names.
		 */
		[[nodiscard]] QStringList databaseList() const;
		/**
		 * @brief Returns database info value by info selector code.
		 * @param name Logical database handle name.
		 * @param infoType Info selector code.
		 * @return Requested info value.
		 */
		[[nodiscard]] QVariant    databaseInfo(const QString &name, int infoType) const;
		/**
		 * @brief Executes SQL text directly on named database.
		 * @param name Logical database handle name.
		 * @param sql SQL text to execute.
		 * @return SQLite-style status code.
		 */
		int                       databaseExec(const QString &name, const QString &sql);
		/**
		 * @brief Returns all column names for current prepared rowset.
		 * @param name Logical database handle name.
		 * @return Column-name list.
		 */
		[[nodiscard]] QStringList databaseColumnNames(const QString &name) const;
		/**
		 * @brief Returns all column values for current prepared row.
		 * @param name Logical database handle name.
		 * @param values Output values vector.
		 * @return `true` when extraction succeeds.
		 */
		bool                      databaseColumnValues(const QString &name, QVector<QVariant> &values) const;
		/**
		 * @brief Returns color-rule list.
		 * @return Immutable color-rule list.
		 */
		[[nodiscard]] const QList<Colour>        &colours() const;
		/**
		 * @brief Replaces color-rule list.
		 * @param colours New color-rule list.
		 */
		void                                      setColours(const QList<Colour> &colours);
		/**
		 * @brief Returns ANSI color entry for bold/normal table.
		 * @param bold Read from bold table when `true`, normal table otherwise.
		 * @param index ANSI colour index.
		 * @return Color value for the requested entry.
		 */
		[[nodiscard]] QColor                      ansiColour(bool bold, int index) const;
		/**
		 * @brief Sets ANSI colour entry.
		 * @param bold Write to bold table when `true`, normal table otherwise.
		 * @param index ANSI colour index.
		 * @param color New colour value.
		 */
		void                                      setAnsiColour(bool bold, int index, const QColor &color);
		/**
		 * @brief Returns keypad mapping entries.
		 * @return Immutable keypad entry list.
		 */
		[[nodiscard]] const QList<Keypad>        &keypadEntries() const;
		/**
		 * @brief Replaces keypad entry list.
		 * @param entries New keypad entry list.
		 */
		void                                      setKeypadEntries(const QList<Keypad> &entries);
		/**
		 * @brief Returns printing-style entries.
		 * @return Immutable printing-style list.
		 */
		[[nodiscard]] const QList<PrintingStyle> &printingStyles() const;
		/**
		 * @brief Replaces printing-style list.
		 * @param styles New printing-style list.
		 */
		void                                      setPrintingStyles(const QList<PrintingStyle> &styles);
		/**
		 * @brief Plugin lifecycle, messaging, and variable APIs.
		 */
		/**
		 * @brief Returns installed plugin list.
		 * @return Immutable plugin list.
		 */
		[[nodiscard]] const QList<Plugin>        &plugins() const;
		/**
		 * @brief Returns mutable plugin list.
		 * @return Mutable plugin list.
		 */
		QList<Plugin>                            &pluginsMutable();
		/**
		 * @brief Loads plugin file and installs plugin.
		 * @param fileName Plugin XML file path.
		 * @param error Optional output error message.
		 * @param markGlobal Mark loaded plugin as global when `true`.
		 * @return `true` on successful load/install.
		 */
		bool loadPluginFile(const QString &fileName, QString *error = nullptr, bool markGlobal = false);
		/**
		 * @brief Unloads installed plugin.
		 * @param pluginId Plugin id to unload.
		 * @param error Optional output error message.
		 * @return `true` on successful unload.
		 */
		bool unloadPlugin(const QString &pluginId, QString *error = nullptr);
		/**
		 * @brief Enables/disables plugin.
		 * @param pluginId Plugin id to update.
		 * @param enable Enable plugin when `true`, disable otherwise.
		 * @return `true` when state change succeeds.
		 */
		bool enablePlugin(const QString &pluginId, bool enable);
		/**
		 * @brief Returns whether a plugin id is currently installed.
		 * @param pluginId Plugin id to check.
		 * @return `true` when the plugin is installed.
		 */
		[[nodiscard]] bool isPluginInstalled(const QString &pluginId) const;
		/**
		 * @brief Checks plugin support level for a named callback routine.
		 * @param pluginId Plugin id to query.
		 * @param routine Callback routine name.
		 * @return Support code for the requested callback.
		 */
		[[nodiscard]] int  pluginSupports(const QString &pluginId, const QString &routine) const;
		/**
		 * @brief Calls plugin callback routine with string argument.
		 * @param pluginId Target plugin id.
		 * @param routine Callback routine name.
		 * @param argument String argument forwarded to callback.
		 * @param callingPluginId Optional originating plugin id.
		 * @return Plugin callback result code.
		 */
		int callPlugin(const QString &pluginId, const QString &routine, const QString &argument,
		               const QString &callingPluginId = QString());
#ifdef QMUD_ENABLE_LUA_SCRIPTING
		/**
		 * @brief Calls plugin callback routine with forwarded Lua arguments.
		 * @param pluginId Target plugin id.
		 * @param routine Callback routine name.
		 * @param callerState Lua state containing arguments.
		 * @param firstArg First Lua argument index to forward.
		 * @param callingPluginId Optional originating plugin id.
		 * @return Plugin callback result code.
		 */
		int callPluginLua(const QString &pluginId, const QString &routine, lua_State *callerState,
		                  int firstArg, const QString &callingPluginId = QString());
#endif
		/**
		 * @brief Broadcasts plugin message packet to installed plugins.
		 * @param message Numeric message id.
		 * @param text Message payload text.
		 * @param callingPluginId Originating plugin id.
		 * @param callingPluginName Originating plugin display name.
		 * @return Broadcast result code.
		 */
		int                broadcastPlugin(long message, const QString &text, const QString &callingPluginId,
		                                   const QString &callingPluginName);
		/**
		 * @brief In-client chat network APIs.
		 */
		/**
		 * @brief Starts listening for incoming chat calls.
		 * @param port Local port to listen on.
		 * @return Chat API status code.
		 */
		int                chatAcceptCalls(short port);
		/**
		 * @brief Connects to remote chat server.
		 * @param server Remote host or IP.
		 * @param port Remote chat port.
		 * @param zChat Enable ZChat protocol mode when `true`.
		 * @return Chat API status code.
		 */
		int                chatCall(const QString &server, long port, bool zChat);
		/**
		 * @brief Disconnects one chat connection by id.
		 * @param id Chat connection id.
		 * @return Chat API status code.
		 */
		int                chatDisconnect(long id);
		/**
		 * @brief Disconnects all chat connections.
		 * @return Chat API status code.
		 */
		int                chatDisconnectAll();
		/**
		 * @brief Sends chat message to all peers.
		 * @param message Message text.
		 * @param emote Send as emote when `true`.
		 * @return Chat API status code.
		 */
		int                chatEverybody(const QString &message, bool emote);
		/**
		 * @brief Resolves chat connection id by peer name.
		 * @param who Peer display name.
		 * @return Connection id, or 0 when not found.
		 */
		[[nodiscard]] long chatGetId(const QString &who) const;
		/**
		 * @brief Sends message to named chat group.
		 * @param group Group name.
		 * @param message Message text.
		 * @param emote Send as emote when `true`.
		 * @return Chat API status code.
		 */
		int                chatGroup(const QString &group, const QString &message, bool emote);
		/**
		 * @brief Sends message to specific chat id.
		 * @param id Chat connection id.
		 * @param message Message text.
		 * @param emote Send as emote when `true`.
		 * @return Chat API status code.
		 */
		int                chatId(long id, const QString &message, bool emote);
		/**
		 * @brief Sends low-level chat protocol message code.
		 * @param id Chat connection id.
		 * @param message Protocol message code.
		 * @param text Message payload text.
		 * @return Chat API status code.
		 */
		[[nodiscard]] int  chatMessage(long id, short message, const QString &text) const;
		/**
		 * @brief Changes local chat display name.
		 * @param newName New local chat display name.
		 * @return Chat API status code.
		 */
		int                chatNameChange(const QString &newName);
		/**
		 * @brief Emits chat note/notification locally.
		 * @param noteType Note type code.
		 * @param message Note message text.
		 */
		void               chatNote(short noteType, const QString &message);
		/**
		 * @brief Pastes clipboard text to all chat peers.
		 * @return Chat API status code.
		 */
		int                chatPasteEverybody();
		/**
		 * @brief Pastes clipboard text to one chat peer.
		 * @param id Chat connection id.
		 * @return Chat API status code.
		 */
		int                chatPasteText(long id);
		/**
		 * @brief Requests peer connection-list preview.
		 * @param id Chat connection id.
		 * @return Chat API status code.
		 */
		[[nodiscard]] int  chatPeekConnections(long id) const;
		/**
		 * @brief Sends private chat message to named peer.
		 * @param who Peer display name.
		 * @param message Message text.
		 * @param emote Send as emote when `true`.
		 * @return Chat API status code.
		 */
		int                chatPersonal(const QString &who, const QString &message, bool emote);
		/**
		 * @brief Sends ping to chat peer.
		 * @param id Chat connection id.
		 * @return Chat API status code.
		 */
		[[nodiscard]] int  chatPing(long id) const;
		/**
		 * @brief Requests full connection list from peer.
		 * @param id Chat connection id.
		 * @return Chat API status code.
		 */
		[[nodiscard]] int  chatRequestConnections(long id) const;
		/**
		 * @brief Initiates file transfer to chat peer.
		 * @param id Chat connection id.
		 * @param path Local file path to send.
		 * @return Chat API status code.
		 */
		int                chatSendFile(long id, const QString &path);
		/**
		 * @brief Stops listening for incoming chat calls.
		 * @return Chat API status code.
		 */
		int                chatStopAcceptingCalls();
		/**
		 * @brief Cancels active file transfer for connection.
		 * @param id Chat connection id.
		 * @return Chat API status code.
		 */
		int                chatStopFileTransfer(long id);
		/**
		 * @brief Returns chat connection info by info selector code.
		 * @param id Chat connection id.
		 * @param infoType Info selector code.
		 * @return Requested info value.
		 */
		[[nodiscard]] QVariant    chatInfo(long id, int infoType) const;
		/**
		 * @brief Returns list of active chat connection ids.
		 * @return Active chat connection ids.
		 */
		[[nodiscard]] QList<long> chatList() const;
		/**
		 * @brief Returns chat option value for a specific connection.
		 * @param id Chat connection id.
		 * @param optionName Option name.
		 * @return Option value.
		 */
		[[nodiscard]] QVariant    chatOption(long id, const QString &optionName) const;
		/**
		 * @brief Sets chat connection option.
		 * @param id Chat connection id.
		 * @param optionName Option name.
		 * @param value Option value text.
		 * @return Chat API status code.
		 */
		int                       chatSetOption(long id, const QString &optionName, const QString &value);
		/**
		 * @brief Saves plugin state to plugin-state file.
		 * @param pluginId Plugin id to save.
		 * @param scripted Mark save as scripted when `true`.
		 * @param error Optional output error message.
		 * @return Save status code.
		 */
		int savePluginState(const QString &pluginId, bool scripted = false, QString *error = nullptr);
		/**
		 * @brief Returns plugin metadata value by info selector code.
		 * @param pluginId Plugin id to query.
		 * @param infoType Info selector code.
		 * @return Requested metadata value.
		 */
		[[nodiscard]] QVariant    pluginInfo(const QString &pluginId, int infoType) const;
		/**
		 * @brief Lists installed plugin ids in current order.
		 * @return Plugin id list.
		 */
		[[nodiscard]] QStringList pluginIdList() const;
		/**
		 * @brief Returns plugin variable value or empty string.
		 * @param pluginId Plugin id to query.
		 * @param name Plugin variable name.
		 * @return Variable value, or empty string when missing.
		 */
		[[nodiscard]] QString     pluginVariableValue(const QString &pluginId, const QString &name) const;
		/**
		 * @brief Looks up plugin variable into output parameter.
		 * @param pluginId Plugin id to query.
		 * @param name Plugin variable name.
		 * @param value Output variable value.
		 * @return `true` when the variable exists.
		 */
		bool findPluginVariable(const QString &pluginId, const QString &name, QString &value) const;
		/**
		 * @brief Sets plugin variable value.
		 * @param pluginId Plugin id to update.
		 * @param name Plugin variable name.
		 * @param value New variable value.
		 */
		void setPluginVariableValue(const QString &pluginId, const QString &name, const QString &value);
		/**
		 * @brief Lists variable names for plugin.
		 * @param pluginId Plugin id to query.
		 * @return Variable name list.
		 */
		[[nodiscard]] QStringList   pluginVariableList(const QString &pluginId) const;
		/**
		 * @brief Lists trigger names for plugin.
		 * @param pluginId Plugin id to query.
		 * @return Trigger name list.
		 */
		[[nodiscard]] QStringList   pluginTriggerList(const QString &pluginId) const;
		/**
		 * @brief Lists alias names for plugin.
		 * @param pluginId Plugin id to query.
		 * @return Alias name list.
		 */
		[[nodiscard]] QStringList   pluginAliasList(const QString &pluginId) const;
		/**
		 * @brief Lists timer names for plugin.
		 * @param pluginId Plugin id to query.
		 * @return Timer name list.
		 */
		[[nodiscard]] QStringList   pluginTimerList(const QString &pluginId) const;
		/**
		 * @brief Returns mutable plugin pointer by id.
		 * @param pluginId Plugin id to resolve.
		 * @return Mutable plugin pointer, or `nullptr` when missing.
		 */
		[[nodiscard]] Plugin       *pluginForId(const QString &pluginId);
		/**
		 * @brief Returns immutable plugin pointer by id.
		 * @param pluginId Plugin id to resolve.
		 * @return Immutable plugin pointer, or `nullptr` when missing.
		 */
		[[nodiscard]] const Plugin *pluginForId(const QString &pluginId) const;
		/**
		 * @brief Stores trigger wildcard captures for world scope.
		 * @param triggerName Trigger name associated with captures.
		 * @param wildcards Positional wildcard captures.
		 * @param namedWildcards Named wildcard captures.
		 */
		void setTriggerWildcards(const QString &triggerName, const QStringList &wildcards,
		                         const QMap<QString, QString> &namedWildcards);
		/**
		 * @brief Stores alias wildcard captures for world scope.
		 * @param aliasName Alias name associated with captures.
		 * @param wildcards Positional wildcard captures.
		 * @param namedWildcards Named wildcard captures.
		 */
		void setAliasWildcards(const QString &aliasName, const QStringList &wildcards,
		                       const QMap<QString, QString> &namedWildcards);
		/**
		 * @brief Reads world trigger wildcard by numeric/name key.
		 * @param triggerName Trigger name for lookup context.
		 * @param wildcardName Wildcard index/name key.
		 * @param value Output wildcard value when found.
		 * @return `true` when lookup succeeds.
		 */
		bool triggerWildcard(const QString &triggerName, const QString &wildcardName, QString &value) const;
		/**
		 * @brief Reads world alias wildcard by numeric/name key.
		 * @param aliasName Alias name for lookup context.
		 * @param wildcardName Wildcard index/name key.
		 * @param value Output wildcard value when found.
		 * @return `true` when lookup succeeds.
		 */
		bool aliasWildcard(const QString &aliasName, const QString &wildcardName, QString &value) const;
		/**
		 * @brief Stores trigger wildcard captures for a plugin trigger.
		 * @param pluginId Plugin id owning the trigger.
		 * @param triggerName Trigger name associated with captures.
		 * @param wildcards Positional wildcard captures.
		 * @param namedWildcards Named wildcard captures.
		 */
		void setPluginTriggerWildcards(const QString &pluginId, const QString &triggerName,
		                               const QStringList            &wildcards,
		                               const QMap<QString, QString> &namedWildcards);
		/**
		 * @brief Stores alias wildcard captures for a plugin alias.
		 * @param pluginId Plugin id owning the alias.
		 * @param aliasName Alias name associated with captures.
		 * @param wildcards Positional wildcard captures.
		 * @param namedWildcards Named wildcard captures.
		 */
		void setPluginAliasWildcards(const QString &pluginId, const QString &aliasName,
		                             const QStringList            &wildcards,
		                             const QMap<QString, QString> &namedWildcards);
		/**
		 * @brief Reads plugin trigger wildcard by numeric/name key.
		 * @param pluginId Plugin id owning the trigger.
		 * @param triggerName Trigger name for lookup context.
		 * @param wildcardName Wildcard index/name key.
		 * @param value Output wildcard value when found.
		 * @return `true` when lookup succeeds.
		 */
		bool pluginTriggerWildcard(const QString &pluginId, const QString &triggerName,
		                           const QString &wildcardName, QString &value) const;
		/**
		 * @brief Reads plugin alias wildcard by numeric/name key.
		 * @param pluginId Plugin id owning the alias.
		 * @param aliasName Alias name for lookup context.
		 * @param wildcardName Wildcard index/name key.
		 * @param value Output wildcard value when found.
		 * @return `true` when lookup succeeds.
		 */
		bool pluginAliasWildcard(const QString &pluginId, const QString &aliasName,
		                         const QString &wildcardName, QString &value) const;
		/**
		 * @brief Include/script/comment and output line-buffer APIs.
		 */
		/**
		 * @brief Returns include blocks list.
		 * @return Immutable include block list.
		 */
		[[nodiscard]] const QList<Include> &includes() const;
		/**
		 * @brief Returns script blocks list.
		 * @return Immutable script block list.
		 */
		[[nodiscard]] const QList<Script>  &scripts() const;
		/**
		 * @brief Returns world comment text.
		 * @return World comment text.
		 */
		[[nodiscard]] QString               comments() const;
		/**
		 * @brief Appends line entry to output buffer.
		 * @param text Line text.
		 * @param flags Line flags bitmask.
		 * @param hardReturn Mark line as hard-return terminated when `true`.
		 * @param time Timestamp associated with the line.
		 */
		void                                addLine(const QString &text, int flags, bool hardReturn = true,
		                                            const QDateTime &time = QDateTime::currentDateTime());
		/**
		 * @brief Appends styled line entry to output buffer.
		 * @param text Line text.
		 * @param flags Line flags bitmask.
		 * @param spans Style spans applied to `text`.
		 * @param hardReturn Mark line as hard-return terminated when `true`.
		 * @param time Timestamp associated with the line.
		 */
		void addLine(const QString &text, int flags, const QVector<StyleSpan> &spans, bool hardReturn = true,
		             const QDateTime &time = QDateTime::currentDateTime());
		/**
		 * @brief Returns immutable output line buffer.
		 * @return Immutable buffered line list.
		 */
		[[nodiscard]] const QVector<LineEntry> &lines() const;
		/**
		 * @brief Begins temporary incoming-line context for Lua callbacks.
		 * @param text Incoming line text.
		 * @param flags Line flags bitmask.
		 * @param spans Style spans applied to `text`.
		 * @param hardReturn Mark line as hard-return terminated when `true`.
		 */
		void beginIncomingLineLuaContext(const QString &text, int flags, const QVector<StyleSpan> &spans,
		                                 bool hardReturn = true);
		/**
		 * @brief Ends current incoming-line Lua context.
		 */
		void endIncomingLineLuaContext();
		/**
		 * @brief Marks current Lua-context line as buffered.
		 */
		void markIncomingLineLuaContextBuffered();
		/**
		 * @brief Marks current Lua-context line as committed.
		 */
		void markIncomingLineLuaContextCommitted();
		/**
		 * @brief Returns buffered Lua-context line entry by index.
		 * @param lineNumber Zero-based buffered line index.
		 * @param entry Output line entry.
		 * @return `true` when the line exists.
		 */
		bool luaContextLineEntry(int lineNumber, LineEntry &entry) const;
		/**
		 * @brief Returns buffered Lua-context line count.
		 * @return Buffered Lua-context line count.
		 */
		[[nodiscard]] int luaContextLinesInBufferCount() const;
		/**
		 * @brief Miniwindow graphics, text, image, and hotspot APIs.
		 */
		/**
		 * @brief Creates or reconfigures a miniwindow.
		 * @param name Miniwindow name.
		 * @param left Left position.
		 * @param top Top position.
		 * @param width Window width.
		 * @param height Window height.
		 * @param position Anchor/position code.
		 * @param flags Miniwindow flags bitmask.
		 * @param background Background colour.
		 * @param pluginId Owning plugin id.
		 * @return API status code.
		 */
		int windowCreate(const QString &name, int left, int top, int width, int height, int position,
		                 int flags, const QColor &background, const QString &pluginId);
		/**
		 * @brief Shows or hides miniwindow.
		 * @param name Miniwindow name.
		 * @param show Show window when `true`, hide otherwise.
		 * @return API status code.
		 */
		int windowShow(const QString &name, bool show);
		/**
		 * @brief Deletes miniwindow.
		 * @param name Miniwindow name.
		 * @return API status code.
		 */
		int windowDelete(const QString &name);
		/**
		 * @brief Lists all miniwindow names.
		 * @return Miniwindow name list.
		 */
		[[nodiscard]] QStringList       windowList() const;
		/**
		 * @brief Returns miniwindow metadata by info selector code.
		 * @param name Miniwindow name.
		 * @param infoType Info selector code.
		 * @return Requested metadata value.
		 */
		[[nodiscard]] QVariant          windowInfo(const QString &name, int infoType) const;
		/**
		 * @brief Returns mutable miniwindow by name.
		 * @param name Miniwindow name.
		 * @return Mutable miniwindow pointer, or `nullptr` when missing.
		 */
		[[nodiscard]] MiniWindow       *miniWindow(const QString &name);
		/**
		 * @brief Returns immutable miniwindow by name.
		 * @param name Miniwindow name.
		 * @return Immutable miniwindow pointer, or `nullptr` when missing.
		 */
		[[nodiscard]] const MiniWindow *miniWindow(const QString &name) const;
		/**
		 * @brief Returns miniwindows sorted by z-order.
		 * @return Miniwindows sorted by z-order.
		 */
		QVector<MiniWindow *>           sortedMiniWindows();
		/**
		 * @brief Lays out miniwindows for current viewport sizes.
		 * @param clientSize Current output client size.
		 * @param ownerSize Owning widget size.
		 * @param underneath Layout behind output surface when `true`.
		 */
		void layoutMiniWindows(const QSize &clientSize, const QSize &ownerSize, bool underneath);
		/**
		 * @brief Draws rectangle/frame/fill operation on miniwindow.
		 * @param name Miniwindow name.
		 * @param action Drawing action code.
		 * @param left Left coordinate.
		 * @param top Top coordinate.
		 * @param right Right coordinate.
		 * @param bottom Bottom coordinate.
		 * @param colour1 Primary colour.
		 * @param colour2 Secondary colour.
		 * @return API status code.
		 */
		int  windowRectOp(const QString &name, int action, int left, int top, int right, int bottom,
		                  long colour1, long colour2);
		/**
		 * @brief Draws circle/ellipse operation on miniwindow.
		 * @param name Miniwindow name.
		 * @param action Drawing action code.
		 * @param left Left coordinate.
		 * @param top Top coordinate.
		 * @param right Right coordinate.
		 * @param bottom Bottom coordinate.
		 * @param penColour Pen colour.
		 * @param penStyle Pen style code.
		 * @param penWidth Pen width.
		 * @param brushColour Brush colour.
		 * @param brushStyle Brush style code.
		 * @param extra1 Extra mode parameter 1.
		 * @param extra2 Extra mode parameter 2.
		 * @param extra3 Extra mode parameter 3.
		 * @param extra4 Extra mode parameter 4.
		 * @return API status code.
		 */
		int  windowCircleOp(const QString &name, int action, int left, int top, int right, int bottom,
		                    long penColour, long penStyle, int penWidth, long brushColour, long brushStyle,
		                    int extra1, int extra2, int extra3, int extra4);
		/**
		 * @brief Draws line segment on miniwindow.
		 * @param name Miniwindow name.
		 * @param x1 Start x coordinate.
		 * @param y1 Start y coordinate.
		 * @param x2 End x coordinate.
		 * @param y2 End y coordinate.
		 * @param penColour Pen colour.
		 * @param penStyle Pen style code.
		 * @param penWidth Pen width.
		 * @return API status code.
		 */
		int  windowLine(const QString &name, int x1, int y1, int x2, int y2, long penColour, long penStyle,
		                int penWidth);
		/**
		 * @brief Draws arc segment on miniwindow.
		 * @param name Miniwindow name.
		 * @param left Left bound.
		 * @param top Top bound.
		 * @param right Right bound.
		 * @param bottom Bottom bound.
		 * @param x1 Arc start x.
		 * @param y1 Arc start y.
		 * @param x2 Arc end x.
		 * @param y2 Arc end y.
		 * @param penColour Pen colour.
		 * @param penStyle Pen style code.
		 * @param penWidth Pen width.
		 * @return API status code.
		 */
		int  windowArc(const QString &name, int left, int top, int right, int bottom, int x1, int y1, int x2,
		               int y2, long penColour, long penStyle, int penWidth);
		/**
		 * @brief Draws bezier polyline from point list.
		 * @param name Miniwindow name.
		 * @param points Point-list string.
		 * @param penColour Pen colour.
		 * @param penStyle Pen style code.
		 * @param penWidth Pen width.
		 * @return API status code.
		 */
		int  windowBezier(const QString &name, const QString &points, long penColour, long penStyle,
		                  int penWidth);
		/**
		 * @brief Draws polygon/polyline from point list.
		 * @param name Miniwindow name.
		 * @param points Point-list string.
		 * @param penColour Pen colour.
		 * @param penStyle Pen style code.
		 * @param penWidth Pen width.
		 * @param brushColour Brush colour.
		 * @param brushStyle Brush style code.
		 * @param closePolygon Close shape when `true`.
		 * @param winding Use winding fill rule when `true`.
		 * @return API status code.
		 */
		int  windowPolygon(const QString &name, const QString &points, long penColour, long penStyle,
		                   int penWidth, long brushColour, long brushStyle, bool closePolygon, bool winding);
		/**
		 * @brief Paints gradient-filled rectangle.
		 * @param name Miniwindow name.
		 * @param left Left coordinate.
		 * @param top Top coordinate.
		 * @param right Right coordinate.
		 * @param bottom Bottom coordinate.
		 * @param startColour Gradient start colour.
		 * @param endColour Gradient end colour.
		 * @param mode Gradient mode code.
		 * @return API status code.
		 */
		int  windowGradient(const QString &name, int left, int top, int right, int bottom, long startColour,
		                    long endColour, int mode);
		/**
		 * @brief Creates or updates named miniwindow font resource.
		 * @param name Miniwindow name.
		 * @param fontId Font resource id.
		 * @param fontName Font family name.
		 * @param size Font size.
		 * @param bold Enable bold when `true`.
		 * @param italic Enable italic when `true`.
		 * @param underline Enable underline when `true`.
		 * @param strikeout Enable strikeout when `true`.
		 * @param charset Charset code.
		 * @param pitchAndFamily Pitch/family code.
		 * @return API status code.
		 */
		int  windowFont(const QString &name, const QString &fontId, const QString &fontName, double size,
		                bool bold, bool italic, bool underline, bool strikeout, int charset,
		                int pitchAndFamily);
		/**
		 * @brief Returns miniwindow font metadata.
		 * @param name Miniwindow name.
		 * @param fontId Font resource id.
		 * @param infoType Info selector code.
		 * @return Requested font metadata value.
		 */
		[[nodiscard]] QVariant windowFontInfo(const QString &name, const QString &fontId, int infoType) const;
		/**
		 * @brief Lists font ids registered in miniwindow.
		 * @param name Miniwindow name.
		 * @return Font id list.
		 */
		[[nodiscard]] QStringList windowFontList(const QString &name) const;
		/**
		 * @brief Draws text using miniwindow font resource.
		 * @param name Miniwindow name.
		 * @param fontId Font resource id.
		 * @param text Text to draw.
		 * @param left Left bound.
		 * @param top Top bound.
		 * @param right Right bound.
		 * @param bottom Bottom bound.
		 * @param colour Text colour.
		 * @return API status code.
		 */
		int windowText(const QString &name, const QString &fontId, const QString &text, int left, int top,
		               int right, int bottom, long colour);
		/**
		 * @brief Measures text width for a miniwindow font.
		 * @param name Miniwindow name.
		 * @param fontId Font resource id.
		 * @param text Text to measure.
		 * @return Text width in pixels.
		 */
		[[nodiscard]] int      windowTextWidth(const QString &name, const QString &fontId,
		                                       const QString &text) const;
		/**
		 * @brief Sets one pixel in miniwindow backing image.
		 * @param name Miniwindow name.
		 * @param x Pixel x coordinate.
		 * @param y Pixel y coordinate.
		 * @param colour Pixel colour.
		 * @return API status code.
		 */
		int                    windowSetPixel(const QString &name, int x, int y, long colour);
		/**
		 * @brief Reads one pixel value from miniwindow backing image.
		 * @param name Miniwindow name.
		 * @param x Pixel x coordinate.
		 * @param y Pixel y coordinate.
		 * @return Pixel colour value.
		 */
		[[nodiscard]] QVariant windowGetPixel(const QString &name, int x, int y) const;
		/**
		 * @brief Creates image from 8x8 monochrome bitmap rows.
		 * @param name Miniwindow name.
		 * @param imageId Image resource id.
		 * @param row1 Bitmap row 1 bits.
		 * @param row2 Bitmap row 2 bits.
		 * @param row3 Bitmap row 3 bits.
		 * @param row4 Bitmap row 4 bits.
		 * @param row5 Bitmap row 5 bits.
		 * @param row6 Bitmap row 6 bits.
		 * @param row7 Bitmap row 7 bits.
		 * @param row8 Bitmap row 8 bits.
		 * @return API status code.
		 */
		int windowCreateImage(const QString &name, const QString &imageId, long row1, long row2, long row3,
		                      long row4, long row5, long row6, long row7, long row8);
		/**
		 * @brief Loads image resource from disk.
		 * @param name Miniwindow name.
		 * @param imageId Image resource id.
		 * @param filename Source image file path.
		 * @return API status code.
		 */
		int windowLoadImage(const QString &name, const QString &imageId, const QString &filename);
		/**
		 * @brief Loads image resource from memory bytes.
		 * @param name Miniwindow name.
		 * @param imageId Image resource id.
		 * @param data Encoded image bytes.
		 * @param hasAlpha Indicates whether the image data includes alpha.
		 * @return API status code.
		 */
		int windowLoadImageMemory(const QString &name, const QString &imageId, const QByteArray &data,
		                          bool hasAlpha);
		/**
		 * @brief Lists image ids available in miniwindow.
		 * @param name Miniwindow name.
		 * @return Image resource id list.
		 */
		[[nodiscard]] QStringList windowImageList(const QString &name) const;
		/**
		 * @brief Returns image metadata by info selector code.
		 * @param name Miniwindow name.
		 * @param imageId Image resource id.
		 * @param infoType Info selector code.
		 * @return Requested image metadata value.
		 */
		[[nodiscard]] QVariant    windowImageInfo(const QString &name, const QString &imageId,
		                                          int infoType) const;
		/**
		 * @brief Captures rendered image from another miniwindow.
		 * @param name Destination miniwindow name.
		 * @param imageId Destination image resource id.
		 * @param sourceWindow Source miniwindow name.
		 * @return API status code.
		 */
		int windowImageFromWindow(const QString &name, const QString &imageId, const QString &sourceWindow);
		/**
		 * @brief Draws image sub-rectangle into destination rectangle.
		 * @param name Miniwindow name.
		 * @param imageId Image resource id.
		 * @param left Destination left.
		 * @param top Destination top.
		 * @param right Destination right.
		 * @param bottom Destination bottom.
		 * @param mode Draw mode code.
		 * @param srcLeft Source left.
		 * @param srcTop Source top.
		 * @param srcRight Source right.
		 * @param srcBottom Source bottom.
		 * @return API status code.
		 */
		int windowDrawImage(const QString &name, const QString &imageId, int left, int top, int right,
		                    int bottom, int mode, int srcLeft, int srcTop, int srcRight, int srcBottom);
		/**
		 * @brief Draws image with opacity blend.
		 * @param name Miniwindow name.
		 * @param imageId Image resource id.
		 * @param left Destination left.
		 * @param top Destination top.
		 * @param right Destination right.
		 * @param bottom Destination bottom.
		 * @param opacity Opacity from 0.0 to 1.0.
		 * @param srcLeft Source left.
		 * @param srcTop Source top.
		 * @return API status code.
		 */
		int windowDrawImageAlpha(const QString &name, const QString &imageId, int left, int top, int right,
		                         int bottom, double opacity, int srcLeft, int srcTop);
		/**
		 * @brief Performs image brush draw operation (ellipse/round-rect/etc.).
		 * @param name Miniwindow name.
		 * @param action Drawing action code.
		 * @param left Left coordinate.
		 * @param top Top coordinate.
		 * @param right Right coordinate.
		 * @param bottom Bottom coordinate.
		 * @param penColour Pen colour.
		 * @param penStyle Pen style code.
		 * @param penWidth Pen width.
		 * @param brushColour Brush colour.
		 * @param imageId Brush image id.
		 * @param ellipseWidth Ellipse corner width.
		 * @param ellipseHeight Ellipse corner height.
		 * @return API status code.
		 */
		int windowImageOp(const QString &name, int action, int left, int top, int right, int bottom,
		                  long penColour, long penStyle, int penWidth, long brushColour,
		                  const QString &imageId, int ellipseWidth, int ellipseHeight);
		/**
		 * @brief Composites image with mask and opacity.
		 * @param name Miniwindow name.
		 * @param imageId Source image id.
		 * @param maskId Mask image id.
		 * @param left Destination left.
		 * @param top Destination top.
		 * @param right Destination right.
		 * @param bottom Destination bottom.
		 * @param mode Blend mode code.
		 * @param opacity Opacity from 0.0 to 1.0.
		 * @param srcLeft Source left.
		 * @param srcTop Source top.
		 * @param srcRight Source right.
		 * @param srcBottom Source bottom.
		 * @return API status code.
		 */
		int windowMergeImageAlpha(const QString &name, const QString &imageId, const QString &maskId,
		                          int left, int top, int right, int bottom, int mode, double opacity,
		                          int srcLeft, int srcTop, int srcRight, int srcBottom);
		/**
		 * @brief Extracts alpha channel from image region.
		 * @param name Miniwindow name.
		 * @param imageId Image resource id.
		 * @param left Destination left.
		 * @param top Destination top.
		 * @param right Destination right.
		 * @param bottom Destination bottom.
		 * @param srcLeft Source left.
		 * @param srcTop Source top.
		 * @return API status code.
		 */
		int windowGetImageAlpha(const QString &name, const QString &imageId, int left, int top, int right,
		                        int bottom, int srcLeft, int srcTop);
		/**
		 * @brief Blends source image into target using blend mode.
		 * @param name Miniwindow name.
		 * @param imageId Source image id.
		 * @param left Destination left.
		 * @param top Destination top.
		 * @param right Destination right.
		 * @param bottom Destination bottom.
		 * @param mode Blend mode code.
		 * @param opacity Opacity from 0.0 to 1.0.
		 * @param srcLeft Source left.
		 * @param srcTop Source top.
		 * @param srcRight Source right.
		 * @param srcBottom Source bottom.
		 * @return API status code.
		 */
		int windowBlendImage(const QString &name, const QString &imageId, int left, int top, int right,
		                     int bottom, int mode, double opacity, int srcLeft, int srcTop, int srcRight,
		                     int srcBottom);
		/**
		 * @brief Applies image filter operation to region.
		 * @param name Miniwindow name.
		 * @param left Left coordinate.
		 * @param top Top coordinate.
		 * @param right Right coordinate.
		 * @param bottom Bottom coordinate.
		 * @param operation Filter operation code.
		 * @param options Filter options value.
		 * @param extra Extra operation parameter.
		 * @return API status code.
		 */
		int windowFilter(const QString &name, int left, int top, int right, int bottom, int operation,
		                 double options, int extra);
		/**
		 * @brief Draws image with affine transform matrix.
		 * @param name Miniwindow name.
		 * @param imageId Image resource id.
		 * @param left Destination left.
		 * @param top Destination top.
		 * @param mode Draw mode code.
		 * @param mxx Matrix xx component.
		 * @param mxy Matrix xy component.
		 * @param myx Matrix yx component.
		 * @param myy Matrix yy component.
		 * @return API status code.
		 */
		int windowTransformImage(const QString &name, const QString &imageId, float left, float top, int mode,
		                         float mxx, float mxy, float myx, float myy);
		/**
		 * @brief Writes miniwindow surface to image file.
		 * @param name Miniwindow name.
		 * @param filename Destination image file path.
		 * @return API status code.
		 */
		int windowWrite(const QString &name, const QString &filename);
		/**
		 * @brief Repositions miniwindow.
		 * @param name Miniwindow name.
		 * @param left Left position.
		 * @param top Top position.
		 * @param position Anchor/position code.
		 * @param flags Positioning flags.
		 * @return API status code.
		 */
		int windowPosition(const QString &name, int left, int top, int position, int flags);
		/**
		 * @brief Resizes miniwindow surface.
		 * @param name Miniwindow name.
		 * @param width New width.
		 * @param height New height.
		 * @param colour Fill color for new area.
		 * @return API status code.
		 */
		int windowResize(const QString &name, int width, int height, long colour);
		/**
		 * @brief Sets miniwindow z-order.
		 * @param name Miniwindow name.
		 * @param zOrder New z-order value.
		 * @return API status code.
		 */
		int windowSetZOrder(const QString &name, int zOrder);
		/**
		 * @brief Adds interactive hotspot rectangle to miniwindow.
		 * @param name Miniwindow name.
		 * @param hotspotId Hotspot id.
		 * @param left Left coordinate.
		 * @param top Top coordinate.
		 * @param right Right coordinate.
		 * @param bottom Bottom coordinate.
		 * @param mouseOver Mouse-over callback.
		 * @param cancelMouseOver Mouse-over cancel callback.
		 * @param mouseDown Mouse-down callback.
		 * @param cancelMouseDown Mouse-down cancel callback.
		 * @param mouseUp Mouse-up callback.
		 * @param tooltip Tooltip text.
		 * @param cursor Cursor code.
		 * @param flags Hotspot flags.
		 * @param pluginId Owning plugin id.
		 * @return API status code.
		 */
		int windowAddHotspot(const QString &name, const QString &hotspotId, int left, int top, int right,
		                     int bottom, const QString &mouseOver, const QString &cancelMouseOver,
		                     const QString &mouseDown, const QString &cancelMouseDown, const QString &mouseUp,
		                     const QString &tooltip, int cursor, int flags, const QString &pluginId);
		/**
		 * @brief Deletes hotspot by id.
		 * @param name Miniwindow name.
		 * @param hotspotId Hotspot id.
		 * @return API status code.
		 */
		int windowDeleteHotspot(const QString &name, const QString &hotspotId);
		/**
		 * @brief Deletes all hotspots from miniwindow.
		 * @param name Miniwindow name.
		 * @return API status code.
		 */
		int windowDeleteAllHotspots(const QString &name);
		/**
		 * @brief Lists hotspot ids for miniwindow.
		 * @param name Miniwindow name.
		 * @return Hotspot id list.
		 */
		[[nodiscard]] QStringList windowHotspotList(const QString &name) const;
		/**
		 * @brief Returns hotspot metadata by info selector code.
		 * @param name Miniwindow name.
		 * @param hotspotId Hotspot id.
		 * @param infoType Info selector code.
		 * @return Requested hotspot metadata.
		 */
		[[nodiscard]] QVariant    windowHotspotInfo(const QString &name, const QString &hotspotId,
		                                            int infoType) const;
		/**
		 * @brief Updates hotspot tooltip text.
		 * @param name Miniwindow name.
		 * @param hotspotId Hotspot id.
		 * @param tooltip New tooltip text.
		 * @return API status code.
		 */
		int     windowHotspotTooltip(const QString &name, const QString &hotspotId, const QString &tooltip);
		/**
		 * @brief Moves hotspot rectangle to new coordinates.
		 * @param name Miniwindow name.
		 * @param hotspotId Hotspot id.
		 * @param left Left coordinate.
		 * @param top Top coordinate.
		 * @param right Right coordinate.
		 * @param bottom Bottom coordinate.
		 * @return API status code.
		 */
		int     windowMoveHotspot(const QString &name, const QString &hotspotId, int left, int top, int right,
		                          int bottom);
		/**
		 * @brief Assigns drag callbacks to hotspot.
		 * @param name Miniwindow name.
		 * @param hotspotId Hotspot id.
		 * @param moveCallback Drag-move callback name.
		 * @param releaseCallback Drag-release callback name.
		 * @param flags Handler flags.
		 * @param pluginId Owning plugin id.
		 * @return API status code.
		 */
		int     windowDragHandler(const QString &name, const QString &hotspotId, const QString &moveCallback,
		                          const QString &releaseCallback, int flags, const QString &pluginId);
		/**
		 * @brief Assigns mouse-wheel callback to hotspot.
		 * @param name Miniwindow name.
		 * @param hotspotId Hotspot id.
		 * @param moveCallback Mouse-wheel callback name.
		 * @param pluginId Owning plugin id.
		 * @return API status code.
		 */
		int     windowScrollwheelHandler(const QString &name, const QString &hotspotId,
		                                 const QString &moveCallback, const QString &pluginId);
		/**
		 * @brief Shows miniwindow popup menu and returns chosen item id.
		 * @param name Miniwindow name.
		 * @param left Popup x coordinate.
		 * @param top Popup y coordinate.
		 * @param items Menu specification string.
		 * @param pluginId Owning plugin id.
		 * @return Selected menu item id, or empty when canceled.
		 */
		QString windowMenu(const QString &name, int left, int top, const QString &items,
		                   const QString &pluginId);

		/**
		 * @brief View binding and runtime-mode/stat counters.
		 */
		/**
		 * @brief Binds runtime to a world view.
		 * @param view World view pointer.
		 */
		void    setView(WorldView *view);
		/**
		 * @brief Returns bound world view.
		 * @return Bound world view pointer.
		 */
		[[nodiscard]] WorldView            *view() const;
		/**
		 * @brief Installs plugins queued for delayed install.
		 */
		void                                installPendingPlugins();
		/**
		 * @brief Enables/disables deferred plugin installation.
		 * @param deferred Defer installs when `true`.
		 */
		void                                setPluginInstallDeferred(bool deferred);
		/**
		 * @brief Returns whether plugin installation is currently deferred.
		 * @return `true` when plugin install is deferred.
		 */
		[[nodiscard]] bool                  pluginInstallDeferred() const;
		/**
		 * @brief Sets default world output background color.
		 * @param colour RGB background color value.
		 */
		void                                setBackgroundColour(long colour);
		/**
		 * @brief Returns default world output background color.
		 * @return RGB background color value.
		 */
		[[nodiscard]] long                  backgroundColour() const;
		/**
		 * @brief Returns whether runtime is in simulate mode.
		 * @return `true` when simulate mode is active.
		 */
		[[nodiscard]] bool                  doingSimulate() const;
		/**
		 * @brief Enables/disables simulate mode.
		 * @param value Enable simulate mode when `true`.
		 */
		void                                setDoingSimulate(bool value);

		/**
		 * @brief Returns active world trigger count.
		 * @return Active trigger count.
		 */
		[[nodiscard]] int                   triggerCount() const;
		/**
		 * @brief Returns active world alias count.
		 * @return Active alias count.
		 */
		[[nodiscard]] int                   aliasCount() const;
		/**
		 * @brief Returns active world timer count.
		 * @return Active timer count.
		 */
		[[nodiscard]] int                   timerCount() const;
		/**
		 * @brief Returns macro count.
		 * @return Macro count.
		 */
		[[nodiscard]] int                   macroCount() const;
		/**
		 * @brief Returns variable count.
		 * @return Variable count.
		 */
		[[nodiscard]] int                   variableCount() const;
		/**
		 * @brief Returns color rule count.
		 * @return Colour-rule count.
		 */
		[[nodiscard]] int                   colourCount() const;
		/**
		 * @brief Returns keypad mapping count.
		 * @return Keypad mapping count.
		 */
		[[nodiscard]] int                   keypadCount() const;
		/**
		 * @brief Returns printing-style rule count.
		 * @return Printing-style rule count.
		 */
		[[nodiscard]] int                   printingStyleCount() const;
		/**
		 * @brief Returns installed plugin count.
		 * @return Installed plugin count.
		 */
		[[nodiscard]] int                   pluginCount() const;
		/**
		 * @brief Returns include block count.
		 * @return Include block count.
		 */
		[[nodiscard]] int                   includeCount() const;
		/**
		 * @brief Returns script block count.
		 * @return Script block count.
		 */
		[[nodiscard]] int                   scriptCount() const;

		/**
		 * @brief Network I/O, command dispatch, and session counters.
		 */
		/**
		 * @brief Feeds raw socket bytes into telnet/output processing.
		 * @param data Raw socket payload bytes.
		 */
		void                                receiveRawData(const QByteArray &data);
		/**
		 * @brief Opens world socket connection.
		 * @param host Remote host name or IP.
		 * @param port Remote port number.
		 * @return `true` when the connect attempt was started.
		 */
		bool                                connectToWorld(const QString &host, quint16 port);
		/**
		 * @brief Closes active world socket connection.
		 */
		void                                disconnectFromWorld();
		/**
		 * @brief Sends raw bytes to world socket.
		 * @param payload Bytes to send.
		 */
		void                                sendToWorld(const QByteArray &payload);
		/**
		 * @brief Increments sent-line counter.
		 */
		void                                incrementLinesSent();
		/**
		 * @brief Returns total sent lines for current session.
		 * @return Total sent line count.
		 */
		[[nodiscard]] int                   totalLinesSent() const;
		/**
		 * @brief Returns total received lines for current session.
		 * @return Total received line count.
		 */
		[[nodiscard]] int                   totalLinesReceived() const;
		/**
		 * @brief Returns unread/new line counter.
		 * @return New line counter.
		 */
		[[nodiscard]] int                   newLines() const;
		/**
		 * @brief Sets unread/new line counter.
		 * @param value New unread line counter value.
		 */
		void                                setNewLines(int value);
		/**
		 * @brief Increments unread/new line counter.
		 */
		void                                incrementNewLines();
		/**
		 * @brief Resets unread/new line counter.
		 */
		void                                clearNewLines();
		/**
		 * @brief Marks runtime as active/inactive in UI context.
		 * @param active Mark runtime active when `true`.
		 */
		void                                setActive(bool active);
		/**
		 * @brief Returns active/inactive UI state.
		 * @return `true` when runtime is active.
		 */
		[[nodiscard]] bool                  isActive() const;
		/**
		 * @brief Returns whether outstanding-line badges are suppressed.
		 * @return `true` when outstanding-line badges are hidden.
		 */
		[[nodiscard]] bool                  doNotShowOutstandingLines() const;
		/**
		 * @brief Sets current action source for command dispatch context.
		 * @param source Action source code.
		 */
		void                                setCurrentActionSource(unsigned short source);
		/**
		 * @brief Returns current action source code.
		 * @return Current action source code.
		 */
		[[nodiscard]] unsigned short        currentActionSource() const;
		/**
		 * @brief Increments UTF-8 decode error counter.
		 */
		void                                incrementUtf8ErrorCount();
		/**
		 * @brief Returns UTF-8 decode error counter.
		 * @return UTF-8 error count.
		 */
		[[nodiscard]] int                   utf8ErrorCount() const;
		/**
		 * @brief Records last line number that ended with IAC-GA.
		 * @param lineNumber Line number ending with IAC-GA.
		 */
		void                                setLastLineWithIacGa(int lineNumber);
		/**
		 * @brief Returns last line number that ended with IAC-GA.
		 * @return Last IAC-GA line number.
		 */
		[[nodiscard]] int                   lastLineWithIacGa() const;
		/**
		 * @brief Increments evaluated-trigger counter.
		 */
		void                                incrementTriggersEvaluated();
		/**
		 * @brief Increments matched-trigger counter.
		 */
		void                                incrementTriggersMatched();
		/**
		 * @brief Increments evaluated-alias counter.
		 */
		void                                incrementAliasesEvaluated();
		/**
		 * @brief Increments matched-alias counter.
		 */
		void                                incrementAliasesMatched();
		/**
		 * @brief Increments fired-timer counter.
		 */
		void                                incrementTimersFired();
		/**
		 * @brief Returns total evaluated triggers this session.
		 * @return Evaluated trigger count.
		 */
		[[nodiscard]] int                   triggersEvaluatedCount() const;
		/**
		 * @brief Returns total matched triggers this session.
		 * @return Matched trigger count.
		 */
		[[nodiscard]] int                   triggersMatchedThisSession() const;
		/**
		 * @brief Returns total evaluated aliases this session.
		 * @return Evaluated alias count.
		 */
		[[nodiscard]] int                   aliasesEvaluatedCount() const;
		/**
		 * @brief Returns total matched aliases this session.
		 * @return Matched alias count.
		 */
		[[nodiscard]] int                   aliasesMatchedThisSession() const;
		/**
		 * @brief Returns total fired timers this session.
		 * @return Fired timer count.
		 */
		[[nodiscard]] int                   timersFiredThisSession() const;
		/**
		 * @brief Records timestamp of last user-entered command.
		 * @param when Timestamp to store.
		 */
		void                                setLastUserInput(const QDateTime &when);
		/**
		 * @brief Returns timestamp of last user-entered command.
		 * @return Last user input timestamp.
		 */
		[[nodiscard]] QDateTime             lastUserInput() const;
		/**
		 * @brief Loads font file and tracks it for cleanup.
		 * @param path Font file path.
		 * @return API status code.
		 */
		int                                 addFontFromFile(const QString &path);
		/**
		 * @brief Returns first loaded special-font path.
		 * @return First special-font path.
		 */
		[[nodiscard]] QString               firstSpecialFontPath() const;
		/**
		 * @brief Adds mapper direction pair.
		 * @param direction Forward direction command.
		 * @param reverse Reverse direction command.
		 * @return API status code.
		 */
		int                                 addToMapper(const QString &direction, const QString &reverse);
		/**
		 * @brief Adds mapper comment entry.
		 * @param comment Comment text.
		 * @return API status code.
		 */
		int                                 addMapperComment(const QString &comment);
		/**
		 * @brief Moves item to front of shift-tab completion list.
		 * @param item Completion item text.
		 * @return API status code.
		 */
		int                                 shiftTabCompleteItem(const QString &item);
		/**
		 * @brief Deletes latest mapper entry.
		 * @return API status code.
		 */
		int                                 deleteLastMapItem();
		/**
		 * @brief Clears mapper history.
		 * @return API status code.
		 */
		int                                 deleteAllMapItems();
		/**
		 * @brief Removes last output lines from buffer.
		 * @param count Number of trailing lines to remove.
		 */
		void                                deleteLines(int count);
		/**
		 * @brief Clears output line buffer.
		 */
		void                                deleteOutput();
		/**
		 * @brief Deletes world variable by name.
		 * @param name Variable name.
		 * @return API status code.
		 */
		int                                 deleteVariable(const QString &name);
		/**
		 * @brief Clears queued commands and returns discarded count.
		 * @return Number of discarded commands.
		 */
		[[nodiscard]] int                   discardQueuedCommands() const;
		/**
		 * @brief Enables/disables mapper collection.
		 * @param enabled Enable mapping collection when `true`.
		 */
		void                                setMappingEnabled(bool enabled);
		/**
		 * @brief Expands speedwalk notation into commands.
		 * @param speedWalkString Speedwalk text.
		 * @return Expanded command sequence.
		 */
		[[nodiscard]] QString               evaluateSpeedwalk(const QString &speedWalkString) const;
		/**
		 * @brief Executes one command via command processor.
		 * @param text Command text.
		 * @return API status code.
		 */
		[[nodiscard]] int                   executeCommand(const QString &text) const;
		/**
		 * @brief Gets MXP entity value.
		 * @param name Entity name.
		 * @return Entity value.
		 */
		[[nodiscard]] QString               getEntityValue(const QString &name) const;
		/**
		 * @brief Sets MXP entity value.
		 * @param name Entity name.
		 * @param value Entity value.
		 */
		void                                setEntityValue(const QString &name, const QString &value);
		/**
		 * @brief Emits ANSI text through output pipeline.
		 * @param text Text to emit.
		 * @param note Emit as note when `true`.
		 */
		void                                outputAnsiText(const QString &text, bool note);
		/**
		 * @brief Returns whether world socket is connected.
		 * @return `true` when connected.
		 */
		[[nodiscard]] bool                  isConnected() const;
		/**
		 * @brief Returns current connection phase enum value.
		 * @return Connection phase code.
		 */
		[[nodiscard]] int                   connectPhase() const;
		/**
		 * @brief Returns last-opened preferences page index.
		 * @return Preferences page index.
		 */
		[[nodiscard]] int                   lastPreferencesPage() const;
		/**
		 * @brief Stores last-opened preferences page index.
		 * @param page Preferences page index.
		 */
		void                                setLastPreferencesPage(int page);
		/**
		 * @brief Returns last expanded trigger-tree group in world preferences.
		 * @return Last expanded trigger group label.
		 */
		[[nodiscard]] QString               lastTriggerTreeExpandedGroup() const;
		/**
		 * @brief Stores last expanded trigger-tree group in world preferences.
		 * @param group Group label to persist.
		 */
		void                                setLastTriggerTreeExpandedGroup(const QString &group);
		/**
		 * @brief Returns last expanded alias-tree group in world preferences.
		 * @return Last expanded alias group label.
		 */
		[[nodiscard]] QString               lastAliasTreeExpandedGroup() const;
		/**
		 * @brief Stores last expanded alias-tree group in world preferences.
		 * @param group Group label to persist.
		 */
		void                                setLastAliasTreeExpandedGroup(const QString &group);
		/**
		 * @brief Returns last expanded timer-tree group in world preferences.
		 * @return Last expanded timer group label.
		 */
		[[nodiscard]] QString               lastTimerTreeExpandedGroup() const;
		/**
		 * @brief Stores last expanded timer-tree group in world preferences.
		 * @param group Group label to persist.
		 */
		void                                setLastTimerTreeExpandedGroup(const QString &group);
		/**
		 * @brief Returns connection start timestamp.
		 * @return Connection start time.
		 */
		[[nodiscard]] QDateTime             connectTime() const;
		/**
		 * @brief Sets disconnect-status flag.
		 * @param ok Disconnect status flag.
		 */
		void                                setDisconnectOk(bool ok);
		/**
		 * @brief Returns disconnect-status flag.
		 * @return Disconnect status flag.
		 */
		[[nodiscard]] bool                  disconnectOk() const;
		/**
		 * @brief Enables/disables auto-reconnect on link failure.
		 * @param enabled Enable auto-reconnect when `true`.
		 */
		void                                setReconnectOnLinkFailure(bool enabled);
		/**
		 * @brief Returns auto-reconnect setting.
		 * @return Auto-reconnect flag.
		 */
		[[nodiscard]] bool                  reconnectOnLinkFailure() const;
		/**
		 * @brief Returns timestamp of last status update.
		 * @return Last status timestamp.
		 */
		[[nodiscard]] QDateTime             statusTime() const;
		/**
		 * @brief Resets status timestamp to current time.
		 */
		void                                resetStatusTime();
		/**
		 * @brief Returns last output/log flush timestamp.
		 * @return Last flush timestamp.
		 */
		[[nodiscard]] QDateTime             lastFlushTime() const;
		/**
		 * @brief Returns watched script file modification timestamp.
		 * @return Script-file modification timestamp.
		 */
		[[nodiscard]] QDateTime             scriptFileModTime() const;
		/**
		 * @brief Returns client startup timestamp.
		 * @return Client startup timestamp.
		 */
		[[nodiscard]] QDateTime             clientStartTime() const;
		/**
		 * @brief Returns world runtime startup timestamp.
		 * @return World startup timestamp.
		 */
		[[nodiscard]] QDateTime             worldStartTime() const;
		/**
		 * @brief Overrides stored client startup timestamp.
		 * @param when Startup timestamp to store.
		 */
		void                                setClientStartTime(const QDateTime &when);
		/**
		 * @brief Persistence paths, save/load operations, and option mirrors.
		 */
		/**
		 * @brief Sets world file path used by save/load operations.
		 * @param path World file path.
		 */
		void                                setWorldFilePath(const QString &path);
		/**
		 * @brief Returns world file path used by save/load.
		 * @return World file path.
		 */
		[[nodiscard]] QString               worldFilePath() const;
		/**
		 * @brief Saves world file synchronously.
		 * @param fileName Destination world file path.
		 * @param error Optional output error message.
		 * @return `true` when save succeeds.
		 */
		bool                                saveWorldFile(const QString &fileName, QString *error = nullptr);
		/**
		 * @brief Saves world file asynchronously.
		 * @param fileName Destination world file path.
		 * @param completion Completion callback with success flag and error text.
		 */
		void                                saveWorldFileAsync(const QString                             &fileName,
		                                                       std::function<void(bool, const QString &)> completion);
		/**
		 * @brief Sets plugins directory path.
		 * @param path Plugins directory path.
		 */
		void                                setPluginsDirectory(const QString &path);
		/**
		 * @brief Returns plugins directory path.
		 * @return Plugins directory path.
		 */
		[[nodiscard]] QString               pluginsDirectory() const;
		/**
		 * @brief Sets plugin-state files directory path.
		 * @param path Plugin-state files directory path.
		 */
		void                                setStateFilesDirectory(const QString &path);
		/**
		 * @brief Returns plugin-state files directory path.
		 * @return Plugin-state files directory path.
		 */
		[[nodiscard]] QString               stateFilesDirectory() const;
		/**
		 * @brief Sets last-used file-browsing directory.
		 * @param path File-browsing directory path.
		 */
		void                                setFileBrowsingDirectory(const QString &path);
		/**
		 * @brief Returns last-used file-browsing directory.
		 * @return File-browsing directory path.
		 */
		[[nodiscard]] QString               fileBrowsingDirectory() const;
		/**
		 * @brief Sets preferences database filename/path.
		 * @param path Preferences database path.
		 */
		void                                setPreferencesDatabaseName(const QString &path);
		/**
		 * @brief Returns preferences database filename/path.
		 * @return Preferences database path.
		 */
		[[nodiscard]] QString               preferencesDatabaseName() const;
		/**
		 * @brief Sets translation catalog file path.
		 * @param path Translation catalog file path.
		 */
		void                                setTranslatorFile(const QString &path);
		/**
		 * @brief Returns translation catalog file path.
		 * @return Translation catalog file path.
		 */
		[[nodiscard]] QString               translatorFile() const;
		/**
		 * @brief Sets locale identifier.
		 * @param value Locale identifier.
		 */
		void                                setLocale(const QString &value);
		/**
		 * @brief Returns locale identifier.
		 * @return Locale identifier.
		 */
		[[nodiscard]] QString               locale() const;
		/**
		 * @brief Sets configured fixed-pitch font family.
		 * @param value Fixed-pitch font family name.
		 */
		void                                setFixedPitchFont(const QString &value);
		/**
		 * @brief Returns configured fixed-pitch font family.
		 * @return Fixed-pitch font family name.
		 */
		[[nodiscard]] QString               fixedPitchFont() const;
		/**
		 * @brief Applies default world option values.
		 */
		void                                applyDefaultWorldOptions();
		/**
		 * @brief Sets runtime status message text.
		 * @param value Status message text.
		 */
		void                                setStatusMessage(const QString &value);
		/**
		 * @brief Returns runtime status message text.
		 * @return Status message text.
		 */
		[[nodiscard]] QString               statusMessage() const;
		/**
		 * @brief Sets cached word-under-menu text.
		 * @param value Word-under-menu text.
		 */
		void                                setWordUnderMenu(const QString &value);
		/**
		 * @brief Returns cached word-under-menu text.
		 * @return Word-under-menu text.
		 */
		[[nodiscard]] QString               wordUnderMenu() const;
		/**
		 * @brief Enables/disables incoming-packet debug.
		 * @param enabled Enable packet debug when `true`.
		 */
		void                                setDebugIncomingPackets(bool enabled);
		/**
		 * @brief Returns incoming-packet debug flag.
		 * @return Packet debug flag.
		 */
		[[nodiscard]] bool                  debugIncomingPackets() const;
		/**
		 * @brief Stores last evaluated immediate-expression text.
		 * @param value Immediate-expression text.
		 */
		void                                setLastImmediateExpression(const QString &value);
		/**
		 * @brief Returns last evaluated immediate-expression text.
		 * @return Immediate-expression text.
		 */
		[[nodiscard]] QString               lastImmediateExpression() const;
		/**
		 * @brief Marks variable set dirty/clean.
		 * @param changed Dirty flag value.
		 */
		void                                setVariablesChanged(bool changed);
		/**
		 * @brief Returns variable-dirty flag.
		 * @return Variable set dirty flag.
		 */
		[[nodiscard]] bool                  variablesChanged() const;
		/**
		 * @brief Marks current line as omitted from output.
		 * @param omitted Omitted flag.
		 */
		void                                setLineOmittedFromOutput(bool omitted);
		/**
		 * @brief Returns omitted-line flag.
		 * @return Omitted-line flag.
		 */
		[[nodiscard]] bool                  lineOmittedFromOutput() const;
		/**
		 * @brief Pushes line to recent-line history.
		 * @param line Line text.
		 */
		void                                addRecentLine(const QString &line);
		/**
		 * @brief Returns recent-line history.
		 * @param maxCount Maximum number of lines, or `-1` for all.
		 * @return Recent-line list.
		 */
		[[nodiscard]] QStringList           recentLines(int maxCount = -1) const;
		/**
		 * @brief Clears recent-line history.
		 */
		void                                clearRecentLines();
		/**
		 * @brief Sets/clears bookmark flag on output line.
		 * @param lineNumber Zero-based output line number.
		 * @param set Set bookmark when `true`, clear otherwise.
		 */
		void                                bookmarkLine(int lineNumber, bool set);
		/**
		 * @brief Sets trigger-evaluation stop mode.
		 * @param mode New stop-evaluation mode.
		 */
		void                                setStopTriggerEvaluation(StopTriggerEvaluation mode);
		/**
		 * @brief Returns trigger-evaluation stop mode.
		 * @return Current stop-evaluation mode.
		 */
		[[nodiscard]] StopTriggerEvaluation stopTriggerEvaluation() const;
		/**
		 * @brief Returns Lua callback engine pointer.
		 * @return Lua callback engine pointer.
		 */
		[[nodiscard]] LuaCallbackEngine    *luaCallbacks() const;
		/**
		 * @brief Applies Lua package-loading restrictions.
		 * @param enablePackage Enable package access when `true`.
		 */
		void                                applyPackageRestrictions(bool enablePackage);
		/**
		 * @brief Enables/disables trace output.
		 * @param enabled Enable trace when `true`.
		 */
		void                                setTraceEnabled(bool enabled);
		/**
		 * @brief Returns trace output flag.
		 * @return Trace-enabled flag.
		 */
		[[nodiscard]] bool                  traceEnabled() const;
		/**
		 * @brief Marks world document dirty/clean.
		 * @param modified Dirty flag.
		 */
		void                                setWorldFileModified(bool modified);
		/**
		 * @brief Returns world document dirty flag.
		 * @return World-file dirty flag.
		 */
		[[nodiscard]] bool                  worldFileModified() const;
		/**
		 * @brief Returns low-level newline count from network stream.
		 * @return Received newline count.
		 */
		[[nodiscard]] qint64                newlinesReceived() const;
		/**
		 * @brief Sets script-file-changed flag.
		 * @param changed Script-file-changed flag.
		 */
		void                                setScriptFileChanged(bool changed);
		/**
		 * @brief Returns script-file-changed flag.
		 * @return Script-file-changed flag.
		 */
		[[nodiscard]] bool                  scriptFileChanged() const;
		/**
		 * @brief Stores last command text sent to world.
		 * @param value Last command text.
		 */
		void                                setLastCommandSent(const QString &value);
		/**
		 * @brief Returns last command text sent to world.
		 * @return Last command text.
		 */
		[[nodiscard]] QString               lastCommandSent() const;
		/**
		 * @brief Returns startup working directory.
		 * @return Startup directory path.
		 */
		[[nodiscard]] QString               startupDirectory() const;
		/**
		 * @brief Returns default world-files directory.
		 * @return Default world directory path.
		 */
		[[nodiscard]] QString               defaultWorldDirectory() const;
		/**
		 * @brief Returns default log-files directory.
		 * @return Default log directory path.
		 */
		[[nodiscard]] QString               defaultLogDirectory() const;
		/**
		 * @brief Returns whether MXP mode is active.
		 * @return `true` when MXP mode is active.
		 */
		[[nodiscard]] bool                  isMxpActive() const;
		/**
		 * @brief Returns accumulated MXP error count.
		 * @return MXP error count.
		 */
		[[nodiscard]] int                   mxpErrorCount() const;
		/**
		 * @brief Enables/disables output freeze.
		 * @param frozen Freeze output when `true`.
		 */
		void                                setOutputFrozen(bool frozen);
		/**
		 * @brief Returns output freeze state.
		 * @return `true` when output is frozen.
		 */
		[[nodiscard]] bool                  outputFrozen() const;
		/**
		 * @brief Returns text rectangle drawing settings.
		 * @return Text rectangle settings.
		 */
		[[nodiscard]] const TextRectangleSettings &textRectangle() const;
		/**
		 * @brief Sets text rectangle drawing settings.
		 * @param settings New text rectangle settings.
		 */
		void                                       setTextRectangle(const TextRectangleSettings &settings);
		/**
		 * @brief Returns list of active UDP listener ports.
		 * @return Active UDP ports.
		 */
		[[nodiscard]] QList<int>                   udpPortList() const;
		/**
		 * @brief UDP and sound utility APIs.
		 */
		/**
		 * @brief Starts UDP listener with script callback.
		 * @param pluginId Owning plugin id.
		 * @param ip Bind IP address.
		 * @param port Bind port.
		 * @param script Callback script/routine name.
		 * @return API status code.
		 */
		int        udpListen(const QString &pluginId, const QString &ip, int port, const QString &script);
		/**
		 * @brief Sends UDP payload to destination.
		 * @param ip Destination IP address.
		 * @param port Destination port.
		 * @param payload UDP payload bytes.
		 * @return API status code.
		 */
		static int udpSend(const QString &ip, int port, const QByteArray &payload);
		/**
		 * @brief Returns last received telnet subnegotiation.
		 * @return Last telnet subnegotiation payload.
		 */
		[[nodiscard]] QString lastTelnetSubnegotiation() const;
		/**
		 * @brief Plays sound file in buffer slot.
		 * @param buffer Sound buffer slot index.
		 * @param fileName Sound file path.
		 * @param loop Loop playback when `true`.
		 * @param volume Playback volume.
		 * @param pan Stereo pan value.
		 * @return API status code.
		 */
		int playSound(int buffer, const QString &fileName, bool loop, double volume, double pan);
		/**
		 * @brief Plays sound from memory bytes.
		 * @param buffer Sound buffer slot index.
		 * @param data Encoded audio bytes.
		 * @param loop Loop playback when `true`.
		 * @param volume Playback volume.
		 * @param pan Stereo pan value.
		 * @return API status code.
		 */
		int playSoundMemory(int buffer, const QByteArray &data, bool loop, double volume, double pan);
		/**
		 * @brief Stops playback in buffer slot.
		 * @param buffer Sound buffer slot index.
		 * @return API status code.
		 */
		int stopSound(int buffer);
		/**
		 * @brief Returns playback status for buffer slot.
		 * @param buffer Sound buffer slot index.
		 * @return Playback status code.
		 */
		[[nodiscard]] int              soundStatus(int buffer) const;
		/**
		 * @brief Static helper APIs exposed to scripting/commands.
		 */
		/**
		 * @brief Adjusts colour value using a transformation method.
		 * @param colour Input colour value.
		 * @param method Adjustment method code.
		 * @return Adjusted colour value.
		 */
		static long                    adjustColour(long colour, short method);
		/**
		 * @brief Blends two pixels with blend mode and opacity.
		 * @param blend Blend/source colour.
		 * @param base Base/destination colour.
		 * @param mode Blend mode code.
		 * @param opacity Blend opacity.
		 * @return Blended colour value.
		 */
		static long                    blendPixel(long blend, long base, short mode, double opacity);
		/**
		 * @brief Applies pixel filter operation.
		 * @param pixel Input colour value.
		 * @param operation Filter operation code.
		 * @param options Filter options value.
		 * @return Filtered colour value.
		 */
		static long                    filterPixel(long pixel, short operation, double options);
		/**
		 * @brief Encodes text as Base64.
		 * @param text Input text.
		 * @param multiLine Emit multiline Base64 when `true`.
		 * @return Base64-encoded text.
		 */
		static QString                 base64Encode(const QString &text, bool multiLine);
		/**
		 * @brief Decodes Base64 text.
		 * @param text Base64 text.
		 * @return Decoded text.
		 */
		static QString                 base64Decode(const QString &text);
		/**
		 * @brief Converts RGB value to canonical colour name.
		 * @param colour RGB colour value.
		 * @return Colour name.
		 */
		static QString                 rgbColourToName(long colour);
		/**
		 * @brief Returns client version string.
		 * @return Client version string.
		 */
		static QString                 clientVersionString();
		/**
		 * @brief Returns text description for error code.
		 * @param code Error/status code.
		 * @return Human-readable error description.
		 */
		static QString                 errorDesc(int code);
		/**
		 * @brief Generates random fantasy-style name.
		 * @return Generated name.
		 */
		static QString                 generateName();
		/**
		 * @brief Returns list of built-in internal command names.
		 * @return Internal command names.
		 */
		static QStringList             internalCommandsList();
		/**
		 * @brief Returns mapped colour replacement or original.
		 * @param value Input colour value.
		 * @return Mapped colour value.
		 */
		[[nodiscard]] long             getMapColour(long value) const;
		/**
		 * @brief Returns mapped system colour value.
		 * @param index System color index.
		 * @return RGB colour value.
		 */
		static long                    getSysColor(int index);
		/**
		 * @brief Returns process-wide unique numeric id.
		 * @return Unique numeric id.
		 */
		static long                    getUniqueNumber();
		/**
		 * @brief Escapes plain text into regex-safe expression.
		 * @param text Plain text.
		 * @return Regex-safe text.
		 */
		static QString                 makeRegularExpression(const QString &text);
		/**
		 * @brief Computes metaphone code.
		 * @param text Input text.
		 * @param length Requested metaphone length.
		 * @return Metaphone code.
		 */
		static QString                 metaphone(const QString &text, int length);
		/**
		 * @brief Returns random floating-point value.
		 * @return Random number.
		 */
		static double                  mtRand();
		/**
		 * @brief Loads external names source file.
		 * @param fileName Names file path.
		 * @return API status code.
		 */
		static int                     readNamesFile(const QString &fileName);
		/**
		 * @brief Stores output-font metrics.
		 * @param height Output font height.
		 * @param width Output font width.
		 */
		void                           setOutputFontMetrics(int height, int width);
		/**
		 * @brief Stores input-font metrics.
		 * @param height Input font height.
		 * @param width Input font width.
		 */
		void                           setInputFontMetrics(int height, int width);
		/**
		 * @brief Returns output-font height.
		 * @return Output font height in pixels.
		 */
		[[nodiscard]] int              outputFontHeight() const;
		/**
		 * @brief Returns output-font width.
		 * @return Output font width in pixels.
		 */
		[[nodiscard]] int              outputFontWidth() const;
		/**
		 * @brief Returns input-font height.
		 * @return Input font height in pixels.
		 */
		[[nodiscard]] int              inputFontHeight() const;
		/**
		 * @brief Returns input-font width.
		 * @return Input font width in pixels.
		 */
		[[nodiscard]] int              inputFontWidth() const;
		/**
		 * @brief Sets queued-command count.
		 * @param count Queued-command count value.
		 */
		void                           setQueuedCommandCount(int count);
		/**
		 * @brief Converts color name to RGB value.
		 * @param name Colour name.
		 * @return RGB colour value.
		 */
		static long                    colourNameToRGB(const QString &name);
		/**
		 * @brief Sets custom note text color entry.
		 * @param index Custom color index.
		 * @param color Text colour value.
		 * @return API status code.
		 */
		int                            setCustomColourText(int index, const QColor &color);
		/**
		 * @brief Sets custom note background color entry.
		 * @param index Custom color index.
		 * @param color Background colour value.
		 * @return API status code.
		 */
		int                            setCustomColourBackground(int index, const QColor &color);
		/**
		 * @brief Sets custom color display name.
		 * @param index Custom color index.
		 * @param name Display name.
		 * @return API status code.
		 */
		int                            setCustomColourName(int index, const QString &name);
		/**
		 * @brief Returns custom note text color entry.
		 * @param index Custom color index.
		 * @return Text colour RGB value.
		 */
		[[nodiscard]] long             customColourText(int index) const;
		/**
		 * @brief Returns custom note background color entry.
		 * @param index Custom color index.
		 * @return Background colour RGB value.
		 */
		[[nodiscard]] long             customColourBackground(int index) const;
		/**
		 * @brief Loads world output background image.
		 * @param fileName Image file path.
		 * @param mode Draw mode code.
		 * @return API status code.
		 */
		int                            setBackgroundImage(const QString &fileName, int mode);
		/**
		 * @brief Loads world output foreground image.
		 * @param fileName Image file path.
		 * @param mode Draw mode code.
		 * @return API status code.
		 */
		int                            setForegroundImage(const QString &fileName, int mode);
		/**
		 * @brief Returns configured background image.
		 * @return Background image.
		 */
		[[nodiscard]] QImage           backgroundImage() const;
		/**
		 * @brief Returns configured foreground image.
		 * @return Foreground image.
		 */
		[[nodiscard]] QImage           foregroundImage() const;
		/**
		 * @brief Returns background image draw mode.
		 * @return Background image mode code.
		 */
		[[nodiscard]] int              backgroundImageMode() const;
		/**
		 * @brief Returns foreground image draw mode.
		 * @return Foreground image mode code.
		 */
		[[nodiscard]] int              foregroundImageMode() const;
		/**
		 * @brief Returns background image source name.
		 * @return Background image source name.
		 */
		[[nodiscard]] QString          backgroundImageName() const;
		/**
		 * @brief Returns foreground image source name.
		 * @return Foreground image source name.
		 */
		[[nodiscard]] QString          foregroundImageName() const;
		/**
		 * @brief Requests close of a named notepad window.
		 * @param title Notepad title.
		 * @param querySave Prompt to save when `true`.
		 * @return `true` when close is accepted.
		 */
		bool                           closeNotepad(const QString &title, bool querySave);
		/**
		 * @brief Executes debug command and returns result payload.
		 * @param command Debug command text.
		 * @return Command result payload.
		 */
		QVariant                       debugCommand(const QString &command);
		/**
		 * @brief Returns queued-command count.
		 * @return Queued-command count.
		 */
		[[nodiscard]] int              queuedCommandCount() const;
		/**
		 * @brief Returns bytes received from socket.
		 * @return Received byte count.
		 */
		[[nodiscard]] qint64           bytesIn() const;
		/**
		 * @brief Returns bytes sent to socket.
		 * @return Sent byte count.
		 */
		[[nodiscard]] qint64           bytesOut() const;
		/**
		 * @brief Returns input packet counter.
		 * @return Input packet count.
		 */
		[[nodiscard]] int              inputPacketCount() const;
		/**
		 * @brief Returns output packet counter.
		 * @return Output packet count.
		 */
		[[nodiscard]] int              outputPacketCount() const;
		/**
		 * @brief Returns compressed-byte total for MCCP.
		 * @return Total compressed bytes.
		 */
		[[nodiscard]] qint64           totalCompressedBytes() const;
		/**
		 * @brief Returns uncompressed-byte total for MCCP.
		 * @return Total uncompressed bytes.
		 */
		[[nodiscard]] qint64           totalUncompressedBytes() const;
		/**
		 * @brief Returns active MCCP type code.
		 * @return MCCP type code.
		 */
		[[nodiscard]] int              mccpType() const;
		/**
		 * @brief Returns processed MXP tag count.
		 * @return MXP tag count.
		 */
		[[nodiscard]] qint64           mxpTagCount() const;
		/**
		 * @brief Returns processed MXP entity count.
		 * @return MXP entity count.
		 */
		[[nodiscard]] qint64           mxpEntityCount() const;
		/**
		 * @brief Returns custom MXP element count.
		 * @return Custom element count.
		 */
		[[nodiscard]] int              customElementCount() const;
		/**
		 * @brief Returns custom MXP entity count.
		 * @return Custom entity count.
		 */
		[[nodiscard]] int              customEntityCount() const;
		/**
		 * @brief Returns remote peer address string.
		 * @return Peer address string.
		 */
		[[nodiscard]] QString          peerAddressString() const;
		/**
		 * @brief Returns remote peer IPv4 address.
		 * @return Peer IPv4 address.
		 */
		[[nodiscard]] quint32          peerAddressV4() const;
		/**
		 * @brief Returns proxy address string.
		 * @return Proxy address string.
		 */
		[[nodiscard]] QString          proxyAddressString() const;
		/**
		 * @brief Returns proxy IPv4 address.
		 * @return Proxy IPv4 address.
		 */
		[[nodiscard]] quint32          proxyAddressV4() const;
		/**
		 * @brief Returns whether MCCP compression is active.
		 * @return `true` when compression is active.
		 */
		[[nodiscard]] bool             isCompressing() const;
		/**
		 * @brief Returns whether Pueblo mode is active.
		 * @return `true` when Pueblo mode is active.
		 */
		[[nodiscard]] bool             isPuebloActive() const;
		/**
		 * @brief Returns whether connection is in-progress.
		 * @return `true` when connecting.
		 */
		[[nodiscard]] bool             isConnecting() const;
		/**
		 * @brief Returns whether mapper capture is active.
		 * @return `true` when mapping is active.
		 */
		[[nodiscard]] bool             isMapping() const;
		/**
		 * @brief Returns mapper item count.
		 * @return Mapper item count.
		 */
		[[nodiscard]] int              mappingCount() const;
		/**
		 * @brief Returns mapper item by index.
		 * @param index Zero-based mapper item index.
		 * @return Mapper item text.
		 */
		[[nodiscard]] QString          mappingItem(int index) const;
		/**
		 * @brief Returns mapper history as string.
		 * @param omitComments Omit comment entries when `true`.
		 * @return Mapper history string.
		 */
		[[nodiscard]] QString          mappingString(bool omitComments = false) const;
		/**
		 * @brief Returns note-style bitmask.
		 * @return Note-style bitmask.
		 */
		[[nodiscard]] unsigned short   noteStyle() const;
		/**
		 * @brief Sets note-style bitmask.
		 * @param style Note-style bitmask.
		 */
		void                           setNoteStyle(unsigned short style);
		/**
		 * @brief Returns mapped color for original color.
		 * @param original Original color value.
		 * @return Mapped colour value.
		 */
		[[nodiscard]] long             mapColourValue(long original) const;
		/**
		 * @brief Sets color translation mapping entry.
		 * @param original Original color value.
		 * @param replacement Replacement color value.
		 */
		void                           mapColour(long original, long replacement);
		/**
		 * @brief Returns full colour translation map.
		 * @return Colour translation map.
		 */
		[[nodiscard]] QMap<long, long> mapColourList() const;
		/**
		 * @brief Returns normal ANSI color by index.
		 * @param index ANSI colour index.
		 * @return ANSI colour value.
		 */
		[[nodiscard]] long             normalColour(int index) const;
		/**
		 * @brief Sets normal ANSI color by index.
		 * @param index ANSI colour index.
		 * @param value ANSI colour value.
		 */
		void                           setNormalColour(int index, long value);
		/**
		 * @brief Returns whether notes use RGB mode.
		 * @return `true` when notes are in RGB mode.
		 */
		[[nodiscard]] bool             notesInRgb() const;
		/**
		 * @brief Returns note text colour index.
		 * @return Note text colour index.
		 */
		[[nodiscard]] int              noteTextColour() const;
		/**
		 * @brief Sets note text colour index.
		 * @param value Note text colour index.
		 */
		void                           setNoteTextColour(int value);
		/**
		 * @brief Returns note foreground RGB value.
		 * @return Note foreground color.
		 */
		[[nodiscard]] long             noteColourFore() const;
		/**
		 * @brief Returns note background RGB value.
		 * @return Note background color.
		 */
		[[nodiscard]] long             noteColourBack() const;
		/**
		 * @brief Sets note foreground RGB value.
		 * @param value Note foreground color.
		 */
		void                           setNoteColourFore(long value);
		/**
		 * @brief Sets note background RGB value.
		 * @param value Note background color.
		 */
		void                           setNoteColourBack(long value);
		/**
		 * @brief Adds trigger execution time sample.
		 * @param ns Trigger duration in nanoseconds.
		 */
		void                           addTriggerTimeNs(qint64 ns);
		/**
		 * @brief Adds alias execution time sample.
		 * @param ns Alias duration in nanoseconds.
		 */
		void                           addAliasTimeNs(qint64 ns);
		/**
		 * @brief Returns cumulative trigger execution time.
		 * @return Trigger execution time in seconds.
		 */
		[[nodiscard]] double           triggerTimeSeconds() const;
		/**
		 * @brief Returns cumulative alias execution time.
		 * @return Alias execution time in seconds.
		 */
		[[nodiscard]] double           aliasTimeSeconds() const;
		/**
		 * @brief Returns mapper reverse-step filtering flag.
		 * @return `true` when reverse-step filtering is enabled.
		 */
		[[nodiscard]] bool             removeMapReverses() const;
		/**
		 * @brief Enables/disables mapper reverse-step filtering.
		 * @param enabled Enable filtering when `true`.
		 */
		void                           setRemoveMapReverses(bool enabled);
		/**
		 * @brief Returns last goto-line value.
		 * @return Last goto-line number.
		 */
		[[nodiscard]] int              lastGoToLine() const;
		/**
		 * @brief Sets last goto-line value.
		 * @param line Last goto-line number.
		 */
		void                           setLastGoToLine(int line);
		/**
		 * @brief Fires OnPluginWorldConnect callbacks.
		 */
		void                           fireWorldConnectHandlers();
		/**
		 * @brief Fires OnPluginWorldDisconnect callbacks.
		 */
		void                           fireWorldDisconnectHandlers();
		/**
		 * @brief Fires OnPluginWorldOpen callbacks.
		 */
		void                           fireWorldOpenHandlers();
		/**
		 * @brief Fires OnPluginWorldClose callbacks.
		 */
		void                           fireWorldCloseHandlers();
		/**
		 * @brief Fires OnPluginWorldSave callbacks.
		 */
		void                           fireWorldSaveHandlers();
		/**
		 * @brief Fires OnPluginWorldGetFocus callbacks.
		 */
		void                           fireWorldGetFocusHandlers();
		/**
		 * @brief Fires OnPluginWorldLoseFocus callbacks.
		 */
		void                           fireWorldLoseFocusHandlers();
		/**
		 * @brief Records and emits MXP error.
		 * @param level Error severity level.
		 * @param messageNumber MXP message id.
		 * @param message Error text.
		 */
		void                           mxpError(int level, long messageNumber, const QString &message);
		/**
		 * @brief Replaces main world Lua script source text.
		 * @param script Lua script text.
		 */
		void                           setLuaScriptText(const QString &script);
		/**
		 * @brief Sends text to world socket.
		 * @param text Text to send.
		 * @param addNewline Append newline when `true`.
		 */
		void                           sendText(const QString &text, bool addNewline = true);
		/**
		 * @brief Emits plain output text to view.
		 * @param text Output text.
		 * @param note Emit as note when `true`.
		 * @param newLine Append newline when `true`.
		 */
		void                           outputText(const QString &text, bool note, bool newLine);
		/**
		 * @brief Emits styled output text.
		 * @param text Output text.
		 * @param spans Style spans.
		 * @param note Emit as note when `true`.
		 * @param newLine Append newline when `true`.
		 */
		void outputStyledText(const QString &text, const QVector<StyleSpan> &spans, bool note, bool newLine);
		/**
		 * @brief Emits HTML output fragment to view.
		 * @param html HTML fragment.
		 */
		void outputHtml(const QString &html);
		/**
		 * @brief Sends command through runtime command pipeline.
		 * @param text Command text.
		 * @param echo Echo command when `true`.
		 * @param queue Queue command when `true`.
		 * @param log Log command when `true`.
		 * @param history Add command to history when `true`.
		 * @param immediate Send as immediate when `true`.
		 * @return API status code.
		 */
		[[nodiscard]] int  sendCommand(const QString &text, bool echo, bool queue, bool log, bool history,
		                               bool immediate) const;
		/**
		 * @brief Command-processor bridge, accelerators, and plugin callbacks.
		 */
		/**
		 * @brief Logs outgoing input command line.
		 * @param text Command text.
		 */
		void               logInputCommand(const QString &text) const;
		/**
		 * @brief Executes command bound to accelerator id.
		 * @param commandId Accelerator command id.
		 * @param keyLabel Optional key label text.
		 * @return `true` when a command was executed.
		 */
		bool               executeAcceleratorCommand(int commandId, const QString &keyLabel = QString());
		/**
		 * @brief Enables/disables command echo suppression.
		 * @param enabled Enable no-echo command mode when `true`.
		 */
		void               setNoCommandEcho(bool enabled) const;
		/**
		 * @brief Returns command echo suppression state.
		 * @return `true` when command echo is suppressed.
		 */
		[[nodiscard]] bool noCommandEcho() const;
		/**
		 * @brief Syncs chat-listening state with preferences.
		 */
		void               syncChatAcceptCallsWithPreferences();
		/**
		 * @brief Binds runtime command processor instance.
		 * @param processor Command processor pointer.
		 */
		void               setCommandProcessor(WorldCommandProcessor *processor);
		/**
		 * @brief Re-applies runtime options into command processor.
		 */
		void               refreshCommandProcessorOptions();
		/**
		 * @brief Returns current queued command list.
		 * @return Queued command list.
		 */
		[[nodiscard]] QStringList queuedCommands() const;
		/**
		 * @brief Allocates next dynamic accelerator command id.
		 * @return Newly allocated command id.
		 */
		int                       allocateAcceleratorCommand();
		/**
		 * @brief Registers accelerator binding.
		 * @param key Accelerator key value.
		 * @param commandId Accelerator command id.
		 * @param entry Accelerator metadata entry.
		 */
		void               registerAccelerator(qint64 key, int commandId, const AcceleratorEntry &entry);
		/**
		 * @brief Removes accelerator binding by key.
		 * @param key Accelerator key value.
		 */
		void               removeAccelerator(qint64 key);
		/**
		 * @brief Resolves command id for accelerator key.
		 * @param key Accelerator key value.
		 * @return Command id, or 0 when unbound.
		 */
		[[nodiscard]] int  acceleratorCommandForKey(qint64 key) const;
		/**
		 * @brief Returns whether command id has accelerator binding.
		 * @param commandId Accelerator command id.
		 * @return `true` when binding exists.
		 */
		[[nodiscard]] bool hasAccelerator(int commandId) const;
		/**
		 * @brief Returns accelerator entry for command id.
		 * @param commandId Accelerator command id.
		 * @return Accelerator entry pointer, or `nullptr`.
		 */
		[[nodiscard]] const AcceleratorEntry *acceleratorEntryForCommand(int commandId) const;
		/**
		 * @brief Returns all registered accelerator keys.
		 * @return Registered accelerator keys.
		 */
		[[nodiscard]] QVector<qint64>         acceleratorKeys() const;
		/**
		 * @brief Returns text payload for accelerator command.
		 * @param commandId Accelerator command id.
		 * @return Accelerator command text.
		 */
		[[nodiscard]] QString                 acceleratorCommandText(int commandId) const;
		/**
		 * @brief Returns send-target mode for accelerator command.
		 * @param commandId Accelerator command id.
		 * @return Send-target mode code.
		 */
		[[nodiscard]] int                     acceleratorSendTarget(int commandId) const;
		/**
		 * @brief Returns plugin id that owns accelerator command.
		 * @param commandId Accelerator command id.
		 * @return Owning plugin id.
		 */
		[[nodiscard]] QString                 acceleratorPluginId(int commandId) const;
		/**
		 * @brief Fires plugin command callback chain.
		 * @param text Command text.
		 * @return `true` when processing should continue.
		 */
		bool                                  firePluginCommand(const QString &text);
		/**
		 * @brief Fires plugin command-changed callback chain.
		 */
		void                                  firePluginCommandChanged();
		/**
		 * @brief Fires plugin send callback chain.
		 * @param text Outgoing text.
		 * @return `true` when processing should continue.
		 */
		bool                                  firePluginSend(const QString &text);
		/**
		 * @brief Fires plugin command-entered transform callback.
		 * @param text Command text, possibly modified in place.
		 */
		void                                  firePluginCommandEntered(QString &text);
		/**
		 * @brief Fires plugin tab-complete transform callback.
		 * @param text Completion text, possibly modified in place.
		 */
		void                                  firePluginTabComplete(QString &text);
		/**
		 * @brief Fires plugin line-received callback chain.
		 * @param text Received line text.
		 * @return `true` when line processing should continue.
		 */
		bool                                  firePluginLineReceived(const QString &text);
		/**
		 * @brief Fires plugin play-sound callback chain.
		 * @param sound Sound identifier or path.
		 * @return `true` when default handling should continue.
		 */
		bool                                  firePluginPlaySound(const QString &sound);
		/**
		 * @brief Fires plugin trace callback chain.
		 * @param message Trace message text.
		 * @return `true` when trace should be emitted.
		 */
		bool                                  firePluginTrace(const QString &message);
		/**
		 * @brief Fires plugin screen-draw callback.
		 * @param type Draw event type.
		 * @param log Log flag value.
		 * @param text Draw payload text.
		 */
		void                                  firePluginScreendraw(int type, int log, const QString &text);
		/**
		 * @brief Fires plugin periodic tick callback.
		 */
		void                                  firePluginTick();
		/**
		 * @brief Fires plugin sent-line callback.
		 * @param text Sent line text.
		 */
		void                                  firePluginSent(const QString &text);
		/**
		 * @brief Fires plugin partial-line callback.
		 * @param text Partial line text.
		 */
		void                                  firePluginPartialLine(const QString &text);
		/**
		 * @brief Dispatches miniwindow mouse-move notification.
		 * @param x Mouse x coordinate.
		 * @param y Mouse y coordinate.
		 * @param windowName Miniwindow name.
		 */
		void                  notifyMiniWindowMouseMoved(int x, int y, const QString &windowName);
		/**
		 * @brief Dispatches output-resized notification.
		 */
		void                  notifyWorldOutputResized();
		/**
		 * @brief Dispatches draw-output-window notification.
		 * @param firstLine First visible line index.
		 * @param offset Pixel offset.
		 */
		void                  notifyDrawOutputWindow(int firstLine, int offset);
		/**
		 * @brief Returns script-error output suppression flag.
		 * @return `true` when script errors are suppressed from world output.
		 */
		[[nodiscard]] bool    suppressScriptErrorOutputToWorld() const;
		/**
		 * @brief Returns draw-output notification count.
		 * @return Output-window redraw count.
		 */
		[[nodiscard]] int     outputWindowRedrawCount() const;
		/**
		 * @brief Dispatches output-selection-changed notification.
		 */
		void                  notifyOutputSelectionChanged();
		/**
		 * @brief Reloads plugin by id.
		 * @param pluginId Plugin id.
		 * @param error Optional output error message.
		 * @return API status code.
		 */
		int                   reloadPlugin(const QString &pluginId, QString *error = nullptr);
		/**
		 * @brief Starts MXP mode state.
		 */
		void                  mxpStartUp();
		/**
		 * @brief Stops MXP mode state.
		 */
		void                  mxpShutDown();
		/**
		 * @brief Resets MXP runtime state.
		 */
		void                  resetMxp();
		/**
		 * @brief Clears open/custom MXP tag state.
		 */
		void                  resetMxpTags();
		/**
		 * @brief Clears cached host/proxy IP state.
		 */
		void                  resetIpCache();
		/**
		 * @brief Recomputes and resets all timers.
		 */
		void                  resetAllTimers();
		/**
		 * @brief Returns whether IP cache currently exists.
		 * @return `true` when cached IP information is available.
		 */
		[[nodiscard]] bool    hasCachedIp() const;
		/**
		 * @brief MXP state and logging APIs.
		 */
		/**
		 * @brief Returns built-in MXP element definition count.
		 * @return Built-in MXP element count.
		 */
		static int            mxpBuiltinElementCount();
		/**
		 * @brief Returns built-in MXP entity definition count.
		 * @return Built-in MXP entity count.
		 */
		static int            mxpBuiltinEntityCount();
		/**
		 * @brief Returns currently open MXP tag count.
		 * @return Open MXP tag count.
		 */
		[[nodiscard]] int     mxpOpenTagCount() const;
		/**
		 * @brief Opens log file.
		 * @param logFileName Log file path.
		 * @param append Append when `true`, truncate otherwise.
		 * @return API status code.
		 */
		int                   openLog(const QString &logFileName, bool append);
		/**
		 * @brief Closes active log file.
		 * @return API status code.
		 */
		int                   closeLog();
		/**
		 * @brief Writes raw bytes to log file.
		 * @param bytes Bytes to write.
		 * @return API status code.
		 */
		int                   writeLog(const QByteArray &bytes);
		/**
		 * @brief Writes text to log file.
		 * @param text Text to write.
		 * @return API status code.
		 */
		int                   writeLog(const QString &text);
		/**
		 * @brief Flushes log file buffers.
		 * @return API status code.
		 */
		int                   flushLog();
		/**
		 * @brief Returns whether log file is open.
		 * @return `true` when log is open.
		 */
		[[nodiscard]] bool    isLogOpen() const;
		/**
		 * @brief Returns active log filename.
		 * @return Log file path.
		 */
		[[nodiscard]] QString logFileName() const;
		/**
		 * @brief Returns current log file offset.
		 * @return Log file position in bytes.
		 */
		[[nodiscard]] qint64  logFilePosition() const;
		/**
		 * @brief Formats time with world settings.
		 * @param time Time value.
		 * @param format Format string.
		 * @param fixHtml Escape/adjust HTML-sensitive output when `true`.
		 * @return Formatted time text.
		 */
		[[nodiscard]] QString formatTime(const QDateTime &time, const QString &format, bool fixHtml) const;
		/**
		 * @brief Sets default world directory.
		 * @param path Default world directory path.
		 */
		void                  setDefaultWorldDirectory(const QString &path);
		/**
		 * @brief Sets default log directory.
		 * @param path Default log directory path.
		 */
		void                  setDefaultLogDirectory(const QString &path);
		/**
		 * @brief Sets startup directory.
		 * @param path Startup directory path.
		 */
		void                  setStartupDirectory(const QString &path);
		/**
		 * @brief Sets world window title override.
		 * @param title Window title text.
		 */
		void                  setWindowTitleOverride(const QString &title);
		/**
		 * @brief Returns world window title override.
		 * @return World window title override.
		 */
		[[nodiscard]] QString windowTitleOverride() const;
		/**
		 * @brief Sets main window title override.
		 * @param title Main window title text.
		 */
		void                  setMainTitleOverride(const QString &title);
		/**
		 * @brief Returns main window title override.
		 * @return Main window title override.
		 */
		[[nodiscard]] QString mainTitleOverride() const;

	signals:
		/**
		 * @brief Emitted for line/output/socket/world state changes.
		 */
		/**
		 * @brief Emitted when full line text arrives.
		 * @param line Incoming line text.
		 */
		void incomingLineReceived(const QString &line);
		/**
		 * @brief Emitted when full styled line arrives.
		 * @param line Incoming line text.
		 * @param spans Style spans.
		 */
		void incomingStyledLineReceived(const QString &line, const QVector<WorldRuntime::StyleSpan> &spans);
		/**
		 * @brief Emitted when styled partial line arrives.
		 * @param line Partial line text.
		 * @param spans Style spans.
		 */
		void incomingStyledLinePartialReceived(const QString                          &line,
		                                       const QVector<WorldRuntime::StyleSpan> &spans);
		/**
		 * @brief Emitted when HTML output fragment arrives.
		 * @param html HTML fragment.
		 */
		void incomingHtmlReceived(const QString &html);
		/**
		 * @brief Emitted to append plain output.
		 * @param text Output text.
		 * @param newLine Append newline when `true`.
		 * @param note Emit as note when `true`.
		 */
		void outputRequested(const QString &text, bool newLine, bool note);
		/**
		 * @brief Emitted to append styled output.
		 * @param text Output text.
		 * @param spans Style spans.
		 * @param newLine Append newline when `true`.
		 * @param note Emit as note when `true`.
		 */
		void outputStyledRequested(const QString &text, const QVector<WorldRuntime::StyleSpan> &spans,
		                           bool newLine, bool note);
		/**
		 * @brief Emitted when log open state changes.
		 * @param open New log-open state.
		 */
		void logStateChanged(bool open);
		/**
		 * @brief Emitted for MXP debug trace.
		 * @param title Debug message title.
		 * @param message Debug message text.
		 */
		void mxpDebugMessage(const QString &title, const QString &message);
		/**
		 * @brief Emitted when socket error occurs.
		 * @param message Error text.
		 */
		void socketError(const QString &message);
		/**
		 * @brief Emitted when world connection is established.
		 */
		void connected();
		/**
		 * @brief Emitted when world connection is closed.
		 */
		void disconnected();
		/**
		 * @brief Emitted when world window title changes.
		 */
		void windowTitleChanged();
		/**
		 * @brief Emitted when main window title changes.
		 */
		void mainTitleChanged();
		/**
		 * @brief Emitted when miniwindow set changes.
		 */
		void miniWindowsChanged();
		/**
		 * @brief Emitted when watched script file changes.
		 */
		void scriptFileChangedDetected();
		/**
		 * @brief Emitted when world attribute changes.
		 * @param key Changed attribute key.
		 */
		void worldAttributeChanged(const QString &key);

	private:
		/**
		 * @brief Internal plugin-callback, chat, save-snapshot, and log-rotation helpers.
		 */
		/**
		 * @brief Runs plugin callbacks until one returns false.
		 * @param functionName Callback function name.
		 * @param payload Callback payload text.
		 * @return `true` when all callbacks returned true.
		 */
		bool callPluginCallbacksStopOnFalse(const QString &functionName, const QString &payload);
		/**
		 * @brief Runs plugin callbacks with string payload.
		 * @param functionName Callback function name.
		 * @param payload Callback payload text.
		 */
		void callPluginCallbacks(const QString &functionName, const QString &payload);
		/**
		 * @brief Runs plugin callbacks without arguments.
		 * @param functionName Callback function name.
		 */
		void callPluginCallbacksNoArgs(const QString &functionName);
		/**
		 * @brief Runs callbacks until one returns true.
		 * @param functionName Callback function name.
		 * @param arg1 Numeric callback argument.
		 * @param arg2 String callback argument.
		 * @return `true` when any callback returned true.
		 */
		bool callPluginCallbacksStopOnTrue(const QString &functionName, long arg1, const QString &arg2);
		/**
		 * @brief Runs callbacks until one returns true with string payload.
		 * @param functionName Callback function name.
		 * @param payload Callback payload text.
		 * @return `true` when any callback returned true.
		 */
		bool callPluginCallbacksStopOnTrueWithString(const QString &functionName, const QString &payload);
		/**
		 * @brief Applies plugin byte-transform callbacks in sequence.
		 * @param functionName Callback function name.
		 * @param payload Byte payload transformed in place.
		 */
		void callPluginCallbacksTransformBytes(const QString &functionName, QByteArray &payload);
		/**
		 * @brief Applies plugin string-transform callbacks in sequence.
		 * @param functionName Callback function name.
		 * @param payload String payload transformed in place.
		 */
		void callPluginCallbacksTransformString(const QString &functionName, QString &payload);
		/**
		 * @brief Pushes current terminal size via NAWS.
		 */
		void updateTelnetWindowSizeForNaws();
		/**
		 * @brief Resets ANSI rendering state machine.
		 */
		void resetAnsiRenderState();
		/**
		 * @brief Resets MXP rendering state machine.
		 */
		void resetMxpRenderState();
		/**
		 * @brief Runs callbacks with numeric and string arguments.
		 * @param functionName Callback function name.
		 * @param arg1 Numeric callback argument.
		 * @param arg2 String callback argument.
		 */
		void callPluginCallbacksWithNumberAndString(const QString &functionName, long arg1,
		                                            const QString &arg2);
		/**
		 * @brief Runs callbacks with byte payload.
		 * @param functionName Callback function name.
		 * @param payload Byte payload.
		 */
		void callPluginCallbacksWithBytes(const QString &functionName, const QByteArray &payload);
		/**
		 * @brief Runs byte callbacks until one returns true.
		 * @param functionName Callback function name.
		 * @param arg1 Numeric callback argument.
		 * @param payload Byte payload.
		 * @return `true` when any callback returned true.
		 */
		bool callPluginCallbacksStopOnTrueBytes(const QString &functionName, long arg1,
		                                        const QByteArray &payload);
		/**
		 * @brief Runs callbacks with numeric and byte arguments.
		 * @param functionName Callback function name.
		 * @param arg1 Numeric callback argument.
		 * @param payload Byte payload.
		 */
		void callPluginCallbacksWithNumberAndBytes(const QString &functionName, long arg1,
		                                           const QByteArray &payload);
		/**
		 * @brief Runs callbacks with two numbers and string.
		 * @param functionName Callback function name.
		 * @param arg1 First numeric callback argument.
		 * @param arg2 Second numeric callback argument.
		 * @param arg3 String callback argument.
		 */
		void callPluginCallbacksWithTwoNumbersAndString(const QString &functionName, long arg1, long arg2,
		                                                const QString &arg3);
		/**
		 * @brief Runs callbacks until one returns false.
		 * @param functionName Callback function name.
		 * @param arg1 Numeric callback argument.
		 * @param arg2 String callback argument.
		 * @return `true` when all callbacks returned true.
		 */
		bool callPluginCallbacksStopOnFalseWithNumberAndString(const QString &functionName, long arg1,
		                                                       const QString &arg2);
		/**
		 * @brief Runs callbacks until one returns false.
		 * @param functionName Callback function name.
		 * @param arg1 First numeric callback argument.
		 * @param arg2 Second numeric callback argument.
		 * @param arg3 String callback argument.
		 * @return `true` when all callbacks returned true.
		 */
		bool callPluginCallbacksStopOnFalseWithTwoNumbersAndString(const QString &functionName, long arg1,
		                                                           long arg2, const QString &arg3);
		/**
		 * @brief Queues plugin for deferred installation.
		 * @param plugin Plugin instance.
		 */
		void queuePluginInstall(Plugin &plugin);
		/**
		 * @brief Returns whether current Lua-context line is buffered.
		 * @return `true` when current Lua-context line is in buffer.
		 */
		[[nodiscard]] bool    luaContextLinePresentInBuffer() const;
		/**
		 * @brief Returns effective output line-limit setting.
		 * @return Effective output line limit.
		 */
		[[nodiscard]] int     maxOutputLinesLimit() const;
		/**
		 * @brief Trims output buffer to configured line limit.
		 */
		void                  enforceOutputLineLimit();
		/**
		 * @brief Returns resolved world name.
		 * @return Resolved world name.
		 */
		[[nodiscard]] QString worldName() const;
		/**
		 * @brief Returns effective local chat name.
		 * @return Local chat display name.
		 */
		QString               chatOurName();
		/**
		 * @brief Returns configured incoming chat port.
		 * @return Incoming chat port.
		 */
		[[nodiscard]] int     chatIncomingPort() const;
		/**
		 * @brief Returns best local address for chat announcements.
		 * @return Local address string.
		 */
		static QString        chatLocalAddress();
		/**
		 * @brief Returns incoming chat-call validation flag.
		 * @return `true` when incoming calls are validated.
		 */
		[[nodiscard]] bool    validateIncomingChatCalls() const;
		/**
		 * @brief Returns auto-allow incoming files setting.
		 * @return Auto-allow-files flag.
		 */
		[[nodiscard]] bool    autoAllowFiles() const;
		/**
		 * @brief Returns auto-allow snooping setting.
		 * @return Auto-allow-snooping flag.
		 */
		[[nodiscard]] bool    autoAllowSnooping() const;
		/**
		 * @brief Returns chat transfer save directory.
		 * @return Chat transfer save directory.
		 */
		[[nodiscard]] QString chatSaveDirectory() const;
		/**
		 * @brief Returns ignore chat colour-codes setting.
		 * @return Ignore-chat-colours flag.
		 */
		[[nodiscard]] bool    ignoreChatColours() const;
		/**
		 * @brief Returns max chat transcript lines.
		 * @return Max chat lines.
		 */
		[[nodiscard]] int     chatMaxLines() const;
		/**
		 * @brief Returns max chat transcript bytes.
		 * @return Max chat bytes.
		 */
		[[nodiscard]] int     chatMaxBytes() const;
		/**
		 * @brief Returns default chat foreground color.
		 * @return Chat foreground color.
		 */
		[[nodiscard]] long    chatForegroundColour() const;
		/**
		 * @brief Returns default chat background colour.
		 * @return Chat background color.
		 */
		[[nodiscard]] long    chatBackgroundColour() const;
		/**
		 * @brief Broadcasts chat message to matching connections.
		 * @param message Protocol message code.
		 * @param text Message payload text.
		 * @param unlessIgnoring Skip recipients that ignore sender when `true`.
		 * @param incomingOnly Send only to incoming links when `true`.
		 * @param outgoingOnly Send only to outgoing links when `true`.
		 * @param exceptId Connection id to exclude.
		 * @param group Target group filter.
		 * @param stamp Message timestamp.
		 * @return Number of recipients.
		 */
		int  sendChatMessageToAll(int message, const QString &text, bool unlessIgnoring, bool incomingOnly,
		                          bool outgoingOnly, long exceptId, const QString &group, long stamp);
		/**
		 * @brief Returns whether target chat endpoint is already connected.
		 * @param address Target host/address.
		 * @param port Target port.
		 * @param exclude Optional connection to exclude.
		 * @return `true` when a matching connection already exists.
		 */
		bool isChatAlreadyConnected(const QString &address, int port, const ChatConnection *exclude) const;
		/**
		 * @brief Returns active chat connection list.
		 * @return Active chat connections.
		 */
		[[nodiscard]] QList<ChatConnection *> chatConnections() const;
		/**
		 * @brief Returns chat connection by id.
		 * @param id Chat connection id.
		 * @return Matching chat connection pointer, or `nullptr`.
		 */
		[[nodiscard]] ChatConnection         *chatConnectionById(long id) const;
		/**
		 * @brief Removes and deletes chat connection.
		 * @param connection Chat connection pointer.
		 */
		void                                  removeChatConnection(ChatConnection *connection);
		/**
		 * @brief Calls plugin hotspot callback.
		 * @param pluginId Plugin id.
		 * @param functionName Callback function name.
		 * @param flags Mouse/keyboard flags.
		 * @param hotspotId Hotspot id.
		 * @return `true` when callback succeeds.
		 */
		bool callPluginHotspotFunction(const QString &pluginId, const QString &functionName, long flags,
		                               const QString &hotspotId);
		/**
		 * @brief Calls world hotspot callback.
		 * @param functionName Callback function name.
		 * @param flags Mouse/keyboard flags.
		 * @param hotspotId Hotspot id.
		 * @return `true` when callback succeeds.
		 */
		bool callWorldHotspotFunction(const QString &functionName, long flags, const QString &hotspotId);
		/**
		 * @brief Saves one plugin state snapshot.
		 * @param plugin Plugin instance.
		 * @param scripted Scripted-save flag.
		 * @param error Optional output error message.
		 * @return API status code.
		 */
		int  savePluginStateForPlugin(Plugin &plugin, bool scripted, QString *error) const;
		/**
		 * @brief Sorts plugins by sequence ordering.
		 */
		void sortPluginsBySequence();
		/**
		 * @brief Returns log-rotation size threshold.
		 * @return Rotation threshold in bytes.
		 */
		[[nodiscard]] qint64  logRotateLimitBytes() const;
		/**
		 * @brief Returns whether rotated logs should be gzipped.
		 * @return `true` when gzip is enabled for rotated logs.
		 */
		[[nodiscard]] bool    logRotateGzipEnabled() const;
		/**
		 * @brief Builds rotated log filename.
		 * @param previousFileName Current log filename.
		 * @return Rotated log filename.
		 */
		[[nodiscard]] QString buildRotatedLogFileName(const QString &previousFileName) const;
		/**
		 * @brief Compresses file to gzip and replaces original.
		 * @param sourceFileName Source file path.
		 * @param errorMessage Optional output error message.
		 * @return `true` when compression succeeds.
		 */
		static bool           gzipFileInPlace(const QString &sourceFileName, QString *errorMessage);
		/**
		 * @brief Performs log rotation if configured threshold was reached.
		 * @return API status code.
		 */
		int                   rotateLogFile();
		struct SavePluginSnapshot
		{
				QMap<QString, QString> attributes;
				QString                source;
				bool                   global{false};
		};
		struct SaveSnapshot
		{
				QString                   targetFilePath;
				QString                   worldFilePath;
				QString                   pluginsDirectory;
				QMap<QString, QString>    worldAttributes;
				QMap<QString, QString>    worldMultilineAttributes;
				QList<Include>            includes;
				QList<Trigger>            triggers;
				QList<Alias>              aliases;
				QList<Timer>              timers;
				QList<Macro>              macros;
				QList<Variable>           variables;
				QList<Colour>             colours;
				QList<Keypad>             keypadEntries;
				QList<PrintingStyle>      printingStyles;
				QList<SavePluginSnapshot> plugins;
		};
		/**
		 * @brief Builds immutable snapshot of current savable state.
		 * @param fileName Target world file name.
		 * @return Snapshot of current save state.
		 */
		[[nodiscard]] SaveSnapshot buildSaveSnapshot(const QString &fileName) const;
		/**
		 * @brief Compares live state with save snapshot.
		 * @param snapshot Snapshot to compare with.
		 * @return `true` when live state matches snapshot.
		 */
		[[nodiscard]] bool         saveStateMatchesSnapshot(const SaveSnapshot &snapshot) const;
		/**
		 * @brief Serializes snapshot to XML world file.
		 * @param snapshot Snapshot to write.
		 * @param error Optional output error message.
		 * @return `true` when write succeeds.
		 */
		static bool                writeSaveSnapshot(const SaveSnapshot &snapshot, QString *error);

		struct MxpTagFrame
		{
				QByteArray          tag;
				int                 contentStart{0};
				QString             variableName;
				QVector<QByteArray> closeTags;
		};
		struct MxpOpenTag
		{
				QByteArray tag;
				bool       openedSecure{false};
				bool       noReset{false};
		};

		QMap<QString, QString>                m_worldAttributes;
		QMap<QString, QString>                m_worldMultilineAttributes;
		qint64                                m_scriptTimeNanos{0};
		unsigned short                        m_noteStyle{0};
		QMap<long, long>                      m_colourTranslationMap;
		bool                                  m_notesInRgb{false};
		int                                   m_noteTextColour{kSameColour};
		long                                  m_noteColourFore{0xFFFFFF};
		long                                  m_noteColourBack{0x000000};
		int                                   m_worldFileVersion{0};
			QString                               m_qmudVersion;
		QDateTime                             m_dateSaved;
		WorldSocketService                   *m_socket{nullptr};
		WorldView                            *m_view{nullptr};
		long                                  m_backgroundColour{0};
		QImage                                m_backgroundImage;
		QImage                                m_foregroundImage;
		QString                               m_backgroundImageName;
		QString                               m_foregroundImageName;
		int                                   m_backgroundImageMode{0};
		int                                   m_foregroundImageMode{0};
		TelnetProcessor                       m_telnet;

		int                                   m_triggerCount{0};
		int                                   m_aliasCount{0};
		int                                   m_timerCount{0};
		int                                   m_macroCount{0};
		int                                   m_variableCount{0};
		int                                   m_colourCount{0};
		int                                   m_keypadCount{0};
		int                                   m_printingStyleCount{0};
		int                                   m_pluginCount{0};
		int                                   m_includeCount{0};
		int                                   m_scriptCount{0};
		enum class AnsiParserState {
			Normal,
			ESC,
			CSI,
			OSC,
			OSC_ESC
		}                                     m_ansiParserState;
		QByteArray                            m_pendingAnsiSequence;
		AnsiRenderState                       m_ansiRenderState;
		QByteArray                            m_streamUtf8Carry;
		QStringDecoder                        m_streamLocalDecoder{QStringConverter::System};
		bool                                  m_streamUtf8DecoderEnabled{false};
		MxpStyleState                         m_mxpRenderStyle;
		QVector<MxpStyleFrame>                m_mxpRenderStack;
		QVector<QByteArray>                   m_mxpRenderBlockStack;
		bool                                  m_mxpRenderLinkOpen{false};
		int                                   m_mxpRenderPreDepth{0};

		QString                               m_partialLineText;
		QVector<StyleSpan>                    m_partialLineSpans;

		QList<Trigger>                        m_triggers;
		QList<Alias>                          m_aliases;
		QList<Timer>                          m_timers;
		QStringList                           m_recentLines;
		StopTriggerEvaluation                 m_stopTriggerEvaluation{KeepEvaluating};
		QMap<QString, QStringList>            m_triggerWildcards;
		QMap<QString, QMap<QString, QString>> m_triggerNamedWildcards;
		QMap<QString, QStringList>            m_aliasWildcards;
		QMap<QString, QMap<QString, QString>> m_aliasNamedWildcards;
		QList<Macro>                          m_macros;
		QList<Variable>                       m_variables;
		QMap<QString, ArrayEntry>             m_arrays;
		QMap<QString, DatabaseEntry>          m_databases;
		QList<Colour>                         m_colours;
		QList<Keypad>                         m_keypadEntries;
		QList<PrintingStyle>                  m_printingStyles;
		QTcpServer                           *m_chatServer{nullptr};
		QMap<long, ChatConnection *>          m_chatConnections;
		long                                  m_nextChatId{0};
		QString                               m_lastChatMessageSent;
		QString                               m_lastChatGroupMessageSent;
		QDateTime                             m_lastChatMessageTime;
		QDateTime                             m_lastChatGroupMessageTime;
		QList<Plugin>                         m_plugins;
		QList<Include>                        m_includes;
		QList<Script>                         m_scripts;
		QString                               m_comments;
		QVector<LineEntry>                    m_lines;
		QMap<qint64, int>                     m_acceleratorKeyToCommand;
		QMap<int, AcceleratorEntry>           m_commandToAcceleratorEntry;
		int                                   m_nextAcceleratorCommand{kAcceleratorFirstCommand};
		QMap<QString, MiniWindow>             m_miniWindows;
		int                                   m_absoluteReferenceRightOver{0};
		int                                   m_absoluteReferenceBottomOver{0};
		int                                   m_absoluteReferenceRightUnder{0};
		int                                   m_absoluteReferenceBottomUnder{0};
		QSet<QString>                         m_specialFontPaths;
		QVector<QString>                      m_specialFontPathOrder;
		QVector<int>                          m_specialFontIds;
		QFile                                 m_logFile;
		QString                               m_logFileName;
		QString                               m_logRotationBaseFileName;
		bool                                  m_logRotateInProgress{false};
		QString                               m_defaultWorldDirectory;
		QString                               m_defaultLogDirectory;
		QString                               m_startupDirectory;
		QString                               m_worldFilePath;
		QString                               m_pluginsDirectory;
		QString                               m_stateFilesDirectory;
		QString                               m_fileBrowsingDirectory;
		QString                               m_preferencesDatabaseName;
		QString                               m_translatorFile;
		QString                               m_locale;
		QString                               m_fixedPitchFont;
		QString                               m_statusMessage;
		QString                               m_wordUnderMenu;
		bool                                  m_debugIncomingPackets{false};
		QString                               m_lastImmediateExpression;
		QString                               m_lastCommandSent;
		QString                               m_lastTelnetSubnegotiation;
		int                                   m_totalLinesSent{0};
		QDateTime                             m_lastUserInput;
		int                                   m_linesReceived{0};
		int                                   m_utf8ErrorCount{0};
		int                                   m_lastLineWithIacGa{0};
		unsigned short                        m_currentActionSource{eUnknownActionSource};
		int                                   m_newLines{0};
		bool                                  m_doingSimulate{false};
		bool                                  m_tabCompleteFunctions{false};
		QSet<QString>                         m_shiftTabCompleteItems;
		bool                                  m_active{false};
		int                                   m_triggersMatchedThisSession{0};
		int                                   m_triggersEvaluatedCount{0};
		int                                   m_aliasesMatchedThisSession{0};
		int                                   m_aliasesEvaluatedCount{0};
		int                                   m_timersFiredThisSession{0};
		int                                   m_connectPhase{eConnectNotConnected};
		bool                                  m_connectViaProxy{false};
		QString                               m_proxyAddressString;
		quint32                               m_proxyAddressV4{0};
		int                                   m_lastPreferencesPage{0};
		QString                               m_lastTriggerTreeExpandedGroup;
		QString                               m_lastAliasTreeExpandedGroup;
		QString                               m_lastTimerTreeExpandedGroup;
		bool                                  m_hasCachedIp{false};
		QDateTime                             m_connectTime;
		bool                                  m_disconnectOk{true};
		bool                                  m_reconnectOnLinkFailure{false};
		QDateTime                             m_statusTime;
		QDateTime                             m_lastFlushTime;
		QDateTime                             m_clientStartTime;
		QDateTime                             m_worldStartTime;
		int                                   m_mxpErrors{0};
		bool                                  m_mxpActive{false};
		bool                                  m_outputFrozen{false};
		TextRectangleSettings                 m_textRectangle;
		QMap<int, UdpListener>                m_udpListeners;
		QVector<SoundBuffer>                  m_soundBuffers;
		int                                   m_outputFontHeight{0};
		int                                   m_outputFontWidth{0};
		int                                   m_inputFontHeight{0};
		int                                   m_inputFontWidth{0};
		int                                   m_queuedCommandCount{0};
		bool                                  m_removeMapReverses{true};
		qint64                                m_bytesIn{0};
		qint64                                m_bytesOut{0};
		int                                   m_inputPacketCount{0};
		int                                   m_outputPacketCount{0};
		bool                                  m_isMapping{false};
		bool                                  m_variablesChanged{false};
		bool                                  m_lineOmittedFromOutput{false};
		bool                                  m_traceEnabled{false};
		bool                                  m_worldFileModified{false};
		qint64                                m_newlinesReceived{0};
		bool                                  m_scriptFileChanged{false};
		bool                                  m_loadingDocument{false};
		bool                                  m_inPlaySoundPluginCallback{false};
		bool                                  m_inCancelSoundPluginCallback{false};
		bool                                  m_inScreendrawCallback{false};
		bool                                  m_inDrawOutputWindowCallback{false};
		bool                                  m_pluginInstallDeferred{false};
		int                                   m_outputWindowRedrawCount{0};
		QFileSystemWatcher                   *m_scriptWatcher{nullptr};
		QVector<MxpTagFrame>                  m_mxpTagStack;
		QVector<MxpOpenTag>                   m_mxpOpenTags;
		QByteArray                            m_mxpTextBuffer;
		QString                               m_windowTitleOverride;
		QString                               m_mainTitleOverride;
		class LuaCallbackEngine              *m_luaCallbacks{nullptr};
		QString                               m_luaScriptText;
		WorldCommandProcessor                *m_commandProcessor{nullptr};
		int                                   m_lastGoTo{1};
		QList<QString>                        m_mappingList;
		qint64                                m_triggerTimeNs{0};
		qint64                                m_aliasTimeNs{0};
		QElapsedTimer                         m_lineTimer;
		qint64                                m_nextLineNumber{1};
		bool                                  m_luaContextLineActive{false};
		bool                                  m_luaContextLineBuffered{false};
		bool                                  m_luaContextLineCommitted{false};
		int                                   m_luaContextLineBufferIndex{0};
		LineEntry                             m_luaContextLineEntry;
};

#endif // QMUD_WORLDRUNTIME_H
