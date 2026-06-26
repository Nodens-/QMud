/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldRuntime_PluginLifecycle.cpp
 * Role: Integration coverage for WorldRuntime plugin lifecycle callback ordering.
 */

#include "NativePluginRegistry.h"
#include "WorldChildWindow.h"
#include "WorldCommandProcessor.h"
#include "WorldCommandProcessorUtils.h"
#include "WorldDocument.h"
#include "WorldOptions.h"
#include "WorldRuntime.h"
#include "WorldView.h"
#include "scripting/ScriptingErrors.h"

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
	const QString           kDeferredConnectPluginId = QStringLiteral("abcdeffedcbaabcdeffedcba");
	const QString           kTeardownStatePluginId   = QStringLiteral("fedcbaabcdeffedcbaabcdef");
	const QString           kHiddenMessagePluginId   = QStringLiteral("112233445566778899aabbcc");
	const QString           kNestedCallPluginId      = QStringLiteral("2233445566778899aabbccdd");
	const QString           kTelnetOrderingPluginId  = QStringLiteral("00112233445566778899aabb");
	const QString           kTimerCommandPluginId    = QStringLiteral("33445566778899aabbccddee");
	const QString           kFocusCallbackPluginId   = QStringLiteral("445566778899aabbccddeeff");
	const QString           kTelnetTriggerLine       = QStringLiteral("qxv-lattice-17");
	const QString           kTelnetAfterLine         = QStringLiteral("qxv-after-64");

	constexpr unsigned char IAC  = 0xFF;
	constexpr unsigned char SB   = 0xFA;
	constexpr unsigned char SE   = 0xF0;
	constexpr unsigned char GMCP = 201;

	/**
	 * @brief Builds a byte array from unsigned byte literals.
	 * @param raw Raw byte values.
	 * @return Byte array containing the values.
	 */
	QByteArray              bytes(std::initializer_list<unsigned char> raw)
	{
		QByteArray out;
		for (const unsigned char c : raw)
			out.append(static_cast<char>(c));
		return out;
	}

	/**
	 * @brief Writes text to a test fixture file.
	 * @param path Destination file path.
	 * @param text Text to write.
	 * @return `true` when the file was written completely.
	 */
	bool writeTextFile(const QString &path, const QString &text)
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
	 * @brief Writes a plugin fixture with connect/disconnect lifecycle markers.
	 * @param pluginsDir Plugin fixture directory.
	 * @return `true` when the plugin fixture was written.
	 */
	bool writeHiddenMessageLifecyclePlugin(const QString &pluginsDir)
	{
		const QString pluginPath = QDir(pluginsDir).filePath(QStringLiteral("hidden_messages.xml"));
		return writeTextFile(pluginPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin
    name="HiddenMessagesLifecycle"
    author="QMud Test"
    id="112233445566778899aabbcc"
    language="lua"
    enabled="y"
    save_state="n"
    sequence="100">
    <script><![CDATA[
function OnPluginConnect()
  SetVariable("connect_marker", "connected")
end

function OnPluginDisconnect()
  SetVariable("disconnect_marker", "disconnected")
end
]]></script>
  </plugin>
</muclient>
)xml"));
	}

	/**
	 * @brief Writes a plugin fixture whose telnet subnegotiation callback sends a command.
	 * @param pluginsDir Plugin fixture directory.
	 * @param insertTriggerLineBeforeGmcp Whether packet receive should inject a trigger line before GMCP.
	 * @return `true` when the plugin fixture was written.
	 */
	bool writeTelnetOrderingPlugin(const QString &pluginsDir, const bool insertTriggerLineBeforeGmcp = false)
	{
		const QString pluginPath = QDir(pluginsDir).filePath(QStringLiteral("telnet_ordering.xml"));
		QString       script     = QStringLiteral(R"lua(
function qcb_append_order(marker)
  local current = GetVariable("callback_order") or ""
  if current ~= "" then
    current = current .. ","
  end
  SetVariable("callback_order", current .. marker)
end

function qcb_mark_trigger(arg)
  qcb_append_order("trigger")
end

function OnPluginTelnetSubnegotiation(msg_type, data)
  qcb_append_order("telnet")
  Send("qcmd-telnet-b73")
end
)lua");
		if (insertTriggerLineBeforeGmcp)
		{
			script += QStringLiteral(R"lua(

function OnPluginPacketReceived(packet)
  local gmcp = string.char(255, 250, 201)
  local transformed = string.gsub(packet, gmcp, "qxv-lattice-17\r\n" .. gmcp, 1)
  return transformed
end
)lua");
		}
		return writeTextFile(pluginPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin
    name="TelnetOrdering"
    author="QMud Test"
    id=")xml") + kTelnetOrderingPluginId + QStringLiteral(R"xml("
    language="lua"
    enabled="y"
    save_state="n"
    sequence="100">
)xml") + QStringLiteral("    <script><![CDATA[") +
		                                     script + QStringLiteral("]]></script>\n") +
		                                     QStringLiteral(R"xml(  </plugin>
</muclient>
)xml"));
	}

	/**
	 * @brief Writes a plugin fixture whose routine sends a command.
	 * @param pluginsDir Plugin fixture directory.
	 * @return `true` when the plugin fixture was written.
	 */
	bool writeNestedCallPluginSendPlugin(const QString &pluginsDir)
	{
		const QString pluginPath = QDir(pluginsDir).filePath(QStringLiteral("nested_call_send.xml"));
		return writeTextFile(pluginPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin
    name="NestedCallSend"
    author="QMud Test"
    id=")xml") + kNestedCallPluginId + QStringLiteral(R"xml("
    language="lua"
    enabled="y"
    save_state="n"
    sequence="100">
    <script><![CDATA[
function qcb_nested_priority_check(arg)
  Send("qcmd-nested-p54")
end
]]></script>
  </plugin>
</muclient>
)xml"));
	}

	/**
	 * @brief Writes a plugin fixture that records command callbacks.
	 * @param pluginsDir Plugin fixture directory.
	 * @return `true` when the plugin fixture was written.
	 */
	bool writeTimerCommandRecorderPlugin(const QString &pluginsDir)
	{
		const QString pluginPath = QDir(pluginsDir).filePath(QStringLiteral("timer_command_recorder.xml"));
		return writeTextFile(pluginPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin
    name="TimerCommandRecorder"
    author="QMud Test"
    id=")xml") + kTimerCommandPluginId + QStringLiteral(R"xml("
    language="lua"
    enabled="y"
    save_state="n"
    sequence="100">
    <script><![CDATA[
function OnPluginCommand(command)
  local current = GetVariable("timer_commands") or ""
  if current ~= "" then
    current = current .. ","
  end
  SetVariable("timer_commands", current .. command)
  return false
end
]]></script>
  </plugin>
</muclient>
)xml"));
	}

	/**
	 * @brief Writes a plugin fixture that records focus lifecycle callback order.
	 * @param pluginsDir Plugin fixture directory.
	 * @return `true` when the plugin fixture was written.
	 */
	bool writeFocusCallbackPlugin(const QString &pluginsDir)
	{
		const QString pluginPath = QDir(pluginsDir).filePath(QStringLiteral("focus_callbacks.xml"));
		return writeTextFile(pluginPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin
    name="FocusCallbacks"
    author="QMud Test"
    id=")xml") + kFocusCallbackPluginId + QStringLiteral(R"xml("
    language="lua"
    enabled="y"
    save_state="n"
    sequence="100">
    <script><![CDATA[
function qcb_append_plugin_focus(marker)
  local current = GetVariable("plugin_focus_order") or ""
  if current ~= "" then
    current = current .. ","
  end
  SetVariable("plugin_focus_order", current .. marker)

  local states = GetVariable("plugin_focus_active_values") or ""
  if states ~= "" then
    states = states .. ","
  end
  SetVariable("plugin_focus_active_values", states .. tostring(GetInfo(113)))

  local sources = GetVariable("plugin_focus_action_sources") or ""
  if sources ~= "" then
    sources = sources .. ","
  end
  SetVariable("plugin_focus_action_sources", sources .. string.format("%.0f", GetInfo(239)))
end

function OnPluginGetFocus()
  qcb_append_plugin_focus("plugin_get")
end

function OnPluginLoseFocus()
  qcb_append_plugin_focus("plugin_lose")
end
]]></script>
  </plugin>
</muclient>
)xml"));
	}

	/**
	 * @brief Creates a script trigger that sends a command when matched text arrives.
	 * @param match Trigger match text.
	 * @return Trigger fixture.
	 */
	WorldRuntime::Trigger makeTelnetOrderingTrigger(const QString &match = kTelnetTriggerLine)
	{
		WorldRuntime::Trigger trigger;
		trigger.attributes.insert(QStringLiteral("enabled"), QStringLiteral("y"));
		trigger.attributes.insert(QStringLiteral("match"), match);
		trigger.attributes.insert(QStringLiteral("send_to"), QString::number(eSendToScript));
		trigger.attributes.insert(QStringLiteral("sequence"), QStringLiteral("100"));
		trigger.children.insert(QStringLiteral("send"),
		                        QStringLiteral("CallPlugin(\"%1\", \"qcb_mark_trigger\", \"\")\n"
		                                       "Send(\"qcmd-trigger-a91\")")
		                            .arg(kTelnetOrderingPluginId));
		return trigger;
	}

	/**
	 * @brief Creates a direct script trigger whose Lua body sends multiple commands.
	 * @return Trigger fixture.
	 */
	WorldRuntime::Trigger makeLuaSendPriorityTrigger()
	{
		WorldRuntime::Trigger trigger;
		trigger.attributes.insert(QStringLiteral("enabled"), QStringLiteral("y"));
		trigger.attributes.insert(QStringLiteral("match"), QStringLiteral("qxv-priority-line-38"));
		trigger.attributes.insert(QStringLiteral("send_to"), QString::number(eSendToScript));
		trigger.attributes.insert(QStringLiteral("sequence"), QStringLiteral("100"));
		trigger.children.insert(QStringLiteral("send"),
		                        QStringLiteral("Send(\"qcmd-lua-a91\")\nSend(\"qcmd-lua-b26\")"));
		return trigger;
	}

	/**
	 * @brief Creates a direct script trigger whose Lua body executes a command.
	 * @return Trigger fixture.
	 */
	WorldRuntime::Trigger makeLuaExecutePriorityTrigger()
	{
		WorldRuntime::Trigger trigger;
		trigger.attributes.insert(QStringLiteral("enabled"), QStringLiteral("y"));
		trigger.attributes.insert(QStringLiteral("match"), QStringLiteral("qxv-lua-execute-line-73"));
		trigger.attributes.insert(QStringLiteral("send_to"), QString::number(eSendToScript));
		trigger.attributes.insert(QStringLiteral("sequence"), QStringLiteral("100"));
		trigger.children.insert(QStringLiteral("send"), QStringLiteral("Execute(\"qcmd-lua-execute-c52\")"));
		return trigger;
	}

	/**
	 * @brief Creates a direct script trigger whose Lua body mixes Send and Execute commands.
	 * @return Trigger fixture.
	 */
	WorldRuntime::Trigger makeLuaMixedPriorityTrigger()
	{
		WorldRuntime::Trigger trigger;
		trigger.attributes.insert(QStringLiteral("enabled"), QStringLiteral("y"));
		trigger.attributes.insert(QStringLiteral("match"), QStringLiteral("qxv-lua-mixed-line-84"));
		trigger.attributes.insert(QStringLiteral("send_to"), QString::number(eSendToScript));
		trigger.attributes.insert(QStringLiteral("sequence"), QStringLiteral("100"));
		trigger.children.insert(QStringLiteral("send"), QStringLiteral("Send(\"qcmd-lua-mixed-a19\")\n"
		                                                               "Execute(\"qcmd-lua-mixed-b42\")\n"
		                                                               "Send(\"qcmd-lua-mixed-c86\")"));
		return trigger;
	}

	/**
	 * @brief Creates a direct script trigger whose Lua body calls a plugin routine.
	 * @return Trigger fixture.
	 */
	WorldRuntime::Trigger makeLuaCallPluginPriorityTrigger()
	{
		WorldRuntime::Trigger trigger;
		trigger.attributes.insert(QStringLiteral("enabled"), QStringLiteral("y"));
		trigger.attributes.insert(QStringLiteral("match"), QStringLiteral("qxv-callplugin-line-52"));
		trigger.attributes.insert(QStringLiteral("send_to"), QString::number(eSendToScript));
		trigger.attributes.insert(QStringLiteral("sequence"), QStringLiteral("100"));
		trigger.children.insert(QStringLiteral("send"),
		                        QStringLiteral("CallPlugin(\"%1\", \"qcb_nested_priority_check\", \"\")")
		                            .arg(kNestedCallPluginId));
		return trigger;
	}

	/**
	 * @brief Creates a named callback trigger whose Lua function sends a command.
	 * @return Trigger fixture.
	 */
	WorldRuntime::Trigger makeNamedCallbackQueueNormalTrigger()
	{
		WorldRuntime::Trigger trigger;
		trigger.attributes.insert(QStringLiteral("enabled"), QStringLiteral("y"));
		trigger.attributes.insert(QStringLiteral("match"), QStringLiteral("qxv-callback-line-61"));
		trigger.attributes.insert(QStringLiteral("script"), QStringLiteral("qcb_priority_check"));
		trigger.attributes.insert(QStringLiteral("sequence"), QStringLiteral("100"));
		return trigger;
	}

	/**
	 * @brief Creates an execute trigger whose command should claim the direct trigger priority band.
	 * @param match Trigger match text.
	 * @return Trigger fixture.
	 */
	WorldRuntime::Trigger makeExecuteQueuePriorityTrigger(const QString &match)
	{
		WorldRuntime::Trigger trigger;
		trigger.attributes.insert(QStringLiteral("enabled"), QStringLiteral("y"));
		trigger.attributes.insert(QStringLiteral("match"), match);
		trigger.attributes.insert(QStringLiteral("send_to"), QString::number(eSendToExecute));
		trigger.attributes.insert(QStringLiteral("sequence"), QStringLiteral("100"));
		trigger.children.insert(QStringLiteral("send"), QStringLiteral("qcmd-execute-a14"));
		return trigger;
	}

	/**
	 * @brief Creates an execute trigger with a multiline action body.
	 * @param match Trigger match text.
	 * @return Trigger fixture.
	 */
	WorldRuntime::Trigger makeMultilineExecuteTrigger(const QString &match)
	{
		WorldRuntime::Trigger trigger;
		trigger.attributes.insert(QStringLiteral("enabled"), QStringLiteral("y"));
		trigger.attributes.insert(QStringLiteral("match"), match);
		trigger.attributes.insert(QStringLiteral("send_to"), QString::number(eSendToExecute));
		trigger.attributes.insert(QStringLiteral("sequence"), QStringLiteral("100"));
		trigger.children.insert(QStringLiteral("send"),
		                        QStringLiteral("qcmd-trigger-multi-a23\nqcmd-trigger-multi-b58\n"));
		return trigger;
	}

	/**
	 * @brief Creates an execute alias with a multiline action body.
	 * @param match Alias match text.
	 * @return Alias fixture.
	 */
	WorldRuntime::Alias makeMultilineExecuteAlias(const QString &match)
	{
		WorldRuntime::Alias alias;
		alias.attributes.insert(QStringLiteral("enabled"), QStringLiteral("y"));
		alias.attributes.insert(QStringLiteral("match"), match);
		alias.attributes.insert(QStringLiteral("send_to"), QString::number(eSendToExecute));
		alias.attributes.insert(QStringLiteral("sequence"), QStringLiteral("100"));
		alias.children.insert(QStringLiteral("send"),
		                      QStringLiteral("qcmd-alias-multi-a23\nqcmd-alias-multi-b58\n"));
		return alias;
	}

	/**
	 * @brief Creates an execute timer with a multiline action body.
	 * @return Timer fixture.
	 */
	WorldRuntime::Timer makeMultilineExecuteTimer()
	{
		WorldRuntime::Timer timer;
		timer.attributes.insert(QStringLiteral("name"), QStringLiteral("qxv-timer-multiline-41"));
		timer.attributes.insert(QStringLiteral("enabled"), QStringLiteral("1"));
		timer.attributes.insert(QStringLiteral("active_closed"), QStringLiteral("1"));
		timer.attributes.insert(QStringLiteral("send_to"), QString::number(eSendToExecute));
		timer.attributes.insert(QStringLiteral("at_time"), QStringLiteral("0"));
		timer.attributes.insert(QStringLiteral("hour"), QStringLiteral("0"));
		timer.attributes.insert(QStringLiteral("minute"), QStringLiteral("0"));
		timer.attributes.insert(QStringLiteral("second"), QStringLiteral("3600"));
		timer.attributes.insert(QStringLiteral("offset_hour"), QStringLiteral("0"));
		timer.attributes.insert(QStringLiteral("offset_minute"), QStringLiteral("0"));
		timer.attributes.insert(QStringLiteral("offset_second"), QStringLiteral("0"));
		timer.children.insert(QStringLiteral("send"),
		                      QStringLiteral("\nqcmd-timer-multi-a23\n\nqcmd-timer-multi-b58\n"));
		timer.nextFireTime = QDateTime::currentDateTime().addSecs(3600);
		return timer;
	}

	/**
	 * @brief Test fixture binding a runtime to a view and command processor.
	 */
	struct RuntimeCommandHarness
	{
			/**
			 * @brief Constructs and wires the runtime command-processing path.
			 * @param boundRuntime Runtime to bind.
			 */
			explicit RuntimeCommandHarness(WorldRuntime &boundRuntime) : runtime(boundRuntime)
			{
				view.resize(640, 480);
				processor.setView(&view);
				processor.setRuntime(&runtime);
				runtime.setCommandProcessor(&processor);
				view.setRuntime(&runtime);
				QObject::connect(&runtime, &WorldRuntime::incomingStyledLineReceived, &processor,
				                 &WorldCommandProcessor::onIncomingStyledLineReceived);
				QObject::connect(&runtime, &WorldRuntime::incomingStyledLinePartialReceived, &processor,
				                 &WorldCommandProcessor::onIncomingStyledLinePartialReceived);
			}

			/**
			 * @brief Detaches runtime pointers before members are destroyed.
			 */
			~RuntimeCommandHarness()
			{
				view.setRuntime(nullptr);
				runtime.setCommandProcessor(nullptr);
				processor.setRuntime(nullptr);
			}

			/**
			 * @brief Shows the view and waits until exposed.
			 * @return `true` when the view is exposed.
			 */
			bool showAndWait()
			{
				view.show();
				return QTest::qWaitForWindowExposed(&view);
			}

			WorldRuntime         &runtime;
			WorldView             view;
			WorldCommandProcessor processor;
	};

	/**
	 * @brief Decodes queued command payloads for assertions.
	 * @param runtime Runtime whose queue should be inspected.
	 * @return Queue payloads in dispatch order.
	 */
	QStringList queuedPayloads(const WorldRuntime &runtime)
	{
		QStringList payloads;
		for (const QString &entry : runtime.queuedCommands())
			payloads.push_back(QMudCommandQueue::decodeQueueEntry(entry).payload);
		return payloads;
	}

	/**
	 * @brief Reads a plugin variable from the runtime.
	 * @param runtime Runtime to inspect.
	 * @param pluginId Plugin identifier to inspect.
	 * @param name Plugin variable name.
	 * @return Variable value, or empty when missing.
	 */
	QString pluginVariable(const WorldRuntime &runtime, const QString &pluginId, const QString &name)
	{
		QString value;
		if (!runtime.findPluginVariable(pluginId, name, value))
			return {};
		return value;
	}

	/**
	 * @brief Reads a deferred-connect plugin variable from the runtime.
	 * @param runtime Runtime to inspect.
	 * @param name Plugin variable name.
	 * @return Variable value, or empty when missing.
	 */
	QString pluginVariable(const WorldRuntime &runtime, const QString &name)
	{
		return pluginVariable(runtime, kDeferredConnectPluginId, name);
	}

	/**
	 * @brief Reads a world variable from the runtime.
	 * @param runtime Runtime to inspect.
	 * @param name World variable name.
	 * @return Variable value, or empty when missing.
	 */
	QString worldVariable(const WorldRuntime &runtime, const QString &name)
	{
		QString value;
		if (!runtime.findVariable(name, value))
			return {};
		return value;
	}

	/**
	 * @brief Configures a runtime with world and plugin focus callback recorders.
	 * @param runtime Runtime to configure.
	 * @param tempDir Temporary root directory.
	 */
	void configureFocusCallbackRuntime(WorldRuntime &runtime, const QTemporaryDir &tempDir)
	{
		runtime.setStartupDirectory(tempDir.path());
		runtime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
		runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
		runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
		runtime.setWorldAttribute(QStringLiteral("on_world_get_focus"),
		                          QStringLiteral("qcb_world_get_focus"));
		runtime.setWorldAttribute(QStringLiteral("on_world_lose_focus"),
		                          QStringLiteral("qcb_world_lose_focus"));
		runtime.setLuaScriptText(QStringLiteral(R"lua(
function qcb_append_world_focus(marker)
  local current = GetVariable("world_focus_order") or ""
  if current ~= "" then
    current = current .. ","
  end
  SetVariable("world_focus_order", current .. marker)

  local states = GetVariable("world_focus_active_values") or ""
  if states ~= "" then
    states = states .. ","
  end
  SetVariable("world_focus_active_values", states .. tostring(GetInfo(113)))

  local sources = GetVariable("world_focus_action_sources") or ""
  if sources ~= "" then
    sources = sources .. ","
  end
  SetVariable("world_focus_action_sources", sources .. string.format("%.0f", GetInfo(239)))
end

function qcb_world_get_focus()
  qcb_append_world_focus("world_get")
end

function qcb_world_lose_focus()
  qcb_append_world_focus("world_lose")
end
)lua"));
	}

	/**
	 * @brief Verifies callback order and socket send order for telnet ordering tests.
	 * @param runtime Runtime to inspect.
	 * @param acceptedSocket Accepted test-server socket receiving client commands.
	 * @param callbackOrder Expected callback order marker list.
	 */
	void verifyTelnetCallbackAndSocketSendOrder(const WorldRuntime &runtime, QTcpSocket *acceptedSocket,
	                                            const QString &callbackOrder)
	{
		QTRY_COMPARE_WITH_TIMEOUT(
		    pluginVariable(runtime, kTelnetOrderingPluginId, QStringLiteral("callback_order")), callbackOrder,
		    5000);

		QByteArray received;
		auto       receivedBothCommands = [&]
		{
			if (acceptedSocket->bytesAvailable() == 0)
				acceptedSocket->waitForReadyRead(10);
			received += acceptedSocket->readAll();
			return received.contains("qcmd-trigger-a91\r\n") && received.contains("qcmd-telnet-b73\r\n");
		};
		QTRY_VERIFY_WITH_TIMEOUT(receivedBothCommands(), 5000);
		QVERIFY(received.indexOf("qcmd-trigger-a91\r\n") < received.indexOf("qcmd-telnet-b73\r\n"));
	}

	/**
	 * @brief Applies shared runtime setup for telnet ordering tests.
	 * @param runtime Runtime to configure.
	 * @param startupDirectory Temporary startup directory containing plugin fixtures.
	 */
	void configureTelnetOrderingRuntime(WorldRuntime &runtime, const QString &startupDirectory)
	{
		runtime.setStartupDirectory(startupDirectory);
		runtime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
		runtime.setWorldAttribute(QStringLiteral("enable_triggers"), QStringLiteral("y"));
		runtime.setWorldAttribute(QStringLiteral("enable_trigger_sounds"), QStringLiteral("n"));
		runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
		runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
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
		static void hiddenConnectDisconnectMessagesDoNotSuppressPluginLifecycleCallbacks()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("worlds/plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));
			QVERIFY(writeHiddenMessageLifecyclePlugin(pluginsDir));

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
			runtime.setWorldAttribute(QStringLiteral("show_connect_disconnect"), QStringLiteral("0"));

			WorldChildWindow window(QStringLiteral("Hidden Messages"));
			window.resize(640, 480);
			window.setRuntime(&runtime);
			window.show();
			QVERIFY(QTest::qWaitForWindowExposed(&window));

			QString loadError;
			QVERIFY2(runtime.loadPluginFile(QStringLiteral("hidden_messages.xml"), &loadError),
			         qPrintable(loadError));
			QTRY_VERIFY_WITH_TIMEOUT(!runtime.plugins().constFirst().installPending, 5000);

			QVERIFY(QMetaObject::invokeMethod(&runtime, "connected", Qt::DirectConnection));
			QTRY_COMPARE_WITH_TIMEOUT(
			    pluginVariable(runtime, kHiddenMessagePluginId, QStringLiteral("connect_marker")),
			    QStringLiteral("connected"), 5000);

			QVERIFY(QMetaObject::invokeMethod(&runtime, "disconnected", Qt::DirectConnection));
			QTRY_COMPARE_WITH_TIMEOUT(
			    pluginVariable(runtime, kHiddenMessagePluginId, QStringLiteral("disconnect_marker")),
			    QStringLiteral("disconnected"), 5000);
		}

		static void hiddenConnectDisconnectMessagesDoNotSuppressLifecycleActions()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("worlds/plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));
			QVERIFY(writeHiddenMessageLifecyclePlugin(pluginsDir));

			QTcpServer server;
			if (!server.listen(QHostAddress::LocalHost, 0))
				QSKIP("Local TCP listen is unavailable in this environment.");

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
			runtime.setWorldAttribute(QStringLiteral("show_connect_disconnect"), QStringLiteral("0"));
			runtime.setWorldMultilineAttribute(QStringLiteral("connect_text"),
			                                   QStringLiteral("hidden_connect_text"));

			WorldChildWindow window(QStringLiteral("Hidden Messages"));
			window.resize(640, 480);
			window.setRuntime(&runtime);
			window.show();
			QVERIFY(QTest::qWaitForWindowExposed(&window));

			QString loadError;
			QVERIFY2(runtime.loadPluginFile(QStringLiteral("hidden_messages.xml"), &loadError),
			         qPrintable(loadError));
			QTRY_VERIFY_WITH_TIMEOUT(!runtime.plugins().constFirst().installPending, 5000);

			QSignalSpy connectedSpy(&runtime, &WorldRuntime::connected);
			QVERIFY(connectedSpy.isValid());
			QSignalSpy disconnectedSpy(&runtime, &WorldRuntime::disconnected);
			QVERIFY(disconnectedSpy.isValid());
			QSignalSpy serverAcceptedSpy(&server, &QTcpServer::newConnection);
			QVERIFY(serverAcceptedSpy.isValid());

			QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
			QVERIFY(connectedSpy.wait(5000));
			QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() || serverAcceptedSpy.count() > 0, 5000);
			QScopedPointer<QTcpSocket> acceptedSocket(server.nextPendingConnection());
			QVERIFY(!acceptedSocket.isNull());

			QByteArray received;
			auto       hasReceivedConnectText = [&acceptedSocket, &received]
			{
				if (acceptedSocket->bytesAvailable() == 0)
					acceptedSocket->waitForReadyRead(10);
				received += acceptedSocket->readAll();
				return received.contains("hidden_connect_text");
			};
			QTRY_VERIFY_WITH_TIMEOUT(hasReceivedConnectText(), 5000);
			QTRY_COMPARE_WITH_TIMEOUT(
			    pluginVariable(runtime, kHiddenMessagePluginId, QStringLiteral("connect_marker")),
			    QStringLiteral("connected"), 5000);

			runtime.disconnectFromWorld();
			if (disconnectedSpy.isEmpty())
				QVERIFY(disconnectedSpy.wait(5000));
			QTRY_COMPARE_WITH_TIMEOUT(
			    pluginVariable(runtime, kHiddenMessagePluginId, QStringLiteral("disconnect_marker")),
			    QStringLiteral("disconnected"), 5000);
		}

		static void focusCallbacksFollowMushclientWorldThenPluginOrder()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("worlds/plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));
			QVERIFY(writeFocusCallbackPlugin(pluginsDir));

			WorldRuntime runtime;
			configureFocusCallbackRuntime(runtime, tempDir);

			WorldView view;
			view.resize(640, 480);
			view.setRuntime(&runtime);
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			QString loadError;
			QVERIFY2(runtime.loadPluginFile(QStringLiteral("focus_callbacks.xml"), &loadError),
			         qPrintable(loadError));
			QTRY_VERIFY_WITH_TIMEOUT(!runtime.plugins().constFirst().installPending, 5000);

			runtime.requestActiveState(true);
			QTRY_COMPARE_WITH_TIMEOUT(worldVariable(runtime, QStringLiteral("world_focus_order")),
			                          QStringLiteral("world_get"), 5000);
			QTRY_COMPARE_WITH_TIMEOUT(
			    pluginVariable(runtime, kFocusCallbackPluginId, QStringLiteral("plugin_focus_order")),
			    QStringLiteral("plugin_get"), 5000);
			QCOMPARE(worldVariable(runtime, QStringLiteral("world_focus_active_values")),
			         QStringLiteral("true"));
			QCOMPARE(
			    pluginVariable(runtime, kFocusCallbackPluginId, QStringLiteral("plugin_focus_active_values")),
			    QStringLiteral("true"));
			QCOMPARE(worldVariable(runtime, QStringLiteral("world_focus_action_sources")),
			         QString::number(WorldRuntime::eWorldAction));
			QCOMPARE(pluginVariable(runtime, kFocusCallbackPluginId,
			                        QStringLiteral("plugin_focus_action_sources")),
			         QString::number(WorldRuntime::eWorldAction));

			runtime.requestActiveState(true);
			QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
			QCOMPARE(worldVariable(runtime, QStringLiteral("world_focus_order")),
			         QStringLiteral("world_get"));
			QCOMPARE(pluginVariable(runtime, kFocusCallbackPluginId, QStringLiteral("plugin_focus_order")),
			         QStringLiteral("plugin_get"));

			runtime.requestActiveState(false);
			QTRY_COMPARE_WITH_TIMEOUT(worldVariable(runtime, QStringLiteral("world_focus_order")),
			                          QStringLiteral("world_get,world_lose"), 5000);
			QTRY_COMPARE_WITH_TIMEOUT(
			    pluginVariable(runtime, kFocusCallbackPluginId, QStringLiteral("plugin_focus_order")),
			    QStringLiteral("plugin_get,plugin_lose"), 5000);
			QCOMPARE(worldVariable(runtime, QStringLiteral("world_focus_active_values")),
			         QStringLiteral("true,false"));
			QCOMPARE(
			    pluginVariable(runtime, kFocusCallbackPluginId, QStringLiteral("plugin_focus_active_values")),
			    QStringLiteral("true,false"));
			QCOMPARE(worldVariable(runtime, QStringLiteral("world_focus_action_sources")),
			         QStringLiteral("%1,%1").arg(WorldRuntime::eWorldAction));
			QCOMPARE(pluginVariable(runtime, kFocusCallbackPluginId,
			                        QStringLiteral("plugin_focus_action_sources")),
			         QStringLiteral("%1,%1").arg(WorldRuntime::eWorldAction));
		}

		static void focusCallbacksPreserveRapidOppositeTransitions()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("worlds/plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));
			QVERIFY(writeFocusCallbackPlugin(pluginsDir));

			WorldRuntime runtime;
			configureFocusCallbackRuntime(runtime, tempDir);

			WorldView view;
			view.resize(640, 480);
			view.setRuntime(&runtime);
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			QString loadError;
			QVERIFY2(runtime.loadPluginFile(QStringLiteral("focus_callbacks.xml"), &loadError),
			         qPrintable(loadError));
			QTRY_VERIFY_WITH_TIMEOUT(!runtime.plugins().constFirst().installPending, 5000);

			runtime.requestActiveState(true);
			runtime.requestActiveState(false);
			runtime.requestActiveState(true);

			QTRY_COMPARE_WITH_TIMEOUT(worldVariable(runtime, QStringLiteral("world_focus_order")),
			                          QStringLiteral("world_get,world_lose,world_get"), 5000);
			QTRY_COMPARE_WITH_TIMEOUT(
			    pluginVariable(runtime, kFocusCallbackPluginId, QStringLiteral("plugin_focus_order")),
			    QStringLiteral("plugin_get,plugin_lose,plugin_get"), 5000);
			QCOMPARE(worldVariable(runtime, QStringLiteral("world_focus_active_values")),
			         QStringLiteral("true,false,true"));
			QCOMPARE(
			    pluginVariable(runtime, kFocusCallbackPluginId, QStringLiteral("plugin_focus_active_values")),
			    QStringLiteral("true,false,true"));
		}

		static void asyncExecuteScriptActionSourceDoesNotLeakToNextCallback()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
			runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));

			QSharedPointer<LuaCallbackEngine> engine(runtime.luaCallbacks(),
			                                         [](LuaCallbackEngine * /*unused*/) {});
			QVERIFY(engine);

			bool executeCompleted = false;
			bool executeOk        = false;
			runtime.dispatchLuaExecuteScriptAsync(engine, QStringLiteral(R"lua(
SetVariable("async_execute_source", string.format("%.0f", GetInfo(239)))
function qcb_record_post_async_source(label)
  SetVariable(label .. "_source", string.format("%.0f", GetInfo(239)))
end
)lua"),
			                                      QStringLiteral("async action source isolation"), nullptr,
			                                      false, false, 0, 0,
			                                      [&executeCompleted, &executeOk](const bool ok)
			                                      {
				                                      executeOk        = ok;
				                                      executeCompleted = true;
			                                      });

			QTRY_VERIFY_WITH_TIMEOUT(executeCompleted, 5000);
			QVERIFY(executeOk);
			QCOMPARE(worldVariable(runtime, QStringLiteral("async_execute_source")),
			         QString::number(WorldRuntime::eLuaSandbox));
			QCOMPARE(runtime.currentActionSource(), WorldRuntime::eUnknownActionSource);

			bool callbackCompleted = false;
			runtime.dispatchLuaStringsAndWildcardsAsync(
			    engine, QStringLiteral("qcb_record_post_async_source"),
			    {QStringLiteral("post_async_callback")}, {}, {}, nullptr, -1, false, 0, 0,
			    [&callbackCompleted](const LuaBatchDispatchResult & /*unused*/)
			    { callbackCompleted = true; });

			QTRY_VERIFY_WITH_TIMEOUT(callbackCompleted, 5000);
			QCOMPARE(worldVariable(runtime, QStringLiteral("post_async_callback_source")),
			         QString::number(WorldRuntime::eUnknownActionSource));
			QCOMPARE(runtime.currentActionSource(), WorldRuntime::eUnknownActionSource);
		}

		static void executeTriggerSendCommandEntersPriorityQueueBand()
		{
			QTcpServer server;
			if (!server.listen(QHostAddress::LocalHost, 0))
				QSKIP("Local TCP listen is unavailable in this environment.");

			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("enable_triggers"), QStringLiteral("y"));
			runtime.setWorldAttribute(QStringLiteral("enable_trigger_sounds"), QStringLiteral("n"));
			runtime.setWorldAttribute(QStringLiteral("speed_walk_delay"), QStringLiteral("60000"));
			runtime.triggersMutable().push_back(
			    makeExecuteQueuePriorityTrigger(QStringLiteral("qxv-execute-line-42")));
			runtime.markTriggersChanged();

			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			QSignalSpy connectedSpy(&runtime, &WorldRuntime::connected);
			QVERIFY(connectedSpy.isValid());
			QSignalSpy serverAcceptedSpy(&server, &QTcpServer::newConnection);
			QVERIFY(serverAcceptedSpy.isValid());

			QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
			QVERIFY(connectedSpy.wait(5000));
			QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() || serverAcceptedSpy.count() > 0, 5000);
			QScopedPointer<QTcpSocket> acceptedSocket(server.nextPendingConnection());
			QVERIFY(!acceptedSocket.isNull());

			QCOMPARE(runtime.sendCommand(QStringLiteral("qcmd-tail-z77"), false, true, true, false, false),
			         eOK);
			QTRY_COMPARE_WITH_TIMEOUT(queuedPayloads(runtime), (QStringList{QStringLiteral("qcmd-tail-z77")}),
			                          5000);

			const QByteArray triggerLine = QByteArrayLiteral("qxv-execute-line-42\r\n");
			QCOMPARE(acceptedSocket->write(triggerLine), static_cast<qint64>(triggerLine.size()));
			QVERIFY(acceptedSocket->flush());

			QTRY_COMPARE_WITH_TIMEOUT(
			    queuedPayloads(runtime),
			    (QStringList{QStringLiteral("qcmd-execute-a14"), QStringLiteral("qcmd-tail-z77")}), 5000);
		}

		static void executeTriggerMultilineActionRunsEachCommandWithoutTrailingBlank()
		{
			QTcpServer server;
			if (!server.listen(QHostAddress::LocalHost, 0))
				QSKIP("Local TCP listen is unavailable in this environment.");

			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("enable_triggers"), QStringLiteral("y"));
			runtime.setWorldAttribute(QStringLiteral("enable_trigger_sounds"), QStringLiteral("n"));
			runtime.setWorldAttribute(QStringLiteral("speed_walk_delay"), QStringLiteral("60000"));
			runtime.triggersMutable().push_back(
			    makeMultilineExecuteTrigger(QStringLiteral("qxv-execute-multiline-18")));
			runtime.markTriggersChanged();

			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			QSignalSpy connectedSpy(&runtime, &WorldRuntime::connected);
			QVERIFY(connectedSpy.isValid());
			QSignalSpy serverAcceptedSpy(&server, &QTcpServer::newConnection);
			QVERIFY(serverAcceptedSpy.isValid());

			QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
			QVERIFY(connectedSpy.wait(5000));
			QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() || serverAcceptedSpy.count() > 0, 5000);
			QScopedPointer<QTcpSocket> acceptedSocket(server.nextPendingConnection());
			QVERIFY(!acceptedSocket.isNull());

			QCOMPARE(runtime.sendCommand(QStringLiteral("qcmd-tail-z77"), false, true, true, false, false),
			         eOK);
			QTRY_COMPARE_WITH_TIMEOUT(queuedPayloads(runtime), (QStringList{QStringLiteral("qcmd-tail-z77")}),
			                          5000);

			const QByteArray triggerLine = QByteArrayLiteral("qxv-execute-multiline-18\r\n");
			QCOMPARE(acceptedSocket->write(triggerLine), static_cast<qint64>(triggerLine.size()));
			QVERIFY(acceptedSocket->flush());

			QTRY_COMPARE_WITH_TIMEOUT(
			    queuedPayloads(runtime),
			    (QStringList{QStringLiteral("qcmd-trigger-multi-a23"),
			                 QStringLiteral("qcmd-trigger-multi-b58"), QStringLiteral("qcmd-tail-z77")}),
			    5000);
		}

		static void executeAliasMultilineActionRunsEachCommandWithoutTrailingBlank()
		{
			QTcpServer server;
			if (!server.listen(QHostAddress::LocalHost, 0))
				QSKIP("Local TCP listen is unavailable in this environment.");

			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("enable_aliases"), QStringLiteral("y"));
			runtime.setWorldAttribute(QStringLiteral("speed_walk_delay"), QStringLiteral("60000"));
			runtime.aliasesMutable().push_back(
			    makeMultilineExecuteAlias(QStringLiteral("qxv-alias-multiline-29")));
			runtime.markAliasesChanged();

			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			QSignalSpy connectedSpy(&runtime, &WorldRuntime::connected);
			QVERIFY(connectedSpy.isValid());
			QSignalSpy serverAcceptedSpy(&server, &QTcpServer::newConnection);
			QVERIFY(serverAcceptedSpy.isValid());

			QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
			QVERIFY(connectedSpy.wait(5000));
			QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() || serverAcceptedSpy.count() > 0, 5000);
			QScopedPointer<QTcpSocket> acceptedSocket(server.nextPendingConnection());
			QVERIFY(!acceptedSocket.isNull());

			QCOMPARE(runtime.sendCommand(QStringLiteral("qcmd-tail-z77"), false, true, true, false, false),
			         eOK);
			QTRY_COMPARE_WITH_TIMEOUT(queuedPayloads(runtime), (QStringList{QStringLiteral("qcmd-tail-z77")}),
			                          5000);

			QCOMPARE(runtime.executeCommand(QStringLiteral("qxv-alias-multiline-29")), eOK);

			QTRY_COMPARE_WITH_TIMEOUT(
			    queuedPayloads(runtime),
			    (QStringList{QStringLiteral("qcmd-tail-z77"), QStringLiteral("qcmd-alias-multi-a23"),
			                 QStringLiteral("qcmd-alias-multi-b58")}),
			    5000);
		}

		static void executeTimerMultilineActionRunsEachCommandWithoutBlankLines()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("worlds/plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));
			QVERIFY(writeTimerCommandRecorderPlugin(pluginsDir));

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
			runtime.setWorldAttribute(QStringLiteral("enable_timers"), QStringLiteral("y"));
			runtime.timersMutable().push_back(makeMultilineExecuteTimer());
			runtime.markTimersChanged();

			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			QString loadError;
			QVERIFY2(runtime.loadPluginFile(QStringLiteral("timer_command_recorder.xml"), &loadError),
			         qPrintable(loadError));
			QTRY_VERIFY_WITH_TIMEOUT(!runtime.plugins().constFirst().installPending, 5000);

			runtime.timersMutable().first().nextFireTime = QDateTime::currentDateTime().addMSecs(-1);

			QTRY_COMPARE_WITH_TIMEOUT(runtime.timersFiredThisSession(), 1, 5000);
			QTRY_COMPARE_WITH_TIMEOUT(
			    pluginVariable(runtime, kTimerCommandPluginId, QStringLiteral("timer_commands")),
			    QStringLiteral("qcmd-timer-multi-a23,qcmd-timer-multi-b58"), 5000);
		}

		static void directLuaTriggerSendCommandsEnterPriorityQueueBand()
		{
			QTcpServer server;
			if (!server.listen(QHostAddress::LocalHost, 0))
				QSKIP("Local TCP listen is unavailable in this environment.");

			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("enable_triggers"), QStringLiteral("y"));
			runtime.setWorldAttribute(QStringLiteral("enable_trigger_sounds"), QStringLiteral("n"));
			runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
			runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
			runtime.setWorldAttribute(QStringLiteral("speed_walk_delay"), QStringLiteral("60000"));
			runtime.triggersMutable().push_back(makeLuaSendPriorityTrigger());
			runtime.markTriggersChanged();

			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			QSignalSpy connectedSpy(&runtime, &WorldRuntime::connected);
			QVERIFY(connectedSpy.isValid());
			QSignalSpy serverAcceptedSpy(&server, &QTcpServer::newConnection);
			QVERIFY(serverAcceptedSpy.isValid());

			QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
			QVERIFY(connectedSpy.wait(5000));
			QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() || serverAcceptedSpy.count() > 0, 5000);
			QScopedPointer<QTcpSocket> acceptedSocket(server.nextPendingConnection());
			QVERIFY(!acceptedSocket.isNull());

			QCOMPARE(runtime.sendCommand(QStringLiteral("qcmd-tail-z77"), false, true, true, false, false),
			         eOK);
			QTRY_COMPARE_WITH_TIMEOUT(queuedPayloads(runtime), (QStringList{QStringLiteral("qcmd-tail-z77")}),
			                          5000);

			const QByteArray triggerLine = QByteArrayLiteral("qxv-priority-line-38\r\n");
			QCOMPARE(acceptedSocket->write(triggerLine), static_cast<qint64>(triggerLine.size()));
			QVERIFY(acceptedSocket->flush());

			QTRY_COMPARE_WITH_TIMEOUT(
			    queuedPayloads(runtime),
			    (QStringList{QStringLiteral("qcmd-lua-a91"), QStringLiteral("qcmd-lua-b26"),
			                 QStringLiteral("qcmd-tail-z77")}),
			    5000);
		}

		static void directLuaTriggerExecuteCommandEntersPriorityQueueBand()
		{
			QTcpServer server;
			if (!server.listen(QHostAddress::LocalHost, 0))
				QSKIP("Local TCP listen is unavailable in this environment.");

			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("enable_triggers"), QStringLiteral("y"));
			runtime.setWorldAttribute(QStringLiteral("enable_trigger_sounds"), QStringLiteral("n"));
			runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
			runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
			runtime.setWorldAttribute(QStringLiteral("speed_walk_delay"), QStringLiteral("60000"));
			runtime.triggersMutable().push_back(makeLuaExecutePriorityTrigger());
			runtime.markTriggersChanged();

			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			QSignalSpy connectedSpy(&runtime, &WorldRuntime::connected);
			QVERIFY(connectedSpy.isValid());
			QSignalSpy serverAcceptedSpy(&server, &QTcpServer::newConnection);
			QVERIFY(serverAcceptedSpy.isValid());

			QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
			QVERIFY(connectedSpy.wait(5000));
			QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() || serverAcceptedSpy.count() > 0, 5000);
			QScopedPointer<QTcpSocket> acceptedSocket(server.nextPendingConnection());
			QVERIFY(!acceptedSocket.isNull());

			QCOMPARE(runtime.sendCommand(QStringLiteral("qcmd-tail-z77"), false, true, true, false, false),
			         eOK);
			QTRY_COMPARE_WITH_TIMEOUT(queuedPayloads(runtime), (QStringList{QStringLiteral("qcmd-tail-z77")}),
			                          5000);

			const QByteArray triggerLine = QByteArrayLiteral("qxv-lua-execute-line-73\r\n");
			QCOMPARE(acceptedSocket->write(triggerLine), static_cast<qint64>(triggerLine.size()));
			QVERIFY(acceptedSocket->flush());

			QTRY_COMPARE_WITH_TIMEOUT(
			    queuedPayloads(runtime),
			    (QStringList{QStringLiteral("qcmd-lua-execute-c52"), QStringLiteral("qcmd-tail-z77")}), 5000);
		}

		static void directLuaTriggerMixedSendAndExecuteCommandsPreservePriorityOrder()
		{
			QTcpServer server;
			if (!server.listen(QHostAddress::LocalHost, 0))
				QSKIP("Local TCP listen is unavailable in this environment.");

			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("enable_triggers"), QStringLiteral("y"));
			runtime.setWorldAttribute(QStringLiteral("enable_trigger_sounds"), QStringLiteral("n"));
			runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
			runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
			runtime.setWorldAttribute(QStringLiteral("speed_walk_delay"), QStringLiteral("60000"));
			runtime.triggersMutable().push_back(makeLuaMixedPriorityTrigger());
			runtime.markTriggersChanged();

			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			QSignalSpy connectedSpy(&runtime, &WorldRuntime::connected);
			QVERIFY(connectedSpy.isValid());
			QSignalSpy serverAcceptedSpy(&server, &QTcpServer::newConnection);
			QVERIFY(serverAcceptedSpy.isValid());

			QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
			QVERIFY(connectedSpy.wait(5000));
			QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() || serverAcceptedSpy.count() > 0, 5000);
			QScopedPointer<QTcpSocket> acceptedSocket(server.nextPendingConnection());
			QVERIFY(!acceptedSocket.isNull());

			QCOMPARE(runtime.sendCommand(QStringLiteral("qcmd-tail-z77"), false, true, true, false, false),
			         eOK);
			QTRY_COMPARE_WITH_TIMEOUT(queuedPayloads(runtime), (QStringList{QStringLiteral("qcmd-tail-z77")}),
			                          5000);

			const QByteArray triggerLine = QByteArrayLiteral("qxv-lua-mixed-line-84\r\n");
			QCOMPARE(acceptedSocket->write(triggerLine), static_cast<qint64>(triggerLine.size()));
			QVERIFY(acceptedSocket->flush());

			QTRY_COMPARE_WITH_TIMEOUT(
			    queuedPayloads(runtime),
			    (QStringList{QStringLiteral("qcmd-lua-mixed-a19"), QStringLiteral("qcmd-lua-mixed-b42"),
			                 QStringLiteral("qcmd-lua-mixed-c86"), QStringLiteral("qcmd-tail-z77")}),
			    5000);
		}

		static void namedLuaTriggerCallbackSendDoesNotEnterDirectActionPriorityQueueBand()
		{
			QTcpServer server;
			if (!server.listen(QHostAddress::LocalHost, 0))
				QSKIP("Local TCP listen is unavailable in this environment.");

			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("enable_triggers"), QStringLiteral("y"));
			runtime.setWorldAttribute(QStringLiteral("enable_trigger_sounds"), QStringLiteral("n"));
			runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
			runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
			runtime.setWorldAttribute(QStringLiteral("speed_walk_delay"), QStringLiteral("60000"));
			runtime.setLuaScriptText(
			    QStringLiteral("function qcb_priority_check(name, line, wildcards, styles)\n"
			                   "  Send(\"qcmd-callback-e71\")\n"
			                   "end\n"));
			runtime.triggersMutable().push_back(makeNamedCallbackQueueNormalTrigger());
			runtime.markTriggersChanged();

			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			QSignalSpy connectedSpy(&runtime, &WorldRuntime::connected);
			QVERIFY(connectedSpy.isValid());
			QSignalSpy serverAcceptedSpy(&server, &QTcpServer::newConnection);
			QVERIFY(serverAcceptedSpy.isValid());

			QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
			QVERIFY(connectedSpy.wait(5000));
			QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() || serverAcceptedSpy.count() > 0, 5000);
			QScopedPointer<QTcpSocket> acceptedSocket(server.nextPendingConnection());
			QVERIFY(!acceptedSocket.isNull());

			QCOMPARE(runtime.sendCommand(QStringLiteral("qcmd-tail-z77"), false, true, true, false, false),
			         eOK);
			QTRY_COMPARE_WITH_TIMEOUT(queuedPayloads(runtime), (QStringList{QStringLiteral("qcmd-tail-z77")}),
			                          5000);

			const QByteArray triggerLine = QByteArrayLiteral("qxv-callback-line-61\r\n");
			QCOMPARE(acceptedSocket->write(triggerLine), static_cast<qint64>(triggerLine.size()));
			QVERIFY(acceptedSocket->flush());

			QTRY_COMPARE_WITH_TIMEOUT(
			    queuedPayloads(runtime),
			    (QStringList{QStringLiteral("qcmd-tail-z77"), QStringLiteral("qcmd-callback-e71")}), 5000);
		}

		static void directLuaTriggerCallPluginSendDoesNotInheritDirectActionPriorityQueueBand()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("worlds/plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));
			QVERIFY(writeNestedCallPluginSendPlugin(pluginsDir));

			QTcpServer server;
			if (!server.listen(QHostAddress::LocalHost, 0))
				QSKIP("Local TCP listen is unavailable in this environment.");

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
			runtime.setWorldAttribute(QStringLiteral("enable_triggers"), QStringLiteral("y"));
			runtime.setWorldAttribute(QStringLiteral("enable_trigger_sounds"), QStringLiteral("n"));
			runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
			runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
			runtime.setWorldAttribute(QStringLiteral("speed_walk_delay"), QStringLiteral("60000"));
			runtime.triggersMutable().push_back(makeLuaCallPluginPriorityTrigger());
			runtime.markTriggersChanged();

			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			QString loadError;
			QVERIFY2(runtime.loadPluginFile(QStringLiteral("nested_call_send.xml"), &loadError),
			         qPrintable(loadError));
			QTRY_VERIFY_WITH_TIMEOUT(!runtime.plugins().constFirst().installPending, 5000);

			QSignalSpy connectedSpy(&runtime, &WorldRuntime::connected);
			QVERIFY(connectedSpy.isValid());
			QSignalSpy serverAcceptedSpy(&server, &QTcpServer::newConnection);
			QVERIFY(serverAcceptedSpy.isValid());

			QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
			QVERIFY(connectedSpy.wait(5000));
			QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() || serverAcceptedSpy.count() > 0, 5000);
			QScopedPointer<QTcpSocket> acceptedSocket(server.nextPendingConnection());
			QVERIFY(!acceptedSocket.isNull());

			QCOMPARE(runtime.sendCommand(QStringLiteral("qcmd-tail-z77"), false, true, true, false, false),
			         eOK);
			QTRY_COMPARE_WITH_TIMEOUT(queuedPayloads(runtime), (QStringList{QStringLiteral("qcmd-tail-z77")}),
			                          5000);

			const QByteArray triggerLine = QByteArrayLiteral("qxv-callplugin-line-52\r\n");
			QCOMPARE(acceptedSocket->write(triggerLine), static_cast<qint64>(triggerLine.size()));
			QVERIFY(acceptedSocket->flush());

			QTRY_COMPARE_WITH_TIMEOUT(
			    queuedPayloads(runtime),
			    (QStringList{QStringLiteral("qcmd-tail-z77"), QStringLiteral("qcmd-nested-p54")}), 5000);
		}

		static void telnetSubnegotiationCallbacksPreserveStreamOrderAfterCompletedTriggerLine()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("worlds/plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));
			QVERIFY(writeTelnetOrderingPlugin(pluginsDir));

			QTcpServer server;
			if (!server.listen(QHostAddress::LocalHost, 0))
				QSKIP("Local TCP listen is unavailable in this environment.");

			WorldRuntime runtime;
			configureTelnetOrderingRuntime(runtime, tempDir.path());
			runtime.triggersMutable().push_back(makeTelnetOrderingTrigger());
			runtime.markTriggersChanged();

			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			QString loadError;
			QVERIFY2(runtime.loadPluginFile(QStringLiteral("telnet_ordering.xml"), &loadError),
			         qPrintable(loadError));
			QTRY_VERIFY_WITH_TIMEOUT(!runtime.plugins().constFirst().installPending, 5000);

			QSignalSpy connectedSpy(&runtime, &WorldRuntime::connected);
			QVERIFY(connectedSpy.isValid());
			QSignalSpy serverAcceptedSpy(&server, &QTcpServer::newConnection);
			QVERIFY(serverAcceptedSpy.isValid());
			QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
			QVERIFY(connectedSpy.wait(5000));
			QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() || serverAcceptedSpy.count() > 0, 5000);
			QScopedPointer<QTcpSocket> acceptedSocket(server.nextPendingConnection());
			QVERIFY(!acceptedSocket.isNull());

			QByteArray payload = kTelnetTriggerLine.toUtf8() + QByteArrayLiteral("\r\n");
			payload += bytes({IAC, SB, GMCP, 'q', '7', 'x', IAC, SE});
			QCOMPARE(acceptedSocket->write(payload), static_cast<qint64>(payload.size()));
			QVERIFY(acceptedSocket->flush());

			verifyTelnetCallbackAndSocketSendOrder(runtime, acceptedSocket.data(),
			                                       QStringLiteral("trigger,telnet"));
		}

		static void telnetSubnegotiationCallbacksUsePacketTransformedStreamOrder()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("worlds/plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));
			QVERIFY(writeTelnetOrderingPlugin(pluginsDir, true));

			QTcpServer server;
			if (!server.listen(QHostAddress::LocalHost, 0))
				QSKIP("Local TCP listen is unavailable in this environment.");

			WorldRuntime runtime;
			configureTelnetOrderingRuntime(runtime, tempDir.path());
			runtime.triggersMutable().push_back(makeTelnetOrderingTrigger());
			runtime.markTriggersChanged();

			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			QString loadError;
			QVERIFY2(runtime.loadPluginFile(QStringLiteral("telnet_ordering.xml"), &loadError),
			         qPrintable(loadError));
			QTRY_VERIFY_WITH_TIMEOUT(!runtime.plugins().constFirst().installPending, 5000);

			QSignalSpy connectedSpy(&runtime, &WorldRuntime::connected);
			QVERIFY(connectedSpy.isValid());
			QSignalSpy serverAcceptedSpy(&server, &QTcpServer::newConnection);
			QVERIFY(serverAcceptedSpy.isValid());
			QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
			QVERIFY(connectedSpy.wait(5000));
			QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() || serverAcceptedSpy.count() > 0, 5000);
			QScopedPointer<QTcpSocket> acceptedSocket(server.nextPendingConnection());
			QVERIFY(!acceptedSocket.isNull());

			QByteArray payload = bytes({IAC, SB, GMCP, 'q', '7', 'x', IAC, SE});
			QCOMPARE(acceptedSocket->write(payload), static_cast<qint64>(payload.size()));
			QVERIFY(acceptedSocket->flush());

			verifyTelnetCallbackAndSocketSendOrder(runtime, acceptedSocket.data(),
			                                       QStringLiteral("trigger,telnet"));
		}

		static void telnetSubnegotiationCallbacksRebaseOffsetsAfterFilteredBytes()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("worlds/plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));
			QVERIFY(writeTelnetOrderingPlugin(pluginsDir));

			QTcpServer server;
			if (!server.listen(QHostAddress::LocalHost, 0))
				QSKIP("Local TCP listen is unavailable in this environment.");

			WorldRuntime runtime;
			configureTelnetOrderingRuntime(runtime, tempDir.path());
			runtime.triggersMutable().push_back(makeTelnetOrderingTrigger(kTelnetAfterLine));
			runtime.markTriggersChanged();

			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			QString loadError;
			QVERIFY2(runtime.loadPluginFile(QStringLiteral("telnet_ordering.xml"), &loadError),
			         qPrintable(loadError));
			QTRY_VERIFY_WITH_TIMEOUT(!runtime.plugins().constFirst().installPending, 5000);

			QSignalSpy connectedSpy(&runtime, &WorldRuntime::connected);
			QVERIFY(connectedSpy.isValid());
			QSignalSpy serverAcceptedSpy(&server, &QTcpServer::newConnection);
			QVERIFY(serverAcceptedSpy.isValid());
			QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
			QVERIFY(connectedSpy.wait(5000));
			QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() || serverAcceptedSpy.count() > 0, 5000);
			QScopedPointer<QTcpSocket> acceptedSocket(server.nextPendingConnection());
			QVERIFY(!acceptedSocket.isNull());

			QByteArray payload(10, '\0');
			payload += bytes({IAC, SB, GMCP, 'q', '7', 'x', IAC, SE});
			payload += kTelnetAfterLine.toUtf8() + QByteArrayLiteral("\r\n");
			QCOMPARE(acceptedSocket->write(payload), static_cast<qint64>(payload.size()));
			QVERIFY(acceptedSocket->flush());

			verifyTelnetCallbackAndSocketSendOrder(runtime, acceptedSocket.data(),
			                                       QStringLiteral("telnet,trigger"));
		}

		static void telnetSubnegotiationCallbacksRebaseOffsetsAfterRemovedPuebloMarker()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("worlds/plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));
			QVERIFY(writeTelnetOrderingPlugin(pluginsDir));

			QTcpServer server;
			if (!server.listen(QHostAddress::LocalHost, 0))
				QSKIP("Local TCP listen is unavailable in this environment.");

			WorldRuntime runtime;
			configureTelnetOrderingRuntime(runtime, tempDir.path());
			runtime.setWorldAttribute(QStringLiteral("detect_pueblo"), QStringLiteral("1"));
			runtime.triggersMutable().push_back(makeTelnetOrderingTrigger(kTelnetAfterLine));
			runtime.markTriggersChanged();

			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			QString loadError;
			QVERIFY2(runtime.loadPluginFile(QStringLiteral("telnet_ordering.xml"), &loadError),
			         qPrintable(loadError));
			QTRY_VERIFY_WITH_TIMEOUT(!runtime.plugins().constFirst().installPending, 5000);

			QSignalSpy connectedSpy(&runtime, &WorldRuntime::connected);
			QVERIFY(connectedSpy.isValid());
			QSignalSpy serverAcceptedSpy(&server, &QTcpServer::newConnection);
			QVERIFY(serverAcceptedSpy.isValid());
			QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
			QVERIFY(connectedSpy.wait(5000));
			QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() || serverAcceptedSpy.count() > 0, 5000);
			QScopedPointer<QTcpSocket> acceptedSocket(server.nextPendingConnection());
			QVERIFY(!acceptedSocket.isNull());

			QByteArray payload = QByteArrayLiteral("</xch_mudtext>\r\n");
			payload += bytes({IAC, SB, GMCP, 'q', '7', 'x', IAC, SE});
			payload += kTelnetAfterLine.toUtf8() + QByteArrayLiteral("\r\n");
			QCOMPARE(acceptedSocket->write(payload), static_cast<qint64>(payload.size()));
			QVERIFY(acceptedSocket->flush());

			verifyTelnetCallbackAndSocketSendOrder(runtime, acceptedSocket.data(),
			                                       QStringLiteral("telnet,trigger"));
		}

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
