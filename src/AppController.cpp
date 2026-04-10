/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: AppController.cpp
 * Role: Central command dispatcher implementation coordinating global actions, dialogs, world lifecycle, and
 * cross-window workflows.
 */

#include "AppController.h"

#include "ActivityDocument.h"
#include "AsciiArt.h"
#include "BraceMatch.h"
#include "ColorPacking.h"
#include "ColorUtils.h"
#include "DocConstants.h"
#include "Environment.h"
#include "FileExtensions.h"
#include "FontUtils.h"
#include "ImportMergeUtils.h"
#include "LuaCallbackEngine.h"
#include "LuaFunctionTypes.h"
#include "MainFrame.h"
#include "MainWindowHost.h"
#include "MainWindowHostResolver.h"
#include "NameGeneration.h"
#include "ReloadPlannerUtils.h"
#include "ReloadRecoveryUtils.h"
#include "ReloadStateUtils.h"
#include "SqliteCompat.h"
#include "UpdateCheckUtils.h"
#include "Version.h"
#include "WorldChildWindow.h"
#include "WorldDocument.h"
#include "WorldOptionDefaults.h"
#include "WorldOptions.h"
#include "WorldPreferencesRoutingUtils.h"
#include "WorldRuntime.h"
#include "WorldSessionRestoreFlowUtils.h"
#include "WorldSessionStateUtils.h"
#include "WorldView.h"
#include "dialogs/ColourPickerDialog.h"
#include "dialogs/ConfirmPreambleDialog.h"
#include "dialogs/GeneratedNameDialog.h"
#include "dialogs/GlobalPreferencesDialog.h"
#include "dialogs/HighlightPhraseDialog.h"
#include "dialogs/ImportXmlDialog.h"
#include "dialogs/LogSessionDialog.h"
#include "dialogs/PluginWizardDialog.h"
#include "dialogs/PluginsDialog.h"
#include "dialogs/SpellCheckDialog.h"
#include "dialogs/TipDialog.h"
#include "dialogs/WelcomeDialog.h"
#include "dialogs/WelcomeUpgradeDialog.h"
#include "dialogs/WorldEditUtils.h"
#include "dialogs/WorldPreferencesDialog.h"
#include "scripting/ScriptingErrors.h"

#if defined(QMUD_ENABLE_LUA_I18N) || defined(QMUD_ENABLE_LUA_SCRIPTING)
#include "LuaSupport.h"

extern "C"
{
	LUALIB_API int luaopen_lsqlite3(lua_State *L);
}
#endif

#include <QAction>
#include <QApplication>
#include <QByteArrayView>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
// ReSharper disable once CppUnusedIncludeDirective
#include <QCryptographicHash>
#include <QDebug>
#include <QDesktopServices>
#include <QDialog>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QHostInfo>
#include <QImageReader>
#include <QInputDialog>
// ReSharper disable once CppUnusedIncludeDirective
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
// ReSharper disable once CppUnusedIncludeDirective
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPageLayout>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPrintDialog>
#include <QPrinter>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScreen>
#include <QSettings>
#include <QSpinBox>
#include <QSplashScreen>
#include <QStandardPaths>
// ReSharper disable once CppUnusedIncludeDirective
#include <QSysInfo>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextStream>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QTranslator>
#include <QUrl>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>
#include <algorithm>
#include <clocale>
#include <cstring>
#include <ctime>
#include <exception>
// ReSharper disable once CppUnusedIncludeDirective
#include <limits>
#include <memory>
#include <vector>
#include <zlib.h>

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

#ifdef Q_OS_WIN
// clang-format off
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
// clang-format on
#endif

#ifdef Q_OS_MACOS
#include <CoreServices/CoreServices.h>
#endif

namespace
{
	bool envFlagEnabled(const char *name)
	{
		const QString value = qmudEnvironmentVariable(QString::fromLatin1(name)).trimmed();
		return value == QStringLiteral("1") || value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
		       value.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0 ||
		       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
	}

	struct FileAssociationEntry
	{
			const char *programIdSuffix;
			const char *description;
			const char *mimeType;
			const char *modernExtension;
			const char *legacyExtension;
	};

	constexpr quint8 kXtermCubeValues[6]         = {0, 95, 135, 175, 215, 255};
	constexpr int    kReloadStateStaleAgeSeconds = 10 * 60;
	constexpr char   kReloadStateArgName[]       = "--reload-state";
	constexpr char   kReloadTokenArgName[]       = "--reload-token";
	constexpr char   kReloadLogTag[]             = "[ReloadQMud]";
	constexpr char   kWorldSessionStateSuffix[]  = ".qws";
	constexpr char   kWorldSessionStateDir[]     = "worlds/state";
	constexpr char   kUpdateLatestReleaseUrl[] = "https://api.github.com/repos/Nodens-/QMud/releases/latest";

	using UpdateInstallTarget = QMudUpdateCheck::InstallTarget;
	using QMudUpdateCheck::compareVersions;
	using QMudUpdateCheck::normalizeSha256Digest;
	using QMudUpdateCheck::versionCore;

	UpdateInstallTarget detectUpdateInstallTarget()
	{
		const auto updateDisabledByEnvironment = []() -> bool
		{
			if (!qmudEnvironmentVariableIsSet(QStringLiteral("QMUD_DISABLE_UPDATE")))
				return false;
			const QString value =
			    qmudEnvironmentVariable(QStringLiteral("QMUD_DISABLE_UPDATE")).trimmed().toLower();
			if (value.isEmpty())
				return true;
			return value != QStringLiteral("0") && value != QStringLiteral("false") &&
			       value != QStringLiteral("no") && value != QStringLiteral("off") &&
			       value != QStringLiteral("n");
		};
		if (updateDisabledByEnvironment())
			return UpdateInstallTarget::Unsupported;
#ifdef Q_OS_MACOS
		return UpdateInstallTarget::MacBundle;
#elif defined(Q_OS_WIN)
		return UpdateInstallTarget::WindowsInstaller;
#elif defined(Q_OS_LINUX)
		return qEnvironmentVariable("APPIMAGE").trimmed().isEmpty() ? UpdateInstallTarget::Unsupported
		                                                            : UpdateInstallTarget::LinuxAppImage;
#else
		return UpdateInstallTarget::Unsupported;
#endif
	}

	QString updateNotSupportedMessage()
	{
		const auto updateDisabledByEnvironment = []() -> bool
		{
			if (!qmudEnvironmentVariableIsSet(QStringLiteral("QMUD_DISABLE_UPDATE")))
				return false;
			const QString value =
			    qmudEnvironmentVariable(QStringLiteral("QMUD_DISABLE_UPDATE")).trimmed().toLower();
			if (value.isEmpty())
				return true;
			return value != QStringLiteral("0") && value != QStringLiteral("false") &&
			       value != QStringLiteral("no") && value != QStringLiteral("off") &&
			       value != QStringLiteral("n");
		};
		if (updateDisabledByEnvironment())
		{
			return QStringLiteral(
			    "Automatic updates are disabled by QMUD_DISABLE_UPDATE (environment/system config).");
		}
#ifdef Q_OS_WIN
		return QStringLiteral("Automatic updates are not supported on this platform.");
#elif defined(Q_OS_LINUX)
		return QStringLiteral("Automatic updates are available only for AppImage builds on Linux.");
#else
		return QStringLiteral("Automatic updates are not supported on this platform.");
#endif
	}

	bool computeFileSha256(const QString &filePath, QString *sha256Hex, QString *errorMessage)
	{
		if (errorMessage)
			errorMessage->clear();
		if (sha256Hex)
			sha256Hex->clear();
		QFile file(filePath);
		if (!file.open(QIODevice::ReadOnly))
		{
			if (errorMessage)
				*errorMessage =
				    QStringLiteral("Unable to open %1 for checksum: %2").arg(filePath, file.errorString());
			return false;
		}
		QCryptographicHash hash(QCryptographicHash::Sha256);
		while (!file.atEnd())
		{
			const QByteArray chunk = file.read(1024 * 1024);
			if (chunk.isEmpty() && file.error() != QFile::NoError)
			{
				if (errorMessage)
				{
					*errorMessage = QStringLiteral("Failed to read %1 for checksum: %2")
					                    .arg(filePath, file.errorString());
				}
				return false;
			}
			hash.addData(chunk);
		}
		const QString digest = QString::fromLatin1(hash.result().toHex());
		if (sha256Hex)
			*sha256Hex = digest;
		return true;
	}

	bool removePathIfExists(const QString &path, QString *errorMessage)
	{
		if (errorMessage)
			errorMessage->clear();
		if (path.trimmed().isEmpty())
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Path is empty.");
			return false;
		}

		const QFileInfo info(path);
		if (!info.exists() && !info.isSymLink())
			return true;

		if (info.isDir() && !info.isSymLink())
		{
			QDir dir(path);
			if (!dir.removeRecursively())
			{
				if (errorMessage)
					*errorMessage = QStringLiteral("Failed to remove directory: %1").arg(path);
				return false;
			}
			return true;
		}

		if (!QFile::remove(path))
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Failed to remove file: %1").arg(path);
			return false;
		}
		return true;
	}

	bool replaceFileWithDownloadedPayload(const QString &downloadedFilePath, const QString &destinationPath,
	                                      const QFileDevice::Permissions destinationPermissions,
	                                      QString                       *errorMessage)
	{
		if (errorMessage)
			errorMessage->clear();
		const QFileInfo sourceInfo(downloadedFilePath);
		if (!sourceInfo.exists() || !sourceInfo.isFile())
		{
			if (errorMessage)
			{
				*errorMessage =
				    QStringLiteral("Downloaded package file is missing: %1").arg(downloadedFilePath);
			}
			return false;
		}

		const QFileInfo destinationInfo(destinationPath);
		const QString   destinationDir = destinationInfo.absolutePath();
		if (!QDir().mkpath(destinationDir))
		{
			if (errorMessage)
				*errorMessage =
				    QStringLiteral("Unable to create destination directory: %1").arg(destinationDir);
			return false;
		}

		const QString stagedPath = QDir(destinationDir)
		                               .filePath(QStringLiteral(".qmud.update.%1.%2")
		                                             .arg(QCoreApplication::applicationPid())
		                                             .arg(destinationInfo.fileName()));
		const QString backupPath =
		    QDir(destinationDir)
		        .filePath(QStringLiteral(".qmud.update.backup.%1").arg(destinationInfo.fileName()));
		QString ignoredError;
		(void)removePathIfExists(stagedPath, &ignoredError);
		(void)removePathIfExists(backupPath, &ignoredError);

		if (!QFile::copy(downloadedFilePath, stagedPath))
		{
			if (errorMessage)
			{
				*errorMessage = QStringLiteral("Failed to stage update payload from %1 to %2.")
				                    .arg(downloadedFilePath, stagedPath);
			}
			return false;
		}

		QFileDevice::Permissions finalPermissions = destinationPermissions;
		if (finalPermissions == QFileDevice::Permissions())
			finalPermissions = QFile::permissions(downloadedFilePath);
		if (finalPermissions != QFileDevice::Permissions())
			QFile::setPermissions(stagedPath, finalPermissions);

		const bool hadExistingDestination = QFileInfo::exists(destinationPath);
		if (hadExistingDestination && !QFile::rename(destinationPath, backupPath))
		{
			(void)removePathIfExists(stagedPath, &ignoredError);
			if (errorMessage)
				*errorMessage =
				    QStringLiteral("Failed to stage existing file for replacement: %1").arg(destinationPath);
			return false;
		}

		if (!QFile::rename(stagedPath, destinationPath))
		{
			if (!QFile::copy(stagedPath, destinationPath))
			{
				(void)removePathIfExists(stagedPath, &ignoredError);
				if (hadExistingDestination)
					(void)QFile::rename(backupPath, destinationPath);
				if (errorMessage)
					*errorMessage = QStringLiteral("Failed to place updated file: %1").arg(destinationPath);
				return false;
			}
			QFile::remove(stagedPath);
		}
		if (hadExistingDestination)
			(void)removePathIfExists(backupPath, &ignoredError);
		return true;
	}

	QString findContainingAppBundlePath()
	{
		QDir currentDir = QFileInfo(QCoreApplication::applicationFilePath()).absoluteDir();
		while (true)
		{
			if (currentDir.dirName().endsWith(QStringLiteral(".app"), Qt::CaseInsensitive))
				return currentDir.absolutePath();
			if (!currentDir.cdUp())
				return {};
		}
	}

	QString findExtractedAppBundlePath(const QString &rootPath)
	{
		const QFileInfo rootInfo(rootPath);
		if (rootInfo.exists() && rootInfo.isDir() &&
		    rootInfo.fileName().endsWith(QStringLiteral(".app"), Qt::CaseInsensitive))
		{
			return rootInfo.absoluteFilePath();
		}

		QString      firstMatch;
		QDirIterator it(rootPath, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
		while (it.hasNext())
		{
			const QString bundlePath = it.next();
			const QString bundleName = QFileInfo(bundlePath).fileName();
			if (!bundleName.endsWith(QStringLiteral(".app"), Qt::CaseInsensitive))
				continue;
			if (bundleName.compare(QStringLiteral("QMud.app"), Qt::CaseInsensitive) == 0)
				return bundlePath;
			if (firstMatch.isEmpty())
				firstMatch = bundlePath;
		}
		return firstMatch;
	}

	bool runUpdateHelperCommand(const QString &program, const QStringList &arguments, const int timeoutMs,
	                            QString *errorMessage)
	{
		if (errorMessage)
			errorMessage->clear();
		QProcess process;
		process.start(program, arguments);
		if (!process.waitForStarted(10000))
		{
			if (errorMessage)
			{
				*errorMessage = QStringLiteral("Failed to start helper command %1: %2")
				                    .arg(program, process.errorString());
			}
			return false;
		}
		if (!process.waitForFinished(timeoutMs))
		{
			process.kill();
			if (errorMessage)
				*errorMessage = QStringLiteral("Helper command timed out: %1").arg(program);
			return false;
		}
		if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
		{
			const QString stderrText = QString::fromUtf8(process.readAllStandardError()).trimmed();
			const QString stdoutText = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
			QString       detail     = !stderrText.isEmpty() ? stderrText : stdoutText;
			if (detail.isEmpty())
				detail = QStringLiteral("exit code %1").arg(process.exitCode());
			if (errorMessage)
				*errorMessage = QStringLiteral("Helper command failed (%1): %2").arg(program, detail);
			return false;
		}
		return true;
	}

	const QList<FileAssociationEntry> &fileAssociationEntries()
	{
		static const QList<FileAssociationEntry> kEntries = {
		    {"World",     "QMud World File",    "application/x-qmud-world",     "qdl", "mcl"},
		    {"Triggers",  "QMud Trigger File",  "application/x-qmud-triggers",  "qdt", "mct"},
		    {"Aliases",   "QMud Alias File",    "application/x-qmud-aliases",   "qda", "mca"},
		    {"Timers",    "QMud Timer File",    "application/x-qmud-timers",    "qdi", "mci"},
		    {"Colours",   "QMud Colour File",   "application/x-qmud-colours",   "qdc", "mcc"},
		    {"Macros",    "QMud Macro File",    "application/x-qmud-macros",    "qdm", "mcm"},
		    {"Variables", "QMud Variable File", "application/x-qmud-variables", "qdv", "mcv"},
		};
		return kEntries;
	}

	QString makeReloadArgument(const QString &name, const QString &value)
	{
		return value.isEmpty() ? QString() : (name + QLatin1Char('=') + value);
	}

	QString generateReloadToken()
	{
		const quint64 high = QRandomGenerator::global()->generate64();
		const quint64 low  = QRandomGenerator::global()->generate64();
		return QStringLiteral("%1%2").arg(high, 16, 16, QLatin1Char('0')).arg(low, 16, 16, QLatin1Char('0'));
	}

	QString reloadWorldIdentity(const ReloadWorldState &worldState)
	{
		QString       displayName = worldState.displayName.trimmed();
		const QString worldId     = worldState.worldId.trimmed();
		if (!displayName.isEmpty() && !worldId.isEmpty())
			return QStringLiteral("%1 (id=%2)").arg(displayName, worldId);
		if (!displayName.isEmpty())
			return displayName;
		if (!worldId.isEmpty())
			return QStringLiteral("id=%1").arg(worldId);
		return QStringLiteral("<unnamed>");
	}

	void printReloadInfoToStdout(const QString &message)
	{
		QTextStream out(stdout);
		out << kReloadLogTag << ' ' << message << Qt::endl;
	}

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
	bool setSocketDescriptorInheritable(const int descriptor, const bool inheritable, QString *errorMessage)
	{
		if (errorMessage)
			errorMessage->clear();
		if (descriptor < 0)
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Descriptor is invalid.");
			return false;
		}

		constexpr int maxRetryableRetries = 8;

		int           flags                 = -1;
		int           getFdRetryableRetries = 0;
		for (;;)
		{
			errno = 0;
			flags = fcntl(descriptor, F_GETFD);
			if (flags >= 0)
				break;
			if (errno == EINTR || errno == EAGAIN)
			{
				++getFdRetryableRetries;
				if (getFdRetryableRetries <= maxRetryableRetries)
					continue;
				if (errorMessage)
				{
					*errorMessage =
					    QStringLiteral("fcntl(F_GETFD) retryable failure persisted for descriptor %1: %2")
					        .arg(descriptor)
					        .arg(QString::fromLocal8Bit(strerror(errno)));
				}
				return false;
			}
			if (errorMessage)
				*errorMessage =
				    QStringLiteral("fcntl(F_GETFD) failed: %1").arg(QString::fromLocal8Bit(strerror(errno)));
			return false;
		}

		int nextFlags = flags;
		if (inheritable)
			nextFlags &= ~FD_CLOEXEC;
		else
			nextFlags |= FD_CLOEXEC;
		if (nextFlags == flags)
			return true;

		int setFdRetryableRetries = 0;
		for (;;)
		{
			errno = 0;
			if (fcntl(descriptor, F_SETFD, nextFlags) == 0)
				return true;
			if (errno == EINTR || errno == EAGAIN)
			{
				++setFdRetryableRetries;
				if (setFdRetryableRetries <= maxRetryableRetries)
					continue;
				if (errorMessage)
				{
					*errorMessage =
					    QStringLiteral("fcntl(F_SETFD) retryable failure persisted for descriptor %1: %2")
					        .arg(descriptor)
					        .arg(QString::fromLocal8Bit(strerror(errno)));
				}
				return false;
			}
			if (errorMessage)
				*errorMessage =
				    QStringLiteral("fcntl(F_SETFD) failed: %1").arg(QString::fromLocal8Bit(strerror(errno)));
			return false;
		}
	}

	bool closeSocketDescriptorIfOpen(const int descriptor, QString *errorMessage)
	{
		if (errorMessage)
			errorMessage->clear();
		if (descriptor < 0)
			return true;
		errno = 0;
		if (fcntl(descriptor, F_GETFD) < 0)
		{
			if (errno == EBADF)
				return true;
			if (errorMessage)
				*errorMessage = QStringLiteral("fcntl(F_GETFD) before close failed: %1")
				                    .arg(QString::fromLocal8Bit(strerror(errno)));
			return false;
		}
		if (close(descriptor) == 0)
			return true;
		if (errorMessage)
			*errorMessage = QStringLiteral("close(%1) failed: %2")
			                    .arg(descriptor)
			                    .arg(QString::fromLocal8Bit(strerror(errno)));
		return false;
	}
#endif

#ifdef Q_OS_LINUX
	QStringList registeredMimeTypes()
	{
		QStringList                        mimeTypes;
		const QList<FileAssociationEntry> &entries = fileAssociationEntries();
		mimeTypes.reserve(entries.size());
		for (const FileAssociationEntry &entry : entries)
			mimeTypes.push_back(QString::fromLatin1(entry.mimeType));
		return mimeTypes;
	}
#endif

#ifdef Q_OS_WIN
	bool writeRegistryStringValue(const QString &subKey, const QString &valueName, const QString &value)
	{
		HKEY          key        = nullptr;
		const QString fullSubKey = QStringLiteral("Software\\Classes\\") + subKey;
		const LONG    createResult =
		    RegCreateKeyExW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(fullSubKey.utf16()), 0, nullptr,
		                    REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
		if (createResult != ERROR_SUCCESS)
			return false;

		const LPCWSTR valueNamePtr =
		    valueName.isEmpty() ? nullptr : reinterpret_cast<LPCWSTR>(valueName.utf16());
		const DWORD valueSize   = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
		const LONG  writeResult = RegSetValueExW(key, valueNamePtr, 0, REG_SZ,
		                                         reinterpret_cast<const BYTE *>(value.utf16()), valueSize);
		RegCloseKey(key);
		return writeResult == ERROR_SUCCESS;
	}

	bool registerWindowsFileAssociations(QString *errorMessage)
	{
		const QString executablePath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
		const QString openCommand = QStringLiteral("\"%1\" \"%2\"").arg(executablePath, QStringLiteral("%1"));
		const QString defaultIcon = QStringLiteral("\"%1\",0").arg(executablePath);
		const QString baseProgramId                = QStringLiteral("QMud");
		const QList<FileAssociationEntry> &entries = fileAssociationEntries();

		for (const FileAssociationEntry &entry : entries)
		{
			const QString programId =
			    QStringLiteral("%1.%2").arg(baseProgramId, QString::fromLatin1(entry.programIdSuffix));
			const QString description     = QString::fromLatin1(entry.description);
			const QString modernExtension = QStringLiteral(".") + QString::fromLatin1(entry.modernExtension);
			const QString legacyExtension = QStringLiteral(".") + QString::fromLatin1(entry.legacyExtension);

			const bool    ok = writeRegistryStringValue(modernExtension, QString(), programId) &&
			                writeRegistryStringValue(legacyExtension, QString(), programId) &&
			                writeRegistryStringValue(programId, QString(), description) &&
			                writeRegistryStringValue(programId + QStringLiteral("\\DefaultIcon"), QString(),
			                                         defaultIcon) &&
			                writeRegistryStringValue(programId + QStringLiteral("\\shell\\open\\command"),
			                                         QString(), openCommand);
			if (!ok)
			{
				if (errorMessage)
					*errorMessage = QStringLiteral("Failed to write Windows file association keys.");
				return false;
			}
		}

		SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
		return true;
	}
#endif

#ifdef Q_OS_MACOS
	CFStringRef cfStringFromQString(const QString &value)
	{
		return CFStringCreateWithCharacters(kCFAllocatorDefault,
		                                    reinterpret_cast<const UniChar *>(value.utf16()),
		                                    static_cast<CFIndex>(value.size()));
	}

	bool registerMacFileAssociations(QString *errorMessage)
	{
		const QString bundleId    = QStringLiteral("com.abnormalfrequency.qmud");
		CFStringRef   bundleIdRef = cfStringFromQString(bundleId);
		if (!bundleIdRef)
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Unable to construct macOS bundle identifier.");
			return false;
		}

		bool                               ok = true;
		QString                            firstError;
		const QList<FileAssociationEntry> &entries = fileAssociationEntries();
		for (const FileAssociationEntry &entry : entries)
		{
			const QStringList extensions = {QString::fromLatin1(entry.modernExtension),
			                                QString::fromLatin1(entry.legacyExtension)};

			for (const QString &extension : extensions)
			{
				CFStringRef extensionRef = cfStringFromQString(extension);
				if (!extensionRef)
					continue;

				CFStringRef utiRef = UTTypeCreatePreferredIdentifierForTag(kUTTagClassFilenameExtension,
				                                                           extensionRef, nullptr);
				CFRelease(extensionRef);
				if (!utiRef)
					continue;

				const OSStatus status =
				    LSSetDefaultRoleHandlerForContentType(utiRef, kLSRolesAll, bundleIdRef);
				CFRelease(utiRef);
				if (status != noErr)
				{
					ok = false;
					if (firstError.isEmpty())
						firstError = QStringLiteral("LaunchServices status %1 while setting default handler.")
						                 .arg(status);
				}
			}
		}

		CFRelease(bundleIdRef);
		if (!ok && errorMessage)
			*errorMessage = firstError;
		return ok;
	}
#endif

#ifdef Q_OS_LINUX
	bool writeTextFile(const QString &path, const QString &text, QString &errorMessage)
	{
		QSaveFile file(path);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
		{
			errorMessage =
			    QStringLiteral("Unable to open '%1' for writing: %2").arg(path, file.errorString());
			return false;
		}
		if (const QByteArray bytes = text.toUtf8(); file.write(bytes) != bytes.size())
		{
			errorMessage = QStringLiteral("Failed writing '%1': %2").arg(path, file.errorString());
			return false;
		}
		if (!file.commit())
		{
			errorMessage = QStringLiteral("Failed committing '%1': %2").arg(path, file.errorString());
			return false;
		}
		return true;
	}

	QString desktopExecField(const QString &executablePath)
	{
		QString escaped = executablePath;
		escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
		escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
		return QStringLiteral("\"%1\" %f").arg(escaped);
	}

	bool runCommandIfAvailable(const QString &command, const QStringList &arguments, QString &errorMessage)
	{
		const QString executable = QStandardPaths::findExecutable(command);
		if (executable.isEmpty())
			return true;

		QProcess process;
		process.start(executable, arguments);
		if (!process.waitForFinished(10000))
		{
			process.kill();
			errorMessage = QStringLiteral("Timed out while executing '%1'.").arg(command);
			return false;
		}

		if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
		{
			const QString stderrText = QString::fromUtf8(process.readAllStandardError()).trimmed();
			const QString stdoutText = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
			QString       detail     = stderrText;
			if (detail.isEmpty())
				detail = stdoutText;
			if (detail.isEmpty())
				detail = QStringLiteral("exit code %1").arg(process.exitCode());
			errorMessage = QStringLiteral("Command '%1' failed: %2").arg(command, detail);
			return false;
		}
		return true;
	}

	bool registerLinuxFileAssociations(QString &errorMessage)
	{
		const QString dataHome = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
		if (dataHome.isEmpty())
		{
			errorMessage = QStringLiteral("Generic data location is unavailable.");
			return false;
		}

		const QString applicationsDir = QDir(dataHome).filePath(QStringLiteral("applications"));
		const QString mimePackagesDir = QDir(dataHome).filePath(QStringLiteral("mime/packages"));
		const QString mimeRootDir     = QDir(dataHome).filePath(QStringLiteral("mime"));
		if (!QDir().mkpath(applicationsDir) || !QDir().mkpath(mimePackagesDir))
		{
			errorMessage =
			    QStringLiteral("Unable to create local MIME/applications directories in %1.").arg(dataHome);
			return false;
		}

		const QString desktopPath    = QDir(applicationsDir).filePath(QStringLiteral("qmud.desktop"));
		const QString mimeXmlPath    = QDir(mimePackagesDir).filePath(QStringLiteral("qmud.xml"));
		const QString executablePath = QCoreApplication::applicationFilePath();

		QString       desktopText;
		QTextStream   desktopStream(&desktopText);
		desktopStream << "[Desktop Entry]\n";
		desktopStream << "Type=Application\n";
		desktopStream << "Name=QMud\n";
		desktopStream << "Comment=QMud MUD client\n";
		desktopStream << "Exec=" << desktopExecField(executablePath) << "\n";
		desktopStream << "Icon=QMud\n";
		desktopStream << "Categories=Network;Game;\n";
		desktopStream << "Terminal=false\n";
		desktopStream << "MimeType=" << registeredMimeTypes().join(QLatin1Char(';')) << ";\n";

		QString     mimeXmlText;
		QTextStream mimeXmlStream(&mimeXmlText);
		mimeXmlStream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
		mimeXmlStream << "<mime-info xmlns=\"http://www.freedesktop.org/standards/shared-mime-info\">\n";
		for (const FileAssociationEntry &entry : fileAssociationEntries())
		{
			mimeXmlStream << "  <mime-type type=\"" << entry.mimeType << "\">\n";
			mimeXmlStream << "    <comment>" << entry.description << "</comment>\n";
			mimeXmlStream << "    <glob pattern=\"*." << entry.modernExtension << "\"/>\n";
			mimeXmlStream << "    <glob pattern=\"*." << entry.legacyExtension << "\"/>\n";
			mimeXmlStream << "  </mime-type>\n";
		}
		mimeXmlStream << "</mime-info>\n";

		QString writeError;
		if (!writeTextFile(desktopPath, desktopText, writeError) ||
		    !writeTextFile(mimeXmlPath, mimeXmlText, writeError))
		{
			errorMessage = writeError;
			return false;
		}

		QString commandError;
		if (!runCommandIfAvailable(QStringLiteral("update-mime-database"), {mimeRootDir}, commandError))
		{
			errorMessage = commandError;
			return false;
		}
		if (!runCommandIfAvailable(QStringLiteral("update-desktop-database"), {applicationsDir},
		                           commandError))
		{
			errorMessage = commandError;
			return false;
		}

		const auto desktopId = QStringLiteral("qmud.desktop");
		for (const auto &mimeType : registeredMimeTypes())
		{
			if (!runCommandIfAvailable(QStringLiteral("xdg-mime"),
			                           {QStringLiteral("default"), desktopId, mimeType}, commandError))
			{
				errorMessage = commandError;
				return false;
			}
		}

		return true;
	}
#endif

	QString canonicalCommandName(const QString &command)
	{
		static const QHash<QString, QString> kCanonicalCommandAliases = {
		    {QStringLiteral("InputGlobalChange"),        QStringLiteral("GlobalChange")         },
		    {QStringLiteral("WindowsSocketInformation"), QStringLiteral("WindowsSocketInfo")    },
		    {QStringLiteral("DoMapperSpecial"),          QStringLiteral("MapperSpecial")        },
		    {QStringLiteral("DoMapperComment"),          QStringLiteral("MapperComment")        },
		    {QStringLiteral("ASCIIart"),                 QStringLiteral("AsciiArt")             },
		    {QStringLiteral("MinimiseProgram"),          QStringLiteral("Minimize")             },
		    {QStringLiteral("GoToURL"),                  QStringLiteral("GoToUrl")              },
		    {QStringLiteral("SelectMatchingBrace"),      QStringLiteral("SelectToMatchingBrace")},
		    {QStringLiteral("ConfigureAutosay"),         QStringLiteral("ConfigureAutoSay")     },
		    {QStringLiteral("ConfigureMudaddress"),      QStringLiteral("ConfigureMudAddress")  },
		    {QStringLiteral("ConfigureMxpPueblo"),       QStringLiteral("ConfigureMxp")         },
		    {QStringLiteral("ConfigurePasteToWorld"),    QStringLiteral("ConfigurePaste")       },
		};

		if (const auto it = kCanonicalCommandAliases.constFind(command);
		    it != kCanonicalCommandAliases.cend())
			return it.value();
		return command;
	}

	bool isTokenCharacter(const QChar ch)
	{
		return ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('.') ||
		       ch == QLatin1Char(':');
	}

	bool isEnabledFlag(const QString &value)
	{
		return value == QStringLiteral("1") || value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
		       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
	}

	QString ensureWorldFilePathForRestartSave(WorldRuntime *runtime, const AppController &app)
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

		QString baseDir = app.defaultWorldDirectory();
		if (baseDir.isEmpty())
			baseDir = app.makeAbsolutePath(QStringLiteral("."));
		if (baseDir.isEmpty())
			baseDir = QCoreApplication::applicationDirPath();
		const QDir dir(baseDir);
		dir.mkpath(QStringLiteral("."));
		filePath = dir.filePath(baseName);
		runtime->setWorldFilePath(filePath);
		return filePath;
	}

	QString worldDisplayNameForRestartSave(const WorldWindowDescriptor &entry)
	{
		QString name;
		if (entry.runtime)
			name = entry.runtime->worldAttributes().value(QStringLiteral("name")).trimmed();
		if (name.isEmpty() && entry.window)
			name = entry.window->windowTitle().trimmed();
		if (name.isEmpty())
			name = QStringLiteral("Untitled");
		return name;
	}

	int hexDigitValue(const char ch)
	{
		if (ch >= '0' && ch <= '9')
			return ch - '0';
		if (ch >= 'a' && ch <= 'f')
			return ch - 'a' + 10;
		if (ch >= 'A' && ch <= 'F')
			return ch - 'A' + 10;
		return -1;
	}

	bool isAsciiSpace(const char ch)
	{
		switch (ch)
		{
		case ' ':
		case '\t':
		case '\n':
		case '\r':
		case '\f':
		case '\v':
			return true;
		default:
			return false;
		}
	}

	bool isAsciiAlnum(const unsigned char ch)
	{
		return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
	}

	bool applySqliteWalAndNormalSynchronous(const QSqlDatabase &db, QString &errorMessage)
	{
		if (!db.isValid() || !db.isOpen())
		{
			errorMessage = QStringLiteral("Database connection is not open.");
			return false;
		}

		QSqlQuery query(db);
		if (!query.exec(QStringLiteral("PRAGMA journal_mode=WAL")))
		{
			errorMessage = query.lastError().text();
			return false;
		}

		if (const QString mode = query.next() ? query.value(0).toString().trimmed() : QString();
		    mode.compare(QStringLiteral("wal"), Qt::CaseInsensitive) != 0)
		{
			errorMessage = QStringLiteral("PRAGMA journal_mode returned '%1' instead of 'wal'.").arg(mode);
			return false;
		}

		if (!query.exec(QStringLiteral("PRAGMA synchronous=NORMAL")))
		{
			errorMessage = query.lastError().text();
			return false;
		}

		return true;
	}

	void appendHexByte(QByteArray &out, const unsigned char c)
	{
		constexpr char kHex[] = "0123456789abcdef";
		out.append(kHex[c >> 4 & 0x0F]);
		out.append(kHex[c & 0x0F]);
	}

	QString normalizeDirectionKey(const QString &direction)
	{
		return direction.trimmed().toLower();
	}

	QPlainTextEdit *resolveActiveTextEditor(const MainWindow *mainWindow)
	{
		QWidget *focus = QApplication::focusWidget();
		if (auto *edit = qobject_cast<QPlainTextEdit *>(focus))
		{
			QWidget *parent = edit;
			while (parent)
			{
				if (qobject_cast<TextChildWindow *>(parent))
					return edit;
				parent = parent->parentWidget();
			}
		}
		if (!mainWindow)
			return nullptr;
		if (const TextChildWindow *text = mainWindow->activeTextChildWindow())
			return text->editor();
		return nullptr;
	}

	const QStringList &internalFunctionNames()
	{
		static const QStringList names = []
		{
			QStringList list;
			for (int i = 0; kInternalFunctionMetadataTable[i].functionName[0]; ++i)
				list.push_back(QString::fromUtf8(kInternalFunctionMetadataTable[i].functionName));
			list.removeDuplicates();
			std::ranges::sort(list, [](const QString &a, const QString &b)
			                  { return QString::compare(a, b, Qt::CaseInsensitive) < 0; });
			return list;
		}();
		return names;
	}

	QString chooseInternalFunction(MainWindow *owner, const QStringList &names, const QString &initialFilter)
	{
		if (names.isEmpty())
			return {};

		QDialog dialog(owner);
		dialog.setWindowTitle(QStringLiteral("Functions List"));
		dialog.setWindowFlags(dialog.windowFlags() | Qt::Tool);
		QVBoxLayout layout(&dialog);
		QLabel      filterLabel(QStringLiteral("Filter:"), &dialog);
		QLineEdit   filterEdit(&dialog);
		filterEdit.setText(initialFilter);
		QListWidget list(&dialog);
		layout.addWidget(&filterLabel);
		layout.addWidget(&filterEdit);
		layout.addWidget(&list, 1);
		QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
		layout.addWidget(&buttons);

		QObject::connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
		QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
		QObject::connect(&list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);

		auto populate = [&]
		{
			const QString needle = filterEdit.text().trimmed();
			list.clear();
			for (const QString &name : names)
			{
				if (needle.isEmpty() || name.contains(needle, Qt::CaseInsensitive))
					list.addItem(name);
			}
			if (list.count() > 0)
				list.setCurrentRow(0);
		};
		QObject::connect(&filterEdit, &QLineEdit::textChanged, &dialog, [&](const QString &) { populate(); });
		populate();

		if (dialog.exec() != QDialog::Accepted)
			return {};
		if (const QListWidgetItem *item = list.currentItem())
			return item->text();
		return {};
	}

	QByteArray decodeDebugWorldInput(const QString &text)
	{
		const QByteArray bytes = text.toLocal8Bit();
		QByteArray       out;
		out.reserve(bytes.size());
		const char *p = bytes.constData();
		while (*p)
		{
			if (*p == '\\')
			{
				++p;
				if (*p == '\\')
				{
					out.append(*p++);
					continue;
				}
				char result = 0;
				for (int i = 0; i < 2; ++i)
				{
					if (const int digit = hexDigitValue(*p); digit >= 0)
					{
						++p;
						result = static_cast<char>(result << 4);
						result = static_cast<char>(result + digit);
					}
				}
				out.append(result);
				continue;
			}
			out.append(*p++);
		}
		return out;
	}

	QString normalizePathString(const QString &input)
	{
		QString output = input;
		output.replace(QLatin1Char('\\'), QLatin1Char('/'));
		return output;
	}

	QString absolutePathFromBase(const QString &baseDir, const QString &path);

	QString canonicalAbsolutePath(const QString &path, const QString &workingDir)
	{
		const QString normalized = QMudFileExtensions::canonicalizePathExtension(normalizePathString(path));
		return absolutePathFromBase(workingDir, normalized);
	}

	QString mruComparisonKey(const QString &path, const QString &workingDir)
	{
		if (path.trimmed().isEmpty())
			return {};
		const QString absolute = canonicalAbsolutePath(path, workingDir);
#ifdef Q_OS_WIN
		return absolute.toLower();
#else
		return absolute;
#endif
	}

	QString preferredMruStoragePath(const QString &path, const QString &workingDir)
	{
		if (path.trimmed().isEmpty())
			return {};
		const QString absolute = canonicalAbsolutePath(path, workingDir);
		const QString baseDir  = QDir::cleanPath(workingDir);
		if (QString relative = normalizePathString(QDir(baseDir).relativeFilePath(absolute));
		    !relative.isEmpty() && relative != QStringLiteral("..") &&
		    !relative.startsWith(QStringLiteral("../")) && !QDir::isAbsolutePath(relative))
		{
			if (relative.startsWith(QStringLiteral("./")) || relative.startsWith(QStringLiteral("../")))
				return relative;
			return QStringLiteral("./") + relative;
		}
		return normalizePathString(absolute);
	}

	QString keyLeafName(const QString &key)
	{
		const qsizetype slash = key.lastIndexOf(QLatin1Char('/'));
		if (slash < 0)
			return key;
		return key.mid(slash + 1);
	}

	bool isPathListKey(const QString &key)
	{
		const QString leaf = keyLeafName(key);
		return leaf.compare(QStringLiteral("WorldList"), Qt::CaseInsensitive) == 0 ||
		       leaf.compare(QStringLiteral("PluginList"), Qt::CaseInsensitive) == 0;
	}

	using PathTransformFn = QString (*)(const QString &);

	QString transformPathList(const QString &input, const PathTransformFn transform)
	{
		if (input.isEmpty())
			return input;
		const QStringList items = input.split(QLatin1Char('*'), Qt::KeepEmptyParts);
		QStringList       transformed;
		transformed.reserve(items.size());
		for (const QString &item : items)
			transformed.push_back(transform(item));
		return transformed.join(QLatin1Char('*'));
	}

	QString normalizePathList(const QString &input)
	{
		return transformPathList(input, normalizePathString);
	}

	QString pathForRuntime(const QString &input)
	{
#ifdef Q_OS_WIN
		return QDir::toNativeSeparators(input);
#else
		return normalizePathString(input);
#endif
	}

	QString pathListForRuntime(const QString &input)
	{
		return transformPathList(input, pathForRuntime);
	}

	QStringList splitSerializedPathList(const QString &valueList)
	{
		if (valueList.trimmed().isEmpty())
			return {};
		QString normalized = valueList;
		normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
		normalized.replace(QLatin1Char('*'), QLatin1Char('\n'));

		const auto stripOptionalQuotesLocal = [](const QString &value) -> QString
		{
			if (value.size() < 2)
				return value;
			const QChar first = value.front();
			const QChar last  = value.back();
			if ((first == QLatin1Char('"') && last == QLatin1Char('"')) ||
			    (first == QLatin1Char('\'') && last == QLatin1Char('\'')))
			{
				return value.mid(1, value.size() - 2);
			}
			return value;
		};

		QStringList items;
		for (QString entry : normalized.split(QLatin1Char('\n'), Qt::KeepEmptyParts))
		{
			entry = stripOptionalQuotesLocal(entry.trimmed());
			if (!entry.isEmpty())
				items.push_back(entry);
		}
		return items;
	}

	QStringList splitSerializedWorldList(const QString &worldList)
	{
		return splitSerializedPathList(worldList);
	}

	bool shouldNormalizePathKey(const QString &key)
	{
		const QString leaf = keyLeafName(key);
		return leaf.contains(QStringLiteral("Directory")) || leaf.contains(QStringLiteral("File")) ||
		       leaf.contains(QStringLiteral("Path"));
	}

#ifndef Q_OS_WIN
	bool isPortableDataDirectoryKey(const QString &key)
	{
		const QString leaf = keyLeafName(key);
		return leaf.compare(QStringLiteral("DefaultWorldFileDirectory"), Qt::CaseInsensitive) == 0 ||
		       leaf.compare(QStringLiteral("DefaultLogFileDirectory"), Qt::CaseInsensitive) == 0 ||
		       leaf.compare(QStringLiteral("PluginsDirectory"), Qt::CaseInsensitive) == 0 ||
		       leaf.compare(QStringLiteral("StateFilesDirectory"), Qt::CaseInsensitive) == 0;
	}
#endif

	QString normalizeLegacyPortableDirectory(const QString &key, const QString &value)
	{
#ifdef Q_OS_WIN
		Q_UNUSED(key);
		return value;
#else
		if (!isPortableDataDirectoryKey(key))
			return value;

		const QString trimmed = value.trimmed();
		if (trimmed.isEmpty())
			return value;

		const QString normalized = normalizePathString(trimmed);
		QString       candidate  = normalized;
		while (candidate.startsWith(QLatin1Char('/')))
			candidate.remove(0, 1);
		const QString candidateLower = candidate.toLower();
		QString       portableRelative;
		const bool    hasDrivePath =
		    candidate.size() > 1 && candidate.at(0).isLetter() && candidate.at(1) == QLatin1Char(':');
		if (hasDrivePath)
		{
			qsizetype rootPos = candidateLower.indexOf(QStringLiteral("/worlds/"));
			if (rootPos < 0 && (candidateLower == QStringLiteral("worlds") ||
			                    candidateLower.endsWith(QStringLiteral("/worlds"))))
				rootPos = candidateLower.lastIndexOf(QStringLiteral("/worlds"));
			if (rootPos < 0)
				rootPos = candidateLower.indexOf(QStringLiteral("/logs/"));
			if (rootPos < 0 && (candidateLower == QStringLiteral("logs") ||
			                    candidateLower.endsWith(QStringLiteral("/logs"))))
				rootPos = candidateLower.lastIndexOf(QStringLiteral("/logs"));
			if (rootPos >= 0)
				portableRelative = candidate.mid(rootPos + 1);
		}
		else
		{
			const bool looksLikePortableTree = candidateLower == QStringLiteral("worlds") ||
			                                   candidateLower.startsWith(QStringLiteral("worlds/")) ||
			                                   candidateLower == QStringLiteral("logs") ||
			                                   candidateLower.startsWith(QStringLiteral("logs/"));
			if (looksLikePortableTree)
				portableRelative = candidate;
		}

		if (portableRelative.isEmpty())
			return value;

		QString fixed = QStringLiteral("./") + portableRelative;
		if ((trimmed.endsWith(QLatin1Char('/')) || trimmed.endsWith(QLatin1Char('\\'))) &&
		    !fixed.endsWith(QLatin1Char('/')))
		{
			fixed += QLatin1Char('/');
		}
		return fixed;
#endif
	}

	QString normalizeStoredGlobalStringValue(const QString &key, const QString &value)
	{
		QString adjusted = normalizeLegacyPortableDirectory(key, value);
		if (isPathListKey(key))
			return normalizePathList(adjusted);
		if (shouldNormalizePathKey(key))
			return QMudFileExtensions::canonicalizePathExtension(normalizePathString(adjusted));
		return adjusted;
	}

	QString normalizeRuntimeGlobalStringValue(const QString &key, const QString &value)
	{
		QString stored = normalizeStoredGlobalStringValue(key, value);
		if (isPathListKey(key))
			return pathListForRuntime(stored);
		if (shouldNormalizePathKey(key))
			return pathForRuntime(stored);
		return stored;
	}

	QString migrateLegacyLuaScriptTipText(const QString &script)
	{
		static const auto kLegacyLine =
		    QStringLiteral("-- Possible sandbox, and security tips: http://www.gammon.com.au/security");
		static const auto kModernLine =
		    QStringLiteral("-- Example sandbox can be found in the docs directory.");

		QString migrated = script;
		migrated.replace(kLegacyLine, kModernLine);
		return migrated;
	}

	bool bringOwnedWindowToFrontByTitle(const QString &title)
	{
		const QString target = title.trimmed();
		if (target.isEmpty())
			return false;
		for (const auto windows = QApplication::topLevelWidgets(); QWidget *window : windows)
		{
			if (!window)
				continue;
			if (window->windowTitle().trimmed().compare(target, Qt::CaseInsensitive) != 0)
				continue;
			if (!window->isVisible())
				window->show();
			window->raise();
			window->activateWindow();
			return true;
		}
		return false;
	}

	constexpr qint64 kTarBlockSize = 512;

	QString          sanitizedBackupNamePart(const QString &value)
	{
		QString out = value.trimmed();
		if (out.isEmpty())
			out = QStringLiteral("unknown");
		for (QChar &ch : out)
		{
			if (ch.isLetterOrNumber() || ch == QLatin1Char('.') || ch == QLatin1Char('_') ||
			    ch == QLatin1Char('-'))
				continue;
			ch = QLatin1Char('_');
		}
		return out;
	}

	bool writeTarOctalField(QByteArray &header, const int offset, const int fieldSize, const quint64 value,
	                        QString &errorMessage, const QString &fieldName)
	{
		const QByteArray octal = QByteArray::number(value, 8);
		if (octal.size() > fieldSize - 1)
		{
			errorMessage =
			    QStringLiteral("Tar field '%1' overflow while creating upgrade backup.").arg(fieldName);
			return false;
		}

		const int padding = fieldSize - 1 - static_cast<int>(octal.size());
		for (int i = 0; i < padding; ++i)
			header[offset + i] = '0';
		for (qsizetype i = 0; i < octal.size(); ++i)
			header[offset + padding + i] = octal.at(i);
		header[offset + fieldSize - 1] = '\0';
		return true;
	}

	bool splitTarPathForUstar(const QByteArray &rawPath, QByteArray &nameOut, QByteArray &prefixOut,
	                          QString &errorMessage)
	{
		QByteArray path = rawPath;
		while (path.endsWith('/'))
			path.chop(1);
		if (path.isEmpty())
		{
			errorMessage = QStringLiteral("Invalid empty tar entry path during upgrade backup.");
			return false;
		}

		if (path.size() <= 100)
		{
			nameOut   = path;
			prefixOut = QByteArray();
			return true;
		}

		qsizetype split = -1;
		for (qsizetype i = path.size() - 1; i >= 0; --i)
		{
			if (path.at(i) != '/')
				continue;
			const qsizetype prefixLength = i;
			if (const qsizetype nameLength = path.size() - i - 1; prefixLength <= 155 && nameLength <= 100)
			{
				split = i;
				break;
			}
		}

		if (split < 0)
		{
			errorMessage =
			    QStringLiteral("Path too long for ustar backup entry: %1").arg(QString::fromUtf8(path));
			return false;
		}

		prefixOut = path.left(split);
		nameOut   = path.mid(split + 1);
		return true;
	}

	bool buildTarHeader(const QString &entryPath, const qint64 fileSize, const qint64 modifiedEpochSeconds,
	                    const bool directory, QByteArray &headerOut, QString &errorMessage)
	{
		const QString normalizedPath = QDir::fromNativeSeparators(QDir::cleanPath(entryPath));
		if (normalizedPath.isEmpty() || normalizedPath == QStringLiteral("."))
		{
			errorMessage = QStringLiteral("Invalid archive entry path while creating upgrade backup.");
			return false;
		}

		QByteArray nameField;
		QByteArray prefixField;
		if (!splitTarPathForUstar(normalizedPath.toUtf8(), nameField, prefixField, errorMessage))
			return false;

		QByteArray header(static_cast<int>(kTarBlockSize), '\0');

		for (int i = 0; i < nameField.size(); ++i)
			header[i] = nameField.at(i);
		for (int i = 0; i < prefixField.size(); ++i)
			header[345 + i] = prefixField.at(i);

		if (!writeTarOctalField(header, 100, 8, directory ? 0755 : 0644, errorMessage,
		                        QStringLiteral("mode")) ||
		    !writeTarOctalField(header, 108, 8, 0, errorMessage, QStringLiteral("uid")) ||
		    !writeTarOctalField(header, 116, 8, 0, errorMessage, QStringLiteral("gid")) ||
		    !writeTarOctalField(header, 124, 12,
		                        directory ? 0 : static_cast<quint64>(qMax<qint64>(0, fileSize)), errorMessage,
		                        QStringLiteral("size")) ||
		    !writeTarOctalField(header, 136, 12, static_cast<quint64>(qMax<qint64>(0, modifiedEpochSeconds)),
		                        errorMessage, QStringLiteral("mtime")))
		{
			return false;
		}

		header[156] = directory ? '5' : '0';
		header[257] = 'u';
		header[258] = 's';
		header[259] = 't';
		header[260] = 'a';
		header[261] = 'r';
		header[262] = '\0';
		header[263] = '0';
		header[264] = '0';

		for (int i = 148; i < 156; ++i)
			header[i] = ' ';

		quint64 checksum = 0;
		for (const unsigned char value : header)
			checksum += value;

		const QByteArray checksumOctal = QByteArray::number(checksum, 8);
		if (checksumOctal.size() > 6)
		{
			errorMessage = QStringLiteral("Tar checksum overflow while creating upgrade backup.");
			return false;
		}

		for (int i = 0; i < 6; ++i)
			header[148 + i] = '0';
		const qsizetype checksumPadding = 6 - checksumOctal.size();
		for (qsizetype i = 0; i < checksumOctal.size(); ++i)
			header[148 + checksumPadding + i] = checksumOctal.at(i);
		header[154] = '\0';
		header[155] = ' ';

		headerOut = header;
		return true;
	}

	bool writeGzipBytes(gzFile gz, const QByteArray &bytes, QString &errorMessage, const QString &context)
	{
		if (bytes.isEmpty())
			return true;

		qsizetype offset = 0;
		while (offset < bytes.size())
		{
			const qsizetype remaining = bytes.size() - offset;
			const qsizetype chunkSize = qMin<qsizetype>(remaining, 64 * 1024);
			const int       expected  = static_cast<int>(chunkSize);
			if (const int written =
			        gzwrite(gz, bytes.constData() + offset, static_cast<unsigned int>(expected));
			    written != expected)
			{
				int         zError   = Z_OK;
				const char *zMessage = gzerror(gz, &zError);
				if (zMessage && zError != Z_OK)
				{
					errorMessage = QStringLiteral("%1: gzip write failed: %2")
					                   .arg(context, QString::fromLatin1(zMessage));
				}
				else
				{
					errorMessage = QStringLiteral("%1: gzip write failed.").arg(context);
				}
				return false;
			}
			offset += chunkSize;
		}
		return true;
	}

	bool writeTarDirectoryEntry(gzFile gz, const QString &entryPath, const qint64 modifiedEpochSeconds,
	                            QString &errorMessage)
	{
		QByteArray header;
		if (!buildTarHeader(entryPath, 0, modifiedEpochSeconds, true, header, errorMessage))
			return false;
		return writeGzipBytes(gz, header, errorMessage, QStringLiteral("Writing tar directory entry"));
	}

	bool writeTarFileEntry(gzFile gz, const QString &sourcePath, const QString &entryPath,
	                       const qint64 fileSize, const qint64 modifiedEpochSeconds, QString &errorMessage)
	{
		QByteArray header;
		if (!buildTarHeader(entryPath, fileSize, modifiedEpochSeconds, false, header, errorMessage))
			return false;
		if (!writeGzipBytes(gz, header, errorMessage, QStringLiteral("Writing tar file header")))
			return false;

		QFile source(sourcePath);
		if (!source.open(QIODevice::ReadOnly))
		{
			errorMessage =
			    QStringLiteral("Unable to open '%1' for backup: %2").arg(sourcePath, source.errorString());
			return false;
		}

		QByteArray chunk;
		chunk.resize(64 * 1024);
		while (!source.atEnd())
		{
			const qint64 readBytes = source.read(chunk.data(), chunk.size());
			if (readBytes < 0)
			{
				errorMessage = QStringLiteral("Unable to read '%1' for backup: %2")
				                   .arg(sourcePath, source.errorString());
				return false;
			}
			if (readBytes == 0)
				continue;
			if (!writeGzipBytes(gz, chunk.left(static_cast<int>(readBytes)), errorMessage,
			                    QStringLiteral("Writing tar file content")))
			{
				return false;
			}
		}

		if (const qint64 remainder = fileSize % kTarBlockSize; remainder != 0)
		{
			if (const QByteArray padding(static_cast<int>(kTarBlockSize - remainder), '\0');
			    !writeGzipBytes(gz, padding, errorMessage, QStringLiteral("Writing tar file padding")))
				return false;
		}

		return true;
	}

	bool createUpgradeBackupArchive(const QString &dataDirPath, const QString &versionLabel,
	                                QString &archivePathOut, QString &errorMessage)
	{
		const QDir    dataDir(dataDirPath);
		const QString backupDirPath = dataDir.filePath(QStringLiteral("backup"));
		if (!QDir().mkpath(backupDirPath))
		{
			errorMessage = QStringLiteral("Unable to create backup directory: %1").arg(backupDirPath);
			return false;
		}

		const QString timestamp =
		    QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmssZ"));
		const QString fileName =
		    QStringLiteral("QMud_Backup_%1_%2.tar.gz")
		        .arg(sanitizedBackupNamePart(versionLabel), sanitizedBackupNamePart(timestamp));
		const QString archivePath = QDir(backupDirPath).filePath(fileName);
		QFile::remove(archivePath);

		const QByteArray archivePathBytes = QFile::encodeName(archivePath);
		gzFile           gz               = gzopen(archivePathBytes.constData(), "wb");
		if (!gz)
		{
			errorMessage = QStringLiteral("Unable to open backup archive for writing: %1").arg(archivePath);
			return false;
		}

		bool              wroteEntries = false;
		const QStringList roots        = {QStringLiteral("worlds"), QStringLiteral("sounds"),
		                                  QStringLiteral("scripts"), QStringLiteral("lua")};
		for (const QString &rootName : roots)
		{
			const QString rootPath = dataDir.filePath(rootName);
			QFileInfo     rootInfo(rootPath);
			if (!rootInfo.exists() || rootInfo.isSymLink())
				continue;

			const QString rootArchivePath = QDir::fromNativeSeparators(rootName);
			if (rootInfo.isDir())
			{
				if (!writeTarDirectoryEntry(gz, rootArchivePath, rootInfo.lastModified().toSecsSinceEpoch(),
				                            errorMessage))
				{
					gzclose(gz);
					QFile::remove(archivePath);
					return false;
				}
				wroteEntries = true;

				QDirIterator iterator(rootPath,
				                      QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
				                      QDirIterator::Subdirectories);
				while (iterator.hasNext())
				{
					const QString path = iterator.next();
					QFileInfo     info = iterator.fileInfo();
					if (info.isSymLink())
						continue;

					QString archiveEntryPath =
					    QDir::fromNativeSeparators(QDir::cleanPath(dataDir.relativeFilePath(path)));
					if (archiveEntryPath.isEmpty() || archiveEntryPath == QStringLiteral(".") ||
					    archiveEntryPath.startsWith(QStringLiteral("..")))
					{
						continue;
					}

					const qint64 modified = info.lastModified().toSecsSinceEpoch();
					if (info.isDir())
					{
						if (!writeTarDirectoryEntry(gz, archiveEntryPath, modified, errorMessage))
						{
							gzclose(gz);
							QFile::remove(archivePath);
							return false;
						}
						wroteEntries = true;
					}
					else if (info.isFile())
					{
						if (!writeTarFileEntry(gz, path, archiveEntryPath, info.size(), modified,
						                       errorMessage))
						{
							gzclose(gz);
							QFile::remove(archivePath);
							return false;
						}
						wroteEntries = true;
					}
				}
			}
			else if (rootInfo.isFile())
			{
				if (!writeTarFileEntry(gz, rootPath, rootArchivePath, rootInfo.size(),
				                       rootInfo.lastModified().toSecsSinceEpoch(), errorMessage))
				{
					gzclose(gz);
					QFile::remove(archivePath);
					return false;
				}
				wroteEntries = true;
			}
		}

		if (const QByteArray endMarker(static_cast<int>(kTarBlockSize * 2), '\0');
		    !writeGzipBytes(gz, endMarker, errorMessage, QStringLiteral("Writing tar end marker")))
		{
			gzclose(gz);
			QFile::remove(archivePath);
			return false;
		}

		if (gzclose(gz) != Z_OK)
		{
			errorMessage = QStringLiteral("Unable to finalize backup archive: %1").arg(archivePath);
			QFile::remove(archivePath);
			return false;
		}

		if (!wroteEntries)
			qInfo() << "Upgrade backup archive created with no matching data directories:" << archivePath;

		archivePathOut = archivePath;
		return true;
	}
} // namespace

// ints - WARNING - we assume these are ints, the type-casting will defeat
// warnings if they are not.
static const struct
{
		const char *name;
		int         defaultValue;
} kGlobalOptionsTable[] = {

    // option name                          default
    {"AllTypingToCommandWindow",        1                       },
    {"AlwaysOnTop",                     0                       },
    {"AppendToLogFiles",                0                       },
    {"AutoConnectWorlds",               1                       },
    {"AutoExpandConfig",                1                       },
    {"AutoCheckForUpdates",             1                       },
    {"BackupOnUpgrades",                1                       },
    {"FlatToolbars",                    1                       },
    {"AutoLogWorld",                    0                       },
    {"BleedBackground",                 0                       },
    {"ColourGradientConfig",            1                       },
    {"ConfirmBeforeClosingMXPdebug",    0                       },
    {"ConfirmBeforeClosingQmud",        0                       },
    {"ConfirmBeforeClosingWorld",       1                       },
    {"ConfirmBeforeSavingVariables",    1                       },
    {"ConfirmLogFileClose",             1                       },
    {"EnableSpellCheck",                1                       },
    {"AllowLoadingDlls",                1                       },
    {"F1macro",                         0                       },
    {"DisableWindowScaler",             0                       },
    {"FixedFontForEditing",             1                       },
    {"NotepadWordWrap",                 1                       },
    {"NotifyIfCannotConnect",           1                       },
    {"ErrorNotificationToOutputWindow", 1                       },
    {"NotifyOnDisconnect",              1                       },
    {"OpenActivityWindow",              0                       },
    {"OpenWorldsMaximised",             0                       },
    {"WindowTabsStyle",                 0                       },
    {"ReconnectOnLinkFailure",          0                       },
    {"EnableReloadFeature",             1                       },
    {"RegexpMatchEmpty",                1                       },
    {"ShowGridLinesInListViews",        1                       },
    {"SmoothScrolling",                 0                       },
    {"SmootherScrolling",               0                       },
    {"DisableKeyboardMenuActivation",   0                       },
    {"TriggerRemoveCheck",              1                       },
    {"NotepadBackColour",               0                       },
    {"NotepadTextColour",               0                       },
    {"ActivityButtonBarStyle",          0                       },
    {"AsciiArtLayout",                  0                       },
    {"DefaultInputFontHeight",          9                       },
    {"DefaultInputFontItalic",          0                       },
    {"DefaultInputFontWeight",          400                     },
    {"DefaultOutputFontHeight",         9                       },
    {"Icon Placement",                  0                       },
    {"Tray Icon",                       0                       }, // normal icon
    {"ActivityWindowRefreshInterval",   15                      },
    {"ActivityWindowRefreshType",       2                       },
    {"ParenMatchFlags",                 0x0001 | 0x0020 | 0x0040},
    {"PrinterFontSize",                 10                      },
    {"PrinterLeftMargin",               15                      },
    {"PrinterLinesPerPage",             60                      },
    {"PrinterTopMargin",                15                      },
    {"ReloadMccpDisableTimeoutMs",      1000                    },
    {"TimerInterval",                   0                       },
    {"UpdateCheckIntervalHours",        1                       },
    {"FixedPitchFontSize",              9                       },
    {"TabInsertsTabInMultiLineDialogs", 0                       },

    {nullptr,                           0                       }  // end of table marker
}; // end of table

// strings
static const struct
{
		const char *name;
		const char *defaultValue;
} kAlphaGlobalOptionsTable[] = {

    // option name                              default

    {"AsciiArtFont",                  "fonts\\standard.flf"       },
    {"DefaultAliasesFile",            ""                          },
    {"DefaultColoursFile",            ""                          },
    {"DefaultInputFont",              "DejaVu Sans Mono"          },
    {"DefaultLogFileDirectory",       ".\\logs\\"                 },
    {"DefaultMacrosFile",             ""                          },
    {"DefaultNameGenerationFile",     "names/names.txt"           },
    {"DefaultOutputFont",             "DejaVu Sans Mono"          },
    {"DefaultTimersFile",             ""                          },
    {"DefaultTriggersFile",           ""                          },
    {"DefaultWorldFileDirectory",     ".\\worlds\\"               },
    {"NotepadQuoteString",            "> "                        },
    {"PluginList",                    ""                          },
    {"PluginsDirectory",              R"(.\worlds\plugins\)"      },
    {"StateFilesDirectory",           R"(.\worlds\plugins\state\)"}, // however see below
    {"PrinterFont",                   "Courier"                   },
    {"TrayIconFileName",              ""                          },
    {"WordDelimiters",                ".,()[]\"\'"                },
    {"WordDelimitersDblClick",        ".,()[]\"\'"                },
    {"WorldList",                     ""                          },
    {"LuaScript",                     ""                          },
    {"Locale",                        "EN"                        },
    {"FixedPitchFont",                "DejaVu Sans Mono"          },
    {"SkipUpdateNotificationVersion", ""                          },

    {nullptr,                         nullptr                     }  // end of table marker
}; // end of table

namespace
{
	struct ExtraIntPref
	{
			const char *key;
			int         defaultValue;
	};

	constexpr ExtraIntPref kDbOnlyGlobalIntPrefs[] = {
	    {"ViewToolbar",              1},
	    {"ViewWorldToolbar",         1},
	    {"ActivityToolbar",          1},
	    {"ViewStatusbar",            1},
	    {"ViewInfoBar",              0},
	    {"DefaultInputFontCharset",  1},
	    {"DefaultOutputFontCharset", 1},
	};

	int dbOnlyGlobalIntDefault(const QString &key)
	{
		for (const auto &[prefKey, prefDefault] : kDbOnlyGlobalIntPrefs)
		{
			if (key == QLatin1String(prefKey))
				return prefDefault;
		}
		return 0;
	}

	int dbOnlyGlobalIntValue(const QMap<QString, int> &prefs, const QString &key)
	{
		return prefs.value(key, dbOnlyGlobalIntDefault(key));
	}

	QString absolutePathFromBase(const QString &baseDir, const QString &path)
	{
		auto looksLikeWindowsDrivePath = [](const QString &value, const int offset = 0) -> bool
		{
			return value.size() > offset + 1 && value.at(offset).isLetter() &&
			       value.at(offset + 1) == QLatin1Char(':');
		};

		QString normalized = normalizePathString(path.trimmed());
		while (normalized.startsWith(QStringLiteral("./")))
			normalized = normalized.mid(2);
		if (normalized.startsWith(QLatin1Char('/')) && looksLikeWindowsDrivePath(normalized, 1))
			normalized = normalized.mid(1);
		if (QFileInfo(normalized).isAbsolute() || looksLikeWindowsDrivePath(normalized))
			return QDir::cleanPath(normalized);
		return QDir::cleanPath(QDir(baseDir).filePath(normalized));
	}

	QString archiveRelativePathFor(const QString &baseDir, const QString &absolutePath);

	QString resolveExistingPathCaseInsensitive(const QString &path)
	{
		QString cleaned = QDir::cleanPath(path);
		if (cleaned.isEmpty())
			return {};
		if (QFileInfo::exists(cleaned))
			return cleaned;
#ifdef Q_OS_WIN
		return {};
#else
		if (!QDir::isAbsolutePath(cleaned))
			return {};
		QStringList segments = cleaned.split(QLatin1Char('/'), Qt::SkipEmptyParts);
		QString     current  = QStringLiteral("/");
		for (const QString &segment : segments)
		{
			const QDir      dir(current);
			const QFileInfo matched = [&]() -> QFileInfo
			{
				const QFileInfoList entries =
				    dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
				for (const QFileInfo &entry : entries)
				{
					if (entry.fileName().compare(segment, Qt::CaseInsensitive) == 0)
						return entry;
				}
				return {};
			}();
			if (!matched.exists())
				return {};
			current = matched.absoluteFilePath();
		}
		return QDir::cleanPath(current);
#endif
	}

	QString storagePathFromAbsolute(const QString &baseDir, const QString &sourcePath,
	                                const QString &absolutePath)
	{
		if (QFileInfo(sourcePath).isAbsolute())
			return normalizePathString(absolutePath);
		return archiveRelativePathFor(baseDir, absolutePath);
	}

	QString remapLegacyWindowsWorldPathToBase(const QString &baseDir, const QString &path)
	{
		const QString normalized = normalizePathString(path.trimmed());
		const bool    hasDrive =
		    normalized.size() > 1 && normalized.at(0).isLetter() && normalized.at(1) == QLatin1Char(':');
		if (!hasDrive)
			return {};
		const qsizetype worldsPos = normalized.indexOf(QStringLiteral("/worlds/"), 0, Qt::CaseInsensitive);
		if (worldsPos < 0)
			return {};
		const QString relativeWorldPath = normalized.mid(worldsPos + 1); // strip leading '/'
		return absolutePathFromBase(baseDir, relativeWorldPath);
	}

	QString stripOptionalQuotes(const QString &value)
	{
		if (value.size() < 2)
			return value;
		if (const QChar first = value.front(), last = value.back();
		    (first == QLatin1Char('"') && last == QLatin1Char('"')) ||
		    (first == QLatin1Char('\'') && last == QLatin1Char('\'')))
			return value.mid(1, value.size() - 2);
		return value;
	}

	QString archiveRelativePathFor(const QString &baseDir, const QString &absolutePath)
	{
		QString relative = QDir(baseDir).relativeFilePath(absolutePath);
		if (relative == QStringLiteral("..") || relative.startsWith(QStringLiteral("../")) ||
		    QDir::isAbsolutePath(relative))
		{
			relative = QDir::cleanPath(absolutePath);
#ifdef Q_OS_WIN
			if (relative.size() > 1 && relative.at(1) == QLatin1Char(':'))
				relative.replace(1, 1, QStringLiteral("_"));
#endif
			while (relative.startsWith(QLatin1Char('/')))
				relative.remove(0, 1);
		}
		return normalizePathString(relative);
	}

	bool moveFileToArchive(const QString &sourcePath, const QString &targetPath)
	{
		const QFileInfo targetInfo(targetPath);
		if (const QString targetDir = targetInfo.absolutePath(); !QDir().mkpath(targetDir))
			return false;

		if (QFileInfo::exists(targetPath) && !QFile::remove(targetPath))
			return false;

		if (QFile::rename(sourcePath, targetPath))
			return true;

		if (!QFile::copy(sourcePath, targetPath))
			return false;

		return QFile::remove(sourcePath);
	}

	bool directoryContainsFileCaseInsensitive(const QString &directoryPath, const QString &fileName)
	{
		const QDir dir(directoryPath);
		if (!dir.exists())
			return false;

		if (QFileInfo::exists(dir.filePath(fileName)))
			return true;

		const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
		return std::ranges::any_of(entries, [&fileName](const QFileInfo &entry)
		                           { return entry.fileName().compare(fileName, Qt::CaseInsensitive) == 0; });
	}

	bool shouldCopySyncedFile(const QString &sourcePath, const QString &relativePath,
	                          const QString &destinationPath, const bool targetHasLegacyPrefsDb,
	                          const bool targetHasLegacyIni)
	{
		const QFileInfo sourceInfo(sourcePath);
		if (!sourceInfo.exists() || !sourceInfo.isFile())
			return false;

		const QFileInfo relativeInfo(relativePath);
		const QString   fileName = relativeInfo.fileName();
		const QString   suffix   = relativeInfo.suffix().toLower();
		const bool isQmudPrefsDb = fileName.compare(QStringLiteral("QMud.sqlite"), Qt::CaseInsensitive) == 0;
		const bool isQmudConf    = fileName.compare(QStringLiteral("QMud.conf"), Qt::CaseInsensitive) == 0;
		const bool isHelpDb      = fileName.compare(QStringLiteral("help.db"), Qt::CaseInsensitive) == 0;
		const bool isWorldFile   = suffix == QStringLiteral("qdl");
		const bool isDatabase    = suffix == QStringLiteral("db");

		if (isQmudPrefsDb && targetHasLegacyPrefsDb)
			return false;
		if (isQmudConf && targetHasLegacyIni)
			return false;

		const QFileInfo destinationInfo(destinationPath);
		if ((isQmudPrefsDb || isQmudConf) && destinationInfo.exists())
			return false;
		if (!destinationInfo.exists())
			return true;

		if (isWorldFile)
			return false;

		if (isDatabase && !isHelpDb)
			return false;

		const QDateTime sourceTime      = sourceInfo.lastModified();
		const QDateTime destinationTime = destinationInfo.lastModified();
		if (!sourceTime.isValid() || !destinationTime.isValid())
			return false;

		return sourceTime > destinationTime;
	}

	bool copySyncedFileWithPolicy(const QString &sourcePath, const QString &relativePath,
	                              const QString &destinationPath, const bool targetHasLegacyPrefsDb,
	                              const bool targetHasLegacyIni)
	{
		if (!shouldCopySyncedFile(sourcePath, relativePath, destinationPath, targetHasLegacyPrefsDb,
		                          targetHasLegacyIni))
			return false;

		const bool ensuredDestinationDir = QDir().mkpath(QFileInfo(destinationPath).path());
		Q_UNUSED(ensuredDestinationDir);
		if (QFileInfo::exists(destinationPath) && !QFile::remove(destinationPath))
		{
			qWarning() << "Failed to replace synced file:" << destinationPath;
			return false;
		}

		if (!QFile::copy(sourcePath, destinationPath))
		{
			qWarning() << "Failed to sync file:" << sourcePath << "->" << destinationPath;
			return false;
		}

		QFile::setPermissions(destinationPath, QFile::permissions(sourcePath));
		return true;
	}

	void snapshotPreferencesDatabaseToMigratedDir(const QString &sourcePath, const QString &workingDir)
	{
		if (sourcePath.trimmed().isEmpty() || workingDir.trimmed().isEmpty())
			return;
		if (!QFileInfo::exists(sourcePath))
			return;

		const QString baseDir     = QDir::cleanPath(workingDir);
		const QString migratedDir = QDir(baseDir).filePath(QStringLiteral("migrated"));
		if (!QDir().mkpath(migratedDir))
		{
			qWarning() << "Failed to create migrated directory for preferences snapshot:" << migratedDir;
			return;
		}

		const QString targetPath = QDir(migratedDir).filePath(QFileInfo(sourcePath).fileName());
		if (QDir::cleanPath(targetPath) == QDir::cleanPath(sourcePath))
			return;
		if (QFileInfo::exists(targetPath))
			return;
		if (!QFile::copy(sourcePath, targetPath))
			qWarning() << "Failed to snapshot preferences database:" << sourcePath << "->" << targetPath;
	}

	QString migrateLegacyWorldFilePath(const QString &baseDir, const QString &path)
	{
		QString normalizedPath = stripOptionalQuotes(normalizePathString(path).trimmed());
		if (normalizedPath.startsWith(QStringLiteral("./")) && normalizedPath.size() > 3 &&
		    normalizedPath.at(2).isLetter() && normalizedPath.at(3) == QLatin1Char(':'))
		{
			normalizedPath = normalizedPath.mid(2);
		}
		if (normalizedPath.startsWith(QLatin1Char('/')) && normalizedPath.size() > 3 &&
		    normalizedPath.at(1).isLetter() && normalizedPath.at(2) == QLatin1Char(':'))
		{
			normalizedPath = normalizedPath.mid(1);
		}
		if (normalizedPath.trimmed().isEmpty())
			return normalizedPath;

		const QString suffix = QFileInfo(normalizedPath).suffix().toLower();
		if (!QMudFileExtensions::isWorldSuffix(suffix))
			return QMudFileExtensions::canonicalizePathExtension(normalizedPath);

		QString canonicalPath = QMudFileExtensions::canonicalizePathExtension(normalizedPath);
		if (!QMudFileExtensions::isLegacyWorldSuffix(suffix))
		{
			if (const QString modernAbsolutePath = absolutePathFromBase(baseDir, canonicalPath);
			    QFileInfo::exists(modernAbsolutePath))
				return storagePathFromAbsolute(baseDir, normalizedPath, modernAbsolutePath);
			if (const QString resolvedModernPath =
			        resolveExistingPathCaseInsensitive(absolutePathFromBase(baseDir, canonicalPath));
			    !resolvedModernPath.isEmpty())
				return storagePathFromAbsolute(baseDir, normalizedPath, resolvedModernPath);
			if (const QString remappedModernPath = resolveExistingPathCaseInsensitive(
			        remapLegacyWindowsWorldPathToBase(baseDir, canonicalPath));
			    !remappedModernPath.isEmpty())
			{
				return storagePathFromAbsolute(baseDir, normalizedPath, remappedModernPath);
			}
			const QString legacyFallbackPath =
			    QMudFileExtensions::replaceOrAppendExtension(normalizedPath, QStringLiteral("mcl"));
			if (const QString legacyFallbackAbsolutePath = absolutePathFromBase(baseDir, legacyFallbackPath);
			    QFileInfo::exists(legacyFallbackAbsolutePath) ||
			    !resolveExistingPathCaseInsensitive(legacyFallbackAbsolutePath).isEmpty())
				return migrateLegacyWorldFilePath(baseDir, legacyFallbackPath);
			return canonicalPath;
		}

		const QString legacyAbsolutePath = absolutePathFromBase(baseDir, normalizedPath);
		QString       resolvedLegacyPath = QFileInfo::exists(legacyAbsolutePath)
		                                       ? QDir::cleanPath(legacyAbsolutePath)
		                                       : resolveExistingPathCaseInsensitive(legacyAbsolutePath);
		if (resolvedLegacyPath.isEmpty())
		{
			if (const QString remappedLegacyPath = resolveExistingPathCaseInsensitive(
			        remapLegacyWindowsWorldPathToBase(baseDir, normalizedPath));
			    !remappedLegacyPath.isEmpty())
			{
				resolvedLegacyPath = remappedLegacyPath;
			}
		}
		const QString defaultModernAbsolutePath = absolutePathFromBase(baseDir, canonicalPath);
		QString       resolvedModernPath        = QFileInfo::exists(defaultModernAbsolutePath)
		                                              ? QDir::cleanPath(defaultModernAbsolutePath)
		                                              : resolveExistingPathCaseInsensitive(defaultModernAbsolutePath);
		if (resolvedModernPath.isEmpty())
		{
			if (const QString remappedModernPath = resolveExistingPathCaseInsensitive(
			        remapLegacyWindowsWorldPathToBase(baseDir, canonicalPath));
			    !remappedModernPath.isEmpty())
			{
				resolvedModernPath = remappedModernPath;
			}
		}

		if (resolvedLegacyPath.isEmpty())
		{
			if (!resolvedModernPath.isEmpty())
				return storagePathFromAbsolute(baseDir, normalizedPath, resolvedModernPath);
			return normalizedPath;
		}

		QString effectiveModernAbsolutePath =
		    !resolvedModernPath.isEmpty()
		        ? resolvedModernPath
		        : QMudFileExtensions::replaceOrAppendExtension(resolvedLegacyPath, QStringLiteral("qdl"));

		if (!QFileInfo::exists(effectiveModernAbsolutePath))
		{
			if (const QString modernDir = QFileInfo(effectiveModernAbsolutePath).absolutePath();
			    !QDir().mkpath(modernDir))
			{
				qWarning() << "Failed to create migrated world directory:" << modernDir;
				return normalizedPath;
			}

			QFile legacyFile(resolvedLegacyPath);
			if (!legacyFile.open(QIODevice::ReadOnly))
			{
				qWarning() << "Failed to read legacy world file:" << resolvedLegacyPath;
				return normalizedPath;
			}
			const QByteArray data = legacyFile.readAll();
			legacyFile.close();

			QSaveFile modernFile(effectiveModernAbsolutePath);
			if (!modernFile.open(QIODevice::WriteOnly))
			{
				qWarning() << "Failed to create migrated world file:" << effectiveModernAbsolutePath;
				return normalizedPath;
			}
			if (modernFile.write(data) != data.size() || !modernFile.commit())
			{
				qWarning() << "Failed to write migrated world file:" << effectiveModernAbsolutePath;
				return normalizedPath;
			}
		}

		const QString relativeLegacy = archiveRelativePathFor(baseDir, resolvedLegacyPath);
		const QString archivePath    = QDir(baseDir).filePath(QStringLiteral("migrated/") + relativeLegacy);
		if (!moveFileToArchive(resolvedLegacyPath, archivePath))
			qWarning() << "Failed to archive legacy world file:" << resolvedLegacyPath << "->" << archivePath;

		return storagePathFromAbsolute(baseDir, normalizedPath, effectiveModernAbsolutePath);
	}

	QString migrateWorldListPaths(const QString &baseDir, const QString &worldList, bool *changed = nullptr)
	{
		const QStringList items = splitSerializedWorldList(worldList);
		QStringList       migrated;
		migrated.reserve(items.size());
		bool anyChanged = false;
		for (const QString &item : items)
		{
			const QString value = migrateLegacyWorldFilePath(baseDir, item);
			if (value != item)
				anyChanged = true;
			migrated.push_back(value);
		}
		const QString joined = migrated.join(QLatin1Char('*'));
		if (changed)
			*changed = anyChanged || (joined != worldList);
		return joined;
	}

	QString canonicalizeWorldListForRuntime(const QString &worldList)
	{
		return splitSerializedWorldList(worldList).join(QLatin1Char('*'));
	}

	QString migrateLegacyPluginFilePath(const QString &baseDir, const QString &path)
	{
		QString normalizedPath = stripOptionalQuotes(normalizePathString(path).trimmed());
		if (normalizedPath.startsWith(QStringLiteral("./")) && normalizedPath.size() > 3 &&
		    normalizedPath.at(2).isLetter() && normalizedPath.at(3) == QLatin1Char(':'))
		{
			normalizedPath = normalizedPath.mid(2);
		}
		if (normalizedPath.startsWith(QLatin1Char('/')) && normalizedPath.size() > 3 &&
		    normalizedPath.at(1).isLetter() && normalizedPath.at(2) == QLatin1Char(':'))
		{
			normalizedPath = normalizedPath.mid(1);
		}
		if (normalizedPath.trimmed().isEmpty())
			return normalizedPath;

		const QString defaultAbsolutePath = absolutePathFromBase(baseDir, normalizedPath);
		if (const QString resolvedPath = QFileInfo::exists(defaultAbsolutePath)
		                                     ? QDir::cleanPath(defaultAbsolutePath)
		                                     : resolveExistingPathCaseInsensitive(defaultAbsolutePath);
		    !resolvedPath.isEmpty())
		{
			return storagePathFromAbsolute(baseDir, normalizedPath, resolvedPath);
		}

		if (const QString remappedPath = resolveExistingPathCaseInsensitive(
		        remapLegacyWindowsWorldPathToBase(baseDir, normalizedPath));
		    !remappedPath.isEmpty())
		{
			return storagePathFromAbsolute(baseDir, normalizedPath, remappedPath);
		}

		return normalizedPath;
	}

	QString migratePluginListPaths(const QString &baseDir, const QString &pluginList, bool *changed = nullptr)
	{
		const QStringList items = splitSerializedPathList(pluginList);
		QStringList       migrated;
		migrated.reserve(items.size());
		bool anyChanged = false;
		for (const QString &item : items)
		{
			const QString value = migrateLegacyPluginFilePath(baseDir, item);
			if (value != item)
				anyChanged = true;
			migrated.push_back(value);
		}
		const QString joined = migrated.join(QLatin1Char('*'));
		if (changed)
			*changed = anyChanged || (joined != pluginList);
		return joined;
	}

	QString canonicalizePluginListForRuntime(const QString &pluginList)
	{
		return splitSerializedPathList(pluginList).join(QLatin1Char('*'));
	}

	void migrateLegacyWorldTree(const QString &baseDir, const QString &worldDirectory)
	{
		const QString normalizedWorldDir = normalizePathString(worldDirectory.trimmed());
		if (normalizedWorldDir.isEmpty())
			return;

		const QString absoluteWorldDir = absolutePathFromBase(baseDir, normalizedWorldDir);
		const QDir    rootDir(absoluteWorldDir);
		if (!rootDir.exists())
			return;

		QStringList  legacyWorldFiles;
		QDirIterator iterator(rootDir.absolutePath(),
		                      QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
		                      QDirIterator::Subdirectories);
		while (iterator.hasNext())
		{
			const QString filePath = iterator.next();
			const QString suffix   = iterator.fileInfo().suffix().toLower();
			if (!QMudFileExtensions::isLegacyWorldSuffix(suffix))
				continue;
			legacyWorldFiles.push_back(QDir::cleanPath(filePath));
		}

		for (const QString &legacyAbsolutePath : legacyWorldFiles)
		{
			const QString migrationInput = archiveRelativePathFor(baseDir, legacyAbsolutePath);
			(void)migrateLegacyWorldFilePath(baseDir, migrationInput);
		}
	}

	QString migrateLegacyIniPathValue(const QString &baseDir, const QString &key, const QString &value)
	{
		const QString leaf = keyLeafName(key);
		if (isPathListKey(leaf))
		{
			if (leaf.compare(QStringLiteral("WorldList"), Qt::CaseInsensitive) == 0)
				return migrateWorldListPaths(baseDir, value);
			if (leaf.compare(QStringLiteral("PluginList"), Qt::CaseInsensitive) == 0)
				return migratePluginListPaths(baseDir, value);
			return normalizePathList(value);
		}

		if (!shouldNormalizePathKey(leaf))
			return value;

		const QString normalized = normalizePathString(value);
		if (const QString suffix = QFileInfo(normalized).suffix().toLower();
		    QMudFileExtensions::isWorldSuffix(suffix))
			return migrateLegacyWorldFilePath(baseDir, normalized);
		return QMudFileExtensions::canonicalizePathExtension(normalized);
	}

	QVariant migrateLegacyIniValue(const QString &baseDir, const QString &key, const QVariant &value)
	{
		if (value.canConvert<QStringList>())
		{
			const QStringList list = value.toStringList();
			QStringList       migrated;
			migrated.reserve(list.size());
			for (const QString &item : list)
				migrated.push_back(migrateLegacyIniPathValue(baseDir, key, item));
			return {migrated};
		}

		if (value.canConvert<QString>())
			return migrateLegacyIniPathValue(baseDir, key, value.toString());

		return value;
	}

	bool isLikelyCorruptedRelativeWorldPath(const QString &path)
	{
		const QString normalized = normalizePathString(path.trimmed());
		if (normalized.isEmpty())
			return false;
		if (const QString suffix = QFileInfo(normalized).suffix().toLower();
		    !QMudFileExtensions::isWorldSuffix(suffix))
			return false;
		if (!normalized.startsWith(QLatin1Char('.')))
			return false;
		return !normalized.contains(QLatin1Char('/'));
	}

	QHash<QString, QString> parseLegacyIniRawValues(const QString &iniPath)
	{
		QHash<QString, QString> values;
		QFile                   file(iniPath);
		if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
			return values;

		QString           currentSection;
		const QString     content = QString::fromUtf8(file.readAll());
		const QStringList lines =
		    content.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::KeepEmptyParts);
		for (QString line : lines)
		{
			line = line.trimmed();
			if (line.isEmpty())
				continue;
			if (line.startsWith(QLatin1Char(';')) || line.startsWith(QLatin1Char('#')))
				continue;

			if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']')) && line.size() >= 2)
			{
				currentSection = line.mid(1, line.size() - 2).trimmed();
				continue;
			}

			const qsizetype equalsPos = line.indexOf(QLatin1Char('='));
			if (equalsPos < 0)
				continue;

			const QString key   = line.left(equalsPos).trimmed();
			const QString value = line.mid(equalsPos + 1);
			if (key.isEmpty())
				continue;

			const QString fullKey = currentSection.isEmpty() ? key : currentSection + QLatin1Char('/') + key;
			values.insert(fullKey, value);
		}
		return values;
	}

	QVariant migrateLegacyIniValueWithRawSource(const QString &baseDir, const QString &key,
	                                            const QVariant                &value,
	                                            const QHash<QString, QString> &rawValues)
	{
		if (const QString leaf = keyLeafName(key);
		    (isPathListKey(leaf) || shouldNormalizePathKey(leaf)) && rawValues.contains(key))
			return migrateLegacyIniPathValue(baseDir, key, rawValues.value(key));
		return migrateLegacyIniValue(baseDir, key, value);
	}

	void pruneLegacyCtrlBarsGroups(QSettings &modern)
	{
		for (const QStringList keys = modern.allKeys(); const QString &key : keys)
		{
			if (key.startsWith(QStringLiteral("CtrlBars-Bar"), Qt::CaseInsensitive) ||
			    key.startsWith(QStringLiteral("CtrlBars-Summary"), Qt::CaseInsensitive))
			{
				modern.remove(key);
			}
		}
	}

	void migrateLegacyIniToQmudConf(const QString &workingDir)
	{
		const QString baseDir    = QDir::cleanPath(workingDir);
		const QString legacyPath = QDir(baseDir).filePath(QStringLiteral("MUSHclient.ini"));
		if (!QFileInfo::exists(legacyPath))
			return;

		const QString                 newPath = QDir(baseDir).filePath(QStringLiteral("QMud.conf"));
		const QSettings               legacy(legacyPath, QSettings::IniFormat);
		QSettings                     modern(newPath, QSettings::IniFormat);
		const QHash<QString, QString> rawValues = parseLegacyIniRawValues(legacyPath);
		for (const QStringList keys = legacy.allKeys(); const QString &key : keys)
			modern.setValue(key,
			                migrateLegacyIniValueWithRawSource(baseDir, key, legacy.value(key), rawValues));
		pruneLegacyCtrlBarsGroups(modern);
		modern.sync();

		const QString migratedDir = QDir(baseDir).filePath(QStringLiteral("migrated"));
		if (!QDir().mkpath(migratedDir))
		{
			qWarning() << "Failed to create migrated directory:" << migratedDir;
			return;
		}

		const QString migratedPath = QDir(migratedDir).filePath(QStringLiteral("MUSHclient.ini"));
		if (QFileInfo::exists(migratedPath))
			QFile::remove(migratedPath);
		if (!QFile::rename(legacyPath, migratedPath))
			qWarning() << "Failed to archive legacy config file:" << legacyPath << "->" << migratedPath;
	}
} // namespace

namespace
{
	bool showHelpDoc(QWidget *parent, const QString &docName)
	{
		const QString dbPath = AppController::resolveHelpDatabasePath();
		if (dbPath.isEmpty())
		{
			QMessageBox::information(parent, QStringLiteral("Help"),
			                         QStringLiteral("Help database (help.db) not found."));
			return false;
		}
		static int    connectionCounter = 0;
		const QString connectionName    = QStringLiteral("help_db_%1").arg(++connectionCounter);
		QString       title;
		QString       description;
		bool          opened = false;
		{
			QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
			db.setDatabaseName(dbPath);
			db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
			if (!db.open())
			{
				QMessageBox::warning(parent, QStringLiteral("Help"),
				                     QStringLiteral("Unable to open help database."));
			}
			else
			{
				opened = true;
				QSqlQuery query(db);
				query.prepare(
				    QStringLiteral("SELECT title, description FROM general_doc WHERE doc_name = ?"));
				query.addBindValue(docName);
				if (query.exec() && query.next())
				{
					title       = query.value(0).toString();
					description = query.value(1).toString();
				}
			}
			db.close();
		}
		QSqlDatabase::removeDatabase(connectionName);
		if (!opened)
		{
			return false;
		}

		if (description.isEmpty())
			return false;

		QDialog dialog(parent);
		dialog.setWindowTitle(title.isEmpty() ? QStringLiteral("Help") : title);
		dialog.setWindowFlags(dialog.windowFlags() | Qt::Tool);
		dialog.setMinimumSize(820, 600);
		QVBoxLayout      layout(&dialog);
		QTextBrowser     browser(&dialog);
		QDialogButtonBox buttons(QDialogButtonBox::Close, &dialog);
		browser.setOpenExternalLinks(true);
		browser.setHtml(Qt::convertFromPlainText(description));
		layout.addWidget(&browser);
		QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
		QObject::connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
		layout.addWidget(&buttons);
		dialog.exec();
		return true;
	}

	QString loadResourceText(const QString &resourcePath)
	{
		QFile file(resourcePath);
		if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
			return {};
		return QString::fromUtf8(file.readAll());
	}

	void showTextDialog(QWidget *parent, const QString &title, const QString &text)
	{
		QDialog dialog(parent);
		dialog.setWindowTitle(title);
		dialog.setWindowFlags(dialog.windowFlags() | Qt::Tool);
		dialog.setMinimumSize(820, 600);
		QVBoxLayout      layout(&dialog);
		QPlainTextEdit   edit(&dialog);
		QDialogButtonBox buttons(QDialogButtonBox::Close, &dialog);
		edit.setReadOnly(true);
		edit.setPlainText(text);
		edit.setFont(qmudPreferredMonospaceFont());
		layout.addWidget(&edit);
		QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
		QObject::connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
		layout.addWidget(&buttons);
		dialog.exec();
	}
} // namespace

AppController *AppController::s_instance = nullptr;

AppController::AppController(QObject *parent) : QObject(parent)
{
	s_instance = this;
	m_nameGenerator.reset(new NameGenerator(this));
}

AppController::~AppController()
{
#ifdef QMUD_ENABLE_LUA_I18N
	if (m_translatorLua)
	{
		lua_close(m_translatorLua);
		m_translatorLua = nullptr;
	}
#endif
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	if (m_spellCheckerLua)
	{
		lua_close(m_spellCheckerLua);
		m_spellCheckerLua = nullptr;
	}
#endif
}

AppController *AppController::instance()
{
	return s_instance;
}

QString AppController::resolveHelpDatabasePath()
{
	const auto appDir = QCoreApplication::applicationDirPath();
	if (const auto direct = QDir(appDir).filePath(QStringLiteral("help.db")); QFileInfo::exists(direct))
		return direct;
	if (const auto cwd = QDir::current().filePath(QStringLiteral("help.db")); QFileInfo::exists(cwd))
		return cwd;
	return {};
}

NameGenerator *AppController::nameGenerator()
{
	return m_nameGenerator.data();
}

const NameGenerator *AppController::nameGenerator() const
{
	return m_nameGenerator.data();
}

qint64 AppController::nextUniqueNumber()
{
	return m_uniqueNumber.fetch_add(1);
}

void AppController::seedRandom(const quint32 seed)
{
	m_rng.seed(seed);
}

void AppController::seedRandomFromArray(const QVector<quint32> &values)
{
	if (values.isEmpty())
		return;

	quint32 seed = 0x6d2b79f5u;
	for (const quint32 value : values)
		seed ^= value + 0x9e3779b9u + (seed << 6) + (seed >> 2);
	m_rng.seed(seed);
}

double AppController::nextRandomUnit()
{
	return m_rng.generateDouble();
}

void AppController::setMainWindow(MainWindow *window)
{
	m_mainWindow = window;
	if (m_mainWindow)
	{
		connect(m_mainWindow, &MainWindow::commandTriggered, this, &AppController::onCommandTriggered);
		connect(m_mainWindow, &MainWindow::viewPreferenceChanged, this,
		        [this](const QString &, int) { saveViewPreferences(); });
		connect(m_mainWindow, &MainWindow::recentFileTriggered, this,
		        [this](const QString &path) { openDocumentFile(path); });
	}
}

MainWindow *AppController::mainWindow() const
{
	return m_mainWindow;
}

void AppController::checkForUpdatesNow(QWidget *uiParent)
{
	requestUpdateCheck(true, uiParent);
}

bool AppController::isUpdateMechanismAvailable()
{
	return detectUpdateInstallTarget() != UpdateInstallTarget::Unsupported;
}

QString AppController::updateMechanismUnavailableReason()
{
	if (isUpdateMechanismAvailable())
		return {};
	return updateNotSupportedMessage();
}

void AppController::startWithSplash()
{
	if (m_startupFinalized)
		return;

	static constexpr int kMinSplashVisibleMs = 500;
	m_splashMinDelayElapsed                  = false;
	m_initializeFinished                     = false;
	m_initializeSucceeded                    = false;
	m_startupFinalized                       = false;

	showSplashScreen();

	QTimer::singleShot(kMinSplashVisibleMs, this,
	                   [this]
	                   {
		                   m_splashMinDelayElapsed = true;
		                   finalizeStartupIfReady();
	                   });

	QTimer::singleShot(0, this,
	                   [this]
	                   {
		                   m_initializeSucceeded = initialize();
		                   m_initializeFinished  = true;
		                   finalizeStartupIfReady();
	                   });
}

void AppController::setActivityDocument(ActivityDocument *doc)
{
	m_activityDoc = doc;
}

ActivityDocument *AppController::activityDocument() const
{
	return m_activityDoc;
}

QVector<WorldRuntime *> AppController::activeWorldRuntimes() const
{
	QVector<WorldRuntime *> runtimes;
	const auto             *host = resolveMainWindowHost(m_mainWindow);
	if (!host)
		return runtimes;
	const auto entries = host->worldWindowDescriptors();
	runtimes.reserve(entries.size());
	for (const auto &entry : entries)
	{
		if (entry.runtime)
			runtimes.push_back(entry.runtime);
	}
	return runtimes;
}

bool AppController::saveDirtyAutoSaveWorldsBeforeRestart(QString *errorMessage) const
{
	if (errorMessage)
		errorMessage->clear();

	const MainWindowHost *host = resolveMainWindowHost(m_mainWindow);
	if (!host)
		return true;

	const QVector<WorldWindowDescriptor> entries = host->worldWindowDescriptors();
	for (const WorldWindowDescriptor &entry : entries)
	{
		WorldRuntime *runtime = entry.runtime;
		if (!runtime)
			continue;

		const QMap<QString, QString> &attrs = runtime->worldAttributes();
		if (!isEnabledFlag(attrs.value(QStringLiteral("save_world_automatically"))))
			continue;
		if (!runtime->worldFileModified() && !runtime->variablesChanged())
			continue;

		const QString worldName = worldDisplayNameForRestartSave(entry);
		const QString filePath  = ensureWorldFilePathForRestartSave(runtime, *this);
		if (filePath.trimmed().isEmpty())
		{
			if (errorMessage)
			{
				*errorMessage =
				    QStringLiteral("Unable to determine save path for world \"%1\".").arg(worldName);
			}
			return false;
		}

		QString saveError;
		if (!runtime->saveWorldFile(filePath, &saveError))
		{
			if (errorMessage)
			{
				*errorMessage =
				    QStringLiteral("Unable to save world \"%1\" before restart: %2")
				        .arg(worldName, saveError.isEmpty() ? QStringLiteral("Unknown error.") : saveError);
			}
			return false;
		}
		runtime->setVariablesChanged(false);
		runtime->setWorldFileModified(false);
	}

	return true;
}

bool AppController::closeOpenWorldLogsBeforeRestart(QString *errorMessage) const
{
	if (errorMessage)
		errorMessage->clear();

	const MainWindowHost *host = resolveMainWindowHost(m_mainWindow);
	if (!host)
		return true;

	const QVector<WorldWindowDescriptor> entries = host->worldWindowDescriptors();
	for (const WorldWindowDescriptor &entry : entries)
	{
		WorldRuntime *runtime = entry.runtime;
		if (!runtime || !runtime->isLogOpen())
			continue;

		const QString worldName = worldDisplayNameForRestartSave(entry);
		if (const int result = runtime->closeLog(); result != eOK)
		{
			if (errorMessage)
			{
				*errorMessage = QStringLiteral("Unable to close log for world \"%1\".").arg(worldName);
			}
			return false;
		}
	}

	return true;
}

bool AppController::saveOpenWorldSessionStatesBeforeRestart(QString *errorMessage) const
{
	if (errorMessage)
		errorMessage->clear();

	const MainWindowHost *host = resolveMainWindowHost(m_mainWindow);
	if (!host)
		return true;

	const QVector<WorldWindowDescriptor> entries = host->worldWindowDescriptors();
	for (const WorldWindowDescriptor &entry : entries)
	{
		WorldRuntime *runtime = entry.runtime;
		WorldView    *view    = entry.window ? entry.window->view() : nullptr;
		if (!runtime || !view)
			continue;

		bool       saveOk    = true;
		QString    saveError = {};
		bool       done      = false;
		QEventLoop waitLoop;
		saveWorldSessionStateAsync(
		    runtime, view,
		    [&saveOk, &saveError, &done, &waitLoop](const bool ok, const QString &error)
		    {
			    saveOk    = ok;
			    saveError = error;
			    done      = true;
			    waitLoop.quit();
		    });
		if (!done)
			waitLoop.exec();
		if (!saveOk)
		{
			if (errorMessage)
			{
				const QString worldName = worldDisplayNameForRestartSave(entry);
				*errorMessage =
				    QStringLiteral("Unable to persist session state for world \"%1\": %2")
				        .arg(worldName, saveError.isEmpty() ? QStringLiteral("Unknown error.") : saveError);
			}
			return false;
		}
	}

	return true;
}

bool AppController::saveOpenWorldPluginStatesBeforeRestart(QString *errorMessage) const
{
	if (errorMessage)
		errorMessage->clear();

	const MainWindowHost *host = resolveMainWindowHost(m_mainWindow);
	if (!host)
		return true;

	const QVector<WorldWindowDescriptor> entries = host->worldWindowDescriptors();
	for (const WorldWindowDescriptor &entry : entries)
	{
		WorldRuntime *runtime = entry.runtime;
		if (!runtime)
			continue;

		QStringList pluginIds;
		for (const WorldRuntime::Plugin &plugin : runtime->plugins())
		{
			const QString pluginId = plugin.attributes.value(QStringLiteral("id")).trimmed();
			if (!pluginId.isEmpty())
				pluginIds.push_back(pluginId);
		}

		for (const QString &pluginId : std::as_const(pluginIds))
		{
			QString   pluginSaveError;
			const int result = runtime->savePluginState(pluginId, false, &pluginSaveError);
			if (result == eNoSuchPlugin)
				continue;
			if (result == eOK && pluginSaveError.trimmed().isEmpty())
				continue;

			if (errorMessage)
			{
				const QString worldName = worldDisplayNameForRestartSave(entry);
				const QString detail    = !pluginSaveError.trimmed().isEmpty()
				                              ? pluginSaveError.trimmed()
				                              : QStringLiteral("SaveState returned code %1").arg(result);
				*errorMessage = QStringLiteral("Unable to save plugin state for world \"%1\" (plugin %2): %3")
				                    .arg(worldName, pluginId, detail);
			}
			return false;
		}
	}

	return true;
}

QString AppController::worldSessionStateDirectoryPath() const
{
	QString baseDir = m_workingDir.trimmed();
	if (baseDir.isEmpty())
		baseDir = makeAbsolutePath(QStringLiteral("."));
	if (baseDir.isEmpty())
		baseDir = QCoreApplication::applicationDirPath();
	return QDir(baseDir).filePath(QString::fromLatin1(kWorldSessionStateDir));
}

QString AppController::worldSessionStateFilePath(const WorldRuntime *runtime) const
{
	if (!runtime)
		return {};

	QString worldId = runtime->worldAttributes().value(QStringLiteral("id")).trimmed().toLower();
	if (worldId.isEmpty())
		return {};
	for (QChar &ch : worldId)
	{
		if (!ch.isLetterOrNumber() && ch != QLatin1Char('_') && ch != QLatin1Char('-'))
			ch = QLatin1Char('_');
	}
	if (worldId.isEmpty())
		return {};

	return QDir(worldSessionStateDirectoryPath())
	    .filePath(worldId + QString::fromLatin1(kWorldSessionStateSuffix));
}

void AppController::saveWorldSessionStateAsync(const WorldRuntime *runtime, const WorldView *view,
                                               std::function<void(bool, const QString &)> completion) const
{
	const auto completionFn =
	    QSharedPointer<std::function<void(bool, const QString &)>>::create(std::move(completion));
	const auto finish = [completionFn](const bool ok, const QString &error)
	{
		if (completionFn && *completionFn)
			(*completionFn)(ok, error);
	};

	if (!runtime || !view)
	{
		finish(false, QStringLiteral("Missing runtime or view."));
		return;
	}

	const QString                 filePath = worldSessionStateFilePath(runtime);
	const QMap<QString, QString> &attrs    = runtime->worldAttributes();
	const bool persistOutputBuffer   = isEnabledFlag(attrs.value(QStringLiteral("persist_output_buffer")));
	const bool persistCommandHistory = isEnabledFlag(attrs.value(QStringLiteral("persist_command_history")));
	if (filePath.trimmed().isEmpty())
	{
		if (persistOutputBuffer || persistCommandHistory)
			finish(false, QStringLiteral("World session-state path is not available."));
		else
			finish(true, QString());
		return;
	}

	QMudWorldSessionState::WorldSessionStateData state;
	state.hasOutputBuffer   = persistOutputBuffer;
	state.hasCommandHistory = persistCommandHistory;
	if (persistOutputBuffer)
		state.outputLines = runtime->lines();
	if (persistCommandHistory)
		state.commandHistory = view->commandHistoryList();

	QThreadPool::globalInstance()->start(
	    [filePath, state = std::move(state), completionFn]
	    {
		    QString error;
		    bool    ok = true;
		    if (!state.hasOutputBuffer && !state.hasCommandHistory)
			    ok = QMudWorldSessionState::removeSessionStateFile(filePath, &error);
		    else
			    ok = QMudWorldSessionState::writeSessionStateFile(filePath, state, &error);

		    QMetaObject::invokeMethod(
		        qApp,
		        [completionFn, ok, error]
		        {
			        if (completionFn && *completionFn)
				        (*completionFn)(ok, error);
		        },
		        Qt::QueuedConnection);
	    });
}

void AppController::beginRestoreScrollbackStatus() const
{
	if (!m_mainWindow)
		return;
	if (m_restoreScrollbackInFlight == 0)
	{
		if (const WorldChildWindow *world = m_mainWindow->activeWorldChildWindow())
			m_restoreScrollbackStatusRuntime = world->runtime();
		else
			m_restoreScrollbackStatusRuntime = nullptr;
		m_restoreScrollbackStatusPrevious =
		    m_restoreScrollbackStatusRuntime ? m_restoreScrollbackStatusRuntime->statusMessage() : QString{};
	}
	++m_restoreScrollbackInFlight;
	m_mainWindow->setStatusMessageNow(
	    QMudWorldSessionRestoreFlow::restoreScrollbackStatusMessage(m_restoreScrollbackInFlight));
}

void AppController::endRestoreScrollbackStatus() const
{
	if (m_restoreScrollbackInFlight <= 0)
		return;
	--m_restoreScrollbackInFlight;
	if (!m_mainWindow)
		return;
	if (m_restoreScrollbackInFlight > 0)
	{
		m_mainWindow->setStatusMessageNow(
		    QMudWorldSessionRestoreFlow::restoreScrollbackStatusMessage(m_restoreScrollbackInFlight));
		return;
	}

	WorldRuntime *activeRuntime = nullptr;
	if (const WorldChildWindow *world = m_mainWindow->activeWorldChildWindow())
		activeRuntime = world->runtime();
	if (activeRuntime == m_restoreScrollbackStatusRuntime && !m_restoreScrollbackStatusPrevious.isEmpty())
		m_mainWindow->setStatusMessageNow(m_restoreScrollbackStatusPrevious);
	else
		m_mainWindow->setStatusNormal();

	m_restoreScrollbackStatusRuntime = nullptr;
	m_restoreScrollbackStatusPrevious.clear();
	maybeShowDeferredUpgradeWelcomeAfterStartupRestores();
}

void AppController::restoreWorldSessionStateAsync(WorldRuntime *runtime, WorldView *view,
                                                  std::function<void(bool, const QString &)> completion) const
{
	const auto completionFn =
	    QSharedPointer<std::function<void(bool, const QString &)>>::create(std::move(completion));
	const auto finish = [completionFn](const bool ok, const QString &error)
	{
		if (completionFn && *completionFn)
			(*completionFn)(ok, error);
	};

	if (!runtime || !view)
	{
		finish(false, QStringLiteral("Missing runtime or view."));
		return;
	}

	const QPointer<WorldRuntime> runtimeGuard(runtime);
	const QPointer<WorldView>    viewGuard(view);
	const QString                filePath = worldSessionStateFilePath(runtime);
	const QMap<QString, QString> attrs    = runtime->worldAttributes();
	const bool persistOutputBuffer   = isEnabledFlag(attrs.value(QStringLiteral("persist_output_buffer")));
	const bool persistCommandHistory = isEnabledFlag(attrs.value(QStringLiteral("persist_command_history")));
	if (filePath.trimmed().isEmpty())
	{
		finish(true, QString());
		return;
	}
	const auto loadPlan = QMudWorldSessionRestoreFlow::computeSessionStateLoadPlan(
	    persistOutputBuffer, persistCommandHistory, QFileInfo::exists(filePath));
	const bool trackScrollbackRestoreStatus =
	    QMudWorldSessionRestoreFlow::shouldTrackScrollbackRestoreStatus(persistOutputBuffer, loadPlan);
	const QPointer<AppController> controllerGuard(const_cast<AppController *>(this));
	if (trackScrollbackRestoreStatus && controllerGuard)
		controllerGuard->beginRestoreScrollbackStatus();

	QThreadPool::globalInstance()->start(
	    [controllerGuard, runtimeGuard, viewGuard, filePath, persistOutputBuffer, persistCommandHistory,
	     completionFn, loadPlan, trackScrollbackRestoreStatus]
	    {
		    QString                                      error;
		    bool                                         ok = true;
		    QMudWorldSessionState::WorldSessionStateData state;
		    switch (loadPlan)
		    {
		    case QMudWorldSessionRestoreFlow::SessionStateLoadPlan::RemoveFileAndSucceed:
			    ok = QMudWorldSessionState::removeSessionStateFile(filePath, &error);
			    break;
		    case QMudWorldSessionRestoreFlow::SessionStateLoadPlan::ReadFileAndApply:
			    ok = QMudWorldSessionState::readSessionStateFile(filePath, &state, &error);
			    break;
		    case QMudWorldSessionRestoreFlow::SessionStateLoadPlan::SkipApplyAndSucceed:
			    break;
		    }

		    QMetaObject::invokeMethod(
		        qApp,
		        [runtimeGuard, viewGuard, completionFn, persistOutputBuffer, persistCommandHistory,
		         state = std::move(state), ok, error, trackScrollbackRestoreStatus, controllerGuard]
		        {
			        const auto finishTrackedRestore = [trackScrollbackRestoreStatus, controllerGuard]
			        {
				        if (trackScrollbackRestoreStatus && controllerGuard)
					        controllerGuard->endRestoreScrollbackStatus();
			        };

			        if (!runtimeGuard || !viewGuard)
			        {
				        if (completionFn && *completionFn)
					        (*completionFn)(false, QStringLiteral("Runtime or view was destroyed."));
				        finishTrackedRestore();
				        return;
			        }

			        if (ok)
			        {
				        if (persistOutputBuffer && state.hasOutputBuffer)
					        runtimeGuard->replaceOutputLines(state.outputLines);
				        if (persistCommandHistory && state.hasCommandHistory)
					        viewGuard->setCommandHistoryList(state.commandHistory);
			        }

			        if (completionFn && *completionFn)
				        (*completionFn)(ok, error);
			        finishTrackedRestore();
		        },
		        Qt::QueuedConnection);
	    });
}

bool AppController::restoreWorldSessionStateSync(WorldRuntime *runtime, WorldView *view,
                                                 QString *errorMessage) const
{
	if (errorMessage)
		errorMessage->clear();

	bool       loadOk = true;
	QString    loadError;
	bool       done = false;
	QEventLoop waitLoop;
	restoreWorldSessionStateAsync(runtime, view,
	                              [&loadOk, &loadError, &done, &waitLoop](const bool ok, const QString &error)
	                              {
		                              loadOk    = ok;
		                              loadError = error;
		                              done      = true;
		                              waitLoop.quit();
	                              });
	if (!done)
		waitLoop.exec();
	if (!loadOk && errorMessage)
		*errorMessage = loadError;
	return loadOk;
}

void AppController::runWorldStartupPostRestore(WorldRuntime *runtime) const
{
	if (!runtime)
		return;

	emitStartupBanner(runtime);
	loadGlobalPlugins(runtime);
	runtime->setPluginInstallDeferred(false);
	runtime->installPendingPlugins();
}

void AppController::detectReloadStartupArguments()
{
	m_reloadLaunchRequested = false;
	m_reloadStatePathArg.clear();
	m_reloadTokenArg.clear();

	QString statePath;
	QString token;
	if (!parseReloadStartupArguments(QCoreApplication::arguments(), &statePath, &token))
		return;

	m_reloadLaunchRequested = true;
	m_reloadStatePathArg    = statePath;
	m_reloadTokenArg        = token;
}

void AppController::cleanupReloadStateOnNormalStartup() const
{
	const QString statePath = reloadStateDefaultPath(m_workingDir);
	QString       error;
	if (!removeReloadStateFile(statePath, &error) && !error.isEmpty())
		qWarning() << "Unable to remove stale reload state file:" << error;
}

bool AppController::openDocumentFile(const QString &path)
{
	// Open-document flow with world/template routing.
	if (path.isEmpty())
		return openWorldDocument(QString());

	QString normalized = path;
#ifdef Q_OS_WIN
	normalized = QDir::fromNativeSeparators(normalized);
#else
	normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
#endif
	auto resolvedPath = normalized;
	if (const QFileInfo info(normalized); !info.isAbsolute())
		resolvedPath = makeAbsolutePath(normalized);

	const auto suffix = QFileInfo(resolvedPath).suffix().toLower();
	const auto opened = QMudFileExtensions::isWorldSuffix(suffix) ? openWorldDocument(resolvedPath)
	                                                              : openTextDocument(resolvedPath);
	if (!opened)
	{
		return false;
	}

	// update MRU list
	QSettings settings(iniFilePath(), QSettings::IniFormat);
	settings.beginGroup(QStringLiteral("Recent File List"));
	QStringList files;
	QStringList fileKeys;
	for (int i = 1; i <= 4; ++i)
	{
		const auto key = QStringLiteral("File%1").arg(i);
		if (const auto value = settings.value(key).toString(); !value.isEmpty())
		{
			const QString migrated = migrateLegacyIniPathValue(m_workingDir, key, value);
			if (isLikelyCorruptedRelativeWorldPath(migrated))
			{
				settings.setValue(key, QString());
				continue;
			}
			const QString stored     = preferredMruStoragePath(migrated, m_workingDir);
			const QString compareKey = mruComparisonKey(stored, m_workingDir);
			if (compareKey.isEmpty())
				continue;
			if (fileKeys.contains(compareKey))
				continue;
			files.push_back(stored);
			fileKeys.push_back(compareKey);
		}
	}
	const auto recentPath       = QMudFileExtensions::isWorldSuffix(suffix)
	                                  ? QMudFileExtensions::canonicalizePathExtension(resolvedPath)
	                                  : resolvedPath;
	const auto recentStoredPath = preferredMruStoragePath(recentPath, m_workingDir);
	const auto recentCompareKey = mruComparisonKey(recentStoredPath, m_workingDir);
	for (qsizetype i = fileKeys.size() - 1; i >= 0; --i)
	{
		if (fileKeys.at(i) == recentCompareKey)
		{
			fileKeys.removeAt(i);
			files.removeAt(i);
		}
	}
	files.prepend(recentStoredPath);
	fileKeys.prepend(recentCompareKey);
	while (files.size() > 4)
	{
		files.removeLast();
		fileKeys.removeLast();
	}
	for (int i = 1; i <= 4; ++i)
	{
		const QString key = QStringLiteral("File%1").arg(i);
		settings.setValue(key, i <= files.size() ? files[i - 1] : QString());
	}
	settings.endGroup();
	if (m_mainWindow)
		m_mainWindow->setRecentFiles(files);
	return true;
}

void AppController::openWorldsFromList(const QStringList &items, const bool activateFirstOnly)
{
	if (items.isEmpty())
		return;

	const bool    prevBatchMode      = m_batchOpeningWorldList;
	const bool    prevFirstActivated = m_batchWorldListActivatedFirst;
	WorldRuntime *firstOpenedRuntime = nullptr;
	if (activateFirstOnly)
	{
		m_batchOpeningWorldList        = true;
		m_batchWorldListActivatedFirst = false;
	}

	for (const QString &item : items)
	{
		const bool opened = openDocumentFile(item);
		if (!activateFirstOnly || !opened || firstOpenedRuntime != nullptr || !m_mainWindow)
			continue;
		if (WorldChildWindow *world = m_mainWindow->activeWorldChildWindow())
			firstOpenedRuntime = world->runtime();
	}

	if (activateFirstOnly && firstOpenedRuntime != nullptr && m_mainWindow)
		m_mainWindow->activateWorldRuntime(firstOpenedRuntime);

	if (activateFirstOnly)
	{
		m_batchOpeningWorldList        = prevBatchMode;
		m_batchWorldListActivatedFirst = prevFirstActivated;
	}
}

void AppController::restoreWorldWindowPlacement(const QString &worldName, QMdiSubWindow *window) const
{
	if (!window)
		return;

	const QString trimmedName = worldName.trimmed();
	if (trimmedName.isEmpty())
		return;

	const auto keyBase = trimmedName + QStringLiteral(" World Position");
	const auto current = window->geometry();
	const auto left =
	    dbGetInt(QStringLiteral("worlds"), keyBase + QStringLiteral(":wp.left"), current.left());
	const auto right =
	    dbGetInt(QStringLiteral("worlds"), keyBase + QStringLiteral(":wp.right"), current.right());
	const auto top = dbGetInt(QStringLiteral("worlds"), keyBase + QStringLiteral(":wp.top"), current.top());
	const auto bottom =
	    dbGetInt(QStringLiteral("worlds"), keyBase + QStringLiteral(":wp.bottom"), current.bottom());
	const auto showCmd = dbGetInt(QStringLiteral("worlds"), keyBase + QStringLiteral(":wp.showCmd"), 1);

	const auto width = right - left;
	if (const auto height = bottom - top; width > 0 && height > 0)
		window->setGeometry(QRect(left, top, width, height));

	if (showCmd == 3)
		window->showMaximized();
	else
		window->showNormal();
}

void AppController::saveWorldWindowPlacement(const QString &worldName, const QMdiSubWindow *window) const
{
	if (!window)
		return;

	const QString trimmedName = worldName.trimmed();
	if (trimmedName.isEmpty())
		return;

	const auto keyBase = trimmedName + QStringLiteral(" World Position");
	auto       rc      = window->geometry();
	if (window->isMaximized())
	{
		if (window->normalGeometry().isValid() && !window->normalGeometry().isNull())
			rc = window->normalGeometry();
	}
	if (!rc.isValid() || rc.isNull())
		return;

	(void)dbWriteInt(QStringLiteral("worlds"), keyBase + QStringLiteral(":wp.showCmd"),
	                 window->isMaximized() ? 3 : 1);
	(void)dbWriteInt(QStringLiteral("worlds"), keyBase + QStringLiteral(":wp.flags"), 0);
	(void)dbWriteInt(QStringLiteral("worlds"), keyBase + QStringLiteral(":wp.ptMinPosition.x"), 0);
	(void)dbWriteInt(QStringLiteral("worlds"), keyBase + QStringLiteral(":wp.ptMinPosition.y"), 0);
	(void)dbWriteInt(QStringLiteral("worlds"), keyBase + QStringLiteral(":wp.ptMaxPosition.x"), 0);
	(void)dbWriteInt(QStringLiteral("worlds"), keyBase + QStringLiteral(":wp.ptMaxPosition.y"), 0);
	(void)dbWriteInt(QStringLiteral("worlds"), keyBase + QStringLiteral(":wp.left"), rc.left());
	(void)dbWriteInt(QStringLiteral("worlds"), keyBase + QStringLiteral(":wp.right"), rc.right());
	(void)dbWriteInt(QStringLiteral("worlds"), keyBase + QStringLiteral(":wp.top"), rc.top());
	(void)dbWriteInt(QStringLiteral("worlds"), keyBase + QStringLiteral(":wp.bottom"), rc.bottom());
}

void AppController::ensureActivityDocument()
{
	if (!m_activityDoc)
		m_activityDoc = new ActivityDocument(this, this);
}

void AppController::changeToFileBrowsingDirectory() const
{
	if (!m_fileBrowsingDir.isEmpty())
		QDir::setCurrent(m_fileBrowsingDir);
}

void AppController::changeToStartupDirectory() const
{
	if (!m_workingDir.isEmpty())
		QDir::setCurrent(m_workingDir);
}

QString AppController::defaultWorldDirectory() const
{
	return getGlobalOption(QStringLiteral("DefaultWorldFileDirectory")).toString();
}

QVariant AppController::getGlobalOption(const QString &name) const
{
	if (name.trimmed().isEmpty())
		return {};
	const QString lookupName =
	    name.trimmed().compare(QStringLiteral("ConfirmBeforeClosingMushclient"), Qt::CaseInsensitive) == 0
	        ? QStringLiteral("ConfirmBeforeClosingQmud")
	        : name.trimmed();

	const auto findKey = [&](const QString &target, const auto &table) -> int
	{
		for (int i = 0; table[i].name; ++i)
		{
			if (const auto key = QString::fromUtf8(table[i].name);
			    key.compare(target.trimmed(), Qt::CaseInsensitive) == 0)
				return i;
		}
		return -1;
	};

	if (const auto intIndex = findKey(lookupName, kGlobalOptionsTable); intIndex >= 0)
	{
		if (const auto key = QString::fromUtf8(kGlobalOptionsTable[intIndex].name);
		    m_globalIntPrefs.contains(key))
			return m_globalIntPrefs.value(key);
		return kGlobalOptionsTable[intIndex].defaultValue;
	}

	if (const auto stringIndex = findKey(lookupName, kAlphaGlobalOptionsTable); stringIndex >= 0)
	{
		if (const auto key = QString::fromUtf8(kAlphaGlobalOptionsTable[stringIndex].name);
		    m_globalStringPrefs.contains(key))
			return m_globalStringPrefs.value(key);
		return QString::fromUtf8(kAlphaGlobalOptionsTable[stringIndex].defaultValue);
	}

	return {};
}

QStringList AppController::globalOptionList()
{
	QStringList result;
	for (int i = 0; kGlobalOptionsTable[i].name; ++i)
		result.append(QString::fromUtf8(kGlobalOptionsTable[i].name));
	for (int i = 0; kAlphaGlobalOptionsTable[i].name; ++i)
		result.append(QString::fromUtf8(kAlphaGlobalOptionsTable[i].name));
	return result;
}

void AppController::setGlobalOptionString(const QString &name, const QString &value)
{
	if (name.trimmed().isEmpty())
		return;
	const QString lookupName = name.trimmed();

	const auto    findKey = [&](const QString &target) -> QString
	{
		for (int i = 0; kAlphaGlobalOptionsTable[i].name; ++i)
		{
			if (const auto key = QString::fromUtf8(kAlphaGlobalOptionsTable[i].name);
			    key.compare(target.trimmed(), Qt::CaseInsensitive) == 0)
				return key;
		}
		return target;
	};

	const auto key          = findKey(lookupName);
	const auto storedValue  = normalizeStoredGlobalStringValue(key, value);
	const auto runtimeValue = normalizeRuntimeGlobalStringValue(key, storedValue);
	m_globalStringPrefs.insert(key, runtimeValue);
	(void)dbWriteString(QStringLiteral("prefs"), key, storedValue);
}

void AppController::setGlobalOptionInt(const QString &name, const int value)
{
	if (name.trimmed().isEmpty())
		return;
	const QString lookupName =
	    name.trimmed().compare(QStringLiteral("ConfirmBeforeClosingMushclient"), Qt::CaseInsensitive) == 0
	        ? QStringLiteral("ConfirmBeforeClosingQmud")
	        : name.trimmed();

	const auto findKey = [&](const QString &target) -> QString
	{
		for (int i = 0; kGlobalOptionsTable[i].name; ++i)
		{
			if (const auto key = QString::fromUtf8(kGlobalOptionsTable[i].name);
			    key.compare(target.trimmed(), Qt::CaseInsensitive) == 0)
				return key;
		}
		return target;
	};

	const auto key = findKey(lookupName);
	m_globalIntPrefs.insert(key, value);
	(void)dbWriteInt(QStringLiteral("prefs"), key, value);
}

QString AppController::makeAbsolutePath(const QString &fileName) const
{
	// Convert relative file names against the configured working directory.
	if (fileName.isEmpty())
		return fileName;

	QString    normalized = normalizePathString(fileName);
	const bool hadTrailingSeparator =
	    normalized.endsWith(QLatin1Char('/')) || normalized.endsWith(QLatin1Char('\\'));
	const QChar first   = normalized.at(0);
	const bool  isDrive = normalized.size() > 1 && normalized.at(1) == QChar(':') && first.isLetter();
	if (const bool isAbsolute = isDrive || first == QChar('\\') || first == QChar('/'); !isAbsolute)
	{
		QString relative = normalized;
		if (relative.startsWith(QStringLiteral("./")) || relative.startsWith(QStringLiteral(".\\")))
			relative = relative.mid(2);
		QString resolved = QDir(m_workingDir).filePath(relative);
#ifndef Q_OS_WIN
		resolved = QDir::cleanPath(resolved);
#endif
		if (hadTrailingSeparator && !resolved.endsWith(QLatin1Char('/')))
			resolved += QLatin1Char('/');
#ifdef Q_OS_WIN
		return QDir::toNativeSeparators(resolved);
#else
		return resolved;
#endif
	}

#ifdef Q_OS_WIN
	if (hadTrailingSeparator && !normalized.endsWith(QLatin1Char('/')))
		normalized += QLatin1Char('/');
	return QDir::toNativeSeparators(normalized);
#else
	normalized = QDir::cleanPath(normalized);
	if (hadTrailingSeparator && !normalized.endsWith(QLatin1Char('/')))
		normalized += QLatin1Char('/');
	return normalized;
#endif
}

QStringList AppController::activeOpenWorldLogFiles() const
{
	QStringList openLogs;
	for (WorldRuntime *runtime : activeWorldRuntimes())
	{
		if (!runtime || !runtime->isLogOpen())
			continue;
		const QString logFile = runtime->logFileName().trimmed();
		if (logFile.isEmpty())
			continue;
		openLogs.push_back(makeAbsolutePath(logFile));
	}
	return openLogs;
}

bool AppController::initialize()
{
	// Initialize startup directories, persistent preferences, and UI startup state.

	// working directory at login time / data directory resolution:
	// - QMUD_HOME environment variable overrides on all platforms.
	// - If QMUD_HOME is not set, read QMUD_HOME from config fallback:
	//   Linux: ~/.config/QMud/config, then /etc/QMud/config
	//   macOS: ~/Library/Application Support/QMud/config, then /Library/Application Support/QMud/config
	//   Windows: %LOCALAPPDATA%/QMud/config
	// - In multi-instance mode, config fallback is disabled and QMUD_HOME must be set explicitly in process env.
	// - AppImage defaults to $HOME/QMud when QMUD_HOME is not set.
	// - macOS defaults to ~/Library/Application Support/QMud when QMUD_HOME is not set.
	// - Windows/default keep executable directory when QMUD_HOME is not set.
	const auto isAppImage                  = !qEnvironmentVariable("APPIMAGE").trimmed().isEmpty();
	const bool hasQmudHomeFromEnv          = !qEnvironmentVariable("QMUD_HOME").trimmed().isEmpty();
	auto       qmudHome                    = qmudEnvironmentVariable(QStringLiteral("QMUD_HOME")).trimmed();
	bool       hasQmudHomeFromSystemConfig = false;
	if (!hasQmudHomeFromEnv && !qmudHome.isEmpty())
		hasQmudHomeFromSystemConfig = true;

	const auto defaultStartupDir = [isAppImage]() -> QString
	{
		if (isAppImage)
		{
			QString homeDir = qEnvironmentVariable("HOME").trimmed();
			if (homeDir.isEmpty())
				homeDir = QDir::homePath();
			if (homeDir.isEmpty())
				homeDir = QCoreApplication::applicationDirPath();
			return QDir(homeDir).filePath(QStringLiteral("QMud"));
		}
#ifdef Q_OS_MACOS
		QString homeDir = qEnvironmentVariable("HOME").trimmed();
		if (homeDir.isEmpty())
			homeDir = QDir::homePath();
		if (homeDir.isEmpty())
			homeDir = QCoreApplication::applicationDirPath();
		return QDir(homeDir).filePath(QStringLiteral("Library/Application Support/QMud"));
#else
		return QCoreApplication::applicationDirPath();
#endif
	};

	auto startupDir = !qmudHome.isEmpty() ? qmudHome : defaultStartupDir();

	startupDir = QDir::cleanPath(startupDir);
	if (!QDir().mkpath(startupDir))
	{
		if (!hasQmudHomeFromEnv && hasQmudHomeFromSystemConfig)
		{
			if (const auto fallbackStartupDir = QDir::cleanPath(defaultStartupDir());
			    QDir::cleanPath(fallbackStartupDir) != QDir::cleanPath(startupDir) &&
			    QDir().mkpath(fallbackStartupDir))
			{
				qWarning() << "Failed to use QMUD_HOME from config fallback, falling back to defaults:"
				           << startupDir;
				startupDir = fallbackStartupDir;
			}
			else
			{
				QMessageBox::critical(m_mainWindow, QStringLiteral("QMud"),
				                      QStringLiteral("Unable to create data directory: %1").arg(startupDir));
				return false;
			}
		}
		else
		{
			QMessageBox::critical(m_mainWindow, QStringLiteral("QMud"),
			                      QStringLiteral("Unable to create data directory: %1").arg(startupDir));
			return false;
		}
	}

	syncAppImageSkeleton(startupDir);
	syncMacBundlePayload(startupDir);

	m_workingDir = startupDir;
	QDir::setCurrent(m_workingDir);

	// make sure directory name ends in a slash
	if (!m_workingDir.endsWith('/'))
		m_workingDir += '/';

	detectReloadStartupArguments();
	if (!m_reloadLaunchRequested)
		cleanupReloadStateOnNormalStartup();
	else
	{
		QString resolvedStatePath = m_reloadStatePathArg.trimmed();
		if (resolvedStatePath.isEmpty())
			resolvedStatePath = reloadStateDefaultPath(m_workingDir);
		if (const QFileInfo stateInfo(resolvedStatePath); stateInfo.isRelative())
			resolvedStatePath = QDir(m_workingDir).filePath(resolvedStatePath);
		if (isReloadStateFileStale(resolvedStatePath, kReloadStateStaleAgeSeconds))
		{
			qWarning() << kReloadLogTag << "Reload state file is stale; ignoring reload startup request.";
			QString cleanupError;
			if (!removeReloadStateFile(resolvedStatePath, &cleanupError) && !cleanupError.isEmpty())
				qWarning() << kReloadLogTag << "Unable to remove stale reload state file:" << cleanupError;
			m_reloadLaunchRequested = false;
			m_reloadStatePathArg.clear();
			m_reloadTokenArg.clear();
		}
	}

	migrateLegacyIniToQmudConf(m_workingDir);

	// where we do file browsing from
	m_fileBrowsingDir = m_workingDir;

	m_whenClientStarted = QDateTime::currentDateTime();

	m_version = QString::fromLatin1(kVersionString);

#ifdef CI_BUILD
	m_version += "-ci";
#endif

	m_fixedPitchFont   = QStringLiteral("DejaVu Sans Mono");
	m_pluginsDirectory = QStringLiteral(".\\\\worlds\\\\plugins\\\\");

	// open SQLite database for preferences
	if (!openPreferencesDatabase())
	{
		return false;
	}

	// i18n setup  -------------------------------
	if (!setupI18N())
	{
		return false; // no resources, or Lua won't start up
	}

	loadMapDirections();
	generate256Colours();

	// read global prefs from the database
	loadGlobalsFromDatabase();

	// Keep process locale for UI/text discovery, but force stable persistence
	// formatting for file/database serialization.
	setlocale(LC_ALL, "");
	setlocale(LC_NUMERIC, "C");
	setlocale(LC_TIME, "C");

	// seed the random number generator
	const time_t timer = time(nullptr);
	srand(static_cast<unsigned int>(timer));

	// Seed Qt RNG service used by world/runtime Lua APIs and rendering paths.
	seedRandom(static_cast<quint32>(timer));

	// Capture startup UI actions and run them later (after splash is dismissed)
	// to avoid hidden-modal deadlocks during startup.
	const auto firsttime         = dbGetInt(QStringLiteral("control"), QStringLiteral("First time"), 1) != 0;
	const auto version           = dbGetInt(QStringLiteral("control"), QStringLiteral("Version"), 0);
	m_startupFirstTime           = firsttime;
	m_startupNeedsUpgradeWelcome = !firsttime && version < kThisVersion;

	backupDataOnUpgradeIfNeeded(version, firsttime);

	if (firsttime)
		(void)dbWriteInt(QStringLiteral("control"), QStringLiteral("First time"), 0);

	if (version != kThisVersion)
		(void)dbWriteInt(QStringLiteral("control"), QStringLiteral("Version"), kThisVersion);

	loadToolbarLayout();
	restoreWindowPlacement();
	setupRecentFiles();
	loadPrintSetupPreferences();
	applyGlobalPreferences();

	return true;
}

bool AppController::registerFileAssociations(QString *errorMessage)
{
	QString  localErrorMessage;
	QString *targetError = errorMessage ? errorMessage : &localErrorMessage;
	bool     ok          = false;
#ifdef Q_OS_WIN
	ok = registerWindowsFileAssociations(targetError);
#elif defined(Q_OS_MACOS)
	ok = registerMacFileAssociations(targetError);
#elif defined(Q_OS_LINUX)
	ok = registerLinuxFileAssociations(*targetError);
#else
	ok = true;
#endif

	if (!ok)
		qWarning() << "File association registration failed:" << *targetError;
	return ok;
}

void AppController::syncAppImageSkeleton(const QString &startupDir)
{
	if (const auto appImagePath = qEnvironmentVariable("APPIMAGE"); appImagePath.isEmpty())
		return;

	const auto appRoot     = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../.."));
	const auto skeletonSrc = QDir(appRoot).filePath(QStringLiteral("skeleton"));
	const QDir srcDir(skeletonSrc);
	if (!srcDir.exists())
	{
		return;
	}

	const QDir dstRoot(startupDir);
	if (!dstRoot.exists() && !QDir().mkpath(startupDir))
		return;
	const auto targetHasLegacyPrefsDb =
	    directoryContainsFileCaseInsensitive(startupDir, QStringLiteral("mushclient_prefs.sqlite"));
	const auto targetHasLegacyIni =
	    directoryContainsFileCaseInsensitive(startupDir, QStringLiteral("MUSHCLIENT.INI"));

	int          createdDirs = 0;
	int          copiedFiles = 0;

	QDirIterator dirIt(skeletonSrc, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
	while (dirIt.hasNext())
	{
		const auto srcPath = dirIt.next();
		const auto rel     = srcDir.relativeFilePath(srcPath);
		if (const auto dstPath = dstRoot.filePath(rel); QDir().mkpath(dstPath))
			++createdDirs;
	}

	QDirIterator fileIt(skeletonSrc, QDir::Files, QDirIterator::Subdirectories);
	while (fileIt.hasNext())
	{
		const auto srcPath = fileIt.next();
		const auto rel     = srcDir.relativeFilePath(srcPath);
		if (rel.endsWith(QStringLiteral(".dll"), Qt::CaseInsensitive))
			continue;

		if (const auto dstPath = dstRoot.filePath(rel);
		    copySyncedFileWithPolicy(srcPath, rel, dstPath, targetHasLegacyPrefsDb, targetHasLegacyIni))
			++copiedFiles;
	}
	Q_UNUSED(createdDirs);
	Q_UNUSED(copiedFiles);
}

void AppController::syncMacBundlePayload(const QString &startupDir)
{
#ifndef Q_OS_MACOS
	Q_UNUSED(startupDir);
#else
	const auto sourceRoot = QCoreApplication::applicationDirPath();
	const QDir srcDir(sourceRoot);
	if (!srcDir.exists())
		return;

	const QDir dstRoot(startupDir);
	if (!dstRoot.exists() && !QDir().mkpath(startupDir))
		return;
	const auto targetHasLegacyPrefsDb =
	    directoryContainsFileCaseInsensitive(startupDir, QStringLiteral("mushclient_prefs.sqlite"));
	const auto targetHasLegacyIni =
	    directoryContainsFileCaseInsensitive(startupDir, QStringLiteral("MUSHCLIENT.INI"));

	const auto   binaryName = QFileInfo(QCoreApplication::applicationFilePath()).fileName();

	QDirIterator dirIt(sourceRoot, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
	while (dirIt.hasNext())
	{
		const auto srcPath = dirIt.next();
		const auto rel     = srcDir.relativeFilePath(srcPath);
		const auto dstPath = dstRoot.filePath(rel);
		QDir().mkpath(dstPath);
	}

	QDirIterator fileIt(sourceRoot, QDir::Files, QDirIterator::Subdirectories);
	while (fileIt.hasNext())
	{
		const auto srcPath = fileIt.next();
		const auto rel     = srcDir.relativeFilePath(srcPath);
		if (rel == binaryName)
			continue;

		const QString dstPath = dstRoot.filePath(rel);
		copySyncedFileWithPolicy(srcPath, rel, dstPath, targetHasLegacyPrefsDb, targetHasLegacyIni);
	}
#endif
}

bool AppController::setupI18N()
{
	// file names might be: (MUSHclient executable)\locale\EN.dll  - resources
	//                      (MUSHclient executable)\locale\EN.lua  - localization strings

	const QScreen *const screen       = QGuiApplication::primaryScreen();
	const QSize          screenSize   = screen ? screen->geometry().size() : QSize(1024, 768);
	const bool           bSmallScreen = screenSize.width() < 1024 || screenSize.height() < 768;

	// find 2-character country ID - default
	const QString        localeBuf = QLocale::system().name().left(2);

	// see if different in prefs
	QString              prefLocale;
	dbSimpleQuery(QStringLiteral("SELECT value FROM prefs WHERE name = 'Locale'"), prefLocale, false,
	              localeBuf);
	m_locale = prefLocale.isEmpty() ? localeBuf : prefLocale;

	// executable directory
	m_translatorFile = QCoreApplication::applicationDirPath();

	// locale subdirectory
	if (!m_translatorFile.endsWith('/'))
		m_translatorFile += '/';
	m_translatorFile += "locale/";

	// english resource file
	const QString englishResourceFile = m_translatorFile + (bSmallScreen ? "EN_small.qm" : "EN.qm");
	const QString englishResourceResource =
	    QStringLiteral(":/locale/") + (bSmallScreen ? "EN_small.qm" : "EN.qm");

	// locale-specific file
	QString localeResourceFile = m_translatorFile + m_locale;
	if (bSmallScreen)
		localeResourceFile += "_small";
	localeResourceFile += ".qm";
	QString localeResourceResource = QStringLiteral(":/locale/") + m_locale;
	if (bSmallScreen)
		localeResourceResource += "_small";
	localeResourceResource += ".qm";

	// translator file is Lua
	m_translatorFile += m_locale; // eg. EN
	m_translatorFile += ".lua";

	auto installQtTranslator = [this](const QString &resourcePath, const QString &filePath) -> bool
	{
		if (!m_qtTranslator)
			m_qtTranslator = new QTranslator(this);
		if (QFile::exists(resourcePath) && m_qtTranslator->load(resourcePath))
		{
			QCoreApplication::installTranslator(m_qtTranslator);
			return true;
		}
		if (m_qtTranslator->load(filePath))
		{
			QCoreApplication::installTranslator(m_qtTranslator);
			return true;
		}
		return false;
	};

	// try non-English one, if not EN locale
	if (englishResourceFile.compare(localeResourceFile, Qt::CaseInsensitive) != 0)
	{
		if (!installQtTranslator(localeResourceResource, localeResourceFile))
		{
			QString strMessage = "Failed to load resources file: ";
			strMessage += localeResourceFile;
			strMessage += " - trying English file";

			QMessageBox::information(m_mainWindow, QStringLiteral("QMud"), strMessage);
		}
	}

	// not found? try English
	if (!m_qtTranslator || m_qtTranslator->isEmpty())
	{
		if (!installQtTranslator(englishResourceResource, englishResourceFile))
		{
			if (m_locale.compare(QStringLiteral("EN"), Qt::CaseInsensitive) != 0)
			{
				QString strMessage = "Failed to load resources file: ";
				strMessage += englishResourceFile;

				QMessageBox::critical(m_mainWindow, QStringLiteral("QMud"), strMessage);
				return false; // failed to load the localized resources
			}
		}
	}

#ifdef QMUD_ENABLE_LUA_I18N
	if (m_translatorLua)
	{
		lua_close(m_translatorLua);
		m_translatorLua = nullptr;
	}
	// see if translator file present - if so, load it
	QFileInfo status(m_translatorFile);
	if (status.exists())
	{
		LuaStateOwner state(QMudLuaSupport::makeLuaState()); /* opens Lua */
		if (!state)
		{
			QMessageBox::critical(m_mainWindow, QStringLiteral("QMud"),
			                      QStringLiteral("Lua (i18n) initialization failed"));
			return false; // can't open Lua
		}

		luaL_openlibs(state.get()); // open all standard Lua libraries
		QMudLuaSupport::applyLua51Compat(state.get());
		qmudLogLua51CompatState(state.get(), "AppController translator");
		QMudLuaSupport::callLuaCFunction(state.get(), luaopen_lsqlite3); // open sqlite library

		const bool enablePackage = getGlobalOption(QStringLiteral("AllowLoadingDlls")).toInt() != 0;
		QMudLuaSupport::applyLuaSecurityRestrictions(state.get(), enablePackage);

		lua_settop(state.get(), 0); // clear stack

		static constexpr char luaSandbox[] =
		    // only allow safe os functions
		    " os = { "
		    "   date = os.date, "
		    "   time = os.time, "
		    "   setlocale = os.setlocale, "
		    "   clock = os.clock, "
		    "   difftime = os.difftime, "
		    "   exit = os.exit,  " // not really implemented but we have nice error message
		    "  }  "
		    // no io calls
		    " io = nil "
		    "";

		// sandbox it
		const bool loaded = !(luaL_loadbuffer(state.get(), luaSandbox, sizeof(luaSandbox) - 1, "sandbox") ||
		                      lua_pcall(state.get(), 0, 0, 0) ||
		                      luaL_loadfile(state.get(), m_translatorFile.toUtf8().constData()) ||
		                      lua_pcall(state.get(), 0, 0, 0));
		if (!loaded)
		{
			QMudLuaSupport::luaError(state.get(), "Localization initialization");
		}
		if (loaded)
			m_translatorLua = state.release();
	} // end of localization file exists
#else
	// Lua localization support is disabled in this build.
#endif

	return true;
} // end of AppController::setupI18N

lua_State *AppController::translatorLuaState() const
{
	return m_translatorLua;
}

void AppController::loadMapDirections()
{
	m_mapDirections.clear();

	auto put = [&](const QString &key, const QString &toLog, const QString &toSend, const QString &reverse)
	{ m_mapDirections.insert(normalizeDirectionKey(key), MapDirection{toLog, toSend, reverse}); };

	//              direction  log  full-send reverse
	put(QStringLiteral("north"), QStringLiteral("n"), QStringLiteral("north"), QStringLiteral("s"));
	put(QStringLiteral("south"), QStringLiteral("s"), QStringLiteral("south"), QStringLiteral("n"));
	put(QStringLiteral("east"), QStringLiteral("e"), QStringLiteral("east"), QStringLiteral("w"));
	put(QStringLiteral("west"), QStringLiteral("w"), QStringLiteral("west"), QStringLiteral("e"));
	put(QStringLiteral("up"), QStringLiteral("u"), QStringLiteral("up"), QStringLiteral("d"));
	put(QStringLiteral("down"), QStringLiteral("d"), QStringLiteral("down"), QStringLiteral("u"));
	put(QStringLiteral("ne"), QStringLiteral("ne"), QStringLiteral("ne"), QStringLiteral("sw"));
	put(QStringLiteral("nw"), QStringLiteral("nw"), QStringLiteral("nw"), QStringLiteral("se"));
	put(QStringLiteral("se"), QStringLiteral("se"), QStringLiteral("se"), QStringLiteral("nw"));
	put(QStringLiteral("sw"), QStringLiteral("sw"), QStringLiteral("sw"), QStringLiteral("ne"));
	put(QStringLiteral("f"), QStringLiteral("f"), QStringLiteral("f"), QStringLiteral("f"));

	// abbreviations
	m_mapDirections.insert(QStringLiteral("n"), m_mapDirections.value(QStringLiteral("north")));
	m_mapDirections.insert(QStringLiteral("s"), m_mapDirections.value(QStringLiteral("south")));
	m_mapDirections.insert(QStringLiteral("e"), m_mapDirections.value(QStringLiteral("east")));
	m_mapDirections.insert(QStringLiteral("w"), m_mapDirections.value(QStringLiteral("west")));
	m_mapDirections.insert(QStringLiteral("u"), m_mapDirections.value(QStringLiteral("up")));
	m_mapDirections.insert(QStringLiteral("d"), m_mapDirections.value(QStringLiteral("down")));
}

QString AppController::mapDirectionToLog(const QString &direction) const
{
	const MapDirection *mapping = findMapDirection(direction);
	return mapping ? mapping->toLog : QString{};
}

QString AppController::mapDirectionToSend(const QString &direction) const
{
	const MapDirection *mapping = findMapDirection(direction);
	return mapping ? mapping->toSend : QString{};
}

QString AppController::mapDirectionReverse(const QString &direction) const
{
	const MapDirection *mapping = findMapDirection(direction);
	return mapping ? mapping->reverse : QString{};
}

const AppController::MapDirection *AppController::findMapDirection(const QString &direction) const
{
	if (m_mapDirections.isEmpty())
		const_cast<AppController *>(this)->loadMapDirections();
	if (const auto it = m_mapDirections.constFind(normalizeDirectionKey(direction));
	    it != m_mapDirections.cend())
		return &it.value();
	return nullptr;
}

QMudColorRef AppController::xtermColorAt(const int index) const
{
	if (index < 0 || index >= m_xterm256Colours.size())
		return qmudRgb(0, 0, 0);
	return m_xterm256Colours[index];
}

void AppController::setXtermColourCube(const int which)
{
	if (which == 1)
	{
		for (int red = 0; red < 6; ++red)
		{
			for (int green = 0; green < 6; ++green)
			{
				for (int blue = 0; blue < 6; ++blue)
				{
					const int idx = 16 + red * 36 + green * 6 + blue;
					m_xterm256Colours[idx] =
					    qmudRgb(kXtermCubeValues[red], kXtermCubeValues[green], kXtermCubeValues[blue]);
				}
			}
		}
	}
	else if (which == 2)
	{
		constexpr quint8 increment = 255 / 5;
		for (int red = 0; red < 6; ++red)
		{
			for (int green = 0; green < 6; ++green)
			{
				for (int blue = 0; blue < 6; ++blue)
				{
					const int idx          = 16 + red * 36 + green * 6 + blue;
					m_xterm256Colours[idx] = qmudRgb(red * increment, green * increment, blue * increment);
				}
			}
		}
	}
}

void AppController::generate256Colours()
{
	m_xterm256Colours[0] = qmudRgb(0, 0, 0);
	m_xterm256Colours[1] = qmudRgb(128, 0, 0);
	m_xterm256Colours[2] = qmudRgb(0, 128, 0);
	m_xterm256Colours[3] = qmudRgb(128, 128, 0);
	m_xterm256Colours[4] = qmudRgb(0, 0, 128);
	m_xterm256Colours[5] = qmudRgb(128, 0, 128);
	m_xterm256Colours[6] = qmudRgb(0, 128, 128);
	m_xterm256Colours[7] = qmudRgb(192, 192, 192);

	m_xterm256Colours[8]  = qmudRgb(128, 128, 128);
	m_xterm256Colours[9]  = qmudRgb(255, 0, 0);
	m_xterm256Colours[10] = qmudRgb(0, 255, 0);
	m_xterm256Colours[11] = qmudRgb(255, 255, 0);
	m_xterm256Colours[12] = qmudRgb(0, 0, 255);
	m_xterm256Colours[13] = qmudRgb(255, 0, 255);
	m_xterm256Colours[14] = qmudRgb(0, 255, 255);
	m_xterm256Colours[15] = qmudRgb(255, 255, 255);

	for (int red = 0; red < 6; ++red)
		for (int green = 0; green < 6; ++green)
			for (int blue = 0; blue < 6; ++blue)
			{
				const int idx = 16 + red * 36 + green * 6 + blue;
				m_xterm256Colours[idx] =
				    qmudRgb(kXtermCubeValues[red], kXtermCubeValues[green], kXtermCubeValues[blue]);
			}

	for (int grey = 0; grey < 24; ++grey)
	{
		const auto value              = static_cast<quint8>(8 + grey * 10);
		m_xterm256Colours[232 + grey] = qmudRgb(value, value, value);
	}
}

bool AppController::openWorldForReloadRecovery(const ReloadWorldState &worldState, const bool activateWindow,
                                               WorldRuntime **runtime, WorldView **view)
{
	if (runtime)
		*runtime = nullptr;
	if (view)
		*view = nullptr;
	if (!m_mainWindow || !runtime)
		return false;

	const QVector<WorldRuntime *> existingRuntimes           = activeWorldRuntimes();
	bool                          opened                     = false;
	const int                     previousActivationOverride = m_nextNewWorldActivationOverride;
	m_nextNewWorldActivationOverride                         = activateWindow ? 1 : 0;
	if (!worldState.worldFilePath.trimmed().isEmpty() && QFileInfo::exists(worldState.worldFilePath))
		opened = openDocumentFile(worldState.worldFilePath);
	if (!opened)
		opened = openDocumentFile(QString());
	m_nextNewWorldActivationOverride = previousActivationOverride;
	if (!opened)
		return false;

	WorldRuntime *worldRuntime = nullptr;
	for (WorldRuntime *candidate : activeWorldRuntimes())
	{
		if (!existingRuntimes.contains(candidate))
		{
			worldRuntime = candidate;
			break;
		}
	}
	if (!worldRuntime && activateWindow)
	{
		if (WorldChildWindow *activeChild = m_mainWindow->activeWorldChildWindow(); activeChild)
			worldRuntime = activeChild->runtime();
	}
	if (!worldRuntime)
		return false;

	WorldChildWindow *child = m_mainWindow->findWorldChildWindow(worldRuntime);
	if (!child)
		return false;
	WorldView *worldView = child->view();

	if (!worldState.worldId.trimmed().isEmpty())
		worldRuntime->setWorldAttribute(QStringLiteral("id"), worldState.worldId.trimmed());
	if (!worldState.displayName.trimmed().isEmpty())
	{
		worldRuntime->setWorldAttribute(QStringLiteral("name"), worldState.displayName.trimmed());
		child->setWindowTitle(worldState.displayName.trimmed());
		if (worldView)
			worldView->setWorldName(worldState.displayName.trimmed());
	}
	if (!worldState.host.trimmed().isEmpty())
		worldRuntime->setWorldAttribute(QStringLiteral("site"), worldState.host.trimmed());
	if (worldState.port > 0)
		worldRuntime->setWorldAttribute(QStringLiteral("port"), QString::number(worldState.port));
	if (!worldState.worldFilePath.trimmed().isEmpty() && worldRuntime->worldFilePath().trimmed().isEmpty())
		worldRuntime->setWorldFilePath(worldState.worldFilePath);
	worldRuntime->setWorldAttribute(QStringLiteral("utf_8"),
	                                worldState.utf8Enabled ? QStringLiteral("1") : QStringLiteral("0"));

	*runtime = worldRuntime;
	if (view)
		*view = worldView;
	return true;
}

void AppController::reconnectRecoveredWorld(WorldRuntime *runtime, const ReloadWorldState &worldState,
                                            const bool closeSocketFirst)
{
	if (!runtime)
		return;
	if (const QString warning = reconnectRecoveredRuntime(*runtime, worldState, closeSocketFirst);
	    !warning.isEmpty())
	{
		qWarning() << warning;
	}
}

bool AppController::recoverReloadStartupState()
{
	if (!m_reloadLaunchRequested)
		return false;
	++m_reloadRecoveryRuns;

	QString statePath = m_reloadStatePathArg.trimmed();
	if (statePath.isEmpty())
		statePath = reloadStateDefaultPath(m_workingDir);
	if (const QFileInfo stateInfo(statePath); stateInfo.isRelative())
		statePath = QDir(m_workingDir).filePath(statePath);

	ReloadStateSnapshot                snapshot;
	const ReloadStartupValidationInput validationInput{
	    m_reloadTokenArg.trimmed(),
	    QCoreApplication::applicationFilePath(),
	};
	QString error;
	QString cleanupWarning;
	if (!loadValidatedAndConsumeReloadStateSnapshot(statePath, validationInput, &snapshot, &error,
	                                                &cleanupWarning))
	{
		qWarning() << kReloadLogTag << "Recovery skipped:" << error;
		if (!cleanupWarning.isEmpty())
			qWarning() << kReloadLogTag << "Reload state cleanup warning:" << cleanupWarning;
		return false;
	}
	if (!cleanupWarning.isEmpty())
	{
		qWarning() << kReloadLogTag
		           << "Unable to consume reload state file before recovery:" << cleanupWarning;
	}

	QList<ReloadWorldState> worlds = snapshot.worlds;
	std::ranges::sort(worlds,
	                  [](const ReloadWorldState &lhs, const ReloadWorldState &rhs)
	                  {
		                  if (lhs.sequence == rhs.sequence)
			                  return lhs.displayName < rhs.displayName;
		                  return lhs.sequence < rhs.sequence;
	                  });

	struct OpenedRecoveryWorld
	{
			QPointer<WorldRuntime> runtime;
			QPointer<WorldView>    view;
			ReloadWorldState       state;
	};
	QList<OpenedRecoveryWorld> openedWorlds;
	QPointer<WorldRuntime>     requestedActiveRuntime;
	int                        openFailures      = 0;
	const bool                 verboseReloadLogs = envFlagEnabled("QMUD_RELOAD_VERBOSE");
	const bool                 previousSuppress  = m_suppressAutoConnect;
	m_suppressAutoConnect                        = true;

	for (const ReloadWorldState &worldState : worlds)
	{
		WorldRuntime *runtime = nullptr;
		WorldView    *view    = nullptr;
		const bool    activateWindow =
		    snapshot.activeWorldSequence > 0 && worldState.sequence == snapshot.activeWorldSequence;
		if (!openWorldForReloadRecovery(worldState, activateWindow, &runtime, &view) || !runtime || !view)
		{
			++openFailures;
			qWarning() << kReloadLogTag << "World recovery open failed for"
			           << (worldState.displayName.isEmpty() ? QStringLiteral("<unnamed>")
			                                                : worldState.displayName);
			continue;
		}
		if (!requestedActiveRuntime && snapshot.activeWorldSequence > 0 &&
		    worldState.sequence == snapshot.activeWorldSequence)
		{
			requestedActiveRuntime = runtime;
			if (m_mainWindow)
				m_mainWindow->activateWorldRuntime(runtime);
		}
		else if (requestedActiveRuntime && m_mainWindow)
		{
			m_mainWindow->activateWorldRuntime(requestedActiveRuntime.data());
		}
		openedWorlds.push_back({runtime, view, worldState});
	}

	m_suppressAutoConnect = previousSuppress;

	struct ReloadRecoveryAsyncContext
	{
			qint64                 pending{0};
			qint64                 openedCount{0};
			int                    openFailures{0};
			int                    reattachedCount{0};
			int                    reconnectCount{0};
			int                    adoptFailures{0};
			bool                   verboseReloadLogs{false};
			QPointer<WorldRuntime> requestedActiveRuntime;
	};
	auto       asyncContext              = std::make_shared<ReloadRecoveryAsyncContext>();
	const auto openedWorldCount          = static_cast<qint64>(openedWorlds.size());
	asyncContext->pending                = openedWorldCount;
	asyncContext->openedCount            = openedWorldCount;
	asyncContext->openFailures           = openFailures;
	asyncContext->verboseReloadLogs      = verboseReloadLogs;
	asyncContext->requestedActiveRuntime = requestedActiveRuntime;

	const auto finalizeRecovery = [this, asyncContext]
	{
		if (m_mainWindow)
		{
			bool activated = false;
			if (asyncContext->requestedActiveRuntime)
				activated = m_mainWindow->activateWorldRuntime(asyncContext->requestedActiveRuntime.data());
			if (!activated)
				m_mainWindow->activateWorldSlot(1);
		}

		qInfo() << kReloadLogTag << "Recovery summary:"
		        << "opened=" << asyncContext->openedCount << "reattached=" << asyncContext->reattachedCount
		        << "reconnect_queued=" << asyncContext->reconnectCount
		        << "open_failures=" << asyncContext->openFailures
		        << "adopt_failures=" << asyncContext->adoptFailures;
		m_reloadRecoveryReattached += asyncContext->reattachedCount;
		m_reloadRecoveryReconnectQueued += asyncContext->reconnectCount;
		qInfo() << kReloadLogTag << "Telemetry:"
		        << "attempts=" << m_reloadAttempts << "exec_failures=" << m_reloadExecFailures
		        << "recoveries=" << m_reloadRecoveryRuns << "reattached_total=" << m_reloadRecoveryReattached
		        << "reconnect_queued_total=" << m_reloadRecoveryReconnectQueued;
		if (m_mainWindow)
		{
			m_mainWindow->showStatusMessage(
			    QStringLiteral("Reload recovery: %1 reattached, %2 reconnect queued.")
			        .arg(asyncContext->reattachedCount)
			        .arg(asyncContext->reconnectCount),
			    5000);
		}
	};

	if (asyncContext->pending <= 0)
	{
		finalizeRecovery();
		return true;
	}

	const auto handleRecoveredWorld = [this, asyncContext](WorldRuntime           *runtime,
	                                                       const ReloadWorldState &worldState, const bool ok,
	                                                       const QString &error)
	{
		if (!runtime)
			return;

		if (!ok && !error.isEmpty())
		{
			qWarning() << "Failed to restore world session state during reload recovery for"
			           << reloadWorldIdentity(worldState) << ":" << error;
		}
		runWorldStartupPostRestore(runtime);

		if (worldState.connectedAtReload && worldState.socketDescriptor >= 0)
		{
			const ReloadRecoverySocketDecision decision = applyReloadSocketRecovery(*runtime, worldState);
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
			QString cloexecError;
			if (!setSocketDescriptorInheritable(worldState.socketDescriptor, false, &cloexecError) &&
			    !cloexecError.isEmpty())
			{
				printReloadInfoToStdout(
				    QStringLiteral("Unable to restore close-on-exec for descriptor %1 after recovery: %2")
				        .arg(worldState.socketDescriptor)
				        .arg(cloexecError));
			}
#endif
			if (decision.outcome == ReloadRecoverySocketOutcome::ReconnectQueued)
			{
				if (!decision.error.isEmpty())
				{
					++asyncContext->adoptFailures;
					printReloadInfoToStdout(
					    QStringLiteral("Socket reattach failed for %1; reconnect queued. Descriptor=%2. "
					                   "Reason: %3")
					        .arg(reloadWorldIdentity(worldState))
					        .arg(worldState.socketDescriptor)
					        .arg(decision.error.trimmed().isEmpty() ? QStringLiteral("Unknown error.")
					                                                : decision.error.trimmed()));
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
					QString closeError;
					if (!closeSocketDescriptorIfOpen(worldState.socketDescriptor, &closeError) &&
					    !closeError.isEmpty())
					{
						printReloadInfoToStdout(
						    QStringLiteral("Unable to close orphaned descriptor %1 during reconnect "
						                   "fallback: %2")
						        .arg(worldState.socketDescriptor)
						        .arg(closeError));
					}
#endif
				}
				else
				{
					const QString reason =
					    !worldState.notes.trimmed().isEmpty()
					        ? worldState.notes.trimmed()
					        : QStringLiteral("Policy selected reconnect after descriptor adoption.");
					printReloadInfoToStdout(
					    QStringLiteral("Reconnect queued for %1. Descriptor=%2. Reason: %3")
					        .arg(reloadWorldIdentity(worldState))
					        .arg(worldState.socketDescriptor)
					        .arg(reason));
				}
				++asyncContext->reconnectCount;
				if (asyncContext->verboseReloadLogs)
				{
					qInfo() << kReloadLogTag << "Queued reconnect for"
					        << (worldState.displayName.isEmpty() ? QStringLiteral("<unnamed>")
					                                             : worldState.displayName);
				}
				reconnectRecoveredWorld(runtime, worldState, decision.closeSocketFirst);
			}
			else if (decision.outcome == ReloadRecoverySocketOutcome::Reattached)
			{
				if (worldState.mccpWasActive)
					runtime->requestMccpResumeAfterReloadReattach();
				++asyncContext->reattachedCount;
				if (asyncContext->verboseReloadLogs)
				{
					qInfo() << kReloadLogTag << "Reattached socket for"
					        << (worldState.displayName.isEmpty() ? QStringLiteral("<unnamed>")
					                                             : worldState.displayName);
				}
			}
		}
		else if (worldState.connectedAtReload)
		{
			++asyncContext->reconnectCount;
			const QString reason = !worldState.notes.trimmed().isEmpty()
			                           ? worldState.notes.trimmed()
			                           : QStringLiteral("Connected world had no reusable socket descriptor.");
			printReloadInfoToStdout(QStringLiteral("Reconnect queued for %1. Descriptor=%2. Reason: %3")
			                            .arg(reloadWorldIdentity(worldState))
			                            .arg(worldState.socketDescriptor)
			                            .arg(reason));
			if (asyncContext->verboseReloadLogs)
			{
				qInfo() << kReloadLogTag << "Queued reconnect (no descriptor) for"
				        << (worldState.displayName.isEmpty() ? QStringLiteral("<unnamed>")
				                                             : worldState.displayName);
			}
			reconnectRecoveredWorld(runtime, worldState, false);
		}
	};

	auto restoredOneWorld = [asyncContext, finalizeRecovery]()
	{
		--asyncContext->pending;
		if (asyncContext->pending <= 0)
			finalizeRecovery();
	};

	if (asyncContext->requestedActiveRuntime && !openedWorlds.isEmpty())
	{
		for (int i = 0; i < openedWorlds.size(); ++i)
		{
			if (openedWorlds.at(i).runtime == asyncContext->requestedActiveRuntime)
			{
				if (i > 0)
					openedWorlds.move(i, 0);
				break;
			}
		}
	}

	if (asyncContext->requestedActiveRuntime && !openedWorlds.isEmpty() &&
	    openedWorlds.front().runtime == asyncContext->requestedActiveRuntime)
	{
		const OpenedRecoveryWorld opened = openedWorlds.front();
		openedWorlds.pop_front();
		QString    restoreError;
		const bool restored = restoreWorldSessionStateSync(opened.runtime, opened.view, &restoreError);
		handleRecoveredWorld(opened.runtime, opened.state, restored, restoreError);
		restoredOneWorld();
	}

	if (asyncContext->pending <= 0)
		return true;

	for (const OpenedRecoveryWorld &opened : std::as_const(openedWorlds))
	{
		const QPointer<WorldRuntime> runtimeGuard = opened.runtime;
		const QPointer<WorldView>    viewGuard    = opened.view;
		const ReloadWorldState       worldState   = opened.state;

		restoreWorldSessionStateAsync(runtimeGuard, viewGuard,
		                              [runtimeGuard, worldState, handleRecoveredWorld,
		                               restoredOneWorld](const bool ok, const QString &error)
		                              {
			                              handleRecoveredWorld(runtimeGuard, worldState, ok, error);
			                              restoredOneWorld();
		                              });
	}

	return true;
}

void AppController::setupStartupBehavior()
{
	QString    reloadStateArg;
	QString    reloadTokenArg;
	const bool startedWithReloadArgs =
	    parseReloadStartupArguments(QCoreApplication::arguments(), &reloadStateArg, &reloadTokenArg);
	if (startedWithReloadArgs)
	{
		if (m_reloadLaunchRequested)
		{
			const bool recoveredFromReload = recoverReloadStartupState();
			if (!recoveredFromReload)
			{
				qWarning() << kReloadLogTag
				           << "Reload launch arguments were provided but recovery did not complete;"
				              " continuing normal startup.";
			}
			else
			{
				return;
			}
		}
		else
		{
			qWarning() << kReloadLogTag
			           << "Reload launch arguments were provided but reload request was disabled;"
			              " continuing normal startup.";
		}
	}

	const bool        skipStartupWorldListAutoOpen = startedWithReloadArgs;

	// simple command line parsing for auto-open behavior
	const QStringList args    = filterReloadStartupArguments(QCoreApplication::arguments());
	const auto        cmdLine = args.mid(1).join(QStringLiteral(" "));

	bool              bAutoOpen = true;
	if (skipStartupWorldListAutoOpen)
		bAutoOpen = false;

	if (cmdLine.isEmpty())
	{
		// No explicit action: startup flow opens defaults/auto-open entries as configured.
	}
	else
	{
		auto strTemp = cmdLine.toLower();
		strTemp      = strTemp.trimmed();

		// look for --noauto command-line option
		if (strTemp == QStringLiteral("--noauto"))
			bAutoOpen = false;
		else if (strTemp.contains(QStringLiteral(".mcl")) || strTemp.contains(QStringLiteral(".qdl")))
		// open an existing document
		{
			openDocumentFile(cmdLine);
		}
		else if (strTemp.startsWith(QLatin1Char('/')) || strTemp.startsWith(QLatin1Char('-')))
		{
		} // Ignore unknown startup switches.
		else
		{
			// switch to browser-telnet startup mode
			m_typeOfNewDocument = eTelnetFromBrowser;
			openDocumentFile(QString());
			// back to normal
			m_typeOfNewDocument = eNormalNewDocument;
			bAutoOpen           = false; // and cancel auto-open
		} // end of world and port supplied
	}

	m_autoOpen = bAutoOpen;

	// open all worlds specified in global preferences if no shift key is down
	if (const auto modifiers = QGuiApplication::keyboardModifiers();
	    !(modifiers & Qt::ShiftModifier) && m_autoOpen && !skipStartupWorldListAutoOpen)
	{
		auto       worldList         = getGlobalOption(QStringLiteral("WorldList")).toString();
		bool       worldListChanged  = false;
		const auto migratedWorldList = migrateWorldListPaths(m_workingDir, worldList, &worldListChanged);
		if (worldListChanged)
		{
			setGlobalOptionString(QStringLiteral("WorldList"), migratedWorldList);
			worldList = migratedWorldList;
		}
		worldList = canonicalizeWorldListForRuntime(worldList);
		if (!worldList.isEmpty())
		{
			const auto items = splitSerializedWorldList(worldList);
			openWorldsFromList(items, false);
		}
	}

	showTipAtStartup();
}

bool AppController::openWorldDocument(const QString &path)
{
	const bool allTypingToCommandWindow =
	    getGlobalOption(QStringLiteral("AllTypingToCommandWindow")).toInt() != 0;
	const QString wordDelimiters = getGlobalOption(QStringLiteral("WordDelimiters")).toString();
	const QString wordDelimitersDblClick =
	    getGlobalOption(QStringLiteral("WordDelimitersDblClick")).toString();
	const bool smoothScrolling   = getGlobalOption(QStringLiteral("SmoothScrolling")).toInt() != 0;
	const bool smootherScrolling = getGlobalOption(QStringLiteral("SmootherScrolling")).toInt() != 0;
	const bool bleedBackground   = getGlobalOption(QStringLiteral("BleedBackground")).toInt() != 0;

	auto       applyViewGlobalOptions = [&](WorldView *view)
	{
		if (!view)
			return;
		view->setAllTypingToCommandWindow(allTypingToCommandWindow);
		view->setWordDelimiters(wordDelimiters, wordDelimitersDblClick);
		view->setSmoothScrolling(smoothScrolling, smootherScrolling);
		view->setBleedBackground(bleedBackground);
	};

	auto shouldActivateNewWorld = [this]() -> bool
	{
		if (m_nextNewWorldActivationOverride >= 0)
		{
			const bool activate              = m_nextNewWorldActivationOverride != 0;
			m_nextNewWorldActivationOverride = -1;
			return activate;
		}
		return false;
	};

	// returns true if archive turns out to be XML
	auto isArchiveXML = [](const QString &fileName) -> bool
	{
		// auto-detect XML files

		static const char *sigs[] = {
		    "<?xml",     "<!--",      "<!DOCTYPE", "<muclient", "<qmud",      "<world",
		    "<triggers", "<aliases",  "<timers",   "<macros",   "<variables", "<colours",
		    "<keypad",   "<printing", "<comment",  "<include",  "<plugin",    "<script",
		};

		QByteArray buf(500, 0); // should be even number of bytes in case Unicode
		QByteArray buf2(500, 0);

		QFile      file(fileName);
		if (!file.open(QIODevice::ReadOnly))
			return false;
		const qint64 readBytes = file.read(buf.data(), buf.size() - 2); // allow for Unicode 00 00
		file.seek(0); // back to start for further serialization

		if (readBytes <= 0)
			return false;

		// look for Unicode (FF FE)
		if (static_cast<unsigned char>(buf[0]) == 0xFF && static_cast<unsigned char>(buf[1]) == 0xFE)
		{
			const auto *wide      = reinterpret_cast<const char16_t *>(buf.constData() + 2);
			const auto  wideLen   = static_cast<int>((readBytes - 2) / 2);
			const auto  converted = QString::fromUtf16(wide, wideLen);
			buf2                  = converted.toUtf8();
		}
		else
			// look for UTF-8 indicator bytes (EF BB BF)
			if (static_cast<unsigned char>(buf[0]) == 0xEF && static_cast<unsigned char>(buf[1]) == 0xBB &&
			    static_cast<unsigned char>(buf[2]) == 0xBF)
				buf2 = QByteArray(buf.constData() + 3);
			else
				buf2 = QByteArray(buf.constData());

		const char *p = buf2.constData();

		// skip leading whitespace
		while (*p && isAsciiSpace(*p))
			p++;

		// can't see them squeezing much into less than 15 chars
		//  (e.g. minimum would be <macros></macros> )
		const QByteArrayView remaining(p);
		if (remaining.size() < 15)
			return false;

		return std::ranges::any_of(sigs, [&remaining](const char *sig)
		                           { return remaining.startsWith(QByteArrayView(sig)); });
	};

	QString normalized = path;
#ifdef Q_OS_WIN
	normalized = QDir::fromNativeSeparators(normalized);
#else
	normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
#endif
	normalized = normalizePathString(normalized);
	if (!normalized.isEmpty())
	{
		normalized = migrateLegacyWorldFilePath(m_workingDir, normalized);
		if (const QFileInfo info(normalized); !info.exists())
		{
			QMessageBox::critical(m_mainWindow, QStringLiteral("QMud"),
			                      QStringLiteral("File not found: %1").arg(normalized));
			return false;
		}

		if (!isArchiveXML(normalized))
		{
			QMessageBox::critical(m_mainWindow, QStringLiteral("QMud"),
			                      QStringLiteral("File does not have a valid QMud XML signature."));
			return false;
		}

		WorldDocument doc;
		if (!doc.loadFromFile(normalized))
		{
			QMessageBox::critical(m_mainWindow, QStringLiteral("QMud"), doc.errorString());
			return false;
		}
		const auto stateDir =
		    makeAbsolutePath(getGlobalOption(QStringLiteral("StateFilesDirectory")).toString());
		const auto absolutePluginsDir = makeAbsolutePath(m_pluginsDirectory);
		if (!doc.expandIncludes(normalized, absolutePluginsDir, m_workingDir, stateDir))
		{
			QMessageBox::critical(m_mainWindow, QStringLiteral("QMud"), doc.errorString());
			return false;
		}

		auto *runtime = new WorldRuntime(m_mainWindow);
		runtime->setClientStartTime(m_whenClientStarted);
		runtime->setStartupDirectory(m_workingDir);
		runtime->setDefaultWorldDirectory(
		    makeAbsolutePath(getGlobalOption(QStringLiteral("DefaultWorldFileDirectory")).toString()));
		runtime->setDefaultLogDirectory(
		    makeAbsolutePath(getGlobalOption(QStringLiteral("DefaultLogFileDirectory")).toString()));
		runtime->setPluginsDirectory(absolutePluginsDir);
		runtime->setStateFilesDirectory(
		    makeAbsolutePath(getGlobalOption(QStringLiteral("StateFilesDirectory")).toString()));
		runtime->setFileBrowsingDirectory(m_fileBrowsingDir);
		runtime->setPreferencesDatabaseName(m_preferencesDatabaseName);
		runtime->setTranslatorFile(m_translatorFile);
		runtime->setLocale(m_locale);
		runtime->setFixedPitchFont(getGlobalOption(QStringLiteral("FixedPitchFont")).toString());
		runtime->applyPackageRestrictions(getGlobalOption(QStringLiteral("AllowLoadingDlls")).toInt() != 0);
		runtime->setReconnectOnLinkFailure(
		    getGlobalOption(QStringLiteral("ReconnectOnLinkFailure")).toInt() != 0);
		if (!m_mainWindow)
			return false;
		const QFileInfo loadedInfo(normalized);
		auto            title = loadedInfo.fileName();
		if (QMudFileExtensions::isWorldSuffix(loadedInfo.suffix().toLower()))
			title = loadedInfo.completeBaseName();
		if (title.trimmed().isEmpty())
			title = loadedInfo.fileName();
		auto *window = new WorldChildWindow(title);
		window->setRuntime(runtime);
		m_mainWindow->addMdiSubWindow(window, shouldActivateNewWorld());
		applyViewGlobalOptions(window->view());
		runtime->setPluginInstallDeferred(true);
		runtime->applyFromDocument(doc);
		runtime->setWorldFilePath(normalized);
		const auto &attrs            = runtime->worldAttributes();
		const auto  useDefaultInput  = attrs.value(QStringLiteral("use_default_input_font"));
		const auto  useDefaultOutput = attrs.value(QStringLiteral("use_default_output_font"));
		const auto  useInput = useDefaultInput.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
		                      useDefaultInput == QStringLiteral("1") ||
		                      useDefaultInput.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
		const auto useOutput = useDefaultOutput.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
		                       useDefaultOutput == QStringLiteral("1") ||
		                       useDefaultOutput.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
		if (useInput)
		{
			const auto inputFont   = getGlobalOption(QStringLiteral("DefaultInputFont")).toString();
			const auto inputHeight = getGlobalOption(QStringLiteral("DefaultInputFontHeight")).toInt();
			const auto inputWeight = getGlobalOption(QStringLiteral("DefaultInputFontWeight")).toInt();
			const auto inputItalic = getGlobalOption(QStringLiteral("DefaultInputFontItalic")).toInt();
			const auto inputCharset =
			    dbOnlyGlobalIntValue(m_globalIntPrefs, QStringLiteral("DefaultInputFontCharset"));
			if (!inputFont.isEmpty())
				runtime->setWorldAttribute(QStringLiteral("input_font_name"), inputFont);
			runtime->setWorldAttribute(QStringLiteral("input_font_height"), QString::number(inputHeight));
			runtime->setWorldAttribute(QStringLiteral("input_font_weight"), QString::number(inputWeight));
			runtime->setWorldAttribute(QStringLiteral("input_font_italic"), QString::number(inputItalic));
			runtime->setWorldAttribute(QStringLiteral("input_font_charset"), QString::number(inputCharset));
		}
		if (useOutput)
		{
			const auto     outputFont   = getGlobalOption(QStringLiteral("DefaultOutputFont")).toString();
			const auto     outputHeight = getGlobalOption(QStringLiteral("DefaultOutputFontHeight")).toInt();
			constexpr auto outputWeight = 400;
			const auto     outputCharset =
			    dbOnlyGlobalIntValue(m_globalIntPrefs, QStringLiteral("DefaultOutputFontCharset"));
			if (!outputFont.isEmpty())
				runtime->setWorldAttribute(QStringLiteral("output_font_name"), outputFont);
			runtime->setWorldAttribute(QStringLiteral("output_font_height"), QString::number(outputHeight));
			runtime->setWorldAttribute(QStringLiteral("output_font_weight"), QString::number(outputWeight));
			runtime->setWorldAttribute(QStringLiteral("output_font_charset"), QString::number(outputCharset));
		}
		const auto regexpMatchEmpty = getGlobalOption(QStringLiteral("RegexpMatchEmpty")).toInt();
		runtime->setWorldAttribute(QStringLiteral("regexp_match_empty"), QString::number(regexpMatchEmpty));
		const auto notifyCannotConnect = getGlobalOption(QStringLiteral("NotifyIfCannotConnect")).toInt();
		const auto notifyDisconnect    = getGlobalOption(QStringLiteral("NotifyOnDisconnect")).toInt();
		const auto errorToOutput = getGlobalOption(QStringLiteral("ErrorNotificationToOutputWindow")).toInt();
		runtime->setWorldAttribute(QStringLiteral("notify_if_cannot_connect"),
		                           QString::number(notifyCannotConnect));
		runtime->setWorldAttribute(QStringLiteral("notify_on_disconnect"), QString::number(notifyDisconnect));
		runtime->setWorldAttribute(QStringLiteral("error_notification_to_output"),
		                           QString::number(errorToOutput));
		applyConfiguredWorldDefaults(runtime);
		if (auto *view = window->view())
			view->applyRuntimeSettings();
		const auto worldName = runtime->worldAttributes().value(QStringLiteral("name")).trimmed();
		if (!worldName.isEmpty())
		{
			window->setWindowTitle(worldName);
			if (auto *view = window->view())
				view->setWorldName(worldName);
		}
		const auto placementName = worldName.isEmpty() ? window->windowTitle() : worldName;
		restoreWorldWindowPlacement(placementName, window);
		if (getGlobalOption(QStringLiteral("OpenWorldsMaximised")).toInt() != 0)
			window->showMaximized();
		if (m_suppressAutoConnect)
		{
			// Reload-recovery path restores state/reconnect policy explicitly after open.
			return true;
		}
		const QPointer<WorldRuntime> runtimeGuard(runtime);
		restoreWorldSessionStateAsync(
		    runtime, window->view(),
		    [this, runtimeGuard](const bool ok, const QString &error)
		    {
			    if (!runtimeGuard)
				    return;
			    QMudWorldSessionRestoreFlow::runPostRestoreFlow(
			        ok, error,
			        {
			            [this, runtimeGuard] { runWorldStartupPostRestore(runtimeGuard); },
			            [this, runtimeGuard] { maybeAutoConnectWorld(runtimeGuard); },
			            [runtimeGuard](const QString &restoreError)
			            {
				            qWarning()
				                << "Failed to restore world session state for"
				                << runtimeGuard->worldAttributes().value(QStringLiteral("name")).trimmed()
				                << ":" << restoreError;
			            },
			        });
		    });
		return true;
	}

	if (!m_mainWindow)
	{
		return false;
	}

	const auto title   = path.isEmpty() ? QStringLiteral("World") : QFileInfo(path).fileName();
	auto      *runtime = new WorldRuntime(m_mainWindow);
	initializeWorldRuntime(runtime);
	auto *window = new WorldChildWindow(title);
	window->setRuntime(runtime);
	m_mainWindow->addMdiSubWindow(window, shouldActivateNewWorld());
	applyViewGlobalOptions(window->view());
	restoreWorldWindowPlacement(window->windowTitle(), window);
	if (getGlobalOption(QStringLiteral("OpenWorldsMaximised")).toInt() != 0)
		window->showMaximized();
	runtime->setPluginInstallDeferred(true);
	applyConfiguredWorldDefaults(runtime);
	if (m_suppressAutoConnect)
		return true;
	const QPointer<WorldRuntime> runtimeGuard(runtime);
	restoreWorldSessionStateAsync(
	    runtime, window->view(),
	    [this, runtimeGuard](const bool ok, const QString &error)
	    {
		    if (!runtimeGuard)
			    return;
		    QMudWorldSessionRestoreFlow::runPostRestoreFlow(
		        ok, error,
		        {
		            [this, runtimeGuard] { runWorldStartupPostRestore(runtimeGuard); },
		            [this, runtimeGuard] { maybeAutoConnectWorld(runtimeGuard); },
		            [runtimeGuard](const QString &restoreError)
		            {
			            qWarning() << "Failed to restore world session state for"
			                       << runtimeGuard->worldAttributes().value(QStringLiteral("name")).trimmed()
			                       << ":" << restoreError;
		            },
		        });
	    });
	return true;
}

void AppController::maybeAutoConnectWorld(WorldRuntime *runtime) const
{
	if (!runtime)
		return;
	if (m_suppressAutoConnect)
		return;
	if (getGlobalOption(QStringLiteral("AutoConnectWorlds")).toInt() == 0)
		return;
	if (runtime->isConnected() || runtime->isConnecting())
		return;

	const auto &attrs     = runtime->worldAttributes();
	const auto  worldName = attrs.value(QStringLiteral("name"));
	const auto  host      = attrs.value(QStringLiteral("site"));
	const auto  port      = attrs.value(QStringLiteral("port")).toInt();

	if (host == QStringLiteral("0.0.0.0"))
		return;

	if (worldName.isEmpty())
	{
		QMessageBox::warning(m_mainWindow, QStringLiteral("QMud"),
		                     QStringLiteral("Cannot connect. World name not specified"));
		return;
	}

	if (host.isEmpty())
	{
		QMessageBox::warning(
		    m_mainWindow, QStringLiteral("QMud"),
		    QStringLiteral("Cannot connect to \"%1\", TCP/IP address not specified").arg(worldName));
		return;
	}

	if (port <= 0)
	{
		QMessageBox::warning(
		    m_mainWindow, QStringLiteral("QMud"),
		    QStringLiteral("Cannot connect to \"%1\", port number not specified").arg(worldName));
		return;
	}

	runtime->connectToWorld(host, static_cast<quint16>(port));
}

void AppController::initializeWorldRuntime(WorldRuntime *runtime) const
{
	if (!runtime)
		return;

	runtime->applyDefaultWorldOptions();
	runtime->setClientStartTime(m_whenClientStarted);
	runtime->setStartupDirectory(m_workingDir);
	runtime->setDefaultWorldDirectory(
	    makeAbsolutePath(getGlobalOption(QStringLiteral("DefaultWorldFileDirectory")).toString()));
	runtime->setDefaultLogDirectory(
	    makeAbsolutePath(getGlobalOption(QStringLiteral("DefaultLogFileDirectory")).toString()));
	runtime->setPluginsDirectory(makeAbsolutePath(m_pluginsDirectory));
	runtime->setStateFilesDirectory(
	    makeAbsolutePath(getGlobalOption(QStringLiteral("StateFilesDirectory")).toString()));
	runtime->setFileBrowsingDirectory(m_fileBrowsingDir);
	runtime->setPreferencesDatabaseName(m_preferencesDatabaseName);
	runtime->setTranslatorFile(m_translatorFile);
	runtime->setLocale(m_locale);
	runtime->setFixedPitchFont(getGlobalOption(QStringLiteral("FixedPitchFont")).toString());
	runtime->applyPackageRestrictions(getGlobalOption(QStringLiteral("AllowLoadingDlls")).toInt() != 0);
	runtime->setReconnectOnLinkFailure(getGlobalOption(QStringLiteral("ReconnectOnLinkFailure")).toInt() !=
	                                   0);

	const QString defaultInput    = getGlobalOption(QStringLiteral("DefaultInputFont")).toString();
	const QString defaultOutput   = getGlobalOption(QStringLiteral("DefaultOutputFont")).toString();
	const QString defaultTriggers = getGlobalOption(QStringLiteral("DefaultTriggersFile")).toString();
	const QString defaultAliases  = getGlobalOption(QStringLiteral("DefaultAliasesFile")).toString();
	const QString defaultTimers   = getGlobalOption(QStringLiteral("DefaultTimersFile")).toString();
	const QString defaultMacros   = getGlobalOption(QStringLiteral("DefaultMacrosFile")).toString();
	const QString defaultColours  = getGlobalOption(QStringLiteral("DefaultColoursFile")).toString();
	if (!defaultInput.isEmpty())
		runtime->setWorldAttribute(QStringLiteral("use_default_input_font"), QStringLiteral("1"));
	if (!defaultOutput.isEmpty())
		runtime->setWorldAttribute(QStringLiteral("use_default_output_font"), QStringLiteral("1"));
	if (!defaultTriggers.isEmpty())
		runtime->setWorldAttribute(QStringLiteral("use_default_triggers"), QStringLiteral("1"));
	if (!defaultAliases.isEmpty())
		runtime->setWorldAttribute(QStringLiteral("use_default_aliases"), QStringLiteral("1"));
	if (!defaultTimers.isEmpty())
		runtime->setWorldAttribute(QStringLiteral("use_default_timers"), QStringLiteral("1"));
	if (!defaultMacros.isEmpty())
		runtime->setWorldAttribute(QStringLiteral("use_default_macros"), QStringLiteral("1"));
	if (!defaultColours.isEmpty())
		runtime->setWorldAttribute(QStringLiteral("use_default_colours"), QStringLiteral("1"));

	const int regexpMatchEmpty = getGlobalOption(QStringLiteral("RegexpMatchEmpty")).toInt();
	runtime->setWorldAttribute(QStringLiteral("regexp_match_empty"), QString::number(regexpMatchEmpty));
	const int notifyCannotConnect = getGlobalOption(QStringLiteral("NotifyIfCannotConnect")).toInt();
	const int notifyDisconnect    = getGlobalOption(QStringLiteral("NotifyOnDisconnect")).toInt();
	const int errorToOutput = getGlobalOption(QStringLiteral("ErrorNotificationToOutputWindow")).toInt();
	runtime->setWorldAttribute(QStringLiteral("notify_if_cannot_connect"),
	                           QString::number(notifyCannotConnect));
	runtime->setWorldAttribute(QStringLiteral("notify_on_disconnect"), QString::number(notifyDisconnect));
	runtime->setWorldAttribute(QStringLiteral("error_notification_to_output"),
	                           QString::number(errorToOutput));
}

void AppController::applyConfiguredWorldDefaults(WorldRuntime *runtime) const
{
	if (!runtime)
		return;

	const QMap<QString, QString> &attrs = runtime->worldAttributes();
	const QString                 stateDir =
	    makeAbsolutePath(getGlobalOption(QStringLiteral("StateFilesDirectory")).toString());

	auto loadDefaultDoc = [&](const QString &prefKey, const unsigned long mask, WorldDocument &doc) -> bool
	{
		const QString configured = m_globalStringPrefs.value(prefKey, QString()).trimmed();
		if (configured.isEmpty())
			return false;
		const QString fileName = makeAbsolutePath(configured);
		if (!QFileInfo::exists(fileName))
			return false;
		doc.setLoadMask(mask | WorldDocument::XML_NO_PLUGINS | WorldDocument::XML_IMPORT_MAIN_FILE_ONLY);
		if (!doc.loadFromFile(fileName))
			return false;
		const QString absolutePluginsDir = makeAbsolutePath(m_pluginsDirectory);
		return doc.expandIncludes(fileName, absolutePluginsDir, m_workingDir, stateDir);
	};

	if (isEnabledFlag(attrs.value(QStringLiteral("use_default_triggers"))))
	{
		if (WorldDocument doc;
		    loadDefaultDoc(QStringLiteral("DefaultTriggersFile"), WorldDocument::XML_TRIGGERS, doc))
		{
			QList<WorldRuntime::Trigger> combined;
			combined.reserve(doc.triggers().size());
			for (const auto &[attributes, children, included] : doc.triggers())
			{
				WorldRuntime::Trigger trigger;
				trigger.attributes = attributes;
				trigger.children   = children;
				trigger.included   = included;
				combined.push_back(trigger);
			}
			runtime->setTriggers(combined);
		}
	}

	if (isEnabledFlag(attrs.value(QStringLiteral("use_default_aliases"))))
	{
		if (WorldDocument doc;
		    loadDefaultDoc(QStringLiteral("DefaultAliasesFile"), WorldDocument::XML_ALIASES, doc))
		{
			QList<WorldRuntime::Alias> combined;
			combined.reserve(doc.aliases().size());
			for (const auto &[attributes, children, included] : doc.aliases())
			{
				WorldRuntime::Alias alias;
				alias.attributes = attributes;
				alias.children   = children;
				alias.included   = included;
				combined.push_back(alias);
			}
			runtime->setAliases(combined);
		}
	}

	if (isEnabledFlag(attrs.value(QStringLiteral("use_default_timers"))))
	{
		if (WorldDocument doc;
		    loadDefaultDoc(QStringLiteral("DefaultTimersFile"), WorldDocument::XML_TIMERS, doc))
		{
			QList<WorldRuntime::Timer> combined;
			combined.reserve(doc.timers().size());
			for (const auto &[attributes, children, included] : doc.timers())
			{
				WorldRuntime::Timer timer;
				timer.attributes = attributes;
				timer.children   = children;
				timer.included   = included;
				combined.push_back(timer);
			}
			runtime->setTimers(combined);
		}
	}

	if (isEnabledFlag(attrs.value(QStringLiteral("use_default_macros"))))
	{
		if (WorldDocument doc;
		    loadDefaultDoc(QStringLiteral("DefaultMacrosFile"), WorldDocument::XML_MACROS, doc))
		{
			QMap<QString, QString>           macroIndexByName;
			const QList<WorldRuntime::Macro> existingMacros = runtime->macros();
			for (int i = 0; i < existingMacros.size(); ++i)
			{
				const QString name = existingMacros.at(i).attributes.value(QStringLiteral("name")).trimmed();
				if (name.isEmpty())
					continue;
				QString index = existingMacros.at(i).attributes.value(QStringLiteral("index")).trimmed();
				if (index.isEmpty())
					index = QString::number(i);
				macroIndexByName.insert(name, index);
			}
			QList<WorldRuntime::Macro> macros;
			macros.reserve(doc.macros().size());
			for (const auto &[attributes, children] : doc.macros())
			{
				auto macro = WorldRuntime::Macro{attributes, children};
				if (const QString macroName = macro.attributes.value(QStringLiteral("name")).trimmed();
				    macroIndexByName.contains(macroName))
				{
					macro.attributes.insert(QStringLiteral("index"), macroIndexByName.value(macroName));
				}
				macros.push_back(macro);
			}
			runtime->setMacros(macros);
		}
	}

	if (isEnabledFlag(attrs.value(QStringLiteral("use_default_colours"))))
	{
		if (WorldDocument doc;
		    loadDefaultDoc(QStringLiteral("DefaultColoursFile"), WorldDocument::XML_COLOURS, doc))
		{
			QList<WorldRuntime::Colour> colours;
			colours.reserve(doc.colours().size());
			for (const auto &[group, attributes] : doc.colours())
				colours.push_back({group, attributes});
			runtime->setColours(colours);
		}
	}
}

void AppController::emitStartupBanner(WorldRuntime *runtime)
{
	if (!runtime)
		return;

	constexpr QRgb bannerFore = qRgb(0, 170, 255);
	constexpr QRgb bannerBack = qRgb(0, 0, 0);
	constexpr QRgb linkFore   = qRgb(0, 255, 255);

	auto           emitStyledNote = [runtime](const QString &text, const bool newLine)
	{
		if (text.isEmpty())
		{
			runtime->outputText(QString(), true, true);
			return;
		}
		WorldRuntime::StyleSpan span;
		span.length  = static_cast<int>(text.size());
		span.fore    = QColor::fromRgb(bannerFore);
		span.back    = QColor::fromRgb(bannerBack);
		span.changed = true;
		runtime->outputStyledText(text, {span}, true, newLine);
	};

	auto emitStyledLink = [runtime](const QString &url, const QString &text, const bool newLine)
	{
		WorldRuntime::StyleSpan span;
		span.length     = static_cast<int>(text.size());
		span.fore       = QColor::fromRgb(linkFore);
		span.back       = QColor::fromRgb(bannerBack);
		span.underline  = true;
		span.changed    = true;
		span.actionType = WorldRuntime::ActionHyperlink;
		span.action     = url;
		span.hint       = text;
		runtime->outputStyledText(text, {span}, true, newLine);
	};

	emitStyledNote(QString(), true);
	emitStyledNote(QStringLiteral("Welcome to QMud version %1!").arg(QString::fromLatin1(kVersionString)),
	               true);
	emitStyledNote(QStringLiteral("Written by Panagiotis Kalogiratos (Nodens)."), true);
	emitStyledNote(QString(), true);
	emitStyledNote(
	    QStringLiteral("A compatible, modern, Qt successor to MUSHclient (originally by Nick Gammon)."),
	    true);
	emitStyledNote(QString(), true);
	emitStyledNote(QStringLiteral("Compiled: %1 at %2 UTC.")
	                   .arg(QStringLiteral(QMUD_BUILD_DATE_UTC), QStringLiteral(QMUD_BUILD_TIME_UTC)),
	               true);
	emitStyledNote(QStringLiteral("Using: %1, Qt %2, zlib %3")
	                   .arg(QStringLiteral(LUA_RELEASE), QStringLiteral(QT_VERSION_STR),
	                        QString::fromLatin1(zlibVersion())),
	               true);
	emitStyledNote(QString(), true);
	emitStyledNote(QStringLiteral("For information and assistance about QMud visit the CthulhuMUD Discord: "),
	               false);
	emitStyledLink(QStringLiteral("https://discord.gg/secxwnTJCq"),
	               QStringLiteral("https://discord.gg/secxwnTJCq"), true);
	emitStyledNote(QString(), true);
}

void AppController::processScriptFileChange(WorldRuntime *runtime)
{
	if (!runtime || !runtime->scriptFileChanged())
		return;

	const QMap<QString, QString> &attrs = runtime->worldAttributes();
	const bool                    scriptingEnabled =
	    isEnabledFlag(attrs.value(QStringLiteral("enable_scripts"))) &&
	    attrs.value(QStringLiteral("script_language")).compare(QStringLiteral("Lua"), Qt::CaseInsensitive) ==
	        0;
	if (!scriptingEnabled)
	{
		runtime->setScriptFileChanged(false);
		return;
	}

	int reloadOption = attrs.value(QStringLiteral("script_reload_option")).toInt();
	if (reloadOption < eReloadConfirm || reloadOption > eReloadNever)
		reloadOption = eReloadConfirm;

	if (reloadOption == eReloadNever)
	{
		runtime->setScriptFileChanged(false);
		return;
	}

	if (reloadOption == eReloadAlways)
	{
		runtime->setScriptFileChanged(false);
		onCommandTriggered(QStringLiteral("ReloadScriptFile"));
		return;
	}

	runtime->setScriptFileChanged(false);
	const QMessageBox::StandardButton answer =
	    QMessageBox::question(m_mainWindow, QStringLiteral("Reload Script File"),
	                          QStringLiteral("Script file has changed on disk. Reload it now?"),
	                          QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
	if (answer == QMessageBox::Yes)
		onCommandTriggered(QStringLiteral("ReloadScriptFile"));
}

bool AppController::openTextDocument(const QString &path) const
{
	if (!m_mainWindow)
		return false;

	QString text;
	if (QFile file(path); file.open(QIODevice::ReadOnly))
		text = QString::fromLocal8Bit(file.readAll());

	const auto title = QFileInfo(path).fileName();
	auto      *child = new TextChildWindow(title, text);
	child->setFilePath(path);
	if (const QPlainTextEdit *editor = child->editor())
		if (editor->document())
			editor->document()->setModified(false);
	m_mainWindow->addMdiSubWindow(child);
	return true;
}

void AppController::applyWindowPreferences()
{
	if (!m_mainWindow)
		return;

	// Always on top
	const int             alwaysOnTop   = getGlobalOption(QStringLiteral("AlwaysOnTop")).toInt();
	const Qt::WindowFlags originalFlags = m_mainWindow->windowFlags();
	Qt::WindowFlags       flags         = originalFlags;
	if (alwaysOnTop)
		flags |= Qt::WindowStaysOnTopHint;
	else
		flags &= ~Qt::WindowStaysOnTopHint;
	if (flags != originalFlags)
	{
		m_mainWindow->setWindowFlags(flags);
		if (!m_deferMainWindowShowUntilSplash)
			m_mainWindow->show();
	}

	// open activity window if wanted
	if (const auto openActivity = getGlobalOption(QStringLiteral("OpenActivityWindow")).toInt(); openActivity)
	{
		ensureActivityDocument();
		if (!m_mainWindow->hasActivityWindow())
			m_mainWindow->addMdiSubWindow(new ActivityChildWindow);
	}

	// Icon/tray visibility is handled by applyIconPreferences().
}

void AppController::applyUpdatePreferences()
{
	const bool enableReloadFeature = getGlobalOption(QStringLiteral("EnableReloadFeature")).toInt() != 0;
	if (m_mainWindow)
	{
		if (QAction *reloadAction = m_mainWindow->actionForCommand(QStringLiteral("ReloadQMud")))
		{
			reloadAction->setVisible(enableReloadFeature);
			reloadAction->setEnabled(enableReloadFeature);
		}
	}
	configureUpdateCheckTimer();
}

void AppController::applyViewPreferences() const
{
	if (!m_mainWindow)
		return;

	// Apply persisted view visibility toggles.
	const int showToolbar      = dbOnlyGlobalIntValue(m_globalIntPrefs, QStringLiteral("ViewToolbar"));
	const int showWorldToolbar = dbOnlyGlobalIntValue(m_globalIntPrefs, QStringLiteral("ViewWorldToolbar"));
	const int showActivityToolbar = dbOnlyGlobalIntValue(m_globalIntPrefs, QStringLiteral("ActivityToolbar"));
	const int showStatusbar       = dbOnlyGlobalIntValue(m_globalIntPrefs, QStringLiteral("ViewStatusbar"));
	const int showInfoBar         = dbOnlyGlobalIntValue(m_globalIntPrefs, QStringLiteral("ViewInfoBar"));

	m_mainWindow->setToolbarVisible(showToolbar != 0);
	m_mainWindow->setWorldToolbarVisible(showWorldToolbar != 0);
	m_mainWindow->setActivityToolbarVisible(showActivityToolbar != 0);
	m_mainWindow->setStatusbarVisible(showStatusbar != 0);
	m_mainWindow->setInfoBarVisible(showInfoBar != 0);
}

void AppController::saveViewPreferences()
{
	// Persist toolbar/status/info bar visibility state from the actual widgets.
	// This keeps restart/reload behavior correct even if visibility changed outside
	// the View-menu actions.
	const int viewToolbar =
	    m_mainWindow && m_mainWindow->mainToolbar() && !m_mainWindow->mainToolbar()->isHidden() ? 1 : 0;
	const int viewWorldToolbar =
	    m_mainWindow && m_mainWindow->worldToolbar() && !m_mainWindow->worldToolbar()->isHidden() ? 1 : 0;
	const int activityToolbar =
	    m_mainWindow && m_mainWindow->activityToolbar() && !m_mainWindow->activityToolbar()->isHidden() ? 1
	                                                                                                    : 0;
	const int viewStatusbar =
	    m_mainWindow && m_mainWindow->frameStatusBar() && !m_mainWindow->frameStatusBar()->isHidden() ? 1 : 0;
	const int viewInfoBar =
	    m_mainWindow && m_mainWindow->infoDock() && !m_mainWindow->infoDock()->isHidden() ? 1 : 0;

	(void)dbWriteInt(QStringLiteral("prefs"), QStringLiteral("ViewToolbar"), viewToolbar);
	(void)dbWriteInt(QStringLiteral("prefs"), QStringLiteral("ViewWorldToolbar"), viewWorldToolbar);
	(void)dbWriteInt(QStringLiteral("prefs"), QStringLiteral("ActivityToolbar"), activityToolbar);
	(void)dbWriteInt(QStringLiteral("prefs"), QStringLiteral("ViewStatusbar"), viewStatusbar);
	(void)dbWriteInt(QStringLiteral("prefs"), QStringLiteral("ViewInfoBar"), viewInfoBar);

	// Keep runtime cache aligned with persisted DB-only view-state keys.
	m_globalIntPrefs.insert(QStringLiteral("ViewToolbar"), viewToolbar);
	m_globalIntPrefs.insert(QStringLiteral("ViewWorldToolbar"), viewWorldToolbar);
	m_globalIntPrefs.insert(QStringLiteral("ActivityToolbar"), activityToolbar);
	m_globalIntPrefs.insert(QStringLiteral("ViewStatusbar"), viewStatusbar);
	m_globalIntPrefs.insert(QStringLiteral("ViewInfoBar"), viewInfoBar);

	if (m_mainWindow)
	{
		QSettings settings(iniFilePath(), QSettings::IniFormat);
		settings.beginGroup(QStringLiteral("CtrlBars"));
		settings.setValue(QStringLiteral("QtState"), m_mainWindow->saveState());
		settings.endGroup();
	}
}

void AppController::loadToolbarLayout() const
{
	if (!m_mainWindow)
		return;

	QSettings settings(iniFilePath(), QSettings::IniFormat);
	pruneLegacyCtrlBarsGroups(settings);
	settings.beginGroup(QStringLiteral("CtrlBars"));
	if (const QByteArray state = settings.value(QStringLiteral("QtState")).toByteArray(); !state.isEmpty())
	{
		m_mainWindow->restoreState(state);
		m_mainWindow->syncToolbarVisibilityFromState();
	}
	settings.endGroup();
	settings.sync();
}

void AppController::applyGlobalPreferences()
{
	applyWindowPreferences();
	applyUpdatePreferences();
	applyViewPreferences();
	applyConnectionPreferences();
	applySpellCheckPreferences();
	applyPluginPreferences();
	applyDefaultDirectories();
	applyFontPreferences();
	applyNotepadPreferences();
	applyChildWindowPreferences();
	applyTimerPreferences();
	applyWordDelimiterPreferences();
	applyEditorPreferences();
	applyLocalePreferences();
	applyListViewPreferences();
	applyInputPreferences();
	applyNotificationPreferences();
	applyTypingPreferences();
	applyRegexPreferences();
	applyIconPreferences();
	applyFontMetricPreferences();
	applyActivityPreferences();
	applyPackagePreferences();
	applyMiscPreferences();
	applyRenderingPreferences();
}

void AppController::configureUpdateCheckTimer()
{
	if (!m_updateCheckTimer)
	{
		m_updateCheckTimer = new QTimer(this);
		m_updateCheckTimer->setSingleShot(false);
		connect(m_updateCheckTimer, &QTimer::timeout, this, [this]() { requestUpdateCheck(false); });
	}

	const auto clearDiscoveredUpdateState = [this]()
	{
		m_availableUpdateVersion.clear();
		m_availableUpdateChangelog.clear();
		m_availableUpdateAssetUrl.clear();
		m_availableUpdateAssetName.clear();
		m_availableUpdateAssetSha256.clear();
	};

	if (detectUpdateInstallTarget() == UpdateInstallTarget::Unsupported)
	{
		m_updateCheckTimer->stop();
		setUpdateNowActionVisible(false);
		clearDiscoveredUpdateState();
		return;
	}

	const bool autoCheckEnabled = getGlobalOption(QStringLiteral("AutoCheckForUpdates")).toInt() != 0;
	const int  intervalHours =
	    qBound(1, getGlobalOption(QStringLiteral("UpdateCheckIntervalHours")).toInt(), 168);
	const int intervalMs = intervalHours * 60 * 60 * 1000;

	if (!autoCheckEnabled)
	{
		m_updateCheckTimer->stop();
		return;
	}

	if (m_updateCheckTimer->interval() != intervalMs)
		m_updateCheckTimer->setInterval(intervalMs);
	if (!m_updateCheckTimer->isActive())
		m_updateCheckTimer->start();
}

void AppController::requestUpdateCheck(const bool manual, QWidget *uiParent)
{
	if (manual)
	{
		m_updateUiParent = uiParent;
		if (!m_updateUiParent)
			m_updateUiParent = QApplication::activeModalWidget();
	}

	if (m_updatePackageDownloadInProgress)
	{
		if (manual)
		{
			QWidget *uiOwner =
			    m_updateUiParent ? m_updateUiParent.data() : static_cast<QWidget *>(m_mainWindow);
			QMessageBox::information(uiOwner, QStringLiteral("QMud Update"),
			                         QStringLiteral("An update installation is already in progress."));
		}
		return;
	}

	const auto clearDiscoveredUpdateState = [this]()
	{
		m_availableUpdateVersion.clear();
		m_availableUpdateChangelog.clear();
		m_availableUpdateAssetUrl.clear();
		m_availableUpdateAssetName.clear();
		m_availableUpdateAssetSha256.clear();
	};

	if (detectUpdateInstallTarget() == UpdateInstallTarget::Unsupported)
	{
		setUpdateNowActionVisible(false);
		clearDiscoveredUpdateState();
		if (m_updateCheckTimer)
			m_updateCheckTimer->stop();
		if (manual)
		{
			QWidget *uiOwner =
			    m_updateUiParent ? m_updateUiParent.data() : static_cast<QWidget *>(m_mainWindow);
			QMessageBox::information(uiOwner, QStringLiteral("QMud Update"), updateNotSupportedMessage());
		}
		return;
	}

	if (m_updateCheckInProgress)
	{
		if (manual)
		{
			QWidget *uiOwner =
			    m_updateUiParent ? m_updateUiParent.data() : static_cast<QWidget *>(m_mainWindow);
			QMessageBox::information(uiOwner, QStringLiteral("QMud Update"),
			                         QStringLiteral("An update check is already in progress."));
		}
		return;
	}

	if (!m_updateNetworkManager)
		m_updateNetworkManager = new QNetworkAccessManager(this);

	QNetworkRequest request(QUrl(QString::fromLatin1(kUpdateLatestReleaseUrl)));
	request.setRawHeader("User-Agent", "QMud");
	request.setRawHeader("Accept", "application/vnd.github+json");

	QNetworkReply *reply    = m_updateNetworkManager->get(request);
	m_updateCheckInProgress = true;
	connect(reply, &QNetworkReply::finished, this,
	        [this, reply, manual]()
	        {
		        const QByteArray payload = reply->readAll();
		        QString          networkError;
		        if (reply->error() != QNetworkReply::NoError)
			        networkError = reply->errorString();
		        int httpStatus = 0;
		        if (const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
		            status.isValid())
		        {
			        httpStatus = status.toInt();
		        }
		        reply->deleteLater();
		        m_updateCheckInProgress = false;
		        handleUpdateCheckResponse(manual, payload, networkError, httpStatus);
	        });
}

void AppController::handleUpdateCheckResponse(const bool manual, const QByteArray &payload,
                                              const QString &networkError, const int httpStatus)
{
	const auto clearDiscoveredUpdateState = [this]()
	{
		m_availableUpdateVersion.clear();
		m_availableUpdateChangelog.clear();
		m_availableUpdateAssetUrl.clear();
		m_availableUpdateAssetName.clear();
		m_availableUpdateAssetSha256.clear();
	};

	QWidget *uiOwner = nullptr;
	if (QWidget *activeModal = QApplication::activeModalWidget(); activeModal)
		uiOwner = activeModal;
	else if (m_updateUiParent && m_updateUiParent->isVisible())
		uiOwner = m_updateUiParent.data();
	else
		uiOwner = m_mainWindow;

	if (!networkError.isEmpty() || (httpStatus >= 400))
	{
		if (manual)
		{
			const QString message =
			    httpStatus > 0
			        ? QStringLiteral("Update check failed (%1): %2").arg(httpStatus).arg(networkError)
			        : QStringLiteral("Update check failed: %1").arg(networkError);
			QMessageBox::warning(uiOwner, QStringLiteral("QMud Update"), message);
		}
		return;
	}

	const QString currentVersion = versionCore(m_version);
	const QString skipVersion =
	    versionCore(getGlobalOption(QStringLiteral("SkipUpdateNotificationVersion")).toString());

	const QMudUpdateCheck::ReleaseEvaluationResult evaluation = QMudUpdateCheck::evaluateLatestReleasePayload(
	    payload, currentVersion, skipVersion, detectUpdateInstallTarget());
	if (evaluation.clearSkipVersion)
		setGlobalOptionString(QStringLiteral("SkipUpdateNotificationVersion"), QString());

	switch (evaluation.status)
	{
	case QMudUpdateCheck::ReleaseEvaluationStatus::ParseError:
		if (manual)
		{
			QMessageBox::warning(uiOwner, QStringLiteral("QMud Update"),
			                     QStringLiteral("Failed to parse update response."));
		}
		return;
	case QMudUpdateCheck::ReleaseEvaluationStatus::NoStableRelease:
		setUpdateNowActionVisible(false);
		clearDiscoveredUpdateState();
		if (manual)
		{
			QMessageBox::information(uiOwner, QStringLiteral("QMud Update"),
			                         QStringLiteral("No stable update is currently available."));
		}
		return;
	case QMudUpdateCheck::ReleaseEvaluationStatus::InvalidVersion:
		if (manual)
		{
			QMessageBox::warning(uiOwner, QStringLiteral("QMud Update"),
			                     QStringLiteral("Update response did not include a valid version."));
		}
		return;
	case QMudUpdateCheck::ReleaseEvaluationStatus::UpToDate:
		setUpdateNowActionVisible(false);
		clearDiscoveredUpdateState();
		if (manual)
		{
			QMessageBox::information(
			    uiOwner, QStringLiteral("QMud Update"),
			    QStringLiteral("Your current version (v%1) is up to date.").arg(currentVersion));
		}
		return;
	case QMudUpdateCheck::ReleaseEvaluationStatus::NoCompatibleAsset:
		setUpdateNowActionVisible(false);
		clearDiscoveredUpdateState();
		if (manual)
		{
			QMessageBox::information(
			    uiOwner, QStringLiteral("QMud Update"),
			    QStringLiteral("A newer version (v%1) is available, but no compatible update package was "
			                   "found for this platform.")
			        .arg(evaluation.releaseVersion));
		}
		return;
	case QMudUpdateCheck::ReleaseEvaluationStatus::UpdateAvailable:
		break;
	}

	m_availableUpdateVersion     = evaluation.releaseVersion;
	m_availableUpdateChangelog   = evaluation.changelog;
	m_availableUpdateAssetUrl    = evaluation.asset.url;
	m_availableUpdateAssetName   = evaluation.asset.name;
	m_availableUpdateAssetSha256 = evaluation.asset.sha256;
	if (m_availableUpdateChangelog.trimmed().isEmpty())
	{
		m_availableUpdateChangelog = QStringLiteral("No changelog text was provided for this release.");
	}

	if (evaluation.isSkippedVersion && !manual)
	{
		setUpdateNowActionVisible(false);
		return;
	}

	setUpdateNowActionVisible(true);
	showUpdateAvailableDialog(currentVersion, m_availableUpdateVersion, m_availableUpdateChangelog);
}

void AppController::showUpdateAvailableDialog(const QString &currentVersion, const QString &version,
                                              const QString &changelog)
{
	QWidget *uiOwner = nullptr;
	if (QWidget *activeModal = QApplication::activeModalWidget(); activeModal)
		uiOwner = activeModal;
	else if (m_updateUiParent && m_updateUiParent->isVisible())
		uiOwner = m_updateUiParent.data();
	else
		uiOwner = m_mainWindow;
	if (!uiOwner)
		return;

	if (m_updateAvailableDialog)
		m_updateAvailableDialog->close();

	auto *dialog = new QDialog(uiOwner);
	dialog->setAttribute(Qt::WA_DeleteOnClose, true);
	dialog->setWindowModality(Qt::NonModal);
	dialog->setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	dialog->setWindowTitle(QStringLiteral("QMud Update Available"));
	dialog->setMinimumSize(760, 520);

	auto *layout = new QVBoxLayout(dialog);
	auto *title = new QLabel(QStringLiteral("A newer QMud version (v%1) is available.").arg(version), dialog);
	title->setWordWrap(true);
	layout->addWidget(title);
	layout->addWidget(new QLabel(QStringLiteral("Changelog:"), dialog));

	auto *changelogView = new QPlainTextEdit(dialog);
	changelogView->setReadOnly(true);
	changelogView->setPlainText(changelog);
	changelogView->setFont(qmudPreferredMonospaceFont());
	layout->addWidget(changelogView, 1);

	auto *fullChangelogLink = new QLabel(dialog);
	fullChangelogLink->setTextInteractionFlags(Qt::TextBrowserInteraction);
	fullChangelogLink->setOpenExternalLinks(true);
	fullChangelogLink->setWordWrap(true);
	const QString current = versionCore(currentVersion);
	const QString target  = versionCore(version);
	if (!current.isEmpty() && !target.isEmpty())
	{
		const QString compareUrl =
		    QStringLiteral("https://github.com/Nodens-/QMud/compare/v%1...v%2").arg(current, target);
		fullChangelogLink->setText(QStringLiteral("Full Changelog from v%1: <a href=\"%2\">%2</a>")
		                               .arg(current.toHtmlEscaped(), compareUrl.toHtmlEscaped()));
	}
	else
	{
		fullChangelogLink->setText(QStringLiteral("Full Changelog link unavailable for this release."));
	}
	layout->addWidget(fullChangelogLink);

	auto *skipVersionCheck = new QCheckBox(QStringLiteral("Do not notify me again for this version"), dialog);
	const QString currentSkip =
	    versionCore(getGlobalOption(QStringLiteral("SkipUpdateNotificationVersion")).toString());
	skipVersionCheck->setChecked(!currentSkip.isEmpty() && compareVersions(currentSkip, version) == 0);
	layout->addWidget(skipVersionCheck);

	auto *buttonBox   = new QDialogButtonBox(Qt::Horizontal, dialog);
	auto *updateLater = buttonBox->addButton(QStringLiteral("Update Later"), QDialogButtonBox::RejectRole);
	auto *updateNow   = buttonBox->addButton(QStringLiteral("Update Now"), QDialogButtonBox::AcceptRole);
	layout->addWidget(buttonBox);

	m_updateAvailableDialog = dialog;
	connect(dialog, &QObject::destroyed, this, [this]() { m_updateAvailableDialog = nullptr; });
	connect(dialog, &QDialog::finished, dialog,
	        [this, skipVersionCheck, version](const int result)
	        {
		        if (result == QDialog::Accepted)
			        return;
		        applySkipVersionChoice(version, skipVersionCheck && skipVersionCheck->isChecked());
	        });
	connect(updateLater, &QPushButton::clicked, dialog, [dialog]() { dialog->done(QDialog::Rejected); });
	connect(updateNow, &QPushButton::clicked, dialog,
	        [this, dialog, skipVersionCheck, version]()
	        {
		        applySkipVersionChoice(version, skipVersionCheck && skipVersionCheck->isChecked());
		        handleUpdateQmudNow();
		        dialog->done(QDialog::Accepted);
	        });

	dialog->show();
	dialog->raise();
	dialog->activateWindow();
}

void AppController::setUpdateNowActionVisible(const bool visible) const
{
	if (!m_mainWindow)
		return;
	if (QAction *updateAction = m_mainWindow->actionForCommand(QStringLiteral("UpdateQmudNow")))
	{
		updateAction->setVisible(visible);
		updateAction->setEnabled(visible);
	}
}

void AppController::handleUpdateQmudNow()
{
	if (m_updatePackageDownloadInProgress)
	{
		QWidget *uiOwner = nullptr;
		if (QWidget *activeModal = QApplication::activeModalWidget(); activeModal)
			uiOwner = activeModal;
		else if (m_updateUiParent && m_updateUiParent->isVisible())
			uiOwner = m_updateUiParent.data();
		else
			uiOwner = m_mainWindow;
		QMessageBox::information(uiOwner, QStringLiteral("QMud Update"),
		                         QStringLiteral("An update installation is already in progress."));
		return;
	}

	const QString updateVersion = m_availableUpdateVersion;
	const QString assetUrl      = m_availableUpdateAssetUrl.trimmed();
	const QString assetName     = m_availableUpdateAssetName.trimmed();
	const QString assetSha256   = normalizeSha256Digest(m_availableUpdateAssetSha256);

	QWidget      *uiOwner = nullptr;
	if (QWidget *activeModal = QApplication::activeModalWidget(); activeModal)
		uiOwner = activeModal;
	else if (m_updateUiParent && m_updateUiParent->isVisible())
		uiOwner = m_updateUiParent.data();
	else
		uiOwner = m_mainWindow;

	const UpdateInstallTarget installTarget = detectUpdateInstallTarget();
	if (installTarget == UpdateInstallTarget::Unsupported)
	{
		QMessageBox::information(uiOwner, QStringLiteral("QMud Update"), updateNotSupportedMessage());
		return;
	}
	if (assetUrl.isEmpty() || assetName.isEmpty())
	{
		QMessageBox::warning(uiOwner, QStringLiteral("QMud Update"),
		                     QStringLiteral("No compatible update package is available for this release."));
		return;
	}
	if (assetSha256.isEmpty())
	{
		QMessageBox::warning(
		    uiOwner, QStringLiteral("QMud Update"),
		    QStringLiteral(
		        "No SHA-256 checksum metadata was found for this release asset. Update was cancelled."));
		return;
	}

	if (!m_updateNetworkManager)
		m_updateNetworkManager = new QNetworkAccessManager(this);

	auto stagingDir = std::make_shared<QTemporaryDir>();
	if (!stagingDir->isValid())
	{
		QMessageBox::warning(uiOwner, QStringLiteral("QMud Update"),
		                     QStringLiteral("Unable to create a temporary staging directory."));
		return;
	}

	const QString downloadedPackagePath = QDir(stagingDir->path()).filePath(assetName);
	auto          outputFile            = std::make_shared<QSaveFile>(downloadedPackagePath);
	if (!outputFile->open(QIODevice::WriteOnly | QIODevice::Truncate))
	{
		QMessageBox::warning(
		    uiOwner, QStringLiteral("QMud Update"),
		    QStringLiteral("Unable to write %1: %2").arg(downloadedPackagePath, outputFile->errorString()));
		return;
	}

	QNetworkRequest request{QUrl(assetUrl)};
	request.setRawHeader("User-Agent", "QMud");
	request.setRawHeader("Accept", "application/octet-stream");
	QNetworkReply *reply = m_updateNetworkManager->get(request);
	if (!reply)
	{
		QMessageBox::warning(uiOwner, QStringLiteral("QMud Update"),
		                     QStringLiteral("Failed to start update package download."));
		outputFile->cancelWriting();
		return;
	}

	if (m_updateAvailableDialog)
		m_updateAvailableDialog->close();
	setUpdateNowActionVisible(false);
	m_updatePackageDownloadInProgress = true;

	const QPointer<QWidget> updateUiOwner(uiOwner);
	const auto              resolveUiOwner = [this, updateUiOwner]() -> QWidget *
	{
		if (QWidget *activeModal = QApplication::activeModalWidget(); activeModal)
			return activeModal;
		if (updateUiOwner && updateUiOwner->isVisible())
			return updateUiOwner.data();
		if (m_updateUiParent && m_updateUiParent->isVisible())
			return m_updateUiParent.data();
		return m_mainWindow;
	};
	const auto restoreUpdateActionVisibility = [this]()
	{
		if (!m_availableUpdateVersion.isEmpty() && !m_availableUpdateAssetUrl.trimmed().isEmpty() &&
		    !m_availableUpdateAssetName.trimmed().isEmpty())
		{
			setUpdateNowActionVisible(true);
		}
	};
	const auto failUpdateInstall =
	    [this, resolveUiOwner, restoreUpdateActionVisibility](const QString &message)
	{
		m_updatePackageDownloadInProgress = false;
		restoreUpdateActionVisibility();
		QMessageBox::warning(resolveUiOwner(), QStringLiteral("QMud Update"), message);
	};

	const auto timeoutTimer = std::make_shared<QTimer>();
	timeoutTimer->setSingleShot(true);
	const auto                    timedOut   = std::make_shared<bool>(false);
	const auto                    writeError = std::make_shared<QString>();
	const QPointer<QNetworkReply> replyGuard(reply);
	const auto flushReplyPayload = [replyGuard, outputFile, downloadedPackagePath, writeError]()
	{
		if (!replyGuard || !writeError->isEmpty())
			return;
		const QByteArray chunk = replyGuard->readAll();
		if (chunk.isEmpty())
			return;
		if (outputFile->write(chunk) != chunk.size())
		{
			*writeError = QStringLiteral("Write failed for %1: %2")
			                  .arg(downloadedPackagePath, outputFile->errorString());
			replyGuard->abort();
		}
	};

	connect(reply, &QIODevice::readyRead, this, [flushReplyPayload]() { flushReplyPayload(); });
	connect(timeoutTimer.get(), &QTimer::timeout, this,
	        [replyGuard, timedOut]()
	        {
		        *timedOut = true;
		        if (replyGuard)
			        replyGuard->abort();
	        });
	connect(
	    reply, &QNetworkReply::finished, this,
	    [this, replyGuard, timeoutTimer, timedOut, writeError, flushReplyPayload, outputFile,
	     downloadedPackagePath, assetSha256, updateVersion, installTarget, resolveUiOwner, failUpdateInstall,
	     stagingDir]()
	    {
		    timeoutTimer->stop();
		    flushReplyPayload();
		    if (!replyGuard)
		    {
			    outputFile->cancelWriting();
			    failUpdateInstall(QStringLiteral("Update download reply became unavailable."));
			    return;
		    }

		    const QVariant statusVar  = replyGuard->attribute(QNetworkRequest::HttpStatusCodeAttribute);
		    const int      httpStatus = statusVar.isValid() ? statusVar.toInt() : 0;
		    const QString  networkError =
                replyGuard->error() == QNetworkReply::NoError ? QString() : replyGuard->errorString();
		    replyGuard->deleteLater();

		    if (*timedOut)
		    {
			    outputFile->cancelWriting();
			    failUpdateInstall(QStringLiteral("Download timed out."));
			    return;
		    }
		    if (!writeError->isEmpty())
		    {
			    outputFile->cancelWriting();
			    failUpdateInstall(*writeError);
			    return;
		    }
		    if (!networkError.isEmpty() || httpStatus >= 400)
		    {
			    outputFile->cancelWriting();
			    const QString message =
			        httpStatus > 0
			            ? QStringLiteral("Failed to download the update package "
			                             "(HTTP %1): %2")
			                  .arg(httpStatus)
			                  .arg(networkError)
			            : QStringLiteral("Failed to download the update package: %1").arg(networkError);
			    failUpdateInstall(message);
			    return;
		    }
		    if (!outputFile->commit())
		    {
			    failUpdateInstall(QStringLiteral("Commit failed for %1: %2")
			                          .arg(downloadedPackagePath, outputFile->errorString()));
			    return;
		    }

		    const auto completeSuccessfulInstall =
		        [this, resolveUiOwner, updateVersion, installTarget](const QString &relaunchExecutable)
		    {
			    m_updatePackageDownloadInProgress = false;
			    m_availableUpdateVersion.clear();
			    m_availableUpdateChangelog.clear();
			    m_availableUpdateAssetUrl.clear();
			    m_availableUpdateAssetName.clear();
			    m_availableUpdateAssetSha256.clear();
			    setUpdateNowActionVisible(false);

			    const auto ensureWorldStatePreparedBeforeRestart = [this, resolveUiOwner]() -> bool
			    {
				    QString restartSaveError;
				    if (!saveDirtyAutoSaveWorldsBeforeRestart(&restartSaveError))
				    {
					    QMessageBox::warning(
					        resolveUiOwner(), QStringLiteral("QMud Update"),
					        QStringLiteral(
					            "Update installed, but failed to save dirty worlds before restart.\n%1\n\n"
					            "Restart was cancelled; save the affected world(s) and restart QMud "
					            "manually.")
					            .arg(restartSaveError));
					    return false;
				    }
				    QString restartLogCloseError;
				    if (!closeOpenWorldLogsBeforeRestart(&restartLogCloseError))
				    {
					    QMessageBox::warning(
					        resolveUiOwner(), QStringLiteral("QMud Update"),
					        QStringLiteral("Update installed, but failed to close active world logs before "
					                       "restart.\n%1\n\nRestart was cancelled; close logs manually and "
					                       "restart QMud manually.")
					            .arg(restartLogCloseError));
					    return false;
				    }
				    QString restartSessionStateError;
				    if (!saveOpenWorldSessionStatesBeforeRestart(&restartSessionStateError))
				    {
					    QMessageBox::warning(
					        resolveUiOwner(), QStringLiteral("QMud Update"),
					        QStringLiteral("Update installed, but failed to persist world session state "
					                       "before restart.\n%1\n\nRestart was cancelled; restart QMud "
					                       "manually.")
					            .arg(restartSessionStateError));
					    return false;
				    }
				    return true;
			    };

#ifndef Q_OS_WIN
			    Q_UNUSED(installTarget);
#endif

#ifdef Q_OS_WIN
			    if (installTarget == UpdateInstallTarget::WindowsInstaller)
			    {
				    if (!ensureWorldStatePreparedBeforeRestart())
					    return;

				    const QString installerPath = relaunchExecutable.trimmed();
				    if (installerPath.isEmpty())
				    {
					    QMessageBox::warning(resolveUiOwner(), QStringLiteral("QMud Update"),
					                         QStringLiteral("Update installer path is unavailable."));
					    return;
				    }

				    const QString targetDir = QDir::toNativeSeparators(
				        QFileInfo(QCoreApplication::applicationFilePath()).absolutePath());
				    const QString runProcess =
				        QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
				    const QString waitPid =
				        QString::number(static_cast<qint64>(QCoreApplication::applicationPid()));
				    const QStringList installerArgs = {QStringLiteral("/S"),
				                                       QStringLiteral("/WAITPID=%1").arg(waitPid),
				                                       QStringLiteral("/TARGETDIR=%1").arg(targetDir),
				                                       QStringLiteral("/RUNPROCESS=%1").arg(runProcess)};

				    if (!QProcess::startDetached(installerPath, installerArgs))
				    {
					    QMessageBox::warning(resolveUiOwner(), QStringLiteral("QMud Update"),
					                         QStringLiteral("Update installer failed to start."));
					    return;
				    }

				    QCoreApplication::quit();
				    return;
			    }
#endif

			    const bool reloadEnabled =
			        getGlobalOption(QStringLiteral("EnableReloadFeature")).toInt() != 0;
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
			    if (reloadEnabled)
			    {
				    if (QWidget *updateParent = m_updateUiParent.data();
				        updateParent && updateParent != m_mainWindow)
				    {
					    if (auto *dialog = qobject_cast<QDialog *>(updateParent))
						    dialog->close();
					    else
						    updateParent->close();
					    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
				    }
				    for (int attempt = 0; attempt < 4; ++attempt)
				    {
					    QWidget *activeModal = QApplication::activeModalWidget();
					    if (!activeModal || activeModal == m_mainWindow)
						    break;
					    if (auto *dialog = qobject_cast<QDialog *>(activeModal))
						    dialog->close();
					    else
						    activeModal->close();
					    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
				    }

				    m_reloadTargetExecutableOverride      = relaunchExecutable;
				    const QByteArray previousReloadAssume = qgetenv("QMUD_RELOAD_ASSUME_YES");
				    qputenv("QMUD_RELOAD_ASSUME_YES", QByteArrayLiteral("1"));
				    handleReloadQmud();
				    if (previousReloadAssume.isNull())
					    qunsetenv("QMUD_RELOAD_ASSUME_YES");
				    else
					    qputenv("QMUD_RELOAD_ASSUME_YES", previousReloadAssume);
				    if (!m_reloadInProgress)
				    {
					    if (!ensureWorldStatePreparedBeforeRestart())
						    return;
					    m_reloadTargetExecutableOverride.clear();
					    QStringList relaunchArgumentsFallback = QCoreApplication::arguments();
					    if (!relaunchArgumentsFallback.isEmpty())
						    relaunchArgumentsFallback.removeFirst();
					    if (!QProcess::startDetached(relaunchExecutable, relaunchArgumentsFallback))
					    {
						    QMessageBox::warning(
						        resolveUiOwner(), QStringLiteral("QMud Update"),
						        QStringLiteral("Reload failed and automatic restart also failed.\nPlease "
						                       "start QMud manually."));
						    return;
					    }
					    QCoreApplication::quit();
					    return;
				    }
				    return;
			    }
#else
			    Q_UNUSED(reloadEnabled);
#endif

			    if (!ensureWorldStatePreparedBeforeRestart())
				    return;

			    QStringList relaunchArguments = QCoreApplication::arguments();
			    if (!relaunchArguments.isEmpty())
				    relaunchArguments.removeFirst();
			    if (!QProcess::startDetached(relaunchExecutable, relaunchArguments))
			    {
				    QMessageBox::warning(resolveUiOwner(), QStringLiteral("QMud Update"),
				                         QStringLiteral("Update installed but failed to restart "
				                                        "automatically.\nPlease start QMud manually."));
				    return;
			    }

			    QMessageBox::information(
			        resolveUiOwner(), QStringLiteral("QMud Update"),
			        QStringLiteral("QMud v%1 was updated successfully and will now restart.")
			            .arg(updateVersion.isEmpty() ? QStringLiteral("?") : updateVersion));
			    QCoreApplication::quit();
		    };

		    const QPointer<AppController> controllerGuard(this);
		    QThreadPool::globalInstance()->start(
		        [controllerGuard, downloadedPackagePath, assetSha256, installTarget, stagingDir,
		         failUpdateInstall, completeSuccessfulInstall]()
		        {
			        QString downloadedSha256;
			        QString hashError;
			        QString installError;
			        QString relaunchExecutable;

			        if (!computeFileSha256(downloadedPackagePath, &downloadedSha256, &hashError))
			        {
				        installError =
				            QStringLiteral("Failed to verify update package checksum:\n%1").arg(hashError);
			        }
			        else if (normalizeSha256Digest(downloadedSha256) != assetSha256)
			        {
				        installError =
				            QStringLiteral("Update package checksum mismatch.\nExpected: %1\nDownloaded: %2")
				                .arg(assetSha256, downloadedSha256);
			        }
			        else
			        {
				        if (installTarget == UpdateInstallTarget::Unsupported)
				        {
					        installError = updateNotSupportedMessage();
				        }
				        else if (installTarget == UpdateInstallTarget::WindowsInstaller)
				        {
					        relaunchExecutable = downloadedPackagePath;
				        }
				        else if (installTarget == UpdateInstallTarget::LinuxAppImage)
				        {
					        const QString appImagePath = qEnvironmentVariable("APPIMAGE").trimmed();
					        if (appImagePath.isEmpty())
					        {
						        installError = QStringLiteral("APPIMAGE environment path is missing.");
					        }
					        else
					        {
						        QFileDevice::Permissions appImagePermissions =
						            QFile::permissions(appImagePath);
						        if (appImagePermissions == QFileDevice::Permissions())
						        {
							        appImagePermissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner |
							                              QFileDevice::ExeOwner | QFileDevice::ReadGroup |
							                              QFileDevice::ExeGroup | QFileDevice::ReadOther |
							                              QFileDevice::ExeOther;
						        }
						        else
						        {
							        appImagePermissions |=
							            QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther;
						        }

						        if (!replaceFileWithDownloadedPayload(downloadedPackagePath, appImagePath,
						                                              appImagePermissions, &installError))
						        {
							        installError =
							            QStringLiteral("Failed to update AppImage:\n%1").arg(installError);
						        }
						        else
						        {
							        relaunchExecutable = appImagePath;
						        }
					        }
				        }
				        else if (installTarget == UpdateInstallTarget::MacBundle)
				        {
					        const QString unzipPath = QStandardPaths::findExecutable(QStringLiteral("unzip"));
					        if (unzipPath.isEmpty())
					        {
						        installError = QStringLiteral("Required tool 'unzip' is unavailable.");
					        }
					        else
					        {
						        const QString extractRoot =
						            QDir(stagingDir->path()).filePath(QStringLiteral("extract"));
						        if (!QDir().mkpath(extractRoot))
						        {
							        installError =
							            QStringLiteral("Unable to create temporary extraction directory: %1")
							                .arg(extractRoot);
						        }
						        else if (!runUpdateHelperCommand(unzipPath,
						                                         {QStringLiteral("-oq"),
						                                          downloadedPackagePath, QStringLiteral("-d"),
						                                          extractRoot},
						                                         5 * 60 * 1000, &installError))
						        {
							        installError = QStringLiteral("Failed to extract update archive:\n%1")
							                           .arg(installError);
						        }
						        else
						        {
							        const QString extractedBundlePath =
							            findExtractedAppBundlePath(extractRoot);
							        const QString currentBundlePath = findContainingAppBundlePath();
							        if (extractedBundlePath.isEmpty())
							        {
								        installError = QStringLiteral(
								            "Extracted archive did not contain a .app bundle.");
							        }
							        else if (currentBundlePath.isEmpty())
							        {
								        installError =
								            QStringLiteral("Unable to locate current QMud.app bundle path.");
							        }
							        else
							        {
								        const QString dittoPath =
								            QStandardPaths::findExecutable(QStringLiteral("ditto"));
								        if (dittoPath.isEmpty())
								        {
									        installError =
									            QStringLiteral("Required tool 'ditto' is unavailable.");
								        }
								        else
								        {
									        const QFileInfo currentBundleInfo(currentBundlePath);
									        const QString bundleParentDir = currentBundleInfo.absolutePath();
									        const QString stagedBundlePath =
									            QDir(bundleParentDir)
									                .filePath(QStringLiteral(".%1.update.staged")
									                              .arg(currentBundleInfo.fileName()));
									        const QString backupBundlePath =
									            QDir(bundleParentDir)
									                .filePath(QStringLiteral(".%1.update.backup")
									                              .arg(currentBundleInfo.fileName()));
									        QString cleanupError;
									        if (!removePathIfExists(stagedBundlePath, &cleanupError) ||
									            !removePathIfExists(backupBundlePath, &cleanupError))
									        {
										        installError =
										            QStringLiteral(
										                "Failed to prepare bundle staging paths:\n%1")
										                .arg(cleanupError);
									        }
									        else if (!runUpdateHelperCommand(
									                     dittoPath, {extractedBundlePath, stagedBundlePath},
									                     5 * 60 * 1000, &installError))
									        {
										        installError =
										            QStringLiteral("Failed to stage updated app bundle:\n%1")
										                .arg(installError);
									        }
									        else if (!QDir().rename(currentBundlePath, backupBundlePath))
									        {
										        installError = QStringLiteral(
										            "Failed to move current app bundle aside.");
									        }
									        else if (!QDir().rename(stagedBundlePath, currentBundlePath))
									        {
										        installError =
										            QStringLiteral("Failed to place updated app bundle.");
										        (void)QDir().rename(backupBundlePath, currentBundlePath);
									        }
									        else
									        {
										        (void)removePathIfExists(backupBundlePath, &cleanupError);
										        const QString binaryName =
										            QFileInfo(QCoreApplication::applicationFilePath())
										                .fileName();
										        relaunchExecutable =
										            QDir(currentBundlePath)
										                .filePath(QStringLiteral("Contents/MacOS/%1")
										                              .arg(binaryName));
									        }
								        }
							        }
						        }
					        }
				        }
			        }

			        if (installError.isEmpty() && relaunchExecutable.trimmed().isEmpty())
			        {
				        installError =
				            QStringLiteral("Update was applied but relaunch target is unavailable.");
			        }

			        QMetaObject::invokeMethod(
			            qApp,
			            [controllerGuard, installError, failUpdateInstall, relaunchExecutable, stagingDir,
			             completeSuccessfulInstall]()
			            {
				            Q_UNUSED(stagingDir);
				            if (!controllerGuard)
					            return;
				            if (!installError.isEmpty())
				            {
					            failUpdateInstall(installError);
					            return;
				            }
				            completeSuccessfulInstall(relaunchExecutable);
			            },
			            Qt::QueuedConnection);
		        });
	    });
	timeoutTimer->start(5 * 60 * 1000);
}

void AppController::applySkipVersionChoice(const QString &version, const bool skipWhenTrue)
{
	const QString normalizedVersion = versionCore(version);
	if (normalizedVersion.isEmpty())
		return;

	const QString currentSkip =
	    versionCore(getGlobalOption(QStringLiteral("SkipUpdateNotificationVersion")).toString());
	if (skipWhenTrue)
	{
		setGlobalOptionString(QStringLiteral("SkipUpdateNotificationVersion"), normalizedVersion);
	}
	else if (!currentSkip.isEmpty() && compareVersions(currentSkip, normalizedVersion) == 0)
	{
		setGlobalOptionString(QStringLiteral("SkipUpdateNotificationVersion"), QString());
	}

	if (!m_availableUpdateVersion.isEmpty() &&
	    compareVersions(versionCore(m_availableUpdateVersion), normalizedVersion) == 0)
	{
		setUpdateNowActionVisible(!skipWhenTrue);
	}
}

void AppController::reloadGlobalPreferencesForLua()
{
	loadGlobalsFromDatabase();
	applyGlobalPreferences();
}

void AppController::restoreWindowPlacement()
{
	// Qt-native main window geometry restore.
	if (!m_mainWindow)
		return;

	QSettings settings(iniFilePath(), QSettings::IniFormat);
	settings.beginGroup(QStringLiteral("MainWindow"));
	const QByteArray geometry       = settings.value(QStringLiteral("Geometry")).toByteArray();
	const QRect      normalGeometry = settings.value(QStringLiteral("NormalGeometry")).toRect();
	const bool       maximized      = settings.value(QStringLiteral("Maximized"), false).toBool();
	settings.endGroup();

	if (!geometry.isEmpty())
		m_mainWindow->restoreGeometry(geometry);

	if (maximized)
	{
		// Keep this Qt-native: restore geometry blob, then apply maximized state.
		// Avoid forcing normal/maximized transitions here (they can poison restore target state).
		if (normalGeometry.isValid() && !normalGeometry.isNull())
		{
			// Seed the non-maximized rectangle before maximizing so window-manager
			// Restore has a concrete target when starting already maximized.
			m_mainWindow->setGeometry(normalGeometry);
			m_mainWindow->setLastNormalGeometry(normalGeometry);
		}
		else if (m_mainWindow->geometry().isValid() && !m_mainWindow->geometry().isNull())
			m_mainWindow->setLastNormalGeometry(m_mainWindow->geometry());
		if (m_deferMainWindowShowUntilSplash)
			m_showMainWindowMaximizedAfterSplash = true;
		else
			m_mainWindow->showMaximized();
	}
	else
	{
		if (!m_deferMainWindowShowUntilSplash)
			m_mainWindow->show();
		if (m_mainWindow->geometry().isValid() && !m_mainWindow->geometry().isNull())
			m_mainWindow->setLastNormalGeometry(m_mainWindow->geometry());
	}
}

void AppController::setupRecentFiles() const
{
	// Initialize MRU list defaults.
	if (!m_mainWindow)
		return;

	QSettings settings(iniFilePath(), QSettings::IniFormat);
	settings.beginGroup(QStringLiteral("Recent File List"));
	QStringList files;
	QStringList fileKeys;
	bool        changed = false;
	for (int i = 1; i <= 4; ++i)
	{
		const QString key = QStringLiteral("File%1").arg(i);
		if (const QString value = settings.value(key).toString(); !value.isEmpty())
		{
			const QString migrated = migrateLegacyIniPathValue(m_workingDir, key, value);
			if (isLikelyCorruptedRelativeWorldPath(migrated))
			{
				settings.setValue(key, QString());
				changed = true;
				continue;
			}
			if (migrated != value)
			{
				settings.setValue(key, migrated);
				changed = true;
			}
			const QString stored     = preferredMruStoragePath(migrated, m_workingDir);
			const QString compareKey = mruComparisonKey(stored, m_workingDir);
			if (compareKey.isEmpty())
				continue;
			if (fileKeys.contains(compareKey))
			{
				changed = true;
				continue;
			}
			if (stored != migrated)
				changed = true;
			files.push_back(stored);
			fileKeys.push_back(compareKey);
		}
	}
	while (files.size() > 4)
	{
		files.removeLast();
		fileKeys.removeLast();
		changed = true;
	}
	for (int i = 1; i <= 4; ++i)
	{
		const QString key = QStringLiteral("File%1").arg(i);
		if (const QString wanted = i <= files.size() ? files[i - 1] : QString();
		    settings.value(key).toString() != wanted)
		{
			settings.setValue(key, wanted);
			changed = true;
		}
	}
	settings.endGroup();
	if (changed)
		settings.sync();

	m_mainWindow->setRecentFiles(files);
}

void AppController::loadPrintSetupPreferences()
{
	QSettings settings(iniFilePath(), QSettings::IniFormat);
	settings.beginGroup(QStringLiteral("PrintSetup"));
	m_hasPrintSetup         = settings.value(QStringLiteral("HasSetup"), false).toBool();
	m_printSetupPrinterName = settings.value(QStringLiteral("PrinterName")).toString();

	QPageLayout layout;
	if (settings.contains(QStringLiteral("PageLayout/PageSizeId")))
	{
		const auto pageSizeId = static_cast<QPageSize::PageSizeId>(
		    settings.value(QStringLiteral("PageLayout/PageSizeId")).toInt());
		const auto orientation = static_cast<QPageLayout::Orientation>(
		    settings.value(QStringLiteral("PageLayout/Orientation"), QPageLayout::Portrait).toInt());
		const auto units = static_cast<QPageLayout::Unit>(
		    settings.value(QStringLiteral("PageLayout/Units"), QPageLayout::Point).toInt());
		const auto mode = static_cast<QPageLayout::Mode>(
		    settings.value(QStringLiteral("PageLayout/Mode"), QPageLayout::StandardMode).toInt());
		const QMarginsF margins(settings.value(QStringLiteral("PageLayout/LeftMargin"), 0.0).toDouble(),
		                        settings.value(QStringLiteral("PageLayout/TopMargin"), 0.0).toDouble(),
		                        settings.value(QStringLiteral("PageLayout/RightMargin"), 0.0).toDouble(),
		                        settings.value(QStringLiteral("PageLayout/BottomMargin"), 0.0).toDouble());

		QPageLayout     loadedLayout;
		loadedLayout.setPageSize(QPageSize(pageSizeId));
		loadedLayout.setOrientation(orientation);
		loadedLayout.setUnits(units);
		loadedLayout.setMode(mode);
		loadedLayout.setMargins(margins);
		if (loadedLayout.isValid())
			layout = loadedLayout;
	}

	// QVariant-based page-layout loading can trigger clangd meta-type diagnostics on some toolchains.
	// Runtime persistence now uses explicit scalar keys written by savePrintSetupPreferences().

	if (layout.isValid())
	{
		m_printSetupLayout = layout;
	}
	settings.endGroup();
}

void AppController::savePrintSetupPreferences() const
{
	QSettings settings(iniFilePath(), QSettings::IniFormat);
	settings.beginGroup(QStringLiteral("PrintSetup"));
	settings.setValue(QStringLiteral("HasSetup"), m_hasPrintSetup);
	settings.setValue(QStringLiteral("PrinterName"), m_printSetupPrinterName);
	settings.remove(QStringLiteral("PageLayout"));
	if (m_printSetupLayout.isValid())
	{
		const QMarginsF margins = m_printSetupLayout.margins();
		settings.setValue(QStringLiteral("PageLayout/PageSizeId"), m_printSetupLayout.pageSize().id());
		settings.setValue(QStringLiteral("PageLayout/Orientation"), m_printSetupLayout.orientation());
		settings.setValue(QStringLiteral("PageLayout/Units"), m_printSetupLayout.units());
		settings.setValue(QStringLiteral("PageLayout/Mode"), m_printSetupLayout.mode());
		settings.setValue(QStringLiteral("PageLayout/LeftMargin"), margins.left());
		settings.setValue(QStringLiteral("PageLayout/TopMargin"), margins.top());
		settings.setValue(QStringLiteral("PageLayout/RightMargin"), margins.right());
		settings.setValue(QStringLiteral("PageLayout/BottomMargin"), margins.bottom());
	}
	else
		settings.remove(QStringLiteral("PageLayout"));
	settings.endGroup();
}

void AppController::applyConnectionPreferences() const
{
	const bool reconnect = getGlobalOption(QStringLiteral("ReconnectOnLinkFailure")).toInt() != 0;
	for (const auto runtimes = activeWorldRuntimes(); WorldRuntime *runtime : runtimes)
	{
		if (runtime)
			runtime->setReconnectOnLinkFailure(reconnect);
	}
}

void AppController::applySpellCheckPreferences()
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	if (const auto enableSpellCheck = getGlobalOption(QStringLiteral("EnableSpellCheck")).toInt();
	    !enableSpellCheck)
	{
		closeSpellChecker();
		return;
	}
	ensureSpellCheckerLoaded();
#endif
}

static QString convertToRegularExpression(const QString &input, const bool wholeLine,
                                          const bool makeAsterisksWildcards)
{
	QString out;
	if (wholeLine)
		out.append(QLatin1Char('^'));

	for (QChar ch : input)
	{
		if (ch == QLatin1Char('\n'))
		{
			out.append(QStringLiteral("\\n"));
			continue;
		}
		if (ch.unicode() < 0x20)
		{
			out.append(QStringLiteral("\\x"));
			out.append(QStringLiteral("%1").arg(static_cast<int>(ch.unicode()), 2, 16, QLatin1Char('0')));
			continue;
		}
		if (ch.isLetterOrNumber() || ch == QLatin1Char(' ') || ch.unicode() >= 0x80)
		{
			out.append(ch);
			continue;
		}
		if (ch == QLatin1Char('*') && makeAsterisksWildcards)
		{
			out.append(QStringLiteral("(.*?)"));
			continue;
		}
		out.append(QLatin1Char('\\'));
		out.append(ch);
	}

	if (wholeLine)
		out.append(QLatin1Char('$'));
	return out;
}

#ifdef QMUD_ENABLE_LUA_SCRIPTING
static QString findSpellCheckerPath(const QString &baseDir)
{
	if (baseDir.isEmpty())
		return {};
	const QDir root(QDir::cleanPath(baseDir));
	if (const QString primary = root.filePath(QStringLiteral("spell/spellchecker.lua"));
	    QFile::exists(primary))
		return QDir::cleanPath(primary);
	if (const QString fallback = root.filePath(QStringLiteral("spellchecker.lua")); QFile::exists(fallback))
		return QDir::cleanPath(fallback);
	return {};
}

namespace
{
	constexpr auto kSpellProgressMetaName = "qmud.spell_progress_dialog";

	struct SpellProgressDialog
	{
			QPointer<QProgressDialog> dialog;
			int                       step{1};
	};

	SpellProgressDialog *spellProgressCheck(lua_State *L)
	{
		auto **ud = static_cast<SpellProgressDialog **>(luaL_checkudata(L, 1, kSpellProgressMetaName));
		if (!ud || !*ud)
		{
			luaL_argerror(L, 1, "progress dialog userdata expected");
			return nullptr;
		}
		return *ud;
	}

	int luaSpellProgressGc(lua_State *L)
	{
		auto **ud = static_cast<SpellProgressDialog **>(luaL_checkudata(L, 1, kSpellProgressMetaName));
		if (!ud || !*ud)
			return 0;

		SpellProgressDialog *p = *ud;
		if (p->dialog)
		{
			p->dialog->close();
			delete p->dialog;
			p->dialog = nullptr;
		}
		delete p;
		*ud = nullptr;
		return 0;
	}

	int luaSpellProgressNew(lua_State *L)
	{
		const char          *status     = luaL_optstring(L, 1, "");
		const AppController *controller = AppController::instance();
		QWidget             *parent = controller ? static_cast<QWidget *>(controller->mainWindow()) : nullptr;

		auto                *p = new SpellProgressDialog;
		p->dialog = new QProgressDialog(QString::fromUtf8(status), QStringLiteral("Cancel"), 0, 100, parent);
		p->dialog->setWindowTitle(QStringLiteral("QMud"));
		p->dialog->setAutoClose(false);
		p->dialog->setAutoReset(false);
		p->dialog->setMinimumDuration(0);
		p->dialog->setWindowModality(Qt::WindowModal);
		p->dialog->show();
		QCoreApplication::processEvents();

		auto **ud = static_cast<SpellProgressDialog **>(lua_newuserdata(L, sizeof(SpellProgressDialog *)));
		*ud       = p;
		luaL_getmetatable(L, kSpellProgressMetaName);
		lua_setmetatable(L, -2);
		return 1;
	}

	int luaSpellProgressSetStatus(lua_State *L)
	{
		const SpellProgressDialog *p      = spellProgressCheck(L);
		const char                *status = luaL_checkstring(L, 2);
		if (p->dialog)
		{
			p->dialog->setLabelText(QString::fromUtf8(status));
			QCoreApplication::processEvents();
		}
		return 0;
	}

	int luaSpellProgressSetRange(lua_State *L)
	{
		const SpellProgressDialog *p     = spellProgressCheck(L);
		const int                  start = static_cast<int>(luaL_checkinteger(L, 2));
		const int                  end   = static_cast<int>(luaL_checkinteger(L, 3));
		if (p->dialog)
		{
			p->dialog->setRange(start, end);
			QCoreApplication::processEvents();
		}
		return 0;
	}

	int luaSpellProgressSetPosition(lua_State *L)
	{
		const SpellProgressDialog *p   = spellProgressCheck(L);
		const int                  pos = static_cast<int>(luaL_checkinteger(L, 2));
		if (p->dialog)
		{
			p->dialog->setValue(pos);
			QCoreApplication::processEvents();
		}
		return 0;
	}

	int luaSpellProgressSetStep(lua_State *L)
	{
		SpellProgressDialog *p = spellProgressCheck(L);
		p->step                = static_cast<int>(luaL_checkinteger(L, 2));
		if (p->step <= 0)
			p->step = 1;
		return 0;
	}

	int luaSpellProgressStep(lua_State *L)
	{
		if (const SpellProgressDialog *p = spellProgressCheck(L); p->dialog)
		{
			p->dialog->setValue(p->dialog->value() + p->step);
			QCoreApplication::processEvents();
		}
		return 0;
	}

	int luaSpellProgressCheckCancel(lua_State *L)
	{
		const SpellProgressDialog *p = spellProgressCheck(L);
		lua_pushboolean(L, p->dialog && p->dialog->wasCanceled() ? 1 : 0);
		return 1;
	}

	void registerSpellProgressLibrary(lua_State *L)
	{
		if (!L)
			return;

		luaL_newmetatable(L, kSpellProgressMetaName);
		lua_pushvalue(L, -1);
		lua_setfield(L, -2, "__index");
		lua_pushcfunction(L, luaSpellProgressGc);
		lua_setfield(L, -2, "__gc");
		lua_pushcfunction(L, luaSpellProgressGc);
		lua_setfield(L, -2, "close");
		lua_pushcfunction(L, luaSpellProgressSetStatus);
		lua_setfield(L, -2, "status");
		lua_pushcfunction(L, luaSpellProgressSetRange);
		lua_setfield(L, -2, "range");
		lua_pushcfunction(L, luaSpellProgressSetPosition);
		lua_setfield(L, -2, "position");
		lua_pushcfunction(L, luaSpellProgressSetStep);
		lua_setfield(L, -2, "setstep");
		lua_pushcfunction(L, luaSpellProgressStep);
		lua_setfield(L, -2, "step");
		lua_pushcfunction(L, luaSpellProgressCheckCancel);
		lua_setfield(L, -2, "checkcancel");
		lua_pop(L, 1);

		lua_newtable(L);
		lua_pushcfunction(L, luaSpellProgressNew);
		lua_setfield(L, -2, "new");
		lua_setglobal(L, "progress");
	}
} // namespace

static int luaSpellCheckDialog(lua_State *L)
{
	const char *word = luaL_checkstring(L, 1);
	QStringList suggestions;

	if (lua_gettop(L) >= 2 && !lua_isnil(L, 2))
	{
		if (!lua_istable(L, 2))
			luaL_error(L, "argument 2 must be a table of suggestions, or nil");
		for (int i = 1;; ++i)
		{
			lua_rawgeti(L, 2, i);
			if (lua_isnil(L, -1))
			{
				lua_pop(L, 1);
				break;
			}
			if (lua_isstring(L, -1))
				suggestions.push_back(QString::fromUtf8(lua_tostring(L, -1)));
			lua_pop(L, 1);
		}
	}

	const AppController *controller = AppController::instance();
	MainWindow          *main       = controller ? controller->mainWindow() : nullptr;
	SpellCheckDialog     dlg(QString::fromUtf8(word), suggestions, main);
	if (dlg.exec() != QDialog::Accepted)
	{
		lua_pushnil(L);
		return 1;
	}

	const QString action = dlg.action();
	if (action.isEmpty())
	{
		lua_pushnil(L);
		return 1;
	}
	const QByteArray actionBytes = action.toUtf8();
	const QByteArray replBytes   = dlg.replacement().toUtf8();
	lua_pushlstring(L, actionBytes.constData(), actionBytes.size());
	lua_pushlstring(L, replBytes.constData(), replBytes.size());
	return 2;
}

static void installSpellPathCompat(lua_State *L)
{
	if (!L)
		return;

	static const auto *kPathCompatScript = R"lua(
local dirsep = package and package.config and package.config:sub(1, 1) or "/"
if dirsep ~= "\\" then
  local function normalize_path(path)
    if type(path) ~= "string" then
      return path
    end
    local out = path:gsub("\\", "/")
    out = out:gsub("/+", "/")
    return out
  end

  local io_open = io.open
  if type(io_open) == "function" then
    io.open = function(filename, mode)
      return io_open(normalize_path(filename), mode)
    end
  end

  local io_lines = io.lines
  if type(io_lines) == "function" then
    io.lines = function(filename, ...)
      if filename == nil then
        return io_lines(nil, ...)
      end
      return io_lines(normalize_path(filename), ...)
    end
  end

  local io_output = io.output
  if type(io_output) == "function" then
    io.output = function(file)
      if file == nil then
        return io_output()
      end
      return io_output(normalize_path(file))
    end
  end

  local io_input = io.input
  if type(io_input) == "function" then
    io.input = function(file)
      if file == nil then
        return io_input()
      end
      return io_input(normalize_path(file))
    end
  end

  if type(sqlite3) == "table" and type(sqlite3.open) == "function" then
    local sqlite_open = sqlite3.open
    sqlite3.open = function(filename, ...)
      return sqlite_open(normalize_path(filename), ...)
    end
  end
end
)lua";

	if (luaL_dostring(L, kPathCompatScript) != 0)
	{
		const char *err = lua_tostring(L, -1);
		qWarning() << "Failed to install spell path normalization mapping:" << (err ? err : "<unknown>");
		lua_pop(L, 1);
	}
}

static QString ensureTrailingSeparator(const QString &path)
{
	if (path.isEmpty())
		return path;
	QString out = QDir::cleanPath(path);
#ifdef Q_OS_WIN
	out                          = QDir::toNativeSeparators(out);
	constexpr QChar preferredSep = QLatin1Char('\\');
#else
	out.replace(QLatin1Char('\\'), QLatin1Char('/'));
	while (out.contains(QStringLiteral("//")))
		out.replace(QStringLiteral("//"), QStringLiteral("/"));
	constexpr QChar preferredSep = QLatin1Char('/');
#endif
	if (!out.endsWith('/') && !out.endsWith('\\'))
		out += preferredSep;
	return out;
}

static int luaUtilsInfoQt(lua_State *L)
{
	lua_newtable(L);

	const AppController *app    = AppController::instance();
	QString              appDir = ensureTrailingSeparator(QCoreApplication::applicationDirPath());
	if (app)
	{
		if (const QString startupDir = ensureTrailingSeparator(app->makeAbsolutePath(QStringLiteral(".")));
		    !startupDir.isEmpty())
		{
			appDir = startupDir;
		}
	}
	auto setString = [L](const char *key, const QString &value)
	{
		const QByteArray bytes = value.toUtf8();
		lua_pushlstring(L, bytes.constData(), bytes.size());
		lua_setfield(L, -2, key);
	};
	auto setNumber = [L](const char *key, const double value)
	{
		lua_pushnumber(L, value);
		lua_setfield(L, -2, key);
	};

	setString("app_directory", appDir);

	const QString currentDir = ensureTrailingSeparator(QDir::currentPath());
	setString("current_directory", currentDir);

	if (app)
	{
		const QString worldsDir = ensureTrailingSeparator(app->makeAbsolutePath(
		    app->getGlobalOption(QStringLiteral("DefaultWorldFileDirectory")).toString()));
		const QString stateDir  = ensureTrailingSeparator(
            app->makeAbsolutePath(app->getGlobalOption(QStringLiteral("StateFilesDirectory")).toString()));
		const QString logDir     = ensureTrailingSeparator(app->makeAbsolutePath(
            app->getGlobalOption(QStringLiteral("DefaultLogFileDirectory")).toString()));
		const QString pluginsDir = ensureTrailingSeparator(
		    app->makeAbsolutePath(app->getGlobalOption(QStringLiteral("PluginsDirectory")).toString()));

		if (!worldsDir.isEmpty())
			setString("world_files_directory", worldsDir);
		if (!stateDir.isEmpty())
			setString("state_files_directory", stateDir);
		if (!logDir.isEmpty())
			setString("log_files_directory", logDir);
		if (!pluginsDir.isEmpty())
			setString("plugins_directory", pluginsDir);

		setString("locale", app->getGlobalOption(QStringLiteral("Locale")).toString());
		setString("fixed_pitch_font", app->getGlobalOption(QStringLiteral("FixedPitchFont")).toString());
		setNumber("fixed_pitch_font_size",
		          app->getGlobalOption(QStringLiteral("FixedPitchFontSize")).toDouble());
		setString("startup_directory", ensureTrailingSeparator(app->makeAbsolutePath(QStringLiteral("."))));
	}

	return 1;
}

bool AppController::ensureSpellCheckerLoaded()
{
	if (const int enableSpellCheck = getGlobalOption(QStringLiteral("EnableSpellCheck")).toInt();
	    !enableSpellCheck)
	{
		return false;
	}
	if (m_spellCheckerLua)
		return m_spellCheckOk;

	LuaStateOwner state(QMudLuaSupport::makeLuaState());
	if (!state)
		return false;

	luaL_openlibs(state.get());
	QMudLuaSupport::applyLua51Compat(state.get());
	qmudLogLua51CompatState(state.get(), "AppController spellchecker");
	QMudLuaSupport::callLuaCFunction(state.get(), luaopen_lsqlite3);
	installSpellPathCompat(state.get());
	registerSpellProgressLibrary(state.get());
	lua_getglobal(state.get(), "utils");
	if (!lua_istable(state.get(), -1))
	{
		lua_pop(state.get(), 1);
		lua_newtable(state.get());
		lua_setglobal(state.get(), "utils");
		lua_getglobal(state.get(), "utils");
	}
	if (lua_istable(state.get(), -1))
	{
		lua_pushcfunction(state.get(), luaUtilsInfoQt);
		lua_setfield(state.get(), -2, "info");
		lua_pushcfunction(state.get(), luaSpellCheckDialog);
		lua_setfield(state.get(), -2, "spellcheckdialog");
	}
	lua_pop(state.get(), 1);

	const bool enablePackage = getGlobalOption(QStringLiteral("AllowLoadingDlls")).toInt() != 0;
	QMudLuaSupport::applyLuaSecurityRestrictions(state.get(), enablePackage);

	QString spellPath = findSpellCheckerPath(m_workingDir);
	if (spellPath.isEmpty())
		spellPath = findSpellCheckerPath(QCoreApplication::applicationDirPath());

	if (spellPath.isEmpty())
		return false;

	if (const QByteArray pathBytes = spellPath.toUtf8();
	    luaL_loadfile(state.get(), pathBytes.constData()) || lua_pcall(state.get(), 0, 0, 0))
	{
		QMudLuaSupport::luaError(state.get(), "Spellcheck initialization");
		return false;
	}

	lua_getglobal(state.get(), "spellcheck");
	if (!lua_isfunction(state.get(), -1))
	{
		lua_pop(state.get(), 1);
		return false;
	}
	lua_pop(state.get(), 1);

	m_spellCheckerLua = state.release();
	m_spellCheckOk    = true;
	return true;
}

lua_State *AppController::spellCheckerLuaState() const
{
	return m_spellCheckerLua;
}

void AppController::closeSpellChecker()
{
	if (m_spellCheckerLua)
	{
		lua_close(m_spellCheckerLua);
		m_spellCheckerLua = nullptr;
	}
	m_spellCheckOk = false;
}
#endif

void AppController::applyPluginPreferences()
{
	m_pluginsDirectory               = getGlobalOption(QStringLiteral("PluginsDirectory")).toString();
	const QString absolutePluginsDir = makeAbsolutePath(m_pluginsDirectory);
	for (const auto runtimes = activeWorldRuntimes(); WorldRuntime *runtime : runtimes)
	{
		runtime->setPluginsDirectory(absolutePluginsDir);
	}
}

void AppController::loadGlobalPlugins(WorldRuntime *runtime) const
{
	if (!runtime)
		return;

	const QString pluginList =
	    canonicalizePluginListForRuntime(getGlobalOption(QStringLiteral("PluginList")).toString());
	if (pluginList.trimmed().isEmpty())
		return;

	for (const QStringList entries = splitSerializedPathList(pluginList); const auto &entry : entries)
	{
		if (QString error; !runtime->loadPluginFile(entry, &error, true))
		{
			if (!error.isEmpty())
			{
				qWarning() << "Plugin load failed:" << error;
				runtime->outputText(
				    QStringLiteral("Plugin load failed (%1): %2").arg(QDir::toNativeSeparators(entry), error),
				    true, true);
			}
		}
	}
}

void AppController::applyFontPreferences() const
{
	const QString inputFont   = getGlobalOption(QStringLiteral("DefaultInputFont")).toString();
	const QString outputFont  = getGlobalOption(QStringLiteral("DefaultOutputFont")).toString();
	const int     inputHeight = getGlobalOption(QStringLiteral("DefaultInputFontHeight")).toInt();
	const int     inputWeight = getGlobalOption(QStringLiteral("DefaultInputFontWeight")).toInt();
	const int     inputItalic = getGlobalOption(QStringLiteral("DefaultInputFontItalic")).toInt();
	const int     inputCharset =
	    dbOnlyGlobalIntValue(m_globalIntPrefs, QStringLiteral("DefaultInputFontCharset"));
	const int outputHeight = getGlobalOption(QStringLiteral("DefaultOutputFontHeight")).toInt();
	const int outputCharset =
	    dbOnlyGlobalIntValue(m_globalIntPrefs, QStringLiteral("DefaultOutputFontCharset"));

	for (const auto runtimes = activeWorldRuntimes(); WorldRuntime *runtime : runtimes)
	{
		const QMap<QString, QString> &attrs = runtime->worldAttributes();
		if (isEnabledFlag(attrs.value(QStringLiteral("use_default_input_font"))))
		{
			if (!inputFont.isEmpty())
				runtime->setWorldAttribute(QStringLiteral("input_font_name"), inputFont);
			runtime->setWorldAttribute(QStringLiteral("input_font_height"), QString::number(inputHeight));
			runtime->setWorldAttribute(QStringLiteral("input_font_weight"), QString::number(inputWeight));
			runtime->setWorldAttribute(QStringLiteral("input_font_italic"), QString::number(inputItalic));
			runtime->setWorldAttribute(QStringLiteral("input_font_charset"), QString::number(inputCharset));
		}
		if (isEnabledFlag(attrs.value(QStringLiteral("use_default_output_font"))))
		{
			constexpr int outputWeight = 400;
			if (!outputFont.isEmpty())
				runtime->setWorldAttribute(QStringLiteral("output_font_name"), outputFont);
			runtime->setWorldAttribute(QStringLiteral("output_font_height"), QString::number(outputHeight));
			runtime->setWorldAttribute(QStringLiteral("output_font_weight"), QString::number(outputWeight));
			runtime->setWorldAttribute(QStringLiteral("output_font_charset"), QString::number(outputCharset));
		}
		runtime->setFixedPitchFont(getGlobalOption(QStringLiteral("FixedPitchFont")).toString());
	}
}

void AppController::applyChildWindowPreferences() const
{
	const int     openWorldsMax            = getGlobalOption(QStringLiteral("OpenWorldsMaximised")).toInt();
	const int     tabsStyle                = getGlobalOption(QStringLiteral("WindowTabsStyle")).toInt();
	WorldRuntime *activeWorldRuntimeBefore = nullptr;
	if (m_mainWindow)
	{
		if (WorldChildWindow *activeWorld = m_mainWindow->activeWorldChildWindow(); activeWorld)
			activeWorldRuntimeBefore = activeWorld->runtime();
	}
	if (m_mainWindow)
		m_mainWindow->setWindowTabsStyle(tabsStyle);
	if (openWorldsMax)
	{
		for (const auto runtimes = activeWorldRuntimes(); WorldRuntime *runtime : runtimes)
		{
			if (!m_mainWindow)
				break;
			if (WorldChildWindow *child = m_mainWindow->findWorldChildWindow(runtime); child)
			{
				if (!child->isMaximized())
					child->showMaximized();
			}
		}
	}
	if (m_mainWindow && activeWorldRuntimeBefore)
		m_mainWindow->activateWorldRuntime(activeWorldRuntimeBefore);
}

void AppController::applyTimerPreferences() const
{
	const int timerInterval = getGlobalOption(QStringLiteral("TimerInterval")).toInt();
	if (m_mainWindow)
		m_mainWindow->setTimerInterval(timerInterval);
}

void AppController::applyWordDelimiterPreferences() const
{
	const QString wordDelims    = getGlobalOption(QStringLiteral("WordDelimiters")).toString();
	const QString wordDelimsDbl = getGlobalOption(QStringLiteral("WordDelimitersDblClick")).toString();
	if (!m_mainWindow)
		return;
	for (const auto runtimes = activeWorldRuntimes(); WorldRuntime *runtime : runtimes)
	{
		if (const WorldChildWindow *child = m_mainWindow->findWorldChildWindow(runtime); child)
		{
			if (WorldView *view = child->view(); view)
				view->setWordDelimiters(wordDelims, wordDelimsDbl);
		}
	}
}

void AppController::applyEditorPreferences() const
{
	const QString fixedFont = getGlobalOption(QStringLiteral("FixedPitchFont")).toString();
	const int     fixedSize = getGlobalOption(QStringLiteral("FixedPitchFontSize")).toInt();
	if (!m_mainWindow)
		return;
	if (fixedFont.isEmpty() && fixedSize <= 0)
		return;

	const QFont font = qmudPreferredMonospaceFont(fixedFont, fixedSize);
	m_mainWindow->setNotepadFont(font);
}

void AppController::applyDefaultDirectories() const
{
	const QString worldsDir =
	    makeAbsolutePath(getGlobalOption(QStringLiteral("DefaultWorldFileDirectory")).toString());
	const QString stateDir =
	    makeAbsolutePath(getGlobalOption(QStringLiteral("StateFilesDirectory")).toString());
	const QString logDir =
	    makeAbsolutePath(getGlobalOption(QStringLiteral("DefaultLogFileDirectory")).toString());
	for (const auto runtimes = activeWorldRuntimes(); WorldRuntime *runtime : runtimes)
	{
		runtime->setDefaultWorldDirectory(worldsDir);
		runtime->setStateFilesDirectory(stateDir);
		runtime->setDefaultLogDirectory(logDir);
	}
}

void AppController::applyLocalePreferences()
{
	if (const QString locale = getGlobalOption(QStringLiteral("Locale")).toString();
	    locale.isEmpty() || locale.compare(m_locale, Qt::CaseInsensitive) == 0)
	{
		return;
	}
	setupI18N();
	for (const auto runtimes = activeWorldRuntimes(); WorldRuntime *runtime : runtimes)
		runtime->setLocale(m_locale);
}

void AppController::applyNotepadPreferences() const
{
	if (!m_mainWindow)
		return;
	const int notepadWrap  = getGlobalOption(QStringLiteral("NotepadWordWrap")).toInt();
	const int notepadBack  = getGlobalOption(QStringLiteral("NotepadBackColour")).toInt();
	const int notepadText  = getGlobalOption(QStringLiteral("NotepadTextColour")).toInt();
	auto      colorFromRef = [](const int colorRef)
	{
		const int r = colorRef & 0xFF;
		const int g = colorRef >> 8 & 0xFF;
		const int b = colorRef >> 16 & 0xFF;
		return QColor(r, g, b);
	};
	m_mainWindow->applyNotepadPreferences(notepadWrap != 0, colorFromRef(notepadText),
	                                      colorFromRef(notepadBack));
}

void AppController::applyListViewPreferences() const
{
	const bool smoothScroll   = getGlobalOption(QStringLiteral("SmoothScrolling")).toInt() != 0;
	const bool smootherScroll = getGlobalOption(QStringLiteral("SmootherScrolling")).toInt() != 0;
	const bool gridLines      = getGlobalOption(QStringLiteral("ShowGridLinesInListViews")).toInt() != 0;
	if (!m_mainWindow)
		return;
	m_mainWindow->setListViewGridLinesVisible(gridLines);
	for (const auto runtimes = activeWorldRuntimes(); WorldRuntime *runtime : runtimes)
	{
		if (const WorldChildWindow *child = m_mainWindow->findWorldChildWindow(runtime); child)
		{
			if (WorldView *view = child->view(); view)
				view->setSmoothScrolling(smoothScroll, smootherScroll);
		}
	}
}

void AppController::applyInputPreferences() const
{
	if (!m_mainWindow)
		return;
	const int disableMenuKey = getGlobalOption(QStringLiteral("DisableKeyboardMenuActivation")).toInt();
	m_mainWindow->setDisableKeyboardMenuActivation(disableMenuKey != 0);
}

void AppController::applyNotificationPreferences() const
{
	const int notifyCannotConnect = getGlobalOption(QStringLiteral("NotifyIfCannotConnect")).toInt();
	const int notifyDisconnect    = getGlobalOption(QStringLiteral("NotifyOnDisconnect")).toInt();
	const int errorToOutput = getGlobalOption(QStringLiteral("ErrorNotificationToOutputWindow")).toInt();
	for (const auto runtimes = activeWorldRuntimes(); WorldRuntime *runtime : runtimes)
	{
		runtime->setWorldAttribute(QStringLiteral("notify_if_cannot_connect"),
		                           QString::number(notifyCannotConnect));
		runtime->setWorldAttribute(QStringLiteral("notify_on_disconnect"), QString::number(notifyDisconnect));
		runtime->setWorldAttribute(QStringLiteral("error_notification_to_output"),
		                           QString::number(errorToOutput));
	}
}

void AppController::applyTypingPreferences() const
{
	const bool allTypingToCommand = getGlobalOption(QStringLiteral("AllTypingToCommandWindow")).toInt() != 0;
	if (!m_mainWindow)
		return;
	for (const auto runtimes = activeWorldRuntimes(); WorldRuntime *runtime : runtimes)
	{
		if (const WorldChildWindow *child = m_mainWindow->findWorldChildWindow(runtime); child)
		{
			if (WorldView *view = child->view(); view)
				view->setAllTypingToCommandWindow(allTypingToCommand);
		}
	}
}

void AppController::applyRegexPreferences() const
{
	const int regexEmpty = getGlobalOption(QStringLiteral("RegexpMatchEmpty")).toInt();
	for (const auto runtimes = activeWorldRuntimes(); WorldRuntime *runtime : runtimes)
		runtime->setWorldAttribute(QStringLiteral("regexp_match_empty"), QString::number(regexEmpty));
}

void AppController::applyIconPreferences() const
{
	const int     iconPlacement = getGlobalOption(QStringLiteral("Icon Placement")).toInt();
	const int     trayIcon      = getGlobalOption(QStringLiteral("Tray Icon")).toInt();
	const QString trayIconFile  = getGlobalOption(QStringLiteral("TrayIconFileName")).toString();
	if (!m_mainWindow)
		return;

	auto iconForId = [](const int id) -> QIcon
	{
		switch (id)
		{
		case 1:
			return QIcon(QStringLiteral(":/qmud/res/Cdrom01.ico"));
		case 2:
			return QIcon(QStringLiteral(":/qmud/res/Wrench.ico"));
		case 3:
			return QIcon(QStringLiteral(":/qmud/res/Clock05.ico"));
		case 4:
			return QIcon(QStringLiteral(":/qmud/res/Earth.ico"));
		case 5:
			return QIcon(QStringLiteral(":/qmud/res/Graph08.ico"));
		case 6:
			return QIcon(QStringLiteral(":/qmud/res/Handshak.ico"));
		case 7:
			return QIcon(QStringLiteral(":/qmud/res/Mail16b.ico"));
		case 8:
			return QIcon(QStringLiteral(":/qmud/res/Net01.ico"));
		case 9:
			return QIcon(QStringLiteral(":/qmud/res/Point11.ico"));
		default:
			return QIcon(QStringLiteral(":/qmud/res/QMud.png"));
		}
	};

	QIcon icon = iconForId(trayIcon);
	if (trayIcon == 10 && !trayIconFile.isEmpty())
	{
		QIcon customIcon(trayIconFile);
		if (customIcon.isNull())
		{
			QImageReader reader(trayIconFile);
			if (const QImage image = reader.read(); !image.isNull())
				customIcon = QIcon(QPixmap::fromImage(image));
		}
		if (!customIcon.isNull())
			icon = customIcon;
	}

	if (!icon.isNull())
	{
		m_mainWindow->setTrayIconIcon(icon);
		QGuiApplication::setWindowIcon(icon);
	}

	const bool trayVisible = iconPlacement == 1 || iconPlacement == 2;
	const bool taskbarOnly = iconPlacement == 0;
	const bool trayOnly    = iconPlacement == 1;
	const bool both        = iconPlacement == 2;

	QGuiApplication::setQuitOnLastWindowClosed(false);
	m_mainWindow->setTrayIconVisible(trayVisible);

	if (taskbarOnly)
	{
		if (!m_deferMainWindowShowUntilSplash)
		{
			m_mainWindow->showNormal();
			m_mainWindow->raise();
			m_mainWindow->activateWindow();
		}
		QGuiApplication::setQuitOnLastWindowClosed(true);
	}
	else if (trayOnly)
	{
		// Do not hide on apply; tray-only hide/restore is user-action driven.
		QGuiApplication::setQuitOnLastWindowClosed(false);
	}
	else if (both)
	{
		if (!m_deferMainWindowShowUntilSplash && !m_mainWindow->isVisible())
			m_mainWindow->showNormal();
		QGuiApplication::setQuitOnLastWindowClosed(false);
	}
}

void AppController::applyFontMetricPreferences() const
{
	const FontMetricApplySignature signature{
	    .defaultInputFont       = getGlobalOption(QStringLiteral("DefaultInputFont")).toString(),
	    .defaultInputFontHeight = getGlobalOption(QStringLiteral("DefaultInputFontHeight")).toInt(),
	    .defaultInputFontWeight = getGlobalOption(QStringLiteral("DefaultInputFontWeight")).toInt(),
	    .defaultInputFontItalic = getGlobalOption(QStringLiteral("DefaultInputFontItalic")).toInt(),
	    .defaultInputFontCharset =
	        dbOnlyGlobalIntValue(m_globalIntPrefs, QStringLiteral("DefaultInputFontCharset")),
	    .defaultOutputFont       = getGlobalOption(QStringLiteral("DefaultOutputFont")).toString(),
	    .defaultOutputFontHeight = getGlobalOption(QStringLiteral("DefaultOutputFontHeight")).toInt(),
	    .defaultOutputFontCharset =
	        dbOnlyGlobalIntValue(m_globalIntPrefs, QStringLiteral("DefaultOutputFontCharset")),
	};

	const auto signaturesEqual = [](const FontMetricApplySignature &lhs, const FontMetricApplySignature &rhs)
	{
		return lhs.defaultInputFont == rhs.defaultInputFont &&
		       lhs.defaultInputFontHeight == rhs.defaultInputFontHeight &&
		       lhs.defaultInputFontWeight == rhs.defaultInputFontWeight &&
		       lhs.defaultInputFontItalic == rhs.defaultInputFontItalic &&
		       lhs.defaultInputFontCharset == rhs.defaultInputFontCharset &&
		       lhs.defaultOutputFont == rhs.defaultOutputFont &&
		       lhs.defaultOutputFontHeight == rhs.defaultOutputFontHeight &&
		       lhs.defaultOutputFontCharset == rhs.defaultOutputFontCharset;
	};

	if (m_hasFontMetricApplySignature && signaturesEqual(signature, m_lastFontMetricApplySignature))
		return;
	m_lastFontMetricApplySignature = signature;
	m_hasFontMetricApplySignature  = true;

	if (!m_mainWindow)
		return;
	for (const auto runtimes = activeWorldRuntimes(); WorldRuntime *runtime : runtimes)
	{
		if (!runtime)
			continue;
		const QMap<QString, QString> &attrs = runtime->worldAttributes();
		const bool useDefaultInputFont = isEnabledFlag(attrs.value(QStringLiteral("use_default_input_font")));
		const bool useDefaultOutputFont =
		    isEnabledFlag(attrs.value(QStringLiteral("use_default_output_font")));
		if (!useDefaultInputFont && !useDefaultOutputFont)
			continue;
		if (const WorldChildWindow *child = m_mainWindow->findWorldChildWindow(runtime); child)
		{
			if (WorldView *view = child->view(); view)
				view->applyRuntimeSettings();
		}
	}
}

void AppController::applyActivityPreferences() const
{
	const int refreshInterval = getGlobalOption(QStringLiteral("ActivityWindowRefreshInterval")).toInt();
	const int refreshType     = getGlobalOption(QStringLiteral("ActivityWindowRefreshType")).toInt();
	if (m_mainWindow)
	{
		m_mainWindow->setActivityToolbarStyle(
		    getGlobalOption(QStringLiteral("ActivityButtonBarStyle")).toInt());
		m_mainWindow->setActivityRefresh(refreshType, refreshInterval);
	}
}

void AppController::applyPackagePreferences() const
{
	const bool enablePackage = getGlobalOption(QStringLiteral("AllowLoadingDlls")).toInt() != 0;

#ifdef QMUD_ENABLE_LUA_SCRIPTING
	if (!enablePackage)
	{
		if (m_translatorLua)
			QMudLuaSupport::applyLuaSecurityRestrictions(m_translatorLua, enablePackage);
		if (m_spellCheckerLua)
			QMudLuaSupport::applyLuaSecurityRestrictions(m_spellCheckerLua, enablePackage);
	}
#endif

	for (const auto runtimes = activeWorldRuntimes(); WorldRuntime *runtime : runtimes)
	{
		if (runtime)
			runtime->applyPackageRestrictions(enablePackage);
	}
}

void AppController::applyMiscPreferences() const
{
	if (!m_mainWindow)
		return;
	const bool flat = getGlobalOption(QStringLiteral("FlatToolbars")).toInt() != 0;
	m_mainWindow->setFlatToolbars(flat);
}

void AppController::applyRenderingPreferences() const
{
	const bool bleed = getGlobalOption(QStringLiteral("BleedBackground")).toInt() != 0;
	if (!m_mainWindow)
		return;
	for (const auto runtimes = activeWorldRuntimes(); WorldRuntime *runtime : runtimes)
	{
		if (const WorldChildWindow *child = m_mainWindow->findWorldChildWindow(runtime); child)
		{
			if (WorldView *view = child->view(); view)
				view->setBleedBackground(bleed);
		}
	}
}

void AppController::saveWindowPlacement() const
{
	// Qt-native main window geometry save.
	if (!m_mainWindow)
		return;

	// don't save if minimized, it doesn't work properly
	if (m_mainWindow->windowState() & Qt::WindowMinimized)
		return;

	QSettings settings(iniFilePath(), QSettings::IniFormat);
	settings.beginGroup(QStringLiteral("MainWindow"));
	const QRect previousNormalGeometry = settings.value(QStringLiteral("NormalGeometry")).toRect();
	settings.setValue(QStringLiteral("Geometry"), m_mainWindow->saveGeometry());
	settings.setValue(QStringLiteral("Maximized"), m_mainWindow->isMaximized());
	QRect normalGeometry;
	if (m_mainWindow->isMaximized())
		normalGeometry = m_mainWindow->lastNormalGeometry();
	if (m_mainWindow->isMaximized() &&
	    (!normalGeometry.isValid() || normalGeometry.isNull() ||
	     normalGeometry.size() == m_mainWindow->geometry().size()) &&
	    previousNormalGeometry.isValid() && !previousNormalGeometry.isNull())
		normalGeometry = previousNormalGeometry;
	if (!normalGeometry.isValid() || normalGeometry.isNull())
		normalGeometry = m_mainWindow->normalGeometry();
	if (m_mainWindow->isMaximized() && normalGeometry.isValid() && !normalGeometry.isNull() &&
	    normalGeometry.size() == m_mainWindow->geometry().size() && previousNormalGeometry.isValid() &&
	    !previousNormalGeometry.isNull())
		normalGeometry = previousNormalGeometry;
	if ((!normalGeometry.isValid() || normalGeometry.isNull()) && !m_mainWindow->isMaximized())
		normalGeometry = m_mainWindow->geometry();
	if (normalGeometry.isValid() && !normalGeometry.isNull())
		settings.setValue(QStringLiteral("NormalGeometry"), normalGeometry);
	settings.endGroup();
}

QString AppController::iniFilePath() const
{
	return m_workingDir + QStringLiteral("QMud.conf");
}

void AppController::saveSessionState() const
{
	savePrintSetupPreferences();
	// App-level session/world-set persistence is intentionally not stored here.
}

void AppController::showTipDialog() const
{
	TipDialog dlg([this](const QString &section, const QString &entry, const int defValue)
	              { return dbGetInt(section, entry, defValue); },
	              [this](const QString &section, const QString &entry, const QString &defValue)
	              { return dbGetString(section, entry, defValue); },
	              [this](const QString &section, const QString &entry, const int value)
	              { return dbWriteInt(section, entry, value); },
	              [this](const QString &section, const QString &entry, const QString &value)
	              { return dbWriteString(section, entry, value); }, m_mainWindow);
	dlg.exec();
}

void AppController::showTipAtStartup() const
{
	if (const int tipDisabled = dbGetInt(QStringLiteral("control"), QStringLiteral("Tip_StartUp"), 0);
	    tipDisabled == 0)
		showTipDialog();
}

void AppController::showGettingStartedIfNeeded() const
{
	// Show the getting-started help topic when available.
	if (!showHelpDoc(m_mainWindow, QStringLiteral("starting")))
	{
		QMessageBox::information(m_mainWindow, QStringLiteral("Getting Started"),
		                         QStringLiteral("Getting Started help is not available."));
	}
}

void AppController::showUpgradeWelcomeIfNeeded() const
{
	const QString        msg1 = QStringLiteral("Welcome to QMud, version %1").arg(m_version);
	const QString        msg2 = QStringLiteral("Thank you for upgrading QMud to version %1").arg(m_version);
	WelcomeUpgradeDialog dlg(msg1, msg2, m_mainWindow);
	dlg.exec();
}

void AppController::maybeShowDeferredUpgradeWelcomeAfterStartupRestores() const
{
	if (!QMudWorldSessionRestoreFlow::shouldShowDeferredUpgradeWelcome(
	        m_deferUpgradeWelcomeUntilStartupRestores, m_startupRestoreDispatchComplete,
	        m_restoreScrollbackInFlight))
	{
		return;
	}
	showUpgradeWelcomeIfNeeded();
	m_deferUpgradeWelcomeUntilStartupRestores = false;
	m_startupRestoreDispatchComplete          = false;
}

void AppController::backupDataOnUpgradeIfNeeded(const int previousVersion, const bool firstTime) const
{
	if (firstTime || previousVersion >= kThisVersion)
		return;

	if (getGlobalOption(QStringLiteral("BackupOnUpgrades")).toInt() == 0)
	{
		qInfo() << "Upgrade backup is disabled by preference.";
		return;
	}

	QString archivePath;
	QString errorMessage;
	if (!createUpgradeBackupArchive(m_workingDir, m_version, archivePath, errorMessage))
	{
		qWarning() << "Failed to create upgrade backup before migration from version" << previousVersion
		           << ":" << errorMessage;
		return;
	}

	qInfo() << "Created upgrade backup archive:" << archivePath;
}

void AppController::finalizeStartupIfReady()
{
	if (m_startupFinalized || !m_splashMinDelayElapsed || !m_initializeFinished)
		return;

	m_startupFinalized = true;
	hideSplashScreen();

	if (!m_initializeSucceeded)
	{
		QCoreApplication::exit(1);
		return;
	}

	if (m_startupNeedsUpgradeWelcome)
	{
		m_deferUpgradeWelcomeUntilStartupRestores = true;
		m_startupRestoreDispatchComplete          = false;
	}

	if (!m_reloadLaunchRequested && m_startupFirstTime)
	{
		WelcomeDialog dlg(QStringLiteral("I notice that this is the first time you have used"
		                                 " QMud on this PC."),
		                  m_mainWindow);
		dlg.exec();
	}

	setupStartupBehavior();
	m_startupRestoreDispatchComplete = true;
	maybeShowDeferredUpgradeWelcomeAfterStartupRestores();

	if (!m_reloadLaunchRequested && m_startupFirstTime)
		showGettingStartedIfNeeded();

	m_startupFirstTime           = false;
	m_startupNeedsUpgradeWelcome = false;
}

void AppController::showSplashScreen()
{
	if (m_splash || !m_mainWindow)
		return;

	QPixmap pixmap(QStringLiteral(":/qmud/res/splash.png"));
	if (pixmap.isNull())
	{
		pixmap = QPixmap(400, 200);
		pixmap.fill(Qt::white);
	}
	const QSize   halfSize(qMax(1, pixmap.width() / 2), qMax(1, pixmap.height() / 2));
	const QPixmap splashPixmap = pixmap.scaled(halfSize, Qt::IgnoreAspectRatio, Qt::FastTransformation);
	m_splash                   = new QSplashScreen(splashPixmap, Qt::WindowStaysOnTopHint);
	QFont splashFont           = m_splash->font();
	splashFont.setPointSize(14);
	m_splash->setFont(splashFont);
	m_splash->showMessage(QStringLiteral("Copyright 2026 Panagiotis Kalogiratos"),
	                      Qt::AlignHCenter | Qt::AlignBottom, QColor(255, 140, 0));
	m_splash->show();
	m_splash->raise();
	m_splash->activateWindow();
	m_deferMainWindowShowUntilSplash     = true;
	m_showMainWindowMaximizedAfterSplash = false;
}

void AppController::hideSplashScreen()
{
	if (m_splash)
	{
		m_splash->finish(m_mainWindow);
		delete m_splash;
		m_splash                         = nullptr;
		m_deferMainWindowShowUntilSplash = false;
		if (m_mainWindow && !m_mainWindow->isVisible())
		{
			if (m_showMainWindowMaximizedAfterSplash)
			{
				m_mainWindow->showMaximized();
			}
			else
			{
				m_mainWindow->show();
			}
		}
		m_showMainWindowMaximizedAfterSplash = false;
	}
}

bool AppController::openPreferencesDatabase()
{
	static const auto kPreferencesDatabaseFile       = QStringLiteral("QMud.sqlite");
	static const auto kLegacyPreferencesDatabaseFile = QStringLiteral("mushclient_prefs.sqlite");

	auto              pathFor = [](const QString &baseDir, const QString &fileName) -> QString
	{ return QDir(baseDir).filePath(fileName); };

	const auto workingDir = QDir::cleanPath(m_workingDir);
	const auto appDir     = QDir::cleanPath(QCoreApplication::applicationDirPath());

	const auto directoryHasPreferencesDatabase = [&](const QString &dir) -> bool
	{
		return QFileInfo::exists(pathFor(dir, kPreferencesDatabaseFile)) ||
		       QFileInfo::exists(pathFor(dir, kLegacyPreferencesDatabaseFile));
	};

	QString selectedBaseDir = workingDir;
	if (!directoryHasPreferencesDatabase(workingDir) && qEnvironmentVariableIsEmpty("APPIMAGE") &&
	    directoryHasPreferencesDatabase(appDir))
	{
		selectedBaseDir = appDir;
	}

	const QString canonicalDbPath = pathFor(selectedBaseDir, kPreferencesDatabaseFile);

	if (const QString legacyDbPath = pathFor(selectedBaseDir, kLegacyPreferencesDatabaseFile);
	    QFileInfo::exists(legacyDbPath))
	{
		snapshotPreferencesDatabaseToMigratedDir(legacyDbPath, m_workingDir);

		if (QFileInfo::exists(canonicalDbPath))
		{
			if (!QFile::remove(legacyDbPath))
			{
				QMessageBox::critical(
				    m_mainWindow, QStringLiteral("Database Error"),
				    QStringLiteral("Unable to remove legacy preferences database: %1").arg(legacyDbPath));
				return false;
			}
		}
		else
		{
			if (const QString canonicalDir = QFileInfo(canonicalDbPath).absolutePath();
			    !QDir().mkpath(canonicalDir))
			{
				QMessageBox::critical(
				    m_mainWindow, QStringLiteral("Database Error"),
				    QStringLiteral("Unable to create preferences database directory: %1").arg(canonicalDir));
				return false;
			}

			if (!QFile::rename(legacyDbPath, canonicalDbPath))
			{
				if (!QFile::copy(legacyDbPath, canonicalDbPath) || !QFile::remove(legacyDbPath))
				{
					QMessageBox::critical(
					    m_mainWindow, QStringLiteral("Database Error"),
					    QStringLiteral("Unable to migrate legacy preferences database:\n%1\n->\n%2")
					        .arg(legacyDbPath, canonicalDbPath));
					return false;
				}
			}
		}
	}

	m_preferencesDatabaseName = canonicalDbPath;

	if (!m_dbConnectionName.isEmpty())
	{
		m_db = QSqlDatabase();
		QSqlDatabase::removeDatabase(m_dbConnectionName);
		m_dbConnectionName.clear();
	}

	static int connectionCounter = 0;
	m_dbConnectionName           = QStringLiteral("qmud_prefs_%1").arg(++connectionCounter);
	m_db                         = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_dbConnectionName);
	m_db.setDatabaseName(m_preferencesDatabaseName);
	m_db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=2000"));

	if (!m_db.open())
	{
		const QString err = m_db.lastError().text();
		QMessageBox::critical(m_mainWindow, QStringLiteral("Database Error"),
		                      QStringLiteral("Can't open global preferences database at: %1\n(Error was: "
		                                     "\"%2\")\nCheck you have write-access to that file.")
		                          .arg(m_preferencesDatabaseName, err));
		m_db = QSqlDatabase();
		QSqlDatabase::removeDatabase(m_dbConnectionName);
		m_dbConnectionName.clear();
		return false;
	}

	if (const QFileInfo dbInfo(m_preferencesDatabaseName); dbInfo.exists() && !dbInfo.isWritable())
	{
		QMessageBox::critical(
		    m_mainWindow, QStringLiteral("Database Error"),
		    QStringLiteral("The global preferences database at: <%1> is read-only.\nPlease ensure "
		                   "that you have write-access to that file.")
		        .arg(m_preferencesDatabaseName));
		m_db.close();
		m_db = QSqlDatabase();
		QSqlDatabase::removeDatabase(m_dbConnectionName);
		m_dbConnectionName.clear();
		return false;
	}

	QString pragmaError;
	if (!applySqliteWalAndNormalSynchronous(m_db, pragmaError))
	{
		QMessageBox::critical(
		    m_mainWindow, QStringLiteral("Database Error"),
		    QStringLiteral(
		        "Unable to configure preferences database for WAL/NORMAL mode at: %1\n(Error was: \"%2\")")
		        .arg(m_preferencesDatabaseName, pragmaError));
		m_db.close();
		m_db = QSqlDatabase();
		QSqlDatabase::removeDatabase(m_dbConnectionName);
		m_dbConnectionName.clear();
		return false;
	}

	QString dbVersion;
	int     db_rc = SQLITE_OK;

	(void)dbSimpleQuery(QStringLiteral("SELECT value FROM control WHERE name = 'database_version'"),
	                    dbVersion, false, QString());

	// no version or out of date, make database
	if (constexpr int kCurrentDbVersion = 1; dbVersion.isEmpty() || dbVersion.toInt() < kCurrentDbVersion)
	{
		m_db.transaction();

		db_rc = dbExecute(
		    QStringLiteral(
		        // general control information
		        "DROP TABLE IF EXISTS control;"
		        "CREATE TABLE control (name VARCHAR(10) NOT NULL PRIMARY KEY, value INT NOT NULL );"),
		    true);

		if (db_rc != SQLITE_OK)
		{
			m_db.rollback();
			return false; // SQL error
		}

		(void)dbWriteInt(QStringLiteral("control"), QStringLiteral("database_version"), kCurrentDbVersion);

		db_rc = dbExecute(
		    QStringLiteral(

		        // global preferences
		        "DROP TABLE IF EXISTS prefs;"
		        "CREATE TABLE prefs (name VARCHAR(50) NOT NULL PRIMARY KEY, value TEXT NOT NULL ); "

		        // world window positions
		        "DROP TABLE IF EXISTS worlds;"
		        "CREATE TABLE worlds (name VARCHAR(50) NOT NULL PRIMARY KEY, value TEXT NOT NULL ); "

		        ),
		    true);

		if (db_rc != SQLITE_OK)
		{
			m_db.rollback();
			return false; // SQL error
		}

		// copy from registry to database for legacy support
		if (populateDatabase() != SQLITE_OK)
		{
			m_db.rollback();
			return false; // SQL error
		}

		m_db.commit();
	} // end database empty

	return true;
}

int AppController::populateDatabase() const
{
	// copy the registry prefs into the SQLite database into table 'prefs'

	int db_rc = SQLITE_OK;

	for (int i = 0; kGlobalOptionsTable[i].name; i++)
	{
		const int value = kGlobalOptionsTable[i].defaultValue;

		db_rc = dbExecute(QStringLiteral("INSERT INTO prefs (name, value) VALUES ('%1', %2)")
		                      .arg(QString::fromUtf8(kGlobalOptionsTable[i].name))
		                      .arg(value),
		                  true);

		if (db_rc != SQLITE_OK)
			return db_rc;
	}

	for (int i = 0; kAlphaGlobalOptionsTable[i].name; i++)
	{
		const QString key        = QString::fromUtf8(kAlphaGlobalOptionsTable[i].name);
		QString       strDefault = QString::fromUtf8(kAlphaGlobalOptionsTable[i].defaultValue);

		// fix up the fixed-pitch font
		if (key == QStringLiteral("DefaultInputFont") || key == QStringLiteral("DefaultOutputFont") ||
		    key == QStringLiteral("FixedPitchFont"))
			strDefault = m_fixedPitchFont;

		QString strValue = normalizeStoredGlobalStringValue(key, strDefault);

		strValue.replace('\'', QStringLiteral("''")); // fix up quotes

		db_rc = dbExecute(
		    QStringLiteral("INSERT INTO prefs (name, value) VALUES ('%1', '%2')").arg(key, strValue), true);

		if (db_rc != SQLITE_OK)
			return db_rc;
	}

	// Seed DB-only prefs that are not part of canonical global option tables.
	for (const auto &[prefKey, prefDefault] : kDbOnlyGlobalIntPrefs)
	{
		db_rc = dbExecute(QStringLiteral("INSERT INTO prefs (name, value) VALUES ('%1', %2)")
		                      .arg(QString::fromUtf8(prefKey))
		                      .arg(prefDefault),
		                  true);
		if (db_rc != SQLITE_OK)
			return db_rc;
	}

	return SQLITE_OK;
} // end of AppController::populateDatabase

void AppController::loadGlobalsFromDatabase()
{
	const auto legacyGlobalKeyForCanonical = [](const QString &key) -> QString
	{
		if (key == QStringLiteral("ConfirmBeforeClosingQmud"))
			return QStringLiteral("ConfirmBeforeClosingMushclient");
		if (key == QStringLiteral("DefaultInputFontItalic"))
			return QStringLiteral("DefaultInputFontItalic ");
		if (key == QStringLiteral("DefaultOutputFont"))
			return QStringLiteral("DefaultOutputFont ");
		if (key == QStringLiteral("DefaultTimersFile"))
			return QStringLiteral("DefaultTimersFile ");
		return {};
	};
	const auto readPrefsValue = [this](const QString &key, QString &out) -> bool
	{
		if (!m_db.isOpen())
			return false;
		QSqlQuery query(m_db);
		if (!query.exec(QStringLiteral("SELECT value FROM prefs WHERE name = '%1'").arg(escapeSql(key))))
			return false;
		if (!query.next())
			return false;
		const QVariant value = query.value(0);
		out                  = value.isNull() ? QString() : value.toString();
		return true;
	};

	QString     dbValue;
	QStringList migratedLegacyPrefKeys;
	bool        prefsSnapshotTaken  = false;
	const auto  ensurePrefsSnapshot = [&]
	{
		if (prefsSnapshotTaken)
			return;
		snapshotPreferencesDatabaseToMigratedDir(m_preferencesDatabaseName, m_workingDir);
		prefsSnapshotTaken = true;
	};

	for (int i = 0; kGlobalOptionsTable[i].name; i++)
	{
		const QString key   = QString::fromUtf8(kGlobalOptionsTable[i].name);
		bool          found = readPrefsValue(key, dbValue);
		if (!found)
		{
			if (const QString legacyKey = legacyGlobalKeyForCanonical(key);
			    !legacyKey.isEmpty() && readPrefsValue(legacyKey, dbValue))
			{
				bool      migrateOk     = false;
				const int migratedValue = dbValue.toInt(&migrateOk);
				ensurePrefsSnapshot();
				(void)dbWriteInt(QStringLiteral("prefs"), key,
				                 migrateOk ? migratedValue : kGlobalOptionsTable[i].defaultValue);
				(void)dbExecute(
				    QStringLiteral("DELETE FROM prefs WHERE name = '%1'").arg(escapeSql(legacyKey)), false);
				migratedLegacyPrefKeys.append(legacyKey);
				found = true;
			}
		}
		if (!found)
			dbValue = QString::number(kGlobalOptionsTable[i].defaultValue);

		bool      ok    = false;
		const int value = dbValue.toInt(&ok);
		m_globalIntPrefs.insert(key, ok ? value : kGlobalOptionsTable[i].defaultValue);
	}

	for (int i = 0; kAlphaGlobalOptionsTable[i].name; i++)
	{
		const QString key        = QString::fromUtf8(kAlphaGlobalOptionsTable[i].name);
		QString       strDefault = QString::fromUtf8(kAlphaGlobalOptionsTable[i].defaultValue);
		if (key == QStringLiteral("StateFilesDirectory"))
		{
			strDefault = m_pluginsDirectory;
			strDefault += "state/";
		}

		bool found = readPrefsValue(key, dbValue);
		if (!found)
		{
			if (const QString legacyKey = legacyGlobalKeyForCanonical(key);
			    !legacyKey.isEmpty() && readPrefsValue(legacyKey, dbValue))
			{
				ensurePrefsSnapshot();
				(void)dbWriteString(QStringLiteral("prefs"), key, dbValue);
				(void)dbExecute(
				    QStringLiteral("DELETE FROM prefs WHERE name = '%1'").arg(escapeSql(legacyKey)), false);
				migratedLegacyPrefKeys.append(legacyKey);
				found = true;
			}
		}
		if (!found)
		{
			dbValue = strDefault;
		}
		const QString originalDbValue = dbValue;
		if (key == QStringLiteral("LuaScript"))
			dbValue = migrateLegacyLuaScriptTipText(dbValue);

		QString storedValue = normalizeStoredGlobalStringValue(key, dbValue);
		if (key.compare(QStringLiteral("WorldList"), Qt::CaseInsensitive) == 0)
		{
			bool          worldListChanged = false;
			const QString migratedWorldList =
			    migrateWorldListPaths(m_workingDir, storedValue, &worldListChanged);
			if (worldListChanged)
				storedValue = migratedWorldList;
		}
		if (key.compare(QStringLiteral("PluginList"), Qt::CaseInsensitive) == 0)
		{
			bool          pluginListChanged = false;
			const QString migratedPluginList =
			    migratePluginListPaths(m_workingDir, storedValue, &pluginListChanged);
			if (pluginListChanged)
				storedValue = migratedPluginList;
		}
		if (storedValue != originalDbValue)
		{
			ensurePrefsSnapshot();
			(void)dbWriteString(QStringLiteral("prefs"), key, storedValue);
		}
		const QString runtimeValue = normalizeRuntimeGlobalStringValue(key, storedValue);
		m_globalStringPrefs.insert(key, runtimeValue);
		if (key == QStringLiteral("PluginsDirectory"))
			m_pluginsDirectory = runtimeValue;
		if (key == QStringLiteral("FixedPitchFont"))
			m_fixedPitchFont = dbValue;
		if (key == QStringLiteral("LuaScript"))
			m_luaScript = dbValue;
	}

	migrateLegacyWorldTree(m_workingDir,
	                       m_globalStringPrefs.value(QStringLiteral("DefaultWorldFileDirectory")));

	// Legacy behavior: these prefs are stored in DB but are not part of the canonical
	// global option tables. Load them explicitly so session restarts preserve them.
	for (const auto &[prefKey, prefDefault] : kDbOnlyGlobalIntPrefs)
	{
		const QString key = QString::fromUtf8(prefKey);
		if (!readPrefsValue(key, dbValue))
			dbValue = QString::number(prefDefault);
		bool      ok    = false;
		const int value = dbValue.toInt(&ok);
		m_globalIntPrefs.insert(key, ok ? value : prefDefault);
	}

	if (!migratedLegacyPrefKeys.isEmpty())
		qInfo() << "Migrated legacy global preference keys:" << migratedLegacyPrefKeys;

	// get Lua initialisation (sandbox) if necessary

	if (m_luaScript.isEmpty())
	{
		m_luaScript = "-- Put Lua initialization code (eg. sandbox) here.\r\n"
		              "-- Example sandbox can be found in the docs directory.\r\n";
	} // end of needing to put something in sandbox
} // end of AppController::loadGlobalsFromDatabase

int AppController::dbExecute(const QString &sql, const bool showError) const
{
	if (!m_db.isOpen())
		return SQLITE_ERROR;

	const QStringList statements = sql.split(QLatin1Char(';'), Qt::SkipEmptyParts);
	QSqlQuery         query(m_db);
	for (const QString &stmt : statements)
	{
		const QString trimmed = stmt.trimmed();
		if (trimmed.isEmpty())
			continue;
		if (!query.exec(trimmed))
		{
			if (showError)
			{
				const QString msg =
				    QStringLiteral("SQL error: %1\n%2").arg(trimmed, query.lastError().text());
				QMessageBox::critical(m_mainWindow, QStringLiteral("Database Error"), msg);
			}
			return SQLITE_ERROR;
		}
	}
	return SQLITE_OK;
}

int AppController::dbSimpleQuery(const QString &sql, QString &result, const bool showError,
                                 const QString &defaultValue) const
{
	if (!m_db.isOpen())
	{
		result = defaultValue;
		return SQLITE_ERROR;
	}

	QSqlQuery query(m_db);
	if (!query.exec(sql))
	{
		if (showError)
			QMessageBox::critical(m_mainWindow, QStringLiteral("Database Error"),
			                      QStringLiteral("SQL error: %1\n%2").arg(sql, query.lastError().text()));
		result = defaultValue;
		return SQLITE_ERROR;
	}

	if (query.next())
	{
		const QVariant value = query.value(0);
		result               = value.isNull() ? defaultValue : value.toString();
		return SQLITE_OK;
	}

	result = defaultValue;
	return SQLITE_OK;
}

int AppController::dbGetInt(const QString &section, const QString &entry, const int defaultValue) const
{
	QString result;
	dbSimpleQuery(QStringLiteral("SELECT value FROM %1 WHERE name = '%2'").arg(section, escapeSql(entry)),
	              result, false, QString::number(defaultValue));
	bool      ok    = false;
	const int value = result.toInt(&ok);
	return ok ? value : defaultValue;
}

int AppController::dbWriteInt(const QString &section, const QString &entry, const int value) const
{
	if (!m_db.isOpen())
		return SQLITE_ERROR;

	const QString escapedEntry = escapeSql(entry);
	const QString sqlUpdate    = QStringLiteral("UPDATE %1 SET value = '%2' WHERE name = '%3'")
	                              .arg(section, QString::number(value), escapedEntry);
	int rc = dbExecute(sqlUpdate, false);
	if (rc != SQLITE_OK)
		return rc;

	if (dbChanges() == 0)
	{
		const QString sqlInsert = QStringLiteral("INSERT INTO %1 (name, value) VALUES ('%2', '%3')")
		                              .arg(section, escapedEntry, QString::number(value));
		rc = dbExecute(sqlInsert, false);
	}

	return rc;
}

QString AppController::dbGetString(const QString &section, const QString &entry,
                                   const QString &defaultValue) const
{
	QString result;
	dbSimpleQuery(QStringLiteral("SELECT value FROM %1 WHERE name = '%2'").arg(section, escapeSql(entry)),
	              result, false, defaultValue);
	return result;
}

int AppController::dbWriteString(const QString &section, const QString &entry, const QString &value) const
{
	if (!m_db.isOpen())
		return SQLITE_ERROR;

	QString normalizedValue = value;
	if (section.compare(QStringLiteral("prefs"), Qt::CaseInsensitive) == 0)
		normalizedValue = normalizeStoredGlobalStringValue(entry, normalizedValue);

	const QString escapedEntry = escapeSql(entry);
	const QString escapedValue = escapeSql(normalizedValue);
	const QString sqlUpdate    = QStringLiteral("UPDATE %1 SET value = '%2' WHERE name = '%3'")
	                              .arg(section, escapedValue, escapedEntry);
	int rc = dbExecute(sqlUpdate, false);
	if (rc != SQLITE_OK)
		return rc;

	if (dbChanges() == 0)
	{
		const QString sqlInsert = QStringLiteral("INSERT INTO %1 (name, value) VALUES ('%2', '%3')")
		                              .arg(section, escapedEntry, escapedValue);
		rc = dbExecute(sqlInsert, false);
	}

	return rc;
}

QString AppController::escapeSql(const QString &input)
{
	QString out = input;
	return out.replace('\'', QStringLiteral("''"));
}

int AppController::dbChanges() const
{
	if (!m_db.isOpen())
		return 0;
	QSqlQuery query(m_db);
	if (!query.exec(QStringLiteral("SELECT changes()")))
		return 0;
	if (!query.next())
		return 0;
	return query.value(0).toInt();
}

void AppController::onCommandTriggered(const QString &cmdName)
{
	const QString canonicalCmd = canonicalCommandName(cmdName);
	const auto    isCommand    = [&](const QString &commandName) -> bool
	{ return cmdName == commandName || canonicalCmd == commandName; };

	// Keep command-name dispatch behavior aligned across legacy and Qt-native paths.
	auto focusedEditor = []() -> QWidget *
	{
		if (QWidget *focus = QApplication::focusWidget(); qobject_cast<QLineEdit *>(focus) ||
		                                                  qobject_cast<QPlainTextEdit *>(focus) ||
		                                                  qobject_cast<QTextEdit *>(focus))
			return focus;
		return nullptr;
	};

	const auto dispatchEditCommand = [](auto *edit, const QString &name) -> bool
	{
		if (name == QStringLiteral("Undo"))
			edit->undo();
		else if (name == QStringLiteral("Copy"))
			edit->copy();
		else if (name == QStringLiteral("Cut"))
			edit->cut();
		else if (name == QStringLiteral("Paste"))
			edit->paste();
		else if (name == QStringLiteral("SelectAll"))
			edit->selectAll();
		else
			return false;
		return true;
	};

	auto handleEditCommand = [&](const QString &name) -> bool
	{
		QWidget *focus = focusedEditor();
		if (!focus)
			return false;
		if (auto *edit = qobject_cast<QLineEdit *>(focus))
			return dispatchEditCommand(edit, name);
		if (auto *edit = qobject_cast<QPlainTextEdit *>(focus))
			return dispatchEditCommand(edit, name);
		if (auto *edit = qobject_cast<QTextEdit *>(focus))
			return dispatchEditCommand(edit, name);
		return false;
	};

	auto saveTextFile = [](const QString &path, const QString &text, QString *error) -> bool
	{
		const QString trimmed = path.trimmed();
		if (trimmed.isEmpty())
		{
			if (error)
				*error = QStringLiteral("No filename specified.");
			return false;
		}
		QSaveFile file(trimmed);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
		{
			if (error)
				*error = QStringLiteral("Unable to create the requested file.");
			return false;
		}
		if (const QByteArray payload = text.toLocal8Bit(); file.write(payload) != payload.size())
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
		return true;
	};

	auto ensureExtension = [](const QString &path, const QString &suffix) -> QString
	{
		if (const QFileInfo info(path); !info.suffix().isEmpty())
			return path;
		return path + QStringLiteral(".") + suffix;
	};

	auto quoteForumCodes = [this](const QString &input) -> QString
	{
		int     changes = 0;
		QString out;
		out.reserve(input.size() * 2);
		for (const QChar ch : input)
		{
			if (ch == QLatin1Char('[') || ch == QLatin1Char(']') || ch == QLatin1Char('\\'))
			{
				out += QLatin1Char('\\');
				++changes;
			}
			out += ch;
		}
		const QString message = QStringLiteral("Clipboard converted for use with the Forum, %1 change%2 made")
		                            .arg(changes)
		                            .arg(changes == 1 ? QString() : QStringLiteral("s"));
		const auto response = QMessageBox::question(m_mainWindow, QStringLiteral("QMud"), message,
		                                            QMessageBox::Ok | QMessageBox::Cancel);
		if (response != QMessageBox::Ok)
			return input;
		return out;
	};

	auto decodeEscapes = [](const QString &input) -> QString
	{
		QString out = input;
		out.replace(QStringLiteral("\\\\"), QStringLiteral("\x01"));
		out.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
		out.replace(QStringLiteral("\\t"), QStringLiteral("\t"));
		out.replace(QStringLiteral("\x01"), QStringLiteral("\\"));
		return out;
	};

	if (cmdName == QStringLiteral("About"))
		handleAppAbout();
	else if (cmdName == QStringLiteral("New"))
		handleFileNew();
	else if (cmdName == QStringLiteral("NewActivityWindow"))
	{
		if (m_mainWindow)
		{
			ensureActivityDocument();
			m_mainWindow->addMdiSubWindow(new ActivityChildWindow);
		}
	}
	else if (cmdName == QStringLiteral("NewTextWindow"))
	{
		if (m_mainWindow)
			m_mainWindow->addMdiSubWindow(new TextChildWindow);
	}
	else if (cmdName == QStringLiteral("Import"))
	{
		if (!m_mainWindow || !m_mainWindow->activeWorldChildWindow())
		{
			QMessageBox::information(m_mainWindow, QStringLiteral("Import"),
			                         QStringLiteral("Open a world before importing XML."));
			return;
		}
		ImportXmlDialog dlg(this, m_mainWindow);
		dlg.exec();
	}
	else if (cmdName == QStringLiteral("ResetToolbars"))
	{
		if (m_mainWindow)
			m_mainWindow->resetToolbarsToDefaults();
	}
	else if (cmdName == QStringLiteral("ViewToolbar") || cmdName == QStringLiteral("ViewWorldToolbar") ||
	         cmdName == QStringLiteral("ActivityToolbar") || cmdName == QStringLiteral("ViewStatusbar") ||
	         cmdName == QStringLiteral("ViewInfoBar"))
	{
		// Handled directly by MainWindow toggle slots; preference persistence runs
		// through MainWindow::viewPreferenceChanged.
	}
	else if (cmdName.startsWith(QStringLiteral("World")))
	{
		const QString suffix = cmdName.mid(QStringLiteral("World").size());
		bool          ok     = false;
		const int     slot   = suffix.toInt(&ok);
		if (ok && m_mainWindow)
			m_mainWindow->activateWorldSlot(slot);
	}
	else if (cmdName == QStringLiteral("HelpContents") || cmdName == QStringLiteral("ContextHelp"))
		handleHelpContents();
	else if (cmdName == QStringLiteral("Copy"))
	{
		if (!handleEditCommand(cmdName))
			handleCopy();
	}
	else if (cmdName == QStringLiteral("CopyAsHTML"))
		handleCopyAsHtml();
	else if (cmdName == QStringLiteral("EditColourPicker"))
		handleEditColourPicker();
	else if (cmdName == QStringLiteral("QuickConnect"))
		handleQuickConnect();
	else if (cmdName == QStringLiteral("Connect"))
		handleConnect();
	else if (cmdName == QStringLiteral("Connect_Or_Reconnect"))
		handleConnectOrReconnect();
	else if (cmdName == QStringLiteral("Disconnect"))
		handleDisconnect();
	else if (isCommand(QStringLiteral("Minimize")))
		handleGameMinimize();
	else if (cmdName == QStringLiteral("LogSession"))
		handleLogSession();
	else if (cmdName == QStringLiteral("ReloadQMud"))
		handleReloadQmud();
	else if (cmdName == QStringLiteral("UpdateQmudNow"))
		handleUpdateQmudNow();
	else if (cmdName == QStringLiteral("GameWrapLines"))
	{
		if (!m_mainWindow)
			return;
		WorldChildWindow *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		WorldRuntime *runtime = world->runtime();
		WorldView    *view    = world->view();
		if (!runtime || !view)
			return;
		if (const int wrapColumn = runtime->worldAttributes().value(QStringLiteral("wrap_column")).toInt();
		    wrapColumn > 0)
		{
			m_savedWrapColumns.insert(runtime, wrapColumn);
			runtime->setWorldAttribute(QStringLiteral("wrap_column"), QStringLiteral("0"));
		}
		else
		{
			const int fallback = m_savedWrapColumns.value(runtime, 80);
			runtime->setWorldAttribute(QStringLiteral("wrap_column"), QString::number(fallback));
		}
		view->applyRuntimeSettings();
		m_mainWindow->updateStatusBar();
		m_mainWindow->refreshActionState();
	}
	else if (cmdName == QStringLiteral("AutoSay"))
	{
		if (!m_mainWindow)
			return;
		WorldChildWindow *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		WorldRuntime *runtime = world->runtime();
		if (!runtime)
			return;
		const QMap<QString, QString> &attrs = runtime->worldAttributes();
		if (const QString autoSayString = attrs.value(QStringLiteral("auto_say_string"));
		    autoSayString.isEmpty())
			return;
		const QString enabled   = attrs.value(QStringLiteral("enable_auto_say"));
		const bool    isEnabled = isEnabledFlag(enabled);
		runtime->setWorldAttribute(QStringLiteral("enable_auto_say"),
		                           isEnabled ? QStringLiteral("0") : QStringLiteral("1"));
		m_mainWindow->updateStatusBar();
		m_mainWindow->refreshActionState();
	}
	else if (isCommand(QStringLiteral("AsciiArt")))
	{
		if (!m_mainWindow)
			return;

		auto *target = focusedEditor();
		if (auto *textChild = m_mainWindow->activeTextChildWindow(); textChild)
		{
			if (!target)
				target = textChild->editor();
		}
		else
		{
			// Legacy behavior: Ascii Art belongs to IDR_NORMALTYPE (text/notepad), not world windows.
			target = nullptr;
		}
		if (!target)
		{
			QMessageBox::information(m_mainWindow, QStringLiteral("ASCII Art"),
			                         QStringLiteral("ASCII Art is only available in text/notepad windows."));
			return;
		}

		const auto savedLayout = getGlobalOption(QStringLiteral("AsciiArtLayout")).toInt();
		const auto savedFont   = getGlobalOption(QStringLiteral("AsciiArtFont")).toString();

		QDialog    dlg(m_mainWindow);
		dlg.setWindowTitle(QStringLiteral("ASCII Art"));
		auto *layout = new QVBoxLayout(&dlg);

		auto *form     = new QFormLayout;
		auto *textEdit = new QPlainTextEdit(&dlg);
		textEdit->setPlainText(m_asciiArtText);
		form->addRow(QStringLiteral("Text:"), textEdit);

		auto *fontEdit = new QLineEdit(savedFont, &dlg);
		auto *browse   = new QPushButton(QStringLiteral("Browse..."), &dlg);
		auto *fontRow  = new QHBoxLayout;
		fontRow->addWidget(fontEdit, 1);
		fontRow->addWidget(browse);
		form->addRow(QStringLiteral("Font:"), fontRow);

		auto *layoutCombo = new QComboBox(&dlg);
		layoutCombo->addItem(QStringLiteral("Default smush"), 0);
		layoutCombo->addItem(QStringLiteral("Full smush"), 1);
		layoutCombo->addItem(QStringLiteral("Kern"), 2);
		layoutCombo->addItem(QStringLiteral("Full width"), 3);
		layoutCombo->addItem(QStringLiteral("Overlap"), 4);
		const int savedIndex = qMax(0, layoutCombo->findData(savedLayout));
		layoutCombo->setCurrentIndex(savedIndex);
		form->addRow(QStringLiteral("Layout:"), layoutCombo);
		layout->addLayout(form);

		auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
		layout->addWidget(box);
		connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
		connect(browse, &QPushButton::clicked, &dlg,
		        [this, fontEdit]
		        {
			        const QString start = makeAbsolutePath(fontEdit->text().trimmed());
			        const QString path  = QFileDialog::getOpenFileName(
                        m_mainWindow, QStringLiteral("Select FIGlet Font"), start,
                        QStringLiteral("FIGlet Font (*.flf);;All Files (*)"));
			        if (path.isEmpty())
				        return;
			        fontEdit->setText(path);
		        });

		if (dlg.exec() != QDialog::Accepted)
			return;

		const auto text         = textEdit->toPlainText();
		const auto layoutMode   = layoutCombo->currentData().toInt();
		const auto selectedFont = fontEdit->text().trimmed();
		const auto resolvedFont = makeAbsolutePath(
		    selectedFont.isEmpty() ? getGlobalOption(QStringLiteral("AsciiArtFont")).toString()
		                           : selectedFont);

		QStringList renderedLines;
		QString     renderError;
		if (!QMudAsciiArt::render(text, resolvedFont, layoutMode, &renderedLines, &renderError))
		{
			QMessageBox::warning(m_mainWindow, QStringLiteral("ASCII Art"),
			                     renderError.isEmpty() ? QStringLiteral("Could not generate ASCII art.")
			                                           : renderError);
			return;
		}

		m_asciiArtText = text;
		if (layoutMode != savedLayout)
			setGlobalOptionInt(QStringLiteral("AsciiArtLayout"), layoutMode);
		if (selectedFont != savedFont)
			setGlobalOptionString(QStringLiteral("AsciiArtFont"), selectedFont);

		const auto output = renderedLines.join(QStringLiteral("\n")) + QStringLiteral("\n");
		if (auto *lineEdit = qobject_cast<QLineEdit *>(target))
			lineEdit->insert(output);
		else if (auto *plainEdit = qobject_cast<QPlainTextEdit *>(target))
			plainEdit->insertPlainText(output);
		else if (auto *richTextEdit = qobject_cast<QTextEdit *>(target))
			richTextEdit->insertPlainText(output);
	}
	else if (cmdName == QStringLiteral("TextGoTo") || cmdName == QStringLiteral("InsertDateTime") ||
	         cmdName == QStringLiteral("WordCount") || cmdName == QStringLiteral("SendToCommandWindow") ||
	         cmdName == QStringLiteral("SendToScript") || cmdName == QStringLiteral("SendToWorld") ||
	         cmdName == QStringLiteral("RefreshRecalledData"))
	{
		if (!m_mainWindow)
			return;
		TextChildWindow *textChild = m_mainWindow->activeTextChildWindow();
		QPlainTextEdit  *editor    = textChild ? textChild->editor() : nullptr;
		if (!textChild || !editor)
		{
			QMessageBox::information(
			    m_mainWindow, QStringLiteral("Notepad"),
			    QStringLiteral("This command is only available in a notepad/text window."));
			return;
		}

		auto selectedOrAll = [editor]() -> QString
		{
			if (const QTextCursor c = editor->textCursor(); c.hasSelection())
				return c.selectedText().replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
			return editor->toPlainText();
		};

		if (cmdName == QStringLiteral("TextGoTo"))
		{
			const int lineCount = qMax(1, editor->document()->blockCount());
			bool      ok        = false;
			int       line      = QInputDialog::getInt(m_mainWindow, QStringLiteral("Go To"),
			                                           QStringLiteral("Line number:"), 1, 1, lineCount, 1, &ok);
			if (!ok)
				return;
			QTextCursor cursor(editor->document());
			cursor.movePosition(QTextCursor::Start);
			for (int i = 1; i < line; ++i)
			{
				if (!cursor.movePosition(QTextCursor::NextBlock))
					break;
			}
			editor->setTextCursor(cursor);
			editor->setFocus();
			return;
		}

		if (cmdName == QStringLiteral("InsertDateTime"))
		{
			const QString stamp =
			    QDateTime::currentDateTime().toString(QStringLiteral("dddd, MMMM d, yyyy h:mm AP"));
			editor->insertPlainText(stamp);
			return;
		}

		if (cmdName == QStringLiteral("WordCount"))
		{
			const QString            text  = selectedOrAll();
			const qsizetype          chars = text.size();
			const qsizetype          lines = text.isEmpty() ? 0 : text.count(QLatin1Char('\n')) + 1;
			const QRegularExpression wordRx(QStringLiteral("\\S+"));
			int                      words = 0;
			auto                     it    = wordRx.globalMatch(text);
			while (it.hasNext())
			{
				it.next();
				++words;
			}
			QMessageBox::information(
			    m_mainWindow, QStringLiteral("Word Count"),
			    QStringLiteral("Lines: %1\nWords: %2\nCharacters: %3").arg(lines).arg(words).arg(chars));
			return;
		}

		if (cmdName == QStringLiteral("RefreshRecalledData"))
		{
			const QString path = textChild->filePath();
			if (path.trimmed().isEmpty() || !QFileInfo::exists(path))
			{
				QMessageBox::information(m_mainWindow, QStringLiteral("Refresh Recalled Data"),
				                         QStringLiteral("This text window is not backed by a file."));
				return;
			}
			QFile f(path);
			if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
			{
				QMessageBox::warning(m_mainWindow, QStringLiteral("Refresh Recalled Data"),
				                     QStringLiteral("Unable to read the source file."));
				return;
			}
			editor->setPlainText(QString::fromLocal8Bit(f.readAll()));
			if (editor->document())
				editor->document()->setModified(false);
			return;
		}

		WorldChildWindow *world   = m_mainWindow->activeWorldChildWindow();
		WorldRuntime     *runtime = world ? world->runtime() : nullptr;
		WorldView        *view    = world ? world->view() : nullptr;

		const QString     payload = selectedOrAll().trimmed();
		if (payload.isEmpty())
			return;

		if (cmdName == QStringLiteral("SendToCommandWindow"))
		{
			if (!view)
			{
				QMessageBox::information(m_mainWindow, QStringLiteral("Send To Command Window"),
				                         QStringLiteral("No active world window."));
				return;
			}
			view->setInputText(payload, true);
			view->focusInput();
			return;
		}

		if (cmdName == QStringLiteral("SendToScript"))
		{
			if (!runtime || !runtime->luaCallbacks())
			{
				QMessageBox::information(m_mainWindow, QStringLiteral("Send To Script"),
				                         QStringLiteral("No active world with Lua scripting available."));
				return;
			}
			runtime->setLastImmediateExpression(payload);
			if (!runtime->luaCallbacks()->executeScript(payload, QStringLiteral("Immediate")))
				QMessageBox::warning(m_mainWindow, QStringLiteral("Send To Script"),
				                     QStringLiteral("Script execution failed."));
			return;
		}

		if (cmdName == QStringLiteral("SendToWorld"))
		{
			if (!runtime)
			{
				QMessageBox::information(m_mainWindow, QStringLiteral("Send To World"),
				                         QStringLiteral("No active world window."));
				return;
			}
			const QStringList lines =
			    payload.split(QRegularExpression(QStringLiteral("\\r?\\n")), Qt::SkipEmptyParts);
			for (const QString &line : lines)
				runtime->sendText(line, true);
		}
	}
	else if (cmdName == QStringLiteral("ResetAllTimers"))
	{
		if (!m_mainWindow)
			return;
		WorldChildWindow *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		WorldRuntime *runtime = world->runtime();
		if (!runtime)
			return;
		runtime->resetAllTimers();
		m_mainWindow->showStatusMessage(QStringLiteral("Timers reset."), 2000);
	}
	else if (cmdName == QStringLiteral("ReloadScriptFile"))
	{
		if (!m_mainWindow)
			return;
		WorldChildWindow *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		WorldRuntime *runtime = world->runtime();
		if (!runtime)
			return;
		if (const QString language = runtime->worldAttributes().value(QStringLiteral("script_language"));
		    language.compare(QStringLiteral("Lua"), Qt::CaseInsensitive) != 0)
		{
			QMessageBox::information(m_mainWindow, QStringLiteral("Reload Script File"),
			                         QStringLiteral("Only Lua scripting is supported."));
			return;
		}

		QString       scriptText;
		const QString scriptFile =
		    runtime->worldAttributes().value(QStringLiteral("script_filename")).trimmed();
		if (!scriptFile.isEmpty())
		{
			const QString path = makeAbsolutePath(scriptFile);
			QFile         file(path);
			if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
			{
				QMessageBox::warning(m_mainWindow, QStringLiteral("Reload Script File"),
				                     QStringLiteral("Unable to open the script file."));
				return;
			}
			scriptText = QString::fromLocal8Bit(file.readAll());
		}
		else
		{
			scriptText = runtime->worldMultilineAttributes().value(QStringLiteral("script"));
		}

		runtime->setLuaScriptText(scriptText);
		runtime->setScriptFileChanged(false);
		if (LuaCallbackEngine *lua = runtime->luaCallbacks())
		{
			lua->resetState();
			int allowPackage = 1;
			if (AppController *app = instance())
				allowPackage = app->getGlobalOption(QStringLiteral("AllowLoadingDlls")).toInt();
			runtime->applyPackageRestrictions(allowPackage != 0);
			if (!lua->loadScript())
			{
				QMessageBox::warning(m_mainWindow, QStringLiteral("Reload Script File"),
				                     QStringLiteral("Failed to reload the script."));
				return;
			}
		}
		m_mainWindow->showStatusMessage(QStringLiteral("Script file reloaded."), 3000);
	}
	else if (QMudWorldPreferencesRouting::isPreferencesCommand(cmdName, isCommand))
	{
		if (!m_mainWindow)
			return;
		WorldChildWindow *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		WorldRuntime *runtime = world->runtime();
		WorldView    *view    = world->view();
		if (!runtime || !view)
			return;

		const WorldPreferencesDialog::Page page = QMudWorldPreferencesRouting::initialPageForCommand(
		    cmdName, runtime->lastPreferencesPage(), isCommand);

		WorldPreferencesDialog dlg(runtime, view, m_mainWindow);
		dlg.setInitialPage(page);
		if (dlg.exec() == QDialog::Accepted)
		{
			saveWorldSessionStateAsync(runtime, view, [](const bool, const QString &) {});
		}
		m_mainWindow->updateStatusBar();
		m_mainWindow->refreshActionState();
	}
	else if (cmdName == QStringLiteral("GlobalPreferences"))
	{
		GlobalPreferencesDialog dlg(m_mainWindow);
		dlg.exec();
	}
	else if (cmdName == QStringLiteral("CommandHistory"))
	{
		if (!m_mainWindow)
			return;
		if (WorldChildWindow *world = m_mainWindow->activeWorldChildWindow(); world)
		{
			if (WorldView *view = world->view(); view)
				view->showCommandHistoryDialog();
		}
	}
	else if (cmdName == QStringLiteral("ClearCommandHistory"))
	{
		if (!m_mainWindow)
			return;
		if (WorldChildWindow *world = m_mainWindow->activeWorldChildWindow(); world)
		{
			if (WorldView *view = world->view(); view)
			{
				view->clearCommandHistory();
				m_mainWindow->updateEditActions();
			}
		}
	}
	else if (cmdName == QStringLiteral("Find"))
		handleOutputFind(false, false, true);
	else if (cmdName == QStringLiteral("FindAgain"))
		handleOutputFind(true, false, true);
	else if (isCommand(QStringLiteral("FindAgainForwards")))
		handleOutputFind(true, true, true);
	else if (cmdName == QStringLiteral("FindAgainBackwards"))
		handleOutputFind(true, true, false);
	else if (cmdName == QStringLiteral("FreezeOutput"))
	{
		if (!m_mainWindow)
			return;
		if (WorldChildWindow *world = m_mainWindow->activeWorldChildWindow(); world)
		{
			if (WorldView *view = world->view(); view)
			{
				view->setFrozen(!view->isFrozen());
				m_mainWindow->updateStatusBar();
				m_mainWindow->refreshActionState();
			}
		}
	}
	else if (cmdName == QStringLiteral("GoToLine"))
	{
		if (!m_mainWindow)
			return;
		auto *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *view    = world->view();
		auto *runtime = world->runtime();
		if (!view || !runtime)
			return;
		int maxLine = static_cast<int>(runtime->lines().size());
		if (maxLine > 0 && runtime->lines().last().text.isEmpty())
			--maxLine;
		if (maxLine <= 0)
			return;
		int current = runtime->lastGoToLine();
		if (current < 1 || current > maxLine)
			current = qMin(maxLine, 1);
		bool      ok   = false;
		const int line = QInputDialog::getInt(m_mainWindow, QStringLiteral("Go To Line"),
		                                      QStringLiteral("Line number (1 - %1):").arg(maxLine), current,
		                                      1, maxLine, 1, &ok);
		if (!ok)
			return;
		view->selectOutputLine(line - 1);
		runtime->setLastGoToLine(line);
	}
	else if (cmdName == QStringLiteral("ActivityList"))
	{
		if (!m_mainWindow)
			return;
		ensureActivityDocument();
		if (!m_mainWindow->activateActivityWindow())
		{
			m_mainWindow->addMdiSubWindow(new ActivityChildWindow);
		}
	}
	else if (cmdName == QStringLiteral("RecallText"))
	{
		if (!m_mainWindow)
			return;
		auto *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *runtime = world->runtime();
		if (!runtime)
			return;
		if (auto *view = world->view(); !view)
			return;
		const auto     &lines      = runtime->lines();
		const qsizetype totalLines = lines.size();
		const int       maxLines   = qMax(1, static_cast<int>(totalLines));

		QDialog         dlg(m_mainWindow);
		dlg.setWindowTitle(QStringLiteral("Recall..."));
		auto *mainLayout = new QVBoxLayout(&dlg);
		auto *form       = new QFormLayout();

		auto *findCombo = new QComboBox(&dlg);
		findCombo->setEditable(true);
		if (!m_recallFindHistory.isEmpty())
		{
			findCombo->addItems(m_recallFindHistory);
			findCombo->setCurrentText(m_recallFindHistory.first());
		}
		form->addRow(QStringLiteral("Find what:"), findCombo);

		auto *matchCase = new QCheckBox(QStringLiteral("Match case"), &dlg);
		matchCase->setChecked(m_recallMatchCase);
		auto *useRegexp = new QCheckBox(QStringLiteral("Regular expression"), &dlg);
		useRegexp->setChecked(m_recallRegexp);
		auto *matchRow    = new QWidget(&dlg);
		auto *matchLayout = new QHBoxLayout(matchRow);
		matchLayout->setContentsMargins(0, 0, 0, 0);
		matchLayout->addWidget(matchCase);
		matchLayout->addWidget(useRegexp);
		matchLayout->addStretch();
		form->addRow(QString(), matchRow);

		auto *linesSpin = new QSpinBox(&dlg);
		linesSpin->setRange(1, maxLines);
		linesSpin->setValue(maxLines);
		form->addRow(QStringLiteral("Lines to search:"), linesSpin);

		const QString worldRecallPreamble =
		    runtime->worldAttributes().value(QStringLiteral("recall_line_preamble"));
		auto *preambleEdit = new QLineEdit(&dlg);
		preambleEdit->setText(worldRecallPreamble);
		form->addRow(QStringLiteral("Line preamble:"), preambleEdit);

		mainLayout->addLayout(form);

		auto *typeBox    = new QGroupBox(QStringLiteral("Line types"), &dlg);
		auto *typeLayout = new QHBoxLayout(typeBox);
		auto *commands   = new QCheckBox(QStringLiteral("Commands"), typeBox);
		auto *output     = new QCheckBox(QStringLiteral("Output"), typeBox);
		auto *notes      = new QCheckBox(QStringLiteral("Notes"), typeBox);
		commands->setChecked(m_recallCommands);
		output->setChecked(m_recallOutput);
		notes->setChecked(m_recallNotes);
		typeLayout->addWidget(commands);
		typeLayout->addWidget(output);
		typeLayout->addWidget(notes);
		typeLayout->addStretch();
		mainLayout->addWidget(typeBox);

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
		mainLayout->addWidget(buttons);
		connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

		if (dlg.exec() != QDialog::Accepted)
			return;

		const QString findText = findCombo->currentText();
		if (findText.isEmpty())
		{
			QMessageBox::warning(m_mainWindow, QStringLiteral("QMud"),
			                     QStringLiteral("The find text cannot be blank."));
			return;
		}

		QRegularExpression regex;
		if (useRegexp->isChecked())
		{
			QRegularExpression::PatternOptions opts = QRegularExpression::NoPatternOption;
			if (!matchCase->isChecked())
				opts |= QRegularExpression::CaseInsensitiveOption;
			regex = QRegularExpression(findText, opts);
			if (!regex.isValid())
			{
				QMessageBox::warning(
				    m_mainWindow, QStringLiteral("QMud"),
				    QStringLiteral("Cannot compile regular expression: %1").arg(regex.errorString()));
				return;
			}
		}

		const int linesToSearch = linesSpin->value();
		qsizetype startIndex    = 0;
		if (linesToSearch > 0 && static_cast<qsizetype>(linesToSearch) < totalLines)
			startIndex = totalLines - static_cast<qsizetype>(linesToSearch);

		const auto       preamble = preambleEdit->text();
		QString          result;
		const qsizetype  scanCount = totalLines - startIndex;
		QProgressDialog *progress  = nullptr;
		if (scanCount > 500)
		{
			progress =
			    new QProgressDialog(QStringLiteral("Recalling: %1").arg(findText), QStringLiteral("Cancel"),
			                        0, static_cast<int>(scanCount), m_mainWindow);
			progress->setWindowTitle(QStringLiteral("Recalling..."));
			progress->setWindowModality(Qt::WindowModal);
			progress->setMinimumDuration(0);
		}

		int milestone = 0;
		for (qsizetype i = startIndex; i < totalLines; ++i)
		{
			if (progress)
			{
				++milestone;
				if (milestone > 31)
				{
					progress->setValue(static_cast<int>(i - startIndex));
					milestone = 0;
					if (progress->wasCanceled())
						break;
				}
			}

			const auto &line = lines.at(i);
			if (constexpr int kCommentFlag = 0x01, kUserInputFlag = 0x02, kNoteOrCommandFlag = 0x03;
			    !(((line.flags & kUserInputFlag) != 0 && commands->isChecked()) ||
			      ((line.flags & kNoteOrCommandFlag) == 0 && output->isChecked()) ||
			      ((line.flags & kCommentFlag) != 0 && notes->isChecked())))
				continue;

			bool match = false;
			if (useRegexp->isChecked())
				match = regex.match(line.text).hasMatch();
			else
				match = line.text.contains(findText,
				                           matchCase->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive);

			if (!match)
				continue;

			if (!preamble.isEmpty())
				result += runtime->formatTime(line.time, preamble, false);
			result += line.text;
			result += QLatin1Char('\n');
		}

		if (progress)
		{
			progress->setValue(static_cast<int>(scanCount));
			delete progress;
		}

		if (result.isEmpty())
		{
			const auto findType =
			    useRegexp->isChecked() ? QStringLiteral("regular expression") : QStringLiteral("text");
			QMessageBox::information(m_mainWindow, QStringLiteral("QMud"),
			                         QStringLiteral("The %1 \"%2\" was not found.").arg(findType, findText));
			return;
		}

		auto history = m_recallFindHistory;
		history.removeAll(findText);
		history.prepend(findText);
		while (history.size() > 20)
			history.removeLast();
		m_recallFindHistory  = history;
		m_recallMatchCase    = matchCase->isChecked();
		m_recallRegexp       = useRegexp->isChecked();
		m_recallCommands     = commands->isChecked();
		m_recallOutput       = output->isChecked();
		m_recallNotes        = notes->isChecked();
		m_recallLinePreamble = preamble;
		runtime->setWorldAttribute(QStringLiteral("recall_line_preamble"), preamble);

		auto *child = new TextChildWindow(QStringLiteral("Recall: %1").arg(findText), result);
		m_mainWindow->addMdiSubWindow(child);
	}
	else if (isCommand(QStringLiteral("GoToUrl")) || cmdName == QStringLiteral("SendMailTo"))
	{
		if (!m_mainWindow)
			return;
		QString selection;
		if (auto *world = m_mainWindow->activeWorldChildWindow())
		{
			if (auto *view = world->view())
			{
				selection = view->outputSelectedText().trimmed();
				if (selection.isEmpty())
				{
					if (auto *input = view->inputEditor())
					{
						if (const QTextCursor cursor = input->textCursor(); cursor.hasSelection())
						{
							QString text = cursor.selectedText();
							text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
							selection = text.trimmed();
						}
					}
				}
			}
		}

		const bool    goToUrl = isCommand(QStringLiteral("GoToUrl"));
		const QString title   = goToUrl ? QStringLiteral("Go To URL") : QStringLiteral("Send Mail To");
		const QString label   = goToUrl ? QStringLiteral("URL:") : QStringLiteral("Email address:");
		bool          ok      = false;
		QString text = QInputDialog::getText(m_mainWindow, title, label, QLineEdit::Normal, selection, &ok);
		text         = text.trimmed();
		if (!ok || text.isEmpty())
			return;

		QUrl url;
		if (!goToUrl)
		{
			if (!text.startsWith(QStringLiteral("mailto:"), Qt::CaseInsensitive))
				text = QStringLiteral("mailto:%1").arg(text);
			url = QUrl(text);
		}
		else
		{
			url = QUrl::fromUserInput(text);
		}

		if (!url.isValid())
		{
			QMessageBox::warning(m_mainWindow, QStringLiteral("QMud"),
			                     QStringLiteral("The specified address is not valid."));
			return;
		}
		QDesktopServices::openUrl(url);
	}
	else if (cmdName == QStringLiteral("StopSoundPlaying"))
	{
		if (!m_mainWindow)
			return;
		auto *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *runtime = world->runtime();
		if (!runtime)
			return;
		runtime->stopSound(0);
	}
	else if (cmdName == QStringLiteral("MultiLineTrigger"))
	{
		if (!m_mainWindow)
			return;
		auto *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *runtime = world->runtime();
		if (!runtime)
			return;
		auto *view = world->view();
		if (!view)
			return;

		QDialog dlg(m_mainWindow);
		dlg.setWindowTitle(QStringLiteral("Multi-Line Trigger"));
		auto *layout    = new QVBoxLayout(&dlg);
		auto *label     = new QLabel(QStringLiteral("Trigger text:"), &dlg);
		auto *edit      = new QTextEdit(&dlg);
		auto *matchCase = new QCheckBox(QStringLiteral("Match case"), &dlg);
		matchCase->setChecked(true);

		if (QString selection = view->outputSelectedText(); !selection.isEmpty())
		{
			selection = selection.left(10000);
			selection.replace(QLatin1Char('\r'), QString());
			const auto convertToRegexp = [](const QString &text, const bool wholeLine,
			                                const bool makeAsterisksWildcards) -> QString
			{
				const auto input = text.toLatin1();
				QByteArray out;
				if (wholeLine)
					out.append('^');
				for (const unsigned char c : input)
				{
					if (c == '\n')
					{
						out.append("\\n");
					}
					else if (c < ' ')
					{
						out.append("\\x");
						appendHexByte(out, c);
					}
					else if (isAsciiAlnum(c) || c == ' ' || c >= 0x80)
					{
						out.append(static_cast<char>(c));
					}
					else if (c == '*' && makeAsterisksWildcards)
					{
						out.append("(.*?)");
					}
					else
					{
						out.append('\\');
						out.append(static_cast<char>(c));
					}
				}
				if (wholeLine)
					out.append('$');
				return QString::fromLatin1(out);
			};
			QString triggerText = convertToRegexp(selection, false, false);
			triggerText.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
			edit->setPlainText(triggerText);
		}

		const auto fixedFont = getGlobalOption(QStringLiteral("FixedPitchFont")).toString();
		if (const auto fixedSize = getGlobalOption(QStringLiteral("FixedPitchFontSize")).toInt();
		    !fixedFont.isEmpty() || fixedSize > 0)
			edit->setFont(qmudPreferredMonospaceFont(fixedFont, fixedSize));

		layout->addWidget(label);
		layout->addWidget(edit);
		layout->addWidget(matchCase);
		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
		layout->addWidget(buttons);
		connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

		if (dlg.exec() != QDialog::Accepted)
			return;

		QString triggerText = edit->toPlainText();
		if (triggerText.isEmpty())
		{
			QMessageBox::warning(m_mainWindow, QStringLiteral("QMud"),
			                     QStringLiteral("The trigger match text cannot be empty."));
			return;
		}

		triggerText.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
		triggerText.replace(QLatin1Char('\r'), QLatin1Char('\n'));
		const auto lines     = triggerText.split(QLatin1Char('\n'));
		const auto lineCount = lines.size();
		if (lineCount <= 1)
		{
			QMessageBox::warning(m_mainWindow, QStringLiteral("QMud"),
			                     QStringLiteral("Multi-line triggers must match at least 2 lines."));
			return;
		}

		if (constexpr int kMaxRecentLines = 200; lineCount > kMaxRecentLines)
		{
			QMessageBox::warning(
			    m_mainWindow, QStringLiteral("QMud"),
			    QStringLiteral("Multi-line triggers can match a maximum of %1 lines.").arg(kMaxRecentLines));
			return;
		}

		QString pattern = triggerText;
		pattern.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
		pattern += QStringLiteral("\\Z");

		QRegularExpression::PatternOptions opts = QRegularExpression::NoPatternOption;
		if (!matchCase->isChecked())
			opts |= QRegularExpression::CaseInsensitiveOption;
		if (QRegularExpression re(pattern, opts); !re.isValid())
		{
			QMessageBox::warning(
			    m_mainWindow, QStringLiteral("QMud"),
			    QStringLiteral("Cannot compile regular expression: %1").arg(re.errorString()));
			return;
		}

		auto                  triggers = runtime->triggers();
		WorldRuntime::Trigger trigger;
		auto                 &attrs = trigger.attributes;
		attrs.insert(QStringLiteral("name"),
		             QStringLiteral("*trigger%1").arg(WorldRuntime::getUniqueNumber()));
		attrs.insert(QStringLiteral("match"), pattern);
		attrs.insert(QStringLiteral("regexp"), QStringLiteral("1"));
		attrs.insert(QStringLiteral("multi_line"), QStringLiteral("1"));
		attrs.insert(QStringLiteral("lines_to_match"), QString::number(lineCount));
		attrs.insert(QStringLiteral("ignore_case"),
		             matchCase->isChecked() ? QStringLiteral("0") : QStringLiteral("1"));
		attrs.insert(QStringLiteral("keep_evaluating"), QStringLiteral("1"));
		attrs.insert(QStringLiteral("send_to"), QString::number(eSendToOutput));
		attrs.insert(QStringLiteral("group"), QStringLiteral("Multi Line"));
		trigger.children.insert(QStringLiteral("send"), QStringLiteral("%0"));
		triggers.push_back(trigger);
		runtime->setTriggers(triggers);
	}
	else if (cmdName == QStringLiteral("TextAttributes"))
	{
		if (!m_mainWindow)
			return;
		auto *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *runtime = world->runtime();
		if (!runtime)
			return;
		auto *view = world->view();
		if (!view)
			return;
		const auto &lines = runtime->lines();
		if (lines.isEmpty())
		{
			QMessageBox::information(m_mainWindow, QStringLiteral("QMud"),
			                         QStringLiteral("There is no output to inspect."));
			return;
		}

		int lineIndex = static_cast<int>(lines.size()) - 1;
		int column    = 1;
		if (view->hasOutputSelection())
		{
			lineIndex = view->outputSelectionStartLine() - 1;
			column    = view->outputSelectionStartColumn();
		}
		if (lineIndex < 0 || lineIndex >= lines.size())
			return;

		const auto                    &line         = lines.at(lineIndex);
		auto                           zeroBasedCol = qMax(1, column) - 1;
		WorldRuntime::StyleSpan        fallbackSpan;
		const WorldRuntime::StyleSpan *activeSpan = nullptr;

		auto                           parseColourValue = [](const QString &value) -> QColor
		{
			if (value.isEmpty())
				return {};
			if (const QColor color(value); color.isValid())
				return color;
			bool      ok      = false;
			const int numeric = value.toInt(&ok);
			if (!ok)
				return {};
			return {numeric & 0xFF, numeric >> 8 & 0xFF, numeric >> 16 & 0xFF};
		};

		QColor       defaultFore(192, 192, 192);
		QColor       defaultBack(0, 0, 0);
		const auto  &attrs   = runtime->worldAttributes();
		const QColor outFore = parseColourValue(attrs.value(QStringLiteral("output_text_colour")));
		const QColor outBack = parseColourValue(attrs.value(QStringLiteral("output_background_colour")));
		if (outFore.isValid())
			defaultFore = outFore;
		if (outBack.isValid())
			defaultBack = outBack;

		int spanOffset = 0;
		for (const auto &span : line.spans)
		{
			if (zeroBasedCol >= spanOffset && zeroBasedCol < spanOffset + span.length)
			{
				activeSpan = &span;
				break;
			}
			spanOffset += span.length;
		}
		if (!activeSpan)
		{
			fallbackSpan.length = static_cast<int>(line.text.size());
			fallbackSpan.fore   = defaultFore;
			fallbackSpan.back   = defaultBack;
			activeSpan          = &fallbackSpan;
		}

		QVector<QColor>  customText(MAX_CUSTOM, QColor(255, 255, 255));
		QVector<QColor>  customBack(MAX_CUSTOM, QColor(0, 0, 0));
		QVector<QString> customNames(MAX_CUSTOM);
		for (int i = 0; i < MAX_CUSTOM; ++i)
		{
			customNames[i] = QStringLiteral("Custom%1").arg(i + 1);
		}
		for (const auto &colours = runtime->colours(); const auto &[group, attributes] : colours)
		{
			if (!group.startsWith(QStringLiteral("custom/")) && group.toLower() != QStringLiteral("custom"))
				continue;
			bool      ok  = false;
			const int seq = attributes.value(QStringLiteral("seq")).toInt(&ok);
			if (!ok || seq < 1 || seq > MAX_CUSTOM)
				continue;
			const auto index      = seq - 1;
			const auto textColour = parseColourValue(attributes.value(QStringLiteral("text")));
			const auto backColour = parseColourValue(attributes.value(QStringLiteral("back")));
			if (textColour.isValid())
				customText[index] = textColour;
			if (backColour.isValid())
				customBack[index] = backColour;
			if (const auto nameValue = attributes.value(QStringLiteral("name")).trimmed();
			    !nameValue.isEmpty())
				customNames[index] = nameValue;
		}

		auto matchCustomIndex = [&](const QColor &fore, const QColor &back) -> int
		{
			for (int i = 0; i < customText.size(); ++i)
			{
				if (fore == customText[i] && back == customBack[i])
					return i;
			}
			return -1;
		};

		const auto matchAnsiIndex = [&](const QColor &color) -> int
		{
			if (!color.isValid())
				return -1;
			for (auto i = 0; i < 256; ++i)
			{
				if (const auto ref = xtermColorAt(i); color.red() == qmudRed(ref) &&
				                                      color.green() == qmudGreen(ref) &&
				                                      color.blue() == qmudBlue(ref))
					return i;
			}
			return -1;
		};

		auto colorToRgbString = [](const QColor &color) -> QString
		{ return QStringLiteral("R=%1, G=%2, B=%3").arg(color.red()).arg(color.green()).arg(color.blue()); };

		QString    textColour;
		QString    backColour;
		QString    customColour;
		const auto spanFore = activeSpan->fore.isValid() ? activeSpan->fore : defaultFore;
		const auto spanBack = activeSpan->back.isValid() ? activeSpan->back : defaultBack;

		if (const auto customIndex = matchCustomIndex(spanFore, spanBack); customIndex >= 0)
		{
			textColour   = QStringLiteral("Custom");
			backColour   = QStringLiteral("Custom");
			customColour = customNames.value(customIndex);
		}
		else
		{
			if (const auto foreAnsi = matchAnsiIndex(spanFore), backAnsi = matchAnsiIndex(spanBack);
			    foreAnsi >= 0 && backAnsi >= 0)
			{
				const char *sColours[] = {"Black", "Red",     "Green", "Yellow",
				                          "Blue",  "Magenta", "Cyan",  "White"};
				if (foreAnsi >= 8)
					textColour = colorToRgbString(spanFore);
				else
					textColour = QString::fromLatin1(sColours[foreAnsi & 7]);
				if (backAnsi >= 8)
					backColour = colorToRgbString(spanBack);
				else
					backColour = QString::fromLatin1(sColours[backAnsi & 7]);
				customColour = QStringLiteral("n/a");
			}
			else
			{
				textColour   = colorToRgbString(spanFore);
				backColour   = colorToRgbString(spanBack);
				customColour = QStringLiteral("RGB");
			}
		}

		const QString rgbText = QStringLiteral("#%1%2%3")
		                            .arg(spanFore.red(), 2, 16, QLatin1Char('0'))
		                            .arg(spanFore.green(), 2, 16, QLatin1Char('0'))
		                            .arg(spanFore.blue(), 2, 16, QLatin1Char('0'))
		                            .toUpper();
		const QString rgbBack = QStringLiteral("#%1%2%3")
		                            .arg(spanBack.red(), 2, 16, QLatin1Char('0'))
		                            .arg(spanBack.green(), 2, 16, QLatin1Char('0'))
		                            .arg(spanBack.blue(), 2, 16, QLatin1Char('0'))
		                            .toUpper();

		QString letter;
		if (zeroBasedCol >= 0 && zeroBasedCol < line.text.size())
			letter = line.text.mid(zeroBasedCol, 1);

		QDialog dlg(m_mainWindow);
		dlg.setWindowTitle(QStringLiteral("Text attributes"));
		auto *layout = new QVBoxLayout(&dlg);
		layout->addWidget(new QLabel(QStringLiteral("The start of the current selection is:"), &dlg));

		const auto readOnlyField = [&dlg](const QString &value)
		{
			auto *edit = new QLineEdit(value, &dlg);
			edit->setReadOnly(true);
			return edit;
		};

		auto *form = new QFormLayout();
		form->addRow(QStringLiteral("Letter:"), readOnlyField(letter));
		form->addRow(QStringLiteral("Text colour:"), readOnlyField(textColour));
		form->addRow(QStringLiteral("Background colour:"), readOnlyField(backColour));
		form->addRow(QStringLiteral("Custom colour:"), readOnlyField(customColour));
		layout->addLayout(form);

		auto *flags   = new QHBoxLayout();
		auto *bold    = new QCheckBox(QStringLiteral("Bold"), &dlg);
		auto *italic  = new QCheckBox(QStringLiteral("Italic"), &dlg);
		auto *inverse = new QCheckBox(QStringLiteral("Inverse"), &dlg);
		bold->setEnabled(false);
		italic->setEnabled(false);
		inverse->setEnabled(false);
		bold->setChecked(activeSpan->bold);
		italic->setChecked(activeSpan->italic);
		inverse->setChecked(activeSpan->inverse);
		flags->addWidget(bold);
		flags->addWidget(italic);
		flags->addWidget(inverse);
		flags->addStretch(1);
		layout->addLayout(flags);

		auto *modified = new QLabel(&dlg);
		if (activeSpan->changed)
			modified->setText(QStringLiteral("The colour or style HAS been modified by a trigger."));
		layout->addWidget(modified);

		auto *rgbGroup = new QGroupBox(QStringLiteral("RGB colour"), &dlg);
		auto *rgbGrid  = new QGridLayout(rgbGroup);
		rgbGrid->addWidget(new QLabel(QStringLiteral("Text:"), rgbGroup), 0, 0);
		auto *rgbTextEdit = readOnlyField(rgbText);
		rgbGrid->addWidget(rgbTextEdit, 0, 1);
		auto *textSwatch = new QPushButton(rgbGroup);
		textSwatch->setEnabled(false);
		textSwatch->setFixedSize(15, 15);
		textSwatch->setStyleSheet(QStringLiteral("background-color: %1;").arg(spanFore.name()));
		rgbGrid->addWidget(textSwatch, 0, 2);
		rgbGrid->addWidget(new QLabel(QStringLiteral("Background:"), rgbGroup), 1, 0);
		auto *rgbBackEdit = readOnlyField(rgbBack);
		rgbGrid->addWidget(rgbBackEdit, 1, 1);
		auto *backSwatch = new QPushButton(rgbGroup);
		backSwatch->setEnabled(false);
		backSwatch->setFixedSize(15, 15);
		backSwatch->setStyleSheet(QStringLiteral("background-color: %1;").arg(spanBack.name()));
		rgbGrid->addWidget(backSwatch, 1, 2);
		layout->addWidget(rgbGroup);

		auto *buttons  = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
		auto *lineInfo = new QPushButton(QStringLiteral("Line info..."), &dlg);
		buttons->addButton(lineInfo, QDialogButtonBox::ActionRole);
		connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
		connect(
		    lineInfo, &QPushButton::clicked, &dlg,
		    [this, &dlg, runtime, line, lineIndex, customText, customBack, customNames, matchAnsiIndex]
		    {
			    auto title = QStringLiteral("Line Information");
			    if (const auto worldName = runtime->worldAttributes().value(QStringLiteral("name"));
			        !worldName.isEmpty())
			    {
				    title = QStringLiteral("Line Information - %1").arg(worldName);
			    }

			    const auto yesNo = [](const bool value) -> QString
			    { return value ? QStringLiteral("YES") : QStringLiteral("no"); };
			    const auto rgbFromColorRef = [](const QMudColorRef ref) -> QString
			    {
				    return QStringLiteral("R=%1, G=%2, B=%3")
				        .arg(qmudRed(ref))
				        .arg(qmudGreen(ref))
				        .arg(qmudBlue(ref));
			    };

			    QString info;
			    info += QStringLiteral("Line %1, %2\n")
			                .arg(lineIndex + 1)
			                .arg(QLocale::system().toString(
			                    line.time, QStringLiteral("dddd, MMMM dd, yyyy, h:mm:ss AP")));
			    info += QStringLiteral(" Flags = Output: %1, Note: %2, User input: %3\n")
			                .arg(yesNo(line.flags & WorldRuntime::LineOutput))
			                .arg(yesNo(line.flags & WorldRuntime::LineNote))
			                .arg(yesNo(line.flags & WorldRuntime::LineInput));

			    const auto lastSpace = line.text.lastIndexOf(QLatin1Char(' '));
			    info +=
			        QStringLiteral(" Length = %1, last space = %2\n").arg(line.text.size()).arg(lastSpace);
			    info += QStringLiteral(" Text = \"%1\"\n\n").arg(line.text);

			    info += QStringLiteral("%1 style run%2\n\n")
			                .arg(line.spans.size())
			                .arg(line.spans.size() == 1 ? QString() : QStringLiteral("s"));

			    auto offset = 0;
			    auto count  = 1;
			    for (const auto &span : line.spans)
			    {
				    const QString spanText = line.text.mid(offset, span.length);
				    info += QStringLiteral("%1: Offset = %2, Length = %3, Text = \"%4\"\n")
				                .arg(count)
				                .arg(offset)
				                .arg(span.length)
				                .arg(spanText);

				    switch (span.actionType)
				    {
				    case WorldRuntime::ActionSend:
					    info += QStringLiteral(" Action - send to MUD: \"%1\"\n").arg(span.action);
					    if (!span.hint.isEmpty())
						    info += QStringLiteral(" Hint: \"%1\"\n").arg(span.hint);
					    break;
				    case WorldRuntime::ActionHyperlink:
					    info += QStringLiteral(" Action - hyperlink: \"%1\"\n").arg(span.action);
					    if (!span.hint.isEmpty())
						    info += QStringLiteral(" Hint: \"%1\"\n").arg(span.hint);
					    break;
				    case WorldRuntime::ActionPrompt:
					    info += QStringLiteral(" Action - send to command window: \"%1\"\n").arg(span.action);
					    if (!span.hint.isEmpty())
						    info += QStringLiteral(" Hint: \"%1\"\n").arg(span.hint);
					    break;
				    default:
					    info += QStringLiteral(" No action.\n");
					    break;
				    }

				    info += QStringLiteral(
				                " Flags = Hilite: %1, Underline: %2, Blink: %3, Inverse: %4, Changed: %5\n")
				                .arg(yesNo(span.bold))
				                .arg(yesNo(span.underline))
				                .arg(yesNo(span.italic))
				                .arg(yesNo(span.inverse))
				                .arg(yesNo(span.changed));

				    const auto fore               = span.fore.isValid() ? span.fore : QColor(192, 192, 192);
				    const auto back               = span.back.isValid() ? span.back : QColor(0, 0, 0);
				    const auto matchedCustomIndex = [&]
				    {
					    for (auto i = 0; i < customText.size(); ++i)
						    if (fore == customText[i] && back == customBack[i])
							    return i;
					    return -1;
				    }();
				    if (matchedCustomIndex >= 0)
				    {
					    info += QStringLiteral(" Custom colour: %1 (%2)\n")
					                .arg(matchedCustomIndex)
					                .arg(customNames.value(matchedCustomIndex));
				    }
				    else
				    {
					    const auto foreAnsi = matchAnsiIndex(fore);
					    const auto backAnsi = matchAnsiIndex(back);
					    if (foreAnsi >= 0)
					    {
						    if (foreAnsi >= 8)
							    info += QStringLiteral(" Foreground colour 256-ANSI   : %1\n")
							                .arg(rgbFromColorRef(xtermColorAt(foreAnsi)));
						    else
						    {
							    static const char *sColours[] = {"Black", "Red",     "Green", "Yellow",
							                                     "Blue",  "Magenta", "Cyan",  "White"};
							    info += QStringLiteral(" Foreground colour ANSI  : %1 (%2)\n")
							                .arg(foreAnsi)
							                .arg(QString::fromLatin1(sColours[foreAnsi & 7]));
						    }
					    }
					    if (backAnsi >= 0)
					    {
						    if (backAnsi >= 8)
							    info += QStringLiteral(" Background colour 256-ANSI   : %1\n")
							                .arg(rgbFromColorRef(xtermColorAt(backAnsi)));
						    else
						    {
							    static const char *sColours[] = {"Black", "Red",     "Green", "Yellow",
							                                     "Blue",  "Magenta", "Cyan",  "White"};
							    info += QStringLiteral(" Background colour ANSI  : %1 (%2)\n")
							                .arg(backAnsi)
							                .arg(QString::fromLatin1(sColours[backAnsi & 7]));
						    }
					    }
					    if (foreAnsi < 0 && backAnsi < 0)
					    {
						    info += QStringLiteral(" Foreground colour RGB   : %1\n")
						                .arg(rgbFromColorRef(qmudRgb(fore.red(), fore.green(), fore.blue())));
						    info += QStringLiteral(" Background colour RGB   : %1\n")
						                .arg(rgbFromColorRef(qmudRgb(back.red(), back.green(), back.blue())));
					    }
				    }

				    info += QLatin1Char('\n');
				    offset += span.length;
				    ++count;
			    }

			    info += QStringLiteral("%1 column%2 in %3 style run%4\n")
			                .arg(offset)
			                .arg(offset == 1 ? QString() : QStringLiteral("s"))
			                .arg(line.spans.size())
			                .arg(line.spans.size() == 1 ? QString() : QStringLiteral("s"));
			    if (offset != line.text.size())
				    info += QStringLiteral("** WARNING - length discrepancy **\n");
			    info += QStringLiteral("\n------ (end line information) ------\n");

			    (void)appendToNotepad(title, info, false);
			    dlg.reject();
		    });
		layout->addWidget(buttons);
		dlg.exec();
	}
	else if (cmdName == QStringLiteral("NoCommandEcho"))
	{
		if (!m_mainWindow)
			return;
		auto *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *runtime = world->runtime();
		if (!runtime)
			return;
		const bool enabled = !runtime->noCommandEcho();
		runtime->setNoCommandEcho(enabled);
		if (QAction *action = m_mainWindow->actionForCommand(QStringLiteral("NoCommandEcho")))
			action->setChecked(enabled);
	}
	else if (cmdName == QStringLiteral("ChatSessions"))
	{
		if (!m_mainWindow)
			return;
		auto *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *runtime = world->runtime();
		if (!runtime)
			return;

		QDialog dlg(m_mainWindow);
		dlg.setWindowTitle(QStringLiteral("Chat Sessions"));
		auto *layout = new QVBoxLayout(&dlg);
		auto *table  = new QTableWidget(&dlg);
		table->setColumnCount(7);
		table->setHorizontalHeaderLabels(
		    {QStringLiteral("ID"), QStringLiteral("Name"), QStringLiteral("Address"), QStringLiteral("Port"),
		     QStringLiteral("Status"), QStringLiteral("Type"), QStringLiteral("Incoming")});
		table->horizontalHeader()->setStretchLastSection(true);
		table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
		table->setSelectionBehavior(QAbstractItemView::SelectRows);
		table->setSelectionMode(QAbstractItemView::SingleSelection);
		layout->addWidget(table);

		const auto statusText = [](const int status) -> QString
		{
			switch (status)
			{
			case 0:
				return QStringLiteral("Closed");
			case 1:
				return QStringLiteral("Connecting");
			case 2:
				return QStringLiteral("Awaiting confirm");
			case 3:
				return QStringLiteral("Awaiting request");
			case 4:
				return QStringLiteral("Connected");
			default:
				return QStringLiteral("Unknown");
			}
		};

		const auto typeText = [](const int type) -> QString
		{
			switch (type)
			{
			case 0:
				return QStringLiteral("MudMaster");
			case 1:
				return QStringLiteral("zChat");
			default:
				return QStringLiteral("Unknown");
			}
		};

		const auto populate = [table, runtime, statusText, typeText]
		{
			const auto ids = runtime->chatList();
			table->setRowCount(static_cast<int>(ids.size()));
			auto row = 0;
			for (const auto id : ids)
			{
				const auto name     = runtime->chatInfo(id, 2).toString();
				const auto address  = runtime->chatInfo(id, 6).toString();
				const auto port     = runtime->chatInfo(id, 7).toInt();
				const auto status   = runtime->chatInfo(id, 9).toInt();
				const auto type     = runtime->chatInfo(id, 10).toInt();
				const auto incoming = runtime->chatInfo(id, 12).toBool();
				table->setItem(row, 0, new QTableWidgetItem(QString::number(id)));
				table->setItem(row, 1, new QTableWidgetItem(name));
				table->setItem(row, 2, new QTableWidgetItem(address));
				table->setItem(row, 3, new QTableWidgetItem(QString::number(port)));
				table->setItem(row, 4, new QTableWidgetItem(statusText(status)));
				table->setItem(row, 5, new QTableWidgetItem(typeText(type)));
				table->setItem(row, 6,
				               new QTableWidgetItem(incoming ? QStringLiteral("Yes") : QStringLiteral("No")));
				++row;
			}
		};

		populate();

		auto *buttons    = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
		auto *disconnect = new QPushButton(QStringLiteral("Disconnect"), &dlg);
		buttons->addButton(disconnect, QDialogButtonBox::ActionRole);
		connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
		connect(disconnect, &QPushButton::clicked, &dlg,
		        [table, runtime, populate]
		        {
			        const auto ranges = table->selectedRanges();
			        if (ranges.isEmpty())
				        return;
			        const auto  row  = ranges.first().topRow();
			        const auto *item = table->item(row, 0);
			        if (!item)
				        return;
			        bool       ok = false;
			        const auto id = item->text().toLong(&ok);
			        if (!ok)
				        return;
			        runtime->chatDisconnect(id);
			        populate();
		        });
		layout->addWidget(buttons);
		dlg.exec();
	}
	else if (cmdName == QStringLiteral("TestTrigger"))
	{
		if (!m_mainWindow)
			return;
		auto *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *runtime = world->runtime();
		if (!runtime)
			return;
		auto *view = world->view();
		if (!view)
			return;

		QDialog dlg(m_mainWindow);
		dlg.setWindowTitle(QStringLiteral("Test Trigger Evaluation"));
		auto *layout = new QVBoxLayout(&dlg);
		layout->addWidget(new QLabel(QStringLiteral("Test text:"), &dlg));
		auto *edit = new QTextEdit(&dlg);
		if (auto selection = view->outputSelectedText(); !selection.isEmpty())
			edit->setPlainText(selection);
		layout->addWidget(edit);
		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
		layout->addWidget(buttons);
		connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

		if (dlg.exec() != QDialog::Accepted)
			return;
		const auto text = edit->toPlainText();
		if (text.isEmpty())
			return;

		QString report;
		report += QStringLiteral("Tested %1 triggers against text:\n\"%2\"\n\n")
		              .arg(runtime->triggers().size())
		              .arg(text);

		auto matchCount = 0;
		for (const auto &trigger : runtime->triggers())
		{
			const auto &attrs = trigger.attributes;
			if (!isEnabledFlag(attrs.value(QStringLiteral("enabled"))))
				continue;
			const auto matchText = attrs.value(QStringLiteral("match"));
			if (matchText.isEmpty())
				continue;
			const auto isRegexp    = isEnabledFlag(attrs.value(QStringLiteral("regexp")));
			const auto ignoreCase  = isEnabledFlag(attrs.value(QStringLiteral("ignore_case")));
			const auto isMultiLine = isEnabledFlag(attrs.value(QStringLiteral("multi_line")));
			QString    pattern     = matchText;
			if (!isRegexp)
				pattern = WorldRuntime::makeRegularExpression(matchText);

			QRegularExpression::PatternOptions opts = QRegularExpression::NoPatternOption;
			if (ignoreCase)
				opts |= QRegularExpression::CaseInsensitiveOption;
			if (isMultiLine)
				opts |= QRegularExpression::DotMatchesEverythingOption;

			const QRegularExpression re(pattern, opts);
			if (!re.isValid())
				continue;
			if (!re.match(text).hasMatch())
				continue;

			const auto name  = attrs.value(QStringLiteral("name"));
			const auto group = attrs.value(QStringLiteral("group"));
			report += QStringLiteral("Matched: %1  Group: %2  Pattern: %3\n")
			              .arg(name.isEmpty() ? QStringLiteral("(unnamed)") : name,
			                   group.isEmpty() ? QStringLiteral("(none)") : group, pattern);
			++matchCount;
		}

		if (matchCount == 0)
			report += QStringLiteral("No triggers matched.\n");

		(void)appendToNotepad(QStringLiteral("Trigger Test"), report, true);
	}
	else if (cmdName == QStringLiteral("Immediate"))
	{
		if (!m_mainWindow)
			return;
		auto *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *runtime = world->runtime();
		if (!runtime)
			return;
		const auto &attrs         = runtime->worldAttributes();
		const auto  enableScripts = attrs.value(QStringLiteral("enable_scripts"));
		const auto  language      = attrs.value(QStringLiteral("script_language"));
		if (const auto scriptingEnabled = isEnabledFlag(enableScripts);
		    !scriptingEnabled || language.compare(QStringLiteral("Lua"), Qt::CaseInsensitive) != 0)
		{
			QMessageBox::information(m_mainWindow, QStringLiteral("Immediate"),
			                         QStringLiteral("Lua scripting is not enabled for this world."));
			return;
		}
		auto *lua = runtime->luaCallbacks();
		if (!lua)
			return;

		QDialog dlg(m_mainWindow);
		dlg.setWindowTitle(QStringLiteral("Immediate"));
		QVBoxLayout      layout(&dlg);
		QPlainTextEdit   edit(&dlg);
		QDialogButtonBox buttons(QDialogButtonBox::Close, &dlg);
		QPushButton      runButton(QStringLiteral("Run"), &dlg);
		QPushButton      editButton(QStringLiteral("Edit..."), &dlg);
		edit.setPlainText(runtime->lastImmediateExpression());
		WorldEditUtils::applyEditorPreferences(&edit);
		auto cursor = edit.textCursor();
		cursor.movePosition(QTextCursor::End);
		edit.setTextCursor(cursor);
		layout.addWidget(&edit);
		buttons.addButton(&runButton, QDialogButtonBox::AcceptRole);
		buttons.addButton(&editButton, QDialogButtonBox::ActionRole);
		layout.addWidget(&buttons);
		connect(&buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
		connect(&editButton, &QPushButton::clicked, &dlg,
		        [this, &edit]
		        {
			        QDialog editDlg(m_mainWindow);
			        editDlg.setWindowTitle(QStringLiteral("Edit immediate expression"));
			        QVBoxLayout      editLayout(&editDlg);
			        QPlainTextEdit   editor(&editDlg);
			        QDialogButtonBox editButtons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &editDlg);
			        editor.setPlainText(edit.toPlainText());
			        WorldEditUtils::applyEditorPreferences(&editor);
			        editLayout.addWidget(&editor);
			        connect(&editButtons, &QDialogButtonBox::accepted, &editDlg, &QDialog::accept);
			        connect(&editButtons, &QDialogButtonBox::rejected, &editDlg, &QDialog::reject);
			        editLayout.addWidget(&editButtons);
			        if (editDlg.exec() != QDialog::Accepted)
				        return;
			        edit.setPlainText(editor.toPlainText());
		        });
		connect(&runButton, &QPushButton::clicked, &dlg,
		        [this, runtime, lua, &edit]
		        {
			        const auto code = edit.toPlainText();
			        runtime->setLastImmediateExpression(code);
			        if (code.trimmed().isEmpty())
				        return;
			        if (m_mainWindow)
				        m_mainWindow->setStatusMessageNow(QStringLiteral("Executing immediate script"));
			        if (!lua->executeScript(code, QStringLiteral("Immediate")))
				        QMessageBox::warning(m_mainWindow, QStringLiteral("QMud"),
				                             QStringLiteral("Immediate execution failed."));
			        if (m_mainWindow)
				        m_mainWindow->setStatusMessage(QStringLiteral("Ready"));
		        });
		dlg.exec();
		if (const auto lastText = edit.toPlainText(); !lastText.trimmed().isEmpty())
			runtime->setLastImmediateExpression(lastText);
	}
	else if (cmdName == QStringLiteral("EditScriptFile"))
	{
		if (!m_mainWindow)
			return;
		auto *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *runtime = world->runtime();
		if (!runtime)
			return;
		const auto &attrs      = runtime->worldAttributes();
		const auto  scriptFile = attrs.value(QStringLiteral("script_filename")).trimmed();
		if (scriptFile.isEmpty())
		{
			QMessageBox::information(m_mainWindow, QStringLiteral("Edit Script File"),
			                         QStringLiteral("No script file has been specified for this world."));
			return;
		}

		const auto path = makeAbsolutePath(scriptFile);
		if (!QFileInfo::exists(path))
		{
			QMessageBox::warning(m_mainWindow, QStringLiteral("Edit Script File"),
			                     QStringLiteral("Unable to open the script file."));
			return;
		}

		const auto editWithNotepad  = attrs.value(QStringLiteral("edit_script_with_notepad"));
		const auto editorWindowName = attrs.value(QStringLiteral("editor_window_name")).trimmed();
		const auto tryRaiseConfiguredEditorWindow = [&]
		{
			if (editorWindowName.isEmpty())
				return;
			if (m_mainWindow && m_mainWindow->activateNotepad(editorWindowName))
				return;
			bringOwnedWindowToFrontByTitle(editorWindowName);
		};
		if (const auto useNotepad = isEnabledFlag(editWithNotepad); useNotepad)
		{
			(void)openTextDocument(path);
			tryRaiseConfiguredEditorWindow();
			return;
		}

		QString editor = attrs.value(QStringLiteral("script_editor")).trimmed();
		QString args   = attrs.value(QStringLiteral("script_editor_argument")).trimmed();
		if (args.isEmpty())
			args = QStringLiteral("\"%file\"");
		args.replace(QStringLiteral("%file"), path);
		if (!editor.isEmpty())
		{
			if (const auto splitArgs = QProcess::splitCommand(args);
			    QProcess::startDetached(editor, splitArgs))
			{
				tryRaiseConfiguredEditorWindow();
				return;
			}
		}
		if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
			QMessageBox::warning(m_mainWindow, QStringLiteral("Edit Script File"),
			                     QStringLiteral("Unable to open the script file."));
		else
			tryRaiseConfiguredEditorWindow();
	}
	else if (cmdName == QStringLiteral("Trace"))
	{
		if (!m_mainWindow)
			return;
		auto *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *runtime = world->runtime();
		if (!runtime)
			return;
		const auto wasEnabled = runtime->traceEnabled();
		if (wasEnabled)
			runtime->outputText(QStringLiteral("Trace off"), true, true);
		runtime->setTraceEnabled(!wasEnabled);
		if (!wasEnabled)
			runtime->outputText(QStringLiteral("Trace on"), true, true);
		if (QAction *action = m_mainWindow->actionForCommand(QStringLiteral("Trace")))
		{
			action->setCheckable(true);
			action->setChecked(!wasEnabled);
		}
	}
	else if (cmdName == QStringLiteral("SendToAllWorlds"))
	{
		if (!m_mainWindow)
			return;
		const auto entries = m_mainWindow->worldWindowDescriptors();

		QDialog    dlg(m_mainWindow);
		dlg.setWindowTitle(QStringLiteral("Send To All Worlds"));
		auto *layout    = new QVBoxLayout(&dlg);
		auto *listLabel = new QLabel(QStringLiteral("Send to:"), &dlg);
		layout->addWidget(listLabel);
		auto *list = new QListWidget(&dlg);
		list->setSelectionMode(QAbstractItemView::NoSelection);
		layout->addWidget(list);

		for (const auto &[sequence, window, runtime] : entries)
		{
			if (!runtime)
				continue;
			if (!runtime->isConnected())
				continue;
			auto name = runtime->worldAttributes().value(QStringLiteral("name")).trimmed();
			if (name.isEmpty() && window)
				name = window->windowTitle();
			if (name.isEmpty())
				name = QStringLiteral("World %1").arg(sequence);
			const auto label = QStringLiteral("%1: %2").arg(sequence).arg(name);
			auto      *item  = new QListWidgetItem(label, list);
			item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
			item->setCheckState(Qt::Checked);
			item->setData(Qt::UserRole, QVariant::fromValue(static_cast<QObject *>(runtime)));
		}

		auto *selectLayout = new QHBoxLayout();
		auto *selectAll    = new QPushButton(QStringLiteral("Select All"), &dlg);
		auto *selectNone   = new QPushButton(QStringLiteral("Select None"), &dlg);
		selectLayout->addWidget(selectAll);
		selectLayout->addWidget(selectNone);
		selectLayout->addStretch(1);
		layout->addLayout(selectLayout);

		auto *echoCheck = new QCheckBox(QStringLiteral("Echo in output window"), &dlg);
		echoCheck->setChecked(m_echoSendToAll);
		layout->addWidget(echoCheck);

		auto *messageLabel = new QLabel(QStringLiteral("Message:"), &dlg);
		layout->addWidget(messageLabel);
		auto *messageEdit = new QTextEdit(&dlg);
		messageEdit->setPlainText(m_lastSendToAllWorlds);
		messageEdit->setMinimumHeight(90);
		layout->addWidget(messageEdit);

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
		layout->addWidget(buttons);
		auto *okButton = buttons->button(QDialogButtonBox::Ok);
		connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

		const auto selectedCount = [list]() -> int
		{
			auto count = 0;
			for (auto i = 0; i < list->count(); ++i)
			{
				if (const auto *item = list->item(i); item && item->checkState() == Qt::Checked)
					++count;
			}
			return count;
		};

		const auto updateOk = [okButton, selectedCount, messageEdit]
		{
			if (!okButton)
				return;
			const bool ready = selectedCount() > 0 && !messageEdit->toPlainText().trimmed().isEmpty();
			okButton->setEnabled(ready);
		};

		connect(selectAll, &QPushButton::clicked, &dlg,
		        [list, updateOk]
		        {
			        for (auto i = 0; i < list->count(); ++i)
			        {
				        if (auto *item = list->item(i))
					        item->setCheckState(Qt::Checked);
			        }
			        updateOk();
		        });
		connect(selectNone, &QPushButton::clicked, &dlg,
		        [list, updateOk]
		        {
			        for (auto i = 0; i < list->count(); ++i)
			        {
				        if (auto *item = list->item(i))
					        item->setCheckState(Qt::Unchecked);
			        }
			        updateOk();
		        });
		connect(list, &QListWidget::itemChanged, &dlg, [updateOk](QListWidgetItem *) { updateOk(); });
		connect(messageEdit, &QTextEdit::textChanged, &dlg, updateOk);
		updateOk();

		if (dlg.exec() != QDialog::Accepted)
			return;

		auto text = messageEdit->toPlainText();
		if (text.isEmpty())
			return;
		text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
		text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
		text.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
		if (!text.endsWith(QStringLiteral("\r\n")))
			text += QStringLiteral("\r\n");
		m_lastSendToAllWorlds = messageEdit->toPlainText();
		m_echoSendToAll       = echoCheck->isChecked();

		auto sentCount = 0;
		for (auto i = 0; i < list->count(); ++i)
		{
			const auto *item = list->item(i);
			if (!item || item->checkState() != Qt::Checked)
				continue;
			auto *runtime = qobject_cast<WorldRuntime *>(item->data(Qt::UserRole).value<QObject *>());
			if (!runtime)
				continue;
			const auto &attrs    = runtime->worldAttributes();
			const auto  logInput = isEnabledFlag(attrs.value(QStringLiteral("log_input")));
			runtime->setCurrentActionSource(WorldRuntime::eUserMenuAction);
			if (runtime->sendCommand(text, m_echoSendToAll, false, logInput, true, false) == eOK)
				++sentCount;
			runtime->setCurrentActionSource(WorldRuntime::eUnknownActionSource);
		}

		m_mainWindow->showStatusMessage(QStringLiteral("Sent to %1 world%2.")
		                                    .arg(sentCount)
		                                    .arg(sentCount == 1 ? QString() : QStringLiteral("s")),
		                                2000);
	}
	else if (cmdName == QStringLiteral("Mapper"))
	{
		if (!m_mainWindow)
			return;
		auto *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *runtime = world->runtime();
		if (!runtime)
			return;

		const auto  wasMapping         = runtime->isMapping();
		const auto  initialCount       = runtime->mappingCount();
		const auto &attrs              = runtime->worldAttributes();
		const auto  originalMapFailure = attrs.value(QStringLiteral("mapping_failure"));
		const auto  originalMapFailureRegexp =
		    isEnabledFlag(attrs.value(QStringLiteral("map_failure_regexp")));

		QDialog dlg(m_mainWindow);
		dlg.setWindowTitle(QStringLiteral("Mapper"));
		auto *layout = new QVBoxLayout(&dlg);
		auto *enable = new QCheckBox(QStringLiteral("Enable mapping"), &dlg);
		enable->setChecked(wasMapping || initialCount == 0);
		layout->addWidget(enable);
		auto *removeReverses = new QCheckBox(QStringLiteral("Remove map reverses"), &dlg);
		removeReverses->setChecked(runtime->removeMapReverses());
		layout->addWidget(removeReverses);
		auto *failureLabel = new QLabel(QStringLiteral("Mapping failure text:"), &dlg);
		auto *failureEdit  = new QLineEdit(originalMapFailure, &dlg);
		auto *failureRegexp =
		    new QCheckBox(QStringLiteral("Treat mapping failure as regular expression"), &dlg);
		failureRegexp->setChecked(originalMapFailureRegexp);
		layout->addWidget(failureLabel);
		layout->addWidget(failureEdit);
		layout->addWidget(failureRegexp);
		auto *countLabel = new QLabel(QStringLiteral("Map items: %1").arg(initialCount), &dlg);
		layout->addWidget(countLabel);
		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
		layout->addWidget(buttons);
		connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
		if (dlg.exec() != QDialog::Accepted)
		{
			if (!wasMapping && initialCount == 0)
			{
				if (QMessageBox::question(
				        m_mainWindow, QStringLiteral("Mapper"),
				        QStringLiteral(
				            "Warning - mapping has not been turned on because you pressed \"Cancel\".\n\n"
				            "Do you want mapping enabled now?"),
				        QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes)
					runtime->setMappingEnabled(true);
			}
			return;
		}

		const auto updatedMapFailure       = failureEdit->text();
		const auto updatedMapFailureRegexp = failureRegexp->isChecked();
		if (updatedMapFailureRegexp && !updatedMapFailure.isEmpty())
		{
			if (const QRegularExpression test(updatedMapFailure); !test.isValid())
			{
				QMessageBox::warning(
				    m_mainWindow, QStringLiteral("Mapper"),
				    QStringLiteral("Invalid mapping failure regular expression: %1").arg(test.errorString()));
				return;
			}
		}

		runtime->setMappingEnabled(enable->isChecked());
		runtime->setRemoveMapReverses(removeReverses->isChecked());
		runtime->setWorldAttribute(QStringLiteral("mapping_failure"), updatedMapFailure);
		runtime->setWorldAttribute(QStringLiteral("map_failure_regexp"),
		                           updatedMapFailureRegexp ? QStringLiteral("1") : QStringLiteral("0"));
		if (updatedMapFailure != originalMapFailure || updatedMapFailureRegexp != originalMapFailureRegexp)
		{
			runtime->setWorldFileModified(true);
		}
		if (QAction *action = m_mainWindow->actionForCommand(QStringLiteral("Mapper")))
		{
			action->setCheckable(true);
			action->setChecked(runtime->isMapping());
		}
		m_mainWindow->showStatusMessage(runtime->isMapping() ? QStringLiteral("Mapping enabled.")
		                                                     : QStringLiteral("Mapping disabled."),
		                                2000);
	}
	else if (isCommand(QStringLiteral("MapperSpecial")))
	{
		if (!m_mainWindow)
			return;
		auto *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *runtime = world->runtime();
		if (!runtime)
			return;
		if (!runtime->isMapping())
		{
			QMessageBox::information(m_mainWindow, QStringLiteral("Mapper"),
			                         QStringLiteral("Mapping is not enabled."));
			return;
		}

		QDialog dlg(m_mainWindow);
		dlg.setWindowTitle(QStringLiteral("Mapper Special Move"));
		auto *layout       = new QGridLayout(&dlg);
		auto *actionLabel  = new QLabel(QStringLiteral("Action:"), &dlg);
		auto *reverseLabel = new QLabel(QStringLiteral("Reverse:"), &dlg);
		auto *actionEdit   = new QLineEdit(m_lastMapperSpecialAction, &dlg);
		auto *reverseEdit  = new QLineEdit(m_lastMapperSpecialReverse, &dlg);
		auto *sendToMud    = new QCheckBox(QStringLiteral("Send to MUD"), &dlg);
		sendToMud->setChecked(true);
		layout->addWidget(actionLabel, 0, 0);
		layout->addWidget(actionEdit, 0, 1);
		layout->addWidget(reverseLabel, 1, 0);
		layout->addWidget(reverseEdit, 1, 1);
		layout->addWidget(sendToMud, 2, 0, 1, 2);
		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
		layout->addWidget(buttons, 3, 0, 1, 2);
		connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
		if (dlg.exec() != QDialog::Accepted)
			return;

		m_lastMapperSpecialAction  = actionEdit->text();
		m_lastMapperSpecialReverse = reverseEdit->text();
		const auto &action         = m_lastMapperSpecialAction;
		const auto &reverse        = m_lastMapperSpecialReverse;

		if (sendToMud->isChecked() && !action.trimmed().isEmpty())
		{
			const auto &attrs    = runtime->worldAttributes();
			const auto  echo     = isEnabledFlag(attrs.value(QStringLiteral("display_my_input")));
			const auto  logInput = isEnabledFlag(attrs.value(QStringLiteral("log_input")));
			runtime->setCurrentActionSource(WorldRuntime::eUserMenuAction);
			(void)runtime->sendCommand(action, echo, false, logInput, true, false);
			runtime->setCurrentActionSource(WorldRuntime::eUnknownActionSource);
		}

		if (const auto result = runtime->addToMapper(action, reverse); result != eOK)
		{
			const auto error = WorldRuntime::errorDesc(result);
			QMessageBox::warning(m_mainWindow, QStringLiteral("Mapper"),
			                     error.isEmpty() ? QStringLiteral("Invalid mapper item.") : error);
			return;
		}

		auto summary = runtime->mappingString(true).trimmed();
		if (summary.size() > 50)
			summary = summary.left(50) + QStringLiteral(" ...");
		if (!summary.isEmpty())
			m_mainWindow->showStatusMessage(QStringLiteral("Mapper: %1").arg(summary), 2000);
	}
	else if (isCommand(QStringLiteral("MapperComment")))
	{
		if (!m_mainWindow)
			return;
		auto *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *runtime = world->runtime();
		if (!runtime)
			return;
		if (!runtime->isMapping())
		{
			QMessageBox::information(m_mainWindow, QStringLiteral("Mapper"),
			                         QStringLiteral("Mapping is not enabled."));
			return;
		}

		bool       ok = false;
		const auto comment =
		    QInputDialog::getText(m_mainWindow, QStringLiteral("Mapper Comment"), QStringLiteral("Comment:"),
		                          QLineEdit::Normal, QString(), &ok);
		if (!ok || comment.trimmed().isEmpty())
			return;
		if (const auto result = runtime->addMapperComment(comment.trimmed()); result != eOK)
		{
			const auto error = WorldRuntime::errorDesc(result);
			QMessageBox::warning(m_mainWindow, QStringLiteral("Mapper"),
			                     error.isEmpty() ? QStringLiteral("Invalid mapper comment.") : error);
			return;
		}

		auto summary = runtime->mappingString(true).trimmed();
		if (summary.size() > 50)
			summary = summary.left(50) + QStringLiteral(" ...");
		if (!summary.isEmpty())
			m_mainWindow->showStatusMessage(QStringLiteral("Mapper: %1").arg(summary), 2000);
	}
	else if (cmdName == QStringLiteral("NewWindow"))
	{
		if (!m_mainWindow)
			return;
		auto *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *runtime = world->runtime();
		if (!runtime)
			return;

		auto *window = new WorldChildWindow(world->windowTitle());
		window->setRuntimeObserver(runtime);
		if (auto *view = window->view())
			view->restoreOutputFromPersistedLines(runtime->lines());
		m_mainWindow->addMdiSubWindow(window);
		if (world->isMaximized())
			window->showMaximized();
		else
			window->show();
	}
	else if (cmdName == QStringLiteral("HelpIndex") || cmdName == QStringLiteral("HelpUsing"))
	{
		if (!m_mainWindow)
			return;
		if (const auto url = QString::fromLatin1(DOCUMENTATION_PAGE); !QDesktopServices::openUrl(QUrl(url)))
			QMessageBox::warning(m_mainWindow, QStringLiteral("Help"),
			                     QStringLiteral("Unable to open the documentation web page: %1").arg(url));
	}
	else if (cmdName == QStringLiteral("BugReports"))
	{
		if (!m_mainWindow)
			return;
		if (const auto url = QString::fromLatin1(BUG_REPORT_PAGE); !QDesktopServices::openUrl(QUrl(url)))
			QMessageBox::warning(m_mainWindow, QStringLiteral("Bug Reports"),
			                     QStringLiteral("Unable to open the bug report web page: %1").arg(url));
	}
	else if (cmdName == QStringLiteral("DocumentationWebPage"))
	{
		if (!m_mainWindow)
			return;
		if (const auto url = QString::fromLatin1(DOCUMENTATION_PAGE); !QDesktopServices::openUrl(QUrl(url)))
			QMessageBox::warning(m_mainWindow, QStringLiteral("Documentation"),
			                     QStringLiteral("Unable to open the documentation web page: %1").arg(url));
	}
	else if (cmdName == QStringLiteral("HelpForum"))
	{
		if (!m_mainWindow)
			return;
		if (const auto url = QString::fromLatin1(QMUD_FORUM_URL); !QDesktopServices::openUrl(QUrl(url)))
			QMessageBox::warning(m_mainWindow, QStringLiteral("Help Forum"),
			                     QStringLiteral("Unable to open the QMud forum web page: %1").arg(url));
	}
	else if (cmdName == QStringLiteral("FunctionsList") || cmdName == QStringLiteral("FunctionsWebPage"))
	{
		if (!m_mainWindow)
			return;
		if (const auto url = QString::fromLatin1(QMUD_FUNCTIONS_URL); !QDesktopServices::openUrl(QUrl(url)))
			QMessageBox::warning(m_mainWindow, QStringLiteral("Functions"),
			                     QStringLiteral("Unable to open the QMud functions web page: %1").arg(url));
	}
	else if (cmdName == QStringLiteral("HelpMudLists"))
	{
		if (!m_mainWindow)
			return;
		if (const auto url = QString::fromLatin1(MUD_LIST); !QDesktopServices::openUrl(QUrl(url)))
			QMessageBox::warning(m_mainWindow, QStringLiteral("MUD Lists"),
			                     QStringLiteral("Unable to open the MUD lists web page: %1").arg(url));
	}
	else if (cmdName == QStringLiteral("WebPage"))
	{
		if (!m_mainWindow)
			return;
		if (const auto url = QString::fromLatin1(MY_WEB_PAGE); !QDesktopServices::openUrl(QUrl(url)))
			QMessageBox::warning(m_mainWindow, QStringLiteral("Web Page"),
			                     QStringLiteral("Unable to open the web page: %1").arg(url));
	}
	else if (cmdName == QStringLiteral("PluginsList"))
	{
		if (!m_mainWindow)
			return;
		if (const auto url = QString::fromLatin1(PLUGINS_PAGE); !QDesktopServices::openUrl(QUrl(url)))
			QMessageBox::warning(m_mainWindow, QStringLiteral("Plugins"),
			                     QStringLiteral("Unable to open the plugins web page: %1").arg(url));
	}
	else if (cmdName == QStringLiteral("RegularExpressionsWebPage"))
	{
		if (!m_mainWindow)
			return;
		if (const auto url = QString::fromLatin1(REGEXP_PAGE); !QDesktopServices::openUrl(QUrl(url)))
			QMessageBox::warning(
			    m_mainWindow, QStringLiteral("Regular Expressions"),
			    QStringLiteral("Unable to open the regular expressions web page: %1").arg(url));
	}
	else if (cmdName == QStringLiteral("DisplayStart") || cmdName == QStringLiteral("DisplayPageUp") ||
	         cmdName == QStringLiteral("DisplayPageDown") || cmdName == QStringLiteral("DisplayEnd"))
	{
		if (!m_mainWindow)
			return;
		auto *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *view = world->view();
		if (!view)
			return;
		if (cmdName == QStringLiteral("DisplayPageDown"))
			view->scrollOutputPageDown();
		else if (cmdName == QStringLiteral("DisplayStart"))
			view->scrollOutputToStart();
		else if (cmdName == QStringLiteral("DisplayEnd"))
			view->scrollOutputToEnd();
		else
			view->scrollOutputPageUp();
	}
	else if (cmdName == QStringLiteral("DisplayLineUp") || cmdName == QStringLiteral("DisplayLineDown"))
	{
		if (!m_mainWindow)
			return;
		auto *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *view = world->view();
		if (!view)
			return;
		if (cmdName == QStringLiteral("DisplayLineUp"))
			view->scrollOutputLineUp();
		else
			view->scrollOutputLineDown();
	}
	else if (cmdName == QStringLiteral("ClearOutputBuffer"))
	{
		if (!m_mainWindow)
			return;
		WorldChildWindow *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		if (WorldRuntime *runtime = world->runtime(); runtime)
		{
			runtime->deleteOutput();
			return;
		}
		WorldView *view = world->view();
		if (!view)
			return;
		view->clearOutputBuffer();
	}
	else if (cmdName == QStringLiteral("HighlightWord"))
	{
		if (!m_mainWindow)
			return;
		WorldChildWindow *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		WorldView    *view    = world->view();
		WorldRuntime *runtime = world->runtime();
		if (!view || !runtime)
			return;

		auto parseColourValue = [](const QString &value) -> QColor
		{
			if (value.isEmpty())
				return {};
			if (const QColor color(value); color.isValid())
				return color;
			bool      ok      = false;
			const int numeric = value.toInt(&ok);
			if (!ok)
				return {};
			return {numeric & 0xFF, numeric >> 8 & 0xFF, numeric >> 16 & 0xFF};
		};

		QString suggested = view->outputSelectionText();
		if (!suggested.isEmpty())
		{
			suggested.replace(QLatin1Char('\r'), QString());
			if (const qsizetype newline = suggested.indexOf(QLatin1Char('\n')); newline >= 0)
				suggested = suggested.left(newline);
			if (suggested.size() > 80)
				suggested = suggested.left(80);
		}
		else
		{
			suggested = view->wordUnderCursor().trimmed();
		}

		QColor                        otherFore(192, 192, 192);
		QColor                        otherBack(0, 0, 0);
		const QMap<QString, QString> &attrs = runtime->worldAttributes();
		const QColor defaultFore = parseColourValue(attrs.value(QStringLiteral("output_text_colour")));
		const QColor defaultBack = parseColourValue(attrs.value(QStringLiteral("output_background_colour")));
		if (defaultFore.isValid())
			otherFore = defaultFore;
		if (defaultBack.isValid())
			otherBack = defaultBack;

		if (view->hasOutputSelection())
		{
			const QVector<WorldRuntime::LineEntry> &lines = runtime->lines();
			if (const int lineIndex = view->outputSelectionStartLine() - 1;
			    lineIndex >= 0 && lineIndex < lines.size())
			{
				const WorldRuntime::LineEntry &line         = lines.at(lineIndex);
				const int                      column       = qMax(1, view->outputSelectionStartColumn());
				const int                      zeroBasedCol = column - 1;
				int                            offset       = 0;
				for (const auto &span : line.spans)
				{
					if (zeroBasedCol >= offset && zeroBasedCol < offset + span.length)
					{
						if (span.fore.isValid())
							otherFore = span.fore;
						if (span.back.isValid())
							otherBack = span.back;
						break;
					}
					offset += span.length;
				}
			}
		}

		QVector<QColor>  customText(MAX_CUSTOM, QColor(255, 255, 255));
		QVector<QColor>  customBack(MAX_CUSTOM, QColor(0, 0, 0));
		QVector<QString> customNames(MAX_CUSTOM);
		for (int i = 0; i < MAX_CUSTOM; ++i)
		{
			customNames[i] = QStringLiteral("Custom%1").arg(i + 1);
		}

		for (const QList<WorldRuntime::Colour> &colours = runtime->colours();
		     const auto &[group, attributes] : colours)
		{
			if (!group.startsWith(QStringLiteral("custom/")) && group.toLower() != QStringLiteral("custom"))
				continue;
			bool      ok  = false;
			const int seq = attributes.value(QStringLiteral("seq")).toInt(&ok);
			if (!ok || seq < 1 || seq > MAX_CUSTOM)
				continue;
			const int    index      = seq - 1;
			const QColor textColour = parseColourValue(attributes.value(QStringLiteral("text")));
			const QColor backColour = parseColourValue(attributes.value(QStringLiteral("back")));
			if (textColour.isValid())
				customText[index] = textColour;
			if (backColour.isValid())
				customBack[index] = backColour;
			if (const QString nameValue = attributes.value(QStringLiteral("name")).trimmed();
			    !nameValue.isEmpty())
				customNames[index] = nameValue;
		}

		HighlightPhraseDialog dlg(m_mainWindow);
		dlg.setCustomColours(customText, customBack, customNames);
		dlg.setOtherColours(otherFore, otherBack);
		dlg.setInitialText(suggested);
		dlg.setWholeWord(true);
		dlg.setMatchCase(false);
		dlg.setSelectedColourIndex(OTHER_CUSTOM + 1);
		if (dlg.exec() != QDialog::Accepted)
			return;

		QString pattern;
		if (dlg.wholeWord())
			pattern += QStringLiteral("\\b");
		pattern += convertToRegularExpression(dlg.phraseText(), false, false);
		if (dlg.wholeWord())
			pattern += QStringLiteral("\\b");

		QRegularExpression::PatternOptions opts = QRegularExpression::NoPatternOption;
		if (!dlg.matchCase())
			opts |= QRegularExpression::CaseInsensitiveOption;
		if (QRegularExpression re(pattern, opts); !re.isValid())
		{
			QMessageBox::warning(
			    m_mainWindow, QStringLiteral("QMud"),
			    QStringLiteral("Cannot compile regular expression: %1").arg(re.errorString()));
			return;
		}

		QList<WorldRuntime::Trigger> triggers = runtime->triggers();
		WorldRuntime::Trigger        trigger;
		QMap<QString, QString>      &t = trigger.attributes;
		t.insert(QStringLiteral("name"), QStringLiteral("*trigger%1").arg(WorldRuntime::getUniqueNumber()));
		t.insert(QStringLiteral("match"), pattern);
		t.insert(QStringLiteral("regexp"), QStringLiteral("1"));
		t.insert(QStringLiteral("ignore_case"), dlg.matchCase() ? QStringLiteral("0") : QStringLiteral("1"));
		t.insert(QStringLiteral("repeat"), QStringLiteral("1"));
		t.insert(QStringLiteral("keep_evaluating"), QStringLiteral("1"));
		t.insert(QStringLiteral("send_to"), QString::number(eSendToWorld));
		t.insert(QStringLiteral("sequence"), QStringLiteral("90"));
		t.insert(QStringLiteral("group"), QStringLiteral("Highlighted Words"));
		t.insert(QStringLiteral("custom_colour"), QString::number(dlg.selectedColourIndex()));
		t.insert(QStringLiteral("colour_change_type"), QString::number(TRIGGER_COLOUR_CHANGE_BOTH));
		if (dlg.selectedColourIndex() == OTHER_CUSTOM + 1)
		{
			auto toColourString = [](const QColor &color) -> QString
			{
				return QStringLiteral("#%1%2%3")
				    .arg(color.red(), 2, 16, QLatin1Char('0'))
				    .arg(color.green(), 2, 16, QLatin1Char('0'))
				    .arg(color.blue(), 2, 16, QLatin1Char('0'))
				    .toUpper();
			};
			t.insert(QStringLiteral("other_text_colour"), toColourString(dlg.otherTextColour()));
			t.insert(QStringLiteral("other_back_colour"), toColourString(dlg.otherBackColour()));
		}
		triggers.push_back(trigger);
		runtime->setTriggers(triggers);
	}
	else if (cmdName == QStringLiteral("BookmarkSelection"))
	{
		if (!m_mainWindow)
			return;
		WorldChildWindow *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		WorldView    *view    = world->view();
		WorldRuntime *runtime = world->runtime();
		if (!view || !runtime)
			return;
		int line = view->outputSelectionStartLine();
		if (line <= 0)
			line = static_cast<int>(runtime->lines().size());
		if (line <= 0)
			return;
		constexpr int kBookmarkFlag = 0x08;
		bool          isBookmarked  = false;
		if (const QVector<WorldRuntime::LineEntry> &lines = runtime->lines();
		    line >= 1 && line <= lines.size())
			isBookmarked = lines.at(line - 1).flags & kBookmarkFlag;
		runtime->bookmarkLine(line, !isBookmarked);
		view->selectOutputLine(line - 1);
		m_mainWindow->showStatusMessage(
		    isBookmarked ? QStringLiteral("Bookmark cleared.") : QStringLiteral("Bookmark set."), 2000);
	}
	else if (cmdName == QStringLiteral("GoToBookmark"))
	{
		if (!m_mainWindow)
			return;
		WorldChildWindow *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *view    = world->view();
		auto *runtime = world->runtime();
		if (!view || !runtime)
			return;
		const auto &lines = runtime->lines();
		if (lines.isEmpty())
			return;
		const int totalLines = static_cast<int>(lines.size());
		int       startLine  = view->outputSelectionStartLine();
		if (startLine < 1 || startLine > totalLines)
			startLine = 1;
		int found = -1;
		for (int offset = 1; offset <= totalLines; ++offset)
		{
			int index = startLine - 1 + offset;
			if (index >= totalLines)
				index -= totalLines;
			if (constexpr int kBookmarkFlag = 0x08; lines.at(index).flags & kBookmarkFlag)
			{
				found = index;
				break;
			}
		}
		if (found < 0)
			return;
		view->selectOutputLine(found);
	}
	else if (cmdName == QStringLiteral("RecallLastWord"))
	{
		if (!m_mainWindow)
			return;
		auto *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *view = world->view();
		if (!view)
			return;
		view->recallLastWord();
	}
	else if (cmdName == QStringLiteral("NextCommand") || cmdName == QStringLiteral("PreviousCommand") ||
	         cmdName == QStringLiteral("RepeatLastCommand"))
	{
		if (!m_mainWindow)
			return;
		auto *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *view = world->view();
		if (!view)
			return;
		if (cmdName == QStringLiteral("NextCommand"))
			view->recallNextCommand();
		else if (cmdName == QStringLiteral("PreviousCommand"))
			view->recallPreviousCommand();
		else
			view->repeatLastCommand();
	}
	else if (cmdName == QStringLiteral("GoToMatchingBrace") ||
	         isCommand(QStringLiteral("SelectToMatchingBrace")))
	{
		QPlainTextEdit *target = nullptr;
		if (auto *focus = QApplication::focusWidget(); auto *edit = qobject_cast<QPlainTextEdit *>(focus))
		{
			QWidget *parent = edit;
			while (parent)
			{
				if (qobject_cast<TextChildWindow *>(parent) || qobject_cast<WorldChildWindow *>(parent))
				{
					target = edit;
					break;
				}
				parent = parent->parentWidget();
			}
		}
		if (!target && m_mainWindow)
		{
			if (auto *world = m_mainWindow->activeWorldChildWindow())
			{
				if (auto *view = world->view())
					target = view->inputEditor();
			}
		}
		if (!target)
			return;

		const auto flags = getGlobalOption(QStringLiteral("ParenMatchFlags")).toInt();
		QMudBraceMatch::findMatchingBrace(target, isCommand(QStringLiteral("SelectToMatchingBrace")), flags);
	}
	else if (cmdName == QStringLiteral("QuoteLines"))
	{
		QPlainTextEdit *target = nullptr;
		if (auto *focus = QApplication::focusWidget(); auto *edit = qobject_cast<QPlainTextEdit *>(focus))
		{
			QWidget *parent = edit;
			while (parent)
			{
				if (qobject_cast<TextChildWindow *>(parent))
				{
					target = edit;
					break;
				}
				parent = parent->parentWidget();
			}
		}
		if (!target)
			return;

		const auto quote = getGlobalOption(QStringLiteral("NotepadQuoteString")).toString();
		if (quote.isEmpty())
			return;

		auto cursor    = target->textCursor();
		auto selectAll = !cursor.hasSelection();
		auto text      = selectAll ? target->toPlainText() : cursor.selectedText();
		if (!selectAll)
			text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));

		QString quoted;
		quoted.reserve(text.size() + (text.count(QLatin1Char('\n')) + 1) * quote.size());
		quoted += quote;
		for (const QChar ch : text)
		{
			quoted += ch;
			if (ch == QLatin1Char('\n'))
				quoted += quote;
		}

		auto replaceCursor = target->textCursor();
		if (selectAll)
			replaceCursor.select(QTextCursor::Document);
		replaceCursor.insertText(quoted);
		if (!selectAll)
		{
			const int start = replaceCursor.position() - static_cast<int>(quoted.size());
			replaceCursor.setPosition(start);
			replaceCursor.setPosition(start + static_cast<int>(quoted.size()), QTextCursor::KeepAnchor);
			target->setTextCursor(replaceCursor);
		}
	}
	else if (cmdName == QStringLiteral("AlwaysOnTop"))
	{
		if (!m_mainWindow)
			return;
		auto *action  = m_mainWindow->actionForCommand(QStringLiteral("AlwaysOnTop"));
		bool  enabled = false;
		if (action && action->isCheckable())
			enabled = action->isChecked();
		else
			enabled = getGlobalOption(QStringLiteral("AlwaysOnTop")).toInt() == 0;

		setGlobalOptionInt(QStringLiteral("AlwaysOnTop"), enabled ? 1 : 0);
		auto flags = m_mainWindow->windowFlags();
		if (enabled)
			flags |= Qt::WindowStaysOnTopHint;
		else
			flags &= ~Qt::WindowStaysOnTopHint;
		m_mainWindow->setWindowFlags(flags);
		m_mainWindow->show();
	}
	else if (cmdName == QStringLiteral("ResetConnectedTime"))
	{
		if (!m_mainWindow)
			return;
		auto *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *runtime = world->runtime();
		if (!runtime)
			return;
		runtime->resetStatusTime();
		m_mainWindow->updateStatusBar();
		m_mainWindow->refreshActionState();
	}
	else if (cmdName == QStringLiteral("ExitClient"))
	{
		if (m_mainWindow)
			m_mainWindow->close();
	}
	else if (cmdName == QStringLiteral("Notepad") || cmdName == QStringLiteral("NotesWorkArea"))
	{
		if (!m_mainWindow)
			return;
		auto title = QStringLiteral("Notepad");
		if (auto *world = m_mainWindow->activeWorldChildWindow())
		{
			if (auto *runtime = world->runtime())
			{
				if (const auto worldName = runtime->worldAttributes().value(QStringLiteral("name")).trimmed();
				    !worldName.isEmpty())
				{
					title = QStringLiteral("Notepad: %1").arg(worldName);
				}
			}
		}
		auto *text = new TextChildWindow(title, QString());
		m_mainWindow->addMdiSubWindow(text);
	}
	else if (cmdName == QStringLiteral("FlipToNotepad"))
	{
		if (m_mainWindow)
			m_mainWindow->switchToNotepad();
	}
	else if (cmdName == QStringLiteral("GettingStarted"))
		handleHelpGettingStarted();
	else if (cmdName == QStringLiteral("Undo") || cmdName == QStringLiteral("Cut") ||
	         cmdName == QStringLiteral("Paste") || cmdName == QStringLiteral("SelectAll"))
		handleEditCommand(cmdName);
	else if (cmdName == QStringLiteral("ActivateInputArea"))
	{
		if (!m_mainWindow)
			return;
		auto *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *view = world->view();
		if (!view)
			return;
		if (auto *input = view->inputEditor())
		{
			input->setFocus(Qt::OtherFocusReason);
			input->ensureCursorVisible();
		}
	}
	else if (cmdName == QStringLiteral("TipOfTheDay"))
	{
		showTipDialog();
	}
	else if (cmdName == QStringLiteral("Open"))
	{
		auto initialDir = defaultWorldDirectory();
		if (initialDir.isEmpty())
			initialDir = m_fileBrowsingDir;
		changeToFileBrowsingDirectory();
		const auto filter = QStringLiteral("World or text files (*.qdl *.mcl *.txt);;All files (*.*)");
		const auto fileName =
		    QFileDialog::getOpenFileName(m_mainWindow, QStringLiteral("Open"), initialDir, filter);
		changeToStartupDirectory();
		if (fileName.isEmpty())
			return;
		const QFileInfo info(fileName);
		m_fileBrowsingDir                    = info.absolutePath();
		const int previousActivationOverride = m_nextNewWorldActivationOverride;
		if (QMudFileExtensions::isWorldSuffix(info.suffix().toLower()))
			m_nextNewWorldActivationOverride = 1;
		const bool opened = openDocumentFile(fileName);
		Q_UNUSED(opened);
		m_nextNewWorldActivationOverride = previousActivationOverride;
	}
	else if (cmdName == QStringLiteral("OpenWorldsInStartupList"))
	{
		changeToStartupDirectory();
		auto       worldList         = getGlobalOption(QStringLiteral("WorldList")).toString();
		bool       worldListChanged  = false;
		const auto migratedWorldList = migrateWorldListPaths(m_workingDir, worldList, &worldListChanged);
		if (worldListChanged)
		{
			setGlobalOptionString(QStringLiteral("WorldList"), migratedWorldList);
			worldList = migratedWorldList;
		}
		worldList = canonicalizeWorldListForRuntime(worldList);
		if (worldList.isEmpty())
			return;
		const auto items = splitSerializedWorldList(worldList);
		openWorldsFromList(items, false);
		changeToStartupDirectory();
	}
	else if (cmdName == QStringLiteral("CloseWorld"))
	{
		if (!m_mainWindow)
			return;
		if (WorldChildWindow *world = m_mainWindow->activeWorldChildWindow())
			world->close();
	}
	else if (cmdName == QStringLiteral("Plugins"))
	{
		if (!m_mainWindow)
			return;
		WorldChildWindow *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		WorldRuntime *runtime = world->runtime();
		if (!runtime)
			return;
		PluginsDialog dlg(runtime, m_mainWindow, m_mainWindow);
		dlg.exec();
	}
	else if (cmdName == QStringLiteral("PluginWizard"))
	{
		if (!m_mainWindow)
			return;
		WorldChildWindow *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
		{
			QMessageBox::information(m_mainWindow, QStringLiteral("Plugin Wizard"),
			                         QStringLiteral("Open a world before running the plugin wizard."));
			return;
		}
		WorldRuntime *runtime = world->runtime();
		if (!runtime)
			return;

		PluginWizardDialog wizard(runtime, m_mainWindow);
		if (wizard.exec() != QDialog::Accepted)
			return;
		const auto &state = wizard.state();

		QString     initialDir = m_pluginsDirectory;
		if (!runtime->pluginsDirectory().isEmpty())
			initialDir = runtime->pluginsDirectory();
		const QString fileName =
		    QFileDialog::getSaveFileName(m_mainWindow, QStringLiteral("Create Plugin"), initialDir,
		                                 QStringLiteral("QMud Plugins (*.xml);;All files (*.*)"));
		if (fileName.isEmpty())
			return;

		const QString outputPath = ensureExtension(fileName, QStringLiteral("xml"));
		QSaveFile     file(outputPath);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
		{
			QMessageBox::warning(m_mainWindow, QStringLiteral("Plugin Wizard"),
			                     QStringLiteral("Unable to create the requested file."));
			return;
		}

		auto fixHtmlString = [](const QString &source) -> QString
		{
			QString   strOldString = source;
			QString   strNewString;
			qsizetype i;
			while ((i = strOldString.indexOf(QRegularExpression(QStringLiteral("[<>&\\\"]")))) != -1)
			{
				strNewString += strOldString.left(i);
				switch (strOldString.at(i).unicode())
				{
				case '<':
					strNewString += QStringLiteral("&lt;");
					break;
				case '>':
					strNewString += QStringLiteral("&gt;");
					break;
				case '&':
					strNewString += QStringLiteral("&amp;");
					break;
				case '\"':
					strNewString += QStringLiteral("&quot;");
					break;
				default:
					break;
				}
				strOldString = strOldString.mid(i + 1);
			}
			strNewString += strOldString;
			return strNewString;
		};

		auto fixHtmlMultilineString = [](const QString &source) -> QString
		{
			QString   strOldString = source;
			QString   strNewString;
			qsizetype i;
			while ((i = strOldString.indexOf(QRegularExpression(QStringLiteral("[<>&\\t]")))) != -1)
			{
				strNewString += strOldString.left(i);
				switch (strOldString.at(i).unicode())
				{
				case '<':
					strNewString += QStringLiteral("&lt;");
					break;
				case '>':
					strNewString += QStringLiteral("&gt;");
					break;
				case '&':
					strNewString += QStringLiteral("&amp;");
					break;
				case '\t':
					strNewString += QStringLiteral("&#9;");
					break;
				default:
					break;
				}
				strOldString = strOldString.mid(i + 1);
			}
			strNewString += strOldString;
			return strNewString;
		};

		auto replaceNewlines = [](const QString &value) -> QString
		{
			QString result = value;
			result.replace(QStringLiteral("\r\n"), QStringLiteral(" "));
			result.replace(QLatin1Char('\n'), QLatin1Char(' '));
			result.replace(QLatin1Char('\r'), QLatin1Char(' '));
			return result;
		};

		auto saveXmlBoolean = [](QTextStream &out, const QString &nl, const char *name, const bool value,
		                         const bool sameLine = false)
		{
			if (value)
				out << (sameLine ? "" : "   ") << name << "=\"y\"" << (sameLine ? " " : nl);
		};

		auto saveXmlString = [&fixHtmlString, &replaceNewlines](QTextStream &out, const QString &nl,
		                                                        const char *name, const QString &value,
		                                                        const bool sameLine = false)
		{
			if (!value.isEmpty())
				out << (sameLine ? "" : "   ") << name << "=\"" << fixHtmlString(replaceNewlines(value))
				    << "\"" << (sameLine ? " " : nl);
		};

		auto saveXmlNumber = [](QTextStream &out, const QString &nl, const char *name, const long long number,
		                        const bool sameLine = false)
		{
			if (number)
				out << (sameLine ? "" : "   ") << name << "=\"" << number << "\"" << (sameLine ? " " : nl);
		};

		auto saveXmlDouble = [&saveXmlString](QTextStream &out, const QString &nl, const char *name,
		                                      const double value, const bool sameLine = false)
		{
			QString number = QString::asprintf("%.2f", value);
			number.replace(QLatin1Char(','), QLatin1Char('.'));
			saveXmlString(out, nl, name, number, sameLine);
		};

		auto saveXmlColour = [&saveXmlString](QTextStream &out, const QString &nl, const char *name,
		                                      const long colour, const bool sameLine = false)
		{
			if (!colour)
				return;
			saveXmlString(out, nl, name, qmudColourToName(colour), sameLine);
		};

		auto saveXmlMulti = [&fixHtmlMultilineString](QTextStream &out, const QString &nl, const char *name,
		                                              const QString &value)
		{
			if (!value.isEmpty())
				out << "  <" << name << ">" << fixHtmlMultilineString(value) << "</" << name << ">" << nl;
		};

		auto parseColorRef = [](const QString &value) -> long
		{
			if (value.isEmpty())
				return 0;
			if (const QColor color(value); color.isValid())
			{
				const long red   = color.red() & 0xFF;
				const long green = (color.green() & 0xFF) << 8;
				const long blue  = (color.blue() & 0xFF) << 16;
				return red | green | blue;
			}
			bool       ok      = false;
			const long numeric = value.toLong(&ok);
			return ok ? numeric : 0;
		};

		QTextStream out(&file);
		out.setEncoding(QStringConverter::Utf8);
		const auto    nl      = QStringLiteral("\r\n");
		const auto    now     = QDateTime::currentDateTime();
		const QString savedOn = runtime->formatTime(now, QStringLiteral("%A, %B %d, %Y, %#I:%M %p"), false);

		out << R"(<?xml version="1.0" encoding="utf-8"?>)" << nl;
		out << "<!DOCTYPE qmud>" << nl;
		out << "<!-- Saved on " << fixHtmlString(savedOn) << " -->" << nl;
		out << "<!-- QMud version " << kVersionString << " -->" << nl;
		out << nl << "<!-- Plugin \"" << fixHtmlString(state.name) << "\" generated by Plugin Wizard -->"
		    << nl << nl;

		if (!state.comments.trimmed().isEmpty())
		{
			out << "<!--" << nl;
			out << state.comments;
			if (!state.comments.endsWith(QLatin1Char('\n')))
				out << nl;
			out << "-->" << nl << nl;
		}

		out << "<qmud>" << nl;
		out << "<plugin" << nl;
		saveXmlString(out, nl, "name", state.name);
		saveXmlString(out, nl, "author", state.author);
		saveXmlString(out, nl, "id", state.id);
		saveXmlString(out, nl, "language", state.language);
		saveXmlString(out, nl, "purpose", state.purpose);
		saveXmlBoolean(out, nl, "save_state", state.saveState);
		saveXmlString(out, nl, "date_written", state.dateWritten);
		saveXmlDouble(out, nl, "requires", state.requiresVersion);
		saveXmlString(out, nl, "version", state.version);
		out << "   >" << nl;

		if (!state.description.trimmed().isEmpty())
		{
			out << "<description trim=\"y\">" << nl;
			out << "<![CDATA[" << nl;
			out << state.description;
			if (!state.description.endsWith(QLatin1Char('\n')))
				out << nl;
			out << "]]>" << nl;
			out << "</description>" << nl;
		}

		out << nl << "</plugin>" << nl << nl;

		if (state.standardConstants &&
		    state.language.compare(QStringLiteral("Lua"), Qt::CaseInsensitive) == 0)
			out << "<include name=\"constants.lua\"/>" << nl;

		auto exportTriggersList = [&]
		{
			if (state.selectedTriggers.isEmpty())
				return;
			QList<const WorldRuntime::Trigger *> triggers;
			const QList<WorldRuntime::Trigger>  &all = runtime->triggers();
			for (int i = 0; i < all.size(); ++i)
			{
				if (!state.selectedTriggers.contains(i))
					continue;
				const auto &tr = all.at(i);
				if (tr.included || isEnabledFlag(tr.attributes.value(QStringLiteral("temporary"))))
					continue;
				triggers.push_back(&tr);
			}
			if (triggers.isEmpty())
				return;
			out << nl << "<!--  Triggers  -->" << nl << nl;
			out << "<triggers" << nl;
			saveXmlString(out, nl, "qmud_version", QString::fromLatin1(kVersionString));
			saveXmlNumber(out, nl, "world_file_version", kThisVersion);
			out << "  >" << nl;
			std::ranges::stable_sort(triggers,
			                         [](const WorldRuntime::Trigger *a, const WorldRuntime::Trigger *b)
			                         {
				                         bool      okA = false;
				                         bool      okB = false;
				                         const int seqA =
				                             a->attributes.value(QStringLiteral("sequence")).toInt(&okA);
				                         const int seqB =
				                             b->attributes.value(QStringLiteral("sequence")).toInt(&okB);
				                         const int seqValA = okA ? seqA : 0;
				                         if (const int seqValB = okB ? seqB : 0; seqValA != seqValB)
					                         return seqValA < seqValB;
				                         return a->attributes.value(QStringLiteral("match")) <
				                                b->attributes.value(QStringLiteral("match"));
			                         });
			for (const auto *tr : triggers)
			{
				out << "  <trigger" << nl;
				auto num = [&](const char *key)
				{
					bool            ok    = false;
					const long long value = tr->attributes.value(QString::fromLatin1(key)).toLongLong(&ok);
					if (ok)
						saveXmlNumber(out, nl, key, value);
				};
				auto boolean = [&](const char *key)
				{
					saveXmlBoolean(out, nl, key,
					               isEnabledFlag(tr->attributes.value(QString::fromLatin1(key))));
				};
				auto text = [&](const char *key)
				{ saveXmlString(out, nl, key, tr->attributes.value(QString::fromLatin1(key))); };
				num("back_colour");
				boolean("bold");
				num("clipboard_arg");
				{
					bool            ok = false;
					const long long value =
					    tr->attributes.value(QStringLiteral("custom_colour")).toLongLong(&ok);
					if (ok && value != 0)
						saveXmlNumber(out, nl, "custom_colour", value);
				}
				num("colour_change_type");
				if (const bool triggerEnabled =
				        isEnabledFlag(tr->attributes.value(QStringLiteral("enabled")));
				    triggerEnabled)
					saveXmlBoolean(out, nl, "enabled", true);
				else
					out << "   enabled=\"n\"" << nl;
				boolean("expand_variables");
				text("group");
				boolean("ignore_case");
				boolean("inverse");
				boolean("italic");
				num("lines_to_match");
				boolean("keep_evaluating");
				boolean("make_bold");
				boolean("make_italic");
				boolean("make_underline");
				text("match");
				boolean("match_back_colour");
				boolean("match_bold");
				boolean("match_inverse");
				boolean("match_italic");
				boolean("match_text_colour");
				boolean("match_underline");
				boolean("multi_line");
				text("name");
				boolean("one_shot");
				boolean("omit_from_log");
				boolean("omit_from_output");
				boolean("regexp");
				boolean("repeat");
				text("script");
				num("send_to");
				num("sequence");
				text("sound");
				boolean("sound_if_inactive");
				boolean("lowercase_wildcard");
				boolean("temporary");
				num("text_colour");
				num("user");
				text("variable");
				{
					const long otherText =
					    parseColorRef(tr->attributes.value(QStringLiteral("other_text_colour")));
					if (otherText)
						saveXmlColour(out, nl, "other_text_colour", otherText);
					const long otherBack =
					    parseColorRef(tr->attributes.value(QStringLiteral("other_back_colour")));
					if (otherBack)
						saveXmlColour(out, nl, "other_back_colour", otherBack);
				}
				out << "  >" << nl;
				saveXmlMulti(out, nl, "send", tr->children.value(QStringLiteral("send")));
				out << "  </trigger>" << nl;
			}
			out << "</triggers>" << nl;
		};

		auto exportAliasesList = [&]
		{
			if (state.selectedAliases.isEmpty())
				return;
			QList<const WorldRuntime::Alias *> aliases;
			const QList<WorldRuntime::Alias>  &all = runtime->aliases();
			for (int i = 0; i < all.size(); ++i)
			{
				if (!state.selectedAliases.contains(i))
					continue;
				const auto &al = all.at(i);
				if (al.included || isEnabledFlag(al.attributes.value(QStringLiteral("temporary"))))
					continue;
				aliases.push_back(&al);
			}
			if (aliases.isEmpty())
				return;
			out << nl << "<!--  Aliases  -->" << nl << nl;
			out << "<aliases" << nl;
			saveXmlString(out, nl, "qmud_version", QString::fromLatin1(kVersionString));
			saveXmlNumber(out, nl, "world_file_version", kThisVersion);
			out << "  >" << nl;
			std::ranges::stable_sort(aliases,
			                         [](const WorldRuntime::Alias *a, const WorldRuntime::Alias *b)
			                         {
				                         return a->attributes.value(QStringLiteral("match")) <
				                                b->attributes.value(QStringLiteral("match"));
			                         });
			for (const auto *al : aliases)
			{
				out << "  <alias" << nl;
				saveXmlString(out, nl, "name", al->attributes.value(QStringLiteral("name")));
				saveXmlString(out, nl, "script", al->attributes.value(QStringLiteral("script")));
				saveXmlString(out, nl, "match", al->attributes.value(QStringLiteral("match")));
				if (const bool aliasEnabled = isEnabledFlag(al->attributes.value(QStringLiteral("enabled")));
				    aliasEnabled)
					saveXmlBoolean(out, nl, "enabled", true);
				else
					out << "   enabled=\"n\"" << nl;
				saveXmlBoolean(out, nl, "echo_alias",
				               isEnabledFlag(al->attributes.value(QStringLiteral("echo_alias"))));
				saveXmlBoolean(out, nl, "expand_variables",
				               isEnabledFlag(al->attributes.value(QStringLiteral("expand_variables"))));
				saveXmlString(out, nl, "group", al->attributes.value(QStringLiteral("group")));
				saveXmlString(out, nl, "variable", al->attributes.value(QStringLiteral("variable")));
				saveXmlBoolean(
				    out, nl, "omit_from_command_history",
				    isEnabledFlag(al->attributes.value(QStringLiteral("omit_from_command_history"))));
				saveXmlBoolean(out, nl, "omit_from_log",
				               isEnabledFlag(al->attributes.value(QStringLiteral("omit_from_log"))));
				saveXmlBoolean(out, nl, "regexp",
				               isEnabledFlag(al->attributes.value(QStringLiteral("regexp"))));
				{
					bool            ok    = false;
					const long long value = al->attributes.value(QStringLiteral("send_to")).toLongLong(&ok);
					if (ok)
						saveXmlNumber(out, nl, "send_to", value);
				}
				saveXmlBoolean(out, nl, "omit_from_output",
				               isEnabledFlag(al->attributes.value(QStringLiteral("omit_from_output"))));
				saveXmlBoolean(out, nl, "one_shot",
				               isEnabledFlag(al->attributes.value(QStringLiteral("one_shot"))));
				saveXmlBoolean(out, nl, "menu", isEnabledFlag(al->attributes.value(QStringLiteral("menu"))));
				saveXmlBoolean(out, nl, "ignore_case",
				               isEnabledFlag(al->attributes.value(QStringLiteral("ignore_case"))));
				saveXmlBoolean(out, nl, "keep_evaluating",
				               isEnabledFlag(al->attributes.value(QStringLiteral("keep_evaluating"))));
				{
					bool            ok    = false;
					const long long value = al->attributes.value(QStringLiteral("sequence")).toLongLong(&ok);
					if (ok)
						saveXmlNumber(out, nl, "sequence", value);
				}
				saveXmlBoolean(out, nl, "temporary",
				               isEnabledFlag(al->attributes.value(QStringLiteral("temporary"))));
				{
					bool            ok    = false;
					const long long value = al->attributes.value(QStringLiteral("user")).toLongLong(&ok);
					if (ok)
						saveXmlNumber(out, nl, "user", value);
				}
				out << "  >" << nl;
				saveXmlMulti(out, nl, "send", al->children.value(QStringLiteral("send")));
				out << "  </alias>" << nl;
			}
			out << "</aliases>" << nl;
		};

		auto exportTimersList = [&]
		{
			if (state.selectedTimers.isEmpty())
				return;
			QList<const WorldRuntime::Timer *> timers;
			const QList<WorldRuntime::Timer>  &all = runtime->timers();
			for (int i = 0; i < all.size(); ++i)
			{
				if (!state.selectedTimers.contains(i))
					continue;
				const auto &tm = all.at(i);
				if (tm.included || isEnabledFlag(tm.attributes.value(QStringLiteral("temporary"))))
					continue;
				timers.push_back(&tm);
			}
			if (timers.isEmpty())
				return;
			out << nl << "<!--  Timers  -->" << nl << nl;
			out << "<timers" << nl;
			saveXmlString(out, nl, "qmud_version", QString::fromLatin1(kVersionString));
			saveXmlNumber(out, nl, "world_file_version", kThisVersion);
			out << "  >" << nl;
			for (const auto *tm : timers)
			{
				out << "  <timer ";
				saveXmlString(out, nl, "name", tm->attributes.value(QStringLiteral("name")), true);
				saveXmlString(out, nl, "script", tm->attributes.value(QStringLiteral("script")), true);
				if (const bool timerEnabled = isEnabledFlag(tm->attributes.value(QStringLiteral("enabled")));
				    timerEnabled)
					saveXmlBoolean(out, nl, "enabled", true, true);
				else
					out << "enabled=\"n\" ";
				bool            ok   = false;
				const long long hour = tm->attributes.value(QStringLiteral("hour")).toLongLong(&ok);
				if (ok)
					saveXmlNumber(out, nl, "hour", hour, true);
				const long long minute = tm->attributes.value(QStringLiteral("minute")).toLongLong(&ok);
				if (ok)
					saveXmlNumber(out, nl, "minute", minute, true);
				const double second = tm->attributes.value(QStringLiteral("second")).toDouble(&ok);
				if (ok)
					saveXmlDouble(out, nl, "second", second, true);
				const long long offsetHour =
				    tm->attributes.value(QStringLiteral("offset_hour")).toLongLong(&ok);
				if (ok)
					saveXmlNumber(out, nl, "offset_hour", offsetHour, true);
				const long long offsetMinute =
				    tm->attributes.value(QStringLiteral("offset_minute")).toLongLong(&ok);
				if (ok)
					saveXmlNumber(out, nl, "offset_minute", offsetMinute, true);
				const double offsetSecond =
				    tm->attributes.value(QStringLiteral("offset_second")).toDouble(&ok);
				if (ok)
					saveXmlDouble(out, nl, "offset_second", offsetSecond, true);
				const long long sendTo = tm->attributes.value(QStringLiteral("send_to")).toLongLong(&ok);
				if (ok)
					saveXmlNumber(out, nl, "send_to", sendTo);
				saveXmlBoolean(out, nl, "temporary",
				               isEnabledFlag(tm->attributes.value(QStringLiteral("temporary"))));
				const long long user = tm->attributes.value(QStringLiteral("user")).toLongLong(&ok);
				if (ok)
					saveXmlNumber(out, nl, "user", user);
				saveXmlBoolean(out, nl, "at_time",
				               isEnabledFlag(tm->attributes.value(QStringLiteral("at_time"))), true);
				saveXmlString(out, nl, "group", tm->attributes.value(QStringLiteral("group")), true);
				saveXmlString(out, nl, "variable", tm->attributes.value(QStringLiteral("variable")));
				saveXmlBoolean(out, nl, "one_shot",
				               isEnabledFlag(tm->attributes.value(QStringLiteral("one_shot"))), true);
				saveXmlBoolean(out, nl, "omit_from_output",
				               isEnabledFlag(tm->attributes.value(QStringLiteral("omit_from_output"))), true);
				saveXmlBoolean(out, nl, "omit_from_log",
				               isEnabledFlag(tm->attributes.value(QStringLiteral("omit_from_log"))), true);
				saveXmlBoolean(out, nl, "active_closed",
				               isEnabledFlag(tm->attributes.value(QStringLiteral("active_closed"))), true);
				out << ">" << nl;
				saveXmlMulti(out, nl, "send", tm->children.value(QStringLiteral("send")));
				out << nl << "  </timer>" << nl;
			}
			out << "</timers>" << nl;
		};

		auto exportVariablesList = [&]
		{
			if (state.selectedVariables.isEmpty())
				return;
			QList<WorldRuntime::Variable>        vars;
			const QList<WorldRuntime::Variable> &all = runtime->variables();
			for (int i = 0; i < all.size(); ++i)
			{
				if (!state.selectedVariables.contains(i))
					continue;
				vars.push_back(all.at(i));
			}
			if (vars.isEmpty())
				return;
			out << nl << "<!--  Variables  -->" << nl << nl;
			out << "<variables" << nl;
			saveXmlString(out, nl, "qmud_version", QString::fromLatin1(kVersionString));
			saveXmlNumber(out, nl, "world_file_version", kThisVersion);
			out << "  >" << nl;
			std::ranges::stable_sort(vars,
			                         [](const WorldRuntime::Variable &a, const WorldRuntime::Variable &b)
			                         {
				                         return a.attributes.value(QStringLiteral("name")) <
				                                b.attributes.value(QStringLiteral("name"));
			                         });
			for (const auto &[attributes, content] : vars)
			{
				const QString name = attributes.value(QStringLiteral("name"));
				out << "  <variable name=\"" << fixHtmlString(name) << "\">"
				    << fixHtmlMultilineString(content) << "</variable>" << nl;
			}
			out << "</variables>" << nl;
		};

		exportTriggersList();
		exportAliasesList();
		exportTimersList();
		exportVariablesList();

		if (!state.script.trimmed().isEmpty())
		{
			out << nl << "<!--  Script  -->" << nl << nl;
			out << "<script>" << nl;
			out << "<![CDATA[" << nl;
			out << state.script;
			if (!state.script.endsWith(QLatin1Char('\n')))
				out << nl;
			out << "]]>" << nl;
			out << "</script>" << nl << nl;
		}

		if (state.generateHelp && !state.helpAlias.isEmpty() && !state.description.trimmed().isEmpty())
		{
			out << nl << "<!--  Plugin help  -->" << nl << nl;
			out << "<aliases" << nl;
			saveXmlString(out, nl, "qmud_version", QString::fromLatin1(kVersionString));
			saveXmlNumber(out, nl, "world_file_version", kThisVersion);
			out << "  >" << nl;
			out << "  <alias" << nl;
			saveXmlString(out, nl, "script", QStringLiteral("OnHelp"));
			saveXmlString(out, nl, "match", state.helpAlias);
			saveXmlBoolean(out, nl, "enabled", true);
			out << "  >" << nl;
			out << "  </alias>" << nl;
			out << "</aliases>" << nl;
			out << nl << "<script>" << nl;
			out << "<![CDATA[" << nl;
			out << "function OnHelp ()" << nl;
			out << "  world.Note (world.GetPluginInfo (world.GetPluginID (), 3))" << nl;
			out << "end" << nl;
			out << "]]>" << nl;
			out << "</script>" << nl;
		}

		out << nl << "</qmud>" << nl;

		if (!file.commit())
		{
			QMessageBox::warning(m_mainWindow, QStringLiteral("Plugin Wizard"),
			                     QStringLiteral("Unable to save the requested file."));
			return;
		}

		if (state.removeItems)
		{
			if (!state.selectedTriggers.isEmpty())
			{
				QList<WorldRuntime::Trigger>        filtered;
				const QList<WorldRuntime::Trigger> &all = runtime->triggers();
				for (int i = 0; i < all.size(); ++i)
				{
					if (state.selectedTriggers.contains(i))
						continue;
					filtered.push_back(all.at(i));
				}
				runtime->setTriggers(filtered);
			}
			if (!state.selectedAliases.isEmpty())
			{
				QList<WorldRuntime::Alias>        filtered;
				const QList<WorldRuntime::Alias> &all = runtime->aliases();
				for (int i = 0; i < all.size(); ++i)
				{
					if (state.selectedAliases.contains(i))
						continue;
					filtered.push_back(all.at(i));
				}
				runtime->setAliases(filtered);
			}
			if (!state.selectedTimers.isEmpty())
			{
				QList<WorldRuntime::Timer> filtered;
				const auto                &all = runtime->timers();
				for (auto i = 0; i < all.size(); ++i)
				{
					if (state.selectedTimers.contains(i))
						continue;
					filtered.push_back(all.at(i));
				}
				runtime->setTimers(filtered);
			}
			if (!state.selectedVariables.isEmpty())
			{
				QList<WorldRuntime::Variable> filtered;
				const auto                   &all = runtime->variables();
				for (auto i = 0; i < all.size(); ++i)
				{
					if (state.selectedVariables.contains(i))
						continue;
					filtered.push_back(all.at(i));
				}
				runtime->setVariables(filtered);
			}
		}

		m_fileBrowsingDir = QFileInfo(outputPath).absolutePath();
		openDocumentFile(outputPath);
	}
	else if (cmdName == QStringLiteral("Save") || cmdName == QStringLiteral("SaveAs"))
	{
		if (!m_mainWindow)
			return;
		const auto saveAs = cmdName == QStringLiteral("SaveAs");
		if (auto *world = m_mainWindow->activeWorldChildWindow())
		{
			auto *runtime = world->runtime();
			if (!runtime)
				return;
			QString path = runtime->worldFilePath();
			if (saveAs || path.trimmed().isEmpty())
			{
				QString initialDir;
				QString suggestedName;
				if (!path.trimmed().isEmpty())
				{
					const QFileInfo info(path);
					initialDir    = info.absolutePath();
					suggestedName = info.fileName();
				}
				if (initialDir.isEmpty())
					initialDir = makeAbsolutePath(defaultWorldDirectory());
				if (initialDir.isEmpty())
					initialDir = m_fileBrowsingDir;
				if (suggestedName.isEmpty())
				{
					auto baseName = runtime->worldAttributes().value(QStringLiteral("name")).trimmed();
					if (baseName.isEmpty())
						baseName = QStringLiteral("World");
					static const auto invalid = QStringLiteral("<>\"|?:#%;/\\");
					for (const QChar &ch : invalid)
						baseName.replace(ch, QLatin1Char('_'));
					suggestedName = baseName;
				}
				if (!suggestedName.endsWith(QStringLiteral(".qdl"), Qt::CaseInsensitive))
					suggestedName += QStringLiteral(".qdl");
				const auto dialogPath =
				    initialDir.isEmpty() ? suggestedName : QDir(initialDir).filePath(suggestedName);
				const auto fileName = QFileDialog::getSaveFileName(
				    m_mainWindow, QStringLiteral("Save World"), dialogPath,
				    QStringLiteral("World files (*.qdl *.mcl);;All files (*.*)"));
				if (fileName.isEmpty())
					return;
				path = QMudFileExtensions::replaceOrAppendExtension(fileName, QStringLiteral("qdl"));
			}
			QString error;
			if (!runtime->saveWorldFile(path, &error))
			{
				QMessageBox::warning(m_mainWindow, QStringLiteral("Save World"),
				                     error.isEmpty() ? QStringLiteral("Unable to save the world file.")
				                                     : error);
				return;
			}
			runtime->setWorldFilePath(path);
			runtime->setWorldFileModified(false);
			runtime->setVariablesChanged(false);
			m_fileBrowsingDir = QFileInfo(path).absolutePath();
			m_mainWindow->showStatusMessage(QStringLiteral("World saved."), 2000);
			return;
		}
		if (auto *text = m_mainWindow->activeTextChildWindow())
		{
			QString error;
			if (!saveAs && text->saveToCurrentFile(&error))
			{
				m_mainWindow->showStatusMessage(QStringLiteral("File saved."), 2000);
				return;
			}
			auto initialDir = m_fileBrowsingDir;
			if (!text->filePath().isEmpty())
				initialDir = QFileInfo(text->filePath()).absolutePath();
			const auto fileName =
			    QFileDialog::getSaveFileName(m_mainWindow, QStringLiteral("Save File"), initialDir,
			                                 QStringLiteral("Text files (*.txt);;All files (*.*)"));
			if (fileName.isEmpty())
				return;
			const auto path = ensureExtension(fileName, QStringLiteral("txt"));
			if (!text->saveToFile(path, &error))
			{
				QMessageBox::warning(m_mainWindow, QStringLiteral("Save File"),
				                     error.isEmpty() ? QStringLiteral("Unable to save the file.") : error);
				return;
			}
			m_fileBrowsingDir = QFileInfo(path).absolutePath();
			m_mainWindow->showStatusMessage(QStringLiteral("File saved."), 2000);
		}
	}
	else if (cmdName == QStringLiteral("SaveSelection"))
	{
		if (!m_mainWindow)
			return;
		QString selected;
		if (auto *text = m_mainWindow->activeTextChildWindow())
		{
			if (auto *editor = text->editor())
			{
				if (auto cursor = editor->textCursor(); cursor.hasSelection())
				{
					selected = cursor.selectedText();
					selected.replace(QChar(0x2029), QLatin1Char('\n'));
				}
			}
		}
		else if (auto *world = m_mainWindow->activeWorldChildWindow())
		{
			if (auto *view = world->view())
			{
				selected = view->outputSelectionText();
				if (selected.isEmpty())
					selected = view->inputSelectionText();
			}
		}
		if (selected.isEmpty())
			return;
		changeToFileBrowsingDirectory();
		const auto fileName =
		    QFileDialog::getSaveFileName(m_mainWindow, QStringLiteral("Saved selection"),
		                                 QDir(m_fileBrowsingDir).filePath(QStringLiteral("selection.txt")),
		                                 QStringLiteral("Text files (*.txt)"));
		changeToStartupDirectory();
		if (fileName.isEmpty())
			return;
		const auto path = ensureExtension(fileName, QStringLiteral("txt"));
		if (QString error; !saveTextFile(path, selected, &error))
		{
			QMessageBox::warning(m_mainWindow, QStringLiteral("Save Selection"),
			                     error.isEmpty() ? QStringLiteral("Unable to save the selection.") : error);
			return;
		}
		m_fileBrowsingDir = QFileInfo(path).absolutePath();
		m_mainWindow->showStatusMessage(QStringLiteral("Selection saved."), 2000);
	}
	else if (cmdName == QStringLiteral("PrintSetup"))
	{
		if (!m_mainWindow)
			return;
		QPrinter printer(QPrinter::HighResolution);
		if (m_hasPrintSetup)
		{
			if (!m_printSetupPrinterName.isEmpty())
				printer.setPrinterName(m_printSetupPrinterName);
			if (m_printSetupLayout.isValid())
				printer.setPageLayout(m_printSetupLayout);
		}
		QPrintDialog dlg(&printer, m_mainWindow);
		dlg.setWindowTitle(QStringLiteral("Print Setup"));
		dlg.setOption(QAbstractPrintDialog::PrintSelection, false);
		if (dlg.exec() == QDialog::Accepted)
		{
			m_printSetupPrinterName = printer.printerName();
			m_printSetupLayout      = printer.pageLayout();
			m_hasPrintSetup         = true;
		}
	}
	else if (cmdName == QStringLiteral("ReloadDefaults"))
	{
		if (!m_mainWindow)
			return;
		auto *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *runtime = world->runtime();
		auto *view    = world->view();
		if (!runtime || !view)
			return;
		runtime->applyDefaultWorldOptions();
		view->applyRuntimeSettings();
		m_mainWindow->showStatusMessage(QStringLiteral("Defaults reloaded."), 2000);
	}
	else if (isCommand(QStringLiteral("WindowsSocketInfo")))
	{
		if (!m_mainWindow)
			return;
		QDialog dlg(m_mainWindow);
		dlg.setWindowTitle(QStringLiteral("Windows Socket Info"));
		auto *form = new QFormLayout(&dlg);

		auto *description = new QLabel(QStringLiteral("Qt Network"), &dlg);
		auto *status      = new QLabel(QStringLiteral("OK"), &dlg);
		auto *maxSockets  = new QLabel(QStringLiteral("n/a"), &dlg);
		auto *version     = new QLabel(QStringLiteral("n/a"), &dlg);
		auto *highVersion = new QLabel(QStringLiteral("n/a"), &dlg);

		auto *hostName  = new QLabel(QHostInfo::localHostName(), &dlg);
		auto *addresses = new QPlainTextEdit(&dlg);
		addresses->setReadOnly(true);
		QStringList addrLines;
		for (const auto addrs = QNetworkInterface::allAddresses(); const auto &addr : addrs)
		{
			if (addr.protocol() == QAbstractSocket::IPv4Protocol ||
			    addr.protocol() == QAbstractSocket::IPv6Protocol)
				addrLines << addr.toString();
		}
		addresses->setPlainText(addrLines.join(QLatin1Char('\n')));

		form->addRow(QStringLiteral("Description"), description);
		form->addRow(QStringLiteral("Status"), status);
		form->addRow(QStringLiteral("Max sockets"), maxSockets);
		form->addRow(QStringLiteral("Version"), version);
		form->addRow(QStringLiteral("High version"), highVersion);
		form->addRow(QStringLiteral("Host name"), hostName);
		form->addRow(QStringLiteral("Addresses"), addresses);

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, &dlg);
		form->addRow(buttons);
		connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		dlg.exec();
	}
	else if (cmdName == QStringLiteral("ConvertClipboardForumCodes"))
	{
		auto text = QGuiApplication::clipboard()->text();
		if (text.isEmpty())
			return;
		const auto converted = quoteForumCodes(text);
		QGuiApplication::clipboard()->setText(converted);
	}
	else if (cmdName == QStringLiteral("PasteToWorld"))
	{
		if (!m_mainWindow)
			return;
		auto *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *runtime = world->runtime();
		if (!runtime)
			return;
		const auto clip = QGuiApplication::clipboard()->text();
		if (clip.isEmpty())
			return;

		const auto &attrs = runtime->worldAttributes();
		const auto &multi = runtime->worldMultilineAttributes();
		if (isEnabledFlag(attrs.value(QStringLiteral("confirm_on_paste"))))
		{
			if (QMessageBox::question(m_mainWindow, QStringLiteral("Paste to World"),
			                          QStringLiteral("Paste the clipboard contents to the world?"),
			                          QMessageBox::Ok | QMessageBox::Cancel) != QMessageBox::Ok)
				return;
		}

		const auto preamble          = multi.value(QStringLiteral("paste_preamble"));
		const auto postamble         = multi.value(QStringLiteral("paste_postamble"));
		const auto linePre           = attrs.value(QStringLiteral("paste_line_preamble"));
		const auto linePost          = attrs.value(QStringLiteral("paste_line_postamble"));
		const auto echo              = isEnabledFlag(attrs.value(QStringLiteral("paste_echo")));
		const auto commentedSoftcode = isEnabledFlag(attrs.value(QStringLiteral("paste_commented_softcode")));
		int        delayMs           = attrs.value(QStringLiteral("paste_delay")).toInt();
		int        perLines          = attrs.value(QStringLiteral("paste_delay_per_lines")).toInt();
		if (perLines < 1)
			perLines = 1;

		auto lines = clip.split(QRegularExpression(QStringLiteral("\\r\\n|\\n|\\r")));
		if (lines.isEmpty())
			return;

		const auto trimLeft = [](const QString &text)
		{
			auto pos = 0;
			while (pos < text.size() && text.at(pos).isSpace())
				++pos;
			return text.mid(pos);
		};

		const auto waitDelay = [](const int ms)
		{
			if (ms <= 0)
				return;
			QElapsedTimer timer;
			timer.start();
			while (timer.elapsed() < ms)
			{
				QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
				QThread::msleep(10);
			}
		};

		auto       lineCount = 0;
		const auto sendLine  = [&](const QString &payload)
		{
			runtime->setCurrentActionSource(WorldRuntime::eUserMenuAction);
			(void)runtime->sendCommand(payload, echo, false, false, false, false);
			runtime->setCurrentActionSource(WorldRuntime::eUnknownActionSource);
			if (delayMs > 0 && ++lineCount >= perLines)
			{
				waitDelay(delayMs);
				lineCount = 0;
			}
		};

		if (!preamble.isEmpty())
		{
			runtime->setCurrentActionSource(WorldRuntime::eUserMenuAction);
			(void)runtime->sendCommand(preamble, echo, false, false, false, false);
			runtime->setCurrentActionSource(WorldRuntime::eUnknownActionSource);
		}
		if (commentedSoftcode)
		{
			QString softcode;
			bool    hashCommenting = false;
			bool    firstNonBlank  = true;
			for (const QString &rawLine : lines)
			{
				auto line = trimLeft(rawLine);
				if (line.isEmpty())
					continue;
				if (firstNonBlank && line.startsWith(QLatin1Char('#')))
					hashCommenting = true;
				firstNonBlank = false;
				if (line == QStringLiteral("-"))
				{
					sendLine(linePre + softcode + linePost);
					softcode.clear();
					continue;
				}
				if (hashCommenting)
				{
					if (line.startsWith(QLatin1Char('#')))
						continue;
				}
				else
				{
					if (const auto commentPos = line.indexOf(QStringLiteral("@@")); commentPos >= 0)
						line = line.left(commentPos);
				}
				line = trimLeft(line);
				softcode += line;
			}
			sendLine(linePre + softcode + linePost);
		}
		else
		{
			for (const QString &line : lines)
				sendLine(linePre + line + linePost);
		}
		if (!postamble.isEmpty())
		{
			runtime->setCurrentActionSource(WorldRuntime::eUserMenuAction);
			(void)runtime->sendCommand(postamble, echo, false, false, false, false);
			runtime->setCurrentActionSource(WorldRuntime::eUnknownActionSource);
		}
	}
	else if (cmdName == QStringLiteral("SpellCheck"))
	{
		if (!m_mainWindow)
			return;
#ifdef QMUD_ENABLE_LUA_SCRIPTING
		if (!ensureSpellCheckerLoaded())
			return;
		auto *spell = spellCheckerLuaState();
		if (!spell)
			return;

		QPlainTextEdit *edit = nullptr;
		if (auto *focus = QApplication::focusWidget(); auto *plain = qobject_cast<QPlainTextEdit *>(focus))
			edit = plain;
		if (!edit)
		{
			if (auto *world = m_mainWindow->activeWorldChildWindow())
			{
				if (auto *view = world->view())
					edit = view->inputEditor();
			}
		}
		if (!edit)
		{
			if (auto *text = m_mainWindow->activeTextChildWindow())
				edit = text->editor();
		}
		if (!edit)
			return;

		auto       cursor    = edit->textCursor();
		const auto origStart = cursor.selectionStart();
		const auto origEnd   = cursor.selectionEnd();

		auto       selected = cursor.selectedText();
		bool       all      = false;
		if (selected.isEmpty())
		{
			all      = true;
			selected = edit->toPlainText();
		}
		selected.replace(QChar(0x2029), QLatin1Char('\n'));

		lua_settop(spell, 0);
		lua_getglobal(spell, "spellcheck");
		if (!lua_isfunction(spell, -1))
		{
			lua_settop(spell, 0);
			return;
		}
		const auto textBytes = selected.toUtf8();
		lua_pushlstring(spell, textBytes.constData(), textBytes.size());
		lua_pushboolean(spell, all);
		if (const auto error = QMudLuaSupport::callLuaWithTraceback(spell, 2, 1); error)
		{
			QMudLuaSupport::luaError(spell, "Run-time error", "spellcheck", "Command-line spell-check");
			closeSpellChecker();
			lua_settop(spell, 0);
			return;
		}

		if (lua_isstring(spell, -1))
		{
			const auto replacement = QString::fromUtf8(lua_tostring(spell, -1));
			if (all)
				edit->selectAll();
			auto replaceCursor = edit->textCursor();
			replaceCursor.insertText(replacement);
		}
		lua_settop(spell, 0);

		auto restore = edit->textCursor();
		restore.setPosition(origStart);
		restore.setPosition(origEnd, QTextCursor::KeepAnchor);
		edit->setTextCursor(restore);
#else
		QMessageBox::information(m_mainWindow, QStringLiteral("Spell Check"),
		                         QStringLiteral("Spell check is not available in this build."));
#endif
	}
	else if (cmdName == QStringLiteral("GenerateCharacterName"))
	{
		if (!m_mainWindow)
			return;
		WorldRuntime *runtime = nullptr;
		if (auto *world = m_mainWindow->activeWorldChildWindow())
			runtime = world->runtime();
		GeneratedNameDialog dlg(runtime, m_mainWindow);
		dlg.exec();
	}
	else if (cmdName == QStringLiteral("ReloadNamesFile"))
	{
		try
		{
			qmudReadNames(QStringLiteral("*"), true);
			if (m_mainWindow)
				m_mainWindow->showStatusMessage(QStringLiteral("Names file reloaded."), 2000);
		}
		catch (const std::exception &e)
		{
			if (m_mainWindow)
				QMessageBox::warning(m_mainWindow, QStringLiteral("Reload Names File"),
				                     QString::fromUtf8(e.what()));
		}
	}
	else if (cmdName == QStringLiteral("GenerateUniqueId"))
	{
		if (!m_mainWindow)
			return;
		QDialog dlg(m_mainWindow);
		dlg.setWindowTitle(QStringLiteral("Unique ID"));
		auto *layout = new QVBoxLayout(&dlg);
		auto *label  = new QLabel(QStringLiteral("Unique ID:"), &dlg);
		auto *edit   = new QLineEdit(&dlg);
		edit->setReadOnly(true);
		const auto uniqueId = QMudWorldOptionDefaults::generateWorldUniqueId();
		edit->setText(uniqueId);
		layout->addWidget(label);
		layout->addWidget(edit);
		auto *buttonRow = new QHBoxLayout();
		auto *copy      = new QPushButton(QStringLiteral("Copy"), &dlg);
		auto *buttons   = new QDialogButtonBox(QDialogButtonBox::Ok, &dlg);
		buttonRow->addWidget(copy);
		buttonRow->addStretch(1);
		buttonRow->addWidget(buttons);
		layout->addLayout(buttonRow);
		connect(copy, &QPushButton::clicked, &dlg,
		        [edit] { QGuiApplication::clipboard()->setText(edit->text()); });
		connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		dlg.exec();
	}
	else if (cmdName == QStringLiteral("DebugPackets"))
	{
		if (!m_mainWindow)
			return;
		auto *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *runtime = world->runtime();
		if (!runtime)
			return;
		const auto enabled = !runtime->debugIncomingPackets();
		runtime->setDebugIncomingPackets(enabled);
		m_mainWindow->showStatusMessage(enabled ? QStringLiteral("Packet debugging enabled.")
		                                        : QStringLiteral("Packet debugging disabled."),
		                                2000);
		if (QAction *action = m_mainWindow->actionForCommand(QStringLiteral("DebugPackets")))
		{
			action->setCheckable(true);
			action->setChecked(enabled);
		}
	}
	else if (cmdName == QStringLiteral("DebugWorldInput"))
	{
		if (!m_mainWindow)
			return;
		WorldChildWindow *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		WorldRuntime *runtime = world->runtime();
		if (!runtime)
			return;

		QDialog dlg(m_mainWindow);
		dlg.setWindowTitle(QStringLiteral("Debug World Input"));
		auto *layout = new QVBoxLayout(&dlg);
		auto *label =
		    new QLabel(QStringLiteral("Enter bytes to simulate incoming world data. Use \\\\ for backslash, "
		                              "\\\\HH for hex byte pairs."),
		               &dlg);
		label->setWordWrap(true);
		auto *text = new QPlainTextEdit(&dlg);
		text->setPlainText(m_lastDebugWorldInput);
		text->setMinimumSize(520, 220);
		layout->addWidget(label);
		layout->addWidget(text);
		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
		layout->addWidget(buttons);
		connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
		if (dlg.exec() != QDialog::Accepted)
			return;

		m_lastDebugWorldInput    = text->toPlainText();
		const QByteArray payload = decodeDebugWorldInput(m_lastDebugWorldInput);
		if (payload.isEmpty())
			return;

		const bool wasSimulating = runtime->doingSimulate();
		runtime->setDoingSimulate(true);
		runtime->receiveRawData(payload);
		runtime->setDoingSimulate(wasSimulating);
	}
	else if (cmdName == QStringLiteral("FullScreenMode"))
	{
		if (!m_mainWindow)
			return;
		const bool isFull = m_mainWindow->windowState() & Qt::WindowFullScreen;
		if (isFull)
			m_mainWindow->showNormal();
		else
			m_mainWindow->showFullScreen();
		m_mainWindow->setFullScreenMode(!isFull);
		if (QAction *action = m_mainWindow->actionForCommand(QStringLiteral("FullScreenMode")))
		{
			action->setCheckable(true);
			action->setChecked(!isFull);
		}
	}
	else if (cmdName == QStringLiteral("AutoConnect"))
	{
		const int current = getGlobalOption(QStringLiteral("AutoConnectWorlds")).toInt();
		const int next    = current == 0 ? 1 : 0;
		setGlobalOptionInt(QStringLiteral("AutoConnectWorlds"), next);
		if (QAction *action =
		        m_mainWindow ? m_mainWindow->actionForCommand(QStringLiteral("AutoConnect")) : nullptr)
		{
			action->setCheckable(true);
			action->setChecked(next != 0);
		}
	}
	else if (cmdName == QStringLiteral("ReconnectOnDisconnect"))
	{
		const int current = getGlobalOption(QStringLiteral("ReconnectOnLinkFailure")).toInt();
		const int next    = current == 0 ? 1 : 0;
		setGlobalOptionInt(QStringLiteral("ReconnectOnLinkFailure"), next);
		for (const QVector<WorldRuntime *> runtimes = activeWorldRuntimes(); WorldRuntime *runtime : runtimes)
			if (runtime)
				runtime->setReconnectOnLinkFailure(next != 0);
		if (QAction *action = m_mainWindow
		                          ? m_mainWindow->actionForCommand(QStringLiteral("ReconnectOnDisconnect"))
		                          : nullptr)
		{
			action->setCheckable(true);
			action->setChecked(next != 0);
		}
	}
	else if (cmdName == QStringLiteral("ConnectToAllOpenWorlds") ||
	         cmdName == QStringLiteral("ConnectToWorldsInStartupList"))
	{
		auto connectRuntime = [](WorldRuntime *runtime)
		{
			if (!runtime)
				return;
			if (runtime->isConnected() || runtime->isConnecting())
				return;
			const QMap<QString, QString> &attrs     = runtime->worldAttributes();
			const QString                 worldName = attrs.value(QStringLiteral("name"));
			const QString                 host      = attrs.value(QStringLiteral("site"));
			const int                     port      = attrs.value(QStringLiteral("port")).toInt();
			if (host == QStringLiteral("0.0.0.0"))
				return;
			if (worldName.isEmpty() || host.isEmpty() || port <= 0)
				return;
			runtime->connectToWorld(host, static_cast<quint16>(port));
		};

		if (cmdName == QStringLiteral("ConnectToAllOpenWorlds"))
		{
			for (const QVector<WorldRuntime *> runtimes = activeWorldRuntimes();
			     WorldRuntime *runtime : runtimes)
				connectRuntime(runtime);
			return;
		}

		QString       worldList         = getGlobalOption(QStringLiteral("WorldList")).toString();
		bool          worldListChanged  = false;
		const QString migratedWorldList = migrateWorldListPaths(m_workingDir, worldList, &worldListChanged);
		if (worldListChanged)
		{
			setGlobalOptionString(QStringLiteral("WorldList"), migratedWorldList);
			worldList = migratedWorldList;
		}
		worldList = canonicalizeWorldListForRuntime(worldList);
		if (worldList.isEmpty())
			return;
		const QStringList items = splitSerializedWorldList(worldList);
		openWorldsFromList(items, false);

		for (WorldRuntime *runtime : activeWorldRuntimes())
			connectRuntime(runtime);
	}
	else if (cmdName == QStringLiteral("QuitFromWorld"))
	{
		if (!m_mainWindow)
			return;
		WorldChildWindow *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		WorldRuntime *runtime = world->runtime();
		if (!runtime)
			return;
		const QMap<QString, QString> &attrs     = runtime->worldAttributes();
		const QString                 worldName = attrs.value(QStringLiteral("name"));
		const QString                 prompt    = QStringLiteral("Quit from %1?")
		                           .arg(worldName.isEmpty() ? QStringLiteral("this world") : worldName);
		if (QMessageBox::question(m_mainWindow, QStringLiteral("Quit"), prompt,
		                          QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
			return;
		runtime->setDisconnectOk(true);
		const bool echo     = isEnabledFlag(attrs.value(QStringLiteral("display_my_input")));
		const bool logInput = isEnabledFlag(attrs.value(QStringLiteral("log_input")));
		runtime->setCurrentActionSource(WorldRuntime::eUserMenuAction);
		(void)runtime->sendCommand(QStringLiteral("quit"), echo, false, logInput, true, false);
		runtime->setCurrentActionSource(WorldRuntime::eUnknownActionSource);
	}
	else if (cmdName == QStringLiteral("DiscardQueuedCommands"))
	{
		if (!m_mainWindow)
			return;
		WorldChildWindow *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		WorldRuntime *runtime = world->runtime();
		if (!runtime)
			return;
		const int discarded = runtime->discardQueuedCommands();
		m_mainWindow->showStatusMessage(QStringLiteral("Discarded %1 queued command%2.")
		                                    .arg(discarded)
		                                    .arg(discarded == 1 ? QString() : QStringLiteral("s")),
		                                2000);
	}
	else if (cmdName == QStringLiteral("SendFile"))
	{
		if (!m_mainWindow)
			return;
		WorldChildWindow *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		WorldRuntime *runtime = world->runtime();
		if (!runtime)
			return;

		QString       initialDir = m_fileBrowsingDir;
		const QString worldName =
		    runtime->worldAttributes().value(QStringLiteral("name"), QStringLiteral("world"));
		const QString dialogTitle = QStringLiteral("File to paste into %1").arg(worldName);
		const QString fileName    = QFileDialog::getOpenFileName(
            m_mainWindow, dialogTitle, initialDir,
            QStringLiteral("MUD files (*.mud;*.mush);;Text files (*.txt);;All files (*.*)"));
		if (fileName.isEmpty())
			return;

		QFile file(fileName);
		if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
		{
			QMessageBox::warning(m_mainWindow, QStringLiteral("Send File"),
			                     QStringLiteral("Unable to open the selected file."));
			return;
		}

		const QMap<QString, QString> &attrs = runtime->worldAttributes();
		const QMap<QString, QString> &multi = runtime->worldMultilineAttributes();

		m_fileBrowsingDir = QFileInfo(fileName).absolutePath();

		QTextStream countStream(&file);
		int         totalLines = 0;
		while (!countStream.atEnd())
		{
			countStream.readLine();
			++totalLines;
		}
		file.seek(0);

		QString filePreamble   = multi.value(QStringLiteral("send_to_world_file_preamble"));
		QString linePreamble   = attrs.value(QStringLiteral("send_to_world_line_preamble"));
		QString linePostamble  = attrs.value(QStringLiteral("send_to_world_line_postamble"));
		QString filePostamble  = multi.value(QStringLiteral("send_to_world_file_postamble"));
		bool commentedSoftcode = isEnabledFlag(attrs.value(QStringLiteral("send_file_commented_softcode")));
		int  delayMs           = attrs.value(QStringLiteral("send_file_delay")).toInt();
		int  perLines          = attrs.value(QStringLiteral("send_file_delay_per_lines")).toInt();
		if (perLines < 1)
			perLines = 1;
		bool          echo     = isEnabledFlag(attrs.value(QStringLiteral("send_echo")));
		const bool    logInput = isEnabledFlag(attrs.value(QStringLiteral("log_input")));
		const bool    confirm  = isEnabledFlag(attrs.value(QStringLiteral("confirm_on_send")));

		const qint64  length  = file.size();
		const QString message = QStringLiteral("About to send: %1 character%2, %3 line%4 to %5.")
		                            .arg(length)
		                            .arg(length == 1 ? QString() : QStringLiteral("s"))
		                            .arg(totalLines)
		                            .arg(totalLines == 1 ? QString() : QStringLiteral("s"))
		                            .arg(worldName);

		if (confirm)
		{
			ConfirmPreambleDialog dlg(m_mainWindow);
			dlg.setPasteMessage(message);
			dlg.setFilePreamble(filePreamble);
			dlg.setLinePreamble(linePreamble);
			dlg.setLinePostamble(linePostamble);
			dlg.setFilePostamble(filePostamble);
			dlg.setCommentedSoftcode(commentedSoftcode);
			dlg.setLineDelayMs(delayMs);
			dlg.setDelayPerLines(perLines);
			dlg.setEcho(echo);
			if (dlg.exec() != QDialog::Accepted)
				return;
			filePreamble      = dlg.filePreamble();
			linePreamble      = dlg.linePreamble();
			linePostamble     = dlg.linePostamble();
			filePostamble     = dlg.filePostamble();
			commentedSoftcode = dlg.commentedSoftcode();
			delayMs           = dlg.lineDelayMs();
			perLines          = qMax(1, dlg.delayPerLines());
			echo              = dlg.echo();
		}

		auto trimLeft = [](const QString &text)
		{
			int pos = 0;
			while (pos < text.size() && text.at(pos).isSpace())
				++pos;
			return text.mid(pos);
		};

		auto waitDelay = [](const int ms)
		{
			if (ms <= 0)
				return;
			QElapsedTimer timer;
			timer.start();
			while (timer.elapsed() < ms)
			{
				QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
				QThread::msleep(10);
			}
		};

		QProgressDialog progress(QStringLiteral("Sending to world..."), QStringLiteral("Cancel"), 0,
		                         totalLines, m_mainWindow);
		progress.setWindowTitle(QStringLiteral("Sending..."));
		progress.setMinimumDuration(0);
		progress.setValue(0);

		QTextStream stream(&file);
		int         currentLine = 0;
		int         lineCount   = 0;
		QString     softcode;
		bool        hashCommenting = false;
		bool        firstNonBlank  = true;

		auto        sendLine = [&](const QString &payload)
		{
			runtime->setCurrentActionSource(WorldRuntime::eUserMenuAction);
			(void)runtime->sendCommand(payload, echo, false, logInput, true, false);
			runtime->setCurrentActionSource(WorldRuntime::eUnknownActionSource);
			if (delayMs > 0 && ++lineCount >= perLines)
			{
				waitDelay(delayMs);
				lineCount = 0;
			}
		};

		if (!filePreamble.isEmpty())
			sendLine(filePreamble);

		while (!stream.atEnd())
		{
			QString line = stream.readLine();
			++currentLine;
			progress.setValue(currentLine);
			QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
			if (progress.wasCanceled())
				break;

			if (commentedSoftcode)
			{
				line = trimLeft(line);
				if (line.isEmpty())
					continue;
				if (firstNonBlank)
				{
					if (line.startsWith(QLatin1Char('#')))
						hashCommenting = true;
					firstNonBlank = false;
				}
				if (line == QStringLiteral("-"))
				{
					const QString fullLine = linePreamble + softcode + linePostamble;
					sendLine(fullLine);
					softcode.clear();
					continue;
				}
				if (hashCommenting)
				{
					if (line.startsWith(QLatin1Char('#')))
						continue;
				}
				else
				{
					if (const qsizetype pos = line.indexOf(QStringLiteral("@@")); pos != -1)
						line = line.left(pos);
				}
				line = trimLeft(line);
				softcode += line;
				continue;
			}

			const QString fullLine = linePreamble + line + linePostamble;
			sendLine(fullLine);
		}

		if (commentedSoftcode)
		{
			const QString fullLine = linePreamble + softcode + linePostamble;
			sendLine(fullLine);
		}

		if (!filePostamble.isEmpty())
			sendLine(filePostamble);

		file.close();
	}
	else if (cmdName == QStringLiteral("FunctionList") || cmdName == QStringLiteral("CompleteFunction"))
	{
		auto *editor = resolveActiveTextEditor(m_mainWindow);
		if (!editor)
			return;

		const QStringList &allNames = internalFunctionNames();
		if (allNames.isEmpty())
			return;

		QTextCursor   cursor   = editor->textCursor();
		const QString fullText = editor->toPlainText();
		int           start    = cursor.selectionStart();
		int           end      = cursor.selectionEnd();
		if (!cursor.hasSelection())
		{
			const int position = cursor.position();
			start              = position;
			while (start > 0 && isTokenCharacter(fullText.at(start - 1)))
				--start;
			end = position;
			while (end < fullText.size() && isTokenCharacter(fullText.at(end)))
				++end;
		}

		const int     textSize      = static_cast<int>(fullText.size());
		const int     safeStart     = qMax(0, qMin(start, textSize));
		const int     safeEnd       = qMax(safeStart, qMin(end, textSize));
		const int     cursorPos     = qMax(safeStart, qMin(cursor.position(), safeEnd));
		const QString selectedToken = fullText.mid(safeStart, safeEnd - safeStart);
		QString       prefix        = fullText.mid(safeStart, cursorPos - safeStart);
		if (prefix.isEmpty())
			prefix = selectedToken;

		QString chosen;
		if (cmdName == QStringLiteral("CompleteFunction"))
		{
			QStringList matches;
			if (!prefix.isEmpty())
			{
				for (const QString &name : allNames)
				{
					if (name.startsWith(prefix, Qt::CaseInsensitive))
						matches.push_back(name);
				}
			}
			else
			{
				matches = allNames;
			}

			if (matches.isEmpty())
			{
				if (m_mainWindow)
					m_mainWindow->showStatusMessage(QStringLiteral("No matching function names."), 2000);
				return;
			}
			if (matches.size() == 1)
				chosen = matches.first();
			else
				chosen = chooseInternalFunction(m_mainWindow, matches, prefix);
		}
		else
		{
			const QString initialFilter =
			    !selectedToken.trimmed().isEmpty() ? selectedToken.trimmed() : m_lastFunctionListFilter;
			chosen = chooseInternalFunction(m_mainWindow, allNames, initialFilter);
		}

		if (chosen.isEmpty())
			return;

		m_lastFunctionListFilter  = chosen;
		QTextCursor replaceCursor = editor->textCursor();
		replaceCursor.setPosition(safeStart);
		replaceCursor.setPosition(safeEnd, QTextCursor::KeepAnchor);
		replaceCursor.insertText(chosen);
		editor->setTextCursor(replaceCursor);
	}
	else if (cmdName == QStringLiteral("GlobalReplace"))
	{
		if (!m_mainWindow)
			return;
		auto *editor = resolveActiveTextEditor(m_mainWindow);
		if (!editor)
			return;

		QDialog dlg(m_mainWindow);
		dlg.setWindowTitle(QStringLiteral("Global Replace"));
		auto *layout       = new QGridLayout(&dlg);
		auto *findLabel    = new QLabel(QStringLiteral("Find pattern:"), &dlg);
		auto *replaceLabel = new QLabel(QStringLiteral("Replace with:"), &dlg);
		auto *findEdit     = new QLineEdit(m_lastGlobalReplaceFind, &dlg);
		auto *replaceEdit  = new QLineEdit(m_lastGlobalReplaceReplace, &dlg);
		auto *regexp       = new QCheckBox(QStringLiteral("Use regular expression"), &dlg);
		regexp->setChecked(m_lastGlobalReplaceRegexp);
		auto *eachLine = new QCheckBox(QStringLiteral("Replace each line separately"), &dlg);
		eachLine->setChecked(m_lastGlobalReplaceEachLine);
		auto *escapes = new QCheckBox(QStringLiteral("Translate escape sequences"), &dlg);
		escapes->setChecked(m_lastGlobalReplaceEscapeSequences);
		layout->addWidget(findLabel, 0, 0);
		layout->addWidget(findEdit, 0, 1);
		layout->addWidget(replaceLabel, 1, 0);
		layout->addWidget(replaceEdit, 1, 1);
		layout->addWidget(regexp, 2, 0, 1, 2);
		layout->addWidget(eachLine, 3, 0, 1, 2);
		layout->addWidget(escapes, 4, 0, 1, 2);
		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
		layout->addWidget(buttons, 5, 0, 1, 2);
		connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
		if (dlg.exec() != QDialog::Accepted)
			return;

		m_lastGlobalReplaceFind            = findEdit->text();
		m_lastGlobalReplaceReplace         = replaceEdit->text();
		m_lastGlobalReplaceRegexp          = regexp->isChecked();
		m_lastGlobalReplaceEachLine        = eachLine->isChecked();
		m_lastGlobalReplaceEscapeSequences = escapes->isChecked();

		QString pattern     = m_lastGlobalReplaceFind;
		QString replacement = m_lastGlobalReplaceReplace;
		if (m_lastGlobalReplaceEscapeSequences)
		{
			pattern     = decodeEscapes(pattern);
			replacement = decodeEscapes(replacement);
		}

		if (pattern.isEmpty())
		{
			QMessageBox::information(m_mainWindow, QStringLiteral("Global Replace"),
			                         QStringLiteral("Find pattern cannot be empty."));
			return;
		}

		QTextCursor cursor       = editor->textCursor();
		const bool  hasSelection = cursor.hasSelection();
		QString     source       = hasSelection ? cursor.selectedText() : editor->toPlainText();
		source.replace(QChar(0x2029), QLatin1Char('\n'));

		bool               changed     = false;
		QString            transformed = source;
		QRegularExpression regexPattern;
		if (m_lastGlobalReplaceRegexp)
		{
			regexPattern = QRegularExpression(pattern);
			if (!regexPattern.isValid())
			{
				QMessageBox::warning(
				    m_mainWindow, QStringLiteral("Global Replace"),
				    QStringLiteral("Invalid regular expression: %1").arg(regexPattern.errorString()));
				return;
			}
		}

		auto replaceChunk = [&](const QString &chunk) -> QString
		{
			QString out = chunk;
			if (m_lastGlobalReplaceRegexp)
			{
				out.replace(regexPattern, replacement);
			}
			else
			{
				out.replace(pattern, replacement);
			}
			if (!changed && out != chunk)
				changed = true;
			return out;
		};

		if (m_lastGlobalReplaceEachLine)
		{
			const QStringList lines = source.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
			QStringList       output;
			output.reserve(lines.size());
			for (const QString &line : lines)
				output.push_back(replaceChunk(line));
			transformed = output.join(QLatin1Char('\n'));
		}
		else
		{
			transformed = replaceChunk(source);
		}

		if (!changed)
		{
			QMessageBox::information(m_mainWindow, QStringLiteral("Global Replace"),
			                         QStringLiteral("No replacements made."));
			return;
		}

		QTextCursor replaceCursor = editor->textCursor();
		if (!hasSelection)
			replaceCursor.select(QTextCursor::Document);
		replaceCursor.insertText(transformed);
		m_mainWindow->showStatusMessage(QStringLiteral("Replacement complete."), 2000);
	}
	else if (isCommand(QStringLiteral("GlobalChange")))
	{
		if (!m_mainWindow)
			return;
		WorldChildWindow *world = m_mainWindow->activeWorldChildWindow();
		if (!world)
			return;
		auto *view = world->view();
		if (!view)
			return;
		auto *editor = view->inputEditor();
		if (!editor)
			return;

		QDialog dlg(m_mainWindow);
		dlg.setWindowTitle(QStringLiteral("Global Change"));
		auto *layout    = new QGridLayout(&dlg);
		auto *fromLabel = new QLabel(QStringLiteral("Change from:"), &dlg);
		auto *toLabel   = new QLabel(QStringLiteral("Change to:"), &dlg);
		auto *fromEdit  = new QLineEdit(m_lastGlobalChangeFrom, &dlg);
		auto *toEdit    = new QLineEdit(m_lastGlobalChangeTo, &dlg);
		layout->addWidget(fromLabel, 0, 0);
		layout->addWidget(fromEdit, 0, 1);
		layout->addWidget(toLabel, 1, 0);
		layout->addWidget(toEdit, 1, 1);
		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
		layout->addWidget(buttons, 2, 0, 1, 2);
		connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
		if (dlg.exec() != QDialog::Accepted)
			return;

		m_lastGlobalChangeFrom = fromEdit->text();
		m_lastGlobalChangeTo   = toEdit->text();

		const QString fromDecoded = decodeEscapes(m_lastGlobalChangeFrom);
		const QString toDecoded   = decodeEscapes(m_lastGlobalChangeTo);

		QTextCursor   cursor = editor->textCursor();
		const int     start  = cursor.selectionStart();
		const int     end    = cursor.selectionEnd();
		QString       text   = editor->toPlainText();
		text.replace(QStringLiteral("\r"), QString());

		QString target       = text;
		bool    hadSelection = end > start;
		if (hadSelection)
			target = text.mid(start, end - start);

		QString replaced = target;
		replaced.replace(fromDecoded, toDecoded);
		if (replaced == target)
		{
			QMessageBox::information(
			    m_mainWindow, QStringLiteral("Global Change"),
			    QStringLiteral("No replacements made for \"%1\".").arg(m_lastGlobalChangeFrom));
			return;
		}

		if (!hadSelection)
		{
			editor->selectAll();
		}
		else
		{
			QTextCursor sel = editor->textCursor();
			sel.setPosition(start);
			sel.setPosition(end, QTextCursor::KeepAnchor);
			editor->setTextCursor(sel);
		}
		editor->textCursor().insertText(replaced);
	}
	else if (cmdName == QStringLiteral("KeyName"))
	{
		if (!m_mainWindow)
			return;
		QDialog dlg(m_mainWindow);
		dlg.setWindowTitle(QStringLiteral("Key Name"));
		auto *layout    = new QGridLayout(&dlg);
		auto *keyLabel  = new QLabel(QStringLiteral("Key:"), &dlg);
		auto *nameLabel = new QLabel(QStringLiteral("Name:"), &dlg);
		auto *keyEdit   = new QKeySequenceEdit(&dlg);
		auto *nameEdit  = new QLineEdit(&dlg);
		nameEdit->setReadOnly(true);
		layout->addWidget(keyLabel, 0, 0);
		layout->addWidget(keyEdit, 0, 1);
		layout->addWidget(nameLabel, 1, 0);
		layout->addWidget(nameEdit, 1, 1);
		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, &dlg);
		layout->addWidget(buttons, 2, 0, 1, 2);
		connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		connect(keyEdit, &QKeySequenceEdit::keySequenceChanged, &dlg, [nameEdit](const QKeySequence &seq)
		        { nameEdit->setText(seq.toString(QKeySequence::NativeText)); });
		dlg.exec();
	}
	else if (cmdName == QStringLiteral("Print"))
	{
		if (!m_mainWindow)
			return;
		WorldChildWindow *world      = m_mainWindow->activeWorldChildWindow();
		TextChildWindow  *textWindow = m_mainWindow->activeTextChildWindow();
		if (!world && !textWindow)
			return;

		WorldRuntime *runtime = world ? world->runtime() : nullptr;
		WorldView    *view    = world ? world->view() : nullptr;
		const QString docName = world        ? world->windowTitle()
		                        : textWindow ? textWindow->windowTitle()
		                                     : QStringLiteral("QMud");

		const int     printerFontSize  = getGlobalOption(QStringLiteral("PrinterFontSize")).toInt();
		const int     leftMarginMm     = getGlobalOption(QStringLiteral("PrinterLeftMargin")).toInt();
		const int     topMarginMm      = getGlobalOption(QStringLiteral("PrinterTopMargin")).toInt();
		const int     linesPerPagePref = getGlobalOption(QStringLiteral("PrinterLinesPerPage")).toInt();
		QString       printerFontName  = getGlobalOption(QStringLiteral("PrinterFont")).toString();
		if (printerFontName.trimmed().isEmpty())
			printerFontName = QStringLiteral("Courier");

		QPrinter printer(QPrinter::HighResolution);
		if (m_hasPrintSetup)
		{
			if (!m_printSetupPrinterName.isEmpty())
				printer.setPrinterName(m_printSetupPrinterName);
			if (m_printSetupLayout.isValid())
				printer.setPageLayout(m_printSetupLayout);
		}
		printer.setDocName(docName);
		printer.setPageMargins(QMarginsF(leftMarginMm, topMarginMm, leftMarginMm, topMarginMm),
		                       QPageLayout::Millimeter);

		QPrintDialog dlg(&printer, m_mainWindow);
		bool         hasSelection = false;
		if (world && view && !view->outputSelectionText().isEmpty())
			hasSelection = true;
		if (!world && textWindow)
		{
			if (QPlainTextEdit *editor = textWindow->editor())
				hasSelection = editor->textCursor().hasSelection();
		}
		if (hasSelection)
			dlg.setOption(QAbstractPrintDialog::PrintSelection, true);
		else
			dlg.setOption(QAbstractPrintDialog::PrintSelection, false);
		if (dlg.exec() != QDialog::Accepted)
			return;
		m_printSetupPrinterName = printer.printerName();
		m_printSetupLayout      = printer.pageLayout();
		m_hasPrintSetup         = true;

		const bool selectionOnly = dlg.printRange() == QAbstractPrintDialog::Selection;

		auto       drawHeader = [&](QPainter &painter, const QRect &pageRect, const int pageNumber)
		{
			QFont headerFont(printerFontName, printerFontSize);
			headerFont.setBold(true);
			headerFont.setUnderline(true);
			painter.setFont(headerFont);
			const QString timeText =
			    QDateTime::currentDateTime().toString(QStringLiteral("dddd, MMMM dd, yyyy, h:mm AP"));
			const QString header = QStringLiteral("%1 - %2").arg(docName, timeText);
			const int     x      = pageRect.left();
			const int     y      = pageRect.top() + painter.fontMetrics().ascent();
			painter.drawText(x, y, header);
			QFont footerFont(printerFontName, printerFontSize);
			footerFont.setBold(true);
			footerFont.setUnderline(false);
			painter.setFont(footerFont);
			const QString footer  = QStringLiteral("Page %1").arg(pageNumber);
			const int     footerY = pageRect.bottom() - painter.fontMetrics().descent();
			painter.drawText(pageRect.left(), footerY, footer);
		};

		auto printPlainLines = [&](const QStringList &lines)
		{
			if (lines.isEmpty())
				return;
			QPainter    painter(&printer);
			const QFont font(printerFontName, printerFontSize);
			painter.setFont(font);
			const QFontMetrics metrics(font);
			const int          lineHeight = metrics.lineSpacing();
			const QRect        pageRect   = printer.pageRect(QPrinter::DevicePixel).toRect();
			const int          linesPerPage =
                linesPerPagePref > 0 ? linesPerPagePref : qMax(1, pageRect.height() / qMax(1, lineHeight));
			const int contentLines = qMax(1, linesPerPage - 4);
			int       pageNumber   = 1;
			int       lineOnPage   = 0;
			drawHeader(painter, pageRect, pageNumber);
			int y = pageRect.top() + lineHeight * 2;
			for (const QString &lineText : lines)
			{
				if (lineOnPage >= contentLines)
				{
					painter.end();
					printer.newPage();
					painter.begin(&printer);
					painter.setFont(font);
					drawHeader(painter, pageRect, ++pageNumber);
					y          = pageRect.top() + lineHeight * 2;
					lineOnPage = 0;
				}
				painter.drawText(pageRect.left(), y + metrics.ascent(), lineText);
				y += lineHeight;
				++lineOnPage;
			}
		};

		auto printRuntimeLines = [&](const QVector<WorldRuntime::LineEntry> &lines)
		{
			if (lines.isEmpty())
				return;
			QPainter    painter(&printer);
			const QFont baseFont(printerFontName, printerFontSize);
			painter.setFont(baseFont);
			const QFontMetrics baseMetrics(baseFont);
			const int          lineHeight = baseMetrics.lineSpacing();
			const QRect        pageRect   = printer.pageRect(QPrinter::DevicePixel).toRect();
			const int          linesPerPage =
                linesPerPagePref > 0 ? linesPerPagePref : qMax(1, pageRect.height() / qMax(1, lineHeight));
			const int contentLines = qMax(1, linesPerPage - 4);
			int       pageNumber   = 1;
			int       lineOnPage   = 0;
			drawHeader(painter, pageRect, pageNumber);
			int y = pageRect.top() + lineHeight * 2;
			for (const WorldRuntime::LineEntry &entry : lines)
			{
				if (lineOnPage >= contentLines)
				{
					painter.end();
					printer.newPage();
					painter.begin(&printer);
					painter.setFont(baseFont);
					drawHeader(painter, pageRect, ++pageNumber);
					y          = pageRect.top() + lineHeight * 2;
					lineOnPage = 0;
				}

				int x = pageRect.left();
				if (entry.spans.isEmpty())
				{
					painter.setFont(baseFont);
					painter.setPen(Qt::black);
					painter.drawText(x, y + baseMetrics.ascent(), entry.text);
				}
				else
				{
					int offset = 0;
					for (const WorldRuntime::StyleSpan &span : entry.spans)
					{
						if (span.length <= 0 || offset >= entry.text.size())
							continue;
						const QString chunk = entry.text.mid(offset, span.length);
						QFont         font  = baseFont;
						font.setBold(span.bold);
						font.setItalic(span.italic);
						font.setUnderline(span.underline);
						painter.setFont(font);
						const QColor fore = span.fore.isValid() ? span.fore : QColor(Qt::black);
						painter.setPen(fore);
						painter.drawText(x, y + baseMetrics.ascent(), chunk);
						x += QFontMetrics(font).horizontalAdvance(chunk);
						offset += span.length;
					}
				}
				y += lineHeight;
				++lineOnPage;
			}
		};

		if (world && runtime)
		{
			if (selectionOnly && view)
			{
				if (const QString selection = view->outputSelectionText(); !selection.isEmpty())
					printPlainLines(selection.split(QRegularExpression(QStringLiteral("\\r\\n|\\n|\\r"))));
				else
					printRuntimeLines(runtime->lines());
			}
			else
				printRuntimeLines(runtime->lines());
		}
		else if (textWindow)
		{
			QString text;
			if (QPlainTextEdit *editor = textWindow->editor())
			{
				if (selectionOnly && editor->textCursor().hasSelection())
					text = editor->textCursor().selectedText();
				else
					text = editor->toPlainText();
			}
			if (text.isEmpty())
				return;
			text.replace(QChar(0x2029), QLatin1Char('\n'));
			printPlainLines(text.split(QRegularExpression(QStringLiteral("\\r\\n|\\n|\\r"))));
		}
	}
	else
		qDebug() << "Unhandled command:" << cmdName;
}

void AppController::handleOutputFind(const bool again, const bool forceDirection, const bool forwards) const
{
	if (!m_mainWindow)
		return;
	const WorldChildWindow *world = m_mainWindow->activeWorldChildWindow();
	if (!world)
		return;
	if (WorldView *view = world->view(); !view)
		return;
	else
	{
		if (forceDirection)
			view->setOutputFindDirection(forwards);
		view->doOutputFind(again);
	}
	m_mainWindow->updateEditActions();
}

bool AppController::activateNotepad(const QString &title) const
{
	if (!m_mainWindow)
		return false;
	return m_mainWindow->activateNotepad(title);
}

bool AppController::appendToNotepad(const QString &title, const QString &text, const bool replace) const
{
	if (!m_mainWindow)
		return false;
	return m_mainWindow->appendToNotepad(title, text, replace);
}

bool AppController::sendToNotepad(const QString &title, const QString &text) const
{
	if (!m_mainWindow)
		return false;
	return m_mainWindow->sendToNotepad(title, text);
}

void AppController::handleAppAbout()
{
	QDialog dialog(m_mainWindow);
	dialog.setWindowTitle(QStringLiteral("About QMud"));
	dialog.setWindowFlags(dialog.windowFlags() | Qt::Tool);
	dialog.setFixedSize(700, 480);

	QVBoxLayout mainLayout(&dialog);
	auto        topLayout  = std::make_unique<QHBoxLayout>();
	auto        textLayout = std::make_unique<QVBoxLayout>();
	QLabel      iconLabel(&dialog);
	iconLabel.setFixedSize(256, 256);
	iconLabel.setAlignment(Qt::AlignCenter);
	if (const QPixmap iconPixmap(QStringLiteral(":/qmud/res/QMud.png")); !iconPixmap.isNull())
		iconLabel.setPixmap(iconPixmap.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation));

	QLabel title(QStringLiteral("QMud"), &dialog);
	QFont  titleFont = title.font();
	titleFont.setBold(true);
	title.setFont(titleFont);
	textLayout->addWidget(&title);

	QLabel version(QStringLiteral("Version %1").arg(QString::fromLatin1(kVersionString)), &dialog);
	textLayout->addWidget(&version);

	QLabel copyright(QStringLiteral("Copyright (C) 2026 Panagiotis Kalogiratos\n"), &dialog);
	textLayout->addWidget(&copyright);

	QLabel support(QStringLiteral("Support (CthulhuMUD Discord): <a href=\"%1\">%1</a>")
	                   .arg(QStringLiteral("https://discord.gg/secxwnTJCq")),
	               &dialog);
	support.setOpenExternalLinks(true);
	textLayout->addWidget(&support);
	QLabel disclaimer(
	    QStringLiteral(
	        "See License Agreement and GPL v3 for limitation of liability for use of this program."),
	    &dialog);
	disclaimer.setWordWrap(true);
	textLayout->addWidget(&disclaimer);

	topLayout->addLayout(textLayout.release(), 1);
	topLayout->addWidget(&iconLabel, 0, Qt::AlignTop | Qt::AlignRight);
	mainLayout.addLayout(topLayout.release());

	QDialogButtonBox buttons(QDialogButtonBox::Ok, &dialog);
	QPushButton      credits(QStringLiteral("Credits..."), &dialog);
	QPushButton      license(QStringLiteral("License ..."), &dialog);
	buttons.addButton(&credits, QDialogButtonBox::ActionRole);
	buttons.addButton(&license, QDialogButtonBox::ActionRole);
	mainLayout.addWidget(&buttons);
	connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(&credits, &QPushButton::clicked, &dialog,
	        [this]
	        {
		        const QString text = loadResourceText(QStringLiteral(":/qmud/text/credits.txt"));
		        showTextDialog(m_mainWindow, QStringLiteral("Credits"),
		                       text.isEmpty() ? QStringLiteral("Could not load text.") : text);
	        });
	connect(&license, &QPushButton::clicked, &dialog,
	        [this]
	        {
		        const QString text = loadResourceText(QStringLiteral(":/qmud/text/LICENSE.md"));
		        showTextDialog(m_mainWindow, QStringLiteral("License Agreement"),
		                       text.isEmpty() ? QStringLiteral("Could not load text.") : text);
	        });

	dialog.exec();
}

void AppController::handleFileNew()
{
	qDebug() << "File->New requested";
	if (!m_mainWindow)
		return;

	m_typeOfNewDocument = eNormalNewDocument;
	const QPointer<WorldRuntime> previouslyActiveRuntime =
	    m_mainWindow->activeWorldChildWindow() ? m_mainWindow->activeWorldChildWindow()->runtime() : nullptr;

	auto *runtime = new WorldRuntime(m_mainWindow);
	initializeWorldRuntime(runtime);

	auto *window = new WorldChildWindow(QStringLiteral("World"));
	window->setRuntime(runtime);
	m_mainWindow->addMdiSubWindow(window, true);

	WorldPreferencesDialog dlg(runtime, window->view(), m_mainWindow);
	dlg.setInitialPage(WorldPreferencesDialog::PageGeneral);
	if (dlg.exec() != QDialog::Accepted)
	{
		window->close();
		runtime->deleteLater();
		if (previouslyActiveRuntime)
		{
			const QPointer<MainWindow> mainWindowGuard = m_mainWindow;
			QMetaObject::invokeMethod(
			    qApp,
			    [mainWindowGuard, previouslyActiveRuntime]
			    {
				    if (!mainWindowGuard || !previouslyActiveRuntime)
					    return;
				    mainWindowGuard->activateWorldRuntime(previouslyActiveRuntime);
			    },
			    Qt::QueuedConnection);
		}
		return;
	}

	if (const QString worldName = runtime->worldAttributes().value(QStringLiteral("name")).trimmed();
	    !worldName.isEmpty())
	{
		window->setWindowTitle(worldName);
		if (WorldView *view = window->view())
			view->setWorldName(worldName);
		m_mainWindow->updateMdiTabs();
		m_mainWindow->refreshTitleBar();
	}

	runtime->setPluginInstallDeferred(true);
	if (!m_suppressAutoConnect)
	{
		const QPointer<WorldRuntime> runtimeGuard(runtime);
		restoreWorldSessionStateAsync(
		    runtime, window->view(),
		    [this, runtimeGuard](const bool ok, const QString &error)
		    {
			    if (!runtimeGuard)
				    return;
			    QMudWorldSessionRestoreFlow::runPostRestoreFlow(
			        ok, error,
			        {
			            [this, runtimeGuard] { runWorldStartupPostRestore(runtimeGuard); },
			            [this, runtimeGuard] { maybeAutoConnectWorld(runtimeGuard); },
			            [runtimeGuard](const QString &restoreError)
			            {
				            qWarning()
				                << "Failed to restore world session state for"
				                << runtimeGuard->worldAttributes().value(QStringLiteral("name")).trimmed()
				                << ":" << restoreError;
			            },
			        });
		    });
	}
	else
	{
		runWorldStartupPostRestore(runtime);
	}
}

void AppController::handleHelpGettingStarted() const
{
	if (!showHelpDoc(m_mainWindow, QStringLiteral("starting")))
	{
		QMessageBox::information(m_mainWindow, QStringLiteral("Getting Started"),
		                         QStringLiteral("Getting Started help is not available."));
	}
}

void AppController::handleHelpContents() const
{
	if (!showHelpDoc(m_mainWindow, QStringLiteral("contents")))
	{
		QMessageBox::information(m_mainWindow, QStringLiteral("Help Contents"),
		                         QStringLiteral("Help contents are not available."));
	}
}

void AppController::handleEditColourPicker() const
{
	if (!m_mainWindow)
		return;
	ColourPickerDialog dlg(m_mainWindow);
	dlg.setPickColour(false);
	dlg.exec();
}

void AppController::handleCopy() const
{
	if (!m_mainWindow)
		return;
	const WorldChildWindow *child = m_mainWindow->activeWorldChildWindow();
	if (!child)
		return;
	if (WorldView *view = child->view(); view)
		view->copySelection();
}

void AppController::handleCopyAsHtml() const
{
	if (!m_mainWindow)
		return;
	const WorldChildWindow *child = m_mainWindow->activeWorldChildWindow();
	if (!child)
		return;
	if (WorldView *view = child->view(); view)
		view->copySelectionAsHtml();
}

void AppController::handleQuickConnect()
{
	if (!m_mainWindow)
		return;

	WorldChildWindow            *existingChild   = m_mainWindow->activeWorldChildWindow();
	WorldRuntime                *existingRuntime = existingChild ? existingChild->runtime() : nullptr;
	const QMap<QString, QString> attrs =
	    existingRuntime ? existingRuntime->worldAttributes() : QMap<QString, QString>();

	QString defaultName = m_lastQuickConnectWorldName;
	QString defaultHost = m_lastQuickConnectHost;
	quint16 defaultPort = m_lastQuickConnectPort == 0 ? 4000 : m_lastQuickConnectPort;

	if (defaultName.trimmed().isEmpty())
		defaultName = attrs.value(QStringLiteral("name")).trimmed();
	if (defaultHost.trimmed().isEmpty())
		defaultHost = attrs.value(QStringLiteral("site")).trimmed();
	if (defaultPort == 0)
		defaultPort = attrs.value(QStringLiteral("port")).toUShort();
	if (defaultPort == 0)
		defaultPort = 4000;
	if (defaultName.isEmpty())
		defaultName = QStringLiteral("Untitled world");

	QDialog dlg(m_mainWindow);
	dlg.setWindowTitle(QStringLiteral("Quick Connect"));
	dlg.setMinimumSize(460, 220);
	QGridLayout      layout(&dlg);
	QLabel           nameLabel(QStringLiteral("World name:"), &dlg);
	QLabel           hostLabel(QStringLiteral("Address:"), &dlg);
	QLabel           portLabel(QStringLiteral("Port:"), &dlg);
	QLineEdit        nameEdit(defaultName, &dlg);
	QLineEdit        hostEdit(defaultHost, &dlg);
	QSpinBox         portEdit(&dlg);
	QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
	portEdit.setRange(1, 65535);
	portEdit.setValue(defaultPort);
	layout.addWidget(&nameLabel, 0, 0);
	layout.addWidget(&nameEdit, 0, 1);
	layout.addWidget(&hostLabel, 1, 0);
	layout.addWidget(&hostEdit, 1, 1);
	layout.addWidget(&portLabel, 2, 0);
	layout.addWidget(&portEdit, 2, 1);
	layout.addWidget(&buttons, 3, 0, 1, 2);
	connect(&buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(&buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	if (dlg.exec() != QDialog::Accepted)
		return;

	const QString worldName = nameEdit.text().trimmed();
	const QString host      = hostEdit.text().trimmed();
	const auto    port      = static_cast<quint16>(portEdit.value());

	if (worldName.isEmpty())
	{
		QMessageBox::warning(m_mainWindow, QStringLiteral("Quick Connect"),
		                     QStringLiteral("World name cannot be empty."));
		return;
	}
	if (host.isEmpty())
	{
		QMessageBox::warning(m_mainWindow, QStringLiteral("Quick Connect"),
		                     QStringLiteral("Address cannot be empty."));
		return;
	}

	m_lastQuickConnectWorldName = worldName;
	m_lastQuickConnectHost      = host;
	m_lastQuickConnectPort      = port;

	WorldChildWindow *child   = existingChild;
	WorldRuntime     *runtime = existingRuntime;
	if (!runtime)
	{
		runtime = new WorldRuntime(m_mainWindow);
		initializeWorldRuntime(runtime);
		child = new WorldChildWindow(worldName);
		child->setRuntime(runtime);
		m_mainWindow->addMdiSubWindow(child);
		runtime->setPluginInstallDeferred(true);
		applyConfiguredWorldDefaults(runtime);
		emitStartupBanner(runtime);
		loadGlobalPlugins(runtime);
		runtime->setPluginInstallDeferred(false);
		runtime->installPendingPlugins();
	}

	runtime->setWorldAttribute(QStringLiteral("name"), worldName);
	runtime->setWorldAttribute(QStringLiteral("site"), host);
	runtime->setWorldAttribute(QStringLiteral("port"), QString::number(port));
	runtime->setWorldFileModified(true);

	child->setWindowTitle(worldName);
	if (WorldView *view = child->view())
		view->setWorldName(worldName);

	if (runtime->isConnected() || runtime->isConnecting())
	{
		QMessageBox::information(m_mainWindow, QStringLiteral("Quick Connect"),
		                         QStringLiteral("Disconnect the current world before quick connecting."));
		return;
	}
	runtime->connectToWorld(host, port);
}

void AppController::handleConnectOrReconnect() const
{
	if (!m_mainWindow)
		return;

	const WorldChildWindow *child = m_mainWindow->activeWorldChildWindow();
	if (!child)
	{
		qDebug() << "Connect: no active world window";
		return;
	}

	WorldRuntime *runtime = child->runtime();
	if (!runtime)
		return;

	const QMap<QString, QString> &attrs = runtime->worldAttributes();
	const QString                 host  = attrs.value(QStringLiteral("site"));
	const quint16                 port  = attrs.value(QStringLiteral("port")).toUShort();

	if (host.isEmpty() || host == QStringLiteral("0.0.0.0") || port == 0)
	{
		QMessageBox::warning(m_mainWindow, QStringLiteral("QMud"),
		                     QStringLiteral("Cannot connect: host/port not specified."));
		return;
	}

	if (runtime->isConnected() || runtime->isConnecting())
	{
		runtime->setDisconnectOk(true);
		runtime->disconnectFromWorld();
		return;
	}

	runtime->connectToWorld(host, port);
}

void AppController::handleConnect() const
{
	if (!m_mainWindow)
		return;

	const WorldChildWindow *child = m_mainWindow->activeWorldChildWindow();
	if (!child)
	{
		qDebug() << "Connect: no active world window";
		return;
	}

	WorldRuntime *runtime = child->runtime();
	if (!runtime)
		return;

	if (runtime->isConnected() || runtime->isConnecting())
		return;

	const QMap<QString, QString> &attrs = runtime->worldAttributes();
	const QString                 host  = attrs.value(QStringLiteral("site"));
	const quint16                 port  = attrs.value(QStringLiteral("port")).toUShort();

	if (host.isEmpty() || host == QStringLiteral("0.0.0.0") || port == 0)
	{
		QMessageBox::warning(m_mainWindow, QStringLiteral("QMud"),
		                     QStringLiteral("Cannot connect: host/port not specified."));
		return;
	}

	runtime->connectToWorld(host, port);
}

void AppController::handleDisconnect() const
{
	if (!m_mainWindow)
		return;

	const WorldChildWindow *child = m_mainWindow->activeWorldChildWindow();
	if (!child)
		return;

	WorldRuntime *runtime = child->runtime();
	if (!runtime)
		return;

	runtime->setDisconnectOk(true);
	runtime->disconnectFromWorld();
}

void AppController::handleGameMinimize() const
{
	if (m_mainWindow)
		m_mainWindow->showMinimized();
}

void AppController::handleLogSession() const
{
	if (!m_mainWindow)
		return;

	WorldChildWindow *child = m_mainWindow->activeWorldChildWindow();
	if (!child)
	{
		qDebug() << "LogSession: no active world window";
		return;
	}

	QPointer<WorldRuntime> runtime = child->runtime();
	if (!runtime)
		return;

	if (runtime->isLogOpen())
	{
		if (const int confirmClose = getGlobalOption(QStringLiteral("ConfirmLogFileClose")).toInt();
		    confirmClose)
		{
			if (const QString prompt = QStringLiteral("Close log file %1?").arg(runtime->logFileName());
			    QMessageBox::question(m_mainWindow, QStringLiteral("QMud"), prompt,
			                          QMessageBox::Ok | QMessageBox::Cancel,
			                          QMessageBox::Cancel) != QMessageBox::Ok)
				return;
		}
		runtime->closeLog();
		return;
	}

	const QMap<QString, QString> attrs = runtime->worldAttributes();
	const QMap<QString, QString> multi = runtime->worldMultilineAttributes();

	bool      appendToLog    = getGlobalOption(QStringLiteral("AppendToLogFiles")).toInt() != 0;
	const int linesThen      = static_cast<int>(runtime->lines().size());
	int       lines          = linesThen;
	bool      writeWorldName = isEnabledFlag(attrs.value(QStringLiteral("write_world_name_to_log")));
	QString   preamble       = multi.value(QStringLiteral("log_file_preamble"));
	if (preamble.isEmpty())
		preamble = attrs.value(QStringLiteral("log_file_preamble"));
	bool       logOutput = isEnabledFlag(attrs.value(QStringLiteral("log_output")));
	bool       logInput  = isEnabledFlag(attrs.value(QStringLiteral("log_input")));
	bool       logNotes  = isEnabledFlag(attrs.value(QStringLiteral("log_notes")));

	const bool logRaw = isEnabledFlag(attrs.value(QStringLiteral("log_raw")));
	if (!logRaw)
	{
		LogSessionDialog dlg(m_mainWindow);
		dlg.setLines(lines);
		dlg.setAppendToLogFile(appendToLog);
		dlg.setWriteWorldName(writeWorldName);
		dlg.setPreamble(preamble);
		dlg.setLogOutput(logOutput);
		dlg.setLogInput(logInput);
		dlg.setLogNotes(logNotes);
		if (dlg.exec() != QDialog::Accepted)
			return;
		if (!runtime)
			return;

		lines          = dlg.lines();
		appendToLog    = dlg.appendToLogFile();
		writeWorldName = dlg.writeWorldName();
		preamble       = dlg.preamble();
		logOutput      = dlg.logOutput();
		logInput       = dlg.logInput();
		logNotes       = dlg.logNotes();

		if (const int linesNow = static_cast<int>(runtime->lines().size()); lines > 0 && linesNow > linesThen)
			lines += linesNow - linesThen;
	}

	runtime->setWorldAttribute(QStringLiteral("log_output"),
	                           logOutput ? QStringLiteral("1") : QStringLiteral("0"));
	runtime->setWorldAttribute(QStringLiteral("log_input"),
	                           logInput ? QStringLiteral("1") : QStringLiteral("0"));
	runtime->setWorldAttribute(QStringLiteral("log_notes"),
	                           logNotes ? QStringLiteral("1") : QStringLiteral("0"));
	if (!runtime)
		return;

	const QMap<QString, QString> attrsAfterOptions = runtime->worldAttributes();

	const QString autoLogFileName = attrsAfterOptions.value(QStringLiteral("auto_log_file_name"));
	QString       suggestedName;
	if (!autoLogFileName.isEmpty())
	{
		suggestedName = runtime->formatTime(QDateTime::currentDateTime(), autoLogFileName, false);
	}
	else
	{
		QString           worldName = attrsAfterOptions.value(QStringLiteral("name"));
		static const auto invalid   = QStringLiteral("<>\"|?:#%;/\\");
		for (const QChar &ch : invalid)
			worldName.remove(ch);
		suggestedName =
		    makeAbsolutePath(getGlobalOption(QStringLiteral("DefaultLogFileDirectory")).toString());
		if (!suggestedName.isEmpty() && !suggestedName.endsWith(QChar('/')) &&
		    !suggestedName.endsWith(QChar('\\')))
			suggestedName += QLatin1Char('/');
		suggestedName += worldName;
		suggestedName += QStringLiteral(" log");
	}

	changeToFileBrowsingDirectory();
	QFileDialog dialog(m_mainWindow);
	dialog.setAcceptMode(QFileDialog::AcceptSave);
	dialog.setWindowTitle(QStringLiteral("Log file name"));
	dialog.setNameFilter(QStringLiteral("Text files (*.txt);;All files (*.*)"));
	dialog.setDefaultSuffix(QStringLiteral("txt"));
	dialog.selectFile(suggestedName);
	if (appendToLog)
		dialog.setOption(QFileDialog::DontConfirmOverwrite, true);

	const QString fileName = dialog.exec() == QDialog::Accepted ? dialog.selectedFiles().value(0) : QString();
	changeToStartupDirectory();
	if (!runtime)
		return;
	if (fileName.isEmpty())
		return;

	if (runtime->openLog(fileName, appendToLog) == eCouldNotOpenFile)
	{
		QMessageBox::warning(m_mainWindow, QStringLiteral("QMud"),
		                     QStringLiteral("Unable to open log file \"%1\"").arg(fileName));
		return;
	}

	if (logRaw)
		return;

	const bool logHtml     = isEnabledFlag(attrsAfterOptions.value(QStringLiteral("log_html")));
	const bool logInColour = isEnabledFlag(attrsAfterOptions.value(QStringLiteral("log_in_colour")));
	const auto now         = QDateTime::currentDateTime();
	if (!preamble.isEmpty())
	{
		preamble.replace(QStringLiteral("%n"), QStringLiteral("\n"));
		preamble = runtime->formatTime(now, preamble, logHtml);
		preamble.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
		runtime->writeLog(preamble);
		runtime->writeLog(QStringLiteral("\n"));
	}

	if (writeWorldName)
	{
		const QString strTime = runtime->formatTime(now, QStringLiteral("%A, %B %d, %Y, %#I:%M %p"), false);
		QString       strPreamble = attrsAfterOptions.value(QStringLiteral("name"));
		strPreamble += QStringLiteral(" - ");
		strPreamble += strTime;

		if (logHtml)
		{
			if (appendToLog)
				runtime->writeLog(QStringLiteral("<br>\n"));
			runtime->writeLog(strPreamble.toHtmlEscaped());
			runtime->writeLog(QStringLiteral("<br>\n"));
		}
		else
		{
			if (appendToLog)
				runtime->writeLog(QStringLiteral("\n"));
			runtime->writeLog(strPreamble);
			runtime->writeLog(QStringLiteral("\n"));
		}

		QString strHyphens(strPreamble.length(), QLatin1Char('-'));
		runtime->writeLog(strHyphens);
		if (logHtml)
			runtime->writeLog(QStringLiteral("<br><br>"));
		else
			runtime->writeLog(QStringLiteral("\n\n"));
	}

	if (lines > 0)
	{
		const QVector<WorldRuntime::LineEntry> &buffer = runtime->lines();
		const int start = buffer.size() > lines ? static_cast<int>(buffer.size()) - lines : 0;
		for (int i = start; i < buffer.size(); ++i)
		{
			const WorldRuntime::LineEntry &entry = buffer.at(i);
			if (entry.flags & WorldRuntime::LineInput && !logInput)
				continue;
			if (entry.flags & WorldRuntime::LineNote && !logNotes)
				continue;
			if (entry.flags & WorldRuntime::LineOutput && !logOutput)
				continue;

			QString preambleKey;
			QString postambleKey;
			if (entry.flags & WorldRuntime::LineInput)
			{
				preambleKey  = QStringLiteral("log_line_preamble_input");
				postambleKey = QStringLiteral("log_line_postamble_input");
			}
			else if (entry.flags & WorldRuntime::LineNote)
			{
				preambleKey  = QStringLiteral("log_line_preamble_notes");
				postambleKey = QStringLiteral("log_line_postamble_notes");
			}
			else
			{
				preambleKey  = QStringLiteral("log_line_preamble_output");
				postambleKey = QStringLiteral("log_line_postamble_output");
			}

			QString linePreamble = multi.value(preambleKey);
			if (linePreamble.isEmpty())
				linePreamble = attrs.value(preambleKey);
			QString linePostamble = multi.value(postambleKey);
			if (linePostamble.isEmpty())
				linePostamble = attrs.value(postambleKey);

			if (!linePreamble.isEmpty())
			{
				linePreamble.replace(QStringLiteral("%n"), QStringLiteral("\n"));
				if (linePreamble.contains(QLatin1Char('%')))
					linePreamble = runtime->formatTime(entry.time, linePreamble, logHtml);
				runtime->writeLog(linePreamble);
			}

			bool wroteColourLine = false;
			if (logHtml && logInColour && !entry.spans.isEmpty())
			{
				int       iCol    = 0;
				const int textLen = static_cast<int>(entry.text.size());
				for (const auto &span : entry.spans)
				{
					if (span.length <= 0 || iCol >= textLen)
						continue;
					const int     spanLen  = qMin(span.length, textLen - iCol);
					const QString fragment = entry.text.mid(iCol, spanLen);
					if (span.fore.isValid())
					{
						runtime->writeLog(QStringLiteral("<font color=\"#%1%2%3\">")
						                      .arg(span.fore.red(), 2, 16, QLatin1Char('0'))
						                      .arg(span.fore.green(), 2, 16, QLatin1Char('0'))
						                      .arg(span.fore.blue(), 2, 16, QLatin1Char('0')));
					}
					if (span.underline)
						runtime->writeLog(QStringLiteral("<u>"));
					runtime->writeLog(fragment.toHtmlEscaped());
					if (span.underline)
						runtime->writeLog(QStringLiteral("</u>"));
					if (span.fore.isValid())
						runtime->writeLog(QStringLiteral("</font>"));
					iCol += spanLen;
				}
				runtime->writeLog(QStringLiteral("\n"));
				wroteColourLine = true;
			}
			else if (logHtml)
			{
				runtime->writeLog(entry.text.toHtmlEscaped());
			}
			else
			{
				runtime->writeLog(entry.text);
			}

			if (!linePostamble.isEmpty())
			{
				linePostamble.replace(QStringLiteral("%n"), QStringLiteral("\n"));
				if (linePostamble.contains(QLatin1Char('%')))
					linePostamble = runtime->formatTime(entry.time, linePostamble, logHtml);
				runtime->writeLog(linePostamble);
			}

			if (!wroteColourLine)
				runtime->writeLog(QStringLiteral("\n"));
		}
	}
}

void AppController::handleReloadQmud()
{
#if !defined(Q_OS_LINUX) && !defined(Q_OS_MACOS)
	QMessageBox::information(m_mainWindow, QStringLiteral("Reload QMud"),
	                         QStringLiteral("Reload QMud is available only on Linux and macOS."));
#else
	if (getGlobalOption(QStringLiteral("EnableReloadFeature")).toInt() == 0)
	{
		QMessageBox::information(m_mainWindow, QStringLiteral("Reload QMud"),
		                         QStringLiteral("Reload QMud is disabled in global update preferences."));
		return;
	}

	const bool autoConfirmReload = envFlagEnabled("QMUD_RELOAD_ASSUME_YES");
	const bool verboseReloadLogs = envFlagEnabled("QMUD_RELOAD_VERBOSE");
	if (m_reloadInProgress)
	{
		QMessageBox::information(m_mainWindow, QStringLiteral("Reload QMud"),
		                         QStringLiteral("A reload operation is already in progress."));
		return;
	}
	if (QWidget *activeModal = QApplication::activeModalWidget(); activeModal && activeModal != m_mainWindow)
	{
		if (autoConfirmReload)
		{
			if (auto *dialog = qobject_cast<QDialog *>(activeModal))
				dialog->close();
			else
				activeModal->close();
			QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
		}
		if (QWidget *stillModal = QApplication::activeModalWidget(); stillModal && stillModal != m_mainWindow)
		{
			QMessageBox::information(m_mainWindow, QStringLiteral("Reload QMud"),
			                         QStringLiteral("Close the active dialog before reloading QMud."));
			return;
		}
	}

	const int configuredTimeout    = getGlobalOption(QStringLiteral("ReloadMccpDisableTimeoutMs")).toInt();
	const int mccpDisableTimeoutMs = qBound(300, configuredTimeout, 2000);
	if (!autoConfirmReload)
	{
		if (QMessageBox::question(
		        m_mainWindow, QStringLiteral("Reload QMud"),
		        QStringLiteral("QMud will restart in place and restore open worlds.\n\nContinue?"),
		        QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Ok)
			return;
	}
	else
	{
		if (verboseReloadLogs)
			qInfo() << kReloadLogTag << "Auto-confirm enabled by QMUD_RELOAD_ASSUME_YES.";
	}

	// Persist current frame visibility/layout before reload handoff.
	saveViewPreferences();
	saveWindowPlacement();
	saveSessionState();

	QString preReloadSaveError;
	if (!saveDirtyAutoSaveWorldsBeforeRestart(&preReloadSaveError))
	{
		QMessageBox::warning(
		    m_mainWindow, QStringLiteral("Reload QMud"),
		    QStringLiteral("Failed to save dirty worlds before reload.\n%1").arg(preReloadSaveError));
		return;
	}
	QString preReloadLogCloseError;
	QString preReloadPluginStateError;
	if (!closeOpenWorldLogsBeforeRestart(&preReloadLogCloseError))
	{
		QMessageBox::warning(m_mainWindow, QStringLiteral("Reload QMud"),
		                     QStringLiteral("Failed to close active world logs before reload.\n%1")
		                         .arg(preReloadLogCloseError));
		return;
	}
	if (!saveOpenWorldPluginStatesBeforeRestart(&preReloadPluginStateError))
	{
		QMessageBox::warning(
		    m_mainWindow, QStringLiteral("Reload QMud"),
		    QStringLiteral("Failed to save plugin state before reload.\n%1").arg(preReloadPluginStateError));
		return;
	}
	QString preReloadSessionStateError;
	if (!saveOpenWorldSessionStatesBeforeRestart(&preReloadSessionStateError))
	{
		QMessageBox::warning(m_mainWindow, QStringLiteral("Reload QMud"),
		                     QStringLiteral("Failed to persist world session state before reload.\n%1")
		                         .arg(preReloadSessionStateError));
		return;
	}

	++m_reloadAttempts;
	m_reloadInProgress = true;
	if (m_mainWindow)
		m_mainWindow->showStatusMessage(QStringLiteral("Preparing Reload QMud..."), 0);
	qInfo() << kReloadLogTag << "Preparing reload handoff. attempt=" << m_reloadAttempts
	        << "exec_failures=" << m_reloadExecFailures << "recoveries=" << m_reloadRecoveryRuns
	        << "mccp_disable_timeout_ms=" << mccpDisableTimeoutMs;

	QVector<int> inheritableDescriptors;
	bool         snapshotWritten    = false;
	auto         restoreDescriptors = [&inheritableDescriptors]()
	{
		for (const int descriptor : std::as_const(inheritableDescriptors))
		{
			QString ignoredError;
			(void)setSocketDescriptorInheritable(descriptor, false, &ignoredError);
		}
	};
	auto failReload =
	    [this, &snapshotWritten, &restoreDescriptors](const QString &statePath, const QString &message)
	{
		++m_reloadExecFailures;
		restoreDescriptors();
		if (snapshotWritten)
		{
			QString cleanupError;
			if (!removeReloadStateFile(statePath, &cleanupError) && !cleanupError.isEmpty())
				qWarning() << "Reload cleanup failed after setup error:" << cleanupError;
		}
		if (m_mainWindow)
		{
			const QString summary = message.section(QLatin1Char('\n'), 0, 0).trimmed();
			m_mainWindow->showStatusMessage(summary.isEmpty()
			                                    ? QStringLiteral("Reload failed.")
			                                    : QStringLiteral("Reload failed: %1").arg(summary),
			                                7000);
		}
		qWarning() << kReloadLogTag << "Reload failure detail:" << message;
		qWarning() << kReloadLogTag << "Reload setup failed. attempt=" << m_reloadAttempts
		           << "exec_failures=" << m_reloadExecFailures;
		m_reloadInProgress = false;
	};

	QString executablePath = m_reloadTargetExecutableOverride.trimmed();
	m_reloadTargetExecutableOverride.clear();
	if (executablePath.isEmpty())
	{
#ifdef Q_OS_LINUX
		if (const QString appImagePath = qEnvironmentVariable("APPIMAGE").trimmed();
		    !appImagePath.isEmpty() && QFileInfo(appImagePath).exists())
		{
			executablePath = appImagePath;
		}
		else
#endif
			executablePath = QCoreApplication::applicationFilePath();
	}
	if (executablePath.trimmed().isEmpty())
	{
		failReload(QString(), QStringLiteral("Unable to determine executable path for reload."));
		return;
	}

	QString statePath = reloadStateDefaultPath(m_workingDir);
	if (const QFileInfo info(statePath); info.isRelative())
		statePath = QDir(m_workingDir).filePath(statePath);
	const QString       reloadToken = generateReloadToken();

	ReloadStateSnapshot snapshot;
	snapshot.schemaVersion    = 1;
	snapshot.createdAtUtc     = QDateTime::currentDateTimeUtc();
	snapshot.reloadToken      = reloadToken;
	snapshot.targetExecutable = executablePath;

	QStringList arguments = QCoreApplication::arguments();
	if (arguments.isEmpty())
		arguments.push_back(executablePath);
	else
		arguments[0] = executablePath;
	const QString reloadStatePrefix = QString::fromLatin1(kReloadStateArgName) + QLatin1Char('=');
	const QString reloadTokenPrefix = QString::fromLatin1(kReloadTokenArgName) + QLatin1Char('=');
	for (const QString &arg : std::as_const(arguments))
	{
		if (arg.startsWith(reloadStatePrefix) || arg.startsWith(reloadTokenPrefix))
			continue;
		snapshot.arguments.push_back(arg);
	}
	snapshot.arguments.push_back(makeReloadArgument(QString::fromLatin1(kReloadStateArgName), statePath));
	snapshot.arguments.push_back(makeReloadArgument(QString::fromLatin1(kReloadTokenArgName), reloadToken));

	MainWindowHost                      *host = resolveMainWindowHost(m_mainWindow);
	const QVector<WorldWindowDescriptor> worlds =
	    host ? host->worldWindowDescriptors() : QVector<WorldWindowDescriptor>{};
	if (host)
	{
		if (const WorldChildWindow *activeWorld = host->activeWorldChildWindow(); activeWorld)
		{
			for (const WorldWindowDescriptor &entry : worlds)
			{
				if (entry.window == activeWorld)
				{
					snapshot.activeWorldSequence = entry.sequence;
					break;
				}
			}
		}
	}
	snapshot.worlds.reserve(worlds.size());
	struct ReloadPlanRuntimeContext
	{
			QPointer<WorldRuntime> runtime;
	};
	QVector<ReloadPlanRuntimeContext> runtimeContexts;
	runtimeContexts.reserve(worlds.size());
	struct PendingMccpDisable
	{
			int                    worldIndex{-1};
			QPointer<WorldRuntime> runtime;
			QString                displayName;
	};
	QVector<PendingMccpDisable> pendingMccpDisables;
	pendingMccpDisables.reserve(worlds.size());
	int connectedWorlds = 0;
	int reattachWorlds  = 0;
	int reconnectWorlds = 0;
	int mccpFallbacks   = 0;

	for (const WorldWindowDescriptor &entry : worlds)
	{
		WorldRuntime *runtime = entry.runtime;
		if (!runtime)
			continue;

		ReloadWorldState world;
		world.sequence = entry.sequence;
		if (entry.window)
			world.displayName = entry.window->windowTitle().trimmed();

		const QMap<QString, QString> &attrs = runtime->worldAttributes();
		world.worldId                       = attrs.value(QStringLiteral("id")).trimmed();
		if (world.displayName.isEmpty())
			world.displayName = attrs.value(QStringLiteral("name")).trimmed();
		world.worldFilePath = runtime->worldFilePath();
		world.host          = attrs.value(QStringLiteral("site")).trimmed();
		world.port          = attrs.value(QStringLiteral("port")).toUShort();
		world.utf8Enabled   = isEnabledFlag(attrs.value(QStringLiteral("utf_8")));

		const bool connected = runtime->isConnected();
		runtimeContexts.push_back({runtime});
		world.socketDescriptor = runtime->nativeSocketDescriptor();
		world.mccpWasActive    = runtime->isCompressing() || runtime->mccpType() != 0;
		if (shouldAttemptReloadMccpDisable(connected, world.socketDescriptor, world.mccpWasActive))
		{
			world.mccpDisableAttempted = true;
			runtime->queueMccpDisableForReload();
			pendingMccpDisables.push_back(
			    {static_cast<int>(snapshot.worlds.size()), runtime, world.displayName});
		}
		snapshot.worlds.push_back(world);
	}
	if (!pendingMccpDisables.isEmpty())
	{
		QElapsedTimer waitTimer;
		waitTimer.start();
		auto allMccpDisabled = [&pendingMccpDisables]() -> bool
		{
			return std::ranges::all_of(
			    pendingMccpDisables, [](const PendingMccpDisable &pending)
			    { return !pending.runtime || pending.runtime->isMccpDisableCompleteForReload(); });
		};
		while (!allMccpDisabled() && waitTimer.elapsed() < mccpDisableTimeoutMs)
			QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
	}
	for (const PendingMccpDisable &pending : std::as_const(pendingMccpDisables))
	{
		if (pending.worldIndex < 0 || pending.worldIndex >= snapshot.worlds.size())
			continue;
		ReloadWorldState &world = snapshot.worlds[pending.worldIndex];
		if (!pending.runtime)
		{
			world.mccpDisableSucceeded = false;
			continue;
		}
		world.mccpDisableSucceeded = pending.runtime->isMccpDisableCompleteForReload();
		if (!world.mccpDisableSucceeded)
		{
			++mccpFallbacks;
#ifndef NDEBUG
			qWarning() << kReloadLogTag << "MCCP disable incomplete for"
			           << (pending.displayName.isEmpty() ? QStringLiteral("<unnamed>") : pending.displayName)
			           << "isCompressing=" << pending.runtime->isCompressing()
			           << "mccpType=" << pending.runtime->mccpType();
#endif
		}
		if (verboseReloadLogs)
		{
			qInfo() << kReloadLogTag << "MCCP disable"
			        << (world.mccpDisableSucceeded ? "succeeded" : "timed out") << "for"
			        << (pending.displayName.isEmpty() ? QStringLiteral("<unnamed>") : pending.displayName);
		}
	}
	QVector<int> droppedWorldIndices;
	for (int i = 0; i < snapshot.worlds.size() && i < runtimeContexts.size(); ++i)
	{
		ReloadWorldState               &world = snapshot.worlds[i];
		const ReloadPlanRuntimeContext &ctx   = runtimeContexts[i];
		if (!ctx.runtime)
		{
			droppedWorldIndices.push_back(i);
			continue;
		}

		const bool connectedNow  = ctx.runtime->isConnected();
		const bool connectingNow = ctx.runtime->isConnecting();
		world.socketDescriptor   = ctx.runtime->nativeSocketDescriptor();
		if (!world.mccpDisableAttempted)
		{
			const bool mccpActiveNow   = ctx.runtime->isCompressing() || ctx.runtime->mccpType() != 0;
			world.mccpDisableSucceeded = !mccpActiveNow;
		}
		const ReloadWorldPolicyDecision policyDecision =
		    computeReloadWorldPolicy({connectedNow, connectingNow, world.socketDescriptor,
		                              world.mccpWasActive, world.mccpDisableSucceeded});
		world.connectedAtReload = policyDecision.connectedAtReload;
		world.policy            = policyDecision.policy;
		if (world.connectedAtReload && world.policy == ReloadSocketPolicy::ParkReconnect &&
		    world.socketDescriptor >= 0 && world.mccpWasActive && !world.mccpDisableSucceeded &&
		    world.notes.trimmed().isEmpty())
		{
			world.notes =
			    QStringLiteral("MCCP disable did not complete before reload timeout; reconnect required.");
		}

		if (!world.connectedAtReload)
			continue;
		++connectedWorlds;
		if (policyDecision.shouldAttemptDescriptorInheritance)
		{
			QString inheritError;
			if (!setSocketDescriptorInheritable(world.socketDescriptor, true, &inheritError))
			{
				world.notes =
				    QStringLiteral("Failed to preserve socket across reload: %1").arg(inheritError.trimmed());
				printReloadInfoToStdout(
				    QStringLiteral("Descriptor inheritance failed for %1; reconnect fallback forced. "
				                   "Descriptor=%2. Reason: %3")
				        .arg(reloadWorldIdentity(world))
				        .arg(world.socketDescriptor)
				        .arg(inheritError.trimmed().isEmpty() ? QStringLiteral("Unknown error.")
				                                              : inheritError.trimmed()));
				world.socketDescriptor = -1;
				world.policy           = ReloadSocketPolicy::ParkReconnect;
			}
			else if (!inheritableDescriptors.contains(world.socketDescriptor))
			{
				inheritableDescriptors.push_back(world.socketDescriptor);
			}
		}
		else
		{
			world.notes = QStringLiteral("No socket descriptor available; reconnect fallback required.");
			printReloadInfoToStdout(
			    QStringLiteral("No inheritable descriptor is available for %1; reconnect fallback required.")
			        .arg(reloadWorldIdentity(world)));
		}
		if (world.policy == ReloadSocketPolicy::Reattach)
			++reattachWorlds;
		else
			++reconnectWorlds;
	}
	for (qsizetype idx = droppedWorldIndices.size(); idx > 0; --idx)
		snapshot.worlds.removeAt(droppedWorldIndices.at(idx - 1));
	qInfo() << kReloadLogTag << "Plan summary:"
	        << "worlds=" << snapshot.worlds.size() << "connected=" << connectedWorlds
	        << "reattach=" << reattachWorlds << "reconnect=" << reconnectWorlds
	        << "fallbacks=" << mccpFallbacks;
	if (mccpFallbacks > 0)
	{
		if (verboseReloadLogs)
		{
			qInfo() << kReloadLogTag << mccpFallbacks
			        << "world(s) downgraded to reconnect due to MCCP fallback.";
		}
		if (m_mainWindow)
		{
			m_mainWindow->showStatusMessage(
			    QStringLiteral("Reload: %1 world(s) will reconnect due to MCCP fallback.").arg(mccpFallbacks),
			    5000);
		}
	}

	QString writeError;
	if (!writeReloadStateSnapshot(statePath, snapshot, &writeError))
	{
		failReload(statePath, QStringLiteral("Failed to write reload state file:\n%1").arg(writeError));
		return;
	}
	snapshotWritten = true;

	if (const bool dryRunReload = envFlagEnabled("QMUD_RELOAD_DRY_RUN"); dryRunReload)
	{
		qInfo() << kReloadLogTag << "Dry-run enabled by QMUD_RELOAD_DRY_RUN; skipping exec.";
		restoreDescriptors();
		QString cleanupError;
		if (!removeReloadStateFile(statePath, &cleanupError) && !cleanupError.isEmpty())
			qWarning() << kReloadLogTag << "Dry-run cleanup failed:" << cleanupError;
		m_reloadInProgress = false;
		if (m_mainWindow)
			m_mainWindow->showStatusMessage(QStringLiteral("Reload dry-run completed."), 4000);
		return;
	}

	QByteArray executablePathBytes = QFile::encodeName(executablePath);
	if (executablePathBytes.isEmpty())
	{
		failReload(statePath, QStringLiteral("Failed to encode executable path for reload."));
		return;
	}

	std::vector<QByteArray> argvStorage;
	argvStorage.reserve(snapshot.arguments.size());
	for (const QString &arg : std::as_const(snapshot.arguments))
		argvStorage.push_back(QFile::encodeName(arg));
	if (argvStorage.empty())
		argvStorage.push_back(executablePathBytes);

	std::vector<char *> argv;
	argv.reserve(argvStorage.size() + 1);
	for (QByteArray &arg : argvStorage)
		argv.push_back(arg.data());
	argv.push_back(nullptr);

	execv(executablePathBytes.constData(), argv.data());

	const int savedErrno = errno;
	qWarning() << kReloadLogTag << "execv failed with errno" << savedErrno;
	failReload(
	    statePath,
	    QStringLiteral("Failed to reload QMud:\n%1").arg(QString::fromLocal8Bit(strerror(savedErrno))));
#endif
}

AppController::ImportResult AppController::importXmlFromFile(const QString &path, const unsigned long mask)
{
	ImportResult result;
	if (!path.isEmpty())
		m_fileBrowsingDir = QFileInfo(path).absolutePath();
	if (!m_mainWindow)
	{
		result.errorMessage = QStringLiteral("No active window.");
		return result;
	}
	WorldChildWindow *world = m_mainWindow->activeWorldChildWindow();
	if (!world)
	{
		result.errorMessage = QStringLiteral("No active world to import into.");
		return result;
	}
	WorldRuntime *runtime = world->runtime();
	if (!runtime)
	{
		result.errorMessage = QStringLiteral("No active world runtime available.");
		return result;
	}

	WorldDocument doc;
	doc.setLoadMask(mask | WorldDocument::XML_NO_PLUGINS | WorldDocument::XML_IMPORT_MAIN_FILE_ONLY);
	if (!doc.loadFromFile(path))
	{
		result.errorMessage = doc.errorString();
		return result;
	}
	if (const QString absolutePluginsDir = makeAbsolutePath(m_pluginsDirectory);
	    !doc.expandIncludes(path, absolutePluginsDir, m_workingDir, QString()))
	{
		result.errorMessage = doc.errorString();
		return result;
	}

	const bool allowOverwrite      = mask & WorldDocument::XML_OVERWRITE;
	const bool allowPasteDuplicate = mask & WorldDocument::XML_PASTE_DUPLICATE;
	auto       mergeNamedList      = [&](auto &dest, const auto &src, const QString &kind) -> bool
	{
		return QMudImportMerge::mergeNamedList(dest, src, kind, allowOverwrite, allowPasteDuplicate,
		                                       &result.duplicates, &result.errorMessage);
	};

	if (mask & WorldDocument::XML_GENERAL)
	{
		for (auto it = doc.worldAttributes().begin(); it != doc.worldAttributes().end(); ++it)
			runtime->setWorldAttribute(it.key(), it.value());
		for (auto it = doc.worldMultilineAttributes().begin(); it != doc.worldMultilineAttributes().end();
		     ++it)
			runtime->setWorldMultilineAttribute(it.key(), it.value());
	}

	if (mask & WorldDocument::XML_TRIGGERS)
	{
		QList<WorldRuntime::Trigger> merged = runtime->triggers();
		QList<WorldRuntime::Trigger> incoming;
		incoming.reserve(doc.triggers().size());
		for (const auto &[attributes, children, included] : doc.triggers())
		{
			WorldRuntime::Trigger trigger;
			trigger.attributes = attributes;
			trigger.children   = children;
			trigger.included   = included;
			incoming.push_back(trigger);
		}
		if (!mergeNamedList(merged, incoming, QStringLiteral("trigger")))
			return result;
		runtime->setTriggers(merged);
	}

	if (mask & WorldDocument::XML_ALIASES)
	{
		QList<WorldRuntime::Alias> merged = runtime->aliases();
		QList<WorldRuntime::Alias> incoming;
		incoming.reserve(doc.aliases().size());
		for (const auto &[attributes, children, included] : doc.aliases())
		{
			WorldRuntime::Alias alias;
			alias.attributes = attributes;
			alias.children   = children;
			alias.included   = included;
			incoming.push_back(alias);
		}
		if (!mergeNamedList(merged, incoming, QStringLiteral("alias")))
			return result;
		runtime->setAliases(merged);
	}

	if (mask & WorldDocument::XML_TIMERS)
	{
		QList<WorldRuntime::Timer> merged = runtime->timers();
		QList<WorldRuntime::Timer> incoming;
		incoming.reserve(doc.timers().size());
		for (const auto &[attributes, children, included] : doc.timers())
		{
			WorldRuntime::Timer timer;
			timer.attributes = attributes;
			timer.children   = children;
			timer.included   = included;
			incoming.push_back(timer);
		}
		if (!mergeNamedList(merged, incoming, QStringLiteral("timer")))
			return result;
		runtime->setTimers(merged);
	}

	if (mask & WorldDocument::XML_MACROS)
	{
		QList<WorldRuntime::Macro> merged = runtime->macros();
		QList<WorldRuntime::Macro> incoming;
		incoming.reserve(doc.macros().size());
		for (const auto &[attributes, children] : doc.macros())
			incoming.push_back({attributes, children});
		if (!mergeNamedList(merged, incoming, QStringLiteral("macro")))
			return result;
		runtime->setMacros(merged);
	}

	if (mask & WorldDocument::XML_VARIABLES)
	{
		QList<WorldRuntime::Variable> merged = runtime->variables();
		QList<WorldRuntime::Variable> incoming;
		incoming.reserve(doc.variables().size());
		for (const auto &[attributes, content] : doc.variables())
		{
			incoming.push_back({attributes, content});
		}
		QMudImportMerge::mergeVariables(merged, incoming, allowOverwrite, allowPasteDuplicate,
		                                &result.duplicates);
		runtime->setVariables(merged);
	}

	if (mask & WorldDocument::XML_COLOURS)
	{
		QList<WorldRuntime::Colour> merged = runtime->colours();
		for (const auto &[group, attributes] : doc.colours())
			merged.push_back({group, attributes});
		runtime->setColours(merged);
	}

	if (mask & WorldDocument::XML_KEYPAD)
	{
		QList<WorldRuntime::Keypad> merged = runtime->keypadEntries();
		for (const auto &[attributes, content] : doc.keypadEntries())
			merged.push_back({attributes, content});
		runtime->setKeypadEntries(merged);
	}

	if (mask & WorldDocument::XML_PRINTING)
	{
		QList<WorldRuntime::PrintingStyle> merged = runtime->printingStyles();
		for (const auto &[group, attributes] : doc.printingStyles())
			merged.push_back({group, attributes});
		runtime->setPrintingStyles(merged);
	}

	if (WorldView *view = world->view())
		view->applyRuntimeSettings();
	m_mainWindow->updateStatusBar();
	m_mainWindow->refreshActionState();

	result.ok        = true;
	result.triggers  = static_cast<int>(doc.triggers().size());
	result.aliases   = static_cast<int>(doc.aliases().size());
	result.timers    = static_cast<int>(doc.timers().size());
	result.macros    = static_cast<int>(doc.macros().size());
	result.variables = static_cast<int>(doc.variables().size());
	result.colours   = static_cast<int>(doc.colours().size());
	result.keypad    = static_cast<int>(doc.keypadEntries().size());
	result.printing  = static_cast<int>(doc.printingStyles().size());
	return result;
}

AppController::ImportResult AppController::importXmlFromText(const QString &xml, const unsigned long mask)
{
	ImportResult result;
	if (xml.isEmpty())
	{
		result.errorMessage = QStringLiteral("Not in XML format");
		return result;
	}

	QTemporaryFile tmp;
	if (!tmp.open())
	{
		result.errorMessage = QStringLiteral("Unable to open temporary file for import.");
		return result;
	}
	tmp.write(xml.toUtf8());
	tmp.flush();

	return importXmlFromFile(tmp.fileName(), mask);
}

bool AppController::isSpellCheckerAvailable()
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	if (const int enableSpellCheck = getGlobalOption(QStringLiteral("EnableSpellCheck")).toInt();
	    !enableSpellCheck)
		return false;
	return ensureSpellCheckerLoaded();
#else
	return false;
#endif
}
