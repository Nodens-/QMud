/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: AppController.h
 * Role: Application-level command routing interfaces that bridge menus/actions to world, dialog, and runtime
 * operations.
 */

#ifndef QMUD_APPCONTROLLER_H
#define QMUD_APPCONTROLLER_H

#include "ColorPacking.h"
// ReSharper disable once CppUnusedIncludeDirective
#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QMap>
#include <QObject>
#include <QPageLayout>
#include <QPointer>
#include <QRandomGenerator>
#include <QScopedPointer>
#include <QSplashScreen>
#include <QString>
#include <QStringList>
#include <QTranslator>
#include <QVariant>
// ReSharper disable once CppUnusedIncludeDirective
#include <QVector>
#include <QtSql/QSqlDatabase>
#include <atomic>
#include <functional>

class MainWindow;
class ActivityDocument;
class WorldRuntime;
class WorldView;
class QMdiSubWindow;
class NameGenerator;
class QDialog;
class QNetworkAccessManager;
class QTimer;
class QWidget;
struct ReloadWorldState;
struct lua_State;

/**
 * @brief Application-level orchestrator for windows, commands, and persistence.
 *
 * Coordinates world lifecycle, menu command dispatch, settings migration, and
 * cross-component operations spanning runtimes and top-level UI.
 */
class AppController : public QObject
{
		Q_OBJECT

	public:
		/**
		 * @brief Direction alias tuple used for logging, sending, and reverse lookup.
		 */
		struct MapDirection
		{
				QString toLog;
				QString toSend;
				QString reverse;
		};

		/**
		 * @brief Returns singleton app-controller instance.
		 * @return Singleton app-controller pointer.
		 */
		static AppController *instance();
		/**
		 * @brief Resolves bundled help database path.
		 * @return Help database file path.
		 */
		static QString        resolveHelpDatabasePath();
		/**
		 * @brief Destroys app controller and owned resources.
		 */
		~AppController() override;

		enum NewDocumentType
		{
			eNormalNewDocument,
			eQuickConnect,
			eTelnetFromBrowser
		};

		/**
		 * @brief Creates application controller.
		 * @param parent Optional Qt parent object.
		 */
		explicit AppController(QObject *parent = nullptr);

		/**
		 * @brief Starts initialization path with splash-screen flow.
		 */
		void                            startWithSplash();
		/**
		 * @brief Binds top-level main window.
		 * @param window Main window pointer.
		 */
		void                            setMainWindow(MainWindow *window);
		/**
		 * @brief Returns bound main window.
		 * @return Bound main window pointer, or `nullptr`.
		 */
		[[nodiscard]] MainWindow       *mainWindow() const;
		/**
		 * @brief Performs startup initialization sequence.
		 * @return `true` when initialization succeeds.
		 */
		bool                            initialize();
		/**
		 * @brief Applies global preferences to runtime UI/components.
		 */
		void                            applyGlobalPreferences();
		/**
		 * @brief Reloads Lua-visible globals after preference changes.
		 */
		void                            reloadGlobalPreferencesForLua();
		/**
		 * @brief Triggers immediate manual check for application updates.
		 * @param uiParent Preferred parent for any immediate update-check UI.
		 */
		void                            checkForUpdatesNow(QWidget *uiParent = nullptr);
		/**
		 * @brief Returns whether the update-check/update-install mechanism is available.
		 * @return `true` when update checks are enabled for this runtime.
		 */
		[[nodiscard]] static bool       isUpdateMechanismAvailable();
		/**
		 * @brief Returns a user-facing reason when update mechanism is unavailable.
		 * @return Empty when updates are available, otherwise unavailable reason.
		 */
		[[nodiscard]] static QString    updateMechanismUnavailableReason();
		/**
		 * @brief Registers OS-level file associations.
		 * @param errorMessage Optional output error message.
		 * @return `true` on successful registration.
		 */
		static bool                     registerFileAssociations(QString *errorMessage = nullptr);
		/**
		 * @brief Saves main window placement settings.
		 */
		void                            saveWindowPlacement() const;
		/**
		 * @brief Saves view-related preference values.
		 */
		void                            saveViewPreferences();
		/**
		 * @brief Saves session-state data for reopen flows.
		 */
		void                            saveSessionState() const;
		/**
		 * @brief Sets activity document model instance.
		 * @param doc Activity document pointer.
		 */
		void                            setActivityDocument(ActivityDocument *doc);
		/**
		 * @brief Returns current activity document model.
		 * @return Activity document pointer, or `nullptr`.
		 */
		[[nodiscard]] ActivityDocument *activityDocument() const;
		/**
		 * @brief Opens world/text file based on extension/content.
		 * @param path Document path.
		 * @return `true` when document opens successfully.
		 */
		bool                            openDocumentFile(const QString &path);
		/**
		 * @brief Opens a plain text document in notepad/text view.
		 * @param path Text document path.
		 * @return `true` on successful open.
		 */
		[[nodiscard]] bool              openTextDocument(const QString &path) const;
		/**
		 * @brief Opens multiple worlds from persisted list.
		 * @param items World file list.
		 * @param activateFirstOnly Activate only the first opened world when `true`.
		 */
		void                            openWorldsFromList(const QStringList &items, bool activateFirstOnly);
		/**
		 * @brief Restores child window geometry for one world.
		 * @param worldName World name key.
		 * @param window Target subwindow.
		 */
		void restoreWorldWindowPlacement(const QString &worldName, QMdiSubWindow *window) const;
		/**
		 * @brief Saves child window geometry for one world.
		 * @param worldName World name key.
		 * @param window Source subwindow.
		 */
		void saveWorldWindowPlacement(const QString &worldName, const QMdiSubWindow *window) const;
		/**
		 * @brief Changes process cwd to file-browsing directory.
		 */
		void changeToFileBrowsingDirectory() const;
		/**
		 * @brief Changes process cwd to startup directory.
		 */
		void changeToStartupDirectory() const;
		/**
		 * @brief Returns default world-files directory path.
		 * @return Default world directory path.
		 */
		[[nodiscard]] QString              defaultWorldDirectory() const;
		/**
		 * @brief Returns ini/preferences file path.
		 * @return INI/preferences database path.
		 */
		[[nodiscard]] QString              iniFilePath() const;
		/**
		 * @brief Reads one global option value.
		 * @param name Global option key.
		 * @return Option value.
		 */
		[[nodiscard]] QVariant             getGlobalOption(const QString &name) const;
		/**
		 * @brief Returns list of known global option names.
		 * @return Global option key list.
		 */
		static QStringList                 globalOptionList();
		/**
		 * @brief Writes integer global option.
		 * @param name Option key.
		 * @param value Integer value.
		 */
		void                               setGlobalOptionInt(const QString &name, int value);
		/**
		 * @brief Writes string global option.
		 * @param name Option key.
		 * @param value String value.
		 */
		void                               setGlobalOptionString(const QString &name, const QString &value);
		/**
		 * @brief Returns xterm palette colour at index.
		 * @param index Palette index.
		 * @return Packed xterm colour value.
		 */
		[[nodiscard]] QMudColorRef         xtermColorAt(int index) const;
		/**
		 * @brief Selects xterm colour cube preset.
		 * @param which Preset selector.
		 */
		void                               setXtermColourCube(int which);
		/**
		 * @brief Returns mutable name generator instance.
		 * @return Mutable name generator pointer.
		 */
		NameGenerator                     *nameGenerator();
		/**
		 * @brief Returns const name generator instance.
		 * @return Const name generator pointer.
		 */
		[[nodiscard]] const NameGenerator *nameGenerator() const;
		/**
		 * @brief Maps movement direction to configured log text.
		 * @param direction Direction token.
		 * @return Mapped log text.
		 */
		[[nodiscard]] QString              mapDirectionToLog(const QString &direction) const;
		/**
		 * @brief Maps movement direction to configured send text.
		 * @param direction Direction token.
		 * @return Mapped send text.
		 */
		[[nodiscard]] QString              mapDirectionToSend(const QString &direction) const;
		/**
		 * @brief Returns reverse mapping for a movement direction.
		 * @param direction Direction token.
		 * @return Reverse direction token/text.
		 */
		[[nodiscard]] QString              mapDirectionReverse(const QString &direction) const;
		/**
		 * @brief Resolves potentially relative path to absolute path.
		 * @param fileName Relative or absolute path.
		 * @return Absolute path.
		 */
		[[nodiscard]] QString              makeAbsolutePath(const QString &fileName) const;
		/**
		 * @brief Returns absolute paths for currently open world log files.
		 * @return Absolute open log file paths for active worlds.
		 */
		[[nodiscard]] QStringList          activeOpenWorldLogFiles() const;
		/**
		 * @brief Applies configured default world options to runtime.
		 * @param runtime Runtime instance to initialize.
		 */
		void                               applyConfiguredWorldDefaults(WorldRuntime *runtime) const;
		/**
		 * @brief Returns next monotonically increasing unique number.
		 * @return Unique number value.
		 */
		qint64                             nextUniqueNumber();
		/**
		 * @brief Seeds shared random generator.
		 * @param seed Seed value.
		 */
		void                               seedRandom(quint32 seed);
		/**
		 * @brief Seeds random generator from seed array.
		 * @param values Seed values.
		 */
		void                               seedRandomFromArray(const QVector<quint32> &values);
		/**
		 * @brief Returns random value in [0,1).
		 * @return Random unit value.
		 */
		double                             nextRandomUnit();
		/**
		 * @brief Creates activity document if not already present.
		 */
		void                               ensureActivityDocument();
		/**
		 * @brief Reacts to world script-file change notification.
		 * @param runtime Runtime that reported script file change.
		 */
		void                               processScriptFileChange(WorldRuntime *runtime);
		/**
		 * @brief Result summary returned by XML import operations.
		 */
		struct ImportResult
		{
				bool    ok{false};
				QString errorMessage;
				int     triggers{0};
				int     aliases{0};
				int     timers{0};
				int     macros{0};
				int     variables{0};
				int     colours{0};
				int     keypad{0};
				int     printing{0};
				int     duplicates{0};
		};

		/**
		 * @brief Imports selected XML file into current world.
		 * @param path XML file path.
		 * @param mask Import mask.
		 * @return Import result payload.
		 */
		ImportResult             importXmlFromFile(const QString &path, unsigned long mask);
		/**
		 * @brief Imports XML text payload into current world.
		 * @param xml XML payload text.
		 * @param mask Import mask.
		 * @return Import result payload.
		 */
		ImportResult             importXmlFromText(const QString &xml, unsigned long mask);
		/**
		 * @brief Returns true when spell-check subsystem is available.
		 * @return `true` when spell checker is available.
		 */
		bool                     isSpellCheckerAvailable();
		/**
		 * @brief Returns translator Lua state pointer.
		 * @return Translator Lua state pointer, or `nullptr`.
		 */
		[[nodiscard]] lua_State *translatorLuaState() const;
#ifdef QMUD_ENABLE_LUA_SCRIPTING
		/**
		 * @brief Loads spell-check Lua subsystem if needed.
		 * @return `true` when spell checker is loaded/available.
		 */
		bool                     ensureSpellCheckerLoaded();
		/**
		 * @brief Returns spell-check Lua state pointer.
		 * @return Spell-check Lua state pointer, or `nullptr`.
		 */
		[[nodiscard]] lua_State *spellCheckerLuaState() const;
		/**
		 * @brief Closes and resets spell-check Lua subsystem.
		 */
		void                     closeSpellChecker();
#endif

	public slots:
		/**
		 * @brief Dispatches top-level command trigger by command name.
		 * @param cmdName Command identifier/name.
		 */
		void onCommandTriggered(const QString &cmdName);

	private:
		static AppController             *s_instance;
		/**
		 * @brief Database and preference loading helpers.
		 * @return `true` when database opens successfully.
		 */
		bool                              openPreferencesDatabase();
		/**
		 * @brief Creates/updates preferences schema and seed rows.
		 * @return Number of schema/population changes applied.
		 */
		[[nodiscard]] int                 populateDatabase() const;
		/**
		 * @brief Loads global settings from preferences database.
		 */
		void                              loadGlobalsFromDatabase();
		/**
		 * @brief Returns count of pending DB changes.
		 * @return Number of pending DB changes.
		 */
		[[nodiscard]] int                 dbChanges() const;
		/**
		 * @brief Initializes translation/localization subsystem.
		 * @return `true` when i18n setup succeeds.
		 */
		bool                              setupI18N();
		/**
		 * @brief Loads compass/map direction aliases.
		 */
		void                              loadMapDirections();
		/**
		 * @brief Finds configured map direction entry.
		 * @param direction Direction token.
		 * @return Matching map-direction entry, or `nullptr`.
		 */
		[[nodiscard]] const MapDirection *findMapDirection(const QString &direction) const;
		/**
		 * @brief Fills xterm-256 color table.
		 */
		void                              generate256Colours();
		/**
		 * @brief Startup/setup preference application helpers.
		 */
		void                              setupStartupBehavior();
		/**
		 * @brief Applies persisted main-window preferences.
		 */
		void                              applyWindowPreferences();
		/**
		 * @brief Applies update-check/reload-gating preferences.
		 */
		void                              applyUpdatePreferences();
		/**
		 * @brief Applies persisted view preferences.
		 */
		void                              applyViewPreferences() const;
		/**
		 * @brief Restores toolbar layout from persisted state.
		 */
		void                              loadToolbarLayout() const;
		/**
		 * @brief Restores top-level window placement.
		 */
		void                              restoreWindowPlacement();
		/**
		 * @brief Builds recent-files action list.
		 */
		void                              setupRecentFiles() const;
		/**
		 * @brief Loads print setup preferences from storage.
		 */
		void                              loadPrintSetupPreferences();
		/**
		 * @brief Saves print setup preferences to storage.
		 */
		void                              savePrintSetupPreferences() const;
		/**
		 * @brief Applies global connection preferences.
		 */
		void                              applyConnectionPreferences() const;
		/**
		 * @brief Applies global spell-check preferences.
		 */
		void                              applySpellCheckPreferences();
		/**
		 * @brief Applies plugin-related preferences.
		 */
		void                              applyPluginPreferences();
		/**
		 * @brief Applies global font preferences.
		 */
		void                              applyFontPreferences() const;
		/**
		 * @brief Applies child-window behavior preferences.
		 */
		void                              applyChildWindowPreferences() const;
		/**
		 * @brief Applies timer engine preferences.
		 */
		void                              applyTimerPreferences() const;
		/**
		 * @brief Applies word-delimiter preferences.
		 */
		void                              applyWordDelimiterPreferences() const;
		/**
		 * @brief Applies editor behavior preferences.
		 */
		void                              applyEditorPreferences() const;
		/**
		 * @brief Applies default directories/preferences paths.
		 */
		void                              applyDefaultDirectories() const;
		/**
		 * @brief Applies locale/language settings.
		 */
		void                              applyLocalePreferences();
		/**
		 * @brief Applies notepad appearance/settings.
		 */
		void                              applyNotepadPreferences() const;
		/**
		 * @brief Applies list view presentation settings.
		 */
		void                              applyListViewPreferences() const;
		/**
		 * @brief Applies input control settings.
		 */
		void                              applyInputPreferences() const;
		/**
		 * @brief Applies notification settings.
		 */
		void                              applyNotificationPreferences() const;
		/**
		 * @brief Applies typing behavior settings.
		 */
		void                              applyTypingPreferences() const;
		/**
		 * @brief Applies regex-engine preferences.
		 */
		void                              applyRegexPreferences() const;
		/**
		 * @brief Applies icon and icon-placement preferences.
		 */
		void                              applyIconPreferences() const;
		/**
		 * @brief Applies font-metric compatibility preferences.
		 */
		void                              applyFontMetricPreferences() const;
		/**
		 * @brief Captures global default-font settings relevant to world view metric refresh.
		 */
		struct FontMetricApplySignature
		{
				QString defaultInputFont;
				int     defaultInputFontHeight{0};
				int     defaultInputFontWeight{0};
				int     defaultInputFontItalic{0};
				int     defaultInputFontCharset{0};
				QString defaultOutputFont;
				int     defaultOutputFontHeight{0};
				int     defaultOutputFontCharset{0};
		};

		/**
		 * @brief Applies activity-window preferences.
		 */
		void applyActivityPreferences() const;
		/**
		 * @brief Applies package/runtime deployment preferences.
		 */
		void applyPackagePreferences() const;
		/**
		 * @brief Applies miscellaneous preferences.
		 */
		void applyMiscPreferences() const;
		/**
		 * @brief Applies rendering pipeline preferences.
		 */
		void applyRenderingPreferences() const;
		/**
		 * @brief Schedules or stops periodic update checks from preferences.
		 */
		void configureUpdateCheckTimer();
		/**
		 * @brief Starts one update-check request.
		 * @param manual `true` for user-triggered checks.
		 * @param uiParent Preferred parent for user-facing dialogs when `manual`.
		 */
		void requestUpdateCheck(bool manual, QWidget *uiParent = nullptr);
		/**
		 * @brief Handles completion of one update-check request.
		 * @param manual `true` for user-triggered checks.
		 * @param payload Raw response payload.
		 * @param networkError Network error description (empty on success).
		 * @param httpStatus HTTP status code, or `0` when unavailable.
		 */
		void handleUpdateCheckResponse(bool manual, const QByteArray &payload, const QString &networkError,
		                               int httpStatus);
		/**
		 * @brief Shows modeless update-available dialog.
		 * @param currentVersion Running version string.
		 * @param version Available version string.
		 * @param changelog Release changelog text.
		 */
		void showUpdateAvailableDialog(const QString &currentVersion, const QString &version,
		                               const QString &changelog);
		/**
		 * @brief Sets visibility/enabled state of Help->Update action.
		 * @param visible Show and enable action when `true`.
		 */
		void setUpdateNowActionVisible(bool visible) const;
		/**
		 * @brief Downloads and applies the currently discovered update package.
		 */
		void handleUpdateQmudNow();
		/**
		 * @brief Applies skip-version preference for currently shown update version.
		 * @param version Version currently being prompted.
		 * @param skipWhenTrue Store skip preference when `true`.
		 */
		void applySkipVersionChoice(const QString &version, bool skipWhenTrue);
		/**
		 * @brief Returns active world runtime instances.
		 * @return Active world runtime list.
		 */
		[[nodiscard]] QVector<WorldRuntime *> activeWorldRuntimes() const;
		/**
		 * @brief Saves dirty worlds that have save-on-close enabled before reload/restart.
		 *
		 * This helper does not close windows; it only persists eligible world files.
		 *
		 * @param errorMessage Optional output error text when a save fails.
		 * @return `true` when all eligible worlds were saved successfully.
		 */
		[[nodiscard]] bool saveDirtyAutoSaveWorldsBeforeRestart(QString *errorMessage = nullptr) const;
		/**
		 * @brief Closes open world logs before reload/restart so postamble/compression runs.
		 * @param errorMessage Optional output error text when closing a log fails.
		 * @return `true` when all open world logs closed successfully.
		 */
		[[nodiscard]] bool closeOpenWorldLogsBeforeRestart(QString *errorMessage = nullptr) const;
		/**
		 * @brief Saves per-world output/history session-state files before reload/restart.
		 * @param errorMessage Optional output error text when persistence fails.
		 * @return `true` when all eligible worlds were persisted successfully.
		 */
		[[nodiscard]] bool saveOpenWorldSessionStatesBeforeRestart(QString *errorMessage = nullptr) const;
		/**
		 * @brief Saves plugin state snapshots for open worlds before reload/restart.
		 * @param errorMessage Optional output error text when a plugin-state save fails.
		 * @return `true` when all plugin-state saves completed successfully.
		 */
		[[nodiscard]] bool saveOpenWorldPluginStatesBeforeRestart(QString *errorMessage = nullptr) const;
		/**
		 * @brief Loads global plugins into runtime context.
		 * @param runtime Runtime receiving global plugin state.
		 */
		void               loadGlobalPlugins(WorldRuntime *runtime) const;
		/**
		 * @brief Startup UX helpers.
		 */
		void               showTipDialog() const;
		/**
		 * @brief Displays startup tip if enabled.
		 */
		void               showTipAtStartup() const;
		/**
		 * @brief Shows getting-started page when appropriate.
		 */
		void               showGettingStartedIfNeeded() const;
		/**
		 * @brief Shows upgrade welcome dialog when required.
		 */
		void               showUpgradeWelcomeIfNeeded() const;
		/**
		 * @brief Performs data backup when upgrading older installs.
		 * @param previousVersion Previously stored application version.
		 * @param firstTime `true` when this is first launch after install/migration.
		 */
		void               backupDataOnUpgradeIfNeeded(int previousVersion, bool firstTime) const;
		/**
		 * @brief Finalizes startup once all prerequisites are complete.
		 */
		void               finalizeStartupIfReady();
		/**
		 * @brief Parses reload startup arguments from process command line.
		 */
		void               detectReloadStartupArguments();
		/**
		 * @brief Deletes stale reload state file when startup is not reload-mode.
		 */
		void               cleanupReloadStateOnNormalStartup() const;
		/**
		 * @brief Executes startup recovery for reload-mode launches.
		 * @return `true` when recovery path completed.
		 */
		bool               recoverReloadStartupState();
		/**
		 * @brief Opens one runtime/window pair from serialized reload world state.
		 * @param worldState Serialized world state.
		 * @param activateWindow Activate the recovered world window immediately when `true`.
		 * @param runtime Output runtime pointer.
		 * @param view Optional output world view pointer.
		 * @return `true` when world opening succeeds.
		 */
		bool               openWorldForReloadRecovery(const ReloadWorldState &worldState, bool activateWindow,
		                                              WorldRuntime **runtime, WorldView **view = nullptr);
		/**
		 * @brief Reconnects recovered runtime for `park_reconnect` fallback.
		 * @param runtime Runtime to reconnect.
		 * @param worldState Serialized host/port policy data.
		 * @param closeSocketFirst Close inherited socket before reconnect when `true`.
		 */
		static void        reconnectRecoveredWorld(WorldRuntime *runtime, const ReloadWorldState &worldState,
		                                           bool closeSocketFirst);
		/**
		 * @brief Creates and shows splash screen.
		 */
		void               showSplashScreen();
		/**
		 * @brief Hides splash screen.
		 */
		void               hideSplashScreen();
		/**
		 * @brief Synchronizes AppImage payload skeleton files.
		 * @param startupDir Startup directory path.
		 */
		static void        syncAppImageSkeleton(const QString &startupDir);
		/**
		 * @brief Synchronizes macOS bundle payload files.
		 * @param startupDir Startup directory path.
		 */
		static void        syncMacBundlePayload(const QString &startupDir);

		/**
		 * @brief Low-level preferences DB helpers.
		 * @param sql SQL statement text.
		 * @param showError Show query errors in UI/log when `true`.
		 * @return SQLite-style result code.
		 */
		[[nodiscard]] int  dbExecute(const QString &sql, bool showError = false) const;
		/**
		 * @brief Executes simple SQL query returning one string.
		 * @param sql SQL query text.
		 * @param result Output string result.
		 * @param showError Show query errors in UI/log when `true`.
		 * @param defaultValue Value to use when query returns empty/missing data.
		 * @return SQLite-style result code.
		 */
		int                dbSimpleQuery(const QString &sql, QString &result, bool showError = false,
		                                 const QString &defaultValue = QString()) const;
		/**
		 * @brief Reads integer value from preferences DB.
		 * @param section Preferences section name.
		 * @param entry Preferences key name.
		 * @param defaultValue Value returned when key is missing.
		 * @return Stored integer value, or `defaultValue`.
		 */
		[[nodiscard]] int  dbGetInt(const QString &section, const QString &entry, int defaultValue = 0) const;
		/**
		 * @brief Writes integer value to preferences DB.
		 * @param section Preferences section name.
		 * @param entry Preferences key name.
		 * @param value Integer value to store.
		 * @return SQLite-style result code.
		 */
		[[nodiscard]] int  dbWriteInt(const QString &section, const QString &entry, int value) const;
		/**
		 * @brief Reads string value from preferences DB.
		 * @param section Preferences section name.
		 * @param entry Preferences key name.
		 * @param defaultValue Value returned when key is missing.
		 * @return Stored string value, or `defaultValue`.
		 */
		[[nodiscard]] QString  dbGetString(const QString &section, const QString &entry,
		                                   const QString &defaultValue = QString()) const;
		/**
		 * @brief Writes string value to preferences DB.
		 * @param section Preferences section name.
		 * @param entry Preferences key name.
		 * @param value String value to store.
		 * @return SQLite-style result code.
		 */
		[[nodiscard]] int      dbWriteString(const QString &section, const QString &entry,
		                                     const QString &value) const;
		/**
		 * @brief Escapes string for SQL literal usage.
		 * @param input Raw string input.
		 * @return SQL-literal-safe string.
		 */
		static QString         escapeSql(const QString &input);

		/**
		 * @brief Command handlers for main-window actions.
		 */
		void                   handleAppAbout();
		/**
		 * @brief Handles File->New action.
		 */
		void                   handleFileNew();
		/**
		 * @brief Handles Help->Getting Started action.
		 */
		void                   handleHelpGettingStarted() const;
		/**
		 * @brief Handles Help->Contents action.
		 */
		void                   handleHelpContents() const;
		/**
		 * @brief Handles Edit->Colour Picker action.
		 */
		void                   handleEditColourPicker() const;
		/**
		 * @brief Handles Copy action.
		 */
		void                   handleCopy() const;
		/**
		 * @brief Handles Copy as HTML action.
		 */
		void                   handleCopyAsHtml() const;
		/**
		 * @brief Handles Quick Connect action.
		 */
		void                   handleQuickConnect();
		/**
		 * @brief Handles connect-or-reconnect action.
		 */
		void                   handleConnectOrReconnect() const;
		/**
		 * @brief Handles connect action.
		 */
		void                   handleConnect() const;
		/**
		 * @brief Handles disconnect action.
		 */
		void                   handleDisconnect() const;
		/**
		 * @brief Handles game minimize action.
		 */
		void                   handleGameMinimize() const;
		/**
		 * @brief Handles log-session action.
		 */
		void                   handleLogSession() const;
		/**
		 * @brief Handles reload-QMud action with socket handoff/reconnect policies.
		 */
		void                   handleReloadQmud();
		/**
		 * @brief Handles output-find command variants.
		 * @param again Repeat previous find operation.
		 * @param forceDirection Force search direction when `true`.
		 * @param forwards Search forward when `true`, backward otherwise.
		 */
		void                   handleOutputFind(bool again, bool forceDirection, bool forwards) const;
		/**
		 * @brief Activates notepad by title.
		 * @param title Notepad title.
		 * @return `true` when a matching notepad is activated.
		 */
		[[nodiscard]] bool     activateNotepad(const QString &title) const;
		/**
		 * @brief Appends/replaces notepad content.
		 * @param title Notepad title.
		 * @param text Text payload.
		 * @param replace Replace existing content when `true`; append otherwise.
		 * @return `true` when destination notepad is found or created successfully.
		 */
		[[nodiscard]] bool     appendToNotepad(const QString &title, const QString &text, bool replace) const;
		/**
		 * @brief Sends text to notepad, replacing existing content.
		 * @param title Notepad title.
		 * @param text Text payload.
		 * @return `true` when destination notepad is found or created successfully.
		 */
		[[nodiscard]] bool     sendToNotepad(const QString &title, const QString &text) const;
		/**
		 * @brief Applies app-level initialization to a new world runtime.
		 * @param runtime Runtime to initialize.
		 */
		void                   initializeWorldRuntime(WorldRuntime *runtime) const;
		/**
		 * @brief Emits startup banner lines to runtime output.
		 * @param runtime Runtime that receives startup banner output.
		 */
		static void            emitStartupBanner(WorldRuntime *runtime);
		/**
		 * @brief Opens and initializes one world document.
		 * @param path World-document path.
		 * @return `true` when world document opens successfully.
		 */
		bool                   openWorldDocument(const QString &path);
		/**
		 * @brief Resolves world session-state directory under the data directory.
		 * @return Absolute world session-state directory path.
		 */
		[[nodiscard]] QString  worldSessionStateDirectoryPath() const;
		/**
		 * @brief Resolves one world's session-state file path.
		 * @param runtime World runtime.
		 * @return Absolute session-state file path (empty when unresolved).
		 */
		[[nodiscard]] QString  worldSessionStateFilePath(const WorldRuntime *runtime) const;
		/**
		 * @brief Persists one world's session-state file on a worker thread.
		 * @param runtime World runtime.
		 * @param view World view.
		 * @param completion Completion callback executed on the GUI thread.
		 */
		void                   saveWorldSessionStateAsync(const WorldRuntime *runtime, const WorldView *view,
		                                                  std::function<void(bool, const QString &)> completion) const;
		/**
		 * @brief Loads one world's session state on a worker thread and applies it.
		 * @param runtime World runtime.
		 * @param view World view.
		 * @param completion Completion callback executed on the GUI thread.
		 */
		void                   restoreWorldSessionStateAsync(WorldRuntime *runtime, WorldView *view,
		                                                     std::function<void(bool, const QString &)> completion) const;
		/**
		 * @brief Loads one world's session state and waits for completion.
		 * @param runtime World runtime.
		 * @param view World view.
		 * @param errorMessage Optional output error text.
		 * @return `true` on success.
		 */
		[[nodiscard]] bool     restoreWorldSessionStateSync(WorldRuntime *runtime, WorldView *view,
		                                                    QString *errorMessage = nullptr) const;
		/**
		 * @brief Emits startup banner and loads/installs global plugins after session-state restore.
		 * @param runtime World runtime.
		 */
		void                   runWorldStartupPostRestore(WorldRuntime *runtime) const;
		/**
		 * @brief Auto-connects runtime when settings request it.
		 * @param runtime Runtime to auto-connect.
		 */
		void                   maybeAutoConnectWorld(WorldRuntime *runtime) const;
		void                   beginRestoreScrollbackStatus() const;
		void                   endRestoreScrollbackStatus() const;
		MainWindow            *m_mainWindow{nullptr};
		QDateTime              m_whenClientStarted;
		QString                m_version;
		QString                m_workingDir;
		QString                m_fileBrowsingDir;
		QString                m_preferencesDatabaseName;
		QString                m_locale;
		QString                m_translatorFile;
		QMap<QString, int>     m_globalIntPrefs;
		QMap<QString, QString> m_globalStringPrefs;
		QString                m_luaScript;
		QString                m_pluginsDirectory;
		QString                m_fixedPitchFont;
		QString                m_lastGlobalChangeFrom;
		QString                m_lastGlobalChangeTo;
		QString                m_lastGlobalReplaceFind;
		QString                m_lastGlobalReplaceReplace;
		QString                m_lastFunctionListFilter;
		QString                m_lastQuickConnectWorldName;
		QString                m_lastQuickConnectHost;
		QString                m_lastDebugWorldInput;
		quint16                m_lastQuickConnectPort{4000};
		bool                   m_lastGlobalReplaceRegexp{false};
		bool                   m_lastGlobalReplaceEachLine{false};
		bool                   m_lastGlobalReplaceEscapeSequences{false};
		QString                m_lastMapperSpecialAction;
		QString                m_lastMapperSpecialReverse;
		QString                m_lastSendToAllWorlds;
		QString                m_asciiArtText;
		QString                m_printSetupPrinterName;
		QPageLayout            m_printSetupLayout;
		bool                   m_echoSendToAll{true};
		bool                   m_hasPrintSetup{false};
		QStringList            m_recallFindHistory;
		QString                m_recallLinePreamble;
		bool                   m_recallMatchCase{false};
		bool                   m_recallRegexp{false};
		bool                   m_recallCommands{true};
		bool                   m_recallOutput{true};
		bool                   m_recallNotes{true};
		QHash<class WorldRuntime *, int> m_savedWrapColumns;
		bool                             m_autoOpen{true};
		NewDocumentType                  m_typeOfNewDocument{eNormalNewDocument};
		QSplashScreen                   *m_splash{nullptr};
		bool                             m_deferMainWindowShowUntilSplash{false};
		bool                             m_showMainWindowMaximizedAfterSplash{false};
		bool                             m_splashMinDelayElapsed{false};
		bool                             m_initializeFinished{false};
		bool                             m_initializeSucceeded{false};
		bool                             m_startupFinalized{false};
		bool                             m_startupFirstTime{false};
		bool                             m_startupNeedsUpgradeWelcome{false};
		QTranslator                     *m_qtTranslator{nullptr};
		lua_State                       *m_translatorLua{nullptr};
#ifdef QMUD_ENABLE_LUA_SCRIPTING
		lua_State *m_spellCheckerLua{nullptr};
		bool       m_spellCheckOk{false};
#endif
		QVector<QMudColorRef>            m_xterm256Colours = QVector<QMudColorRef>(256);
		QHash<QString, MapDirection>     m_mapDirections;
		QScopedPointer<NameGenerator>    m_nameGenerator;
		QSqlDatabase                     m_db;
		QString                          m_dbConnectionName;
		ActivityDocument                *m_activityDoc{nullptr};
		std::atomic<qint64>              m_uniqueNumber{0};
		QRandomGenerator                 m_rng{1u};
		mutable int                      m_restoreScrollbackInFlight{0};
		mutable QPointer<WorldRuntime>   m_restoreScrollbackStatusRuntime;
		mutable QString                  m_restoreScrollbackStatusPrevious;
		mutable bool                     m_hasFontMetricApplySignature{false};
		mutable FontMetricApplySignature m_lastFontMetricApplySignature;
		bool                             m_batchOpeningWorldList{false};
		bool                             m_batchWorldListActivatedFirst{false};
		int                              m_nextNewWorldActivationOverride{-1};
		bool                             m_suppressAutoConnect{false};
		bool                             m_reloadInProgress{false};
		QTimer                          *m_updateCheckTimer{nullptr};
		QNetworkAccessManager           *m_updateNetworkManager{nullptr};
		QPointer<QDialog>                m_updateAvailableDialog;
		QPointer<QWidget>                m_updateUiParent;
		QString                          m_availableUpdateVersion;
		QString                          m_availableUpdateChangelog;
		QString                          m_availableUpdateAssetUrl;
		QString                          m_availableUpdateAssetName;
		QString                          m_availableUpdateAssetSha256;
		bool                             m_updatePackageDownloadInProgress{false};
		bool                             m_updateCheckInProgress{false};
		bool                             m_reloadLaunchRequested{false};
		QString                          m_reloadTargetExecutableOverride;
		int                              m_reloadAttempts{0};
		int                              m_reloadExecFailures{0};
		int                              m_reloadRecoveryRuns{0};
		int                              m_reloadRecoveryReattached{0};
		int                              m_reloadRecoveryReconnectQueued{0};
		QString                          m_reloadStatePathArg;
		QString                          m_reloadTokenArg;
};

#endif // QMUD_APPCONTROLLER_H
