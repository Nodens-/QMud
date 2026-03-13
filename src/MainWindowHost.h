/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MainWindowHost.h
 * Role: Abstraction interfaces used by non-UI subsystems to interact with the currently active world main window
 * safely.
 */

#ifndef QMUD_MAINWINDOWHOST_H
#define QMUD_MAINWINDOWHOST_H

// ReSharper disable once CppUnusedIncludeDirective
#include <QVector>

class QAction;
class QString;
class WorldChildWindow;
class WorldRuntime;

/**
 * @brief Snapshot describing one open world child window.
 *
 * Used for slot ordering and activation flows in the main-window host.
 */
struct WorldWindowDescriptor
{
		int               sequence{0};
		WorldChildWindow *window{nullptr};
		WorldRuntime     *runtime{nullptr};
};

/**
 * @brief Host-side interface exposed by the main window to controllers/services.
 *
 * Provides command lookup and world-window activation/query operations without
 * coupling callers to a concrete `MainWindow` implementation.
 */
class MainWindowHost
{
	public:
		/**
		 * @brief Virtual destructor for interface-safe cleanup.
		 */
		virtual ~MainWindowHost() = default;

		/**
		 * @brief Returns QAction mapped to a command id/name.
		 * @param cmdName Command identifier/name.
		 * @return Matching action pointer, or `nullptr` if not found.
		 */
		[[nodiscard]] virtual QAction          *actionForCommand(const QString &cmdName) const = 0;
		/**
		 * @brief Returns the currently active world child window.
		 * @return Active world child window pointer, or `nullptr`.
		 */
		[[nodiscard]] virtual WorldChildWindow *activeWorldChildWindow() const = 0;
		/**
		 * @brief Finds child window hosting the given runtime.
		 * @param runtime Runtime instance.
		 * @return Matching child window pointer, or `nullptr`.
		 */
		virtual WorldChildWindow               *findWorldChildWindow(WorldRuntime *runtime) const = 0;
		/**
		 * @brief Activates the child window for the given runtime.
		 * @param runtime Runtime instance.
		 * @return `true` when activation succeeds.
		 */
		virtual bool                            activateWorldRuntime(WorldRuntime *runtime) = 0;
		/**
		 * @brief Updates availability/checked state for edit-related actions.
		 */
		virtual void                            updateEditActions() = 0;
		/**
		 * @brief Refreshes status bar contents.
		 */
		virtual void                            updateStatusBar() = 0;
		/**
		 * @brief Recomputes global action enabled state.
		 */
		virtual void                            refreshActionState() = 0;
		/**
		 * @brief Schedules deferred refresh for selected UI regions.
		 * @param refreshStatus Refresh status bar when `true`.
		 * @param refreshTabs Refresh tabs when `true`.
		 * @param refreshActivity Refresh activity area when `true`.
		 */
		virtual void requestDeferredUiRefresh(bool refreshStatus, bool refreshTabs, bool refreshActivity) = 0;
		/**
		 * @brief Updates activity toolbar button states.
		 */
		virtual void updateActivityToolbarButtons() = 0;
		/**
		 * @brief Rebuilds or refreshes MDI tab presentation.
		 */
		virtual void updateMdiTabs() = 0;
		/**
		 * @brief Updates connected/disconnected UI state.
		 * @param connected Connected-state flag.
		 */
		virtual void setConnectedState(bool connected) = 0;
		/**
		 * @brief Refreshes main window title text.
		 */
		virtual void refreshTitleBar() = 0;
		/**
		 * @brief Sets an immediate status message without timeout behavior.
		 * @param message Status message text.
		 */
		virtual void setStatusMessageNow(const QString &message) = 0;
		/**
		 * @brief Restores normal/default status bar message.
		 */
		virtual void setStatusNormal() = 0;
		/**
		 * @brief Sends text to a notepad document, replacing existing content.
		 * @param title Notepad title.
		 * @param text Text content to send.
		 * @param relatedRuntime Optional related runtime context.
		 * @return `true` when send succeeds.
		 */
		virtual bool sendToNotepad(const QString &title, const QString &text,
		                           WorldRuntime *relatedRuntime) = 0;
		/**
		 * @brief Appends text to a notepad document, optionally replacing first.
		 * @param title Notepad title.
		 * @param text Text content to append.
		 * @param replace Replace existing text before append when `true`.
		 * @param relatedRuntime Optional related runtime context.
		 * @return `true` when append succeeds.
		 */
		virtual bool appendToNotepad(const QString &title, const QString &text, bool replace,
		                             WorldRuntime *relatedRuntime) = 0;
		/**
		 * @brief Activates notepad UI if present.
		 * @return `true` when notepad UI is activated.
		 */
		virtual bool switchToNotepad() = 0;
		/**
		 * @brief Shows status bar message for optional timeout duration.
		 * @param message Status message text.
		 * @param timeoutMs Display timeout in milliseconds.
		 */
		virtual void showStatusMessage(const QString &message, int timeoutMs) = 0;
		/**
		 * @brief Activates world tab by slot index.
		 * @param slot Zero-based world slot index.
		 */
		virtual void activateWorldSlot(int slot) = 0;
		/**
		 * @brief Returns descriptors for open world windows in display order.
		 * @return Ordered world window descriptors.
		 */
		[[nodiscard]] virtual QVector<WorldWindowDescriptor> worldWindowDescriptors() const = 0;
};

#endif // QMUD_MAINWINDOWHOST_H
