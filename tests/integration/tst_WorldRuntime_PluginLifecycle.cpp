/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldRuntime_PluginLifecycle.cpp
 * Role: Integration coverage for WorldRuntime plugin lifecycle callback ordering.
 */

#include "NativePluginRegistry.h"
#include "WorldDocument.h"
#include "WorldRuntime.h"
#include "WorldView.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFile>
// ReSharper disable once CppUnusedIncludeDirective
#include <QHostAddress>
#include <QScopeGuard>
#include <QScopedPointer>
#include <QTcpServer>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTcpSocket>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

namespace
{
	const QString kDeferredConnectPluginId = QStringLiteral("abcdeffedcbaabcdeffedcba");
	const QString kTeardownStatePluginId   = QStringLiteral("fedcbaabcdeffedcbaabcdef");

	/**
	 * @brief Writes text to a test fixture file.
	 * @param path Destination file path.
	 * @param text Text to write.
	 * @return `true` when the file was written completely.
	 */
	bool          writeTextFile(const QString &path, const QString &text)
	{
		QFile file(path);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
			return false;
		const QByteArray bytes = text.toUtf8();
		return file.write(bytes) == bytes.size();
	}

	/**
	 * @brief Reads a whole text fixture file.
	 * @param path Source file path.
	 * @param text Receives file text on success.
	 * @return `true` when the file was read.
	 */
	bool readTextFile(const QString &path, QString &text)
	{
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
			return false;
		text = QString::fromUtf8(file.readAll());
		return true;
	}

	/**
	 * @brief Reads a plugin variable from the runtime.
	 * @param runtime Runtime to inspect.
	 * @param name Plugin variable name.
	 * @return Variable value, or empty when missing.
	 */
	QString pluginVariable(const WorldRuntime &runtime, const QString &name)
	{
		QString value;
		if (!runtime.findPluginVariable(kDeferredConnectPluginId, name, value))
			return {};
		return value;
	}

	/**
	 * @brief Extracts a saved plugin-state variable from a state XML document.
	 * @param xml State XML text.
	 * @param name Variable name to find.
	 * @return Saved variable value, or empty when missing.
	 */
	QString savedStateVariable(const QString &xml, const QString &name)
	{
		QXmlStreamReader reader(xml);
		while (!reader.atEnd())
		{
			reader.readNext();
			if (!reader.isStartElement() || reader.name() != QLatin1String("variable"))
				continue;
			if (reader.attributes().value(QStringLiteral("name")) == name)
				return reader.readElementText();
		}
		return {};
	}
} // namespace

/**
 * @brief QTest fixture covering real WorldRuntime plugin lifecycle behavior.
 */
class tst_WorldRuntime_PluginLifecycle : public QObject
{
		Q_OBJECT

	private slots:
		static void teardownPluginCloseRunsBeforeSaveStateWithQueuedAsyncCallback()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString originalCurrentPath = QDir::currentPath();
			QVERIFY(QDir::setCurrent(tempDir.path()));
			const auto restoreCurrentPath =
			    qScopeGuard([originalCurrentPath]() { QDir::setCurrent(originalCurrentPath); });

			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("worlds/plugins"));
			const QString stateDir   = QDir(tempDir.path()).filePath(QStringLiteral("state"));
			QVERIFY(QDir().mkpath(pluginsDir));
			QVERIFY(QDir().mkpath(stateDir));

			const QString pluginPath = QDir(pluginsDir).filePath(QStringLiteral("teardown_state.xml"));
			QVERIFY(writeTextFile(pluginPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin
    name="TeardownState"
    author="QMud Test"
    id="fedcbaabcdeffedcbaabcdef"
    language="lua"
    enabled="y"
    save_state="y"
    sequence="100">
    <script><![CDATA[
function OnPluginClose()
  SetVariable("close_marker", "closed")
end

function OnPluginAsyncResult(request_id, api_name, status, payload)
  SetVariable("async_marker", "ran")
end

function OnPluginSaveState()
  SetVariable("save_marker", (GetVariable("close_marker") or "missing") .. ":saved")
end
]]></script>
  </plugin>
</muclient>
)xml")));

			const QString worldId   = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaa");
			const QString statePath = QDir(stateDir).filePath(
			    worldId + QStringLiteral("-") + kTeardownStatePluginId + QStringLiteral("-state.xml"));
			{
				WorldView view;
				view.resize(640, 480);
				view.show();
				QVERIFY(QTest::qWaitForWindowExposed(&view));

				WorldRuntime runtime;
				runtime.setStartupDirectory(tempDir.path());
				runtime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
				runtime.setStateFilesDirectory(stateDir);
				runtime.setWorldAttribute(QStringLiteral("id"), worldId);
				view.setRuntime(&runtime);
				QCoreApplication::processEvents();

				QString loadError;
				QVERIFY2(runtime.loadPluginFile(QStringLiteral("teardown_state.xml"), &loadError),
				         qPrintable(loadError));
				QCOMPARE(runtime.plugins().size(), 1);
				QCOMPARE(runtime.plugins().constFirst().attributes.value(QStringLiteral("id")),
				         kTeardownStatePluginId);
				QVERIFY(runtime.plugins().constFirst().saveState);
				QTRY_VERIFY_WITH_TIMEOUT(!runtime.plugins().constFirst().installPending, 5000);
				runtime.dispatchPluginAsyncResult(kTeardownStatePluginId, 1, QStringLiteral("teardown-test"),
				                                  true, 0, QStringLiteral("queued"));
			}

			QString savedText;
			QVERIFY(readTextFile(statePath, savedText));
			QCOMPARE(savedStateVariable(savedText, QStringLiteral("close_marker")), QStringLiteral("closed"));
			QCOMPARE(savedStateVariable(savedText, QStringLiteral("save_marker")),
			         QStringLiteral("closed:saved"));
			QCOMPARE(savedStateVariable(savedText, QStringLiteral("async_marker")), QString());
		}

		static void nativeShimPluginSourceSurvivesWorldSaveReload()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString worldsDir  = QDir(tempDir.path()).filePath(QStringLiteral("worlds"));
			const QString pluginsDir = QDir(worldsDir).filePath(QStringLiteral("plugins"));
			const QString stateDir   = QDir(tempDir.path()).filePath(QStringLiteral("state"));
			QVERIFY(QDir().mkpath(pluginsDir));
			QVERIFY(QDir().mkpath(stateDir));

			const QString worldPath = QDir(worldsDir).filePath(QStringLiteral("native_source.qdl"));
			QVERIFY(writeTextFile(worldPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<qmud>
  <world id="aaaaaaaaaaaaaaaaaaaaaaaa" name="Native Source"/>
  <include name="worlds/plugins/qmud:native/MushReader" plugin="y" enabled="y"/>
</qmud>
)xml")));

			WorldDocument doc;
			QVERIFY2(doc.loadFromFile(worldPath), qPrintable(doc.errorString()));
			QVERIFY2(doc.expandIncludes(worldPath, pluginsDir, tempDir.path(), stateDir),
			         qPrintable(doc.errorString()));
			QCOMPARE(doc.plugins().size(), 1);
			QCOMPARE(doc.plugins().constFirst().attributes.value(QStringLiteral("id")),
			         QMudNativePluginRegistry::mushReaderPluginId());
			QCOMPARE(doc.plugins().constFirst().attributes.value(QStringLiteral("source")),
			         QStringLiteral("qmud:native/MushReader"));

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
			runtime.setStateFilesDirectory(stateDir);
			runtime.applyFromDocument(doc);
			QCOMPARE(runtime.includes().size(), 1);
			QCOMPARE(runtime.includes().constFirst().attributes.value(QStringLiteral("name")),
			         QStringLiteral("qmud:native/MushReader"));
			runtime.setWorldFileModified(true);

			const QString savedPath = QDir(worldsDir).filePath(QStringLiteral("native_source_saved.qdl"));
			QString       saveError;
			QVERIFY2(runtime.saveWorldFile(savedPath, &saveError), qPrintable(saveError));
			QVERIFY(!runtime.worldFileModified());

			QString savedText;
			QVERIFY(readTextFile(savedPath, savedText));
			QVERIFY(savedText.contains(QStringLiteral("name=\"qmud:native/MushReader\"")));
			QVERIFY(!savedText.contains(QStringLiteral("worlds/plugins/qmud:native/MushReader")));

			WorldDocument reloaded;
			QVERIFY2(reloaded.loadFromFile(savedPath), qPrintable(reloaded.errorString()));
			QVERIFY2(reloaded.expandIncludes(savedPath, pluginsDir, tempDir.path(), stateDir),
			         qPrintable(reloaded.errorString()));
			QCOMPARE(reloaded.plugins().size(), 1);
			QCOMPARE(reloaded.plugins().constFirst().attributes.value(QStringLiteral("id")),
			         QMudNativePluginRegistry::mushReaderPluginId());
		}

		static void deferredWorldConnectHandlersRunOnceAfterPluginInstallCompletes()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("worlds/plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));

			const QString pluginPath =
			    QDir(pluginsDir).filePath(QStringLiteral("deferred_connect_counter.xml"));
			QVERIFY(writeTextFile(pluginPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin
    name="DeferredConnectCounter"
    author="QMud Test"
    id="abcdeffedcbaabcdeffedcba"
    language="lua"
    enabled="y"
    save_state="n"
    sequence="100">
    <script><![CDATA[
function OnPluginConnect()
  local current = tonumber(GetVariable("connect_count") or "0") or 0
  SetVariable("connect_count", tostring(current + 1))
end
]]></script>
  </plugin>
</muclient>
)xml")));

			QTcpServer server;
			if (!server.listen(QHostAddress::LocalHost, 0))
				QSKIP("Local TCP listen is unavailable in this environment.");

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
			runtime.setPluginInstallDeferred(true);

			WorldView view;
			view.resize(640, 480);
			view.setRuntime(&runtime);
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			QString loadError;
			QVERIFY2(runtime.loadPluginFile(QStringLiteral("deferred_connect_counter.xml"), &loadError),
			         qPrintable(loadError));

			QSignalSpy connectedSpy(&runtime, &WorldRuntime::connected);
			QVERIFY(connectedSpy.isValid());
			QSignalSpy serverAcceptedSpy(&server, &QTcpServer::newConnection);
			QVERIFY(serverAcceptedSpy.isValid());
			QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
			QVERIFY(connectedSpy.wait(5000));
			QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() || serverAcceptedSpy.count() > 0, 5000);
			QScopedPointer<QTcpSocket> acceptedSocket(server.nextPendingConnection());
			QVERIFY(!acceptedSocket.isNull());

			runtime.fireWorldConnectHandlers();
			runtime.setPluginInstallDeferred(false);

			QTRY_COMPARE_WITH_TIMEOUT(pluginVariable(runtime, QStringLiteral("connect_count")),
			                          QStringLiteral("1"), 5000);

			runtime.installPendingPlugins();
			runtime.installPendingPlugins();
			QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

			QCOMPARE(pluginVariable(runtime, QStringLiteral("connect_count")), QStringLiteral("1"));
		}
};

QTEST_MAIN(tst_WorldRuntime_PluginLifecycle)

#if __has_include("tst_WorldRuntime_PluginLifecycle.moc")
#include "tst_WorldRuntime_PluginLifecycle.moc"
#endif
