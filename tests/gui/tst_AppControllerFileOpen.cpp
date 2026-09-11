/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_AppControllerFileOpen.cpp
 * Role: Verifies application file-open requests remain inert until startup and cannot escape QMUD_HOME.
 */

#include "AppController.h"
#include "MainFrame.h"
#include "WorldChildWindow.h"

#include <QCoreApplication>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QMdiArea>
#include <QScopeGuard>
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
	QCOMPARE(controller.m_pendingFileAssociationRequests, QStringList({firstPath, outside, secondPath}));
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

QTEST_MAIN(tst_AppControllerFileOpen)

#include "tst_AppControllerFileOpen.moc"
