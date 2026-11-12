/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: GmcpBrowserDialog.h
 * Role: Independent current and timestamped historical GMCP inspection views.
 */
#ifndef QMUD_GMCPBROWSERDIALOG_H
#define QMUD_GMCPBROWSERDIALOG_H
#include <QPointer>
#include <QWidget>
#include <optional>
#include <utility>
class WorldRuntime;
class QLabel;
class QLineEdit;
class QCheckBox;
class QTableView;
class GmcpBrowserDataView;
/**
 * @brief Modeless browser with independent Current and History tree/raw views.
 */
class GmcpBrowserDialog final : public QWidget
{
	public:
		/**
		 * @brief Opens or raises the world's single browser.
		 * @param runtime Owning world, or null for debug layout inspection.
		 * @param parent GUI owner.
		 */
		static void open(WorldRuntime *runtime, QWidget *parent);
		/**
		 * @brief Constructs an inspector.
		 * @param runtime Owning world, or null for an empty view.
		 * @param parent GUI owner.
		 */
		explicit GmcpBrowserDialog(WorldRuntime *runtime, QWidget *parent = nullptr);

	private:
		/**
		 * @brief Coalesces status and connection notifications into one GUI refresh.
		 */
		void                                       scheduleRefresh();
		/**
		 * @brief Updates status and binds a new connection's existing publication when ownership changes.
		 */
		void                                       refresh();
		/**
		 * @brief Binds the current native publication for initial inspection or explicit resume.
		 */
		void                                       bindCurrent();
		/**
		 * @brief Displays the version belonging to the selected history timestamp.
		 */
		void                                       showHistory();
		QPointer<WorldRuntime>                     m_runtime;
		QLabel                                    *m_status{nullptr};
		QLineEdit                                 *m_filter{nullptr};
		QCheckBox                                 *m_pause{nullptr};
		GmcpBrowserDataView                       *m_current{nullptr};
		GmcpBrowserDataView                       *m_past{nullptr};
		QTableView                                *m_history{nullptr};
		std::optional<std::pair<quint64, quint64>> m_historyIdentity;
		std::optional<std::pair<quint64, quint64>> m_historySelection;
		bool                                       m_historySelectionChanging{false};
		bool                                       m_refreshQueued{false};
		bool                                       m_historyOpened{false};
		bool                                       m_historySelectionEvicted{false};
		quint64                                    m_currentSession{0};
		quint64                                    m_currentRevision{0};
};
#endif // QMUD_GMCPBROWSERDIALOG_H
