/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: ActivityDocument.h
 * Role: Document model interfaces for the Activity pane that stores rendered lines, metadata, and scrollback state.
 */

#ifndef QMUD_ACTIVITYDOCUMENT_H
#define QMUD_ACTIVITYDOCUMENT_H

#include <QObject>

class AppController;

/**
 * @brief Document/controller model for activity output windows.
 *
 * Provides load/save/open workflows for activity text content.
 */
class ActivityDocument : public QObject
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates an activity document bound to the application controller.
		 * @param app Owning application controller.
		 * @param parent Optional Qt parent object.
		 */
		explicit ActivityDocument(AppController *app, QObject *parent = nullptr);
		/**
		 * @brief Destroys the activity document instance.
		 */
		~ActivityDocument() override;

		/**
		 * @brief Creates a new empty activity document through the UI flow.
		 * @return `true` when the new-document flow completes successfully.
		 */
		static bool onNewDocument();
		/**
		 * @brief Opens an activity document from disk.
		 */
		void        onFileOpen() const;

	private:
		AppController *m_app{nullptr};
};

#endif // QMUD_ACTIVITYDOCUMENT_H
