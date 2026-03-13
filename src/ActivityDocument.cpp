/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: ActivityDocument.cpp
 * Role: Activity pane document model logic for buffering, updating, and exposing runtime messages to the UI.
 */

#include "ActivityDocument.h"
#include "AppController.h"

#include <QFileDialog>

ActivityDocument::ActivityDocument(AppController *app, QObject *parent) : QObject(parent), m_app(app)
{
	if (m_app)
		m_app->setActivityDocument(this);
}

ActivityDocument::~ActivityDocument()
{
	if (m_app)
		m_app->setActivityDocument(nullptr);
}

bool ActivityDocument::onNewDocument()
{
	return true;
}

void ActivityDocument::onFileOpen() const
{
	if (!m_app)
		return;

	// use default world file directory
	const QString initialDir = m_app->makeAbsolutePath(m_app->defaultWorldDirectory());

	m_app->changeToFileBrowsingDirectory();
	const QString fileName = QFileDialog::getOpenFileName(
	    nullptr, QStringLiteral("Open"), initialDir,
	    QStringLiteral("World or text files (*.qdl;*.mcl;*.txt);;All files (*.*)"));
	m_app->changeToStartupDirectory();

	if (fileName.isEmpty())
		return;

	m_app->openDocumentFile(fileName);
}
