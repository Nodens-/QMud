/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldChildWindow.h
 * Role: MDI child-window interfaces for a single world session, linking world runtime state to visible window chrome.
 */

#ifndef QMUD_WORLDCHILDWINDOW_H
#define QMUD_WORLDCHILDWINDOW_H

#include <QMdiSubWindow>
#include <QPlainTextEdit>
#include <QString>

class ActivityWindow;
class QCloseEvent;
class QEvent;
class QResizeEvent;
class QShowEvent;
class QTimer;
class WorldRuntime;

/**
 * @brief MDI child container for a single world view/runtime pair.
 *
 * Owns autosave/update wiring for the world tab and tracks runtime bindings.
 */
class WorldChildWindow : public QMdiSubWindow
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates a world child window with default title.
		 * @param parent Optional Qt parent widget.
		 */
		explicit WorldChildWindow(QWidget *parent = nullptr);
		/**
		 * @brief Creates a world child window with explicit initial title.
		 * @param title Initial window title.
		 * @param parent Optional Qt parent widget.
		 */
		explicit WorldChildWindow(const QString &title, QWidget *parent = nullptr);
		/**
		 * @brief Binds primary runtime/controller to this window.
		 * @param runtime Runtime instance to bind.
		 */
		void                           setRuntime(WorldRuntime *runtime);
		/**
		 * @brief Binds observer runtime without taking primary control ownership.
		 * @param runtime Runtime instance to observe.
		 */
		void                           setRuntimeObserver(WorldRuntime *runtime);
		/**
		 * @brief Returns currently bound runtime.
		 * @return Bound runtime pointer, or `nullptr`.
		 */
		[[nodiscard]] WorldRuntime    *runtime() const;
		/**
		 * @brief Returns world view widget hosted by this child window.
		 * @return Hosted world view pointer, or `nullptr`.
		 */
		[[nodiscard]] class WorldView *view() const;

	protected:
		/**
		 * @brief Handles close semantics including autosave/query behavior.
		 * @param event Close-event payload.
		 */
		void closeEvent(QCloseEvent *event) override;
		/**
		 * @brief Finalizes first-show behavior and delayed initialization.
		 * @param event Show-event payload.
		 */
		void showEvent(QShowEvent *event) override;
		/**
		 * @brief Propagates resize handling to hosted view/runtime.
		 * @param event Resize-event payload.
		 */
		void resizeEvent(QResizeEvent *event) override;
		/**
		 * @brief Processes custom events used by runtime/window coordination.
		 * @param event Event payload.
		 * @return `true` when event is handled.
		 */
		bool event(QEvent *event) override;

	private:
		enum class RuntimeBindingRole
		{
			Primary,
			Observer
		};
		/**
		 * @brief Builds internal UI widgets and action bindings.
		 * @param title Initial window title.
		 */
		void                         initializeWorldUi(const QString &title);
		/**
		 * @brief Reacts to runtime world-attribute changes.
		 * @param key Changed world attribute key.
		 */
		void                         onWorldAttributeChanged(const QString &key);
		/**
		 * @brief Starts/stops autosave timer according to settings.
		 */
		void                         refreshAutosaveTimer() const;
		/**
		 * @brief Executes one autosave cycle.
		 */
		void                         handleAutosaveTick();
		/**
		 * @brief Internal helper for runtime binding in selected role.
		 * @param worldRuntime Runtime pointer to bind.
		 * @param role Binding role.
		 */
		void                         bindRuntime(class WorldRuntime *worldRuntime, RuntimeBindingRole role);
		/**
		 * @brief Installs deferred plugin packages for the bound runtime.
		 */
		void                         tryInstallPendingPlugins() const;
		class WorldRuntime          *m_runtime{nullptr};
		class WorldView             *m_view{nullptr};
		class WorldCommandProcessor *m_commandProcessor{nullptr};
		class MxpDebugWindow        *m_mxpDebug{nullptr};
		QTimer                      *m_autosaveTimer{nullptr};
		bool                         m_primaryRuntimeBinding{false};
		bool                         m_autosaveInFlight{false};
};

/**
 * @brief MDI child window hosting an activity feed widget.
 */
class ActivityChildWindow : public QMdiSubWindow
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates an activity child window with default title.
		 * @param parent Optional Qt parent widget.
		 */
		explicit ActivityChildWindow(QWidget *parent = nullptr);
		/**
		 * @brief Creates an activity child window with explicit title.
		 * @param title Initial window title.
		 * @param parent Optional Qt parent widget.
		 */
		explicit ActivityChildWindow(const QString &title, QWidget *parent = nullptr);

		/**
		 * @brief Returns hosted activity widget.
		 * @return Hosted activity widget pointer.
		 */
		[[nodiscard]] ActivityWindow *activityWindow() const;
};

/**
 * @brief MDI child window hosting a plain text viewer/editor surface.
 */
class TextChildWindow : public QMdiSubWindow
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates a text child window with empty content.
		 * @param parent Optional Qt parent widget.
		 */
		explicit TextChildWindow(QWidget *parent = nullptr);
		/**
		 * @brief Creates a text child window with title and initial content.
		 * @param title Initial window title.
		 * @param text Initial editor content.
		 * @param parent Optional Qt parent widget.
		 */
		explicit TextChildWindow(const QString &title, const QString &text, QWidget *parent = nullptr);
		/**
		 * @brief Returns hosted plain-text editor.
		 * @return Hosted editor pointer.
		 */
		[[nodiscard]] QPlainTextEdit *editor() const;
		/**
		 * @brief Replaces editor content with given text.
		 * @param text New editor text.
		 */
		void                          setText(const QString &text) const;
		/**
		 * @brief Appends text to editor content.
		 * @param text Text fragment to append.
		 */
		void                          appendText(const QString &text) const;
		/**
		 * @brief Returns associated file path.
		 * @return Current associated file path.
		 */
		[[nodiscard]] QString         filePath() const;
		/**
		 * @brief Sets associated file path.
		 * @param path File path to associate with this window.
		 */
		void                          setFilePath(const QString &path);
		/**
		 * @brief Saves content to specified file path.
		 * @param path Destination file path.
		 * @param error Optional output error text.
		 * @return `true` on successful save.
		 */
		bool                          saveToFile(const QString &path, QString *error);
		/**
		 * @brief Saves content to current associated file path.
		 * @param error Optional output error text.
		 * @return `true` on successful save.
		 */
		bool                          saveToCurrentFile(QString *error);
		/**
		 * @brief Enables/disables close-time save confirmation.
		 * @param querySave Prompt for save on close when `true`.
		 */
		void                          setQuerySaveOnClose(bool querySave);

	protected:
		/**
		 * @brief Prompts for save if required before closing.
		 * @param event Close-event payload.
		 */
		void closeEvent(QCloseEvent *event) override;

	private:
		/**
		 * @brief Internal save-confirmation helper used during close.
		 * @param querySave Prompt for save when `true`.
		 * @return `true` when close can proceed.
		 */
		bool maybeSaveBeforeClose(bool querySave);

	private:
		QPlainTextEdit *m_editor{nullptr};
		QString         m_filePath;
		bool            m_querySaveOnClose{true};
		bool            m_querySaveOverrideSet{false};
};

#endif // QMUD_WORLDCHILDWINDOW_H
