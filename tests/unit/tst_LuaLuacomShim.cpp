/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_LuaLuacomShim.cpp
 * Role: Unit coverage for luacom compatibility shim used by Mushclient SAPI plugins.
 */

#include "LuaLuacomShim.h"
#include "TtsEngine.h"

#include <QtTest/QTest>
#include <utility>

namespace
{
	class FakeSynthesizeRuntime final : public QMudTtsEngine::TtsEngine
	{
		public:
			bool               hasSpeechFlag{true};
			bool               waitForIdleResult{true};
			bool               synthesizeResult{true};
			bool               emitAudioChunk{true};
			QByteArray         synthesizedChunk{QByteArrayLiteral("fake-audio")};
			QStringList        synthesizedUtterances;
			QList<bool>        synthesizePurgeFlags;
			QList<int>         waitTimeouts;

			[[nodiscard]] bool hasSpeech() const override
			{
				return hasSpeechFlag;
			}

			bool waitForSpeechIdle(int timeoutMs) override
			{
				waitTimeouts.push_back(timeoutMs);
				return waitForIdleResult;
			}

			bool synthesizeToStream(const QString &utterance, bool purgeBeforeSpeak,
			                        const std::function<void(QByteArray)> &onChunk) override
			{
				synthesizedUtterances.push_back(utterance);
				synthesizePurgeFlags.push_back(purgeBeforeSpeak);
				if (emitAudioChunk && onChunk)
					onChunk(synthesizedChunk);
				return synthesizeResult;
			}
	};

	struct RuntimeFactoryGuard
	{
			explicit RuntimeFactoryGuard(QMudLuacomShim::RuntimeFactory factory)
			{
				QMudLuacomShim::setRuntimeFactoryForTesting(std::move(factory));
			}

			~RuntimeFactoryGuard()
			{
				QMudLuacomShim::resetRuntimeFactoryForTesting();
			}
	};

	struct LuaStateGuard
	{
			lua_State *state{nullptr};

			LuaStateGuard()
			{
				state = luaL_newstate();
				if (state)
					luaL_openlibs(state);
			}

			~LuaStateGuard()
			{
				if (state)
					lua_close(state);
			}
	};

	struct LuaStringEvalResult
	{
			bool    ok{false};
			QString value;
			QString error;
	};

	LuaStringEvalResult evaluateLuaToString(lua_State *L, const QByteArray &chunk)
	{
		LuaStringEvalResult result;
		if (!L)
		{
			result.error = QStringLiteral("Lua state is null.");
			return result;
		}
		if (luaL_loadbuffer(L, chunk.constData(), static_cast<size_t>(chunk.size()), "tst_LuaLuacomShim") !=
		    0)
		{
			const char *err = lua_tostring(L, -1);
			result.error    = QString::fromUtf8(err ? err : "unknown load error");
			lua_pop(L, 1);
			return result;
		}
		if (lua_pcall(L, 0, 1, 0) != 0)
		{
			const char *err = lua_tostring(L, -1);
			result.error    = QString::fromUtf8(err ? err : "unknown runtime error");
			lua_pop(L, 1);
			return result;
		}
		if (!lua_isstring(L, -1))
		{
			result.error = QStringLiteral("Lua chunk did not return a string result.");
			lua_pop(L, 1);
			return result;
		}

		size_t      len   = 0;
		const char *bytes = lua_tolstring(L, -1, &len);
		result.ok         = true;
		result.value      = QString::fromUtf8(bytes ? bytes : "", static_cast<int>(len));
		lua_pop(L, 1);
		return result;
	}

	LuaStateGuard makeLuacomState()
	{
		LuaStateGuard state;
		if (!state.state)
			return state;
		QMudLuacomShim::registerPreload(state.state);
		return state;
	}
} // namespace

class tst_LuaLuacomShim : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void requireRegistersLuacomAndComAliases()
		{
			LuaStateGuard state = makeLuacomState();
			QVERIFY(state.state);

			const auto result =
			    evaluateLuaToString(state.state, QByteArrayLiteral("local luacom = require(\"luacom\")\n"
			                                                       "local com = require(\"com\")\n"
			                                                       "return table.concat({\n"
			                                                       "  type(luacom.CreateObject),\n"
			                                                       "  type(luacom.GetTypeInfo),\n"
			                                                       "  type(luacom.GetEnumerator),\n"
			                                                       "  type(com.CreateObject),\n"
			                                                       "  type(com.GetTypeInfo),\n"
			                                                       "  type(_G.luacom)\n"
			                                                       "}, \":\")"));
			QVERIFY2(result.ok, qPrintable(result.error));
			QCOMPARE(result.value, QStringLiteral("function:function:function:function:function:table"));
		}

		void createObjectRejectsUnsupportedProgramIds()
		{
			LuaStateGuard state = makeLuacomState();
			QVERIFY(state.state);

			const auto result = evaluateLuaToString(
			    state.state, QByteArrayLiteral("local luacom = require(\"luacom\")\n"
			                                   "return tostring(luacom.CreateObject(\"Foo.Bar\") "
			                                   "== nil)"));
			QVERIFY2(result.ok, qPrintable(result.error));
			QCOMPARE(result.value, QStringLiteral("true"));
		}

		void typeInfoExportsMushclientSpeakFlagConstants()
		{
			LuaStateGuard state = makeLuacomState();
			QVERIFY(state.state);

			const auto result = evaluateLuaToString(
			    state.state, QByteArrayLiteral("local luacom = require(\"luacom\")\n"
			                                   "local voice = luacom.CreateObject(\"SAPI.SpVoice\")\n"
			                                   "if not voice then return \"NO_VOICE\" end\n"
			                                   "local typeInfo = luacom.GetTypeInfo(voice)\n"
			                                   "local typeLib = typeInfo:GetTypeLib()\n"
			                                   "local enums = typeLib:ExportEnumerations()\n"
			                                   "local flags = enums.SpeechVoiceSpeakFlags\n"
			                                   "local runState = enums.SpeechRunState\n"
			                                   "local priorities = enums.SpeechVoicePriority\n"
			                                   "local events = enums.SpeechVoiceEvents\n"
			                                   "return table.concat({\n"
			                                   "  type(typeInfo),\n"
			                                   "  type(typeLib),\n"
			                                   "  type(enums),\n"
			                                   "  type(flags),\n"
			                                   "  tostring(flags.SVSFlagsAsync),\n"
			                                   "  tostring(flags.SVSFPurgeBeforeSpeak),\n"
			                                   "  tostring(flags.SVSFNLPSpeakPunc),\n"
			                                   "  tostring(flags.SVSFDefault),\n"
			                                   "  tostring(flags.SVSFIsXML),\n"
			                                   "  type(runState),\n"
			                                   "  tostring(runState.SRSEDone),\n"
			                                   "  tostring(runState.SRSEIsSpeaking),\n"
			                                   "  type(priorities),\n"
			                                   "  tostring(priorities.SVPNormal),\n"
			                                   "  tostring(priorities.SVPAlert),\n"
			                                   "  tostring(priorities.SVPOver),\n"
			                                   "  type(events),\n"
			                                   "  tostring(events.SVEAllEvents)\n"
			                                   "}, \":\")"));
			QVERIFY2(result.ok, qPrintable(result.error));
			if (result.value == QStringLiteral("NO_VOICE"))
			{
				QSKIP("No Qt TextToSpeech engine available for SAPI.SpVoice creation in this environment.");
			}
			QCOMPARE(
			    result.value,
			    QStringLiteral("userdata:userdata:table:table:1:2:64:0:8:table:1:2:table:0:1:2:table:33790"));
		}

		void spVoiceSurfaceMatchesLuacomCompatibilityContracts()
		{
			LuaStateGuard state = makeLuacomState();
			QVERIFY(state.state);

			const auto result = evaluateLuaToString(
			    state.state,
			    QByteArrayLiteral("local luacom = require(\"luacom\")\n"
			                      "local voice = luacom.CreateObject(\"SAPI.SpVoice\")\n"
			                      "if not voice then return \"NO_VOICE\" end\n"
			                      "local speakResult = voice:Speak(\"coverage\", 0)\n"
			                      "local skipResult = voice:Skip(\"Sentence\", 1)\n"
			                      "local voices = voice:GetVoices()\n"
			                      "local count = voices.Count\n"
			                      "local outputs = voice:GetAudioOutputs()\n"
			                      "local outputCount = outputs.Count\n"
			                      "local filteredCount = voice:GetVoices(\"Name=__qmud_no_voice__\").Count\n"
			                      "local filteredOutputCount = "
			                      "voice:GetAudioOutputs(\"Name=__qmud_no_audio_output__\").Count\n"
			                      "local missingVoiceCount = select('#', voices:Item(-1))\n"
			                      "local missingVoiceIsNil = voices:Item(-1) == nil\n"
			                      "local missingOutputCount = select('#', outputs:Item(-1))\n"
			                      "local missingOutputIsNil = outputs:Item(-1) == nil\n"
			                      "local enumerator = luacom.GetEnumerator(voices)\n"
			                      "local outputEnumerator = luacom.GetEnumerator(outputs)\n"
			                      "local seen = 0\n"
			                      "local seenOutputs = 0\n"
			                      "while true do\n"
			                      "  local item = enumerator:Next()\n"
			                      "  if not item then break end\n"
			                      "  seen = seen + 1\n"
			                      "end\n"
			                      "while true do\n"
			                      "  local output = outputEnumerator:Next()\n"
			                      "  if not output then break end\n"
			                      "  seenOutputs = seenOutputs + 1\n"
			                      "end\n"
			                      "local firstOk = true\n"
			                      "local idOk = true\n"
			                      "local descriptionOk = true\n"
			                      "local outputOk = true\n"
			                      "if count > 0 then\n"
			                      "  local first = voices:Item(0)\n"
			                      "  firstOk = first ~= nil\n"
			                      "  idOk = first ~= nil and type(first.ID) == \"string\" and "
			                      "#first.ID > 0\n"
			                      "  descriptionOk = first ~= nil and type(first:GetDescription()) "
			                      "== \"string\" and #first:GetDescription() > 0\n"
			                      "  voice:setVoice(first)\n"
			                      "else\n"
			                      "  firstOk = voices:Item(0) == nil\n"
			                      "end\n"
			                      "if outputCount > 0 then\n"
			                      "  local output = outputs:Item(0)\n"
			                      "  outputOk = output ~= nil and type(output.ID) == \"string\" and "
			                      "#output.ID > 0 and type(output:GetDescription()) == \"string\"\n"
			                      "  voice:setAudioOutput(output)\n"
			                      "  voice.AudioOutput = output\n"
			                      "end\n"
			                      "local setVoiceNilOk = pcall(function() voice:setVoice(nil) "
			                      "end)\n"
			                      "local setVoiceBadOk, setVoiceBadErr = pcall(function() "
			                      "voice:setVoice(123) end)\n"
			                      "local rateType = type(voice.Rate)\n"
			                      "voice.Rate = 3\n"
			                      "local rateAfterType = type(voice.Rate)\n"
			                      "local volumeType = type(voice.Volume)\n"
			                      "voice.Volume = 77\n"
			                      "local volumeAfterType = type(voice.Volume)\n"
			                      "local statusType = type(voice.Status)\n"
			                      "local runningStateType = type(voice.Status.RunningState)\n"
			                      "local pauseOk = pcall(function() voice:Pause() end)\n"
			                      "local resumeOk = pcall(function() voice:Resume() end)\n"
			                      "local waitType = type(voice:WaitUntilDone(0))\n"
			                      "return table.concat({\n"
			                      "  type(speakResult),\n"
			                      "  type(skipResult),\n"
			                      "  tostring(skipResult >= 0),\n"
			                      "  type(count),\n"
			                      "  type(outputCount),\n"
			                      "  tostring(filteredCount >= 0 and filteredCount <= count),\n"
			                      "  tostring(filteredOutputCount == 0),\n"
			                      "  tostring(missingVoiceCount == 1),\n"
			                      "  tostring(missingVoiceIsNil),\n"
			                      "  tostring(missingOutputCount == 1),\n"
			                      "  tostring(missingOutputIsNil),\n"
			                      "  tostring(seen == count),\n"
			                      "  tostring(seenOutputs == outputCount),\n"
			                      "  tostring(firstOk),\n"
			                      "  tostring(idOk),\n"
			                      "  tostring(descriptionOk),\n"
			                      "  tostring(outputOk),\n"
			                      "  tostring(setVoiceNilOk),\n"
			                      "  tostring(setVoiceBadOk),\n"
			                      "  tostring(type(setVoiceBadErr)),\n"
			                      "  rateType,\n"
			                      "  rateAfterType,\n"
			                      "  volumeType,\n"
			                      "  volumeAfterType,\n"
			                      "  statusType,\n"
			                      "  runningStateType,\n"
			                      "  tostring(pauseOk),\n"
			                      "  tostring(resumeOk),\n"
			                      "  waitType,\n"
			                      "  type(voice.SpeakCompleteEvent),\n"
			                      "  type(voice.Priority),\n"
			                      "  type(voice.EventInterests),\n"
			                      "  type(voice.AlertBoundary),\n"
			                      "  type(voice.SynchronousSpeakTimeout),\n"
			                      "  type(voice.AllowAudioOutputFormatChangesOnNextSet)\n"
			                      "}, \":\")"));
			QVERIFY2(result.ok, qPrintable(result.error));
			if (result.value == QStringLiteral("NO_VOICE"))
			{
				QSKIP("No Qt TextToSpeech engine available for SAPI.SpVoice creation in this environment.");
			}
			QCOMPARE(result.value,
			         QStringLiteral(
			             "number:number:true:number:number:true:true:true:true:true:true:true:true:true:true:"
			             "true:true:true:false:"
			             "string:number:"
			             "number:number:number:userdata:number:true:true:boolean:number:number:number:number:"
			             "number:"
			             "boolean"));
		}

		void sapiAuxiliaryObjectFamiliesAreUsable()
		{
			LuaStateGuard state = makeLuacomState();
			QVERIFY(state.state);

			const auto result = evaluateLuaToString(
			    state.state,
			    QByteArrayLiteral("local luacom = require(\"luacom\")\n"
			                      "local category = assert(luacom.CreateObject(\"SAPI."
			                      "SpObjectTokenCategory\"))\n"
			                      "category:SetId(\"HKEY_LOCAL_MACHINE\\\\SOFTWARE\\\\Microsoft\\\\"
			                      "Speech\\\\Voices\", false)\n"
			                      "local tokens = category:EnumerateTokens(nil, nil)\n"
			                      "local tokenCount = tokens.Count\n"
			                      "category:SetId(\"HKEY_LOCAL_MACHINE\\\\SOFTWARE\\\\Microsoft\\\\"
			                      "Speech\\\\AudioInput\", false)\n"
			                      "local inputTokenCount = category:EnumerateTokens(nil, nil).Count\n"
			                      "local missingTokenCount = select('#', tokens:Item(-1))\n"
			                      "local missingTokenIsNil = tokens:Item(-1) == nil\n"
			                      "local enum = luacom.GetEnumerator(tokens)\n"
			                      "local enumSeen = 0\n"
			                      "while true do\n"
			                      "  local tk = enum:Next()\n"
			                      "  if not tk then break end\n"
			                      "  enumSeen = enumSeen + 1\n"
			                      "end\n"
			                      "local defaultId = category:GetDefaultTokenId() or \"\"\n"
			                      "local tokenObj = assert(luacom.CreateObject(\"SAPI."
			                      "SpObjectToken\"))\n"
			                      "tokenObj:SetId(\"QMUD\\\\TOKEN\\\\X\", "
			                      "\"HKEY_LOCAL_MACHINE\\\\SOFTWARE\\\\Microsoft\\\\Speech\\\\"
			                      "Voices\", false)\n"
			                      "local lexicon = assert(luacom.CreateObject(\"SAPI.SpLexicon\"))\n"
			                      "lexicon:AddPronunciation(\"qmud\", 1033, 0, \"cue-mud\")\n"
			                      "local pron = lexicon:GetPronunciations(\"qmud\", 1033, 0)\n"
			                      "local audioFormat = assert(luacom.CreateObject(\"SAPI."
			                      "SpAudioFormat\"))\n"
			                      "audioFormat.Type = 18\n"
			                      "local stream = assert(luacom.CreateObject(\"SAPI.SpFileStream\"))\n"
			                      "stream.Format = audioFormat\n"
			                      "local path = \"__qmud_luacom_stream_test.txt\"\n"
			                      "stream:Open(path, 3, false)\n"
			                      "local isOpenType = type(stream.IsOpen)\n"
			                      "local writeType = type(stream:Write(\"hello\"))\n"
			                      "stream:Seek(0)\n"
			                      "local readType = type(stream:Read())\n"
			                      "stream:Close()\n"
			                      "os.remove(path)\n"
			                      "local voice = luacom.CreateObject(\"SAPI.SpVoice\")\n"
			                      "local voiceSetTokenOk = true\n"
			                      "local voiceAssignTokenOk = true\n"
			                      "if voice and tokenCount > 0 then\n"
			                      "  local tk = tokens:Item(0)\n"
			                      "  voiceSetTokenOk = pcall(function() voice:setVoice(tk) end)\n"
			                      "  voiceAssignTokenOk = pcall(function() voice.Voice = tk end)\n"
			                      "  voice.AudioOutputStream = stream\n"
			                      "  voice:setAudioOutputStream(stream)\n"
			                      "end\n"
			                      "return table.concat({\n"
			                      "  type(category),\n"
			                      "  type(tokens),\n"
			                      "  type(tokenCount),\n"
			                      "  tostring(inputTokenCount == 0),\n"
			                      "  tostring(missingTokenCount == 1),\n"
			                      "  tostring(missingTokenIsNil),\n"
			                      "  tostring(tokenCount >= 0),\n"
			                      "  tostring(enumSeen == tokenCount),\n"
			                      "  type(defaultId),\n"
			                      "  type(tokenObj.Id),\n"
			                      "  type(tokenObj:GetDescription()),\n"
			                      "  type(tokenObj:GetAttribute(\"Name\")),\n"
			                      "  type(pron),\n"
			                      "  type(pron[1] or \"\"),\n"
			                      "  type(stream.Format),\n"
			                      "  type(audioFormat.Type),\n"
			                      "  isOpenType,\n"
			                      "  writeType,\n"
			                      "  readType,\n"
			                      "  type(voice),\n"
			                      "  tostring(voiceSetTokenOk),\n"
			                      "  tostring(voiceAssignTokenOk)\n"
			                      "}, \":\")"));
			QVERIFY2(result.ok, qPrintable(result.error));
			const QStringList parts = result.value.split(':');
			QCOMPARE(parts.size(), 22);
			QCOMPARE(parts.at(0), QStringLiteral("userdata"));
			QCOMPARE(parts.at(1), QStringLiteral("userdata"));
			QCOMPARE(parts.at(2), QStringLiteral("number"));
			QCOMPARE(parts.at(3), QStringLiteral("true"));
			QCOMPARE(parts.at(4), QStringLiteral("true"));
			QCOMPARE(parts.at(5), QStringLiteral("true"));
			QCOMPARE(parts.at(6), QStringLiteral("true"));
			QCOMPARE(parts.at(7), QStringLiteral("true"));
			QCOMPARE(parts.at(8), QStringLiteral("string"));
			QCOMPARE(parts.at(9), QStringLiteral("string"));
			QCOMPARE(parts.at(10), QStringLiteral("string"));
			QCOMPARE(parts.at(11), QStringLiteral("string"));
			QCOMPARE(parts.at(12), QStringLiteral("table"));
			QCOMPARE(parts.at(13), QStringLiteral("string"));
			QCOMPARE(parts.at(14), QStringLiteral("userdata"));
			QCOMPARE(parts.at(15), QStringLiteral("number"));
			QCOMPARE(parts.at(16), QStringLiteral("boolean"));
			QCOMPARE(parts.at(17), QStringLiteral("number"));
			QCOMPARE(parts.at(18), QStringLiteral("string"));
			QVERIFY(parts.at(19) == QStringLiteral("userdata") || parts.at(19) == QStringLiteral("nil"));
			QCOMPARE(parts.at(20), QStringLiteral("true"));
			QCOMPARE(parts.at(21), QStringLiteral("true"));
		}

		void tokenSetIdAndStreamLifetimeParity()
		{
			LuaStateGuard state = makeLuacomState();
			QVERIFY(state.state);

			const auto result = evaluateLuaToString(
			    state.state,
			    QByteArrayLiteral("local luacom = require(\"luacom\")\n"
			                      "local voice = luacom.CreateObject(\"SAPI.SpVoice\")\n"
			                      "if not voice then return \"NO_VOICE\" end\n"
			                      "local voices = voice:GetVoices()\n"
			                      "if voices.Count == 0 then return \"NO_VOICE\" end\n"
			                      "local first = voices:Item(0)\n"
			                      "local token = assert(luacom.CreateObject(\"SAPI.SpObjectToken\"))\n"
			                      "token:SetId(first.ID, nil, false)\n"
			                      "local setVoiceOk = pcall(function() voice:setVoice(token) end)\n"
			                      "local assignVoiceOk = pcall(function() voice.Voice = token end)\n"
			                      "local hasEnglish = false\n"
			                      "for i = 0, voices.Count - 1 do\n"
			                      "  local v = voices:Item(i)\n"
			                      "  local language = string.lower(v:GetAttribute(\"Language\") or \"\")\n"
			                      "  if string.match(language, \"^en\") then\n"
			                      "    hasEnglish = true\n"
			                      "    break\n"
			                      "  end\n"
			                      "end\n"
			                      "local languageFilterOk = true\n"
			                      "if hasEnglish then\n"
			                      "  languageFilterOk = voice:GetVoices(\"Language=409\").Count > 0 and\n"
			                      "                     voice:GetVoices(\"Language=1033\").Count > 0\n"
			                      "end\n"
			                      "local stream = assert(luacom.CreateObject(\"SAPI.SpFileStream\"))\n"
			                      "local path = \"__qmud_luacom_stream_lifetime_test.bin\"\n"
			                      "stream:Open(path, 3, false)\n"
			                      "voice.AudioOutputStream = stream\n"
			                      "stream = nil\n"
			                      "collectgarbage(\"collect\")\n"
			                      "local held = voice.AudioOutputStream\n"
			                      "local isOpen = held ~= nil and held.IsOpen == true\n"
			                      "if held then\n"
			                      "  held:Write(\"x\")\n"
			                      "  held:Close()\n"
			                      "end\n"
			                      "voice.AudioOutputStream = nil\n"
			                      "os.remove(path)\n"
			                      "return table.concat({\n"
			                      "  tostring(setVoiceOk),\n"
			                      "  tostring(assignVoiceOk),\n"
			                      "  tostring(languageFilterOk),\n"
			                      "  tostring(isOpen)\n"
			                      "}, \":\")"));
			QVERIFY2(result.ok, qPrintable(result.error));
			if (result.value == QStringLiteral("NO_VOICE"))
				QSKIP("No Qt TextToSpeech engine available for SAPI.SpVoice creation in this environment.");
			QCOMPARE(result.value, QStringLiteral("true:true:true:true"));
		}

		void audioOutputFilteringParityUsesSameSubsetAcrossCollectionItemAndEnumerator()
		{
			LuaStateGuard state = makeLuacomState();
			QVERIFY(state.state);

			const auto result = evaluateLuaToString(
			    state.state,
			    QByteArrayLiteral(
			        "local luacom = require(\"luacom\")\n"
			        "local voice = luacom.CreateObject(\"SAPI.SpVoice\")\n"
			        "if not voice then return \"NO_VOICE\" end\n"
			        "local outputs = voice:GetAudioOutputs()\n"
			        "if outputs.Count == 0 then return \"NO_OUTPUT\" end\n"
			        "local first = outputs:Item(0)\n"
			        "if first == nil or type(first.ID) ~= \"string\" or #first.ID == 0 then\n"
			        "  return \"NO_FIRST\"\n"
			        "end\n"
			        "local filtered = voice:GetAudioOutputs(\"Id=\" .. first.ID)\n"
			        "local filteredCount = filtered.Count\n"
			        "local filteredFirst = filtered:Item(0)\n"
			        "local itemOk = filteredFirst ~= nil and type(filteredFirst.ID) == \"string\" "
			        "and #filteredFirst.ID > 0\n"
			        "local enum = luacom.GetEnumerator(filtered)\n"
			        "local enumCount = 0\n"
			        "local enumIncludesFirst = false\n"
			        "while true do\n"
			        "  local out = enum:Next()\n"
			        "  if not out then break end\n"
			        "  enumCount = enumCount + 1\n"
			        "  if out.ID == first.ID then enumIncludesFirst = true end\n"
			        "end\n"
			        "return table.concat({\n"
			        "  tostring(filteredCount > 0),\n"
			        "  tostring(itemOk),\n"
			        "  tostring(enumCount == filteredCount),\n"
			        "  tostring(enumIncludesFirst)\n"
			        "}, \":\")"));
			QVERIFY2(result.ok, qPrintable(result.error));
			if (result.value == QStringLiteral("NO_VOICE"))
				QSKIP("No Qt TextToSpeech engine available for SAPI.SpVoice creation in this environment.");
			QCOMPARE(result.value, QStringLiteral("true:true:true:true"));
		}

		void syncSpeakSurfacesOutputStreamWriteFailureWithDeterministicRuntime()
		{
			RuntimeFactoryGuard factoryGuard([]() -> std::shared_ptr<QMudTtsEngine::TtsEngine>
			                                 { return std::make_shared<FakeSynthesizeRuntime>(); });

			LuaStateGuard       state = makeLuacomState();
			QVERIFY(state.state);

			const auto result = evaluateLuaToString(
			    state.state,
			    QByteArrayLiteral(
			        "local luacom = require(\"luacom\")\n"
			        "local voice = assert(luacom.CreateObject(\"SAPI.SpVoice\"))\n"
			        "local path = \"__qmud_luacom_stream_write_failure.bin\"\n"
			        "local f = assert(io.open(path, \"wb\"))\n"
			        "f:write(\"seed\")\n"
			        "f:close()\n"
			        "local stream = assert(luacom.CreateObject(\"SAPI.SpFileStream\"))\n"
			        "stream:Open(path, 0, false)\n"
			        "voice.AudioOutputStream = stream\n"
			        "local ok, err = pcall(function()\n"
			        "  voice:Speak(\"hello\", 0)\n"
			        "end)\n"
			        "stream:Close()\n"
			        "os.remove(path)\n"
			        "local matched = type(err) == \"string\" and\n"
			        "                string.find(err, \"Unable to write synthesized audio stream\", 1, "
			        "true) ~= nil\n"
			        "return table.concat({tostring(ok), type(err), tostring(matched)}, \":\")"));
			QVERIFY2(result.ok, qPrintable(result.error));
			QCOMPARE(result.value, QStringLiteral("false:string:true"));
		}

		void asyncSpeakDoesNotSurfaceOutputStreamWriteFailure()
		{
			RuntimeFactoryGuard factoryGuard([]() -> std::shared_ptr<QMudTtsEngine::TtsEngine>
			                                 { return std::make_shared<FakeSynthesizeRuntime>(); });

			LuaStateGuard       state = makeLuacomState();
			QVERIFY(state.state);

			const auto result = evaluateLuaToString(
			    state.state,
			    QByteArrayLiteral(
			        "local luacom = require(\"luacom\")\n"
			        "local voice = assert(luacom.CreateObject(\"SAPI.SpVoice\"))\n"
			        "local flags = "
			        "luacom.GetTypeInfo(voice):GetTypeLib():ExportEnumerations().SpeechVoiceSpeakFlags\n"
			        "local path = \"__qmud_luacom_async_stream_write_failure.bin\"\n"
			        "local f = assert(io.open(path, \"wb\"))\n"
			        "f:write(\"seed\")\n"
			        "f:close()\n"
			        "local stream = assert(luacom.CreateObject(\"SAPI.SpFileStream\"))\n"
			        "stream:Open(path, 0, false)\n"
			        "voice.AudioOutputStream = stream\n"
			        "local ok, streamNumber = pcall(function()\n"
			        "  return voice:Speak(\"hello\", flags.SVSFlagsAsync)\n"
			        "end)\n"
			        "local waitResult = voice:WaitUntilDone(0)\n"
			        "stream:Close()\n"
			        "os.remove(path)\n"
			        "return table.concat({tostring(ok), type(streamNumber), tostring(waitResult)}, \":\")"));
			QVERIFY2(result.ok, qPrintable(result.error));
			QCOMPARE(result.value, QStringLiteral("true:number:true"));
		}

		void deterministicSpeakPreprocessParsesXmlAndPunctuationFlags()
		{
			const std::shared_ptr<FakeSynthesizeRuntime> runtime = std::make_shared<FakeSynthesizeRuntime>();
			runtime->hasSpeechFlag                               = true;
			runtime->waitForIdleResult                           = true;
			runtime->synthesizeResult                            = true;
			runtime->emitAudioChunk                              = true;
			RuntimeFactoryGuard factoryGuard([runtime]() -> std::shared_ptr<QMudTtsEngine::TtsEngine>
			                                 { return runtime; });

			LuaStateGuard       state = makeLuacomState();
			QVERIFY(state.state);

			const auto result = evaluateLuaToString(
			    state.state,
			    QByteArrayLiteral(
			        "local luacom = require(\"luacom\")\n"
			        "local voice = assert(luacom.CreateObject(\"SAPI.SpVoice\"))\n"
			        "local flags = "
			        "luacom.GetTypeInfo(voice):GetTypeLib():ExportEnumerations().SpeechVoiceSpeakFlags\n"
			        "local streamPath = \"__qmud_luacom_preprocess_stream.bin\"\n"
			        "local stream = assert(luacom.CreateObject(\"SAPI.SpFileStream\"))\n"
			        "stream:Open(streamPath, 3, false)\n"
			        "voice.AudioOutputStream = stream\n"
			        "local xmlOk = pcall(function()\n"
			        "  voice:Speak(\"<speak>Hello &amp; world</speak>\", flags.SVSFIsXML)\n"
			        "end)\n"
			        "local notXmlOk = pcall(function()\n"
			        "  voice:Speak(\"<speak>Raw</speak>\", flags.SVSFIsXML + flags.SVSFIsNotXML)\n"
			        "end)\n"
			        "local punctOk = pcall(function()\n"
			        "  voice:Speak(\"A,B?\", flags.SVSFNLPSpeakPunc)\n"
			        "end)\n"
			        "local xmlFile = \"__qmud_luacom_preprocess_input.xml\"\n"
			        "local f = assert(io.open(xmlFile, \"w\"))\n"
			        "f:write(\"<speak>From file &amp; text</speak>\")\n"
			        "f:close()\n"
			        "local fileXmlOk = pcall(function()\n"
			        "  voice:Speak(xmlFile, flags.SVSFIsFilename + flags.SVSFIsXML + "
			        "flags.SVSFPurgeBeforeSpeak)\n"
			        "end)\n"
			        "local missingFileOk = pcall(function()\n"
			        "  voice:Speak(\"__qmud_luacom_missing_preprocess_file.xml\", flags.SVSFIsFilename)\n"
			        "end)\n"
			        "stream:Close()\n"
			        "os.remove(streamPath)\n"
			        "os.remove(xmlFile)\n"
			        "return table.concat({\n"
			        "  tostring(xmlOk),\n"
			        "  tostring(notXmlOk),\n"
			        "  tostring(punctOk),\n"
			        "  tostring(fileXmlOk),\n"
			        "  tostring(missingFileOk)\n"
			        "}, \":\")"));
			QVERIFY2(result.ok, qPrintable(result.error));
			QCOMPARE(result.value, QStringLiteral("true:true:true:true:false"));
			QCOMPARE(runtime->synthesizedUtterances.size(), 4);
			QCOMPARE(runtime->synthesizePurgeFlags.size(), 4);
			QCOMPARE(runtime->synthesizedUtterances.at(0), QStringLiteral("Hello & world"));
			QCOMPARE(runtime->synthesizedUtterances.at(1), QStringLiteral("<speak>Raw</speak>"));
			QVERIFY(
			    runtime->synthesizedUtterances.at(2).contains(QStringLiteral("comma"), Qt::CaseInsensitive));
			QVERIFY(runtime->synthesizedUtterances.at(2).contains(QStringLiteral("question mark"),
			                                                      Qt::CaseInsensitive));
			QCOMPARE(runtime->synthesizedUtterances.at(3), QStringLiteral("From file & text"));
			QVERIFY(!runtime->synthesizePurgeFlags.at(0));
			QVERIFY(!runtime->synthesizePurgeFlags.at(1));
			QVERIFY(!runtime->synthesizePurgeFlags.at(2));
			QVERIFY(runtime->synthesizePurgeFlags.at(3));
		}

		void deterministicAudioOutputTokenFilteringCoversRequiredAndOptionalConstraints()
		{
			const std::shared_ptr<FakeSynthesizeRuntime> runtime = std::make_shared<FakeSynthesizeRuntime>();
			runtime->audioOutputIds          = {QStringLiteral("OUT_A"), QStringLiteral("OUT_B"),
			                                    QStringLiteral("OUT_C")};
			runtime->audioOutputDescriptions = {QStringLiteral("Main Speaker"), QStringLiteral("USB Headset"),
			                                    QStringLiteral("HDMI Output")};
			runtime->audioOutputSelectors    = {QByteArrayLiteral("a"), QByteArrayLiteral("b"),
			                                    QByteArrayLiteral("c")};
			runtime->audioOutputsEnumerated  = true;
			RuntimeFactoryGuard factoryGuard([runtime]() -> std::shared_ptr<QMudTtsEngine::TtsEngine>
			                                 { return runtime; });

			LuaStateGuard       state = makeLuacomState();
			QVERIFY(state.state);

			const auto result = evaluateLuaToString(
			    state.state,
			    QByteArrayLiteral(
			        "local luacom = require(\"luacom\")\n"
			        "local category = assert(luacom.CreateObject(\"SAPI.SpObjectTokenCategory\"))\n"
			        "category:SetId(\"HKEY_LOCAL_MACHINE\\\\SOFTWARE\\\\Microsoft\\\\Speech\\\\AudioOutput\","
			        " "
			        "false)\n"
			        "local all = category:EnumerateTokens(nil, nil)\n"
			        "local required = category:EnumerateTokens(\"Vendor=QMud;Id=OUT_B;broken\", nil)\n"
			        "local byDescription = category:EnumerateTokens(\"Description=USB\", nil)\n"
			        "local optionalAny = category:EnumerateTokens(nil, \"Name=Speaker;Name=HDMI\")\n"
			        "local requiredOptionalMismatch = category:EnumerateTokens(\"Vendor=QMud\", "
			        "\"Name=DoesNotExist\")\n"
			        "local first = all:Item(0)\n"
			        "local defaultId = category:GetDefaultTokenId() or \"\"\n"
			        "category:SetId(\"HKEY_LOCAL_MACHINE\\\\SOFTWARE\\\\Microsoft\\\\Speech\\\\AudioInput\", "
			        "false)\n"
			        "local unknownCount = category:EnumerateTokens(nil, nil).Count\n"
			        "return table.concat({\n"
			        "  tostring(all.Count),\n"
			        "  tostring(required.Count),\n"
			        "  tostring(byDescription.Count),\n"
			        "  tostring(optionalAny.Count),\n"
			        "  tostring(requiredOptionalMismatch.Count),\n"
			        "  tostring(unknownCount),\n"
			        "  tostring(first ~= nil and first.ID or \"\"),\n"
			        "  defaultId\n"
			        "}, \":\")"));
			QVERIFY2(result.ok, qPrintable(result.error));
			QCOMPARE(result.value, QStringLiteral("3:1:1:2:0:0:OUT_A:OUT_A"));
		}

		void fileStreamEdgeCasesCoverClosedAndErrorPaths()
		{
			LuaStateGuard state = makeLuacomState();
			QVERIFY(state.state);

			const auto result = evaluateLuaToString(
			    state.state,
			    QByteArrayLiteral("local luacom = require(\"luacom\")\n"
			                      "local stream = assert(luacom.CreateObject(\"SAPI.SpFileStream\"))\n"
			                      "local unopenedReadNil = (stream:Read() == nil)\n"
			                      "local unopenedWriteZero = (stream:Write(\"x\") == 0)\n"
			                      "local unopenedSeekFalse = (stream:Seek(0) == false)\n"
			                      "local badOpenOk = pcall(function()\n"
			                      "  stream:Open(\"__qmud_missing_dir__/stream.bin\", 0, false)\n"
			                      "end)\n"
			                      "local path = \"__qmud_luacom_stream_edges.bin\"\n"
			                      "stream.Path = path\n"
			                      "local pathEchoOk = (stream.Path == path)\n"
			                      "stream:Open(path, 3, false)\n"
			                      "local openedAfterCreate = stream.IsOpen\n"
			                      "local writtenSix = (stream:Write(\"abcdef\") == 6)\n"
			                      "local seekNegative = stream:Seek(-10)\n"
			                      "stream:Close()\n"
			                      "local openedAfterClose = stream.IsOpen\n"
			                      "stream:Open(path, 0, false)\n"
			                      "local firstTwo = stream:Read(2)\n"
			                      "local remaining = stream:Read()\n"
			                      "stream:Close()\n"
			                      "os.remove(path)\n"
			                      "return table.concat({\n"
			                      "  tostring(unopenedReadNil),\n"
			                      "  tostring(unopenedWriteZero),\n"
			                      "  tostring(unopenedSeekFalse),\n"
			                      "  tostring(badOpenOk),\n"
			                      "  tostring(pathEchoOk),\n"
			                      "  tostring(openedAfterCreate),\n"
			                      "  tostring(openedAfterClose),\n"
			                      "  tostring(writtenSix),\n"
			                      "  tostring(seekNegative),\n"
			                      "  firstTwo,\n"
			                      "  remaining\n"
			                      "}, \":\")"));
			QVERIFY2(result.ok, qPrintable(result.error));
			QCOMPARE(result.value, QStringLiteral("true:true:true:false:true:true:false:true:true:ab:cdef"));
		}

		void waitUntilDoneSurfacesFalseWhenRuntimeReportsTimeout()
		{
			const std::shared_ptr<FakeSynthesizeRuntime> runtime = std::make_shared<FakeSynthesizeRuntime>();
			runtime->waitForIdleResult                           = false;
			RuntimeFactoryGuard factoryGuard([runtime]() -> std::shared_ptr<QMudTtsEngine::TtsEngine>
			                                 { return runtime; });

			LuaStateGuard       state = makeLuacomState();
			QVERIFY(state.state);

			const auto result = evaluateLuaToString(
			    state.state,
			    QByteArrayLiteral(
			        "local luacom = require(\"luacom\")\n"
			        "local voice = assert(luacom.CreateObject(\"SAPI.SpVoice\"))\n"
			        "local ok, completed = pcall(function()\n"
			        "  return voice:WaitUntilDone(123)\n"
			        "end)\n"
			        "return table.concat({tostring(ok), type(completed), tostring(completed)}, \":\")"));
			QVERIFY2(result.ok, qPrintable(result.error));
			QCOMPARE(result.value, QStringLiteral("true:boolean:false"));
			QCOMPARE(runtime->waitTimeouts.size(), 1);
			QCOMPARE(runtime->waitTimeouts.constFirst(), 123);
		}

		void invalidUsageSurfacesExpectedLuacomErrors()
		{
			LuaStateGuard state = makeLuacomState();
			QVERIFY(state.state);

			const auto result = evaluateLuaToString(
			    state.state,
			    QByteArrayLiteral(
			        "local luacom = require(\"luacom\")\n"
			        "local voice = luacom.CreateObject(\"SAPI.SpVoice\")\n"
			        "if not voice then return \"NO_VOICE\" end\n"
			        "local badSetVoiceOk, badSetVoiceErr = pcall(function() voice:setVoice({}) end)\n"
			        "local badSetOutputOk, badSetOutputErr = pcall(function() voice:setAudioOutput({}) end)\n"
			        "local badSetStreamOk, badSetStreamErr = pcall(function() voice:setAudioOutputStream({}) "
			        "end)\n"
			        "local badPropVoiceOk, badPropVoiceErr = pcall(function() voice.Voice = {} end)\n"
			        "local badPropOutputOk, badPropOutputErr = pcall(function() voice.AudioOutput = {} end)\n"
			        "local badPropStreamOk, badPropStreamErr = pcall(function() voice.AudioOutputStream = {} "
			        "end)\n"
			        "local badVoiceAssignOk, badVoiceAssignErr = pcall(function() voice.NoSuchProperty = 1 "
			        "end)\n"
			        "local missingUiArgOk, missingUiArgErr = pcall(function() voice:IsUISupported() end)\n"
			        "local getTypeInfoBadOk, getTypeInfoBadErr = pcall(function() luacom.GetTypeInfo(123) "
			        "end)\n"
			        "local getEnumeratorBadOk, getEnumeratorBadErr = pcall(function() "
			        "luacom.GetEnumerator(123) end)\n"
			        "local token = assert(luacom.CreateObject(\"SAPI.SpObjectToken\"))\n"
			        "local badTokenAssignOk, badTokenAssignErr = pcall(function() token.NoSuchProperty = "
			        "\"x\" "
			        "end)\n"
			        "local category = assert(luacom.CreateObject(\"SAPI.SpObjectTokenCategory\"))\n"
			        "local badCategoryAssignOk, badCategoryAssignErr = pcall(function() "
			        "category.NoSuchProperty = "
			        "\"x\" end)\n"
			        "local format = assert(luacom.CreateObject(\"SAPI.SpAudioFormat\"))\n"
			        "local badFormatAssignOk, badFormatAssignErr = pcall(function() format.NoSuchProperty = "
			        "7 end)\n"
			        "local stream = assert(luacom.CreateObject(\"SAPI.SpFileStream\"))\n"
			        "local badStreamAssignOk, badStreamAssignErr = pcall(function() stream.NoSuchProperty = "
			        "\"x\" "
			        "end)\n"
			        "return table.concat({\n"
			        "  tostring(badSetVoiceOk), type(badSetVoiceErr),\n"
			        "  tostring(badSetOutputOk), type(badSetOutputErr),\n"
			        "  tostring(badSetStreamOk), type(badSetStreamErr),\n"
			        "  tostring(badPropVoiceOk), type(badPropVoiceErr),\n"
			        "  tostring(badPropOutputOk), type(badPropOutputErr),\n"
			        "  tostring(badPropStreamOk), type(badPropStreamErr),\n"
			        "  tostring(badVoiceAssignOk), type(badVoiceAssignErr),\n"
			        "  tostring(missingUiArgOk), type(missingUiArgErr),\n"
			        "  tostring(getTypeInfoBadOk), type(getTypeInfoBadErr),\n"
			        "  tostring(getEnumeratorBadOk), type(getEnumeratorBadErr),\n"
			        "  tostring(badTokenAssignOk), type(badTokenAssignErr),\n"
			        "  tostring(badCategoryAssignOk), type(badCategoryAssignErr),\n"
			        "  tostring(badFormatAssignOk), type(badFormatAssignErr),\n"
			        "  tostring(badStreamAssignOk), type(badStreamAssignErr)\n"
			        "}, \":\")"));
			QVERIFY2(result.ok, qPrintable(result.error));
			if (result.value == QStringLiteral("NO_VOICE"))
				QSKIP("No Qt TextToSpeech engine available for SAPI.SpVoice creation in this environment.");
			QCOMPARE(result.value,
			         QStringLiteral("false:string:false:string:false:string:false:string:false:string:false:"
			                        "string:false:string:false:string:false:string:false:string:false:string:"
			                        "false:string:false:string:false:string"));
		}

		void getTypeInfoAcceptsAllSupportedSapiFamilies()
		{
			LuaStateGuard state = makeLuacomState();
			QVERIFY(state.state);

			const auto result = evaluateLuaToString(
			    state.state,
			    QByteArrayLiteral(
			        "local luacom = require(\"luacom\")\n"
			        "local voice = luacom.CreateObject(\"SAPI.SpVoice\")\n"
			        "if not voice then return \"NO_VOICE\" end\n"
			        "local objects = {}\n"
			        "local function add(obj)\n"
			        "  if obj ~= nil then table.insert(objects, obj) end\n"
			        "end\n"
			        "add(voice)\n"
			        "add(voice:GetVoices())\n"
			        "add(voice:GetAudioOutputs())\n"
			        "add(voice.Status)\n"
			        "local voices = voice:GetVoices()\n"
			        "if voices.Count > 0 then add(voices:Item(0)) end\n"
			        "local outputs = voice:GetAudioOutputs()\n"
			        "if outputs.Count > 0 then add(outputs:Item(0)) end\n"
			        "local category = assert(luacom.CreateObject(\"SAPI.SpObjectTokenCategory\"))\n"
			        "category:SetId(\"HKEY_LOCAL_MACHINE\\\\SOFTWARE\\\\Microsoft\\\\Speech\\\\Voices\", "
			        "false)\n"
			        "add(category)\n"
			        "local tokens = category:EnumerateTokens(nil, nil)\n"
			        "add(tokens)\n"
			        "if tokens.Count > 0 then add(tokens:Item(0)) end\n"
			        "local lexicon = assert(luacom.CreateObject(\"SAPI.SpLexicon\"))\n"
			        "add(lexicon)\n"
			        "local stream = assert(luacom.CreateObject(\"SAPI.SpFileStream\"))\n"
			        "add(stream)\n"
			        "local format = assert(luacom.CreateObject(\"SAPI.SpAudioFormat\"))\n"
			        "add(format)\n"
			        "stream.Format = format\n"
			        "add(stream.Format)\n"
			        "local okCount = 0\n"
			        "for _, obj in ipairs(objects) do\n"
			        "  local ok = pcall(function()\n"
			        "    local ti = luacom.GetTypeInfo(obj)\n"
			        "    local lib = ti:GetTypeLib()\n"
			        "    local enums = lib:ExportEnumerations()\n"
			        "    assert(type(enums) == \"table\")\n"
			        "  end)\n"
			        "  if ok then okCount = okCount + 1 end\n"
			        "end\n"
			        "return table.concat({tostring(#objects), tostring(okCount)}, \":\")"));
			QVERIFY2(result.ok, qPrintable(result.error));
			if (result.value == QStringLiteral("NO_VOICE"))
				QSKIP("No Qt TextToSpeech engine available for SAPI.SpVoice creation in this environment.");
			const QStringList parts = result.value.split(':');
			QCOMPARE(parts.size(), 2);
			bool      okObjectCount = false;
			bool      okTypeCount   = false;
			const int objectCount   = parts.at(0).toInt(&okObjectCount);
			const int typeCount     = parts.at(1).toInt(&okTypeCount);
			QVERIFY(okObjectCount);
			QVERIFY(okTypeCount);
			QVERIFY(objectCount >= 10);
			QCOMPARE(typeCount, objectCount);
		}

		void speakFlagsAndPhoneLexiconSurface()
		{
			LuaStateGuard state = makeLuacomState();
			QVERIFY(state.state);

			const auto result = evaluateLuaToString(
			    state.state,
			    QByteArrayLiteral(
			        "local luacom = require(\"luacom\")\n"
			        "local voice = luacom.CreateObject(\"SAPI.SpVoice\")\n"
			        "if not voice then return \"NO_VOICE\" end\n"
			        "local enums = luacom.GetTypeInfo(voice):GetTypeLib():ExportEnumerations()\n"
			        "local flags = enums.SpeechVoiceSpeakFlags\n"
			        "voice.SynchronousSpeakTimeout = 0\n"
			        "local path = \"__qmud_luacom_speak_flags_test.xml\"\n"
			        "local f = assert(io.open(path, \"w\"))\n"
			        "f:write(\"<speak>hello <break/> world</speak>\")\n"
			        "f:close()\n"
			        "local fileSpeakOk = pcall(function()\n"
			        "  voice:Speak(path, flags.SVSFlagsAsync + flags.SVSFIsFilename + "
			        "flags.SVSFIsXML)\n"
			        "end)\n"
			        "os.remove(path)\n"
			        "local missingOk = pcall(function()\n"
			        "  voice:Speak(\"__qmud_luacom_missing_speak_file.xml\", flags.SVSFIsFilename)\n"
			        "end)\n"
			        "local outputs = voice:GetAudioOutputs()\n"
			        "local outputSelectionOk = true\n"
			        "if outputs.Count > 1 then\n"
			        "  local second = outputs:Item(1)\n"
			        "  outputSelectionOk = pcall(function() voice:setAudioOutput(second) end)\n"
			        "end\n"
			        "local lexicon = assert(luacom.CreateObject(\"SAPI.SpLexicon\"))\n"
			        "local addPhoneOk = pcall(function()\n"
			        "  lexicon:AddPronunciationByPhoneIds(\"qmud_phone\", 1033, 0, {1, 2, 3})\n"
			        "end)\n"
			        "local pronunciations = lexicon:GetPronunciations(\"qmud_phone\", 1033, 0)\n"
			        "local phoneStored = type(pronunciations[1]) == \"string\" and\n"
			        "                    string.find(pronunciations[1], \"1\", 1, true) ~= nil\n"
			        "local removePhoneOk = pcall(function()\n"
			        "  lexicon:RemovePronunciationByPhoneIds(\"qmud_phone\", 1033, 0, {1, 2, 3})\n"
			        "end)\n"
			        "local addSparsePhoneOk = pcall(function()\n"
			        "  lexicon:AddPronunciationByPhoneIds(\"qmud_sparse\", 1033, 0, {[1] = 7, [3] = 9})\n"
			        "end)\n"
			        "local sparse = lexicon:GetPronunciations(\"qmud_sparse\", 1033, 0)\n"
			        "local sparseStored = type(sparse[1]) == \"string\" and sparse[1] == \"7 9\"\n"
			        "local after = lexicon:GetPronunciations(\"qmud_phone\", 1033, 0)\n"
			        "local removed = (after[1] == nil)\n"
			        "return table.concat({\n"
			        "  tostring(fileSpeakOk),\n"
			        "  tostring(missingOk),\n"
			        "  tostring(outputSelectionOk),\n"
			        "  tostring(addPhoneOk),\n"
			        "  tostring(phoneStored),\n"
			        "  tostring(removePhoneOk),\n"
			        "  tostring(addSparsePhoneOk),\n"
			        "  tostring(sparseStored),\n"
			        "  tostring(removed)\n"
			        "}, \":\")"));
			QVERIFY2(result.ok, qPrintable(result.error));
			if (result.value == QStringLiteral("NO_VOICE"))
				QSKIP("No Qt TextToSpeech engine available for SAPI.SpVoice creation in this environment.");
			QCOMPARE(result.value, QStringLiteral("true:false:true:true:true:true:true:true:true"));
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_LuaLuacomShim)

#if __has_include("tst_LuaLuacomShim.moc")
#include "tst_LuaLuacomShim.moc"
#endif
