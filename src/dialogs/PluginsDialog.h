/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: PluginsDialog.h
 * Role: Plugin manager dialog interfaces for listing plugins and controlling enable/disable/load/unload actions.
 */

#ifndef QMUD_PLUGINS_DIALOG_H
#define QMUD_PLUGINS_DIALOG_H

#include <QDialog>
#include <QTableWidget>
#include <QVector>

class WorldRuntime;
class MainWindow;
class QPushButton;

/**
 * @brief Plugin management dialog for install/remove/enable operations.
 */
class PluginsDialog : public QDialog
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates plugin manager dialog for a runtime/main window pair.
		 * @param runtime Runtime whose plugin list is managed.
		 * @param main Main window context.
		 * @param parent Optional Qt parent widget.
		 */
		explicit PluginsDialog(WorldRuntime *runtime, MainWindow *main, QWidget *parent = nullptr);

	protected:
		/**
		 * @brief Schedules sizing after first display or an application-font change.
		 * @param event Event being delivered to the dialog.
		 * @return Result returned by the base dialog event handler.
		 */
		bool event(QEvent *event) override;
		/**
		 * @brief Finalizes first-display sizing when the native window becomes exposed.
		 * @param watched Object receiving the event.
		 * @param event Event delivered to the watched object.
		 * @return Base event-filter result.
		 */
		bool eventFilter(QObject *watched, QEvent *event) override;

	private slots:
		/**
		 * @brief Adds plugin from file and refreshes list.
		 */
		void onAddPlugin();
		/**
		 * @brief Installs the selected plugin file and records persistent membership changes.
		 * @param path Plugin XML file selected by the user.
		 */
		void installPluginFile(const QString &path);
		/**
		 * @brief Removes selected plugin entries.
		 */
		void onRemovePlugin();
		/**
		 * @brief Enables selected plugins.
		 */
		void onEnablePlugin() const;
		/**
		 * @brief Disables selected plugins.
		 */
		void onDisablePlugin() const;
		/**
		 * @brief Reloads selected plugin.
		 */
		void onReloadPlugin();
		/**
		 * @brief Opens selected plugin source for editing.
		 */
		void onEditPlugin() const;
		/**
		 * @brief Shows selected plugin description.
		 */
		void onShowDescription() const;
		/**
		 * @brief Deletes persisted state for selected plugin.
		 */
		void onDeleteState();
		/**
		 * @brief Updates button enablement after selection changes.
		 */
		void onSelectionChanged() const;
		/**
		 * @brief Moves selected plugin up in execution order.
		 */
		void onMoveUp() const;
		/**
		 * @brief Moves selected plugin down in execution order.
		 */
		void onMoveDown() const;
		/**
		 * @brief Reapplies default or preferred column widths after the table sort indicator changes.
		 */
		void refreshColumnWidthsForSortChange() const;

	private:
		/**
		 * @brief Recalculates font-aware button widths and minimum dialog size.
		 */
		void                     refreshDialogSizing();
		/**
		 * @brief Applies one content-driven resize while preserving user-added space.
		 * @param contentMinimum Global content requirement for the dialog.
		 */
		void                     applyDialogContentSize(QSize contentMinimum);
		/**
		 * @brief Queues one sizing refresh after pending native and font metrics settle.
		 */
		void                     scheduleDialogSizingRefresh();
		/**
		 * @brief Queues the one-time native-frame-aware sizing pass after first exposure.
		 */
		void                     finalizeInitialDialogSizing();
		/**
		 * @brief Rebuilds plugin table from runtime state.
		 */
		void                     reloadList() const;
		/**
		 * @brief Returns plugin id for the specified table row.
		 * @param row Table row index.
		 * @return Plugin id for the row.
		 */
		[[nodiscard]] QString    pluginIdForRow(int row) const;
		/**
		 * @brief Returns selected table rows.
		 * @return Selected row indices.
		 */
		[[nodiscard]] QList<int> selectedRows() const;
		/**
		 * @brief Applies relative move to selected plugin(s).
		 * @param delta Relative row delta (`-1` up, `+1` down).
		 */
		void                     movePlugin(int delta) const;
		/**
		 * @brief Persists dialog UI settings.
		 */
		void                     saveSettings() const;
		/**
		 * @brief Restores selection to plugin id after list refresh.
		 * @param pluginId Plugin id to reselect.
		 */
		void                     restoreSelection(const QString &pluginId) const;

		WorldRuntime            *m_runtime{nullptr};
		MainWindow              *m_main{nullptr};
		QTableWidget            *m_table{nullptr};
		QPushButton             *m_addButton{nullptr};
		QPushButton             *m_removeButton{nullptr};
		QPushButton             *m_deleteStateButton{nullptr};
		QPushButton             *m_moveUpButton{nullptr};
		QPushButton             *m_moveDownButton{nullptr};
		QPushButton             *m_enableButton{nullptr};
		QPushButton             *m_disableButton{nullptr};
		QPushButton             *m_reloadButton{nullptr};
		QPushButton             *m_editButton{nullptr};
		QPushButton             *m_showDescriptionButton{nullptr};
		QPushButton             *m_closeButton{nullptr};
		bool                     m_dialogSizingRefreshPending{false};
		bool                     m_initialDialogSizingFinalized{false};
		bool                     m_usingDefaultColumnWidths{true};
		QVector<int>             m_preferredColumnWidths;
		QSize                    m_dialogSizeBeforeRefresh;
		QSize                    m_lastDialogContentMinimum;
		QSize                    m_desiredDialogSize;
		QSize                    m_lastAppliedDialogSize;
};

#endif // QMUD_PLUGINS_DIALOG_H
