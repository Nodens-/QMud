/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_AppControllerFileOpen.cpp
 * Role: Verifies application file-open requests remain inert until startup and cannot escape QMUD_HOME.
 */

#include "AppController.h"
#include "MainFrame.h"
#include "ReloadUtils.h"
#include "WorldChildWindow.h"
#include "WorldRuntime.h"

#include <QCoreApplication>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QMdiArea>
#include <QScopeGuard>
// ReSharper disable once CppUnusedIncludeDirective
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include <filesystem>
#include <system_error>
#include <utility>

namespace
{
	/** @brief Converts a Qt native path without losing platform filename characters. */
	std::filesystem::path filesystemPath(const QString &path)
	{
#ifdef Q_OS_WIN
		return {path.toStdWString()};
#else
		return {QFile::encodeName(path).constData()};
#endif
	}
} // namespace

class tst_AppControllerFileOpen final : public QObject
{
		Q_OBJECT

	private:
		/** @brief Writes a small text fixture at the requested path. */
		static bool        writeFile(const QString &path, const QByteArray &contents);
		/** @brief Returns text-document paths in MDI creation order. */
		static QStringList openTextDocumentPaths(const MainWindow &window);
		/** @brief Installs the one preference needed to keep startup finalization non-interactive. */
		static bool        disableStartupTip(AppController &controller);
		/** @brief Releases the test-owned preferences connection. */
		static void        releasePreferencesConnection(AppController &controller);
		/** @brief Replaces the persisted recent-file list beneath one isolated QMud home. */
		static void        writeRecentFiles(const QString &home, const QStringList &files);
		/** @brief Reads non-empty recent-file entries from one isolated QMud home. */
		static QStringList readRecentFiles(const QString &home);

	private slots:
		/** @brief Verifies real startup finalization drains valid requests in arrival order. */
		static void startupFinalizationDrainsContainedRequestsInOrder();
		/** @brief Verifies ready controllers immediately open contained local files. */
		static void readyControllerDispatchesContainedFileImmediately();
		/** @brief Verifies association dispatch preserves exact native filenames. */
		static void exactNativeFilenamesArePreserved();
		/** @brief Verifies local requests outside QMUD_HOME and invalid targets are rejected. */
		static void invalidOrEscapingLocalFilesAreRejected();
		/** @brief Verifies a real filesystem symlink cannot escape QMUD_HOME. */
		static void symlinkEscapesAreRejected();
		/** @brief Verifies non-local URLs are not treated as document paths. */
		static void nonLocalUrlsAreIgnored();
		/** @brief Verifies direct user and file-association opens update and deduplicate MRU entries. */
		static void userAndFileAssociationOpensUpdateRecentFiles();
		/** @brief Verifies programmatic, automatic-startup, and reload-recovery opens preserve MRU entries. */
		static void automaticAndProgrammaticOpensPreserveRecentFiles();
		/** @brief Verifies execution and per-open overrides block only preference-driven automatic connection. */
		static void autoConnectOverridesPreservePreference();
};

bool tst_AppControllerFileOpen::writeFile(const QString &path, const QByteArray &contents)
{
	QFile file(path);
	return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(contents) == contents.size();
}

QStringList tst_AppControllerFileOpen::openTextDocumentPaths(const MainWindow &window)
{
	QStringList paths;
	const auto *mdiArea = window.findChild<QMdiArea *>();
	if (!mdiArea)
		return paths;
	for (QMdiSubWindow *subWindow : mdiArea->subWindowList(QMdiArea::CreationOrder))
	{
		if (const auto *textWindow = qobject_cast<TextChildWindow *>(subWindow))
			paths.push_back(textWindow->filePath());
	}
	return paths;
}

bool tst_AppControllerFileOpen::disableStartupTip(AppController &controller)
{
	controller.m_dbConnectionName =
	    QStringLiteral("tst_AppControllerFileOpen_%1").arg(reinterpret_cast<quintptr>(&controller));
	controller.m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), controller.m_dbConnectionName);
	controller.m_db.setDatabaseName(QStringLiteral(":memory:"));
	if (!controller.m_db.open())
		return false;
	QSqlQuery query(controller.m_db);
	return query.exec(QStringLiteral("CREATE TABLE control (name TEXT PRIMARY KEY, value INT NOT NULL)")) &&
	       query.exec(QStringLiteral("INSERT INTO control (name, value) VALUES ('Tip_StartUp', 1)"));
}

void tst_AppControllerFileOpen::releasePreferencesConnection(AppController &controller)
{
	const QString connectionName = controller.m_dbConnectionName;
	controller.m_db.close();
	controller.m_db = QSqlDatabase();
	controller.m_dbConnectionName.clear();
	QSqlDatabase::removeDatabase(connectionName);
}

void tst_AppControllerFileOpen::writeRecentFiles(const QString &home, const QStringList &files)
{
	QSettings settings(QDir(home).filePath(QStringLiteral("QMud.conf")), QSettings::IniFormat);
	settings.beginGroup(QStringLiteral("Recent File List"));
	for (int i = 1; i <= 4; ++i)
	{
		settings.setValue(QStringLiteral("File%1").arg(i), i <= files.size() ? files.at(i - 1) : QString());
	}
	settings.endGroup();
	settings.sync();
}

QStringList tst_AppControllerFileOpen::readRecentFiles(const QString &home)
{
	QSettings settings(QDir(home).filePath(QStringLiteral("QMud.conf")), QSettings::IniFormat);
	settings.sync();
	settings.beginGroup(QStringLiteral("Recent File List"));
	QStringList files;
	for (int i = 1; i <= 4; ++i)
	{
		if (const QString path = settings.value(QStringLiteral("File%1").arg(i)).toString(); !path.isEmpty())
			files.push_back(path);
	}
	settings.endGroup();
	return files;
}

void tst_AppControllerFileOpen::startupFinalizationDrainsContainedRequestsInOrder()
{
	QTemporaryDir directory(QDir::current().filePath(QStringLiteral("tst_AppControllerFileOpen-XXXXXX")));
	QVERIFY(directory.isValid());
	const QString home       = QDir(directory.path()).filePath(QStringLiteral("home"));
	const QString outside    = QDir(directory.path()).filePath(QStringLiteral("outside.txt"));
	const QString firstPath  = QDir(home).filePath(QStringLiteral("first.txt"));
	const QString secondPath = QDir(home).filePath(QStringLiteral("second.txt"));
	QVERIFY(QDir().mkpath(home));
	QVERIFY(writeFile(firstPath, QByteArrayLiteral("first")));
	QVERIFY(writeFile(secondPath, QByteArrayLiteral("second")));
	QVERIFY(writeFile(outside, QByteArrayLiteral("outside")));

	AppController controller;
	MainWindow    window;
	controller.setMainWindow(&window);
	controller.m_workingDir       = home + QLatin1Char('/');
	const bool startupTipDisabled = disableStartupTip(controller);
	const auto releaseConnection  = qScopeGuard([&controller] { releasePreferencesConnection(controller); });
	QVERIFY(startupTipDisabled);

	QFileOpenEvent firstRequest(firstPath);
	QFileOpenEvent outsideRequest(outside);
	QFileOpenEvent secondRequest(QUrl::fromLocalFile(secondPath));
	QCoreApplication::sendEvent(QCoreApplication::instance(), &firstRequest);
	QCoreApplication::sendEvent(QCoreApplication::instance(), &outsideRequest);
	QCoreApplication::sendEvent(QCoreApplication::instance(), &secondRequest);
	QCOMPARE(controller.m_pendingFileAssociationRequests.size(), 3);
	QCOMPARE(controller.m_pendingFileAssociationRequests.at(0).path, firstPath);
	QCOMPARE(controller.m_pendingFileAssociationRequests.at(1).path, outside);
	QCOMPARE(controller.m_pendingFileAssociationRequests.at(2).path, secondPath);
	for (const auto &request : std::as_const(controller.m_pendingFileAssociationRequests))
		QVERIFY(!request.suppressAutoConnect);
	QVERIFY(openTextDocumentPaths(window).isEmpty());

	controller.m_splashMinDelayElapsed = true;
	controller.m_initializeFinished    = true;
	controller.m_initializeSucceeded   = true;
	controller.finalizeStartupIfReady();

	QVERIFY(controller.m_startupFinalized);
	QVERIFY(controller.m_fileAssociationDispatchReady);
	QVERIFY(controller.m_pendingFileAssociationRequests.isEmpty());
	QCOMPARE(openTextDocumentPaths(window), QStringList({QFileInfo(firstPath).canonicalFilePath(),
	                                                     QFileInfo(secondPath).canonicalFilePath()}));
}

void tst_AppControllerFileOpen::readyControllerDispatchesContainedFileImmediately()
{
	QTemporaryDir directory(QDir::current().filePath(QStringLiteral("tst_AppControllerFileOpen-XXXXXX")));
	QVERIFY(directory.isValid());
	const QString home = QDir(directory.path()).filePath(QStringLiteral("home"));
	const QString path = QDir(home).filePath(QStringLiteral("immediate.txt"));
	QVERIFY(QDir().mkpath(home));
	QVERIFY(writeFile(path, QByteArrayLiteral("immediate")));

	AppController controller;
	MainWindow    window;
	controller.setMainWindow(&window);
	controller.m_workingDir = home + QLatin1Char('/');
	controller.enableFileAssociationDispatch();

	QFileOpenEvent request(QUrl::fromLocalFile(path));
	request.setAccepted(false);
	QCoreApplication::sendEvent(QCoreApplication::instance(), &request);
	QVERIFY(request.isAccepted());
	QCOMPARE(openTextDocumentPaths(window), QStringList({QFileInfo(path).canonicalFilePath()}));
}

void tst_AppControllerFileOpen::exactNativeFilenamesArePreserved()
{
#ifdef Q_OS_WIN
	QSKIP("Windows does not permit the POSIX filename characters exercised by this test.");
#else
	QTemporaryDir directory(QDir::current().filePath(QStringLiteral("tst_AppControllerFileOpen-XXXXXX")));
	QVERIFY(directory.isValid());
	const QString home           = QDir(directory.path()).filePath(QStringLiteral("home"));
	const QString whitespacePath = QDir(home).filePath(QStringLiteral(" exact name.txt "));
	const QString backslashPath  = QDir(home).filePath(QStringLiteral(R"(literal\name.txt)"));
	QVERIFY(QDir().mkpath(home));
	QVERIFY(writeFile(whitespacePath, QByteArrayLiteral("whitespace")));
	QVERIFY(writeFile(backslashPath, QByteArrayLiteral("backslash")));

	AppController controller;
	MainWindow    window;
	controller.setMainWindow(&window);
	controller.m_workingDir = home + QLatin1Char('/');
	controller.enableFileAssociationDispatch();

	QVERIFY(controller.handleFileAssociationRequest(whitespacePath));
	QVERIFY(controller.handleFileAssociationRequest(backslashPath));
	QCOMPARE(openTextDocumentPaths(window), QStringList({QFileInfo(whitespacePath).canonicalFilePath(),
	                                                     QFileInfo(backslashPath).canonicalFilePath()}));
#endif
}

void tst_AppControllerFileOpen::invalidOrEscapingLocalFilesAreRejected()
{
	QTemporaryDir directory(QDir::current().filePath(QStringLiteral("tst_AppControllerFileOpen-XXXXXX")));
	QVERIFY(directory.isValid());
	const QString home          = QDir(directory.path()).filePath(QStringLiteral("home"));
	const QString sibling       = QDir(directory.path()).filePath(QStringLiteral("home-sibling"));
	const QString outside       = QDir(directory.path()).filePath(QStringLiteral("outside.txt"));
	const QString siblingFile   = QDir(sibling).filePath(QStringLiteral("sibling.txt"));
	const QString missing       = QDir(home).filePath(QStringLiteral("missing.txt"));
	const QString directoryPath = QDir(home).filePath(QStringLiteral("directory.txt"));
	const QString traversalFile = QDir(home).filePath(QStringLiteral("nested/inside.txt"));
	QVERIFY(QDir().mkpath(home));
	QVERIFY(QDir().mkpath(sibling));
	QVERIFY(QDir().mkpath(directoryPath));
	QVERIFY(QDir().mkpath(QFileInfo(traversalFile).absolutePath()));
	QVERIFY(writeFile(outside, QByteArrayLiteral("outside")));
	QVERIFY(writeFile(siblingFile, QByteArrayLiteral("sibling")));
	QVERIFY(writeFile(traversalFile, QByteArrayLiteral("inside")));

	AppController controller;
	MainWindow    window;
	controller.setMainWindow(&window);
	controller.m_workingDir = home + QLatin1Char('/');
	controller.enableFileAssociationDispatch();

	QStringList rejectedPaths = {outside, siblingFile, missing, directoryPath,
	                             QDir(home).filePath(QStringLiteral("nested/../nested/inside.txt"))};

	for (const QString &path : std::as_const(rejectedPaths))
	{
		QFileOpenEvent request(path);
		request.setAccepted(false);
		QCoreApplication::sendEvent(QCoreApplication::instance(), &request);
		QVERIFY2(!request.isAccepted(), qPrintable(path));
	}
	QVERIFY(openTextDocumentPaths(window).isEmpty());
}

void tst_AppControllerFileOpen::symlinkEscapesAreRejected()
{
	QTemporaryDir directory(QDir::current().filePath(QStringLiteral("tst_AppControllerFileOpen-XXXXXX")));
	QVERIFY(directory.isValid());
	const QString home       = QDir(directory.path()).filePath(QStringLiteral("home"));
	const QString outside    = QDir(directory.path()).filePath(QStringLiteral("outside.txt"));
	const QString escapeLink = QDir(home).filePath(QStringLiteral("escape.txt"));
	QVERIFY(QDir().mkpath(home));
	QVERIFY(writeFile(outside, QByteArrayLiteral("outside")));

	std::error_code linkError;
	std::filesystem::create_symlink(filesystemPath(outside), filesystemPath(escapeLink), linkError);
	if (linkError)
	{
		QSKIP(qPrintable(QStringLiteral("File symlinks unavailable: %1")
		                     .arg(QString::fromStdString(linkError.message()))));
	}
	QVERIFY(QFileInfo(escapeLink).isSymLink());

	AppController controller;
	MainWindow    window;
	controller.setMainWindow(&window);
	controller.m_workingDir = home + QLatin1Char('/');
	controller.enableFileAssociationDispatch();

	QFileOpenEvent request(escapeLink);
	request.setAccepted(false);
	QCoreApplication::sendEvent(QCoreApplication::instance(), &request);
	QVERIFY(!request.isAccepted());
	QVERIFY(openTextDocumentPaths(window).isEmpty());
}

void tst_AppControllerFileOpen::nonLocalUrlsAreIgnored()
{
	AppController  controller;
	QFileOpenEvent request(QUrl(QStringLiteral("https://example.com/world.qdl")));
	request.setAccepted(false);
	QCoreApplication::sendEvent(QCoreApplication::instance(), &request);
	QVERIFY(!request.isAccepted());
	QVERIFY(controller.m_pendingFileAssociationRequests.isEmpty());
}

void tst_AppControllerFileOpen::userAndFileAssociationOpensUpdateRecentFiles()
{
	QTemporaryDir directory(QDir::current().filePath(QStringLiteral("tst_AppControllerFileOpen-XXXXXX")));
	QVERIFY(directory.isValid());
	const QString home            = QDir(directory.path()).filePath(QStringLiteral("home"));
	const QString firstPath       = QDir(home).filePath(QStringLiteral("first.txt"));
	const QString secondPath      = QDir(home).filePath(QStringLiteral("second.txt"));
	const QString associationPath = QDir(home).filePath(QStringLiteral("association.txt"));
	QVERIFY(QDir().mkpath(home));
	QVERIFY(writeFile(firstPath, QByteArrayLiteral("first")));
	QVERIFY(writeFile(secondPath, QByteArrayLiteral("second")));
	QVERIFY(writeFile(associationPath, QByteArrayLiteral("association")));

	AppController controller;
	MainWindow    window;
	controller.setMainWindow(&window);
	controller.m_workingDir = home + QLatin1Char('/');

	QVERIFY(controller.openDocumentFileFromUserAction(firstPath));
	QVERIFY(controller.openDocumentFileFromUserAction(secondPath));
	QVERIFY(controller.openDocumentFileFromUserAction(firstPath));
	QCOMPARE(readRecentFiles(home),
	         QStringList({QStringLiteral("./first.txt"), QStringLiteral("./second.txt")}));

	controller.enableFileAssociationDispatch();
	QVERIFY(controller.handleFileAssociationRequest(associationPath));
	QCOMPARE(readRecentFiles(home),
	         QStringList({QStringLiteral("./association.txt"), QStringLiteral("./first.txt"),
	                      QStringLiteral("./second.txt")}));
}

void tst_AppControllerFileOpen::automaticAndProgrammaticOpensPreserveRecentFiles()
{
	QTemporaryDir directory(QDir::current().filePath(QStringLiteral("tst_AppControllerFileOpen-XXXXXX")));
	QVERIFY(directory.isValid());
	const QString home             = QDir(directory.path()).filePath(QStringLiteral("home"));
	const QString programmaticPath = QDir(home).filePath(QStringLiteral("programmatic.txt"));
	const QString automaticOnePath = QDir(home).filePath(QStringLiteral("automatic-one.txt"));
	const QString automaticTwoPath = QDir(home).filePath(QStringLiteral("automatic-two.txt"));
	const QString recoveredPath    = QDir(home).filePath(QStringLiteral("worlds/recovered.qdl"));
	QVERIFY(QDir().mkpath(QFileInfo(recoveredPath).absolutePath()));
	QVERIFY(writeFile(programmaticPath, QByteArrayLiteral("programmatic")));
	QVERIFY(writeFile(automaticOnePath, QByteArrayLiteral("automatic one")));
	QVERIFY(writeFile(automaticTwoPath, QByteArrayLiteral("automatic two")));
	QVERIFY(writeFile(recoveredPath, QByteArrayLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<qmud>
  <world id="aaaaaaaaaaaaaaaaaaaaaaaa" name="Recovered" site="0.0.0.0" port="4000"/>
</qmud>)xml")));

	const QStringList originalRecentFiles = {
	    QStringLiteral("./recent-one.qdl"), QStringLiteral("./recent-two.qdl"),
	    QStringLiteral("./recent-three.qdl"), QStringLiteral("./recent-four.qdl")};
	writeRecentFiles(home, originalRecentFiles);
	const QString previousCurrentDirectory = QDir::currentPath();
	QVERIFY(QDir::setCurrent(home));
	const auto restoreCurrentDirectory =
	    qScopeGuard([previousCurrentDirectory] { QDir::setCurrent(previousCurrentDirectory); });
	Q_UNUSED(restoreCurrentDirectory);

	AppController controller;
	MainWindow    window;
	controller.setMainWindow(&window);
	controller.m_workingDir = home + QLatin1Char('/');

	QVERIFY(controller.openDocumentFile(programmaticPath));
	controller.openWorldsFromList({automaticOnePath, automaticTwoPath}, false);
	QCOMPARE(readRecentFiles(home), originalRecentFiles);

	controller.m_suppressAutoConnect = true;
	ReloadWorldState recoveredState;
	recoveredState.worldFilePath   = QStringLiteral("./worlds/recovered.qdl");
	WorldRuntime *recoveredRuntime = nullptr;
	QVERIFY(controller.openWorldForReloadRecovery(recoveredState, false, &recoveredRuntime));
	QVERIFY(recoveredRuntime);
	QCOMPARE(readRecentFiles(home), originalRecentFiles);
}

void tst_AppControllerFileOpen::autoConnectOverridesPreservePreference()
{
	{
		AppController controller(true);
		controller.m_globalIntPrefs.insert(QStringLiteral("AutoConnectWorlds"), 1);

		WorldRuntime runtime;
		runtime.setWorldAttribute(QStringLiteral("name"), QStringLiteral("No Auto Connect"));
		runtime.setWorldAttribute(QStringLiteral("site"), QStringLiteral("127.0.0.1"));
		runtime.setWorldAttribute(QStringLiteral("port"), QStringLiteral("1"));

		controller.maybeAutoConnectWorld(&runtime);
		QVERIFY(!runtime.isConnecting());
		QVERIFY(!runtime.isConnected());
		QCOMPARE(controller.getGlobalOption(QStringLiteral("AutoConnectWorlds")).toInt(), 1);
	}

	AppController controller;
	controller.m_globalIntPrefs.insert(QStringLiteral("AutoConnectWorlds"), 1);

	WorldRuntime runtime;
	runtime.setWorldAttribute(QStringLiteral("name"), QStringLiteral("No Auto Connect"));
	runtime.setWorldAttribute(QStringLiteral("site"), QStringLiteral("127.0.0.1"));
	runtime.setWorldAttribute(QStringLiteral("port"), QStringLiteral("1"));

	controller.maybeAutoConnectWorld(&runtime, true);
	QVERIFY(!runtime.isConnecting());
	QVERIFY(!runtime.isConnected());
	QCOMPARE(controller.getGlobalOption(QStringLiteral("AutoConnectWorlds")).toInt(), 1);
}

QTEST_MAIN(tst_AppControllerFileOpen)

#include "tst_AppControllerFileOpen.moc"
