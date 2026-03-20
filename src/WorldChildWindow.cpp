/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldChildWindow.cpp
 * Role: World child-window implementation managing per-session window lifecycle, focus, and integration with
 * frame/tabs.
 */

#include "WorldChildWindow.h"
#include "ActivityWindow.h"
#include "AppController.h"
#include "DocConstants.h"
#include "FileExtensions.h"
#include "MainWindowHost.h"
#include "MainWindowHostResolver.h"
#include "MxpDebugWindow.h"
#include "WorldCommandProcessor.h"
#include "WorldRuntime.h"
#include "WorldView.h"
#include <QCloseEvent>
#include <QColor>
#include <QCoreApplication>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSaveFile>
#include <QTextCursor>
#include <QTextOption>
#include <QTimer>
#include <limits>

namespace
{
	bool isEnabledFlagValue(const QString &value)
	{
		return value == QStringLiteral("1") || value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
		       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
	}

	QString ensureWorldFilePathForRuntime(WorldRuntime *runtime, const AppController *app)
	{
		QString filePath = runtime ? runtime->worldFilePath() : QString();
		if (!runtime || !filePath.trimmed().isEmpty())
			return filePath;

		QString baseName =
		    runtime->worldAttributes().value(QStringLiteral("name"), QStringLiteral("World")).trimmed();
		if (baseName.isEmpty())
			baseName = QStringLiteral("World");
		static const auto invalid = QStringLiteral("<>\"|?:#%;/\\");
		for (const QChar &ch : invalid)
			baseName.replace(ch, QLatin1Char('_'));
		if (!baseName.endsWith(QStringLiteral(".qdl"), Qt::CaseInsensitive))
			baseName += QStringLiteral(".qdl");
		baseName = QMudFileExtensions::replaceOrAppendExtension(baseName, QStringLiteral("qdl"));

		QString baseDir = app ? app->defaultWorldDirectory() : QString();
		if (baseDir.isEmpty())
		{
			if (app)
				baseDir = app->makeAbsolutePath(QStringLiteral("."));
			else
				baseDir = QCoreApplication::applicationDirPath();
		}
		const QDir dir(baseDir);
		dir.mkpath(QStringLiteral("."));
		filePath = dir.filePath(baseName);
		runtime->setWorldFilePath(filePath);
		return filePath;
	}

	bool shouldSaveDirtyWorldState(const WorldRuntime *runtime, const AppController *app)
	{
		if (!runtime)
			return false;
		if (runtime->worldFileModified())
			return true;
		if (!runtime->variablesChanged())
			return false;
		return app && app->getGlobalOption(QStringLiteral("ConfirmBeforeSavingVariables")).toInt() != 0;
	}

	int autosaveIntervalMinutes(const WorldRuntime &runtime)
	{
		bool ok      = false;
		int  minutes = runtime.worldAttributes().value(QStringLiteral("autosave_minutes")).toInt(&ok);
		if (!ok)
			minutes = 60;
		return minutes < 0 ? 0 : minutes;
	}
} // namespace

WorldChildWindow::WorldChildWindow(QWidget *parent) : QMdiSubWindow(parent)
{
	initializeWorldUi(QStringLiteral("World"));
}

WorldChildWindow::WorldChildWindow(const QString &title, QWidget *parent) : QMdiSubWindow(parent)
{
	initializeWorldUi(title);
}

void WorldChildWindow::initializeWorldUi(const QString &title)
{
	setAttribute(Qt::WA_DeleteOnClose, true);
	setWindowTitle(title);
	m_view             = new WorldView(this);
	m_commandProcessor = new WorldCommandProcessor(this);
	m_commandProcessor->setView(m_view);
	connect(m_view, &WorldView::sendText, m_commandProcessor, &WorldCommandProcessor::onCommandEntered);
	connect(m_view, &WorldView::outputSelectionChanged, this,
	        [this]
	        {
		        if (MainWindowHost *main = resolveMainWindowHost(window()))
			        main->updateEditActions();
	        });
	connect(m_view, &WorldView::inputSelectionChanged, this,
	        [this]
	        {
		        if (MainWindowHost *main = resolveMainWindowHost(window()))
			        main->updateEditActions();
	        });
	connect(m_view, &WorldView::outputScrollChanged, this,
	        [this]
	        {
		        if (MainWindowHost *main = resolveMainWindowHost(window()))
			        main->updateStatusBar();
	        });
	connect(m_view, &WorldView::freezeStateChanged, this,
	        [this](const bool)
	        {
		        if (MainWindowHost *main = resolveMainWindowHost(window()))
		        {
			        main->updateStatusBar();
			        main->refreshActionState();
		        }
	        });
	setWidget(m_view);
	m_autosaveTimer = new QTimer(this);
	connect(m_autosaveTimer, &QTimer::timeout, this, &WorldChildWindow::handleAutosaveTick);
	m_autosaveTimer->stop();
}

void WorldChildWindow::setRuntime(WorldRuntime *runtime)
{
	bindRuntime(runtime, RuntimeBindingRole::Primary);
}

void WorldChildWindow::setRuntimeObserver(WorldRuntime *runtime)
{
	bindRuntime(runtime, RuntimeBindingRole::Observer);
}

void WorldChildWindow::bindRuntime(WorldRuntime *worldRuntime, const RuntimeBindingRole role)
{
	const bool primary      = role == RuntimeBindingRole::Primary;
	m_runtime               = worldRuntime;
	m_primaryRuntimeBinding = primary;
	m_autosaveInFlight      = false;
	refreshAutosaveTimer();
	if (!worldRuntime || !m_view)
		return;

	if (const QString worldName = worldRuntime->worldAttributes().value(QStringLiteral("name"));
	    !worldName.isEmpty())
	{
		setWindowTitle(worldName);
		m_view->setWorldName(worldName);
	}

	if (primary)
		m_view->setRuntime(worldRuntime);
	else
		m_view->setRuntimeObserver(worldRuntime);
	connect(worldRuntime, &WorldRuntime::worldAttributeChanged, this,
	        &WorldChildWindow::onWorldAttributeChanged, Qt::UniqueConnection);
	if (m_commandProcessor)
		m_commandProcessor->setRuntime(worldRuntime);
	if (primary)
		worldRuntime->setCommandProcessor(m_commandProcessor);
	if (m_commandProcessor)
	{
		connect(worldRuntime, &WorldRuntime::incomingStyledLineReceived, m_commandProcessor,
		        &WorldCommandProcessor::onIncomingStyledLineReceived);
		connect(worldRuntime, &WorldRuntime::incomingStyledLinePartialReceived, m_commandProcessor,
		        &WorldCommandProcessor::onIncomingStyledLinePartialReceived);
		connect(worldRuntime, &WorldRuntime::incomingStyledLineReceived, this,
		        [this](const QString &, const QVector<WorldRuntime::StyleSpan> &)
		        {
			        if (MainWindowHost *main = resolveMainWindowHost(window()))
				        main->requestDeferredUiRefresh(true, true, true);
		        });
		connect(worldRuntime, &WorldRuntime::logStateChanged, this,
		        [this](bool)
		        {
			        if (MainWindowHost *main = resolveMainWindowHost(window()))
			        {
				        main->updateStatusBar();
				        main->refreshActionState();
			        }
		        });
		connect(worldRuntime, &WorldRuntime::scriptFileChangedDetected, this,
		        [this, worldRuntime]
		        {
			        const MainWindowHost *main = resolveMainWindowHost(window());
			        if (!main)
				        return;
			        if (main->activeWorldChildWindow() != this)
				        return;
			        if (AppController *app = AppController::instance())
				        app->processScriptFileChange(worldRuntime);
		        });
	}
	if (m_view)
	{
		connect(worldRuntime, &WorldRuntime::incomingHtmlReceived, m_view,
		        [this](const QString &html)
		        {
			        if (m_view)
				        m_view->appendOutputHtml(html);
		        });
		connect(worldRuntime, &WorldRuntime::outputRequested, m_view,
		        [this](const QString &text, const bool newLine, const bool note)
		        {
			        if (!m_view)
				        return;
			        if (note)
				        m_view->appendNoteText(text, newLine);
			        else
				        m_view->appendOutputText(text, newLine);
		        });
		connect(worldRuntime, &WorldRuntime::outputStyledRequested, m_view,
		        [this](const QString &text, const QVector<WorldRuntime::StyleSpan> &spans, const bool newLine,
		               const bool note)
		        {
			        if (!m_view)
				        return;
			        if (note)
				        m_view->appendNoteTextStyled(text, spans, newLine);
			        else
				        m_view->appendOutputTextStyled(text, spans, newLine);
		        });
		connect(worldRuntime, &WorldRuntime::miniWindowsChanged, m_view,
		        [this]
		        {
			        if (m_view)
				        m_view->refreshMiniWindows();
		        });
		connect(m_view, &WorldView::outputSelectionChanged, worldRuntime,
		        [worldRuntime]
		        {
			        if (worldRuntime)
				        worldRuntime->notifyOutputSelectionChanged();
		        });
		connect(m_view, &WorldView::hyperlinkHighlighted, this,
		        [this](const QString &href)
		        {
			        if (MainWindowHost *main = resolveMainWindowHost(window()))
			        {
				        if (!href.isEmpty())
					        main->setHyperlinkStatusLock(href);
				        else
				        {
					        if (m_view && m_view->hyperlinkHoverActive())
						        return;
					        main->clearHyperlinkStatusLock();
				        }
			        }
		        });
		connect(worldRuntime, &WorldRuntime::windowTitleChanged, this,
		        [this, worldRuntime]
		        {
			        if (!worldRuntime)
				        return;
			        if (const QString overrideTitle = worldRuntime->windowTitleOverride();
			            !overrideTitle.isEmpty())
			        {
				        setWindowTitle(overrideTitle);
				        if (m_view)
					        m_view->setWorldName(overrideTitle);
				        if (MainWindowHost *main = resolveMainWindowHost(window()))
					        main->showStatusMessage(overrideTitle, 0);
			        }
			        if (MainWindowHost *main = resolveMainWindowHost(window()))
				        main->updateMdiTabs();
		        });
		connect(worldRuntime, &WorldRuntime::mainTitleChanged, this,
		        [this]
		        {
			        if (MainWindowHost *main = resolveMainWindowHost(window()))
				        main->refreshTitleBar();
		        });
		connect(worldRuntime, &WorldRuntime::connected, this,
		        [this, worldRuntime]
		        {
			        if (!m_view || !worldRuntime)
				        return;
			        bool          shouldFocusInput = false;
			        const QString flag =
			            worldRuntime->worldAttributes().value(QStringLiteral("show_connect_disconnect"));
			        const bool show = flag.isEmpty() ||
			                          flag.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
			                          flag == QStringLiteral("1") ||
			                          flag.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
			        if (!show)
				        return;
			        const QString when =
			            QDateTime::currentDateTime().toString(QStringLiteral("dddd, MMMM dd, yyyy, h:mm AP"));
			        if (m_commandProcessor)
				        m_commandProcessor->note(QStringLiteral("--- Connected on %1 ---").arg(when), true);
			        else if (m_view)
				        m_view->appendNoteText(QStringLiteral("--- Connected on %1 ---").arg(when), true);
			        if (MainWindowHost *main = resolveMainWindowHost(window()))
			        {
				        main->setConnectedState(true);
				        main->refreshTitleBar();
				        main->updateActivityToolbarButtons();
				        shouldFocusInput = main->activeWorldChildWindow() == this;
			        }
			        if (m_commandProcessor)
				        m_commandProcessor->handleWorldConnected();
			        if (m_view && shouldFocusInput)
				        m_view->focusInput();
		        });
		connect(worldRuntime, &WorldRuntime::disconnected, this,
		        [this]
		        {
			        if (MainWindowHost *main = resolveMainWindowHost(window()))
				        main->updateActivityToolbarButtons();
		        });
		connect(
		    worldRuntime, &WorldRuntime::socketError, this,
		    [this, worldRuntime](const QString &message)
		    {
			    if (!m_view || !worldRuntime)
				    return;
			    // Connection-failure messaging is only valid while establishing a session.
			    // Once connected (or intentionally disconnecting), let the disconnected path
			    // handle user-facing messaging with current UI wording/flags.
			    if (const int phase = worldRuntime->connectPhase();
			        phase == WorldRuntime::eConnectConnectedToMud ||
			        phase == WorldRuntime::eConnectDisconnecting)
				    return;
			    const QMap<QString, QString> &attrs = worldRuntime->worldAttributes();
			    const bool                    notify =
			        attrs.value(QStringLiteral("notify_if_cannot_connect"), QStringLiteral("1")).toInt() != 0;
			    if (!notify)
				    return;
			    const bool toOutput =
			        attrs.value(QStringLiteral("error_notification_to_output"), QStringLiteral("1"))
			            .toInt() != 0;
			    const QString runtimeWorldName = attrs.value(QStringLiteral("name"), QStringLiteral("world"));
			    const QString host             = attrs.value(QStringLiteral("site"));
			    const QString port             = attrs.value(QStringLiteral("port"));
			    QString       details          = message;
			    if (!host.isEmpty())
				    details = QStringLiteral("%1 (%2:%3)").arg(message, host, port);
			    const QString text =
			        QStringLiteral("Unable to connect to \"%1\": %2").arg(runtimeWorldName, details);
			    if (toOutput)
			    {
				    if (m_commandProcessor)
				    {
					    m_commandProcessor->note(text, true);
					    m_commandProcessor->note(QString(), true);
					    m_commandProcessor->note(
					        QStringLiteral("For assistance with connection problems see:"), true);
				    }
				    else
				    {
					    m_view->appendNoteText(text, true);
					    m_view->appendNoteText(QString(), true);
					    m_view->appendNoteText(QStringLiteral("For assistance with connection problems see:"),
					                           true);
				    }
				    const QString forumLink = QString::fromLatin1(FORUM_URL);
				    m_view->appendOutputHtml(
				        QStringLiteral("<a href=\"%1\">%2</a>")
				            .arg(forumLink.toHtmlEscaped(),
				                 QStringLiteral("How to resolve network connection problems")),
				        true);
				    if (m_commandProcessor)
					    m_commandProcessor->note(QString(), true);
				    else
					    m_view->appendNoteText(QString(), true);
			    }
			    else
				    QMessageBox::warning(this, QStringLiteral("QMud"), text);
		    });
		connect(
		    worldRuntime, &WorldRuntime::disconnected, this,
		    [this, worldRuntime]
		    {
			    if (!m_view || !worldRuntime)
				    return;
			    const QString flag =
			        worldRuntime->worldAttributes().value(QStringLiteral("show_connect_disconnect"));
			    const bool show = flag.isEmpty() ||
			                      flag.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
			                      flag == QStringLiteral("1") ||
			                      flag.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
			    if (!show)
				    return;
			    const QString when =
			        QDateTime::currentDateTime().toString(QStringLiteral("dddd, MMMM dd, yyyy, h:mm AP"));
			    if (m_commandProcessor)
				    m_commandProcessor->note(QStringLiteral("--- Disconnected on %1 ---").arg(when), true);
			    else if (m_view)
				    m_view->appendNoteText(QStringLiteral("--- Disconnected on %1 ---").arg(when), true);
			    if (worldRuntime->connectTime().isValid())
			    {
				    const qint64  seconds = worldRuntime->connectTime().secsTo(QDateTime::currentDateTime());
				    const qint64  days    = seconds / 86400;
				    const qint64  hours   = seconds % 86400 / 3600;
				    const qint64  minutes = seconds % 3600 / 60;
				    const qint64  secs    = seconds % 60;
				    const QString duration =
				        QStringLiteral("--- Connected for %1 day%2, %3 hour%4, %5 minute%6, %7 second%8. ---")
				            .arg(days)
				            .arg(days == 1 ? QString() : QStringLiteral("s"))
				            .arg(hours)
				            .arg(hours == 1 ? QString() : QStringLiteral("s"))
				            .arg(minutes)
				            .arg(minutes == 1 ? QString() : QStringLiteral("s"))
				            .arg(secs)
				            .arg(secs == 1 ? QString() : QStringLiteral("s"));
				    if (m_commandProcessor)
					    m_commandProcessor->note(duration, true);
				    else if (m_view)
					    m_view->appendNoteText(duration, true);
			    }
			    if (m_commandProcessor)
			    {
				    const int     received       = worldRuntime->totalLinesReceived();
				    const int     sent           = worldRuntime->totalLinesSent();
				    const QString receivedPlural = received == 1 ? QString() : QStringLiteral("s");
				    const QString sentPlural     = sent == 1 ? QString() : QStringLiteral("s");
				    const QString info           = QStringLiteral("--- Received %1 line%2, sent %3 line%4.")
				                             .arg(received)
				                             .arg(receivedPlural)
				                             .arg(sent)
				                             .arg(sentPlural);
				    m_commandProcessor->note(info, true);

				    const int maxLines =
				        worldRuntime->worldAttributes().value(QStringLiteral("max_output_lines")).toInt();
				    if (maxLines > 0)
				    {
					    const auto   count = worldRuntime->lines().size();
					    const double percent =
					        static_cast<double>(count) / static_cast<double>(maxLines) * 100.0;
					    const QString countPlural = count == 1 ? QString() : QStringLiteral("s");
					    const QString bufferInfo =
					        QStringLiteral("--- Output buffer has %1/%2 line%3 in it (%4% full).")
					            .arg(count)
					            .arg(maxLines)
					            .arg(countPlural)
					            .arg(QString::number(percent, 'f', 1));
					    m_commandProcessor->note(bufferInfo, true);
				    }

				    const int     triggers    = worldRuntime->triggersMatchedThisSession();
				    const int     aliases     = worldRuntime->aliasesMatchedThisSession();
				    const int     timers      = worldRuntime->timersFiredThisSession();
				    const QString trigPlural  = triggers == 1 ? QString() : QStringLiteral("s");
				    const QString aliasPlural = aliases == 1 ? QString() : QStringLiteral("es");
				    const QString timerPlural = timers == 1 ? QString() : QStringLiteral("s");
				    const QString matchInfo =
				        QStringLiteral("--- Matched %1 trigger%2, %3 alias%4, and %5 timer%6 fired.")
				            .arg(triggers)
				            .arg(trigPlural)
				            .arg(aliases)
				            .arg(aliasPlural)
				            .arg(timers)
				            .arg(timerPlural);
				    m_commandProcessor->note(matchInfo, true);
			    }
			    const QMap<QString, QString> &attrs = worldRuntime->worldAttributes();
			    const bool                    notifyDisconnect =
			        attrs.value(QStringLiteral("notify_on_disconnect"), QStringLiteral("1")).toInt() != 0;
			    const bool toOutput =
			        attrs.value(QStringLiteral("error_notification_to_output"), QStringLiteral("1"))
			            .toInt() != 0;
			    if (const bool expectedDisconnect = worldRuntime->disconnectOk();
			        notifyDisconnect && !expectedDisconnect)
			    {
				    if (!toOutput)
				    {
					    QMessageBox::information(this, QStringLiteral("QMud"),
					                             QStringLiteral("The server has closed the connection."));
				    }
				    else if (m_commandProcessor)
				    {
					    const QString disconnectedWorldName =
					        attrs.value(QStringLiteral("name"), QStringLiteral("world"));
					    const QString msg = QStringLiteral("The \"%1\" server has closed the connection")
					                            .arg(disconnectedWorldName);
					    m_commandProcessor->note(msg, true);
				    }
				    else if (MainWindowHost *main = resolveMainWindowHost(window()))
				    {
					    const QString disconnectedWorldName =
					        attrs.value(QStringLiteral("name"), QStringLiteral("world"));
					    const QString msg = QStringLiteral("The \"%1\" server has closed the connection")
					                            .arg(disconnectedWorldName);
					    main->showStatusMessage(msg, 5000);
				    }
			    }
			    else if (MainWindowHost *main = resolveMainWindowHost(window()))
			    {
				    const QString disconnectedWorldName =
				        attrs.value(QStringLiteral("name"), QStringLiteral("world"));
				    const QString msg = QStringLiteral("The \"%1\" server has closed the connection")
				                            .arg(disconnectedWorldName);
				    main->showStatusMessage(msg, 5000);
			    }
			    if (MainWindowHost *main = resolveMainWindowHost(window()))
			    {
				    main->setConnectedState(false);
				    main->refreshTitleBar();
			    }
			    if (m_commandProcessor)
				    m_commandProcessor->handleWorldDisconnected();
		    });
		connect(worldRuntime, &WorldRuntime::mxpDebugMessage, this,
		        [this](const QString &title, const QString &message)
		        {
			        if (!m_mxpDebug)
			        {
				        QWidget *host = parentWidget();
				        m_mxpDebug    = new MxpDebugWindow(host);
				        m_mxpDebug->setAttribute(Qt::WA_DeleteOnClose, false);
			        }
			        m_mxpDebug->setTitle(title);
			        m_mxpDebug->appendMessage(message);
			        if (!m_mxpDebug->isVisible())
				        m_mxpDebug->show();
			        m_mxpDebug->raise();
		        });
		connect(m_view, &WorldView::hyperlinkActivated, m_commandProcessor,
		        &WorldCommandProcessor::onHyperlinkActivated);
	}
	tryInstallPendingPlugins();
	if (primary)
	{
		worldRuntime->syncChatAcceptCallsWithPreferences();
		worldRuntime->fireWorldOpenHandlers();
	}
}

void WorldChildWindow::refreshAutosaveTimer() const
{
	if (!m_autosaveTimer)
		return;

	if (!m_primaryRuntimeBinding)
	{
		m_autosaveTimer->stop();
		return;
	}

	const WorldRuntime *runtime = m_runtime;
	if (!runtime)
	{
		m_autosaveTimer->stop();
		return;
	}

	const int minutes = autosaveIntervalMinutes(*runtime);
	if (minutes <= 0)
	{
		m_autosaveTimer->stop();
		return;
	}

	const qint64 intervalMs64 = static_cast<qint64>(minutes) * 60 * 1000;
	const int    intervalMs = intervalMs64 > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max()
	                                                                         : static_cast<int>(intervalMs64);
	if (m_autosaveTimer->interval() != intervalMs)
		m_autosaveTimer->setInterval(intervalMs);
	if (!m_autosaveTimer->isActive())
		m_autosaveTimer->start();
}

void WorldChildWindow::onWorldAttributeChanged(const QString &key)
{
	if (key == QStringLiteral("autosave_minutes"))
		refreshAutosaveTimer();

	if (key != QStringLiteral("name"))
		return;

	const WorldRuntime *runtime = m_runtime;
	if (!runtime)
		return;

	if (!runtime->windowTitleOverride().isEmpty())
		return;

	QString worldName = runtime->worldAttributes().value(QStringLiteral("name")).trimmed();
	if (worldName.isEmpty())
		worldName = QStringLiteral("World");

	setWindowTitle(worldName);
	if (m_view)
		m_view->setWorldName(worldName);

	if (MainWindowHost *main = resolveMainWindowHost(window()))
	{
		main->updateMdiTabs();
		main->refreshTitleBar();
	}
}

WorldRuntime *WorldChildWindow::runtime() const
{
	return m_runtime;
}

WorldView *WorldChildWindow::view() const
{
	return m_view;
}

void WorldChildWindow::handleAutosaveTick()
{
	refreshAutosaveTimer();

	if (!m_primaryRuntimeBinding)
		return;

	WorldRuntime *runtime = m_runtime;
	if (!runtime)
		return;

	AppController *app = AppController::instance();
	if (!shouldSaveDirtyWorldState(runtime, app))
		return;
	if (m_autosaveInFlight)
		return;

	const QString filePath = ensureWorldFilePathForRuntime(runtime, app);
	if (filePath.trimmed().isEmpty())
		return;

	m_autosaveInFlight = true;
	QPointer guard(this);
	runtime->saveWorldFileAsync(filePath,
	                            [guard](const bool ok, const QString &saveError)
	                            {
		                            if (!guard)
			                            return;
		                            guard->m_autosaveInFlight = false;
		                            guard->refreshAutosaveTimer();
		                            if (!ok && guard->m_commandProcessor && !saveError.isEmpty())
			                            guard->m_commandProcessor->note(
			                                QStringLiteral("Unable to autosave world: %1").arg(saveError),
			                                true);
	                            });
}

void WorldChildWindow::closeEvent(QCloseEvent *event)
{
	AppController *app     = AppController::instance();
	WorldRuntime  *runtime = m_runtime;
	if (app && runtime && runtime->isConnected() &&
	    app->getGlobalOption(QStringLiteral("ConfirmBeforeClosingWorld")).toInt() != 0)
	{
		const QString worldName =
		    runtime->worldAttributes().value(QStringLiteral("name"), QStringLiteral("QMud"));
		const QString message = QStringLiteral("This will end your %1 session.").arg(worldName);
		const QMessageBox::StandardButton result =
		    QMessageBox::information(this, QStringLiteral("QMud"), message,
		                             QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);
		if (result != QMessageBox::Ok)
		{
			event->ignore();
			return;
		}
	}

	if (runtime &&
	    isEnabledFlagValue(runtime->worldAttributes().value(QStringLiteral("save_world_automatically"))) &&
	    (runtime->worldFileModified() || runtime->variablesChanged()))
	{
		const QString filePath = ensureWorldFilePathForRuntime(runtime, app);
		QString       error;
		if (!runtime->saveWorldFile(filePath, &error))
		{
			QMessageBox::warning(this, QStringLiteral("Save world"),
			                     error.isEmpty() ? QStringLiteral("Unable to save the world file.") : error);
			event->ignore();
			return;
		}
		runtime->setVariablesChanged(false);
		runtime->setWorldFileModified(false);
	}

	if (app && runtime && runtime->variablesChanged() && !runtime->worldFileModified() &&
	    app->getGlobalOption(QStringLiteral("ConfirmBeforeSavingVariables")).toInt() != 0)
	{
		QString name = runtime->worldFilePath();
		if (name.isEmpty())
			name = runtime->worldAttributes().value(QStringLiteral("name"), QStringLiteral("Untitled"));
		const QString prompt = QStringLiteral("World internal variables (only) have changed.\n\n"
		                                      "Save changes to %1?")
		                           .arg(name);
		const QMessageBox::StandardButton response = QMessageBox::question(
		    this, QStringLiteral("QMud"), prompt, QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
		    QMessageBox::Cancel);
		if (response == QMessageBox::Cancel)
		{
			event->ignore();
			return;
		}
		if (response == QMessageBox::Yes)
		{
			const QString filePath = ensureWorldFilePathForRuntime(runtime, app);
			QString       error;
			if (!runtime->saveWorldFile(filePath, &error))
			{
				QMessageBox::warning(this, QStringLiteral("Save world"),
				                     error.isEmpty() ? QStringLiteral("Unable to save the world file.")
				                                     : error);
				event->ignore();
				return;
			}
			runtime->setVariablesChanged(false);
			runtime->setWorldFileModified(false);
		}
	}

	if (runtime)
		runtime->fireWorldCloseHandlers();

	if (AppController *controller = AppController::instance())
	{
		QString placementName;
		if (runtime)
			placementName = runtime->worldAttributes().value(QStringLiteral("name")).trimmed();
		if (placementName.isEmpty())
			placementName = windowTitle();
		controller->saveWorldWindowPlacement(placementName, this);
	}

	QMdiSubWindow::closeEvent(event);
}

bool WorldChildWindow::event(QEvent *event)
{
	if (WorldRuntime *runtime = m_runtime; runtime && event)
	{
		if (event->type() == QEvent::WindowActivate)
			runtime->fireWorldGetFocusHandlers();
		else if (event->type() == QEvent::WindowDeactivate)
			runtime->fireWorldLoseFocusHandlers();
		else if (event->type() == QEvent::WindowStateChange)
		{
			if (m_view)
				m_view->refreshMiniWindows();
			runtime->notifyWorldOutputResized();
		}
	}
	return QMdiSubWindow::event(event);
}

void WorldChildWindow::showEvent(QShowEvent *event)
{
	QMdiSubWindow::showEvent(event);
	if (m_view)
		m_view->refreshMiniWindows();
	if (WorldRuntime *runtime = m_runtime)
		runtime->notifyWorldOutputResized();
	tryInstallPendingPlugins();
}

void WorldChildWindow::resizeEvent(QResizeEvent *event)
{
	QMdiSubWindow::resizeEvent(event);
	if (m_view)
		m_view->refreshMiniWindows();
	if (WorldRuntime *runtime = m_runtime)
		runtime->notifyWorldOutputResized();
	tryInstallPendingPlugins();
}

void WorldChildWindow::tryInstallPendingPlugins() const
{
	WorldRuntime *worldRuntime = m_runtime;
	if (!worldRuntime || !m_view)
		return;
	if (!isVisible() || !m_view->isVisible())
		return;
	if (m_view->outputClientWidth() <= 0 || m_view->outputClientHeight() <= 0)
		return;
	worldRuntime->installPendingPlugins();
}

ActivityChildWindow::ActivityChildWindow(QWidget *parent) : QMdiSubWindow(parent)
{
	setWindowTitle(QStringLiteral("Activity"));
	setWidget(new ActivityWindow(this));
}

ActivityChildWindow::ActivityChildWindow(const QString &title, QWidget *parent) : QMdiSubWindow(parent)
{
	setWindowTitle(title);
	setWidget(new ActivityWindow(this));
}

ActivityWindow *ActivityChildWindow::activityWindow() const
{
	return qobject_cast<ActivityWindow *>(widget());
}

TextChildWindow::TextChildWindow(QWidget *parent) : QMdiSubWindow(parent)
{
	setAttribute(Qt::WA_DeleteOnClose, true);
	setWindowTitle(QStringLiteral("Text"));
	m_editor = new QPlainTextEdit;
	setWidget(m_editor);
	if (m_editor)
	{
		if (AppController *app = AppController::instance(); app)
		{
			const int wrap         = app->getGlobalOption(QStringLiteral("NotepadWordWrap")).toInt();
			const int textRef      = app->getGlobalOption(QStringLiteral("NotepadTextColour")).toInt();
			const int backRef      = app->getGlobalOption(QStringLiteral("NotepadBackColour")).toInt();
			auto      colorFromRef = [](const int colorRef)
			{
				const int r = colorRef & 0xFF;
				const int g = colorRef >> 8 & 0xFF;
				const int b = colorRef >> 16 & 0xFF;
				return QColor(r, g, b);
			};
			QPalette pal = m_editor->palette();
			pal.setColor(QPalette::Text, colorFromRef(textRef));
			pal.setColor(QPalette::Base, colorFromRef(backRef));
			m_editor->setPalette(pal);
			m_editor->setAutoFillBackground(true);
			m_editor->setLineWrapMode(wrap != 0 ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
			m_editor->setWordWrapMode(wrap != 0 ? QTextOption::WrapAtWordBoundaryOrAnywhere
			                                    : QTextOption::NoWrap);
		}
	}
	if (m_editor && m_editor->document())
	{
		connect(m_editor->document(), &QTextDocument::modificationChanged, this,
		        [this](bool)
		        {
			        if (MainWindowHost *main = resolveMainWindowHost(window()))
				        main->updateMdiTabs();
		        });
	}
	if (m_editor)
	{
		connect(m_editor, &QPlainTextEdit::cursorPositionChanged, this,
		        [this]
		        {
			        if (MainWindowHost *main = resolveMainWindowHost(window()))
				        main->updateStatusBar();
		        });
	}
}

TextChildWindow::TextChildWindow(const QString &title, const QString &text, QWidget *parent)
    : QMdiSubWindow(parent)
{
	setAttribute(Qt::WA_DeleteOnClose, true);
	setWindowTitle(title);
	m_editor = new QPlainTextEdit;
	m_editor->setPlainText(text);
	setWidget(m_editor);
	if (m_editor)
	{
		if (AppController *app = AppController::instance(); app)
		{
			const int wrap         = app->getGlobalOption(QStringLiteral("NotepadWordWrap")).toInt();
			const int textRef      = app->getGlobalOption(QStringLiteral("NotepadTextColour")).toInt();
			const int backRef      = app->getGlobalOption(QStringLiteral("NotepadBackColour")).toInt();
			auto      colorFromRef = [](const int colorRef)
			{
				const int r = colorRef & 0xFF;
				const int g = colorRef >> 8 & 0xFF;
				const int b = colorRef >> 16 & 0xFF;
				return QColor(r, g, b);
			};
			QPalette pal = m_editor->palette();
			pal.setColor(QPalette::Text, colorFromRef(textRef));
			pal.setColor(QPalette::Base, colorFromRef(backRef));
			m_editor->setPalette(pal);
			m_editor->setAutoFillBackground(true);
			m_editor->setLineWrapMode(wrap != 0 ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
			m_editor->setWordWrapMode(wrap != 0 ? QTextOption::WrapAtWordBoundaryOrAnywhere
			                                    : QTextOption::NoWrap);
		}
	}
	if (m_editor && m_editor->document())
	{
		connect(m_editor->document(), &QTextDocument::modificationChanged, this,
		        [this](bool)
		        {
			        if (MainWindowHost *main = resolveMainWindowHost(window()))
				        main->updateMdiTabs();
		        });
	}
	if (m_editor)
	{
		connect(m_editor, &QPlainTextEdit::cursorPositionChanged, this,
		        [this]
		        {
			        if (MainWindowHost *main = resolveMainWindowHost(window()))
				        main->updateStatusBar();
		        });
	}
}

QPlainTextEdit *TextChildWindow::editor() const
{
	return m_editor;
}

void TextChildWindow::setText(const QString &text) const
{
	if (!m_editor)
		return;
	m_editor->setPlainText(text);
}

void TextChildWindow::appendText(const QString &text) const
{
	if (!m_editor)
		return;
	QTextCursor cursor = m_editor->textCursor();
	cursor.movePosition(QTextCursor::End);
	cursor.insertText(text);
	m_editor->setTextCursor(cursor);
}

QString TextChildWindow::filePath() const
{
	return m_filePath;
}

void TextChildWindow::setFilePath(const QString &path)
{
	m_filePath = path;
	if (!path.isEmpty())
		setWindowTitle(QFileInfo(path).fileName());
}

bool TextChildWindow::saveToFile(const QString &path, QString *error)
{
	if (!m_editor)
	{
		if (error)
			*error = QStringLiteral("No editor available.");
		return false;
	}
	if (path.trimmed().isEmpty())
	{
		if (error)
			*error = QStringLiteral("No filename specified.");
		return false;
	}
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		if (error)
			*error = QStringLiteral("Unable to create the requested file.");
		return false;
	}
	if (const QByteArray payload = m_editor->toPlainText().toLocal8Bit();
	    file.write(payload) != payload.size())
	{
		if (error)
			*error = QStringLiteral("Unable to write the requested file.");
		return false;
	}
	if (!file.commit())
	{
		if (error)
			*error = QStringLiteral("Unable to save the requested file.");
		return false;
	}
	setFilePath(path);
	if (m_editor->document())
		m_editor->document()->setModified(false);
	return true;
}

bool TextChildWindow::saveToCurrentFile(QString *error)
{
	if (m_filePath.isEmpty())
	{
		if (error)
			*error = QStringLiteral("No filename specified.");
		return false;
	}
	return saveToFile(m_filePath, error);
}

void TextChildWindow::setQuerySaveOnClose(const bool querySave)
{
	m_querySaveOnClose     = querySave;
	m_querySaveOverrideSet = true;
}

bool TextChildWindow::maybeSaveBeforeClose(const bool querySave)
{
	if (QTextDocument *document = m_editor ? m_editor->document() : nullptr;
	    !document || !document->isModified())
		return true;
	if (m_editor->toPlainText().isEmpty())
		return true;

	if (!querySave)
		return true;

	bool ok         = false;
	int  saveMethod = property("save_method").toInt(&ok);
	if (!ok || saveMethod < 0 || saveMethod > 2)
		saveMethod = 0;

	if (saveMethod == 2)
		return true;

	auto saveNow = [&]() -> bool
	{
		QString error;
		if (!m_filePath.trimmed().isEmpty())
		{
			if (!saveToCurrentFile(&error))
			{
				QMessageBox::warning(this, QStringLiteral("Save notepad"),
				                     error.isEmpty() ? QStringLiteral("Unable to save the requested file.")
				                                     : error);
				return false;
			}
			return true;
		}

		const QString chosenPath =
		    QFileDialog::getSaveFileName(this, QStringLiteral("Save notepad as"), windowTitle(),
		                                 QStringLiteral("Text files (*.txt);;All files (*.*)"));
		if (chosenPath.isEmpty())
			return false;
		if (!saveToFile(chosenPath, &error))
		{
			QMessageBox::warning(this, QStringLiteral("Save notepad"),
			                     error.isEmpty() ? QStringLiteral("Unable to save the requested file.")
			                                     : error);
			return false;
		}
		return true;
	};

	if (saveMethod == 1)
		return saveNow();

	const QMessageBox::StandardButton answer = QMessageBox::question(
	    this, QStringLiteral("Save notepad"), QStringLiteral("Save changes to %1?").arg(windowTitle()),
	    QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Yes);

	if (answer == QMessageBox::Cancel)
		return false;
	if (answer == QMessageBox::No)
		return true;
	return saveNow();
}

void TextChildWindow::closeEvent(QCloseEvent *event)
{
	const bool querySave   = m_querySaveOverrideSet ? m_querySaveOnClose : true;
	m_querySaveOnClose     = true;
	m_querySaveOverrideSet = false;

	if (!maybeSaveBeforeClose(querySave))
	{
		event->ignore();
		return;
	}
	QMdiSubWindow::closeEvent(event);
}
