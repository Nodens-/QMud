/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MxpDebugWindow.cpp
 * Role: MXP debug-window implementation used to surface parser diagnostics and runtime MXP event traces to users.
 */

#include "MxpDebugWindow.h"
#include "AppController.h"

#include <QMessageBox>
#include <QPlainTextEdit>
#include <QVBoxLayout>

MxpDebugWindow::MxpDebugWindow(QWidget *parent) : QWidget(parent)
{
	setLayout(new QVBoxLayout);
	m_output = new QPlainTextEdit(this);
	m_output->setReadOnly(true);
	layout()->addWidget(m_output);
	resize(600, 300);
}

void MxpDebugWindow::setTitle(const QString &title)
{
	setWindowTitle(title);
}

void MxpDebugWindow::appendMessage(const QString &message) const
{
	if (!m_output)
		return;
	m_output->appendPlainText(message);
}

void MxpDebugWindow::closeEvent(QCloseEvent *event)
{
	AppController *app = AppController::instance();
	if (app && app->getGlobalOption(QStringLiteral("ConfirmBeforeClosingMXPdebug")).toInt() != 0)
	{
		if (m_output && !m_output->document()->isEmpty())
		{
			const QMessageBox::StandardButton result = QMessageBox::information(
			    this, QStringLiteral("QMud"), QStringLiteral("Close MXP debug window?"),
			    QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);
			if (result != QMessageBox::Ok)
			{
				event->ignore();
				return;
			}
		}
	}

	QWidget::closeEvent(event);
}
