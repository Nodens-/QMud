/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: ActivityWindow.h
 * Role: Window interfaces for the Activity view container that hosts and coordinates ActivityDocument presentation.
 */

#ifndef QMUD_ACTIVITYWINDOW_H
#define QMUD_ACTIVITYWINDOW_H

#include <QTableWidget>
#include <QTimer>
#include <QWidget>

class WorldChildWindow;

/**
 * @brief Visual widget that renders activity/news-style document text.
 */
class ActivityWindow : public QWidget
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates the activity table widget and timers.
		 * @param parent Optional Qt parent widget.
		 */
		explicit ActivityWindow(QWidget *parent = nullptr);
		/**
		 * @brief Toggles grid line visibility in the activity table.
		 * @param visible Show grid lines when `true`.
		 */
		void setGridLinesVisible(bool visible) const;

		/**
		 * @brief Rebuilds displayed rows from current world activity state.
		 */
		void refresh() const;

	private slots:
		/**
		 * @brief Activates world/tab for a clicked activity row.
		 * @param row Clicked row index.
		 * @param column Clicked column index.
		 */
		void onCellActivated(int row, int column) const;
		/**
		 * @brief Shows row context menu at table-local position.
		 * @param pos Table-local position.
		 */
		void showContextMenu(const QPoint &pos);

	private:
		struct RowWorld
		{
				int               sequence{0};
				WorldChildWindow *window{nullptr};
		};

		/**
		 * @brief Resolves world window descriptor for a given table row.
		 * @param row Table row index.
		 * @return Row-to-world mapping descriptor.
		 */
		[[nodiscard]] RowWorld worldForRow(int row) const;

		QTableWidget          *m_table{nullptr};
		QTimer                *m_refreshTimer{nullptr};
};

#endif // QMUD_ACTIVITYWINDOW_H
