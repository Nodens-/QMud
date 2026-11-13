/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_GmcpIntegration.cpp
 * Role: Native transport independence and worker-lane Lua access to immutable GMCP publications.
 */
#include "AppController.h"
#include "GmcpProtocol.h"
#include "LuaCallbackEngine.h"
#include "LuaExecutor.h"
#include "MainFrame.h"
#include "WorldChildWindow.h"
#include "WorldCommandProcessor.h"
#include "WorldRuntime.h"
#include "WorldRuntimeTestAccess.h"
#include "WorldSocket.h"
#include "WorldView.h"
#include "dialogs/GmcpBrowserDialog.h"
#include "helpers/ReloadUtils.h"
#include <QAbstractItemModelTester>
#include <QAbstractListModel>
#include <QAbstractProxyModel>
#include <QAction>
#include <QCheckBox>
#include <QDeadlineTimer>
#include <QDialog>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
// ReSharper disable once CppUnusedIncludeDirective
#include <QPersistentModelIndex>
#include <QPlainTextEdit>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTableView>
#include <QTcpServer>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QThread>
#include <QTimer>
#include <QTreeView>
#include <QtTest/QTest>
#include <algorithm>
#include <zlib.h>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

/**
 * @brief Provides recovery entry points without replacing the production startup flow.
 */
class AppControllerTestAccess final
{
	public:
		/**
		 * @brief Reads runtimes opened by the production document paths.
		 * @param controller Application controller.
		 * @return Current runtime owners without creating presentations.
		 */
		static QVector<WorldRuntime *> worlds(const AppController &controller)
		{
			return controller.activeWorldRuntimes();
		}
		/**
		 * @brief Runs the production initializer for an ordinary or genuinely new runtime.
		 * @param controller Application controller.
		 * @param runtime Runtime to initialize.
		 * @param newWorld Whether to apply the new-world GMCP default.
		 */
		static void initialize(const AppController &controller, WorldRuntime &runtime, bool newWorld)
		{
			if (newWorld)
				controller.initializeNewWorldRuntime(&runtime);
			else
				controller.initializeWorldRuntime(&runtime);
		}
		/**
		 * @brief Opens a world without running startup or connection actions.
		 * @param controller Application controller.
		 * @param path World file, or empty for a genuinely new world.
		 * @return Whether the production document path succeeded.
		 */
		static bool openDocument(AppController &controller, const QString &path)
		{
			const bool previous              = controller.m_suppressAutoConnect;
			controller.m_suppressAutoConnect = true;
			const auto restore = qScopeGuard([&] { controller.m_suppressAutoConnect = previous; });
			return controller.openWorldDocument(path, AppController::InitialAutoConnectPolicy::Suppress);
		}
		/**
		 * @brief Uses the build directory as home with portable fixture plugin and state paths.
		 * @param controller Application controller.
		 * @param directory Fixture directory beneath the build directory.
		 */
		static void setHome(AppController &controller, const QString &directory)
		{
			controller.m_workingDir = QStringLiteral(QMUD_TEST_BUILD_DIR) + QLatin1Char('/');
			controller.m_pluginsDirectory =
			    QDir(QStringLiteral(QMUD_TEST_BUILD_DIR)).relativeFilePath(directory) + QLatin1Char('/');
			controller.setGlobalOptionString(QStringLiteral("StateFilesDirectory"),
			                                 controller.m_pluginsDirectory + QStringLiteral("state/"));
		}
		/**
		 * @brief Opens a world through reload recovery.
		 * @param controller Application controller.
		 * @param state Preserved world state.
		 * @param runtime Output recovered runtime.
		 * @return Whether opening succeeded.
		 */
		static bool openRecovery(AppController &controller, const ReloadWorldState &state,
		                         WorldRuntime **runtime)
		{
			const bool previous              = controller.m_suppressAutoConnect;
			controller.m_suppressAutoConnect = true;
			const auto restore = qScopeGuard([&] { controller.m_suppressAutoConnect = previous; });
			return controller.openWorldForReloadRecovery(state, false, runtime);
		}
		/**
		 * @brief Installs recovered plugins through the production startup flow.
		 * @param controller Application controller.
		 * @param runtime Recovered runtime.
		 * @param completion Startup completion.
		 */
		static void startup(const AppController &controller, WorldRuntime *runtime,
		                    std::function<void()> completion)
		{
			controller.runWorldStartupPostRestore(runtime, std::move(completion));
		}
		/**
		 * @brief Runs confirmed reload preparation with the fixture's dry-run environment.
		 * @param controller Application controller.
		 */
		static void reload(AppController &controller)
		{
			controller.handleReloadQmud(false, true);
		}
};

namespace
{
	/**
	 * @brief Composes the production transport stream and GMCP handler for synthetic wire-level tests.
	 */
	class GmcpWireFixture : public QMudGmcp::Session
	{
		public:
			WorldSocketStream                         stream;
			std::function<qint64(const QByteArray &)> write;
			std::function<void()>                    &startTlsFollowsSent{stream.startTlsFollowsSent};
			/**
			 * @brief Connects complete transport frames to the GMCP handler using accepted-write observation.
			 */
			GmcpWireFixture()
			{
				QMudGmcp::Session::write = [this](const QByteArray &bytes) -> qint64
				{
					const auto accepted = write ? write(bytes) : -1;
					if (accepted > 0)
						stream.observeSent(bytes.left(accepted));
					return accepted;
				};
				stream.write = QMudGmcp::Session::write;
				stream.fatal = [this](const QString &error)
				{
					QMudGmcp::Session::close();
					if (fatal)
						fatal(error);
				};
			}
			/**
			 * @brief Starts a synthetic connection using the selected native handler.
			 * @param native Whether native GMCP is selected.
			 * @param generation Connection identity.
			 * @param disabled Whether future MCCP offers must be refused.
			 */
			void reset(bool native, quint64 generation, bool disabled)
			{
				stream.clearHandlers();
				if (native)
					configureHandler();
				stream.reset(disabled);
				QMudGmcp::Session::reset(native, generation);
			}
			/**
			 * @brief Delivers synthetic socket bytes through the production stream decoder.
			 * @param bytes Wire bytes.
			 * @return Legacy-path projection.
			 */
			QByteArray receive(const QByteArray &bytes)
			{
				return stream.receive(bytes);
			}
			/**
			 * @brief Observes accepted wire writes through the production outgoing frame parser.
			 * @param bytes Accepted bytes.
			 */
			void observeWireSent(const QByteArray &bytes)
			{
				stream.observeSent(bytes);
			}
			/**
			 * @brief Updates the stream's future compression preference.
			 * @param disabled Whether to refuse offers.
			 */
			void setCompressionDisabled(bool disabled)
			{
				stream.setCompressionDisabled(disabled);
			}
			/**
			 * @brief Closes both protocol and stream without resetting lifetime statistics.
			 */
			void closeConnection()
			{
				stream.close();
				QMudGmcp::Session::close();
			}
			/**
			 * @brief Checks transport eligibility before serializing GMCP state.
			 * @return GMCP state, or empty when transport needs reconnect.
			 */
			[[nodiscard]] QByteArray saveConnection() const
			{
				return stream.atBoundary() ? QMudGmcp::Session::save() : QByteArray{};
			}
			/**
			 * @brief Restores GMCP state and prepares a fresh transport framing boundary.
			 * @param bytes Saved GMCP data.
			 * @param generation Connection identity.
			 * @param activate Whether to activate immediately.
			 * @return Whether restoration succeeded.
			 */
			bool restoreConnection(const QByteArray &bytes, quint64 generation, bool activate = true)
			{
				if (!QMudGmcp::Session::restore(bytes, generation, activate))
					return false;
				stream.clearHandlers();
				configureHandler();
				stream.reset(false);
				return true;
			}

		private:
			/**
			 * @brief Installs the GMCP protocol at its Telnet option dispatch boundary.
			 */
			void configureHandler()
			{
				stream.setHandler(201, {[this](unsigned char verb, const QByteArray &payload)
				                        { QMudGmcp::Session::receive(verb, payload); },
				                        [this](const QByteArray &payload)
				                        { QMudGmcp::Session::observeSent(payload); }});
			}
	};
	/**
	 * @brief Activates the composed transport/GMCP fixture.
	 * @param session Wire fixture.
	 */
	void activate(GmcpWireFixture &session)
	{
		session.write = [](const QByteArray &bytes) -> qint64 { return bytes.size(); };
		session.reset(true, 17, false);
		session.receive(QByteArray::fromHex("fffbc9"));
	}
	/**
	 * @brief Creates a complete MCCP stream.
	 * @param plain Uncompressed bytes.
	 * @return Zlib bytes.
	 */
	QByteArray compressed(const QByteArray &plain)
	{
		QByteArray result(static_cast<qsizetype>(compressBound(plain.size())), Qt::Uninitialized);
		auto       count = static_cast<uLongf>(result.size());
		if (compress2(reinterpret_cast<Bytef *>(result.data()), &count,
		              reinterpret_cast<const Bytef *>(plain.constData()), plain.size(),
		              Z_DEFAULT_COMPRESSION) != Z_OK)
			return {};
		result.resize(static_cast<qsizetype>(count));
		return result;
	}
	/**
	 * @brief Connects a synthetic native socket without admitting input to the runtime.
	 * @param socket Socket facade.
	 * @param server Local synthetic server.
	 * @return Accepted peer, or null on failure.
	 */
	QTcpSocket *connectNativeSocket(WorldSocket &socket, QTcpServer &server)
	{
		if (!server.listen(QHostAddress::LocalHost, 0))
			return nullptr;
		WorldSocketConnectionSettings settings;
		settings.nativeGmcp = true;
		socket.applyConnectionSettings(settings);
		if (!socket.connectToHost(QStringLiteral("127.0.0.1"), server.serverPort()) ||
		    !QTest::qWaitFor([&] { return server.hasPendingConnections(); }))
			return nullptr;
		return server.nextPendingConnection();
	}
	/**
	 * @brief Waits for a negotiation reply after synthetic bytes, then fences worker processing without
	 * pumping runtime delivery.
	 * @param socket Socket facade.
	 * @param peer Synthetic peer.
	 * @param bytes Bytes preceding the negotiation marker.
	 * @return Whether all bytes have been queued by the worker.
	 */
	bool queueNativeBatch(WorldSocket &socket, QTcpSocket &peer, const QByteArray &bytes)
	{
		const auto input = bytes + QByteArray::fromHex("fffbc9");
		if (peer.write(input) != input.size())
			return false;
		peer.flush();
		QByteArray     reply = peer.readAll();
		QDeadlineTimer deadline(5000);
		while (!reply.contains(QByteArrayLiteral("Core.Hello")))
		{
			if (deadline.hasExpired() || !peer.waitForReadyRead(static_cast<int>(deadline.remainingTime())))
				return false;
			reply += peer.readAll();
		}
		return socket.sendPacket({}) == 0 && socket.gmcpPublication()->current.load()->negotiated;
	}

	/**
	 * @brief Computes the build-specific browser gate.
	 * @param native Whether native connection state exists.
	 * @return Whether the menu is enabled.
	 */
	bool browserAllowed(bool native)
	{
#ifndef NDEBUG
		Q_UNUSED(native);
		return true;
#else
		return native;
#endif
	}
	/**
	 * @brief Finds one package or field in the direct JSON model.
	 * @param tree Tree view to search.
	 * @param path Component labels from the root.
	 * @return Matching index, or invalid.
	 */
	QModelIndex treeIndex(const QTreeView *tree, const QStringList &path)
	{
		QModelIndex parent;
		for (const auto &part : path)
		{
			QModelIndex next;
			for (int i = 0; i < tree->model()->rowCount(parent); ++i)
			{
				const auto child = tree->model()->index(i, 0, parent);
				if (child.data().toString() == part)
				{
					next = child;
					break;
				}
			}
			if (!next.isValid())
				return {};
			parent = next;
		}
		return parent;
	}
	/**
	 * @brief Reads the value column of a model index.
	 * @param index Package or field index.
	 * @return Displayed scalar or container description.
	 */
	QString treeValue(const QModelIndex &index)
	{
		return index.siblingAtColumn(1).data().toString();
	}
	/**
	 * @brief Counts history-view model access without allocating or traversing synthetic records.
	 */
	class HistoryLayoutModel final : public QAbstractListModel
	{
		public:
			/**
			 * @brief Starts a synthetic newest-first timestamp list.
			 * @param rows Initial retained row count.
			 */
			explicit HistoryLayoutModel(int rows) : m_rows(rows), m_newest(rows)
			{
			}
			/**
			 * @brief Counts retained root rows without visiting records.
			 * @param parent Parent index.
			 * @return Retained count, or zero for child indexes.
			 */
			int rowCount(const QModelIndex &parent) const override
			{
				return parent.isValid() ? 0 : m_rows;
			}
			/**
			 * @brief Counts every row-index request, including geometry work that never asks for size hints.
			 * @param row Requested row.
			 * @param column Requested column.
			 * @param parent Parent index.
			 * @return Valid root index, or an invalid index outside the model.
			 */
			QModelIndex index(int row, int column, const QModelIndex &parent) const override
			{
				++indexRequests;
				return !parent.isValid() && column == 0 && row >= 0 && row < m_rows ? createIndex(row, column)
				                                                                    : QModelIndex{};
			}
			/**
			 * @brief Supplies a synthetic timestamp or stable identity and observes display requests.
			 * @param index Requested row.
			 * @param role Requested data role.
			 * @return Timestamp, row identity, or an empty value.
			 */
			QVariant data(const QModelIndex &index, int role) const override
			{
				if (!index.isValid())
					return {};
				if (role == Qt::DisplayRole)
				{
					++displayRequests;
					return QStringLiteral("2026-01-02 12:00:00.000");
				}
				return role == Qt::UserRole ? QVariant(m_newest - index.row()) : QVariant{};
			}
			/**
			 * @brief Announces one incoming history record without moving any existing records.
			 */
			void prepend()
			{
				beginInsertRows({}, 0, 0);
				++m_rows;
				++m_newest;
				endInsertRows();
			}
			/**
			 * @brief Announces eviction of the oldest records without relocating surviving rows.
			 * @param count Number of oldest rows to remove.
			 */
			void removeOldest(int count)
			{
				beginRemoveRows({}, m_rows - count, m_rows - 1);
				m_rows -= count;
				endRemoveRows();
			}
			/**
			 * @brief Simulates the model reset used to rebind a paused history.
			 */
			void rebind()
			{
				beginResetModel();
				endResetModel();
			}
			/**
			 * @brief Starts a fresh view-access observation interval.
			 */
			void clearRequests() const
			{
				indexRequests = displayRequests = 0;
			}
			mutable quint64 indexRequests{0};
			mutable quint64 displayRequests{0};

		private:
			int m_rows;
			int m_newest;
	};

#ifdef QMUD_ENABLE_LUA_SCRIPTING
	/**
	 * @brief Executes assertions and checks committed completion using the runtime Lua executor.
	 * @param runtime Owning runtime.
	 * @param code Assertion script.
	 * @param error Optional failure diagnostic, cleared on success.
	 * @return Whether the script completed without errors.
	 */
	bool run(WorldRuntime &runtime, const QString &code, QString *error = nullptr)
	{
		const QString resultName = QStringLiteral("gmcp_test_completion");
		runtime.deleteVariable(resultName);
		const QString wrapped = QStringLiteral("local ok,err=xpcall(function()\n") + code +
		                        QStringLiteral("\nend,debug.traceback)\nSetVariable('gmcp_test_completion',"
		                                       "ok and 'passed' or ('failed:'..tostring(err)))");
		const bool    dispatched = runtime.dispatchLuaExecuteScript(
		    runtime.luaCallbacks(), wrapped, QStringLiteral("synthetic GMCP test"), LuaCallbackEvent::None);
		QString    outcome;
		const bool found = runtime.findVariable(resultName, outcome);
		runtime.deleteVariable(resultName);
		const bool succeeded = dispatched && found && outcome == QLatin1String("passed");
		if (error)
			*error = succeeded ? QString()
			                   : (found ? outcome
			                            : QStringLiteral("Lua test did not publish a completion result."));
		return succeeded;
	}
	/**
	 * @brief Writes a synthetic Lua plugin beneath the test build directory.
	 * @param root Fixture root.
	 * @param name Synthetic file/name stem.
	 * @param id Synthetic plugin identity.
	 * @param script Synthetic Lua source.
	 * @param sequence Plugin execution order.
	 * @return Whether the complete fixture was written.
	 */
	bool writeGmcpPlugin(const QString &root, const QString &name, const QString &id, const QString &script,
	                     int sequence = 100)
	{
		QFile file(QDir(root).filePath(name + QStringLiteral(".xml")));
		if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
			return false;
		const auto data =
		    QStringLiteral("<qmud><plugin name=\"Synthetic_%1\" id=\"%2\" language=\"Lua\" "
		                   "save_state=\"n\" sequence=\"%4\"><script><![CDATA[%3]]></script></plugin></qmud>")
		        .arg(name, id, script, QString::number(sequence))
		        .toUtf8();
		return file.write(data) == data.size();
	}
	/**
	 * @brief Waits for preceding GMCP notifications using an explicit no-op Lua command.
	 * @param runtime Owning runtime.
	 */
	void gmcpBarrier(WorldRuntime &runtime)
	{
		LuaBatchDispatchRequest request;
		request.kind          = LuaBatchDispatchKind::NoArgs;
		request.callbackEvent = LuaCallbackEvent::GmcpMessage;
		request.functionName  = QStringLiteral("GmcpTestBarrier");
		request.engines       = {
		    QSharedPointer<LuaCallbackEngine>(runtime.luaCallbacks(), [](LuaCallbackEngine *) {})};
		static_cast<void>(WorldRuntimeTestAccess::queuePluginCallbackDispatchResult(runtime, request));
	}
	/**
	 * @brief Observes only the synthetic Lua modal window.
	 */
	class GmcpModalObserver final : public QObject
	{
		public:
			std::function<void(QDialog *)> shown;
			/**
			 * @brief Invokes the fixture action when its real modal dialog opens.
			 * @param object Event receiver.
			 * @param event Delivered GUI event.
			 * @return False to preserve normal event processing.
			 */
			bool                           eventFilter(QObject *object, QEvent *event) override
			{
				if (event->type() == QEvent::Show)
					if (auto *dialog = qobject_cast<QDialog *>(object);
					    dialog && dialog->windowTitle() == QStringLiteral("Synthetic GMCP modal"))
						shown(dialog);
				return false;
			}
	};
#endif
	/**
	 * @brief Integration fixture using only synthetic protocol messages.
	 */
	class tst_GmcpIntegration final : public QObject
	{
			Q_OBJECT
		private slots:
			/**
			 * @brief Delivers worker-measured Core RTT and Goodbye output through the real socket lifecycle.
			 */
			static void coreRuntimeMessages()
			{
				WorldRuntime runtime;
				WorldView    view;
				view.setRuntime(&runtime);
				const auto detach = qScopeGuard([&] { view.setRuntime(nullptr); });
				view.show();
				runtime.applyDefaultWorldOptions();
				runtime.setWorldAttribute(QStringLiteral("native_gmcp"), QStringLiteral("1"));
				QSignalSpy roundTrips(&runtime, &WorldRuntime::gmcpRoundTripChanged);
				QTcpServer server;
				QVERIFY(server.listen(QHostAddress::LocalHost, 0));
				QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
				QTRY_VERIFY(runtime.isConnected() && server.hasPendingConnections());
				auto *peer = server.nextPendingConnection();
				QCOMPARE(peer->write(QByteArray::fromHex("fffbc9")), qint64{3});
				peer->flush();
				QTRY_VERIFY(runtime.gmcpPublication()->current.load()->negotiated);
				const auto session = runtime.gmcpPublication()->current.load()->session;
				QVERIFY(runtime.sendGmcp("Core.Ping", session));
				const auto response = QMudGmcp::frame("Core.Ping");
				QCOMPARE(peer->write(response), response.size());
				peer->flush();
				QTRY_VERIFY(runtime.gmcpPublication()->current.load()->roundTripMs >= 0);
				QTRY_VERIFY(!roundTrips.isEmpty());
				const auto goodbye = QMudGmcp::frame("Core.Goodbye \"Synthetic final message\"");
				QCOMPARE(peer->write(goodbye), goodbye.size());
				peer->disconnectFromHost();
				QTRY_VERIFY(std::ranges::any_of(
				    runtime.lines(), [](const WorldRuntime::LineEntry &line)
				    { return line.text.contains(QStringLiteral("Synthetic final message")); }));
				QTRY_VERIFY(!runtime.isConnected());
				runtime.setWorldFileModified(false);
			}

			/**
			 * @brief Dispatches a synthetic OOB option without installing any GMCP handler.
			 */
			static void transportIndependentOption()
			{
				WorldSocketStream                       stream;
				QList<QPair<unsigned char, QByteArray>> received;
				QList<QByteArray>                       sent;
				stream.write = [](const QByteArray &bytes) -> qint64 { return bytes.size(); };
				stream.setHandler(77, {[&](unsigned char verb, const QByteArray &payload)
				                       { received.append({verb, payload}); },
				                       [&](const QByteArray &payload) { sent.append(payload); }});
				stream.reset(false);
				const auto frame = QByteArray::fromHex("fffa4d61ffff62fff0");
				for (const char byte : frame)
					QCOMPARE(stream.receive(QByteArray(1, byte)), QByteArray{});
				QCOMPARE(received.size(), 1);
				QCOMPARE(received.first().first, static_cast<unsigned char>(250));
				QCOMPARE(received.first().second, QByteArray::fromHex("61ff62"));
				stream.observeSent(frame);
				QCOMPARE(sent, QList<QByteArray>{QByteArray::fromHex("61ff62")});
				QCOMPARE(stream.receive(QByteArray::fromHex("fffac978fff0")),
				         QByteArray::fromHex("fffac978fff0"));
				const auto zipped = compressed(QByteArrayLiteral("plain") + frame);
				QCOMPARE(stream.receive(QByteArray::fromHex("fffb56fffa56fff0") + zipped),
				         QByteArrayLiteral("plain"));
				QCOMPARE(received.size(), 2);
				QCOMPARE(stream.statistics().compressedBytes, zipped.size());
				const auto count = stream.statistics().compressedBytes;
				stream.close();
				stream.clearHandlers();
				stream.reset(false);
				QCOMPARE(stream.statistics().compressedBytes, count);
				QCOMPARE(stream.receive(frame), frame);
				int upgrades               = 0;
				stream.startTlsFollowsSent = [&] { ++upgrades; };
				const auto follows         = QByteArray::fromHex("fffa2e01fff0");
				for (const char byte : follows)
					stream.observeSent(QByteArray(1, byte));
				QCOMPARE(upgrades, 1);
			}
			/**
			 * @brief Keeps world compression totals across reconnects and both decoder ownership transitions.
			 */
			static void transportStatisticsAcrossConnections()
			{
				WorldRuntime runtime;
				QTcpServer   server;
				QVERIFY(server.listen(QHostAddress::LocalHost, 0));
				const auto plain                = QByteArrayLiteral("synthetic compression sample\n");
				const auto zipped               = compressed(plain);
				qint64     expectedCompressed   = 0;
				qint64     expectedUncompressed = 0;
				for (const bool native : {true, true, false, true, false})
				{
					runtime.setWorldAttribute(QStringLiteral("native_gmcp"),
					                          native ? QStringLiteral("1") : QStringLiteral("0"));
					QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
					QTRY_VERIFY(runtime.isConnected() && server.hasPendingConnections());
					auto      *peer       = server.nextPendingConnection();
					const auto gmcpBefore = runtime.gmcpPublication()->current.load();
					const auto bytes      = QByteArray::fromHex("fffb56fffa56fff0") + zipped;
					QCOMPARE(peer->write(bytes), bytes.size());
					peer->flush();
					expectedCompressed += zipped.size();
					expectedUncompressed += plain.size();
					QTRY_COMPARE(runtime.totalCompressedBytes(), expectedCompressed);
					QTRY_COMPARE(runtime.totalUncompressedBytes(), expectedUncompressed);
					QCOMPARE(runtime.runtimeCountersSnapshot(false).totalCompressedBytes, expectedCompressed);
					QCOMPARE(runtime.runtimeCountersSnapshot(false).totalUncompressedBytes,
					         expectedUncompressed);
					QCOMPARE(runtime.gmcpPublication()->current.load(), gmcpBefore);
					runtime.disconnectFromWorld();
					QTRY_VERIFY(!runtime.isConnected());
					QTRY_COMPARE(peer->state(), QAbstractSocket::UnconnectedState);
					delete peer;
					QCOMPARE(runtime.totalCompressedBytes(), expectedCompressed);
					QCOMPARE(runtime.totalUncompressedBytes(), expectedUncompressed);
				}
			}

			/**
			 * @brief Observes outgoing GMCP and START-TLS boundaries across arbitrary write splits.
			 */
			static void transportOutgoingFraming()
			{
				QList<QMudGmcp::Record> records;
				int                     upgrades = 0;
				GmcpWireFixture         session;
				activate(session);
				session.observed = [&](const auto &record, const auto &) { records.push_back(record); };
				session.startTlsFollowsSent = [&] { ++upgrades; };
				const auto gmcp             = QMudGmcp::frame("Test.Send {\"value\":3}");
				for (const char c : gmcp)
					session.observeWireSent(QByteArray(1, c));
				QCOMPARE(records.size(), 1);
				QVERIFY(records.first().outgoing);
				QCOMPARE(records.first().raw, QByteArray("Test.Send {\"value\":3}"));
				const auto tls = QByteArray::fromHex("fffa2e01fff0");
				for (const char c : tls)
					session.observeWireSent(QByteArray(1, c));
				QCOMPARE(upgrades, 1);
				session.observeWireSent(QByteArray::fromHex("fffa6401fffffa2e01fffff0fff0"));
				QCOMPARE(upgrades, 1);
			}
			/**
			 * @brief Preserves nonnative data and handles every split of a native frame.
			 */
			static void transportFraming()
			{
				const QByteArray raw    = QByteArrayLiteral("Test.Value {\"label\":\"snow ") +
				                          QByteArray::fromHex("e29883") + QByteArrayLiteral("\"}");
				const auto       packet = QMudGmcp::frame(raw);
				for (qsizetype split = 0; split <= packet.size(); ++split)
				{
					GmcpWireFixture session;
					activate(session);
					QCOMPARE(session.receive(QByteArrayLiteral("left") + packet.left(split)),
					         QByteArray("left"));
					QCOMPARE(session.receive(packet.mid(split) + QByteArrayLiteral("right")),
					         QByteArray("right"));
					QCOMPARE(session.publication()
					             ->current.load()
					             ->messages.value(QStringLiteral("test.value"))
					             ->raw,
					         raw);
				}
				GmcpWireFixture legacy;
				legacy.reset(false, 1, false);
				QCOMPARE(legacy.receive(QByteArray::fromHex("fffbc9") + packet),
				         QByteArray::fromHex("fffbc9") + packet);
				GmcpWireFixture native;
				activate(native);
				const QByteArray unknown = QByteArray::fromHex("fffaa861ffff62fff0");
				QCOMPARE(native.receive(unknown), unknown);
				QCOMPARE(native.receive(QByteArray::fromHex("ffff")), QByteArray::fromHex("ffff"));
			}
			/**
			 * @brief Exercises MCCP1/2 byte splits and uncompressed data following zlib termination.
			 */
			static void transportCompression()
			{
				for (const int option : {85, 86})
				{
					const auto body   = QByteArrayLiteral("left") +
					                    QMudGmcp::frame("Test.State {\"value\":7}") +
					                    QByteArrayLiteral("right");
					const auto zipped = compressed(body);
					QVERIFY(!zipped.isEmpty());
					for (qsizetype split = 0; split <= zipped.size(); ++split)
					{
						GmcpWireFixture session;
						activate(session);
						QByteArray activation = QByteArray::fromHex("fffa");
						activation += static_cast<char>(option);
						if (option == 85)
							activation += static_cast<char>(251);
						activation += QByteArray::fromHex(option == 85 ? "f0" : "fff0");
						QByteArray output = session.receive(activation + zipped.left(split));
						output += session.receive(zipped.mid(split) + QByteArrayLiteral("tail"));
						QCOMPARE(output, QByteArray("leftrighttail"));
						const auto state = session.publication()->current.load();
						QCOMPARE(session.stream.statistics().mccpType, 0);
						QCOMPARE(state->messages.value(QStringLiteral("test.state"))
						             ->state.toObject()
						             .value(QStringLiteral("value"))
						             .toInt(),
						         7);
					}
				}
			}
			/**
			 * @brief Supplies both MCCP1 marker forms and the MCCP2 marker.
			 */
			static void transportCompressionPreferenceTransitions_data()
			{
				QTest::addColumn<QByteArray>("start");
				QTest::newRow("mccp1-short") << QByteArray::fromHex("fffa55fbf0");
				QTest::newRow("mccp1-iac") << QByteArray::fromHex("fffa55fbfff0");
				QTest::newRow("mccp2") << QByteArray::fromHex("fffa56fff0");
			}
			/**
			 * @brief Keeps received compression decodable across preference changes, reload and reset.
			 */
			static void transportCompressionPreferenceTransitions()
			{
				QFETCH(QByteArray, start);
				const auto offer  = QByteArray::fromHex("fffb") + start.mid(2, 1);
				const auto accept = QByteArray::fromHex("fffd") + start.mid(2, 1);
				const auto refuse = QByteArray::fromHex("fffe") + start.mid(2, 1);
				const auto body   = QByteArrayLiteral("left") + QMudGmcp::frame("Test.State {\"value\":7}") +
				                    QByteArrayLiteral("right");
				const auto zipped = compressed(body);
				QVERIFY(zipped.size() > 2);
				const auto wire = start + zipped + QByteArrayLiteral("tail");
				for (qsizetype split = 0; split <= wire.size(); ++split)
				{
					GmcpWireFixture session;
					activate(session);
					session.receive(offer);
					auto output = session.receive(wire.left(split));
					session.setCompressionDisabled(true);
					output += session.receive(wire.mid(split));
					QCOMPARE(output, QByteArray("leftrighttail"));
					const auto state   = session.publication()->current.load();
					const auto message = state->messages.value(QStringLiteral("test.state"));
					QVERIFY(message);
					QCOMPARE(message->state.toObject().value(QStringLiteral("value")).toInt(), 7);
					QCOMPARE(session.stream.statistics().mccpType, 0);
				}

				QByteArray      writes;
				GmcpWireFixture session;
				activate(session);
				session.write = [&](const QByteArray &bytes) -> qint64
				{
					writes += bytes;
					return bytes.size();
				};
				session.receive(offer);
				QCOMPARE(writes, accept);
				session.setCompressionDisabled(true);
				session.receive(offer);
				QCOMPARE(writes, accept + refuse);
				QCOMPARE(session.receive(wire), QByteArray("leftrighttail"));

				GmcpWireFixture before;
				activate(before);
				before.receive(offer);
				before.receive(start.left(2));
				QVERIFY(before.saveConnection().isEmpty());
				before.setCompressionDisabled(true);
				QCOMPARE(before.receive(start.mid(2) + zipped + "tail"), QByteArray("leftrighttail"));
				QVERIFY(!before.saveConnection().isEmpty());

				session.reset(true, 3, false);
				session.receive(offer);
				session.receive(start + zipped.left(2));
				QVERIFY(session.stream.statistics().mccpType != 0);
				session.setCompressionDisabled(true);
				session.closeConnection();
				QCOMPARE(session.stream.statistics().mccpType, 0);
				session.reset(true, 4, true);
				writes.clear();
				session.receive(offer);
				QCOMPARE(writes, refuse);
				QCOMPARE(session.receive("ordinary text"), QByteArray("ordinary text"));
				QVERIFY(session.publication()->current.load()->messages.isEmpty());
				session.setCompressionDisabled(false);
				writes.clear();
				session.receive(offer);
				QCOMPARE(writes, accept);
				session.setCompressionDisabled(true);
				int failures  = 0;
				session.fatal = [&](const QString &) { ++failures; };
				QCOMPARE(session.receive(start + QByteArrayLiteral("invalid compressed bytes")),
				         QByteArray());
				QCOMPARE(failures, 1);
				QCOMPARE(session.receive("later bytes"), QByteArray());
			}

			/**
			 * @brief Verifies the configured build-directory home and creates the fixture storage tree.
			 */
			static void initTestCase()
			{
				QCOMPARE(qEnvironmentVariable("QMUD_HOME"), QStringLiteral(QMUD_TEST_BUILD_DIR));
				QVERIFY(QDir(QStringLiteral(QMUD_TEST_BUILD_DIR)).mkpath(QStringLiteral("worlds")));
			}
			/**
			 * @brief Verifies that new-world initialization changes only GMCP and preserves every ordinary
			 * connection default used by New World and Quick Connect.
			 */
			static void newWorldInitialization()
			{
				AppController controller;
				AppControllerTestAccess::setHome(
				    controller, QDir(QStringLiteral(QMUD_TEST_BUILD_DIR)).filePath(QStringLiteral("worlds")));
				WorldRuntime                 ordinary;
				WorldRuntime                 fresh;
				const QMap<QString, QString> connectionFields{
				    {QStringLiteral("id"),       QStringLiteral("010101010101010101010101")},
				    {QStringLiteral("name"),     QStringLiteral("Synthetic connection")    },
				    {QStringLiteral("site"),     QStringLiteral("example.invalid")         },
				    {QStringLiteral("port"),     QStringLiteral("43123")                   },
				    {QStringLiteral("player"),   QStringLiteral("synthetic-player")        },
				    {QStringLiteral("password"), QStringLiteral("synthetic-password")      }
                };
				for (auto field = connectionFields.cbegin(); field != connectionFields.cend(); ++field)
				{
					ordinary.setWorldAttribute(field.key(), field.value());
					fresh.setWorldAttribute(field.key(), field.value());
				}
				AppControllerTestAccess::initialize(controller, ordinary, false);
				AppControllerTestAccess::initialize(controller, fresh, true);
				auto expected = ordinary.worldAttributes();
				expected.insert(QStringLiteral("native_gmcp"), QStringLiteral("1"));
				QCOMPARE(fresh.worldAttributes(), expected);
				QCOMPARE(fresh.worldMultilineAttributes(), ordinary.worldMultilineAttributes());
				QVERIFY(fresh.nativeGmcpConfigured());
				QVERIFY(!fresh.nativeGmcpAvailable());
				ordinary.setWorldFileModified(false);
				fresh.setWorldFileModified(false);
			}
			/**
			 * @brief Enables native GMCP for empty-document creation while old files retain their saved
			 * connection fields and the legacy GMCP default.
			 */
			static void newWorldDocumentDefaults()
			{
				QTemporaryDir directory(QDir(QStringLiteral(QMUD_TEST_BUILD_DIR))
				                            .filePath(QStringLiteral("worlds/gmcp-new-world-XXXXXX")));
				QVERIFY(directory.isValid());
				AppController controller;
				MainWindow    window;
				controller.setMainWindow(&window);
				AppControllerTestAccess::setHome(controller, directory.path());
				controller.setGlobalOptionInt(QStringLiteral("ConfirmBeforeClosingWorld"), 0);
				controller.setGlobalOptionInt(QStringLiteral("ConfirmBeforeSavingVariables"), 0);
				QVERIFY(AppControllerTestAccess::openDocument(controller, QString()));
				QCOMPARE(AppControllerTestAccess::worlds(controller).size(), 1);
				auto *fresh = AppControllerTestAccess::worlds(controller).first();
				QVERIFY(fresh->nativeGmcpConfigured());
				QVERIFY(!fresh->isConnected() && !fresh->isConnecting());
				fresh->setWorldFileModified(false);
				const auto fileName = QDir(directory.path()).filePath(QStringLiteral("old-world.qdl"));
				QFile      file(fileName);
				QVERIFY(file.open(QIODevice::WriteOnly));
				const QByteArray xml(
				    R"(<?xml version="1.0"?><muclient><world name="Synthetic old world" site="example.invalid" port="43124" player="synthetic-player"/></muclient>)");
				QCOMPARE(file.write(xml), xml.size());
				file.close();
				QVERIFY(AppControllerTestAccess::openDocument(controller, fileName));
				QCOMPARE(AppControllerTestAccess::worlds(controller).size(), 2);
				for (auto *runtime : AppControllerTestAccess::worlds(controller))
				{
					if (runtime == fresh)
						continue;
					QVERIFY(!runtime->nativeGmcpConfigured());
					QCOMPARE(runtime->worldAttributes().value(QStringLiteral("site")),
					         QStringLiteral("example.invalid"));
					QCOMPARE(runtime->worldAttributes().value(QStringLiteral("port")),
					         QStringLiteral("43124"));
					QCOMPARE(runtime->worldAttributes().value(QStringLiteral("player")),
					         QStringLiteral("synthetic-player"));
					QVERIFY(!runtime->isConnected() && !runtime->isConnecting());
					runtime->setWorldFileModified(false);
				}
			}
			/**
			 * @brief Keeps the original chunk cap and event-loop budget on the runtime thread.
			 */
			void runtimeDeliveryBudget()
			{
				QTcpServer  server;
				WorldSocket socket;
				auto       *peer = connectNativeSocket(socket, server);
				QVERIFY(peer);
				QByteArray received;
				int        chunks        = 0;
				int        chunksAtYield = 0;
				bool       wrongThread   = false;
				bool       oversized     = false;
				connect(&socket, &WorldSocketService::rawDataReceived, this,
				        [&](const QByteArray &bytes)
				        {
					        received += bytes;
					        ++chunks;
					        wrongThread |= QThread::currentThread() != socket.thread();
					        oversized |= bytes.size() > 16 * 1024;
					        if (chunks == 1)
						        QMetaObject::invokeMethod(
						            &socket, [&] { chunksAtYield = chunks; }, Qt::QueuedConnection);
				        });
				const QByteArray input(10 * 16 * 1024, 'x');
				QVERIFY(queueNativeBatch(socket, *peer, input));
				QTRY_COMPARE(received, input);
				QVERIFY(chunksAtYield > 0 && chunksAtYield <= 4);
				QVERIFY(!wrongThread);
				QVERIFY(!oversized);
			}

			/**
			 * @brief Captures raw input only when debugging was enabled at read time and preserves runtime
			 * accounting.
			 */
			void nativePacketDebugCapture()
			{
				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("native_gmcp"), QStringLiteral("1"));
				QTcpServer server;
				QVERIFY(server.listen(QHostAddress::LocalHost, 0));
				QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
				QTRY_VERIFY(runtime.isConnected() && server.hasPendingConnections());
				auto *peer   = server.nextPendingConnection();
				auto *socket = runtime.findChild<WorldSocket *>();
				QVERIFY(peer && socket);
				qint64            counted = 0;
				int               packets = 0;
				QList<QByteArray> captures;
				const auto        accountingConnection =
				    connect(socket, &WorldSocketService::rawDataReceived, this,
				            [&](const QByteArray &, const WorldSocketInputMetadata &metadata)
				            {
					            if (metadata.receivedBytes == 0)
						            return;
					            counted += metadata.receivedBytes;
					            ++packets;
					            captures.push_back(metadata.debugPacket);
				            });
				const auto disconnectAccounting = qScopeGuard([&] { disconnect(accountingConnection); });
				const auto first                = QMudGmcp::frame("Test.Capture {\"value\":1}");
				QVERIFY(queueNativeBatch(*socket, *peer, first));
				runtime.setDebugIncomingPackets(true);
				QCoreApplication::sendPostedEvents(socket, QEvent::MetaCall);
				QCOMPARE(counted, first.size() + 3);
				QVERIFY(std::ranges::all_of(captures, [](const auto &bytes) { return bytes.isEmpty(); }));
				QCOMPARE(runtime.bytesIn(), counted);
				QCOMPARE(runtime.inputPacketCount(), packets);
				captures.clear();
				const auto second =
				    QMudGmcp::frame("Test.Capture {\"value\":2}") + QByteArray::fromHex("fffb56");
				QCOMPARE(peer->write(second), second.size());
				peer->flush();
				QByteArray     reply = peer->readAll();
				QDeadlineTimer deadline(5000);
				while (!reply.contains(QByteArray::fromHex("fffd56")))
				{
					QVERIFY(!deadline.hasExpired());
					QVERIFY(peer->waitForReadyRead(static_cast<int>(deadline.remainingTime())));
					reply += peer->readAll();
				}
				runtime.setDebugIncomingPackets(false);
				QCoreApplication::sendPostedEvents(socket, QEvent::MetaCall);
				QCOMPARE(QByteArrayList(captures).join(), second);
				QCOMPARE(counted, first.size() + 3 + second.size());
				QCOMPARE(runtime.bytesIn(), counted);
				QCOMPARE(runtime.inputPacketCount(), packets);
				captures.clear();
				const auto third = QMudGmcp::frame("Test.Capture {\"value\":3}");
				QCOMPARE(peer->write(third), third.size());
				peer->flush();
				QTRY_COMPARE(counted, first.size() + 3 + second.size() + third.size());
				QVERIFY(std::ranges::all_of(captures, [](const auto &bytes) { return bytes.isEmpty(); }));
				QCOMPARE(runtime.bytesIn(), counted);
				QCOMPARE(runtime.inputPacketCount(), packets);
				runtime.setWorldFileModified(false);
			}

			/**
			 * @brief Exercises both a pending receive notification and a pending budget continuation.
			 */
			static void greetingWaitsForConnectionInitialization_data()
			{
				QTest::addColumn<bool>("pendingContinuation");
				QTest::newRow("receive-notification") << false;
				QTest::newRow("budget-continuation") << true;
			}

			/**
			 * @brief Preserves the new greeting when a previous connection has queued runtime receive work.
			 */
			void greetingWaitsForConnectionInitialization()
			{
				QFETCH(bool, pendingContinuation);
				QTcpServer server;
				QVERIFY(server.listen(QHostAddress::LocalHost, 0));
				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("native_gmcp"), QStringLiteral("1"));
				QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
				QTRY_VERIFY(runtime.isConnected() && server.hasPendingConnections());
				auto *oldPeer = server.nextPendingConnection();
				auto *socket  = runtime.findChild<WorldSocket *>();
				QVERIFY(socket);
				bool        initialized                  = true;
				bool        receivedBeforeInitialization = false;
				QByteArray  received;
				QStringList lines;
				connect(&runtime, &WorldRuntime::connected, this, [&] { initialized = true; });
				connect(socket, &WorldSocketService::rawDataReceived, this,
				        [&](const QByteArray &bytes)
				        {
					        receivedBeforeInitialization |= !initialized;
					        received += bytes;
				        });
				connect(&runtime, &WorldRuntime::incomingLineReceived, this,
				        [&](const QString &line) { lines.push_back(line); });
				const QByteArray oldInput(pendingContinuation ? 10 * 16 * 1024 : 3, 'a');
				QVERIFY(queueNativeBatch(*socket, *oldPeer, oldInput));
				if (pendingContinuation)
				{
					QCoreApplication::sendPostedEvents(socket, QEvent::MetaCall);
					QVERIFY(!received.isEmpty());
					QVERIFY(received.size() < oldInput.size());
				}
				runtime.closeSocketForReloadReconnect();
				initialized                  = false;
				receivedBeforeInitialization = false;
				received.clear();
				QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
				QVERIFY(server.waitForNewConnection(5000));
				auto *newPeer = server.nextPendingConnection();
				QVERIFY(newPeer);
				// The negotiation reply and worker fence establish that the greeting is queued while
				// the runtime still has the previous connection's receive work ahead of connected.
				QVERIFY(queueNativeBatch(*socket, *newPeer, QByteArrayLiteral("new")));
				QCoreApplication::sendPostedEvents(socket, QEvent::MetaCall);
				QTRY_COMPARE(received, QByteArrayLiteral("new"));
				QVERIFY(initialized);
				QVERIFY(!receivedBeforeInitialization);
				newPeer->write("\n");
				newPeer->flush();
				QTRY_COMPARE(lines.size(), 1);
				QCOMPARE(lines.constFirst(), QStringLiteral("new"));
				runtime.setWorldFileModified(false);
			}

			/**
			 * @brief Keeps input buffered when initialization pumps nested runtime events.
			 */
			void inputWaitsWhileConnectedHandlerRuns()
			{
				QTcpServer server;
				QVERIFY(server.listen(QHostAddress::LocalHost, 0));
				WorldSocket                   socket;
				WorldSocketConnectionSettings settings;
				settings.nativeGmcp = true;
				socket.applyConnectionSettings(settings);
				bool       initializing                  = false;
				bool       queued                        = false;
				bool       buffered                      = false;
				bool       deliveredDuringInitialization = false;
				QByteArray received;
				connect(&socket, &WorldSocketService::rawDataReceived, this,
				        [&](const QByteArray &bytes)
				        {
					        deliveredDuringInitialization |= initializing;
					        received += bytes;
				        });
				connect(&socket, &WorldSocketService::connected, this,
				        [&]
				        {
					        initializing = true;
					        if (server.hasPendingConnections() || server.waitForNewConnection(5000))
					        {
						        auto *peer = server.nextPendingConnection();
						        queued     = queueNativeBatch(socket, *peer, QByteArrayLiteral("hello"));
						        QCoreApplication::sendPostedEvents(&socket, QEvent::MetaCall);
						        buffered = received.isEmpty();
					        }
					        initializing = false;
				        });
				QVERIFY(socket.connectToHost(QStringLiteral("127.0.0.1"), server.serverPort()));
				QTRY_COMPARE(received, QByteArrayLiteral("hello"));
				QVERIFY(queued);
				QVERIFY(buffered);
				QVERIFY(!deliveredDuringInitialization);
			}

			/**
			 * @brief Preserves the newest generation when an older connected handler reconnects and pumps
			 * events.
			 */
			void connectionInitializationCanReconnect()
			{
				QTcpServer firstServer;
				QTcpServer secondServer;
				QVERIFY(firstServer.listen(QHostAddress::LocalHost, 0));
				QVERIFY(secondServer.listen(QHostAddress::LocalHost, 0));
				WorldSocket                   socket;
				WorldSocketConnectionSettings settings;
				settings.nativeGmcp = true;
				socket.applyConnectionSettings(settings);
				int         connected   = 0;
				bool        reconnected = false;
				bool        negotiated  = false;
				QTcpSocket *peer        = nullptr;
				QByteArray  received;
				connect(&socket, &WorldSocketService::rawDataReceived, this,
				        [&](const QByteArray &bytes) { received += bytes; });
				connect(&socket, &WorldSocketService::connected, this,
				        [&]
				        {
					        ++connected;
					        if (connected == 1)
					        {
						        reconnected = socket.connectToHost(QStringLiteral("127.0.0.1"),
						                                           secondServer.serverPort()) &&
						                      QTest::qWaitFor([&] { return connected == 2; });
					        }
					        else if (connected == 2 && (secondServer.hasPendingConnections() ||
					                                    secondServer.waitForNewConnection(5000)))
					        {
						        peer       = secondServer.nextPendingConnection();
						        negotiated = queueNativeBatch(socket, *peer, QByteArrayLiteral("new"));
					        }
				        });
				QVERIFY(socket.connectToHost(QStringLiteral("127.0.0.1"), firstServer.serverPort()));
				QTRY_VERIFY(reconnected);
				QVERIFY(negotiated);
				QVERIFY(peer);
				QTRY_COMPARE(received, QByteArrayLiteral("new"));
				peer->write("more");
				peer->flush();
				QTRY_COMPARE(received, QByteArrayLiteral("newmore"));
				QCOMPARE(connected, 2);
			}

			/**
			 * @brief Drains worker input while reception is frozen, without storing transport bytes in GMCP
			 * state.
			 */
			void frozenWorkerDrainsRuntimeInput()
			{
				QTcpServer  server;
				WorldSocket socket;
				auto       *peer = connectNativeSocket(socket, server);
				QVERIFY(peer);
				QByteArray received;
				bool       froze = false;
				connect(&socket, &WorldSocketService::rawDataReceived, this,
				        [&](const QByteArray &bytes)
				        {
					        received += bytes;
					        if (!std::exchange(froze, true))
					        {
						        socket.freezeForReload();
						        QVERIFY(socket.saveNativeState().isEmpty());
					        }
				        });
				const QByteArray input(10 * 16 * 1024, 'x');
				QVERIFY(queueNativeBatch(socket, *peer, input));
				QTRY_VERIFY(socket.reloadInputDrained());
				QCOMPARE(received, input);
				QVERIFY(socket.flushForReload());
				const auto saved = socket.saveNativeState();
				QVERIFY(!saved.isEmpty());
				GmcpWireFixture restored;
				QVERIFY(restored.restoreConnection(saved, 2));
				peer->write(QMudGmcp::frame("Test.Frozen {\"value\":9}"));
				peer->flush();
				QCOMPARE(socket.sendPacket({}), 0);
				QVERIFY(!socket.gmcpPublication()->current.load()->messages.contains(
				    QStringLiteral("test.frozen")));
				QVERIFY(socket.sendGmcp("Test.Reply {}", socket.gmcpPublication()->current.load()->session));
				QVERIFY(socket.flushForReload());
				socket.pauseTransport(false);
				QTRY_VERIFY(socket.gmcpPublication()->current.load()->messages.contains(
				    QStringLiteral("test.frozen")));
			}

			/**
			 * @brief Drops old-generation bytes when a delivery callback reconnects.
			 */
			void reconnectDuringRuntimeDelivery()
			{
				QTcpServer  server;
				WorldSocket socket;
				auto       *peer = connectNativeSocket(socket, server);
				QVERIFY(peer);
				QByteArray received;
				bool       reconnected = false;
				connect(&socket, &WorldSocketService::rawDataReceived, this,
				        [&](const QByteArray &bytes)
				        {
					        received += bytes;
					        if (bytes.startsWith('a'))
					        {
						        reconnected =
						            socket.connectToHost(QStringLiteral("127.0.0.1"), server.serverPort()) &&
						            QTest::qWaitFor([&] { return server.hasPendingConnections(); });
						        if (reconnected)
						        {
							        auto *nextPeer = server.nextPendingConnection();
							        nextPeer->write("c");
							        nextPeer->flush();
						        }
					        }
				        });
				const QByteArray prefix(16 * 1024, 'a');
				QVERIFY(queueNativeBatch(socket, *peer, prefix + QByteArray(16 * 1024, 'b')));
				QTRY_COMPARE(received, prefix + 'c');
				QVERIFY(reconnected);
			}

			/**
			 * @brief Stops a live runtime drain safely when a callback destroys the socket facade.
			 */
			void destructionDuringRuntimeDelivery()
			{
				QTcpServer            server;
				auto                 *socket = new WorldSocket;
				QPointer<WorldSocket> guard(socket);
				const auto            cleanup = qScopeGuard([&] { delete guard.data(); });
				auto                 *peer    = connectNativeSocket(*socket, server);
				QVERIFY(peer);
				QByteArray received;
				connect(socket, &WorldSocketService::rawDataReceived, this,
				        [&](const QByteArray &bytes)
				        {
					        received += bytes;
					        delete socket;
				        });
				const QByteArray prefix(16 * 1024, 'a');
				QVERIFY(queueNativeBatch(*socket, *peer, prefix + QByteArray(16 * 1024, 'b')));
				QCoreApplication::sendPostedEvents(socket, QEvent::MetaCall);
				QVERIFY(!guard);
				QCOMPARE(received, prefix);
			}

			/**
			 * @brief Preserves accounting for queued reads when their text is discarded on abort.
			 */
			void discardedInputRetainsAccounting()
			{
				QTcpServer  server;
				WorldSocket socket;
				auto       *peer = connectNativeSocket(socket, server);
				QVERIFY(peer);
				qint64     counted = 0;
				int        reads   = 0;
				QByteArray received;
				connect(&socket, &WorldSocketService::rawDataReceived, this,
				        [&](const QByteArray &bytes, const WorldSocketInputMetadata &metadata)
				        {
					        received += bytes;
					        counted += metadata.receivedBytes;
					        reads += metadata.receivedBytes > 0;
				        });
				const QByteArray input(10 * 16 * 1024, 'x');
				QVERIFY(queueNativeBatch(socket, *peer, input));
				QCOMPARE(counted, 0);
				socket.abortSocket();
				QCOMPARE(counted, input.size() + 3);
				QVERIFY(reads > 0);
				QVERIFY(received.isEmpty());
				QCoreApplication::sendPostedEvents(&socket, QEvent::MetaCall);
				QCOMPARE(counted, input.size() + 3);
			}

			/**
			 * @brief Counts a read even when native decoding closes the socket before publishing its batch.
			 */
			static void failedNativeReadRetainsAccounting()
			{
				WorldRuntime runtime;
				qint64       bytesAtDisconnect = -1;
				connect(&runtime, &WorldRuntime::disconnected, &runtime,
				        [&] { bytesAtDisconnect = runtime.bytesIn(); });
				runtime.setWorldAttribute(QStringLiteral("native_gmcp"), QStringLiteral("1"));
				QTcpServer server;
				QVERIFY(server.listen(QHostAddress::LocalHost, 0));
				QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
				QTRY_VERIFY(runtime.isConnected() && server.hasPendingConnections());
				auto      *peer = server.nextPendingConnection();
				QSignalSpy errors(&runtime, &WorldRuntime::socketError);
				const auto input =
				    QByteArray::fromHex("fffb56fffa56fff0") + QByteArrayLiteral("invalid-stream");
				QCOMPARE(peer->write(input), input.size());
				peer->flush();
				QTRY_VERIFY(!errors.isEmpty());
				QTRY_VERIFY(!runtime.isConnected());
				QTRY_COMPARE(runtime.bytesIn(), input.size());
				QTRY_COMPARE(bytesAtDisconnect, input.size());
				QVERIFY(runtime.inputPacketCount() > 0);
				runtime.setWorldFileModified(false);
			}

			/**
			 * @brief Transfers automatic writes exactly once through success, rejection, and frozen handoffs.
			 */
			void automaticWriteHandoff()
			{
				WorldSocket                   socket;
				WorldSocketConnectionSettings settings;
				settings.nativeGmcp = true;
				socket.applyConnectionSettings(settings);
				QList<WorldSocketProtocolWrite> notifications;
				connect(&socket, &WorldSocketService::nativeProtocolWritten, this,
				        [&](const WorldSocketProtocolWrite &write) { notifications.push_back(write); });
				QTcpServer server;
				QVERIFY(server.listen(QHostAddress::LocalHost, 0));
				QVERIFY(socket.connectToHost(QStringLiteral("127.0.0.1"), server.serverPort()));
				QTRY_VERIFY(socket.isConnected() && server.hasPendingConnections());
				auto      *peer = server.nextPendingConnection();
				QByteArray received;
				connect(peer, &QTcpSocket::readyRead, this, [&] { received += peer->readAll(); });
				peer->write(QByteArray::fromHex("fffbc9"));
				peer->flush();
				QTRY_COMPARE(notifications.size(), 3);
				QTRY_VERIFY(received.contains("Core.Supports.Set"));
				const auto state = socket.gmcpPublication()->current.load();
				QVERIFY(state->negotiated);
				const QByteArray application  = QMudGmcp::frame("Test.Request");
				const auto       changeModule = [&](const QString &name)
				{
					socket.setGmcpModules({
					    {name, 1}
                    });
				};
				changeModule(QStringLiteral("Test.First"));
				const auto successful = socket.writePacket(application, state->session);
				QCOMPARE(successful.accepted, application.size());
				QCOMPARE(successful.precedingProtocolWrites.size(), 1);
				QVERIFY(successful.precedingProtocolWrites.front().payload.contains("Test.First 1"));
				QVERIFY(successful.precedingProtocolWrites.front().timestamp.isValid());
				QCoreApplication::sendPostedEvents(&socket, QEvent::MetaCall);
				QCOMPARE(notifications.size(), 3);
				// Reject an explicit write for a different session without losing the accepted advertisement.
				changeModule(QStringLiteral("Test.Second"));
				const auto rejected = socket.writePacket(application, state->session + 1);
				QCOMPARE(rejected.accepted, -1);
				QCOMPARE(rejected.precedingProtocolWrites.size(), 1);
				QVERIFY(rejected.precedingProtocolWrites.front().payload.contains("Test.Second 1"));
				QCoreApplication::sendPostedEvents(&socket, QEvent::MetaCall);
				QCOMPARE(notifications.size(), 3);
				changeModule(QStringLiteral("Test.Queued"));
				QCoreApplication::sendPostedEvents(&socket, QEvent::MetaCall);
				QCOMPARE(notifications.size(), 4);
				const auto alreadyDelivered = socket.writePacket(QByteArrayLiteral("raw-output"), 0);
				QCOMPARE(alreadyDelivered.accepted, 10);
				QVERIFY(alreadyDelivered.precedingProtocolWrites.isEmpty());
				changeModule(QStringLiteral("Test.Frozen"));
				socket.freezeForReload();
				const auto frozen = socket.writePacket(application, state->session);
				QCOMPARE(frozen.accepted, application.size());
				QCOMPARE(frozen.precedingProtocolWrites.size(), 1);
				QVERIFY(frozen.precedingProtocolWrites.front().payload.contains("Test.Frozen 1"));
				QCoreApplication::sendPostedEvents(&socket, QEvent::MetaCall);
				QVERIFY(socket.reloadInputDrained());
				QCOMPARE(notifications.size(), 4);
				socket.pauseTransport(false);
				QTRY_COMPARE(received.count("Test.Request"), 2);
				QCOMPARE(received.count("Core.Supports.Set"), 5);
			}

#ifdef QMUD_ENABLE_LUA_SCRIPTING
			/**
			 * @brief Retains each read's counters through split input and delayed Lua line processing,
			 * including accounting-only GMCP reads.
			 */
			void receiveCountersFollowInput()
			{
				QTemporaryDir directory(QDir(QStringLiteral(QMUD_TEST_BUILD_DIR))
				                            .filePath(QStringLiteral("worlds/gmcp-counters-XXXXXX")));
				QVERIFY(directory.isValid());
				const QString id = QStringLiteral("161616161616161616161616");
				QVERIFY(
				    writeGmcpPlugin(directory.path(), QStringLiteral("counters"), id, QStringLiteral(R"lua(
function OnPluginPacketReceived(packet)
 return packet
end
function OnPluginLineReceived(line)
 if line=='first' or line=='second' then
  SetVariable(line..'_counts',string.format('%d,%d',GetInfo(204),GetInfo(216)))
 end
 return true
end
)lua")));
				WorldRuntime          runtime;
				WorldView             view;
				WorldCommandProcessor processor;
				processor.setView(&view);
				processor.setRuntime(&runtime);
				runtime.setCommandProcessor(&processor);
				connect(&runtime, &WorldRuntime::incomingStyledLineReceived, &processor,
				        &WorldCommandProcessor::onIncomingStyledLineReceived);
				view.setRuntime(&runtime);
				const auto detach = qScopeGuard(
				    [&]
				    {
					    view.setRuntime(nullptr);
					    runtime.setCommandProcessor(nullptr);
					    processor.setRuntime(nullptr);
				    });
				runtime.setStartupDirectory(QStringLiteral(QMUD_TEST_BUILD_DIR));
				runtime.setPluginsDirectory(directory.path());
				runtime.setWorldAttribute(QStringLiteral("native_gmcp"), QStringLiteral("1"));
				QString error;
				QVERIFY2(runtime.loadPluginFile(QStringLiteral("counters.xml"), &error), qPrintable(error));
				QTRY_VERIFY(!runtime.pluginForId(id)->installPending);
				QTcpServer server;
				QVERIFY(server.listen(QHostAddress::LocalHost, 0));
				QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
				QTRY_VERIFY(runtime.isConnected() && server.hasPendingConnections());
				auto *peer   = server.nextPendingConnection();
				auto *socket = runtime.findChild<WorldSocket *>();
				QVERIFY(peer && socket);
				QVERIFY(disconnect(socket, &WorldSocketService::rawDataReceived, &runtime,
				                   &WorldRuntime::receiveRawData));
				QList<QPair<QByteArray, WorldSocketInputMetadata>> batches;
				QByteArray                                         received;
				qint64                                             capturedBytes = 0;
				const auto                                         connection =
				    connect(socket, &WorldSocketService::rawDataReceived, this,
				            [&](const QByteArray &bytes, const WorldSocketInputMetadata &metadata)
				            {
					            batches.push_back({bytes, metadata});
					            received += bytes;
					            capturedBytes += metadata.receivedBytes;
				            });
				const auto       disconnectCapture = qScopeGuard([&] { disconnect(connection); });
				const QByteArray first             = QByteArray(20 * 1024, 'x') + "\nfirst\n";
				QVERIFY(queueNativeBatch(*socket, *peer, first));
				QTRY_COMPARE(received, first);
				peer->write("second\n");
				peer->flush();
				QTRY_COMPARE(received, first + "second\n");
				const auto beforeProtocolOnly = batches.size();
				const auto protocolOnly       = QMudGmcp::frame("Test.Accounting {\"value\":1}");
				peer->write(protocolOnly);
				peer->flush();
				QTRY_COMPARE(capturedBytes, first.size() + 3 + 7 + protocolOnly.size());
				QVERIFY(batches.size() > beforeProtocolOnly);
				QVERIFY(batches.constLast().first.isEmpty());
				qint64                 counted = 0;
				int                    reads   = 0;
				QByteArray             pending;
				QMap<QString, QString> expected;
				for (const auto &batch : batches)
				{
					counted += batch.second.receivedBytes;
					reads += batch.second.receivedBytes > 0;
					pending += batch.first;
					for (auto newline = pending.indexOf('\n'); newline >= 0; newline = pending.indexOf('\n'))
					{
						const auto line = QString::fromUtf8(pending.left(newline));
						expected.insert(line, QStringLiteral("%1,%2").arg(reads).arg(counted));
						pending.remove(0, newline + 1);
					}
					runtime.receiveRawData(batch.first, batch.second);
				}
				QCOMPARE(counted, first.size() + 3 + 7 + protocolOnly.size());
				QCOMPARE(runtime.bytesIn(), counted);
				QCOMPARE(runtime.inputPacketCount(), reads);
				QTRY_COMPARE(runtime.pluginVariableValue(id, QStringLiteral("first_counts")),
				             expected.value(QStringLiteral("first")));
				QTRY_COMPARE(runtime.pluginVariableValue(id, QStringLiteral("second_counts")),
				             expected.value(QStringLiteral("second")));
				runtime.setWorldFileModified(false);
			}

			/**
			 * @brief Covers both native MCCP versions with delayed runtime input delivery.
			 */
			static void compressionStatisticsFollowInput_data()
			{
				QTest::addColumn<int>("version");
				QTest::newRow("mccp1") << 1;
				QTest::newRow("mccp2") << 2;
			}

			/**
			 * @brief Keeps compression statistics with split and queued input after the worker has processed
			 * later data.
			 */
			void compressionStatisticsFollowInput()
			{
				QFETCH(int, version);
				QTemporaryDir directory(QDir(QStringLiteral(QMUD_TEST_BUILD_DIR))
				                            .filePath(QStringLiteral("worlds/gmcp-compression-XXXXXX")));
				QVERIFY(directory.isValid());
				const QString id = QStringLiteral("151515151515151515151515");
				QVERIFY(
				    writeGmcpPlugin(directory.path(), QStringLiteral("compression"), id, QStringLiteral(R"lua(
function OnPluginLineReceived(line)
 if line=='first' or line=='second' or line=='simulation' then
  SetVariable(line..'_stats',string.format('%d,%d,%d,%s',GetInfo(206),GetInfo(207),GetInfo(208),tostring(GetInfo(103))))
 end
 return true
end
)lua")));
				WorldRuntime          runtime;
				WorldView             view;
				WorldCommandProcessor processor;
				processor.setView(&view);
				processor.setRuntime(&runtime);
				runtime.setCommandProcessor(&processor);
				connect(&runtime, &WorldRuntime::incomingStyledLineReceived, &processor,
				        &WorldCommandProcessor::onIncomingStyledLineReceived);
				connect(&runtime, &WorldRuntime::incomingStyledLinePartialReceived, &processor,
				        &WorldCommandProcessor::onIncomingStyledLinePartialReceived);
				view.setRuntime(&runtime);
				const auto detach = qScopeGuard(
				    [&]
				    {
					    view.setRuntime(nullptr);
					    runtime.setCommandProcessor(nullptr);
					    processor.setRuntime(nullptr);
				    });
				view.show();
				QVERIFY(QTest::qWaitForWindowExposed(&view));
				runtime.setStartupDirectory(QStringLiteral(QMUD_TEST_BUILD_DIR));
				runtime.setPluginsDirectory(directory.path());
				runtime.setWorldAttribute(QStringLiteral("native_gmcp"), QStringLiteral("1"));
				QString error;
				QVERIFY2(runtime.loadPluginFile(QStringLiteral("compression.xml"), &error),
				         qPrintable(error));
				QTRY_VERIFY(!runtime.pluginForId(id)->installPending);
				QTcpServer server;
				QVERIFY(server.listen(QHostAddress::LocalHost, 0));
				QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
				QTRY_VERIFY(runtime.isConnected() && server.hasPendingConnections());
				auto *peer   = server.nextPendingConnection();
				auto *socket = runtime.findChild<WorldSocket *>();
				QVERIFY(peer && socket);
				// Hold admission to the runtime while retaining the real transport and facade delivery.
				QVERIFY(disconnect(socket, &WorldSocketService::rawDataReceived, &runtime,
				                   &WorldRuntime::receiveRawData));
				QList<QPair<QByteArray, WorldSocketInputMetadata>> batches;
				QByteArray                                         received;
				const auto                                         connection =
				    connect(socket, &WorldSocketService::rawDataReceived, this,
				            [&](const QByteArray &bytes, const WorldSocketInputMetadata &metadata)
				            {
					            received += bytes;
					            batches.push_back({bytes, metadata});
				            });
				const auto disconnectCapture = qScopeGuard([&] { disconnect(connection); });
				z_stream   encoder{};
				QCOMPARE(deflateInit(&encoder, Z_DEFAULT_COMPRESSION), Z_OK);
				const auto endEncoder = qScopeGuard([&] { deflateEnd(&encoder); });
				auto       encode     = [&](const QByteArray &plain, int flush)
				{
					encoder.next_in  = reinterpret_cast<Bytef *>(const_cast<char *>(plain.constData()));
					encoder.avail_in = static_cast<uInt>(plain.size());
					QByteArray             encoded;
					std::array<char, 1024> buffer{};
					do
					{
						encoder.next_out  = reinterpret_cast<Bytef *>(buffer.data());
						encoder.avail_out = static_cast<uInt>(buffer.size());
						const int result  = deflate(&encoder, flush);
						if (result != Z_OK && result != Z_STREAM_END)
							return QByteArray{};
						encoded.append(buffer.data(),
						               static_cast<qsizetype>(buffer.size() - encoder.avail_out));
					} while (encoder.avail_in != 0 || encoder.avail_out == 0);
					return encoded;
				};
				const QByteArray first        = QByteArray(20 * 1024, 'x') + "\nfirst\n";
				const auto       firstEncoded = encode(first, Z_SYNC_FLUSH);
				QVERIFY(!firstEncoded.isEmpty());
				const auto marker =
				    QByteArray::fromHex(version == 1 ? "fffb55fffa55fbf0" : "fffb56fffa56fff0");
				const auto input = QByteArray::fromHex("fffbc9") + marker + firstEncoded;
				QCOMPARE(peer->write(input), input.size());
				peer->flush();
				QTRY_COMPARE(received, first);
				QVERIFY(batches.size() >= 2);
				const auto firstStats = batches.constLast().second;
				QVERIFY(firstStats.transportDecoded);
				QCOMPARE(firstStats.mccpType, version);
				QCOMPARE(firstStats.totalCompressedBytes, firstEncoded.size());
				QCOMPARE(firstStats.totalUncompressedBytes, first.size());
				const QByteArray second("second\n");
				const auto       secondEncoded = encode(second, Z_FINISH);
				QVERIFY(!secondEncoded.isEmpty());
				QCOMPARE(peer->write(secondEncoded), secondEncoded.size());
				peer->flush();
				QTRY_COMPARE(received, first + second);
				const auto secondStats = batches.constLast().second;
				QCOMPARE(secondStats.mccpType, 0);
				QCOMPARE(secondStats.totalCompressedBytes, firstEncoded.size() + secondEncoded.size());
				QCOMPARE(secondStats.totalUncompressedBytes, first.size() + second.size());
				QVERIFY(!runtime.isCompressing());
				for (const auto &batch : batches)
					runtime.receiveRawData(batch.first, batch.second);
				auto expected = [](const WorldSocketInputMetadata &metadata)
				{
					return QStringLiteral("%1,%2,%3,%4")
					    .arg(metadata.totalUncompressedBytes)
					    .arg(metadata.totalCompressedBytes)
					    .arg(metadata.mccpType)
					    .arg(metadata.mccpType ? QStringLiteral("true") : QStringLiteral("false"));
				};
				QTRY_COMPARE(runtime.pluginVariableValue(id, QStringLiteral("first_stats")),
				             expected(firstStats));
				QTRY_COMPARE(runtime.pluginVariableValue(id, QStringLiteral("second_stats")),
				             expected(secondStats));
				runtime.simulateInput("simulation\n");
				QTRY_COMPARE(runtime.pluginVariableValue(id, QStringLiteral("simulation_stats")),
				             expected(secondStats));
				runtime.freezeForReload();
				QTRY_VERIFY(runtime.reloadDrained());
				QVERIFY(runtime.flushForReload());
				QVERIFY(!runtime.saveNativeGmcpState().isEmpty());
				runtime.resumeTransportAfterReload();
				runtime.setWorldFileModified(false);
			}

			/**
			 * @brief Keeps world-script APIs usable without subscribing its functions to plugin
			 * notifications.
			 */
			static void worldScriptDoesNotReceiveGmcpCallbacks()
			{
				GmcpWireFixture session;
				session.write = [](const QByteArray &bytes) -> qint64 { return bytes.size(); };
				session.reset(true, 1, false);
				session.receive(QByteArray::fromHex("fffbc9"));
				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("native_gmcp"), QStringLiteral("1"));
				runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("1"));
				WorldRuntimeTestAccess::setGmcpPublication(runtime, session.publication());
				QVERIFY(run(runtime, QStringLiteral(R"lua(
function GmcpTestBarrier() end
function onPluginGmcpMessage() SetVariable('unexpected_message','yes') end
function OnPluginGmcpStateChanged() SetVariable('unexpected_state','yes') end
)lua")));
				session.observed = [&](const auto &record, const auto &message)
				{ WorldRuntimeTestAccess::deliverGmcpRecord(runtime, record, message); };
				const auto captures = WorldRuntimeTestAccess::callbackSnapshotCaptureCount(runtime);
				runtime.findChild<WorldSocketService *>()->gmcpStateChanged(true);
				session.receive(QMudGmcp::frame("Test.State {\"value\":17}"));
				QCOMPARE(WorldRuntimeTestAccess::callbackSnapshotCaptureCount(runtime), captures);
				gmcpBarrier(runtime);
				QVERIFY(run(runtime, QStringLiteral(R"lua(
assert(GetVariable('unexpected_message')==nil and GetVariable('unexpected_state')==nil)
assert(gmcpIsEnabled() and gmcp.Test.State.value==17)
assert(gmcpGet('Test.State').value==17 and gmcpGetRaw('Test.State')=='Test.State {"value":17}')
assert(#gmcpGetMessages()==1)
assert(type(gmcpSend)=='function' and type(gmcpRegisterModule)=='function' and type(gmcpUnregisterModule)=='function')
)lua")));
				runtime.setWorldFileModified(false);
			}

			/**
			 * @brief Drains incoming plugin mutations before the production reload path persists plugin
			 * state.
			 */
			static void reloadPersistsAfterDrain()
			{
				QTemporaryDir directory(QDir(QStringLiteral(QMUD_TEST_BUILD_DIR))
				                            .filePath(QStringLiteral("worlds/gmcp-drain-save-XXXXXX")));
				QVERIFY(directory.isValid());
				const QString id = QStringLiteral("101010101010101010101010");
				QVERIFY(writeGmcpPlugin(directory.path(), QStringLiteral("persist"), id, QStringLiteral(R"lua(
function OnPluginPacketReceived(packet)
 SetVariable('input_drained','yes')
 return packet
end
function OnPluginSaveState()
 assert(GetVariable('input_drained')=='yes')
 SetVariable('saved_after_drain','yes')
end
)lua")));
				QFile fixture(QDir(directory.path()).filePath(QStringLiteral("persist.xml")));
				QVERIFY(fixture.open(QIODevice::ReadOnly));
				auto xml = fixture.readAll();
				fixture.close();
				xml.replace("save_state=\"n\"", "save_state=\"y\"");
				QVERIFY(fixture.open(QIODevice::WriteOnly | QIODevice::Truncate));
				QCOMPARE(fixture.write(xml), xml.size());
				fixture.close();
				AppController controller;
				MainWindow    window;
				controller.setMainWindow(&window);
				AppControllerTestAccess::setHome(controller, directory.path());
				controller.setGlobalOptionInt(QStringLiteral("EnableReloadFeature"), 1);
				WorldRuntime runtime;
				auto        *child = new WorldChildWindow(QStringLiteral("Synthetic drain save"));
				child->setRuntime(&runtime);
				const auto detachChild = qScopeGuard([&] { child->setRuntime(nullptr); });
				window.addMdiSubWindow(child, true);
				window.show();
				QVERIFY(QTest::qWaitForWindowExposed(&window));
				runtime.setStartupDirectory(QStringLiteral(QMUD_TEST_BUILD_DIR));
				runtime.setPluginsDirectory(directory.path());
				runtime.setStateFilesDirectory(directory.path());
				runtime.setWorldAttribute(QStringLiteral("id"), QStringLiteral("111111111111111111111111"));
				runtime.setWorldAttribute(QStringLiteral("native_gmcp"), QStringLiteral("1"));
				runtime.setWorldAttribute(QStringLiteral("save_world_automatically"), QStringLiteral("0"));
				QString error;
				QVERIFY2(runtime.loadPluginFile(QStringLiteral("persist.xml"), &error), qPrintable(error));
				QTRY_VERIFY(!runtime.pluginForId(id)->installPending);
				QTcpServer server;
				QVERIFY(server.listen(QHostAddress::LocalHost, 0));
				QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
				QTRY_VERIFY(runtime.isConnected() && server.hasPendingConnections());
				auto *peer   = server.nextPendingConnection();
				auto *socket = runtime.findChild<WorldSocket *>();
				QVERIFY(socket);
				static_cast<void>(runtime.deletePluginVariable(id, QStringLiteral("input_drained")));
				QVERIFY(queueNativeBatch(*socket, *peer, QByteArray("synthetic input\n")));
				QVERIFY(runtime.pluginVariableValue(id, QStringLiteral("input_drained")).isEmpty());
				const auto oldDryRun          = qgetenv("QMUD_RELOAD_DRY_RUN");
				const auto restoreEnvironment = qScopeGuard(
				    [&]
				    {
					    if (oldDryRun.isNull())
						    qunsetenv("QMUD_RELOAD_DRY_RUN");
					    else
						    qputenv("QMUD_RELOAD_DRY_RUN", oldDryRun);
				    });
				qputenv("QMUD_RELOAD_DRY_RUN", "1");
				AppControllerTestAccess::reload(controller);
				QCOMPARE(runtime.pluginVariableValue(id, QStringLiteral("saved_after_drain")),
				         QStringLiteral("yes"));
				QFile stateFile(QDir(directory.path())
				                    .filePath(QStringLiteral("111111111111111111111111-") + id +
				                              QStringLiteral("-state.xml")));
				QVERIFY(stateFile.open(QIODevice::ReadOnly));
				QVERIFY(stateFile.readAll().contains("saved_after_drain"));
				stateFile.close();
				WorldRuntime restored;
				restored.setStartupDirectory(QStringLiteral(QMUD_TEST_BUILD_DIR));
				restored.setPluginsDirectory(directory.path());
				restored.setStateFilesDirectory(directory.path());
				restored.setWorldAttribute(QStringLiteral("id"), QStringLiteral("111111111111111111111111"));
				QVERIFY2(restored.loadPluginFile(QStringLiteral("persist.xml"), &error), qPrintable(error));
				QCOMPARE(restored.pluginVariableValue(id, QStringLiteral("saved_after_drain")),
				         QStringLiteral("yes"));
				restored.setWorldFileModified(false);
				QVERIFY(!runtime.reloadPreparing());
				QVERIFY(runtime.isConnected());
				runtime.setWorldFileModified(false);
			}

			/**
			 * @brief Completes input callbacks and their causal output while reception remains frozen.
			 */
			static void frozenRuntimeCompletesCallbacks()
			{
				QTemporaryDir directory(QDir(QStringLiteral(QMUD_TEST_BUILD_DIR))
				                            .filePath(QStringLiteral("worlds/gmcp-drain-callback-XXXXXX")));
				QVERIFY(directory.isValid());
				const QString id = QStringLiteral("121212121212121212121212");
				AppController controller;
				MainWindow    window;
				controller.setMainWindow(&window);
				WorldRuntime runtime;
				WorldView    view;
				view.setRuntime(&runtime);
				const auto detach = qScopeGuard([&] { view.setRuntime(nullptr); });
				view.show();
				QVERIFY(QTest::qWaitForWindowExposed(&view));
				runtime.setWorldAttribute(QStringLiteral("native_gmcp"), QStringLiteral("1"));
				runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
				QString error;
				QVERIFY(writeGmcpPlugin(directory.path(), QStringLiteral("drain"), id, QStringLiteral(R"lua(
function onPluginGmcpMessage(name,payload)
 SetVariable('drained_value',tostring(payload.value))
 assert(gmcpSend('Test.Reply','{}')==0)
end
)lua")));
				runtime.setStartupDirectory(QStringLiteral(QMUD_TEST_BUILD_DIR));
				runtime.setPluginsDirectory(directory.path());
				QVERIFY2(runtime.loadPluginFile(QStringLiteral("drain.xml"), &error), qPrintable(error));
				QTRY_VERIFY(!runtime.pluginForId(id)->installPending);
				QTcpServer server;
				QVERIFY(server.listen(QHostAddress::LocalHost, 0));
				QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
				QTRY_VERIFY(runtime.isConnected() && server.hasPendingConnections());
				auto *peer   = server.nextPendingConnection();
				auto *socket = runtime.findChild<WorldSocket *>();
				QVERIFY(socket);
				QByteArray text(10 * 16 * 1024, 'q');
				text += '\n';
				QVERIFY(queueNativeBatch(*socket, *peer,
				                         QByteArray::fromHex("fffbc9") +
				                             QMudGmcp::frame("Test.State {\"value\":23}") + text));
				runtime.freezeForReload();
				QTRY_VERIFY(runtime.reloadDrained());
				QCOMPARE(runtime.pluginVariableValue(id, QStringLiteral("drained_value")),
				         QStringLiteral("23"));
				QVERIFY(runtime.flushForReload());
				const auto saved = runtime.saveNativeGmcpState();
				QVERIFY(!saved.isEmpty());
				QVERIFY(!saved.contains(QByteArray(1024, 'q')));
				GmcpWireFixture restored;
				QVERIFY(restored.restoreConnection(saved, 2));
				QCOMPARE(restored.publication()
				             ->current.load()
				             ->messages.value(QStringLiteral("test.state"))
				             ->state.toObject()
				             .value(QStringLiteral("value"))
				             .toInt(),
				         23);
				runtime.resumeTransportAfterReload();
				QVERIFY(!runtime.reloadPreparing());
				runtime.setWorldFileModified(false);
			}

			/**
			 * @brief Releases snapshot references when Lua argument validation fails before allocating C++
			 * resources.
			 */
			static void argumentErrorsReleaseSnapshots()
			{
				GmcpWireFixture session;
				session.write = [](const QByteArray &bytes) -> qint64 { return bytes.size(); };
				session.reset(true, 1, false);
				session.receive(QByteArray::fromHex("fffbc9"));
				session.receive(QMudGmcp::frame("Test.State {\"value\":1}"));
				std::weak_ptr<const QMudGmcp::Snapshot> old = session.publication()->current.load();
				{
					WorldRuntime runtime;
					runtime.setWorldAttribute(QStringLiteral("native_gmcp"), QStringLiteral("1"));
					WorldRuntimeTestAccess::setGmcpPublication(runtime, session.publication());
					const auto references = old.use_count();
					QString    error;
					QVERIFY2(run(runtime, QStringLiteral(R"lua(
local calls={
 function() gmcpGet({}) end, function() gmcpGetRaw({}) end,
 function() gmcpSend({}) end, function() gmcpSend('Test.State',{}) end,
 function() gmcpRegisterModule('Test.Feature',{}) end,
 function() gmcpGet() end, function() gmcpGetRaw(false) end,
 function() gmcpGet(gmcp) end, function() gmcpGet('Test.State',false) end,
 function() gmcpGet('Test.State',{[1]='value',[3]='missing'}) end,
 function() gmcpGet('Test.State',{[0]='value'}) end,
 function() gmcpGet('Test.State',{'value',false}) end,
 function() gmcpGet('Test.State',{1.5}) end,
 function() gmcpGetString({}) end, function() gmcpGetString('Test.State',{}, {}) end,
 function() gmcpGetString('Test.State',{false}) end,
 function() gmcpToTable({}) end, function() gmcpToTable(gmcp) end,
 function() gmcpToTable(io.stdout) end, function() gmcpToTable(nil,'true') end,
 function() gmcpType() end,
 function() gmcpSend(false) end, function() gmcpSend('Test.State',false) end
}
for i=1,100 do for _,call in ipairs(calls) do assert(not pcall(call)) end end
collectgarbage('collect')
assert(gmcpGet('Test.State').value==1 and gmcpRegisterModule('Test.Feature',2)==0)
)lua"),
					             &error),
					         qPrintable(error));
					QCOMPARE(old.use_count(), references);
					session.receive(QMudGmcp::frame("Test.State {\"value\":2}"));
					QVERIFY(old.expired());
					runtime.setWorldFileModified(false);
				}
				QVERIFY(old.expired());
			}

#ifdef Q_OS_UNIX
			/**
			 * @brief Covers preserved native, unnegotiated native, and legacy ownership against opposite
			 * preferences.
			 */
			static void recoveryUsesSavedMode_data()
			{
				QTest::addColumn<bool>("native");
				QTest::addColumn<bool>("negotiated");
				QTest::addColumn<QString>("pathForm");
				for (const auto &form :
				     {QStringLiteral("portable"), QStringLiteral("native"), QStringLiteral("windows")})
				{
					QTest::newRow(qPrintable(QStringLiteral("native-preserved-") + form))
					    << true << true << form;
					QTest::newRow(qPrintable(QStringLiteral("native-before-negotiation-") + form))
					    << true << false << form;
					QTest::newRow(qPrintable(QStringLiteral("legacy-preserved-") + form))
					    << false << false << form;
				}
			}
			/**
			 * @brief Runs real recovered-world opening and plugin startup before descriptor adoption.
			 */
			void recoveryUsesSavedMode()
			{
				QFETCH(bool, native);
				QFETCH(bool, negotiated);
				QFETCH(QString, pathForm);
				QTemporaryDir directory(QDir(QStringLiteral(QMUD_TEST_BUILD_DIR))
				                            .filePath(QStringLiteral("worlds/gmcp-recovery-XXXXXX")));
				QVERIFY(directory.isValid());
				const QString executionDirectory = QDir::currentPath();
				QVERIFY(executionDirectory != QStringLiteral(QMUD_TEST_BUILD_DIR));
				QTcpServer server;
				QVERIFY(server.listen(QHostAddress::LocalHost, 0));
				WorldSocket                   original;
				WorldSocketConnectionSettings settings;
				settings.nativeGmcp = native;
				original.applyConnectionSettings(settings);
				QVERIFY(original.connectToHost(QStringLiteral("127.0.0.1"), server.serverPort()));
				QTRY_VERIFY(original.isConnected() && server.hasPendingConnections());
				auto *peer = server.nextPendingConnection();
				if (negotiated)
				{
					peer->write(QByteArray::fromHex("fffbc9") + QMudGmcp::frame("Test.State {\"value\":11}"));
					peer->flush();
					QTRY_VERIFY(original.gmcpPublication()->current.load()->messages.contains(
					    QStringLiteral("test.state")));
				}
				original.freezeForReload();
				QTRY_VERIFY(original.reloadInputDrained());
				QVERIFY(original.flushForReload());
				const auto saved = native ? original.saveNativeState() : QByteArray();
				QVERIFY(!native || !saved.isEmpty());
				int descriptor = ::dup(static_cast<int>(original.nativeSocketDescriptor()));
				QVERIFY(descriptor >= 0);
				const auto closeDescriptor = qScopeGuard(
				    [&]
				    {
					    if (descriptor >= 0)
						    ::close(descriptor);
				    });
				const QString pluginId = QStringLiteral("080808080808080808080808");
				QVERIFY(writeGmcpPlugin(directory.path(), QStringLiteral("recovered"), pluginId,
				                        QStringLiteral(R"lua(
function OnPluginInstall()
 SetVariable('startup_mode',gmcp and 'native' or 'legacy')
 if gmcp then
  SetVariable('startup_negotiated',tostring(gmcpIsEnabled()))
  local data=gmcpGet('Test.State')
  SetVariable('startup_value',data and tostring(data.value) or 'absent')
  assert(gmcpRegisterModule('Test.Feature',7)==0)
  SetVariable('startup_send',tostring(gmcpSend('Test.BeforeAdoption','{}')))
 end
end
)lua")));
				const auto worldPath = QDir(directory.path()).filePath(QStringLiteral("recovered.qdl"));
				QFile      world(worldPath);
				QVERIFY(world.open(QIODevice::WriteOnly));
				const QString portableWorldPath =
				    QDir(QStringLiteral(QMUD_TEST_BUILD_DIR)).relativeFilePath(worldPath);
				world.write(
				    QStringLiteral("<qmud><world id=\"090909090909090909090909\" name=\"Synthetic Recovery\" "
				                   "native_gmcp=\"%1\"/><include name=\"fragment.xml\"/>"
				                   "<include name=\"recovered.xml\" plugin=\"y\"/></qmud>")
				        .arg(native ? 0 : 1)
				        .toUtf8());
				world.close();
				QFile fragment(QDir(directory.path()).filePath(QStringLiteral("fragment.xml")));
				QVERIFY(fragment.open(QIODevice::WriteOnly));
				fragment.write("<qmud><variables><variable name=\"recovery_include\">fixture</variable>"
				               "</variables></qmud>");
				fragment.close();
				AppController controller;
				MainWindow    window;
				controller.setMainWindow(&window);
				AppControllerTestAccess::setHome(controller, directory.path());
				controller.setGlobalOptionInt(QStringLiteral("ConfirmBeforeClosingWorld"), 0);
				controller.setGlobalOptionInt(QStringLiteral("ConfirmBeforeSavingVariables"), 0);
				ReloadWorldState state;
				state.connectedAtReload = true;
				state.socketDescriptor  = descriptor;
				state.nativeGmcpState   = saved;
				state.worldFilePath     = pathForm == QLatin1String("native") ? worldPath : portableWorldPath;
				if (pathForm == QLatin1String("windows"))
					state.worldFilePath.replace(QLatin1Char('/'), QLatin1Char('\\'));
				WorldRuntime *runtime = nullptr;
				QVERIFY(AppControllerTestAccess::openRecovery(controller, state, &runtime));
				QVERIFY(runtime);
				QVERIFY(runtime->pluginForId(pluginId));
				QCOMPARE(runtime->startupDirectory(), QStringLiteral(QMUD_TEST_BUILD_DIR) + QLatin1Char('/'));
				QCOMPARE(runtime->worldFilePath(), portableWorldPath);
				const auto portableFixtureDirectory =
				    QDir(QStringLiteral(QMUD_TEST_BUILD_DIR)).relativeFilePath(directory.path());
				QCOMPARE(runtime->pluginForId(pluginId)->source,
				         portableFixtureDirectory + QStringLiteral("/recovered.xml"));
				QCOMPARE(runtime->pluginForId(pluginId)->directory,
				         portableFixtureDirectory + QLatin1Char('/'));
				QString includedValue;
				QVERIFY(runtime->findVariable(QStringLiteral("recovery_include"), includedValue));
				QCOMPARE(includedValue, QStringLiteral("fixture"));
				QCOMPARE(QDir::currentPath(), executionDirectory);
				QVERIFY(runtime->gmcpPublication()->current.load()->restoring);
				QCOMPARE(runtime->gmcpPublication()->current.load()->native, native);
				QCOMPARE(runtime->nativeGmcpAvailable(), native);
				bool installed = false;
				AppControllerTestAccess::startup(controller, runtime, [&] { installed = true; });
				QTRY_VERIFY(installed);
				QCOMPARE(runtime->pluginVariableValue(pluginId, QStringLiteral("startup_mode")),
				         native ? QStringLiteral("native") : QStringLiteral("legacy"));
				if (native)
				{
					QCOMPARE(runtime->pluginVariableValue(pluginId, QStringLiteral("startup_negotiated")),
					         negotiated ? QStringLiteral("true") : QStringLiteral("false"));
					QCOMPARE(runtime->pluginVariableValue(pluginId, QStringLiteral("startup_value")),
					         negotiated ? QStringLiteral("11") : QStringLiteral("absent"));
					QVERIFY(runtime->pluginVariableValue(pluginId, QStringLiteral("startup_send")) !=
					        QStringLiteral("0"));
				}
				QByteArray received = peer->readAll();
				received.clear();
				connect(peer, &QTcpSocket::readyRead, this, [&] { received += peer->readAll(); });
				QVERIFY(runtime->prepareNativeGmcpRestore(saved));
				QString error;
				QVERIFY2(runtime->adoptConnectedSocketDescriptor(descriptor, false, false, {}, {}, &error),
				         qPrintable(error));
				descriptor = -1;
				QVERIFY(!runtime->gmcpPublication()->current.load()->restoring);
				QCOMPARE(runtime->nativeGmcpConfigured(), !native);
				QCOMPARE(runtime->nativeGmcpAvailable(), native);
				if (native)
				{
					if (!negotiated)
					{
						peer->write(QByteArray::fromHex("fffbc9"));
						peer->flush();
					}
					QTRY_VERIFY(runtime->gmcpPublication()->current.load()->negotiated);
					QVERIFY2(
					    run(*runtime, QStringLiteral("assert(gmcpSend('Test.Barrier','{}')==0)"), &error),
					    qPrintable(error));
					QTRY_VERIFY(received.contains("Test.Barrier"));
					QVERIFY(received.contains("test.feature 7"));
					if (negotiated)
						QVERIFY(!received.contains("Core.Hello"));
					QCOMPARE(WorldRuntimeTestAccess::gmcpModuleVersion(*runtime, pluginId,
					                                                   QStringLiteral("Test.Feature")),
					         7);
				}
				runtime->setWorldFileModified(false);
			}
#endif

			/**
			 * @brief Keeps legacy globals untouched, rejects unavailable APIs, and follows connection-latched
			 * native mode across reconnects.
			 */
			static void luaNativeAvailability()
			{
				WorldRuntime runtime;
				QString      error;
				QVERIFY2(run(runtime, QStringLiteral(R"lua(
assert(gmcp==nil and gmcpNull==nil)
legacyGmcp={Test={value=1}}; gmcp=legacyGmcp
legacyNull={}; gmcpNull=legacyNull
function assertGmcpUnavailable()
 local calls={
  function() return gmcpSend('Test.State','{}') end,
  function() return gmcpGet('Test.State') end,
  function() return gmcpGetString('Test.State','value') end,
  function() return gmcpToTable(nil,true) end,
  function() return gmcpType(nil) end,
  function() return gmcpGetRaw('Test.State') end,
  function() return gmcpGetMessages() end,
  function() return gmcpIsEnabled() end,
  function() return gmcpRegisterModule('Test.Unavailable',1) end,
  function() return gmcpUnregisterModule('Test.Unavailable') end,
  function() return world.gmcpSend('Test.State','{}') end,
  function() return world.gmcpGet('Test.State') end,
  function() return world.gmcpGetString('Test.State','value') end,
  function() return world.gmcpToTable(nil,true) end,
  function() return world.gmcpType(nil) end,
  function() return world.gmcpGetRaw('Test.State') end,
  function() return world.gmcpGetMessages() end,
  function() return world.gmcpIsEnabled() end,
  function() return world.gmcpRegisterModule('Test.Unavailable',1) end,
  function() return world.gmcpUnregisterModule('Test.Unavailable') end
 }
 for _,call in ipairs(calls) do
  local ok,err=pcall(call)
  assert(not ok and tostring(err):find('GMCP is not available',1,true))
 end
 assert(gmcp==legacyGmcp and gmcpNull==legacyNull)
end
assertGmcpUnavailable()
assert(SetOption('native_gmcp',1)==0 and GetOption('native_gmcp')==1)
assertGmcpUnavailable()
assert(SetOption('native_gmcp',0)==0)
assertGmcpUnavailable()
assert(SetOption('native_gmcp',1)==0)
)lua"),
				             &error),
				         qPrintable(error));
				QVERIFY(!runtime.nativeGmcpAvailable());
				QCOMPARE(WorldRuntimeTestAccess::gmcpModuleVersion(runtime, {},
				                                                   QStringLiteral("Test.Unavailable")),
				         0);
				QTcpServer server;
				QVERIFY(server.listen(QHostAddress::LocalHost, 0));
				QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
				QVERIFY(runtime.nativeGmcpAvailable());
				QTRY_VERIFY(runtime.isConnected() && server.hasPendingConnections());
				server.nextPendingConnection();
				QVERIFY2(run(runtime, QStringLiteral(R"lua(
assert(type(gmcp)=='userdata' and type(gmcpNull)=='userdata')
nativeGmcp=gmcp; nativeNull=gmcpNull
assert(not gmcpIsEnabled() and gmcpGet('Test.State')==nil and gmcpGetRaw('Test.State')==nil)
assert(#gmcpGetMessages()==0 and gmcpSend('Test.State','{}')~=0)
assert(gmcpRegisterModule('Test.BeforeNegotiation',2)==0)
assert(SetOption('native_gmcp',0)==0)
assert(gmcp==nativeGmcp and not gmcpIsEnabled())
)lua"),
				             &error),
				         qPrintable(error));
				runtime.closeSocketForReloadReconnect();
				QVERIFY(!runtime.nativeGmcpAvailable());
				QVERIFY2(run(runtime, QStringLiteral("assertGmcpUnavailable()"), &error), qPrintable(error));
				QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
				QTRY_VERIFY(runtime.isConnected() && server.hasPendingConnections());
				server.nextPendingConnection();
				QVERIFY2(run(runtime,
				             QStringLiteral("assert(SetOption('native_gmcp',1)==0); assertGmcpUnavailable()"),
				             &error),
				         qPrintable(error));
				runtime.closeSocketForReloadReconnect();
				QVERIFY2(run(runtime, QStringLiteral("assertGmcpUnavailable()"), &error), qPrintable(error));
				QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
				QTRY_VERIFY(runtime.isConnected() && server.hasPendingConnections());
				auto *peer = server.nextPendingConnection();
				peer->write(QByteArray::fromHex("fffbc9") + QMudGmcp::frame("Test.State {\"value\":3}"));
				peer->flush();
				QTRY_VERIFY(runtime.gmcpPublication()->current.load()->messages.contains(
				    QStringLiteral("test.state")));
				QVERIFY2(run(runtime,
				             QStringLiteral("assert(gmcp==nativeGmcp and gmcpNull==nativeNull and "
				                            "gmcpIsEnabled() and gmcpGet('Test.State').value==3)"),
				             &error),
				         qPrintable(error));
				runtime.closeSocketForReloadReconnect();
				QVERIFY2(
				    run(runtime,
				        QStringLiteral("assertGmcpUnavailable(); replacementGmcp={}; replacementNull={}; "
				                       "gmcp=replacementGmcp; gmcpNull=replacementNull"),
				        &error),
				    qPrintable(error));
				QTcpServer refused;
				QVERIFY(refused.listen(QHostAddress::LocalHost, 0));
				const auto port = refused.serverPort();
				refused.close();
				QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), port));
				QTRY_VERIFY(!runtime.isConnected() && !runtime.isConnecting());
				QVERIFY2(run(runtime,
				             QStringLiteral("assert(gmcp==replacementGmcp and gmcpNull==replacementNull); "
				                            "local ok,err=pcall(gmcpIsEnabled); assert(not ok and "
				                            "err:find('GMCP is not available',1,true))"),
				             &error),
				         qPrintable(error));
				runtime.setWorldFileModified(false);
			}

			/**
			 * @brief Exercises GMCP, raw, and command output with and without packet diagnostics.
			 */
			static void automaticWriteAccounting_data()
			{
				QTest::addColumn<QString>("sendScript");
				QTest::addColumn<QByteArray>("applicationPacket");
				QTest::addColumn<bool>("command");
				QTest::addColumn<bool>("debug");
				for (const bool debug : {false, true})
				{
					const QByteArray suffix = debug ? "-debug" : "-counts";
					QTest::newRow(("gmcp" + suffix).constData())
					    << QStringLiteral("assert(gmcpSend('Test.Request')==0)")
					    << QMudGmcp::frame("Test.Request") << false << debug;
					QTest::newRow(("raw" + suffix).constData())
					    << QStringLiteral("assert(SendPkt('synthetic-output')==0)")
					    << QByteArrayLiteral("synthetic-output") << false << debug;
					QTest::newRow(("command" + suffix).constData())
					    << QStringLiteral("assert(Send('synthetic-output')==0)")
					    << QByteArrayLiteral("synthetic-output\r\n") << true << debug;
				}
			}
			/**
			 * @brief Keeps automatic advertisements and subsequent Lua sends in physical accounting order.
			 */
			void automaticWriteAccounting()
			{
				QFETCH(QString, sendScript);
				QFETCH(QByteArray, applicationPacket);
				QFETCH(bool, command);
				QFETCH(bool, debug);
				QTemporaryDir directory(QDir(QStringLiteral(QMUD_TEST_BUILD_DIR))
				                            .filePath(QStringLiteral("worlds/write-accounting-XXXXXX")));
				QVERIFY(directory.isValid());
				const QString id = QStringLiteral("282828282828282828282828");
				QVERIFY(writeGmcpPlugin(directory.path(), QStringLiteral("accounting"), id,
				                        QStringLiteral(R"lua(
function onPluginGmcpMessage(name)
 if name~='Test.Begin' then return end
 assert(gmcpRegisterModule('Test.Feature',1)==0)
 %1
end
function OnPluginSent()
 SetVariable('sent_counts',string.format('%d:%d',GetInfo(205),GetInfo(217)))
end
function OnPluginPacketDebug(text)
 if text:find('Sent  packet:',1,true) then
  SetVariable('headers',(GetVariable('headers') or '')..string.format('%d:%d;',GetInfo(205),GetInfo(217)))
 elseif text:find('Core.Supports',1,true) then
  SetVariable('order',(GetVariable('order') or '')..'supports;')
 elseif text:find('Test.Request',1,true) or text:find('synthetic-output',1,true) then
  SetVariable('order',(GetVariable('order') or '')..'application;')
 end
 return true
end
)lua")
				                            .arg(sendScript)));
				WorldRuntime          runtime;
				WorldView             view;
				WorldCommandProcessor processor;
				processor.setView(&view);
				processor.setRuntime(&runtime);
				runtime.setCommandProcessor(&processor);
				view.setRuntime(&runtime);
				const auto detach = qScopeGuard(
				    [&]
				    {
					    view.setRuntime(nullptr);
					    runtime.setCommandProcessor(nullptr);
					    processor.setRuntime(nullptr);
				    });
				runtime.setStartupDirectory(QStringLiteral(QMUD_TEST_BUILD_DIR));
				runtime.setPluginsDirectory(directory.path());
				runtime.setWorldAttribute(QStringLiteral("native_gmcp"), QStringLiteral("1"));
				QString error;
				QVERIFY2(runtime.loadPluginFile(QStringLiteral("accounting.xml"), &error), qPrintable(error));
				QTRY_VERIFY(!runtime.pluginForId(id)->installPending);
				QTcpServer server;
				QVERIFY(server.listen(QHostAddress::LocalHost, 0));
				QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
				QTRY_VERIFY(runtime.isConnected() && server.hasPendingConnections());
				auto      *peer = server.nextPendingConnection();
				QByteArray received;
				connect(peer, &QTcpSocket::readyRead, this, [&] { received += peer->readAll(); });
				peer->write(QByteArray::fromHex("fffbc9"));
				peer->flush();
				QTRY_VERIFY(received.contains("Core.Supports.Set"));
				QTRY_COMPARE(runtime.bytesOut(), received.size());
				const int    packetsBefore = runtime.outputPacketCount();
				const qint64 bytesBefore   = runtime.bytesOut();
				received.clear();
				runtime.setDebugIncomingPackets(debug);
				peer->write(QMudGmcp::frame("Test.Begin {}"));
				peer->flush();
				const QByteArray supports = QMudGmcp::frame(
				    R"(Core.Supports.Set ["Client.GMCPMerge 1","Char 1","Comm 1","Room 1","test.feature 1"])");
				QTRY_COMPARE(received, supports + applicationPacket);
				QTRY_COMPARE(runtime.outputPacketCount(), packetsBefore + 2);
				QCOMPARE(runtime.bytesOut(), bytesBefore + received.size());
				if (command)
					QTRY_COMPARE(
					    runtime.pluginVariableValue(id, QStringLiteral("sent_counts")),
					    QStringLiteral("%1:%2").arg(packetsBefore + 1).arg(bytesBefore + supports.size()));
				if (debug)
				{
					QTRY_COMPARE(runtime.pluginVariableValue(id, QStringLiteral("order")),
					             QStringLiteral("supports;application;"));
					QCOMPARE(runtime.pluginVariableValue(id, QStringLiteral("headers")),
					         QStringLiteral("%1:%2;%3:%4;")
					             .arg(packetsBefore + 1)
					             .arg(bytesBefore + supports.size())
					             .arg(packetsBefore + 2)
					             .arg(bytesBefore + received.size()));
				}
				// Drain the original queued notifications as well; a synchronous handoff must not count twice.
				auto *socket = runtime.findChild<WorldSocket *>();
				QVERIFY(socket);
				socket->freezeForReload();
				QTRY_VERIFY(socket->reloadInputDrained());
				QCOMPARE(runtime.outputPacketCount(), packetsBefore + 2);
				QCOMPARE(runtime.bytesOut(), bytesBefore + received.size());
				socket->pauseTransport(false);
				runtime.setWorldFileModified(false);
			}

			/**
			 * @brief Retains disabled owners' declarations, restores shared advertisements, and releases
			 * unloaded owners.
			 */
			void pluginModuleRegistrationLifecycle()
			{
				QTemporaryDir directory(QDir(QStringLiteral(QMUD_TEST_BUILD_DIR))
				                            .filePath(QStringLiteral("worlds/gmcp-modules-XXXXXX")));
				QVERIFY(directory.isValid());
				const QString lowId  = QStringLiteral("050505050505050505050505");
				const QString highId = QStringLiteral("060606060606060606060606");
				const QString script = QStringLiteral(R"lua(
function OnPluginConnect()
 assert(gmcpRegisterModule('Client.GMCPMerge',2)~=0)
 assert(gmcpRegisterModule('cLiEnT.GmCpMeRgE',7)~=0)
 assert(gmcpUnregisterModule('CLIENT.GMCPMERGE')~=0)
 SetVariable('registration_result',tostring(gmcpRegisterModule('Test.Extra',%1)))
end
)lua");
				QVERIFY(writeGmcpPlugin(directory.path(), QStringLiteral("low"), lowId, script.arg(3)));
				QVERIFY(writeGmcpPlugin(directory.path(), QStringLiteral("high"), highId, script.arg(7)));
				AppController controller;
				MainWindow    frame;
				controller.setMainWindow(&frame);
				WorldRuntime     runtime;
				WorldChildWindow child(QStringLiteral("Synthetic world"));
				child.setRuntime(&runtime);
				const auto detach = qScopeGuard([&] { child.setRuntime(nullptr); });
				child.show();
				QVERIFY(QTest::qWaitForWindowExposed(&child));
				runtime.setStartupDirectory(QStringLiteral(QMUD_TEST_BUILD_DIR));
				runtime.setPluginsDirectory(directory.path());
				runtime.setWorldAttribute(QStringLiteral("native_gmcp"), QStringLiteral("1"));
				QString error;
				QVERIFY2(runtime.loadPluginFile(QStringLiteral("low.xml"), &error), qPrintable(error));
				QVERIFY2(runtime.loadPluginFile(QStringLiteral("high.xml"), &error), qPrintable(error));
				QTcpServer server;
				QVERIFY(server.listen(QHostAddress::LocalHost, 0));
				QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
				QTRY_VERIFY(runtime.isConnected() && server.hasPendingConnections());
				QTRY_COMPARE(runtime.pluginVariableValue(lowId, QStringLiteral("registration_result")),
				             QStringLiteral("0"));
				QTRY_COMPARE(runtime.pluginVariableValue(highId, QStringLiteral("registration_result")),
				             QStringLiteral("0"));
				auto      *peer = server.nextPendingConnection();
				QByteArray received;
				connect(peer, &QTcpSocket::readyRead, this, [&] { received += peer->readAll(); });
				peer->write(QByteArray::fromHex("fffbc9"));
				peer->flush();
				QTRY_VERIFY(received.contains("test.extra 7"));
				QCOMPARE(received.count("Client.GMCPMerge 1"), 1);
				QVERIFY(!received.contains("client.gmcpmerge"));
				QCOMPARE(WorldRuntimeTestAccess::gmcpModuleVersion(runtime, lowId,
				                                                   QStringLiteral("Client.GMCPMerge")),
				         0);
				QCOMPARE(WorldRuntimeTestAccess::gmcpModuleVersion(runtime, highId,
				                                                   QStringLiteral("Client.GMCPMerge")),
				         0);
				received.clear();
				QVERIFY(runtime.enablePlugin(highId, false));
				QCOMPARE(
				    WorldRuntimeTestAccess::gmcpModuleVersion(runtime, highId, QStringLiteral("Test.Extra")),
				    7);
				QTRY_VERIFY(received.contains("test.extra 3"));
				QVERIFY(!received.contains("test.extra 7"));
				received.clear();
				QVERIFY(runtime.enablePlugin(highId, true));
				QTRY_VERIFY(received.contains("test.extra 7"));
				received.clear();
				QVERIFY(runtime.enablePlugin(highId, false));
				QTRY_VERIFY(received.contains("test.extra 3"));
				received.clear();
				QVERIFY(runtime.enablePlugin(lowId, false));
				QCOMPARE(
				    WorldRuntimeTestAccess::gmcpModuleVersion(runtime, lowId, QStringLiteral("Test.Extra")),
				    3);
				QTRY_VERIFY(received.contains("Core.Supports.Set"));
				QVERIFY(!received.contains("test.extra"));
				QVERIFY(runtime.unloadPlugin(highId));
				QCOMPARE(
				    WorldRuntimeTestAccess::gmcpModuleVersion(runtime, highId, QStringLiteral("Test.Extra")),
				    0);
				received.clear();
				QVERIFY(runtime.enablePlugin(lowId, true));
				QTRY_VERIFY(received.contains("test.extra 3"));
				QVERIFY(runtime.dispatchLuaExecuteScript(
				    runtime.pluginForId(lowId)->lua, QStringLiteral(R"lua(
assert(gmcpRegisterModule('Test.Cleanup',1)==0)
function OnPluginDisable()
 SetVariable('cleanup_result',tostring(gmcpUnregisterModule('Test.Cleanup')))
end
)lua"),
				    QStringLiteral("synthetic module cleanup"), LuaCallbackEvent::None));
				QTRY_VERIFY(received.contains("test.cleanup 1"));
				received.clear();
				QVERIFY(runtime.enablePlugin(lowId, false));
				QTRY_COMPARE(runtime.pluginVariableValue(lowId, QStringLiteral("cleanup_result")),
				             QStringLiteral("0"));
				QCOMPARE(
				    WorldRuntimeTestAccess::gmcpModuleVersion(runtime, lowId, QStringLiteral("Test.Cleanup")),
				    0);
				QCOMPARE(
				    WorldRuntimeTestAccess::gmcpModuleVersion(runtime, lowId, QStringLiteral("Test.Extra")),
				    3);
				QTRY_VERIFY(received.contains("Core.Supports.Set"));
				received.clear();
				QVERIFY(runtime.enablePlugin(lowId, true));
				QTRY_VERIFY(received.contains("test.extra 3"));
				QVERIFY(!received.contains("test.cleanup"));
				received.clear();
				QVERIFY(runtime.unloadPlugin(lowId));
				QCOMPARE(
				    WorldRuntimeTestAccess::gmcpModuleVersion(runtime, lowId, QStringLiteral("Test.Extra")),
				    0);
				QTRY_VERIFY(received.contains("Core.Supports.Set"));
				QVERIFY(!received.contains("test.extra"));
				runtime.setWorldFileModified(false);
			}

			/**
			 * @brief Preserves wire order when actual Telnet callback execution opens a modal dialog.
			 */
			void modalCallbackPreservesInputOrder()
			{
				QTemporaryDir directory(QDir(QStringLiteral(QMUD_TEST_BUILD_DIR))
				                            .filePath(QStringLiteral("worlds/gmcp-order-XXXXXX")));
				QVERIFY(directory.isValid());
				QVERIFY(writeGmcpPlugin(directory.path(), QStringLiteral("ordering"),
				                        QStringLiteral("030303030303030303030303"), QStringLiteral(R"lua(
function OnPluginTelnetRequest(number,kind)
 if number==77 and kind=='WILL' then utils.inputbox('Synthetic prompt','Synthetic GMCP modal','') end
 return false
end
)lua")));
				AppController controller;
				MainWindow    frame;
				controller.setMainWindow(&frame);
				WorldRuntime runtime;
				WorldView    view;
				view.resize(640, 480);
				view.setRuntime(&runtime);
				const auto detach = qScopeGuard([&] { view.setRuntime(nullptr); });
				view.show();
				QVERIFY(QTest::qWaitForWindowExposed(&view));
				runtime.setStartupDirectory(QStringLiteral(QMUD_TEST_BUILD_DIR));
				runtime.setPluginsDirectory(directory.path());
				runtime.setWorldAttribute(QStringLiteral("native_gmcp"), QStringLiteral("1"));
				QString error;
				QVERIFY2(runtime.loadPluginFile(QStringLiteral("ordering.xml"), &error), qPrintable(error));
				QTRY_VERIFY(!runtime.plugins().constFirst().installPending);
				QTcpServer server;
				QVERIFY(server.listen(QHostAddress::LocalHost, 0));
				QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
				QTRY_VERIFY(server.hasPendingConnections() && runtime.isConnected());
				auto *peer   = server.nextPendingConnection();
				auto *socket = runtime.findChild<WorldSocket *>();
				QVERIFY(socket);
				QStringList lines;
				connect(&runtime, &WorldRuntime::incomingLineReceived, this,
				        [&](const QString &line) { lines.push_back(line); });
				QPointer<QDialog> modal;
				bool              shown = false;
				GmcpModalObserver observer;
				observer.shown = [&](QDialog *dialog)
				{
					shown = true;
					modal = dialog;
					peer->write(QByteArrayLiteral("c\n") + QMudGmcp::frame("Test.AfterC 1"));
					peer->flush();
				};
				qApp->installEventFilter(&observer);
				const auto remove = qScopeGuard([&] { qApp->removeEventFilter(&observer); });
				connect(socket, &WorldSocketService::gmcpObserved, this,
				        [&](const QMudGmcp::Record &record, const auto &)
				        {
					        if (record.raw == QByteArrayLiteral("Test.AfterC 1"))
					        {
						        static_cast<void>(socket->sendPacket({}));
						        QMetaObject::invokeMethod(
						            this,
						            [&]
						            {
							            if (modal)
								            modal->reject();
						            },
						            Qt::QueuedConnection);
					        }
				        });
				QTimer timeout;
				timeout.setSingleShot(true);
				bool timedOut = false;
				connect(&timeout, &QTimer::timeout, this,
				        [&]
				        {
					        timedOut = true;
					        if (modal)
						        modal->reject();
				        });
				timeout.start(5000);
				const QByteArray first(16381, 'a');
				const QByteArray second(16383, 'b');
				QVERIFY(
				    queueNativeBatch(*socket, *peer, QByteArray::fromHex("fffb4d") + first + second + '\n'));
				QCoreApplication::sendPostedEvents(socket, QEvent::MetaCall);
				QTRY_COMPARE(lines.size(), 2);
				timeout.stop();
				QVERIFY(shown);
				QVERIFY(!timedOut);
				QCOMPARE(lines.at(0), QString::fromLatin1(first + second));
				QCOMPARE(lines.at(1), QStringLiteral("c"));
				runtime.setWorldFileModified(false);
			}

			/**
			 * @brief Revalidates plugin-only GMCP delivery across mutations, installation, unload, and plugin
			 * modal resume.
			 */
			static void notificationRecipientEligibility()
			{
				QTemporaryDir directory(QDir(QStringLiteral(QMUD_TEST_BUILD_DIR))
				                            .filePath(QStringLiteral("worlds/gmcp-eligibility-XXXXXX")));
				QVERIFY(directory.isValid());
				const QString enabledId  = QStringLiteral("010101010101010101010101");
				const QString disabledId = QStringLiteral("020202020202020202020202");
				const QString mutatorId  = QStringLiteral("030303030303030303030303");
				const QString pendingId  = QStringLiteral("040404040404040404040404");
				const QString script     = QStringLiteral(R"lua(
function onPluginGmcpMessage(name,payload,state,raw)
 SetVariable('message_seen',tostring((tonumber(GetVariable('message_seen')) or 0)+1))
end
function OnPluginGmcpStateChanged(enabled) SetVariable('state_seen','yes') end
)lua");
				QVERIFY(writeGmcpPlugin(directory.path(), QStringLiteral("enabled"), enabledId, script));
				QVERIFY(writeGmcpPlugin(directory.path(), QStringLiteral("disabled"), disabledId, script));
				GmcpWireFixture session;
				session.write = [](const QByteArray &data) -> qint64 { return data.size(); };
				session.reset(true, 1, false);
				session.receive(QByteArray::fromHex("fffbc9"));
				AppController controller;
				MainWindow    frame;
				controller.setMainWindow(&frame);
				WorldRuntime runtime;
				WorldView    view;
				view.resize(640, 480);
				view.setRuntime(&runtime);
				const auto detach = qScopeGuard([&] { view.setRuntime(nullptr); });
				view.show();
				QVERIFY(QTest::qWaitForWindowExposed(&view));
				runtime.setStartupDirectory(QStringLiteral(QMUD_TEST_BUILD_DIR));
				runtime.setPluginsDirectory(directory.path());
				runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
				WorldRuntimeTestAccess::setGmcpPublication(runtime, session.publication());
				session.observed = [&](const auto &record, const auto &message)
				{ WorldRuntimeTestAccess::deliverGmcpRecord(runtime, record, message); };
				QString error;
				QVERIFY2(runtime.loadPluginFile(QStringLiteral("enabled.xml"), &error), qPrintable(error));
				QVERIFY2(runtime.loadPluginFile(QStringLiteral("disabled.xml"), &error), qPrintable(error));
				QTRY_VERIFY(std::ranges::none_of(runtime.plugins(),
				                                 [](const auto &plugin) { return plugin.installPending; }));
				QVERIFY(runtime.enablePlugin(disabledId, false));
				QVERIFY(writeGmcpPlugin(directory.path(), QStringLiteral("mutator"), mutatorId,
				                        QStringLiteral(R"lua(
function onPluginGmcpMessage(name,payload,state,raw)
 SetVariable('mutator_seen',tostring((tonumber(GetVariable('mutator_seen')) or 0)+1))
 if name=='Test.Enable' then EnablePlugin('020202020202020202020202',true)
 elseif name=='Test.Disable' then EnablePlugin('020202020202020202020202',false)
 elseif name=='Test.Remove' then UnloadPlugin('020202020202020202020202')
 elseif name=='Test.Modal' then
  utils.inputbox('Synthetic prompt','Synthetic GMCP modal','')
  SetVariable('mutator_modal_completed','yes')
 end
end
function OnPluginGmcpStateChanged(enabled) SetVariable('mutator_state','yes') end
)lua"),
				                        50));
				QVERIFY2(runtime.loadPluginFile(QStringLiteral("mutator.xml"), &error), qPrintable(error));
				QTRY_VERIFY(!runtime.pluginForId(mutatorId)->installPending);
				QVERIFY(run(runtime, QStringLiteral(R"lua(
function GmcpTestBarrier() end
function onPluginGmcpMessage() SetVariable('world_seen','unexpected') end
function OnPluginGmcpStateChanged() SetVariable('world_state','unexpected') end
)lua")));
				auto worldVariable = [&](const QString &name)
				{
					QString value;
					static_cast<void>(runtime.findVariable(name, value));
					return value;
				};
				session.receive(QMudGmcp::frame("Test.State {}"));
				gmcpBarrier(runtime);
				auto *socket = runtime.findChild<WorldSocketService *>();
				QVERIFY(socket);
				socket->gmcpStateChanged(true);
				gmcpBarrier(runtime);
				QCOMPARE(runtime.pluginVariableValue(enabledId, QStringLiteral("message_seen")),
				         QStringLiteral("1"));
				QCOMPARE(runtime.pluginVariableValue(enabledId, QStringLiteral("state_seen")),
				         QStringLiteral("yes"));
				QVERIFY(runtime.pluginVariableValue(disabledId, QStringLiteral("message_seen")).isEmpty());
				QVERIFY(runtime.pluginVariableValue(disabledId, QStringLiteral("state_seen")).isEmpty());
				QVERIFY(worldVariable(QStringLiteral("world_seen")).isEmpty());
				QVERIFY(worldVariable(QStringLiteral("world_state")).isEmpty());
				session.receive(QMudGmcp::frame("Test.Enable {}"));
				gmcpBarrier(runtime);
				QCOMPARE(runtime.pluginVariableValue(disabledId, QStringLiteral("message_seen")),
				         QStringLiteral("1"));
				session.receive(QMudGmcp::frame("Test.Disable {}"));
				gmcpBarrier(runtime);
				QCOMPARE(runtime.pluginVariableValue(disabledId, QStringLiteral("message_seen")),
				         QStringLiteral("1"));
				GmcpModalObserver observer;
				bool              shown = false;
				observer.shown          = [&](const QDialog *dialog)
				{
					shown = true;
					QTimer::singleShot(0, dialog, &QDialog::reject);
				};
				qApp->installEventFilter(&observer);
				const auto remove = qScopeGuard([&] { qApp->removeEventFilter(&observer); });
				session.receive(QMudGmcp::frame("Test.Modal {}"));
				QTRY_COMPARE(
				    runtime.pluginVariableValue(mutatorId, QStringLiteral("mutator_modal_completed")),
				    QStringLiteral("yes"));
				QVERIFY(shown);
				gmcpBarrier(runtime);
				session.receive(QMudGmcp::frame("Test.Remove {}"));
				gmcpBarrier(runtime);
				QVERIFY(!runtime.pluginForId(disabledId));
				runtime.setPluginInstallDeferred(true);
				QVERIFY(writeGmcpPlugin(directory.path(), QStringLiteral("pending"), pendingId, script));
				QVERIFY2(runtime.loadPluginFile(QStringLiteral("pending.xml"), &error), qPrintable(error));
				session.receive(QMudGmcp::frame("Test.Pending {}"));
				gmcpBarrier(runtime);
				QVERIFY(runtime.pluginVariableValue(pendingId, QStringLiteral("message_seen")).isEmpty());
				runtime.setPluginInstallDeferred(false);
				QTRY_VERIFY(!runtime.pluginForId(pendingId)->installPending);
				session.receive(QMudGmcp::frame("Test.Pending {}"));
				gmcpBarrier(runtime);
				QCOMPARE(runtime.pluginVariableValue(pendingId, QStringLiteral("message_seen")),
				         QStringLiteral("1"));
				QVERIFY(worldVariable(QStringLiteral("world_seen")).isEmpty());
				QVERIFY(worldVariable(QStringLiteral("world_state")).isEmpty());
				runtime.setWorldFileModified(false);
			}
#endif

			/**
			 * @brief Covers native and legacy connection modes independently of the next-connection
			 * preference.
			 */
			static void browserTracksConnectionMode_data()
			{
				QTest::addColumn<bool>("native");
				QTest::newRow("native") << true;
				QTest::newRow("legacy") << false;
			}

			/**
			 * @brief Checks automatic menu updates and command handling before connection, after preference
			 * changes, and after reconnect.
			 */
			static void browserTracksConnectionMode()
			{
				QFETCH(bool, native);
				AppController controller;
				WorldRuntime  runtime;
				MainWindow    window;
				controller.setMainWindow(&window);
				controller.setGlobalOptionInt(QStringLiteral("ConfirmBeforeClosingWorld"), 0);
				controller.setGlobalOptionInt(QStringLiteral("ConfirmBeforeSavingVariables"), 0);
				runtime.setWorldAttribute(QStringLiteral("native_gmcp"),
				                          native ? QStringLiteral("1") : QStringLiteral("0"));
				auto *child = new WorldChildWindow(QStringLiteral("Synthetic world"));
				child->setRuntime(&runtime);
				window.addMdiSubWindow(child, true);
				window.refreshActionState();
				auto *action = window.findChild<QAction *>(QStringLiteral("GmcpBrowser"));
				QVERIFY(action);
				QCOMPARE(action->isEnabled(), browserAllowed(false));
				QTcpServer server;
				QVERIFY(server.listen(QHostAddress::LocalHost, 0));
				QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
				QTRY_VERIFY(runtime.isConnected() && server.hasPendingConnections());
				server.nextPendingConnection();
				runtime.setWorldAttribute(QStringLiteral("native_gmcp"),
				                          native ? QStringLiteral("0") : QStringLiteral("1"));
				QCOMPARE(runtime.nativeGmcpAvailable(), native);
				QCOMPARE(action->isEnabled(), browserAllowed(native));
				window.refreshActionState();
				QCOMPARE(action->isEnabled(), browserAllowed(native));
				controller.onCommandTriggered(QStringLiteral("GmcpBrowser"));
				QCOMPARE(window.findChild<QWidget *>(QStringLiteral("gmcpBrowser")) != nullptr,
				         browserAllowed(native));
				runtime.closeSocketForReloadReconnect();
				QVERIFY(!runtime.nativeGmcpAvailable());
				QCOMPARE(action->isEnabled(), browserAllowed(false));
				QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
				QTRY_VERIFY(runtime.isConnected() && server.hasPendingConnections());
				server.nextPendingConnection();
				QCOMPARE(runtime.nativeGmcpAvailable(), !native);
				QCOMPARE(action->isEnabled(), browserAllowed(!native));
				runtime.setWorldFileModified(false);
			}

			/**
			 * @brief Checks the exact Game-menu location and compile-time debug access without native state.
			 */
			static void menuPlacement()
			{
				WorldRuntime runtime;
				MainWindow   window;
				auto        *immediate = window.findChild<QAction *>(QStringLiteral("Immediate"));
				auto        *browser   = window.findChild<QAction *>(QStringLiteral("GmcpBrowser"));
				QVERIFY(immediate && browser);
				QMenu *game = nullptr;
				for (auto *menu : window.findChildren<QMenu *>())
					if (menu->actions().contains(immediate))
					{
						game = menu;
						break;
					}
				QVERIFY(game);
				const auto actions = game->actions();
				QCOMPARE(actions.indexOf(browser), actions.indexOf(immediate) + 1);
				window.refreshActionState();
				QCOMPARE(browser->isEnabled(), browserAllowed(false));
				auto *child = new WorldChildWindow(QStringLiteral("Synthetic world"));
				child->setRuntime(&runtime);
				window.addMdiSubWindow(child, true);
				window.refreshActionState();
				QCOMPARE(browser->isEnabled(), browserAllowed(false));
				runtime.setWorldAttribute(QStringLiteral("native_gmcp"), QStringLiteral("1"));
				QCOMPARE(browser->isEnabled(), browserAllowed(false));
				runtime.setWorldAttribute(QStringLiteral("native_gmcp"), QStringLiteral("0"));
				QCOMPARE(browser->isEnabled(), browserAllowed(false));
				runtime.setWorldFileModified(false);
			}

			/**
			 * @brief Opens the empty debug layout without constructing a world or connection.
			 */
			static void browserWithoutWorld()
			{
				AppController controller;
				MainWindow    window;
				controller.setMainWindow(&window);
				window.refreshActionState();
				auto *action = window.findChild<QAction *>(QStringLiteral("GmcpBrowser"));
				QVERIFY(action);
				QCOMPARE(action->isEnabled(), browserAllowed(false));
				controller.onCommandTriggered(QStringLiteral("GmcpBrowser"));
				auto *browser = window.findChild<QWidget *>(QStringLiteral("gmcpBrowser"));
				QCOMPARE(browser != nullptr, browserAllowed(false));
				if (browser)
				{
					auto *tabs = browser->findChild<QTabWidget *>(QStringLiteral("gmcpTabs"));
					QVERIFY(tabs && tabs->count() == 2);
					QCOMPARE(browser->findChild<QTreeView *>(QStringLiteral("gmcpCurrentTree"))
					             ->model()
					             ->rowCount(),
					         0);
					QCOMPARE(browser->findChild<QTableView *>(QStringLiteral("gmcpHistoryVersions"))
					             ->model()
					             ->rowCount(),
					         0);
				}
			}

			/**
			 * @brief Opens paused, inspects complete versions, and preserves history selection through
			 * reception and eviction.
			 */
			static void browserInspection()
			{
				GmcpWireFixture session;
				session.write = [](const QByteArray &bytes) -> qint64 { return bytes.size(); };
				session.reset(true, 1, false);
				session.receive(QByteArray::fromHex("fffbc9"));
				WorldRuntime runtime;
				WorldRuntimeTestAccess::setGmcpPublication(runtime, session.publication());
				session.observed = [&](const auto &record, const auto &message)
				{ WorldRuntimeTestAccess::deliverGmcpRecord(runtime, record, message); };
				const QByteArray first(
				    R"(Test.State {"__gmcp_merge":"recursive","retained":7,"nested":{"value":1}})");
				session.receive(QMudGmcp::frame(first));
				session.receive(QMudGmcp::frame(R"(Test.State {"nested":{"value":2}})"));
				GmcpBrowserDialog browser(&runtime);
				browser.show();
				auto *tabs          = browser.findChild<QTabWidget *>(QStringLiteral("gmcpTabs"));
				auto *current       = browser.findChild<QTreeView *>(QStringLiteral("gmcpCurrentTree"));
				auto *past          = browser.findChild<QTreeView *>(QStringLiteral("gmcpHistoryTree"));
				auto *history       = browser.findChild<QTableView *>(QStringLiteral("gmcpHistoryVersions"));
				auto *currentRaw    = browser.findChild<QPlainTextEdit *>(QStringLiteral("gmcpCurrentRaw"));
				auto *pastRaw       = browser.findChild<QPlainTextEdit *>(QStringLiteral("gmcpHistoryRaw"));
				auto *currentToggle = browser.findChild<QCheckBox *>(QStringLiteral("gmcpCurrentRawToggle"));
				auto *pastToggle    = browser.findChild<QCheckBox *>(QStringLiteral("gmcpHistoryRawToggle"));
				auto *pause         = browser.findChild<QCheckBox *>(QStringLiteral("gmcpPause"));
				QVERIFY(tabs && current && past && history && currentRaw && pastRaw && currentToggle &&
				        pastToggle && pause);
				QVERIFY(pause->isChecked());
				QAbstractItemModelTester currentTester(
				    current->model(), QAbstractItemModelTester::FailureReportingMode::QtTest);
				QAbstractItemModelTester historyTester(
				    history->model(), QAbstractItemModelTester::FailureReportingMode::QtTest);
				QCOMPARE(tabs->count(), 2);
				QCOMPARE(tabs->tabText(0), QStringLiteral("Current"));
				QCOMPARE(tabs->tabText(1), QStringLiteral("History"));
				QCOMPARE(history->model()->rowCount(), 2);
				QCOMPARE(history->currentIndex().row(), 0);
				QCOMPARE(history->model()->index(0, 0).data().toString(),
				         runtime.gmcpHistory().last().time.toLocalTime().toString(
				             QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")));
				const QStringList valuePath{QStringLiteral("Test"), QStringLiteral("State"),
				                            QStringLiteral("nested"), QStringLiteral("value")};
				current->setCurrentIndex(treeIndex(current, valuePath));
				QCOMPARE(treeValue(current->currentIndex()), QStringLiteral("2"));
				session.receive(QMudGmcp::frame(R"(Test.State {"nested":{"value":3}})"));
				QCoreApplication::sendPostedEvents(&browser, QEvent::MetaCall);
				QCOMPARE(treeValue(current->currentIndex()), QStringLiteral("2"));
				QCOMPARE(history->model()->rowCount(), 2);
				currentToggle->setChecked(true);
				QTRY_VERIFY(!currentRaw->toPlainText().isEmpty());
				const auto data = QJsonDocument::fromJson(currentRaw->toPlainText().toUtf8())
				                      .object()
				                      .value(QStringLiteral("Test.State"))
				                      .toObject();
				QCOMPARE(data.value(QStringLiteral("retained")).toInt(), 7);
				QCOMPARE(
				    data.value(QStringLiteral("nested")).toObject().value(QStringLiteral("value")).toInt(),
				    2);
				tabs->setCurrentIndex(1);
				QCOMPARE(treeValue(treeIndex(past, valuePath)), QStringLiteral("2"));
				history->setCurrentIndex(history->model()->index(1, 0));
				const auto historicalIdentity = history->currentIndex().data(Qt::UserRole);
				QCOMPARE(treeValue(treeIndex(past, valuePath)), QStringLiteral("1"));
				pastToggle->setChecked(true);
				QCOMPARE(pastRaw->toPlainText(), QString::fromUtf8(first));
				pause->setChecked(false);
				QCOMPARE(history->model()->rowCount(), 3);
				QCOMPARE(history->currentIndex().data(Qt::UserRole), historicalIdentity);
				QCOMPARE(pastRaw->toPlainText(), QString::fromUtf8(first));
				tabs->setCurrentIndex(0);
				currentToggle->setChecked(false);
				QCOMPARE(treeValue(current->currentIndex()), QStringLiteral("3"));
				QCOMPARE(current->currentIndex(), treeIndex(current, valuePath));
				for (int i = 0; i < 10; ++i)
					session.receive(
					    QMudGmcp::frame(R"(Test.State {"nested":{"value":)" + QByteArray::number(i) + "}}"));
				QCOMPARE(treeValue(current->currentIndex()), QStringLiteral("9"));
				pause->setChecked(true);
				WorldRuntimeTestAccess::evictGmcpHistory(runtime,
				                                         QDateTime::currentDateTimeUtc().addSecs(7200));
				QCOMPARE(runtime.gmcpHistory().size(), 5);
				QCOMPARE(history->model()->rowCount(), 13);
				QCOMPARE(pastRaw->toPlainText(), QString::fromUtf8(first));
				pause->setChecked(false);
				QCOMPARE(history->model()->rowCount(), 5);
				QCOMPARE(history->currentIndex().row(), 0);
				tabs->setCurrentIndex(1);
				pastToggle->setChecked(false);
				QCOMPARE(treeValue(treeIndex(past, valuePath)), QStringLiteral("9"));
				history->setCurrentIndex(history->model()->index(4, 0));
				session.receive(QMudGmcp::frame(R"(Test.State {"nested":{"value":10}})"));
				WorldRuntimeTestAccess::evictGmcpHistory(runtime,
				                                         QDateTime::currentDateTimeUtc().addSecs(7200));
				QCOMPARE(history->currentIndex().row(), 0);
				QCOMPARE(treeValue(treeIndex(past, valuePath)), QStringLiteral("10"));
			}

			/**
			 * @brief Keeps semantic navigation through package replacement, sorted insertion, shape changes,
			 * and reconnect.
			 */
			static void browserTreeStructure()
			{
				GmcpWireFixture session;
				session.write = [](const QByteArray &bytes) -> qint64 { return bytes.size(); };
				session.reset(true, 1, false);
				session.receive(QByteArray::fromHex("fffbc9"));
				WorldRuntime runtime;
				WorldRuntimeTestAccess::setGmcpPublication(runtime, session.publication());
				session.observed = [&](const auto &record, const auto &message)
				{ WorldRuntimeTestAccess::deliverGmcpRecord(runtime, record, message); };
				GmcpBrowserDialog browser(&runtime);
				browser.show();
				auto *tree  = browser.findChild<QTreeView *>(QStringLiteral("gmcpCurrentTree"));
				auto *pause = browser.findChild<QCheckBox *>(QStringLiteral("gmcpPause"));
				pause->setChecked(false);
				auto *proxy = qobject_cast<QAbstractProxyModel *>(tree->model());
				QVERIFY(proxy);
				QAbstractItemModelTester sourceTester(proxy->sourceModel(),
				                                      QAbstractItemModelTester::FailureReportingMode::QtTest);
				QAbstractItemModelTester proxyTester(proxy,
				                                     QAbstractItemModelTester::FailureReportingMode::QtTest);
				session.receive(QMudGmcp::frame(R"(Test.Container {"b":1,"list":[true,null],"slot":{}})"));
				const QStringList packagePath{QStringLiteral("Test"), QStringLiteral("Container")};
				const QStringList fieldPath = packagePath + QStringList{QStringLiteral("b")};
				tree->setCurrentIndex(treeIndex(tree, fieldPath));
				tree->setExpanded(treeIndex(tree, packagePath), true);
				tree->setExpanded(treeIndex(tree, packagePath).parent(), false);
				session.receive(QMudGmcp::frame(R"(Test.Container {"a":2,"b":3,"list":[true,null,5]})"));
				session.receive(QMudGmcp::frame("Test.Container.Child {\"leaf\":7}"));
				session.receive(QMudGmcp::frame("Test.Ping"));
				QCOMPARE(tree->currentIndex(), treeIndex(tree, fieldPath));
				auto container = treeIndex(tree, packagePath);
				QVERIFY(!tree->isExpanded(container.parent()));
				QVERIFY(tree->isExpanded(container));
				QCOMPARE(tree->model()->index(0, 0, container).data().toString(), QStringLiteral("Child"));
				QCOMPARE(tree->model()->index(1, 0, container).data().toString(), QStringLiteral("a"));
				QCOMPARE(tree->model()->index(2, 0, container).data().toString(), QStringLiteral("b"));
				QCOMPARE(treeValue(tree->currentIndex()), QStringLiteral("3"));
				QVERIFY(!treeIndex(tree, packagePath + QStringList{QStringLiteral("slot")}).isValid());
				const auto list = treeIndex(tree, packagePath + QStringList{QStringLiteral("list")});
				QCOMPARE(treeValue(tree->model()->index(0, 0, list)), QStringLiteral("true"));
				QCOMPARE(treeValue(tree->model()->index(1, 0, list)), QStringLiteral("null"));
				session.receive(QMudGmcp::frame(R"(Test.Container {"a":2,"b":3,"list":[false]})"));
				QCOMPARE(tree->currentIndex(), treeIndex(tree, fieldPath));
				const auto shorter = treeIndex(tree, packagePath + QStringList{QStringLiteral("list")});
				QCOMPARE(tree->model()->rowCount(shorter), 1);
				session.receive(QMudGmcp::frame("Test.Container 5"));
				QCOMPARE(tree->model()->rowCount(treeIndex(tree, packagePath)), 1);
				QCOMPARE(treeValue(treeIndex(tree, packagePath)), QStringLiteral("5"));
				session.reset(true, 2, false);
				session.receive(QByteArray::fromHex("fffbc9"));
				session.receive(QMudGmcp::frame("Test.Container.Child {\"leaf\":8}"));
				QCoreApplication::sendPostedEvents(&browser, QEvent::MetaCall);
				container = treeIndex(tree, packagePath);
				QCOMPARE(tree->model()->rowCount(container), 1);
				QVERIFY(treeValue(container).isEmpty());
				QCOMPARE(treeValue(treeIndex(tree, packagePath + QStringList{QStringLiteral("Child"),
				                                                             QStringLiteral("leaf")})),
				         QStringLiteral("8"));
				session.receive(QMudGmcp::frame(".Empty..Branch {\"field\":5}"));
				QVERIFY(treeIndex(tree, {QString{}, QStringLiteral("Empty"), QString{},
				                         QStringLiteral("Branch"), QStringLiteral("field")})
				            .isValid());
			}

			/**
			 * @brief Exercises arrays with zero, one, or multiple child packages arriving in either order.
			 */
			static void browserArrayPackageOverlap_data()
			{
				QTest::addColumn<int>("childPackages");
				QTest::addColumn<bool>("childrenFirst");
				for (const int count : {0, 1, 2})
					for (const bool first : {false, true})
						QTest::newRow(
						    qPrintable(QStringLiteral("children-%1-%2")
						                   .arg(count)
						                   .arg(first ? QStringLiteral("first") : QStringLiteral("last"))))
						    << count << first;
			}

			/**
			 * @brief Preserves array parents and user navigation with namespace rows through live updates and
			 * historical versions.
			 */
			static void browserArrayPackageOverlap()
			{
				QFETCH(int, childPackages);
				QFETCH(bool, childrenFirst);
				GmcpWireFixture session;
				session.write = [](const QByteArray &bytes) -> qint64 { return bytes.size(); };
				session.reset(true, 1, false);
				session.receive(QByteArray::fromHex("fffbc9"));
				WorldRuntime runtime;
				WorldRuntimeTestAccess::setGmcpPublication(runtime, session.publication());
				session.observed = [&](const auto &record, const auto &message)
				{ WorldRuntimeTestAccess::deliverGmcpRecord(runtime, record, message); };
				GmcpBrowserDialog browser(&runtime);
				browser.show();
				auto *current  = browser.findChild<QTreeView *>(QStringLiteral("gmcpCurrentTree"));
				auto *past     = browser.findChild<QTreeView *>(QStringLiteral("gmcpHistoryTree"));
				auto *tabs     = browser.findChild<QTabWidget *>(QStringLiteral("gmcpTabs"));
				auto *versions = browser.findChild<QTableView *>(QStringLiteral("gmcpHistoryVersions"));
				auto *pause    = browser.findChild<QCheckBox *>(QStringLiteral("gmcpPause"));
				auto *filter   = browser.findChild<QLineEdit *>(QStringLiteral("gmcpFilter"));
				pause->setChecked(false);
				auto *currentProxy = qobject_cast<QAbstractProxyModel *>(current->model());
				QVERIFY(currentProxy);
				auto *pastProxy = qobject_cast<QAbstractProxyModel *>(past->model());
				QVERIFY(pastProxy);
				QAbstractItemModelTester currentSourceTester(
				    currentProxy->sourceModel(), QAbstractItemModelTester::FailureReportingMode::QtTest);
				QAbstractItemModelTester currentProxyTester(
				    currentProxy, QAbstractItemModelTester::FailureReportingMode::QtTest);
				QAbstractItemModelTester pastSourceTester(
				    pastProxy->sourceModel(), QAbstractItemModelTester::FailureReportingMode::QtTest);
				QAbstractItemModelTester pastProxyTester(
				    pastProxy, QAbstractItemModelTester::FailureReportingMode::QtTest);
				const QStringList packagePath{QStringLiteral("Test"), QStringLiteral("List")};
				const QStringList firstPath    = packagePath + QStringList{QStringLiteral("1")};
				const QStringList nestedPath   = firstPath + QStringList{QStringLiteral("nested")};
				const QStringList objectPath   = nestedPath + QStringList{QStringLiteral("1")};
				const QStringList selectedPath = objectPath + QStringList{QStringLiteral("leaf")};
				auto              sendChildren = [&]
				{
					if (childPackages >= 1)
						session.receive(QMudGmcp::frame("Test.List.Meta {\"count\":4}"));
					if (childPackages >= 2)
						session.receive(QMudGmcp::frame("Test.List.Options {\"flag\":true}"));
				};
				auto sendArray = [&](int value)
				{
					const QJsonArray payload{
					    QJsonObject{{QStringLiteral("nested"),
					                 QJsonArray{QJsonObject{{QStringLiteral("leaf"), value}}}}},
					    QJsonObject{{QStringLiteral("nested"),
					                 QJsonArray{QJsonObject{{QStringLiteral("leaf"), value + 1}}}}},
					    QJsonArray{QJsonObject{{QStringLiteral("leaf"), value + 2}}}, value + 3};
					session.receive(QMudGmcp::frame("Test.List " +
					                                QJsonDocument(payload).toJson(QJsonDocument::Compact)));
				};
				if (childrenFirst)
					sendChildren();
				sendArray(10);
				current->setExpanded(treeIndex(current, {QStringLiteral("Test")}), true);
				current->setExpanded(treeIndex(current, packagePath), true);
				current->setExpanded(treeIndex(current, firstPath), true);
				current->setExpanded(treeIndex(current, nestedPath), true);
				current->setExpanded(treeIndex(current, objectPath), true);
				current->setCurrentIndex(treeIndex(current, selectedPath));
				const QPersistentModelIndex beforeInsertion(current->currentIndex());
				if (!childrenFirst)
					sendChildren();
				QVERIFY(beforeInsertion.isValid());
				QCOMPARE(current->currentIndex(), treeIndex(current, selectedPath));
				const auto initial         = runtime.gmcpHistory().last();
				const auto initialIdentity = QString::number(initial.generation) + QLatin1Char(':') +
				                             QString::number(initial.sequence);
				auto       checkTree       = [&](const QTreeView *tree, int expected)
				{
					const auto package = treeIndex(tree, packagePath);
					QVERIFY(package.isValid());
					QCOMPARE(tree->model()->rowCount(package), childPackages + 4);
					for (int i = 0; i < 4; ++i)
					{
						const auto element = tree->model()->index(childPackages + i, 0, package);
						QCOMPARE(element.data().toString(), QString::number(i + 1));
						QCOMPARE(element.parent(), package);
					}
					const auto leaf = treeIndex(tree, selectedPath);
					QVERIFY(leaf.isValid());
					QCOMPARE(leaf.parent(), treeIndex(tree, objectPath));
					QCOMPARE(leaf.parent().parent(), treeIndex(tree, nestedPath));
					QCOMPARE(leaf.parent().parent().parent(), treeIndex(tree, firstPath));
					QCOMPARE(treeValue(leaf), QString::number(expected));
					const auto arrayElement = treeIndex(tree, packagePath + QStringList{QStringLiteral("3")});
					const auto nestedElement = tree->model()->index(0, 0, arrayElement);
					QCOMPARE(nestedElement.parent(), arrayElement);
					const auto nestedLeaf = tree->model()->index(0, 0, nestedElement);
					QCOMPARE(nestedLeaf.parent(), nestedElement);
					QCOMPARE(treeValue(nestedLeaf), QString::number(expected + 2));
					for (int column = 0; column < 3; ++column)
						QCOMPARE(leaf.siblingAtColumn(column).parent(), treeIndex(tree, objectPath));
				};
				checkTree(current, 10);
				sendArray(20);
				checkTree(current, 20);
				QCOMPARE(current->currentIndex(), treeIndex(current, selectedPath));
				QVERIFY(current->isExpanded(treeIndex(current, firstPath)));
				QVERIFY(current->isExpanded(treeIndex(current, nestedPath)));
				tabs->setCurrentIndex(1);
				checkTree(past, 20);
				past->setExpanded(treeIndex(past, {QStringLiteral("Test")}), true);
				past->setExpanded(treeIndex(past, packagePath), true);
				past->setExpanded(treeIndex(past, firstPath), true);
				past->setExpanded(treeIndex(past, nestedPath), true);
				past->setExpanded(treeIndex(past, objectPath), true);
				past->setCurrentIndex(treeIndex(past, selectedPath));
				int initialRow = -1;
				for (int row = 0; row < versions->model()->rowCount(); ++row)
					if (versions->model()->index(row, 0).data(Qt::UserRole).toString() == initialIdentity)
						initialRow = row;
				QVERIFY(initialRow >= 0);
				versions->setCurrentIndex(versions->model()->index(initialRow, 0));
				checkTree(past, 10);
				QCOMPARE(past->currentIndex(), treeIndex(past, selectedPath));
				QVERIFY(past->isExpanded(treeIndex(past, firstPath)));
				QVERIFY(past->isExpanded(treeIndex(past, nestedPath)));
				sendArray(30);
				QCOMPARE(treeValue(treeIndex(past, selectedPath)), QStringLiteral("10"));
				tabs->setCurrentIndex(0);
				checkTree(current, 30);
				QCOMPARE(current->currentIndex(), treeIndex(current, selectedPath));
				filter->setText(QStringLiteral("Test.List.1.nested"));
				QCOMPARE(current->currentIndex(), treeIndex(current, selectedPath));
				filter->setText(QStringLiteral("missing"));
				QCOMPARE(current->model()->rowCount(), 0);
				filter->clear();
				QCOMPARE(current->currentIndex(), treeIndex(current, selectedPath));
				pause->setChecked(true);
				sendArray(40);
				QCOMPARE(treeValue(current->currentIndex()), QStringLiteral("30"));
				pause->setChecked(false);
				checkTree(current, 40);
				QCOMPARE(current->currentIndex(), treeIndex(current, selectedPath));
				session.receive(QMudGmcp::frame("Test.List []"));
				QCOMPARE(current->model()->rowCount(treeIndex(current, packagePath)), childPackages);
				QVERIFY(!treeIndex(current, firstPath).isValid());
				sendArray(50);
				checkTree(current, 50);
			}

			/**
			 * @brief Filters dotted paths while preserving field choices across hidden results and live
			 * replacement.
			 */
			static void browserFiltering()
			{
				GmcpWireFixture session;
				session.write = [](const QByteArray &bytes) -> qint64 { return bytes.size(); };
				session.reset(true, 1, false);
				session.receive(QByteArray::fromHex("fffbc9"));
				WorldRuntime runtime;
				WorldRuntimeTestAccess::setGmcpPublication(runtime, session.publication());
				session.observed = [&](const auto &record, const auto &message)
				{ WorldRuntimeTestAccess::deliverGmcpRecord(runtime, record, message); };
				session.receive(QMudGmcp::frame(R"(Test.State {"nested":{"items":[{"value":1}]}})"));
				session.receive(QMudGmcp::frame("Test.Vitals {\"energy\":10}"));
				GmcpBrowserDialog browser(&runtime);
				browser.show();
				auto             *tree   = browser.findChild<QTreeView *>(QStringLiteral("gmcpCurrentTree"));
				auto             *past   = browser.findChild<QTreeView *>(QStringLiteral("gmcpHistoryTree"));
				auto             *tabs   = browser.findChild<QTabWidget *>(QStringLiteral("gmcpTabs"));
				auto             *filter = browser.findChild<QLineEdit *>(QStringLiteral("gmcpFilter"));
				const QStringList itemsPath{QStringLiteral("Test"), QStringLiteral("State"),
				                            QStringLiteral("nested"), QStringLiteral("items")};
				browser.findChild<QCheckBox *>(QStringLiteral("gmcpPause"))->setChecked(false);
				tree->setCurrentIndex(treeIndex(tree, itemsPath));
				tree->setExpanded(tree->currentIndex().parent(), true);
				tree->setExpanded(tree->currentIndex(), false);
				tabs->setCurrentIndex(1);
				filter->setText(QStringLiteral("test.state"));
				QVERIFY(treeIndex(past, itemsPath).isValid());
				QVERIFY(!treeIndex(past, {QStringLiteral("Test"), QStringLiteral("Vitals")}).isValid());
				tabs->setCurrentIndex(0);
				QVERIFY(treeIndex(tree, itemsPath).isValid());
				QCOMPARE(tree->model()->rowCount(treeIndex(tree, itemsPath)), 1);
				filter->setText(QStringLiteral("Test.State.nested.items"));
				QVERIFY(treeIndex(tree, itemsPath).isValid());
				filter->setText(QStringLiteral("missing"));
				QCOMPARE(tree->model()->rowCount(), 0);
				filter->clear();
				QCOMPARE(tree->currentIndex(), treeIndex(tree, itemsPath));
				QVERIFY(!tree->isExpanded(tree->currentIndex()));
				QVERIFY(tree->isExpanded(tree->currentIndex().parent()));
				session.receive(
				    QMudGmcp::frame(R"(Test.State {"nested":{"items":[{"value":2}],"added":true}})"));
				QCOMPARE(tree->currentIndex(), treeIndex(tree, itemsPath));
				QVERIFY(!tree->isExpanded(tree->currentIndex()));
				QCOMPARE(treeValue(treeIndex(
				             tree, itemsPath + QStringList{QStringLiteral("1"), QStringLiteral("value")})),
				         QStringLiteral("2"));
				tabs->setCurrentIndex(1);
				QCOMPARE(treeValue(treeIndex(
				             past, itemsPath + QStringList{QStringLiteral("1"), QStringLiteral("value")})),
				         QStringLiteral("1"));
			}

			/**
			 * @brief Preserves complete ancestor and descendant expansion chains while live filtering removes
			 * and recreates proxy rows.
			 */
			static void browserFilteredExpansion()
			{
				GmcpWireFixture session;
				session.write = [](const QByteArray &bytes) -> qint64 { return bytes.size(); };
				session.reset(true, 1, false);
				session.receive(QByteArray::fromHex("fffbc9"));
				WorldRuntime runtime;
				WorldRuntimeTestAccess::setGmcpPublication(runtime, session.publication());
				session.observed = [&](const auto &record, const auto &message)
				{ WorldRuntimeTestAccess::deliverGmcpRecord(runtime, record, message); };
				session.receive(QMudGmcp::frame(R"(Test.State {"nested":{"energy":1}})"));
				session.receive(QMudGmcp::frame("Test.State.Child [1,2]"));
				session.receive(QMudGmcp::frame("Test.State.Child.Leaf 7"));
				GmcpBrowserDialog browser(&runtime);
				browser.show();
				auto *tree   = browser.findChild<QTreeView *>(QStringLiteral("gmcpCurrentTree"));
				auto *filter = browser.findChild<QLineEdit *>(QStringLiteral("gmcpFilter"));
				browser.findChild<QCheckBox *>(QStringLiteral("gmcpPause"))->setChecked(false);
				const QStringList root{QStringLiteral("Test")};
				const QStringList package = root + QStringList{QStringLiteral("State")};
				const QStringList nested  = package + QStringList{QStringLiteral("nested")};
				const QStringList leaf    = nested + QStringList{QStringLiteral("energy")};
				for (const auto &path : {root, package, nested})
					tree->setExpanded(treeIndex(tree, path), true);
				tree->setCurrentIndex(treeIndex(tree, root));
				filter->setText(QStringLiteral("Test.State.nested.energy"));
				session.receive(QMudGmcp::frame(R"(Test.State {"nested":{"energy":2}})"));
				QVERIFY(tree->isExpanded(treeIndex(tree, root)));
				QVERIFY(tree->isExpanded(treeIndex(tree, package)));
				QVERIFY(tree->isExpanded(treeIndex(tree, nested)));
				QCOMPARE(tree->currentIndex(), treeIndex(tree, root));
				QCOMPARE(treeValue(treeIndex(tree, leaf)), QStringLiteral("2"));
				filter->clear();
				const QStringList child = package + QStringList{QStringLiteral("Child")};
				tree->setExpanded(treeIndex(tree, child), true);
				tree->setCurrentIndex(treeIndex(tree, child + QStringList{QStringLiteral("1")}));
				filter->setText(QStringLiteral("object"));
				session.receive(QMudGmcp::frame(R"(Test.State {"nested":{"energy":3}})"));
				for (const auto &path : {root, package, nested, child})
					QVERIFY(tree->isExpanded(treeIndex(tree, path)));
				QCOMPARE(tree->currentIndex(), treeIndex(tree, child + QStringList{QStringLiteral("1")}));
				filter->clear();
				tree->setExpanded(treeIndex(tree, root), false);
				session.receive(QMudGmcp::frame(R"(Test.State {"nested":{"energy":4}})"));
				QVERIFY(!tree->isExpanded(treeIndex(tree, root)));
				tree->setExpanded(treeIndex(tree, root), true);
				QVERIFY(tree->isExpanded(treeIndex(tree, package)));
				QVERIFY(tree->isExpanded(treeIndex(tree, child)));
			}

			/**
			 * @brief Selects the closest surviving parent as nested fields, array elements, and entire
			 * packages disappear across versions.
			 */
			static void browserNavigationFallback()
			{
				GmcpWireFixture session;
				session.write = [](const QByteArray &bytes) -> qint64 { return bytes.size(); };
				session.reset(true, 1, false);
				session.receive(QByteArray::fromHex("fffbc9"));
				WorldRuntime runtime;
				WorldRuntimeTestAccess::setGmcpPublication(runtime, session.publication());
				session.observed = [&](const auto &record, const auto &message)
				{ WorldRuntimeTestAccess::deliverGmcpRecord(runtime, record, message); };
				session.receive(QMudGmcp::frame("Test.Anchor {\"kept\":1}"));
				session.receive(QMudGmcp::frame("Test.State.Child {\"kept\":2}"));
				const auto intermediate         = runtime.gmcpHistory().last();
				const auto intermediateIdentity = QString::number(intermediate.generation) +
				                                  QLatin1Char(':') + QString::number(intermediate.sequence);
				const QByteArray full(
				    "Test.State.Child.List "
				    "{\"group\":{\"items\":[{\"nested\":{\"leaf\":1}}]},\"a.b\":{\"leaf\":2},\"empty\":{}}");
				session.receive(QMudGmcp::frame(full));
				GmcpBrowserDialog browser(&runtime);
				browser.show();
				auto *tree     = browser.findChild<QTreeView *>(QStringLiteral("gmcpCurrentTree"));
				auto *past     = browser.findChild<QTreeView *>(QStringLiteral("gmcpHistoryTree"));
				auto *tabs     = browser.findChild<QTabWidget *>(QStringLiteral("gmcpTabs"));
				auto *versions = browser.findChild<QTableView *>(QStringLiteral("gmcpHistoryVersions"));
				auto *filter   = browser.findChild<QLineEdit *>(QStringLiteral("gmcpFilter"));
				browser.findChild<QCheckBox *>(QStringLiteral("gmcpPause"))->setChecked(false);
				const QStringList root{QStringLiteral("Test")};
				const QStringList state    = root + QStringList{QStringLiteral("State")};
				const QStringList child    = state + QStringList{QStringLiteral("Child")};
				const QStringList package  = child + QStringList{QStringLiteral("List")};
				const QStringList group    = package + QStringList{QStringLiteral("group")};
				const QStringList items    = group + QStringList{QStringLiteral("items")};
				const QStringList element  = items + QStringList{QStringLiteral("1")};
				const QStringList nested   = element + QStringList{QStringLiteral("nested")};
				const QStringList selected = nested + QStringList{QStringLiteral("leaf")};
				for (const auto &path : {root, state, child, package, group, items, element, nested})
					tree->setExpanded(treeIndex(tree, path), true);
				tree->setCurrentIndex(treeIndex(tree, selected));
				session.receive(QMudGmcp::frame(
				    R"(Test.State.Child.List {"group":{"items":[{}]},"a.b":{"leaf":2},"empty":{}})"));
				QCOMPARE(tree->currentIndex(), treeIndex(tree, element));
				session.receive(QMudGmcp::frame(
				    R"(Test.State.Child.List {"group":{"items":[]},"a.b":{"leaf":2},"empty":{}})"));
				QCOMPARE(tree->currentIndex(), treeIndex(tree, items));
				session.receive(
				    QMudGmcp::frame(R"(Test.State.Child.List {"group":{},"a.b":{"leaf":2},"empty":{}})"));
				QCOMPARE(tree->currentIndex(), treeIndex(tree, group));
				tree->setCurrentIndex(
				    treeIndex(tree, package + QStringList{QStringLiteral("a.b"), QStringLiteral("leaf")}));
				session.receive(QMudGmcp::frame(R"(Test.State.Child.List {"group":{},"a.b":{},"empty":{}})"));
				QCOMPARE(tree->currentIndex(), treeIndex(tree, package + QStringList{QStringLiteral("a.b")}));
				tree->setCurrentIndex(treeIndex(tree, group));
				tree->setExpanded(treeIndex(tree, package), false);
				session.receive(QMudGmcp::frame("Test.State.Child.List {\"empty\":{}}"));
				QCOMPARE(tree->currentIndex(), treeIndex(tree, package));
				QVERIFY(!tree->isExpanded(treeIndex(tree, package)));
				tree->setExpanded(treeIndex(tree, package), true);
				session.receive(QMudGmcp::frame(full));
				for (const auto &path : {group, items, element, nested})
					QVERIFY(tree->isExpanded(treeIndex(tree, path)));
				tree->setCurrentIndex(treeIndex(tree, selected));
				filter->setText(QStringLiteral("Test.State.Child.List.group.items.1.nested.leaf"));
				session.receive(QMudGmcp::frame("Test.State.Child.List {\"group\":{}}"));
				QVERIFY(!tree->currentIndex().isValid());
				filter->clear();
				QCOMPARE(tree->currentIndex(), treeIndex(tree, group));
				tabs->setCurrentIndex(1);
				for (const auto &path : {root, state, child, package, group})
					past->setExpanded(treeIndex(past, path), true);
				past->setCurrentIndex(treeIndex(past, group));
				int intermediateRow = -1;
				for (int row = 0; row < versions->model()->rowCount(); ++row)
					if (versions->model()->index(row, 0).data(Qt::UserRole).toString() ==
					    intermediateIdentity)
						intermediateRow = row;
				QVERIFY(intermediateRow >= 0);
				versions->setCurrentIndex(versions->model()->index(intermediateRow, 0));
				QCOMPARE(past->currentIndex(), treeIndex(past, child));
				QVERIFY(past->isExpanded(treeIndex(past, root)));
				QVERIFY(past->isExpanded(treeIndex(past, state)));
				versions->setCurrentIndex(versions->model()->index(versions->model()->rowCount() - 1, 0));
				QCOMPARE(past->currentIndex(), treeIndex(past, root));
			}

			/**
			 * @brief Leaves a searched unrelated package's indexes and model structure untouched by live
			 * messages.
			 */
			static void browserPackageIsolation()
			{
				GmcpWireFixture session;
				session.write = [](const QByteArray &bytes) -> qint64 { return bytes.size(); };
				session.reset(true, 1, false);
				session.receive(QByteArray::fromHex("fffbc9"));
				WorldRuntime runtime;
				WorldRuntimeTestAccess::setGmcpPublication(runtime, session.publication());
				session.observed = [&](const auto &record, const auto &message)
				{ WorldRuntimeTestAccess::deliverGmcpRecord(runtime, record, message); };
				QJsonObject large;
				for (int i = 0; i < 2000; ++i)
					large.insert(QStringLiteral("field%1").arg(i, 4, 10, QLatin1Char('0')), i);
				session.receive(
				    QMudGmcp::frame("Test.Room " + QJsonDocument(large).toJson(QJsonDocument::Compact)));
				session.receive(QMudGmcp::frame("Test.Vitals {\"energy\":1}"));
				GmcpBrowserDialog browser(&runtime);
				browser.show();
				browser.findChild<QCheckBox *>(QStringLiteral("gmcpPause"))->setChecked(false);
				auto *tree  = browser.findChild<QTreeView *>(QStringLiteral("gmcpCurrentTree"));
				auto *proxy = qobject_cast<QAbstractProxyModel *>(tree->model());
				QVERIFY(proxy);
				auto *source = proxy->sourceModel();
				auto *filter = browser.findChild<QLineEdit *>(QStringLiteral("gmcpFilter"));
				filter->setText(QStringLiteral("missing"));
				filter->clear();
				const QStringList           selectedPath{QStringLiteral("Test"), QStringLiteral("Room"),
				                                         QStringLiteral("field1000")};
				const QPersistentModelIndex retained(treeIndex(tree, selectedPath));
				tree->setCurrentIndex(retained);
				QSignalSpy resets(source, &QAbstractItemModel::modelReset);
				QSignalSpy layouts(source, &QAbstractItemModel::layoutChanged);
				QSignalSpy removed(source, &QAbstractItemModel::rowsAboutToBeRemoved);
				QSignalSpy inserted(source, &QAbstractItemModel::rowsAboutToBeInserted);
				for (int i = 0; i < 5; ++i)
					session.receive(
					    QMudGmcp::frame("Test.Vitals {\"energy\":" + QByteArray::number(i) + "}"));
				QVERIFY(retained.isValid());
				QCOMPARE(tree->currentIndex(), QModelIndex(retained));
				QCOMPARE(resets.count(), 0);
				QCOMPARE(layouts.count(), 0);
				QCOMPARE(removed.count(), 5);
				QCOMPARE(inserted.count(), 5);
				for (const auto &signal : removed)
					QCOMPARE(qvariant_cast<QModelIndex>(signal.at(0)).data(Qt::UserRole + 1).toString(),
					         QStringLiteral("test.vitals"));
				for (const auto &signal : inserted)
					QCOMPARE(qvariant_cast<QModelIndex>(signal.at(0)).data(Qt::UserRole + 1).toString(),
					         QStringLiteral("test.vitals"));
				const auto room = treeIndex(tree, {QStringLiteral("Test"), QStringLiteral("Room")});
				tree->setExpanded(room.parent(), true);
				tree->setExpanded(room, true);
				tree->doItemsLayout();
				tree->scrollTo(retained, QAbstractItemView::PositionAtTop);
				const int horizontal = tree->horizontalScrollBar()->value();
				const int vertical   = tree->verticalScrollBar()->value();
				large.insert(QStringLiteral("aaa"), 7);
				session.receive(
				    QMudGmcp::frame("Test.Room " + QJsonDocument(large).toJson(QJsonDocument::Compact)));
				tree->doItemsLayout();
				QCOMPARE(tree->currentIndex(), treeIndex(tree, selectedPath));
				QCOMPARE(tree->horizontalScrollBar()->value(), horizontal);
				QCOMPARE(tree->verticalScrollBar()->value(), vertical);
			}

			/**
			 * @brief Keeps both viewport coordinates despite offscreen selections, replacement, history
			 * switching, and raw updates.
			 */
			static void browserUpdatesDoNotScroll()
			{
				GmcpWireFixture session;
				session.write = [](const QByteArray &bytes) -> qint64 { return bytes.size(); };
				session.reset(true, 1, false);
				session.receive(QByteArray::fromHex("fffbc9"));
				WorldRuntime runtime;
				WorldRuntimeTestAccess::setGmcpPublication(runtime, session.publication());
				session.observed = [&](const auto &record, const auto &message)
				{ WorldRuntimeTestAccess::deliverGmcpRecord(runtime, record, message); };
				QJsonObject payload;
				for (int i = 0; i < 250; ++i)
					payload.insert(QStringLiteral("field%1").arg(i, 4, 10, QLatin1Char('0')), i);
				auto sendPayload = [&]
				{
					session.receive(QMudGmcp::frame("Test.Data " +
					                                QJsonDocument(payload).toJson(QJsonDocument::Compact)));
				};
				sendPayload();
				GmcpBrowserDialog browser(&runtime);
				browser.show();
				auto *tabs      = browser.findChild<QTabWidget *>(QStringLiteral("gmcpTabs"));
				auto *current   = browser.findChild<QTreeView *>(QStringLiteral("gmcpCurrentTree"));
				auto *past      = browser.findChild<QTreeView *>(QStringLiteral("gmcpHistoryTree"));
				auto *versions  = browser.findChild<QTableView *>(QStringLiteral("gmcpHistoryVersions"));
				auto *pause     = browser.findChild<QCheckBox *>(QStringLiteral("gmcpPause"));
				auto *rawToggle = browser.findChild<QCheckBox *>(QStringLiteral("gmcpCurrentRawToggle"));
				auto *raw       = browser.findChild<QPlainTextEdit *>(QStringLiteral("gmcpCurrentRaw"));
				pause->setChecked(false);
				const QStringList packagePath{QStringLiteral("Test"), QStringLiteral("Data")};
				const QStringList selectedPath = packagePath + QStringList{QStringLiteral("field0002")};
				auto              prepare      = [&](QTreeView *tree)
				{
					tree->setExpanded(treeIndex(tree, {QStringLiteral("Test")}), true);
					tree->setExpanded(treeIndex(tree, packagePath), true);
					tree->setCurrentIndex(treeIndex(tree, selectedPath));
					tree->header()->resizeSection(0, tree->viewport()->width() + 160);
					tree->doItemsLayout();
					QVERIFY(tree->horizontalScrollBar()->maximum() > 0);
					QVERIFY(tree->verticalScrollBar()->maximum() > 0);
					tree->horizontalScrollBar()->setValue(tree->horizontalScrollBar()->maximum());
					tree->verticalScrollBar()->setValue(tree->verticalScrollBar()->maximum() / 2 + 3);
					QVERIFY(!tree->viewport()->rect().intersects(tree->visualRect(tree->currentIndex())));
				};
				auto position = [](const QAbstractScrollArea *area)
				{ return QPoint(area->horizontalScrollBar()->value(), area->verticalScrollBar()->value()); };
				prepare(current);
				const auto original = position(current);
				payload.insert(QStringLiteral("aaa"), 1);
				sendPayload();
				QCOMPARE(position(current), original);
				QCOMPARE(current->currentIndex(), treeIndex(current, selectedPath));
				QCoreApplication::sendPostedEvents(&browser, QEvent::MetaCall);
				current->doItemsLayout();
				QCOMPARE(position(current), original);
				session.receive(QMudGmcp::frame("Test.Other {\"value\":1}"));
				session.receive(QMudGmcp::frame("Test.Data.Meta {\"value\":2}"));
				QCOMPARE(position(current), original);
				payload.remove(QStringLiteral("field0002"));
				sendPayload();
				QCOMPARE(position(current), original);
				QCOMPARE(current->currentIndex(), treeIndex(current, packagePath));
				payload.insert(QStringLiteral("field0002"), 2);
				sendPayload();
				prepare(current);
				const auto beforeResume = position(current);
				pause->setChecked(true);
				payload.insert(QStringLiteral("aab"), 3);
				sendPayload();
				QCOMPARE(position(current), beforeResume);
				pause->setChecked(false);
				QCOMPARE(position(current), beforeResume);
				QCOMPARE(current->currentIndex(), treeIndex(current, selectedPath));
				tabs->setCurrentIndex(1);
				prepare(past);
				const auto historical = position(past);
				versions->setCurrentIndex(versions->model()->index(versions->model()->rowCount() - 1, 0));
				QCOMPARE(position(past), historical);
				QCOMPARE(past->currentIndex(), treeIndex(past, selectedPath));
				for (int i = 0; i < 40; ++i)
					session.receive(QMudGmcp::frame("Test.Other {\"value\":" + QByteArray::number(i) + "}"));
				versions->doItemsLayout();
				QVERIFY(versions->verticalScrollBar()->maximum() > 0);
				versions->verticalScrollBar()->setValue(versions->verticalScrollBar()->maximum() / 2);
				const auto timestamps = position(versions);
				session.receive(QMudGmcp::frame("Test.Other {\"value\":40}"));
				QCoreApplication::sendPostedEvents(&browser, QEvent::MetaCall);
				versions->doItemsLayout();
				QCOMPARE(position(versions), timestamps);
				QCOMPARE(position(past), historical);
				const auto observer = session.observed;
				session.observed    = [&](const auto &record, const auto &message)
				{
					auto aged = record;
					aged.time = QDateTime::currentDateTimeUtc().addSecs(-7200);
					WorldRuntimeTestAccess::deliverGmcpRecord(runtime, aged, message);
				};
				session.receive(QMudGmcp::frame("Test.Other {\"value\":41}"));
				const auto aged = runtime.gmcpHistory().last();
				const auto agedIdentity =
				    QString::number(aged.generation) + QLatin1Char(':') + QString::number(aged.sequence);
				session.observed = observer;
				for (int i = 42; i < 48; ++i)
					session.receive(QMudGmcp::frame("Test.Other {\"value\":" + QByteArray::number(i) + "}"));
				for (int row = 0; row < versions->model()->rowCount(); ++row)
					if (versions->model()->index(row, 0).data(Qt::UserRole).toString() == agedIdentity)
						versions->setCurrentIndex(versions->model()->index(row, 0));
				QCOMPARE(versions->currentIndex().data(Qt::UserRole).toString(), agedIdentity);
				versions->doItemsLayout();
				versions->verticalScrollBar()->setValue(versions->verticalScrollBar()->maximum() / 2);
				const auto beforeEviction        = position(versions);
				const auto payloadBeforeEviction = position(past);
				WorldRuntimeTestAccess::evictGmcpHistory(runtime, QDateTime::currentDateTimeUtc());
				versions->doItemsLayout();
				QCOMPARE(versions->currentIndex().row(), 0);
				QCOMPARE(position(versions), beforeEviction);
				QCOMPARE(position(past), payloadBeforeEviction);
				tabs->setCurrentIndex(0);
				QCOMPARE(position(current), beforeResume);
				rawToggle->setChecked(true);
				QTRY_VERIFY(!raw->toPlainText().isEmpty());
				auto       cursor = raw->textCursor();
				const auto selectedText =
				    static_cast<int>(raw->toPlainText().indexOf(QStringLiteral("field0002")));
				QVERIFY(selectedText >= 0);
				cursor.setPosition(selectedText);
				cursor.setPosition(selectedText + 9, QTextCursor::KeepAnchor);
				raw->setTextCursor(cursor);
				QVERIFY(raw->verticalScrollBar()->maximum() > 0);
				raw->verticalScrollBar()->setValue(raw->verticalScrollBar()->maximum() / 2);
				const auto rawPosition = position(raw);
				const int  anchor      = raw->textCursor().anchor();
				const int  end         = raw->textCursor().position();
				payload.insert(QStringLiteral("field0100"), 999);
				sendPayload();
				QTRY_COMPARE(QJsonDocument::fromJson(raw->toPlainText().toUtf8())
				                 .object()
				                 .value(QStringLiteral("Test.Data"))
				                 .toObject()
				                 .value(QStringLiteral("field0100"))
				                 .toInt(),
				             999);
				QCOMPARE(position(raw), rawPosition);
				QCOMPARE(raw->textCursor().anchor(), anchor);
				QCOMPARE(raw->textCursor().position(), end);
				QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
				QCOMPARE(position(raw), rawPosition);
				rawToggle->setChecked(false);
				QCOMPARE(position(current), beforeResume);
				QVERIFY(current->hasAutoScroll());
				current->scrollTo(treeIndex(current, selectedPath), QAbstractItemView::PositionAtTop);
				QVERIFY(position(current) != beforeResume);
			}

			/**
			 * @brief Ignores already displayed queued versions rather than rolling a resumed view backward.
			 */
			static void browserQueuedVersions()
			{
				GmcpWireFixture session;
				session.write = [](const QByteArray &bytes) -> qint64 { return bytes.size(); };
				session.reset(true, 1, false);
				session.receive(QByteArray::fromHex("fffbc9"));
				QList<std::pair<QMudGmcp::Record, std::shared_ptr<const QMudGmcp::Message>>> queued;
				session.observed = [&](const auto &record, const auto &message)
				{
					if (message)
						queued.append({record, message});
				};
				session.receive(QMudGmcp::frame("Test.State {\"value\":1}"));
				session.receive(QMudGmcp::frame("Test.State {\"value\":2}"));
				WorldRuntime runtime;
				WorldRuntimeTestAccess::setGmcpPublication(runtime, session.publication());
				GmcpBrowserDialog browser(&runtime);
				browser.show();
				browser.findChild<QCheckBox *>(QStringLiteral("gmcpPause"))->setChecked(false);
				auto *tree = browser.findChild<QTreeView *>(QStringLiteral("gmcpCurrentTree"));
				for (const auto &[record, message] : queued)
					WorldRuntimeTestAccess::deliverGmcpRecord(runtime, record, message);
				QCOMPARE(treeValue(treeIndex(tree, {QStringLiteral("Test"), QStringLiteral("State"),
				                                    QStringLiteral("value")})),
				         QStringLiteral("2"));
				session.observed = [&](const auto &record, const auto &message)
				{ WorldRuntimeTestAccess::deliverGmcpRecord(runtime, record, message); };
				session.receive(QMudGmcp::frame("Test.State {\"value\":3}"));
				QCOMPARE(treeValue(treeIndex(tree, {QStringLiteral("Test"), QStringLiteral("State"),
				                                    QStringLiteral("value")})),
				         QStringLiteral("3"));
			}

			/**
			 * @brief Updates only changed Raw package blocks through insertion, renaming, deferred display,
			 * and connection replacement.
			 */
			static void browserRawPackageUpdates()
			{
				GmcpWireFixture session;
				session.write = [](const QByteArray &bytes) -> qint64 { return bytes.size(); };
				session.reset(true, 1, false);
				session.receive(QByteArray::fromHex("fffbc9"));
				WorldRuntime runtime;
				WorldRuntimeTestAccess::setGmcpPublication(runtime, session.publication());
				session.observed = [&](const auto &record, const auto &message)
				{ WorldRuntimeTestAccess::deliverGmcpRecord(runtime, record, message); };
				QJsonObject large;
				for (int i = 0; i < 1000; ++i)
					large.insert(QStringLiteral("field%1").arg(i, 4, 10, QLatin1Char('0')), i);
				session.receive(
				    QMudGmcp::frame("Test.Middle " + QJsonDocument(large).toJson(QJsonDocument::Compact)));
				session.receive(QMudGmcp::frame("Test.Last 1"));
				GmcpBrowserDialog browser(&runtime);
				browser.show();
				auto *raw    = browser.findChild<QPlainTextEdit *>(QStringLiteral("gmcpCurrentRaw"));
				auto *toggle = browser.findChild<QCheckBox *>(QStringLiteral("gmcpCurrentRawToggle"));
				auto *pause  = browser.findChild<QCheckBox *>(QStringLiteral("gmcpPause"));
				toggle->setChecked(true);
				QTRY_VERIFY(!raw->toPlainText().isEmpty());
				pause->setChecked(false);
				// Wait for the explicit resume binding before observing incremental edits.
				auto data = [&] { return QJsonDocument::fromJson(raw->toPlainText().toUtf8()).object(); };
				session.receive(QMudGmcp::frame("Test.Last 2"));
				QTRY_COMPARE(data().value(QStringLiteral("Test.Last")).toInt(), 2);
				const auto retained = raw->document()->findBlock(
				    static_cast<int>(raw->toPlainText().indexOf(QStringLiteral("field0500"))));
				QVERIFY(retained.isValid());
				QSignalSpy edits(raw->document(), &QTextDocument::contentsChange);
				session.receive(QMudGmcp::frame("Test.Last {\"value\":3}"));
				QTRY_COMPARE(data()
				                 .value(QStringLiteral("Test.Last"))
				                 .toObject()
				                 .value(QStringLiteral("value"))
				                 .toInt(),
				             3);
				QCOMPARE(retained, raw->document()->findBlock(static_cast<int>(
				                       raw->toPlainText().indexOf(QStringLiteral("field0500")))));
				for (const auto &edit : edits)
					QVERIFY(edit.at(0).toInt() + edit.at(1).toInt() <= retained.position());
				for (const auto &message : {QByteArray("Test.First [1,2]"), QByteArray("Test.Zulu null"),
				                            QByteArray("test.last [3,4]"), QByteArray("test.FIRST 5"),
				                            QByteArray("test.MIDDLE {\"retained\":true}"),
				                            QByteArray("TEST.LAST false"), QByteArray("test.last [3,4]")})
				{
					session.receive(QMudGmcp::frame(message));
					QTRY_VERIFY(
					    raw->toPlainText().contains(QString::fromUtf8(message.left(message.indexOf(' ')))));
					QTRY_COMPARE(data().size(), session.publication()->current.load()->messages.size());
					QCOMPARE(raw->toPlainText(),
					         QString::fromUtf8(QJsonDocument(data()).toJson(QJsonDocument::Indented)));
				}
				QCOMPARE(data().value(QStringLiteral("test.last")).toArray().size(), 2);
				QCOMPARE(data().value(QStringLiteral("test.FIRST")).toInt(), 5);
				toggle->setChecked(false);
				session.receive(QMudGmcp::frame("Test.Zulu 6"));
				session.receive(QMudGmcp::frame("Test.Zulu 7"));
				toggle->setChecked(true);
				QTRY_COMPARE(data().value(QStringLiteral("Test.Zulu")).toInt(), 7);
				pause->setChecked(true);
				session.receive(QMudGmcp::frame("Test.Zulu 8"));
				QCOMPARE(data().value(QStringLiteral("Test.Zulu")).toInt(), 7);
				pause->setChecked(false);
				QTRY_COMPARE(data().value(QStringLiteral("Test.Zulu")).toInt(), 8);
				// Replace the binding while its previous formatting completion can still be queued.
				session.receive(QMudGmcp::frame("Test.Zulu 9"));
				session.reset(true, 2, false);
				session.receive(QByteArray::fromHex("fffbc9"));
				session.receive(QMudGmcp::frame("Fresh.State 10"));
				QTRY_COMPARE(data().size(), 1);
				QTRY_COMPARE(data().value(QStringLiteral("Fresh.State")).toInt(), 10);
				session.receive(QMudGmcp::frame("FRESH.STATE \"updated\""));
				QTRY_COMPARE(data().value(QStringLiteral("FRESH.STATE")).toString(),
				             QStringLiteral("updated"));
				QCOMPARE(data().size(), 1);
				session.receive(QMudGmcp::frame("Fresh.State 11"));
				// Destruction must not wait for or be referenced by the outstanding formatting task.
			}

			/**
			 * @brief Adds constant JSON type markers and defers hidden raw rendering until its pane is
			 * displayed.
			 */
			static void browserTypesAndRawVisibility()
			{
				GmcpWireFixture session;
				session.write = [](const QByteArray &bytes) -> qint64 { return bytes.size(); };
				session.reset(true, 1, false);
				session.receive(QByteArray::fromHex("fffbc9"));
				WorldRuntime runtime;
				WorldRuntimeTestAccess::setGmcpPublication(runtime, session.publication());
				session.observed = [&](const auto &record, const auto &message)
				{ WorldRuntimeTestAccess::deliverGmcpRecord(runtime, record, message); };
				session.receive(QMudGmcp::frame("Test.Types "
				                                "{\"text\":\"null\",\"nothing\":null,\"number\":1,"
				                                "\"boolean\":true,\"array\":[],\"object\":{}}"));
				session.receive(QMudGmcp::frame("Test.Ping"));
				GmcpBrowserDialog browser(&runtime);
				browser.show();
				auto *tree   = browser.findChild<QTreeView *>(QStringLiteral("gmcpCurrentTree"));
				auto *tabs   = browser.findChild<QTabWidget *>(QStringLiteral("gmcpTabs"));
				auto *pause  = browser.findChild<QCheckBox *>(QStringLiteral("gmcpPause"));
				auto *toggle = browser.findChild<QCheckBox *>(QStringLiteral("gmcpCurrentRawToggle"));
				auto *raw    = browser.findChild<QPlainTextEdit *>(QStringLiteral("gmcpCurrentRaw"));
				const std::pair<QString, QString> cases[] = {
				    {QStringLiteral("text"),    QStringLiteral("string") },
				    {QStringLiteral("nothing"), QStringLiteral("null")   },
				    {QStringLiteral("number"),  QStringLiteral("number") },
				    {QStringLiteral("boolean"), QStringLiteral("boolean")},
				    {QStringLiteral("array"),   QStringLiteral("array")  },
				    {QStringLiteral("object"),  QStringLiteral("object") }
                };
				for (const auto &[field, type] : cases)
					QCOMPARE(treeIndex(tree, {QStringLiteral("Test"), QStringLiteral("Types"), field})
					             .siblingAtColumn(2)
					             .data()
					             .toString(),
					         type);
				QCOMPARE(treeIndex(tree, {QStringLiteral("Test"), QStringLiteral("Ping")})
				             .siblingAtColumn(2)
				             .data()
				             .toString(),
				         QStringLiteral("no payload"));
				toggle->setChecked(true);
				QTRY_VERIFY(!raw->toPlainText().isEmpty());
				const auto original = raw->toPlainText();
				pause->setChecked(false);
				tabs->setCurrentIndex(1);
				QSignalSpy changed(raw, &QPlainTextEdit::textChanged);
				session.receive(QMudGmcp::frame("Test.Types {\"number\":2}"));
				QCOMPARE(changed.count(), 0);
				QCOMPARE(raw->toPlainText(), original);
				tabs->setCurrentIndex(0);
				QTRY_COMPARE(changed.count(), 1);
				QCOMPARE(QJsonDocument::fromJson(raw->toPlainText().toUtf8())
				             .object()
				             .value(QStringLiteral("Test.Types"))
				             .toObject()
				             .value(QStringLiteral("number"))
				             .toInt(),
				         2);
				changed.clear();
				browser.hide();
				session.receive(QMudGmcp::frame("Test.Types {\"number\":3}"));
				QCOMPARE(changed.count(), 0);
				browser.show();
				QTRY_COMPARE(changed.count(), 1);
				toggle->setChecked(false);
				QCOMPARE(treeValue(treeIndex(tree, {QStringLiteral("Test"), QStringLiteral("Types"),
				                                    QStringLiteral("number")})),
				         QStringLiteral("3"));
			}

			/**
			 * @brief Exercises browser ownership through hidden panes, Raw mode, pause, and destruction.
			 */
			static void browserBindingRetirement_data()
			{
				QTest::addColumn<QString>("mode");
				for (const auto &mode : {"current-raw", "current-hidden", "history-raw", "paused-resume",
				                         "browser-close", "runtime-close"})
					QTest::newRow(mode) << QString::fromLatin1(mode);
			}
			/**
			 * @brief Checks that expired browser payloads are released on a worker without reopening a hidden tree.
			 */
			static void browserBindingRetirement()
			{
				QFETCH(QString, mode);
				/**
				 * @brief Records the final payload release independently of widget and fixture lifetimes.
				 */
				struct Probe
				{
						QSemaphore destroyed;
						bool       onGuiThread{false};
				};
				auto probe       = std::make_shared<Probe>();
				auto runtime     = std::make_unique<WorldRuntime>();
				auto publication = std::make_shared<QMudGmcp::Publication>();
				WorldRuntimeTestAccess::setGmcpPublication(*runtime, publication);
				{
					auto snapshot    = std::make_shared<QMudGmcp::Snapshot>();
					snapshot->native = snapshot->connectionSelected = snapshot->negotiated = true;
					snapshot->session = snapshot->generation = snapshot->revision = 1;
					snapshot->packages.add(QStringLiteral("Test.State"));
					auto message = QSharedPointer<QMudGmcp::Message>(
					    new QMudGmcp::Message,
					    [probe, guiThread = QThread::currentThread()](const QMudGmcp::Message *value)
					    {
						    probe->onGuiThread = QThread::currentThread() == guiThread;
						    delete value;
						    probe->destroyed.release();
					    });
					message->name  = QStringLiteral("Test.State");
					message->raw   = QByteArrayLiteral("Test.State {\"value\":1}");
					message->state = message->payload = QJsonObject{
					    {QStringLiteral("value"), 1}
                    };
					snapshot->messages = snapshot->messages.withUpdates({
					    {QStringLiteral("test.state"), message}
                    });
					publication->current.store(snapshot);
					QMudGmcp::Record record;
					record.generation = record.sequence = record.revision = 1;
					record.time                                           = QDateTime::currentDateTimeUtc();
					record.messages                                       = snapshot->messages;
					record.packages                                       = snapshot->packages;
					record.raw                                            = message->raw;
					WorldRuntimeTestAccess::deliverGmcpRecord(*runtime, record, {});
				}
				auto browser = std::make_unique<GmcpBrowserDialog>(runtime.get());
				browser->show();
				auto      *pause = browser->findChild<QCheckBox *>(QStringLiteral("gmcpPause"));
				auto      *tabs  = browser->findChild<QTabWidget *>(QStringLiteral("gmcpTabs"));
				const bool paused =
				    mode == QLatin1String("paused-resume") || mode.endsWith(QLatin1String("close"));
				pause->setChecked(paused);
				if (mode == QLatin1String("current-hidden") || mode == QLatin1String("history-raw"))
					tabs->setCurrentIndex(1);
				if (mode.endsWith(QLatin1String("raw")))
				{
					const auto prefix = mode == QLatin1String("history-raw") ? QStringLiteral("gmcpHistory")
					                                                         : QStringLiteral("gmcpCurrent");
					browser->findChild<QCheckBox *>(prefix + QStringLiteral("RawToggle"))->setChecked(true);
					QCOMPARE(
					    browser->findChild<QTreeView *>(prefix + QStringLiteral("Tree"))->model()->rowCount(),
					    0);
				}
				{
					auto snapshot    = std::make_shared<QMudGmcp::Snapshot>();
					snapshot->native = snapshot->connectionSelected = snapshot->negotiated = true;
					snapshot->session = snapshot->generation = 1;
					snapshot->revision                       = 6;
					snapshot->packages.add(QStringLiteral("Test.State"));
					QMudGmcp::Message message;
					message.name  = QStringLiteral("Test.State");
					message.raw   = QByteArrayLiteral("Test.State {\"value\":2}");
					message.state = message.payload = QJsonObject{
					    {QStringLiteral("value"), 2}
                    };
					message.session    = 1;
					snapshot->messages = snapshot->messages.withUpdates({
					    {QStringLiteral("test.state"),
					     QSharedPointer<const QMudGmcp::Message>::create(message)}
                    });
					publication->current.store(snapshot);
					for (quint64 sequence = 2; sequence <= 6; ++sequence)
					{
						QMudGmcp::Record record;
						record.generation = 1;
						record.sequence = record.revision = sequence;
						record.time                       = QDateTime::currentDateTimeUtc();
						record.messages                   = snapshot->messages;
						record.packages                   = snapshot->packages;
						record.raw                        = message.raw;
						WorldRuntimeTestAccess::deliverGmcpRecord(
						    *runtime, record, std::make_shared<const QMudGmcp::Message>(message));
					}
				}
				WorldRuntimeTestAccess::evictGmcpHistory(*runtime,
				                                         QDateTime::currentDateTimeUtc().addSecs(7200));
				QCOMPARE(runtime->gmcpHistory().size(), 5);
				if (paused)
					QVERIFY(!probe->destroyed.tryAcquire());
				if (mode == QLatin1String("browser-close"))
					browser.reset();
				else if (mode == QLatin1String("runtime-close"))
				{
					const QPointer<GmcpBrowserDialog> guard(browser.release());
					runtime.reset();
					QTRY_VERIFY(guard.isNull());
				}
				else if (paused)
					pause->setChecked(false);
				QTRY_VERIFY(probe->destroyed.available() > 0);
				QVERIFY(probe->destroyed.tryAcquire());
				QVERIFY(!probe->onGuiThread);
			}
			/**
			 * @brief Exercises the browser's configured history widget with increasing retained counts.
			 */
			static void browserHistoryLayout_data()
			{
				QTest::addColumn<int>("rows");
				QTest::newRow("one-thousand") << 1000;
				QTest::newRow("one-hundred-thousand") << 100000;
			}
			/**
			 * @brief Bounds initial layout, incoming updates, eviction, and resume geometry by the viewport.
			 */
			static void browserHistoryLayout()
			{
				QFETCH(int, rows);
				HistoryLayoutModel model(rows);
				GmcpBrowserDialog  browser(nullptr);
				browser.show();
				auto *tabs     = browser.findChild<QTabWidget *>(QStringLiteral("gmcpTabs"));
				auto *versions = browser.findChild<QTableView *>(QStringLiteral("gmcpHistoryVersions"));
				QVERIFY(tabs && versions);
				tabs->setCurrentIndex(1);
				// Replace only the empty preview's model; the other browser tests exercise the real
				// runtime history, its pause/resume binding, and stable selection through eviction.
				versions->setModel(&model);
				auto checkLayout = [&]
				{
					versions->doItemsLayout();
					QCoreApplication::processEvents();
					versions->viewport()->repaint();
					const int visibleRows =
					    versions->viewport()->height() / qMax(1, versions->rowHeight(0)) + 2;
					const auto limit = static_cast<quint64>(visibleRows) * 16 + 128;
					QVERIFY2(model.indexRequests <= limit,
					         qPrintable(
					             QStringLiteral("%1 index requests for %2 visible rows and %3 retained rows")
					                 .arg(model.indexRequests)
					                 .arg(visibleRows)
					                 .arg(model.rowCount({}))));
					QVERIFY(model.displayRequests > 0);
					QVERIFY(model.displayRequests <= limit);
				};
				checkLayout();
				versions->setCurrentIndex(model.index(rows / 2, 0, {}));
				versions->doItemsLayout();
				versions->verticalScrollBar()->setValue(versions->verticalScrollBar()->maximum() / 2);
				const auto selected = versions->currentIndex().data(Qt::UserRole);
				const int  position = versions->verticalScrollBar()->value();
				model.clearRequests();
				model.prepend();
				checkLayout();
				QCOMPARE(versions->currentIndex().data(Qt::UserRole), selected);
				QCOMPARE(versions->verticalScrollBar()->value(), position);
				model.clearRequests();
				model.removeOldest(100);
				checkLayout();
				QCOMPARE(versions->currentIndex().data(Qt::UserRole), selected);
				QCOMPARE(versions->verticalScrollBar()->value(), position);
				model.clearRequests();
				model.rebind();
				checkLayout();
				model.clearRequests();
				browser.resize(browser.width() + 40, browser.height() + 40);
				checkLayout();
			}

			/**
			 * @brief Keeps the selected record across large paused histories, reconnect identities, and timestamp holes.
			 */
			static void browserPinnedHistorySelection()
			{
				WorldRuntime runtime;
				const auto   now    = QDateTime::currentDateTimeUtc();
				auto         append = [&](quint64 generation, quint64 first, quint64 last)
				{
					for (quint64 sequence = first; sequence <= last; ++sequence)
					{
						QMudGmcp::Record record;
						record.generation = generation;
						record.sequence   = sequence;
						record.outgoing   = sequence % 2 == 0;
						record.time       = sequence % 13 == 0 ? now.addSecs(-7200) : now;
						record.raw        = QByteArrayLiteral("Test.Ping");
						WorldRuntimeTestAccess::deliverGmcpRecord(runtime, record, {});
					}
				};
				append(1, 1, 4096);
				append(2, 1, 4096);
				GmcpBrowserDialog browser(&runtime);
				browser.show();
				auto *tabs     = browser.findChild<QTabWidget *>(QStringLiteral("gmcpTabs"));
				auto *versions = browser.findChild<QTableView *>(QStringLiteral("gmcpHistoryVersions"));
				auto *past     = browser.findChild<QTreeView *>(QStringLiteral("gmcpHistoryTree"));
				auto *pause    = browser.findChild<QCheckBox *>(QStringLiteral("gmcpPause"));
				tabs->setCurrentIndex(1);
				versions->setCurrentIndex(versions->model()->index(8192 - 257, 0));
				QCOMPARE(versions->currentIndex().data(Qt::UserRole).toString(), QStringLiteral("1:257"));
				versions->doItemsLayout();
				versions->verticalScrollBar()->setValue(versions->verticalScrollBar()->maximum() / 2);
				const int  pausedPosition = versions->verticalScrollBar()->value();
				QSignalSpy resets(past->model(), &QAbstractItemModel::modelReset);
				append(2, 4097, 8192);
				append(3, 1, 4096);
				WorldRuntimeTestAccess::evictGmcpHistory(runtime, now);
				QCOMPARE(versions->model()->rowCount(), 8192);
				QCOMPARE(versions->verticalScrollBar()->value(), pausedPosition);
				pause->setChecked(false);
				QCOMPARE(versions->currentIndex().data(Qt::UserRole).toString(), QStringLiteral("1:257"));
				QCOMPARE(versions->verticalScrollBar()->value(), pausedPosition);
				QCOMPARE(resets.count(), 0);
				append(3, 4097, 4097);
				QCoreApplication::sendPostedEvents(&browser, QEvent::MetaCall);
				versions->doItemsLayout();
				QCOMPARE(versions->currentIndex().data(Qt::UserRole).toString(), QStringLiteral("1:257"));
				QCOMPARE(versions->verticalScrollBar()->value(), pausedPosition);
				QCOMPARE(resets.count(), 0);
				pause->setChecked(true);
				const int frozenRows = versions->model()->rowCount();
				append(3, 4098, 4098);
				QCOMPARE(versions->model()->rowCount(), frozenRows);
				WorldRuntimeTestAccess::evictGmcpHistory(runtime, now.addSecs(7200));
				pause->setChecked(false);
				QCOMPARE(versions->currentIndex().row(), 0);
				QCOMPARE(versions->currentIndex().data(Qt::UserRole).toString(), QStringLiteral("3:4098"));
			}

			/**
			 * @brief Checks five-minute scheduling, timestamp expiration, the protected latest five, and
			 * absence of size/count limits.
			 */
			static void historyRetention()
			{
				WorldRuntime runtime;
				const auto   now = QDateTime::fromString(QStringLiteral("2026-01-02T12:00:00Z"), Qt::ISODate);
				auto        *timer = runtime.findChild<QTimer *>(QStringLiteral("gmcpHistoryEviction"));
				QVERIFY(timer && timer->isActive());
				QCOMPARE(timer->interval(), 300000);
				quint64 sequence = 0;
				auto    append   = [&](const QDateTime &time, const QByteArray &raw = {})
				{
					QMudGmcp::Record record;
					record.time       = time;
					record.generation = 1;
					record.sequence   = ++sequence;
					record.raw        = raw;
					WorldRuntimeTestAccess::deliverGmcpRecord(runtime, record, {});
				};
				for (int i = 0; i < 3; ++i)
					append(now.addSecs(-5400));
				WorldRuntimeTestAccess::evictGmcpHistory(runtime, now);
				QCOMPARE(runtime.gmcpHistory().size(), 3);
				for (int i = 0; i < 2; ++i)
					append(now.addSecs(-5400));
				WorldRuntimeTestAccess::evictGmcpHistory(runtime, now);
				QCOMPARE(runtime.gmcpHistory().size(), 5);
				append(now.addSecs(-5400));
				WorldRuntimeTestAccess::evictGmcpHistory(runtime, now);
				QCOMPARE(runtime.gmcpHistory().size(), 5);
				QCOMPARE(runtime.gmcpHistory().first().sequence, static_cast<quint64>(2));
				append(now.addSecs(-3600));
				append(now.addSecs(-60));
				append(now.addSecs(-5400));
				for (int i = 0; i < 5; ++i)
					append(now);
				WorldRuntimeTestAccess::evictGmcpHistory(runtime, now);
				QCOMPARE(runtime.gmcpHistory().size(), 7);
				QCOMPARE(runtime.gmcpHistory().at(0).sequence, static_cast<quint64>(7));
				QCOMPARE(runtime.gmcpHistory().at(1).sequence, static_cast<quint64>(8));
				append(now, QByteArray(9 * 1024 * 1024, 'x'));
				for (int i = 0; i < 2100; ++i)
					append(now);
				QCOMPARE(runtime.gmcpHistory().size(), 2108);
				WorldRuntimeTestAccess::evictGmcpHistory(runtime, now);
				QCOMPARE(runtime.gmcpHistory().size(), 2108);
				WorldRuntimeTestAccess::evictGmcpHistory(runtime, now.addSecs(3601));
				QCOMPARE(runtime.gmcpHistory().size(), 5);
				QCOMPARE(runtime.gmcpHistory().last().sequence, sequence);
			}

			/**
			 * @brief Preserves the existing public MCCP version numbering in native mode.
			 */
			static void compressionVersion()
			{
				for (const int version : {1, 2})
				{
					WorldRuntime runtime;
					runtime.setWorldAttribute(QStringLiteral("native_gmcp"), QStringLiteral("1"));
					QTcpServer server;
					QVERIFY(server.listen(QHostAddress::LocalHost, 0));
					QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
					QTRY_VERIFY(runtime.isConnected() && server.hasPendingConnections());
					auto      *peer   = server.nextPendingConnection();
					const auto marker = QByteArray::fromHex(version == 1 ? "fffa55fbf0" : "fffa56fff0");
					QCOMPARE(peer->write(marker), marker.size());
					peer->flush();
					QTRY_VERIFY(runtime.isCompressing());
					QCOMPARE(runtime.mccpType(), version);
					runtime.disconnectFromWorld();
					QTRY_VERIFY(!runtime.isConnected());
					QTRY_COMPARE(peer->state(), QAbstractSocket::UnconnectedState);
					delete peer;
				}
			}

			/**
			 * @brief Preserves buffered and later input when a reload capture is canceled.
			 */
			void failedReloadResumes()
			{
				QTcpServer server;
				QVERIFY2(server.listen(QHostAddress::LocalHost, 0), qPrintable(server.errorString()));
				WorldSocket                   socket;
				WorldSocketConnectionSettings settings;
				settings.nativeGmcp = true;
				socket.applyConnectionSettings(settings);
				QVERIFY(socket.connectToHost(QStringLiteral("127.0.0.1"), server.serverPort()));
				QTRY_VERIFY(server.hasPendingConnections());
				auto *peer = server.nextPendingConnection();
				QVERIFY(peer);
				peer->write(QByteArray::fromHex("fffbc9"));
				peer->flush();
				const auto publication = socket.gmcpPublication();
				QTRY_VERIFY(publication->current.load()->negotiated);
				QList<QByteArray> received;
				connect(&socket, &WorldSocketService::rawDataReceived, this,
				        [&](const QByteArray &bytes) { received.push_back(bytes); });
				const auto frame = QMudGmcp::frame("Test.Reload {\"value\":42}");
				peer->write("before" + frame.left(10));
				peer->flush();
				QTRY_COMPARE(QByteArrayList(received).join(), QByteArray("before"));
				socket.freezeForReload();
				QTRY_VERIFY(socket.reloadInputDrained());
				QVERIFY(socket.flushForReload());
				QVERIFY(socket.saveNativeState().isEmpty());
				peer->write(frame.mid(10) + "after");
				peer->flush();
				QVERIFY(socket.isConnected());
				socket.pauseTransport(false);
				QTRY_VERIFY(publication->current.load()->messages.contains(QStringLiteral("test.reload")));
				QTRY_COMPARE(QByteArrayList(received).join(), QByteArray("beforeafter"));
			}

#ifdef Q_OS_UNIX
			/**
			 * @brief Restores actual GMCP state on an inherited plain socket without another handshake.
			 */
			void inheritedTransportContinues()
			{
				QTcpServer server;
				QVERIFY2(server.listen(QHostAddress::LocalHost, 0), qPrintable(server.errorString()));
				WorldSocket                   original;
				WorldSocketConnectionSettings settings;
				settings.nativeGmcp = true;
				original.applyConnectionSettings(settings);
				QVERIFY(original.connectToHost(QStringLiteral("127.0.0.1"), server.serverPort()));
				QTRY_VERIFY(server.hasPendingConnections());
				auto *peer = server.nextPendingConnection();
				QVERIFY(peer);
				peer->write(QByteArray::fromHex("fffbc9"));
				peer->flush();
				QTRY_VERIFY(original.gmcpPublication()->current.load()->negotiated);
				QByteArray before;
				connect(&original, &WorldSocketService::rawDataReceived, this,
				        [&](const QByteArray &bytes) { before += bytes; });
				const auto message = QMudGmcp::frame("Test.Inherited {\"value\":19}");
				peer->write("prefix" + message);
				peer->flush();
				QTRY_COMPARE(before, QByteArray("prefix"));
				original.freezeForReload();
				QTRY_VERIFY(original.reloadInputDrained());
				QVERIFY(original.flushForReload());
				const auto saved = original.saveNativeState();
				QVERIFY(!saved.isEmpty());
				const int inherited = ::dup(static_cast<int>(original.nativeSocketDescriptor()));
				QVERIFY(inherited >= 0);
				WorldSocket restored;
				QByteArray  after;
				connect(&restored, &WorldSocketService::rawDataReceived, this,
				        [&](const QByteArray &bytes) { after += bytes; });
				QString error;
				QVERIFY2(restored.adoptNativeSocketDescriptor(inherited, saved, &error), qPrintable(error));
				QVERIFY(restored.gmcpPublication()->current.load()->negotiated);
				peer->write("suffix");
				peer->flush();
				QTRY_VERIFY(restored.gmcpPublication()->current.load()->messages.contains(
				    QStringLiteral("test.inherited")));
				QTRY_COMPARE(after, QByteArray("suffix"));
				QCOMPARE(restored.gmcpPublication()
				             ->current.load()
				             ->messages.value(QStringLiteral("test.inherited"))
				             ->state.toObject()
				             .value(QStringLiteral("value"))
				             .toInt(),
				         19);
			}
#endif

			/**
			 * @brief Proves reception/publication continues while the runtime thread is blocked.
			 */
			static void transportPublishesWhileMainThreadWaits()
			{
				QSemaphore publicationReady;
				QThread    worker;
				auto      *transport   = new WorldSocketTransport;
				const auto publication = transport->gmcpPublication();
				transport->moveToThread(&worker);
				worker.start();
				const auto cleanup = qScopeGuard(
				    [&]
				    {
					    QMetaObject::invokeMethod(
					        transport, [&] { delete transport; }, Qt::BlockingQueuedConnection);
					    worker.quit();
					    worker.wait();
				    });
				QObject::connect(
				    transport, &WorldSocketService::gmcpObserved, transport,
				    [&](const QMudGmcp::Record &, const std::shared_ptr<const QMudGmcp::Message> &message)
				    {
					    if (message && message->name == QLatin1String("Test.State"))
						    publicationReady.release();
				    },
				    Qt::DirectConnection);
				QTcpServer server;
				QVERIFY2(server.listen(QHostAddress::LocalHost, 0), qPrintable(server.errorString()));
				bool started = false;
				QMetaObject::invokeMethod(
				    transport,
				    [&]
				    {
					    WorldSocketConnectionSettings settings;
					    settings.nativeGmcp = true;
					    transport->applyConnectionSettings(settings);
					    transport->prepareGeneration(1);
					    started = transport->connectToHost(QStringLiteral("127.0.0.1"), server.serverPort());
				    },
				    Qt::BlockingQueuedConnection);
				QVERIFY(started);
				QTRY_VERIFY(server.hasPendingConnections());
				auto *peer = server.nextPendingConnection();
				QVERIFY(peer);
				peer->write(QByteArray::fromHex("fffbc9") + QMudGmcp::frame("Test.State {\"value\":73}"));
				peer->flush();
				QVERIFY2(publicationReady.tryAcquire(1, 5000),
				         "Native reception depended on the blocked runtime event loop");
				const auto state = publication->current.load(std::memory_order_acquire);
				QVERIFY(state->negotiated);
				QCOMPARE(state->messages.value(QStringLiteral("test.state"))
				             ->state.toObject()
				             .value(QStringLiteral("value"))
				             .toInt(),
				         73);
			}
#ifdef QMUD_ENABLE_LUA_SCRIPTING
			/**
			 * @brief Rejects assertion, runtime and syntax failures without reusing stale completion.
			 */
			static void assertionHelperReportsFailures()
			{
				WorldRuntime runtime;
				QString      error;
				QVERIFY(run(runtime, QStringLiteral("assert(true)"), &error));
				QVERIFY(error.isEmpty());
				QVERIFY(!run(runtime, QStringLiteral("assert(false)"), &error));
				QVERIFY(error.contains(QStringLiteral("assertion failed")));
				QVERIFY(!run(runtime, QStringLiteral("error('synthetic runtime failure')"), &error));
				QVERIFY(error.contains(QStringLiteral("synthetic runtime failure")));
				runtime.setVariable(QStringLiteral("gmcp_test_completion"), QStringLiteral("passed"));
				QVERIFY(!run(runtime, QStringLiteral("local ="), &error));
				QVERIFY(!error.isEmpty());
				QString stale;
				QVERIFY(!runtime.findVariable(QStringLiteral("gmcp_test_completion"), stale));
				QVERIFY(run(runtime, QStringLiteral("return"), &error));
				QVERIFY(error.isEmpty());
			}

			/**
			 * @brief Preserves complete JSON keys in lookup, iteration, callbacks and retained views.
			 */
			static void embeddedNulKeys()
			{
				QTemporaryDir directory(QDir(QStringLiteral(QMUD_TEST_BUILD_DIR))
				                            .filePath(QStringLiteral("worlds/gmcp-keys-XXXXXX")));
				QVERIFY(directory.isValid());
				const QString   id = QStringLiteral("131313131313131313131313");
				GmcpWireFixture session;
				session.write = [](const QByteArray &bytes) -> qint64 { return bytes.size(); };
				session.reset(true, 1, false);
				session.receive(QByteArray::fromHex("fffbc9"));
				std::shared_ptr<const QMudGmcp::Message> event;
				session.observed = [&](const auto &, const auto &message)
				{
					if (message)
						event = message;
				};
				session.receive(QMudGmcp::frame(
				    R"(Test.Keys {"a\u0000b":1,"a":2,"\u0000":3,"":4,"snow-\u96ea":5,"nested":{"\u0000\u0000":6}})"));
				WorldRuntime runtime;
				WorldView    view;
				view.setRuntime(&runtime);
				const auto detach = qScopeGuard([&] { view.setRuntime(nullptr); });
				view.show();
				QVERIFY(QTest::qWaitForWindowExposed(&view));
				runtime.setWorldAttribute(QStringLiteral("native_gmcp"), QStringLiteral("1"));
				WorldRuntimeTestAccess::setGmcpPublication(runtime, session.publication());
				QVERIFY(run(runtime, QStringLiteral(R"lua(
            savedKeys=gmcpGet('Test.Keys')
            local z=string.char(0)
            assert(savedKeys['a'..z..'b']==1 and savedKeys.a==2)
            assert(savedKeys[z]==3 and savedKeys['']==4 and savedKeys.nested[z..z]==6)
            local seen={}; local n=0
            for k,v in pairs(savedKeys) do seen[k]=v; n=n+1 end
            assert(n==6 and seen['a'..z..'b']==1 and seen.a==2)
            assert(seen[z]==3 and seen['']==4 and seen['snow-雪']==5)
            assert(gmcp.Test.Keys['a'..z..'b']==1)
            assert(gmcpGet('Test.Keys'..z..'suffix')==nil)
            assert(gmcpGetRaw('Test.Keys'..z..'suffix')==nil)
            assert(gmcp.Test['Keys'..z..'suffix']==nil)
        )lua")));
				QVERIFY(writeGmcpPlugin(directory.path(), QStringLiteral("keys"), id, QStringLiteral(R"lua(
function onPluginGmcpMessage(name,payload,state,raw)
 local z=string.char(0)
 assert(payload['a'..z..'b']==1 and state[z]==3)
 SetVariable('gmcp_keys_seen','yes')
end
)lua")));
				runtime.setStartupDirectory(QStringLiteral(QMUD_TEST_BUILD_DIR));
				runtime.setPluginsDirectory(directory.path());
				QString loadError;
				QVERIFY2(runtime.loadPluginFile(QStringLiteral("keys.xml"), &loadError),
				         qPrintable(loadError));
				QTRY_VERIFY(!runtime.pluginForId(id)->installPending);
				LuaBatchDispatchRequest request;
				request.kind           = LuaBatchDispatchKind::GmcpMessage;
				request.callbackEvent  = LuaCallbackEvent::GmcpMessage;
				request.functionName   = QStringLiteral("onPluginGmcpMessage");
				request.engines        = {runtime.pluginForId(id)->lua};
				request.gmcpMessageArg = event;
				static_cast<void>(
				    WorldRuntimeTestAccess::queuePluginCallbackDispatchResult(runtime, request));
				QCOMPARE(runtime.pluginVariableValue(id, QStringLiteral("gmcp_keys_seen")),
				         QStringLiteral("yes"));
				session.receive(QMudGmcp::frame(R"(Test.Keys {"a\u0000b":9})"));
				QVERIFY(
				    run(runtime, QStringLiteral("local k='a'..string.char(0)..'b'; assert(savedKeys[k]==1 "
				                                "and gmcpGet('Test.Keys')[k]==9)")));
				session.closeConnection();
				QVERIFY(run(runtime,
				            QStringLiteral("assert(savedKeys['a'..string.char(0)..'b']==1 and gmcp==nil); "
				                           "local ok,err=pcall(gmcpGet,'Test.Keys'); assert(not ok and "
				                           "err:find('GMCP is not available',1,true))")));
				runtime.setWorldFileModified(false);
			}

			/**
			 * @brief Prevents NUL-containing module arguments from modifying a valid prefix registration.
			 */
			static void moduleNamesRejectNulAliases()
			{
				GmcpWireFixture session;
				session.reset(true, 1, false);
				WorldRuntime runtime;
				WorldRuntimeTestAccess::setGmcpPublication(runtime, session.publication());
				runtime.setWorldAttribute(QStringLiteral("native_gmcp"), QStringLiteral("1"));
				QVERIFY(run(runtime, QStringLiteral("assert(gmcpRegisterModule('Test.Feature',2)==0)")));
				QVERIFY(
				    run(runtime,
				        QStringLiteral(
				            "assert(gmcpRegisterModule('Test.Feature'..string.char(0)..'suffix',1)~=0)")));
				QCOMPARE(
				    WorldRuntimeTestAccess::gmcpModuleVersion(runtime, {}, QStringLiteral("Test.Feature")),
				    2);
				QVERIFY(
				    run(runtime,
				        QStringLiteral(
				            "assert(gmcpUnregisterModule('Test.Feature'..string.char(0)..'suffix')~=0)")));
				QCOMPARE(
				    WorldRuntimeTestAccess::gmcpModuleVersion(runtime, {}, QStringLiteral("Test.Feature")),
				    2);
			}

			/**
			 * @brief Releases only the resetting state's registrations before loading replacement script.
			 */
			static void moduleRegistrationLifecycle()
			{
				GmcpWireFixture session;
				session.reset(true, 1, false);
				WorldRuntime runtime;
				WorldRuntimeTestAccess::setGmcpPublication(runtime, session.publication());
				runtime.setWorldAttribute(QStringLiteral("native_gmcp"), QStringLiteral("1"));
				QVERIFY(run(runtime, QStringLiteral("assert(gmcpRegisterModule('Test.Feature',2)==0)")));
				QCOMPARE(
				    WorldRuntimeTestAccess::gmcpModuleVersion(runtime, {}, QStringLiteral("Test.Feature")),
				    2);
				QVERIFY(WorldRuntimeTestAccess::dispatchLuaResetAndLoadScript(
				    runtime,
				    QSharedPointer<LuaCallbackEngine>(runtime.luaCallbacks(), [](LuaCallbackEngine *){})));
				QCOMPARE(
				    WorldRuntimeTestAccess::gmcpModuleVersion(runtime, {}, QStringLiteral("Test.Feature")),
				    0);
				QVERIFY(run(runtime, QStringLiteral("assert(gmcpRegisterModule('Test.Feature',3)==0)")));
				QVERIFY(run(runtime, QStringLiteral("assert(gmcpUnregisterModule('Test.Feature')==0)")));
				QCOMPARE(
				    WorldRuntimeTestAccess::gmcpModuleVersion(runtime, {}, QStringLiteral("Test.Feature")),
				    0);
			}

			/**
			 * @brief Enumerates immediate namespace children with mixed case, overlapping paths, and every
			 * payload kind while unrelated packages remain present.
			 */
			static void luaNamespaceIteration()
			{
				GmcpWireFixture session;
				session.write = [](const QByteArray &bytes) -> qint64 { return bytes.size(); };
				session.reset(true, 1, false);
				session.receive(QByteArray::fromHex("fffbc9"));
				const QList<QByteArray> messages{R"(Test.Vitals {"hp":10,"HP":11})",
				                                 "Test.Deep.Value 1",
				                                 "Test.Deep.Other 2",
				                                 R"(Test.Parent {"value":3})",
				                                 "Test.Parent.Child 4",
				                                 "Test.Absent",
				                                 "Test.Absent.Child 5",
				                                 "Test.Null null",
				                                 "Test.False false",
				                                 "Test.Zero 0",
				                                 "Test.Text \"data\"",
				                                 "Test.EmptyObject {}",
				                                 "Test.Array [1,null,false]",
				                                 "Test.Mixed.Child 6",
				                                 "TEST.MIXED.CHILD 7",
				                                 "TestOther.Value 8"};
				for (const auto &message : messages)
					session.receive(QMudGmcp::frame(message));
				for (int i = 0; i < 1024; ++i)
					session.receive(QMudGmcp::frame("Other.Item" + QByteArray::number(i) + ".Value 9"));
				WorldRuntime runtime;
				WorldRuntimeTestAccess::setGmcpPublication(runtime, session.publication());
				QVERIFY(run(runtime, QStringLiteral(R"lua(
local function collect(view)
 local result,count={},0
 for key,value in pairs(view) do
  key=key:lower()
  assert(result[key]==nil)
  result[key]=value
  count=count+1
 end
 return result,count
end
local roots,n=collect(gmcp)
assert(n==3 and roots.test and roots.other and roots.testother)
for key in pairs(gmcp) do assert(key=='Test' or key=='Other' or key=='TestOther') end
local values,count=collect(gmcp.tEsT)
assert(count==10)
assert(values.vitals.hp==10 and values.vitals.HP==11)
assert(values.parent.value==3 and values.parent.Child==nil)
assert(gmcpGet('test.parent.child')==4)
assert(values.absent==nil and gmcpGet('test.absent.child')==5)
assert(values.null==gmcpNull and values['false']==false and values.zero==0)
assert(values.text=='data')
assert(#values.array==3 and values.array[1]==1 and values.array[2]==gmcpNull and values.array[3]==false)
local empty,emptyCount=collect(values.emptyobject)
assert(emptyCount==0 and next(empty)==nil)
local deep,deepCount=collect(values.deep)
assert(deepCount==2 and deep.value==1 and deep.other==2)
local mixedCount=0
for key,value in pairs(gmcp.test.mixed) do
 assert(key=='CHILD' and value==7)
 mixedCount=mixedCount+1
end
assert(mixedCount==1)
assert(not pcall(function() roots.test.New=1 end))
)lua")));
			}

			/**
			 * @brief Keeps namespace iterators on their captured version across updates, restoration, and
			 * reconnects, then releases that version when the Lua state resets.
			 */
			static void luaNamespaceIterationSnapshots()
			{
				GmcpWireFixture session;
				session.write = [](const QByteArray &bytes) -> qint64 { return bytes.size(); };
				session.reset(true, 1, false);
				session.receive(QByteArray::fromHex("fffbc9"));
				session.receive(QMudGmcp::frame("Test.Branch.Value 1"));
				session.receive(QMudGmcp::frame("Test.First 1"));
				session.receive(QMudGmcp::frame("Other.Value 0"));
				std::weak_ptr<const QMudGmcp::Snapshot> original = session.publication()->current.load();
				WorldRuntime                            runtime;
				WorldRuntimeTestAccess::setGmcpPublication(runtime, session.publication());
				QVERIFY(run(runtime, QStringLiteral(R"lua(
function collectGmcpNamespace(view)
 local result,count={},0
 for key,value in pairs(view) do result[key:lower()]=value; count=count+1 end
 return result,count
end
rootIterator,rootState,rootControl=pairs(gmcp)
branchIterator,branchState,branchControl=pairs(gmcp.Test.Branch)
liveNamespace=gmcp.Test
)lua")));
				session.receive(QMudGmcp::frame("Test.Branch.Value 2"));
				session.receive(QMudGmcp::frame("Test.Added 3"));
				session.receive(QMudGmcp::frame("NewRoot.Value 4"));
				QVERIFY(!original.expired());
				QVERIFY(run(runtime, QStringLiteral(R"lua(
local roots,count={},0
for key,value in rootIterator,rootState,rootControl do roots[key:lower()]=value; count=count+1 end
assert(count==2 and roots.test and roots.other and roots.newroot==nil)
savedNamespace=roots.test
assert(savedNamespace.Branch.Value==1 and savedNamespace.Added==nil)
local old,oldCount=collectGmcpNamespace(savedNamespace)
assert(oldCount==2 and old.branch.Value==1 and old.first==1)
local branchCount=0
for key,value in branchIterator,branchState,branchControl do
 assert(key=='Value' and value==1); branchCount=branchCount+1
end
assert(branchCount==1)
local fresh,freshCount=collectGmcpNamespace(liveNamespace)
assert(freshCount==3 and fresh.branch.Value==2 and fresh.added==3)
)lua")));
				const auto saved = session.saveConnection();
				QVERIFY(!saved.isEmpty());
				session.closeConnection();
				QVERIFY(run(runtime, QStringLiteral(R"lua(
assert(gmcp==nil and savedNamespace.Branch.Value==1)
local empty,n=collectGmcpNamespace(liveNamespace)
assert(n==0 and next(empty)==nil)
)lua")));
				QVERIFY(session.restoreConnection(saved, 2));
				QVERIFY(run(runtime, QStringLiteral(R"lua(
local roots,n=collectGmcpNamespace(gmcp)
assert(n==3 and roots.test and roots.other and roots.newroot)
local restored,count=collectGmcpNamespace(gmcp.Test)
assert(count==3 and restored.branch.Value==2 and restored.added==3)
assert(savedNamespace.Branch.Value==1)
)lua")));
				session.reset(true, 3, false);
				session.receive(QByteArray::fromHex("fffbc9"));
				session.receive(QMudGmcp::frame("Test.Fresh false"));
				QVERIFY(run(runtime, QStringLiteral(R"lua(
local current,n=collectGmcpNamespace(liveNamespace)
assert(n==1 and current.fresh==false and current.branch==nil)
assert(savedNamespace.Branch.Value==1)
)lua")));
				QVERIFY(WorldRuntimeTestAccess::dispatchLuaResetAndLoadScript(
				    runtime,
				    QSharedPointer<LuaCallbackEngine>(runtime.luaCallbacks(), [](LuaCallbackEngine *){})));
				QVERIFY(original.expired());
			}

			/**
			 * @brief Reads live publications through Lua without mutating previously obtained values.
			 */
			static void luaViews()
			{
				GmcpWireFixture session;
				session.write = [](const QByteArray &bytes) -> qint64 { return bytes.size(); };
				session.reset(true, 1, false);
				session.receive(QByteArray::fromHex("fffbc9"));
				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("native_gmcp"), QStringLiteral("1"));
				WorldRuntimeTestAccess::setGmcpPublication(runtime, session.publication());
				session.receive(
				    QMudGmcp::frame(R"(Test.State {"value":1,"array":[true,null,3],"empty":{}})"));
				QVERIFY(run(runtime, QStringLiteral(R"lua(
            assert(gmcpIsEnabled())
            saved=gmcp.Test.State
            assert(saved.value==1 and gmcpGet("test.STATE").value==1)
            assert(#saved.array==3 and saved.array[1]==true and saved.array[2]==gmcpNull)
            local count=0; for k,v in pairs(saved.array) do count=count+1 end; assert(count==3)
            assert(not pcall(function() saved.value=42 end))
            assert(not pcall(function() saved.array[1]=false end))
            assert(not pcall(function() rawset(saved,"value",42) end))
            assert(gmcpGetRaw("Test.State"):find('"value":1',1,true))
            assert(#gmcpGetMessages()==1)
            assert(gmcpEnableMerge==nil and gmcpEnableMergeR==nil)
        )lua")));
				session.receive(QMudGmcp::frame("Test.State {\"value\":2}"));
				QVERIFY(run(
				    runtime,
				    QStringLiteral(
				        "assert(saved.value==1 and gmcp.Test.State.value==2 and gmcpGet('missing')==nil)")));
				session.closeConnection();
				QVERIFY(run(
				    runtime,
				    QStringLiteral("assert(gmcp==nil and saved.value==1); local ok,err=pcall(gmcpIsEnabled); "
				                   "assert(not ok and err:find('GMCP is not available',1,true))")));
			}
			/**
			 * @brief Exercises retained-value lookup, Lua serialization, independent table copies, and scalar wrapping.
			 */
			static void luaNativeUtilities()
			{
				GmcpWireFixture session;
				session.write = [](const QByteArray &bytes) -> qint64 { return bytes.size(); };
				session.reset(true, 1, false);
				session.receive(QByteArray::fromHex("fffbc9"));
				const QString controls = QStringLiteral("quote\" slash\\ newline\nreturn\rtab\tzero") +
				                         QChar(0) + QStringLiteral("7del") + QChar(127) +
				                         QStringLiteral("雪");
				QJsonObject   values;
				values.insert(QStringLiteral("nested"), QJsonObject{
				                                            {QStringLiteral("value"), 7               },
				                                            {QStringLiteral("null"),  QJsonValue::Null},
				                                            {QStringLiteral("false"), false           },
				                                            {QStringLiteral("zero"),  0               }
                });
				values.insert(QStringLiteral("array"), QJsonArray{QJsonObject{
				                                                      { QStringLiteral("value"),
					                                                    QStringLiteral("first") }},
				                                                  QJsonValue::Null, false, 3});
				const QJsonObject dotted{
				    { QString(),
                     QJsonObject {
						  { QStringLiteral("1"),
							QStringLiteral("numeric-key") }
					  } }
                };
				values.insert(QStringLiteral("a.b"), dotted);
				values.insert(QStringLiteral("controls"), controls);
				values.insert(QStringLiteral("keys"),
				              QJsonObject{
				                  {QStringLiteral("a") + QChar(0) + QStringLiteral("b"), 9},
				                  {QStringLiteral("end"),
                                   QStringLiteral("reserved-key")                         }
                });
				values.insert(QStringLiteral("integer"), qint64{9007199254740993});
				values.insert(QStringLiteral("fraction"), 0.125);
				values.insert(QStringLiteral("emptyObject"), QJsonObject{});
				values.insert(QStringLiteral("emptyArray"), QJsonArray{});
				values.insert(QStringLiteral("inert"), QStringLiteral("\"; error('must remain text') --"));
				session.receive(QMudGmcp::frame("Test.Utilities " +
				                                QJsonDocument(values).toJson(QJsonDocument::Compact)));
				WorldRuntime runtime;
				WorldRuntimeTestAccess::setGmcpPublication(runtime, session.publication());
				QString error;
				QVERIFY2(run(runtime, QStringLiteral(R"lua(
local root=gmcpGet('tEsT.uTiLiTiEs')
savedUtilities=root
assert(gmcpGet('Test.Utilities','nested.value')==7)
assert(gmcpGet(root,{'nested','value'})==7 and gmcpGet(root,'Nested.value')==nil)
assert(gmcpGet(root,{'a.b','','1'})=='numeric-key')
assert(gmcpGet(root,'array.1.value')=='first' and gmcpGet(root,{'array',1,'value'})=='first')
assert(gmcpGet(root,{'array',5},'absent')=='absent')
assert(gmcpGet(root,'array.0')==nil and gmcpGet(root,'array.1.5')==nil)
assert(gmcpGet(root,'nested.value.child','absent')=='absent')
local marker={}
assert(gmcpGet(root,'missing',marker)==marker and gmcpGet(nil,'value',false)==false)
assert(gmcpGet(root,'nested.null',marker)==gmcpNull)
assert(gmcpGet(root,'nested.false',true)==false and gmcpGet(root,'nested.zero',1)==0)
assert(gmcpGet(root)==root or gmcpGet(root).nested.value==7)
assert(gmcpGet(root,'').nested.value==7 and gmcpGet(root,{}).nested.value==7)
local zero=string.char(0)
assert(gmcpGet(root,{'keys','a'..zero..'b'})==9)
assert(gmcpGet('Test.Utilities'..zero..'suffix','nested.value','absent')=='absent')
assert(gmcpGetString(root,'nested.value')=='7' and gmcpGetString(root,'nested.false')=='false')
assert(gmcpGetString(root,'nested.zero')=='0' and gmcpGetString(root,'integer')=='9007199254740993')
assert(gmcpGetString(root,'fraction')=='0.125')
assert(gmcpGetString(root,'nested.null')=='' and gmcpGetString(root,'missing')=='')
assert(gmcpGetString(root,'nested.null','fallback')=='fallback')
assert(gmcpGetString(root,'missing','a'..zero..'b')=='a'..zero..'b')
assert(gmcpGetString(root,'controls')==root.controls)
local serialized=gmcpGetString(root)
assert(serialized:sub(1,1)=='{')
local decoded=assert(loadstring('return '..serialized))()
assert(type(decoded)=='table' and decoded.nested.value==7 and decoded.nested.null==gmcpNull)
assert(decoded.nested['false']==false and decoded.nested.zero==0 and decoded.integer==9007199254740993)
assert(decoded.controls==root.controls and decoded.inert==root.inert and decoded.keys['a'..zero..'b']==9)
assert(decoded.keys['end']=='reserved-key' and decoded.array[2]==gmcpNull and decoded.array[3]==false)
local copy=gmcpToTable(root)
assert(type(copy)=='table' and type(copy.nested)=='table' and type(copy.array)=='table')
assert(#copy.array==4 and copy.array[2]==gmcpNull and copy.array[3]==false)
assert(type(copy.emptyObject)=='table' and next(copy.emptyObject)==nil)
assert(type(copy.emptyArray)=='table' and #copy.emptyArray==0)
copy.nested.value=99; copy.array[1].value='changed'
assert(root.nested.value==7 and root.array[1].value=='first')
local wrapped=gmcpToTable(root,true)
assert(wrapped.nested.value==7 and wrapped.nested['false']==false and wrapped[1]==nil)
local array=gmcpToTable(root.array,true)
assert(#array==4 and array[1].value=='first' and array[2]==gmcpNull)
for _,v in ipairs({'example',7,false,gmcpNull}) do
 assert(gmcpToTable(v)==v)
 local scalar=gmcpToTable(v,true)
 assert(type(scalar)=='table' and #scalar==1 and scalar[1]==v)
end
assert(gmcpToTable(nil)==nil and gmcpToTable(nil,true)==nil)
assert(gmcpType(root)=='object' and gmcpType(root.array)=='array')
assert(gmcpType(root.emptyObject)=='object' and gmcpType(root.emptyArray)=='array')
assert(gmcpType(gmcp)=='namespace' and gmcpType(gmcp.Test)=='namespace')
assert(gmcpType(gmcpNull)=='null' and gmcpType(nil)=='nil')
assert(gmcpType('text')=='string' and gmcpType(7)=='number' and gmcpType(false)=='boolean')
assert(gmcpType({})=='table' and gmcpType(function() end)=='function' and gmcpType(io.stdout)=='userdata')
assert(world.gmcpGetString('Test.Utilities','nested.value')=='7')
assert(world.gmcpToTable(root).nested.value==7 and world.gmcpType(root.array)=='array')
assert(utils.functionargs('gmcpGetString')~=nil and utils.functionargs('gmcpToTable')~=nil)
)lua"),
				             &error),
				         qPrintable(error));
				session.receive(QMudGmcp::frame(R"(Test.Utilities {"nested":{"value":8}})"));
				QVERIFY2(run(runtime, QStringLiteral(R"lua(
assert(gmcpGet(savedUtilities,'nested.value')==7)
assert(gmcpGetString(savedUtilities,'nested.value')=='7')
assert(gmcpToTable(savedUtilities).nested.value==7)
assert(gmcpGet('Test.Utilities','nested.value')==8)
)lua"),
				             &error),
				         qPrintable(error));
				runtime.setWorldFileModified(false);
			}

			/**
			 * @brief Resolves target-world names while keeping supplied values and globals owned by the caller.
			 */
			static void luaNativeUtilitiesAcrossWorlds()
			{
				AppController controller;
				MainWindow    frame;
				controller.setMainWindow(&frame);
				WorldRuntime source;
				WorldRuntime target;
				source.setWorldAttribute(QStringLiteral("id"), QStringLiteral("303030303030303030303030"));
				target.setWorldAttribute(QStringLiteral("id"), QStringLiteral("313131313131313131313131"));
				source.setWorldAttribute(QStringLiteral("name"), QStringLiteral("UtilitySource"));
				target.setWorldAttribute(QStringLiteral("name"), QStringLiteral("UtilityTarget"));
				auto *sourceWindow = new WorldChildWindow(QStringLiteral("UtilitySource"));
				auto *targetWindow = new WorldChildWindow(QStringLiteral("UtilityTarget"));
				sourceWindow->setRuntime(&source);
				targetWindow->setRuntime(&target);
				frame.addMdiSubWindow(sourceWindow);
				frame.addMdiSubWindow(targetWindow);
				const auto detach = qScopeGuard(
				    [&]
				    {
					    sourceWindow->setRuntime(nullptr);
					    targetWindow->setRuntime(nullptr);
				    });
				GmcpWireFixture sourceSession;
				GmcpWireFixture targetSession;
				sourceSession.write =
				    targetSession.write = [](const QByteArray &bytes) -> qint64 { return bytes.size(); };
				sourceSession.reset(true, 1, false);
				targetSession.reset(true, 1, false);
				sourceSession.receive(QByteArray::fromHex("fffbc9"));
				targetSession.receive(QByteArray::fromHex("fffbc9"));
				sourceSession.receive(QMudGmcp::frame(R"(Test.State {"value":1})"));
				targetSession.receive(QMudGmcp::frame(R"(Test.State {"value":2,"array":[null,false]})"));
				WorldRuntimeTestAccess::setGmcpPublication(source, sourceSession.publication());
				WorldRuntimeTestAccess::setGmcpPublication(target, targetSession.publication());
				const auto completion = std::make_shared<std::optional<bool>>();
				source.dispatchLuaExecuteScriptAsync(
				    QSharedPointer<LuaCallbackEngine>(source.luaCallbacks(), [](LuaCallbackEngine *){}),
				    QStringLiteral(R"lua(
local other=assert(GetWorld('UtilityTarget'))
local own=gmcpGet('Test.State')
assert(other:gmcpGet('Test.State','value')==2 and gmcpGet('Test.State','value')==1)
assert(other:gmcpGet(own,'value')==1)
assert(other:gmcpGetString('Test.State','value')=='2')
local value=other:gmcpGet('Test.State')
local copied=other:gmcpToTable(value)
assert(type(copied)=='table' and copied.value==2 and copied.array[1]==gmcpNull)
assert(other:gmcpType(value.array)=='array' and other:gmcpToTable(false,true)[1]==false)
local decoded=assert(loadstring('return '..other:gmcpGetString('Test.State')))()
assert(decoded.value==2 and decoded.array[1]==gmcpNull and decoded.array[2]==false)
assert(gmcp.Test.State.value==1)
)lua"),
				    QStringLiteral("synthetic native utility world proxy"), LuaCallbackEvent::None, nullptr,
				    false, false, -1, 0, [completion](bool succeeded) { *completion = succeeded; });
				QTRY_VERIFY(completion->has_value());
				QVERIFY(**completion);
				source.setWorldFileModified(false);
				target.setWorldFileModified(false);
			}

			/**
			 * @brief Ensures callback arguments retain their own event while global reads are live.
			 */
			static void notificationValues()
			{
				QTemporaryDir directory(QDir(QStringLiteral(QMUD_TEST_BUILD_DIR))
				                            .filePath(QStringLiteral("worlds/gmcp-values-XXXXXX")));
				QVERIFY(directory.isValid());
				const QString   id = QStringLiteral("141414141414141414141414");
				GmcpWireFixture session;
				session.write = [](const QByteArray &bytes) -> qint64 { return bytes.size(); };
				session.reset(true, 1, false);
				session.receive(QByteArray::fromHex("fffbc9"));
				std::shared_ptr<const QMudGmcp::Message> first;
				session.observed = [&](const QMudGmcp::Record &, const auto &message)
				{
					if (message && !first)
						first = message;
				};
				session.receive(QMudGmcp::frame("Test.State {\"value\":1}"));
				session.receive(QMudGmcp::frame("Test.State {\"value\":2}"));
				WorldRuntime runtime;
				WorldView    view;
				view.setRuntime(&runtime);
				const auto detach = qScopeGuard([&] { view.setRuntime(nullptr); });
				view.show();
				QVERIFY(QTest::qWaitForWindowExposed(&view));
				runtime.setWorldAttribute(QStringLiteral("native_gmcp"), QStringLiteral("1"));
				WorldRuntimeTestAccess::setGmcpPublication(runtime, session.publication());

				QVERIFY(writeGmcpPlugin(directory.path(), QStringLiteral("values"), id, QStringLiteral(R"lua(
function onPluginGmcpMessage(name,payload,state,raw)
 assert(payload.value==1 and state.value==1 and gmcpGet(name).value==2)
 assert(gmcpGet(payload,'value')==1 and gmcpGetString(state,'value')=='1')
 assert(gmcpToTable(payload).value==1 and gmcpType(payload)=='object')
 SetVariable('gmcp_seen','yes')
end
)lua")));
				runtime.setStartupDirectory(QStringLiteral(QMUD_TEST_BUILD_DIR));
				runtime.setPluginsDirectory(directory.path());
				QString loadError;
				QVERIFY2(runtime.loadPluginFile(QStringLiteral("values.xml"), &loadError),
				         qPrintable(loadError));
				QTRY_VERIFY(!runtime.pluginForId(id)->installPending);
				LuaBatchDispatchRequest request;
				request.kind           = LuaBatchDispatchKind::GmcpMessage;
				request.callbackEvent  = LuaCallbackEvent::GmcpMessage;
				request.functionName   = QStringLiteral("onPluginGmcpMessage");
				request.engines        = {runtime.pluginForId(id)->lua};
				request.gmcpMessageArg = first;
				static_cast<void>(
				    WorldRuntimeTestAccess::queuePluginCallbackDispatchResult(runtime, request));
				QCOMPARE(runtime.pluginVariableValue(id, QStringLiteral("gmcp_seen")), QStringLiteral("yes"));
				runtime.setWorldFileModified(false);
			}
#endif
	};
} // namespace
QTEST_MAIN(tst_GmcpIntegration)
#include "tst_GmcpIntegration.moc"
