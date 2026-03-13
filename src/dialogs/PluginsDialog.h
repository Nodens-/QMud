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

	private slots:
		/**
		 * @brief Adds plugin from file and refreshes list.
		 */
		void onAddPlugin();
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

	private:
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
};

#endif // QMUD_PLUGINS_DIALOG_H
