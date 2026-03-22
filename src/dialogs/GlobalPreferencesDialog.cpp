/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: GlobalPreferencesDialog.cpp
 * Role: Global preferences dialog behavior for editing and persisting application-level settings.
 */

#include "dialogs/GlobalPreferencesDialog.h"

#include "AppController.h"
#include "LogCompressionUtils.h"
#include "MainFrame.h"
#include "WorldChildWindow.h"
#include "WorldRuntime.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMetaObject>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QTextEdit>
#include <QThreadPool>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

GlobalPreferencesDialog::GlobalPreferencesDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QStringLiteral("Global Preferences"));

	auto *root    = new QVBoxLayout(this);
	auto *tabRows = new QVBoxLayout;
	tabRows->setContentsMargins(0, 0, 0, 0);
	tabRows->setSpacing(6);
	m_tabRowOne = new QTabBar(this);
	m_tabRowTwo = new QTabBar(this);
	m_tabRowOne->setShape(QTabBar::RoundedNorth);
	m_tabRowTwo->setShape(QTabBar::RoundedNorth);
	for (QTabBar *row : {m_tabRowOne, m_tabRowTwo})
	{
		row->setExpanding(true);
		row->setElideMode(Qt::ElideNone);
		row->setUsesScrollButtons(false);
		row->setDrawBase(false);
		row->setDocumentMode(false);
		row->setFocusPolicy(Qt::NoFocus);
		row->setMinimumWidth(0);
		row->setStyleSheet(QStringLiteral("QTabBar::tab {"
		                                  "padding: 5px 12px;"
		                                  "min-height: 24px;"
		                                  "margin: 0px 2px 0px 0px;"
		                                  "top: 0px;"
		                                  "border-top-left-radius: 6px;"
		                                  "border-top-right-radius: 6px;"
		                                  "border-left: 1px solid palette(mid);"
		                                  "border-right: 1px solid palette(mid);"
		                                  "border-top: 1px solid palette(mid);"
		                                  "border-bottom: 0px;"
		                                  "background: palette(button);"
		                                  "color: palette(buttonText);"
		                                  "font-weight: normal;"
		                                  "text-decoration: none;"
		                                  "outline: none;"
		                                  "}"
		                                  "QTabBar::tab:hover {"
		                                  "background: palette(light);"
		                                  "text-decoration: none;"
		                                  "}"
		                                  "QTabBar[activeRow=\"true\"]::tab:selected {"
		                                  "background: palette(base);"
		                                  "color: palette(windowText);"
		                                  "font-weight: 600;"
		                                  "margin: 0px 2px 0px 0px;"
		                                  "top: 0px;"
		                                  "border-top: 1px solid palette(highlight);"
		                                  "border-left: 1px solid palette(highlight);"
		                                  "border-right: 1px solid palette(highlight);"
		                                  "border-bottom: 0px;"
		                                  "text-decoration: none;"
		                                  "outline: none;"
		                                  "}"
		                                  "QTabBar[activeRow=\"true\"]::tab:!selected {"
		                                  "background: palette(button);"
		                                  "color: palette(buttonText);"
		                                  "font-weight: normal;"
		                                  "margin: 0px 2px 0px 0px;"
		                                  "top: 0px;"
		                                  "border-top: 1px solid palette(mid);"
		                                  "border-bottom: 0px;"
		                                  "text-decoration: none;"
		                                  "outline: none;"
		                                  "}"
		                                  "QTabBar[activeRow=\"false\"]::tab:selected {"
		                                  "background: palette(button);"
		                                  "color: palette(buttonText);"
		                                  "font-weight: normal;"
		                                  "margin: 0px 2px 0px 0px;"
		                                  "top: 0px;"
		                                  "border-top: 1px solid palette(mid);"
		                                  "border-left: 1px solid palette(mid);"
		                                  "border-right: 1px solid palette(mid);"
		                                  "border-bottom: 0px;"
		                                  "text-decoration: none;"
		                                  "outline: none;"
		                                  "}"));
	}
	tabRows->addWidget(m_tabRowOne);
	tabRows->addWidget(m_tabRowTwo);
	root->addLayout(tabRows);

	m_tabs = new QTabWidget(this);
	m_tabs->setUsesScrollButtons(false);
	m_tabs->tabBar()->setExpanding(false);
	m_tabs->tabBar()->setElideMode(Qt::ElideNone);
	m_tabs->tabBar()->hide();
	m_tabs->addTab(buildWorldsPage(), QStringLiteral("Worlds"));
	m_tabs->addTab(buildGeneralPage(), QStringLiteral("General"));
	m_tabs->addTab(buildClosingPage(), QStringLiteral("Closing"));
	m_tabs->addTab(buildPrintingPage(), QStringLiteral("Printing"));
	m_tabs->addTab(buildLoggingPage(), QStringLiteral("Logging"));
	m_tabs->addTab(buildTimersPage(), QStringLiteral("Timers"));
	m_tabs->addTab(buildActivityPage(), QStringLiteral("Activity"));
	m_tabs->addTab(buildDefaultsPage(), QStringLiteral("Defaults"));
	m_tabs->addTab(buildNotepadPage(), QStringLiteral("Notepad"));
	m_tabs->addTab(buildTrayPage(), QStringLiteral("Tray/Taskbar"));
	m_tabs->addTab(buildPluginsPage(), QStringLiteral("Plugins"));
	m_tabs->addTab(buildLuaPage(), QStringLiteral("Lua"));
	m_tabs->addTab(buildUpdatesPage(), QStringLiteral("Updates"));
	rebuildExternalTabRows();
	root->addWidget(m_tabs);

	connect(m_tabs, &QTabWidget::currentChanged, this, &GlobalPreferencesDialog::syncExternalTabSelection);
	connect(m_tabRowOne, &QTabBar::tabBarClicked, this,
	        [this](const int rowIndex)
	        {
		        if (rowIndex < 0 || rowIndex >= m_tabRowOneToPage.size() || !m_tabs)
			        return;
		        m_tabs->setCurrentIndex(m_tabRowOneToPage.at(rowIndex));
	        });
	connect(m_tabRowTwo, &QTabBar::tabBarClicked, this,
	        [this](const int rowIndex)
	        {
		        if (rowIndex < 0 || rowIndex >= m_tabRowTwoToPage.size() || !m_tabs)
			        return;
		        m_tabs->setCurrentIndex(m_tabRowTwoToPage.at(rowIndex));
	        });

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	root->addWidget(buttons);

	setFixedSize(650, 600);

	loadPreferences();

	if (AppController *app = AppController::instance(); app)
	{
		QSettings settings(app->iniFilePath(), QSettings::IniFormat);
		settings.beginGroup(QStringLiteral("GlobalPreferencesDialog"));
		const QByteArray geometry = settings.value(QStringLiteral("Geometry")).toByteArray();
		if (!geometry.isEmpty())
			restoreGeometry(geometry);
		const int tabIndex = settings.value(QStringLiteral("TabIndex"), 0).toInt();
		if (m_tabs && tabIndex >= 0 && tabIndex < m_tabs->count())
			m_tabs->setCurrentIndex(tabIndex);
		syncExternalTabSelection(m_tabs ? m_tabs->currentIndex() : -1);
		settings.endGroup();
		connect(this, &QDialog::finished, this,
		        [this, app](int)
		        {
			        QSettings settings(app->iniFilePath(), QSettings::IniFormat);
			        settings.beginGroup(QStringLiteral("GlobalPreferencesDialog"));
			        settings.setValue(QStringLiteral("Geometry"), saveGeometry());
			        if (m_tabs)
				        settings.setValue(QStringLiteral("TabIndex"), m_tabs->currentIndex());
			        settings.endGroup();
		        });
	}
}

namespace
{
	bool isFileOpenForWriteByAnotherProcess(const QString &path)
	{
		if (path.trimmed().isEmpty())
			return false;
#ifdef Q_OS_WIN
		const QString normalized = QDir::toNativeSeparators(QDir::cleanPath(path));
		const auto    nativePath = reinterpret_cast<LPCWSTR>(normalized.utf16());
		HANDLE        handle = CreateFileW(nativePath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
		                                   FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
		{
			const DWORD error = GetLastError();
			return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION ||
			       error == ERROR_ACCESS_DENIED;
		}
		CloseHandle(handle);
		return false;
#else
		const QByteArray nativePath = QFile::encodeName(path);
		const int        descriptor = ::open(nativePath.constData(), O_RDWR);
		if (descriptor < 0)
			return false;

		struct flock lock{};
		lock.l_type   = F_WRLCK;
		lock.l_whence = SEEK_SET;
		lock.l_start  = 0;
		lock.l_len    = 0;

		const int lockResult = fcntl(descriptor, F_SETLK, &lock);
		if (lockResult == -1 && (errno == EACCES || errno == EAGAIN))
		{
			::close(descriptor);
			return true;
		}

		if (lockResult == 0)
		{
			lock.l_type = F_UNLCK;
			(void)fcntl(descriptor, F_SETLK, &lock);
		}
		::close(descriptor);
		return false;
#endif
	}

	QString storagePath(const QString &value)
	{
		QString out = value.trimmed();
		out.replace(QLatin1Char('\\'), QLatin1Char('/'));
		return out;
	}

	QString runtimePath(const QString &value)
	{
		const QString stored = storagePath(value);
#ifdef Q_OS_WIN
		return QDir::toNativeSeparators(stored);
#else
		return stored;
#endif
	}

	QStringList canonicalPathList(const QStringList &paths)
	{
		QStringList out;
		out.reserve(paths.size());
		for (const QString &path : paths)
			out.push_back(runtimePath(path));
		return out;
	}

	void addUniqueItemsToList(QListWidget *list, const QStringList &paths)
	{
		if (!list)
			return;
		QSet<QString> existing;
		existing.reserve(list->count());
		for (int i = 0; i < list->count(); ++i)
		{
			if (QListWidgetItem *item = list->item(i))
				existing.insert(storagePath(item->text()));
		}
		for (const QString &path : paths)
		{
			const QString normalized        = runtimePath(path);
			const QString normalizedStorage = storagePath(normalized);
			if (normalized.isEmpty() || existing.contains(normalizedStorage))
				continue;
			list->addItem(normalized);
			existing.insert(normalizedStorage);
		}
		if (list->count() > 0 && list->currentRow() < 0)
			list->setCurrentRow(0);
	}

	QString withTrailingSlash(QString path)
	{
		path = storagePath(path);
		if (path.isEmpty())
			return path;
		if (!path.endsWith(QLatin1Char('/')))
			path += QLatin1Char('/');
		return path;
	}

	QString fontStyleSummary(const int pointSize, const int weight, const bool italic)
	{
		QStringList parts;
		if (pointSize > 0)
			parts << QStringLiteral("%1 pt.").arg(pointSize);
		if (weight >= 700)
			parts << QStringLiteral("Bold");
		else if (weight >= 500)
			parts << QStringLiteral("Medium");
		else
			parts << QStringLiteral("Regular");
		if (italic)
			parts << QStringLiteral("Italic");
		return parts.join(QLatin1Char(' '));
	}

	void updateTabRowVisualState(QTabBar *row, const bool active)
	{
		if (!row)
			return;
		row->setProperty("activeRow", active);
		if (QStyle *style = row->style(); style)
		{
			style->unpolish(row);
			style->polish(row);
		}
		row->update();
	}
} // namespace

void GlobalPreferencesDialog::accept()
{
	if (!applyPreferences())
		return;
	QDialog::accept();
}

QPushButton *GlobalPreferencesDialog::makeSwatchButton()
{
	auto *button = new QPushButton;
	button->setFixedSize(56, 56);
	button->setFlat(true);
	button->setStyleSheet(QStringLiteral("background-color: #000000; border: 1px solid #666666;"));
	return button;
}

QWidget *GlobalPreferencesDialog::buildWorldsPage()
{
	auto *page   = new QWidget;
	auto *layout = new QVBoxLayout(page);

	auto *header      = new QHBoxLayout;
	auto *title       = new QLabel(QStringLiteral("Worlds to open at startup ..."));
	m_worldCountLabel = new QLabel(QStringLiteral("0 worlds"));
	m_worldCountLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	header->addWidget(title);
	header->addStretch();
	header->addWidget(m_worldCountLabel);
	layout->addLayout(header);

	m_worldList = new QListWidget;
	m_worldList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	layout->addWidget(m_worldList);

	m_worldSelected = new QLabel(QStringLiteral("Selected world"));
	m_worldSelected->setWordWrap(true);
	layout->addWidget(m_worldSelected);

	auto *buttons     = new QHBoxLayout;
	auto *addWorld    = new QPushButton(QStringLiteral("Add..."));
	auto *removeWorld = new QPushButton(QStringLiteral("Remove"));
	auto *moveUp      = new QPushButton(QStringLiteral("Move Up"));
	auto *moveDown    = new QPushButton(QStringLiteral("Move Down"));
	buttons->addWidget(addWorld);
	buttons->addWidget(removeWorld);
	buttons->addWidget(moveUp);
	buttons->addWidget(moveDown);
	layout->addLayout(buttons);

	auto *bottomButtons    = new QHBoxLayout;
	auto *defaultDirButton = new QPushButton(QStringLiteral("Default World Files Directory..."));
	bottomButtons->addWidget(defaultDirButton);
	bottomButtons->addStretch();
	auto *addCurrent = new QPushButton(QStringLiteral("Add Current World"));
	bottomButtons->addWidget(addCurrent);
	layout->addLayout(bottomButtons);

	m_worldDefaultDir = new QLabel(QStringLiteral("Default directory"));
	layout->addWidget(m_worldDefaultDir);

	QObject::connect(m_worldList, &QListWidget::currentTextChanged, page,
	                 [this](const QString &) { syncWorldListSelection(); });
	const auto updateWorldButtons = [this, removeWorld, moveUp, moveDown]()
	{
		if (!m_worldList)
			return;
		const int row   = m_worldList->currentRow();
		const int count = m_worldList->count();
		removeWorld->setEnabled(row >= 0);
		moveUp->setEnabled(row > 0);
		moveDown->setEnabled(row >= 0 && row < (count - 1));
		syncWorldListSelection();
	};
	QObject::connect(addWorld, &QPushButton::clicked, page,
	                 [this]()
	                 {
		                 AppController *app = AppController::instance();
		                 if (app)
			                 app->changeToFileBrowsingDirectory();
		                 const QString startDir  = m_worldDefaultDir ? m_worldDefaultDir->text() : QString();
		                 const QStringList paths = QFileDialog::getOpenFileNames(
		                     this, QStringLiteral("Select World File"), startDir,
		                     QStringLiteral("World files (*.qdl *.mcl)"));
		                 if (app)
			                 app->changeToStartupDirectory();
		                 if (paths.isEmpty())
			                 return;
		                 addUniqueItemsToList(m_worldList, canonicalPathList(paths));
		                 m_worldList->setCurrentRow(m_worldList->count() - 1);
	                 });
	QObject::connect(removeWorld, &QPushButton::clicked, page,
	                 [this, updateWorldButtons]()
	                 {
		                 delete m_worldList->currentItem();
		                 updateWorldButtons();
	                 });
	QObject::connect(moveUp, &QPushButton::clicked, page,
	                 [this, updateWorldButtons]()
	                 {
		                 const int row = m_worldList->currentRow();
		                 if (row > 0)
		                 {
			                 QListWidgetItem *item = m_worldList->takeItem(row);
			                 m_worldList->insertItem(row - 1, item);
			                 m_worldList->setCurrentRow(row - 1);
		                 }
		                 updateWorldButtons();
	                 });
	QObject::connect(moveDown, &QPushButton::clicked, page,
	                 [this, updateWorldButtons]()
	                 {
		                 const int row = m_worldList->currentRow();
		                 if (row >= 0 && row < m_worldList->count() - 1)
		                 {
			                 QListWidgetItem *item = m_worldList->takeItem(row);
			                 m_worldList->insertItem(row + 1, item);
			                 m_worldList->setCurrentRow(row + 1);
		                 }
		                 updateWorldButtons();
	                 });
	QObject::connect(defaultDirButton, &QPushButton::clicked, page,
	                 [this]()
	                 {
		                 AppController *app = AppController::instance();
		                 if (app)
			                 app->changeToFileBrowsingDirectory();
		                 const QString startDir = m_worldDefaultDir ? m_worldDefaultDir->text() : QString();
		                 const QString dir      = QFileDialog::getExistingDirectory(
                             this, QStringLiteral("Select Default World Files Directory"), startDir);
		                 if (app)
			                 app->changeToStartupDirectory();
		                 if (!dir.isEmpty())
			                 m_worldDefaultDir->setText(runtimePath(dir));
	                 });
	QObject::connect(addCurrent, &QPushButton::clicked, page,
	                 [this, updateWorldButtons]()
	                 {
		                 AppController *app = AppController::instance();
		                 if (!app)
			                 return;
		                 auto *frame = static_cast<MainWindow *>(app->mainWindow());
		                 if (!frame)
			                 return;
		                 WorldChildWindow *world = frame->activeWorldChildWindow();
		                 if (!world)
			                 return;
		                 WorldRuntime *runtime = world->runtime();
		                 if (!runtime)
			                 return;
		                 const QString path = runtimePath(runtime->worldFilePath());
		                 if (path.isEmpty())
			                 return;
		                 const QList<QListWidgetItem *> existing =
		                     m_worldList->findItems(path, Qt::MatchExactly);
		                 if (!existing.isEmpty())
		                 {
			                 m_worldList->setCurrentItem(existing.front());
			                 updateWorldButtons();
			                 return;
		                 }
		                 m_worldList->addItem(path);
		                 m_worldList->setCurrentRow(m_worldList->count() - 1);
		                 updateWorldButtons();
	                 });
	QObject::connect(m_worldList, &QListWidget::currentRowChanged, page,
	                 [updateWorldButtons](int) { updateWorldButtons(); });
	updateWorldButtons();

	return page;
}

QWidget *GlobalPreferencesDialog::buildGeneralPage()
{
	auto *page   = new QWidget;
	auto *layout = new QVBoxLayout(page);

	auto *checksRow = new QHBoxLayout;
	auto *left      = new QVBoxLayout;
	auto *right     = new QVBoxLayout;

	auto *autoConnect      = new QCheckBox(QStringLiteral("Auto connect to world on open"));
	auto *reconnect        = new QCheckBox(QStringLiteral("Reconnect on disconnect"));
	auto *openMax          = new QCheckBox(QStringLiteral("Open worlds Maximized"));
	auto *notifyDisconnect = new QCheckBox(QStringLiteral("Notify me when connection broken"));
	auto *notifyCannot     = new QCheckBox(QStringLiteral("Notify me if unable to connect"));
	auto *notifyOutput     = new QCheckBox(QStringLiteral("Notify to output window"));
	notifyOutput->setStyleSheet(QStringLiteral("margin-left: 20px;"));
	auto *allTyping   = new QCheckBox(QStringLiteral("All typing goes to command window"));
	auto *disableMenu = new QCheckBox(QStringLiteral("ALT key does not activate menu bar"));
	auto *fixedFont =
	    new QCheckBox(QStringLiteral("Use fixed space font when editing triggers/aliases etc."));
	auto *regexEmpty    = new QCheckBox(QStringLiteral("Regular expressions can match on an empty string"));
	auto *triggerRemove = new QCheckBox(QStringLiteral("Confirm before removing triggers/aliases/timers"));
	auto *backupOnUpgrades = new QCheckBox(QStringLiteral("Backup on upgrades"));

	registerCheck(QStringLiteral("AutoConnectWorlds"), autoConnect);
	registerCheck(QStringLiteral("ReconnectOnLinkFailure"), reconnect);
	registerCheck(QStringLiteral("OpenWorldsMaximised"), openMax);
	registerCheck(QStringLiteral("NotifyOnDisconnect"), notifyDisconnect);
	registerCheck(QStringLiteral("NotifyIfCannotConnect"), notifyCannot);
	registerCheck(QStringLiteral("ErrorNotificationToOutputWindow"), notifyOutput);
	registerCheck(QStringLiteral("AllTypingToCommandWindow"), allTyping);
	registerCheck(QStringLiteral("DisableKeyboardMenuActivation"), disableMenu);
	registerCheck(QStringLiteral("FixedFontForEditing"), fixedFont);
	registerCheck(QStringLiteral("RegexpMatchEmpty"), regexEmpty);
	registerCheck(QStringLiteral("TriggerRemoveCheck"), triggerRemove);
	registerCheck(QStringLiteral("BackupOnUpgrades"), backupOnUpgrades);

	left->addWidget(autoConnect);
	left->addWidget(reconnect);
	left->addWidget(openMax);
	left->addWidget(notifyDisconnect);
	left->addWidget(notifyCannot);
	left->addWidget(notifyOutput);
	left->addWidget(allTyping);
	left->addWidget(disableMenu);
	left->addWidget(fixedFont);
	left->addWidget(regexEmpty);
	left->addWidget(triggerRemove);

	auto *autoExpand          = new QCheckBox(QStringLiteral("Auto-expand config screens"));
	auto *colourGradient      = new QCheckBox(QStringLiteral("Use colour gradient in config"));
	auto *bleedBackground     = new QCheckBox(QStringLiteral("Bleed background colour to edge"));
	auto *smoothScroll        = new QCheckBox(QStringLiteral("Smoother scrolling"));
	auto *smootherScroll      = new QCheckBox(QStringLiteral("Very smooth scrolling"));
	auto *gridLines           = new QCheckBox(QStringLiteral("Show grid lines in list views"));
	auto *flatToolbars        = new QCheckBox(QStringLiteral("Flat toolbars"));
	auto *f1Macros            = new QCheckBox(QStringLiteral("F1, F6 are macros"));
	auto *disableWindowScaler = new QCheckBox(QStringLiteral("Disable window scaler"));

	registerCheck(QStringLiteral("AutoExpandConfig"), autoExpand);
	registerCheck(QStringLiteral("ColourGradientConfig"), colourGradient);
	registerCheck(QStringLiteral("BleedBackground"), bleedBackground);
	registerCheck(QStringLiteral("SmoothScrolling"), smoothScroll);
	registerCheck(QStringLiteral("SmootherScrolling"), smootherScroll);
	registerCheck(QStringLiteral("ShowGridLinesInListViews"), gridLines);
	registerCheck(QStringLiteral("FlatToolbars"), flatToolbars);
	registerCheck(QStringLiteral("F1macro"), f1Macros);
	registerCheck(QStringLiteral("DisableWindowScaler"), disableWindowScaler);

	right->addWidget(autoExpand);
	right->addWidget(colourGradient);
	right->addWidget(bleedBackground);
	right->addWidget(smoothScroll);
	right->addWidget(smootherScroll);
	right->addWidget(gridLines);
	right->addWidget(flatToolbars);
	right->addWidget(f1Macros);
	right->addWidget(backupOnUpgrades);
	right->addWidget(disableWindowScaler);
	right->addStretch();

	checksRow->addLayout(left, 1);
	checksRow->addLayout(right, 1);
	layout->addLayout(checksRow);

	auto *wordGroup  = new QGroupBox(QStringLiteral("Word Delimiters"));
	auto *wordLayout = new QGridLayout(wordGroup);
	wordLayout->addWidget(new QLabel(QStringLiteral("Tab completion:")), 0, 0);
	auto *tabDelims = new QLineEdit;
	registerEdit(QStringLiteral("WordDelimiters"), tabDelims);
	wordLayout->addWidget(tabDelims, 0, 1);
	wordLayout->addWidget(new QLabel(QStringLiteral("Double-click:")), 1, 0);
	auto *dblDelims = new QLineEdit;
	registerEdit(QStringLiteral("WordDelimitersDblClick"), dblDelims);
	wordLayout->addWidget(dblDelims, 1, 1);

	auto *spellGroup  = new QGroupBox(QStringLiteral("Spell Checker"));
	auto *spellLayout = new QVBoxLayout(spellGroup);
	auto *spellCheck  = new QCheckBox(QStringLiteral("Enable"));
	registerCheck(QStringLiteral("EnableSpellCheck"), spellCheck);
	spellLayout->addWidget(spellCheck);

	auto *leftBottom = new QVBoxLayout;
	leftBottom->addWidget(wordGroup);
	leftBottom->addWidget(spellGroup);

	auto *rightBottom = new QVBoxLayout;
	rightBottom->addWidget(new QLabel(QStringLiteral("Window Tabs:")));
	m_tabsStyle = new QComboBox;
	m_tabsStyle->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	m_tabsStyle->addItem(QStringLiteral("None"), 0);
	m_tabsStyle->addItem(QStringLiteral("Top"), 1);
	m_tabsStyle->addItem(QStringLiteral("Bottom"), 2);
	registerCombo(QStringLiteral("WindowTabsStyle"), m_tabsStyle);
	rightBottom->addWidget(m_tabsStyle);
	rightBottom->addSpacing(6);
	auto *localeRow = new QHBoxLayout;
	localeRow->addWidget(new QLabel(QStringLiteral("Locale:")));
	m_localeEdit = new QLineEdit;
	m_localeEdit->setMaxLength(3);
	registerEdit(QStringLiteral("Locale"), m_localeEdit);
	localeRow->addWidget(m_localeEdit);
	rightBottom->addLayout(localeRow);
	auto *registerFileExtensions = new QPushButton(QStringLiteral("Register File Extensions"));
	QObject::connect(
	    registerFileExtensions, &QPushButton::clicked, page,
	    [this]()
	    {
		    AppController *app = AppController::instance();
		    if (!app)
		    {
			    QMessageBox::warning(this, QStringLiteral("Global Preferences"),
			                         QStringLiteral("App controller is unavailable."));
			    return;
		    }

		    QString errorMessage;
		    if (AppController::registerFileAssociations(&errorMessage))
		    {
			    QMessageBox::information(this, QStringLiteral("Global Preferences"),
			                             QStringLiteral("File extensions were registered successfully."));
			    return;
		    }

		    const QString message =
		        errorMessage.isEmpty()
		            ? QStringLiteral("Failed to register file extensions.")
		            : QStringLiteral("Failed to register file extensions:\n%1").arg(errorMessage);
		    QMessageBox::warning(this, QStringLiteral("Global Preferences"), message);
	    });
	rightBottom->addWidget(registerFileExtensions);
	rightBottom->addStretch();

	auto *bottomRow = new QHBoxLayout;
	bottomRow->addLayout(leftBottom);
	bottomRow->addStretch();
	bottomRow->addLayout(rightBottom);
	layout->addLayout(bottomRow);

	layout->addStretch();
	return page;
}

QWidget *GlobalPreferencesDialog::buildClosingPage()
{
	auto *page   = new QWidget;
	auto *layout = new QVBoxLayout(page);
	registerCheck(QStringLiteral("ConfirmBeforeClosingWorld"),
	              new QCheckBox(QStringLiteral("Confirm before closing World")));
	registerCheck(QStringLiteral("ConfirmBeforeSavingVariables"),
	              new QCheckBox(QStringLiteral("Offer to save world if only Variables have changed.")));
	registerCheck(QStringLiteral("ConfirmBeforeClosingQmud"),
	              new QCheckBox(QStringLiteral("Confirm before closing QMud")));
	registerCheck(QStringLiteral("ConfirmBeforeClosingMXPdebug"),
	              new QCheckBox(QStringLiteral("Offer to save MXP debug windows")));
	layout->addWidget(m_intChecks.value(QStringLiteral("ConfirmBeforeClosingWorld")));
	layout->addWidget(m_intChecks.value(QStringLiteral("ConfirmBeforeSavingVariables")));
	layout->addWidget(m_intChecks.value(QStringLiteral("ConfirmBeforeClosingQmud")));
	layout->addWidget(m_intChecks.value(QStringLiteral("ConfirmBeforeClosingMXPdebug")));
	layout->addStretch();
	return page;
}

QWidget *GlobalPreferencesDialog::buildPrintingPage()
{
	auto *page   = new QWidget;
	auto *layout = new QVBoxLayout(page);

	auto *fontRow       = new QHBoxLayout;
	m_printerFontButton = new QPushButton(QStringLiteral("Font..."));
	fontRow->addWidget(m_printerFontButton);
	fontRow->addStretch();
	layout->addLayout(fontRow);
	m_printerFontLabel = new QLabel(QStringLiteral("Font name"));
	registerLabel(QStringLiteral("PrinterFont"), m_printerFontLabel);
	layout->addWidget(m_printerFontLabel);

	auto *grid = new QGridLayout;
	grid->addWidget(new QLabel(QStringLiteral("Top margin:")), 0, 0, Qt::AlignRight);
	m_printerTopMargin = new QSpinBox;
	m_printerTopMargin->setRange(0, 100);
	registerSpin(QStringLiteral("PrinterTopMargin"), m_printerTopMargin);
	grid->addWidget(m_printerTopMargin, 0, 1);
	grid->addWidget(new QLabel(QStringLiteral("mm.")), 0, 2);

	grid->addWidget(new QLabel(QStringLiteral("Left margin:")), 1, 0, Qt::AlignRight);
	m_printerLeftMargin = new QSpinBox;
	m_printerLeftMargin->setRange(0, 100);
	registerSpin(QStringLiteral("PrinterLeftMargin"), m_printerLeftMargin);
	grid->addWidget(m_printerLeftMargin, 1, 1);
	grid->addWidget(new QLabel(QStringLiteral("mm.")), 1, 2);

	grid->addWidget(new QLabel(QStringLiteral("Lines per page:")), 2, 0, Qt::AlignRight);
	m_printerLinesPerPage = new QSpinBox;
	m_printerLinesPerPage->setRange(10, 500);
	registerSpin(QStringLiteral("PrinterLinesPerPage"), m_printerLinesPerPage);
	grid->addWidget(m_printerLinesPerPage, 2, 1);

	layout->addLayout(grid);
	layout->addStretch();

	QObject::connect(m_printerFontButton, &QPushButton::clicked, page,
	                 [this]()
	                 {
		                 bool  ok = false;
		                 QFont current(m_printerFontLabel->text());
		                 if (m_printerFontSize > 0)
			                 current.setPointSize(m_printerFontSize);
		                 QFont chosen =
		                     QFontDialog::getFont(&ok, current, this, QStringLiteral("Select Printer Font"));
		                 if (!ok)
			                 return;
		                 m_printerFontLabel->setText(chosen.family());
		                 m_printerFontSize = chosen.pointSize();
	                 });
	return page;
}

QWidget *GlobalPreferencesDialog::buildLoggingPage()
{
	auto *page   = new QWidget;
	auto *layout = new QVBoxLayout(page);

	auto *defaultDirButton = new QPushButton(QStringLiteral("Default Log Files Directory..."));
	defaultDirButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	auto *compressLogsButton = new QPushButton(QStringLiteral("Compress all uncompressed logs"));
	compressLogsButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	m_logDefaultDirLabel = new QLabel(QStringLiteral("Default Directory"));
	auto *dirRow         = new QHBoxLayout;
	dirRow->addWidget(defaultDirButton);
	dirRow->addWidget(compressLogsButton);
	dirRow->addStretch();
	layout->addLayout(dirRow);
	layout->addWidget(m_logDefaultDirLabel);
	layout->addSpacing(8);
	QObject::connect(defaultDirButton, &QPushButton::clicked, page,
	                 [this]()
	                 {
		                 AppController *app = AppController::instance();
		                 if (app)
			                 app->changeToFileBrowsingDirectory();
		                 const QString startDir =
		                     m_logDefaultDirLabel ? m_logDefaultDirLabel->text() : QString();
		                 const QString dir = QFileDialog::getExistingDirectory(
		                     this, QStringLiteral("Select Default Log Files Directory"), startDir);
		                 if (app)
			                 app->changeToStartupDirectory();
		                 if (!dir.isEmpty())
			                 m_logDefaultDirLabel->setText(runtimePath(dir));
	                 });
	QObject::connect(
	    compressLogsButton, &QPushButton::clicked, page,
	    [this, compressLogsButton]()
	    {
		    AppController *app = AppController::instance();
		    if (!app || !m_logDefaultDirLabel)
			    return;

		    const QString configuredDir = m_logDefaultDirLabel->text().trimmed();
		    if (configuredDir.isEmpty())
		    {
			    QMessageBox::information(this, QStringLiteral("Compress Logs"),
			                             QStringLiteral("Default log directory is not set."));
			    return;
		    }

		    const QString   absoluteDir = QDir::cleanPath(app->makeAbsolutePath(configuredDir));
		    const QFileInfo dirInfo(absoluteDir);
		    if (!dirInfo.exists() || !dirInfo.isDir())
		    {
			    QMessageBox::warning(
			        this, QStringLiteral("Compress Logs"),
			        QStringLiteral("Default log directory does not exist:\n%1").arg(absoluteDir));
			    return;
		    }

		    QSet<QString> openLogPathKeys;
		    const auto    toPathKey = [](const QString &path) -> QString
		    {
			    QString key = QDir::cleanPath(storagePath(path));
#ifdef Q_OS_WIN
			    key = key.toLower();
#endif
			    return key;
		    };
		    for (const QString &openLogPath : app->activeOpenWorldLogFiles())
			    openLogPathKeys.insert(toPathKey(openLogPath));

		    compressLogsButton->setEnabled(false);
		    const QPointer<GlobalPreferencesDialog> dialogGuard(this);
		    const QPointer<QPushButton>             buttonGuard(compressLogsButton);

		    struct CompressionSummary
		    {
				    int         compressedCount{0};
				    int         skippedCount{0};
				    int         inUseSkippedCount{0};
				    QStringList failures;
		    };

		    QThreadPool::globalInstance()->start(
		        [absoluteDir, openLogPathKeys, toPathKey, dialogGuard, buttonGuard]()
		        {
			        CompressionSummary summary;
			        QDirIterator       it(absoluteDir, QDir::Files | QDir::NoDotAndDotDot,
			                              QDirIterator::Subdirectories);
			        while (it.hasNext())
			        {
				        const QString filePath = it.next();
				        if (openLogPathKeys.contains(toPathKey(filePath)))
				        {
					        ++summary.inUseSkippedCount;
					        continue;
				        }
				        if (filePath.endsWith(QStringLiteral(".gz"), Qt::CaseInsensitive))
				        {
					        ++summary.skippedCount;
					        continue;
				        }
				        if (isFileOpenForWriteByAnotherProcess(filePath))
				        {
					        ++summary.inUseSkippedCount;
					        continue;
				        }

				        QString gzipError;
				        if (qmudGzipFileInPlace(filePath, &gzipError))
				        {
					        ++summary.compressedCount;
				        }
				        else
				        {
					        if (gzipError.isEmpty())
						        gzipError = QStringLiteral("Unknown error.");
					        summary.failures.push_back(QStringLiteral("%1: %2").arg(filePath, gzipError));
				        }
			        }

			        if (!dialogGuard && !buttonGuard)
				        return;

			        if (dialogGuard)
			        {
				        QMetaObject::invokeMethod(
				            dialogGuard.data(),
				            [dialogGuard, buttonGuard, summary = std::move(summary)]()
				            {
					            if (buttonGuard)
						            buttonGuard->setEnabled(true);
					            if (!dialogGuard)
						            return;

					            if (!summary.failures.isEmpty())
					            {
						            QMessageBox::warning(
						                dialogGuard, QStringLiteral("Compress Logs"),
						                QStringLiteral(
						                    "Compressed %1 file(s), skipped %2 existing .gz file(s), "
						                    "skipped %3 file(s) currently in use, failed %4 file(s).\n\n"
						                    "First error:\n%5")
						                    .arg(summary.compressedCount)
						                    .arg(summary.skippedCount)
						                    .arg(summary.inUseSkippedCount)
						                    .arg(summary.failures.size())
						                    .arg(summary.failures.front()));
						            return;
					            }

					            QMessageBox::information(
					                dialogGuard, QStringLiteral("Compress Logs"),
					                QStringLiteral("Compressed %1 file(s), skipped %2 existing .gz "
					                               "file(s), skipped %3 file(s) currently in use.")
					                    .arg(summary.compressedCount)
					                    .arg(summary.skippedCount)
					                    .arg(summary.inUseSkippedCount));
				            },
				            Qt::QueuedConnection);
			        }
			        else if (buttonGuard)
			        {
				        QMetaObject::invokeMethod(
				            buttonGuard.data(), [buttonGuard]() { buttonGuard->setEnabled(true); },
				            Qt::QueuedConnection);
			        }
		        });
	    });

	m_autoLogCheck = new QCheckBox(QStringLiteral("Auto Log when opening world"));
	registerCheck(QStringLiteral("AutoLogWorld"), m_autoLogCheck);
	layout->addWidget(m_autoLogCheck);
	registerCheck(QStringLiteral("AppendToLogFiles"), new QCheckBox(QStringLiteral("Append to log files")));
	registerCheck(QStringLiteral("ConfirmLogFileClose"),
	              new QCheckBox(QStringLiteral("Confirm when closing log file")));
	layout->addWidget(m_intChecks.value(QStringLiteral("AppendToLogFiles")));
	layout->addWidget(m_intChecks.value(QStringLiteral("ConfirmLogFileClose")));

	layout->addStretch();
	return page;
}

QWidget *GlobalPreferencesDialog::buildTimersPage()
{
	auto *page   = new QWidget;
	auto *layout = new QVBoxLayout(page);

	auto *row = new QHBoxLayout;
	row->addWidget(new QLabel(QStringLiteral("Timer interval (seconds):")));
	m_timerInterval = new QSpinBox;
	m_timerInterval->setMaximum(120);
	registerSpin(QStringLiteral("TimerInterval"), m_timerInterval);
	row->addWidget(m_timerInterval);
	row->addStretch();
	layout->addLayout(row);

	auto *note =
	    new QLabel(QStringLiteral("Set to zero for timers that can fire up to 1/10 of a second apart."));
	note->setWordWrap(true);
	layout->addWidget(note);
	layout->addStretch();
	return page;
}

QWidget *GlobalPreferencesDialog::buildActivityPage()
{
	auto *page   = new QWidget;
	auto *layout = new QVBoxLayout(page);

	m_openActivityWindow = new QCheckBox(QStringLiteral("Open activity window at startup"));
	registerCheck(QStringLiteral("OpenActivityWindow"), m_openActivityWindow);
	layout->addWidget(m_openActivityWindow);

	auto *group        = new QGroupBox(QStringLiteral("Update activity window"));
	auto *groupLayout  = new QGridLayout(group);
	m_activityOnNew    = new QRadioButton(QStringLiteral("On New Activity"));
	m_activityPeriodic = new QRadioButton(QStringLiteral("Periodically, every:"));
	m_activityBoth     = new QRadioButton(QStringLiteral("Both"));
	groupLayout->addWidget(m_activityOnNew, 0, 0, 1, 2);
	groupLayout->addWidget(m_activityPeriodic, 1, 0);
	m_activityPeriod = new QSpinBox;
	m_activityPeriod->setRange(1, 300);
	groupLayout->addWidget(m_activityPeriod, 1, 1);
	groupLayout->addWidget(new QLabel(QStringLiteral("second(s)")), 1, 2);
	groupLayout->addWidget(m_activityBoth, 2, 0);
	layout->addWidget(group);

	auto *styleRow = new QHBoxLayout;
	styleRow->addWidget(new QLabel(QStringLiteral("Activity button bar style:")));
	m_activityBarStyle = new QComboBox;
	m_activityBarStyle->addItem(QStringLiteral("Default"), 0);
	m_activityBarStyle->addItem(QStringLiteral("Style 1"), 1);
	m_activityBarStyle->addItem(QStringLiteral("Style 2"), 2);
	m_activityBarStyle->addItem(QStringLiteral("Style 3"), 3);
	m_activityBarStyle->addItem(QStringLiteral("Style 4"), 4);
	registerCombo(QStringLiteral("ActivityButtonBarStyle"), m_activityBarStyle);
	styleRow->addWidget(m_activityBarStyle);
	styleRow->addStretch();
	layout->addLayout(styleRow);

	layout->addStretch();
	return page;
}

QWidget *GlobalPreferencesDialog::buildDefaultsPage()
{
	auto *page   = new QWidget;
	auto *layout = new QVBoxLayout(page);

	auto  connectBrowse = [this](const QPushButton *button, QLineEdit *target, const QString &title,
                                const QString &filter, const QString &suggestedName)
	{
		QObject::connect(button, &QPushButton::clicked, this,
		                 [this, target, title, filter, suggestedName]()
		                 {
			                 AppController *app = AppController::instance();
			                 if (app)
				                 app->changeToFileBrowsingDirectory();

			                 QString startPath = suggestedName;
			                 if (!suggestedName.isEmpty())
			                 {
				                 if (app)
					                 startPath = app->makeAbsolutePath(suggestedName);
				                 else
					                 startPath =
					                     QDir(QCoreApplication::applicationDirPath()).filePath(suggestedName);
			                 }

			                 const QString file =
			                     QFileDialog::getOpenFileName(this, title, startPath, filter);

			                 if (app)
				                 app->changeToStartupDirectory();

			                 if (!file.isEmpty() && target)
				                 target->setText(file);
		                 });
	};

	auto *grid = new QGridLayout;
	grid->addWidget(new QLabel(QStringLiteral("Colours:")), 0, 0, Qt::AlignRight);
	m_defaultColoursEdit = new QLineEdit;
	registerEdit(QStringLiteral("DefaultColoursFile"), m_defaultColoursEdit);
	grid->addWidget(m_defaultColoursEdit, 0, 1);
	auto *browseColours = new QPushButton(QStringLiteral("Browse..."));
	grid->addWidget(browseColours, 0, 2);

	grid->addWidget(new QLabel(QStringLiteral("Triggers:")), 1, 0, Qt::AlignRight);
	m_defaultTriggersEdit = new QLineEdit;
	registerEdit(QStringLiteral("DefaultTriggersFile"), m_defaultTriggersEdit);
	grid->addWidget(m_defaultTriggersEdit, 1, 1);
	auto *browseTriggers = new QPushButton(QStringLiteral("Browse..."));
	grid->addWidget(browseTriggers, 1, 2);

	grid->addWidget(new QLabel(QStringLiteral("Aliases:")), 2, 0, Qt::AlignRight);
	m_defaultAliasesEdit = new QLineEdit;
	registerEdit(QStringLiteral("DefaultAliasesFile"), m_defaultAliasesEdit);
	grid->addWidget(m_defaultAliasesEdit, 2, 1);
	auto *browseAliases = new QPushButton(QStringLiteral("Browse..."));
	grid->addWidget(browseAliases, 2, 2);

	grid->addWidget(new QLabel(QStringLiteral("Macros:")), 3, 0, Qt::AlignRight);
	m_defaultMacrosEdit = new QLineEdit;
	registerEdit(QStringLiteral("DefaultMacrosFile"), m_defaultMacrosEdit);
	grid->addWidget(m_defaultMacrosEdit, 3, 1);
	auto *browseMacros = new QPushButton(QStringLiteral("Browse..."));
	grid->addWidget(browseMacros, 3, 2);

	grid->addWidget(new QLabel(QStringLiteral("Timers:")), 4, 0, Qt::AlignRight);
	m_defaultTimersEdit = new QLineEdit;
	registerEdit(QStringLiteral("DefaultTimersFile"), m_defaultTimersEdit);
	grid->addWidget(m_defaultTimersEdit, 4, 1);
	auto *browseTimers = new QPushButton(QStringLiteral("Browse..."));
	grid->addWidget(browseTimers, 4, 2);

	layout->addLayout(grid);

	connectBrowse(browseColours, m_defaultColoursEdit, QStringLiteral("Colour file name"),
	              QStringLiteral("QMud colours (*.qdc *.mcc)"), QStringLiteral("Default colours.qdc"));
	connectBrowse(browseTriggers, m_defaultTriggersEdit, QStringLiteral("Trigger file name"),
	              QStringLiteral("QMud triggers (*.qdt *.mct)"), QStringLiteral("Default triggers.qdt"));
	connectBrowse(browseAliases, m_defaultAliasesEdit, QStringLiteral("Alias file name"),
	              QStringLiteral("QMud aliases (*.qda *.mca)"), QStringLiteral("Default aliases.qda"));
	connectBrowse(browseMacros, m_defaultMacrosEdit, QStringLiteral("Macro file name"),
	              QStringLiteral("QMud macros (*.qdm *.mcm)"), QStringLiteral("Default macros.qdm"));
	connectBrowse(browseTimers, m_defaultTimersEdit, QStringLiteral("Timers file name"),
	              QStringLiteral("QMud timers (*.qdi *.mci)"), QStringLiteral("Default timers.qdi"));

	auto *fontGrid     = new QGridLayout;
	m_outputFontButton = new QPushButton(QStringLiteral("Output Font..."));
	fontGrid->addWidget(m_outputFontButton, 0, 0);
	m_outputFontName = new QLineEdit;
	m_outputFontName->setReadOnly(true);
	registerEdit(QStringLiteral("DefaultOutputFont"), m_outputFontName);
	fontGrid->addWidget(m_outputFontName, 0, 1);
	m_outputFontStyle = new QLineEdit;
	m_outputFontStyle->setReadOnly(true);
	fontGrid->addWidget(m_outputFontStyle, 0, 2);

	m_inputFontButton = new QPushButton(QStringLiteral("Input Font..."));
	fontGrid->addWidget(m_inputFontButton, 1, 0);
	m_inputFontName = new QLineEdit;
	m_inputFontName->setReadOnly(true);
	registerEdit(QStringLiteral("DefaultInputFont"), m_inputFontName);
	fontGrid->addWidget(m_inputFontName, 1, 1);
	m_inputFontStyle = new QLineEdit;
	m_inputFontStyle->setReadOnly(true);
	fontGrid->addWidget(m_inputFontStyle, 1, 2);

	layout->addLayout(fontGrid);
	layout->addStretch();

	QObject::connect(
	    m_outputFontButton, &QPushButton::clicked, page,
	    [this]()
	    {
		    bool  ok = false;
		    QFont current(m_outputFontName->text());
		    if (m_outputFontHeight > 0)
			    current.setPointSize(m_outputFontHeight);
		    current.setWeight(QFont::Normal);
		    current.setItalic(false);
		    QFont chosen = QFontDialog::getFont(&ok, current, this, QStringLiteral("Select Output Font"));
		    if (!ok)
			    return;
		    m_outputFontName->setText(chosen.family());
		    m_outputFontHeight = chosen.pointSize();
		    if (m_outputFontStyle)
			    m_outputFontStyle->setText(fontStyleSummary(m_outputFontHeight, QFont::Normal, false));
	    });
	QObject::connect(m_inputFontButton, &QPushButton::clicked, page,
	                 [this]()
	                 {
		                 bool  ok = false;
		                 QFont current(m_inputFontName->text());
		                 if (m_inputFontHeight > 0)
			                 current.setPointSize(m_inputFontHeight);
		                 if (m_inputFontWeight > 0)
			                 current.setWeight(static_cast<QFont::Weight>(m_inputFontWeight));
		                 current.setItalic(m_inputFontItalic != 0);
		                 QFont chosen =
		                     QFontDialog::getFont(&ok, current, this, QStringLiteral("Select Input Font"));
		                 if (!ok)
			                 return;
		                 m_inputFontName->setText(chosen.family());
		                 m_inputFontHeight = chosen.pointSize();
		                 m_inputFontWeight = chosen.weight();
		                 m_inputFontItalic = chosen.italic() ? 1 : 0;
		                 if (m_inputFontStyle)
			                 m_inputFontStyle->setText(fontStyleSummary(m_inputFontHeight, m_inputFontWeight,
			                                                            m_inputFontItalic != 0));
	                 });
	return page;
}

QWidget *GlobalPreferencesDialog::buildNotepadPage()
{
	auto *page   = new QWidget;
	auto *layout = new QVBoxLayout(page);

	m_notepadWordWrap = new QCheckBox(QStringLiteral("Word Wrap"));
	registerCheck(QStringLiteral("NotepadWordWrap"), m_notepadWordWrap);
	layout->addWidget(m_notepadWordWrap);

	auto *colourGroup = new QGroupBox(QStringLiteral("Window Colour"));
	colourGroup->setMinimumWidth(240);
	colourGroup->setMinimumHeight(70);
	colourGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	auto *colourLayout = new QHBoxLayout(colourGroup);
	colourLayout->setContentsMargins(8, 8, 8, 8);
	colourLayout->addStretch();
	m_notepadTextSwatch = makeSwatchButton();
	colourLayout->addWidget(m_notepadTextSwatch);
	auto *onLabel = new QLabel(QStringLiteral("on"));
	onLabel->setAlignment(Qt::AlignCenter);
	colourLayout->addWidget(onLabel);
	m_notepadBackSwatch = makeSwatchButton();
	colourLayout->addWidget(m_notepadBackSwatch);
	colourLayout->addStretch();
	layout->addWidget(colourGroup, 0, Qt::AlignHCenter);

	auto *parenGroup    = new QGroupBox(QStringLiteral("Parenthesis Matching Preferences"));
	auto *parenLayout   = new QGridLayout(parenGroup);
	m_parenNestBraces   = new QCheckBox(QStringLiteral("Braces nest"));
	m_parenBackslash    = new QCheckBox(QStringLiteral("\\ escapes next character"));
	m_parenPercent      = new QCheckBox(QStringLiteral("% escapes next character"));
	m_parenSingleQuotes = new QCheckBox(QStringLiteral("' quotes a string"));
	m_parenSingleEscape = new QCheckBox(QStringLiteral("... with escape inside quotes"));
	m_parenDoubleQuotes = new QCheckBox(QStringLiteral("\" quotes a string"));
	m_parenDoubleEscape = new QCheckBox(QStringLiteral("... with escape inside quotes"));
	parenLayout->addWidget(m_parenNestBraces, 0, 0);
	parenLayout->addWidget(m_parenBackslash, 1, 0);
	parenLayout->addWidget(m_parenPercent, 2, 0);
	parenLayout->addWidget(m_parenSingleQuotes, 3, 0);
	parenLayout->addWidget(m_parenSingleEscape, 3, 1);
	parenLayout->addWidget(m_parenDoubleQuotes, 4, 0);
	parenLayout->addWidget(m_parenDoubleEscape, 4, 1);
	layout->addWidget(parenGroup);

	auto *quoteRow = new QHBoxLayout;
	quoteRow->addWidget(new QLabel(QStringLiteral("Quote string:")));
	m_notepadQuote = new QLineEdit;
	registerEdit(QStringLiteral("NotepadQuoteString"), m_notepadQuote);
	quoteRow->addWidget(m_notepadQuote);
	layout->addLayout(quoteRow);

	QObject::connect(m_notepadTextSwatch, &QPushButton::clicked, page,
	                 [this]()
	                 {
		                 QColor current = colorFromColorRef(m_notepadTextColorRef);
		                 QColor chosen =
		                     QColorDialog::getColor(current, this, QStringLiteral("Select Text Colour"));
		                 if (!chosen.isValid())
			                 return;
		                 m_notepadTextColorRef = colorRefFromColor(chosen);
		                 updateSwatchButton(m_notepadTextSwatch, chosen);
	                 });
	QObject::connect(m_notepadBackSwatch, &QPushButton::clicked, page,
	                 [this]()
	                 {
		                 QColor current = colorFromColorRef(m_notepadBackColorRef);
		                 QColor chosen  = QColorDialog::getColor(current, this,
		                                                         QStringLiteral("Select Background Colour"));
		                 if (!chosen.isValid())
			                 return;
		                 m_notepadBackColorRef = colorRefFromColor(chosen);
		                 updateSwatchButton(m_notepadBackSwatch, chosen);
	                 });

	layout->addStretch();
	return page;
}

QWidget *GlobalPreferencesDialog::buildTrayPage()
{
	auto *page   = new QWidget;
	auto *layout = new QVBoxLayout(page);

	auto *topRow = new QHBoxLayout;
	topRow->addWidget(new QLabel(QStringLiteral("Show QMud in:")));
	m_iconPlacement = new QComboBox;
	m_iconPlacement->addItem(QStringLiteral("Taskbar"), 0);
	m_iconPlacement->addItem(QStringLiteral("Tray"), 1);
	m_iconPlacement->addItem(QStringLiteral("Both"), 2);
	registerCombo(QStringLiteral("Icon Placement"), m_iconPlacement);
	topRow->addWidget(m_iconPlacement);
	topRow->addStretch();
	layout->addLayout(topRow);

	auto *grid      = new QGridLayout;
	m_trayIconGroup = new QButtonGroup(page);
	constexpr QSize trayPreviewSize(32, 32);
	auto            iconRadio = [this, trayPreviewSize](const QString &text, const QString &path, int id)
	{
		auto *radio = new QRadioButton(text);
		QIcon icon(path);
		if (icon.isNull())
		{
			QPixmap pix(path);
			if (!pix.isNull())
				icon = QIcon(pix.scaled(trayPreviewSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
		}
		radio->setIcon(icon);
		radio->setIconSize(trayPreviewSize);
		m_trayIconGroup->addButton(radio, id);
		return radio;
	};

	grid->addWidget(iconRadio(QStringLiteral("QMud"), QStringLiteral(":/qmud/res/QMud.png"), 0), 0, 0);
	grid->addWidget(iconRadio(QStringLiteral("Icon 1"), QStringLiteral(":/qmud/res/Cdrom01.ico"), 1), 1, 0);
	grid->addWidget(iconRadio(QStringLiteral("Icon 2"), QStringLiteral(":/qmud/res/Wrench.ico"), 2), 2, 0);
	grid->addWidget(iconRadio(QStringLiteral("Icon 3"), QStringLiteral(":/qmud/res/Clock05.ico"), 3), 3, 0);
	grid->addWidget(iconRadio(QStringLiteral("Icon 4"), QStringLiteral(":/qmud/res/Earth.ico"), 4), 4, 0);
	grid->addWidget(iconRadio(QStringLiteral("Icon 5"), QStringLiteral(":/qmud/res/Graph08.ico"), 5), 0, 1);
	grid->addWidget(iconRadio(QStringLiteral("Icon 6"), QStringLiteral(":/qmud/res/Handshak.ico"), 6), 1, 1);
	grid->addWidget(iconRadio(QStringLiteral("Icon 7"), QStringLiteral(":/qmud/res/Mail16b.ico"), 7), 2, 1);
	grid->addWidget(iconRadio(QStringLiteral("Icon 8"), QStringLiteral(":/qmud/res/Net01.ico"), 8), 3, 1);
	grid->addWidget(iconRadio(QStringLiteral("Icon 9"), QStringLiteral(":/qmud/res/Point11.ico"), 9), 4, 1);

	layout->addLayout(grid);

	auto *customRow   = new QHBoxLayout;
	m_customIconRadio = new QRadioButton(QStringLiteral("Custom:"));
	m_trayIconGroup->addButton(m_customIconRadio, 10);
	customRow->addWidget(m_customIconRadio);
	m_customIconButton = new QPushButton(QStringLiteral("Choose..."));
	customRow->addWidget(m_customIconButton);
	customRow->addStretch();
	layout->addLayout(customRow);

	m_customIconLabel = new QLabel(QStringLiteral("File name"));
	layout->addWidget(m_customIconLabel);
	layout->addStretch();

	QObject::connect(m_customIconButton, &QPushButton::clicked, page,
	                 [this]()
	                 {
		                 AppController *app = AppController::instance();
		                 if (app)
			                 app->changeToFileBrowsingDirectory();
		                 const QString file = QFileDialog::getOpenFileName(
		                     this, QStringLiteral("Select Custom Tray Icon"), QString(),
		                     QStringLiteral("Icon files (*.png *.jpg *.ico)"));
		                 if (app)
			                 app->changeToStartupDirectory();
		                 if (!file.isEmpty())
		                 {
			                 m_customIconLabel->setText(runtimePath(file));
			                 if (m_customIconRadio)
				                 m_customIconRadio->setChecked(true);
		                 }
	                 });
	return page;
}

QWidget *GlobalPreferencesDialog::buildPluginsPage()
{
	auto *page   = new QWidget;
	auto *layout = new QVBoxLayout(page);

	auto *header        = new QHBoxLayout;
	auto *title         = new QLabel(QStringLiteral("Global plugins (load into each world) ..."));
	m_pluginsCountLabel = new QLabel(QStringLiteral("0 plugins"));
	m_pluginsCountLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	header->addWidget(title);
	header->addStretch();
	header->addWidget(m_pluginsCountLabel);
	layout->addLayout(header);

	m_pluginsList = new QListWidget;
	layout->addWidget(m_pluginsList);

	m_pluginsSelected = new QLabel(QStringLiteral("Selected plugin"));
	m_pluginsSelected->setWordWrap(true);
	layout->addWidget(m_pluginsSelected);

	auto *buttons      = new QHBoxLayout;
	auto *addPlugin    = new QPushButton(QStringLiteral("Add..."));
	auto *removePlugin = new QPushButton(QStringLiteral("Remove"));
	auto *moveUp       = new QPushButton(QStringLiteral("Move Up"));
	auto *moveDown     = new QPushButton(QStringLiteral("Move Down"));
	buttons->addWidget(addPlugin);
	buttons->addWidget(removePlugin);
	buttons->addWidget(moveUp);
	buttons->addWidget(moveDown);
	layout->addLayout(buttons);

	auto *bottom           = new QHBoxLayout;
	auto *pluginsDirButton = new QPushButton(QStringLiteral("Plugins Directory..."));
	bottom->addWidget(pluginsDirButton);
	bottom->addStretch();
	layout->addLayout(bottom);

	m_pluginsDefaultDir = new QLabel(QStringLiteral("Default plugins directory"));
	layout->addWidget(m_pluginsDefaultDir);

	QObject::connect(m_pluginsList, &QListWidget::currentTextChanged, page,
	                 [this](const QString &) { syncPluginListSelection(); });
	const auto updatePluginButtons = [this, removePlugin, moveUp, moveDown]()
	{
		if (!m_pluginsList)
			return;
		const int row   = m_pluginsList->currentRow();
		const int count = m_pluginsList->count();
		removePlugin->setEnabled(row >= 0);
		moveUp->setEnabled(row > 0);
		moveDown->setEnabled(row >= 0 && row < (count - 1));
		syncPluginListSelection();
	};
	QObject::connect(addPlugin, &QPushButton::clicked, page,
	                 [this]()
	                 {
		                 AppController *app = AppController::instance();
		                 if (app)
			                 app->changeToFileBrowsingDirectory();
		                 const QString startDir =
		                     m_pluginsDefaultDir ? m_pluginsDefaultDir->text() : QString();
		                 const QStringList paths =
		                     QFileDialog::getOpenFileNames(this, QStringLiteral("Select Plugin"), startDir,
		                                                   QStringLiteral("Plugin files (*.xml)"));
		                 if (app)
			                 app->changeToStartupDirectory();
		                 if (paths.isEmpty())
			                 return;
		                 addUniqueItemsToList(m_pluginsList, canonicalPathList(paths));
		                 m_pluginsList->setCurrentRow(m_pluginsList->count() - 1);
	                 });
	QObject::connect(removePlugin, &QPushButton::clicked, page,
	                 [this, updatePluginButtons]()
	                 {
		                 delete m_pluginsList->currentItem();
		                 updatePluginButtons();
	                 });
	QObject::connect(moveUp, &QPushButton::clicked, page,
	                 [this, updatePluginButtons]()
	                 {
		                 const int row = m_pluginsList->currentRow();
		                 if (row > 0)
		                 {
			                 QListWidgetItem *item = m_pluginsList->takeItem(row);
			                 m_pluginsList->insertItem(row - 1, item);
			                 m_pluginsList->setCurrentRow(row - 1);
		                 }
		                 updatePluginButtons();
	                 });
	QObject::connect(moveDown, &QPushButton::clicked, page,
	                 [this, updatePluginButtons]()
	                 {
		                 const int row = m_pluginsList->currentRow();
		                 if (row >= 0 && row < m_pluginsList->count() - 1)
		                 {
			                 QListWidgetItem *item = m_pluginsList->takeItem(row);
			                 m_pluginsList->insertItem(row + 1, item);
			                 m_pluginsList->setCurrentRow(row + 1);
		                 }
		                 updatePluginButtons();
	                 });
	QObject::connect(pluginsDirButton, &QPushButton::clicked, page,
	                 [this]()
	                 {
		                 AppController *app = AppController::instance();
		                 if (app)
			                 app->changeToFileBrowsingDirectory();
		                 const QString startDir =
		                     m_pluginsDefaultDir ? m_pluginsDefaultDir->text() : QString();
		                 const QString dir = QFileDialog::getExistingDirectory(
		                     this, QStringLiteral("Select Plugins Directory"), startDir);
		                 if (app)
			                 app->changeToStartupDirectory();
		                 if (!dir.isEmpty())
			                 m_pluginsDefaultDir->setText(runtimePath(dir));
	                 });
	QObject::connect(m_pluginsList, &QListWidget::currentRowChanged, page,
	                 [updatePluginButtons](int) { updatePluginButtons(); });
	updatePluginButtons();
	return page;
}

QWidget *GlobalPreferencesDialog::buildLuaPage()
{
	auto *page   = new QWidget;
	auto *layout = new QVBoxLayout(page);

	auto *label =
	    new QLabel(QStringLiteral("Preliminary code (executed each time a Lua script session starts):"));
	layout->addWidget(label);

	m_luaScript = new QTextEdit;
	layout->addWidget(m_luaScript);

	auto *bottom     = new QHBoxLayout;
	auto *editButton = new QPushButton(QStringLiteral("Edit..."));
	bottom->addWidget(editButton);
	bottom->addStretch();
	m_allowDllsCheck = new QCheckBox(QStringLiteral("Allow dynamic libraries to be loaded"));
	registerCheck(QStringLiteral("AllowLoadingDlls"), m_allowDllsCheck);
	bottom->addWidget(m_allowDllsCheck);
	layout->addLayout(bottom);

	QObject::connect(editButton, &QPushButton::clicked, page,
	                 [this]()
	                 {
		                 QDialog dlg(this);
		                 dlg.setWindowTitle(QStringLiteral("Edit Lua Script"));
		                 auto *layout = new QVBoxLayout(&dlg);
		                 auto *editor = new QTextEdit(&dlg);
		                 editor->setPlainText(m_luaScript ? m_luaScript->toPlainText() : QString());
		                 layout->addWidget(editor);
		                 auto *buttons =
		                     new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
		                 layout->addWidget(buttons);
		                 QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		                 QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
		                 if (dlg.exec() == QDialog::Accepted)
		                 {
			                 if (m_luaScript)
				                 m_luaScript->setPlainText(editor->toPlainText());
		                 }
	                 });

	return page;
}

QWidget *GlobalPreferencesDialog::buildUpdatesPage()
{
	auto *page   = new QWidget;
	auto *layout = new QVBoxLayout(page);

	m_autoCheckUpdatesCheck = new QCheckBox(QStringLiteral("Automatically check for updates"));
	registerCheck(QStringLiteral("AutoCheckForUpdates"), m_autoCheckUpdatesCheck);
	layout->addWidget(m_autoCheckUpdatesCheck);

	auto *checkRow          = new QHBoxLayout;
	m_updateCheckEveryLabel = new QLabel(QStringLiteral("Check every:"));
	checkRow->addWidget(m_updateCheckEveryLabel);
	m_updateCheckHoursSpin = new QSpinBox;
	m_updateCheckHoursSpin->setRange(1, 168);
	m_updateCheckHoursSpin->setSuffix(QStringLiteral(" hour(s)"));
	registerSpin(QStringLiteral("UpdateCheckIntervalHours"), m_updateCheckHoursSpin);
	checkRow->addWidget(m_updateCheckHoursSpin);
	m_checkNowButton = new QPushButton(QStringLiteral("Check now"));
	checkRow->addWidget(m_checkNowButton);
	checkRow->addStretch();
	layout->addLayout(checkRow);

	m_enableReloadFeatureCheck = new QCheckBox(QStringLiteral("Enable reload feature (Linux/MacOS)"));
	registerCheck(QStringLiteral("EnableReloadFeature"), m_enableReloadFeatureCheck);
	layout->addWidget(m_enableReloadFeatureCheck);

	auto *timeoutRow = new QHBoxLayout;
	timeoutRow->addWidget(new QLabel(QStringLiteral("Timeout for MCCP worlds to tear down compression:")));
	m_reloadMccpTimeoutSpin = new QSpinBox;
	m_reloadMccpTimeoutSpin->setRange(300, 2000);
	m_reloadMccpTimeoutSpin->setSingleStep(50);
	m_reloadMccpTimeoutSpin->setSuffix(QStringLiteral(" ms"));
	registerSpin(QStringLiteral("ReloadMccpDisableTimeoutMs"), m_reloadMccpTimeoutSpin);
	timeoutRow->addWidget(m_reloadMccpTimeoutSpin);
	timeoutRow->addStretch();
	layout->addLayout(timeoutRow);

	layout->addStretch();

	connect(m_autoCheckUpdatesCheck, &QCheckBox::toggled, page,
	        [this](const bool) { refreshUpdateCheckControlsEnabledState(); });
	connect(m_checkNowButton, &QPushButton::clicked, page,
	        [this]()
	        {
		        if (AppController *app = AppController::instance(); app)
			        app->checkForUpdatesNow(this);
	        });

	return page;
}

void GlobalPreferencesDialog::rebuildExternalTabRows()
{
	m_tabRowOneToPage.clear();
	m_tabRowTwoToPage.clear();
	if (!m_tabs || !m_tabRowOne || !m_tabRowTwo)
		return;

	QSignalBlocker blockOne(m_tabRowOne);
	QSignalBlocker blockTwo(m_tabRowTwo);
	while (m_tabRowOne->count() > 0)
		m_tabRowOne->removeTab(0);
	while (m_tabRowTwo->count() > 0)
		m_tabRowTwo->removeTab(0);

	const int tabCount   = m_tabs->count();
	const int splitIndex = (tabCount + 1) / 2;
	for (int i = 0; i < tabCount; ++i)
	{
		const QString text = m_tabs->tabText(i);
		if (i < splitIndex)
		{
			m_tabRowOne->addTab(text);
			m_tabRowOneToPage.push_back(i);
		}
		else
		{
			m_tabRowTwo->addTab(text);
			m_tabRowTwoToPage.push_back(i);
		}
	}
	syncExternalTabSelection(m_tabs->currentIndex());
}

void GlobalPreferencesDialog::syncExternalTabSelection(const int pageIndex) const
{
	if (!m_tabRowOne || !m_tabRowTwo)
		return;

	QSignalBlocker blockOne(m_tabRowOne);
	QSignalBlocker blockTwo(m_tabRowTwo);
	m_tabRowOne->setCurrentIndex(-1);
	m_tabRowTwo->setCurrentIndex(-1);

	for (int i = 0; i < m_tabRowOneToPage.size(); ++i)
	{
		if (m_tabRowOneToPage.at(i) == pageIndex)
		{
			m_tabRowOne->setCurrentIndex(i);
			updateTabRowVisualState(m_tabRowOne, true);
			updateTabRowVisualState(m_tabRowTwo, false);
			return;
		}
	}
	for (int i = 0; i < m_tabRowTwoToPage.size(); ++i)
	{
		if (m_tabRowTwoToPage.at(i) == pageIndex)
		{
			m_tabRowTwo->setCurrentIndex(i);
			updateTabRowVisualState(m_tabRowOne, false);
			updateTabRowVisualState(m_tabRowTwo, true);
			return;
		}
	}
	updateTabRowVisualState(m_tabRowOne, false);
	updateTabRowVisualState(m_tabRowTwo, false);
}

void GlobalPreferencesDialog::registerCheck(const QString &key, QCheckBox *box)
{
	if (!box)
		return;
	m_intChecks.insert(key, box);
}

void GlobalPreferencesDialog::registerSpin(const QString &key, QSpinBox *spin)
{
	if (!spin)
		return;
	m_intSpins.insert(key, spin);
}

void GlobalPreferencesDialog::registerCombo(const QString &key, QComboBox *combo)
{
	if (!combo)
		return;
	m_intCombos.insert(key, combo);
}

void GlobalPreferencesDialog::registerEdit(const QString &key, QLineEdit *edit)
{
	if (!edit)
		return;
	m_stringEdits.insert(key, edit);
}

void GlobalPreferencesDialog::registerLabel(const QString &key, QLabel *label)
{
	if (!label)
		return;
	m_stringLabels.insert(key, label);
}

void GlobalPreferencesDialog::loadPreferences()
{
	AppController *app = AppController::instance();
	if (!app)
		return;

	for (auto it = m_intChecks.constBegin(); it != m_intChecks.constEnd(); ++it)
		it.value()->setChecked(app->getGlobalOption(it.key()).toInt() != 0);

	for (auto it = m_intSpins.constBegin(); it != m_intSpins.constEnd(); ++it)
		it.value()->setValue(app->getGlobalOption(it.key()).toInt());

	for (auto it = m_intCombos.constBegin(); it != m_intCombos.constEnd(); ++it)
	{
		const int value = app->getGlobalOption(it.key()).toInt();
		const int index = it.value()->findData(value);
		it.value()->setCurrentIndex(index >= 0 ? index : 0);
	}

	for (auto it = m_stringEdits.constBegin(); it != m_stringEdits.constEnd(); ++it)
		it.value()->setText(app->getGlobalOption(it.key()).toString());

	for (auto it = m_stringLabels.constBegin(); it != m_stringLabels.constEnd(); ++it)
		it.value()->setText(app->getGlobalOption(it.key()).toString());

	const QString worldList = app->getGlobalOption(QStringLiteral("WorldList")).toString();
	setListFromStrings(m_worldList, canonicalPathList(worldList.split('*', Qt::SkipEmptyParts)));
	syncWorldListSelection();

	const QString pluginList = app->getGlobalOption(QStringLiteral("PluginList")).toString();
	setListFromStrings(m_pluginsList, canonicalPathList(pluginList.split('*', Qt::SkipEmptyParts)));
	syncPluginListSelection();

	m_worldDefaultDir->setText(
	    runtimePath(app->getGlobalOption(QStringLiteral("DefaultWorldFileDirectory")).toString()));
	m_logDefaultDirLabel->setText(
	    runtimePath(app->getGlobalOption(QStringLiteral("DefaultLogFileDirectory")).toString()));
	m_pluginsDefaultDir->setText(
	    runtimePath(app->getGlobalOption(QStringLiteral("PluginsDirectory")).toString()));

	if (m_printerFontLabel)
		m_printerFontLabel->setText(app->getGlobalOption(QStringLiteral("PrinterFont")).toString());
	m_printerFontSize = app->getGlobalOption(QStringLiteral("PrinterFontSize")).toInt();

	m_outputFontHeight = app->getGlobalOption(QStringLiteral("DefaultOutputFontHeight")).toInt();
	m_inputFontHeight  = app->getGlobalOption(QStringLiteral("DefaultInputFontHeight")).toInt();
	m_inputFontWeight  = app->getGlobalOption(QStringLiteral("DefaultInputFontWeight")).toInt();
	m_inputFontItalic  = app->getGlobalOption(QStringLiteral("DefaultInputFontItalic")).toInt();

	if (m_outputFontStyle)
		m_outputFontStyle->setText(fontStyleSummary(m_outputFontHeight, QFont::Normal, false));
	if (m_inputFontStyle)
		m_inputFontStyle->setText(
		    fontStyleSummary(m_inputFontHeight, m_inputFontWeight, m_inputFontItalic != 0));

	const int refreshType = app->getGlobalOption(QStringLiteral("ActivityWindowRefreshType")).toInt();
	if (refreshType == 0)
		m_activityOnNew->setChecked(true);
	else if (refreshType == 1)
		m_activityPeriodic->setChecked(true);
	else
		m_activityBoth->setChecked(true);

	if (m_activityPeriod)
		m_activityPeriod->setValue(
		    app->getGlobalOption(QStringLiteral("ActivityWindowRefreshInterval")).toInt());

	if (m_trayIconGroup)
	{
		const int     trayIcon = app->getGlobalOption(QStringLiteral("Tray Icon")).toInt();
		const QString custom =
		    runtimePath(app->getGlobalOption(QStringLiteral("TrayIconFileName")).toString());
		if (m_customIconLabel)
			m_customIconLabel->setText(custom.isEmpty() ? QStringLiteral("File name") : custom);

		QAbstractButton *selected = nullptr;
		if (trayIcon == 10)
		{
			if (!custom.isEmpty())
				selected = m_trayIconGroup->button(10);
		}
		else
		{
			selected = m_trayIconGroup->button(trayIcon);
		}

		if (!selected)
			selected = m_trayIconGroup->button(0);
		if (selected)
			selected->setChecked(true);
	}

	m_notepadTextColorRef = app->getGlobalOption(QStringLiteral("NotepadTextColour")).toInt();
	m_notepadBackColorRef = app->getGlobalOption(QStringLiteral("NotepadBackColour")).toInt();
	updateSwatchButton(m_notepadTextSwatch, colorFromColorRef(m_notepadTextColorRef));
	updateSwatchButton(m_notepadBackSwatch, colorFromColorRef(m_notepadBackColorRef));

	const int parenFlags = app->getGlobalOption(QStringLiteral("ParenMatchFlags")).toInt();
	m_parenNestBraces->setChecked(parenFlags & 0x0001);
	m_parenSingleQuotes->setChecked(parenFlags & 0x0002);
	m_parenDoubleQuotes->setChecked(parenFlags & 0x0004);
	m_parenSingleEscape->setChecked(parenFlags & 0x0008);
	m_parenDoubleEscape->setChecked(parenFlags & 0x0010);
	m_parenBackslash->setChecked(parenFlags & 0x0020);
	m_parenPercent->setChecked(parenFlags & 0x0040);

	if (m_luaScript)
		m_luaScript->setPlainText(app->getGlobalOption(QStringLiteral("LuaScript")).toString());

	m_updateMechanismAvailable            = AppController::isUpdateMechanismAvailable();
	const QString updateUnavailableReason = AppController::updateMechanismUnavailableReason();
	if (m_autoCheckUpdatesCheck)
		m_autoCheckUpdatesCheck->setToolTip(updateUnavailableReason);
	if (m_updateCheckEveryLabel)
		m_updateCheckEveryLabel->setToolTip(updateUnavailableReason);
	if (m_updateCheckHoursSpin)
		m_updateCheckHoursSpin->setToolTip(updateUnavailableReason);
	if (m_checkNowButton)
		m_checkNowButton->setToolTip(updateUnavailableReason);
	refreshUpdateCheckControlsEnabledState();
}

void GlobalPreferencesDialog::refreshUpdateCheckControlsEnabledState() const
{
	const bool autoCheckEnabled = m_autoCheckUpdatesCheck && m_autoCheckUpdatesCheck->isChecked();
	if (m_autoCheckUpdatesCheck)
		m_autoCheckUpdatesCheck->setEnabled(m_updateMechanismAvailable);
	if (m_updateCheckHoursSpin)
		m_updateCheckHoursSpin->setEnabled(m_updateMechanismAvailable && autoCheckEnabled);
	if (m_updateCheckEveryLabel)
		m_updateCheckEveryLabel->setEnabled(m_updateMechanismAvailable && autoCheckEnabled);
	if (m_checkNowButton)
		m_checkNowButton->setEnabled(m_updateMechanismAvailable);
}

bool GlobalPreferencesDialog::applyPreferences()
{
	AppController *app = AppController::instance();
	if (!app)
		return false;

	if (m_localeEdit)
	{
		const QString locale = m_localeEdit->text().trimmed();
		if (locale.size() < 2 || locale.size() > 3)
		{
			QMessageBox::warning(this, QStringLiteral("Global Preferences"),
			                     QStringLiteral("Locale must be 2 or 3 characters."));
			m_tabs->setCurrentIndex(1); // General
			m_localeEdit->setFocus();
			return false;
		}
	}

	for (auto it = m_intChecks.constBegin(); it != m_intChecks.constEnd(); ++it)
		app->setGlobalOptionInt(it.key(), it.value()->isChecked() ? 1 : 0);

	for (auto it = m_intSpins.constBegin(); it != m_intSpins.constEnd(); ++it)
		app->setGlobalOptionInt(it.key(), it.value()->value());

	for (auto it = m_intCombos.constBegin(); it != m_intCombos.constEnd(); ++it)
		app->setGlobalOptionInt(it.key(), it.value()->currentData().toInt());

	for (auto it = m_stringEdits.constBegin(); it != m_stringEdits.constEnd(); ++it)
		app->setGlobalOptionString(it.key(), it.value()->text());

	for (auto it = m_stringLabels.constBegin(); it != m_stringLabels.constEnd(); ++it)
		app->setGlobalOptionString(it.key(), it.value()->text());

	if (m_worldList)
		app->setGlobalOptionString(QStringLiteral("WorldList"),
		                           listFromWidget(m_worldList).join(QStringLiteral("*")));

	if (m_pluginsList)
		app->setGlobalOptionString(QStringLiteral("PluginList"),
		                           listFromWidget(m_pluginsList).join(QStringLiteral("*")));

	if (m_worldDefaultDir)
		app->setGlobalOptionString(QStringLiteral("DefaultWorldFileDirectory"),
		                           withTrailingSlash(m_worldDefaultDir->text()));
	if (m_logDefaultDirLabel)
		app->setGlobalOptionString(QStringLiteral("DefaultLogFileDirectory"),
		                           withTrailingSlash(m_logDefaultDirLabel->text()));
	if (m_pluginsDefaultDir)
		app->setGlobalOptionString(QStringLiteral("PluginsDirectory"),
		                           withTrailingSlash(m_pluginsDefaultDir->text()));

	if (m_trayIconGroup)
	{
		const int checkedId = m_trayIconGroup->checkedId();
		app->setGlobalOptionInt(QStringLiteral("Tray Icon"), checkedId >= 0 ? checkedId : 0);
	}
	if (m_customIconLabel)
		app->setGlobalOptionString(QStringLiteral("TrayIconFileName"), m_customIconLabel->text());

	int parenFlags = 0;
	if (m_parenNestBraces->isChecked())
		parenFlags |= 0x0001;
	if (m_parenSingleQuotes->isChecked())
		parenFlags |= 0x0002;
	if (m_parenDoubleQuotes->isChecked())
		parenFlags |= 0x0004;
	if (m_parenSingleEscape->isChecked())
		parenFlags |= 0x0008;
	if (m_parenDoubleEscape->isChecked())
		parenFlags |= 0x0010;
	if (m_parenBackslash->isChecked())
		parenFlags |= 0x0020;
	if (m_parenPercent->isChecked())
		parenFlags |= 0x0040;
	app->setGlobalOptionInt(QStringLiteral("ParenMatchFlags"), parenFlags);

	app->setGlobalOptionInt(QStringLiteral("NotepadTextColour"), m_notepadTextColorRef);
	app->setGlobalOptionInt(QStringLiteral("NotepadBackColour"), m_notepadBackColorRef);

	const int refreshType = m_activityOnNew->isChecked() ? 0 : (m_activityPeriodic->isChecked() ? 1 : 2);
	app->setGlobalOptionInt(QStringLiteral("ActivityWindowRefreshType"), refreshType);
	app->setGlobalOptionInt(QStringLiteral("ActivityWindowRefreshInterval"), m_activityPeriod->value());

	app->setGlobalOptionInt(QStringLiteral("PrinterFontSize"), m_printerFontSize);
	app->setGlobalOptionInt(QStringLiteral("DefaultOutputFontHeight"), m_outputFontHeight);
	app->setGlobalOptionInt(QStringLiteral("DefaultInputFontHeight"), m_inputFontHeight);
	app->setGlobalOptionInt(QStringLiteral("DefaultInputFontWeight"), m_inputFontWeight);
	app->setGlobalOptionInt(QStringLiteral("DefaultInputFontItalic"), m_inputFontItalic);

	if (m_luaScript)
		app->setGlobalOptionString(QStringLiteral("LuaScript"), m_luaScript->toPlainText());

	app->applyGlobalPreferences();
	return true;
}

void GlobalPreferencesDialog::syncWorldListSelection() const
{
	if (!m_worldList || !m_worldSelected)
		return;
	if (m_worldCountLabel)
	{
		const int count = m_worldList->count();
		m_worldCountLabel->setText(
		    QStringLiteral("%1 world%2").arg(count).arg(count == 1 ? QString() : QStringLiteral("s")));
	}
	const QListWidgetItem *item = m_worldList->currentItem();
	m_worldSelected->setText(item ? item->text() : QString());
}

void GlobalPreferencesDialog::syncPluginListSelection() const
{
	if (!m_pluginsList || !m_pluginsSelected)
		return;
	if (m_pluginsCountLabel)
	{
		const int count = m_pluginsList->count();
		m_pluginsCountLabel->setText(
		    QStringLiteral("%1 plugin%2").arg(count).arg(count == 1 ? QString() : QStringLiteral("s")));
	}
	const QListWidgetItem *item = m_pluginsList->currentItem();
	m_pluginsSelected->setText(item ? item->text() : QString());
}

void GlobalPreferencesDialog::updateSwatchButton(QPushButton *button, const QColor &color)
{
	if (!button || !color.isValid())
		return;
	button->setStyleSheet(
	    QStringLiteral("background-color: %1; border: 1px solid #666666;").arg(color.name()));
}

QColor GlobalPreferencesDialog::colorFromColorRef(int colorRef)
{
	const int r = colorRef & 0xFF;
	const int g = (colorRef >> 8) & 0xFF;
	const int b = (colorRef >> 16) & 0xFF;
	return {r, g, b};
}

int GlobalPreferencesDialog::colorRefFromColor(const QColor &color)
{
	if (!color.isValid())
		return 0;
	return (color.blue() << 16) | (color.green() << 8) | color.red();
}

QStringList GlobalPreferencesDialog::listFromWidget(const QListWidget *list)
{
	QStringList result;
	if (!list)
		return result;
	for (int i = 0; i < list->count(); ++i)
		result.append(runtimePath(list->item(i)->text()));
	return result;
}

void GlobalPreferencesDialog::setListFromStrings(QListWidget *list, const QStringList &items)
{
	if (!list)
		return;
	list->clear();
	for (const QString &item : items)
		list->addItem(runtimePath(item));
	if (list->count() > 0)
		list->setCurrentRow(0);
}
