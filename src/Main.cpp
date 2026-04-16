/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: Main.cpp
 * Role: Application entry point that bootstraps Qt, enforces single-instance behavior, and initializes platform
 * startup/runtime state.
 */

#include "AppController.h"
#include "Environment.h"
#include "LuaApiExport.h"
#include "MainFrame.h"
#include "ReloadUtils.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QIcon>
#include <QLibraryInfo>
#include <QLocalServer>
#include <QLocalSocket>
#ifdef Q_OS_WIN
#include <wchar.h>
#include <windows.h>
#endif

namespace
{
#ifdef Q_OS_WIN
#ifdef LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
	constexpr DWORD kLoadLibrarySearchDefaultDirs = LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
#else
	constexpr DWORD kLoadLibrarySearchDefaultDirs = 0x00001000;
#endif
#endif

	QString singleInstanceServerName()
	{
		QString userKey = qEnvironmentVariable("UID");
		if (userKey.isEmpty())
			userKey = qEnvironmentVariable("USER");
		if (userKey.isEmpty())
			userKey = qEnvironmentVariable("USERNAME");
		if (userKey.isEmpty())
			userKey = QStringLiteral("default");
		return QStringLiteral("qmud-single-instance-%1").arg(userKey);
	}

	bool isEnabledValue(const QString &value)
	{
		return value == QStringLiteral("1") || value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
		       value.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0 ||
		       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
	}

#ifdef Q_OS_WIN
	void configureWindowsDllSearchPath()
	{
		wchar_t     exePath[MAX_PATH] = {0};
		const DWORD copied            = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
		if (copied == 0 || copied >= MAX_PATH)
			return;

		wchar_t *slash = wcsrchr(exePath, L'\\');
		if (!slash)
			return;
		*slash = L'\0';

		const QString  libDir     = QString::fromWCharArray(exePath) + QStringLiteral("\\lib");
		const wchar_t *libDirPath = reinterpret_cast<const wchar_t *>(libDir.utf16());

		HMODULE        kernel32 = GetModuleHandleW(L"kernel32.dll");
		if (!kernel32)
			return;

		using SetDefaultDllDirectoriesFn = BOOL(WINAPI *)(DWORD);
		using AddDllDirectoryFn          = DLL_DIRECTORY_COOKIE(WINAPI *)(PCWSTR);
		using SetDllDirectoryWFn         = BOOL(WINAPI *)(LPCWSTR);

		auto setDefaultDllDirectoriesFn = reinterpret_cast<SetDefaultDllDirectoriesFn>(
		    GetProcAddress(kernel32, "SetDefaultDllDirectories"));
		auto addDllDirectoryFn =
		    reinterpret_cast<AddDllDirectoryFn>(GetProcAddress(kernel32, "AddDllDirectory"));
		if (setDefaultDllDirectoriesFn && addDllDirectoryFn)
		{
			setDefaultDllDirectoriesFn(kLoadLibrarySearchDefaultDirs);
			addDllDirectoryFn(libDirPath);
			return;
		}

		auto setDllDirectoryWFn =
		    reinterpret_cast<SetDllDirectoryWFn>(GetProcAddress(kernel32, "SetDllDirectoryW"));
		if (setDllDirectoryWFn)
			setDllDirectoryWFn(libDirPath);
	}
#endif
} // namespace

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
	configureWindowsDllSearchPath();
	if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
		qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("windows:darkmode=2"));
#endif

	if (qEnvironmentVariableIsEmpty("QT_PLUGIN_PATH"))
		qputenv("QT_PLUGIN_PATH", QLibraryInfo::path(QLibraryInfo::PluginsPath).toUtf8());
	if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM_PLUGIN_PATH"))
		qputenv("QT_QPA_PLATFORM_PLUGIN_PATH",
		        (QLibraryInfo::path(QLibraryInfo::PluginsPath) + QStringLiteral("/platforms")).toUtf8());

	QApplication app(argc, argv);
#ifdef Q_OS_WIN
	app.addLibraryPath(QCoreApplication::applicationDirPath() + QStringLiteral("/qtplugins"));
#endif
	QCoreApplication::setApplicationName(QStringLiteral("QMud"));
	QCoreApplication::setOrganizationName(QStringLiteral("QMudOrg"));
	QApplication::setWindowIcon(QIcon(QStringLiteral(":/qmud/res/QMud.png")));

	const QStringList args      = QCoreApplication::arguments();
	bool allowMultipleInstances = isEnabledValue(qEnvironmentVariable("QMUD_ALLOW_MULTI_INSTANCE").trimmed());
	QString    reloadStatePathArg;
	QString    reloadTokenArg;
	const bool reloadLaunchArguments =
	    parseReloadStartupArguments(args, &reloadStatePathArg, &reloadTokenArg);
	for (int i = 1; i < args.size(); ++i)
	{
		const QString arg = args.at(i).trimmed();
		if (arg.compare(QStringLiteral("--multi-instance"), Qt::CaseInsensitive) == 0 ||
		    arg.compare(QStringLiteral("--allow-multi-instance"), Qt::CaseInsensitive) == 0)
		{
			allowMultipleInstances = true;
			continue;
		}
		if (arg.compare(QStringLiteral("--dump-lua-api"), Qt::CaseInsensitive) != 0)
			continue;

		if ((i + 1) >= args.size())
		{
			qCritical() << "--dump-lua-api requires an output directory argument";
			return 1;
		}

		const QString &outputDir = args.at(i + 1);
		QString        errorMessage;
		if (!exportLuaApiInventory(outputDir, &errorMessage))
		{
			qCritical() << "Failed to export Lua API inventory:" << errorMessage;
			return 1;
		}

		qInfo() << "Lua API inventory exported to" << outputDir;
		return 0;
	}

	if (allowMultipleInstances)
	{
		qmudSetEnvironmentConfigFallbackEnabled(false);
		if (qEnvironmentVariable("QMUD_HOME").trimmed().isEmpty())
		{
			qCritical() << "Multi-instance is enabled. QMUD_HOME needs to be set.";
			return 1;
		}
	}

	QLocalServer instanceServer;
	if (!allowMultipleInstances)
	{
		const QString instanceServerName = singleInstanceServerName();
		if (!reloadLaunchArguments)
		{
			QLocalSocket existingInstanceProbe;
			existingInstanceProbe.connectToServer(instanceServerName, QIODevice::WriteOnly);
			if (existingInstanceProbe.waitForConnected(150))
			{
				existingInstanceProbe.write("raise");
				existingInstanceProbe.flush();
				existingInstanceProbe.waitForBytesWritten(100);
				return 0;
			}
		}
		QLocalServer::removeServer(instanceServerName);
		if (!instanceServer.listen(instanceServerName))
			return 0;
	}

	MainWindow *mainWindow = nullptr;
	QObject::connect(&instanceServer, &QLocalServer::newConnection, &app,
	                 [&instanceServer, &mainWindow]()
	                 {
		                 while (QLocalSocket *client = instanceServer.nextPendingConnection())
		                 {
			                 client->readAll();
			                 client->disconnectFromServer();
			                 client->deleteLater();
		                 }

		                 if (!mainWindow)
			                 return;
		                 if (mainWindow->isMinimized())
			                 mainWindow->showNormal();
		                 if (!mainWindow->isVisible())
			                 mainWindow->show();
		                 mainWindow->raise();
		                 mainWindow->activateWindow();
	                 });

	AppController controller;
	MainWindow    w;
	mainWindow = &w;
	w.setWindowIcon(QIcon(QStringLiteral(":/qmud/res/QMud.png")));
	controller.setMainWindow(&w);
	controller.startWithSplash();
	return QCoreApplication::exec();
}
