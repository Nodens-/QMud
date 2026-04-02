/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MainFrame.cpp
 * Role: Top-level frame implementation that owns the primary workspace layout and orchestrates high-level UI behavior.
 */

#include "MainFrame.h"
#include "ActivityWindow.h"
#include "AppController.h"
#include "MainFrameActionUtils.h"
#include "MainFrameMdiUtils.h"
#include "WorldChildWindow.h"
#include "WorldRuntime.h"
#include "WorldView.h"

#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QCloseEvent>
#include <QColor>
#include <QDateTime>
#include <QDebug>
#include <QFontMetrics>
#include <QIcon>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTextCursor>
#include <QTextOption>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

// We need two timers, because processing a tick shouldn't reset timer processing nor vice versa

StatusPaneLabel::StatusPaneLabel(const QString &text, QWidget *parent) : QLabel(text, parent)
{
	setMargin(2);
}

void StatusPaneLabel::mousePressEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton)
		emit clicked();
	QLabel::mousePressEvent(event);
}

void StatusPaneLabel::mouseDoubleClickEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton)
		emit doubleClicked();
	QLabel::mouseDoubleClickEvent(event);
}

static QIcon toolbarIconByName(const QString &iconName)
{
	return QIcon(QStringLiteral(":/qmud/res/toolbar/%1.png").arg(iconName));
}

static int sizeToInt(const qsizetype value)
{
	return static_cast<int>(
	    qBound(static_cast<qsizetype>(0), value, static_cast<qsizetype>(std::numeric_limits<int>::max())));
}

static bool isTrayOnlyIconPlacement()
{
	const AppController *app = AppController::instance();
	if (!app)
		return false;
	return app->getGlobalOption(QStringLiteral("Icon Placement")).toInt() == 1;
}

void MainWindow::addToolbarSeparator(QToolBar *toolbar)
{
	if (!toolbar)
		return;
	toolbar->addSeparator();
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
	qApp->installEventFilter(this);
	setWindowTitle(QStringLiteral("QMud"));
	setWindowIcon(QIcon(QStringLiteral(":/qmud/res/QMud.png")));
	resize(1024, 768);

	buildMenus();
	buildMdiArea();
	buildToolbars();
	buildStatusBar();
	buildInfoBar();

	if (m_statusMessageTimer)
		m_statusMessageTimer->stop();
	if (m_statusMessage)
		m_statusMessage->setText(QStringLiteral("Ready"));
	setTimerInterval(0);
}

QDockWidget *MainWindow::infoDock() const
{
	return m_infoDock;
}

MainWindow::~MainWindow()
{
	qApp->removeEventFilter(this);
	if (m_mdiArea)
	{
		for (const QMdiSubWindow *sub : m_mdiArea->subWindowList())
		{
			if (sub)
				disconnect(sub, nullptr, this, nullptr);
		}
		m_mdiArea->closeAllSubWindows();
		delete m_mdiArea;
		m_mdiArea = nullptr;
	}
}

QRect MainWindow::lastNormalGeometry() const
{
	return m_lastNormalGeometry;
}

void MainWindow::setLastNormalGeometry(const QRect &rect)
{
	if (rect.isValid() && !rect.isNull())
		m_lastNormalGeometry = rect;
}

void MainWindow::showEvent(QShowEvent *event)
{
	QMainWindow::showEvent(event);

	if ((windowState() & (Qt::WindowMaximized | Qt::WindowMinimized | Qt::WindowFullScreen)) == 0)
		m_lastNormalGeometry = geometry();

	refreshWorldMiniWindows();

	if (m_initialShowHandled)
		return;
	m_initialShowHandled = true;

	if (const QAction *toolbarAction = actionForCommand(QStringLiteral("ViewToolbar")))
		setToolbarVisible(toolbarAction->isChecked());
	if (const QAction *worldToolbarAction = actionForCommand(QStringLiteral("ViewWorldToolbar")))
		setWorldToolbarVisible(worldToolbarAction->isChecked());
	if (const QAction *activityToolbarAction = actionForCommand(QStringLiteral("ActivityToolbar")))
		setActivityToolbarVisible(activityToolbarAction->isChecked());
	if (const QAction *statusAction = actionForCommand(QStringLiteral("ViewStatusbar")))
		setStatusbarVisible(statusAction->isChecked());
	if (const QAction *infoAction = actionForCommand(QStringLiteral("ViewInfoBar")))
		setInfoBarVisible(infoAction->isChecked());

	const bool anyToolbarVisible = (m_mainToolbarWidget && !m_mainToolbarWidget->isHidden()) ||
	                               (m_worldToolbarWidget && !m_worldToolbarWidget->isHidden()) ||
	                               (m_activityToolbarWidget && !m_activityToolbarWidget->isHidden());
	if (!anyToolbarVisible)
		resetToolbarsToDefaults();
}

void MainWindow::moveEvent(QMoveEvent *event)
{
	QMainWindow::moveEvent(event);
	if ((windowState() & (Qt::WindowMaximized | Qt::WindowMinimized | Qt::WindowFullScreen)) == 0)
		m_lastNormalGeometry = geometry();
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
	QMainWindow::resizeEvent(event);
	if ((windowState() & (Qt::WindowMaximized | Qt::WindowMinimized | Qt::WindowFullScreen)) == 0)
		m_lastNormalGeometry = geometry();
	refreshWorldMiniWindows();
}

QAction *MainWindow::actionForCommand(const QString &cmdName) const
{
	return m_actions.value(cmdName, nullptr);
}

QAction *MainWindow::addActionToMenu(QMenu *menu, const QString &cmdName, const QString &text,
                                     const QKeySequence &shortcut)
{
	auto *a = new QAction(text, this);
	if (!shortcut.isEmpty())
		a->setShortcut(shortcut);
	a->setShortcutContext(Qt::ApplicationShortcut);
	a->setObjectName(cmdName); // preserve original command-name for lookup
	a->setIconVisibleInMenu(false);
	QString tipText = text;
	tipText.remove(QLatin1Char('&'));
	if (tipText.endsWith(QStringLiteral("...")))
		tipText.chop(3);
	tipText = tipText.trimmed();
	a->setToolTip(tipText);
	a->setStatusTip(tipText);
	connect(a, &QAction::triggered, this, &MainWindow::onActionTriggered);
	menu->addAction(a);
	m_actions.insert(cmdName, a);
	return a;
}

void MainWindow::buildMenus()
{
	// File menu
	m_fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
	addActionToMenu(m_fileMenu, QStringLiteral("New"), QStringLiteral("&New World...\tCtrl+N"),
	                QKeySequence::New);
	addActionToMenu(m_fileMenu, QStringLiteral("Open"), QStringLiteral("&Open World...\tCtrl+O"),
	                QKeySequence::Open);
	addActionToMenu(m_fileMenu, QStringLiteral("OpenWorldsInStartupList"),
	                QStringLiteral("Open Worlds In Startup List\tCtrl+Alt+O"),
	                QKeySequence(QStringLiteral("Ctrl+Alt+O")));
	m_fileMenu->addSeparator();
	addActionToMenu(m_fileMenu, QStringLiteral("CloseWorld"), QStringLiteral("&Close World"));
	addActionToMenu(m_fileMenu, QStringLiteral("Import"), QStringLiteral("&Import...\tCtrl+Alt+I"),
	                QKeySequence(QStringLiteral("Ctrl+Alt+I")));
	addActionToMenu(m_fileMenu, QStringLiteral("Plugins"), QStringLiteral("Pl&ugins...\tShift+Ctrl+P"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+P")));
	addActionToMenu(m_fileMenu, QStringLiteral("PluginWizard"),
	                QStringLiteral("Plugin Wizard...\tShift+Ctrl+Alt+P"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+Alt+P")));
	addActionToMenu(m_fileMenu, QStringLiteral("Save"), QStringLiteral("&Save World Details\tCtrl+S"),
	                QKeySequence::Save);
	addActionToMenu(m_fileMenu, QStringLiteral("SaveAs"), QStringLiteral("Save World Details &As..."));
	addActionToMenu(m_fileMenu, QStringLiteral("SaveSelection"), QStringLiteral("Save Selection..."));
	m_fileMenu->addSeparator();
	addActionToMenu(m_fileMenu, QStringLiteral("Print"), QStringLiteral("&Print...\tCtrl+P"),
	                QKeySequence::Print);
	addActionToMenu(m_fileMenu, QStringLiteral("PrintSetup"), QStringLiteral("Print Se&tup..."));
	m_fileMenu->addSeparator();
	addActionToMenu(m_fileMenu, QStringLiteral("GlobalPreferences"),
	                QStringLiteral("&Global Preferences...\tCtrl+Alt+G"),
	                QKeySequence(QStringLiteral("Ctrl+Alt+G")));
	addActionToMenu(m_fileMenu, QStringLiteral("LogSession"), QStringLiteral("&Log Session...\tShift+Ctrl+J"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+J")));
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
	addActionToMenu(m_fileMenu, QStringLiteral("ReloadQMud"), QStringLiteral("Reload &QMud"));
#endif
	addActionToMenu(m_fileMenu, QStringLiteral("ReloadDefaults"),
	                QStringLiteral("&Reload Defaults\tCtrl+Alt+R"),
	                QKeySequence(QStringLiteral("Ctrl+Alt+R")));
	auto *worldPropertiesAction = addActionToMenu(m_fileMenu, QStringLiteral("Preferences"),
	                                              QStringLiteral("&World Properties...\tAlt+Enter"),
	                                              QKeySequence(QStringLiteral("Alt+Return")));
	worldPropertiesAction->setShortcuts({QKeySequence(QStringLiteral("Alt+Return")),
	                                     QKeySequence(QStringLiteral("Alt+Enter")),
	                                     QKeySequence(QStringLiteral("Ctrl+G"))});
	addActionToMenu(m_fileMenu, QStringLiteral("WindowsSocketInfo"),
	                QStringLiteral("Windows Socket Info..."));
	m_fileMenu->addSeparator();
	auto *recentLabel = m_fileMenu->addAction(QStringLiteral("Recent File"));
	recentLabel->setEnabled(false);
	m_fileMenu->addSeparator();
	for (int i = 0; i < m_recentMax; ++i)
	{
		auto *act = new QAction(this);
		act->setVisible(false);
		act->setIconVisibleInMenu(false);
		connect(act, &QAction::triggered, this, &MainWindow::onRecentFileAction);
		m_fileMenu->addAction(act);
		m_recentActions.push_back(act);
	}
	m_fileMenu->addSeparator();
	addActionToMenu(m_fileMenu, QStringLiteral("ExitClient"), QStringLiteral("E&xit"));

	// Edit menu
	QMenu *editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
	addActionToMenu(editMenu, QStringLiteral("Undo"), QStringLiteral("&Undo\tCtrl+Z"), QKeySequence::Undo);
	editMenu->addSeparator();
	addActionToMenu(editMenu, QStringLiteral("Cut"), QStringLiteral("Cu&t\tCtrl+X"), QKeySequence::Cut);
	addActionToMenu(editMenu, QStringLiteral("Copy"), QStringLiteral("&Copy\tCtrl+C"), QKeySequence::Copy);
	addActionToMenu(editMenu, QStringLiteral("CopyAsHTML"), QStringLiteral("Copy as &HTML"),
	                QKeySequence(QStringLiteral("Ctrl+Alt+C")));
	addActionToMenu(editMenu, QStringLiteral("Paste"), QStringLiteral("&Paste\tCtrl+V"), QKeySequence::Paste);
	editMenu->addSeparator();
	addActionToMenu(editMenu, QStringLiteral("PasteToWorld"),
	                QStringLiteral("Paste To &World...\tShift+Ctrl+V"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+V")));
	addActionToMenu(editMenu, QStringLiteral("RecallLastWord"),
	                QStringLiteral("Recall Last Word\tCtrl+Backspace"),
	                QKeySequence(QStringLiteral("Ctrl+Backspace")));
	editMenu->addSeparator();
	addActionToMenu(editMenu, QStringLiteral("SelectAll"), QStringLiteral("Select &All\tCtrl+A"),
	                QKeySequence::SelectAll);
	addActionToMenu(editMenu, QStringLiteral("SpellCheck"), QStringLiteral("Sp&ell Check\tCtrl+J"),
	                QKeySequence(QStringLiteral("Ctrl+J")));
	editMenu->addSeparator();
	addActionToMenu(editMenu, QStringLiteral("GenerateCharacterName"),
	                QStringLiteral("&Generate Character Name...\tCtrl+Alt+N"),
	                QKeySequence(QStringLiteral("Ctrl+Alt+N")));
	addActionToMenu(editMenu, QStringLiteral("ReloadNamesFile"), QStringLiteral("&Reload Names File..."));
	addActionToMenu(editMenu, QStringLiteral("GenerateUniqueId"), QStringLiteral("Generate Unique &ID..."));
	editMenu->addSeparator();
	addActionToMenu(editMenu, QStringLiteral("Notepad"), QStringLiteral("&Notepad\tCtrl+Alt+W"),
	                QKeySequence(QStringLiteral("Ctrl+Alt+W")));
	addActionToMenu(editMenu, QStringLiteral("FlipToNotepad"),
	                QStringLiteral("&Flip To Notepad\tCtrl+Alt+Space"),
	                QKeySequence(QStringLiteral("Ctrl+Alt+Space")));
	editMenu->addSeparator();
	addActionToMenu(editMenu, QStringLiteral("EditColourPicker"), QStringLiteral("Colour Picker\tCtrl+Alt+P"),
	                QKeySequence(QStringLiteral("Ctrl+Alt+P")));
	addActionToMenu(editMenu, QStringLiteral("DebugPackets"), QStringLiteral("Debug Packets\tCtrl+Alt+F11"),
	                QKeySequence(QStringLiteral("Ctrl+Alt+F11")));
	editMenu->addSeparator();
	addActionToMenu(editMenu, QStringLiteral("GoToMatchingBrace"),
	                QStringLiteral("Go To &Matching Brace\tCtrl+E"), QKeySequence(QStringLiteral("Ctrl+E")));
	addActionToMenu(editMenu, QStringLiteral("SelectToMatchingBrace"),
	                QStringLiteral("Select To Matching &Brace\tShift+Ctrl+E"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+E")));
	editMenu->addSeparator();
	addActionToMenu(editMenu, QStringLiteral("ConvertClipboardForumCodes"),
	                QStringLiteral("Convert Clipboard Forum Codes\tShift+Ctrl+Alt+Q"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+Alt+Q")));
	addActionToMenu(editMenu, QStringLiteral("AsciiArt"), QStringLiteral("ASCII Art...\tShift+Ctrl+A"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+A")));
	addActionToMenu(editMenu, QStringLiteral("TextGoTo"), QStringLiteral("&Go To..."));
	addActionToMenu(editMenu, QStringLiteral("InsertDateTime"), QStringLiteral("Insert &Date/Time"));
	addActionToMenu(editMenu, QStringLiteral("WordCount"), QStringLiteral("&Word Count..."));
	editMenu->addSeparator();
	addActionToMenu(editMenu, QStringLiteral("SendToCommandWindow"),
	                QStringLiteral("Send To Command Wind&ow"));
	addActionToMenu(editMenu, QStringLiteral("SendToScript"), QStringLiteral("Send To Script"));
	addActionToMenu(editMenu, QStringLiteral("SendToWorld"), QStringLiteral("&Send To World"));
	addActionToMenu(editMenu, QStringLiteral("RefreshRecalledData"),
	                QStringLiteral("&Refresh Recalled Data"));

	// View menu
	QMenu *viewMenu    = menuBar()->addMenu(QStringLiteral("&View"));
	auto  *viewToolbar = addActionToMenu(viewMenu, QStringLiteral("ViewToolbar"), QStringLiteral("&Toolbar"));
	viewToolbar->setCheckable(true);
	viewToolbar->setChecked(true);
	connect(viewToolbar, &QAction::toggled, this, &MainWindow::onToggleToolbar);

	auto *viewWorldToolbar =
	    addActionToMenu(viewMenu, QStringLiteral("ViewWorldToolbar"), QStringLiteral("&World Toolbar"));
	viewWorldToolbar->setCheckable(true);
	viewWorldToolbar->setChecked(true);
	connect(viewWorldToolbar, &QAction::toggled, this,
	        [this](const bool checked)
	        {
		        if (m_worldToolbarWidget)
			        m_worldToolbarWidget->setVisible(checked);
		        emit viewPreferenceChanged(QStringLiteral("ViewWorldToolbar"), checked ? 1 : 0);
	        });

	auto *viewActivityToolbar =
	    addActionToMenu(viewMenu, QStringLiteral("ActivityToolbar"), QStringLiteral("&Activity Toolbar"));
	viewActivityToolbar->setCheckable(true);
	viewActivityToolbar->setChecked(true);
	connect(viewActivityToolbar, &QAction::toggled, this,
	        [this](const bool checked)
	        {
		        if (m_activityToolbarWidget)
			        m_activityToolbarWidget->setVisible(checked);
		        emit viewPreferenceChanged(QStringLiteral("ActivityToolbar"), checked ? 1 : 0);
	        });

	auto *viewStatusbar =
	    addActionToMenu(viewMenu, QStringLiteral("ViewStatusbar"), QStringLiteral("&Status Bar"));
	viewStatusbar->setCheckable(true);
	viewStatusbar->setChecked(true);
	connect(viewStatusbar, &QAction::toggled, this, &MainWindow::onToggleStatusbar);
	auto *viewInfoBar = addActionToMenu(viewMenu, QStringLiteral("ViewInfoBar"), QStringLiteral("&Info Bar"));
	viewInfoBar->setCheckable(true);
	viewInfoBar->setChecked(false);
	connect(viewInfoBar, &QAction::toggled, this, &MainWindow::onToggleInfoBar);
	viewMenu->addSeparator();
	addActionToMenu(viewMenu, QStringLiteral("ResetToolbars"), QStringLiteral("&Reset Toolbar Locations"));
	auto *alwaysOnTop =
	    addActionToMenu(viewMenu, QStringLiteral("AlwaysOnTop"), QStringLiteral("Always &On Top"));
	alwaysOnTop->setCheckable(true);
	if (const AppController *app = AppController::instance())
		alwaysOnTop->setChecked(app->getGlobalOption(QStringLiteral("AlwaysOnTop")).toInt() != 0);
	auto *fullScreen = addActionToMenu(viewMenu, QStringLiteral("FullScreenMode"),
	                                   QStringLiteral("&Full Screen Mode\tCtrl+Alt+F"),
	                                   QKeySequence(QStringLiteral("Ctrl+Alt+F")));
	fullScreen->setCheckable(true);

	// Connection menu
	QMenu *connectionMenu = menuBar()->addMenu(QStringLiteral("&Connection"));
	addActionToMenu(connectionMenu, QStringLiteral("QuickConnect"),
	                QStringLiteral("&Quick Connect...\tCtrl+Alt+Shift+K"),
	                QKeySequence(QStringLiteral("Ctrl+Alt+Shift+K")));
	connectionMenu->addSeparator();
	addActionToMenu(connectionMenu, QStringLiteral("Connect"), QStringLiteral("&Connect\tCtrl+K"),
	                QKeySequence(QStringLiteral("Ctrl+K")));
	addActionToMenu(connectionMenu, QStringLiteral("Disconnect"), QStringLiteral("&Disconnect\tShift+Ctrl+K"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+K")));
	connectionMenu->addSeparator();
	QAction *autoConnect =
	    addActionToMenu(connectionMenu, QStringLiteral("AutoConnect"), QStringLiteral("&Auto Connect"));
	autoConnect->setCheckable(true);
	QAction *reconnect = addActionToMenu(connectionMenu, QStringLiteral("ReconnectOnDisconnect"),
	                                     QStringLiteral("&Reconnect On Disconnect"));
	reconnect->setCheckable(true);
	connectionMenu->addSeparator();
	addActionToMenu(connectionMenu, QStringLiteral("ConnectToAllOpenWorlds"),
	                QStringLiteral("Connect To All Open &Worlds\tCtrl+Alt+K"),
	                QKeySequence(QStringLiteral("Ctrl+Alt+K")));
	addActionToMenu(connectionMenu, QStringLiteral("ConnectToWorldsInStartupList"),
	                QStringLiteral("Connect To Worlds &In Startup List"));

	// Input menu
	QMenu *inputMenu = menuBar()->addMenu(QStringLiteral("&Input"));
	addActionToMenu(inputMenu, QStringLiteral("ActivateInputArea"),
	                QStringLiteral("Activate &Input Area\tTab"), QKeySequence(QStringLiteral("Tab")));
	inputMenu->addSeparator();
	addActionToMenu(inputMenu, QStringLiteral("NextCommand"), QStringLiteral("&Next Command\tAlt+Down"),
	                QKeySequence(QStringLiteral("Alt+Down")));
	addActionToMenu(inputMenu, QStringLiteral("PreviousCommand"), QStringLiteral("&Previous Command\tAlt+Up"),
	                QKeySequence(QStringLiteral("Alt+Up")));
	addActionToMenu(inputMenu, QStringLiteral("RepeatLastCommand"),
	                QStringLiteral("&Repeat Last Command\tCtrl+R"), QKeySequence(QStringLiteral("Ctrl+R")));
	addActionToMenu(inputMenu, QStringLiteral("QuitFromWorld"),
	                QStringLiteral("&Quit From This World...\tShift+Ctrl+Q"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+Q")));
	inputMenu->addSeparator();
	addActionToMenu(inputMenu, QStringLiteral("CommandHistory"),
	                QStringLiteral("Command &History...\tCtrl+H"), QKeySequence(QStringLiteral("Ctrl+H")));
	addActionToMenu(inputMenu, QStringLiteral("ClearCommandHistory"),
	                QStringLiteral("&Clear Command History...\tShift+Ctrl+D"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+D")));
	addActionToMenu(inputMenu, QStringLiteral("DiscardQueuedCommands"),
	                QStringLiteral("&Discard Queued Commands\tCtrl+D"),
	                QKeySequence(QStringLiteral("Ctrl+D")));
	inputMenu->addSeparator();
	addActionToMenu(inputMenu, QStringLiteral("AutoSay"), QStringLiteral("&Auto Say\tShift+Ctrl+A"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+A")));
	addActionToMenu(inputMenu, QStringLiteral("SendFile"), QStringLiteral("&Send File...\tShift+Ctrl+O"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+O")));
	addActionToMenu(inputMenu, QStringLiteral("GlobalChange"),
	                QStringLiteral("Global Change...\tShift+Ctrl+G"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+G")));
	addActionToMenu(inputMenu, QStringLiteral("KeyName"), QStringLiteral("&Key Name..."));

	// Display menu
	QMenu *displayMenu = menuBar()->addMenu(QStringLiteral("&Display"));
	addActionToMenu(displayMenu, QStringLiteral("DisplayStart"), QStringLiteral("&Start\tCtrl+Home"),
	                QKeySequence(QStringLiteral("Ctrl+Home")));
	addActionToMenu(displayMenu, QStringLiteral("DisplayPageUp"), QStringLiteral("Page &Up\tPageUp"),
	                QKeySequence(QStringLiteral("PageUp")));
	addActionToMenu(displayMenu, QStringLiteral("DisplayPageDown"), QStringLiteral("Page &Down\tPageDown"),
	                QKeySequence(QStringLiteral("PageDown")));
	addActionToMenu(displayMenu, QStringLiteral("DisplayEnd"), QStringLiteral("&End\tCtrl+End"),
	                QKeySequence(QStringLiteral("Ctrl+End")));
	addActionToMenu(displayMenu, QStringLiteral("DisplayLineUp"), QStringLiteral("Line Up\tCtrl+Up"),
	                QKeySequence(QStringLiteral("Ctrl+Up")));
	addActionToMenu(displayMenu, QStringLiteral("DisplayLineDown"), QStringLiteral("Line Down\tCtrl+Down"),
	                QKeySequence(QStringLiteral("Ctrl+Down")));
	displayMenu->addSeparator();
	addActionToMenu(displayMenu, QStringLiteral("ActivityList"),
	                QStringLiteral("&Activity List\tShift+Ctrl+L"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+L")));
	addActionToMenu(displayMenu, QStringLiteral("FreezeOutput"), QStringLiteral("Pause &Output\tCtrl+Space"),
	                QKeySequence(QStringLiteral("Ctrl+Space")));
	displayMenu->addSeparator();
	addActionToMenu(displayMenu, QStringLiteral("Find"), QStringLiteral("&Find...\tCtrl+F"),
	                QKeySequence::Find);
	addActionToMenu(displayMenu, QStringLiteral("FindAgain"), QStringLiteral("Find Again\tShift+Ctrl+F"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+F")));
	addActionToMenu(displayMenu, QStringLiteral("RecallText"), QStringLiteral("&Recall Text...\tCtrl+U"),
	                QKeySequence(QStringLiteral("Ctrl+U")));
	addActionToMenu(displayMenu, QStringLiteral("GoToLine"), QStringLiteral("Go To Line...\tCtrl+Alt+L"),
	                QKeySequence(QStringLiteral("Ctrl+Alt+L")));
	displayMenu->addSeparator();
	addActionToMenu(displayMenu, QStringLiteral("GoToUrl"), QStringLiteral("Go To URL...\tCtrl+Alt+J"),
	                QKeySequence(QStringLiteral("Ctrl+Alt+J")));
	addActionToMenu(displayMenu, QStringLiteral("SendMailTo"), QStringLiteral("Send Mail To..."));
	displayMenu->addSeparator();
	addActionToMenu(displayMenu, QStringLiteral("ClearOutputBuffer"),
	                QStringLiteral("Clear Output Buffer...\tShift+Ctrl+C"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+C")));
	addActionToMenu(displayMenu, QStringLiteral("StopSoundPlaying"),
	                QStringLiteral("Stop Sound Playing\tCtrl+Alt+B"),
	                QKeySequence(QStringLiteral("Ctrl+Alt+B")));
	displayMenu->addSeparator();
	addActionToMenu(displayMenu, QStringLiteral("BookmarkSelection"),
	                QStringLiteral("&Bookmark Selection\tShift+Ctrl+B"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+B")));
	addActionToMenu(displayMenu, QStringLiteral("GoToBookmark"), QStringLiteral("&Go To Bookmark\tCtrl+B"),
	                QKeySequence(QStringLiteral("Ctrl+B")));
	addActionToMenu(displayMenu, QStringLiteral("HighlightWord"),
	                QStringLiteral("&Highlight Word...\tCtrl+Alt+H"),
	                QKeySequence(QStringLiteral("Ctrl+Alt+H")));
	addActionToMenu(displayMenu, QStringLiteral("MultiLineTrigger"),
	                QStringLiteral("&Multi-Line Trigger..."));
	addActionToMenu(displayMenu, QStringLiteral("TextAttributes"),
	                QStringLiteral("&Text Attributes...\tCtrl+Alt+A"),
	                QKeySequence(QStringLiteral("Ctrl+Alt+A")));
	displayMenu->addSeparator();
	addActionToMenu(displayMenu, QStringLiteral("NoCommandEcho"),
	                QStringLiteral("No Command Echo\tCtrl+Alt+E"),
	                QKeySequence(QStringLiteral("Ctrl+Alt+E")));

	// Game / World menu
	QMenu   *gameMenu               = menuBar()->addMenu(QStringLiteral("&Game"));
	QMenu   *configureMenu          = gameMenu->addMenu(QStringLiteral("&Configure"));
	QAction *allConfigurationAction = addActionToMenu(configureMenu, QStringLiteral("Preferences"),
	                                                  QStringLiteral("All &Configuration...\tAlt+Enter"),
	                                                  QKeySequence(QStringLiteral("Alt+Return")));
	allConfigurationAction->setShortcuts({QKeySequence(QStringLiteral("Alt+Return")),
	                                      QKeySequence(QStringLiteral("Alt+Enter")),
	                                      QKeySequence(QStringLiteral("Ctrl+G"))});
	configureMenu->addSeparator();
	addActionToMenu(configureMenu, QStringLiteral("ConfigureMudAddress"),
	                QStringLiteral("MUD Name/IP address...\tAlt+1"), QKeySequence(QStringLiteral("Alt+1")));
	addActionToMenu(configureMenu, QStringLiteral("ConfigureConnecting"),
	                QStringLiteral("Connecting...\tAlt+2"), QKeySequence(QStringLiteral("Alt+2")));
	addActionToMenu(configureMenu, QStringLiteral("ConfigureChat"), QStringLiteral("Chat...\tAlt+7"),
	                QKeySequence(QStringLiteral("Alt+7")));
	addActionToMenu(configureMenu, QStringLiteral("ConfigureLogging"), QStringLiteral("Logging...\tAlt+3"),
	                QKeySequence(QStringLiteral("Alt+3")));
	addActionToMenu(configureMenu, QStringLiteral("ConfigureNotes"), QStringLiteral("Notes...\tAlt+4"),
	                QKeySequence(QStringLiteral("Alt+4")));
	configureMenu->addSeparator();
	addActionToMenu(configureMenu, QStringLiteral("ConfigureOutput"), QStringLiteral("Output...\tAlt+5"),
	                QKeySequence(QStringLiteral("Alt+5")));
	addActionToMenu(configureMenu, QStringLiteral("ConfigureMxp"),
	                QStringLiteral("MXP / Pueblo...\tCtrl+Alt+U"),
	                QKeySequence(QStringLiteral("Ctrl+Alt+U")));
	addActionToMenu(configureMenu, QStringLiteral("ConfigureAnsiColours"),
	                QStringLiteral("ANSI Colours...\tAlt+6"), QKeySequence(QStringLiteral("Alt+6")));
	addActionToMenu(configureMenu, QStringLiteral("ConfigureCustomColours"),
	                QStringLiteral("Custom Colours...\tAlt+8"), QKeySequence(QStringLiteral("Alt+8")));
	addActionToMenu(configureMenu, QStringLiteral("ConfigurePrinting"), QStringLiteral("Printing...\tAlt+9"),
	                QKeySequence(QStringLiteral("Alt+9")));
	configureMenu->addSeparator();
	addActionToMenu(configureMenu, QStringLiteral("ConfigureCommands"), QStringLiteral("Commands...\tAlt+0"),
	                QKeySequence(QStringLiteral("Alt+0")));
	addActionToMenu(configureMenu, QStringLiteral("ConfigureKeypad"),
	                QStringLiteral("Keypad...\tShift+Ctrl+1"), QKeySequence(QStringLiteral("Shift+Ctrl+1")));
	addActionToMenu(configureMenu, QStringLiteral("ConfigureMacros"),
	                QStringLiteral("Macros...\tShift+Ctrl+2"), QKeySequence(QStringLiteral("Shift+Ctrl+2")));
	addActionToMenu(configureMenu, QStringLiteral("ConfigureAutoSay"),
	                QStringLiteral("Auto say...\tShift+Ctrl+3"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+3")));
	configureMenu->addSeparator();
	addActionToMenu(configureMenu, QStringLiteral("ConfigurePaste"),
	                QStringLiteral("Paste to world...\tShift+Ctrl+4"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+4")));
	addActionToMenu(configureMenu, QStringLiteral("ConfigureSendFile"),
	                QStringLiteral("Send file...\tShift+Ctrl+5"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+5")));
	configureMenu->addSeparator();
	addActionToMenu(configureMenu, QStringLiteral("ConfigureScripting"),
	                QStringLiteral("Scripting...\tShift+Ctrl+6"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+6")));
	addActionToMenu(configureMenu, QStringLiteral("ConfigureVariables"),
	                QStringLiteral("Variables...\tShift+Ctrl+7"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+7")));
	configureMenu->addSeparator();
	addActionToMenu(configureMenu, QStringLiteral("ConfigureTimers"),
	                QStringLiteral("Timers...\tShift+Ctrl+0"), QKeySequence(QStringLiteral("Shift+Ctrl+0")));
	addActionToMenu(configureMenu, QStringLiteral("ConfigureTriggers"),
	                QStringLiteral("Triggers...\tShift+Ctrl+8"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+8")));
	addActionToMenu(configureMenu, QStringLiteral("ConfigureAliases"),
	                QStringLiteral("Aliases...\tShift+Ctrl+9"), QKeySequence(QStringLiteral("Shift+Ctrl+9")));
	configureMenu->addSeparator();
	addActionToMenu(configureMenu, QStringLiteral("ConfigureInfo"), QStringLiteral("Info...\tShift+Ctrl+I"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+I")));
	addActionToMenu(gameMenu, QStringLiteral("ChatSessions"),
	                QStringLiteral("Chat Sessions\tCtrl+Alt+Shift+C"),
	                QKeySequence(QStringLiteral("Ctrl+Alt+Shift+C")));
	addActionToMenu(gameMenu, QStringLiteral("GameWrapLines"), QStringLiteral("Wrap &Output"));
	gameMenu->addSeparator();
	addActionToMenu(gameMenu, QStringLiteral("TestTrigger"),
	                QStringLiteral("Test &Trigger...\tShift+Ctrl+F12"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+F12")));
	gameMenu->addSeparator();
	addActionToMenu(gameMenu, QStringLiteral("Minimize"), QStringLiteral("Minimize &Program\tCtrl+M"),
	                QKeySequence(QStringLiteral("Ctrl+M")));
	gameMenu->addSeparator();
	addActionToMenu(gameMenu, QStringLiteral("Immediate"), QStringLiteral("&Immediate...\tCtrl+I"),
	                QKeySequence(QStringLiteral("Ctrl+I")));
	addActionToMenu(gameMenu, QStringLiteral("EditScriptFile"),
	                QStringLiteral("&Edit Script File...\tShift+Ctrl+H"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+H")));
	addActionToMenu(gameMenu, QStringLiteral("ReloadScriptFile"),
	                QStringLiteral("&Reload Script File\tShift+Ctrl+R"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+R")));
	addActionToMenu(gameMenu, QStringLiteral("Trace"), QStringLiteral("Tr&ace\tCtrl+Alt+T"),
	                QKeySequence(QStringLiteral("Ctrl+Alt+T")));
	gameMenu->addSeparator();
	addActionToMenu(gameMenu, QStringLiteral("ResetAllTimers"),
	                QStringLiteral("Reset All Timers\tShift+Ctrl+T"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+T")));
	gameMenu->addSeparator();
	addActionToMenu(gameMenu, QStringLiteral("SendToAllWorlds"),
	                QStringLiteral("&Send To All Worlds...\tCtrl+Alt+S"),
	                QKeySequence(QStringLiteral("Ctrl+Alt+S")));
	addActionToMenu(gameMenu, QStringLiteral("Mapper"), QStringLiteral("&Mapper\tCtrl+Alt+M"),
	                QKeySequence(QStringLiteral("Ctrl+Alt+M")));
	addActionToMenu(gameMenu, QStringLiteral("MapperSpecial"),
	                QStringLiteral("&Do Mapper Special...\tCtrl+Alt+D"),
	                QKeySequence(QStringLiteral("Ctrl+Alt+D")));
	addActionToMenu(gameMenu, QStringLiteral("MapperComment"),
	                QStringLiteral("Add Mapper Comment...\tCtrl+Alt+Shift+D"),
	                QKeySequence(QStringLiteral("Ctrl+Alt+Shift+D")));

	// Window menu
	m_windowMenu = menuBar()->addMenu(QStringLiteral("&Window"));
	addActionToMenu(m_windowMenu, QStringLiteral("NewWindow"), QStringLiteral("&New Window"));
	addActionToMenu(m_windowMenu, QStringLiteral("CascadeWindows"), QStringLiteral("&Cascade"));
	addActionToMenu(m_windowMenu, QStringLiteral("TileWindows"), QStringLiteral("&Tile Horizontally"));
	addActionToMenu(m_windowMenu, QStringLiteral("TileWindowsVert"), QStringLiteral("Tile Vertically"));
	addActionToMenu(m_windowMenu, QStringLiteral("ArrangeIcons"), QStringLiteral("&Arrange Icons"));
	addActionToMenu(m_windowMenu, QStringLiteral("WindowMinimize"), QStringLiteral("&Minimize\tShift+Ctrl+M"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+M")));
	addActionToMenu(m_windowMenu, QStringLiteral("CloseAllNotepads"),
	                QStringLiteral("Close A&ll Notepad Windows"));
	addActionToMenu(m_windowMenu, QStringLiteral("WindowMaximize"), QStringLiteral("Maximize"));
	addActionToMenu(m_windowMenu, QStringLiteral("WindowRestore"), QStringLiteral("Restore"));
	connect(m_windowMenu, &QMenu::aboutToShow, this, &MainWindow::updateWindowMenu);

	// Help menu
	QMenu *helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
	addActionToMenu(helpMenu, QStringLiteral("TipOfTheDay"), QStringLiteral("&Tip Of The Day..."));
	helpMenu->addSeparator();
	addActionToMenu(helpMenu, QStringLiteral("GettingStarted"), QStringLiteral("&Getting Started..."));
	helpMenu->addSeparator();
	addActionToMenu(helpMenu, QStringLiteral("HelpContents"), QStringLiteral("&Contents"));
	addActionToMenu(helpMenu, QStringLiteral("HelpIndex"), QStringLiteral("&Index"));
	addActionToMenu(helpMenu, QStringLiteral("HelpUsing"), QStringLiteral("&Using Help"));
	helpMenu->addSeparator();
	addActionToMenu(helpMenu, QStringLiteral("BugReports"), QStringLiteral("&Bug Reports..."));
	addActionToMenu(helpMenu, QStringLiteral("DocumentationWebPage"),
	                QStringLiteral("&Documentation Web Page..."));
	addActionToMenu(helpMenu, QStringLiteral("HelpForum"), QStringLiteral("&Discord...\tShift+Ctrl+Alt+F"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+Alt+F")));
	addActionToMenu(helpMenu, QStringLiteral("FunctionsList"),
	                QStringLiteral("Functions &List...\tShift+Ctrl+Alt+L"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+Alt+L")));
	addActionToMenu(helpMenu, QStringLiteral("FunctionsWebPage"),
	                QStringLiteral("Fu&nctions Web Page ...\tShift+Ctrl+Alt+U"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+Alt+U")));
	addActionToMenu(helpMenu, QStringLiteral("HelpMudLists"), QStringLiteral("&MUD Lists..."));
	addActionToMenu(helpMenu, QStringLiteral("WebPage"),
	                QStringLiteral("QMud &Web Page...\tShift+Ctrl+Alt+W"),
	                QKeySequence(QStringLiteral("Shift+Ctrl+Alt+W")));
	addActionToMenu(helpMenu, QStringLiteral("PluginsList"), QStringLiteral("&Plugins List..."));
	addActionToMenu(helpMenu, QStringLiteral("RegularExpressionsWebPage"),
	                QStringLiteral("Regular &Expressions Web Page..."));
	helpMenu->addSeparator();
	if (QAction *updateNowAction =
	        addActionToMenu(helpMenu, QStringLiteral("UpdateQmudNow"), QStringLiteral("&Update QMud Now")))
	{
		updateNowAction->setVisible(false);
		updateNowAction->setEnabled(false);
	}
	addActionToMenu(helpMenu, QStringLiteral("About"), QStringLiteral("&About QMud..."));
}

void MainWindow::buildMdiArea()
{
	// MDI area hosting world, activity, and text child windows.
	m_mdiArea = new QMdiArea(this);

	m_centralContainer = new QWidget(this);
	m_centralLayout    = new QVBoxLayout(m_centralContainer);
	m_centralLayout->setContentsMargins(0, 0, 0, 0);
	m_centralLayout->setSpacing(0);

	// Tabbed Windows - if wanted
	m_mdiTabs.create(m_mdiArea, kMdiTabsTop);

	m_centralLayout->addWidget(&m_mdiTabs);
	m_centralLayout->addWidget(m_mdiArea);
	setCentralWidget(m_centralContainer);

	connect(m_mdiArea, &QMdiArea::subWindowActivated, this, &MainWindow::onMdiSubWindowActivated);
}

void MainWindow::buildStatusBar()
{
	m_qtStatusBar = statusBar();
	if (!m_qtStatusBar)
	{
		m_qtStatusBar = new QStatusBar(this);
		setStatusBar(m_qtStatusBar);
	}

	m_statusMessage  = new StatusPaneLabel(QStringLiteral("Ready"), this);
	m_statusFreeze   = new StatusPaneLabel(QStringLiteral(""), this);
	m_statusMushName = new StatusPaneLabel(QStringLiteral("(No world active)"), this);
	m_statusTime     = new StatusPaneLabel(QStringLiteral(""), this);
	m_statusLines    = new StatusPaneLabel(QStringLiteral(""), this);
	m_statusLog      = new StatusPaneLabel(QStringLiteral(""), this);
	m_statusCaps     = new StatusPaneLabel(QStringLiteral(""), this);

	// Keep permanent panes visibly present like fixed status indicators.
	auto setupPane = [](StatusPaneLabel *pane, const QString &widestText)
	{
		if (!pane)
			return;
		pane->setFrameStyle(QFrame::Panel | QFrame::Sunken);
		pane->setAlignment(Qt::AlignCenter);
		const int width = QFontMetrics(pane->font()).horizontalAdvance(widestText) + 10;
		pane->setMinimumWidth(width);
	};
	setupPane(m_statusFreeze, QStringLiteral(" Exit Split View "));
	setupPane(m_statusMushName, QStringLiteral(" (No world active) "));
	setupPane(m_statusTime, QStringLiteral(" 99d 23h 59m 59s "));
	setupPane(m_statusLines, QStringLiteral(" 999999 "));
	setupPane(m_statusLog, QStringLiteral(" LOG "));
	setupPane(m_statusCaps, QStringLiteral(" CAP "));

	m_qtStatusBar->addWidget(m_statusMessage, 1);
	m_qtStatusBar->addPermanentWidget(m_statusFreeze);
	m_qtStatusBar->addPermanentWidget(m_statusMushName);
	m_qtStatusBar->addPermanentWidget(m_statusTime);
	m_qtStatusBar->addPermanentWidget(m_statusLines);
	m_qtStatusBar->addPermanentWidget(m_statusLog);
	m_qtStatusBar->addPermanentWidget(m_statusCaps);

	// status bar interactions mirror fixed status pane clicks
	connect(m_statusFreeze, &StatusPaneLabel::doubleClicked, this, &MainWindow::onStatusFreezeDoubleClick);
	connect(m_statusLines, &StatusPaneLabel::doubleClicked, this, &MainWindow::onStatusLinesDoubleClick);
	connect(m_statusTime, &StatusPaneLabel::doubleClicked, this, &MainWindow::onStatusTimeDoubleClick);
	connect(m_statusMushName, &StatusPaneLabel::clicked, this, &MainWindow::onStatusMushnameClick);

	m_statusTimer = new QTimer(this);
	connect(m_statusTimer, &QTimer::timeout, this, [this] { updateStatusBar(); });
	m_statusTimer->start(1000);

	m_statusMessageTimer = new QTimer(this);
	m_statusMessageTimer->setSingleShot(true);
	connect(m_statusMessageTimer, &QTimer::timeout, this, &MainWindow::setStatusNormal);

	// Keep freeze pane visibly delineated regardless of style/theme.
	m_statusFreeze->setStyleSheet(
	    QStringLiteral("border: 1px solid black; padding-left: 3px; padding-right: 3px;"));
	updateStatusBar();
	updateEditActions();
	refreshActionState();
}

void MainWindow::buildInfoBar()
{
	// new in 3.29 - Info Bar
	m_infoDock = new QDockWidget(QStringLiteral("Info"), this);
	m_infoDock->setObjectName(QStringLiteral("InfoBar"));
	m_infoDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
	m_infoDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
	m_infoDock->setTitleBarWidget(new QWidget(m_infoDock));
	m_infoText = new QTextEdit(m_infoDock);
	m_infoText->setReadOnly(true);
	m_infoText->setLineWrapMode(QTextEdit::NoWrap);
	m_infoText->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_infoText->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_infoText->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

	const QFontMetrics metrics(m_infoText->font());
	const int          textHeight = metrics.height() + 10;
	m_infoText->setFixedHeight(textHeight);
	const int frameWidth =
	    m_infoDock->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, m_infoDock);
	const int dockHeight = textHeight + frameWidth * 2;
	m_infoDock->setMinimumHeight(dockHeight);
	m_infoDock->setMaximumHeight(dockHeight);

	// remember default format
	m_defaultInfoBarFormat = m_infoText->currentCharFormat();

	// delete everything
	m_infoText->clear();

	m_infoDock->setWidget(m_infoText);
	addDockWidget(Qt::BottomDockWidgetArea, m_infoDock);
	m_infoBarWidget = m_infoDock;

	applyInfoBarAppearance(nullptr);
	m_infoDock->setVisible(false);
}

void MainWindow::applyInfoBarAppearance(WorldRuntime *runtime) const
{
	if (!m_infoText)
		return;
	QColor outputBack;
	QColor outputText;
	if (runtime)
	{
		const QMap<QString, QString> &attrs = runtime->worldAttributes();
		outputBack = WorldView::parseColor(attrs.value(QStringLiteral("output_background_colour")));
		outputText = WorldView::parseColor(attrs.value(QStringLiteral("output_text_colour")));
		if (!outputBack.isValid() && runtime->backgroundColour() != 0)
		{
			const long colour = runtime->backgroundColour();
			const int  r      = static_cast<int>(colour & 0xFF);
			const int  g      = static_cast<int>(colour >> 8 & 0xFF);
			const int  b      = static_cast<int>(colour >> 16 & 0xFF);
			outputBack        = QColor(r, g, b);
		}
	}
	constexpr QColor fallbackBack(0, 0, 0);
	constexpr QColor fallbackText(192, 192, 192);
	const QColor     resolvedBack = outputBack.isValid() ? outputBack : fallbackBack;
	const QColor     resolvedText = outputText.isValid() ? outputText : fallbackText;

	QPalette         pal = m_infoText->palette();
	pal.setColor(QPalette::Base, resolvedBack);
	pal.setColor(QPalette::Window, resolvedBack);
	pal.setColor(QPalette::Text, resolvedText);
	m_infoText->setAutoFillBackground(true);
	m_infoText->setPalette(pal);

	QTextCharFormat fmt = m_infoText->currentCharFormat();
	fmt.setForeground(resolvedText);
	m_infoText->setCurrentCharFormat(fmt);
}

void MainWindow::buildToolbars()
{
	// Main and game toolbars now use standalone PNG icons from resources/qmud/res/toolbar/.

	const QHash<QString, QString> tooltipOverrides = {
	    {QStringLiteral("New"),                  QStringLiteral("New world")                   },
	    {QStringLiteral("Open"),                 QStringLiteral("Open world")                  },
	    {QStringLiteral("Save"),                 QStringLiteral("Save world")                  },
	    {QStringLiteral("Print"),                QStringLiteral("Print")                       },
	    {QStringLiteral("NotesWorkArea"),        QStringLiteral("Notepad")                     },
	    {QStringLiteral("Cut"),                  QStringLiteral("Cut")                         },
	    {QStringLiteral("Copy"),                 QStringLiteral("Copy")                        },
	    {QStringLiteral("Paste"),                QStringLiteral("Paste")                       },
	    {QStringLiteral("About"),                QStringLiteral("About")                       },
	    {QStringLiteral("ContextHelp"),          QStringLiteral("Help")                        },
	    {QStringLiteral("GameWrapLines"),        QStringLiteral("Line wrap")                   },
	    {QStringLiteral("LogSession"),           QStringLiteral("Log session (Shift+Ctrl+J)")  },
	    {QStringLiteral("Connect_Or_Reconnect"), QStringLiteral("Connect / Disconnect")        },
	    {QStringLiteral("Preferences"),          QStringLiteral("World details (Ctrl+G)")      },
	    {QStringLiteral("ConfigureTriggers"),    QStringLiteral("Triggers (Shift+Ctrl+8)")     },
	    {QStringLiteral("ConfigureAliases"),     QStringLiteral("Aliases (Shift+Ctrl+9)")      },
	    {QStringLiteral("ConfigureTimers"),      QStringLiteral("Timers (Shift+Ctrl+0)")       },
	    {QStringLiteral("ConfigureOutput"),      QStringLiteral("Output (Alt+5)")              },
	    {QStringLiteral("ConfigureCommands"),    QStringLiteral("Commands (Alt+0)")            },
	    {QStringLiteral("ConfigureScripting"),   QStringLiteral("Scripting (Shift+Ctrl+6)")    },
	    {QStringLiteral("ConfigureNotes"),       QStringLiteral("Notes (Alt+4)")               },
	    {QStringLiteral("ConfigureVariables"),   QStringLiteral("Variables (Shift+Ctrl+7)")    },
	    {QStringLiteral("ResetAllTimers"),       QStringLiteral("Reset timers (Shift+Ctrl+T)") },
	    {QStringLiteral("ReloadScriptFile"),     QStringLiteral("Reload script (Shift+Ctrl+R)")},
	    {QStringLiteral("AutoSay"),              QStringLiteral("Auto Say (Shift+Ctrl+A)")     },
	    {QStringLiteral("FreezeOutput"),         QStringLiteral("Pause (Ctrl+Space)")          },
	    {QStringLiteral("Find"),                 QStringLiteral("Find (Ctrl+F)")               },
	    {QStringLiteral("FindAgain"),            QStringLiteral("Find again (Shift+Ctrl+F)")   },
	    {QStringLiteral("FindAgainBackwards"),   QStringLiteral("Find again backwards")        },
	    {QStringLiteral("World1"),               QStringLiteral("Activates world #1 (Ctrl+1)") },
	    {QStringLiteral("World2"),               QStringLiteral("Activates world #2 (Ctrl+2)") },
	    {QStringLiteral("World3"),               QStringLiteral("Activates world #3 (Ctrl+3)") },
	    {QStringLiteral("World4"),               QStringLiteral("Activates world #4 (Ctrl+4)") },
	    {QStringLiteral("World5"),               QStringLiteral("Activates world #5 (Ctrl+5)") },
	    {QStringLiteral("World6"),               QStringLiteral("Activates world #6 (Ctrl+6)") },
	    {QStringLiteral("World7"),               QStringLiteral("Activates world #7 (Ctrl+7)") },
	    {QStringLiteral("World8"),               QStringLiteral("Activates world #8 (Ctrl+8)") },
	    {QStringLiteral("World9"),               QStringLiteral("Activates world #9 (Ctrl+9)") },
	    {QStringLiteral("World10"),              QStringLiteral("Activates world #10 (Ctrl+0)")}
    };

	auto prettyName = [](const QString &cmd) -> QString
	{
		QString out;
		for (int i = 0; i < cmd.size(); ++i)
		{
			const QChar c = cmd.at(i);
			if (c == QLatin1Char('_'))
			{
				out += QLatin1Char(' ');
				continue;
			}
			if (i > 0 && c.isUpper())
			{
				if (const QChar prev = cmd.at(i - 1); prev.isLower() || prev.isDigit())
					out += QLatin1Char(' ');
			}
			out += c;
		}
		return out.trimmed();
	};

	auto sanitizeTip = [](const QString &raw) -> QString
	{
		QString text = raw;
		text.remove(QLatin1Char('&'));
		if (text.endsWith(QStringLiteral("...")))
			text.chop(3);
		return text.trimmed();
	};

	auto applyTooltip = [tooltipOverrides, prettyName, sanitizeTip](QAction *action, const QString &cmdName,
	                                                                const QString &fallback)
	{
		if (!action)
			return;
		QString tip;
		if (tooltipOverrides.contains(cmdName))
			tip = tooltipOverrides.value(cmdName);
		else
			tip = fallback.isEmpty() ? prettyName(cmdName) : fallback;
		tip = sanitizeTip(tip);
		if (tip.isEmpty())
			return;
		action->setToolTip(tip);
		action->setStatusTip(tip);
	};

	auto applyIcon = [](QAction *action, const QString &iconName)
	{
		if (!action)
			return;
		action->setIcon(toolbarIconByName(iconName));
	};
	auto ensureAction = [this, applyTooltip](const QString &cmdName) -> QAction *
	{
		if (m_actions.contains(cmdName))
			return m_actions.value(cmdName);
		auto *action = new QAction(this);
		action->setObjectName(cmdName);
		action->setIconVisibleInMenu(false);
		applyTooltip(action, cmdName, QString());
		connect(action, &QAction::triggered, this, &MainWindow::onActionTriggered);
		m_actions.insert(cmdName, action);
		return action;
	};

	m_mainToolbarWidget = new QToolBar(QStringLiteral("Main"), this);
	m_mainToolbarWidget->setObjectName(QStringLiteral("MainToolBar"));
	addToolBar(m_mainToolbarWidget);
	m_mainToolbarWidget->setToolButtonStyle(Qt::ToolButtonIconOnly);
	m_mainToolbarWidget->setIconSize(QSize(32, 32));
	if (QAction *a = ensureAction(QStringLiteral("New")))
	{
		applyIcon(a, QStringLiteral("main_new"));
		applyTooltip(a, QStringLiteral("New"), QString());
		m_mainToolbarWidget->addAction(a);
	}
	addToolbarSeparator(m_mainToolbarWidget);
	if (m_actions.contains(QStringLiteral("Open")))
	{
		QAction *a = m_actions.value(QStringLiteral("Open"));
		applyIcon(a, QStringLiteral("main_open"));
		applyTooltip(a, QStringLiteral("Open"), QString());
		m_mainToolbarWidget->addAction(a);
	}
	if (m_actions.contains(QStringLiteral("Save")))
	{
		QAction *a = m_actions.value(QStringLiteral("Save"));
		applyIcon(a, QStringLiteral("main_save"));
		applyTooltip(a, QStringLiteral("Save"), QString());
		m_mainToolbarWidget->addAction(a);
	}
	if (m_actions.contains(QStringLiteral("Print")))
	{
		QAction *a = m_actions.value(QStringLiteral("Print"));
		applyIcon(a, QStringLiteral("main_print"));
		applyTooltip(a, QStringLiteral("Print"), QString());
		m_mainToolbarWidget->addAction(a);
	}
	addToolbarSeparator(m_mainToolbarWidget);
	if (QAction *a = ensureAction(QStringLiteral("NotesWorkArea")))
	{
		applyIcon(a, QStringLiteral("main_notes"));
		applyTooltip(a, QStringLiteral("NotesWorkArea"), QString());
		m_mainToolbarWidget->addAction(a);
	}
	addToolbarSeparator(m_mainToolbarWidget);
	if (m_actions.contains(QStringLiteral("Cut")))
	{
		QAction *a = m_actions.value(QStringLiteral("Cut"));
		applyIcon(a, QStringLiteral("main_cut"));
		applyTooltip(a, QStringLiteral("Cut"), QString());
		m_mainToolbarWidget->addAction(a);
	}
	if (m_actions.contains(QStringLiteral("Copy")))
	{
		QAction *a = m_actions.value(QStringLiteral("Copy"));
		applyIcon(a, QStringLiteral("main_copy"));
		applyTooltip(a, QStringLiteral("Copy"), QString());
		m_mainToolbarWidget->addAction(a);
	}
	if (m_actions.contains(QStringLiteral("Paste")))
	{
		QAction *a = m_actions.value(QStringLiteral("Paste"));
		applyIcon(a, QStringLiteral("main_paste"));
		applyTooltip(a, QStringLiteral("Paste"), QString());
		m_mainToolbarWidget->addAction(a);
	}
	addToolbarSeparator(m_mainToolbarWidget);
	if (m_actions.contains(QStringLiteral("About")))
	{
		QAction *a = m_actions.value(QStringLiteral("About"));
		applyIcon(a, QStringLiteral("main_about"));
		applyTooltip(a, QStringLiteral("About"), QString());
		m_mainToolbarWidget->addAction(a);
	}
	if (QAction *a = ensureAction(QStringLiteral("ContextHelp")))
	{
		applyIcon(a, QStringLiteral("main_help"));
		applyTooltip(a, QStringLiteral("ContextHelp"), QString());
		m_mainToolbarWidget->addAction(a);
	}

	m_worldToolbarWidget = new QToolBar(QStringLiteral("Game"), this);
	m_worldToolbarWidget->setObjectName(QStringLiteral("GameToolBar"));
	addToolBar(m_worldToolbarWidget);
	m_worldToolbarWidget->setToolButtonStyle(Qt::ToolButtonIconOnly);
	m_worldToolbarWidget->setIconSize(QSize(32, 32));
	if (QAction *a = ensureAction(QStringLiteral("GameWrapLines")))
	{
		applyIcon(a, QStringLiteral("game_wrap"));
		applyTooltip(a, QStringLiteral("GameWrapLines"), QString());
		m_worldToolbarWidget->addAction(a);
	}
	if (QAction *a = ensureAction(QStringLiteral("LogSession")))
	{
		applyIcon(a, QStringLiteral("game_log"));
		applyTooltip(a, QStringLiteral("LogSession"), QString());
		m_worldToolbarWidget->addAction(a);
	}
	addToolbarSeparator(m_worldToolbarWidget);
	if (QAction *a = ensureAction(QStringLiteral("Connect_Or_Reconnect")))
	{
		applyIcon(a, QStringLiteral("game_connect"));
		applyTooltip(a, QStringLiteral("Connect_Or_Reconnect"), QString());
		m_worldToolbarWidget->addAction(a);
	}
	addToolbarSeparator(m_worldToolbarWidget);
	if (QAction *a = ensureAction(QStringLiteral("Preferences")))
	{
		applyIcon(a, QStringLiteral("game_preferences"));
		applyTooltip(a, QStringLiteral("Preferences"), QString());
		m_worldToolbarWidget->addAction(a);
	}
	if (QAction *a = ensureAction(QStringLiteral("ConfigureTriggers")))
	{
		applyIcon(a, QStringLiteral("game_triggers"));
		applyTooltip(a, QStringLiteral("ConfigureTriggers"), QString());
		m_worldToolbarWidget->addAction(a);
	}
	if (QAction *a = ensureAction(QStringLiteral("ConfigureAliases")))
	{
		applyIcon(a, QStringLiteral("game_aliases"));
		applyTooltip(a, QStringLiteral("ConfigureAliases"), QString());
		m_worldToolbarWidget->addAction(a);
	}
	if (QAction *a = ensureAction(QStringLiteral("ConfigureTimers")))
	{
		applyIcon(a, QStringLiteral("game_timers"));
		applyTooltip(a, QStringLiteral("ConfigureTimers"), QString());
		m_worldToolbarWidget->addAction(a);
	}
	if (QAction *a = ensureAction(QStringLiteral("ConfigureOutput")))
	{
		applyIcon(a, QStringLiteral("game_output"));
		applyTooltip(a, QStringLiteral("ConfigureOutput"), QString());
		m_worldToolbarWidget->addAction(a);
	}
	if (QAction *a = ensureAction(QStringLiteral("ConfigureCommands")))
	{
		applyIcon(a, QStringLiteral("game_commands"));
		applyTooltip(a, QStringLiteral("ConfigureCommands"), QString());
		m_worldToolbarWidget->addAction(a);
	}
	if (QAction *a = ensureAction(QStringLiteral("ConfigureScripting")))
	{
		applyIcon(a, QStringLiteral("game_scripting"));
		applyTooltip(a, QStringLiteral("ConfigureScripting"), QString());
		m_worldToolbarWidget->addAction(a);
	}
	if (QAction *a = ensureAction(QStringLiteral("ConfigureNotes")))
	{
		applyIcon(a, QStringLiteral("game_notes"));
		applyTooltip(a, QStringLiteral("ConfigureNotes"), QString());
		m_worldToolbarWidget->addAction(a);
	}
	if (QAction *a = ensureAction(QStringLiteral("ConfigureVariables")))
	{
		applyIcon(a, QStringLiteral("game_variables"));
		applyTooltip(a, QStringLiteral("ConfigureVariables"), QString());
		m_worldToolbarWidget->addAction(a);
	}
	addToolbarSeparator(m_worldToolbarWidget);
	if (QAction *a = ensureAction(QStringLiteral("ResetAllTimers")))
	{
		applyIcon(a, QStringLiteral("game_reset_timers"));
		applyTooltip(a, QStringLiteral("ResetAllTimers"), QString());
		m_worldToolbarWidget->addAction(a);
	}
	if (QAction *a = ensureAction(QStringLiteral("ReloadScriptFile")))
	{
		applyIcon(a, QStringLiteral("game_reload_script"));
		applyTooltip(a, QStringLiteral("ReloadScriptFile"), QString());
		m_worldToolbarWidget->addAction(a);
	}
	addToolbarSeparator(m_worldToolbarWidget);
	if (QAction *a = ensureAction(QStringLiteral("AutoSay")))
	{
		applyIcon(a, QStringLiteral("game_autosay"));
		applyTooltip(a, QStringLiteral("AutoSay"), QString());
		m_worldToolbarWidget->addAction(a);
	}
	if (QAction *a = ensureAction(QStringLiteral("FreezeOutput")))
	{
		a->setCheckable(true);
		applyIcon(a, QStringLiteral("game_freeze"));
		applyTooltip(a, QStringLiteral("FreezeOutput"), QString());
		m_worldToolbarWidget->addAction(a);
	}
	addToolbarSeparator(m_worldToolbarWidget);
	if (QAction *a = ensureAction(QStringLiteral("Find")))
	{
		applyIcon(a, QStringLiteral("game_find"));
		applyTooltip(a, QStringLiteral("Find"), QString());
		m_worldToolbarWidget->addAction(a);
	}
	if (QAction *a = ensureAction(QStringLiteral("FindAgainBackwards")))
	{
		applyIcon(a, QStringLiteral("game_find_backward"));
		applyTooltip(a, QStringLiteral("FindAgainBackwards"), QString());
		m_worldToolbarWidget->addAction(a);
	}
	if (QAction *a = ensureAction(QStringLiteral("FindAgainForwards")))
	{
		applyIcon(a, QStringLiteral("game_find_forward"));
		applyTooltip(a, QStringLiteral("FindAgainForwards"), QString());
		m_worldToolbarWidget->addAction(a);
	}

	m_activityToolbarWidget = new QToolBar(QStringLiteral("Activity"), this);
	m_activityToolbarWidget->setObjectName(QStringLiteral("ActivityToolBar"));
	addToolBar(m_activityToolbarWidget);
	m_activityToolbarWidget->setToolButtonStyle(Qt::ToolButtonTextOnly);
	m_activityToolbarWidget->setIconSize(QSize(32, 32));
	m_worldActions.clear();
	updateActivityToolbarButtons();

	if (m_defaultToolbarState.isEmpty())
		m_defaultToolbarState = saveState();
}

QWidget *MainWindow::createMdiChild(const QString &title)
{
	if (!m_mdiArea)
		return nullptr;

	auto *child  = new QWidget();
	auto *layout = new QVBoxLayout(child);
	auto *label  = new QLabel(title, child);
	layout->addWidget(label);
	child->setLayout(layout);

	auto *sub = new QMdiSubWindow();
	sub->setWidget(child);
	sub->setWindowTitle(title);
	addMdiSubWindow(sub, false);
	return child;
}

void MainWindow::addMdiSubWindow(QMdiSubWindow *subWindow, const bool activate)
{
	if (!m_mdiArea || !subWindow)
		return;

	const QPointer<QMdiSubWindow> previousActive =
	    !activate ? QMudMainFrameMdiUtils::resolveBackgroundAddRestoreTarget(
	                    m_mdiArea->activeSubWindow(), m_lastActiveSubWindow,
	                    m_mdiArea->subWindowList(QMdiArea::CreationOrder), subWindow)
	              : nullptr;

	auto *world      = qobject_cast<WorldChildWindow *>(subWindow);
	auto *textWindow = qobject_cast<TextChildWindow *>(subWindow);
	m_mdiArea->addSubWindow(subWindow);
	subWindow->installEventFilter(this);
	connect(subWindow, &QObject::destroyed, this,
	        [this](QObject *obj)
	        {
		        if (m_lastActiveSubWindow && m_lastActiveSubWindow.data() == obj)
			        m_lastActiveSubWindow.clear();
		        if (!m_mdiArea)
			        return;
		        const QList<QMdiSubWindow *>  windows = m_mdiArea->subWindowList(QMdiArea::CreationOrder);
		        const QPointer<QMdiSubWindow> fallback =
		            m_tabActivationHistory.takeFallbackOnDestroyed(obj, windows);
		        if (!fallback)
			        return;
		        if (m_mdiArea->activeSubWindow() != fallback)
			        m_mdiArea->setActiveSubWindow(fallback);
	        });
	if (world || textWindow)
		subWindow->showMaximized();
	else
		subWindow->show();
	if (world)
	{
		connect(world, &QObject::destroyed, this,
		        [this, world]
		        {
			        Q_UNUSED(world);
			        updateActivityToolbarButtons();
		        });
		updateActivityToolbarButtons();
	}
	if (activate)
	{
		m_mdiArea->setActiveSubWindow(subWindow);
		if (world || textWindow)
			subWindow->showMaximized();
		else
			subWindow->show();
		subWindow->raise();
		subWindow->activateWindow();
		if (world)
		{
			if (WorldView *view = world->view())
				view->focusInput();
		}
	}
	else if (previousActive)
	{
		m_mdiArea->setActiveSubWindow(previousActive);
	}
	m_mdiTabs.updateTabs();
}

WorldChildWindow *MainWindow::activeWorldChildWindow() const
{
	return qobject_cast<WorldChildWindow *>(currentOrLastActiveSubWindow());
}

TextChildWindow *MainWindow::activeTextChildWindow() const
{
	return qobject_cast<TextChildWindow *>(currentOrLastActiveSubWindow());
}

QMdiSubWindow *MainWindow::currentOrLastActiveSubWindow() const
{
	if (!m_mdiArea)
		return nullptr;
	return QMudMainFrameMdiUtils::resolveCurrentOrLastActiveSubWindow(
	    m_mdiArea->activeSubWindow(), m_lastActiveSubWindow,
	    m_mdiArea->subWindowList(QMdiArea::CreationOrder));
}

WorldChildWindow *MainWindow::findWorldChildWindow(WorldRuntime *runtime) const
{
	if (!m_mdiArea || !runtime)
		return nullptr;

	for (QMdiSubWindow *sub : m_mdiArea->subWindowList(QMdiArea::CreationOrder))
	{
		auto *world = qobject_cast<WorldChildWindow *>(sub);
		if (!world)
			continue;
		if (world->runtime() == runtime)
			return world;
	}

	return nullptr;
}

bool MainWindow::activateWorldRuntime(WorldRuntime *runtime)
{
	WorldChildWindow *world = findWorldChildWindow(runtime);
	if (!world || !m_mdiArea)
		return false;

	m_mdiArea->setActiveSubWindow(world);
	if (world->isMinimized())
		world->showNormal();
	world->show();
	world->activateWindow();
	world->raise();
	if (WorldView *view = world->view())
		view->focusInput();
	return true;
}

void MainWindow::setTrayIconVisible(const bool visible)
{
	if (visible)
	{
		if (!m_trayIcon)
		{
			m_trayIcon = new QSystemTrayIcon(this);
			m_trayIcon->setToolTip(QStringLiteral("QMud"));
			m_trayIcon->setIcon(windowIcon());
			connect(m_trayIcon, &QSystemTrayIcon::activated, this,
			        [this](const QSystemTrayIcon::ActivationReason reason)
			        {
				        if (reason == QSystemTrayIcon::Context)
					        return;

				        const bool shouldRestore = isMinimized() || !isVisible() ||
				                                   windowState().testFlag(Qt::WindowMinimized) ||
				                                   !isActiveWindow();

				        if (shouldRestore)
				        {
					        showNormal();
					        raise();
					        activateWindow();
				        }
				        else if (reason == QSystemTrayIcon::Trigger)
				        {
					        if (isTrayOnlyIconPlacement())
						        hide();
					        else
						        showMinimized();
				        }
			        });
		}
		m_trayIcon->show();
	}
	else
	{
		if (m_trayIcon)
			m_trayIcon->hide();
	}
}

void MainWindow::setTrayIconIcon(const QIcon &icon)
{
	setWindowIcon(icon);
	if (m_trayIcon)
		m_trayIcon->setIcon(icon);
}

void MainWindow::setDisableKeyboardMenuActivation(const bool disabled)
{
	m_disableKeyboardMenuActivation = disabled;
}

void MainWindow::applyNotepadPreferences(const bool wrap, const QColor &text, const QColor &back) const
{
	if (!m_mdiArea)
		return;
	for (QMdiSubWindow *sub : m_mdiArea->subWindowList())
	{
		auto *textWindow = qobject_cast<TextChildWindow *>(sub);
		if (!textWindow)
			continue;
		auto *editor = textWindow->editor();
		if (!editor)
			continue;
		editor->setLineWrapMode(wrap ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
		editor->setWordWrapMode(wrap ? QTextOption::WrapAtWordBoundaryOrAnywhere : QTextOption::NoWrap);
		QPalette pal = editor->palette();
		pal.setColor(QPalette::Text, text);
		pal.setColor(QPalette::Base, back);
		editor->setPalette(pal);
		editor->setAutoFillBackground(true);
	}
}

void MainWindow::setNotepadFont(const QFont &font) const
{
	if (!m_mdiArea)
		return;
	for (QMdiSubWindow *sub : m_mdiArea->subWindowList())
	{
		auto *textWindow = qobject_cast<TextChildWindow *>(sub);
		if (!textWindow)
			continue;
		auto *editor = textWindow->editor();
		if (!editor)
			continue;
		editor->setFont(font);
	}
}

void MainWindow::setListViewGridLinesVisible(const bool visible) const
{
	if (!m_mdiArea)
		return;
	for (QMdiSubWindow *sub : m_mdiArea->subWindowList())
	{
		if (!sub)
			continue;
		if (auto *activity = qobject_cast<ActivityChildWindow *>(sub))
		{
			if (auto *view = activity->activityWindow())
				view->setGridLinesVisible(visible);
		}
	}
}

void MainWindow::setFlatToolbars(const bool flat) const
{
	const auto flatStyle = QString();
	const auto raisedStyle =
	    QStringLiteral("QToolButton { border: 1px solid palette(mid); border-radius: 2px; padding: 1px; } "
	                   "QToolButton:pressed { background: palette(button); }");

	const QString style      = flat ? flatStyle : raisedStyle;
	auto          applyStyle = [&style](QToolBar *toolbar)
	{
		if (toolbar)
			toolbar->setStyleSheet(style);
	};

	applyStyle(m_mainToolbarWidget);
	applyStyle(m_worldToolbarWidget);
	applyStyle(m_activityToolbarWidget);
}

bool MainWindow::hasActivityWindow() const
{
	if (!m_mdiArea)
		return false;
	return std::ranges::any_of(m_mdiArea->subWindowList(),
	                           [](QMdiSubWindow *sub) { return qobject_cast<ActivityChildWindow *>(sub); });
}

ActivityChildWindow *MainWindow::activityChildWindow() const
{
	if (!m_mdiArea)
		return nullptr;
	for (QMdiSubWindow *sub : m_mdiArea->subWindowList())
	{
		if (auto *activity = qobject_cast<ActivityChildWindow *>(sub))
			return activity;
	}
	return nullptr;
}

bool MainWindow::activateActivityWindow() const
{
	ActivityChildWindow *activity = activityChildWindow();
	if (!activity)
		return false;
	m_mdiArea->setActiveSubWindow(activity);
	activity->show();
	activity->raise();
	return true;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
	AppController *app = AppController::instance();
	if (app && app->getGlobalOption(QStringLiteral("ConfirmBeforeClosingQmud")).toInt() != 0 &&
	    worldWindowCount() > 0)
	{
		const QMessageBox::StandardButton result = QMessageBox::information(
		    this, QStringLiteral("QMud"), QStringLiteral("This will end your QMud session."),
		    QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);
		if (result != QMessageBox::Ok)
		{
			event->ignore();
			return;
		}
	}

	// Close child windows first while the frame is still visible, so any world
	// confirmation dialogs can cancel shutdown reliably.
	if (m_mdiArea)
	{
		m_mdiArea->closeAllSubWindows();
		if (!m_mdiArea->subWindowList().isEmpty())
		{
			event->ignore();
			return;
		}
	}

	if (app)
	{
		if (windowState() & Qt::WindowFullScreen)
		{
			showNormal();
			m_fullScreenMode = false;
			if (QAction *fullScreen = actionForCommand(QStringLiteral("FullScreenMode")))
			{
				QSignalBlocker blocker(fullScreen);
				fullScreen->setChecked(false);
			}
		}
		app->saveWindowPlacement();
		app->saveViewPreferences();
		app->saveSessionState();
	}

	QMainWindow::closeEvent(event);
}

void MainWindow::onToggleToolbar(const bool checked)
{
	if (m_mainToolbarWidget)
		m_mainToolbarWidget->setVisible(checked);
	emit viewPreferenceChanged(QStringLiteral("ViewToolbar"), checked ? 1 : 0);
}

void MainWindow::onToggleStatusbar(const bool checked)
{
	if (statusBar())
		statusBar()->setVisible(checked);
	emit viewPreferenceChanged(QStringLiteral("ViewStatusbar"), checked ? 1 : 0);
}

void MainWindow::onToggleInfoBar(const bool checked)
{
	if (m_infoDock)
		m_infoDock->setVisible(checked);
	emit viewPreferenceChanged(QStringLiteral("ViewInfoBar"), checked ? 1 : 0);
}

void MainWindow::showStatusMessage(const QString &message, const int timeoutMs)
{
	if (m_hyperlinkStatusLocked)
		return;
	if (m_statusMessage)
		m_statusMessage->setText(message);
	m_statusTipOwnsMessage = false;
	if (m_statusMessageTimer)
	{
		if (timeoutMs > 0)
			m_statusMessageTimer->start(timeoutMs);
		else
			m_statusMessageTimer->stop();
	}
}

void MainWindow::setHyperlinkStatusLock(const QString &href)
{
	const QString trimmed = href.trimmed();
	if (trimmed.isEmpty())
		return;
	m_hyperlinkStatusLocked = true;
	m_statusTipOwnsMessage  = false;
	if (m_statusMessage)
		m_statusMessage->setText(trimmed);
	if (m_statusMessageTimer)
		m_statusMessageTimer->stop();
	if (const auto *world = activeWorldChildWindow())
		if (WorldRuntime *runtime = world->runtime())
			runtime->setStatusMessage(trimmed);
}

void MainWindow::clearHyperlinkStatusLock()
{
	if (!m_hyperlinkStatusLocked)
		return;
	m_hyperlinkStatusLocked = false;
	setStatusNormal();
}

void MainWindow::setRecentFiles(const QStringList &files)
{
	const int count = qMin(sizeToInt(files.size()), m_recentMax);
	for (int i = 0; i < m_recentActions.size(); ++i)
	{
		QAction *act = m_recentActions[i];
		if (i < count)
		{
			const QString &path = files[i];
			act->setText(QStringLiteral("&%1 %2").arg(i + 1).arg(path));
			act->setData(path);
			act->setVisible(true);
		}
		else
		{
			act->setVisible(false);
		}
	}
}

void MainWindow::onRecentFileAction()
{
	const QAction *act = qobject_cast<QAction *>(sender());
	if (!act)
		return;
	if (const QString path = act->data().toString(); !path.isEmpty())
		emit recentFileTriggered(path);
}

void MainWindow::resetToolbarsToDefaults()
{
	if (m_mainToolbarWidget)
	{
		const bool visible = m_mainToolbarWidget->isVisible();
		addToolBar(Qt::TopToolBarArea, m_mainToolbarWidget);
		m_mainToolbarWidget->setVisible(visible);
	}
	if (m_worldToolbarWidget)
	{
		const bool visible = m_worldToolbarWidget->isVisible();
		addToolBar(Qt::TopToolBarArea, m_worldToolbarWidget);
		m_worldToolbarWidget->setVisible(visible);
	}
	if (m_activityToolbarWidget)
	{
		const bool visible = m_activityToolbarWidget->isVisible();
		addToolBar(Qt::TopToolBarArea, m_activityToolbarWidget);
		m_activityToolbarWidget->setVisible(visible);
	}
	if (m_infoDock)
	{
		const bool visible = m_infoDock->isVisible();
		addDockWidget(Qt::BottomDockWidgetArea, m_infoDock);
		m_infoDock->setVisible(visible);
	}
}

void MainWindow::syncToolbarVisibilityFromState() const
{
	if (QAction *toolbarAction = actionForCommand(QStringLiteral("ViewToolbar")))
	{
		QSignalBlocker blocker(toolbarAction);
		toolbarAction->setChecked(m_mainToolbarWidget && !m_mainToolbarWidget->isHidden());
	}
	if (QAction *worldToolbarAction = actionForCommand(QStringLiteral("ViewWorldToolbar")))
	{
		QSignalBlocker blocker(worldToolbarAction);
		worldToolbarAction->setChecked(m_worldToolbarWidget && !m_worldToolbarWidget->isHidden());
	}
	if (QAction *activityToolbarAction = actionForCommand(QStringLiteral("ActivityToolbar")))
	{
		QSignalBlocker blocker(activityToolbarAction);
		activityToolbarAction->setChecked(m_activityToolbarWidget && !m_activityToolbarWidget->isHidden());
	}
	if (QAction *statusAction = actionForCommand(QStringLiteral("ViewStatusbar")))
	{
		QSignalBlocker blocker(statusAction);
		statusAction->setChecked(m_qtStatusBar && !m_qtStatusBar->isHidden());
	}
	if (QAction *infoAction = actionForCommand(QStringLiteral("ViewInfoBar")))
	{
		QSignalBlocker blocker(infoAction);
		infoAction->setChecked(m_infoDock && !m_infoDock->isHidden());
	}
}

void MainWindow::setConnectedState(const bool connected)
{
	const WorldRuntime *runtime = nullptr;
	if (const WorldChildWindow *world = activeWorldChildWindow())
		runtime = world->runtime();
	updateConnectionActions(runtime);
	showStatusMessage(connected ? QStringLiteral("Connected.") : QStringLiteral("Disconnected."), 3000);
}

void MainWindow::updateConnectionActions(const WorldRuntime *runtime) const
{
	const bool hasRuntime = runtime != nullptr;
	const bool connected  = hasRuntime && runtime->isConnected();
	const bool connecting = hasRuntime && runtime->isConnecting();
	bool       canConnect = false;
	if (hasRuntime)
	{
		const QMap<QString, QString> &attrs = runtime->worldAttributes();
		const QString                 host  = attrs.value(QStringLiteral("site"));
		canConnect = !connected && !connecting && !host.isEmpty() && host != QStringLiteral("0.0.0.0");
	}

	if (m_actions.contains(QStringLiteral("Connect")))
		m_actions.value(QStringLiteral("Connect"))->setEnabled(canConnect);
	if (m_actions.contains(QStringLiteral("Disconnect")))
		m_actions.value(QStringLiteral("Disconnect"))->setEnabled(hasRuntime && (connected || connecting));
	if (QAction *action = m_actions.value(QStringLiteral("Connect_Or_Reconnect"), nullptr))
	{
		action->setCheckable(true);
		action->setChecked(connected || connecting);
		action->setEnabled(hasRuntime);
	}
}

void MainWindow::onActionTriggered()
{
	const QAction *a = qobject_cast<QAction *>(sender());
	if (!a)
		return;

	const QString cmd = a->objectName();
	qDebug() << "Action triggered:" << cmd;

	if (cmd == QStringLiteral("CascadeWindows"))
	{
		if (m_mdiArea)
			m_mdiArea->cascadeSubWindows();
		return;
	}
	if (cmd == QStringLiteral("TileWindows"))
	{
		if (m_mdiArea)
		{
			const QList<QMdiSubWindow *> windows = m_mdiArea->subWindowList(QMdiArea::CreationOrder);
			if (const int count = sizeToInt(windows.size()); count > 0)
			{
				const QRect area   = m_mdiArea->viewport()->rect();
				const int   height = area.height() / count;
				int         y      = area.top();
				for (QMdiSubWindow *sub : windows)
				{
					if (!sub)
						continue;
					sub->setGeometry(area.left(), y, area.width(), height);
					y += height;
				}
			}
		}
		return;
	}
	if (cmd == QStringLiteral("TileWindowsVert"))
	{
		if (m_mdiArea)
		{
			const QList<QMdiSubWindow *> windows = m_mdiArea->subWindowList(QMdiArea::CreationOrder);
			if (const int count = sizeToInt(windows.size()); count > 0)
			{
				const QRect area  = m_mdiArea->viewport()->rect();
				const int   width = area.width() / count;
				int         x     = area.left();
				for (QMdiSubWindow *sub : windows)
				{
					if (!sub)
						continue;
					sub->setGeometry(x, area.top(), width, area.height());
					x += width;
				}
			}
		}
		return;
	}
	if (cmd == QStringLiteral("ArrangeIcons"))
	{
		if (m_mdiArea)
		{
			const QList<QMdiSubWindow *> windows = m_mdiArea->subWindowList(QMdiArea::CreationOrder);
			QVector<QMdiSubWindow *>     minimized;
			minimized.reserve(windows.size());
			for (QMdiSubWindow *sub : windows)
			{
				if (sub && sub->windowState().testFlag(Qt::WindowMinimized))
					minimized.push_back(sub);
			}
			if (!minimized.isEmpty())
			{
				const QRect          area       = m_mdiArea->viewport()->rect();
				static constexpr int iconHeight = 28;
				static constexpr int iconWidth  = 160;
				int                  x          = area.left();
				int                  y          = area.bottom() - iconHeight;
				for (QMdiSubWindow *sub : minimized)
				{
					if (!sub)
						continue;
					if (x + iconWidth > area.right())
					{
						x = area.left();
						y -= iconHeight;
					}
					sub->setGeometry(x, y, iconWidth, iconHeight);
					x += iconWidth;
				}
			}
		}
		return;
	}
	if (cmd == QStringLiteral("WindowMinimize") || cmd == QStringLiteral("WindowMaximize") ||
	    cmd == QStringLiteral("WindowRestore"))
	{
		if (m_mdiArea)
		{
			if (QMdiSubWindow *sub = m_mdiArea->activeSubWindow())
			{
				if (cmd == QStringLiteral("WindowMinimize"))
					sub->showMinimized();
				else if (cmd == QStringLiteral("WindowMaximize"))
					sub->showMaximized();
				else
					sub->showNormal();
			}
		}
		return;
	}
	if (cmd == QStringLiteral("CloseAllNotepads"))
	{
		if (!m_mdiArea)
			return;
		for (QMdiSubWindow *sub : m_mdiArea->subWindowList(QMdiArea::CreationOrder))
		{
			if (qobject_cast<TextChildWindow *>(sub))
				sub->close();
		}
		return;
	}
	if (cmd == QStringLiteral("FreezeOutput"))
	{
		emit commandTriggered(cmd);
		return;
	}

	emit commandTriggered(cmd);

	// Keep command dispatch centralized through action identifiers.
	showStatusMessage(QStringLiteral("Triggered: ") + cmd, 3000);
}

void MainWindow::updateWindowMenu()
{
	if (!m_windowMenu)
		return;

	m_windowMenu->clear();
	QAction *newWindow        = actionForCommand(QStringLiteral("NewWindow"));
	QAction *cascade          = actionForCommand(QStringLiteral("CascadeWindows"));
	QAction *tile             = actionForCommand(QStringLiteral("TileWindows"));
	QAction *tileVert         = actionForCommand(QStringLiteral("TileWindowsVert"));
	QAction *arrangeIcons     = actionForCommand(QStringLiteral("ArrangeIcons"));
	QAction *minimize         = actionForCommand(QStringLiteral("WindowMinimize"));
	QAction *closeAllNotepads = actionForCommand(QStringLiteral("CloseAllNotepads"));
	QAction *maximize         = actionForCommand(QStringLiteral("WindowMaximize"));
	QAction *restore          = actionForCommand(QStringLiteral("WindowRestore"));
	if (newWindow)
		m_windowMenu->addAction(newWindow);
	if (cascade)
		m_windowMenu->addAction(cascade);
	if (tile)
		m_windowMenu->addAction(tile);
	if (tileVert)
		m_windowMenu->addAction(tileVert);
	if (arrangeIcons)
		m_windowMenu->addAction(arrangeIcons);
	if (minimize)
		m_windowMenu->addAction(minimize);
	if (closeAllNotepads)
		m_windowMenu->addAction(closeAllNotepads);
	if (maximize || restore)
		m_windowMenu->addSeparator();
	if (maximize)
		m_windowMenu->addAction(maximize);
	if (restore)
		m_windowMenu->addAction(restore);

	if (!m_mdiArea)
		return;

	const QList<QMdiSubWindow *> windows     = m_mdiArea->subWindowList(QMdiArea::ActivationHistoryOrder);
	const int                    windowCount = sizeToInt(windows.size());
	if (cascade)
		cascade->setEnabled(windowCount > 0);
	if (tile)
		tile->setEnabled(windowCount > 0);
	if (tileVert)
		tileVert->setEnabled(windowCount > 0);
	if (arrangeIcons)
		arrangeIcons->setEnabled(windowCount > 0);
	if (minimize)
		minimize->setEnabled(windowCount > 0);
	if (maximize)
		maximize->setEnabled(windowCount > 0);
	if (restore)
		restore->setEnabled(windowCount > 0);
	if (closeAllNotepads)
	{
		bool hasNotepad = false;
		for (QMdiSubWindow *sub : windows)
		{
			if (qobject_cast<TextChildWindow *>(sub))
			{
				hasNotepad = true;
				break;
			}
		}
		closeAllNotepads->setEnabled(hasNotepad);
	}

	if (!windows.isEmpty())
		m_windowMenu->addSeparator();

	const QMdiSubWindow *active = m_mdiArea->activeSubWindow();
	int                  index  = 1;
	for (QMdiSubWindow *sub : windows)
	{
		const QString title = sub->windowTitle();
		QString       label;
		if (index < 10)
			label = QStringLiteral("&%1 %2").arg(index).arg(title);
		else
			label = QStringLiteral("%1 %2").arg(index).arg(title);

		QPointer ptr = sub;
		auto    *act = new QAction(label, m_windowMenu);
		act->setCheckable(true);
		act->setChecked(sub == active);
		connect(act, &QAction::triggered, this,
		        [this, ptr]
		        {
			        if (!m_mdiArea || !ptr)
				        return;
			        m_mdiArea->setActiveSubWindow(ptr);
			        ptr->show();
			        ptr->setFocus();
		        });
		m_windowMenu->addAction(act);
		++index;
	}
}

void MainWindow::onMdiSubWindowActivated(QMdiSubWindow *window)
{
	if (window)
	{
		m_lastActiveSubWindow = window;
		if (m_mdiArea)
		{
			const QList<QMdiSubWindow *> currentWindows = m_mdiArea->subWindowList(QMdiArea::CreationOrder);
			m_tabActivationHistory.onActivated(window, currentWindows);
		}
	}

	WorldRuntime *activeRuntime = nullptr;
	if (m_mdiArea)
	{
		for (QMdiSubWindow *sub : m_mdiArea->subWindowList(QMdiArea::CreationOrder))
		{
			auto *world = qobject_cast<WorldChildWindow *>(sub);
			if (!world)
				continue;
			WorldRuntime *runtime = world->runtime();
			if (!runtime)
				continue;
			const bool active = sub == window;
			runtime->setActive(active);
			if (active)
				runtime->clearNewLines();
			if (active)
				activeRuntime = runtime;
		}
	}
	refreshTitleBar();
	if (activeRuntime)
	{
		if (AppController *app = AppController::instance())
			app->processScriptFileChange(activeRuntime);
	}
	updateStatusBar();
	updateEditActions();
	refreshActionState();
	updateActivityToolbarButtons();
	m_mdiTabs.updateTabs();
	applyInfoBarAppearance(activeRuntime);
	if (auto *world = qobject_cast<WorldChildWindow *>(window))
	{
		if (WorldView *view = world->view())
			view->focusInput();
	}
}

int MainWindow::worldWindowCount() const
{
	if (!m_mdiArea)
		return 0;
	int count = 0;
	for (QMdiSubWindow *sub : m_mdiArea->subWindowList(QMdiArea::CreationOrder))
	{
		if (qobject_cast<WorldChildWindow *>(sub))
			++count;
	}
	return count;
}

void MainWindow::setStatusMessage(const QString &msg) const
{
	if (m_hyperlinkStatusLocked)
		return;
	if (m_statusMessage)
		m_statusMessage->setText(msg);
	m_statusTipOwnsMessage = false;
	if (m_statusMessageTimer)
		m_statusMessageTimer->stop();
	if (const auto *world = activeWorldChildWindow())
		if (WorldRuntime *runtime = world->runtime())
			runtime->setStatusMessage(msg);
}

void MainWindow::updateEditActions()
{
	const auto *world = activeWorldChildWindow();
	const auto *view  = world ? world->view() : nullptr;
	const auto *text  = activeTextChildWindow();

	const bool  canCopy         = view && (view->hasOutputSelection() || view->hasInputSelection());
	const bool  canCopyHtml     = view && view->hasOutputSelection();
	const bool  canFindAgain    = view && view->hasOutputFindHistory();
	const bool  canClearHistory = view && view->hasCommandHistory();
	const bool  textContext     = text != nullptr;

	if (QAction *a = actionForCommand(QStringLiteral("Copy")))
		a->setEnabled(canCopy);
	if (QAction *a = actionForCommand(QStringLiteral("CopyAsHTML")))
		a->setEnabled(canCopyHtml);
	if (QAction *a = actionForCommand(QStringLiteral("Find")))
		a->setEnabled(view != nullptr);
	if (QAction *a = actionForCommand(QStringLiteral("FindAgain")))
		a->setEnabled(canFindAgain);
	if (QAction *a = actionForCommand(QStringLiteral("FindAgainBackwards")))
		a->setEnabled(canFindAgain);
	if (QAction *a = actionForCommand(QStringLiteral("ClearCommandHistory")))
		a->setEnabled(canClearHistory);
	if (QAction *a = actionForCommand(QStringLiteral("AsciiArt")))
	{
		a->setVisible(textContext);
		a->setEnabled(textContext);
	}
	const QStringList textOnly = {QStringLiteral("TextGoTo"),           QStringLiteral("InsertDateTime"),
	                              QStringLiteral("WordCount"),          QStringLiteral("SendToCommandWindow"),
	                              QStringLiteral("SendToScript"),       QStringLiteral("SendToWorld"),
	                              QStringLiteral("RefreshRecalledData")};
	for (const QString &id : textOnly)
	{
		if (QAction *a = actionForCommand(id))
		{
			a->setVisible(textContext);
			a->setEnabled(textContext);
		}
	}
}

void MainWindow::setStatusMessageNow(const QString &msg)
{
	if (m_hyperlinkStatusLocked)
		return;
	if (m_statusMessage)
	{
		m_statusMessage->setText(msg);
		m_statusMessage->repaint(); // draw now
	}
	m_statusTipOwnsMessage = false;
	if (m_statusMessageTimer)
		m_statusMessageTimer->stop();
	if (const auto *world = activeWorldChildWindow())
		if (WorldRuntime *runtime = world->runtime())
			runtime->setStatusMessage(msg);
}

void MainWindow::setShowDebugStatus(const bool enabled)
{
	m_showDebugStatus = enabled;
}

void MainWindow::setStatusNormal()
{
	if (m_hyperlinkStatusLocked)
		return;
	if (m_statusTipOwnsMessage)
		return;
	if (m_statusMessageTimer)
		m_statusMessageTimer->stop();
	setStatusMessageNow(QStringLiteral("Ready"));
}

void MainWindow::refreshTitleBar()
{
	const auto baseTitle = QStringLiteral("QMud");
	QString    mainTitle;

	if (m_mdiArea)
	{
		if (QMdiSubWindow *active = m_mdiArea->activeSubWindow())
		{
			if (const auto *world = qobject_cast<WorldChildWindow *>(active))
			{
				if (const WorldRuntime *runtime = world->runtime())
					mainTitle = runtime->mainTitleOverride();
			}
		}
	}

	if (!mainTitle.isEmpty())
		setWindowTitle(mainTitle);
	else
		setWindowTitle(baseTitle);
}

void MainWindow::checkTimerFallback()
{
	const AppController *app = AppController::instance();
	if (!app)
		return;

	if (!m_timerFallbackClock.isValid())
	{
		m_timerFallbackClock.start();
		m_lastTimerProcessNs = m_timerFallbackClock.nsecsElapsed();
		m_lastTickProcessNs  = m_lastTimerProcessNs;
		return;
	}

	const qint64 nowNs = m_timerFallbackClock.nsecsElapsed();
	if (m_lastTimerProcessNs == 0)
		m_lastTimerProcessNs = nowNs;
	if (m_lastTickProcessNs == 0)
		m_lastTickProcessNs = nowNs;

	if (const double elapsedTickSeconds = static_cast<double>(nowNs - m_lastTickProcessNs) / 1000000000.0;
	    elapsedTickSeconds > 0.040)
		processPluginTicks();

	double intervalSeconds = 0.10;
	if (const int configuredInterval = app->getGlobalOption(QStringLiteral("TimerInterval")).toInt();
	    configuredInterval > 0)
		intervalSeconds = static_cast<double>(configuredInterval);

	if (const double elapsedTimerSeconds = static_cast<double>(nowNs - m_lastTimerProcessNs) / 1000000000.0;
	    elapsedTimerSeconds > intervalSeconds)
		processTimers();
}

void MainWindow::processPluginTicks()
{
	if (!m_timerFallbackClock.isValid())
		m_timerFallbackClock.start();
	m_lastTickProcessNs = m_timerFallbackClock.nsecsElapsed();

	for (const WorldWindowDescriptor &entry : worldWindowDescriptors())
	{
		WorldRuntime *runtime = entry.runtime;
		if (!runtime)
			continue;
		runtime->firePluginTick();
	}
}

void MainWindow::processTimers()
{
	if (!m_timerFallbackClock.isValid())
		m_timerFallbackClock.start();
	m_lastTimerProcessNs = m_timerFallbackClock.nsecsElapsed();

	const AppController *app = AppController::instance();
	if (!app)
		return;

	const bool reconnectEnabled = app->getGlobalOption(QStringLiteral("ReconnectOnLinkFailure")).toInt() != 0;
	for (const WorldWindowDescriptor &entry : worldWindowDescriptors())
	{
		WorldRuntime *runtime = entry.runtime;
		if (!runtime)
			continue;

		if (reconnectEnabled)
		{
			if (!runtime->reconnectOnLinkFailure())
				continue;
			if (runtime->disconnectOk())
				continue;
			if (runtime->isConnected() || runtime->isConnecting())
				continue;

			const QMap<QString, QString> &attrs     = runtime->worldAttributes();
			const QString                 worldName = attrs.value(QStringLiteral("name"));
			const QString                 host      = attrs.value(QStringLiteral("site"));
			const int                     port      = attrs.value(QStringLiteral("port")).toInt();

			if (host == QStringLiteral("0.0.0.0"))
				continue;
			if (worldName.isEmpty() || host.isEmpty() || port <= 0)
				continue;

			setStatusMessage(QStringLiteral("Reconnecting ..."));
			runtime->connectToWorld(host, static_cast<quint16>(port));
		}
	}
}

void MainWindow::updateStatusBar()
{
	auto                   mushName = QStringLiteral("(No world active)");
	QString                timeText;
	QString                linesText;
	QString                logText;
	auto                   freezeText = QStringLiteral("Freeze");

	const WorldRuntime    *runtime    = nullptr;
	const WorldView       *view       = nullptr;
	const TextChildWindow *textWindow = nullptr;
	QPlainTextEdit        *textEditor = nullptr;
	if (QMdiSubWindow *active = currentOrLastActiveSubWindow(); active)
	{
		if (const auto *world = qobject_cast<WorldChildWindow *>(active))
		{
			runtime = world->runtime();
			view    = world->view();
		}
		else if (const auto *textChild = qobject_cast<TextChildWindow *>(active))
		{
			textWindow = textChild;
			textEditor = textChild->editor();
		}
	}
	if (runtime)
	{
		if (const QString name = runtime->worldAttributes().value(QStringLiteral("name")); !name.isEmpty())
			mushName = name;

		// connected time display
		if (runtime->isConnected() && runtime->statusTime().isValid())
		{
			const qint64 seconds = runtime->statusTime().secsTo(QDateTime::currentDateTime());
			const qint64 days    = seconds / 86400;
			const qint64 hours   = (seconds % 86400) / 3600;
			const qint64 minutes = (seconds % 3600) / 60;
			const qint64 secs    = seconds % 60;

			if (days > 0)
				timeText = QStringLiteral("%1d %2h %3m %4s").arg(days).arg(hours).arg(minutes).arg(secs);
			else if (hours > 0)
				timeText = QStringLiteral("%1h %2m %3s").arg(hours).arg(minutes).arg(secs);
			else if (minutes > 0)
				timeText = QStringLiteral("%1m %2s").arg(minutes).arg(secs);
			else
				timeText = QStringLiteral("%1s").arg(secs);
		}

		// lines count
		int lineCount = sizeToInt(runtime->lines().size());
		if (lineCount > 0 && runtime->lines().last().text.isEmpty())
			--lineCount;
		if (lineCount < 0)
			lineCount = 0;
		linesText = QString::number(lineCount);

		// log indicator
		logText = runtime->isLogOpen() ? QStringLiteral("LOG") : QString();
	}
	else if (textEditor)
	{
		if (const QString title = textWindow->windowTitle(); title.startsWith(QStringLiteral("Notepad: ")))
		{
			const QString related = title.mid(9).trimmed();
			mushName              = related.isEmpty() ? QStringLiteral("(no related world)") : related;
		}
		else
		{
			mushName = QStringLiteral("(no related world)");
		}

		int lineCount   = 0;
		int currentLine = 0;
		if (textEditor->document())
		{
			lineCount                = textEditor->document()->blockCount();
			const QTextCursor cursor = textEditor->textCursor();
			currentLine              = cursor.blockNumber() + 1;
		}
		if (lineCount < 0)
			lineCount = 0;
		if (currentLine < 1)
			currentLine = 1;
		if (lineCount > 0)
			timeText = QStringLiteral("Line %1 / %2").arg(currentLine).arg(lineCount);
		double percent = 0.0;
		if (lineCount > 0)
			percent = static_cast<double>(currentLine) / static_cast<double>(lineCount) * 100.0;
		linesText = QString::asprintf("%3.0f %%", percent);

		logText = textEditor->overwriteMode() ? QStringLiteral("OVR") : QStringLiteral("INS");
	}

	const bool isFrozen = view && view->isFrozen();

	if (runtime)
	{
		if (view && view->isScrollbackSplitActive())
			freezeText = QStringLiteral("Exit Split View");
		else
			freezeText = isFrozen ? QStringLiteral("Unfreeze") : QStringLiteral("Freeze");
	}

	if (m_statusMushName)
		m_statusMushName->setText(mushName);
	if (m_statusTime)
		m_statusTime->setText(timeText);
	if (m_statusLines)
		m_statusLines->setText(linesText);
	if (m_statusLog)
		m_statusLog->setText(logText);
	if (m_statusCaps)
		m_statusCaps->setText(m_capsLockOn ? QStringLiteral("CAP") : QString());
	if (m_statusFreeze)
	{
		m_statusFreeze->setText(freezeText);
		m_statusFreeze->setStyleSheet(
		    QStringLiteral("border: 1px solid black; padding-left: 3px; padding-right: 3px;"));
	}
}

void MainWindow::requestDeferredUiRefresh(const bool refreshStatus, const bool refreshTabs,
                                          const bool refreshActivity)
{
	if (refreshStatus)
		m_deferredUiRefreshStatus = true;
	if (refreshTabs)
		m_deferredUiRefreshTabs = true;
	if (refreshActivity)
		m_deferredUiRefreshActivity = true;

	if (m_deferredUiRefreshQueued)
		return;
	m_deferredUiRefreshQueued = true;

	QMetaObject::invokeMethod(
	    this,
	    [this]
	    {
		    m_deferredUiRefreshQueued = false;

		    const bool doStatus         = m_deferredUiRefreshStatus;
		    const bool doTabs           = m_deferredUiRefreshTabs;
		    const bool doActivity       = m_deferredUiRefreshActivity;
		    m_deferredUiRefreshStatus   = false;
		    m_deferredUiRefreshTabs     = false;
		    m_deferredUiRefreshActivity = false;

		    if (doStatus)
			    updateStatusBar();
		    if (doTabs)
			    m_mdiTabs.updateTabs();
		    if (doActivity)
			    updateActivityToolbarButtons();
	    },
	    Qt::QueuedConnection);
}

void MainWindow::refreshActionState()
{
	const auto         *world   = activeWorldChildWindow();
	const WorldRuntime *runtime = world ? world->runtime() : nullptr;
	auto               *view    = world ? world->view() : nullptr;
	auto               *text    = activeTextChildWindow();

	const bool          hasWorld = runtime != nullptr;
	const bool          hasText  = text != nullptr;
	const bool          isFrozen = view && view->isFrozen();

	if (m_actions.contains(QStringLiteral("FreezeOutput")))
	{
		QAction *freezeAction = m_actions.value(QStringLiteral("FreezeOutput"));
		freezeAction->setCheckable(true);
		freezeAction->setChecked(isFrozen);
		freezeAction->setEnabled(view != nullptr);
	}

	// keep toolbar/menu states in sync with runtime
	updateConnectionActions(runtime);
	if (m_actions.contains(QStringLiteral("Preferences")))
		m_actions.value(QStringLiteral("Preferences"))->setEnabled(hasWorld);
	if (m_actions.contains(QStringLiteral("OpenWorldsInStartupList")))
	{
		bool enableStartup = false;
		if (const AppController *app = AppController::instance())
			enableStartup = !app->getGlobalOption(QStringLiteral("WorldList")).toString().isEmpty();
		m_actions.value(QStringLiteral("OpenWorldsInStartupList"))->setEnabled(enableStartup);
	}
	if (m_actions.contains(QStringLiteral("ConnectToWorldsInStartupList")))
	{
		bool enableStartup = false;
		if (const AppController *app = AppController::instance())
			enableStartup = !app->getGlobalOption(QStringLiteral("WorldList")).toString().isEmpty();
		m_actions.value(QStringLiteral("ConnectToWorldsInStartupList"))->setEnabled(enableStartup);
	}
	if (m_actions.contains(QStringLiteral("Import")))
		m_actions.value(QStringLiteral("Import"))->setEnabled(hasWorld);
	if (m_actions.contains(QStringLiteral("PluginWizard")))
		m_actions.value(QStringLiteral("PluginWizard"))->setEnabled(hasWorld);
	if (m_actions.contains(QStringLiteral("Save")))
		m_actions.value(QStringLiteral("Save"))->setEnabled(hasWorld || hasText);
	if (m_actions.contains(QStringLiteral("SaveAs")))
		m_actions.value(QStringLiteral("SaveAs"))->setEnabled(hasWorld || hasText);
	if (m_actions.contains(QStringLiteral("ConfigureTriggers")))
		m_actions.value(QStringLiteral("ConfigureTriggers"))->setEnabled(hasWorld);
	if (m_actions.contains(QStringLiteral("ConfigureAliases")))
		m_actions.value(QStringLiteral("ConfigureAliases"))->setEnabled(hasWorld);
	if (m_actions.contains(QStringLiteral("ConfigureTimers")))
		m_actions.value(QStringLiteral("ConfigureTimers"))->setEnabled(hasWorld);
	if (m_actions.contains(QStringLiteral("ConfigureOutput")))
		m_actions.value(QStringLiteral("ConfigureOutput"))->setEnabled(hasWorld);
	if (m_actions.contains(QStringLiteral("ConfigureCommands")))
		m_actions.value(QStringLiteral("ConfigureCommands"))->setEnabled(hasWorld);
	if (m_actions.contains(QStringLiteral("ConfigureScripting")))
		m_actions.value(QStringLiteral("ConfigureScripting"))->setEnabled(hasWorld);
	if (m_actions.contains(QStringLiteral("ConfigureNotes")))
		m_actions.value(QStringLiteral("ConfigureNotes"))->setEnabled(hasWorld);
	if (m_actions.contains(QStringLiteral("ConfigureVariables")))
		m_actions.value(QStringLiteral("ConfigureVariables"))->setEnabled(hasWorld);
	if (m_actions.contains(QStringLiteral("ResetAllTimers")))
		m_actions.value(QStringLiteral("ResetAllTimers"))->setEnabled(hasWorld);
	if (m_actions.contains(QStringLiteral("Immediate")))
	{
		const QString enableScripts =
		    runtime ? runtime->worldAttributes().value(QStringLiteral("enable_scripts")) : QString();
		const QString language =
		    runtime ? runtime->worldAttributes().value(QStringLiteral("script_language")) : QString();
		const bool enableImmediate =
		    hasWorld &&
		    (enableScripts == QStringLiteral("1") ||
		     enableScripts.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
		     enableScripts.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0) &&
		    language.compare(QStringLiteral("Lua"), Qt::CaseInsensitive) == 0;
		m_actions.value(QStringLiteral("Immediate"))->setEnabled(enableImmediate);
	}
	if (m_actions.contains(QStringLiteral("EditScriptFile")))
	{
		const QString scriptFile =
		    runtime ? runtime->worldAttributes().value(QStringLiteral("script_filename")).trimmed()
		            : QString();
		m_actions.value(QStringLiteral("EditScriptFile"))->setEnabled(hasWorld && !scriptFile.isEmpty());
	}
	if (m_actions.contains(QStringLiteral("ReloadScriptFile")))
	{
		QAction      *reloadAction = m_actions.value(QStringLiteral("ReloadScriptFile"));
		const QString enableScripts =
		    runtime ? runtime->worldAttributes().value(QStringLiteral("enable_scripts")) : QString();
		const QString language =
		    runtime ? runtime->worldAttributes().value(QStringLiteral("script_language")) : QString();
		const bool enableReload = hasWorld &&
		                          (enableScripts == QStringLiteral("1") ||
		                           enableScripts.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
		                           enableScripts.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0) &&
		                          language.compare(QStringLiteral("Lua"), Qt::CaseInsensitive) == 0;
		reloadAction->setEnabled(enableReload);
	}
	if (m_actions.contains(QStringLiteral("Trace")))
	{
		QAction *traceAction = m_actions.value(QStringLiteral("Trace"));
		traceAction->setEnabled(hasWorld);
		traceAction->setCheckable(true);
		traceAction->setChecked(runtime && runtime->traceEnabled());
	}
	if (m_actions.contains(QStringLiteral("AutoSay")))
	{
		const QString autoSayString =
		    runtime ? runtime->worldAttributes().value(QStringLiteral("auto_say_string")) : QString();
		const bool enableAutoSay = hasWorld && !autoSayString.isEmpty();
		QAction   *autoSayAction = m_actions.value(QStringLiteral("AutoSay"));
		autoSayAction->setEnabled(enableAutoSay);
		if (runtime)
		{
			const QString enabled   = runtime->worldAttributes().value(QStringLiteral("enable_auto_say"));
			const bool    isEnabled = enabled == QStringLiteral("1") ||
			                       enabled.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
			                       enabled.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
			autoSayAction->setCheckable(true);
			autoSayAction->setChecked(isEnabled);
		}
	}
	if (m_actions.contains(QStringLiteral("GameWrapLines")))
	{
		m_actions.value(QStringLiteral("GameWrapLines"))->setEnabled(hasWorld);
		if (runtime)
		{
			const int wrapColumn = runtime->worldAttributes().value(QStringLiteral("wrap_column")).toInt();
			m_actions.value(QStringLiteral("GameWrapLines"))->setCheckable(true);
			m_actions.value(QStringLiteral("GameWrapLines"))->setChecked(wrapColumn > 0);
		}
	}
	if (m_actions.contains(QStringLiteral("LogSession")))
	{
		const bool logOpen = runtime && runtime->isLogOpen();
		m_actions.value(QStringLiteral("LogSession"))->setCheckable(true);
		m_actions.value(QStringLiteral("LogSession"))->setChecked(logOpen);
		m_actions.value(QStringLiteral("LogSession"))->setEnabled(hasWorld);
	}
}

void MainWindow::updateMdiTabs()
{
	m_mdiTabs.updateTabs();
}

void MainWindow::onStatusFreezeDoubleClick()
{
	// Qt split-scrollback behavior: if split view is active, collapse back to
	// the normal output path and jump to current output.
	if (const auto *world = activeWorldChildWindow())
	{
		if (WorldView *view = world->view())
		{
			if (view->isScrollbackSplitActive())
			{
				view->collapseScrollbackSplitToLiveOutput();
				updateStatusBar();
				refreshActionState();
				return;
			}
		}
	}
	emit commandTriggered(QStringLiteral("FreezeOutput"));
}

void MainWindow::onStatusLinesDoubleClick()
{
	emit commandTriggered(QStringLiteral("GoToLine"));
}

void MainWindow::onStatusTimeDoubleClick()
{
	emit commandTriggered(QStringLiteral("ResetConnectedTime"));
}

void MainWindow::onStatusMushnameClick() const
{
	Q_UNUSED(this);
}

void MainWindow::updateActivityToolbarButtons()
{
	if (!m_activityToolbarWidget)
		return;

	const QVector<WorldWindowDescriptor> entries = worldWindowDescriptors();
	while (m_worldActions.size() < entries.size())
	{
		const int slot   = sizeToInt(m_worldActions.size()) + 1;
		auto     *action = new QAction(this);
		action->setObjectName(QMudMainFrameActionUtils::worldCommandNameForSlot(slot));
		action->setText(QString::number(slot));
		const QString tooltip = QMudMainFrameActionUtils::worldButtonTooltipForSlot(slot);
		action->setToolTip(tooltip);
		action->setStatusTip(tooltip);
		connect(action, &QAction::triggered, this, &MainWindow::onActionTriggered);
		m_actions.insert(action->objectName(), action);
		m_activityToolbarWidget->addAction(action);
		m_worldActions.push_back(action);
	}

	const WorldChildWindow *activeWorld = activeWorldChildWindow();
	for (int i = 0; i < m_worldActions.size(); ++i)
	{
		QAction *action = m_worldActions.at(i);
		if (!action)
			continue;
		const bool hasWorld = i < entries.size() && entries.at(i).window;
		action->setVisible(hasWorld);
		action->setEnabled(hasWorld);
		action->setCheckable(hasWorld);
		action->setChecked(hasWorld && entries.at(i).window == activeWorld);
		if (!hasWorld)
			action->setChecked(false);
	}
}

void MainWindow::activateWorldSlot(const int slot)
{
	if (slot < 1)
		return;

	const QVector<WorldWindowDescriptor> entries = worldWindowDescriptors();
	if (slot > entries.size())
		return;

	WorldChildWindow *world = entries.at(slot - 1).window;
	if (!world)
		return;
	if (m_mdiArea)
		m_mdiArea->setActiveSubWindow(world);
	world->show();
	world->raise();
}

QVector<WorldWindowDescriptor> MainWindow::worldWindowDescriptors() const
{
	QVector<WorldWindowDescriptor> entries;
	if (!m_mdiArea)
		return entries;

	QList<QMdiSubWindow *> windows = m_mdiTabs.orderedWindows();
	if (windows.isEmpty())
		windows = m_mdiArea->subWindowList(QMdiArea::CreationOrder);
	entries.reserve(windows.size());
	int sequence = 1;
	for (QMdiSubWindow *sub : windows)
	{
		auto *world = qobject_cast<WorldChildWindow *>(sub);
		if (!world)
			continue;

		WorldWindowDescriptor entry;
		entry.window   = world;
		entry.runtime  = world->runtime();
		entry.sequence = sequence++;

		entries.push_back(entry);
	}

	return entries;
}

void MainWindow::infoBarClear() const
{
	if (!m_infoText)
		return;
	m_infoText->setCurrentCharFormat(m_defaultInfoBarFormat);
	m_infoText->clear();
}

void MainWindow::infoBarAppend(const QString &text) const
{
	if (!m_infoText)
		return;
	QTextCursor cursor = m_infoText->textCursor();
	cursor.movePosition(QTextCursor::End);
	cursor.insertText(text);
	m_infoText->setTextCursor(cursor);
}

void MainWindow::infoBarSetFont(const QString &fontName, const int size, const int styleFlags) const
{
	if (!m_infoText)
		return;
	QTextCharFormat fmt = m_infoText->currentCharFormat();
	if (!fontName.isEmpty())
		fmt.setFontFamilies(QStringList{fontName});
	if (size > 0)
		fmt.setFontPointSize(size);
	fmt.setFontWeight(styleFlags & 1 ? QFont::Bold : QFont::Normal);
	fmt.setFontItalic(styleFlags & 2);
	fmt.setFontUnderline(styleFlags & 4);
	fmt.setFontStrikeOut(styleFlags & 8);
	m_infoText->setCurrentCharFormat(fmt);
}

void MainWindow::infoBarSetColour(const QColor &color) const
{
	if (!m_infoText || !color.isValid())
		return;
	QTextCharFormat fmt = m_infoText->currentCharFormat();
	fmt.setForeground(color);
	m_infoText->setCurrentCharFormat(fmt);
}

void MainWindow::infoBarSetBackground(const QColor &color) const
{
	if (!m_infoText || !color.isValid())
		return;
	QPalette pal = m_infoText->palette();
	pal.setColor(QPalette::Base, color);
	m_infoText->setPalette(pal);
}

static WorldRuntime *resolveRelatedRuntime(const MainWindow *frame, WorldRuntime *explicitRuntime);
static qulonglong    runtimeOwnerToken(WorldRuntime *runtime);

bool                 MainWindow::switchToNotepad()
{
	if (!m_mdiArea)
		return false;

	WorldRuntime    *owner      = resolveRelatedRuntime(this, nullptr);
	const qulonglong ownerToken = runtimeOwnerToken(owner);
	const QString    ownerWorldId =
        owner ? owner->worldAttributes().value(QStringLiteral("id")).trimmed() : QString();

	QList<TextChildWindow *> notepads;
	for (QMdiSubWindow *sub : m_mdiArea->subWindowList(QMdiArea::CreationOrder))
	{
		auto *text = qobject_cast<TextChildWindow *>(sub);
		if (!text)
			continue;

		if (ownerToken != 0)
		{
			const qulonglong relatedToken = text->property("worldRuntimeToken").toULongLong();
			if (relatedToken == ownerToken)
			{
				notepads.append(text);
				continue;
			}
			if (relatedToken != 0 && relatedToken != ownerToken)
				continue;
			if (!ownerWorldId.isEmpty())
			{
				if (const QString related = text->property("worldId").toString().trimmed();
				    !related.isEmpty() && related.compare(ownerWorldId, Qt::CaseInsensitive) != 0)
					continue;
			}
		}
		notepads.append(text);
	}

	if (notepads.isEmpty())
		return false;

	if (notepads.size() == 1)
	{
		m_mdiArea->setActiveSubWindow(notepads.front());
		notepads.front()->show();
		notepads.front()->raise();
		return true;
	}

	QStringList titles;
	for (const TextChildWindow *text : notepads)
		titles.append(text->windowTitle());

	bool          ok = false;
	const QString choice =
	    QInputDialog::getItem(this, QStringLiteral("Choose Notepad"),
	                          QStringLiteral("Choose a notepad to activate:"), titles, 0, false, &ok);
	if (!ok || choice.isEmpty())
		return false;

	return activateNotepad(choice);
}

bool MainWindow::activateNotepad(const QString &title) const
{
	return activateNotepad(title, nullptr);
}

bool MainWindow::activateNotepad(const QString &title, WorldRuntime *relatedRuntime) const
{
	if (!m_mdiArea)
		return false;

	WorldRuntime    *owner      = resolveRelatedRuntime(this, relatedRuntime);
	const qulonglong ownerToken = runtimeOwnerToken(owner);
	const QString    ownerWorldId =
        owner ? owner->worldAttributes().value(QStringLiteral("id")).trimmed() : QString();
	for (QMdiSubWindow *sub : m_mdiArea->subWindowList(QMdiArea::CreationOrder))
	{
		auto *text = qobject_cast<TextChildWindow *>(sub);
		if (!text)
			continue;
		if (text->windowTitle().compare(title, Qt::CaseInsensitive) != 0)
			continue;

		if (ownerToken != 0)
		{
			const qulonglong relatedToken = text->property("worldRuntimeToken").toULongLong();
			if (relatedToken == ownerToken)
			{
				m_mdiArea->setActiveSubWindow(text);
				text->show();
				text->raise();
				return true;
			}
			if (relatedToken != 0 && relatedToken != ownerToken)
				continue;
			if (!ownerWorldId.isEmpty())
			{
				if (const QString related = text->property("worldId").toString().trimmed();
				    !related.isEmpty() && related.compare(ownerWorldId, Qt::CaseInsensitive) != 0)
					continue;
			}
		}
		m_mdiArea->setActiveSubWindow(text);
		text->show();
		text->raise();
		return true;
	}

	return false;
}

static WorldRuntime *resolveRelatedRuntime(const MainWindow *frame, WorldRuntime *explicitRuntime)
{
	if (explicitRuntime)
		return explicitRuntime;
	if (!frame)
		return nullptr;
	const auto *worldChild = frame->activeWorldChildWindow();
	if (!worldChild)
		return nullptr;
	return worldChild->runtime();
}

static qulonglong runtimeOwnerToken(WorldRuntime *runtime)
{
	return runtime ? reinterpret_cast<quintptr>(runtime) : 0;
}

static void assignNotepadOwner(TextChildWindow *notepad, WorldRuntime *runtime)
{
	if (!notepad || !runtime)
		return;
	notepad->setProperty("worldRuntimeToken", QVariant::fromValue(runtimeOwnerToken(runtime)));
	if (const QString worldId = runtime->worldAttributes().value(QStringLiteral("id")).trimmed();
	    !worldId.isEmpty())
		notepad->setProperty("worldId", worldId);
}

bool MainWindow::appendToNotepad(const QString &title, const QString &text, const bool replace,
                                 WorldRuntime *relatedRuntime)
{
	if (title.isEmpty())
		return false;

	if (!m_mdiArea)
		return false;

	WorldRuntime    *owner      = resolveRelatedRuntime(this, relatedRuntime);
	const qulonglong ownerToken = runtimeOwnerToken(owner);
	const QString    ownerWorldId =
        owner ? owner->worldAttributes().value(QStringLiteral("id")).trimmed() : QString();
	TextChildWindow *target       = nullptr;
	TextChildWindow *unnamedMatch = nullptr;
	for (QMdiSubWindow *sub : m_mdiArea->subWindowList(QMdiArea::CreationOrder))
	{
		auto *textWindow = qobject_cast<TextChildWindow *>(sub);
		if (!textWindow)
			continue;
		if (textWindow->windowTitle().compare(title, Qt::CaseInsensitive) == 0)
		{
			const qulonglong relatedToken = textWindow->property("worldRuntimeToken").toULongLong();
			if (ownerToken != 0 && relatedToken == ownerToken)
			{
				target = textWindow;
				break;
			}
			if (ownerToken != 0 && relatedToken != 0 && relatedToken != ownerToken)
				continue;
			if (ownerWorldId.isEmpty())
			{
				target = textWindow;
				break;
			}
			const QString related = textWindow->property("worldId").toString().trimmed();
			if (related.compare(ownerWorldId, Qt::CaseInsensitive) == 0)
			{
				target = textWindow;
				break;
			}
			if (related.isEmpty() && !unnamedMatch)
				unnamedMatch = textWindow;
		}
	}
	if (!target && unnamedMatch)
		target = unnamedMatch;

	if (!target)
	{
		target = new TextChildWindow(title, text);
		assignNotepadOwner(target, owner);
		addMdiSubWindow(target);
		return true;
	}

	assignNotepadOwner(target, owner);
	if (replace)
		target->setText(text);
	else
		target->appendText(text);
	return true;
}

bool MainWindow::sendToNotepad(const QString &title, const QString &text, WorldRuntime *relatedRuntime)
{
	if (title.isEmpty())
		return false;
	WorldRuntime *owner  = resolveRelatedRuntime(this, relatedRuntime);
	auto         *target = new TextChildWindow(title, text);
	assignNotepadOwner(target, owner);
	addMdiSubWindow(target);
	return true;
}

void MainWindow::setToolbarVisible(const bool visible) const
{
	if (m_mainToolbarWidget)
		m_mainToolbarWidget->setVisible(visible);
	if (QAction *action = actionForCommand(QStringLiteral("ViewToolbar")))
	{
		QSignalBlocker blocker(action);
		action->setChecked(visible);
	}
}

void MainWindow::setWorldToolbarVisible(const bool visible) const
{
	if (m_worldToolbarWidget)
		m_worldToolbarWidget->setVisible(visible);
	if (QAction *action = actionForCommand(QStringLiteral("ViewWorldToolbar")))
	{
		QSignalBlocker blocker(action);
		action->setChecked(visible);
	}
}

void MainWindow::setActivityToolbarVisible(const bool visible) const
{
	if (m_activityToolbarWidget)
		m_activityToolbarWidget->setVisible(visible);
	if (QAction *action = actionForCommand(QStringLiteral("ActivityToolbar")))
	{
		QSignalBlocker blocker(action);
		action->setChecked(visible);
	}
}

void MainWindow::setWindowTabsStyle(const int style)
{
	m_windowTabsStyle = style;
	if (!m_centralLayout || !m_mdiArea)
		return;

	if (style == 0)
	{
		m_mdiTabs.hide();
		return;
	}

	m_mdiTabs.show();
	m_centralLayout->removeWidget(&m_mdiTabs);
	m_centralLayout->removeWidget(m_mdiArea);
	if (style == 2)
	{
		m_centralLayout->addWidget(m_mdiArea);
		m_centralLayout->addWidget(&m_mdiTabs);
	}
	else
	{
		m_centralLayout->addWidget(&m_mdiTabs);
		m_centralLayout->addWidget(m_mdiArea);
	}
}

void MainWindow::setActivityToolbarStyle(const int style)
{
	m_activityToolbarStyle = style;
	updateActivityToolbarButtons();
}

void MainWindow::setTimerInterval(int seconds)
{
	if (seconds <= 0)
		seconds = 0;

	if (!m_activityTimer)
	{
		m_activityTimer = new QTimer(this);
		connect(m_activityTimer, &QTimer::timeout, this, &MainWindow::processTimers);
	}
	if (!m_tickTimer)
	{
		m_tickTimer = new QTimer(this);
		connect(m_tickTimer, &QTimer::timeout, this, &MainWindow::processPluginTicks);
	}

	if (seconds == 0)
		m_activityTimer->start(100);
	else
		m_activityTimer->start(seconds * 1000);

	m_tickTimer->start(40);
}

void MainWindow::setActivityRefresh(const int mode, const int interval)
{
	m_activityRefreshType     = mode;
	m_activityRefreshInterval = interval;
}

void MainWindow::setStatusbarVisible(const bool visible) const
{
	if (statusBar())
		statusBar()->setVisible(visible);
	if (QAction *action = actionForCommand(QStringLiteral("ViewStatusbar")))
	{
		QSignalBlocker blocker(action);
		action->setChecked(visible);
	}
}

void MainWindow::setInfoBarVisible(const bool visible) const
{
	if (m_infoDock)
		m_infoDock->setVisible(visible);
	if (QAction *action = actionForCommand(QStringLiteral("ViewInfoBar")))
	{
		QSignalBlocker blocker(action);
		action->setChecked(visible);
	}
}

bool MainWindow::event(QEvent *event)
{
	if (event && event->type() == QEvent::StatusTip)
	{
		if (m_hyperlinkStatusLocked)
			return true;

		const auto   *statusEvent = dynamic_cast<QStatusTipEvent *>(event);
		const QString tip         = statusEvent ? statusEvent->tip().trimmed() : QString();
		if (!tip.isEmpty())
		{
			if (m_statusMessage)
				m_statusMessage->setText(tip);
			if (m_statusMessageTimer)
				m_statusMessageTimer->stop();
			m_statusTipOwnsMessage = true;
		}
		else if (m_statusTipOwnsMessage)
		{
			if (m_statusMessage)
				m_statusMessage->setText(QStringLiteral("Ready"));
			if (m_statusMessageTimer)
				m_statusMessageTimer->stop();
			m_statusTipOwnsMessage = false;
		}
		return true;
	}

	if (event && m_disableKeyboardMenuActivation &&
	    (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease))
	{
		if (const auto *keyEvent = dynamic_cast<QKeyEvent *>(event);
		    keyEvent && keyEvent->key() == Qt::Key_Alt && keyEvent->modifiers() == Qt::AltModifier)
			return true;
	}

	return QMainWindow::event(event);
}

void MainWindow::changeEvent(QEvent *event)
{
	QMainWindow::changeEvent(event);
	if (!event || event->type() != QEvent::WindowStateChange)
		return;

	if ((windowState() & (Qt::WindowMaximized | Qt::WindowMinimized | Qt::WindowFullScreen)) == 0)
		m_lastNormalGeometry = geometry();

	refreshWorldMiniWindows();
	QMetaObject::invokeMethod(this, [this] { refreshWorldMiniWindows(); }, Qt::QueuedConnection);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
	if (!event)
		return QMainWindow::eventFilter(watched, event);

	if (event->type() == QEvent::KeyPress)
	{
		if (const auto *keyEvent = dynamic_cast<QKeyEvent *>(event); keyEvent)
		{
			if (!keyEvent->isAutoRepeat() && keyEvent->key() == Qt::Key_CapsLock)
			{
				m_capsLockOn = !m_capsLockOn;
				if (m_statusCaps)
					m_statusCaps->setText(m_capsLockOn ? QStringLiteral("CAP") : QString());
			}
			else if (!keyEvent->isAutoRepeat())
			{
				// Cross-platform lock-state sync: if CapsLock was toggled outside app focus,
				// infer current lock state from actual typed alphabetic characters.
				if (const QString text = keyEvent->text(); text.size() == 1)
				{
					if (const QChar ch = text.at(0); ch.isLetter())
					{
						const bool shift     = keyEvent->modifiers().testFlag(Qt::ShiftModifier);
						const bool uppercase = ch.isUpper();
						m_capsLockOn         = uppercase != shift;
						if (m_statusCaps)
							m_statusCaps->setText(m_capsLockOn ? QStringLiteral("CAP") : QString());
					}
				}
			}
		}
		else
			return QMainWindow::eventFilter(watched, event);
	}

	if ((watched == m_mdiArea || watched == m_centralContainer || watched == m_centralLayout ||
	     watched == this) &&
	    (event->type() == QEvent::Resize || event->type() == QEvent::WindowStateChange))
	{
		refreshWorldMiniWindows();
	}

	if (event->type() == QEvent::Close)
	{
		if (auto *closingSubWindow = qobject_cast<QMdiSubWindow *>(watched);
		    closingSubWindow && m_mdiArea && m_mdiArea->activeSubWindow() == closingSubWindow)
		{
			const QList<QMdiSubWindow *> windows = m_mdiArea->subWindowList(QMdiArea::CreationOrder);
			m_tabActivationHistory.onCloseEvent(closingSubWindow, m_mdiArea->activeSubWindow(), windows);
		}
	}

	if (event->type() != QEvent::KeyPress && event->type() != QEvent::ShortcutOverride)
		return QMainWindow::eventFilter(watched, event);

	auto *keyEvent = dynamic_cast<QKeyEvent *>(event);
	if (!keyEvent)
		return QMainWindow::eventFilter(watched, event);
	const Qt::KeyboardModifiers mods = keyEvent->modifiers();
	const bool hasOnlyAlt = mods.testFlag(Qt::AltModifier) && !mods.testFlag(Qt::ControlModifier) &&
	                        !mods.testFlag(Qt::ShiftModifier) && !mods.testFlag(Qt::MetaModifier);
	const bool hasOnlyCtrl = mods.testFlag(Qt::ControlModifier) && !mods.testFlag(Qt::AltModifier) &&
	                         !mods.testFlag(Qt::ShiftModifier) && !mods.testFlag(Qt::MetaModifier);
	const bool hasOnlyShift = mods.testFlag(Qt::ShiftModifier) && !mods.testFlag(Qt::ControlModifier) &&
	                          !mods.testFlag(Qt::AltModifier) && !mods.testFlag(Qt::MetaModifier);
	const bool altEnter =
	    hasOnlyAlt && (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter);
	const bool ctrlG      = hasOnlyCtrl && keyEvent->key() == Qt::Key_G;
	const bool shiftLeft  = hasOnlyShift && keyEvent->key() == Qt::Key_Left;
	const bool shiftRight = hasOnlyShift && keyEvent->key() == Qt::Key_Right;

	if (!altEnter && !ctrlG && !shiftLeft && !shiftRight)
		return QMainWindow::eventFilter(watched, event);

	if (event->type() == QEvent::ShortcutOverride)
	{
		if (shiftLeft || shiftRight)
		{
			if (QApplication::activeModalWidget())
				return QMainWindow::eventFilter(watched, event);
			if (!m_mdiArea)
				return QMainWindow::eventFilter(watched, event);
			m_mdiTabs.updateTabs();
			if (m_mdiTabs.count() < 2)
				return QMainWindow::eventFilter(watched, event);
		}
		keyEvent->accept();
		return true;
	}

	if (QApplication::activeModalWidget())
	{
		if (shiftLeft || shiftRight)
			return QMainWindow::eventFilter(watched, event);
		keyEvent->accept();
		return true;
	}

	if (keyEvent->isAutoRepeat())
	{
		keyEvent->accept();
		return true;
	}

	if ((altEnter || ctrlG) && triggerWorldPreferencesFromShortcut())
	{
		keyEvent->accept();
		return true;
	}

	if ((shiftLeft || shiftRight) && triggerAdjacentMdiTabFromShortcut(shiftRight ? 1 : -1))
	{
		keyEvent->accept();
		return true;
	}
	return QMainWindow::eventFilter(watched, event);
}

void MainWindow::refreshWorldMiniWindows() const
{
	if (!m_mdiArea)
		return;

	auto refreshWorld = [](const WorldChildWindow *world)
	{
		if (!world)
			return;
		if (WorldView *view = world->view())
			view->refreshMiniWindows();
	};

	if (auto *activeWorld = qobject_cast<WorldChildWindow *>(m_mdiArea->activeSubWindow());
	    activeWorld && activeWorld->windowState().testFlag(Qt::WindowMaximized))
	{
		refreshWorld(activeWorld);
		return;
	}

	for (QMdiSubWindow *sub : m_mdiArea->subWindowList(QMdiArea::CreationOrder))
	{
		auto *world = qobject_cast<WorldChildWindow *>(sub);
		if (!world)
			continue;
		if (!world->isVisible() || world->windowState().testFlag(Qt::WindowMinimized))
			continue;
		refreshWorld(world);
	}
}

bool MainWindow::triggerWorldPreferencesFromShortcut()
{
	if (QApplication::activeModalWidget())
		return false;
	if (!m_mdiArea || !qobject_cast<WorldChildWindow *>(m_mdiArea->activeSubWindow()))
		return false;

	if (QAction *action = actionForCommand(QStringLiteral("Preferences")))
	{
		if (!action->isEnabled())
			return false;
		action->trigger();
		return true;
	}

	emit commandTriggered(QStringLiteral("Preferences"));
	return true;
}

bool MainWindow::triggerAdjacentMdiTabFromShortcut(const int step)
{
	if (!m_mdiArea || step == 0)
		return false;

	m_mdiTabs.updateTabs();

	const int tabCount = m_mdiTabs.count();
	if (tabCount < 2)
		return false;

	int currentIndex = m_mdiTabs.currentIndex();
	if (currentIndex < 0 || currentIndex >= tabCount)
		currentIndex = 0;

	const int normalizedStep = step > 0 ? 1 : -1;
	const int nextIndex      = (currentIndex + normalizedStep + tabCount) % tabCount;
	if (nextIndex == currentIndex)
		return false;

	m_mdiTabs.setCurrentIndex(nextIndex);
	if (QMdiSubWindow *target = m_mdiArea->activeSubWindow())
	{
		target->show();
		target->setFocus();
	}
	return true;
}

void MainWindow::setFrameBackgroundColour(const long colour)
{
	m_backgroundColour = static_cast<unsigned int>(colour);
	if (!m_mdiArea)
		return;

	if (static_cast<unsigned int>(colour) == 0xFFFFFFFFu)
	{
		const QPalette pal = m_mdiArea->palette();
		m_mdiArea->setBackground(pal.brush(QPalette::Window));
	}
	else
	{
		const int r = static_cast<int>(colour & 0xFF);
		const int g = static_cast<int>(colour >> 8 & 0xFF);
		const int b = static_cast<int>(colour >> 16 & 0xFF);
		m_mdiArea->setBackground(QBrush(QColor(r, g, b)));
	}

	if (QWidget *viewport = m_mdiArea->viewport())
		viewport->update();
}
