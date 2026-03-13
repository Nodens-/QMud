/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: ActivityWindow.cpp
 * Role: Activity window behavior, wiring document updates to the docked UI and user interactions.
 */

#include "ActivityWindow.h"
#include "MainWindowHost.h"
#include "MainWindowHostResolver.h"
#include "WorldChildWindow.h"
#include "WorldRuntime.h"

#include <QDateTime>
#include <QHeaderView>
#include <QMenu>
#include <QVBoxLayout>
#include <limits>
#include <memory>

namespace
{
	enum Columns
	{
		eColumnSeq      = 0,
		eColumnMush     = 1,
		eColumnNew      = 2,
		eColumnLines    = 3,
		eColumnStatus   = 4,
		eColumnSince    = 5,
		eColumnDuration = 6,
		eColumnCount    = 7
	};
}

ActivityWindow::ActivityWindow(QWidget *parent) : QWidget(parent)
{
	m_table = new QTableWidget(this);
	m_table->setColumnCount(eColumnCount);
	m_table->setHorizontalHeaderLabels({QStringLiteral("Seq"), QStringLiteral("World"), QStringLiteral("New"),
	                                    QStringLiteral("Lines"), QStringLiteral("Status"),
	                                    QStringLiteral("Since"), QStringLiteral("Duration")});
	m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_table->setSelectionMode(QAbstractItemView::SingleSelection);
	m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_table->horizontalHeader()->setStretchLastSection(true);
	m_table->horizontalHeader()->setSectionsClickable(true);
	m_table->setSortingEnabled(true);
	m_table->setContextMenuPolicy(Qt::CustomContextMenu);

	connect(m_table, &QTableWidget::cellDoubleClicked, this, &ActivityWindow::onCellActivated);
	connect(m_table, &QTableWidget::cellActivated, this, &ActivityWindow::onCellActivated);
	connect(m_table, &QTableWidget::customContextMenuRequested, this, &ActivityWindow::showContextMenu);

	m_refreshTimer = new QTimer(this);
	connect(m_refreshTimer, &QTimer::timeout, this, &ActivityWindow::refresh);
	m_refreshTimer->start(1000);

	refresh();

	auto layout = std::make_unique<QVBoxLayout>();
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(m_table);
	setLayout(layout.release());
}

void ActivityWindow::setGridLinesVisible(const bool visible) const
{
	if (m_table)
		m_table->setShowGrid(visible);
}

ActivityWindow::RowWorld ActivityWindow::worldForRow(const int row) const
{
	RowWorld result;
	if (!m_table || row < 0 || row >= m_table->rowCount())
		return result;

	const QTableWidgetItem *item = m_table->item(row, eColumnMush);
	if (!item)
		return result;

	const QVariant data = item->data(Qt::UserRole);
	if (!data.isValid())
		return result;

	const auto object = data.value<QObject *>();
	result.window     = qobject_cast<WorldChildWindow *>(object);
	result.sequence =
	    m_table->item(row, eColumnSeq) ? m_table->item(row, eColumnSeq)->data(Qt::UserRole).toInt() : 0;
	return result;
}

void ActivityWindow::refresh() const
{
	const MainWindowHost *host = resolveMainWindowHost(window());
	if (!host)
		return;

	const QVector<WorldWindowDescriptor> worlds     = host->worldWindowDescriptors();
	const qsizetype                      worldCount = worlds.size();
	const int rowCount = worldCount > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max()
	                                                                  : static_cast<int>(worldCount);

	m_table->setSortingEnabled(false);
	m_table->setRowCount(rowCount);

	int row = 0;
	for (const auto &[sequence, world, runtime] : worlds)
	{
		const QString name =
		    runtime ? runtime->worldAttributes().value(QStringLiteral("name")) : QStringLiteral("(world)");
		const int newLines = runtime ? runtime->newLines() : 0;

		qsizetype lineCount = 0;
		if (runtime)
		{
			const auto &runtimeLines = runtime->lines();
			lineCount                = runtimeLines.size();
			if (lineCount > 0 && runtimeLines.last().text.isEmpty())
				--lineCount;
		}
		const int     lines = lineCount > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max()
		                                                                  : static_cast<int>(lineCount);

		const bool    connected = runtime && runtime->isConnected();
		const QString status    = connected ? QStringLiteral("Connected") : QStringLiteral("Not connected");

		QString       since;
		QString       duration;
		if (runtime && connected && runtime->connectTime().isValid())
		{
			since = runtime->formatTime(runtime->connectTime(), QStringLiteral("%#I:%M %p, %d %b"), false);
			const qint64 seconds = runtime->connectTime().secsTo(QDateTime::currentDateTime());
			const qint64 days    = seconds / 86400;
			const qint64 hours   = seconds % 86400 / 3600;
			const qint64 minutes = seconds % 3600 / 60;
			const qint64 secs    = seconds % 60;
			if (days > 0)
				duration = QStringLiteral("%1d %2h %3m %4s").arg(days).arg(hours).arg(minutes).arg(secs);
			else if (hours > 0)
				duration = QStringLiteral("%1h %2m %3s").arg(hours).arg(minutes).arg(secs);
			else if (minutes > 0)
				duration = QStringLiteral("%1m %2s").arg(minutes).arg(secs);
			else
				duration = QStringLiteral("%1s").arg(secs);
		}

		auto seqItem = std::make_unique<QTableWidgetItem>(QString::number(sequence));
		seqItem->setData(Qt::UserRole, sequence);
		auto mushItem = std::make_unique<QTableWidgetItem>(name);
		mushItem->setData(Qt::UserRole, QVariant::fromValue<QObject *>(world));
		auto newItem = std::make_unique<QTableWidgetItem>(QString::number(newLines));
		newItem->setData(Qt::UserRole, newLines);
		auto linesItem = std::make_unique<QTableWidgetItem>(QString::number(lines));
		linesItem->setData(Qt::UserRole, lines);
		auto statusItem   = std::make_unique<QTableWidgetItem>(status);
		auto sinceItem    = std::make_unique<QTableWidgetItem>(since);
		auto durationItem = std::make_unique<QTableWidgetItem>(duration);

		m_table->setItem(row, eColumnSeq, seqItem.release());
		m_table->setItem(row, eColumnMush, mushItem.release());
		m_table->setItem(row, eColumnNew, newItem.release());
		m_table->setItem(row, eColumnLines, linesItem.release());
		m_table->setItem(row, eColumnStatus, statusItem.release());
		m_table->setItem(row, eColumnSince, sinceItem.release());
		m_table->setItem(row, eColumnDuration, durationItem.release());

		if (host->activeWorldChildWindow() == world)
			m_table->selectRow(row);

		++row;
		if (row >= rowCount)
			break;
	}

	m_table->setSortingEnabled(true);
}

void ActivityWindow::onCellActivated(const int row, const int column) const
{
	Q_UNUSED(column);
	const auto [sequence, targetWindow] = worldForRow(row);
	if (!targetWindow)
		return;

	MainWindowHost *const host = resolveMainWindowHost(window());
	if (!host)
		return;

	host->activateWorldSlot(sequence);
}

void ActivityWindow::showContextMenu(const QPoint &pos)
{
	const QModelIndex index = m_table->indexAt(pos);
	if (!index.isValid())
		return;

	const auto [sequence, targetWindow] = worldForRow(index.row());
	if (!targetWindow)
		return;

	WorldRuntime *const runtime = targetWindow->runtime();

	QMenu               menu(this);
	const QAction      *switchAction     = menu.addAction(QStringLiteral("Switch to world"));
	QAction            *connectAction    = menu.addAction(QStringLiteral("Connect"));
	QAction            *disconnectAction = menu.addAction(QStringLiteral("Disconnect"));

	if (!runtime)
	{
		connectAction->setEnabled(false);
		disconnectAction->setEnabled(false);
	}
	else
	{
		connectAction->setEnabled(!runtime->isConnected());
		disconnectAction->setEnabled(runtime->isConnected());
	}

	const QAction *chosen = menu.exec(m_table->viewport()->mapToGlobal(pos));
	if (!chosen)
		return;

	MainWindowHost *const host = resolveMainWindowHost(window());
	if (!host)
		return;

	if (chosen == switchAction)
		host->activateWorldSlot(sequence);
	else if (chosen == connectAction && runtime)
	{
		const QMap<QString, QString> &attrs     = runtime->worldAttributes();
		const QString                 worldHost = attrs.value(QStringLiteral("site"));
		if (const quint16 port = attrs.value(QStringLiteral("port")).toUShort();
		    !worldHost.isEmpty() && port > 0)
			runtime->connectToWorld(worldHost, port);
	}
	else if (chosen == disconnectAction && runtime)
		runtime->disconnectFromWorld();
}
