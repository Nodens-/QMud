/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MainFrame.h
 * Role: Top-level application frame interfaces for hosting MDI content, toolbars, and global UI actions.
 */

#ifndef QMUD_MAINFRAME_H
#define QMUD_MAINFRAME_H

#include "MainWindowHost.h"
#include "MdiTabs.h"
#include <QDockWidget>
#include <QElapsedTimer>
#include <QLabel>
#include <QMainWindow>
#include <QMap>
#include <QRect>
#include <QStatusBar>
#include <QStringList>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>

class QAction;
class QMenu;
class QMdiArea;
class QMdiSubWindow;
class QIcon;
class QSystemTrayIcon;
class WorldChildWindow;
class ActivityChildWindow;
class TextChildWindow;
class WorldRuntime;
class QShowEvent;
class QWidget;
class QVBoxLayout;
class QColor;
class QMoveEvent;
class QResizeEvent;

/**
 * @brief Clickable status-bar label used by main-window status panes.
 */
class StatusPaneLabel : public QLabel
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates clickable status-pane label.
		 * @param text Initial label text.
		 * @param parent Optional Qt parent widget.
		 */
		explicit StatusPaneLabel(const QString &text = QString(), QWidget *parent = nullptr);

	signals:
		/**
		 * @brief Emitted on single click.
		 */
		void clicked();
		/**
		 * @brief Emitted on double click.
		 */
		void doubleClicked();

	protected:
		/**
		 * @brief Emits clicked() for single-click interaction.
		 * @param event Mouse event payload.
		 */
		void mousePressEvent(QMouseEvent *event) override;
		/**
		 * @brief Emits doubleClicked() for double-click interaction.
		 * @param event Mouse event payload.
		 */
		void mouseDoubleClickEvent(QMouseEvent *event) override;
};

/**
 * @brief Top-level application main window and UI command host.
 *
 * Owns MDI management, menus/toolbars/status panes, and bridges UI actions
 * to world windows/runtimes via the `MainWindowHost` interface.
 */
class MainWindow : public QMainWindow, public MainWindowHost
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates top-level main window and base UI structure.
		 * @param parent Optional Qt parent widget.
		 */
		explicit MainWindow(QWidget *parent = nullptr);
		/**
		 * @brief Destroys main window and owned widgets/services.
		 */
		~MainWindow() override;

		// Lookup action by command-name used in original code
		/**
		 * @brief Returns QAction for command name mapping.
		 * @param cmdName Command identifier/name.
		 * @return Matching action pointer, or `nullptr`.
		 */
		[[nodiscard]] QAction          *actionForCommand(const QString &cmdName) const override;
		/**
		 * @brief Creates generic MDI child container.
		 * @param title Initial child window title.
		 * @return Created child widget.
		 */
		QWidget                        *createMdiChild(const QString &title);
		/**
		 * @brief Adds child window to MDI area.
		 * @param subWindow Subwindow to add.
		 * @param activate Activate subwindow immediately when `true`.
		 */
		void                            addMdiSubWindow(QMdiSubWindow *subWindow, bool activate = true);
		/**
		 * @brief Returns active world child window.
		 * @return Active world child window, or `nullptr`.
		 */
		[[nodiscard]] WorldChildWindow *activeWorldChildWindow() const override;
		/**
		 * @brief Returns active text child window.
		 * @return Active text child window, or `nullptr`.
		 */
		[[nodiscard]] TextChildWindow  *activeTextChildWindow() const;
		/**
		 * @brief Finds world child for runtime.
		 * @param runtime Runtime to resolve.
		 * @return Matching world child window, or `nullptr`.
		 */
		WorldChildWindow               *findWorldChildWindow(WorldRuntime *runtime) const override;
		/**
		 * @brief Activates window for runtime.
		 * @param runtime Runtime whose window should be activated.
		 * @return `true` when activation succeeds.
		 */
		bool                            activateWorldRuntime(WorldRuntime *runtime) override;
		/**
		 * @brief Shows/hides tray icon.
		 * @param visible Show tray icon when `true`.
		 */
		void                            setTrayIconVisible(bool visible);
		/**
		 * @brief Shows status message with optional timeout.
		 * @param message Status text.
		 * @param timeoutMs Timeout in milliseconds; `0` means no timeout.
		 */
		void                            showStatusMessage(const QString &message, int timeoutMs) override;
		/**
		 * @brief Shows status message with no timeout.
		 */
		void                            showStatusMessage(const QString &message)
		{
			showStatusMessage(message, 0);
		}
		/**
		 * @brief Switches focus to notepad child window.
		 * @return `true` when focus switched successfully.
		 */
		bool               switchToNotepad() override;
		/**
		 * @brief Activates named notepad window.
		 * @param title Notepad title.
		 * @return `true` when notepad was found and activated.
		 */
		[[nodiscard]] bool activateNotepad(const QString &title) const;
		/**
		 * @brief Activates named notepad related to runtime.
		 * @param title Notepad title.
		 * @param relatedRuntime Runtime associated with the notepad.
		 * @return `true` when notepad was found and activated.
		 */
		bool               activateNotepad(const QString &title, WorldRuntime *relatedRuntime) const;
		/**
		 * @brief Appends/replaces text in target notepad.
		 * @param title Notepad title.
		 * @param text Text payload.
		 * @param replace Replace existing content when `true`; append otherwise.
		 * @param relatedRuntime Runtime associated with the notepad.
		 * @return `true` when operation succeeds.
		 */
		bool               appendToNotepad(const QString &title, const QString &text, bool replace,
		                                   WorldRuntime *relatedRuntime) override;
		/**
		 * @brief Convenience overload appending text without runtime association.
		 */
		bool               appendToNotepad(const QString &title, const QString &text, bool replace)
		{
			return appendToNotepad(title, text, replace, nullptr);
		}
		/**
		 * @brief Sends text to notepad (replace mode).
		 * @param title Notepad title.
		 * @param text Text payload.
		 * @param relatedRuntime Runtime associated with the notepad.
		 * @return `true` when operation succeeds.
		 */
		bool sendToNotepad(const QString &title, const QString &text, WorldRuntime *relatedRuntime) override;
		/**
		 * @brief Convenience overload sending text without runtime association.
		 */
		bool sendToNotepad(const QString &title, const QString &text)
		{
			return sendToNotepad(title, text, nullptr);
		}
		/**
		 * @brief UI appearance/control helpers.
		 * @param visible Show toolbar when `true`.
		 */
		void               setToolbarVisible(bool visible) const;
		/**
		 * @brief Shows or hides world toolbar.
		 * @param visible Show world toolbar when `true`.
		 */
		void               setWorldToolbarVisible(bool visible) const;
		/**
		 * @brief Shows or hides activity toolbar.
		 * @param visible Show activity toolbar when `true`.
		 */
		void               setActivityToolbarVisible(bool visible) const;
		/**
		 * @brief Shows or hides status bar.
		 * @param visible Show status bar when `true`.
		 */
		void               setStatusbarVisible(bool visible) const;
		/**
		 * @brief Shows or hides info bar.
		 * @param visible Show info bar when `true`.
		 */
		void               setInfoBarVisible(bool visible) const;
		/**
		 * @brief Sets style flags for window tabs.
		 * @param style Window-tab style flags.
		 */
		void               setWindowTabsStyle(int style);
		/**
		 * @brief Sets style for activity toolbar.
		 * @param style Activity-toolbar style value.
		 */
		void               setActivityToolbarStyle(int style);
		/**
		 * @brief Sets periodic timer interval.
		 * @param seconds Interval in seconds.
		 */
		void               setTimerInterval(int seconds);
		/**
		 * @brief Sets activity refresh mode and interval.
		 * @param mode Activity refresh mode.
		 * @param interval Refresh interval in seconds.
		 */
		void               setActivityRefresh(int mode, int interval);
		/**
		 * @brief Updates tray icon glyph.
		 * @param icon Tray icon image.
		 */
		void               setTrayIconIcon(const QIcon &icon);
		/**
		 * @brief Toggles keyboard menu activation behavior.
		 * @param disabled Disable keyboard menu activation when `true`.
		 */
		void               setDisableKeyboardMenuActivation(bool disabled);
		/**
		 * @brief Stores last non-maximized geometry rectangle.
		 * @param rect Last normal geometry rectangle.
		 */
		void               setLastNormalGeometry(const QRect &rect);
		/**
		 * @brief Applies notepad appearance preferences.
		 * @param wrap Enable line wrapping when `true`.
		 * @param text Notepad text color.
		 * @param back Notepad background color.
		 */
		void               applyNotepadPreferences(bool wrap, const QColor &text, const QColor &back) const;
		/**
		 * @brief Sets notepad font.
		 * @param font Notepad font.
		 */
		void               setNotepadFont(const QFont &font) const;
		/**
		 * @brief Toggles gridlines in list views.
		 * @param visible Show list-view grid lines when `true`.
		 */
		void               setListViewGridLinesVisible(bool visible) const;
		/**
		 * @brief Toggles flat toolbar style.
		 * @param flat Enable flat toolbar styling when `true`.
		 */
		void               setFlatToolbars(bool flat) const;
		/**
		 * @brief Returns true when activity window exists.
		 * @return `true` when activity window is present.
		 */
		[[nodiscard]] bool hasActivityWindow() const;
		[[nodiscard]] QToolBar *mainToolbar() const
		{
			return m_mainToolbarWidget;
		}
		[[nodiscard]] QToolBar *worldToolbar() const
		{
			return m_worldToolbarWidget;
		}
		[[nodiscard]] QToolBar *activityToolbar() const
		{
			return m_activityToolbarWidget;
		}
		[[nodiscard]] QWidget *infoBarWidget() const
		{
			return m_infoBarWidget;
		}
		[[nodiscard]] QStatusBar *frameStatusBar() const
		{
			return m_qtStatusBar;
		}
		/**
		 * @brief Returns info dock widget.
		 * @return Info dock widget pointer.
		 */
		[[nodiscard]] QDockWidget         *infoDock() const;
		/**
		 * @brief Returns activity child window.
		 * @return Activity child window pointer, or `nullptr`.
		 */
		[[nodiscard]] ActivityChildWindow *activityChildWindow() const;
		/**
		 * @brief Activates activity child window.
		 * @return `true` when activation succeeds.
		 */
		[[nodiscard]] bool                 activateActivityWindow() const;
		/**
		 * @brief Sets MDI frame background colur.
		 * @param colour Packed color value.
		 */
		void                               setFrameBackgroundColour(long colour);
		/**
		 * @brief Resets toolbars to default layout/state.
		 */
		void                               resetToolbarsToDefaults();
		/**
		 * @brief Applies toolbar visibility from persisted state.
		 */
		void                               syncToolbarVisibilityFromState() const;
		/**
		 * @brief Updates recent-file menu items.
		 * @param files Recent file path list.
		 */
		void                               setRecentFiles(const QStringList &files);
		/**
		 * @brief Updates UI for connected/disconnected state.
		 * @param connected Connection state.
		 */
		void                               setConnectedState(bool connected) override;
		/**
		 * @brief Refreshes edit command enable states.
		 */
		void                               updateEditActions() override;
		/**
		 * @brief Returns count of open world windows.
		 * @return Number of open world windows.
		 */
		[[nodiscard]] int                  worldWindowCount() const;
		/**
		 * @brief Queues deferred UI refresh for selected areas.
		 * @param refreshStatus Refresh status panes when `true`.
		 * @param refreshTabs Refresh MDI tabs when `true`.
		 * @param refreshActivity Refresh activity controls when `true`.
		 */
		void requestDeferredUiRefresh(bool refreshStatus, bool refreshTabs, bool refreshActivity) override;

		/**
		 * @brief Status/info bar update helpers.
		 * @param msg Status text.
		 */
		void setStatusMessage(const QString &msg) const;
		/**
		 * @brief Updates status text immediately bypassing delay.
		 * @param msg Status text.
		 */
		void setStatusMessageNow(const QString &msg) override;
		/**
		 * @brief Restores status pane to normal/default text.
		 */
		void setStatusNormal() override;
		/**
		 * @brief Enables/disables debug status details.
		 * @param enabled Enable debug status details when `true`.
		 */
		void setShowDebugStatus(bool enabled);
		/**
		 * @brief Returns whether frame is currently in full-screen mode.
		 */
		[[nodiscard]] bool isFullScreenMode() const
		{
			return m_fullScreenMode;
		}
		/**
		 * @brief Sets full-screen mode flag.
		 * @param fullScreen `true` to mark frame as full-screen mode.
		 */
		void setFullScreenMode(bool fullScreen)
		{
			m_fullScreenMode = fullScreen;
		}

		/**
		 * @brief Rebuilds main window title text.
		 */
		void                                         refreshTitleBar() override;

		/**
		 * @brief Runtime ticker/status refresh helpers.
		 */
		void                                         checkTimerFallback();
		/**
		 * @brief Processes plugin tick callbacks.
		 */
		void                                         processPluginTicks();
		/**
		 * @brief Processes world timers.
		 */
		void                                         processTimers();
		/**
		 * @brief Refreshes status bar values.
		 */
		void                                         updateStatusBar() override;
		/**
		 * @brief Recalculates action enabled/check states.
		 */
		void                                         refreshActionState() override;
		/**
		 * @brief Refreshes activity toolbar controls.
		 */
		void                                         updateActivityToolbarButtons() override;
		/**
		 * @brief Refreshes MDI tabs.
		 */
		void                                         updateMdiTabs() override;
		/**
		 * @brief Triggers miniwindow refresh in world views.
		 */
		void                                         refreshWorldMiniWindows() const;
		/**
		 * @brief Activates world window by slot.
		 * @param slot One-based world slot index.
		 */
		void                                         activateWorldSlot(int slot) override;
		/**
		 * @brief Returns last remembered normal geometry.
		 * @return Last non-maximized geometry rectangle.
		 */
		[[nodiscard]] QRect                          lastNormalGeometry() const;
		/**
		 * @brief Returns descriptors of open world windows.
		 * @return Descriptor list for open world windows.
		 */
		[[nodiscard]] QVector<WorldWindowDescriptor> worldWindowDescriptors() const override;
		/**
		 * @brief Clears info bar contents.
		 */
		void                                         infoBarClear() const;
		/**
		 * @brief Appends text to info bar.
		 * @param text Text to append.
		 */
		void                                         infoBarAppend(const QString &text) const;
		/**
		 * @brief Sets info bar font style.
		 * @param fontName Font family name.
		 * @param size Font point size.
		 * @param styleFlags Style bitmask.
		 */
		void infoBarSetFont(const QString &fontName, int size, int styleFlags) const;
		/**
		 * @brief Sets info bar text colur.
		 * @param color Text color.
		 */
		void infoBarSetColour(const QColor &color) const;
		/**
		 * @brief Sets info bar background color.
		 * @param color Background color.
		 */
		void infoBarSetBackground(const QColor &color) const;

	signals:
		// Fired when any menu/toolbar action is triggered; cmdName maps to original command IDs
		/**
		 * @brief Emitted when command action triggers.
		 * @param cmdName Triggered command identifier.
		 */
		void commandTriggered(const QString &cmdName);
		/**
		 * @brief Emitted when a view preference changes.
		 * @param key Preference key.
		 * @param value Preference value.
		 */
		void viewPreferenceChanged(const QString &key, int value);
		/**
		 * @brief Emitted when a recent-file entry is activated.
		 * @param path Activated recent-file path.
		 */
		void recentFileTriggered(const QString &path);

	protected:
		/**
		 * @brief Qt event handlers for top-level window lifecycle/input.
		 * @param event Change event payload.
		 */
		void            changeEvent(QEvent *event) override;
		/**
		 * @brief Handles first/show lifecycle updates.
		 * @param event Show event payload.
		 */
		void            showEvent(QShowEvent *event) override;
		/**
		 * @brief Tracks main-window move events.
		 * @param event Move event payload.
		 */
		void            moveEvent(QMoveEvent *event) override;
		/**
		 * @brief Handles main-window resize.
		 * @param event Resize event payload.
		 */
		void            resizeEvent(QResizeEvent *event) override;
		/**
		 * @brief Handles generic main-window events.
		 * @param event Event payload.
		 * @return `true` when event is consumed.
		 */
		bool            event(QEvent *event) override;
		/**
		 * @brief Handles filtered child-widget events.
		 * @param watched Object receiving the event.
		 * @param event Event payload.
		 * @return `true` when event is consumed.
		 */
		bool            eventFilter(QObject *watched, QEvent *event) override;

		QToolBar       *m_mainToolbarWidget{nullptr};
		QToolBar       *m_worldToolbarWidget{nullptr};
		QToolBar       *m_activityToolbarWidget{nullptr};

		QWidget        *m_infoBarWidget{nullptr};

		QTextCharFormat m_defaultInfoBarFormat;

		unsigned int m_backgroundColour{0xFFFFFFFF}; // MDI frame background color, 0xFFFFFFFF for the default

	protected: // control bar embedded members
		bool                     m_fullScreenMode{false};
		MdiTabs                  m_mdiTabs;

		QMdiArea                *m_mdiArea{nullptr};
		QPointer<QMdiSubWindow>  m_lastActiveSubWindow;

		bool                     m_initialShowHandled{false};
		QRect                    m_lastNormalGeometry;

		QWidget                 *m_centralContainer{nullptr};
		QVBoxLayout             *m_centralLayout{nullptr};

		QSystemTrayIcon         *m_trayIcon{nullptr};

		QMenu                   *m_fileMenu{nullptr};
		QMenu                   *m_windowMenu{nullptr};

		QVector<QAction *>       m_recentActions;
		int                      m_recentMax{4};

		QMap<QString, QAction *> m_actions;
		QVector<QAction *>       m_worldActions;
		QByteArray               m_defaultToolbarState;
		QStatusBar              *m_qtStatusBar{nullptr};
		StatusPaneLabel         *m_statusMessage{nullptr};
		StatusPaneLabel         *m_statusFreeze{nullptr};
		StatusPaneLabel         *m_statusMushName{nullptr};
		StatusPaneLabel         *m_statusTime{nullptr};
		StatusPaneLabel         *m_statusLines{nullptr};
		StatusPaneLabel         *m_statusLog{nullptr};
		StatusPaneLabel         *m_statusCaps{nullptr};
		QDockWidget             *m_infoDock{nullptr};
		QTextEdit               *m_infoText{nullptr};
		QTimer                  *m_statusTimer{nullptr};
		QTimer                  *m_statusMessageTimer{nullptr};
		QTimer                  *m_activityTimer{nullptr};
		QTimer                  *m_tickTimer{nullptr};
		QElapsedTimer            m_timerFallbackClock;
		qint64                   m_lastTimerProcessNs{0};
		qint64                   m_lastTickProcessNs{0};
		int                      m_windowTabsStyle{1};
		int                      m_activityToolbarStyle{0};
		int                      m_activityRefreshType{0};
		int                      m_activityRefreshInterval{15};
		bool                     m_disableKeyboardMenuActivation{false};
		bool                     m_deferredUiRefreshQueued{false};
		bool                     m_deferredUiRefreshStatus{false};
		bool                     m_deferredUiRefreshTabs{false};
		bool                     m_deferredUiRefreshActivity{false};

	private slots:
		/**
		 * @brief Slots handling menu/toolbar/status actions.
		 */
		void onActionTriggered();
		/**
		 * @brief Toggles toolbar visibility from menu action.
		 * @param checked Checked state from action.
		 */
		void onToggleToolbar(bool checked);
		/**
		 * @brief Toggles statusbar visibility from menu action.
		 * @param checked Checked state from action.
		 */
		void onToggleStatusbar(bool checked);
		/**
		 * @brief Toggles info bar visibility action.
		 * @param checked Checked state from action.
		 */
		void onToggleInfoBar(bool checked);
		/**
		 * @brief Handles recent-file action invocation.
		 */
		void onRecentFileAction();
		/**
		 * @brief Handles MDI active window change.
		 * @param window Newly activated MDI subwindow.
		 */
		void onMdiSubWindowActivated(QMdiSubWindow *window);
		/**
		 * @brief Handles freeze status double-click action.
		 */
		void onStatusFreezeDoubleClick();
		/**
		 * @brief Handles lines status double-click action.
		 */
		void onStatusLinesDoubleClick();
		/**
		 * @brief Handles time status double-click action.
		 */
		void onStatusTimeDoubleClick();
		/**
		 * @brief Handles mushname status click.
		 */
		void onStatusMushnameClick() const;

	private:
		/**
		 * @brief Internal UI construction and routing helpers.
		 * @return `true` when world preferences shortcut was handled.
		 */
		bool                         triggerWorldPreferencesFromShortcut();
		/**
		 * @brief Returns active MDI subwindow or last active fallback.
		 * @return Active or fallback MDI subwindow, or `nullptr`.
		 */
		[[nodiscard]] QMdiSubWindow *currentOrLastActiveSubWindow() const;
		/**
		 * @brief Handles frame close to persist state and shutdown.
		 * @param event Close event payload.
		 */
		void                         closeEvent(QCloseEvent *event) override;
		/**
		 * @brief Builds menu bar and command actions.
		 */
		void                         buildMenus();
		/**
		 * @brief Builds central MDI area.
		 */
		void                         buildMdiArea();
		/**
		 * @brief Builds toolbars.
		 */
		void                         buildToolbars();
		/**
		 * @brief Builds status bar panes.
		 */
		void                         buildStatusBar();
		/**
		 * @brief Builds info bar dock widgets.
		 */
		void                         buildInfoBar();
		/**
		 * @brief Applies info-bar appearance from runtime.
		 * @param runtime Runtime source for info-bar settings.
		 */
		void                         applyInfoBarAppearance(WorldRuntime *runtime) const;
		/**
		 * @brief Rebuilds Window menu content.
		 */
		void                         updateWindowMenu();
		/**
		 * @brief Adds separator to toolbar.
		 * @param toolbar Target toolbar.
		 */
		static void                  addToolbarSeparator(QToolBar *toolbar);
		/**
		 * @brief Creates and inserts mapped command action in menu.
		 * @param menu Target menu.
		 * @param cmdName Command identifier.
		 * @param text Visible action text.
		 * @param shortcut Optional keyboard shortcut.
		 * @return Created action instance.
		 */
		QAction                     *addActionToMenu(QMenu *menu, const QString &cmdName, const QString &text,
		                                             const QKeySequence &shortcut = QKeySequence());
		/**
		 * @brief Updates connect/disconnect action states.
		 * @param runtime Active runtime used to derive connection state.
		 */
		void                         updateConnectionActions(const WorldRuntime *runtime) const;

		bool                         m_showDebugStatus{false};
		bool                         m_capsLockOn{false};
};

#endif // QMUD_MAINFRAME_H
