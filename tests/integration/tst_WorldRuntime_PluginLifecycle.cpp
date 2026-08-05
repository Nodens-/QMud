/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldRuntime_PluginLifecycle.cpp
 * Role: Integration coverage for WorldRuntime plugin lifecycle callback ordering.
 */

#include "LuaCallbackEngine.h"
#include "LuaExecutor.h"
#include "MiniWindow.h"
#include "NativePluginRegistry.h"
#include "WorldChildWindow.h"
#include "WorldCommandProcessor.h"
#include "WorldCommandProcessorUtils.h"
#include "WorldDocument.h"
#include "WorldOptions.h"
#include "WorldRuntime.h"
#include "WorldView.h"
#include "helpers/MiniWindowUtils.h"
#include "scripting/ScriptingErrors.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFile>
// ReSharper disable once CppUnusedIncludeDirective
#include <QHostAddress>
#include <QRegularExpression>
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

#ifdef QMUD_ENABLE_LUA_SCRIPTING
extern "C"
{
#include <lauxlib.h>
#include <lua.h>
}
#endif

namespace
{
	const QString           kDeferredConnectPluginId   = QStringLiteral("abcdeffedcbaabcdeffedcba");
	const QString           kTeardownStatePluginId     = QStringLiteral("fedcbaabcdeffedcbaabcdef");
	const QString           kHiddenMessagePluginId     = QStringLiteral("112233445566778899aabbcc");
	const QString           kNestedCallPluginId        = QStringLiteral("2233445566778899aabbccdd");
	const QString           kTelnetOrderingPluginId    = QStringLiteral("00112233445566778899aabb");
	const QString           kTimerCommandPluginId      = QStringLiteral("33445566778899aabbccddee");
	const QString           kFocusCallbackPluginId     = QStringLiteral("445566778899aabbccddeeff");
	const QString           kMxpEntityCallbackPluginId = QStringLiteral("5566778899aabbccddeeff00");
	const QString           kTelnetTriggerLine         = QStringLiteral("qxv-lattice-17");
	const QString           kTelnetAfterLine           = QStringLiteral("qxv-after-64");

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
	 * @brief Appends all currently available socket bytes to a buffer.
	 * @param socket Socket to drain.
	 * @param buffer Destination buffer.
	 * @return `true` when a socket was supplied.
	 */
	bool appendAvailableSocketBytes(QTcpSocket *socket, QByteArray &buffer)
	{
		if (!socket)
			return false;
		buffer.append(socket->readAll());
		return true;
	}

	/**
	 * @brief Creates a miniwindow with a font suitable for WindowOutputText regression checks.
	 * @param runtime Runtime that owns the miniwindow.
	 * @return `true` when the miniwindow and font were created.
	 */
	bool createWindowOutputTextTarget(WorldRuntime &runtime)
	{
		return runtime.windowCreate(QStringLiteral("output"), 0, 0, 320, 80, 0, 0, QColor(Qt::black),
		                            QString()) == eOK &&
		       runtime.windowFont(QStringLiteral("output"), QStringLiteral("font"),
		                          QStringLiteral("Sans Serif"), 10.0, false, false, false, false, 0,
		                          0) == eOK;
	}

	/**
	 * @brief Loads script text into a callback engine.
	 * @param engine Engine to initialize.
	 * @param script Lua script text.
	 * @return `true` when the script loaded.
	 */
	bool loadCallbackEngineScript(LuaCallbackEngine &engine, const QString &script)
	{
		engine.setPluginInfo(QStringLiteral("Plugin.Id"), QStringLiteral("Plugin Name"),
		                     QStringLiteral("/tmp/plugin"));
		engine.setScriptText(script);
		return engine.loadScript();
	}

	/**
	 * @brief Creates callback snapshot data for WindowOutputText shadow tests.
	 * @return Snapshot containing one miniwindow, one font, render context state, and one entity.
	 */
	QSharedPointer<LuaCallbackMiniWindowSnapshot> captureWindowOutputTextDispatchSnapshotForTest()
	{
		MiniWindow window;
		MiniWindowUtils::create(window, QStringLiteral("output"), 0, 0, 320, 80, 0, 0, QColor(Qt::black),
		                        QString());
		if (MiniWindowUtils::font(window, QStringLiteral("font"), QStringLiteral("Sans Serif"), 10.0, false,
		                          false, false, false, 0, 0) != eOK)
		{
			return {};
		}

		auto snapshot = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
		snapshot->windowNames.push_back(QStringLiteral("output"));
		snapshot->fontIdsByWindow.insert(QStringLiteral("output"), {QStringLiteral("font")});
		snapshot->miniWindowsByWindow.insert(QStringLiteral("output"),
		                                     QSharedPointer<MiniWindow>::create(window.detachedImageCopy()));
		LuaCallbackMiniWindowSnapshot::WindowInfoSnapshot windowInfo;
		windowInfo.width  = window.width;
		windowInfo.height = window.height;
		snapshot->windowInfoByWindow.insert(QStringLiteral("output"), windowInfo);
		snapshot->worldAttributesSnapshot.insert(QStringLiteral("use_mxp"), QStringLiteral("2"));
		snapshot->hasWorldAttributeSnapshot         = true;
		snapshot->hasEntitySnapshot                 = true;
		snapshot->hasWindowOutputTextRenderSnapshot = true;
		snapshot->entityValuesByName.insert(QStringLiteral("cmd"), QStringLiteral(" "));
		snapshot->rebuildMiniWindowLookupCaches();
		return snapshot;
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
	 * @brief Writes a plugin fixture that records MXP entity definition callback payloads.
	 * @param pluginsDir Plugin fixture directory.
	 * @return `true` when the plugin fixture was written.
	 */
	bool writeMxpEntityCallbackPlugin(const QString &pluginsDir)
	{
		const QString pluginPath = QDir(pluginsDir).filePath(QStringLiteral("mxp_entity_callbacks.xml"));
		return writeTextFile(pluginPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin
    name="MxpEntityCallbacks"
    author="QMud Test"
    id=")xml") + kMxpEntityCallbackPluginId + QStringLiteral(R"xml("
    language="lua"
    enabled="y"
    save_state="n"
    sequence="100">
    <script><![CDATA[
function OnPluginMXPsetEntity(payload)
  SetVariable("mxp_entity_payload", payload)
end

function OnPluginPartialLine(payload)
  local count = tonumber(GetVariable("partial_line_count") or "0") or 0
  SetVariable("partial_line_count", tostring(count + 1))
  SetVariable("partial_line_payload", payload)
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
	 * @brief Creates a direct script trigger that inserts note lines above the matched prompt.
	 * @param prompt Prompt text to match.
	 * @return Trigger fixture.
	 */
	WorldRuntime::Trigger makePromptNoteInjectionTrigger(const QString &prompt)
	{
		WorldRuntime::Trigger trigger;
		trigger.attributes.insert(QStringLiteral("enabled"), QStringLiteral("y"));
		trigger.attributes.insert(QStringLiteral("match"),
		                          QStringLiteral("^%1$").arg(QRegularExpression::escape(prompt)));
		trigger.attributes.insert(QStringLiteral("regexp"), QStringLiteral("y"));
		trigger.attributes.insert(QStringLiteral("send_to"), QString::number(eSendToScript));
		trigger.attributes.insert(QStringLiteral("sequence"), QStringLiteral("100"));
		trigger.children.insert(QStringLiteral("send"),
		                        QStringLiteral("Note(\"[695, 770]\")\nNote(\"0 0 +196\")"));
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
	 * @brief Verifies a two-line MXP send action resolved from closed body text.
	 * @param lines Runtime output buffer.
	 * @param expectedAction Expected resolved action text.
	 */
	void verifyTwoLineResolvedMxpAction(const IndexedRingBuffer<WorldRuntime::LineEntry> &lines,
	                                    const QString                                    &expectedAction)
	{
		QTRY_COMPARE_WITH_TIMEOUT(lines.size(), qsizetype{2}, 1000);
		QCOMPARE(lines.at(0).text, QStringLiteral("start-"));
		QCOMPARE(lines.at(1).text, QStringLiteral("newbie"));
		QVERIFY(!lines.at(0).spans.isEmpty());
		QVERIFY(!lines.at(1).spans.isEmpty());
		QCOMPARE(lines.at(0).spans.first().actionType, static_cast<int>(WorldRuntime::ActionSend));
		QCOMPARE(lines.at(1).spans.first().actionType, static_cast<int>(WorldRuntime::ActionSend));
		QCOMPARE(lines.at(0).spans.first().action, expectedAction);
		QCOMPARE(lines.at(1).spans.first().action, expectedAction);
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
		static void utf8CarrySurvivesInactiveLegacyEncodingChange()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("utf_8"), QStringLiteral("1"));
			runtime.setWorldAttribute(QStringLiteral("legacy_encoding"), QStringLiteral("windows-1252"));

			QSignalSpy lineSpy(&runtime, &WorldRuntime::incomingLineReceived);
			runtime.receiveRawData(QByteArray::fromHex("C3"));
			runtime.setWorldAttribute(QStringLiteral("legacy_encoding"), QStringLiteral("GB18030"));
			runtime.receiveRawData(QByteArray::fromHex("A90A"));

			QCOMPARE(lineSpy.count(), 1);
			QCOMPARE(lineSpy.takeFirst().at(0).toString(), QStringLiteral("é"));
		}

		static void legacyEncodingDecodesIncomingWorldBytes()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("utf_8"), QStringLiteral("0"));
			runtime.setWorldAttribute(QStringLiteral("legacy_encoding"), QStringLiteral("GB18030"));

			QSignalSpy lineSpy(&runtime, &WorldRuntime::incomingLineReceived);
			QByteArray payload = QByteArray::fromHex("D6D0CEC4");
			payload.append('\n');
			runtime.receiveRawData(payload);

			QCOMPARE(lineSpy.count(), 1);
			QCOMPARE(lineSpy.takeFirst().at(0).toString(), QStringLiteral("中文"));
		}

		static void outputColourCacheRefreshesAfterRuntimeColourChanges()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("utf_8"), QStringLiteral("1"));
			runtime.setWorldAttribute(QStringLiteral("output_background_colour"), QStringLiteral("#000000"));

			QString                          line;
			QVector<WorldRuntime::StyleSpan> spans;
			QObject::connect(&runtime, &WorldRuntime::incomingStyledLineReceived, &runtime,
			                 [&line, &spans](const QString                          &incomingLine,
			                                 const QVector<WorldRuntime::StyleSpan> &incomingSpans)
			                 {
				                 line  = incomingLine;
				                 spans = incomingSpans;
			                 });
			auto receiveLine = [&](const QByteArray &payload)
			{
				line.clear();
				spans.clear();
				runtime.receiveRawData(payload);
			};

			runtime.setWorldAttribute(QStringLiteral("output_text_colour"), QStringLiteral("#112233"));
			receiveLine(QByteArrayLiteral("\x1b[0mA\n"));
			QCOMPARE(line, QStringLiteral("A"));
			QVERIFY(!spans.isEmpty());
			QCOMPARE(spans.first().fore, QColor(QStringLiteral("#112233")));

			runtime.setWorldAttribute(QStringLiteral("output_text_colour"), QStringLiteral("#445566"));
			receiveLine(QByteArrayLiteral("\x1b[0mB\n"));
			QCOMPARE(line, QStringLiteral("B"));
			QVERIFY(!spans.isEmpty());
			QCOMPARE(spans.first().fore, QColor(QStringLiteral("#445566")));

			WorldRuntime::Colour red;
			red.group = QStringLiteral("ansi/normal");
			red.attributes.insert(QStringLiteral("seq"), QStringLiteral("2"));
			red.attributes.insert(QStringLiteral("rgb"), QStringLiteral("#123456"));
			runtime.setColours({red});
			receiveLine(QByteArrayLiteral("\x1b[0;31mC\n"));
			QCOMPARE(line, QStringLiteral("C"));
			QVERIFY(!spans.isEmpty());
			QCOMPARE(spans.first().fore, QColor(QStringLiteral("#123456")));

			runtime.setAnsiColour(false, 2, QColor(QStringLiteral("#654321")));
			receiveLine(QByteArrayLiteral("\x1b[0;31mD\n"));
			QCOMPARE(line, QStringLiteral("D"));
			QVERIFY(!spans.isEmpty());
			QCOMPARE(spans.first().fore, QColor(QStringLiteral("#654321")));

			runtime.setWorldAttribute(QStringLiteral("output_text_colour"), QString());
			runtime.setWorldAttribute(QStringLiteral("custom_16_is_default_colour"), QStringLiteral("1"));
			QCOMPARE(runtime.setCustomColourText(16, QColor(QStringLiteral("#abcdef"))), eOK);
			receiveLine(QByteArrayLiteral("\x1b[0mE\n"));
			QCOMPARE(line, QStringLiteral("E"));
			QVERIFY(!spans.isEmpty());
			QCOMPARE(spans.first().fore, QColor(QStringLiteral("#abcdef")));
		}

		static void legacyDecoderStateResetsWhenLegacyEncodingChanges()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("utf_8"), QStringLiteral("0"));
			runtime.setWorldAttribute(QStringLiteral("legacy_encoding"), QStringLiteral("GB18030"));

			QSignalSpy lineSpy(&runtime, &WorldRuntime::incomingLineReceived);
			runtime.receiveRawData(QByteArray::fromHex("D6"));
			runtime.setWorldAttribute(QStringLiteral("legacy_encoding"), QStringLiteral("windows-1252"));
			runtime.receiveRawData(QByteArrayLiteral("A\n"));

			QCOMPARE(lineSpy.count(), 1);
			QCOMPARE(lineSpy.takeFirst().at(0).toString(), QStringLiteral("A"));
		}

		static void legacyEncodingEncodesOutboundWorldCommands()
		{
			QTcpServer server;
			if (!server.listen(QHostAddress::LocalHost, 0))
				QSKIP("Local TCP listen is unavailable in this environment.");

			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("utf_8"), QStringLiteral("0"));
			runtime.setWorldAttribute(QStringLiteral("legacy_encoding"), QStringLiteral("GB18030"));

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

			QCOMPARE(runtime.sendCommand(QStringLiteral("中文"), false, false, true, false, false), eOK);
			const QByteArray expected = QByteArray::fromHex("D6D0CEC40D0A");
			QByteArray       received;
			QTRY_VERIFY_WITH_TIMEOUT(
			    (appendAvailableSocketBytes(acceptedSocket.data(), received), received.contains(expected)),
			    5000);
		}

		static void legacyEncodingEncodesDirectSendTextWithoutNewline()
		{
			QTcpServer server;
			if (!server.listen(QHostAddress::LocalHost, 0))
				QSKIP("Local TCP listen is unavailable in this environment.");

			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("utf_8"), QStringLiteral("0"));
			runtime.setWorldAttribute(QStringLiteral("legacy_encoding"), QStringLiteral("GB18030"));

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

			runtime.sendText(QStringLiteral("中文"), false);
			const QByteArray expected = QByteArray::fromHex("D6D0CEC4");
			QByteArray       received;
			QTRY_VERIFY_WITH_TIMEOUT(
			    (appendAvailableSocketBytes(acceptedSocket.data(), received), received.contains(expected)),
			    5000);
			QVERIFY(!received.contains(QByteArrayLiteral("\r\n")));
		}

		static void legacyEncodingDecodesServerMxpEntityValuesForApi()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("utf_8"), QStringLiteral("0"));
			runtime.setWorldAttribute(QStringLiteral("legacy_encoding"), QStringLiteral("GB18030"));
			runtime.setWorldAttribute(QStringLiteral("use_mxp"), QStringLiteral("2"));

			QByteArray payload = QByteArrayLiteral("\x1B[1z<!ENTITY server '");
			payload.append(QByteArray::fromHex("D6D0CEC4"));
			payload.append("'>");
			runtime.receiveRawData(payload);

			QCOMPARE(runtime.getEntityValue(QStringLiteral("server")), QStringLiteral("中文"));
			const QMap<QString, QString> snapshot = runtime.customEntitySnapshot();
			QCOMPARE(snapshot.value(QStringLiteral("server")), QStringLiteral("中文"));
			runtime.setWorldAttribute(QStringLiteral("legacy_encoding"), QStringLiteral("Big5"));
			QCOMPARE(runtime.getEntityValue(QStringLiteral("server")), QStringLiteral("中文"));
		}

		static void legacyEncodingExpandsMxpEntitiesAsWorldBytes()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("utf_8"), QStringLiteral("0"));
			runtime.setWorldAttribute(QStringLiteral("legacy_encoding"), QStringLiteral("GB18030"));
			runtime.setWorldAttribute(QStringLiteral("use_mxp"), QStringLiteral("2"));

			QString                          line;
			QVector<WorldRuntime::StyleSpan> spans;
			QObject::connect(&runtime, &WorldRuntime::incomingStyledLineReceived, &runtime,
			                 [&line, &spans](const QString                          &incomingLine,
			                                 const QVector<WorldRuntime::StyleSpan> &incomingSpans)
			                 {
				                 line  = incomingLine;
				                 spans = incomingSpans;
			                 });

			runtime.receiveRawData(QByteArrayLiteral(
			    "\x1B[1z<!ELEMENT hi '<send href=\"look &eacute; &#20013; &cmd;\">' OPEN>"));
			runtime.setEntityValue(QStringLiteral("cmd"), QStringLiteral("中文"));
			runtime.receiveRawData(QByteArrayLiteral("<hi>Link</hi>\n"));

			QCOMPARE(line, QStringLiteral("Link"));
			QVERIFY(!spans.isEmpty());
			QCOMPARE(spans.first().actionType, static_cast<int>(WorldRuntime::ActionSend));
			QCOMPARE(spans.first().action, QStringLiteral("look é 中 中文"));
		}

		static void mxpSendTextEntityResolvesFromClosedBody()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("use_mxp"), QStringLiteral("2"));

			QString                          line;
			QVector<WorldRuntime::StyleSpan> spans;
			QObject::connect(&runtime, &WorldRuntime::incomingStyledLineReceived, &runtime,
			                 [&line, &spans](const QString                          &incomingLine,
			                                 const QVector<WorldRuntime::StyleSpan> &incomingSpans)
			                 {
				                 line  = incomingLine;
				                 spans = incomingSpans;
			                 });

			runtime.receiveRawData(
			    QByteArrayLiteral("\x1B[1z<send href=\"help &text;\">start-newbie</send>\n"));

			QCOMPARE(line, QStringLiteral("start-newbie"));
			QVERIFY(!spans.isEmpty());
			QCOMPARE(spans.first().actionType, static_cast<int>(WorldRuntime::ActionSend));
			QCOMPARE(spans.first().action, QStringLiteral("help start-newbie"));
		}

		static void mxpSendTextEntityResolvesCurrentPartialLineWithExistingBuffer()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("use_mxp"), QStringLiteral("2"));

			constexpr int existingLineCount = 2048;
			for (int i = 0; i < existingLineCount; ++i)
				runtime.addLine(QStringLiteral("old line %1").arg(i), WorldRuntime::LineOutput);

			QString                          line;
			QVector<WorldRuntime::StyleSpan> spans;
			QObject::connect(&runtime, &WorldRuntime::incomingStyledLineReceived, &runtime,
			                 [&line, &spans](const QString                          &incomingLine,
			                                 const QVector<WorldRuntime::StyleSpan> &incomingSpans)
			                 {
				                 line  = incomingLine;
				                 spans = incomingSpans;
			                 });

			runtime.receiveRawData(
			    QByteArrayLiteral("\x1B[1z<send href=\"help &text;\">start-newbie</send>\n"));

			QCOMPARE(line, QStringLiteral("start-newbie"));
			QVERIFY(!spans.isEmpty());
			QCOMPARE(spans.first().actionType, static_cast<int>(WorldRuntime::ActionSend));
			QCOMPARE(spans.first().action, QStringLiteral("help start-newbie"));
		}

		static void mxpSendTextEntityIgnoresInsertedOutputWhileBodyStaysPartial()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("use_mxp"), QStringLiteral("2"));

			QString                          line;
			QVector<WorldRuntime::StyleSpan> spans;
			QObject::connect(&runtime, &WorldRuntime::incomingStyledLineReceived, &runtime,
			                 [&line, &spans](const QString                          &incomingLine,
			                                 const QVector<WorldRuntime::StyleSpan> &incomingSpans)
			                 {
				                 line  = incomingLine;
				                 spans = incomingSpans;
			                 });

			runtime.receiveRawData(QByteArrayLiteral("\x1B[1z<send href=\"help &text;\">start-"));
			runtime.addLine(QStringLiteral("inserted-output"), WorldRuntime::LineOutput);
			runtime.receiveRawData(QByteArrayLiteral("newbie</send>\n"));

			QCOMPARE(line, QStringLiteral("start-newbie"));
			QVERIFY(!spans.isEmpty());
			QCOMPARE(spans.first().actionType, static_cast<int>(WorldRuntime::ActionSend));
			QCOMPARE(spans.first().action, QStringLiteral("help start-newbie"));
		}

		static void mxpSendTextEntityTreatsCommittedPartialAsStaleCurrentLine()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("use_mxp"), QStringLiteral("2"));

			QString                          line;
			QVector<WorldRuntime::StyleSpan> spans;
			QObject::connect(&runtime, &WorldRuntime::incomingStyledLineReceived, &runtime,
			                 [&line, &spans](const QString                          &incomingLine,
			                                 const QVector<WorldRuntime::StyleSpan> &incomingSpans)
			                 {
				                 line  = incomingLine;
				                 spans = incomingSpans;
			                 });

			runtime.receiveRawData(QByteArrayLiteral("\x1B[1zprefix <send href=\"help &text;\">start-"));
			QVERIFY(runtime.commitPendingIncomingPartialLine());
			runtime.receiveRawData(QByteArrayLiteral("newbie</send>\n"));

			QCOMPARE(line, QStringLiteral("newbie"));
			QVERIFY(!spans.isEmpty());
			QCOMPARE(spans.first().actionType, static_cast<int>(WorldRuntime::ActionSend));
			QCOMPARE(spans.first().action, QStringLiteral("help start-newbie"));
		}

		static void mxpSendTextEntityReplacementIsNotRecursive()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("use_mxp"), QStringLiteral("2"));

			QString                          line;
			QVector<WorldRuntime::StyleSpan> spans;
			QObject::connect(&runtime, &WorldRuntime::incomingStyledLineReceived, &runtime,
			                 [&line, &spans](const QString                          &incomingLine,
			                                 const QVector<WorldRuntime::StyleSpan> &incomingSpans)
			                 {
				                 line  = incomingLine;
				                 spans = incomingSpans;
			                 });

			runtime.receiveRawData(
			    QByteArrayLiteral("\x1B[1z<send href=\"say &text;\">literal &amp;text;</send>\n"));

			QCOMPARE(line, QStringLiteral("literal &text;"));
			QVERIFY(!spans.isEmpty());
			QCOMPARE(spans.first().actionType, static_cast<int>(WorldRuntime::ActionSend));
			QCOMPARE(spans.first().action, QStringLiteral("say literal &text;"));
		}

		static void mxpCustomElementTextEntityResolvesFromClosedBody()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("use_mxp"), QStringLiteral("2"));

			QString                          line;
			QVector<WorldRuntime::StyleSpan> spans;
			QObject::connect(&runtime, &WorldRuntime::incomingStyledLineReceived, &runtime,
			                 [&line, &spans](const QString                          &incomingLine,
			                                 const QVector<WorldRuntime::StyleSpan> &incomingSpans)
			                 {
				                 line  = incomingLine;
				                 spans = incomingSpans;
			                 });

			runtime.receiveRawData(QByteArrayLiteral("\x1B[1z<!ELEMENT help '<send href=\"help &text;\">'>"));
			runtime.receiveRawData(QByteArrayLiteral("<help>start-newbie</help>\n"));

			QCOMPARE(line, QStringLiteral("start-newbie"));
			QVERIFY(!spans.isEmpty());
			QCOMPARE(spans.first().actionType, static_cast<int>(WorldRuntime::ActionSend));
			QCOMPARE(spans.first().action, QStringLiteral("help start-newbie"));
		}

		static void mxpSendTextEntityResolvesAcrossCompletedLines()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("use_mxp"), QStringLiteral("2"));
			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			runtime.receiveRawData(QByteArrayLiteral("\x1B[6z<send href=\"help &text;\">start-\n"));
			runtime.receiveRawData(QByteArrayLiteral("newbie</send>\n"));

			const IndexedRingBuffer<WorldRuntime::LineEntry> &lines = runtime.lines();
			verifyTwoLineResolvedMxpAction(lines, QStringLiteral("help start-\nnewbie"));
		}

		static void mxpSendTextEntityIgnoresCallbackLinesInsertedBeforeActionStart()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("use_mxp"), QStringLiteral("2"));
			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			runtime.receiveRawData(QByteArrayLiteral("\x1B[6z<send href=\"help &text;\">start-\n"));
			QTRY_COMPARE_WITH_TIMEOUT(runtime.lines().size(), qsizetype{1}, 1000);
			const qint64 actionStartLineNumber = runtime.lines().at(0).lineNumber;

			QVERIFY(runtime.writeLuaCallbackOutputAtLineAnchor(actionStartLineNumber, 0, false,
			                                                   QStringLiteral("callback"),
			                                                   WorldRuntime::LineNote, {}, true));
			runtime.receiveRawData(QByteArrayLiteral("newbie</send>\n"));

			const IndexedRingBuffer<WorldRuntime::LineEntry> &lines = runtime.lines();
			QTRY_COMPARE_WITH_TIMEOUT(lines.size(), qsizetype{3}, 1000);
			QCOMPARE(lines.at(0).text, QStringLiteral("callback"));
			QCOMPARE(lines.at(1).text, QStringLiteral("start-"));
			QCOMPARE(lines.at(2).text, QStringLiteral("newbie"));
			QVERIFY(lines.at(0).spans.isEmpty());
			QVERIFY(!lines.at(1).spans.isEmpty());
			QVERIFY(!lines.at(2).spans.isEmpty());
			QCOMPARE(lines.at(1).spans.first().action, QStringLiteral("help start-\nnewbie"));
			QCOMPARE(lines.at(2).spans.first().action, QStringLiteral("help start-\nnewbie"));
		}

		static void mxpCustomElementTextEntityResolvesAcrossCompletedLines()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("use_mxp"), QStringLiteral("2"));
			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			runtime.receiveRawData(QByteArrayLiteral("\x1B[6z<!ELEMENT help '<send href=\"help &text;\">'>"));
			runtime.receiveRawData(QByteArrayLiteral("<help>start-\n"));
			runtime.receiveRawData(QByteArrayLiteral("newbie</help>\n"));

			const IndexedRingBuffer<WorldRuntime::LineEntry> &lines = runtime.lines();
			verifyTwoLineResolvedMxpAction(lines, QStringLiteral("help start-\nnewbie"));
		}

		static void mxpSendTextEntityResolutionUsesCompleteBodyText()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("use_mxp"), QStringLiteral("2"));

			QString                          line;
			QVector<WorldRuntime::StyleSpan> spans;
			QObject::connect(&runtime, &WorldRuntime::incomingStyledLineReceived, &runtime,
			                 [&line, &spans](const QString                          &incomingLine,
			                                 const QVector<WorldRuntime::StyleSpan> &incomingSpans)
			                 {
				                 line  = incomingLine;
				                 spans = incomingSpans;
			                 });

			const QString body    = QString(1001, QLatin1Char('x'));
			QByteArray    payload = QByteArrayLiteral("\x1B[1z<send href=\"help &text;\">") + body.toUtf8() +
			                        QByteArrayLiteral("</send>\n");
			runtime.receiveRawData(payload);

			QCOMPARE(line, body);
			QVERIFY(!spans.isEmpty());
			QCOMPARE(spans.first().actionType, static_cast<int>(WorldRuntime::ActionSend));
			QCOMPARE(spans.first().action, QStringLiteral("help ") + body);
		}

		static void mxpSendWithoutHrefUsesClosedBodyAsAction()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("use_mxp"), QStringLiteral("2"));

			QString                          line;
			QVector<WorldRuntime::StyleSpan> spans;
			QObject::connect(&runtime, &WorldRuntime::incomingStyledLineReceived, &runtime,
			                 [&line, &spans](const QString                          &incomingLine,
			                                 const QVector<WorldRuntime::StyleSpan> &incomingSpans)
			                 {
				                 line  = incomingLine;
				                 spans = incomingSpans;
			                 });

			runtime.receiveRawData(QByteArrayLiteral("\x1B[1z<send>look portal</send>\n"));

			QCOMPARE(line, QStringLiteral("look portal"));
			QVERIFY(!spans.isEmpty());
			QCOMPARE(spans.first().actionType, static_cast<int>(WorldRuntime::ActionSend));
			QCOMPARE(spans.first().action, QStringLiteral("look portal"));
		}

		static void legacyEncodingExpandsBuiltinMxpEntityOutputAsWorldBytes()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("utf_8"), QStringLiteral("0"));
			runtime.setWorldAttribute(QStringLiteral("legacy_encoding"), QStringLiteral("GB18030"));
			runtime.setWorldAttribute(QStringLiteral("use_mxp"), QStringLiteral("2"));

			QSignalSpy lineSpy(&runtime, &WorldRuntime::incomingLineReceived);
			runtime.receiveRawData(QByteArrayLiteral("&eacute; &#233;\n"));

			QCOMPARE(lineSpy.count(), 1);
			QCOMPARE(lineSpy.takeFirst().at(0).toString(), QStringLiteral("é é"));
		}

		static void mxpAnsiStrikeSpansPreserved()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("use_mxp"), QStringLiteral("2"));

			QString                          line;
			QVector<WorldRuntime::StyleSpan> spans;
			QObject::connect(&runtime, &WorldRuntime::incomingStyledLineReceived, &runtime,
			                 [&line, &spans](const QString                          &incomingLine,
			                                 const QVector<WorldRuntime::StyleSpan> &incomingSpans)
			                 {
				                 line  = incomingLine;
				                 spans = incomingSpans;
			                 });

			runtime.receiveRawData(QByteArrayLiteral("\x1B[1zplain \x1B[9mstrike\x1B[29m plain\n"));

			QCOMPARE(line, QStringLiteral("plain strike plain"));
			QVERIFY(!spans.isEmpty());
			int offset = 0;
			for (const WorldRuntime::StyleSpan &span : std::as_const(spans))
			{
				for (int i = offset; i < offset + span.length; ++i)
				{
					constexpr int strikeStart    = 6;
					constexpr int strikeEnd      = 12;
					const bool    expectedStrike = i >= strikeStart && i < strikeEnd;
					QCOMPARE(span.strike, expectedStrike);
				}
				offset += span.length;
			}
			QCOMPARE(offset, static_cast<int>(line.size()));
		}

		static void mxpResetPromptPartialIsPresentedWithoutLineTerminator()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("use_mxp"), QStringLiteral("0"));
			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			QStringList partialLines;
			QObject::connect(
			    &runtime, &WorldRuntime::incomingStyledLinePartialReceived, &runtime,
			    [&partialLines](const QString &line, const QVector<WorldRuntime::StyleSpan> & /*spans*/)
			    { partialLines.push_back(line); });

			runtime.receiveRawData(bytes({IAC, SB, 91, IAC, SE}));

			QByteArray payload = QByteArrayLiteral("There are 10 characters on.\r\n\r\n");
			payload.append(
			    QByteArrayLiteral("\x1B[3z\x1B[0;32m\x1B[1;33m<Ex>N</Ex><Ex>Sw</Ex>\x1B[0;32m 2:30pm "
			                      "\x1B[1;31m32/32hp \x1B[1;34m100/100m \x1B[0;35m300mv "
			                      "\x1B[0;32m2000xp\x1B[0;32m&gt; \x1B[0;37m"));
			runtime.receiveRawData(payload);

			const QString expectedPrompt = QStringLiteral("NSw 2:30pm 32/32hp 100/100m 300mv 2000xp> ");
			QTRY_VERIFY(!partialLines.isEmpty());
			QCOMPARE(partialLines.constLast(), expectedPrompt);
			auto outputLastLine = [&harness]
			{
				const QStringList lines = harness.view.outputLines();
				return lines.isEmpty() ? QString() : lines.constLast();
			};
			QTRY_COMPARE(outputLastLine(), expectedPrompt);
		}

		static void legacyEncodingMxpSetEntityCallbackPayloadUsesInternalUtf8()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("worlds/plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));
			QVERIFY(writeMxpEntityCallbackPlugin(pluginsDir));

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
			runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
			runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
			runtime.setWorldAttribute(QStringLiteral("utf_8"), QStringLiteral("0"));
			runtime.setWorldAttribute(QStringLiteral("legacy_encoding"), QStringLiteral("GB18030"));
			runtime.setWorldAttribute(QStringLiteral("use_mxp"), QStringLiteral("2"));

			WorldChildWindow window(QStringLiteral("MXP Entity Callback"));
			window.resize(640, 480);
			window.setRuntime(&runtime);
			window.show();
			QVERIFY(QTest::qWaitForWindowExposed(&window));

			QString loadError;
			QVERIFY2(runtime.loadPluginFile(QStringLiteral("mxp_entity_callbacks.xml"), &loadError),
			         qPrintable(loadError));
			QTRY_VERIFY_WITH_TIMEOUT(
			    !runtime.plugins().isEmpty() && !runtime.plugins().constFirst().installPending, 5000);

			runtime.receiveRawData(QByteArrayLiteral("partial"));
			QTRY_COMPARE_WITH_TIMEOUT(
			    pluginVariable(runtime, kMxpEntityCallbackPluginId, QStringLiteral("partial_line_count")),
			    QStringLiteral("1"), 5000);
			QCOMPARE(
			    pluginVariable(runtime, kMxpEntityCallbackPluginId, QStringLiteral("partial_line_payload")),
			    QStringLiteral("partial"));

			QByteArray payload = QByteArrayLiteral("\x1B[1z<!ENTITY server '");
			payload.append(QByteArray::fromHex("D6D0CEC4"));
			payload.append("'>");
			runtime.receiveRawData(payload);

			QTRY_COMPARE_WITH_TIMEOUT(
			    pluginVariable(runtime, kMxpEntityCallbackPluginId, QStringLiteral("mxp_entity_payload")),
			    QStringLiteral("server=中文"), 5000);
			QCOMPARE(
			    pluginVariable(runtime, kMxpEntityCallbackPluginId, QStringLiteral("partial_line_count")),
			    QStringLiteral("1"));
		}

		static void legacyEncodingExpandsMiniWindowMxpEntitiesAsInternalText()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("utf_8"), QStringLiteral("0"));
			runtime.setWorldAttribute(QStringLiteral("legacy_encoding"), QStringLiteral("GB18030"));
			runtime.setWorldAttribute(QStringLiteral("use_mxp"), QStringLiteral("2"));
			QVERIFY(createWindowOutputTextTarget(runtime));

			QByteArray payload = QByteArrayLiteral("\x1B[1z<!ENTITY cmd '");
			payload.append(QByteArray::fromHex("D6D0CEC4"));
			payload.append("'>");
			runtime.receiveRawData(payload);
			runtime.setWorldAttribute(QStringLiteral("legacy_encoding"), QStringLiteral("Big5"));
			QSignalSpy actionSpy(&runtime, &WorldRuntime::miniWindowOutputActionActivated);
			QVERIFY(actionSpy.isValid());

			WorldRuntime::WindowOutputMetrics metrics;
			const int                         width = runtime.windowOutputText(
			    QStringLiteral("output"), QStringLiteral("font"),
			    QStringLiteral("\3send href=\"look &cmd;\"\4Link\3/send\4"), 0, 0, 319, 79, 0x00FFFFFF,
			    QString(), QStringLiteral("link"), QString(), &metrics);

			QVERIFY(width >= 0);
			QCOMPARE(metrics.hotspotCount, 1);
			const QStringList hotspots = runtime.windowHotspotList(QStringLiteral("output"));
			QCOMPARE(hotspots.size(), 1);
			QCOMPARE(runtime.windowOutputActivate(QStringLiteral("output"), hotspots.first(), false), eOK);
			QCOMPARE(actionSpy.count(), 1);
			const QList<QVariant> action = actionSpy.takeFirst();
			QCOMPARE(action.at(0).toInt(), static_cast<int>(WorldRuntime::ActionSend));
			QCOMPARE(action.at(1).toString(), QStringLiteral("look 中文"));
		}

		static void callbackWindowOutputTextUsesEntityDeltaForShadowActivation()
		{
			WorldRuntime runtime;
			auto         engine = QSharedPointer<LuaCallbackEngine>::create();
			engine->setWorldRuntime(&runtime);
			QVERIFY(loadCallbackEngineScript(*engine, QStringLiteral(R"lua(
activation_status = ""
function OnPluginEnable()
  SetEntity("cmd", "look")
  local text = string.char(3) .. 'send href="&cmd;"' .. string.char(4) ..
    'Link' .. string.char(3) .. '/send' .. string.char(4)
  local render_result, metrics = WindowOutputText("output", "font", text, 0, 0, 319, 79, 0xFFFFFF, "", "link")
  local hotspots = WindowHotspotList("output") or {}
  local activate_result = #hotspots == 1 and WindowOutputActivate("output", hotspots[1]) or eHotspotNotInstalled
  activation_status = string.format("%s|%d|%d",
    tostring(render_result >= 0),
    metrics.hotspot_count or -1,
    activate_result or -1)
end
function activation_result(value)
  return activation_status
end
)lua")));

			const auto snapshot = captureWindowOutputTextDispatchSnapshotForTest();
			QVERIFY(snapshot);

			LuaExecutorDirect       executor;
			LuaBatchDispatchRequest request;
			request.engines               = {engine};
			request.kind                  = LuaBatchDispatchKind::NoArgs;
			request.functionName          = QStringLiteral("OnPluginEnable");
			request.miniWindowSnapshotArg = snapshot;
			static_cast<void>(executor.dispatchBatch(request));

			request.kind                        = LuaBatchDispatchKind::StringInOut;
			request.functionName                = QStringLiteral("activation_result");
			request.stringArg                   = QStringLiteral("ignored");
			const LuaBatchDispatchResult result = executor.dispatchBatch(request);
			QCOMPARE(result.stringResult, QStringLiteral("true|1|0"));
		}

		static void legacyEncodingExpandsMiniWindowCustomMxpElementsAsInternalText()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("utf_8"), QStringLiteral("0"));
			runtime.setWorldAttribute(QStringLiteral("legacy_encoding"), QStringLiteral("GB18030"));
			runtime.setWorldAttribute(QStringLiteral("use_mxp"), QStringLiteral("2"));
			QVERIFY(createWindowOutputTextTarget(runtime));

			QByteArray payload = QByteArrayLiteral("\x1B[1z<!ELEMENT hi '<send href=\"look ");
			payload.append(QByteArray::fromHex("D6D0CEC4"));
			payload.append(QByteArrayLiteral("\">' OPEN>"));
			runtime.receiveRawData(payload);
			runtime.setWorldAttribute(QStringLiteral("legacy_encoding"), QStringLiteral("Big5"));

			QSignalSpy actionSpy(&runtime, &WorldRuntime::miniWindowOutputActionActivated);
			QVERIFY(actionSpy.isValid());

			WorldRuntime::WindowOutputMetrics metrics;
			const int                         width = runtime.windowOutputText(
			    QStringLiteral("output"), QStringLiteral("font"), QStringLiteral("\3hi\4Link\3/hi\4"), 0, 0,
			    319, 79, 0x00FFFFFF, QString(), QStringLiteral("link"), QString(), &metrics);

			QVERIFY(width >= 0);
			QCOMPARE(metrics.hotspotCount, 1);
			const QStringList hotspots = runtime.windowHotspotList(QStringLiteral("output"));
			QCOMPARE(hotspots.size(), 1);
			QCOMPARE(runtime.windowOutputActivate(QStringLiteral("output"), hotspots.first(), false), eOK);
			QCOMPARE(actionSpy.count(), 1);
			const QList<QVariant> action = actionSpy.takeFirst();
			QCOMPARE(action.at(0).toInt(), static_cast<int>(WorldRuntime::ActionSend));
			QCOMPARE(action.at(1).toString(), QStringLiteral("look 中文"));
		}

		static void miniWindowMxpTextEntityResolvesFromClosedBody()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("use_mxp"), QStringLiteral("2"));
			QVERIFY(createWindowOutputTextTarget(runtime));

			QSignalSpy actionSpy(&runtime, &WorldRuntime::miniWindowOutputActionActivated);
			QVERIFY(actionSpy.isValid());

			WorldRuntime::WindowOutputMetrics metrics;
			const int                         width = runtime.windowOutputText(
			    QStringLiteral("output"), QStringLiteral("font"),
			    QStringLiteral("\3send href=\"help &text;\"\4start-newbie\3/send\4"), 0, 0, 319, 79,
			    0x00FFFFFF, QString(), QStringLiteral("link"), QString(), &metrics);

			QVERIFY(width >= 0);
			QCOMPARE(metrics.hotspotCount, 1);
			const QStringList hotspots = runtime.windowHotspotList(QStringLiteral("output"));
			QCOMPARE(hotspots.size(), 1);
			QCOMPARE(runtime.windowOutputActivate(QStringLiteral("output"), hotspots.first(), false), eOK);
			QCOMPARE(actionSpy.count(), 1);
			const QList<QVariant> action = actionSpy.takeFirst();
			QCOMPARE(action.at(0).toInt(), static_cast<int>(WorldRuntime::ActionSend));
			QCOMPARE(action.at(1).toString(), QStringLiteral("help start-newbie"));
		}

		static void miniWindowUnclosedTextEntityActionIsNotHotspot()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("use_mxp"), QStringLiteral("2"));
			QVERIFY(createWindowOutputTextTarget(runtime));

			WorldRuntime::WindowOutputMetrics metrics;
			const int                         width = runtime.windowOutputText(
			    QStringLiteral("output"), QStringLiteral("font"),
			    QStringLiteral("\3send href=\"help &text;\"\4start-newbie"), 0, 0, 319, 79, 0x00FFFFFF,
			    QString(), QStringLiteral("link"), QString(), &metrics);

			QVERIFY(width >= 0);
			QCOMPARE(metrics.hotspotCount, 0);
			QVERIFY(runtime.windowHotspotList(QStringLiteral("output")).isEmpty());
		}

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

		static void promptPartialCommitPresentsTriggerInjectedLinesWithoutBlankRows()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("enable_triggers"), QStringLiteral("y"));
			runtime.setWorldAttribute(QStringLiteral("enable_trigger_sounds"), QStringLiteral("n"));
			runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
			runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));

			const QString prompt = QStringLiteral("[SAFE]<2083hp 2220sp 1990st> ");
			runtime.triggersMutable().push_back(makePromptNoteInjectionTrigger(prompt));
			runtime.markTriggersChanged();

			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			harness.processor.onIncomingStyledLineReceived(QStringLiteral("(Mount: 2614st)"), {});
			QTRY_VERIFY_WITH_TIMEOUT(harness.view.outputLines().contains(QStringLiteral("(Mount: 2614st)")),
			                         5000);

			harness.processor.onIncomingStyledLinePartialReceived(prompt, {});
			QTRY_COMPARE_WITH_TIMEOUT(harness.view.outputLines().constLast(), prompt, 5000);

			harness.processor.onIncomingStyledLineReceived(prompt, {});
			auto tailLinesMatch = [&harness, &prompt]
			{
				const QStringList lines = harness.view.outputLines();
				if (lines.size() < 4)
					return false;
				const QStringList tailLines = lines.sliced(lines.size() - 4);
				return tailLines == QStringList{QStringLiteral("(Mount: 2614st)"),
				                                QStringLiteral("[695, 770]"), QStringLiteral("0 0 +196"),
				                                prompt} &&
				       std::ranges::none_of(tailLines, [](const QString &line) { return line.isEmpty(); });
			};
			QTRY_VERIFY_WITH_TIMEOUT(tailLinesMatch(), 5000);
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

		static void mushReaderPluginInfoEnabledIgnoresQtAccessibilitySpeechToggle()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString worldsDir  = QDir(tempDir.path()).filePath(QStringLiteral("worlds"));
			const QString pluginsDir = QDir(worldsDir).filePath(QStringLiteral("plugins"));
			const QString stateDir   = QDir(tempDir.path()).filePath(QStringLiteral("state"));
			QVERIFY(QDir().mkpath(pluginsDir));
			QVERIFY(QDir().mkpath(stateDir));

			const QString worldPath = QDir(worldsDir).filePath(QStringLiteral("native_disabled.qdl"));
			QVERIFY(writeTextFile(worldPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<qmud>
  <world id="bbbbbbbbbbbbbbbbbbbbbbbb" name="Native Disabled"/>
  <include name="qmud:native/MushReader" plugin="y" enabled="n"/>
</qmud>
)xml")));

			WorldDocument doc;
			QVERIFY2(doc.loadFromFile(worldPath), qPrintable(doc.errorString()));
			QVERIFY2(doc.expandIncludes(worldPath, pluginsDir, tempDir.path(), stateDir),
			         qPrintable(doc.errorString()));

			QVector<QMudNativePluginRegistry::TestSpeechEvent> speechEvents;
			QMudNativePluginRegistry::setTestSpeechSink(
			    [&speechEvents](const QMudNativePluginRegistry::TestSpeechEvent &event)
			    { speechEvents.push_back(event); });
			const auto restoreSpeechSink =
			    qScopeGuard([] { QMudNativePluginRegistry::setTestSpeechSink({}); });

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
			runtime.setStateFilesDirectory(stateDir);
			runtime.applyFromDocument(doc);
			speechEvents.clear();

			const QString mushReaderId = QMudNativePluginRegistry::mushReaderPluginId();
			QCOMPARE(runtime.plugins().size(), 1);
			QCOMPARE(runtime.plugins().constFirst().attributes.value(QStringLiteral("id")), mushReaderId);
			QVERIFY(!runtime.plugins().constFirst().enabled);
			QVERIFY(!runtime.hasMushReaderLiveSpeechOwner());
			QVERIFY(runtime.isQtAccessibilitySpeechEnabled());
			QVERIFY(runtime.isPluginInstalled(mushReaderId));
			QVERIFY(runtime.pluginIdList().contains(mushReaderId, Qt::CaseInsensitive));
			QCOMPARE(runtime.pluginInfo(mushReaderId, 17).toBool(), false);
			QCOMPARE(runtime.pluginSupports(mushReaderId, QStringLiteral("say")), eOK);

			WorldRuntime absentMushReaderRuntime;
			QVERIFY(!absentMushReaderRuntime.isPluginInstalled(mushReaderId));
			QVERIFY(!absentMushReaderRuntime.pluginIdList().contains(mushReaderId, Qt::CaseInsensitive));
			QVERIFY(!absentMushReaderRuntime.pluginInfo(mushReaderId, 1).isValid());
			QCOMPARE(absentMushReaderRuntime.pluginSupports(mushReaderId, QStringLiteral("say")),
			         eNoSuchPlugin);
			QCOMPARE(absentMushReaderRuntime.callPlugin(mushReaderId, QStringLiteral("say"),
			                                            QStringLiteral("absent")),
			         eNoSuchPlugin);
			QVERIFY(speechEvents.isEmpty());

			QCOMPARE(runtime.callPlugin(mushReaderId, QStringLiteral("say"), QStringLiteral("blocked")),
			         ePluginDisabled);
			QVERIFY(speechEvents.isEmpty());

#ifdef QMUD_ENABLE_LUA_SCRIPTING
			lua_State *luaState = luaL_newstate();
			QVERIFY(luaState != nullptr);
			const auto closeLuaState = qScopeGuard([luaState] { lua_close(luaState); });

			lua_pushstring(luaState, "lua blocked");
			QCOMPARE(runtime.callPluginLua(mushReaderId, QStringLiteral("say"), luaState, 1), 2);
			QCOMPARE(lua_gettop(luaState), 3);
			QCOMPARE(static_cast<int>(lua_tointeger(luaState, -2)), ePluginDisabled);
			QVERIFY(QString::fromUtf8(lua_tostring(luaState, -1)).contains(QStringLiteral("disabled")));
			lua_settop(luaState, 0);
			QVERIFY(speechEvents.isEmpty());

			lua_pushstring(luaState, "lua absent");
			QCOMPARE(absentMushReaderRuntime.callPluginLua(mushReaderId, QStringLiteral("say"), luaState, 1),
			         2);
			QCOMPARE(lua_gettop(luaState), 3);
			QCOMPARE(static_cast<int>(lua_tointeger(luaState, -2)), eNoSuchPlugin);
			lua_settop(luaState, 0);
			QVERIFY(speechEvents.isEmpty());

			lua_newtable(luaState);
			QCOMPARE(runtime.callPluginLua(mushReaderId, QStringLiteral("say"), luaState, 1), 2);
			QCOMPARE(lua_gettop(luaState), 3);
			QCOMPARE(static_cast<int>(lua_tointeger(luaState, -2)), ePluginDisabled);
			lua_settop(luaState, 0);
			QVERIFY(speechEvents.isEmpty());
#endif

			QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("tts")));
			QVERIFY(!runtime.isQtAccessibilitySpeechEnabled());
			QVERIFY(!runtime.hasMushReaderLiveSpeechOwner());
			QCOMPARE(runtime.pluginInfo(mushReaderId, 17).toBool(), false);

			QVERIFY(runtime.enablePlugin(mushReaderId, true));
			QVERIFY(runtime.hasMushReaderLiveSpeechOwner());
			QVERIFY(!runtime.isQtAccessibilitySpeechEnabled());
			QCOMPARE(runtime.pluginInfo(mushReaderId, 17).toBool(), true);
			QCOMPARE(runtime.callPlugin(mushReaderId, QStringLiteral("say"), QStringLiteral("direct")), eOK);
			QCOMPARE(speechEvents.size(), 1);
			QCOMPARE(speechEvents.constLast().text, QStringLiteral("       direct"));
			speechEvents.clear();

#ifdef QMUD_ENABLE_LUA_SCRIPTING
			lua_pushstring(luaState, "lua");
			QCOMPARE(runtime.callPluginLua(mushReaderId, QStringLiteral("say"), luaState, 1), 1);
			QCOMPARE(lua_gettop(luaState), 2);
			QCOMPARE(static_cast<int>(lua_tointeger(luaState, -1)), eOK);
			lua_settop(luaState, 0);
			QCOMPARE(speechEvents.size(), 1);
			QCOMPARE(speechEvents.constLast().text, QStringLiteral("       lua"));
			speechEvents.clear();
#endif

			QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("tts")));
			QVERIFY(runtime.hasMushReaderLiveSpeechOwner());
			QCOMPARE(runtime.pluginInfo(mushReaderId, 17).toBool(), true);

			QVERIFY(runtime.enablePlugin(mushReaderId, false));
			QVERIFY(!runtime.hasMushReaderLiveSpeechOwner());
			QCOMPARE(runtime.pluginInfo(mushReaderId, 17).toBool(), false);
		}

		static void luaAudioNativeCallPluginRespectsShadowEnabledState()
		{
			const QString audioId = QMudNativePluginRegistry::luaAudioPluginId();

			WorldRuntime  absentRuntime;
			QVERIFY(absentRuntime.isPluginInstalled(audioId));
			QVERIFY(absentRuntime.pluginIdList().contains(audioId, Qt::CaseInsensitive));
			QCOMPARE(absentRuntime.pluginInfo(audioId, 17).toBool(), true);
			QCOMPARE(absentRuntime.pluginSupports(audioId, QStringLiteral("plugin_update_url")), eOK);
			QCOMPARE(absentRuntime.callPlugin(audioId, QStringLiteral("plugin_update_url"), QString()), eOK);

			WorldRuntime::Plugin audioPlugin;
			audioPlugin.enabled    = false;
			audioPlugin.nativeShim = true;
			audioPlugin.attributes.insert(QStringLiteral("id"), audioId);
			audioPlugin.attributes.insert(QStringLiteral("name"), QStringLiteral("LuaAudio"));

			WorldRuntime runtime;
			runtime.pluginsMutable().push_back(audioPlugin);
			QCOMPARE(runtime.pluginInfo(audioId, 17).toBool(), false);
			QCOMPARE(runtime.callPlugin(audioId, QStringLiteral("plugin_update_url"), QString()),
			         ePluginDisabled);

#ifdef QMUD_ENABLE_LUA_SCRIPTING
			lua_State *luaState = luaL_newstate();
			QVERIFY(luaState != nullptr);
			const auto closeLuaState = qScopeGuard([luaState] { lua_close(luaState); });

			lua_newtable(luaState);
			QCOMPARE(runtime.callPluginLua(audioId, QStringLiteral("plugin_update_url"), luaState, 1), 2);
			QCOMPARE(lua_gettop(luaState), 3);
			QCOMPARE(static_cast<int>(lua_tointeger(luaState, -2)), ePluginDisabled);
			lua_settop(luaState, 0);
#endif

			QVERIFY(runtime.enablePlugin(audioId, true));
			QCOMPARE(runtime.pluginInfo(audioId, 17).toBool(), true);
			QCOMPARE(runtime.callPlugin(audioId, QStringLiteral("plugin_update_url"), QString()), eOK);

#ifdef QMUD_ENABLE_LUA_SCRIPTING
			QCOMPARE(runtime.callPluginLua(audioId, QStringLiteral("plugin_update_url"), luaState, 1), 2);
			QCOMPARE(lua_gettop(luaState), 2);
			QCOMPARE(static_cast<int>(lua_tointeger(luaState, -2)), eOK);
			QCOMPARE(QString::fromUtf8(lua_tostring(luaState, -1)), QStringLiteral("qmud:native/LuaAudio"));
#endif
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
