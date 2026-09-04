/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldRuntime_PluginLifecycle.cpp
 * Role: Integration coverage for WorldRuntime plugin lifecycle callback ordering.
 */

#include "AppController.h"
#include "LuaCallbackEngine.h"
#include "LuaExecutor.h"
#include "MainFrame.h"
#include "MiniWindow.h"
#include "NativePluginRegistry.h"
#include "WorldChildWindow.h"
#include "WorldCommandProcessor.h"
#include "WorldCommandProcessorUtils.h"
#include "WorldDocument.h"
#include "WorldOptions.h"
#include "WorldRuntime.h"
#include "WorldRuntimeTestAccess.h"
#include "WorldView.h"
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
#include <algorithm>

#ifdef QMUD_ENABLE_LUA_SCRIPTING
extern "C"
{
#include <lauxlib.h>
#include <lua.h>
}
#endif

namespace
{
	const QString           kDeferredConnectPluginId      = QStringLiteral("abcdeffedcbaabcdeffedcba");
	const QString           kTeardownStatePluginId        = QStringLiteral("fedcbaabcdeffedcbaabcdef");
	const QString           kHiddenMessagePluginId        = QStringLiteral("112233445566778899aabbcc");
	const QString           kNestedCallPluginId           = QStringLiteral("2233445566778899aabbccdd");
	const QString           kTelnetOrderingPluginId       = QStringLiteral("00112233445566778899aabb");
	const QString           kTimerCommandPluginId         = QStringLiteral("33445566778899aabbccddee");
	const QString           kFocusCallbackPluginId        = QStringLiteral("445566778899aabbccddeeff");
	const QString           kMxpEntityCallbackPluginId    = QStringLiteral("5566778899aabbccddeeff00");
	const QString           kSnapshotMutatorPluginId      = QStringLiteral("66778899aabbccddeeff0011");
	const QString           kSnapshotObserverPluginId     = QStringLiteral("778899aabbccddeeff001122");
	const QString           kDispatchPolicyFirstPluginId  = QStringLiteral("8899aabbccddeeff00112233");
	const QString           kDispatchPolicySecondPluginId = QStringLiteral("99aabbccddeeff0011223344");
	const QString           kDispatchPolicyThirdPluginId  = QStringLiteral("aabbccddeeff001122334455");
	const QString           kInstallDrawPluginId          = QStringLiteral("bbccddeeff00112233445566");
	const QString           kEligibilityMutatorPluginId   = QStringLiteral("ccddee001122334455667788");
	const QString           kEligibilityVictimPluginId    = QStringLiteral("ddee00112233445566778899");
	const QString           kEligibilityRecorderPluginId  = QStringLiteral("ee00112233445566778899aa");
	const QString           kTelnetTriggerLine            = QStringLiteral("qxv-lattice-17");
	const QString           kTelnetAfterLine              = QStringLiteral("qxv-after-64");

	constexpr unsigned char IAC   = 0xFF;
	constexpr unsigned char SB    = 0xFA;
	constexpr unsigned char SE    = 0xF0;
	constexpr unsigned char GMCP  = 201;
	constexpr unsigned char MCCP2 = 86;

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
	 * @brief Activates an MCCP v2 stream without supplying compressed payload bytes.
	 * @param runtime Runtime whose telnet processor receives the activation sequence.
	 */
	void activateMccp2(WorldRuntime &runtime)
	{
		runtime.receiveRawData(bytes({IAC, SB, MCCP2, IAC, SE}));
	}

	/**
	 * @brief Produces a complete raw zlib stream suitable for MCCP input.
	 * @param payload Uncompressed stream payload.
	 * @return Raw zlib stream without Qt's uncompressed-size prefix.
	 */
	QByteArray completeMccpPayload(const QByteArray &payload)
	{
		QByteArray compressed = qCompress(payload);
		compressed.remove(0, 4);
		return compressed;
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
	 * @brief Writes a minimal plugin with a declared dispatch sequence.
	 * @param pluginsDir Destination plugin directory.
	 * @param fileName Plugin file name.
	 * @param name Plugin display name.
	 * @param id Plugin identifier.
	 * @param sequence Declared plugin sequence.
	 * @return `true` when the fixture was written completely.
	 */
	bool writeSequencedPlugin(const QString &pluginsDir, const QString &fileName, const QString &name,
	                          const QString &id, const int sequence)
	{
		return writeTextFile(QDir(pluginsDir).filePath(fileName),
		                     QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<qmud>
  <plugin name="%1" id="%2" sequence="%3"/>
</qmud>
)xml")
		                         .arg(name, id, QString::number(sequence)));
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
		engine.setPluginInfo(QStringLiteral("plugin.id"), QStringLiteral("Plugin Name"),
		                     QStringLiteral("/tmp/plugin"));
		engine.setScriptText(script);
		return engine.loadScript();
	}

	QSharedPointer<LuaCallbackEngine> addDirectCallbackPlugin(WorldRuntime &runtime, const QString &id,
	                                                          const QString &name, const QString &script)
	{
		auto engine = QSharedPointer<LuaCallbackEngine>::create();
		engine->setWorldRuntime(&runtime);
		engine->setPluginInfo(id, name, QString());
		engine->setScriptText(script);
		WorldRuntime::Plugin plugin;
		plugin.attributes.insert(QStringLiteral("id"), id);
		plugin.attributes.insert(QStringLiteral("name"), name);
		plugin.attributes.insert(QStringLiteral("language"), QStringLiteral("Lua"));
		plugin.attributes.insert(QStringLiteral("enabled"), QStringLiteral("y"));
		plugin.enabled = true;
		plugin.lua     = engine;
		WorldRuntimeTestAccess::plugins(runtime).push_back(std::move(plugin));
		return engine;
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
	 * @brief Writes ordered callbacks that mutate output, suspend, and inspect the following snapshot.
	 * @param pluginsDir Plugin fixture directory.
	 * @return `true` when both plugin fixtures were written.
	 */
	bool writeSuspendedRecipientSnapshotPlugins(const QString &pluginsDir)
	{
		const QString mutatorPath  = QDir(pluginsDir).filePath(QStringLiteral("snapshot_mutator.xml"));
		const QString observerPath = QDir(pluginsDir).filePath(QStringLiteral("snapshot_observer.xml"));
		const bool    mutatorWritten =
		    writeTextFile(mutatorPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin
    name="SnapshotMutator"
    author="QMud Test"
    id=")xml") + kSnapshotMutatorPluginId + QStringLiteral(R"xml("
    language="lua"
    enabled="y"
    save_state="n"
    sequence="100">
    <script><![CDATA[
function OnPluginConnect()
  Note("fresh callback output")
  GetLineInfo(1, 1)
end
function OnPluginDisconnect()
  Note("fresh unsuspended callback output")
end
]]></script>
  </plugin>
</muclient>
)xml"));
		const bool observerWritten =
		    writeTextFile(observerPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin
    name="SnapshotObserver"
    author="QMud Test"
    id=")xml") + kSnapshotObserverPluginId + QStringLiteral(R"xml("
    language="lua"
    enabled="y"
    save_state="n"
    sequence="200">
    <script><![CDATA[
function OnPluginConnect()
  local count = GetLinesInBufferCount()
  SetVariable("observed_count", string.format("%.0f", count))
  SetVariable("observed_last_line", GetLineInfo(count, 1) or "<nil>")
end
function OnPluginDisconnect()
  local count = GetLinesInBufferCount()
  SetVariable("observed_unsuspended_count", string.format("%.0f", count))
  SetVariable("observed_unsuspended_last_line", GetLineInfo(count, 1) or "<nil>")
end
]]></script>
  </plugin>
</muclient>
)xml"));
		return mutatorWritten && observerWritten;
	}

	/**
	 * @brief Writes ordered callbacks covering stop-policy continuations after mutation and suspension.
	 * @param pluginsDir Plugin fixture directory.
	 * @return `true` when all plugin fixtures were written.
	 */
	bool writeDispatchContinuationPolicyPlugins(const QString &pluginsDir)
	{
		const auto writePlugin = [&](const QString &fileName, const QString &name, const QString &id,
		                             const int sequence, const QString &script)
		{
			return writeTextFile(
			    QDir(pluginsDir).filePath(fileName),
			    QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
			                   "<muclient>\n"
			                   "  <plugin name=\"%1\" author=\"QMud Test\" id=\"%2\" language=\"lua\" "
			                   "enabled=\"y\" save_state=\"n\" sequence=\"%3\">\n"
			                   "    <script><![CDATA[%4]]></script>\n"
			                   "  </plugin>\n"
			                   "</muclient>\n")
			        .arg(name, id, QString::number(sequence), script));
		};

		const bool firstWritten =
		    writePlugin(QStringLiteral("dispatch_policy_first.xml"), QStringLiteral("DispatchPolicyFirst"),
		                kDispatchPolicyFirstPluginId, 100, QStringLiteral(R"lua(
function OnPluginCommand(command)
  Note("dispatch policy mutation boundary")
  return true
end

function OnPluginTrace(message)
  GetLineInfo(1, 1)
  SetVariable("trace_resumed", "yes")
  return false
end

function OnPluginBroadcast(message, calling_id, calling_name, text)
  GetLineInfo(120, 1)
  SetVariable("broadcast_resumed", "yes")
end
)lua"));
		const bool secondWritten =
		    writePlugin(QStringLiteral("dispatch_policy_second.xml"), QStringLiteral("DispatchPolicySecond"),
		                kDispatchPolicySecondPluginId, 200, QStringLiteral(R"lua(
function OnPluginCommand(command)
  SetVariable("command_called", "yes")
  return false
end

function OnPluginTrace(message)
  SetVariable("trace_called", "yes")
end

function OnPluginBroadcast(message, calling_id, calling_name, text)
  SetVariable("broadcast_called", "yes")
end
)lua"));
		const bool thirdWritten =
		    writePlugin(QStringLiteral("dispatch_policy_third.xml"), QStringLiteral("DispatchPolicyThird"),
		                kDispatchPolicyThirdPluginId, 300, QStringLiteral(R"lua(
function OnPluginCommand(command)
  SetVariable("command_called", "yes")
  return true
end

function OnPluginTrace(message)
  SetVariable("trace_called", "yes")
end

function OnPluginBroadcast(message, calling_id, calling_name, text)
  SetVariable("broadcast_called", "yes")
end
)lua"));
		return firstWritten && secondWritten && thirdWritten;
	}

	/**
	 * @brief Writes a mutator, a later callback recipient, and a non-recipient recorder.
	 * @param pluginsDir Plugin fixture directory.
	 * @param mutation Lua lifecycle mutation performed by the first recipient.
	 * @param suspendAfterMutation Whether the first recipient pages output after recording the mutation.
	 * @return `true` when all plugin fixtures were written.
	 */
	bool writeRecipientEligibilityMutationPlugins(const QString &pluginsDir, const QString &mutation,
	                                              const bool suspendAfterMutation)
	{
		const auto writePlugin = [&](const QString &fileName, const QString &name, const QString &id,
		                             const int sequence, const QString &script)
		{
			return writeTextFile(
			    QDir(pluginsDir).filePath(fileName),
			    QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
			                   "<muclient>\n"
			                   "  <plugin name=\"%1\" author=\"QMud Test\" id=\"%2\" language=\"lua\" "
			                   "enabled=\"y\" save_state=\"n\" sequence=\"%3\">\n"
			                   "    <script><![CDATA[%4]]></script>\n"
			                   "  </plugin>\n"
			                   "</muclient>\n")
			        .arg(name, id, QString::number(sequence), script));
		};

		const QString suspend = suspendAfterMutation ? QStringLiteral("  GetLineInfo(1, 1)\n") : QString();
		const bool    mutatorWritten =
		    writePlugin(QStringLiteral("eligibility_mutator.xml"), QStringLiteral("EligibilityMutator"),
		                kEligibilityMutatorPluginId, 100,
		                QStringLiteral("\nfunction OnPluginConnect()\n  %1\n%2"
		                               "  SetVariable(\"mutator_completed\", \"yes\")\nend\n")
		                    .arg(mutation, suspend));
		const bool victimWritten =
		    writePlugin(QStringLiteral("eligibility_victim.xml"), QStringLiteral("EligibilityVictim"),
		                kEligibilityVictimPluginId, 200,
		                QStringLiteral(R"lua(
function OnPluginConnect()
  CallPlugin("%1", "record_victim_delivery", "yes")
end
)lua")
		                    .arg(kEligibilityRecorderPluginId));
		const bool recorderWritten =
		    writePlugin(QStringLiteral("eligibility_recorder.xml"), QStringLiteral("EligibilityRecorder"),
		                kEligibilityRecorderPluginId, 300, QStringLiteral(R"lua(
function record_victim_delivery(value)
  SetVariable("victim_delivered", value)
end
)lua"));
		return mutatorWritten && victimWritten && recorderWritten;
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
		static void pluginIdsAreCanonicalizedAtXmlAndApiBoundaries()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString uppercaseId = QStringLiteral("ABCDEFABCDEFABCDEFABCDEF");
			const QString lowercaseId = uppercaseId.toLower();
			const QString pluginPath  = QDir(tempDir.path()).filePath(QStringLiteral("mixed_case_id.xml"));
			QVERIFY(writeTextFile(pluginPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin name="MixedCaseId" id="%1" language="lua" enabled="y" save_state="n">
    <script><![CDATA[
function mixed_case_probe(value)
  SetVariable("api_boundary_probe", table.concat({
    tostring(IsPluginInstalled(value)),
    tostring(GetPluginInfo(value, 7))
  }, "|"))
  return GetPluginID()
end
]]></script>
  </plugin>
</muclient>
)xml")
			                                      .arg(uppercaseId)));

			WorldDocument document;
			QVERIFY2(document.loadFromPluginFile(pluginPath), qPrintable(document.errorString()));
			QCOMPARE(document.plugins().constFirst().attributes.value(QStringLiteral("id")), lowercaseId);

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(tempDir.path());
			QString error;
			QVERIFY2(runtime.loadPluginFile(pluginPath, &error), qPrintable(error));
			QVERIFY(!runtime.pluginForId(uppercaseId));
			WorldRuntime::Plugin *plugin = WorldRuntimeTestAccess::plugin(runtime, lowercaseId);
			QVERIFY(plugin);
			runtime.setPluginInstallPending(*plugin, false);
			QCOMPARE(plugin->attributes.value(QStringLiteral("id")), lowercaseId);
			QVERIFY(plugin->lua);
			QCOMPARE(plugin->lua->pluginIdMetadata(), lowercaseId);
			QCOMPARE(runtime.callPlugin(lowercaseId, QStringLiteral("mixed_case_probe"), uppercaseId), eOK);
			QCOMPARE(runtime.pluginVariableValue(lowercaseId, QStringLiteral("api_boundary_probe")),
			         QStringLiteral("true|%1").arg(lowercaseId));
		}

		static void reloadMccpFatalInflatePreservesSocketAndRecordsFailure()
		{
			QTcpServer server;
			if (!server.listen(QHostAddress::LocalHost, 0))
				QSKIP("Local TCP listen is unavailable in this environment.");

			WorldRuntime runtime;
			QSignalSpy   connectedSpy(&runtime, &WorldRuntime::connected);
			QSignalSpy   disconnectedSpy(&runtime, &WorldRuntime::disconnected);
			QSignalSpy   serverAcceptedSpy(&server, &QTcpServer::newConnection);
			QVERIFY(connectedSpy.isValid());
			QVERIFY(disconnectedSpy.isValid());
			QVERIFY(serverAcceptedSpy.isValid());

			QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
			QVERIFY(connectedSpy.wait(5000));
			QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() || serverAcceptedSpy.count() > 0, 5000);
			QScopedPointer<QTcpSocket> acceptedSocket(server.nextPendingConnection());
			QVERIFY(!acceptedSocket.isNull());

			activateMccp2(runtime);
			QVERIFY(runtime.isCompressing());
			runtime.queueMccpDisableForReload();
			QCOMPARE(runtime.reloadMccpDisableStatus(), WorldRuntime::ReloadMccpDisableStatus::Pending);

			runtime.receiveRawData(QByteArrayLiteral("not-a-zlib-stream"));

			QCOMPARE(runtime.reloadMccpDisableStatus(), WorldRuntime::ReloadMccpDisableStatus::Failed);
			QVERIFY(runtime.isMccpDisableCompleteForReload());
			QVERIFY(runtime.isConnected());
			QCOMPARE(disconnectedSpy.count(), 0);

			runtime.clearReloadMccpDisableForReload();
			QCOMPARE(runtime.reloadMccpDisableStatus(), WorldRuntime::ReloadMccpDisableStatus::Inactive);
			activateMccp2(runtime);
			QVERIFY(runtime.isCompressing());
			runtime.receiveRawData(QByteArrayLiteral("not-a-zlib-stream"));

			QTRY_COMPARE_WITH_TIMEOUT(disconnectedSpy.count(), 1, 5000);
			QVERIFY(!runtime.isConnected());
		}

		static void fatalMccpInflateOutsideReloadStillDisconnects()
		{
			QTcpServer server;
			if (!server.listen(QHostAddress::LocalHost, 0))
				QSKIP("Local TCP listen is unavailable in this environment.");

			WorldRuntime runtime;
			QSignalSpy   connectedSpy(&runtime, &WorldRuntime::connected);
			QSignalSpy   disconnectedSpy(&runtime, &WorldRuntime::disconnected);
			QSignalSpy   serverAcceptedSpy(&server, &QTcpServer::newConnection);
			QVERIFY(connectedSpy.isValid());
			QVERIFY(disconnectedSpy.isValid());
			QVERIFY(serverAcceptedSpy.isValid());

			QVERIFY(runtime.connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
			QVERIFY(connectedSpy.wait(5000));
			QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() || serverAcceptedSpy.count() > 0, 5000);
			QScopedPointer<QTcpSocket> acceptedSocket(server.nextPendingConnection());
			QVERIFY(!acceptedSocket.isNull());

			activateMccp2(runtime);
			QVERIFY(runtime.isCompressing());
			runtime.receiveRawData(QByteArrayLiteral("not-a-zlib-stream"));

			QTRY_COMPARE_WITH_TIMEOUT(disconnectedSpy.count(), 1, 5000);
			QVERIFY(!runtime.isConnected());
			QCOMPARE(runtime.reloadMccpDisableStatus(), WorldRuntime::ReloadMccpDisableStatus::Inactive);
		}

		static void reloadMccpCleanEndRecordsSuccess()
		{
			WorldRuntime runtime;
			activateMccp2(runtime);
			QVERIFY(runtime.isCompressing());

			runtime.queueMccpDisableForReload();
			QCOMPARE(runtime.reloadMccpDisableStatus(), WorldRuntime::ReloadMccpDisableStatus::Pending);
			runtime.receiveRawData(completeMccpPayload(QByteArrayLiteral("clean shutdown\r\n")));

			QCOMPARE(runtime.reloadMccpDisableStatus(), WorldRuntime::ReloadMccpDisableStatus::Succeeded);
			QVERIFY(runtime.isMccpDisableCompleteForReload());
		}

		static void reloadMccpNoResponseRemainsPendingUntilPreparationClears()
		{
			WorldRuntime runtime;
			activateMccp2(runtime);
			QVERIFY(runtime.isCompressing());

			runtime.queueMccpDisableForReload();
			QCOMPARE(runtime.reloadMccpDisableStatus(), WorldRuntime::ReloadMccpDisableStatus::Pending);
			QVERIFY(!runtime.isMccpDisableCompleteForReload());

			runtime.clearReloadMccpDisableForReload();
			QCOMPARE(runtime.reloadMccpDisableStatus(), WorldRuntime::ReloadMccpDisableStatus::Inactive);
			QVERIFY(!runtime.isMccpDisableCompleteForReload());
		}

		static void reloadControllerReturnClearsMccpPreparationState()
		{
			const QString artifactDirectory =
			    QDir(QCoreApplication::applicationDirPath())
			        .filePath(QStringLiteral("test-artifacts/tst_WorldRuntime_PluginLifecycle/"
			                                 "reload-controller-mccp-cleanup"));
			QVERIFY(QDir().mkpath(artifactDirectory));

			const QByteArray originalAssumeYes    = qgetenv("QMUD_RELOAD_ASSUME_YES");
			const QByteArray originalReloadDryRun = qgetenv("QMUD_RELOAD_DRY_RUN");
			const QString    originalCurrentPath  = QDir::currentPath();
			const auto       restoreEnvironment   = qScopeGuard(
			    [&]
			    {
				    const auto restoreVariable = [](const char *name, const QByteArray &value)
				    {
					    if (value.isNull())
						    qunsetenv(name);
					    else
						    qputenv(name, value);
				    };
				    restoreVariable("QMUD_RELOAD_ASSUME_YES", originalAssumeYes);
				    restoreVariable("QMUD_RELOAD_DRY_RUN", originalReloadDryRun);
				    (void)QDir::setCurrent(originalCurrentPath);
			    });
			QVERIFY(QDir::setCurrent(artifactDirectory));
			QVERIFY(qputenv("QMUD_RELOAD_ASSUME_YES", QByteArrayLiteral("1")));
			QVERIFY(qputenv("QMUD_RELOAD_DRY_RUN", QByteArrayLiteral("1")));

			AppController app;
			MainWindow    frame;
			app.setMainWindow(&frame);
			app.setGlobalOptionInt(QStringLiteral("EnableReloadFeature"), 1);
			app.setGlobalOptionInt(QStringLiteral("ReloadMccpDisableTimeoutMs"), 300);

			QTcpServer server;
			if (!server.listen(QHostAddress::LocalHost, 0))
				QSKIP("Local TCP listen is unavailable in this environment.");

			auto *runtime = new WorldRuntime(&frame);
			runtime->applyDefaultWorldOptions();
			runtime->setStartupDirectory(artifactDirectory);
			runtime->setReconnectOnLinkFailure(false);
			runtime->setWorldAttribute(QStringLiteral("id"),
			                           QStringLiteral("reload-controller-mccp-cleanup"));
			auto *primary = new WorldChildWindow(QStringLiteral("Reload MCCP cleanup"));
			primary->setRuntime(runtime);
			frame.addMdiSubWindow(primary, true);

			QSignalSpy connectedSpy(runtime, &WorldRuntime::connected);
			QSignalSpy serverAcceptedSpy(&server, &QTcpServer::newConnection);
			QVERIFY(connectedSpy.isValid());
			QVERIFY(serverAcceptedSpy.isValid());
			QVERIFY(runtime->connectToWorld(QStringLiteral("127.0.0.1"), server.serverPort()));
			QVERIFY(connectedSpy.wait(5000));
			QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() || serverAcceptedSpy.count() > 0, 5000);
			QScopedPointer<QTcpSocket> acceptedSocket(server.nextPendingConnection());
			QVERIFY(!acceptedSocket.isNull());

			activateMccp2(*runtime);
			QVERIFY(runtime->isCompressing());
			app.onCommandTriggered(QStringLiteral("ReloadQMud"));

			QCOMPARE(runtime->reloadMccpDisableStatus(), WorldRuntime::ReloadMccpDisableStatus::Inactive);
			QVERIFY(runtime->isCompressing());

			runtime->disconnectFromWorld();
			primary->setRuntime(nullptr);
		}

		static void reloadMccpRequestSucceedsWhenCompressionIsAlreadyInactive()
		{
			WorldRuntime runtime;
			QVERIFY(runtime.isMccpDisableCompleteForReload());
			QVERIFY(runtime.requestMccpDisableForReload(0));
			QCOMPARE(runtime.reloadMccpDisableStatus(), WorldRuntime::ReloadMccpDisableStatus::Inactive);
		}

		static void tabCompletionSymbolPreferencesRoundTripThroughWorldFile()
		{
			const QString artifactDirectory =
			    QDir(QCoreApplication::applicationDirPath())
			        .filePath(QStringLiteral("test-artifacts/tst_WorldRuntime_PluginLifecycle/"
			                                 "tab-completion-symbol-options-round-trip"));
			QVERIFY(QDir().mkpath(artifactDirectory));

			const QString prefixOptionKey = QStringLiteral("tab_completion_excludes_symbol_prefix");
			const QString suffixOptionKey = QStringLiteral("tab_completion_excludes_symbol_suffix");
			WorldRuntime  runtime;
			runtime.setStartupDirectory(artifactDirectory);
			runtime.applyDefaultWorldOptions();
			runtime.setWorldAttribute(prefixOptionKey, QStringLiteral("0"));
			runtime.setWorldAttribute(suffixOptionKey, QStringLiteral("0"));

			const QString disabledPath = QDir(artifactDirectory).filePath(QStringLiteral("disabled.qdl"));
			QString       saveError;
			QVERIFY2(runtime.saveWorldFile(disabledPath, &saveError), qPrintable(saveError));

			QString disabledXml;
			QVERIFY(readTextFile(disabledPath, disabledXml));
			QVERIFY(disabledXml.contains(QStringLiteral("tab_completion_excludes_symbol_prefix=\"n\"")));
			QVERIFY(disabledXml.contains(QStringLiteral("tab_completion_excludes_symbol_suffix=\"n\"")));

			WorldDocument disabledDocument;
			QVERIFY2(disabledDocument.loadFromFile(disabledPath), qPrintable(disabledDocument.errorString()));
			WorldRuntime disabledRuntime;
			disabledRuntime.setStartupDirectory(artifactDirectory);
			disabledRuntime.applyFromDocument(disabledDocument);
			QCOMPARE(disabledRuntime.worldAttributes().value(prefixOptionKey), QStringLiteral("n"));
			QCOMPARE(disabledRuntime.worldAttributes().value(suffixOptionKey), QStringLiteral("n"));

			disabledRuntime.setWorldAttribute(prefixOptionKey, QStringLiteral("1"));
			disabledRuntime.setWorldAttribute(suffixOptionKey, QStringLiteral("1"));
			const QString enabledPath = QDir(artifactDirectory).filePath(QStringLiteral("enabled.qdl"));
			QVERIFY2(disabledRuntime.saveWorldFile(enabledPath, &saveError), qPrintable(saveError));

			QString enabledXml;
			QVERIFY(readTextFile(enabledPath, enabledXml));
			QVERIFY(!enabledXml.contains(QStringLiteral("tab_completion_excludes_symbol_prefix=")));
			QVERIFY(!enabledXml.contains(QStringLiteral("tab_completion_excludes_symbol_suffix=")));

			WorldDocument enabledDocument;
			QVERIFY2(enabledDocument.loadFromFile(enabledPath), qPrintable(enabledDocument.errorString()));
			WorldRuntime enabledRuntime;
			enabledRuntime.setStartupDirectory(artifactDirectory);
			enabledRuntime.applyFromDocument(enabledDocument);
			QCOMPARE(enabledRuntime.worldAttributes().value(prefixOptionKey), QStringLiteral("y"));
			QCOMPARE(enabledRuntime.worldAttributes().value(suffixOptionKey), QStringLiteral("y"));
		}

		static void inputEchoWrapUsesCompleteReopenedLinePrefix()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("wrap"), QStringLiteral("1"));
			runtime.setWorldAttribute(QStringLiteral("auto_wrap_window_width"), QStringLiteral("0"));
			runtime.setWorldAttribute(QStringLiteral("wrap_column"), QStringLiteral("10"));
			runtime.addLine(QStringLiteral("123"), WorldRuntime::LineOutput, false);
			runtime.addLine(QStringLiteral("4 "), WorldRuntime::LineOutput, true);

			QString                          echo = QStringLiteral("abcdef");
			QVector<WorldRuntime::StyleSpan> spans;
			QVERIFY(runtime.prepareInputEchoForDisplay(echo, spans, true));

			QCOMPARE(echo, QStringLiteral("\nabcdef"));
			QVERIFY(spans.isEmpty());
			QVERIFY(!runtime.lines().constLast().hardReturn);

			WorldRuntime graphemeRuntime;
			graphemeRuntime.setWorldAttribute(QStringLiteral("wrap"), QStringLiteral("1"));
			graphemeRuntime.setWorldAttribute(QStringLiteral("auto_wrap_window_width"), QStringLiteral("0"));
			graphemeRuntime.setWorldAttribute(QStringLiteral("wrap_column"), QStringLiteral("11"));
			graphemeRuntime.addLine(QStringLiteral("123456789\U0001F469\u200D"), WorldRuntime::LineOutput,
			                        false);
			graphemeRuntime.addLine(QStringLiteral("\U0001F4BB"), WorldRuntime::LineOutput, true);

			QString graphemeEcho = QStringLiteral("x");
			QVERIFY(graphemeRuntime.prepareInputEchoForDisplay(graphemeEcho, spans, true));
			QCOMPARE(graphemeEcho, QStringLiteral("x"));

			WorldRuntime hiddenTailRuntime;
			hiddenTailRuntime.setWorldAttribute(QStringLiteral("wrap"), QStringLiteral("1"));
			hiddenTailRuntime.setWorldAttribute(QStringLiteral("auto_wrap_window_width"),
			                                    QStringLiteral("0"));
			hiddenTailRuntime.setWorldAttribute(QStringLiteral("wrap_column"), QStringLiteral("10"));
			hiddenTailRuntime.addLine(QStringLiteral("1234 "), WorldRuntime::LineOutput, true);
			hiddenTailRuntime.addLine(QStringLiteral("hidden"),
			                          WorldRuntime::LineOutput | WorldRuntime::LineHidden, true);

			QString hiddenTailEcho = QStringLiteral("abcdef");
			QVERIFY(hiddenTailRuntime.prepareInputEchoForDisplay(hiddenTailEcho, spans, true));
			QCOMPARE(hiddenTailEcho, QStringLiteral("\nabcdef"));
			QVERIFY(!hiddenTailRuntime.lines().at(0).hardReturn);
			QVERIFY(hiddenTailRuntime.lines().at(1).hardReturn);
		}

		static void mxpInversePreservesStoredColourOrderAcrossRenderPaths()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("use_mxp"), QStringLiteral("2"));
			runtime.setWorldAttribute(QStringLiteral("output_text_colour"), QStringLiteral("#ff0000"));
			runtime.setWorldAttribute(QStringLiteral("output_background_colour"), QStringLiteral("#0000ff"));

			QString                          line;
			QVector<WorldRuntime::StyleSpan> spans;
			QObject::connect(&runtime, &WorldRuntime::incomingStyledLineReceived, &runtime,
			                 [&line, &spans](const QString                          &receivedLine,
			                                 const QVector<WorldRuntime::StyleSpan> &receivedSpans)
			                 {
				                 line  = receivedLine;
				                 spans = receivedSpans;
			                 });
			runtime.receiveRawData(QByteArrayLiteral("\x1b[1z<font color=\"inverse\">X</font>\n"));
			QCOMPARE(line, QStringLiteral("X"));
			QCOMPARE(spans.size(), 1);
			QCOMPARE(spans.constFirst().fore, QColor(QStringLiteral("#ff0000")));
			QCOMPARE(spans.constFirst().back, QColor(QStringLiteral("#0000ff")));
			QVERIFY(spans.constFirst().inverse);

			QVERIFY(createWindowOutputTextTarget(runtime));
			WorldRuntime::WindowOutputMetrics metrics;
			const int width = runtime.windowOutputText(QStringLiteral("output"), QStringLiteral("font"),
			                                           QStringLiteral("\3font color=\"inverse\"\4X\3/font\4"),
			                                           0, 0, 319, 79, 0x00FFFFFF, QString(),
			                                           QStringLiteral("inverse"), QString(), &metrics);
			QVERIFY(width > 0);
			const MiniWindow *const window = runtime.miniWindow(QStringLiteral("output"));
			QVERIFY(window);
			QCOMPARE(window->backingSurface().pixelColor(0, 0), QColor(QStringLiteral("#ff0000")));
		}

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

		static void mxpResetPromptPartialIsRepublishedAfterIncomingLinesWithoutLineTerminator()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("use_mxp"), QStringLiteral("0"));
			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			QStringList                               partialLines;
			QVector<QVector<WorldRuntime::StyleSpan>> partialSpans;
			QObject::connect(&runtime, &WorldRuntime::incomingStyledLinePartialReceived, &runtime,
			                 [&partialLines, &partialSpans](const QString                          &line,
			                                                const QVector<WorldRuntime::StyleSpan> &spans)
			                 {
				                 partialLines.push_back(line);
				                 partialSpans.push_back(spans);
			                 });

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
			QCOMPARE(partialSpans.size(), partialLines.size());
			const QVector<WorldRuntime::StyleSpan> expectedPromptSpans = partialSpans.constLast();
			QVERIFY(!expectedPromptSpans.isEmpty());
			auto outputLastLine = [&harness]
			{
				const QStringList lines = harness.view.outputLines();
				return lines.isEmpty() ? QString() : lines.constLast();
			};
			QTRY_COMPARE(outputLastLine(), expectedPrompt);

			const qsizetype partialPublicationCount = partialLines.size();
			QByteArray      unsolicitedPayload      = QByteArrayLiteral("\r\n");
			unsolicitedPayload.append(payload);
			runtime.receiveRawData(unsolicitedPayload);

			QTRY_COMPARE(partialLines.size(), partialPublicationCount + 1);
			QCOMPARE(partialLines.constLast(), expectedPrompt);
			QCOMPARE(partialSpans.size(), partialLines.size());
			QVERIFY(partialSpans.constLast() == expectedPromptSpans);
			QTRY_COMPARE(outputLastLine(), expectedPrompt);

			runtime.receiveRawData(QByteArrayLiteral("\r\n"));
			QTRY_COMPARE(partialLines.constLast(), QString());
			const qsizetype completedOutputPublicationCount = partialLines.size();

			runtime.receiveRawData(QByteArrayLiteral("\x1B[3z<Ex>N</Ex>\r\n"));

			QCOMPARE(partialLines.size(), completedOutputPublicationCount);
			QTRY_COMPARE(outputLastLine(), QStringLiteral("N"));
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
			runtime.setWorldAttribute(QStringLiteral("use_mxp"), QStringLiteral("2"));
			QVERIFY(createWindowOutputTextTarget(runtime));
			auto engine = QSharedPointer<LuaCallbackEngine>::create();
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

			const LuaBatchDispatchResult callbackResult =
			    runtime.dispatchLuaStringsAndWildcards(engine, QStringLiteral("OnPluginEnable"), {});
			QVERIFY(callbackResult.hasFunctionValid);
			QVERIFY(callbackResult.hasFunction);

			LuaExecutorDirect       executor;
			LuaBatchDispatchRequest request;
			request.engines                     = {engine};
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

		static void asyncCallbackPagesOlderOutputThroughProductionResumePath()
		{
			WorldRuntime runtime;
			runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
			runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
			for (int lineNumber = 1; lineNumber <= 400; ++lineNumber)
				runtime.addLine(QStringLiteral("line %1").arg(lineNumber), WorldRuntime::LineOutput);

			QSharedPointer<LuaCallbackEngine> engine(runtime.luaCallbacks(),
			                                         [](LuaCallbackEngine * /*unused*/) {});
			QVERIFY(engine);

			bool executeCompleted = false;
			bool executeOk        = false;
			runtime.dispatchLuaExecuteScriptAsync(engine, QStringLiteral(R"lua(
function qcb_read_older_output(name, line, wildcards)
  SetVariable("paged_older_output", GetLineInfo(20, 1) or "<nil>")
end
)lua"),
			                                      QStringLiteral("paged callback line lookup"), nullptr,
			                                      false, false, 0, 0,
			                                      [&executeCompleted, &executeOk](const bool ok)
			                                      {
				                                      executeOk        = ok;
				                                      executeCompleted = true;
			                                      });

			QTRY_VERIFY_WITH_TIMEOUT(executeCompleted, 5000);
			QVERIFY(executeOk);

			bool                   callbackCompleted = false;
			LuaBatchDispatchResult callbackResult;
			runtime.dispatchLuaStringsAndWildcardsAsync(
			    engine, QStringLiteral("qcb_read_older_output"),
			    {QStringLiteral("older_output"), QStringLiteral("ignored")}, {}, {}, nullptr, -1, false, 0, 0,
			    [&callbackCompleted, &callbackResult](const LuaBatchDispatchResult &result)
			    {
				    callbackResult    = result;
				    callbackCompleted = true;
			    });

			QTRY_VERIFY_WITH_TIMEOUT(callbackCompleted, 5000);
			QVERIFY(callbackResult.hasFunctionValid);
			QVERIFY(callbackResult.hasFunction);
			QVERIFY(!callbackResult.suspended);
			QCOMPARE(worldVariable(runtime, QStringLiteral("paged_older_output")), QStringLiteral("line 20"));
		}

		static void suspendedRecipientRefreshesSnapshotForFollowingPlugin()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("worlds/plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));
			QVERIFY(writeSuspendedRecipientSnapshotPlugins(pluginsDir));

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
			runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
			runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
			for (int lineNumber = 1; lineNumber <= 150; ++lineNumber)
				runtime.addLine(QStringLiteral("baseline output %1").arg(lineNumber),
				                WorldRuntime::LineOutput);

			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			QString loadError;
			QVERIFY2(runtime.loadPluginFile(QStringLiteral("snapshot_mutator.xml"), &loadError),
			         qPrintable(loadError));
			QVERIFY2(runtime.loadPluginFile(QStringLiteral("snapshot_observer.xml"), &loadError),
			         qPrintable(loadError));
			QTRY_COMPARE_WITH_TIMEOUT(runtime.plugins().size(), 2, 5000);
			QTRY_VERIFY_WITH_TIMEOUT(std::ranges::none_of(runtime.plugins(),
			                                              [](const WorldRuntime::Plugin &plugin)
			                                              { return plugin.installPending; }),
			                         5000);

			const int lineCountBefore = runtime.luaContextLinesInBufferCount();
			runtime.fireWorldConnectHandlers();

			QTRY_COMPARE_WITH_TIMEOUT(
			    pluginVariable(runtime, kSnapshotObserverPluginId, QStringLiteral("observed_count")),
			    QString::number(lineCountBefore + 1), 5000);
			QCOMPARE(pluginVariable(runtime, kSnapshotObserverPluginId, QStringLiteral("observed_last_line")),
			         QStringLiteral("fresh callback output"));
		}

		static void deferredMutationRefreshesSnapshotBeforeFollowingPlugin()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("worlds/plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));
			QVERIFY(writeSuspendedRecipientSnapshotPlugins(pluginsDir));

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
			runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
			runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
			for (int lineNumber = 1; lineNumber <= 20; ++lineNumber)
				runtime.addLine(QStringLiteral("baseline output %1").arg(lineNumber),
				                WorldRuntime::LineOutput);

			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			QString loadError;
			QVERIFY2(runtime.loadPluginFile(QStringLiteral("snapshot_mutator.xml"), &loadError),
			         qPrintable(loadError));
			QVERIFY2(runtime.loadPluginFile(QStringLiteral("snapshot_observer.xml"), &loadError),
			         qPrintable(loadError));
			QTRY_COMPARE_WITH_TIMEOUT(runtime.plugins().size(), 2, 5000);
			QTRY_VERIFY_WITH_TIMEOUT(std::ranges::none_of(runtime.plugins(),
			                                              [](const WorldRuntime::Plugin &plugin)
			                                              { return plugin.installPending; }),
			                         5000);

			const int lineCountBefore = runtime.luaContextLinesInBufferCount();
			runtime.fireWorldDisconnectHandlers();

			QTRY_COMPARE_WITH_TIMEOUT(pluginVariable(runtime, kSnapshotObserverPluginId,
			                                         QStringLiteral("observed_unsuspended_count")),
			                          QString::number(lineCountBefore + 1), 5000);
			QCOMPARE(pluginVariable(runtime, kSnapshotObserverPluginId,
			                        QStringLiteral("observed_unsuspended_last_line")),
			         QStringLiteral("fresh unsuspended callback output"));
		}

		static void suspendedLifecycleMutationPrunesDisabledRemainingRecipient()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("worlds/plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));
			QVERIFY(writeRecipientEligibilityMutationPlugins(
			    pluginsDir, QStringLiteral("EnablePlugin(\"%1\", false)").arg(kEligibilityVictimPluginId),
			    true));

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
			for (int lineNumber = 1; lineNumber <= 150; ++lineNumber)
				runtime.addLine(QStringLiteral("eligibility baseline %1").arg(lineNumber),
				                WorldRuntime::LineOutput);
			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			QString loadError;
			for (const QString &fileName :
			     {QStringLiteral("eligibility_mutator.xml"), QStringLiteral("eligibility_victim.xml"),
			      QStringLiteral("eligibility_recorder.xml")})
			{
				QVERIFY2(runtime.loadPluginFile(fileName, &loadError), qPrintable(loadError));
			}
			QTRY_VERIFY_WITH_TIMEOUT(std::ranges::none_of(runtime.plugins(),
			                                              [](const WorldRuntime::Plugin &plugin)
			                                              { return plugin.installPending; }),
			                         5000);

			runtime.fireWorldConnectHandlers();

			QTRY_COMPARE_WITH_TIMEOUT(
			    pluginVariable(runtime, kEligibilityMutatorPluginId, QStringLiteral("mutator_completed")),
			    QStringLiteral("yes"), 5000);
			const WorldRuntime::Plugin *victim = runtime.pluginForId(kEligibilityVictimPluginId);
			QVERIFY(victim);
			QVERIFY(!victim->enabled);
			QVERIFY(pluginVariable(runtime, kEligibilityRecorderPluginId, QStringLiteral("victim_delivered"))
			            .isEmpty());
		}

		static void lifecycleMutationPrunesUnloadedRemainingRecipient()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("worlds/plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));
			QVERIFY(writeRecipientEligibilityMutationPlugins(
			    pluginsDir, QStringLiteral("UnloadPlugin(\"%1\")").arg(kEligibilityVictimPluginId), false));

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			QString loadError;
			for (const QString &fileName :
			     {QStringLiteral("eligibility_mutator.xml"), QStringLiteral("eligibility_victim.xml"),
			      QStringLiteral("eligibility_recorder.xml")})
			{
				QVERIFY2(runtime.loadPluginFile(fileName, &loadError), qPrintable(loadError));
			}
			QTRY_VERIFY_WITH_TIMEOUT(std::ranges::none_of(runtime.plugins(),
			                                              [](const WorldRuntime::Plugin &plugin)
			                                              { return plugin.installPending; }),
			                         5000);

			runtime.fireWorldConnectHandlers();

			QTRY_COMPARE_WITH_TIMEOUT(
			    pluginVariable(runtime, kEligibilityMutatorPluginId, QStringLiteral("mutator_completed")),
			    QStringLiteral("yes"), 5000);
			QVERIFY(!runtime.pluginForId(kEligibilityVictimPluginId));
			QVERIFY(pluginVariable(runtime, kEligibilityRecorderPluginId, QStringLiteral("victim_delivered"))
			            .isEmpty());
		}

		static void continuedDispatchPreservesRecipientStopPolicies()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("worlds/plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));
			QVERIFY(writeDispatchContinuationPolicyPlugins(pluginsDir));

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
			runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
			runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
			for (int lineNumber = 1; lineNumber <= 150; ++lineNumber)
				runtime.addLine(QStringLiteral("dispatch baseline %1").arg(lineNumber),
				                WorldRuntime::LineOutput);

			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			QString loadError;
			for (const QString &fileName :
			     {QStringLiteral("dispatch_policy_first.xml"), QStringLiteral("dispatch_policy_second.xml"),
			      QStringLiteral("dispatch_policy_third.xml")})
			{
				QVERIFY2(runtime.loadPluginFile(fileName, &loadError), qPrintable(loadError));
			}
			QTRY_COMPARE_WITH_TIMEOUT(runtime.plugins().size(), 3, 5000);
			QTRY_VERIFY_WITH_TIMEOUT(std::ranges::none_of(runtime.plugins(),
			                                              [](const WorldRuntime::Plugin &plugin)
			                                              { return plugin.installPending; }),
			                         5000);

			QVERIFY(!runtime.firePluginCommand(QStringLiteral("blocked command")));
			QCOMPARE(pluginVariable(runtime, kDispatchPolicySecondPluginId, QStringLiteral("command_called")),
			         QStringLiteral("yes"));
			QVERIFY(pluginVariable(runtime, kDispatchPolicyThirdPluginId, QStringLiteral("command_called"))
			            .isEmpty());

			QVERIFY(runtime.firePluginTrace(QStringLiteral("handled trace")));
			QCOMPARE(pluginVariable(runtime, kDispatchPolicyFirstPluginId, QStringLiteral("trace_resumed")),
			         QStringLiteral("yes"));
			QVERIFY(pluginVariable(runtime, kDispatchPolicySecondPluginId, QStringLiteral("trace_called"))
			            .isEmpty());
			QVERIFY(pluginVariable(runtime, kDispatchPolicyThirdPluginId, QStringLiteral("trace_called"))
			            .isEmpty());

			const quint64 nextResumeIdBeforeBroadcast = runtime.m_nextSuspendedPluginCallbackResumeId;
			QCOMPARE(runtime.broadcastPlugin(7, QStringLiteral("payload"), QStringLiteral("sender-id"),
			                                 QStringLiteral("Sender")),
			         3);
			QVERIFY(runtime.m_nextSuspendedPluginCallbackResumeId != nextResumeIdBeforeBroadcast);
			QCOMPARE(
			    pluginVariable(runtime, kDispatchPolicyFirstPluginId, QStringLiteral("broadcast_resumed")),
			    QStringLiteral("yes"));
			QCOMPARE(
			    pluginVariable(runtime, kDispatchPolicySecondPluginId, QStringLiteral("broadcast_called")),
			    QStringLiteral("yes"));
			QCOMPARE(
			    pluginVariable(runtime, kDispatchPolicyThirdPluginId, QStringLiteral("broadcast_called")),
			    QStringLiteral("yes"));
		}

		static void suspendedDispatchTeardownPreservesUnrelatedCurrentRecipient()
		{
			WorldRuntime      runtime;
			auto              currentRecipient = QSharedPointer<LuaCallbackEngine>::create();
			auto              removedRecipient = QSharedPointer<LuaCallbackEngine>::create();
			auto              laterRecipient   = QSharedPointer<LuaCallbackEngine>::create();

			constexpr quint64 kResumeId = 41;
			WorldRuntime::SuspendedPluginCallbackDispatch suspended;
			suspended.command.request.engines = {currentRecipient, removedRecipient, laterRecipient};
			suspended.nextEngineIndex         = 1;
			suspended.engineModalResumeId     = 73;
			runtime.m_suspendedPluginCallbackDispatches.insert(kResumeId, std::move(suspended));

			runtime.cancelSuspendedPluginCallbackDispatchesForEngines({removedRecipient});

			QVERIFY(runtime.m_suspendedPluginCallbackDispatches.contains(kResumeId));
			const auto &stored = runtime.m_suspendedPluginCallbackDispatches.constFind(kResumeId).value();
			QCOMPARE(stored.nextEngineIndex, 1);
			QCOMPARE(stored.command.request.engines.size(), 3);
			QVERIFY(stored.command.request.engines.at(0) == currentRecipient);
			QVERIFY(stored.command.request.engines.at(1).isNull());
			QVERIFY(stored.command.request.engines.at(2) == laterRecipient);

			runtime.m_suspendedPluginCallbackDispatches.clear();
		}

		static void currentSuspendedRecipientTeardownCancelsCoroutineAndFinishesFallback()
		{
			WorldRuntime runtime;
			runtime.m_luaExecutor = std::make_unique<LuaExecutorDirect>();
			for (int lineNumber = 1; lineNumber <= 200; ++lineNumber)
				runtime.addLine(QStringLiteral("cancellation baseline %1").arg(lineNumber),
				                WorldRuntime::LineOutput);

			const QString                           pluginId = QStringLiteral("cancel-suspended-plugin");
			const QSharedPointer<LuaCallbackEngine> engine   = QSharedPointer<LuaCallbackEngine>::create();
			engine->setWorldRuntime(&runtime);
			engine->setPluginInfo(pluginId, QStringLiteral("Cancellation plugin"), QString());
			engine->setScriptText(QStringLiteral(R"lua(
function qcb_cancel_suspended_recipient()
  Note("cancellation output")
  utils.inputbox("prompt", "title", "")
  SetVariable("resumed_after_cancel", "yes")
  return false
end
)lua"));
			WorldRuntime::Plugin plugin;
			plugin.attributes.insert(QStringLiteral("id"), pluginId);
			plugin.attributes.insert(QStringLiteral("name"), QStringLiteral("Cancellation plugin"));
			plugin.attributes.insert(QStringLiteral("language"), QStringLiteral("Lua"));
			plugin.lua = engine;
			WorldRuntimeTestAccess::plugins(runtime).push_back(std::move(plugin));

			LuaBatchDispatchRequest callbackRequest;
			callbackRequest.kind                    = LuaBatchDispatchKind::NoArgs;
			callbackRequest.engines                 = {engine};
			callbackRequest.functionName            = QStringLiteral("qcb_cancel_suspended_recipient");
			callbackRequest.defaultResult           = true;
			bool                   completionCalled = false;
			LuaBatchDispatchResult completionResult;
			runtime.queuePluginCallbackDispatchAsync(
			    callbackRequest,
			    [&completionCalled, &completionResult](const LuaBatchDispatchResult &result)
			    {
				    completionCalled = true;
				    completionResult = result;
			    });
			runtime.drainPluginCallbackDispatchQueue();

			QCOMPARE(runtime.m_suspendedPluginCallbackDispatches.size(), 1);
			const auto    suspendedIt     = runtime.m_suspendedPluginCallbackDispatches.constBegin();
			const quint64 runtimeResumeId = suspendedIt.key();
			const quint64 engineResumeId  = suspendedIt->engineModalResumeId;
			QVERIFY(runtimeResumeId != 0);
			QVERIFY(engineResumeId != 0);
			QVERIFY(runtime.luaCallbackOutputCursorCount() > 0);
			QVERIFY(!completionCalled);

			runtime.cancelSuspendedPluginCallbackDispatchesForEngines({engine});

			QVERIFY(!runtime.m_suspendedPluginCallbackDispatches.contains(runtimeResumeId));
			QCOMPARE(runtime.luaCallbackOutputCursorCount(), qsizetype{0});
			QVERIFY(completionCalled);
			QVERIFY(completionResult.boolResultValid);
			QVERIFY(completionResult.boolResult);
			QVERIFY(completionResult.hasFunctionValid);
			QVERIFY(!completionResult.hasFunction);
			QVERIFY(worldVariable(runtime, QStringLiteral("resumed_after_cancel")).isEmpty());
			QCoreApplication::processEvents();
			QVERIFY(worldVariable(runtime, QStringLiteral("resumed_after_cancel")).isEmpty());

			LuaBatchDispatchRequest staleResumeRequest;
			staleResumeRequest.kind                  = LuaBatchDispatchKind::ResumeSuspendedModalString;
			staleResumeRequest.engines               = {engine};
			staleResumeRequest.modalResumeId         = engineResumeId;
			const LuaBatchDispatchResult staleResume = runtime.dispatchLuaBatch(staleResumeRequest);
			QVERIFY(!staleResume.suspended);
			QVERIFY(!staleResume.boolResultValid);
			QVERIFY(!staleResume.hasFunctionValid);
		}

		static void directSuspendedContinuationPublishesEveryMutationBoundary()
		{
			WorldRuntime runtime;
			runtime.m_luaExecutor = std::make_unique<LuaExecutorDirect>();

			const QString firstId  = QStringLiteral("111111111111111111111111");
			const QString secondId = QStringLiteral("222222222222222222222222");
			const QString thirdId  = QStringLiteral("333333333333333333333333");
			const QString callback = QStringLiteral("qcb_direct_suspended_boundary");

			const auto addPlugin = [&runtime](const QString &id, const QString &name, const QString &script)
			{
				auto engine = QSharedPointer<LuaCallbackEngine>::create();
				engine->setWorldRuntime(&runtime);
				engine->setPluginInfo(id, name, QString());
				engine->setScriptText(script);
				WorldRuntime::Plugin plugin;
				plugin.attributes.insert(QStringLiteral("id"), id);
				plugin.attributes.insert(QStringLiteral("name"), name);
				plugin.attributes.insert(QStringLiteral("language"), QStringLiteral("Lua"));
				plugin.attributes.insert(QStringLiteral("enabled"), QStringLiteral("y"));
				plugin.lua = engine;
				WorldRuntimeTestAccess::plugins(runtime).push_back(std::move(plugin));
				return engine;
			};

			const auto first  = addPlugin(firstId, QStringLiteral("First"), QStringLiteral(R"lua(
function qcb_direct_suspended_boundary(value)
  utils.inputbox("prompt", "title", "")
  SetVariable("shared", "after-resume")
  return "first-result"
end
)lua"));
			const auto second = addPlugin(secondId, QStringLiteral("Second"),
			                              QStringLiteral(R"lua(
function qcb_direct_suspended_boundary(value)
  local observed = GetPluginVariable("%1", "shared") or "<missing>"
  SetVariable("shared", observed .. ":second")
  return value .. ":" .. observed
end
)lua")
			                                  .arg(firstId));
			const auto third  = addPlugin(thirdId, QStringLiteral("Third"),
			                              QStringLiteral(R"lua(
function qcb_direct_suspended_boundary(value)
  return value .. ":" .. (GetPluginVariable("%1", "shared") or "<missing>")
end
)lua")
			                                  .arg(secondId));

			LuaBatchDispatchRequest originalRequest;
			originalRequest.kind         = LuaBatchDispatchKind::StringInOut;
			originalRequest.engines      = {first, second, third};
			originalRequest.functionName = callback;
			originalRequest.stringArg    = QStringLiteral("initial");
			originalRequest.callbackSnapshotArg =
			    runtime.captureLuaCallbackSnapshotForRequest(originalRequest);
			QVERIFY(originalRequest.callbackSnapshotArg);

			LuaBatchDispatchRequest firstRequest = originalRequest;
			firstRequest.engines                 = {first};
			LuaBatchDispatchResult initialResult = runtime.m_luaExecutor->dispatchBatch(firstRequest);
			QVERIFY(initialResult.suspended);
			QVERIFY(initialResult.modalResumeId != 0);
			QVERIFY(initialResult.deferredRuntimeMutationBatches.isEmpty());

			constexpr quint64                             runtimeResumeId = 901;
			constexpr quint64                             commandId       = 902;
			WorldRuntime::SuspendedPluginCallbackDispatch suspended;
			suspended.command.id           = commandId;
			suspended.command.retainResult = true;
			suspended.command.request      = originalRequest;
			suspended.partialResult        = initialResult;
			suspended.pluginId             = firstId;
			suspended.engineModalResumeId  = initialResult.modalResumeId;
			suspended.nextEngineIndex      = 1;
			runtime.m_suspendedPluginCallbackDispatches.insert(runtimeResumeId, std::move(suspended));

			LuaBatchDispatchRequest resumeRequest;
			resumeRequest.kind                  = LuaBatchDispatchKind::ResumeSuspendedModalString;
			resumeRequest.engines               = {first};
			resumeRequest.modalResumeId         = initialResult.modalResumeId;
			resumeRequest.runtimeModalResumeId  = runtimeResumeId;
			LuaBatchDispatchResult resumeResult = runtime.m_luaExecutor->dispatchBatch(resumeRequest);
			QVERIFY(!resumeResult.suspended);
			QVERIFY(resumeResult.deferredRuntimeMutationBatches.isEmpty());
			QVERIFY(resumeResult.callbackSnapshotAfterMutations);

			WorldRuntime::PluginCallbackDispatchCommand resumeCommand;
			resumeCommand.request = resumeRequest;
			runtime.handleCompletedPluginCallbackDispatchCommand(std::move(resumeCommand),
			                                                     std::move(resumeResult));

			QVERIFY(!runtime.m_suspendedPluginCallbackDispatches.contains(runtimeResumeId));
			auto completed = runtime.m_pluginCallbackDispatchResults.find(commandId);
			QVERIFY(completed != runtime.m_pluginCallbackDispatchResults.end());
			QCOMPARE(completed->stringResult,
			         QStringLiteral("first-result:after-resume:after-resume:second"));
			QCOMPARE(pluginVariable(runtime, firstId, QStringLiteral("shared")),
			         QStringLiteral("after-resume"));
			QCOMPARE(pluginVariable(runtime, secondId, QStringLiteral("shared")),
			         QStringLiteral("after-resume:second"));
			runtime.m_pluginCallbackDispatchResults.erase(completed);
		}

		static void terminalModalResumeDoesNotRecaptureUnusedRequestSnapshot()
		{
			WorldRuntime runtime;
			runtime.m_luaExecutor = std::make_unique<LuaExecutorDirect>();

			const QString pluginId = QStringLiteral("515151515151515151515151");
			const QString callback = QStringLiteral("qcb_terminal_resume_capture");
			const auto engine = addDirectCallbackPlugin(runtime, pluginId, QStringLiteral("Terminal resume"),
			                                            QStringLiteral(R"lua(
function qcb_terminal_resume_capture(value)
  utils.inputbox("prompt", "title", "")
  SetVariable("after_resume", "committed")
  return value .. ":done"
end
)lua"));

			LuaBatchDispatchRequest originalRequest;
			originalRequest.kind         = LuaBatchDispatchKind::StringInOut;
			originalRequest.engines      = {engine};
			originalRequest.functionName = callback;
			originalRequest.stringArg    = QStringLiteral("initial");
			originalRequest.callbackSnapshotArg =
			    runtime.captureLuaCallbackSnapshotForRequest(originalRequest);

			LuaBatchDispatchResult initialResult = runtime.m_luaExecutor->dispatchBatch(originalRequest);
			QVERIFY(initialResult.suspended);
			QVERIFY(initialResult.modalResumeId != 0);

			constexpr quint64                             runtimeResumeId = 911;
			constexpr quint64                             commandId       = 912;
			WorldRuntime::SuspendedPluginCallbackDispatch suspended;
			suspended.command.id           = commandId;
			suspended.command.retainResult = true;
			suspended.command.request      = originalRequest;
			suspended.partialResult        = initialResult;
			suspended.pluginId             = pluginId;
			suspended.engineModalResumeId  = initialResult.modalResumeId;
			suspended.nextEngineIndex      = 1;
			runtime.m_suspendedPluginCallbackDispatches.insert(runtimeResumeId, std::move(suspended));

			LuaBatchDispatchRequest resumeRequest;
			resumeRequest.kind                  = LuaBatchDispatchKind::ResumeSuspendedModalString;
			resumeRequest.engines               = {engine};
			resumeRequest.modalResumeId         = initialResult.modalResumeId;
			resumeRequest.runtimeModalResumeId  = runtimeResumeId;
			resumeRequest.stringArg             = QStringLiteral("accepted");
			LuaBatchDispatchResult resumeResult = runtime.m_luaExecutor->dispatchBatch(resumeRequest);
			QVERIFY(!resumeResult.suspended);
			QVERIFY(resumeResult.callbackSnapshotAfterMutations);

			const quint64 capturesBefore = runtime.m_luaCallbackDispatchSnapshotCaptureCount;
			WorldRuntime::PluginCallbackDispatchCommand resumeCommand;
			resumeCommand.request = resumeRequest;
			runtime.handleCompletedPluginCallbackDispatchCommand(std::move(resumeCommand),
			                                                     std::move(resumeResult));

			QCOMPARE(runtime.m_luaCallbackDispatchSnapshotCaptureCount, capturesBefore);
			QCOMPARE(pluginVariable(runtime, pluginId, QStringLiteral("after_resume")),
			         QStringLiteral("committed"));
			auto completed = runtime.m_pluginCallbackDispatchResults.find(commandId);
			QVERIFY(completed != runtime.m_pluginCallbackDispatchResults.end());
			QCOMPARE(completed->stringResult, QStringLiteral("initial:done"));
			runtime.m_pluginCallbackDispatchResults.erase(completed);
		}

		static void stoppingContinuationDoesNotRecaptureOrDispatchLaterRecipient()
		{
			WorldRuntime runtime;
			runtime.m_luaExecutor = std::make_unique<LuaExecutorDirect>();

			const QString stoppingId = QStringLiteral("616161616161616161616161");
			const QString laterId    = QStringLiteral("717171717171717171717171");
			const QString callback   = QStringLiteral("qcb_stop_without_recapture");
			const auto    stopping   = addDirectCallbackPlugin(
			    runtime, stoppingId, QStringLiteral("Stopping recipient"), QStringLiteral(R"lua(
function qcb_stop_without_recapture(flags, value)
  SetVariable("stopped", "yes")
  return true
end
)lua"));
			const auto later = addDirectCallbackPlugin(runtime, laterId, QStringLiteral("Later recipient"),
			                                           QStringLiteral(R"lua(
function qcb_stop_without_recapture(flags, value)
  SetVariable("called", "yes")
  return false
end
)lua"));

			LuaBatchDispatchRequest originalRequest;
			originalRequest.kind         = LuaBatchDispatchKind::NumberAndStringStopOnTrue;
			originalRequest.engines      = {stopping, later};
			originalRequest.functionName = callback;
			originalRequest.numberArg1   = 1;
			originalRequest.stringArg2   = QStringLiteral("payload");
			originalRequest.callbackSnapshotArg =
			    runtime.captureLuaCallbackSnapshotForRequest(originalRequest);

			constexpr quint64                             commandId = 913;
			WorldRuntime::SuspendedPluginCallbackDispatch suspended;
			suspended.command.id           = commandId;
			suspended.command.retainResult = true;
			suspended.command.request      = originalRequest;
			suspended.partialResult        = makeLuaBatchDispatchFallback(originalRequest);
			suspended.nextEngineIndex      = 0;

			LuaBatchDispatchResult resumedRecipient;
			resumedRecipient.boolResult       = false;
			resumedRecipient.boolResultValid  = true;
			resumedRecipient.hasFunction      = true;
			resumedRecipient.hasFunctionValid = true;

			const quint64 capturesBefore = runtime.m_luaCallbackDispatchSnapshotCaptureCount;
			runtime.continueSuspendedPluginCallbackDispatch(std::move(suspended),
			                                                std::move(resumedRecipient));

			// The stopping callback publishes its cumulative mutation snapshot once. A second increment
			// would be the unused request recapture that used to occur before the stop result was checked.
			QCOMPARE(runtime.m_luaCallbackDispatchSnapshotCaptureCount, capturesBefore + 1);
			QCOMPARE(pluginVariable(runtime, stoppingId, QStringLiteral("stopped")), QStringLiteral("yes"));
			QVERIFY(pluginVariable(runtime, laterId, QStringLiteral("called")).isEmpty());
			auto completed = runtime.m_pluginCallbackDispatchResults.find(commandId);
			QVERIFY(completed != runtime.m_pluginCallbackDispatchResults.end());
			QVERIFY(completed->boolResultValid);
			QVERIFY(completed->boolResult);
			runtime.m_pluginCallbackDispatchResults.erase(completed);
		}

		static void directRecipientMutationBoundaryCapturesExactlyOnce()
		{
			WorldRuntime runtime;
			runtime.m_luaExecutor = std::make_unique<LuaExecutorDirect>();

			const QString firstId  = QStringLiteral("101010101010101010101010");
			const QString secondId = QStringLiteral("202020202020202020202020");
			const QString callback = QStringLiteral("qcb_direct_single_boundary_capture");
			const auto    first =
			    addDirectCallbackPlugin(runtime, firstId, QStringLiteral("First"), QStringLiteral(R"lua(
function qcb_direct_single_boundary_capture(value)
  SetVariable("shared", "published-once")
  return "first-result"
end
)lua"));
			const auto second = addDirectCallbackPlugin(runtime, secondId, QStringLiteral("Second"),
			                                            QStringLiteral(R"lua(
function qcb_direct_single_boundary_capture(value)
  return value .. ":" .. (GetPluginVariable("%1", "shared") or "<missing>")
end
)lua")
			                                                .arg(firstId));

			LuaBatchDispatchRequest request;
			request.kind         = LuaBatchDispatchKind::StringInOut;
			request.engines      = {first, second};
			request.functionName = callback;
			request.stringArg    = QStringLiteral("initial");

			const quint64                capturesBefore = runtime.m_luaCallbackDispatchSnapshotCaptureCount;
			const quint64                buildsBefore   = runtime.m_luaCallbackDispatchSnapshotBaseBuildCount;
			const quint64                patchesBefore  = runtime.m_luaCallbackDispatchSnapshotBasePatchCount;
			const LuaBatchDispatchResult result         = runtime.queuePluginCallbackDispatch(request, true);

			QCOMPARE(result.stringResult, QStringLiteral("first-result:published-once"));
			QCOMPARE(pluginVariable(runtime, firstId, QStringLiteral("shared")),
			         QStringLiteral("published-once"));
			// Initial request capture, the mutating callback's cumulative publication, and one
			// continuation capture. The old direct-path duplication produced a fourth capture here.
			QCOMPARE(runtime.m_luaCallbackDispatchSnapshotCaptureCount, capturesBefore + 3);
			QCOMPARE(runtime.m_luaCallbackDispatchSnapshotBaseBuildCount, buildsBefore + 1);
			QVERIFY(runtime.m_luaCallbackDispatchSnapshotBasePatchCount > patchesBefore);
		}

		static void directRecipientMutationBoundaryCarriesBytesInOutValue()
		{
			WorldRuntime runtime;
			runtime.m_luaExecutor = std::make_unique<LuaExecutorDirect>();

			const QString firstId  = QStringLiteral("121212121212121212121212");
			const QString secondId = QStringLiteral("232323232323232323232323");
			const QString callback = QStringLiteral("qcb_direct_bytes_boundary");
			const auto    first =
			    addDirectCallbackPlugin(runtime, firstId, QStringLiteral("First bytes"), QStringLiteral(R"lua(
function qcb_direct_bytes_boundary(value)
  SetVariable("shared", "bytes-published")
  return value .. ":first"
end
)lua"));
			const auto second = addDirectCallbackPlugin(runtime, secondId, QStringLiteral("Second bytes"),
			                                            QStringLiteral(R"lua(
function qcb_direct_bytes_boundary(value)
  return value .. ":" .. (GetPluginVariable("%1", "shared") or "<missing>")
end
)lua")
			                                                .arg(firstId));

			LuaBatchDispatchRequest request;
			request.kind         = LuaBatchDispatchKind::BytesInOut;
			request.engines      = {first, second};
			request.functionName = callback;
			request.bytesArg     = QByteArrayLiteral("initial");

			const LuaBatchDispatchResult result = runtime.queuePluginCallbackDispatch(request, true);

			QCOMPARE(result.bytesResult, QByteArrayLiteral("initial:first:bytes-published"));
			QCOMPARE(pluginVariable(runtime, firstId, QStringLiteral("shared")),
			         QStringLiteral("bytes-published"));
		}

		static void directRepeatedYieldRetainsCommonContinuationState()
		{
			WorldRuntime runtime;
			runtime.m_luaExecutor = std::make_unique<LuaExecutorDirect>();
			for (int lineNumber = 1; lineNumber <= 500; ++lineNumber)
				runtime.addLine(QStringLiteral("yield line %1").arg(lineNumber), WorldRuntime::LineOutput);

			const QString firstId  = QStringLiteral("303030303030303030303030");
			const QString secondId = QStringLiteral("404040404040404040404040");
			const QString callback = QStringLiteral("qcb_direct_repeated_yield");
			const auto    first = addDirectCallbackPlugin(runtime, firstId, QStringLiteral("Yielding first"),
			                                              QStringLiteral(R"lua(
function qcb_direct_repeated_yield(value)
  SetVariable("phase", "before-first-yield")
  local first = GetLineInfo(1, 1) or "<missing-first>"
  SetVariable("phase", "between-yields")
  local second = GetLineInfo(300, 1) or "<missing-second>"
  SetVariable("phase", "after-second-yield")
  return first .. "|" .. second
end
)lua"));
			const auto second   = addDirectCallbackPlugin(runtime, secondId, QStringLiteral("Yield observer"),
			                                              QStringLiteral(R"lua(
function qcb_direct_repeated_yield(value)
  return value .. "|" .. (GetPluginVariable("%1", "phase") or "<missing-phase>")
end
)lua")
			                                                  .arg(firstId));

			LuaBatchDispatchRequest request;
			request.kind         = LuaBatchDispatchKind::StringInOut;
			request.engines      = {first, second};
			request.functionName = callback;
			request.stringArg    = QStringLiteral("initial");

			const quint64                nextResumeIdBefore = runtime.m_nextSuspendedPluginCallbackResumeId;
			const LuaBatchDispatchResult result = runtime.queuePluginCallbackDispatch(request, true);

			QVERIFY(!result.suspended);
			QCOMPARE(result.stringResult, QStringLiteral("yield line 1|yield line 300|after-second-yield"));
			QCOMPARE(pluginVariable(runtime, firstId, QStringLiteral("phase")),
			         QStringLiteral("after-second-yield"));
			QVERIFY(runtime.m_nextSuspendedPluginCallbackResumeId >= nextResumeIdBefore + 2);
			QVERIFY(runtime.m_suspendedPluginCallbackDispatches.isEmpty());
		}

		static void suspendedDispatchFallbackPreservesEveryPartialAggregateShape()
		{
			QVERIFY(luaBatchDispatchRequiresFunctionName(LuaBatchDispatchKind::NoArgs));
			QVERIFY(!luaBatchDispatchRequiresFunctionName(LuaBatchDispatchKind::ExecuteScript));
			QVERIFY(!luaBatchDispatchRequiresFunctionName(LuaBatchDispatchKind::CancelSuspendedModalString));
			QVERIFY(!luaBatchDispatchRequiresFunctionName(LuaBatchDispatchKind::ResumeSuspendedModalString));

			const auto fallbackFor = [](const LuaBatchDispatchKind kind)
			{
				LuaBatchDispatchRequest request;
				request.kind = kind;
				return makeLuaBatchDispatchFallback(request);
			};
			const LuaBatchDispatchResult aggregateNeutral = fallbackFor(LuaBatchDispatchKind::MxpStartUp);
			QVERIFY(!aggregateNeutral.boolResultValid);
			QVERIFY(!aggregateNeutral.hasFunctionValid);
			const LuaBatchDispatchResult hasFunctionFallback = fallbackFor(LuaBatchDispatchKind::HasFunction);
			QVERIFY(hasFunctionFallback.hasFunctionValid);
			QVERIFY(!hasFunctionFallback.hasFunction);
			const LuaBatchDispatchResult boolFallback = fallbackFor(LuaBatchDispatchKind::ExecuteScript);
			QVERIFY(boolFallback.boolResultValid);
			QVERIFY(!boolFallback.boolResult);
			const LuaBatchDispatchResult procedureFallback =
			    fallbackFor(LuaBatchDispatchKind::ProcedureWithString);
			QVERIFY(procedureFallback.boolResultValid);
			QVERIFY(!procedureFallback.boolResult);
			QVERIFY(procedureFallback.hasFunctionValid);
			QVERIFY(!procedureFallback.hasFunction);

			WorldRuntime runtime;
			quint64      nextCommandId = 1;
			quint64      nextResumeId  = 1;
			auto         finishFallback =
			    [&runtime, &nextCommandId,
			     &nextResumeId](const LuaBatchDispatchKind kind,
			                    LuaBatchDispatchResult     partial) -> std::pair<bool, LuaBatchDispatchResult>
			{
				const quint64                                 commandId = nextCommandId++;
				const quint64                                 resumeId  = nextResumeId++;
				WorldRuntime::SuspendedPluginCallbackDispatch suspended;
				suspended.command.id           = commandId;
				suspended.command.retainResult = true;
				suspended.command.request.kind = kind;
				suspended.partialResult        = std::move(partial);
				runtime.m_suspendedPluginCallbackDispatches.insert(resumeId, std::move(suspended));
				runtime.finishSuspendedPluginCallbackDispatchWithFallback(resumeId, false);
				auto resultIt = runtime.m_pluginCallbackDispatchResults.find(commandId);
				if (resultIt == runtime.m_pluginCallbackDispatchResults.end())
					return {};
				LuaBatchDispatchResult result = std::move(resultIt.value());
				runtime.m_pluginCallbackDispatchResults.erase(resultIt);
				return {true, std::move(result)};
			};

			LuaBatchDispatchResult partial;
			partial.boolResult                      = false;
			partial.hasFunction                     = true;
			partial.linePresentationRequiresRefresh = true;
			auto [noArgsFinished, noArgs]           = finishFallback(LuaBatchDispatchKind::NoArgs, partial);
			QVERIFY(noArgsFinished);
			QVERIFY(noArgs.boolResultValid);
			QVERIFY(!noArgs.boolResult);
			QVERIFY(noArgs.hasFunctionValid);
			QVERIFY(noArgs.hasFunction);
			QVERIFY(noArgs.linePresentationRequiresRefresh);

			for (const LuaBatchDispatchKind kind :
			     {LuaBatchDispatchKind::StringStopOnFalse, LuaBatchDispatchKind::NumberAndStringStopOnTrue,
			      LuaBatchDispatchKind::NumberAndStringStopOnFalse,
			      LuaBatchDispatchKind::TwoNumbersAndStringStopOnFalse,
			      LuaBatchDispatchKind::NumberAndBytesStopOnTrue})
			{
				partial                 = {};
				partial.boolResult      = luaBatchDispatchRecipientStopCondition(kind) ==
				                          LuaBatchRecipientStopCondition::FalseResult;
				partial.hasFunction     = true;
				auto [finished, result] = finishFallback(kind, partial);
				QVERIFY(finished);
				QVERIFY(result.boolResultValid);
				QCOMPARE(result.boolResult, partial.boolResult);
				QVERIFY(result.hasFunctionValid);
				QVERIFY(result.hasFunction);
			}

			partial                         = {};
			partial.boolResult              = true;
			partial.hasFunction             = true;
			auto [handledFinished, handled] = finishFallback(LuaBatchDispatchKind::StringHandled, partial);
			QVERIFY(handledFinished);
			QVERIFY(handled.boolResultValid);
			QVERIFY(handled.boolResult);
			QVERIFY(handled.hasFunctionValid);
			QVERIFY(handled.hasFunction);

			partial             = {};
			partial.countResult = 2;
			auto [countFinished, count] =
			    finishFallback(LuaBatchDispatchKind::NumberAndUtf8StringsCount, partial);
			QVERIFY(countFinished);
			QVERIFY(count.countResultValid);
			QCOMPARE(count.countResult, 2);

			partial             = {};
			partial.hasFunction = true;
			auto [wildcardsFinished, wildcards] =
			    finishFallback(LuaBatchDispatchKind::StringsAndWildcards, partial);
			QVERIFY(wildcardsFinished);
			QVERIFY(wildcards.hasFunctionValid);
			QVERIFY(wildcards.hasFunction);

			partial                     = {};
			partial.bytesResult         = QByteArray("transformed bytes");
			auto [bytesFinished, bytes] = finishFallback(LuaBatchDispatchKind::BytesInOut, partial);
			QVERIFY(bytesFinished);
			QCOMPARE(bytes.bytesResult, QByteArray("transformed bytes"));

			partial                       = {};
			partial.stringResult          = QStringLiteral("transformed string");
			auto [stringFinished, string] = finishFallback(LuaBatchDispatchKind::StringInOut, partial);
			QVERIFY(stringFinished);
			QCOMPARE(string.stringResult, QStringLiteral("transformed string"));
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
			WorldRuntimeTestAccess::triggers(runtime).push_back(
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
			WorldRuntimeTestAccess::triggers(runtime).push_back(
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
			WorldRuntimeTestAccess::aliases(runtime).push_back(
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
			WorldRuntimeTestAccess::timers(runtime).push_back(makeMultilineExecuteTimer());
			runtime.markTimersChanged();

			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			QString loadError;
			QVERIFY2(runtime.loadPluginFile(QStringLiteral("timer_command_recorder.xml"), &loadError),
			         qPrintable(loadError));
			QTRY_VERIFY_WITH_TIMEOUT(!runtime.plugins().constFirst().installPending, 5000);

			WorldRuntimeTestAccess::timers(runtime).first().nextFireTime =
			    QDateTime::currentDateTime().addMSecs(-1);

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
			WorldRuntimeTestAccess::triggers(runtime).push_back(makePromptNoteInjectionTrigger(prompt));
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
			WorldRuntimeTestAccess::triggers(runtime).push_back(makeLuaSendPriorityTrigger());
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
			WorldRuntimeTestAccess::triggers(runtime).push_back(makeLuaExecutePriorityTrigger());
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
			WorldRuntimeTestAccess::triggers(runtime).push_back(makeLuaMixedPriorityTrigger());
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
			WorldRuntimeTestAccess::triggers(runtime).push_back(makeNamedCallbackQueueNormalTrigger());
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
			WorldRuntimeTestAccess::triggers(runtime).push_back(makeLuaCallPluginPriorityTrigger());
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
			WorldRuntimeTestAccess::triggers(runtime).push_back(makeTelnetOrderingTrigger());
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
			WorldRuntimeTestAccess::triggers(runtime).push_back(makeTelnetOrderingTrigger());
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
			WorldRuntimeTestAccess::triggers(runtime).push_back(makeTelnetOrderingTrigger(kTelnetAfterLine));
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
			WorldRuntimeTestAccess::triggers(runtime).push_back(makeTelnetOrderingTrigger(kTelnetAfterLine));
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

		static void pluginEnableRevalidatesTargetAfterCallbackPluginRemoval()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));

			const QString victimId   = QStringLiteral("101010101010101010101010");
			const QString targetId   = QStringLiteral("202020202020202020202020");
			const QString sentinelId = QStringLiteral("303030303030303030303030");
			QVERIFY(writeSequencedPlugin(pluginsDir, QStringLiteral("enable_victim.xml"),
			                             QStringLiteral("EnableVictim"), victimId, 100));
			QVERIFY(writeTextFile(QDir(pluginsDir).filePath(QStringLiteral("enable_target.xml")),
			                      QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin name="EnableTarget" id="%1" language="lua" enabled="n" save_state="n" sequence="200">
    <script><![CDATA[
function OnPluginEnable()
  UnloadPlugin("%2")
end

function OnPluginInstall()
  SetVariable("install_marker", "installed")
end
]]></script>
  </plugin>
</muclient>
)xml")
			                          .arg(targetId, victimId)));
			QVERIFY(writeSequencedPlugin(pluginsDir, QStringLiteral("enable_sentinel.xml"),
			                             QStringLiteral("EnableSentinel"), sentinelId, 300));

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(pluginsDir);
			QString error;
			for (const QString &fileName :
			     {QStringLiteral("enable_victim.xml"), QStringLiteral("enable_target.xml"),
			      QStringLiteral("enable_sentinel.xml")})
			{
				QVERIFY2(runtime.loadPluginFile(QDir(pluginsDir).filePath(fileName), &error),
				         qPrintable(error));
			}

			for (WorldRuntime::Plugin &plugin : WorldRuntimeTestAccess::plugins(runtime))
				plugin.installPending = false;
			WorldRuntime::Plugin *target = WorldRuntimeTestAccess::plugin(runtime, targetId);
			QVERIFY(target);
			target->enabled             = false;
			target->disableAfterInstall = false;
			target->installPending      = true;
			target->attributes.insert(QStringLiteral("enabled"), QStringLiteral("0"));

			WorldView view;
			view.resize(640, 480);
			view.setRuntime(&runtime);

			QVERIFY(runtime.enablePlugin(targetId, true));
			QTRY_COMPARE_WITH_TIMEOUT(pluginVariable(runtime, targetId, QStringLiteral("install_marker")),
			                          QStringLiteral("installed"), 5000);
			QVERIFY(!runtime.isPluginInstalled(victimId));
			QVERIFY(runtime.isPluginInstalled(targetId));
			QVERIFY(runtime.isPluginInstalled(sentinelId));
			target = WorldRuntimeTestAccess::plugin(runtime, targetId);
			QVERIFY(target);
			QVERIFY(!target->installPending);
		}

		static void pluginUnloadRevalidatesTargetAfterCloseCallbackPluginRemoval()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));

			const QString victimId   = QStringLiteral("111111111111111111111111");
			const QString targetId   = QStringLiteral("222222222222222222222222");
			const QString sentinelId = QStringLiteral("333333333333333333333333");
			QVERIFY(writeSequencedPlugin(pluginsDir, QStringLiteral("unload_victim.xml"),
			                             QStringLiteral("UnloadVictim"), victimId, 100));
			QVERIFY(writeTextFile(QDir(pluginsDir).filePath(QStringLiteral("unload_target.xml")),
			                      QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin name="UnloadTarget" id="%1" language="lua" enabled="y" save_state="n" sequence="200">
    <script><![CDATA[
function OnPluginClose()
  UnloadPlugin("%2")
end
]]></script>
  </plugin>
</muclient>
)xml")
			                          .arg(targetId, victimId)));
			QVERIFY(writeSequencedPlugin(pluginsDir, QStringLiteral("unload_sentinel.xml"),
			                             QStringLiteral("UnloadSentinel"), sentinelId, 300));

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(pluginsDir);
			QString error;
			for (const QString &fileName :
			     {QStringLiteral("unload_victim.xml"), QStringLiteral("unload_target.xml"),
			      QStringLiteral("unload_sentinel.xml")})
			{
				QVERIFY2(runtime.loadPluginFile(QDir(pluginsDir).filePath(fileName), &error),
				         qPrintable(error));
			}

			QVERIFY2(runtime.unloadPlugin(targetId, &error), qPrintable(error));
			QVERIFY(!runtime.isPluginInstalled(victimId));
			QVERIFY(!runtime.isPluginInstalled(targetId));
			QVERIFY(runtime.isPluginInstalled(sentinelId));
		}

		static void pluginSaveStateRevalidatesTargetAfterCallbackPluginRemoval()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("plugins"));
			const QString stateDir   = QDir(tempDir.path()).filePath(QStringLiteral("state"));
			QVERIFY(QDir().mkpath(pluginsDir));
			QVERIFY(QDir().mkpath(stateDir));
			const QString originalCurrentPath = QDir::currentPath();
			QVERIFY(QDir::setCurrent(tempDir.path()));
			const auto restoreCurrentPath =
			    qScopeGuard([originalCurrentPath] { QDir::setCurrent(originalCurrentPath); });

			const QString victimId   = QStringLiteral("444444444444444444444444");
			const QString targetId   = QStringLiteral("555555555555555555555555");
			const QString sentinelId = QStringLiteral("666666666666666666666666");
			QVERIFY(writeSequencedPlugin(pluginsDir, QStringLiteral("save_victim.xml"),
			                             QStringLiteral("SaveVictim"), victimId, 100));
			QVERIFY(writeTextFile(QDir(pluginsDir).filePath(QStringLiteral("save_target.xml")),
			                      QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin name="SaveTarget" id="%1" language="lua" enabled="y" save_state="y" sequence="200">
    <script><![CDATA[
function OnPluginSaveState()
  SetVariable("owner", "target")
  UnloadPlugin("%2")
end
]]></script>
  </plugin>
</muclient>
)xml")
			                          .arg(targetId, victimId)));
			QVERIFY(writeSequencedPlugin(pluginsDir, QStringLiteral("save_sentinel.xml"),
			                             QStringLiteral("SaveSentinel"), sentinelId, 300));

			const QString worldId = QStringLiteral("777777777777777777777777");
			WorldRuntime  runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(pluginsDir);
			runtime.setStateFilesDirectory(stateDir);
			runtime.setWorldAttribute(QStringLiteral("id"), worldId);
			QString error;
			for (const QString &fileName :
			     {QStringLiteral("save_victim.xml"), QStringLiteral("save_target.xml"),
			      QStringLiteral("save_sentinel.xml")})
			{
				QVERIFY2(runtime.loadPluginFile(QDir(pluginsDir).filePath(fileName), &error),
				         qPrintable(error));
			}
			WorldRuntime::Plugin *sentinel = WorldRuntimeTestAccess::plugin(runtime, sentinelId);
			QVERIFY(sentinel);
			sentinel->variables.insert(QStringLiteral("owner"), QStringLiteral("sentinel"));

			QCOMPARE(runtime.savePluginState(targetId, true, &error), eOK);
			QVERIFY2(error.isEmpty(), qPrintable(error));
			QVERIFY(!runtime.isPluginInstalled(victimId));
			QVERIFY(runtime.isPluginInstalled(targetId));
			QVERIFY(runtime.isPluginInstalled(sentinelId));

			const QString statePath = QDir(stateDir).filePath(worldId + QStringLiteral("-") + targetId +
			                                                  QStringLiteral("-state.xml"));
			QString       savedText;
			QVERIFY(readTextFile(statePath, savedText));
			QCOMPARE(savedStateVariable(savedText, QStringLiteral("owner")), QStringLiteral("target"));
		}

#ifdef QMUD_ENABLE_LUA_SCRIPTING
		static void callPluginLuaKeepsTargetMetadataAcrossCallbackPluginRemoval()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));

			const QString victimId   = QStringLiteral("818181818181818181818181");
			const QString targetId   = QStringLiteral("828282828282828282828282");
			const QString sentinelId = QStringLiteral("838383838383838383838383");
			QVERIFY(writeSequencedPlugin(pluginsDir, QStringLiteral("call_victim.xml"),
			                             QStringLiteral("CallVictim"), victimId, 100));
			QVERIFY(writeTextFile(QDir(pluginsDir).filePath(QStringLiteral("call_target.xml")),
			                      QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin name="CallTarget" id="%1" language="lua" enabled="y" save_state="n" sequence="200">
    <script><![CDATA[
function mutate_and_return_table()
  UnloadPlugin("%2")
  return {}
end
]]></script>
  </plugin>
</muclient>
)xml")
			                          .arg(targetId, victimId)));
			QVERIFY(writeSequencedPlugin(pluginsDir, QStringLiteral("call_sentinel.xml"),
			                             QStringLiteral("CallSentinel"), sentinelId, 300));

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(pluginsDir);
			QString error;
			for (const QString &fileName :
			     {QStringLiteral("call_victim.xml"), QStringLiteral("call_target.xml"),
			      QStringLiteral("call_sentinel.xml")})
			{
				QVERIFY2(runtime.loadPluginFile(QDir(pluginsDir).filePath(fileName), &error),
				         qPrintable(error));
			}
			WorldRuntime::Plugin *target = WorldRuntimeTestAccess::plugin(runtime, targetId);
			QVERIFY(target);
			runtime.setPluginInstallPending(*target, false);

			lua_State *luaState = luaL_newstate();
			QVERIFY(luaState != nullptr);
			const auto closeLuaState = qScopeGuard([luaState] { lua_close(luaState); });
			QCOMPARE(runtime.callPluginLua(targetId, QStringLiteral("mutate_and_return_table"), luaState, 1),
			         2);
			QCOMPARE(static_cast<int>(lua_tointeger(luaState, -2)), eErrorCallingPluginRoutine);
			const QString message = QString::fromUtf8(lua_tostring(luaState, -1));
			QVERIFY(message.contains(QStringLiteral("CallTarget")));
			QVERIFY(!message.contains(QStringLiteral("CallSentinel")));
			QVERIFY(!runtime.isPluginInstalled(victimId));
			QVERIFY(runtime.isPluginInstalled(targetId));
			QVERIFY(runtime.isPluginInstalled(sentinelId));
		}
#endif

		static void teardownRevalidatesPluginsAfterCloseCallbackPluginRemoval()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("plugins"));
			const QString stateDir   = QDir(tempDir.path()).filePath(QStringLiteral("state"));
			QVERIFY(QDir().mkpath(pluginsDir));
			QVERIFY(QDir().mkpath(stateDir));
			const QString originalCurrentPath = QDir::currentPath();
			QVERIFY(QDir::setCurrent(tempDir.path()));
			const auto restoreCurrentPath =
			    qScopeGuard([originalCurrentPath] { QDir::setCurrent(originalCurrentPath); });

			const QString victimId   = QStringLiteral("919191919191919191919191");
			const QString targetId   = QStringLiteral("929292929292929292929292");
			const QString sentinelId = QStringLiteral("939393939393939393939393");
			QVERIFY(writeSequencedPlugin(pluginsDir, QStringLiteral("teardown_victim.xml"),
			                             QStringLiteral("TeardownVictim"), victimId, 100));
			QVERIFY(writeTextFile(QDir(pluginsDir).filePath(QStringLiteral("teardown_target.xml")),
			                      QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin name="TeardownTarget" id="%1" language="lua" enabled="y" save_state="y" sequence="200">
    <script><![CDATA[
function OnPluginClose()
  SetVariable("close_marker", "target")
  UnloadPlugin("%2")
end
]]></script>
  </plugin>
</muclient>
)xml")
			                          .arg(targetId, victimId)));
			QVERIFY(writeTextFile(QDir(pluginsDir).filePath(QStringLiteral("teardown_sentinel.xml")),
			                      QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin name="TeardownSentinel" id="%1" language="lua" enabled="y" save_state="y" sequence="300">
    <script><![CDATA[
function OnPluginClose()
  SetVariable("close_marker", "sentinel")
end
]]></script>
  </plugin>
</muclient>
)xml")
			                          .arg(sentinelId)));

			const QString worldId = QStringLiteral("949494949494949494949494");
			{
				WorldRuntime runtime;
				runtime.setStartupDirectory(tempDir.path());
				runtime.setPluginsDirectory(pluginsDir);
				runtime.setStateFilesDirectory(stateDir);
				runtime.setWorldAttribute(QStringLiteral("id"), worldId);
				QString error;
				for (const QString &fileName :
				     {QStringLiteral("teardown_victim.xml"), QStringLiteral("teardown_target.xml"),
				      QStringLiteral("teardown_sentinel.xml")})
				{
					QVERIFY2(runtime.loadPluginFile(QDir(pluginsDir).filePath(fileName), &error),
					         qPrintable(error));
				}
			}

			const QMap<QString, QString> expectedMarkers{
			    {targetId,   QStringLiteral("target")  },
			    {sentinelId, QStringLiteral("sentinel")},
			};
			for (auto it = expectedMarkers.constBegin(); it != expectedMarkers.constEnd(); ++it)
			{
				const QString statePath = QDir(stateDir).filePath(worldId + QStringLiteral("-") + it.key() +
				                                                  QStringLiteral("-state.xml"));
				QString       savedText;
				QVERIFY(readTextFile(statePath, savedText));
				QCOMPARE(savedStateVariable(savedText, QStringLiteral("close_marker")), it.value());
			}
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
				const auto engine = runtime.plugins().constFirst().lua;
				QVERIFY(engine);
				runtime.dispatchPluginAsyncResult({kTeardownStatePluginId, engine->instanceId(), 1},
				                                  QStringLiteral("teardown-test"), true, 0,
				                                  QStringLiteral("queued"));
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
			const QString replacementPath =
			    QDir(pluginsDir).filePath(QStringLiteral("native_shim_replacement.xml"));
			QVERIFY(writeTextFile(replacementPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<qmud>
  <plugin name="MushReader" id="%1" language="lua" sequence="5000"/>
</qmud>
)xml")
			                                           .arg(QMudNativePluginRegistry::mushReaderPluginId())));
			QVERIFY(writeTextFile(worldPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<qmud>
  <world id="aaaaaaaaaaaaaaaaaaaaaaaa" name="Native Source"/>
  <include name="worlds/plugins/qmud:native/MushReader" plugin="y" enabled="y" sequence="123"/>
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
			QVERIFY(doc.plugins().constFirst().dispatchSequenceOverride.has_value());
			QCOMPARE(*doc.plugins().constFirst().dispatchSequenceOverride, 123);

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
			runtime.setStateFilesDirectory(stateDir);
			runtime.applyFromDocument(doc);
			QCOMPARE(runtime.includes().size(), 1);
			QCOMPARE(runtime.includes().constFirst().attributes.value(QStringLiteral("name")),
			         QStringLiteral("qmud:native/MushReader"));
			const WorldRuntime::Plugin *mushReader =
			    runtime.pluginForId(QMudNativePluginRegistry::mushReaderPluginId());
			QVERIFY(mushReader);
			QCOMPARE(mushReader->sequence, 123);
			QVERIFY(mushReader->dispatchSequenceOverride.has_value());
			QCOMPARE(*mushReader->dispatchSequenceOverride, 123);

			QString replacementError;
			QVERIFY2(runtime.loadPluginFile(QStringLiteral("native_shim_replacement.xml"), &replacementError),
			         qPrintable(replacementError));
			mushReader = runtime.pluginForId(QMudNativePluginRegistry::mushReaderPluginId());
			QVERIFY(mushReader);
			QCOMPARE(mushReader->sequence, 123);
			QVERIFY(mushReader->dispatchSequenceOverride.has_value());
			QCOMPARE(*mushReader->dispatchSequenceOverride, 123);
			QCOMPARE(runtime.pluginInfo(QMudNativePluginRegistry::mushReaderPluginId(), 25).toInt(), 123);
			runtime.setWorldFileModified(true);

			const QString savedPath = QDir(worldsDir).filePath(QStringLiteral("native_source_saved.qdl"));
			QString       saveError;
			QVERIFY2(runtime.saveWorldFile(savedPath, &saveError), qPrintable(saveError));
			QVERIFY(!runtime.worldFileModified());

			QString savedText;
			QVERIFY(readTextFile(savedPath, savedText));
			QVERIFY(savedText.contains(QStringLiteral("name=\"qmud:native/MushReader\"")));
			QVERIFY(savedText.contains(QStringLiteral("sequence=\"123\"")));
			QVERIFY(!savedText.contains(QStringLiteral("worlds/plugins/qmud:native/MushReader")));

			WorldDocument reloaded;
			QVERIFY2(reloaded.loadFromFile(savedPath), qPrintable(reloaded.errorString()));
			QVERIFY2(reloaded.expandIncludes(savedPath, pluginsDir, tempDir.path(), stateDir),
			         qPrintable(reloaded.errorString()));
			QCOMPARE(reloaded.plugins().size(), 1);
			QCOMPARE(reloaded.plugins().constFirst().attributes.value(QStringLiteral("id")),
			         QMudNativePluginRegistry::mushReaderPluginId());
			QVERIFY(reloaded.plugins().constFirst().dispatchSequenceOverride.has_value());
			QCOMPARE(*reloaded.plugins().constFirst().dispatchSequenceOverride, 123);

			WorldRuntime restoredRuntime;
			restoredRuntime.setStartupDirectory(tempDir.path());
			restoredRuntime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
			restoredRuntime.setStateFilesDirectory(stateDir);
			restoredRuntime.applyFromDocument(reloaded);
			QVERIFY(!restoredRuntime.worldFileModified());
			QString reinstallError;
			QCOMPARE(
			    restoredRuntime.reloadPlugin(QMudNativePluginRegistry::mushReaderPluginId(), &reinstallError),
			    eOK);
			QVERIFY2(reinstallError.isEmpty(), qPrintable(reinstallError));
			QMudNativePluginRegistry::NativePluginMetadata metadata;
			QVERIFY(QMudNativePluginRegistry::metadataForShim(QMudNativePluginRegistry::mushReaderPluginId(),
			                                                  metadata));
			mushReader = restoredRuntime.pluginForId(QMudNativePluginRegistry::mushReaderPluginId());
			QVERIFY(mushReader);
			QCOMPARE(mushReader->sequence, metadata.sequence);
			QVERIFY(!mushReader->dispatchSequenceOverride.has_value());
			QVERIFY(restoredRuntime.worldFileModified());

			const QString reinstalledPath =
			    QDir(worldsDir).filePath(QStringLiteral("native_source_reinstalled.qdl"));
			QVERIFY2(restoredRuntime.saveWorldFile(reinstalledPath, &saveError), qPrintable(saveError));
			WorldDocument reinstalledDocument;
			QVERIFY2(reinstalledDocument.loadFromFile(reinstalledPath),
			         qPrintable(reinstalledDocument.errorString()));
			QVERIFY2(
			    reinstalledDocument.expandIncludes(reinstalledPath, pluginsDir, tempDir.path(), stateDir),
			    qPrintable(reinstalledDocument.errorString()));
			QCOMPARE(reinstalledDocument.plugins().size(), 1);
			QVERIFY(!reinstalledDocument.plugins().constFirst().dispatchSequenceOverride.has_value());
		}

		static void pluginReorderPersistsEffectiveSequenceWithoutChangingDeclaredMetadata()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString worldsDir  = QDir(tempDir.path()).filePath(QStringLiteral("worlds"));
			const QString pluginsDir = QDir(worldsDir).filePath(QStringLiteral("plugins"));
			const QString stateDir   = QDir(tempDir.path()).filePath(QStringLiteral("state"));
			QVERIFY(QDir().mkpath(pluginsDir));
			QVERIFY(QDir().mkpath(stateDir));

			const QString firstId  = QStringLiteral("111111111111111111111111");
			const QString secondId = QStringLiteral("222222222222222222222222");
			QVERIFY(writeSequencedPlugin(pluginsDir, QStringLiteral("order_first.xml"),
			                             QStringLiteral("OrderFirst"), firstId, 100));
			QVERIFY(writeSequencedPlugin(pluginsDir, QStringLiteral("order_second.xml"),
			                             QStringLiteral("OrderSecond"), secondId, 200));

			const QString worldPath = QDir(worldsDir).filePath(QStringLiteral("plugin_order.qdl"));
			QVERIFY(writeTextFile(worldPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<qmud>
  <world id="333333333333333333333333" name="Plugin Order"/>
  <include name="order_first.xml" plugin="y"/>
  <include name="order_second.xml" plugin="y"/>
</qmud>
)xml")));

			WorldDocument document;
			QVERIFY2(document.loadFromFile(worldPath), qPrintable(document.errorString()));
			QVERIFY2(document.expandIncludes(worldPath, pluginsDir, tempDir.path(), stateDir),
			         qPrintable(document.errorString()));

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
			runtime.setStateFilesDirectory(stateDir);
			runtime.applyFromDocument(document);
			const auto fixtureOrder = [&firstId, &secondId](const QStringList &pluginIds)
			{
				QStringList order;
				for (const QString &pluginId : pluginIds)
				{
					if (pluginId == firstId || pluginId == secondId)
						order.push_back(pluginId);
				}
				return order;
			};
			QCOMPARE(fixtureOrder(runtime.pluginIdList()), QStringList({firstId, secondId}));
			QVERIFY(runtime.reorderPlugin(secondId, -1));
			QCOMPARE(fixtureOrder(runtime.pluginIdList()), QStringList({secondId, firstId}));
			QCOMPARE(runtime.plugins().at(0).sequence, 100);
			QCOMPARE(runtime.plugins().at(0).attributes.value(QStringLiteral("sequence")),
			         QStringLiteral("200"));
			QVERIFY(runtime.plugins().at(0).dispatchSequenceOverride.has_value());
			QCOMPARE(*runtime.plugins().at(0).dispatchSequenceOverride, 100);
			QCOMPARE(runtime.plugins().at(1).sequence, 200);
			QCOMPARE(runtime.plugins().at(1).attributes.value(QStringLiteral("sequence")),
			         QStringLiteral("100"));
			QVERIFY(runtime.plugins().at(1).dispatchSequenceOverride.has_value());
			QCOMPARE(*runtime.plugins().at(1).dispatchSequenceOverride, 200);

			const QString savedPath = QDir(worldsDir).filePath(QStringLiteral("plugin_order_saved.qdl"));
			QString       saveError;
			QVERIFY2(runtime.saveWorldFile(savedPath, &saveError), qPrintable(saveError));
			QString savedText;
			QVERIFY(readTextFile(savedPath, savedText));
			QVERIFY(savedText.contains(QRegularExpression(
			    QStringLiteral(R"(<include [^>]*name="[^"]*order_second\.xml"[^>]*sequence="100")"))));
			QVERIFY(savedText.contains(QRegularExpression(
			    QStringLiteral(R"(<include [^>]*name="[^"]*order_first\.xml"[^>]*sequence="200")"))));

			WorldDocument reloadedDocument;
			QVERIFY2(reloadedDocument.loadFromFile(savedPath), qPrintable(reloadedDocument.errorString()));
			QVERIFY2(reloadedDocument.expandIncludes(savedPath, pluginsDir, tempDir.path(), stateDir),
			         qPrintable(reloadedDocument.errorString()));
			QCOMPARE(reloadedDocument.plugins().size(), 2);
			QCOMPARE(reloadedDocument.plugins().at(0).attributes.value(QStringLiteral("id")), secondId);
			QCOMPARE(reloadedDocument.plugins().at(0).attributes.value(QStringLiteral("sequence")),
			         QStringLiteral("200"));
			QVERIFY(reloadedDocument.plugins().at(0).dispatchSequenceOverride.has_value());
			QCOMPARE(*reloadedDocument.plugins().at(0).dispatchSequenceOverride, 100);
			QCOMPARE(reloadedDocument.plugins().at(1).attributes.value(QStringLiteral("id")), firstId);
			QCOMPARE(reloadedDocument.plugins().at(1).attributes.value(QStringLiteral("sequence")),
			         QStringLiteral("100"));
			QVERIFY(reloadedDocument.plugins().at(1).dispatchSequenceOverride.has_value());
			QCOMPARE(*reloadedDocument.plugins().at(1).dispatchSequenceOverride, 200);

			WorldRuntime restoredRuntime;
			restoredRuntime.setStartupDirectory(tempDir.path());
			restoredRuntime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
			restoredRuntime.setStateFilesDirectory(stateDir);
			restoredRuntime.applyFromDocument(reloadedDocument);
			QCOMPARE(fixtureOrder(restoredRuntime.pluginIdList()), QStringList({secondId, firstId}));
			QCOMPARE(restoredRuntime.plugins().at(0).sequence, 100);
			QCOMPARE(restoredRuntime.plugins().at(1).sequence, 200);
			QVERIFY(!restoredRuntime.worldFileModified());
			QString reinstallError;
			QCOMPARE(restoredRuntime.reloadPlugin(secondId, &reinstallError), eOK);
			QVERIFY2(reinstallError.isEmpty(), qPrintable(reinstallError));
			QCOMPARE(fixtureOrder(restoredRuntime.pluginIdList()), QStringList({firstId, secondId}));
			const WorldRuntime::Plugin *reinstalledPlugin = restoredRuntime.pluginForId(secondId);
			QVERIFY(reinstalledPlugin);
			QCOMPARE(reinstalledPlugin->sequence, 200);
			QCOMPARE(reinstalledPlugin->attributes.value(QStringLiteral("sequence")), QStringLiteral("200"));
			QVERIFY(!reinstalledPlugin->dispatchSequenceOverride.has_value());
			QVERIFY(restoredRuntime.worldFileModified());

			const QString reinstalledPath =
			    QDir(worldsDir).filePath(QStringLiteral("plugin_order_reinstalled.qdl"));
			QVERIFY2(restoredRuntime.saveWorldFile(reinstalledPath, &saveError), qPrintable(saveError));
			WorldDocument reinstalledDocument;
			QVERIFY2(reinstalledDocument.loadFromFile(reinstalledPath),
			         qPrintable(reinstalledDocument.errorString()));
			QVERIFY2(
			    reinstalledDocument.expandIncludes(reinstalledPath, pluginsDir, tempDir.path(), stateDir),
			    qPrintable(reinstalledDocument.errorString()));
			WorldRuntime reloadedRuntime;
			reloadedRuntime.setStartupDirectory(tempDir.path());
			reloadedRuntime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
			reloadedRuntime.setStateFilesDirectory(stateDir);
			reloadedRuntime.applyFromDocument(reinstalledDocument);
			QCOMPARE(fixtureOrder(reloadedRuntime.pluginIdList()), QStringList({firstId, secondId}));
			const WorldRuntime::Plugin *reloadedPlugin = reloadedRuntime.pluginForId(secondId);
			QVERIFY(reloadedPlugin);
			QVERIFY(!reloadedPlugin->dispatchSequenceOverride.has_value());

			const QString invalidWorldPath =
			    QDir(worldsDir).filePath(QStringLiteral("plugin_order_invalid.qdl"));
			QVERIFY(
			    writeTextFile(invalidWorldPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<qmud>
  <world id="444444444444444444444444" name="Invalid Plugin Order"/>
  <include name="order_first.xml" plugin="y" sequence="10001"/>
</qmud>
)xml")));
			WorldDocument invalidDocument;
			QVERIFY2(invalidDocument.loadFromFile(invalidWorldPath),
			         qPrintable(invalidDocument.errorString()));
			QVERIFY(!invalidDocument.expandIncludes(invalidWorldPath, pluginsDir, tempDir.path(), stateDir));
			QVERIFY(invalidDocument.errorString().contains(QStringLiteral("invalid sequence override")));

			const QString invalidDeclaredId = QStringLiteral("888888888888888888888888");
			QVERIFY(writeSequencedPlugin(pluginsDir, QStringLiteral("order_invalid_declared.xml"),
			                             QStringLiteral("OrderInvalidDeclared"), invalidDeclaredId, 10001));
			const QString invalidDeclaredWorldPath =
			    QDir(worldsDir).filePath(QStringLiteral("plugin_order_invalid_declared.qdl"));
			QVERIFY(writeTextFile(invalidDeclaredWorldPath,
			                      QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<qmud>
  <world id="999999999999999999999999" name="Invalid Declared Plugin Order"/>
  <include name="order_invalid_declared.xml" plugin="y"/>
</qmud>
)xml")));
			WorldDocument invalidDeclaredDocument;
			QVERIFY2(invalidDeclaredDocument.loadFromFile(invalidDeclaredWorldPath),
			         qPrintable(invalidDeclaredDocument.errorString()));
			QVERIFY(!invalidDeclaredDocument.expandIncludes(invalidDeclaredWorldPath, pluginsDir,
			                                                tempDir.path(), stateDir));
			QVERIFY(invalidDeclaredDocument.errorString().contains(QStringLiteral("invalid sequence")));
		}

		static void localPluginEnabledOverridesRoundTripInBothDirections()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString worldsDir  = QDir(tempDir.path()).filePath(QStringLiteral("worlds"));
			const QString pluginsDir = QDir(worldsDir).filePath(QStringLiteral("plugins"));
			const QString stateDir   = QDir(tempDir.path()).filePath(QStringLiteral("state"));
			QVERIFY(QDir().mkpath(pluginsDir));
			QVERIFY(QDir().mkpath(stateDir));

			const QString initiallyDisabledId = QStringLiteral("343434343434343434343434");
			const QString initiallyEnabledId  = QStringLiteral("565656565656565656565656");
			QVERIFY(writeTextFile(QDir(pluginsDir).filePath(QStringLiteral("initially_disabled.xml")),
			                      QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<qmud>
  <plugin name="InitiallyDisabled" id="%1" sequence="100" enabled="n"/>
</qmud>
)xml")
			                          .arg(initiallyDisabledId)));
			QVERIFY(writeTextFile(QDir(pluginsDir).filePath(QStringLiteral("initially_enabled.xml")),
			                      QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<qmud>
  <plugin name="InitiallyEnabled" id="%1" sequence="200" enabled="y"/>
</qmud>
)xml")
			                          .arg(initiallyEnabledId)));

			const QString worldPath = QDir(worldsDir).filePath(QStringLiteral("plugin_enabled.qdl"));
			QVERIFY(writeTextFile(worldPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<qmud>
  <world id="787878787878787878787878" name="Plugin Enabled Overrides"/>
  <include name="initially_disabled.xml" plugin="y"/>
  <include name="initially_enabled.xml" plugin="y"/>
</qmud>
)xml")));

			WorldDocument document;
			QVERIFY2(document.loadFromFile(worldPath), qPrintable(document.errorString()));
			QVERIFY2(document.expandIncludes(worldPath, pluginsDir, tempDir.path(), stateDir),
			         qPrintable(document.errorString()));

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
			runtime.setStateFilesDirectory(stateDir);
			runtime.applyFromDocument(document);
			const WorldRuntime::Plugin *initiallyDisabled = runtime.pluginForId(initiallyDisabledId);
			const WorldRuntime::Plugin *initiallyEnabled  = runtime.pluginForId(initiallyEnabledId);
			QVERIFY(initiallyDisabled);
			QVERIFY(initiallyEnabled);
			QVERIFY(!initiallyDisabled->enabled);
			QVERIFY(initiallyEnabled->enabled);

			runtime.setWorldFileModified(false);
			QVERIFY(runtime.enablePlugin(initiallyDisabledId, true));
			QVERIFY(runtime.worldFileModified());
			runtime.setWorldFileModified(false);
			QVERIFY(runtime.enablePlugin(initiallyEnabledId, false));
			QVERIFY(runtime.worldFileModified());

			QString reloadError;
			runtime.setWorldFileModified(false);
			QCOMPARE(runtime.reloadPlugin(initiallyDisabledId, &reloadError), eOK);
			QVERIFY2(reloadError.isEmpty(), qPrintable(reloadError));
			initiallyDisabled = runtime.pluginForId(initiallyDisabledId);
			QVERIFY(initiallyDisabled);
			QVERIFY(initiallyDisabled->enabled);
			QVERIFY(runtime.worldFileModified());

			reloadError.clear();
			runtime.setWorldFileModified(false);
			QCOMPARE(runtime.reloadPlugin(initiallyEnabledId, &reloadError), eOK);
			QVERIFY2(reloadError.isEmpty(), qPrintable(reloadError));
			initiallyEnabled = runtime.pluginForId(initiallyEnabledId);
			QVERIFY(initiallyEnabled);
			QVERIFY(!initiallyEnabled->enabled);
			QVERIFY(runtime.worldFileModified());

			const QString savedPath = QDir(worldsDir).filePath(QStringLiteral("plugin_enabled_saved.qdl"));
			QString       saveError;
			QVERIFY2(runtime.saveWorldFile(savedPath, &saveError), qPrintable(saveError));

			WorldDocument reloadedDocument;
			QVERIFY2(reloadedDocument.loadFromFile(savedPath), qPrintable(reloadedDocument.errorString()));
			QVERIFY2(reloadedDocument.expandIncludes(savedPath, pluginsDir, tempDir.path(), stateDir),
			         qPrintable(reloadedDocument.errorString()));

			WorldRuntime restoredRuntime;
			restoredRuntime.setStartupDirectory(tempDir.path());
			restoredRuntime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
			restoredRuntime.setStateFilesDirectory(stateDir);
			restoredRuntime.applyFromDocument(reloadedDocument);
			initiallyDisabled = restoredRuntime.pluginForId(initiallyDisabledId);
			initiallyEnabled  = restoredRuntime.pluginForId(initiallyEnabledId);
			QVERIFY(initiallyDisabled);
			QVERIFY(initiallyEnabled);
			QVERIFY(initiallyDisabled->enabled);
			QVERIFY(!initiallyEnabled->enabled);
			QVERIFY(!restoredRuntime.worldFileModified());
		}

		static void equalSequencePluginReorderPersistsByIncludeOrder()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString worldsDir  = QDir(tempDir.path()).filePath(QStringLiteral("worlds"));
			const QString pluginsDir = QDir(worldsDir).filePath(QStringLiteral("plugins"));
			const QString stateDir   = QDir(tempDir.path()).filePath(QStringLiteral("state"));
			QVERIFY(QDir().mkpath(pluginsDir));
			QVERIFY(QDir().mkpath(stateDir));

			const QString firstId  = QStringLiteral("555555555555555555555555");
			const QString secondId = QStringLiteral("666666666666666666666666");
			QVERIFY(writeSequencedPlugin(pluginsDir, QStringLiteral("equal_first.xml"),
			                             QStringLiteral("EqualFirst"), firstId, 300));
			QVERIFY(writeSequencedPlugin(pluginsDir, QStringLiteral("equal_second.xml"),
			                             QStringLiteral("EqualSecond"), secondId, 300));

			const QString worldPath = QDir(worldsDir).filePath(QStringLiteral("equal_order.qdl"));
			QVERIFY(writeTextFile(worldPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<qmud>
  <world id="777777777777777777777777" name="Equal Plugin Order"/>
  <include name="equal_first.xml" plugin="y"/>
  <include name="equal_second.xml" plugin="y"/>
</qmud>
)xml")));

			WorldDocument document;
			QVERIFY2(document.loadFromFile(worldPath), qPrintable(document.errorString()));
			QVERIFY2(document.expandIncludes(worldPath, pluginsDir, tempDir.path(), stateDir),
			         qPrintable(document.errorString()));
			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
			runtime.setStateFilesDirectory(stateDir);
			runtime.applyFromDocument(document);
			const auto fixtureOrder = [&firstId, &secondId](const QStringList &pluginIds)
			{
				QStringList order;
				for (const QString &pluginId : pluginIds)
				{
					if (pluginId == firstId || pluginId == secondId)
						order.push_back(pluginId);
				}
				return order;
			};
			QVERIFY(runtime.reorderPlugin(secondId, -1));
			QCOMPARE(fixtureOrder(runtime.pluginIdList()), QStringList({secondId, firstId}));
			QVERIFY(!runtime.plugins().at(0).dispatchSequenceOverride.has_value());
			QVERIFY(!runtime.plugins().at(1).dispatchSequenceOverride.has_value());

			const QString savedPath = QDir(worldsDir).filePath(QStringLiteral("equal_order_saved.qdl"));
			QString       saveError;
			QVERIFY2(runtime.saveWorldFile(savedPath, &saveError), qPrintable(saveError));
			QString savedText;
			QVERIFY(readTextFile(savedPath, savedText));
			QVERIFY(!savedText.contains(QRegularExpression(
			    QStringLiteral(R"(<include [^>]*name="[^"]*equal_[^"]*"[^>]*sequence=)"))));

			WorldDocument reloadedDocument;
			QVERIFY2(reloadedDocument.loadFromFile(savedPath), qPrintable(reloadedDocument.errorString()));
			QVERIFY2(reloadedDocument.expandIncludes(savedPath, pluginsDir, tempDir.path(), stateDir),
			         qPrintable(reloadedDocument.errorString()));
			WorldRuntime restoredRuntime;
			restoredRuntime.setStartupDirectory(tempDir.path());
			restoredRuntime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
			restoredRuntime.setStateFilesDirectory(stateDir);
			restoredRuntime.applyFromDocument(reloadedDocument);
			QCOMPARE(fixtureOrder(restoredRuntime.pluginIdList()), QStringList({secondId, firstId}));
			QCOMPARE(restoredRuntime.plugins().at(0).sequence, 300);
			QCOMPARE(restoredRuntime.plugins().at(1).sequence, 300);
			QVERIFY(!restoredRuntime.worldFileModified());
			QString reinstallError;
			QCOMPARE(restoredRuntime.reloadPlugin(secondId, &reinstallError), eOK);
			QVERIFY2(reinstallError.isEmpty(), qPrintable(reinstallError));
			QCOMPARE(fixtureOrder(restoredRuntime.pluginIdList()), QStringList({firstId, secondId}));
			QVERIFY(restoredRuntime.worldFileModified());

			const QString reinstalledPath =
			    QDir(worldsDir).filePath(QStringLiteral("equal_order_reinstalled.qdl"));
			QVERIFY2(restoredRuntime.saveWorldFile(reinstalledPath, &saveError), qPrintable(saveError));
			WorldDocument reinstalledDocument;
			QVERIFY2(reinstalledDocument.loadFromFile(reinstalledPath),
			         qPrintable(reinstalledDocument.errorString()));
			QVERIFY2(
			    reinstalledDocument.expandIncludes(reinstalledPath, pluginsDir, tempDir.path(), stateDir),
			    qPrintable(reinstalledDocument.errorString()));
			WorldRuntime reloadedRuntime;
			reloadedRuntime.setStartupDirectory(tempDir.path());
			reloadedRuntime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
			reloadedRuntime.setStateFilesDirectory(stateDir);
			reloadedRuntime.applyFromDocument(reinstalledDocument);
			QCOMPARE(fixtureOrder(reloadedRuntime.pluginIdList()), QStringList({firstId, secondId}));
		}

		static void globalPluginsAreExcludedFromPluginReordering()
		{
			const auto makePlugin = [](const QString &id, const int sequence, const bool global)
			{
				WorldRuntime::Plugin plugin;
				plugin.attributes.insert(QStringLiteral("id"), id);
				plugin.attributes.insert(QStringLiteral("sequence"), QString::number(sequence));
				plugin.sequence = sequence;
				plugin.global   = global;
				return plugin;
			};

			const QString firstId  = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaa1");
			const QString globalId = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaa2");
			const QString secondId = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaa3");
			WorldRuntime  runtime;
			WorldRuntimeTestAccess::plugins(runtime) = {makePlugin(firstId, 100, false),
			                                            makePlugin(globalId, 100, true),
			                                            makePlugin(secondId, 200, false)};

			QVERIFY(!runtime.reorderPlugin(globalId, -1));
			QVERIFY(!runtime.worldFileModified());
			QVERIFY(runtime.reorderPlugin(secondId, -1));
			QVERIFY(runtime.worldFileModified());
			QCOMPARE(runtime.plugins().at(0).attributes.value(QStringLiteral("id")), secondId);
			QCOMPARE(runtime.plugins().at(1).attributes.value(QStringLiteral("id")), globalId);
			QCOMPARE(runtime.plugins().at(2).attributes.value(QStringLiteral("id")), firstId);
			QCOMPARE(runtime.plugins().at(1).sequence, 100);
			QVERIFY(!runtime.plugins().at(1).dispatchSequenceOverride.has_value());
			QVERIFY(!runtime.reorderPlugin(secondId, -1));
		}

		static void persistentPluginMembershipDirtinessIsCentralized()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));
			const QString localId  = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbb1");
			const QString globalId = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbb2");
			QVERIFY(writeSequencedPlugin(pluginsDir, QStringLiteral("local_membership.xml"),
			                             QStringLiteral("LocalMembership"), localId, 100));
			QVERIFY(writeSequencedPlugin(pluginsDir, QStringLiteral("global_membership.xml"),
			                             QStringLiteral("GlobalMembership"), globalId, 200));

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(pluginsDir);
			QString error;
			runtime.setWorldFileModified(false);
			QVERIFY2(runtime.loadPluginFile(QDir(pluginsDir).filePath(QStringLiteral("local_membership.xml")),
			                                &error, false),
			         qPrintable(error));
			QVERIFY(runtime.worldFileModified());

			runtime.setWorldFileModified(false);
			QVERIFY(runtime.unloadPlugin(localId, &error));
			QVERIFY(runtime.worldFileModified());

			runtime.setWorldFileModified(false);
			QVERIFY2(runtime.loadPluginFile(
			             QDir(pluginsDir).filePath(QStringLiteral("global_membership.xml")), &error, true),
			         qPrintable(error));
			QVERIFY(!runtime.worldFileModified());

			runtime.setWorldFileModified(false);
			QVERIFY(runtime.unloadPlugin(globalId, &error));
			QVERIFY(!runtime.worldFileModified());
		}

		static void luaPluginMembershipApisUseCentralizedDirtiness()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));
			const QString controllerId = QStringLiteral("ccccccccccccccccccccccc1");
			const QString targetId     = QStringLiteral("ccccccccccccccccccccccc2");
			const QString targetPath = QDir(pluginsDir).filePath(QStringLiteral("lua_membership_target.xml"));
			QVERIFY(writeSequencedPlugin(pluginsDir, QStringLiteral("lua_membership_target.xml"),
			                             QStringLiteral("LuaMembershipTarget"), targetId, 200));
			const QString controllerPath =
			    QDir(pluginsDir).filePath(QStringLiteral("lua_membership_controller.xml"));
			const QString controllerContents = QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin name="LuaMembershipController" id="%1" language="lua" enabled="y" save_state="n"
          sequence="100">
    <script><![CDATA[
function load_membership_target(path)
  local status = LoadPlugin(path)
  SetVariable("load_membership_status", string.format("%.0f", status))
end
function unload_membership_target(id)
  SetVariable("dirty_before_unload", tostring(GetInfo(111)))
  local status = UnloadPlugin(id)
  SetVariable("unload_membership_status", string.format("%.0f", status))
  SetVariable("dirty_after_unload", tostring(GetInfo(111)))
end
function disable_membership_target(id)
  SetVariable("dirty_before_enable", tostring(GetInfo(111)))
  local status = EnablePlugin(id, false)
  SetVariable("enable_membership_status", string.format("%.0f", status))
  SetVariable("dirty_after_enable", tostring(GetInfo(111)))
end
]]></script>
  </plugin>
</muclient>
)xml")
			                                       .arg(controllerId);
			QVERIFY(writeTextFile(controllerPath, controllerContents));

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(pluginsDir);
			WorldView view;
			view.resize(640, 480);
			view.setRuntime(&runtime);
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));
			QString error;
			QVERIFY2(runtime.loadPluginFile(controllerPath, &error, false), qPrintable(error));
			QTRY_VERIFY_WITH_TIMEOUT(runtime.pluginForId(controllerId) &&
			                             !runtime.pluginForId(controllerId)->installPending,
			                         5000);
			QCOMPARE(runtime.pluginSupports(controllerId, QStringLiteral("load_membership_target")), eOK);
			QCOMPARE(runtime.pluginSupports(controllerId, QStringLiteral("unload_membership_target")), eOK);
			QCOMPARE(runtime.pluginSupports(controllerId, QStringLiteral("disable_membership_target")), eOK);

			runtime.setWorldFileModified(false);
			QCOMPARE(runtime.callPlugin(controllerId, QStringLiteral("load_membership_target"), targetPath),
			         eOK);
			QTRY_VERIFY_WITH_TIMEOUT(runtime.isPluginInstalled(targetId), 5000);
			QTRY_COMPARE_WITH_TIMEOUT(
			    pluginVariable(runtime, controllerId, QStringLiteral("load_membership_status")),
			    QStringLiteral("0"), 5000);
			QVERIFY(runtime.worldFileModified());
			QTRY_VERIFY_WITH_TIMEOUT(!runtime.m_pluginCallbackDispatchActive &&
			                             !runtime.m_pluginCallbackDispatchWorkerInFlight &&
			                             runtime.m_pluginCallbackDispatchQueue.isEmpty(),
			                         5000);

			runtime.setWorldFileModified(false);
			QCOMPARE(runtime.callPlugin(controllerId, QStringLiteral("disable_membership_target"), targetId),
			         eOK);
			QTRY_COMPARE_WITH_TIMEOUT(
			    pluginVariable(runtime, controllerId, QStringLiteral("enable_membership_status")),
			    QStringLiteral("0"), 5000);
			QCOMPARE(pluginVariable(runtime, controllerId, QStringLiteral("dirty_before_enable")),
			         QStringLiteral("false"));
			QCOMPARE(pluginVariable(runtime, controllerId, QStringLiteral("dirty_after_enable")),
			         QStringLiteral("true"));
			QVERIFY(runtime.worldFileModified());

			// Repeating a successful persistence request is still dirty immediately in the callback
			// overlay and after its deferred runtime application.
			runtime.setWorldFileModified(false);
			QCOMPARE(runtime.callPlugin(controllerId, QStringLiteral("disable_membership_target"), targetId),
			         eOK);
			QTRY_COMPARE_WITH_TIMEOUT(
			    pluginVariable(runtime, controllerId, QStringLiteral("enable_membership_status")),
			    QStringLiteral("0"), 5000);
			QCOMPARE(pluginVariable(runtime, controllerId, QStringLiteral("dirty_before_enable")),
			         QStringLiteral("false"));
			QCOMPARE(pluginVariable(runtime, controllerId, QStringLiteral("dirty_after_enable")),
			         QStringLiteral("true"));
			QVERIFY(runtime.worldFileModified());

			runtime.setWorldFileModified(false);
			QCOMPARE(runtime.callPlugin(controllerId, QStringLiteral("unload_membership_target"), targetId),
			         eOK);
			QTRY_VERIFY_WITH_TIMEOUT(!runtime.isPluginInstalled(targetId), 5000);
			QTRY_COMPARE_WITH_TIMEOUT(
			    pluginVariable(runtime, controllerId, QStringLiteral("unload_membership_status")),
			    QStringLiteral("0"), 5000);
			QCOMPARE(pluginVariable(runtime, controllerId, QStringLiteral("dirty_before_unload")),
			         QStringLiteral("false"));
			QCOMPARE(pluginVariable(runtime, controllerId, QStringLiteral("dirty_after_unload")),
			         QStringLiteral("true"));
			QVERIFY(runtime.worldFileModified());

			QVERIFY2(runtime.loadPluginFile(targetPath, &error, true), qPrintable(error));
			QTRY_VERIFY_WITH_TIMEOUT(
			    runtime.pluginForId(targetId) && !runtime.pluginForId(targetId)->installPending, 5000);
			runtime.setWorldFileModified(false);
			QCOMPARE(runtime.callPlugin(controllerId, QStringLiteral("disable_membership_target"), targetId),
			         eOK);
			QTRY_COMPARE_WITH_TIMEOUT(
			    pluginVariable(runtime, controllerId, QStringLiteral("enable_membership_status")),
			    QStringLiteral("0"), 5000);
			QCOMPARE(pluginVariable(runtime, controllerId, QStringLiteral("dirty_before_enable")),
			         QStringLiteral("false"));
			QCOMPARE(pluginVariable(runtime, controllerId, QStringLiteral("dirty_after_enable")),
			         QStringLiteral("true"));
			QVERIFY(runtime.worldFileModified());

			runtime.setWorldFileModified(false);
			QCOMPARE(runtime.callPlugin(controllerId, QStringLiteral("disable_membership_target"), targetId),
			         eOK);
			QTRY_COMPARE_WITH_TIMEOUT(
			    pluginVariable(runtime, controllerId, QStringLiteral("enable_membership_status")),
			    QStringLiteral("0"), 5000);
			QCOMPARE(pluginVariable(runtime, controllerId, QStringLiteral("dirty_before_enable")),
			         QStringLiteral("false"));
			QCOMPARE(pluginVariable(runtime, controllerId, QStringLiteral("dirty_after_enable")),
			         QStringLiteral("true"));
			QVERIFY(runtime.worldFileModified());

			runtime.setWorldFileModified(false);
			QCOMPARE(runtime.callPlugin(controllerId, QStringLiteral("unload_membership_target"), targetId),
			         eOK);
			QTRY_VERIFY_WITH_TIMEOUT(!runtime.isPluginInstalled(targetId), 5000);
			QCOMPARE(pluginVariable(runtime, controllerId, QStringLiteral("dirty_before_unload")),
			         QStringLiteral("false"));
			QCOMPARE(pluginVariable(runtime, controllerId, QStringLiteral("dirty_after_unload")),
			         QStringLiteral("false"));
			QVERIFY(!runtime.worldFileModified());
		}

		static void nativePluginReinstallRestoresCanonicalSequenceOrder()
		{
			const auto makePlugin = [](const QString &id, const int sequence, const bool global)
			{
				WorldRuntime::Plugin plugin;
				plugin.attributes.insert(QStringLiteral("id"), id);
				plugin.attributes.insert(QStringLiteral("sequence"), QString::number(sequence));
				plugin.sequence = sequence;
				plugin.global   = global;
				return plugin;
			};
			QMudNativePluginRegistry::NativePluginMetadata metadata;
			QVERIFY(QMudNativePluginRegistry::metadataForShim(QMudNativePluginRegistry::mushReaderPluginId(),
			                                                  metadata));
			const QString        localId      = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaa4");
			const QString        globalId     = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaa5");
			const QString        equalId      = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaa6");
			WorldRuntime::Plugin nativePlugin = makePlugin(metadata.id, 50, false);
			nativePlugin.attributes.insert(QStringLiteral("name"), metadata.name);
			nativePlugin.nativeShim               = true;
			nativePlugin.dispatchSequenceOverride = 50;

			WorldRuntime::Plugin localPlugin = makePlugin(localId, 300, false);
			WorldRuntime::Timer  localTimer;
			localTimer.attributes.insert(QStringLiteral("name"), QStringLiteral("surviving-timer"));
			localPlugin.timers.push_back(localTimer);

			WorldRuntime runtime;
			WorldRuntimeTestAccess::plugins(runtime) = {nativePlugin, makePlugin(globalId, 250, true),
			                                            localPlugin,
			                                            makePlugin(equalId, metadata.sequence, false)};
			const quint64 timerMutationSerial        = runtime.timerStructureMutationSerial();
			QString       reinstallError;
			QCOMPARE(runtime.reloadPlugin(metadata.id, &reinstallError), eOK);
			QVERIFY2(reinstallError.isEmpty(), qPrintable(reinstallError));
			QCOMPARE(runtime.plugins().at(0).attributes.value(QStringLiteral("id")), globalId);
			QCOMPARE(runtime.plugins().at(1).attributes.value(QStringLiteral("id")), localId);
			QCOMPARE(runtime.plugins().at(2).attributes.value(QStringLiteral("id")), equalId);
			QCOMPARE(runtime.plugins().at(3).attributes.value(QStringLiteral("id")), metadata.id);
			QVERIFY(std::ranges::is_sorted(runtime.plugins(), {}, &WorldRuntime::Plugin::sequence));
			QCOMPARE(runtime.plugins().at(3).sequence, metadata.sequence);
			QVERIFY(!runtime.plugins().at(3).dispatchSequenceOverride.has_value());
			QCOMPARE(runtime.timerStructureMutationSerial(), timerMutationSerial + 1);
			const WorldRuntime::Plugin *survivingPlugin = runtime.pluginForId(localId);
			QVERIFY(survivingPlugin);
			QCOMPARE(survivingPlugin->timers.size(), 1);
			QCOMPARE(survivingPlugin->timers.constFirst().attributes.value(QStringLiteral("name")),
			         QStringLiteral("surviving-timer"));
			QVERIFY(runtime.worldFileModified());
		}

		static void globalPluginEnabledOverridesDirtyAndRoundTrip()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("plugins"));
			const QString stateDir   = QDir(tempDir.path()).filePath(QStringLiteral("state"));
			QVERIFY(QDir().mkpath(pluginsDir));
			QVERIFY(QDir().mkpath(stateDir));

			const QString globalId   = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaa7");
			const QString pluginPath = QDir(pluginsDir).filePath(QStringLiteral("global.xml"));
			QVERIFY(writeSequencedPlugin(pluginsDir, QStringLiteral("global.xml"), QStringLiteral("Global"),
			                             globalId, 100));
			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(pluginsDir);
			runtime.setStateFilesDirectory(stateDir);
			QString error;
			QVERIFY2(runtime.loadPluginFile(pluginPath, &error, true), qPrintable(error));
			runtime.setWorldFileModified(false);

			QVERIFY(runtime.enablePlugin(globalId, false));
			QVERIFY(runtime.worldFileModified());
			WorldRuntime::Plugin *globalPlugin = WorldRuntimeTestAccess::plugin(runtime, globalId);
			QVERIFY(globalPlugin);
			QVERIFY(globalPlugin->global);
			QVERIFY(!globalPlugin->enabled);

			const QString disabledWorldPath =
			    QDir(tempDir.path()).filePath(QStringLiteral("global-disabled.qdl"));
			QVERIFY2(runtime.saveWorldFile(disabledWorldPath, &error), qPrintable(error));

			WorldDocument disabledDocument;
			QVERIFY2(disabledDocument.loadFromFile(disabledWorldPath),
			         qPrintable(disabledDocument.errorString()));
			QVERIFY2(
			    disabledDocument.expandIncludes(disabledWorldPath, tempDir.path(), tempDir.path(), stateDir),
			    qPrintable(disabledDocument.errorString()));
			QCOMPARE(disabledDocument.plugins().size(), 1);
			QCOMPARE(disabledDocument.plugins().constFirst().attributes.value(QStringLiteral("id")),
			         globalId);
			QCOMPARE(disabledDocument.plugins().constFirst().attributes.value(QStringLiteral("enabled")),
			         QStringLiteral("n"));
			WorldRuntime disabledRuntime;
			disabledRuntime.setStartupDirectory(tempDir.path());
			disabledRuntime.setPluginsDirectory(pluginsDir);
			disabledRuntime.setStateFilesDirectory(stateDir);
			disabledRuntime.applyFromDocument(disabledDocument);
			QVERIFY2(disabledRuntime.loadPluginFile(pluginPath, &error, true), qPrintable(error));
			globalPlugin = WorldRuntimeTestAccess::plugin(disabledRuntime, globalId);
			QVERIFY(globalPlugin);
			QVERIFY(globalPlugin->global);
			QVERIFY(!globalPlugin->enabled);
			QVERIFY(!disabledRuntime.worldFileModified());

			QVERIFY(disabledRuntime.enablePlugin(globalId, true));
			QVERIFY(disabledRuntime.worldFileModified());
			globalPlugin = WorldRuntimeTestAccess::plugin(disabledRuntime, globalId);
			QVERIFY(globalPlugin);
			const QString enabledWorldPath =
			    QDir(tempDir.path()).filePath(QStringLiteral("global-enabled.qdl"));
			QVERIFY2(disabledRuntime.saveWorldFile(enabledWorldPath, &error), qPrintable(error));
			WorldDocument enabledDocument;
			QVERIFY2(enabledDocument.loadFromFile(enabledWorldPath),
			         qPrintable(enabledDocument.errorString()));
			QVERIFY2(
			    enabledDocument.expandIncludes(enabledWorldPath, tempDir.path(), tempDir.path(), stateDir),
			    qPrintable(enabledDocument.errorString()));
			QCOMPARE(enabledDocument.plugins().size(), 1);
			QCOMPARE(enabledDocument.plugins().constFirst().attributes.value(QStringLiteral("id")), globalId);
			QCOMPARE(enabledDocument.plugins().constFirst().attributes.value(QStringLiteral("enabled")),
			         QStringLiteral("y"));

			WorldRuntime enabledRuntime;
			enabledRuntime.setStartupDirectory(tempDir.path());
			enabledRuntime.setPluginsDirectory(pluginsDir);
			enabledRuntime.setStateFilesDirectory(stateDir);
			enabledRuntime.applyFromDocument(enabledDocument);
			QVERIFY2(enabledRuntime.loadPluginFile(pluginPath, &error, true), qPrintable(error));
			const WorldRuntime::Plugin *enabledGlobalPlugin =
			    WorldRuntimeTestAccess::plugin(enabledRuntime, globalId);
			QVERIFY(enabledGlobalPlugin);
			QVERIFY(enabledGlobalPlugin->global);
			QVERIFY(enabledGlobalPlugin->enabled);
			QVERIFY(!enabledRuntime.worldFileModified());

			globalPlugin->disableAfterInstall = true;
			disabledRuntime.setWorldFileModified(false);
			QVERIFY(disabledRuntime.enablePlugin(globalId, true));
			QVERIFY(!globalPlugin->disableAfterInstall);
			QVERIFY(disabledRuntime.worldFileModified());

			WorldView installView;
			installView.resize(640, 480);
			installView.setRuntime(&disabledRuntime);
			installView.show();
			QVERIFY(QTest::qWaitForWindowExposed(&installView));
			disabledRuntime.setWorldFileModified(false);
			QVERIFY(writeTextFile(pluginPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin name="Global" id="%1" language="lua" enabled="n" save_state="n" sequence="100"/>
</muclient>
)xml")
			                                      .arg(globalId)));
			QCOMPARE(disabledRuntime.reloadPlugin(globalId, &error), eOK);
			QVERIFY2(error.isEmpty(), qPrintable(error));
			QVERIFY(!disabledRuntime.worldFileModified());
			QTRY_VERIFY_WITH_TIMEOUT(disabledRuntime.pluginForId(globalId) &&
			                             !disabledRuntime.pluginForId(globalId)->installPending,
			                         5000);
			globalPlugin = WorldRuntimeTestAccess::plugin(disabledRuntime, globalId);
			QVERIFY(globalPlugin);
			QVERIFY(globalPlugin->global);
			QVERIFY(globalPlugin->enabled);
			QVERIFY(!globalPlugin->disableAfterInstall);
			QVERIFY(!disabledRuntime.worldFileModified());

			QMudNativePluginRegistry::NativePluginMetadata metadata;
			QVERIFY(QMudNativePluginRegistry::metadataForShim(QMudNativePluginRegistry::mushReaderPluginId(),
			                                                  metadata));
			WorldRuntime::Plugin nativeGlobalPlugin;
			nativeGlobalPlugin.attributes.insert(QStringLiteral("id"), metadata.id);
			nativeGlobalPlugin.attributes.insert(QStringLiteral("name"), metadata.name);
			nativeGlobalPlugin.attributes.insert(QStringLiteral("sequence"),
			                                     QString::number(metadata.sequence));
			nativeGlobalPlugin.attributes.insert(QStringLiteral("enabled"), QStringLiteral("1"));
			nativeGlobalPlugin.sequence   = metadata.sequence;
			nativeGlobalPlugin.enabled    = true;
			nativeGlobalPlugin.global     = true;
			nativeGlobalPlugin.nativeShim = true;

			WorldRuntime nativeRuntime;
			WorldRuntimeTestAccess::plugins(nativeRuntime) = {nativeGlobalPlugin};
			nativeRuntime.setWorldFileModified(false);
			QCOMPARE(nativeRuntime.reloadPlugin(metadata.id, &error), eOK);
			QVERIFY2(error.isEmpty(), qPrintable(error));
			QVERIFY(!nativeRuntime.worldFileModified());
		}

		static void successfulPluginEnableRequestsAlwaysDirty()
		{
			const QString        pluginId = QStringLiteral("b00112233445566778899aab");
			WorldRuntime         runtime;
			WorldRuntime::Plugin plugin;
			plugin.attributes.insert(QStringLiteral("id"), pluginId);
			plugin.attributes.insert(QStringLiteral("name"), QStringLiteral("DirtyRequest"));
			plugin.attributes.insert(QStringLiteral("enabled"), QStringLiteral("1"));
			plugin.enabled = true;
			WorldRuntimeTestAccess::plugins(runtime).push_back(plugin);

			runtime.setWorldFileModified(false);
			QVERIFY(runtime.enablePlugin(pluginId, true));
			QVERIFY(runtime.worldFileModified());

			runtime.setWorldFileModified(false);
			QVERIFY(runtime.enablePlugin(pluginId, false));
			QVERIFY(runtime.worldFileModified());

			runtime.setWorldFileModified(false);
			QVERIFY(runtime.enablePlugin(pluginId, false));
			QVERIFY(runtime.worldFileModified());

			WorldRuntime::Plugin *installedPlugin = WorldRuntimeTestAccess::plugin(runtime, pluginId);
			QVERIFY(installedPlugin);
			installedPlugin->global = true;
			runtime.setWorldFileModified(false);
			QVERIFY(runtime.enablePlugin(pluginId, false));
			QVERIFY(runtime.worldFileModified());

			installedPlugin->lua                 = QSharedPointer<LuaCallbackEngine>::create();
			installedPlugin->enabled             = true;
			installedPlugin->installPending      = true;
			installedPlugin->disableAfterInstall = false;
			installedPlugin->attributes.insert(QStringLiteral("enabled"), QStringLiteral("1"));
			runtime.setWorldFileModified(false);
			QVERIFY(runtime.enablePlugin(pluginId, false));
			QVERIFY(runtime.worldFileModified());
			QVERIFY(installedPlugin->enabled);
			QVERIFY(installedPlugin->disableAfterInstall);
			QCOMPARE(installedPlugin->attributes.value(QStringLiteral("enabled")), QStringLiteral("0"));

			runtime.setWorldFileModified(false);
			QVERIFY(!runtime.enablePlugin(QStringLiteral("b10112233445566778899aab"), false));
			QVERIFY(!runtime.worldFileModified());

			WorldRuntime nativeShimRuntime;
			nativeShimRuntime.setWorldFileModified(false);
			QVERIFY(nativeShimRuntime.enablePlugin(QMudNativePluginRegistry::luaAudioPluginId(), false));
			QVERIFY(nativeShimRuntime.worldFileModified());
		}

		static void reloadPreservesRequestedDisabledStateWhileInstallIsPending()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));
			const QString pluginId   = QStringLiteral("f00112233445566778899aab");
			const QString pluginPath = QDir(pluginsDir).filePath(QStringLiteral("pending_reload.xml"));
			QVERIFY(writeTextFile(pluginPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin name="PendingReload" id="%1" language="lua" enabled="n" save_state="n">
    <script><![CDATA[
function OnPluginInstall()
  SetVariable("install_completed", "yes")
end
]]></script>
  </plugin>
</muclient>
)xml")
			                                      .arg(pluginId)));

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(pluginsDir);
			runtime.setPluginInstallDeferred(true);
			QString error;
			QVERIFY2(runtime.loadPluginFile(pluginPath, &error, true), qPrintable(error));
			const WorldRuntime::Plugin *plugin = runtime.pluginForId(pluginId);
			QVERIFY(plugin);
			QVERIFY(plugin->installPending);
			QVERIFY(plugin->enabled);
			QVERIFY(plugin->disableAfterInstall);
			QCOMPARE(plugin->attributes.value(QStringLiteral("enabled")), QStringLiteral("n"));

			QCOMPARE(runtime.reloadPlugin(pluginId, &error), eOK);
			QVERIFY2(error.isEmpty(), qPrintable(error));
			plugin = runtime.pluginForId(pluginId);
			QVERIFY(plugin);
			QVERIFY(plugin->installPending);
			QVERIFY(plugin->enabled);
			QVERIFY(plugin->disableAfterInstall);
			QCOMPARE(plugin->attributes.value(QStringLiteral("enabled")), QStringLiteral("0"));

			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());
			runtime.setPluginInstallDeferred(false);
			QTRY_VERIFY_WITH_TIMEOUT(
			    runtime.pluginForId(pluginId) && !runtime.pluginForId(pluginId)->installPending, 5000);
			plugin = runtime.pluginForId(pluginId);
			QVERIFY(plugin);
			QVERIFY(!plugin->enabled);
			QVERIFY(!plugin->disableAfterInstall);
			QCOMPARE(plugin->attributes.value(QStringLiteral("enabled")), QStringLiteral("0"));
			QCOMPARE(pluginVariable(runtime, pluginId, QStringLiteral("install_completed")),
			         QStringLiteral("yes"));
		}

		static void installCompletionDoesNotConsumeSameIdReplacementInstall()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));
			const QString targetId    = QStringLiteral("f10112233445566778899aab");
			const QString updaterId   = QStringLiteral("f20112233445566778899aab");
			const QString targetPath  = QDir(pluginsDir).filePath(QStringLiteral("install_target.xml"));
			const QString updaterPath = QDir(pluginsDir).filePath(QStringLiteral("install_updater.xml"));

			QVERIFY(writeTextFile(updaterPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin name="InstallUpdater" id="%1" language="lua" enabled="y" save_state="n">
    <script><![CDATA[
function replace_once(id)
  local calls = (tonumber(GetVariable("calls")) or 0) + 1
  SetVariable("calls", tostring(calls))
  if calls == 1 then
    return ReloadPlugin(id)
  end
  return 0
end
]]></script>
  </plugin>
</muclient>
)xml")
			                                       .arg(updaterId)));
			QVERIFY(writeTextFile(targetPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin name="InstallTarget" id="%1" language="lua" enabled="y" save_state="n">
    <script><![CDATA[
function OnPluginInstall()
  CallPlugin("%2", "replace_once", "%1")
end
]]></script>
  </plugin>
</muclient>
)xml")
			                                      .arg(targetId, updaterId)));

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(pluginsDir);
			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			QString error;
			QVERIFY2(runtime.loadPluginFile(updaterPath, &error, false), qPrintable(error));
			QTRY_VERIFY_WITH_TIMEOUT(
			    runtime.pluginForId(updaterId) && !runtime.pluginForId(updaterId)->installPending, 5000);
			QVERIFY2(runtime.loadPluginFile(targetPath, &error, false), qPrintable(error));
			QTRY_COMPARE_WITH_TIMEOUT(pluginVariable(runtime, updaterId, QStringLiteral("calls")),
			                          QStringLiteral("2"), 5000);

			const WorldRuntime::Plugin *target = runtime.pluginForId(targetId);
			QVERIFY(target);
			QVERIFY(!target->installPending);
			QVERIFY(target->enabled);
			QVERIFY(!target->disableAfterInstall);
		}

		static void directCallRejectsInstallPendingPlugin()
		{
			WorldRuntime  runtime;
			const QString pluginId = QStringLiteral("f50112233445566778899aab");
			addDirectCallbackPlugin(runtime, pluginId, QStringLiteral("Pending direct target"),
			                        QStringLiteral(R"lua(
function ping(value)
  SetVariable("ping_value", value)
end
)lua"));
			WorldRuntime::Plugin *plugin = WorldRuntimeTestAccess::plugin(runtime, pluginId);
			QVERIFY(plugin);
			runtime.setPluginInstallPending(*plugin, true);
			QCOMPARE(runtime.callPlugin(pluginId, QStringLiteral("ping"), QStringLiteral("blocked")),
			         ePluginDisabled);
			QCOMPARE(pluginVariable(runtime, pluginId, QStringLiteral("ping_value")), QString());

			runtime.setPluginInstallPending(*plugin, false);
			QCOMPARE(runtime.callPlugin(pluginId, QStringLiteral("ping"), QStringLiteral("delivered")), eOK);
			QCOMPARE(pluginVariable(runtime, pluginId, QStringLiteral("ping_value")),
			         QStringLiteral("delivered"));
		}

		static void asyncResultDoesNotCrossSameIdEngineReplacement()
		{
			WorldRuntime  runtime;
			const QString pluginId  = QStringLiteral("f60112233445566778899aab");
			const auto    oldEngine = addDirectCallbackPlugin(
			    runtime, pluginId, QStringLiteral("Async owner"),
			    QStringLiteral("function OnPluginAsyncResult() SetVariable('async_payload', 'old') end"));
			QVERIFY(oldEngine);

			auto replacement = QSharedPointer<LuaCallbackEngine>::create();
			replacement->setWorldRuntime(&runtime);
			replacement->setPluginInfo(pluginId, QStringLiteral("Async replacement"), QString());
			replacement->setScriptText(QStringLiteral(R"lua(
function OnPluginAsyncResult(request_id, api_name, status, payload)
  SetVariable("async_payload", payload)
end
)lua"));
			QVERIFY(replacement->loadScript());
			WorldRuntime::Plugin *plugin = WorldRuntimeTestAccess::plugin(runtime, pluginId);
			QVERIFY(plugin);
			plugin->lua = replacement;

			runtime.dispatchPluginAsyncResult({pluginId, oldEngine->instanceId(), 1},
			                                  QStringLiteral("replacement-test"), true, eOK,
			                                  QStringLiteral("stale"));
			QCOMPARE(pluginVariable(runtime, pluginId, QStringLiteral("async_payload")), QString());

			runtime.dispatchPluginAsyncResult({pluginId, replacement->instanceId(), 2},
			                                  QStringLiteral("replacement-test"), true, eOK,
			                                  QStringLiteral("current"));
			QTRY_COMPARE_WITH_TIMEOUT(pluginVariable(runtime, pluginId, QStringLiteral("async_payload")),
			                          QStringLiteral("current"), 5000);
		}

		static void disableCompletionDoesNotConsumeSameIdReplacementInstall()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));
			const QString targetId    = QStringLiteral("f30112233445566778899aab");
			const QString updaterId   = QStringLiteral("f40112233445566778899aab");
			const QString targetPath  = QDir(pluginsDir).filePath(QStringLiteral("disable_target.xml"));
			const QString updaterPath = QDir(pluginsDir).filePath(QStringLiteral("disable_updater.xml"));

			QVERIFY(writeTextFile(updaterPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin name="DisableUpdater" id="%1" language="lua" enabled="y" save_state="n">
    <script><![CDATA[
function replace_once(id)
  local calls = (tonumber(GetVariable("calls")) or 0) + 1
  SetVariable("calls", tostring(calls))
  if calls == 1 then
    return ReloadPlugin(id)
  end
  return 0
end
]]></script>
  </plugin>
</muclient>
)xml")
			                                       .arg(updaterId)));
			QVERIFY(writeTextFile(targetPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin name="DisableTarget" id="%1" language="lua" enabled="n" save_state="n">
    <script><![CDATA[
function OnPluginDisable()
  CallPlugin("%2", "replace_once", "%1")
end
]]></script>
  </plugin>
</muclient>
)xml")
			                                      .arg(targetId, updaterId)));

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(pluginsDir);
			RuntimeCommandHarness harness(runtime);
			QVERIFY(harness.showAndWait());

			QString error;
			QVERIFY2(runtime.loadPluginFile(updaterPath, &error, false), qPrintable(error));
			QTRY_VERIFY_WITH_TIMEOUT(
			    runtime.pluginForId(updaterId) && !runtime.pluginForId(updaterId)->installPending, 5000);
			QVERIFY2(runtime.loadPluginFile(targetPath, &error, false), qPrintable(error));
			QTRY_COMPARE_WITH_TIMEOUT(pluginVariable(runtime, updaterId, QStringLiteral("calls")),
			                          QStringLiteral("2"), 5000);

			const WorldRuntime::Plugin *target = runtime.pluginForId(targetId);
			QVERIFY(target);
			QVERIFY(!target->installPending);
			QVERIFY(!target->enabled);
			QVERIFY(!target->disableAfterInstall);
			QCOMPARE(target->attributes.value(QStringLiteral("enabled")), QStringLiteral("0"));
		}

		static void globalPluginInstallRejectionPersistsDisabledState()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));

			const QString globalId   = QStringLiteral("abababababababababababa8");
			const QString pluginPath = QDir(pluginsDir).filePath(QStringLiteral("global_rejected.xml"));
			QVERIFY(writeTextFile(pluginPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin name="GlobalRejected" id="%1" language="lua" enabled="y" save_state="n"
          sequence="100">
    <script><![CDATA[
function OnPluginInstall()
  return false
end
]]></script>
  </plugin>
</muclient>
)xml")
			                                      .arg(globalId)));

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(pluginsDir);
			WorldView view;
			view.resize(640, 480);
			view.setRuntime(&runtime);
			runtime.setWorldFileModified(false);
			QString error;
			QVERIFY2(runtime.loadPluginFile(pluginPath, &error, true), qPrintable(error));
			QTRY_VERIFY_WITH_TIMEOUT(
			    runtime.pluginForId(globalId) && !runtime.pluginForId(globalId)->installPending, 5000);
			const WorldRuntime::Plugin *plugin = runtime.pluginForId(globalId);
			QVERIFY(plugin);
			QVERIFY(plugin->global);
			QVERIFY(!plugin->enabled);
			QVERIFY(runtime.worldFileModified());
			const QString worldPath = QDir(tempDir.path()).filePath(QStringLiteral("global-rejected.qdl"));
			QVERIFY2(runtime.saveWorldFile(worldPath, &error), qPrintable(error));
			WorldDocument savedDocument;
			QVERIFY2(savedDocument.loadFromFile(worldPath), qPrintable(savedDocument.errorString()));
			QVERIFY2(savedDocument.expandIncludes(worldPath, tempDir.path(), tempDir.path(), QString()),
			         qPrintable(savedDocument.errorString()));
			QCOMPARE(savedDocument.plugins().size(), 1);
			QCOMPARE(savedDocument.plugins().constFirst().attributes.value(QStringLiteral("id")), globalId);
			QCOMPARE(savedDocument.plugins().constFirst().attributes.value(QStringLiteral("enabled")),
			         QStringLiteral("n"));
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
			QVERIFY(runtime.pluginIdList().contains(mushReaderId));
			QCOMPARE(runtime.pluginInfo(mushReaderId, 17).toBool(), false);
			QCOMPARE(runtime.pluginSupports(mushReaderId, QStringLiteral("say")), eOK);

			WorldRuntime absentMushReaderRuntime;
			QVERIFY(!absentMushReaderRuntime.isPluginInstalled(mushReaderId));
			QVERIFY(!absentMushReaderRuntime.pluginIdList().contains(mushReaderId));
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
			QVERIFY(absentRuntime.pluginIdList().contains(audioId));
			QCOMPARE(absentRuntime.pluginInfo(audioId, 17).toBool(), true);
			QCOMPARE(absentRuntime.pluginSupports(audioId, QStringLiteral("plugin_update_url")), eOK);
			QCOMPARE(absentRuntime.callPlugin(audioId, QStringLiteral("plugin_update_url"), QString()), eOK);

			WorldRuntime::Plugin audioPlugin;
			audioPlugin.enabled    = false;
			audioPlugin.nativeShim = true;
			audioPlugin.attributes.insert(QStringLiteral("id"), audioId);
			audioPlugin.attributes.insert(QStringLiteral("name"), QStringLiteral("LuaAudio"));

			WorldRuntime runtime;
			WorldRuntimeTestAccess::plugins(runtime).push_back(audioPlugin);
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

		static void pluginInstallDrawNotificationUsesPresentedViewport()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("worlds/plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));
			const QString pluginPath = QDir(pluginsDir).filePath(QStringLiteral("install_draw.xml"));
			QVERIFY(writeTextFile(pluginPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin
    name="InstallDraw"
    author="QMud Test"
    id="bbccddeeff00112233445566"
    language="lua"
    enabled="y"
    save_state="n"
    sequence="100">
    <script><![CDATA[
function OnPluginDrawOutputWindow(first_line, offset)
  local calls = tonumber(GetVariable("draw_calls") or "0") or 0
  SetVariable("draw_calls", tostring(calls + 1))
  SetVariable("draw_first_line", tostring(first_line))
  SetVariable("draw_offset", tostring(offset))
  local started = os.clock()
  while os.clock() - started < 0.25 do end
end
]]></script>
  </plugin>
</muclient>
)xml")));

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
			for (int line = 0; line < 300; ++line)
				runtime.addLine(QStringLiteral("install-draw-%1").arg(line), WorldRuntime::LineOutput, true);

			WorldView view;
			view.resize(640, 480);
			view.setRuntime(&runtime);
			view.rebuildOutputFromLines(runtime.lines());
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));
			QTRY_COMPARE(view.setOutputScroll(100, true), eOK);

			WorldView observer;
			observer.resize(640, 480);
			observer.setPassiveBufferView(true);
			observer.setRuntimeObserver(&runtime);
			observer.restoreOutputFromPersistedLines(runtime.lines());
			observer.show();
			QVERIFY(QTest::qWaitForWindowExposed(&observer));
			QTRY_COMPARE(observer.setOutputScroll(40, true), eOK);

			QString loadError;
			QVERIFY2(runtime.loadPluginFile(QStringLiteral("install_draw.xml"), &loadError),
			         qPrintable(loadError));
			QTRY_VERIFY_WITH_TIMEOUT(runtime.drawOutputWindowCallbackActive(), 5000);
			QVERIFY(view.m_drawOutputWindowCallbackActive);
			QVERIFY(!observer.m_drawOutputWindowCallbackActive);
			QVERIFY(view.m_pendingSemanticOutputRepaint != WorldView::SemanticOutputRepaint::None);
			QCOMPARE(observer.m_pendingSemanticOutputRepaint, WorldView::SemanticOutputRepaint::None);
			QVERIFY(view.m_drawCallbackCompletionsPending > 0);
			QCOMPARE(observer.m_drawCallbackCompletionsPending, 0);
			QTRY_COMPARE_WITH_TIMEOUT(
			    pluginVariable(runtime, kInstallDrawPluginId, QStringLiteral("draw_offset")),
			    QStringLiteral("100"), 5000);
			QVERIFY(pluginVariable(runtime, kInstallDrawPluginId, QStringLiteral("draw_first_line")).toInt() >
			        1);
			QVERIFY(pluginVariable(runtime, kInstallDrawPluginId, QStringLiteral("draw_calls")).toInt() >= 1);
			QTRY_VERIFY_WITH_TIMEOUT(!runtime.drawOutputWindowCallbackActive(), 5000);
			QTRY_COMPARE_WITH_TIMEOUT(view.m_pendingSemanticOutputRepaint,
			                          WorldView::SemanticOutputRepaint::None, 5000);
			QTRY_COMPARE_WITH_TIMEOUT(observer.m_pendingSemanticOutputRepaint,
			                          WorldView::SemanticOutputRepaint::None, 5000);
		}

		static void sharedRuntimeOutputHasOneOwnerAndUpdatesEveryPresentation()
		{
			WorldRuntime     runtime;
			WorldChildWindow primary(QStringLiteral("Primary"));
			WorldChildWindow observer(QStringLiteral("Observer"));
			primary.setRuntime(&runtime);
			observer.setRuntimeObserver(&runtime);

			QCOMPARE(runtime.presentationViews().size(), 2);
			QCOMPARE(runtime.view(), primary.view());

			runtime.outputText(QStringLiteral("single-runtime-write"), false, true);
			QTRY_COMPARE_WITH_TIMEOUT(runtime.lines().size(), 1, 2000);
			QTRY_COMPARE_WITH_TIMEOUT(
			    primary.view()->outputLines().count(QStringLiteral("single-runtime-write")), 1, 2000);
			QTRY_COMPARE_WITH_TIMEOUT(
			    observer.view()->outputLines().count(QStringLiteral("single-runtime-write")), 1, 2000);

			observer.view()->appendNoteText(QStringLiteral("observer-originated-write"), true);
			QTest::qWait(100);
			QCOMPARE(runtime.lines().size(), 1);
			QVERIFY(!primary.view()->outputLines().contains(QStringLiteral("observer-originated-write")));
			QVERIFY(!observer.view()->outputLines().contains(QStringLiteral("observer-originated-write")));

			WorldRuntime::LineEntry replacement;
			replacement.text       = QStringLiteral("replacement-buffer-line");
			replacement.flags      = WorldRuntime::LineOutput;
			replacement.hardReturn = true;
			replacement.lineNumber = 91;
			runtime.replaceOutputLines(QVector<WorldRuntime::LineEntry>{replacement});
			QTRY_COMPARE_WITH_TIMEOUT(primary.view()->outputLines(),
			                          QStringList{QStringLiteral("replacement-buffer-line")}, 2000);
			QTRY_COMPARE_WITH_TIMEOUT(observer.view()->outputLines(),
			                          QStringList{QStringLiteral("replacement-buffer-line")}, 2000);

			runtime.deleteOutput();
			QTRY_VERIFY_WITH_TIMEOUT(runtime.lines().isEmpty(), 2000);
			QTRY_VERIFY_WITH_TIMEOUT(primary.view()->outputLines().isEmpty(), 2000);
			QTRY_VERIFY_WITH_TIMEOUT(observer.view()->outputLines().isEmpty(), 2000);

			primary.setRuntime(nullptr);
			QCOMPARE(runtime.presentationViews().size(), 1);
			QVERIFY(!runtime.view());

			observer.setRuntimeObserver(nullptr);
			QVERIFY(runtime.presentationViews().isEmpty());
			QVERIFY(!runtime.view());
		}

		static void pluginInstallWithoutPresentationDoesNotSynthesizeDrawViewport()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());

			const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("worlds/plugins"));
			QVERIFY(QDir().mkpath(pluginsDir));
			const QString pluginPath = QDir(pluginsDir).filePath(QStringLiteral("detached_install_draw.xml"));
			QVERIFY(writeTextFile(pluginPath, QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin
    name="DetachedInstallDraw"
    author="QMud Test"
    id="ccddeeff0011223344556677"
    language="lua"
    enabled="y"
    save_state="n"
    sequence="100">
    <script><![CDATA[
function OnPluginInstall()
  local started = os.clock()
  while os.clock() - started < 0.25 do end
end
function OnPluginDrawOutputWindow(first_line, offset)
  SetVariable("draw_position", string.format("%.0f,%.0f", first_line, offset))
end
]]></script>
  </plugin>
</muclient>
)xml")));

			WorldRuntime runtime;
			runtime.setStartupDirectory(tempDir.path());
			runtime.setPluginsDirectory(QStringLiteral("worlds/plugins"));

			WorldView view;
			view.resize(640, 480);
			view.setRuntime(&runtime);
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			QString loadError;
			QVERIFY2(runtime.loadPluginFile(QStringLiteral("detached_install_draw.xml"), &loadError),
			         qPrintable(loadError));
			view.setRuntime(nullptr);
			QVERIFY(runtime.presentationViews().isEmpty());
			QTRY_VERIFY_WITH_TIMEOUT(
			    !runtime.plugins().isEmpty() && !runtime.plugins().constFirst().installPending, 5000);
			QCOMPARE(pluginVariable(runtime, QStringLiteral("ccddeeff0011223344556677"),
			                        QStringLiteral("draw_position")),
			         QString());
		}
};

QTEST_MAIN(tst_WorldRuntime_PluginLifecycle)

#if __has_include("tst_WorldRuntime_PluginLifecycle.moc")
#include "tst_WorldRuntime_PluginLifecycle.moc"
#endif
